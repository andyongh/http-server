/**
 * hs_listener.c  –  TCP and UDS listening socket helpers
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "hs_listener.h"
#include "hs_log.h"

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

/* ── helpers ─────────────────────────────────────────────────────────────── */
static int set_cloexec(int fd)
{
    int f = fcntl(fd, F_GETFD, 0);
    if (f == -1) return -1;
    return fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

static int set_nonblocking(int fd)
{
    int f = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

/* ── TCP ──────────────────────────────────────────────────────────────────── */
int hs_listener_tcp(hs_listener_t *l, const char *host, uint16_t port,
                    int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { hs_log(HS_LOG_ERROR, "socket(TCP): %s", strerror(errno)); return -1; }
    set_cloexec(fd);

    /* SO_REUSEADDR: allow immediate rebind after restart */
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    /* SO_REUSEPORT: multiple listeners on same port (kernel load-balances) */
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    set_nonblocking(fd);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    const char *h = (host && *host) ? host : "0.0.0.0";
    if (inet_pton(AF_INET, h, &addr.sin_addr) <= 0) {
        hs_log(HS_LOG_ERROR, "hs_listener_tcp: bad host \"%s\"", h);
        close(fd); return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        hs_log(HS_LOG_ERROR, "bind(TCP): %s", strerror(errno)); close(fd); return -1;
    }
    if (listen(fd, backlog) < 0) {
        hs_log(HS_LOG_ERROR, "listen(TCP): %s", strerror(errno)); close(fd); return -1;
    }

    l->fd    = fd;
    l->proto = HS_PROTO_TCP;
    l->path[0] = '\0';
    return fd;
}

/* ── UDS ──────────────────────────────────────────────────────────────────── */
int hs_listener_uds(hs_listener_t *l, const char *path, int backlog)
{
    if (!path || !*path) {
        hs_log(HS_LOG_ERROR, "hs_listener_uds: empty path");
        return -1;
    }

    /* Remove stale socket file */
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { hs_log(HS_LOG_ERROR, "socket(UDS): %s", strerror(errno)); return -1; }
    set_cloexec(fd);

    set_nonblocking(fd);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        hs_log(HS_LOG_ERROR, "hs_listener_uds: path too long: %s", path);
        close(fd); return -1;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        hs_log(HS_LOG_ERROR, "bind(UDS): %s", strerror(errno)); close(fd); return -1;
    }
    if (listen(fd, backlog) < 0) {
        hs_log(HS_LOG_ERROR, "listen(UDS): %s", strerror(errno)); close(fd); return -1;
    }

    l->fd    = fd;
    l->proto = HS_PROTO_UDS;
    strncpy(l->path, path, sizeof(l->path) - 1);
    l->path[sizeof(l->path)-1] = '\0';
    return fd;
}

/* ── close ────────────────────────────────────────────────────────────────── */
void hs_listener_close(hs_listener_t *l)
{
    if (l->fd < 0) return;
    close(l->fd);
    l->fd = -1;
    if (l->proto == HS_PROTO_UDS && l->path[0])
        unlink(l->path);
}
