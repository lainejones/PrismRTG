# PrismD's ARexx port

PrismD opens a public ARexx port named `PRISM` when it starts (from 1.1 beta 8),
if `rexxsyslib.library` is there. RexxMast must be running for scripts to reach
it: OS 3.2 starts it at boot; on OS 3.1 run `SYS:System/RexxMast` once, or put it
in WBStartup. PrismD looks at the port once a frame, so a command is answered at
once.

Commands are words separated by spaces, in any case. With `OPTIONS RESULTS` the
answer is in `RESULT`; `RC` is 0 when the command worked, 5 when it was refused
for a reason the script can handle (a mode not offered, a test picture the board
can't show) and 10 for a bad command or argument. A trailing `2` (or `BOARD=2`)
asks about the second board where it makes sense.

| Command | Answer |
|---|---|
| `VERSION` | `1.1b8 13.10.26 8`: the release, its date, the interface version of PrismD's semaphore |
| `BOARDS` | how many boards PrismD drives (1 or 2) |
| `BOARD [2]` | the board's name, e.g. `ZZ9000 (Zorro III)` |
| `VRAM [2]` | its video memory and how much of it is free, in KB: `14272 13334` |
| `MODES` | how many PrismRTG modes there are (both boards) |
| `MODE <n>` | the n-th, from 1: `$7A000101 640 480 16 PRISM:640x480 16bit` (ModeID, width, height, bits, name); RC 10 past the last |
| `MODEID <w> <h> <bits> [2]` | the ModeID PrismD offers for that size and depth, e.g. `$7A000102`; RC 5 if it offers none |
| `TEST <w> <h> <bits> [hz] [2]` | shows PrismPrefs' test picture in that mode, on or off in PrismPrefs, until `TESTEND` or until another screen comes to the front. `OK`, or RC 5 when the board can't (`NOMODE`, `NOMEM`, `BUSY`) |
| `TESTEND` | ends the test picture |
| `POWERSAVE [minutes]` | the minutes without input before the monitors go dark (0 or `OFF` = never), after setting it if given (0-240). For this run of PrismD; PrismPrefs saves it |
| `SLEEP` | dark now, until a key or mouse move, or `WAKE` |
| `WAKE` | the picture back now |
| `DARK` | `1` while the monitors are dark, else `0` |
| `LOG [ON\|OFF]` | PrismD's debug log, after switching it if given |
| `HELP` | the command names |

## Example

```rexx
/* every mode of the first board, five seconds each */
OPTIONS RESULTS
ADDRESS PRISM
'MODES'
n = RESULT
DO i = 1 TO n
    'MODE' i
    PARSE VAR RESULT id w h bits name
    IF LEFT(id, 5) ~= '$7A00' THEN ITERATE
    SAY name
    'TEST' w h bits
    ADDRESS COMMAND 'Wait 5'
    'TESTEND'
END
```

`tools/arexxtest.rexx` in the source tree runs every command and checks the
answers; the release gate runs it on every test machine.
