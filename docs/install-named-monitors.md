# Installing on systems that name their monitors (Caffeine OS)

A stock AmigaOS Startup-Sequence starts every monitor in `DEVS:Monitors`,
and Prism's monitor file (`DEVS:Monitors/Prism`) is what starts PrismD at
boot. Some distributions load only the monitors they name instead.
Caffeine OS, for one, has lines like

```text
loadMonDrvs >nil: "emu68-videoCore|Native|Generic"
```

and picks one of them depending on the machine (`IF $BOARDNAME EQ "UAE"`
and so on). Prism's monitor is never started there, so PrismRTG would be
installed but never run. Adding `Prism` to those names does not help.

Since 1.2 beta 2 the installer handles this. These are all the changes
it makes, and Remove undoes each of them.

## The Startup-Sequence

`PrismSetup STARTUP=CHECK` looks for a `LoadMonDrvs` line that is given
names (a quoted argument; the stock command takes none). If it finds
one, the installer asks before changing anything, then
`PrismSetup STARTUP=ADD` inserts this block:

```text
;BEGIN PrismRTG - LoadMonDrvs here starts only the monitors it names
IF EXISTS DEVS:Monitors/Prism
 DEVS:Monitors/Prism >NIL:
EndIF
;END PrismRTG
```

Where it goes:

- after `LAB ENDMONITORS` when there is one (Caffeine OS);
- otherwise after the last `LoadMonDrvs` line, or after the `EndIF`
  that closes the `IF` around it, so the block runs on every path;
- never inside an `IF` that is not closed: then nothing is added and
  the installer says PrismRTG will not start at boot until
  `DEVS:Monitors/Prism` is run from the Startup-Sequence.

The file as it was is kept as `S:Startup-Sequence.before-prism`, with
its protection bits. `PrismSetup STARTUP=REMOVE` takes the block out
again (the rest of the file stays exactly as it is, including later
edits) and deletes the `.before-prism` copy, which is out of date by
then. CHECK returns 0 when the block is there, 5 when this
Startup-Sequence needs none, and 10 when one is missing.

## The Workbench screen mode

Caffeine OS keeps a screen mode per machine,
`ENVARC:Sys/screenmode.prefs.UAE` and `.PI`, and copies the right one
into place at boot. `PrismSetup WBID=` writes the chosen PRISM: mode
into `ENVARC:Sys/screenmode.prefs` and into each of those copies that
exists, keeping each one first as `<name>.before-prism`. Remove copies
them back.

On UAE's card and on cards driven through their Picasso96 driver the
installer suggests a 16-bit Workbench: Caffeine OS's true-colour
backdrops and icons look rough when dithered to 256 colours.

## The card

On Caffeine OS in WinUAE the card is UAE's RTG card
([uaegfx.md](uaegfx.md)). On a PiStorm it is the VideoCore, which Prism
drives through its Picasso96 driver ([p96-drivers.md](p96-drivers.md));
that path has not been tested on a PiStorm yet. As everywhere, holding
the left mouse button while booting starts the Amiga without PrismRTG.

## The Installer

Caffeine OS uses the OS 3.9 Installer (44.10). It refuses a whole script
that uses an Installer parameter name, such as `back`, as a variable;
1.2 beta 1's script did, and 1.2 beta 2's does not.
