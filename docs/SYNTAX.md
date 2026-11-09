# PDPy11: language and command line reference

This document describes the language accepted by PDPy11 (the C version, compatible with PDPy11 2.0.4) and the
command line. It is based on the documentation of the Python PDPy11 ([older
docs](https://github.com/purplesyringa/PDPy11/blob/master/docs.md), [2.x
README](https://github.com/pdpy11/pdpy11)), which no longer describes the current compiler completely, and it was
checked against the behaviour of this program.

Contents: [1. Command line](#1-command-line) · [2. Program layout](#2-program-layout) ·
[3. Numbers, characters and strings](#3-numbers-characters-and-strings) · [4. Expressions](#4-expressions) ·
[5. Symbols and labels](#5-symbols-and-labels) · [6. Operands and addressing modes](#6-operands-and-addressing-modes) ·
[7. Instructions](#7-instructions) · [8. Directives](#8-directives) ·
[9. Files, linking and visibility](#9-files-linking-and-visibility) ·
[10. Output formats](#10-output-formats) · [11. Macros and conditional assembly](#11-macros-and-conditional-assembly) ·
[12. Diagnostics](#12-diagnostics)

---

## 1. Command line

```
pdpy11 [-o outfile] [--bin | --raw | --sav | --wav | --turbo-wav] [--link N] [--play | --play-turbo]
       [--lst] [--implicit-bin] [--charset encoding] [--source-charset encoding]
       [--report-format graphical|bare] [-W xxx] [--version] infile [infile ...]
```

| Option | Meaning |
|---|---|
| `infile ...` | source files. Several files are compiled as one program, one after another |
| `-o outfile` | name of the output file. `-o -` writes it to the standard output |
| `--bin` | write a `.bin` file: a 4-byte header (load address, length) followed by the image |
| `--raw` | write the bare image, without a header |
| `--sav` | write an RT-11 `.sav` file (see [10](#10-output-formats)) |
| `--wav`, `--turbo-wav` | write the program as a tape recording for the BK (normal or turbo format) |
| `--link N` | default link address, octal (`0x`, `0o`, `0b` prefixes and a trailing dot for decimal are accepted). Default `1000`. `.link` in the source wins |
| `--play`, `--play-turbo` | play the program through the sound output in the BK tape format |
| `--implicit-bin` | if neither the source (`make_*`) nor the command line asks for an output file, write `<source>.bin` |
| `--lst` | write a listing file next to the output file |
| `--charset enc` | encoding of strings in the output: `bk` (default, the BK character set, KOI-8 based), `koi8-r`, `cp1251`, `utf-8` |
| `--source-charset enc` | encoding of source files: `auto` (default, detected for every file separately), `utf-8`, `utf-16`, `cp1251`, `cp866`, `koi8-r`, `latin-1` |
| `--report-format` | `graphical` (colours and source excerpts, default) or `bare` (one line per message: `file:line:column: Error: text`, convenient for editors) |
| `-W xxx` | enable warning `xxx`; `-Wno-xxx` disables it; `-Wall` enables all; `-Wdefault` is the default set (see [12](#12-diagnostics)) |

If the source has no `make_*` directive and neither `-o` nor `--implicit-bin` (nor a format option) is given, the
program is only checked and nothing is written. The exit status is `0` when there are no errors and `1` otherwise.

---

## 2. Program layout

A program is a sequence of **statements**, one per line. A statement is any of:

```
label:                    a label definition (several labels may precede a statement)
name = expression         a constant definition
instruction operands      an instruction (section 7)
.directive arguments      a directive (section 8)
```

* Names of instructions, directives, registers and symbols are **case-insensitive**: `MOV`, `Mov` and `mov` are the same.
  (The exception is the contents of strings.)
* Comments: `; to the end of the line`, `// to the end of the line` and `/* block comment */`.
* Operands are separated by commas. The leading dot of a directive may be omitted in a few cases, but the assembler
  warns about it (`-Wmeta-typo`); always write the dot.
* Whitespace between tokens is free; a statement ends at the end of the line.
* Words and instructions must lie at even addresses. If a `.word` or an instruction lands at an odd address, an error
  is reported (and a zero byte is inserted so that compilation can go on). Use `.even` after byte data.

```asm
        .link 1000
Start:  mov     #msg, r1            ; load the address of the string
1:      movb    (r1)+, r0
        beq     2
        emt     16
        br      1
2:      halt
msg:    .asciz  "Hello"
        .even
```

---

## 3. Numbers, characters and strings

### Numbers

| Form | Example | Base |
|---|---|---|
| `1234` | `1777` | **octal** (the PDP-11 tradition) |
| `1234.` | `1024.` | decimal (a trailing dot) |
| `0o1234`, `^O1234` | `0o17`, `^O17` | octal |
| `0x1234`, `^X1234` | `0x1f`, `^XFF` | hexadecimal |
| `0b1010`, `^B1010` | `0b101` | binary |
| `^D1234` | `^D10` | decimal |
| `^O<377>`, `^X<ff>`, … | `^O<377>` | the digits may be put in angle brackets |

A number written without a dot that contains the digits 8 or 9 is an error with the hint "add a dot to make it decimal".
`0` followed by digits (`017`) is octal as well.

#### Default radix: `.radix`

`.radix n` changes the base of the numbers written **without** a prefix and without a trailing dot, for all the text
below it (as in MACRO-11). `n` is 2, 8, 10 or 16 and is always read as a decimal number; `.radix` with no argument
returns to 8. The default is 8, and it is reset to 8 at the start of every source file given on the command line
(an `.include`d file continues with the current radix).

```asm
        .radix  10
        mov     #100, r0        ; 100 decimal = 0144
        .word   10, 10.         ; 12 and 12 (octal)
        .radix  16
        .word   1F, 0FF         ; digits a-f are allowed; a number starts with a digit (0FF, not FF)
        .radix  8
        .word   17.             ; the dot, ^D, ^O, ^X, ^B, 0x, 0b still work in any radix
```

- A number with a trailing dot is always decimal; numbers with a prefix (`^D`, `^O`, `^X`, `^B`, `0x`, `0o`, `0b`) keep
  their own base.
- In radix 16 a word made only of hex digits that starts with a digit (`0b1`, `0d9`) is a hexadecimal number, not a
  `0b…`/`0d…` prefixed one. A word that starts with a letter (`FF`, `A1`) is still a symbol.
- A digit not valid in the current radix (`2` in radix 2) is an error.
- `.radix` acts while the text is read, like `.if` and macros: it does not take effect in a skipped `.if` branch, and
  a macro or `.rept` body is read in the radix valid at the point where it is expanded. The numbers in `.if`
  conditions follow the same radix.
- Labels are not affected (a local label such as `1:` is still a label).

### Characters

* `'A` is the code of a character: `mov #'A, r0` loads 101 (octal). There is no closing quote.
* `"AB` is a 16-bit word made of two characters; the first one is in the low byte: `"AB` = `0x4241`.
* Characters are converted with the output charset (`--charset`, default `bk`).
* A closing quote (`'A'`) is tolerated and gives the warning `-Wexcess-quote`.

### Strings

Strings are written in double quotes: `"text"`. Escapes: `\n`, `\r`, `\t`, `\\`, `\"`, `\'`, `\xHH` (two hex digits).
Strings are used by `.ascii`, `.asciz`, `.rad50`, `.include`, `insert_file` and the `make_*` directives. `.ascii` also
accepts byte values in angle brackets, which may be combined with strings: `.ascii <15><12>"Done"`.

---

## 4. Expressions

An expression is built from numbers, characters, symbols, the location counter `.` and the operators below.
Parentheses group subexpressions. Operators in order from the **tightest** to the loosest:

| Level | Operators | Meaning |
|---|---|---|
| 1 | `+x` `-x` `~x` `^Cx` | unary plus, negation, bitwise complement (`^C` is a synonym of `~`) |
| 2 | `*` `/` `%` | multiplication, integer division (floor), remainder |
| 3 | `+` `-` | addition, subtraction |
| 4 | `<<` `>>` `_` | shift left, shift right, and `a _ b`: shift left by *b*, or right if *b* is negative |
| 5 | `&` | bitwise AND |
| 6 | `^` | bitwise XOR |
| 7 | `\|` `!` | bitwise OR (`!` is the MACRO-11 spelling) |

Operators of one level are evaluated from left to right. The order is the same as in C and Python for the operators
they share: `1<<2+1` is `1<<(2+1)` = 8, and `1|2^3&4` is `1|(2^(3&4))`. (MACRO-11 and Turbo8 differ: there `+`, `-`,
`*`, `/`, `&` and `!` are all one level, evaluated from left to right.)

```asm
A = (B + C / 2) * 2 + 7 * 3        ; any mathematical expression
.word 6&3|10, 1<<4, 100 _ -2
```

Values are 64-bit integers while evaluating; when stored they must fit the field (`.byte` 8 bits, `.word` 16 bits, …),
otherwise an error "does not fit" is reported. A negative number fits too (`.word -1` is `177777`).

The location counter `.` is the address of the current statement: `.word .` stores its own address, `br .` is a loop,
and `. = 3000` (or `.link 3000`) moves it (see [8](#8-directives)).

---

## 5. Symbols and labels

### Constants

```asm
size = 40
total = size * 2 + 1
```

A constant is defined once; defining it again is the error "Duplicate variable". The value may depend on labels and on
constants defined further down: PDPy11 evaluates everything after reading the whole program.

```asm
A = B + 100
B = 200 + C
C = L2 - L1
L1: .blkw 100
L2:
```

A circular definition (`x = x`, `a = b`, `b = a`) is reported as an error.

### Labels

* A **global label** is a name followed by a colon: `Loop:`. Names consist of letters, digits, `_`, `$` and `.` and
  may not start with a digit (a name that starts with a digit is a local label). Defining one twice is an error ("Duplicate symbol").
* A **local label** is a name that **starts with a digit**, followed by a colon: `1:`, `2x:`, `10_loop:`, `1abc$:`.
  After the first digit the name may contain letters, digits, `_`, `$` and `.`, as in a global label. A local label is
  visible until the next ordinary (global) label, so the same names can be reused in every routine; defining the same
  name twice inside one scope is the error "Duplicate local label". `br 1` and `jmp (1:)` refer to the `1:` of the
  current scope, whether it is above or below the instruction. A purely numeric name (`1`) has to be written with a
  colon or in a branch target to be taken as a label (see [Local labels in arithmetic](#local-labels-in-arithmetic));
  a name with letters (`1abc`) cannot be a number, so it can be written without a colon: `br 1abc`.
* A local label of another scope is written `Name:N` (no spaces): the local label `N:` that follows the global label
  `Name:`. A symbol of another file is written `Name:Label`: the label `Label` of the file where the label `Name` is
  defined (see [Qualified names](#qualified-names-namen-and-namelabel)).
* A label ending with two colons, `name::`, is **external**: it is visible in the other files of the program (see
  [9](#9-files-linking-and-visibility)). A local label cannot be external (`1::` is the error "A local label cannot be
  external").

### Qualified names: `Name:N` and `Name:Label`

The scope of a local label starts after a global label and ends at the next global label. To refer to a local label
`N:` of a scope from anywhere, write the global label that opens the scope, a colon and the number, with no spaces:
`Name:N`.

```asm
Metka:  nop
1:      nop
2:      nop
        jmp  Metka:2        ; back, the scope of Metka:
Other:  nop
1:      nop                 ; another label 1: (the scope of Other:)
        jmp  @#Metka:2      ; the 2: of Metka:, though the current scope is Other:
        jmp  Last:1         ; forward, to a scope that is defined below
        .word Metka:1, Other:1, Metka:2 - Metka:1
Last:   nop
1:      nop
```

* It works wherever an expression is allowed: operands of instructions (`jmp Metka:2`, `br Metka:1`, `mov #Metka:2, r0`),
  `.word`, parenthesised and arithmetic expressions; references forward and back both work.
* `Name` must be a global label (or a global label shared with `::` or `.extern`, then the form works from other files
  too: `Start::` in one file, `Start:2` in another). A name that is not a label, a private label of another file or a
  missing `N:` after `Name:` is the error "Local label 'Name:N' is not defined".
* The number must follow the colon at once: `Metka:2` is the reference, while `Metka: 2` is the label `Metka:`
  followed by something else.
* In `1: … br 1` and in `(1:)` the current scope is meant; `Name:N` is needed only for another scope.

#### `Name:Label`: a symbol of another file

If the second part is not a number, `Name:Label` means the symbol `Label` (a label or a constant) of **the file** where
the label `Name` is defined. It is meant for the case when several files (libraries) are put into one program and have
the same names inside, for example `Init`:

```asm
; main.mac
        .include "play.mac"
        .include "load.mac"
        call    Play:Init       ; Init of play.mac
        call    Load:Init       ; Init of load.mac

; play.mac                      ; load.mac
Play::  nop                     Load::  nop
Init:   nop                     Init:   nop
```

Export (with `::` or `.extern`) only one label of each file, with a name that is unique in the program (`Play`,
`Load`), and reach the others through it. The private labels `Init` do not clash with each other.

* `Name` must be a label. `Label` is looked up among the symbols of the file where `Name` is defined, whether they are
  shared or private; the whole file counts, not only the part below `Name`.
* The names are case-insensitive, like all symbols. There are no spaces around the colon.
* Only one level is allowed: `A:B:C` and `A:B:1` are an error ("Only one level is allowed").
* The form works in the same file as well (`Play:Init` inside play.mac). A missing label gives "Symbol 'Name:Label'
  is not defined".
* `Name:Label` is only a **reference**; it never defines a label. Several labels in a row are written with whitespace
  between them: `A: B: nop`. The form without spaces, `A:B: nop`, is the error `label-chain` ("Several labels in a row
  must be separated by whitespace … Write 'A: B:' instead"), because it looks like a qualified name. `Loop:nop` (a
  label immediately followed by an instruction) is still fine.

### Local labels in arithmetic

A bare number as a branch target is a *local label*; in other contexts it is a plain number. To mix the two, add a colon
or parentheses:

```asm
1:
2:
        br   1            ; the local label 1:
        br   1 - 2        ; the local label 1: minus 2 bytes  (warning -Wlabel-fixup)
        br   (1)          ; the global address 1
        br   (1 - 2)      ; the global address 177777
        br   (1:)         ; the local label 1:
        br   (1: - 2)     ; the local label 1: minus 2 bytes
        br   (1: - 2:)    ; the local label 1: minus the local label 2:
        jmp  1            ; the global address 1 (JMP takes an address, not an offset)
        jmp  (1:)         ; the local label 1:
```

(The lines above show how each form is parsed; several of them would be out of branch range in a real program.)

Normally you only need `br label`, `br label ± offset` and `jmp global_address`. Add the colon whenever you want a
local label inside a bigger expression.

---

## 6. Operands and addressing modes

Registers: `r0`–`r7`, with `sp` = `r6` and `pc` = `r7`; `%0`–`%7` are the same registers written as numbers.
Floating-point accumulators: `ac0`–`ac5`.

| Mode | Syntax | Example |
|---|---|---|
| Register | `Rn` | `clr r0` |
| Register deferred | `(Rn)` | `clr (r1)` |
| Autoincrement | `(Rn)+` | `mov (r1)+, r0` |
| Autoincrement deferred | `@(Rn)+` | `mov @(r2)+, r0` |
| Autodecrement | `-(Rn)` | `mov r0, -(sp)` |
| Autodecrement deferred | `@-(Rn)` | `clr @-(r2)` |
| Index | `E(Rn)` | `clr 2(r3)`, `clr table(r3)` |
| Index deferred | `@E(Rn)` | `mov @4(r2), r0` |
| Immediate | `#E` | `mov #1000, r0` (the value follows the instruction: `(pc)+`) |
| Absolute | `@#E` | `mov @#177716, r0` (`@(pc)+`) |
| Relative | `E` | `mov var, r0` (a bare expression is a **PC-relative** address: `E-.(pc)`) |
| Relative deferred | `@E` | `mov @ptr, r0` |

Notes:

* A bare name or expression is *relative* addressing, so code that refers to its own variables stays
  position-independent. Use `#E` for the value itself and `@#E` for a fixed absolute address.
* `(E)` is a register only when `E` is a register name; `(1000)` is an expression in parentheses (a plain number).
* Instructions such as `bne`, `br`, `sob` take **offsets**: the operand is the target address, and the assembler
  computes the displacement. A branch reaches a limited distance (−128 … +127 words); `sob` only jumps backwards.
  Out-of-range and odd offsets are reported as errors.
* Floating-point instructions take an accumulator `ac0`–`ac5` or, where the instruction allows it, any CPU addressing
  mode; a plain register `r0`–`r3` in an accumulator place is treated as `ac0`–`ac3` with a note.

---

## 7. Instructions

Instruction names and operands follow the standard PDP-11 mnemonics (DEC syntax, source first, destination second):
`mov src, dst`, `add src, dst`, `jsr reg, dst`, `sob reg, label`, `mul src, reg`, `xor reg, dst`, `emt n`, `trap n`, …

Groups known to the assembler (all names are lower case here, any case is accepted in a program):

| Group | Instructions |
|---|---|
| Control | `halt wait rti rtt bpt iot reset nop mfpt` and the model-specific `start step rd urd rdpc rdps uwr wrpc wrps spl csm tstset wrtlck` |
| Condition codes | `clc clv clz cln ccc sec sev sez sen scc` and their combinations (`clzc`, `senv`, …) |
| Branches | `br bne beq bge blt bgt ble bpl bmi bhi blos bvc bvs bcc bhis bcs blo` |
| Single operand | `clr com inc dec neg adc sbc tst ror rol asr asl swab sxt mfpi mtpi mfpd mtpd jmp` and the byte forms `clrb comb incb decb negb adcb sbcb tstb rorb rolb asrb aslb` |
| Processor status | `mtps mfps` |
| Double operand | `mov cmp bit bic bis add sub` and `movb cmpb bitb bicb bisb` |
| Subroutines | `jsr rts mark sob emt trap` |
| EIS | `mul div ash ashc xor` |
| FIS | `fadd fsub fmul fdiv` |
| FP11 | `ldf ldd stf std addf addd subf subd mulf muld divf divd cmpf cmpd tstf tstd absf absd negf negd modf modd clrf clrd ldfps stfps stst cfcc setf setd seti setl ldexp stexp ldcif ldcid ldclf ldcld ldcfd ldcdf stcfi stcfl stcdi stcdl stcfd stcdf` |
| Commercial / character | `movc movrc movtc locc skpc scanc spanc cmpc matc addn subn cmpn …` (the CIS set, with the `…i` variants) |

Convenience aliases:

| Alias | Equals |
|---|---|
| `push x` | `mov x, -(sp)` |
| `pop x` | `mov (sp)+, x` |
| `call x` | `jsr pc, x` |
| `callr x` | `jmp x` |
| `ret`, `return` | `rts pc` |
| `sys n` | `trap n` |
| `hlt` | `halt` |

Operand ranges are checked (for example `emt` and `trap` take an 8-bit number, `mark` a 6-bit one, `sob` jumps
backwards by at most 63 words), and violations are reported with a message.

---

## 8. Directives

Directive names start with a dot. They are case-insensitive.

### Data

| Directive | Action |
|---|---|
| `.byte n, …` (`.db`) | store bytes. A value must fit 8 bits (a negative number counts as its two's complement) |
| `.word n, …` (`.dw`) | store 16-bit words (little-endian). The address must be even |
| `.dword n, …` | store 32-bit values as two words, high word first |
| `.ascii "text"` | store the bytes of the text (in the output charset). Byte values in `<…>` may be mixed in |
| `.asciz "text"` | the same plus a zero byte |
| `.rad50 "text"` | pack the text into RADIX-50 words (3 characters per word; the alphabet is space, `A`–`Z`, `$`, `.`, `0`–`9`) |
| `.blkb n` | reserve *n* zero bytes |
| `.blkw n` | reserve *n* zero words |
| `.even` | if the address is odd, add one zero byte |
| `.odd` | if the address is even, add one zero byte |
| `.align n` | pad with zero bytes up to a multiple of *n* |
| `insert_file "file"` | insert the contents of a binary file |

### Location and flow

| Directive | Action |
|---|---|
| `.link addr` (or `. = addr`) | sets the address the program is **linked** (loaded) from; the default is `1000`. The first such directive of the main file gives the load address of the whole program, even if some code precedes it. A later one moves the location counter forward and fills the gap with zero bytes; moving backwards is an error |
| `.include "file"` | compile another source file at this point (path relative to the current file) |
| `.once` | the file can be included only once; further `.include`s of it do nothing |
| `.extern name, …` / `.extern all` | symbols visible to other files (see [9](#9-files-linking-and-visibility)) |
| `.repeat n { … }` | repeat the statements in braces *n* times |
| `.end` | stop reading this file. Inside `.repeat`, it ends the current repetition (like `continue`) |
| `.error "text"` | report an error with this text (useful in conditional code) |

`.repeat` example:

```asm
        .repeat 4 {
                .word 0
                nop
        }
```

### Accepted and ignored

`.list`, `.nlist`, `.title`, `.sbttl`, `.ident`, `.page` are parsed so that MACRO-11 sources are accepted; they do
nothing and produce the warning `-Wnot-implemented`.

### Output (see [10](#10-output-formats))

`make_bin`, `make_bk0010_rom`, `make_raw`, `make_sav`, `make_wav`, `make_turbo_wav`, `play_wav`, `play_turbo_wav`.

---

## 9. Files, linking and visibility

All files named on the command line, and all files pulled in by `.include`, form **one program**: the code of each file
follows the previous one. `.include` is practically the same as pasting the included file at that place.

The **link address** is where the program will be loaded (`.link`, `--link`, default `1000`). The location counter `.`
always shows the linked address, so labels contain final addresses and absolute references work.

If an included file contains its own `.link`, `.` is not moved: the file's code is still placed where the include
happens, but its labels are computed as if it were loaded from that address:

```asm
; main.mac
        .link 1000
        .word 1
        .word 2
        .word 3
        .include "inc.mac"

; inc.mac
        .link 2000
A:      .word A

; result (from address 1000):   000001 000002 000003 002000
```

### Visibility of symbols

By default the labels and constants of a file are visible only inside that file; a same-named symbol in another file
is a different symbol. `.extern` changes that:

* `.extern name1, name2, …` makes these symbols shared with the other files;
* `.extern all` shares every symbol of the file;
* a label defined with `name::` is shared as well; so is a constant defined with `name == value`. Local labels cannot
  be shared (`1::` is an error), but those that follow a shared label are available from other files as
  `name:N`, and every other symbol of the file as `name:Label` (see
  [Qualified names](#qualified-names-namen-and-namelabel)). So it is enough to share one uniquely named label in a
  library file: the rest stays private and cannot clash with the symbols of other files.

```asm
; a.mac                            ; b.mac
.extern LABEL_A                    LABEL_B = 3
LABEL_A = 1                        .word LABEL_A, LABEL_B
LABEL_B = 2
.word LABEL_A, LABEL_B
.include "b.mac"
.word LABEL_A, LABEL_B

; result: 000001 000002 000001 000003 000001 000002
```

(`LABEL_A` is shared, so `b.mac` sees it; `LABEL_B` is private to each file.)

### `.once` and library files

A typical use of `.include` is a library of routines or constants. Put `.once` at the top of the library so that
including it from several files does not define its labels twice:

```asm
; lib/hdd.mac
        .once
HddRead: …
```

---

## 10. Output formats

| Format | Option | Directive | File contents |
|---|---|---|---|
| `bin` | `--bin` | `make_bin ["file"]` / `make_bk0010_rom` | 2 bytes load address, 2 bytes length, then the image (the BK-0010 `.bin` file) |
| raw | `--raw` | `make_raw ["file"]` | the bare image |
| `sav` | `--sav` | `make_sav ["file" [, final_address]]` | a 512-byte RT-11 `.sav` header (start address at 040, stack 01000 at 042, final address at 050, block bitmap at 0360) followed by the image. `final_address` defaults to the end of the image |
| BK tape WAV | `--wav` | `make_wav ["file.wav" [, "name on tape"]]` | a `.wav` recording that the BK loads from a tape input |
| BK turbo WAV | `--turbo-wav` | `make_turbo_wav ["file.wav" [, "name on tape"]]` | the same in the turbo format |
| playback | `--play`, `--play-turbo` | `play_wav ["name on tape"]`, `play_turbo_wav [...]` | the recording is played through the speakers instead of being saved |

* Without a file name, the output goes to the source file name with the extension replaced (`prog.mac` → `prog.bin`,
  `prog.sav`, `prog.wav`; raw output has no extension).
* A directive in the source and a command-line option both work: the option adds one more output. If the source has
  at least one `make_*` directive, the default `.bin` is not written unless asked for.
* The name on the tape is up to 16 characters in the output charset. If omitted, it is made from the output file
  name (or the source name for `play_*`).
* `make_wav "~speaker"` plays the sound instead of writing a file (the same as `play_wav`).
* `.sav` places the image right after the header regardless of the link address, as the original PDPy11 does.
* Sound is played by the system: PlaySound on Windows, `afplay` on macOS, `paplay` or `aplay` on Linux.
* `--lst` writes a listing (`prog.lst`) with the addresses of the labels.

---

## 11. Macros and conditional assembly

PDPy11 has text macros (in the style of MACRO-11), conditional assembly and repeat blocks; they are expanded while the
source is read. The full description (in Russian) is in [MACROS.md](MACROS.md); here is a summary.

```asm
; Psw is 340 (interrupts disabled) unless given
	.macro	SetVec	Vec,Handler,Psw=340
		MOV	#Handler,@#Vec
		MOV	#Psw,@#Vec+2
	.endm

; ?Loop is a unique label for each call
	.macro	Delay	Count=1000,?Loop
		PUSH	R0
		MOV	#Count,R0
Loop:		SOB	R0,Loop
		POP	R0
	.endm

		SetVec	60,KeyIrq
		SetVec	100,Timer,200
		Delay				; 1000 iterations
		Delay	200
```

* `.macro name p1, p2=default, ?label` … `.endm`; `.mexit` leaves the expansion early. A parameter written `?name`
  receives a unique local label if no argument is given.
* An argument that contains commas is written in `<…>`. A macro may call other macros and itself (depth ≤ 64).
* The apostrophe joins a parameter to adjacent text: `x'y:` with `x=a`, `y=1` gives `a1:`.
* Macros from another file are made available with `.include`; use `.once` in the library.

Conditional assembly:

```asm
debug = 0                                 ; a constant defined above the condition
        .if eq  debug                       ; also: ne gt lt ge le, b, nb, idn, dif, df, ndf
                nop
        .iff
                halt
        .endc
        .iif ne size, .word size            ; one-line form
```

`.irp`, `.irpc` and `.rept` repeat a block until `.endr`. Conditions are evaluated while the text is read, so they can
only test constants (`name = value`) defined above them and macro arguments, not labels.

---

## 12. Diagnostics

Messages have the form `file:line:column: Error|Warning: text` (with `--report-format bare`) or a coloured excerpt of
the source (default). Errors make the exit status `1`; compilation goes on after most errors so that as many problems as
possible are found in one run.

Warnings are enabled and disabled with `-W`:

```
pdpy11 -Wall prog.mac          # every warning
pdpy11 -Wno-excess-hash prog.mac
pdpy11 -Wdefault -Wexcess-quote prog.mac
```

| Warning | On by default | Meaning |
|---|---|---|
| `implicit-operand` | yes | an operand was omitted and zero assumed (`.byte` with no value) |
| `not-implemented` | yes | an accepted but ignored directive (`.list`, `.title`, …) |
| `label-fixup` | yes | a number that looks like a local label was taken as one (`br 1 - 2`); add a colon or parentheses |
| `excess-hash` | yes | `#` on an instruction that takes an immediate operand implicitly (`emt #5`) |
| `suspicious-name` | no | a label, constant or operand that looks like an instruction or a register; usually a missing newline or comma |
| `excess-quote` | no | a closing quote after a character constant (`'A'`) |
| `meta-typo` | no | a directive written without its leading dot |
| `legacy-deferred` | no | an obsolete spelling of deferred addressing |
| `implicit-index` | no | `E(R)` forms that really mean index deferred with a zero offset |
| `source-encoding` | no | the source was not UTF-8 and the detected encoding was used |

Typical errors and what they mean:

| Message | Cause |
|---|---|
| "Unknown symbol 'x' is referenced here" | an undefined label or constant (also checked after everything is read, so forward references are fine) |
| "Duplicate symbol 'x:'" / "Duplicate variable 'x'" | a label or constant defined twice |
| "numbers are considered base-8 by default…" | a number with the digits 8 or 9 and no trailing dot |
| "value … does not fit in … bits" | a number too large for `.byte`, `.word` or an immediate field |
| "can only jump from … to …" | a branch is too far; use `jmp` |
| "'x' is an undefined instruction. Did you mean 'y'?" | a typo in an instruction or directive name |
| "This '.word' was emitted on an odd address" | add `.even` after byte data |
| "There is no file at path …" | a wrong path in `.include` or `insert_file` |
