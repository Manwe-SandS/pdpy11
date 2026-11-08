#include "charset.h"
#include "text.h"
#include "tables.h"
#include <ctype.h>

const char RADIX50_TABLE[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.%0123456789";

bool charset_lookup(const char *name, Charset *out) {
    char n[64];
    size_t l = strlen(name);
    if (l >= sizeof n) return false;
    for (size_t i = 0; i < l; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c == '-' || c == ' ') c = '_';
        n[i] = c;
    }
    n[l] = 0;
    struct { const char *n; CharsetId id; } T[] = {
        {"bk", CS_BK},
        {"ascii", CS_ASCII}, {"us_ascii", CS_ASCII}, {"646", CS_ASCII}, {"ansi_x3.4_1968", CS_ASCII}, {"ansi_x3_4_1968", CS_ASCII}, {"iso646_us", CS_ASCII}, {"iso_646.irv_1991", CS_ASCII},
        {"utf_8", CS_UTF8}, {"utf8", CS_UTF8}, {"u8", CS_UTF8}, {"utf", CS_UTF8}, {"utf8_ucs2", CS_UTF8}, {"utf8_ucs4", CS_UTF8}, {"cp65001", CS_UTF8},
        {"latin_1", CS_LATIN1}, {"latin1", CS_LATIN1}, {"iso_8859_1", CS_LATIN1}, {"iso8859_1", CS_LATIN1}, {"8859", CS_LATIN1}, {"cp819", CS_LATIN1}, {"l1", CS_LATIN1}, {"latin", CS_LATIN1}, {"iso_ir_100", CS_LATIN1}, {"ibm819", CS_LATIN1},
        {"cp1251", CS_CP1251}, {"windows_1251", CS_CP1251},
        {"cp866", CS_CP866}, {"866", CS_CP866}, {"ibm866", CS_CP866}, {"csibm866", CS_CP866},
        {"koi8_r", CS_KOI8R}, {"cskoi8r", CS_KOI8R},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (strcmp(T[i].n, n) == 0) { out->id = T[i].id; out->raw_name = name; return true; }
    return false;
}

static int bk_lookup(uint32_t cp) {
    size_t lo = 0, hi = N_BK_ENC;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (bk_enc[m].cp == cp) return bk_enc[m].b;
        if (bk_enc[m].cp < cp) lo = m + 1; else hi = m;
    }
    return -1;
}

static int sb_lookup(const uint16_t *hi, uint32_t cp) {
    if (cp < 128) return (int)cp;
    for (int i = 0; i < 128; i++) if (hi[i] == cp) return 128 + i;
    return -1;
}

/* returns byte (0..255) or -1 for unencodable; utf8 handled separately */
static int enc_one(CharsetId id, uint32_t cp) {
    switch (id) {
    case CS_BK: return bk_lookup(cp);
    case CS_ASCII: return cp < 128 ? (int)cp : -1;
    case CS_LATIN1: return cp < 256 ? (int)cp : -1;
    case CS_CP1251: return sb_lookup(cs_cp1251_hi, cp);
    case CS_CP866: return sb_lookup(cs_cp866_hi, cp);
    case CS_KOI8R: return sb_lookup(cs_koi8_r_hi, cp);
    default: return -1;
    }
}

static char *esc_char(uint32_t cp) {
    if (cp <= 0xff) return afmt("\\x%02x", cp);
    if (cp <= 0xffff) return afmt("\\u%04x", cp);
    return afmt("\\U%08x", cp);
}

static bool is_bad(CharsetId id, uint32_t cp) {
    if (id == CS_UTF8) return cp >= 0xD800 && cp <= 0xDFFF;
    return enc_one(id, cp) < 0;
}

bool cs_encode(const Charset *cs, const U32 *s, Buf *out, char **err) {
    size_t n = s->n;
    for (size_t i = 0; i < n; i++) {
        if (!is_bad(cs->id, s->cp[i])) continue;
        size_t start, end;
        const char *enc, *reason;
        if (cs->id == CS_BK) {
            /* custom codec: first bad .. last bad (inclusive of valid chars in between) */
            start = 0; while (!is_bad(cs->id, s->cp[start])) start++;
            end = n; while (!is_bad(cs->id, s->cp[end - 1])) end--;
            enc = "bk"; reason = "invalid character";
        } else {
            start = i; end = i;
            while (end < n && is_bad(cs->id, s->cp[end])) end++;
            switch (cs->id) {
            case CS_ASCII: enc = "ascii"; reason = "ordinal not in range(128)"; break;
            case CS_LATIN1: enc = "latin-1"; reason = "ordinal not in range(256)"; break;
            case CS_UTF8: enc = "utf-8"; reason = "surrogates not allowed"; break;
            default: enc = "charmap"; reason = "character maps to <undefined>"; break;
            }
        }
        if (end == start + 1)
            *err = afmt("'%s' codec can't encode character '%s' in position %zu: %s", enc, esc_char(s->cp[start]), start, reason);
        else
            *err = afmt("'%s' codec can't encode characters in position %zu-%zu: %s", enc, start, end - 1, reason);
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (cs->id == CS_UTF8) buf_put_utf8(out, s->cp[i]);
        else buf_putc(out, enc_one(cs->id, s->cp[i]));
    }
    return true;
}

uint32_t cs_decode_hi(CharsetId id, unsigned char b) {
    if (b < 128) return b;
    switch (id) {
    case CS_CP1251: return cs_cp1251_hi[b - 128];
    case CS_CP866: return cs_cp866_hi[b - 128];
    case CS_KOI8R: return cs_koi8_r_hi[b - 128];
    case CS_LATIN1: return b;
    default: return 0;
    }
}
