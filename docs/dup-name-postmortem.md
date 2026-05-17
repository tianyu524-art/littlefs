# Duplicate-Name Post-Mortem: Two `PWR.IDX` After Compact

This document explains how a littlefs directory can end up holding **two entries with the same name** after a successful compact while every commit's CRC still validates. It is based on the metadata sample below, captured from a real device:

```
reg id=6 size=7 name PWR.IDX
ctzsutruc id=6 size=8 head 0x148,size 988
reg id=5  name PWR.IDX.tmp
inlinestruct id=5 size=0
reg id=4 size=7 name PWR.IDX
reg id=1 size=12 name 0F0C4149.PWR
ctzsutruc id=1 size=8 head 0x3a, size 30664
reg id=2 size=12 name 0F0C416F.PWR
ctzsutruc id=2 size=8 head 0x159,size 14624
reg id=3 size=12 name 0F0C5E42.PWR
ctzsutruc id=3 size=8 head 0xda, size 14344
reg id=0 size=12 name 0F0C3FCC.PWR
ctzsutruc id=4 size=8 head 0x78, size 988
ctzsutruc id=0 size=8 head 0x9c, size 4804
softtail pair= 0xb0,0xb1
crc
```

The two `PWR.IDX` entries are `id=4` (head `0x78`) and `id=6` (head `0x148`). `id=5 PWR.IDX.tmp` is an empty inline stub.

## 1. Why CRC alone cannot catch this

`lfs_dir_commitcrc` (see [lfs.c:1523][commitcrc]) writes a CRC tag at the **end of each commit** and then resets the rolling CRC for the next commit:

```c
// build crc tag
tag = LFS_MKTAG(LFS_TYPE_CRC + reset, 0x3ff, noff - off);
footer[0] = lfs_tobe32(tag ^ commit->ptag);
commit->crc = lfs_crc(commit->crc, &footer[0], sizeof(footer[0]));
footer[1] = lfs_tole32(commit->crc);
err = lfs_bd_prog(lfs, ..., commit->block, commit->off, &footer, sizeof(footer));
...
commit->crc = 0xffffffff; // reset crc for next "commit"
```

A metadata block is therefore a chain of independent commits:

```
[revcount][tags1][CRC1][tags2][CRC2] ... [tagsN][CRCN][0xff...erased]
```

`lfs_dir_fetch` replays them in order and stops at the first broken CRC. Each commit is **individually atomic**, but the cross-commit semantic outcome is the responsibility of the API layer — CRC has nothing to say about it.

## 2. The three guards inside lfs.c that normally prevent duplicate names

```
┌──────────────────────────────────────────────────────────────┐
│ Guard 1: lfs_dir_find on every create / rename                │
│   lfs.c:1268 → lfs_dir_get(LFS_TYPE_NAME, ...)                │
│   - Refuses to create a duplicate via the standard API.       │
└──────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌──────────────────────────────────────────────────────────────┐
│ Guard 2: single-commit rename                                 │
│   lfs.c:3906 (lfs_rawrename, same-pair path)                  │
│   LFS_MKATTRS({DELETE prev}, {CREATE newid}, {NAME},          │
│               {FROM_MOVE}, {DELETE newoldid})                 │
│   - The delete-old + create-new pair lives inside one commit. │
│   - CRC makes the whole thing atomic — there is no power-loss │
│     window in which only "create new" landed.                 │
└──────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌──────────────────────────────────────────────────────────────┐
│ Guard 3: mlist ID fix-up after every commit                   │
│   lfs.c:2160-2195 (lfs_dir_relocatingcommit fixmlist block)   │
│   - Walks lfs->mlist and applies splice attrs in attr order:  │
│       DELETE id == handle.id  → handle.m.pair = {NULL,NULL}   │
│       DELETE id <  handle.id  → handle.id -= 1                │
│       CREATE id <= handle.id  → handle.id += 1                │
│   - Stale handles are *neutered*, not invalidated silently.   │
│   - lfs_file_rawsync (lfs.c:3237) refuses to commit when      │
│       lfs_pair_isnull(file->m.pair) is true.                  │
└──────────────────────────────────────────────────────────────┘
```

Any normal sequence on the public API (`lfs_file_open`, `lfs_remove`, `lfs_rename`, etc.) goes through all three guards, so duplicate names cannot persist.

## 3. The compact path preserves whatever was alive

`lfs_dir_compact` calls `lfs_dir_traverse` with `tmask = LFS_MKTAG(0x400, 0x3ff, 0)` and `ttag = LFS_MKTAG(LFS_TYPE_NAME, 0, 0)` (see [lfs.c:1825][compact-traverse]). The traversal:

- visits every tag in the source block in physical order
- for each tag, scans forward for a newer tag with the same `(type, id)` and skips the older one
- emits the surviving tag at its **first-occurrence offset**

Crucially, the dedup key is `(type, id)`, **not the name string**. So if two NAME tags exist for two *different* ids that happen to share a name, **both survive compact**. This is exactly what we see in the sample dump: `id=4 PWR.IDX` and `id=6 PWR.IDX` are both retained because no `DELETE id=4` ever made it into the block.

It also explains the unusual ordering: `id=0` and `id=4` have their NAME tags near the top of the block but their CTZSTRUCT tags near the bottom. Their NAMEs were written once at creation; their CTZs were rewritten by later file updates, and only the *latest* CTZ survives — at its later physical position.

## 4. How the bad state actually arises

Given the three guards, the bug is not in the API path. The remaining vectors, ranked by likelihood:

### (A) Application-level use of lfs's internal (non-public) interfaces

Upper-layer code that calls `lfs_dir_commit`, `lfs_dir_traverse`, or assembles `LFS_MKATTRS` directly bypasses Guards 1 and 3 entirely. The CRC will still match because the commit itself is well-formed.

This is the most likely explanation here because:

- the user's directory is already **split** (`softtail pair=0xb0,0xb1`), which the repo's own `chaosstress` / `lsrepair2` test infrastructure exists specifically to debug;
- the surrounding code shells (`simulator/littlefs_simulator.c`'s `inject_*` family) demonstrate stale-handle write patterns that the authors knew were dangerous;
- 2 minutes of `chaosstressdeep 500` with random byte corruption produced 2 422 `soft candidate` warnings but **zero** real `idx_count > 1` hits — the public API really does hold the line.

### (B) An older lfs that was patched later

`LFS_VERSION` in the source is `0x00020005`. Several mlist/split corner cases were fixed in early `v2.x`. If the image was written by an older binary and is now being inspected with newer code, the historical bug is frozen into the bytes.

### (C) Power loss inside cross-directory rename

`lfs_rawrename`'s non-`samepair` path issues **two commits** — one in the target dir, one in the source dir — using `gstate.move` as the bridge ([lfs.c:3906][rawrename-1] and [lfs.c:3924][rawrename-2]):

```c
// commit 1: target dir
err = lfs_dir_commit(lfs, &newcwd, LFS_MKATTRS({DELETE prev?}, {CREATE newid},
                     {NAME newid newpath}, {FROM_MOVE newid from oldid}, ...));

// commit 2: source dir
if (!samepair && lfs_gstate_hasmove(&lfs->gstate)) {
    lfs_fs_prepmove(lfs, 0x3ff, NULL);
    err = lfs_dir_commit(lfs, &oldcwd, LFS_MKATTRS({DELETE lfs_tag_id(oldtag)}));
}
```

A power loss between the two commits leaves the source entry undeleted, but `gstate.move` (XORed into every commit's footer) tells the next mount to finish the cleanup via `lfs_fs_forceconsistency`. If — and only if — `gstate` itself was lost or corrupted, the source entry survives. This *can* yield duplicate names, but only across two different parent directories. The user's two `PWR.IDX` share a parent, so this vector does not fit cleanly.

### (D) Hardware-level corruption with a coincidental CRC match

A random byte flip has roughly a 2⁻³² probability of producing a valid CRC. Aging NOR flash near end of life can flip bits; if exactly the bits forming a `DELETE id=4` tag flipped to look like padding *and* the CRC happened to still match, you would see this state. Possible, but vanishingly unlikely.

## 5. Reproduction attempts in this repo

| Method | Iterations | Result |
|--------|-----------:|--------|
| `dual-handle` (one shot of `inject_dual_idx_handle_crossclose`) | 1 | clean — mlist neutered the stale handle |
| `stale-rename` (one shot of `inject_idx_rename_recreate_handle`) | 1 | clean |
| Mixed `dual-handle` + `stale-rename` | 6 | clean |
| `chaosstress 3` | 3 | `soft candidate ... unsorted=yes`, no real hit |
| `chaosstress 200` (≈ 32 k log lines) | 200 | 4 611 soft candidates, **0** real hits |
| `chaosstressdeep 500` (120 s timeout) | partial | 2 422 soft candidates, **0** real hits |
| Pure shell `create → write → rename` (no held handle) | 1 | clean — `lfs_rename` is single-commit atomic |

The `idxstress_checkpoint` helper at [littlefs_simulator.c:1952][checkpoint] is *specifically* watching for `idx_count > 1` and dumps the metadata to `chaosstress_hit_meta.txt` the moment it sees it. Two long runs produced no such file.

This is itself the punchline: **the on-disk shape from the user is not reachable from clean public-API use of current lfs**. The application above lfs has to be doing something that lfs does not see — direct metadata writes, or use of internal helpers, or an older binary.

## 6. Detection and repair

### Detection from inside the simulator
- `lschk [path]` — single-directory duplicate scan based on `collect_lschk_entries` (sorted entry walk with pairwise name compare; see [littlefs_simulator.c:2215][lschk-dup]).
- `lschk-on-mount on` — opt-in recursive scan that runs immediately after `mount`. Added in this commit; flags any `(parent, name)` pair appearing twice or more.

### Repair inside the simulator
- `lsrepair` — heuristic: keep the first-found "real" entry for each name, mark the others as ghosts and delete them.
- `lsrepair2` — same idea, but uses the live-file lookup (`lfs_file_open` / `lfs_dir_open`) to pick the winner, so it prefers the entry that the public API actually resolves.

### Recommendations for the application layer
1. **Never call `lfs_dir_commit` / `LFS_MKATTRS` from outside lfs.** Use only `lfs_file_*`, `lfs_remove`, `lfs_rename`, `lfs_mkdir`.
2. **Close every handle to a path before `lfs_remove` or `lfs_rename` it.** The mlist fix-up is only ever invoked from a `lfs_dir_commit` triggered by a public call; if the application opens, removes, then keeps writing through the old `lfs_file_t`, that handle is dead by mlist's bookkeeping but the application does not know that until `sync` returns 0 with no commit.
3. **Make atomic updates a single `lfs_rename`.** The pattern is `write to .tmp → close → lfs_rename(.tmp, real)`. The `lfs_remove(real)` step in `lfs_remove → rename` style is two commits — avoid it.
4. **On mount, run a duplicate-name scan and call `lsrepair2` if anything is found.** Until guard 1 can be enforced retroactively, scrubbing inconsistent state at boot is the safest backstop.
5. **Upgrade lfs.** If the binary that wrote the image is older than `LFS_VERSION = 0x00020006`, several rename/split edge cases were tightened in the meantime.

[commitcrc]: ../lfs.c
[compact-traverse]: ../lfs.c
[rawrename-1]: ../lfs.c
[rawrename-2]: ../lfs.c
[checkpoint]: ../simulator/littlefs_simulator.c
[lschk-dup]: ../simulator/littlefs_simulator.c
