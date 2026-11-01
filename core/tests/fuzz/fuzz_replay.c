/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_replay.c — run a harness over files, without libFuzzer.
 *
 * Linked into every harness in the ORDINARY host build (gcc, no
 * -fsanitize=fuzzer), so `meson test` replays the committed seed corpus and
 * every regression input under whatever sanitizers that build has — the
 * plain suite, and the CI ASan + UBSan job. This is libFuzzer's `-runs=0`
 * over a corpus, minus the dependency on clang: a crash that was fixed and
 * then comes back fails the unit suite, not just the next fuzzing session.
 *
 * Usage: fuzz_<name> <file-or-directory>...
 * Directories are read one level deep (the corpus layout), files in any
 * order; a missing argument is an error so a renamed corpus directory cannot
 * turn the test into a no-op. Prints how many inputs ran.
 */

#include "fuzz.h"

#include <dirent.h>
#include <sys/stat.h>

/* One input: read it into an exactly-sized heap buffer — the same shape
 * libFuzzer hands over, so ASan catches a read one byte past the end. */
static int run_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "replay: cannot open %s\n", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *buf = (uint8_t *)malloc(n ? (size_t)n : 1u);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return -1; }
    (void)LLVMFuzzerTestOneInput(buf, (size_t)n);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file-or-dir>...\n", argv[0]);
        return 2;
    }
    unsigned ran = 0;
    for (int a = 1; a < argc; a++) {
        struct stat st;
        if (stat(argv[a], &st) != 0) {
            fprintf(stderr, "replay: %s does not exist\n", argv[a]);
            return 1;
        }
        if (!S_ISDIR(st.st_mode)) {
            if (run_file(argv[a]) != 0) return 1;
            ran++;
            continue;
        }
        DIR *d = opendir(argv[a]);
        if (!d) {
            fprintf(stderr, "replay: cannot read %s\n", argv[a]);
            return 1;
        }
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;   /* ., .., dotfiles */
            char path[4096];
            snprintf(path, sizeof path, "%s/%s", argv[a], e->d_name);
            if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
            if (run_file(path) != 0) { closedir(d); return 1; }
            ran++;
        }
        closedir(d);
    }
    printf("replay: %u inputs, no findings\n", ran);
    return ran ? 0 : 1;   /* an empty corpus is a broken test, not a pass */
}
