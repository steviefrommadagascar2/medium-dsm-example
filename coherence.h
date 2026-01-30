/*
 * DSM - Coherence Protocol Header
 */

#ifndef COHERENCE_H
#define COHERENCE_H

#include "dsm.h"

/* Client: Request page for reading (SHARED state) */
int dsm_request_page_read(dsm_context_t *ctx, uint32_t page_num);

/* Client: Request page for writing (EXCLUSIVE state) */
int dsm_request_page_write(dsm_context_t *ctx, uint32_t page_num);

/* Server: Handle page request from client */
int dsm_handle_page_request(dsm_context_t *ctx, int client_id, 
                            uint32_t page_num, bool for_write);

/* Client: Handle invalidation request from server */
int dsm_handle_invalidate(dsm_context_t *ctx, uint32_t page_num);

/* Client: Handle received page data */
int dsm_handle_page_data(dsm_context_t *ctx, uint32_t page_num, 
                         void *data, page_state_t new_state);

/* Utility: state name for debugging */
const char *dsm_state_name(page_state_t state);

#endif /* COHERENCE_H */
