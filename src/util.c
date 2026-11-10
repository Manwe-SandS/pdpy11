#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

_Noreturn void die_oom(void) {
    fputs("Out of memory\n", stderr);
    exit(1);
}

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die_oom(); return p; }
void *xrealloc(void *p, size_t n) { void *q = realloc(p, n ? n : 1); if (!q) die_oom(); return q; }

/* ---- arena: blocks growing from 64 KB up to 1 MB ---- */
typedef struct Block { struct Block *next; size_t used, cap; } Block;
static Block *g_blocks;
static size_t g_next_cap = 64 * 1024;

void *amalloc(size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (n == 0) n = 8;
    if (n > 256 * 1024) {
        Block *b = (Block *)calloc(1, sizeof(Block) + n);
        if (!b) die_oom();
        b->cap = b->used = n;
        b->next = g_blocks ? g_blocks->next : NULL;
        if (g_blocks) g_blocks->next = b; else g_blocks = b;
        return (char *)(b + 1);
    }
    if (!g_blocks || g_blocks->used + n > g_blocks->cap) {
        size_t cap = g_next_cap;
        while (cap < n) cap *= 2;      /* a request up to 256 KB must fit into the new block, even if the block size is still smaller */
        Block *b = (Block *)calloc(1, sizeof(Block) + cap);
        if (!b) die_oom();
        b->cap = cap;
        b->next = g_blocks;
        g_blocks = b;
        if (g_next_cap < 1024 * 1024) g_next_cap *= 2;
    }
    void *p = (char *)(g_blocks + 1) + g_blocks->used;
    g_blocks->used += n;
    return p;
}
void *axcalloc(size_t n) { return amalloc(n); }
char *astrndup(const char *s, size_t n) {
    char *p = (char *)amalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
char *astrdup(const char *s) { return astrndup(s, strlen(s)); }

/* ---- Buf ---- */
void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->len + extra + 1) nc *= 2;
        b->p = (char *)xrealloc(b->p, nc);
        b->cap = nc;
    }
}
void buf_putc(Buf *b, int c) { buf_reserve(b, 1); b->p[b->len++] = (char)c; b->p[b->len] = 0; }
void buf_putn(Buf *b, const void *s, size_t n) { buf_reserve(b, n); memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0; }
void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }
void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    buf_reserve(b, (size_t)n);
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}
void buf_put_utf8(Buf *b, uint32_t cp) {
    if (cp < 0x80) buf_putc(b, (int)cp);
    else if (cp < 0x800) { buf_putc(b, 0xC0 | (int)(cp >> 6)); buf_putc(b, 0x80 | (int)(cp & 0x3F)); }
    else if (cp < 0x10000) { buf_putc(b, 0xE0 | (int)(cp >> 12)); buf_putc(b, 0x80 | (int)((cp >> 6) & 0x3F)); buf_putc(b, 0x80 | (int)(cp & 0x3F)); }
    else { buf_putc(b, 0xF0 | (int)(cp >> 18)); buf_putc(b, 0x80 | (int)((cp >> 12) & 0x3F)); buf_putc(b, 0x80 | (int)((cp >> 6) & 0x3F)); buf_putc(b, 0x80 | (int)(cp & 0x3F)); }
}
void buf_free(Buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

void pvec_push(PVec *v, void *x) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 4; v->a = (void **)xrealloc(v->a, v->cap * sizeof(void *)); }
    v->a[v->n++] = x;
}

const U32 U32_EMPTY = { NULL, 0 };

/* ---- hash map ---- */
uint32_t str_hash(const char *k, size_t kl) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < kl; i++) { h ^= (unsigned char)k[i]; h *= 16777619u; }
    return h;
}
static void smap_grow(SMap *m) {
    size_t nc = m->cap ? m->cap * 2 : 64;
    HEnt *nt = (HEnt *)xmalloc(nc * sizeof(HEnt));
    memset(nt, 0, nc * sizeof(HEnt));
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->tab[i].key) continue;
        size_t j = m->tab[i].hash & (nc - 1);
        while (nt[j].key) j = (j + 1) & (nc - 1);
        nt[j] = m->tab[i];
    }
    free(m->tab);
    m->tab = nt;
    m->cap = nc;
}
void **smap_slot(SMap *m, const char *k, size_t kl, bool *created) {
    if (m->n * 2 >= m->cap) smap_grow(m);
    uint32_t h = str_hash(k, kl);
    size_t j = h & (m->cap - 1);
    while (m->tab[j].key) {
        if (m->tab[j].hash == h && m->tab[j].klen == kl && memcmp(m->tab[j].key, k, kl) == 0) {
            if (created) *created = false;
            return &m->tab[j].val;
        }
        j = (j + 1) & (m->cap - 1);
    }
    m->tab[j].key = astrndup(k, kl);
    m->tab[j].klen = kl;
    m->tab[j].hash = h;
    m->tab[j].val = NULL;
    m->n++;
    if (created) *created = true;
    return &m->tab[j].val;
}
void *smap_get(SMap *m, const char *k, size_t kl) {
    if (!m->cap) return NULL;
    uint32_t h = str_hash(k, kl);
    size_t j = h & (m->cap - 1);
    while (m->tab[j].key) {
        if (m->tab[j].hash == h && m->tab[j].klen == kl && memcmp(m->tab[j].key, k, kl) == 0) return m->tab[j].val;
        j = (j + 1) & (m->cap - 1);
    }
    return NULL;
}

size_t lower_ascii_n(char *dst, const char *src, size_t n) {
    for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)src[i]; dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c; }
    dst[n] = 0;
    return n;
}
bool streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
        a++; b++;
    }
    return *a == *b;
}
