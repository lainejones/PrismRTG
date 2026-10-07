# Driver architecture

The driver implementations are linked into PrismD. The surface interface
keeps pixel formats and memory ownership explicit across the native and
P96 adapter backends.

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
Unsupported requests decline before any command is submitted. Existing
void callbacks remain for diagnostic programs; Prism's render paths use
the result-bearing surface interface.

## Surfaces and planar operations

Each surface carries its exact pixel format, byte size, dimensions, pitch,
allocation size, logical VRAM offset and stable CPU pointer. Copy operations
receive both surfaces. Equal pixel sizes do not imply equal formats.
Drivers may decline layouts or geometry their commands cannot represent.

The optional planar operation receives the source BitMap and palette plus
the destination surface. P96 and UAE implement plain copies with a full
plane mask through their chunky/direct hooks. UAE probes each enabled
direct-colour format with a saved VRAM scratch region before scanout starts. A working host hook stays enabled; missing
or silently broken copies select the CPU default. Firmware versions alone
cannot distinguish affected hosts.
Other minterms and masks fall back to Prism's renderer. The CPU defaults
support interleaved planes and the null/all-ones plane sentinels.

## CPU access

Linear boards retain direct VRAM access. A driver setting `PBF_SHADOW`
must supply byte-range `read` and `write` operations. Its bitmaps retain
stable CPU shadows, including while applications hold bitmap pointers.
The driver owns address translation and aperture changes; the core
serializes transfers with rendering through its existing semaphore.

Displayed shadows are compared byte-for-byte against the last successful
upload. Only the changed span of each row crosses the bus. This detects
writes through retained application pointers without requiring new lock
rules or relying on hashes. If the comparison buffer cannot be allocated,
uploads fall back to the full bitmap. Accelerated operations read back only
the destination rectangle and update the comparison buffer. Each resident
shadow can therefore use two full CPU buffers: pixels and upload history.

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

## Driver lifetime

Drivers share PrismD's process and library bases. P96 and UAE interfaces
can retain board-context pointers and have no general release operation.
After claiming either card, PrismD keeps that context resident until
reboot, including after a failed startup.

## Development fixtures

`sh tests/architecture.sh` runs the operation-contract, Cirrus-limit,
ZZ9000 surface, replay, damage and P96 ABI fixtures using amiga-gcc and
vamos. The P96 and UAE guest exercisers run in an isolated Amiga boot;
all guest fixtures claim the virtual card until reset.
