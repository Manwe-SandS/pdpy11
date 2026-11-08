#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdio.h>
#include <stdbool.h>
#include "util.h"

/* Open a file given a UTF-8 path (uses the wide API on Windows). */
FILE *pf_fopen(const char *path, const char *mode);
/* 0 = regular/other file, 1 = does not exist, 2 = directory, 3 = other error */
int pf_file_kind(const char *path);
/* Make console ANSI escape sequences work on Windows; no-op elsewhere. */
void pf_enable_ansi(void);
void pf_set_binary(FILE *f);
#endif
