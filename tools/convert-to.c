/* convert-to: rewrite an assembler source in the style of PDPy11 or of Turbo8.

     convert-to pdpy11|turbo  file.mac      ->  file.mac.pdpy11  /  file.mac.turbo

   Both modes
     - macro definitions (.macro ... .endm) are removed and every macro call is replaced by its expansion
       (the call stays as a comment line); macro libraries pulled in with .include are read to learn the macros,
       and an .include of a file that only defines macros is dropped.
   pdpy11 mode
     - character constants are written in double quotes: movb #'.,r0 -> movb #".",r0
     - register synonyms (%n = name) are removed and the name is replaced by rn
     - the built-in macros .addr and .print are expanded as well (to real instructions)
     - comments become ';'   (// and slash-star comments are rewritten)
     - .la -> .link, .insert -> insert_file
   turbo mode
     - .addr and .print are left alone (Turbo8 knows them)
     - ';' comments become '//'
     - (both modes) a changed line is preceded by a comment 'was: ...' with the original text
     - .link -> .la, insert_file -> .insert
     - directives that Turbo8 does not have are reported on stderr
     - .end is appended when the source has none (Turbo8 requires it)

   Every changed line is preceded by a comment 'was: <original line>'; removed macro definitions and synonym
   declarations leave a comment too.
   Operator precedence (| ^ & << >> + - * / %) differs between the assemblers; mixed expressions get explicit
   < > groups so that the value stays the same (--from=pdpy11|turbo tells the dialect of the source).

   --from=micro11 (sources of the Micro-11 assembler)
     - a comment after the operands without ';' is recognised: when the text after the operands does not begin with an
       instruction, directive or macro name, it becomes a comment
     - ".+n" -> ".blkb n"; TYIN -> emt 6; TOUT -> emt 16
     - a label defined again gets a new name (NAME_2, NAME_3, ...); references below it use the newest name
     - 'AJ' (two characters closed by a quote) -> "AJ ;  'Y' -> 'Y

   Build: make convert-to */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include "../src/macro.h"
#include "../src/cond.h"

typedef enum { M_PDPY11, M_TURBO } Mode;
#define MAX_DEPTH 64

static Mode g_mode;
static int g_errors;
static bool g_saw_end;                      /* a top-level .end was seen */

static bool blank(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }
static bool wc(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '.'; }

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    Buf b = {0};
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_putn(&b, tmp, n);
    fclose(f);
    buf_putc(&b, 0);
    *len = b.len - 1;
    return b.p;
}

static char *dir_of(const char *path) {
    const char *s = strrchr(path, '/');
    if (!s) return astrdup(".");
    return astrndup(path, s == path ? 1 : (size_t)(s - path));
}

/* ---- comments ---- */

/* Rewrite the comments of one source line (no newline) and append the result to o.
   *in_block is true while a slash-star comment spans lines. */
static void convert_line(const char *l, size_t n, bool *in_block, Buf *o) {
    size_t i = 0;
    if (g_mode == M_TURBO) {
        while (i < n) {
            char c = l[i];
            if (*in_block) {
                if (c == '*' && i + 1 < n && l[i + 1] == '/') { buf_puts(o, "*/"); i += 2; *in_block = false; }
                else { buf_putc(o, c); i++; }
            } else if (c == '"') {
                size_t j = i + 1;
                while (j < n && l[j] != '"') j++;
                if (j < n) j++;
                buf_putn(o, l + i, j - i); i = j;
            } else if (c == '\'') {
                size_t j = i + 2 <= n ? i + 2 : n;
                buf_putn(o, l + i, j - i); i = j;
            } else if (c == ';') { buf_puts(o, "//"); i++; buf_putn(o, l + i, n - i); i = n; }
            else if (c == '/' && i + 1 < n && l[i + 1] == '/') { buf_putn(o, l + i, n - i); i = n; }
            else if (c == '/' && i + 1 < n && l[i + 1] == '*') { buf_puts(o, "/*"); i += 2; *in_block = true; }
            else { buf_putc(o, c); i++; }
        }
        return;
    }
    /* PDPy11: code stays, every comment piece goes to the end of the line after ';' */
    Buf code = {0}, cm = {0};
    bool have_cm = false;
    #define ADD_COMMENT(s, e) do { \
        size_t _a = (s), _b = (e); \
        while (_b > _a && (l[_b - 1] == ' ' || l[_b - 1] == '\t')) _b--; \
        if (have_cm) { buf_putc(&cm, ' '); while (_a < _b && blank(l[_a])) _a++; } \
        buf_putn(&cm, l + _a, _b - _a); have_cm = true; } while (0)
    if (*in_block) {
        size_t j = 0;
        while (j + 1 < n && !(l[j] == '*' && l[j + 1] == '/')) j++;
        if (j + 1 < n) { ADD_COMMENT(0, j); i = j + 2; *in_block = false; }
        else { ADD_COMMENT(0, n); i = n; }
    }
    while (i < n) {
        char c = l[i];
        if (c == '"') {
            size_t j = i + 1;
            while (j < n && l[j] != '"') j++;
            if (j < n) j++;
            buf_putn(&code, l + i, j - i); i = j;
        } else if (c == '\'') {
            size_t j = i + 2 <= n ? i + 2 : n;
            buf_putn(&code, l + i, j - i); i = j;
        } else if (c == ';') { ADD_COMMENT(i + 1, n); i = n; }
        else if (c == '/' && i + 1 < n && l[i + 1] == '/') { ADD_COMMENT(i + 2, n); i = n; }
        else if (c == '/' && i + 1 < n && l[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < n && !(l[j] == '*' && l[j + 1] == '/')) j++;
            if (j + 1 < n) { ADD_COMMENT(i + 2, j); i = j + 2; }
            else { ADD_COMMENT(i + 2, n); i = n; *in_block = true; }
        } else { buf_putc(&code, c); i++; }
    }
    #undef ADD_COMMENT
    buf_putn(o, code.p, code.len);
    if (have_cm) {
        if (code.len && !blank((unsigned char)code.p[code.len - 1])) buf_putc(o, '\t');
        buf_putc(o, ';');
        buf_putn(o, cm.p, cm.len);
    }
    buf_free(&code); buf_free(&cm);
}

/* ---- line analysis ---- */

/* label part and first word of a line: [ws] [label:] [ws] word */
typedef struct { size_t lab_end, ws, we; } Words;      /* label text is l[0..lab_end), word is l[ws..we) */

static Words find_word(const char *l, size_t n) {
    Words w = {0, 0, 0};
    size_t p = 0;
    while (p < n && blank(l[p])) p++;
    size_t s = p;
    while (p < n && wc(l[p])) p++;
    if (p < n && l[p] == ':' && p > s) {
        p++;
        w.lab_end = p;
        while (p < n && blank(l[p])) p++;
        s = p;
        while (p < n && wc(l[p])) p++;
    }
    w.ws = s; w.we = p;
    return w;
}

static bool word_is(const char *l, Words w, const char *name) {
    size_t k = w.we - w.ws;
    if (strlen(name) != k) return false;
    for (size_t i = 0; i < k; i++) {
        char c = l[w.ws + i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != name[i]) return false;
    }
    return true;
}

static const char *const NO_TURBO[] = {".once", ".repeat", ".dword", ".odd", ".error", ".list", ".title", ".extern", NULL};



/* ---- constants redefined inside macros (MACRO-11 allows "x = 1 ... x = 2"; PDPy11 does not) ---- */

#define MAX_REDEF 256
static struct { char name[96]; long v; } g_redef[MAX_REDEF];
static int g_nredef;
static SMap g_assigned_names;

static void redef_set(const char *name, size_t n, long v) {
    char lo[96];
    if (n >= sizeof lo) return;
    lower_ascii_n(lo, name, n); lo[n] = 0;
    for (int i = 0; i < g_nredef; i++) if (!strcmp(g_redef[i].name, lo)) { g_redef[i].v = v; return; }
    if (g_nredef < MAX_REDEF) { strcpy(g_redef[g_nredef].name, lo); g_redef[g_nredef++].v = v; }
}

static bool redef_find(const char *name, size_t n, long *v) {
    char lo[96];
    if (n >= sizeof lo) return false;
    lower_ascii_n(lo, name, n); lo[n] = 0;
    for (int i = 0; i < g_nredef; i++) if (!strcmp(g_redef[i].name, lo)) { *v = g_redef[i].v; return true; }
    return false;
}

/* register_index() is what cond.c needs for .ntype: r0-r7, sp, pc and the synonyms declared so far */
int register_index(const char *s, size_t n);

/* ---- register synonyms:  %n = name  (Turbo8) ---- */

#define MAX_SYN 64
static struct { char name[64]; int reg; } g_syn[MAX_SYN];
static int g_nsyn;

static void syn_add(const char *name, size_t n, int reg) {
    if (n >= sizeof g_syn[0].name) return;
    char lo[64];
    lower_ascii_n(lo, name, n); lo[n] = 0;
    for (int i = 0; i < g_nsyn; i++) if (strcmp(g_syn[i].name, lo) == 0) { g_syn[i].reg = reg; return; }
    if (g_nsyn < MAX_SYN) { strcpy(g_syn[g_nsyn].name, lo); g_syn[g_nsyn++].reg = reg; }
}

static int syn_find(const char *name, size_t n) {
    char lo[64];
    if (n >= sizeof lo) return -1;
    lower_ascii_n(lo, name, n); lo[n] = 0;
    for (int i = 0; i < g_nsyn; i++) if (strcmp(g_syn[i].name, lo) == 0) return g_syn[i].reg;
    return -1;
}

int register_index(const char *s, size_t n) {
    if (n == 2 && (s[0] == 'r' || s[0] == 'R') && s[1] >= '0' && s[1] <= '7') return s[1] - '0';
    if (n == 2 && (s[0] | 32) == 's' && (s[1] | 32) == 'p') return 6;
    if (n == 2 && (s[0] | 32) == 'p' && (s[1] | 32) == 'c') return 7;
    return syn_find(s, n);
}

/* "%n = name" (only blanks or a comment may follow); returns true and the parts if the line is one */
static bool parse_syn_decl(const char *l, size_t n, int *reg, size_t *ns, size_t *ne) {
    size_t p = 0;
    while (p < n && blank(l[p])) p++;
    if (p + 1 >= n || l[p] != '%' || l[p + 1] < '0' || l[p + 1] > '7') return false;
    *reg = l[p + 1] - '0';
    p += 2;
    while (p < n && blank(l[p])) p++;
    if (p >= n || l[p] != '=') return false;
    p++;
    while (p < n && blank(l[p])) p++;
    *ns = p;
    while (p < n && wc(l[p])) p++;
    *ne = p;
    if (*ne == *ns || (l[*ns] >= '0' && l[*ns] <= '9')) return false;
    while (p < n && blank(l[p])) p++;
    return p >= n || l[p] == ';' || (l[p] == '/' && p + 1 < n && (l[p + 1] == '/' || l[p + 1] == '*'));
}

/* ---- expression precedence ----
   The two assemblers order the binary operators differently:
     PDPy11 (low -> high):  |   ^   &   << >>   + -   * / %
     Turbo8 (low -> high):  | ^ (one level)   &   + -   * / % << >> (one level)
   An expression written for one is parsed with its table and written for the other one with explicit < > groups. */

static bool g_chars = true;                 /* rewrite 'c as "c" */
static int g_src_turbo;                     /* dialect of the source text */

static int op_prec(int op, int turbo) {
    switch (op) {
    case '|': return 1;
    case '^': return turbo ? 1 : 2;
    case '&': return turbo ? 2 : 3;
    case 'L': case 'R': return turbo ? 4 : 4;
    case '+': case '-': return turbo ? 3 : 5;
    default: return turbo ? 4 : 6;           /* * / % */
    }
}

typedef struct Node { int kind; int op; const char *a; size_t al; struct Node *l, *r; } Node;   /* 0 atom 1 binary 2 unary 3 group */
typedef struct { const char *s; size_t n, i; bool ok; } Pz;

static Node *mk(int kind) { return (Node *)amalloc(sizeof(Node)); (void)kind; }

static void pz_ws(Pz *z) { while (z->i < z->n && blank(z->s[z->i])) z->i++; }

static int peek_op(Pz *z, size_t *len) {
    pz_ws(z);
    if (z->i >= z->n) return 0;
    char c = z->s[z->i];
    *len = 1;
    if (c == '<' && z->i + 1 < z->n && z->s[z->i + 1] == '<') { *len = 2; return 'L'; }
    if (c == '>' && z->i + 1 < z->n && z->s[z->i + 1] == '>') { *len = 2; return 'R'; }
    if (c == '|' || c == '^' || c == '&' || c == '+' || c == '-' || c == '*' || c == '/' || c == '%') return c;
    return 0;
}

static Node *pz_expr(Pz *z, int minp);

static Node *pz_unary(Pz *z) {
    pz_ws(z);
    if (z->i >= z->n) { z->ok = false; return NULL; }
    char c = z->s[z->i];
    Node *nd = mk(0);
    if (c == '-' || c == '+' || c == '~') {
        z->i++;
        nd->kind = 2; nd->op = c;
        nd->l = pz_unary(z);
        return z->ok ? nd : NULL;
    }
    if (c == '<' || c == '(') {
        z->i++;
        nd->kind = 3; nd->op = c;
        nd->l = pz_expr(z, 1);
        pz_ws(z);
        if (!z->ok || z->i >= z->n || z->s[z->i] != (c == '<' ? '>' : ')')) { z->ok = false; return NULL; }
        z->i++;
        return nd;
    }
    size_t a = z->i;
    if (c == '\'' || c == '"') z->i = a + 2 <= z->n ? a + 2 : z->n;
    else if (c == '^' && a + 1 < z->n && wc(z->s[a + 1])) { z->i = a + 2; while (z->i < z->n && wc(z->s[z->i])) z->i++; }
    else if (wc(c)) { while (z->i < z->n && wc(z->s[z->i])) z->i++; }
    else { z->ok = false; return NULL; }
    nd->kind = 0; nd->a = z->s + a; nd->al = z->i - a;
    return nd;
}

static Node *pz_expr(Pz *z, int minp) {
    Node *left = pz_unary(z);
    while (z->ok) {
        size_t len;
        int op = peek_op(z, &len);
        if (!op) break;
        int p = op_prec(op, g_src_turbo);
        if (p < minp) break;
        z->i += len;
        Node *r = pz_expr(z, p + 1);
        if (!z->ok) return NULL;
        Node *b = mk(1);
        b->kind = 1; b->op = op; b->l = left; b->r = r;
        left = b;
    }
    return left;
}

static bool is_reg_name(const char *a, size_t n) {
    if (n == 2 && (a[0] == 'r' || a[0] == 'R') && a[1] >= '0' && a[1] <= '7') return true;
    if (n == 2 && (((a[0] | 32) == 's' && (a[1] | 32) == 'p') || ((a[0] | 32) == 'p' && (a[1] | 32) == 'c'))) return true;
    return syn_find(a, n) >= 0;
}

static bool has_reg(const Node *nd) {
    if (!nd) return false;
    if (nd->kind == 0) return is_reg_name(nd->a, nd->al);
    return has_reg(nd->l) || has_reg(nd->r);
}

static void emit_node(const Node *nd, int turbo_out, int parent, bool right, Buf *o, int *added) {
    switch (nd->kind) {
    case 0: buf_putn(o, nd->a, nd->al); return;
    case 2: buf_putc(o, nd->op); emit_node(nd->l, turbo_out, 9, false, o, added); return;
    case 3: {
        /* PDPy11 style groups are round, Turbo8 ones angular; a group that mentions a register stays as it was, because
           (r1) would turn into a register-deferred operand */
        char want = turbo_out ? '<' : '(';
        if (has_reg(nd->l)) want = (char)nd->op;
        if (want != nd->op) (*added)++;
        buf_putc(o, want); emit_node(nd->l, turbo_out, 0, false, o, added); buf_putc(o, want == '<' ? '>' : ')');
        return;
    }
    }
    int p = op_prec(nd->op, turbo_out);
    bool par = parent > 0 && (p < parent || (p == parent && right));
    char pc = turbo_out ? '<' : '(';
    if (par) { buf_putc(o, pc); (*added)++; }
    emit_node(nd->l, turbo_out, p, false, o, added);
    if (nd->op == 'L') buf_puts(o, "<<"); else if (nd->op == 'R') buf_puts(o, ">>"); else buf_putc(o, nd->op);
    size_t before = o->len;
    emit_node(nd->r, turbo_out, p, true, o, added);
    if (o->len > before && o->p[before] == '<') {                 /* keep "<<" and "<" apart */
        buf_putc(o, 0); o->len--;
        memmove(o->p + before + 1, o->p + before, o->len - before); o->p[before] = ' '; o->len++;
    }
    if (par) buf_putc(o, turbo_out ? '>' : ')');
}

/* rewrite one operand (the expression part of it); appends to o */
static void fix_operand(const char *t, size_t n, Buf *o) {
    size_t a = 0, b = n;
    while (a < b && (blank(t[a]) || t[a] == '#' || t[a] == '@' || t[a] == '=')) a++;
    while (b > a && blank(t[b - 1])) b--;
    size_t tail = b;
    if (b > a && t[b - 1] == '+' ) { /* (r)+ */ }
    size_t e = b;
    if (e > a && t[e - 1] == '+' && e - 1 > a && t[e - 2] == ')') e--;
    if (e > a && t[e - 1] == ')') {                          /* index(reg): the expression is before '(' */
        int depth = 0; size_t k = e;
        while (k > a) { k--; if (t[k] == ')') depth++; else if (t[k] == '(' && --depth == 0) break; }
        e = k;
    }
    (void)tail;
    if (e <= a) { buf_putn(o, t, n); return; }
    Pz z = {t + a, e - a, 0, true};
    Node *tree = pz_expr(&z, 1);
    pz_ws(&z);
    if (!z.ok || !tree || z.i != z.n) { buf_putn(o, t, n); return; }
    Buf tmp = {0};
    int added = 0;
    emit_node(tree, g_mode == M_TURBO, 0, false, &tmp, &added);
    if (!added) { buf_putn(o, t, n); buf_free(&tmp); return; }
    buf_putn(o, t, a);
    buf_putn(o, tmp.p, tmp.len);
    buf_putn(o, t + e, n - e);
    buf_free(&tmp);
}

/* operands of one code run: split at top-level commas */
static void fix_precedence(const char *t, size_t n, Buf *o) {
    size_t i = 0, st = 0;
    int depth = 0;
    while (i <= n) {
        if (i == n || (t[i] == ',' && depth == 0)) {
            fix_operand(t + st, i - st, o);
            if (i < n) buf_putc(o, ',');
            st = i + 1; i++;
            continue;
        }
        char c = t[i];
        if ((c == '<' || c == '>') && i + 1 < n && t[i + 1] == c) { i += 2; continue; }
        if (c == '(' || c == '<') depth++;
        else if ((c == ')' || c == '>') && depth > 0) depth--;
        i++;
    }
}

/* replace register synonyms by register names in a code run */
static void fix_synonyms(const char *t, size_t n, Buf *o) {
    size_t i = 0;
    while (i < n) {
        if (wc((unsigned char)t[i])) {
            size_t j = i;
            while (j < n && wc((unsigned char)t[j])) j++;
            int r = (t[i] >= '0' && t[i] <= '9') ? -1 : syn_find(t + i, j - i);
            if (r >= 0) { buf_putc(o, 'r'); buf_putc(o, '0' + r); } else buf_putn(o, t + i, j - i);
            i = j;
        } else { buf_putc(o, t[i]); i++; }
    }
}

/* names redefined inside macros are replaced by the value in force: "x" -> "5." */
static void fix_redef(const char *t, size_t n, Buf *o) {
    size_t i = 0;
    while (i < n) {
        if (wc((unsigned char)t[i])) {
            size_t j = i;
            while (j < n && wc((unsigned char)t[j])) j++;
            long v;
            if (!(t[i] >= '0' && t[i] <= '9') && redef_find(t + i, j - i, &v)) buf_printf(o, "%ld.", v);
            else buf_putn(o, t + i, j - i);
            i = j;
        } else { buf_putc(o, t[i]); i++; }
    }
}

/* a name beginning with '$' cannot start a line after a word list in PDPy11: $name -> S$name */
static void fix_dollar(const char *t, size_t n, Buf *o) {
    size_t i = 0;
    while (i < n) {
        if (wc((unsigned char)t[i])) {
            size_t j = i;
            while (j < n && wc((unsigned char)t[j])) j++;
            if (t[i] == '$' && j - i > 1) buf_putc(o, 'S');
            buf_putn(o, t + i, j - i);
            i = j;
        } else { buf_putc(o, t[i]); i++; }
    }
}

static void fix_run(const char *t, size_t n, Buf *o0) {
    Buf dl = {0};
    Buf *o = o0;
    if (g_mode == M_PDPY11) { o = &dl; }
    Buf a = {0}, b = {0};
    if (g_nredef) fix_redef(t, n, &b); else buf_putn(&b, t, n);
    if (g_mode == M_PDPY11 && g_nsyn) fix_synonyms(b.p, b.len, &a); else buf_putn(&a, b.p, b.len);
    buf_free(&b);
    if (g_mode == M_PDPY11 || g_src_turbo != (g_mode == M_TURBO)) fix_precedence(a.p, a.len, o); else buf_putn(o, a.p, a.len);
    buf_free(&a);
    if (o == &dl) { fix_dollar(dl.p ? dl.p : "", dl.len, o0); buf_free(&dl); }
}

/* apply the fixes to the code parts of a line (strings and comments are copied unchanged) */
static void rewrite_code(const char *l, size_t n, bool in_block, size_t first_off, Buf *o) {
    size_t i = 0, run = 0;
    #define FLUSH(e) do { size_t _a = run, _b = (e); \
        if (_a < first_off) { size_t _m = _b < first_off ? _b : first_off; if (g_mode == M_PDPY11) fix_dollar(l + _a, _m - _a, o); else buf_putn(o, l + _a, _m - _a); _a = _m; } \
        if (_b > _a) fix_run(l + _a, _b - _a, o); } while (0)
    if (in_block) {
        while (i + 1 < n && !(l[i] == '*' && l[i + 1] == '/')) i++;
        if (i + 1 >= n) { buf_putn(o, l, n); return; }
        i += 2; buf_putn(o, l, i); run = i;
    }
    while (i < n) {
        char c = l[i];
        if (c == '"' || c == '\'') {
            FLUSH(i);
            size_t j;
            if (c == '"') { j = i + 1; while (j < n && l[j] != '"') j++; if (j < n) j++; }
            else j = i + 2 <= n ? i + 2 : n;
            unsigned char ch = (c == '\'' && j == i + 2) ? (unsigned char)l[i + 1] : 0;
            if (g_chars && g_mode == M_PDPY11 && ch > ' ' && ch < 0x7f && ch != '"' && ch != '\\' && (i == 0 || !wc((unsigned char)l[i - 1])))
                { buf_putc(o, '"'); buf_putc(o, ch); buf_putc(o, '"'); }     /* 'c  ->  "c" */
            else buf_putn(o, l + i, j - i);
            i = j; run = i;
        } else if (c == ';' || (c == '/' && i + 1 < n && l[i + 1] == '/')) {
            FLUSH(i); buf_putn(o, l + i, n - i); return;
        } else if (c == '/' && i + 1 < n && l[i + 1] == '*') {
            FLUSH(i);
            size_t j = i + 2;
            while (j + 1 < n && !(l[j] == '*' && l[j + 1] == '/')) j++;
            j = j + 1 < n ? j + 2 : n;
            buf_putn(o, l + i, j - i); i = j; run = i;
        } else i++;
    }
    FLUSH(n);
    #undef FLUSH
}

/* ---- the converter ---- */


/* ---- conditional assembly and repeat blocks ---- */

typedef struct { bool cond, branch, parent; } CFrame;

/* body of a repeat block: the lines after `p` (the end of the header line) up to the matching .endr */
static bool repeat_body(const char *text, size_t len, size_t p, size_t *bs, size_t *be, size_t *after) {
    const char *nl = memchr(text + p, '\n', len - p);
    p = nl ? (size_t)(nl - text) + 1 : len;
    *bs = p;
    int depth = 0;
    while (p < len) {
        const char *e = memchr(text + p, '\n', len - p);
        size_t le = e ? (size_t)(e - text) : len;
        char w[16];
        if (macro_line_word(text, p, le, w, sizeof w)) {
            if (!strcmp(w, ".irp") || !strcmp(w, ".irpc") || !strcmp(w, ".rept")) depth++;
            else if (!strcmp(w, ".endr")) { if (depth == 0) { *be = p; *after = le; return true; } depth--; }
        }
        p = e ? le + 1 : len;
    }
    return false;
}

static void process(const char *text, size_t len, const char *path, int depth, Buf *out, bool top);

/* .ascii / .asciz operands in MACRO-11 style: <15><12> are byte values, any other delimiter (\text\, |text|) opens a string:
   the strings are written in double quotes, the <...> parts are kept (they are not arithmetic groups) */
static void fix_ascii(const char *l, size_t n, size_t from, Buf *o) {
    fix_dollar(l, from, o);
    size_t i = from;
    while (i < n) {
        char c = l[i];
        if (c == ';') { buf_putn(o, l + i, n - i); return; }
        if (c == '<') {
            size_t j = i; int d = 0;
            while (j < n) { if (l[j] == '<') d++; else if (l[j] == '>' && --d == 0) { j++; break; } j++; }
            buf_putn(o, l + i, j - i); i = j;
        } else if (c == '"' || c == '\'' || c == '/') {
            size_t j = i + 1;
            while (j < n && l[j] != c) j++;
            if (j < n) j++;
            buf_putn(o, l + i, j - i); i = j;
        } else if (blank((unsigned char)c) || c == ',') { buf_putc(o, c); i++; }
        else {
            size_t j = i + 1;
            while (j < n && l[j] != c) j++;
            if (j >= n) { buf_putn(o, l + i, n - i); return; }
            buf_putc(o, '"');
            for (size_t k = i + 1; k < j; k++) { if (l[k] == '"' || l[k] == '\\') buf_putc(o, '\\'); buf_putc(o, l[k]); }
            buf_putc(o, '"');
            i = j + 1;
        }
    }
}


/* ---- Micro-11 sources (--from=micro11) ----
   - a comment may follow the operands without ';'  ->  ';' is inserted
   - ".+n" (reserve n bytes) -> ".blkb n";  TYIN -> emt 6;  TOUT -> emt 16
   - a label defined again gets a new name (name_2, name_3, ...); later references use the newest name
   - 'AJ' (two characters closed by a quote) -> "AJ ;  'Y' -> 'Y */

static int g_micro;
static SMap g_lab_count, g_lab_cur;

static int insn_operands(const char *w, size_t n) {
    static const char *const TWO[] = {"mov", "movb", "cmp", "cmpb", "bit", "bitb", "bic", "bicb", "bis", "bisb", "add", "sub", "xor", "jsr", "sob", NULL};
    static const char *const ONE[] = {"clr", "clrb", "com", "comb", "inc", "incb", "dec", "decb", "neg", "negb", "adc", "adcb", "sbc", "sbcb",
        "tst", "tstb", "ror", "rorb", "rol", "rolb", "asr", "asrb", "asl", "aslb", "swab", "sxt", "mtps", "mfps", "jmp", "rts", "mark", "trap", "emt", "call",
        "br", "bne", "beq", "bpl", "bmi", "bcc", "bcs", "bvc", "bvs", "bhi", "blos", "bhis", "blo", "bge", "blt", "bgt", "ble", NULL};
    static const char *const ZERO[] = {"return", "nop", "bpt", "iot", "wait", "reset", "halt", "rti", "rtt", "clc", "clv", "cln", "clz", "ccc",
        "sec", "sev", "sen", "sez", "scc", "tyin", "tout", NULL};
    char lo[16];
    if (n == 0 || n >= sizeof lo) return -1;
    lower_ascii_n(lo, w, n); lo[n] = 0;
    for (int k = 0; TWO[k]; k++) if (!strcmp(lo, TWO[k])) return 2;
    for (int k = 0; ONE[k]; k++) if (!strcmp(lo, ONE[k])) return 1;
    for (int k = 0; ZERO[k]; k++) if (!strcmp(lo, ZERO[k])) return 0;
    return -1;
}

/* a character constant at s[i] == '\'' : its length, and how many characters it holds (closed forms 'Y' and 'AJ') */
static size_t micro_char(const char *s, size_t i, size_t n, int *nch, bool *closed) {
    *closed = false; *nch = 1;
    if (i + 1 >= n) { *nch = 0; return 1; }
    if (i + 2 < n && s[i + 2] == '\'') { *closed = true; return 3; }
    if (i + 3 < n && s[i + 3] == '\'' && s[i + 2] != ',' && s[i + 2] != ' ' && s[i + 2] != '\t'
        && (wc((unsigned char)s[i + 1]) && wc((unsigned char)s[i + 2]))) { *closed = true; *nch = 2; return 4; }
    return 2;
}

/* text with labels renamed and closed character constants normalised */
static void micro_refs(const char *s, size_t n, Buf *o) {
    size_t i = 0;
    while (i < n) {
        if (s[i] == ';') { buf_putn(o, s + i, n - i); return; }
        if (s[i] == '\'') {
            int nch; bool closed;
            size_t k = micro_char(s, i, n, &nch, &closed);
            if (closed && g_mode == M_PDPY11) {
                if (nch == 2) { buf_putc(o, '"'); buf_putn(o, s + i + 1, 2); }          /* 'AJ' -> "AJ */
                else { buf_putc(o, '\''); buf_putn(o, s + i + 1, 1); }                  /* 'Y'  -> 'Y  */
            } else buf_putn(o, s + i, k);
            i += k;
            continue;
        }
        if (wc((unsigned char)s[i])) {
            size_t j = i;
            while (j < n && wc((unsigned char)s[j])) j++;
            char lo[96];
            const char *cur = NULL;
            if (j - i < sizeof lo) { lower_ascii_n(lo, s + i, j - i); cur = (const char *)smap_get(&g_lab_cur, lo, j - i); }
            if (cur) buf_puts(o, cur); else buf_putn(o, s + i, j - i);
            i = j;
        } else { buf_putc(o, s[i]); i++; }
    }
}

/* end of an operand: up to a blank or comma outside brackets (character constants are skipped as a whole) */
static size_t micro_operand_end(const char *s, size_t i, size_t n) {
    int depth = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\'') { int nch; bool cl; i += micro_char(s, i, n, &nch, &cl); continue; }
        if (c == '(') depth++;
        else if (c == ')') { if (depth) depth--; }
        else if (depth == 0 && (c == ',' || blank((unsigned char)c) || c == ';')) break;
        i++;
    }
    return i;
}

static bool micro_known_word(const char *w, size_t n) {
    if (n == 0) return false;
    if (w[0] == '.') return true;
    return insn_operands(w, n) >= 0 || macro_lookup(w, n) != NULL;
}

/* whatever follows the operands: a comment when it is not a statement */
static void micro_tail(const char *s, size_t i, size_t n, Buf *o) {
    size_t t = i;
    while (t < n && blank((unsigned char)s[t])) t++;
    if (t >= n) return;
    if (s[t] == ';' || (s[t] == '/' && t + 1 < n && (s[t + 1] == '/' || s[t + 1] == '*'))) { buf_putn(o, s + i, n - i); return; }
    size_t e = t;
    while (e < n && wc((unsigned char)s[e])) e++;
    if (micro_known_word(s + t, e - t)) { buf_putn(o, s + i, n - i); return; }
    buf_puts(o, "\t; ");
    buf_putn(o, s + t, n - t);
}

static bool micro_pseudo_at(const char *s, size_t p, size_t n) {
    if (p + 1 >= n || s[p] != '.') return false;
    char a = s[p + 1], u = (a >= 'a' && a <= 'z') ? a - 32 : a;
    if (a == '#' || a == '@') return true;
    if ((u == 'B' || u == 'A' || u == 'R') && p + 2 < n && s[p + 2] == ':') return true;
    if (u == 'E') return p + 2 >= n || !wc((unsigned char)s[p + 2]);
    return false;
}

/* expression of a pseudo-command: a run of name/number characters, + and -, up to a blank or the next pseudo-command */
static size_t micro_pexpr(const char *s, size_t p, size_t n, size_t *start) {
    while (p < n && blank((unsigned char)s[p])) p++;
    *start = p;
    while (p < n) {
        if (s[p] == '.') { if (micro_pseudo_at(s, p, n)) break; p++; continue; }
        if (wc((unsigned char)s[p]) || s[p] == '+' || s[p] == '-' || (unsigned char)s[p] >= 0x80) { p++; continue; }
        break;
    }
    return p;
}

/* returns true when the line was changed in a way that deserves a "was:" note; label notes go to notes */
static bool micro_line(const char *l, size_t n, Buf *o, Buf *notes) {
    size_t i = 0;
    bool noted = false;
    for (;;) {                                                       /* labels  A: B: */
        size_t s0 = i;
        while (i < n && blank((unsigned char)l[i])) i++;
        size_t a = i;
        if (i < n && !(isalnum((unsigned char)l[i]) || l[i] == '$')) { buf_putn(o, l + s0, a - s0); break; }
        while (i < n && wc((unsigned char)l[i])) i++;
        if (i > a && i < n && l[i] == ':') {
            char lo[96];
            size_t len = i - a;
            buf_putn(o, l + s0, a - s0);
            if (len < sizeof lo) {
                lower_ascii_n(lo, l + a, len);
                uintptr_t cnt = (uintptr_t)smap_get(&g_lab_count, lo, len);
                *smap_slot(&g_lab_count, lo, len, NULL) = (void *)(cnt + 1);
                if (cnt == 0) { buf_putn(o, l + a, len); }
                else {
                    char *nn = (char *)xmalloc(len + 16);
                    snprintf(nn, len + 16, "%.*s_%u", (int)len, l + a, (unsigned)(cnt + 1));
                    *smap_slot(&g_lab_cur, lo, len, NULL) = nn;
                    buf_puts(o, nn);
                    buf_printf(notes, "%s label %.*s is defined again: renamed to %s, later references use the new name\n",
                               g_mode == M_TURBO ? "//" : ";", (int)len, l + a, nn);
                }
            } else buf_putn(o, l + a, len);
            buf_putc(o, ':'); i++;
            continue;
        }
        i = a; buf_putn(o, l + s0, a - s0);
        break;
    }
    if (i >= n || l[i] == ';' || l[i] == '/') { buf_putn(o, l + i, n - i); return noted; }

    if (l[i] == '.' && i + 1 < n && l[i + 1] == '+') {               /* .+n  ->  .blkb n */
        size_t a; size_t e = micro_pexpr(l, i + 2, n, &a);
        buf_puts(o, ".blkb ");
        micro_refs(l + a, e - a, o);
        micro_tail(l, e, n, o);
        return true;
    }
    if (micro_pseudo_at(l, i, n)) {                                   /* chain of pseudo-commands (kept as they are) */
        size_t p = i;
        while (p < n && micro_pseudo_at(l, p, n)) {
            char k = l[p + 1]; if (k >= 'a' && k <= 'z') k -= 32;
            size_t a, e;
            if (k == '#' || k == '@') { e = micro_pexpr(l, p + 2, n, &a); buf_putn(o, l + p, 2); micro_refs(l + p + 2, e - (p + 2), o); }
            else if (k == 'B') { e = micro_pexpr(l, p + 3, n, &a); buf_putn(o, l + p, 3); micro_refs(l + p + 3, e - (p + 3), o); }
            else if (k == 'A') { e = p + 3; while (e < n && l[e] != '!') e++; if (e < n) e++; buf_putn(o, l + p, e - p); }
            else if (k == 'R') { e = p + 3; while (e < n && l[e] != ';') e++; buf_putn(o, l + p, e - p); }
            else { e = p + 2; buf_putn(o, l + p, 2); }
            size_t q = e;
            while (q < n && blank((unsigned char)l[q])) q++;
            if (q < n && micro_pseudo_at(l, q, n)) { buf_putn(o, l + e, q - e); p = q; } else p = e;
        }
        micro_tail(l, p, n, o);
        return noted;
    }
    size_t ws = i, we = i;
    while (we < n && wc((unsigned char)l[we])) we++;
    int ops = insn_operands(l + ws, we - ws);
    if (ops < 0) { micro_refs(l + ws, n - ws, o); return noted; }      /* a macro, a directive or an assignment: only the names change */
    char lo[8]; size_t wl = we - ws; lower_ascii_n(lo, l + ws, wl > 7 ? 7 : wl); lo[wl > 7 ? 7 : wl] = 0;
    if (!strcmp(lo, "tyin") || !strcmp(lo, "tout")) {
        buf_puts(o, !strcmp(lo, "tyin") ? "emt\t6" : "emt\t16");
        micro_tail(l, we, n, o);
        return true;
    }
    buf_putn(o, l + ws, wl);
    size_t q = we;
    for (int k = 0; k < ops; k++) {
        size_t b = q;
        while (q < n && blank((unsigned char)l[q])) q++;
        size_t a = q;
        q = micro_operand_end(l, q, n);
        if (q == a) { q = b; break; }                                  /* operand missing: leave the rest to the assembler */
        buf_putn(o, l + b, a - b);
        micro_refs(l + a, q - a, o);
        if (k + 1 < ops) {
            size_t c = q;
            while (c < n && blank((unsigned char)l[c])) c++;
            if (c < n && l[c] == ',') {
                q = c + 1;
                buf_putc(o, ',');
            } else break;
        }
    }
    micro_tail(l, q, n, o);
    return noted;
}


static void emit_comment(Buf *out, const char *s, size_t n) {
    buf_puts(out, g_mode == M_TURBO ? "// " : "; ");
    buf_putn(out, s, n);
    buf_putc(out, '\n');
}

/* does the converted text contain anything but blanks and comments? */
static bool has_code(const char *s, size_t n) {
    bool in_b = false;
    for (size_t i = 0; i < n; ) {
        size_t e = i;
        while (e < n && s[e] != '\n') e++;
        size_t p = i;
        if (!in_b) { Words w = find_word(s + i, e - i); if (word_is(s + i, w, ".once")) { i = e + 1; continue; } }
        while (p < e) {
            if (in_b) { if (s[p] == '*' && p + 1 < e && s[p + 1] == '/') { in_b = false; p += 2; } else p++; continue; }
            if (blank(s[p])) { p++; continue; }
            if (s[p] == ';' || (s[p] == '/' && p + 1 < e && s[p + 1] == '/')) break;
            if (s[p] == '/' && p + 1 < e && s[p + 1] == '*') { in_b = true; p += 2; continue; }
            return true;
        }
        i = e + 1;
    }
    return false;
}

static void process(const char *text, size_t len, const char *path, int depth, Buf *out, bool top) {
    char *dir = dir_of(path);
    size_t p = 0;
    int lineno = 1;
    bool in_block = false;
    CFrame fr[128];
    int nfr = 0, skip_mac = 0;
    bool active = true;
    while (p < len) {
        const char *nl = memchr(text + p, '\n', len - p);
        size_t le = nl ? (size_t)(nl - text) : len;
        const char *l = text + p;
        size_t n = le - p;
        if (g_micro && !in_block && active) {
            Buf mo = {0}, notes = {0};
            bool noted = micro_line(l, n, &mo, &notes);
            if (notes.len) buf_putn(out, notes.p, notes.len);
            if (noted) {
                size_t a = 0, b = n;
                while (a < b && blank(l[a])) a++;
                while (b > a && blank(l[b - 1])) b--;
                buf_puts(out, g_mode == M_TURBO ? "// was: " : "; was: ");
                buf_putn(out, l + a, b - a);
                buf_putc(out, '\n');
            }
            l = mo.p ? mo.p : ""; n = mo.len;
        }
        Words w = {0, 0, 0};
        if (!in_block) w = find_word(l, n);
        size_t wl = w.we - w.ws;


        if (!active && !(wl && !in_block)) goto next;
        if (!in_block && wl) {
            bool is_mac = word_is(l, w, ".macro") || word_is(l, w, "macro");
            bool is_endm = word_is(l, w, ".endm") || word_is(l, w, "endm");
            bool is_if = word_is(l, w, ".if");
            bool is_alt = word_is(l, w, ".iff") || word_is(l, w, ".ift") || word_is(l, w, ".iftf");
            bool is_endc = word_is(l, w, ".endc");
            if (!active) {                                  /* inside a false branch: only the nesting is followed */
                if (skip_mac) { if (is_mac) skip_mac++; else if (is_endm) skip_mac--; goto next; }
                if (is_mac) { skip_mac = 1; goto next; }
                if (!is_if && !is_alt && !is_endc) goto next;
            }
            if (is_if) {
                if (nfr >= 128) { fprintf(stderr, "%s:%d: error: conditional blocks are nested too deeply\n", path, lineno); exit(1); }
                bool r = false;
                if (active) {
                    const char *err;
                    size_t e = cond_test(text, len, p + w.we, &r, &err);
                    if (e == SIZE_MAX) { fprintf(stderr, "%s:%d: error: %s\n", path, lineno, err); exit(1); }
                }
                fr[nfr++] = (CFrame){r, r, active};
                active = active && r;
                goto next;
            }
            if (is_alt || is_endc) {
                if (nfr == 0) { fprintf(stderr, "%s:%d: error: this directive has no matching '.if'\n", path, lineno); exit(1); }
                if (is_endc) { nfr--; active = nfr == 0 ? true : (fr[nfr - 1].parent && fr[nfr - 1].branch); }
                else {
                    CFrame *f = &fr[nfr - 1];
                    f->branch = word_is(l, w, ".iff") ? !f->cond : word_is(l, w, ".ift") ? f->cond : true;
                    active = f->parent && f->branch;
                }
                goto next;
            }
            if (word_is(l, w, ".iif")) {
                bool r = false;
                const char *err;
                size_t e = cond_test(text, len, p + w.we, &r, &err);
                if (e == SIZE_MAX) { fprintf(stderr, "%s:%d: error: %s\n", path, lineno, err); exit(1); }
                while (e < le && blank(text[e])) e++;
                if (e < le && text[e] == ',') e++;
                if (r) {
                    if (w.lab_end) { buf_putn(out, l, w.lab_end); buf_putc(out, '\n'); }
                    Buf tail = {0};
                    buf_putn(&tail, text + e, le - e);
                    buf_putc(&tail, '\n');
                    process(tail.p, tail.len, path, depth + 1, out, false);
                    buf_free(&tail);
                }
                goto next;
            }
            if (word_is(l, w, ".mcall")) goto next;
            if (g_mode == M_PDPY11 && !w.lab_end && (word_is(l, w, ".list") || word_is(l, w, ".nlist") || word_is(l, w, ".asect"))) {
                size_t a = 0, b = n;                       /* listing control and the absolute section are of no use to PDPy11 */
                while (a < b && blank(l[a])) a++;
                while (b > a && blank(l[b - 1])) b--;
                buf_puts(out, "; removed (not needed in PDPy11): ");
                buf_putn(out, l + a, b - a);
                buf_putc(out, '\n');
                goto next;
            }
            if (word_is(l, w, ".ntype")) {
                size_t a = p + w.we;
                while (a < le && blank(text[a])) a++;
                size_t ns = a;
                while (a < le && wc((unsigned char)text[a])) a++;
                size_t ne = a;
                while (a < le && blank(text[a])) a++;
                if (ne == ns || a >= le || text[a] != ',') { fprintf(stderr, "%s:%d: error: '.ntype' expects 'symbol, operand'\n", path, lineno); exit(1); }
                a++;
                size_t oe = a;
                while (oe < le && text[oe] != ';') oe++;
                long v = cond_ntype(text + a, oe - a);
                cond_set(text + ns, ne - ns, v);
                redef_set(text + ns, ne - ns, v);
                buf_printf(out, "%s .ntype %.*s = %lo (octal), later uses are replaced by %ld.\n", g_mode == M_TURBO ? "//" : ";", (int)(ne - ns), text + ns, v, v);
                goto next;
            }
            if (word_is(l, w, ".error")) {
                size_t a = w.we;
                while (a < n && blank(l[a])) a++;
                if (a >= n || l[a] == ';') {
                    size_t b = n;
                    size_t from = a < n ? a + 1 : a;
                    while (b > from && blank(l[b - 1])) b--;
                    if (b > from && l[b - 1] == ';') b--;
                    fprintf(stderr, "%s:%d: warning: .error: %.*s\n", path, lineno, (int)(b - from), l + from);
                    buf_printf(out, "%s .error: %.*s\n", g_mode == M_TURBO ? "//" : ";", (int)(b - from), l + from);
                    goto next;
                }
            }
            if (word_is(l, w, ".irp") || word_is(l, w, ".irpc") || word_is(l, w, ".rept")) {
                int kind = word_is(l, w, ".rept") ? 2 : word_is(l, w, ".irpc") ? 1 : 0;
                size_t bs, be, after;
                if (!repeat_body(text, len, p, &bs, &be, &after)) { fprintf(stderr, "%s:%d: error: this repeat block has no '.endr'\n", path, lineno); exit(1); }
                Buf rep = {0};
                if (kind == 2) {
                    long cnt; const char *err;
                    size_t ce = p + w.we;
                    while (ce < le && text[ce] != ';') ce++;
                    if (!cond_eval(text + p + w.we, ce - (p + w.we), &cnt, &err) || cnt < 0 || cnt > 65536) {
                        fprintf(stderr, "%s:%d: error: bad repeat count\n", path, lineno); exit(1);
                    }
                    for (long i = 0; i < cnt; i++) buf_putn(&rep, text + bs, be - bs);
                } else {
                    size_t a = p + w.we;
                    while (a < le && blank(text[a])) a++;
                    size_t ns = a;
                    while (a < le && wc((unsigned char)text[a])) a++;
                    size_t ne = a;
                    while (a < le && blank(text[a])) a++;
                    if (ne == ns || a >= le || text[a] != ',') { fprintf(stderr, "%s:%d: error: expected 'symbol, <list>'\n", path, lineno); exit(1); }
                    a++;
                    while (a < le && blank(text[a])) a++;
                    PVec args = {0};
                    if (a < le && text[a] == '<') {
                        size_t q = a; int d = 0;
                        for (; q < le; q++) { if (text[q] == '<') d++; else if (text[q] == '>' && --d == 0) break; }
                        if (q >= le) { fprintf(stderr, "%s:%d: error: missing '>'\n", path, lineno); exit(1); }
                        if (kind == 0) macro_collect_args(text + a + 1, q - a - 1, 0, &args);
                        else for (size_t k = a + 1; k < q; k++) pvec_push(&args, astrndup(text + k, 1));
                    } else if (kind == 0) macro_collect_args(text, le, a, &args);
                    else { size_t q = a; while (q < le && !blank(text[q]) && text[q] != ';') q++; for (size_t k = a; k < q; k++) pvec_push(&args, astrndup(text + k, 1)); }
                    Buf def = {0};
                    buf_puts(&def, "__irp ");
                    buf_putn(&def, text + ns, ne - ns);
                    buf_putc(&def, '\n');
                    buf_putn(&def, text + bs, be - bs);
                    buf_puts(&def, ".endm\n");
                    const char *err; size_t ep;
                    if (macro_define(def.p, def.len, 0, &err, &ep) == SIZE_MAX) { fprintf(stderr, "%s:%d: error: %s\n", path, lineno, err); exit(1); }
                    buf_free(&def);
                    const Macro *im = macro_lookup("__irp", 5);
                    for (size_t i = 0; i < args.n; i++) {
                        PVec one = {0};
                        pvec_push(&one, args.a[i]);
                        size_t xl; char *x = macro_expand(im, &one, &xl);
                        free(one.a);
                        buf_putn(&rep, x, xl);
                    }
                    free(args.a);
                }
                if (w.lab_end) { buf_putn(out, l, w.lab_end); buf_putc(out, '\n'); }
                size_t cl = n - w.ws;
                while (cl && blank((unsigned char)l[w.ws + cl - 1])) cl--;
                emit_comment(out, l + w.ws, cl);
                process(rep.p, rep.len, path, depth + 1, out, false);
                buf_free(&rep);
                for (size_t i = p; i < after && i < len; i++) if (text[i] == '\n') lineno++;
                lineno++;
                p = after < len ? after + 1 : len;
                continue;
            }
        }

        if (wl && (word_is(l, w, ".macro") || word_is(l, w, "macro"))) {
            const char *err; size_t ep;
            size_t r = macro_define(text, len, p + w.we, &err, &ep);
            if (r == SIZE_MAX) {
                int ln = lineno;
                for (size_t i = p; i < ep && i < len; i++) if (text[i] == '\n') ln++;
                fprintf(stderr, "%s:%d: error: %s\n", path, ln, err);
                g_errors++;
                exit(1);
            }
            for (size_t i = p; i < r && i < len; i++) if (text[i] == '\n') lineno++;
            if (r < len) lineno++;                     /* the newline that ends the .endm line */
            p = r < len ? r + 1 : len;
            continue;
        }
        if (wl && (word_is(l, w, ".endm") || word_is(l, w, "endm"))) {
            fprintf(stderr, "%s:%d: error: '.endm' without '.macro'\n", path, lineno);
            exit(1);
        }

        if (!in_block && g_mode == M_PDPY11) {
            int sreg; size_t sns, sne;
            if (parse_syn_decl(l, n, &sreg, &sns, &sne)) {
                syn_add(l + sns, sne - sns, sreg);
                buf_printf(out, "; register synonym %.*s = r%d: the name is replaced by r%d below\n", (int)(sne - sns), l + sns, sreg, sreg);
                goto next;
            }
        }

        const Macro *m = wl ? macro_lookup(l + w.ws, wl) : NULL;
        if (m && (g_mode == M_PDPY11 || !macro_is_builtin(m))) {
            if (depth >= MAX_DEPTH) {
                fprintf(stderr, "%s:%d: error: macro expansion is nested too deeply (recursion?)\n", path, lineno);
                exit(1);
            }
            PVec args = {0};
            size_t endp = macro_collect_args(text, len, p + w.we, &args);
            if (args.n > macro_ndummies(m)) {
                fprintf(stderr, "%s:%d: error: macro '%s' takes %zu argument(s), %zu given\n", path, lineno,
                        macro_name(m), macro_ndummies(m), args.n);
                exit(1);
            }
            if (w.lab_end) { buf_putn(out, l, w.lab_end); buf_putc(out, '\n'); }
            size_t cl = endp - (p + w.ws);
            while (cl && blank((unsigned char)l[w.ws + cl - 1])) cl--;
            buf_puts(out, g_mode == M_TURBO ? "// " : "; ");
            buf_putn(out, l + w.ws, cl);
            /* a trailing comment of the call line stays on the comment line */
            size_t q = endp;
            while (q < le && blank(text[q])) q++;
            if (q < le) { bool ib = false; Buf t = {0}; convert_line(text + q, le - q, &ib, &t); buf_putc(out, '\t'); buf_putn(out, t.p, t.len); buf_free(&t); }
            buf_putc(out, '\n');
            size_t xl;
            char *x = macro_expand(m, &args, &xl);
            process(x, xl, path, depth + 1, out, false);
            free(args.a);
            goto next;
        }

        if (wl && (word_is(l, w, ".include") || word_is(l, w, "include"))) {
            size_t a = w.we;
            while (a < n && blank(l[a])) a++;
            size_t b;
            if (a < n && l[a] == '"') { a++; b = a; while (b < n && l[b] != '"') b++; }
            else { b = a; while (b < n && !blank(l[b]) && l[b] != ';') b++; }
            char *name = astrndup(l + a, b - a);
            char *full = name;
            if (name[0] != '/') {
                Buf pb = {0};
                buf_puts(&pb, dir); buf_putc(&pb, '/'); buf_puts(&pb, name); buf_putc(&pb, 0);
                full = astrndup(pb.p, pb.len - 1);
                buf_free(&pb);
            }
            size_t ilen;
            char *itext = depth < MAX_DEPTH ? read_all(full, &ilen) : NULL;
            if (itext) {
                Buf ib = {0};
                process(itext, ilen, full, depth + 1, &ib, false);
                bool code = has_code(ib.p, ib.len);
                buf_free(&ib);
                free(itext);
                if (!code) {                           /* a pure macro library: nothing is left to include */
                    if (w.lab_end) { buf_putn(out, l, w.lab_end); buf_putc(out, '\n'); }
                    emit_comment(out, l + w.ws, n - w.ws);
                    goto next;
                }
            }
        }

        if (!in_block && wl && !w.lab_end && !(wl == 1 && l[w.ws] == '.')) {                  /* "name = constant": remember it for .if conditions */
            size_t a = w.we;
            while (a < n && blank(l[a])) a++;
            if (a < n && l[a] == '=' && (a + 1 >= n || l[a + 1] != '=')) {
                size_t ve = a + 1;
                while (ve < n && l[ve] != ';' && !(l[ve] == '/' && ve + 1 < n && (l[ve + 1] == '/' || l[ve + 1] == '*'))) ve++;
                long v; const char *err;
                if (cond_eval(l + a + 1, ve - (a + 1), &v, &err)) {
                    cond_set(l + w.ws, wl, v);
                    char lo[96];
                    bool seen = false;
                    if (wl < sizeof lo) {
                        lower_ascii_n(lo, l + w.ws, wl);
                        seen = smap_get(&g_assigned_names, lo, wl) != NULL;
                        if (!seen) *smap_slot(&g_assigned_names, lo, wl, NULL) = (void *)1;
                    }
                    if (seen && depth > 0) {                  /* a new value inside a macro: later uses get the number */
                        redef_set(l + w.ws, wl, v);
                        size_t pe = ve;
                        while (pe > w.ws && blank((unsigned char)l[pe - 1])) pe--;
                        buf_printf(out, "%s %.*s redefined: later uses are replaced by %ld.\n", g_mode == M_TURBO ? "//" : ";", (int)(pe - w.ws), l + w.ws, v);
                        goto next;
                    }
                } else cond_forget(l + w.ws, wl);
            }
        }

        {   /* ordinary line: rename directives, convert comments */
            Buf line = {0};
            const char *rep = NULL;
            if (wl) {
                if (g_mode == M_PDPY11) {
                    if (word_is(l, w, ".la")) rep = ".link";
                    else if (word_is(l, w, ".insert")) rep = "insert_file";
                } else {
                    if (word_is(l, w, ".link")) rep = ".la";
                    else if (word_is(l, w, "insert_file") || word_is(l, w, ".insert_file")) rep = ".insert";
                    else if (top)
                        for (int k = 0; NO_TURBO[k]; k++)
                            if (word_is(l, w, NO_TURBO[k]))
                                fprintf(stderr, "%s:%d: warning: Turbo8 has no '%s'\n", path, lineno, NO_TURBO[k]);
                }
            }
            if (wl && top && word_is(l, w, ".end")) g_saw_end = true;
            if (rep) { buf_putn(&line, l, w.ws); buf_puts(&line, rep); buf_putn(&line, l + w.we, n - w.we); }
            else buf_putn(&line, l, n);
            bool ib0 = in_block;
            Buf fixed = {0};
            bool asc = g_mode == M_PDPY11 && !ib0 && wl && (word_is(l, w, ".ascii") || word_is(l, w, ".asciz"));
            if (asc) fix_ascii(line.p, line.len, w.we, &fixed);
            else rewrite_code(line.p, line.len, ib0, (!ib0 && wl) ? w.we + (rep ? strlen(rep) - wl : 0) : 0, &fixed);
            /* the quoting of character constants is too obvious to be noted; anything else is */
            Buf plain = {0};
            g_chars = false;
            if (asc) buf_putn(&plain, fixed.p ? fixed.p : "", fixed.len);
            else rewrite_code(line.p, line.len, ib0, (!ib0 && wl) ? w.we + (rep ? strlen(rep) - wl : 0) : 0, &plain);
            g_chars = true;
            bool noteworthy = plain.len != n || memcmp(plain.p, l, n) != 0;
            buf_free(&plain);
            if (noteworthy) {                                           /* the line was changed: show the original */
                size_t a = 0, b = n;
                while (a < b && blank(l[a])) a++;
                while (b > a && blank(l[b - 1])) b--;
                buf_puts(out, g_mode == M_TURBO ? "// was: " : "; was: ");
                buf_putn(out, l + a, b - a);
                buf_putc(out, '\n');
            }
            convert_line(fixed.p, fixed.len, &in_block, out);
            buf_free(&fixed);
            buf_putc(out, '\n');
            buf_free(&line);
        }
    next:
        lineno++;
        p = nl ? le + 1 : len;
    }
    if (nfr > 0) { fprintf(stderr, "%s: error: an '.if' is never closed with '.endc'\n", path); exit(1); }
}

int main(int argc, char **argv) {
    const char *from = NULL;
    int ac = 0;
    char *av[3];
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--from=", 7) == 0) from = argv[i] + 7;
        else if (ac < 3) av[ac++] = argv[i];
        else ac = 99;
    }
    if (ac != 2) {
        fprintf(stderr, "usage: convert-to [--from=pdpy11|turbo|micro11] pdpy11|turbo file\n"
                        "  writes file.pdpy11 or file.turbo next to the source\n"
                        "  --from tells which assembler the source was written for (it decides how operator\n"
                        "  precedence is converted); by default it is the other one than the target\n");
        return 2;
    }
    argv[1] = av[0]; argv[2] = av[1];
    char key[16];
    size_t kl = strlen(argv[1]);
    if (kl >= sizeof key) kl = sizeof key - 1;
    for (size_t i = 0; i < kl; i++) { char c = argv[1][i]; key[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c; }
    key[kl] = 0;
    const char *ext;
    if (strcmp(key, "pdpy11") == 0) { g_mode = M_PDPY11; ext = ".pdpy11"; }
    else if (strcmp(key, "turbo") == 0) { g_mode = M_TURBO; ext = ".turbo"; }
    else { fprintf(stderr, "convert-to: unknown key '%s' (expected pdpy11 or turbo)\n", argv[1]); return 2; }

    g_micro = from && strcasecmp(from, "micro11") == 0;
    g_src_turbo = g_micro ? 0 : from ? (strcasecmp(from, "turbo") == 0) : (g_mode == M_PDPY11);
    size_t len;
    char *text = read_all(argv[2], &len);
    if (!text) { fprintf(stderr, "convert-to: cannot read '%s'\n", argv[2]); return 1; }
    while (len > 0 && text[len - 1] == 0) len--;       /* zero padding at the end of an RT-11 file */
    macro_init_builtin();
    Buf out = {0};
    process(text, len, argv[2], 0, &out, true);
    if (g_mode == M_TURBO && !g_saw_end) {             /* Turbo8 insists on .END */
        if (out.len && out.p[out.len - 1] != '\n') buf_putc(&out, '\n');
        buf_puts(&out, "\t.end\n");
    } else if (len > 0 && text[len - 1] != '\n' && out.len > 0 && out.p[out.len - 1] == '\n') out.len--;   /* keep "no final newline" */

    Buf name = {0};
    buf_puts(&name, argv[2]); buf_puts(&name, ext); buf_putc(&name, 0);
    FILE *f = fopen(name.p, "wb");
    if (!f) { fprintf(stderr, "convert-to: cannot write '%s'\n", name.p); return 1; }
    fwrite(out.p, 1, out.len, f);
    fclose(f);
    fprintf(stderr, "%s written\n", name.p);
    return g_errors ? 1 : 0;
}
