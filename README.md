# TextDraw Anti-Aliasing Fix

`TextDrawAntiAliasingFix.asi` is a standalone GTA San Andreas plugin that
supersamples the SA-MP model preview pass used by preview-model textdraws.

SA-MP does not draw preview-model textdraws directly into the frame. The client
creates its own RenderWare camera with a 256x256 camera-texture raster and a
matching Z raster, renders the model into that off-screen raster, and then
composes the result on screen as a 2D sprite. This is the same rendering
principle the game uses for planar mirrors, and it has the same consequence:
driver anti-aliasing profiles applied to the main backbuffer never reach the
off-screen target, so preview models stay jagged while the scene behind them is
smooth.

The plugin redirects only that preview pass into a plain render
target several times the size of the preview raster, together with a matching
depth surface. The clear the client performed on the original raster is repeated
there, so the background colour and alpha are preserved, and after the model is
rendered the image is reduced back into the client's preview texture. The
preview camera, its projection and the sprite composition pass are untouched, so
the framing of the preview is exactly what the client asked for.

The anti-aliasing is done by supersampling rather than by multisampling:
rendering this pass into a multisampled target loses the depth comparison, and the body shell of a model is rejected
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
- Release of the plugin's surfaces before every device reset, so alt-tab from
  exclusive fullscreen restores the game as it did without the plugin.

## Requirements

- GTA San Andreas 1.0 US (Compact or Hoodlum executable).
- An ASI loader, such as Silent's ASI Loader or Ultimate ASI Loader.
- SA-MP. The interception was verified against the `0.3.7-R1` client; the
  preview pass is identified by the calling module rather than by client
  addresses, so other builds are expected to work but were not inspected.
- A Direct3D 9 device able to allocate a render target and depth surface of
  `previewScale * supersample * 256` pixels a side in the preview raster's
  formats.

Other executables are unsupported: the hooks and RenderWare bindings are
address-specific, and the plugin does nothing when the image base or the
expected bytes do not match.

## Installation

1. Extract `TextDrawAntiAliasingFix.asi` and `TextDrawAntiAliasingFix.ini`
   into the GTA San Andreas directory or its `scripts` directory.
2. Start the game and connect to a server that uses preview-model textdraws.

## Configuration

```ini
# TextDraw Anti-Aliasing Fix v1.0.2
# Created by sonochiwa
# Source code: https://github.com/sonochiwa/sa-textdraw-anti-aliasing-fix

[antiAliasing]
supersample=4
previewScale=2
```

| Setting | Default | Meaning |
| --- | ---: | --- |
| `[antiAliasing]` | | |
| `supersample` | `4` | Rendering resolution multiplier for the preview. Values are normalized to `1`, `2`, `4` or `8`; `1` disables anti-aliasing while keeping the rest of the path. |
| `previewScale` | `2` | Multiplies the client's 256x256 preview raster. Values are normalized to `1`, `2`, `4` or `8`; `1` keeps the original size. |

Both settings are read once when the plugin loads, and the preview raster is
allocated once, when the client builds its preview camera, so a changed value
needs a game restart.

The two multipliers compound: `previewScale` decides how large the texture the
client composes is, and `supersample` decides how much larger than that the
model is rasterized before being reduced into it. The defaults render at
2048x2048 for a 512x512 preview texture.

## Building

Visual Studio 2022 (v143), `Release|Win32`. Open `TextDrawAntiAliasingFix.sln`
or run:

```powershell
msbuild TextDrawAntiAliasingFix.sln /t:Rebuild /p:Configuration=Release /p:Platform=Win32
```

The plugin is written to `build\TextDrawAntiAliasingFix.asi` next to a copy of
the INI.

## Repository Layout

```text
TextDrawAntiAliasingFix.sln
README.md
CHANGELOG.md
LICENSE
.github\workflows\release.yml   Tagged release build, checksum and attestation
Config\
  TextDrawAntiAliasingFix.ini   Canonical configuration, embedded as RCDATA
src\
  TextDrawAntiAliasingFix.cpp   DllMain and the initialization thread
  TextDrawAntiAliasingFix.rc    Version resource and the embedded INI
  TextDrawAntiAliasingFix.vcxproj
  addresses.h                   Game addresses, offsets, expected bytes
  config.cpp / config.h         INI creation and loading
  hooks.cpp / hooks.h           RenderWare camera and raster detours
  patch.cpp / patch.h           Safe reads, protected writes, detours
  samp.cpp / samp.h             Caller check against samp.dll
  surfaces.cpp / surfaces.h     Supersampled surfaces and the reset hook
  resource.h
  version.h
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

Every surface the plugin creates lives in `D3DPOOL_DEFAULT`, and
`IDirect3DDevice9::Reset` fails while any such surface is alive. The game resets
the device after it was lost, which is what alt-tab does in exclusive fullscreen,
and RenderWare retries a failed reset every frame without drawing anything. The
plugin therefore hooks `Reset` through the device vtable the first time it sees
the device, releases its surfaces before forwarding the call, and recreates them
on the next preview.

## Release Integrity

Tagged releases are built by GitHub Actions from the tagged commit. Each
release carries `TextDrawAntiAliasingFix-vX.Y.Z.zip`, its SHA-256 in
`TextDrawAntiAliasingFix-vX.Y.Z.zip.sha256` and a signed build-provenance
attestation, which proves that the archive was produced by this repository's
workflow from that revision. It does not prove the code is bug-free.

```text
gh attestation verify TextDrawAntiAliasingFix-vX.Y.Z.zip -R sonochiwa/sa-textdraw-anti-aliasing-fix
```

## License

MIT. See [LICENSE](LICENSE).
