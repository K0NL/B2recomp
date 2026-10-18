B2recomp for Linux - START HERE
================================

B2recomp turns YOUR OWN copy of Bayonetta 2 (Nintendo Switch, version 1.2) into a native Linux game.
This folder holds only the tools: the game is built on your PC from your own dump. Nothing has to be installed.


What you need
-------------
- A 64-bit Linux PC or a Steam Deck (Ubuntu 22.04 or newer, Fedora, Arch, SteamOS 3 and the like)
- A graphics driver with Vulkan 1.3: Mesa for AMD, Intel and the Steam Deck, or NVIDIA's own driver
- Your dump of Bayonetta 2 version 1.2, extracted into one folder that holds:
      exefs/   main, sdk, subsdk0, rtld, main.npdm
      romfs/   data000.cpk ... data006.cpk and the movie/ and sound/ folders
- About 2 GB of free space for the build (your romfs stays where it is)


Getting started
---------------
1. Unpack this zip where you want to keep the game, for example ~/Games/B2recomp.
   The game is built inside this folder, so leave it there afterwards.

2. Start B2recomp: double-click "B2recomp" in the folder (choose "Run" or "Execute" if asked),
   or in a terminal:
       cd ~/Games/B2recomp
       ./B2recomp
   If it says "permission denied", start it with:   sh ./B2recomp

3. The first start shows the build screen. Choose your dump folder with Browse..., then press BUILD.
   B2recomp checks that the dump is the right version, then translates and compiles the game.
   This takes about 5 to 20 minutes, depending on your CPU (older 4-core processors take the longest).

4. When the build is done, the same window becomes the launcher: graphics and controller settings,
   and the PLAY button.

From now on you never need the terminal or this folder to play: the first start put B2recomp into your app
menu (under Games, with the game's icon after the build). Open it from your app launcher like any other app.
(./B2recomp --remove-from-menu takes it out again, ./B2recomp --add-to-menu puts it back.)


Playing
-------
- Open B2recomp from your app menu, then PLAY starts the game.
- Settings live in config/ (graphics.ini, controllers.ini); the launcher edits them for you.
- Your saves are in build-linux/play/userdata/
- To play without the launcher, run scripts/play.sh

AMD Radeon RX 400 / 500 cards (Polaris)
- These cards run the full renderer on Linux. On Windows, AMD's driver for them lacks Vulkan features B2recomp
  normally uses (dynamic vertex input, pipeline libraries, mailbox vsync), so the Windows version has to take a slower
  path there (more shader building while you play, short hitches). The Mesa driver on Linux (RADV) has all of them:
  just keep Mesa up to date (Arch: vulkan-radeon; Ubuntu / Debian / Fedora: mesa-vulkan-drivers).
- In fights their speed is often set by the processor of these PCs: a 4-core CPU from around 2013 gives a steady
  30 fps there (menus and cutscenes stay at 60).

Steam Deck
- Do steps 1 to 4 in Desktop Mode.
- To play in Game Mode: in Desktop Mode open Steam, choose Games > "Add a Non-Steam Game to My Library...",
  press Browse..., pick scripts/play.sh in this folder, then "Add Selected Programs".
  Back in Game Mode it is in your library; the Deck's controls work as a controller.


Updating to a new version
-------------------------
1. Close the game and B2recomp.
2. Unpack the new zip over this folder and let it replace the files.
   Your settings (config/), your saves (build-linux/play/userdata/) and your build stay.
3. Start B2recomp as usual. It sees the new version and shows the build screen: press BUILD.
   Only what changed is made again, usually a minute or two. Then press PLAY.


If something goes wrong
-----------------------
- Build problems: press "Logs folder" in the build screen (build/logs/ has one log per step).
  You can press BUILD again after fixing the cause; finished steps are kept.
- Game problems: the game's log is build-linux/play/stdout.txt
- To see the build screen again later: ./B2recomp --build

- If the game stops with an error, the launcher says why and offers to make a report.
- Before PLAY the launcher checks for a Vulkan driver and tells you what to install if there is none.

To ask for help, make a report: press "Report..." at the bottom of the launcher (or of the build screen),
or open a terminal in this folder (right-click > "Open Terminal Here") and run
       sh scripts/report.sh
It makes b2recomp-report.tar.gz in this folder, with the logs and this PC's details (Linux version, desktop,
CPU, graphics driver) and nothing of the game. Send that file, and say what happened and when.
