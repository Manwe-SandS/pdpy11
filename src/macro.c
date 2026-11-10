/* Text macros: definition, argument collection and expansion (see macro.h). */
#include "macro.h"
#include "text.h"

typedef struct { const char *name; const char *def; bool auto_label; } Dummy;   /* name is lower case */
struct Macro { const char *name; size_t nd; Dummy *d; const char *body; size_t bl; bool builtin; };

static SMap g_macros;
static bool g_defining_builtin;          /* set while the built-in macros are being defined */
static unsigned g_auto_label = 30000;    /* MACRO-11: automatic local labels 30000$ ... 65535$ */

static bool is_wc(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '.'; }
static bool is_alpha_(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$'; }
static bool is_blank(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

/* length of a comment starting at text[p] (semicolon or double slash to the end of the line,
   or a slash-star block), 0 if none */
static size_t comment_len(const char *t, size_t len, size_t p) {
    if (p >= len) return 0;
    if (t[p] == ';' || (t[p] == '/' && p + 1 < len && t[p + 1] == '/')) {
        const char *nl = memchr(t + p, '\n', len - p);
        return (nl ? (size_t)(nl - t) : len) - p;
    }
    if (t[p] == '/' && p + 1 < len && t[p + 1] == '*') {
        for (size_t q = p + 2; q + 1 < len; q++) if (t[q] == '*' && t[q + 1] == '/') return q + 2 - p;
        return len - p;
    }
    return 0;
}

const Macro *macro_lookup(const char *name, size_t len) {
    char buf[96];
    if (!g_macros.cap || len == 0 || len >= sizeof buf) return NULL;
    lower_ascii_n(buf, name, len);
    const Macro *m = (const Macro *)smap_get(&g_macros, buf, len);
    /* MACRO-11 keeps only 6 characters of a name: .LOOKUP calls the macro defined as .LOOKU */
    if (!m && len > 6 && buf[0] == '.') m = (const Macro *)smap_get(&g_macros, buf, 6);
    return m;
}
const char *macro_name(const Macro *m) { return m->name; }
size_t macro_ndummies(const Macro *m) { return m->nd; }
bool macro_is_builtin(const Macro *m) { return m->builtin; }

/* ---- definition ---- */

/* first word of a line (after an optional "label:"), lower-cased into w; returns its length (0 if none) */
size_t macro_line_word(const char *t, size_t a, size_t b, char *w, size_t wcap) {
    size_t p = a;
    while (p < b && is_blank(t[p])) p++;
    size_t s = p;
    while (p < b && is_wc(t[p])) p++;
    if (p < b && t[p] == ':' && p > s) {                    /* skip a label */
        p++;
        while (p < b && is_blank(t[p])) p++;
        s = p;
        while (p < b && is_wc(t[p])) p++;
    }
    size_t n = p - s;
    if (n == 0 || n >= wcap) return 0;
    lower_ascii_n(w, t + s, n);
    w[n] = 0;
    return n;
}

size_t macro_define(const char *text, size_t len, size_t pos, const char **err, size_t *err_pos) {
    size_t p = pos;
    while (p < len && is_blank(text[p])) p++;
    size_t ns = p;
    while (p < len && text[p] == '.') p++;                     /* names such as .MACS or ...CM0 */
    if (p >= len || !is_alpha_((unsigned char)text[p]) || text[p] == '$') {
        *err = "A macro name is expected after '.macro'"; *err_pos = ns; return SIZE_MAX;
    }
    while (p < len && is_wc((unsigned char)text[p])) p++;
    size_t ne = p;
    Macro *m = (Macro *)amalloc(sizeof(Macro));
    char lname[96];
    if (ne - ns >= sizeof lname) { *err = "The macro name is too long"; *err_pos = ns; return SIZE_MAX; }
    lower_ascii_n(lname, text + ns, ne - ns);
    m->name = astrndup(lname, ne - ns);

    /* dummy arguments up to the end of the line */
    PVec ds = {0};
    for (;;) {
        while (p < len && (is_blank(text[p]) || text[p] == ',')) p++;
        if (p >= len || text[p] == '\n' || comment_len(text, len, p)) break;
        Dummy *d = (Dummy *)amalloc(sizeof(Dummy));
        if (text[p] == '?') { d->auto_label = true; p++; }
        size_t as = p;
        while (p < len && is_wc((unsigned char)text[p])) p++;
        if (p == as || !is_alpha_((unsigned char)text[as])) { *err = "A dummy argument name is expected here"; *err_pos = as; return SIZE_MAX; }
        char dn[96];
        if (p - as >= sizeof dn) { *err = "The dummy argument name is too long"; *err_pos = as; return SIZE_MAX; }
        lower_ascii_n(dn, text + as, p - as);
        d->name = astrndup(dn, p - as);
        if (p < len && text[p] == '=') {
            p++;
            size_t vs = p;
            if (p < len && text[p] == '<') {
                int depth = 0;
                bool closed = false;
                for (; p < len && text[p] != '\n'; p++) {
                    if (text[p] == '<') depth++;
                    else if (text[p] == '>' && --depth == 0) { p++; closed = true; break; }
                }
                if (!closed) { *err = "The default value of a dummy argument is missing its closing '>'"; *err_pos = vs; return SIZE_MAX; }
                d->def = astrndup(text + vs + 1, p - vs - 2);
            } else {
                while (p < len && text[p] != ',' && text[p] != '\n' && !is_blank(text[p]) && !comment_len(text, len, p)) p++;
                d->def = astrndup(text + vs, p - vs);
            }
        }
        pvec_push(&ds, d);
    }
    m->nd = ds.n;
    m->builtin = g_defining_builtin;
    m->d = (Dummy *)amalloc((ds.n ? ds.n : 1) * sizeof(Dummy));
    for (size_t i = 0; i < ds.n; i++) m->d[i] = *(Dummy *)ds.a[i];
    free(ds.a);

    /* body: from the next line to the matching .endm line */
    while (p < len && text[p] != '\n') p++;
    if (p < len) p++;
    size_t body_start = p;
    int depth = 0;
    while (p < len) {
        const char *nl = memchr(text + p, '\n', len - p);
        size_t le = nl ? (size_t)(nl - text) : len;
        char w[16];
        if (macro_line_word(text, p, le, w, sizeof w)) {
            if (strcmp(w, ".macro") == 0 || strcmp(w, "macro") == 0) depth++;
            else if (strcmp(w, ".endm") == 0 || strcmp(w, "endm") == 0) {
                if (depth == 0) {
                    m->body = astrndup(text + body_start, p - body_start);
                    m->bl = p - body_start;
                    *smap_slot(&g_macros, m->name, strlen(m->name), NULL) = m;
                    return le;
                }
                depth--;
            }
        }
        p = nl ? le + 1 : len;
    }
    *err = "This macro is never closed: a matching '.endm' is expected before the end of the file";
    *err_pos = ns;
    return SIZE_MAX;
}

/* ---- call arguments ---- */

size_t macro_collect_args(const char *text, size_t len, size_t pos, PVec *args) {
    size_t p = pos;
    for (;;) {
        while (p < len && is_blank(text[p])) p++;
        if (p >= len || text[p] == '\n' || comment_len(text, len, p)) return p;
        if (text[p] == ',') { pvec_push(args, astrdup("")); p++; goto after_comma; }
        size_t as, ae;
        if (text[p] == '<') {                              /* <...> : the brackets are removed */
            int depth = 0;
            size_t q = p;
            for (; q < len && text[q] != '\n'; q++) {
                if (text[q] == '<') depth++;
                else if (text[q] == '>' && --depth == 0) break;
            }
            if (q < len && text[q] == '>') { as = p + 1; ae = q; p = q + 1; goto got; }
        }
        {
            int par = 0;
            bool instr = false;
            as = p;
            while (p < len && text[p] != '\n') {
                char ch = text[p];
                if (instr) { if (ch == '"') instr = false; p++; continue; }
                if (ch == '"') { instr = true; p++; continue; }
                if (ch == '(') par++;
                else if (ch == ')' && par > 0) par--;
                else if (ch == ',' && par == 0) break;
                else if (par == 0 && comment_len(text, len, p)) break;
                p++;
            }
            ae = p;
            while (ae > as && is_blank(text[ae - 1])) ae--;
        }
    got:
        pvec_push(args, astrndup(text + as, ae - as));
        while (p < len && is_blank(text[p])) p++;
        if (p < len && text[p] == ',') {
            p++;
        after_comma:
            {   /* a trailing comma means one more (empty) argument */
                size_t q = p;
                while (q < len && is_blank(text[q])) q++;
                if (q >= len || text[q] == '\n' || comment_len(text, len, q)) { pvec_push(args, astrdup("")); return q; }
            }
            continue;
        }
        return p;
    }
}

/* ---- expansion ---- */

char *macro_expand(const Macro *m, const PVec *args, size_t *outlen) {
    Buf out = {0};
    const char *b = m->body;
    size_t bl = m->bl;
    /* real argument for every dummy: given, else default, else (automatic label) else empty */
    const char **val = (const char **)amalloc((m->nd ? m->nd : 1) * sizeof(char *));
    for (size_t i = 0; i < m->nd; i++) {
        const char *a = i < args->n ? (const char *)args->a[i] : NULL;
        if (a && *a) val[i] = a;
        else if (m->d[i].auto_label) val[i] = afmt("%u$", g_auto_label++);
        else if (m->d[i].def) val[i] = m->d[i].def;
        else val[i] = "";
    }
    size_t i = 0;
    bool line_start = true;
    while (i < bl) {
        if (line_start) {                                   /* .mexit ends the expansion */
            size_t le = i;
            while (le < bl && b[le] != '\n') le++;
            char w[16];
            if (macro_line_word(b, i, le, w, sizeof w) && (strcmp(w, ".mexit") == 0 || strcmp(w, "mexit") == 0)) break;
            line_start = false;
        }
        size_t cl = comment_len(b, bl, i);
        if (cl) { buf_putn(&out, b + i, cl); i += cl; continue; }
        unsigned char c = (unsigned char)b[i];
        if (c == '\n') { buf_putc(&out, '\n'); i++; line_start = true; continue; }
        bool start = is_alpha_(c) || (c >= '0' && c <= '9') || (c == '.' && i + 1 < bl && is_alpha_((unsigned char)b[i + 1]));
        if (!start) { buf_putc(&out, c); i++; continue; }
        size_t j = i;
        while (j < bl && is_wc((unsigned char)b[j])) j++;
        size_t wl = j - i;
        long found = -1;
        if (!(c >= '0' && c <= '9') && wl < 96) {
            char lw[96];
            lower_ascii_n(lw, b + i, wl);
            for (size_t k = 0; k < m->nd; k++) if (memcmp(m->d[k].name, lw, wl) == 0 && m->d[k].name[wl] == 0) { found = (long)k; break; }
        }
        if (found < 0) { buf_putn(&out, b + i, wl); i = j; continue; }
        /* a dummy: "x'DUMMY" and "DUMMY'x" concatenate (the apostrophe is dropped) */
        if (out.len >= 2 && out.p[out.len - 1] == '\'' && is_wc((unsigned char)out.p[out.len - 2])) out.len--;
        buf_puts(&out, val[found]);
        i = j;
        if (i + 1 < bl && b[i] == '\'' && !is_blank((unsigned char)b[i + 1]) && b[i + 1] != '\n') i++;     /* the apostrophe after a dummy is the concatenation operator */
    }
    if (out.len == 0 || out.p[out.len - 1] != '\n') buf_putc(&out, '\n');
    char *r = astrndup(out.p, out.len);
    if (outlen) *outlen = out.len;
    buf_free(&out);
    return r;
}

/* ---- built-in compatibility macros (hidden) ---- */

static void define_builtin(const char *text) {
    const char *err; size_t ep;
    macro_define(text, strlen(text), 0, &err, &ep);
}

void macro_init_builtin(void) {
    static bool done;
    if (done) return;
    done = true;
    g_defining_builtin = true;
    /* .addr REG,LABEL : REG = absolute address of LABEL, position independent (3 words) */
    define_builtin(".addr reg, lbl\n\tmov pc,reg\n\tadd #lbl-.,reg\n.endm\n");
    /* .print #TEXT : print the zero-terminated string through EMT 20 */
    define_builtin(".print arg\n\tmov arg,r1\n\tclr r2\n\temt 20\n.endm\n");
    g_defining_builtin = false;
}
