/* host test: decode a JPEG with jpegdec.c, write a PPM (or test DHT insertion) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../tools/jpegdec.h"

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    unsigned char *in, *out, *work;
    long n, w, h, y;
    JPEG_INFO info;
    int r;
    if (!f) return 2;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    in = malloc(n + 1024); fread(in, 1, n, f); fclose(f);
    if (argc > 3 && !strcmp(argv[3], "dht")) {
        unsigned char *o = malloc(n + JPEG_DHT_SIZE);
        unsigned long m = JpegAddDht(in, n, o, n + JPEG_DHT_SIZE);
        f = fopen(argv[2], "wb"); fwrite(o, 1, m, f); fclose(f);
        printf("dht %ld -> %lu\n", n, m);
        return m ? 0 : 1;
    }
    r = JpegInfo(in, n, &info);
    printf("info r=%d %dx%d nc=%d dht=%d h0=%d v0=%d\n", r, info.Width, info.Height, info.Components, info.HasDht, info.H[0], info.V[0]);
    w = info.Width; h = info.Height;
    out = malloc(w * h * 3 + 16);
    work = malloc(JpegWorkSize(w));
    r = JpegDecode(in, n, out, w * 3, 0, 4096, 4096, work, JpegWorkSize(w), &info);
    printf("decode r=%d\n", r);
    f = fopen(argv[2], "wb");
    fprintf(f, "P6\n%ld %ld\n255\n", w, h);
    for (y = 0; y < w * h; y++) { unsigned char px[3] = { out[y * 3 + 2], out[y * 3 + 1], out[y * 3] }; fwrite(px, 1, 3, f); }
    fclose(f);
    return r == 0 ? 0 : 1;
}
