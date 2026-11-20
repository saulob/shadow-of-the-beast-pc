# Graphics

The video core is shadPS4-derived (`gpu/`, see `gpu/VENDOR.txt`). The game renders a 1920 x 1080 picture; the window
scales it.

## Renderers

`renderer` in `user\sotb.ini`: `vulkan` (default), `auto`, `opengl`, `d3d11` (Windows; OpenGL ES through ANGLE's Direct3D 11).

- **Auto** asks whether Vulkan can run before the window exists. If it can, Vulkan runs; if not, OpenGL runs and the log says
  why (`Renderer: selected auto, active OpenGL`, then `Renderer: Auto fell back to OpenGL: <cause>`).
- **Vulkan, OpenGL and Direct3D 11 chosen by name** start or fail as themselves; there is no silent fallback.
- An unknown value runs Vulkan.

The selection, Auto fallback and parsing are covered by `sotb-launcher-tests`. Whether OpenGL and Direct3D 11 draw this
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

Not proven without running the game: that the game's memory budget holds 1440p and above.
