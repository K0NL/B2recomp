# B2recomp

*by [K0NL](https://github.com/K0NL)*

**B2recomp** (short for *Bayonetta 2 Recompiled*) is an unofficial static recompilation of *Bayonetta 2* (Nintendo
Switch) for Windows. It translates the Switch version's AArch64 program code to C **on your own PC**, from **your own**
legally dumped copy of the game, and runs it natively on Windows with a re-implemented system layer (threads, file
system, audio, input, and the NVN graphics API on Vulkan).

It is not an emulator (no CPU interpreter, no JIT) and it is **not a way to get the game**.

> **This repository contains no game code, no game assets, no Nintendo SDK code, no firmware, no system fonts and no
> encryption keys, and it does not tell you where to get any of them.** It cannot do anything without a copy of the
> game you dumped yourself from a console and a game you own. Everything derived from the game (the translated code,
> the compiled game DLL, icons, shaders, textures) is generated on your PC from your dump and must stay there. Please
> do not share it, and do not open issues asking for it.

## Status

Measured on the developer's PC (Windows 11, 16 threads, a Vulkan 1.3 GPU):

| What | Result |
|---|---|
| Title screen, attract sequence, new game | runs |
| Prologue (cutscenes, chapter card, first fights) | played through: 900 s, no crash, every system call the game makes handled |
| Chapter 1 with a controller (DualSense, Xbox, Switch Pro and other SDL3 pads) | played by hand past the first part |
| Standard test run (`scripts/bench.py`: tutorial and the first fight, scripted) | 59.5 fps (the display rate), 0 faults |
| Later chapters | being played and tested |
| Other PCs, GPUs, Windows 10 | not tested yet |

Expect bugs. Builds are compared with a scripted test run (`python scripts/bench.py`, see [BUILDING.md](BUILDING.md)).

## The game is never downloaded

There is no prebuilt game, no `game.dll`, no `game_native.exe` to download, and there never will be: the game is
translated and compiled **on your PC, from your files**. What you can download is this source code, either alone or as
the **builder package**: the same source plus the free tools the build needs, so you install nothing.

## What you need

- **Your own extracted copy of Bayonetta 2, version 1.2 (update v131072) applied** (the ExeFS and RomFS as plain files; see
  [BUILDING.md](BUILDING.md#what-you-extract-from-your-own-console)). The build checks that it is exactly that version
  and explains a mismatch. Encrypted packages (`.nsp`, `.xci`, `.nca`, ...) are refused: this project decrypts nothing
  and reads no keys.
- Windows 10/11 x64 and a GPU with a Vulkan 1.3 driver. A Linux version is in testing (the Linux builder on the
  **Releases** page, with its own `START HERE.txt`).
- **AMD Radeon RX 400 / 500 cards (Polaris):** AMD's Windows driver for these cards lacks Vulkan features that
  B2recomp's renderer normally uses (dynamic vertex input and pipeline libraries), and it offers no mailbox vsync.
  B2recomp works around that on Windows: the game runs (confirmed on an RX 560), but its first runs build more shaders
  while you play, so expect short hitches until they are cached, and vsync waits for the screen (FIFO). This is the
  driver, not your card: on **Linux** the Mesa driver (RADV) has all of these features, so the same card runs the full
  renderer there. If you have Linux on that PC, the Linux version is the better choice for these cards. (Their speed
  in fights is often set by the processor these PCs have: a 4-core CPU from around 2013 gives a steady 30 fps there.)
- With the builder package: nothing else. From the source alone: Python 3.10+ and the free tools listed in
  [BUILDING.md](BUILDING.md#what-you-need-on-your-pc).

## Getting started (step by step)

**1. Have your dump ready.** One folder that holds `exefs\` and `romfs\`, extracted from your own copy of the game
with update 1.2 applied (plain files, not an `.nsp` or `.xci`). Put it where it can stay: the game reads `romfs\`
from there every time it runs. Optional: `sysdata\fonts\` with your console's system font
([BUILDING.md](BUILDING.md#system-fonts)); without it a font of your PC stands in.

**2. Get the builder.** Download `B2recomp-builder-<version>.zip` from this repository's **Releases** page and unzip
it to a folder of its own, with about 1.5 GB free. It holds no game files: only this source, the compiler and the tools
the build needs, so you install nothing.

**3. Build the game.** Start **`B2recomp.exe`**. Click **Browse...** and choose your dump folder (or drag the folder
onto the window); it tells you at once whether the folder looks right. Press **Build** and wait, about 5 minutes on a
fast PC. If something is wrong, the window says what (for example the wrong game version); **Logs folder** has the
details.

**4. Set it up.** When the build is done, the window turns into the launcher. Good first settings:

| Setting | What to choose |
|---|---|
| Window mode | **Borderless window** or **Fullscreen** to fill the screen; Windowed with a size you like |
| Resolution scale | **1.5** for a 1080p screen, **2.0** for 1440p, **3.0** for 4K (strong graphics cards only) |
| Aspect ratio | **Keep 16:9** (black bars on an ultrawide screen) or **Stretch** |
| Frame rate (beta) | **60** is the game as made. Up to **120** for a high-refresh screen: experimental, some effects and menus may run faster |
| Preset | **Low** only for a weak or laptop graphics chip |
| Vertical sync, Input latency | leave them as they are |

Hover over any setting for a short explanation. Press **Save** (Play also saves).

**5. Choose your controls.**
- **Controller** (DualSense, DualShock, Xbox, Switch Pro and most others): connect it before you press Play. "Face
  buttons" decides whether A is the right button (as on a Switch) or the button printed A. **Test controller...**
  shows whether your pad is seen; **Remap controller...** fixes a pad whose buttons are mixed up.
- **PlayStation or Xbox button pictures** (optional): the game shows Switch buttons. Download *Controller Prompts
  Replacement (DS4/XBOX)* by Timo654 ([GameBanana 360121](https://gamebanana.com/mods/360121), the **Switch**
  version), unpack it and put its `DS4Prompts` and/or `XBOXPrompts` folder into the `mods\` folder beside
  `B2recomp.exe` (make the folder if it is not there). With **Button prompts** on Auto the game then shows the
  buttons of the controller you play with; or choose PlayStation or Xbox. It is chosen when the game starts.
- **Keyboard and mouse** work at the same time as a controller, with a layout like the original Bayonetta on PC:

  | Action | Keys |
  |---|---|
  | Move | W A S D |
  | Camera | mouse (or the arrow keys) |
  | Punch / Kick | left mouse / right mouse |
  | Jump | Space |
  | Shoot | E or middle mouse |
  | Dodge (and Witch Time) | left Shift |
  | Lock-on | left Alt |
  | Umbran Climax | Q |
  | Pause | Esc |
  | Menus: confirm / back | Enter / Backspace |

  **Keyboard and mouse...** changes any key: click a slot, then press the key or mouse button you want (Delete clears
  it). The same window sets the mouse sensitivity and can turn the mouse camera off. In game the cursor is hidden and
  kept in the window; Alt+Tab lets it go. **Input** (top of the controller panel) can lock the game to the keyboard
  and mouse only, or to the controller only.

**6. Play.** Press **Play**. The first start takes about 10 seconds longer, and the first time you see a new place
the game can hitch briefly while it prepares its graphics; this is saved, so it happens only once. Your saves are in
`build\play\userdata`.

**Next time:** start `B2recomp.exe` and press Play. Keep the dump folder where it was.

**Mods (optional).** Put mods made for the Switch version (emulator mod folders) into a folder `mods\` beside
`B2recomp.exe`, then click **Mods...** to switch them on or off. Texture, model and sound mods work at the next start.
Code mods (`.pchtxt`, `.ips`) need **Apply (rebuild)**: about a minute, with the game closed. Details:
[BUILDING.md](BUILDING.md#code-mods-pchtxt--ips).

**If something goes wrong:**
- Black window or a Vulkan error at start: update your graphics driver.
- Keys or the controller do nothing: click the game window first (input is read only while it is in front).
- The build says the version is wrong: extract again with update 1.2 (v131072) applied.
- "File in use" during a rebuild: close the game and press the button again.
- More: [BUILDING.md, Troubleshooting](BUILDING.md#troubleshooting).
- **To ask for help:** press **Report...** at the bottom of the launcher (or of the build screen). It makes
  `b2recomp-report.zip` in the B2recomp folder with the logs and your PC's details (Windows version, CPU, graphics card
  and driver), nothing of the game. Attach it to an issue with a few words on what happened.

## Building from the source

The builder package does all of this for you. To build from a clone instead, with the tools installed yourself:

```
git clone <URL of the B2recomp repository>
cd B2recomp
python scripts/setup.py --dump <your dump folder>
```

It is one command with resumable steps; about **5 minutes** on a 16-thread PC and 0.8 GB in `build/`. Then start
`build\launcher.exe` (the same launcher as `B2recomp.exe`; `scripts\play.ps1` starts the game from a console). Details,
options, measured times and troubleshooting: [BUILDING.md](BUILDING.md).

`targets/`, `build/` and `cache/` (and the generated files in `runtime/gen/`) hold material made from your copy of
the game. They are ignored by git: never commit, upload or share them.

## Credits and licence

- B2recomp is made by **K0NL**: the Bayonetta 2 port of the lifter and runtime, the launcher, the build (`setup.py`,
  llvm-mingw), the test run and the tools in `scripts/`.
- AI helped in reverse engineering the game, with Ghidra and the open-source project REA.
- Reverse-engineering tools, with thanks: [Ghidra](https://github.com/NationalSecurityAgency/ghidra) (NSA, Apache-2.0)
  with Adubbz's [Ghidra-Switch-Loader](https://github.com/Adubbz/Ghidra-Switch-Loader), and
  [REA](https://github.com/morluto/rea) by morluto (MIT). None of them is included in this repository.
- The lifter (`lifter/`) and the runtime (`runtime/`) started as copies of
  [ssbu-recomp](https://github.com/Denkishi/ssbu-recomp) by Denkishi (commit `3a96916`), licensed GPL-3.0-or-later;
  every change is marked `[Recomps#1]` and listed in `runtime/UPSTREAM.md` and `lifter/README.md`.
- Other open-source components and references: see [NOTICE.md](NOTICE.md).
- Copyright (C) 2026 K0NL, and the ssbu-recomp authors for the parts taken from it. This project as a whole is licensed
  under the **GNU General Public License, version 3 or (at your option) any later version**; see [LICENSE](LICENSE).
  The need for your own game dump is a technical requirement, not a licence term.

## Legal

B2recomp is not affiliated with, endorsed, sponsored or approved by Nintendo, PlatinumGames or SEGA.
*Bayonetta*, *Nintendo Switch* and related names are trademarks of their respective owners and are used here only to
say which software this project is compatible with. No copyrighted game data, Nintendo SDK code, firmware, system
data or console keys are included or linked, and this project does not describe how to obtain them. This is a
non-commercial project: no donations, paid builds or advertising.

Rights holders with a concern can open an issue on this repository's Issues page, or use GitHub's own takedown
process.
<!-- OPEN before publishing (notes/release-audit.md section 5): lawyer-reviewed wording of this section. -->
