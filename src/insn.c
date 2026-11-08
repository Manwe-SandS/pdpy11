/* Port of pdpy11/insns.py: encoding of machine instructions */
#include "comp.h"

static bool is_op(const Node *n, OpId id) {
    return (n->kind == N_INFIX || n->kind == N_PREFIX || n->kind == N_POSTFIX) && n->u.op.op->id == id;
}
static bool is_paren_round(const Node *n) { return n->kind == N_PAREN && strcmp(n->u.paren.open, "(") == 0; }

/* returns: -1 no register; 0..7 register; 100 = %expr (needs evaluation) */
static int as_register_static(const Node *n) {
    if (n->kind == N_SYMBOL && !n->u.sym.nec_label) return register_index(n->u.sym.lname, strlen(n->u.sym.lname));
    if (is_op(n, OP_REGISTER)) return 100;
    return -1;
}
static int try_as_register(Node *n, State *st, bool evaluate) {
    int r = as_register_static(n);
    if (r == 100) {
        if (!evaluate) return 0;
        return (int)get_as_int(st, "register index", n, n->u.op.rhs, 3, true, false, 0);
    }
    return r;
}
static int try_accumulator(const Node *n) {
    if (n->kind == N_SYMBOL) {
        const char *s = n->u.sym.lname;
        if (strlen(s) == 3 && s[0] == 'a' && s[1] == 'c' && s[2] >= '0' && s[2] <= '5') return s[2] - '0';
    }
    return -1;
}

static Node *mk_call(Node *tmpl, size_t s, size_t e, Node *lhs, Node *rhs) {
    Node *n = node_new(N_INFIX, tmpl->src, s, e);
    n->u.op.op = op_def(OP_CALL); n->u.op.lhs = lhs; n->u.op.rhs = rhs;
    return n;
}

static Node *hoist(Node *t, State *st) {
    (void)st;
    if (t->kind == N_INFIX && t->u.op.op->id != OP_CALL) {
        t->u.op.rhs = hoist(t->u.op.rhs, st);
        Node *r = t->u.op.rhs;
        if (is_op(r, OP_CALL) && as_register_static(r->u.op.rhs) >= 0) {
            Node *reg = r->u.op.rhs;
            size_t ce = t->e;
            t->u.op.rhs = r->u.op.lhs;
            t->e = t->u.op.rhs->e;
            return mk_call(t, t->s, ce, t, reg);
        }
    } else if (t->kind == N_PREFIX) {
        t->u.op.rhs = hoist(t->u.op.rhs, st);
        Node *r = t->u.op.rhs;
        if (is_op(r, OP_CALL) && as_register_static(r->u.op.rhs) >= 0) {
            Node *reg = r->u.op.rhs;
            size_t ce = t->e;
            t->u.op.rhs = r->u.op.lhs;
            t->e = t->u.op.rhs->e;
            return mk_call(t, t->s, ce, t, reg);
        }
    }
    return t;
}

typedef struct { int64_t val; bool has_extra; uint16_t extra; } Enc;

static const char *ordinal(char pc) { return (pc == 's' || pc == 'S') ? "first" : "second"; }

static uint16_t extra_int(State *st, const char *what, Node *tok, Node *arg, bool layout, int64_t addr) {
    if (layout) return 0;
    return (uint16_t)get_as_int_ref(st, what, tok, arg, 16, false, false, 0, RT_WORD, addr);
}
static uint16_t extra_rel(State *st, Node *e, int64_t rel, bool layout) {
    if (layout) return 0;
    int h0 = g_cl_hits;
    if (g_cl_mode) g_cl_allow = true;
    Value v = eval(e, st);
    g_cl_allow = false;
    if (g_cl_mode && g_cl_hits != h0) { cl_record(st, e, RT_REL, rel); return 0; }
    if (v.t != V_INT) internal_error("TypeError: unsupported operand type(s) for -: 'str' and 'int'");
    int64_t d = v.i - rel - 2;
    return (uint16_t)((uint64_t)d & 0xFFFF);
}

static void enc_rm(Node **slot, State *st, int64_t rel, bool layout, Enc *out) {
    Node *operand = hoist(*slot, st);
    *slot = operand;
    int reg;
    out->has_extra = false; out->extra = 0;
    if ((reg = try_as_register(operand, st, !layout)) >= 0) { out->val = reg; return; }
    if (is_paren_round(operand) && (reg = try_as_register(operand->u.paren.expr, st, !layout)) >= 0) { out->val = 010 | reg; return; }
    if (is_op(operand, OP_DEFERRED) && (reg = try_as_register(operand->u.op.rhs, st, !layout)) >= 0) {
        WARN1("legacy-deferred", operand->src, operand->s, operand->e, "%s is a legacy way of spelling (%s), please use the new syntax", node_repr_str(operand), node_repr_str(operand->u.op.rhs));
        out->val = 010 | reg; return;
    }
    if (is_op(operand, OP_POSTADD) && is_paren_round(operand->u.op.rhs) && (reg = try_as_register(operand->u.op.rhs->u.paren.expr, st, !layout)) >= 0) { out->val = 020 | reg; return; }
    if (is_op(operand, OP_DEFERRED) && is_op(operand->u.op.rhs, OP_POSTADD) && is_paren_round(operand->u.op.rhs->u.op.rhs) &&
        (reg = try_as_register(operand->u.op.rhs->u.op.rhs->u.paren.expr, st, !layout)) >= 0) { out->val = 030 | reg; return; }
    if (is_op(operand, OP_NEG) && is_paren_round(operand->u.op.rhs) && (reg = try_as_register(operand->u.op.rhs->u.paren.expr, st, !layout)) >= 0) { out->val = 040 | reg; return; }
    if (is_op(operand, OP_DEFERRED) && is_op(operand->u.op.rhs, OP_NEG) && is_paren_round(operand->u.op.rhs->u.op.rhs) &&
        (reg = try_as_register(operand->u.op.rhs->u.op.rhs->u.paren.expr, st, !layout)) >= 0) { out->val = 050 | reg; return; }
    if (is_op(operand, OP_CALL) && is_op(operand->u.op.lhs, OP_DEFERRED) && (reg = try_as_register(operand->u.op.rhs, st, !layout)) >= 0) {
        out->val = 070 | reg; out->has_extra = true;
        out->extra = extra_int(st, "an index", operand, operand->u.op.lhs->u.op.rhs, layout, rel);
        return;
    }
    if (is_op(operand, OP_CALL) && (reg = try_as_register(operand->u.op.rhs, st, !layout)) >= 0) {
        out->val = 060 | reg; out->has_extra = true;
        out->extra = extra_int(st, "an index", operand, operand->u.op.lhs, layout, rel);
        return;
    }
    if (is_op(operand, OP_DEFERRED) && is_paren_round(operand->u.op.rhs) && (reg = try_as_register(operand->u.op.rhs->u.paren.expr, st, !layout)) >= 0) {
        WARN1("implicit-index", operand->src, operand->s, operand->e, "PDP-11 doesn't have %s addressing mode.\nThis expression is parsed as @0%s, which does what you probably expect.\nHowever, this is in fact index deferred addressing with an implicit zero offset.\nYou might want to insert a zero for clarity.", node_repr_str(operand), node_repr_str(operand->u.op.rhs));
        out->val = 070 | reg; out->has_extra = true; out->extra = 0;
        return;
    }
    if (is_op(operand, OP_IMMEDIATE)) {
        out->val = 027; out->has_extra = true;
        out->extra = extra_int(st, "an immediate value", operand, operand->u.op.rhs, layout, rel);
        return;
    }
    if (is_op(operand, OP_DEFERRED) && is_op(operand->u.op.rhs, OP_IMMEDIATE)) {
        out->val = 037; out->has_extra = true;
        out->extra = extra_int(st, "an absolute address", operand, operand->u.op.rhs->u.op.rhs, layout, rel);
        return;
    }
    if (is_op(operand, OP_DEFERRED)) {
        out->val = 077; out->has_extra = true;
        out->extra = extra_rel(st, operand->u.op.rhs, rel, layout);
        return;
    }
    out->val = 067; out->has_extra = true;
    out->extra = extra_rel(st, operand, rel, layout);
}

static bool text_has(const Node *n, char ch) { return memchr(n->src->text + n->s, ch, n->e - n->s) != NULL; }

static Node *fixup_label(Node *t, bool *active, Node *insn, Node *top) {
    switch (t->kind) {
    case N_INFIX:
        t->u.op.lhs = fixup_label(t->u.op.lhs, active, insn, top);
        t->u.op.rhs = fixup_label(t->u.op.rhs, active, insn, top);
        return t;
    case N_PREFIX: case N_POSTFIX:
        t->u.op.rhs = fixup_label(t->u.op.rhs, active, insn, top);
        return t;
    case N_NUMBER:
        if (t->u.num.valid_label) {
            if (*active) {
                const char *tr = node_repr_str(t);
                rep3(PRI_WARNING, "label-fixup",
                     insn->u.insn.name->src, insn->u.insn.name->s, insn->u.insn.name->e, afmt("Instruction '%s' takes an offset.", insn->u.insn.name->u.sym.name),
                     top->src, top->s, top->e, "It's operand is a complex expression.",
                     t->src, t->s, t->e, afmt("Thus, for compatibility, the first number that can be parsed as a local label is treated as such.\nFor example, '1 + 2' is parsed as 'address of local label 1 plus two'.\nThis may be unintended, so please state your intent explicitly:\n- if you meant numbers to be numbers, add parentheses around the operand: '(%s)', and\n- if you wanted '%s' to be a label, add a colon after it: '%s:'.", node_repr_str(top), tr, tr));
                *active = false;
                Node *s = node_new(N_SYMBOL, t->src, t->s, t->e);
                s->u.sym.name = t->u.num.repr;
                size_t l = strlen(t->u.num.repr);
                char *low = (char *)amalloc(l + 1); lower_ascii_n(low, t->u.num.repr, l);
                s->u.sym.lname = low; s->u.sym.nec_label = true;
                return s;
            }
            return t;
        }
        return t;      /* a number that cannot be a local label (e.g. "2." in ".-2."): the reference implementation crashes here */
    case N_SYMBOL: case N_IPTR:
        *active = false;
        return t;
    default:
        internal_error("AssertionError");
    }
}

static void enc_off(const OperandDef *d, Node *operand, State *st, Node *insn, int64_t rel, bool layout, Enc *out) {
    out->has_extra = false;
    out->val = 0;
    if (layout) return;
    if (operand->kind == N_NUMBER && operand->u.num.valid_label) {
        Node *s = node_new(N_SYMBOL, operand->src, operand->s, operand->e);
        s->u.sym.name = operand->u.num.repr;
        size_t l = strlen(operand->u.num.repr);
        char *low = (char *)amalloc(l + 1); lower_ascii_n(low, operand->u.num.repr, l);
        s->u.sym.lname = low; s->u.sym.nec_label = true;
        operand = s;
    } else if (!text_has(operand, '(') && !text_has(operand, ':')) {
        bool active = true;
        fixup_label(operand, &active, insn, operand);
    }
    int undef0 = g_undef, cu0 = g_cl_undef;
    if (g_cl_mode && !d->unsigned_) g_cl_undef_ok = true;      /* object mode: a branch to an external label is left for the linker */
    Value v = eval(operand, st);
    g_cl_undef_ok = false;
    if (g_cl_mode && g_cl_undef != cu0) { cl_record(st, operand, RT_BRANCH, rel - 2); out->val = 0; return; }
    if (v.t != V_INT) internal_error("TypeError: unsupported operand type(s) for -: 'str' and 'int'");
    if (g_undef != undef0) { out->val = 0; return; }       /* unknown target: no derived range errors */
    int64_t offset = v.i - rel;
    bool error = false;
    bool uns = d->unsigned_;
    const char *iname = insn->u.insn.name->u.sym.name;
    Node *iname_n = insn->u.insn.name;
    if (uns && offset > 0) {
        Sym *def = (operand->kind == N_SYMBOL) ? sym_lookup_symbol(operand, st, false) : NULL;
        if (def && def->is_label) {
            rep2(PRI_ERROR, "branch-out-of-bounds", iname_n->src, iname_n->s, iname_n->e, afmt("Instruction '%s' can only jump backwards", iname),
                 def->def->src, def->def->s, def->def->e, "...but the destination is located further");
        } else {
            rep2(PRI_ERROR, "branch-out-of-bounds", iname_n->src, iname_n->s, iname_n->e, afmt("Instruction '%s' can only jump backwards", iname),
                 operand->src, operand->s, operand->e, afmt("...but the offset to the destination is positive (exactly %lld)", (long long)offset));
        }
        error = true;
    } else {
        int bits = d->nbits;
        int64_t min_off = -((int64_t)1 << (bits + (uns ? 1 : 0))) + (uns ? 2 : 0);
        int64_t max_off = uns ? 0 : ((int64_t)1 << bits) - 2;
        if (!(min_off <= offset && offset <= max_off)) {
            rep2(PRI_ERROR, "branch-out-of-bounds", iname_n->src, iname_n->s, iname_n->e, afmt("Instruction '%s' can only jump from %lld to %lld (inclusively)", iname, (long long)min_off, (long long)max_off),
                 operand->src, operand->s, operand->e, afmt("...but this offset is out of bounds (%lld)", (long long)offset));
            error = true;
        }
    }
    if ((offset % 2) != 0) {
        rep2(PRI_ERROR, "odd-branch", iname_n->src, iname_n->s, iname_n->e, afmt("Instruction '%s' can only jump by an even offset", iname),
             operand->src, operand->s, operand->e, afmt("...but this offset is odd (%lld, in particular)", (long long)offset));
        error = true;
    }
    if (error) { out->val = 0; return; }
    out->val = uns ? (-offset) / 2 : offset / 2;
}

static void enc_imm(const OperandDef *d, Node *operand, State *st, Node *insn, bool layout, Enc *out) {
    out->has_extra = false;
    out->val = 0;
    const char *iname = insn->u.insn.name->u.sym.name;
    Node *iname_n = insn->u.insn.name;
    if (is_op(operand, OP_IMMEDIATE)) {
        WARN1("excess-hash", operand->src, operand->s, operand->e, "'%s' instruction takes an immediate value implicitly, a hash is unnecessary", iname);
        operand = operand->u.op.rhs;
    }
    if (layout) return;
    int bits = d->nbits;
    int undef0 = g_undef, h0 = g_cl_hits;
    if (g_cl_mode) g_cl_allow = true;
    Value v = eval(operand, st);
    g_cl_allow = false;
    if (g_cl_mode && g_cl_hits != h0) {
        if (bits == 8) cl_record(st, operand, RT_BYTE, st->emit_address);
        else ERR1("object-unsupported", operand->src, operand->s, operand->e, "A symbol in this operand cannot be stored in an object file");
        return;
    }
    if (v.t != V_INT) internal_error("TypeError: '<' not supported between instances of 'str' and 'int'");
    if (g_undef != undef0) return;
    int64_t value = v.i;
    if (d->unsigned_ && value < 0) {
        rep2(PRI_ERROR, "value-out-of-bounds", iname_n->src, iname_n->s, iname_n->e, afmt("Instruction '%s' takes a non-negative immediate operand", iname),
             operand->src, operand->s, operand->e, afmt("...but this value is negative (exactly %lld)", (long long)value));
        return;
    }
    int64_t min_v = d->unsigned_ ? 0 : -((int64_t)1 << bits) + 1;
    int64_t max_v = ((int64_t)1 << bits) - 1;
    if (!(min_v <= value && value <= max_v)) {
        rep2(PRI_ERROR, "value-out-of-bounds", iname_n->src, iname_n->s, iname_n->e, afmt("Instruction '%s' takes an immediate operand from %lld to %lld (inclusively)", iname, (long long)min_v, (long long)max_v),
             operand->src, operand->s, operand->e, afmt("...but this value is out of bounds (%lld)", (long long)value));
        return;
    }
    int64_t mod = (int64_t)1 << bits;
    int64_t r = value % mod; if (r < 0) r += mod;
    out->val = r;
}

void compile_machine_insn(const Builtin *b, Node *insn, State *st) {
    const InsnDef *def = b->insn;
    size_t nops = insn->u.insn.ops.n;
    if (nops < def->nops) {
        ERR1("wrong-operands", insn->src, insn->s, insn->e, "Too few operands for '%s' instruction: %d expected, %zu given", def->name, def->nops, nops);
        return;
    }
    if (nops > def->nops) {
        ERR1("wrong-operands", insn->src, insn->s, insn->e, "Too many operands for '%s' instruction: %d expected, %zu given", def->name, def->nops, nops);
        return;
    }
    bool layout = IS_LAYOUT();
    uint16_t opcode = def->base;
    uint16_t extras[3]; int nextra = 0;
    int64_t extra_bytes = 0;
    for (int i = 0; i < def->nops; i++) {
        const OperandDef *d = &def->ops[i];
        Node **slot = (Node **)&insn->u.insn.ops.a[i];
        Node *operand = *slot;
        int64_t rel = st->emit_address + 2 + extra_bytes;
        Enc e = {0};
        switch (d->kind) {
        case OPK_REG: {
            int r = try_as_register(operand, st, !layout);
            if (r < 0) {
                rep2(PRI_ERROR, "invalid-addressing", insn->src, insn->s, insn->e, afmt("Instruction '%s' accepts a register as the %s argument", insn->u.insn.name->u.sym.name, ordinal((char)d->pchar)),
                     operand->src, operand->s, operand->e, "...but this value does not look like a register");
                recoverable_error();
            }
            e.val = r;
            break;
        }
        case OPK_RM: enc_rm(slot, st, rel, layout, &e); break;
        case OPK_FPACC: {
            int acc = try_accumulator(operand);
            if (acc < 0) {
                rep2(PRI_ERROR, "invalid-addressing", insn->src, insn->s, insn->e, afmt("'%s' FP11 instruction accepts an accumulator as the %s argument", insn->u.insn.name->u.sym.name, ordinal((char)d->pchar)),
                     operand->src, operand->s, operand->e, "...but this value does not look like a accumulator.\nThe supported accumulator names are 'ac0' to 'ac5'.");
                recoverable_error();
            }
            e.val = acc;
            break;
        }
        case OPK_FPRM: {
            int acc = try_accumulator(operand);
            if (acc >= 0) { e.val = acc; break; }
            int r = as_register_static(operand);
            if (r >= 0) {
                if (r == 100) r = try_as_register(operand, st, !layout);
                rep1(r < 6 ? PRI_WARNING : PRI_ERROR, "implicit-accumulator", operand->src, operand->s, operand->e,
                     afmt("This FP11 instruction takes either an accumulator, or any CPU addressing mode except simple register for this operand.\n%s will be implicitly treated as ac%d in this context -- please use the latter mnemonic for clarity.%s", node_repr_str(operand), r,
                          r < 6 ? "" : "\nMoreover, accumulator ac{register} does not exist, because only accumulators ac0 to ac5 exist."));
                e.val = r;
                break;
            }
            enc_rm(slot, st, rel, layout, &e);
            break;
        }
        case OPK_OFF: enc_off(d, operand, st, insn, rel, layout, &e); break;
        case OPK_IMM: enc_imm(d, operand, st, insn, layout, &e); break;
        }
        for (int k = 0; k < d->nbits; k++)
            if ((e.val >> k) & 1) opcode |= (uint16_t)(1u << d->pos[k]);
        if (e.has_extra) { extras[nextra++] = e.extra; extra_bytes += 2; }
    }
    emit_byte(opcode & 0xFF); emit_byte(opcode >> 8);
    for (int i = 0; i < nextra; i++) { emit_byte(extras[i] & 0xFF); emit_byte(extras[i] >> 8); }
}
