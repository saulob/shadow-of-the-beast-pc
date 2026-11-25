# Game builds

The port supports three base (`CATEGORY = gd`) builds, all with `APP_VER = 01.00`, `VERSION = 01.01`, `eboot.bin` size 75,181,963 bytes, and entry point `0x19105d0`:

| Field | CUSA03762 (Korea) | CUSA02871 (USA) | CUSA00951 (Europe) |
| --- | --- | --- | --- |
| TITLE_ID | `CUSA03762` | `CUSA02871` | `CUSA00951` |
| CONTENT_ID | `HP9000-CUSA03762_00-SHADOWOFBEAST0KR` | `UP9000-CUSA02871_00-SHADOWOFBEAST0US` | `EP9000-CUSA00951_00-SHADOWOFBEAST0EU` |
| Region | Korea | USA | Europe |
| SYSTEM_VER | `0x04500000` | `0x02700000` | `0x02508000` |
| ATTRIBUTE | `0x0` | `0x2` | `0x2` |
| Package size | 8,548,450,304 bytes | 8,548,777,984 bytes | 8,548,843,520 bytes |
| PFS image | 8,536,915,968 bytes at `0xb00000` | 8,537,243,648 bytes at `0xb00000` | 8,537,309,184 bytes at `0xb00000` |
| Extracted size | 17,413,167,431 bytes (63 files) | 17,681,603,184 bytes (63 files) | 17,681,603,261 bytes (63 files) |
| `eboot.bin` SHA-256 | `ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023` | `ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023` | `7c35e4edc6304dc22f665182fb89385a589d04c9d4a7ac54fd8d4652e74a9343` |
| `ue4commandline.txt` | `-BuildID=16-03-15-15-59_BuildMachine_Shipping_KR_BD_CL13287 -SKU=KR_BD` | `-BuildID=16-03-15-11-58_BuildMachine_Shipping_US_CL13283 -SKU=US` | `-BuildID=16-03-15-11-58_BuildMachine_Shipping_EU_CL13283 -SKU=EU` |
| `pakchunk0-ps4.pak` | 6,634,484,704 bytes (index CRC `0x7259963b`) | 6,902,920,160 bytes (index CRC `0xa1cd828c`) | 6,902,920,160 bytes (index CRC `0xa1cd828c`) |

Engine data shared across all three packages:

- Unreal Engine 4.8 (`BranchName=++depot+UE4-Releases+4.8`)
- `EngineVersion=2591939`
- Build timestamp `2015-06-18_10.40.50`
- NP Communication ID (`sce_sys/npbind.dat`): `NPWR08784_00` (`trophy00.trp` is byte-identical across all three builds)

Executable facts:

- `CUSA03762` and `CUSA02871` ship byte-identical `eboot.bin` files; `CUSA00951` differs only in 39 SELF/ELF header bytes and 8 trailing bytes at `0x3be8010..0x3be8017` (the very end of `PT_LOAD[0]`). All code, data, relocations, imports and patch sites are identical.

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
