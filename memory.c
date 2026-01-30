/*
 * DSM - Memory Management
 * Handles mmap region, mprotect for page access control
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

#include "memory.h"

int dsm_memory_init(dsm_context_t *ctx)
{
    /* Create backing file for the shared region */
    snprintf(ctx->backing_file, sizeof(ctx->backing_file), 
             "/tmp/dsm_region_%d.dat", getpid());
    
    ctx->backing_fd = open(ctx->backing_file, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (ctx->backing_fd < 0) {
        DSM_ERROR("Failed to create backing file: %s", strerror(errno));
        return -1;
    }
    
    /* Extend file to region size */
    if (ftruncate(ctx->backing_fd, DSM_REGION_SIZE) < 0) {
        DSM_ERROR("Failed to extend backing file: %s", strerror(errno));
        close(ctx->backing_fd);
        return -1;
    }
    
    /* Map the region at a fixed address */
    ctx->region_base = mmap(DSM_BASE_ADDR, DSM_REGION_SIZE,
                            PROT_NONE,  /* Start with no access */
                            MAP_SHARED | MAP_FIXED,
                            ctx->backing_fd, 0);
    
    if (ctx->region_base == MAP_FAILED) {
        DSM_ERROR("Failed to mmap region at fixed address: %s", strerror(errno));
        close(ctx->backing_fd);
        return -1;
    }
    
    if (ctx->region_base != DSM_BASE_ADDR) {
        DSM_ERROR("mmap returned different address: %p vs %p", 
                  ctx->region_base, DSM_BASE_ADDR);
        munmap(ctx->region_base, DSM_REGION_SIZE);
        close(ctx->backing_fd);
        return -1;
    }
    
    /* 
     * Classic pattern: unlink immediately after mmap succeeds.
     * File disappears from filesystem, but mmap region persists.
     * Kernel auto-cleans on process exit (no garbage files on crash).
     */
    unlink(ctx->backing_file);
    ctx->backing_file[0] = '\0';  /* Mark as already deleted */
    
    ctx->region_size = DSM_REGION_SIZE;
    
    /* Allocate local page table */
    ctx->local_pages = calloc(DSM_NUM_PAGES, sizeof(local_page_entry_t));
    if (!ctx->local_pages) {
        DSM_ERROR("Failed to allocate local page table");
        munmap(ctx->region_base, DSM_REGION_SIZE);
        close(ctx->backing_fd);
        return -1;
    }
    
    /* Initialize page entries */
    for (uint32_t i = 0; i < DSM_NUM_PAGES; i++) {
        ctx->local_pages[i].state = PAGE_ABSENT;
        ctx->local_pages[i].version = 0;
        ctx->local_pages[i].dirty = false;
        pthread_mutex_init(&ctx->local_pages[i].lock, NULL);
    }
    
    /* Server also needs global page directory */
    if (ctx->is_server) {
        ctx->global_pages = calloc(DSM_NUM_PAGES, sizeof(global_page_entry_t));
        if (!ctx->global_pages) {
            DSM_ERROR("Failed to allocate global page directory");
            free(ctx->local_pages);
            munmap(ctx->region_base, DSM_REGION_SIZE);
            close(ctx->backing_fd);
            return -1;
        }
        
        for (uint32_t i = 0; i < DSM_NUM_PAGES; i++) {
            ctx->global_pages[i].owner = -1;  /* Server owns all pages initially */
            ctx->global_pages[i].version = 0;
            ctx->global_pages[i].sharers = 0;
            pthread_mutex_init(&ctx->global_pages[i].lock, NULL);
        }
        
        /* Server starts with all pages in EXCLUSIVE mode */
        if (mprotect(ctx->region_base, DSM_REGION_SIZE, PROT_READ | PROT_WRITE) < 0) {
            DSM_ERROR("Failed to set initial protection: %s", strerror(errno));
            free(ctx->global_pages);
            free(ctx->local_pages);
            munmap(ctx->region_base, DSM_REGION_SIZE);
            close(ctx->backing_fd);
            return -1;
        }
        
        for (uint32_t i = 0; i < DSM_NUM_PAGES; i++) {
            ctx->local_pages[i].state = PAGE_EXCLUSIVE;
        }
    }
    
    DSM_LOG(ctx, "Memory region initialized at %p (%zu bytes, %u pages)",
            ctx->region_base, ctx->region_size, DSM_NUM_PAGES);
    
    return 0;
}

void dsm_memory_cleanup(dsm_context_t *ctx)
{
    if (ctx->local_pages) {
        for (uint32_t i = 0; i < DSM_NUM_PAGES; i++) {
            pthread_mutex_destroy(&ctx->local_pages[i].lock);
        }
        free(ctx->local_pages);
        ctx->local_pages = NULL;
    }
    
    if (ctx->global_pages) {
        for (uint32_t i = 0; i < DSM_NUM_PAGES; i++) {
            pthread_mutex_destroy(&ctx->global_pages[i].lock);
        }
        free(ctx->global_pages);
        ctx->global_pages = NULL;
    }
    
    if (ctx->region_base && ctx->region_base != MAP_FAILED) {
        munmap(ctx->region_base, ctx->region_size);
        ctx->region_base = NULL;
    }
    
    if (ctx->backing_fd >= 0) {
        close(ctx->backing_fd);
        ctx->backing_fd = -1;
    }
    
    /* Remove backing file */
    if (ctx->backing_file[0]) {
        unlink(ctx->backing_file);
        ctx->backing_file[0] = '\0';
    }
    
    DSM_LOG(ctx, "Memory cleanup complete");
}

void dsm_protect_page(dsm_context_t *ctx, uint32_t page_num, page_state_t state)
{
    if (page_num >= DSM_NUM_PAGES) {
        DSM_ERROR("Invalid page number: %u", page_num);
        return;
    }
    
    void *page_addr = dsm_page_addr(ctx, page_num);
    int prot;
    
    switch (state) {
        case PAGE_ABSENT:
            prot = PROT_NONE;
            break;
        case PAGE_SHARED:
            prot = PROT_READ;
            break;
        case PAGE_EXCLUSIVE:
            prot = PROT_READ | PROT_WRITE;
            break;
        default:
            DSM_ERROR("Unknown page state: %d", state);
            return;
    }
    
    if (mprotect(page_addr, PAGE_SIZE, prot) < 0) {
        DSM_ERROR("mprotect failed for page %u at %p: %s", 
                  page_num, page_addr, strerror(errno));
        return;
    }
    
    ctx->local_pages[page_num].state = state;
}

void *dsm_page_addr(dsm_context_t *ctx, uint32_t page_num)
{
    return (void*)((uintptr_t)ctx->region_base + (page_num * PAGE_SIZE));
}

uint32_t dsm_addr_to_page(dsm_context_t *ctx, void *addr)
{
    uintptr_t offset = (uintptr_t)addr - (uintptr_t)ctx->region_base;
    return (uint32_t)(offset / PAGE_SIZE);
}

bool dsm_addr_in_region(dsm_context_t *ctx, void *addr)
{
    uintptr_t a = (uintptr_t)addr;
    uintptr_t base = (uintptr_t)ctx->region_base;
    return (a >= base && a < base + ctx->region_size);
}

void dsm_sync_page(dsm_context_t *ctx, uint32_t page_num)
{
    void *page_addr = dsm_page_addr(ctx, page_num);
    if (msync(page_addr, PAGE_SIZE, MS_SYNC) < 0) {
        DSM_ERROR("msync failed for page %u: %s", page_num, strerror(errno));
    }
}
