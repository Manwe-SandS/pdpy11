#ifndef OBJFILE_H
#define OBJFILE_H
#include "util.h"

/* Object files (.obj) of the Turbo8 cross assembler: relocatable modules (compile only) and the linker.
   Format: little-endian 32-bit fields; every record starts with a tag 0x55xx. Sections:
     5501 5502 : header   {8, 1, 0, 0x1020, link address | flags << 16}      (flags 2: absolute module / table)
     5501 5503 : code     {size, 1, 0} + bytes                                (absent in a table of labels)
     5501 5504 : labels   {bytes, count, 0} + count * {5508 kind value, 550a name}   (kind 1: offset, 3: constant)
     5501 5506 : refs     {bytes, count, 0} + count * {5509 type offset, 550a "", 5510 ntokens, tokens}
   A token is {550b kind} + (1: 550d name | 2: 550d offset name | 0: 550e number | 3: 550f offset | 4: 550c opcode),
   the tokens form a postfix expression; the type tells how the value is stored (see obj_link). */

enum { OT_NUM = 0, OT_SYM = 1, OT_LOCAL = 2, OT_DOT = 3, OT_OP = 4 };
/* operator codes of an OT_OP token */
enum { OC_POS = 1, OC_NEG, OC_INV, OC_ADD, OC_SUB, OC_MUL, OC_DIV, OC_MOD, OC_SHL, OC_SHR, OC_OR, OC_XOR, OC_AND };
/* reference types */
enum { RT_WORD = 0x32, RT_WORD_LOCAL = 0x4032, RT_BYTE = 0x8032, RT_REL = 0x22, RT_BRANCH = 0x12 };

typedef struct { int kind; int64_t v; char *name; } ObjTok;
typedef struct { uint32_t type, off; ObjTok *tok; size_t ntok; } ObjRef;
typedef struct { char *name; uint32_t kind; int64_t val; } ObjLabel;
typedef struct {
    uint32_t link_addr, flags;
    bool has_code;
    Buf code;
    ObjLabel *lab; size_t nlab, caplab;
    ObjRef *ref; size_t nref, capref;
    const char *path;
} Obj;

void obj_add_label(Obj *o, const char *name, uint32_t kind, int64_t val);
ObjRef *obj_add_ref(Obj *o, uint32_t type, uint32_t off);
Buf obj_serialize(const Obj *o);
bool obj_parse(Obj *o, const unsigned char *d, size_t n, const char **err);

/* Link: the code of the modules is placed one after another starting at the address of the first one
   (or `base` if have_base); the expressions of the relocatable modules are evaluated and stored.
   Errors are appended to *errs as strings. If `table` is not NULL, all labels (absolute) are put into it. */
bool obj_link(Obj *objs, size_t n, bool have_base, int64_t base, Buf *out, int64_t *out_base, Obj *table, PVec *errs);

#endif
