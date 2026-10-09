# PrismRTG tests

## Running

Inside WSL, from the repository root:

    sh tests/architecture.sh      # whole host suite (runs p96-adapter.sh too)
    sh tests/p96-adapter.sh       # the P96 adapter fixture on its own

Needs only amiga-gcc (`/opt/amiga/bin`, added to `PATH` by the scripts) and
vamos from amitools (`vamos` on `PATH`, else `~/amitools-venv/bin/vamos`;
override with `VAMOS=...`, the compiler with `CC=...`). Each fixture is
built as a 68020 AmigaOS program and run under vamos (`-C 68020`); a failed
`assert` aborts the program and the script stops with a non-zero exit.
Binaries go to `out/tests/`. The suite ends with `All host tests passed.`

Fixtures `#include` the driver/source file they test and stub the rest of
PrismD. They define the library bases they mention (`GfxBase = NULL`, ...)
so libnix does not auto-open graphics.library or expansion.library, which
vamos does not provide. When a `src/` change adds a new external call to
one of these files, the test needs a matching stub.

## Host fixtures (run under vamos)

| Fixture | Checks |
|---|---|
| `boardops.c` | `board_fill/copy/line` contracts: bounds and format rejection, retry vs. failure, `PBF_ACCEL_BROKEN` after a fault |
| `cirrus_limits.c` | Picasso II/GD542x/5434 blitter surface limits and the blit-timeout path (`drv_picasso2.c`) |
| `zz_surfaces.c` | ZZ9000 mailbox commands: 15-bit fills/lines via the 565 colour mode, same-surface vs. between-surface byte copies, declines (`drv_zz9000.c`) |
| `render_replay.c` | `render.c` fill/line/template/blit: CPU replay after a failed or retried hardware op, read-modify-write guards |
| `damage.c` | `changed_span` (shadow damage spans) |
| `shadow_upload.c` | `bitmap.c` shadow uploads racing a writer, partial-write failure, readback, eviction to fast RAM and back, low memory |
| `pointer_mode.c` | `pointer.c`: hardware cursor used or dropped per mode |
| `presentation.c` | `present.c` + `pointer.c`: no composition for the pointer alone, PIP/dragging composition, shadow-mode in-place scanout, split screens, damage-only writes, allocation/upload failure keeps the hardware sprite, software sprite with save-under, CLUT remapping |
| `driver_module.c` | Driver module request: ABI, size, board layout and ops ABI mismatches are rejected |
| `p96_adapter.c` (`p96-adapter.sh`) | `drv_p96.c` through the real 68k register ABI: fills, overlapping blits, templates, mode setup order, card memory pool and aperture, banked memory, planar probe |

## Guest exercisers (built, not run: reported as SKIP)

These need a real AmigaOS boot (WinUAE). `architecture.sh` builds them into
`out/tests/guest/` so they keep compiling. They report on the Amiga serial
port (`PASS` / `FAIL ...`) and claim the card until reset, so run each in a
dedicated boot with serial output captured, never on a working system.

| Program | Source / flags | What it does |
|---|---|---|
| `p96_guest` | `p96_guest.c` + `src/boardops.c src/rtg_ops.c` | Probes `uaegfx.card` through Prism's P96 adapter; fills, copies, templates and cursor at 8/16/24/32-bit |
| `uaegfx_guest` | `uaegfx_guest.c` + `src/boardops.c src/rtg_ops.c` | Same for the native UAE driver, plus mode limits, planar conversion through host and fallback callbacks |
| `driver_guest` | `driver_guest.c` (`-DDRIVER_ARGUMENTS='"BOARD=..."'`, default `BOARD=UAEGFX`) | Starts `C:PrismD`, runs `C:p96test DEPTH=8/16/24/32`, stops PrismD, checks its semaphore is gone. Needs the real modules in `LIBS:Prism/` |
| `driver_module_guest` | `driver_module_guest.c` | Boot task: runs `C:ModuleCheck >RAM:module.log` and checks the log for each expected message |
| `ModuleCheck` | `driver_module_guest.c -DMODULE_CHECK` + `src/driver_loader.c` | Loads `MissingFixture` (absent), `OldFixture`, `FailedFixture` and `Fixture` (twice) through `driver_open` |
| `Drivers/Fixture.driver` | `module_fixture.c` | Module that answers READY, reports its stack size as `vramSize`, writes final output on STOP |
| `Drivers/FailedFixture.driver` | `module_fixture.c -DFIXTURE_NOT_FOUND` | Module whose probe fails (`PRD_NOT_FOUND`) |
| `Drivers/OldFixture.driver` | `module_fixture.c -DPRISM_DRIVER_ABI=2` | Module built for an older ABI (mismatch path) |

For the module test, copy `ModuleCheck` to `C:` and the three fixtures to
`C:Drivers/` (ModuleCheck looks in `PROGDIR:Drivers/`, then `LIBS:Prism/`),
and boot `driver_module_guest` as the startup program. It cannot run under
vamos: vamos has no `CreateNewProc`, so no driver process can be started.
