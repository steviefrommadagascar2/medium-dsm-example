/*
 * DSM - Network Communication
 * TCP-based message passing between server and clients
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>

#include "network.h"
#include "coherence.h"

/* Helper to send all bytes */
static ssize_t send_all(int fd, const void *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char*)buf + sent, len - sent, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        sent += n;
    }
    return sent;
}

/* Helper to receive all bytes */
static ssize_t recv_all(int fd, void *buf, size_t len)
{
    size_t received = 0;
    while (received < len) {
        ssize_t n = recv(fd, (char*)buf + received, len - received, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return n == 0 ? 0 : -1;
        }
        received += n;
    }
    return received;
}

int dsm_network_init_server(dsm_context_t *ctx)
{
    struct sockaddr_in addr;
    int opt = 1;
    
    ctx->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->socket_fd < 0) {
        DSM_ERROR("Failed to create socket: %s", strerror(errno));
        return -1;
    }
    
    /* Allow address reuse */
    if (setsockopt(ctx->socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        DSM_ERROR("setsockopt SO_REUSEADDR failed: %s", strerror(errno));
        close(ctx->socket_fd);
        return -1;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(ctx->server_port);
    
    if (bind(ctx->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        DSM_ERROR("Failed to bind to port %d: %s", ctx->server_port, strerror(errno));
        close(ctx->socket_fd);
        return -1;
    }
    
    if (listen(ctx->socket_fd, MAX_CLIENTS) < 0) {
        DSM_ERROR("Failed to listen: %s", strerror(errno));
        close(ctx->socket_fd);
        return -1;
    }
    
    DSM_INFO("Server listening on port %d", ctx->server_port);
    return 0;
}

int dsm_network_init_client(dsm_context_t *ctx)
{
    struct sockaddr_in addr;
    struct hostent *he;
    
    ctx->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->socket_fd < 0) {
        DSM_ERROR("Failed to create socket: %s", strerror(errno));
        return -1;
    }
    
    /* Disable Nagle's algorithm for lower latency */
    int opt = 1;
    setsockopt(ctx->socket_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    
    he = gethostbyname(ctx->server_host);
    if (!he) {
        DSM_ERROR("Failed to resolve host %s", ctx->server_host);
        close(ctx->socket_fd);
        return -1;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    addr.sin_port = htons(ctx->server_port);
    
    DSM_INFO("Connecting to %s:%d...", ctx->server_host, ctx->server_port);
    
    if (connect(ctx->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        DSM_ERROR("Failed to connect: %s", strerror(errno));
        close(ctx->socket_fd);
        return -1;
    }
    
    /* Send connect message */
    dsm_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_CONNECT;
    
    if (dsm_send_msg(ctx->socket_fd, &msg) < 0) {
        DSM_ERROR("Failed to send connect message");
        close(ctx->socket_fd);
        return -1;
    }
    
    /* Wait for acknowledgment */
    if (dsm_recv_msg(ctx->socket_fd, &msg) < 0) {
        DSM_ERROR("Failed to receive connect ack");
        close(ctx->socket_fd);
        return -1;
    }
    
    if (msg.type != MSG_CONNECT_ACK) {
        DSM_ERROR("Unexpected response type: %d", msg.type);
        close(ctx->socket_fd);
        return -1;
    }
    
    ctx->client_id = msg.client_id;
    DSM_INFO("Connected as client %d", ctx->client_id);
    
    return 0;
}

void dsm_network_cleanup(dsm_context_t *ctx)
{
    if (ctx->socket_fd >= 0) {
        close(ctx->socket_fd);
        ctx->socket_fd = -1;
    }
    
    if (ctx->is_server) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (ctx->clients[i].connected && ctx->clients[i].socket_fd >= 0) {
                close(ctx->clients[i].socket_fd);
                ctx->clients[i].socket_fd = -1;
                ctx->clients[i].connected = false;
            }
        }
    }
}

int dsm_send_msg(int fd, dsm_msg_t *msg)
{
    /* Calculate actual message size based on type */
    size_t msg_size = sizeof(dsm_msg_t) - sizeof(msg->payload);
    
    switch (msg->type) {
        case MSG_PAGE_DATA:
        case MSG_INVALIDATE_ACK:
            msg_size += PAGE_SIZE;
            break;
        case MSG_TASK_ASSIGN:
            msg_size += sizeof(msg->payload.task);
            break;
        case MSG_RESULT:
            msg_size += sizeof(msg->payload.result);
            break;
        default:
            /* No payload needed */
            break;
    }
    
    if (send_all(fd, msg, msg_size) < 0) {
        return -1;
    }
    return 0;
}

int dsm_recv_msg(int fd, dsm_msg_t *msg)
{
    /* First receive header */
    size_t header_size = sizeof(dsm_msg_t) - sizeof(msg->payload);
    
    ssize_t n = recv_all(fd, msg, header_size);
    if (n <= 0) {
        return -1;
    }
    
    /* Then receive payload based on type */
    size_t payload_size = 0;
    
    switch (msg->type) {
        case MSG_PAGE_DATA:
        case MSG_INVALIDATE_ACK:
            payload_size = PAGE_SIZE;
            break;
        case MSG_TASK_ASSIGN:
            payload_size = sizeof(msg->payload.task);
            break;
        case MSG_RESULT:
            payload_size = sizeof(msg->payload.result);
            break;
        default:
            break;
    }
    
    if (payload_size > 0) {
        n = recv_all(fd, &msg->payload, payload_size);
        if (n <= 0) {
            return -1;
        }
    }
    
    return 0;
}

int dsm_accept_client(dsm_context_t *ctx)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    int client_fd = accept(ctx->socket_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
        if (errno != EINTR && errno != EAGAIN) {
            DSM_ERROR("accept failed: %s", strerror(errno));
        }
        return -1;
    }
    
    /* Disable Nagle */
    int opt = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    
    /* Find free slot */
    pthread_mutex_lock(&ctx->clients_lock);
    
    int client_id = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!ctx->clients[i].connected) {
            client_id = i;
            break;
        }
    }
    
    if (client_id < 0) {
        pthread_mutex_unlock(&ctx->clients_lock);
        DSM_ERROR("Maximum clients reached");
        close(client_fd);
        return -1;
    }
    
    ctx->clients[client_id].id = client_id;
    ctx->clients[client_id].socket_fd = client_fd;
    ctx->clients[client_id].connected = true;
    ctx->clients[client_id].task_done = false;
    ctx->num_clients++;
    
    pthread_mutex_unlock(&ctx->clients_lock);
    
    DSM_INFO("Client %d connected from %s:%d",
             client_id,
             inet_ntoa(client_addr.sin_addr),
             ntohs(client_addr.sin_port));
    
    return client_id;
}

/* Thread function to handle messages from a specific client */
void *dsm_client_handler_thread(void *arg)
{
    client_info_t *client = (client_info_t*)arg;
    dsm_context_t *ctx = g_dsm;
    dsm_msg_t msg;
    
    DSM_LOG(ctx, "Handler thread started for client %d", client->id);
    
    /* Wait for initial connect message */
    if (dsm_recv_msg(client->socket_fd, &msg) < 0) {
        DSM_ERROR("Failed to receive from client %d", client->id);
        goto cleanup;
    }
    
    if (msg.type != MSG_CONNECT) {
        DSM_ERROR("Expected CONNECT message, got %d", msg.type);
        goto cleanup;
    }
    
    /* Send acknowledgment */
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_CONNECT_ACK;
    msg.client_id = client->id;
    
    if (dsm_send_msg(client->socket_fd, &msg) < 0) {
        DSM_ERROR("Failed to send CONNECT_ACK to client %d", client->id);
        goto cleanup;
    }
    
    /* Mark client as ready for tasks */
    client->ready = true;
    
    /* Main message loop */
    while (ctx->running && client->connected) {
        if (dsm_recv_msg(client->socket_fd, &msg) < 0) {
            if (ctx->running) {
                DSM_ERROR("Client %d disconnected", client->id);
            }
            break;
        }
        
        switch (msg.type) {
            case MSG_PAGE_REQUEST_READ:
                DSM_LOG(ctx, "Client %d requests page %u for READ", 
                        client->id, msg.page_num);
                dsm_handle_page_request(ctx, client->id, msg.page_num, false);
                break;
                
            case MSG_PAGE_REQUEST_WRITE:
                DSM_LOG(ctx, "Client %d requests page %u for WRITE",
                        client->id, msg.page_num);
                dsm_handle_page_request(ctx, client->id, msg.page_num, true);
                break;
                
            case MSG_INVALIDATE_ACK:
                /* Client returned invalidated page data */
                DSM_LOG(ctx, "Client %d returned page %u data",
                        client->id, msg.page_num);
                /* Copy data back to server's region */
                memcpy(dsm_page_addr(ctx, msg.page_num), 
                       msg.payload.page_data, PAGE_SIZE);
                break;
                
            case MSG_TASK_DONE:
                DSM_LOG(ctx, "Client %d finished task", client->id);
                client->task_done = true;
                break;
                
            case MSG_RESULT:
                DSM_LOG(ctx, "Client %d result: %" PRId64, 
                        client->id, msg.payload.result.value);
                client->result = msg.payload.result.value;
                client->task_done = true;
                break;
                
            default:
                DSM_ERROR("Unknown message type %d from client %d",
                          msg.type, client->id);
        }
    }
    
cleanup:
    pthread_mutex_lock(&ctx->clients_lock);
    client->connected = false;
    if (client->socket_fd >= 0) {
        close(client->socket_fd);
        client->socket_fd = -1;
    }
    ctx->num_clients--;
    pthread_mutex_unlock(&ctx->clients_lock);
    
    DSM_LOG(ctx, "Handler thread ended for client %d", client->id);
    return NULL;
}
