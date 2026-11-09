/* Hand-written port of pdpy11/parser.py. Every p_* function returns NULL/false on a
   "RecoverableError" and restores the context to where it started. */
#include "ast.h"
#include "builtins.h"
#include "report.h"
#include "charset.h"
#include "macro.h"
#include <ctype.h>
#include "cond.h"

typedef struct { Src *src; size_t pos; } Ctx;
typedef struct { char c[4]; int n; } Term;
static const Term NEVER = {{0}, 0};

#define CODE(c) ((c)->src->text)
#define LEN(c) ((c)->src->len)
#define NONE 0xFFFFFFFFu

static uint32_t cp_at(const Ctx *c, size_t *adv) {
    if (c->pos >= LEN(c)) { if (adv) *adv = 0; return NONE; }
    size_t i = c->pos;
    uint32_t cp = u8_next(CODE(c), LEN(c), &i);
    if (adv) *adv = i - c->pos;
    return cp;
}
static int ascii_at(const Ctx *c, size_t off) {
    size_t p = c->pos + off;
    return p < LEN(c) ? (unsigned char)CODE(c)[p] : -1;
}
static inline int lc(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; }
static inline bool is_alpha_(int ch) { ch = lc(ch); return (ch >= 'a' && ch <= 'z') || ch == '_'; }
static inline bool is_digit_(int ch) { return ch >= '0' && ch <= '9'; }
static inline bool is_wordc(int ch) { return is_alpha_(ch) || is_digit_(ch); }

/* Comments: PDPy11 semicolon and Turbo8 double slash run to the end of the line; a Turbo8 slash-star block
   comment may span lines and may be followed by code on the same line. Returns the comment length at
   position p (0 if none); *open is set when a block comment has no closing star-slash (it then runs to
   the end of the file). */
static size_t comment_at(const Ctx *c, size_t p, bool *open) {
    if (open) *open = false;
    if (p >= LEN(c)) return 0;
    const char *s = CODE(c);
    size_t end = LEN(c);
    int ch = (unsigned char)s[p];
    if (ch == ';' || (ch == '/' && p + 1 < end && s[p + 1] == '/')) {
        const char *nl = memchr(s + p, '\n', end - p);
        return (nl ? (size_t)(nl - s) : end) - p;
    }
    if (ch == '/' && p + 1 < end && s[p + 1] == '*') {
        for (size_t q = p + 2; q + 1 < end; q++)
            if (s[q] == '*' && s[q + 1] == '/') return q + 2 - p;
        if (open) *open = true;
        return end - p;
    }
    return 0;
}

static void skip_ws(Ctx *c) {
    while (c->pos < LEN(c)) {
        size_t adv;
        uint32_t cp = cp_at(c, &adv);
        if (py_isspace(cp)) { c->pos += adv; continue; }
        bool open;
        size_t cl = comment_at(c, c->pos, &open);
        if (!cl) break;
        if (open) ERR1("unclosed-comment", c->src, c->pos, c->pos + 2, "This '/*' comment is never closed with '*/'");
        c->pos += cl;
    }
}
static bool at_eof(Ctx *c) { Ctx t = *c; skip_ws(&t); return t.pos >= LEN(&t); }

/* case-insensitive ASCII literal after whitespace */
static bool lit(Ctx *c, const char *s) {
    Ctx st = *c;
    skip_ws(c);
    size_t n = strlen(s);
    if (c->pos + n > LEN(c)) { *c = st; return false; }
    for (size_t i = 0; i < n; i++) if (lc((unsigned char)CODE(c)[c->pos + i]) != lc((unsigned char)s[i])) { *c = st; return false; }
    c->pos += n;
    return true;
}
/* literal without skipping whitespace */
static bool lit_nows(Ctx *c, const char *s) {
    size_t n = strlen(s);
    if (c->pos + n > LEN(c)) return false;
    for (size_t i = 0; i < n; i++) if (lc((unsigned char)CODE(c)[c->pos + i]) != lc((unsigned char)s[i])) return false;
    c->pos += n;
    return true;
}

static bool term_match(Ctx *c, const Term *t) {
    if (!t->n) return false;
    Ctx st = *c;
    skip_ws(c);
    int ch = ascii_at(c, 0);
    for (int i = 0; i < t->n; i++) if (ch == (unsigned char)t->c[i]) { c->pos++; return true; }
    *c = st;
    return false;
}
/* (~terminator + colon) */
static bool nt_colon(Ctx *c, const Term *t) {
    Ctx st = *c;
    Ctx probe = *c;
    if (term_match(&probe, t)) return false;
    if (lit(c, ":")) return true;
    *c = st;
    return false;
}

/* ---- regex-like scanners (with whitespace skipping) ---- */
static bool re_symbol(Ctx *c, size_t *s, size_t *e) {      /* [a-z_$][a-z_0-9$.]* */
    Ctx st = *c; skip_ws(c);
    int ch = ascii_at(c, 0);
    if (!(is_alpha_(ch) || ch == '$' || (ch == '.' && ascii_at(c, 1) == '.'))) { *c = st; return false; }      /* ..name: MACRO-11 style */
    size_t p = c->pos + 1;
    while (p < LEN(c)) { int d = (unsigned char)CODE(c)[p]; if (is_wordc(d) || d == '$' || d == '.') p++; else break; }
    *s = c->pos; *e = p; c->pos = p;
    return true;
}
static bool re_local_symbol(Ctx *c, size_t *s, size_t *e) { /* \d[a-z_0-9$.]* */
    Ctx st = *c; skip_ws(c);
    if (!is_digit_(ascii_at(c, 0))) { *c = st; return false; }
    size_t p = c->pos + 1;
    while (p < LEN(c)) { int d = (unsigned char)CODE(c)[p]; if (is_wordc(d) || d == '$' || d == '.') p++; else break; }
    *s = c->pos; *e = p; c->pos = p;
    return true;
}
static bool re_insn_name(Ctx *c, size_t *s, size_t *e) {    /* \.?[a-z_][a-z_0-9]* */
    Ctx st = *c; skip_ws(c);
    size_t p = c->pos;
    if (ascii_at(c, 0) == '.') p++;
    if (p >= LEN(c) || !is_alpha_((unsigned char)CODE(c)[p])) { *c = st; return false; }
    p++;
    while (p < LEN(c) && is_wordc((unsigned char)CODE(c)[p])) p++;
    *s = c->pos; *e = p; c->pos = p;
    return true;
}
static bool re_label_name(Ctx *c, size_t *s, size_t *e) {   /* [a-z_0-9$.]+ */
    Ctx st = *c; skip_ws(c);
    size_t p = c->pos;
    while (p < LEN(c)) { int d = (unsigned char)CODE(c)[p]; if (is_wordc(d) || d == '$' || d == '.') p++; else break; }
    if (p == c->pos) { *c = st; return false; }
    *s = c->pos; *e = p; c->pos = p;
    return true;
}
/* newline regex "\s*\n|\s*;[^\n]*" at current position (no ws skipping), lookahead only.
   Turbo8 comments count too: '//' like ';', and a block comment is whitespace (a newline if it spans lines). */
static bool newline_ahead(const Ctx *c) {
    size_t p = c->pos;
    bool seen_nl = false;
    while (p < LEN(c)) {
        size_t q = p;
        uint32_t cp = u8_next(CODE(c), LEN(c), &q);
        if (py_isspace(cp)) {
            if (cp == '\n') seen_nl = true;
            p = q;
            continue;
        }
        if (CODE(c)[p] == '/' && p + 1 < LEN(c) && CODE(c)[p + 1] == '*') {
            size_t cl = comment_at(c, p, NULL);
            if (memchr(CODE(c) + p, '\n', cl)) seen_nl = true;
            p += cl;
            continue;
        }
        break;
    }
    if (seen_nl) return true;
    return p < LEN(c) && comment_at(c, p, NULL) > 0;
}

/* ahead of the cursor: hidden register-synonym statement "%n = name"? (so that '%' is not taken as modulo) */
static bool reg_synonym_ahead(const Ctx *c) {
    Ctx p = *c;
    skip_ws(&p);
    if (ascii_at(&p, 0) != '%') return false;
    int d = ascii_at(&p, 1);
    if (d < '0' || d > '7' || is_wordc(ascii_at(&p, 2))) return false;
    p.pos += 2;
    skip_ws(&p);
    return ascii_at(&p, 0) == '=' && ascii_at(&p, 1) != '=';
}

/* ---- helpers ---- */
static const char *sub_(const Ctx *c, size_t s, size_t e) { return astrndup(CODE(c) + s, e - s); }
static Node *mk(const Ctx *c, NodeKind k, size_t s, size_t e) { return node_new(k, c->src, s, e); }

static uint32_t lower_cp(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c >= 0x391 && c <= 0x3A9) return c + 32;
    return c;
}

static void u32_push(U32 *s, size_t *cap, uint32_t cp) {
    if (s->n == *cap) { *cap = *cap ? *cap * 2 : 8; s->cp = (uint32_t *)xrealloc(s->cp, *cap * 4); }
    s->cp[s->n++] = cp;
}
static U32 u32_arena(const U32 *tmp) {
    U32 r; r.n = tmp->n;
    r.cp = (uint32_t *)amalloc((tmp->n + 1) * 4);
    if (tmp->n) memcpy(r.cp, tmp->cp, tmp->n * 4);
    return r;
}

static bool is_builtin_name(const char *s, size_t l) { return builtin_find(s, l) != NULL; }

/* ---- forward decls ---- */
static Node *p_expression(Ctx *c, const Term *t, bool report_crit_args);
static Node *p_code(Ctx *c, bool brk, size_t open_pos);
static int g_macro_depth;
static Node *p_instruction_pointer(Ctx *c);

/* ---- int parsing like Python's int(s, base) restricted to our alphabets ---- */
static bool py_int(const char *s, size_t n, int base, int64_t *out) {
    if (n >= 2 && s[0] == '0') {
        int b2 = lc((unsigned char)s[1]) == 'x' ? 16 : lc((unsigned char)s[1]) == 'o' ? 8 : lc((unsigned char)s[1]) == 'b' ? 2 : 0;
        if (b2 && b2 == base) { s += 2; n -= 2; }
    }
    if (!n) return false;
    uint64_t v = 0; bool ovf = false;
    for (size_t i = 0; i < n; i++) {
        int ch = lc((unsigned char)s[i]), d;
        if (is_digit_(ch)) d = ch - '0'; else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 10; else return false;
        if (d >= base) return false;
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) ovf = true; else v = v * (uint64_t)base + (uint64_t)d;
    }
    if (ovf || v > (uint64_t)INT64_MAX) v = (uint64_t)INT64_MAX;
    *out = (int64_t)v;
    return true;
}

/* ---- number ---- */
static Node *p_number(Ctx *c, const Term *term) {
    Ctx st = *c;
    bool negative = lit(c, "-");
    int64_t sign = negative ? -1 : 1;
    const char *sign_str = negative ? "-" : "";
    skip_ws(c);
    size_t cs = c->pos;

    static const struct { const char *prefix, *adj; int base; } P[] = {
        {"^X", "A hexadecimal", 16}, {"^O", "An octal", 8}, {"^B", "A binary", 2}, {"^D", "A decimal", 10}};
    for (int k = 0; k < 4; k++) {
        if (!lit(c, P[k].prefix)) continue;
        if (ascii_at(c, 0) == '<') {                       /* MACRO-11 style ^O<377>: the digits in angle brackets */
            size_t q = c->pos + 1, ds = q;
            while (q < LEN(c) && CODE(c)[q] != '>' && CODE(c)[q] != '\n') q++;
            if (q < LEN(c) && CODE(c)[q] == '>' && q > ds) {
                int64_t v = 0;
                if (py_int(sub_(c, ds, q), q - ds, P[k].base, &v)) {
                    c->pos = q + 1;
                    Node *n = mk(c, N_NUMBER, cs, c->pos);
                    n->u.num.repr = afmt("%s%s<%s>", sign_str, P[k].prefix, sub_(c, ds, q));
                    n->u.num.value = v * sign;
                    return n;
                }
            }
        }
        size_t p = c->pos;
        while (p < LEN(c)) {
            int d = lc((unsigned char)CODE(c)[p]);
            bool ok = P[k].base == 16 ? (is_digit_(d) || (d >= 'a' && d <= 'f')) : P[k].base == 8 ? (d >= '0' && d <= '7') : P[k].base == 2 ? (d == '0' || d == '1') : is_digit_(d);
            if (ok) p++; else break;
        }
        bool good = p > c->pos && (p >= LEN(c) || (!is_wordc((unsigned char)CODE(c)[p]) && CODE(c)[p] != '$' && CODE(c)[p] != '.' && ((unsigned char)CODE(c)[p] < 0x80 || true)));
        if (good && p < LEN(c) && (unsigned char)CODE(c)[p] >= 0x80) { /* non-ASCII follows: \b depends on category; treat as word char */
            size_t q = p; uint32_t cp = u8_next(CODE(c), LEN(c), &q); (void)cp; good = false;
        }
        if (!good) {
            CRIT1("invalid-number", c->src, cs, c->pos, "%s number was expected after '%s'", P[k].adj, P[k].prefix);
        }
        const char *num = sub_(c, c->pos, p);
        int64_t v = 0; py_int(num, p - c->pos, P[k].base, &v);
        c->pos = p;
        Node *n = mk(c, N_NUMBER, cs, c->pos);
        n->u.num.repr = afmt("%s%s%s", sign_str, P[k].prefix, num);
        n->u.num.value = v * sign;
        return n;
    }

    size_t ns, ne;
    if (!re_local_symbol(c, &ns, &ne)) { *c = st; return NULL; }
    {
        if (nt_colon(c, term)) { *c = st; return NULL; }
    }
    size_t nl = ne - ns;
    bool has_dot = CODE(c)[ne - 1] == '.';
    if (has_dot) nl--;
    const char *num = sub_(c, ns, ns + nl);
    bool has_bad = false, all_digit = true, has89 = false;
    for (size_t i = 0; i < nl; i++) {
        char ch = num[i];
        if (ch == '$' || ch == '_' || ch == '.') has_bad = true;
        if (!is_digit_(ch)) all_digit = false;
        if (ch == '8' || ch == '9') has89 = true;
    }
    if (has_bad) { *c = st; return NULL; }
    int radix = cond_radix();                          /* set by .radix: the base of the numbers without a prefix and a dot */
    if (radix == 16 && !has_dot && !all_digit && nl > 0) {
        bool hex = true;
        for (size_t i = 0; i < nl; i++) if (!is_digit_(num[i]) && !(lc((unsigned char)num[i]) >= 'a' && lc((unsigned char)num[i]) <= 'f')) hex = false;
        int64_t v;
        if (hex && py_int(num, nl, 16, &v)) {
            Node *n = mk(c, N_NUMBER, cs, c->pos);
            n->u.num.repr = afmt("%s%s", sign_str, num);
            n->u.num.value = v * sign;
            return n;
        }
    }
    if (all_digit && nl > 0 && !has_dot && radix != 8) {
        int64_t v = 0;
        bool ok = py_int(num, nl, radix, &v);
        if (!ok) {
            ERR1("invalid-number", c->src, cs, c->pos, "This number has a digit that is not valid in radix %d (set by .radix).\nAdd a dot after the number to make it decimal.", radix);
            v = 0;
        }
        Node *n = mk(c, N_NUMBER, cs, c->pos);
        n->u.num.repr = afmt("%s%s", sign_str, num);
        n->u.num.value = v * sign;
        n->u.num.valid_label = sign == 1;
        return n;
    }
    if (all_digit && nl > 0) {
        int64_t v;
        if (has_dot) {
            py_int(num, nl, 10, &v);
            Node *n = mk(c, N_NUMBER, cs, c->pos);
            n->u.num.repr = afmt("%s%s.", sign_str, num);
            n->u.num.value = v * sign;
            return n;
        }
        if (has89) {
            py_int(num, nl, 10, &v);
            if (sign == -1) {
                char d = strchr(num, '8') ? '8' : '9';
                ERR1("invalid-number", c->src, cs, c->pos,
                     "In PDP-11 assembly, numbers are considered base-8 by default.\nThis number has digit %c, so you probably wanted it base-10.\nAdd a dot after the number to switch to decimal: '-%s.'\nIf you wanted to specify a local label of the same name, add a colon: '-%s:'", d, num, num);
                Node *n = mk(c, N_NUMBER, cs, c->pos);
                n->u.num.repr = afmt("-%s", num);
                n->u.num.value = v * sign;
                return n;
            }
            Node *n = mk(c, N_NUMBER, cs, c->pos);
            n->u.num.repr = num;
            n->u.num.value = v;
            n->u.num.valid_label = true;
            n->u.num.invalid_base8 = true;
            return n;
        }
        py_int(num, nl, 8, &v);
        Node *n = mk(c, N_NUMBER, cs, c->pos);
        n->u.num.repr = afmt("%s%s", sign_str, num);
        n->u.num.value = v * sign;
        n->u.num.valid_label = sign == 1;
        return n;
    }
    if (nl >= 2 && num[0] == '0' && is_alpha_((unsigned char)num[1]) && num[1] != '_') {
        int bc = lc((unsigned char)num[1]);
        int base = bc == 'x' ? 16 : bc == 'o' ? 8 : bc == 'b' ? 2 : 0;
        if (!base) { *c = st; return NULL; }
        int64_t v;
        if (!py_int(num + 2, nl - 2, base, &v)) { *c = st; return NULL; }
        Node *n = mk(c, N_NUMBER, cs, c->pos);
        n->u.num.repr = afmt("%s%s", sign_str, num);
        n->u.num.value = v * sign;
        n->u.num.valid_label = sign == 1;
        return n;
    }
    *c = st;
    return NULL;
}

static Node *p_radix50_literal(Ctx *c) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    if (!lit(c, "^R")) { *c = st; return NULL; }
    size_t p = c->pos;
    while (p < LEN(c)) {
        int ch = (unsigned char)CODE(c)[p];
        if (is_alpha_(ch) && ch != '_' ) p++;
        else if (is_digit_(ch) || ch == '$' || ch == '.' || ch == '%') p++;
        else break;
    }
    char *str;
    size_t n = p - c->pos;
    if (n == 0) {
        ERR1("invalid-string", c->src, cs, c->pos, "Up to three radix-50 characters are expected after ^R.");
        str = astrdup("");
    } else {
        str = astrndup(CODE(c) + c->pos, n);
        c->pos = p;
    }
    size_t len = strlen(str);
    if (len > 3) {
        ERR1("invalid-string", c->src, cs, c->pos, "This radix-50 literal contains more than 3 characters (%zu, in particular).", len);
        str[3] = 0; len = 3;
    }
    for (size_t i = 0; i < len; i++) if (str[i] >= 'a' && str[i] <= 'z') str[i] = (char)(str[i] - 32);
    /* pack_to_int */
    char padded[4] = "   ";
    memcpy(padded, str, len);
    int64_t v = 0; int mul[3] = {1600, 40, 1};
    for (int i = 0; i < 3; i++) {
        const char *q = strchr(RADIX50_TABLE, padded[i]);
        v += (int64_t)(q ? q - RADIX50_TABLE : 0) * mul[i];
    }
    Node *nd = mk(c, N_NUMBER, cs, c->pos);
    nd->u.num.repr = afmt("^R%s", str);
    nd->u.num.value = v;
    return nd;
}

static Node *p_label(Ctx *c) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    size_t ns, ne;
    if (!re_label_name(c, &ns, &ne)) { *c = st; return NULL; }
    if (!lit(c, ":")) { *c = st; return NULL; }
    bool is_extern = lit_nows(c, ":");
    const char *name = sub_(c, ns, ne);
    size_t nl = ne - ns;
    if (is_builtin_name(name, nl)) {
        WARN1("suspicious-name", c->src, cs, c->pos, "This symbol suspiciously resembles an instruction, but is parsed as a label definition.\nPlease consider changing the label not to look like an instruction");
    } else if (is_register_name(name, nl)) {
        ERR1("reserved-name", c->src, cs, c->pos, "Label name clashes with a register. All accesses to this symbol would be ambiguous.");
    }
    if (is_digit_((unsigned char)name[0]) && is_extern) {
        is_extern = false;
        ERR1("invalid-extern", c->src, cs, c->pos, "A local label cannot be external. Either give it a global name or remove double colon.");
    }
    {   /* "A:B:" with no space: it looks like a qualified name "A:B", so the labels must be separated */
        size_t q = c->pos;
        while (q < LEN(c) && (is_wordc((unsigned char)CODE(c)[q]) || CODE(c)[q] == '$' || CODE(c)[q] == '.')) q++;
        if (q > c->pos && q < LEN(c) && CODE(c)[q] == ':') {
            const char *nx = sub_(c, c->pos, q);
            ERR1("label-chain", c->src, cs, q + 1,
                 "Several labels in a row must be separated by whitespace: 'Name:Label' without spaces is a reference to the symbol Label of the file where Name is defined, not a label definition.\nWrite '%s:%s %s:' instead.", name, is_extern ? ":" : "", nx);
        }
    }
    Node *n = mk(c, N_LABEL, cs, c->pos);
    n->u.label.name = name;
    n->u.label.local = is_digit_((unsigned char)name[0]);
    n->u.label.is_extern = is_extern;
    return n;
}

/* A constant assigned again inside a macro expansion (MACRO-11 allows "x = 1 ... x = 2"): each new value gets its own
   internal name, and the references that follow use it, so the value is the one in force at the point of use. */
typedef struct { const char *name, *lname; } SymAlias;
static SMap g_assigned, g_alias;
static unsigned g_alias_ver;

static Node *mk_symbol(Ctx *c, size_t s, size_t e, const char *name, bool nec) {
    Node *n = mk(c, N_SYMBOL, s, e);
    size_t l = strlen(name);
    char *low = (char *)amalloc(l + 1);
    lower_ascii_n(low, name, l);
    const SymAlias *al = g_alias.cap ? (const SymAlias *)smap_get(&g_alias, low, l) : NULL;
    if (al) { name = al->name; low = (char *)al->lname; }
    n->u.sym.name = name;
    n->u.sym.lname = low;
    n->u.sym.nec_label = nec;
    return n;
}

static Node *p_assignment(Ctx *c) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    Node *target = p_instruction_pointer(c);
    const char *symname = NULL;
    if (!target) {
        size_t a, b;
        if (!re_symbol(c, &a, &b)) { *c = st; return NULL; }
        symname = sub_(c, a, b);
        target = mk_symbol(c, cs, c->pos, symname, false);
    }
    skip_ws(c);
    size_t ctx_equals = c->pos;
    if (!lit(c, "=")) { *c = st; return NULL; }
    bool is_extern = lit_nows(c, "=");
    size_t after_equals = c->pos;
    skip_ws(c);
    Node *value = p_expression(c, &NEVER, false);
    if (!value) {
        rep2(PRI_CRITICAL, "invalid-assignment", c->src, ctx_equals, after_equals, "An equals sign must be followed by an expression (as in assignment)",
             c->src, c->pos, c->pos, "...yet no expression was matched here");
    }
    if (target->kind == N_SYMBOL) {
        if (is_builtin_name(target->u.sym.name, strlen(target->u.sym.name))) {
            WARN1("suspicious-name", c->src, target->s, target->e, "This symbol suspiciously resembles an instruction, but is parsed as a constant definition.\nPlease consider changing the constant name not to look like an instruction");
        } else if (is_register_name(target->u.sym.name, strlen(target->u.sym.name))) {
            ERR1("reserved-name", c->src, target->s, target->e, "Symbol name clashes with a register. All accesses to this symbol would be\nambiguous. Maybe you are unfamiliar with assembly and wanted to say 'mov #%s, %s'?", node_repr_str(value), symname);
        }
    }
    if (is_extern && target->kind == N_IPTR) {
        ERR1("invalid-assignment", c->src, cs, after_equals, "Assignment to '.' cannot be external");
        is_extern = false;
    }
    if (target->kind == N_SYMBOL) {
        /* the plain name (before any alias) decides whether this is a new value of an already assigned constant */
        const char *pn = symname;
        size_t pl = strlen(pn);
        char *plow = (char *)amalloc(pl + 1);
        lower_ascii_n(plow, pn, pl);
        if (smap_get(&g_assigned, plow, pl)) {
            if (g_macro_depth > 0) {
                SymAlias *al = (SymAlias *)amalloc(sizeof(SymAlias));
                al->name = afmt("%s~%u", pn, ++g_alias_ver);
                char *ln = (char *)amalloc(strlen(al->name) + 1);
                lower_ascii_n(ln, al->name, strlen(al->name));
                al->lname = ln;
                *smap_slot(&g_alias, plow, pl, NULL) = al;
                target->u.sym.name = al->name;
                target->u.sym.lname = al->lname;
            }
        } else {
            *smap_slot(&g_assigned, plow, pl, NULL) = (void *)1;
        }
    }
    Node *n = mk(c, N_ASSIGN, cs, c->pos);
    n->u.assign.target = target; n->u.assign.value = value; n->u.assign.is_extern = is_extern;
    return n;
}

static Node *p_symbol_expression(Ctx *c, const Term *term) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    size_t a, b;
    if (!re_symbol(c, &a, &b)) { *c = st; return NULL; }
    bool has_colon = nt_colon(c, term);
    const char *name = sub_(c, a, b);
    if (is_builtin_name(name, b - a) && !has_colon) {
        WARN1("suspicious-name", c->src, cs, c->pos, "This symbol suspiciously resembles an instruction, but is parsed as an operand.\nCheck for a missing newline or an excess comma before it.");
    }
    /* "Name:2" is the local label 2: of the scope that starts after the global label Name:; "Name:Init" is the symbol Init of the
       file where Name is defined. The second part follows the colon at once; only one level is allowed. */
    if (has_colon && !is_digit_((unsigned char)name[0]) && (is_wordc((unsigned char)ascii_at(c, 0)) || ascii_at(c, 0) == '$' || ascii_at(c, 0) == '.')) {
        size_t p = c->pos;
        bool local = is_digit_(ascii_at(c, 0));
        while (p < LEN(c) && (is_wordc((unsigned char)CODE(c)[p]) || CODE(c)[p] == '$' || (!local && CODE(c)[p] == '.'))) p++;
        const char *loc = sub_(c, c->pos, p);
        size_t ll = p - c->pos;
        (void)ll;
        char *low = (char *)loc;                        /* the original case; it is lowered when looked up */
        c->pos = p;
        if (ascii_at(c, 0) == ':' && (is_wordc((unsigned char)ascii_at(c, 1)) || ascii_at(c, 1) == '$')) {
            size_t q = c->pos + 1;
            while (q < LEN(c) && (is_wordc((unsigned char)CODE(c)[q]) || CODE(c)[q] == '$')) q++;
            ERR1("nested-qualifier", c->src, cs, q, "Only one level is allowed: 'Name:Label' or 'Name:N'. A reference like 'A:B:C' is not supported");
            c->pos = q;
        }
        Node *n = mk_symbol(c, cs, c->pos, name, true);
        n->u.sym.loc = low;
        return n;
    }
    return mk_symbol(c, cs, c->pos, name, has_colon);
}

static Node *p_local_symbol_expression(Ctx *c, const Term *term) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    size_t a, b;
    if (!re_local_symbol(c, &a, &b)) { *c = st; return NULL; }
    const char *name = sub_(c, a, b);
    bool all_digits = true;
    for (size_t i = 0; i < b - a; i++) if (!is_digit_((unsigned char)name[i])) all_digits = false;
    bool has_colon = nt_colon(c, term);
    if (all_digits && !has_colon) { *c = st; return NULL; }
    return mk_symbol(c, cs, c->pos, name, has_colon);
}

/* ---- strings ---- */
/* string_escape | character ; appends to out. Returns false if at EOF */
static bool p_string_char(Ctx *c, U32 *out, size_t *cap) {
    if (ascii_at(c, 0) == '\\') {
        size_t cs = c->pos;
        c->pos++;
        size_t adv;
        uint32_t ch = cp_at(c, &adv);
        if (ch == NONE) {
            ERR1("invalid-escape", c->src, cs, c->pos, "A letter is expected after a backslash '\\' in a string");
            return true;
        }
        c->pos += adv;
        uint32_t l = lower_cp(ch);
        if (l == 'n') u32_push(out, cap, '\n');
        else if (l == 'r') u32_push(out, cap, '\r');
        else if (l == 't') u32_push(out, cap, '\t');
        else if (l == '\\' || l == '"' || l == '\'' || l == '/') u32_push(out, cap, l);
        else if (l == '\n') { }
        else if (l == 'x') {
            skip_ws(c);
            int h1 = ascii_at(c, 0), h2 = ascii_at(c, 1);
            #define ISHEX(x) (is_digit_(x) || (lc(x) >= 'a' && lc(x) <= 'f'))
            if (h1 >= 0 && h2 >= 0 && ISHEX(h1) && ISHEX(h2)) {
                char t[3] = {(char)h1, (char)h2, 0};
                int64_t v; py_int(t, 2, 16, &v);
                c->pos += 2;
                u32_push(out, cap, (uint32_t)v);
            } else {
                ERR1("invalid-escape", c->src, cs, c->pos, "Two hexadecimal digits are expected after '\\x' in a string");
            }
        } else {
            Buf b = {0}; buf_put_utf8(&b, l);
            ERR1("invalid-escape", c->src, cs, c->pos, "Unknown escape '\\%s' in a string", b.p);
            buf_free(&b);
        }
        return true;
    }
    size_t adv;
    uint32_t ch = cp_at(c, &adv);
    if (ch == NONE) return false;
    c->pos += adv;
    u32_push(out, cap, ch);
    return true;
}

static bool at_nl_or_eof(const Ctx *c) {
    if (c->pos >= LEN(c)) return true;
    char ch = CODE(c)[c->pos];
    return ch == '\t' || ch == '\r' || ch == '\n';
}

static Node *p_single_quoted(Ctx *c) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    if (!lit(c, "'")) { *c = st; return NULL; }
    if (at_nl_or_eof(c)) {
        CRIT1("unterminated-string", c->src, cs, c->pos, "Unterminated string literal. A single character is expected after '.");
    }
    U32 val = {0}; size_t cap = 0;
    if (ascii_at(c, 0) != '\'') p_string_char(c, &val, &cap);
    bool pair = false;
    if (val.n == 1) {                      /* 'AJ' : two characters closed by a quote make a word (first one in the low byte) */
        int b1 = ascii_at(c, 0);
        if (b1 >= 0x80 || (b1 > 0 && (isalnum(b1) || b1 == '_'))) {
            Ctx t = *c;
            p_string_char(&t, &val, &cap);
            if (val.n == 2 && ascii_at(&t, 0) == '\'') { *c = t; c->pos++; pair = true; }
            else val.n = 1;
        }
    }
    if (pair) {
    } else if (c->pos < LEN(c) && CODE(c)[c->pos] == '\'') {
        c->pos++;
        WARN1("excess-quote", c->src, cs, c->pos, "Single quotation mark denotes not a string, but an ASCII value of a character.\nUnlike C, a character must not be terminated by a single quotation mark.\nFor example, 'a should be used instead of 'a'. %s",
              val.n == 0 ? "An empty string can be safely replaced with a zero." : "Please remove the second quotation mark.");
    }
    Buf b = {0}; buf_putc(&b, '\''); for (size_t i = 0; i < val.n; i++) buf_put_utf8(&b, val.cp[i]);
    if (pair) buf_putc(&b, '\'');
    Node *n = mk(c, N_CHARLIT, cs, c->pos);
    n->u.chr.repr = astrndup(b.p, b.len); buf_free(&b);
    n->u.chr.str = u32_arena(&val); free(val.cp);
    return n;
}

static Node *p_double_quoted(Ctx *c) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    if (!lit(c, "\"")) { *c = st; return NULL; }
    U32 val = {0}; size_t cap = 0;
    for (int k = 0; k < 2; k++) {
        if (at_nl_or_eof(c)) {
            CRIT1("unterminated-string", c->src, cs, c->pos, "Unterminated string literal. Exactly two characters are expected after \".");
        }
        if (CODE(c)[c->pos] != '"') p_string_char(c, &val, &cap);
    }
    if (c->pos < LEN(c) && CODE(c)[c->pos] == '"') {
        c->pos++;
        Buf vb = {0}; for (size_t i = 0; i < val.n; i++) buf_put_utf8(&vb, val.cp[i]);
        const char *vs = vb.p ? vb.p : "";
        const char *tail;
        if (val.n == 0) tail = "An empty string, as here, can be safely replaced with a zero.";
        else if (val.n == 1) tail = afmt("Exactly two characters are expected after the quotation mark, and the closing mark is unnecessary.\nPlease use '%s instead.", vs);
        else tail = afmt("Unlike C, this literal must not be terminated by a quotation mark -- please remove it: \"%s", vs);
        WARN1("excess-quote", c->src, cs, c->pos, "Double quotation mark does not denote a string. It denotes an ASCII conversion operator,\nwhich means that the two characters after \" are converted to a 16-bit word.\nFor example, \"AB is the same as 0x4142 (or ^H4142).\n%s", tail);
        buf_free(&vb);
    }
    Buf b = {0}; buf_putc(&b, '"'); for (size_t i = 0; i < val.n; i++) buf_put_utf8(&b, val.cp[i]);
    Node *n = mk(c, N_CHARLIT, cs, c->pos);
    n->u.chr.repr = astrndup(b.p, b.len); buf_free(&b);
    n->u.chr.str = u32_arena(&val); free(val.cp);
    return n;
}

static Node *p_quoted_string(Ctx *c) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    int q = ascii_at(c, 0);
    if (!(q == '\'' || q == '"' || q == '/')) { *c = st; return NULL; }
    c->pos++;
    U32 val = {0}; size_t cap = 0;
    while (c->pos < LEN(c) && CODE(c)[c->pos] != q) p_string_char(c, &val, &cap);
    if (c->pos >= LEN(c)) {
        CRIT1("unterminated-string", c->src, cs, c->pos, "Unterminated string literal");
    }
    c->pos++;
    Node *n = mk(c, N_QSTR, cs, c->pos);
    n->u.qstr.quote = astrndup((const char *)&(char){(char)q}, 1);
    n->u.qstr.str = u32_arena(&val); free(val.cp);
    return n;
}

static Node *p_angle(Ctx *c) {
    Ctx st = *c;
    size_t cs = c->pos;
    if (!lit(c, "<")) { *c = st; return NULL; }
    Node *e = p_expression(c, &NEVER, false);
    if (!e) { *c = st; return NULL; }
    if (!lit(c, ">")) { *c = st; return NULL; }
    Node *n = mk(c, N_ANGLE, cs, c->pos);
    n->u.angle.expr = e;
    return n;
}

static Node *p_long_string(Ctx *c) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    PVec ch = {0};
    Node *f = p_quoted_string(c);
    if (!f) f = p_angle(c);
    if (!f) { *c = st; return NULL; }
    pvec_push(&ch, f);
    for (;;) {
        Node *x = p_quoted_string(c);
        if (!x) x = p_angle(c);
        if (!x) break;
        pvec_push(&ch, x);
    }
    if (ch.n == 1) { Node *r = (Node *)ch.a[0]; free(ch.a); return r; }
    Node *n = mk(c, N_STRCAT, cs, c->pos);
    n->u.cat.chunks = ch;
    return n;
}

static Node *p_instruction_pointer(Ctx *c) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    if (ascii_at(c, 0) != '.') { *c = st; return NULL; }
    int nx = ascii_at(c, 1);
    if (nx >= 0 && (is_wordc(nx) || nx == '.')) { *c = st; return NULL; }
    c->pos++;
    return mk(c, N_IPTR, cs, c->pos);
}

static Node *p_expression_literal(Ctx *c, const Term *term) {
    Node *e = p_symbol_expression(c, term);
    if (e) return e;
    e = p_radix50_literal(c);
    if (e) return e;
    e = p_number(c, term);
    if (e) return e;
    e = p_local_symbol_expression(c, term);
    if (e) return e;
    e = p_single_quoted(c);
    if (e) return e;
    e = p_double_quoted(c);
    if (e) return e;
    return p_instruction_pointer(c);
}

/* operator literal tables in registration order */
static const OpId INFIX_ORDER[] = {OP_MUL, OP_DIV, OP_MOD, OP_ADD, OP_SUB, OP_LSHIFT, OP_RSHIFT, OP_LSH, OP_AND, OP_XOR, OP_OR, OP_OR2, OP_CALL};
static const OpId PREFIX_ORDER[] = {OP_POS, OP_NEG, OP_INV, OP_INV2, OP_IMMEDIATE, OP_DEFERRED, OP_REGISTER};
static const OpId POSTFIX_ORDER[] = {OP_POSTADD, OP_POSTSUB};

static const OpDef *match_op(Ctx *c, const OpId *order, size_t n) {
    for (size_t i = 0; i < n; i++) if (lit(c, OPS[order[i]].ch)) return &OPS[order[i]];
    return NULL;
}
#define MATCH_INFIX(c) match_op(c, INFIX_ORDER, sizeof INFIX_ORDER / sizeof INFIX_ORDER[0])
#define MATCH_PREFIX(c) match_op(c, PREFIX_ORDER, sizeof PREFIX_ORDER / sizeof PREFIX_ORDER[0])
#define MATCH_POSTFIX(c) match_op(c, POSTFIX_ORDER, sizeof POSTFIX_ORDER / sizeof POSTFIX_ORDER[0])

static bool caret_paren(Ctx *c, char out[3]) {   /* \^[$_=[\]\\{}|:/<>?] after ws */
    Ctx st = *c; skip_ws(c);
    if (ascii_at(c, 0) == '^') {
        int d = ascii_at(c, 1);
        if (d >= 0 && strchr("$_=[]\\{}|:/<>?", d) && d != 0) { out[0] = '^'; out[1] = (char)d; out[2] = 0; c->pos += 2; return true; }
    }
    *c = st; return false;
}

static Node *mk_op(Ctx *c, const OpDef *op, size_t s, size_t e, Node *lhs, Node *rhs) {
    Node *n = mk(c, op->kind == 0 ? N_INFIX : op->kind == 1 ? N_PREFIX : N_POSTFIX, s, e);
    n->u.op.op = op; n->u.op.lhs = lhs; n->u.op.rhs = rhs;
    return n;
}

static Node *p_expression_literal_rec(Ctx *c, const Term *term) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    Node *value = NULL;
    bool have_value = false;
    {
        Ctx probe = *c; char cb[3];
        bool open = lit(&probe, "(") || lit(&probe, "<") || caret_paren(&probe, cb);
        if (!open) {
            value = p_expression_literal(c, term);
            if (!value) { *c = st; return NULL; }
            have_value = true;
        }
    }
    for (;;) {
        char opening[3] = {0};
        bool got;
        if (!have_value) {
            got = false;
            if (lit(c, "(")) { strcpy(opening, "("); got = true; }
            else if (lit(c, "<")) { strcpy(opening, "<"); got = true; }
            else got = caret_paren(c, opening);
        } else {
            got = false;
            if (lit(c, "(")) { strcpy(opening, "("); got = true; }
        }
        if (!got) break;
        char closing[2] = {0};
        Term nt = *term;
        if (opening[0] == '(') closing[0] = ')';
        else if (opening[0] == '<') closing[0] = '>';
        else { closing[0] = opening[1]; if (nt.n < 4) nt.c[nt.n++] = closing[0]; }
        size_t after_paren = c->pos;
        skip_ws(c);
        Node *expr = p_expression(c, &nt, false);
        if (!expr) {
            rep2(PRI_CRITICAL, "invalid-expression", c->src, c->pos, c->pos, "Could not parse an expression here",
                 c->src, cs, after_paren, "...as expected after an opening parenthesis here");
        }
        skip_ws(c);
        if (!lit(c, closing)) {
            rep2(PRI_CRITICAL, "invalid-expression", c->src, c->pos, c->pos, "This is not a operator, so a closing parenthesis is expected here",
                 c->src, cs, after_paren, "...to match an opening parenthesis here");
        }
        if (!have_value) {
            Node *pn = mk(c, N_PAREN, cs, c->pos);
            pn->u.paren.expr = expr;
            pn->u.paren.open = astrdup(opening);
            pn->u.paren.close = astrdup(closing);
            value = pn; have_value = true;
        } else {
            value = mk_op(c, &OPS[OP_CALL], cs, c->pos, value, expr);
        }
    }
    return value;
}

typedef struct { const OpDef *op; size_t cstart; } OpEnt;

/* report_crit_args is unused: kept for symmetry */
static Node *p_expression(Ctx *c, const Term *term, bool unused) {
    (void)unused;
    Ctx st = *c;
    OpEnt *ops = NULL; size_t nops = 0, capops = 0;
    Node **stack = NULL; size_t nst = 0, capst = 0;
#define PUSHOP(o, cs_) do { if (nops == capops) { capops = capops ? capops * 2 : 8; ops = (OpEnt *)xrealloc(ops, capops * sizeof(OpEnt)); } ops[nops].op = (o); ops[nops].cstart = (cs_); nops++; } while (0)
#define PUSHST(n_) do { if (nst == capst) { capst = capst ? capst * 2 : 8; stack = (Node **)xrealloc(stack, capst * sizeof(Node *)); } stack[nst++] = (n_); } while (0)
    skip_ws(c);
    size_t ctx_start = c->pos;
    size_t ctx_op = c->pos;
    Node *expr;
    for (;;) {
        expr = p_expression_literal_rec(c, term);
        if (expr) break;
        bool report = nops > 0;
        {
            Ctx probe = *c;
            if (term_match(&probe, term)) {
                if (report) rep2(PRI_CRITICAL, "invalid-expression", c->src, probe.pos, probe.pos, "Expected an expression or an operator here", c->src, ctx_start, probe.pos, "...after a prefix here");
                free(ops); free(stack); *c = st; return NULL;
            }
        }
        const OpDef *po = MATCH_PREFIX(c);
        if (!po) {
            skip_ws(c);
            size_t before_op = c->pos;
            if (ascii_at(c, 0) == '^' && ascii_at(c, 1) >= 0 && !py_isspace((uint32_t)ascii_at(c, 1)) ) {
                size_t adv = 1; { Ctx t2 = *c; t2.pos++; cp_at(&t2, &adv); }
                c->pos += 1 + adv;
                const char *txt = sub_(c, before_op, c->pos);
                CRIT1("invalid-expression", c->src, before_op, c->pos, "'%s' is an invalid prefix operator", txt);
            }
            if (report) rep2(PRI_CRITICAL, "invalid-expression", c->src, c->pos, c->pos, "Expected an expression or an operator here", c->src, ctx_start, c->pos, "...after a prefix here");
            free(ops); free(stack); *c = st; return NULL;
        }
        PUSHOP(po, ctx_op);
        skip_ws(c);
        ctx_op = c->pos;
    }
    PUSHST(expr);

#define POP_OP(end_) do { \
        OpEnt info = ops[--nops]; \
        if (info.op->kind == 0) { Node *rhs_ = stack[--nst]; Node *lhs_ = stack[--nst]; PUSHST(mk_op(c, info.op, lhs_->s, (end_), lhs_, rhs_)); } \
        else { Node *o_ = stack[--nst]; PUSHST(mk_op(c, info.op, info.cstart, (end_), NULL, o_)); } \
    } while (0)

    for (;;) {
        size_t ctx_prev = c->pos;
        skip_ws(c);
        ctx_op = c->pos;
        bool is_postfix = false;
        {
            Ctx p = *c;
            if (!term_match(&p, term)) {
                const OpDef *pf = MATCH_POSTFIX(&p);
                if (pf) {
                    Ctx p2 = p;
                    if (newline_ahead(&p2) || lit(&p2, ",") || lit(&p2, ")") || lit(&p2, "}") || at_eof(&p2)) is_postfix = true;
                    /* "(newline | comma | ...)" - newline is tested at p exactly */
                    if (!is_postfix) { Ctx p3 = p; if (newline_ahead(&p3)) is_postfix = true; }
                }
            }
        }
        if (is_postfix) {
            const OpDef *pf = MATCH_POSTFIX(c);
            size_t op_end = c->pos;
            while (nops > 0 && (pf->prec > ops[nops - 1].op->prec || (pf->prec == ops[nops - 1].op->prec && pf->left))) POP_OP(ctx_prev);
            stack[nst - 1] = mk_op(c, pf, ctx_op, op_end, NULL, stack[nst - 1]);
            break;
        } else {
            Ctx p = *c;
            const OpDef *inf = NULL;
            if (!term_match(&p, term) && !reg_synonym_ahead(c)) {
                Ctx p2 = *c;
                inf = MATCH_INFIX(&p2);
                if (inf) *c = p2;
            }
            if (!inf) { c->pos = ctx_prev; break; }
            size_t op_end = c->pos;
            Node *rhs = p_expression_literal_rec(c, term);
            if (!rhs) {
                skip_ws(c);
                rep2(PRI_CRITICAL, "invalid-expression", c->src, c->pos, c->pos, "Could not parse an expression here",
                     c->src, ctx_op, op_end, afmt("...as expected after operator '%s'.%s", inf->ch, strcmp(inf->ch, ">>") == 0 ? " If this was intended as two closing parentheses rather than right shift, please add spaces: '> >'." : ""));
            }
            while (nops > 0 && (inf->prec > ops[nops - 1].op->prec || (inf->prec == ops[nops - 1].op->prec && inf->left))) POP_OP(ctx_prev);
            PUSHST(rhs);
            PUSHOP(inf, 0);
        }
    }
    while (nops > 0) POP_OP(c->pos);
    Node *res = stack[nst - 1];
    free(ops); free(stack);
    return res;
}


/* ---- instructions ---- */
static OpType operand_type_for(Ctx *c, const char *name, size_t nl, int idx) {
    const Builtin *b = builtin_find(name, nl);
    if (!b && nl < 90) {
        char tmp[100]; tmp[0] = '.'; memcpy(tmp + 1, name, nl);
        b = builtin_find(tmp, nl + 1);
    }
    bool is_meta = name[0] == '.' || (b && b->meta);
    if (is_meta) {
        if (b && b->meta && b->meta->nopinfo) {
            int k = idx < b->meta->nopinfo ? idx : b->meta->nopinfo - 1;
            return b->meta->info[k].type;
        }
        Ctx p = *c; skip_ws(&p);
        int q = ascii_at(&p, 0);
        if (q == '\'' || q == '"' || q == '/') return OT_STR;
        return OT_INT;
    }
    return OT_INT;
}

static Node *parse_insn_operand(Ctx *c, const char *name, size_t nl, int idx, bool critical, size_t bc_s, size_t bc_e, size_t is_, size_t ie_) {
    OpType t = operand_type_for(c, name, nl, idx);
    if (t == OT_BLOCK) t = OT_INT; /* AssertionError in Python; not expected */
    Node *r = (t == OT_STR) ? p_long_string(c) : p_expression(c, &NEVER, false);
    if (!r && critical) {
        size_t here = c->pos;
        RepItem it[3] = {
            {c->src, bc_s, bc_e, "Expected operand after comma in an instruction"},
            {c->src, is_, ie_, "(instruction started here)"},
            {c->src, here, here, "This definitely does not look like an operand"}};
        report_emit(PRI_CRITICAL, "invalid-operand", 3, it);
    }
    return r;
}

static Node *p_instruction(Ctx *c) {
    Ctx st = *c;
    size_t cs = c->pos;
    size_t ns, ne;
    if (!re_insn_name(c, &ns, &ne)) { *c = st; return NULL; }
    const char *name = sub_(c, ns, ne);
    size_t nl = ne - ns;
    if (is_register_name(name, nl)) {
        WARN1("suspicious-name", c->src, cs, c->pos, "Instruction name suspiciously resembles a register.\nCheck for an excess newline or a missing comma before the register name");
    }
    size_t after_name = c->pos;
    Node *name_sym = mk_symbol(c, cs, after_name, name, false);
    const Builtin *bi = builtin_find(name, nl);

    if (bi) {
        Ctx p = *c;
        if (lit(&p, ",")) {
            skip_ws(c);
            size_t before_comma = c->pos;
            lit(c, ",");
            rep2(PRI_CRITICAL, "invalid-insn", c->src, before_comma, c->pos,
                 afmt("Unexpected comma right after instruction name; expected an operand. This is not\nparsed as implicit '.word' because '%s' is a real instruction.", name),
                 c->src, cs, after_name, "Instruction started here");
        }
    } else if (name[0] != '.') {
        Ctx p = *c;
        bool m = false;
        if (lit(&p, ",")) m = true;
        else {
            Ctx q = *c;
            if (!MATCH_PREFIX(&q)) {
                Ctx q2 = *c; char cb[3];
                if (!caret_paren(&q2, cb)) {
                    Ctx q3 = *c;
                    if (MATCH_INFIX(&q3)) m = true;
                }
            }
        }
        if (m) { *c = st; return NULL; }
    }

    if (bi && bi->meta && bi->meta->literal_string) {
        const char *nl_ = memchr(CODE(c) + c->pos, '\n', LEN(c) - c->pos);
        size_t idx = nl_ ? (size_t)(nl_ - CODE(c)) : LEN(c);
        /* text = code[pos:idx].strip() */
        size_t a = c->pos, b = idx;
        while (a < b) { size_t q = a; uint32_t cp = u8_next(CODE(c), LEN(c), &q); if (py_isspace(cp)) a = q; else break; }
        while (b > a) {
            size_t q = b - 1; while (q > a && ((unsigned char)CODE(c)[q] & 0xC0) == 0x80) q--;
            size_t q2 = q; uint32_t cp = u8_next(CODE(c), LEN(c), &q2);
            if (py_isspace(cp)) b = q; else break;
        }
        PVec ops = {0};
        if (b > a) {
            size_t nchars = u8_count(CODE(c), a, b);
            skip_ws(c);
            size_t before_msg = c->pos;
            for (size_t k = 0; k < nchars && c->pos < LEN(c); k++) c->pos += u8_len_at(CODE(c), c->pos);
            Node *q = mk(c, N_QSTR, before_msg, c->pos);
            q->u.qstr.quote = "";
            U32 tmp = {0}; size_t cap = 0;
            for (size_t i = a; i < b;) u32_push(&tmp, &cap, u8_next(CODE(c), LEN(c), &i));
            q->u.qstr.str = u32_arena(&tmp); free(tmp.cp);
            pvec_push(&ops, q);
        }
        Node *n = mk(c, N_INSN, cs, c->pos);
        n->u.insn.name = name_sym; n->u.insn.ops = ops;
        return n;
    }

    {
        Ctx p = *c;
        if (lit(&p, "}")) {
            Node *n = mk(c, N_INSN, cs, c->pos);
            n->u.insn.name = name_sym;
            return n;
        }
    }

    if (newline_ahead(c)) {
        if (bi && bi->min_ops > 0) {
            /* 'unexpected-newline' is not in any default warning class */
            rep1(PRI_WARNING, "unexpected-newline", c->src, cs, after_name,
                 "This instruction always takes at least one argument, but there is a newline right after its name.\nPDPy11 will attempt to parse the next line as an operand, but many other compilers (including Macro-11) wouldn't, and this can potentially even cause miscompilations.\nPlease put the operand to the same line.");
        } else {
            Node *n = mk(c, N_INSN, cs, c->pos);
            n->u.insn.name = name_sym;
            return n;
        }
    }

    {
        Ctx p = *c;
        size_t a, b;
        if (re_insn_name(&p, &a, &b) && !lit(&p, ":") ) {
            const char *nn = sub_(c, a, b);
            if (bi && bi->max_ops == 0 && builtin_find(nn, b - a)) {
                Ctx bi_ctx = *c;
                skip_ws(&bi_ctx);
                size_t na, nb;
                re_insn_name(&bi_ctx, &na, &nb);
                Ctx look = bi_ctx;
                bool follows = lit(&look, ",") || MATCH_INFIX(&look) || MATCH_POSTFIX(&look);
                if (!follows) {
                    rep2(PRI_WARNING, "missing-newline", c->src, cs, after_name,
                         "There is no newline after the name of this instruction, hence an operand is naturally expected to follow,",
                         c->src, bi_ctx.pos, bi_ctx.pos,
                         afmt("but it suspiciously resembles another instruction.\nYou probably \x1b[3mmeant\x1b[23m an instruction '%s' without operands followed by a '%s' instruction,\nand pdpy will compile this code as such, but this is against standards; please add a newline between instructions.", name, nn));
                    Node *n = mk(c, N_INSN, cs, c->pos);
                    n->u.insn.name = name_sym;
                    return n;
                }
            }
        }
    }

    PVec operands = {0};
    Node *first = NULL;
    { Ctx sv = *c; first = parse_insn_operand(c, name, nl, 0, false, 0, 0, 0, 0); if (!first) *c = sv; }
    if (first) {
        if (after_name < LEN(c)) {
            size_t q = after_name; uint32_t cp = u8_next(CODE(c), LEN(c), &q);
            if (!py_isspace(cp)) {
                ERR1("missing-whitespace", c->src, after_name, c->pos, "Expected whitespace after instruction name. This is ambiguous: Macro-11 would\ntreat this character as a separator and ignore it, while a sane assembler would\nassume it is part of the first operand. Proceeding under the latter assumption.");
            }
        }
        pvec_push(&operands, first);
        size_t before_comma = c->pos;
        while (lit(c, ",")) {
            size_t after_comma = c->pos;
            skip_ws(c);
            Node *o = parse_insn_operand(c, name, nl, (int)operands.n, true, before_comma, after_comma, cs, after_name);
            pvec_push(&operands, o);
            before_comma = c->pos;
        }
        if (lit(c, "{")) {
            Node *blk = p_code(c, true, c->pos - 1);
            pvec_push(&operands, blk);
        }
        if (c->pos < LEN(c)) {
            size_t adv; uint32_t cp = cp_at(c, &adv);
            if (!py_isspace(cp) && !comment_at(c, c->pos, NULL)) {
                ERR1("missing-whitespace", c->src, c->pos, c->pos, "Expected whitespace after instruction. Proceeding as if a new instruction is starting");
            }
        }
    } else {
        if (after_name < LEN(c)) {
            skip_ws(c);
            rep2(PRI_WARNING, "missing-newline", c->src, c->pos, c->pos,
                 "Could not parse an operand starting from here; assuming a new instruction.\nPlease add a newline here if an instruction was implied.",
                 c->src, cs, after_name, "The previous instruction started here");
        }
    }
    Node *n = mk(c, N_INSN, cs, c->pos);
    n->u.insn.name = name_sym;
    n->u.insn.ops = operands;
    return n;
}

static Node *p_word_list(Ctx *c) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    Node *first = p_expression(c, &NEVER, false);
    if (!first) { *c = st; return NULL; }
    PVec words = {0};
    pvec_push(&words, first);
    size_t after_first = c->pos;
    for (;;) {
        size_t before_comma = c->pos;
        if (!lit(c, ",")) break;
        size_t after_comma = c->pos;
        skip_ws(c);
        Node *w = p_expression(c, &NEVER, false);
        if (!w) {
            RepItem it[3] = {
                {c->src, before_comma, after_comma, "Expected word after comma in a word list"},
                {c->src, cs, after_first, "(list started here)"},
                {c->src, c->pos, c->pos, "This does not look like a word"}};
            report_emit(PRI_CRITICAL, "invalid-operand", 3, it);
        }
        pvec_push(&words, w);
    }
    if (c->pos < LEN(c)) {
        size_t adv; uint32_t cp = cp_at(c, &adv);
        if (!py_isspace(cp) && !comment_at(c, c->pos, NULL)) {
            ERR1("missing-whitespace", c->src, c->pos, c->pos, "Expected whitespace after word list. Proceeding as if a new instruction is starting");
            goto done;
        }
    }
    {
        Ctx p = *c;
        bool nl = newline_ahead(&p) || at_eof(&p);
        if (!nl) {
            rep2(PRI_WARNING, "missing-newline", c->src, c->pos, c->pos,
                 "Expected newline after a word list; assuming a new instruction. Please add a\nnewline here if an instruction was implied, and a comma if it's the continuation\nof a word list.",
                 c->src, cs, c->pos, "The word list started here");
        }
    }
done:;
    Node *n = mk(c, N_WORDLIST, cs, c->pos);
    n->u.wl.words = words;
    return n;
}

/* hidden compatibility statement "%n = name": gives register n one more name (consumed here, no AST node) */
static bool p_register_synonym(Ctx *c) {
    Ctx st = *c;
    skip_ws(c);
    if (ascii_at(c, 0) != '%') {
        /* the same, written the other way round: "name = %n" */
        size_t a, b;
        if (!re_symbol(c, &a, &b)) { *c = st; return false; }
        skip_ws(c);
        if (ascii_at(c, 0) != '=' || ascii_at(c, 1) == '=') { *c = st; return false; }
        c->pos++;
        skip_ws(c);
        int r = ascii_at(c, 1);
        if (ascii_at(c, 0) != '%' || r < '0' || r > '7' || is_wordc(ascii_at(c, 2))) { *c = st; return false; }
        c->pos += 2;
        register_alias_add(CODE(c) + a, b - a, r - '0');
        return true;
    }
    int d = ascii_at(c, 1);
    if (d < '0' || d > '7' || is_wordc(ascii_at(c, 2))) { *c = st; return false; }
    c->pos += 2;
    skip_ws(c);
    if (ascii_at(c, 0) != '=' || ascii_at(c, 1) == '=') { *c = st; return false; }
    c->pos++;
    size_t a, b;
    if (!re_symbol(c, &a, &b)) { *c = st; return false; }
    register_alias_add(CODE(c) + a, b - a, d - '0');
    return true;
}

/* ---- macros: definition, call and early parsing of included files (so that macros defined there are known) ---- */
extern char *u32_to_utf8(const U32 *s);
extern char *resolve_relative_path(const char *rel, const char *base_file);
static const char *g_real_path = "";                 /* path of the file being parsed (not a macro expansion) */
static SMap g_parsed_inc;                            /* resolved path -> FileAst (parsed ahead of the compile pass) */
static FileAst g_inc_busy;

FileAst *parsed_include_lookup(const char *path) {
    FileAst *f = (FileAst *)smap_get(&g_parsed_inc, path, strlen(path));
    return f == &g_inc_busy ? NULL : f;
}
static FileAst g_inc_failed;                          /* parsed ahead, fatal syntax error already reported */
bool parsed_include_failed(const char *path) { return smap_get(&g_parsed_inc, path, strlen(path)) == &g_inc_failed; }

/* ".macro"/"macro" (a whole word followed by blank, newline or end) at the cursor */
static size_t macro_word_len(const Ctx *c, const char *w) {
    size_t n = strlen(w);
    if (c->pos + n > LEN(c)) return 0;
    for (size_t i = 0; i < n; i++) if (lc((unsigned char)CODE(c)[c->pos + i]) != w[i]) return 0;
    int nx = c->pos + n < LEN(c) ? (unsigned char)CODE(c)[c->pos + n] : '\n';
    return (nx == ' ' || nx == '\t' || nx == '\r' || nx == '\n') ? n : 0;
}

static bool p_macro_def(Ctx *c) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    size_t n = macro_word_len(c, ".macro");
    if (!n) n = macro_word_len(c, "macro");
    if (!n) { *c = st; return false; }
    const char *err; size_t ep;
    size_t end = macro_define(CODE(c), LEN(c), cs + n, &err, &ep);
    if (end == SIZE_MAX) CRIT1("invalid-macro", c->src, ep, ep + 1, "%s", err);
    c->pos = end;
    return true;
}

static bool p_macro_call(Ctx *c, PVec *insns) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos, a = c->pos, b = a;
    while (b < LEN(c) && (is_wordc((unsigned char)CODE(c)[b]) || CODE(c)[b] == '.' || CODE(c)[b] == '$')) b++;
    if (b == a) { *c = st; return false; }
    c->pos = b;
    const Macro *m = macro_lookup(CODE(c) + a, b - a);
    int nx = ascii_at(c, 0);
    if (!m || !(nx == -1 || nx == ' ' || nx == '\t' || nx == '\r' || nx == '\n' || nx == ';' || nx == '/')) { *c = st; return false; }
    PVec args = {0};
    size_t end = macro_collect_args(CODE(c), LEN(c), c->pos, &args);
    c->pos = end;
    if (args.n > macro_ndummies(m))
        ERR1("macro-arguments", c->src, cs, end, "Macro '%s' takes %u argument(s), but %u were given", macro_name(m), (unsigned)macro_ndummies(m), (unsigned)args.n);
    if (g_macro_depth >= 64)
        CRIT1("macro-recursion", c->src, cs, b, "Macro '%s' is expanded too deeply (is it recursive?)", macro_name(m));
    size_t ol;
    char *txt = macro_expand(m, &args, &ol);
    free(args.a);
    size_t line, col;
    src_linecol(c->src, cs, &line, &col, NULL);
    Src *s = (Src *)amalloc(sizeof(Src));
    s->path = afmt("%s [macro '%s' called at line %u]", c->src->path, macro_name(m), (unsigned)(line + 1));
    s->encoding = "utf-8";
    s->text = txt;
    s->len = ol;
    Ctx sub = {s, 0};
    g_macro_depth++;
    Node *blk = p_code(&sub, false, 0);
    g_macro_depth--;
    for (size_t i = 0; i < blk->u.block.insns.n; i++) pvec_push(insns, blk->u.block.insns.a[i]);
    return true;
}

/* ".include "file"": parse the file right away, so that macros it defines are available to the code that follows */
static void prescan_include(const Node *in) {
    if (in->kind != N_INSN || in->u.insn.name->kind != N_SYMBOL || in->u.insn.ops.n != 1) return;
    const char *nm = in->u.insn.name->u.sym.lname;
    if (strcmp(nm, "include") != 0 && strcmp(nm, ".include") != 0) return;
    const Node *q = (const Node *)in->u.insn.ops.a[0];
    if (q->kind != N_QSTR) return;
    char *path = resolve_relative_path(u32_to_utf8(&q->u.qstr.str), g_real_path);
    if (smap_get(&g_parsed_inc, path, strlen(path))) return;
    int kind = 0; char *err = NULL;
    Src *src = src_load(path, astrdup(path), &kind, &err);
    if (!src) return;                                    /* the compile pass reports it */
    *smap_slot(&g_parsed_inc, path, strlen(path), NULL) = &g_inc_busy;
    jmp_buf saved; memcpy(&saved, &g_unrecoverable, sizeof saved);
    bool armed = g_unrec_armed, catch_ = g_layout_catch;
    int depth = g_macro_depth, silent = g_silent;
    const char *rp = g_real_path;
    g_unrec_armed = true; g_layout_catch = false; g_silent = 0;
    FileAst *f = NULL;
    if (setjmp(g_unrecoverable) == 0) {
        warn_source_encoding(src);
        f = parse_file(src);
    }
    memcpy(&g_unrecoverable, &saved, sizeof saved);
    g_unrec_armed = armed; g_layout_catch = catch_; g_macro_depth = depth; g_silent = silent; g_real_path = rp;
    *smap_slot(&g_parsed_inc, path, strlen(path), NULL) = f ? f : &g_inc_failed;
}

/* ---- conditional assembly (.if/.iff/.ift/.iftf/.endc, .iif), .ntype, .mcall, .irp/.irpc/.rept, empty .error ---- */

typedef struct { bool cond; Src *src; size_t pos; } CondFrame;
static CondFrame g_cframes[128];
static int g_ncframes;

/* parse the expanded text of a macro or repeat block in place of the statement and splice the nodes */
static void splice_text(Ctx *c, PVec *insns, char *txt, size_t len, const char *what, size_t cs) {
    if (g_macro_depth >= 64)
        CRIT1("macro-recursion", c->src, cs, cs + 1, "'%s' is expanded too deeply (is it recursive?)", what);
    size_t line, col;
    src_linecol(c->src, cs, &line, &col, NULL);
    Src *s = (Src *)amalloc(sizeof(Src));
    s->path = afmt("%s [%s called at line %u]", c->src->path, what, (unsigned)(line + 1));
    s->encoding = "utf-8";
    s->text = txt;
    s->len = len;
    Ctx sub = {s, 0};
    g_macro_depth++;
    Node *blk = p_code(&sub, false, 0);
    g_macro_depth--;
    for (size_t i = 0; i < blk->u.block.insns.n; i++) pvec_push(insns, blk->u.block.insns.a[i]);
}

static size_t line_end(const Ctx *c, size_t p) {
    while (p < LEN(c) && CODE(c)[p] != '\n') p++;
    return p;
}

/* from the end of the current line, find the next .iff/.ift/.iftf/.endc of this nesting level; the cursor is left at its line */
static void skip_branch(Ctx *c) {
    size_t p = line_end(c, c->pos);
    if (p < LEN(c)) p++;
    int depth = 0;
    while (p < LEN(c)) {
        size_t le = line_end(c, p);
        char w[16];
        if (macro_line_word(CODE(c), p, le, w, sizeof w)) {
            if (!strcmp(w, ".if")) depth++;
            else if (!strcmp(w, ".endc")) { if (depth == 0) { c->pos = p; return; } depth--; }
            else if (depth == 0 && (!strcmp(w, ".iff") || !strcmp(w, ".ift") || !strcmp(w, ".iftf"))) { c->pos = p; return; }
        }
        p = le < LEN(c) ? le + 1 : le;
    }
    c->pos = LEN(c);
}

/* text of the lines after the current one up to the matching .endr; *after is the position after the .endr line */
static bool take_repeat_body(Ctx *c, size_t *bs, size_t *be, size_t *after) {
    size_t p = line_end(c, c->pos);
    if (p < LEN(c)) p++;
    *bs = p;
    int depth = 0;
    while (p < LEN(c)) {
        size_t le = line_end(c, p);
        char w[16];
        if (macro_line_word(CODE(c), p, le, w, sizeof w)) {
            if (!strcmp(w, ".irp") || !strcmp(w, ".irpc") || !strcmp(w, ".rept")) depth++;
            else if (!strcmp(w, ".endr")) {
                if (depth == 0) { *be = p; *after = le; return true; }
                depth--;
            }
        }
        p = le < LEN(c) ? le + 1 : le;
    }
    return false;
}

static void cond_error(Ctx *c, const char *id, size_t s, size_t e, const char *msg) {
    CRIT1(id, c->src, s, e, "%s", msg);
}

static bool p_conditional(Ctx *c, PVec *insns) {
    Ctx st = *c;
    skip_ws(c);
    size_t ws = c->pos, we = ws;
    while (we < LEN(c) && (is_wordc((unsigned char)CODE(c)[we]) || CODE(c)[we] == '.')) we++;
    if (we == ws || CODE(c)[ws] != '.' || we - ws > 6) { *c = st; return false; }
    char w[8];
    lower_ascii_n(w, CODE(c) + ws, we - ws); w[we - ws] = 0;
    int nx = we < LEN(c) ? (unsigned char)CODE(c)[we] : '\n';
    if (!(nx == ' ' || nx == '\t' || nx == '\r' || nx == '\n' || nx == ';' || nx == '/' || nx == '<')) { *c = st; return false; }
    const char *T = CODE(c);
    size_t L = LEN(c);

    if (!strcmp(w, ".if") || !strcmp(w, ".iif")) {
        bool iif = w[2] == 'i', r = false;
        const char *err;
        size_t end = cond_test(T, L, we, &r, &err);
        if (end == SIZE_MAX) cond_error(c, "invalid-condition", ws, we, err);
        if (iif) {
            size_t q = end;
            while (q < L && (T[q] == ' ' || T[q] == '\t' || T[q] == '\r')) q++;
            if (q < L && T[q] == ',') q++;
            if (r) { c->pos = q; return true; }       /* the statement after the condition is parsed normally */
            c->pos = line_end(c, q);
            return true;
        }
        if (g_ncframes >= 128) cond_error(c, "invalid-condition", ws, we, "Conditional blocks are nested too deeply");
        g_cframes[g_ncframes++] = (CondFrame){r, c->src, ws};
        c->pos = end;
        if (!r) skip_branch(c);
        return true;
    }
    if (!strcmp(w, ".iff") || !strcmp(w, ".ift") || !strcmp(w, ".iftf") || !strcmp(w, ".endc")) {
        if (g_ncframes == 0 || g_cframes[g_ncframes - 1].src != c->src)
            cond_error(c, "stray-conditional", ws, we, "This directive has no matching '.if'");
        c->pos = we;
        if (w[1] == 'e') { g_ncframes--; return true; }
        bool cond = g_cframes[g_ncframes - 1].cond;
        bool active = w[3] == 'f' && w[4] == 0 ? !cond : w[3] == 't' && w[4] == 0 ? cond : true;
        if (!active) skip_branch(c);
        return true;
    }
    if (!strcmp(w, ".mcall")) { c->pos = line_end(c, we); return true; }
    if (!strcmp(w, ".radix")) {                       /* MACRO-11: the argument is always decimal; no argument means 8 */
        size_t p = we;
        while (p < L && (T[p] == ' ' || T[p] == '\t')) p++;
        size_t as = p;
        while (p < L && is_digit_((unsigned char)T[p])) p++;
        size_t ae = p;
        if (p < L && T[p] == '.') p++;                 /* "10." is accepted too */
        int r = 8;
        if (ae > as) {
            long v = 0;
            for (size_t k = as; k < ae && v < 1000; k++) v = v * 10 + (T[k] - '0');
            r = (int)v;
        } else if (p < L && T[p] != '\n' && T[p] != ';' && T[p] != '\r' && T[p] != '/') {
            ERR1("invalid-radix", c->src, ws, p + 1, "'.radix' expects a number: 2, 8, 10 or 16");
            c->pos = line_end(c, p);
            return true;
        }
        if (r != 2 && r != 8 && r != 10 && r != 16) {
            ERR1("invalid-radix", c->src, as, ae, "The radix must be 2, 8, 10 or 16 (the number is read as a decimal one)");
            c->pos = ae > as ? p : line_end(c, p);
            return true;
        }
        cond_set_radix(r);
        c->pos = p;
        return true;
    }
    if (!strcmp(w, ".ntype")) {
        size_t p = we;
        while (p < L && (T[p] == ' ' || T[p] == '\t')) p++;
        size_t ns = p;
        while (p < L && (is_wordc((unsigned char)T[p]) || T[p] == '.' || T[p] == '$')) p++;
        size_t ne = p;
        while (p < L && (T[p] == ' ' || T[p] == '\t')) p++;
        if (ne == ns || p >= L || T[p] != ',') cond_error(c, "invalid-ntype", ws, we, "'.ntype' expects 'symbol, operand'");
        p++;
        size_t os = p, oe = p;
        while (oe < L && T[oe] != '\n' && T[oe] != ';') oe++;
        cond_set(T + ns, ne - ns, cond_ntype(T + os, oe - os));
        c->pos = oe;
        return true;
    }
    if (!strcmp(w, ".error")) {                       /* MACRO-11 style: the text is the comment ".error ;text;" */
        size_t p = we;
        while (p < L && (T[p] == ' ' || T[p] == '\t' || T[p] == '\r')) p++;
        if (p < L && T[p] != ';' && T[p] != '\n') { *c = st; return false; }     /* has an operand: the normal directive */
        size_t e = line_end(c, p);
        size_t a = p < e ? p + 1 : p, b = e;
        while (b > a && (T[b - 1] == ' ' || T[b - 1] == '\t' || T[b - 1] == '\r')) b--;
        if (b > a && T[b - 1] == ';') b--;
        ERR1("macro-error", c->src, ws, we, "%.*s", (int)(b - a), T + a);
        c->pos = e;
        return true;
    }
    if (!strcmp(w, ".irp") || !strcmp(w, ".irpc") || !strcmp(w, ".rept")) {
        int kind = w[1] == 'r' && w[2] == 'e' ? 2 : w[4] == 'c' ? 1 : 0;
        size_t bs, be, after;
        Ctx hdr = *c;
        hdr.pos = we;
        if (!take_repeat_body(&hdr, &bs, &be, &after))
            cond_error(c, "unclosed-repeat", ws, we, "This repeat block is never closed: a matching '.endr' is expected");
        Buf out = {0};
        if (kind == 2) {
            long n; const char *err;
            size_t e = line_end(c, we);
            size_t ce = we; while (ce < e && T[ce] != ';') ce++;
            if (!cond_eval(T + we, ce - we, &n, &err)) cond_error(c, "invalid-repeat", ws, we, err);
            if (n < 0 || n > 65536 || (size_t)n * (be - bs) > (16u << 20)) cond_error(c, "invalid-repeat", ws, we, "The repeat count is too large");
            for (long i = 0; i < n; i++) buf_putn(&out, T + bs, be - bs);
        } else {
            size_t p = we;
            while (p < L && (T[p] == ' ' || T[p] == '\t')) p++;
            size_t ns = p;
            while (p < L && (is_wordc((unsigned char)T[p]) || T[p] == '$')) p++;
            size_t ne = p;
            while (p < L && (T[p] == ' ' || T[p] == '\t')) p++;
            if (ne == ns || p >= L || T[p] != ',') cond_error(c, "invalid-repeat", ws, we, "'.irp'/'.irpc' expect 'symbol, <list>'");
            p++;
            PVec args = {0};
            while (p < L && (T[p] == ' ' || T[p] == '\t')) p++;
            size_t e = line_end(c, p);
            if (p < e && T[p] == '<') {
                size_t q = p; int depth = 0;
                for (; q < e; q++) { if (T[q] == '<') depth++; else if (T[q] == '>' && --depth == 0) break; }
                if (q >= e) cond_error(c, "invalid-repeat", ws, we, "The list is missing its closing '>'");
                if (kind == 0) macro_collect_args(T + p + 1, q - p - 1, 0, &args);
                else for (size_t k = p + 1; k < q; k++) pvec_push(&args, astrndup(T + k, 1));
            } else if (kind == 0) macro_collect_args(T, e, p, &args);
            else { size_t q = p; while (q < e && T[q] != ';' && T[q] != ' ' && T[q] != '\t') q++; for (size_t k = p; k < q; k++) pvec_push(&args, astrndup(T + k, 1)); }
            Buf def = {0};
            buf_puts(&def, "__irp ");
            buf_putn(&def, T + ns, ne - ns);
            buf_putc(&def, '\n');
            buf_putn(&def, T + bs, be - bs);
            buf_puts(&def, ".endm\n");
            const char *err; size_t ep;
            if (macro_define(def.p, def.len, 0, &err, &ep) == SIZE_MAX) cond_error(c, "invalid-repeat", ws, we, err);
            buf_free(&def);
            const Macro *m = macro_lookup("__irp", 5);
            for (size_t i = 0; i < args.n; i++) {
                PVec one = {0};
                pvec_push(&one, args.a[i]);
                size_t xl; char *x = macro_expand(m, &one, &xl);
                free(one.a);
                buf_putn(&out, x, xl);
            }
            free(args.a);
        }
        c->pos = after;
        char *txt = astrndup(out.p, out.len);
        size_t tl = out.len;
        buf_free(&out);
        splice_text(c, insns, txt, tl, "repeat block", ws);
        return true;
    }
    *c = st;
    return false;
}

/* remember "name = constant" so that later conditions can use it */
static void remember_assignment(Ctx *c, const Node *a) {
    if (a->kind != N_ASSIGN || a->u.assign.target->kind != N_SYMBOL) return;
    const Node *t = a->u.assign.target, *v = a->u.assign.value;
    long x; const char *err;
    if (t->e > LEN(c) || t->s >= t->e) return;
    const char *nm = CODE(c) + t->s;                       /* the name as written (the node may carry an internal alias) */
    size_t nl = t->e - t->s;
    while (nl && (nm[nl - 1] == ' ' || nm[nl - 1] == '\t')) nl--;
    if (v->s < v->e && v->e <= LEN(c) && cond_eval(CODE(c) + v->s, v->e - v->s, &x, &err)) cond_set(nm, nl, x);
    else cond_forget(nm, nl);
}

/* ---- hidden compatibility with the pseudo-commands of Micro-11: .#E .@E .B:n .A:text! .R:abc,def .E ----
   they are rewritten into ordinary directives and parsed from that text */
static bool micro_start(const Ctx *c, size_t p) {
    if (p + 1 >= LEN(c) || CODE(c)[p] != '.') return false;
    int a = (unsigned char)CODE(c)[p + 1];
    if (a == '#' || a == '@') return true;
    int up = toupper(a);
    if ((up == 'B' || up == 'A' || up == 'R') && p + 2 < LEN(c) && CODE(c)[p + 2] == ':') return true;
    if (up == 'E') {
        int nx = p + 2 < LEN(c) ? (unsigned char)CODE(c)[p + 2] : '\n';
        return !(isalnum(nx) || nx == '_' || nx == '$' || nx == '.');
    }
    return false;
}

/* expression of a pseudo-command: blanks, then a run of name/number characters, '+' and '-' up to a blank or the next pseudo-command */
static size_t micro_expr(const Ctx *c, size_t p, size_t *start) {
    while (p < LEN(c) && (CODE(c)[p] == ' ' || CODE(c)[p] == '\t')) p++;
    *start = p;
    while (p < LEN(c)) {
        int ch = (unsigned char)CODE(c)[p];
        if (ch == '.') { if (micro_start(c, p)) break; p++; continue; }
        if (isalnum(ch) || ch == '$' || ch == '_' || ch == '+' || ch == '-' || ch >= 0x80) { p++; continue; }
        break;
    }
    return p;
}

static bool p_micro11(Ctx *c, PVec *insns) {
    Ctx st = *c;
    skip_ws(c);
    size_t cs = c->pos;
    if (!micro_start(c, c->pos)) { *c = st; return false; }
    Buf out = {0};
    size_t p = c->pos;
    while (p < LEN(c) && micro_start(c, p)) {
        int kind = toupper((unsigned char)CODE(c)[p + 1]);
        size_t a, e;
        if (kind == '#' || kind == '@') {
            e = micro_expr(c, p + 2, &a);
            if (e == a) { *c = st; buf_free(&out); return false; }
            if (kind == '#') buf_puts(&out, ".word "); else buf_puts(&out, ".word -(.)+(");
            if (kind == '#') buf_putn(&out, CODE(c) + a, e - a);
            else { buf_putn(&out, CODE(c) + a, e - a); buf_putc(&out, ')'); }
            buf_putc(&out, '\n');
            p = e;
        } else if (kind == 'B') {
            e = micro_expr(c, p + 3, &a);
            if (e == a) { *c = st; buf_free(&out); return false; }
            buf_puts(&out, ".byte "); buf_putn(&out, CODE(c) + a, e - a); buf_putc(&out, '\n');
            p = e;
        } else if (kind == 'A') {
            a = p + 3;
            while (a < LEN(c) && (CODE(c)[a] == ' ' || CODE(c)[a] == '\t')) a++;
            e = a;
            while (e < LEN(c) && CODE(c)[e] != '!' && CODE(c)[e] != '\n' && CODE(c)[e] != '\r') e++;
            if (e > a) {
                buf_puts(&out, ".ascii \"");
                for (size_t i = a; i < e; i++) { if (CODE(c)[i] == '"' || CODE(c)[i] == '\\') buf_putc(&out, '\\'); buf_putc(&out, CODE(c)[i]); }
                buf_puts(&out, "\"\n");
            }
            p = e < LEN(c) && CODE(c)[e] == '!' ? e + 1 : e;
        } else if (kind == 'R') {
            a = p + 3;
            e = a;
            while (e < LEN(c) && CODE(c)[e] != '\n' && CODE(c)[e] != '\r' && CODE(c)[e] != ';') e++;
            size_t g = a;
            while (g <= e) {
                size_t q = g;
                while (q < e && CODE(c)[q] != ',') q++;
                if (q > g || q < e) { buf_puts(&out, ".rad50 \""); buf_putn(&out, CODE(c) + g, q - g); buf_puts(&out, "\"\n"); }
                g = q + 1;
            }
            p = e;
        } else {                                  /* .E : zero the current byte if the address is odd, otherwise the word */
            buf_puts(&out, ".blkb 2 - (. & 1)\n");
            p = p + 2;
        }
        while (p < LEN(c) && (CODE(c)[p] == ' ' || CODE(c)[p] == '\t')) p++;
    }
    c->pos = p;
    char *txt = (char *)amalloc(out.len + 1);
    memcpy(txt, out.p ? out.p : "", out.len); txt[out.len] = 0;
    splice_text(c, insns, txt, out.len, "Micro-11 pseudo-command", cs);
    buf_free(&out);
    return true;
}

static Node *p_code(Ctx *c, bool brk, size_t open_pos) {
    size_t cs = c->pos;
    PVec insns = {0};
    bool closed = false;
    while (!at_eof(c)) {
        skip_ws(c);
        cs = c->pos;
        if (brk && lit(c, "}")) { closed = true; break; }
        if (p_register_synonym(c)) continue;
        if (p_micro11(c, &insns)) continue;
        if (p_conditional(c, &insns)) continue;
        if (p_macro_def(c)) continue;
        Node *in = p_label(c);
        if (!in) in = p_assignment(c);
        if (!in && p_macro_call(c, &insns)) continue;
        if (!in) in = p_instruction(c);
        if (!in) in = p_word_list(c);
        if (!in) {
            if (ascii_at(c, 0) == '}')
                CRIT1("invalid-insn", c->src, cs, cs + 1, "Unexpected '}': there is no matching '{' (code blocks are only used by directives like '.repeat N {')");
            CRIT1("invalid-insn", c->src, cs, cs, "Could not parse instruction starting from here");
        }
        pvec_push(&insns, in);
        remember_assignment(c, in);
        prescan_include(in);
        if (!brk && in->kind == N_INSN) {
            const char *nm = in->u.insn.name->u.sym.name;
            if (streq_ci(nm, "end") || streq_ci(nm, ".end")) break;
        }
    }
    while (!brk && g_ncframes && g_cframes[g_ncframes - 1].src == c->src) {
        ERR1("unclosed-conditional", c->src, g_cframes[g_ncframes - 1].pos, g_cframes[g_ncframes - 1].pos + 3, "This '.if' is never closed: a matching '.endc' is expected before the end of the text");
        g_ncframes--;
    }
    if (brk && !closed) {
        ERR1("unclosed-block", c->src, open_pos, open_pos + 1, "This '{' is never closed: a matching '}' is expected before the end of the file");
    }
    Node *n = mk(c, N_BLOCK, cs, c->pos);
    n->u.block.insns = insns;
    return n;
}

FileAst *parse_file(Src *src) {
    macro_init_builtin();
    g_real_path = src->path;
    Ctx c = {src, 0};
    FileAst *f = (FileAst *)amalloc(sizeof(FileAst));
    f->src = src;
    f->body = p_code(&c, false, 0);
    return f;
}
