# littlefs Simulator

`littlefs_simulator` is a Windows/Linux command-line simulator that runs the
real `littlefs` sources on top of a RAM-backed block device loaded from an
image file.

It is intended for:

- creating and opening littlefs image files on a PC
- debugging filesystem behavior without target hardware
- inspecting blocks and metadata pairs
- running repeatable shell/script workflows against a flash image

## Current scope

This README describes the simulator as it exists in this repository today.
It does not describe planned features that are not yet implemented.

Implemented:

- image creation with sidecar config
- opening existing images with automatic geometry inference
- file and directory operations in an interactive shell
- script execution with `run`
- block inspection commands
- metadata pair dump and export
- recursive tree printing with optional depth limit
- RAM-backed flash erase behavior using `0xFF`

Not implemented in the current simulator:

- fault injection commands
- wear statistics
- usage statistics
- JSON output for `tree`

## Files

- `simulator/littlefs_simulator.c`
- `simulator/littlefs_simulator.exe`

## Default flash geometry

Unless overridden:

- `block_size = 131072` bytes
- `block_count = 256`
- `read_size = 256`
- `prog_size = 256`
- `cache_size = 1024`
- `lookahead_size = 32`

When opening an existing image, the simulator can automatically:

- infer `block_count` from `image_size / block_size`
- grow `lookahead_size` so it is large enough for the inferred block count

## Build

Example with GCC:

```bash
gcc -I. -I./bd -std=c99 -Wall -Wextra -pedantic \
    -o simulator/littlefs_simulator.exe \
    lfs.c lfs_util.c bd/lfs_rambd.c simulator/littlefs_simulator.c
```

## Top-level commands

### `create`

Create a blank flash image filled with `0xFF` and write a sidecar config file.

```bash
simulator/littlefs_simulator.exe create --image test.bin
simulator/littlefs_simulator.exe create --image test.bin --block-size 131072 --block-count 832
```

This creates:

- `test.bin`
- `test.bin.cfg`

### `open`

Open an image and enter the interactive shell.

```bash
simulator/littlefs_simulator.exe open test.bin
simulator/littlefs_simulator.exe open text.img --block-size 131072
```

Behavior:

- loads `*.cfg` if present
- loads the image contents into RAM before mounting
- applies command-line overrides
- auto-adjusts geometry when possible for existing images
- attempts to mount immediately
- if mount fails, keeps the device open and enters the shell unmounted
- writes the RAM-backed device back to the image on exit

Important:

- a newly created blank image is not formatted as littlefs yet
- for a fresh image, run `format` inside the shell first

### `run`

Execute a script file containing shell commands.

```bash
simulator/littlefs_simulator.exe run test.lfs --image test.bin
simulator/littlefs_simulator.exe run test.lfs --image test.bin --stop-on-error
```

## Shell commands

### Filesystem operations

- `ls [path]`
- `cd <path>`
- `pwd`
- `cat <file>`
- `hexdump <file>`
- `create <file>`
- `write <file> <data>`
- `mkdir <dir>`
- `rm <path> [--recursive]`
- `cp <src> <dst>`
- `rename <src> <dst>`
- `stat <path>`
- `issue1177 [attempts]`
- `format`
- `mount`
- `umount`
- `help`
- `quit`

### Diagnostics

- `tree [path] [--depth N]`
- `inspect blocks`
- `inspect block <N>`
- `meta-dump <path> [--block-only] [--parsed-only] [--export [file.txt]]`

### Issue reproduction

`issue1177 [attempts]` runs a destructive reproduction pass for the
multiple-write-handle stale-file scenario discussed around issue `#1177`.
It reformats the currently opened image, then tries several parameter sets
that:

- keep one write handle open after `sync`
- truncate the same file through a second write handle
- force another file to reuse released space
- write again through the stale first handle

If the second file's contents change unexpectedly, the command reports that
the issue was reproduced.

## Examples

### First-time workflow

```text
open test.bin
format
mkdir /config
write /config/device.txt ABC123
tree /
```

### Tree view

```text
tree /
tree / --depth 1
tree /config --depth 0
```

### Block inspection

```text
inspect blocks
inspect block 0
inspect block 0xe1
```

`inspect blocks` prints a simple block usage map:

- `#` means used
- `.` means free

### Metadata dump

```text
meta-dump /config
meta-dump /config --block-only
meta-dump /config --parsed-only
meta-dump /config --export
meta-dump /config --export config_dump.txt
meta-dump /config/device.txt --export file_meta.txt
```

Current `meta-dump` behavior:

- identifies the metadata pair for a directory or file path
- prints active and mirror block information
- prints each tag's raw bytes followed immediately by a parsed summary
- trims large erased tails instead of dumping the entire block
- for fully erased blocks, prints only two `0xFF` lines plus an omission note

### Export behavior

If `--export` is used:

- an export file is written on the host filesystem
- relative export paths are resolved under the simulator executable directory
- if no file name is provided, a default name such as `meta_dump__config.txt`
  is generated

## Geometry validation

When opening an existing image, the simulator validates:

- the file exists
- image size is divisible by `block_size`
- final `block_count` matches the image size
- `lookahead_size * 8 >= block_count`

Example of automatic adjustment:

```text
Auto-adjusted block_count: 256 -> 832 based on image size 109051904
Auto-adjusted lookahead_size: 32 -> 104 for block_count 832
```

## Notes

- `meta-dump` is intended as a debugging aid, not a complete offline littlefs
  forensic parser
- the simulator uses a file-backed block device, so changes are persisted to the
  image file immediately
- erase operations write `0xFF` to match flash-style erased state
