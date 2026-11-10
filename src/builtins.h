#ifndef BUILTINS_H
#define BUILTINS_H
#include "ast.h"
#include "tables.h"
#include <limits.h>

typedef enum { OT_INT, OT_STR, OT_BLOCK } OpType;
typedef struct { OpType type; int bitness; bool unsigned_; const char *name; } OpInfo;

typedef enum {
    M_BYTE, M_WORD, M_DWORD, M_ASCII, M_ASCIZ, M_RAD50, M_BLKB, M_BLKW, M_EVEN, M_ODD, M_ALIGN, M_REPEAT,
    M_ERROR, M_LIST, M_NLIST, M_TITLE, M_SBTTL, M_IDENT, M_PAGE, M_INSERT_FILE, M_MAKE_BIN, M_MAKE_ROM,
    M_MAKE_RAW, M_MAKE_SAV, M_MAKE_WAV, M_MAKE_TURBO_WAV, M_PLAY_WAV, M_PLAY_TURBO_WAV, M_LINK, M_INCLUDE, M_EXTERN, M_END, M_ONCE
} MetaId;

typedef struct MetaDef {
    const char *name;        /* canonical name (with dot where applicable) */
    MetaId id;
    int nopinfo;
    OpInfo info[2];
    int min_ops, max_ops;    /* max_ops INT_MAX = unbounded */
    bool takes_block, literal_string, raw;
    int size;                /* >= 0 fixed size, -1 = by operands/none (see compiler) */
} MetaDef;

typedef struct Builtin {
    const InsnDef *insn;     /* machine instruction or NULL */
    const MetaDef *meta;
    int min_ops, max_ops;
} Builtin;

const Builtin *builtin_find(const char *name, size_t len);
const char *builtin_suggest(const char *name);
bool is_register_name(const char *s, size_t len);   /* r0..r7 sp pc, case-insens */
int register_index(const char *s, size_t len);       /* -1 if not */
void register_alias_add(const char *name, size_t len, int idx);   /* extra name for register idx */
#endif
