#include "text.h"
#include "platform.h"
#include "charset.h"
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

uint32_t u8_next(const char *s, size_t len, size_t *i) {
    unsigned char c = (unsigned char)s[*i];
    uint32_t cp;
    size_t n;
    if (c < 0x80) { (*i)++; return c; }
    if (c < 0xE0) { cp = c & 0x1F; n = 1; }
    else if (c < 0xF0) { cp = c & 0x0F; n = 2; }
    else { cp = c & 0x07; n = 3; }
    (*i)++;
    while (n-- && *i < len) { cp = (cp << 6) | ((unsigned char)s[*i] & 0x3F); (*i)++; }
    return cp;
}
size_t u8_len_at(const char *s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}
size_t u8_count(const char *s, size_t a, size_t b) {
    size_t n = 0;
    for (size_t i = a; i < b; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

bool py_isspace(uint32_t c) {
    if (c < 0x80) return c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1c && c <= 0x1f);
    return c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
           c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

char *afmt(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *p = (char *)amalloc((size_t)(n < 0 ? 0 : n) + 1);
    if (n >= 0) vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

/* Validate UTF-8 like CPython's strict decoder and produce its error message. */
static bool utf8_validate(const unsigned char *s, size_t n, char **err) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t need; unsigned char lo = 0x80, hi = 0xBF;
        const char *why = "invalid continuation byte";
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
        else {
            *err = afmt("'utf-8' codec can't decode byte 0x%02x in position %zu: invalid start byte", c, i);
            return false;
        }
        size_t k = 1;
        for (; k <= need; k++) {
            if (i + k >= n) { why = "unexpected end of data"; break; }
            unsigned char d = s[i + k];
            unsigned char l = (k == 1) ? lo : 0x80, h = (k == 1) ? hi : 0xBF;
            if (d < l || d > h) break;
        }
        if (k <= need) {
            if (k == 1) *err = afmt("'utf-8' codec can't decode byte 0x%02x in position %zu: %s", c, i, why);
            else *err = afmt("'utf-8' codec can't decode bytes in position %zu-%zu: %s", i, i + k - 1, why);
            return false;
        }
        i += need + 1;
    }
    return true;
}

const char *g_source_charset = "auto";

bool source_charset_valid(const char *n) {
    static const char *ok[] = {"auto", "utf-8", "utf8", "utf-16", "utf16", "cp1251", "windows-1251", "cp866", "koi8-r", "koi8_r", "latin-1", "latin1", "iso-8859-1", "ascii"};
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) if (streq_ci(n, ok[i])) return true;
    return false;
}

static void put_cp_checked(Buf *o, uint32_t cp) { buf_put_utf8(o, cp); }

/* UTF-16 -> UTF-8; returns false on a broken surrogate pair */
static bool utf16_to_utf8(const unsigned char *d, size_t n, bool be, Buf *o, char **err) {
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = be ? (uint32_t)((d[i] << 8) | d[i + 1]) : (uint32_t)((d[i + 1] << 8) | d[i]);
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 3 >= n) { *err = afmt("truncated UTF-16 surrogate pair at position %zu", i); return false; }
            uint32_t v = be ? (uint32_t)((d[i + 2] << 8) | d[i + 3]) : (uint32_t)((d[i + 3] << 8) | d[i + 2]);
            if (v < 0xDC00 || v > 0xDFFF) { *err = afmt("invalid UTF-16 surrogate pair at position %zu", i); return false; }
            u = 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00);
            i += 2;
        } else if (u >= 0xDC00 && u <= 0xDFFF) { *err = afmt("unpaired UTF-16 surrogate at position %zu", i); return false; }
        put_cp_checked(o, u);
    }
    if (n & 1) { *err = afmt("odd number of bytes in a UTF-16 file"); return false; }
    return true;
}

static bool is_cyr_lower(uint32_t c) { return (c >= 0x430 && c <= 0x44F) || c == 0x451; }
static bool is_cyr_upper(uint32_t c) { return (c >= 0x410 && c <= 0x42F) || c == 0x401; }

/* Frequency of Russian letters in percent (а..я, ё), used as a unigram language model. */
static const double RU_FREQ[33] = {
    8.01, 1.59, 4.54, 1.70, 2.98, 8.45, 0.94, 1.65, 7.35, 1.21, 3.49, 4.34, 3.21, 6.70, 10.97, 2.81,   /* а б в г д е ж з и й к л м н о п */
    4.73, 5.47, 6.26, 2.62, 0.27, 0.97, 0.49, 1.45, 0.72, 0.36, 0.04, 1.90, 1.74, 0.33, 0.64, 2.01,    /* р с т у ф х ц ч ш щ ъ ы ь э ю я */
    0.04                                                                                                /* ё */
};

static double letter_logp(uint32_t c) {
    uint32_t l = c;
    if (is_cyr_upper(c)) l = (c == 0x401) ? 0x451 : c + 32;
    int idx = (l == 0x451) ? 32 : (int)(l - 0x430);
    return log(RU_FREQ[idx] / 100.0);
}

/* How plausible is it that the 8-bit text d is written in charset id? Higher is better.
   Log-likelihood under a Russian letter-frequency model, so that an all-capitals cp1251 text is not mistaken
   for lower-case koi8-r (their letters have very different frequencies). */
static double score_charset(const unsigned char *d, size_t n, CharsetId id) {
    double score = 0;
    int prev = 0;                       /* 0 none, 1 lower, 2 upper */
    for (size_t i = 0; i < n; i++) {
        unsigned char b = d[i];
        if (b < 128) { prev = 0; continue; }
        uint32_t c = cs_decode_hi(id, b);
        if (c == 0) { score += log(1e-6); prev = 0; continue; }
        if (is_cyr_lower(c) || is_cyr_upper(c)) {
            score += letter_logp(c);
            int cur = is_cyr_lower(c) ? 1 : 2;
            if (prev == 1 && cur == 2) score += log(0.02);        /* aLTERNATING case inside a word is unusual */
            prev = cur;
        } else { score += log(0.002); prev = 0; }     /* box drawing, symbols, accented Latin: unlikely in source text */
    }
    return score;
}

static bool cyrillic_dominates(const unsigned char *d, size_t n, CharsetId id) {
    size_t hi = 0, cyr = 0;
    for (size_t i = 0; i < n; i++) if (d[i] >= 128) { hi++; uint32_t c = cs_decode_hi(id, d[i]); if (is_cyr_lower(c) || is_cyr_upper(c)) cyr++; }
    return hi > 0 && cyr * 2 >= hi;
}

/* Decode raw file bytes into UTF-8 (without newline normalization). */
static bool decode_source(const unsigned char *d, size_t n, Buf *out, const char **enc, bool *detected, char **err) {
    const char *want = g_source_charset;
    *detected = false;
    /* byte order marks */
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF && (streq_ci(want, "auto") || streq_ci(want, "utf-8") || streq_ci(want, "utf8"))) {
        d += 3; n -= 3;                  /* a UTF-8 BOM is not part of the program */
        want = "utf-8";
    }
    bool is_utf16 = streq_ci(want, "utf-16") || streq_ci(want, "utf16");
    if (streq_ci(want, "auto") && n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) is_utf16 = true;
    if (streq_ci(want, "auto") && !is_utf16 && n >= 4) {          /* UTF-16 without BOM: every other byte is zero */
        size_t even0 = 0, odd0 = 0;
        for (size_t i = 0; i + 1 < n; i += 2) { even0 += d[i] == 0; odd0 += d[i + 1] == 0; }
        size_t pairs = n / 2;
        if (odd0 * 10 > pairs * 4 && even0 * 20 < pairs) { *enc = "utf-16le"; *detected = true; return utf16_to_utf8(d, n & ~(size_t)1, false, out, err); }
        if (even0 * 10 > pairs * 4 && odd0 * 20 < pairs) { *enc = "utf-16be"; *detected = true; return utf16_to_utf8(d, n & ~(size_t)1, true, out, err); }
    }
    if (is_utf16) {
        bool be = false;
        if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) { d += 2; n -= 2; }
        else if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) { d += 2; n -= 2; be = true; }
        *enc = be ? "utf-16be" : "utf-16le";
        *detected = streq_ci(g_source_charset, "auto");
        return utf16_to_utf8(d, n, be, out, err);
    }
    bool want_utf8 = streq_ci(want, "utf-8") || streq_ci(want, "utf8");
    if (want_utf8 || streq_ci(want, "auto")) {
        char *e = NULL;
        if (utf8_validate(d, n, &e)) { *enc = "utf-8"; buf_putn(out, d, n); return true; }
        if (want_utf8) { *err = e; return false; }
    }
    /* 8-bit encodings */
    CharsetId id = CS_CP1251;
    const char *name = "cp1251";
    if (streq_ci(want, "auto")) {
        bool has_hi = false;
        for (size_t i = 0; i < n; i++) if (d[i] >= 128) { has_hi = true; break; }
        double best = -1e300;
        static const struct { CharsetId id; const char *name; } C[] = {{CS_CP1251, "cp1251"}, {CS_CP866, "cp866"}, {CS_KOI8R, "koi8-r"}};
        for (int k = 0; k < 3; k++) {
            double sc = score_charset(d, n, C[k].id);
            if (sc > best) { best = sc; id = C[k].id; name = C[k].name; }
        }
        /* require real Cyrillic evidence: at least one decoded Cyrillic letter
           per two high bytes in the best candidate */
        if (!has_hi || !cyrillic_dominates(d, n, id)) { id = CS_LATIN1; name = "latin-1"; }   /* no Cyrillic evidence: every byte is defined */
        *detected = true;
    } else if (streq_ci(want, "cp1251") || streq_ci(want, "windows-1251")) { id = CS_CP1251; name = "cp1251"; }
    else if (streq_ci(want, "cp866")) { id = CS_CP866; name = "cp866"; }
    else if (streq_ci(want, "koi8-r") || streq_ci(want, "koi8_r")) { id = CS_KOI8R; name = "koi8-r"; }
    else if (streq_ci(want, "ascii")) { id = CS_LATIN1; name = "ascii"; }
    else { id = CS_LATIN1; name = "latin-1"; }
    *enc = name;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = d[i];
        uint32_t c = cs_decode_hi(id, b);
        if (c == 0 && b >= 128) { *err = afmt("'%s' codec can't decode byte 0x%02x in position %zu: character maps to <undefined>", name, b, i); return false; }
        if (streq_ci(name, "ascii") && b >= 128) { *err = afmt("'ascii' codec can't decode byte 0x%02x in position %zu: ordinal not in range(128)", b, i); return false; }
        put_cp_checked(out, c);
    }
    return true;
}

Src *src_from_buf(const char *report_path, const char *data, size_t n, int *kind, char **err) {
    Buf u = {0};
    const char *enc = "utf-8";
    bool detected = false;
    if (!decode_source((const unsigned char *)data, n, &u, &enc, &detected, err)) { buf_free(&u); *kind = 1; return NULL; }
    Src *s = (Src *)amalloc(sizeof(Src));
    char *t = (char *)amalloc(u.len + 1);
    size_t o = 0;
    for (size_t i = 0; i < u.len; i++) {
        char c = u.p[i];
        if (c == '\r') { t[o++] = '\n'; if (i + 1 < u.len && u.p[i + 1] == '\n') i++; }
        else t[o++] = c;
    }
    while (o > 0 && t[o - 1] == 0) o--;        /* RT-11 pads files to a block with zero bytes: ignore them at the end */
    t[o] = 0;
    buf_free(&u);
    s->path = report_path;
    s->encoding = enc;
    s->detected = detected && strcmp(enc, "utf-8") != 0;
    s->text = t;
    s->len = o;
    return s;
}

static char *read_all(FILE *f, size_t *n) {
    size_t cap = 1 << 16, len = 0;
    char *b = (char *)xmalloc(cap);
    for (;;) {
        size_t r = fread(b + len, 1, cap - len, f);
        len += r;
        if (len < cap) { if (feof(f) || ferror(f)) break; else continue; }
        cap *= 2;
        b = (char *)xrealloc(b, cap);
    }
    *n = len;
    return b;
}

Src *src_load_file_ptr(void *fp, const char *report_path, int *kind, char **err) {
    size_t n;
    char *b = read_all((FILE *)fp, &n);
    Src *s = src_from_buf(report_path, b, n, kind, err);
    free(b);
    return s;
}

Src *src_load(const char *path, const char *report_path, int *kind, char **err) {
    FILE *f = pf_fopen(path, "rb");
    if (!f) {
        int e = errno;
        *kind = (e == ENOENT) ? 3 : 0;
        *err = afmt("[Errno %d] %s: '%s'", e, strerror(e), path);
        return NULL;
    }
    /* directories: fopen succeeds on POSIX for reading but fread fails with EISDIR */
    size_t n;
    char *b = read_all(f, &n);
    if (ferror(f) && errno == EISDIR) {
        fclose(f); free(b);
        *kind = 2;
        *err = afmt("[Errno 21] Is a directory: '%s'", path);
        return NULL;
    }
    fclose(f);
    Src *s = src_from_buf(report_path, b, n, kind, err);
    free(b);
    return s;
}

static void build_lines(Src *s) {
    if (s->line_starts) return;
    size_t cnt = 1;
    for (size_t i = 0; i < s->len; i++) if (s->text[i] == '\n') cnt++;
    s->line_starts = (size_t *)amalloc(cnt * sizeof(size_t));
    s->nlines = cnt;
    size_t k = 0;
    s->line_starts[k++] = 0;
    for (size_t i = 0; i < s->len; i++) if (s->text[i] == '\n') s->line_starts[k++] = i + 1;
}
size_t src_nlines(Src *s) { build_lines(s); return s->nlines; }
size_t src_line_start(Src *s, size_t line) { build_lines(s); return s->line_starts[line]; }

void src_linecol(Src *s, size_t off, size_t *line, size_t *col, size_t *char_col) {
    build_lines(s);
    size_t lo = 0, hi = s->nlines;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (s->line_starts[mid] <= off) lo = mid; else hi = mid;
    }
    size_t ls = s->line_starts[lo], chars = 0, tabs = 0;
    for (size_t i = ls; i < off; i++) {
        unsigned char c = (unsigned char)s->text[i];
        if ((c & 0xC0) != 0x80) chars++;
        if (c == '\t') tabs++;
    }
    *line = lo;
    *col = chars + tabs * 3;
    if (char_col) *char_col = chars;
}
