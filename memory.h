/*
 * DSM - Memory Management Header
 */

#ifndef MEMORY_H
#define MEMORY_H

#include "dsm.h"

/* Initialize the shared memory region with mmap */
int dsm_memory_init(dsm_context_t *ctx);

/* Cleanup memory region */
void dsm_memory_cleanup(dsm_context_t *ctx);

/* Set page protection based on state */
void dsm_protect_page(dsm_context_t *ctx, uint32_t page_num, page_state_t state);

/* Get address of a specific page */
void *dsm_page_addr(dsm_context_t *ctx, uint32_t page_num);

/* Convert address to page number */
uint32_t dsm_addr_to_page(dsm_context_t *ctx, void *addr);

/* Check if address is within DSM region */
bool dsm_addr_in_region(dsm_context_t *ctx, void *addr);

/* Sync page to backing file */
void dsm_sync_page(dsm_context_t *ctx, uint32_t page_num);

#endif /* MEMORY_H */
