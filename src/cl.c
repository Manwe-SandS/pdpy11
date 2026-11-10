/* Relocatable object mode (hidden Turbo8 compatibility): while the final walk emits the code,
   every word whose value mentions a symbol is written as zero and described by a reference
   (postfix expression) for the linker. */
#include "comp.h"
#include "objfile.h"

bool g_cl_mode, g_cl_allow, g_cl_undef_ok;
int g_cl_hits, g_cl_undef;
int64_t g_cl_base;
Obj g_cl_acc;                                    /* references of the current walk */

void cl_reset_walk(int64_t base) {
    g_cl_base = base;
    g_cl_acc.nref = 0;
    g_cl_allow = g_cl_undef_ok = false;
}

static void add_tok(ObjRef *r, int kind, int64_t v, const char *name) {
    r->tok = (ObjTok *)xrealloc(r->tok, (r->ntok + 1) * sizeof(ObjTok));
    ObjTok *t = &r->tok[r->ntok++];
    t->kind = kind; t->v = v; t->name = NULL;
    if (name) {
        size_t l = strlen(name);
        char *u = (char *)amalloc(l + 1);
        for (size_t i = 0; i < l; i++) u[i] = (name[i] >= 'a' && name[i] <= 'z') ? (char)(name[i] - 32) : name[i];
        u[l] = 0;
        t->name = u;
    }
}

static int op_code(OpId id) {
    switch (id) {
    case OP_POS: return OC_POS;
    case OP_NEG: return OC_NEG;
    case OP_INV: case OP_INV2: return OC_INV;
    case OP_ADD: return OC_ADD;
    case OP_SUB: return OC_SUB;
    case OP_MUL: return OC_MUL;
    case OP_DIV: return OC_DIV;
    case OP_MOD: return OC_MOD;
    case OP_LSHIFT: return OC_SHL;
    case OP_RSHIFT: return OC_SHR;
    case OP_OR: case OP_OR2: return OC_OR;
    case OP_XOR: return OC_XOR;
    case OP_AND: return OC_AND;
    default: return 0;
    }
}

static bool rpn(Node *n, State *st, ObjRef *r, bool *has_local) {
    switch (n->kind) {
    case N_NUMBER: add_tok(r, OT_NUM, n->u.num.value, NULL); return true;
    case N_CHARLIT: { Value v = eval(n, st); add_tok(r, OT_NUM, v.t == V_INT ? v.i : 0, NULL); return true; }
    case N_PAREN: return rpn(n->u.paren.expr, st, r, has_local);
    case N_IPTR: add_tok(r, OT_DOT, st->emit_address - g_cl_base, NULL); return true;
    case N_SYMBOL: {
        Sym *s = sym_lookup_symbol(n, st, false);
        if (s && s->is_label && strncmp(s->key, ".local", 6) == 0) {
            add_tok(r, OT_LOCAL, s->addr - g_cl_base, n->u.sym.name);
            *has_local = true;
        } else add_tok(r, OT_SYM, 0, n->u.sym.name);
        return true;
    }
    case N_INFIX: {
        int c = op_code(n->u.op.op->id);
        if (!c) return false;
        if (!rpn(n->u.op.lhs, st, r, has_local) || !rpn(n->u.op.rhs, st, r, has_local)) return false;
        add_tok(r, OT_OP, c, NULL);
        return true;
    }
    case N_PREFIX: {
        int c = op_code(n->u.op.op->id);
        if (!c) return false;
        if (!rpn(n->u.op.rhs, st, r, has_local)) return false;
        add_tok(r, OT_OP, c, NULL);
        return true;
    }
    default: return false;
    }
}

/* the value of `expr` is stored (as `type`) at address `addr` after linking */
void cl_record(State *st, Node *expr, uint32_t type, int64_t addr) {
    ObjRef *r = obj_add_ref(&g_cl_acc, type, (uint32_t)(addr - g_cl_base));
    bool has_local = false;
    if (!rpn(expr, st, r, &has_local)) {
        ERR1("object-unsupported", expr->src, expr->s, expr->e, "This expression cannot be stored in an object file (only numbers, symbols, '.' and arithmetic are allowed)");
        return;
    }
    if (has_local && type == RT_WORD) r->type = RT_WORD_LOCAL;
}

int64_t get_as_int_ref(State *st, const char *what, const Node *token, Node *arg, int bitness, bool unsigned_, bool has_default, int64_t def, uint32_t type, int64_t addr) {
    if (!g_cl_mode || IS_LAYOUT()) return get_as_int(st, what, token, arg, bitness, unsigned_, has_default, def);
    int h0 = g_cl_hits;
    g_cl_allow = true;
    int64_t v = get_as_int(st, what, token, arg, bitness, unsigned_, has_default, def);
    g_cl_allow = false;
    if (g_cl_hits != h0) { cl_record(st, arg, type, addr); return 0; }
    return v;
}

/* the module built by the final walk */
void cl_build_object(Obj *o, int64_t base, bool relocatable) {
    memset(o, 0, sizeof *o);
    o->link_addr = (uint32_t)(base & 0xFFFF);
    o->flags = relocatable ? 0 : 2;
    o->has_code = true;
    buf_putn(&o->code, G.out.p ? G.out.p : "", G.out.len);
    for (size_t i = 0; i < G.sym_order.n; i++) {
        Sym *s = (Sym *)G.sym_order.a[i];
        if (strncmp(s->key, ".internal", 9) != 0 || s->stamp != G.walk_id) continue;
        const char *dot = strchr(s->key + 9, '.');
        if (!dot) continue;
        char *name = astrdup(dot + 1);
        for (char *p = name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        if (s->is_label) obj_add_label(o, name, 1, s->addr - base);
        else { Value v = eval_assignment(s); if (v.t == V_INT) obj_add_label(o, name, 3, v.i); }
    }
    if (relocatable) {
        o->ref = g_cl_acc.ref; o->nref = g_cl_acc.nref; o->capref = g_cl_acc.capref;
    }
}

/* the table of labels (absolute values) of the module */
void cl_build_table(Obj *o, int64_t base) {
    Obj tmp;
    cl_build_object(&tmp, base, false);
    memset(o, 0, sizeof *o);
    o->link_addr = (uint32_t)(base & 0xFFFF);
    o->flags = 2;
    for (size_t i = 0; i < tmp.nlab; i++) obj_add_label(o, tmp.lab[i].name, 3, tmp.lab[i].kind == 3 ? tmp.lab[i].val : base + tmp.lab[i].val);
}
