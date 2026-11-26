# Release security checks

A VirusTotal scan counts only for the exact file that was scanned. **Any rebuild invalidates every earlier VirusTotal
result**: if the ZIP or the AppImage changes by one byte, all four files must be scanned again. VirusTotal is
an additional verification signal, not an absolute guarantee of safety.

## Files to scan

Four files are scanned, each on its own, as in the other ports: the two downloads and the two Windows programs inside
the ZIP. Uploads are manual: no script uploads, publishes or tags anything.

| File | Why |
| --- | --- |
| `dist\ShadowOfTheBeastPC-v<version>-windows-x64.zip` | What Windows players download |
| `dist\ShadowOfTheBeastPC-v<version>-x86_64.AppImage` | What Linux players download |
| `dist\ShadowOfTheBeastPC-v<version>-windows-x64\shadow_of_the_beast.exe` | The program Windows players open: launcher and game |
| `dist\ShadowOfTheBeastPC-v<version>-windows-x64\trg-updater.exe` | The update helper that installs a newer release |

The two Windows programs come from the staged folder the ZIP is made from: `tools\package-release.ps1` checks that every
file in the ZIP is byte-identical to the staged one. During the full run, the Linux program is also read out of the final
AppImage with `unsquashfs` into a temporary folder under `build\release\` (the AppImage is never run) and checked
against the program the packager staged. That is an internal check: the Linux program is not a VirusTotal item and not
a release file.

Not scanned: the `.sha256` files, the release notes and manifests, the debug symbols (`dist\<name>-symbols\`, never
released), sources, game files and PKGs. The DLLs and libraries in the packages are third-party files copied unchanged;
scan them separately only if an engine names one.

## Steps

1. Run `.\tools\package-release.ps1`. It ends with **ARTIFACTS READY FOR VIRUSTOTAL** and the SHA-256 of the four
   files. It writes `dist\release-notes-v<version>.md`, `dist\virustotal-v<version>.json` and
   `dist\virustotal-v<version>.txt` with every VirusTotal entry `PENDING`. A failed or interrupted run leaves none of
   this version's artifacts in `dist\`: they are moved to `build\package-failed\`.
2. Do not modify, rebuild or repackage anything in `dist\` from now on.
3. Upload each of the four files on its own at https://www.virustotal.com/gui/home/upload.
4. Check that the SHA-256 VirusTotal shows for each file is the one in `dist\virustotal-v<version>.txt`.
5. In `dist\virustotal-v<version>.json`, replace each `PENDING` with that file's scan address,
   `https://www.virustotal.com/gui/file/<sha256>`, optionally ending in `?nocache=1`. Any other form (for example with
   `/detection` at the end) stays `PENDING`.
6. Run `.\tools\package-release.ps1 -NotesOnly`. It builds, tests and packages nothing, and never moves, deletes or
   changes an artifact. It checks the `.sha256` files and the staged folder against the ZIP, keeps only the addresses
   that name the exact current SHA-256 of their file, and rewrites the notes and both manifests.
7. Confirm that no `PENDING` remains: `-NotesOnly` ends with **READY FOR MANUAL RUNTIME TEST**, and the notes' Security
   table links all four scans.
8. Test the same frozen files: the ZIP and the AppImage from `dist\`, not a new build.
9. Publish exactly those files, the two `.sha256` files and the notes.

If anything has to change after step 1, start again from step 1: the new files need new scans.

## Before publishing

Do not publish while an engine flags any of these files, unless the detection has been investigated to a conclusion
and that conclusion is written down.

Never work around a detection by packing, obfuscating or encrypting a program, by disabling a security product or by
asking players to add an exclusion.

Write no more into the release notes than the scans show: link each scan, and give a detection count only when it was
read from that scan.
