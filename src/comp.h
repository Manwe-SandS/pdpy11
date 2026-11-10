#ifndef COMP_H
#define COMP_H
#include "ast.h"
#include "builtins.h"
#include "charset.h"
#include "report.h"

typedef struct LinkBase {
    int64_t used_base;     /* base assumed at the start of the current walk */
    int64_t new_base;      /* base determined in the current walk */
    bool set;              /* promise settled (this walk) */
    Node *set_where;
    int64_t pred;          /* predicted base for the next walk */
    bool has_pred;
} LinkBase;

typedef struct FileState {
    Src *src;
    const char *filename;
    int internal_id;
    const char *internal_prefix;     /* ".internalN." */
    size_t internal_prefix_len;
    LinkBase *lb;
    PVec internal_symbols;           /* const char* names */
} FileState;

typedef struct State {
    FileState *fs;
    bool in_repeat;
    const char *local_prefix;
    int64_t emit_address;
    Node *insn;
    bool extern_all;
} State;

typedef struct AsgExt {
    State st;                  /* captured state of an assignment */
    Value val;
    uint32_t cyc_stamp, eval_stamp;
    struct LinkBase *dep_lb;   /* link base of the (first) label the value depends on */
    uint8_t evaluating;
    bool dep_multi;
} AsgExt;

typedef struct Sym {
    const char *key;           /* original-case key incl. prefix */
    Node *def;                 /* Label or Assignment node */
    int64_t addr;              /* label address */
    struct LinkBase *lb;       /* link base domain of a label */
    AsgExt *x;                 /* assignments only */
    uint32_t stamp;            /* walk id in which (re)defined */
    bool is_label;
    const char *scope_prefix;  /* global label: the local-label prefix of the scope that starts after it */
    const char *file_prefix;   /* the symbol prefix of the file where it is defined (for "Name:Label") */
    size_t file_prefix_len;
} Sym;

typedef struct ExtEnt {
    Node *loc;
    const char *key;           /* lowercase symbols key */
    uint32_t stamp;
} ExtEnt;

typedef struct Emitted {
    Src *src; size_t s, e;     /* directive location for io errors */
    int fmt;                   /* 0 bin, 1 raw, 2 bk_wav, 3 bk_turbo_wav, 4 sav */
    int64_t final_addr;        /* sav: final address of the program, -1 = not given */
    const char *path;
    unsigned char bkname[16];
} Emitted;

typedef struct Comp {
    SMap symbols;              /* lowercase key -> Sym* */
    PVec sym_order;            /* Sym* in first-insertion order */
    SMap externs;              /* lowercase name -> ExtEnt* */
    SMap times_compiled;       /* filename -> counter (as uintptr) */
    PVec internal_files;       /* filename by internal id (index id-1) */
    PVec emitted;
    Charset charset;
    uint32_t walk_id;          /* unique per walk */
    int next_local, next_internal;
    int unresolved;
    bool labels_changed, base_changed;
    bool final_walk;
    Buf out;
    char *charset_raw;
} Comp;
extern Comp G;

/* eval.c */
Value eval(Node *n, State *st);
int64_t get_as_int(State *st, const char *what, const Node *token, Node *arg, int bitness, bool unsigned_, bool has_default, int64_t def);
U32 get_as_str(State *st, const char *what, const Node *token, Node *arg);
void recoverable_error(void);               /* raise RecoverableError (longjmp to catcher or abort) */
extern jmp_buf *g_recov;
extern struct LinkBase *g_hit_lb;
extern bool g_hit_multi, g_track_on;
void note_hit(struct LinkBase *lb);
PD_NORETURN void internal_error(const char *msg);
Sym *sym_lookup_symbol(Node *n, State *st, bool report_missing);   /* for locate_definition */
Value eval_assignment(Sym *s);
bool compile_and_link(FileAst **files, size_t nfiles, int64_t *base_out);
char *abspath_of(const char *path);
Buf format_bin(int64_t base, const char *code, size_t n);
extern int64_t g_default_link;
Buf format_sav(int64_t base, const char *code, size_t n, int64_t final_addr);
Buf format_wav(int64_t base, const unsigned char *code, size_t n, const unsigned char *name16, bool turbo);
char *generate_listing(void);

/* cl.c: relocatable object mode */
#include "objfile.h"
extern bool g_cl_mode, g_cl_allow, g_cl_undef_ok;
extern int g_cl_hits, g_cl_undef;
extern int64_t g_cl_base;
void cl_reset_walk(int64_t base);
void cl_record(State *st, Node *expr, uint32_t type, int64_t addr);
int64_t get_as_int_ref(State *st, const char *what, const Node *token, Node *arg, int bitness, bool unsigned_, bool has_default, int64_t def, uint32_t type, int64_t addr);
void cl_build_object(Obj *o, int64_t base, bool relocatable);
void cl_build_table(Obj *o, int64_t base);

/* insn.c */
void compile_machine_insn(const Builtin *b, Node *insn, State *st);
/* meta.c */
void compile_meta_insn(const Builtin *b, Node *insn, State *st);
void compile_word_list(Node *insn, PVec *words, State *st);
void st_odd_address_error(State *st, const char *what);

/* compile.c */
int64_t walk_block(State *st0, Node *block, int64_t start, bool is_repeat_ctx);
int64_t compile_include_file(FileAst *f, State *st, LinkBase *lb);
void set_link_address(Node *addr_expr, State *st);
void declare_external(Node *loc, const char *name, State *st);
void emit_bytes(const void *p, size_t n);
void emit_byte(int b);
void emit_zeros(size_t n);
extern jmp_buf *g_stop;
extern jmp_buf *g_item;
extern int g_undef;          /* counts undefined-symbol hits: derived range errors are suppressed */
extern bool g_fatal;
void stop_iteration(void);
char *resolve_relative_path(const char *rel, const char *base_file);
bool is_absolute_path(const char *p);
bool is_device_path(const char *p);
bool sound_play(const unsigned char *wav, size_t n, char **err);   /* sound.c */

typedef struct IncCache {
    char *path;          /* resolved path used for the cached parse */
    FileAst *ast;
    int fail_kind;       /* 0 ok, 1 not found, 2 directory, 3 io, 4 not utf-8 */
    char *fail_msg;
    LinkBase lb;
} IncCache;
char *u32_to_utf8(const U32 *s);
U32 utf8_to_u32(const char *s, size_t n);
/* misc */
#define IS_LAYOUT() (g_silent > 0)
#endif
