# SAGA - what vampiregfx.card 1.56 does

**Source:** observed in vampiregfx.card 1.56 (26.12.25), from the Apollo
team's SAGADriver 3.4 package, by disassembly, for interoperability. No
code was copied. This page records hardware facts and behaviour only:
register addresses, bit meanings, value tables, and register-write
sequences described in words. It adds to [saga-hw.md](saga-hw.md), which
was written from the older MPL-2.0 SAGA.card source.

References like `@$52e8` are offsets into the card's CODE hunk ($8a90
bytes; the DATA hunk is $2a8 bytes, written `DATA+$xx`). They match the
amitools `hunktool` disassembly. The card uses 68020 instructions (MULS.L,
BFEXTU, scaled indexing), so a 68000-mode disassembler shows some of them
as `ILLEGAL`. Its blit routines also contain about 560 line-F words
($FExx/$FFxx), which fits the 68080 AMMX encoding. Those were not decoded.

Confidence labels:
- **certain**: read directly from the instructions.
- **inferred**: a reasonable reading that has not been checked on hardware.
- **unknown**: not determinable from this binary.

---

## 1. Library, FindCard, InitCard

### Library
* Standard library with a resident tag. Init `@$28` refuses to load
  unless exec is V37 or newer **and** `AttnFlags` has AFF_68020. *certain*
* Vector table (`DATA+$18`): Open `$b4`, Close `$15e`, Expunge `$fa`,
  reserved 0, **FindCard `$1c4`** (LVO -30), **InitCard `$6b8`** (LVO -36).
  *certain*

### FindCard `@$1c4` (A0 = BoardInfo)
In order:
1. It reads the word at **$DFF3FC**, shifts it right by 8 and keeps the
   high byte as the **board ID** (`DATA+$74`). **ID 0 makes FindCard
   return FALSE.** Nothing else reads `DATA+$74`. *certain*
2. **IDs 1, 2 and 6 set a "V2" flag** (`DATA+$7a` = 1). Every other
   non-zero ID takes the V4 path (flag = 0). *certain*
   The ID names, from the order of ApolloControl 2.18's board-name strings
   (*inferred*, but consistent with the V2 test above):

   | ID | Board | Family in the card |
   |---|---|---|
   | 1 | Vampire V2 V600 | V2 |
   | 2 | Vampire V2 V500 | V2 |
   | 3 | Apollo V4 Firebird (V4-500) | V4 |
   | 4 | Apollo V4 Icedrake (V4-1200) | V4 |
   | 5 | Apollo V4 Standalone (V4-SA) | V4 |
   | 6 | Vampire V2 V1200 | V2 |
   | 7 | Apollo V4 Manticore (V4-600) | V4 |
   | 8 | Apollo 6000 "Unicorn" (V4-UNI) | V4 |

   **expansion.library is never used.** Detection is only the $DFF3FC
   read, with no autoconfig check. *certain*
3. `OpenResource("vampire.resource")`. A version below 45 counts as
   absent. When it is present, the AMMX thresholds are set (width 8,
   height 1; see §6) and a small self-test runs `@$8188`. The self-test
   installs a privilege-violation trap and tries `ORI #$0800,SR`. If the
   CPU accepts that directly, the card later toggles SR bit 11 itself.
   Otherwise it calls **vampire.resource LVO -18 (-$12)** with D0 = 2 (on)
   or 0 (off). This is almost certainly V_EnableAMMX(version): SR bit 11
   is the 68080 AMMX enable. *inferred*
4. Opens dos.library V36, graphics.library V36 and misc.resource.
5. Opens input.device and calls **PeekQualifier** (LVO -42). **If either
   Shift key is held, FindCard returns FALSE**, so the card stays off when
   Shift is held at boot. *certain*
6. Sets `MemoryBase` = $FFFFFFFF and `MemorySize` = 0 (InitCard replaces
   both), then returns TRUE. *certain*

### InitCard `@$6b8` (A0 = BoardInfo, A1 = tool-type array)
* **The card parses the tool types itself**, using utility.library
  `Strnicmp` (LVO -168) through `BoardInfo->UtilBase` (+$40). So **UtilBase
  must be valid before InitCard runs.** The loop walks the array until it
  reaches a NULL entry (a NULL array pointer is also accepted). Each entry
  is compared against every key, so **the last matching entry wins**.
  *certain*

  | Key (case-insensitive prefix) | Effect | Default |
  |---|---|---|
  | `NOVSYNC=YES` | compared, result ignored (dead) | - |
  | `SCANLINES=YES` | CardData[3] bit 4. Only the V2 SetDAC uses it (FORMAT bit 10) | off |
  | `SWITCHDTR=YES` | CardData[3] bit 5, and `AllocMiscResource(MR_SERIALBITS, "vampiregfx")` (result ignored) | off |
  | `VIDEOMEMSIZE=` | frame-buffer size, parsed as below | **8 MB** |
  | `VIDEOWIDTH=` / `VIDEOHEIGHT=` | parsed to DATA+$7c/$80, **never used** | - |
  | `SHOWPCMODES=YES` | adds the byte-swapped "PC" formats to RGBFormats | off |
  | `LIBDEBUG=YES` | debug printing (flags, timings, AllocCardMem) | off |
  | `NOAMMX=YES` | sets both AMMX thresholds to $7FFFFFFF, so AMMX paths are never taken | AMMX on |

  These keys are **not** read by the card: `BoardType`, `$SettingsFile`
  (or `SettingFile`), `SoftSprite`, `BigSprite`, `NoBlitter`, `NoDPMS`,
  `NoSwitch`, `NoPLL`, `OverClock`, `DisplayChain`, `IgnoreMask`,
  `GrantDirectAccess`. They belong to rtg.library, which turns them into
  BIF_ flags before calling InitCard. *certain (by absence)*

* **VIDEOMEMSIZE value parsing** (`@$4514`): decimal digits, then one
  suffix character. *certain*
  - end of string, `b` or `B`: the value is in **bytes**. So `VideoMemSize=8`
    means 8 bytes.
  - `k` or `K`: value x 1024.
  - `m` or `M`: value x 1048576. Anything after the M is ignored, so
    `8MB` means 8 MB.
  - **any other character** (for example a space in `8 M`) gives **0**.
  - There is no upper limit and no lower limit.

* **Frame buffer** (`@$7e0`):
  - **If the size is non-zero**, the card does `AllocMem(size + 31,
    MEMF_FAST|MEMF_LOCAL|MEMF_REVERSE)` ($40104). If that fails, InitCard
    returns FALSE. Otherwise `MemoryBase` = the block rounded up to 32
    bytes, `MemorySize` = size, and `MemorySpaceBase/Size` are set to the
    same values. *certain*
  - **If the size is 0** (`VideoMemSize=0M`, as in the icon the package
    ships), **nothing is allocated**. `MemoryBase` = **0 (NULL)**,
    `MemorySize` = 8 MB (a nominal figure), and the card installs **its
    own AllocCardMem `$4cbc` / FreeCardMem `$4ec2`**. Each bitmap is then
    allocated on its own: `AllocMem(size + $a30, $40104)`, rounded up to 32
    bytes, with magic longs $01234567 / $89ABCDEF, the size and the raw
    pointer stored just below the returned address. FreeCardMem checks the
    magic longs before freeing. The force/system/bytes-per-row arguments
    are ignored. *certain*
  - The `RegisterBase` and `MemoryIOBase` fields are left at 0.
* **BoardInfo fields set:**

  | Field | Value |
  |---|---|
  | BoardName | `vampiregfx` |
  | BoardType | 1 |
  | PaletteChipType, GraphicsControllerType | 0 |
  | BitsPerCannon | 8 |
  | SoftSpriteFlags | 0 |
  | MaxHorValue[5], MaxVerValue[5] | $4000 |
  | MaxHorResolution[5], MaxVerResolution[5] | $8000 |
  | PixelClockCount[5], V2 | **439**, or **539** with BIF_OVERCLOCK |
  | PixelClockCount[5], V4 | **1** |
  | RGBFormats | **$0C46** (CLUT, R8G8B8, A8R8G8B8, R5G6B5, R5G5B5), plus $02B0 with SHOWPCMODES (R5G6B5PC, R5G5B5PC, A8B8G8R8, B8G8R8A8) |
  | EssentialFormats | $0446 (CLUT, R8G8B8, A8R8G8B8, R5G6B5) |
  | CardData[0..15] | cleared, then CardData[0] = 1 |

  *certain*
* **Flags** are OR'ed in: BIF_NOMEMORYMODEMIX (bit 1),
  BIF_NEEDSALIGNMENT (bit 2), BIF_GRANTDIRECTACCESS (bit 26) and
  BIF_HARDWARESPRITE (bit 0). On **V4 only**, BIF_INTERNALMODESONLY
  (bit 11) as well. The card never sets BIF_VBLANKINTERRUPT, BIF_BLITTER,
  BIF_VIDEOWINDOW or BIF_VGASCREENSPLIT. *certain*
* **V4: the card builds its own mode list** (`@$4868`, see §3) and adds
  it to `ResolutionsList` (+$9e) with AddTail. V2 uses P96's settings file
  (user-defined timings). *certain*

### Function slots filled (BoardInfo offset -> code offset)

| Slot | BI+ | V2 | V4 |
|---|---|---|---|
| SetSwitch | $11a | $56c8 | $5756 |
| SetColorArray | $11e | $509e | $513e |
| SetDAC | $122 | $51de | $523a |
| SetGC | $126 | $52e8 | $5532 |
| SetPanning | $12a | $5588 | $561c |
| CalculateBytesPerRow | $12e | $4f0a | same |
| CalculateMemory | $132 | $4f30 (returns its address argument unchanged) | same |
| GetCompatibleFormats | $136 | $4f3e (returns $FFFFFFFF) | same |
| SetDisplay | $13a | $527c | same |
| ResolvePixelClock | $13e | $4fbc | $5038 (returns 1) |
| GetPixelClock | $142 | $4f4c | $4fac (returns ModeInfo->PixelClock) |
| SetClock | $146 | $5046 (programs the PLL) | $5092 (does nothing) |
| SetMemoryMode / SetWriteMask / SetClearMask / SetReadPlane | $14a/$14e/$152/$156 | no-ops | same |
| WaitVerticalSync | $15a | **$5836, a no-op** | same |
| WaitBlitter | $162 | no-op | same |
| BlitPattern | $19e | $589a (AMMX or Default) | same |
| GetVSyncState | $1e6 | **$588a, returns its argument** | same |
| GetVBeamPos | $1ea | $5800 | same |
| SetDPMSLevel | $1ee | $52bc | same |
| ResetChip | $1f2 | no-op | same |
| SetSprite / SetSpritePosition / SetSpriteImage / SetSpriteColor | $206/$20a/$20e/$212 | $598a/$5aba/$5a62/$59f6 | same |
| AllocCardMem / FreeCardMem | $112/$116 | only when VideoMemSize is 0: $4cbc/$4ec2 | same |

The ten drawing slots (BlitPlanar2Chunky, BlitPlanar2Direct, BlitRect,
BlitRectNoMaskComplete, BlitTemplate, DrawLine, FillRect, InvertRect,
ScrollPlanar, UpdatePlanar) are first copied from their `...Default`
slots. **When vampire.resource is present** (V45 or newer), seven of them
are then replaced by the card's own CPU/AMMX routines:
BlitPlanar2Chunky $7a16, BlitPlanar2Direct $7a08, BlitRect $691a,
BlitRectNoMaskComplete $6e00, BlitTemplate $7094, FillRect $5f28 and
InvertRect $6f88. *certain*

The card does not install SetInterrupt, SetSplitPosition, the feature
functions or WriteYUVRect.

### CardData use (private, BI+$5ca)

| Entry | Meaning |
|---|---|
| [0] | 1 |
| [1] | the PLL index last programmed (V2) |
| [2] | **the last FORMAT value written** (restored by SetDisplay, DPMS and SetSwitch) |
| [3] | flags: bit 0 horizontal doubling (V2), bit 1 vertical doubling (V2), bit 2 sprite hidden, bit 3 display off, bit 4 Scanlines, bit 5 SwitchDTR |
| [4] | the visible width in pixels (used for the modulo) |
| [7], [8] | sprite X and Y as written to the hardware |
| [9] | V4: mode number << 8 |

---

## 2. Tool types and the PrismRTG P96MONITOR problem

The package ships `Devs/Monitors/vampiregfx.info` with:
`BoardType=vampiregfx`, `DisplayChain=Yes`, `GrantDirectAccess=Yes`,
`IgnoreMask=Yes`, `NoAMMX=No`, `NoBlitter=NO`, `NoPLL=No`, `NoSwitch=No`,
`OverClock=Yes`, `Scanlines=Yes`, `$SettingsFile=DEVS:Picasso96Settings`,
`ShowPCModes=No`, `SoftSprite=NO`, `SwitchDTR=No`, **`VideoMemSize=0M`**,
and others.

Of these, only `Scanlines=Yes` and `VideoMemSize=0M` change anything in
the card. `Scanlines` matters only on V2. **`VideoMemSize=0M` switches
the card into its "allocate each bitmap separately" mode, with a NULL
`MemoryBase`** (see §1). With an empty tool-type list the card keeps its
8 MB default and fills in a real `MemoryBase`.

How `src/drv_p96.c` (P96_Probe) behaves:
- It passes `do_ToolTypes` from `GetDiskObject(P96MONITOR)` unchanged to
  both FindCard and InitCard, or `emptyToolTypes` when no icon is given.
- `init_boardinfo()` sets UtilBase, so the card's Strnicmp works.
- `attach_board()` then rejects `!bi->MemoryBase`.

Taken together, **with `P96MONITOR` pointing at the shipped icon, InitCard
succeeds but attach_board fails with "P96 driver lacks required
framebuffer operations"**. That is the reported symptom: PrismD does not
come up, and it works without the icon. Even with the NULL check removed,
the adapter's allocator model would not fit the card's AllocCardMem: it
gives out offsets inside `MemoryBase..+MemorySize`, and `select_formats()`
probes `CalculateMemory(MemoryBase...)`. The card's AllocCardMem returns
scattered system RAM instead. **inferred**: this is the most likely
cause; it matches the code exactly but has not been confirmed with the
log on a real Vampire.

Related cases that fail the same way:
* `VideoMemSize=8` with no suffix gives 8 bytes. InitCard succeeds, then
  attach_board fails on `MemorySize < 65536`.
* `VideoMemSize=8 M` (with a space) gives 0, the NULL-base case.

---

## 3. Mode setting

### FORMAT ($DFF1F4, word) - the pixel-format codes
`DATA+$ae` maps each RGBFTYPE to a SAGA format code. *certain*

| RGBFTYPE | Code | | RGBFTYPE | Code |
|---|---|---|---|---|
| NONE (0) | 0 | | R5G6B5 (10) | 2 |
| CLUT (1) | 1 | | R5G5B5 (11) | 3 |
| R8G8B8 (2) | 4 | | B5G6R5PC (12) | $82 |
| B8G8R8 (3) | 4 | | B5G5R5PC (13) | $83 |
| R5G6B5PC (4) | **$82** | | YUV formats (14-20) | 6 |
| R5G5B5PC (5) | **$83** | | | |
| A8R8G8B8 (6) | 5 | | | |
| A8B8G8R8 (7) | 5 | | | |
| R8G8B8A8 (8) | 5 | | | |
| B8G8R8A8 (9) | **$85** | | | |

* **Bit 7 of the format code ($80) means byte-swapped (little-endian)
  pixels**: $82 is RGB565 little-endian, $83 is RGB555 little-endian and
  $85 is 32-bit with bytes reversed (B,G,R,A). *inferred* from the
  mapping. The swapped formats are advertised only with SHOWPCMODES.
* Several codes look wrong for their format: B8G8R8 maps to 4,
  A8B8G8R8 and R8G8B8A8 to 5, and B5G6R5PC/B5G5R5PC to $82/$83. Only
  R8G8B8, A8R8G8B8, R5G6B5 and R5G5B5 are trustworthy.
* Bytes per pixel (`DATA+$84`, used by CalculateBytesPerRow and the
  panning code): CLUT 1, 24-bit 3, 32-bit 4, 15- and 16-bit 2, YUV422 2,
  YUV411 1.
* **CalculateBytesPerRow = width x bytes per pixel, with no padding.**
  *certain*

### V4: a fixed mode table
**V4 SetGC** (`@$5532`) **writes no timing registers.** It stores the
ModeInfo, the visible width (CardData[4]) and **the mode number << 8**
(CardData[9]). It finds the mode number by searching ResolutionsList for
a matching width and height (`@$47fc`). An unknown size gives 0.
**V4 SetDAC** (`@$523a`) then writes `FORMAT = (mode << 8) | code` and
keeps a copy in CardData[2]. *certain*

The DisplayID is $5400_1000 | (mode number in BCD << 16). The lookup
turns the BCD back into binary: subtract 6 if the value is $20 or more,
then subtract 6 if it is $10 or more. So for example mode 10 has
DisplayID byte $10 and FORMAT high byte $0A. *certain*

The mode list the card builds (`@$4868`). Each mode gets one ModeInfo
per depth: 8, 16, 24 and 32 bits, Flags = 1. In the table, Clock is the
PixelClock the card reports, HS and HE are the horizontal sync start and
end (the same convention in the vertical columns), and Hz = clock /
(Htot x Vtot).

| No. | Size | Clock (MHz) | HS | HE | Htot | VS | VE | Vtot | Hz |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 320x200 | 28.37516 | 753 | 817 | 907 | 490 | 492 | 525 | 59.6 |
| 2 | 320x240 | 28.37516 | 753 | 817 | 907 | 490 | 492 | 525 | 59.6 |
| 3 | 320x256 | 28.37516 | 704 | 763 | 799 | 544 | 554 | 591 | 60.1 |
| 4 | 640x400 | 28.37516 | 753 | 817 | 907 | 490 | 492 | 525 | 59.6 |
| 5 | 640x480 | 28.37516 | 753 | 817 | 907 | 490 | 492 | 519 | 60.3 |
| 6 | 640x512 | 28.37516 | 753 | 817 | 907 | 582 | 586 | 625 | 50.1 |
| 7 | 960x540 | 42.56274 | 1024 | 1124 | 1131 | 552 | 556 | 625 | 60.2 |
| 8 | 480x270 | 42.56274 | 1024 | 1124 | 1131 | 552 | 556 | 625 | 60.2 |
| 9 | 304x224 | 28.37516 | 753 | 817 | 907 | 490 | 492 | 519 | 60.3 |
| 10 ($0A) | 1280x720 | 85.12548 | 1300 | 1332 | 2207 | 725 | 730 | 769 | 50.2 |
| 11 ($0B) | 640x360 | 85.12548 | 1300 | 1332 | 2207 | 725 | 730 | 769 | 50.2 |
| 12 ($0C) | 800x600 | 42.56274 | 900 | 1000 | 1087 | 620 | 640 | 649 | 60.3 |
| 13 ($0D) | 1024x768 | 85.12548 | 1100 | 1150 | 1599 | 770 | 774 | 883 | 60.3 |
| 14 ($0E) | 720x576 | 28.37516 | 753 | 817 | 907 | 582 | 586 | 625 | 50.1 |
| 15 ($0F) | 848x480 | 42.56274 | 936 | 984 | 1131 | 490 | 492 | 625 | 60.2 |
| **16 ($10)** | **640x200** | 28.37516 | 753 | 817 | 907 | 490 | 492 | 525 | 59.6 |
| **17 ($11)** | **1920x1080** | 85.12548 | 2060 | 2068 | 2751 | 1084 | 1089 | 1285 | 24.1 |
| **18 ($12)** | **1280x1024** | 85.12548 | 1352 | 1480 | 1631 | 1027 | 1032 | 1039 | 50.2 |
| **19 ($13)** | **1280x800** | 85.12548 | 1344 | 1480 | 1679 | 801 | 804 | 843 | 60.1 |
| **20 ($14)** | **1440x900** | 85.12548 | 1512 | 1656 | 1831 | 903 | 909 | 927 | 50.2 |
| **21 ($15)** | **720x405** | 28.37516 | 753 | 817 | 907 | 582 | 586 | 625 | 50.1 |

* Numbers 1-15 match saga-hw.md. **Numbers 16-21 are new.** *certain*
* The timing columns are the output timing the card reports to P96 (for
  small modes they are the doubled output, not the mode's own pixels).
  The clocks are multiples of the PAL colour clock: 28.375 MHz x 1, 1.5
  and 3. Whether the hardware really runs these timings is **unknown**,
  because nothing is written to the timing registers on V4. Read the Hz
  column as the card's claim. In particular, 24 Hz for 1920x1080 and
  60 Hz for 320x256 differ from the refresh rates in our `sizes[]` table.

### V2: timing registers and PLL (not used on V4)
* **V2 SetGC** (`@$52e8`) writes nine word registers from the ModeInfo.
  HSS means "Width + HorBlankSize + HorSyncStart", and VSS the vertical
  equivalent:

  | Reg | Value |
  |---|---|
  | $DFF300 | HPIXEL = Width |
  | $DFF302 | HSSTRT = HSS |
  | $DFF304 | HSSTOP = HSS + HorSyncSize |
  | $DFF306 | HTOTAL = HorTotal |
  | $DFF308 | VPIXEL = Height |
  | $DFF30A | VSSTRT = VSS |
  | $DFF30C | VSSTOP = VSS + VerSyncSize |
  | $DFF30E | VTOTAL = VerTotal |
  | $DFF310 | HVSYNC: bit 0 = GMF_HPOLARITY, bit 1 = GMF_VPOLARITY |

  If Width <= 500, the four horizontal values are doubled before writing
  and CardData[3] bit 0 is set. If Height <= 300, the vertical values are
  doubled and bit 1 is set. *certain*
* **V2 SetDAC** (`@$51de`): `FORMAT = ((CardData[3] & 3) << 8) | code`,
  plus **bit 10 when Scanlines is on**. So on V2, FORMAT bits 8-9 are
  horizontal/vertical pixel doubling and bit 10 is scanlines, not a mode
  number. *certain*
* **PLL clock table** at CODE $e0c: 539 entries of 26 bytes each. Each
  entry is a long (clock in Hz with a PAL machine clock), a long (the
  clock with an NTSC machine clock, always x 1.00921 = 28.63636/28.37516),
  and 18 bytes of PLL configuration. The clocks run from 20.268 MHz
  (index 0) to 74.721 MHz (index 538). Index 438 is 50.01 MHz, the last
  one without OverClock (count 439). GetPixelClock picks the NTSC column
  when `ExecBase->PowerSupplyFrequency` == 60 (+$213). It halves the
  clock for Width <= 500 and again for Height <= 300. ResolvePixelClock
  stores the nearest index in ModeInfo bytes $2a/$2b. *certain*
* **PLL programming** (`@$d80`, from V2 SetClock). It runs only when the
  index differs from CardData[1]. The sequence, all long writes to
  $DFF1F8:
  1. Write **$43430000** (the ASCII "CC" key, start).
  2. Shift out the 18 configuration bytes, last byte first and each byte
     LSB first. Each bit takes two writes: the data bit in bit 2 with
     bit 1 = 0, then the same value with bit 1 = 1 (a clock edge).
  3. Write **$43430001**, then 3, 9 and $0B.
  4. Write 1 and then 3, 128 times over.

  *certain* for the sequence. Which PLL chip it is: **unknown**.
* V2 palette: **$DFF400 + 4 x index**, a long holding $00RRGGBB, one
  register per entry. *certain*

### Panning and modulo (SetPanning)
Arguments: memory, pitch in pixels, xoffset, yoffset, RGBFTYPE. The card
also stores XOffset and YOffset in the BoardInfo (+$236/+$238) for the
sprite.

* Start address = memory + yoffset x pitch x bpp + xoffset x bpp.
  * **V4 rounds the start UP to a multiple of 16 bytes** (`@$568a`)
    for every depth except 24-bit.
  * **24-bit on V4**: the x offset is first set to (x + 11) AND $FFF4,
    and the address is not rounded.
  * **V2** does no rounding. For 24-bit it clears the low two bits of x.

  The result is written as a long to **$DFF1EC**. *certain*
* **Modulo** = (pitch - visible width) x bpp, in bytes, written as a word
  to **$DFF1E6**. The visible width comes from CardData[4]. *certain*

---

## 4. Beam position and vertical blank

* **GetVBeamPos** (`@$5800`) returns **(long at $DFF204 >> 8) AND $FFF**:
  a 12-bit line number, bits 8-19 of the long. *certain*
* **WaitVerticalSync is a no-op**, GetVSyncState returns its argument,
  and there is no SetInterrupt and no BIF_VBLANKINTERRUPT. **The card
  does no vertical-blank waiting of its own.** *certain*
* Unused code in 1.56 (nothing references it):
  * `@$5842`: a GetVSyncState-style test. It returns FALSE when the
    display is off or no mode is set. Otherwise it returns TRUE when the
    $DFF204 line (same 12 bits) is **>= ModeInfo->Height - 1**.
  * `@$581c`: calls graphics.library WaitTOF.
  * `@$d66` / `@$d74`: return an 11-bit line, bits 8-18, read from
    $DFF004 (VPOSR/VHPOSR) and from $DFF204 respectively.

  These suggest that the $DFF204 line counts from 0 at the first visible
  line of the RTG mode, so "in vertical blank" means line >= height-1.
  *inferred*. Whether it counts mode lines or output lines on V4's
  doubled low-resolution modes is **unknown**.

---

## 5. Hardware sprite

* Hide (SetSprite off): write **$3FFF** to both $DFF1D0 and $DFF1D2 and
  set CardData[3] bit 2. Show: rewrite the saved X/Y. *certain*
* **Position** (`@$5aba`): X = MouseX - XOffset, Y = MouseY - YOffset
  (the panning offsets; the hotspot offsets are not applied). On V2, X is
  doubled when horizontal doubling is on and Y when vertical doubling is
  on. Then **X + 16 and Y + 8** are written to $DFF1D0/$DFF1D2. Nothing is
  written while the sprite is hidden. *certain*
* **Image** (`@$5a62`): the card skips the first long of
  `BoardInfo->MouseImage` (the P96 header). It copies min(MouseHeight, 16)
  longs to **$DFF800 + 4 x row**, then writes zero to the remaining rows
  up to 16. Each long is **plane 0 word << 16 | plane 1 word**: the
  classic 16-pixel-wide, two-plane layout. **A 16x16 sprite only.**
  BIF_HIRESSPRITE and BIF_BIGSPRITE are not honoured. P96's BigSprite
  option affects only the software sprite, as the Tooltypes doc says.
  *certain*
* **Colours** (`@$59f6`): only for indexes 0-2. The colour goes to
  **$DFF3A2 + 2 x index** as the word
  ((R AND $F0) << 4) | (G AND $F0) | (B >> 4), a 12-bit 0RGB value.
  *certain*. The code is meant to skip this on V2, but the check reads a
  byte of the word-sized V2 flag at DATA+$7a, and that byte is always 0.
  So in practice the colours are written on V2 as well. *certain*
* SetGC calls SetSprite(off) when SoftSpriteFlags is $FFFF.

---

## 6. Everything else

* **Drawing acceleration** uses the CPU and AMMX only. **No SAGA
  register is touched in any blit routine** (none of the $DFFxxx accesses
  fall in CODE $5b2c-$8184). The AMMX paths run only when vampire.resource
  is present, width > 8 and height > 1 (the thresholds at DATA+$6c/$70),
  the format is not 24-bit, and (for BlitPattern) the mask is $FF.
  Otherwise the card calls the P96 `...Default` routine. Each AMMX use is
  bracketed by vampire.resource LVO -18 with D0 = 2. FillRect dispatches
  on the RGBFTYPE through a 21-entry jump table. *certain*
* **SetDisplay** (`@$527c`):
  * on: clear CardData[3] bit 3 and write the saved FORMAT (CardData[2])
    to $DFF1F4.
  * off: set bit 3 and **write 0 to $DFF1F4**.

  *certain*
* **DPMS** (`@$52bc`): level 0 restores the saved FORMAT. Standby,
  suspend and off all **write FORMAT 0**. There is no separate sync
  control. *certain*
* **SetSwitch**: both versions keep the state in MoniSwitch bit 0 (+$b6)
  and return the old state. *certain*
  * **V4** (`@$5756`): only switching *to RTG* writes anything (the saved
    FORMAT). **Switching to native writes nothing**, so the RTG picture
    stays until SetDisplay(FALSE) or DPMS writes FORMAT 0.
  * **V2** (`@$56c8`): on a change, it calls SetDPMSLevel. Switching to
    native uses level 3, which writes FORMAT 0; switching to RTG uses
    level 0, which restores FORMAT.
  * With SwitchDTR, it also drives **CIA-B PRA ($BFD000) bit 7, the serial
    DTR line**: 0 while RTG is shown, 1 while native is shown. This lets
    an external video switch follow the display.
* **No native/RTG selection register** is written. The only "RTG off"
  this card uses is FORMAT = 0. *certain* (by absence)
* **PiP ($DFF3D0-$DFF3E2), $DFF38C and the $DFF400 palette on V4 are
  not used.** No split screen and no feature (video window) support.
  *certain* (by absence)
* Register writes go through two tiny helpers: `@$44d4` writes a word and
  `@$4500` writes a long.
* All SAGA registers this card touches:
  $DFF1D0, $1D2, $1E6, $1EC, $1F4, $1F8 (V2 PLL), $204 (beam), $300-$310
  (V2), $388 (V4 palette), $3A2 (V4 sprite colours), $3FC (board ID),
  $400 (V2 palette) and $800 (sprite image). $DFF004 appears only in
  unused code.
