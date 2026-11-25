# Graphics

The video core is shadPS4-derived (`gpu/`, see `gpu/VENDOR.txt`). The game renders a 1920 x 1080 picture; the window
scales it.

## Renderers

`renderer` in `user\sotb.ini`: `vulkan` (default), `auto`, `opengl`, `d3d11` (Windows; OpenGL ES through ANGLE's Direct3D 11).

- **Auto** asks whether Vulkan can run before the window exists. If it can, Vulkan runs; if not, OpenGL runs and the log says
  why (`Renderer: selected auto, active OpenGL`, then `Renderer: Auto fell back to OpenGL: <cause>`).
- **Vulkan, OpenGL and Direct3D 11 chosen by name** start or fail as themselves; there is no silent fallback.
- An unknown value runs Vulkan.

## Vulkan preflight

Before any renderer or window exists, `gpu/shim/sotb_vulkan_support.h` decides step by step whether Vulkan can run: loader,
runtime version (1.3), instance, devices, then each device's mandatory capabilities. It returns the stage, the GPU and driver
and the first missing capability instead of asserting, and the launcher's Play check, the Graphics card list and the direct
launch all use it. Renderer Vulkan that fails it stops with the cause (`Renderer: Vulkan was requested and cannot run: ...`);
Auto falls back to OpenGL as above.

- **Mandatory**: `VK_KHR_swapchain`, `VK_KHR_push_descriptor`, `VK_EXT_vertex_attribute_divisor`, `VK_EXT_robustness2` with
  `robustBufferAccess2`, `robustImageAccess2` and `nullDescriptor`, a graphics queue and a device-local memory heap.
- **Optional**: every other extension the renderer enables; missing ones never reject a card.
- **Handled by a fallback**: sparse buffers (below).

The log shows `Renderer: requested ...`, `Renderer: Vulkan preflight: passed|failed (...); GPU ..., ...; missing ...`, and the
selected and active renderer.

## GPU memory arenas

The buffer cache uses sparse arenas when the device has `sparseBinding`, `sparseResidencyBuffer` and a graphics queue that can
bind sparse memory. Anything less selects fully resident arenas (16 MiB pages at most, 64 KiB blocks, migrated by copy when
an access spans arenas); guest-visible behaviour is the same. The log says `GPU memory arena: sparse` or
`GPU memory arena: fully resident fallback (reason)`. The decision and the arena layout live in
`gpu/shadps4/video_core/buffer_cache/arena_mode.h` and `arena_layout.h`.

The selection, Auto fallback and parsing are covered by `sotb-renderer-selection-tests`, the preflight by
`sotb-vulkan-preflight-tests` and the arenas by `sotb-buffer-arena-tests`. Whether OpenGL and Direct3D 11 draw this
game correctly has not been verified on a GPU; Vulkan is the renderer the game has been brought up with.

## Graphics card

`graphics_card`: the launcher lists only usable devices (no Auto entry) and normalizes a missing saved card to the first
usable one. An empty or legacy `automatic` value, or a name that is not installed, makes the renderer take the first usable
device and log `GPU: requested ... is not available`. The list comes from
Vulkan, so the OpenGL and Direct3D 11 renderers ignore it.

## Host passes (Vulkan renderer only)

- **Upscaling** (`fsr1`): FSR 1 when the window is larger than the game's picture.
- **Sharpening** (`rcas`, `rcas_strength`): RCAS, alone at the game's picture size or after FSR 1. Off means it does not run.
- **Anti-aliasing** (`smaa`): SMAA on the finished picture. The game keeps its own anti-aliasing either way.

`FsrPassRuns` decides when the pass runs: FSR 1 only with a larger window, RCAS alone only at the same size, nothing for a
smaller window. The OpenGL and Direct3D 11 renderers apply none of these three.

## Texture filtering

`anisotropy` (0 is the game's own, else 2, 4, 8, 16) sets the sampler anisotropy in the Vulkan and OpenGL rasterizers.

## Presentation, VSync, frame rate

- **Presentation**: Letterbox keeps the picture's shape, Stretch fills the window.
- **VSync** (`vsync`) is read by the window and every renderer's swapchain.
- **Frame rate** (`frame_rate`) is presentation pacing: the game's own 60 Hz tick is not changed and a higher rate shows
  the last frame again between two game flips (`gpu/shim/sotb_frame_pacing.h`). It does not speed the game up.
- **Frame counter** (`show_fps`, F2) draws the frame rate and frame time top right. The title options (`show_fps_title`,
  `show_resolution_title`) put the frame rate and the render resolution in the window title.

## Render resolution

The Unreal Engine 4.8 PS4 platform layer of CUSA03762 1.00 hard-codes the display as 1920 x 1080 in six instructions that
fill the engine's display metrics (image offsets 0x607f0c, 0x6080b7, 0x609011, 0x609037, 0x609041, 0x609290: `movabs` of
0x0000043800000780 into rax or rdx, and two `mov dword [rbp-0x2268/0x2264]` immediates). `src/guest_display.c` replaces those
immediates with the selected 16:9 resolution, so the engine sizes its viewport, scene targets and flip buffer to it; the video
core takes the frame size from the registered flip buffer and scales it to the window. Every site is compared with its
original 10 bytes first and nothing is written unless all six match.

The six sites hold the same bytes at the same offsets in CUSA03762, CUSA02871 and CUSA00951 (`tests/aspect_ratio_tests.cpp`
reads all three), so one table serves them and any altered image is refused.

**Aspect Ratio** (`aspect_ratio`) reuses the same sites with a width and a height of any shape. The height is the Render
resolution's; the width is the shape's at that height rounded to a multiple of 8, and the height drops in steps of 8 until
the target fits 3840 x 2160 pixels. 16:9 passes the Render resolution through untouched (854 x 480 included) and Native in
16:9 still patches nothing. The run log prints `Display: render resolution WxH (key), aspect ratio R`. The presenter takes the
frame's shape from the game's registered image, and SMAA and FSR 1 take its size as their input, so nothing in the host
assumes 16:9. The loading screen hint is laid out in its 1920 x 1080 design space, centred, in any shape. Camera, field of
view, HUD and prerendered videos are left to the game.

**True Ultrawide (21:9 and 32:9, `true_ultrawide`, on by default).** The game's cameras are constrained to 16:9 (aspect
ratio field 1.777778 and the constrain flag), so a wider target shows the 3D inside a 16:9 rectangle. Every local player
view goes through `FMinimalViewInfo::CalculateProjectionMatrixGivenView` (image offset 0x127fd80, 1200 bytes, identical on
the three builds; its two callers are the scene view and `GetProjectionData`). `src/guest_ultrawide.c` hooks its entry (a
6-byte jump to a stub in the patch cave) and, for a constrained perspective view, sets the view's aspect ratio to the
viewport's and its field of view to `2 * atan(tan(fov / 2) * viewport / original)`. In that function the field of view is
the horizontal one (x scale `1 / tan(fov * pi / 360)`, y scale `aspect / tan`), so this keeps the camera's vertical view and
adds width at the sides. Gameplay (61.5 degrees) and the title (39.5) are such views; the map, the options and the default
camera are unconstrained and are left alone, as are orthographic views.

The game's widgets lay out in a centered 16:9 area at the full height of the viewport: `SGameLayerManager` (constructor
0x145b290, vtable 0x3d136a0) inherits `SCompoundWidget::OnArrangeChildren` in slot 62, and that vtable entry is replaced by a
function that hands the original a geometry narrowed to the 16:9 part. Widgets anchored left, right or centered then resolve
against that area; the 3D keeps the whole viewport. The vtable is read after relocation, so the patch is applied by the loader
once the image is relocated and refuses anything that is not the checked constructor and vtable.

Which screen is open comes from the game itself. `UFrontendManager` keeps a stack of open screens (a `TArray` at +0x280 of
16-byte entries: the `FrontendScreen` byte, then the widget); its constructor (0x190d50), the function that reads the top
widget (0x196c60) and its per-frame function (0x196cc0, slot 80 of the vtable at 0x3c0a290) are fingerprinted, and the slot
is replaced by a function that runs the original and then reads the top screen. `MainMenu` (2) is the title; `Map` (3),
`MapLiveFeed` (14), `MapConfirmationOverlayScreen` (27), `TimelineInfoOverlayScreen` (28), `MapInfoOverlayScreen` (29) and
`ManaOverlayScreen` (32, the LEVEL SCORE / TOTAL MANA animation, `ManaPopupOverlayScreen_C` in the map's `FrontendManagerBP`) are the map. The loading screen
clears the state, and the next frame of the manager sets it again. The layout then follows the screen:

- the title and the map lay out in the safe area, always, from the first frame they open (the map is a canvas of
  locations at absolute positions in a 1920 x 1080 design space, so with the whole viewport it sat at the left edge);
- `Options` (4, `OptionsScreenBP`) and `ShopContainer` (30, `ShopContainerBP`: Wisdom of Shadows) do the same: both are
  1920 x 1080 UMG canvases with no background image of their own, so their content is centred while the scene behind them
  keeps the whole viewport and the presenter adds no side fill;
- everything else (gameplay and what shares its world: the HUD, the pause) uses the safe area for 750 ms after a widened
  view was drawn, as before.

None of these screens reads a clock or a camera.

The gameplay HUD is not Slate. `ABeastHUD` (a native `AHUD`) draws its components into a `UCanvas` the size of the view
rectangle, in a 1920 x 1080 design scaled by the canvas width alone (scale = width / 1920, kept in `hud+0x518`), with the
Health and Enemies Remaining blocks anchored to the bottom (design y 904 to 1060) and the right edge. On a canvas wider than
16:9 the bottom blocks therefore fall off the screen. With True Ultrawide on, `src/guest_hud.c` replaces the Draw slot (vtable
slot 103, 0x348 after the vtable object) of every persistent component class (fingerprinted, all or nothing) with a wrapper:
on a canvas wider than 16:9 it lays the component out as a 16:9 screen of the same height (scale = height / 1080, canvas
width = `round(height * 16 / 9)`) and shifts the draw origin with `FCanvas::PushRelativeTransform` by `0` (left anchored:
Health, the top left combo score), `width - virtual_width` (right anchored: Enemies Remaining, the jetpack bar, the
encounter score and target scores, elixir, unlock, talisman text, cutscene skip, save throbber) or `(width - virtual_width) /
2` (centred: time remaining, rage chain, prompts, subtitles, boss bar, tutorials). The side glows keep the real width and
move only vertically so their centre is the screen's. The full-screen effects (blood, hit flash, fades) are drawn by direct
calls and keep the whole canvas. The one exception inside a wrapped component is the cutscene skip's fade, a black tile
the size of the canvas: its `UCanvas::DrawItem` call (0x2e37c0) goes through a cave stub that maps the tile back onto the
whole real canvas while the component is right anchored. At 16:9 or narrower nothing changes.

The game only draws its central 16:9 part on the title and on the loading screen, so with True Ultrawide on the Vulkan
presenter's post process pass treats the sides of the picture (`gpu/shim/sotb_side_fill.h`, `post_process.frag`). There are
two separate modes. Loading blur: while the loading screen's own arrangement is drawn (held 200 ms), the sides show the
central part zoomed to the full width, blurred (32 taps) and darkened to 55%. Title and map: while one of them is the open screen,
the sides keep the scene the game draws there (its sky, which is brighter than what the central part shows) and get the
vignette that the screen's own background image lays over the central part. That image is `FullScreenDarken`, a black
texture with per-pixel alpha, stretched over the 16:9 layout (at full alpha on the title, at half on the map); the post
process applies its left and right edge alpha per row (108 values, from the texture itself) in the encoded values of the
picture, so the sides continue the central part without a seam. The slot of that image stops 1.92188 of its 1920 units
short of the central part's right edge, which left a thin unshaded line there; that strip gets the vignette too. The
game darkens whole source columns (those whose centre the image covers), so a frame pixel that straddles either edge of
that span is vignetted column by column instead of all or nothing. Nothing
is read from any other pixel: no repeated edge, no blur, no zoom, no mirror. Both end the moment the game's
screen stack says so, with no hold. The central part is never touched.
The OpenGL and Direct3D 11 renderers have no side fill.

16:9, 16:10, 4:3 and 5:4, and True Ultrawide off, install no hook and mark no fill. The run log has `Ultrawide: ...` lines at start.

Not proven without running the game: that the game's memory budget holds 1440p and above, and how the game's camera and
HUD behave in a shape other than 16:9.
