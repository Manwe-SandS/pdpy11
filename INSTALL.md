# Installing PDPy11 and the Sublime Text support

PDPy11 is a single executable: there is nothing to install besides copying files. Python, Homebrew or pipx are not needed.

## What is in the archive

| File | Purpose |
|---|---|
| `pdpy11` (`pdpy11.exe` on Windows) | the assembler |
| `convert-to` (`convert-to.exe`) | optional converter of sources written for other assemblers (see `docs/CONVERT-TO.md`) |
| `docs/` | the language reference (`SYNTAX.md`), the converter guide (`CONVERT-TO.md`) and `MACROS.md` |
| `PDPy11.sublime-syntax` | syntax highlighting for Sublime Text |
| `PDPy11.sublime-color-scheme` | colour scheme |
| `pdpy11.sublime-build` | build system (Ctrl+B / Command+B) |

Pick the archive for your system from the [Releases](https://github.com/Manwe-SandS/pdpy11/releases/latest) page:

| System | Archive |
|---|---|
| Windows 64-bit | `pdpy11-<version>-windows-x64.zip` |
| Windows 32-bit | `pdpy11-<version>-windows-x86.zip` |
| macOS (Apple silicon and Intel) | `pdpy11-<version>-macos-universal.zip` |
| Linux x86-64 | `pdpy11-<version>-linux-x64.tar.gz` |
| Linux ARM64 | `pdpy11-<version>-linux-arm64.tar.gz` |

The file `SHA256SUMS` lists the checksums of all archives.

## Sublime Text

1. Download and install [Sublime Text](https://www.sublimetext.com/download). Start it once and close it.
2. In Sublime Text choose **Preferences / Browse Packages** and open the folder `User`:
   * macOS: `~/Library/Application Support/Sublime Text/Packages/User`
   * Windows: `%APPDATA%\Sublime Text\Packages\User`
   * Linux: `~/.config/sublime-text/Packages/User`
3. Copy `pdpy11` (or `pdpy11.exe`) and the three Sublime Text files into that folder.
   The compiler must keep exactly this name: `pdpy11` or `pdpy11.exe`.
4. **Windows:** nothing more to do. If SmartScreen shows a warning on the first start, click "More info" and
   "Run anyway". If you downloaded the archive with a browser you can also open the properties of `pdpy11.exe`
   and tick "Unblock".
5. **macOS:** remove the quarantine flag, then make the file executable:
   ```
   cd "$HOME/Library/Application Support/Sublime Text/Packages/User"
   xattr -d com.apple.quarantine pdpy11
   chmod +x pdpy11
   ```
6. **Linux:** `chmod +x pdpy11`
7. Open any `.mac` file in Sublime Text. Highlighting turns on by itself. Press **Command+B** (Windows and Linux:
   **Ctrl+B**): the program is assembled, the `.bin` and `.lst` files appear next to the source, and errors are
   listed in the panel below; click an error to jump to its line.

### Running the program in an emulator (optional)

To start an emulator after a successful build, open `pdpy11.sublime-build` in the `User` folder and:

* comment out the `"cmd"` line;
* uncomment one of the `"shell_cmd"` lines for your system;
* replace the emulator path in it with your own.

## Command line

Put `pdpy11` in any folder that is on your `PATH` (for example `/usr/local/bin`), or call it by its full path:

```
pdpy11 --implicit-bin --lst program.mac
```

Run `pdpy11 --help` for the list of options. The encoding of the sources (UTF-8, UTF-16, CP1251, CP866 or
KOI8-R) is detected automatically; use `--source-charset` to set it explicitly.

## Sound output

`make_wav "~speaker"`, `play_wav`, `play_turbo_wav` and the options `--play` and `--play-turbo` play the program
through the sound output. Windows needs nothing extra. macOS uses the built-in `afplay`. Linux needs `paplay`
(PulseAudio / PipeWire) or `aplay` (ALSA) to be installed.

## Building from source

```
make            # or: cc -O2 -std=c11 -o pdpy11 src/*.c -lm        (add -lwinmm on Windows/MinGW)
make test
```

With MSVC run `build.bat` from a Developer Command Prompt.
