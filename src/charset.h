#ifndef CHARSET_H
#define CHARSET_H
#include "util.h"
typedef enum { CS_BK, CS_ASCII, CS_UTF8, CS_LATIN1, CS_CP1251, CS_CP866, CS_KOI8R } CharsetId;
typedef struct { CharsetId id; const char *raw_name; } Charset;
bool charset_lookup(const char *name, Charset *out);
/* Encode string. On failure returns false and *err holds Python-like UnicodeEncodeError text. */
bool cs_encode(const Charset *cs, const U32 *s, Buf *out, char **err);
/* single-byte decoding for source files: code point of byte b (>=128) in charset id, 0 if undefined */
uint32_t cs_decode_hi(CharsetId id, unsigned char b);
/* radix-50 */
extern const char RADIX50_TABLE[]; /* " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.%0123456789" */
#endif
