/*
 * littlefs simulator
 *
 * A simple Windows/Linux command-line simulator that runs the real littlefs
 * sources against a file-backed block device.
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define strcasecmp _stricmp
#define close _close
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#else
#include <unistd.h>
#endif

#include "bd/lfs_filebd.h"
#include "lfs.h"
#include "lfs_util.h"

extern volatile int g_flash_fault_injection_start;

#define SIM_PATH_MAX 1024
#define SIM_LINE_MAX 4096
#define SIM_ARGV_MAX 64
#define SIM_READ_CHUNK 4096

#define DEFAULT_BLOCK_SIZE      (256u * 1024u)
#define DEFAULT_BLOCK_COUNT     416u
#define DEFAULT_READ_SIZE       16u
#define DEFAULT_PROG_SIZE       16u
#define DEFAULT_CACHE_SIZE      256u
#define DEFAULT_LOOKAHEAD_SIZE  16u
#define DEFAULT_BLOCK_CYCLES    500

typedef struct sim_storage_cfg {
    lfs_size_t read_size;
    lfs_size_t prog_size;
    lfs_size_t block_size;
    lfs_size_t block_count;
    lfs_size_t cache_size;
    lfs_size_t lookahead_size;
    int32_t block_cycles;
} sim_storage_cfg_t;

typedef struct cli_options {
    const char *command;
    const char *image_path;
    const char *script_path;
    bool stop_on_error;

    bool has_read_size;
    bool has_prog_size;
    bool has_block_size;
    bool has_block_count;

    lfs_size_t read_size;
    lfs_size_t prog_size;
    lfs_size_t block_size;
    lfs_size_t block_count;
} cli_options_t;

typedef struct sim_state {
    sim_storage_cfg_t storage;
    lfs_filebd_t bd;
    struct lfs_filebd_config bd_cfg;
    struct lfs_config cfg;
    lfs_t lfs;

    uint8_t *read_buffer;
    uint8_t *prog_buffer;
    uint8_t *lookahead_buffer;

    bool device_open;
    bool mounted;
    char image_path[SIM_PATH_MAX];
    char current_path[SIM_PATH_MAX];
    bool fdwrite_open;
    lfs_file_t fdwrite;
    char fdwrite_path[SIM_PATH_MAX];
} sim_state_t;

typedef struct tree_stats {
    int dirs;
    int files;
    int corrupted;
    uint64_t total_bytes;
} tree_stats_t;

typedef struct block_usage_map {
    uint8_t *used;
    lfs_size_t count;
} block_usage_map_t;

typedef enum lschk_status {
    LSCHK_STATUS_REAL = 0,
    LSCHK_STATUS_GHOST,
    LSCHK_STATUS_DAMAGED,
} lschk_status_t;

typedef struct lschk_entry {
    char name[LFS_NAME_MAX+1];
    uint8_t type;
    lfs_size_t size;
    lfs_block_t pair[2];
    uint16_t id;
    uint16_t struct_type;
    bool duplicate;
    bool has_name;
    bool has_struct;
    bool data_valid;
    lschk_status_t status;
} lschk_entry_t;

static char g_exe_dir[SIM_PATH_MAX];

static void print_help(void);
static int parse_cli(int argc, char **argv, cli_options_t *options);
static void detect_executable_dir(
        const char *argv0, char *buffer, size_t buffer_size);
static void storage_cfg_set_defaults(sim_storage_cfg_t *storage);
static void storage_cfg_apply_overrides(
        sim_storage_cfg_t *storage, const cli_options_t *options);
static void storage_cfg_autosize_lookahead(sim_storage_cfg_t *storage);
static int storage_cfg_validate(const sim_storage_cfg_t *storage);
static int parse_size_arg(const char *text, lfs_size_t *value);
static void make_sidecar_path(
        const char *image_path, char *buffer, size_t buffer_size);
static int save_sidecar_config(
        const char *image_path, const sim_storage_cfg_t *storage);
static int load_sidecar_config(
        const char *image_path, sim_storage_cfg_t *storage, bool *found);
static int create_blank_image(
        const char *image_path, const sim_storage_cfg_t *storage);
static int image_file_size(const char *image_path, uint64_t *size);
static int infer_geometry_from_image(
        const char *image_path, sim_storage_cfg_t *storage);
static int validate_image_size(
        const char *image_path, const sim_storage_cfg_t *storage);
static int file_exists(const char *path);

static void sim_state_init(sim_state_t *sim);
static void sim_state_deinit(sim_state_t *sim);
static int sim_open_device(sim_state_t *sim, const char *image_path);
static int sim_mount(sim_state_t *sim);
static int sim_unmount(sim_state_t *sim);
static int sim_format(sim_state_t *sim);
static int sim_prepare_default_pwd(sim_state_t *sim);

static int resolve_path(
        const sim_state_t *sim, const char *input, char *output, size_t output_size);
static char *join_args(int argc, char **argv, int start);
static int split_command(char *line, char **argv, int max_args);
static char *trim_whitespace(char *text);
static void fill_random_bytes(uint8_t *buffer, size_t size);
static int run_internal_command(sim_state_t *sim, const char *fmt, ...);
static bool is_pwr_data_name(const char *name, uint32_t *index_out);
static int scan_pwr_files(sim_state_t *sim, const char *path,
        size_t *count_out, bool *have_any_out, uint32_t *min_index_out,
        uint32_t *max_index_out);
static int cmd_test(sim_state_t *sim, int argc, char **argv);
static int cmd_faulttest(sim_state_t *sim, int argc, char **argv);
static int cmd_ops(sim_state_t *sim, int argc, char **argv);
static int cmd_renametest(sim_state_t *sim, int argc, char **argv);

static int cmd_help(sim_state_t *sim, int argc, char **argv);
static int cmd_ls(sim_state_t *sim, int argc, char **argv);
static int cmd_lschk(sim_state_t *sim, int argc, char **argv);
static int cmd_lsrepair(sim_state_t *sim, int argc, char **argv);
static int cmd_cd(sim_state_t *sim, int argc, char **argv);
static int cmd_pwd(sim_state_t *sim, int argc, char **argv);
static int cmd_read(sim_state_t *sim, int argc, char **argv);
static int cmd_cat(sim_state_t *sim, int argc, char **argv);
static int cmd_hexdump(sim_state_t *sim, int argc, char **argv);
static int cmd_create_file(sim_state_t *sim, int argc, char **argv);
static int cmd_write(sim_state_t *sim, int argc, char **argv);
static int cmd_ops(sim_state_t *sim, int argc, char **argv);
static int cmd_renametest(sim_state_t *sim, int argc, char **argv);
static int cmd_mkdir(sim_state_t *sim, int argc, char **argv);
static int cmd_rm(sim_state_t *sim, int argc, char **argv);
static int cmd_cp(sim_state_t *sim, int argc, char **argv);
static int cmd_rename(sim_state_t *sim, int argc, char **argv);
static int cmd_stat(sim_state_t *sim, int argc, char **argv);
static int cmd_mount(sim_state_t *sim, int argc, char **argv);
static int cmd_umount(sim_state_t *sim, int argc, char **argv);
static int cmd_format(sim_state_t *sim, int argc, char **argv);
static int cmd_tree(sim_state_t *sim, int argc, char **argv);
static int cmd_meta_dump(sim_state_t *sim, int argc, char **argv);
static int cmd_inspect(sim_state_t *sim, int argc, char **argv);

static int dispatch_command(sim_state_t *sim, int argc, char **argv, bool *should_exit);
static int run_shell(sim_state_t *sim);
static int run_script(sim_state_t *sim, const char *script_path, bool stop_on_error);
static int remove_path_recursive(sim_state_t *sim, const char *path);
static int tree_walk(sim_state_t *sim, const char *path, int level, int max_depth,
        tree_stats_t *stats);
static int ensure_mounted(sim_state_t *sim);
static int read_file_alloc(sim_state_t *sim, const char *path, uint8_t **buffer, lfs_size_t *size);
static void print_hexdump(const uint8_t *data, size_t size);
static void print_hexdump_with_base(
        const uint8_t *data, size_t size, uint32_t base_offset);
static void fprint_hexdump_with_base(
        FILE *out, const uint8_t *data, size_t size, uint32_t base_offset);
static void fprint_erased_block_preview(FILE *out, size_t block_size);
static size_t effective_block_dump_size(const uint8_t *buffer, size_t block_size);
static size_t inspect_block_dump_size(const uint8_t *buffer, size_t block_size);
static int read_device_bytes(
        sim_state_t *sim, uint64_t offset, void *buffer, size_t size);
static int read_device_block(
        sim_state_t *sim, lfs_block_t block, uint8_t *buffer);
static int inspect_mark_used_block(void *data, lfs_block_t block);
static const char *meta_tag_type_name(uint16_t type);
static void fmeta_print_tag_data(
        FILE *out, uint16_t type, const uint8_t *data, lfs_size_t size);
static bool buffer_is_erased(const uint8_t *buffer, size_t size);
static bool meta_type_is_crc(uint16_t type);
static const char *lschk_status_name(lschk_status_t status);
static bool pair_equals(const lfs_block_t a[2], const lfs_block_t b[2]);
static int build_child_path(
        const char *dir_path, const char *name, char *buffer, size_t buffer_size);
static int collect_lschk_entries(
        sim_state_t *sim, const char *path, lschk_entry_t **entries_out, size_t *count_out);
static void build_default_meta_dump_name(
        const char *path, char *buffer, size_t buffer_size);
static void resolve_export_path(
        const char *requested, const char *target_path,
        char *buffer, size_t buffer_size);
static void emit_progress_tick(int saved_stdout);
static int meta_dump_target(sim_state_t *sim, const char *path,
        bool block_only, bool parsed_only, FILE *out);

int main(int argc, char **argv) {
    cli_options_t options;
    memset(&options, 0, sizeof(options));
    detect_executable_dir(argv[0], g_exe_dir, sizeof(g_exe_dir));
    srand((unsigned int)time(NULL));

    int err = parse_cli(argc, argv, &options);
    if (err) {
        return (err == 2) ? 0 : (err > 0 ? err : 1);
    }

    if (strcmp(options.command, "create") == 0) {
        sim_storage_cfg_t storage;
        storage_cfg_set_defaults(&storage);
        storage_cfg_apply_overrides(&storage, &options);
        storage_cfg_autosize_lookahead(&storage);

        err = storage_cfg_validate(&storage);
        if (err) {
            return 1;
        }

        err = create_blank_image(options.image_path, &storage);
        if (err) {
            return 1;
        }

        err = save_sidecar_config(options.image_path, &storage);
        if (err) {
            return 1;
        }

        printf("Created image: %s\n", options.image_path);
        printf("  block_size  : %"PRIu32"\n", (uint32_t)storage.block_size);
        printf("  block_count : %"PRIu32"\n", (uint32_t)storage.block_count);
        printf("  read_size   : %"PRIu32"\n", (uint32_t)storage.read_size);
        printf("  prog_size   : %"PRIu32"\n", (uint32_t)storage.prog_size);
        return 0;
    }

    sim_storage_cfg_t storage;
    storage_cfg_set_defaults(&storage);

    bool sidecar_found = false;
    err = load_sidecar_config(options.image_path, &storage, &sidecar_found);
    if (err) {
        return 1;
    }
    storage_cfg_apply_overrides(&storage, &options);

    if (!file_exists(options.image_path)) {
        fprintf(stderr, "Image does not exist: %s\n", options.image_path);
        return 1;
    }

    err = infer_geometry_from_image(options.image_path, &storage);
    if (err) {
        return 1;
    }

    err = storage_cfg_validate(&storage);
    if (err) {
        return 1;
    }

    err = validate_image_size(options.image_path, &storage);
    if (err) {
        return 1;
    }

    sim_state_t sim;
    sim_state_init(&sim);
    sim.storage = storage;

    err = sim_open_device(&sim, options.image_path);
    if (err) {
        sim_state_deinit(&sim);
        return 1;
    }

    printf("Opened image: %s\n", options.image_path);
    if (sidecar_found) {
        printf("Loaded sidecar config.\n");
    } else {
        printf("Sidecar config not found, using defaults/overrides.\n");
    }

    err = sim_mount(&sim);
    if (err) {
        printf("Continuing with device open but filesystem unmounted.\n");
        err = 0;
    }

    if (strcmp(options.command, "open") == 0) {
        err = run_shell(&sim);
    } else {
        err = run_script(&sim, options.script_path, options.stop_on_error);
    }

    sim_state_deinit(&sim);
    return (err == 0) ? 0 : 1;
}

static void print_help(void) {
    printf("littlefs simulator\n");
    printf("\n");
    printf("Commands:\n");
    printf("  create --image <file.bin> [--block-size N] [--block-count N]\n");
    printf("         [--read-size N] [--prog-size N]\n");
    printf("  open <file.bin> [--block-size N] [--block-count N]\n");
    printf("       [--read-size N] [--prog-size N]\n");
    printf("  run <script.lfs> --image <file.bin> [--stop-on-error]\n");
    printf("      [--block-size N] [--block-count N] [--read-size N] [--prog-size N]\n");
    printf("\n");
    printf("Default flash config:\n");
    printf("  block_size  = %u bytes\n", DEFAULT_BLOCK_SIZE);
    printf("  block_count = %u\n", DEFAULT_BLOCK_COUNT);
    printf("  read_size   = %u bytes\n", DEFAULT_READ_SIZE);
    printf("  prog_size   = %u bytes\n", DEFAULT_PROG_SIZE);
    printf("\n");
    printf("Shell commands:\n");
    printf("  ls [path]\n");
    printf("  lschk [path]\n");
    printf("  lsrepair [path] [--damaged]\n");
    printf("  cd <path>\n");
    printf("  pwd\n");
    printf("  read <file>\n");
    printf("  cat <file>\n");
    printf("  hexdump <file>\n");
    printf("  create <file>\n");
    printf("  write <file> <size> [data] [--append]\n");
    printf("  ops <name>\n");
    printf("  renametest <count>\n");
    printf("  faulttest <count> [output-file]\n");
    printf("  mkdir <dir>\n");
    printf("  rm <path> [--recursive]\n");
    printf("  cp <src> <dst>\n");
    printf("  rename <src> <dst>\n");
    printf("  stat <path>\n");
    printf("  test <count>\n");
    printf("  tree [path] [--depth N]\n");
    printf("  meta-dump <path> [--block-only] [--parsed-only] [--export [file.txt]]\n");
    printf("  inspect blocks\n");
    printf("  inspect block <N>\n");
    printf("  format\n");
    printf("  mount\n");
    printf("  umount\n");
    printf("  help\n");
    printf("  quit\n");
}

static int parse_cli(int argc, char **argv, cli_options_t *options) {
    if (argc < 2) {
        print_help();
        return 1;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help();
        return 2;
    }

    options->command = argv[1];
    if (strcmp(options->command, "create") != 0 &&
            strcmp(options->command, "open") != 0 &&
            strcmp(options->command, "run") != 0) {
        fprintf(stderr, "Unknown command: %s\n", options->command);
        print_help();
        return 1;
    }

    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--image") == 0) {
            if (i+1 >= argc) {
                fprintf(stderr, "--image requires a value\n");
                return 1;
            }
            options->image_path = argv[++i];
        } else if (strcmp(arg, "--block-size") == 0) {
            if (i+1 >= argc || parse_size_arg(argv[++i], &options->block_size)) {
                fprintf(stderr, "Invalid --block-size value\n");
                return 1;
            }
            options->has_block_size = true;
        } else if (strcmp(arg, "--block-count") == 0) {
            if (i+1 >= argc || parse_size_arg(argv[++i], &options->block_count)) {
                fprintf(stderr, "Invalid --block-count value\n");
                return 1;
            }
            options->has_block_count = true;
        } else if (strcmp(arg, "--read-size") == 0) {
            if (i+1 >= argc || parse_size_arg(argv[++i], &options->read_size)) {
                fprintf(stderr, "Invalid --read-size value\n");
                return 1;
            }
            options->has_read_size = true;
        } else if (strcmp(arg, "--prog-size") == 0) {
            if (i+1 >= argc || parse_size_arg(argv[++i], &options->prog_size)) {
                fprintf(stderr, "Invalid --prog-size value\n");
                return 1;
            }
            options->has_prog_size = true;
        } else if (strcmp(arg, "--stop-on-error") == 0) {
            options->stop_on_error = true;
        } else if (arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return 1;
        } else if (strcmp(options->command, "open") == 0 && options->image_path == NULL) {
            options->image_path = arg;
        } else if (strcmp(options->command, "run") == 0 && options->script_path == NULL) {
            options->script_path = arg;
        } else if (strcmp(options->command, "create") == 0 && options->image_path == NULL) {
            options->image_path = arg;
        } else {
            fprintf(stderr, "Unexpected positional argument: %s\n", arg);
            return 1;
        }
    }

    if (strcmp(options->command, "run") == 0 && options->script_path == NULL) {
        fprintf(stderr, "run requires a script path\n");
        return 1;
    }

    if (options->image_path == NULL) {
        fprintf(stderr, "%s requires an image path\n", options->command);
        return 1;
    }

    return 0;
}

static void detect_executable_dir(
        const char *argv0, char *buffer, size_t buffer_size) {
    if (buffer_size == 0) {
        return;
    }

#ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, buffer, (DWORD)buffer_size);
    if (len > 0 && len < buffer_size) {
        char *slash = strrchr(buffer, '\\');
        if (!slash) {
            slash = strrchr(buffer, '/');
        }
        if (slash) {
            *slash = '\0';
            return;
        }
    }
#else
    ssize_t len = readlink("/proc/self/exe", buffer, buffer_size - 1);
    if (len > 0 && (size_t)len < buffer_size) {
        buffer[len] = '\0';
        char *slash = strrchr(buffer, '/');
        if (slash) {
            *slash = '\0';
            return;
        }
    }
#endif

    if (argv0 && argv0[0] != '\0') {
        strncpy(buffer, argv0, buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
        char *slash = strrchr(buffer, '\\');
        if (!slash) {
            slash = strrchr(buffer, '/');
        }
        if (slash) {
            *slash = '\0';
            return;
        }
    }

#ifdef _WIN32
    _getcwd(buffer, (int)buffer_size);
#else
    getcwd(buffer, buffer_size);
#endif
    buffer[buffer_size - 1] = '\0';
}

static void storage_cfg_set_defaults(sim_storage_cfg_t *storage) {
    storage->read_size = DEFAULT_READ_SIZE;
    storage->prog_size = DEFAULT_PROG_SIZE;
    storage->block_size = DEFAULT_BLOCK_SIZE;
    storage->block_count = DEFAULT_BLOCK_COUNT;
    storage->cache_size = DEFAULT_CACHE_SIZE;
    storage->lookahead_size = DEFAULT_LOOKAHEAD_SIZE;
    storage->block_cycles = DEFAULT_BLOCK_CYCLES;
}

static void storage_cfg_apply_overrides(
        sim_storage_cfg_t *storage, const cli_options_t *options) {
    if (options->has_read_size) {
        storage->read_size = options->read_size;
    }
    if (options->has_prog_size) {
        storage->prog_size = options->prog_size;
    }
    if (options->has_block_size) {
        storage->block_size = options->block_size;
        if (storage->cache_size > storage->block_size) {
            storage->cache_size = storage->block_size;
        }
    }
    if (options->has_block_count) {
        storage->block_count = options->block_count;
    }
}

static void storage_cfg_autosize_lookahead(sim_storage_cfg_t *storage) {
    if (storage->block_count == 0) {
        return;
    }

    uint64_t min_bytes = ((uint64_t)storage->block_count + 7u) / 8u;
    uint64_t aligned = ((min_bytes + 7u) / 8u) * 8u;
    lfs_size_t required = (lfs_size_t)aligned;

    if (storage->lookahead_size < required) {
        storage->lookahead_size = required;
    }
}

static int storage_cfg_validate(const sim_storage_cfg_t *storage) {
    if (storage->read_size == 0 || storage->prog_size == 0 ||
            storage->block_size == 0 || storage->block_count == 0) {
        fprintf(stderr, "Storage config contains zero values\n");
        return -1;
    }

    if (storage->block_size < 128) {
        fprintf(stderr, "block_size must be >= 128\n");
        return -1;
    }

    if (storage->block_size % storage->read_size != 0) {
        fprintf(stderr, "block_size must be a multiple of read_size\n");
        return -1;
    }

    if (storage->block_size % storage->prog_size != 0) {
        fprintf(stderr, "block_size must be a multiple of prog_size\n");
        return -1;
    }

    if (storage->cache_size < storage->read_size ||
            storage->cache_size < storage->prog_size) {
        fprintf(stderr, "cache_size must be >= read_size/prog_size\n");
        return -1;
    }

    if (storage->block_size % storage->cache_size != 0) {
        fprintf(stderr, "block_size must be a multiple of cache_size\n");
        return -1;
    }

    if (storage->lookahead_size == 0 || storage->lookahead_size % 8 != 0) {
        fprintf(stderr, "lookahead_size must be a non-zero multiple of 8\n");
        return -1;
    }

    if (storage->lookahead_size * 8 < storage->block_count) {
        fprintf(stderr, "lookahead_size is too small for block_count\n");
        return -1;
    }

    return 0;
}

static int parse_size_arg(const char *text, lfs_size_t *value) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno || end == text || *end != '\0') {
        return -1;
    }

    *value = (lfs_size_t)parsed;
    return 0;
}

static void make_sidecar_path(
        const char *image_path, char *buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, "%s.cfg", image_path);
}

static int save_sidecar_config(
        const char *image_path, const sim_storage_cfg_t *storage) {
    char sidecar[SIM_PATH_MAX];
    make_sidecar_path(image_path, sidecar, sizeof(sidecar));

    FILE *f = fopen(sidecar, "w");
    if (!f) {
        fprintf(stderr, "Failed to write sidecar config %s: %s\n",
                sidecar, strerror(errno));
        return -1;
    }

    fprintf(f, "block_size=%"PRIu32"\n", (uint32_t)storage->block_size);
    fprintf(f, "block_count=%"PRIu32"\n", (uint32_t)storage->block_count);
    fprintf(f, "read_size=%"PRIu32"\n", (uint32_t)storage->read_size);
    fprintf(f, "prog_size=%"PRIu32"\n", (uint32_t)storage->prog_size);
    fclose(f);
    return 0;
}

static int load_sidecar_config(
        const char *image_path, sim_storage_cfg_t *storage, bool *found) {
    char sidecar[SIM_PATH_MAX];
    make_sidecar_path(image_path, sidecar, sizeof(sidecar));
    *found = false;

    FILE *f = fopen(sidecar, "r");
    if (!f) {
        if (errno == ENOENT) {
            return 0;
        }
        fprintf(stderr, "Failed to read sidecar config %s: %s\n",
                sidecar, strerror(errno));
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim_whitespace(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            continue;
        }

        char *eq = strchr(trimmed, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim_whitespace(trimmed);
        char *value = trim_whitespace(eq + 1);

        lfs_size_t parsed = 0;
        if (parse_size_arg(value, &parsed)) {
            continue;
        }

        if (strcmp(key, "block_size") == 0) {
            storage->block_size = parsed;
        } else if (strcmp(key, "block_count") == 0) {
            storage->block_count = parsed;
        } else if (strcmp(key, "read_size") == 0) {
            storage->read_size = parsed;
        } else if (strcmp(key, "prog_size") == 0) {
            storage->prog_size = parsed;
        }
    }

    fclose(f);
    *found = true;
    return 0;
}

static int create_blank_image(
        const char *image_path, const sim_storage_cfg_t *storage) {
    FILE *f = fopen(image_path, "wb");
    if (!f) {
        fprintf(stderr, "Failed to create image %s: %s\n",
                image_path, strerror(errno));
        return -1;
    }

    uint8_t *chunk = malloc(1024 * 1024);
    if (!chunk) {
        fclose(f);
        fprintf(stderr, "Out of memory while creating image\n");
        return -1;
    }
    memset(chunk, 0xff, 1024 * 1024);

    uint64_t total = (uint64_t)storage->block_size * storage->block_count;
    while (total > 0) {
        size_t n = (size_t)lfs_min((uint64_t)(1024 * 1024), total);
        if (fwrite(chunk, 1, n, f) != n) {
            free(chunk);
            fclose(f);
            fprintf(stderr, "Failed to initialize image %s: %s\n",
                    image_path, strerror(errno));
            return -1;
        }
        total -= n;
    }

    free(chunk);
    fclose(f);
    return 0;
}

static int image_file_size(const char *image_path, uint64_t *size) {
    struct stat st;
    if (stat(image_path, &st) != 0) {
        fprintf(stderr, "Failed to stat %s: %s\n", image_path, strerror(errno));
        return -1;
    }

    *size = (uint64_t)st.st_size;
    return 0;
}

static int infer_geometry_from_image(
        const char *image_path, sim_storage_cfg_t *storage) {
    uint64_t image_size = 0;
    int err = image_file_size(image_path, &image_size);
    if (err) {
        return err;
    }

    if (storage->block_size == 0) {
        fprintf(stderr, "block_size must be non-zero before inferring geometry\n");
        return -1;
    }

    if (image_size == 0) {
        fprintf(stderr, "Image %s is empty\n", image_path);
        return -1;
    }

    if (image_size % storage->block_size != 0) {
        fprintf(stderr,
                "Image size mismatch for %s: size %"PRIu64
                " is not divisible by block_size %"PRIu32"\n",
                image_path, image_size, (uint32_t)storage->block_size);
        return -1;
    }

    lfs_size_t inferred_block_count =
            (lfs_size_t)(image_size / storage->block_size);
    if (storage->block_count != inferred_block_count) {
        printf("Auto-adjusted block_count: %"PRIu32" -> %"PRIu32
                " based on image size %"PRIu64"\n",
                (uint32_t)storage->block_count,
                (uint32_t)inferred_block_count,
                image_size);
        storage->block_count = inferred_block_count;
    }

    lfs_size_t previous_lookahead = storage->lookahead_size;
    storage_cfg_autosize_lookahead(storage);
    if (storage->lookahead_size != previous_lookahead) {
        printf("Auto-adjusted lookahead_size: %"PRIu32" -> %"PRIu32
                " for block_count %"PRIu32"\n",
                (uint32_t)previous_lookahead,
                (uint32_t)storage->lookahead_size,
                (uint32_t)storage->block_count);
    }

    return 0;
}

static int validate_image_size(
        const char *image_path, const sim_storage_cfg_t *storage) {
    uint64_t image_size = 0;
    int err = image_file_size(image_path, &image_size);
    if (err) {
        return err;
    }

    uint64_t expected = (uint64_t)storage->block_size * storage->block_count;
    if (image_size != expected) {
        fprintf(stderr,
                "Image size mismatch for %s: expected %"PRIu64", got %"PRIu64"\n",
                image_path, expected, image_size);
        return -1;
    }

    return 0;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void sim_state_init(sim_state_t *sim) {
    memset(sim, 0, sizeof(*sim));
    strcpy(sim->current_path, "/");
    sim->fdwrite_path[0] = '\0';
}

static void sim_state_deinit(sim_state_t *sim) {
    if (sim->mounted && sim->fdwrite_open) {
        lfs_file_close(&sim->lfs, &sim->fdwrite);
        sim->fdwrite_open = false;
        sim->fdwrite_path[0] = '\0';
    }

    if (sim->mounted) {
        lfs_unmount(&sim->lfs);
        sim->mounted = false;
    }

    if (sim->device_open) {
        lfs_filebd_destroy(&sim->cfg);
        sim->device_open = false;
    }

    free(sim->read_buffer);
    free(sim->prog_buffer);
    free(sim->lookahead_buffer);
    sim->read_buffer = NULL;
    sim->prog_buffer = NULL;
    sim->lookahead_buffer = NULL;
}

static int sim_open_device(sim_state_t *sim, const char *image_path) {
    memset(&sim->cfg, 0, sizeof(sim->cfg));
    memset(&sim->bd_cfg, 0, sizeof(sim->bd_cfg));

    sim->read_buffer = malloc(sim->storage.cache_size);
    sim->prog_buffer = malloc(sim->storage.cache_size);
    sim->lookahead_buffer = malloc(sim->storage.lookahead_size);
    if (!sim->read_buffer || !sim->prog_buffer || !sim->lookahead_buffer) {
        fprintf(stderr, "Failed to allocate littlefs buffers\n");
        return -1;
    }

    sim->bd_cfg.erase_value = 0xff;

    sim->cfg.context = &sim->bd;
    sim->cfg.read = lfs_filebd_read;
    sim->cfg.prog = lfs_filebd_prog;
    sim->cfg.erase = lfs_filebd_erase;
    sim->cfg.sync = lfs_filebd_sync;
    sim->cfg.read_size = sim->storage.read_size;
    sim->cfg.prog_size = sim->storage.prog_size;
    sim->cfg.block_size = sim->storage.block_size;
    sim->cfg.block_count = sim->storage.block_count;
    sim->cfg.block_cycles = sim->storage.block_cycles;
    sim->cfg.cache_size = sim->storage.cache_size;
    sim->cfg.lookahead_size = sim->storage.lookahead_size;
    sim->cfg.read_buffer = sim->read_buffer;
    sim->cfg.prog_buffer = sim->prog_buffer;
    sim->cfg.lookahead_buffer = sim->lookahead_buffer;
    sim->cfg.name_max = LFS_NAME_MAX;
    sim->cfg.file_max = LFS_FILE_MAX;
    sim->cfg.attr_max = LFS_ATTR_MAX;
    sim->cfg.metadata_max = sim->storage.block_size;

    int err = lfs_filebd_createcfg(&sim->cfg, image_path, &sim->bd_cfg);
    if (err) {
        fprintf(stderr, "Failed to open block device %s: %d\n", image_path, err);
        return -1;
    }

    strncpy(sim->image_path, image_path, sizeof(sim->image_path)-1);
    sim->device_open = true;
    return 0;
}

static int sim_mount(sim_state_t *sim) {
    if (sim->mounted) {
        printf("Filesystem already mounted.\n");
        return 0;
    }

    int err = lfs_mount(&sim->lfs, &sim->cfg);
    if (err) {
        fprintf(stderr, "Mount failed: %d\n", err);
        return err;
    }

    sim->mounted = true;
    strcpy(sim->current_path, "/lfs0/LOG/PWR");
    printf("Filesystem mounted.\n");
    return sim_prepare_default_pwd(sim);
}

static int sim_unmount(sim_state_t *sim) {
    if (!sim->mounted) {
        printf("Filesystem already unmounted.\n");
        return 0;
    }

    if (sim->fdwrite_open) {
        int close_err = lfs_file_close(&sim->lfs, &sim->fdwrite);
        if (close_err) {
            fprintf(stderr, "fdwrite close before unmount failed: %d\n", close_err);
            return close_err;
        }
        sim->fdwrite_open = false;
        sim->fdwrite_path[0] = '\0';
    }

    int err = lfs_unmount(&sim->lfs);
    if (err) {
        fprintf(stderr, "Unmount failed: %d\n", err);
        return err;
    }

    sim->mounted = false;
    printf("Filesystem unmounted.\n");
    return 0;
}

static int sim_format(sim_state_t *sim) {
    if (sim->mounted) {
        if (sim->fdwrite_open) {
            int close_err = lfs_file_close(&sim->lfs, &sim->fdwrite);
            if (close_err) {
                fprintf(stderr, "fdwrite close before format failed: %d\n", close_err);
                return close_err;
            }
            sim->fdwrite_open = false;
            sim->fdwrite_path[0] = '\0';
        }

        int err = lfs_unmount(&sim->lfs);
        if (err) {
            fprintf(stderr, "Unmount before format failed: %d\n", err);
            return err;
        }
        sim->mounted = false;
    }

    int err = lfs_format(&sim->lfs, &sim->cfg);
    if (err) {
        fprintf(stderr, "Format failed: %d\n", err);
        return err;
    }

    printf("Filesystem formatted.\n");
    return sim_mount(sim);
}

static int sim_prepare_default_pwd(sim_state_t *sim) {
    const char *default_path = "/lfs0/LOG/PWR";
    struct lfs_info info;

    if (lfs_stat(&sim->lfs, "/lfs0", &info) < 0) {
        int err = lfs_mkdir(&sim->lfs, "/lfs0");
        if (err && err != LFS_ERR_EXIST) {
            fprintf(stderr, "Failed to create /lfs0: %d\n", err);
            return err;
        }
    }

    if (lfs_stat(&sim->lfs, "/lfs0/LOG", &info) < 0) {
        int err = lfs_mkdir(&sim->lfs, "/lfs0/LOG");
        if (err && err != LFS_ERR_EXIST) {
            fprintf(stderr, "Failed to create /lfs0/LOG: %d\n", err);
            return err;
        }
    }

    if (lfs_stat(&sim->lfs, default_path, &info) < 0) {
        int err = lfs_mkdir(&sim->lfs, default_path);
        if (err && err != LFS_ERR_EXIST) {
            fprintf(stderr, "Failed to create %s: %d\n", default_path, err);
            return err;
        }
    }

    strncpy(sim->current_path, default_path, sizeof(sim->current_path) - 1);
    sim->current_path[sizeof(sim->current_path) - 1] = '\0';
    return 0;
}

static int resolve_path(
        const sim_state_t *sim, const char *input, char *output, size_t output_size) {
    char working[SIM_PATH_MAX * 2];
    if (!input || input[0] == '\0') {
        return -1;
    }

    if (input[0] == '/') {
        snprintf(working, sizeof(working), "%s", input);
    } else if (strcmp(sim->current_path, "/") == 0) {
        snprintf(working, sizeof(working), "/%s", input);
    } else {
        snprintf(working, sizeof(working), "%s/%s", sim->current_path, input);
    }

    char *parts[SIM_ARGV_MAX];
    int part_count = 0;
    char *token = strtok(working, "/\\");
    while (token) {
        if (strcmp(token, ".") == 0) {
            // noop
        } else if (strcmp(token, "..") == 0) {
            if (part_count > 0) {
                part_count -= 1;
            }
        } else if (part_count < SIM_ARGV_MAX) {
            parts[part_count++] = token;
        }
        token = strtok(NULL, "/\\");
    }

    if (part_count == 0) {
        if (output_size < 2) {
            return -1;
        }
        strcpy(output, "/");
        return 0;
    }

    size_t off = 0;
    output[off++] = '/';
    for (int i = 0; i < part_count; i++) {
        size_t len = strlen(parts[i]);
        if (off + len + 1 >= output_size) {
            return -1;
        }
        memcpy(&output[off], parts[i], len);
        off += len;
        if (i + 1 != part_count) {
            output[off++] = '/';
        }
    }
    output[off] = '\0';
    return 0;
}

static char *join_args(int argc, char **argv, int start) {
    size_t len = 1;
    for (int i = start; i < argc; i++) {
        len += strlen(argv[i]) + 1;
    }

    char *result = malloc(len);
    if (!result) {
        return NULL;
    }

    result[0] = '\0';
    for (int i = start; i < argc; i++) {
        if (i != start) {
            strcat(result, " ");
        }
        strcat(result, argv[i]);
    }
    return result;
}

static int split_command(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < max_args) {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            argv[argc++] = p;
            while (*p != '\0' && *p != quote) {
                if (*p == '\\' && p[1] != '\0') {
                    memmove(p, p+1, strlen(p));
                }
                p++;
            }
            if (*p == quote) {
                *p++ = '\0';
            }
        } else {
            argv[argc++] = p;
            while (*p != '\0' && !isspace((unsigned char)*p)) {
                p++;
            }
            if (*p != '\0') {
                *p++ = '\0';
            }
        }
    }

    return argc;
}

static char *trim_whitespace(char *text) {
    while (isspace((unsigned char)*text)) {
        text++;
    }

    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static void fill_random_bytes(uint8_t *buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)(rand() & 0xff);
    }
}

static int run_internal_command(sim_state_t *sim, const char *fmt, ...) {
    char line[SIM_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    printf("test> %s\n", line);

    char parse_buffer[SIM_LINE_MAX];
    strncpy(parse_buffer, line, sizeof(parse_buffer) - 1);
    parse_buffer[sizeof(parse_buffer) - 1] = '\0';

    char *argv[SIM_ARGV_MAX];
    int argc = split_command(parse_buffer, argv, SIM_ARGV_MAX);
    bool should_exit = false;
    int err = dispatch_command(sim, argc, argv, &should_exit);
    if (should_exit) {
        return -1;
    }
    return err;
}

static bool is_pwr_data_name(const char *name, uint32_t *index_out) {
    if (strlen(name) != 12 || strcmp(name + 8, ".PWR") != 0) {
        return false;
    }

    uint32_t value = 0;
    for (int i = 0; i < 8; i++) {
        if (!isdigit((unsigned char)name[i])) {
            return false;
        }
        value = value * 10u + (uint32_t)(name[i] - '0');
    }

    if (index_out) {
        *index_out = value;
    }
    return true;
}

static int scan_pwr_files(sim_state_t *sim, const char *path,
        size_t *count_out, bool *have_any_out, uint32_t *min_index_out,
        uint32_t *max_index_out) {
    lfs_dir_t dir;
    int err = lfs_dir_open(&sim->lfs, &dir, path);
    if (err) {
        return err;
    }

    size_t count = 0;
    bool have_any = false;
    uint32_t min_index = 0;
    uint32_t max_index = 0;
    struct lfs_info info;

    while ((err = lfs_dir_read(&sim->lfs, &dir, &info)) > 0) {
        uint32_t index = 0;
        if (info.type != LFS_TYPE_REG || !is_pwr_data_name(info.name, &index)) {
            continue;
        }

        if (!have_any) {
            have_any = true;
            min_index = index;
            max_index = index;
        } else {
            if (index < min_index) {
                min_index = index;
            }
            if (index > max_index) {
                max_index = index;
            }
        }
        count++;
    }

    lfs_dir_close(&sim->lfs, &dir);
    if (err < 0) {
        return err;
    }

    if (count_out) {
        *count_out = count;
    }
    if (have_any_out) {
        *have_any_out = have_any;
    }
    if (min_index_out) {
        *min_index_out = min_index;
    }
    if (max_index_out) {
        *max_index_out = max_index;
    }
    return 0;
}

static const char *lschk_status_name(lschk_status_t status) {
    switch (status) {
    case LSCHK_STATUS_REAL:
        return "realfile";
    case LSCHK_STATUS_GHOST:
        return "ghostfile";
    case LSCHK_STATUS_DAMAGED:
        return "damagedfile";
    default:
        return "unknown";
    }
}

static bool pair_equals(const lfs_block_t a[2], const lfs_block_t b[2]) {
    return a[0] == b[0] && a[1] == b[1];
}

static int build_child_path(
        const char *dir_path, const char *name, char *buffer, size_t buffer_size) {
    if (strcmp(dir_path, "/") == 0) {
        return snprintf(buffer, buffer_size, "/%s", name) < (int)buffer_size
                ? 0 : -1;
    }

    return snprintf(buffer, buffer_size, "%s/%s", dir_path, name) < (int)buffer_size
            ? 0 : -1;
}

static int cmd_help(sim_state_t *sim, int argc, char **argv) {
    (void)sim;
    (void)argc;
    (void)argv;
    print_help();
    return 0;
}

static int ensure_mounted(sim_state_t *sim) {
    if (!sim->mounted) {
        fprintf(stderr, "Filesystem is not mounted.\n");
        return -1;
    }
    return 0;
}

static int cmd_ls(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim)) {
        return -1;
    }

    char path[SIM_PATH_MAX];
    const char *target = (argc >= 2) ? argv[1] : sim->current_path;
    if (resolve_path(sim, target, path, sizeof(path))) {
        fprintf(stderr, "Invalid path.\n");
        return -1;
    }

    lfs_dir_t dir;
    int err = lfs_dir_open(&sim->lfs, &dir, path);
    if (err) {
        fprintf(stderr, "ls: failed to open %s: %d\n", path, err);
        return err;
    }

    printf("Listing %s\n", path);
    printf("%-6s %-10s %s\n", "TYPE", "SIZE", "NAME");

    struct lfs_info info;
    while ((err = lfs_dir_read(&sim->lfs, &dir, &info)) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }
        printf("%-6s %-10"PRIu32" %s\n",
                (info.type == LFS_TYPE_DIR) ? "DIR" : "FILE",
                (uint32_t)info.size,
                info.name);
    }

    lfs_dir_close(&sim->lfs, &dir);
    return (err < 0) ? err : 0;
}

static int collect_lschk_entries(
        sim_state_t *sim, const char *path, lschk_entry_t **entries_out, size_t *count_out) {
    lfs_dir_t dir;
    int err = lfs_dir_open(&sim->lfs, &dir, path);
    if (err) {
        return err;
    }

    lschk_entry_t *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;

    struct lfs_info info;
    while ((err = lfs_dir_read(&sim->lfs, &dir, &info)) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }

        if (count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 16;
            lschk_entry_t *next = realloc(entries, new_capacity * sizeof(*entries));
            if (!next) {
                free(entries);
                lfs_dir_close(&sim->lfs, &dir);
                fprintf(stderr, "lschk: out of memory\n");
                return -1;
            }
            entries = next;
            capacity = new_capacity;
        }

        lschk_entry_t *entry = &entries[count++];
        memset(entry, 0, sizeof(*entry));
        strncpy(entry->name, info.name, sizeof(entry->name) - 1);
        entry->type = info.type;
        entry->size = info.size;
        entry->pair[0] = dir.m.pair[0];
        entry->pair[1] = dir.m.pair[1];
        entry->id = (dir.id > 0) ? (uint16_t)(dir.id - 1) : 0;
        entry->status = LSCHK_STATUS_REAL;
    }

    lfs_dir_close(&sim->lfs, &dir);
    if (err < 0) {
        free(entries);
        return err;
    }

    for (size_t i = 0; i < count; i++) {
        lfs_debug_entry_t probe;
        err = lfs_debug_probeentry(&sim->lfs, path, entries[i].id, &probe);
        if (err) {
            entries[i].status = LSCHK_STATUS_DAMAGED;
            continue;
        }

        entries[i].has_name = probe.has_name;
        entries[i].has_struct = probe.has_struct;
        entries[i].data_valid = probe.data_valid;
        entries[i].struct_type = probe.struct_type;
        if (probe.has_name && probe.name[0] != '\0') {
            strncpy(entries[i].name, probe.name, sizeof(entries[i].name) - 1);
            entries[i].name[sizeof(entries[i].name) - 1] = '\0';
        }
        if (probe.type != 0) {
            entries[i].type = probe.type;
        }
        entries[i].size = probe.size;

        if (!probe.has_struct) {
            entries[i].status = LSCHK_STATUS_GHOST;
        } else if (!probe.data_valid) {
            entries[i].status = LSCHK_STATUS_DAMAGED;
        } else {
            entries[i].status = LSCHK_STATUS_REAL;
        }
    }

    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (strcmp(entries[i].name, entries[j].name) == 0) {
                entries[i].duplicate = true;
                entries[j].duplicate = true;
            }
        }
    }

    bool *processed = calloc(count ? count : 1, sizeof(bool));
    if (!processed) {
        free(entries);
        fprintf(stderr, "lschk: out of memory\n");
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        if (processed[i]) {
            continue;
        }

        char child_path[SIM_PATH_MAX];
        if (build_child_path(path, entries[i].name, child_path, sizeof(child_path))) {
            entries[i].status = LSCHK_STATUS_DAMAGED;
            processed[i] = true;
            continue;
        }

        bool winner_found = false;
        bool winner_is_file = false;
        lfs_block_t winner_pair[2] = {0, 0};
        uint16_t winner_id = 0;
        uint8_t winner_type = 0;
        int winner_index = -1;

        lfs_file_t file;
        int open_err = lfs_file_open(&sim->lfs, &file, child_path, LFS_O_RDONLY);
        if (open_err == 0) {
            winner_found = true;
            winner_is_file = true;
            winner_pair[0] = file.m.pair[0];
            winner_pair[1] = file.m.pair[1];
            winner_id = file.id;
            winner_type = file.type;
            lfs_file_close(&sim->lfs, &file);
        } else {
            lfs_dir_t child_dir;
            open_err = lfs_dir_open(&sim->lfs, &child_dir, child_path);
            if (open_err == 0) {
                winner_found = true;
                winner_type = LFS_TYPE_DIR;
                lfs_dir_close(&sim->lfs, &child_dir);
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
            if (entries[j].status == LSCHK_STATUS_GHOST ||
                    entries[j].status == LSCHK_STATUS_DAMAGED) {
                continue;
            }

            if (!winner_found) {
                entries[j].status = LSCHK_STATUS_DAMAGED;
                continue;
            }

            if (!entries[j].duplicate) {
                entries[j].status = LSCHK_STATUS_REAL;
                continue;
            }

            if (winner_is_file) {
                if (entries[j].type == winner_type &&
                        entries[j].id == winner_id &&
                        pair_equals(entries[j].pair, winner_pair)) {
                    entries[j].status = LSCHK_STATUS_REAL;
                } else if (entries[j].type == winner_type) {
                    entries[j].status = LSCHK_STATUS_GHOST;
                } else {
                    entries[j].status = LSCHK_STATUS_DAMAGED;
                }
            } else {
                if ((int)j == winner_index) {
                    entries[j].status = LSCHK_STATUS_REAL;
                } else if (entries[j].type == winner_type) {
                    entries[j].status = LSCHK_STATUS_GHOST;
                } else {
                    entries[j].status = LSCHK_STATUS_DAMAGED;
                }
            }
        }
    }

    free(processed);
    *entries_out = entries;
    *count_out = count;
    return 0;
}

static int cmd_lschk(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim)) {
        return -1;
    }

    char path[SIM_PATH_MAX];
    const char *target = (argc >= 2) ? argv[1] : sim->current_path;
    if (resolve_path(sim, target, path, sizeof(path))) {
        fprintf(stderr, "Invalid path.\n");
        return -1;
    }

    lschk_entry_t *entries = NULL;
    size_t count = 0;
    int err = collect_lschk_entries(sim, path, &entries, &count);
    if (err) {
        fprintf(stderr, "lschk: failed to scan %s: %d\n", path, err);
        return err;
    }

    printf("Checking %s\n", path);
    printf("%-6s %-10s %s\n", "TYPE", "SIZE", "NAME");
    for (size_t i = 0; i < count; i++) {
        printf("%-6s %-10"PRIu32" %s [%s] id=%u pair={0x%"PRIx32",0x%"PRIx32"}\n",
                (entries[i].type == LFS_TYPE_DIR) ? "DIR" : "FILE",
                (uint32_t)entries[i].size,
                entries[i].name,
                lschk_status_name(entries[i].status),
                entries[i].id,
                entries[i].pair[0],
                entries[i].pair[1]);
    }

    free(entries);
    return 0;
}

static int cmd_lsrepair(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim)) {
        return -1;
    }

    char path[SIM_PATH_MAX];
    const char *target = sim->current_path;
    bool remove_damaged = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--damaged") == 0) {
            remove_damaged = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "lsrepair: unknown option %s\n", argv[i]);
            return -1;
        } else if (target == sim->current_path) {
            target = argv[i];
        } else {
            fprintf(stderr, "lsrepair: unexpected argument %s\n", argv[i]);
            return -1;
        }
    }

    if (resolve_path(sim, target, path, sizeof(path))) {
        fprintf(stderr, "Invalid path.\n");
        return -1;
    }

    int removed = 0;
    int skipped = 0;

    while (true) {
        lschk_entry_t *entries = NULL;
        size_t count = 0;
        int err = collect_lschk_entries(sim, path, &entries, &count);
        if (err) {
            fprintf(stderr, "lsrepair: failed to scan %s: %d\n", path, err);
            return err;
        }

        int target_index = -1;
        for (size_t i = 0; i < count; i++) {
            if ((entries[i].status == LSCHK_STATUS_GHOST ||
                    (remove_damaged &&
                     entries[i].status == LSCHK_STATUS_DAMAGED)) &&
                    entries[i].type == LFS_TYPE_REG) {
                target_index = (int)i;
                break;
            }
        }

        if (target_index < 0) {
            for (size_t i = 0; i < count; i++) {
                if (entries[i].status == LSCHK_STATUS_GHOST &&
                        entries[i].type != LFS_TYPE_REG) {
                    printf("skip  %s [ghostfile] id=%u : directory ghost entries are not auto-repaired\n",
                            entries[i].name, entries[i].id);
                    skipped++;
                } else if (entries[i].status == LSCHK_STATUS_DAMAGED) {
                    if (entries[i].type != LFS_TYPE_REG) {
                        printf("skip  %s [damagedfile] id=%u : directory damaged entries are not auto-repaired\n",
                                entries[i].name, entries[i].id);
                    } else if (!remove_damaged) {
                        printf("skip  %s [damagedfile] id=%u : use --damaged to allow repair\n",
                                entries[i].name, entries[i].id);
                    } else {
                        printf("skip  %s [damagedfile] id=%u : manual repair required\n",
                                entries[i].name, entries[i].id);
                    }
                    skipped++;
                }
            }
            free(entries);
            break;
        }

        lschk_entry_t victim = entries[target_index];
        free(entries);

        int remove_err = lfs_debug_removeghost(&sim->lfs, path, victim.name, victim.id);
        if (remove_err) {
            printf("fail  %s [%s] id=%u : %d\n",
                    victim.name,
                    lschk_status_name(victim.status),
                    victim.id,
                    remove_err);
            return remove_err;
        }

        printf("fix   %s [%s] id=%u removed\n",
                victim.name,
                lschk_status_name(victim.status),
                victim.id);
        removed++;
    }

    printf("lsrepair summary: removed=%d skipped=%d\n", removed, skipped);
    return 0;
}

static int cmd_cd(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 2) {
        return -1;
    }

    char path[SIM_PATH_MAX];
    if (resolve_path(sim, argv[1], path, sizeof(path))) {
        fprintf(stderr, "cd: invalid path\n");
        return -1;
    }

    struct lfs_info info;
    int err = lfs_stat(&sim->lfs, path, &info);
    if (err) {
        fprintf(stderr, "cd: %s: %d\n", path, err);
        return err;
    }
    if (info.type != LFS_TYPE_DIR) {
        fprintf(stderr, "cd: not a directory: %s\n", path);
        return -1;
    }

    strncpy(sim->current_path, path, sizeof(sim->current_path)-1);
    sim->current_path[sizeof(sim->current_path)-1] = '\0';
    return 0;
}

static int cmd_pwd(sim_state_t *sim, int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("%s\n", sim->current_path);
    return 0;
}

static int cmd_read(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 2) {
        fprintf(stderr, "usage: read <file>\n");
        return -1;
    }

    char path[SIM_PATH_MAX];
    if (resolve_path(sim, argv[1], path, sizeof(path))) {
        fprintf(stderr, "read: invalid path\n");
        return -1;
    }

    uint8_t *buffer = NULL;
    lfs_size_t size = 0;
    int err = read_file_alloc(sim, path, &buffer, &size);
    if (err) {
        fprintf(stderr, "read: %s: %d\n", path, err);
        return err;
    }

    free(buffer);
    printf("Read %"PRIu32" bytes from %s\n", (uint32_t)size, path);
    return 0;
}

static int read_file_alloc(sim_state_t *sim, const char *path, uint8_t **buffer, lfs_size_t *size) {
    struct lfs_info info;
    int err = lfs_stat(&sim->lfs, path, &info);
    if (err) {
        return err;
    }
    if (info.type != LFS_TYPE_REG) {
        return LFS_ERR_ISDIR;
    }

    *buffer = malloc(info.size ? info.size : 1);
    if (!*buffer) {
        return LFS_ERR_NOMEM;
    }

    lfs_file_t file;
    err = lfs_file_open(&sim->lfs, &file, path, LFS_O_RDONLY);
    if (err) {
        free(*buffer);
        *buffer = NULL;
        return err;
    }

    lfs_ssize_t res = lfs_file_read(&sim->lfs, &file, *buffer, info.size);
    lfs_file_close(&sim->lfs, &file);
    if (res < 0) {
        free(*buffer);
        *buffer = NULL;
        return (int)res;
    }

    *size = (lfs_size_t)res;
    return 0;
}

static int read_device_bytes(
        sim_state_t *sim, uint64_t offset, void *buffer, size_t size) {
    uint8_t *cursor = buffer;
    size_t remaining = size;

    off_t pos = lseek(sim->bd.fd, (off_t)offset, SEEK_SET);
    if (pos < 0) {
        int err = -errno;
        fprintf(stderr, "device read seek failed at 0x%"PRIx64": %d\n",
                offset, err);
        return err;
    }

    while (remaining > 0) {
        ssize_t res = read(sim->bd.fd, cursor, remaining);
        if (res < 0) {
            int err = -errno;
            fprintf(stderr, "device read failed at 0x%"PRIx64": %d\n",
                    offset + (uint64_t)(cursor - (uint8_t*)buffer), err);
            return err;
        }
        if (res == 0) {
            memset(cursor, 0xff, remaining);
            break;
        }

        cursor += res;
        remaining -= (size_t)res;
    }

    return 0;
}

static int read_device_block(
        sim_state_t *sim, lfs_block_t block, uint8_t *buffer) {
    if (block >= sim->storage.block_count) {
        fprintf(stderr, "block out of range: %"PRIu32"\n", block);
        return -1;
    }

    return read_device_bytes(sim,
            (uint64_t)block * sim->storage.block_size,
            buffer, sim->storage.block_size);
}

static int cmd_cat(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 2) {
        return -1;
    }

    char path[SIM_PATH_MAX];
    if (resolve_path(sim, argv[1], path, sizeof(path))) {
        fprintf(stderr, "cat: invalid path\n");
        return -1;
    }

    uint8_t *buffer = NULL;
    lfs_size_t size = 0;
    int err = read_file_alloc(sim, path, &buffer, &size);
    if (err) {
        fprintf(stderr, "cat: %s: %d\n", path, err);
        return err;
    }

    fwrite(buffer, 1, size, stdout);
    if (size == 0 || buffer[size-1] != '\n') {
        putchar('\n');
    }
    free(buffer);
    return 0;
}

static void print_hexdump_with_base(
        const uint8_t *data, size_t size, uint32_t base_offset) {
    fprint_hexdump_with_base(stdout, data, size, base_offset);
}

static void fprint_hexdump_with_base(
        FILE *out, const uint8_t *data, size_t size, uint32_t base_offset) {
    for (size_t i = 0; i < size; i += 16) {
        fprintf(out, "%08"PRIx32"  ", base_offset + (uint32_t)i);
        for (size_t j = 0; j < 16; j++) {
            if (i+j < size) {
                fprintf(out, "%02x ", data[i+j]);
            } else {
                fprintf(out, "   ");
            }
        }
        fprintf(out, " ");
        for (size_t j = 0; j < 16 && i+j < size; j++) {
            uint8_t c = data[i+j];
            fputc(isprint(c) ? c : '.', out);
        }
        fputc('\n', out);
    }
}

static void print_hexdump(const uint8_t *data, size_t size) {
    print_hexdump_with_base(data, size, 0);
}

static void fprint_erased_block_preview(FILE *out, size_t block_size) {
    uint8_t line[32];
    memset(line, 0xff, sizeof(line));
    fprint_hexdump_with_base(out, line, sizeof(line), 0);
    if (block_size > sizeof(line)) {
        fprintf(out, "... erased block omitted (%zu bytes total)\n", block_size);
    }
}

static size_t effective_block_dump_size(const uint8_t *buffer, size_t block_size) {
    size_t end = block_size;
    while (end > 0 && buffer[end - 1] == 0xff) {
        end--;
    }

    if (end == 0) {
        return 0;
    }

    // Round up to a full hexdump row so the final line stays aligned.
    size_t rounded = ((end + 15) / 16) * 16;
    return lfs_min(rounded, block_size);
}

static size_t inspect_block_dump_size(const uint8_t *buffer, size_t block_size) {
    if (block_size < 1024) {
        return block_size;
    }

    for (size_t start = 0; start + 1024 <= block_size; start++) {
        bool erased = true;
        for (size_t i = 0; i < 1024; i++) {
            if (buffer[start + i] != 0xff) {
                erased = false;
                break;
            }
        }

        if (erased) {
            size_t rounded = (start / 16) * 16;
            return lfs_min(rounded, block_size);
        }
    }

    return block_size;
}

static int inspect_mark_used_block(void *data, lfs_block_t block) {
    block_usage_map_t *map = data;
    if (block < map->count) {
        map->used[block] = 1;
    }
    return 0;
}

static bool buffer_is_erased(const uint8_t *buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (buffer[i] != 0xff) {
            return false;
        }
    }
    return true;
}

static void build_default_meta_dump_name(
        const char *path, char *buffer, size_t buffer_size) {
    const char *source = (strcmp(path, "/") == 0) ? "root" : path;
    size_t off = 0;
    const char *prefix = "meta_dump_";
    while (*prefix != '\0' && off + 1 < buffer_size) {
        buffer[off++] = *prefix++;
    }

    for (const char *p = source; *p != '\0' && off + 5 < buffer_size; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_') {
            buffer[off++] = (char)c;
        } else {
            buffer[off++] = '_';
        }
    }

    const char *suffix = ".txt";
    while (*suffix != '\0' && off + 1 < buffer_size) {
        buffer[off++] = *suffix++;
    }
    buffer[off] = '\0';
}

static void resolve_export_path(
        const char *requested, const char *target_path,
        char *buffer, size_t buffer_size) {
    char filename[SIM_PATH_MAX];
    if (requested && requested[0] != '\0') {
        strncpy(filename, requested, sizeof(filename) - 1);
        filename[sizeof(filename) - 1] = '\0';
    } else {
        build_default_meta_dump_name(target_path, filename, sizeof(filename));
    }

    bool absolute = false;
#ifdef _WIN32
    absolute = (strlen(filename) >= 2 && filename[1] == ':') ||
            filename[0] == '\\' || filename[0] == '/';
#else
    absolute = filename[0] == '/';
#endif

    if (absolute) {
        strncpy(buffer, filename, buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
        return;
    }

#ifdef _WIN32
    snprintf(buffer, buffer_size, "%s\\%s", g_exe_dir, filename);
#else
    snprintf(buffer, buffer_size, "%s/%s", g_exe_dir, filename);
#endif
}

static void emit_progress_tick(int saved_stdout) {
    const char dot = '.';
    int fd = (saved_stdout >= 0) ? saved_stdout : fileno(stdout);
    if (fd < 0) {
        return;
    }
#ifdef _WIN32
    _write(fd, &dot, 1);
#else
    write(fd, &dot, 1);
#endif
}

static const char *meta_tag_type_name(uint16_t type) {
    switch (type) {
    case LFS_TYPE_REG:
        return "reg";
    case LFS_TYPE_DIR:
        return "dir";
    case LFS_TYPE_SUPERBLOCK:
        return "superblock";
    case LFS_TYPE_DIRSTRUCT:
        return "dirstruct";
    case LFS_TYPE_INLINESTRUCT:
        return "inlinestruct";
    case LFS_TYPE_CTZSTRUCT:
        return "ctzstruct";
    case LFS_TYPE_SOFTTAIL:
        return "softtail";
    case LFS_TYPE_HARDTAIL:
        return "hardtail";
    case LFS_TYPE_MOVESTATE:
        return "movestate";
    case LFS_TYPE_CREATE:
        return "create";
    case LFS_TYPE_DELETE:
        return "delete";
    default:
        break;
    }

    if ((type & 0x700) == LFS_TYPE_NAME) {
        return "name";
    }
    if ((type & 0x700) == LFS_TYPE_STRUCT) {
        return "struct";
    }
    if ((type & 0x700) == LFS_TYPE_USERATTR) {
        return "userattr";
    }
    if ((type & 0x700) == LFS_TYPE_TAIL) {
        return "tail";
    }
    if ((type & 0x700) == LFS_TYPE_GLOBALS) {
        return "gstate";
    }
    if ((type & 0x700) == LFS_TYPE_CRC) {
        return "crc";
    }
    if ((type & 0x700) == LFS_TYPE_SPLICE) {
        return "splice";
    }

    return "unknown";
}

static bool meta_type_is_crc(uint16_t type) {
    return (type & 0x700u) == LFS_TYPE_CRC;
}

static void fmeta_print_tag_data(
        FILE *out, uint16_t type, const uint8_t *data, lfs_size_t size) {
    bool show_ascii = (type == LFS_TYPE_REG || type == LFS_TYPE_DIR ||
            type == LFS_TYPE_SUPERBLOCK || (type & 0x700) == LFS_TYPE_NAME);
    if (show_ascii && size > 0) {
        fprintf(out, " data=\"");
        for (lfs_size_t i = 0; i < size; i++) {
            uint8_t c = data[i];
            if (isprint(c) && c != '"' && c != '\\') {
                fputc(c, out);
            } else {
                fprintf(out, "\\x%02x", c);
            }
        }
        fprintf(out, "\"");
        return;
    }

    if ((type == LFS_TYPE_DIRSTRUCT || type == LFS_TYPE_SOFTTAIL ||
            type == LFS_TYPE_HARDTAIL || type == LFS_TYPE_MOVESTATE) &&
            size >= 8) {
        uint32_t pair0;
        uint32_t pair1;
        memcpy(&pair0, data, sizeof(pair0));
        memcpy(&pair1, data + 4, sizeof(pair1));
        fprintf(out, " pair={0x%"PRIx32",0x%"PRIx32"}",
                lfs_fromle32(pair0), lfs_fromle32(pair1));
        return;
    }

    if (type == LFS_TYPE_CTZSTRUCT && size >= 8) {
        uint32_t head;
        uint32_t bytes;
        memcpy(&head, data, sizeof(head));
        memcpy(&bytes, data + 4, sizeof(bytes));
        fprintf(out, " ctz={head=0x%"PRIx32", size=%"PRIu32"}",
                lfs_fromle32(head), lfs_fromle32(bytes));
        return;
    }

    if (meta_type_is_crc(type) && size >= 4) {
        uint32_t crc;
        memcpy(&crc, data, sizeof(crc));
        fprintf(out, " value=0x%08"PRIx32, lfs_fromle32(crc));
        return;
    }

    if (size == 0) {
        return;
    }

    fprintf(out, " data=");
    lfs_size_t limit = lfs_min(size, (lfs_size_t)16);
    for (lfs_size_t i = 0; i < limit; i++) {
        fprintf(out, "%02x", data[i]);
    }
    if (size > limit) {
        fprintf(out, "...");
    }
}

static int cmd_hexdump(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 2) {
        return -1;
    }

    char path[SIM_PATH_MAX];
    if (resolve_path(sim, argv[1], path, sizeof(path))) {
        fprintf(stderr, "hexdump: invalid path\n");
        return -1;
    }

    uint8_t *buffer = NULL;
    lfs_size_t size = 0;
    int err = read_file_alloc(sim, path, &buffer, &size);
    if (err) {
        fprintf(stderr, "hexdump: %s: %d\n", path, err);
        return err;
    }

    print_hexdump(buffer, size);
    free(buffer);
    return 0;
}

static int cmd_create_file(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 2) {
        fprintf(stderr, "usage: create <file>\n");
        return -1;
    }

    char path[SIM_PATH_MAX];
    if (resolve_path(sim, argv[1], path, sizeof(path))) {
        fprintf(stderr, "create: invalid path\n");
        return -1;
    }

    lfs_file_t file;
    int err = lfs_file_open(&sim->lfs, &file, path, LFS_O_CREAT | LFS_O_WRONLY);
    if (err) {
        fprintf(stderr, "create: failed to open %s: %d\n", path, err);
        return err;
    }

    err = lfs_file_close(&sim->lfs, &file);
    if (err) {
        fprintf(stderr, "create: failed to close %s: %d\n", path, err);
        return err;
    }

    return 0;
}

static int cmd_write(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 3) {
        fprintf(stderr, "usage: write <file> <size> [data] [--append]\n");
        return -1;
    }

    char path[SIM_PATH_MAX];
    if (resolve_path(sim, argv[1], path, sizeof(path))) {
        fprintf(stderr, "write: invalid path\n");
        return -1;
    }

    lfs_size_t size = 0;
    if (parse_size_arg(argv[2], &size)) {
        fprintf(stderr, "write: invalid size %s\n", argv[2]);
        return -1;
    }

    bool append = false;
    size_t data_argc = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--append") == 0) {
            append = true;
        } else {
            data_argc++;
        }
    }

    char *data = NULL;
    size_t data_len = 0;
    if (data_argc > 0) {
        char **data_argv = malloc(data_argc * sizeof(*data_argv));
        if (!data_argv) {
            fprintf(stderr, "write: out of memory\n");
            return -1;
        }

        size_t index = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--append") != 0) {
                data_argv[index++] = argv[i];
            }
        }

        data = join_args((int)data_argc, data_argv, 0);
        free(data_argv);
        if (!data) {
            fprintf(stderr, "write: out of memory\n");
            return -1;
        }
        data_len = strlen(data);
    }

    uint8_t *buffer = malloc(size > 0 ? size : 1);
    if (!buffer) {
        fprintf(stderr, "write: out of memory\n");
        free(data);
        return -1;
    }

    if (size > 0) {
        fill_random_bytes(buffer, size);
        if (data_len > 0) {
            size_t copy_len = data_len < size ? data_len : size;
            memcpy(buffer, data, copy_len);
        }
    }

    lfs_file_t file;
    int err = lfs_file_open(&sim->lfs, &file, path,
            LFS_O_WRONLY | LFS_O_CREAT | (append ? LFS_O_APPEND : LFS_O_TRUNC));
    if (err) {
        fprintf(stderr, "write: failed to open %s: %d\n", path, err);
        free(buffer);
        free(data);
        return err;
    }

    lfs_ssize_t res = lfs_file_write(&sim->lfs, &file, buffer, size);
    if (res < 0) {
        fprintf(stderr, "write: failed to write %s: %d\n", path, (int)res);
        lfs_file_close(&sim->lfs, &file);
        free(buffer);
        free(data);
        return (int)res;
    }

    err = lfs_file_close(&sim->lfs, &file);
    free(buffer);
    free(data);
    if (err) {
        fprintf(stderr, "write: failed to close %s: %d\n", path, err);
        return err;
    }

    printf("Wrote %d bytes to %s\n", (int)res, path);
    return 0;
}

static int cmd_ops(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 2) {
        fprintf(stderr, "usage: ops <name>\n");
        return -1;
    }

    char rel_tmp1[SIM_PATH_MAX];
    char rel_tmp[SIM_PATH_MAX];
    char rel_final[SIM_PATH_MAX];
    char path_tmp1[SIM_PATH_MAX];
    char path_tmp[SIM_PATH_MAX];
    char path_final[SIM_PATH_MAX];

    snprintf(rel_tmp1, sizeof(rel_tmp1), "%s.PWR.tmp1", argv[1]);
    snprintf(rel_tmp, sizeof(rel_tmp), "%s.PWR.tmp", argv[1]);
    snprintf(rel_final, sizeof(rel_final), "%s.PWR", argv[1]);

    if (resolve_path(sim, rel_tmp1, path_tmp1, sizeof(path_tmp1)) ||
            resolve_path(sim, rel_tmp, path_tmp, sizeof(path_tmp)) ||
            resolve_path(sim, rel_final, path_final, sizeof(path_final))) {
        fprintf(stderr, "ops: invalid name\n");
        return -1;
    }

    if (sim->fdwrite_open) {
        int close_err = lfs_file_close(&sim->lfs, &sim->fdwrite);
        if (close_err) {
            fprintf(stderr, "ops: failed to close previous fdwrite %s: %d\n",
                    sim->fdwrite_path, close_err);
            return close_err;
        }
        sim->fdwrite_open = false;
        sim->fdwrite_path[0] = '\0';
    }

    lfs_file_t file;
    int err = lfs_file_open(&sim->lfs, &file, path_tmp1, LFS_O_CREAT | LFS_O_WRONLY);
    if (err) {
        fprintf(stderr, "ops: failed to create %s: %d\n", path_tmp1, err);
        return err;
    }
    err = lfs_file_close(&sim->lfs, &file);
    if (err) {
        fprintf(stderr, "ops: failed to close %s: %d\n", path_tmp1, err);
        return err;
    }

    err = lfs_file_open(&sim->lfs, &file, path_tmp, LFS_O_CREAT | LFS_O_WRONLY);
    if (err) {
        fprintf(stderr, "ops: failed to create %s: %d\n", path_tmp, err);
        return err;
    }
    err = lfs_file_close(&sim->lfs, &file);
    if (err) {
        fprintf(stderr, "ops: failed to close %s: %d\n", path_tmp, err);
        return err;
    }

    uint8_t *buffer = malloc(14344);
    if (!buffer) {
        fprintf(stderr, "ops: out of memory\n");
        return -1;
    }
    fill_random_bytes(buffer, 14344);

    err = lfs_file_open(&sim->lfs, &sim->fdwrite, path_tmp,
            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err) {
        free(buffer);
        fprintf(stderr, "ops: failed to open %s for write: %d\n", path_tmp, err);
        return err;
    }

    lfs_ssize_t written = lfs_file_write(&sim->lfs, &sim->fdwrite, buffer, 14344);
    free(buffer);
    if (written < 0 || written != 14344) {
        int write_err = (written < 0) ? (int)written : -1;
        lfs_file_close(&sim->lfs, &sim->fdwrite);
        sim->fdwrite_open = false;
        sim->fdwrite_path[0] = '\0';
        fprintf(stderr, "ops: failed to write %s: %d\n", path_tmp, write_err);
        return write_err;
    }

    err = lfs_file_sync(&sim->lfs, &sim->fdwrite);
    if (err) {
        lfs_file_close(&sim->lfs, &sim->fdwrite);
        sim->fdwrite_open = false;
        sim->fdwrite_path[0] = '\0';
        fprintf(stderr, "ops: failed to sync %s: %d\n", path_tmp, err);
        return err;
    }

    sim->fdwrite_open = true;
    strncpy(sim->fdwrite_path, path_tmp, sizeof(sim->fdwrite_path) - 1);
    sim->fdwrite_path[sizeof(sim->fdwrite_path) - 1] = '\0';

    err = lfs_remove(&sim->lfs, path_tmp1);
    if (err) {
        fprintf(stderr, "ops: failed to remove %s: %d\n", path_tmp1, err);
        return err;
    }

    err = lfs_rename(&sim->lfs, path_tmp, path_final);
    if (err) {
        fprintf(stderr, "ops: failed to rename %s -> %s: %d\n",
                path_tmp, path_final, err);
        return err;
    }

    strncpy(sim->fdwrite_path, path_final, sizeof(sim->fdwrite_path) - 1);
    sim->fdwrite_path[sizeof(sim->fdwrite_path) - 1] = '\0';
    printf("ops completed for %s, fdwrite remains open on %s\n", argv[1], path_final);
    return 0;
}

static int cmd_renametest(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 2) {
        fprintf(stderr, "usage: renametest <count>\n");
        return -1;
    }

    lfs_size_t count = 0;
    if (parse_size_arg(argv[1], &count) || count == 0) {
        fprintf(stderr, "renametest: invalid count %s\n", argv[1]);
        return -1;
    }

    int err = run_internal_command(sim, "create aaaaa");
    if (err) {
        return err;
    }

    err = run_internal_command(sim, "create bbbbb");
    if (err) {
        return err;
    }

    for (lfs_size_t i = 0; i < count; i++) {
        err = run_internal_command(sim, "rename aaaaa bbbbb");
        if (err) {
            return err;
        }

        err = run_internal_command(sim, "rename bbbbb aaaaa");
        if (err) {
            return err;
        }
    }

    printf("renametest completed: %"PRIu32" iterations\n", (uint32_t)count);
    return 0;
}

static int cmd_faulttest(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 2) {
        fprintf(stderr, "usage: faulttest <count> [output-file]\n");
        return -1;
    }

    lfs_size_t count = 0;
    if (parse_size_arg(argv[1], &count) || count == 0) {
        fprintf(stderr, "faulttest: invalid count %s\n", argv[1]);
        return -1;
    }

    const char *target_dir = "/lfs0/LOG/PWR";
    FILE *log_file = NULL;
    int saved_stdout = -1;
    int saved_stderr = -1;
    char output_path[SIM_PATH_MAX] = {0};
    int err = 0;
    struct lfs_info info;

    if (argc >= 3) {
        resolve_export_path(argv[2], "faulttest", output_path, sizeof(output_path));
        log_file = fopen(output_path, "w");
        if (!log_file) {
            fprintf(stderr, "faulttest: failed to open output file %s: %s\n",
                    output_path, strerror(errno));
            return -1;
        }

        fflush(stdout);
        fflush(stderr);
        saved_stdout = dup(fileno(stdout));
        saved_stderr = dup(fileno(stderr));
        if (saved_stdout < 0 || saved_stderr < 0 ||
                dup2(fileno(log_file), fileno(stdout)) < 0 ||
                dup2(fileno(log_file), fileno(stderr)) < 0) {
            if (saved_stdout >= 0) {
                close(saved_stdout);
            }
            if (saved_stderr >= 0) {
                close(saved_stderr);
            }
            fclose(log_file);
            fprintf(stderr, "faulttest: failed to redirect output\n");
            return -1;
        }

        printf("faulttest log path: %s\n", output_path);
    }


    if (lfs_stat(&sim->lfs, target_dir, &info) < 0) {
        if (lfs_stat(&sim->lfs, "/lfs0", &info) < 0) {
            err = run_internal_command(sim, "mkdir /lfs0");
            if (err) {
                goto cleanup;
            }
        }
        if (lfs_stat(&sim->lfs, "/lfs0/LOG", &info) < 0) {
            err = run_internal_command(sim, "mkdir /lfs0/LOG");
            if (err) {
                goto cleanup;
            }
        }
        if (lfs_stat(&sim->lfs, target_dir, &info) < 0) {
            err = run_internal_command(sim, "mkdir %s", target_dir);
            if (err) {
                goto cleanup;
            }
        }
    }

    err = run_internal_command(sim, "cd %s", target_dir);
    if (err) {
        goto cleanup;
    }

    for (lfs_size_t iteration = 0; iteration < count; iteration++) {
        if (lfs_stat(&sim->lfs, "/lfs0/LOG/PWR/PWR.IDX", &info) < 0) {
            err = run_internal_command(sim, "create PWR.IDX");
            if (err) {
                goto cleanup;
            }
        }

        size_t pwr_count = 0;
        bool have_any = false;
        uint32_t min_index = 0;
        uint32_t max_index = 0;
        err = scan_pwr_files(sim, sim->current_path,
                &pwr_count, &have_any, &min_index, &max_index);
        if (err) {
            fprintf(stderr, "faulttest: failed to scan %s: %d\n",
                    sim->current_path, err);
            goto cleanup;
        }

        uint32_t next_index = have_any ? (max_index + 1u) : 0u;
        char new_name[32];
        snprintf(new_name, sizeof(new_name), "%08"PRIu32".PWR", next_index);
        g_flash_fault_injection_start = 1;
        err = run_internal_command(sim, "create %s", new_name);
        if (err) {
            goto cleanup;
        }
        g_flash_fault_injection_start = 0;

        int write_count = 10 + (rand() % 11);
        for (int i = 0; i < write_count; i++) {
            int chunk = (3 + (rand() % 7)) * 1024;
            for (int repeat = 0; repeat < 2; repeat++) {
                err = run_internal_command(sim, "write %s %d --append", new_name, chunk);
                if (err) {
                    goto cleanup;
                }

                err = run_internal_command(sim, "write PWR.IDX 988 --append");
                if (err) {
                    goto cleanup;
                }
            }
        }

        err = run_internal_command(sim, "write PWR.IDX %d --append", 800 + (rand() % 201));
        if (err) {
            goto cleanup;
        }
        if (lfs_stat(&sim->lfs, "/lfs0/LOG/PWR/PWR.IDX", &info) == 0 &&
                info.type == LFS_TYPE_REG &&
                info.size > (30u * 1024u)) {
           
            if (lfs_stat(&sim->lfs, "/lfs0/LOG/PWR/PWR.IDX.tmp", &info) == 0) {
                err = run_internal_command(sim, "rm PWR.IDX.tmp");
                if (err) {
                    goto cleanup;
                }
            }

            err = run_internal_command(sim, "cp PWR.IDX PWR.IDX.tmp");
            if (err) {
                goto cleanup;
            }
            g_flash_fault_injection_start = 1;
            err = run_internal_command(sim, "rm PWR.IDX");
            if (err) {
                goto cleanup;
            }
            err = run_internal_command(sim, "rename PWR.IDX.tmp PWR.IDX");
            g_flash_fault_injection_start = 0;
            if (err) {
                goto cleanup;
            }
        }
        g_flash_fault_injection_start = 0;
        err = run_internal_command(sim, "lschk .");
        if (err) {
            goto cleanup;
        }

        lschk_entry_t *entries = NULL;
        size_t entry_count = 0;
        err = collect_lschk_entries(sim, sim->current_path, &entries, &entry_count);
        if (err) {
            fprintf(stderr, "faulttest: lschk scan failed: %d\n", err);
            goto cleanup;
        }

        bool all_real = true;
        for (size_t i = 0; i < entry_count; i++) {
            if (entries[i].status != LSCHK_STATUS_REAL) {
                all_real = false;
                break;
            }
        }
        free(entries);

        if (!all_real) {
            printf("faulttest paused at iteration %"PRIu32
                    ": non-real entries detected\n",
                    (uint32_t)(iteration + 1));
            err = 0;
            goto cleanup;
        }

        err = scan_pwr_files(sim, sim->current_path,
                &pwr_count, &have_any, &min_index, &max_index);
        if (err) {
            fprintf(stderr, "faulttest: failed to rescan %s: %d\n",
                    sim->current_path, err);
            goto cleanup;
        }

        g_flash_fault_injection_start = 0;
        if (pwr_count > 10 && have_any) {
            char oldest_name[32];
            snprintf(oldest_name, sizeof(oldest_name), "%08"PRIu32".PWR", min_index);
            err = run_internal_command(sim, "rm %s", oldest_name);
            if (err) {
                goto cleanup;
            }
        }

        emit_progress_tick(saved_stdout);
    }

    printf("\nfaulttest completed: %"PRIu32" iterations\n", (uint32_t)count);
    err = 0;

cleanup:
    g_flash_fault_injection_start = 0;
    if (log_file) {
        fflush(stdout);
        fflush(stderr);
        dup2(saved_stdout, fileno(stdout));
        dup2(saved_stderr, fileno(stderr));
        close(saved_stdout);
        close(saved_stderr);
        fclose(log_file);
        printf("\nfaulttest output saved to %s\n", output_path);
    }
    return err;
}

static int cmd_test(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 2) {
        fprintf(stderr, "usage: test <count> [output-file]\n");
        return -1;
    }

    lfs_size_t count = 0;
    if (parse_size_arg(argv[1], &count) || count == 0) {
        fprintf(stderr, "test: invalid count %s\n", argv[1]);
        return -1;
    }

    const char *target_dir = "/lfs0/LOG/PWR";
    const int pwr_chunk_sizes[3] = {600, 625, 650};
    FILE *log_file = NULL;
    int saved_stdout = -1;
    int saved_stderr = -1;
    char output_path[SIM_PATH_MAX] = {0};

    if (argc >= 3) {
        resolve_export_path(argv[2], "test", output_path, sizeof(output_path));
        log_file = fopen(output_path, "w");
        if (!log_file) {
            fprintf(stderr, "test: failed to open output file %s: %s\n",
                    output_path, strerror(errno));
            return -1;
        }

        fflush(stdout);
        fflush(stderr);
        saved_stdout = dup(fileno(stdout));
        saved_stderr = dup(fileno(stderr));
        if (saved_stdout < 0 || saved_stderr < 0 ||
                dup2(fileno(log_file), fileno(stdout)) < 0 ||
                dup2(fileno(log_file), fileno(stderr)) < 0) {
            if (saved_stdout >= 0) {
                close(saved_stdout);
            }
            if (saved_stderr >= 0) {
                close(saved_stderr);
            }
            fclose(log_file);
            fprintf(stderr, "test: failed to redirect output\n");
            return -1;
        }

        printf("test log path: %s\n", output_path);
    }

    struct lfs_info info;
    if (lfs_stat(&sim->lfs, target_dir, &info) < 0) {
        if (lfs_stat(&sim->lfs, "/lfs0", &info) < 0) {
            int err = run_internal_command(sim, "mkdir /lfs0");
            if (err) {
                return err;
            }
        }
        if (lfs_stat(&sim->lfs, "/lfs0/LOG", &info) < 0) {
            int err = run_internal_command(sim, "mkdir /lfs0/LOG");
            if (err) {
                return err;
            }
        }
        if (lfs_stat(&sim->lfs, target_dir, &info) < 0) {
            int err = run_internal_command(sim, "mkdir %s", target_dir);
            if (err) {
                return err;
            }
        }
    }

    int err = run_internal_command(sim, "cd %s", target_dir);
    if (err) {
        return err;
    }

    for (lfs_size_t iteration = 0; iteration < count; iteration++) {
        if (lfs_stat(&sim->lfs, "/lfs0/LOG/PWR/PWR.IDX", &info) < 0) {
            err = run_internal_command(sim, "create PWR.IDX");
            if (err) {
                goto cleanup;
            }
        } else {
            err = run_internal_command(sim, "read PWR.IDX");
            if (err) {
                goto cleanup;
            }
        }

        size_t pwr_count = 0;
        bool have_any = false;
        uint32_t min_index = 0;
        uint32_t max_index = 0;
        err = scan_pwr_files(sim, sim->current_path,
                &pwr_count, &have_any, &min_index, &max_index);
        if (err) {
            fprintf(stderr, "test: failed to scan %s: %d\n", sim->current_path, err);
            goto cleanup;
        }

        uint32_t next_index = have_any ? (max_index + 1u) : 0u;
        char new_name[32];
        snprintf(new_name, sizeof(new_name), "%08"PRIu32".PWR", next_index);

        err = run_internal_command(sim, "create %s", new_name);
        if (err) {
            goto cleanup;
        }

        int write_count = 40 + (rand() % 21);
        for (int i = 0; i < write_count; i++) {
            int chunk = pwr_chunk_sizes[rand() % 3];
            err = run_internal_command(sim, "write %s %d --append", new_name, chunk);
            if (err) {
                goto cleanup;
            }
        }

        int idx_append = 800 + (rand() % 201);
        err = run_internal_command(sim, "write PWR.IDX %d --append", idx_append);
        if (err) {
            goto cleanup;
        }

        if (lfs_stat(&sim->lfs, "/lfs0/LOG/PWR/PWR.IDX", &info) == 0 &&
                info.type == LFS_TYPE_REG &&
                info.size > (30u * 1024u)) {
            if (lfs_stat(&sim->lfs, "/lfs0/LOG/PWR/PWR.IDX.tmp", &info) == 0) {
                err = run_internal_command(sim, "rm PWR.IDX.tmp");
                if (err) {
                    goto cleanup;
                }
            }

            err = run_internal_command(sim, "cp PWR.IDX PWR.IDX.tmp");
            if (err) {
                goto cleanup;
            }
            err = run_internal_command(sim, "rm PWR.IDX");
            if (err) {
                goto cleanup;
            }
            err = run_internal_command(sim, "rename PWR.IDX.tmp PWR.IDX");
            if (err) {
                goto cleanup;
            }
        }

        err = run_internal_command(sim, "lschk .");
        if (err) {
            goto cleanup;
        }

        lschk_entry_t *entries = NULL;
        size_t entry_count = 0;
        err = collect_lschk_entries(sim, sim->current_path, &entries, &entry_count);
        if (err) {
            fprintf(stderr, "test: lschk scan failed: %d\n", err);
            goto cleanup;
        }

        bool all_real = true;
        for (size_t i = 0; i < entry_count; i++) {
            if (entries[i].status != LSCHK_STATUS_REAL) {
                all_real = false;
                break;
            }
        }
        free(entries);

        if (!all_real) {
            printf("test paused at iteration %"PRIu32": non-real entries detected\n",
                    (uint32_t)(iteration + 1));
            err = 0;
            goto cleanup;
        }

        err = scan_pwr_files(sim, sim->current_path,
                &pwr_count, &have_any, &min_index, &max_index);
        if (err) {
            fprintf(stderr, "test: failed to scan %s: %d\n", sim->current_path, err);
            goto cleanup;
        }

        if (pwr_count > 5 && have_any) {
            char oldest_name[32];
            snprintf(oldest_name, sizeof(oldest_name), "%08"PRIu32".PWR", min_index);
            err = run_internal_command(sim, "rm %s", oldest_name);
            if (err) {
                goto cleanup;
            }
        }
    }

    printf("test completed: %"PRIu32" iterations\n", (uint32_t)count);
    err = 0;

cleanup:
    if (log_file) {
        fflush(stdout);
        fflush(stderr);
        dup2(saved_stdout, fileno(stdout));
        dup2(saved_stderr, fileno(stderr));
        close(saved_stdout);
        close(saved_stderr);
        fclose(log_file);
        printf("test output saved to %s\n", output_path);
    }
    return err;
}

static int cmd_mkdir(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 2) {
        fprintf(stderr, "usage: mkdir <dir>\n");
        return -1;
    }

    char path[SIM_PATH_MAX];
    if (resolve_path(sim, argv[1], path, sizeof(path))) {
        fprintf(stderr, "mkdir: invalid path\n");
        return -1;
    }

    int err = lfs_mkdir(&sim->lfs, path);
    if (err) {
        fprintf(stderr, "mkdir: %s: %d\n", path, err);
        return err;
    }
    return 0;
}

static int remove_path_recursive(sim_state_t *sim, const char *path) {
    struct lfs_info info;
    int err = lfs_stat(&sim->lfs, path, &info);
    if (err) {
        return err;
    }

    if (info.type == LFS_TYPE_DIR) {
        lfs_dir_t dir;
        err = lfs_dir_open(&sim->lfs, &dir, path);
        if (err) {
            return err;
        }

        struct lfs_info child;
        while ((err = lfs_dir_read(&sim->lfs, &dir, &child)) > 0) {
            if (strcmp(child.name, ".") == 0 || strcmp(child.name, "..") == 0) {
                continue;
            }

            char child_path[SIM_PATH_MAX];
            if (strcmp(path, "/") == 0) {
                snprintf(child_path, sizeof(child_path), "/%s", child.name);
            } else {
                snprintf(child_path, sizeof(child_path), "%s/%s", path, child.name);
            }

            err = remove_path_recursive(sim, child_path);
            if (err) {
                lfs_dir_close(&sim->lfs, &dir);
                return err;
            }
        }

        lfs_dir_close(&sim->lfs, &dir);
        if (err < 0) {
            return err;
        }
    }

    return lfs_remove(&sim->lfs, path);
}

static int cmd_rm(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 2) {
        fprintf(stderr, "usage: rm <path> [--recursive]\n");
        return -1;
    }

    bool recursive = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--recursive") == 0) {
            recursive = true;
        }
    }

    char path[SIM_PATH_MAX];
    if (resolve_path(sim, argv[1], path, sizeof(path))) {
        fprintf(stderr, "rm: invalid path\n");
        return -1;
    }

    int err = recursive ? remove_path_recursive(sim, path) : lfs_remove(&sim->lfs, path);
    if (err) {
        fprintf(stderr, "rm: %s: %d\n", path, err);
        return err;
    }
    return 0;
}

static int cmd_cp(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 3) {
        fprintf(stderr, "usage: cp <src> <dst>\n");
        return -1;
    }

    char src[SIM_PATH_MAX];
    char dst[SIM_PATH_MAX];
    if (resolve_path(sim, argv[1], src, sizeof(src)) ||
            resolve_path(sim, argv[2], dst, sizeof(dst))) {
        fprintf(stderr, "cp: invalid path\n");
        return -1;
    }

    lfs_file_t in;
    lfs_file_t out;
    int err = lfs_file_open(&sim->lfs, &in, src, LFS_O_RDONLY);
    if (err) {
        fprintf(stderr, "cp: failed to open %s: %d\n", src, err);
        return err;
    }

    err = lfs_file_open(&sim->lfs, &out, dst,
            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err) {
        fprintf(stderr, "cp: failed to open %s: %d\n", dst, err);
        lfs_file_close(&sim->lfs, &in);
        return err;
    }

    uint8_t buffer[SIM_READ_CHUNK];
    while (true) {
        lfs_ssize_t res = lfs_file_read(&sim->lfs, &in, buffer, sizeof(buffer));
        if (res < 0) {
            err = (int)res;
            break;
        }
        if (res == 0) {
            break;
        }

        lfs_ssize_t written = lfs_file_write(&sim->lfs, &out, buffer, res);
        if (written < 0 || written != res) {
            err = (written < 0) ? (int)written : -1;
            break;
        }
    }

    lfs_file_close(&sim->lfs, &in);
    lfs_file_close(&sim->lfs, &out);

    if (err) {
        fprintf(stderr, "cp: failed while copying %s -> %s: %d\n", src, dst, err);
        return err;
    }
    return 0;
}

static int cmd_rename(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 3) {
        fprintf(stderr, "usage: rename <src> <dst>\n");
        return -1;
    }

    char src[SIM_PATH_MAX];
    char dst[SIM_PATH_MAX];
    if (resolve_path(sim, argv[1], src, sizeof(src)) ||
            resolve_path(sim, argv[2], dst, sizeof(dst))) {
        fprintf(stderr, "rename: invalid path\n");
        return -1;
    }

    int err = lfs_rename(&sim->lfs, src, dst);
    if (err) {
        fprintf(stderr, "rename: %s -> %s: %d\n", src, dst, err);
        return err;
    }

    return 0;
}

static int cmd_stat(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 2) {
        fprintf(stderr, "usage: stat <path>\n");
        return -1;
    }

    char path[SIM_PATH_MAX];
    if (resolve_path(sim, argv[1], path, sizeof(path))) {
        fprintf(stderr, "stat: invalid path\n");
        return -1;
    }

    struct lfs_info info;
    int err = lfs_stat(&sim->lfs, path, &info);
    if (err) {
        fprintf(stderr, "stat: %s: %d\n", path, err);
        return err;
    }

    printf("path : %s\n", path);
    printf("type : %s\n", (info.type == LFS_TYPE_DIR) ? "DIR" : "FILE");
    printf("size : %"PRIu32"\n", (uint32_t)info.size);
    return 0;
}

static int cmd_mount(sim_state_t *sim, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return sim_mount(sim);
}

static int cmd_umount(sim_state_t *sim, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return sim_unmount(sim);
}

static int cmd_format(sim_state_t *sim, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return sim_format(sim);
}

static int tree_walk(sim_state_t *sim, const char *path, int level, int max_depth,
        tree_stats_t *stats) {
    lfs_dir_t dir;
    int err = lfs_dir_open(&sim->lfs, &dir, path);
    if (err) {
        printf("%*s[CORRUPTED] %s (%d)\n", (level + 1) * 2, "", path, err);
        stats->corrupted++;
        return err;
    }

    struct lfs_info info;
    while ((err = lfs_dir_read(&sim->lfs, &dir, &info)) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }

        char child[SIM_PATH_MAX];
        if (strcmp(path, "/") == 0) {
            snprintf(child, sizeof(child), "/%s", info.name);
        } else {
            snprintf(child, sizeof(child), "%s/%s", path, info.name);
        }

        printf("%*s|- %s%s", (level + 1) * 2, "",
                info.name,
                (info.type == LFS_TYPE_DIR) ? "/" : "");
        if (info.type == LFS_TYPE_REG) {
            printf(" (%"PRIu32" B)", (uint32_t)info.size);
            stats->files++;
            stats->total_bytes += info.size;
        } else {
            stats->dirs++;
        }
        printf("\n");

        if (info.type == LFS_TYPE_DIR &&
                (max_depth < 0 || level < max_depth)) {
            tree_walk(sim, child, level + 1, max_depth, stats);
        }
    }

    lfs_dir_close(&sim->lfs, &dir);
    return (err < 0) ? err : 0;
}

static int cmd_tree(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim)) {
        return -1;
    }

    const char *target = sim->current_path;
    int max_depth = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--depth") == 0) {
            lfs_size_t parsed = 0;
            if (i + 1 >= argc || parse_size_arg(argv[++i], &parsed)) {
                fprintf(stderr, "tree: --depth requires a non-negative value\n");
                return -1;
            }
            max_depth = (int)parsed;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "tree: unknown option %s\n", argv[i]);
            return -1;
        } else if (target == sim->current_path) {
            target = argv[i];
        } else {
            fprintf(stderr, "tree: unexpected argument %s\n", argv[i]);
            return -1;
        }
    }

    char path[SIM_PATH_MAX];
    if (resolve_path(sim, target, path, sizeof(path))) {
        fprintf(stderr, "tree: invalid path\n");
        return -1;
    }

    struct lfs_info info;
    int err = lfs_stat(&sim->lfs, path, &info);
    if (err) {
        fprintf(stderr, "tree: %s: %d\n", path, err);
        return err;
    }
    if (info.type != LFS_TYPE_DIR) {
        fprintf(stderr, "tree: not a directory: %s\n", path);
        return -1;
    }

    tree_stats_t stats = {0};
    printf("%s\n", path);
    err = tree_walk(sim, path, 0, max_depth, &stats);
    printf("%d director%s, %d file%s, %"PRIu64" bytes",
            stats.dirs, (stats.dirs == 1) ? "y" : "ies",
            stats.files, (stats.files == 1) ? "" : "s",
            stats.total_bytes);
    if (stats.corrupted > 0) {
        printf(", %d corrupted director%s",
                stats.corrupted, (stats.corrupted == 1) ? "y" : "ies");
    }
    putchar('\n');
    return err;
}

static int cmd_inspect(sim_state_t *sim, int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: inspect blocks | inspect block <N>\n");
        return -1;
    }

    if (strcmp(argv[1], "block") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: inspect block <N>\n");
            return -1;
        }

        lfs_size_t block_value = 0;
        if (parse_size_arg(argv[2], &block_value)) {
            fprintf(stderr, "inspect block: invalid block number %s\n", argv[2]);
            return -1;
        }
        if (block_value >= sim->storage.block_count) {
            fprintf(stderr, "inspect block: block %"PRIu32" out of range (max %"PRIu32")\n",
                    (uint32_t)block_value, (uint32_t)(sim->storage.block_count - 1));
            return -1;
        }

        uint8_t *buffer = malloc(sim->storage.block_size);
        if (!buffer) {
            fprintf(stderr, "inspect block: out of memory\n");
            return -1;
        }

        int err = read_device_block(sim, (lfs_block_t)block_value, buffer);
        if (err == 0) {
            size_t dump_size = inspect_block_dump_size(buffer, sim->storage.block_size);
            printf("Block %"PRIu32" (offset 0x%"PRIx64", %s)\n",
                    (uint32_t)block_value,
                    (uint64_t)block_value * sim->storage.block_size,
                    buffer_is_erased(buffer, sim->storage.block_size) ? "erased" : "programmed");
            if (dump_size > 0) {
                print_hexdump(buffer, dump_size);
            }
            if (dump_size < sim->storage.block_size) {
                printf("... stopped at first 1024-byte erased run (offset 0x%08"PRIx32")\n",
                        (uint32_t)dump_size);
            }
        }
        free(buffer);
        return err;
    }

    if (strcmp(argv[1], "blocks") == 0) {
        if (ensure_mounted(sim)) {
            return -1;
        }

        uint8_t *used = calloc(sim->storage.block_count, 1);
        if (!used) {
            fprintf(stderr, "inspect blocks: out of memory\n");
            return -1;
        }

        block_usage_map_t map = {
            .used = used,
            .count = sim->storage.block_count,
        };

        int err = lfs_fs_traverse(&sim->lfs, inspect_mark_used_block, &map);
        if (err) {
            fprintf(stderr, "inspect blocks: traverse failed: %d\n", err);
            free(used);
            return err;
        }

        int used_count = 0;
        printf("Block map (%"PRIu32" blocks, %"PRIu32" bytes/block)\n",
                (uint32_t)sim->storage.block_count,
                (uint32_t)sim->storage.block_size);
        for (lfs_size_t i = 0; i < sim->storage.block_count; i++) {
            putchar(used[i] ? '#' : '.');
            if (used[i]) {
                used_count++;
            }
            if ((i + 1) % 64 == 0 || i + 1 == sim->storage.block_count) {
                putchar('\n');
            }
        }
        printf("# = used, . = free\n");
        printf("Used blocks: %d/%"PRIu32" (%.1f%%)\n",
                used_count,
                (uint32_t)sim->storage.block_count,
                sim->storage.block_count ?
                    (100.0 * used_count / sim->storage.block_count) : 0.0);

        free(used);
        return 0;
    }

    fprintf(stderr, "inspect: unknown subcommand %s\n", argv[1]);
    return -1;
}

static int meta_dump_target(sim_state_t *sim, const char *path,
        bool block_only, bool parsed_only, FILE *out) {
    lfs_mdir_t mdir;
    memset(&mdir, 0, sizeof(mdir));

    bool has_entry = false;
    uint16_t entry_id = 0;
    uint8_t entry_type = 0;

    lfs_dir_t dir;
    int err = lfs_dir_open(&sim->lfs, &dir, path);
    if (err == 0) {
        mdir = dir.m;
        entry_id = dir.id;
        entry_type = dir.type;
        has_entry = strcmp(path, "/") != 0;
        lfs_dir_close(&sim->lfs, &dir);
    } else {
        lfs_file_t file;
        err = lfs_file_open(&sim->lfs, &file, path, LFS_O_RDONLY);
        if (err) {
            fprintf(stderr, "meta-dump: failed to open %s: %d\n", path, err);
            return err;
        }
        mdir = file.m;
        entry_id = file.id;
        entry_type = file.type;
        has_entry = true;
        lfs_file_close(&sim->lfs, &file);
    }

    uint8_t *block0 = malloc(sim->storage.block_size);
    uint8_t *block1 = malloc(sim->storage.block_size);
    if (!block0 || !block1) {
        free(block0);
        free(block1);
        fprintf(stderr, "meta-dump: out of memory\n");
        return -1;
    }

    err = read_device_block(sim, mdir.pair[0], block0);
    if (err == 0) {
        err = read_device_block(sim, mdir.pair[1], block1);
    }
    if (err) {
        free(block0);
        free(block1);
        return err;
    }

    uint32_t rev0;
    uint32_t rev1;
    memcpy(&rev0, block0, sizeof(rev0));
    memcpy(&rev1, block1, sizeof(rev1));
    rev0 = lfs_fromle32(rev0);
    rev1 = lfs_fromle32(rev1);

    int active_index = 0;
    if (rev1 == mdir.rev && rev0 != mdir.rev) {
        active_index = 1;
    } else if (rev0 != mdir.rev && rev1 != mdir.rev &&
            lfs_scmp(rev1, rev0) > 0) {
        active_index = 1;
    }

    fprintf(out, "Metadata dump for %s\n", path);
    fprintf(out, "  pair     : {0x%"PRIx32", 0x%"PRIx32"}\n",
            mdir.pair[0], mdir.pair[1]);
    fprintf(out, "  active   : 0x%"PRIx32" (rev=%"PRIu32")\n",
            mdir.pair[active_index], active_index == 0 ? rev0 : rev1);
    fprintf(out, "  mirror   : 0x%"PRIx32" (rev=%"PRIu32")\n",
            mdir.pair[1 - active_index], active_index == 0 ? rev1 : rev0);
    fprintf(out, "  dir.rev  : %"PRIu32"\n", mdir.rev);
    if (has_entry) {
        fprintf(out, "  entry    : id=%u type=%s\n", entry_id,
                entry_type == LFS_TYPE_DIR ? "DIR" : "FILE");
    }
    fputc('\n', out);

    const uint8_t *blocks[2] = {block0, block1};
    const uint32_t revs[2] = {rev0, rev1};
    for (int bi = 0; bi < 2; bi++) {
        const uint8_t *block = blocks[bi];
        fprintf(out, "Block 0x%"PRIx32" [%s] revision=%"PRIu32"\n",
                mdir.pair[bi], (bi == active_index) ? "ACTIVE" : "MIRROR", revs[bi]);

        size_t dump_size = 0;
        if (!buffer_is_erased(block, sim->storage.block_size)) {
            dump_size = effective_block_dump_size(block, sim->storage.block_size);
        }

        if (block_only) {
            if (buffer_is_erased(block, sim->storage.block_size)) {
                fprint_erased_block_preview(out, sim->storage.block_size);
            } else {
                fprint_hexdump_with_base(out, block, dump_size, 0);
                if (dump_size < sim->storage.block_size) {
                    fprintf(out, "... trailing erased area omitted (%zu bytes shown of %"PRIu32")\n",
                            dump_size, (uint32_t)sim->storage.block_size);
                }
            }
            fputc('\n', out);
        }

        if (!block_only) {
            uint32_t prev_tag = 0xffffffffu;
            uint32_t crc = lfs_crc(0xffffffffu, block, 4);
            lfs_off_t off = 4;
            int commit_index = 0;
            int tag_index = 0;

            while (off + 4 <= sim->storage.block_size) {
                if (buffer_is_erased(block + off, sim->storage.block_size - off)) {
                    break;
                }

                uint32_t raw_tag;
                memcpy(&raw_tag, block + off, sizeof(raw_tag));
                raw_tag = lfs_frombe32(raw_tag);

                uint32_t decoded_tag = (prev_tag ^ raw_tag) & 0x7fffffffu;
                uint16_t type = (decoded_tag & 0x7ff00000u) >> 20;
                uint16_t id = (decoded_tag & 0x000ffc00u) >> 10;
                uint16_t size = decoded_tag & 0x3ffu;
                lfs_size_t dsize = 4 + ((size != 0x3ffu) ? size : 0);

                if (off + dsize > sim->storage.block_size) {
                    fprintf(out, "  [TRUNCATED] off=0x%04"PRIx32" decoded=0x%08"PRIx32"\n",
                            (uint32_t)off, decoded_tag);
                    break;
                }

                const uint8_t *data = block + off + 4;
                uint32_t crc_after = meta_type_is_crc(type)
                        ? lfs_crc(crc, block + off, 8)
                        : lfs_crc(crc, block + off, dsize);

                if (tag_index == 0) {
                    fprintf(out, "  commit #%d (offset 0x%04"PRIx32")\n",
                            commit_index, (uint32_t)off);
                }

                if (!parsed_only) {
                    fprint_hexdump_with_base(out, block + off, dsize, (uint32_t)off);
                }
                fprintf(out, "      -> [tag %d] raw=0x%08"PRIx32
                        " decoded=0x%08"PRIx32" type=%s id=%u size=%u",
                        tag_index, raw_tag, decoded_tag,
                        meta_tag_type_name(type), id, size);
                fmeta_print_tag_data(out, type, data, size == 0x3ffu ? 0 : size);
                fputc('\n', out);

                off += dsize;
                tag_index++;

                if (meta_type_is_crc(type)) {
                    crc = 0;
                    prev_tag = decoded_tag ^ ((type & 1u) ? 0x80000000u : 0u);
                    commit_index++;
                    tag_index = 0;
                } else {
                    crc = crc_after;
                    prev_tag = decoded_tag;
                }
            }
            if (!parsed_only && dump_size > 0 && dump_size < sim->storage.block_size) {
                fprintf(out, "  ... trailing erased area omitted (%zu bytes shown of %"PRIu32")\n",
                        dump_size, (uint32_t)sim->storage.block_size);
            }
            fputc('\n', out);
        }
    }

    free(block0);
    free(block1);
    return 0;
}

static int cmd_meta_dump(sim_state_t *sim, int argc, char **argv) {
    if (ensure_mounted(sim) || argc < 2) {
        fprintf(stderr,
                "usage: meta-dump <path> [--block-only] [--parsed-only] [--export [file.txt]]\n");
        return -1;
    }

    const char *target = NULL;
    const char *export_name = NULL;
    bool block_only = false;
    bool parsed_only = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--block-only") == 0) {
            block_only = true;
        } else if (strcmp(argv[i], "--parsed-only") == 0) {
            parsed_only = true;
        } else if (strcmp(argv[i], "--export") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                export_name = argv[++i];
            } else {
                export_name = "";
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "meta-dump: unknown option %s\n", argv[i]);
            return -1;
        } else if (target == NULL) {
            target = argv[i];
        } else {
            fprintf(stderr, "meta-dump: unexpected argument %s\n", argv[i]);
            return -1;
        }
    }

    if (target == NULL) {
        fprintf(stderr, "meta-dump: missing path\n");
        return -1;
    }
    if (block_only && parsed_only) {
        fprintf(stderr, "meta-dump: choose only one of --block-only or --parsed-only\n");
        return -1;
    }

    char path[SIM_PATH_MAX];
    if (resolve_path(sim, target, path, sizeof(path))) {
        fprintf(stderr, "meta-dump: invalid path\n");
        return -1;
    }

    if (export_name != NULL) {
        char export_path[SIM_PATH_MAX];
        resolve_export_path(export_name, path, export_path, sizeof(export_path));

        FILE *f = fopen(export_path, "w");
        if (!f) {
            fprintf(stderr, "meta-dump: failed to open export file %s: %s\n",
                    export_path, strerror(errno));
            return -1;
        }

        int err = meta_dump_target(sim, path, block_only, parsed_only, f);
        fclose(f);
        if (err) {
            return err;
        }

        printf("meta-dump exported to %s\n", export_path);
        return 0;
    }

    return meta_dump_target(sim, path, block_only, parsed_only, stdout);
}

static int dispatch_command(sim_state_t *sim, int argc, char **argv, bool *should_exit) {
    *should_exit = false;
    if (argc == 0) {
        return 0;
    }

    if (strcmp(argv[0], "help") == 0) {
        return cmd_help(sim, argc, argv);
    }
    if (strcmp(argv[0], "ls") == 0) {
        return cmd_ls(sim, argc, argv);
    }
    if (strcmp(argv[0], "lschk") == 0) {
        return cmd_lschk(sim, argc, argv);
    }
    if (strcmp(argv[0], "lsrepair") == 0) {
        return cmd_lsrepair(sim, argc, argv);
    }
    if (strcmp(argv[0], "cd") == 0) {
        return cmd_cd(sim, argc, argv);
    }
    if (strcmp(argv[0], "pwd") == 0) {
        return cmd_pwd(sim, argc, argv);
    }
    if (strcmp(argv[0], "read") == 0) {
        return cmd_read(sim, argc, argv);
    }
    if (strcmp(argv[0], "cat") == 0) {
        return cmd_cat(sim, argc, argv);
    }
    if (strcmp(argv[0], "hexdump") == 0) {
        return cmd_hexdump(sim, argc, argv);
    }
    if (strcmp(argv[0], "create") == 0) {
        return cmd_create_file(sim, argc, argv);
    }
    if (strcmp(argv[0], "write") == 0) {
        return cmd_write(sim, argc, argv);
    }
    if (strcmp(argv[0], "ops") == 0) {
        return cmd_ops(sim, argc, argv);
    }
    if (strcmp(argv[0], "renametest") == 0) {
        return cmd_renametest(sim, argc, argv);
    }
    if (strcmp(argv[0], "faulttest") == 0) {
        return cmd_faulttest(sim, argc, argv);
    }
    if (strcmp(argv[0], "mkdir") == 0) {
        return cmd_mkdir(sim, argc, argv);
    }
    if (strcmp(argv[0], "rm") == 0) {
        return cmd_rm(sim, argc, argv);
    }
    if (strcmp(argv[0], "cp") == 0) {
        return cmd_cp(sim, argc, argv);
    }
    if (strcmp(argv[0], "rename") == 0) {
        return cmd_rename(sim, argc, argv);
    }
    if (strcmp(argv[0], "stat") == 0) {
        return cmd_stat(sim, argc, argv);
    }
    if (strcmp(argv[0], "test") == 0) {
        return cmd_test(sim, argc, argv);
    }
    if (strcmp(argv[0], "mount") == 0) {
        return cmd_mount(sim, argc, argv);
    }
    if (strcmp(argv[0], "umount") == 0) {
        return cmd_umount(sim, argc, argv);
    }
    if (strcmp(argv[0], "format") == 0) {
        return cmd_format(sim, argc, argv);
    }
    if (strcmp(argv[0], "tree") == 0) {
        return cmd_tree(sim, argc, argv);
    }
    if (strcmp(argv[0], "meta-dump") == 0) {
        return cmd_meta_dump(sim, argc, argv);
    }
    if (strcmp(argv[0], "inspect") == 0) {
        return cmd_inspect(sim, argc, argv);
    }
    if (strcmp(argv[0], "quit") == 0 || strcmp(argv[0], "exit") == 0) {
        *should_exit = true;
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", argv[0]);
    return -1;
}

static int run_shell(sim_state_t *sim) {
    char line[SIM_LINE_MAX];
    char *argv[SIM_ARGV_MAX];

    printf("Entering littlefs shell. Type 'help' for commands.\n");
    while (true) {
        printf("lfs> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            putchar('\n');
            break;
        }

        char *trimmed = trim_whitespace(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            continue;
        }

        int argc = split_command(trimmed, argv, SIM_ARGV_MAX);
        bool should_exit = false;
        int err = dispatch_command(sim, argc, argv, &should_exit);
        if (err) {
            printf("[FAIL] %d\n", err);
        }
        if (should_exit) {
            break;
        }
    }

    return 0;
}

static int run_script(sim_state_t *sim, const char *script_path, bool stop_on_error) {
    FILE *f = fopen(script_path, "r");
    if (!f) {
        fprintf(stderr, "Failed to open script %s: %s\n",
                script_path, strerror(errno));
        return -1;
    }

    char line[SIM_LINE_MAX];
    int line_no = 0;
    int total = 0;
    int passed = 0;
    int failed = 0;

    while (fgets(line, sizeof(line), f)) {
        line_no++;
        char *trimmed = trim_whitespace(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            continue;
        }

        char line_copy[SIM_LINE_MAX];
        strncpy(line_copy, trimmed, sizeof(line_copy)-1);
        line_copy[sizeof(line_copy)-1] = '\0';

        char *argv[SIM_ARGV_MAX];
        int argc = split_command(trimmed, argv, SIM_ARGV_MAX);
        bool should_exit = false;
        total++;

        printf("[%d] %s\n", line_no, line_copy);
        int err = dispatch_command(sim, argc, argv, &should_exit);
        if (err) {
            failed++;
            printf("[FAIL] line %d -> %d\n", line_no, err);
            if (stop_on_error) {
                break;
            }
        } else {
            passed++;
        }

        if (should_exit) {
            break;
        }
    }

    fclose(f);

    printf("=== Script Summary ===\n");
    printf("Total commands : %d\n", total);
    printf("Passed         : %d\n", passed);
    printf("Failed         : %d\n", failed);

    return failed ? -1 : 0;
}
