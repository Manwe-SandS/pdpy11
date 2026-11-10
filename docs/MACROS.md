# Macros

**English** · [Русский](MACROS-RU.md)

A macro is a named piece of source text with parameters. When a macro is called, the compiler substitutes the actual
arguments into the text of the macro and parses the resulting text in place of the call. The model is taken from
MACRO-11 (PDP-11 MACRO-11 Language Reference Manual, chapter 7), but simplified.

## Definition

```asm
.macro Name parameter1, parameter2=value, ?label
		macro body
.endm
```

- `.macro` and `.endm` can also be written without the dot (`macro`, `endm`).
- A macro name may start with a dot. Letter case does not matter.
- Parameters are separated by commas or spaces.
- `parameter=value` is a default value. It is used when the argument is not passed or is empty.
  A value that contains commas is enclosed in `<…>`.
- `?parameter` is an automatic label: if the argument is not passed, a unique local label
  `30000$`, `30001$`, … is substituted (as in MACRO-11).
- Definitions can be nested: an inner `.macro` becomes known after the first call of the outer macro.
- A macro can only be defined before it is used (parsing goes from top to bottom).
- Defining a macro again with the same name replaces the previous one. A macro can override the name of an
  instruction or a directive: macros are checked before the built-in names.

Example: setting an interrupt vector, and a delay with an automatic label.

```asm
; PSW is 340 by default (interrupts disabled)
	.macro	SetVec	Vec,Handler,Psw=340
		MOV	#Handler,@#Vec
		MOV	#Psw,@#Vec+2
	.endm

; ?Loop: every call gets its own unique label
	.macro	Delay	Count=1000,?Loop
		PUSH	R0
		MOV	#Count,R0
Loop:	SOB	R0,Loop
		POP	R0
	.endm

		SetVec	60,KeyIrq
		Delay	200
```

## Call

```asm
[label:]	Name argument1, argument2, …
```

- Arguments are separated by commas. A comma inside parentheses or inside a string in double quotes does not split
  the argument.
- An argument that contains commas is enclosed in angle brackets: `Name <mov r0,r1>, 5`. The brackets are removed
  during substitution.
- A skipped argument: `Name ,b` (the first one is empty). An empty argument is replaced by the default value.
- More arguments than parameters is the error `macro-arguments`; the extra arguments are ignored.
- A comment (`;`, `//`, `/* */`) at the end of the call line is not part of the argument.

## Substitution

- A parameter is replaced as a whole word: in `mov a,b` with the parameter `a`, the word `ab` is not changed.
- Substitution is done everywhere in the body, including strings in quotes, except in comments.
- An apostrophe joins a parameter to the adjacent text and disappears itself: `x'y:` with `x=a`, `y=1` gives `a1:`.
  The apostrophe disappears only next to a parameter, so `#'A` remains a character constant.
- `.mexit` (or `mexit`) at the beginning of a body line stops the expansion of the macro.

## Nesting

The body of a macro can call other macros (including itself). The nesting depth of expansions is limited
(64 levels): exceeding it is the error `macro-recursion`.

## Macros from other files

Macros from a library are made available with an ordinary `.include`. The included file is parsed at once, in place of
the `.include` line, so the macros from it can be called in the lines below:

```asm
	.include	"macros.mac"
		SetVec	60,KeyIrq
```

`.once` is useful in the library file, so that including it again does not redefine the macros. If the library has a
syntax error, it is reported once.

## Diagnostics

An error in expanded text points to the expanded text, and the name of the source says where the macro was called
from: `file.mac [macro 'Name' called at line 12]`.

| Code | When |
|---|---|
| `invalid-macro` | no name, an invalid parameter, the macro is not closed with `.endm` |
| `macro-arguments` | more arguments than parameters |
| `macro-recursion` | the expansion depth is greater than 64 |

## Conditional assembly

Conditional directives are evaluated while the text is parsed (including inside macro expansions):

```asm
.if condition arguments   ; the block is assembled if the condition is true
.iff                      ; the block for a false condition
.ift                      ; the block for a true condition
.iftf                     ; the block in both cases
.endc
.iif condition arguments, statement     ; a one-line condition
```

| Condition | True if |
|---|---|
| `EQ` `NE` `GT` `LT` `GE` `LE` expression (synonyms `Z`, `NZ`, `G`, `L`) | the value of the expression is equal to / not equal to / greater than / less than … zero |
| `B <text>` / `NB <text>` | the text is empty / not empty |
| `IDN <a>,<b>` / `DIF <a>,<b>` | the texts are identical / different (letter case does not matter) |
| `DF name` / `NDF name` | the constant is already defined / not defined; `a&b` and `a!b` are allowed |

- Arguments are written in `<…>` or without brackets (up to a space or a comma). In `.iif`, the comma between the
  arguments and the statement is optional.
- Expressions in conditions are evaluated by the rules of MACRO-11: the operations `+ - * / & !` are on one level,
  from left to right; numbers are octal, a trailing dot makes them decimal, and `^O` `^D` `^B` `^X`, character
  constants and `<…>` are available.
- `.ntype name, operand` stores the addressing mode code of the operand in the constant `name` (register `Rn` = 0n,
  `(Rn)` = 1n, `(Rn)+` = 2n, `-(Rn)` = 4n, `x(Rn)` = 6n, `#x` = 27, `@#x` = 37, `x` = 67; `@` adds 10).
- `.irp name,<a,b,c>`, `.irpc name,<string>` and `.rept N` repeat a block up to `.endr`.
- `.radix n` (n = 2, 8, 10 or 16; no argument means 8) changes the base of the numbers without a prefix and without a
  trailing dot in the rest of the text, as in MACRO-11; it applies to the numbers in conditions too. It is read while
  the text is parsed, so it has no effect in a skipped `.if` branch, and the body of a macro is read in the radix in
  force at the place of the expansion. See [SYNTAX.md](SYNTAX.md#numbers) for details.
- `.mcall` is parsed and does nothing (macros are made available through `.include`).
- `.error ;text` (with no operand) reports the error `macro-error` with the text of the comment.
- Inside a macro expansion a constant can be assigned a new value (`x = 1 … x = 2`): the references below use the new
  value. Outside a macro, assigning again remains an error.
- Names of symbols and macros may start with dots (`...V1`, `.MACS`); the form `^O<377>` is allowed.

## Limitations

- There are no `.narg`, `.nchr`, `.enabl`/`.dsabl` directives and no `.psect`/`.asect` sections.
- There are no keyword arguments (`Name parameter=value`) and no `\x` numeric substitution.
- A macro cannot be called before it is defined.
- Conditions are checked while the text is expanded, not at compile time: values are known only for constants
  (`name = expression`) that have already appeared above. Labels cannot be used in conditions.
