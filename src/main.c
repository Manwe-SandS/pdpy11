#include <errno.h>
#include "platform.h"
#include <stdio.h>
#include "comp.h"
#include "cond.h"

#define VERSION "2.1.1"

static const char *USAGE =
    "usage: pdpy11 [-h] [-o outfile] [--implicit-bin] [--bin|--raw|--sav|--wav|--turbo-wav]\n              [--link N] [--play|--play-turbo] [--lst] [--charset encoding]\n"
    "              [--source-charset encoding] [--report-format {graphical,bare}]\n"
    "              [-W xxx] [--version]\n"
    "              infile [infile ...]\n";

static const char *HELP =
    "\nCross-assembler for PDP-11\n\n"
    "positional arguments:\n"
    "  infile                assembly source files\n\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  -o outfile            name of output file\n"
    "  --implicit-bin        if no output file is configured in both source and CLI\n"
    "                        arguments, assume an implicit make_bin directive\n"
    "  --bin, --raw, --sav, --wav, --turbo-wav\n"
    "                        write the output file in this format\n"
    "  --link N              default link address (octal; default: 1000)\n"
    "  --play, --play-turbo  play the program through the sound output (BK tape\n"
    "                        format, normal or turbo)\n"
    "  --lst                 generate a listing file\n"
    "  --charset encoding    output string encoding for .ascii and other directives\n"
    "                        (default: bk)\n"
    "  --source-charset encoding\n"
    "                        encoding of the source files and of the files they\n"
    "                        .include: auto (default; detected for every file\n"
    "                        separately), utf-8, utf-16, cp1251, cp866, koi8-r,\n"
    "                        latin-1\n"
    "  --report-format {graphical,bare}\n"
    "                        format in which error messages and warnings are\n"
    "                        printed\n"
    "  -W xxx                enable warning xxx, use '-Wno-xxx' to disable, '-Wall'\n"
    "                        to enable all. Only few critical warnings are enabled\n"
    "                        by default (-Wdefault)\n"
    "  --version, -v         show program's version number and exit\n\n"
    "PDPy11 is licensed under GNU GPLv3 or (at your option) any later version.\n";

PD_NORETURN static void usage_error(const char *msg) {
    fputs(USAGE, stderr);
    fprintf(stderr, "pdpy11: error: %s\n", msg);
    exit(2);
}

static void set_binary(FILE *f) { pf_set_binary(f); }

static bool write_file(const char *path, const void *data, size_t n, bool text, char **err) {
    FILE *f = pf_fopen(path, text ? "w" : "wb");
    if (!f) {
        int e = errno;
        *err = afmt("[Errno %d] %s: '%s'", e, strerror(e), path);
        return false;
    }
    size_t w = fwrite(data, 1, n, f);
    int cl = fclose(f);
    if (w != n || cl != 0) { *err = afmt("[Errno %d] %s", errno, strerror(errno)); return false; }
    return true;
}


static void write_obj(const Obj *o, const char *path) {
    Buf b = obj_serialize(o);
    char *err;
    if (!write_file(path, b.p, b.len, false, &err)) { fprintf(stderr, "Could not write to '%s':\n%s\n", path, err); exit(1); }
    fprintf(stderr, "File '%s' was written in format 'obj'\n", path);
    buf_free(&b);
}

/* --link OUT a.obj b.obj ...: put the object modules together into a binary file */
static int link_objects(PVec *files, const char *out_path, const char *address, bool table_out, const char *table_name) {
    Obj *objs = (Obj *)amalloc(files->n * sizeof(Obj));
    for (size_t i = 0; i < files->n; i++) {
        const char *path = (const char *)files->a[i];
        FILE *f = pf_fopen(path, "rb");
        if (!f) { fprintf(stderr, "Could not read object file '%s'\n", path); return 1; }
        Buf data = {0};
        char tmp[65536]; size_t r;
        while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) buf_putn(&data, tmp, r);
        fclose(f);
        const char *err = NULL;
        if (!obj_parse(&objs[i], (const unsigned char *)(data.p ? data.p : ""), data.len, &err)) { fprintf(stderr, "'%s' is not a valid object file (%s)\n", path, err); return 1; }
        objs[i].path = path;
        buf_free(&data);
    }
    Buf code = {0};
    PVec errs = {0};
    int64_t base = 0;
    Obj table;
    bool have_base = address != NULL;
    int64_t abase = have_base ? strtol(address, NULL, 0) : 0;
    bool ok = obj_link(objs, files->n, have_base, abase, &code, &base, table_out ? &table : NULL, &errs);
    for (size_t i = 0; i < errs.n; i++) fprintf(stderr, "Error: %s\n", (const char *)errs.a[i]);
    if (!ok) return 1;
    if (base < 0 || base > 65535 || code.len > 65535) { fprintf(stderr, "The linked program does not fit in 64 KB\n"); return 1; }
    Buf data = format_bin(base, code.p ? code.p : "", code.len);
    char *err;
    if (!write_file(out_path, data.p, data.len, false, &err)) { fprintf(stderr, "Could not write to '%s':\n%s\n", out_path, err); return 1; }
    fprintf(stderr, "File '%s' was written in format 'bin'\n", out_path);
    if (table_out) {
        const char *nm = table_name;
        if (!nm) { char *t = astrdup(out_path); char *d = strrchr(t, '.'); if (d && !strchr(d, '/')) *d = 0; nm = t; }
        write_obj(&table, afmt("%s_tbl.obj", nm));
    }
    return 0;
}

static int dump_ast(const char *path) {
    int kind; char *err;
    Src *s = src_load(path, path, &kind, &err);
    if (!s) { fprintf(stderr, "%s\n", err); return 1; }
    report_init(false);
    static const char *all[] = {"implicit-operand", "not-implemented", "suspicious-name", "excess-quote", "missing-newline", "meta-typo", "legacy-deferred", "implicit-index", "label-fixup", "excess-hash", "unexpected-newline"};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) warn_set(all[i], true);
    g_unrec_armed = true;
    if (setjmp(g_unrecoverable)) { fflush(stdout); return 1; }
    FileAst *f = parse_file(s);
    Buf b = {0};
    node_repr(f->body, &b);
    printf("<%s> %s\n", path, b.p ? b.p : "");
    return g_error_condition ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--dump-ast") == 0) return dump_ast(argv[2]);
    pf_set_binary(stdin);

    const char *outfile = NULL, *charset = "bk", *report_format = "graphical";
    bool implicit_bin = false, want_lst = false;
    int want_play = 0;   /* --play 1, --play-turbo 2 */
    int want_fmt = -1;   /* --bin 0, --raw 1, --wav 2, --turbo-wav 3, --sav 4 */
    /* hidden compatibility options (object files like those of Turbo8) */
    bool cl_obj = false, obj_out = false, table_out = false, link_mode = false;
    const char *obj_name = NULL, *table_name = NULL, *link_out = NULL, *address = NULL;
    PVec infiles = {0}, warnings = {0}, extras = {0};
    bool only_pos = false;
    static const char *LONGS[] = {"--help", "--implicit-bin", "--lst", "--charset", "--source-charset", "--report-format", "--version", "--sav", "--raw", "--bin", "--wav", "--turbo-wav", "--link", "--play", "--play-turbo"};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (only_pos || a[0] != '-' || a[1] == 0) { pvec_push(&infiles, (void *)a); continue; }
        if (strcmp(a, "--") == 0) { only_pos = true; continue; }
        if (a[1] == '-') {
            char name[64]; const char *eq = strchr(a, '=');
            size_t nl = eq ? (size_t)(eq - a) : strlen(a);
            if (nl >= sizeof name) usage_error(afmt("unrecognized arguments: %s", a));
            memcpy(name, a, nl); name[nl] = 0;
            if (!strcmp(name, "--compile-obj")) { cl_obj = true; continue; }
            if (!strcmp(name, "--object")) { obj_out = true; obj_name = eq ? eq + 1 : NULL; continue; }
            if (!strcmp(name, "--table")) { table_out = true; table_name = eq ? eq + 1 : NULL; continue; }
            if (!strcmp(name, "--ld") || !strcmp(name, "--address")) {
                const char *v = eq ? eq + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!v) usage_error(afmt("argument %s: expected one argument", name));
                if (!strcmp(name, "--ld")) { link_mode = true; link_out = v; } else address = v;
                continue;
            }
            const char *match = NULL; int nmatch = 0;
            for (size_t k = 0; k < sizeof LONGS / sizeof LONGS[0]; k++) {
                if (strcmp(LONGS[k], name) == 0) { match = LONGS[k]; nmatch = 1; break; }
                if (strncmp(LONGS[k], name, nl) == 0) { match = LONGS[k]; nmatch++; }
            }
            if (nmatch == 0) { pvec_push(&extras, (void *)a); continue; }
            if (nmatch > 1) {
                Buf mb = {0};
                for (size_t k = 0; k < sizeof LONGS / sizeof LONGS[0]; k++)
                    if (strncmp(LONGS[k], name, nl) == 0) { if (mb.len) buf_puts(&mb, ", "); buf_puts(&mb, LONGS[k]); }
                usage_error(afmt("ambiguous option: %s could match %s", name, mb.p));
            }
            if (strcmp(match, "--help") == 0) { fputs(USAGE, stdout); fputs(HELP, stdout); return 0; }
            if (strcmp(match, "--version") == 0) { printf("pdpy11 %s running on C\n", VERSION); return 0; }
            if (strcmp(match, "--implicit-bin") == 0) { if (eq) usage_error(afmt("argument --implicit-bin: ignored explicit argument '%s'", eq + 1)); implicit_bin = true; continue; }
            if (strcmp(match, "--play") == 0 || strcmp(match, "--play-turbo") == 0) {
                if (eq) usage_error(afmt("argument %s: ignored explicit argument '%s'", match, eq + 1));
                want_play = strcmp(match, "--play") == 0 ? 1 : 2;
                continue;
            }
            {
                static const struct { const char *n; int f; } FM[] = {{"--bin", 0}, {"--raw", 1}, {"--wav", 2}, {"--turbo-wav", 3}, {"--sav", 4}};
                bool hit = false;
                for (size_t k = 0; k < sizeof FM / sizeof FM[0]; k++) if (strcmp(match, FM[k].n) == 0) {
                    if (eq) usage_error(afmt("argument %s: ignored explicit argument '%s'", match, eq + 1));
                    want_fmt = FM[k].f; hit = true;
                }
                if (hit) continue;
            }
            if (strcmp(match, "--lst") == 0) { if (eq) usage_error(afmt("argument --lst: ignored explicit argument '%s'", eq + 1)); want_lst = true; continue; }
            const char *val;
            if (strcmp(match, "--link") == 0) {      /* default link address, octal like in sources */
                val = eq ? eq + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!val) usage_error("argument --link: expected one argument");
                char *endp; const char *v = val; int rad = 8;
                if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) { rad = 16; v += 2; }
                else if (v[0] == '0' && (v[1] == 'o' || v[1] == 'O')) { v += 2; }
                else if (v[0] == '0' && (v[1] == 'b' || v[1] == 'B')) { rad = 2; v += 2; }
                long n = strtol(v, &endp, rad);
                if (*endp == '.' && !endp[1] && rad == 8) n = strtol(v, &endp, 10);
                if (endp == v || (*endp && !(*endp == '.' && !endp[1])) || n < 0 || n > 65535)
                    usage_error(afmt("argument --link: invalid address: '%s'", val));
                g_default_link = n;
                continue;
            }
            if (eq) val = eq + 1;
            else if (i + 1 < argc && !(argv[i + 1][0] == '-' && argv[i + 1][1])) val = argv[++i];
            else usage_error(afmt("argument %s: expected one argument", match));
            if (strcmp(match, "--charset") == 0) charset = val;
            else if (strcmp(match, "--source-charset") == 0) {
                if (!source_charset_valid(val)) { fprintf(stderr, "'%s' source encoding is unsupported (use auto, utf-8, utf-16, cp1251, cp866, koi8-r or latin-1)\n", val); return 1; }
                g_source_charset = val;
            } else {
                if (strcmp(val, "graphical") != 0 && strcmp(val, "bare") != 0)
                    usage_error(afmt("argument --report-format: invalid choice: '%s' (choose from graphical, bare)", val));
                report_format = val;
            }
            continue;
        }
        /* short options */
        char c = a[1];
        if (c == 'h') { fputs(USAGE, stdout); fputs(HELP, stdout); return 0; }
        if (c == 'v') { printf("pdpy11 %s running on C\n", VERSION); return 0; }
        if (c == 'o' || c == 'W') {
            const char *val;
            if (a[2]) val = a[2] == '=' ? a + 3 : a + 2;
            else if (i + 1 < argc && !(argv[i + 1][0] == '-' && argv[i + 1][1])) val = argv[++i];
            else usage_error(afmt("argument -%c: expected one argument", c));
            if (c == 'o') outfile = val; else pvec_push(&warnings, (void *)val);
            continue;
        }
        if (strchr(a, ' ')) { pvec_push(&infiles, (void *)a); continue; }
        pvec_push(&extras, (void *)a);
    }
    if (!infiles.n) usage_error("the following arguments are required: infile");
    if (link_mode) return link_objects(&infiles, link_out, address, table_out, table_name);
    if (extras.n) {
        Buf eb = {0};
        for (size_t i = 0; i < extras.n; i++) { if (i) buf_putc(&eb, ' '); buf_puts(&eb, (const char *)extras.a[i]); }
        usage_error(afmt("unrecognized arguments: %s", eb.p));
    }

    if (!charset_lookup(charset, &G.charset)) {
        fprintf(stderr, "'%s' encoding is unsupported\n", charset);
        return 1;
    }
    G.charset.raw_name = charset;

    report_init(strcmp(report_format, "graphical") == 0);
    pf_enable_ansi();
    for (size_t i = 0; i < warnings.n; i++) {
        const char *w = (const char *)warnings.a[i];
        bool val = true;
        if (strncmp(w, "no-", 3) == 0) { w += 3; val = false; }
        const char **ids; int n;
        if (warn_class_expand(w, &ids, &n)) for (int k = 0; k < n; k++) warn_set(ids[k], val);
        else warn_set(w, val);
    }

    /* read sources */
    bool error = false;
    PVec srcs = {0};
    for (size_t i = 0; i < infiles.n; i++) {
        const char *path = (const char *)infiles.a[i];
        int kind; char *err; Src *s;
        if (strcmp(path, "-") == 0) {
            s = src_load_file_ptr(stdin, "stdin", &kind, &err);
            if (!s) { fprintf(stderr, "Source file 'stdin' is not in UTF-8:\n%s\n", err); error = true; continue; }
        } else {
            char *ap = abspath_of(path);
            s = src_load(ap, ap, &kind, &err);
            if (!s) {
                if (kind == 1) fprintf(stderr, "Source file '%s' cannot be decoded:\n%s\n", ap, err);
                else fprintf(stderr, "Could not read source file '%s':\n%s\n", ap, err);
                error = true; continue;
            }
        }
        pvec_push(&srcs, s);
    }
    if (error) return 1;

    g_unrec_armed = true;
    if (setjmp(g_unrecoverable)) { fflush(stdout); return 1; }

    FileAst **asts = (FileAst **)xmalloc(sizeof(FileAst *) * srcs.n);
    for (size_t i = 0; i < srcs.n; i++) { warn_source_encoding((Src *)srcs.a[i]); cond_set_radix(8); asts[i] = parse_file((Src *)srcs.a[i]); }
    int64_t base;
    g_cl_mode = cl_obj;
    compile_and_link(asts, srcs.n, &base);
    if (g_error_condition) { fflush(stdout); return 1; }
    if (cl_obj || obj_out || table_out) {
        const char *fn = ((Src *)srcs.a[0])->path;
        size_t l = strlen(fn);
        if (l >= 4 && streq_ci(fn + l - 4, ".mac")) l -= 4;
        if (cl_obj || obj_out) {
            Obj o;
            cl_build_object(&o, base, cl_obj);
            write_obj(&o, obj_name ? obj_name : afmt("%.*s.obj", (int)l, fn));
        }
        if (table_out) {
            Obj t;
            cl_build_table(&t, base);
            write_obj(&t, afmt("%.*s_tbl.obj", (int)strlen(table_name ? table_name : afmt("%.*s", (int)l, fn)), table_name ? table_name : afmt("%.*s", (int)l, fn)));
        }
        if (cl_obj) { fflush(stdout); return 0; }
    }
    const char *code = G.out.p ? G.out.p : "";
    size_t code_len = G.out.len;

    /* emit_files */
    bool was_emitted = G.emitted.n > 0;
    int emitted_fmt = -1; const char *emitted_path = NULL;
    static const char *FMT_NAMES[] = {"bin", "raw", "bk_wav", "bk_turbo_wav", "sav", "bk_wav", "bk_turbo_wav"};
    if (was_emitted) {
        for (size_t i = 0; i < G.emitted.n; i++) {
            Emitted *e = (Emitted *)G.emitted.a[i];
            Buf data = {0};
            if (e->fmt <= 0 || e->fmt >= 2) {
                if (base < 0 || base > 65535 || code_len > 65535) internal_error("struct.error: ushort format requires 0 <= number <= 65535");
            }
            if (e->fmt == 0) data = format_bin(base, code, code_len);
            else if (e->fmt == 1) { buf_putn(&data, code, code_len); }
            else if (e->fmt == 4) data = format_sav(base, code, code_len, e->final_addr);
            else data = format_wav(base, (const unsigned char *)code, code_len, e->bkname, e->fmt == 3 || e->fmt == 6);
            char *err;
            if (e->fmt >= 5 || is_device_path(e->path)) {
                char *serr;
                fprintf(stderr, "Playing %s audio\n", e->fmt == 3 || e->fmt == 6 ? "turbo wave" : "wave");
                if (!sound_play((const unsigned char *)(data.p ? data.p : ""), data.len, &serr))
                    ERR1("sound-error", e->src, e->s, e->e, "Could not play the sound: %s", serr);
            } else if (!write_file(e->path, data.p ? data.p : "", data.len, false, &err)) {
                ERR1("io-error", e->src, e->s, e->e, "Could not write to '%s':\n%s", e->path, err);
            } else {
                fprintf(stderr, "File '%s' was written in format '%s'\n", e->path, FMT_NAMES[e->fmt]);
            }
            buf_free(&data);
        }
        Emitted *e0 = (Emitted *)G.emitted.a[0];
        emitted_fmt = e0->fmt; emitted_path = e0->path;
        if (g_error_condition) { fflush(stdout); return 1; }
    }

    if (want_play) {
        const char *fn = ((Src *)srcs.a[0])->path;
        const char *sl = strrchr(fn, '/');
        const char *bn = sl ? sl + 1 : fn;
        size_t bl = strlen(bn);
        if (bl >= 4 && streq_ci(bn + bl - 4, ".mac")) bl -= 4;
        unsigned char nm[16]; memset(nm, ' ', 16);
        Buf enc = {0}; char *cerr; U32 bu = utf8_to_u32(bn, bl);
        if (!cs_encode(&G.charset, &bu, &enc, &cerr)) internal_error(cerr);
        if (enc.len > 16) { fprintf(stderr, "The BK tape file name '%.*s' does not fit in 16 bytes\n", (int)bl, bn); return 1; }
        if (enc.len) memcpy(nm, enc.p, enc.len);
        if (base < 0 || base > 65535 || code_len > 65535) internal_error("struct.error: ushort format requires 0 <= number <= 65535");
        Buf wav = format_wav(base, (const unsigned char *)code, code_len, nm, want_play == 2);
        char *serr;
        fprintf(stderr, "Playing %s audio\n", want_play == 2 ? "turbo wave" : "wave");
        if (!sound_play((const unsigned char *)(wav.p ? wav.p : ""), wav.len, &serr)) {
            fprintf(stderr, "Could not play the sound: %s\n", serr);
            return 1;
        }
        buf_free(&wav);
    }
    if (!outfile && want_fmt >= 0) {
        static const char *EXT[] = {".bin", "", ".wav", ".wav", ".sav"};
        const char *fn = ((Src *)srcs.a[0])->path;
        size_t l = strlen(fn);
        if (l >= 4 && streq_ci(fn + l - 4, ".mac")) l -= 4;
        outfile = afmt("%.*s%s", (int)l, fn, EXT[want_fmt]);
    }
    if (!outfile && !was_emitted && !want_play) {
        if (implicit_bin) {
            const char *fn = ((Src *)srcs.a[0])->path;
            size_t l = strlen(fn);
            if (l >= 4 && streq_ci(fn + l - 4, ".mac")) l -= 4;
            outfile = afmt("%.*s.bin", (int)l, fn);
        } else {
            fputs("The file was compiled successfully, but no output files were saved because 'make_xxx' directive was not encountered and '-o' option was not passed.\n", stderr);
        }
    }
    const char *lst_fmt = emitted_fmt >= 0 ? FMT_NAMES[emitted_fmt] : NULL;
    if (outfile) {
        const char *slash = strrchr(outfile, '/');
        const char *name = slash ? slash + 1 : outfile;
        const char *dot = strrchr(name, '.');
        const char *ext = dot ? dot + 1 : "";
        size_t nl = strlen(name);
        bool is_bin = nl >= 4 && streq_ci(name + nl - 4, ".bin");
        int fmt = want_fmt >= 0 ? want_fmt : (nl >= 4 && streq_ci(name + nl - 4, ".sav")) ? 4 : is_bin ? 0 : 1;
        lst_fmt = FMT_NAMES[fmt];
        emitted_path = outfile;
        if (base < 0 || base > 65535 || code_len > 65535) { if (fmt == 0) internal_error("struct.error: ushort format requires 0 <= number <= 65535"); }
        Buf data = {0};
        if (fmt == 0) data = format_bin(base, code, code_len);
        else if (fmt == 4) data = format_sav(base, code, code_len, -1);
        else if (fmt >= 2) {
            unsigned char nm[16]; memset(nm, ' ', 16);
            size_t bl = nl; if (bl >= 4 && streq_ci(name + bl - 4, ".wav")) bl -= 4;
            Buf enc = {0}; char *cerr; U32 bu = utf8_to_u32(name, bl);
            if (!cs_encode(&G.charset, &bu, &enc, &cerr)) internal_error(cerr);
            if (enc.len > 16) { fprintf(stderr, "The BK tape file name '%.*s' does not fit in 16 bytes\n", (int)bl, name); return 1; }
            if (enc.len) memcpy(nm, enc.p, enc.len);
            data = format_wav(base, (const unsigned char *)code, code_len, nm, fmt == 3);
        }
        else buf_putn(&data, code, code_len);
        bool to_stdout = strcmp(outfile, "-") == 0 || (strncmp(outfile, "-.", 2) == 0 && strcmp(outfile + 2, ext) == 0 && !slash);
        if (to_stdout) {
            set_binary(stdout);
            fwrite(data.p ? data.p : "", 1, data.len, stdout);
            fflush(stdout);
        } else {
            char *err;
            if (!write_file(outfile, data.p ? data.p : "", data.len, false, &err)) {
                fprintf(stderr, "Could not write to '%s':\n%s\n", outfile, err);
                return 1;
            }
            fprintf(stderr, "File '%s' was written in format '%s'\n", outfile, lst_fmt);
        }
        buf_free(&data);
    }

    if (want_lst) {
        if (!emitted_path) {
            fputs("No listing file was generated because no output file was specified\n", stderr);
        } else {
            char *lst = astrdup(emitted_path);
            size_t l = strlen(lst), fl = strlen(lst_fmt);
            if (l > fl + 1 && lst[l - fl - 1] == '.' && strcmp(lst + l - fl, lst_fmt) == 0) {
                char *d = strrchr(lst, '.');
                if (d) *d = 0;
            }
            lst = afmt("%s.lst", lst);
            if (strcmp(lst, "-.lst") == 0) lst = astrdup("listing.lst");
            char *text = generate_listing();
            char *err;
            if (!write_file(lst, text, strlen(text), true, &err)) {
                fprintf(stderr, "Could not write to '%s':\n%s\n", lst, err);
                return 1;
            }
            fprintf(stderr, "File '%s' was written in format 'lst'\n", lst);
        }
    }
    fflush(stdout);
    return 0;
}
