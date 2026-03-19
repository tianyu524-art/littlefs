/*
 * littlefs Windows Simulator
 * 
 * A command-line tool to simulate littlefs file system operations
 * Supports mounting image files, creating files, and performing read/write/rename operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <stdbool.h>
#include <windows.h>
#include <direct.h>
#include <unistd.h>  // For access function in MSYS2
#define access _access
#define F_OK 0
#define mkdir _mkdir
#else
#include <unistd.h>
#include <sys/stat.h>
#include <getopt.h>
#include <stdbool.h>
#endif

// For getopt which might not be available on Windows by default
#ifndef _WIN32
#include <getopt.h>
#endif

#ifdef _WIN32
#include <getopt.h>  // If available in MSYS2 environment
#include <sys/stat.h>
#endif

#include "lfs.h"
#include "bd/lfs_filebd.h"

// Default configuration - make them const to ensure compile-time initialization
static const int read_size = 16;
static const int prog_size = 16;
static const int block_size = 512;
static const int block_count = 1024;  // 512KB - back to original
static const int cache_size = 512;   // Use larger cache size for better consistency

// Static buffers for littlefs - use larger size to avoid potential issues
static uint8_t read_buffer[512] = {0};  // Size should match block_size or prog_size
static uint8_t prog_buffer[512] = {0};  // Size should match block_size or prog_size  
static uint32_t lookahead_buffer[512/sizeof(uint32_t)] = {0};  // For lookahead (enough for 512*8 bits)

// Current working directory support  
static char current_path[1024] = "/";  // Current directory path (increased size)

// Block device context - initialized at runtime
static struct lfs_filebd bd = {0};
static struct lfs_config cfg = {0};
static lfs_t lfs = {0};
static char *image_path = NULL;
static bool mounted = false;

// Function declarations
static int init_lfs_config(const char *img_path);
static int mount_filesystem();
static int unmount_filesystem();
static int format_filesystem();
static void print_help();
static int cmd_ls();
static int cmd_create(const char *path);
static int cmd_write(const char *path, const char *data);
static int cmd_read(const char *path);
static int cmd_rm(const char *path);
static int cmd_mkdir(const char *path);
static int cmd_rename(const char *old_path, const char *new_path);
static int cmd_cd(const char *path);
static int cmd_pwd();

int main(int argc, char *argv[]) {
    int c;
    bool format_flag = false;
    char *cmd = NULL;
    char *arg1 = NULL;
    char *arg2 = NULL;
    
    while ((c = getopt(argc, argv, "hfi:c:w:r:m:d:n:")) != -1) {
        switch (c) {
            case 'h':
                print_help();
                return 0;
            case 'f':
                format_flag = true;
                break;
            case 'i':
                image_path = optarg;
                break;
            case 'c':
                cmd = "create";
                arg1 = optarg;
                break;
            case 'w':
                cmd = "write";
                arg1 = optarg;
                // Extract path and data: path=data
                {
                    char *eq_w = strchr(optarg, '=');
                    if (eq_w) {
                        *eq_w = '\0';
                        arg1 = optarg;
                        arg2 = eq_w + 1;
                    }
                }
                break;
            case 'r':
                cmd = "read";
                arg1 = optarg;
                break;
            case 'm':
                cmd = "mkdir";
                arg1 = optarg;
                break;
            case 'd':
                cmd = "rm";
                arg1 = optarg;
                break;
            case 'n':
                cmd = "rename";
                arg1 = optarg;
                // Extract old and new path: old=new
                {
                    char *eq_n = strchr(optarg, '=');
                    if (eq_n) {
                        *eq_n = '\0';
                        arg1 = optarg;
                        arg2 = eq_n + 1;
                    }
                }
                break;
            default:
                fprintf(stderr, "Unknown option: %c\n", c);
                print_help();
                return 1;
        }
    }

    if (!image_path) {
        fprintf(stderr, "Error: Image path is required (-i option)\n");
        print_help();
        return 1;
    }

    printf("littlefs Windows Simulator\n");
    printf("Image: %s\n", image_path);
    
    // Initialize the configuration
    if (init_lfs_config(image_path) != 0) {
        fprintf(stderr, "Failed to initialize littlefs configuration\n");
        return 1;
    }
    
    // Handle formatting flag first
    if (format_flag) {
        if (format_filesystem() != 0) {
            fprintf(stderr, "Failed to format filesystem\n");
            return 1;
        }
        if (mount_filesystem() != 0) {
            fprintf(stderr, "Failed to mount filesystem after format\n");
            return 1;
        }
    } else {
        // Mount the filesystem
        if (mount_filesystem() != 0) {
            printf("Mount failed. Attempting to format...\n");
            if (format_filesystem() != 0) {
                fprintf(stderr, "Failed to format filesystem\n");
                return 1;
            }
            if (mount_filesystem() != 0) {
                fprintf(stderr, "Failed to mount filesystem after format\n");
                return 1;
            }
        }
    }
    
    // Execute command if provided
    if (cmd) {
        if (strcmp(cmd, "create") == 0) {
            cmd_create(arg1);
        } else if (strcmp(cmd, "write") == 0) {
            cmd_write(arg1, arg2);
        } else if (strcmp(cmd, "read") == 0) {
            cmd_read(arg1);
        } else if (strcmp(cmd, "rm") == 0) {
            cmd_rm(arg1);
        } else if (strcmp(cmd, "mkdir") == 0) {
            cmd_mkdir(arg1);
        } else if (strcmp(cmd, "rename") == 0) {
            cmd_rename(arg1, arg2);
        } else if (strcmp(cmd, "ls") == 0) {
            cmd_ls();
        }
    } else {
        // Interactive mode
        printf("Filesystem mounted. Enter commands (ls, create, read, write, mkdir, rm, rename, quit):\n");
        char input[256];
        while (fgets(input, sizeof(input), stdin)) {
            // Remove newline
            input[strcspn(input, "\n")] = 0;
            
            if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
                break;
            } else if (strcmp(input, "ls") == 0) {
                cmd_ls();
            } else if (strncmp(input, "create ", 7) == 0) {
                cmd_create(input + 7);
            } else if (strncmp(input, "mkdir ", 6) == 0) {
                cmd_mkdir(input + 6);
            } else if (strncmp(input, "rm ", 3) == 0) {
                cmd_rm(input + 3);
            } else if (strncmp(input, "read ", 5) == 0) {
                cmd_read(input + 5);
            } else if (strncmp(input, "write ", 6) == 0) {
                // Parse write command: write path=data
                char *path = input + 6;
                char *eq = strchr(path, '=');
                if (eq) {
                    *eq = '\0';
                    cmd_write(path, eq + 1);
                } else {
                    printf("Usage: write path=data\n");
                }
            } else if (strncmp(input, "rename ", 7) == 0) {
                // Parse rename command: rename old_path=new_path
                char *path = input + 7;
                char *eq = strchr(path, '=');
                if (eq) {
                    *eq = '\0';
                    cmd_rename(path, eq + 1);
                } else {
                    printf("Usage: rename old_path=new_path\n");
                }
            } else if (strncmp(input, "cd ", 3) == 0) {
                cmd_cd(input + 3);
            } else if (strcmp(input, "pwd") == 0) {
                cmd_pwd();
            } else {
                printf("Unknown command. Available: ls, create, read, write, mkdir, rm, rename, cd, pwd, quit\n");
            }
        }
    }
    
    unmount_filesystem();
    return 0;
}

static int init_lfs_config(const char *img_path) {
    // Initialize the lfs_config struct completely before use
    memset(&cfg, 0, sizeof(cfg));  // Clear entire config struct
    
    // Initialize configuration - make sure context and function pointers are set BEFORE calling lfs_filebd_create
    cfg.context = &bd;
    cfg.read  = lfs_filebd_read;
    cfg.prog  = lfs_filebd_prog;
    cfg.erase = lfs_filebd_erase;
    cfg.sync  = lfs_filebd_sync;

    // Initialize block device config
    struct lfs_filebd_config bd_cfg = {
        .read_size = read_size,
        .prog_size = prog_size,
        .erase_size = block_size,
        .erase_count = block_count
    };
    
    // Create the block device - this should now work since cfg.context is properly set
    int err = lfs_filebd_create(&cfg, img_path, &bd_cfg);
    if (err) {
        fprintf(stderr, "Failed to create block device: %d\n", err);
        return err;
    }
    
    // Set custom attributes for the block device
    cfg.read_size = read_size;
    cfg.prog_size = prog_size;
    cfg.block_size = block_size;
    cfg.block_count = block_count;
    cfg.cache_size = cache_size;  // Use the defined cache_size
    cfg.lookahead_size = 16;
    cfg.block_cycles = 500;  // Wear leveling cycles
    
    // Optional static buffers
    cfg.read_buffer = read_buffer;
    cfg.prog_buffer = prog_buffer;
    cfg.lookahead_buffer = lookahead_buffer;
    
    // Ensure proper initialization for all config fields to avoid undefined behavior
    cfg.name_max = LFS_NAME_MAX;  // Use default name max
    cfg.file_max = LFS_FILE_MAX;  // Use default file max
    cfg.attr_max = LFS_ATTR_MAX;  // Use default attr max
    cfg.metadata_max = block_size;  // Set metadata max to block size
    
    // Increase lookahead size for better metadata handling - must be multiple of 8 and match buffer
    cfg.lookahead_size = 64;  // Use 64 bytes for lookahead (enough for 512 bits)
    
    return 0;
}

static int mount_filesystem() {
    // Make sure config is fully initialized before mounting
    if (cfg.read == NULL || cfg.prog == NULL || cfg.erase == NULL || cfg.sync == NULL) {
        fprintf(stderr, "Error: Config not properly initialized\n");
        return -1;
    }
    int err = lfs_mount(&lfs, &cfg);
    if (err) {
        printf("Mount failed with error: %d\n", err);
        return err;
    }
    mounted = true;
    printf("Filesystem mounted successfully\n");
    return 0;
}

static int unmount_filesystem() {
    if (mounted) {
        // First sync the filesystem to ensure all changes are written
        // Note: There's no direct lfs_sync function, but proper close operations should sync
        
        int err = lfs_unmount(&lfs);
        if (err) {
            printf("Warning: lfs_unmount returned %d\n", err);
        }
        mounted = false;
        printf("Filesystem unmounted\n");
    }
    return 0;
}

static int format_filesystem() {
    unmount_filesystem();
    int err = lfs_format(&lfs, &cfg);
    if (err) {
        fprintf(stderr, "Format failed: %d\n", err);
        return err;
    }
    printf("Filesystem formatted successfully\n");
    return 0;
}

static void print_help() {
    printf("littlefs Windows Simulator\n");
    printf("Usage: littlefs_simulator [OPTIONS]\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h              Show this help\n");
    printf("  -f              Format filesystem\n");
    printf("  -i <image>      Image file path\n");
    printf("  -c <path>       Create file\n");
    printf("  -w <path=data>  Write data to file\n");
    printf("  -r <path>       Read file\n");
    printf("  -m <path>       Create directory\n");
    printf("  -d <path>       Delete file/directory\n");
    printf("  -n <old=new>    Rename file/directory\n");
    printf("\n");
    printf("Interactive commands:\n");
    printf("  ls              List directory contents\n");
    printf("  create <path>   Create a file\n");
    printf("  mkdir <path>    Create a directory\n");
    printf("  read <path>     Read a file\n");
    printf("  write <path=data> Write data to file\n");
    printf("  rename <old=new> Rename file/directory\n");
    printf("  cd <path>       Change directory\n");
    printf("  pwd             Print working directory\n");
    printf("  rm <path>       Remove file/directory\n");
    printf("  quit            Exit simulator\n");
}

static int cmd_ls() {
    if (!mounted) {
        printf("Filesystem not mounted\n");
        return -1;
    }
    
    lfs_dir_t dir;
    int err = lfs_dir_open(&lfs, &dir, current_path);
    if (err) {
        printf("Failed to open directory '%s': %d\n", current_path, err);
        return err;
    }
    
    struct lfs_info info;
    printf("Contents of directory '%s':\n", current_path);
    printf("ID\tType\tSize\tName\n");
    printf("--\t----\t----\t----\n");
    
    int id = 0;  // Simple incrementing ID for display purposes
    
    while (true) {
        int res = lfs_dir_read(&lfs, &dir, &info);
        if (res <= 0) {
            break;
        }
        
        // Skip special directory entries (like "." and "..")
        if (info.name[0] == '.' && (info.name[1] == 0 || (info.name[1] == '.' && info.name[2] == 0))) {
            continue;  // Skip . and .. entries
        }
        
        char type = '?';
        if (info.type == LFS_TYPE_REG) {
            type = 'F';  // File
        } else if (info.type == LFS_TYPE_DIR) {
            type = 'D';  // Directory
        }
        
        printf("%d\t%c\t%u\t%s\n", id, type, (unsigned int)info.size, info.name);
        id++;
    }
    
    lfs_dir_close(&lfs, &dir);
    return 0;
}

static int cmd_create(const char *path) {
    if (!mounted) {
        printf("Filesystem not mounted\n");
        return -1;
    }
    
    // Handle relative path by prepending current path if not in root directory
    char full_path[1024];
    if (path[0] == '/') {
        // Absolute path, use as-is
        strncpy(full_path, path, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    } else {
        // Relative path, prepend current path
        if (strcmp(current_path, "/") == 0) {
            // In root directory, just add the path
            snprintf(full_path, sizeof(full_path) - 1, "/%s", path);
            full_path[sizeof(full_path) - 1] = '\0';
        } else {
            // In subdirectory, construct full path
            int len = snprintf(full_path, sizeof(full_path) - 1, "%s/%s", current_path, path);
            if (len >= sizeof(full_path) - 1) {
                printf("Path too long: %s\n", full_path);
                return -1;
            }
        }
    }
    
    lfs_file_t file;
    int err = lfs_file_open(&lfs, &file, full_path, LFS_O_CREAT | LFS_O_WRONLY);
    if (err) {
        printf("Failed to create file '%s': %d\n", full_path, err);
        return err;
    }
    
    err = lfs_file_close(&lfs, &file);
    if (err) {
        printf("Failed to close file '%s': %d\n", full_path, err);
        return err;
    }
    
    printf("Created file: %s\n", full_path);
    return 0;
}

static int cmd_write(const char *path, const char *data) {
    if (!mounted) {
        printf("Filesystem not mounted\n");
        return -1;
    }
    
    if (!data) {
        printf("No data provided for write operation\n");
        return -1;
    }
    
    // Handle relative path by prepending current path if not in root directory
    char full_path[1024];
    if (path[0] == '/') {
        // Absolute path, use as-is
        strncpy(full_path, path, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    } else {
        // Relative path, prepend current path
        if (strcmp(current_path, "/") == 0) {
            // In root directory, just add the path
            snprintf(full_path, sizeof(full_path) - 1, "/%s", path);
            full_path[sizeof(full_path) - 1] = '\0';
        } else {
            // In subdirectory, construct full path
            int len = snprintf(full_path, sizeof(full_path) - 1, "%s/%s", current_path, path);
            if (len >= sizeof(full_path) - 1) {
                printf("Path too long: %s\n", full_path);
                return -1;
            }
        }
    }
    
    lfs_file_t file;
    int err = lfs_file_open(&lfs, &file, full_path, LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC);
    if (err) {
        printf("Failed to open file '%s' for writing: %d\n", full_path, err);
        return err;
    }
    
    lfs_size_t size = strlen(data);
    lfs_ssize_t res = lfs_file_write(&lfs, &file, data, size);
    if (res < 0) {
        printf("Failed to write to file '%s': %d\n", full_path, (int)res);
        lfs_file_close(&lfs, &file);
        return res;
    }
    
    err = lfs_file_close(&lfs, &file);
    if (err) {
        printf("Failed to close file '%s': %d\n", full_path, err);
        return err;
    }
    
    printf("Wrote %d bytes to file: %s\n", (int)res, full_path);
    return 0;
}

static int cmd_read(const char *path) {
    if (!mounted) {
        printf("Filesystem not mounted\n");
        return -1;
    }
    
    // Handle relative path by prepending current path if not in root directory
    char full_path[1024];
    if (path[0] == '/') {
        // Absolute path, use as-is
        strncpy(full_path, path, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    } else {
        // Relative path, prepend current path
        if (strcmp(current_path, "/") == 0) {
            // In root directory, just add the path
            snprintf(full_path, sizeof(full_path) - 1, "/%s", path);
            full_path[sizeof(full_path) - 1] = '\0';
        } else {
            // In subdirectory, construct full path
            int len = snprintf(full_path, sizeof(full_path) - 1, "%s/%s", current_path, path);
            if (len >= sizeof(full_path) - 1) {
                printf("Path too long: %s\n", full_path);
                return -1;
            }
        }
    }
    
    lfs_file_t file;
    int err = lfs_file_open(&lfs, &file, full_path, LFS_O_RDONLY);
    if (err) {
        printf("Failed to open file '%s' for reading: %d\n", full_path, err);
        return err;
    }
    
    struct lfs_info info;
    err = lfs_stat(&lfs, full_path, &info);
    if (err) {
        printf("Failed to get file info: %d\n", err);
        lfs_file_close(&lfs, &file);
        return err;
    }
    
    if (info.type != LFS_TYPE_REG) {
        printf("'%s' is not a regular file\n", full_path);
        lfs_file_close(&lfs, &file);
        return -1;
    }
    
    // Ensure buffer size is properly aligned for block device operations
    char *buffer = malloc(info.size + 1);
    if (!buffer) {
        printf("Failed to allocate memory for reading\n");
        lfs_file_close(&lfs, &file);
        return -1;
    }
    
    lfs_ssize_t res = lfs_file_read(&lfs, &file, buffer, info.size);
    if (res < 0) {
        printf("Failed to read file '%s': %d\n", full_path, (int)res);
        free(buffer);
        lfs_file_close(&lfs, &file);
        return res;
    }
    
    buffer[res] = '\0';
    printf("Contents of %s (%d bytes):\n", full_path, (int)res);
    printf("%s", buffer);
    if (res > 0 && buffer[res-1] != '\n') {
        printf("\n");  // Add newline if content doesn't end with one
    }
    
    free(buffer);
    err = lfs_file_close(&lfs, &file);
    if (err) {
        printf("Failed to close file '%s': %d\n", full_path, err);
        return err;
    }
    
    return 0;
}

static int cmd_rm(const char *path) {
    if (!mounted) {
        printf("Filesystem not mounted\n");
        return -1;
    }
    
    // Handle relative path by prepending current path if not in root directory
    char full_path[1024];
    if (path[0] == '/') {
        // Absolute path, use as-is
        strncpy(full_path, path, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    } else {
        // Relative path, prepend current path
        if (strcmp(current_path, "/") == 0) {
            // In root directory, just add the path
            snprintf(full_path, sizeof(full_path) - 1, "/%s", path);
            full_path[sizeof(full_path) - 1] = '\0';
        } else {
            // In subdirectory, construct full path
            int len = snprintf(full_path, sizeof(full_path) - 1, "%s/%s", current_path, path);
            if (len >= sizeof(full_path) - 1) {
                printf("Path too long: %s\n", full_path);
                return -1;
            }
        }
    }
    
    int err = lfs_remove(&lfs, full_path);
    if (err) {
        printf("Failed to remove '%s': %d\n", full_path, err);
        return err;
    }
    
    printf("Removed: %s\n", full_path);
    return 0;
}

static int cmd_mkdir(const char *path) {
    if (!mounted) {
        printf("Filesystem not mounted\n");
        return -1;
    }
    
    // Handle relative path by prepending current path if not in root directory
    char full_path[1024];
    if (path[0] == '/') {
        // Absolute path, use as-is
        strncpy(full_path, path, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    } else {
        // Relative path, prepend current path
        if (strcmp(current_path, "/") == 0) {
            // In root directory, just add the path
            snprintf(full_path, sizeof(full_path) - 1, "/%s", path);
            full_path[sizeof(full_path) - 1] = '\0';
        } else {
            // In subdirectory, construct full path
            int len = snprintf(full_path, sizeof(full_path) - 1, "%s/%s", current_path, path);
            if (len >= sizeof(full_path) - 1) {
                printf("Path too long: %s\n", full_path);
                return -1;
            }
        }
    }
    
    int err = lfs_mkdir(&lfs, full_path);
    if (err) {
        printf("Failed to create directory '%s': %d\n", full_path, err);
        return err;
    }
    
    printf("Created directory: %s\n", full_path);
    return 0;
}

static int cmd_rename(const char *old_path, const char *new_path) {
    if (!mounted) {
        printf("Filesystem not mounted\n");
        return -1;
    }
    
    if (!old_path || !new_path) {
        printf("Old path and new path must be provided\n");
        return -1;
    }
    
    // Handle relative paths by prepending current path if not in root directory
    char full_old_path[1024];
    char full_new_path[1024];
    
    // Process old_path
    if (old_path[0] == '/') {
        // Absolute path, use as-is
        strncpy(full_old_path, old_path, sizeof(full_old_path) - 1);
        full_old_path[sizeof(full_old_path) - 1] = '\0';
    } else {
        // Relative path, prepend current path
        if (strcmp(current_path, "/") == 0) {
            // In root directory, just add the path
            snprintf(full_old_path, sizeof(full_old_path) - 1, "/%s", old_path);
            full_old_path[sizeof(full_old_path) - 1] = '\0';
        } else {
            // In subdirectory, construct full path
            int len = snprintf(full_old_path, sizeof(full_old_path) - 1, "%s/%s", current_path, old_path);
            if (len >= sizeof(full_old_path) - 1) {
                printf("Path too long: %s\n", full_old_path);
                return -1;
            }
        }
    }
    
    // Process new_path
    if (new_path[0] == '/') {
        // Absolute path, use as-is
        strncpy(full_new_path, new_path, sizeof(full_new_path) - 1);
        full_new_path[sizeof(full_new_path) - 1] = '\0';
    } else {
        // Relative path, prepend current path
        if (strcmp(current_path, "/") == 0) {
            // In root directory, just add the path
            snprintf(full_new_path, sizeof(full_new_path) - 1, "/%s", new_path);
            full_new_path[sizeof(full_new_path) - 1] = '\0';
        } else {
            // In subdirectory, construct full path
            int len = snprintf(full_new_path, sizeof(full_new_path) - 1, "%s/%s", current_path, new_path);
            if (len >= sizeof(full_new_path) - 1) {
                printf("Path too long: %s\n", full_new_path);
                return -1;
            }
        }
    }
    
    int err = lfs_rename(&lfs, full_old_path, full_new_path);
    if (err) {
        printf("Failed to rename '%s' to '%s': %d\n", full_old_path, full_new_path, err);
        return err;
    }
    
    printf("Renamed '%s' to '%s'\n", full_old_path, full_new_path);
    return 0;
}

static int cmd_cd(const char *path) {
    if (!mounted) {
        printf("Filesystem not mounted\n");
        return -1;
    }
    
    if (!path || strlen(path) == 0) {
        printf("Path not provided\n");
        return -1;
    }
    
    // Handle special case: cd ..
    if (strcmp(path, "..") == 0) {
        // Find last '/' and remove the last directory part
        char *last_slash = strrchr(current_path, '/');
        if (last_slash && last_slash != current_path) {
            *(last_slash) = '\0';
        } else if (last_slash && last_slash == current_path) {
            // We're already at root
            strcpy(current_path, "/");
        } else {
            // Should not happen for valid paths
            strcpy(current_path, "/");
        }
        printf("Changed to parent directory: %s\n", current_path);
        return 0;
    }
    
    // Handle absolute path (starts with /)
    char new_path[1024];  // Increased size
    if (path[0] == '/') {
        strncpy(new_path, path, sizeof(new_path) - 1);
        new_path[sizeof(new_path) - 1] = '\0';
    } else {
        // Handle relative path
        if (strcmp(current_path, "/") == 0) {
            snprintf(new_path, sizeof(new_path) - 1, "/%s", path);  // Ensure space for null terminator
        } else {
            int written = snprintf(new_path, sizeof(new_path) - 1, "%s/%s", current_path, path);
            if (written < 0 || written >= (int)(sizeof(new_path) - 1)) {
                printf("Path too long\n");
                return -1;
            }
        }
        // Normalize the path by resolving .. and .
    }
    
    // Check if the directory exists
    struct lfs_info info;
    int err = lfs_stat(&lfs, new_path, &info);
    if (err) {
        printf("Directory '%s' does not exist\n", new_path);
        return err;
    }
    
    if (info.type != LFS_TYPE_DIR) {
        printf("'%s' is not a directory\n", new_path);
        return -1;
    }
    
    strncpy(current_path, new_path, sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';
    printf("Changed directory to: %s\n", current_path);
    return 0;
}

static int cmd_pwd() {
    printf("%s\n", current_path);
    return 0;
}