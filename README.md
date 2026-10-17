# AnyPS5 - portable one-folder setup

Everything stays in this one folder. Nothing is installed, and there are no PATH, registry or user-profile changes. To remove it completely, delete the folder.

## Requirements

- Windows 10 (version 1803 or newer) or Windows 11, 64-bit
- About 10 GB free space for the tools and the build, plus the size of each game you port
- A short folder path without spaces, for example `D:\AnyPS5`

**Before the first start:** these files came from the internet, so Windows may block them. Right-click each `.bat` file > **Properties** > tick **Unblock** > **OK**.

## 1. Build AnyPS5 (and later: update it)

Double-click `build-anyps5.bat`.

The first run downloads two portable tools from GitHub (about 330 MB) and checks each one against its official SHA-256 checksum:

- **WinLibs GCC 15.2.0 r7**, which includes CMake and Ninja. It is the exact compiler that AnyPS5's own Windows build uses:
  - <https://github.com/brechtsanders/winlibs_mingw/releases/download/15.2.0posix-14.0.0-ucrt-r7/winlibs-x86_64-posix-seh-gcc-15.2.0-mingw-w64ucrt-14.0.0-r7.zip>
  - SHA-256 `cb2fbad6162540cdf5e1facdce08d4dac359e8cf64f7f696a99274291763b815`
- **Portable Git 2.56.0.2, 64-bit:**
  - <https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.2/PortableGit-2.56.0.2-64-bit.7z.exe>
  - SHA-256 `075e158ef8e1f0ab80b347e245405d3eca735c2dc88fd8e032e137d0ca61f61b`

If the automatic download fails, download both files in your browser, put them into `tools\downloads\` and run the script again.

Then it downloads the newest AnyPS5 source and builds it. The first build takes a long time; later runs only rebuild what changed. Run it again whenever you want the newest AnyPS5. `bin\VERSION.txt` shows your version.

It also compiles `bin\port-helper.exe` from `port-helper.c` - the small program that does the work of `port-game.bat`. The build uses it in place of PowerShell too, so PowerShell (which writes cache files into AppData) is never started.

## 2. Port a game

Drag the game's dump folder onto `port-game.bat`. You can also drag:

- a folder that has the dump somewhere inside it
- the `eboot.bin` file
- the game folder and its patch folder together, or the folder that holds both. The patch files are copied over the game files.

Any extracted dump works:

- dumps made to run on a jailbroken PS5: their `eboot.bin` and modules are fake-signed (SELF files), and `port-game.bat` converts them to the plain ELF files AnyPS5 needs
- dumps that are already decrypted to plain ELF files
- modules in `sce_module\`, `sce_modules\` or `prx\`, and extra module folders, for example `Media\Modules` and `Media\Plugins` in Unity games
- patches with or without their own `eboot.bin`

The `fakelib\` folder (system files only for the console) is left out.

What cannot work:

- encrypted (retail) files. They can only be decrypted on a PS5, when the game is dumped. `port-game.bat` names any encrypted file it finds, and stops if `eboot.bin` is one of them.
- `.exfat` or `.pkg` images. Use the extracted folder instead (for an `.exfat` image: the folder you built it from).

Your dump is never changed: the game is copied to `ported\<folder name>\` and only that copy is converted. Running `port-game.bat` again for the same game updates the copy. After updating AnyPS5, port your games again. To start over cleanly, delete the game's folder in `ported` first.

**Only converting the files (without AnyPS5):** drag a dump folder, or single `eboot.bin` / `.prx` files, onto `convert-to-elf.bat`. Fake-signed files become plain ELF files in `converted\` - the dump itself is not changed.

## 3. Play

Double-click `Play.bat` in the game's folder, not the `.exe`. For a shortcut, point it at `Play.bat`. If the game stops with an error, the window stays open so you can read it.

AnyPS5 is very early: its official compatibility list currently has one game marked playable (Dreaming Sarah). Most games will not run yet. See `src\docs\user\COMPATIBILITY.md`

## Folder layout

```
AnyPS5\
  build-anyps5.bat  port-game.bat  convert-to-elf.bat  port-helper.c
  README.txt
  tools\    mingw64\ (compiler)  PortableGit\  home\ (Git's settings)
  src\      AnyPS5 source code
  build\    build files
  bin\      relinker.exe + libs\ (same files as an official AnyPS5 release)
            and port-helper.exe
  tmp\      temporary files
  converted\ results of convert-to-elf.bat
  ported\<game>\
    <game>.exe  Play.bat  libs\  app0\ (the game's files)
    tmp\                  the game's temporary files
    _sd\  _sd_mem\        saves, created when the game saves
    shader_cache\         compiled shaders, created when the game runs
    temp0\  download0\    only if the game uses them
```

## What can still end up outside this folder

Only what Windows and your graphics driver do for every program:

- the graphics driver's own shader cache (NVIDIA, AMD or Intel folder in `AppData\Local`)
- Windows bookkeeping such as Prefetch and Defender scan history
- a Windows Firewall rule, if a game uses the network and Windows asks whether to allow it: Windows remembers your answer as a rule

## Tips

- **Controls:** optional `anyps5-input.ini` next to the game's `.exe`, see `src\docs\user\INPUT_MAPPING.md`
- **Fonts:** some games need font files in `anyps5-fonts\` next to the `.exe`, see "System fonts" in `src\docs\user\USAGE.md`
- **Error "create direct memory backing":** the Windows page file is too small. Make it bigger or close other programs (see `USAGE.md`).
- If the PC runs out of memory during the build, open `build-anyps5.bat` in Notepad and change both `--parallel` to `--parallel 4`.
