#ifdef RADIO_TEST_HOST
#include "../tests/mock_game.h"
#else
#include "plugin.h"
#include "CPlayerPed.h"
#include "CVehicle.h"
#include "CPools.h"
#include "cMusicManager.h"
#include <windows.h>
#endif
#include "RadioPolicy.h"
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cstdlib>
#include <cctype>

using namespace plugin;

// Declared in Main.cpp
struct RadioStation {
    std::string name;
    std::string file;
};
extern std::vector<RadioStation> stations;
extern int gCurrentStation;
extern int gPendingStation;
extern std::string gScriptsFolder;
extern std::ofstream gLog;

// The last station the player was listening to (set when leaving a vehicle).
// Used only by RadioAutoTune to carry that station into the next vehicle.
static int gLastStation = RadioPolicy::Unassigned;

// RadioAutoTune=1 carries over the player's last station only when the car
// hasn't already been assigned an NPC broadcast. Default behavior is per car.
static bool gRadioAutoTune = false;

// The game's pool handle includes the slot generation, unlike a raw pointer.
// Recycled vehicles of the SAME model must not inherit a previous vehicle's
// station simply because the pool reused the same address.
struct SavedStation { int station; int modelId; bool npcAssigned; };
static std::map<int, SavedStation> gVehicleSavedStation;

// Vehicle model ID -> list of station indices (pick randomly if multiple)
std::map<int, std::vector<int>> gVehicleStationMap;

// Vehicle model IDs that should have NO radio at all (from [NORADIO] section).
// Takes precedence over everything: no music, no police radio, no announcements,
// no ambient broadcast from this model.
std::set<int> gNoRadioVehicles;

bool IsNoRadioVehicle(int modelId)
{
    return gNoRadioVehicles.find(modelId) != gNoRadioVehicles.end();
}

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    return out;
}

static void LoadVehicleSection()
{
    gVehicleSavedStation.clear();
    gVehicleStationMap.clear();
    gNoRadioVehicles.clear();
    gLastStation = RadioPolicy::Unassigned;
    gRadioAutoTune = false;

    std::string iniPath = gScriptsFolder + "III.AmbientRadio.ini";
    std::ifstream ini(iniPath);
    if (!ini.is_open()) {
        gLog << "RadioVehicles: cannot open III.AmbientRadio.ini" << std::endl;
        gLog.flush();
        return;
    }

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end = s.find_last_not_of(" \t\r\n");
        s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
        };

    bool inVehicleSection = false;
    bool inNoRadioSection = false;
    bool inSettingsSection = false;
    int mapped = 0;
    std::string line;

    while (std::getline(ini, line))
    {
        // Strip inline comments
        size_t comment = line.find('#');
        if (comment != std::string::npos)
            line = line.substr(0, comment);

        trim(line);
        if (line.empty())
            continue;

        if (!line.empty() && line[0] == '[') {
            std::string header = line;
            std::transform(header.begin(), header.end(), header.begin(), ::tolower);
            inVehicleSection = (header == "[vehicles]");
            inNoRadioSection = (header == "[noradio]");
            inSettingsSection = (header == "[settings]");
            continue;
        }

        // [SETTINGS]: read the RadioAutoTune toggle (AmbientRadio etc. are read
        // separately by Main.cpp's own INI loader).
        if (inSettingsSection) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                trim(key);
                trim(val);
                if (ToLower(key) == "radioautotune")
                    gRadioAutoTune = (atoi(val.c_str()) != 0);
            }
            continue;
        }

        // [NORADIO]: one numeric vehicle model ID per line = total radio silence
        if (inNoRadioSection) {
            int modelId = atoi(line.c_str());
            if (modelId > 0) {
                gNoRadioVehicles.insert(modelId);
                gLog << "RadioVehicles: model " << modelId << " set to NO RADIO" << std::endl;
            }
            continue;
        }

        if (!inVehicleSection)
            continue;

        size_t sep = line.find('|');
        if (sep == std::string::npos)
            continue;

        std::string modelStr = line.substr(0, sep);
        std::string stationName = line.substr(sep + 1);
        trim(modelStr);
        trim(stationName);

        if (modelStr.empty() || stationName.empty())
            continue;

        int modelId = atoi(modelStr.c_str());
        if (modelId <= 0)
            continue;

        std::string nameLower = ToLower(stationName);
        int stationIndex = -1;
        for (int i = 0; i < (int)stations.size(); i++) {
            if (ToLower(stations[i].name) == nameLower) {
                stationIndex = i;
                break;
            }
        }

        if (stationIndex == -1) {
            gLog << "RadioVehicles: station [" << stationName << "] not found for model " << modelId << std::endl;
        }
        else {
            gVehicleStationMap[modelId].push_back(stationIndex);
            gLog << "RadioVehicles: model " << modelId << " -> [" << stations[stationIndex].name << "]" << std::endl;
            mapped++;
        }
    }

    gLog << "RadioVehicles: " << mapped << " entries mapped, "
         << gNoRadioVehicles.size() << " no-radio models" << std::endl;
    gLog << "RadioVehicles: RadioAutoTune = " << (gRadioAutoTune ? 1 : 0) << std::endl;
    gLog.flush();
}

// All assignments belong to a generation-safe pool instance, not the most
// recently audible car. The map is owned exclusively by the game thread.
static SavedStation* FindAssignment(CVehicle* vehicle)
{
    if (!vehicle || !CPools::ms_pVehiclePool) return nullptr;
    const int handle = CPools::GetVehicleRef(vehicle);
    if (handle < 0 || CPools::GetVehicle(handle) != vehicle) return nullptr;
    auto found = gVehicleSavedStation.find(handle);
    if (found == gVehicleSavedStation.end()) return nullptr;
    if (found->second.modelId == vehicle->m_nModelIndex &&
        RadioPolicy::ValidStation(found->second.station, (int)stations.size()))
        return &found->second;
    gVehicleSavedStation.erase(found);
    return nullptr;
}

static void SaveAssignment(CVehicle* vehicle, int station, bool npcAssigned)
{
    if (!vehicle || !CPools::ms_pVehiclePool ||
        !RadioPolicy::ValidStation(station, (int)stations.size())) return;
    const int handle = CPools::GetVehicleRef(vehicle);
    if (handle < 0 || CPools::GetVehicle(handle) != vehicle) return;
    gVehicleSavedStation[handle] = { station, vehicle->m_nModelIndex, npcAssigned };
    if (gVehicleSavedStation.size() > (size_t)CPools::ms_pVehiclePool->m_nSize) {
        for (auto it = gVehicleSavedStation.begin(); it != gVehicleSavedStation.end();) {
            if (!CPools::GetVehicle(it->first)) it = gVehicleSavedStation.erase(it);
            else ++it;
        }
    }
}

static bool ExteriorStation(int index)
{
    // MP3 PLAYER has no single station file and is player-only. Do not send
    // the empty playlist filename to BASS through the exterior decoder.
    return index >= 0 && index < (int)stations.size() && !stations[index].file.empty() &&
        stations[index].name != "MP3 PLAYER";
}

static int PickStation(CVehicle* vehicle, bool exterior)
{
    const auto mapped = gVehicleStationMap.find(vehicle->m_nModelIndex);
    int pick = RadioPolicy::Off, eligible = 0;
    auto consider = [&](int index) {
        if (index < 0 || index >= (int)stations.size()) return;
        if (exterior && !ExteriorStation(index)) return;
        if (rand() % ++eligible == 0) pick = index;
    };
    if (mapped != gVehicleStationMap.end() && !mapped->second.empty()) {
        for (int index : mapped->second) consider(index);
        return pick;
    }
    for (int index = 0; index < (int)stations.size(); ++index)
        if (stations[index].name != "MP3 PLAYER") consider(index);
    if (!exterior && eligible == 0 && !stations.empty()) return (int)stations.size() - 1;
    return pick;
}

bool CanVehiclePlayAmbient(CVehicle* vehicle)
{
    if (!vehicle || IsNoRadioVehicle(vehicle->m_nModelIndex) ||
        gMusicManager.UsesPoliceRadio(vehicle)) return false;
    if (auto* saved = FindAssignment(vehicle)) return ExteriorStation(saved->station);
    const auto mapped = gVehicleStationMap.find(vehicle->m_nModelIndex);
    if (mapped != gVehicleStationMap.end() && !mapped->second.empty()) {
        for (int i : mapped->second) if (ExteriorStation(i)) return true;
        return false;
    }
    for (int i = 0; i < (int)stations.size(); ++i) if (ExteriorStation(i)) return true;
    return false;
}

int GetAmbientStationForVehicle(CVehicle* vehicle)
{
    if (!CanVehiclePlayAmbient(vehicle)) return RadioPolicy::Off;
    if (auto* saved = FindAssignment(vehicle)) {
        saved->npcAssigned = true;
        return saved->station;
    }
    const int station = PickStation(vehicle, true);
    if (station >= 0) SaveAssignment(vehicle, station, true);
    return station;
}

int GetStationForVehicle(CVehicle* vehicle, bool* inheritedNpc)
{
    if (inheritedNpc) *inheritedNpc = false;
    if (!vehicle || stations.empty() || IsNoRadioVehicle(vehicle->m_nModelIndex))
        return RadioPolicy::Off;
    const auto* saved = FindAssignment(vehicle);
    const bool npc = saved && saved->npcAssigned;
    int station = RadioPolicy::EntryStation(saved ? saved->station : RadioPolicy::Unassigned,
        npc, gRadioAutoTune, gLastStation, (int)stations.size());
    if (station == RadioPolicy::Unassigned) station = PickStation(vehicle, false);
    if (inheritedNpc) *inheritedNpc = npc;
    SaveAssignment(vehicle, station, npc);
    gLog << "RadioVehicles: " << (npc ? "inherited NPC" : "entry") << " station ["
         << (station >= 0 ? stations[station].name : "RADIO OFF") << "]" << std::endl;
    return station;
}

void OnPlayerExitVehicle(CVehicle* vehicle)
{
    // The station shown during a pending retune is the player's latest choice.
    // Explicit OFF is stored too; it must not silently turn back on on reentry.
    int station = gPendingStation >= 0 ? gPendingStation : gCurrentStation;
    if (!RadioPolicy::ValidStation(station, (int)stations.size())) return;
    SaveAssignment(vehicle, station, false);
    gLastStation = station;
}

class RadioVehiclesPlugin
{
public:
    RadioVehiclesPlugin()
    {
        Events::initGameEvent.Add([]()
            {
                LoadVehicleSection();
            });
    }
} radioVehiclesPlugin;
