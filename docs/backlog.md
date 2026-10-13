# Backlog

Features an RTG system is expected to have that PrismRTG does not yet,
roughly in the order users ask for them. Kept here so nothing is
forgotten; none of these is promised for a given release.

Done 2026-10-11: the mode editor (2), video overlay (1), gamma (4), oversized screens with autoscroll (5), larger pointers (6).
Done 2026-10-13: monitor power saving (3), the CyberGraphX 43 calls (7), Picasso96's settings (9), the ARexx port (10), and a native Picasso IV driver (Zorro III mode).

1. **Video overlay.** A YUV window with colour keying through
   cgxvideo.library and the Picasso96 PIP calls with YUV formats, for
   video players. The Cirrus 5426/5428/5434 have no overlay engine, so
   this is a software path: the player's YUV frames are converted on the
   CPU and shown through the PIP compositor. The ZZ9000's firmware (2.8.1)
   has OP_WRITE_YUV (the ARM converts a YUV 4:2:2 rectangle into the
   framebuffer) and OP_VIDEO_OVERLAY (overlay_handle_op): a later step
   can hand the conversion to the card there. Done 2026-10-12: the
   ZZ9000 driver's overlaySet/overlayOff speak OP_VIDEO_OVERLAY
   (BlitterStudio firmware 2.8+); a cgxvideo layer with a colour key, in a
   firmware YUV format, on a 16- or 32-bit screen, is shown by the board;
   everything else stays on the CPU path (HWOVERLAY=OFF forces it).
2. **A mode editor.** Custom resolutions and refresh rates per monitor
   with a test button, as Picasso96Mode and CGXMode have. Where the driver
   computes its own timings (the Cirrus family, UAE) any size can be
   offered; the ZZ9000 has a fixed list in its firmware.
3. **Monitor power saving.** DPMS standby and off from the screen
   blanker. The Cirrus register bits are in docs/cirrus-picasso2-hw.md
   (GRE on the 5428, the sync bits on the 5434). Done 2026-10-13:
   POWERSAVE= minutes ("Sleep" in PrismPrefs), an input handler in PrismD,
   the board hook setPower (the Cirrus boards but the Picasso IV: SR1
   picture off and both GRE syncs off), else a black palette or a cleared
   bitmap in VRAM on display.
4. **Gamma and brightness** per board where the DAC supports it.
5. **Oversized screens with autoscroll.** A screen larger than the display
   that pans with the pointer. The panning primitive exists in the
   drivers; the rest is Intuition-side work (SA_Overscan, SA_AutoScroll).
6. **Larger and multi-colour pointers** from the OS 3.2 pointer
   preferences. The classic two-colour sprite is handled; bigger ones fall
   to the software pointer today. (Larger: done 2026-10-11. Multi-colour:
   closed 2026-10-12 - OS 3.2's pointerclass hands over a two-plane sprite
   even from a four-plane bitmap, es_flags 0, no attached second sprite,
   so Intuition never offers a 16-colour pointer and there is nothing for
   PrismD to show; the ZZ9000's CLUT cursor would take 15 pens if it did.)
7. **The newer CyberGraphX calls** (WritePixelArrayAlpha,
   ProcessPixelArray, ...). Check which our cybergraphics.library answers;
   a few modern programs use them. Done 2026-10-13: WritePixelArrayAlpha,
   BltTemplateAlpha and ProcessPixelArray (every operation); the library
   says version 43.
8. **Planar screens on the card** with a chunky conversion, so old
   planar-only software runs on a card-only monitor. Low priority now
   that screen mode promotion exists.
9. **Settings from the other systems.** Reading Picasso96's settings on a
   first install so a user who switches keeps their resolutions. Done
   2026-10-13: PrismImport (DEVS:Picasso96Settings, IFF P96S), offered by
   the installer on a first install.
10. **An ARexx port** on PrismD or PrismPrefs, for scripts that switch
    modes or query the board. Done 2026-10-13: PRISM on PrismD (docs/arexx.md).
11. **Update mirror.** An HTTPS directory of the author's with latest.txt
    and the archive, for the MIRROR= setting (the tool supports it; no
    mirror exists yet).

Out of scope: 3D acceleration (no supported card has it).
