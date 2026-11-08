#ifndef TEXT_H
#define TEXT_H
#include "util.h"

typedef struct Src {
    const char *path;      /* as shown in reports */
    const char *encoding;  /* detected/forced source encoding, e.g. "utf-8", "cp1251" */
    bool detected;         /* true if the encoding was auto-detected and is not UTF-8 */
    char *text;            /* UTF-8, newlines normalized to \n */
    size_t len;
    size_t *line_starts;   /* lazily built */
    size_t nlines;
} Src;

/* Decode next code point from valid UTF-8, advancing *i. */
uint32_t u8_next(const char *s, size_t len, size_t *i);
size_t u8_len_at(const char *s, size_t i);           /* byte length of the char starting at i */
bool py_isspace(uint32_t cp);                        /* Python str.isspace() for a single char */

/* Source encoding policy: "auto" (default) or one of utf-8, utf-16, cp1251, cp866, koi8-r, latin-1. */
extern const char *g_source_charset;
bool source_charset_valid(const char *name);
void warn_source_encoding(Src *s);   /* -Wsource-encoding: file was not UTF-8 */

/* Read a file as Python's open(path, encoding='utf-8').read() would.
   Returns NULL and sets *err (arena string) on failure; *kind: 0=io error, 1=not utf-8, 2=is directory, 3=not found */
Src *src_load(const char *path, const char *report_path, int *kind, char **err);
Src *src_from_buf(const char *report_path, const char *data, size_t n, int *kind, char **err);
Src *src_load_file_ptr(void *fp, const char *report_path, int *kind, char **err);

/* line (0-based) and column in "Python reports" terms: chars + 3 per tab before it */
void src_linecol(Src *s, size_t off, size_t *line, size_t *col, size_t *char_col);
size_t src_line_start(Src *s, size_t line);
size_t src_nlines(Src *s);

/* number of code points in [a,b) */
size_t u8_count(const char *s, size_t a, size_t b);

/* format helper: arena printf */
char *afmt(const char *fmt, ...);
#endif
