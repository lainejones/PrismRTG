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

`driver_module.h` defines protocol version 3. The loader allocates a public
request with a reply port and starts the module with the request address as
its argument. The message, magic, ABI version and request-size prefix is
fixed. Before probing hardware, a module compares the protocol version,
request size, board ABI version and size, a board-layout signature, and
operation-table ABI version and size. The board carries a `PrismOps`
table, so the operation ABI is `PRISM_OPS_ABI` = 1 (`boardops.h`) and the
size is that of `struct PrismOps`. Changes to the operation table must
increment `PRISM_OPS_ABI`; extensions to the board interface must update
the board ABI.

The layout signature includes every board member's offset and size, mode
member offsets, scalar representation and pixel-format count. Semantic
changes to callback contracts or pixel-format meanings must increment the
board ABI even if no C structure changes size. Incompatible modules return
an explicit mismatch result before touching hardware; missing executables,
resource failures and probes finding no board have different diagnostics.

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
