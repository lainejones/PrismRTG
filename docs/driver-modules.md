# Loadable board modules

PrismD loads one backend executable from `LIBS:Prism/<name>.driver`, then
tries `PROGDIR:Drivers/<name>.driver`. The supplied names are PICASSO2,
ZZ9000, P96 and UAEGFX. Automatic selection tries the native hardware
backends in their existing order. P96 and UAEGFX remain explicit choices.

Each module runs as an AmigaDOS process with a 16 KB stack, its own C
runtime and library bases. The module exports the same `PrismBoard`
callbacks previously linked into PrismD. This change does not introduce a
new drawing API or alter native card synchronization and mode setup.
The optional `textExpand` board callback preserves Cirrus startup
information without linking that backend into PrismD.

## Protocol and compatibility

`driver_module.h` defines protocol version 3. The loader allocates a public
request with a reply port and starts the module with the request address as
its argument. The message, magic, ABI version and request-size prefix is
fixed. Before probing hardware, a module compares the protocol version,
request size, board ABI version and size, a board-layout signature, and
operation-table ABI version and size. This standalone board interface has
no operation table, so those last fields are zero. A future surface API
must define its own nonzero operation version and size and update the
board ABI when extending the board interface.

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

A startup timeout cancels the request but keeps the loader, request and
borrowed output alive until the module acknowledges release. A late READY
reply is followed by STOP before cleanup. A permanently stuck driver
therefore requires a reboot; the loader does not free memory or close an
output handle that the child may still use.

## Packaging and development

`build.sh` builds the four modules into `out/Drivers`. `mkpkg.sh` includes
them in the release drawer; the installer copies them to `LIBS:Prism`.
Uninstall removes only the four supplied module files and removes the
drawer only when it is empty. PrismCheck and its tools remain separate.

`tests/driver_module.c` checks ABI/layout rejection. `module_fixture.c`
and `driver_module_guest.c` exercise missing modules, mismatched versions,
probe failures, the child stack and output ownership in a dedicated Amiga
boot. `tests/driver_guest.c` exercises PrismD startup and its P96 API with
real modules, then removes the PrismD semaphore on shutdown.
