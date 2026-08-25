/*
 * HotPod â€” Phase 1 MVP: single-process self-faulting prototype
 * ==================================================================
 *
 * WHAT THIS PROVES
 * ----------------
 * That a process can keep RUNNING while its own memory is "not there yet":
 *   1. We mmap a region and populate it with known data (the "warm heap").
 *   2. We arm userfaultfd(MISSING) on that region â€” the kernel now owes us
 *      a notification every time a *not-present* page is touched.
 *   3. We wipe the physical pages with madvise(MADV_DONTNEED). The virtual
 *      addresses remain valid; the backing RAM is gone. This is exactly the
 *      state a target host is in right after receiving only a checkpoint
 *      "skeleton" (registers + page-table metadata, no bulk pages).
 *   4. The main thread keeps executing instantly (activation != hydration).
 *      The FIRST touch of each wiped page traps into the kernel, which wakes
 *      our background PF-daemon thread via epoll on the userfaultfd.
 *   5. The daemon fabricates the page (in production: fetched over the wire
 *      from the source host) and installs it atomically with
 *      ioctl(UFFDIO_COPY). The trapped thread resumes as if nothing happened
 *      â€” no SIGSEGV, no manual signal handling, pure kernel cooperation.
 *
 * THE TWO THREADS
 * ---------------
 *   [main]     plays the restored application: runs, reads memory.
 *   [handler]  plays the PF-Daemon: epoll()s the uffd, serves pages.
 *
 * They communicate ONLY through the userfaultfd file descriptor:
 *   kernel -> handler : struct uffd_msg records (event=PAGEFAULT, address)
 *   handler -> kernel : ioctl(uffd, UFFDIO_COPY) installs the page and wakes
 *                       the exact thread that was sleeping on that fault.
 *
 * BUILD:  make            (Linux, kernel >= 5.11 recommended)
 * RUN:    ./uffd_selffault
 *         IS_SIM_LATENCY_US=500 ./uffd_selffault   # fake network RTT per page
 */

#define _GNU_SOURCE /* for syscall(), MAP_ANONYMOUS, etc. */

#include <errno.h>              /* errno, EXXX */
#include <fcntl.h>              /* O_CLOEXEC, O_NONBLOCK */
#include <inttypes.h>           /* PRIu64 fixed-width printf */
#include <linux/userfaultfd.h>  /* uffd_msg, uffdio_api/register/copy, ioctls */
#include <pthread.h>            /* background PF-daemon thread */
#include <stdatomic.h>          /* atomic_bool shutdown handshake */
#include <stdint.h>             /* uint64_t, uintptr_t */
#include <stdbool.h>            /* bool for the atomic shutdown flag   */
#include <stdio.h>              /* printf, fprintf, perror */
#include <stdlib.h>             /* strtol, abort */
#include <string.h>             /* memset, memcpy, strerror_r */
#include <sys/epoll.h>          /* epoll_create1, epoll_wait â€” async fault pump */
#include <sys/eventfd.h>        /* eventfd â€” race-free shutdown wakeup          */
#include <sys/ioctl.h>          /* ioctl(UFFDIO_*) */
#include <sys/mman.h>           /* mmap, madvise, MADV_DONTNEED */
#include <sys/syscall.h>        /* __NR_userfaultfd */
#include <time.h>               /* clock_gettime(CLOCK_MONOTONIC) for metrics */
#include <unistd.h>             /* read, close, sysconf */

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#ifndef NUM_PAGES
#define NUM_PAGES 8 /* heap size in pages; keep small, output stays readable */
#endif

#define MAX_EPOLL_EVENTS 16    /* batch size for one epoll_wait wakeup       */
#define PATTERN_BYTE 0xA5      /* filler byte proving real data was copied   */

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

/* Milliseconds since an arbitrary fixed point (CLOCK_MONOTONIC never jumps),
 * as double so sub-millisecond activation numbers are visible. */
static double now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        abort();
    }
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* Fatal error helper: prints "<who>: <what>: <errno string>" and dies.
 * Every syscall failure funnels through here with its raw errno preserved â€”
 * granular errors are a hard project constraint. */
static void die(const char *who, const char *what, int err)
{
    fprintf(stderr, "[FATAL] %s: %s failed: errno=%d (%s)\n",
            who, what, err, strerror(err));
    exit(1);
}

/* State shared between main thread and the PF-daemon thread. Plain ints are
 * fine where only one side writes before the other reads in an ordered way,
 * but the shutdown flag is genuinely concurrent => atomic. */
struct pf_daemon_ctx {
    int uffd_fd;                 /* the userfaultfd we epoll on                */
    int stop_efd;                /* eventfd: main pokes it to end the daemon   */
    const void *region_base;     /* start of the tracked mmap region           */
    size_t region_len;           /* total tracked bytes                        */
    size_t page_size;            /* sysconf(_SC_PAGESIZE), assumed power-of-2  */
    long sim_latency_us;         /* artificial per-page "network" delay (demo) */
    atomic_bool shutdown;        /* set by nobody today; HUP-driven exit       */
    atomic_long faults_served;   /* statistics: number of pages injected       */
};

/* ------------------------------------------------------------------ */
/* THE PF-DAEMON THREAD                                                */
/* ------------------------------------------------------------------ */

/* Serve ONE page fault:
 *   1. build the authoritative 64-byte header + filler body,
 *   2. (optionally) sleep to emulate fetching it over the network,
 *   3. install it atomically with ioctl(UFFDIO_COPY).
 * In Phase 3+, step 2 becomes a real REQ(addr)->PAGEDATA(4K) round trip. */
static void serve_page_fault(struct pf_daemon_ctx *ctx, uint64_t fault_addr,
                             uint64_t fault_flags)
{
    size_t ps = ctx->page_size;

    /* The kernel reports the exact byte that missed; UFFDIO_COPY works on
     * whole pages, so snap DOWN to the page boundary. Pure integer math on
     * uintptr_t â€” no pointer arithmetic games. */
    uintptr_t page = (uintptr_t)fault_addr & ~(uintptr_t)(ps - 1);

    /* Which slot inside the tracked region does this page belong to?
     * Used to regenerate the SAME value main originally stored there, which
     * lets us verify correctness deterministically (counter == index + 1). */
    uintptr_t off = page - (uintptr_t)ctx->region_base;
    uint64_t counter_value = off / ps + 1;

    /* Staging buffer for the "network-received" page. Aligned to the system
     * page size because UFFDIO_COPY copies from here straight into user
     * PTEs; alignment costs nothing and keeps DMA-style paths happy later. */
    void *staging = aligned_alloc(ps, ps);
    if (!staging)
        die("pf_daemon", "aligned_alloc(staging)", errno);

    /* Fabricate the payload = what the SOURCE HOST would send:
     * word 0: the original counter; words 1..: recognizable pattern. */
    memset(staging, PATTERN_BYTE, ps);
    *(uint64_t *)staging = counter_value;

    /* Emulated wire latency so activation-vs-hydration costs are visible. */
    if (ctx->sim_latency_us > 0) {
        struct timespec req = {
            .tv_sec = 0,
            .tv_nsec = (long)ctx->sim_latency_us * 1000L,
        };
        nanosleep(&req, NULL); /* best effort; demo-only */
    }

    /* --- THE INJECTION: ioctl(uffd, UFFDIO_COPY, &uffdio_copy) ---------
     *   .dst  : TARGET page inside the process (page-aligned, registered)
     *   .src  : our staging buffer holding the full page image
     *   .len  : exactly one page (must match registration granularity)
     *   .mode : 0 = wake the waiting thread after installing the PTE
     *   .copy : OUT â€” bytes actually copied, OR -errno on failure
     *
     * The kernel atomically maps the page and wakes EVERY thread blocked on
     * this specific missing page. From main's point of view the load simply
     * completes. No signals, no polling, no races. */
    struct uffdio_copy uc = {
        .dst = (uint64_t)page,
        .src = (uint64_t)(uintptr_t)staging,
        .len = ps,
        .mode = 0,
        .copy = 0,
    };

    /* Retry loop: with an O_NONBLOCK userfaultfd, UFFDIO_COPY can legally
     * return -EAGAIN if it would have to block (e.g. page being migrated).
     * Partial progress (.copy > 0 but < len) is retried at the advanced dst. */
    for (;;) {
        if (ioctl(ctx->uffd_fd, UFFDIO_COPY, &uc) == -1) {
            int e = errno;
            if (e == EAGAIN || e == EINTR)
                continue; /* transient â€” spin (bounded in real daemons) */
            free(staging);
            die("pf_daemon", "ioctl(UFFDIO_COPY)", e);
        }
        if (uc.copy == -EAGAIN) /* documented alternate failure channel */
            continue;
        if ((size_t)uc.copy < ps) { /* short copy: advance and finish it */
            uc.dst += uc.copy;
            uc.src += uc.copy;
            uc.len -= (uint64_t)((size_t)uc.copy);
            continue;
        }
        break;
    }

    atomic_fetch_add(&ctx->faults_served, 1);
    printf("  [pf-daemon] fault  @0x%016" PRIx64 " (flags=0x%" PRIx64 ")"
           " -> served page %2" PRIu64 "/%zu via UFFDIO_COPY (%zu bytes)\n",
           fault_addr, fault_flags, counter_value,
           ctx->region_len / ps, ps);
    free(staging);
}

/* Thread entry: owns an epoll instance watching the userfaultfd.
 * Why epoll instead of blocking read()? Production PF-Daemons multiplex MANY
 * fds (sockets to page servers, timers, control plane) into one event loop;
 * the uffd is just another EPOLLIN source. This mirrors that shape early. */
static void *pf_daemon_main(void *arg)
{
    struct pf_daemon_ctx *ctx = arg;

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1)
        die("pf_daemon", "epoll_create1", errno);

    /* Register interest: uffd becomes readable whenever an event record
     * (struct uffd_msg) is queued by the kernel. Level-triggered is correct
     * here: we keep reading until EAGAIN drains the queue. The eventfd is
     * the shutdown channel: main writes a counter to it, we wake up and
     * leave WITHOUT anyone ever closing an fd under our feet (no EBADF
     * races like naive close-based teardown). */
    struct epoll_event ev_uffd = { .events = EPOLLIN,
                                   .data.fd = ctx->uffd_fd };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, ctx->uffd_fd, &ev_uffd) == -1)
        die("pf_daemon", "epoll_ctl(ADD uffd)", errno);

    struct epoll_event ev_stop = { .events = EPOLLIN,
                                   .data.fd = ctx->stop_efd };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, ctx->stop_efd, &ev_stop) == -1)
        die("pf_daemon", "epoll_ctl(ADD stop_efd)", errno);

    bool shutting_down = false;
    for (;;) {
        struct epoll_event out[MAX_EPOLL_EVENTS];
        int n = epoll_wait(epfd, out, MAX_EPOLL_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR)
                continue; /* signal interruption is benign */
            die("pf_daemon", "epoll_wait", errno);
        }

        for (int i = 0; i < n; ++i) {
            int fd = out[i].data.fd;

            /* Shutdown path: main wrote to the stop eventfd. */
            if (fd == ctx->stop_efd) {
                uint64_t one;
                if (read(ctx->stop_efd, &one, sizeof(one)) == -1)
                    die("pf_daemon", "read(stop_efd)", errno);
                shutting_down = true;
                break;
            }

            if (!(out[i].events & EPOLLIN))
                continue;

            /* Drain every queued message (non-blocking fd => EAGAIN ends it). */
            for (;;) {
                struct uffd_msg msg;
                ssize_t r = read(fd, &msg, sizeof(msg));

                if (r == -1 && errno == EAGAIN)
                    break; /* queue empty â€” back to epoll */

                if (r != (ssize_t)sizeof(msg))
                    die("pf_daemon", "read(uffd_msg)", errno == 0 ? EIO : errno);

                /* Only MISSING-mode faults are possible in this prototype;
                 * the switch documents how other modes would arrive. */
                switch (msg.event) {
                case UFFD_EVENT_PAGEFAULT:
                    serve_page_fault(ctx,
                                     msg.arg.pagefault.address,
                                     msg.arg.pagefault.flags);
                    break;
                case UFFD_EVENT_FORK:   /* future: inherit registrations  */
                case UFFD_EVENT_REMAP:  /* future: track mremap moves     */
                case UFFD_EVENT_REMOVE: /* future: forget unmapped ranges */
                    fprintf(stderr, "[pf-daemon] unhandled event %u\n",
                            msg.event);
                    break;
                default:
                    fprintf(stderr, "[pf-daemon] unknown event %u\n",
                            msg.event);
                }
            }
        }

        if (shutting_down)
            break; /* leave the epoll loop; fds are owned by main now */
    }

    close(epfd); /* only our epoll instance â€” uffd/eventfd belong to main */
    printf("  [pf-daemon] shutdown signal received -> exiting cleanly\n");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* MAIN â€” plays the restored application                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    double t_wipe_done, t_first_read_issued, t_first_read_done;

    size_t ps = (size_t)sysconf(_SC_PAGESIZE); /* never hardcode 4096 */
    size_t region_len = NUM_PAGES * ps;

    printf("=== HotPod Phase 1 MVP â€” self-faulting prototype ===\n");
    printf("[main] page size         : %zu bytes\n", ps);
    printf("[main] heap simulation   : %zu pages, %zu bytes total\n",
           (size_t)NUM_PAGES, region_len);

    /* STEP 1 â€” allocate the "heap". MAP_ANONYMOUS|MAP_PRIVATE gives zeroed
     * demand-paged memory, exactly what a runtime's heap looks like to the
     * kernel. Virtual range reserved; physical pages appear lazily. */
    void *region = mmap(NULL, region_len, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED)
        die("main", "mmap(heap)", errno);
    printf("[main] mmap region       : %p .. %p\n",
           region, (char *)region + region_len - 1);

    /* Warm it: page i stores counter (i+1) in word 0 + pattern elsewhere.
     * This is the state a CRIU checkpoint would have captured on Host A. */
    for (size_t i = 0; i < NUM_PAGES; ++i) {
        char *page = (char *)region + i * ps;
        memset(page, PATTERN_BYTE, ps);
        *(uint64_t *)page = (uint64_t)i + 1;
    }
    printf("[main] seeded warm state : page[i] word0 == i+1\n");

    /* STEP 2 â€” acquire the userfaultfd. There is no glibc wrapper; this is a
     * raw syscall. O_CLOEXEC: fds must never leak across exec. O_NONBLOCK:
     * event reads and UFFDIO_COPY never wedge the daemon thread. */
    int uffd = (int)syscall(__NR_userfaultfd, (unsigned long)O_CLOEXEC |
                                                (unsigned long)O_NONBLOCK);
    if (uffd == -1) {
        int e = errno;
        if (e == ENOSYS)
            fprintf(stderr,
                    "userfaultfd unsupported: kernel too old (<4.3)?\n");
        else if (e == EPERM)
            fprintf(stderr,
                    "userfaultfd blocked: set "
                    "'sysctl vm.unprivileged_userfaultfd=1' or run with "
                    "CAP_SYS_PTRACE (containers: seccomp profile).\n");
        die("main", "syscall(__NR_userfaultfd)", e);
    }

    /* Handshake with the kernel uffd protocol version. features=0 requests
     * the baseline: MISSING-mode events (what lazy paging needs). */
    struct uffdio_api api = { .api = UFFD_API, .features = 0 };
    if (ioctl(uffd, UFFDIO_API, &api) == -1)
        die("main", "ioctl(UFFDIO_API)", errno);
    if (api.api != UFFD_API)
        die("main", "UFFD_API mismatch", EINVAL);
    printf("[main] userfaultfd ready : api=%" PRIu64 "\n",
           (uint64_t)api.api);

    /* STEP 3 â€” register the region in MISSING mode: "pages under here may be
     * absent; when someone touches an absent page, wake me on this uffd."
     * Range must be page-aligned â€” ours is, since mmap returned a boundary. */
    struct uffdio_register reg = {
        .range = { .start = (uint64_t)(uintptr_t)region,
                   .len = region_len },
        .mode = UFFDIO_REGISTER_MODE_MISSING,
    };
    if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1)
        die("main", "ioctl(UFFDIO_REGISTER)", errno);

    /* Kernel confirms which per-range ioctls are valid; require UFFDIO_COPY
     * up front or the whole scheme is unusable. */
    if (!(reg.ioctls & (uint64_t)(1U << _UFFDIO_COPY)))
        die("main", "UFFDIO_COPY not supported for range", EINVAL);
    printf("[main] registered MISSING: %p +%zu bytes\n", region, region_len);

    /* STEP 4 â€” start the PF-daemon BEFORE wiping memory. Any fault between
     * registration and daemon-start would hang forever otherwise. */
    /* Shutdown channel for the daemon: eventfd is an 8-byte counter that
     * epoll treats as readable the instant anyone writes to it â€” the
     * standard race-free way to wake (and stop) an event loop. */
    int stop_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (stop_efd == -1)
        die("main", "eventfd(stop)", errno);

    struct pf_daemon_ctx ctx = {
        .uffd_fd = uffd,
        .stop_efd = stop_efd,
        .region_base = region,
        .region_len = region_len,
        .page_size = ps,
        .sim_latency_us = 0,
        .shutdown = ATOMIC_VAR_INIT(false),
        .faults_served = ATOMIC_VAR_INIT(0),
    };
    const char *simlat = getenv("IS_SIM_LATENCY_US"); /* demo knob */
    if (simlat)
        ctx.sim_latency_us = strtol(simlat, NULL, 10);

    pthread_t daemon_tid;
    if (pthread_create(&daemon_tid, NULL, pf_daemon_main, &ctx) != 0)
        die("main", "pthread_create(pf-daemon)", errno);

    /* STEP 5 â€” the "migration moment": discard all backing RAM while keeping
     * the virtual mappings. Post-madvise, the region is byte-for-byte what a
     * skeleton-restored process sees: valid VMAs, no pages behind them.
     * MADV_DONTNEED on MAP_PRIVATE drops the private copy immediately. */
    printf("\n--- CHECKPOINT RESTORED AS SKELETON: wiping physical pages ---\n");
    if (madvise(region, region_len, MADV_DONTNEED) == -1)
        die("main", "madvise(MADV_DONTNEED)", errno);
    t_wipe_done = now_ms();

    /* STEP 6 â€” ACTIVATION. Nothing blocks startup: the very next statement
     * executes within microseconds of the wipe. THIS is the metric that
     * matters: process state went SKELETON -> RUNNING without transferring
     * a single bulk page. */
    printf("--- STATE: RUNNING â€” resuming execution immediately ---\n");
    t_first_read_issued = now_ms();

    volatile uint64_t *first_word = (volatile uint64_t *)region;
    uint64_t got = *first_word; /* <<< TRAPS HERE: kernel parks this thread */
    t_first_read_done = now_ms();

    /* Metrics against the acceptance criteria. */
    double issue_after_wipe = t_first_read_issued - t_wipe_done;
    double resolve_latency = t_first_read_done - t_first_read_issued;
    printf("\n[main] activation        : first read issued %.3f ms after "
           "madvise (target < 20 ms)\n", issue_after_wipe);
    printf("[main] first fault cost  : %.3f ms until data available%s\n",
           resolve_latency,
           ctx.sim_latency_us > 0 ? " (includes simulated network)" : "");
    printf("[main] recovered value   : %" PRIu64 " (expected 1)\n", got);
    if (got != 1)
        die("main", "value integrity check", EPROTO);

    /* STEP 7 â€” walk the rest of the "heap": every untouched page takes its
     * own trap/inject round trip on first access. Sequential access patterns
     * like this are what the future prefetcher will exploit. */
    printf("\n--- hydrating remaining pages on demand ---\n");
    for (size_t i = 1; i < NUM_PAGES; ++i) {
        uint64_t *word = (uint64_t *)((char *)region + i * ps);
        uint64_t v = *word; /* traps on first touch */
        if (v != (uint64_t)i + 1)
            die("main", "per-page integrity check", EPROTO);

        /* Verify the pattern bytes too â€” proves FULL pages arrived, not just
         * the first word (a classic lazy-paging bug class). */
        char *body = (char *)word + sizeof(uint64_t);
        for (size_t b = 0; b < ps - sizeof(uint64_t); ++b)
            if ((unsigned char)body[b] != PATTERN_BYTE)
                die("main", "pattern integrity check", EPROTO);
    }

    /* Teardown order matters (this is where naive versions deadlock or
     * race with EBADF):
     *  1. All pages are present again â€” closing the uffd is safe, since no
     *     future MISSING fault exists to hit a deaf listener (= SIGSEGV).
     *  2. Poke the eventfd FIRST so the daemon's epoll wakes on purpose,
     *     then join it. Only after the thread is gone do we close its fds;
     *     nobody can ever read/write a closed fd this way.
     *  3. Join to reap the thread cleanly before printing the verdict. */
    long served = atomic_load(&ctx.faults_served);
    uint64_t one = 1;
    if (write(stop_efd, &one, sizeof(one)) == -1)
        die("main", "write(stop_efd)", errno);
    pthread_join(daemon_tid, NULL);
    close(uffd);
    close(stop_efd);

    printf("\n=== RESULT ===\n");
    printf("page faults served by pf-daemon : %ld (expected %d)\n",
           served, NUM_PAGES);
    printf("SIGSEGVs                        : 0 (kernel trapped silently)\n");
    printf("VERDICT                         : %s\n",
           served == NUM_PAGES ? "PASS â€” trap/inject cycle verified"
                               : "FAIL â€” fault count mismatch");
    return served == NUM_PAGES ? 0 : 1;
}
