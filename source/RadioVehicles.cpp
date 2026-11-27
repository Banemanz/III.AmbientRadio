#include "plugin.h"
#include "CPlayerPed.h"
#include "CVehicle.h"
#include "CPools.h"
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <windows.h>

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
static int gLastStation = -1;

// [SETTINGS] RadioAutoTune: when 1, the station the player is listening to follows
// them into EVERY vehicle they enter, regardless of distance. When 0 (default), a
// different/new vehicle gets its own station (its [VEHICLES] assignment or random)
// and the station does NOT carry over from the previous vehicle.
static bool gRadioAutoTune = false;

// The game's pool handle includes the slot generation, unlike a raw pointer.
// Recycled vehicles of the SAME model must not inherit a previous vehicle's
// station simply because the pool reused the same address.
struct SavedStation { int station; int modelId; };
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
    std::string iniPath = gScriptsFolder + "III.AmbientRadio.ini";
    std::ifstream ini(iniPath);
    if (!ini.is_open()) {
        gLog << "RadioVehicles: cannot open III.AmbientRadio.ini" << std::endl;
        gLog.flush();
        return;
    }

    gVehicleSavedStation.clear();
    gVehicleStationMap.clear();
    gNoRadioVehicles.clear();
    gLastStation = -1;

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

int GetStationForVehicle(CVehicle* pVehicle)
{
    // RadioAutoTune: the station the player is listening to follows them into EVERY
    // vehicle, regardless of distance or which vehicle it is. Overrides per-vehicle
    // memory and model defaults. Off by default.
    if (gRadioAutoTune && gLastStation >= 0 && gLastStation < (int)stations.size()) {
        gLog << "RadioVehicles: RadioAutoTune -> keeping [" << stations[gLastStation].name << "]" << std::endl;
        gLog.flush();
        return gLastStation;
    }

    // If this specific vehicle instance has a saved station, resume it — but only
    // if the model still matches (guards against the pool reusing this pointer for
    // a different model). This is what makes a bike/car keep its station no matter
    // how long the player is away, like stock VC.
    const int vehicleHandle = CPools::GetVehicleRef(pVehicle);
    auto saved = gVehicleSavedStation.find(vehicleHandle);
    if (saved != gVehicleSavedStation.end()) {
        if (saved->second.modelId == pVehicle->m_nModelIndex &&
            saved->second.station >= 0 && saved->second.station < (int)stations.size()) {
            int station = saved->second.station;
            gLog << "RadioVehicles: resuming saved station [" << stations[station].name << "] for this vehicle" << std::endl;
            gLog.flush();
            return station;
        }
        gVehicleSavedStation.erase(saved);
    }

    // The player heard this vehicle's NPC radio outside: use the same station
    // inside, even if this vehicle model had no explicit [VEHICLES] mapping.
    extern CVehicle* gAmbientVehicle;
    extern int gAmbientStation;
    if (gAmbientVehicle == pVehicle && gAmbientStation >= 0 &&
        gAmbientStation < (int)stations.size())
        return gAmbientStation;

    // (No time-based carry-over: a DIFFERENT/new vehicle gets its own station — its
    // [VEHICLES] assignment or a random pick — unless RadioAutoTune is on, above.)

    // Look up vehicle model
    int modelId = pVehicle->m_nModelIndex;
    auto it = gVehicleStationMap.find(modelId);
    if (it != gVehicleStationMap.end()) {
        const std::vector<int>& options = it->second;

        if (options.empty()) return 0;
        int pick = options[rand() % options.size()];
        gLog << "RadioVehicles: model " << modelId << " -> [" << stations[pick].name << "]" << std::endl;
        gLog.flush();
        return pick;
    }

    // Not in map — pick a random station from INI only (excludes MP3 PLAYER)
    if (!stations.empty()) {
        int count = (int)stations.size();
        if (count > 0 && stations[count - 1].name == "MP3 PLAYER") count--;
        if (count == 0) return (int)stations.size() - 1; // MP3-only config
        int pick = rand() % count;
        gLog << "RadioVehicles: model " << modelId << " not mapped, random -> [" << stations[pick].name << "]" << std::endl;
        gLog.flush();
        return pick;
    }

    return 0;
}

void OnPlayerExitVehicle(CVehicle* pVehicle)
{
    // Save the current station to this specific vehicle instance so re-entering
    // the SAME vehicle later resumes it regardless of how long we were away.
    int station = gCurrentStation != -1 ? gCurrentStation : gPendingStation;
    if (pVehicle && station >= 0 && station < (int)stations.size()) {
        int vehicleHandle = CPools::GetVehicleRef(pVehicle);
        gVehicleSavedStation[vehicleHandle] = { station, pVehicle->m_nModelIndex };
        if (gVehicleSavedStation.size() > 256) {
            for (auto it = gVehicleSavedStation.begin(); it != gVehicleSavedStation.end();) {
                if (!CPools::GetVehicle(it->first)) it = gVehicleSavedStation.erase(it);
                else ++it;
            }
        }
        gLog << "RadioVehicles: saved [" << stations[station].name << "] to vehicle instance" << std::endl;
    }

    // Remember the last station the player was listening to — used only by
    // RadioAutoTune to carry it into the next vehicle.
    gLastStation = station;
    if (station >= 0 && station < (int)stations.size())
        gLog << "RadioVehicles: player exited, last station was [" << stations[station].name << "]" << std::endl;
    gLog.flush();
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
