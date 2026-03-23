/*
 * Block device emulated in RAM
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bd/lfs_rambd.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

extern volatile int g_flash_fault_injection_enabled;
extern volatile int g_flash_fault_injection_start;

#ifdef _WIN32
static DWORD WINAPI lfs_rambd_fault_worker(LPVOID arg) {
    (void)arg;
    while (1) {
        DWORD delay_ms = 100u + (DWORD)(rand() % 401);
        Sleep(delay_ms);
        if (g_flash_fault_injection_start == 1) {
            g_flash_fault_injection_enabled = 1;
        }
    }
    return 0;
}
#else
static void *lfs_rambd_fault_worker(void *arg) {
    (void)arg;
    while (1) {
        unsigned delay_ms = 100u + (unsigned)(rand() % 401);
        usleep(delay_ms * 1000u);
        if (g_flash_fault_injection_start == 1) {
            g_flash_fault_injection_enabled = 1;
        }
    }
    return NULL;
}
#endif

int lfs_rambd_createcfg(const struct lfs_config *cfg,
        const struct lfs_rambd_config *bdcfg) {
    LFS_RAMBD_TRACE("lfs_rambd_createcfg(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"}, "
                "%p {.erase_value=%"PRId32", .buffer=%p})",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            (void*)bdcfg, bdcfg->erase_value, bdcfg->buffer);
    lfs_rambd_t *bd = cfg->context;
    bd->cfg = bdcfg;
    g_flash_fault_injection_enabled = 0;
    g_flash_fault_injection_start = 1;
    srand((unsigned)time(NULL));

    // allocate buffer?
    if (bd->cfg->buffer) {
        bd->buffer = bd->cfg->buffer;
    } else {
        bd->buffer = lfs_malloc(cfg->block_size * cfg->block_count);
        if (!bd->buffer) {
            LFS_RAMBD_TRACE("lfs_rambd_createcfg -> %d", LFS_ERR_NOMEM);
            return LFS_ERR_NOMEM;
        }
    }

    // zero for reproducibility?
    if (bd->cfg->erase_value != -1) {
        memset(bd->buffer, bd->cfg->erase_value,
                cfg->block_size * cfg->block_count);
    } else {
        memset(bd->buffer, 0, cfg->block_size * cfg->block_count);
    }

#ifdef _WIN32
    HANDLE worker = CreateThread(NULL, 0, lfs_rambd_fault_worker, NULL, 0, NULL);
    if (worker) {
        CloseHandle(worker);
    }
#else
    pthread_t worker;
    if (pthread_create(&worker, NULL, lfs_rambd_fault_worker, NULL) == 0) {
        pthread_detach(worker);
    }
#endif

    LFS_RAMBD_TRACE("lfs_rambd_createcfg -> %d", 0);
    return 0;
}

int lfs_rambd_create(const struct lfs_config *cfg) {
    LFS_RAMBD_TRACE("lfs_rambd_create(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"})",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count);
    static const struct lfs_rambd_config defaults = {.erase_value=-1};
    int err = lfs_rambd_createcfg(cfg, &defaults);
    LFS_RAMBD_TRACE("lfs_rambd_create -> %d", err);
    return err;
}

int lfs_rambd_destroy(const struct lfs_config *cfg) {
    LFS_RAMBD_TRACE("lfs_rambd_destroy(%p)", (void*)cfg);
    // clean up memory
    lfs_rambd_t *bd = cfg->context;
    if (!bd->cfg->buffer) {
        lfs_free(bd->buffer);
    }
    LFS_RAMBD_TRACE("lfs_rambd_destroy -> %d", 0);
    return 0;
}

int lfs_rambd_read(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    LFS_RAMBD_TRACE("lfs_rambd_read(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs_rambd_t *bd = cfg->context;

    // check if read is valid
    LFS_ASSERT(off  % cfg->read_size == 0);
    LFS_ASSERT(size % cfg->read_size == 0);
    LFS_ASSERT(block < cfg->block_count);

    // read data
    memcpy(buffer, &bd->buffer[block*cfg->block_size + off], size);

    LFS_RAMBD_TRACE("lfs_rambd_read -> %d", 0);
    return 0;
}

int lfs_rambd_prog(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    LFS_RAMBD_TRACE("lfs_rambd_prog(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs_rambd_t *bd = cfg->context;

    // check if write is valid
    LFS_ASSERT(off  % cfg->prog_size == 0);
    LFS_ASSERT(size % cfg->prog_size == 0);
    LFS_ASSERT(block < cfg->block_count);

    // check that data was erased? only needed for testing
    if (bd->cfg->erase_value != -1) {
        for (lfs_off_t i = 0; i < size; i++) {
            LFS_ASSERT(bd->buffer[block*cfg->block_size + off + i] ==
                    bd->cfg->erase_value);
        }
    }

    uint8_t *fault_buffer = NULL;
    const void *write_buffer = buffer;
    if (g_flash_fault_injection_enabled && size > 0 && size < 256) {
        fault_buffer = lfs_malloc(size);
        if (!fault_buffer) {
            LFS_RAMBD_TRACE("lfs_rambd_prog -> %d", LFS_ERR_NOMEM);
            return LFS_ERR_NOMEM;
        }

        memcpy(fault_buffer, buffer, size);
        lfs_size_t fault_offset = (lfs_size_t)(rand() % size);
        fault_buffer[fault_offset] = rand() % 0xff;
        write_buffer = fault_buffer;
        g_flash_fault_injection_enabled = 0;
        printf("\n***#####flash error#####***\n");
    }

    // program data
    memcpy(&bd->buffer[block*cfg->block_size + off], write_buffer, size);
    if (fault_buffer) {
        lfs_free(fault_buffer);
    }

    LFS_RAMBD_TRACE("lfs_rambd_prog -> %d", 0);
    return 0;
}

int lfs_rambd_erase(const struct lfs_config *cfg, lfs_block_t block) {
    LFS_RAMBD_TRACE("lfs_rambd_erase(%p, 0x%"PRIx32")", (void*)cfg, block);
    lfs_rambd_t *bd = cfg->context;

    // check if erase is valid
    LFS_ASSERT(block < cfg->block_count);

    // erase, only needed for testing
    if (bd->cfg->erase_value != -1) {
        memset(&bd->buffer[block*cfg->block_size],
                bd->cfg->erase_value, cfg->block_size);
    }

    LFS_RAMBD_TRACE("lfs_rambd_erase -> %d", 0);
    return 0;
}

int lfs_rambd_sync(const struct lfs_config *cfg) {
    LFS_RAMBD_TRACE("lfs_rambd_sync(%p)", (void*)cfg);
    // sync does nothing because we aren't backed by anything real
    (void)cfg;
    LFS_RAMBD_TRACE("lfs_rambd_sync -> %d", 0);
    return 0;
}
