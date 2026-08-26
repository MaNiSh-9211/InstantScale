
/* ------------------------------------------------------------------ */
/* Prometheus text-exposition endpoint (optional, --metrics-port P).   */
/* Minimal HTTP/1.1 server on a detached thread; counters are read     */
/* with __atomic so the epoll loop needs no locks.                     */
/* ------------------------------------------------------------------ */

static void metrics_serve_conn(int cfd)
{
    char req[512];
    size_t got = 0;
    while (got < sizeof(req) - 1) {
        ssize_t n = recv(cfd, req + got, 1, 0);
        if (n <= 0) { close(cfd); return; }
        if (req[got] == '\n') break; /* end of request line is enough */
        ++got;
    }
    req[got] = '\0';

    uint64_t pages   = __atomic_load_n(&g_pages_served, __ATOMIC_RELAXED);
    uint64_t zero    = __atomic_load_n(&g_zero_served,  __ATOMIC_RELAXED);
    uint64_t bytes   = __atomic_load_n(&g_bytes_served, __ATOMIC_RELAXED);
    uint64_t conns   = __atomic_load_n(&g_conns_served, __ATOMIC_RELAXED);
    uint64_t afail   = __atomic_load_n(&g_auth_fail,    __ATOMIC_RELAXED);
    long     live    = g_live_conns;

    char body[1024];
    int bl = snprintf(body, sizeof(body),
        "# HELP hotpod_pages_served_total data pages served.\n"
        "# TYPE hotpod_pages_served_total counter\n"
        "hotpod_pages_served_total %" PRIu64 "\n"
        "# HELP hotpod_zeropages_served_total zero pages elided.\n"
        "# TYPE hotpod_zeropages_served_total counter\n"
        "hotpod_zeropages_served_total %" PRIu64 "\n"
        "# HELP hotpod_bytes_served_total payload bytes served.\n"
        "# TYPE hotpod_bytes_served_total counter\n"
        "hotpod_bytes_served_total %" PRIu64 "\n"
        "# HELP hotpod_connections_total accepted client connections.\n"
        "# TYPE hotpod_connections_total counter\n"
        "hotpod_connections_total %" PRIu64 "\n"
        "# HELP hotpod_live_connections currently open connections.\n"
        "# TYPE hotpod_live_connections gauge\n"
        "hotpod_live_connections %ld\n"
        "# HELP hotpod_auth_failures_total rejected auth attempts.\n"
        "# TYPE hotpod_auth_failures_total counter\n"
        "hotpod_auth_failures_total %" PRIu64 "\n",
        pages, zero, bytes, conns, live, afail);

    char hdr[128];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n", bl);
    send(cfd, hdr, (size_t)hl, MSG_NOSIGNAL);
    send(cfd, body, (size_t)bl, MSG_NOSIGNAL);
    close(cfd);
}

static void *metrics_main(void *arg)
{
    fprintf(stderr, "[metrics] thread up\n");
    int port = (int)(intptr_t)arg;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == -1)
        return NULL;
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)port),
    };
    int brc = bind(lfd, (struct sockaddr *)&a, sizeof(a));
    fprintf(stderr, "[metrics] bind rc=%d errno=%d\n", brc, errno);
    if (brc == -1 || listen(lfd, 16) == -1) {
        fprintf(stderr, "[metrics] cannot bind :%d — metrics disabled\n",
                port);
        close(lfd);
        return NULL;
    }
        fprintf(stderr, "[metrics] listening on :%d\n", port);
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd == -1)
            continue;
        metrics_serve_conn(cfd);
    }
    return NULL;
}
