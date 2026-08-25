/*
 * InstantScale — common.h
 * Shared definitions for the Phase 2 split-process prototype:
 *   seeder     -> builds the "checkpointed warm heap" image on disk
 *   pageserver -> TCP server streaming 4 KB pages out of that image
 *   restorer   -> userfaultfd-driven client that activates instantly and
 *                 pulls pages on demand (with batching + prefetch ring)
 *
 * WIRE PROTOCOL (little-endian hosts; both sides assert x86_64-style layout)
 * --------------------------------------------------------------------------
 * Every frame starts with wire_hdr {magic,type,count,pad}.
 *   C->S  META_REQ    count=0                      -> S->C META_RESP + wire_meta
 *   C->S  PAGES_REQ   count=N + N × wire_off       -> S->C PAGES_RESP(count=N)
 *                                                       + N × (wire_page_hdr + data)
 *   C->S  BYE         count=0                      -> server closes
 *
 * Pages are addressed by OFFSET within the logical region, never by absolute
 * virtual address — source and target ASLR layouts are free to differ, which
 * is exactly the situation after a real CRIU restore on another host.
 */
#ifndef IS_COMMON_H
#define IS_COMMON_H

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <arpa/inet.h>         /* inet_pton */
#include <netdb.h>             /* getaddrinfo — resolve hostnames */
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>            /* SIGPIPE disposition */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>            /* offsetof */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <linux/userfaultfd.h>

/* ------------------------------------------------------------------ */
/* Protocol constants                                                  */
/* ------------------------------------------------------------------ */

#define IS_WIRE_MAGIC   0x49535047u          /* "ISPG" InstantScale PaGe */
#define IS_PROTO_VER    1u

enum {
    IS_REQ_META  = 1,      /* client asks for region metadata            */
    IS_REQ_PAGES = 2,      /* client asks for `count` pages by offset    */
    IS_REQ_BYE   = 3,      /* graceful close                             */
    IS_RSP_META  = 101,
    IS_RSP_PAGES = 102,
};

/* Status codes carried per page in PAGES_RESP. */
enum {
    IS_PAGE_OK   = 0,
    IS_PAGE_OOB  = 1,      /* offset beyond region — protocol violation  */
};

#define IS_MAX_BATCH     64u   /* hard cap on pages per request frame     */

/* On-disk checkpoint image ("warm heap snapshot") ------------------------ */
#define IS_IMG_MAGIC     0x4953494Du        /* "ISIM" InstantScale IMage */
#define IS_IMG_VER       1u

typedef struct {
    uint32_t magic;        /* IS_IMG_MAGIC                               */
    uint32_t version;      /* IS_IMG_VER                                 */
    uint32_t page_size;    /* bytes per page (4096 on stock kernels)     */
    uint32_t num_pages;    /* region length = page_size * num_pages      */
    uint64_t region_len;   /* convenience duplicate of the product       */
    uint64_t digest;       /* rolling CRC32 over every page byte in order*/
    char     svc_name[32]; /* human label, e.g. "payments-jvm-warm"      */
} is_img_hdr;              /* 64 bytes, naturally aligned                */

/* Wire structs ------------------------------------------------------------ */

typedef struct {
    uint32_t magic;
    uint32_t type;         /* IS_REQ_* / IS_RSP_*                        */
    uint32_t count;        /* entries following this header              */
    uint32_t pad;
} is_wire_hdr;             /* 16 bytes                                   */

typedef struct {
    uint64_t offset;       /* byte offset of a page inside the region    */
} is_wire_off;             /* 8 bytes                                    */

typedef struct {
    uint64_t region_len;
    uint64_t digest;       /* expected CRC over all page bytes           */
    uint32_t page_size;
    uint32_t num_pages;
} is_wire_meta;            /* 24 bytes                                   */

typedef struct {
    uint64_t offset;       /* echoes the request                         */
    uint32_t status;       /* IS_PAGE_OK / IS_PAGE_OOB                   */
    uint32_t data_len;     /* page_size when status==OK                  */
                            /* followed immediately by `data_len` bytes   */
} is_wire_page_hdr;        /* 16 bytes                                   */

/* ------------------------------------------------------------------ */
/* Tunables (environment-overridable at runtime)                      */
/* ------------------------------------------------------------------ */

#define IS_DEFAULT_PORT      46100
#define IS_DEFAULT_PREFETCH  4u      /* pages ahead to piggyback per fault  */
#define IS_RING_SLOTS        128u    /* prefetch cache capacity in pages    */
#define IS_SERVER_SBUF_CAP   (8u << 20)  /* backpressure kill-switch        */

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

static inline double is_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}
static inline double is_now_ms(void) { return is_now_us() / 1e3; }

/* Fatal error with errno context. Every syscall failure lands here so no
 * error is ever swallowed or misattributed (project constraint). */
static inline void is_die(const char *who, const char *what, int err)
{
    fprintf(stderr, "[FATAL] %s: %s failed: errno=%d (%s)\n",
            who, what, err, strerror(err));
    exit(1);
}

/* Timestamped component logger. */
static inline void is_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    printf("[%9.3f ms] [%-9s] ", is_now_ms(), tag);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* CRC32 (IEEE, reflected) — table built once per process              */
/* Used for end-to-end integrity: seeder computes it over all page     */
/* bytes in index order; restorer recomputes after hydration. Equal    */
/* values prove every bit survived the wire.                           */
/* ------------------------------------------------------------------ */

static uint32_t is_crc_table[256];
static inline void is_crc_init(void)
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        is_crc_table[i] = c;
    }
}
static inline uint32_t is_crc_update(uint32_t crc,
                                     const void *buf, size_t len)
{
    const uint8_t *p = buf;
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = is_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* Deterministic PRNG so "heap contents" are reproducible everywhere:
 * seeder fills, restorer could predict, yet content looks non-trivial. */
static inline uint64_t is_xorshift64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

/* ------------------------------------------------------------------ */
/* Socket helpers                                                      */
/* ------------------------------------------------------------------ */

static inline void is_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl == -1 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) == -1)
        is_die("sock", "fcntl(O_NONBLOCK)", errno);
}

static inline void is_set_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); /* best effort */
}

static inline int is_tcp_listen(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
        is_die("pageserver", "socket(listen)", errno);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(port),
    };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == -1)
        is_die("pageserver", "bind", errno);
    if (listen(fd, 128) == -1)
        is_die("pageserver", "listen", errno);
    is_set_nonblock(fd);
    return fd;
}

/* Connect by hostname or dotted IP. getaddrinfo resolves container/service
 * names (e.g. docker DNS "iscale-src"), which inet_pton cannot do. */
static inline int is_tcp_connect(const char *host, uint16_t port)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char portstr[6];
    snprintf(portstr, sizeof(portstr), "%u", port);

    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0)
        is_die("wire", host, gai == EAI_SYSTEM ? errno : EADDRNOTAVAIL);

    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd == -1)
        is_die("wire", "socket(connect)", errno);
    if (connect(fd, res->ai_addr, res->ai_addrlen) == -1)
        is_die("wire", "connect(pageserver)", errno);
    freeaddrinfo(res);

    is_set_nodelay(fd);
    return fd;
}

/* Blocking-mode exact IO used during the short handshake phase only.
 * EINTR-safe; any other failure is fatal with the true errno. */
static inline void is_send_exact(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            is_die("wire", "send", errno);
        }
        p += n;
        len -= (size_t)n;
    }
}

static inline void is_recv_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        if (n == 0)
            is_die("wire", "recv(EOF mid-frame)", ECONNRESET);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            is_die("wire", "recv", errno);
        }
        p += n;
        len -= (size_t)n;
    }
}

#endif /* IS_COMMON_H */
