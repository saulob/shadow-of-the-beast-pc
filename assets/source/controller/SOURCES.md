# Xbox Series controller line art

`xbox-series-lineart.png` is the source of the controller outline the Xbox controller prompts are built from
(`tools/prompts/xbox_diagrams.py`, `tools/prompts/xbox_start_map.py`):

- Source: Gamepad Line Art Pack, https://gidedin.itch.io/gamepad-line-art-pack
- File: `png/regular/light/xbox-series.png`, a 1024 px transparent drawing, unmodified
- Author: gidedin
- License: CC0 1.0 Universal. See `LICENSE-Gamepad-Line-Art-Pack.txt`.
- The pack covers the drawings only and grants no trademark rights. Xbox is a trademark of Microsoft; the art is not
  affiliated with or endorsed by it.

The generator thins and rescales the outline and draws the caption lines over it. Nothing else in this folder is used at
run time: the game ships the generated result (`src/sotb_prompt_assets_data.c`), not this file.

The button glyphs (A, B, X, Y, LB, RB, LT, RT, stick clicks, Menu, View, D-pad) are drawn by the generator from vector shapes
with the letters of DejaVu Sans (`gpu/third_party/fonts`, notice in `packaging/licenses/THIRD-PARTY-LICENSES.txt`).
