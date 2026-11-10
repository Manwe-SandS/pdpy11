#ifndef UTIL_H
#define UTIL_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#if defined(_MSC_VER)
#define PD_NORETURN __declspec(noreturn)
#else
#define PD_NORETURN _Noreturn
#endif

/* ---- memory ---- */
void *amalloc(size_t n);                 /* arena allocation (zeroed), never freed individually */
void *axcalloc(size_t n);
char *astrndup(const char *s, size_t n); /* arena copy, NUL-terminated */
char *astrdup(const char *s);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
PD_NORETURN void die_oom(void);

/* ---- growable byte/string buffer ---- */
typedef struct { char *p; size_t len, cap; } Buf;
void buf_reserve(Buf *b, size_t extra);
void buf_putc(Buf *b, int c);
void buf_putn(Buf *b, const void *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);
void buf_put_utf8(Buf *b, uint32_t cp);
void buf_free(Buf *b);

/* ---- vector of pointers / generic ---- */
typedef struct { void **a; size_t n, cap; } PVec;
void pvec_push(PVec *v, void *x);

/* ---- unicode string (array of code points) ---- */
typedef struct { uint32_t *cp; size_t n; } U32;
extern const U32 U32_EMPTY;

/* ---- string-keyed hash map (keys are copied into arena) ---- */
typedef struct HEnt { const char *key; size_t klen; uint32_t hash; void *val; } HEnt;
typedef struct { HEnt *tab; size_t cap, n; } SMap;
void *smap_get(SMap *m, const char *k, size_t kl);
/* returns pointer to value slot; *created set if new */
void **smap_slot(SMap *m, const char *k, size_t kl, bool *created);
uint32_t str_hash(const char *k, size_t kl);

/* ---- misc ---- */
size_t lower_ascii_n(char *dst, const char *src, size_t n);
bool streq_ci(const char *a, const char *b); /* ASCII case-insensitive */

#endif
