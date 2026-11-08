# PDPy11 (C version)

A cross-assembler for PDP-11 and its relatives (Elektronika BK-0010 / BK-0011M, UKNC, Soyuz-Neon, Elektronika-60).

This is a port of [PDPy11](https://github.com/pdpy11/pdpy11) 2.0.4 from Python to portable, single-threaded ISO C11
with no external dependencies. For valid programs it produces byte-for-byte the same output files as the Python
version, and it runs about 100 times faster with about 5 times less memory.

(c) Ivanq & Manwe 2018-2026. Licensed under GNU GPLv3 or (at your option) any later version.

## Download and install

Ready-to-use archives for Windows (x64, x86), macOS (Apple silicon and Intel), and Linux (x64, arm64) are on the
[Releases](../../releases) page. Every archive contains the compiler, the Sublime Text support files and
[INSTALL.md](INSTALL.md) with step-by-step instructions.

## Build from source

```
make                 # macOS / Linux / MinGW; or: cc -O2 -std=c11 -o pdpy11 src/*.c -lm
make test            # the test suite; no Python needed
```

Windows with MSVC: run `build.bat` from a Developer Command Prompt. Sound output on Windows needs `winmm`
(it is linked automatically by `build.bat` and by the Makefile under MinGW).

## Usage

```
pdpy11 [-o outfile] [--bin | --raw | --sav | --wav | --turbo-wav] [--link N] [--play | --play-turbo]
       [--lst] [--implicit-bin] [--charset encoding] [--source-charset encoding]
       [--report-format graphical|bare] [-Wxxx] infile [infile ...]
```

| Option | Meaning |
|---|---|
| `-o outfile` | name of the output file |
| `--bin`, `--raw`, `--sav`, `--wav`, `--turbo-wav` | output format (`.bin` with the BK header, raw image, RT-11 `.sav`, BK tape WAV, BK turbo tape WAV) |
| `--link N` | default link address (octal, default `1000`); `.link` in the source overrides it |
| `--play`, `--play-turbo` | play the program through the sound output in the BK tape format (normal or turbo) |
| `--implicit-bin` | write `<source>.bin` when neither the source nor the command line asks for an output file |
| `--lst` | write a listing file |
| `--charset`, `--source-charset` | encoding of strings in the output / of the source files (UTF-8, UTF-16, CP1251, CP866, KOI8-R are detected automatically) |

The output format can also be chosen in the source:

```asm
make_bin   ["file.bin"]
make_raw   ["file"]
make_sav   ["file.sav" [, final_address]]
make_wav        ["file.wav" [, "name on tape"]]     ; "~speaker" instead of the file name plays the sound
make_turbo_wav  ["file.wav" [, "name on tape"]]
play_wav        ["name on tape"]                    ; plays instead of writing a file
play_turbo_wav  ["name on tape"]
```

Sound is played by the system: PlaySound on Windows, `afplay` on macOS, `paplay` or `aplay` on Linux.

Text macros, conditional assembly and repeat blocks are described in [docs/MACROS.md](docs/MACROS.md) (in Russian).
The tool `tools/convert-to.c` (`make convert-to`) rewrites sources written for other assemblers in the PDPy11 style.

`tests/DIFFERENCES.md` lists the intentional differences from the Python version.
