The launcher header art is an original gradient and silhouette made for this project. It contains no game art.

`sponsor-heart.svg` is the source of the heart used by the Sponsor on GitHub buttons (CC0, see
`packaging/licenses/THIRD-PARTY-LICENSES.txt`). `sponsor-heart.png` is a white 64x64 rendering with a transparent
background; regenerate it after changing the SVG with:

```sh
rsvg-convert -w 64 -h 64 -f png -o assets/launcher/sponsor-heart.png assets/launcher/sponsor-heart.svg
```

`icons/` holds the sidebar's page SVGs, the source of truth, compiled into the launcher by
`launcher/page_icons.cpp`. `icons/SOURCES.md` records where each one came from and its license.
