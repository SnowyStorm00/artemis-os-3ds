# Artemis OS 3DS

A tiny Windows 95-style desktop shell running as 3DS homebrew.

[![platform](https://img.shields.io/badge/platform-3DS-blue)
[![Ko-fi](https://img.shields.io/badge/support_me_on_ko--fi-F16061?style=for-the-badge&logo=kofi&logoColor=f5f5f5)](https://ko-fi.com/snowystorm)

## Features

- Draggable windows with title bars, close buttons, z-order and focus
- Taskbar with a Start menu, live clock, and battery indicator
- On-screen QWERTY keyboard and a trackpad on the bottom touch screen
- Classic-style synthesized sound effects
- Apps:
  - **Notes** - text editor, saves to the SD card
  - **Paint** - 192x128 indexed-color canvas with pencil, brush, eraser, fill, and line tools. Draw with the stylus directly on the bottom screen in touch-draw mode.
  - **Calculator**
  - **Minesweeper** - 9x9 classic board
  - **File Explorer** - browses `sdmc:/3ds/artemis/`
  - **About**

## Controls

| Input | Action |
| --- | --- |
| Stylus on trackpad | Move cursor, tap to click |
| Stylus on keyboard | Type into focused field |
| A or L | Click at cursor |
| R | Right-click (flags Minesweeper cells) |
| B | Close focused window |
| D-pad / Circle pad | Nudge cursor |
| START | Toggle Start menu |
| SELECT | Quit |

## Building

Requires [devkitPro](https://devkitpro.org) with `devkitARM` and `libctru` installed.

```
make
```

The resulting `artemis-os-3ds.3dsx` can be copied to `sdmc:/3ds/` on your 3DS and launched from the Homebrew Launcher, or opened directly in [Azahar](https://github.com/azahar-emu/azahar) / Citra.

## Audio

Audio needs the DSP firmware (`dspfirm.cdc`) which Nintendo's copyright prevents shipping. Run [DSP1](https://github.com/zoogie/DSP1/releases) once to dump it from your console, then Artemis OS (and all other homebrew) will play sound. The About window will tell you if audio isn't initialized.

## File layout

Saves and bundled data live under `sdmc:/3ds/artemis/`:

- `*.txt` - Notes documents
- `*.apt` - Paint images (custom 4bpp format)

## License

MIT.
