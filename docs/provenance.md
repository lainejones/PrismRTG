# Where PrismRTG's code comes from

PrismRTG is GPL-3.0-only. Its code was written for it, by Laine Jones
(with Claude, Anthropic's AI, as co-author in every commit) and Stefan
Reinauer, whose pull requests are merged under his own name. Each source
file names its authors in its header, and `git log` shows who wrote what.

## Third-party files

| Files | From | Licence | Why |
|---|---|---|---|
| `src/p96sdk/boardinfo.h`, `src/p96sdk/settings.h`, `src/p96sdk/libraries/Picasso96.h` | P96 driver development files and P96 development files 3.6.0, Individual Computers (original authors Alexander Kneer and Tobias Abt), on Aminet via Thomas Richter | CC-BY 4.0, granted in the archives' readmes (included as `src/p96sdk/*.readme`) | The interface of Picasso96 card drivers, which the optional P96 adapter (`BOARD=P96`) talks to. Copied unchanged, original copyright line kept; see `src/p96sdk/README`. |
| `LICENSE` | Free Software Foundation | verbatim copying permitted | The GPL text. |

Nothing else in the repository's code, and nothing in the release
archives, comes from another project. The icons are drawn by
`tools/mkicons.py`. The pictures in `docs/` are screenshots of
PrismRTG running other people's programs (AmigaOS Workbench, Directory
Opus, IBrowse, NetSurf, MysticView, Personal Paint, Doom, OpenDune and
others), shown to document what works; those programs belong to their
authors.

## Interfaces reproduced

PrismRTG provides its own `cybergraphics.library` and
`Picasso96API.library`, and patches `graphics.library`, so that existing
programs work on it. For that it reproduces those interfaces' published
numbers: function order (library vector offsets), tag values, pixel
format codes and structure layouts. Those come from the AmigaOS NDK, the
CyberGraphX developer headers and the P96 development headers above.
The implementations behind them are PrismRTG's own. `src/p96api.h` lists
the Picasso96 API numbers PrismRTG uses.

## Hardware facts

No data book was available for the boards. Register numbers, bit
meanings and command sequences were read from public source code and
written down, with file and line references, in
[cirrus-picasso2-hw.md](cirrus-picasso2-hw.md) (Cirrus GD542x/543x boards:
NetBSD, Linux, WinUAE/PCem, QEMU, 86Box, xf86-video-cirrus, the GBAPII++
design) and [zz9000-hw.md](zz9000-hw.md) (MNT's ZZ9000 driver and
firmware). The drivers were then written from those notes. The README's
credits name every source.

## Checking it

`tools/provenance/` compares every PrismRTG source file with those public
sources and with the closest other implementations (MNT's ZZ9000 drivers,
the P96 example Cirrus driver, open P96 card drivers, WinUAE's Picasso96
emulation, the Z3660 driver):

```
sh tools/provenance/fetch.sh
python3 tools/provenance/compare.py
```

It reports, for each pair of files, the longest run of identical tokens
(copying), of tokens with names and numbers normalised (copying with
renaming), and of numeric constants in order (tables and register
programs - what survives a translation from assembly to C).

**Result, 9 October 2026:** no PrismRTG code matches any of these
sources. The one finding was in `src/p96api.h`: its Picasso96 memory-window
(PIP) section, added with PR #10, had been copied from `Picasso96.h`
together with that header's comments, under a file header saying nothing
in it came from Picasso96. The section was rewritten in PrismRTG's own
words the same day; the tag names and numbers remain, being the interface
itself, and the file now says where they come from. Every other match the
tool reports is either `#include` and `#define` boilerplate shared by all
Amiga programs, or a published interface number that has to be the same.

The same day, every source file got copyright lines matching its git
history (Stefan Reinauer's contributions had carried only a licence tag).
