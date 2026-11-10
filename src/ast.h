#ifndef AST_H
#define AST_H
#include "util.h"
#include "text.h"

typedef enum {
    N_NUMBER, N_SYMBOL, N_IPTR, N_PAREN, N_INFIX, N_PREFIX, N_POSTFIX, N_CHARLIT, N_QSTR, N_ANGLE, N_STRCAT,
    N_INSN, N_WORDLIST, N_LABEL, N_ASSIGN, N_BLOCK
} NodeKind;

typedef enum {
    OP_POSTADD, OP_POSTSUB, OP_POS, OP_NEG, OP_INV, OP_INV2, OP_MUL, OP_DIV, OP_MOD, OP_ADD, OP_SUB,
    OP_LSHIFT, OP_RSHIFT, OP_LSH, OP_AND, OP_XOR, OP_OR, OP_OR2, OP_IMMEDIATE, OP_DEFERRED, OP_REGISTER, OP_CALL,
    OP__COUNT
} OpId;

typedef struct OpDef {
    OpId id;
    const char *ch;      /* operator characters, lower case */
    int kind;            /* 0 infix, 1 prefix, 2 postfix */
    int prec;
    bool left;
    bool pure;
} OpDef;

typedef enum { V_INT, V_STR } VType;
typedef struct { VType t; int64_t i; U32 s; } Value;

typedef struct Node Node;
struct Node {
    uint8_t kind;
    uint32_t s, e;           /* source offsets (sources are limited to 4 GB) */
    Src *src;
    union {
        struct { const char *repr; int64_t value; bool valid_label, invalid_base8, reported; } num;
        struct { const char *name; const char *lname; bool nec_label; const char *loc; } sym;   /* loc: "Name:2" = local label 2: after the global label Name:; "Name:Init" = symbol Init of the file where Name is defined */
        struct { Node *expr; const char *open, *close; } paren;
        struct { const OpDef *op; Node *lhs, *rhs; Value *cached; } op;   /* infix: lhs,rhs; unary: rhs = operand */
        struct { const char *repr; U32 str; bool evaluated; int64_t value; } chr;
        struct { const char *quote; U32 str; } qstr;
        struct { Node *expr; bool reported; } angle;
        struct { PVec chunks; } cat;
        struct { Node *name; PVec ops; void *aux; } insn;
        struct { PVec words; } wl;
        struct { const char *name; bool local, is_extern, reported; } label;
        struct { Node *target; Node *value; bool is_extern, reported; } assign;
        struct { PVec insns; } block;
    } u;
};

Node *node_new(NodeKind k, Src *src, size_t s, size_t e);
const OpDef *op_def(OpId id);
extern const OpDef OPS[];
extern const int OP_NUM;

void node_repr(const Node *n, Buf *b);
char *node_repr_str(const Node *n);
char *node_text(const Node *n);                  /* source text between ctx_start and ctx_end */

typedef struct { Src *src; Node *body; } FileAst;

/* parser */
FileAst *parse_file(Src *src);
bool parsed_include_failed(const char *path);       /* its fatal syntax error was already reported */
FileAst *parsed_include_lookup(const char *path);   /* include parsed ahead of the compile pass, or NULL */
extern int g_parse_depth;
#endif
