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
must have UAE RTG memory enabled. Enable its hardware cursor as well.

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
expansion have CPU fallback callbacks. Other rendering stays with Prism.
The adapter waits for the blitter before handing control back to Prism.

Prism uses the driver's bitmap pitch requirements. The adapter chooses
one format per byte depth from the formats Prism supports, retaining only
formats that can coexist in the same linear CPU aperture. It excludes
formats whose `CalculateMemory` mapping differs from the logical VRAM
address. A driver that replaces the VRAM allocator is rejected; private
allocations through the core return failure.

Drivers with `BIF_INTERNALMODESONLY`, including uaegfx, supply their own
mode lists. Other drivers receive standard timings for 640x400, 640x480,
800x600, 1024x768, 1280x720 and 1280x1024. Only modes fitting the driver's
limits and Prism's existing preferences grid are offered. P96 settings
files and arbitrary user-defined timings are not imported. External
drivers must provide a pixel-clock resolver; large clock deviations are
rejected.

Hardware cursor input is limited to 32x48 pixels. Prism has no software
cursor fallback: a driver or pixel format requiring one will not show an
RTG pointer. Screen dragging, overlays, special memory allocators and
aperture switching between incompatible formats are not supported by this
backend. It is a compatibility path for suitable drivers, not full P96
board or feature parity.

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
