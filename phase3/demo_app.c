/*
 * HotPod â€” demo_app.c (Phase 3)
 * A realistic stand-in for a pre-warmed production service whose lifecycle
 * is owned by HotPod:
 *
 *   RUN      cold bootstrap (--init-ms models JVM/V8 startup tax) -> heap
 *            warm-up -> steady heartbeats carrying a monotonic SEQ number.
 *   CKPT     SIGUSR2 => snapshot heap+seq into an ISIM image, then exit
 *            (the "scale-in to warm image" moment). SIGUSR1 snapshots but
 *            keeps serving (warm-pool refresh).
 *   RESUME   --resume IMG               eager: whole heap read before HB #1
 *            --resume-lazy HOST PORT IMG  HotPod: arm userfaultfd,
 *            activate INSTANTLY, stream pages on demand from a phase2
 *            pageserver. First post-resume heartbeat continues at seq=N+1:
 *            proof the process RESUMED instead of restarted.
 *
 * Checkpoint layout reuses the phase2 image definitions verbatim:
 *   [is_img_hdr][heap pages][tail_meta {TAIL_MAGIC, seq, uptime_ms}]
 */
#include "../phase2/common.h"
#include "puller.h"
#include <signal.h>
#include <pthread.h>

#define TAIL_MAGIC 0x4953544Cull /* "ISTL" */

typedef struct {
    uint64_t magic;
    uint64_t seq;
    uint64_t uptime_ms;
} tail_meta;

static const uint64_t SENTINEL = 0x1A2B3C4D5E6F7081ull;

static volatile sig_atomic_t g_snapshot = 0; /* SIGUSR1/2 seen */
static volatile sig_atomic_t g_stop = 0;     /* SIGTERM graceful stop  */

static void on_sigterm(int sig) { (void)sig; g_stop = 1; }

static volatile sig_atomic_t g_exit_after = 0;

static void on_sigusr(int sig)
{
    g_snapshot = 1;
    if (sig == SIGUSR2)
        g_exit_after = 1;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static unsigned long arg_ul(int argc, char **argv, const char *key,
                            unsigned long dflt)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], key))
            return strtoul(argv[i + 1], NULL, 10);
    return dflt;
}

static const char *arg_str(int argc, char **argv, const char *key)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], key))
            return argv[i + 1];
    return NULL;
}

/* Fill pages: word0 = SENTINEL^idx (O(1) live verification), body PRNG. */
static void fill_heap(uint8_t *heap, size_t npages, size_t ps)
{
    for (size_t pi = 0; pi < npages; ++pi) {
        uint8_t *page = heap + pi * ps;
        *(uint64_t *)page = SENTINEL ^ (uint64_t)pi;
        uint64_t s2 = 0x9E3779B97F4A7C15ull ^ ((uint64_t)pi << 1);
        for (size_t b = sizeof(uint64_t); b < ps; b += sizeof(uint64_t)) {
            s2 ^= s2 << 13; s2 ^= s2 >> 7; s2 ^= s2 << 17;
            *(uint64_t *)(page + b) = s2;
        }
    }
}

/* Write [hdr][pages][tail] in one pass; returns wall milliseconds spent. */
static double write_checkpoint(const char *path, const uint8_t *heap,
                               size_t npages, size_t ps, uint64_t seq,
                               double uptime_ms)
{
    double t0 = now_ms();
    is_img_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = IS_IMG_MAGIC;
    hdr.version = IS_IMG_VER;
    hdr.page_size = (uint32_t)ps;
    hdr.num_pages = (uint32_t)npages;
    hdr.region_len = (uint64_t)npages * ps;
    snprintf(hdr.svc_name, sizeof(hdr.svc_name), "demo-app");
    hdr.digest = is_crc_update(0, heap, npages * ps);

    tail_meta tail = { .magic = TAIL_MAGIC, .seq = seq,
                       .uptime_ms = (uint64_t)uptime_ms };

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
        is_die("ckpt", "open(image)", errno);
    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        write(fd, heap, npages * ps) != (ssize_t)(npages * ps) ||
        write(fd, &tail, sizeof(tail)) != (ssize_t)sizeof(tail))
        is_die("ckpt", "write(image)", errno);
    fsync(fd);
    close(fd);
    return now_ms() - t0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, on_sigusr);
    signal(SIGUSR2, on_sigusr);
    signal(SIGTERM, on_sigterm);
    setvbuf(stdout, NULL, _IOLBF, 0);
    is_crc_init();

    unsigned long init_ms    = arg_ul(argc, argv, "--init-ms", 0);
    unsigned long init_work_mb = arg_ul(argc, argv, "--init-work-mb", 256);
    unsigned long warm_mb    = arg_ul(argc, argv, "--warm-mb", 32);
    unsigned long interval   = arg_ul(argc, argv, "--interval-ms", 100);
    const char *resume     = arg_str(argc, argv, "--resume");
    const char *rlazy_img  = arg_str(argc, argv, "--resume-lazy-img");
    const char *host       = arg_str(argc, argv, "--host");
    const char *ckpt       = arg_str(argc, argv, "--ckpt");
    if (!ckpt)
        ckpt = "/tmp/demo_app.isim";

    size_t ps = (size_t)sysconf(_SC_PAGESIZE);

    uint8_t *heap = NULL;
    size_t   npages = 0;
    uint64_t seq = 0, born_shift_ms = 0;
    double   t_start = now_ms();
    int      uffd = -1;

    if (resume || rlazy_img) { /* ------------------ RESUME PATHS -------- */
        const char *img = resume ? resume : rlazy_img;
        int fd = open(img, O_RDONLY);
        if (fd == -1)
            is_die("resume", "open(image)", errno);

        is_img_hdr hdr;
        if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
            hdr.magic != IS_IMG_MAGIC)
            is_die("resume", "bad image header", EINVAL);
        npages = hdr.num_pages;

                tail_meta tail;
        if (lseek(fd, -(off_t)sizeof(tail), SEEK_END) == (off_t)-1 ||
            read(fd, &tail, sizeof(tail)) != (ssize_t)sizeof(tail) ||
            tail.magic != TAIL_MAGIC)
            is_die("resume", "bad checkpoint tail", EINVAL);
        seq = tail.seq;                    /* continuity anchor          */
        born_shift_ms = tail.uptime_ms;    /* uptime keeps counting      */

        heap = mmap(NULL, hdr.region_len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (heap == MAP_FAILED)
            is_die("resume", "mmap(heap)", errno);

        if (resume) { /* EAGER: every byte must be resident before HB #1 */
            if (pread(fd, heap, npages * ps, (off_t)sizeof(hdr)) !=
                (ssize_t)(npages * ps))
                is_die("resume", "pread(heap)", errno);
            close(fd);
            printf("RESUMED-EAGER seq=%llu activated_in=%.3f ms\n",
                   (unsigned long long)seq, now_ms() - t_start);
        } else {      /* LAZY: skeleton only â€” pages arrive via uffd      */
            close(fd);
            uffd = (int)syscall(__NR_userfaultfd,
                                (unsigned long)O_CLOEXEC |
                                (unsigned long)O_NONBLOCK);
            if (uffd == -1) {
                int e = errno;
                if (e == EPERM)
                    fprintf(stderr, "hint: sysctl "
                            "vm.unprivileged_userfaultfd=1\n");
                is_die("resume-lazy", "userfaultfd", e);
            }
            struct uffdio_api api = { .api = UFFD_API };
            if (ioctl(uffd, UFFDIO_API, &api) == -1)
                is_die("resume-lazy", "UFFDIO_API", errno);
            struct uffdio_register reg = {
                .range = { .start = (uint64_t)(uintptr_t)heap,
                           .len = hdr.region_len },
                .mode = UFFDIO_REGISTER_MODE_MISSING,
            };
            if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1)
                is_die("resume-lazy", "UFFDIO_REGISTER", errno);

            /* Hand the fd pair to the condensed PF-Daemon thread. */
            static pp_ctx pp;
            int sock = is_tcp_connect(
                host ? host : "127.0.0.1",
                (uint16_t)arg_ul(argc, argv, "--port", IS_DEFAULT_PORT));

            /* PSK auth before anything else flows on this socket. */
            const char *tok = getenv("HOTPOD_TOKEN");
            const char *tokfile = arg_str(argc, argv, "--token-file");
            if (tokfile) {
                tok = is_read_token_file(tokfile);
                if (!tok)
                    is_die("resume-lazy", "read token file", errno);
            }
            if (tok)
                is_client_auth(sock, tok);

            is_set_nonblock(sock);
            pp.uffd = uffd;
            pp.sock = sock;
            pp.base = heap;
            pp.ps = ps;
            pp.np = npages;
            pp.state = calloc(npages, 1);
            pp.slotbuf = malloc((size_t)PP_SLOTS * ps);
            pp.icap = sizeof(is_wire_hdr) +
                      (size_t)PP_MAX_BATCH * (sizeof(is_wire_page_hdr) + ps);
            pp.ibuf = malloc(pp.icap);
            if (!pp.state || !pp.slotbuf || !pp.ibuf)
                is_die("resume-lazy", "alloc(puller)", ENOMEM);

            pthread_t tid;
            if (pthread_create(&tid, NULL, pp_main, &pp) != 0)
                is_die("resume-lazy", "pthread_create", errno);

            printf("RESUMED-LAZY seq=%llu activated_in=%.3f ms"
                   " (0%% of %zu pages present)\n",
                   (unsigned long long)seq, now_ms() - t_start, npages);
        }
    } else { /* --------------------------------------- FRESH COLD PATH ---- */
        /* Legacy sleep hook (0 by default — we do REAL work instead). */
        struct timespec sl = { .tv_nsec = 50 * 1000 * 1000 };
        while (now_ms() - t_start < (double)init_ms)
            nanosleep(&sl, NULL);

        /* REAL cold-start work — the cost every runtime pays: load a
         * dataset (memory writes) and verify it (CPU-bound hashing).
         * No sleeps. This is the number HotPod deletes on resume. */
        if (init_work_mb > 0) {
            size_t wbytes = init_work_mb * (1024 * 1024);
            uint8_t *wbuf = malloc(wbytes);
            if (!wbuf)
                is_die("cold", "malloc(init-work)", ENOMEM);
            double w0 = now_ms();
            uint64_t st = 0x243F6A8885A308D3ull;
            for (size_t b = 0; b < wbytes; b += sizeof(uint64_t)) {
                st ^= st << 13; st ^= st >> 7; st ^= st << 17;
                *(uint64_t *)(wbuf + b) = st;
            }
            uint32_t wcrc = is_crc_update(0, wbuf, wbytes);
            double load_ms = now_ms() - w0;
            printf("INIT real-work: loaded+verified %lu MB in %.1f ms"
                   " (verify=0x%08x)\n", init_work_mb, load_ms, wcrc);
            free(wbuf);
        }

        npages = warm_mb * (1024 * 1024) / ps;
        heap = mmap(NULL, npages * ps, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (heap == MAP_FAILED)
            is_die("cold", "mmap(heap)", errno);
        fill_heap(heap, npages, ps);
    }

    if (rlazy_img) /* lazy heap still empty here; CRC would force hydration */
        printf("READY pid=%d pages=%zu digest=PENDING(lazy)\n", getpid(),
               npages);
    else
        printf("READY pid=%d pages=%zu digest=0x%08x\n", getpid(), npages,
               is_crc_update(0, heap, npages * ps));
    /* ------------------------------ steady state ---------------------- */
    while (!g_stop) {
        struct timespec iv = { .tv_sec = (time_t)(interval / 1000),
                               .tv_nsec = (long)(interval % 1000) * 1000000L };
        nanosleep(&iv, NULL);

        if (g_snapshot) {
            double ms = write_checkpoint(ckpt, heap,
                                         npages, ps, seq,
                                         now_ms() - t_start + born_shift_ms);
            printf("CKPT path=%s bytes=%zu took=%.2f ms\n",
                   ckpt,
                   sizeof(is_img_hdr) + npages * ps + sizeof(tail_meta), ms);
            g_snapshot = 0;
            if (g_exit_after)
                break;
        }

        ++seq;
        size_t pi = (size_t)((seq * 7919) % npages);
        if (*(uint64_t *)(heap + pi * ps) != (SENTINEL ^ (uint64_t)pi)) {
            fprintf(stderr, "CORRUPTION at page %zu\n", pi);
            return 2;
        }
        printf("HB seq=%llu pid=%d uptime_ms=%.0f\n",
               (unsigned long long)seq, getpid(),
               now_ms() - t_start + born_shift_ms);
    }

    /* Full integrity sweep before exit (also proves ALL lazy pages arrived).*/
    uint32_t crc = is_crc_update(0, heap, npages * ps);
    printf("FINAL digest=0x%08x seq=%llu\n", crc, (unsigned long long)seq);
    if (uffd != -1)
        close(uffd); /* daemon exits with process; demo scope ends here */
    return 0;
}
