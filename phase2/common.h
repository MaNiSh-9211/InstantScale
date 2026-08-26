/*
 * HotPod ??? common.h
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
 *   C->S  PAGES_REQ   count=N + N ?? wire_off       -> S->C PAGES_RESP(count=N)
 *                                                       + N ?? (wire_page_hdr + data)
 *   C->S  BYE         count=0                      -> server closes
 *
 * Pages are addressed by OFFSET within the logical region, never by absolute
 * virtual address ??? source and target ASLR layouts are free to differ, which
 * is exactly the situation after a real CRIU restore on another host.
 */
#ifndef IS_COMMON_H
#define IS_COMMON_H

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <arpa/inet.h>         /* inet_pton */
#include <netdb.h>             /* getaddrinfo ??? resolve hostnames */
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

#define IS_WIRE_MAGIC   0x49535047u          /* "ISPG" HotPod PaGe */
#define IS_PROTO_VER    1u

enum {
    IS_REQ_META  = 1,      /* client asks for region metadata            */
    IS_REQ_PAGES = 2,      /* client asks for `count` pages by offset    */
    IS_REQ_BYE   = 3,      /* graceful close                             */
    IS_REQ_AUTH  = 4,      /* PSK challenge: 8B nonce + 32B HMAC         */
    IS_RSP_META  = 101,
    IS_RSP_PAGES = 102,
    IS_RSP_AUTH  = 103,    /* server proof: 8B nonce + 32B HMAC          */
};

/* Status codes carried per page in PAGES_RESP. */
enum {
    IS_PAGE_OK   = 0,
    IS_PAGE_OOB  = 1,      /* offset beyond region ? protocol violation  */
    IS_PAGE_ZERO = 2,      /* all-zero page: install via UFFDIO_ZEROPAGE */
};

#define IS_MAX_BATCH     64u   /* hard cap on pages per request frame     */

/* On-disk checkpoint image ("warm heap snapshot") ------------------------ */
#define IS_IMG_MAGIC     0x4953494Du        /* "ISIM" InstantScale IMage */
#define IS_IMG_VER       2u   /* v2: per-page zero-flags after page data */

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
/* CRC32 (IEEE, reflected) ??? table built once per process              */
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
 * names (e.g. docker DNS "hotpod-src"), which inet_pton cannot do. */
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

/* Same, but EOF is reported as 0 instead of dying ? lets auth distinguish
 * "server said no" from transport faults. */
static inline size_t is_recv_exact_or_eof(int fd, void *buf, size_t len)
{
    size_t got = 0;
    uint8_t *p = buf;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        if (n == 0)
            return got;
        if (n == -1) {
            if (errno == EINTR)
                continue;
            is_die("wire", "recv", errno);
        }
        p += n; got += (size_t)n; len -= (size_t)n;
    }
    return got;
}

/* ------------------------------------------------------------------ */
/* SHA-256 + HMAC (FIPS 180-4 / RFC 2104) ? dependency-free, used for  */
/* production PSK authentication of migration sessions.                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t h[8];
    uint64_t total_len;
    uint8_t  buf[64];
    size_t   buflen;
} is_sha256;

static const uint32_t IS_K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline uint32_t is_ror(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static inline void is_sha256_init(is_sha256 *s)
{
    static const uint32_t iv[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    memcpy(s->h, iv, sizeof(iv));
    s->total_len = 0;
    s->buflen = 0;
}

static inline void is_sha256_block(is_sha256 *s, const uint8_t p[64])
{
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = is_ror(w[i-15],7) ^ is_ror(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = is_ror(w[i-2],17) ^ is_ror(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=s->h[0]; b=s->h[1]; c=s->h[2]; d=s->h[3];
    e=s->h[4]; f=s->h[5]; g=s->h[6]; h=s->h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = is_ror(e,6) ^ is_ror(e,11) ^ is_ror(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + IS_K256[i] + w[i];
        uint32_t S0 = is_ror(a,2) ^ is_ror(a,13) ^ is_ror(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static inline void is_sha256_update(is_sha256 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->total_len += len;
    while (len) {
        size_t take = 64 - s->buflen;
        if (take > len) take = len;
        memcpy(s->buf + s->buflen, p, take);
        s->buflen += take; p += take; len -= take;
        if (s->buflen == 64) {
            is_sha256_block(s, s->buf);
            s->buflen = 0;
        }
    }
}

static inline void is_sha256_final(is_sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->total_len * 8;
    uint8_t pad = 0x80;
    is_sha256_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->buflen != 56)
        is_sha256_update(s, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; ++i)
        lenb[i] = (uint8_t)(bits >> (56 - i * 8));
    memcpy(s->buf + 56, lenb, 8);
    is_sha256_block(s, s->buf);
    for (int i = 0; i < 8; ++i) {
        out[i*4]   = (uint8_t)(s->h[i] >> 24);
        out[i*4+1] = (uint8_t)(s->h[i] >> 16);
        out[i*4+2] = (uint8_t)(s->h[i] >> 8);
        out[i*4+3] = (uint8_t)(s->h[i]);
    }
}

/* HMAC-SHA256(key, msg) -> 32B tag */
static inline void is_hmac_sha256(const uint8_t *key, size_t keylen,
                                  const uint8_t *msg, size_t msglen,
                                  uint8_t out[32])
{
    uint8_t k[64], ipad[64], opad[64], khash[32], inner[32];
    memset(k, 0, sizeof(k));
    if (keylen > 64) {
        is_sha256 s; is_sha256_init(&s);
        is_sha256_update(&s, key, keylen);
        is_sha256_final(&s, khash);
        memcpy(k, khash, 32);
    } else {
        memcpy(k, key, keylen);
    }
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    is_sha256 s; is_sha256_init(&s);
    is_sha256_update(&s, ipad, 64);
    is_sha256_update(&s, msg, msglen);
    is_sha256_final(&s, inner);
    is_sha256_init(&s);
    is_sha256_update(&s, opad, 64);
    is_sha256_update(&s, inner, 32);
    is_sha256_final(&s, out);
}

/* Constant-time tag comparison (never leaks match position). */
static inline int is_ct_memcmp(const uint8_t *a, const uint8_t *b, size_t n)
{
    volatile uint8_t d = 0;
    for (size_t i = 0; i < n; ++i)
        d |= a[i] ^ b[i];
    return d != 0;
}

/* Random 8B nonce from the OS; falls back to clock entropy in sandboxes. */
static inline uint64_t is_rand64(void)
{
    uint64_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd != -1) {
        if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
            v = 0;
        close(fd);
    }
    if (v == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        v = ((uint64_t)ts.tv_nsec << 17) ^ (uint64_t)ts.tv_sec ^ 0xD1B54A32D192ED03ull;
    }
    return v;
}

/* Read first line of a token file (trimmed). Returns malloc'd string. */
static inline char *is_read_token_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd == -1)
        return NULL;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return NULL;
    buf[n] = '\0';
    char *nl = strpbrk(buf, "\r\n");
    if (nl)
        *nl = '\0';
    if (!buf[0])
        return NULL;
    char *out = malloc(strlen(buf) + 1);
    if (out)
        strcpy(out, buf);
    return out;
}

/* ------------------------------------------------------------------ */
/* PSK session auth (challenge-response, both directions)              */
/*   client -> AUTH: nonce_c(8) || HMAC(token, nonce_c || "C")         */
/*   server -> AUTH: nonce_s(8) || HMAC(token, nonce_s || "S")         */
/* A server configured with a token drops any pre-AUTH frame; a client */
/* with a token must auth before META. Mismatch = instant drop.        */
/* ------------------------------------------------------------------ */

#define IS_AUTH_NONCE 8
#define IS_AUTH_TAGLEN 32
#define IS_AUTH_PAYLOAD (IS_AUTH_NONCE + IS_AUTH_TAGLEN)

static inline void is_auth_hmac(const char *token, uint64_t nonce,
                                const char *role, uint8_t tag[32])
{
    uint8_t msg[IS_AUTH_NONCE + 1];
    for (int i = 0; i < IS_AUTH_NONCE; ++i)
        msg[i] = (uint8_t)(nonce >> (8 * i));
    msg[IS_AUTH_NONCE] = (uint8_t)role[0];
    is_hmac_sha256((const uint8_t *)token, strlen(token),
                   msg, sizeof(msg), tag);
}

/* Client side: perform the exchange on a blocking socket. Fatal on any
 * mismatch ? a wrong token must never silently proceed. */
static inline void is_client_auth(int fd, const char *token)
{
    uint64_t nc = is_rand64();
    uint8_t tag[32];
    is_auth_hmac(token, nc, "C", tag);

    is_wire_hdr h = { .magic = IS_WIRE_MAGIC, .type = IS_REQ_AUTH,
                      .count = 0 };
    uint8_t frame[sizeof(h) + IS_AUTH_PAYLOAD];
    memcpy(frame, &h, sizeof(h));
    memcpy(frame + sizeof(h), &nc, IS_AUTH_NONCE);
    memcpy(frame + sizeof(h) + IS_AUTH_NONCE, tag, IS_AUTH_TAGLEN);
    is_send_exact(fd, frame, sizeof(frame));

    uint8_t rh[sizeof(is_wire_hdr)];
    is_recv_exact(fd, rh, sizeof(rh));
    memcpy(&h, rh, sizeof(h));
    if (h.magic != IS_WIRE_MAGIC || h.type != IS_RSP_AUTH)
        is_die("auth", "server rejected token (bad AUTH response)", EACCES);

    /* Rejection = header-only frame then close. */
    uint8_t rs[IS_AUTH_NONCE + IS_AUTH_TAGLEN];
    if (is_recv_exact_or_eof(fd, rs, sizeof(rs)) != sizeof(rs))
        is_die("auth", "token rejected by pageserver", EACCES);

    uint64_t ns;
    memcpy(&ns, rs, IS_AUTH_NONCE);
    uint8_t expect[32];
    is_auth_hmac(token, ns, "S", expect);
    if (is_ct_memcmp(expect, rs + IS_AUTH_NONCE, 32) != 0)
        is_die("auth", "server proof mismatch", EACCES);
}

/* Server side: verify one AUTH frame payload. 0 = ok, EACCES = reject. */
static inline int is_server_verify_auth(const char *token,
                                        const uint8_t *payload)
{
    uint64_t nc;
    memcpy(&nc, payload, IS_AUTH_NONCE);
    uint8_t expect[32];
    is_auth_hmac(token, nc, "C", expect);
    return is_ct_memcmp(expect, payload + IS_AUTH_NONCE, 32) ? EACCES : 0;
}


/* ------------------------------------------------------------------ */
/* Zero-page elision (v2 images): real heaps are 30-60% zero pages.    */
/* Zero pages travel as a status flag (no 4KB payload) and install     */
/* with UFFDIO_ZEROPAGE - bandwidth and memory both win.               */
/* ------------------------------------------------------------------ */

static inline size_t is_flags_offset(const is_img_hdr *h)
{
    return sizeof(is_img_hdr) + h->region_len; /* np bytes, one per page */
}

/* Install `len` bytes of ZERO pages at `dst` (dst must be page-aligned).
 * Returns 0 on success, -1 with errno set on hard failure. */
static inline int is_uffdio_zeropage(int uffd, uint64_t dst, size_t len)
{
    struct uffdio_zeropage z;
    memset(&z, 0, sizeof(z));
    z.range.start = dst;
    z.range.len = len;
    for (;;) {
        if (ioctl(uffd, UFFDIO_ZEROPAGE, &z) == -1) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            return -1;
        }
        return 0;
    }
}

#endif /* IS_COMMON_H */
