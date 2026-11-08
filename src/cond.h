#ifndef COND_H
#define COND_H
#include "util.h"

/* Conditional assembly at macro-expansion time (MACRO-11 style .if/.iif/.ntype).
   The condition is evaluated while the text is parsed, with its own table of constant symbols: every
   "name = constant expression" statement seen so far (and every .ntype) is remembered here.
   Expressions follow MACRO-11: the binary operators + - * / & ! have one level and go from left to right. */

bool cond_get(const char *name, size_t n, long *v);          /* value of a known constant symbol */
void cond_set(const char *name, size_t n, long v);
void cond_forget(const char *name, size_t n);

/* evaluate an expression; false (and *err) if a symbol has no known value or the syntax is wrong */
bool cond_eval(const char *s, size_t n, long *v, const char **err);

/* The condition of ".if" / ".iif": `pos` is just after the directive word. On success returns the position after
   the condition arguments and the truth value in *result; SIZE_MAX (and *err) otherwise. */
size_t cond_test(const char *t, size_t len, size_t pos, bool *result, const char **err);

/* .ntype: the 6-bit address mode code of an operand (e.g. r0 = 0, #x = 027, x = 067) */
long cond_ntype(const char *s, size_t n);

#endif
