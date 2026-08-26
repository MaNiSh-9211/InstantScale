/*
 * HotPod â€” restorer.c
 * The TARGET-HOST side: activates a "restored" process instantly by mapping an
 * EMPTY memory region, arming userfaultfd(MISSING), and pulling every page
 * over TCP exactly when (and only when) execution touches it.
 *
 *   LAZY mode (default)                      EAGER mode (IS_EAGER=1)
 *   -------------------                      -----------------------
 *   handshake (~KB metadata)                 handshake (~KB metadata)
 *   register uffd                            copy EVERY page first
 *   ACTIVATE: process runs immediately       ACTIVATE: only now may it run
 *   faults stream pages behind execution     (classic migration bottleneck)
 *
 * Both modes verify the same rolling CRC32 digest at the end, so correctness
 * is proven identically â€” only the time-to-activation differs, dramatically.
 *
 * Inventions on top of plain demand paging (all measurable via SUMMARY line):
 *   1. Request batching      â€” multiple missing pages leave in one frame.
 *   2. Prefetch piggybacking â€” touching page N silently requests N+1..N+K,
 *                              so sequential access never waits on the wire.
 *   3. Ring page cache       â€” prefetched pages land in a lock-free ring;
 *                              the fault is served from RAM with ZERO RTT
 *                              (counted in `hits`).
 *
 * Usage:
 *   ./restorer [--host IP] [--port N]
 *   IS_EAGER=1 IS_PREFETCH=4 IS_STRIDE=1 ./restorer ...
 */
#include "common.h"
#include <pthread.h>           /* PF-Daemon thread */

/* Temporary trace facility: IS_DBG=1 ./restorer â€¦ (dev only) */
static int DBGv;
#define DBG(...) do { if (DBGv) fprintf(stderr, __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/* Page lifecycle states                                               */
/* ------------------------------------------------------------------ */
#define ST_IDLE   0u   /* untouched; no request outstanding            */
#define ST_REQ    1u   /* requested (fault-driven or prefetch)         */
#define ST_LOCAL  2u   /* mapped locally via UFFDIO_COPY / eager copy  */

#define MAX_PEND          512u  /* faults awaiting data (bounded queue)  */
#define EAGER_CHUNK       32u   /* pages per eager-mode request          */

typedef struct {
    uint64_t idx;
    uint64_t aligned_addr;
    double   t_fault_us;         /* when the fault was enqueued          */
} pend_t;

typedef struct {
    /* fds */
    int      uffd;               /* userfaultfd (lazy mode only, else -1)*/
    int      sock;               /* connection to pageserver             */
    int      stop_efd;           /* shutdown channel (main pokes once)   */
    int      epfd;

    /* geometry */
    uint8_t *base;
    size_t   ps;
    uint32_t np;

    uint8_t *state;              /* np Ã— ST_*                            */

    /* prefetch ring cache */
    unsigned   ring_slots;
    uint64_t  *ring_tag;
    bool      *ring_valid;
    uint8_t   *ring_buf;         /* ring_slots Ã— ps bytes                */
    unsigned   ring_next;

    uint32_t   prefetch_k;

    /* pending faults + outgoing batch */
    pend_t      pend[MAX_PEND];
    unsigned    npend;
    is_wire_off sendq[IS_MAX_BATCH];
    unsigned    nsend;
    uint64_t    last_fault_idx;

    /* partial outbound frame (request) buffer */
    uint8_t     out[sizeof(is_wire_hdr) +
                    IS_MAX_BATCH * sizeof(is_wire_off)];
    size_t      out_len, out_sent;

    /* inbound reassembly buffer */
    uint8_t    *ibuf;
    size_t      icap, ilen;

    /* statistics (daemon writes; main reads after join) */
    long  st_net_served, st_cache_hits, st_prefetch_issued, st_batches;
    long  st_zeropages;           /* pages installed via UFFDIO_ZEROPAGE  */
    long  hist[9];                /* fault-service latency buckets        */

    /* adaptive prefetch lookahead */
    uint32_t k_cur;               /* effective lookahead (grows/shrinks)  */
    long     adj_hits, adj_misses; /* window since last adaptation        */

    /* speculative run-merging: one UFFDIO_COPY installs a contiguous run
     * (waiter's page + already-fetched successors) â€” kills per-page faults */
    uint8_t *merge_buf;           /* IS_MAX_BATCH Ã— ps                    */
    long     st_merge_runs, st_merged_pages;
} rctx_t;

/* Latency histogram edges in microseconds. Bucket i counts samples in
 * [edge[i-1], edge[i]); last bucket is open-ended. */
static const double LAT_EDGES[] = { 50, 100, 250, 500, 1000, 5000, 20000 };
#define LAT_BUCKETS 8u

/* ------------------------------------------------------------------ */
/* Daemon helpers                                                      */
/* ------------------------------------------------------------------ */

/* Atomically install `len` bytes (one or more whole pages) at `dst` and wake
 * every thread blocked anywhere inside that range. Same retry semantics as a
 * single-page copy: O_NONBLOCK uffds surface EAGAIN; short copies resume. */
static void inject_range(rctx_t *c, uint64_t dst, const uint8_t *src,
                         size_t len)
{
    struct uffdio_copy uc = {
        .dst = dst,
        .src = (uint64_t)(uintptr_t)src,
        .len = len,
        .mode = 0,
    };
    for (;;) {
        if (ioctl(c->uffd, UFFDIO_COPY, &uc) == -1) {
            int e = errno;
            if (e == EAGAIN || e == EINTR)
                continue;
            is_die("daemon", "ioctl(UFFDIO_COPY)", e);
        }
        if (uc.copy == -EAGAIN)
            continue;
        if ((size_t)uc.copy < len) {
            uc.dst += uc.copy;
            uc.src += uc.copy;
            uc.len -= uc.copy;
            continue;
        }
        break;
    }
}

/* Single-page convenience wrapper. */
static void inject_page(rctx_t *c, uint64_t dst, const uint8_t *src)
{
    inject_range(c, dst, src, c->ps);
}

static void lat_record(rctx_t *c, double us)
{
    unsigned b = 0;
    while (b < LAT_BUCKETS - 1 && us >= LAT_EDGES[b])
        ++b;
    ++c->hist[b];
}

/* Ring lookup: full tag scan. Placement uses a FIFO cursor while faults
 * arrive in arbitrary order, so probing by index arithmetic can miss pages
 * that ARE cached â€” and a miss on an already-consumed prefetch means
 * waiting forever for a response that will never re-arrive (deadlock).
 * 128 validity checks cost ~nothing next to a network round trip. */
static inline int ring_lookup(rctx_t *c, uint64_t idx)
{
    for (unsigned s = 0; s < c->ring_slots; ++s)
        if (c->ring_valid[s] && c->ring_tag[s] == idx)
            return (int)s;
    return -1;
}

static inline void ring_put(rctx_t *c, uint64_t idx, const uint8_t *data)
{
    unsigned s = c->ring_next++ % c->ring_slots;
    c->ring_tag[s] = idx;
    c->ring_valid[s] = true;
    memcpy(c->ring_buf + (size_t)s * c->ps, data, c->ps);
}

/* Try to push any staged request frame; keep remainder on EAGAIN. */
static void try_flush_out(rctx_t *c)
{
    while (c->out_sent < c->out_len) {
        ssize_t n = send(c->sock, c->out + c->out_sent,
                         c->out_len - c->out_sent, MSG_NOSIGNAL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return; /* caller keeps EPOLLOUT armed */
            is_die("daemon", "send(request)", errno);
        }
        c->out_sent += (size_t)n;
    }
    c->out_len = c->out_sent = 0;
}

/* Build one PAGES_REQ frame from the current sendq and stage it. */
static void send_batch(rctx_t *c)
{
    if (c->nsend == 0)
        return;
    is_wire_hdr h = { .magic = IS_WIRE_MAGIC, .type = IS_REQ_PAGES,
                      .count = c->nsend };
    memcpy(c->out, &h, sizeof(h));
    memcpy(c->out + sizeof(h), c->sendq,
           (size_t)c->nsend * sizeof(is_wire_off));
    c->out_len = c->out_sent = 0;
    c->out_len = sizeof(h) + (size_t)c->nsend * sizeof(is_wire_off);
    ++c->st_batches;
    c->nsend = 0;
    DBG("batch: staging %zu bytes (first off=%llu)\n", c->out_len,
        (unsigned long long)c->sendq[0].offset);
    try_flush_out(c);
    DBG("batch: %s\n",
        c->out_len == 0 ? "fully sent" : "partial (resume on EPOLLOUT)");
}

static long pend_find(rctx_t *c, uint64_t idx)
{
    for (unsigned j = 0; j < c->npend; ++j)
        if (c->pend[j].idx == idx)
            return (long)j;
    return -1;
}

/* Parse every complete response frame currently buffered, then install pages
 * using SPECULATIVE RUN-MERGING: consecutive offsets (the waiter's page plus
 * already-fetched prefetch successors) are staged contiguously and installed
 * with ONE ranged UFFDIO_COPY. Future touches of those successors find memory
 * PRESENT â€” no fault, no syscall, no RTT. Batched hydration, invented here so
 * sequential sweeps cost ~1 syscall per K pages instead of per page. */
static void process_responses(rctx_t *c)
{
    static __thread uint64_t       r_idx[IS_MAX_BATCH];
    static __thread const uint8_t *r_dat[IS_MAX_BATCH];
    static __thread uint8_t        r_zero[IS_MAX_BATCH];

    for (;;) {
        if (c->ilen < sizeof(is_wire_hdr))
            return;
        is_wire_hdr h;
        memcpy(&h, c->ibuf, sizeof(h));
        if (h.magic != IS_WIRE_MAGIC || h.type != IS_RSP_PAGES ||
            h.count > IS_MAX_BATCH)
            is_die("daemon", "bad response header", EPROTO);

        /* Walk the frame incrementally: zero pages carry NO payload, so the
         * total length is only known entry-by-entry (a fixed computation
         * would wait forever on bytes the server never sends). */
        uint32_t n = 0;
        size_t pos = sizeof(h);
        while (n < h.count) {
            if (c->ilen < pos + sizeof(is_wire_page_hdr))
                return; /* need more bytes */
            is_wire_page_hdr ph;
            memcpy(&ph, c->ibuf + pos, sizeof(ph));
            if (ph.status != IS_PAGE_OK && ph.status != IS_PAGE_ZERO)
                is_die("daemon", "server reported page error", EPROTO);
            if (ph.offset % c->ps != 0 ||
                ph.offset >= (uint64_t)c->np * c->ps)
                is_die("daemon", "response offset out of range", EPROTO);
            size_t el = sizeof(ph) +
                        (ph.status == IS_PAGE_OK ? c->ps : 0);
            if (c->ilen < pos + el)
                return; /* payload split across recvs */
            r_idx[n] = ph.offset / c->ps;
            r_dat[n] = c->ibuf + pos + sizeof(ph);
            r_zero[n] = (ph.status == IS_PAGE_ZERO);
            ++n;
            pos += el;
        }
        /* r_dat[] points into ibuf; consumption happens only AFTER the
         * installs below have copied everything out. */

        /* Group into maximal consecutive runs and resolve each run. */
        uint32_t i = 0;
        while (i < n) {
            uint32_t j = i;
            while (j + 1 < n && r_idx[j + 1] == r_idx[j] + 1)
                ++j;

            long w = -1; /* first blocked thread inside this run */
            for (uint32_t k = i; k <= j && w < 0; ++k)
                w = pend_find(c, r_idx[k]);

            if (w < 0) {
                /* Pure-prefetch run: zero pages install instantly
                 * (UFFDIO_ZEROPAGE needs no fault); data pages park. */
                for (uint32_t k = i; k <= j; ++k) {
                    if (r_zero[k]) {
                        uint64_t d =
                            (uint64_t)(uintptr_t)(c->base + r_idx[k] * c->ps);
                        if (is_uffdio_zeropage(c->uffd, d, c->ps) == -1 &&
                            errno != EEXIST)
                            is_die("daemon", "UFFDIO_ZEROPAGE", errno);
                        c->state[r_idx[k]] = ST_LOCAL;
                        ++c->st_zeropages;
                    } else {
                        ring_put(c, r_idx[k], r_dat[k]);
                    }
                }
                i = j + 1;
                continue;
            }

            uint64_t dst0 = (uint64_t)(uintptr_t)(c->base + r_idx[i] * c->ps);
            (void)dst0;

            /* Install the run in kind-homogeneous sub-runs: contiguous DATA
             * pages merge into one ranged COPY; contiguous ZERO pages merge
             * into one ranged UFFDIO_ZEROPAGE (no payload, no memory). */
            uint32_t k = i;
            while (k <= j) {
                uint8_t zk = r_zero[k];
                uint32_t e = k;
                while (e + 1 <= j && r_zero[e + 1] == zk)
                    ++e;

                uint64_t d = (uint64_t)(uintptr_t)(c->base + r_idx[k] * c->ps);
                size_t bytes = (size_t)(e - k + 1) * c->ps;
                if (zk) {
                    if (is_uffdio_zeropage(c->uffd, d, bytes) == -1 &&
                        errno != EEXIST)
                        is_die("daemon", "UFFDIO_ZEROPAGE", errno);
                    c->st_zeropages += (long)(e - k + 1);
                } else if (e > k) {
                    for (uint32_t q = k; q <= e; ++q)
                        memcpy(c->merge_buf + (size_t)(q - k) * c->ps,
                               r_dat[q], c->ps);
                    inject_range(c, d, c->merge_buf, bytes);
                    ++c->st_merge_runs;
                    c->st_merged_pages += (long)(e - k + 1);
                } else {
                    inject_range(c, d, r_dat[k], c->ps);
                }
                k = e + 1;
            }

            /* Resolve waiters + mark the whole run present. Speculatively
             * installed pages count as prefetch "hits" â€” that IS the signal
             * the adaptive lookahead needs to keep growing. */
            double first_lat = -1.0;
            long   waiters = 0;
            for (uint32_t k = i; k <= j; ++k) {
                long f = pend_find(c, r_idx[k]);
                if (f >= 0) {
                    if (first_lat < 0)
                        first_lat = is_now_us() - c->pend[f].t_fault_us;
                    c->pend[f] = c->pend[--c->npend]; /* swap-remove */
                    ++c->st_net_served;
                    ++waiters;
                }
                c->state[r_idx[k]] = ST_LOCAL;
                if (r_idx[k] > c->last_fault_idx)
                    c->last_fault_idx = r_idx[k];
            }
            if ((long)(j - i + 1) > waiters)
                c->adj_hits += (long)(j - i + 1) - waiters;
            if (first_lat >= 0)
                lat_record(c, first_lat);
            i = j + 1;
        }

        /* All installs copied out of ibuf — safe to consume the frame now. */
        memmove(c->ibuf, c->ibuf + pos, c->ilen - pos);
        c->ilen -= pos;
    }
}

/* ------------------------------------------------------------------ */
/* The PF-Daemon thread: epoll over {uffd, socket, stop-eventfd}       */
/* ------------------------------------------------------------------ */

static void *daemon_main(void *arg)
{
    rctx_t *c = arg;

    c->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (c->epfd == -1)
        is_die("daemon", "epoll_create1", errno);

    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = c->uffd;
    if (epoll_ctl(c->epfd, EPOLL_CTL_ADD, c->uffd, &ev) == -1)
        is_die("daemon", "epoll_ctl(uffd)", errno);
    DBG("daemon up: epfd=%d uffd=%d sock=%d stop=%d\n",
        c->epfd, c->uffd, c->sock, c->stop_efd);
    ev.data.fd = c->sock;
    if (epoll_ctl(c->epfd, EPOLL_CTL_ADD, c->sock, &ev) == -1)
        is_die("daemon", "epoll_ctl(sock)", errno);
    ev.data.fd = c->stop_efd;
    if (epoll_ctl(c->epfd, EPOLL_CTL_ADD, c->stop_efd, &ev) == -1)
        is_die("daemon", "epoll_ctl(stop)", errno);

    bool shutdown_req = false;
    while (!shutdown_req) {
        struct epoll_event out[16];
        int n = epoll_wait(c->epfd, out, 16, -1);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            is_die("daemon", "epoll_wait", errno);
        }
        for (int i = 0; i < n; ++i)
            DBG("epoll: n=%d fd=%d ev=%x\n", n, out[i].data.fd,
                out[i].events);

        for (int i = 0; i < n && !shutdown_req; ++i) {
            int fd = out[i].data.fd;

            if (fd == c->stop_efd) { /* graceful teardown signal */
                uint64_t one;
                if (read(c->stop_efd, &one, sizeof(one)) == -1)
                    is_die("daemon", "read(stop)", errno);
                shutdown_req = true;
                break;
            }

            if (fd == c->uffd && (out[i].events & (EPOLLIN | EPOLLERR))) {
                /* Drain every queued fault message. */
                for (;;) {
                    struct uffd_msg m;
                    ssize_t r = read(c->uffd, &m, sizeof(m));
                    if (r == -1 && errno == EAGAIN)
                        break;
                    if (r != (ssize_t)sizeof(m))
                        is_die("daemon", "read(uffd_msg)",
                               errno ? errno : EIO);
                    if (m.event != UFFD_EVENT_PAGEFAULT)
                        continue;
                    DBG("uffd msg: addr=%llx\n",
                        (unsigned long long)m.arg.pagefault.address);

                    uint64_t addr = m.arg.pagefault.address;
                    uint64_t aligned = addr & ~(uint64_t)(c->ps - 1);
                    uint64_t idx = (aligned - (uint64_t)(uintptr_t)c->base)
                                   / c->ps;

                    int s = ring_lookup(c, idx);
                    if (s >= 0) { /* prefetch paid off: zero network RTT */
                        inject_page(c, aligned,
                                    c->ring_buf + (size_t)s * c->ps);
                        c->ring_valid[s] = false;
                        c->state[idx] = ST_LOCAL;
                        ++c->st_cache_hits;
                        ++c->adj_hits;
                    } else {
                        if (c->npend == MAX_PEND)
                            is_die("daemon", "pending queue overflow",
                                   ENOMEM);
                        c->pend[c->npend].idx = idx;
                        c->pend[c->npend].aligned_addr = aligned;
                        c->pend[c->npend].t_fault_us = is_now_us();
                        ++c->npend;
                        ++c->adj_misses;
                        if (c->state[idx] == ST_IDLE) {
                            c->state[idx] = ST_REQ;
                            if (c->nsend < IS_MAX_BATCH) {
                                c->sendq[c->nsend].offset =
                                    idx * c->ps;
                                ++c->nsend;
                            }
                        } else if (c->state[idx] == ST_REQ) {
                            /* Marked requested earlier (prefetch), yet the
                             * data is neither here nor cached â€” the ring
                             * evicted it before this fault arrived. Re-
                             * request idempotently instead of waiting for
                             * a response that will never come. Pages are
                             * immutable in this model, so a duplicate
                             * response is harmless (it lands in the ring). */
                            if (c->nsend < IS_MAX_BATCH) {
                                c->sendq[c->nsend].offset =
                                    idx * c->ps;
                                ++c->nsend;
                            }
                        }
                        if (idx > c->last_fault_idx)
                            c->last_fault_idx = idx;
                    }
                }
            }

            if (fd == c->sock && (out[i].events & (EPOLLIN | EPOLLERR))) {
                for (;;) {
                    if (c->ilen == c->icap)
                        is_die("daemon", "response buffer overflow",
                               EPROTO);
                    ssize_t r = recv(c->sock, c->ibuf + c->ilen,
                                     c->icap - c->ilen, 0);
                    if (r == 0)
                        is_die("daemon", "pageserver vanished", ECONNRESET);
                    if (r == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        if (errno == EINTR)
                            continue;
                        is_die("daemon", "recv(response)", errno);
                    }
                    c->ilen += (size_t)r;
                }
                DBG("sock: ilen=%zu\n", c->ilen);
                process_responses(c);
            }
        }

        if (shutdown_req)
            break;

        /* Adaptive prefetch: grow the lookahead while the miss rate stays
         * low (sequential streams), halve it when random access dominates.
         * Every K-th touch is inherently a boundary miss, so we compare the
         * observed rate against that floor rather than demanding zero. */
        {
            long tot = c->adj_hits + c->adj_misses;
            if (tot >= 32) {
                uint32_t next = c->k_cur;
                if ((uint64_t)c->adj_misses * 4 <= (uint64_t)tot &&
                    c->k_cur < 32)
                    next = c->k_cur * 2;      /* <=25% misses: widen   */
                else if ((uint64_t)c->adj_misses * 2 > (uint64_t)tot &&
                         c->k_cur > 1)
                    next = c->k_cur / 2;      /* >50% misses: narrow   */
                c->k_cur = next;
                c->adj_hits = c->adj_misses = 0;
            }
        }

        /* Opportunistic prefetch: piggyback the K successors of the highest
         * touched page onto whatever batch is forming. Sequential access â€”
         * the dominant heap pattern â€” then NEVER waits on the wire.
         * k_cur adapts between 1 and 32 based on observed hit rate. */
        for (uint32_t k = 1; k <= c->k_cur; ++k) {
            uint64_t pi = c->last_fault_idx + k;
            if (pi >= c->np)
                break;
            if (c->state[pi] == ST_IDLE) {
                c->state[pi] = ST_REQ;
                if (c->nsend < IS_MAX_BATCH) {
                    c->sendq[c->nsend].offset = pi * c->ps;
                    ++c->nsend;
                    ++c->st_prefetch_issued;
                }
            }
        }
        if (c->nsend > 0 && c->out_len == 0)
            send_batch(c);
        else if (c->out_len > c->out_sent)
            try_flush_out(c);
    }

    close(c->epfd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Eager mode: the traditional approach â€” copy everything, THEN run    */
/* ------------------------------------------------------------------ */

static void eager_fetch_all(int sock, uint8_t *base, size_t ps, uint32_t np,
                            double *transfer_ms)
{
    uint8_t req[sizeof(is_wire_hdr) + EAGER_CHUNK * sizeof(is_wire_off)];
    uint8_t rxs[sizeof(is_wire_hdr)];
    uint8_t rxp[sizeof(is_wire_page_hdr)];

    double t0 = is_now_us();
    uint32_t done = 0;
    while (done < np) {
        uint32_t cnt = np - done > EAGER_CHUNK ? EAGER_CHUNK : np - done;

        is_wire_hdr h = { .magic = IS_WIRE_MAGIC, .type = IS_REQ_PAGES,
                          .count = cnt };
        memcpy(req, &h, sizeof(h));
        for (uint32_t i = 0; i < cnt; ++i) {
            is_wire_off o = { .offset = (uint64_t)(done + i) * ps };
            memcpy(req + sizeof(h) + (size_t)i * sizeof(o), &o, sizeof(o));
        }
        is_send_exact(sock, req, sizeof(h) + (size_t)cnt * sizeof(is_wire_off));

        is_recv_exact(sock, rxs, sizeof(rxs));
        is_wire_hdr rh;
        memcpy(&rh, rxs, sizeof(rh));
        if (rh.magic != IS_WIRE_MAGIC || rh.type != IS_RSP_PAGES ||
            rh.count != cnt)
            is_die("eager", "bad response header", EPROTO);

        for (uint32_t i = 0; i < cnt; ++i) {
            is_recv_exact(sock, rxp, sizeof(rxp));
            is_wire_page_hdr ph;
            memcpy(&ph, rxp, sizeof(ph));
            if (ph.status != IS_PAGE_OK)
                is_die("eager", "page error from server", EPROTO);
            /* Straight into its final address â€” no bounce buffer. */
            is_recv_exact(sock, base + ph.offset, ps);
        }
        done += cnt;
    }
    *transfer_ms = (is_now_us() - t0) / 1e3;
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static void print_hist(const long *hist)
{
    static const char *lbl[LAT_BUCKETS] = {
        "<50us", "50-100us", "100-250us", "250-500us",
        "0.5-1ms", "1-5ms", "5-20ms", ">20ms"
    };
    printf("  fault-service latency histogram:\n");
    for (unsigned b = 0; b < LAT_BUCKETS; ++b)
        printf("    %-10s %8ld\n", lbl[b], hist[b]);
}

static unsigned long env_ul(const char *k, unsigned long dflt)
{
    const char *v = getenv(k);
    return v ? strtoul(v, NULL, 10) : dflt;
}

static uint32_t gcd_u32(uint32_t a, uint32_t b)
{
    while (b) { uint32_t t = a % b; a = b; b = t; }
    return a;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    is_crc_init();
    DBGv = getenv("IS_DBG") != NULL;

    const char *host = "127.0.0.1";
    uint16_t port = (uint16_t)env_ul("IS_PORT", IS_DEFAULT_PORT);
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc)
            host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = (uint16_t)strtoul(argv[++i], NULL, 10);
    }
    bool     eager    = env_ul("IS_EAGER", 0) != 0;
    uint32_t prefetch = (uint32_t)env_ul("IS_PREFETCH", IS_DEFAULT_PREFETCH);
    uint32_t stride   = (uint32_t)env_ul("IS_STRIDE", 1);

    double t0 = is_now_us();

    /* ---- handshake: the ONLY thing standing between nothing and RUNNING */
    int sock = is_tcp_connect(host, port);

    const char *tok = getenv("HOTPOD_TOKEN");
    const char *tokfile = NULL;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--token-file"))
            tokfile = argv[i + 1];
    if (tokfile) {
        tok = is_read_token_file(tokfile);
        if (!tok)
            is_die("restorer", "read token file", errno);
    }
    if (tok)
        is_client_auth(sock, tok); /* fatal on mismatch — never proceed */

    is_wire_hdr q = { .magic = IS_WIRE_MAGIC, .type = IS_REQ_META, .count = 0 };
    is_send_exact(sock, &q, sizeof(q));

    /* A tokenless client talking to a PSK server gets the AUTH-rejection
     * marker instead of META — translate it into an actionable error. */
    uint8_t rh0[sizeof(is_wire_hdr)];
    is_recv_exact(sock, rh0, sizeof(rh0));
    is_wire_hdr h;
    memcpy(&h, rh0, sizeof(h));
    if (h.magic != IS_WIRE_MAGIC)
        is_die("restorer", "bad frame magic from pageserver", EPROTO);
    if (h.type == IS_RSP_AUTH)
        is_die("restorer",
               "pageserver requires a token (set HOTPOD_TOKEN or --token-file)",
               EACCES);
    if (h.type != IS_RSP_META)
        is_die("restorer", "unexpected first response", EPROTO);

    uint8_t rm[sizeof(is_wire_meta)];
    is_recv_exact(sock, rm, sizeof(rm));
    is_wire_meta meta;
    memcpy(&meta, rm, sizeof(meta));

    size_t   ps  = meta.page_size;
    uint32_t np  = meta.num_pages;
    if (meta.region_len != (uint64_t)np * ps || ps == 0 || (ps & (ps - 1)))
        is_die("restorer", "inconsistent metadata", EPROTO);

    printf("[%-9s] target region     : %u pages Ã— %zu B (%.1f MB)"
           " digest=0x%08" PRIx32 "\n", "restorer", np, ps,
           (double)meta.region_len / (1024.0 * 1024.0),
           (uint32_t)meta.digest);

    /* ---- map the EMPTY region: valid VMAs, zero backing pages --------- */
    uint8_t *base = mmap(NULL, meta.region_len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        is_die("restorer", "mmap(region)", errno);

    pthread_t tid;
    rctx_t    ctx;
    memset(&ctx, 0, sizeof(ctx));

    double eager_transfer_ms = 0.0;
    if (!eager) {
        /* Arm userfaultfd BEFORE anything can touch the region. */
        int uffd = (int)syscall(__NR_userfaultfd,
                                (unsigned long)O_CLOEXEC |
                                (unsigned long)O_NONBLOCK);
        if (uffd == -1) {
            int e = errno;
            if (e == EPERM)
                fprintf(stderr, "hint: sysctl vm.unprivileged_userfaultfd=1 "
                                "(or privileged container)\n");
            is_die("restorer", "syscall(__NR_userfaultfd)", e);
        }
        struct uffdio_api api = { .api = UFFD_API };
        if (ioctl(uffd, UFFDIO_API, &api) == -1)
            is_die("restorer", "ioctl(UFFDIO_API)", errno);
        struct uffdio_register reg = {
            .range = { .start = (uint64_t)(uintptr_t)base,
                       .len = meta.region_len },
            .mode = UFFDIO_REGISTER_MODE_MISSING,
        };
        if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1)
            is_die("restorer", "ioctl(UFFDIO_REGISTER)", errno);
        if (!(reg.ioctls & (1ULL << _UFFDIO_COPY)))
            is_die("restorer", "UFFDIO_COPY unsupported", EINVAL);

        /* Bring up the PF-Daemon before the first touch can ever happen. */
        int stop_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (stop_efd == -1)
            is_die("restorer", "eventfd", errno);

        ctx.sock = sock;
        ctx.uffd = uffd;
        ctx.stop_efd = stop_efd;
        ctx.base = base;
        ctx.ps = ps;
        ctx.np = np;
        ctx.state = calloc(np, 1);
        ctx.ring_slots = IS_RING_SLOTS;
        ctx.ring_tag = calloc(IS_RING_SLOTS, sizeof(uint64_t));
        ctx.ring_valid = calloc(IS_RING_SLOTS, sizeof(bool));
        ctx.ring_buf = malloc((size_t)IS_RING_SLOTS * ps);
        ctx.icap = sizeof(is_wire_hdr) +
                   (size_t)IS_MAX_BATCH * (sizeof(is_wire_page_hdr) + ps);
        ctx.ibuf = malloc(ctx.icap);
        ctx.merge_buf = malloc((size_t)IS_MAX_BATCH * ps);
        ctx.prefetch_k = prefetch;
        ctx.k_cur = prefetch ? prefetch : 1;
        if (!ctx.state || !ctx.ring_tag || !ctx.ring_valid ||
            !ctx.ring_buf || !ctx.ibuf || !ctx.merge_buf)
            is_die("restorer", "alloc(daemon structures)", ENOMEM);

        is_set_nonblock(sock); /* handshake was blocking; hot path is not */
        if (pthread_create(&tid, NULL, daemon_main, &ctx) != 0)
            is_die("restorer", "pthread_create(daemon)", errno);
    } else {
        eager_fetch_all(sock, base, ps, np, &eager_transfer_ms);
    }

    /* ==================== THE ACTIVATION POINT ======================== */
    double activation_ms = (is_now_us() - t0) / 1e3;
    printf("[%-9s] STATE: RUNNING     after %.3f ms  (%s)\n", "restorer",
           activation_ms,
           eager ? "but ALL pages had to arrive first"
                 : "with 0% of heap pages transferred");

    /* ---- visit every page (stride permutation covers all indices) ----- */
    if (stride == 0 || gcd_u32(np, stride) != 1)
        stride = 1;

    double first_fault_ms = -1.0;
    volatile uint64_t sink = 0;
    for (uint32_t k = 0; k < np; ++k) {
        uint32_t idx = (uint32_t)(((uint64_t)k * stride) % np);
        double ta = is_now_us();
        uint64_t v = *(volatile uint64_t *)(base + (size_t)idx * ps);
        if (k == 0)
            first_fault_ms = (is_now_us() - ta) / 1e3;
        sink += v; /* keep the read real; checked below via CRC anyway */
    }
    (void)sink;
    double swept_ms = (is_now_us() - t0) / 1e3;

    /* ---- end-to-end integrity: recompute the seeder's rolling CRC ----- */
    uint32_t crc = 0;
    for (uint32_t i = 0; i < np; ++i)
        crc = is_crc_update(crc, base + (size_t)i * ps, ps);
    bool digest_ok = (crc == (uint32_t)meta.digest);
    double total_ms = (is_now_us() - t0) / 1e3;

    if (!digest_ok)
        is_die("restorer", "digest mismatch â€” memory corrupted in transit",
               EPROTO);

    /* ---- teardown: stop daemon FIRST, then BYE, then close ------------ */
    /* Hydration throughput: LAZY streams during the sweep; EAGER already
     * paid everything pre-start, so report its bulk-transfer rate. */
    double hydrate_secs = eager
        ? eager_transfer_ms / 1e3
        : (swept_ms - activation_ms) / 1e3;
    double mbps = (double)meta.region_len / (1024.0 * 1024.0) /
                  (hydrate_secs > 0 ? hydrate_secs : 1e-9);
    long hits = ctx.st_cache_hits, net = ctx.st_net_served;
    long pref = ctx.st_prefetch_issued, bat = ctx.st_batches;
    long mruns = ctx.st_merge_runs, mpages = ctx.st_merged_pages;
    long zpages = ctx.st_zeropages;

    if (!eager) {
        uint64_t one = 1;
        if (write(ctx.stop_efd, &one, sizeof(one)) == -1)
            is_die("restorer", "write(stop_efd)", errno);
        pthread_join(tid, NULL);
    }
    is_wire_hdr bye = { .magic = IS_WIRE_MAGIC, .type = IS_REQ_BYE };
    is_send_exact(sock, &bye, sizeof(bye));
    close(sock);
    if (!eager) {
        close(ctx.uffd);
        close(ctx.stop_efd);
    }

    /* ---- report -------------------------------------------------------- */
    printf("\n=== RESTORATION REPORT (%s) ===\n", eager ? "EAGER" : "LAZY");
    printf("  activation time            : %10.3f ms\n", activation_ms);
    if (eager)
        printf("  bulk transfer (pre-start)  : %10.3f ms\n",
               eager_transfer_ms);
    printf("  first-touch resolution     : %10.3f ms\n", first_fault_ms);
    printf("  full sweep complete        : %10.3f ms\n", swept_ms);
    printf("  hydration throughput       : %10.1f MB/s\n", mbps);
    if (!eager) {
        printf("  faults served from network : %ld\n", net);
        printf("  served from prefetch cache : %ld  (RTT eliminated)\n",
               hits);
        printf("  speculative merged installs: %ld pages in %ld runs"
               " (single-syscall hydration)\n", mpages, mruns);
        printf("  zero pages elided           : %ld  (UFFDIO_ZEROPAGE,"
               " no payload)\n", zpages);
        printf("  prefetched requests issued : %ld in %ld batches\n",
               pref, bat);
        print_hist(ctx.hist);
    }
    printf("  digest                     :  OK (0x%08" PRIx32 ")\n", crc);

    /* Machine-readable summary for demo.sh comparisons */
    printf("SUMMARY mode=%s activate_ms=%.3f ready_ms=%.3f total_ms=%.3f"
           " pages=%u net=%ld hits=%ld merged=%ld merge_runs=%ld zeros=%ld"
           " hydrate_mbps=%.1f\n",
           eager ? "eager" : "lazy", activation_ms, swept_ms, total_ms,
           np, net, hits, mpages, mruns, zpages, mbps);
    return 0;
}
