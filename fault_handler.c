/*
 * DSM - Fault Handler
 * Uses setjmp/longjmp pattern for safe page fault handling
 * 
 * The pattern:
 * 1. Worker wraps memory access with dsm_try_access()
 * 2. If fault happens, handler longjmps back with fault info
 * 3. Caller handles the fault (network I/O) outside signal context
 * 4. Caller retries the access
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include "fault_handler.h"
#include "memory.h"
#include "coherence.h"

static struct sigaction old_sigsegv_action;
static struct sigaction old_sigbus_action;

/* Thread-local storage for fault handling */
static __thread sigjmp_buf fault_jmp_buf;
static __thread volatile sig_atomic_t in_protected_region = 0;
static __thread void *last_fault_addr = NULL;
static __thread int last_fault_write = 0;

/*
 * Signal handler - just records fault info and jumps back
 * Does NOT do any I/O - that's unsafe in signal context
 */
static void fault_signal_handler(int sig, siginfo_t *info, void *context)
{
    (void)context;
    dsm_context_t *ctx = g_dsm;
    
    if (!ctx || !info) {
        goto chain_handler;
    }
    
    void *addr = info->si_addr;
    
    /* Check if fault is in our DSM region */
    if (!dsm_addr_in_region(ctx, addr)) {
        goto chain_handler;
    }
    
    /* Only handle if we're in a protected region */
    if (!in_protected_region) {
        DSM_ERROR("DSM fault at %p but not in protected region!", addr);
        goto chain_handler;
    }
    
    /* Record fault info */
    last_fault_addr = addr;
    
    /* Determine if this was a write attempt */
    uint32_t page_num = dsm_addr_to_page(ctx, addr);
    page_state_t state = ctx->local_pages[page_num].state;
    /* If page is SHARED and we faulted, it must be a write attempt */
    last_fault_write = (state == PAGE_SHARED) ? 1 : 0;
    
    /* Jump back to the setjmp point - this exits the signal handler safely */
    siglongjmp(fault_jmp_buf, 1);
    /* NOT REACHED */
    
chain_handler:
    ;  /* Empty statement after label */
    {
        /* Chain to previous handler */
        struct sigaction *old = (sig == SIGSEGV) ? &old_sigsegv_action : &old_sigbus_action;
        
        if (old->sa_flags & SA_SIGINFO) {
            if (old->sa_sigaction) {
                old->sa_sigaction(sig, info, context);
                return;
            }
        } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
            old->sa_handler(sig);
            return;
        }
        
        /* Default: terminate */
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

int dsm_fault_handler_init(dsm_context_t *ctx)
{
    struct sigaction sa;
    
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    
    /* Install for both SIGSEGV and SIGBUS */
    if (sigaction(SIGSEGV, &sa, &old_sigsegv_action) < 0) {
        DSM_ERROR("Failed to install SIGSEGV handler: %s", strerror(errno));
        return -1;
    }
    
    if (sigaction(SIGBUS, &sa, &old_sigbus_action) < 0) {
        DSM_ERROR("Failed to install SIGBUS handler: %s", strerror(errno));
        sigaction(SIGSEGV, &old_sigsegv_action, NULL);
        return -1;
    }
    
    DSM_LOG(ctx, "Fault handlers installed (SIGSEGV + SIGBUS)");
    return 0;
}

void dsm_fault_handler_cleanup(dsm_context_t *ctx)
{
    sigaction(SIGSEGV, &old_sigsegv_action, NULL);
    sigaction(SIGBUS, &old_sigbus_action, NULL);
    DSM_LOG(ctx, "Fault handlers restored");
}

/*
 * Try to access a DSM address safely.
 * Returns a pointer to use, or NULL if a page fault occurred.
 * On NULL return, *out_page_num and *out_for_write contain fault info.
 * 
 * Usage pattern:
 *   while ((ptr = dsm_try_access(ctx, addr, &page, &write)) == NULL) {
 *       // Handle fault - fetch page from server
 *       dsm_request_page(ctx, page, write);
 *   }
 *   // Now ptr is safe to use
 */
void *dsm_try_access(dsm_context_t *ctx, void *addr, uint32_t *out_page_num, int *out_for_write)
{
    /* First check if page is already accessible */
    if (dsm_addr_in_region(ctx, addr)) {
        uint32_t page_num = dsm_addr_to_page(ctx, addr);
        page_state_t state = ctx->local_pages[page_num].state;
        
        if (state == PAGE_EXCLUSIVE || state == PAGE_SHARED) {
            /* Page is accessible (at least for reading) */
            return addr;
        }
        
        /* Page is absent - report fault without actually faulting */
        if (out_page_num) *out_page_num = page_num;
        if (out_for_write) *out_for_write = 0;  /* Assume read initially */
        return NULL;
    }
    
    /* Not in DSM region - just return the address */
    return addr;
}

/*
 * Read an int32_t from DSM region safely, handling page faults.
 * This is the main access function for the worker.
 */
int32_t dsm_read_int32(dsm_context_t *ctx, int32_t *addr)
{
    uint32_t page_num;
    int for_write;
    
    /* Keep trying until we have the page */
    while (dsm_try_access(ctx, addr, &page_num, &for_write) == NULL) {
        DSM_LOG(ctx, "Need page %u for read, fetching...", page_num);
        if (dsm_request_page_read(ctx, page_num) < 0) {
            DSM_ERROR("Failed to fetch page %u", page_num);
            return 0;
        }
    }
    
    /* Now try the actual access with signal protection */
    in_protected_region = 1;
    last_fault_addr = NULL;
    
    if (sigsetjmp(fault_jmp_buf, 1) == 0) {
        /* Normal path */
        int32_t value = *addr;
        in_protected_region = 0;
        return value;
    } else {
        /* Fault occurred - handle it */
        in_protected_region = 0;
        
        if (last_fault_addr) {
            page_num = dsm_addr_to_page(ctx, last_fault_addr);
            DSM_LOG(ctx, "Caught fault for page %u, fetching...", page_num);
            
            if (last_fault_write) {
                dsm_request_page_write(ctx, page_num);
            } else {
                dsm_request_page_read(ctx, page_num);
            }
            
            /* Retry */
            return dsm_read_int32(ctx, addr);
        }
        
        return 0;
    }
}
