# Changelog

## 1.0.0

- Added `2x`, `4x` or `8x` supersampling of the SA-MP model preview render pass
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
- Added an `Alt + T` toggle that enables and disables the fix in a running game,
  reloads `supersample` when it turns back on, and persists the state to
  `isEnabled`.
- Added an optional diagnostic log and preview dumps for measuring what the
  plugin intercepted.
- Preserved the preview camera, its projection, its clear colour and the sprite
  composition path.
