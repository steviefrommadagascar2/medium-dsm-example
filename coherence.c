/*
 * DSM - Coherence Protocol Implementation
 * Page state management and transitions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "coherence.h"
#include "memory.h"
#include "network.h"

const char *dsm_state_name(page_state_t state)
{
    switch (state) {
        case PAGE_ABSENT:    return "ABSENT";
        case PAGE_SHARED:    return "SHARED";
        case PAGE_EXCLUSIVE: return "EXCLUSIVE";
        default:             return "UNKNOWN";
    }
}

/*
 * Client-side: Request a page for reading
 */
int dsm_request_page_read(dsm_context_t *ctx, uint32_t page_num)
{
    dsm_msg_t msg, response;
    
    DSM_LOG(ctx, "Requesting page %u for READ", page_num);
    
    /* Send request to server */
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_PAGE_REQUEST_READ;
    msg.page_num = page_num;
    msg.client_id = ctx->client_id;
    
    pthread_mutex_lock(&ctx->fault_lock);
    ctx->pending_page = page_num;
    ctx->page_received = false;
    
    if (dsm_send_msg(ctx->socket_fd, &msg) < 0) {
        DSM_ERROR("Failed to send page read request");
        pthread_mutex_unlock(&ctx->fault_lock);
        return -1;
    }
    
    /* Wait for page data */
    if (dsm_recv_msg(ctx->socket_fd, &response) < 0) {
        DSM_ERROR("Failed to receive page data");
        pthread_mutex_unlock(&ctx->fault_lock);
        return -1;
    }
    
    if (response.type != MSG_PAGE_DATA) {
        DSM_ERROR("Expected PAGE_DATA, got %d", response.type);
        pthread_mutex_unlock(&ctx->fault_lock);
        return -1;
    }
    
    /* Copy data and update protection */
    void *page_addr = dsm_page_addr(ctx, page_num);
    
    /* Temporarily allow write to copy data */
    dsm_protect_page(ctx, page_num, PAGE_EXCLUSIVE);
    memcpy(page_addr, response.payload.page_data, PAGE_SIZE);
    
    /* Set to SHARED (read-only) */
    dsm_protect_page(ctx, page_num, PAGE_SHARED);
    ctx->local_pages[page_num].dirty = false;
    
    ctx->page_received = true;
    pthread_mutex_unlock(&ctx->fault_lock);
    
    DSM_LOG(ctx, "Page %u received and mapped as SHARED", page_num);
    return 0;
}

/*
 * Client-side: Request a page for writing
 */
int dsm_request_page_write(dsm_context_t *ctx, uint32_t page_num)
{
    dsm_msg_t msg, response;
    
    DSM_LOG(ctx, "Requesting page %u for WRITE", page_num);
    
    /* Send request to server */
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_PAGE_REQUEST_WRITE;
    msg.page_num = page_num;
    msg.client_id = ctx->client_id;
    
    pthread_mutex_lock(&ctx->fault_lock);
    ctx->pending_page = page_num;
    ctx->page_received = false;
    
    if (dsm_send_msg(ctx->socket_fd, &msg) < 0) {
        DSM_ERROR("Failed to send page write request");
        pthread_mutex_unlock(&ctx->fault_lock);
        return -1;
    }
    
    /* Wait for page data */
    if (dsm_recv_msg(ctx->socket_fd, &response) < 0) {
        DSM_ERROR("Failed to receive page data");
        pthread_mutex_unlock(&ctx->fault_lock);
        return -1;
    }
    
    if (response.type != MSG_PAGE_DATA) {
        DSM_ERROR("Expected PAGE_DATA, got %d", response.type);
        pthread_mutex_unlock(&ctx->fault_lock);
        return -1;
    }
    
    /* Copy data and set to EXCLUSIVE */
    void *page_addr = dsm_page_addr(ctx, page_num);
    dsm_protect_page(ctx, page_num, PAGE_EXCLUSIVE);
    memcpy(page_addr, response.payload.page_data, PAGE_SIZE);
    
    ctx->local_pages[page_num].dirty = false;
    ctx->page_received = true;
    pthread_mutex_unlock(&ctx->fault_lock);
    
    DSM_LOG(ctx, "Page %u received and mapped as EXCLUSIVE", page_num);
    return 0;
}

/*
 * Server-side: Handle page request from a client
 */
int dsm_handle_page_request(dsm_context_t *ctx, int client_id, 
                            uint32_t page_num, bool for_write)
{
    if (page_num >= DSM_NUM_PAGES) {
        DSM_ERROR("Invalid page number: %u", page_num);
        return -1;
    }
    
    pthread_mutex_lock(&ctx->global_pages[page_num].lock);
    
    global_page_entry_t *gpe = &ctx->global_pages[page_num];
    dsm_msg_t msg;
    
    DSM_LOG(ctx, "Handling page %u request from client %d (write=%d), owner=%d, sharers=0x%x",
            page_num, client_id, for_write, gpe->owner, gpe->sharers);
    
    if (for_write) {
        /* Client wants EXCLUSIVE access */
        
        /* If another client has it exclusive, need to get it back */
        if (gpe->owner >= 0 && gpe->owner != client_id) {
            int current_owner = gpe->owner;
            client_info_t *owner = &ctx->clients[current_owner];
            
            /* Send invalidate to current owner */
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_INVALIDATE;
            msg.page_num = page_num;
            
            DSM_LOG(ctx, "Sending INVALIDATE for page %u to client %d", 
                    page_num, current_owner);
            
            if (dsm_send_msg(owner->socket_fd, &msg) < 0) {
                DSM_ERROR("Failed to send invalidate to client %d", current_owner);
                pthread_mutex_unlock(&ctx->global_pages[page_num].lock);
                return -1;
            }
            
            /* Receive the page data back */
            if (dsm_recv_msg(owner->socket_fd, &msg) < 0) {
                DSM_ERROR("Failed to receive invalidate ack from client %d", current_owner);
                pthread_mutex_unlock(&ctx->global_pages[page_num].lock);
                return -1;
            }
            
            if (msg.type == MSG_INVALIDATE_ACK) {
                /* Copy returned data to server's region */
                memcpy(dsm_page_addr(ctx, page_num), msg.payload.page_data, PAGE_SIZE);
            }
            
            gpe->owner = -1;
        }
        
        /* Invalidate all sharers */
        if (gpe->sharers != 0) {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if ((gpe->sharers & (1 << i)) && i != client_id) {
                    client_info_t *sharer = &ctx->clients[i];
                    if (!sharer->connected) continue;
                    
                    memset(&msg, 0, sizeof(msg));
                    msg.type = MSG_INVALIDATE;
                    msg.page_num = page_num;
                    
                    DSM_LOG(ctx, "Sending INVALIDATE for page %u to sharer %d", 
                            page_num, i);
                    
                    if (dsm_send_msg(sharer->socket_fd, &msg) < 0) {
                        DSM_ERROR("Failed to send invalidate to sharer %d", i);
                    } else {
                        /* Wait for ack (no data needed from read-only copy) */
                        dsm_recv_msg(sharer->socket_fd, &msg);
                    }
                }
            }
            gpe->sharers = 0;
        }
        
        /* Send page to requesting client */
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_PAGE_DATA;
        msg.page_num = page_num;
        memcpy(msg.payload.page_data, dsm_page_addr(ctx, page_num), PAGE_SIZE);
        
        if (dsm_send_msg(ctx->clients[client_id].socket_fd, &msg) < 0) {
            DSM_ERROR("Failed to send page to client %d", client_id);
            pthread_mutex_unlock(&ctx->global_pages[page_num].lock);
            return -1;
        }
        
        /* Update ownership */
        gpe->owner = client_id;
        gpe->version++;
        
        /* Mark page as not present locally on server */
        dsm_protect_page(ctx, page_num, PAGE_ABSENT);
        ctx->local_pages[page_num].state = PAGE_ABSENT;
        
    } else {
        /* Client wants SHARED (read) access */
        
        /* If someone has it exclusive, get the data but let them keep it as shared */
        if (gpe->owner >= 0) {
            int current_owner = gpe->owner;
            client_info_t *owner = &ctx->clients[current_owner];
            
            /* For read request, we could downgrade owner to shared
             * But for simplicity, let's just get a copy */
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_INVALIDATE;  /* Request to flush and downgrade */
            msg.page_num = page_num;
            
            DSM_LOG(ctx, "Requesting page %u data from exclusive owner %d", 
                    page_num, current_owner);
            
            if (dsm_send_msg(owner->socket_fd, &msg) < 0) {
                DSM_ERROR("Failed to request page from owner %d", current_owner);
                pthread_mutex_unlock(&ctx->global_pages[page_num].lock);
                return -1;
            }
            
            if (dsm_recv_msg(owner->socket_fd, &msg) < 0) {
                DSM_ERROR("Failed to receive page from owner %d", current_owner);
                pthread_mutex_unlock(&ctx->global_pages[page_num].lock);
                return -1;
            }
            
            if (msg.type == MSG_INVALIDATE_ACK) {
                memcpy(dsm_page_addr(ctx, page_num), msg.payload.page_data, PAGE_SIZE);
            }
            
            /* Owner no longer exclusive, becomes sharer */
            gpe->sharers |= (1 << current_owner);
            gpe->owner = -1;
        }
        
        /* Send page to client */
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_PAGE_DATA;
        msg.page_num = page_num;
        memcpy(msg.payload.page_data, dsm_page_addr(ctx, page_num), PAGE_SIZE);
        
        if (dsm_send_msg(ctx->clients[client_id].socket_fd, &msg) < 0) {
            DSM_ERROR("Failed to send page to client %d", client_id);
            pthread_mutex_unlock(&ctx->global_pages[page_num].lock);
            return -1;
        }
        
        /* Add to sharers */
        gpe->sharers |= (1 << client_id);
    }
    
    pthread_mutex_unlock(&ctx->global_pages[page_num].lock);
    
    DSM_LOG(ctx, "Page %u request handled, new owner=%d, sharers=0x%x",
            page_num, gpe->owner, gpe->sharers);
    
    return 0;
}

/*
 * Client-side: Handle invalidation request from server
 */
int dsm_handle_invalidate(dsm_context_t *ctx, uint32_t page_num)
{
    dsm_msg_t response;
    
    DSM_LOG(ctx, "Handling INVALIDATE for page %u", page_num);
    
    page_state_t current_state = ctx->local_pages[page_num].state;
    
    memset(&response, 0, sizeof(response));
    response.type = MSG_INVALIDATE_ACK;
    response.page_num = page_num;
    response.client_id = ctx->client_id;
    
    /* If we had the page with write access, send back the data */
    if (current_state == PAGE_EXCLUSIVE) {
        memcpy(response.payload.page_data, dsm_page_addr(ctx, page_num), PAGE_SIZE);
    }
    
    /* Mark page as absent */
    dsm_protect_page(ctx, page_num, PAGE_ABSENT);
    ctx->local_pages[page_num].dirty = false;
    
    /* Send ack with data */
    if (dsm_send_msg(ctx->socket_fd, &response) < 0) {
        DSM_ERROR("Failed to send invalidate ack");
        return -1;
    }
    
    DSM_LOG(ctx, "Page %u invalidated", page_num);
    return 0;
}

/*
 * Update local page with received data
 */
int dsm_handle_page_data(dsm_context_t *ctx, uint32_t page_num, 
                         void *data, page_state_t new_state)
{
    if (page_num >= DSM_NUM_PAGES) {
        DSM_ERROR("Invalid page number: %u", page_num);
        return -1;
    }
    
    void *page_addr = dsm_page_addr(ctx, page_num);
    
    /* Temporarily make writable to copy data */
    dsm_protect_page(ctx, page_num, PAGE_EXCLUSIVE);
    memcpy(page_addr, data, PAGE_SIZE);
    
    /* Set final protection */
    dsm_protect_page(ctx, page_num, new_state);
    ctx->local_pages[page_num].dirty = false;
    
    return 0;
}
