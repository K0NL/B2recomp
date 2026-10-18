# Building B2recomp from a fresh clone

The repository holds only this project's own tools and runtime. Nothing that comes from the game or from the console's
software is in it, with two stated exceptions: a list of SDK function names and addresses (see "The SDK function
list") and the hashes and sizes of the supported version's files (see "The version check"). Everything else of that kind
(the translated C code of the game, the translated SDK functions, the generated headers and tables, the icon) is
**generated on your PC from your own dump**, into `targets/`, `build/` and `runtime/gen/` (all ignored by git).

```
git clone <URL of the B2recomp repository>
cd B2recomp
python scripts/setup.py --dump <your dump folder>
```

One command, ten resumable steps, about 5 minutes. When it ends it prints how to start the game.

## The builder package (nothing to install)

`B2recomp-builder-<version>.zip` holds this source tree, `B2recomp.exe` (the launcher, ready-made) and every tool the
build needs, in `deps\`: llvm-mingw trimmed to x86_64, a Python with `lz4` and Pillow, the SDL3 and Vulkan headers
and libraries, a built `ssbu_glslang.dll` and the Microsoft Visual C++ runtime files it needs. Licences and versions:
`THIRD-PARTY-NOTICES.md` and `licenses\` in the package. It holds nothing of the game.

1. Unzip it anywhere (about 400 MB unpacked; the build adds about 0.8 GB in `build\`).
2. Start `B2recomp.exe`. While the game is not built it shows the **Build** screen: choose your dump folder (below,
   "What you extract from your own console") with Browse or by dragging it onto the window; it says at once whether
   the folder looks right. Press Build: it runs `scripts\setup.py` with the bundled Python and tools only (any Python or
   compiler settings of your PC are ignored), ticks off the ten steps, shows the progress and setup's output, and
   when it is done the same window becomes the launcher. Close stops a build (finished steps are kept); "Logs folder"
   opens `build\logs`. `B2recomp.exe --build` shows the Build screen again, to rebuild.
3. Later: start `B2recomp.exe` (or `build\launcher.exe`); it opens straight into the launcher.

`Build B2recomp.bat` does the same build from a console (drag the dump folder onto it, or start it and paste the folder),
then opens the launcher.

Everything below applies to both ways; "What you need on your PC" is only for building from the source alone.
The package is made with `python scripts/make_package.py` (run from a checkout that has the tools; it refuses to finish
if any file of the game or of a local build would end up in it).

## What you need on your PC

| Requirement | Version tested | Where from |
|---|---|---|
| Windows 10/11 x64 | 11 | |
| A GPU with a Vulkan 1.3 driver (to run, not to build) | | your GPU vendor |
| Python | 3.13 (3.10 or newer should do) | https://www.python.org/downloads/ |
| Python packages | lz4, Pillow (unicorn for the self-test in step `check`) | `pip install lz4 Pillow unicorn` |
| llvm-mingw, the UCRT x86_64 build (`llvm-mingw-<version>-ucrt-x86_64.zip`): clang, lld and the mingw-w64 libraries; no Visual Studio needed | **20260922** (clang 23.1.2) | https://github.com/mstorsjo/llvm-mingw/releases |
| SDL3, development package for Visual C++ (`SDL3-devel-<version>-VC.zip`; its import library works with llvm-mingw) | 3.4.18 | https://github.com/libsdl-org/SDL/releases |
| Khronos Vulkan-Headers | vulkan-sdk-1.4.341.0 | https://github.com/KhronosGroup/Vulkan-Headers |
| The shader-compiler shim `ssbu_glslang.dll`, **one of**: (a) glslang's Windows x86_64 release package (`glslang-*-windows-x86_64-release.zip`, has `include/` and `lib/`) **plus** the Visual Studio Build Tools with the "C++ x64/x86 build tools", which build the DLL; (b) a `ssbu_glslang.dll` built before | glslang 16.6.0, MSVC 14.51 | https://github.com/KhronosGroup/glslang/releases, https://visualstudio.microsoft.com/downloads/ ("Build Tools") |
| Media Foundation (movies) | part of Windows | |

Unpack each one anywhere and tell the script where it is, in this order of precedence: the command-line option, else the
environment variable, else the folder under `deps/` in your clone (a copy, or a junction made with `mklink /J`; `deps/`
is ignored by git), else what an earlier run of `setup.py` found (remembered in `build/setup_state.json`, so the options
need to be given only once):

| Tool | Option | Environment variable | Folder | The folder must contain |
|---|---|---|---|---|
| llvm-mingw | `--llvm-mingw` | `LLVM_MINGW` | `deps/llvm-mingw` | `bin\x86_64-w64-mingw32-clang.exe` |
| SDL3 | `--sdl3` | `SDL3_ROOT` | `deps/SDL3` | `lib\x64\SDL3.lib` |
| Vulkan-Headers | `--vulkan-headers` | `VULKAN_HEADERS` | `deps/Vulkan-Headers` | `include\vulkan\vulkan.h` |
| glslang (option a) | `--glslang` | `GLSLANG_ROOT` | `deps/glslang` | `lib\glslang.lib` |
| a built `ssbu_glslang.dll` (option b) | `--glslang-dll` | `GLSLANG_DLL` | `deps/ssbu_glslang.dll` (the file) | |

The Visual Studio Build Tools are found by `vswhere.exe` (installed with them). They are needed only to build
`ssbu_glslang.dll` (option a); keep that DLL (it does not depend on the game) and later builds need no Visual Studio.

**Optional second toolchain**, kept for comparison: `--toolchain msvc` (or `TOOLCHAIN=msvc`) builds with LLVM/clang
9.0.0 (`--llvm`, `LLVM_ROOT` or `deps/llvm`, the folder with `bin\clang.exe`;
https://github.com/llvm/llvm-project/releases/tag/llvmorg-9.0.0) and the MSVC Build Tools' linker and libraries, into
`build/obj_msvc` and `build/out_msvc/`. llvm-mingw is the default: it compiles the game faster and the result used less
CPU time per frame in the test run.

## What you extract from your own console

The port handles **Bayonetta 2 version 1.2 (update v131072) applied** and nothing else (see "The version check"). From your own copy of
the game, with the dumping tools you prefer, running on a console you own, produce a folder of **already extracted,
decrypted** files:

```
<dump>/exefs/      the whole ExeFS: main, sdk, subsdk0, rtld, main.npdm        (update v131072 applied; plain NSO files)
<dump>/romfs/      the RomFS with update v131072 applied: data000.cpk ... data006.cpk, movie/, sound/
<dump>/sysdata/fonts/                     optional: your console's shared system fonts (see "System fonts" below)
<dump>/control/icon_AmericanEnglish.dat   optional: your copy's HOME-menu icon, for the window and launcher icon
```

`main`, `sdk` and `main.npdm` are required; `subsdk0` and `rtld` are checked when present. Without the icon the
programs get the default Windows icon.

### System fonts

The game loads the console's shared font `nintendo_udsg-r_std_003` at start. Put it in `<dump>/sysdata/fonts/` as a
plain TrueType file (`nintendo_udsg-r_std_003.ttf`) or in the form the console stores it (`nintendo_udsg-r_std_003.bfttf`,
decoded when the game starts). The other shared fonts (`nintendo_udsg-r_org_zh-cn_003`, `nintendo_udsg-r_ext_zh-cn_003`,
`nintendo_udjxh-db_zh-tw_003`, `nintendo_udsg-r_ko_003`, `nintendo_ext_003`, `nintendo_ext2_003`) are read the same way
if the game asks for them. Fonts added after the build go straight into `targets\sysdata\fonts\`, with no rebuild.

Without them, a font of your PC stands in, from `C:\Windows\Fonts`: Yu Gothic for the standard font (else Meiryo, Segoe UI
or Arial), Microsoft YaHei / JhengHei for Chinese and Malgun Gothic for Korean. The game runs; text drawn with that font
looks different from the console's, and symbols that only the console's fonts have are missing. The log says which font
was used (the `nn::pl:` lines in `build\play\stdout.txt`).

This project decrypts nothing and reads no keys. Encrypted packages (`.nsp`, `.xci`, `.nca`, `.nsz`, `.xcz`) anywhere
in the dump folder are refused. It contains no keys and no link to any, and it does not explain how to obtain them.
Nothing is copied out of the RomFS: `targets\romfs` becomes a junction to `<dump>\romfs` (12.6 GB stay where they are),
so keep the dump folder where it is after building.

## The version check

The translated code and the generated tables fit one exact build of `main` and `sdk`. Step `verify` compares your files
with SHA-256 hashes and sizes held in `scripts/setup.py` (and a list of the RomFS file names and sizes, compared as one
hash) before anything is converted, and says what is wrong: the base game without the update, another version, a
missing or wrong-size file (an incomplete extraction), or a modified file. `--accept-modified` continues at your own
risk after a warning about modified `subsdk0` / `rtld` / RomFS files (mods are not blocked, but unsupported); a
different `main` or `sdk` is always refused. This is a compatibility check of files you already have, not an ownership
check: the project never contacts any server and reads no ticket, key or certificate.

## The SDK function list

`lifter/sdklift.py` translates a few functions of your `sdk` (texture storage sizes, an allocator, parts of the C
library) to C, for the runtime to call. Which ones, and where they must be cut, is a hand-made list of names and
addresses in that module: `lifter/sdklift_roots.txt` (`lifter/sdklift_install.txt` names the imports whose host
version is replaced). It holds names and addresses only, no code. The lifted code (`runtime/gen/sdklift_gen.c`) is
generated from your `sdk` in step `tables`.

## The steps

| Step | What it does |
|---|---|
| `deps` | checks the Python packages and every tool above; compiles and runs a test program (runs every time: it also finds the tools for the later steps) |
| `verify` | the version check above; refuses encrypted packages |
| `convert` | copies your ExeFS to `targets/exefs/`, converts `main` and `sdk` (NSO) to `main.elf` / `sdk.elf` with `tools/extract/nso2elf.py` (LZ4 only; each segment's SHA-256 checked against the NSO header); makes `targets\romfs` and `targets\sysdata\fonts` junctions to the dump's folders |
| `mods` | optional: applies your own code mods from `mods\` to `main.elf` (below, "Code mods"); with no mods it changes nothing (runs every time, it is cheap) |
| `tables` | `runtime/gen/make_gen.py`: generates `runtime/gen/*` from your `sdk` and `main` (image layout, import list, lifted SDK functions, SDK constants, the graphics driver's device answers, self-test vectors) |
| `lift` | `lifter/recomp_eh.py`: translates your `main.elf` to C in `build/gen_eh` |
| `compile` | `scripts/compile_main.py`: compiles that C with clang -O2 into `build/obj` (functions over 5,000 statements are left unoptimised: clang 23 takes minutes on a few huge ones) |
| `icon` | optional: `build/bayonetta2.ico` from `<dump>/control/icon_AmericanEnglish.dat` (`scripts/make_icon_local.py`) |
| `link` | `scripts/build_runtime.py`: `build/game.dll` (the game), `build/game_native.exe` (the runtime), `SDL3.dll`, `ssbu_glslang.dll` (copied or built), `COPYING.MinGW-w64-runtime.txt`; `scripts/build_launcher.py`: `build/launcher.exe` |
| `check` | `lifter/test_ext.py`: the instructions this port added to the lifter, as found in your `main`, against the Unicorn emulator (skipped without `pip install unicorn`) |

`--status` shows what is done, `--from <step>` repeats a step and those after it, `--only <step>` runs one (plus
`deps`), `--jobs N` sets the number of compiler processes (default: the number of CPU threads, at most 24). A failed step is
reported with the tail of its log (`build/logs/<step>.log`); fix the cause and run the same command again. A new
`--dump` folder starts every step again. `compile` compiles only the C files that changed since its last run (the lift
rewrites only those) and keeps a record in `build/obj/_stamps.json`; a new compiler, other flags or a changed runtime
header compiles everything again. `python scripts/compile_main.py --force` compiles everything on purpose.

### Measured (clean clone, 16-thread PC, `--jobs 16`, nothing else running)

| Step | Time |
|---|---|
| deps, verify, convert, tables | 0.7 s, 0.4 s, 0.4 s, 1.3 s (verify reads about 210 MB of the RomFS: a few seconds from a cold disk cache) |
| lift (386 C files, 392 MB) | 12 s |
| compile (388 objects, 217 MB) | 4 min 28 s |
| icon | 0.2 s |
| link (`game.dll` 144 MB, 44 runtime files, `ssbu_glslang.dll` built with MSVC, launcher) | 10 s |
| check (71 instruction words, 0 failures) | 16 s |
| **Total** | **4 min 53 s** without `check` (5 min 9 s with it) |

Disk: `build/` is about 0.8 GB; `build/gen_eh` and `build/obj` (about 0.6 GB) can be deleted after a successful build
(keep `game.dll`); a later `compile` then needs `--from lift`. Shader and pipeline caches grow in `cache\vk\` as you play (see
"Shader cache"). A 4-core / 8-thread PC should take roughly two to three times as long (estimate, not measured). Verified end
to end: the build above ran the title and attract sequence for 60 s (exit code 0, every system call handled) and passed
the standard test run below.

## Code mods (.pchtxt / .ips)

Code mods made for Switch emulators (Yuzu, Ryujinx and their successors) patch the game's `main` program: frame-rate,
ultrawide and similar mods ship as IPSwitch text patches (`.pchtxt`) or IPS patches (`.ips`). B2recomp translates that
same program, so the build can take them too: put the files into a folder `mods\` beside `scripts\` (any layout below
it, so an emulator mod pack's `<mod name>\exefs\` folder can be copied in as it is) and run `setup.py` again. Step
`mods` patches `targets\exefs\main.elf` with them, so the translated game has them built in, and because their bytes
changed it runs `tables`, `lift`, `compile`, `link` and `check` again by itself. Only the translated files that a patch
changes are compiled again, so this takes about a minute (measured: lift 12 s, compile of the one changed file 7 s, then
link and self-test; the very first build, and the first one after this feature arrived, compile everything once).
Removing the files, or switching them off, and running `setup.py` again gives back the unmodified build, byte for byte.
`--mods <folder>` reads another folder (remembered). This project ships no patches, and none are tested with it: mods
are yours to find and use.

**On / off, as in an emulator.** The launcher's **Mods...** button lists every mod in `mods\` (the first folder below
`mods\` is the mod's name; a file lying directly in `mods\` is a mod of its own), the files inside each mod and the
blocks of each `.pchtxt` (titled by the `//` comment above their `@enabled` / `@disabled` line), each with a check box;
a mod with only RomFS files (below) is listed too. **Apply (rebuild)** saves the switches and runs `setup.py` in the
window, with its log; close the game first (the rebuild replaces `build\game.dll`). When only RomFS-only mods changed,
Apply just saves: the game reads them when it starts. A file marked `!` (in red) cannot be used, for example one written for another version of
the game: switch it off or remove it. The switches are kept in `config\mods.ini`, section `[mods]`, which you can also
edit by hand (`setup.py` reads it on every run):

```
[mods]
60 FPS = off
Ultrawide/exefs/main.pchtxt = off
Ultrawide/exefs/main.pchtxt :: Wider HUD = on
```

A key is a mod (its folder name; this also turns off the mod's `romfs\` files, below), a file (its path below `mods\`,
with `/`), or a block (`<file> :: <block title>`, or `<file> :: #<n>` for an untitled or repeated title). Anything not listed keeps its default: mods and files on, blocks as their `@enabled` / `@disabled` line says (`on`
also applies a block written under `@disabled`). A mod or file switched off is not read at all.

- Only patches for **your** `main` are taken: a `.pchtxt` names it with `@nsobid-f19b2e01e1794f933a9720c1404bb6f6`,
  an `.ips` file is named after it (`f19b2e01e1794f933a9720c1404bb6f6.ips`). A file for another version of the game
  or another program (`sdk`, `subsdk0`) stops the step with a message naming it: take it out of `mods\`.
- What is applied is what an emulator applies: in a `.pchtxt`, the blocks under `@enabled` (not `@disabled`), until
  `@stop`; `@flag offset_shift`, `@little-endian` / `@big-endian`, hex bytes or `"strings"`; offsets are counted from
  the start of the NSO header, as in the emulators. Several files may patch the same bytes; the one whose path sorts
  last wins (the log says so). To leave a file out, rename it (for example to `.pchtxt.off`).
- Every patch line applied is listed with its offset and the old and new bytes, on screen and in `build\logs\mods.log`.
- A code mod changes the game's program, nothing more. One that relies on an emulator setting, or that patches the RomFS,
  `sdk` or `subsdk0`, does nothing here or does not load. A patch can also break the game, as it can on an emulator.
  Report problems with mods removed first.
- `python scripts/pchtxt.py list mods targets\exefs\main` shows what would be applied without building.

## RomFS mods (textures, models, sounds) and button prompts

Asset mods made for the emulators ship a `romfs\` folder: files that replace the game's files at the same path, or add
new ones (the emulators call this LayeredFS). The game itself picks them up: at start it lists its RomFS and reads a loose
file in place of the copy inside its `data00x.cpk` archives. B2recomp does the same with no build step: each folder
directly under `mods\` that holds a `romfs\` folder (in it, or one folder further down, as an archive unpacked into a
folder of its own) is a mod, and the game sees its files the next time it starts. A mod can have both `exefs\` code
patches (above, built in by `setup.py`) and a `romfs\` folder.

- Where two mods have the same file, the one whose folder name sorts last wins.
- `config\mods.ini` turns a mod off without deleting it: under `[mods]`, a line `<folder name> = off`.
- The console window and the trace say at start which mods are active, how many of their files replace the game's
  (each one is listed in the trace, `SSBU_TRACE`), and each file the game then reads from a mod.
- `SSBU_MODS=<folder>` reads another folder, `SSBU_MODS=none` none (for tests).

**Button prompts.** A prompt mod (PlayStation or Xbox button pictures in place of the Switch ones) is used only when its
kind is chosen: "Button prompts" in the launcher, or `button_prompts` in `controllers.ini` `[general]`:
`auto` (the default: the kind of player 1's controller when the game starts, PlayStation pads and Xbox pads get their
own prompts, every other controller and the keyboard the Switch ones), `switch`, `playstation` or `xbox`. Install both
kinds side by side in `mods\`; which folder is which is guessed from its name (a word starting with `ds4`, `ds5`,
`dualshock`, `dualsense`, `playstation`, `ps4`, `ps5` / `xbox`, or the word `ps`; so the `DS4Prompts` and `XBOXPrompts`
folders of Timo654's *Controller Prompts Replacement (DS4/XBOX)*, GameBanana 360121, Switch version, need nothing
more), or set in `config\mods.ini`:

```
[prompts]
playstation = <folder name>
xbox = <folder name>
```

The prompts are chosen when the game starts; restart it after changing the controller or the setting. This project
ships no prompt pictures (or any other game file): the mods are yours to find. Mods made for the Switch version of the
game fit; Wii U mods do not.

## Running

```
build\launcher.exe
```

The launcher edits `config/graphics.ini` and `config/controllers.ini` (each option is explained in the file) and starts
the game (Play), the controller test (`--pad-test`) or the controller mapping (`--pad-map 1`); **Mods...** switches code
mods on and off and rebuilds the game with them ("Code mods" above). From a console,
`scripts\play.ps1` does what Play does: window and sound, no time limit, saves in `build\play\userdata`
(`-NewSave` starts from an empty one). Controllers are read through SDL3 (DualSense, Xbox, Switch Pro and others) while
the game window has focus.

**Shader cache.** The game's shaders are translated and compiled the first time each one is needed, so a scene seen for
the first time can stutter briefly. Everything made is kept in `cache\vk\` (beside `build\`): later starts reuse it and
load faster. It is made from your copy of the game, like `build\`: never share it. Delete the folder to start over (for
example after a new graphics driver). The `shader_prepare` option and the launcher's "Shader preparation" have no effect
for this game (they belong to the Smash port the runtime comes from).

Options for testing are environment variables (a list is in the comments at the top of the runtime sources), for
example `SSBU_SHOW=1` (show the window of a test exe), `SSBU_TRACE=<file>`, `SSBU_SHOT=<frame>` (save a frame).
`scripts/run_boot.ps1 <name> [seconds]` runs the game with a trace into `build\<name>\`.

## The standard test run

`python scripts/bench.py --name <run>` plays a scripted route (`tests/route_first_fight.txt`: button presses only) from
a save made right after the prologue, through the first chapter's tutorial into the first fight, where Bayonetta only
dodges; 200 s, hidden and muted, about 3.5 minutes per run. It reports fps, 1 % low, frames over 20 ms, CPU time per
frame of the game's threads and of the render thread, faults, whether the route stayed on course, and compares them
with `tests/bench_baseline.json` (numbers only) as PASS / BETTER / WORSE. Before the first use, copy your own save
folder from just after the prologue (the game's save data, from `build\play\userdata` after you played the prologue)
to `build\saves\after-prologue`.

To compare two builds use `--ab`: `python scripts/bench.py --name <batch> --runs 3 --ab <exe B>` runs the current
`build\game_native.exe` and exe B alternately and judges B against A with that batch's own spread (a PC drifts by
several per cent between batches an hour apart, more than most changes). Build B beside the real one with
`python scripts/build_runtime.py --out=<folder>` (it reuses `build/obj`). The runner refuses to start while another
game or a compiler is running (`--wait` waits instead); measure on a quiet machine.

## Troubleshooting

- `missing Python packages: pip install ...`: install what it names, with the same Python that runs `setup.py`.
- `llvm-mingw not found` / `SDL3 ... not found` / `Vulkan-Headers not found`: give the folder with the option or the
  environment variable, or put it under `deps/`; the message names the file the folder must contain (for example
  `bin\x86_64-w64-mingw32-clang.exe`). A folder one level too high or too low is the usual cause.
- `the compiler cannot build and run a Windows program`: the llvm-mingw folder is incomplete, or it is not the UCRT
  x86_64 build.
- `ssbu_glslang.dll (the shader compiler shim) has no source`: give a built DLL (`--glslang-dll`), or glslang plus the
  Visual Studio Build Tools. `glslang is there, but the MSVC Build Tools ... are not`: install "Build Tools" with the
  "MSVC C++ x64/x86 build tools" component and a Windows SDK, or give a built DLL.
- `encrypted packages are not accepted`: the dump folder holds an `.nsp`, `.xci`, `.nca` or similar file. Give the
  extracted ExeFS and RomFS folders only.
- `exefs\main is the base game (v0) without the update to version 1.2` / `a different version`: extract again with
  version 1.2 (update v131072) applied.
- `exefs\main is not a plain NSO file`: the file is still encrypted or is not an ExeFS file.
- `romfs: ... files are missing`: the RomFS extraction is incomplete, or the update was not applied.
- `NSO segment hash mismatch`: the `main` or `sdk` file is corrupt or modified.
- The link step fails with a file in use: a running `game_native.exe` or `launcher.exe` holds `game.dll` open; close
  the game first and run the same command again.
- The game window stays black or the game stops at start with a Vulkan error: update the graphics driver (Vulkan 1.3
  is needed); `renderer = opengl` in `config/graphics.ini` is the older fallback renderer (NVIDIA only). A driver
  without `VK_EXT_vertex_input_dynamic_state` (AMD RX 400/500 on Windows) takes a slower start-up path: the log says
  `NO VK_EXT_vertex_input_dynamic_state`, and its first runs build more pipelines while playing.
- A compiler job that fails while the PC is under heavy load or after a power loss: run the same command again.
- Output with non-ASCII characters from Windows tools is shown with `?`; the logs in `build/logs` are the originals.
