# Cirrus GD542x on the Picasso II / II+ (and GBAPII++): hardware reference

Prism's hardware reference for the Picasso-II-type driver. It is written so the mode-setting
and blitter code can be written from it without more research. Every non-obvious fact has a
source tag such as `[NB:530]`. The tags are defined in §10, and the numbers are line numbers in
the pinned revision. Facts are graded like this:

* **(HW)**: real-hardware drivers (NetBSD, Linux) do this on a real Picasso II, so it is proven on silicon.
* **(EMU)**: this is how WinUAE 6.0.3 behaves (`gfxcard_type=PicassoII+`, PCem GD5428 core). It may
  differ from silicon.
* **(INF)**: an inference by the author of this document. Verify it on hardware.
* **UNKNOWN**: not found in any source. It is listed in §9.

No Cirrus GD542x data book was available online in readable form, so register bit names below
come from the driver and emulator sources. They are not quoted from Cirrus.

---

## 0. The single most important finding: the GBAPII++ is not a GD5426/28

* The GBAPII++ (Georg Braun / Matthias Heinrichs, "A500-GraKa") is built around a **CL-GD5434 in
  ISA mode**, with 2 MB of DRAM on a 64-bit bus (four 256K×16 SOJ40 chips) `[GB-README]`
  `[ERKAN]`. Its readme also mentions a "GD5435" once. The official driver is a **modified
  Picasso96 PiccoloSD64 driver**, not the PicassoII driver `[GB-README]`.
* Its CPLD (`GBAPIIPlusPlus.v`) answers autoconfig as **Village Tronic (2167) product 16
  (2 MB memory) + product 17 (64 KB I/O)**, serial `0x00030000`. A comment in the source lists
  other identities that alternative `.jed` builds use: PII+ 11/12 and PiccoloSD64. The repo ships
  `GBAPIIPlusPlus-PiccoloSD64.jed`, `-piccolo-moniswitch.jed`, `-CGFX.jed` and others `[GB-V:378-430]`.
  **Read the IDs from ShowConfig on the real A2000 before assuming anything.**
* The board itself is Picasso-II-like. Registers sit at board offset = ISA port, the +0x1000
  odd-port alias works, the monitor switch is at offset 0x8000/0x9000 (§1.6), and the 2 MB of
  VRAM is linear. **The chip is different**: it is a 5434, and the differences are listed in §8.5.
* WinUAE has **no** GBAPII++ / GD5434-on-Zorro-II emulation. The development target
  (`PicassoII+` = GD5428) and the real board (GD5434) therefore differ. Prism must read
  **CR27** (chip ID, §2.2) and branch on it.

---

## 1. Board level: Zorro II, address map, monitor switch

### 1.1 Autoconfig identities

| Board | Mfr | Product | Size | Notes / source |
|---|---|---|---|---|
| Picasso II / II+ VRAM | 2167 (0x0877) | 11 (0x0B) | 2 MB (Z2 mem space) | `[ZIDS]` `[NB:257-]` `[WU:224]` |
| Picasso II / II+ registers | 2167 | 12 (0x0C) | 64 KB | `[ZIDS]` `[NB]`; WinUAE maps 64 KB `[WU:3500-3510]` |
| Picasso II / II+ in *segmented mode* | 2167 | 13 (0x0D) | - | jumper JP401; Picasso96 refuses it `[ZIDS]` `[P96FAQ]`. Prism: refuse it too |
| GBAPII++ (current CPLD source) | 2167 | 16 mem (2 MB) / 17 I/O (64 KB) | 2 MB + 64 KB | `[GB-V:378-430]`; the mem board configures first (chained), then I/O |
| GBAPII++ alt. CPLD builds | 2167 / 2195 | 11+12 or Piccolo SD64 10+11 | | comment in `[GB-V:399,410]`, `.jed` file names in the repo |

* **II vs II+**: NetBSD treats serial number `0x00100000` as "II+" `[NB:475]`. WinUAE uses
  serial `0x00020000` for the II (GD5426, no IRQ) and `0x00100000` for the II+ (GD5428, IRQ level 2)
  `[WU:222-232]`. To tell the chips apart, use CR27 rather than the serial.
* The two boards may configure in either order. NetBSD stores each half when it matches and only
  attaches once both are present `[NB:346-380]`.
* **VRAM size is not the autoconfig size.** WinUAE always configures 2 MB of address space, even
  for 1 MB of VRAM `[WU:222-232]`, and NetBSD probes for 1 MB vs 2 MB (§2.6 step 13) `[NB:645-665]`.

### 1.2 Register board map (offset from the register-board base = ISA I/O port)

| Board offset | Meaning | Source |
|---|---|---|
| `0x03B0-0x03DF` | standard VGA + Cirrus ports at **offset = port number** (byte access) | `[NBH]` `vgaw(ba, reg)`; `[WU:6414-6425]` |
| `0x1000 + (port & 0xFFE)` | **odd-port alias**: an access at even offset `0x1000+N` reaches port `N|1` | `[WU:6415]` `(addr & 0x1000) → addr |= 1`; GBAPII++: `SA0 = A12 | UDS` `[GB-V:217-230]` |
| `0x13C8` (= `0x3C9 + 0xFFF`) | **DAC data (0x3C9): always use this address on Picasso II** | `[NBH]` `VDAC_DATA`, `[LX:2531-2550]` "DAC data register IS [translated], at least for Picasso II" |
| `0x13C6` (= `0x3C7 + 0xFFF`) | DAC read-index (0x3C7) | `[NBH]` `VDAC_ADDRESS_R`; `[LX:2412-2442]` |
| `0x0102`, `0x03C3`, `0x46E8` | ISA sleep/POS "wakeup" ports; harmless no-ops in WinUAE (PCem decodes 0x3C3 only for MCA, `[WUCL:513-518]`) | `[NB:548-552]` `[LX:1508-1515]` |
| `0x8000-0xFFFF` (write) | board control: monitor switch, II+ interrupt enable (§1.6, §7) | `[NB:1791-1856]` `[LX:2509-2529]` `[WU:6394-6413]` |

Access rules:
* **Byte accesses** are the safe default. NetBSD and Linux use byte writes at the odd data ports
  (`0x3C5`, `0x3CF`, `0x3D5`) directly on a real Picasso II (HW).
* **Word writes** to an index/data pair work: the high byte goes to the even port, the low byte to
  port+1 (EMU `[WU:6419-6421]`). NetBSD uses a 16-bit write to 0x3C4 on a real Picasso II for
  SR11 (HW `[NB:851-869]`), but warns that the same trick did not work for SR10 ("don't ask me why").
* **Never use 32-bit (`move.l`) accesses in register space.** WinUAE's Picasso II register handler
  only implements sizes 0 and 1, so long writes are dropped (EMU `[WU:6414-6425]`).
* Why the `+0xFFF` alias exists (INF, from the GBAPII++ CPLD, which re-implements the PII
  scheme): the CPLD drives ISA `SA0 = A12 | UDS`. An even (UDS, D15-8) access with A12 set
  therefore becomes an **odd-port cycle carried on the even byte lane**. That is what an 8-bit-only
  odd port needs, because ISA puts 8-bit odd transfers on SD7-0. The readme calls this "an ugly
  quirk for 8bit-ISA accesses" `[GB-README]`. Rule for Prism: write **0x3C9 and 0x3C7 through
  the alias**. All other ports go to their direct address (proven, HW). The alias is also safe for
  0x3C1 (INF).

### 1.3 VRAM aperture

* Normal (non-segmented) mode gives a **linear 2 MB window** at the memory-board base. Picasso96
  needs this mode `[P96FAQ]`. Linux maps the whole RAM board and uses it linearly
  `[LX:2196-2290]`.
* **The byte order is the 68k's.** 68k byte address *n* is VRAM byte *n*, with no swapping
  hardware. WinUAE emulates this: word and long writes are byte-reversed into the little-endian
  PCem VRAM, which keeps the memory byte order (EMU `[WU:5562-5590]`, offset `0xA00000` has bit 23
  set). The chip reads 16-bit pixels **little-endian**, so 16-bit pixels must be stored
  byte-swapped (§8.1).
* **SR7[7:4] must be non-zero (use 0x2_).** On ISA-mode GD542x this nibble is the linear-aperture
  address A23-A20 (1 MB units). The window is 1 MB when GRB bit 5 = 1 and 2 MB (A23-A21 only)
  when GRB bit 5 = 0 `[86:1885-1895]` `[WUCL:1076-1160]`. NetBSD and Linux both use **0x2_ for the
  Picasso** and 0x8_ for the Piccolo/Spectrum/SD64 (HW `[NB:1634-1637]` `[LX:160-172]`). In WinUAE,
  if SR7[7:4] = 0 the linear mapping is disabled and **every CPU VRAM access is silently dropped**
  (EMU `[WUCL:1092]`, `[WUG]` `mem_mapping_recalc` takes the linear mapping's handlers). WinUAE does
  not check the window bounds. It calls the linear handler directly and the address is masked by
  the chip's decode mask, so in WinUAE 0x1_…0xF_ all behave the same.
* **SRF bit 7 = 1 is needed to address the second MB.** WinUAE sets the decode mask to 2 MB-1
  only when SRF bit 7 is set, and to 1 MB-1 otherwise, so with bit 7 clear the upper MB aliases
  the lower one (EMU `[WUCL:1148-1152]`). The QEMU core used in older WinUAE versions even
  rejects 2 MB with SRF[7] = 0, because CyberGraphX probes this way `[QM-WU:1217-1234]`.
* **Banking (GR9/GRA/GRB): leave it off.** In packed linear mode GR9 is added to the aperture
  base (`base += GR9 << 12`, or `<< 14` with GRB bit 5), so it shifts which VRAM byte a CPU
  address hits (EMU `[WUCL:1123-1154]`). NetBSD uses exactly this for its 2 MB probe (GR9 = 0x40
  with 16K granularity = +1 MB) `[NB:645-665]`. Keep **GR9 = GRA = 0** and **GRB bit 0 = 0**
  (single bank) during normal operation. The 64 KB A0000 banked window does not exist on the
  Amiga, and the VGA memory map in GR6 bits 3-2 is irrelevant in linear mode.

### 1.4 What the CPU write path does to your data (VGA pipeline)

With GR5 write mode 0, GR8 = 0xFF, SR2 = 0xFF and GR3 = 0, CPU writes reach VRAM unchanged,
**except that VGA Set/Reset still applies**. If GR1 (Enable Set/Reset) is non-zero, every byte
whose `addr & 3` plane bit is set in GR1 is replaced by 0x00/0xFF taken from GR0. The GD5426/28 do
not disable this in packed mode; the GD5429+ do, via SR7 bit 0 (EMU `[WUCL:560]`
`set_reset_disabled` is only set for `type >= GD5429`, `[WU-SVGA:1069,1118]`). The **BLT uses
GR0/GR1 as its background/foreground colour** (§5). **After any BLT that loaded colours, wait for
it to finish, then write GR0 = GR1 = 0 before the CPU touches VRAM.** This is INF for silicon, but
VGA semantics plus the 5429 change make it near certain.

### 1.5 Bus timing notes

* Zorro II: 16 data bits, synchronous to the ~7.09 MHz (PAL) bus clock, at least 4 clocks per
  transfer. That gives at most about 3.5 MB/s, and a card that inserts wait states (every
  ISA-bridged card does) is lower. This is general Amiga knowledge and was not taken from a
  fetched source. A 68030 `move.l` becomes two Zorro II cycles, but still use longs: they halve the
  instruction overhead.
* GBAPII++: every Zorro access runs a ~15-state machine clocked at 50 MHz (MCLK) and waits on
  the chip's `WAIT`/IOCHRDY, with DTACK driven by the CPLD `[GB-V:237-376]`. I/O cycles are as slow
  as memory cycles, so keep register traffic out of inner loops.
* The display refresh comes from local VRAM and is independent of Zorro II. The blitter is
  therefore the main performance tool: a VRAM-to-VRAM BLT never crosses the bus.

### 1.6 Monitor switch (video pass-through relay)

| Action | Write (any byte value) to reg-board offset | Evidence |
|---|---|---|
| **Show VGA (Picasso) output** | **`0xA000`** (also aliases 0x8000 on boards that ignore A13) | EMU: WinUAE 6.0.3 switches only on `(addr & 0x2001) == 0x2000` with A15 set, so idx 0xA → on and 0x8 is ignored `[WU:6397-6403]`. HW: NetBSD writes 0xFF to 0x8000 `[NB:1835-1856]`. GBAPII++: A13 is not connected, so `MONISW = A12` on any I/O write with A15 = 1 `[GB-V:301]` |
| **Show native Amiga video** | **`0xB000`** (aliases 0x9000) | EMU: idx 0xB → off `[WU:6399]` (also disables the II+ interrupt, §7). HW: NetBSD writes 0x01 to **0x9000** "Special Picasso Address" `[NBH]` `[NB:1812-1832]`. Linux: "writing an arbitrary value to [0x9000] causes the monitor switcher to flip to Amiga display" `[LX:2519-2529]` |

* **Recommendation:** use **0xA000 for VGA and 0xB000 for Amiga**. These are the only addresses
  that work in WinUAE 6.0.3. On the GBAPII++ they alias 0x8000/0x9000, since A13 is not decoded
  there. Whether an *original* Picasso II decodes A13 is **UNKNOWN** (§9). If 0xA000 fails on a
  real II, fall back to 0x8000.
* The older WinUAE QEMU-core path accepted 0x8000/0xA000 for VGA and 0x9000/0xB000 for Amiga on
  any even address `[WU:3431-3450]`. It is not used for these boards since PCem became the
  default.
* WinUAE applies the switch after a 25-frame delay (`MONITOR_SWITCH_DELAY`) `[WU:73,1688]`, and
  there is no auto-switch for boards that have a switcher.
* GBAPII++: the switch powers up showing the Amiga (`sigMONITORSW <= 1` on reset). Some
  revisions also carry an automatic "Ratte" switch for a scandoubler input `[GB-V:151]` `[ERKAN]`.
* NetBSD calls the switch with a 200 ms delay `[NB:1791-1856]`.

---

## 2. Mode setting

### 2.1 Access primitives (port numbers; add the register-board base)

```
SEQ:  write idx→0x3C4, val→0x3C5          read: idx→0x3C4, read 0x3C5
GRC:  write idx→0x3CE, val→0x3CF
CRTC: write idx→0x3D4, val→0x3D5          (colour I/O: Misc bit0 = 1)
ATTR: read 0x3DA (resets flip-flop); write idx→0x3C0; write val→0x3C0
      index bit 5 (PAS) must be 1 at the end or the screen stays black
DAC:  0x3C6 pixel mask (and the hidden HDR), 0x3C8 write index,
      0x3C7 read index (via 0x13C6), 0x3C9 data (via 0x13C8)
Misc: write 0x3C2, read 0x3CC.  Input status 1: read 0x3DA
```
Sources: `[NBH]` macros `WSeq/WGfx/WCrt/WAttr`, `[LX:2412-2465]`.

### 2.2 Register reference (only the bits Prism needs)

**Misc output (0x3C2 W / 0x3CC R):** bit0 = colour I/O (1), bit1 = RAM enable (1), bits 3-2 =
clock select (11 = VCLK3, programmed through SR0E/SR1E), bit5 = page select (1), bit6 = 1 → HSYNC
negative, bit7 = 1 → VSYNC negative. NetBSD's base value is 0xEF (both syncs negative) and clears
bits 6/7 for positive syncs `[NB:1546-1555]`. Linux uses `0x0F | (+h ? 0x40 : 0) | (+v ? 0x80 : 0)`,
which inverts the sense of bits 6/7 relative to the VGA convention `[LX:922-927]`. **Use VGA
semantics** (1 = negative), which matches NetBSD and is HW-proven.

**Sequencer:**

| Reg | Use | Value(s) used on Picasso II | Source |
|---|---|---|---|
| SR0 | reset | 0x03 (run). 0x01 = synchronous reset if you want it around clock changes | `[NB:571]` |
| SR1 | clocking | **0x21 = screen off** (bit5, full bandwidth to CPU), **0x01 = on**, 8-dot | `[NB:559,736-742]` |
| SR2 | map mask | 0xFF | `[NB:572]` |
| SR3 | char map | 0x00 | |
| SR4 | memory mode | **0x0E** (chain-4, ext. memory, no odd/even). Linux uses 0x0A | `[NB:574]` `[LX:1551]` |
| **SR6** | unlock extensions | write **0x12**. Reads back 0x12 when unlocked, 0x0F when locked (EMU `[WUCL:949]`) | `[NB:563]` `[LX:1525]` |
| **SR7** | ext. sequencer mode | bit0 = packed-pixel (SVGA) mode; bits 2-1 = pixel/clock mode: `00` 8 bpp, `01` 16 bpp with VCLK = 2×pixclk, `10` 24 bpp with **VCLK = 3×pixclk**, `11` 16 bpp with VCLK = pixclk; bits 7-4 = linear aperture A23-A20 (§1.3). Picasso: **8 bpp 0x21, 15/16 bpp 0x27, 24 bpp 0x25** | `[NB:1634-1637]` `[LX:1021-1030,1094-1100,1153-1160]` `[QM:66-75]` `[WUCL:1252-1262]` |
| SR8 | EEPROM ctl | NetBSD writes 0; Linux says it is not needed | `[NB:576]` |
| SR9, SRA, SR14, SR15 | scratch | **do not touch** (BIOS/driver scratch) | `[LXH]` |
| SR0B-SR0E | VCLK0-3 numerator N (bits 6-0) | program VCLK3 = SR0E | `[NB:1567]` `[LX:889-902]` |
| **SRF** | DRAM control | **0xB0 for 2 MB, 0x30 for 1 MB** (bit7 = 2nd bank / "bank switch", bits 4-3 = DRAM width: 01 = 512K, 10 = 1M, 11 = 64-bit). Text/1 bpp modes use 0xD0 | `[NB:567-569,1527-1537,657]` `[LX:1864-1900]` `[QM:77-81]` |
| SR10/SR11 | HW cursor X/Y (§6) | | |
| SR12 | cursor attributes (§6) | 0x04 at init | `[NB:582]` |
| SR13 | cursor pattern select (§6) | 0x3C | `[NB:605]` |
| SR16 | performance tuning (5424/26/28 only) | 0x0A (NetBSD), 0x0F (Linux) | `[NB:580]` `[LX:1541]` `[LXH]` |
| SR17 | config readback (5428 only) | NetBSD leaves it alone on the PII. Bits 5-3 read the bus type | `[WUCL:951-978]` |
| SR18 | signature generator | 0x02 | `[NB:581]` |
| SR1B-SR1E | VCLK0-3 denominator: bits 5-1 = D, bit0 = P (post-divide by 2) | program SR1E | §2.3 |
| SR1F | MCLK select / ROM write enable | **0x22** on Picasso: MCLK = 14.31818 × (SR1F & 0x3F) / 8 = **60.85 MHz**. Bit6 = use MCLK as VCLK (keep 0) | `[NB:587]` `[LX:172,465-470,627-645]` |

**CRTC:** standard VGA CR00-CR18 (§2.4), plus:

| Reg | Use | Source |
|---|---|---|
| CR11 | bit7 = protect CR0-CR7 (**clear it before writing CR0-7**). bit5 = 1 disables the vertical interrupt. bit4 = 0 clears the vertical interrupt. bits 3-0 = VSyncEnd | `[LX:749]` `[WUCL:162-167]` |
| CR13 | offset (pitch) low 8 bits, in **8-byte units** | §4 |
| CR17 | mode control **0xE3** (NetBSD); Linux 0xC3. bit2 (vertical ×2) only for VT > 1023 | `[NB:1507-1511]` `[LX:905-911]` |
| CR19 | interlace end (= HT/2 for interlaced, else 0) | `[NB:1622]` `[LX:916-918]` |
| **CR1A** | bit0 = interlace; bits 5-4 = HBlankEnd bits 7-6; bits 7-6 = VBlankEnd bits 9-8 | `[NB:1623-1628]` `[LX:822-835]` |
| **CR1B** | bit0 = start addr bit16; bit1 = extended address wrap (needed for > 256 KB display); bit2 = start bit17; bit3 = start bit18; **bit4 = offset bit8**; bit5 = blank-end extension ("special blanking" from display enable). Base **0x22** | `[NB:1694-1697]` `[LX:1205-1212,1347-1358]` `[86:1960-1990]` `[WUCL:1171,1186,1257]` |
| CR1D | 5434+ only: bit7 = start addr bit19. **Absent on 5426/28** (Linux: `scrn_start_bit19 = false` for Picasso) | `[LX:168]` `[86:2047-2054]` |
| CR22/24/26 | latch / ATC flip-flop / ATC index readback (CR24 bit7 = ATC in "data" state) | `[LXH]` `[LX:2446-2465]` |
| **CR27** | **chip ID**: 0x90 = GD5426, 0x98 = GD5428, 0x9C = 5429, 0xA8 (or 0xA4) = GD5434. NetBSD shifts right by 2 (0x24/0x26/0x27) | `[NB:477-488]` `[86:73-88]` `[WUCL:1016-1035]` |

**Graphics controller (GRC):**

| Reg | Value | Note |
|---|---|---|
| GR0 / GR1 | 0 / 0 | Set/Reset, and **BLT background/foreground colour low byte** (§5). Must be 0 when the CPU writes VRAM (§1.4) |
| GR2-GR4 | 0 | |
| **GR5** | **0x40** for all chunky modes (256-colour shift) | 0x00 only for text/planar. Also affects a cursor quirk (§6) `[NB:1630]` `[LX:1077,1131]` |
| GR6 | 0x01 | graphics mode, memory map irrelevant in linear mode |
| GR7 / GR8 | 0x0F / 0xFF | |
| GR9 / GRA | 0 / 0 | bank offsets (§1.3) |
| **GRB** | **0x28 on 5426/28** (HW: NetBSD + Linux on a real PII; bit5 = 16K granularity, bit3 = 8-byte latches). **0x20 on 5434**: "5434 can't have bit 3 set for bitblt" | `[NB:615]` `[LX:1611-1621]` |
| GRC / GRD | 0xFF / 0x00 | colour key compare, which is for the video overlay, not the BLT `[LX:1623-1624]` |
| GRE | 0x00 | 5428 only: bit1 = HSYNC off, bit2 = VSYNC off (DPMS) `[LX:1434]` `[LXH]` |
| GR10-GR15 | | BLT colour bytes 1-3 (§5) |
| GR20-GR3F | | BLT engine (§5) |

**Attribute controller:** AR0-ARF = 0x00-0x0F (identity), **AR10 = 0x01** (graphics), AR11 = 0
(overscan), AR12 = 0x0F, AR13 = 0 (pel pan), AR14 = 0. Finish with a write of an index with
bit5 set, e.g. **0x20**, or the display stays blanked. Linux writes 0x33 = 0x13|0x20 `[LX:2446-2465]`,
and NetBSD ends with `WAttr(0x20|0x12, …)` `[NB:1700-1703]`.

**Hidden DAC register (HDR).** It is reached through 0x3C6 after **four consecutive reads of
0x3C6**; the next write to 0x3C6 goes to the HDR. Any access to 0x3C7/0x3C8/0x3C9 resets the
counter (EMU `[WUCL:576-590,987-997]`).

| HDR | Mode | Source |
|---|---|---|
| 0x00 | 8 bpp CLUT | `[NB:1656-1664]` |
| **0xD0** (or 0xC0) | 15 bpp 1:5:5:5 | `[NB:1665-1668]`; EMU decodes bit7+bit6 with low nibble 0 as 15 bpp, and bit7 alone also as 15 bpp `[WUCL:1193-1226]` |
| **0xC1** | 16 bpp 5:6:5 | `[NB:1669-1672]` |
| **0xC5** | 24 bpp (B,G,R bytes) | `[NB:1673-1676]` `[LX:1192]` |
| 0x4A | 8 bpp "clock double" for > 85 MHz. 5434-class only, **not 5426/28** | `[NB:1660-1661]` `[LX:497-512]` |

HDR write procedure, as NetBSD (`[NB:1638-1689]`) and Linux (`[LX:2469-2507]`, "Klaus' hint")
do it on the Picasso II:
1. Write 0x00 to 3C6. 2. Read 3C8 (resets the counter). 3. Read 3C6 four times. 4. Write the HDR
value to 3C6. 5. Read 3C8. 6. Write 0xFF to 3C6 (restore the pixel mask).
Linux waits 200 µs between steps and NetBSD waits 200 ms. Whether any delay is needed is
UNKNOWN; start with about 1 ms. **Linux writes 0xA0 for 16 bpp on Zorro (marked FIXME)
`[LX:1136]`. EMU treats 0xA0 as 15 bpp, so use 0xC1.**

### 2.3 Pixel clock (VCLK) synthesis

```
VCO  = 14.31818 MHz × N / D          N = SR0E[6:0]  (7 bit)
VCLK = VCO / (1 + P)                 D = SR1E[5:1]  (5 bit, 1..31),  P = SR1E[0]
SR1E = (D << 1) | P                  select VCLK3 with Misc[3:2] = 11
```
`[NB:1270-1345]` (`cl_CompFQ`), `[XFCLK]`, `[WUCL:1237-1256]`, `[86:2003-2010]`.

* **Stability:** keep **28.636 ≤ VCO ≤ 111 MHz** (XFree86 "VCO stability criterion", `MIN_VCO =
  2×OSC`, `MAX_VCO = 111000 kHz`) `[XFCLK]`. Linux additionally limits N to 32..127 and D to 7..63
  `[LX:2741-2790]`. Search all N/D/P values and keep the closest match inside these limits.
* **Chip limit for the Picasso II:** NetBSD uses 86 MHz `[NB:478-490]`. The limits are **8 bpp ≤
  86 MHz, 15/16 bpp ≤ 57.3 MHz (86 − 86/3), 24 bpp pixel clock ≤ 28.7 MHz (VCLK = 3×)**
  `[NB:1348-1408]`. Linux guesses 90 MHz for everything `[LX:160-165]`.
* **Depth multiplier:** 24 bpp on the 5426/28 needs **VCLK = 3 × pixel clock**, with the CRTC
  horizontal values left in pixel/8 units (EMU divides VCLK by 3, `[WUCL:1246-1255]`; NetBSD uses
  `clkmul = 3` and leaves the H timings unchanged `[NB:1490-1496]`). 16 bpp with SR7 = x7 uses VCLK =
  pixel clock. The 5434 does **not** divide by 3 in 24 bpp `[86:2012-2020]`.
* Known-good (N, SR1E) pairs from XFree86's tested table `[XFCLK]`, which XFree prefers when within
  0.1 %:

| MHz | N | SR1E | | MHz | N | SR1E | | MHz | N | SR1E |
|---|---|---|---|---|---|---|---|---|---|---|
| 12.599 | 0x2C | 0x33 | | 36.082 | 0x7E | 0x33 | | 64.983 | 0x76 | 0x34 |
| 25.227 | 0x4A | 0x2B | | 39.992 | 0x51 | 0x3A | | 72.163 | 0x7E | 0x32 |
| 28.325 | 0x5B | 0x2F | | 41.164 | 0x45 | 0x30 | | 75.000 | 0x6E | 0x2A |
| 31.500 | 0x42 | 0x1F | | 45.076 | 0x55 | 0x36 | | 80.013 | 0x5F | 0x22 |
| | | | | 49.867 | 0x65 | 0x3A | | 85.226 | 0x7D | 0x2A |

  Computed (VCO window, D ≥ 7): 25.175 → N = 0x6D, SR1E = 0x3F (25.172); 40.000 → 0x5F/0x23 (40.007);
  50.000 → 0x1C/0x10 (50.114); 65.000 → 0x3B/0x1A (64.983); 75.525 → 0x3A/0x16 (75.496).

### 2.4 CRTC timing math (from VESA-style timings)

Input: active width W, front porch HFP, sync HS, back porch HBP (pixels); active height V,
VFP, VS, VBP (lines). Horizontal values are in **character clocks = pixels / 8**, for every
depth on the 5426/28 (the 24 bpp ×3 is absorbed by the VCLK, §2.3). The formulas follow standard
VGA, cross-checked against `[LX:690-873]` and `[NB:1462-1630]`:

```
HT  = (W+HFP+HS+HBP)/8   HD = W/8   HSS = (W+HFP)/8   HSE = (W+HFP+HS)/8
HBS = HD                 HBE = HT-1                                  (8-bit with CR1A)
VT  = V+VFP+VS+VBP       VSS = V+VFP   VSE = V+VFP+VS   VBS = V   VBE = VT-1

CR00 = HT-5              CR01 = HD-1           CR02 = HBS
CR03 = 0x80 | (HBE & 0x1F)                     CR04 = HSS
CR05 = ((HBE & 0x20) << 2) | (HSE & 0x1F)
CR06 = (VT-2) & 0xFF
CR07 = bit0 (VT-2).8 | bit1 (V-1).8 | bit2 VSS.8 | bit3 VBS.8 | bit4 1 (LC.8)
     | bit5 (VT-2).9 | bit6 (V-1).9 | bit7 VSS.9
CR08 = 0                 CR09 = 0x40 (LC.9) | (VBS.9 << 5) | (doublescan ? 0x80 : 0)
CR10 = VSS & 0xFF        CR11 = 0x20 | (VSE & 0x0F)      (vint disabled, unprotected)
CR12 = (V-1) & 0xFF      CR13 = pitch_bytes/8 & 0xFF     CR14 = 0x00 (no dword mode)
CR15 = VBS & 0xFF        CR16 = VBE & 0xFF               CR17 = 0xE3   CR18 = 0xFF
CR19 = 0 (HT/2 if interlaced)
CR1A = ((HBE >> 6) & 3) << 4 | ((VBE >> 8) & 3) << 6 | interlace
CR1B = 0x22 | ((pitch_bytes/8 >> 8) & 1) << 4 | start-address bits (§4)
```
* For VT > 1023 lines (non-interlaced), halve all vertical values and set CR17 bit2 (0xE7)
  `[NB:1507-1523]` `[LX:724-729]`.
* With CR1B bit5 set, blanking comes from display enable, so HBS/HBE/VBS/VBE are not critical
  `[86:1960-1990]`. The tables below still use consistent values.
* Linux and NetBSD differ by ±1 on several fields (Linux `CR04 = HSS+1`, `CR10 = VSS-1`,
  `CR16 = VT-2`). Monitors tolerate either. Linux also notes that the registers sometimes "require
  writing twice for the settings to take" `[LX:1269-1276]`. Prism: write the CRTC block, then
  write it again.
* Interlace: CR1A bit0 and CR19 = HT/2, with halved vertical values `[NB:1513-1523]`.

### 2.5 Mode-set sequence

1. **Wakeup (cold board):** write 0x3C3 = 0x01 (NetBSD also writes 46E8 = 0x16, 102 = 0x01,
   46E8 = 0x0E first) `[NB:548-552]`.
2. **SR6 = 0x12**, then read back and require 0x12.
3. **SR1 = 0x21** (screen off), then write the Misc output.
4. SR0 = 0x03, SR2 = 0xFF, SR3 = 0x00, SR4 = 0x0E, **SRF** = 0xB0 (2 MB) or 0x30 (1 MB), SR16 = 0x0A,
   SR18 = 0x02, **SR1F = 0x22**, SR12 = 0x04 (cursor off), SR13 = 0x3C.
5. **SR7** = depth value (§2.2). On the Picasso the high nibble is always 0x2.
6. **VCLK3:** SR0E = N, SR1E = D/P, then the Misc output with bits 3-2 = 11 and the sync polarity.
7. **CR11 = 0x20** (unprotect), CR00-CR18, CR19, CR1A, CR1B. Then write them a second time.
8. GR0-GR8 per §2.2, GR9 = GRA = 0, GRB = 0x28 (5426/28) or 0x20 (5434).
9. Read 0x3DA, write AR0-AR14, then write **0x20** to 0x3C0.
10. HDR via the §2.2 procedure, then 3C6 = 0xFF.
11. BLT reset: **GR31 = 0x04, then GR31 = 0x00** `[NB:629-630]` `[LX:1660-1662]`.
12. Load the palette (8 bpp), clear the framebuffer (BLT fill), set up the cursor.
13. *(First init only)* VRAM size probe. NetBSD sets GR9 = 0x40 (+1 MB), writes 0x12345678 at the
    fb base, and reads it back `[NB:645-665]`. **That probe cannot detect aliasing** on a 1 MB
    board (EMU masks the address and reads back fine). Better: with SRF = 0xB0, write different
    longs at fb+0 and fb+0x100000, read both back, and conclude 2 MB only if both survive. Otherwise
    set SRF = 0x30.
14. **SR1 = 0x01** (screen on), then the monitor switch to VGA (§1.6).

### 2.6 Worked register tables (GD5426/28, Picasso II)

Common to all modes: SR0 = 03, SR1 = 01 (after setup), SR2 = FF, SR3 = 00, SR4 = 0E, SR6 = 12,
SRF = B0 (2 MB), SR16 = 0A, SR18 = 02, SR1F = 22, GR0-4 = 00, GR5 = 40, GR6 = 01, GR7 = 0F,
GR8 = FF, GR9 = GRA = 00, GRB = 28, AR0-F = 0-F, AR10 = 01, AR11 = 00, AR12 = 0F, AR13 = AR14 = 00,
CR08 = 00, CR14 = 00, CR17 = E3, CR18 = FF, CR19 = 00. The tables were generated from the §2.4
formulas and the CRTC values hand-checked.

**A. 640×480, 8 bpp, 60 Hz** (VESA DMT: 25.175 MHz, H 640/16/96/48, V 480/10/2/33, −H −V)

| Misc | SR7 | SR0E | SR1E | HDR | pitch |
|---|---|---|---|---|---|
| EF | 21 | 6D (or 4A) | 3F (or 2B) → 25.17 (25.23) MHz | 00 | 640 B |

| CR00 | 01 | 02 | 03 | 04 | 05 | 06 | 07 | 09 | 10 | 11 | 12 | 13 | 15 | 16 | 1A | 1B |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 5F | 4F | 50 | 83 | 52 | 9E | 0B | 3E | 40 | EA | 2C | DF | 50 | E0 | 0C | 90 | 22 |

**B. 800×600, 8 bpp, 60 Hz** (40.000 MHz, H 800/40/128/88, V 600/1/4/23, +H +V)

| Misc | SR7 | SR0E | SR1E | HDR | pitch |
|---|---|---|---|---|---|
| 2F | 21 | 5F (or 51) | 23 (or 3A) → 40.007 (39.992) MHz | 00 | 800 B |

| CR00 | 01 | 02 | 03 | 04 | 05 | 06 | 07 | 09 | 10 | 11 | 12 | 13 | 15 | 16 | 1A | 1B |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 7F | 63 | 64 | 83 | 69 | 19 | 72 | F0 | 60 | 59 | 2D | 57 | 64 | 58 | 73 | A0 | 22 |

(800×600@72, 50 MHz, H 800/56/120/64, V 600/37/6/23, +H +V: CR00 = 7D 63 64 81 6B 1A 98 F0, CR09 = 60,
CR10 = 7D, CR11 = 23, CR12 = 57, CR15 = 58, CR16 = 99, CR1A = A0, SR0E/1E = 1C/10.)
(1024×768@60 8 bpp, 65 MHz, −H −V: CR00 = A3 7F 80 87 83 94 24 FD, CR09 = 60, CR10 = 03, CR11 = 29, CR12 = FF,
CR13 = 80, CR15 = 00, CR16 = 25, CR1A = E0, SR0E/1E = 3B/1A. This fits 1 MB and is within 86 MHz.)

**C. 640×480, 16 bpp (5:6:5), 60 Hz.** Same CRTC as A except **CR13 = A0** (1280 B / 8), plus
**SR7 = 27**, **HDR = C1** (15 bpp: HDR = D0). The VCLK is unchanged (25.175 MHz), because SR7 = x7
is one VCLK per pixel. Pixels are little-endian in VRAM (§8.1).

**D. 24 bpp.** SR7 = **25**, HDR = **C5**, **VCLK = 3 × pixel clock**, CRTC H values unchanged, pitch =
W×3 (CR13 = W×3/8, so it must be a multiple of 8 bytes). 640×480@60: SR0E = 3A, SR1E = 16
(75.50 MHz), CR13 = F0, and everything else as in A. **800×600 is impossible**: even 56 Hz needs
36 MHz × 3 = 108 MHz, which is over the 86 MHz limit `[NB:1378-1383]`. Memory order is **B,G,R**
per pixel (PCem's 24 bpp renderer reads byte0 = blue; NetBSD notes the Picasso does not swap R/B
lines, unlike the Spectrum/Piccolo `[NB:1165-1171]`). No BLT colour expansion is available in
24 bpp on the 5426/28 (§5). Picasso96 does list "TrueColor" for the PicassoII `[P96FAQ]`.

---

## 3. 8-bit palette

* Write: index → **0x3C8**, then R, G, B → **0x13C8** (the alias of 0x3C9). The index
  auto-increments after B. **The values are 6-bit (0-63)**: write `value8 >> 2` `[NB:1204-1250]`
  `[LX:1303]` (`red >> 10` from 16-bit).
* Read: index → **0x13C6** (0x3C7), then read 3× from 0x13C8.
* Picasso order is **R, G, B**. The Spectrum and Piccolo have the R and B lines swapped, so their
  drivers write B, G, R `[NB:1163-1185]`. **The GBAPII++ order is UNKNOWN** (its driver derives from
  the Piccolo SD64 one, §9).
* Pixel mask 0x3C6 = 0xFF. Do not access 3C6 four times in a row without intending an HDR write
  (§2.2).
* Writes take effect immediately. To avoid snow or tearing, write during vertical retrace (INF).

---

## 4. Display start (panning) and pitch

* **Start address** is in **4-byte units** in all packed modes: `start = byte_offset >> 2`
  `[LX:1314-1345]`.
  `CR0D` = bits 7-0, `CR0C` = bits 15-8, **CR1B bit0 = bit16, bit2 = bit17, bit3 = bit18**
  (read-modify-write CR1B with mask 0xF2) `[LX:1347-1358]` `[WUCL:1186]`. 19 bits × 4 = 2 MB
  of reach. The 5434 adds CR1D bit7 = bit19.
* **Fine pan:** AR13 (PEL panning) is unreliable. Linux: "does not quite work in 8bpp" and it is
  only used for 1 bpp `[LX:1368-1373]`; EMU has depth-specific behaviour `[WUCL:1260-1295]`. Prism:
  pan horizontally in 4-byte steps only (4 px at 8 bpp, 2 px at 16 bpp). CR08 bits 6-5 add a byte
  pan (EMU `[86:2056]`). Leave it at 0.
* **Latching:** standard VGA latches the start address at the start of vertical retrace. Write
  CR0C/0D/1B during display, then wait for 3DA bit3 to rise (INF; standard VGA behaviour).
  Linux waits for the BLT before panning `[LX:1340]`.
* **Pitch (row offset):** `CR13 = (pitch_bytes / 8) & 0xFF`, **CR1B bit4 = bit 8**, so the
  maximum is 511 × 8 = **4088 bytes** and the pitch must be a **multiple of 8 bytes**
  `[NB:1689-1697]` `[LX:1205-1212]` `[WUCL:1171]`.

---

## 5. BitBLT engine (GD5426/5428)

### 5.1 Registers (graphics-controller indices, written via 0x3CE/0x3CF)

| GR | Function | 5426/28 width | Source |
|---|---|---|---|
| 00 / 01 | background / foreground colour byte 0 (doubles as VGA Set/Reset, §1.4) | 8 | `[WUCL:602-605]` `[LX:2715-2716]` |
| 10 / 11 | bg / fg colour byte 1 (16 bpp high byte) | 8 | `[WUCL:673-678]` `[LXH]` |
| 12-15 | bg/fg bytes 2-3 (24/32 bpp, **5434+ only**) | | `[WUCL:2346-2387]` |
| 20 / 21 | **width − 1, in bytes** | **11 bits (≤ 2048 B)**; 5434: 13 bits | `[WUCL:2388-2397]` |
| 22 / 23 | **height − 1** (lines) | **10 bits (≤ 1024)**; 5434: 11 bits | `[WUCL:2398-2407]` |
| 24 / 25 | destination pitch (bytes) | 16 in EMU; XFree masks to 13 bits. Use ≤ 4095 | `[WUCL:2408-2420]` `[XFXAA:76-88]` |
| 26 / 27 | source pitch (bytes) | same | |
| 28 / 29 / 2A | destination byte address in VRAM | **21 bits (2 MB)**; 5434: 22 | `[WUCL:2421-2438]` |
| 2C / 2D / 2E | source byte address | 21 bits | `[WUCL:2439-2452]` |
| 2F | left-edge skip for colour expand (bits 2-0 = pixels skipped at the start of every row) | | `[WUCL:2453]` `[QM-ROP2:183-184]` |
| **30** | **BLT mode**: b0 = backwards (decrement), b1 = dst is system memory (avoid), b2 = **src is system memory (CPU)**, b3 = **transparency**, b4 = **16 bpp** (5426/28: only b4; 5434: b5-4 = 8/16/24/32), b6 = **pattern** (8×8), b7 = **colour expand** (mono → colour) | | `[QM:83-94]` `[WUCL:2456-2462]` |
| **31** | **start/status**: write **0x02** = start, **0x04** = reset (then 0x00). Read: **b0 = busy**, b3 = "in progress" (set at start, cleared at the end), b7 = autostart (5436+ only) | | `[QM:96-101]` `[WUCL:2008-2012,2478-2484]` `[LX:1660-1662,2586-2596,1720-1728]` |
| **32** | **ROP** (§5.3) | | `[QM:103-119]` |
| 33 | mode extensions (solid fill, invert expand). **5436+ only, ignored by 5426/28** | | `[QM:124-127]` `[WUCL:2468-2472]` |
| 34 / 35 | transparent colour key (low/high) | 5426/28 only (not 5434) | `[86:690-699]` `[NBH]` |
| 38 / 39 | transparent colour mask (1 = ignore bit) | 5426/28 only | `[86:1167-1173]` `[NBH]` `[XFXAA:259-266]`. WinUAE's older PCem core uses 36/37 instead `[WUCL:764-771]` |

Order: program everything, write GR30/GR32, and write **GR31 = 0x02 last**. Linux writes the
pitch, width, height, destination, source, mode, ROP, then start `[LX:2598-2656]`.

### 5.2 Operations

* **Screen-to-screen copy:** GR30 = 0x00 (+0x01 backwards); ROP 0x0D; width = bytes − 1, so 16 bpp
  is pixels×2 − 1. A plain copy is byte-based, so GR30 b4 is not needed and the copy works at any
  depth, including 24 bpp. **Overlap:** if the source address < the destination address (same
  row: srcX < dstX), use backwards mode and point **both addresses at the last byte**:
  `addr += (h-1)*pitch + (wbytes-1)`. The pitches stay positive `[LX:2658-2700]`
  `[XFXAA:95-108]`.
* **Solid fill (5426/28):** there is no solid-fill bit (that is GR33 on 5436+). Use **colour expand +
  pattern with fg = bg = colour**, so the pattern content does not matter: GR30 = **0xC0** (8 bpp) /
  **0xD0** (16 bpp), GR0 = GR1 = colour low byte, GR10 = GR11 = high byte (16 bpp), source address =
  any 8-byte-aligned VRAM location, ROP 0x0D `[LX:2702-2738]` (Linux uses 0x80|0x40 = 0xC0).
* **Mono pattern fill (8×8):** GR30 = 0xC0/0xD0. The source points at 8 bytes in VRAM, one byte per
  row, MSB = leftmost pixel. **Bits 2-0 of the source address select the starting row**, so the
  base must be 8-byte aligned (EMU `[WUCL:2018-2019,2180-2215]`). The pattern x phase restarts at
  each row's first BLT pixel, so the pattern is aligned to the BLT, not to the screen (INF from EMU).
  To align it to the screen, pre-rotate the pattern.
* **Colour pattern (8×8):** GR30 = 0x40 (8 bpp: 64 bytes) / 0x50 (16 bpp: 128 bytes). Align the
  source to the pattern size (EMU indexes `(src & ~7) + row*8/16`) `[WUCL:2126-2140]`
  `[QM:882-889]`.
* **Colour expansion from VRAM (text, templates):** GR30 = 0x80/0x90. The source is packed
  1 bpp, **each row padded to a byte, consecutive rows contiguous. The source pitch is ignored.**
  MSB first. GR2F skips 0-7 pixels at the left of every row (`[QM-ROP2:171-204]`
  `[WUCL:2144-2178]`).
* **Colour expansion from the CPU (system-memory source):** GR30 = 0x84/0x94, then start. Every
  write to **anywhere in the VRAM aperture** feeds the source FIFO. Bytes are consumed in 68k memory
  order, MSB = leftmost pixel. Rules:
  * The CPU must write **longs** (`move.l`). In WinUAE byte writes are **ignored** during a
    system-source BLT, and word writes are paired into longs (EMU `[WUCL:2023-2040,2554-2595]`).
  * **Row padding differs:** QEMU and real-hardware drivers treat the rows as *byte-padded and
    contiguous*, with only the total padded to 32 bits (`[QM:891-901]`; Linux `imageblit` `memcpy`s
    byte-padded rows `[LX:1811-1860]`; XFree: "XF86 sends DWORD-padded data, not byte-padded",
    so the chip wants byte-padded `[XFXAA:630-670]`). **WinUAE discards the rest of the current
    long at the end of each row, i.e. it expects DWORD-padded rows** (EMU
    `[WUCL:2317-2330]` "if (mode & 0x04) return" at row end). **Portable rule:** run one BLT per
    row, or use only widths whose rows are a multiple of 32 pixels. Otherwise stage the bitmap in
    VRAM and use VRAM-sourced colour expansion, which agrees everywhere.
  * Do not read VRAM while a system-source BLT is active: WinUAE replaces the aperture handlers
    (EMU `[WUCL:2023-2040]`).
* **Non-expanding CPU→screen copy** (GR30 = 0x04/0x14): the CPU streams raw pixel bytes in memory
  order, with rows padded to 32 bits `[QM:897-899]`. 16 bpp data must already be in VRAM byte order
  (§8.1).
* **Transparency (GR30 bit 3):**
  * With colour expansion: 0-bits are not written and only foreground is drawn (EMU
    `[WUCL:2240-2290]` `[86:4003-4019]`). XFree on real silicon additionally sets **GR34/35 = bg
    colour, GR38/39 = 0, with bg = ~fg** `[XFXAA:250-275]`. **Do the same**: it is harmless in EMU and
    required (INF) on hardware.
  * With a plain copy (5426/28 only): destination pixels whose source equals the GR34/35 key
    (with GR38/39 as don't-care mask) are skipped. This works for 8 and 16 bpp only `[86:690-699,
    4024-4032,4490-4500]`. **WinUAE 6.0.3's PCem core does not implement colour-keyed copies**
    (the key is stored but never compared) `[WUCL:756-771,2240-2290]`. **The 5434 has no
    transparency key at all** `[86:690-699]`.
* **Limits:** at 8 bpp a single BLT can be at most 2048 B wide × 1024 lines, at 16 bpp 1024 px wide.
  Split larger rectangles. The 5426/28 has no 24 bpp expand/pattern (GR30 depth is bit4 only,
  EMU `[WUCL:2456-2462]`), and Linux expands 24 bpp only on non-Alpine chips `[LX:1811-1860]`.
  Treat 24 bpp as copy-only.

### 5.3 ROP codes (GR32), D = destination, S = source

| GR32 | Result | GR32 | Result | GR32 | Result | GR32 | Result |
|---|---|---|---|---|---|---|---|
| 0x00 | 0 | 0x0B | ~D | 0x59 | S^D | 0xAD | S\|~D |
| 0x05 | S&D | 0x0D | **S (copy)** | 0x6D | S\|D | 0xD0 | ~S |
| 0x06 | D (nop) | 0x0E | 1 | 0x90 | ~(S\|D) | 0xD6 | ~S\|D |
| 0x09 | S&~D | 0x50 | ~S&D | 0x95 | ~(S^D) | 0xDA | ~(S&D) |

`[QM:103-119]` `[WUCL:2220-2238]` `[XFXAA:25-42]` (X11 GX→Cirrus map).

### 5.4 Completion

* Poll **GR31 & 0x01** (XFree `WAIT_1`, `[XFXAA:20-23]`). Linux polls `& 0x08` before starting a
  new BLT and `& 0x03` for sync `[LX:2586-2596,1720-1728]`. **Prism: wait while (GR31 & 0x09) != 0.**
  Reading GR31 means writing 0x31 to 0x3CE and reading 0x3CF.
* WinUAE runs VRAM→VRAM BLTs **synchronously inside the GR31 write**, so busy is never seen (EMU
  `[WUCL:2008-2060]`). Real hardware is asynchronous. **Always wait before the next BLT, before
  touching GR0/GR1/GR10/GR11, before the CPU accesses VRAM in the BLT's area, and before panning.**
* No BLT-done interrupt exists on the 5426/28 path (none in any source).

### 5.5 Known quirks and bugs

* GR0/GR1 Set/Reset interaction (§1.4).
* WinUAE vs QEMU/HW CPU-colour-expand row padding (§5.2).
* WinUAE ignores the colour-key copy, and GR36/37 vs GR38/39 confusion between the cores (§5.1).
* The NetBSD 542x driver never used the blitter ("TODO: Blitter support"), so it offers no
  real-hardware BLT evidence for the PII. The Linux BT_PICASSO path does use it `[NB:70-72]`
  `[LX:1729-1860]`.

---

## 6. Hardware cursor

| Reg | Function | Source |
|---|---|---|
| SR10 | **X bits 10-3**. X bits 2-0 go in **bits 7-5 of the index byte** written to 0x3C4: `out 3C4, 0x10 | (x&7)<<5 ; out 3C5, x>>3` | `[WUCL:530-539]` `[NB:851-869]` |
| SR11 | Y likewise (index 0x11 \| (y&7)<<5). A 16-bit write `0x1100|(y&7)<<13|(y>>3)` to 0x3C4 works; NetBSD says the same 16-bit trick **does not work for X** | `[NB:851-869]` |
| SR12 | bit0 = **enable**, bit1 = **expose the cursor colours in the DAC**, bit2 = **64×64** (else 32×32) | `[QM:83-87]` `[WUCL:540-548]` `[NB:1029-1075]` |
| SR13 | pattern select. Base = **VRAM end − 16 KB** (2 MB: 0x1FC000). 32×32: addr = base + (SR13 & 0x3F) × 256. 64×64: addr = base + (SR13 & 0x3C) × 256 | `[WUCL:540-556]` `[QM-WU]` `cirrus_cursor_compute_yrange` |

* **Pattern format:** two 1-bit planes, MSB = leftmost pixel.
  * **32×32 (256 B):** plane 0 = bytes 0-127 (4 B/row), plane 1 = bytes 128-255 `[WUCL:1338-1358]`.
  * **64×64 (1 KB):** per row, 8 bytes of plane 0 then 8 bytes of plane 1 (16 B/row) `[WUCL:1312-1336]`.
  * Pixel = (plane0, plane1): (0,0) transparent, (1,0) **invert screen**, (0,1) **cursor colour 0**,
    (1,1) **cursor colour 1** `[WUCL:1322-1327]` (QEMU agrees).
* **Colours:** set SR12 bit1, write 3C8 = **0x00** and R,G,B (colour 0), write 3C8 = **0x0F** and
  R,G,B (colour 1), all 6-bit and in Picasso order R,G,B. Then clear bit1. The normal palette entries
  0 and 15 are not disturbed `[NB:1029-1062]` `[WUCL:590-600,899-908]`.
* **No negative coordinates.** For a hot spot or a left/top clip, write 0 and shift the pattern bits
  (NetBSD `writeshifted`, `[NB:871-940]`).
* **Quirk (5426/28, 15/16 bpp):** WinUAE draws the cursor **8 pixels off** when GR5 bit6 = 0. Its
  comment says Picasso96 needs the offset but CGX4 (which sets GR5 = 0x40) does not, and calls it a
  "possible chip bug" `[WUCL:1228-1235]`. **Keep GR5 = 0x40**, and verify the X offset on real
  hardware.
* QEMU-in-WinUAE notes that CR17 bit2 (vertical ×2) doubles the cursor Y on chips before the 5446
  `[QM-WU:1351-1360]`.
* NetBSD uses 64×64 at `fbsize − 1024` with SR13 = 0x3C `[NB:957,605]`.

---

## 7. VBlank and interrupts

* **Polling:** **0x3DA (Input Status 1) bit 3 = 1 during vertical retrace (sync)**; bit 0 = 1
  during any blanking (display disabled). Reading 0x3DA also resets the attribute flip-flop
  `[WU-SVGA:306-313]` `[NBH]` `GREG_STATUS1_R`. To wait for the start of vblank: wait while bit3 = 1,
  then wait until bit3 = 1.
* **Chip interrupt:** the vertical interrupt is enabled with **CR11 bit5 = 0**. Pending is shown in
  **0x3C2 (Input Status 0) bit 7**. To acknowledge, write CR11 with **bit4 = 0, then bit4 = 1**,
  preserving bits 3-0 (VSyncEnd) (EMU `[WUCL:162-183,788-797,924-927]`).
* **Picasso II (GD5426): no interrupt line** (WinUAE `irq = 0`, `[WU:225]`).
* **Picasso II+ (GD5428): INT2 (PORTS)** in WinUAE (`irq = 2`, `[WU:232]`, `[WU:931-960]`).
  Board-level enable: write to reg offset **0x9001 → enable**, **0x9000 → disable** (any value;
  0xB000 also disables) (EMU `[WU:6405-6411]`). The older QEMU path used 0x1001/0x1000 instead
  `[WU:3326-3332]`. **Real II+ hardware: UNKNOWN.** The handler must be an INT2 server, should check
  0x3C2 bit7, and should acknowledge through CR11.
* **GBAPII++: no interrupt pin** in the CPLD port list `[GB-V:20-47]`. Poll 0x3DA, or use the
  Amiga's own VERTB (which is not in sync with the VGA timing).
* WinUAE's PCem path emulates a short hsync pulse so polling loops do not hang `[WU:910-929]`.

---

## 8. Gotchas

### 8.1 Endianness and pixel formats

| Depth | VRAM layout (byte n = 68k byte n) | Picasso96 name | Source |
|---|---|---|---|
| 8 | 1 byte CLUT index | CLUT | |
| 15 | little-endian 0RRRRRGG GGGBBBBB, **low byte first** | R5G5B5PC | EMU: 16-bit read little-endian + WinUAE keeps byte order `[WU:5562-5590]`. P96 lists HiColor15/16 for the PII `[P96FAQ]` |
| 16 | little-endian RRRRRGGG GGGBBBBB, **low byte first** | R5G6B5PC | same. Swap with `rol.w #8,d0` before storing |
| 24 | B, G, R | B8G8R8 | §2.6 D |

The Picasso II has **no byte-swapping aperture**. All conversion is CPU work, or it is avoided by
working natively in PC formats.

### 8.2 Things the source drivers warn about

* "Write the registers twice" (Linux, `[LX:1269-1276]`).
* HDR access needs the 3C6 = 0 / 3C8-read dance on some boards: "out of 'secret' application note"
  `[NB:1638-1650]` `[LX:2474-2480]`.
* The DAC data/read-index ports must use the +0xFFF alias `[LX:2418,2434,2547]`.
* There is a 200 ms delay after every pass-through toggle (NetBSD `delay(200000)`, `[NB:1791-1856]`).
* "SR7 … 0x20 for Picasso vs 0x80": `cltype == PICASSO ? 0x20 : 0x80`, which is the aperture
  nibble, not a depth bit `[NB:575,1634-1637]`.
* NetBSD writes GRA = 0xEC "magic cookie" before SR6 `[NB:562]`. Linux comments the same line out
  ("doesn't make any sense to me") `[LX:1521-1523]`. Skip it.
* The cursor X 16-bit write fails (§6).

### 8.3 WinUAE 6.0.3 vs real hardware

| Topic | WinUAE (PCem GD5428 core) | Real / other sources |
|---|---|---|
| Monitor switch | only 0xA000 / 0xB000 (A13 must be set) `[WU:6397]` | NetBSD/Linux use 0x8000 / 0x9000 |
| II+ interrupt enable | 0x9001 / 0x9000 `[WU:6405]` | unknown |
| Register long writes | dropped | probably two cycles on hardware |
| Linear aperture | bounds ignored, decode mask from SRF[7] | real window size depends on GRB bit5 `[86:1885-1895]` |
| BLT timing | VRAM BLTs complete instantly | asynchronous, so poll GR31 |
| CPU colour-expand padding | per-row DWORD padding | byte-padded rows (QEMU, XFree, Linux) |
| Colour-key copy (GR34/35) | not implemented | 5426/28 have it `[86]` |
| HW cursor | 8 px offset in 15/16 bpp if GR5 bit6 = 0 | "possible chip bug", unconfirmed |
| BLT GR2F skip | applied as a destination write mask | QEMU: source + destination skip |
| VCLK | any N/D accepted | keep the VCO in 28.6-111 MHz |
| Chip | always GD5428 (II+) / GD5426 (II) | your GBAPII++ is a **GD5434** |

### 8.4 Zorro II / CPU practicalities

* Every register write costs a full ISA-bridged bus cycle (§1.5). Batch the BLT register setup and
  skip registers whose values did not change, such as the pitch.
* The VRAM on these boards is Zorro II memory but **not system RAM**. Never let `AddMem` or a
  memory-board scan claim it. Picasso96 finds it by autoconfig ID.

### 8.5 GD5434 differences (for the GBAPII++)

From 86Box `[86]`, Linux SD64/Alpine paths `[LX]` and NetBSD's `cl_64bit` paths `[NB]`.
**Verify every item on the real board.**

| Item | 5426/28 | 5434 |
|---|---|---|
| CR27 | 0x90 / 0x98 | 0xA8 (0xA4 early rev) |
| DRAM bus / SRF | 32-bit, 0xB0 for 2 MB | 64-bit, **0x38 for 2 MB** (0xB8 = 4 MB) `[NB:567,1527-1531]` |
| SR16 / SR1F | 0x0A / 0x22 | **0x5A / 0x1C** (MCLK 50.1 MHz) `[NB:577-590]` |
| SR7 depth bits | 2-1 | 3-1, adds `1000` = 32 bpp. 24 bpp is **not** VCLK×3 `[86:2012-2020]` `[QM:66-75]` |
| SR1E | D in bits 5-1 | Linux sets bit7 for SD64/Alpine ("6 bit denom; ONLY 5434!!!") `[LX:886-893]`. Meaning UNKNOWN |
| 8 bpp > 85 MHz | no | clock doubling (SR7 = x7 + HDR = 0x4A), up to about 135 MHz `[LX:497-512]` `[NB:1660]` |
| GRB | 0x28 | **0x20** (bit3 breaks BLT) `[LX:1611-1621]` |
| BLT width / height / address | 11 / 10 / 21 bits | **13 / 11 / 22 bits** `[WUCL:2388-2452]` |
| BLT colours | GR0/1 + GR10/11 | + GR12-15 (24/32 bpp), GR30 bits 5-4 = depth `[LX:2715-2737]` |
| Colour-key copy | yes (GR34/35/38/39) | **no** `[86:690-699]` |
| Start address | 19 bits | + CR1D bit7 |
| Linear aperture | SR7[7:4] (ISA mode) | same in ISA mode `[86:1885]`. **The GBAPII++ ISA address-bus wiring is UNKNOWN**, so the correct SR7 nibble (0x2_ like the PII, 0x8_ like the Piccolo SD64) must be found by test |
| MMIO BLT registers at B8000 | no | via SR17 bit2 (5429+), not reachable on the GBAPII++ (INF). Use GR I/O |

---

## 9. Open questions and bring-up tests

1. **GBAPII++ identity:** ShowConfig mfr/product (16/17? 11/12? 2195/10/11?) and the **CR27** value.
   The user expects a 5426/28, but both GitLab and the vendor pages say GD5434.
2. **Monitor switch on a real original Picasso II:** does 0xA000/0xB000 work (A13 decoded or
   ignored)? It works on the GBAPII++ by construction.
3. **GBAPII++ SR7 aperture nibble:** write a pattern via the Zorro window with SR7 = 0x21, then 0x81,
   and see which one sticks.
4. **GBAPII++ byte lanes:** write word 0x1234 at fb+0 and read bytes fb+0/fb+1 (expect 0x12, 0x34).
   Also check 16 bpp pixel order and DAC R/G/B order (the driver lineage is the Piccolo SD64, whose
   board swaps R and B `[NB:1165-1171]`).
5. **II+ interrupt** register on real hardware (no source found).
6. **CPU colour-expand row padding** on real silicon (byte vs dword). Run a 9-px-wide test glyph.
7. **HW cursor X offset** in 16 bpp with GR5 = 0x40 on silicon.
8. **HDR access delays:** are any needed?
9. **Real 5426/28 VCLK/VCO limits:** no datasheet was found. The 86 MHz cap comes from NetBSD.
10. GR38/39 vs GR36/37 for the transparency mask on real 5426/28 (NetBSD header + XFree + 86Box
    say 38/39).

---

## 10. Sources (pinned revisions; `[TAG:line]` refers to these files)

| Tag | Source |
|---|---|
| NB | NetBSD `sys/arch/amiga/dev/grf_cl.c` @ trunk f3530e0 (2026-08-15): <https://github.com/NetBSD/src/blob/trunk/sys/arch/amiga/dev/grf_cl.c>: `grfclmatch` 257, attach/`serno` 475, `cl_boardinit` 530-668, `cl_writesprpos` 851, `cl_setspriteinfo` 954, `cl_getcmap/putcmap` 1138-1250, `cl_CompFQ` 1270, `cl_mondefok` 1348, `cl_load_mon` 1411-1718, `RegWakeup/RegOnpass/RegOffpass` 1791-1856 |
| NBH | NetBSD `grf_clreg.h`: <https://github.com/NetBSD/src/blob/trunk/sys/arch/amiga/dev/grf_clreg.h> (register names, `PASS_ADDRESS 0x8000`, `PASS_ADDRESS_WP 0x9000`, `VDAC_DATA = 0x3c9+0xfff` for the Picasso) |
| LX | Linux `drivers/video/fbdev/cirrusfb.c` @ e840a23 (2026-04-30): <https://github.com/torvalds/linux/blob/master/drivers/video/fbdev/cirrusfb.c>: board info 108-250, Zorro table 276-345, `cirrusfb_set_par_foo` 659-1267, set twice 1269, pan 1314, blank 1378, `init_vgachip` 1447-1660, `switch_monitor` 1669, sync 1720, imageblit 1811, Zorro probe 2196-2290, `WGen/RGen` 2412-2442, `WHDR` 2469, `WSFR/WSFR2` 2509-2529, `WClut` 2531, `WaitBLT` 2586, `set_blitter` 2598, `BitBLT` 2658, `RectFill` 2702, `bestclock` 2741 |
| LXH | Linux `include/video/cirrus.h`: <https://github.com/torvalds/linux/blob/master/include/video/cirrus.h> (register names/notes such as "do not access", "5428 only") |
| ZIDS | Linux `drivers/zorro/zorro.ids`: <https://github.com/torvalds/linux/blob/master/drivers/zorro/zorro.ids> (0877: 0B RAM, 0C, 0D segmented) |
| WU | WinUAE `gfxboard.cpp` **tag 6030 (= 6.0.3)**: <https://github.com/tonioni/WinUAE/blob/6030/gfxboard.cpp>: board table 222-232, `gfxboard_rethink` 931, `set_monswitch` 1688, Z2 mapping for PII 3265-3280, `mungeaddr` 3316 (QEMU path), `gfxboard_bput_regs` 3437 (QEMU path), VRAM byte order `gfxboard_wput/lput_vram_pcem` 5562-5590, `special_pcem_put` PII branch 6391-6425, `special_pcem_get` 6675/6808 |
| WUCL | WinUAE `pcem/vid_cl5429.cpp` tag 6030: <https://github.com/tonioni/WinUAE/blob/6030/pcem/vid_cl5429.cpp>: vsync IRQ 157-183, `gd5429_out` 496 (SR10-13 530-556, SR7 558, HDR 576, GR 602-775, CR11 788), `gd5429_in` 911 (SR6 949, CR27 1016), banking 1052, `recalc_mapping` 1076, `recalctimings` 1164 (HDR decode 1193, cursor quirk 1229-1235, VCLK 1237), cursor draw 1298, `write_linear` 1447, `start_blit` 1968, `mmio_write` (BLT regs) 2337, `blt_write_w/l` 2554-2595 |
| WUG / WU-SVGA | WinUAE `pcem/pcemglue.cpp` (linear-mapping capture in mapping recalc, line 982 at tag 6030) and `pcem/vid_svga.cpp` (0x3DA 306, set/reset 1069/1118): <https://github.com/tonioni/WinUAE/tree/6030/pcem> |
| QM-WU | WinUAE `qemuvga/cirrus_vga.cpp` (older QEMU core, still used by some boards): <https://github.com/tonioni/WinUAE/blob/master/qemuvga/cirrus_vga.cpp>: 1220 (2 MB + SRF[7] note), 1351 (CR17 cursor note) |
| QM | QEMU `hw/display/cirrus_vga.c` @ 405a42e: <https://github.com/qemu/qemu/blob/master/hw/display/cirrus_vga.c>: bit definitions 64-160, `cirrus_bitblt_cputovideo` 870-911, cursor 2138-2311 |
| QM-ROP2 | QEMU `hw/display/cirrus_vga_rop2.h`: <https://github.com/qemu/qemu/blob/master/hw/display/cirrus_vga_rop2.h>: colour expand 171-204 |
| 86 | 86Box `src/video/vid_cl54xx.c` @ 0ecf7e6 (2026-09-22): <https://github.com/86Box/86Box/blob/master/src/video/vid_cl54xx.c>: IDs 73-88, `gd54xx_has_transp` 690, GR34/38 mapping 1159-1173, `recalc_mapping` 1835-1935, recalctimings/VCLK 1950-2060, BLT transparency 3935-4035, 4480-4530 |
| XFCLK | xf86-video-cirrus `src/CirrusClk.c`: <https://gitlab.freedesktop.org/xorg/driver/xf86-video-cirrus/-/blob/master/src/CirrusClk.c> (formula, VCO limits, tested clock table) |
| XFXAA | xf86-video-cirrus 1.5.3 `src/alp_xaa.c`: <https://gitlab.freedesktop.org/xorg/driver/xf86-video-cirrus/-/blob/xf86-video-cirrus-1.5.3/src/alp_xaa.c> (ROP map 25-42, copy 75-125, transparency 250-275, padding comments 640-655) |
| GB-README / GB-V | GBAPII++ / A500-GraKa: <https://gitlab.com/MHeinrichs/A500-GraKa> `readme.md`; CPLD `Logic/GBAPIIPlusPlus-V2/GBAPIIPlusPlus.v` (ports 20-47, reset values 140-176, SA0/SA12 217-230, bus state machine 237-376, monitor switch 301, autoconfig 378-470) |
| ERKAN | <https://amiga.erkan.se/a500-graka-gbapii-amiga-graphics-card-built-and-tested/> (GD5434, 2 MB, Ratte auto-switch) |
| P96FAQ | Picasso96 FAQ (1998): <http://cd.textfiles.com/amigama/amigama199804/WWW/Picasso96/FAQ.html> (segmented mode = ID 13 unsupported; PII colour formats) |

## 9. Verified on the real GBAPII++ (A2000, 2026-10-03)

First contact with the real board (A2000, TF536 68030 @ 50 MHz, ECS NTSC, OS 3.2 / Kickstart
47.115), Picasso96 parked:

* **Autoconfig:** VRAM 2167/16 at `$200000` (2 MB, Zorro II), registers 2167/17 at `$E90000` (64 KB).
* **The chip is asleep after a reset.** Nothing initialises it on the Amiga (no VGA BIOS), and an
  asleep GD542x ignores I/O: every read returns the last index written (SR6 read `06`, CR27 `27`,
  so a probe sees "chip id `$18`/`$27`"). Waking it with the ISA sequence Linux's cirrusfb uses on
  Zorro boards - **`0x46E8 = 0x10`, `0x102 = 0x01`, `0x46E8 = 0x08`, `0x3C3 = 0x01`** - makes it
  answer: SR6 unlocks to `12`, **CR27 = `$A8` = GD5434**. WinUAE ignores these ports (its chip is
  always awake), which is why the emulator never needed them. `tools/p2peek` shows before/after.
* **Aperture nibble `0x2_` works** (VRAM writes land and read back), as on the original Picasso II.
* **Mode set:** 640x480 8-bit with SR0E `66` / SR1E `3B` = 25.18 MHz; ST1 shows it scanning out.
* **Blitter:** fills (colour expansion) and copies including overlapping backwards copies match a
  CPU model byte for byte at 8, 16 and 24 bits (`PrismTest BLIT`), with GRB = `0x20` on the 5434.
* **Modes:** 640x400-1024x768 at 8 bits, up to 1024x768 at 16 bits and up to 800x600 at 24 bits
  (the 5434's 57.3 MHz limit at 16/24 bits, no VCLK tripling) - 11 modes.
* Seen on a monitor since: see §9.2.

### 9.1 More from the real GD5434 (optimisation pass, 2026-10-03)

* **16-bit writes to an index/data pair work** (index in the high byte): tested on GR0, used for
  all BLT registers. One bus cycle instead of two.
* **GR1/GR11 left non-zero do not alter CPU writes to VRAM** in packed-pixel modes (the 5429+
  switch VGA set/reset off via SR7 bit 0) - tested by writing bytes and a word with GR1 = GR11 =
  $FF. WinUAE's GD5428 does alter them, as the real 5426/28 would.
* **Pattern fills ignore the source pitch** (works with GR26/27 = 0).
* **WinUAE's chip counts in GR28-2A / GR2C-2E during a blit** (a second blit that rewrote only the
  low address byte went to the wrong place). Whether the real chip does is untested; the driver
  always writes all address bytes.
* Bus timing: see `tools/iotime` and the table in design.md.

### 9.2 On a real monitor (Dell P190S on the GBAPII++, 2026-10-03)

* 640x480 8-bit, 60 Hz: good picture (windows, text, lines, hardware pointer).
* **24-bit needs VCLK = 3x the pixel clock on the GD5434 too.** With VCLK = pixel clock (what §2.3
  said, from 86Box's source) the monitor reported "timing is off"; with the clock tripled, 640x480
  24-bit shows a correct true-colour picture (SR7 = x5, HDR = $C5, B,G,R byte order). Linux's
  cirrusfb also triples for the 543x. The limit is then the synthesiser (VCO <= 111 MHz), so 24-bit
  stops at 640x480 - Picasso96's mode list for this card stops there as well and offers 800x600 as
  32-bit instead (a different SR7 mode without the triple clock; not in Prism yet).
* 800x600 16-bit, 1024x768 16-bit and 640x480 24-bit all show a correct picture on the same
  monitor (a photo through `PrismShow`, mode name in the corner), 60 Hz timings.
* **32-bit (800x600, SR7 = x9) works** once the CRTC offset is written as pitch / 16: with pitch / 8
  the monitor showed every other row at half height, then the scratch area, then VRAM wrapping
  round (diagnosed from a phone photo). Test card confirmed good at 32-bit and 16-bit.

## 9.3 DDC / EDID on the GBAPII++: not wired (2026-10-04)

`tools/ddcprobe.c` bit-bangs I2C on SR08 (bit 0 clock out, bit 1 data out,
bit 2 clock in, bit 7 data in, bit 6 enable). On the A2000's GBAPII++
(GD5434) with a DDC-capable monitor on the VGA connector:

    SR08 write > read: 40>40 41>45 42>42 43>47 80>00 ff>7f

The clock input follows the clock output once bit 6 is set, but the data
input (bit 7) reads 0 whatever is driven: a released I2C data line would
read 1. So the board does not connect the chip's DDC data pin, and Prism
cannot read the monitor's EDID on this card. (With a stuck-low data line
every byte looks acknowledged and the "EDID" is all zeroes - the probe now
checks the idle level first.) An original Picasso II has not been probed.
