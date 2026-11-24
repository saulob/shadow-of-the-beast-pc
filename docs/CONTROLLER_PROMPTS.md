# Controller prompts in the game

The **Controller prompts** setting (`controller_prompts` in `sotb.ini`: `auto`, `xbox`, `playstation`) decides which button
icons the game draws, as well as the button names on the launcher's Controls page. It is read once, before the guest starts,
and applies at the next game start.

| Setting | Game icons |
| --- | --- |
| `playstation` | The game's own PlayStation art, untouched; no overlay is installed. |
| `xbox` | The Xbox art, whatever controller is connected. |
| `auto` | The first connected controller at startup decides: DualShock 3/4 and DualSense give PlayStation, anything else (Xbox, generic, unknown, none) gives Xbox, as on the Controls page. A controller connected later does not change the game's icons. With none listed the runtime waits up to about 120 ms for a driver that lists it late. |

An unusable value is `auto`. The launcher's button names keep following the controller in use while it runs.

The prompt style is separate from the menu Confirm/Back layout. **Menu Button Layout** decides whether
Cross/A or Circle/B confirms; the glyphs above always follow the key the game binds. See `docs/MENU_BUTTONS.md`.

## How the Xbox art reaches the game

`pakchunk0-ps4.pak` is a version 3 UE4 pak, uncompressed and unencrypted. Every prompt is a cooked `Texture2D` in it. The
runtime serves the pak through the file overlay of `runtime_file.c` (`runtime_file_overlay`): reads return the host bytes
with the replacement bytes written over them. The pak on disk is only read.

`src/sotb_prompt_assets.c` installs the overlay, all or nothing:

1. the build has a prompt set (`CUSA03762-1.00`), the pak has the recorded size, footer, and index CRC-32;
2. every texture is inside the pak, and its package data has the recorded CRC-32;
3. the replacement bytes inflate to the recorded size and CRC-32, and each texture with its runs applied has the recorded
   replacement CRC-32;
4. only then is the overlay registered.

Any mismatch (a modified pak, another build, a damaged table) installs nothing: the game starts with the PlayStation art and
the log has one line naming the asset and the reason. Startup work is bounded: about 22 MB of reads and CRCs and a 2.6 MB
inflate, around 100 ms, once.

`src/sotb_prompt_assets_data.c` is generated. The runs are new Xbox art; on the Press Start controls page the changed blocks also carry that page's own background around the redrawn parts.

## What is replaced (CUSA03762 1.00)

81 textures in `pakchunk0-ps4.pak`, all PS4 1D thin micro-tiled; DXT5 with a mip chain, BGRA8, and one DXT1 page:

| Group | Textures |
| --- | --- |
| HUD prompts, normal and pressed | Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, L3+R3, Options, Touchpad (`HUD/Textures/Button*`), D-pad, left and right stick |
| Alternate HUD prompts | the same set with an outline (`HUD/Textures/ButtonsALT`) |
| In-game prompt font | the second page of `HUD/Fonts/ButtonIconFont_48`: the 16 controller glyphs the HUD, cutscene skip and attack prompts draw inline |
| Controller Settings captions | 12 BGRA captions (`Frontend/ControllerSettingsScreen/Textures/Buttons`) |
| Menu hints | `Frontend/Generic/Buttons/Textures` (L1, R1, L2, R2, L3+R3, Touchpad, small L1/R1) |
| Wisdom of Shadows | the Triangle prompt (`Frontend/MapScreen/3DPanels/ManaPopup/ButtonTriangle`) |
| Controller Settings diagram | the controller body and its 15 caption lines (`Frontend/OptionsScreen/Textures/3DPanels/Controls`) |
| Controls page before Press Start | `HUD/PressStartScreen/ControllerMapStart`, 1920x1080 DXT1: controller, red caption lines, inline icons |

Prompts are textures; the text strings only hold tokens such as `[JumpButton]`, which the prompt font renders as glyphs.
The font's other page and its non-controller inline icons (blood bars, talisman, Wrath of Aarbron, elixir) are not changed.
The 1989 game inside the package (`Beast89`) and the videos are not changed.

Mapping (the default binding of each PS4 button in `sotb_pad.c`): Cross A, Circle B, Square X, Triangle Y, L1 LB, R1 RB, L2 LT,
R2 RT, L3 and R3 the stick clicks, Options Menu, Touchpad View (the controller's Back button), D-pad the Xbox cross. The game has
no Share prompt. The Xbox controller has no trigger drawn in the front view, so the LT and RT caption lines end on the top
edge above their bumper.

## Regenerating

    python tools/prompts/build_controller_prompts.py --game game/prepared/CUSA03762-1.00
    python tools/prompts/build_controller_prompts.py --game game/prepared/CUSA03762-1.00 --check
    python tools/prompts/build_controller_prompts.py --game game/prepared/CUSA03762-1.00 --preview scratch/prompts

Needs Python 3 with numpy and Pillow, at development time only; the generator ignores MSYS2 folders on `PATH`, whose libraries change the encoded bytes. The generator reads the installed game, validates every
texture it patches, redraws the glyphs and the diagram, re-encodes only the blocks that change, and writes the runs. Glyphs
are drawn from vector shapes with the DejaVu Sans letters already in `gpu/third_party/fonts`; the controller outline is the
CC0 Gamepad Line Art Pack drawing in `assets/source/controller` (see `SOURCES.md` there and `packaging/licenses`).
