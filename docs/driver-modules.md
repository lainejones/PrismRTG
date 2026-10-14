# Loadable board modules

Board drivers are separate Amiga executables (#8). PrismD loads one from
`PROGDIR:Drivers/<name>.driver` first, so a build run from its own drawer
uses its own modules, then from `LIBS:Prism/<name>.driver`. At boot PrismD
runs as `C:PrismD`, so the installed modules are used. The supplied names
are PICASSO2, ZZ9000, P96 and UAEGFX. Automatic selection tries PICASSO2,
then ZZ9000, then every other `.driver` in those two drawers in directory
order (`driver_scan`). P96 and UAEGFX remain explicit choices: they need
settings, or claim the emulator's card for good. Each module carries a
`$VER` string with its name and release, so `Version` can report it.

Every module is `src/driver_module.c` plus the driver's own sources,
built by `tools/mkdriver.sh NAME sources...`, which defines `DRIVER_NAME`.
The driver supplies `driver_probe()` and `driver_retain()`
(`prismboard.h`); `src/drv_template.c` is the model, and
[writing-a-driver.md](writing-a-driver.md) the guide.

Each module runs as an AmigaDOS process with a 16 KB stack, its own C
runtime and library bases. It exports its `PrismBoard` callbacks and its
`PrismOps` surface-operation table (#6); native card synchronization and
mode setup are unchanged from the linked drivers. The optional
`modeReady` board callback runs after the first mode set; the Cirrus
driver prints its blitter self-test results there. Modules set libnix's `__BUFSIZE` to
1 KB; the default 64 KB per standard stream would cost about 190 KB per
driver process.

## Protocol and compatibility

From 1.1 beta 8 the interface is frozen: `driver_module.h` defines protocol
version 4 and `prismboard.h` board ABI 3, and a driver built against them
keeps loading in every later PrismD with the same two numbers. The loader
allocates a public request with a reply port and starts the module with the
request address as its argument. The message, magic, protocol version and
request-size prefix is fixed in every protocol; after it come PrismD's
board ABI, its sizes of `struct PrismBoard`, `struct PrismMode`,
`struct PrismOps` and `struct PrismDriverConfig`, a layout signature, and
pointers to a cleared board and to the settings. Before probing hardware the
module accepts the request only if

* the protocol, board ABI, operation-table ABI and layout signature are its
  own, and
* every one of PrismD's structures is at least as big as the driver's.

So a later PrismD, whose structures have grown, takes an older driver; a
driver newer than PrismD is refused with a message ("doesn't fit this
PrismD"), because PrismD would not know what it fills in. The rules that
keep this true:

* The four structures only ever get new members **at their end**, and new
  members are optional: 0 or NULL means "this driver doesn't do it", which
  is what PrismD's cleared board holds for a driver that predates them.
* The layout signature covers the members frozen at board ABI 3 (every
  `PrismBoard` member up to and including `opsSize`, every `PrismMode`
  member up to `pitchPixels`): their offsets and sizes, never a structure's
  size. Appending leaves it unchanged; reordering a frozen member does not.
* The module reports the size of its operation table in `opsSize`, and an
  operation appended to `struct PrismOps` later is only called through
  `BOARD_CAN`, which checks it lies inside that table.
* A new pixel format is appended to `enum PrismFormat`; a driver is only
  ever asked for the formats it put in `formats`.
* A change no older driver can live with - a changed callback contract, a
  frozen member that must move - increments `PRISM_BOARD_ABI`, and every
  driver must be rebuilt.

The gate's `abicheck.py` checks the freeze both ways with
`tests/abiprobe.sh`, which builds a PrismD with a member, a hook and an
operation appended (`PRISM_ABI_PROBE`): the release's drivers must pass every
drawing check under it without the appended operation being called, and a
driver built the same way must be refused by the release's PrismD.
Incompatible modules return an explicit mismatch result before touching
hardware; missing executables, resource failures and probes finding no board
have different diagnostics. (Protocol 3, before 1.1 beta 8, needed exact
sizes and a signature over every member: any addition broke every driver.)
After successful probing the module publishes a control port and board
callbacks, replies to startup, then waits. PrismD may call those callbacks
while the module's runtime remains alive. On stop, PrismD sends a message
and waits for the module's final acknowledgement before releasing the
request and reply port. The native shutdown behavior is unchanged: loading
or rejecting a board does not introduce an unconditional `shutdown` call
before a mode has been initialized.

## Output and retained contexts

The child borrows PrismD's AmigaDOS output with `NP_CloseOutput=FALSE`, so
startup errors and backend diagnostics reach the same redirected log.
The child owns its separate NIL: input. Both sides flush their C output at
handoff points. The module flushes its final diagnostic and detaches its
DOS output before the final acknowledgement; it never closes the parent's
file handle or writes through it after PrismD exits.

P96 and UAE card contexts can install callbacks and interrupts which have
no general release protocol. Those drivers perform their existing
quiescing and retain their process after acknowledging stop. A failed
probe which has already claimed such a context follows the same lifetime
rule. Unclaimed contexts and native modules can return normally.

While waiting for startup, and while waiting for a late reply after a
timeout, the loader checks whether the driver process still exists
(`child_alive`). A process which ends without answering, for example
because its startup failed or `BOARD=` named some other program, is
reported as such instead of being waited for.

A startup timeout cancels the request but keeps the loader, request and
borrowed output alive until the module acknowledges release. A late READY
reply is followed by STOP before cleanup. A driver process which stays
alive but never answers therefore requires a reboot; the loader does not free memory or close an
output handle that the child may still use.

## Packaging and development

`build.sh` builds the four modules into `out/Drivers`. `tools/mkpkg.sh` includes
them in the release drawer; the installer copies them to `LIBS:Prism`.
Uninstall removes only the four supplied module files and removes the
drawer only when it is empty. PrismCheck and its tools remain separate.

`tests/driver_module.c` checks ABI/layout rejection. `module_fixture.c`
and `driver_module_guest.c` exercise missing modules, mismatched versions,
probe failures, the child stack and output ownership in a dedicated Amiga
boot. `tests/driver_guest.c` exercises PrismD startup and its P96 API with
real modules, then removes the PrismD semaphore on shutdown.
