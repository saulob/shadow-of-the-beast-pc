# Game builds

The port has one known build.

| Field | Value |
| --- | --- |
| TITLE | Shadow of the Beast |
| TITLE_ID | CUSA03762 |
| CONTENT_ID | HP9000-CUSA03762_00-SHADOWOFBEAST0KR |
| Region | Korea |
| CATEGORY | `gd` (base game) |
| APP_VER | 01.00 |
| VERSION | 01.01 |
| SYSTEM_VER | 0x04500000 (firmware 4.50) |
| PUBTOOLINFO | `c_date=20180630,sdk_ver=02508000,st_type=digital50` |
| Package size | 8,548,450,304 bytes |
| PFS image | 8,536,915,968 bytes at 0xb00000 |
| Extracted size | 16.2 GB (63 files) |
| eboot.bin size | 75,181,963 bytes |
| eboot.bin SHA-256 | `ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023` |

Engine data from the package:

- Unreal Engine 4.8 (`BranchName=++depot+UE4-Releases+4.8`)
- `EngineVersion=2591939`
- Build ID `16-03-15-15-59_BuildMachine_Shipping_KR_BD_CL13287`, SKU `KR_BD`
- Build timestamp `2015-06-18_10.40.50`

Executable facts:

- PS4 SELF wrapping a non-PIE x86-64 ELF (`ET_SCE_EXEC`, base 0)
- entry point `0x19105d0`
- 11 program headers; one non-required `PT_SCE_COMMENT`-style segment is absent from the SELF
- PT_TLS: filesz 0, memsz 0x190, align 16
- `.eh_frame` present (0x1d5ed4 bytes)
- 779 imports across 49 libraries
- 358,194 relocations
- no exported game symbols
- stripped (no usable symbol table beyond the dynamic import names)

Libraries that dominate the imports:

| Library | Imports |
| --- | ---: |
| libc | 156 |
| libkernel | 69 |
| libScePosix | 60 |
| libSceGnmDriver | 57 |
| libSceNpMatching2 | 34 |
| libSceNpWebApi | 28 |
| libSceFios2 | 25 |
| libSceNpTus | 25 |
| libSceHttp | 23 |
| libSceNpManager | 21 |
| libSceAvPlayer | 20 |
| libSceNet | 20 |
| libSceAudio3d | 14 |
| libSceNpTrophy | 13 |
| libSceNgs2 | 12 |
| libSceVideoOut | 12 |
| libSceAjm | 10 |
| libScePad | 8 |
| libSceSaveData | 8 |
| libSceAudioOut | 7 |
| libSceUserService | 6 |

The boot image links `sce_module/libc.prx` and `sce_module/libSceFios2.prx`. All other imports are left by name for the
native runtime.
