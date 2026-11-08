#include "comp.h"

Buf format_bin(int64_t base, const char *code, size_t n) {
    Buf b = {0};
    buf_putc(&b, (int)(base & 0xFF)); buf_putc(&b, (int)((base >> 8) & 0xFF));
    buf_putc(&b, (int)(n & 0xFF)); buf_putc(&b, (int)((n >> 8) & 0xFF));
    buf_putn(&b, code, n);
    return b;
}

/* .sav: 512-byte header (start address, stack, final address, block bitmap) followed by the image */
Buf format_sav(int64_t base, const char *code, size_t n, int64_t final_addr) {
    Buf b = {0};
    if (final_addr < 0) final_addr = base + (int64_t)n;
    int64_t bs = base / 512, be = (final_addr + 511) / 512;
    unsigned char h[512] = {0};
    h[32] = (unsigned char)(base & 0xFF); h[33] = (unsigned char)((base >> 8) & 0xFF);
    h[34] = 0x00; h[35] = 0x02;                       /* initial stack pointer 01000 */
    h[40] = (unsigned char)(final_addr & 0xFF); h[41] = (unsigned char)((final_addr >> 8) & 0xFF);
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 8; j++) {
            int64_t blk = (7 - j) + i * 8;
            if (blk >= bs && blk < be) h[240 + i] |= (unsigned char)(1 << j);
        }
    buf_putn(&b, (const char *)h, sizeof h);
    buf_putn(&b, code, n);
    return b;
}

/* ---- BK tape WAV (port of bk_wav.py) ---- */
static void levels(Buf *o, const char *s, int times) {
    for (int t = 0; t < times; t++)
        for (const char *p = s; *p; p++) buf_putc(o, *p == 'H' ? 240 : *p == 'S' ? 240 : 16);
}
static void data_bits(Buf *o, const unsigned char *d, size_t n, const char *zero, const char *one) {
    for (size_t k = 0; k < n; k++)
        for (int i = 0; i < 8; i++) levels(o, ((d[k] >> i) & 1) ? one : zero, 1);
}
Buf format_wav(int64_t base, const unsigned char *code, size_t n, const unsigned char *name16, bool turbo) {
    Buf d = {0};
    const char *ONE = turbo ? "HHHLL" : "SSLLHHHHLLLL", *ZERO = turbo ? "HLL" : "SSLLHHLL";
    const char *SYNC_MID = "SSSSSSSSLLLLLLLLHHHHLLLL";
    if (turbo) {
        levels(&d, "SSSLLL", 1024); levels(&d, "SSSSSSSSSSSSLLLLLLLLLLLL", 1);
    } else {
        levels(&d, "SSLL", 4096); levels(&d, SYNC_MID, 1);
        levels(&d, "SSLL", 10); levels(&d, SYNC_MID, 1);
    }
    unsigned char hdr[20];
    hdr[0] = (unsigned char)(base & 0xFF); hdr[1] = (unsigned char)((base >> 8) & 0xFF);
    hdr[2] = (unsigned char)(n & 0xFF); hdr[3] = (unsigned char)((n >> 8) & 0xFF);
    memcpy(hdr + 4, name16, 16);
    data_bits(&d, hdr, 20, ZERO, ONE);
    if (turbo) levels(&d, "LLLL", 1); else { levels(&d, "SSLL", 10); levels(&d, SYNC_MID, 1); }
    data_bits(&d, code, n, ZERO, ONE);
    if (turbo) levels(&d, "LLLL", 1);
    uint64_t sum = 0; for (size_t i = 0; i < n; i++) sum += code[i];
    unsigned cs = (unsigned)(sum % 65535);
    unsigned char c2[2] = {(unsigned char)(cs & 0xFF), (unsigned char)(cs >> 8)};
    data_bits(&d, c2, 2, ZERO, ONE);
    if (turbo) { levels(&d, "SSSLLLSSSLLL", 1); } else levels(&d, "SSLL", 200);
    unsigned rate = turbo ? 40000 : 21428;
    Buf w = {0};
    uint32_t dl = (uint32_t)d.len;
    unsigned char h[44];
    memcpy(h, "RIFF", 4); uint32_t v = 36 + dl; memcpy(h + 4, &(unsigned char[4]){(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)}, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;
    for (int i = 0; i < 4; i++) { h[24 + i] = (unsigned char)(rate >> (8 * i)); h[28 + i] = (unsigned char)(rate >> (8 * i)); }
    h[32] = 1; h[33] = 0; h[34] = 8; h[35] = 0;
    memcpy(h + 36, "data", 4);
    for (int i = 0; i < 4; i++) h[40 + i] = (unsigned char)(dl >> (8 * i));
    buf_putn(&w, h, 44);
    buf_putn(&w, d.p, d.len);
    buf_free(&d);
    return w;
}
