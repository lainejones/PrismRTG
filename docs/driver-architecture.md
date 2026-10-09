# Driver architecture

Each board driver is a separate module, `LIBS:Prism/<NAME>.driver`, which
PrismD loads and starts as its own process (`src/driver_loader.c`,
`src/driver_module.c`; `PROGDIR:Drivers` is tried first). See
[the module protocol](driver-modules.md). The surface interface (#6) keeps
pixel formats and memory ownership explicit across the native and P96
adapter backends.

## Operation results

`PrismOps` operations return `PR_DONE`, `PR_DECLINED`, `PR_RETRY` or
`PR_FAILED`. Declining promises that no destination pixels were written,
so the core can use its CPU renderer. A failure may leave partial output;
the driver must stop its engine before returning. The core can replay
safe overwrite operations, but not read-modify-write operations such as
overlapping copies. `PR_FAILED` disables further acceleration; `PR_RETRY`
lets a driver disable only the failed capability. A Cirrus text timeout
therefore preserves fill and copy acceleration.

Cirrus checks pitch, dimensions and its implemented address range before
issuing commands. Its bounded wait resets the blitter on a timeout.
Unsupported requests decline before any command is submitted.

The common case has a shorter route. For one of Prism's own VRAM bitmaps,
the inline fast paths `pbm_fill`, `pbm_copy` and `pbm_expand` in
`src/prismint.h` call the board's direct `fillRect`, `copyRect`/`copyBetween`
and `expandRect` hooks when the blit fits `board.blitMaxBytes`,
`blitMaxRows` and `blitMaxPitch`. On a 68030 the extra call levels of the
surface layer cost a noticeable part of a small blit. These paths stand
down for shadow boards, broken or software-only acceleration and while the
compositor reuses scanout (`PBF_PRESENT`). A driver that finds its blitter
wedged sets `PBF_ACCEL_BROKEN`, which returns `PR_FAILED`; a text timeout
returns `PR_RETRY`. Everything else goes through `PrismOps`, with the
decline, failure and CPU replay rules above.

## Surfaces and planar operations

Each surface carries its exact pixel format, byte size, dimensions, pitch,
allocation size, logical VRAM offset and stable CPU pointer. Copy operations
receive both surfaces. Equal pixel sizes do not imply equal formats.
Drivers may decline layouts or geometry their commands cannot represent.

The optional planar operation receives the source BitMap and palette plus
the destination surface. P96 and UAE implement plain copies with a full
plane mask through their chunky/direct hooks. UAE probes each enabled
direct-colour format with a saved VRAM scratch region before scanout starts.
A working host hook stays enabled; missing or silently broken copies select
the CPU default. Firmware versions alone cannot distinguish affected hosts.
Other minterms and masks fall back to Prism's renderer. The CPU defaults
support interleaved planes and the null/all-ones plane sentinels.

## CPU access

Linear boards normally retain direct VRAM access. `PBF_SHADOW` bitmaps
retain stable CPU shadows, including while applications hold pointers.
Nonlinear boards supply byte-range `read` and `write` operations; on
linear boards without them, the compositor's uploads use a plain copy.
The driver owns address translation and aperture changes; the core
serializes transfers with rendering through its existing semaphore.

Shadow transfer tracking and driver memory pools came with #9. Displayed
shadows are compared byte-for-byte against the last successful upload. Only the changed span of each row crosses the bus. This detects
writes through retained application pointers without requiring new lock
rules or relying on hashes. If the comparison buffer cannot be allocated,
uploads fall back to full-bitmap transfers through a bounded snapshot.
Each upload uses an immutable snapshot, so application writes during an
upload remain dirty. Accelerated operations read back only the destination
rectangle and update the comparison buffer. Each resident shadow can use two
full CPU buffers: pixels and upload history. Eviction frees the history.

The P96 backend accepts displaced apertures and memory modes selected by
`SetMemoryMode`. Non-contiguous `CalculateMemory` mappings use a conservative
byte-at-a-time transfer, consuming each address before translating the next.
Such boards use CPU rendering. A driver must expose a CPU mapping through
this contract; a private bank-switch protocol is not inferred.

Optional paired `allocate`/`release` operations let a driver own its VRAM
pool. The core checks returned offsets and tracks its live allocations.
P96 supplies first-fit, absolute-allocation and free defaults which a card
can wrap for stricter alignment. Private driver allocations share that
pool. Requests never silently evict Prism bitmaps through P96's force flags;
Prism owns eviction. A `PBF_REINIT` driver requires shadow storage: the core
releases device allocations before setting another mode, allowing the
driver's `ReInitMemory` hook to rebuild its pool for an incompatible format.
Application pointers remain stable throughout.

## Software pointer

`src/pointer.c` draws the pointer as a sprite directly into the shown
bitmap and keeps a save-under of the pixels it covers. It is used when the
board has no working hardware cursor (no `PBF_HW_CURSOR` or cursor hooks,
or P96's `EnableSoftSprite` asks for a software sprite in the current
mode), or when `SOFTWAREPOINTER=ON` is set in `ENV:Prism.prefs`, for
example on MiSTer configurations without a working sprite. Hardware
sprites remain preferred otherwise. The pointer does not set shadow mode
and does not need the compositor.

Drawing takes the pointer out only when it would touch it. `LOCK()` removes
it before any drawing, `LOCK_FOR(p)` only when `p` is the bitmap it is
drawn into (bitmap locks, allocation, freeing), and `draw_lock()` and
`SW_CLEAR()` in the render paths remove it only when the drawn rectangle,
or a copy's source rectangle, overlaps the pointer on the shown bitmap.
Copies read their surfaces again once the lock is held, since VRAM paging
can move a bitmap while a task waits for it. PrismD's tick restores it once a tick has passed without
drawing. The sprite keeps all 64x64 decoded pixels and clips at the screen
edges; it is not drawn into a locked bitmap.

## Presentation (experimental)

The compositor in `src/present.c` (#10) is merged but experimental, and
does nothing unless it is needed. It supplies vertical RTG screen splits,
which are off unless `DRAGGING=ON` is set in `ENV:Prism.prefs`, and P96
memory-window PIPs, which compose whenever an application has one open.
Both preferences survive a save through PrismPrefs.

It copies the front screen at its dragged origin, fills the exposed area
from the immediately behind screen when it is RTG, converts differing
formats, and adds each visible screen's PIPs and the pointer. While a frame
is being composed the pointer is drawn into that frame. Native-chipset
pixels are not sampled. The front screen's palette is used for indexed
scanout.

Application bitmap pixels are never used as a PIP save-under. The
compositor owns a CPU frame and comparison buffer and uploads changed row
spans. It normally reserves a separate scanout allocation through
`vram_get_size`, which moves hidden bitmaps to fast RAM to make room; a
shadow-backed screen can reuse its own VRAM instead. During that reuse
(`PBF_PRESENT`), rendering stays in CPU shadows and the direct fast paths
stand down, so accelerated readback cannot copy overlays into application
data. Stopping presentation restores the original pixels and scanout before
releasing storage.

A working hardware pointer remains visible until the first composed frame
has been uploaded. Failed buffer allocation is retried after a cooldown,
or immediately for an explicit new PIP request.

The compositor rebuilds the whole frame on every tick and compares it
against the last upload. Only changed spans cross the bus, but the
rebuild itself is slow on a 68030. This is not a hardware overlay or
page-flip implementation.

P96 memory windows expose their source bitmap and RastPort, with RGB/CLUT
formats, nearest-neighbor scaling, source cropping, placement, brightness,
palette updates and visible-layer clipping. Capture modes, YUV sources and
custom rendering/saving callbacks remain unavailable. Source dimensions
and format are fixed for the lifetime of the PIP window. Layout constraint
and policy tags such as `P96PIP_Alignment`, `P96PIP_AllowCropping` and
`P96PIP_InitialIntScaling` are not implemented; callers must not rely on
those constraints being enforced.

## Driver lifetime

Each driver runs in its own process with its own C runtime and library
bases; PrismD calls the callbacks it publishes while that process stays
alive. Native drivers end when PrismD stops them. P96 and UAE interfaces
can retain board-context pointers and have no general release operation.
After claiming either card, the driver process keeps that context resident
until reboot, including after a failed startup, while PrismD itself can
exit.

## Development fixtures

`sh tests/architecture.sh` runs the operation-contract, Cirrus-limit,
ZZ9000 surface, replay, damage, shadow upload, pointer mode, presentation,
driver-module request and P96 ABI fixtures using amiga-gcc and vamos. It
also builds the guest programs (P96 and UAE exercisers, the driver and
module-loader checks) into `out/tests/guest` and lists them as skipped:
they need a real or emulated Amiga boot, and all of them claim the
virtual card until reset. `tests/README.md` describes each test and how
to run the guest programs.
