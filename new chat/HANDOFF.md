# NEW CHAT HANDOFF — III.AmbientRadio

Repository: https://github.com/Banemanz/III.AmbientRadio
Single draft PR: https://github.com/Banemanz/III.AmbientRadio/pull/1
Branch: `fix/audio-lifecycle-and-windows-ci`
Base: `main` at `74446c86c839f3dd0640e8ec8675fcb2e11fda8f`

## User requirements

- GTA III **1.0 US**, Plugin-SDK **October 25, 2025** pinned commit `624a6a49265fd7a6fc63bda1611013ceabeacb8a`.
- Windows GitHub Actions build and downloadable Win32 ASI + `bass.dll`.
- Fix radio station-switch artifact, car-exit latency, radio reading on foot, bridge announcements, mission passed tune, and NPC-vehicle radio acoustics. Improve performance and inspect other latent bugs.
- **Every source change stays on PR #1**; do not merge without request.

## Confirmed source defects and fixes in this PR

1. `Main.cpp`: `BASS_StreamCreateFile(TRUE, gLoadedBuffer.data(), ...)` borrowed memory from the staging vector. Replacing staging from the detached I/O thread could invalidate active playback. Fixed with separate `gActiveStreamBuffer` retained until `BASS_StreamFree` and generation-tracked background requests.
2. Startup loaded station 0 while the player was on foot. Removed eager `StartLoadingStation(0)`. HUD gated on player in a vehicle.
3. Car exit previously ran after queued-stream playback, announcements and station changes. Now performs exit teardown before any audio processing.
4. `ForceNativeRadioOff` called `cSampleManager::StopStreamedFile(0)` every frame and set `m_bDoTrackService=false`. The game's native streamed channel 0 also plays mission-passed and bridge audio. Removed unconditional stop and track-service suppression.
5. `Switch.cpp`: VC opcode `057D` does **not** exist in GTA III; GTA III has 0x394 mission passed, 0x3D1 parameterless PLAY_MISSION_AUDIO, 0x41E set radio, and bridge broadcasts call `cDMAudio::PlayRadioAnnouncement` at `0x57CC60` (13=COMOPEN, 14=SUBOPEN). Hook correct native function and use optional MP3s or stock WAVs if present, otherwise fall back to `cMusicManager::PlayAnnouncement`.
6. Wheel/key listeners swallowed input when on foot; gate them on actual seated player.
7. Wrong GTA:VC SDT SFX sample IDs caused static/tuning artifacts; now `RadioSwitchSfx=0` defaults off. Old INIs unchanged.
8. `AmbientCar.cpp` had borrowed-memory race and per-frame BASS_ChannelPlay. Separate ready/active buffers, cancel stale worker via generation; only resume from pause, use gentler EQ/compression, smooth volume, pan to nearby vehicles.
9. Audio files now resolve relative to `gta3.exe`, not the parent of the ASI (which can live anywhere under ModLoader).
10. Create default INI with `CreateFileA(CREATE_NEW)` not `ofstream(trunc)` to avoid overwriting an existing unreadable INI.

## Source reference verification

Verified against user-supplied original GTAIII IDB (gta3.exe 1.0 US):
- `0x57CC60` cDMAudio::PlayRadioAnnouncement(unsigned char), prologue `83 EC 08 8B 44 24 0C`
- `0x439485` script ProcessOneCommand CALL `E8 76 00 00 00`
- `0x57CE60` cDMAudio::SetRadioInCar
- `0x57E6D0` cMusicManager::DisplayRadioStationName
- `0x5680E0` cSampleManager::StopStreamedFile(0), previously called every frame.
Cross-checked with re3 MusicManager and ScriptCommands enum. 0x057D is outside GTA III supported script enum (max ~0x482).

## CI

`.github/workflows/build.yml`: Windows 2022, checkout pinned Plugin-SDK, call its `tools/generate/Visual Studio.bat`, MSBuild `plugin_iii` release, MSBuild original `IIIAmbientRadioEXP.vcxproj` Release GTA3/Win32, source regression checks, archive ASI + SDK-provided BASS runtime, upload artifact.
Local native game executable unavailable. CI compilation and regression checks MUST be consulted before calling changes build-verified.
BASS external library licenses apply.

## Next tasks

- Inspect CI run and fix any MSVC errors, repeating within this **same PR**.
- Test gameplay manually with station-change spam, ADF memory streams, large MP3 player folders, immediate car exit/re-enter, police car, mission-passed tune and bridge unlock missions. User will test actual GTA III.
- Check native radio suppression coexistence with original mission announcements (the native fallback may still require game-specific behaviour with music mode). Verify no simultaneous native radio + BASS.
- Consider improving station seeking and per-station audio caching without introducing unbounded RAM.
- Monitor `III.AmbientRadio.log` for BASS failures and async transitions.

## Late-pass improvements and caveats (2026-11-26)

- Player station `CancelPendingStationLoad` and `StartLoadingStation` now increment request generations **under the same mutex** used by workers to publish buffers. Previous implementation could pass the generation check just before cancellation and publish stale audio. Large ADF decrypt loops check for cancellation every 1 MiB.
- The original GTA III PC streamed-name table in re3 lists `AUDIO\\COMOPEN.WAV` and `AUDIO\\SUBOPEN.WAV`; bridge lookup now supports stock WAV plus MP3 overrides.
- Background workers now call `QueueRadioWorkerLog`, with the queue drained from the main gameProcessEvent; avoids concurrent unsynchronized writes to shared `std::ofstream`.
- Idle ambient loop avoids calling StopAmbientStream on every frame; retry logic checks nonzero deadline and signed timer difference so early uptime does not freeze retries.
- MP3 durations are read once and cached with std::call_once; NPC vehicle saved-station map uses `CPools::GetVehicleRef` handles and prunes dead slots.
- CI builds already succeeded for earlier commits, but **check the latest PR-head Actions run before claiming final revision is build-verified**. A green CI build is still not gameplay verification.
- Review: the native announcement fallback without a matching file may be suppressed when the native radio is off, and in-law-enforcement-car playback has not been runtime tested. Do not call bridge implementation fully proven until actual game testing.


## Follow-up: police bleed, ambient inheritance and configuration (2026-11-27)

The tester clarified that police radio is audible **while any custom station is ON**, not only when radio is off. Root cause verified in the GTA III 1.0 US IDB and re3: native radio IDs 0–8 are regular stations, 9 is MP3, **10 is POLICE.WAV and 11 is OFF / CITY.WAV**. The previous native-off code set 10, thus explicitly selecting dispatch beneath BASS. Native tuning is intentionally distinct from custom station indices (custom index 10 can still be a valid station).

- `RadioPolicy.h` centralizes native IDs, script mapping and entry selection. Native police tuning is only legitimate in actual police-radio vehicles (engine's UsesPoliceRadio predicate), unless explicitly [NORADIO] or overridden by a successfully playing optional police.mp3. Bad/missing custom police audio falls back to the native dispatch.
- Verified `GetCarTuning` at 0x57E530, `GetNextCarTuning` at 0x57E5A0. Hook only game-mode CALLs 0x57D7D8 (E8 53 0D 00 00), 0x57D899 (E8 02 0D 00 00), 0x57DC3A (E8 F1 08 00 00); signatures must all match. F9 branch at 0x57D7EC (74 68 -> EB 68) disables stock MP3 shortcut, not custom MP3 selection. No blanket stop of native streamed channel 0 or overwrite of its active track. In game mode ask native service to retune an already-running 0–10 stream, excluding announcements. Keep stock mission/front-end streaming.
- Native 041E script values are mapped, not interpreted as custom indices. 10 never becomes an arbitrary custom station, 11 is OFF, 9 resolves to the optional custom MP3 station.
- New INI and runtime default AmbientRadioDistance=20.0; preserve existing INIs, so manually change old 60.0 to 20.0. HUD uses RADIO OFF. Stock raw SFX sample numbers verified from the IDB/re3: 188 crackle, 343 click; avoid 344–355 (not radio static). Remains disabled by default.
- Vehicle assignment is keyed by generation-safe pool ref plus model. Ambient and player share the assignment, not a global nearest-station variable. Actual NPC assignment wins over auto-tune on entry; both use shared station clock. Player-selected station (including pending selection or explicit OFF) is remembered on exit. NPC radio skips real police, [NORADIO], and folder-only MP3 stations.
- Ambient acquisition scans at 100 ms intervals with 10% hysteresis, while live range/source validity is checked every frame. Async ready buffers are tagged/cancelled with generation + mutex, using shared chunked `RadioFileLoader.h`; retry is per-source, no raw dangling vehicle pointer. Proper source pan/volume before starting. MP3 playlist stream is non-looping so EOF advances. Manual switch cancels a stale forced SCM timecode.
- Local commands: `python tests/verify_source.py`; `g++ -std=c++17 -Wall -Wextra -Werror -O2 tests/test_radio.cpp -o /tmp/radio-test && /tmp/radio-test`. Tests model native IDs/police isolation, real production vehicle assignment, multi-car handoff, OFF/pending exit, recycled refs, MP3-only and ADF decryption/cancellation. CI also runs the portable C++ test under MSVC x86 before building the ASI.
- In-game acceptance: civilian car with every custom station ON, then OFF and F9; no police bleed. Verify genuine police vehicles (stock / optional override), NPC car station identity with auto-tune both ways, 20-unit ambient range, bridge/mission music, pause, re-entry, MP3 EOF. Linux tests are headless, not gameplay. Check the latest PR-head Windows CI and artifact before distributing.


## Follow-up: exit animation and mouse-wheel sounds (2026-11-27)

Tester reported radio continuing until the car door closes, and a car-like sound on wheel-up in an older build. Verified against the original 1.0 US IDB and pinned Plugin-SDK:

- At 0x57E4B0 native `cMusicManager::PlayerInCar` checks CPed state at +0x224, rejecting 51 (drag from car), 54 (exit car) and 56 (arrested). `CPed::SetExitCar` sets EXIT_CAR early, while `PedSetOutCarCB` clears the physical flag later. The mod previously used only m_bInVehicle. The new shared `RadioOccupancy.h` rejects these states plus death and incomplete entry/train transitions; compile-time assertions compare IDs against the exact SDK enum. Player BASS music/SFX teardown and decoder cancellation occurs before exit bookkeeping and pause/resume; the HUD/input/ambient gates use the same state. Ambient cannot misidentify the departing player's car as an NPC source.
- Original `CPad::UpdatePads` calls `CControllerConfigManager::AffectPadFromMouse` at 0x492755 (E8 46 AA 0F 00 -> 0x58D1A0), after UpdateMouse and before pad-state reconciliation. Previously the late gameProcessEvent cleared the raw wheel after its native action had already been generated. Verified signature and patch only this CALL, call the original mapping for every remaining action, and leave wheel alone on foot/in menus. If another mod owns the signature, log and retain the old late capture fallback. SDK `CPad::NewMouseControllerState.wheelUp/wheelDown` replaces raw bytes.
- Both scroll directions are always silent, irrespective of an old RadioSwitchSfx=1. Removed looping raw SDT static (including sample 188); optional keyboard/pad click retains verified GTA III sample 343. Clear stock retune counters even when native station has already switched to ambience. Cannot confirm recorded older sound's specific source without in-game audio; previous VC sample index bug is consistent with car-like noise.
- Regressions cover exit while the physical flag is still set, death/drag/entry states, both wheel directions, one-shot consumption, preserving unowned scroll, legacy SFX, source ordering, and hook signatures. In game, check that music stops as Claude starts to open the door, scroll both directions in a normal car, test the alternate INI value, pause/load, native police cars and on-foot weapon switching.
