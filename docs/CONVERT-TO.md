# convert-to: a source converter

`convert-to` rewrites an assembler source so that it can be compiled by PDPy11 (or by Turbo8). It is a separate
program; it ships in every release archive next to `pdpy11` and can be built with `make convert-to`.

Typical uses:

* take a MACRO-11 or Turbo8 source with macros, conditional assembly and its own operator rules and get plain
  PDPy11 code that `pdpy11` compiles to the same bytes;
* check what a macro really expands to;
* move a PDPy11 program to Turbo8.

```
convert-to [--from=pdpy11|turbo|micro11] pdpy11|turbo  file
```

| Argument | Meaning |
|---|---|
| `pdpy11` or `turbo` (any case) | the dialect to **produce** |
| `file` | the source to convert |
| `--from=…` | the dialect the source was **written for**; it decides how operator precedence is converted (see [Operators](#operators)). Default: the opposite of the target. `--from=micro11` also switches on the [Micro-11 rules](#micro-11-sources) |

The result is saved next to the source with the dialect appended to the whole name: `prog.mac` → `prog.mac.pdpy11`
(or `prog.mac.turbo`). The original is never changed. Exit status: `0` success, `1` the source cannot be converted
(a message with the line number is printed), `2` wrong command line.

```
convert-to pdpy11 prog.mac && pdpy11 --implicit-bin prog.mac.pdpy11
```

## How changes are marked

Every change that is not completely obvious is shown in the result:

* a changed line is preceded by a comment with the original text: `; was: <original line>`
  (`// was: …` in the Turbo8 dialect);
* every macro call stays as a comment line (`; Clear2 r2, 5`) followed by its expansion;
* removed definitions (register synonyms, redefined constants, unsupported directives) leave an explanatory comment;
* warnings about things that need attention go to the standard error output.

Only the quoting of character constants (`'.` → `"."`) is changed silently: it is too obvious to comment.

## Both directions

### Macros

`.macro … .endm` definitions are removed and every call is replaced by the expansion, so the result contains no macros.
Macro libraries pulled in with `.include` are read to learn the macros; an `.include` of a file that only defines
macros is dropped from the result (a file with real code stays included). `.mcall` lines are dropped. The built-in
macros `.addr` and `.print` are expanded in the PDPy11 dialect and left alone in the Turbo8 one.

```asm
        .macro  Clear2  reg, val=0
        mov     #val, reg
        mov     #val, reg
        .endm
        Clear2  r2, 5
```
becomes
```asm
; Clear2        r2, 5
        mov     #5, r2
        mov     #5, r2
```

Macro semantics (parameters, `?label` automatic labels, `<…>` arguments, the apostrophe, `.mexit`, nesting) are those
described in [MACROS.md](MACROS.md).

### Conditional assembly and repeats

`.if` / `.iff` / `.ift` / `.iftf` / `.endc` and `.iif` are evaluated during conversion, and only the lines of the true
branches are written. `.irp`, `.irpc` and `.rept` blocks are expanded. Conditions: `eq ne gt lt ge le` (`z nz g l`),
`b`, `nb`, `idn`, `dif`, `df`, `ndf`. The values come from the constants (`name = number`) and `.ntype` results that
appear **above** the condition and from macro arguments; a condition that depends on a label cannot be evaluated and
stops the conversion with an error. Expressions in conditions follow MACRO-11: `+ - * / & !` are one level, evaluated
left to right; numbers are octal, `5.` is decimal, and `^O ^D ^B ^X` and `<…>` are accepted.

`.error ;text` is turned into a comment and also reported on the standard error output.

### Constants redefined inside macros

MACRO-11 lets a macro assign a constant again (`x = 1 … x = 2`); PDPy11 does not. The assignment is replaced by a
comment, and the references that follow use the value in force, written as a number (`x` → `2.`).

### Other

* Trailing zero bytes (the padding of an RT-11 file) are cut off.
* A line without a final newline stays without it.

## Producing PDPy11 source (`convert-to pdpy11`)

| What | Change |
|---|---|
| Character constants | `movb #'.,r0` → `movb #".",r0` |
| Register synonyms | the Turbo8 declaration `%1 = counter` is removed (with a comment) and every `counter` becomes `r1` |
| Comments | `//` and `/* … */` comments are rewritten as `;` comments |
| Directives | `.la` → `.link`; `.insert` → `insert_file` |
| Operators | mixed operators are grouped with parentheses, see [Operators](#operators) |
| `.ascii`, `.asciz` | `<15><12>` byte values are kept (they are not arithmetic groups); text in other delimiters (`\text\`) is written in double quotes |
| Names starting with `$` | `$mail` → `S$mail` (PDPy11 cannot start a line with such a name after a word list); every use is renamed |
| `.list`, `.nlist`, `.asect` | removed (with a comment): they have no meaning for PDPy11 |

```asm
%1 = counter
        mov     #<1+2>*3, counter
        .la     1000
```
`convert-to --from=turbo pdpy11` produces
```asm
; register synonym counter = r1: the name is replaced by r1 below
; was: mov      #<1+2>*3, counter
        mov     #(1+2)*3, r1
; was: .la      1000
        .link   1000
```

## Producing Turbo8 source (`convert-to turbo`)

| What | Change |
|---|---|
| Comments | `;` comments become `//` comments |
| Directives | `.link` → `.la`; `insert_file` → `.insert` |
| `.end` | appended when the source has none (Turbo8 requires it) |
| Directives Turbo8 does not have | `.once .repeat .dword .odd .error .list .title .extern` are left in place and reported on the standard error output, so that you can rewrite them by hand |
| Operators | mixed operators are grouped with `< >`, see below |

## Operators

The two assemblers order the binary operators differently (lowest to highest):

| PDPy11 | Turbo8 |
|---|---|
| `\|` | `\|` `^` (one level) |
| `^` | `&` |
| `&` | `+` `-` |
| `<<` `>>` | `*` `/` `%` `<<` `>>` (one level) |
| `+` `-` | |
| `*` `/` `%` | |

The converter parses every expression with the table of the dialect given by `--from` (by default the opposite of the
target) and writes it for the target dialect. Wherever the two orders would give different results it adds explicit
groups: `( )` in PDPy11, `< >` in Turbo8, so the value never changes:

```
convert-to --from=pdpy11 turbo      mov #1<<2+1, r0    →    mov #1<< <2+1>, r0
```

A group that contains a register name is always written with `< >`, because in PDPy11 parentheses around a register
mean register-deferred addressing.

## Micro-11 sources

`--from=micro11` handles the conventions of the Micro-11 assembler (target `pdpy11` or `turbo`):

* **comments without `;`.** After the mnemonic the converter counts the operands the instruction takes. If the text
  that follows does not begin with an instruction, a directive (a word with a dot) or a known macro, it is a comment
  and a `;` is inserted (`clr r0  clear the register` → `clr r0  ; clear the register`). A tail that begins with an
  instruction name is kept as code;
* **`.+n`** (reserve *n* bytes) → `.blkb n`; **`TYIN`** → `emt 6`; **`TOUT`** → `emt 16` (with a `was:` comment);
* **labels defined again.** Micro-11 allows the same label to be defined repeatedly; the second definition becomes
  `NAME_2`, the third `NAME_3`, and references below it use the newest name (with a comment). References above keep
  the earlier name;
* **character constants** `'AJ'` (two characters closed by a quote) → `"AJ`, and `'Y'` → `'Y`.

## Limitations

* Conditions can only test constants defined above them and macro arguments, not labels.
* Macros with keyword arguments and `\x` numeric substitution are not supported.
* A macro must be defined before it is called.
* Only the dialect differences listed above are converted; the program itself is not analysed, so semantic differences
  between assemblers (for example, addressing details) are not touched.
* The result should be compiled and compared with the original build: the converter keeps `was:` comments exactly to
  make such a review easy.
