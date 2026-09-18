// POSITIVE CONTROL 1 for the guest profiler: burn essentially all of the
// guest's time inside libz's inflate, so a profiler that is working must
// attribute nearly every sample to libz and nothing much to anything else.
//
// Uses zlib's uncompress()/compress2() rather than the streaming API on
// purpose: both take only pointers and longs, so this needs no zlib.h and
// makes no assumption about z_stream's layout. dlopen keeps it working on a
// root that ships libz.so.1 without the -dev symlink.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*compress2_fn)(unsigned char *, unsigned long *,
                            const unsigned char *, unsigned long, int);
typedef int (*uncompress_fn)(unsigned char *, unsigned long *,
                             const unsigned char *, unsigned long);

int main(int argc, char **argv) {
    const char *soname = argc > 1 ? argv[1] : "libz.so.1";
    int rounds = argc > 2 ? atoi(argv[2]) : 300;
    void *h = dlopen(soname, RTLD_NOW);
    if (h == NULL) { fprintf(stderr, "dlopen %s: %s\n", soname, dlerror()); return 2; }
    compress2_fn zcompress2 = (compress2_fn) dlsym(h, "compress2");
    uncompress_fn zuncompress = (uncompress_fn) dlsym(h, "uncompress");
    if (zcompress2 == NULL || zuncompress == NULL) { fprintf(stderr, "dlsym failed\n"); return 2; }

    size_t raw_len = 1u << 20;
    unsigned char *raw = malloc(raw_len);
    // Semi-compressible: pure zeros would make inflate trivially fast and
    // pure random would make it incompressible. Neither exercises the codec.
    unsigned seed = 12345;
    for (size_t i = 0; i < raw_len; i++) {
        seed = seed * 1103515245u + 12345u;
        raw[i] = (unsigned char) ((seed >> 16) & ((i % 64 < 40) ? 0x07 : 0xff));
    }
    unsigned long comp_len = raw_len + raw_len / 2 + 1024;
    unsigned char *comp = malloc(comp_len);
    if (zcompress2(comp, &comp_len, raw, raw_len, 6) != 0) { fprintf(stderr, "compress failed\n"); return 2; }

    unsigned char *out = malloc(raw_len);
    unsigned long long checksum = 0;
    for (int r = 0; r < rounds; r++) {
        unsigned long out_len = raw_len;
        if (zuncompress(out, &out_len, comp, comp_len) != 0) { fprintf(stderr, "uncompress failed\n"); return 2; }
        checksum += out_len + out[r % raw_len];
    }
    // Printed so the run cannot "pass" without having done the work.
    printf("CTL_INFLATE_OK rounds=%d comp=%lu raw=%zu checksum=%llu\n",
           rounds, comp_len, raw_len, checksum);
    return 0;
}
