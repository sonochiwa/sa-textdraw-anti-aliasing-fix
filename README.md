# TextDraw Anti-Aliasing Fix

A GTA San Andreas ASI plugin that supersamples the SA-MP model preview render
pass used by preview-model textdraws.

SA-MP does not draw preview-model textdraws directly into the frame. The client
creates its own RenderWare camera with a 256x256 camera-texture raster and a
matching Z raster, renders the model into that off-screen raster, and then
composes the result on screen as a 2D sprite. This is the same rendering
principle the game uses for planar mirrors, and it has the same consequence:
driver anti-aliasing profiles applied to the main backbuffer never reach the
off-screen target, so preview models stay jagged while the scene behind them is
smooth.

TextDraw Anti-Aliasing Fix redirects only that preview pass into a plain render
target several times the size of the preview raster, together with a matching
depth surface. The clear the client performed on the original raster is repeated
there, so the background colour and alpha are preserved, and after the model is
rendered the image is reduced back into the client's preview texture. The
preview camera, its projection and the sprite composition pass are untouched, so
the framing of the preview is exactly what the client asked for.

The anti-aliasing is done by supersampling rather than by multisampling. That is
a measured decision, not a preference: rendering this pass into a multisampled
target loses the depth comparison, and the body shell of a model is rejected
while its interior and far side remain, at every sample count from `2x` upward.
The same swap into a plain single-sample target reproduces the client's own
image exactly. Supersampling also smooths texture detail inside the silhouette,
which multisampling would not have done.

The plugin acts only on RenderWare camera updates entered from `samp.dll`.
Single-player rendering, the game's own mirrors, vehicle environment maps and
water reflections are out of scope.

## Features

- `2x`, `4x` or `8x` supersampling of the SA-MP model preview pass.
- Reduction back to the preview raster through a chain of exact 2:1 steps, which
  is what makes the bilinear filter in `StretchRect` behave as a box filter.
- Automatic fallback to a lower factor when a surface cannot be allocated.
- Faithful replay of the client's camera clear colour and alpha, with depth and
  stencil always cleared on the plugin's own surfaces.
- Optional `2x`, `4x` or `8x` enlargement of the client's 256x256 preview
  raster, with the camera and framing untouched.
- `Alt + T` toggle that turns the fix on and off in a running game and reloads
  `supersample` when it turns back on.
- Optional diagnostic log and preview dumps for measuring what was intercepted.

## Requirements

- Grand Theft Auto: San Andreas PC, Hoodlum/US 1.0 executable.
- SA-MP. The interception was verified against the `0.3.7-R1` client.
- An ASI loader.
- A Direct3D 9 graphics device able to allocate a render target and depth
  surface of `previewScale * supersample * 256` pixels a side in the preview
  raster's formats.

Other game executables are not supported because the hooks and RenderWare
bindings are address-specific. The plugin identifies the preview pass by the
calling module rather than by client-side addresses, so it does not depend on a
single `samp.dll` build, but only `0.3.7-R1` was inspected. Runtime coexistence
still needs validation with the user's complete mod and driver-profile setup.

## Installation

1. Install an ASI loader in the GTA San Andreas directory.
2. Copy `TextDrawAntiAliasingFix.asi` and `TextDrawAntiAliasingFix.ini` next to
   `gta_sa.exe`.
3. Fully restart the game and connect to a server that uses preview-model
   textdraws.

## Configuration

The default `TextDrawAntiAliasingFix.ini` is:

```ini
# TextDraw Anti-Aliasing Fix v1.0.0
# Created by sonochiwa
# Source code: https://github.com/sonochiwa/sa-textdraw-anti-aliasing-fix
# Default toggle hotkey: Alt + T

[general]
isEnabled=1
hotkeyEnabled=1
hotkeyModifier=18
hotkeyKey=84
logging=0
dumpPreviews=0

[antiAliasing]
supersample=4
previewScale=2
```

| Setting | Default | Meaning |
| --- | ---: | --- |
| `isEnabled` | `1` | Master switch. The hotkey rewrites this key, so the last state survives a restart. |
| `hotkeyEnabled` | `1` | Enables the toggle hotkey. |
| `hotkeyModifier` | `18` | Decimal Win32 virtual-key code of the modifier. `18` is `Alt`; `0` means a single-key hotkey. |
| `hotkeyKey` | `84` | Decimal Win32 virtual-key code of the main key. `84` is `T`. An empty or zero value disables hotkey handling. |
| `logging` | `0` | Writes `TextDrawAntiAliasingFix.log` next to the plugin. The file is recreated on every start. |
| `dumpPreviews` | `0` | Diagnostic mode. Writes each sampled preview to a 32-bit TGA next to the plugin and steps the supersample factor on every toggle, so one session produces every factor as files. |
| `supersample` | `4` | Rendering resolution multiplier for the preview. Values are normalized to `1`, `2`, `4` or `8`; `1` disables anti-aliasing while keeping the rest of the path. |
| `previewScale` | `2` | Multiplies the client's 256x256 preview raster. Values are normalized to `1`, `2`, `4` or `8`; `1` keeps the original size. |

The whole file is read once when the plugin loads. `Alt + T` then turns the fix
on and off while the game runs, which makes an A/B comparison possible without a
restart; the hotkey only reacts while the game window is in the foreground.
Turning it back on re-reads `supersample`, so a different factor can be edited
in the INI and applied live. `isEnabled` is rewritten on every toggle.

`previewScale` is not part of the hotkey. The raster is allocated once, when the
client builds its preview camera, so a changed value needs a game restart.

The two multipliers compound: `previewScale` decides how large the texture the
client composes is, and `supersample` decides how much larger than that the
model is rasterized before being reduced into it. The defaults render at
2048x2048 for a 512x512 preview texture.

## Building

Use Visual Studio 2022 with the v143 C++ toolset. Build
`TextDrawAntiAliasingFix.sln` as `Release|Win32`:

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  TextDrawAntiAliasingFix.sln /t:Rebuild /p:Configuration=Release /p:Platform=Win32 /m
```

The plugin and canonical INI are written to `build\`. Release builds use the
static C/C++ runtime and require no vendored SDK or runtime shader compiler.

## Repository Layout

```text
TextDrawAntiAliasingFix.sln
Config/
  TextDrawAntiAliasingFix.ini
src/
  TextDrawAntiAliasingFix.cpp
  TextDrawAntiAliasingFix.vcxproj
.github/workflows/
  release.yml
packaging/
  README.txt
```

## How It Works

The plugin validates the fixed US 1.0 code locations and their expected bytes,
then detours `RwCameraClear`, `RwCameraBeginUpdate`, `RwCameraEndUpdate` and,
when `previewScale` is above `1`, `RwRasterCreate`. The three camera detours
reproduce the original dispatch through the RenderWare engine instance or the
camera's own update pointers; the raster detour relocates the single five-byte
instruction it replaces into a trampoline. Calls that do not come from
`samp.dll` behave exactly as before.

`RwRasterCreate` is only altered for a 256x256 camera-texture or Z-buffer raster
requested by the client, which is the pair its preview camera is built from.

When the client clears its preview camera, the hook records the colour and
alpha. When the client begins the update on that camera, the begin hook captures
the render target and depth surface RenderWare has just bound and substitutes
its own enlarged pair, then replays the recorded colour clear. Depth and stencil
are cleared unconditionally: these surfaces are the plugin's own and persist
between previews, while the client only ever asks for an image and depth clear
even though the depth format it uses carries a stencil channel.

The end hook restores the client's surfaces and reduces the rendered image into
the preview raster. The reduction is a chain of `StretchRect` calls, each
exactly 2:1. This matters: `StretchRect` filters bilinearly and therefore reads
only a 2x2 neighbourhood, so a single `4x` or `8x` reduction would discard most
of the rendered image rather than average it. At 2:1 the bilinear tap lands in
the centre of each 2x2 block and averages all four texels.

## Release Integrity

Tagged archives are built by GitHub Actions from the tagged source revision.
Each release includes a SHA-256 checksum and a signed GitHub build-provenance
attestation. Verify an archive with:

```powershell
gh attestation verify TextDrawAntiAliasingFix-v1.0.0.zip -R sonochiwa/sa-textdraw-anti-aliasing-fix
```

This verifies archive provenance and integrity; it is not a guarantee that the
software is bug-free or safe for every mod configuration.

## License

[MIT](LICENSE)
