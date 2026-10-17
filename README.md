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
- Windows 10/11 x64 and a GPU with a Vulkan 1.3 driver.
- With the builder package: nothing else. From the source alone: Python 3.10+ and the free tools listed in
  [BUILDING.md](BUILDING.md#what-you-need-on-your-pc).

## Build

**With the builder package** (`B2recomp-builder-<version>.zip`, from this repository's Releases page): unzip it and
start **`B2recomp.exe`**. Choose your dump folder (Browse, or drag it onto the window) and press **Build**; the window
shows each step and the progress, and when the build is done it turns into the launcher. (`Build B2recomp.bat` does
the same from a console.) The package holds no game files: only this source, the compiler (llvm-mingw), Python, SDL3,
the Vulkan headers, the shader-compiler shim and their licences (`THIRD-PARTY-NOTICES.md`).

**From the source**, with the tools installed yourself:

```
git clone <URL of the B2recomp repository>
cd B2recomp
python scripts/setup.py --dump <your dump folder>
```

Either way it is one command with resumable steps; about **5 minutes** on a 16-thread PC and 0.8 GB in `build/`.
Details, options, measured times and troubleshooting: [BUILDING.md](BUILDING.md).

`targets/` and `build/` (and the generated files in `runtime/gen/`) hold material made from your copy of the game.
They are ignored by git: never commit, upload or share them.

## Play

```
B2recomp.exe          (builder package)
build\launcher.exe    (either way)
```

The launcher has the settings (`config/graphics.ini`, `config/controllers.ini`: resolution scale, filtering, frame
pacing, controller mapping) and a Play button. `scripts\play.ps1` starts the game the same way from a console. Saves go
to `build\play\userdata`.

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
