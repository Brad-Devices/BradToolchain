# Brad Toolchain — assembler, bradc-mini, Brad-CVT, BVRT
[![CI](https://github.com/Brad-Devices/BradToolchain/actions/workflows/ci.yml/badge.svg)](https://github.com/Brad-Devices/BradToolchain/actions/workflows/ci.yml)


The reference toolchain for **BradISA V2**, built from the same sources that
produce the receipts on <https://brad-devices.vercel.app/now/>.

| Component | What it does |
|---|---|
| `asm/` | BradISA V2 assembler — every mnemonic, format, ABI register name and error path |
| `bradc-mini/` | TCC-class C-subset compiler: int/void, up to 8 arguments, locals, `if`/`else`, `while`, globals, arrays, pointers, int-field structs |
| `bradvector/` | BradVector host tools — `bradc`, the `.bvbc` container, BVRT and the timeline debugger |
| `brad-cvt/` | Brad-CVT: PTX text in, `.bvbc` out, lowered to the BradVector runtime |
| `emu/` | V1 and V2 in-process emulators; the V2 one executes bradc-mini's own output |
| `tests/` | Six suites: assembler, bradc-mini, BVRT, BVRL, Brad-CVT, V2 emulator |

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires CMake 3.28+ and a C23 compiler. No external dependencies.

## What this repository deliberately does not contain

This is a **curated** tree, not a mirror of the working repository. The
BradSense driver layer, its temporal core and its object-class table stay
private: they are the deep moat named in `COMPANY/POLICIES.md` §3, not a
receipt. `ci/boundary.sh` fails the build if `drivers/`, `bradsense`, or
`tifa` ever appear here. That is deliberate — the boundary is checked, not
promised.

The ISA contract lives in [BradISA](https://github.com/Brad-Devices/BradISA).
The vector ISA specification lives in
[BradVector](https://github.com/Brad-Devices/BradVector).

## Licence

Apache-2.0. The wordmark and the names "Brad Devices" and "BradVerse" are
reserved trademarks (see `TRADEMARKS.md`).
