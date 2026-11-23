<!--
SPDX-FileCopyrightText: 2026 Joaquim Serra
SPDX-License-Identifier: GPL-2.0-or-later
-->

<p align="center"><img src="banner.svg" alt="DOSCAD.DRV: variable resolution VESA display driver for the DOS OrCAD 386+ suite, written by Claude Opus 5.5" width="100%"></p>

# DOSCAD.DRV — variable resolution advanced VESA driver for OrCAD 386+

*Developed by Claude Opus 5.5 in high effort mode.*

> **Disclaimer.** This is an independent, unofficial project. It is not affiliated with,
> endorsed, sponsored or supported by Cadence Design Systems, Inc., the former OrCAD, Inc.,
> or any other company named here.
>
> - OrCAD is a registered trademark of Cadence Design Systems, Inc. VESA is a trademark of
>   the Video Electronics Standards Association. IBM, MS-DOS and Windows are trademarks of
>   their respective owners. All other names are the property of their owners. They are
>   used here only to say what this driver works with.
> - This package contains no OrCAD software, code, fonts or other data. The driver reads
>   the font from the user's own copy of `VGA640.DRV` at run time and never copies or
>   distributes it.
> - Running OrCAD with this driver requires a copy of OrCAD that you are licensed to use.
>   This package grants no rights to any OrCAD software.
> - The driver exists so that the OrCAD 386+ programs can keep working on today's emulated hardware
>   (interoperability). It does not change, patch or include the original programs.
> - It is provided "as is", without any warranty (see the licence at the end). Use it at
>   your own risk.

DOSCAD.DRV is a display driver for the 1993 DOS OrCAD 386+ suite (PCB386+, SDT386+ and the
ESP shell, run under DOSBox-X); it is tested with ESP, DRAFT and PCB386.
It replaces `VGA640.DRV` / `VESA800.DRV` / `VESA1024.DRV` and draws through any VESA 256-colour
mode the emulated card has, from 640×480 up to 4096×2160, with the same 16 colours and the
same 8×8 font as the original drivers (read at start-up from your own `VGA640.DRV`). Above 1080 lines it enlarges the whole picture by an
integer scale and smooths the font (Scale2x / Scale3x), so the text stays readable at 4K.

At 640×480 and 800×600 the picture is pixel-identical to `VGA640.DRV` and `VESA800.DRV`.

Package version: **1.2.2**; driver version: **1.2.0** (see the changelog at the end). The
start screen of PCB386 shows
`Variable resolution advanced VESA driver  v1.2.0  1920x1080 256 colours` with the mode that
was actually set.

### Package files

The package `doscad.drv.<version>.zip` (with its `.sha256` checksum) holds:

| File | What it is |
| --- | --- |
| `DOSCAD.DRV` | The driver, ready to use: the only file you need to install (section 1). |
| `README.md` | This document. |
| `COPYING` | The licence: GNU General Public License, version 2 (the driver is GPL-2.0-or-later). |
| `banner.svg` | The picture at the top of this document. |
| `build.sh` | Builds `DOSCAD.DRV` from the sources below (section 6). |
| `stub.asm` | Loader stub, the part of the file DOS loads: unpacks the driver image into an upper memory block and hands control to it. |
| `doscad.asm` | Real-mode part of the driver image: load routine, entry table and the thunks from each PCB386 driver entry into the C code. |
| `doscad.c` | Drawing code: the PCB386 display driver entries on a VESA 256-colour mode, the font loader, the scaling and the frame rate overlay. |
| `link.ld` | Linker script: lays out the flat 16-bit driver image (assembly first, then the C code and data). |
| `lzpack.py` | Compresses the driver image so that `DOSCAD.DRV` stays small; `stub.asm` unpacks it. |

The sources are only needed to rebuild the driver. `DOSCAD.DRV` unpacks itself in memory
when it loads, which some antivirus programs report as suspicious; the sources and
`build.sh` let you build the same file yourself.

## 1. Install

1. Copy `DOSCAD.DRV` into the OrCAD driver directory, next to `VGA640.DRV`. With the
   folders of section 2 (`~/orcad` mounted as `C:`) that is `~/orcad/orcadesp/drv/`,
   `C:\ORCADESP\DRV\DOSCAD.DRV` inside DOSBox-X.
   `VGA640.DRV` must stay there: the 8×8 font belongs to OrCAD, so it is not in this
   package, and the driver reads it from `VGA640.DRV` when PCB386 starts (see
   `DOSCADFONT` in section 3 for another place).
2. Select it, either:
   - with the `ORCADDD` variable (in the DOSBox-X `[autoexec]`, before PCB386 starts):
     `set orcaddd=c:\orcadesp\drv\doscad.drv`; or
   - in the project's `PCB.CFG`: `DD = 'DOSCAD.DRV'` (PCB386 reads `PCB.CFG` only when the
     project has no `PCB.BCF`).
   - In the ESP launcher, set the driver to `DOSCAD.DRV` in every ESP section and in
     every project that names its own driver (e.g. one set up for `svga_et4000`).
3. Configure DOSBox-X (section 2) and, if wanted, the driver (section 3).
4. Start PCB386 as usual. The banner must show the mode you asked for.

The driver needs DOS upper memory (`umb=true`, the DOSBox-X default): it unpacks itself
into an upper memory block. In conventional memory the board picture came out wrong.

## 2. DOSBox-X configuration

Tested with DOSBox-X 2026.01.02. The example below is a complete `dosbox-x.conf` for
the OrCAD suite; DOSBox-X takes its defaults for every setting it does not list. It
assumes these Linux folders:

```
~/orcad/orcadexe/     the OrCAD programs (ORCADEXE)
~/orcad/orcadesp/     the ESP launcher and its drivers (ORCADESP, DRV/DOSCAD.DRV)
~/orcad/designs/      your projects
```

`~/orcad` is mounted as `C:`. Drive `Z:` is DOSBox-X's own built-in drive: it exists
in every session without any setup and holds its DOS tools (`Z:\DOS\LOADFIX.COM`,
`MEM`, ...), so the configuration only mounts `C:`.

```ini
[sdl]
# Window or full screen; see "Full screen" below.
fullscreen=false
output=opengl
# Size of the window when the mode is smaller than the screen (optional).
windowresolution=1920x1080
# Click in the window to capture the mouse for OrCAD.
autolock=true

[dosbox]
# The DOSBox-X SVGA card with a VESA BIOS that lists every mode up to 4096x2160.
machine=svga_dosbox
memsize=64

[video]
# 16 MB of video memory: 4096x2160 at 8 bpp needs 8.4 MB, 3840x2160 8.3 MB.
vmemsize=16
# Without these the VESA BIOS hides the modes above 1280x1024 and the unusual ones
# (848x480, 1440x1080, 2880x2160, ...).
vesa modelist width limit=0
vesa modelist height limit=0
allow high definition vesa modes=true
allow unusual vesa modes=true

[render]
aspect=false
scaler=none

[cpu]
# The fastest emulation: PCB386 makes 13 000-45 000 driver calls per redraw.
core=dynamic
cycles=max

[mixer]
# OrCAD makes no sound.
nosound=true

[dos]
xms=true
ems=true
umb=true

[autoexec]
@echo off
# First, before anything else runs: keep the first 64 KB of memory in use, so the
# EXEPACK-compressed OrCAD utilities (ANNOTATE, ...) load above it.
z:\dos\loadfix -64
mount c ~/orcad
c:
path z:\;c:\orcadexe
set orcadesp=c:\orcadesp\
set orcadexe=c:\orcadexe\
set orcadproj=c:\designs\
set orcaddd=c:\orcadesp\drv\doscad.drv
set doscadvres=1920x1080
rem optional: set doscadvscale=2
rem optional: set doscadvfps=1
rem optional, keyboard layout, e.g. Portuguese: keyb po 850
```

#### On Windows

The same file works with DOSBox-X for Windows; only the `[autoexec]` changes, because the
host folders have Windows paths (DOSBox-X expands `~` only on Linux and macOS). With the
folders in `C:\orcad` (`C:\orcad\orcadexe`, `C:\orcad\orcadesp`, `C:\orcad\designs`):

```ini
[autoexec]
@echo off
z:\dos\loadfix -64
mount c C:\orcad
c:
path z:\;c:\orcadexe
set orcadesp=c:\orcadesp\
set orcadexe=c:\orcadexe\
set orcadproj=c:\designs\
set orcaddd=c:\orcadesp\drv\doscad.drv
set doscadvres=1920x1080
```

Inside DOSBox-X the paths are the same as on Linux, so the ESP and project settings do
not change between the two systems. If the host path has spaces, quote it:
`mount c "C:\My files\orcad"`.

What each setting does:

| Setting | Why |
| --- | --- |
| `machine=svga_dosbox` | Required (replaces `svga_et4000` if you used it). The other machines either have no VESA BIOS (`svga_et4000`, `vga`) or a list that stops at 1280×1024. `VGA640.DRV` also works on `svga_dosbox`, so one file serves both drivers. |
| `vmemsize=16` | Video memory for the largest modes. With less, the BIOS drops the modes that do not fit. |
| `vesa modelist width/height limit=0` | No limit on the mode list (the default hides the large modes). |
| `allow high definition vesa modes=true` | Lists 1920×1080, 2560×1600, 3840×2160, 4096×2160 and the like. |
| `allow unusual vesa modes=true` | Lists the modes with unusual sizes (848×480, 1440×1080, ...). |
| `core=dynamic` (or `auto`), `cycles=max` | Redraw speed; see section 5. |
| `umb=true` | The driver runs from an upper memory block. |
| `z:\dos\loadfix -64` | Programs compressed with Microsoft EXEPACK (some ESP utilities, such as some versions of `ANNOTATE`) stop with `Packed file is corrupt` when DOS loads them in the first 64 KB of memory, and DOSBox-X's small kernel leaves free memory starting there. DOSBox-X retries such a program with LOADFIX only when its own shell starts it (`autoloadfix`), not when the ESP menu does. This has nothing to do with the driver. |
| `aspect=false`, `scaler=none` | One emulated pixel per screen pixel: no blur and no extra cost. |

### Full screen at the monitor's resolution

```ini
[sdl]
fullscreen=true
fullresolution=desktop
output=opengl

[autoexec]
set doscadvres=3840x2160
```

Set `doscadvres` to the monitor's own resolution (or the nearest mode in the list of
section 3). Alt+Enter, or the host key + F (F12+F on Linux and macOS, F11+F on Windows),
switches between window and full screen. Use it when the bottom line of PCB386 does not
fit in the window.

## 3. Driver variables

All are DOS environment variables, read once when PCB386 opens the screen. Set them in
`[autoexec]` or at the DOS prompt before starting PCB386. Names and values are not
case-sensitive in DOS (`set` stores the name in capitals).

### `DOSCADVRES=WxH` — resolution

- `set doscadvres=1920x1080`: that mode.
- A mode the card does not have: the largest 256-colour mode no wider and no taller than
  the request (e.g. `2000x1100` gives 1920×1080).
- Not set, malformed, or nothing fits: the largest 256-colour mode (4096×2160 on
  `svga_dosbox`).

256-colour modes of `svga_dosbox` with the settings of section 2, from 640×480 up:

```
 640x480    720x480    800x600    848x480    960x720   1024x768
1152x864   1280x720   1280x800   1280x960   1280x1024  1400x1050
1440x900   1440x1080  1600x1200  1920x1080  1920x1200  1920x1440
2560x1600  2880x2160  3840x2160  4096x2160
```

### `DOSCADVSCALE=S` — interface scale

- `S` from 1 to 4: every PCB386 pixel becomes an S×S block. PCB386 sees a screen of
  (W/S)×(H/S) pixels, so S=2 at 3840×2160 shows the same area as 1920×1080, with the font
  smoothed instead of blocky.
- Not set or 0: 2 when the mode is at least 1080 lines high, 1 below (as the original
  drivers up to 1280×1024).
- S=1 at 1920×1080 or above gives the largest working area, but the 8×8 text is small.

| Mode | Default scale | PCB386 screen |
| --- | --- | --- |
| 1280×1024 | 1 | 1280×1024 |
| 1920×1080 | 2 | 960×540 |
| 2560×1600 | 2 | 1280×800 |
| 3840×2160 | 2 | 1920×1080 |
| 3840×2160, `doscadvscale=3` | 3 | 1280×720 |

### `DOSCADFONT=path` — where the font is

The driver reads the 8×8 font from OrCAD's `VGA640.DRV` (29 712 bytes, named
`IBM VGA or Compatible 640x480 16 color`; other files are refused). It looks, in order:

1. `DOSCADFONT`, the full DOS path of a `VGA640.DRV` (e.g.
   `set doscadfont=c:\orcadesp\drv\vga640.drv`); when set, only this path is tried;
2. `VGA640.DRV` in the directory of the driver named in `ORCADDD`;
3. `%ORCADESP%\DRV\VGA640.DRV`.

Not set: steps 2 and 3, which find it in a standard installation.

### `DOSCADVFPS=1` — frame rate overlay

- `set doscadvfps=1` writes `1920x1080 | 256 | 7.5 fps` at the bottom right corner, on
  the status line: the physical mode, the colours and the rate of the last redraw.
- A redraw is timed from PCB386's clear of the board area to the moment PCB386 puts the
  cursor back and waits for the user; the rate is 1 / that time. It measures how long a
  full board redraw (zoom, pan, refresh) takes, not a refresh rate of the monitor.
- Not set or 0: off (the default). The overlay costs under 3 % of a redraw.

## 4. Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| Message `DOSCAD.DRV: no VESA BIOS. In dosbox.conf set machine=svga_dosbox`, then PCB386 stops | The machine has no VESA BIOS: set `machine=svga_dosbox`. |
| Message `DOSCAD.DRV: cannot read the 8x8 font from OrCAD's VGA640.DRV.`, then PCB386 stops | `VGA640.DRV` is not next to the driver named in `orcaddd` nor in `%ORCADESP%\DRV`, or `doscadfont` names a wrong file: copy it there or set `doscadfont` (section 3). |
| DRAFT prints `ERROR: You MUST use a V4.20 or later video driver` and exits | `DOSCAD.DRV` older than 1.2.0: install 1.2.0 or later. |
| The banner shows a smaller mode than `doscadvres` | The card does not list it: check `vmemsize=16` and the four `vesa modelist` / `allow ... vesa modes` lines. |
| PCB386 starts with the old 640×480 picture | It loaded another driver: check `orcaddd` (or `DD` in `PCB.CFG`) and that the project has no `PCB.BCF` overriding it. |
| `Packed file is corrupt` when an ESP utility starts | Put `z:\dos\loadfix -64` first in `[autoexec]` (section 2). |
| ESP flashes `MSDOS Error #  6 Invalid Handle` at start; later Tools → Local Configuration says `Unable to redirect STDOUT to a file REPORT.TXT`, and DRAFT started from ESP ignores the keyboard | `EDITOR` in `ESP.CFG` names a file that does not exist (e.g. `'C:\util\ed.EXE'`). ESP's failed open of it closes the console handle by mistake, which then breaks ESP's redirection and the programs it starts. Point `EDITOR` at an editor that exists in the emulated machine, or leave it empty: `EDITOR = ''`. This is not a driver fault; it happens with `VGA640.DRV` too. |
| The bottom line of the screen is cut off | The window is taller than the desktop: switch to full screen (Alt+Enter or the host key + F) or use a smaller `doscadvres`. |
| Board picture garbled, menus right | The driver could not get an upper memory block: set `umb=true` and leave some UMB free. |
| Redraws slow | Use `core=dynamic` and `cycles=max`; a smaller mode or a larger scale draws faster (section 5). |
| Text too small | Raise `doscadvscale`. |
| Window larger than the desktop | Use a smaller `doscadvres`, or full screen with `fullresolution=desktop`. |

## 5. Speed

PCB386 draws a board with thousands of small driver calls (13 000–45 000 per redraw on the
test board), and each call crosses from the DOS extender's protected mode to real mode
and back. That crossing, not the pixel writes, sets most of the cost, so a larger mode is
slower mainly because more of the board is visible and more calls are made.

Zoom out / zoom in on the test board, `core=dynamic`, `cycles=max` (ms, lower is better):

| Driver and mode | Zoom out | Zoom in |
| --- | --- | --- |
| VGA640.DRV 640×480 | 128 | 95 |
| VESA1024.DRV 1024×768 | 260 | 170 |
| DOSCAD.DRV 640×480 | 101 | 80 |
| DOSCAD.DRV 800×600 | 129 | 91 |
| DOSCAD.DRV 1024×768 | 167 | 101 |
| DOSCAD.DRV 1920×1080, scale 2 | 224 | 182 |
| DOSCAD.DRV 1920×1080, scale 1 | 262–271 | 208–210 |

So DOSCAD.DRV is faster than the original drivers at the same mode. To speed it up
further: the fastest CPU core, `cycles=max`, a smaller mode, or a larger scale.

## 6. Build

On Linux (x86), with:

- `nasm`;
- `gcc` able to build 16-bit i386 code (`-m16`, i.e. with 32-bit x86 support);
- GNU binutils (`ld` with `elf_i386`, `objcopy`, `nm`);
- Python 3, and the usual `stat`, `awk`, `sed`, `grep`, `dirname`, `mkdir`, `mktemp`.

`build.sh` checks all of them first and names any that is missing.

The build needs no OrCAD file: the 8×8 font is read at run time (section 3, `DOSCADFONT`).

```sh
sh build.sh
# DOSCAD.DRV v1.2.0: 30112 bytes; image 61344 bytes -> /path/to/DOSCAD.DRV
```

Overrides: `DOSCAD_OUT` (build directory, default `build/`), `DOSCAD_DRV` (the
finished driver, default `DOSCAD.DRV` next to the script).

- `doscad.asm`: the load routine, the entry table and the thunks (real mode, nasm).
- `doscad.c`: the drawing code (gcc `-m16 -O2`, freestanding).
- `link.ld`: the flat image; `lzpack.py`: its compressor; `stub.asm`: the loader that
  unpacks it into upper memory (PCB386 loads drivers into a fixed 32 KB block).
- The version is `DOSCAD_VERSION` in `doscad.c`; `build.sh` prints it.

Trace build, for debugging: `DOSCAD_CFLAGS=-DDOSCAD_TRACE sh build.sh`.
It records every driver call (entry, arguments, start and end time, 16 bytes each) and
appends them to `C:\DOSCAD.TRC` whenever its buffer fills. It is
built with `-Os` to fit the 64 KB image.

## 7. Changelog

The version follows semantic versioning: major for a change that needs a new
configuration, minor for a new feature, patch for a fix.

### 1.2.2 — 2026-11-23

Package only; `DOSCAD.DRV` is unchanged and still reports v1.2.0.

- The package carries the licence text (`COPYING`) and the banner (`banner.svg`).
- Copyright holder named in every file: Joaquim Serra.
- Fix: the image size in the build example of section 6.

### 1.2.1 — 2026-11-23

Documentation only; `DOSCAD.DRV` is unchanged and still reports v1.2.0.

- Troubleshooting: ESP's `MSDOS Error #  6 Invalid Handle` at start, `Unable to redirect
  STDOUT to a file REPORT.TXT` and DRAFT ignoring the keyboard when started from ESP all
  come from an `EDITOR` in `ESP.CFG` that does not exist; leave it empty.

### 1.2.0 — 2026-11-23

- Works with the whole OrCAD 386+ suite: tested with ESP, DRAFT (SDT386+) and PCB386.
- Fix: the values the driver returns never reached the caller (the frame pointer was lost
  around the C code since 1.0.0), and each call overwrote the top 16 bytes of the caller's
  stack. PCB386 ignores the returned values; DRAFT checks one at start-up and refused the
  driver with `ERROR: You MUST use a V4.20 or later video driver`.
- DRAFT's colour probe: after an out-of-range colour, entry 7 returns 0xFF, as `VGA640.DRV`.
- The trace build writes `C:\DOSCAD.TRC`.
- The picture is unchanged (pixel-identical to `VGA640.DRV` and `VESA800.DRV` at 640×480 and
  800×600).

### 1.1.0 — 2026-11-23

- The 8×8 font is no longer built into `DOSCAD.DRV`: the driver reads it at start-up from
  the user's own `VGA640.DRV` (`DOSCADFONT`, else next to the `ORCADDD` driver, else
  `%ORCADESP%\DRV`), so the package carries no OrCAD data and can be published.
- Message and a key wait when the font cannot be read.
- `build.sh` needs no `VGA640.DRV`.
- The picture is unchanged (pixel-identical to 1.0.0 at 640×480 and 800×600).
- Trademark and no-affiliation disclaimer at the top of this README.

### 1.0.1 — 2026-11-23

Documentation only; `DOSCAD.DRV` is unchanged and still reports v1.0.0.

- Complete `dosbox-x.conf` example with the `~/orcad` folders, and its Windows variant.
- `z:\dos\loadfix -64` in `[autoexec]`, against `Packed file is corrupt` from the
  EXEPACK-compressed ESP utilities.
- Driver selection in every ESP section and project when moving from `svga_et4000`.
- Full screen with the host key + F; list of the package files.

### 1.0.0 — 2026-11-21

- Every VESA 256-colour mode from 640×480 to 4096×2160 (`DOSCADVRES`), nearest fitting
  mode when the requested one is missing.
- Integer scale 1–4 (`DOSCADVSCALE`), automatic 2 from 1080 lines up; Scale2x/Scale3x font.
- Frame rate overlay (`DOSCADVFPS=1`).
- Message and a key wait when the machine has no VESA BIOS.
- `-O2` build: about 11 % faster redraws than `-Os`.

## Licence

Copyright 2026 Joaquim Serra. GPL-2.0-or-later: you can redistribute and modify it under
the terms of the GNU General Public License as published by the Free Software Foundation,
either version 2 of the License or (at your option) any later version; the text is in
`COPYING`. This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

OrCAD is a registered trademark of Cadence Design Systems, Inc.; this project is not
affiliated with or endorsed by Cadence.
