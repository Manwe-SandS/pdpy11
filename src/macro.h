#ifndef MACRO_H
#define MACRO_H
#include "util.h"

/* Text macros (MACRO-11 style): .macro NAME dummy1, dummy2=default, ?label ... .endm
   A macro is stored as text; a call substitutes the real arguments into the body and the result is parsed
   again in place of the call. */

typedef struct Macro Macro;

const Macro *macro_lookup(const char *name, size_t len);     /* case-insensitive */
const char *macro_name(const Macro *m);
size_t macro_ndummies(const Macro *m);
bool macro_is_builtin(const Macro *m);                       /* one of the hidden compatibility macros (.addr, .print) */

/* First word of the line text[a..b) after an optional "label:", lower-cased into w; its length (0 if none). */
size_t macro_line_word(const char *t, size_t a, size_t b, char *w, size_t wcap);

/* Parse a macro definition. `pos` is just after the ".macro" word in text[0..len). On success the macro is
   registered and the position of the end of the ".endm" line is returned; on failure SIZE_MAX is returned
   and *err (static string) / *err_pos describe the problem. */
size_t macro_define(const char *text, size_t len, size_t pos, const char **err, size_t *err_pos);

/* Collect the real arguments of a call: `pos` is just after the macro name; reads up to the end of the line
   (a comment or newline). Arguments are arena strings pushed to *args. Returns the end position. */
size_t macro_collect_args(const char *text, size_t len, size_t pos, PVec *args);

/* Expand the macro with the given real arguments. Returns an arena string (newline-terminated lines). */
char *macro_expand(const Macro *m, const PVec *args, size_t *outlen);

/* Define the built-in compatibility macros (hidden; used for Turbo8 sources). */
void macro_init_builtin(void);

#endif
