/* Port of pdpy11/metacommands.py + metacommand_impl.py */
#include "comp.h"
#include "platform.h"
#include <stdio.h>
#include <errno.h>

char *u32_to_utf8(const U32 *s) {
    Buf b = {0};
    for (size_t i = 0; i < s->n; i++) buf_put_utf8(&b, s->cp[i]);
    char *r = astrndup(b.p ? b.p : "", b.len);
    buf_free(&b);
    return r;
}
U32 utf8_to_u32(const char *s, size_t n) {
    U32 r; r.cp = (uint32_t *)amalloc((n + 1) * 4); r.n = 0;
    for (size_t i = 0; i < n;) r.cp[r.n++] = u8_next(s, n, &i);
    return r;
}

void st_odd_address_error(State *st, const char *what) {
    Node *in = st->insn;
    ERR1("odd-address", in->src, in->s, in->e, "This %s was emitted on an odd address.\nThe pointer was automatically adjusted one byte forward by inserting a null byte.\nThis may break labels and address calculation in your program;\nplease add '.even' or '.byte 0' where necessary.", what);
}

void compile_word_list(Node *insn, PVec *words, State *st) {
    bool layout = IS_LAYOUT();
    size_t n = words->n;
    uint16_t *vals = (uint16_t *)xmalloc(n * 2 + 2);
    int64_t adj = (st->emit_address % 2 != 0 && (st->emit_address % 2 + 2) % 2 == 1) ? 1 : 0;
    if (!layout) for (size_t i = 0; i < n; i++) vals[i] = (uint16_t)get_as_int_ref(st, "implicit word", insn, (Node *)words->a[i], 16, false, false, 0, RT_WORD, st->emit_address + adj + 2 * (int64_t)i);
    if (st->emit_address % 2 != 0 && (st->emit_address % 2 + 2) % 2 == 1) {
        emit_byte(0);
        st_odd_address_error(st, "word list");
    }
    for (size_t i = 0; i < n; i++) { emit_byte(layout ? 0 : (vals[i] & 0xFF)); emit_byte(layout ? 0 : (vals[i] >> 8)); }
    free(vals);
}

static bool addr_odd(const State *st) { return ((st->emit_address % 2) + 2) % 2 == 1; }

/* ---- file helpers ---- */
static int file_kind_check(const char *path) { return pf_file_kind(path); }

/* read whole file in binary mode; returns NULL with *kind set on failure */
static unsigned char *read_binary(const char *path, size_t *n, int *kind) {
    int k = file_kind_check(path);
    if (k) { *kind = k; return NULL; }
    FILE *f = pf_fopen(path, "rb");
    if (!f) { *kind = errno == ENOENT ? 1 : 3; return NULL; }
    size_t cap = 1 << 16, len = 0;
    unsigned char *b = (unsigned char *)xmalloc(cap);
    for (;;) {
        size_t r = fread(b + len, 1, cap - len, f);
        len += r;
        if (len < cap) break;
        cap *= 2; b = (unsigned char *)xrealloc(b, cap);
    }
    bool err = ferror(f);
    fclose(f);
    if (err) { free(b); *kind = 3; return NULL; }
    *n = len;
    return b;
}

static void report_io(State *st, int kind, const char *path, const char *extra) {
    Node *in = st->insn;
    if (kind == 1) ERR1("io-error", in->src, in->s, in->e, "There is no file at path '%s'. Double-check file paths?", path);
    else if (kind == 2) ERR1("io-error", in->src, in->s, in->e, "The file at path '%s' is a directory.", path);
    else if (kind == 4) ERR1("io-error", in->src, in->s, in->e, "Source file '%s' is not in UTF-8:\n%s", path, extra);
    else ERR1("io-error", in->src, in->s, in->e, "Could not read file at path '%s'.", path);
}

/* ---- string encoding helpers ---- */
static void ascii_impl(State *st, Node *string, Buf *result) {
    PVec single = {0};
    PVec *chunks;
    if (string->kind == N_STRCAT) chunks = &string->u.cat.chunks;
    else { pvec_push(&single, string); chunks = &single; }
    Node *in = st->insn;
    for (size_t i = 0; i < chunks->n; i++) {
        Node *chunk = (Node *)chunks->a[i];
        if (chunk->kind == N_ANGLE) {
            buf_putc(result, (int)get_as_int(st, "byte character", chunk, chunk->u.angle.expr, 8, true, true, 0));
        } else {
            U32 s = get_as_str(st, afmt("%s operand", in->u.insn.name->u.sym.name), in, chunk);
            Buf tmp = {0}; char *err;
            if (cs_encode(&G.charset, &s, &tmp, &err)) buf_putn(result, tmp.p, tmp.len);
            else ERR1("invalid-character", in->src, in->s, in->e, "Cannot encode this string using the selected output charset:\n%s\nYou can change the charset using --charset CLI argument or '.charset' directive.", err);
            buf_free(&tmp);
        }
    }
    free(single.a);
}

static char *path_for_emit(State *st, Node *insn, bool have, U32 path, const char *ext) {
    (void)insn;
    if (have) return resolve_relative_path(u32_to_utf8(&path), st->fs->filename);
    const char *fn = st->fs->filename;
    size_t l = strlen(fn);
    if (l >= 4 && streq_ci(fn + l - 4, ".mac")) l -= 4;
    char *r = (char *)amalloc(l + (ext ? strlen(ext) + 2 : 1));
    memcpy(r, fn, l); r[l] = 0;
    if (ext) { strcat(r, "."); strcat(r, ext); }
    return r;
}

static void add_emitted(State *st, int fmt, const char *path, const unsigned char *bk) {
    Emitted *e = (Emitted *)amalloc(sizeof(Emitted));
    e->src = st->insn->src; e->s = st->insn->s; e->e = st->insn->e;
    e->fmt = fmt; e->path = path; e->final_addr = -1;
    if (bk) memcpy(e->bkname, bk, 16);
    pvec_push(&G.emitted, e);
}

static void add_emitted_wav(State *st, bool have_path, U32 opath, bool have_name, U32 bkname, int fmt) {
    Node *in = st->insn;
    const char *insn_name = in->u.insn.name->u.sym.name;
    const char *write_path;
    if (have_path) write_path = resolve_relative_path(u32_to_utf8(&opath), st->fs->filename);
    else {
        const char *fn = st->fs->filename;
        size_t l = strlen(fn);
        if (l >= 4 && streq_ci(fn + l - 4, ".mac")) l -= 4;
        char *r = (char *)amalloc(l + 5);
        memcpy(r, fn, l); strcpy(r + l, ".wav");
        write_path = r;
    }
    unsigned char name16[16];
    memset(name16, ' ', 16);
    Buf enc = {0}; char *err;
    if (!have_name) {
        const char *slash = strrchr(write_path, '/');
        const char *base = slash ? slash + 1 : write_path;
        size_t bl = strlen(base);
        if (bl >= 4 && streq_ci(base + bl - 4, ".wav")) bl -= 4;
        U32 bu = utf8_to_u32(base, bl);
        if (!cs_encode(&G.charset, &bu, &enc, &err)) internal_error(err);
        if (enc.len > 16) {
            ERR1("too-long-string", in->src, in->s, in->e, "The BK tape file has a header that contains a 16-byte filename.\nThe filename for this file was inferred from the output file to be '%s',\nwhich does not fit in 16 bytes. You can set the filename manually like this:\n%s 'output.wav', 'bk file name'\nChanging the output charset may help too, if the filename contains non-ASCII characters.", afmt("%.*s", (int)bl, base), insn_name);
            enc.len = 16;
        }
    } else {
        if (!cs_encode(&G.charset, &bkname, &enc, &err)) internal_error(err);
        if (enc.len > 16) {
            ERR1("too-long-string", in->src, in->s, in->e, "The BK filename does not fit in 16 bytes ('%s').\nPlease truncate the filename. Changing the output charset may help too, if the filename contains non-ASCII characters.", u32_to_utf8(&bkname));
            enc.len = 16;
        }
    }
    if (enc.len) memcpy(name16, enc.p, enc.len);
    buf_free(&enc);
    add_emitted(st, fmt, write_path, name16);
}

typedef struct { Value v; } Cooked;

void compile_meta_insn(const Builtin *b, Node *insn, State *st) {
    const MetaDef *m = b->meta;
    bool layout = IS_LAYOUT();
    PVec ops = {0};
    for (size_t i = 0; i < insn->u.insn.ops.n; i++) pvec_push(&ops, insn->u.insn.ops.a[i]);
    Node *block = NULL;
    const char *iname = insn->u.insn.name->u.sym.name;

    if (m->takes_block) {
        if (ops.n && ((Node *)ops.a[ops.n - 1])->kind == N_BLOCK) block = (Node *)ops.a[--ops.n];
        else {
            ERR1("wrong-meta-operands", insn->src, insn->s, insn->e, "Metacommand '%s' expects a code block, but it was not passed", iname);
            recoverable_error();
        }
    }
    {
        const char *plural_max = m->max_ops >= 2 ? "s" : "", *plural_min = m->min_ops >= 2 ? "s" : "";
        const char *expectation;
        if (m->min_ops == m->max_ops) expectation = afmt("%d operand%s", m->max_ops, plural_max);
        else if (m->max_ops == INT_MAX) expectation = afmt("at least %d operand%s", m->min_ops, plural_min);
        else expectation = afmt("from %d to %d operand%s", m->min_ops, m->max_ops, plural_max);
        if ((int)ops.n < m->min_ops) {
            ERR1("wrong-meta-operands", insn->src, insn->s, insn->e, "Too few operands passed to '%s': %zu passed, %s expected", iname, insn->u.insn.ops.n, expectation);
            recoverable_error();
        } else if ((int)ops.n > m->max_ops) {
            ERR1("wrong-meta-operands", insn->src, insn->s, insn->e, "Too many operands passed to '%s': %zu passed, %s expected", iname, insn->u.insn.ops.n, expectation);
            recoverable_error();
        }
    }
    for (size_t i = 0; i < ops.n; i++) {
        Node *o = (Node *)ops.a[i];
        if (o->kind == N_PREFIX && o->u.op.op->id == OP_IMMEDIATE) {
            rep2(PRI_ERROR, "excess-hash", o->src, o->s, o->e,
                 afmt("Unexpected immediate value in '%s' metacommand.\nYou wrote '%s', you probably meant '%s', proceeding under that assumption", m->name, node_text(o), node_text(o->u.op.rhs)),
                 insn->u.insn.name->src, insn->u.insn.name->s, insn->u.insn.name->e, "Metacommand started here");
            ops.a[i] = o->u.op.rhs;
        }
    }

    /* ---- cooking (outside the try block of the original) ---- */
    int64_t ivals[1] = {0};
    int64_t *vlist = NULL;                    /* variadic int operands */
    U32 svals[2] = {{0}, {0}};
    size_t nstr = 0;
    bool needs_cook = !m->raw;
    switch (m->id) {                          /* data directives: operand values are not needed to know the size */
    case M_BYTE: case M_WORD: case M_DWORD: needs_cook = !layout; break;
    case M_MAKE_BIN: case M_MAKE_ROM: case M_MAKE_RAW: case M_MAKE_SAV: case M_MAKE_WAV: case M_MAKE_TURBO_WAV: case M_PLAY_WAV: case M_PLAY_TURBO_WAV: needs_cook = !layout; break;
    case M_ERROR: case M_LIST: case M_NLIST: case M_TITLE: case M_SBTTL: case M_IDENT: case M_PAGE: needs_cook = !layout; break;
    default: break;
    }
    if (needs_cook) {
        vlist = (int64_t *)xmalloc((ops.n + 1) * sizeof(int64_t));
        for (size_t i = 0; i < ops.n; i++) {
            const OpInfo *oi = &m->info[(int)i < m->nopinfo ? (int)i : m->nopinfo - 1];
            if (oi->type == OT_STR) {
                U32 s = get_as_str(st, oi->name, st->insn, (Node *)ops.a[i]);
                if (nstr < 2) svals[nstr++] = s;
            } else if (oi->type == OT_INT) {
                if (m->id == M_BYTE || m->id == M_WORD) {
                    int64_t w = m->id == M_WORD ? 2 : 1;
                    int64_t a0 = st->emit_address + (m->id == M_WORD && addr_odd(st) ? 1 : 0) + w * (int64_t)i;
                    vlist[i] = get_as_int_ref(st, oi->name, st->insn, (Node *)ops.a[i], oi->bitness, oi->unsigned_, false, 0, m->id == M_WORD ? RT_WORD : RT_BYTE, a0);
                } else
                vlist[i] = get_as_int(st, oi->name, st->insn, (Node *)ops.a[i], oi->bitness, oi->unsigned_, false, 0);
            }
        }
        if (ops.n) ivals[0] = vlist[0];
    }

    /* ---- handler under the RecoverableError catcher ---- */
    jmp_buf jb;
    jmp_buf *saved = g_recov;
    jmp_buf *saved_stop = g_stop;
    jmp_buf *saved_item = g_item;
    size_t out_mark = G.out.len;
    g_recov = &jb;
    if (setjmp(jb) != 0) {
        g_recov = saved;
        g_stop = saved_stop; g_item = saved_item;
        G.out.len = out_mark;
        free(vlist); free(ops.a);
        return;
    }
    switch (m->id) {
    case M_BYTE:
        if (ops.n == 0) {
            WARN1("implicit-operand", insn->src, insn->s, insn->e, "'.byte' without an operand is implicitly treated as '.byte 0'.\nPlease consider inserting the zero explicitly.");
            emit_byte(0);
        } else for (size_t i = 0; i < ops.n; i++) emit_byte(layout ? 0 : (int)vlist[i]);
        break;
    case M_WORD: case M_DWORD: {
        int w = m->id == M_WORD ? 2 : 4;
        if (addr_odd(st)) {
            emit_byte(0); st_odd_address_error(st, m->id == M_WORD ? "'.word'" : "'.dword'");
        }
        if (ops.n == 0) {
            WARN1("implicit-operand", insn->src, insn->s, insn->e, "'%s' without an operand is implicitly treated as '%s 0'.\nPlease consider inserting the zero explicitly.", m->name, m->name);
            emit_zeros((size_t)w);
        } else for (size_t i = 0; i < ops.n; i++) {
            uint64_t v = layout ? 0 : (uint64_t)vlist[i];
            if (w == 2) { emit_byte((int)(v & 0xFF)); emit_byte((int)((v >> 8) & 0xFF)); }
            else { emit_byte((int)((v >> 16) & 0xFF)); emit_byte((int)((v >> 24) & 0xFF)); emit_byte((int)(v & 0xFF)); emit_byte((int)((v >> 8) & 0xFF)); }
        }
        break;
    }
    case M_ASCII: case M_ASCIZ: {
        Buf r = {0};
        ascii_impl(st, (Node *)ops.a[0], &r);
        if (m->id == M_ASCIZ) buf_putc(&r, 0);
        emit_bytes(r.p, r.len);
        buf_free(&r);
        break;
    }
    case M_RAD50: {
        Node *string = (Node *)ops.a[0];
        PVec single = {0};
        PVec *chunks;
        if (string->kind == N_STRCAT) chunks = &string->u.cat.chunks; else { pvec_push(&single, string); chunks = &single; }
        int *chars = NULL; size_t nc = 0, cap = 0;
        for (size_t i = 0; i < chunks->n; i++) {
            Node *chunk = (Node *)chunks->a[i];
            if (chunk->kind == N_ANGLE) {
                int64_t val = get_as_int(st, "radix-50 character", chunk, chunk->u.angle.expr, 0, true, true, 0);
                if (val >= 40) {
                    ERR1("value-out-of-bounds", chunk->src, chunk->s, chunk->e, "Character %lld cannot be packed into a radix-50 word.\nA value in range [0; 50) is expected (octal).", (long long)val);
                    val = 0;
                }
                if (nc == cap) { cap = cap ? cap * 2 : 16; chars = (int *)xrealloc(chars, cap * sizeof(int)); }
                chars[nc++] = (int)val;
            } else {
                U32 s = get_as_str(st, "'.rad50' operand", st->insn, chunk);
                for (size_t k = 0; k < s.n; k++) {
                    uint32_t ch = s.cp[k];
                    if (ch >= 'a' && ch <= 'z') ch -= 32;
                    int val = 0;
                    const char *q = (ch < 128 && ch != 0) ? strchr(RADIX50_TABLE, (int)ch) : NULL;
                    if (q) val = (int)(q - RADIX50_TABLE);
                    else {
                        Buf cb = {0}; buf_put_utf8(&cb, s.cp[k]);
                        ERR1("invalid-character", chunk->src, chunk->s, chunk->e, "Character '%s' cannot be converted using radix-50.\nThe supported characters are: ' ABCDEFGHIJKLMNOPQRSTUVWXYZ$.%%0123456789'.", cb.p);
                        buf_free(&cb);
                    }
                    if (nc == cap) { cap = cap ? cap * 2 : 16; chars = (int *)xrealloc(chars, cap * sizeof(int)); }
                    chars[nc++] = val;
                }
            }
        }
        while (nc % 3) { if (nc == cap) { cap = cap ? cap * 2 : 16; chars = (int *)xrealloc(chars, cap * sizeof(int)); } chars[nc++] = 0; }
        for (size_t i = 0; i < nc; i += 3) {
            unsigned w = (unsigned)(chars[i] * 1600 + chars[i + 1] * 40 + chars[i + 2]);
            emit_byte((int)(w & 0xFF)); emit_byte((int)((w >> 8) & 0xFF));
        }
        free(chars); free(single.a);
        break;
    }
    case M_BLKB: emit_zeros((size_t)ivals[0]); break;
    case M_BLKW: emit_zeros((size_t)ivals[0] * 2); break;
    case M_EVEN: if (addr_odd(st)) emit_byte(0); break;
    case M_ODD: if (!addr_odd(st)) emit_byte(0); break;
    case M_ALIGN: {
        int64_t cnt = ivals[0];
        if (cnt == 0) break;   /* nothing to align (Python version crashed here) */
        if (cnt == 1) { if (!addr_odd(st)) emit_byte(0); break; }   /* align to an odd address, like .odd */
        int64_t a = -st->emit_address;
        int64_t r = a % cnt; if (r < 0) r += cnt;
        emit_zeros((size_t)r);
        break;
    }
    case M_REPEAT: {
        int64_t addr = st->emit_address;
        for (int64_t i = 0; i < ivals[0]; i++) {
            size_t before = G.out.len;
            State copy = *st;
            walk_block(&copy, block, addr, true);
            addr += (int64_t)(G.out.len - before);
        }
        break;
    }
    case M_ERROR: {
        char *e = nstr ? u32_to_utf8(&svals[0]) : NULL;
        if (!layout) ERR1("user-error", insn->src, insn->s, insn->e, "Error%s%s", (e && *e) ? ": " : "", (e && *e) ? e : "");
        break;
    }
    case M_LIST: case M_NLIST: case M_TITLE: case M_SBTTL: case M_IDENT: case M_PAGE:
        WARN1("not-implemented", insn->src, insn->s, insn->e, "%s metacommand is not supported by pdpy", m->name);
        break;
    case M_INSERT_FILE: {
        char *path = resolve_relative_path(u32_to_utf8(&svals[0]), st->fs->filename);
        size_t n; int kind = 0;
        unsigned char *d = read_binary(path, &n, &kind);
        if (!d) { if (!layout) report_io(st, kind, path, NULL); break; }
        emit_bytes(d, n);
        free(d);
        break;
    }
    case M_MAKE_BIN: case M_MAKE_ROM:
        if (!layout) add_emitted(st, 0, path_for_emit(st, insn, nstr > 0, svals[0], "bin"), NULL);
        break;
    case M_MAKE_RAW:
        if (!layout) add_emitted(st, 1, path_for_emit(st, insn, nstr > 0, svals[0], NULL), NULL);
        break;
    case M_MAKE_SAV:
        if (!layout) {
            add_emitted(st, 4, path_for_emit(st, insn, nstr > 0, svals[0], "sav"), NULL);
            if (ops.n > 1) ((Emitted *)G.emitted.a[G.emitted.n - 1])->final_addr = vlist[1];
        }
        break;
    case M_PLAY_WAV: case M_PLAY_TURBO_WAV:
        if (!layout) add_emitted_wav(st, false, svals[0], nstr > 0, svals[0], m->id == M_PLAY_WAV ? 5 : 6);
        break;
    case M_MAKE_WAV: case M_MAKE_TURBO_WAV:
        if (!layout) add_emitted_wav(st, nstr > 0, svals[0], nstr > 1, svals[1], m->id == M_MAKE_WAV ? 2 : 3);
        break;
    case M_LINK:
        set_link_address((Node *)ops.a[0], st);
        break;
    case M_INCLUDE: {
        char *path = resolve_relative_path(u32_to_utf8(&svals[0]), st->fs->filename);
        IncCache *ic = (IncCache *)insn->u.insn.aux;
        if (!ic || strcmp(ic->path, path) != 0) {
            ic = (IncCache *)amalloc(sizeof(IncCache));
            ic->path = astrdup(path);
            int kind = 0; char *err = NULL;
            if (parsed_include_failed(path)) { g_error_condition = true; unrecoverable(); }   /* reported while looking for macros */
            FileAst *early = parsed_include_lookup(path);
            Src *src = early ? early->src : src_load(path, ic->path, &kind, &err);
            if (early) ic->ast = early;
            else if (!src) { static const int MAPK[4] = {3, 4, 2, 1}; ic->fail_kind = MAPK[kind & 3]; ic->fail_msg = err; }
            else {
                int saved_silent = g_silent; g_silent = 0;
                warn_source_encoding(src);
                ic->ast = parse_file(src);
                g_silent = saved_silent;
            }
            insn->u.insn.aux = ic;
        }
        if (ic->fail_kind) { if (!layout) report_io(st, ic->fail_kind, path, ic->fail_msg); break; }
        compile_include_file(ic->ast, st, &ic->lb);
        break;
    }
    case M_EXTERN:
        for (size_t i = 0; i < ops.n; i++) {
            Node *sym = (Node *)ops.a[i];
            if (sym->kind != N_SYMBOL) {
                ERR1("meta-type-mismatch", sym->src, sym->s, sym->e, "A symbol name is expected as an argument to '.extern' directive");
                continue;
            }
            if (strcmp(sym->u.sym.lname, "all") == 0) {
                for (size_t k = 0; k < st->fs->internal_symbols.n; k++) declare_external(sym, (const char *)st->fs->internal_symbols.a[k], st);
                st->extern_all = true;
            } else declare_external(sym, sym->u.sym.name, st);
        }
        break;
    case M_END: g_recov = saved; free(vlist); free(ops.a); stop_iteration(); return;
    case M_ONCE: {
        uintptr_t cnt = (uintptr_t)smap_get(&G.times_compiled, st->fs->filename, strlen(st->fs->filename));
        if (cnt > 1) { g_recov = saved; free(vlist); free(ops.a); stop_iteration(); return; }
        break;
    }
    }
    g_recov = saved;
    free(vlist); free(ops.a);
}
