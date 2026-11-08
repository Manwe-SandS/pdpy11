#include "comp.h"
#include <stdio.h>

jmp_buf *g_recov;
jmp_buf *g_item;
bool g_fatal;
int g_undef;
LinkBase *g_hit_lb;
bool g_hit_multi, g_track_on;
void note_hit(LinkBase *lb) {
    if (!g_hit_lb) g_hit_lb = lb; else if (g_hit_lb != lb) g_hit_multi = true;
}

void recoverable_error(void) {
    if (g_recov) longjmp(*g_recov, 1);
    if (g_item) longjmp(*g_item, 1);
    unrecoverable();
}

PD_NORETURN void internal_error(const char *msg) {
    fflush(stdout);
    fprintf(stderr, "An unexpected internal compiler error happened.\nPlease report this to https://github.com/pdpy11/pdpy11/issues.\nThe following information will be of interest to the maintainer\n(hopefully along with some samples for reproduction):\n\n---\n\nVersion: pdpy11 2.0.4 (C)\n\n%s\n", msg);
    exit(1);
}

/* ---- saturating int64 arithmetic (Python ints are unbounded; we clamp) ---- */
static int64_t s_add(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b) return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b) return INT64_MIN;
    return a + b;
}
static int64_t s_sub(int64_t a, int64_t b) {
    if (b < 0 && a > INT64_MAX + b) return INT64_MAX;
    if (b > 0 && a < INT64_MIN + b) return INT64_MIN;
    return a - b;
}
static int64_t s_neg(int64_t a) { return a == INT64_MIN ? INT64_MAX : -a; }
static int64_t s_mul(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return 0;
    if (a == -1) return s_neg(b);
    if (b == -1) return s_neg(a);
    if ((a > 0) == (b > 0)) { if ((a > 0 ? a : -a) > INT64_MAX / (b > 0 ? b : -b)) return INT64_MAX; }
    else { int64_t aa = a > 0 ? a : -a, bb = b > 0 ? b : -b; if (aa > INT64_MAX / bb) return INT64_MIN; }
    return a * b;
}
static int64_t s_shl(int64_t a, int64_t b) {   /* a * 2**b, b >= 0 */
    if (a == 0) return 0;
    if (b >= 63) return a > 0 ? INT64_MAX : INT64_MIN;
    return s_mul(a, (int64_t)1 << b);
}
static int64_t s_sar(int64_t a, int64_t b) {   /* a >> b, b >= 0 */
    if (b >= 63) return a < 0 ? -1 : 0;
    return a >= 0 ? (a >> b) : ~((~a) >> b);
}
static int64_t floordiv(int64_t a, int64_t b) {
    if (a == INT64_MIN && b == -1) return INT64_MAX;
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
static int64_t floormod(int64_t a, int64_t b) {
    if (a == INT64_MIN && b == -1) return 0;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

static Value vint(int64_t i) { Value v; v.t = V_INT; v.i = i; v.s = U32_EMPTY; return v; }
static Value vstr(U32 s) { Value v; v.t = V_STR; v.i = 0; v.s = s; return v; }

static int64_t need_int(const Value *v, const char *what) {
    if (v->t != V_INT) internal_error(afmt("TypeError: unsupported operand type(s) for %s: 'str'", what));
    return v->i;
}

/* ---------------- symbols ---------------- */
Sym *sym_find_key(const char *key, size_t kl) {
    char stack[320];
    char *lo = kl < sizeof stack ? stack : (char *)amalloc(kl + 1);
    lower_ascii_n(lo, key, kl);
    return (Sym *)smap_get(&G.symbols, lo, kl);
}

static Sym *find_prefixed(const char *prefix, size_t pl, const char *lname, size_t nl) {
    char stack[320];
    size_t kl = pl + nl;
    char *k = kl < sizeof stack ? stack : (char *)amalloc(kl + 1);
    memcpy(k, prefix, pl);
    memcpy(k + pl, lname, nl);
    return (Sym *)smap_get(&G.symbols, k, kl);
}

Value eval_assignment(Sym *s);

/* Resolve a Symbol node; returns the Sym (or NULL when undefined) */
Sym *sym_lookup_symbol(Node *n, State *st, bool report_missing) {
    const char *ln = n->u.sym.lname;
    size_t nl = strlen(ln);
    if (!n->u.sym.nec_label && is_register_name(ln, nl)) {
        ERR1("unexpected-register", n->src, n->s, n->e, "A register cannot be used here. An integer or a symbol are expected");
        recoverable_error();
    }
    Sym *s = find_prefixed(st->local_prefix, strlen(st->local_prefix), ln, nl);
    if (!s) s = find_prefixed(st->fs->internal_prefix, st->fs->internal_prefix_len, ln, nl);
    if (!s) {
        ExtEnt *e = (ExtEnt *)smap_get(&G.externs, ln, nl);
        if (e) s = (Sym *)smap_get(&G.symbols, e->key, strlen(e->key));
    }
    if (!s && report_missing && g_cl_undef_ok) { g_cl_undef++; return NULL; }     /* object mode: an external label */
    if (!s && report_missing) {
        g_undef++;
        if (IS_LAYOUT()) G.unresolved++;
        else {
            const char *nm = n->u.sym.name;
            bool regish = (nm[0] == 'r' || nm[0] == 'R') && nm[1] >= '0' && nm[1] <= '9';
            bool digits = true;
            for (const char *q = nm; *q; q++) if (*q < '0' || *q > '9') digits = false;
            if (digits)
                ERR1("undefined-symbol", n->src, n->s, n->e, "Local label '%s:' is not defined (a local label is visible only until the next ordinary label)", nm);
            else
                ERR1("undefined-symbol", n->src, n->s, n->e, "Unknown symbol '%s' is referenced here%s", nm,
                     regish ? " (the registers are r0-r7, sp and pc)" : "");
        }
    }
    return s;
}

Value eval_assignment(Sym *s) {
    AsgExt *x = s->x;
    if (x->eval_stamp == G.walk_id) {
        if (x->dep_lb) note_hit(x->dep_lb);
        if (x->dep_multi) g_hit_multi = true;
        return x->val;
    }
    if (x->evaluating) {
        if (g_track_on) { g_hit_multi = true; return vint(0); }   /* reported by set_link_address */
        if (x->cyc_stamp != G.walk_id) {
            x->cyc_stamp = G.walk_id;
            ERR1("recursive-definition", s->def->src, s->def->s, s->def->e, "The value of '%s' is defined in terms of itself, so it cannot be determined", s->def->u.assign.target->u.sym.name);
        }
        return vint(0);
    }
    x->evaluating = 1;
    State copy = x->st;
    LinkBase *outer_lb = g_hit_lb; bool outer_multi = g_hit_multi;
    g_hit_lb = NULL; g_hit_multi = false;
    Value v = eval(s->def->u.assign.value, &copy);
    x->dep_lb = g_hit_lb; x->dep_multi = g_hit_multi;
    g_hit_lb = outer_lb; g_hit_multi = outer_multi;
    if (x->dep_lb) note_hit(x->dep_lb);
    if (x->dep_multi) g_hit_multi = true;
    x->evaluating = 0;
    x->val = v;
    x->eval_stamp = G.walk_id;
    return v;
}

/* ---------------- get_as_* ---------------- */
static const char *vtype_name(const Value *v) { return v->t == V_INT ? "int" : "str"; }

int64_t get_as_int(State *st, const char *what, const Node *token, Node *arg, int bitness, bool unsigned_, bool has_default, int64_t def) {
    int undef0 = g_undef;
    Value v = eval(arg, st);
    if (g_undef != undef0 && v.t == V_INT) return has_default ? def : 0;   /* value unknown: do not add derived errors */
    if (v.t != V_INT) {
        rep2(PRI_ERROR, "type-mismatch", token->src, token->s, token->e, afmt("A number was expected as %s", what),
             arg->src, arg->s, arg->e, afmt("...yet the evaluated value is not an integer but %s", vtype_name(&v)));
        recoverable_error();
    }
    int64_t value = v.i;
    if (unsigned_ && value < 0) {
        ERR1("value-out-of-bounds", arg->src, arg->s, arg->e, "An unsigned integer is expected as %s, but %lld was passed", what, (long long)value);
        if (!has_default) recoverable_error();
        return def;
    }
    if (bitness == 0) return value;
    int64_t lim = (int64_t)1 << bitness;
    if (value <= -lim) {
        ERR1("value-out-of-bounds", arg->src, arg->s, arg->e, "The value is too small: %s %lld does not fit in %d bits", what, (long long)value, bitness);
        if (!has_default) recoverable_error();
        return def;
    }
    if (value >= lim) {
        ERR1("value-out-of-bounds", arg->src, arg->s, arg->e, "The value is too large: %s %lld does not fit in %d bits", what, (long long)value, bitness);
        if (!has_default) recoverable_error();
        return def;
    }
    return floormod(value, lim);
}

U32 get_as_str(State *st, const char *what, const Node *token, Node *arg) {
    Value v = eval(arg, st);
    if (v.t == V_STR) return v.s;
    rep2(PRI_ERROR, "type-mismatch", token->src, token->s, token->e, afmt("A string was expected for %s", what),
         arg->src, arg->s, arg->e, afmt("...yet the evaluated value is not a string but %s", vtype_name(&v)));
    recoverable_error();
    return U32_EMPTY;
}

/* ---------------- operators ---------------- */
static Value eval_op(Node *n, State *st) {
    const OpDef *op = n->u.op.op;
    if (!op->pure && n->u.op.cached) return *n->u.op.cached;
    Value lhs = {0}, rhs;
    if (n->kind == N_INFIX) lhs = eval(n->u.op.lhs, st);
    rhs = eval(n->u.op.rhs, st);     /* for unary operators rhs holds the operand */
    Value res;
    switch (op->id) {
    case OP_POSTADD:
        ERR1("unexpected-value", n->src, n->s, n->e, "The '...+' operator cannot be used with a value. Only syntax like '(r0)+' is allowed.");
        res = rhs; break;
    case OP_POSTSUB:
        ERR1("unexpected-value", n->src, n->s, n->e, "The '...-' operator cannot be used with a value.");
        res = rhs; break;
    case OP_POS: res = vint(need_int(&rhs, "unary +")); break;
    case OP_NEG: res = vint(s_neg(need_int(&rhs, "unary -"))); break;
    case OP_INV: case OP_INV2: res = vint(~need_int(&rhs, "~")); break;
    case OP_MUL: res = vint(s_mul(need_int(&lhs, "*"), need_int(&rhs, "*"))); break;
    case OP_DIV: {
        int64_t a = need_int(&lhs, "/"), b = need_int(&rhs, "/");
        if (b == 0) { ERR1("arithmetic-error", n->src, n->s, n->e, "Division by zero"); res = vint(0); }
        else res = vint(floordiv(a, b));
        break;
    }
    case OP_MOD: {
        int64_t a = need_int(&lhs, "%"), b = need_int(&rhs, "%");
        if (b == 0) { ERR1("arithmetic-error", n->src, n->s, n->e, "Division by zero"); res = vint(0); }
        else res = vint(floormod(a, b));
        break;
    }
    case OP_ADD:
        if (lhs.t == V_STR && rhs.t == V_STR) {
            U32 r; r.n = lhs.s.n + rhs.s.n; r.cp = (uint32_t *)amalloc((r.n + 1) * 4);
            if (lhs.s.n) memcpy(r.cp, lhs.s.cp, lhs.s.n * 4);
            if (rhs.s.n) memcpy(r.cp + lhs.s.n, rhs.s.cp, rhs.s.n * 4);
            res = vstr(r);
        } else res = vint(s_add(need_int(&lhs, "+"), need_int(&rhs, "+")));
        break;
    case OP_SUB: res = vint(s_sub(need_int(&lhs, "-"), need_int(&rhs, "-"))); break;
    case OP_LSHIFT: {
        int64_t a = need_int(&lhs, "<<"), b = need_int(&rhs, "<<");
        if (b >= 0) res = vint(s_shl(a, b));
        else {
            ERR1("arithmetic-error", n->src, n->s, n->e, "Negative left shift: '<< %lld'. If you want this to be interpreted as '>> %lld',\neither use >> if you know the right hand side is always non-positive, or _ if you don't.", (long long)b, (long long)s_neg(b));
            res = vint(s_sar(a, s_neg(b)));
        }
        break;
    }
    case OP_RSHIFT: {
        int64_t a = need_int(&lhs, ">>"), b = need_int(&rhs, ">>");
        if (b == 0) res = vint(a);
        else if (b > 0) res = vint(s_sar(a, b));
        else {
            ERR1("arithmetic-error", n->src, n->s, n->e, "Negative right shift: '>> %lld'. If you want this to be interpreted as '<< %lld',\neither use << if you know the right hand side is always non-positive, or _ (with an inverted operand) if you don't.", (long long)b, (long long)s_neg(b));
            res = vint(s_shl(a, s_neg(b)));
        }
        break;
    }
    case OP_LSH: {
        int64_t a = need_int(&lhs, "_"), b = need_int(&rhs, "_");
        res = vint(b >= 0 ? s_shl(a, b) : s_sar(a, s_neg(b)));
        break;
    }
    case OP_AND: res = vint(need_int(&lhs, "&") & need_int(&rhs, "&")); break;
    case OP_XOR: res = vint(need_int(&lhs, "^") ^ need_int(&rhs, "^")); break;
    case OP_OR: case OP_OR2: res = vint(need_int(&lhs, "|") | need_int(&rhs, "|")); break;
    case OP_IMMEDIATE:
        ERR1("unexpected-value", n->src, n->s, n->e, "'#...' cannot be used in this context. You should probably remove the hash sign.");
        res = rhs; break;
    case OP_DEFERRED:
        ERR1("unexpected-value", n->src, n->s, n->e, "'@...' cannot be used as a value. A common reason for this error is using '#@' instead of '@#'.");
        res = rhs; break;
    case OP_REGISTER:
        ERR1("unexpected-value", n->src, n->s, n->e, "'%%...' cannot be used as a value because the value of a register is not known during compilation.");
        res = rhs; break;
    case OP_CALL:
        ERR1("unexpected-value", n->src, n->s, n->e, "A construct of kind 'A(B)' is only reasonable in context like '1(r0)'.");
        res = rhs; break;
    default: res = vint(0);
    }
    if (!op->pure && !g_silent) { n->u.op.cached = (Value *)amalloc(sizeof(Value)); *n->u.op.cached = res; }
    return res;
}

static Value eval_charlit(Node *n, State *st) {
    (void)st;
    if (n->u.chr.evaluated) return vint(n->u.chr.value);
    Buf b = {0};
    char *err;
    int64_t result;
    bool cacheable = !g_silent;
    if (!cs_encode(&G.charset, &n->u.chr.str, &b, &err)) {
        ERR1("invalid-character", n->src, n->s, n->e, "Cannot encode this literal using the selected output charset:\n%s\nYou can change the charset using --charset CLI argument or '.charset' directive.", err);
        result = 0;
    } else {
        if (b.len > 2) {
            ERR1("too-long-string", n->src, n->s, n->e, "These characters, encoded to %s, take %zu bytes, which does not fit in 16 bits.\nPlease fix that. Changing the encoding via '.charset ?\?\?' directive or '--charset=?\?\?' CLI argument may help.", G.charset.raw_name, b.len);
        }
        unsigned lo = b.len > 0 ? (unsigned char)b.p[0] : 0, hi = b.len > 1 ? (unsigned char)b.p[1] : 0;
        result = (int64_t)(lo | (hi << 8));
    }
    buf_free(&b);
    if (cacheable) { n->u.chr.evaluated = true; n->u.chr.value = result; }
    return vint(result);
}

Value eval(Node *n, State *st) {
    switch (n->kind) {
    case N_NUMBER:
        if (!n->u.num.reported && n->u.num.invalid_base8 && !g_silent) {
            n->u.num.reported = true;
            const char *r = n->u.num.repr;
            char d = strchr(r, '8') ? '8' : '9';
            ERR1("invalid-number", n->src, n->s, n->e, "In PDP-11 assembly, numbers are considered base-8 by default.\nThis number has digit %c, so you probably wanted it base-10.\nAdd a dot after the number to switch to decimal: '%s.'\nIf you wanted to specify a local label of the same name, add a colon: '%s:'", d, r, r);
        }
        return vint(n->u.num.value);
    case N_IPTR:
        if (g_cl_allow) { g_cl_hits++; return vint(0); }
        note_hit(st->fs->lb); return vint(st->emit_address);
    case N_PAREN: return eval(n->u.paren.expr, st);
    case N_SYMBOL: {
        if (g_cl_allow) { sym_lookup_symbol(n, st, false); g_cl_hits++; return vint(0); }     /* object mode: left for the linker */
        Sym *s = sym_lookup_symbol(n, st, true);
        if (!s) return vint(0);
        if (s->is_label) { note_hit(s->lb); return vint(s->addr); }
        return eval_assignment(s);
    }
    case N_INFIX: case N_PREFIX: case N_POSTFIX: return eval_op(n, st);
    case N_CHARLIT: return eval_charlit(n, st);
    case N_QSTR: return vstr(n->u.qstr.str);
    case N_ANGLE: {
        if (n->u.angle.reported) return vstr(U32_EMPTY);
        int64_t code = get_as_int(st, "Unicode code point", n, n->u.angle.expr, 0, false, false, 0);
        if (code < 0 || code >= 0x110000) {
            if (!g_silent) n->u.angle.reported = true;
            ERR1("value-out-of-bounds", n->src, n->s, n->e, "<%lld> does not specify a valid Unicode code point because it's outside its range: [0; 0x110000).", (long long)code);
            return vstr(U32_EMPTY);
        }
        U32 r; r.n = 1; r.cp = (uint32_t *)amalloc(8); r.cp[0] = (uint32_t)code;
        return vstr(r);
    }
    case N_STRCAT: {
        U32 r = {0}; size_t cap = 0;
        for (size_t i = 0; i < n->u.cat.chunks.n; i++) {
            U32 p = get_as_str(st, "string chunk", n, (Node *)n->u.cat.chunks.a[i]);
            if (r.n + p.n > cap) { cap = (r.n + p.n) * 2 + 8; r.cp = (uint32_t *)xrealloc(r.cp, cap * 4); }
            if (p.n) memcpy(r.cp + r.n, p.cp, p.n * 4);
            r.n += p.n;
        }
        U32 a; a.n = r.n; a.cp = (uint32_t *)amalloc((r.n + 1) * 4); if (r.n) memcpy(a.cp, r.cp, r.n * 4); free(r.cp);
        return vstr(a);
    }
    default:
        internal_error("cannot evaluate this node");
    }
}
