# Rename Opt Optimization Notes

## Goal

Keep the original `lfs_rename()` flow unchanged and add an optimized rename
interface for performance-sensitive paths.

The new public API is:

```c
int lfs_rename_opt(lfs_t *lfs, const char *oldpath, const char *newpath);
```

## Files Changed

- `lfs.h`
- `lfs.c`

## Design

The original `lfs_rename()` path remains untouched. The optimization is
implemented as a copied path:

- `lfs_rawrename_opt()`
- `lfs_rename_opt()`

### Main optimizations

1. Avoid the extra public `lfs_getattr(oldpath, LFS_ATTR_MTIME, ...)`
   lookup before rename.
   Instead, reuse the already found `oldcwd + oldtag` and read `mtime`
   directly by id.

2. Merge the `ctime` and `mtime` updates into the main rename commit.
   The original timestamp-enabled rename path performs two extra
   `lfs_setattr()` calls after rename. The optimized path writes both
   userattrs in the main rename commit.

3. Add a fast path for same-parent, non-split, single-live-entry rename.
   For this narrow but common case, the optimized path computes the new
   insertion id directly by comparing names and skips the second
   `lfs_dir_find(newpath, &newid)` traversal.

## Why this helps

With timestamp support enabled, the original rename path effectively expands
into:

- 1 extra `getattr` lookup for old `mtime`
- 2 `dir_find` traversals for `oldpath` and `newpath`
- 1 main rename commit
- 2 extra `setattr` calls after rename
  - each `setattr` performs its own `dir_find`
  - each `setattr` performs its own metadata commit

So one rename can become roughly:

- 5 directory lookups
- 3 metadata commits

The optimized path reduces this to:

- 2 directory lookups in the general case
- 1 by-id `mtime` read from the already located directory entry
- 1 main rename commit that also carries `ctime/mtime`

In the single-live-entry same-parent case it further reduces to:

- 1 directory lookup for `oldpath`
- 0 full `newpath` lookup
- 1 main rename commit

## Benchmark Setup

Benchmark tool:

- `simulator/_tmp_rename_bench.c`

Geometry used by the benchmark:

- `block_size = 8KB`
- `block_count = 256`
- `read_size = 16`
- `prog_size = 16`
- `cache_size = 256`
- `lookahead_size = 32`
- `block_cycles = 500`

Rounds:

- 120 rounds per scenario

## Benchmark Results

Source report:

- `simulator/_tmp_rename_bench_opt_report.txt`

### Raw `lfs_rename`

- Scenario 1, empty dir, `A -> B`
  - avg `27.414 us`
- Scenario 2, 200 files in dir, `AA -> BB`
  - avg `1537.388 us`
- Scenario 3, delete all files then `AAA -> BBB`
  - avg `2000.628 us`

### Optimized `lfs_rename_opt`

- Scenario 1, empty dir, `A -> B`
  - avg `9.919 us`
- Scenario 2, 200 files in dir, `AA -> BB`
  - avg `1434.843 us`
- Scenario 3, delete all files then `AAA -> BBB`
  - avg `1728.458 us`

## Improvement Summary

- Scenario 1
  - improvement about `63.8%`
- Scenario 2
  - improvement about `6.7%`
- Scenario 3
  - improvement about `13.6%`

## Notes

- The optimization does not change the original rename semantics.
- The optimization does not solve the deeper cost of hot directories with a
  long metadata history. That is why scenario 3 is still much slower than
  scenario 1 even after optimization.
- A trial version that attempted to compact/repack the hot-but-empty
  directory before rename was measured and rejected because it made single
  rename latency worse.

