# TextDraw Anti-Aliasing Fix

`TextDrawAntiAliasingFix.asi` is a GTA San Andreas plugin that supersamples
the SA-MP model preview textdraws.

SA-MP renders preview models into a small off-screen texture that driver
anti-aliasing never reaches, so vehicle and skin previews stay jagged while
the scene behind them is smooth. The plugin renders that pass at a multiple
of the preview's resolution and scales it back down, and can enlarge the
preview texture itself. The framing the server asked for is unchanged.

## Features

- `2x`, `4x` or `8x` supersampling of the model preview.
- Optional `2x`, `4x` or `8x` enlargement of the 256x256 preview texture.
- Falls back to a lower factor when video memory runs out.
- Verifies the bytes it replaces before writing and refuses to patch any
  other executable.
- Creates the default INI when it is missing.

## Requirements

- GTA San Andreas 1.0 US (Compact or Hoodlum executable).
- An ASI loader, such as Silent's ASI Loader or Ultimate ASI Loader.
- SA-MP. Verified against the `0.3.7-R1` client; other builds are expected
  to work but were not inspected.
- A Direct3D 9 device with enough video memory for a render target of
  `previewScale * supersample * 256` pixels a side.

Other executables are left untouched.

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

Settings are read once when the game starts. The two multipliers compound:
the defaults render at 2048x2048 for a 512x512 preview texture.

## Release Integrity

Releases are built by GitHub Actions from the tagged commit and carry a
SHA-256 file and a build-provenance attestation:

```text
gh attestation verify TextDrawAntiAliasingFix-vX.Y.Z.zip -R sonochiwa/sa-textdraw-anti-aliasing-fix
```

## License

MIT. See [LICENSE](LICENSE).
