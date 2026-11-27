#include "plugin.h"
#include "CPad.h"
#include "CPlayerPed.h"
#include "RadioPolicy.h"
#include <cstring>
#include "cDMAudio.h"
#include "cMusicManager.h"
#include "CRunningScript.h"
#include "CTheScripts.h"
#include "CGame.h"
#include <fstream>
#include <string>
#include <atomic>
#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "Xinput.lib")

using namespace plugin;

// Defined in Main.cpp — shared debug log.
extern std::ofstream gLog;

// Shared variables — read by Main.cpp
bool gSwitchNext = false;
bool gSwitchPrev = false;

// Set true when the radio-switch button (keyboard or pad) is HELD ~2.5s — Main.cpp
// then turns the radio off (the same "Radio Off" state as scrolling past the last
// station). A quick tap of the same button still just changes station.
bool gRadioOff = false;

// Set by the SCM opcode hook below when the game's script fires opcode
// Native GTA III bridge announcements: 13 = COMOPEN, 14 = SUBOPEN.
 // -1 = nothing pending; 0 = COMOPEN, 1 = SUBOPEN.
// Read (and reset) by Main.cpp on the main thread. The hook runs on the same
// (main) thread during script processing, so this is never contended, but
// std::atomic keeps the access well-defined.
std::atomic<int> gPendingAnnouncement{ -1 };

// Set by the same hook when main.scm fires opcode 041E SET_RADIO_CHANNEL
// (missions that force a specific car-radio station). Holds the requested
// station index; -1 = nothing pending. Applied to the BASS radio by Main.cpp.
std::atomic<int> gPendingScmStation{ -1 };

// The second parameter of 041E: the requested playback position in ms, or -1
// meaning "continue" (use our synced clock). Main.cpp honors this only for the
// original III stations (indices 0..8); added stations always use the clock.
std::atomic<int> gPendingScmStationTime{ -1 };

// GetTickCount() deadlines until which Main.cpp ducks (dims) the radio, matching
// the original game's audio ducking. gMissionPassedDuckUntil is set by the
// "mission passed" tune (opcode 0394); gDialogueDuckUntil is refreshed by each
// mission dialogue line (opcode 03D1 PLAY_MISSION_AUDIO).
std::atomic<DWORD> gMissionPassedDuckUntil{ 0 };
std::atomic<DWORD> gDialogueDuckUntil{ 0 };

// CPad::NewMouseControllerState.wheelUp / .wheelDown  (current-frame mouse state).
// The vehicle audio code reads these directly to detect scroll-wheel radio changes.
// OldMouseControllerState (0x93690B/C) is the previous-frame copy — that is what
// was being cleared before, which had no effect on the current frame's input.
static uint8_t* const pMouseWheelUp   = (uint8_t*)0x8809F3;
static uint8_t* const pMouseWheelDown = (uint8_t*)0x8809F4;

// Configurable keys
static int gRadioSwitchNextKey = 82;  // keyboard: R by default
static int gRadioSwitchNextPad = 0;   // controller: 0 = disabled

// [SETTINGS] ScriptIntegration — when false, the SCM opcode mid-hook is NOT
// installed, so the plugin never touches the game's script engine: no story
// announcements, no mission radio-station changes, and no audio ducking. The
// core radio (stations, vehicle assignment, ambient, suppression, volume) is
// unaffected. This exists for total-conversion mods whose custom main.scm has
// different opcode / mission-audio behavior and crashes under our interception.
// Default true (no behavior change for stock/standard installs).
static bool gScriptIntegrationEnabled = true;

static void LoadControlsFromINI()
{
    char path[MAX_PATH];
    HMODULE hModule = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&LoadControlsFromINI, &hModule);
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string scriptsFolder = std::string(path);
    scriptsFolder = scriptsFolder.substr(0, scriptsFolder.find_last_of("\\/") + 1);

    std::string iniPath = scriptsFolder + "III.AmbientRadio.ini";
    std::ifstream ini(iniPath, std::ios::binary);
    if (!ini.is_open())
        return;

    // Handle BOM
    unsigned char bom[3] = {};
    ini.read((char*)bom, 3);
    if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF))
        ini.seekg(0);

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end = s.find_last_not_of(" \t\r\n");
        s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
        };

    bool inControlsSection = false;
    std::string line;

    while (std::getline(ini, line))
    {
        size_t comment = line.find('#');
        if (comment != std::string::npos)
            line = line.substr(0, comment);

        trim(line);
        if (line.empty())
            continue;

        if (line[0] == '[') {
            std::string header = line;
            std::transform(header.begin(), header.end(), header.begin(), ::tolower);
            trim(header);
            inControlsSection = (header == "[controls]");
            continue;
        }

        if (!inControlsSection)
            continue;

        size_t sep = line.find('=');
        if (sep == std::string::npos)
            continue;

        std::string key = line.substr(0, sep);
        std::string val = line.substr(sep + 1);
        trim(key);
        trim(val);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        if (key == "radioswitchnext" && !val.empty())
            gRadioSwitchNextKey = atoi(val.c_str());
        else if (key == "radioswitchnextpad" && !val.empty())
            gRadioSwitchNextPad = atoi(val.c_str());
    }
}

// Reads [SETTINGS] ScriptIntegration (default true). Called from the static
// plugin constructor BEFORE Main.cpp's gLog is guaranteed to be open, so it must
// not touch gLog. Self-contained INI scan, mirroring LoadControlsFromINI's
// folder lookup and parsing.
static bool ReadScriptIntegrationFlag()
{
    char path[MAX_PATH];
    HMODULE hModule = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&ReadScriptIntegrationFlag, &hModule);
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string scriptsFolder = std::string(path);
    scriptsFolder = scriptsFolder.substr(0, scriptsFolder.find_last_of("\\/") + 1);

    std::ifstream ini(scriptsFolder + "III.AmbientRadio.ini", std::ios::binary);
    if (!ini.is_open())
        return true; // default: enabled

    unsigned char bom[3] = {};
    ini.read((char*)bom, 3);
    if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF))
        ini.seekg(0);

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end = s.find_last_not_of(" \t\r\n");
        s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
        };

    bool inSettings = false;
    std::string line;
    while (std::getline(ini, line))
    {
        size_t comment = line.find('#');
        if (comment != std::string::npos)
            line = line.substr(0, comment);
        trim(line);
        if (line.empty())
            continue;

        if (line[0] == '[') {
            std::string header = line;
            std::transform(header.begin(), header.end(), header.begin(), ::tolower);
            inSettings = (header == "[settings]");
            continue;
        }
        if (!inSettings)
            continue;

        size_t sep = line.find('=');
        if (sep == std::string::npos)
            continue;
        std::string key = line.substr(0, sep);
        std::string val = line.substr(sep + 1);
        trim(key);
        trim(val);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        if (key == "scriptintegration" && !val.empty())
            return atoi(val.c_str()) != 0;
    }
    return true; // default: enabled
}

// Block game's radio switch button (keyboard / controller)
static bool __fastcall Hook_ChangeStationJustDown(CPad* self, void* edx)
{
    return false;
}

// Suppression of the native radio setter cDMAudio::SetRadioInCar (0x57CE60) and
// stock radio-name drawing (cMusicManager::DisplayRadioStationName, 0x57E6D0).
//
// GTA III does not have VC's standalone vehicle radio process at the VC address.
// The previous port accidentally patched cMusicManager::ServiceGameMode, which is
// III's main music service. Leave the music service alone; only block writes into
// the native radio and the stock station-name banner.
static BYTE gOrigSetRadioInCar[5];
static BYTE gOrigDisplayRadioStationName[5];
static bool gNativeAudioAllowed = false; // false = patches applied (suppressed)

extern bool gPlayerInVehicle; // defined in Main.cpp; updated every frame
bool ShouldSuppressNativeVehicleRadio(CVehicle* vehicle);
extern std::string gGameFolder;

// Patch targets — never called while the patches are lifted.
static void __fastcall Hook_SetRadioInCar(cDMAudio* self, void* edx, unsigned int radio)
{
    // Intentionally empty.
}

static void __fastcall Hook_DisplayRadioStationName(cMusicManager* self, void* edx)
{
    // Intentionally empty.
}

static void ApplyRadioSuppression(bool suppress)
{
    if (suppress) {
        injector::MakeJMP(0x57CE60, (void*)Hook_SetRadioInCar, true);
        injector::MakeJMP(0x57E6D0, (void*)Hook_DisplayRadioStationName, true);
    }
    else {
        injector::WriteMemoryRaw(0x57CE60, gOrigSetRadioInCar, sizeof(gOrigSetRadioInCar), true);
        injector::WriteMemoryRaw(0x57E6D0, gOrigDisplayRadioStationName, sizeof(gOrigDisplayRadioStationName), true);
    }
    gNativeAudioAllowed = !suppress;
}

// Only replace native car-tuning calls inside ServiceGameMode, not the music
// service, stream manager, or mission/front-end sound functions. Calling the
// original methods is safe because these hooks patch their CALLs, not entries.
static bool SuppressCurrentCarTuning()
{
    CPlayerPed* player = FindPlayerPed();
    return player && player->m_bInVehicle && player->m_pVehicle &&
        ShouldSuppressNativeVehicleRadio(player->m_pVehicle);
}
static unsigned char __fastcall Hook_GetCarTuning(cMusicManager* self, void*)
{
    if (SuppressCurrentCarTuning()) return RadioPolicy::NativeOff;
    return self->GetCarTuning();
}
static unsigned char __fastcall Hook_GetNextCarTuning(cMusicManager* self, void*)
{
    if (SuppressCurrentCarTuning()) return RadioPolicy::NativeOff;
    return self->GetNextCarTuning();
}
static bool gNativeTuningHooksInstalled = false;
static bool gNativeF9Suppressed = false;
static void InstallNativeTuningHooks()
{
    // Verified against the original GTA III 1.0 US IDB. Never blindly overwrite
    // another plugin's hooks at these callsites.
    const unsigned addresses[] = { 0x57D7D8, 0x57D899, 0x57DC3A };
    const BYTE originals[][5] = {
        {0xE8,0x53,0x0D,0x00,0x00}, {0xE8,0x02,0x0D,0x00,0x00},
        {0xE8,0xF1,0x08,0x00,0x00}
    };
    bool valid = true;
    for (int i = 0; i < 3; ++i) {
        BYTE actual[5]{};
        injector::ReadMemoryRaw(addresses[i], actual, sizeof(actual), true);
        if (std::memcmp(actual, originals[i], sizeof(actual)) != 0) valid = false;
    }
    if (valid) {
        injector::MakeCALL(addresses[0], Hook_GetCarTuning, true);
        injector::MakeCALL(addresses[1], Hook_GetNextCarTuning, true);
        injector::MakeCALL(addresses[2], Hook_GetCarTuning, true);
        gNativeTuningHooksInstalled = true;
    }
    // F9 independently forces native MP3 directly, bypassing the normal radio
    // switch functions. Skip ONLY this game-mode shortcut; the BASS MP3 PLAYER
    // is still selectable through the mod's original controls.
    BYTE f9[2]{};
    injector::ReadMemoryRaw(0x57D7EC, f9, sizeof(f9), true);
    if (f9[0] == 0x74 && f9[1] == 0x68) {
        BYTE skip[] = {0xEB,0x68};
        injector::WriteMemoryRaw(0x57D7EC, skip, sizeof(skip), true);
        gNativeF9Suppressed = true;
    }
}

// ---- Native GTA III bridge-announcement interception ----
// GTA III does not have VC's opcode 057D. The industrial/commercial-passed
// opcodes call cDMAudio::PlayRadioAnnouncement(13/14) at 0x57CC60.
// If the original COMOPEN.MP3 or SUBOPEN.MP3 exists, queue it for BASS so
// it plays through our own in-vehicle radio. If absent, retain the game's
// original cMusicManager path (no silent drop or fabricated VC filenames).
static void __fastcall Hook_PlayRadioAnnouncement(cDMAudio* self, void*, unsigned char announcement)
{
    if (announcement == RadioPolicy::NativePolice && SuppressCurrentCarTuning()) return;
    if (announcement == 13 || announcement == 14) {
        const char* basename = (announcement == 13) ? "COMOPEN" : "SUBOPEN";
        // Stock GTA III PC ships WAV bridge announcements; accept MP3
        // replacements without requiring a separate modded audio archive.
        std::string mp3 = gGameFolder + "audio\\" + basename + ".MP3";
        std::string wav = gGameFolder + "audio\\" + basename + ".WAV";
        DWORD attributesMp3 = GetFileAttributesA(mp3.c_str());
        DWORD attributesWav = GetFileAttributesA(wav.c_str());
        bool hasMp3 = attributesMp3 != INVALID_FILE_ATTRIBUTES &&
            !(attributesMp3 & FILE_ATTRIBUTE_DIRECTORY);
        bool hasWav = attributesWav != INVALID_FILE_ATTRIBUTES &&
            !(attributesWav & FILE_ATTRIBUTE_DIRECTORY);
        if (hasMp3 || hasWav) {
            gPendingAnnouncement.store(announcement == 13 ? 0 : 1, std::memory_order_release);
            return;
        }
    }
    gMusicManager.PlayAnnouncement(announcement);
}

// ---- SCM opcode interception: mission station changes + dialogue/tune ducking ----
//
// 041E SET_RADIO_CHANNEL: game script forces an in-car station and timecode.
 // 0394 PLAY_MISSION_PASSED_TUNE: native stream; only duck BASS.
 // 03D1 PLAY_MISSION_AUDIO: parameterless; only duck BASS.
 // GTA III bridge openings are handled by the separate native hook above.
//
// We hook the single CRunningScript::Process call site that dispatches
// ProcessOneCommand. The wrapper peeks at the current opcode, then calls the
// original method through plugin-sdk; the script engine still runs unmodified.
// At the call site, the thiscall 'this' pointer is still in ECX.
// Reads one SCM int parameter at ss[*off] and advances *off past it. Resolves
// immediates AND global/local script variables (so a mission passing the radio
// timecode in a variable is read correctly). Returns false only for parameter
// types we can't interpret. *typeOut receives the raw parameter type byte.
static bool ReadScmInt(CRunningScript* self, const unsigned char* ss, int* off, int* out, unsigned char* typeOut)
{
    unsigned char type = ss[*off];
    *off += 1;
    if (typeOut) *typeOut = type;
    switch (type) {
    case SCRIPTPARAM_STATIC_INT_32BITS:  *out = *(const int*)(ss + *off);          *off += 4; return true;
    case SCRIPTPARAM_STATIC_INT_16BITS:  *out = *(const short*)(ss + *off);        *off += 2; return true;
    case SCRIPTPARAM_STATIC_INT_8BITS:   *out = *(const signed char*)(ss + *off);  *off += 1; return true;
    case SCRIPTPARAM_STATIC_FLOAT:       *out = (int)*(const float*)(ss + *off);   *off += 4; return true;
    case SCRIPTPARAM_GLOBAL_NUMBER_VARIABLE: {
        unsigned short goff = *(const unsigned short*)(ss + *off); *off += 2;
        *out = *(const int*)(ss + goff); // globals live in the script space
        return true;
    }
    case SCRIPTPARAM_LOCAL_NUMBER_VARIABLE: {
        unsigned short idx = *(const unsigned short*)(ss + *off); *off += 2;
        if (self && idx < 16) { *out = self->m_aLocalVars[idx].iParam; return true; }
        return false;
    }
    default: return false;
    }
}

static void PeekScmCommand(CRunningScript* self)
{
    if (!self)
        return;

    int ip = self->m_nIp;
    if (ip < 0)
        return;

    const unsigned char* ss = CTheScripts::ScriptSpace;
    // Opcode is the low 15 bits; high bit is the "not" flag.
    unsigned short op = (*(const unsigned short*)(ss + ip)) & 0x7FFF;
    if (op != 0x041E && op != 0x0394 && op != 0x03D1)
        return;

    if (op == 0x0394) {          // PLAY_MISSION_PASSED_TUNE -> native channel 0
        gMissionPassedDuckUntil = GetTickCount() + 5000;
        return;
    }
    if (op == 0x03D1) {          // PLAY_MISSION_AUDIO has NO opcode parameters
        gDialogueDuckUntil = GetTickCount() + 5000;
        return;
    }

    int off = ip + 2;            // first parameter (skip the 2-byte opcode)
    int first;
    if (!ReadScmInt(self, ss, &off, &first, nullptr))
        return;                  // unsupported param — leave it to the game

    // 041E SET_RADIO_CHANNEL: param 1 = station index, param 2 = play timecode
    // in ms (-1 = continue). Decode param 2 (immediate OR variable); it stays -1
    // if the parameter type is unsupported (we then fall back to our synced clock).
    int timecode = -1;
    ReadScmInt(self, ss, &off, &timecode, nullptr);
    gPendingScmStationTime = timecode; // set time first, station last (the signal)
    gPendingScmStation = first;
}


static char __fastcall Hook_ProcessOneCommand(CRunningScript* self, void*)
{
    PeekScmCommand(self);
    return self->ProcessOneCommand();
}
class SwitchDetectorPlugin
{
public:
    SwitchDetectorPlugin()
    {
        injector::MakeJMP(0x493870, (void*)Hook_ChangeStationJustDown, true);
        // Save the original prologues, then suppress the native radio. The saved
        // bytes let gameProcessEvent lift the patches while the player is on foot
        // in an interior (so interior ambient music plays) and re-apply on exit.
        injector::ReadMemoryRaw(0x57CE60, gOrigSetRadioInCar, sizeof(gOrigSetRadioInCar), true);
        injector::ReadMemoryRaw(0x57E6D0, gOrigDisplayRadioStationName, sizeof(gOrigDisplayRadioStationName), true);
        ApplyRadioSuppression(true);
        InstallNativeTuningHooks();

        // Watch the sole CRunningScript::Process -> ProcessOneCommand call for
        // opcodes 041E (mission radio-station changes),
        // 0394/03D1 (audio ducking). We patch the call site instead of using
        // safetyhook, which keeps this compatible with plugin-sdk generator
        // projects that already include their own safetyhook amalgamation.
        gScriptIntegrationEnabled = ReadScriptIntegrationFlag();
        if (gScriptIntegrationEnabled) {
            injector::MakeCALL(0x439485, Hook_ProcessOneCommand, true);
            injector::MakeJMP(0x57CC60, (void*)Hook_PlayRadioAnnouncement, true);
        }

        Events::initGameEvent.Add([]()
            {
                LoadControlsFromINI();
                if (gLog.is_open()) {
                    gLog << "ScriptIntegration (announcements / mission-radio / ducking): "
                         << (gScriptIntegrationEnabled ? "ENABLED"
                                                       : "DISABLED (SCM hook not installed)")
                         << std::endl;
                    gLog << "RadioHooks: Native OFF=11, police=10; car tuning hooks "
                        << (gNativeTuningHooksInstalled ? "ACTIVE" : "signature mismatch (not installed)")
                        << ", native F9 shortcut " << (gNativeF9Suppressed ? "disabled" : "unchanged") << std::endl;
                    gLog << "RadioHooks: native radio suppression; GTA III bridge hook at 0x57CC60 "
                         << (gScriptIntegrationEnabled ? "ACTIVE" : "DISABLED") << std::endl;
                    gLog.flush();
                }
            });

        Events::gameProcessEvent.Add([]()
            {
                // Native radio pass-through toggle: lift the suppression patches
                // only while the player is on foot inside an interior (clubs and
                // shops play their ambient music through the patched functions);
                // re-apply them everywhere else. Transitions are rare and happen
                // on the main thread — the same thread that runs those functions.
                bool allowNative = false; // GTA III has no VC-style currArea interior radio pass-through.
                if (allowNative != gNativeAudioAllowed)
                    ApplyRadioSuppression(!allowNative);

                // Only consume radio input while actually seated. On foot the wheel
                // is used for weapon switching; clearing it there broke the controls
                // and left deferred station changes visible outside vehicles.
                static bool gBtnWasDown = false;
                static DWORD gBtnDownTick = 0;
                static bool gHoldFired = false;
                CPlayerPed* player = FindPlayerPed();
                bool inVehicle = player && player->m_bInVehicle && player->m_pVehicle;
                if (!inVehicle) {
                    gSwitchNext = gSwitchPrev = gRadioOff = false;
                    gBtnWasDown = false;
                    gHoldFired = false;
                    return;
                }
                if (*pMouseWheelUp) {
                    gSwitchNext = true;
                    *pMouseWheelUp = 0;
                }
                if (*pMouseWheelDown) {
                    gSwitchPrev = true;
                    *pMouseWheelDown = 0;
                }

                // Radio-switch button (keyboard key OR controller button): a quick
                // TAP changes station; a HOLD of ~2.5s turns the radio OFF (same as
                // scrolling past the last station). The switch fires on RELEASE so a
                // tap and a hold can be told apart. (Mouse scroll above stays tap-only.)
                bool keyDown = (GetAsyncKeyState(gRadioSwitchNextKey) & 0x8000) != 0;
                bool padDown = false;
                if (gRadioSwitchNextPad != 0) {
                    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
                        XINPUT_STATE state = {};
                        if (XInputGetState(i, &state) == ERROR_SUCCESS &&
                            (state.Gamepad.wButtons & gRadioSwitchNextPad)) {
                            padDown = true;
                            break;
                        }
                    }
                }
                bool btnDown = keyDown || padDown;

                const DWORD RADIO_OFF_HOLD_MS = 2500;

                if (btnDown && !gBtnWasDown) {          // press started
                    gBtnDownTick = GetTickCount();
                    gHoldFired = false;
                }
                else if (btnDown && gBtnWasDown) {      // still held
                    if (!gHoldFired && GetTickCount() - gBtnDownTick >= RADIO_OFF_HOLD_MS) {
                        gRadioOff = true;               // held long enough -> off (once)
                        gHoldFired = true;
                    }
                }
                else if (!btnDown && gBtnWasDown) {     // released
                    if (!gHoldFired)
                        gSwitchNext = true;             // it was a tap -> change station
                }
                gBtnWasDown = btnDown;
            });
    }
} switchDetectorPlugin;
