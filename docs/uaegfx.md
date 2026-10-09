# Native UAE RTG candidate

The native backend is packaged as `LIBS:Prism/UAEGFX.driver`. Select it
explicitly:

```text
Run >RAM:PrismD.log PrismD BOARD=UAEGFX
```

For boot-time use, set `BOARD=UAEGFX` in both `ENV:Prism.prefs` and
`ENVARC:Prism.prefs`. PrismPrefs calls this option "UAE (native)". If the
installer does not recognize the virtual board, choose "Install anyway"
and set the preference before rebooting. Automatic board selection does
not select this candidate yet.

Enable UAE RTG memory. The emulator's hardware cursor is optional. Keep P96's
monitor programs disabled, just as for other Prism drivers. Neither
`P96CARD` nor `P96MONITOR` is used by this driver.

## Interface

`src/drv_uaegfx.c` implements the Prism board interface independently of
`drv_p96.c`. Modern UAE publishes its RTG host traps through an
emulator-resident library named `uaegfx.card`. The driver opens that
already-resident interface, initializes its host context, and calls the
returned ROM entry points. It never loads a `.card` file from disk and
needs neither P96's core nor an installed P96 driver package.

This is not a new register-level interface to UAE: the emulator's wire
protocol uses P96-shaped BoardInfo, ModeInfo and RenderInfo records. The
shared SDK headers supply those binary layouts. The old numbered uaelib
RTG hooks are not used; modern UAE rejects them when its resident card
interface is installed.

Prism builds its own virtual modes at nominal 60 Hz within the emulator's
reported dimensions and available memory. It uses tightly packed bitmap
rows and one enabled format per byte depth (8, 16, 24 and 32 bits). It does
not import P96 mode files or use the generic adapter's mode selection.
Host refresh and presentation remain emulator settings.

Palette changes, display switching, panning, fills, overlapping copies
and hardware sprites use UAE host operations. Fill and copy have CPU
fallbacks if UAE declines an operation. Plain planar copies use the host
hook when a startup probe (#9) confirms its output in each direct-colour format;
missing or broken hooks use a CPU fallback. Other drawing uses Prism's core.
Host blits complete synchronously. Vblank waits use graphics.library's
WaitTOF; UAE's card WaitVerticalSync entry point is a no-op.

UAE RTG memory is linear, so bitmaps use direct VRAM pointers; this driver
does not use shadow mode. The hardware cursor is limited to 32x48 pixels
and is used when WinUAE or Amiberry provides one. Without it, for example
with WinUAE's hardware sprite turned off or in the libretro build, Prism
draws its software pointer into the shown bitmap. The experimental
compositor (#10) supplies vertical RTG screen dragging (`DRAGGING=ON`) and
RGB/CLUT P96 memory-window PIPs. It does not use UAE's host overlay or
split hooks. There is no multi-board support, and compatibility with every
UAE fork or version is not implied. See
[driver architecture](driver-architecture.md) for the pointer, composition
costs and PIP limits.

## Lifetime

UAE retains the host context and installs interrupt servers during card
initialization. Ctrl-C removes Prism's display patches after its screens
and API users close, disables the card interrupt, and switches to native
Amiga video. The driver module and its context then remain resident until reboot;
PrismD itself exits. The final diagnostic is flushed before PrismD closes
its output.
The generic adapter and native driver share a claim guard; reboot before
changing between them or restarting either one.

## Development

The host-side reference implementations are WinUAE's
[Unix RTG backend](https://github.com/tonioni/WinUAE/blob/master/od-unix/rtg.cpp)
and [Windows RTG backend](https://github.com/tonioni/WinUAE/blob/master/od-win32/picasso96_win.cpp).

`tests/uaegfx_guest.c` exercises the native driver in a dedicated UAE boot,
including all four byte depths, mode bounds, host fills and copies,
fallback rendering, palette and sprite calls. It reports through the
Amiga serial port and retains the context until reset. `sh tests/architecture.sh`
builds it (amiga-gcc, `-O2 -m68020-60 -noixemul -Isrc -Isrc/p96sdk`,
linked with `src/boardops.c` and `src/rtg_ops.c`).
