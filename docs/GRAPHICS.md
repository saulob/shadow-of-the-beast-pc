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

Not proven without running the game: that the game's memory budget holds 1440p and above, and how the game's camera and
HUD behave in a shape other than 16:9.
