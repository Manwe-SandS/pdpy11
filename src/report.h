#ifndef REPORT_H
#define REPORT_H
#include <setjmp.h>
#include "text.h"

typedef enum { PRI_ERROR, PRI_CRITICAL, PRI_WARNING } Prio;
typedef struct { Src *src; size_t s, e; const char *text; } RepItem;

void report_init(bool graphical);
void warn_set(const char *id, bool val);
bool warn_class_expand(const char *name, const char ***ids, int *n); /* "all"/"default" */
void report_emit(Prio p, const char *id, int n, const RepItem *items);
void rep1(Prio p, const char *id, Src *a, size_t as, size_t ae, const char *at);
void rep2(Prio p, const char *id, Src *a, size_t as, size_t ae, const char *at, Src *b, size_t bs, size_t be, const char *bt);
void rep3(Prio p, const char *id, Src *a, size_t as, size_t ae, const char *at, Src *b, size_t bs, size_t be, const char *bt, Src *c, size_t cs, size_t ce, const char *ct);

extern bool g_error_condition;   /* an error/critical has been reported */
extern int g_silent;             /* >0: suppress reports (layout passes) */
extern jmp_buf g_unrecoverable;  /* target of critical errors / aborts */
extern bool g_unrec_armed;
extern jmp_buf g_layout_jmp;
extern bool g_layout_catch;
PD_NORETURN void unrecoverable(void);

#define ERR1(id, src, s, e, ...) rep1(PRI_ERROR, id, src, s, e, afmt(__VA_ARGS__))
#define WARN1(id, src, s, e, ...) rep1(PRI_WARNING, id, src, s, e, afmt(__VA_ARGS__))
#define CRIT1(id, src, s, e, ...) rep1(PRI_CRITICAL, id, src, s, e, afmt(__VA_ARGS__))
#endif
