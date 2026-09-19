# Changelog

## 1.0.3

- Added `README.txt` to the release archive.

## 1.0.2

- Added creation of `TextDrawAntiAliasingFix.ini` next to the plugin when it
  is missing, byte for byte the canonical file.
- Added version information to the plugin file.
- Removed `README.txt` from the release archive; the repository README is the
  documentation.

## 1.0.1

- Fixed the game staying on a black screen after alt-tab once a preview had
  been rendered. The plugin's render targets and depth surfaces live in
  `D3DPOOL_DEFAULT`, which made the device reset the game performs on
  regaining focus fail; they are now released before every reset and
  recreated on the next preview.

## 1.0.0

- Added `2x`, `4x` or `8x` supersampling, defaulting to `4`, of the SA-MP model
  preview render pass
  used by preview-model textdraws, which the client draws into an off-screen
  256x256 raster that driver anti-aliasing never reaches.
- Added reduction back into the preview raster through a chain of exact 2:1
  steps, so the bilinear filter in `StretchRect` acts as a box filter instead of
  discarding most of the rendered image.
- Added a `previewScale` factor that enlarges the client's preview raster, which
  is what limits sharpness when a preview textdraw is displayed larger than it.
- Added automatic fallback to a lower factor when a surface cannot be allocated.
- Added faithful replay of the client's camera clear colour and alpha, with
  depth and stencil always cleared on the plugin's own surfaces.
- Preserved the preview camera, its projection, its clear colour and the sprite
  composition path.
