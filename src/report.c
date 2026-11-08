#include "report.h"
#include <stdio.h>
#include <stdarg.h>

bool g_error_condition;
int g_silent;
jmp_buf g_unrecoverable;
bool g_unrec_armed;
jmp_buf g_layout_jmp;
bool g_layout_catch;

static bool g_graphical;
static SMap g_warn_ctl; /* id -> (void*)1 true, (void*)2 false */

static const char *DEFAULT_WARNS[] = {"implicit-operand", "not-implemented", "label-fixup", "excess-hash"};
static const char *ALL_WARNS[] = {"implicit-operand", "not-implemented", "suspicious-name", "excess-quote", "missing-newline", "meta-typo", "legacy-deferred", "implicit-index", "label-fixup", "excess-hash", "source-encoding"};

void report_init(bool graphical) { g_graphical = graphical; }
void warn_set(const char *id, bool val) {
    void **s = smap_slot(&g_warn_ctl, id, strlen(id), NULL);
    *s = (void *)(val ? (uintptr_t)1 : (uintptr_t)2);
}
bool warn_class_expand(const char *name, const char ***ids, int *n) {
    if (strcmp(name, "all") == 0) { *ids = ALL_WARNS; *n = 11; return true; }
    if (strcmp(name, "default") == 0) { *ids = DEFAULT_WARNS; *n = 4; return true; }
    return false;
}

PD_NORETURN void unrecoverable(void) {
    if (g_layout_catch) longjmp(g_layout_jmp, 1);
    if (g_unrec_armed) longjmp(g_unrecoverable, 1);
    fputs("internal error: unrecoverable outside handler\n", stderr);
    exit(1);
}

/* ---------- bare ---------- */
static void emit_bare(Prio p, int n, const RepItem *it) {
    for (int i = 0; i < n; i++) {
        size_t line, col;
        src_linecol(it[i].src, it[i].s, &line, &col, NULL);
        Buf b = {0};
        for (const char *t = it[i].text; *t; t++) buf_putc(&b, *t == '\n' ? ' ' : *t);
        printf("%s:%zu:%zu: %s: %s\n", it[i].src->path, line + 1, col + 1, p == PRI_WARNING ? "Warning" : "Error", b.p ? b.p : "");
        buf_free(&b);
    }
}

/* ---------- graphical ---------- */
static bool is_word(uint32_t c) {
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    if (c < 0xC0) return c == 0xAA || c == 0xB5 || c == 0xBA || (c >= 0xB2 && c <= 0xB3) || c == 0xB9 || (c >= 0xBC && c <= 0xBE);
    if (c == 0xD7 || c == 0xF7) return false;
    if (c >= 0x2000 && c <= 0x2BFF) return false;   /* punctuation, arrows, box drawing, ... */
    if (c >= 0x3000 && c <= 0x303F) return false;
    return true;
}

typedef struct { uint32_t *a; size_t n, cap; } CpVec;
static void cp_push(CpVec *v, uint32_t c) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 64; v->a = (uint32_t *)xrealloc(v->a, v->cap * 4); }
    v->a[v->n++] = c;
}
static void cp_puts(CpVec *v, const char *s) { while (*s) cp_push(v, (unsigned char)*s++); }

static bool match_bound_start(const CpVec *t, size_t i) { return i == 0 ? true : !is_word(t->a[i - 1]); }

/* colorize(): see reports.py */
static void colorize(const uint32_t *in, size_t n, CpVec *out) {
    CpVec t0 = {0}, comment = {0};
    for (size_t i = 0; i < n; i++) {
        uint32_t c = in[i];
        if (c == 0) c = 0x2400; else if (c == 1 || c == 2) continue;
        cp_push(&t0, c);
    }
    size_t cut = t0.n;
    for (size_t i = 0; i < t0.n; i++) if (t0.a[i] == ';' || (t0.a[i] == '/' && i + 1 < t0.n && t0.a[i + 1] == '/')) { cut = i; break; }
    for (size_t i = cut; i < t0.n; i++) cp_push(&comment, t0.a[i]);
    t0.n = cut;
    /* step 1: words */
    CpVec t1 = {0};
    for (size_t i = 0; i < t0.n;) {
        uint32_t c = t0.a[i];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (alpha && match_bound_start(&t0, i)) {
            size_t j = i;
            while (j < t0.n) { uint32_t d = t0.a[j]; if ((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') || d == '_') j++; else break; }
            if (j == t0.n || !is_word(t0.a[j])) {
                cp_puts(&t1, "\x01\x1b[94m\x02");
                for (size_t k = i; k < j; k++) cp_push(&t1, t0.a[k]);
                cp_puts(&t1, "\x01\x1b[39m\x02");
                i = j; continue;
            }
        }
        cp_push(&t1, c); i++;
    }
    /* step 2: registers */
    CpVec t2 = {0};
    for (size_t i = 0; i < t1.n;) {
        uint32_t c = t1.a[i];
        if (is_word(c) && match_bound_start(&t1, i) && i + 2 <= t1.n) {
            uint32_t a = t1.a[i] | 0x20, b = i + 1 < t1.n ? t1.a[i + 1] : 0;
            bool m = false;
            if (i + 1 < t1.n) {
                if (a == 'r' && b >= '0' && b <= '7') m = true;
                else if (a == 's' && (b | 0x20) == 'p') m = true;
                else if (a == 'p' && (b | 0x20) == 'c') m = true;
            }
            if (m && (i + 2 == t1.n || !is_word(t1.a[i + 2]))) {
                cp_puts(&t2, "\x01\x1b[93m\x02");
                cp_push(&t2, c); cp_push(&t2, t1.a[i + 1]);
                cp_puts(&t2, "\x01\x1b[39m\x02");
                i += 2; continue;
            }
        }
        cp_push(&t2, c); i++;
    }
    /* step 3: numbers */
    CpVec t3 = {0};
    for (size_t i = 0; i < t2.n;) {
        uint32_t c = t2.a[i];
        if (c >= '0' && c <= '9' && match_bound_start(&t2, i)) {
            size_t j = 0; bool ok = false;
            if (c == '0' && i + 1 < t2.n && (t2.a[i + 1] | 0x20) == 'x') {
                size_t k = i + 2;
                while (k < t2.n && ((t2.a[k] >= '0' && t2.a[k] <= '9') || ((t2.a[k] | 0x20) >= 'a' && (t2.a[k] | 0x20) <= 'f'))) k++;
                if (k > i + 2 && (k == t2.n || !is_word(t2.a[k]))) { j = k; ok = true; }
            }
            if (!ok && c == '0' && i + 1 < t2.n && t2.a[i + 1] == 'o') {
                size_t k = i + 2;
                while (k < t2.n && t2.a[k] >= '0' && t2.a[k] <= '7') k++;
                if (k > i + 2 && (k == t2.n || !is_word(t2.a[k]))) { j = k; ok = true; }
            }
            if (!ok) {
                size_t k = i;
                while (k < t2.n && t2.a[k] >= '0' && t2.a[k] <= '9') k++;
                if (k < t2.n && t2.a[k] == '.') {
                    if (k + 1 < t2.n && is_word(t2.a[k + 1])) { j = k + 1; ok = true; }
                    else { j = k; ok = true; }
                } else if (k == t2.n || !is_word(t2.a[k])) { j = k; ok = true; }
            }
            if (ok) {
                cp_puts(&t3, "\x01\x1b[95m\x02");
                for (size_t k = i; k < j; k++) cp_push(&t3, t2.a[k]);
                cp_puts(&t3, "\x01\x1b[39m\x02");
                i = j; continue;
            }
        }
        cp_push(&t3, c); i++;
    }
    for (size_t i = 0; i < t3.n; i++) cp_push(out, t3.a[i]);
    if (comment.n) {
        cp_puts(out, "\x01\x1b[38;5;242m\x02");
        for (size_t i = 0; i < comment.n; i++) cp_push(out, comment.a[i]);
        cp_puts(out, "\x01\x1b[39m\x02");
    }
    free(t0.a); free(comment.a); free(t1.a); free(t2.a); free(t3.a);
}

static void put_cps(Buf *b, const CpVec *v) { for (size_t i = 0; i < v->n; i++) buf_put_utf8(b, v->a[i]); }

typedef struct { size_t line_no, start_col, end_col; const char *text; long closing_line_no; long max_column; } RInfo;
typedef struct { int kind; int rep; } Ev;

static int ev_cmp_desc(const void *x, const void *y) { (void)x; (void)y; return 0; }

static void sort_events(Ev *ev, size_t n) {
    /* ascending stable by kind, then reverse */
    for (size_t i = 1; i < n; i++) {
        Ev t = ev[i]; size_t j = i;
        while (j > 0 && ev[j - 1].kind > t.kind) { ev[j] = ev[j - 1]; j--; }
        ev[j] = t;
    }
    for (size_t i = 0; i < n / 2; i++) { Ev t = ev[i]; ev[i] = ev[n - 1 - i]; ev[n - 1 - i] = t; }
}

static void expand_line(Src *s, size_t line, CpVec *out) {
    size_t a = src_line_start(s, line);
    size_t b = (line + 1 < src_nlines(s)) ? src_line_start(s, line + 1) - 1 : s->len;
    for (size_t i = a; i < b;) {
        uint32_t c = u8_next(s->text, s->len, &i);
        if (c == '\t') { for (int k = 0; k < 4; k++) cp_push(out, ' '); } else cp_push(out, c);
    }
}
static size_t raw_line_chars(Src *s, size_t line) {
    size_t a = src_line_start(s, line);
    size_t b = (line + 1 < src_nlines(s)) ? src_line_start(s, line + 1) - 1 : s->len;
    return u8_count(s->text, a, b);
}

static void emit_graphical(Prio p, const char *id, int n, const RepItem *items) {
    (void)ev_cmp_desc;
    Buf o = {0};
    int idx = 0, file_i = 0;
    while (idx < n) {
        int j = idx;
        while (j < n && items[j].src == items[idx].src) j++;   /* groupby filename (same Src <=> same file name here) */
        Src *src = items[idx].src;
        int cnt = j - idx;
        if (file_i == 0)
            buf_printf(&o, "%s in \x1b[96m%s\x1b[0m: \x1b[38;5;208m[-W%s]\x1b[0m\n", p == PRI_WARNING ? "\x1b[33mWarning\x1b[0m" : "\x1b[91mError\x1b[0m", src->path, id);
        else
            buf_printf(&o, "In \x1b[96m%s\x1b[0m:\n", src->path);
        size_t nlines = src_nlines(src);
        RInfo *rl = (RInfo *)xmalloc(sizeof(RInfo) * (size_t)cnt);
        for (int k = 0; k < cnt; k++) {
            const RepItem *r = &items[idx + k];
            size_t line, col, ccol;
            src_linecol(src, r->s, &line, &col, &ccol);
            bool has_nl = false;
            for (size_t q = r->s; q < r->e; q++) if (src->text[q] == '\n') { has_nl = true; break; }
            size_t endc;
            if (has_nl) endc = raw_line_chars(src, line);
            else endc = col + u8_count(src->text, r->s, r->e);
            rl[k] = (RInfo){line, col, endc, r->text, -1, -1};
        }
        size_t minl = rl[0].line_no, maxl = rl[0].line_no;
        for (int k = 1; k < cnt; k++) { if (rl[k].line_no < minl) minl = rl[k].line_no; if (rl[k].line_no > maxl) maxl = rl[k].line_no; }
        size_t min_line = minl >= 2 ? minl - 2 : 0;
        size_t max_line = maxl + 3 < nlines ? maxl + 3 : nlines;
        size_t nl = max_line - min_line;
        Ev **evs = (Ev **)xmalloc(sizeof(Ev *) * nl);
        size_t *evn = (size_t *)xmalloc(sizeof(size_t) * nl);
        for (size_t q = 0; q < nl; q++) { evs[q] = (Ev *)xmalloc(sizeof(Ev) * (size_t)(cnt * 3 + 4 + (maxl - minl) * cnt)); evn[q] = 0; }
        long prev = -1;
        for (int k = 0; k < cnt; k++) {
            RInfo *r = &rl[k];
            if ((long)r->line_no < prev) {
                r->closing_line_no = prev;
                evs[r->line_no - min_line][evn[r->line_no - min_line]++] = (Ev){1, k};
                for (long l = (long)r->line_no + 1; l < prev; l++) evs[(size_t)l - min_line][evn[(size_t)l - min_line]++] = (Ev){0, k};
                evs[(size_t)prev - min_line][evn[(size_t)prev - min_line]++] = (Ev){-1, k};
            } else {
                prev = (long)r->line_no;
                evs[(size_t)prev - min_line][evn[(size_t)prev - min_line]++] = (Ev){2, k};
            }
        }
        int *open = (int *)xmalloc(sizeof(int) * (size_t)(cnt + 1));
        int nopen = 0;
        long cnt_columns = 0;
        for (size_t q = 0; q < nl; q++) {
            sort_events(evs[q], evn[q]);
            for (size_t e = 0; e < evn[q]; e++) {
                Ev ev = evs[q][e];
                if (ev.kind == 1) {
                    rl[ev.rep].max_column = 0;
                    open[nopen++] = ev.rep;
                    for (int i = 0; i < nopen; i++) rl[open[i]].max_column = nopen - i - 1;
                    if (nopen > cnt_columns) cnt_columns = nopen;
                } else if (ev.kind == -1) {
                    for (int i = 0; i < nopen; i++) if (open[i] == ev.rep) { memmove(open + i, open + i + 1, sizeof(int) * (size_t)(nopen - i - 1)); nopen--; break; }
                }
            }
        }
        for (size_t q = 0; q < nl; q++) {
            size_t line_no = min_line + q;
            Ev *ev = evs[q]; size_t en = evn[q];
            sort_events(ev, en);   /* the original sorts+reverses once more in the printing loop */
            buf_printf(&o, "\x1b[92m%5zu\x1b[0m \x1b[38;5;242m\xe2\x94\x82 ", line_no + 1);
            const char **chars = (const char **)xmalloc(sizeof(char *) * (size_t)(cnt_columns + 1));
            for (long c = 0; c < cnt_columns; c++) chars[c] = "  ";
            bool has_report = false;
            for (size_t e = 0; e < en; e++) {
                long i = cnt_columns - 1 - rl[ev[e].rep].max_column;
                if (ev[e].kind == 1) chars[i] = "\xe2\x94\x8c ";          /* ┌ */
                else if (ev[e].kind == 0 || ev[e].kind == -1) chars[i] = "\xe2\x94\x82 "; /* │ */
                has_report = has_report || ev[e].kind == 1 || ev[e].kind == 2;
            }
            buf_puts(&o, "\x1b[38;5;11m");
            for (long c = 0; c < cnt_columns; c++) buf_puts(&o, chars[c]);
            buf_puts(&o, "\x1b[0m");
            CpVec line = {0};
            expand_line(src, line_no, &line);
            if (has_report) {
                buf_puts(&o, "\x1b[0m");
                CpVec col = {0}; colorize(line.a, line.n, &col); put_cps(&o, &col); free(col.a);
            } else {
                buf_puts(&o, "\x1b[38;5;242m"); put_cps(&o, &line); buf_puts(&o, "\x1b[0m");
            }
            for (size_t e = 0; e < en; e++) {
                if (ev[e].kind == 1 || ev[e].kind == 2) {
                    RInfo *r = &rl[ev[e].rep];
                    buf_printf(&o, "\x1b[%ldG\x1b[48;5;52m", 1 + 5 + 3 + 2 * cnt_columns + (long)r->start_col);
                    size_t a = r->start_col < line.n ? r->start_col : line.n, b = r->end_col < line.n ? r->end_col : line.n;
                    CpVec col = {0};
                    if (a < b) colorize(line.a + a, b - a, &col); else colorize(line.a, 0, &col);
                    put_cps(&o, &col); free(col.a);
                    buf_puts(&o, "\x1b[0m");
                }
            }
            buf_putc(&o, '\n');
            for (size_t e = 0; e < en; e++) {
                long i = cnt_columns - 1 - rl[ev[e].rep].max_column;
                if (ev[e].kind == 1) chars[i] = "\xe2\x94\x82 ";
            }
            for (size_t e = 0; e < en; e++) {
                RInfo *r = &rl[ev[e].rep];
                long i = cnt_columns - 1 - r->max_column;
                if (ev[e].kind == -1) {
                    chars[i] = "\xe2\x94\x94 "; /* └ */
                    const char **old = (const char **)xmalloc(sizeof(char *) * (size_t)(cnt_columns + 1));
                    memcpy(old, chars, sizeof(char *) * (size_t)cnt_columns);
                    for (long c = i; c < cnt_columns; c++) {
                        if (strncmp(chars[c], "\xe2\x94\x82", 3) == 0) chars[c] = "\xe2\x94\xbc\xe2\x94\x80";        /* ┼─ */
                        else if (chars[c][0] == ' ') chars[c] = "\xe2\x94\x80\xe2\x94\x80";                          /* ── */
                        else { char *t = (char *)amalloc(8); size_t l = strlen(chars[c]); /* first char + ─ */
                               size_t fl = u8_len_at(chars[c], 0); memcpy(t, chars[c], fl); memcpy(t + fl, "\xe2\x94\x80", 3); t[fl + 3] = 0; (void)l; chars[c] = t; }
                    }
                    const char *tx = r->text; int line_i = 0;
                    for (;;) {
                        const char *nl_ = strchr(tx, '\n');
                        size_t len = nl_ ? (size_t)(nl_ - tx) : strlen(tx);
                        buf_puts(&o, "      \x1b[38;5;242m\xe2\x94\x82 \x1b[38;5;11m");
                        for (long c = 0; c < cnt_columns; c++) buf_puts(&o, chars[c]);
                        size_t reps = r->start_col >= 2 ? r->start_col - 2 : 0;
                        for (size_t z = 0; z < reps; z++) buf_puts(&o, line_i == 0 ? "\xe2\x94\x80" : " ");
                        buf_puts(&o, "\xe2\x94\x80 ");
                        buf_putn(&o, tx, len);
                        buf_puts(&o, "\x1b[0m\n");
                        if (line_i == 0) { memcpy(chars, old, sizeof(char *) * (size_t)cnt_columns); chars[i] = "  "; }
                        line_i++;
                        if (!nl_) break;
                        tx = nl_ + 1;
                    }
                    free(old);
                } else if (ev[e].kind == 2) {
                    const char *tx = r->text; int line_i = 0;
                    for (;;) {
                        const char *nl_ = strchr(tx, '\n');
                        size_t len = nl_ ? (size_t)(nl_ - tx) : strlen(tx);
                        buf_puts(&o, "      \x1b[38;5;242m\xe2\x94\x82 \x1b[38;5;11m");
                        for (long c = 0; c < cnt_columns; c++) buf_puts(&o, chars[c]);
                        for (size_t z = 0; z < r->start_col; z++) buf_putc(&o, ' ');
                        buf_puts(&o, line_i == 0 ? "\xf0\x9f\xa1\xb9 " : "  ");  /* 🡹 */
                        buf_putn(&o, tx, len);
                        buf_puts(&o, "\x1b[0m\n");
                        line_i++;
                        if (!nl_) break;
                        tx = nl_ + 1;
                    }
                }
            }
            free(chars); free(line.a);
        }
        for (size_t q = 0; q < nl; q++) free(evs[q]);
        free(evs); free(evn); free(rl); free(open);
        idx = j; file_i++;
    }
    buf_putc(&o, '\n');
    fwrite(o.p, 1, o.len, stderr);
    buf_free(&o);
}

static SMap g_seen;
static bool already_reported(Prio p, const char *id, int n, const RepItem *items) {
    Buf k = {0};
    buf_printf(&k, "%d|%s|", (int)p, id);
    for (int i = 0; i < n; i++) buf_printf(&k, "%p:%zu:%zu:%s|", (void *)items[i].src, items[i].s, items[i].e, items[i].text);
    bool created;
    smap_slot(&g_seen, k.p, k.len, &created);
    buf_free(&k);
    return !created;
}

void report_emit(Prio p, const char *id, int n, const RepItem *items) {
    if (!g_silent && already_reported(p, id, n, items)) {
        if (p != PRI_WARNING) g_error_condition = true;
        if (p == PRI_CRITICAL) unrecoverable();
        return;
    }
    if (!g_silent) {
        bool show = true;
        if (p == PRI_WARNING) {
            void *c = smap_get(&g_warn_ctl, id, strlen(id));
            if (c) show = (uintptr_t)c == 1;
            else {
                show = false;
                for (int i = 0; i < 4; i++) if (strcmp(DEFAULT_WARNS[i], id) == 0) show = true;
            }
        }
        if (show) {
            fflush(stdout);
            if (g_graphical) emit_graphical(p, id, n, items); else emit_bare(p, n, items);
            fflush(stdout);
        }
    }
    if (p != PRI_WARNING && !g_silent) g_error_condition = true;
    if (p == PRI_CRITICAL) unrecoverable();
}

void rep1(Prio p, const char *id, Src *a, size_t as, size_t ae, const char *at) {
    RepItem it[1] = {{a, as, ae, at}};
    report_emit(p, id, 1, it);
}
void rep2(Prio p, const char *id, Src *a, size_t as, size_t ae, const char *at, Src *b, size_t bs, size_t be, const char *bt) {
    RepItem it[2] = {{a, as, ae, at}, {b, bs, be, bt}};
    report_emit(p, id, 2, it);
}
void rep3(Prio p, const char *id, Src *a, size_t as, size_t ae, const char *at, Src *b, size_t bs, size_t be, const char *bt, Src *c, size_t cs, size_t ce, const char *ct) {
    RepItem it[3] = {{a, as, ae, at}, {b, bs, be, bt}, {c, cs, ce, ct}};
    report_emit(p, id, 3, it);
}

void warn_source_encoding(Src *s) {
    if (!s->detected) return;
    WARN1("source-encoding", s, 0, 0, "This file is not valid UTF-8; it was decoded as %s (detected automatically). Use --source-charset to choose the encoding explicitly.", s->encoding);
}
