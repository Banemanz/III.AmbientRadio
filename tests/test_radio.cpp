#define RADIO_TEST_HOST
#include "../source/RadioVehicles.cpp"
#include "../source/RadioFileLoader.h"
#include <cassert>
#include <cstdio>
#include <iostream>
#include <iterator>

std::vector<RadioStation> stations;
int gCurrentStation = -1, gPendingStation = -1;
std::string gScriptsFolder;
std::ofstream gLog;

int main()
{
    using namespace RadioPolicy;
    assert(NativePolice == 10 && NativeOff == 11);
    for (int n = 0; n < 9; ++n) assert(MapScriptStation(n, 20, 19) == n);
    assert(MapScriptStation(9, 20, 19) == 19);
    assert(MapScriptStation(9, 20, -1) == Unassigned);
    assert(MapScriptStation(10, 20, 19) == Unassigned); // even with custom station 10
    assert(MapScriptStation(11, 20, 19) == Off);
    assert(MapScriptStation(13, 20, 19) == Unassigned);
    assert(SuppressNativeTuning(false, false, false));
    assert(!SuppressNativeTuning(false, true, false));
    assert(SuppressNativeTuning(false, true, true));
    assert(SuppressNativeTuning(true, true, false));
    assert(NativeRadioTrack(10) && !NativeRadioTrack(11) && !NativeRadioTrack(13));
    stations = {{"A","a.wav"},{"B","b.wav"},{"MP3 PLAYER",""}};
    CVehicle a{90, 0x101}, b{91,0x201}, police{116,0x301};
    CPools::live = {{a.handle,&a},{b.handle,&b},{police.handle,&police}};
    gVehicleStationMap[90] = {1};
    gVehicleStationMap[91] = {0};
    bool npc = false;

    // Exterior selection changes to another car; the original car keeps B.
    assert(GetAmbientStationForVehicle(&a) == 1);
    assert(GetAmbientStationForVehicle(&b) == 0);
    gRadioAutoTune = true; gLastStation = 0;
    assert(GetStationForVehicle(&a, &npc) == 1 && npc);
    // Pending tune and explicit radio OFF survive vehicle exit.
    gRadioAutoTune = false;
    gCurrentStation = 1; gPendingStation = 0;
    OnPlayerExitVehicle(&a);
    assert(GetStationForVehicle(&a, &npc) == 0 && !npc);
    gCurrentStation = Off; gPendingStation = Off;
    OnPlayerExitVehicle(&a);
    assert(!CanVehiclePlayAmbient(&a));
    assert(GetStationForVehicle(&a, &npc) == Off);
    assert(!CanVehiclePlayAmbient(&police));
    gNoRadioVehicles.insert(91);
    assert(!CanVehiclePlayAmbient(&b) && GetStationForVehicle(&b, &npc) == Off);

    // Same memory address and model, new pool generation: old OFF must not leak.
    CPools::live.erase(a.handle);
    a.handle = 0x102; CPools::live[a.handle] = &a;
    assert(GetAmbientStationForVehicle(&a) == 1);
    assert(GetStationForVehicle(&a, &npc) == 1 && npc);
    // No exterior files with MP3-only, but player MP3 remains selectable.
    stations = {{"MP3 PLAYER",""}};
    gVehicleSavedStation.clear(); gVehicleStationMap.clear(); gNoRadioVehicles.clear();
    assert(!CanVehiclePlayAmbient(&a));
    assert(GetStationForVehicle(&a, &npc) == 0);

    // Actual chunked ADF loader: decrypt, reject empty/missing, cancel mid-read.
    const std::string path = "radio-test-temp.adf";
    std::vector<unsigned char> expected(2*1024*1024+5);
    for (size_t i=0; i<expected.size(); ++i) expected[i] = static_cast<unsigned char>(i);
    { std::ofstream f(path, std::ios::binary); for (auto v : expected) f.put(char(v^0x22)); }
    std::vector<unsigned char> got;
    assert(LoadDecryptedAdf(path, got, [](){return false;}) && got == expected);
    int calls = 0;
    assert(!LoadDecryptedAdf(path, got, [&](){return ++calls >= 4;}));
    std::remove(path.c_str());
    assert(!LoadDecryptedAdf(path, got, [](){return false;}));
    std::cout << "PASS: native III tuning IDs, police isolation, actual per-vehicle NPC handoff, pending/off, recycled handles, MP3-only, chunked ADF cancellation\n";
}
