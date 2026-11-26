Shadow of the Beast PC Port (Linux)
===================================
Version @VERSION@

The PS4 version of Shadow of the Beast running natively on Linux x86_64: the
game's own code runs on the PC with replacement PS4 system libraries and a
Vulkan renderer. It is not Proton or Wine. The Linux and Steam Deck build is
experimental: it is built and checked headlessly, but not yet tested in real
gameplay or on Steam Deck hardware.

Source code and issue tracker:
https://github.com/saulob/shadow-of-the-beast-pc


NO GAME FILES ARE INCLUDED
--------------------------
Bring your own legally obtained, decrypted PS4 package (.pkg) of one of these
builds:

  CUSA03762 1.00 (Korea)
  CUSA02871 1.00 (USA)
  CUSA00951 1.00 (Europe)

All three have the same port features. Any other build is refused, and the
log says what was found.


REQUIREMENTS
------------
- Linux x86_64 with glibc 2.31 or newer (Ubuntu 20.04, SteamOS 3 or newer)
- A graphics driver with Vulkan 1.3
- A processor with AVX
- zenity or kdialog, for the file dialog of Install game...
- About 17 GB of free disk space for the installed game


INSTALL
-------
1. Put the AppImage in a folder of its own. The game, saves, settings and
   logs are created beside it (or in ~/.local/share/ShadowOfTheBeastPC when
   that folder is read-only; SOTB_PORTABLE_DIR chooses another folder), never
   inside the AppImage.
2. Make it executable (chmod +x, or the file manager's Properties) and run
   it. FUSE is not required: without it the AppImage extracts itself to a
   temporary folder for that run.
3. Click "Install game..." on the Play page and select your .pkg file. When
   the Play page says the game is ready, press PLAY.

Start it with --launcher (or the desktop entry's "Show launcher" action) to
open the launcher when it is set to start straight into the game. Holding
Shift at start does the same under X11.


FILES
-----
  user/sotb.ini            launcher and game settings
  user/savedata/           saves, one folder per region
  user/trophy/, user/keys.json   trophy progress and your Trophy Key
  user/bug_reports.json    the Bug Reports you sent
  logs/                    one log per run; a crash is described there
  game/                    the installed game


UPDATES
-------
The launcher can replace this AppImage with a newer release when you choose
to. The new file is checked against its published SHA-256 first, the game,
saves, settings and logs are kept, and the old AppImage stays in place if
anything fails.


LICENSE
-------
The port's own code is licensed under the GNU General Public License v2.0 or
later. Third-party components keep their own licenses
(usr/share/licenses/ShadowOfTheBeastPC inside the AppImage). Game files are
not included and are not covered by this project's license. This port is not
affiliated with or endorsed by Sony Interactive Entertainment, Heavy Spectrum
Entertainment Labs or any other rights holder of Shadow of the Beast.
