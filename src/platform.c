#include "comp.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

#ifdef _WIN32
static wchar_t *to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)xmalloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (n <= 0 || MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) w[0] = 0;
    return w;
}
FILE *pf_fopen(const char *path, const char *mode) {
    wchar_t *wp = to_wide(path), *wm = to_wide(mode);
    FILE *f = _wfopen(wp, wm);
    free(wp); free(wm);
    return f;
}
int pf_file_kind(const char *path) {
    wchar_t *wp = to_wide(path);
    struct _stat64i32 sb;
    int r = _wstat(wp, &sb);
    free(wp);
    if (r != 0) return errno == ENOENT ? 1 : 3;
    return (sb.st_mode & _S_IFDIR) ? 2 : 0;
}
void pf_enable_ansi(void) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | 0x0004);
}
void pf_set_binary(FILE *f) { _setmode(_fileno(f), _O_BINARY); }
#else
FILE *pf_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int pf_file_kind(const char *path) {
    struct stat sb;
    if (stat(path, &sb) != 0) return errno == ENOENT ? 1 : 3;
    return S_ISDIR(sb.st_mode) ? 2 : 0;
}
void pf_enable_ansi(void) {}
void pf_set_binary(FILE *f) { (void)f; }
#endif

bool is_device_path(const char *p) {
    if (p[0] != '~') return false;
    size_t i = 1;
    while ((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z')) i++;
    if (i == 1 || !(p[i] == 0 || p[i] == ' ')) return false;
    char name[16]; if (i - 1 >= sizeof name) return false;
    lower_ascii_n(name, p + 1, i - 1);
    return strcmp(name, "speaker") == 0;
}

bool is_absolute_path(const char *p) {
    if (is_device_path(p)) return true;
#ifdef _WIN32
    return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
#else
    return p[0] == '/';
#endif
}

/* os.path.normpath (POSIX semantics; on Windows '\\' is treated as '/') */
static char *normpath(const char *path) {
    if (!*path) return astrdup(".");
    size_t n = strlen(path);
    char *p = astrndup(path, n);
#ifdef _WIN32
    for (size_t i = 0; i < n; i++) if (p[i] == '\\') p[i] = '/';
#endif
    int initial = 0;
    if (p[0] == '/') { initial = 1; if (n >= 2 && p[1] == '/' && !(n >= 3 && p[2] == '/')) initial = 2; }
    char **comps = (char **)xmalloc(sizeof(char *) * (n + 2));
    size_t nc = 0;
    char *save = p;
    for (char *tok = save; ;) {
        char *slash = strchr(tok, '/');
        size_t l = slash ? (size_t)(slash - tok) : strlen(tok);
        if (!(l == 0 || (l == 1 && tok[0] == '.'))) {
            if (l == 2 && tok[0] == '.' && tok[1] == '.') {
                if (nc > 0 && strcmp(comps[nc - 1], "..") != 0) nc--;
                else if (!initial) comps[nc++] = astrdup("..");
            } else comps[nc++] = astrndup(tok, l);
        }
        if (!slash) break;
        tok = slash + 1;
    }
    Buf b = {0};
    for (int i = 0; i < initial; i++) buf_putc(&b, '/');
    for (size_t i = 0; i < nc; i++) { if (i) buf_putc(&b, '/'); buf_puts(&b, comps[i]); }
    free(comps);
    char *r = (!b.len) ? astrdup(".") : astrndup(b.p, b.len);
    buf_free(&b);
    return r;
}

char *resolve_relative_path(const char *rel, const char *base_file) {
    if (is_device_path(rel)) return astrdup(rel);
    if (is_absolute_path(rel)) return normpath(rel);
    const char *slash = strrchr(base_file, '/');
#ifdef _WIN32
    const char *bs = strrchr(base_file, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    Buf b = {0};
    if (slash) { buf_putn(&b, base_file, (size_t)(slash - base_file) + 1); }
    buf_puts(&b, rel);
    char *r = normpath(b.p ? b.p : "");
    buf_free(&b);
    return r;
}

/* os.path.abspath */
char *abspath_of(const char *path) {
    if (is_absolute_path(path)) return normpath(path);
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
    Buf b = {0};
    buf_puts(&b, cwd); buf_putc(&b, '/'); buf_puts(&b, path);
    char *r = normpath(b.p);
    buf_free(&b);
    return r;
}
