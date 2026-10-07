# Native UAE RTG candidate

Select the native driver explicitly:

```text
Run >RAM:PrismD.log PrismD BOARD=UAEGFX
```

For boot-time use, set `BOARD=UAEGFX` in both `ENV:Prism.prefs` and
`ENVARC:Prism.prefs`. PrismPrefs calls this option "UAE (native)". If the
installer does not recognize the virtual board, choose "Install anyway"
and set the preference before rebooting. Automatic board selection does
not select this candidate yet.

Enable UAE RTG memory and the emulator's hardware cursor. Keep P96's
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
fallbacks if UAE declines an operation. Other drawing uses Prism's core.
Host blits complete synchronously. Vblank waits use graphics.library's
WaitTOF; UAE's card WaitVerticalSync entry point is a no-op.

The hardware cursor is limited to 32x48 pixels. Prism has no software
cursor fallback, so disabling UAE's hardware cursor leaves no RTG pointer.
There is no overlay, screen-dragging or multi-board support in this
candidate. Compatibility with every UAE fork or version is not implied.

## Lifetime

UAE retains the host context and installs interrupt servers during card
initialization. Ctrl-C removes Prism's display patches after its screens
and API users close, disables the card interrupt, and switches to native
Amiga video. The context and task then remain resident until reboot.
The generic adapter and native driver share a claim guard; reboot before
changing between them or restarting either one.

## Development

The host-side reference implementations are WinUAE's
[Unix RTG backend](https://github.com/tonioni/WinUAE/blob/master/od-unix/rtg.cpp)
and [Windows RTG backend](https://github.com/tonioni/WinUAE/blob/master/od-win32/picasso96_win.cpp).

`tests/uaegfx_guest.c` exercises the native driver in a dedicated UAE boot,
including all four byte depths, mode bounds, host fills and copies,
fallback rendering, palette and sprite calls. It reports through the
Amiga serial port and retains the context until reset. Compile it with
amiga-gcc using `-O2 -m68020-60 -noixemul -Isrc -Isrc/p96sdk`.
