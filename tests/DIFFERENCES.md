# pdpy11 (C) versus PDPy11 2.0.4 even-fix (Python): policy and known differences

## Policy (decided with the project owner)

The Python version is the reference **only for valid programs**: for them the C port must produce byte-identical
output files (`.bin`, `.raw`, `.wav`, `.lst`) and the same diagnostics (warnings).
For **invalid** programs the goal is *correct, understandable behaviour*, not a copy of Python's quirks or bugs:
every real error must be detected, reported with a clear message at the right place, the exit status must be 1,
and the compiler must never crash or hang. Python's crashes (internal errors, hangs) are not reproduced.

## Intentional improvements over the Python version (invalid input)

| Situation | Python | C port |
|---|---|---|
| `x = x`, `a = b` / `b = a` | hang or internal error | error `recursive-definition`: "The value of 'x' is defined in terms of itself..." |
| `.repeat 2 {` never closed | silently accepted | error `unclosed-block` pointing at the `{` |
| stray `}` | "Could not parse instruction" | "Unexpected '}': there is no matching '{'..." |
| unknown instruction `movv` | "Is our database incomplete? Report on GitHub" | "'movv' is an undefined instruction. Did you mean 'mov'?" |
| unknown local label `br 7` | "Unknown symbol '7'" | "Local label '7:' is not defined..." |
| `r9` used as a symbol | "Unknown symbol 'r9'" | same + hint "the registers are r0-r7, sp and pc" |
| undefined symbol in a branch / immediate | extra bogus "offset out of bounds (-518)" errors computed from the missing value | only the real error (derived range errors are suppressed) |
| identical diagnostic repeated (e.g. inside `.repeat`) | printed every time | printed once |
| fatal error in one statement | compilation stops or loses deferred diagnostics, depending on evaluation order | the statement is skipped, the rest of the file is still checked, exit status 1 |
| `. = . + 10` / link base depending on labels | error with an internal polynomial (`-d11+LA+7084`) | same error with the source expression |
| `.extern all` + a symbol declared external twice | `AttributeError` crash | normal `duplicate-symbol` error |

## Other differences

| # | Area | Difference |
|---|------|------------|
| 1 | Order of diagnostics | C reports in program order; Python resolves forward references lazily. The test harness compares diagnostics as sorted sets. |
| 2 | `--version` / internal error banner | `pdpy11 2.1.0 running on C`; `Version: pdpy11 2.1.0 (C)`, no Python traceback. |
| 3a | WAV amplitude | By the owner's request the C port writes WAV samples **16 (low) and 240 (high/middle)** instead of Python's 48 / 200 / 208 (`H`=`S`=240, `L`=16). Timing, headers and checksums are unchanged; `.wav` files therefore differ from Python's, `.bin`/`.raw` are not affected. |
| 3 | Sound | `~speaker` output is not implemented (a note is printed). WAV *files* (`make_wav`, `make_turbo_wav`) are written and are byte-identical to Python's. |
| 4 | Integers | Python integers are unbounded; C uses saturating `int64` (visible only beyond 2^63). |
| 5 | `--charset` | `bk`, `ascii`, `utf-8`, `latin-1`, `cp1251`, `cp866`, `koi8-r` and common aliases; other names give `'xxx' encoding is unsupported`. |
| 6 | Source encoding | **Detected automatically for every file separately, including `.include`d files** (see below). Python accepts only strict UTF-8 and rejects anything else. |
| 7 | Unicode classes | `\d`, `\w`, case folding follow ASCII rules (exotic input only). |
| 8 | Layout | Sizes depending on forward labels are solved by iteration (max 40 passes); non-convergence gives an internal error (Python would solve symbolic cases). |
| 9 | argparse | Help/usage/errors reproduce argparse for this CLI; abbreviations of long options work; other corner cases may differ. |

## Source encoding detection (new)

Every source file (the ones on the command line and every file pulled in by `.include`) is decoded on its own, so a
UTF-8 program can include a CP866 file, and so on. Everything is converted to UTF-8 internally; error positions are
counted in characters. Order of checks:

1. byte order mark: UTF-8 (removed), UTF-16 LE/BE;
2. UTF-16 without BOM (every other byte is zero);
3. valid UTF-8 (this includes plain ASCII);
4. otherwise the best of **cp1251, cp866, koi8-r** by log-likelihood under a Russian letter-frequency model
   (an all-capitals cp1251 text is not mistaken for lower-case koi8-r, box-drawing characters are penalised);
   if the best candidate does not decode mostly to Cyrillic letters the file is read as latin-1, which never fails.

`--source-charset NAME` (`auto` | `utf-8` | `utf-16` | `cp1251` | `cp866` | `koi8-r` | `latin-1`) overrides the
detection for all files; with an explicit charset a file that cannot be decoded is an error.
`-Wsource-encoding` (included in `-Wall`) tells which files were not UTF-8 and how they were decoded.
`insert_file` is binary and never decoded. The output charset (`--charset`, default `bk`) is independent of this.

On the BK corpus 39 sources are not UTF-8: 37 are detected as cp1251, 1 as koi8-r, 1 as UTF-16; Python cannot read them at all.
