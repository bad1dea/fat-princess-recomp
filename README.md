# Fat Princess, native port

A native port of Fat Princess (PS3, PSN release) for Windows, Linux and macOS, with an iOS build. The game's code is
translated ahead of time into C++ and compiled into a normal program, so there is no emulator running. Graphics go
through Vulkan (Metal on Apple devices, through MoltenVK).

It is not affiliated with or endorsed by Sony or the game's developers. The project contains no game code, data,
art, keys or firmware that you did not supply yourself.

## What works

- Linux and Windows: the game plays offline and online, with up to four local players.
- macOS: builds, not tested yet.
- iPhone: runs on an iPhone 14 Pro at 30 frames per second in most scenes.
- Online play goes through our own server and needs a free account (see below).

## What you need

- Your own copy of the Fat Princess package for the US PSN release (title NPUA80139): the game `.pkg` and its
  licence `.rap`. If you own the optional expansion, its `.pkg` and `.rap` go in too.
- A graphics card with Vulkan support.

## Setting it up

1. Download the zip for your system from the releases page and unpack it.
2. Make a folder called `import` next to the program and put the `.pkg` and `.rap` files in it.
3. Start the program. The first start unpacks the game (a few seconds for about 400 MB of files), then
   the game opens. Later starts go straight in.

If something is missing or does not match, the program says what, in a message box. Nothing is downloaded and your
files are only read. Once it has started, the `import` folder is no longer needed.

## Playing

- F1 opens the settings menu, F3 shows the frame rate.
- Arrow keys move, Enter is Start, Tab is Select, Z, X, A and S are the face buttons. Controllers work without setup.
- Settings are saved in `settings.ini` next to the program.

## Online play

Choose Play With Others. If you are not signed in, Register opens the sign-up page in your browser: pick a name and a
password, enter your email address and click the link in the message we send. Then come back to the game and sign
in. One account per email address. Cheats and the debug tools are switched off while online play is on.

## If something goes wrong

The log is not kept by default. To keep it for a bug report, add `keep_log=ON` to `settings.ini`, run the game again
and send the `play.log` it leaves. After a crash the game also sends the end of the log to the server you were
connected to.

## Folders

| | |
|---|---|
| `import/` | the package and licence files you supplied |
| `game/` | the unpacked game, made on first start. It is your copy, do not share this folder |
| `elf/` | the game executable, decrypted on first start |
| `data/` | save data and the game's file system, made on first run |
| `shader-cache/` | compiled shaders, so later starts are smooth |
