/*
 * InstantScale — pageserver.c
 * The SOURCE-HOST side of the wire: holds the checkpoint image (mmap'd,
 * zero extra copies beyond one memcpy per served page) and streams 4 KB
 * pages to any number of restoring hosts.
 *
 * Design notes
 * ------------
 * - Single-threaded epoll event loop, every fd non-blocking: this exact
 *   shape scales to thousands of concurrent restorers and is the model for
 *   the production PF-Daemon's remote side.
 * - Per-connection receive buffer + frame state machine; send side uses a
 *   growable buffer flushed with partial-write handling (EPOLLOUT armed on
 *   EAGAIN). TCP_NODELAY everywhere — a Nagle-delayed 4 KB page would eat
 *   the entire latency budget.
 * - Requests address pages by OFFSET within the logical region so source
 *   and target virtual layouts can differ freely.
 */
#include "common.h"

/* Per-connection state --------------------------------------------------- */
typedef struct {
    int      fd;
    bool     want_write;         /* EPOLLOUT currently armed?               */
    uint8_t *rbuf;               /* accumulate raw request bytes            */
    size_t   rlen, rcap;
    uint8_t *sbuf;               /* pending response bytes                  */
    size_t   slen, scap, ssent;
    bool     closing;            /* BYE seen — flush then drop              */
} conn_t;

static int    g_epfd;
static void  *g_img;                 /* mmap of the whole .isim file          */
static size_t g_img_len;
static const is_img_hdr *g_hdr;
static const uint8_t *g_pages;       /* first page byte inside the map        */
static uint64_t g_conns_served, g_pages_served, g_bytes_served;

/* -- connection lifecycle ------------------------------------------------ */

static void conn_free(conn_t *c)
{
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c->rbuf);
    free(c->sbuf);
    free(c);
}

static void conn_arm(conn_t *c, bool out)
{
    struct epoll_event ev = {
        .events = (uint32_t)(EPOLLIN | (out ? EPOLLOUT : 0)),
        .data.ptr = c,
    };
    if (epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev) == -1)
        is_die("pageserver", "epoll_ctl(MOD)", errno);
    c->want_write = out;
}

/* -- response construction ----------------------------------------------- */

static void sbuf_reserve(conn_t *c, size_t extra)
{
    if (c->slen + extra <= c->scap)
        return;
    size_t cap = c->scap ? c->scap : (size_t)64 * 1024;
    while (cap < c->slen + extra)
        cap *= 2;
    uint8_t *nb = realloc(c->sbuf, cap);
    if (!nb)
        is_die("pageserver", "realloc(sbuf)", errno);
    c->sbuf = nb;
    c->scap = cap;
}

static inline void sbuf_put(conn_t *c, const void *p, size_t n)
{
    sbuf_reserve(c, n);
    memcpy(c->sbuf + c->slen, p, n);
    c->slen += n;
}

/* Serve one PAGES_REQ: append header + N × (page_hdr | page bytes).
 * Page bytes come straight from the mmap'd image — one copy total. */
static void handle_pages_req(conn_t *c, uint32_t count, const uint8_t *offs_raw)
{
    is_wire_hdr rh = {
        .magic = IS_WIRE_MAGIC,
        .type = IS_RSP_PAGES,
        .count = count,
    };
    sbuf_put(c, &rh, sizeof(rh));

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t off;
        memcpy(&off, offs_raw + (size_t)i * sizeof(is_wire_off),
               sizeof(off)); /* alignment-safe load */

        is_wire_page_hdr ph = { .offset = off, .status = IS_PAGE_OK,
                                .data_len = g_hdr->page_size };
        if (off >= g_hdr->region_len ||
            off % g_hdr->page_size != 0) { /* never trust the peer */
            fprintf(stderr, "[pageserver] OOB offset %" PRIu64 " — dropping\n",
                    off);
            ph.status = IS_PAGE_OOB;
            ph.data_len = 0;
            sbuf_put(c, &ph, sizeof(ph));
            continue;
        }
        const uint8_t *src = g_pages + off;
        sbuf_put(c, &ph, sizeof(ph));
        sbuf_put(c, src, g_hdr->page_size);
        ++g_pages_served;
        g_bytes_served += g_hdr->page_size;
    }
}

/* -- request parsing ------------------------------------------------------ */

static void process_recv(conn_t *c)
{
    for (;;) {
        if (c->rlen < sizeof(is_wire_hdr))
            break; /* need more bytes */

        is_wire_hdr h;
        memcpy(&h, c->rbuf, sizeof(h)); /* alignment-safe */
        if (h.magic != IS_WIRE_MAGIC || h.count > IS_MAX_BATCH) {
            fprintf(stderr, "[pageserver] bad frame (magic/type/count)"
                            " — dropping client\n");
            c->closing = true;
            break;
        }

        /* Bytes needed = header + payload implied by type/count. */
        size_t need = sizeof(h);
        switch (h.type) {
        case IS_REQ_META:
            break;
        case IS_REQ_PAGES:
            need += (size_t)h.count * sizeof(is_wire_off);
            break;
        case IS_REQ_BYE:
            break;
        default:
            fprintf(stderr, "[pageserver] unknown type %u — dropping\n",
                    h.type);
            c->closing = true;
            goto done;
        }
        if (c->rlen < need)
            break; /* partial frame; wait for more */

        uint8_t *body = c->rbuf + sizeof(h);
        switch (h.type) {
        case IS_REQ_META: {
            /* Build wire_meta explicitly — never slice the disk header,
             * its field order differs from the wire struct's. */
            is_wire_meta wm = {
                .region_len = g_hdr->region_len,
                .digest = g_hdr->digest,
                .page_size = g_hdr->page_size,
                .num_pages = g_hdr->num_pages,
            };
            is_wire_hdr mh = { .magic = IS_WIRE_MAGIC, .type = IS_RSP_META,
                               .count = 0 };
            sbuf_put(c, &mh, sizeof(mh));
            sbuf_put(c, &wm, sizeof(wm));
            break;
        }
        case IS_REQ_PAGES:
            handle_pages_req(c, h.count, body);
            break;
        case IS_REQ_BYE:
            c->closing = true;
            break;
        }

        /* Consume this frame; keep any pipelined bytes behind it. */
        memmove(c->rbuf, c->rbuf + need, c->rlen - need);
        c->rlen -= need;
        if (c->closing)
            break;
    }
done:;
}

/* -- flush pending response bytes; returns false if conn was freed ------- */

static bool flush_conn(conn_t *c)
{
    while (c->ssent < c->slen) {
        ssize_t n = send(c->fd, c->sbuf + c->ssent, c->slen - c->ssent,
                         MSG_NOSIGNAL);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                conn_arm(c, true); /* resume when socket drains */
                return true;
            }
            if (errno == EINTR)
                continue;
            conn_free(c);
            return false;
        }
        c->ssent += (size_t)n;
    }
    /* Fully flushed. */
    c->slen = c->ssent = 0;
    if (c->want_write)
        conn_arm(c, false);
    if (c->closing) {
        printf("[%-9s] connection closed after service (conns=%" PRIu64 ")\n",
               "pageserver", ++g_conns_served);
        conn_free(c);
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN); /* dead peers must not kill the daemon */
    is_crc_init();

    uint16_t port = IS_DEFAULT_PORT;
    const char *img_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = (uint16_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--image") && i + 1 < argc)
            img_path = argv[++i];
    }
    if (!img_path) {
        fprintf(stderr, "usage: %s --image warm.isim [--port %u]\n",
                argv[0], IS_DEFAULT_PORT);
        return 2;
    }

    /* Map the checkpoint once; every future page serve reads from this
     * mapping — no pread syscalls, no heap copies, page-cache friendly. */
    int ifd = open(img_path, O_RDONLY);
    if (ifd == -1)
        is_die("pageserver", "open(image)", errno);
    struct stat st;
    if (fstat(ifd, &st) == -1)
        is_die("pageserver", "fstat(image)", errno);
    g_img_len = (size_t)st.st_size;
    g_img = mmap(NULL, g_img_len, PROT_READ, MAP_PRIVATE, ifd, 0);
    if (g_img == MAP_FAILED)
        is_die("pageserver", "mmap(image)", errno);
    close(ifd);

    g_hdr = g_img;
    if (g_hdr->magic != IS_IMG_MAGIC || g_hdr->version != IS_IMG_VER)
        is_die("pageserver", "image magic/version", EINVAL);
    g_pages = (const uint8_t *)g_img + sizeof(is_img_hdr);

    printf("[%-9s] serving %-22s %u pages (%.1f MB) digest=0x%08" PRIx32 "\n",
           "pageserver", g_hdr->svc_name, g_hdr->num_pages,
           (double)g_hdr->region_len / (1024.0 * 1024.0),
           (uint32_t)g_hdr->digest);
    printf("[%-9s] listening on 0.0.0.0:%u\n", "pageserver", port);

    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epfd == -1)
        is_die("pageserver", "epoll_create1", errno);

    int lfd = is_tcp_listen(port);
    /* Tag the listener with a unique address — data.fd and data.ptr share a
     * union, so testing ptr==NULL while storing fd would hand back the fd
     * bits reinterpreted as a pointer (instant segfault on first accept). */
    static char listener_tag;
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = &listener_tag };
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, lfd, &ev) == -1)
        is_die("pageserver", "epoll_ctl(ADD listen)", errno);

    bool running = true;
    (void)running;

    for (;;) {
        struct epoll_event out[32];
        int n = epoll_wait(g_epfd, out, 32, -1);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            is_die("pageserver", "epoll_wait", errno);
        }

        for (int i = 0; i < n; ++i) {
            if (out[i].data.ptr == &listener_tag) { /* listener */
                for (;;) {
                    int cfd = accept(lfd, NULL, NULL);
                    if (cfd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        if (errno == EINTR)
                            continue;
                        is_die("pageserver", "accept", errno);
                    }
                    is_set_nonblock(cfd);
                    is_set_nodelay(cfd);
                    conn_t *c = calloc(1, sizeof(*c));
                    if (!c)
                        is_die("pageserver", "calloc(conn)", errno);
                    c->fd = cfd;
                    c->rcap = 64 * 1024;
                    c->rbuf = malloc(c->rcap);
                    if (!c->rbuf)
                        is_die("pageserver", "malloc(rbuf)", errno);
                    struct epoll_event cev = { .events = EPOLLIN,
                                               .data.ptr = c };
                    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, cfd, &cev) == -1)
                        is_die("pageserver", "epoll_ctl(ADD conn)", errno);
                }
                continue;
            }

            conn_t *c = out[i].data.ptr;

            if (out[i].events & (EPOLLHUP | EPOLLERR)) {
                conn_free(c);
                continue;
            }
            bool alive = true;
            if (out[i].events & EPOLLOUT)
                alive = flush_conn(c); /* may free or re-arm */
            if (!alive)
                continue;

            if (out[i].events & EPOLLIN) {
                for (;;) {
                    if (c->rlen == c->rcap) { /* grow recv buffer */
                        c->rcap *= 2;
                        uint8_t *nb = realloc(c->rbuf, c->rcap);
                        if (!nb)
                            is_die("pageserver", "realloc(rbuf)", errno);
                        c->rbuf = nb;
                    }
                    ssize_t r = recv(c->fd, c->rbuf + c->rlen,
                                     c->rcap - c->rlen, 0);
                    if (r == 0) { /* orderly shutdown by peer */
                        c->closing = true;
                        break;
                    }
                    if (r == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        if (errno == EINTR)
                            continue;
                        conn_free(c);
                        c = NULL;
                        break;
                    }
                    c->rlen += (size_t)r;
                }
                if (c) {
                    process_recv(c);
                    alive = flush_conn(c);
                }
            }
            (void)alive;
        }
    }
}
