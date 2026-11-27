"""Static regression checks for the GTA III 1.0 US audio plugin.

These are deliberately not presented as in-game tests: GitHub Actions has no
GTA III executable, game audio archives or runtime to exercise native hooks.
"""
from pathlib import Path
import re
import struct

root = Path(__file__).resolve().parents[1]
main = (root / "source/Main.cpp").read_text(encoding="utf-8")
ambient = (root / "source/AmbientCar.cpp").read_text(encoding="utf-8")
switch = (root / "source/Switch.cpp").read_text(encoding="utf-8")
project = (root / "IIIAmbientRadioEXP.vcxproj").read_text(encoding="utf-8")
workflow = (root / ".github/workflows/build.yml").read_text(encoding="utf-8")

def require(condition, explanation):
    if not condition:
        raise AssertionError(explanation)

require("gActiveStreamBuffer = std::move(gLoadedBuffer)" in main,
        "Player audio must own memory separately from the worker staging buffer")
require(main.index("BASS_StreamFree(gStream);") < main.index("gActiveStreamBuffer.clear();"),
        "Player stream must be freed before the backing memory")
require("std::call_once(gMp3DurationsOnce" in main and
        "gMp3DurationsMs[i]" in main,
        "MP3 selection must use cached metadata, not reopen every track")
require("gLoadGeneration.fetch_add" in main and
        "gLoadGeneration.load(std::memory_order_acquire)" in main,
        "Player radio needs stale-worker cancellation")
require("StartLoadingStation(0);" not in main,
        "Do not load default radio station when on foot")
require("CREATE_NEW" in main and "std::ios::trunc" not in main,
        "INI must be created once, never truncate an existing file")
require("gRadioSwitchSfx = false" in main,
        "Legacy mismatched switch sounds must be opt-in")
require("if (!gPlayerInVehicle || gStationNameToShow.empty()) return;" in main,
        "Radio banner should not appear on foot")
require("if (!inVehicle) {" in main and
        main.index("// Process exit BEFORE") < main.index("if (gWasInVehicle && gWaitingToPlay"),
        "Exit must run before queued playback")
require("plugin::CallMethod<0x5680E0" not in main,
        "Do not stop shared streamed audio channel zero every frame")
require("COMOPEN.MP3" in main and "SUBOPEN.MP3" in main,
        "Use GTA III bridge broadcast filenames")
require("Hook_PlayRadioAnnouncement" in switch and "0x57CC60" in switch,
        "Hook native GTA III announcement dispatch")
require("if (op != 0x041E && op != 0x0394 && op != 0x03D1)" in switch and
        "op == 0x057D" not in switch,
        "Do not misidentify a nonexistent GTA III opcode")
require(re.search(r'if \(op == 0x03D1\)[\s\S]{0,130}gDialogueDuckUntil', switch),
        "Mission-audio opcode has no arguments")
require("gSwitchNext = gSwitchPrev = gRadioOff = false;" in switch,
        "Radio inputs must be disabled on foot")
require("gAmbientReadyBuffer = std::move(ownedAdfBuffer)" in ambient and
        "gAmbientActiveBuffer = std::move(gAmbientReadyBuffer)" in ambient,
        "Ambient decoder buffer lifetime must survive its stream")
require("gAmbientGeneration.fetch_add" in ambient,
        "Ambient workers must be cancellable")
require("gAmbientWasPaused" in ambient and
        "gAmbientLastVolTick" in ambient and "BASS_ATTRIB_PAN" in ambient,
        "Ambient playback should resume once and have stereo positioning")
require("$(PLUGIN_SDK_DIR)\\shared\\bass" in project,
        "MSBuild must find pinned SDK BASS library")
require("624a6a49265fd7a6fc63bda1611013ceabeacb8a" in workflow and
        "windows-2022" in workflow,
        "CI must use the pinned Oct 25, 2025 Plugin-SDK on Windows")

binary = root / "bin/GTA3/Release/III.AmbientRadio.asi"
if binary.exists():
    data = binary.read_bytes()
    require(data[:2] == b"MZ", "ASI must be a PE DLL")
    off = struct.unpack_from("<I", data, 0x3C)[0]
    require(data[off:off + 4] == b"PE" + bytes((0, 0)), "Missing PE signature")
    machine = struct.unpack_from("<H", data, off + 4)[0]
    require(machine == 0x14C, "ASI must be x86 (GTA III 1.0 US)")
print("PASS: static hook/stream lifetime/INI/CI checks" +
      (" and x86 PE build" if binary.exists() else " (source only)"))
