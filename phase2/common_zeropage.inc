
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
