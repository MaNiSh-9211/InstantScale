/*
 * InstantScale — seeder.c
 * Builds the "checkpointed warm heap": a deterministic image file that plays
 * the role of a CRIU memory dump taken from a pre-warmed service on Host A.
 *
 * Page i content (deterministic, verifiable):
 *   word 0            : sentinel  i+1
 *   bytes [8, ps)     : PRNG noise seeded by (i ^ IS_PAGE_SEED)
 * The whole image folds into one rolling CRC32 digest; the restorer proves
 * end-to-end integrity by recomputing it after hydration.
 *
 * Usage: ./seeder <image.isim> [num_pages]
 */
#include "common.h"

#define IS_PAGE_SEED 0x9E3779B97F4A7C15ull /* golden-ratio constant */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image.isim> [num_pages=%u]\n",
                argv[0], 4096u);
        return 2;
    }
    const char *path = argv[1];
    uint32_t num_pages = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 4096;
    if (num_pages == 0)
        is_die("seeder", "num_pages parse", EINVAL);

    size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    size_t region_len = (size_t)num_pages * ps;

    is_crc_init();

    /* Build the entire image in memory first: lets us compute the CRC in one
     * pass and write() the file in one shot. For real multi-GB heaps this
     * streams page-by-page instead — same math, chunked. */
    is_img_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = IS_IMG_MAGIC;
    hdr.version = IS_IMG_VER;
    hdr.page_size = (uint32_t)ps;
    hdr.num_pages = num_pages;
    hdr.region_len = region_len;
    snprintf(hdr.svc_name, sizeof(hdr.svc_name), "payments-jvm-warm");

    uint8_t *img = malloc(sizeof(hdr) + region_len);
    if (!img)
        is_die("seeder", "malloc(image)", ENOMEM);
    memcpy(img, &hdr, sizeof(hdr));

    double t0 = is_now_ms();
    uint32_t crc = 0;
    for (uint32_t i = 0; i < num_pages; ++i) {
        uint8_t *page = img + sizeof(hdr) + (size_t)i * ps;

        /* Sentinel word proves per-page addressing correctness across the
         * wire — if offsets ever skew, sentinels mismatch immediately. */
        *(uint64_t *)page = (uint64_t)i + 1;

        /* Deterministic pseudo-heap: every byte varies, nothing compressible,
         * so bandwidth numbers are honest. Same seed -> same bytes on any
         * host, which is what makes cross-process verification possible. */
        uint64_t st = (uint64_t)i ^ IS_PAGE_SEED;
        for (size_t off = sizeof(uint64_t); off + sizeof(uint64_t) <= ps;
             off += sizeof(uint64_t))
            *(uint64_t *)(page + off) = is_xorshift64(&st);

        crc = is_crc_update(crc, page, ps); /* rolling over page bodies */
    }
    hdr.digest = crc;
    memcpy(img, &hdr, sizeof(hdr)); /* stamp final digest into header */

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
        is_die("seeder", "open(image)", errno);
    size_t total = sizeof(hdr) + region_len;
    if (write(fd, img, total) != (ssize_t)total)
        is_die("seeder", "write(image)", errno);
    if (fsync(fd) == -1)
        is_die("seeder", "fsync(image)", errno);
    close(fd);
    free(img);

    printf("[%-9s] checkpoint image : %s\n", "seeder", path);
    printf("[%-9s] region            : %u pages × %zu B = %.1f MB\n",
           "seeder", num_pages, ps, (double)region_len / (1024.0 * 1024.0));
    printf("[%-9s] digest            : 0x%08" PRIx32 "\n", "seeder", crc);
    printf("[%-9s] built in          : %.2f ms\n", "seeder", is_now_ms() - t0);
    return 0;
}
