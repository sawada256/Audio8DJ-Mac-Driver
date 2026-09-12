# Audio 8 DJ Mac Driver

[日本語](README.md) · [Installation](docs/installation.md) · [Troubleshooting](docs/troubleshooting.md)

An unofficial audio and MIDI driver for the Native Instruments **Audio 8 DJ**, primarily for Apple Silicon Macs. This project is not an official or endorsed Native Instruments product.

## Install a release

From this repository's **Releases** section on GitHub, download `Audio8DJ-Mac-Driver-0.1.0-unsigned.pkg` or `Audio8DJ-Mac-Driver-0.1.0.zip`. The publisher attaches these generated files to a release.

1. Save your work and quit your DAW. Installation briefly interrupts all system audio.
2. Open the `.pkg`, or extract the complete ZIP and run `install.command`.
3. Connect the interface, wait a few seconds, and select **Audio 8 DJ** in System Settings → Sound → Output or your DAW.
4. For MIDI, review [background activity and permissions](docs/permissions.md).

**Prebuilt releases need no compiler, Xcode, Python, or rebuilding.** GitHub's “Code → Download ZIP” contains source, not the prebuilt installer.

The installers are not Developer ID signed or Apple notarized. macOS may require confirmation after downloading. Follow [installation instructions](docs/installation.md).

## Features and requirements

- 8 inputs and 8 outputs at 44.1, 48, 88.2, and 96 kHz.
- Float32 CoreAudio streams with 24-bit USB audio.
- Audio device discovery and removal follow USB presence.
- MIDI input/output through a per-user background agent.
- Bundled firmware is loaded into device RAM when needed.

The build targets macOS 12 or later and produces arm64 / x86_64 Universal binaries. Apple Silicon is the primary target. See [compatibility](docs/compatibility.md) for validation limits; support for every Mac, OS version, and DAW is not claimed.

The menu-bar Sound list may omit the device even when System Settings can select it and play audio. Actual USB MIDI transport requires an active audio stream. Virtual MIDI ports remain visible while the device is disconnected.

## Build from source

On macOS with Xcode Command Line Tools and Python 3:

```sh
make build
make test
make release
```

Build outputs go to `build/`; verified installers and checksums go to `dist/`. All source dependencies are included; no submodules or Git LFS are needed.

## Documentation

The guides include Japanese and English: [installation](docs/installation.md), [permissions](docs/permissions.md), [troubleshooting](docs/troubleshooting.md), [compatibility](docs/compatibility.md), [development](docs/development.md), and [releasing](docs/releasing.md). See [CHANGELOG](CHANGELOG.md) for releases.

## License

Original code and documentation are [MIT licensed](LICENSE). Bundled device firmware is excluded from MIT; rights remain with its respective owners. Read [third-party notices](THIRD_PARTY_NOTICES.md) and [firmware information](firmware/README.md).
