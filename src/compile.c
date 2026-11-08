/* Port of pdpy11/compiler.py, restructured as multi-pass layout + emit (see PLAN.MD section 0.1.2) */
#include "comp.h"
#include <stdio.h>

Comp G;
jmp_buf *g_stop;


void emit_bytes(const void *p, size_t n) { if (n) buf_putn(&G.out, p, n); }
void emit_byte(int b) { buf_putc(&G.out, b); }
void emit_zeros(size_t n) {
    if (!n) return;
    buf_reserve(&G.out, n);
    memset(G.out.p + G.out.len, 0, n);
    G.out.len += n;
    G.out.p[G.out.len] = 0;
}

void stop_iteration(void) {
    if (g_stop) longjmp(*g_stop, 1);
    internal_error("stop_iteration outside of a block");
}

/* ---------- symbols ---------- */
static Sym *sym_get_or_create(const char *prefix, size_t pl, const char *name, bool *created) {
    size_t nl = strlen(name);
    char *key = (char *)amalloc(pl + nl + 1);
    memcpy(key, prefix, pl); memcpy(key + pl, name, nl); key[pl + nl] = 0;
    char *low = (char *)amalloc(pl + nl + 1);
    lower_ascii_n(low, key, pl + nl);
    bool c;
    void **slot = smap_slot(&G.symbols, low, pl + nl, &c);
    if (c) {
        Sym *s = (Sym *)amalloc(sizeof(Sym));
        s->key = key;
        *slot = s;
        pvec_push(&G.sym_order, s);
    }
    if (created) *created = c;
    return (Sym *)*slot;
}

void declare_external(Node *loc, const char *name, State *st) {
    size_t nl = strlen(name);
    char *low = (char *)amalloc(nl + 1);
    lower_ascii_n(low, name, nl);
    bool c;
    void **slot = smap_slot(&G.externs, low, nl, &c);
    ExtEnt *e = (ExtEnt *)*slot;
    if (c) e = (ExtEnt *)(*slot = amalloc(sizeof(ExtEnt)));
    else if (e->stamp == G.walk_id) {
        rep2(PRI_ERROR, "duplicate-symbol", loc->src, loc->s, loc->e, afmt("Duplicate external symbol '%s'.", name),
             e->loc->src, e->loc->s, e->loc->e, "A symbol with the same name was previously declared external here.");
        return;
    }
    e->loc = loc;
    e->stamp = G.walk_id;
    size_t pl = st->fs->internal_prefix_len;
    char *key = (char *)amalloc(pl + nl + 1);
    memcpy(key, st->fs->internal_prefix, pl); memcpy(key + pl, low, nl); key[pl + nl] = 0;
    lower_ascii_n(key, key, pl + nl);
    e->key = key;
}

static void compile_label(Node *label, int64_t addr, State *st) {
    bool local = label->u.label.local;
    const char *prefix = local ? st->local_prefix : st->fs->internal_prefix;
    bool created;
    Sym *s = sym_get_or_create(prefix, strlen(prefix), label->u.label.name, &created);
    if (!created && s->stamp == G.walk_id) {
        rep2(PRI_ERROR, "duplicate-symbol", label->src, label->s, label->e,
             afmt("Duplicate %s '%s:'", local ? "local label" : "symbol", label->u.label.name),
             s->def->src, s->def->s, s->def->e, "A symbol with the same name has been already declared here");
        return;
    }
    if (!created && (!s->is_label || s->addr != addr)) G.labels_changed = true;
    s->def = label; s->is_label = true; s->addr = addr; s->stamp = G.walk_id; s->lb = st->fs->lb;
    if (label->u.label.is_extern) declare_external(label, label->u.label.name, st);
    if (!local) {
        pvec_push(&st->fs->internal_symbols, (void *)label->u.label.name);
        if (st->extern_all) declare_external(label, label->u.label.name, st);
    }
}

static void compile_assignment(Node *as, State *st) {
    const char *name = as->u.assign.target->u.sym.name;
    bool created;
    Sym *s = sym_get_or_create(st->fs->internal_prefix, st->fs->internal_prefix_len, name, &created);
    if (!created && s->stamp == G.walk_id) {
        rep2(PRI_ERROR, "duplicate-symbol", as->src, as->s, as->e, afmt("Duplicate variable '%s'", name),
             s->def->src, s->def->s, s->def->e, "A symbol with the same name has been already declared here");
        return;
    }
    pvec_push(&st->fs->internal_symbols, (void *)name);
    s->def = as; s->is_label = false; s->stamp = G.walk_id;
    if (!s->x) s->x = (AsgExt *)amalloc(sizeof(AsgExt));
    s->x->st = *st;
    if (as->u.assign.is_extern) declare_external(as, name, st);
    if (st->extern_all) declare_external(as, name, st);
}

void set_link_address(Node *addr_expr, State *st) {
    LinkBase *lb = st->fs->lb;
    Node *in = st->insn;
    if (lb->set) {
        if (!lb->set_where)
            ERR1("address-conflict", in->src, in->s, in->e, "The link base cannot be set here because this metacommand cannot be evaluated\nbefore the link base itself is known.");
        else
            rep2(PRI_ERROR, "address-conflict", in->src, in->s, in->e, "The link base has already been set.",
                 lb->set_where->src, lb->set_where->s, lb->set_where->e, "The link base has been previously set here.");
        return;
    }
    LinkBase *saved_hit_lb = g_hit_lb; bool saved_multi = g_hit_multi, saved_on = g_track_on;
    g_hit_lb = NULL; g_hit_multi = false; g_track_on = true;
    jmp_buf jb; jmp_buf *saved_recov = g_recov; jmp_buf *saved_stop = g_stop; jmp_buf *saved_item2 = g_item; g_recov = &jb;
    int64_t v = 0;
    if (setjmp(jb) == 0) v = get_as_int(st, "link address", in, addr_expr, 16, false, false, 0);
    g_recov = saved_recov; g_stop = saved_stop; g_item = saved_item2;
    bool dep = g_hit_multi || g_hit_lb == lb;
    g_hit_lb = saved_hit_lb; g_hit_multi = saved_multi; g_track_on = saved_on;
    if (dep) {
        ERR1("recursive-definition", in->src, in->s, in->e, "The link base is mathematically equal to %s,\nwhere LA denotes link base. In other words, the link base depends on itself,\nand thus cannot be determined.", node_repr_str(addr_expr));
        v = 0;
    }
    lb->set = true; lb->set_where = in; lb->new_base = v;
    if (v != lb->used_base) G.base_changed = true;
    lb->pred = v; lb->has_pred = true;
}

/* ---------- block walk ---------- */
static void compile_insn(Node *insn, State *st) {
    const char *name = insn->u.insn.name->u.sym.name;
    size_t nl = strlen(name);
    const Builtin *b = builtin_find(name, nl);
    if (!b && nl < 90) {
        char tmp[100]; tmp[0] = '.'; memcpy(tmp + 1, name, nl);
        const Builtin *b2 = builtin_find(tmp, nl + 1);
        if (b2) {
            WARN1("meta-typo", insn->u.insn.name->src, insn->u.insn.name->s, insn->u.insn.name->e, "'%s' is not a metacommand by itself, but '.%s' is.\nPlease be explicit and add a dot.", name, name);
            b = b2;
        }
    }
    if (b) {
        if (b->insn) compile_machine_insn(b, insn, st); else compile_meta_insn(b, insn, st);
        return;
    }
    /* user symbol used as an instruction name */
    char *low = (char *)amalloc(nl + 1);
    lower_ascii_n(low, name, nl);
    Sym *s = NULL;
    {
        size_t pl = st->fs->internal_prefix_len;
        char *key = (char *)amalloc(pl + nl + 1);
        memcpy(key, st->fs->internal_prefix, pl); memcpy(key + pl, low, nl); key[pl + nl] = 0;
        lower_ascii_n(key, key, pl + nl);
        s = (Sym *)smap_get(&G.symbols, key, pl + nl);
        if (s && s->stamp != G.walk_id) s = NULL;      /* only symbols defined so far */
    }
    if (s) {
        Node *nm = insn->u.insn.name;
        if (s->is_label) {
            rep2(PRI_ERROR, "meta-type-mismatch", nm->src, nm->s, nm->e, afmt("'%s' is used as an instruction name", name),
                 s->def->src, s->def->s, s->def->e, "...but is defined as a label here. You might want to add '.word' or use macros.");
            return;
        }
        PVec words = {0};
        pvec_push(&words, nm);
        for (size_t i = 0; i < insn->u.insn.ops.n; i++) pvec_push(&words, insn->u.insn.ops.a[i]);
        if (words.n > 1) {
            Node *w1 = (Node *)words.a[1];
            if (w1->kind == N_PAREN && strcmp(w1->u.paren.open, "(") == 0) {
                Node *c = node_new(N_INFIX, nm->src, nm->s, w1->e);
                c->u.op.op = op_def(OP_CALL); c->u.op.lhs = nm; c->u.op.rhs = w1->u.paren.expr;
                words.a[0] = c;
                memmove(words.a + 1, words.a + 2, (words.n - 2) * sizeof(void *));
                words.n--;
            } else {
                Node *w0 = (Node *)words.a[0];
                rep2(PRI_ERROR, "meta-type-mismatch", w0->src, w0->s, w0->e, afmt("'%s' is used as an instruction name", name),
                     s->def->src, s->def->s, s->def->e, "...but is defined as a variable here. This would normally be interpreted as\nimplicit '.word', but the comma between the first and the second words is missing.");
            }
        }
        compile_word_list(insn, &words, st);
        free(words.a);
        return;
    }
    {
        const char *sg = builtin_suggest(name);
        ERR1("unknown-insn", insn->u.insn.name->src, insn->u.insn.name->s, insn->u.insn.name->e,
             "'%s' is an undefined %s.%s%s%s", name, name[0] == '.' ? "directive" : "instruction",
             sg ? " Did you mean '" : " Check the spelling, or define it before use (a symbol defined later cannot be used as an instruction).", sg ? sg : "", sg ? "'?" : "");
    }
}

int64_t walk_block(State *st0, Node *block, int64_t start, bool in_repeat) {
    State st = *st0;
    if (in_repeat) st.in_repeat = true;
    size_t before = G.out.len;
    int64_t addr = start;
    st.local_prefix = afmt(".local%d.", G.next_local++);
    jmp_buf jb, ij;
    jmp_buf *saved = g_stop, *saved_item = g_item, *entry_recov = g_recov;
    g_stop = &jb;
    if (setjmp(jb) == 0) {
        for (size_t i = 0; i < block->u.block.insns.n; i++) {
            Node *insn = (Node *)block->u.block.insns.a[i];
            st.insn = insn;
            st.emit_address = addr;
            g_cl_allow = g_cl_undef_ok = false;
            size_t mark = G.out.len;
            g_item = &ij;
            if (setjmp(ij) != 0) {
                /* a RecoverableError escaped from this statement: remember it, keep going so that later
                   diagnostics are still reported, and abort at the end of the walk */
                g_fatal = true;
                g_recov = entry_recov; g_stop = &jb; g_item = &ij;
                addr += (int64_t)(G.out.len - mark);
                continue;
            }
            switch (insn->kind) {
            case N_INSN:
                compile_insn(insn, &st);
                addr += (int64_t)(G.out.len - mark);
                break;
            case N_WORDLIST:
                compile_word_list(insn, &insn->u.wl.words, &st);
                addr += (int64_t)(G.out.len - mark);
                break;
            case N_LABEL:
                if (st.in_repeat) {
                    if (!insn->u.label.reported) {
                        ERR1("unexpected-symbol-definition", insn->src, insn->s, insn->e, "Labels cannot be defined inside '.repeat' loop");
                        if (!g_silent) insn->u.label.reported = true;
                    }
                    break;
                }
                compile_label(insn, addr, &st);
                if (!insn->u.label.local) st.local_prefix = afmt(".local%d.", G.next_local++);
                break;
            case N_ASSIGN:
                if (insn->u.assign.target->kind == N_IPTR) {
                    if (st.fs->lb->set) {
                        int64_t nv = get_as_int(&st, "link address", insn, insn->u.assign.value, 16, false, false, 0);
                        int64_t len = nv - addr;
                        if (len < 0) {
                            ERR1("value-out-of-bounds", insn->src, insn->s, insn->e, "The new link address is lower than the previous one: a negative skip from %lld to %lld was attempted", (long long)addr, (long long)nv);
                            recoverable_error();
                        }
                        emit_zeros((size_t)len);
                        addr += len;
                    } else set_link_address(insn->u.assign.value, &st);
                    break;
                }
                if (st.in_repeat) {
                    if (!insn->u.assign.reported) {
                        ERR1("unexpected-symbol-definition", insn->src, insn->s, insn->e, "Variables cannot be defined inside '.repeat' loop");
                        if (!g_silent) insn->u.assign.reported = true;
                    }
                    break;
                }
                compile_assignment(insn, &st);
                break;
            default:
                internal_error("unexpected node in block");
            }
        }
    }
    g_stop = saved; g_item = saved_item;
    return (int64_t)(G.out.len - before);
}

/* ---------- base prediction: look for a constant '.link N' / '. = N' at the top of a file ---------- */
static bool predict_base(Node *body, int64_t *out) {
    for (size_t i = 0; i < body->u.block.insns.n; i++) {
        Node *in = (Node *)body->u.block.insns.a[i];
        if (in->kind == N_INSN) {
            const Builtin *b = builtin_find(in->u.insn.name->u.sym.name, strlen(in->u.insn.name->u.sym.name));
            if (b && b->meta && b->meta->id == M_LINK) {
                if (in->u.insn.ops.n == 1 && ((Node *)in->u.insn.ops.a[0])->kind == N_NUMBER) {
                    int64_t v = ((Node *)in->u.insn.ops.a[0])->u.num.value;
                    if (v > -65536 && v < 65536) { *out = ((v % 65536) + 65536) % 65536; return true; }
                }
                return false;
            }
        } else if (in->kind == N_ASSIGN && in->u.assign.target->kind == N_IPTR) {
            if (in->u.assign.value->kind == N_NUMBER) {
                int64_t v = in->u.assign.value->u.num.value;
                if (v > -65536 && v < 65536) { *out = ((v % 65536) + 65536) % 65536; return true; }
            }
            return false;
        }
    }
    return false;
}

static int64_t compile_file(FileAst *f, int64_t start, LinkBase *lb) {
    uintptr_t cnt = (uintptr_t)smap_get(&G.times_compiled, f->src->path, strlen(f->src->path));
    *smap_slot(&G.times_compiled, f->src->path, strlen(f->src->path), NULL) = (void *)(cnt + 1);
    FileState *fs = (FileState *)amalloc(sizeof(FileState));
    fs->src = f->src; fs->filename = f->src->path;
    fs->internal_id = G.next_internal++;
    fs->internal_prefix = afmt(".internal%d.", fs->internal_id);
    fs->internal_prefix_len = strlen(fs->internal_prefix);
    fs->lb = lb;
    while ((int)G.internal_files.n < fs->internal_id) pvec_push(&G.internal_files, NULL);
    G.internal_files.a[fs->internal_id - 1] = (void *)f->src->path;
    State st = {0};
    st.fs = fs;
    st.emit_address = start;
    return walk_block(&st, f->body, start, false);
}

int64_t compile_include_file(FileAst *f, State *st, LinkBase *lb) {
    int64_t pred;
    if (lb->has_pred) pred = lb->pred;
    else if (predict_base(f->body, &pred)) { }
    else pred = st->emit_address;
    lb->used_base = pred;
    lb->set = false; lb->set_where = NULL;
    int64_t size = compile_file(f, pred, lb);
    if (!lb->set) {
        lb->new_base = st->emit_address;
        lb->pred = lb->new_base; lb->has_pred = true;
        if (lb->new_base != pred) G.base_changed = true;
    }
    return size;
}

/* ---------- driver ---------- */
static void reset_walk(void) {
    g_fatal = false;
    G.walk_id++;
    G.next_local = 1; G.next_internal = 1;
    memset(&G.times_compiled, 0, sizeof G.times_compiled);
    G.unresolved = 0;
    G.labels_changed = false; G.base_changed = false;
    G.out.len = 0;
    if (G.out.p) G.out.p[0] = 0;
}

int64_t g_default_link = 0x200;

static void walk_all(FileAst **files, size_t nfiles, LinkBase *lb) {
    reset_walk();
    lb->used_base = lb->has_pred ? lb->pred : g_default_link;
    lb->set = false; lb->set_where = NULL;
    int64_t addr = lb->used_base;
    cl_reset_walk(lb->used_base);
    for (size_t i = 0; i < nfiles; i++) addr += compile_file(files[i], addr, lb);
    if (!lb->set) {
        lb->new_base = g_default_link;
        lb->pred = lb->new_base; lb->has_pred = true;
        if (lb->new_base != lb->used_base) G.base_changed = true;
    }
}

bool compile_and_link(FileAst **files, size_t nfiles, int64_t *base_out) {
    LinkBase *lb = (LinkBase *)amalloc(sizeof(LinkBase));
    int64_t pb;
    if (nfiles && predict_base(files[0]->body, &pb)) { lb->pred = pb; lb->has_pred = true; }
    /* layout passes (silent) */
    g_silent++;
    int pass = 0;
    bool layout_aborted = false;
    g_layout_catch = true;
    if (setjmp(g_layout_jmp) != 0) { layout_aborted = true; g_recov = NULL; g_stop = NULL; }
    for (; !layout_aborted;) {
        walk_all(files, nfiles, lb);
        pass++;
        bool again = G.base_changed || (pass == 1 ? G.unresolved > 0 : G.labels_changed) ;
        if (pass == 1 && G.unresolved > 0) again = true;
        if (!again) break;
        if (pass >= 40) { g_silent = 0; internal_error("layout did not converge"); }
    }
    g_layout_catch = false;
    g_silent = 0;
    /* final walk with diagnostics */
    walk_all(files, nfiles, lb);
    if (g_fatal) unrecoverable();
    /* resolve all symbols in case some have not been used */
    for (size_t i = 0; i < G.sym_order.n; i++) {
        Sym *s = (Sym *)G.sym_order.a[i];
        if (!s->is_label && s->stamp == G.walk_id) eval_assignment(s);
    }
    *base_out = lb->new_base;
    return true;
}

char *generate_listing(void) {
    PVec files = {0};              /* filename in first-seen order */
    typedef struct { const char *name; int64_t v; } Ent;
    Ent **lists = NULL; size_t *counts = NULL, *caps = NULL;
    for (size_t i = 0; i < G.sym_order.n; i++) {
        Sym *s = (Sym *)G.sym_order.a[i];
        if (strncmp(s->key, ".internal", 9) != 0) continue;
        const char *rest = s->key + 9;
        int id = atoi(rest);
        const char *dot = strchr(rest, '.');
        const char *label_name = dot ? dot + 1 : "";
        const char *filename = (id >= 1 && (size_t)id <= G.internal_files.n) ? (const char *)G.internal_files.a[id - 1] : "";
        Value v = s->is_label ? (Value){V_INT, s->addr, U32_EMPTY} : eval_assignment(s);
        size_t k;
        for (k = 0; k < files.n; k++) if (strcmp((const char *)files.a[k], filename) == 0) break;
        if (k == files.n) {
            pvec_push(&files, (void *)filename);
            lists = (Ent **)xrealloc(lists, files.n * sizeof(Ent *));
            counts = (size_t *)xrealloc(counts, files.n * sizeof(size_t));
            caps = (size_t *)xrealloc(caps, files.n * sizeof(size_t));
            lists[k] = NULL; counts[k] = caps[k] = 0;
        }
        if (counts[k] == caps[k]) { caps[k] = caps[k] ? caps[k] * 2 : 16; lists[k] = (Ent *)xrealloc(lists[k], caps[k] * sizeof(Ent)); }
        /* non-integers are kept with a marker (INT64_MIN + sentinel handled below) */
        lists[k][counts[k]].name = label_name;
        lists[k][counts[k]].v = v.t == V_INT ? v.i : INT64_MIN;
        counts[k]++;
    }
    Buf out = {0};
    for (size_t k = 0; k < files.n; k++) {
        buf_puts(&out, (const char *)files.a[k]); buf_putc(&out, '\n');
        Ent *l = lists[k];
        /* sort by (value, name) */
        for (size_t i = 1; i < counts[k]; i++) {
            Ent t = l[i]; size_t j = i;
            while (j > 0 && (l[j - 1].v > t.v || (l[j - 1].v == t.v && strcmp(l[j - 1].name, t.name) > 0))) { l[j] = l[j - 1]; j--; }
            l[j] = t;
        }
        for (size_t i = 0; i < counts[k]; i++) {
            if (l[i].v == INT64_MIN) continue;
            int64_t v = l[i].v;
            char digits[32]; int nd = 0;
            uint64_t u = v < 0 ? (uint64_t)(-v) : (uint64_t)v;
            if (u == 0) digits[nd++] = '0';
            while (u) { digits[nd++] = (char)('0' + (u & 7)); u >>= 3; }
            Buf d = {0};
            if (v < 0) buf_putc(&d, 'o');
            while (nd) buf_putc(&d, digits[--nd]);
            for (size_t pad = d.len; pad < 6; pad++) buf_putc(&out, '0');
            buf_putn(&out, d.p, d.len);
            buf_free(&d);
            buf_putc(&out, ' '); buf_puts(&out, l[i].name); buf_putc(&out, '\n');
        }
        buf_putc(&out, '\n');
    }
    char *r = astrndup(out.p ? out.p : "", out.len);
    buf_free(&out);
    return r;
}
