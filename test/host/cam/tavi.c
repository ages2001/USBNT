/* host test: write an AVI with avi.c (MJPEG frames from files or synthetic DIB frames) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../tools/avi.h"
static int W(void *c, const void *d, unsigned long n) { return fwrite(d, 1, n, (FILE *)c) != n; }
static int S(void *c, unsigned long p) { return fseek((FILE *)c, (long)p, SEEK_SET); }
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "wb");
    AVI_IO io = { f, W, S };
    AVI_WRITER a;
    static AVI_INDEX idx[1000];
    int i, n = atoi(argv[2]);
    if (argc > 3) {                                     /* MJPEG: argv[3] = jpeg file */
        FILE *j = fopen(argv[3], "rb");
        static unsigned char buf[1 << 20];
        long len = fread(buf, 1, sizeof(buf), j);
        AviOpen(&a, &io, AVI_FCC('M', 'J', 'P', 'G'), 640, 480, 24, 33333, idx, 1000);
        for (i = 0; i < n; i++) AviFrame(&a, buf, len - (i & 1));   /* odd sizes too */
    } else {
        static unsigned char fr[160 * 120 * 3];
        AviOpen(&a, &io, 0, 160, 120, 24, 66666, idx, 1000);
        for (i = 0; i < n; i++) { memset(fr, i * 20, sizeof(fr)); AviFrame(&a, fr, sizeof(fr)); }
    }
    printf("close %d frames %lu\n", AviClose(&a, 40000), a.Frames);
    fclose(f);
    return 0;
}
