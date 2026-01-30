/*
 * DSM - Worker Implementation
 * Demo: Distributed vector sum computation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "worker.h"
#include "memory.h"
#include "fault_handler.h"

/*
 * Memory layout in DSM region:
 * - First part: int32_t vector[VECTOR_SIZE]
 * - Remaining: available for other use
 */

#define VECTOR_OFFSET 0
#define VECTOR_BYTES  (VECTOR_SIZE * sizeof(int32_t))

static inline int32_t *get_vector_ptr(dsm_context_t *ctx)
{
    return (int32_t*)((char*)ctx->region_base + VECTOR_OFFSET);
}

/*
 * Server: Initialize the vector with test data
 * Uses a simple pattern: vector[i] = i % 100
 * This gives predictable results for verification
 */
void dsm_server_init_data(dsm_context_t *ctx)
{
    int32_t *vector = get_vector_ptr(ctx);
    
    DSM_INFO("Initializing vector of %d elements...", VECTOR_SIZE);
    
    for (uint32_t i = 0; i < VECTOR_SIZE; i++) {
        vector[i] = i % 100;
    }
    
    /* Mark the pages containing vector as owned by server */
    uint32_t num_vector_pages = (VECTOR_BYTES + PAGE_SIZE - 1) / PAGE_SIZE;
    DSM_LOG(ctx, "Vector uses %u pages", num_vector_pages);
    
    DSM_INFO("Vector initialized");
}

/*
 * Worker: Compute partial sum of vector[start..end)
 * Uses dsm_read_int32 which handles page faults safely
 */
int64_t dsm_worker_compute(dsm_context_t *ctx, uint32_t start, uint32_t end)
{
    int32_t *vector = get_vector_ptr(ctx);
    int64_t sum = 0;
    
    DSM_INFO("Computing sum for range [%u, %u)", start, end);
    
    /* Access each element using safe read function */
    for (uint32_t i = start; i < end; i++) {
        /* dsm_read_int32 handles page faults automatically */
        int32_t value = dsm_read_int32(ctx, &vector[i]);
        sum += value;
        
        /* Log progress occasionally */
        if (ctx->verbose && (i - start) % 100000 == 0 && i > start) {
            DSM_LOG(ctx, "Progress: %u/%u elements", i - start, end - start);
        }
    }
    
    DSM_INFO("Partial sum for [%u, %u) = %" PRId64, start, end, sum);
    return sum;
}

/*
 * Calculate expected result for the full vector
 * sum(i % 100 for i in 0..VECTOR_SIZE-1)
 */
int64_t dsm_expected_result(void)
{
    /* 
     * Pattern repeats every 100 elements
     * Sum of 0+1+2+...+99 = 99*100/2 = 4950
     * Number of complete cycles = VECTOR_SIZE / 100
     * Remaining elements: VECTOR_SIZE % 100
     */
    int64_t full_cycles = VECTOR_SIZE / 100;
    int64_t remainder = VECTOR_SIZE % 100;
    int64_t cycle_sum = 99 * 100 / 2;  /* 4950 */
    
    int64_t result = full_cycles * cycle_sum;
    
    /* Add remaining partial cycle */
    for (int64_t i = 0; i < remainder; i++) {
        result += i;
    }
    
    return result;
}
