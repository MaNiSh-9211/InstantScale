/*
 * InstantScale — puller.h (Phase 3)
 * Condensed PF-Daemon used by demo_app --resume-lazy: arms nothing itself;
 * the caller creates the userfaultfd + socket and hands them over. Implements
 * batched page requests, a fixed-window sequential prefetch landing in tagged
 * slots, full-scan tag lookup, and self-healing re-requests (all Phase 2
 * correctness lessons applied).
 */
#ifndef IS_PULLER_H
#define IS_PULLER_H

#define PP_MAX_BATCH 32u
#define PP_SLOTS     256u
#define PP_MAX_PEND  512u

typedef struct {
    uint64_t idx;
    uint64_t aligned_addr;
} pp_pend;

typedef struct {
    /* fds */
    int      uffd, sock;

    /* geometry */
    uint8_t *base;
    size_t   ps;
    uint32_t np;

    /* per-page state: 0 idle, 1 requested/in-flight-or-evicted, 2 local */
    uint8_t *state;

    /* prefetch landing slots (tag-scan lookup) */
    uint64_t tag[PP_SLOTS];
    bool     valid[PP_SLOTS];
    uint8_t *slotbuf;               /* PP_SLOTS × ps                     */
    unsigned next_slot;

    /* request batching */
    uint64_t sendq_off[PP_MAX_BATCH];
    unsigned nsend;
    uint64_t last_idx;
    volatile bool shutdown;

    /* faults awaiting data — WITHOUT this, responses would be parked into
     * slots while the application thread slept forever (Phase 2 lesson). */
    pp_pend  pend[PP_MAX_PEND];
    unsigned npend;

    /* outbound staging */
    uint8_t  out[sizeof(is_wire_hdr) + PP_MAX_BATCH * sizeof(is_wire_off)];
    size_t   out_len, out_sent;

    /* inbound reassembly */
    uint8_t *ibuf;
    size_t   icap, ilen;
} pp_ctx;

static void pp_inject(pp_ctx *c, uint64_t dst, const uint8_t *src)
{
    struct uffdio_copy uc = { .dst = dst, .src = (uint64_t)(uintptr_t)src,
                              .len = c->ps };
    for (;;) {
        if (ioctl(c->uffd, UFFDIO_COPY, &uc) == -1) {
            int e = errno;
            if (e == EAGAIN || e == EINTR)
                continue;
            is_die("puller", "ioctl(UFFDIO_COPY)", e);
        }
        if (uc.copy == -EAGAIN)
            continue;
        if ((size_t)uc.copy < c->ps) {
            uc.dst += (uint64_t)uc.copy;
            uc.src += (uint64_t)uc.copy;
            uc.len -= (uint64_t)uc.copy;
            continue;
        }
        break;
    }
}

static int pp_find(pp_ctx *c, uint64_t idx)
{
    for (unsigned s = 0; s < PP_SLOTS; ++s)
        if (c->valid[s] && c->tag[s] == idx)
            return (int)s;
    return -1;
}

static void pp_flush(pp_ctx *c)
{
    while (c->out_sent < c->out_len) {
        ssize_t n = send(c->sock, c->out + c->out_sent,
                         c->out_len - c->out_sent, MSG_NOSIGNAL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            is_die("puller", "send(request)", errno);
        }
        c->out_sent += (size_t)n;
    }
    c->out_len = c->out_sent = 0;
}

static void pp_send_batch(pp_ctx *c)
{
    if (!c->nsend || c->out_len)
        return;
    is_wire_hdr h = { .magic = IS_WIRE_MAGIC, .type = IS_REQ_PAGES,
                      .count = c->nsend };
    memcpy(c->out, &h, sizeof(h));
    memcpy(c->out + sizeof(h), c->sendq_off,
           (size_t)c->nsend * sizeof(is_wire_off));
    c->out_len = sizeof(h) + (size_t)c->nsend * sizeof(is_wire_off);
    c->nsend = 0;
    pp_flush(c);
}

static void pp_process(pp_ctx *c)
{
    for (;;) {
        if (c->ilen < sizeof(is_wire_hdr))
            return;
        is_wire_hdr h;
        memcpy(&h, c->ibuf, sizeof(h));
        if (h.magic != IS_WIRE_MAGIC || h.type != IS_RSP_PAGES ||
            h.count > PP_MAX_BATCH)
            is_die("puller", "bad response", EPROTO);
        size_t need = sizeof(h) +
                      (size_t)h.count * (sizeof(is_wire_page_hdr) + c->ps);
        if (c->ilen < need)
            return;

        uint8_t *p = c->ibuf + sizeof(h);
        for (uint32_t i = 0; i < h.count; ++i) {
            is_wire_page_hdr ph;
            memcpy(&ph, p, sizeof(ph));
            p += sizeof(ph);
            const uint8_t *data = p;
            p += ph.status == IS_PAGE_OK ? c->ps : 0;
            if (ph.status != IS_PAGE_OK)
                is_die("puller", "server page error", EPROTO);

            uint64_t idx = ph.offset / c->ps;

            /* Is an application thread actually blocked on this page? */
            long found = -1;
            for (unsigned j = 0; j < c->npend; ++j)
                if (c->pend[j].idx == idx) { found = (long)j; break; }

            if (found >= 0) {
                uint64_t dst = c->pend[found].aligned_addr;
                c->pend[found] = c->pend[--c->npend]; /* swap-remove */
                pp_inject(c, dst, data);              /* wakes sleeper  */
                c->state[idx] = 2;
            } else {
                /* Prefetch landing: park into tagged slot for later. */
                unsigned s = c->next_slot++ % PP_SLOTS;
                c->tag[s]   = idx;
                c->valid[s] = true;
                memcpy(c->slotbuf + (size_t)s * c->ps, data, c->ps);
            }
        }
        memmove(c->ibuf, c->ibuf + need, c->ilen - need);
        c->ilen -= need;
    }
}

static void pp_handle_faults(pp_ctx *c)
{
    for (;;) {
        struct uffd_msg m;
        ssize_t r = read(c->uffd, &m, sizeof(m));
        if (r == -1 && errno == EAGAIN)
            break;
        if (r != (ssize_t)sizeof(m))
            is_die("puller", "read(uffd_msg)", errno ? errno : EIO);
        if (m.event != UFFD_EVENT_PAGEFAULT)
            continue;

        uint64_t aligned =
            m.arg.pagefault.address & ~(uint64_t)(c->ps - 1);
        uint64_t idx = (aligned - (uint64_t)(uintptr_t)c->base) / c->ps;

        int s = pp_find(c, idx); /* landed prefetch => zero-wait injection */
        if (s >= 0) {
            pp_inject(c, aligned, c->slotbuf + (size_t)s * c->ps);
            c->valid[s]   = false;
            c->state[idx] = 2;
            continue;
        }
        if (c->state[idx] == 0) {
            c->state[idx] = 1;
            if (c->nsend < PP_MAX_BATCH)
                c->sendq_off[c->nsend++] = idx * c->ps;
        } else if (c->state[idx] == 1 && c->nsend < PP_MAX_BATCH) {
            /* Prefetched page evicted before its fault arrived: re-request
             * idempotently rather than wait for a response already consumed. */
            c->sendq_off[c->nsend++] = idx * c->ps;
        }
        if (c->npend == PP_MAX_PEND)
            is_die("puller", "pending overflow", ENOMEM);
        c->pend[c->npend].idx          = idx;
        c->pend[c->npend].aligned_addr = aligned;
        ++c->npend;

        if (idx > c->last_idx)
            c->last_idx = idx;
    }
}

static void *pp_main(void *arg)
{
    pp_ctx *c = arg;
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1)
        is_die("puller", "epoll_create1", errno);

    struct epoll_event ev = { .events = EPOLLIN };
    ev.data.fd = c->uffd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, c->uffd, &ev) == -1)
        is_die("puller", "epoll_ctl(uffd)", errno);
    ev.data.fd = c->sock;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, c->sock, &ev) == -1)
        is_die("puller", "epoll_ctl(sock)", errno);

    while (!c->shutdown) {
        struct epoll_event out[16];
        int n = epoll_wait(epfd, out, 16, -1);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            is_die("puller", "epoll_wait", errno);
        }
        for (int i = 0; i < n && !c->shutdown; ++i) {
            if (out[i].data.fd == c->uffd) {
                pp_handle_faults(c);
            } else {
                for (;;) {
                    ssize_t r = recv(c->sock, c->ibuf + c->ilen,
                                     c->icap - c->ilen, 0);
                    if (r == 0)
                        is_die("puller", "pageserver vanished",
                               ECONNRESET);
                    if (r == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        if (errno == EINTR)
                            continue;
                        is_die("puller", "recv", errno);
                    }
                    c->ilen += (size_t)r;
                }
                pp_process(c);
            }
        }
        /* Fixed-window sequential prefetch. */
        for (uint32_t k = 1; k <= 8; ++k) {
            uint64_t pi = c->last_idx + k;
            if (pi >= c->np || c->state[pi])
                continue;
            c->state[pi] = 1;
            if (c->nsend < PP_MAX_BATCH)
                c->sendq_off[c->nsend++] = pi * c->ps;
        }
        pp_send_batch(c);
        if (c->out_len > c->out_sent)
            pp_flush(c);
    }
    close(epfd);
    return NULL;
}

#endif /* IS_PULLER_H */
