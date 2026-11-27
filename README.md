# III.AmbientRadio

GTA III 1.0 US ASI for custom in-car radio and nearby NPC-vehicle radio audio using [BASS](https://www.un4seen.com/bass.html).

## Build (Windows / GitHub Actions)

CI builds **Win32 x86** with Visual Studio 2022. It checks out Plugin-SDK revision `624a6a49265fd7a6fc63bda1611013ceabeacb8a` (October 25, 2025), builds `plugin_iii.lib`, then builds the original project. BASS headers, import library and runtime come from `Plugin-SDK/shared/bass`. No game executable or radio tracks are included.

The Actions artifact contains `III.AmbientRadio.asi` and `bass.dll`. For local builds set `PLUGIN_SDK_DIR` and build **Release GTA3 | Win32** using Visual Studio 2022.

## Installation

Place `III.AmbientRadio.asi` and `bass.dll` in GTA III's `scripts` folder (or another ASI-loader directory). The mod generates `III.AmbientRadio.ini` if missing and keeps existing settings. Station paths are relative to the GTA III executable folder; supply your own station audio.

`[SETTINGS]` includes ambient-radio EQ/compression/reverb options, `ScriptIntegration`, and `RadioSwitchSfx` (disabled by default to avoid old mismatched sample indices). `[CONTROLS]` configures keyboard/controller radio switching. Existing INIs remain compatible.

## Limits

Built for **GTA III 1.0 US**. GitHub Actions checks compilation and static tests; gameplay sound, mission events, and interactions with other audio mods require in-game testing. Please check `new chat/HANDOFF.md` for technical details and test cases.

BASS is a third-party library from Un4seen; review its [license](https://www.un4seen.com/bass.html) for your distribution/use.
