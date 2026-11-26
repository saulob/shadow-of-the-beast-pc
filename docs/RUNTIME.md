# Runtime

## Pipeline

1. The launcher validates the package and installs the game into `game\prepared\<TITLE_ID>-1.00`.
2. `boot_image.cpp` maps the eboot at its link address, links `libc.prx` and `libSceFios2.prx` after it, records every
   relocation and leaves unresolved imports by `NID#library` name.
3. `src/loader.c` maps `boot.bin`, reserves guest memory, initializes the video core, resolves imports and starts the
   linked module initializers followed by the original entry point `0x19105d0`.
4. `src/runtime.c` and `src/runtime_*.c` implement the PS4 system libraries the bundled modules and the game import.
5. `gpu/shim/sotbgpu.cpp` registers the shadPS4 GNM and VideoOut implementations and owns the game window, present
   thread and shader recompiler.

## Guest memory

The guest image is a non-PIE x86-64 image based at 0. The runtime reserves the PS4 user range so normal host allocations
cannot occupy it, and creates the guest memory pool before any GPU or guest thread starts. Windows builds disable
high-entropy ASLR for the same reason.

The eboot's PT_TLS template is empty (filesz 0) with a 0x190-byte TLS block. On Windows, `fs:` TCB loads in the guest
image are rewritten to the TEB TLS slot that holds the guest TCB.

## Imports

`src/import_names.inc` maps every eboot and bundled-module NID to its symbol name using aerolib. The runtime resolves by
exact `NID#library`, then by symbol name for the POSIX/libkernel aliases. An import the runtime does not implement is
replaced by a trap that prints:

- the scoped NID and symbol name
- the caller's return offset
- the first argument
- the linked module containing the caller

It then exits with code 20. Nothing returns a made-up success value.

## Diagnostics

Logs are created in `logs\` next to the program, or in `build\logs\` when started through `tools\build.ps1 -Run`. A fatal exception reports the startup phase, exception code, faulting address, access type,
registers, the top stack words, the `rbp` chain and the loaded modules. Repeated failures are reported once where the
runtime can continue.

## First known blockers

- `libSceAvPlayer` (movies) comes from the Alienation port of shadPS4's AvPlayer over an LGPL FFmpeg built by
  `tools\build-ffmpeg.sh` (`gpu\ffmpeg.txt`: MP4, H.264, HEVC, AAC). `JumpToTime`, `SetTrickSpeed` and `DisableStream` are stubs.
- `libSceNgs2` (`src/runtime_ngs2.c`, sysmodule 0xb) provides the 12 imports the game uses. Systems, racks and voices
  are real handles kept in the buffers the game's own allocator returns; voices never play and `SystemRender` writes
  silence. `sceNgs2ParseWaveformData` refuses with `UNKNOWN_WAVEFORM_FORMAT`. Contracts follow shadPS4
  `src/core/libraries/ngs2` (GPL-2.0-or-later).
- `libSceAjm` decodes ATRAC9 through LibAtrac9; other codecs stop explicitly.
- `libSceAudio3d` is mixed to stereo over AudioOut.
- PSN and online services are signed out or unavailable; they are not a goal.
- Trophies are local and offline (`docs/TROPHIES.md`).
