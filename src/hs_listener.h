/**
 * hs_listener.h  –  create and manage TCP / UDS listening sockets
 */
#pragma once
#include <stdint.h>

typedef enum { HS_PROTO_TCP = 0, HS_PROTO_UDS = 1 } hs_proto_t;

#define HS_MAX_LISTENERS 2

typedef struct {
    int        fd;
    hs_proto_t proto;
    char       path[108];  /* UDS path (struct sockaddr_un.sun_path max) */
} hs_listener_t;

/**
 * Create a non-blocking TCP listen socket.
 * Returns fd >= 0 on success, -1 on error.
 */
int hs_listener_tcp(hs_listener_t *l, const char *host, uint16_t port,
                    int backlog);

/**
 * Create a non-blocking UDS listen socket.
 * Unlinks any stale socket file first.
 * Returns fd >= 0 on success, -1 on error.
 */
int hs_listener_uds(hs_listener_t *l, const char *path, int backlog);

/** Close the socket; for UDS also unlinks the file. */
void hs_listener_close(hs_listener_t *l);
