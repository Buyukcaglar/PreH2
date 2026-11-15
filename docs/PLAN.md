# Prehistorik 2 — reconstruction plan

Goal: rebuild the DOS game *Prehistorik 2* (Titus, 1993) as a native program, first for
Windows x64, later for macOS (built on a separate Mac). Version **v1.0** of the
reconstruction; **v1.1** adds the QoL features (section 7). The Windows program is `Prehistorik2.exe`. Private repository **Prehistorik2**, public repository **PreH2**.
The original game files are never committed. The player supplies them, and the program reads
them at run time.

## 1. What the original is (established 2026-11-12)

| Item | Finding |
|---|---|
| Launcher | `TITUS.BAT` → `CDRUN.COM PRE2.SQZ` ("Protection kit-B 1993 by E.ZMIRO"). CDRUN unpacks and starts the program. |
| Main program | `PREH2/PRE2.SQZ`: Titus SQZ, LZW with CLEAR=0x101/END=0x100 (CDRUN's swapped codes) → MZ EXE, 87,452 bytes |
| Toolchain | Borland TLINK signature (`01 00 FB 20 'rj'`); the code is "100% MACHINE LANGUAGE by ERIC ZMIRO 1992", hand-written real-mode assembly |
| Layout | image 0x1519C bytes; stack seg 0000 (SP 0x200); **code seg 0020** (0x9DF0 bytes, also holds variables); **data seg 09FF** (~45 KB); heap from 0A00+ |
| Entry | 0020:0008. Switches `/F /M /A /E /X /L /D /B /V` set bits in `cs:[1..3]` |
| Hardware | EGA/VGA mode 0Dh (also 12h, 13h, text 03h) with line-compare split screen and pel panning; PIT 40h/43h (game timer at divisor 0x8E = 8403 Hz); PIC; keyboard 60h; PC speaker 42h/61h; Sound Blaster at 210h–260h, DMA ch.1, IRQ 2/3/5/7 probed, 8403 Hz 8-bit blocks; AdLib probe at 388h; EMS (int 67h) and XMS (int 2Fh 4310h) are optional, and the port reports them absent |
| Hooked vectors | int 24h → 0FC0h, int 08h → 06F6h (later 075Ch), int 09h → 1823h, int 06h → 1C4Bh (CPU check). The vectors are swapped through `cs:[027Ah]` (17A4h); that is not self-modifying code |
| Self-modifying code | 1 site: 65E3 writes IRET over 26FA (turns a handler off) |
| Data files | `*.SQZ` either Titus SQZ (LZW 0x10 or Huffman+RLE 0x00) or DIET-packed (`B4 4C CD 21 9D 89 'dlz'`); music `*.TRK` (DIET-packed modules) |

## 2. Approach

The source is hand-written assembly, with no C to recover. The reconstruction runs in three
stages. Every stage ends with a program that runs.

1. **Disassembly** (`tools/p2dis.py`, hints in `tools/p2hints.py`): recursive descent from
   the entry point, the interrupt handlers and the jump tables, until every byte of segment 0020
   is classified as code or data. Then names, comments and data-structure maps.
2. **Static recompilation to C** (`tools/p2rec.py` → `src/gen/*.c`). Each routine becomes a C
   function that operates on an explicit 8086 state (registers, flags, a flat 1 MB memory
   image) with the original control flow. DOS, BIOS and hardware (`int`, `in`, `out`) go
   to a platform layer (`src/hal`). That layer emulates VGA mode 13h/X planes, the keyboard
   and timer IRQs, and Sound Blaster/AdLib/speaker output. The timer interrupt runs at defined
   yield points (busy-wait loops on the tick counter, the vertical-retrace waits). This is
   the first point of **full fidelity**.
3. **Lifting**: replace generated routines one at a time with readable C (named structures,
   no register state). An equivalence harness compares each lifted routine with the generated
   one on recorded inputs. Widescreen and the other QoL changes go into lifted code, never into
   generated code.

## 3. Host (Windows x64, then macOS)

- Win32 window with a presentation layer (4:3 aspect, integer and smooth scaling, full screen
  with Alt+Enter or F11) and audio through waveOut (44.1 kHz stereo, about 70 ms cushion).
- **The window follows the OS theme**: dark or light title bar (DWM immersive dark mode),
  menus through uxtheme, and a live switch on `WM_SETTINGCHANGE "ImmersiveColorSet"`.
  The code is the same as Civ64's (`Civ1RE/claude/port/platform/win32/host_win32.c`).
- **System menu** (v1.0): Full screen, Display ▸, Mute sound, Pause when inactive, Restart game,
  Locate game files…, About…, Close. The About box matches
  Civ64's: icon, title "Prehistorik 2 v1.0", repository link, and the MIT licence of the port's
  own code with a note that the game's program and data belong to their owners. It follows
  the theme and owner-draws the OK button in dark mode.
- VERSIONINFO 1.0.0.0, a per-monitor DPI v2 manifest and our own icon (no game art).
- macOS later: Cocoa host with the same core, appearance through NSApp's effective
  appearance, and the About panel through `orderFrontStandardAboutPanelWithOptions`.

## 4. Milestones

| # | Milestone | Done when | Status |
|---|---|---|---|
| M0 | Tooling: unpackers (SQZ, DIET), disassembler | every data file unpacks; segment 0020 classified | ✔ 99.3 % of the segment reached as code |
| M1 | Annotated disassembly | every routine named; memory map of DS and the CS variables | partly: entry points, handlers, jump tables |
| M2 | Recompiler + HAL, headless | title sequence renders to PNG frames in a test | ✔ `p2test`; recompiled = interpreted on 9,000 frames |
| M3 | Win64 host v1.0 | the game is playable; theme and About done | ✔ 2026-11-13 |
| M4 | Lifting | renderer, input, sound and level code in readable C | next |
| M5 | QoL | widescreen, configurable keys, … | ✔ 2026-11-16 (v1.1, section 7) |
| M6 | macOS port | on the Mac machine | next |

## 5. Rules

- The tools under `tools/` are our own code, written from public format descriptions
  (ModdingWiki "Titus Interactive SQZ Compression") and from the game's own routines.
  No code is copied from other re-implementations.
- `_references/`, `work/` and `build/` are ignored by git.

## 6. As built (v1.0, Windows)

- Stage 2 is done at block level: `tools/p2rec.py` turns every basic block into a C function
  that uses the interpreter's flag helpers, so the generated code and `src/core/cpu.c` agree
  bit for bit. Blocks end at every control transfer and after `sti`/`popf`/`out`/`in`/`int`,
  so interrupts arrive at the same points as in the interpreter. Unreached code, the 386
  probe and the one self-modified site (26FAh) run in the interpreter.
- Time is virtual (24 M instructions/s, 70.086 Hz frames). The host syncs to real time once a
  frame. Polling the VGA status port fast-forwards to the next event.
- Remaining for later milestones: lifting (M4), macOS (M6).

## 7. QoL (v1.1, Windows)

Section 2 planned to put widescreen into lifted code. It was done without lifting instead, beside
the unchanged program, so the game's behaviour stays that of the original.

- **Widescreen** (`src/qol/wide.c`): 384, 428 or 560 pixels in place of 320. The game keeps a
  tile buffer exactly 20 tiles wide, so the extra columns cannot be read from video memory. At a
  fixed point of every frame (background composed, sprites not yet drawn; also in the death,
  passage, level-start and curtain loops) the module copies the machine (CPU, 640 KB, VGA),
  shifts the camera by whole tiles, mirrors and rotates the backdrop picture, and calls the
  game's own routines (tiles, backdrop tiles, compose, sparks, sprites, foreground tiles, insects,
  weather) in a sandbox: no interrupts, no ports other than the VGA's, a trap as the return
  address. It keeps the new columns for the page that was drawn and restores the machine. The
  frame is put together when the page is shown. The status bar stays in the middle; its ends are
  continued with its plain edge columns. The program's bytes at the hook points are checked at
  load, so another game version turns the feature off instead of misbehaving.
- **Save states** (`src/hal/state.c`): each module has one `*_state()` routine for both
  directions (CPU and memory, VGA, Sound Blaster, PIC/PIT/keyboard, open DOS files, the widescreen
  margins). The file carries a hash of the program image. A damaged file leaves the running game
  as it was. F5/F7, four slots.
- **Host**: fast forward (Tab, 4×), Pause key, screenshots (F12), volume, window sizes 2×–6×,
  window placement kept, key bindings (`[Keys]` in the INI, the Keys… box).
- **Checks**: with and without widescreen the game executes the same instructions on every
  frame (8,000 frames); a run continued from a state gives the same pictures and instruction
  counts as the original run (1,000 frames, at each width); recompiled and interpreted runs give
  the same widescreen pictures (3,200 frames).
- Not done: cheats; widescreen for the full-screen pictures (menu, map), which are 320 wide by
  nature; key bindings for the gamepad.
