// Motortown Connect (was Net Tuning) -- the network side, as plugins\mt_plugin_MotortownConnect.dll.
//
// [Connect] DirectIP=1: plain UDP/IP instead of Steam sockets, through the game's own fallback
// (UserEngine.ini: bUseSteamNetworking=false and the IpNetDriver as GameNetDriver). Server and client
// must agree: a DirectIP client reaches only DirectIP servers, and a Steam client never reaches one.
// The engine reads that file before plugins run, so a change applies from the next start. The
// lines sit between two marker comments; the rest of the file is left alone.
//
// The send rate: Steam sockets clamps every connection's send rate to SendRateMin..SendRateMax,
// both 256 KB/s by default, under UE's own MaxClientRate (1 MB/s on the server). A busy server's
// replication to each player is held to that, so updates queue and vehicles arrive late and uneven.
// [Steam] SendRateMin / SendRateMax in plugins\motortown_connect.ini (an old net_tuning.ini's values
// are taken over while the new file has none), bytes/s, 0 = leave Steam's default.
// Default: max 1048576 (the net driver's MaxClientRate) on a dedicated server, nothing on a client.
// The minimum stays where it is, so Steam's bandwidth estimate still backs off on a slow line.
//
// Every 10 s: the game-thread frame rate (mean, worst frame, frames over 33 ms). On a dedicated server
// that is the tick rate, which is also the most a player's car can replicate at: the game asks 100 Hz
// for a driven car, the server sends at most once a tick. Every 30 s: the net driver's limits as the
// game runs them, the connections, the vehicles by NetUpdateFrequency, and the vehicle replication
// console variables. Works on the client and on the dedicated server; delete the dll to remove it, or
// [General] Disabled=1.

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include "../include/mt_plugin.hpp"

using namespace mt;

static uintptr_t statics, fDelta, fTime, ksl, fFloat;
static double lastTime = -1;
static DWORD fpsAt;
static uintptr_t world; static int worldIdx = -1;
static double sum, worst; static int frames, slow;
static volatile LONG queued;

static bool alive(uintptr_t o, int idx) { return o && idx >= 0 && api->object(idx) == o; }

static const wchar_t* INI = L"motortown_connect.ini";
// The new file's value; while it has none, the old net_tuning.ini's, declared into the new file.
static int setting(const wchar_t* section, const wchar_t* key, int def) {
    if (iniGet(INI, section, key).empty()) {
        std::wstring old = iniGet(L"net_tuning.ini", section, key);
        if (!old.empty()) def = _wtoi(old.c_str());
    }
    return iniInt(INI, section, key, def);
}


// ---- DirectIP ----
static const char* IP_BEGIN = "; Motortown Connect DirectIP";
static const char* IP_END = "; Motortown Connect DirectIP end";
static const char* IP_LINES =
    "[OnlineSubsystemSteam]\r\nbUseSteamNetworking=false\r\n\r\n"
    "[/Script/Engine.GameEngine]\r\n!NetDriverDefinitions=ClearArray\r\n"
    "+NetDriverDefinitions=(DefName=\"GameNetDriver\",DriverClassName=\"OnlineSubsystemUtils.IpNetDriver\",DriverClassNameFallback=\"OnlineSubsystemUtils.IpNetDriver\")\r\n"
    "+NetDriverDefinitions=(DefName=\"BeaconNetDriver\",DriverClassName=\"OnlineSubsystemUtils.IpNetDriver\",DriverClassNameFallback=\"OnlineSubsystemUtils.IpNetDriver\")\r\n"
    "+NetDriverDefinitions=(DefName=\"DemoNetDriver\",DriverClassName=\"/Script/Engine.DemoNetDriver\",DriverClassNameFallback=\"/Script/Engine.DemoNetDriver\")\r\n";

// The file's text with our block taken out, then put back at the end when on.
static std::string withBlock(std::string text, bool on) {
    size_t a = text.find(IP_BEGIN);
    if (a != std::string::npos) {
        size_t b = text.find(IP_END, a);
        b = b == std::string::npos ? text.size() : b + strlen(IP_END);
        while (b < text.size() && (text[b] == '\r' || text[b] == '\n')) b++;
        text.erase(a, b - a);
    }
    if (on) {
        if (!text.empty() && text.back() != '\n') text += "\r\n";
        text += std::string(IP_BEGIN) + "\r\n" + IP_LINES + IP_END + "\r\n";
    }
    return text;
}

static bool selfCheck() {
    std::string base = "[ConsoleVariables]\r\nmh.hostServerUrl=\"http://localhost:81/\"\r\n";
    std::string on = withBlock(base, true);
    bool ok = on.find("bUseSteamNetworking=false") != std::string::npos && withBlock(on, true) == on &&
              withBlock(on, false) == base && withBlock(base, false) == base &&
              withBlock("", true).find(IP_END) != std::string::npos;
    MT_LOG("self-check: %s", ok ? "ok" : "FAILED");
    return ok;
}

// <game>\MotorTown\Config\UserEngine.ini, from the exe in MotorTown\Binaries\Win64 (client and server alike).
static std::wstring userEngine() { return gameDir() + L"..\\..\\Config\\UserEngine.ini"; }

// True when the file had to change.
static bool applyDirectIp(bool on) {
    std::ifstream in(userEngine(), std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    std::string was = ss.str(), want = withBlock(was, on);
    in.close();
    if (want == was) return false;
    std::ofstream(userEngine(), std::ios::binary | std::ios::trunc) << want;
    return true;
}

// ---- hitches ----
// Live UObjects, counted on the plugin thread four times a second. A frame over 100 ms is logged with
// how the count moved around it: thousands gone is garbage collection, a jump is assets loading in
// (traffic and world streaming near the player), neither points at shaders or rendering.
struct Sample { DWORD at; int live; };
static Sample samples[16];
static volatile LONG sampleAt;
static void countObjects() {
    int live = 0;
    for (int i = 0, n = api->num_objects(); i < n; i++) live += api->object(i) != 0;
    LONG k = sampleAt;
    samples[(k + 1) % 16] = {GetTickCount(), live};
    InterlockedExchange(&sampleAt, k + 1);
}
// Biggest drop and rise between two samples in the last 2 s.
static void objectMoves(int* drop, int* rise, int* live) {
    *drop = *rise = 0; *live = 0;
    LONG k = sampleAt;
    DWORD now = GetTickCount();
    *live = samples[k % 16].live;
    for (int j = 0; j < 8; j++) {
        const Sample& a = samples[(k - j - 1 + 32) % 16];
        const Sample& b = samples[(k - j + 32) % 16];
        if (!a.at || !b.at || now - b.at > 2000) break;
        int d = b.live - a.live;
        if (d < -*drop) *drop = -d;
        if (d > *rise) *rise = d;
    }
}

static float cvar(const wchar_t* key) {
    struct { const wchar_t* data; int num, max; } in = {key, (int)wcslen(key) + 1, (int)wcslen(key) + 1};
    unsigned char p[0x40] = {0};
    memcpy(p + off(fFloat, "VariableName"), &in, sizeof in);
    api->process_event(ksl, fFloat, p);
    float out = 0;
    memcpy(&out, p + off(fFloat, "ReturnValue"), 4);
    return out;
}

static const wchar_t* CVARS[] = {
    L"mh.vehicleNetFrequencyMultiplier", L"mh.vehicleMovementReplicationPositionLerp",
    L"mh.vehicleMovementReplicationRotationLerp", L"mh.vehicleMovementReplicationVelocityLerp",
    L"mh.vehicleMovementReplicationAngleVelocityLerp", L"mh.vehicleMovementReplicationSnapDistance",
    L"mh.vehicleMovementReplicationSnapVelocity", L"mh.handsOverVehicleToClient", L"mh.disableVehiclePhysicsOnDedi",
    L"mh.vehiclePhysicsMode", L"t.MaxFPS", L"net.MaxNetTickRate"};
static bool wantCvars;

static void tick(void*) {
    InterlockedExchange(&queued, 0);
    if (!alive(world, worldIdx)) return;
    // the hook drains queued work more than once a frame: count a frame only when world time moved
    struct { uintptr_t ctx; double ret; } t = {world, 0}, d = {world, 0};
    api->process_event(statics, fTime, &t);
    if (t.ret == lastTime) return;
    lastTime = t.ret;
    api->process_event(statics, fDelta, &d);
    if (d.ret <= 0 || d.ret > 5) return;
    sum += d.ret; frames++; slow += d.ret > 1 / 30.0;
    worst = d.ret > worst ? d.ret : worst;
    if (d.ret > 0.1) {
        static int said;
        int drop, rise, live;
        objectMoves(&drop, &rise, &live);
        if (said++ < 300)
            MT_LOG("hitch %.0f ms: objects -%d / +%d in the last 2 s (%d live)%s", d.ret * 1000, drop, rise, live,
                   drop > 2000 ? ": garbage collection" : rise > 2000 ? ": loading" : "");
    }
    DWORD now = GetTickCount();
    if (now - fpsAt >= 10000) {
        MT_LOG("fps %.1f mean, worst frame %.0f ms, %d of %d frames over 33 ms", frames / sum, worst * 1000, slow, frames);
        sum = worst = 0; frames = slow = 0; fpsAt = now;
    }
    if (wantCvars) {
        wantCvars = false;
        std::string line;
        for (auto k : CVARS) { char b[96]; snprintf(b, sizeof b, " %ls=%g", k, cvar(k)); line += b; }
        MT_LOG("cvars:%s", line.c_str());
    }
}

// Off the game thread: reads only, every read SEH-guarded by the loader.
static void netInfo() {
    uintptr_t drvCls = api->find_object("Class", "NetDriver", "/Script/Engine");
    uintptr_t vehCls = api->find_object("Class", "MTVehicle", "/Script/MotorTown");
    uintptr_t actCls = api->find_object("Class", "Actor", "/Script/Engine");
    uintptr_t conCls = api->find_object("Class", "NetConnection", "/Script/Engine");
    uintptr_t pcCls = api->find_object("Class", "PlayerController", "/Script/Engine");
    int oFreq = off(actCls, "NetUpdateFrequency"), oPrio = off(actCls, "NetPriority");
    int oSpeed = off(conCls, "CurrentNetSpeed");
    std::map<int, int> byHz;
    int vehicles = 0;
    for (int i = 0, n = api->num_objects(); i < n; i++) {
        uintptr_t o = api->object(i);
        if (!o || isCDO(o)) continue;
        uintptr_t c = classOf(o);
        if (drvCls && api->is_a(c, drvCls)) {
            int oConns = off(c, "ClientConnections"), oServer = off(c, "ServerConnection");
            int conns = oConns >= 0 ? get<int>(o + oConns + 8) : -1;
            uintptr_t server = oServer >= 0 ? get<uintptr_t>(o + oServer) : 0;
            std::string speeds;
            if (oSpeed >= 0 && oConns >= 0)
                for (int k = 0; k < conns && k < 200; k++) {
                    uintptr_t cn = get<uintptr_t>(get<uintptr_t>(o + oConns) + 8ull * k);
                    speeds += " " + std::to_string(get<int>(cn + oSpeed));
                }
            if (server && oSpeed >= 0) speeds += " server " + std::to_string(get<int>(server + oSpeed));
            MT_LOG("driver %s (%s): NetServerMaxTickRate %d MaxNetTickRate %d MaxClientRate %d MaxInternetClientRate %d | %d client conns, net speed B/s:%s",
                   objName(o).c_str(), objName(c).c_str(), get<int>(o + off(c, "NetServerMaxTickRate")),
                   get<int>(o + off(c, "MaxNetTickRate")), get<int>(o + off(c, "MaxClientRate")),
                   get<int>(o + off(c, "MaxInternetClientRate")), conns, speeds.c_str());
        } else if (vehCls && oFreq >= 0 && api->is_a(c, vehCls)) {
            vehicles++;
            byHz[(int)(get<float>(o + oFreq) + 0.5f)]++;
        } else if (!alive(world, worldIdx) && objName(c) == "World" && objName(o).find("Jeju") != std::string::npos) {
            world = o; worldIdx = i;
        } else if (!alive(world, worldIdx) && pcCls && api->is_a(c, pcCls)) {
            // Any other map (Arini, single player): the world the player controller is in.
            uintptr_t w = outerOf(outerOf(o));  // PlayerController -> Level -> World
            if (w && objName(classOf(w)) == "World") { world = w; worldIdx = get<int>(w + L().obj_index); }
        }
    }
    std::string h;
    for (auto& e : byHz) h += " " + std::to_string(e.second) + "x" + std::to_string(e.first) + "Hz";
    MT_LOG("vehicles %d by NetUpdateFrequency:%s (prio+%x)", vehicles, h.c_str(), oPrio);
}

// The transport under the net driver is Steam sockets, which has its own per-connection send rate
// clamp and send buffer: UE's MaxClientRate does not reach them. Read through the flat api.
// Steam copies the global values into the listen socket when the server opens it, and every player's
// connection inherits from that socket: a value set after that only changes the global number. So the
// rate is set from the moment Steam's utils answer, before the world (and its socket) is up.
static bool dedicated;
static volatile LONG rateSet;
static DWORD started;
struct SteamFns { void* utils; int (*get)(void*, int, int, intptr_t, int*, void*, size_t*); bool (*set)(void*, int, int); };
static SteamFns steamFns() {
    HMODULE m = GetModuleHandleW(L"steam_api64.dll");
    // Asked for before Steam is up, the accessor hands back nothing and keeps doing so: wait for its pipe.
    auto pipeFn = m ? (int (*)())GetProcAddress(m, dedicated ? "SteamGameServer_GetHSteamPipe" : "SteamAPI_GetHSteamPipe") : nullptr;
    if (!pipeFn || !pipeFn()) return {};
    auto utilsFn = m ? (void* (*)())GetProcAddress(m, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004") : nullptr;
    SteamFns f = {utilsFn ? utilsFn() : nullptr,
                  m ? (int (*)(void*, int, int, intptr_t, int*, void*, size_t*))GetProcAddress(m, "SteamAPI_ISteamNetworkingUtils_GetConfigValue") : nullptr,
                  m ? (bool (*)(void*, int, int))GetProcAddress(m, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32") : nullptr};
    return f;
}
static int globalValue(const SteamFns& f, int id) {
    int type = 1, v = -1; size_t n = sizeof v;  // k_ESteamNetworkingConfig_Global = 1, Int32 = 1
    return f.get(f.utils, id, 1, 0, &type, &v, &n) > 0 ? v : -1;
}
// True once the values are set and read back.
static bool steamSet() {
    SteamFns f = steamFns();
    if (!f.utils || !f.get || !f.set) return false;
    int rateMin = setting(L"Steam", L"SendRateMin", 0);
    int rateMax = setting(L"Steam", L"SendRateMax", dedicated ? 1048576 : 0);
    if (rateMin > 0) f.set(f.utils, 10, rateMin);
    if (rateMax > 0) f.set(f.utils, 11, rateMax);
    bool ok = (rateMin <= 0 || globalValue(f, 10) == rateMin) && (rateMax <= 0 || globalValue(f, 11) == rateMax);
    if (ok && !InterlockedExchange(&rateSet, 1))
        MT_LOG("steam sockets: SendRateMin %d, SendRateMax %d set %lu ms after start, before the server's socket opens",
               rateMin, rateMax, GetTickCount() - started);
    return ok;
}
static DWORD WINAPI earlyRate(LPVOID) {
    for (int i = 0; i < 9000 && !steamSet(); i++) Sleep(20);  // up to 3 minutes
    return 0;
}
static void steamInfo() {
    SteamFns f = steamFns();
    if (!f.utils || !f.get || !f.set) { MT_LOG("steam sockets: networking utils not available"); return; }
    if (!rateSet && steamSet()) MT_LOG("steam sockets: set only now -- connections may still run at the socket's old rate; restart to apply");
    MT_LOG("steam sockets global config: SendBufferSize=%d SendRateMin=%d SendRateMax=%d NagleTime=%d",
           globalValue(f, 9), globalValue(f, 10), globalValue(f, 11), globalValue(f, 12));
}

static DWORD WINAPI run(LPVOID) {
    if (!api->wait_engine(INFINITE)) return 0;
    statics = api->find_object("GameplayStatics", "Default__GameplayStatics", "/Script/Engine");
    fDelta = api->find_object("Function", "GetWorldDeltaSeconds", "GameplayStatics");
    ksl = api->find_object("KismetSystemLibrary", "Default__KismetSystemLibrary", "/Script/Engine");
    fFloat = api->find_object("Function", "GetConsoleVariableFloatValue", "KismetSystemLibrary");
    dedicated = wcsstr(GetCommandLineW(), L"MotorTownServer") != nullptr;
    MT_LOG("Motortown Connect loaded (%s)", dedicated ? "dedicated server" : "client");
    if (selfCheck()) {
        bool ip = setting(L"Connect", L"DirectIP", 0) != 0;
        bool changed = applyDirectIp(ip);
        MT_LOG("transport: DirectIP=%d (%s)%s", ip, ip ? "plain UDP/IP, the other side must be DirectIP too" : "Steam sockets",
               changed ? " -- written to UserEngine.ini, applies from the next start" : "");
    }
    fTime = api->find_object("Function", "GetTimeSeconds", "GameplayStatics");
    fpsAt = GetTickCount();
    DWORD netAt = 0;
    DWORD countAt = 0;
    for (;;) {
        DWORD now = GetTickCount();
        if (now - countAt >= 250 && api->game_thread_ready()) { countObjects(); countAt = now; }
        if (now - netAt >= 30000 && api->game_thread_ready()) {
            if (!netAt) steamInfo();
            netInfo(); wantCvars = true; netAt = now;
        }
        if (world && api->game_thread_ready() && !InterlockedCompareExchange(&queued, 1, 0)) api->run_on_game_thread(tick, nullptr);
        Sleep(2);
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    if (setting(L"General", L"Disabled", 0)) return 0;
    started = GetTickCount();
    dedicated = wcsstr(GetCommandLineW(), L"MotorTownServer") != nullptr;
    CloseHandle(CreateThread(nullptr, 0, earlyRate, nullptr, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
