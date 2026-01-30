/*
 * DSM - Distributed Shared Memory System
 * Main header file with definitions and structures
 */

#ifndef DSM_H
#define DSM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Page size - must match system page size for mprotect to work */
#ifdef __APPLE__
    /* Apple Silicon uses 16KB pages */
    #define PAGE_SIZE           16384
#else
    /* x86/Linux uses 4KB pages */
    #define PAGE_SIZE           4096
#endif

#define DSM_REGION_SIZE     (16 * 1024 * 1024)  /* 16 MB shared region */
#define DSM_NUM_PAGES       (DSM_REGION_SIZE / PAGE_SIZE)
#define DSM_BASE_ADDR       ((void*)0x700000000000ULL)  /* Fixed address for all processes */

#define MAX_CLIENTS         4
#define MAX_MSG_SIZE        (PAGE_SIZE + 256)
#define DEFAULT_PORT        9000

#define VECTOR_SIZE         (1024 * 1024)  /* 1M integers for demo */

/* ============================================================================
 * Page States (Coherence Protocol)
 * ============================================================================ */

typedef enum {
    PAGE_ABSENT = 0,      /* Page not present locally */
    PAGE_SHARED,          /* Page present, read-only (can be replicated) */
    PAGE_EXCLUSIVE        /* Page present, read-write (single owner) */
} page_state_t;

/* ============================================================================
 * Network Message Types
 * ============================================================================ */

typedef enum {
    MSG_CONNECT = 1,          /* Client connects to server */
    MSG_CONNECT_ACK,          /* Server acknowledges connection */
    MSG_PAGE_REQUEST_READ,    /* Request page for reading */
    MSG_PAGE_REQUEST_WRITE,   /* Request page for writing (exclusive) */
    MSG_PAGE_DATA,            /* Page data transfer */
    MSG_INVALIDATE,           /* Invalidate page (downgrade to ABSENT) */
    MSG_INVALIDATE_ACK,       /* Acknowledge invalidation (with dirty data if needed) */
    MSG_TASK_ASSIGN,          /* Assign work range to client */
    MSG_TASK_DONE,            /* Client finished its task */
    MSG_RESULT,               /* Send result back */
    MSG_SHUTDOWN              /* Shutdown signal */
} msg_type_t;

/* ============================================================================
 * Network Message Structure
 * ============================================================================ */

typedef struct {
    msg_type_t type;
    uint32_t   page_num;      /* Page number (when applicable) */
    uint32_t   client_id;     /* Client identifier */
    uint32_t   data_len;      /* Length of payload data */
    union {
        struct {
            uint32_t start_idx;   /* Start index for task */
            uint32_t end_idx;     /* End index for task */
        } task;
        struct {
            int64_t value;        /* Result value */
        } result;
        uint8_t page_data[PAGE_SIZE];  /* Page content */
    } payload;
} __attribute__((packed)) dsm_msg_t;

/* ============================================================================
 * Local Page Table Entry
 * ============================================================================ */

typedef struct {
    page_state_t state;
    uint32_t     version;     /* For debugging/verification */
    bool         dirty;       /* Modified since last sync */
    pthread_mutex_t lock;     /* Per-page lock */
} local_page_entry_t;

/* ============================================================================
 * Global Page Directory (Server only)
 * ============================================================================ */

typedef struct {
    int          owner;       /* Client ID of exclusive owner, -1 if none */
    uint32_t     version;
    uint32_t     sharers;     /* Bitmask of clients with shared copies */
    pthread_mutex_t lock;
} global_page_entry_t;

/* ============================================================================
 * Client Info (Server side)
 * ============================================================================ */

typedef struct {
    int          id;
    int          socket_fd;
    bool         connected;
    bool         ready;       /* Handshake complete */
    uint32_t     task_start;
    uint32_t     task_end;
    int64_t      result;
    bool         task_done;
    pthread_t    handler_thread;
} client_info_t;

/* ============================================================================
 * DSM Context (Main runtime structure)
 * ============================================================================ */

typedef struct {
    /* Mode */
    bool is_server;
    
    /* Network */
    int socket_fd;
    char server_host[256];
    int server_port;
    int client_id;
    
    /* Memory region */
    void *region_base;
    size_t region_size;
    int backing_fd;
    char backing_file[256];
    
    /* Local page table */
    local_page_entry_t *local_pages;
    
    /* Server-only: global directory and clients */
    global_page_entry_t *global_pages;
    client_info_t clients[MAX_CLIENTS];
    int num_clients;
    pthread_mutex_t clients_lock;
    
    /* Synchronization for fault handling */
    pthread_mutex_t fault_lock;
    pthread_cond_t fault_cond;
    volatile uint32_t pending_page;
    volatile bool page_received;
    
    /* Debug/verbose mode */
    bool verbose;
    
    /* Running state */
    volatile bool running;
} dsm_context_t;

/* ============================================================================
 * Global context (singleton)
 * ============================================================================ */

extern dsm_context_t *g_dsm;

/* ============================================================================
 * Function Prototypes
 * ============================================================================ */

/* memory.c */
int dsm_memory_init(dsm_context_t *ctx);
void dsm_memory_cleanup(dsm_context_t *ctx);
void dsm_protect_page(dsm_context_t *ctx, uint32_t page_num, page_state_t state);
void *dsm_page_addr(dsm_context_t *ctx, uint32_t page_num);
uint32_t dsm_addr_to_page(dsm_context_t *ctx, void *addr);
bool dsm_addr_in_region(dsm_context_t *ctx, void *addr);

/* network.c */
int dsm_network_init_server(dsm_context_t *ctx);
int dsm_network_init_client(dsm_context_t *ctx);
void dsm_network_cleanup(dsm_context_t *ctx);
int dsm_send_msg(int fd, dsm_msg_t *msg);
int dsm_recv_msg(int fd, dsm_msg_t *msg);

/* fault_handler.c */
int dsm_fault_handler_init(dsm_context_t *ctx);
void dsm_fault_handler_cleanup(dsm_context_t *ctx);

/* coherence.c */
int dsm_request_page_read(dsm_context_t *ctx, uint32_t page_num);
int dsm_request_page_write(dsm_context_t *ctx, uint32_t page_num);
int dsm_handle_page_request(dsm_context_t *ctx, int client_id, uint32_t page_num, bool for_write);
int dsm_handle_invalidate(dsm_context_t *ctx, uint32_t page_num);
int dsm_handle_page_data(dsm_context_t *ctx, uint32_t page_num, void *data, page_state_t new_state);

/* worker.c */
void dsm_server_init_data(dsm_context_t *ctx);
int64_t dsm_worker_compute(dsm_context_t *ctx, uint32_t start, uint32_t end);
int64_t dsm_expected_result(void);

/* Logging macros */
#define DSM_LOG(ctx, fmt, ...) \
    do { \
        if ((ctx)->verbose) { \
            fprintf(stderr, "[%s:%d] " fmt "\n", \
                    (ctx)->is_server ? "SERVER" : "CLIENT", \
                    (ctx)->is_server ? 0 : (ctx)->client_id, \
                    ##__VA_ARGS__); \
        } \
    } while(0)

#define DSM_ERROR(fmt, ...) \
    fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define DSM_INFO(fmt, ...) \
    fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)

#endif /* DSM_H */
