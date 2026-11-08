#include "objfile.h"
#include "text.h"

static void put32(Buf *b, uint32_t v) { for (int i = 0; i < 4; i++) buf_putc(b, (int)((v >> (8 * i)) & 0xFF)); }

static void put_name(Buf *b, const char *s) {                   /* 550a, count, UTF-16LE */
    size_t n = strlen(s), cnt = 0;
    Buf u = {0};
    for (size_t i = 0; i < n;) {
        unsigned c = (unsigned char)s[i];
        uint32_t cp = c; size_t k = 1;
        if (c >= 0xF0 && i + 3 < n) { cp = ((c & 7) << 18) | (((unsigned char)s[i + 1] & 63) << 12) | (((unsigned char)s[i + 2] & 63) << 6) | ((unsigned char)s[i + 3] & 63); k = 4; }
        else if (c >= 0xE0 && i + 2 < n) { cp = ((c & 15) << 12) | (((unsigned char)s[i + 1] & 63) << 6) | ((unsigned char)s[i + 2] & 63); k = 3; }
        else if (c >= 0xC0 && i + 1 < n) { cp = ((c & 31) << 6) | ((unsigned char)s[i + 1] & 63); k = 2; }
        i += k;
        if (cp >= 0x10000) { cp -= 0x10000; buf_putc(&u, (int)(0xD800 + (cp >> 10)) & 0xFF); buf_putc(&u, (int)((0xD800 + (cp >> 10)) >> 8)); cp = 0xDC00 + (cp & 0x3FF); cnt++; }
        buf_putc(&u, (int)(cp & 0xFF)); buf_putc(&u, (int)(cp >> 8)); cnt++;
    }
    put32(b, 0x550a); put32(b, (uint32_t)cnt);
    buf_putn(b, u.p, u.len);
    buf_free(&u);
}

void obj_add_label(Obj *o, const char *name, uint32_t kind, int64_t val) {
    if (o->nlab == o->caplab) { o->caplab = o->caplab ? o->caplab * 2 : 16; o->lab = (ObjLabel *)xrealloc(o->lab, o->caplab * sizeof(ObjLabel)); }
    o->lab[o->nlab].name = astrdup(name); o->lab[o->nlab].kind = kind; o->lab[o->nlab].val = val;
    o->nlab++;
}

ObjRef *obj_add_ref(Obj *o, uint32_t type, uint32_t off) {
    if (o->nref == o->capref) { o->capref = o->capref ? o->capref * 2 : 16; o->ref = (ObjRef *)xrealloc(o->ref, o->capref * sizeof(ObjRef)); }
    ObjRef *r = &o->ref[o->nref++];
    memset(r, 0, sizeof *r);
    r->type = type; r->off = off;
    return r;
}

Buf obj_serialize(const Obj *o) {
    Buf b = {0};
    put32(&b, 0x5501); put32(&b, 0x5502);
    put32(&b, 8); put32(&b, 1); put32(&b, 0); put32(&b, 0x1020);
    put32(&b, (o->link_addr & 0xFFFF) | (o->flags << 16));
    if (o->has_code) {
        put32(&b, 0x5501); put32(&b, 0x5503);
        put32(&b, (uint32_t)o->code.len); put32(&b, 1); put32(&b, 0);
        buf_putn(&b, o->code.p, o->code.len);
    }
    if (o->nlab) {
        Buf s = {0};
        for (size_t i = 0; i < o->nlab; i++) {
            put32(&s, 0x5508); put32(&s, o->lab[i].kind); put32(&s, (uint32_t)o->lab[i].val);
            put_name(&s, o->lab[i].name);
        }
        put32(&b, 0x5501); put32(&b, 0x5504);
        put32(&b, (uint32_t)s.len); put32(&b, (uint32_t)o->nlab); put32(&b, 0);
        buf_putn(&b, s.p, s.len);
        buf_free(&s);
    }
    if (o->nref) {
        Buf s = {0};
        for (size_t i = 0; i < o->nref; i++) {
            const ObjRef *r = &o->ref[i];
            put32(&s, 0x5509); put32(&s, r->type); put32(&s, r->off);
            put_name(&s, "");
            put32(&s, 0x5510); put32(&s, (uint32_t)r->ntok);
            for (size_t k = 0; k < r->ntok; k++) {
                const ObjTok *t = &r->tok[k];
                put32(&s, 0x550b); buf_putc(&s, t->kind);
                switch (t->kind) {
                case OT_NUM: put32(&s, 0x550e); put32(&s, (uint32_t)t->v); break;
                case OT_SYM: put32(&s, 0x550d); put_name(&s, t->name); break;
                case OT_LOCAL: put32(&s, 0x550d); put32(&s, (uint32_t)t->v); put_name(&s, t->name); break;
                case OT_DOT: put32(&s, 0x550f); put32(&s, (uint32_t)t->v); break;
                default: put32(&s, 0x550c); buf_putc(&s, (int)t->v); break;
                }
            }
        }
        put32(&b, 0x5501); put32(&b, 0x5506);
        put32(&b, (uint32_t)s.len); put32(&b, (uint32_t)o->nref); put32(&b, 0);
        buf_putn(&b, s.p, s.len);
        buf_free(&s);
    }
    return b;
}

/* ---- reader ---- */
typedef struct { const unsigned char *d; size_t n, p; bool bad; } Rd;
static uint32_t rd32(Rd *r) {
    if (r->p + 4 > r->n) { r->bad = true; return 0; }
    uint32_t v = r->d[r->p] | (r->d[r->p + 1] << 8) | (r->d[r->p + 2] << 16) | ((uint32_t)r->d[r->p + 3] << 24);
    r->p += 4; return v;
}
static int rd8(Rd *r) { if (r->p >= r->n) { r->bad = true; return 0; } return r->d[r->p++]; }
static bool expect(Rd *r, uint32_t tag) { return rd32(r) == tag && !r->bad; }
static char *rd_name(Rd *r) {                                   /* 550a, count, UTF-16LE -> UTF-8 */
    if (!expect(r, 0x550a)) { r->bad = true; return NULL; }
    uint32_t cnt = rd32(r);
    if (r->bad || (size_t)cnt * 2 > r->n - r->p) { r->bad = true; return NULL; }
    Buf b = {0};
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t c = r->d[r->p] | (r->d[r->p + 1] << 8); r->p += 2;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < cnt) { uint32_t c2 = r->d[r->p] | (r->d[r->p + 1] << 8); r->p += 2; i++; c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); }
        buf_put_utf8(&b, c);
    }
    char *s = astrndup(b.p ? b.p : "", b.len);
    buf_free(&b);
    return s;
}

bool obj_parse(Obj *o, const unsigned char *d, size_t n, const char **err) {
    Rd r = {d, n, 0, false};
    memset(o, 0, sizeof *o);
    bool have_hdr = false;
    while (r.p < r.n) {
        if (!expect(&r, 0x5501)) { *err = "bad record tag"; return false; }
        uint32_t sec = rd32(&r);
        if (sec == 0x5502) {
            if (rd32(&r) != 8) { *err = "unsupported header"; return false; }
            rd32(&r); rd32(&r); rd32(&r);
            uint32_t la = rd32(&r);
            o->link_addr = la & 0xFFFF; o->flags = la >> 16;
            have_hdr = true;
        } else if (sec == 0x5503) {
            uint32_t len = rd32(&r); rd32(&r); rd32(&r);
            if (r.bad || len > r.n - r.p) { *err = "truncated code section"; return false; }
            o->has_code = true;
            buf_putn(&o->code, d + r.p, len);
            r.p += len;
        } else if (sec == 0x5504) {
            rd32(&r); uint32_t cnt = rd32(&r); rd32(&r);
            for (uint32_t i = 0; i < cnt && !r.bad; i++) {
                if (!expect(&r, 0x5508)) { *err = "bad label record"; return false; }
                uint32_t kind = rd32(&r), val = rd32(&r);
                char *nm = rd_name(&r);
                if (!nm) { *err = "bad label name"; return false; }
                obj_add_label(o, nm, kind, (int32_t)val);
            }
        } else if (sec == 0x5506) {
            rd32(&r); uint32_t cnt = rd32(&r); rd32(&r);
            for (uint32_t i = 0; i < cnt && !r.bad; i++) {
                if (!expect(&r, 0x5509)) { *err = "bad reference record"; return false; }
                uint32_t type = rd32(&r), off = rd32(&r);
                if (!rd_name(&r) || !expect(&r, 0x5510)) { *err = "bad reference record"; return false; }
                uint32_t nt = rd32(&r);
                if (r.bad || nt > 4096) { *err = "bad reference record"; return false; }
                ObjRef *ref = obj_add_ref(o, type, off);
                ref->tok = (ObjTok *)amalloc((nt ? nt : 1) * sizeof(ObjTok));
                ref->ntok = nt;
                for (uint32_t k = 0; k < nt; k++) {
                    if (!expect(&r, 0x550b)) { *err = "bad token"; return false; }
                    ObjTok *t = &ref->tok[k];
                    t->kind = rd8(&r);
                    switch (t->kind) {
                    case OT_NUM: if (!expect(&r, 0x550e)) { *err = "bad token"; return false; } t->v = (int32_t)rd32(&r); break;
                    case OT_SYM: if (!expect(&r, 0x550d)) { *err = "bad token"; return false; } t->name = rd_name(&r); break;
                    case OT_LOCAL: if (!expect(&r, 0x550d)) { *err = "bad token"; return false; } t->v = rd32(&r); t->name = rd_name(&r); break;
                    case OT_DOT: if (!expect(&r, 0x550f)) { *err = "bad token"; return false; } t->v = rd32(&r); break;
                    case OT_OP: if (!expect(&r, 0x550c)) { *err = "bad token"; return false; } t->v = rd8(&r); break;
                    default: *err = "unknown token kind"; return false;
                    }
                    if (r.bad || ((t->kind == OT_SYM || t->kind == OT_LOCAL) && !t->name)) { *err = "truncated token"; return false; }
                }
            }
        } else { *err = "unknown section"; return false; }
        if (r.bad) { *err = "truncated file"; return false; }
    }
    if (!have_hdr) { *err = "no header"; return false; }
    return true;
}

/* ---- linker ---- */
typedef struct { int64_t val; } LVal;

static int64_t fdiv(int64_t a, int64_t b) { int64_t q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }

static bool eval_ref(const ObjRef *r, int64_t obj_base, SMap *globals, int64_t *res, const char **bad_sym, const char **bad_msg) {
    int64_t st[64]; int sp = 0;
    *bad_sym = NULL; *bad_msg = NULL;
    for (size_t i = 0; i < r->ntok; i++) {
        const ObjTok *t = &r->tok[i];
        switch (t->kind) {
        case OT_NUM: if (sp >= 64) { *bad_msg = "expression is too complex"; return false; } st[sp++] = t->v; break;
        case OT_DOT: case OT_LOCAL: if (sp >= 64) { *bad_msg = "expression is too complex"; return false; } st[sp++] = obj_base + t->v; break;
        case OT_SYM: {
            char lo[256];
            size_t l = strlen(t->name);
            if (l >= sizeof lo) { *bad_sym = t->name; return false; }
            for (size_t k = 0; k < l; k++) { char c = t->name[k]; lo[k] = (c >= 'A' && c <= 'Z') ? c + 32 : c; }
            LVal *v = (LVal *)smap_get(globals, lo, l);
            if (!v) { *bad_sym = t->name; return false; }
            if (sp >= 64) { *bad_msg = "expression is too complex"; return false; }
            st[sp++] = v->val; break;
        }
        default: {
            int op = (int)t->v;
            if (op == OC_POS || op == OC_NEG || op == OC_INV) {
                if (sp < 1) { *bad_msg = "bad expression"; return false; }
                if (op == OC_NEG) st[sp - 1] = -st[sp - 1]; else if (op == OC_INV) st[sp - 1] = ~st[sp - 1];
                break;
            }
            if (sp < 2) { *bad_msg = "bad expression"; return false; }
            int64_t b = st[--sp], a = st[sp - 1], v = 0;
            switch (op) {
            case OC_ADD: v = a + b; break;
            case OC_SUB: v = a - b; break;
            case OC_MUL: v = a * b; break;
            case OC_DIV: if (!b) { *bad_msg = "division by zero"; return false; } v = fdiv(a, b); break;
            case OC_MOD: if (!b) { *bad_msg = "division by zero"; return false; } v = a - fdiv(a, b) * b; break;
            case OC_SHL: v = b >= 0 ? (b > 62 ? 0 : a * ((int64_t)1 << b)) : (-b > 62 ? (a < 0 ? -1 : 0) : fdiv(a, (int64_t)1 << -b)); break;
            case OC_SHR: v = b >= 0 ? (b > 62 ? (a < 0 ? -1 : 0) : fdiv(a, (int64_t)1 << b)) : (-b > 62 ? 0 : a * ((int64_t)1 << -b)); break;
            case OC_OR: v = a | b; break;
            case OC_XOR: v = a ^ b; break;
            case OC_AND: v = a & b; break;
            default: *bad_msg = "unknown operator"; return false;
            }
            st[sp - 1] = v;
        }
        }
    }
    if (sp != 1) { *bad_msg = "bad expression"; return false; }
    *res = st[0];
    return true;
}

bool obj_link(Obj *objs, size_t n, bool have_base, int64_t base, Buf *out, int64_t *out_base, Obj *table, PVec *errs) {
    bool ok = true;
    SMap globals = {0};
    int64_t *obase = (int64_t *)amalloc((n ? n : 1) * sizeof(int64_t));
    int64_t start = have_base ? base : (n ? objs[0].link_addr : 0x200);
    int64_t cur = start;
    for (size_t i = 0; i < n; i++) {                       /* layout and label table */
        Obj *o = &objs[i];
        bool absolute = (o->flags & 2) != 0;
        obase[i] = absolute ? o->link_addr : cur;
        for (size_t k = 0; k < o->nlab; k++) {
            ObjLabel *l = &o->lab[k];
            int64_t v = l->kind == 3 ? l->val : obase[i] + l->val;
            char lo[256];
            size_t nl = strlen(l->name);
            if (nl >= sizeof lo) continue;
            for (size_t c = 0; c < nl; c++) { char ch = l->name[c]; lo[c] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; }
            bool created;
            void **slot = smap_slot(&globals, lo, nl, &created);
            if (!created) { pvec_push(errs, afmt("%s: label '%s' is already defined", o->path, l->name)); ok = false; continue; }
            LVal *lv = (LVal *)amalloc(sizeof(LVal)); lv->val = v; *slot = lv;
        }
        if (o->has_code) { buf_putn(out, o->code.p ? o->code.p : "", o->code.len); cur += (int64_t)o->code.len; }
    }
    for (size_t i = 0; i < n; i++) {                       /* references */
        Obj *o = &objs[i];
        if ((o->flags & 2) != 0 || !o->has_code) continue;
        size_t cbase = (size_t)(obase[i] - start);
        for (size_t k = 0; k < o->nref; k++) {
            const ObjRef *r = &o->ref[k];
            size_t at = cbase + r->off;
            int64_t v; const char *bs, *bm;
            if (!eval_ref(r, obase[i], &globals, &v, &bs, &bm)) {
                if (bs) pvec_push(errs, afmt("%s: undefined label '%s' (offset %05o)", o->path, bs, (unsigned)(start + at)));
                else pvec_push(errs, afmt("%s: %s (offset %05o)", o->path, bm, (unsigned)(start + at)));
                ok = false; continue;
            }
            if (r->off >= o->code.len || at + 1 > out->len) { pvec_push(errs, afmt("%s: reference outside of the module", o->path)); ok = false; continue; }
            int64_t addr = start + (int64_t)at;
            if (r->type == RT_BYTE) {
                out->p[at] = (char)(v & 0xFF);                /* like Turbo8, the low byte is stored without a range check */
            } else if (r->type == RT_BRANCH) {
                int64_t d = v - (addr + 2);
                if ((d & 1) || d < -256 || d > 254) { pvec_push(errs, afmt("%s: branch at %05o cannot reach its target (distance %lld)", o->path, (unsigned)addr, (long long)d)); ok = false; continue; }
                out->p[at] = (char)((d / 2) & 0xFF);
            } else if (at + 2 <= out->len) {
                int64_t w = r->type == RT_REL ? v - (addr + 2) : v;
                out->p[at] = (char)(w & 0xFF); out->p[at + 1] = (char)((w >> 8) & 0xFF);
            } else { pvec_push(errs, afmt("%s: reference outside of the module", o->path)); ok = false; }
        }
    }
    *out_base = start;
    if (table) {
        memset(table, 0, sizeof *table);
        table->link_addr = (uint32_t)start; table->flags = 2;
        for (size_t i = 0; i < n; i++)
            for (size_t k = 0; k < objs[i].nlab; k++) {
                ObjLabel *l = &objs[i].lab[k];
                int64_t v = l->kind == 3 ? l->val : obase[i] + l->val;
                obj_add_label(table, l->name, 3, v);
            }
    }
    return ok;
}
