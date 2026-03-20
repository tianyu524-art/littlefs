#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include "bd/lfs_filebd.h"
#include "lfs.h"

#define AT_MAX_FILES 4096
#define AT_PATH_MAX 1024
#define AT_LINE_MAX 256

#define AT_DEFAULT_BLOCK_SIZE  (256u * 1024u)
#define AT_DEFAULT_READ_SIZE   512u
#define AT_DEFAULT_PROG_SIZE   512u
#define AT_DEFAULT_CACHE_SIZE  512u
#define AT_DEFAULT_LOOKAHEAD   4096u
#define AT_DEFAULT_CYCLES      (-1)

#define AT_SOURCE_IMAGE "C:\\Users\\HW\\.codex\\worktrees\\e439\\littlefs_v0\\simulator\\test001.img"
#define AT_WORK_IMAGE   "C:\\Users\\HW\\.codex\\worktrees\\e439\\littlefs_v0\\autotest\\test001_work.img"
#define AT_LOG_PATH     "C:\\Users\\HW\\.codex\\worktrees\\e439\\littlefs_v0\\autotest\\autotest.txt"
#define AT_TARGET_DIR   "/lfs0/LOG/PWR"

typedef enum at_entry_status {
    AT_STATUS_REAL = 0,
    AT_STATUS_GHOST,
    AT_STATUS_DAMAGED,
} at_entry_status_t;

typedef struct at_dir_entry {
    char name[LFS_NAME_MAX + 1];
    uint8_t type;
    lfs_size_t size;
    lfs_block_t pair[2];
    uint16_t id;
    bool duplicate;
    at_entry_status_t status;
} at_dir_entry_t;

typedef struct at_pwr_file {
    char name[16];
    uint32_t number;
    lfs_size_t size;
} at_pwr_file_t;

typedef struct at_context {
    FILE *log;
    const char *source_image;
    const char *work_image;
    lfs_t lfs;
    lfs_filebd_t bd;
    struct lfs_config cfg;
    struct lfs_filebd_config bd_cfg;
    uint8_t *read_buffer;
    uint8_t *prog_buffer;
    uint8_t *lookahead_buffer;
    lfs_size_t block_count;
} at_context_t;

static void at_log(at_context_t *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(ctx->log, fmt, ap);
    fputc('\n', ctx->log);
    fflush(ctx->log);
    va_end(ap);
}

static bool at_pair_equals(const lfs_block_t a[2], const lfs_block_t b[2]) {
    return a[0] == b[0] && a[1] == b[1];
}

static const char *at_status_name(at_entry_status_t status) {
    switch (status) {
    case AT_STATUS_REAL:
        return "realfile";
    case AT_STATUS_GHOST:
        return "ghostfile";
    case AT_STATUS_DAMAGED:
        return "damagedfile";
    default:
        return "unknown";
    }
}

static int at_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        return -errno;
    }

    FILE *out = fopen(dst, "wb");
    if (!out) {
        int err = -errno;
        fclose(in);
        return err;
    }

    uint8_t buffer[64 * 1024];
    while (true) {
        size_t n = fread(buffer, 1, sizeof(buffer), in);
        if (n > 0 && fwrite(buffer, 1, n, out) != n) {
            int err = -errno;
            fclose(in);
            fclose(out);
            return err;
        }
        if (n < sizeof(buffer)) {
            if (ferror(in)) {
                int err = -errno;
                fclose(in);
                fclose(out);
                return err;
            }
            break;
        }
    }

    fclose(in);
    fclose(out);
    return 0;
}

static int at_file_size(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -errno;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        int err = -errno;
        fclose(f);
        return err;
    }
    long pos = ftell(f);
    if (pos < 0) {
        int err = -errno;
        fclose(f);
        return err;
    }
    fclose(f);
    *size = (uint64_t)pos;
    return 0;
}

static int at_mount(at_context_t *ctx) {
    uint64_t image_size = 0;
    int err = at_file_size(ctx->work_image, &image_size);
    if (err) {
        return err;
    }

    if (image_size % AT_DEFAULT_BLOCK_SIZE != 0) {
        return LFS_ERR_INVAL;
    }

    memset(&ctx->cfg, 0, sizeof(ctx->cfg));
    memset(&ctx->bd_cfg, 0, sizeof(ctx->bd_cfg));
    memset(&ctx->bd, 0, sizeof(ctx->bd));
    memset(&ctx->lfs, 0, sizeof(ctx->lfs));

    ctx->block_count = (lfs_size_t)(image_size / AT_DEFAULT_BLOCK_SIZE);
    ctx->read_buffer = malloc(AT_DEFAULT_CACHE_SIZE);
    ctx->prog_buffer = malloc(AT_DEFAULT_CACHE_SIZE);
    ctx->lookahead_buffer = malloc(AT_DEFAULT_LOOKAHEAD);
    if (!ctx->read_buffer || !ctx->prog_buffer || !ctx->lookahead_buffer) {
        return LFS_ERR_NOMEM;
    }

    ctx->bd_cfg.read_size = AT_DEFAULT_READ_SIZE;
    ctx->bd_cfg.prog_size = AT_DEFAULT_PROG_SIZE;
    ctx->bd_cfg.erase_size = AT_DEFAULT_BLOCK_SIZE;
    ctx->bd_cfg.erase_count = ctx->block_count;

    ctx->cfg.context = &ctx->bd;
    ctx->cfg.read = lfs_filebd_read;
    ctx->cfg.prog = lfs_filebd_prog;
    ctx->cfg.erase = lfs_filebd_erase;
    ctx->cfg.sync = lfs_filebd_sync;
    ctx->cfg.read_size = AT_DEFAULT_READ_SIZE;
    ctx->cfg.prog_size = AT_DEFAULT_PROG_SIZE;
    ctx->cfg.block_size = AT_DEFAULT_BLOCK_SIZE;
    ctx->cfg.block_count = ctx->block_count;
    ctx->cfg.block_cycles = AT_DEFAULT_CYCLES;
    ctx->cfg.cache_size = AT_DEFAULT_CACHE_SIZE;
    ctx->cfg.lookahead_size = AT_DEFAULT_LOOKAHEAD;
    ctx->cfg.read_buffer = ctx->read_buffer;
    ctx->cfg.prog_buffer = ctx->prog_buffer;
    ctx->cfg.lookahead_buffer = ctx->lookahead_buffer;
    ctx->cfg.name_max = LFS_NAME_MAX;
    ctx->cfg.file_max = LFS_FILE_MAX;
    ctx->cfg.attr_max = LFS_ATTR_MAX;
    ctx->cfg.metadata_max = AT_DEFAULT_BLOCK_SIZE;

    err = lfs_filebd_create(&ctx->cfg, ctx->work_image, &ctx->bd_cfg);
    if (err) {
        return err;
    }

    err = lfs_mount(&ctx->lfs, &ctx->cfg);
    if (err) {
        lfs_filebd_destroy(&ctx->cfg);
        return err;
    }

    return 0;
}

static void at_unmount(at_context_t *ctx) {
    lfs_unmount(&ctx->lfs);
    lfs_filebd_destroy(&ctx->cfg);
    free(ctx->read_buffer);
    free(ctx->prog_buffer);
    free(ctx->lookahead_buffer);
    ctx->read_buffer = NULL;
    ctx->prog_buffer = NULL;
    ctx->lookahead_buffer = NULL;
}

static bool at_is_numbered_pwr(const char *name, uint32_t *number_out) {
    if (strlen(name) != 12 || strcmp(name + 8, ".PWR") != 0) {
        return false;
    }

    uint32_t value = 0;
    for (int i = 0; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        value = value * 10u + (uint32_t)(name[i] - '0');
    }

    *number_out = value;
    return true;
}

static int at_collect_pwr_files(at_context_t *ctx, at_pwr_file_t *files, size_t *count_out) {
    lfs_dir_t dir;
    int err = lfs_dir_open(&ctx->lfs, &dir, AT_TARGET_DIR);
    if (err) {
        return err;
    }

    size_t count = 0;
    struct lfs_info info;
    while ((err = lfs_dir_read(&ctx->lfs, &dir, &info)) > 0) {
        uint32_t number = 0;
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }
        if (info.type != LFS_TYPE_REG) {
            continue;
        }
        if (!at_is_numbered_pwr(info.name, &number)) {
            continue;
        }
        if (count >= AT_MAX_FILES) {
            lfs_dir_close(&ctx->lfs, &dir);
            return LFS_ERR_NOMEM;
        }
        memset(&files[count], 0, sizeof(files[count]));
        strncpy(files[count].name, info.name, sizeof(files[count].name) - 1);
        files[count].number = number;
        files[count].size = info.size;
        count++;
    }

    lfs_dir_close(&ctx->lfs, &dir);
    if (err < 0) {
        return err;
    }

    *count_out = count;
    return 0;
}

static int at_write_file(at_context_t *ctx, const char *path, const void *data, size_t size) {
    lfs_file_t file;
    int err = lfs_file_open(&ctx->lfs, &file, path,
            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err) {
        return err;
    }

    const uint8_t *cursor = (const uint8_t *)data;
    size_t remaining = size;
    while (remaining > 0) {
        size_t chunk = remaining > 4096 ? 4096 : remaining;
        lfs_ssize_t res = lfs_file_write(&ctx->lfs, &file, cursor, chunk);
        if (res < 0) {
            lfs_file_close(&ctx->lfs, &file);
            return (int)res;
        }
        cursor += (size_t)res;
        remaining -= (size_t)res;
    }

    err = lfs_file_close(&ctx->lfs, &file);
    return err;
}

static int at_remove_file(at_context_t *ctx, const char *path) {
    return lfs_remove(&ctx->lfs, path);
}

static int at_ensure_idx_exists(at_context_t *ctx) {
    struct lfs_info info;
    int err = lfs_stat(&ctx->lfs, AT_TARGET_DIR "/PWR.IDX", &info);
    if (err == 0) {
        return 0;
    }
    if (err != LFS_ERR_NOENT) {
        return err;
    }
    return at_write_file(ctx, AT_TARGET_DIR "/PWR.IDX", "", 0);
}

static lfs_size_t at_random_length(void) {
    static const lfs_size_t lengths[] = {12244u, 28204u, 30604u};
    return lengths[rand() % 3];
}

static int at_create_random_pwr(at_context_t *ctx, char *name_out, lfs_size_t *size_out) {
    at_pwr_file_t files[AT_MAX_FILES];
    size_t count = 0;
    int err = at_collect_pwr_files(ctx, files, &count);
    if (err) {
        return err;
    }

    uint32_t max_number = 0;
    bool have_any = false;
    for (size_t i = 0; i < count; i++) {
        if (!have_any || files[i].number > max_number) {
            max_number = files[i].number;
            have_any = true;
        }
    }

    uint32_t next_number = have_any ? (max_number + 1u) : 0u;
    snprintf(name_out, 16, "%08"PRIu32".PWR", next_number);

    lfs_size_t len = at_random_length();
    uint8_t *buffer = malloc(len);
    if (!buffer) {
        return LFS_ERR_NOMEM;
    }

    for (lfs_size_t i = 0; i < len; i++) {
        buffer[i] = (uint8_t)(rand() & 0xff);
    }

    char path[AT_PATH_MAX];
    snprintf(path, sizeof(path), AT_TARGET_DIR "/%s", name_out);
    err = at_write_file(ctx, path, buffer, len);
    free(buffer);
    if (err) {
        return err;
    }

    *size_out = len;
    return 0;
}

static int at_write_index_listing(
        at_context_t *ctx, const char *path, const at_pwr_file_t *files, size_t count) {
    size_t cap = count * 64 + 1;
    char *buffer = malloc(cap);
    if (!buffer) {
        return LFS_ERR_NOMEM;
    }

    size_t off = 0;
    buffer[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        int n = snprintf(buffer + off, cap - off, "%s %"PRIu32"\n",
                files[i].name, (uint32_t)files[i].size);
        if (n < 0 || (size_t)n >= cap - off) {
            free(buffer);
            return LFS_ERR_NOMEM;
        }
        off += (size_t)n;
    }

    int err = at_write_file(ctx, path, buffer, off);
    free(buffer);
    return err;
}

static int at_collect_dir_entries(
        at_context_t *ctx, at_dir_entry_t *entries, size_t *count_out) {
    lfs_dir_t dir;
    int err = lfs_dir_open(&ctx->lfs, &dir, AT_TARGET_DIR);
    if (err) {
        return err;
    }

    size_t count = 0;
    struct lfs_info info;
    while ((err = lfs_dir_read(&ctx->lfs, &dir, &info)) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }
        if (count >= AT_MAX_FILES) {
            lfs_dir_close(&ctx->lfs, &dir);
            return LFS_ERR_NOMEM;
        }
        memset(&entries[count], 0, sizeof(entries[count]));
        strncpy(entries[count].name, info.name, sizeof(entries[count].name) - 1);
        entries[count].type = info.type;
        entries[count].size = info.size;
        entries[count].pair[0] = dir.m.pair[0];
        entries[count].pair[1] = dir.m.pair[1];
        entries[count].id = (dir.id > 0) ? (uint16_t)(dir.id - 1) : 0;
        entries[count].status = AT_STATUS_REAL;
        count++;
    }

    lfs_dir_close(&ctx->lfs, &dir);
    if (err < 0) {
        return err;
    }

    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (strcmp(entries[i].name, entries[j].name) == 0) {
                entries[i].duplicate = true;
                entries[j].duplicate = true;
            }
        }
    }

    bool processed[AT_MAX_FILES] = {0};
    for (size_t i = 0; i < count; i++) {
        if (processed[i]) {
            continue;
        }

        char fullpath[AT_PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), AT_TARGET_DIR "/%s", entries[i].name);

        bool winner_found = false;
        bool winner_is_file = false;
        lfs_block_t winner_pair[2] = {0, 0};
        uint16_t winner_id = 0;
        uint8_t winner_type = 0;
        int winner_index = -1;

        lfs_file_t file;
        int open_err = lfs_file_open(&ctx->lfs, &file, fullpath, LFS_O_RDONLY);
        if (open_err == 0) {
            winner_found = true;
            winner_is_file = true;
            winner_pair[0] = file.m.pair[0];
            winner_pair[1] = file.m.pair[1];
            winner_id = file.id;
            winner_type = file.type;
            lfs_file_close(&ctx->lfs, &file);
        } else {
            lfs_dir_t subdir;
            open_err = lfs_dir_open(&ctx->lfs, &subdir, fullpath);
            if (open_err == 0) {
                winner_found = true;
                winner_type = LFS_TYPE_DIR;
                lfs_dir_close(&ctx->lfs, &subdir);
            }
        }

        if (winner_found && !winner_is_file && entries[i].duplicate) {
            for (size_t j = i; j < count; j++) {
                if (strcmp(entries[i].name, entries[j].name) == 0 &&
                        entries[j].type == winner_type) {
                    winner_index = (int)j;
                    break;
                }
            }
        }

        for (size_t j = i; j < count; j++) {
            if (strcmp(entries[i].name, entries[j].name) != 0) {
                continue;
            }

            processed[j] = true;
            if (!winner_found) {
                entries[j].status = AT_STATUS_DAMAGED;
                continue;
            }

            if (!entries[j].duplicate) {
                entries[j].status = AT_STATUS_REAL;
                continue;
            }

            if (winner_is_file) {
                if (entries[j].type == winner_type &&
                        entries[j].id == winner_id &&
                        at_pair_equals(entries[j].pair, winner_pair)) {
                    entries[j].status = AT_STATUS_REAL;
                } else if (entries[j].type == winner_type) {
                    entries[j].status = AT_STATUS_GHOST;
                } else {
                    entries[j].status = AT_STATUS_DAMAGED;
                }
            } else {
                if ((int)j == winner_index) {
                    entries[j].status = AT_STATUS_REAL;
                } else if (entries[j].type == winner_type) {
                    entries[j].status = AT_STATUS_GHOST;
                } else {
                    entries[j].status = AT_STATUS_DAMAGED;
                }
            }
        }
    }

    *count_out = count;
    return 0;
}

static int at_check_all_real(at_context_t *ctx, bool *all_real) {
    at_dir_entry_t entries[AT_MAX_FILES];
    size_t count = 0;
    int err = at_collect_dir_entries(ctx, entries, &count);
    if (err) {
        return err;
    }

    *all_real = true;
    at_log(ctx, "lschk %s", AT_TARGET_DIR);
    for (size_t i = 0; i < count; i++) {
        at_log(ctx, "  %s [%s] id=%u pair={0x%"PRIx32",0x%"PRIx32"} size=%"PRIu32,
                entries[i].name,
                at_status_name(entries[i].status),
                entries[i].id,
                entries[i].pair[0],
                entries[i].pair[1],
                (uint32_t)entries[i].size);
        if (entries[i].status != AT_STATUS_REAL) {
            *all_real = false;
        }
    }

    return 0;
}

static int at_iteration(at_context_t *ctx, int iteration) {
    at_log(ctx, "iteration %d begin", iteration);

    int err = at_ensure_idx_exists(ctx);
    if (err) {
        at_log(ctx, "  ensure PWR.IDX failed: %d", err);
        return err;
    }

    at_pwr_file_t latest;
    memset(&latest, 0, sizeof(latest));
    err = at_create_random_pwr(ctx, latest.name, &latest.size);
    if (err) {
        at_log(ctx, "  create numbered file failed: %d", err);
        return err;
    }
    at_log(ctx, "  created %s size=%"PRIu32, latest.name, (uint32_t)latest.size);

    err = at_write_index_listing(ctx, AT_TARGET_DIR "/PWR.IDX", &latest, 1);
    if (err) {
        at_log(ctx, "  write PWR.IDX failed: %d", err);
        return err;
    }
    at_log(ctx, "  updated PWR.IDX with latest entry");

    at_pwr_file_t files[AT_MAX_FILES];
    size_t count = 0;
    err = at_collect_pwr_files(ctx, files, &count);
    if (err) {
        at_log(ctx, "  collect numbered files failed: %d", err);
        return err;
    }

    if (count == 0) {
        at_log(ctx, "  no numbered PWR files found after update");
        return LFS_ERR_NOENT;
    }

    if (count > 5) {
        size_t min_index = 0;
        for (size_t i = 1; i < count; i++) {
            if (files[i].number < files[min_index].number) {
                min_index = i;
            }
        }

        char remove_path[AT_PATH_MAX];
        snprintf(remove_path, sizeof(remove_path), AT_TARGET_DIR "/%s", files[min_index].name);
        err = at_remove_file(ctx, remove_path);
        if (err) {
            at_log(ctx, "  remove %s failed: %d", files[min_index].name, err);
            return err;
        }
        at_log(ctx, "  removed oldest %s", files[min_index].name);

        err = at_collect_pwr_files(ctx, files, &count);
        if (err) {
            at_log(ctx, "  recollect numbered files failed: %d", err);
            return err;
        }
    }

    err = at_write_index_listing(ctx, AT_TARGET_DIR "/PWR.IDX.tmp", files, count);
    if (err) {
        at_log(ctx, "  write PWR.IDX.tmp failed: %d", err);
        return err;
    }
    at_log(ctx, "  wrote PWR.IDX.tmp with %u entries", (unsigned)count);

    err = lfs_rename(&ctx->lfs, AT_TARGET_DIR "/PWR.IDX.tmp", AT_TARGET_DIR "/PWR.IDX");
    if (err) {
        at_log(ctx, "  rename PWR.IDX.tmp -> PWR.IDX failed: %d", err);
        return err;
    }
    at_log(ctx, "  renamed PWR.IDX.tmp to PWR.IDX");

    bool all_real = false;
    err = at_check_all_real(ctx, &all_real);
    if (err) {
        at_log(ctx, "  lschk failed: %d", err);
        return err;
    }

    if (!all_real) {
        at_log(ctx, "iteration %d stop: found non-realfile entries", iteration);
        return 1;
    }

    at_log(ctx, "iteration %d end: all entries are realfile", iteration);
    return 0;
}

int main(int argc, char **argv) {
    const char *source_image = (argc >= 2) ? argv[1] : AT_SOURCE_IMAGE;
    const char *work_image = (argc >= 3) ? argv[2] : AT_WORK_IMAGE;
    int iterations = (argc >= 4) ? atoi(argv[3]) : 1000;

    FILE *log = fopen(AT_LOG_PATH, "w");
    if (!log) {
        fprintf(stderr, "failed to open log: %s\n", AT_LOG_PATH);
        return 1;
    }

    at_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log = log;
    ctx.source_image = source_image;
    ctx.work_image = work_image;

    srand((unsigned)time(NULL));

    at_log(&ctx, "autotest begin");
    at_log(&ctx, "source image: %s", source_image);
    at_log(&ctx, "work image: %s", work_image);
    at_log(&ctx, "target dir: %s", AT_TARGET_DIR);
    at_log(&ctx, "iterations: %d", iterations);

    int err = at_copy_file(source_image, work_image);
    if (err) {
        at_log(&ctx, "copy image failed: %d", err);
        fclose(log);
        return 1;
    }
    at_log(&ctx, "copied source image to work image");

    err = at_mount(&ctx);
    if (err) {
        at_log(&ctx, "mount failed: %d", err);
        fclose(log);
        return 1;
    }
    at_log(&ctx, "mounted work image successfully");

    int rc = 0;
    for (int i = 1; i <= iterations; i++) {
        err = at_iteration(&ctx, i);
        if (err == 1) {
            rc = 0;
            break;
        }
        if (err) {
            rc = 1;
            break;
        }
        if (i == iterations) {
            at_log(&ctx, "reached iteration limit %d", iterations);
        }
    }

    at_unmount(&ctx);
    at_log(&ctx, "autotest end");
    fclose(log);
    return rc;
}
