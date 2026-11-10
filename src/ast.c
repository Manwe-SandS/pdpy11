#include "ast.h"
#include "report.h"

const OpDef OPS[OP__COUNT] = {
    [OP_POSTADD]   = {OP_POSTADD, "+", 2, 2, true, false},
    [OP_POSTSUB]   = {OP_POSTSUB, "-", 2, 2, true, false},
    [OP_POS]       = {OP_POS, "+", 1, 2, true, true},
    [OP_NEG]       = {OP_NEG, "-", 1, 2, true, true},
    [OP_INV]       = {OP_INV, "~", 1, 2, true, true},
    [OP_INV2]      = {OP_INV2, "^c", 1, 2, true, true},
    [OP_MUL]       = {OP_MUL, "*", 0, 3, true, true},
    [OP_DIV]       = {OP_DIV, "/", 0, 3, true, false},
    [OP_MOD]       = {OP_MOD, "%", 0, 3, true, false},
    [OP_ADD]       = {OP_ADD, "+", 0, 4, true, true},
    [OP_SUB]       = {OP_SUB, "-", 0, 4, true, true},
    [OP_LSHIFT]    = {OP_LSHIFT, "<<", 0, 5, true, false},
    [OP_RSHIFT]    = {OP_RSHIFT, ">>", 0, 5, true, false},
    [OP_LSH]       = {OP_LSH, "_", 0, 5, true, true},
    [OP_AND]       = {OP_AND, "&", 0, 8, true, true},
    [OP_XOR]       = {OP_XOR, "^", 0, 9, true, true},
    [OP_OR]        = {OP_OR, "|", 0, 10, true, true},
    [OP_OR2]       = {OP_OR2, "!", 0, 10, true, true},
    [OP_IMMEDIATE] = {OP_IMMEDIATE, "#", 1, 15, true, false},
    [OP_DEFERRED]  = {OP_DEFERRED, "@", 1, 15, true, false},
    [OP_REGISTER]  = {OP_REGISTER, "%", 1, 15, true, false},
    [OP_CALL]      = {OP_CALL, "$", 0, 1, false, false},
};
const int OP_NUM = OP__COUNT;
const OpDef *op_def(OpId id) { return &OPS[id]; }

Node *node_new(NodeKind k, Src *src, size_t s, size_t e) {
    Node *n = (Node *)amalloc(sizeof(Node));
    n->kind = (uint8_t)k; n->src = src; n->s = s; n->e = e;
    return n;
}

char *node_text(const Node *n) { return astrndup(n->src->text + n->s, n->e - n->s); }

static void put_u32(Buf *b, const U32 *s) { for (size_t i = 0; i < s->n; i++) buf_put_utf8(b, s->cp[i]); }

void node_repr(const Node *n, Buf *b) {
    switch (n->kind) {
    case N_NUMBER: buf_puts(b, n->u.num.repr); break;
    case N_SYMBOL: buf_puts(b, n->u.sym.name); if (n->u.sym.nec_label) buf_putc(b, ':'); if (n->u.sym.loc) buf_puts(b, n->u.sym.loc); break;
    case N_IPTR: buf_putc(b, '.'); break;
    case N_PAREN: buf_puts(b, n->u.paren.open); node_repr(n->u.paren.expr, b); buf_puts(b, n->u.paren.close); break;
    case N_INFIX: node_repr(n->u.op.lhs, b); buf_putc(b, ' '); buf_puts(b, n->u.op.op->ch); buf_putc(b, ' '); node_repr(n->u.op.rhs, b); break;
    case N_PREFIX: buf_puts(b, n->u.op.op->ch); node_repr(n->u.op.rhs, b); break;
    case N_POSTFIX: node_repr(n->u.op.rhs, b); buf_puts(b, n->u.op.op->ch); break;
    case N_CHARLIT: buf_puts(b, n->u.chr.repr); break;
    case N_QSTR: buf_puts(b, n->u.qstr.quote); put_u32(b, &n->u.qstr.str); buf_puts(b, n->u.qstr.quote); break;
    case N_ANGLE: buf_putc(b, '<'); node_repr(n->u.angle.expr, b); buf_putc(b, '>'); break;
    case N_STRCAT:
        for (size_t i = 0; i < n->u.cat.chunks.n; i++) { if (i) buf_putc(b, ' '); node_repr((Node *)n->u.cat.chunks.a[i], b); }
        break;
    case N_INSN: {
        node_repr(n->u.insn.name, b);
        size_t cnt = n->u.insn.ops.n;
        Node *blk = NULL;
        if (cnt && ((Node *)n->u.insn.ops.a[cnt - 1])->kind == N_BLOCK) { blk = (Node *)n->u.insn.ops.a[cnt - 1]; cnt--; }
        if (cnt) { buf_putc(b, ' '); for (size_t i = 0; i < cnt; i++) { if (i) buf_puts(b, ", "); node_repr((Node *)n->u.insn.ops.a[i], b); } }
        if (blk) { buf_putc(b, ' '); node_repr(blk, b); }
        break;
    }
    case N_WORDLIST:
        for (size_t i = 0; i < n->u.wl.words.n; i++) { if (i) buf_puts(b, ", "); node_repr((Node *)n->u.wl.words.a[i], b); }
        break;
    case N_LABEL: buf_puts(b, n->u.label.name); buf_putc(b, ':'); if (n->u.label.is_extern) buf_putc(b, ':'); break;
    case N_ASSIGN: node_repr(n->u.assign.target, b); buf_puts(b, n->u.assign.is_extern ? " == " : " = "); node_repr(n->u.assign.value, b); break;
    case N_BLOCK:
        buf_puts(b, "{ ");
        for (size_t i = 0; i < n->u.block.insns.n; i++) { if (i) buf_puts(b, "; "); node_repr((Node *)n->u.block.insns.a[i], b); }
        buf_puts(b, " }");
        break;
    }
}
char *node_repr_str(const Node *n) {
    Buf b = {0};
    node_repr(n, &b);
    char *r = astrndup(b.p ? b.p : "", b.len);
    buf_free(&b);
    return r;
}
