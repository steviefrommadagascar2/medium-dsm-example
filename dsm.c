/*
 * DSM - Distributed Shared Memory System
 * Main entry point - runs as server or client based on arguments
 *
 * Usage:
 *   Server: ./dsm --server [--port PORT] [--clients N] [-v]
 *   Client: ./dsm --client --host HOST [--port PORT] [-v]
 */

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <sys/select.h>
#include <pthread.h>

#include "dsm.h"
#include "memory.h"
#include "network.h"
#include "fault_handler.h"
#include "coherence.h"
#include "worker.h"

/* Global context */
dsm_context_t *g_dsm = NULL;

static void print_usage(const char *prog)
{
    fprintf(stderr, "DSM - Distributed Shared Memory Demo\n\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  Server: %s --server [--port PORT] [--clients N] [-v]\n", prog);
    fprintf(stderr, "  Client: %s --client --host HOST [--port PORT] [-v]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --server, -s       Run as server (master)\n");
    fprintf(stderr, "  --client, -c       Run as client (worker)\n");
    fprintf(stderr, "  --host HOST, -h    Server hostname (client mode)\n");
    fprintf(stderr, "  --port PORT, -p    Port number (default: %d)\n", DEFAULT_PORT);
    fprintf(stderr, "  --clients N, -n    Number of clients to wait for (server, default: 1)\n");
    fprintf(stderr, "  --verbose, -v      Verbose/debug output\n");
    fprintf(stderr, "  --help             Show this help\n");
}

static void signal_handler(int sig)
{
    if (g_dsm) {
        g_dsm->running = false;
    }
    DSM_INFO("Received signal %d, shutting down...", sig);
}

static dsm_context_t *dsm_context_create(void)
{
    dsm_context_t *ctx = calloc(1, sizeof(dsm_context_t));
    if (!ctx) return NULL;
    
    ctx->is_server = false;
    ctx->socket_fd = -1;
    ctx->backing_fd = -1;
    ctx->server_port = DEFAULT_PORT;
    ctx->client_id = -1;
    ctx->running = true;
    ctx->verbose = false;
    
    pthread_mutex_init(&ctx->clients_lock, NULL);
    pthread_mutex_init(&ctx->fault_lock, NULL);
    pthread_cond_init(&ctx->fault_cond, NULL);
    
    return ctx;
}

static void dsm_context_destroy(dsm_context_t *ctx)
{
    if (!ctx) return;
    
    pthread_mutex_destroy(&ctx->clients_lock);
    pthread_mutex_destroy(&ctx->fault_lock);
    pthread_cond_destroy(&ctx->fault_cond);
    
    free(ctx);
}

/* ============================================================================
 * Server Logic
 * ============================================================================ */

static int run_server(dsm_context_t *ctx, int expected_clients)
{
    DSM_INFO("Starting server mode");
    
    /* Initialize memory region */
    if (dsm_memory_init(ctx) < 0) {
        DSM_ERROR("Failed to initialize memory");
        return -1;
    }
    
    /* Initialize demo data */
    dsm_server_init_data(ctx);
    
    /* Start network listener */
    if (dsm_network_init_server(ctx) < 0) {
        DSM_ERROR("Failed to initialize network");
        dsm_memory_cleanup(ctx);
        return -1;
    }
    
    /* Install fault handler (server might also access pages) */
    if (dsm_fault_handler_init(ctx) < 0) {
        DSM_ERROR("Failed to initialize fault handler");
        dsm_network_cleanup(ctx);
        dsm_memory_cleanup(ctx);
        return -1;
    }
    
    DSM_INFO("Waiting for %d client(s) to connect...", expected_clients);
    
    /* Accept clients */
    while (ctx->running && ctx->num_clients < expected_clients) {
        fd_set readfds;
        struct timeval tv = {1, 0};  /* 1 second timeout */
        
        FD_ZERO(&readfds);
        FD_SET(ctx->socket_fd, &readfds);
        
        int ret = select(ctx->socket_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            DSM_ERROR("select failed: %s", strerror(errno));
            break;
        }
        
        if (ret > 0 && FD_ISSET(ctx->socket_fd, &readfds)) {
            int client_id = dsm_accept_client(ctx);
            if (client_id >= 0) {
                /* Start handler thread for this client */
                pthread_create(&ctx->clients[client_id].handler_thread,
                               NULL, dsm_client_handler_thread,
                               &ctx->clients[client_id]);
            }
        }
    }
    
    if (ctx->num_clients < expected_clients) {
        DSM_ERROR("Not enough clients connected");
        goto cleanup;
    }
    
    /* Wait for all clients to complete handshake */
    DSM_INFO("Waiting for clients to complete handshake...");
    int ready_count = 0;
    while (ctx->running && ready_count < ctx->num_clients) {
        ready_count = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (ctx->clients[i].connected && ctx->clients[i].ready) {
                ready_count++;
            }
        }
        if (ready_count < ctx->num_clients) {
            usleep(10000);  /* 10ms */
        }
    }
    
    DSM_INFO("All %d clients connected, distributing work...", ctx->num_clients);
    
    /* Distribute work ranges to clients */
    uint32_t chunk_size = VECTOR_SIZE / ctx->num_clients;
    uint32_t remaining = VECTOR_SIZE % ctx->num_clients;
    uint32_t offset = 0;
    
    for (int i = 0; i < ctx->num_clients; i++) {
        client_info_t *client = &ctx->clients[i];
        if (!client->connected) continue;
        
        client->task_start = offset;
        client->task_end = offset + chunk_size;
        if (i == ctx->num_clients - 1) {
            client->task_end += remaining;  /* Last client gets remainder */
        }
        
        dsm_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_TASK_ASSIGN;
        msg.client_id = i;
        msg.payload.task.start_idx = client->task_start;
        msg.payload.task.end_idx = client->task_end;
        
        DSM_INFO("Assigning client %d: [%u, %u)", i, 
                 client->task_start, client->task_end);
        
        if (dsm_send_msg(client->socket_fd, &msg) < 0) {
            DSM_ERROR("Failed to send task to client %d", i);
        }
        
        offset = client->task_end;
    }
    
    /* Wait for all clients to finish */
    DSM_INFO("Waiting for clients to complete...");
    
    int done_count = 0;
    while (ctx->running && done_count < ctx->num_clients) {
        done_count = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (ctx->clients[i].connected || ctx->clients[i].task_done) {
                if (ctx->clients[i].task_done) {
                    done_count++;
                }
            }
        }
        if (done_count < ctx->num_clients) {
            usleep(100000);  /* 100ms */
        }
    }
    
    /* Aggregate results */
    int64_t total = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->clients[i].task_done) {
            total += ctx->clients[i].result;
            DSM_INFO("Client %d result: %" PRId64, i, ctx->clients[i].result);
        }
    }
    
    int64_t expected = dsm_expected_result();
    
    DSM_INFO("========================================");
    DSM_INFO("Computed sum: %" PRId64, total);
    DSM_INFO("Expected sum: %" PRId64, expected);
    DSM_INFO("Result: %s", total == expected ? "CORRECT!" : "MISMATCH!");
    DSM_INFO("========================================");
    
    /* Send shutdown to clients */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->clients[i].connected) {
            dsm_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_SHUTDOWN;
            dsm_send_msg(ctx->clients[i].socket_fd, &msg);
        }
    }
    
    /* Wait for handler threads */
    ctx->running = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->clients[i].handler_thread) {
            pthread_join(ctx->clients[i].handler_thread, NULL);
        }
    }
    
cleanup:
    dsm_fault_handler_cleanup(ctx);
    dsm_network_cleanup(ctx);
    dsm_memory_cleanup(ctx);
    
    return 0;
}

/* ============================================================================
 * Client Logic
 * ============================================================================ */

static int run_client(dsm_context_t *ctx)
{
    DSM_INFO("Starting client mode");
    
    /* Initialize memory region */
    if (dsm_memory_init(ctx) < 0) {
        DSM_ERROR("Failed to initialize memory");
        return -1;
    }
    
    /* Install fault handler */
    if (dsm_fault_handler_init(ctx) < 0) {
        DSM_ERROR("Failed to initialize fault handler");
        dsm_memory_cleanup(ctx);
        return -1;
    }
    
    /* Connect to server */
    if (dsm_network_init_client(ctx) < 0) {
        DSM_ERROR("Failed to connect to server");
        dsm_fault_handler_cleanup(ctx);
        dsm_memory_cleanup(ctx);
        return -1;
    }
    
    /* Wait for task assignment */
    DSM_INFO("Waiting for task assignment...");
    
    dsm_msg_t msg;
    while (ctx->running) {
        if (dsm_recv_msg(ctx->socket_fd, &msg) < 0) {
            DSM_ERROR("Connection lost");
            break;
        }
        
        switch (msg.type) {
            case MSG_TASK_ASSIGN: {
                uint32_t start = msg.payload.task.start_idx;
                uint32_t end = msg.payload.task.end_idx;
                
                DSM_INFO("Received task: compute sum [%u, %u)", start, end);
                
                /* Do the computation */
                int64_t result = dsm_worker_compute(ctx, start, end);
                
                /* Send result */
                memset(&msg, 0, sizeof(msg));
                msg.type = MSG_RESULT;
                msg.client_id = ctx->client_id;
                msg.payload.result.value = result;
                
                if (dsm_send_msg(ctx->socket_fd, &msg) < 0) {
                    DSM_ERROR("Failed to send result");
                }
                
                DSM_INFO("Sent result: %" PRId64, result);
                break;
            }
            
            case MSG_INVALIDATE:
                /* Server is requesting a page back */
                dsm_handle_invalidate(ctx, msg.page_num);
                break;
                
            case MSG_SHUTDOWN:
                DSM_INFO("Received shutdown signal");
                ctx->running = false;
                break;
                
            default:
                DSM_ERROR("Unexpected message type: %d", msg.type);
        }
    }
    
    dsm_fault_handler_cleanup(ctx);
    dsm_network_cleanup(ctx);
    dsm_memory_cleanup(ctx);
    
    return 0;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv)
{
    int is_server = -1;  /* -1 = not set */
    char *host = NULL;
    int port = DEFAULT_PORT;
    int num_clients = 1;
    int verbose = 0;
    
    static struct option long_options[] = {
        {"server",  no_argument,       0, 's'},
        {"client",  no_argument,       0, 'c'},
        {"host",    required_argument, 0, 'h'},
        {"port",    required_argument, 0, 'p'},
        {"clients", required_argument, 0, 'n'},
        {"verbose", no_argument,       0, 'v'},
        {"help",    no_argument,       0, '?'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "sch:p:n:v", long_options, NULL)) != -1) {
        switch (opt) {
            case 's':
                is_server = 1;
                break;
            case 'c':
                is_server = 0;
                break;
            case 'h':
                host = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'n':
                num_clients = atoi(optarg);
                if (num_clients < 1) num_clients = 1;
                if (num_clients > MAX_CLIENTS) num_clients = MAX_CLIENTS;
                break;
            case 'v':
                verbose = 1;
                break;
            case '?':
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    /* Validate arguments */
    if (is_server < 0) {
        DSM_ERROR("Must specify --server or --client");
        print_usage(argv[0]);
        return 1;
    }
    
    if (!is_server && !host) {
        DSM_ERROR("Client mode requires --host");
        print_usage(argv[0]);
        return 1;
    }
    
    /* Create context */
    g_dsm = dsm_context_create();
    if (!g_dsm) {
        DSM_ERROR("Failed to create context");
        return 1;
    }
    
    g_dsm->is_server = is_server;
    g_dsm->server_port = port;
    g_dsm->verbose = verbose;
    
    if (host) {
        strncpy(g_dsm->server_host, host, sizeof(g_dsm->server_host) - 1);
    }
    
    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    /* Run */
    int result;
    if (is_server) {
        result = run_server(g_dsm, num_clients);
    } else {
        result = run_client(g_dsm);
    }
    
    dsm_context_destroy(g_dsm);
    g_dsm = NULL;
    
    return result;
}
