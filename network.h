/*
 * DSM - Network Communication Header
 */

#ifndef NETWORK_H
#define NETWORK_H

#include "dsm.h"

/* Initialize server socket and listen */
int dsm_network_init_server(dsm_context_t *ctx);

/* Initialize client and connect to server */
int dsm_network_init_client(dsm_context_t *ctx);

/* Cleanup network resources */
void dsm_network_cleanup(dsm_context_t *ctx);

/* Send a message (blocking) */
int dsm_send_msg(int fd, dsm_msg_t *msg);

/* Receive a message (blocking) */
int dsm_recv_msg(int fd, dsm_msg_t *msg);

/* Server: accept new client connection */
int dsm_accept_client(dsm_context_t *ctx);

/* Server: handle messages from a specific client */
void *dsm_client_handler_thread(void *arg);

#endif /* NETWORK_H */
