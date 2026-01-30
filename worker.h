/*
 * DSM - Worker Header
 * Demo computation (vector sum)
 */

#ifndef WORKER_H
#define WORKER_H

#include "dsm.h"

/* Server: Initialize vector data in DSM region */
void dsm_server_init_data(dsm_context_t *ctx);

/* Worker: Compute partial sum over [start, end) */
int64_t dsm_worker_compute(dsm_context_t *ctx, uint32_t start, uint32_t end);

/* Get expected result for verification */
int64_t dsm_expected_result(void);

#endif /* WORKER_H */
