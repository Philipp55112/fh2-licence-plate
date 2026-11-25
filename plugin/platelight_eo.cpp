#include <set>
// PlateLight: Kennzeichenbeleuchtung fuer Forza Horizon 2 (Horizon-Kennzeichen-Mod).
//
// Der Mechanismus (Lichtmanager, Offsets, Presets) stammt aus Underglow von Bartholomew Ho
// (MIT License, https://github.com/qiangqiang101/FH2Underglow): neue Lichter werden zur Laufzeit ueber den
// eigenen LightManager des Spiels erzeugt (CreateLight mit Key/Index des Rücklichts), bekommen ein eigenes Preset
// (Kopie des Ruecklicht-Presets, Farbe/Reichweite/Kegel aus PlateLight.ini) und werden pro Frame am Auto platziert.
// Hier sitzen sie statt unter dem Auto direkt ueber dem Kennzeichen und leuchten es von oben an.
#include <thread>
#include <map>
#include <functional>
#include <algorithm>
#include <windows.h>
#include "MinHook.h"
#include <intrin.h>
#include <xmmintrin.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include "plate_editor.h"

namespace {

void (*g_logfn)(const char*) = nullptr;
void LOG(const char* t) {
    if (!g_logfn) return;
    char b[1024];
    const int n = snprintf(b, sizeof b, "PlateLight: %s\r\n", t);
    if (n < 0 || n >= int(sizeof b)) memcpy(b + sizeof b - 3, "\r\n", 3);   // gekuerzt: Zeilenende behalten
    g_logfn(b);
}
const uintptr_t FH2_GAME_FORZACAR_KIND = 0xF62280, FH2_GAME_FORZACAR_UPDATE = 0x2A9400;
const char FH2_GAME_FORZACAR_KIND_BYTES[] = "\x8B\x81\x08\x4D\x00\x00\xC3";
const char FH2_GAME_FORZACAR_UPDATE_BYTES[] = "\x48\x8B\xC4\x48\x89\x58\x18\x57\x48\x81\xEC\xA0";
uintptr_t g_base = 0;
wchar_t g_ini[MAX_PATH] = {};

// ---- Game addresses (as Underglow 1.1: same FH2 build) -------------------------------------------------------
const uintptr_t kBeamUpload = 0x10F6C40;
const uintptr_t kCreateLight = 0x10C7E00, kSetIntensity = 0x10F0550, kSetPosition = 0x10F1C30, kSetDirection = 0x10F0DA0;
const uintptr_t kGlowSetIntensity = 0x10F04C0;
const uintptr_t kAttachPreset = 0x10F1EB0;
const uintptr_t kAllocBeamPreset = 0x10BE480;
const uintptr_t kMatrixToQuat = 0x10F89B0;
const uintptr_t kPresWorld = 0x1A0;

const uintptr_t kForzaCarKindOffset = 0x4D08, kForzaCarPresOffset = 0x4D38, kForzaCarIdOffset = 0x118;
const uintptr_t kDeferredPresVtable = 0x2EBEF68;
const uintptr_t kDeferredPresOffset = 0x8;
const uintptr_t kCarPresVtables[] = {0x2EC1450, 0x2EC2B70};

const uintptr_t kPresContext = 0x10, kContextManager = 0x380, kPresAsset = 0x1278;
const uintptr_t kAssetInstances = 0x4BE0;
const uintptr_t kManagerBeams = 0x08, kManagerGlows = 0x18, kManagerMapping = 0x28;
const size_t kMappingStride = 24;
const uint16_t kTypeTailGlow = 69;
const uintptr_t kBeamPresetPool = 0x38, kBeamDirty = 0x100;
const size_t kRecordStride = 0x40;
const uintptr_t kRecPreset = 0x30;
const size_t kBeamPresetStride = 0xC0;
const uintptr_t kPreUsers = 0x30, kPreRgb = 0x50, kPreRange = 0x60, kPrePower = 0x64, kPreInner = 0x68,
                kPreOuter = 0x6C, kPreThreshold = 0x9C;
const size_t kMaxRecords = 100000;
const int kMaxPlayerLights = 16;
const int kMaxLamps = 8;

using UploadFn = void (*)(void* system, void* graphics);
using CreateLightFn = uint32_t* (*)(void* mgr, uint32_t* handle, uint32_t key, uint32_t index);
using SetVecFn = void (*)(void* mgr, uint32_t handle, const float* v);
using SetFloatFn = void (*)(void* mgr, uint32_t handle, float v);
using GlowIntensityFn = void (*)(void* glow_system, uint32_t record, float v);
using AttachPresetFn = void (*)(void* beam_system, uint32_t record, uint32_t preset);
using AllocPresetFn = uint32_t (*)(void* pool);
using MatrixToQuatFn = __m128 (*)(const float* matrix);
using CarUpdateFn = void (*)(void* car, float dt, float b, uint32_t flag);

UploadFn g_orig_beam = nullptr;
CreateLightFn g_orig_create = nullptr;
SetVecFn g_orig_set_pos = nullptr, g_orig_set_dir = nullptr;
SetFloatFn g_orig_set_int = nullptr;
CarUpdateFn g_orig_car_update = nullptr;

// ---- Speicher, der nicht da sein koennte (ReadProcessMemory statt SEH) ----------------------------------------
bool rd(uintptr_t p, void* out, size_t n) {
    if (!p) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(p), out, n, &got) && got == n;
}
bool wr(uintptr_t p, const void* in, size_t n) {
    if (!p) return false;
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(p), in, n, &got) && got == n;
}
bool rdp(uintptr_t p, uintptr_t* out) { return rd(p, out, 8) && *out; }
bool vtable_is(uintptr_t obj, const uintptr_t* rvas, int n) {
    uintptr_t vt = 0;
    if (!rd(obj, &vt, 8)) return false;
    for (int i = 0; i < n; ++i)
        if (vt == g_base + rvas[i]) return true;
    return false;
}
struct Vec { uintptr_t begin, end, cap; };
bool read_vec(uintptr_t at, size_t stride, size_t limit, Vec* v, size_t* count) {
    if (!rd(at, v, sizeof *v) || !v->begin || v->end < v->begin || (v->end - v->begin) % stride) return false;
    *count = (v->end - v->begin) / stride;
    return *count <= limit;
}
bool mapping_of(uintptr_t manager, uint32_t handle, uint32_t* beam, uint32_t* glow) {
    Vec map; size_t n = 0; uint32_t e[2];
    if (!read_vec(manager + kManagerMapping, kMappingStride, kMaxRecords, &map, &n) || handle >= n ||
        !rd(map.begin + handle * kMappingStride, e, 8))
        return false;
    *beam = e[0]; *glow = e[1];
    return true;
}
uintptr_t beam_record(uintptr_t system, uint32_t index) {
    Vec v; size_t n = 0;
    return read_vec(system, kRecordStride, kMaxRecords, &v, &n) && index < n ? v.begin + index * kRecordStride : 0;
}
uintptr_t beam_preset(uintptr_t system, uint32_t index) {
    Vec v; size_t n = 0;
    return read_vec(system + kBeamPresetPool, kBeamPresetStride, kMaxRecords, &v, &n) && index < n
               ? v.begin + index * kBeamPresetStride : 0;
}

// ---- Quaternionen (x, y, z, w) -----------------------------------------------------------------------------------
void qmul(const float* a, const float* b, float* o) {
    float r[4] = {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                  a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                  a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                  a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    memcpy(o, r, sizeof r);
}
void qrot(const float* q, const float* v, float* o) {
    float t[3] = {2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]), 2.0f * (q[0] * v[1] - q[1] * v[0])};
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}
bool finite(const float* v, int n) {
    for (int i = 0; i < n; ++i) if (!std::isfinite(v[i])) return false;
    return true;
}

// ---- Einstellungen (PlateLight.ini, wird bei Aenderung neu gelesen) --------------------------------------------
struct Config {
    int on = 1;
    int count = 2;             // Anzahl Lampen (1..8)
    float x = 0.0f, y = 0.60f, z = -2.50f;   // Mitte der Lampen im Auto-Koordinatensystem (Modell-Raum, hinten = -z)
    float sep = 0.24f;         // Abstand der Lampen in x (bei count=2: +-sep/2)
    float angle = 58.0f;       // Grad: 0 = nach vorn (+z), 90 = nach unten
    float yaw = 0.0f;          // Grad um die Hochachse (0 = nach vorn, +z)
    float range = 0.45f, power = 5.0f, inner = 35.0f, outer = 75.0f;
    float rgb[3] = {1.0f, 0.92f, 0.78f};
    bool rgb_override = false;   // ini [Light] R/G/B gesetzt -> gilt fuer alle Autos
    float brightness = 0.45f;
    int only_dark = 1;         // 1: nur leuchten, wenn die Scheinwerfer des Autos an sind (Phase 2)
    float offx = 0.0f, offy = 0.0f, offz = 0.0f;  // Versatz der Lampenpositionen im Autoraum (m)
    float range_scale = 1.0f;  // v3: Reichweite = Abstand Lampe -> gegenueberliegende Plattenkante x RangeScale
    float out = 0.015f;        // v2: Lichtquelle so weit (m) vor dem Lampenglas, entlang der Plattennormalen
    float tilt = 0.0f;         // v2: Strahl um so viele Grad zur Oberflaeche hin kippen
    int axis = 0;
    int conj = 0;              // 1 = Drehung invertiert an das Spiel geben (Test)
    float glow = 0.0f;         // Glow-Sprite (Leuchtpunkt am Lampenglas), 0 = aus
};
Config g_cfg;
bool g_use_table = true;
int g_want = 2;   // wie viele Lampen das Auto gerade haben soll
SRWLOCK g_cfg_lock = SRWLOCK_INIT;
FILETIME g_cfg_time{};
std::atomic<uint64_t> g_cfg_check{0};
std::atomic<uint32_t> g_cfg_rev{1};

float ini_f(const wchar_t* sec, const wchar_t* key, float def) {
    wchar_t buf[64], dflt[64];
    swprintf(dflt, 64, L"%g", def);
    GetPrivateProfileStringW(sec, key, dflt, buf, 64, g_ini);
    return float(_wtof(buf));
}

void load_config() {
    Config c;
    c.on = int(ini_f(L"Light", L"On", 1));
    c.count = int(ini_f(L"Light", L"Count", 2));
    c.x = ini_f(L"Light", L"X", c.x);
    c.y = ini_f(L"Light", L"Y", c.y);
    c.z = ini_f(L"Light", L"Z", c.z);
    c.sep = ini_f(L"Light", L"Sep", c.sep);
    c.angle = ini_f(L"Light", L"Angle", c.angle);
    c.yaw = ini_f(L"Light", L"Yaw", c.yaw);
    c.range = ini_f(L"Light", L"Range", c.range);
    c.power = ini_f(L"Light", L"Power", c.power);
    c.inner = ini_f(L"Light", L"Inner", c.inner);
    c.outer = ini_f(L"Light", L"Outer", c.outer);
    c.rgb[0] = ini_f(L"Light", L"R", c.rgb[0]);
    c.rgb[1] = ini_f(L"Light", L"G", c.rgb[1]);
    c.rgb[2] = ini_f(L"Light", L"B", c.rgb[2]);
    c.rgb_override = ini_f(L"Light", L"ColorFromIni", 0.0f) > 0.5f;
    c.brightness = ini_f(L"Light", L"Brightness", c.brightness);
    c.glow = ini_f(L"Light", L"Glow", c.glow);
    c.offx = ini_f(L"Light", L"OffX", c.offx); c.offy = ini_f(L"Light", L"OffY", c.offy); c.offz = ini_f(L"Light", L"OffZ", c.offz);
    c.range_scale = ini_f(L"Light", L"RangeScale", c.range_scale);
    c.out = ini_f(L"Light", L"Out", c.out); c.tilt = ini_f(L"Light", L"Tilt", c.tilt);
    c.axis = int(ini_f(L"Light", L"BeamAxis", float(c.axis)));
    c.conj = int(ini_f(L"Light", L"Conj", float(c.conj)));
    c.only_dark = int(ini_f(L"Light", L"OnlyWithHeadlights", 1));
    g_use_table = ini_f(L"Light", L"UseTable", 1) != 0.0f;
    if (c.count < 1) c.count = 1;
    if (c.count > kMaxLamps) c.count = kMaxLamps;
    AcquireSRWLockExclusive(&g_cfg_lock);
    g_cfg = c;
    ReleaseSRWLockExclusive(&g_cfg_lock);
    g_cfg_rev.fetch_add(1);
}

void write_default_ini() {
    if (GetFileAttributesW(g_ini) != INVALID_FILE_ATTRIBUTES) return;
    const char* text =
        "; LicencePlates by Philipp55112 - plate light settings. Changes are applied in-game after about 1 s.\r\n"
        "; Always put notes on their own line, never behind a value.\r\n"
        "[Light]\r\n"
        "; On: 1 = plate light on, 0 = off\r\n"
        "On=1\r\n"
        "; OnlyWithHeadlights: 1 = follows the car lights (key, EO or night), 0 = always on\r\n"
        "OnlyWithHeadlights=1\r\n"
        "; Overall brightness\r\n"
        "Brightness=0.45\r\n"
        "; Light colour (warm white)\r\n"
        "R=1\r\n"
        "G=0.92\r\n"
        "B=0.78\r\n"
        "; Power and cone (inner/outer angle)\r\n"
        "Power=5.0\r\n"
        "Inner=35\r\n"
        "Outer=75\r\n"
        "; RangeScale: range per lamp = distance to the opposite plate edge x RangeScale (larger = light reaches further past the plate)\r\n"
        "RangeScale=1.0\r\n"
        "; Out: light source this many metres in front of the lamp glass\r\n"
        "Out=0.015\r\n"
        "; Tilt: additionally tilt the beam this many degrees towards the body\r\n"
        "Tilt=0\r\n"
        "; UseTable: 1 = only cars with visible plate lamps (PlateLightCars.csv)\r\n"
        "UseTable=1\r\n";
    HANDLE f = CreateFileW(g_ini, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(f, text, DWORD(strlen(text)), &w, nullptr);
        CloseHandle(f);
    }
}

void start_gen(); std::string plate_text_ini(); std::string want_text(); extern std::string g_gen_text;
void poll_config() {
    const uint64_t now = GetTickCount64();
    uint64_t last = g_cfg_check.load();
    if (now - last < 700 || !g_cfg_check.compare_exchange_strong(last, now)) return;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(g_ini, GetFileExInfoStandard, &d)) return;
    if (CompareFileTime(&d.ftLastWriteTime, &g_cfg_time) != 0) {
        g_cfg_time = d.ftLastWriteTime;
        load_config();
        if (want_text() != g_gen_text) start_gen();   // neuer Text: Texturen neu erzeugen (wirkt beim naechsten Laden)
    }
}
Config config() {
    AcquireSRWLockShared(&g_cfg_lock);
    Config c = g_cfg;
    ReleaseSRWLockShared(&g_cfg_lock);
    return c;
}

// ---- Pro-Auto-Tabelle (PlateLightCars.csv): Lampenpositionen aus FH5/FH3/FH6-Platelight relativ zur Platte -------
struct Entry { char name[64]; int n; bool v2; int ver; float reach[kMaxLamps]; float extra[kMaxLamps]; float pos[kMaxLamps][3]; float quat[kMaxLamps][4]; float tgt[kMaxLamps][3]; float nrm[kMaxLamps][3]; long id; bool has_rgb; float rgb[3]; };
const int kMaxEntries = 600;
Entry g_tab[kMaxEntries];
int g_tab_n = 0;
std::atomic<int> g_tab_ids{0};

void load_table(const wchar_t* path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return;
    char line[1024];
    bool v2 = false; int ver = 1;
    while (fgets(line, sizeof line, f) && g_tab_n < kMaxEntries) {
        if (strncmp(line, "#v2", 3) == 0) { v2 = true; ver = 2; continue; }
        if (strncmp(line, "#v3", 3) == 0) { v2 = true; ver = 3; continue; }
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
        char* ctx = nullptr;
        char* tok = strtok_s(line, ";\r\n", &ctx);
        if (!tok) continue;
        Entry& e = g_tab[g_tab_n];
        memset(&e, 0, sizeof e);
        strncpy_s(e.name, tok, _TRUNCATE);
        tok = strtok_s(nullptr, ";\r\n", &ctx);
        if (!tok) continue;
        e.n = atoi(tok);
        if (e.n < 0 || e.n > kMaxLamps) continue;
        bool ok = true;
        e.v2 = v2; e.ver = ver;
        auto num = [&](float* dst, int cnt) { for (int k = 0; k < cnt && ok; ++k) { tok = strtok_s(nullptr, ";\r\n", &ctx); if (!tok) ok = false; else dst[k] = float(atof(tok)); } };
        for (int i = 0; i < e.n && ok; ++i) {
            num(e.pos[i], 3);
            if (v2) { num(e.tgt[i], 3); num(e.nrm[i], 3); if (ver >= 3) { num(&e.reach[i], 1); num(&e.extra[i], 1); } } else num(e.quat[i], 4);
        }
        if (ok) {                                     // optional: Lichtfarbe des Autos (r;g;b) nach den Lampen
            char* t1 = strtok_s(nullptr, ";\r\n", &ctx); char* t2 = t1 ? strtok_s(nullptr, ";\r\n", &ctx) : nullptr; char* t3 = t2 ? strtok_s(nullptr, ";\r\n", &ctx) : nullptr;
            if (t3) { e.has_rgb = true; e.rgb[0] = float(atof(t1)); e.rgb[1] = float(atof(t2)); e.rgb[2] = float(atof(t3)); }
            e.id = 0;
            char* at = strchr(e.name, '@');           // "MediaName@Data_Car.Id" (Id = carbin-Ordinal): keine DB noetig
            if (at) { e.id = atol(at + 1); *at = 0; }
            ++g_tab_n;
        }
    }
    fclose(f);
    int with_id = 0;
    for (int i = 0; i < g_tab_n; ++i) with_id += g_tab[i].id > 0;
    g_tab_ids.store(with_id);
    char m[96]; snprintf(m, sizeof m, "Table: %d cars with id", with_id); LOG(m);
}
const Entry* entry_for(long id) {
    if (id <= 0) return nullptr;
    for (int i = 0; i < g_tab_n; ++i) if (g_tab[i].id == id) return &g_tab[i];
    return nullptr;
}

// ---- Das Spielerauto -------------------------------------------------------------------------------------------
std::atomic<uintptr_t> g_player_pres{0};
std::atomic<long> g_player_car_id{0};
std::atomic<uintptr_t> g_player_obj{0}; std::atomic<bool> g_dead_claimed{false};
std::atomic<uintptr_t> g_car_vt{0}; std::atomic<long> g_new_player{0};
// v4.24: Autowechsel-Fenster: altes Spieler-Objekt abgebaut (Objekt-Id -1), neues noch nicht da. Hoechstens 20 s.
std::atomic<uint64_t> g_gap_t0{0};
bool gap_now(long live) {
    if (live != -1 || !g_player_obj.load()) { g_gap_t0.store(0); return false; }
    uint64_t now = GetTickCount64(), z = 0; g_gap_t0.compare_exchange_strong(z, now);
    return now - g_gap_t0.load() < 20000;
}
void mesh_pend_resolve(long id);
// v4.25: Rohwerte des (alten) Spieler-Objekts. Im Autowechsel ist kind != 0, die Auto-Id kann aber schon die neue sein.
std::atomic<bool> g_gap_scene_claimed{false};
void player_raw(int* kind, long* id) { *kind = -1; *id = -1; uintptr_t o = g_player_obj.load(); int k = -1, v = 0; if (o && rd(o + kForzaCarKindOffset, &k, 4)) *kind = k; if (o && rd(o + kForzaCarIdOffset, &v, 4) && v > 0 && v < 100000) *id = v; }
SRWLOCK g_reg_lock = SRWLOCK_INIT; std::vector<std::pair<uintptr_t, uintptr_t>> g_car_regs;
void add_car_region(uintptr_t c) {
    MEMORY_BASIC_INFORMATION mbi; if (!VirtualQuery(reinterpret_cast<void*>(c), &mbi, sizeof mbi)) return;
    uintptr_t b = reinterpret_cast<uintptr_t>(mbi.BaseAddress), e = b + mbi.RegionSize;
    if (e - b > (64ull << 20)) { b = std::max(b, (c & ~0xFFFFull) - (16ull << 20)); e = std::min(e, (c & ~0xFFFFull) + (16ull << 20)); }
    AcquireSRWLockExclusive(&g_reg_lock); bool f = false; for (auto& r : g_car_regs) if (r.first == b) f = true;
    if (!f && g_car_regs.size() < 64) { g_car_regs.push_back({b, e}); char m[120]; snprintf(m, sizeof m, "Car memory region %llx (%llu KB)", (unsigned long long)b, (unsigned long long)((e - b) >> 10)); LOG(m); }
    ReleaseSRWLockExclusive(&g_reg_lock);
}
// sucht in den Auto-Speicherbereichen ein Spielerauto-Objekt (kind 0) mit anderer Id als dem aktuellen Spielerauto
long scan_new_player() {
    const uintptr_t vt = g_car_vt.load(); if (!vt) return 0;
    std::vector<std::pair<uintptr_t, uintptr_t>> regs; AcquireSRWLockShared(&g_reg_lock); regs = g_car_regs; ReleaseSRWLockShared(&g_reg_lock);
    const long cur = g_player_car_id.load(); std::vector<uint8_t> buf;
    for (auto& r : regs) {
        buf.resize(r.second - r.first); SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(r.first), buf.data(), buf.size(), &got)) continue;
        for (size_t i = 0; i + 8 <= got; i += 16) { uint64_t v; memcpy(&v, &buf[i], 8); if (v != vt) continue;
            const uintptr_t o = r.first + i; int k = -1, id = 0;
            if (rd(o + kForzaCarKindOffset, &k, 4) && k == 0 && rd(o + kForzaCarIdOffset, &id, 4) && id > 0 && id < 100000 && id != cur) return id; }
    }
    return 0;
}
DWORD WINAPI watch_thread(LPVOID) {
    for (;;) { Sleep(30);
        const long n = scan_new_player(); if (n && g_new_player.exchange(n) != n) { char m[120]; snprintf(m, sizeof m, "New player car being built: id %ld (t=%llu)", n, (unsigned long long)GetTickCount64()); LOG(m); } }
    return 0;
}

static void DLOG(const char* t);
bool note_car(void* car) {
    uintptr_t c = reinterpret_cast<uintptr_t>(car);

    int kind = -1;
    if (!rd(c + kForzaCarKindOffset, &kind, 4) || kind != 0) return false;
    { uintptr_t vt = 0; if (!g_car_vt.load() && rd(c, &vt, 8)) g_car_vt.store(vt); }
    g_dead_claimed.store(false); g_gap_scene_claimed.store(false);
    g_player_obj.store(c);
    uintptr_t pres = 0;
    if (!rdp(c + kForzaCarPresOffset, &pres)) return false;
    if (vtable_is(pres, &kDeferredPresVtable, 1) && !rdp(pres + kDeferredPresOffset, &pres)) return false;
    if (!vtable_is(pres, kCarPresVtables, 2)) return false;
    g_player_pres.store(pres);
    int id = 0;
    if (rd(c + kForzaCarIdOffset, &id, 4) && id > 0 && id < 100000 && g_player_car_id.exchange(id) != id) {
        char m[96];
        snprintf(m, sizeof m, "player's car id (Data_Car.Id): %d", id);
        LOG(m);
        { char d[1400]; int k = snprintf(d, sizeof d, "Player id %d (object %p, pres %p, t=%llu) P:", id, (void*)c, (void*)pres, (unsigned long long)GetTickCount64());
          uint64_t pq[64] = {}; rd(pres, pq, sizeof pq); for (int i = 0; i < 64 && k < (int)sizeof d - 24; ++i) k += snprintf(d + k, sizeof d - k, " %llx", (unsigned long long)pq[i]); DLOG(d); }
        mesh_pend_resolve(id);
    }
    return true;
}

// v4.39: alle Auto-Objekte merken (fuer den Besitzer einer Praesentation)
SRWLOCK g_cars_lock = SRWLOCK_INIT; uintptr_t g_cars[256]; uint64_t g_cars_t[256]; int g_cars_n = 0;
void track_car(uintptr_t c) {
    const uint64_t now = GetTickCount64();
    AcquireSRWLockShared(&g_cars_lock); int f = -1; for (int i = 0; i < g_cars_n; ++i) if (g_cars[i] == c) { f = i; g_cars_t[i] = now; break; } ReleaseSRWLockShared(&g_cars_lock);
    if (f >= 0) return;
    AcquireSRWLockExclusive(&g_cars_lock);
    { int slot = -1; for (int i = 0; i < g_cars_n; ++i) if (now - g_cars_t[i] > 60000) { slot = i; break; }
        if (slot < 0 && g_cars_n < 256) slot = g_cars_n++;
        if (slot >= 0) { g_cars[slot] = c; g_cars_t[slot] = now; } }
    ReleaseSRWLockExclusive(&g_cars_lock);
}
void fall_write(uintptr_t pres);
void detour_car_update(void* car, float dt, float b, uint32_t flag) {
    g_orig_car_update(car, dt, b, flag);
    track_car(uintptr_t(car));
    if (note_car(car)) fall_write(g_player_pres.load());
}

struct PlayerLight { uint32_t record; uint16_t type; uint32_t handle; uintptr_t instance; };
struct Resolved {
    uintptr_t pres = 0, manager = 0, asset = 0;
    int count = 0;
    PlayerLight light[kMaxPlayerLights];
};

bool resolve_player(uintptr_t system, Resolved* r) {
    r->count = 0;
    r->pres = g_player_pres.load();
    uintptr_t context = 0, sys = 0;
    if (!r->pres || !vtable_is(r->pres, kCarPresVtables, 2) || !rdp(r->pres + kPresContext, &context) ||
        !rdp(context + kContextManager, &r->manager) || !rdp(r->manager + kManagerBeams, &sys) || sys != system ||
        !rdp(r->pres + kPresAsset, &r->asset))
        return false;
    Vec inst, map, rec;
    size_t n_inst = 0, map_count = 0, rec_count = 0;
    if (!read_vec(r->asset + kAssetInstances, 8, 512, &inst, &n_inst) ||
        !read_vec(r->manager + kManagerMapping, kMappingStride, kMaxRecords, &map, &map_count) ||
        !read_vec(system, kRecordStride, kMaxRecords, &rec, &rec_count))
        return false;
    for (size_t i = 0; i < n_inst && r->count < kMaxPlayerLights; ++i) {
        uintptr_t instance = 0;
        uint8_t data[0x54];
        if (!rdp(inst.begin + i * 8, &instance) || !rd(instance, data, sizeof data)) continue;
        uint16_t type; uint32_t handle, record = ~0u;
        memcpy(&type, data + 4, 2);
        memcpy(&handle, data + 0x50, 4);
        if (handle >= map_count || !rd(map.begin + handle * kMappingStride, &record, 4) || record >= rec_count) continue;
        bool dup = false;
        for (int j = 0; j < r->count; ++j) dup |= r->light[j].record == record;
        if (!dup) r->light[r->count++] = {record, type, handle, instance};
    }
    return true;
}

struct Reference {
    bool valid = false;
    uintptr_t pres = 0, manager = 0, beams = 0, glows = 0;
    int lights = 0;
    uint32_t handle[kMaxPlayerLights];
    uint16_t type[kMaxPlayerLights];
    uint32_t template_handle = ~0u, template_preset = ~0u;
};
SRWLOCK g_ref_lock = SRWLOCK_INIT;
Reference g_ref;

void remember_player(const Resolved& r, uintptr_t beams) {
    Reference ref;
    ref.pres = r.pres; ref.manager = r.manager; ref.beams = beams;
    rdp(r.manager + kManagerGlows, &ref.glows);
    int tail = -1;
    for (int i = 0; i < r.count; ++i) {
        if (r.light[i].type == kTypeTailGlow && tail < 0) tail = i;
        ref.type[ref.lights] = r.light[i].type;
        ref.handle[ref.lights++] = r.light[i].handle;
    }
    if (tail < 0) tail = 0;
    if (r.count) {
        ref.template_handle = r.light[tail].handle;
        uintptr_t rec = beam_record(beams, r.light[tail].record);
        ref.valid = rec && rd(rec + kRecPreset, &ref.template_preset, 4);
    }
    AcquireSRWLockExclusive(&g_ref_lock);
    g_ref = ref;
    ReleaseSRWLockExclusive(&g_ref_lock);
}

// ---- CreateLight: Key/Index jedes Handles merken ---------------------------------------------------------------
const uint32_t kMaxHandles = 1u << 16;
uint32_t g_handle_key[kMaxHandles], g_handle_index[kMaxHandles];
uintptr_t g_handle_manager[kMaxHandles];

std::atomic<bool> g_build{false};
SRWLOCK g_lock = SRWLOCK_INIT;

struct Lamps {
    uintptr_t manager = 0, beams = 0;
    int count = 0;
    uint32_t handle[kMaxLamps], preset[kMaxLamps];
    uint32_t spare = ~0u;
    uint32_t cfg_rev = 0;
    bool lit = false;
    uint64_t last_frame = 0;
    float placed_pos[kMaxLamps][3];   // lokal
    int placed_n = 0;
    float placed_rot[kMaxLamps][4];   // lokal
    uintptr_t placed_pres = 0;
};
Lamps g_l;

void set_dirty() { const uint8_t one = 1; wr(g_l.beams + kBeamDirty, &one, 1); }

// PlateLampSize.csv: MediaName;Anzahl;Breite je Lampe (m)
std::map<long, std::vector<float>> g_lampw;
long id_of(const std::string& nm);
void load_lampsize(const wchar_t* fn) {
    FILE* f = _wfopen(fn, L"rb"); if (!f) return; char l[512];
    while (fgets(l, sizeof l, f)) { if (l[0] == '#') continue; char* p = strchr(l, ';'); if (!p) continue; *p = 0; std::string nm = l; for (auto& ch : nm) ch = (char)tolower((unsigned char)ch);
        char* q = p + 1; int n = (int)strtol(q, &q, 10); std::vector<float> v; for (int i = 0; i < n && *q == ';'; ++i) v.push_back(strtof(q + 1, &q));
        const long id = id_of(nm); if (id > 0 && (int)v.size() == n) g_lampw[id] = v; }
    fclose(f); char m[80]; snprintf(m, sizeof m, "Lamp sizes: %zu cars", g_lampw.size()); LOG(m);
}
float lamp_width(long id, int i) { auto it = g_lampw.find(id); return it != g_lampw.end() && i < (int)it->second.size() ? it->second[i] : 0.0f; }
bool write_preset(const Config& c, uint32_t preset, uint32_t template_preset, float range_override = -1.0f) {
    const float range = range_override > 0.0f ? range_override : c.range;
    uintptr_t ours = beam_preset(g_l.beams, preset), tmpl = beam_preset(g_l.beams, template_preset);
    uint8_t b[kBeamPresetStride], keep[8];
    if (!ours || !tmpl || !rd(tmpl, b, sizeof b) || !rd(ours + kPreUsers, keep, 8)) return false;
    const float zero[2] = {0.0f, 0.0f};
    static uint32_t s_logged_tmpl = ~0u;
    if (s_logged_tmpl != template_preset) {            // Werte der Spiel-Vorlage (Ruecklicht-Projektion) einmal ins Log
        s_logged_tmpl = template_preset;
        char m[400]; int k = snprintf(m, sizeof m, "Template %u:", template_preset);
        for (int o = 0x40; o < 0xB0 && k < 380; o += 4) { float v; memcpy(&v, b + o, 4); k += snprintf(m + k, sizeof m - k, " %02X=%g", o, v); }
        LOG(m);
    }
    memcpy(b + kPreUsers, keep, 8);
    memcpy(b + kPreRgb, c.rgb, 12);
    memcpy(b + kPreRange, &range, 4);
    memcpy(b + kPrePower, &c.power, 4);
    memcpy(b + kPreInner, &c.inner, 4);
    memcpy(b + kPreOuter, &c.outer, 4);
    memcpy(b + kPreThreshold, zero, 8);   // Tageslicht-Schwelle 0: immer an (Helligkeit steuern wir selbst)
    bool ok = wr(ours + 0x10, b + 0x10, sizeof b - 0x10);
    float back[4] = {0, 0, 0, 0};
    rd(ours + kPreRange, &back[0], 4); rd(ours + kPrePower, &back[1], 4); rd(ours + kPreInner, &back[2], 4); rd(ours + kPreOuter, &back[3], 4);
    char m[160]; snprintf(m, sizeof m, "Preset %u: range %.2f power %.2f cone %.0f/%.0f (target %.2f/%.2f/%.0f/%.0f) users=%u", preset, back[0], back[1], back[2], back[3], range, c.power, c.inner, c.outer, *(uint32_t*)keep);
    LOG(m);
    return ok;
}

bool lamp_ok(int i) {
    uint32_t beam, glow, preset = ~0u; uintptr_t rec;
    return mapping_of(g_l.manager, g_l.handle[i], &beam, &glow) && (rec = beam_record(g_l.beams, beam)) &&
           rd(rec + kRecPreset, &preset, 4) && preset == g_l.preset[i];
}

void lamps_dark() {
    for (int i = 0; i < g_l.count; ++i) g_orig_set_int(reinterpret_cast<void*>(g_l.manager), g_l.handle[i], 0.0f);
    g_l.lit = false;
}

// Auf dem Licht-Thread des Spiels, direkt nach einem seiner CreateLight-Aufrufe.
void lamps_build(const Reference& ref) {
    const Config c = config();
    void* mgr = reinterpret_cast<void*>(ref.manager);
    if (g_l.manager != ref.manager || g_l.beams != ref.beams) return;
    if (ref.template_handle >= kMaxHandles || g_handle_manager[ref.template_handle] != ref.manager) {
        LOG("Lamps not possible yet: car was loaded before the mod (switch car once or fast travel)");
        return;
    }
    while (g_l.count < g_want) {
        uint32_t p = g_l.spare;
        g_l.spare = ~0u;
        if (p == ~0u) {
            p = reinterpret_cast<AllocPresetFn>(g_base + kAllocBeamPreset)(reinterpret_cast<void*>(ref.beams + kBeamPresetPool));
            if (p == ~0u || !beam_preset(ref.beams, p)) { LOG("no free preset"); break; }
            const uint32_t no_users[2] = {0, 0};
            wr(beam_preset(ref.beams, p) + kPreUsers, no_users, 8);
        }
        if (!write_preset(c, p, ref.template_preset)) { g_l.spare = p; LOG("Writing preset failed"); break; }
        uint32_t h = ~0u, beam, glow;
        g_orig_create(mgr, &h, g_handle_key[ref.template_handle], g_handle_index[ref.template_handle]);
        if (h == ~0u || !mapping_of(ref.manager, h, &beam, &glow) || beam == ~0u) {
            g_l.spare = p; LOG("CreateLight returned nothing"); break;
        }
        reinterpret_cast<AttachPresetFn>(g_base + kAttachPreset)(reinterpret_cast<void*>(ref.beams), beam, p);
        const int i = g_l.count++;
        g_l.handle[i] = h; g_l.preset[i] = p;
    }
    g_l.cfg_rev = 0;   // Presets neu schreiben
    set_dirty();
    char m[96]; snprintf(m, sizeof m, "%d plate lamp(s) created", g_l.count);
    LOG(m);
}

uint32_t* detour_create(void* mgr, uint32_t* handle, uint32_t key, uint32_t index) {
    uint32_t* r = g_orig_create(mgr, handle, key, index);
    uint32_t h = ~0u;
    rd(reinterpret_cast<uintptr_t>(handle), &h, 4);
    if (h < kMaxHandles) {
        g_handle_key[h] = key; g_handle_index[h] = index; g_handle_manager[h] = reinterpret_cast<uintptr_t>(mgr);
    }
    if (g_build.load() && TryAcquireSRWLockExclusive(&g_lock)) {
        static Reference ref;
        AcquireSRWLockShared(&g_ref_lock);
        ref = g_ref;
        ReleaseSRWLockShared(&g_ref_lock);
        if (ref.valid && ref.manager == reinterpret_cast<uintptr_t>(mgr)) {
            g_build.store(false);
            lamps_build(ref);
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    return r;
}

// v2-Tabelle: Quelle = Lampenglas + Out * Normale, Richtung = zum Ziel auf der Platte, um Tilt zur Oberflaeche gekippt.
void aim_v2(const Config& c, const Entry* e, int i, float* pos, float* rot) {
    const float* g = e->pos[i]; const float* t = e->tgt[i]; const float* n = e->nrm[i];
    const float o = c.out + (e->ver >= 3 ? e->extra[i] : 0.0f);
    for (int k = 0; k < 3; ++k) pos[k] = g[k] + o * n[k];
    float d[3] = {t[0] - pos[0], t[1] - pos[1], t[2] - pos[2]};
    float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); if (l < 1e-5f) l = 1e-5f;
    const float tk = std::tan(c.tilt * 3.14159265f / 180.0f);
    for (int k = 0; k < 3; ++k) d[k] = d[k] / l - tk * n[k];
    l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); for (int k = 0; k < 3; ++k) d[k] /= l;
    // kuerzeste Drehung +z -> d
    float qx = -d[1], qy = d[0], qz = 0.0f, qw = 1.0f + d[2];
    if (qw < 1e-5f) { qx = 0; qy = 1; qz = 0; qw = 0; }
    const float nn = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    rot[0] = qx / nn; rot[1] = qy / nn; rot[2] = qz / nn; rot[3] = qw / nn;
}

// Lokale Lampenpositionen und Richtung aus der Config.
void layout(const Config& c, float pos[kMaxLamps][3], float rot[kMaxLamps][4]) {
    for (int i = 0; i < c.count; ++i) {
        pos[i][0] = c.x + (float(i) - 0.5f * float(c.count - 1)) * c.sep;
        pos[i][1] = c.y;
        pos[i][2] = c.z;
    }
    const float d2r = 3.14159265f / 180.0f;
    const float a = c.angle * d2r * 0.5f, ya = c.yaw * d2r * 0.5f;
    const float qx[4] = {std::sin(a), 0.0f, 0.0f, std::cos(a)};   // Kippen um x: z -> -y
    const float qy[4] = {0.0f, std::sin(ya), 0.0f, std::cos(ya)};
    float q[4];
    qmul(qy, qx, q);
    for (int i = 0; i < kMaxLamps; ++i) memcpy(rot[i], q, sizeof q);
}


bool car_world(uintptr_t pres, float* world16, float* car_rot) {
    if (!vtable_is(pres, kCarPresVtables, 2) || !rd(pres + kPresWorld, world16, 64) || !finite(world16, 16)) return false;
    alignas(16) float q[4];
    _mm_store_ps(q, reinterpret_cast<MatrixToQuatFn>(g_base + kMatrixToQuat)(world16));
    if (!finite(q, 4)) return false;
    memcpy(car_rot, q, 16);
    return true;
}

void place(const Reference& ref, const float* world, const float* car_rot, float level) {
    void* mgr = reinterpret_cast<void*>(ref.manager);
    for (int i = 0; i < g_l.placed_n && i < g_l.count; ++i) {
        alignas(16) float rot[4];
        // Zielrichtung im Autoraum (Tabelle: Lampe -> Kennzeichen) in Weltraum drehen und die Strahlachse der
        // Spiel-Lampe (BeamAxis) auf kuerzestem Weg dorthin drehen. Conj=1: Spiel nutzt die inverse Konvention.
        static const float kAxis[6][3] = {{0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}};
        const int ax = (g_cfg.axis >= 0 && g_cfg.axis < 6) ? g_cfg.axis : 0;
        static const float kZ[3] = {0, 0, 1};
        float t[3], d[3];
        qrot(g_l.placed_rot[i], kZ, t);
        qrot(car_rot, t, d);
        const float* a = kAxis[ax];
        float cx = a[1] * d[2] - a[2] * d[1], cy = a[2] * d[0] - a[0] * d[2], cz = a[0] * d[1] - a[1] * d[0];
        float cw = 1.0f + a[0] * d[0] + a[1] * d[1] + a[2] * d[2];
        if (cw < 1e-5f) {  // 180 Grad: beliebige Achse senkrecht zu a
            if (std::fabs(a[0]) < 0.9f) { cx = 0; cy = -a[2]; cz = a[1]; } else { cx = -a[1]; cy = a[0]; cz = 0; }
            cw = 0.0f;
        }
        const float nn = std::sqrt(cx * cx + cy * cy + cz * cz + cw * cw);
        rot[0] = cx / nn; rot[1] = cy / nn; rot[2] = cz / nn; rot[3] = cw / nn;
        if (g_cfg.conj) { rot[0] = -rot[0]; rot[1] = -rot[1]; rot[2] = -rot[2]; }
        float w[3];
        qrot(car_rot, g_l.placed_pos[i], w);
        alignas(16) const float pos[4] = {world[12] + w[0], world[13] + w[1], world[14] + w[2], 1.0f};
        g_orig_set_pos(mgr, g_l.handle[i], pos);
        g_orig_set_dir(mgr, g_l.handle[i], rot);
        if (level >= 0.0f) g_orig_set_int(mgr, g_l.handle[i], level);
    }
}

// Scheinwerferstand des Spielerautos: die Intensitaet, die das Spiel den Scheinwerfer-Lichtern (Typ 13/17) setzt.
// Sie folgt dem Scheinwerfer-Eingang (Taste/EO) und den automatischen Nachtscheinwerfern.
float g_hl_v[kMaxPlayerLights];
std::atomic<float> g_headlight_level{0.0f};
std::atomic<int> g_player_lamp{1};
// v4.26: Verschiebung der Lampen fuer die geladene Tuning-Variante (PlateLightVariants.csv)
float g_var_delta[8][3] = {}; std::atomic<int> g_var_n{0};
std::map<std::string, std::vector<float>> g_variants;
void load_variants(const wchar_t* fn) {
    FILE* f = _wfopen(fn, L"rb"); if (!f) return; char l[2048];
    while (fgets(l, sizeof l, f)) { if (l[0] == '#') continue; char* p = strchr(l, ';'); if (!p) continue; *p = 0; std::vector<float> v; char* q = p + 1; int n = (int)strtol(q, &q, 10);
        for (int i = 0; i < n * 3 && *q == ';'; ++i) v.push_back(strtof(q + 1, &q)); if ((int)v.size() == n * 3) g_variants[l] = v; }
    fclose(f); char m[80]; snprintf(m, sizeof m, "Lamp variants: %zu", g_variants.size()); LOG(m);
}
void set_variant(const std::string& rel) {
    auto it = g_variants.find(rel); int n = 0;
    if (it != g_variants.end()) { n = std::min<int>(8, int(it->second.size() / 3)); for (int i = 0; i < n; ++i) for (int k = 0; k < 3; ++k) g_var_delta[i][k] = it->second[i * 3 + k]; }
    if (g_var_n.exchange(n) != n || n) { char m[200]; snprintf(m, sizeof m, "Lamp position for %s: %s", rel.c_str(), n ? "variant offset" : "default"); LOG(m); }
}   // v4.23: 0 = geladenes Heckteil des Spielerautos hat keine Kennzeichenlampe (Tuning-Teil)
int g_hl_state = -1;

void lamps_frame(const Reference& ref) {
    const Config c = config();
    poll_config();
    if (g_l.manager != ref.manager || g_l.beams != ref.beams) {
        g_l = Lamps{};
        g_l.manager = ref.manager; g_l.beams = ref.beams;
    }
    g_l.last_frame = GetTickCount64();
    if (!c.on) { if (g_l.lit) lamps_dark(); return; }

    int kept = 0;
    for (int i = 0; i < g_l.count; ++i)
        if (lamp_ok(i)) { g_l.handle[kept] = g_l.handle[i]; g_l.preset[kept] = g_l.preset[i]; ++kept; }
    g_l.count = kept;
    const Entry* ent = g_use_table && g_tab_ids.load() ? entry_for(g_player_car_id.load()) : nullptr;
    const bool tabled = g_use_table && g_tab_ids.load() > 0;
    g_want = tabled ? (ent ? ent->n : 0) : c.count;
    if (g_l.count < g_want) g_build.store(true);
    if (!g_l.count) return;

    static const Entry* s_ent_written = nullptr;
    if (g_l.cfg_rev != g_cfg_rev.load() || ent != s_ent_written) {
        bool dirty = false;
        for (int i = 0; i < g_l.count; ++i) {
            const float rr = (ent && ent->ver >= 3 && i < ent->n && ent->reach[i] > 0.0f) ? ent->reach[i] * c.range_scale : -1.0f;
            Config cc = c; if (ent && ent->has_rgb && !c.rgb_override) memcpy(cc.rgb, ent->rgb, 12);
            {   // Breite Lampe = breiterer Lichtkegel, kleine Punktlampe = schmaler (PlateLampSize.csv, Bezug 5 cm)
                const float w = lamp_width(g_player_car_id.load(), i);
                if (w > 0.0f && ini_f(L"Light", L"ConeByLampSize", 1.0f) > 0.5f) { float f = 0.7f + 0.3f * (w / 0.05f); f = f < 1.0f ? 1.0f : f > 1.3f ? 1.3f : f;   // kleine Lampen: normaler Kegel, nie schmaler
                    cc.inner *= f; cc.outer = std::min(170.0f, cc.outer * f); } }
            dirty |= write_preset(cc, g_l.preset[i], ref.template_preset, rr);
        }
        s_ent_written = ent;
        if (dirty) set_dirty();
        g_l.cfg_rev = g_cfg_rev.load();
    }
    alignas(16) float world[16], car_rot[4];
    if (!car_world(ref.pres, world, car_rot)) return;

    if (ent) {
        for (int i = 0; i < ent->n; ++i) { if (ent->v2) aim_v2(c, ent, i, g_l.placed_pos[i], g_l.placed_rot[i]);
            else { memcpy(g_l.placed_pos[i], ent->pos[i], 12); memcpy(g_l.placed_rot[i], ent->quat[i], 16); }
            g_l.placed_pos[i][0] += c.offx; g_l.placed_pos[i][1] += c.offy; g_l.placed_pos[i][2] += c.offz; }
    } else layout(c, g_l.placed_pos, g_l.placed_rot);
    if (!ent) for (int i = 0; i < g_l.count; ++i) { g_l.placed_pos[i][0] += c.offx; g_l.placed_pos[i][1] += c.offy; g_l.placed_pos[i][2] += c.offz; }
    if (ent) { int vn = g_var_n.load(); for (int i = 0; i < ent->n && i < vn; ++i) for (int k = 0; k < 3; ++k) g_l.placed_pos[i][k] += g_var_delta[i][k]; }
    g_l.placed_n = g_want < g_l.count ? g_want : g_l.count;
    g_l.placed_pres = ref.pres;
    float level = c.brightness;
    if (c.only_dark) level *= g_headlight_level.load();
    if (tabled && !ent) level = 0.0f;
    if (!g_player_lamp.load()) level = 0.0f;
    for (int i = g_l.placed_n; i < g_l.count; ++i) g_orig_set_int(reinterpret_cast<void*>(ref.manager), g_l.handle[i], 0.0f);
    place(ref, world, car_rot, level);
    // Glow-Sprite am Lampenglas: leuchtet mit dem Fahrzeuglicht (ohne Tabelleneintrag aus)
    {
        float gl = (tabled && !ent) ? 0.0f : c.glow * (c.only_dark ? g_headlight_level.load() : 1.0f);
        if (!g_player_lamp.load()) gl = 0.0f;
        for (int i = 0; i < g_l.count; ++i) {
            uint32_t beam, glow;
            if (ref.glows && mapping_of(ref.manager, g_l.handle[i], &beam, &glow) && glow != ~0u)
                reinterpret_cast<GlowIntensityFn>(g_base + kGlowSetIntensity)(reinterpret_cast<void*>(ref.glows), glow, i < g_l.placed_n ? gl : 0.0f);
        }
    }
    g_l.lit = true;
}

void detour_set_int(void* mgr, uint32_t handle, float v) {
    g_orig_set_int(mgr, handle, v);
    static Reference ref;
    if (!TryAcquireSRWLockExclusive(&g_lock)) return;
    AcquireSRWLockShared(&g_ref_lock);
    bool mine = g_ref.valid && reinterpret_cast<uintptr_t>(mgr) == g_ref.manager;
    if (mine) ref = g_ref;
    ReleaseSRWLockShared(&g_ref_lock);
    bool player_light = false;
    for (int k = 0; mine && k < ref.lights; ++k) player_light |= ref.handle[k] == handle;
    if (player_light) {
        for (int k = 0; k < ref.lights; ++k)
            if (ref.handle[k] == handle && (ref.type[k] == 13 || ref.type[k] == 17)) g_hl_v[k] = v;
        float lvl = 0.0f;
        for (int k = 0; k < ref.lights; ++k)
            if ((ref.type[k] == 13 || ref.type[k] == 17) && g_hl_v[k] > lvl) lvl = g_hl_v[k];
        if (!std::isfinite(lvl) || lvl < 0.0f) lvl = 0.0f;
        if (lvl > 1.0f) lvl = 1.0f;
        g_headlight_level.store(lvl);
        const int st = lvl > 0.02f ? 1 : 0;
        if (st != g_hl_state) {
            g_hl_state = st;
            char m[96]; snprintf(m, sizeof m, "Headlights %s (level %.2f)", st ? "ON" : "OFF", lvl);
            LOG(m);
        }
        lamps_frame(ref);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

SRWLOCK g_render_lock = SRWLOCK_INIT;

void detour_beam_upload(void* system_p, void* graphics) {
    const uintptr_t system = reinterpret_cast<uintptr_t>(system_p);
    if (!TryAcquireSRWLockExclusive(&g_render_lock)) { g_orig_beam(system_p, graphics); return; }
    static Resolved r;
    if (resolve_player(system, &r)) remember_player(r, system);
    if (TryAcquireSRWLockExclusive(&g_lock)) {
        if (g_l.lit && GetTickCount64() - g_l.last_frame > 250 && g_l.beams == system) lamps_dark();
        else if (g_l.lit && g_l.beams == system && g_l.placed_n) {
            Reference ref;
            AcquireSRWLockShared(&g_ref_lock);
            ref = g_ref;
            ReleaseSRWLockShared(&g_ref_lock);
            alignas(16) float world[16], car_rot[4];
            if (ref.manager == g_l.manager && car_world(ref.pres, world, car_rot)) place(ref, world, car_rot, -1.0f);
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    g_orig_beam(system_p, graphics);
    ReleaseSRWLockExclusive(&g_render_lock);
}

bool match(const char* at, const char* bytes, size_t n) {
    uint8_t b[16];
    return rd(reinterpret_cast<uintptr_t>(at), b, n) && memcmp(b, bytes, n) == 0;
}

// ==== Mess-Lauf "Kennzeichen-Textur am Spielerauto" (PlateScan.log neben der DLL) ==============================
// Sucht im Speicher die Pfad-Hashes unserer Kennzeichen-Texturen/-Materialien und Namen, und von der Praesentation
// des Spielerautos aus (Zeigerkette), welche dieser Fundstellen erreichbar sind.
FILE* g_scan = nullptr;
void SL(const char* f, ...) {
    if (!g_scan) return; va_list a; va_start(a, f); vfprintf(g_scan, f, a); va_end(a); fputc('\n', g_scan); fflush(g_scan);
}
struct Needle { const char* name; uint32_t v; };
const Needle kNeedles[] = {
    {"eu_atlas", 0xd328333d}, {"eu_nrml", 0xd08009cc}, {"eu_glos", 0x150bb089},
    {"us_atlas", 0xb2ee51ed}, {"us_nrml", 0x931e4560}, {"us_glos", 0xa29cbed8},
};
const char* kStrings[] = {"hzn_p1", "hzn_plate_lp3", "hzn_plate_us", "hzn_plate_atlas2", "hzn_us_atlas"};
struct Hit { uintptr_t a; int kind; };
std::vector<Hit> g_hits;
void hexdump(uintptr_t a, int before, int after) {
    uint8_t b[256]; int n = before + after; if (n > 256) n = 256;
    if (!rd(a - before, b, n)) return;
    char line[600]; int k = 0;
    for (int i = 0; i < n; ++i) k += snprintf(line + k, sizeof line - k, "%s%02x", i == before ? "|" : (i % 8 ? "" : " "), b[i]);
    SL("    %s", line);
}
void scan_memory() {
    const int NN = sizeof kNeedles / sizeof kNeedles[0], NS = sizeof kStrings / sizeof kStrings[0];
    int cnt[16] = {};
    std::vector<uint8_t> buf(1 << 20);
    MEMORY_BASIC_INFORMATION mbi; uintptr_t addr = 0x10000; uint64_t total = 0;
    while (addr < 0x7FFFFFFF0000ull && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi)) {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        const DWORD pr = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_READONLY || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_READWRITE)) {
            for (uintptr_t p = base; p < end; p += buf.size()) {
                size_t n = std::min<uintptr_t>(buf.size(), end - p);
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), buf.data(), n, &got) || got < 8) continue;
                total += got;
                for (size_t i = 0; i + 4 <= got; i += 4) {
                    uint32_t v; memcpy(&v, &buf[i], 4);
                    for (int k = 0; k < NN; ++k) if (v == kNeedles[k].v) { if (cnt[k]++ < 400) g_hits.push_back({p + i, k}); }
                }
                for (int k = 0; k < NS; ++k) {
                    const size_t L = strlen(kStrings[k]);
                    const uint8_t* b0 = buf.data(); const uint8_t* q = b0; const uint8_t* e = b0 + got;
                    while (q + L <= e && (q = (const uint8_t*)memchr(q, kStrings[k][0], e - q - L + 1))) {
                        if (!memcmp(q, kStrings[k], L)) { if (cnt[NN + k]++ < 400) g_hits.push_back({p + (q - b0), NN + k}); }
                        ++q;
                    }
                }
            }
        }
        addr = end;
    }
    SL("Memory read: %.1f MB", total / 1048576.0);
    for (int k = 0; k < NN; ++k) SL("Hash %-9s %08x: %d hits", kNeedles[k].name, kNeedles[k].v, cnt[k]);
    for (int k = 0; k < NS; ++k) SL("Text %-16s: %d hits", kStrings[k], cnt[NN + k]);
    int shown[16] = {};
    for (const Hit& h : g_hits) if (shown[h.kind]++ < 6) {
        SL("  %s @ %p", h.kind < NN ? kNeedles[h.kind].name : kStrings[h.kind - NN], (void*)h.a); hexdump(h.a, 48, 64);
    }
    std::sort(g_hits.begin(), g_hits.end(), [](const Hit& x, const Hit& y) { return x.a < y.a; });
}
bool readable_ptr(uintptr_t v) { return v > 0x10000 && v < 0x7FFFFFFF0000ull && (v & 7) == 0; }
void bfs_from(uintptr_t root, const char* label) {
    const int NN = sizeof kNeedles / sizeof kNeedles[0];
    struct Node { uintptr_t a; int parent; uint16_t off; uint8_t depth; };
    std::vector<Node> q; q.reserve(600000); q.push_back({root, -1, 0, 0});
    std::vector<uintptr_t> seen; seen.reserve(600000); seen.push_back(root);
    const size_t SPAN = 0x400; uint8_t obj[SPAN]; int found = 0;
    auto chain = [&](int i, char* out, size_t n) { int k = 0; std::vector<int> c; for (int j = i; j >= 0; j = q[j].parent) c.push_back(j);
        for (int t = (int)c.size() - 1; t >= 0; --t) k += snprintf(out + k, n - k, t == (int)c.size() - 1 ? "pres" : "->+%X", q[c[t]].off);
    };
    for (size_t qi = 0; qi < q.size() && q.size() < 550000 && found < 60; ++qi) {
        const Node nd = q[qi];
        if (!rd(nd.a, obj, SPAN)) { if (!rd(nd.a, obj, 0x80)) continue; memset(obj + 0x80, 0, SPAN - 0x80); }
        // Fundstellen in diesem Objekt?
        auto it = std::lower_bound(g_hits.begin(), g_hits.end(), Hit{nd.a, 0}, [](const Hit& x, const Hit& y) { return x.a < y.a; });
        for (; it != g_hits.end() && it->a < nd.a + SPAN && found < 60; ++it) {
            char c[400]; chain((int)qi, c, sizeof c);
            SL("[%s] %s +0x%X in %s (depth %d)", label, it->kind < NN ? kNeedles[it->kind].name : kStrings[it->kind - NN], (unsigned)(it->a - nd.a), c, nd.depth);
            ++found;
        }
        if (nd.depth >= 6) continue;
        for (size_t o = 0; o + 8 <= SPAN; o += 8) {
            uintptr_t v; memcpy(&v, obj + o, 8);
            if (!readable_ptr(v)) continue;
            if (std::find(seen.end() - std::min<size_t>(seen.size(), 64), seen.end(), v) != seen.end()) continue;
            seen.push_back(v);
            q.push_back({v, (int)qi, (uint16_t)o, (uint8_t)(nd.depth + 1)});
        }
    }
    SL("[%s] nodes visited: %zu, hits: %d", label, q.size(), found);
}
// Rueckwaerts-Suche: wer zeigt in [lo,hi)? (alle beschreibbaren Bereiche, 8-Byte-Werte)
struct Range { uintptr_t lo, hi; int tag; };
std::vector<std::pair<uintptr_t, int>> find_referrers(std::vector<Range> rs, size_t max_per_tag) {
    std::sort(rs.begin(), rs.end(), [](const Range& x, const Range& y) { return x.lo < y.lo; });
    std::vector<std::pair<uintptr_t, int>> out; std::vector<size_t> per(4096, 0);
    std::vector<uint8_t> buf(1 << 20);
    MEMORY_BASIC_INFORMATION mbi; uintptr_t addr = 0x10000;
    const uintptr_t gmin = rs.front().lo, gmax = rs.back().hi;
    while (addr < 0x7FFFFFFF0000ull && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi)) {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        const DWORD pr = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_READWRITE)) {
            for (uintptr_t p = base; p < end; p += buf.size()) {
                size_t n = std::min<uintptr_t>(buf.size(), end - p); SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), buf.data(), n, &got) || got < 8) continue;
                for (size_t i = 0; i + 8 <= got; i += 8) {
                    uintptr_t v; memcpy(&v, &buf[i], 8);
                    if (v < gmin || v >= gmax) continue;
                    auto it = std::upper_bound(rs.begin(), rs.end(), v, [](uintptr_t x, const Range& r) { return x < r.lo; });
                    if (it == rs.begin()) continue; --it;
                    if (v < it->hi && per[it->tag & 4095]++ < max_per_tag) out.push_back({p + i, it->tag});
                }
            }
        }
        addr = end;
    }
    return out;
}
void dump_obj(const char* what, uintptr_t a, int before, int after) { SL("  %s @ %p", what, (void*)a); hexdump(a, before, after); }
DWORD WINAPI scan_thread(LPVOID) {
    while (!g_player_pres.load()) Sleep(500);
    Sleep(15000);
    wchar_t path[MAX_PATH]; wcscpy_s(path, g_ini); wchar_t* d = wcsrchr(path, L'\\'); if (d) wcscpy_s(d + 1, MAX_PATH - (d + 1 - path), L"PlateScan.log");
    _wfopen_s(&g_scan, path, L"w");
    LOG("Measurement run 2 plate texture started (PlateScan.log)");
    SL("Player car id %ld, presentation %p", g_player_car_id.load(), (void*)g_player_pres.load());
    scan_memory();
    const int NN = sizeof kNeedles / sizeof kNeedles[0];
    // A) Textur-Bindungen im Material (Slot-Hash 8 Byte vor ... Pfad-Hash) und Hash-Tabellen der Modelle
    std::vector<Range> rs; int tag = 0;
    std::vector<uintptr_t> tgt;
    for (const Hit& h : g_hits) {
        if (h.kind >= NN) continue;
        uint32_t s0 = 0; rd(h.a - 0x30, &s0, 4);
        const bool binding = (s0 == 0xd98cd96d || s0 == 0x9424d990 || s0 == 0x6e7a2ec0);
        if (binding) { SL("Binding %s @ %p (slot %08x)", kNeedles[h.kind].name, (void*)h.a, s0); hexdump(h.a - 0x30, 64, 160); }
        if (tag < 200) { rs.push_back({h.a - 0x200, h.a + 0x40, tag}); tgt.push_back(h.a); ++tag; }
    }
    // B) Pfade der geladenen HZN-Modelle (Text "\hzn\hzn_")
    {
        const char* pat = "\\hzn\\hzn_"; const size_t L = strlen(pat); int shown = 0;
        std::vector<uint8_t> buf(1 << 20); MEMORY_BASIC_INFORMATION mbi; uintptr_t addr = 0x10000;
        while (addr < 0x7FFFFFFF0000ull && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) && shown < 80) {
            uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize; const DWORD pr = mbi.Protect & 0xFF;
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_READONLY || pr == PAGE_WRITECOPY)) {
                for (uintptr_t p = base; p < end && shown < 80; p += buf.size()) {
                    size_t n = std::min<uintptr_t>(buf.size(), end - p); SIZE_T got = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), buf.data(), n, &got) || got < L) continue;
                    for (size_t i = 0; i + L <= got && shown < 80; ++i) {
                        if ((buf[i] | 0x20) != 'h' && buf[i] != '\\') continue;
                        bool m = true; for (size_t k = 0; k < L && m; ++k) m = (char)(buf[i + k] | (buf[i + k] >= 'A' && buf[i + k] <= 'Z' ? 0x20 : 0)) == pat[k];
                        if (!m) continue;
                        size_t s0 = i; while (s0 > 0 && i - s0 < 120 && buf[s0 - 1] >= 0x20 && buf[s0 - 1] < 0x7f) --s0;
                        size_t e0 = i; while (e0 < got && e0 - i < 120 && buf[e0] >= 0x20 && buf[e0] < 0x7f) ++e0;
                        std::string str(reinterpret_cast<char*>(&buf[s0]), e0 - s0);
                        SL("Path @ %p: %s", (void*)(p + s0), str.c_str());
                        if (tag < 260) { rs.push_back({p + s0 - 0x100, p + e0 + 0x10, tag}); tgt.push_back(p + s0); ++tag; }
                        ++shown; i = e0;
                    }
                }
            }
            addr = end;
        }
    }
    // C) Wer zeigt auf diese Stellen? (2 Stufen)
    if (!rs.empty()) {
        auto r1 = find_referrers(rs, 6);
        SL("Level 1: %zu references", r1.size());
        std::vector<Range> rs2; int tag2 = 0;
        for (auto& pr : r1) {
            uintptr_t v = 0; rd(pr.first, &v, 8);
            SL("  Reference to target %d (%p, target+%lld) from %p", pr.second, (void*)tgt[pr.second], (long long)v - (long long)tgt[pr.second], (void*)pr.first);
            hexdump(pr.first, 64, 96);
            if (tag2 < 300) rs2.push_back({pr.first - 0x100, pr.first + 8, tag2++});
        }
        if (!rs2.empty()) {
            auto r2 = find_referrers(rs2, 3);
            SL("Level 2: %zu references", r2.size());
            for (auto& pr : r2) { SL("  Reference to level1[%d] (%p) from %p", pr.second, (void*)r1[pr.second].first, (void*)pr.first); hexdump(pr.first, 32, 64); }
        }
    }
    // D) Spielerauto: Praesentation roh
    dump_obj("presentation", g_player_pres.load(), 0, 256);
    SL("done");
    LOG("Measurement run done");
    if (g_scan) fclose(g_scan); g_scan = nullptr;
    return 0;
}
// ==== Spieler-Kennzeichen (DURANCE nur am eigenen Auto) ===========================================================
// Jedes Auto hat ein eigenes Kennzeichen-Material (Auto-Id im Gross/Klein-Muster von "_library\textures" im
// Texturpfad). Ein zusaetzliches Material mit den Spieler-Texturen (*_sp) ist ueber ein verstecktes Mesh geladen.
// Hier: die drei Textur-Bindungen (Diffuse/Normal/Glanz, je 0x70 Byte) im Material des Spielerautos auf die
// Bindungen des Spieler-Materials umbiegen (Felder +0x10..+0x57: Pfad, Laenge, Hash, Textur-Verweis).
const uint32_t kSlot0 = 0x6dd98cd9, kSlot1 = 0x90d92494, kSlot2 = 0xc02e7a6e;   // d98cd96d / 9424d990 / 6e7a2ec0 (Bytes)
const uint32_t kX0 = 0x01c3b2a1, kX1 = 0x02c3b2a1, kX2 = 0x03c3b2a1;   // Zusatz-Slots a1b2c301..03
const size_t kRec = 0x70, kCopyLo = 0x10, kCopyHi = 0x58;
struct Swap { uintptr_t rec; uint8_t orig[3][kRec]; uint8_t mine[3][kRec]; bool on; };
Swap g_swap{};
long g_swap_id = 0;
bool read_path(uintptr_t rec, char* out, size_t n) {
    uintptr_t sp = 0; uint64_t len = 0;
    if (!rd(rec + 0x10, &sp, 8) || !rd(rec + 0x20, &len, 8) || len < 10 || len >= n) return false;
    if (!rd(sp, out, (size_t)len)) return false;
    out[len] = 0; return true;
}
int decode_id(const char* path) {
    const char* seg = "\\cars\\_library\\textures\\"; size_t L = strlen(seg), n = strlen(path);
    for (size_t i = 0; i + L <= n; ++i) {
        bool m = true; for (size_t k = 0; k < L && m; ++k) m = tolower((unsigned char)path[i + k]) == seg[k];
        if (!m) continue;
        const char* q = path + i + strlen("\\cars\\_"); int v = 0, k = 0;
        for (int j = 0; j < 16; ++j) { if (q[j] == '\\') continue; if (isupper((unsigned char)q[j])) v |= 1 << k; ++k; }
        return v;
    }
    return -1;
}
bool records_ok(uintptr_t rec) {
    uint32_t a[2], b, c;
    return rd(rec, a, 8) && a[0] == kSlot0 && a[1] == 5 && rd(rec + kRec, &b, 4) && b == kSlot1 && rd(rec + 2 * kRec, &c, 4) && c == kSlot2;
}
void swap_restore() {
    if (!g_swap.on) return;
    bool same = records_ok(g_swap.rec);
    for (int k = 0; same && k < 3; ++k) { uint8_t cur[kRec]; same = rd(g_swap.rec + k * kRec, cur, kRec) && !memcmp(cur + kCopyLo, g_swap.mine[k] + kCopyLo, kCopyHi - kCopyLo); }
    if (same) {
        for (int k = 0; k < 3; ++k) wr(g_swap.rec + k * kRec + kCopyLo, g_swap.orig[k] + kCopyLo, kCopyHi - kCopyLo);
        LOG("Player plate: reset");
    } else LOG("Player plate: old material already unloaded, nothing to reset");
    g_swap.on = false;
}
// Sucht im Gast-Speicher alle Kennzeichen-Bindungen; liefert Quelle (sp) je Typ und Ziel fuer die Spieler-Id.
// 0 = noch nicht fertig (weiter aufrufen), 1 = fertig ohne Treffer, 2 = getauscht
int swap_scan(long id, char* msg, size_t mn, bool restart) {
    static uintptr_t src[2], dst[2], addr; static std::string idl; static int nrec, regions, cands; static unsigned long long scanned; static ULONGLONG t0;
    if (restart) { src[0] = src[1] = dst[0] = dst[1] = 0; nrec = regions = cands = 0; scanned = 0; idl.clear(); addr = 0x40000000000ull; t0 = GetTickCount64();  }
    std::vector<uint8_t> buf(1 << 20);
    MEMORY_BASIC_INFORMATION mbi; const ULONGLONG slice = GetTickCount64();
    while (addr < 0x7FF00000000ull && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi)) {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize; if (addr > base) base = addr; const DWORD pr = mbi.Protect & 0xFF;
        ++regions;
        if (mbi.RegionSize >= (64u << 20) && mbi.State == MEM_COMMIT) { char d[200]; snprintf(d, sizeof d, "Player plate: large region %llx, %llu MB, protect %lx", (unsigned long long)base, (unsigned long long)(mbi.RegionSize >> 20), (unsigned long)mbi.Protect); LOG(d); }
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_READONLY || pr == PAGE_EXECUTE_READ)) {
            for (uintptr_t p = base; p < end; p += buf.size() - 0x200) {
                size_t n = std::min<uintptr_t>(buf.size(), end - p); SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), buf.data(), n, &got) || got < 0x100) continue;
                scanned += got;
                if (GetTickCount64() - slice > 2000) { addr = p + buf.size() - 0x200; char d[200]; snprintf(d, sizeof d, "Player plate: scan at %llx, %llu MB so far", (unsigned long long)addr, scanned >> 20); LOG(d); return 0; }
                for (size_t i = 0; i + 2 * kRec + 8 <= got; i += 8) {
                    uint32_t v; memcpy(&v, &buf[i], 4);
                    if (v != kSlot0) continue;
                    uint32_t t, b, c; memcpy(&t, &buf[i + 4], 4); memcpy(&b, &buf[i + kRec], 4); memcpy(&c, &buf[i + 2 * kRec], 4);
                    if (t != 5 || b != kSlot1 || c != kSlot2) continue;
                    ++cands;
                    char path[200]; const uintptr_t rec = p + i; ++nrec;
                    { static int hx = 0; uint8_t r[kRec]; if (hx < 0 && rd(rec, r, kRec)) { ++hx; char d[400]; int q = snprintf(d, sizeof d, "Player plate: hex %llx:", (unsigned long long)rec); for (int o = 0; o < (int)kRec; o += 8) { uint64_t v; memcpy(&v, r + o, 8); q += snprintf(d + q, sizeof d - q, " %llx", (unsigned long long)v); } LOG(d); } }
                    { static int dumped = 0;
                      if (dumped < 0) {
                        uint8_t r[kRec]; if (rd(rec, r, kRec)) {
                          for (int o = 8; o + 8 <= (int)kRec; o += 8) {
                            uint64_t q; memcpy(&q, r + o, 8);
                            if (q < 0x10000 || q > 0x7FFFFFFFFFFFull) continue;
                            char t[64]; if (!rd((uintptr_t)q, t, 63)) continue; t[63] = 0;
                            int L = 0; while (L < 63 && (unsigned char)t[L] >= 32 && (unsigned char)t[L] < 127) ++L;
                            std::string ls(t, L); for (auto& ch : ls) ch = (char)tolower((unsigned char)ch);
                            if (ls.find("atlas") != std::string::npos || ls.find("hzn") != std::string::npos) {
                              char d[300]; snprintf(d, sizeof d, "Player plate: candidate %llx +%x -> \"%s\" (len@+20=%llu)", (unsigned long long)rec, o, std::string(t, L).c_str(), (unsigned long long)*(uint64_t*)(r + 0x20)); LOG(d); ++dumped; break;
                            }
                          }
                        }
                      } }
                    if (!read_path(rec, path, sizeof path)) continue;
                    char low[200]; for (size_t k = 0; k < sizeof low; ++k) { low[k] = (char)tolower((unsigned char)path[k]); if (!path[k]) break; }
                    const int us = strstr(low, "hzn_us_atlas") ? 1 : strstr(low, "hzn_plate_atlas2") ? 0 : -1;
                    if (us < 0) continue;
                    { char q[40]; snprintf(q, sizeof q, " %s%d@%llx", us ? "U" : "E", decode_id(path), (unsigned long long)rec); idl += q; }
                    { uint32_t y0, y1, y2; uint8_t cur[3][kRec], ex[3][kRec];
                      if (rd(rec + 3 * kRec, &y0, 4) && rd(rec + 4 * kRec, &y1, 4) && rd(rec + 5 * kRec, &y2, 4) && y0 == kX0 && y1 == kX1 && y2 == kX2) {
                          bool diff = false; for (int k = 0; k < 3; ++k) { if (!rd(rec + k * kRec, cur[k], kRec) || !rd(rec + (3 + k) * kRec, ex[k], kRec)) { diff = false; break; } if (memcmp(cur[k] + kCopyLo, ex[k] + kCopyLo, kCopyHi - kCopyLo)) diff = true; }
                          if (false && diff) { for (int k = 0; k < 3; ++k) wr(rec + k * kRec + kCopyLo, ex[k] + kCopyLo, kCopyHi - kCopyLo); char d[200]; snprintf(d, sizeof d, "TEST-ALL: entry %llx (id %d) switched to DURANCE", (unsigned long long)rec, decode_id(path)); LOG(d); }
                      } else { char d[200]; snprintf(d, sizeof d, "TEST-ALL: entry %llx (id %d) without extra slots (%x %x %x)", (unsigned long long)rec, decode_id(path), y0, y1, y2); static std::string seen; if (seen.find(d) == std::string::npos) { LOG(d); seen += d; } } }
                    if (decode_id(path) == id) { uint32_t x0, x1, x2; if (rd(rec + 3 * kRec, &x0, 4) && rd(rec + 4 * kRec, &x1, 4) && rd(rec + 5 * kRec, &x2, 4) && x0 == kX0 && x1 == kX1 && x2 == kX2) { dst[us] = rec; src[us] = rec + 3 * kRec; } else { char d[200]; snprintf(d, sizeof d, "Player plate: target %llx without extra slots (hash +3: %x +4: %x +5: %x)", (unsigned long long)rec, x0, x1, x2); LOG(d); } }
                }
            }
        }
        addr = end;
    }
    int ty = dst[0] ? 0 : dst[1] ? 1 : -1;
    { static std::string lastidl; if (lastidl != idl) { lastidl = idl; LOG(("Player plate: plate materials:" + idl).c_str()); } }
    if (0) { char d[240]; snprintf(d, sizeof d, "Player plate: scan done: %d regions, %llu MB, %d candidates, %lld ms", regions, scanned >> 20, cands, (long long)(GetTickCount64() - t0)); LOG(d); }
    msg[0] = 0; snprintf(msg, mn, "Bindings %d, source EU %s US %s, target %s", nrec, src[0] ? "yes" : "no", src[1] ? "yes" : "no", ty < 0 ? "no" : ty ? "US" : "EU");
    if (ty < 0 || !src[ty]) return 1;
    Swap s{}; s.rec = dst[ty];
    for (int k = 0; k < 3; ++k) {
        if (!rd(dst[ty] + k * kRec, s.orig[k], kRec) || !rd(src[ty] + k * kRec, s.mine[k], kRec)) return 1;
    }
    char h[300]; int q = 0;
    for (int k = 0; k < 3; ++k) {
        uint64_t a, b; memcpy(&a, s.orig[k] + 0x50, 8); memcpy(&b, s.mine[k] + 0x50, 8);
        q += snprintf(h + q, sizeof h - q, " [%d] reference %llx -> %llx", k, (unsigned long long)a, (unsigned long long)b);
    }
    LOG(h);
    for (int k = 0; k < 3; ++k) wr(dst[ty] + k * kRec + kCopyLo, s.mine[k] + kCopyLo, kCopyHi - kCopyLo);
    s.on = true; g_swap = s;
    return 2;
}
// Diagnose: sucht Textfetzen (ohne Gross/Klein) im Gast-Speicher und loggt Treffer samt Umgebung.
void text_hunt(const char* needle, int maxhits) {
    const size_t nl = strlen(needle); std::vector<uint8_t> buf(1 << 20); MEMORY_BASIC_INFORMATION mbi; uintptr_t addr = 0x60000000000ull; int hits = 0;
    while (addr < 0x70000000000ull && hits < maxhits && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi)) {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize; const DWORD pr = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_READONLY)) {
            for (uintptr_t p = base; p < end && hits < maxhits; p += buf.size() - 0x200) {
                size_t n = std::min<uintptr_t>(buf.size(), end - p); SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), buf.data(), n, &got) || got < nl + 8) continue;
                for (size_t i = 0; i + nl <= got && hits < maxhits; ++i) {
                    size_t k = 0; while (k < nl && tolower(buf[i + k]) == needle[k]) ++k;
                    if (k < nl) continue;
                    char t[100]; size_t a = i >= 60 ? i - 60 : 0, L = std::min<size_t>(99, got - a); size_t m = 0;
                    for (; m < L; ++m) { uint8_t c = buf[a + m]; t[m] = (c >= 32 && c < 127) ? (char)c : '.'; } t[m] = 0;
                    char d[260]; snprintf(d, sizeof d, "Text search '%s' at %llx (region %llx, protect %lx): %s", needle, (unsigned long long)(p + i), (unsigned long long)base, (unsigned long)mbi.Protect, t); LOG(d); ++hits;
                }
            }
        }
        addr = end;
    }
    char d[100]; snprintf(d, sizeof d, "Text search '%s': %d hits", needle, hits); LOG(d);
}
// Diagnose 6: exakter (gross/klein) Pfad des Spielerautos + wer darauf zeigt.
void exact_hunt(long id) {
    std::string base = "Game:\\Media\\cars\\_library\\textures\\hzn\\hzn_plate_atlas2.swatchbin";
    std::string alt = "Game:\\Media\\cars\\_library\\textures\\hzn\\hzn_us_atlas.swatchbin";
    for (int variant = 0; variant < 2; ++variant) {
        std::string nd = variant ? alt : base; int i0 = (int)nd.find("_library"), k = 0;
        for (int j = i0 + 1; j < (int)nd.size() && k < 15; ++j) { if (nd[j] == '\\') continue; if ((id >> k) & 1) nd[j] = (char)toupper((unsigned char)nd[j]); ++k; }
        std::vector<uintptr_t> hits; std::vector<uint8_t> buf(1 << 20); MEMORY_BASIC_INFORMATION mbi; uintptr_t addr = 0x60000000000ull;
        while (addr < 0x70000000000ull && hits.size() < 6 && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi)) {
            uintptr_t b = reinterpret_cast<uintptr_t>(mbi.BaseAddress), e = b + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD)) for (uintptr_t p = b; p < e && hits.size() < 6; p += buf.size() - 0x200) {
                size_t n = std::min<uintptr_t>(buf.size(), e - p); SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), buf.data(), n, &got) || got < nd.size()) continue;
                for (size_t i = 0; i + nd.size() <= got && hits.size() < 6; ++i) if (buf[i] == 'G' && !memcmp(&buf[i], nd.data(), nd.size())) hits.push_back(p + i);
            }
            addr = e;
        }
        char d[300]; snprintf(d, sizeof d, "Exact search id %ld variant %d: %zu hits for %s", id, variant, hits.size(), nd.c_str()); LOG(d);
        for (uintptr_t h : hits) {
            snprintf(d, sizeof d, "  Text at %llx", (unsigned long long)h); LOG(d);
            int refs = 0; addr = 0x60000000000ull;
            while (addr < 0x70000000000ull && refs < 4 && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi)) {
                uintptr_t b = reinterpret_cast<uintptr_t>(mbi.BaseAddress), e = b + mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD)) for (uintptr_t p = b; p < e && refs < 4; p += buf.size() - 0x200) {
                    size_t n = std::min<uintptr_t>(buf.size(), e - p); SIZE_T got = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), buf.data(), n, &got) || got < 16) continue;
                    for (size_t i = 0; i + 8 <= got && refs < 4; i += 8) { uint64_t q; memcpy(&q, &buf[i], 8); if (q != h) continue; ++refs;
                        uintptr_t rec = p + i - 0x10; uint8_t r[0x80]; char l[500]; int w = snprintf(l, sizeof l, "    Reference at %llx (entry %llx):", (unsigned long long)(p + i), (unsigned long long)rec);
                        if (rd(rec, r, 0x80)) for (int o = 0; o < 0x80; o += 8) { uint64_t v; memcpy(&v, r + o, 8); w += snprintf(l + w, sizeof l - w, " %llx", (unsigned long long)v); }
                        LOG(l); }
                }
                addr = e;
            }
        }
    }
}
DWORD WINAPI durance_thread(LPVOID) {
    LOG("Player plate: thread running");
    long last = -1; ULONGLONG next = 0; int tries = 0; bool scanning = false;
    for (;;) {
        Sleep(scanning ? 50 : 1000);
        const long id = g_player_car_id.load();
        if (!id) continue;
        if (id != last) { swap_restore(); last = id; tries = 0; next = GetTickCount64() + 4000; g_swap_id = 0; scanning = false; }
        if (!scanning && GetTickCount64() < next) continue;
        if (!scanning) ++tries;
        char m[200];
        const int r = swap_scan(id, m, sizeof m, !scanning);
        if (r == 0) { scanning = true; continue; }
        scanning = false; next = GetTickCount64() + 400; tries = 1;
        { static std::string lastm; if (lastm == m) continue; lastm = m; }
        char l[260]; snprintf(l, sizeof l, "Player plate car %ld attempt %d: %s -> %s", id, tries, m, r == 2 ? "DURANCE set" : "not yet");
        LOG(l);
        if (r == 2) g_swap_id = id;
    }
    return 0;
}

// Diagnose 9: welche Datei-Oeffnungen betreffen Kennzeichen-Materialien/-Texturen, und wann (relativ zur Spieler-Id)?
typedef HANDLE (WINAPI* CFW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI* CFA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CFW g_cfw = nullptr; CFA g_cfa = nullptr; std::atomic<int> g_fopen_logged{0};
void log_open(const std::string& path, const char* api) {
    std::string low = path; for (auto& c : low) c = (char)tolower((unsigned char)c);
    if (low.find("hzn") == std::string::npos) return;
    if (low.find("hzn_plate_eu_") == std::string::npos && low.find("hzn_plate_us_") == std::string::npos && low.find("_sp.") == std::string::npos && low.find("atlas") == std::string::npos && low.find("lp3") == std::string::npos && low.find("hzn_plate_us.") == std::string::npos) return;
    if (g_fopen_logged.fetch_add(1) > 300) return;
    char d[400]; snprintf(d, sizeof d, "File %s (player id %ld, t=%llu ms): %s", api, g_player_car_id.load(), (unsigned long long)GetTickCount64(), path.c_str()); LOG(d);
}
// Spieler-Kennzeichen per Umleitung: oeffnet das Spiel das Kennzeichen-Material des Spielerautos,
// bekommt es das Spieler-Material (verweist auf die Spieler-Texturen).
std::vector<std::pair<std::string, long>> g_ids;
void load_ids(const wchar_t* fn) {
    FILE* f = _wfopen(fn, L"rb"); if (!f) { LOG("PlateLightIds.csv missing"); return; }
    char l[200]; while (fgets(l, sizeof l, f)) { if (l[0] == '#') continue; char* sc = strchr(l, ';'); if (!sc) continue; *sc = 0; std::string nm = l; for (auto& c : nm) c = (char)tolower((unsigned char)c); g_ids.push_back({nm, atol(sc + 1)}); }
    fclose(f); char m[80]; snprintf(m, sizeof m, "Car ids: %zu", g_ids.size()); LOG(m);
}
long id_of(const std::string& nm) { for (auto& e : g_ids) if (e.first == nm) return e.second; return -1; }
// ==== Kennzeichen-Editor: Spieler-Texturen mit eigenem Text (PlateLight.ini [Plate] Text=...) ====
std::wstring g_pdir; HANDLE g_gen_done = nullptr; std::atomic<bool> g_gen_ok{false}; std::atomic<bool> g_gen_busy{false};
std::string g_gen_text; std::string g_gen_out[2][3];
const char* kSpNames[2][3] = {{"\\hzn_plate_atlas2_sp.swatchbin", "\\hzn_plate_nrml3_sp.swatchbin", "\\hzn_plate_glos3_sp.swatchbin"},
                              {"\\hzn_us_atlas_sp.swatchbin", "\\hzn_us_nrml_sp.swatchbin", "\\hzn_us_glos_sp.swatchbin"}};
std::string u8(const std::wstring& w) { char b[1024]; WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof b, nullptr, nullptr); return b; }
std::string plate_text_ini() {
    wchar_t buf[96]; GetPrivateProfileStringW(L"Plate", L"Text", L"\x01", buf, 96, g_ini);
    if (buf[0] == 1) {
        const char* add = "\r\n[Plate]\r\n; Plate editor: text on the plate of your car (up to 8 characters: A-Z, 0-9, space).\r\n"
                          "; With up to 7 characters a sticker is added on the right: Sticker=0 blue, 1 turquoise, 2 gold.\r\n"
                          "; Place a sticker with {0} {1} {2} in the text, e.g. Text={2}PHIL 12  (each character and each sticker = 1 field, 8 fields)\r\n"
                          "; A new text takes effect after restarting the game (or live via the in-game editor).\r\n"
                          "Text=HORIZON\r\nSticker=0\r\n";
        HANDLE f = CreateFileW(g_ini, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, add, DWORD(strlen(add)), &w, nullptr); CloseHandle(f); }
        wcscpy_s(buf, L"HORIZON");
    }
    std::string t = u8(buf);
    // ohne {n}: bis 7 Zeichen -> Plakette (Variante [Plate] Sticker=0..2) ins 8. Feld
    if (t.find('{') == std::string::npos && t.size() < 8) {
        int v = int(ini_f(L"Plate", L"Sticker", 0)); if (v < 0 || v > 2) v = 0;
        while (t.size() < 7) t += ' ';
        t += "{" + std::to_string(v) + "}";
    }
    return t;
}
std::string plate_want_ini() { return (ini_f(L"Plate", L"Hidden", 0) != 0 ? "!" : "") + plate_text_ini(); }
CRITICAL_SECTION g_want_cs; bool g_want_init = false; std::string g_want_text; bool g_want_set = false;
std::string want_text() { if (!g_want_init) return plate_want_ini(); EnterCriticalSection(&g_want_cs); bool s = g_want_set; std::string t = g_want_text; LeaveCriticalSection(&g_want_cs); return s ? t : plate_want_ini(); }
void set_want(const std::string* t) { if (!g_want_init) { InitializeCriticalSection(&g_want_cs); g_want_init = true; } EnterCriticalSection(&g_want_cs); g_want_set = t != nullptr; if (t) g_want_text = *t; LeaveCriticalSection(&g_want_cs); }
void live_apply(int only = -1);
extern std::atomic<bool> g_editor_open;
DWORD WINAPI gen_thread(LPVOID) {
  for (int round = 0; round < 50; ++round) {
    std::string text = want_text();
    std::string dir = u8(g_pdir) + "\\PlateEditor"; CreateDirectoryA(dir.c_str(), nullptr);
    const char* tn[2] = {"eu", "us"}; const char* fn[3] = {"atlas", "nrml", "glos"};
    for (int t = 0; t < 2; ++t) for (int k = 0; k < 3; ++k) g_gen_out[t][k] = dir + "\\" + tn[t] + "_" + fn[k] + ".swatchbin";
    // Zwischenspeicher: gleicher Text -> vorhandene Dateien benutzen
    std::string stamp = dir + "\\text.txt"; bool same = false;
    if (FILE* f = fopen(stamp.c_str(), "rb")) { char b[64] = {}; fread(b, 1, 63, f); fclose(f); same = text == b; }
    for (int t = 0; t < 2 && same; ++t) for (int k = 0; k < 3; ++k) if (GetFileAttributesA(g_gen_out[t][k].c_str()) == INVALID_FILE_ATTRIBUTES) same = false;
    bool ok = true; uint64_t t0 = GetTickCount64();
    if (!same) {
        // v4.12: EU und US parallel; jede Sorte sofort live schreiben, sobald sie fertig ist
        bool okt[2] = {true, true}; std::string errt[2];
        auto one = [&](int t) { std::string data = u8(g_pdir) + "\\PlateEditor_" + tn[t] + ".bin"; okt[t] = pled_generate(data, text, g_gen_out[t], errt[t]); if (okt[t] && g_editor_open.load()) live_apply(t); };
        std::thread us(one, 1); one(0); us.join();
        for (int t = 0; t < 2; ++t) if (!okt[t]) { ok = false; LOG(("Editor: " + errt[t]).c_str()); }
        if (ok) if (FILE* f = fopen(stamp.c_str(), "wb")) { fwrite(text.data(), 1, text.size(), f); fclose(f); }
    }
    g_gen_text = text; g_gen_ok.store(ok);
    char m[200]; snprintf(m, sizeof m, "Editor: player plate \"%s\" %s (%llu ms)", text.c_str(), !ok ? "ERROR" : same ? "from cache" : "created", (unsigned long long)(GetTickCount64() - t0)); LOG(m);
    SetEvent(g_gen_done);
    if (ok && same) live_apply();
    if (want_text() == text) break;
  }
    g_gen_busy.store(false);
    return 0;
}
void start_gen() {
    if (g_gen_busy.exchange(true)) return;
    ResetEvent(g_gen_done);
    if (HANDLE t = CreateThread(nullptr, 0, gen_thread, nullptr, 0, nullptr)) CloseHandle(t); else g_gen_busy.store(false);
}
// ===== v4.7 Diagnose Live-Aenderung: wohin liest das Spiel die Spieler-Texturen? =====
struct LiveRead { HANDLE h; int ty, k; uintptr_t buf; DWORD len; uint64_t off; bool ovl; };
LiveRead g_lr[64]; std::atomic<int> g_lr_n{0}; HANDLE g_lh[6] = {}; int g_lh_ty[6], g_lh_k[6]; std::atomic<int> g_lh_n{0};
void live_track_handle(HANDLE h, int ty, int k) { int n = g_lh_n.load(); if (n < 6) { g_lh[n] = h; g_lh_ty[n] = ty; g_lh_k[n] = k; g_lh_n.store(n + 1); } }
using RFFn = BOOL (WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
RFFn g_rf = nullptr;
BOOL WINAPI my_rf(HANDLE h, LPVOID buf, DWORD n, LPDWORD rd_, LPOVERLAPPED ov) {
    int ln = g_lh_n.load();
    for (int i = 0; i < ln; ++i) if (g_lh[i] == h) {
        LARGE_INTEGER pos{}; uint64_t off = 0;
        if (ov) off = (uint64_t(ov->OffsetHigh) << 32) | ov->Offset; else if (SetFilePointerEx(h, LARGE_INTEGER{}, &pos, FILE_CURRENT)) off = pos.QuadPart;
        int m = g_lr_n.load(); if (m < 64) { g_lr[m] = {h, g_lh_ty[i], g_lh_k[i], (uintptr_t)buf, n, off, ov != nullptr}; g_lr_n.store(m + 1); }
        MEMORY_BASIC_INFORMATION mb{}; VirtualQuery(buf, &mb, sizeof mb);
        char t[260]; snprintf(t, sizeof t, "Editor: read type %d/%d to %p length %lu offset %llu %s (memory type %lx protect %lx base %p size %llx)",
            g_lh_ty[i], g_lh_k[i], buf, n, (unsigned long long)off, ov ? "async" : "sync", mb.Type, mb.Protect, mb.AllocationBase, (unsigned long long)mb.RegionSize); LOG(t);
        break;
    }
    return g_rf(h, buf, n, rd_, ov);
}
// Nach dem Laden pruefen, ob die gelesenen Puffer noch die Dateidaten enthalten (= dauerhafter Textur-Speicher?)
void live_check(const char* wann) {
    int m = g_lr_n.load();
    for (int i = 0; i < m; ++i) {
        const LiveRead& r = g_lr[i];
        FILE* f = fopen(g_gen_out[r.ty][r.k].c_str(), "rb"); if (!f) continue;
        std::vector<uint8_t> a(r.len), b(r.len); _fseeki64(f, r.off, SEEK_SET); size_t got = fread(a.data(), 1, r.len, f); fclose(f);
        SIZE_T g2 = 0; bool ok = ReadProcessMemory(GetCurrentProcess(), (void*)r.buf, b.data(), r.len, &g2) && g2 == r.len;
        size_t same = 0; for (size_t j = 0; j < got && j < g2; ++j) same += a[j] == b[j];
        char t[200]; snprintf(t, sizeof t, "Editor: check %s: buffer %p (%d/%d, %lu B) %s, equal %.1f %%", wann, (void*)r.buf, r.ty, r.k, r.len, ok ? "readable" : "NOT readable", got ? 100.0 * same / got : 0.0); LOG(t);
    }
}
// ===== v4.9 Live-Aenderung =====
// Beim Laden wird der Dateiinhalt festgehalten (genau das, was das Spiel bekam). Die Suche nimmt
// Stichproben ueber alle Mip-Stufen (Streaming laedt evtl. nicht alle), erweitert jeden Treffer
// seitenweise und merkt sich die zusammenhaengenden Bereiche. Schreiben passiert im Prozess per
// memcpy (nur geaenderte 64-B-Bloecke), damit die Schreibueberwachung des Emulators es bemerkt.
const size_t kSwHdr = 83;
struct LiveSnap { uint8_t* p = nullptr; size_t n = 0; };
const int kLT = 4;   // Zeilen 0/1 = EU/US-Texturen, 2/3 = Schild-Index-Puffer (v4.14, bis 6 Dateien)
LiveSnap g_snap[kLT][3];
struct LiveReg { uintptr_t addr; size_t off, len; };   // Speicher addr <-> Nutzdaten [off, off+len)
LiveReg g_reg[kLT][3][8]; int g_reg_n[kLT][3] = {}; std::atomic<bool> g_live_scan{false}; CRITICAL_SECTION g_live_cs; std::atomic<bool> g_live_cs_ok{false};
void live_cs_init() { if (!g_live_cs_ok.exchange(true)) InitializeCriticalSection(&g_live_cs); }
bool read_file(const std::string& p, std::vector<uint8_t>& v) { FILE* f = fopen(p.c_str(), "rb"); if (!f) return false; _fseeki64(f, 0, SEEK_END); long long n = _ftelli64(f); _fseeki64(f, 0, SEEK_SET); v.resize(size_t(n)); bool ok = fread(v.data(), 1, v.size(), f) == v.size(); fclose(f); return ok; }
void live_snapshot(int ty, int k) {
    live_cs_init(); EnterCriticalSection(&g_live_cs);
    std::vector<uint8_t> f;
    if (read_file(g_gen_out[ty][k], f) && f.size() > kSwHdr + 4096) {
        LiveSnap& S = g_snap[ty][k]; size_t n = f.size() - kSwHdr;
        if (S.p && S.n != n) { VirtualFree(S.p, 0, MEM_RELEASE); S.p = nullptr; }
        if (!S.p) S.p = (uint8_t*)VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (S.p) { memcpy(S.p, f.data() + kSwHdr, n); S.n = n; }
        g_reg_n[ty][k] = 0;
    }
    LeaveCriticalSection(&g_live_cs);
}
// ===== v4.14 Schild entfernen: Index-Bereiche von Schild/Nieten/AO-Schleier degenerieren =====
struct HideEnt { std::string rel; size_t ioff, icount; std::vector<std::pair<size_t, size_t>> rr, ao; long long posoff = -1; int lamp = 1; };
std::vector<HideEnt> g_hide;
std::string g_diag_file;   // v4.18: Pfad der Spieler-modelbin (Diagnose)
struct MeshVar { std::string rel, car; std::vector<uint8_t> orig, hid; };
MeshVar g_mesh[6]; int g_mesh_n = 0;
static char* q_end(char* q) { while (*q && *q != ';' && *q != '\r' && *q != '\n') ++q; return q; }
void load_hide(const wchar_t* fn) {
    FILE* f = _wfopen(fn, L"rb"); if (!f) { LOG("PlateHide.csv missing"); return; }
    char l[4096];
    while (fgets(l, sizeof l, f)) {
        if (l[0] == '#') continue;
        char* p1 = strchr(l, ';'); if (!p1) continue; *p1 = 0; char* p2 = strchr(p1 + 1, ';'); if (!p2) continue; *p2 = 0; char* p3 = strchr(p2 + 1, ';'); if (!p3) continue; *p3 = 0;
        HideEnt e; e.rel = l; e.ioff = strtoull(p1 + 1, nullptr, 10); e.icount = strtoull(p2 + 1, nullptr, 10);
        for (char* q = p3 + 1; *q;) { char* c = strchr(q, ':'); if (!c) break; size_t a = strtoull(q, nullptr, 10), b = strtoull(c + 1, &q, 10); e.rr.push_back({a, b}); if (*q == ',') ++q; else break; }
        if (*q_end(p3 + 1) == ';') { char* p5 = q_end(p3 + 1) + 1; e.posoff = strtoll(p5, nullptr, 10); if (*q_end(p5) == ';') { e.lamp = atoi(q_end(p5) + 1);
            char* q = strchr(q_end(p5) + 1, ';');   // 7. Feld: Bereiche des Schatten-Schleiers (HZN_ao)
            if (q) { ++q; while (*q >= '0' && *q <= '9') { size_t a = strtoull(q, &q, 10); if (*q != ':') break; size_t b = strtoull(q + 1, &q, 10); e.ao.push_back({a, b}); if (*q == ',') ++q; else break; } } } }
        g_hide.push_back(e);
    }
    fclose(f); char m[80]; snprintf(m, sizeof m, "Plate hiding: %zu files", g_hide.size()); LOG(m);
}
bool mesh_hidden_wanted() { std::string t = want_text(); return !t.empty() && t[0] == '!'; }
// Spielerauto-Datei geoeffnet: Varianten bilden, Schnappschuss des Geladenen; ggf. ausgeblendete Kopie liefern
// liefert Pfad einer Ersatzdatei (leer = Original laden)

// ===== v4.27 Teile-Diagnose (F8): wo steht, welche Heck-Variante montiert ist? =====
static uint32_t crc32s(const std::string& s) { uint32_t c = 0xffffffffu; for (unsigned char ch : s) { c ^= ch; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u))); } return ~c; }
static FILE* g_dfile = nullptr;
static void DLOG(const char* t) { return; if (!g_dfile) { std::wstring p = g_pdir + L"\\PlateLight_Teile.txt"; g_dfile = _wfopen(p.c_str(), L"ab"); } if (g_dfile) { fprintf(g_dfile, "%s\r\n", t); fflush(g_dfile); } }
DWORD WINAPI parts_diag(LPVOID) {
    // v4.32: Teile-Konfiguration suchen. Teile-Ids im Spiel = Auto-Id*1000 + Stufe (z.B. 1516000 Serie, 1516002 Tuning).
    // Gesucht werden Haeufungen solcher Werte (int32) im ganzen Nicht-GPU-Speicher; je Fundstelle 0x180 Bytes Umgebung.
    long pl = g_player_car_id.load(); std::string nm; for (auto& e : g_ids) if (e.second == pl) { nm = e.first; break; }
    if (pl <= 0) { LOG("Parts diagnostic: player car unknown"); return 0; }
    const uint32_t lo = uint32_t(pl) * 1000u, hi = lo + 31u;
    char t0[200]; snprintf(t0, sizeof t0, "Parts diagnostic v2 %s (id %ld): searching values %u..%u, object %p", nm.c_str(), pl, lo, hi, (void*)g_player_obj.load()); LOG(t0); DLOG("=========="); DLOG(t0);
    MEMORY_BASIC_INFORMATION mb; std::vector<uint8_t> buf((1 << 20) + 0x200); uint64_t t1 = GetTickCount64(); int hits = 0; uintptr_t last = 0;
    for (uintptr_t p = 0x10000; p < 0x7ffffffff000ull && VirtualQuery((void*)p, &mb, sizeof mb); p = (uintptr_t)mb.BaseAddress + mb.RegionSize) {
        DWORD pr = mb.Protect & 0xff;
        if (mb.State != MEM_COMMIT || mb.Type == MEM_IMAGE || (mb.Protect & (PAGE_GUARD | PAGE_WRITECOMBINE)) || (pr != PAGE_READWRITE && pr != PAGE_READONLY)) continue;
        uintptr_t b0 = (uintptr_t)mb.BaseAddress, e = b0 + mb.RegionSize;
        for (uintptr_t c = b0; c < e && hits < 60; c += 1 << 20) {
            SIZE_T got = 0; if (!ReadProcessMemory(GetCurrentProcess(), (void*)c, buf.data(), std::min<uintptr_t>(1 << 20, e - c), &got) || got < 4) break;
            for (size_t i = 0; i + 4 <= got && hits < 60; i += 4) { uint32_t v; memcpy(&v, &buf[i], 4); if (v < lo || v > hi) continue;
                uintptr_t at = c + i; if (at < last + 0x100) continue;
                // Haeufung: mindestens 4 Teile-Werte in den naechsten 0x100 Bytes
                int n = 0; for (size_t j = i; j + 4 <= got && j < i + 0x100; j += 4) { uint32_t w; memcpy(&w, &buf[j], 4); n += (w >= lo && w <= hi); }
                if (n < 4) continue;
                last = at; ++hits;
                std::vector<uint32_t> r(0x180 / 4); SIZE_T g = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(at - 0x40), r.data(), r.size() * 4, &g);
                char t[200]; snprintf(t, sizeof t, "  Hit %d @%p (%d part values), from -0x40:", hits, (void*)at, n); DLOG(t);
                for (size_t o = 0; o < g / 4; o += 8) { char u[200]; int k = snprintf(u, sizeof u, "    %+05d:", int(o * 4) - 0x40);
                    for (size_t j = o; j < o + 8 && j < g / 4; ++j) { uint32_t w = r[j]; if (w >= lo && w <= hi) k += snprintf(u + k, sizeof u - k, "   T+%-5u", w - lo); else k += snprintf(u + k, sizeof u - k, " %08x", w); }
                    DLOG(u); }
            }
        }
    }
    char t2[120]; snprintf(t2, sizeof t2, "Parts diagnostic done (%llu ms, %d hits)", (unsigned long long)(GetTickCount64() - t1), hits); LOG(t2); DLOG(t2);
    return 0;
}
std::atomic<bool> g_part_reset{false};
void geo_start(); bool geo_start_later = false;
// v4.24: Registrierung in zwei Schritten. Im Autowechsel-Fenster ist noch unklar, welches Auto das neue Spielerauto ist ->
// alle Kandidaten vormerken (g_mesh_pend) und erst aktivieren, wenn die neue Spieler-Id bekannt ist (note_car).
struct MeshPend { std::string rel; std::vector<uint8_t> orig, hid; bool served_hidden; int lamp; uint32_t nvert; size_t nrr; };
std::map<std::string, std::vector<MeshPend>> g_mesh_pend; CRITICAL_SECTION g_pend_cs; std::atomic<bool> g_pend_ok{false};
void pend_init() { if (!g_pend_ok.exchange(true)) InitializeCriticalSection(&g_pend_cs); }
void mesh_activate(const std::string& car, std::vector<MeshPend>& L) {
    live_cs_init(); EnterCriticalSection(&g_live_cs);
    if (g_mesh_n && g_mesh[0].car != car) {   // anderes Spielerauto: alte Eintraege verwerfen
        for (int i = 0; i < g_mesh_n; ++i) { LiveSnap& S = g_snap[2 + i / 3][i % 3]; if (S.p) VirtualFree(S.p, 0, MEM_RELEASE); S = LiveSnap(); g_reg_n[2 + i / 3][i % 3] = 0; g_mesh[i] = MeshVar(); }
        g_mesh_n = 0;
    }
    for (auto& P : L) {
        int slot = -1; for (int i = 0; i < g_mesh_n; ++i) if (g_mesh[i].rel == P.rel) slot = i;
        if (slot < 0 && g_mesh_n < 6) slot = g_mesh_n++;
        if (slot < 0) continue;
        MeshVar& M = g_mesh[slot]; M.rel = P.rel; M.car = car; M.orig = P.orig; M.hid = P.hid;
        const std::vector<uint8_t>& ld = P.served_hidden ? M.hid : M.orig; size_t nb = ld.size();
        LiveSnap& S = g_snap[2 + slot / 3][slot % 3];
        if (S.p && S.n != nb) { VirtualFree(S.p, 0, MEM_RELEASE); S.p = nullptr; }
        if (!S.p) S.p = (uint8_t*)VirtualAlloc(nullptr, nb, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (S.p) { memcpy(S.p, ld.data(), nb); S.n = nb; }
        g_reg_n[2 + slot / 3][slot % 3] = 0;
    }
    LeaveCriticalSection(&g_live_cs);
    if (!L.empty()) {
        const MeshPend& P = L.back();
        set_variant(P.rel); g_part_reset.store(true);
        if (g_player_lamp.exchange(P.lamp) != P.lamp || !P.lamp) { char lm[200]; snprintf(lm, sizeof lm, "Plate lamp player car: %s (%s)", P.lamp ? "on" : "OFF - part without lamp", P.rel.c_str()); LOG(lm); }
        char m[300]; snprintf(m, sizeof m, "Plate geometry player car %s: %s (%u vertices, %zu ranges)%s", car.c_str(), P.rel.c_str(), P.nvert, P.nrr, P.served_hidden ? " -> loaded hidden" : ""); LOG(m);
        if (g_editor_open.load()) geo_start();   // v4.25: GPU-Speicher nur im Editor lesen (Hintergrund-Suche kostete FPS)
    }
}
// vorgemerkte Kandidaten: das bestaetigte Spielerauto aktivieren, den Rest verwerfen
void mesh_pend_resolve(long id) {
    std::string nm; for (auto& e : g_ids) if (e.second == id) { nm = e.first; break; }
    pend_init(); std::vector<MeshPend> L; int n = 0;
    EnterCriticalSection(&g_pend_cs);
    n = int(g_mesh_pend.size());
    auto it = g_mesh_pend.find(nm); if (it != g_mesh_pend.end()) L = std::move(it->second);
    g_mesh_pend.clear();
    LeaveCriticalSection(&g_pend_cs);
    if (n) { char m[200]; snprintf(m, sizeof m, "Car switch: %d candidates queued, player car %s %s", n, nm.empty() ? "?" : nm.c_str(), L.empty() ? "not among them" : "activated"); LOG(m); }
    if (!L.empty()) mesh_activate(nm, L);
}
std::wstring mesh_register(const std::string& low, const wchar_t* path, bool cand) {
    const HideEnt* E = nullptr;
    for (auto& e : g_hide) if (low.size() >= e.rel.size() && low.compare(low.size() - e.rel.size(), e.rel.size(), e.rel) == 0) { E = &e; break; }
    if (!E) return L"";
    std::string car = E->rel.substr(5, E->rel.find('\\', 5) - 5);
    { char pb[600]; WideCharToMultiByte(CP_ACP, 0, path, -1, pb, sizeof pb, nullptr, nullptr); g_diag_file = pb; }
    std::vector<uint8_t> d; { FILE* f = _wfopen(path, L"rb"); if (!f) return L""; _fseeki64(f, 0, SEEK_END); long long n = _ftelli64(f); _fseeki64(f, 0, SEEK_SET); d.resize(size_t(n)); size_t got = fread(d.data(), 1, d.size(), f); fclose(f); if (got != d.size()) return L""; }
    if (E->posoff < 16 || E->ioff + E->icount * 4 > d.size()) { LOG("Plate hiding: file does not match the table"); return L""; }
    uint32_t nvert = 0; memcpy(&nvert, &d[size_t(E->posoff) - 16], 4);
    size_t nb = size_t(nvert) * 12; if (size_t(E->posoff) + nb > d.size()) { LOG("Plate hiding: position stream does not match"); return L""; }
    const size_t po = size_t(E->posoff);
    MeshPend P; P.rel = E->rel; P.lamp = E->lamp; P.nvert = nvert; P.nrr = E->rr.size();
    // v4.17: Positionen statt Indizes – alle Vertices von Schild/Nieten/AO auf einen Punkt (Dreiecke werden unsichtbar)
    P.orig.assign(d.begin() + po, d.begin() + po + nb); P.hid = P.orig;
    uint32_t first = 0xffffffff;
    for (auto& r : E->rr) for (size_t t = r.first; t < r.first + r.second && t < E->icount; ++t) {
        uint32_t vi; memcpy(&vi, &d[E->ioff + t * 4], 4); if (vi >= nvert) continue;
        if (first == 0xffffffff) first = vi;
        memcpy(&P.hid[size_t(vi) * 12], &P.orig[size_t(first) * 12], 12);
    }
    P.served_hidden = mesh_hidden_wanted();
    std::wstring repl;
    if (P.served_hidden) {
        memcpy(&d[po], P.hid.data(), nb);
        static std::atomic<int> s_hn{0}; int hn = s_hn++ % 16;
        wchar_t o[MAX_PATH]; swprintf(o, MAX_PATH, L"%ls\\PlateEditor\\hidden_%d.modelbin", g_pdir.c_str(), hn);
        if (FILE* f = _wfopen(o, L"wb")) { fwrite(d.data(), 1, d.size(), f); fclose(f); repl = o; }
    }
    if (cand) {   // Autowechsel-Fenster: nur vormerken
        pend_init(); EnterCriticalSection(&g_pend_cs);
        auto& V = g_mesh_pend[car]; bool rep = false; for (auto& x : V) if (x.rel == P.rel) { x = P; rep = true; } if (!rep) V.push_back(P);
        LeaveCriticalSection(&g_pend_cs);
        char m[240]; snprintf(m, sizeof m, "Plate geometry candidate %s queued%s", P.rel.c_str(), P.served_hidden ? " (loaded hidden)" : ""); LOG(m);
        return repl;
    }
    std::vector<MeshPend> L{P}; mesh_activate(car, L);
    return repl;
}
static bool own_range(uintptr_t a) {
    for (int ty = 0; ty < kLT; ++ty) for (int k = 0; k < 3; ++k) { const LiveSnap& S = g_snap[ty][k]; if (S.p && a >= (uintptr_t)S.p && a < (uintptr_t)S.p + S.n) return true; }
    return false;
}
static double page_match(uintptr_t mem, const uint8_t* ref, size_t n, uint8_t* tmp) {
    SIZE_T g = 0; if (!ReadProcessMemory(GetCurrentProcess(), (void*)mem, tmp, n, &g) || g != n) return -1;
    size_t same = 0, tot = 0; for (size_t j = 0; j < n; j += 64, ++tot) same += memcmp(tmp + j, ref + j, std::min<size_t>(64, n - j)) == 0;
    return tot ? double(same) / tot : 0;
}
DWORD WINAPI live_scan(LPVOID) {
    uint64_t t0 = GetTickCount64(); live_cs_init();
    struct Smp { int ty, k; size_t off; uint8_t b[64]; };
    std::vector<Smp> sm;
    static const double fr[] = {0.02, 0.15, 0.3, 0.45, 0.6, 0.7, 0.76, 0.8, 0.84, 0.88, 0.91, 0.94, 0.96, 0.975, 0.985, 0.992, 0.996};
    EnterCriticalSection(&g_live_cs);   // nur kurz: Stichproben aus den Snapshots
    for (int ty = 0; ty < kLT; ++ty) for (int k = 0; k < 3; ++k) {
        const LiveSnap& S = g_snap[ty][k]; if (!S.p) continue;
        for (double f : fr) {
            size_t o0 = size_t(S.n * f) & ~size_t(63), best = 0; int bd = -1;
            for (size_t o = o0; o + 64 <= S.n && o < o0 + 8192; o += 64) {
                bool seen[256] = {}; int d = 0; for (int j = 0; j < 64; ++j) if (!seen[S.p[o + j]]) { seen[S.p[o + j]] = true; ++d; }
                if (d > bd) { bd = d; best = o; } if (d >= 40) break;
            }
            if (bd >= (ty >= 2 ? 12 : 24)) { Smp x{ty, k, best, {}}; memcpy(x.b, S.p + best, 64); sm.push_back(x); }
        }
    }
    LeaveCriticalSection(&g_live_cs);
    // Hashtabelle ueber die ersten 8 Bytes jeder Stichprobe
    const int kH = 4096; std::vector<int> ht(kH, -1), nx(sm.size(), -1);
    auto hh = [](uint64_t v) { return int((v * 0x9E3779B97F4A7C15ull) >> 52); };
    for (int i = 0; i < (int)sm.size(); ++i) { uint64_t v; memcpy(&v, sm[i].b, 8); int h = hh(v); nx[i] = ht[h]; ht[h] = i; }
    struct Found { int ty, k; LiveReg r; };
    std::vector<Found> fd;
    auto covered = [&](int ty, int k, uintptr_t at) { for (auto& F : fd) if (F.ty == ty && F.k == k && at >= F.r.addr && at < F.r.addr + F.r.len) return true; return false; };
    MEMORY_BASIC_INFORMATION mb; uintptr_t p = 0x10000; uint64_t scanned = 0;
    std::vector<uint8_t> buf(1 << 20), tmp(4096);
    bool full[kLT][3] = {}, done = false;
    EnterCriticalSection(&g_live_cs); for (int a = 0; a < 2; ++a) for (int b = 0; b < 3; ++b) g_reg_n[a][b] = 0;   /* v4.22: nur Texturen, Geometrie verwaltet geo_scan */ LeaveCriticalSection(&g_live_cs);
    // v4.20: Pass 0 = normaler Speicher, nur Texturen (schnell). Pass 1 = Write-Combine-Speicher (GPU, langsam lesbar),
    // nur Schild-Geometrie, Bereiche nahe dem letzten Fundort zuerst.
    static uintptr_t s_hint = 0;
    for (int pass = 0; pass < 1 && !done; ++pass) {   // v4.21: Geometrie sucht geo_scan (eigener Thread)
    if (pass == 1) Sleep(1000);   // Editor-Seite laedt die Schild-Datei beim Oeffnen neu -> neue GPU-Kopie abwarten
    uint64_t tp = GetTickCount64(), sc0 = scanned;
    std::vector<std::pair<uintptr_t, uintptr_t>> regs;
    for (p = 0x10000; p < 0x7ffffffff000ull && VirtualQuery((void*)p, &mb, sizeof mb); p = (uintptr_t)mb.BaseAddress + mb.RegionSize) {
        DWORD pr = mb.Protect & 0xff;
        bool readable = mb.State == MEM_COMMIT && mb.Type != MEM_IMAGE && !(mb.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_READONLY || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_READWRITE);
        if (readable && (((mb.Protect & PAGE_WRITECOMBINE) != 0) != (pass == 1))) readable = false;
        if (readable) regs.push_back({(uintptr_t)mb.BaseAddress, (uintptr_t)mb.BaseAddress + mb.RegionSize});
    }
    if (pass == 1 && s_hint) std::sort(regs.begin(), regs.end(), [](const std::pair<uintptr_t, uintptr_t>& A, const std::pair<uintptr_t, uintptr_t>& B) {
        auto d = [](uintptr_t x) { return x > s_hint ? x - s_hint : s_hint - x; }; return d(A.first) < d(B.first); });
    bool pdone = false;
    for (auto& R : regs) {
        if (done || pdone) break;
        uintptr_t b0 = R.first, e = R.second;
        if (!own_range(b0) && b0 != (uintptr_t)buf.data()) for (uintptr_t c = b0; c < e; c += 1 << 20) {
            SIZE_T want = std::min<uintptr_t>(1 << 20, e - c), got = 0;
            if (done || pdone) break;
            if (!ReadProcessMemory(GetCurrentProcess(), (void*)c, buf.data(), want, &got) || got < 64) break;
            scanned += got;
            for (size_t i = 0; i + 64 <= got && !done && !pdone; i += 64) {   // Texturdaten liegen mind. 64-B-ausgerichtet
                uint64_t v; memcpy(&v, buf.data() + i, 8);
                for (int si = ht[hh(v)]; si >= 0; si = nx[si]) {
                    const Smp& x = sm[si];
                    if ((pass == 0) != (x.ty < 2)) continue;
                    if (memcmp(buf.data() + i, x.b, 64) != 0) continue;
                    uintptr_t at = c + i; intptr_t base = intptr_t(at) - intptr_t(x.off);
                    if (full[x.ty][x.k] || covered(x.ty, x.k, at) || (base & 0xff)) continue;
                    EnterCriticalSection(&g_live_cs);
                    const LiveSnap& S = g_snap[x.ty][x.k];
                    size_t pg = x.off & ~size_t(4095), lo = pg, hi = pg;
                    while (hi < S.n) { size_t n = std::min<size_t>(4096, S.n - hi); if (page_match(base + hi, S.p + hi, n, tmp.data()) < 0.9) break; hi += n; }
                    while (lo > 0) { if (page_match(base + lo - 4096, S.p + lo - 4096, 4096, tmp.data()) < 0.9) break; lo -= 4096; }
                    size_t sn = S.n;
                    LeaveCriticalSection(&g_live_cs);
                    if (hi <= lo) continue;
                    MEMORY_BASIC_INFORMATION m2{}; VirtualQuery((void*)(base + lo), &m2, sizeof m2);
                    if ((hi - lo) * 10 >= sn) { char t[260]; snprintf(t, sizeof t, "Editor: texture %d/%d in memory at %p: payload %zu..%zu of %zu (%.0f %%), type %lx protect %lx",
                        x.ty, x.k, (void*)(base + lo), lo, hi, sn, 100.0 * (hi - lo) / sn, m2.Type, m2.Protect); LOG(t); }
                    fd.push_back({x.ty, x.k, {uintptr_t(base + lo), lo, hi - lo}});
                    if (hi - lo >= sn - sn / 100) {   // vollstaendige Kopie: sofort benutzbar
                        EnterCriticalSection(&g_live_cs);
                        int& n = g_reg_n[x.ty][x.k]; if (n < 8) g_reg[x.ty][x.k][n++] = {uintptr_t(base + lo), lo, hi - lo};
                        full[x.ty][x.k] = true;
                        LeaveCriticalSection(&g_live_cs);
                        int nf = 0; for (int a = 0; a < kLT; ++a) for (int b = 0; b < 3; ++b) nf += full[a][b] || !g_snap[a][b].p;
                        if (nf == kLT * 3) done = true;
                        { int np = 0, nt = 0; for (int a2 = 0; a2 < kLT; ++a2) for (int b2 = 0; b2 < 3; ++b2) if ((a2 < 2) == (pass == 0)) { ++nt; np += full[a2][b2] || !g_snap[a2][b2].p; } if (np == nt) pdone = true; }
                        if (x.ty >= 2) s_hint = uintptr_t(base);
                        live_apply();
                    }
                }
            }
        }
    }
    { char t[160]; snprintf(t, sizeof t, "Editor: search %s: %llu MB in %llu ms", pass == 0 ? "textures" : "Plate geometry (GPU memory)", (unsigned long long)((scanned - sc0) >> 20), (unsigned long long)(GetTickCount64() - tp)); LOG(t); }
    }
    // Bereiche uebernehmen (enthaltene Teilbereiche verwerfen)
    int found = 0; EnterCriticalSection(&g_live_cs); for (int a = 0; a < kLT; ++a) for (int b = 0; b < 3; ++b) found += g_reg_n[a][b]; LeaveCriticalSection(&g_live_cs);
    char t[160]; snprintf(t, sizeof t, "Editor: texture search done: %d complete copies, %llu MB searched (%llu ms)", found, (unsigned long long)(scanned >> 20), (unsigned long long)(GetTickCount64() - t0)); LOG(t);
    g_live_scan.store(false);
    live_apply();   // falls waehrend der Suche schon etwas geaendert wurde
    return 0;
}
// v4.16 Diagnose: wo liegt der Index-Puffer des Schilds? (u32/u16, absolut oder relativ zum kleinsten Index)
std::atomic<bool> g_mdiag_done{false};
// v4.18: Muster aus dem Lampenglas (HZN_lt) – wird nie ausgeblendet, liegt also im Spiel immer im Original vor
DWORD WINAPI mesh_diag(LPVOID) {
    std::vector<uint8_t> d; { FILE* f = fopen(g_diag_file.c_str(), "rb"); if (f) { _fseeki64(f, 0, SEEK_END); long long n = _ftelli64(f); _fseeki64(f, 0, SEEK_SET); d.resize(size_t(n)); if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear(); fclose(f); } }
    const HideEnt* E = nullptr; { std::string low = g_diag_file; for (auto& c : low) c = (char)tolower((unsigned char)c); for (auto& e : g_hide) if (low.size() >= e.rel.size() && low.compare(low.size() - e.rel.size(), e.rel.size(), e.rel) == 0) E = &e; }
    if (d.empty() || !E || E->posoff < 16) { LOG("Geometry diagnostic: no data"); return 0; }
    // Indizes, die in keinem Ausblende-Bereich liegen: letzter Block der Indexdaten (Lampenglas)
    size_t lt0 = 0; for (auto& r : E->rr) lt0 = std::max(lt0, r.first + r.second);
    if (lt0 + 48 > E->icount) { LOG("Geometry diagnostic: no lamp glass range"); return 0; }
    std::vector<uint32_t> v(48); memcpy(v.data(), &d[E->ioff + lt0 * 4], 48 * 4);
    uint32_t mn = *std::min_element(v.begin(), v.end());
    std::string pos((const char*)&d[size_t(E->posoff) + size_t(mn) * 12], 96);
    struct Pat { const char* nm; std::string b; int step; } P[5];
    auto mk32 = [&](uint32_t sub) { std::string s; for (auto x : v) { uint32_t y = x - sub; s.append((char*)&y, 4); } return s; };
    auto mk16 = [&](uint32_t sub) { std::string s; for (auto x : v) { uint16_t y = uint16_t(x - sub); s.append((char*)&y, 2); } return s; };
    P[0] = {"Index u32 absolute", mk32(0), 4}; P[1] = {"Index u16 absolute", mk16(0), 2}; P[2] = {"Index u32 relative", mk32(mn), 4}; P[3] = {"Index u16 relative", mk16(mn), 2};
    P[4] = {"Positions float", pos, 4};
    char t0[200]; snprintf(t0, sizeof t0, "Geometry diagnostic: lamp glass indices from %zu (lowest vertex %u)", lt0, mn); LOG(t0);
    MEMORY_BASIC_INFORMATION mb; uintptr_t p = 0x10000; int hits = 0; uint64_t t1 = GetTickCount64();
    std::vector<char> buf((1 << 20) + 256);
    while (p < 0x7ffffffff000ull && VirtualQuery((void*)p, &mb, sizeof mb) && hits < 40) {
        uintptr_t b0 = (uintptr_t)mb.BaseAddress, e = b0 + mb.RegionSize; DWORD pr = mb.Protect & 0xff;
        bool readable = mb.State == MEM_COMMIT && mb.Type != MEM_IMAGE && !(mb.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_READONLY || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_READWRITE);
        if (readable && !own_range(b0)) for (uintptr_t c = b0; c < e; c += 1 << 20) {
            SIZE_T want = std::min<uintptr_t>((1 << 20) + 256, e - c), got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (void*)c, buf.data(), want, &got) || got < 256) break;
            for (auto& q : P) {
                auto it = buf.begin(); size_t lim = std::min<size_t>(got, 1 << 20) + q.b.size();
                if (lim > got) lim = got;
                while (true) {
                    it = std::search(it, buf.begin() + lim, std::boyer_moore_horspool_searcher<std::string::const_iterator>(q.b.begin(), q.b.end()));
                    if (it == buf.begin() + lim) break;
                    uintptr_t at = c + (it - buf.begin());
                    bool own = at >= (uintptr_t)d.data() && at < (uintptr_t)d.data() + d.size();
                    if (!own && (size_t)(it - buf.begin()) < (1u << 20)) { char t[220]; snprintf(t, sizeof t, "Geometry diagnostic: %s at %p (type %lx protect %lx, alignment %llu)", q.nm, (void*)at, mb.Type, mb.Protect, (unsigned long long)(at & 0xfff)); LOG(t); ++hits; }
                    ++it;
                }
            }
        }
        p = e;
    }
    char t[160]; snprintf(t, sizeof t, "Geometry diagnostic done: %d hits (%llu ms)", hits, (unsigned long long)(GetTickCount64() - t1)); LOG(t);
    return 0;
}
// ===== v4.21 Geometrie-Suche: eigener Thread, mehrere Leser parallel, nur Write-Combine-Speicher =====
std::atomic<int> g_geo_gen{0}; std::atomic<bool> g_geo_run{false};
DWORD WINAPI geo_scan(LPVOID arg) {
    const int gen = int(reinterpret_cast<intptr_t>(arg));
    Sleep(1500);   // GPU-Kopie nach dem Laden abwarten
    if (gen != g_geo_gen.load()) return 0;
    uint64_t t0 = GetTickCount64(); live_cs_init();
    struct Smp { int ty, k; size_t off; uint8_t b[64]; };
    std::vector<Smp> sm; static const double fr[] = {0.02, 0.1, 0.2, 0.35, 0.5, 0.65, 0.8, 0.9, 0.97};
    EnterCriticalSection(&g_live_cs);
    int want = 0;
    for (int ty = 2; ty < kLT; ++ty) for (int k = 0; k < 3; ++k) {
        const LiveSnap& S = g_snap[ty][k]; if (!S.p) continue; ++want; g_reg_n[ty][k] = 0;
        for (double f : fr) {
            size_t o0 = size_t(S.n * f) & ~size_t(63), best = 0; int bd = -1;
            for (size_t o = o0; o + 64 <= S.n && o < o0 + 4096; o += 64) { bool seen[256] = {}; int d = 0; for (int j = 0; j < 64; ++j) if (!seen[S.p[o + j]]) { seen[S.p[o + j]] = true; ++d; } if (d > bd) { bd = d; best = o; } if (d >= 40) break; }
            if (bd >= 16) { Smp x{ty, k, best, {}}; memcpy(x.b, S.p + best, 64); sm.push_back(x); }
        }
    }
    LeaveCriticalSection(&g_live_cs);
    if (!want || sm.empty()) { g_geo_run.store(false); return 0; }
    // WC-Bereiche in Arbeitsstuecke (16 MB) zerlegen
    std::vector<std::pair<uintptr_t, uintptr_t>> work; uint64_t total = 0;
    MEMORY_BASIC_INFORMATION mb;
    for (uintptr_t p = 0x10000; p < 0x7ffffffff000ull && VirtualQuery((void*)p, &mb, sizeof mb); p = (uintptr_t)mb.BaseAddress + mb.RegionSize) {
        DWORD pr = mb.Protect & 0xff;
        if (mb.State != MEM_COMMIT || !(mb.Protect & PAGE_WRITECOMBINE) || (mb.Protect & PAGE_GUARD) || (pr != PAGE_READWRITE && pr != PAGE_READONLY)) continue;
        uintptr_t b0 = (uintptr_t)mb.BaseAddress, e = b0 + mb.RegionSize; total += e - b0;
        for (uintptr_t c = b0; c < e; c += 16 << 20) work.push_back({c, std::min<uintptr_t>(e, c + (16 << 20))});
    }
    static uintptr_t s_hint = 0;
    if (s_hint) std::sort(work.begin(), work.end(), [](const std::pair<uintptr_t, uintptr_t>& A, const std::pair<uintptr_t, uintptr_t>& B) {
        auto d = [](uintptr_t x) { return x > s_hint ? x - s_hint : s_hint - x; }; return d(A.first) < d(B.first); });
    std::atomic<size_t> next{0}; std::atomic<int> found{0}; std::atomic<uint64_t> scanned{0};
    bool full[kLT][3] = {};
    auto worker = [&]() {
        std::vector<uint8_t> buf((1 << 20) + 64), tmp(4096);
        while (true) {
            if (found.load() >= want || gen != g_geo_gen.load()) return;
            size_t wi = next++; if (wi >= work.size()) return;
            for (uintptr_t c = work[wi].first; c < work[wi].second; c += 1 << 20) {
                SIZE_T n = std::min<uintptr_t>((1 << 20) + 64, work[wi].second - c), got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (void*)c, buf.data(), n, &got) || got < 64) break;
                scanned += got;
                for (size_t i = 0; i + 64 <= got; i += 16) {
                    for (const Smp& x : sm) {
                        if (memcmp(buf.data() + i, x.b, 64) != 0) continue;
                        intptr_t base = intptr_t(c + i) - intptr_t(x.off);
                        EnterCriticalSection(&g_live_cs);
                        bool skip = full[x.ty][x.k];
                        const LiveSnap& S = g_snap[x.ty][x.k];
                        size_t lo = 0, hi = 0;
                        if (!skip && S.p) { size_t pg = x.off & ~size_t(4095); lo = hi = pg;
                            while (hi < S.n) { size_t m = std::min<size_t>(4096, S.n - hi); if (page_match(base + hi, S.p + hi, m, tmp.data()) < 0.9) break; hi += m; }
                            while (lo > 0) { if (page_match(base + lo - 4096, S.p + lo - 4096, 4096, tmp.data()) < 0.9) break; lo -= 4096; } }
                        bool ok = !skip && S.p && hi > lo && hi - lo >= S.n - S.n / 100;
                        if (ok) { int& r = g_reg_n[x.ty][x.k]; if (r < 8) g_reg[x.ty][x.k][r++] = {uintptr_t(base + lo), lo, hi - lo}; full[x.ty][x.k] = true; s_hint = uintptr_t(base); }
                        LeaveCriticalSection(&g_live_cs);
                        if (ok) { ++found; char t[200]; snprintf(t, sizeof t, "Editor: plate geometry %d/%d in GPU memory at %p (%llu ms)", x.ty, x.k, (void*)(base + lo), (unsigned long long)(GetTickCount64() - t0)); LOG(t); live_apply(-2); }
                    }
                }
            }
        }
    };
    // v4.23: im Editor 6 Leser, sonst (Hintergrund beim Laden) 2 Leser mit niedriger Prioritaet
    const bool fast = g_editor_open.load();
    unsigned nt = fast ? std::max(2u, std::min(6u, std::thread::hardware_concurrency())) : 2u;
    std::vector<std::thread> th; for (unsigned i = 0; i < nt; ++i) th.emplace_back([&, fast] { if (!fast) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST); worker(); });
    for (auto& x : th) x.join();
    char t[220]; snprintf(t, sizeof t, "Editor: geometry search %s: %d/%d found, %llu of %llu MB GPU memory read, %u threads (%llu ms)",
        gen != g_geo_gen.load() ? "cancelled (reloaded)" : "done", found.load(), want, (unsigned long long)(scanned.load() >> 20), (unsigned long long)(total >> 20), nt, (unsigned long long)(GetTickCount64() - t0)); LOG(t);
    if (gen == g_geo_gen.load()) g_geo_run.store(false);
    return 0;
}
void geo_start() { int g = ++g_geo_gen; g_geo_run.store(true); if (HANDLE t = CreateThread(nullptr, 0, geo_scan, reinterpret_cast<LPVOID>(intptr_t(g)), 0, nullptr)) CloseHandle(t); }
void live_find() {   /* v4.25: Geometrie-Suche startet erst mit dem Editor */
    { int have = 0, want = 0; live_cs_init(); EnterCriticalSection(&g_live_cs); for (int a = 2; a < kLT; ++a) for (int b = 0; b < 3; ++b) if (g_snap[a][b].p) { ++want; have += g_reg_n[a][b] > 0; } LeaveCriticalSection(&g_live_cs);
      if (want && have < want && !g_geo_run.load()) geo_start(); }
    if (!g_live_scan.exchange(true)) if (HANDLE t = CreateThread(nullptr, 0, live_scan, nullptr, 0, nullptr)) CloseHandle(t); }
// Absicherung: Schreibfehler, die der Emulator nicht selbst behandelt, landen hier (zuletzt registriert)
thread_local bool t_live_write = false; std::atomic<int> g_live_unhandled{0};
LONG CALLBACK live_veh(EXCEPTION_POINTERS* ep) {
    if (!t_live_write || ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    void* a = (void*)ep->ExceptionRecord->ExceptionInformation[1]; DWORD old;
    if (VirtualProtect((void*)((uintptr_t)a & ~uintptr_t(4095)), 4096, PAGE_READWRITE, &old)) { ++g_live_unhandled; return EXCEPTION_CONTINUE_EXECUTION; }
    return EXCEPTION_CONTINUE_SEARCH;
}
void live_apply(int only) {
    static bool veh = false; if (!veh) { veh = true; AddVectoredExceptionHandler(0, live_veh); }
    live_cs_init(); EnterCriticalSection(&g_live_cs);
    int regs = 0; size_t blocks = 0; int before = g_live_unhandled.load();
    std::vector<uint8_t> tmp(65536);
    const bool hide = mesh_hidden_wanted();
    for (int ty = 0; ty < kLT; ++ty) for (int k = 0; k < 3; ++k) {
        if (!g_reg_n[ty][k]) continue;
        if (only == -2 ? ty < 2 : (only >= 0 && ty != only)) continue;   // -2 = nur Schild-Geometrie
        std::vector<uint8_t> f; const uint8_t* np;
        if (ty >= 2) { const MeshVar& M = g_mesh[(ty - 2) * 3 + k]; const std::vector<uint8_t>& v = hide ? M.hid : M.orig; if (v.size() != g_snap[ty][k].n) continue; np = v.data(); }
        else { if (!read_file(g_gen_out[ty][k], f) || f.size() != g_snap[ty][k].n + kSwHdr) continue; np = f.data() + kSwHdr; }
        for (int r = 0; r < g_reg_n[ty][k]; ++r) {
            const LiveReg& R = g_reg[ty][k][r]; ++regs;
            for (size_t o = 0; o < R.len; o += tmp.size()) {
                size_t n = std::min(tmp.size(), R.len - o); SIZE_T g = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (void*)(R.addr + o), tmp.data(), n, &g) || g != n) break;
                for (size_t j = 0; j < n; j += 64) {
                    size_t m = std::min<size_t>(64, n - j);
                    if (memcmp(tmp.data() + j, np + R.off + o + j, m) == 0) continue;
                    t_live_write = true; memcpy((void*)(R.addr + o + j), np + R.off + o + j, m); t_live_write = false; ++blocks;
                }
            }
        }
    }
    LeaveCriticalSection(&g_live_cs);
    char t[160]; snprintf(t, sizeof t, "Editor: written live: %d regions, %zu blocks changed, %d write faults handled", regs, blocks, g_live_unhandled.load() - before); LOG(t);
}
// Spiel oeffnet eine Spieler-Textur -> erzeugte Datei liefern
HANDLE editor_redirect(const std::string& low, LPCWSTR, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    for (int ty = 0; ty < 2; ++ty) for (int k = 0; k < 3; ++k) {
        const char* s = kSpNames[ty][k]; size_t L = strlen(s);
        if (low.size() < L || low.compare(low.size() - L, L, s) != 0) continue;
        if (g_gen_done) WaitForSingleObject(g_gen_done, 20000);
        if (!g_gen_ok.load()) return INVALID_HANDLE_VALUE;
        wchar_t w[1024]; MultiByteToWideChar(CP_UTF8, 0, g_gen_out[ty][k].c_str(), -1, w, 1024);
        HANDLE h = g_cfw(w, a, sh, sa, c, f, t);
        char m[300]; snprintf(m, sizeof m, "Editor: %s -> %s", s + 1, h != INVALID_HANDLE_VALUE ? "own text" : "ERROR on open"); LOG(m);
        if (h != INVALID_HANDLE_VALUE) live_snapshot(ty, k);
        return h;
    }
    return INVALID_HANDLE_VALUE;
}
std::atomic<bool> g_ed_unavail{false};   // v4.19: Spielerauto ohne Kennzeichen -> Popup statt Editor
std::atomic<int> g_editor_pending{0};   // v3.3: naechstes Laden von A_Garage.xaml -> Editor-Seite
std::atomic<uint64_t> g_editor_pending_t{0};
extern std::atomic<bool> g_editor_open;
extern void* g_ed_wrap; extern bool g_ed_ready; extern std::atomic<int> g_cap_n; extern std::atomic<bool> g_cap_on;
extern std::atomic<int> g_hcap_n; extern std::atomic<bool> g_hcap_on; extern void* g_ed_page; extern void* g_ed_beh; extern bool g_ed_closing;   // v3.5: nur innerhalb von 5 s nach dem Druck umleiten
thread_local int t_cfw_depth = 0;   // v4.15: eigene Datei-Zugriffe im Hook nicht erneut behandeln (Rekursion)
struct CfwGuard { CfwGuard() { ++t_cfw_depth; } ~CfwGuard() { --t_cfw_depth; } };
std::atomic<uint64_t> g_fall_due{0}; std::atomic<bool> g_fall_busy{false}; std::atomic<int> g_fall_gen{0};
// ===== v4.36 Spielerauto mit eigenen Kennzeichen-Ressourcen (auch wenn ein Drivatar dasselbe Modell faehrt) =====
// Beim Lesen der Szene des Spielerautos wird "\hzn\" im Pfad zu "\hzp\" (gleiche Laenge). Der Pfad ist dann nicht im Cache,
// das Spiel laedt neu. Hier wird "\hzp\" wieder auf die echte Datei abgebildet; die Kopie verweist auf das Material "hzp_plate_*",
// das auf das Spieler-Material (_sp) zeigt. Drivatare behalten "\hzn\" + HORIZON.
std::atomic<bool> g_alias_on{false}; std::atomic<int> g_alias_logs{0};
SRWLOCK g_alias_lock = SRWLOCK_INIT; std::map<std::string, uint64_t> g_alias_car;   // v4.39: Auto -> Zeitpunkt der letzten Alias-Szene
struct SceneClass { uint64_t t; int cls; }; std::map<std::string, SceneClass> g_scene_class;
// alte Datei-Logik (Material je Auto / Luecke) nur, wenn die letzte Szene dieses Autos nicht eindeutig zugeordnet werden konnte
bool alias_recent(const std::string& car) {
    AcquireSRWLockShared(&g_alias_lock); auto it = g_scene_class.find(car); bool known = it != g_scene_class.end() && GetTickCount64() - it->second.t < 30000 && it->second.cls != 2; ReleaseSRWLockShared(&g_alias_lock);
    return known;
}
bool alias_recent_old(const std::string& car) { AcquireSRWLockShared(&g_alias_lock); auto it = g_alias_car.find(car); bool r = it != g_alias_car.end() && GetTickCount64() - it->second < 30000; ReleaseSRWLockShared(&g_alias_lock); return r; }
static void alog(const char* what, const wchar_t* n) { if (g_alias_logs.fetch_add(1) < 300) { char b[600]; WideCharToMultiByte(CP_UTF8, 0, n, -1, b, sizeof b, nullptr, nullptr); char m[700]; snprintf(m, sizeof m, "Alias %s: %s", what, b); LOG(m); } }
static size_t wfind_ci(const std::wstring& s, const wchar_t* pat) { std::wstring l = s; for (auto& c : l) c = towlower(c); return l.find(pat); }
// liefert fuer Alias-Pfade den Pfad einer existierenden Datei (leer = kein Alias)
std::wstring alias_target(const wchar_t* n) {
    if (!n) return L"";
    std::wstring w(n); size_t p = wfind_ci(w, L"\\scene\\exterior\\hzp\\");
    if (p != std::wstring::npos) { w[p + 18] = L'n'; return w; }   // "\scene\exterior\hz" = 18 Zeichen
    p = wfind_ci(w, L"\\materials\\badges\\hzp_plate_");
    if (p != std::wstring::npos) {
        const bool us = wfind_ci(w, L"\\hzp_plate_us_") != std::wstring::npos;
        size_t sl = w.find_last_of(L'\\'); return w.substr(0, sl + 1) + (us ? L"hzn_plate_us_sp.materialbin" : L"hzn_plate_lp3_sp.materialbin");
    }
    return L"";
}
using GfaFn = DWORD (WINAPI*)(LPCWSTR); GfaFn g_gfa = nullptr;
using GfaxFn = BOOL (WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID); GfaxFn g_gfax = nullptr;
using FffxFn = HANDLE (WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD); FffxFn g_fffx = nullptr;
DWORD WINAPI my_gfa(LPCWSTR n) { std::wstring t = alias_target(n); if (!t.empty()) { alog("GetFileAttributesW", n); return g_gfa(t.c_str()); } return g_gfa(n); }
BOOL WINAPI my_gfax(LPCWSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID d) { std::wstring t = alias_target(n); if (!t.empty()) { alog("GetFileAttributesExW", n); return g_gfax(t.c_str(), l, d); } return g_gfax(n, l, d); }
HANDLE WINAPI my_fffx(LPCWSTR n, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) { std::wstring t = alias_target(n); if (!t.empty()) { alog("FindFirstFileExW", n); return g_fffx(t.c_str(), l, d, o, f, fl); } return g_fffx(n, l, d, o, f, fl); }
HANDLE alias_open_model(const std::string& low, LPCWSTR n, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    std::wstring real = alias_target(n); std::string lowr = low; size_t p = lowr.find("\\scene\\exterior\\hzp\\"); lowr[p + 18] = 'n';
    size_t pc = lowr.find("\\cars\\"); if (pc == std::string::npos) return INVALID_HANDLE_VALUE;
    size_t c0 = pc + 6, c1 = lowr.find('\\', c0); std::string car = lowr.substr(c0, c1 - c0);
    char d[300]; snprintf(d, sizeof d, "Plate geometry opened: %s (alias, t=%llu) PLAYER", car.c_str(), (unsigned long long)GetTickCount64()); LOG(d);
    std::wstring src = mesh_register(lowr, real.c_str(), false); if (src.empty()) src = real;
    std::vector<uint8_t> v; { std::string s8(src.size() * 3 + 1, 0); WideCharToMultiByte(CP_UTF8, 0, src.c_str(), -1, &s8[0], int(s8.size()), nullptr, nullptr); s8.resize(strlen(s8.c_str()));
        FILE* fi = _wfopen(src.c_str(), L"rb"); if (!fi) { alog("model ERROR (source missing)", n); return INVALID_HANDLE_VALUE; } fseek(fi, 0, SEEK_END); long sz = ftell(fi); fseek(fi, 0, SEEK_SET); v.resize(size_t(sz)); fread(v.data(), 1, v.size(), fi); fclose(fi); }
    int np = 0; const char* pats[2] = {"\\hzn_plate_eu_", "\\hzn_plate_us_"};
    for (const char* pt : pats) { size_t L = strlen(pt);
        for (size_t i = 0; i + L <= v.size(); ++i) if (_strnicmp((const char*)&v[i], pt, L) == 0) { v[i + 3] = (v[i + 3] == 'N') ? 'P' : 'p'; ++np; } }
    std::wstring dir = g_pdir + L"\\PlateEditor\\alias"; CreateDirectoryW((g_pdir + L"\\PlateEditor").c_str(), nullptr); CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring base(n); base = base.substr(base.find_last_of(L'\\') + 1);
    std::wstring wcar(car.begin(), car.end()); std::wstring out = dir + L"\\" + wcar + L"_" + base;
    FILE* fo = _wfopen(out.c_str(), L"wb"); if (!fo) { alog("model ERROR (copy)", n); return INVALID_HANDLE_VALUE; } fwrite(v.data(), 1, v.size(), fo); fclose(fo);
    HANDLE h = g_cfw(out.c_str(), a, sh, sa, c, f, t);
    char m[200]; snprintf(m, sizeof m, "Alias model %s: %d material references switched, %s", car.c_str(), np, h != INVALID_HANDLE_VALUE ? "loaded" : "ERROR on open"); LOG(m);
    ++g_fall_gen; g_fall_due = GetTickCount64() + 3000;
    return h;
}
HANDLE WINAPI my_cfw(LPCWSTR n, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    if (t_cfw_depth > 0) return g_cfw(n, a, sh, sa, c, f, t);
    CfwGuard guard;
    if (n) {
        char b[600]; int L = WideCharToMultiByte(CP_UTF8, 0, n, -1, b, sizeof b, nullptr, nullptr);
        if (L > 0) {
            std::string low = b; for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
            if (low.find("\\scene\\exterior\\hzp\\") != std::string::npos && low.size() > 9 && low.compare(low.size() - 9, 9, ".modelbin") == 0) {
                HANDLE h = alias_open_model(low, n, a, sh, sa, c, f, t); if (h != INVALID_HANDLE_VALUE) return h;
            }
            if (low.find("\\materials\\badges\\hzp_plate_") != std::string::npos) {
                std::wstring tg = alias_target(n); HANDLE h = g_cfw(tg.c_str(), a, sh, sa, c, f, t);
                alog(h != INVALID_HANDLE_VALUE ? "material -> player material" : "material ERROR", n);
                if (h != INVALID_HANDLE_VALUE) return h;
            }
            if (low.find("_sp.swatchbin") != std::string::npos) { HANDLE h = editor_redirect(low, n, a, sh, sa, c, f, t); if (h != INVALID_HANDLE_VALUE) return h; }
            if (low.find("cartargetcamera") != std::string::npos) { char m[300]; snprintf(m, sizeof m, "Editor: camera file opened: %s", b); LOG(m); }
            if (low.find("stateflow") != std::string::npos && (low.find("garage") != std::string::npos || low.find("ui_screens") != std::string::npos || low.find(".zip") != std::string::npos)) { char m[300]; snprintf(m, sizeof m, "Editor: stateflow file opened: %s", b); LOG(m); }
            if (low.size() > 16 && low.compare(low.size() - 16, 16, "\\a_upgrades.xaml") == 0) {
                // v3.9: Editor nutzt die Seitenklasse des Upgrade-Menues (gleiche Klasse wie Garage wird nicht neu geladen)
                const bool ed = g_editor_pending.exchange(0) != 0 && GetTickCount64() - g_editor_pending_t.load() < 8000;
                char m[300]; snprintf(m, sizeof m, "Editor: A_Upgrades.xaml loaded%s (%s)", ed ? " -> editor page" : "", b); LOG(m);
                if (ed) {
                    g_editor_open.store(true); g_ed_wrap = nullptr; g_ed_ready = false; g_cap_n.store(0); g_cap_on.store(true); g_hcap_n.store(0); g_hcap_on.store(true); g_ed_page = nullptr; g_ed_beh = nullptr; g_ed_closing = false;
                    std::wstring w(n); size_t sl = w.find_last_of(L'\\');
                    std::wstring nw = w.substr(0, sl + 1) + (g_ed_unavail.load() ? L"HZN_PlateUnavailable.xaml" : L"HZN_PlateEditor.xaml");
                    HANDLE h = g_cfw(nw.c_str(), a, sh, sa, c, f, t);
                    if (h != INVALID_HANDLE_VALUE) return h;
                    LOG("Editor: HZN_PlateEditor.xaml not found");
                }
            }
            if (low.size() > 9 && low.compare(low.size() - 9, 9, ".modelbin") == 0 && low.find("\\scene\\exterior\\hzn\\") != std::string::npos) {
                size_t pc = low.find("\\cars\\");
                if (pc != std::string::npos) {
                    size_t c0 = pc + 6, c1 = low.find('\\', c0); std::string car = low.substr(c0, c1 - c0);
                    const long id = id_of(car); long pl = g_player_car_id.load();
                    long live = -1; { uintptr_t o = g_player_obj.load(); int k = -1, v = 0; if (o && rd(o + kForzaCarKindOffset, &k, 4) && k == 0 && rd(o + kForzaCarIdOffset, &v, 4)) live = v; }
                    int rk; long rid; player_raw(&rk, &rid);
                    bool mine = id > 0 && (id == pl || id == live) && !alias_recent(car);
                    bool gp = !mine && id > 0 && !alias_recent(car) && gap_now(live);
                    if (gp && rid > 0 && rid != pl) { mine = id == rid; gp = false; }   // v4.25: neue Id schon im Objekt -> eindeutig
                    bool cand = gp;   // sonst: Kandidat vormerken, Entscheidung sobald die Id bekannt ist
                    char d[300]; snprintf(d, sizeof d, "Plate geometry opened: %s (id %ld, player %ld, object id %ld, raw %d/%ld, t=%llu)%s", car.c_str(), id, pl, live, rk, rid, (unsigned long long)GetTickCount64(), mine ? " PLAYER" : cand ? " CANDIDATE" : ""); LOG(d);
                    if (mine || cand) {
                        std::wstring r = mesh_register(low, n, cand);
                        if (!r.empty()) { HANDLE h = g_cfw(r.c_str(), a, sh, sa, c, f, t); if (h != INVALID_HANDLE_VALUE) return h; LOG("Plate geometry: replacement file cannot be opened"); }
                    }
                }
            }
            size_t pe = low.find("\\materials\\badges\\hzn_plate_eu_"), pu = low.find("\\materials\\badges\\hzn_plate_us_"), pm = low.rfind(".materialbin");
            size_t p0 = pe != std::string::npos ? pe : pu;
            if (p0 != std::string::npos && pm != std::string::npos) {
                const bool us = pe == std::string::npos;
                size_t ns = p0 + strlen("\\materials\\badges\\hzn_plate_eu_");
                std::string car = low.substr(ns, pm - ns);
                if (car != "sp") {
                    const long id = id_of(car); long pl = g_player_car_id.load();
                    // Autowechsel: das Spieler-Objekt bleibt dasselbe, seine Auto-Id wird vor dem Laden umgeschrieben -> direkt lesen
                    long live = -1; { uintptr_t o = g_player_obj.load(); int k = -1, v = 0; if (o && rd(o + kForzaCarKindOffset, &k, 4) && k == 0 && rd(o + kForzaCarIdOffset, &v, 4)) live = v; }
                    const long nw = 0;
                    bool mine = id > 0 && (id == pl || id == live || id == nw) && !alias_recent(car);
                    // Autowechsel: altes Spielerauto ist schon abgebaut, neues noch nicht da -> das erste Kennzeichen in dieser Luecke ist das neue Spielerauto
                    // v4.24: im ganzen Autowechsel-Fenster (max. 20 s) jedes Kennzeichen umleiten; das richtige ist sicher dabei.
                    // Drivatare, die genau in diesem Fenster geladen werden, tragen dann ebenfalls das Spieler-Kennzeichen.
                    // v4.25: kein "alle umleiten" mehr. Im Autowechsel steht die neue Auto-Id oft schon im (abgebauten) Spieler-Objekt
                    // -> damit eindeutig entscheiden. Nur wenn sie noch fehlt: wie frueher das erste Kennzeichen im Fenster.
                    int rk; long rid; player_raw(&rk, &rid);
                    bool gap = false;
                    if (!mine && id > 0 && !alias_recent(car) && gap_now(live)) {
                        if (rid > 0 && rid != pl) { mine = id == rid; gap = mine; }
                        else if (!g_dead_claimed.exchange(true)) { mine = true; gap = true; }
                    }
                    char d[300]; snprintf(d, sizeof d, "Plate material %s (id %ld, player %ld, object id %ld, raw %d/%ld, t=%llu): %s", car.c_str(), id, pl, live, rk, rid, (unsigned long long)GetTickCount64(), mine ? (gap ? "PLAYER (car switch) -> redirected" : "PLAYER -> redirected") : "HORIZON"); LOG(d);
                    if (mine) {
                        std::wstring w(n); size_t sl = w.find_last_of(L'\\');
                        std::wstring nw = w.substr(0, sl + 1) + (us ? L"hzn_plate_us_sp.materialbin" : L"hzn_plate_lp3_sp.materialbin");
                        HANDLE h = g_cfw(nw.c_str(), a, sh, sa, c, f, t);
                        if (h != INVALID_HANDLE_VALUE) return h;
                        LOG("Redirect failed (player material missing?)");
                    }
                }
            }

        }
    }
    return g_cfw(n, a, sh, sa, c, f, t);
}
HANDLE WINAPI my_cfa(LPCSTR n, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    if (n) log_open(n, "A");
    return g_cfa(n, a, sh, sa, c, f, t);
}
void hook_files() {
    HMODULE kb = GetModuleHandleA("kernelbase.dll"); if (!kb) kb = GetModuleHandleA("kernel32.dll");
    void* w = (void*)GetProcAddress(kb, "CreateFileW"); void* a = (void*)GetProcAddress(kb, "CreateFileA");
    bool ok1 = w && MH_CreateHook(w, (void*)my_cfw, (void**)&g_cfw) == MH_OK && MH_EnableHook(w) == MH_OK;
    { void* g1 = (void*)GetProcAddress(kb, "GetFileAttributesW"), *g2 = (void*)GetProcAddress(kb, "GetFileAttributesExW"), *g3 = (void*)GetProcAddress(kb, "FindFirstFileExW");
      int okn = 0;
      okn += g1 && MH_CreateHook(g1, (void*)my_gfa, (void**)&g_gfa) == MH_OK && MH_EnableHook(g1) == MH_OK;
      okn += g2 && MH_CreateHook(g2, (void*)my_gfax, (void**)&g_gfax) == MH_OK && MH_EnableHook(g2) == MH_OK;
      okn += g3 && MH_CreateHook(g3, (void*)my_fffx, (void**)&g_fffx) == MH_OK && MH_EnableHook(g3) == MH_OK;
      char m[80]; snprintf(m, sizeof m, "Alias file hooks: %d/3", okn); LOG(m); }
    bool ok2 = a && MH_CreateHook(a, (void*)my_cfa, (void**)&g_cfa) == MH_OK && MH_EnableHook(a) == MH_OK;
    void* r = (void*)GetProcAddress(kb, "ReadFile");
    if (r && MH_CreateHook(r, (void*)my_rf, (void**)&g_rf) == MH_OK) MH_EnableHook(r);
    char d[120]; snprintf(d, sizeof d, "File hooks: CreateFileW %s, CreateFileA %s", ok1 ? "ok" : "ERROR", ok2 ? "ok" : "ERROR"); LOG(d);
}

// v3.2: Garage-Kachel "Nummernschilder" (HZN_Button_Plates in A_Garage.xaml).
// Die Seite verdrahtet ihre Kacheln per game+0xd16d20(page, "PART_Button_X", &{handler, 0}).
// Wir haengen uns an den Aufruf fuer PART_Button_BarnFinds und verdrahten danach unsere Kachel.
constexpr uintptr_t kBindButton = 0xd16d20;
static const char kBindButtonBytes[] = "\x48\x8b\xc4\x57\x48\x81\xec\x90\x00\x00\x00";
using BindButtonFn = void (*)(void* page, const char* name, void* delegate);
BindButtonFn g_orig_bind = nullptr;
std::atomic<int> g_plates_clicks{0};
// v3.7: eigenes Transition-Ereignis wie die Spiel-Ereignisse "barn_finds" usw. (game+0xd1cf60 baut + registriert:
// {vtable, int, std::string name @+0x10, int @+0x30}). Ohne Registrierung wird der eingereihte Name ignoriert.
constexpr uintptr_t kMakeTransitionEvent = 0xd1cf60, kBarnFindsEventInit = 0x145190, kBarnFindsEvent = 0x3522608;
alignas(16) uint8_t g_hzn_ev[0x40];
std::atomic<bool> g_ev_reg{false};
void register_ev(const char* how) {
    if (g_ev_reg.load()) return;
    if (!match(reinterpret_cast<char*>(g_base) + kMakeTransitionEvent, "\x48\x89\x4c\x24\x08\x53\x56\x57\x48\x83\xec\x30", 12)) { LOG("Editor: event code differs"); return; }
    g_ev_reg.store(true);
    reinterpret_cast<void (*)(void*, const char*, int)>(g_base + kMakeTransitionEvent)(g_hzn_ev, "hzn_plates", 0x1b);
    char m[120]; snprintf(m, sizeof m, "Editor: event hzn_plates registered (%s)", how); LOG(m);
}
using VoidFn = void (*)();
VoidFn g_orig_bfinit = nullptr;
void detour_bfinit() { g_orig_bfinit(); register_ev("at game start"); }
extern std::atomic<int> g_editor_pending;
extern std::atomic<uint64_t> g_editor_pending_t;
constexpr uintptr_t kQueueTransition = 0x1422f10;   // (page, const std::string* id): reiht Transition der Seiten-Behavior ein
struct MsvcString { char buf[16]; uint64_t size, cap; };
void (*g_orig_queue_fn)(void*, const void*) = nullptr;   // Original (nach Hook), gesetzt beim Einhaengen
// v4.5: Kamera. Eigene Preset-Datei wird nicht gelesen -> Preset "Upgrades_RearBumper" im Speicher suchen
// (pos -2.753303 0.496375 -3.224348, fov 45) und nur fuer den Editor die Brennweite enger stellen (naeher am Kennzeichen).
std::atomic<uintptr_t> g_cam_fov{0}; std::atomic<bool> g_cam_scan{false};
DWORD WINAPI cam_scan(LPVOID) {
    const float key[3] = {-2.753303f, 0.496375f, -3.224348f};
    uint8_t kb[12]; memcpy(kb, key, 12);
    LOG("Editor: searching camera preset");
    MEMORY_BASIC_INFORMATION mb; uintptr_t p = 0x10000; uint64_t t0 = GetTickCount64();
    while (p < 0x7ffffffff000ull && VirtualQuery((void*)p, &mb, sizeof mb)) {
        uintptr_t b0 = (uintptr_t)mb.BaseAddress, e = b0 + mb.RegionSize;
        if (mb.State == MEM_COMMIT && mb.Type == MEM_PRIVATE && (mb.Protect & (PAGE_READWRITE)) && !(mb.Protect & PAGE_GUARD) && mb.RegionSize < (1ull << 30)) {
            static std::vector<uint8_t> buf(1 << 20);   // sicher lesen (Speicher kann waehrenddessen freigegeben werden)
            for (uintptr_t c = b0; c < e && !g_cam_fov.load(); c += (1 << 20) - 64) {
                SIZE_T want = std::min<uintptr_t>(1 << 20, e - c), got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (void*)c, buf.data(), want, &got) || got < 64) break;
                for (size_t i = 0; i + 64 <= got; i += 4) {
                    if (memcmp(buf.data() + i, kb, 12) != 0) continue;
                    for (int k = 12; k < 64; k += 4) { float f; memcpy(&f, buf.data() + i + k, 4); if (f == 45.0f) { g_cam_fov.store(c + i + k); break; } }
                    if (g_cam_fov.load()) break;
                }
            }
        }
        if (g_cam_fov.load()) break;
        p = e;
    }
    char m[120]; snprintf(m, sizeof m, "Editor: camera preset %s (%llu ms)", g_cam_fov.load() ? "found" : "NOT found", (unsigned long long)(GetTickCount64() - t0)); LOG(m);
    return 0;
}
void cam_set(bool editor) {
    uintptr_t a = g_cam_fov.load(); if (!a) return;
    float f = editor ? 26.0f : 45.0f; DWORD old;
    float cur = 0; if (!rd(a, &cur, 4) || (cur != 45.0f && cur != 26.0f)) { LOG("Editor: camera preset no longer valid"); g_cam_fov.store(0); return; }
    SIZE_T w = 0; WriteProcessMemory(GetCurrentProcess(), (void*)a, &f, 4, &w); (void)old;
}
bool player_has_plate(std::string* name) {
    long pl = g_player_car_id.load();
    { uintptr_t o = g_player_obj.load(); int k = -1, v = 0; if (o && rd(o + kForzaCarKindOffset, &k, 4) && k == 0 && rd(o + kForzaCarIdOffset, &v, 4) && v > 0) pl = v; }
    std::string nm; for (auto& e : g_ids) if (e.second == pl) { nm = e.first; break; }
    if (name) *name = nm;
    if (nm.empty()) return false;
    std::string pre = "cars\\" + nm + "\\";
    for (auto& e : g_hide) if (e.rel.compare(0, pre.size(), pre) == 0) return true;
    return false;
}
void plates_tile_clicked(void* page) {
    cam_set(true);
    int n = ++g_plates_clicks;
    uintptr_t beh = 0; rd(reinterpret_cast<uintptr_t>(page) + 0x310, &beh, 8);
    std::string carnm; bool has = player_has_plate(&carnm); g_ed_unavail.store(!has);
    char m[220]; snprintf(m, sizeof m, "Editor: plates tile pressed (%d, page %p, behavior %p), car %s: %s", n, page, (void*)beh, carnm.empty() ? "?" : carnm.c_str(), has ? "with plate" : "WITHOUT plate -> notice"); LOG(m);
    if (!beh) return;
    if (!g_orig_queue_fn && !match(reinterpret_cast<char*>(g_base) + kQueueTransition, "\x48\x8b\x89\x10\x03\x00\x00\x80\x79\x78\x00", 11)) { LOG("Editor: transition code differs"); return; }
    static MsvcString id;                       // kurz (<16): liegt im Puffer
    memset(&id, 0, sizeof id); memcpy(id.buf, "hzn_plates", 10); id.size = 10; id.cap = 15;
    static uint64_t last = 0; uint64_t now = GetTickCount64();
    // v4.21: nur sperren, solange ein Seitenwechsel wirklich laeuft (max. 8 s) bzw. direkt nach einem Druck (Doppeldruck)
    if ((g_editor_pending.load() && now - g_editor_pending_t.load() < 8000) || now - last < 1200) { LOG("Editor: page change already running, press ignored"); return; }
    last = now;
    g_editor_pending.store(1);
    uint8_t fl = 0; uint64_t v0[2] = {0, 0}, v1[2] = {0, 0};
    rd(beh + 0x78, &fl, 1); rd(beh + 0x60, v0, 16);
    g_editor_pending_t.store(now);
    if (!g_ev_reg.load()) register_ev("on press");
    (g_orig_queue_fn ? g_orig_queue_fn : reinterpret_cast<void (*)(void*, const void*)>(g_base + kQueueTransition))(page, g_ev_reg.load() ? (const void*)(g_hzn_ev + 0x10) : (const void*)&id);
    rd(beh + 0x60, v1, 16);
    {   // deklarierte Transitionen der Behavior (aus der Stateflow-XML) auflisten
        uint64_t tl[2] = {0, 0}; rd(beh + 0x40, tl, 16);
        std::string names; int cnt = 0;
        for (uint64_t p = tl[0]; p && p < tl[1] && cnt < 24; p += 8, ++cnt) {
            uintptr_t o = 0, sp = 0; char nm[40] = {0};
            if (rdp(p, &o) && rdp(o, &sp) && rd(sp, nm, 39)) { nm[39] = 0; names += nm; names += ' '; }
        }
        char mm[400]; snprintf(mm, sizeof mm, "Editor: declared transitions (%d): %s", cnt, names.c_str()); LOG(mm);
    }
    snprintf(m, sizeof m, "Editor: transition hzn_plates fired (active %u, queue %llu -> %llu)", fl,
             (unsigned long long)((v0[1] - v0[0]) / 32), (unsigned long long)((v1[1] - v1[0]) / 32));
    LOG(m);
}
// v3.4: Kachel-Navigation setzt den Fokus auf eine gemerkte Rasterposition (gleiche Seitenklasse fuer Garage und Editor).
// Gibt es dort keine Kachel, ist der Zeiger leer -> Absturz game+0x140cc22. Leeren Fokus einfach ueberspringen.
constexpr uintptr_t kNavFocus = 0x140cba0;
static const char kNavFocusBytes[] = "\x48\x89\x54\x24\x10\x53\x48\x83\xec\x30";
using NavFocusFn = void (*)(void* nav, void* arg);
NavFocusFn g_orig_navfocus = nullptr;
void detour_navfocus(void* nav, void* arg) {
    if (arg && *reinterpret_cast<void**>(arg) == nullptr) {
        static int n = 0; if (n++ < 20) LOG("Editor: focus without tile skipped");
        return;
    }
    g_orig_navfocus(nav, arg);
}
// v3.8 Diagnose: Stateflow-Wrapper der UI-Seiten (game+0x2637f0, Update): vergleicht eingereihte Namen mit seiner
// Transitionsliste [this+0x40..0x48] (Eintrag -> Objekt -> char*). Liste je Wrapper einmal loggen.
constexpr uintptr_t kWrapUpdate = 0x2637f0;
using WrapFn = void (*)(void*);
WrapFn g_orig_wrap = nullptr;
void* g_seen_wrap[64]; int g_seen_n = 0;
std::atomic<uint64_t> g_beat{0}; std::atomic<DWORD> g_ui_tid{0}; std::atomic<bool> g_editor_open{false};
// v4.0: Waechter – bleibt die UI-Schleife im Editor > 3 s stehen, Stapel des UI-Threads ins Log (Haenger-Analyse)
// ===== v4.33 Montierte Teile direkt aus dem Spielerauto lesen =====
// Im Auto-Objekt steht ab +0x128 die Teile-Konfiguration (gleiche Reihenfolge wie Career_Garage: Engine, ..., RearWing +0x14C,
// FrontBumper +0x1B0, RearBumper +0x1B4, Hood +0x1B8, SideSkirts +0x1BC). PlateParts.csv ordnet jeder HZN-Variante ihre
// Upgrade-Ids aus dem carbin zu. So stimmen Lampenposition/-an/aus auch, wenn die Variante schon geladen war (keine Datei geoeffnet).
struct PartEnt { std::string rel, car; int field; std::vector<uint32_t> ids; };
std::vector<PartEnt> g_parts;
void load_parts(const wchar_t* fn) {
    FILE* f = _wfopen(fn, L"rb"); if (!f) { LOG("PlateParts.csv missing"); return; }
    char l[1024];
    while (fgets(l, sizeof l, f)) {
        if (l[0] == '#') continue;
        char* a = strchr(l, ';'); if (!a) continue; *a = 0; char* b = strchr(a + 1, ';'); if (!b) continue; *b = 0;
        PartEnt e; e.rel = l; e.field = atoi(a + 1);
        size_t c1 = e.rel.find('\\'), c2 = e.rel.find('\\', c1 + 1); if (c1 == std::string::npos || c2 == std::string::npos) continue; e.car = e.rel.substr(c1 + 1, c2 - c1 - 1);
        for (char* q = b + 1; *q && *q != '\r' && *q != '\n';) { e.ids.push_back(uint32_t(strtoul(q, &q, 10))); if (*q == ',') ++q; else break; }
        if (e.field >= 0x128 && e.field < 0x200 && !e.ids.empty()) g_parts.push_back(e);
    }
    fclose(f); char m[80]; snprintf(m, sizeof m, "Parts mapping: %zu variants", g_parts.size()); LOG(m);
}
std::string g_part_rel;   // zuletzt erkannte montierte Variante (nur im F8-Thread benutzt)
void parts_poll() {
    if (g_parts.empty()) return;
    if (g_part_reset.exchange(false)) g_part_rel.clear();   // Datei-Weg hat Variante gesetzt -> erneut abgleichen
    const uintptr_t o = g_player_obj.load(); const long id = g_player_car_id.load(); if (!o || id <= 0) return;
    int cid = 0, kind = -1; if (!rd(o + kForzaCarIdOffset, &cid, 4) || cid != id || !rd(o + kForzaCarKindOffset, &kind, 4) || kind != 0) return;
    std::string nm; for (auto& e : g_ids) if (e.second == id) { nm = e.first; break; } if (nm.empty()) return;
    for (auto& e : g_parts) {
        if (e.car != nm) continue;
        uint32_t v = 0; if (!rd(o + e.field, &v, 4)) return;
        bool hit = false; for (uint32_t x : e.ids) hit |= x == v; if (!hit) continue;
        if (e.rel == g_part_rel) return;
        g_part_rel = e.rel; set_variant(e.rel);
        int lamp = 1; for (auto& h : g_hide) if (h.rel == e.rel) lamp = h.lamp;
        g_player_lamp.store(lamp);
        char m[300]; snprintf(m, sizeof m, "Fitted part detected (id %u): %s, plate lamp %s", v, e.rel.c_str(), lamp ? "on" : "OFF"); LOG(m);
        return;
    }
}
// ===== Test: Kennzeichen-Geometrie des Spielerautos guenstig finden (ueber die Modell-Ressource statt GPU-Speicher-Suche) =====
// ===== Kennzeichen abfallen (Schadensmodell Kosmetisch/Simulation) =====
// Geometrie des Spieler-Kennzeichens im GPU-Speicher ueber die Modell-Ressource finden (crc32 des Alias-Pfads im Gast-Speicher,
// dann Zeigern folgen bis zu den Positionen), danach bei starkem Schaden am Kennzeichen-Ende die Platte pro Bild "fallen lassen".
struct GeoState { uintptr_t addr = 0; int gen = 0; long chk = -1; std::string rel; std::vector<uint8_t> orig, hid; std::vector<uint32_t> plate, ao; float c[3] = {}; };
SRWLOCK g_fall_lock = SRWLOCK_INIT; GeoState g_fall; 
static bool is_wc(uintptr_t a) { MEMORY_BASIC_INFORMATION mb; return VirtualQuery((void*)a, &mb, sizeof mb) && mb.State == MEM_COMMIT && (mb.Protect & PAGE_WRITECOMBINE); }
static std::string geo_current_rel(std::string* car) {
    const long pl = g_player_car_id.load(); std::string nm; for (auto& e : g_ids) if (e.second == pl) { nm = e.first; break; }
    if (car) *car = nm; if (nm.empty()) return "";
    std::string rel = g_part_rel; const std::string pre = "cars\\" + nm + "\\";
    if (rel.compare(0, pre.size(), pre) != 0) { rel.clear(); for (auto& e : g_hide) if (e.rel.compare(0, pre.size(), pre) == 0) { rel = e.rel; break; } }
    return rel;
}
DWORD WINAPI geo_locate(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    const int gen = g_fall_gen.load(); std::string nm; const std::string rel = geo_current_rel(&nm);
    const HideEnt* E = nullptr; for (auto& e : g_hide) if (e.rel == rel) E = &e;
    if (!E || E->posoff < 16) { g_fall_busy = false; return 0; }
    std::wstring wrel(rel.begin(), rel.end()); std::wstring base = wrel.substr(wrel.find_last_of(L'\\') + 1);
    std::wstring wcar(nm.begin(), nm.end()); std::wstring af = g_pdir + L"\\PlateEditor\\alias\\" + wcar + L"_" + base;
    std::vector<uint8_t> d; { FILE* f = _wfopen(af.c_str(), L"rb"); if (!f) { LOG("Fall-off: plate model of the player car not found (no own resources)"); g_fall_busy = false; return 0; }
        fseek(f, 0, SEEK_END); d.resize(size_t(ftell(f))); fseek(f, 0, SEEK_SET); fread(d.data(), 1, d.size(), f); fclose(f); }
    if (size_t(E->posoff) > d.size()) { g_fall_busy = false; return 0; }
    uint32_t nvert = 0; memcpy(&nvert, &d[size_t(E->posoff) - 16], 4); const size_t po = size_t(E->posoff), nb = size_t(nvert) * 12;
    if (po + nb > d.size() || nb < 48) { g_fall_busy = false; return 0; }
    GeoState G; G.rel = rel; G.orig.assign(d.begin() + po, d.begin() + po + nb); G.hid = G.orig;
    std::vector<uint8_t> mark(nvert, 0); uint32_t first = 0xffffffff;
    for (auto& r : E->rr) for (size_t t = r.first; t < r.first + r.second && t < E->icount; ++t) { uint32_t vi; memcpy(&vi, &d[E->ioff + t * 4], 4); if (vi >= nvert) continue; if (first == 0xffffffff) first = vi; mark[vi] = 1; }
    for (auto& r : E->ao) for (size_t t = r.first; t < r.first + r.second && t < E->icount; ++t) { uint32_t vi; memcpy(&vi, &d[E->ioff + t * 4], 4); if (vi < nvert) mark[vi] = 2; }   // Schatten-Schleier: faellt nicht mit, verschwindet
    double cs[3] = {}; for (uint32_t i = 0; i < nvert; ++i) if (mark[i]) { memcpy(&G.hid[size_t(i) * 12], &G.orig[size_t(first) * 12], 12);
        if (mark[i] == 2) { G.ao.push_back(i); continue; }
        G.plate.push_back(i); float v[3]; memcpy(v, &G.orig[size_t(i) * 12], 12); for (int k = 0; k < 3; ++k) cs[k] += v[k]; }
    if (G.plate.empty()) { g_fall_busy = false; return 0; }
    for (int k = 0; k < 3; ++k) G.c[k] = float(cs[k] / G.plate.size());
    for (uint32_t i = 0; i < nvert; ++i) if (!mark[i]) { G.chk = long(i); break; }   // Pruef-Vertex (wird nie veraendert)
    std::string arel = rel; size_t hz = arel.find("\\hzn\\"); if (hz != std::string::npos) arel[hz + 3] = 'p';
    const uint32_t h = crc32s("game:\\media\\" + arel); const uint64_t t0 = GetTickCount64();
    std::vector<uintptr_t> hits; MEMORY_BASIC_INFORMATION mb; std::vector<uint8_t> buf(1 << 20);
    for (uintptr_t p = 0x60000000000ull; p < 0x70000000000ull && VirtualQuery((void*)p, &mb, sizeof mb); p = (uintptr_t)mb.BaseAddress + mb.RegionSize) {
        DWORD pr = mb.Protect & 0xff;
        if (mb.State != MEM_COMMIT || (mb.Protect & (PAGE_GUARD | PAGE_WRITECOMBINE)) || (pr != PAGE_READWRITE && pr != PAGE_READONLY)) continue;
        uintptr_t b0 = (uintptr_t)mb.BaseAddress, e = b0 + mb.RegionSize;
        for (uintptr_t c = b0; c < e && hits.size() < 16; c += 1 << 20) { SIZE_T got = 0; if (!ReadProcessMemory(GetCurrentProcess(), (void*)c, buf.data(), std::min<uintptr_t>(1 << 20, e - c), &got)) break;
            for (size_t i = 0; i + 4 <= got; i += 4) { uint32_t v; memcpy(&v, &buf[i], 4); if (v == h && hits.size() < 16) hits.push_back(c + i); } }
    }
    // Ziel pruefen: Positionen direkt am Zeiger oder (gemeinsamer Puffer mehrerer Stroeme) innerhalb der ersten 512 KB dahinter
    std::set<uintptr_t> wc_seen; static thread_local std::vector<uint8_t> wcb;
    std::string wcdbg; int wcn = 0;
    auto check = [&](uintptr_t& v) -> bool { if (!is_wc(v)) return false; uint8_t c48[48];
        if (rd(v, c48, 48) && !memcmp(c48, G.orig.data(), 48)) return true;
        if (wcn < 4) { float f3[3]; memcpy(f3, c48, 12); char b[96]; snprintf(b, sizeof b, " %p(%.3g %.3g %.3g)", (void*)v, f3[0], f3[1], f3[2]); wcdbg += b; ++wcn; }
        const uintptr_t pg = v & ~uintptr_t(0xfff); if (wc_seen.count(pg) || wc_seen.size() >= 150) return false; wc_seen.insert(pg);
        MEMORY_BASIC_INFORMATION mi; if (!VirtualQuery((void*)v, &mi, sizeof mi)) return false;
        const size_t avail = size_t((uintptr_t)mi.BaseAddress + mi.RegionSize - v), n = std::min<size_t>(avail, 512 * 1024);
        wcb.resize(n); SIZE_T g = 0; if (!ReadProcessMemory(GetCurrentProcess(), (void*)v, wcb.data(), n, &g) || g < 48) return false;
        for (size_t o = 4; o + 48 <= g; o += 4) if (!memcmp(&wcb[o], G.orig.data(), 48)) { v += o; return true; }
        return false; };
    uintptr_t found = 0; int nodes = 0; const char* how = "path";
    // 1) gemessener Weg (t3): Treffer-0x400 +78 +1b0 +c8 +198 +10
    for (uintptr_t hit : hits) { uintptr_t a = hit - 0x400; const size_t offs[5] = {0x78, 0x1b0, 0xc8, 0x198, 0x10}; bool ok = true;
        for (size_t o : offs) { uintptr_t n = 0; if (!rdp(a + o, &n) || !n) { ok = false; break; } a = n; }
        if (ok && check(a)) { found = a; break; } }
    std::string fpath;
    // 2) sonst Zeigern folgen (Tiefe 4)
    if (!found) { how = "search";
        for (uintptr_t hit : hits) { struct N { uintptr_t a; int depth; int parent; uint32_t off; }; std::vector<N> q{{hit - 0x400, 0, -1, 0}}; std::set<uintptr_t> seen;
            for (size_t qi = 0; qi < q.size() && !found && nodes < 80000 && GetTickCount64() - t0 < 20000; ++qi) {
                N n = q[qi]; uint64_t w[0x100] = {}; size_t sz = n.depth == 0 ? 0x800 : 0x200; SIZE_T g = 0; ++nodes;
                if (!ReadProcessMemory(GetCurrentProcess(), (void*)n.a, w, sz, &g)) continue;
                if (n.depth > 0 && g >= 48 && !memcmp(w, G.orig.data(), 48)) { found = n.a; how = "search (normal memory)";   // Kopie liegt nicht im WC-Speicher
                    char pb[32]; for (int pi = int(qi); pi > 0; pi = q[pi].parent) { snprintf(pb, sizeof pb, " +%x", q[pi].off); fpath = pb + fpath; } break; }
                for (size_t j = 0; j < g / 8; ++j) { const uint64_t v = w[j]; if (v < 0x10000 || v > 0x7fffffffffffull || seen.count(v)) continue; seen.insert(v);
                    if (is_wc(uintptr_t(v))) { uintptr_t t = uintptr_t(v); if (check(t)) { found = t;
                            char pb[32]; snprintf(pb, sizeof pb, " +%zx", j * 8); fpath = pb; for (int pi = int(qi); pi > 0; pi = q[pi].parent) { snprintf(pb, sizeof pb, " +%x", q[pi].off); fpath = pb + fpath; }
                            break; } continue; }
                    if (n.depth < 5 && (v & 7) == 0) q.push_back({uintptr_t(v), n.depth + 1, int(qi), uint32_t(j * 8)}); } }
            if (found) break; } }
    char m[300]; snprintf(m, sizeof m, "Fall-off: plate geometry %s %s (%s%s, %zu hits, %d nodes, %llu ms)", rel.c_str() + rel.rfind('\\') + 1, found ? "found" : "NOT found", how, fpath.c_str(), hits.size(), nodes, (unsigned long long)(GetTickCount64() - t0)); LOG(m);
    if (!found) { float e3[3]; memcpy(e3, G.orig.data(), 12); char d2[900]; snprintf(d2, sizeof d2, "Fall-off: expected %.3f %.3f %.3f, GPU buffers seen:%s", e3[0], e3[1], e3[2], wcdbg.c_str()); LOG(d2); }
    if (!found && gen == g_fall_gen.load()) { static int retries = 0; static int rgen = -1; if (rgen != gen) { rgen = gen; retries = 0; } if (++retries <= 2) g_fall_due = GetTickCount64() + 10000; }   // spaeter nochmal (Daten evtl. noch nicht hochgeladen)
    if (found && gen == g_fall_gen.load()) { G.addr = found; G.gen = gen; AcquireSRWLockExclusive(&g_fall_lock); g_fall = std::move(G); ReleaseSRWLockExclusive(&g_fall_lock); }
    g_fall_busy = false; return 0;
}
// --- Fall-Simulation ---
static void m3_rows(const float* w, float r[3][3]) { for (int i = 0; i < 3; ++i) for (int k = 0; k < 3; ++k) r[i][k] = w[i * 4 + k]; }
static void rot_rows(float B[3][3], const float* om, float dt) {   // Zeilen um die Welt-Achse om drehen (Rodrigues)
    float a = sqrtf(om[0] * om[0] + om[1] * om[1] + om[2] * om[2]) * dt; if (a < 1e-6f) return;
    float k[3] = {om[0] / (a / dt), om[1] / (a / dt), om[2] / (a / dt)}, c = cosf(a), s = sinf(a);
    for (int i = 0; i < 3; ++i) { float* v = B[i]; float kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2]; float cr[3] = {k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0]};
        for (int j = 0; j < 3; ++j) v[j] = v[j] * c + cr[j] * s + k[j] * kv * (1 - c); }
}
struct FallBody { int state = 0; uintptr_t addr = 0; float P[3] = {}, B[3][3] = {}; };
SRWLOCK g_body_lock = SRWLOCK_INIT; FallBody g_body;
// Schreiben der Platten-Vertices im Takt des Autos (ForzaCar::Update des Spielerautos) -> kein Zittern gegenueber dem fahrenden Auto
void fall_write(uintptr_t pres) {
    AcquireSRWLockShared(&g_body_lock); FallBody b = g_body; ReleaseSRWLockShared(&g_body_lock);
    if ((b.state != 1 && b.state != 2) || !b.addr || !pres) return;
    alignas(16) float W[16], q[4]; if (!car_world(pres, W, q)) return;
    float R[3][3]; m3_rows(W, R); float s2[3]; for (int i = 0; i < 3; ++i) s2[i] = R[i][0] * R[i][0] + R[i][1] * R[i][1] + R[i][2] * R[i][2];
    if (!TryAcquireSRWLockShared(&g_fall_lock)) return;
    if (g_fall.addr == b.addr) { static std::vector<uint8_t> out; out = g_fall.orig;
        for (uint32_t i : g_fall.ao) memcpy(&out[size_t(i) * 12], &g_fall.hid[size_t(i) * 12], 12);
        for (uint32_t i : g_fall.plate) { float v[3]; memcpy(v, &g_fall.orig[size_t(i) * 12], 12); float d0 = v[0] - g_fall.c[0], d1 = v[1] - g_fall.c[1], d2 = v[2] - g_fall.c[2]; float w[3], l[3];
            for (int k = 0; k < 3; ++k) w[k] = b.P[k] + d0 * b.B[0][k] + d1 * b.B[1][k] + d2 * b.B[2][k] - W[12 + k];
            for (int j = 0; j < 3; ++j) l[j] = (w[0] * R[j][0] + w[1] * R[j][1] + w[2] * R[j][2]) / (s2[j] > 1e-8f ? s2[j] : 1.0f);
            memcpy(&out[size_t(i) * 12], l, 12); }
        memcpy((void*)b.addr, out.data(), out.size()); }
    ReleaseSRWLockShared(&g_fall_lock);
}
DWORD WINAPI fall_thread(LPVOID) {
    int state = 0; std::string state_rel; float P[3] = {}, V[3] = {}, B[3][3] = {}, om[3] = {}, ground = 0, lastT[3] = {}; uint64_t lastt = 0, t_fall = 0; float carV[3] = {};
    for (;;) {
        Sleep(16);
        static float thr = 0.3f; static uint64_t thr_t = 0; if (GetTickCount64() - thr_t > 1000) { thr = ini_f(L"Plate", L"FallOffAt", 0.3f); thr_t = GetTickCount64(); }
        AcquireSRWLockShared(&g_fall_lock); uintptr_t addr = g_fall.addr; const std::string rel = g_fall.rel; const int fgen = g_fall.gen; const long chk = g_fall.chk;
        uint8_t cv[12] = {}; bool sane = addr && fgen == g_fall_gen.load() && (chk < 0 || (rd(addr + size_t(chk) * 12, cv, 12) && !memcmp(cv, &g_fall.orig[size_t(chk) * 12], 12)));
        ReleaseSRWLockShared(&g_fall_lock);
        if (addr && !sane) { addr = 0; }   // Auto neu aufgebaut / Speicher nicht mehr unsere Geometrie -> nichts schreiben
        if (!addr || rel != state_rel) { state = 0; state_rel = rel; lastt = 0; AcquireSRWLockExclusive(&g_body_lock); g_body.state = 0; g_body.addr = 0; ReleaseSRWLockExclusive(&g_body_lock); if (!addr) continue; }
        const uintptr_t o = g_player_obj.load(), pres = g_player_pres.load(); int kind = -1; if (!o || !rd(o + kForzaCarKindOffset, &kind, 4) || kind != 0) continue;
        alignas(16) float W[16], q[4]; if (!pres || !car_world(pres, W, q)) continue;
        const uint64_t now = GetTickCount64(); const float dt = lastt ? std::min(0.05f, (now - lastt) / 1000.0f) : 0.016f;
        if (lastt && dt > 0) for (int k = 0; k < 3; ++k) carV[k] = (W[12 + k] - lastT[k]) / dt;
        lastt = now; for (int k = 0; k < 3; ++k) lastT[k] = W[12 + k];
        AcquireSRWLockShared(&g_fall_lock); const float cz = g_fall.c[2]; ReleaseSRWLockShared(&g_fall_lock);
        float zone = 0; rd(o + (cz < 0 ? 0x594C : 0x5950), &zone, 4);
        if (state != 0 && zone < 0.01f) {   // repariert -> Kennzeichen wieder dran
            AcquireSRWLockExclusive(&g_body_lock); g_body.state = 0; ReleaseSRWLockExclusive(&g_body_lock); Sleep(40);
            AcquireSRWLockShared(&g_fall_lock); if (g_fall.addr == addr) memcpy((void*)addr, g_fall.orig.data(), g_fall.orig.size()); ReleaseSRWLockShared(&g_fall_lock);
            state = 0; LOG("Fall-off: car repaired, plate back"); continue; }
        if (state == 0) {
            if (thr <= 0 || zone < thr || mesh_hidden_wanted()) continue;
            // Start: Platte uebernimmt Lage und Geschwindigkeit des Autos, kleiner Stoss nach unten/aussen, zufaellige Drehung
            float R[3][3]; m3_rows(W, R); AcquireSRWLockShared(&g_fall_lock); float c[3] = {g_fall.c[0], g_fall.c[1], g_fall.c[2]}; ReleaseSRWLockShared(&g_fall_lock);
            for (int k = 0; k < 3; ++k) P[k] = W[12 + k] + c[0] * R[0][k] + c[1] * R[1][k] + c[2] * R[2][k];
            memcpy(B, R, sizeof B); const float out = c[2] < 0 ? -1.0f : 1.0f;
            for (int k = 0; k < 3; ++k) V[k] = carV[k] * 0.85f + R[2][k] * out * 1.2f + R[1][k] * 0.6f;
            unsigned r = unsigned(now * 2654435761u); auto rnd = [&]() { r = r * 1664525u + 1013904223u; return (r >> 8) / 16777216.0f * 2 - 1; };
            for (int k = 0; k < 3; ++k) om[k] = R[0][k] * (4.0f + 3 * rnd()) + R[1][k] * 2.0f * rnd() + R[2][k] * 3.0f * rnd();
            ground = W[13] + c[0] * R[0][1] + c[2] * R[2][1] + 0.01f;   // Boden unter dem Kennzeichen (Auto-Ursprung = Bodenhoehe)
            state = 1; t_fall = now; char m[160]; snprintf(m, sizeof m, "Fall-off: plate falls off (damage %.2f >= %.2f)", zone, thr); LOG(m);
        }
        if (state == 1) {   // fallen
            V[1] -= 9.81f * dt; for (int k = 0; k < 3; ++k) P[k] += V[k] * dt; rot_rows(B, om, dt);
            if (P[1] <= ground) {   // aufgekommen: flach hinlegen (Plattennormale = Zeile 2 -> nach oben oder unten)
                P[1] = ground; float up = B[2][1] >= 0 ? 1.0f : -1.0f; float x[3] = {B[0][0], 0, B[0][2]}; float l = sqrtf(x[0] * x[0] + x[2] * x[2]); if (l < 1e-4f) { x[0] = 1; x[2] = 0; l = 1; }
                x[0] /= l; x[2] /= l; B[0][0] = x[0]; B[0][1] = 0; B[0][2] = x[2]; B[2][0] = 0; B[2][1] = up; B[2][2] = 0;
                B[1][0] = B[2][1] * B[0][2] - B[2][2] * B[0][1]; B[1][1] = B[2][2] * B[0][0] - B[2][0] * B[0][2]; B[1][2] = B[2][0] * B[0][1] - B[2][1] * B[0][0];
                P[1] = ground + 0.004f; state = 2; }
        }
        if (state == 1 || state == 2) {
            float dx = P[0] - W[12], dz = P[2] - W[14];
            if (dx * dx + dz * dz > 60.0f * 60.0f || now - t_fall > 120000) {   // weit weg: nur noch ausblenden
                AcquireSRWLockExclusive(&g_body_lock); g_body.state = 3; ReleaseSRWLockExclusive(&g_body_lock); Sleep(40);
                AcquireSRWLockShared(&g_fall_lock); if (g_fall.addr == addr) memcpy((void*)addr, g_fall.hid.data(), g_fall.hid.size()); ReleaseSRWLockShared(&g_fall_lock); state = 3; continue; }
            // Boden: niedrigste gemessene Hoehe des Auto-Ursprungs unter dem Kennzeichen, solange das Auto in der Naehe ist (nach dem Aufprall federt es)
            if (dx * dx + dz * dz < 9.0f) { float R2[3][3]; m3_rows(W, R2); AcquireSRWLockShared(&g_fall_lock); const float c0 = g_fall.c[0], c2 = g_fall.c[2]; ReleaseSRWLockShared(&g_fall_lock);
                const float gy = W[13] + c0 * R2[0][1] + c2 * R2[2][1] + 0.004f; if (gy < ground) { ground = gy; if (state == 2) P[1] = ground; } }
            AcquireSRWLockExclusive(&g_body_lock); g_body.state = state; g_body.addr = addr; memcpy(g_body.P, P, sizeof P); memcpy(g_body.B, B, sizeof B); ReleaseSRWLockExclusive(&g_body_lock);
        }
        if (state == 0 || state == 3) { AcquireSRWLockExclusive(&g_body_lock); g_body.state = state; ReleaseSRWLockExclusive(&g_body_lock); }
    }
    return 0;
}
DWORD WINAPI f8_thread(LPVOID) {   // Teile-Abgleich (montierte Stossstange usw.) + Kennzeichen-Geometrie fuer das Abfallen orten
    std::string key;
    for (int t = 0;; ++t) { Sleep(50); if (t % 5 == 0) parts_poll();
        std::string car; const std::string rel = geo_current_rel(&car); const std::string k = car + "|" + rel;
        if (k != key) { key = k; ++g_fall_gen; AcquireSRWLockExclusive(&g_fall_lock); g_fall = GeoState(); ReleaseSRWLockExclusive(&g_fall_lock); if (!rel.empty()) g_fall_due = GetTickCount64() + 3000; }
        const uint64_t due = g_fall_due.load();
        if (due && GetTickCount64() >= due && ini_f(L"Plate", L"FallOffAt", 0.3f) > 0 && !g_fall_busy.exchange(true)) { g_fall_due = 0; CloseHandle(CreateThread(nullptr, 0, geo_locate, nullptr, 0, nullptr)); }
    }
    return 0;
}
DWORD WINAPI watchdog(LPVOID) {
    int dumps = 0;
    for (;;) {
        Sleep(500);
        uint64_t b = g_beat.load(); DWORD tid = g_ui_tid.load();
        if (!g_editor_open.load() || !b || !tid || GetTickCount64() - b < 3000 || dumps >= 3) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &c)) {
            char m[200]; snprintf(m, sizeof m, "Editor: HANG UI thread %lu rip=game+0x%llx rsp=%p", tid, (unsigned long long)(c.Rip - g_base), (void*)c.Rsp); LOG(m);
            std::string st; int n = 0;
            for (int i = 0; i < 0x1000 && n < 40; i += 8) {
                uint64_t v = 0; if (!rd(c.Rsp + i, &v, 8)) break;
                if (v > g_base + 0x1000 && v < g_base + 0x2c00000) { char t[24]; snprintf(t, sizeof t, "%llx ", (unsigned long long)(v - g_base)); st += t; ++n; }
            }
            LOG(("Editor: stack " + st).c_str());
        }
        ResumeThread(th); CloseHandle(th); ++dumps;
        g_beat.store(GetTickCount64());
    }
}
// ===== v4.1: Editor-Bedienung durch das Plugin =====
// TileMenuDataItem.Icon-Setter game+0x1424b30(desc, item, wert): Wert = refgezaehltes Bildobjekt @item+0x50, danach PropertyChanged.
// Beim Laden der Editor-XAML merken wir uns die 60 Setter-Aufrufe in XAML-Reihenfolge: 42 Bibliotheksbilder, 9 Feld-, 9 Auswahl-Objekte.
constexpr uintptr_t kIconSet = 0x1424b30;
using IconSetFn = void (*)(void*, void*, void*);
IconSetFn g_orig_iconset = nullptr;
struct IconCap { void* desc; void* item; void* val; };
IconCap g_cap[64]; std::atomic<int> g_cap_n{0}; std::atomic<bool> g_cap_on{false};
constexpr int kLibN = 42, kCapN = 60;   // Lib: 0 leer, 1..26 A-Z, 27..36 0-9, 37..39 Plaketten, 40 Haken, 41 Auswahl
void detour_iconset(void* desc, void* item, void* val) {
    g_orig_iconset(desc, item, val);
    if (g_cap_on.load()) { int n = g_cap_n.load(); if (n < 64) { g_cap[n] = {desc, item, val}; g_cap_n.store(n + 1); } if (n + 1 >= kCapN) g_cap_on.store(false); }
}
// v4.4: Heading-Setter game+0x1424ad0 (gleiches Muster, Wert @item+0x40): 6 Textobjekte T0 naechstes, T1 akzeptieren, T2 zurueck, T3 vorheriges, HelpA, HelpB
constexpr uintptr_t kHeadSet = 0x1424ad0;
IconSetFn g_orig_headset = nullptr;
IconCap g_hcap[8]; std::atomic<int> g_hcap_n{0}; std::atomic<bool> g_hcap_on{false};
void detour_headset(void* desc, void* item, void* val) {
    g_orig_headset(desc, item, val);
    if (g_hcap_on.load()) { int n = g_hcap_n.load(); if (n < 8) { g_hcap[n] = {desc, item, val}; g_hcap_n.store(n + 1); } if (n + 1 >= 6) g_hcap_on.store(false); }
}
bool g_ed_closing = false; void* g_ed_page = nullptr; void* g_ed_beh = nullptr; uint64_t g_drop_back_until = 0; bool g_allow_back = false;
void* g_ed_wrap = nullptr; bool g_ed_ready = false;
int g_ed_val[8]; int g_ed_cur = 0;     // Wert je Feld = Bibliotheksindex 0..39
bool g_ed_hidden = false;              // X: Schild entfernen ([Plate] Hidden=1)
void ed_set(int capidx, int lib) { if (capidx < kCapN && lib < kLibN) g_orig_iconset(g_cap[capidx].desc, g_cap[capidx].item, g_cap[lib].val); }
void ed_show_help();
void ed_show() {
    ed_show_help();
    for (int i = 0; i < 8; ++i) ed_set(kLibN + i, g_ed_val[i]);   // v4.16: Zeichen bleiben sichtbar, auch wenn das Schild entfernt ist
    for (int i = 0; i < 9; ++i) ed_set(kLibN + 9 + i, (i == g_ed_cur && (!g_ed_hidden || i == 8)) ? 41 : 0);
}
void ed_show_help() {
    if (g_hcap_n.load() < 6) return;
    auto hs = [](int dst, int src) { g_orig_headset(g_hcap[dst].desc, g_hcap[dst].item, g_hcap[src].val); };
    hs(4, g_ed_cur >= 8 ? 1 : 0);
    hs(5, (g_ed_cur > 0 && !g_ed_hidden) ? 3 : 2);
}
int lib_of_char(char c) { if (c >= 'A' && c <= 'Z') return 1 + (c - 'A'); if (c >= 'a' && c <= 'z') return 1 + (c - 'a'); if (c >= '0' && c <= '9') return 27 + (c - '0'); return 0; }
void ed_load_text(const std::string& t) {
    int f = 0;
    for (size_t i = 0; i < t.size() && f < 8; ++i) {
        if (t[i] == '{' && i + 2 < t.size() && t[i + 2] == '}' && t[i + 1] >= '0' && t[i + 1] <= '2') { g_ed_val[f++] = 37 + (t[i + 1] - '0'); i += 2; }
        else g_ed_val[f++] = lib_of_char(t[i]);
    }
    while (f < 8) g_ed_val[f++] = 0;
}
std::string ed_text() {
    std::string t;
    for (int i = 0; i < 8; ++i) { int v = g_ed_val[i];
        if (v >= 37) t += "{" + std::to_string(v - 37) + "}"; else if (v >= 27) t += char('0' + v - 27); else if (v >= 1) t += char('A' + v - 1); else t += ' '; }
    return t;
}
void ed_save() {
    std::string t = ed_text();
    std::wstring w = L"\"" + std::wstring(t.begin(), t.end()) + L"\"";   // in Anfuehrungszeichen: Leerzeichen bleiben, keine Auto-Plakette
    WritePrivateProfileStringW(L"Plate", L"Text", w.c_str(), g_ini);
    WritePrivateProfileStringW(L"Plate", L"Hidden", g_ed_hidden ? L"1" : L"0", g_ini);
    set_want(nullptr); start_gen(); live_apply(-2);
    char m[120]; snprintf(m, sizeof m, "Editor: saved \"%s\"%s", t.c_str(), g_ed_hidden ? " (plate removed)" : ""); LOG(m);
}
// Eingabe: XInput (Pad) + Pfeiltasten/Enter. Flanken mit Wiederholung.
struct XPad { DWORD pkt; WORD btn; BYTE lt, rt; SHORT lx, ly, rx, ry; };
using XGetFn = DWORD (WINAPI*)(DWORD, XPad*);
XGetFn g_xget = nullptr; bool g_xtried = false;
uint32_t ed_input() {      // Bits: 1 links 2 rechts 4 hoch 8 runter 16 A 32 X 64 Y
    if (!g_xtried) { g_xtried = true; HMODULE h = LoadLibraryW(L"xinput1_4.dll"); if (!h) h = LoadLibraryW(L"xinput9_1_0.dll"); if (h) g_xget = (XGetFn)GetProcAddress(h, "XInputGetState"); }
    uint32_t b = 0;
    if (g_xget) for (DWORD i = 0; i < 4; ++i) { XPad p{}; if (g_xget(i, &p) != 0) continue;
        if ((p.btn & 0x4) || p.lx < -16000) b |= 1; if ((p.btn & 0x8) || p.lx > 16000) b |= 2;
        if ((p.btn & 0x1) || p.ly > 16000) b |= 4; if ((p.btn & 0x2) || p.ly < -16000) b |= 8;
        if (p.btn & 0x1000) b |= 16; if (p.btn & 0x4000) b |= 32; if (p.btn & 0x8000) b |= 64; if (p.btn & 0x2000) b |= 128; }
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState(VK_BACK) & 0x8000)) b |= 128;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000) b |= 1; if (GetAsyncKeyState(VK_RIGHT) & 0x8000) b |= 2;
    if (GetAsyncKeyState(VK_UP) & 0x8000) b |= 4; if (GetAsyncKeyState(VK_DOWN) & 0x8000) b |= 8;
    if (GetAsyncKeyState(VK_RETURN) & 0x8000) b |= 16;
    return b;
}
uint32_t g_in_prev = 0xffffffff; uint64_t g_in_next[8] = {};
void ed_frame() {
    uint32_t b = ed_input(); uint64_t now = GetTickCount64(); bool ch = false;
    if (g_in_prev == 0xffffffff) { g_in_prev = b; return; }      // beim Oeffnen gedrueckte Tasten ignorieren (A von der Kachel)
    if ((b & 128) && !(g_in_prev & 128)) {   // B: ab Feld 2 = vorheriges Zeichen (Spiel-"back" wird verworfen), auf Feld 1 = Spiel geht zurueck
        if (g_ed_cur > 0 && !g_ed_hidden) { --g_ed_cur; g_drop_back_until = now + 600; g_in_prev = b; ed_show(); return; }
        set_want(nullptr); start_gen(); live_apply(-2);   // Abbruch: gespeicherten Text wiederherstellen (auch Schild an/aus)
        g_ed_ready = false; g_ed_closing = true; LOG("Editor: left (B)"); return;
    }
    for (int k = 0; k < 7; ++k) {
        uint32_t m = 1u << k; bool down = b & m, was = g_in_prev & m, fire = false;
        if (down && !was) { fire = true; g_in_next[k] = now + 380; }
        else if (down && k < 4 && now >= g_in_next[k]) { fire = true; g_in_next[k] = now + 110; }
        if (!fire) continue;
        if (g_ed_hidden && k < 5 && k != 4) continue;
        if (g_ed_hidden && k == 4) { g_ed_cur = 8; }
        if (k == 0 && g_ed_cur > 0) { --g_ed_cur; ch = true; }
        if (k == 1 && g_ed_cur < 8) { ++g_ed_cur; ch = true; }
        if ((k == 2 || k == 3) && g_ed_cur < 8) { g_ed_val[g_ed_cur] = (g_ed_val[g_ed_cur] + (k == 2 ? 1 : 39)) % 40; ch = true; }
        if (k == 4) {
            if (g_ed_cur < 8) { ++g_ed_cur; ch = true; }
            else {   // Akzeptieren: speichern und Seite verlassen
                ed_save();
                static MsvcString bk; memset(&bk, 0, sizeof bk); memcpy(bk.buf, "back", 4); bk.size = 4; bk.cap = 15;
                uint8_t act = 0;
                if (g_ed_beh && rd(reinterpret_cast<uintptr_t>(g_ed_beh) + 0x78, &act, 1) && act) {
                    reinterpret_cast<void (*)(void*, const void*)>(g_base + 0x335f20)((char*)g_ed_beh + 0x60, &bk);
                    LOG("Editor: accepted, back queued");
                } else if (g_ed_page) {
                    g_allow_back = true; (g_orig_queue_fn ? g_orig_queue_fn : reinterpret_cast<void (*)(void*, const void*)>(g_base + kQueueTransition))(g_ed_page, &bk); g_allow_back = false;
                    LOG("Editor: accepted, back via page");
                } else LOG("Editor: no page known for accept");
                g_ed_ready = false; g_ed_closing = true; return;   // Navigation bleibt aus, bis die Garage wieder laeuft
            }
        }
        if (k == 5) { g_ed_hidden = !g_ed_hidden; if (g_ed_hidden) g_ed_cur = 8;   /* v4.17: entfernt -> direkt auf Akzeptieren */ LOG(g_ed_hidden ? "Editor: plate removed" : "Editor: plate restored"); ch = true; }
        if (k == 6) { ed_load_text("HORIZON{0}"); g_ed_hidden = false; ch = true; }
    }
    g_in_prev = b;
    if (ch) { ed_show(); std::string t = (g_ed_hidden ? "!" : "") + ed_text(); set_want(&t); start_gen(); live_apply(-2); }   // Live-Vorschau (Schild an/aus sofort)
}
// v4.3: Kachel-Navigation des Spiels (game+0x1409450, baut je Richtungsdruck das Raster neu) im Editor abschalten –
// die Bedienung macht das Plugin; mit unserer Seite laeuft die Raster-Schleife game+0x140d160 sonst endlos.
constexpr uintptr_t kNavStep = 0x1409450;
using NavStepFn = void (*)(void*, void*, void*, void*);
NavStepFn g_orig_navstep = nullptr;
void detour_navstep(void* a, void* b, void* c, void* d) {
    if (g_editor_open.load()) return;   // ab Umleitung (auch bevor die Bedienung bereit ist)
    g_orig_navstep(a, b, c, d);
}
void detour_wrap(void* w) {
    g_beat.store(GetTickCount64()); g_ui_tid.store(GetCurrentThreadId());
    if (g_editor_open.load()) {
        if (!g_ed_wrap) {   // Editor-Wrapper erkennen (Upgrade-Klasse: Transition custom_upgrade)
            uint64_t tl[2] = {0, 0}; rd(reinterpret_cast<uintptr_t>(w) + 0x40, tl, 16);
            for (uint64_t p = tl[0]; p && p < tl[1] && p < tl[0] + 8 * 8; p += 8) {
                uintptr_t o = 0, sp = 0; char nm[16] = {0};
                if (rdp(p, &o) && rdp(o, &sp) && rd(sp, nm, 15) && strncmp(nm, "custom_upgrade", 14) == 0) { g_ed_wrap = w; break; }
            }
        }
        if (w != g_ed_wrap && g_ed_wrap && g_ed_closing) {   // Editor geschlossen: sobald die Garage wieder laeuft, Navigation freigeben
            uint64_t tl[2] = {0, 0}; rd(reinterpret_cast<uintptr_t>(w) + 0x40, tl, 16);
            for (uint64_t p = tl[0]; p && p < tl[1] && p < tl[0] + 8 * 8; p += 8) {
                uintptr_t o = 0, sp = 0; char nm[16] = {0};
                if (rdp(p, &o) && rdp(o, &sp) && rd(sp, nm, 15) && strncmp(nm, "barn_finds", 10) == 0) {
                    g_editor_open.store(false); g_ed_wrap = nullptr; g_ed_closing = false; g_ed_unavail.store(false); cam_set(false); LOG("Editor: garage active again"); break; }
            }
        }
        if (w == g_ed_wrap && !g_ed_beh) {   // UI-Behavior der Editor-Seite (wie game+0x263810..0x263821)
            using F1 = void* (*)(void*); using F2 = void** (*)(void*, void*);
            void* r = reinterpret_cast<F1>(g_base + 0x1923a40)((char*)w + 0x28);
            void** pp = reinterpret_cast<F2>(g_base + 0x1923a70)((char*)w + 0x28, r);
            if (pp) g_ed_beh = *pp;
        }
        if (w == g_ed_wrap && g_ed_unavail.load()) {   // Hinweis-Popup: A (oder Enter) schliesst wie B
            static uint32_t prev = 0xffffffff; uint32_t b = ed_input();
            if (prev == 0xffffffff || g_ed_closing) prev = b;
            else { if ((b & 16) && !(prev & 16) && g_ed_beh) {
                    static MsvcString bk; memset(&bk, 0, sizeof bk); memcpy(bk.buf, "back", 4); bk.size = 4; bk.cap = 15;
                    uint8_t act = 0;
                    if (rd(reinterpret_cast<uintptr_t>(g_ed_beh) + 0x78, &act, 1) && act) { reinterpret_cast<void (*)(void*, const void*)>(g_base + 0x335f20)((char*)g_ed_beh + 0x60, &bk); LOG("Editor: notice closed (A)"); g_ed_closing = true; }
                }
                prev = b; }
        }
        if (w == g_ed_wrap && !g_ed_unavail.load()) {
            if (!g_ed_ready && g_cap_n.load() >= kCapN) {
                g_ed_ready = true; g_ed_cur = 0; g_in_prev = 0xffffffff;
                ed_load_text(plate_text_ini()); g_ed_hidden = ini_f(L"Plate", L"Hidden", 0) != 0; if (g_ed_hidden) g_ed_cur = 8; ed_show();
                LOG(("Editor: controls active, text \"" + ed_text() + "\"").c_str());
                live_find();
            }
            if (g_ed_ready) ed_frame();
        }
    }
    bool seen = false; for (int i = 0; i < g_seen_n; ++i) if (g_seen_wrap[i] == w) { seen = true; break; }
    if (!seen && g_seen_n < 64) {
        g_seen_wrap[g_seen_n++] = w;
        uint64_t tl[2] = {0, 0}; rd(reinterpret_cast<uintptr_t>(w) + 0x40, tl, 16);
        std::string names; int cnt = 0;
        for (uint64_t p = tl[0]; p && p < tl[1] && cnt < 24; p += 8, ++cnt) {
            uintptr_t o = 0, sp = 0; char nm[40] = {0};
            if (rdp(p, &o) && rdp(o, &sp) && rd(sp, nm, 39)) { nm[39] = 0; names += nm; names += ' '; }
        }
        char mm[500]; snprintf(mm, sizeof mm, "Editor: wrapper %p transitions (%d): %s", w, cnt, names.c_str()); LOG(mm);
    }
    g_orig_wrap(w);
}
using QueueFn = void (*)(void*, const void*);
QueueFn g_orig_queue = nullptr;
void detour_queue(void* page, const void* str) {
    if (g_editor_open.load() && g_ed_ready && str && !g_allow_back) {
        const MsvcString* m = reinterpret_cast<const MsvcString*>(str);
        const char* p = m->cap >= 16 ? *reinterpret_cast<char* const*>(m->buf) : m->buf;
        if (m->size == 4 && memcmp(p, "back", 4) == 0) {
            if (!g_ed_page) g_ed_page = page;
            if ((g_ed_cur > 0 && !g_ed_hidden) || GetTickCount64() < g_drop_back_until) { LOG("Editor: B = previous character (back discarded)"); return; }
        }
    }
    g_orig_queue(page, str);
}
void detour_bind(void* page, const char* name, void* delegate) {
    if (g_editor_open.load() && !g_ed_ready && !g_ed_page) g_ed_page = page;
    g_orig_bind(page, name, delegate);
    if (name && strcmp(name, "PART_Button_BarnFinds") == 0) {
        struct { void* fn; uint64_t adj; } dg = { (void*)&plates_tile_clicked, 0 };
        alignas(16) uint8_t buf[16]; memcpy(buf, &dg, 16);
        g_orig_bind(page, "HZN_Button_Plates", buf);
        LOG("Editor: plates tile wired");
        // v4.23: Kamera-Suche aus (Kamera zurueckgestellt; las 57 s lang den ganzen Speicher inkl. GPU-Speicher -> Verdacht Haenger Fotomodus)
        // if (!g_cam_scan.exchange(true)) if (HANDLE t = CreateThread(nullptr, 0, cam_scan, nullptr, 0, nullptr)) CloseHandle(t);
    }
}

bool g_mh_ok = false;
// ===== v4.34 Diagnose Autowechsel: liest das Spiel die Auto-Szene (carbin) bei jedem Aufbau, auch wenn das Modell im Cache liegt? =====
// ReadScene / ReadSceneModel (RVAs wie FH2_AutovistaLayer): Pfad steht als std::string bei Datensatz+0x18.
constexpr uintptr_t kReadScene = 0x11AECB0, kReadSceneModel = 0x11ADEE0;
using SceneFn = uintptr_t (*)(void*, void*, void*);
SceneFn g_orig_rs = nullptr, g_orig_rsm = nullptr;
thread_local void* t_scene = nullptr; thread_local int t_scene_hzn = 0;
std::atomic<int> g_scene_logs{0};
static bool read_stdstr(uintptr_t s, char* out, size_t cap) {
    uint64_t len = 0, alloc = 0; if (!rd(s + 0x10, &len, 8) || !rd(s + 0x18, &alloc, 8) || len == 0 || len >= cap) return false;
    uintptr_t txt = s; if (alloc >= 16 && !rd(s, &txt, 8)) return false;
    if (!rd(txt, out, size_t(len))) return false; out[len] = 0;
    for (size_t i = 0; i < len; ++i) if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e) return false;
    return true;
}
constexpr bool kAliasEnabled = true;    // v4.39: Erkennung ueber die Praesentation des Lade-Vorgangs + Besitzer-Auto kind 0
std::atomic<uintptr_t> g_player_scene{0}; thread_local int t_scene_player = 0;
thread_local char t_scene_diag[4600]; thread_local int t_scene_diag_n = 0;
// Spieler-Szene erkennen: (1) Szene liegt in/zeigt aus der Spieler-Praesentation, (2) gelernte Szene des Spielerautos (wird beim Autowechsel wiederverwendet)
thread_local uintptr_t t_scene_pres = 0;
// Besitzer einer Praesentation: ForzaCar-Objekt (vtable g_car_vt) in den Auto-Speicherbereichen, dessen Praesentation (+0x4D38, ggf. verzoegert) = P
bool pres_owner(uintptr_t P, int* kind, int* id) {
    const uintptr_t vt = g_car_vt.load(); if (!vt || !P) return false;
    uintptr_t cs[256]; int n = 0; AcquireSRWLockShared(&g_cars_lock); n = g_cars_n; memcpy(cs, g_cars, sizeof(uintptr_t) * n); ReleaseSRWLockShared(&g_cars_lock);
    for (int i = 0; i < n; ++i) { const uintptr_t o = cs[i]; uint64_t v = 0; if (!rd(o, &v, 8) || v != vt) continue;
        uintptr_t pr = 0; if (!rdp(o + kForzaCarPresOffset, &pr)) continue;
        if (pr != P && vtable_is(pr, &kDeferredPresVtable, 1)) { uintptr_t q = 0; if (rdp(pr + kDeferredPresOffset, &q)) pr = q; }
        if (pr != P) continue;
        *kind = -1; *id = 0; rd(o + kForzaCarKindOffset, kind, 4); rd(o + kForzaCarIdOffset, id, 4); return true; }
    return false;
}
// Spieler-Szene: die Praesentation dieses Lade-Vorgangs (Stapel, fester Abstand zum Aufrufer von ReadScene) gehoert einem Auto mit kind 0
int scene_is_player(long carid, const char** why) {
    static thread_local char wb[96];
    int kd = -1, id = 0;
    if (!t_scene_pres) { *why = ""; return 0; }
    if (!pres_owner(t_scene_pres, &kd, &id)) {
        // v4.40: Auto-Zeiger in der Praesentation selbst (0x400)
        const uintptr_t vt = g_car_vt.load(); uint64_t pq[128] = {}; rd(t_scene_pres, pq, sizeof pq);
        for (int i = 0; i < 128 && vt; ++i) { uint64_t v = 0; if (pq[i] < 0x10000 || pq[i] > 0x7fffffffffffull || !rd(uintptr_t(pq[i]), &v, 8) || v != vt) continue;
            rd(uintptr_t(pq[i]) + kForzaCarKindOffset, &kd, 4); rd(uintptr_t(pq[i]) + kForzaCarIdOffset, &id, 4);
            snprintf(wb, sizeof wb, "pres %p +%x -> car kind %d id %d", (void*)t_scene_pres, i * 8, kd, id); *why = wb; return kd == 0 ? 1 : 0; }
        // 1.0.2: Merkmal in der Praesentation selbst (v134 gemessen: Spieler +0x20=1.0f, +0x28=0, +0x48=1.0f; alle 30 Drivatare +0x20=0, +0x28=-1.0f)
        { uint32_t f20 = 0, f28 = 0, f48 = 0; rd(t_scene_pres + 0x20, &f20, 4); rd(t_scene_pres + 0x28, &f28, 4); rd(t_scene_pres + 0x48, &f48, 4);
          if (f20 == 0x3f800000u && f28 == 0 && f48 == 0x3f800000u) { snprintf(wb, sizeof wb, "pres %p without car, player marker", (void*)t_scene_pres); *why = wb; return 1; }
          if (f28 == 0xbf800000u) { snprintf(wb, sizeof wb, "pres %p without car, drivatar marker", (void*)t_scene_pres); *why = wb; return 0; } }
        // Autowechsel-Luecke: altes Spieler-Objekt schon abgebaut, neues noch nicht da -> Praesentation hat noch keinen Besitzer.
        // Steht die neue Id schon im (abgebauten) Objekt, entscheidet sie; sonst gehoert die erste besitzerlose Szene der Luecke dem Spieler.
        int rk = -1; long rid = -1; player_raw(&rk, &rid);
        if (rk != 0) {
            const long pl = g_player_car_id.load();
            if (rid > 0 && rid != pl) { snprintf(wb, sizeof wb, "pres %p without car, car switch (new id %ld)", (void*)t_scene_pres, rid); *why = wb; return carid == rid ? 1 : 0; }
            if (!g_gap_scene_claimed.exchange(true)) { snprintf(wb, sizeof wb, "pres %p without car, car switch (first in gap)", (void*)t_scene_pres); *why = wb; return 1; }
        }
        snprintf(wb, sizeof wb, "pres %p without car", (void*)t_scene_pres); *why = wb; return 0; }
    snprintf(wb, sizeof wb, "pres %p belongs to car kind %d id %d", (void*)t_scene_pres, kd, id); *why = wb;
    return kd == 0 ? 1 : 0;
}
static void hexq(char* o, int& k, int cap, uintptr_t p, int n) {
    uint64_t q[16] = {}; if (n > 16) n = 16; if (!rd(p, q, size_t(n) * 8)) { k += snprintf(o + k, cap - k, " ?"); return; }
    for (int i = 0; i < n && k < cap - 20; ++i) k += snprintf(o + k, cap - k, " %llx", (unsigned long long)q[i]);
}
__attribute__((noinline)) uintptr_t detour_rs(void* scene, void* reader, void* ctx) {
    {   // Lade-Praesentation = erste Praesentation, die im Stapel zweimal im Abstand 0x90 steht (gemessen v132-v134)
        volatile uint64_t anchor = 0; uintptr_t sp = uintptr_t(&anchor) & ~uintptr_t(7);
        static thread_local uint64_t st[0x600]; SIZE_T g = 0; ReadProcessMemory(GetCurrentProcess(), (void*)sp, st, sizeof st, &g);
        t_scene_pres = 0;
        for (size_t i = 0; i + 0x90 / 8 < g / 8 && !t_scene_pres; ++i)
            if (st[i] && st[i] == st[i + 0x90 / 8] && vtable_is(uintptr_t(st[i]), kCarPresVtables, 2)) t_scene_pres = uintptr_t(st[i]);
    }
    void* prev = t_scene; int ph = t_scene_hzn, pp = t_scene_player; t_scene = scene; t_scene_hzn = 0; t_scene_player = -1;   // -1 = noch nicht geprueft
    uintptr_t r = g_orig_rs(scene, reader, ctx);
    t_scene = prev; t_scene_hzn = ph; t_scene_player = pp; return r;
}
uintptr_t detour_rsm(void* rec, void* reader, void* ctx) {
    uintptr_t r = g_orig_rsm(rec, reader, ctx);
    if (!t_scene) return r;
    char path[300]; if (!read_stdstr(uintptr_t(rec) + 0x18, path, sizeof path)) return r;
    char low[300]; for (int i = 0;; ++i) { low[i] = (char)tolower((unsigned char)path[i]); if (!path[i]) break; }
    char* hz = strstr(low, "\\scene\\exterior\\hzn\\"); if (!hz) return r;
    const char* c0 = strstr(low, "\\cars\\"); std::string car; if (c0) { c0 += 6; const char* c1 = strchr(c0, '\\'); if (c1) car.assign(c0, c1); }
    const long id = id_of(car);
    if (t_scene_player < 0) {   // erste HZN-Datei dieser Szene: gehoert sie zum Spielerauto?
        const char* why = ""; int sp = scene_is_player(id, &why);
        int kind = -1; long rid = -1; player_raw(&kind, &rid);
        // Lernen: Szene eines Autos mit der Spieler-Id, waehrend das Spieler-Objekt lebt und noch keine Spieler-Szene bekannt ist
        t_scene_player = sp ? 1 : 0;
        // Klasse merken: 1 Spieler, 0 Drivatar, 2 unbekannt (Praesentation ohne Auto) -> nur dann greift die alte Datei-Logik
        { AcquireSRWLockExclusive(&g_alias_lock); g_scene_class[car] = {GetTickCount64(), sp ? 1 : (strstr(why, "without") ? 2 : 0)}; ReleaseSRWLockExclusive(&g_alias_lock); }
        if (g_scene_logs.fetch_add(1) < 400) { char m[300]; uint32_t f[3] = {}; if (t_scene_pres) { rd(t_scene_pres + 0x20, &f[0], 4); rd(t_scene_pres + 0x28, &f[1], 4); rd(t_scene_pres + 0x48, &f[2], 4); }
            snprintf(m, sizeof m, "Scene read: %s (id %ld) %s [%s; %08x %08x %08x]", car.c_str(), id, sp ? "PLAYER -> own plate resources" : "other car", why, f[0], f[1], f[2]); LOG(m); }
    }
    if (!kAliasEnabled || t_scene_player != 1 || id <= 0) return r;
    // Pfad im std::string umschreiben: "\hzn\" -> "\hzp\" (gleiche Laenge)
    uint64_t alloc = 0; uintptr_t s0 = uintptr_t(rec) + 0x18, txt = s0;
    if (!rd(s0 + 0x18, &alloc, 8)) return r; if (alloc >= 16 && !rd(s0, &txt, 8)) return r;
    const size_t off = size_t(hz - low) + 18; char pch = (path[off] == 'N') ? 'P' : 'p'; SIZE_T w = 0;
    if (WriteProcessMemory(GetCurrentProcess(), (void*)(txt + off), &pch, 1, &w) && w == 1) {
        if (!g_alias_on.exchange(true)) LOG("Alias mode active: player car gets its own plate resources");
        AcquireSRWLockExclusive(&g_alias_lock); g_alias_car[car] = GetTickCount64(); ReleaseSRWLockExclusive(&g_alias_lock);
        if (g_alias_logs.fetch_add(1) < 300) { char m[360]; snprintf(m, sizeof m, "Alias set: %s", path); m[strlen(m)] = 0; LOG(m); }
    }
    return r;
}
extern "C" __declspec(dllexport) const char LicencePlates_Watermark[] = "LicencePlates 1.0.0 - made by Philipp55112 - do not reupload without credit";
DWORD WINAPI start(LPVOID) {
    LOG("LicencePlates 1.0.0 by Philipp55112");
    char* game = reinterpret_cast<char*>(g_base);
    // Der Code muss bereit sein (bis zu 2 Minuten warten).
    bool ok = false;
    for (int i = 0; i < 1200 && !ok; ++i) {
        ok = match(game + FH2_GAME_FORZACAR_KIND, FH2_GAME_FORZACAR_KIND_BYTES, sizeof FH2_GAME_FORZACAR_KIND_BYTES - 1) &&
             match(game + FH2_GAME_FORZACAR_UPDATE, FH2_GAME_FORZACAR_UPDATE_BYTES, sizeof FH2_GAME_FORZACAR_UPDATE_BYTES - 1);
        if (!ok) Sleep(100);
    }
    if (!ok) { LOG("Unknown FH2 build (ForzaCar code differs): staying disabled"); return 0; }
    if (!g_mh_ok) { LOG("MinHook init failed"); return 0; }
    struct { const char* name; uintptr_t at; void* detour; void** original; } const hooks[] = {
        {"ForzaCar::Update", FH2_GAME_FORZACAR_UPDATE, (void*)detour_car_update, (void**)&g_orig_car_update},
        {"beam preset upload", kBeamUpload, (void*)detour_beam_upload, (void**)&g_orig_beam},
        {"CreateLight", kCreateLight, (void*)detour_create, (void**)&g_orig_create},
        {"SetIntensity", kSetIntensity, (void*)detour_set_int, (void**)&g_orig_set_int},
    };
    for (const auto& h : hooks) {
        MH_STATUS st = MH_CreateHook(game + h.at, h.detour, h.original);
        if (st != MH_OK) {
            char m[160]; snprintf(m, sizeof m, "Hook %s failed (%s)", h.name, MH_StatusToString(st));
            LOG(m);
            return 0;
        }
    }
    if (match(game + kBindButton, kBindButtonBytes, sizeof kBindButtonBytes - 1)) {
        if (MH_CreateHook(game + kBindButton, (void*)detour_bind, (void**)&g_orig_bind) != MH_OK) LOG("Editor: tile hook failed");
    } else LOG("Editor: tile code differs, tile stays without function");
    if (match(game + kNavStep, "\x48\x89\x54\x24\x10\x55\x53\x56\x57\x41\x56\x41\x57", 13)) {
        if (MH_CreateHook(game + kNavStep, (void*)detour_navstep, (void**)&g_orig_navstep) != MH_OK) LOG("Editor: navigation hook failed");
    } else LOG("Editor: navigation code differs");
    if (match(game + kHeadSet, "\x48\x89\x5c\x24\x08\x57\x48\x83\xec\x20\x48\x8b\x4a\x40", 14)) {
        if (MH_CreateHook(game + kHeadSet, (void*)detour_headset, (void**)&g_orig_headset) != MH_OK) LOG("Editor: text hook failed");
    } else LOG("Editor: text code differs");
    if (match(game + kQueueTransition, "\x48\x8b\x89\x10\x03\x00\x00\x80\x79\x78\x00", 11)) {
        if (MH_CreateHook(game + kQueueTransition, (void*)detour_queue, (void**)&g_orig_queue) != MH_OK) LOG("Editor: transition hook failed");
        else g_orig_queue_fn = g_orig_queue;
    }
    if (match(game + kIconSet, "\x48\x89\x5c\x24\x08\x57\x48\x83\xec\x20\x48\x8b", 12)) {
        if (MH_CreateHook(game + kIconSet, (void*)detour_iconset, (void**)&g_orig_iconset) != MH_OK) LOG("Editor: icon hook failed");
    } else LOG("Editor: icon code differs");
    if (match(game + kWrapUpdate, "\x40\x57\x48\x83\xec\x50", 6)) {
        if (MH_CreateHook(game + kWrapUpdate, (void*)detour_wrap, (void**)&g_orig_wrap) != MH_OK) LOG("Editor: wrapper hook failed");
    } else LOG("Editor: wrapper code differs");
    if (match(game + kNavFocus, kNavFocusBytes, sizeof kNavFocusBytes - 1)) {
        if (MH_CreateHook(game + kNavFocus, (void*)detour_navfocus, (void**)&g_orig_navfocus) != MH_OK) LOG("Editor: focus hook failed");
    } else LOG("Editor: focus code differs");
    // v4.35: Autovista Layer hakt dieselben Funktionen ein -> am Anfang steht dann ein jmp (E9); MinHook kettet sich dahinter.
    const bool rs_ok = match(game + kReadScene, "\x48\x8b\xc4\x48\x89\x50\x10\x48\x89\x48\x08", 11) || (unsigned char)game[kReadScene] == 0xE9;
    const bool rsm_ok = match(game + kReadSceneModel, "\x40\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8b\xec", 15) || (unsigned char)game[kReadSceneModel] == 0xE9;
    { char m[160]; snprintf(m, sizeof m, "Scene code: ReadScene %02x %s, ReadSceneModel %02x %s", (unsigned char)game[kReadScene], rs_ok ? "ok" : "foreign", (unsigned char)game[kReadSceneModel], rsm_ok ? "ok" : "foreign"); LOG(m); }
    if (rs_ok && rsm_ok) {
        if (MH_CreateHook(game + kReadScene, (void*)detour_rs, (void**)&g_orig_rs) != MH_OK || MH_CreateHook(game + kReadSceneModel, (void*)detour_rsm, (void**)&g_orig_rsm) != MH_OK) LOG("Scene hook failed");
    } else LOG("Scene code differs");
    g_orig_set_pos = reinterpret_cast<SetVecFn>(game + kSetPosition);
    g_orig_set_dir = reinterpret_cast<SetVecFn>(game + kSetDirection);
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { LOG("Hooks could not be enabled"); return 0; }
    if (!g_ev_reg.load()) { uintptr_t vt = 0; rd(g_base + kBarnFindsEvent, &vt, 8); if (vt) register_ev("after start"); }
    if (HANDLE t = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, f8_thread, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, fall_thread, nullptr, 0, nullptr)) CloseHandle(t);
    LOG("hooked; lamps appear as soon as the player car drives (switch car once if needed)");
    return 0;
}

}  // namespace

struct FH2PluginDatabaseFrame {
    uint32_t size, api_version;
    void* database;
    int (*prepare)(void*, const char*, int, void**, const char**);
    int (*step)(void*);
    int (*finalize)(void*);
    int64_t (*column_int64)(void*, int);
};

extern "C" __declspec(dllexport) BOOL WINAPI FH2PluginDatabaseUpdate(const FH2PluginDatabaseFrame* f) {
    if (!f || !f->prepare || !f->step || !f->finalize || !f->column_int64 || !f->database) return FALSE;
    int found = 0;
    for (int i = 0; i < g_tab_n; ++i) {
        char sql[200];
        snprintf(sql, sizeof sql, "SELECT Id FROM Data_Car WHERE MediaName='%s' COLLATE NOCASE LIMIT 1", g_tab[i].name);
        void* st = nullptr;
        if (f->prepare(f->database, sql, -1, &st, nullptr) != 0 || !st) continue;
        if (f->step(st) == 100) { g_tab[i].id = long(f->column_int64(st, 0)); ++found; } else if (g_tab[i].id > 0) ++found;
        f->finalize(st);
    }
    g_tab_ids.store(found);
    char m[96]; snprintf(m, sizeof m, "Car table: %d of %d cars linked to an id", found, g_tab_n);
    LOG(m);
    return TRUE;
}

struct FH2PluginContext {
    uint32_t size, api_version, adapter, game_image_size;
    void* game_image;
    HMODULE plugin_module;
    const wchar_t* plugins_directory;
    const wchar_t* plugin_path;
    const char* extra_options_version;
    void (*log_utf8)(const char* message);
};

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

extern "C" __declspec(dllexport) BOOL WINAPI FH2PluginInitialize(const FH2PluginContext* ctx) {
    if (!ctx || ctx->api_version != 1u || ctx->size < sizeof(*ctx) || !ctx->game_image) return FALSE;
    g_logfn = ctx->log_utf8;
    g_base = reinterpret_cast<uintptr_t>(ctx->game_image);
    swprintf(g_ini, MAX_PATH, L"%ls\\PlateLight.ini", ctx->plugins_directory);
    write_default_ini();
    wchar_t tab[MAX_PATH];
    swprintf(tab, MAX_PATH, L"%ls\\PlateLightCars.csv", ctx->plugins_directory);
    load_table(tab);
    { wchar_t idf[MAX_PATH]; swprintf(idf, MAX_PATH, L"%ls\\PlateLightIds.csv", ctx->plugins_directory); load_ids(idf); }
    { wchar_t hf[MAX_PATH]; swprintf(hf, MAX_PATH, L"%ls\\PlateHide.csv", ctx->plugins_directory); load_hide(hf); }
    { wchar_t vf[MAX_PATH]; swprintf(vf, MAX_PATH, L"%ls\\PlateLightVariants.csv", ctx->plugins_directory); load_variants(vf); }
    { wchar_t pf[MAX_PATH]; swprintf(pf, MAX_PATH, L"%ls\\PlateParts.csv", ctx->plugins_directory); load_parts(pf); }
    { wchar_t sf[MAX_PATH]; swprintf(sf, MAX_PATH, L"%ls\\PlateLampSize.csv", ctx->plugins_directory); load_lampsize(sf); }
    { char m[96]; snprintf(m, sizeof m, "Table loaded: %d cars", g_tab_n); LOG(m); }
    load_config();
    g_pdir = ctx->plugins_directory; g_gen_done = CreateEventW(nullptr, TRUE, FALSE, nullptr); start_gen();
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExW(g_ini, GetFileExInfoStandard, &d)) g_cfg_time = d.ftLastWriteTime;
    LOG("loaded");
    g_mh_ok = MH_Initialize() == MH_OK;
    if (g_mh_ok) hook_files();          // v3.6: Datei-Hooks sofort (Stateflow wird sehr frueh geladen)
    if (g_mh_ok && match(reinterpret_cast<char*>(g_base) + kBarnFindsEventInit, "\x48\x83\xec\x28\x48\x8d\x15", 7)) {
        uintptr_t vt = 0; rd(g_base + kBarnFindsEvent, &vt, 8);
        if (vt) register_ev("game already initialised");
        else if (MH_CreateHook(reinterpret_cast<char*>(g_base) + kBarnFindsEventInit, (void*)detour_bfinit, (void**)&g_orig_bfinit) == MH_OK) MH_EnableHook(reinterpret_cast<char*>(g_base) + kBarnFindsEventInit);
    }
    if (HANDLE t = CreateThread(nullptr, 0, start, nullptr, 0, nullptr)) CloseHandle(t);
    return TRUE;
}
