# bradc-mini — TCC-class C-subset compiler for BradISA V2

**Status:** v0, shipped as a library + test. Not the general compiler — that is
the LLVM backend (Layer 1). This is Layer 2: a small, self-contained compiler
for kernel bring-up and the in-browser compile receipt.

## Subset (v0)

- Types: `int`, `void`, pointers (`int *`), and named structs with `int` fields
- Declarations:
  - functions with up to 8 params
  - locals: `int x;`, `int x = expr;`, `int a[N];`, `struct Point p;`
  - globals: scalar or array, with an optional constant initialiser
- Statements: assignment, `if`/`else`, `while`, `return`, blocks
- Expressions: literals, variables, calls, `+ - * / %`, `& | ^`, `<< >>`,
  `== != < > <= >=`, unary `-` and `!`, parentheses
- Address and aggregate access: `&x`, `*p`, `a[i]`, `s.field`
- No preprocessor, floating point, string literals, `for`, `do`, or `switch`.

Globals are emitted into a `.org 0x1000` data section, laid out cumulatively by
element count. Local arrays and structs occupy consecutive stack slots and are
zeroed at declaration.

## Usage

```c
#include "bradcmini.h"
struct brad_as_output out;
char err[256];
if (bradcmini_compile(src, &out, err, sizeof err) == 0) {
    /* out.code[0..out.count-1] are the machine words */
}
```

`bradcmini_to_asm()` returns the assembly text instead.

## Verification

`test_bradcmini` (ctest) compiles and **executes** the output on
`brad-emu-v2`, the in-process V2 emulator in `src/emu/brad_core_v2_emu.c`. It
checks:

- `add(3, 4) == 7` — calls and the ABI argument registers
- `sum(10) == 45` — loops and locals
- global read/modify/write
- local array indexing
- struct field access
- pointer write through `&x` / `*p`
- a combined program that uses all of the above

The emulator is a single-cycle functional model, not a timing model. It exists
so compiler changes can be checked end-to-end without the RTL testbench.

## Limitations

- Comparisons are signed.
- No register allocation beyond the temporaries; locals live on the stack.
- `&&`/`||` are not distinct from `&`/`|`.
- Struct fields are `int` only; no nested structs, struct assignment, or
  pass-by-value.
- Pointer arithmetic is byte-addressed, not scaled by the pointee size.
- Global struct arrays are not fully handled.
- Local array sizes are limited to 1..64; global arrays to 1..1024.
- Frame size reserves 8 temp slots and one spill slot for all functions.
