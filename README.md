# ZD Stomp Host

A 15-slot multi-effects rack audio plugin (VST3 / AU) inspired by Zoom MultiStomp pedals, built with the [DISTRHO Plugin Framework (DPF)](https://github.com/DISTRHO/DPF).

## Features

- **15-Slot ZDL Effect Rack**: Loads Zoom `.zdl`/`.zd2` effect metadata (name, parameters, ranges) and processes audio with a C++ multi-effect DSP engine (`UniversalZdlDSP`).
- **Serial & Dual-Chain Routing**: Run all 15 slots in series, or enable Dual Mode (Slots 1–5 Pre-FX, Slots 6–10 Chain A, Slots 11–15 Chain B with blendable A/B mix).
- **Quad IR Cabinet Loader**: 4-channel FFT overlap-add convolution engine supporting WAV impulse responses with per-channel level, pan, high-pass, and low-pass filters.
- **Chromatic Tuner**: Built-in pitch detector with mute/pass-through modes.
- **MIDI Learn & Rig Presets**: Map MIDI CC/Note/Program Change messages to controls and save/load `.zrig` presets.

## Building from Source

### Prerequisites

- **DISTRHO Plugin Framework (DPF)** cloned **recursively** (required for its `pugl` submodule):
  ```sh
  git clone --depth 1 --recursive https://github.com/DISTRHO/DPF.git ../DPF
  ```
- C++17 compiler and GNU Make (`Xcode Command Line Tools` on macOS, MinGW-w64 on Windows).

### macOS (VST3 & AU)

Build for the current architecture:

```sh
make -j"$(sysctl -n hw.ncpu)" vst3 au
```

Build a **Universal Binary (`arm64` + `x86_64`, macOS 11.0+)**:

```sh
make -j"$(sysctl -n hw.ncpu)" \
  CFLAGS="-arch x86_64 -arch arm64 -mmacosx-version-min=11.0" \
  CXXFLAGS="-arch x86_64 -arch arm64 -mmacosx-version-min=11.0" \
  PKG_CONFIG=/usr/bin/false \
  PKG_CONFIG_PATH=/NOT \
  vst3 au
```

If DPF is located elsewhere, pass `DPF_PATH=/path/to/DPF`.

The built bundles are placed in `bin/`:
- `bin/ZDStompHost.vst3` → copy to `~/Library/Audio/Plug-Ins/VST3/`
- `bin/ZDStompHost.component` → copy to `~/Library/Audio/Plug-Ins/Components/`

Ad-hoc signing (recommended on Apple Silicon):

```sh
codesign --force --deep --sign - bin/ZDStompHost.vst3
codesign --force --deep --sign - bin/ZDStompHost.component
```

Optional AU validation after installing to `~/Library/Audio/Plug-Ins/Components/`:

```sh
auval -v aumf ZMSR ZmEp
```

### Windows (VST3)

With MinGW-w64 and DPF in `../DPF`:

```sh
make vst3
```

## Preset Directory

Rig presets (`.zrig`) are stored by default in:
- **macOS**: `~/Documents/ZDStompHost/Presets`
- **Windows**: `%USERPROFILE%\Documents\ZDStompHost\Presets`

## CI Builds

The GitHub Actions workflow in `.github/workflows/macos-build.yml` automatically builds macOS Universal (arm64 + x86_64) VST3 and AU bundles and uploads them as workflow artifacts.
