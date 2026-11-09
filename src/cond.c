/* Conditional assembly helpers: constant table, expression evaluator, .if condition tests, .ntype. */
#include "cond.h"
#include "builtins.h"

static SMap g_const;                          /* lower-case name -> boxed long */

static bool is_blank_(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }
static bool is_wc_(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '.'; }
static bool is_digit_c(int c) { return c >= '0' && c <= '9'; }
static int lower_(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

bool cond_get(const char *name, size_t n, long *v) {
    char buf[128];
    if (!g_const.cap || n == 0 || n >= sizeof buf) return false;
    lower_ascii_n(buf, name, n);
    long *p = (long *)smap_get(&g_const, buf, n);
    if (!p) return false;
    if (v) *v = *p;
    return true;
}

void cond_set(const char *name, size_t n, long v) {
    char buf[128];
    if (n == 0 || n >= sizeof buf) return;
    lower_ascii_n(buf, name, n);
    void **slot = smap_slot(&g_const, buf, n, NULL);
    if (!*slot) *slot = amalloc(sizeof(long));
    *(long *)*slot = v;
}

void cond_forget(const char *name, size_t n) {
    char buf[128];
    if (!g_const.cap || n == 0 || n >= sizeof buf) return;
    lower_ascii_n(buf, name, n);
    void **slot = smap_slot(&g_const, buf, n, NULL);
    if (*slot) { *(long *)*slot = 0; *slot = NULL; }          /* the slot stays, the value is gone */
}

/* ---- expressions ---- */

static int g_radix = 8;
int cond_radix(void) { return g_radix; }
void cond_set_radix(int r) { g_radix = r; }

typedef struct { const char *s; size_t n, i; const char *err; } Ev;

static void ws(Ev *e) { while (e->i < e->n && is_blank_((unsigned char)e->s[e->i])) e->i++; }
static bool fail(Ev *e, const char *m) { if (!e->err) e->err = m; return false; }

static bool ev_expr(Ev *e, long *v);

static bool ev_digits(Ev *e, int base, long *v) {
    size_t st = e->i;
    long r = 0;
    while (e->i < e->n) {
        int c = lower_((unsigned char)e->s[e->i]), d;
        if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else break;
        if (d >= base) break;
        r = r * base + d; e->i++;
    }
    *v = r;
    return e->i > st;
}

static bool ev_atom(Ev *e, long *v) {
    ws(e);
    if (e->i >= e->n) return fail(e, "an expression is expected");
    unsigned char c = (unsigned char)e->s[e->i];
    if (c == '-' || c == '+' || c == '~') {
        e->i++;
        long x;
        if (!ev_atom(e, &x)) return false;
        *v = c == '-' ? -x : c == '~' ? ~x : x;
        return true;
    }
    if (c == '<') {
        e->i++;
        if (!ev_expr(e, v)) return false;
        ws(e);
        if (e->i >= e->n || e->s[e->i] != '>') return fail(e, "'>' is expected");
        e->i++;
        return true;
    }
    if (c == '\'') { if (e->i + 1 >= e->n) return fail(e, "a character is expected after '"); *v = (unsigned char)e->s[e->i + 1]; e->i += 2; return true; }
    if (c == '"') {
        if (e->i + 2 >= e->n) return fail(e, "two characters are expected after \"");
        *v = (unsigned char)e->s[e->i + 1] | ((unsigned char)e->s[e->i + 2] << 8); e->i += 3; return true;
    }
    if (c == '^') {                                           /* radix: ^O ^D ^B ^H ^X, then digits or <expr> */
        int r = e->i + 1 < e->n ? lower_((unsigned char)e->s[e->i + 1]) : 0;
        int base = r == 'o' ? 8 : r == 'd' ? 10 : r == 'b' ? 2 : (r == 'h' || r == 'x') ? 16 : 0;
        if (!base) return fail(e, "unsupported ^ operator");
        e->i += 2;
        if (e->i < e->n && e->s[e->i] == '<') {
            size_t a = ++e->i;
            int depth = 1;
            while (e->i < e->n && depth) { if (e->s[e->i] == '<') depth++; else if (e->s[e->i] == '>') depth--; e->i++; }
            if (depth) return fail(e, "'>' is expected");
            Ev sub = {e->s + a, e->i - 1 - a, 0, NULL};
            long x = 0;
            if (!ev_digits(&sub, base, &x) || (ws(&sub), sub.i != sub.n)) return fail(e, "a number is expected after ^");
            *v = x; return true;
        }
        if (!ev_digits(e, base, v)) return fail(e, "a number is expected after ^");
        return true;
    }
    if (is_digit_c(c)) {                                      /* the current radix (8 by default), decimal with a trailing point */
        size_t a = e->i;
        int rdx = g_radix;
        while (e->i < e->n && (is_digit_c((unsigned char)e->s[e->i]) ||
               (rdx == 16 && lower_((unsigned char)e->s[e->i]) >= 'a' && lower_((unsigned char)e->s[e->i]) <= 'f'))) e->i++;
        size_t b = e->i;
        bool dec = e->i < e->n && e->s[e->i] == '.';
        if (dec) e->i++;
        int base = dec ? 10 : rdx;
        long r = 0;
        for (size_t k = a; k < b; k++) {
            int ch = lower_((unsigned char)e->s[k]);
            int d = ch <= '9' ? ch - '0' : ch - 'a' + 10;
            if (d >= base) return fail(e, "this digit is not valid in the current radix");
            r = r * base + d;
        }
        *v = r;
        return true;
    }
    if (is_wc_(c) && !(c == '.' && (e->i + 1 >= e->n || !is_wc_((unsigned char)e->s[e->i + 1])))) {
        size_t a = e->i;
        while (e->i < e->n && is_wc_((unsigned char)e->s[e->i])) e->i++;
        if (!cond_get(e->s + a, e->i - a, v)) {
            if (!e->err) e->err = "this symbol has no value known at macro-expansion time";
            return false;
        }
        return true;
    }
    return fail(e, "an expression is expected");
}

static bool ev_expr(Ev *e, long *v) {
    if (!ev_atom(e, v)) return false;
    for (;;) {
        ws(e);
        if (e->i >= e->n) return true;
        char op = e->s[e->i];
        if (op != '+' && op != '-' && op != '*' && op != '/' && op != '&' && op != '!') return true;
        e->i++;
        long r;
        if (!ev_atom(e, &r)) return false;
        switch (op) {
        case '+': *v += r; break;
        case '-': *v -= r; break;
        case '*': *v *= r; break;
        case '/': if (r == 0) return fail(e, "division by zero"); *v /= r; break;
        case '&': *v &= r; break;
        default: *v |= r; break;
        }
    }
}

bool cond_eval(const char *s, size_t n, long *v, const char **err) {
    Ev e = {s, n, 0, NULL};
    if (!ev_expr(&e, v)) { *err = e.err ? e.err : "bad expression"; return false; }
    ws(&e);
    if (e.i != e.n) { *err = "unexpected text after the expression"; return false; }
    return true;
}

/* ---- .if conditions ---- */

/* one argument: <...> (brackets removed) or a bare word up to a blank/comma/comment; returns false at end of line */
static bool take_arg(const char *t, size_t len, size_t *p, size_t *as, size_t *ae) {
    size_t i = *p;
    while (i < len && is_blank_((unsigned char)t[i])) i++;
    if (i >= len || t[i] == '\n' || t[i] == ';') { *p = i; *as = *ae = i; return false; }
    if (t[i] == '<') {
        int depth = 0;
        size_t q = i;
        for (; q < len && t[q] != '\n'; q++) {
            if (t[q] == '<') depth++;
            else if (t[q] == '>' && --depth == 0) break;
        }
        if (q < len && t[q] == '>') { *as = i + 1; *ae = q; *p = q + 1; return true; }
    }
    int par = 0;
    size_t q = i;
    while (q < len && t[q] != '\n') {
        char ch = t[q];
        if (ch == '(') par++;
        else if (ch == ')' && par > 0) par--;
        else if (par == 0 && (is_blank_((unsigned char)ch) || ch == ',' || ch == ';')) break;
        q++;
    }
    *as = i; *ae = q; *p = q;
    return true;
}

static void trim(const char *t, size_t *a, size_t *b) {
    while (*a < *b && is_blank_((unsigned char)t[*a])) (*a)++;
    while (*b > *a && is_blank_((unsigned char)t[*b - 1])) (*b)--;
}

static void skip_sep(const char *t, size_t len, size_t *p) {            /* blanks and at most one comma */
    size_t i = *p;
    while (i < len && is_blank_((unsigned char)t[i])) i++;
    if (i < len && t[i] == ',') { i++; *p = i; }
}

static bool same_text(const char *t, size_t a1, size_t b1, size_t a2, size_t b2) {
    trim(t, &a1, &b1); trim(t, &a2, &b2);
    if (b1 - a1 != b2 - a2) return false;
    for (size_t k = 0; k < b1 - a1; k++) if (lower_((unsigned char)t[a1 + k]) != lower_((unsigned char)t[a2 + k])) return false;
    return true;
}

size_t cond_test(const char *t, size_t len, size_t pos, bool *result, const char **err) {
    size_t p = pos;
    while (p < len && is_blank_((unsigned char)t[p])) p++;
    size_t ks = p;
    while (p < len && ((t[p] >= 'a' && t[p] <= 'z') || (t[p] >= 'A' && t[p] <= 'Z'))) p++;
    char kw[8];
    if (p - ks == 0 || p - ks >= sizeof kw) { *err = "A condition (EQ, NE, B, NB, IDN, DIF, DF, NDF, ...) is expected here"; return SIZE_MAX; }
    for (size_t i = 0; i < p - ks; i++) kw[i] = (char)lower_((unsigned char)t[ks + i]);
    kw[p - ks] = 0;

    if (!strcmp(kw, "b") || !strcmp(kw, "nb")) {
        size_t as, ae;
        take_arg(t, len, &p, &as, &ae);
        trim(t, &as, &ae);
        *result = (ae == as) == (kw[0] == 'b');
        return p;
    }
    if (!strcmp(kw, "idn") || !strcmp(kw, "dif")) {
        size_t a1, b1, a2, b2;
        take_arg(t, len, &p, &a1, &b1);
        skip_sep(t, len, &p);
        take_arg(t, len, &p, &a2, &b2);
        *result = same_text(t, a1, b1, a2, b2) == (kw[0] == 'i');
        return p;
    }
    if (!strcmp(kw, "df") || !strcmp(kw, "ndf")) {
        size_t as, ae;
        take_arg(t, len, &p, &as, &ae);
        bool val = false, first = true;
        char op = 0;
        size_t i = as;
        while (i <= ae) {
            size_t j = i;
            while (j < ae && is_wc_((unsigned char)t[j])) j++;
            bool d = cond_get(t + i, j - i, NULL);
            if (first) val = d; else if (op == '&') val = val && d; else val = val || d;
            first = false;
            if (j >= ae) break;
            op = t[j];
            if (op != '&' && op != '!') { *err = "Only the symbol names joined by & and ! are allowed after DF/NDF"; return SIZE_MAX; }
            i = j + 1;
        }
        *result = (kw[0] == 'd') ? val : !val;
        return p;
    }
    int rel = -1;                                 /* 0 eq 1 ne 2 gt 3 lt 4 ge 5 le */
    if (!strcmp(kw, "eq") || !strcmp(kw, "z")) rel = 0;
    else if (!strcmp(kw, "ne") || !strcmp(kw, "nz")) rel = 1;
    else if (!strcmp(kw, "gt") || !strcmp(kw, "g")) rel = 2;
    else if (!strcmp(kw, "lt") || !strcmp(kw, "l")) rel = 3;
    else if (!strcmp(kw, "ge")) rel = 4;
    else if (!strcmp(kw, "le")) rel = 5;
    if (rel < 0) { *err = "Unknown condition (expected EQ, NE, GT, LT, GE, LE, B, NB, IDN, DIF, DF or NDF)"; return SIZE_MAX; }
    size_t as, ae;
    if (!take_arg(t, len, &p, &as, &ae)) { *err = "An expression is expected after the condition"; return SIZE_MAX; }
    long v;
    if (!cond_eval(t + as, ae - as, &v, err)) return SIZE_MAX;
    *result = rel == 0 ? v == 0 : rel == 1 ? v != 0 : rel == 2 ? v > 0 : rel == 3 ? v < 0 : rel == 4 ? v >= 0 : v <= 0;
    return p;
}

/* ---- .ntype ---- */

static int reg_of(const char *s, size_t n) {
    return register_index(s, n);
}

long cond_ntype(const char *s, size_t n) {
    size_t a = 0, b = n;
    trim(s, &a, &b);
    bool defer = false;
    if (a < b && s[a] == '@') { defer = true; a++; }
    if (a < b && s[a] == '#') return defer ? 037 : 027;
    int r = (b - a == 2 && s[a] == '%' && s[a + 1] >= '0' && s[a + 1] <= '7') ? s[a + 1] - '0' : reg_of(s + a, b - a);
    if (r >= 0) return defer ? 070 | r : r;                          /* Rn ; @Rn is the same as @0(Rn) */
    /* (Rn)+  -(Rn)  (Rn)  expr(Rn) */
    if (b > a + 1 && s[b - 1] == '+' && s[b - 2] == ')') {
        size_t o = b - 2;
        while (o > a && s[o] != '(') o--;
        int q = reg_of(s + o + 1, b - 2 - o - 1);
        if (s[o] == '(' && o == a && q >= 0) return (defer ? 030 : 020) | q;
    }
    if (b > a && s[b - 1] == ')') {
        size_t o = b - 1;
        while (o > a && s[o] != '(') o--;
        int q = s[o] == '(' ? reg_of(s + o + 1, b - 1 - o - 1) : -1;
        if (q >= 0) {
            if (o == a) return defer ? (070 | q) : (010 | q);
            if (o == a + 1 && s[a] == '-') return (defer ? 050 : 040) | q;
            return (defer ? 070 : 060) | q;
        }
    }
    return defer ? 077 : 067;
}
