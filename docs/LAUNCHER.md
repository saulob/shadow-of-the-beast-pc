# Launcher

The launcher is TRG Launcher 1.1.0 with three pages: **Play**, **Settings** and **About**.

## Play

- **Install game...** opens a file dialog for a PS4 `.pkg`.
- Before unpacking anything, the launcher reads `param.sfo` and checks TITLE_ID `CUSA03762`, the content ID, CATEGORY
  `gd`, APP_VER `01.00` and VERSION `01.01`.
- The package is unpacked into `game\prepared\CUSA03762-1.00` through a staging folder. The executable hash is checked
  before the staged tree is activated.
- The boot image (`boot.bin`, `content.bin`, `image.json`) is built into `game\image\CUSA03762-1.00`.
- Progress, cancellation and failures are reported on the page; the log has the detailed reason.
- **PLAY** is enabled only when the installed tree, executable hash and boot image all match the supported build.

The game package is only read. Reinstalling replaces the game folder and leaves `user\` and `logs\` untouched.

## Settings

Only settings that exist in this round are shown:

- **Show this launcher**: the launcher can be skipped on later starts. Hold Shift while starting the program to bring
  it back, or pass `--launcher`.

## About

Shows the port version, the supported build, the installed folders, and links to the repository and sponsor page.

## Files

| Path | Contents |
| --- | --- |
| `game\prepared\CUSA03762-1.00\` | installed game (unpacked from your package) |
| `game\image\CUSA03762-1.00\` | boot image built from the installed game |
| `user\sotb.ini` | launcher settings |
| `user\savedata\` | save data (created by the runtime when the game writes) |
| `logs\shadow_of_the_beast_NNN.log` | one log per run |

Nothing is written to the repository by a player installation. `game\`, `user\`, `logs\` and `build\` are ignored by
Git.

## Failure messages

An unsupported package is refused with a diagnostic block written to the log. The block lists the package's
`param.sfo`, the `eboot.bin` hash, the required files that were found or missing, and the supported build. This is
intentional: a different executable can have different code, and the port must not run it with the wrong expectations.
