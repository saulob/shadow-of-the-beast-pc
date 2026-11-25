# Launcher header art

`header-art-source.png` is the official art (2172x724 RGB, approved by the author; no characters, creature, logo or
text). `header-art.png` is derived from it and is the only one that ships: 3840x432 (8.89:1) RGBA. The 2172x360 band
from row 155 (the tree's crown, the small ships, the structures, the horizon and the top of the grass) is scaled
uniformly to 2606x432 and anchored right, with transparent padding on the left. A taller band keeps the large ship and
more grass but leaves the art much narrower in the header. Do not edit `header-art.png` by hand, and there is no SVG
for this art.

A horizontal alpha mask lets the launcher's own header gradient show through on the left: alpha is 0 up to 33% of the
width (the band starts at 32.1%, inside it), rises along a smoothstep curve and reaches full at 50%, so the art has
no visible edge and the title side stays dark. Regenerate with (ffmpeg on PATH):

```powershell
tools\make-header-art.ps1
```

The launcher draws it over the header gradient at the header's full height, flush with the header's top, right and
bottom; a narrow window cuts it from the left (`launcher/header_art.h`). Without the file, or when it cannot be
decoded, it keeps the gradient alone. The build copies it beside the executable whenever it changes.

# Launcher palette

Metallic grey actions with cyan edges, from the art's stone and lit structures (`launcher/launcher_theme.h`):

| Use | Colour |
| --- | --- |
| Action fill / hover / pressed | `#52616C` / `#61727E` / `#3D4C56` |
| Action edge, PLAY glow | `#62D5EF` |
| Links, focus, slider grabs, checks, subtitle | `#82DEF2` |
| Action text | `#FFFFFF` |
| Header gradient | `#10151B` (title side), `#24312C` (dark steel with olive), `#121A1C`, page background |

Status, warning and Sponsor colours are not part of it. The game's own overlays use `src/sotb_accent.h`.

# Sponsor heart

`sponsor-heart.svg` is the source of the heart used by the Sponsor on GitHub buttons (CC0, see
`packaging/licenses/THIRD-PARTY-LICENSES.txt`). `sponsor-heart.png` is a white 64x64 rendering with a transparent
background; regenerate it after changing the SVG with:

```sh
rsvg-convert -w 64 -h 64 -f png -o assets/launcher/sponsor-heart.png assets/launcher/sponsor-heart.svg
```

# Page icons

`icons/` holds the sidebar's page SVGs, the source of truth, compiled into the launcher by
`launcher/page_icons.cpp`. `icons/SOURCES.md` records where each one came from and its license.
