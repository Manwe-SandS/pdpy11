#include "builtins.h"

static const MetaDef METAS[] = {
    {".byte", M_BYTE, 1, {{OT_INT, 8, false, "byte operand"}}, 0, INT_MAX, false, false, false, -1},
    {".word", M_WORD, 1, {{OT_INT, 16, false, "word operand"}}, 0, INT_MAX, false, false, false, -1},
    {".dword", M_DWORD, 1, {{OT_INT, 32, false, "dword operand"}}, 0, INT_MAX, false, false, false, -1},
    {".ascii", M_ASCII, 1, {{OT_STR, 0, false, "ascii text"}}, 1, 1, false, false, true, -1},
    {".asciz", M_ASCIZ, 1, {{OT_STR, 0, false, "ascii text"}}, 1, 1, false, false, true, -1},
    {".rad50", M_RAD50, 1, {{OT_STR, 0, false, "string"}}, 1, 1, false, false, true, -1},
    {".blkb", M_BLKB, 1, {{OT_INT, 16, true, "blkb count"}}, 1, 1, false, false, false, -1},
    {".blkw", M_BLKW, 1, {{OT_INT, 16, true, "blkw count"}}, 1, 1, false, false, false, -1},
    {".even", M_EVEN, 0, {{0}}, 0, 0, false, false, false, -1},
    {".odd", M_ODD, 0, {{0}}, 0, 0, false, false, false, -1},
    {".align", M_ALIGN, 1, {{OT_INT, 0, true, "count"}}, 1, 1, false, false, false, -1},
    {".repeat", M_REPEAT, 2, {{OT_INT, 0, true, "repetitions count"}, {OT_BLOCK, 0, false, "body"}}, 1, 1, true, false, false, -1},
    {".error", M_ERROR, 1, {{OT_STR, 0, false, "error"}}, 0, 1, false, true, false, 0},
    {".list", M_LIST, 1, {{OT_INT, 0, false, " "}}, 0, 1, false, false, false, 0},
    {".nlist", M_NLIST, 1, {{OT_INT, 0, false, " "}}, 0, 1, false, false, false, 0},
    {".title", M_TITLE, 1, {{OT_STR, 0, false, "title"}}, 1, 1, false, true, false, 0},
    {".sbttl", M_SBTTL, 1, {{OT_STR, 0, false, "text"}}, 1, 1, false, true, false, 0},
    {".ident", M_IDENT, 1, {{OT_STR, 0, false, "identification"}}, 1, 1, false, false, false, 0},
    {".page", M_PAGE, 0, {{0}}, 0, 0, false, false, false, 0},
    {"insert_file", M_INSERT_FILE, 1, {{OT_STR, 0, false, "inserted file path"}}, 1, 1, false, false, false, -1},
    {"make_bin", M_MAKE_BIN, 1, {{OT_STR, 0, false, "bin file path"}}, 0, 1, false, false, false, 0},
    {"make_bk0010_rom", M_MAKE_ROM, 1, {{OT_STR, 0, false, "bin file path"}}, 0, 1, false, false, false, 0},
    {"make_raw", M_MAKE_RAW, 1, {{OT_STR, 0, false, "raw file path"}}, 0, 1, false, false, false, 0},
    {"play_wav", M_PLAY_WAV, 1, {{OT_STR, 0, false, "file name on tape"}}, 0, 1, false, false, false, 0},
    {"play_turbo_wav", M_PLAY_TURBO_WAV, 1, {{OT_STR, 0, false, "file name on tape"}}, 0, 1, false, false, false, 0},
    {"make_sav", M_MAKE_SAV, 2, {{OT_STR, 0, false, "sav file path"}, {OT_INT, 16, true, "final address"}}, 0, 2, false, false, false, 0},
    {"make_wav", M_MAKE_WAV, 2, {{OT_STR, 0, false, "output wav path"}, {OT_STR, 0, false, "file name on tape"}}, 0, 2, false, false, false, 0},
    {"make_turbo_wav", M_MAKE_TURBO_WAV, 2, {{OT_STR, 0, false, "output wav path"}, {OT_STR, 0, false, "file name on tape"}}, 0, 2, false, false, false, 0},
    {".link", M_LINK, 1, {{OT_INT, 0, false, "address"}}, 1, 1, false, false, true, 0},
    {".include", M_INCLUDE, 1, {{OT_STR, 0, false, "included file path"}}, 1, 1, false, false, false, 0},
    {".extern", M_EXTERN, 1, {{OT_INT, 0, false, "symbol name"}}, 0, INT_MAX, false, false, true, 0},
    {".end", M_END, 0, {{0}}, 0, 0, false, false, false, 0},
    {".once", M_ONCE, 0, {{0}}, 0, 0, false, false, false, 0},
};
/* ".la" and ".insert" are hidden compatibility aliases (not offered in "did you mean" hints) */
static const struct { const char *alias; MetaId id; } ALIASES[] = {{".db", M_BYTE}, {".dw", M_WORD}, {".la", M_LINK}, {".insert", M_INSERT_FILE}, {".ds", M_BLKB}, {".org", M_LINK}};
static const char *const HIDDEN_NAMES[] = {".la", ".insert", ".ds", ".org"};

static SMap g_map;
static bool g_init;

static void bi_init(void) {
    g_init = true;
    char buf[64];
    for (size_t i = 0; i < N_INSN_DEFS; i++) {
        Builtin *b = (Builtin *)amalloc(sizeof(Builtin));
        b->insn = &insn_defs[i];
        b->min_ops = b->max_ops = insn_defs[i].nops;
        size_t l = strlen(insn_defs[i].name);
        lower_ascii_n(buf, insn_defs[i].name, l);
        *smap_slot(&g_map, buf, l, NULL) = b;
    }
    for (size_t i = 0; i < sizeof METAS / sizeof METAS[0]; i++) {
        Builtin *b = (Builtin *)amalloc(sizeof(Builtin));
        b->meta = &METAS[i];
        b->min_ops = METAS[i].min_ops; b->max_ops = METAS[i].max_ops;
        size_t l = strlen(METAS[i].name);
        lower_ascii_n(buf, METAS[i].name, l);
        *smap_slot(&g_map, buf, l, NULL) = b;
        for (size_t k = 0; k < sizeof ALIASES / sizeof ALIASES[0]; k++)
            if (ALIASES[k].id == METAS[i].id) {
                size_t al = strlen(ALIASES[k].alias);
                *smap_slot(&g_map, ALIASES[k].alias, al, NULL) = b;
            }
    }
}

const Builtin *builtin_find(const char *name, size_t len) {
    if (!g_init) bi_init();
    char buf[96];
    if (len >= sizeof buf) return NULL;
    lower_ascii_n(buf, name, len);
    return (const Builtin *)smap_get(&g_map, buf, len);
}

static SMap g_regalias;   /* extra register names (value = index + 1) */
void register_alias_add(const char *name, size_t len, int idx) {
    char buf[96];
    if (len == 0 || len >= sizeof buf || register_index(name, len) >= 0) return;
    lower_ascii_n(buf, name, len);
    *smap_slot(&g_regalias, buf, len, NULL) = (void *)(intptr_t)(idx + 1);
}
int register_index(const char *s, size_t len) {
    if (len == 2) {
        char a = (char)(s[0] | 0x20), b = (char)(s[1] | 0x20);
        if (a == 'r' && b >= '0' && b <= '7') return b - '0';
        if (a == 's' && b == 'p') return 6;
        if (a == 'p' && b == 'c') return 7;
    }
    if (g_regalias.n) {
        char buf[96];
        if (len < sizeof buf) {
            lower_ascii_n(buf, s, len);
            intptr_t v = (intptr_t)smap_get(&g_regalias, buf, len);
            if (v) return (int)(v - 1);
        }
    }
    return -1;
}
bool is_register_name(const char *s, size_t len) { return register_index(s, len) >= 0; }

static int edit1(const char *a, const char *b) {   /* 1 if edit distance <= 1 (insert/delete/substitute/transpose), else 0 */
    size_t la = strlen(a), lb = strlen(b);
    if (la > lb + 1 || lb > la + 1) return 0;
    size_t i = 0;
    while (i < la && i < lb && a[i] == b[i]) i++;
    if (i == la && i == lb) return 1;
    if (la == lb) {
        if (strcmp(a + i + 1, b + i + 1) == 0) return 1;
        if (i + 1 < la && a[i] == b[i + 1] && a[i + 1] == b[i] && strcmp(a + i + 2, b + i + 2) == 0) return 1;
        return 0;
    }
    if (la > lb) return strcmp(a + i + 1, b + i) == 0;
    return strcmp(a + i, b + i + 1) == 0;
}
const char *builtin_suggest(const char *name) {
    if (!g_init) bi_init();
    char low[96]; size_t l = strlen(name);
    if (l >= sizeof low || l < 2) return NULL;
    lower_ascii_n(low, name, l);
    const char *best = NULL;
    for (size_t i = 0; i < g_map.cap; i++) {
        if (!g_map.tab[i].key) continue;
        const char *k = g_map.tab[i].key;
        bool hidden = false;
        for (size_t h = 0; h < sizeof HIDDEN_NAMES / sizeof HIDDEN_NAMES[0]; h++) if (strcmp(k, HIDDEN_NAMES[h]) == 0) hidden = true;
        if (hidden) continue;
        if (edit1(low, k) && strcmp(low, k) != 0) { if (!best || strcmp(k, best) < 0) best = k; }
        /* also compare with the dot-less form of directives */
        if (k[0] == '.' && edit1(low, k + 1) && strcmp(low, k + 1) != 0) { if (!best || strcmp(k, best) < 0) best = k; }
    }
    return best;
}
