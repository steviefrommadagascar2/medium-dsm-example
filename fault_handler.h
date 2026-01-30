/*
 * DSM - Fault Handler Header
 */

#ifndef FAULT_HANDLER_H
#define FAULT_HANDLER_H

#include "dsm.h"

/* Initialize SIGSEGV/SIGBUS handlers */
int dsm_fault_handler_init(dsm_context_t *ctx);

/* Cleanup handlers */
void dsm_fault_handler_cleanup(dsm_context_t *ctx);

/* Safe read function - handles page faults automatically */
int32_t dsm_read_int32(dsm_context_t *ctx, int32_t *addr);

/* Try to access an address, returns NULL if page fault would occur */
void *dsm_try_access(dsm_context_t *ctx, void *addr, uint32_t *out_page_num, int *out_for_write);

#endif /* FAULT_HANDLER_H */
