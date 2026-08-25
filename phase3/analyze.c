/*
 * HotPod â€” analyze.c (Phase 3)
 * CRIU image-split analyzer: quantifies and DEMONSTRATES the core HotPod
 * thesis on a real checkpoint directory:
 *
 *   SKELETON = everything that is NOT bulk memory (registers, VMAs, page
 *              tables, fds, ...)  â†’ the only thing activation truly needs
 *   BULK     = pages-*.img          â†’ the heavy payload we stream lazily
 *
 * It reports sizes/ratios per file, then physically splits the directory into
 * <dir>-split/skeleton/ + <dir>-split/pages/ â€” programmatic manipulation of
 * CRIU images, separating page-layout metadata from binary page storage.
 */
#include <dirent.h>
#include <sys/stat.h>
#include "common.h" /* reuse logging/crc/etc. via relative include path */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <criu-images-dir>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];

    DIR *d = opendir(dir);
    if (!d)
        is_die("analyze", "opendir(images)", errno);

    double skel_bytes = 0, bulk_bytes = 0;
    int skel_files = 0, bulk_files = 0;

    char split_dir[512], skel_dir[600], pages_dir[600];
    snprintf(split_dir, sizeof(split_dir), "%s-split", dir);
    snprintf(skel_dir, sizeof(skel_dir), "%s/skeleton", split_dir);
    snprintf(pages_dir, sizeof(pages_dir), "%s/pages", split_dir);

    struct stat st;
    bool do_split = stat(split_dir, &st) != 0;
    if (do_split &&
        (mkdir(split_dir, 0755) == -1 || mkdir(skel_dir, 0755) == -1 ||
         mkdir(pages_dir, 0755) == -1))
        is_die("analyze", "mkdir(split dirs)", errno);

    printf("%-40s %12s %s\n", "image file", "bytes", "class");
    printf("---------------------------------------------------------------\n");

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        bool is_bulk = !strncmp(e->d_name, "pages-", 6);
        printf("%-40s %12ld %s\n", e->d_name, (long)st.st_size,
               is_bulk ? "BULK" : "SKELETON");
        if (is_bulk) {
            bulk_bytes += (double)st.st_size;
            ++bulk_files;
        } else {
            skel_bytes += (double)st.st_size;
            ++skel_files;
        }
        if (do_split) { /* physically separate the two classes */
            char dst[1100];
            snprintf(dst, sizeof(dst), "%s/%s",
                     is_bulk ? pages_dir : skel_dir, e->d_name);
            FILE *in = fopen(path, "rb");
            FILE *out = fopen(dst, "wb");
            if (!in || !out)
                is_die("analyze", "fopen(copy)", errno);
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
                fwrite(buf, 1, n, out);
            fclose(in);
            fclose(out);
        }
    }
    closedir(d);

    double total = skel_bytes + bulk_bytes;
    printf("---------------------------------------------------------------\n");
    printf("skeleton : %3d files %10.0f bytes (%5.2f%%)\n",
           skel_files, skel_bytes, total ? 100.0 * skel_bytes / total : 0);
    printf("bulk     : %3d files %10.0f bytes (%5.2f%%)\n",
           bulk_files, bulk_bytes, total ? 100.0 * bulk_bytes / total : 0);
    printf("=> activation needs only the skeleton; the %.1fx larger bulk\n"
           "   stream can arrive lazily while the process already runs.\n",
           total && skel_bytes ? bulk_bytes / skel_bytes : 0);
    if (do_split)
        printf("split written to: %s\n", split_dir);
    return 0;
}
