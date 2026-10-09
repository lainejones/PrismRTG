# P96 card-driver backend

`drv_p96.c` lets Prism use a P96 card driver without running P96's RTG core.
Native Prism drivers remain the default. The adapter is explicitly selected
and does not scan or load every installed card driver.

## Configuration

Install Prism normally. For a card the installer does not recognize, choose
"Install anyway", then configure the backend before rebooting. Keep P96's
monitor programs disabled; the installer can park them. The `.card` and
any required `.chip` library must remain installed.

Add these settings to both `ENV:Prism.prefs` and `ENVARC:Prism.prefs`:

```text
BOARD=P96
P96CARD=LIBS:Picasso96/CyberVision64.card
```

The path is an example, not an assertion that every CyberVision64 driver
version is compatible. Use the actual card library you have installed.
For UAE's resident card library, use `P96CARD=uaegfx.card`. The emulator
must have UAE RTG memory enabled. Its hardware cursor is optional.

An optional `P96MONITOR` setting supplies driver-specific tooltypes from
a monitor icon. Give its path **without** `.info`, for example:

```text
P96MONITOR=SYS:Storage/Picasso96-parked/Monitors/CyberVision64
```

Prism reads this icon; it does not execute the monitor program. Keep the
icon's hardware-specific settings appropriate for the board. The driver
and icon paths also have command-line equivalents:

```text
Run >RAM:PrismD.log PrismD BOARD=P96 P96CARD=uaegfx.card
```

PrismPrefs can select "P96 driver" and preserves the two path settings
when saving. Set the paths in the text preferences file; the GUI does not
provide path editors. Card selection takes effect on the next boot.

## Supported interface

The adapter opens the card library, calls `FindCard` and `InitCard`, and
supplies the `BoardInfo`, system-library bases, lists and interrupt state.
The card driver can open its own chip library. Prism does not install a
replacement `rtg.library` for drivers that depend on private core services.

Mode setting, panning, palette updates and hardware sprites are forwarded
to the driver. Accelerated fills, same-bitmap copies and monochrome text
expansion have CPU fallback callbacks. Plain planar copies can use the
driver's planar-to-chunky or planar-to-direct hook, with CPU defaults.
For uaegfx, a startup probe selects the host hook or a CPU fallback; see
[the native UAE notes](uaegfx.md). Other rendering stays
with Prism.
The adapter waits for the blitter before handing control back to Prism.

Prism uses the driver's bitmap pitch requirements and carries the exact
pixel format through rendering calls. The adapter advertises every pixel
format the driver accepts. When all of them share one direct linear
mapping, bitmaps use direct VRAM pointers. If any accepted format needs a
displaced aperture, a memory-mode switch or has no direct access, or the
driver supplies its own allocator, soft-sprite handling or `ReInitMemory`,
the whole board runs in shadow mode (#9): bitmaps keep stable CPU shadow
buffers and the direct fast paths are not used. The adapter waits for the blitter, selects
the memory mode and translates addresses for each transfer. Applications
keep stable bitmap pointers while the adapter changes device mappings.

Shadow uploads send changed row spans, including writes through retained
bitmap pointers; after an accelerated operation only the rows of the
destination region are read back.
The comparison history consumes an additional CPU buffer. Banked mappings
are supported when `CalculateMemory` exposes each requested byte, using
conservative transfers and CPU rendering. Custom allocators can wrap the
adapter's allocator or own their pool, provided allocation and free hooks
are both available. Incompatible format changes can release device storage
and call `ReInitMemory` while preserving application shadows.

Drivers with `BIF_INTERNALMODESONLY`, including uaegfx, supply their own
mode lists. Other drivers receive standard timings for 640x400, 640x480,
800x600, 1024x768, 1280x720 and 1280x1024. Only modes fitting the driver's
limits and Prism's existing preferences grid are offered. P96 settings
files and arbitrary user-defined timings are not imported. External
drivers must provide a pixel-clock resolver; large clock deviations are
rejected.

Hardware cursor input is limited to 32x48 pixels. When the driver has no
sprite hooks, or its `EnableSoftSprite` asks for a software sprite in the
current mode, the core draws a software pointer into the shown bitmap;
`SOFTWAREPOINTER=ON` forces it over a non-working hardware sprite.

The compositor (#10) is merged but experimental. It supplies vertical
dragging between RTG screens, off unless `DRAGGING=ON` is set, and software
P96 RGB/CLUT memory-window PIPs. PIPs support scaling, cropping, brightness
and layer clipping; capture modes, YUV and custom render/save callbacks are
not implemented. Composition rebuilds the frame each tick, which is slow on
a 68030, and needs an extra frame buffer; to make room it moves hidden
bitmaps to fast RAM. It does not invoke card overlay engines.
See [driver architecture](driver-architecture.md) for the memory and
presentation contracts. Private `rtg.library` services and arbitrary bank
protocols remain outside this adapter's interface.

## Driver lifetime

The card ABI does not provide a general release operation. Drivers may
install interrupt servers, reserve hardware, or retain the `BoardInfo`
pointer outside Prism. Once a card is claimed, Prism therefore keeps its
task, callbacks, card/chip libraries and board data alive until reboot.

Ctrl-C still removes Prism's display patches once its screens and API
users have closed. The task then parks with the card switched back to
native video and its interrupts disabled where the driver supplies that
operation. It does not exit or release the card. Startup failures after
a card has been claimed also leave the driver context resident. Reboot
before trying another driver or restarting this backend.

## Development

The driver ABI headers are included under `src/p96sdk`, with their source
and attribution. The interface is documented by
[Individual Computers](https://wiki.icomp.de/wiki/P96_Driver_Development).
Structure offsets are checked at compile time, including the packed
three-byte palette entries.

`sh tests/p96-adapter.sh` builds and runs the register-ABI and software
fallback checks with amiga-gcc and vamos. `tests/p96_guest.c` is a separate
Amiga guest exerciser for uaegfx; run it only in a dedicated boot because
it claims the card and leaves the driver resident. It reports through the
Amiga serial port. Neither tool is included in the release drawer.

## Module packaging

The adapter is `LIBS:Prism/P96.driver`; the existing `.card` file and its
chip drivers remain in their usual locations. PrismD and the adapter must
come from matching builds. The adapter uses PrismD's output for diagnostics
and keeps a claimed card context resident independently after PrismD exits.
See [the module protocol](driver-modules.md) for lifetime and ABI details.
