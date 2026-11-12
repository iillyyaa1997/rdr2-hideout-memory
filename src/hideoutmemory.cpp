// HideoutMemory.asi - Bandit Hideouts skulls show up on the map only once you have
// been near a hideout, and then stay there.
//
// Bandit Hideouts (Nexus 2213) with ALWAYS_SHOW_HIDEOUT_BLIP=1 puts a skull on the
// map for every hideout. HideoutMemory watches the blips Bandit_Hideouts.asi creates
// (BLIP_ADD_FOR_RADIUS / BLIP_ADD_FOR_COORDS, then SET_BLIP_SPRITE with the skull),
// hides the skulls of hideouts the player has not been near (BLIP_MODIFIER_HIDDEN)
// and shows them for good once the player comes within Radius metres. Visited
// hideouts are kept in <game>\HideoutMemory.dat. Bandit Hideouts itself still
// removes the skull of a cleared hideout until it respawns.
// With OwnBlips=1 a visited hideout is shown by our own point blip (the game's hideout
// style and map card, the same skull) instead of the Bandit Hideouts area blip, which the
// map does not highlight; it lives exactly as long as the Bandit Hideouts skull does.
//
// How: ScriptHook mods call natives through three exports of ScriptHookRDR2.dll
// (nativeInit, nativePush64, nativeCall). HideoutMemory rewrites those import-table
// entries of Bandit_Hideouts.asi only, and passes every call on to whatever the
// entry held before - the ScriptHook export, or another plugin's hook such as
// RuText, which translates the same mod's text. It patches a few seconds after the
// game starts, after other plugins that patch at load time, and checks its entries
// every tick in case another plugin replaced them later.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* kVersion = "1.1.0";

constexpr const char* kExpInit = "?nativeInit@@YAX_K@Z";
constexpr const char* kExpPush = "?nativePush64@@YAX_K@Z";
constexpr const char* kExpCall = "?nativeCall@@YAPEA_KXZ";
constexpr const char* kExpScriptRegister = "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z";
constexpr const char* kExpScriptWait = "?scriptWait@@YAXK@Z";

using FnInit = void (*)(uint64_t);
using FnPush = void (*)(uint64_t);
using FnCall = uint64_t* (*)();
using FnScriptRegister = void (*)(HMODULE, void (*)());
using FnScriptWait = void (*)(DWORD);

constexpr uint64_t kBlipAddForCoords = 0x554D9D53F696D002ULL;
constexpr uint64_t kBlipAddForRadius = 0x45F13B7E0A15C880ULL;  // what Bandit Hideouts uses
constexpr uint64_t kSetBlipSprite = 0x74F74D3207ED525CULL;
constexpr uint64_t kDoesBlipExist = 0xCD82FA174080B3B1ULL;
constexpr uint64_t kRemoveBlip = 0xF2C3C9DA47AAA54AULL;  // REMOVE_BLIP(Blip*)
constexpr uint64_t kBlipAddModifier = 0x662D364ABF16DE2FULL;
constexpr uint64_t kBlipRemoveModifier = 0xB059D7BD3D78C16FULL;
constexpr uint64_t kPlayerPedId = 0x096275889B8E0EE0ULL;
constexpr uint64_t kGetEntityCoords = 0xA86D5F069399F44DULL;
constexpr uint64_t kSetBlipNameFromTextFile = 0x0A062D6D7C0B2C2CULL;
constexpr uint64_t kSetBlipMapCardInfo = 0x02FF4CF43B7209D1ULL;  // (blip, card type, hash); nativedb: SET_BLIP_FLASH_TIMER
constexpr uint32_t kModifierHidden = 0xB946AEF0;  // BLIP_MODIFIER_HIDDEN
constexpr uint32_t kSpriteHideout = 0x5846C31D;   // blip_ambient_bounty_target (HIDEOUT_ICON=1)
// Our own point blip for a visited hideout: Bandit Hideouts draws an area blip (BLIP_STYLE_RCM),
// which the map neither highlights on hover nor gives a card. BLIP_STYLE_MP_HIDEOUT is the game's
// hideout style (label BLIP_REGION_HIDEOUT); card type 57 is the game's hideout card, which the
// game's own map script (map_app_event_handler: func_6/func_25/func_37) fills with its title and picture.
constexpr uint32_t kStyleHideout = 0xE1C50372;  // BLIP_STYLE_MP_HIDEOUT
constexpr int kMapCardHideout = 57;
constexpr const char* kHideoutLabel = "BLIP_REGION_HIDEOUT";

HMODULE g_self = nullptr;
HMODULE g_sh = nullptr;
HMODULE g_target = nullptr;  // Bandit_Hideouts.asi
FnInit g_shInit = nullptr;   // ScriptHook exports, for our own native calls
FnPush g_shPush = nullptr;
FnCall g_shCall = nullptr;
FnScriptWait g_wait = nullptr;
FnInit g_nextInit = nullptr;  // what the target's import entries held before us
FnPush g_nextPush = nullptr;
FnCall g_nextCall = nullptr;

bool g_enabled = true;
bool g_ownBlips = true;  // 1 = our own point blip with a map card instead of the Bandit Hideouts skull
float g_radius = 150.0f;
std::wstring g_dir;
FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
  if (!g_log) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}

// ---------------------------------------------------------------- memory

struct Blip {
  int handle;
  float x, y, z;
  bool skull;
  bool shown;
  uint32_t sprite;  // the skull sprite Bandit Hideouts gave it
  int own;          // our point blip standing in for it, 0 if none
};
std::vector<Blip> g_blips;                     // game thread only (script fibers)
std::vector<int> g_orphans;                    // our stand-in blips to remove on the next tick
std::vector<std::array<float, 3>> g_visited;  // hideouts the player has been near
int g_logged = 0;
DWORD g_scriptStart = 0;  // GetTickCount when our script started; skulls added later are logged

bool LateEnough() { return g_scriptStart && GetTickCount() - g_scriptStart > 30000; }

Blip* Find(int handle) {
  for (auto& b : g_blips)
    if (b.handle == handle) return &b;
  return nullptr;
}

std::wstring DataFile() { return g_dir + L"\\HideoutMemory.dat"; }

void LoadVisited() {
  g_visited.clear();
  FILE* f = _wfopen(DataFile().c_str(), L"r");
  if (!f) return;
  float x, y, z;
  while (fscanf(f, "%f %f %f", &x, &y, &z) == 3) g_visited.push_back({x, y, z});
  fclose(f);
}

void SaveVisited() {
  std::wstring tmp = DataFile() + L".tmp";
  FILE* f = _wfopen(tmp.c_str(), L"w");
  if (!f) return;
  for (auto& v : g_visited) fprintf(f, "%.1f %.1f %.1f\n", v[0], v[1], v[2]);
  fclose(f);
  MoveFileExW(tmp.c_str(), DataFile().c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Visited(const Blip& b) {
  for (auto& v : g_visited) {
    float dx = v[0] - b.x, dy = v[1] - b.y;
    if (dx * dx + dy * dy < 25.0f) return true;  // the same hideout: within 5 m
  }
  return false;
}

void Track(int handle, float x, float y, float z, uint32_t hash) {
  if (!handle) return;
  if (g_logged < 10 || LateEnough()) {
    ++g_logged;
    Log("blip %d at %.1f %.1f, hash 0x%08X", handle, x, y, hash);
  }
  for (auto& b : g_blips)
    if (b.handle == handle) {
      if (b.own) g_orphans.push_back(b.own);  // a reused handle: our stand-in goes, the tick makes a new one
      b = Blip{handle, x, y, z, hash == kSpriteHideout, false, 0, 0};
      return;
    }
  g_blips.push_back(Blip{handle, x, y, z, hash == kSpriteHideout, false, 0, 0});
}

void Sprite(int handle, uint32_t sprite) {
  Blip* b = Find(handle);
  if (!b) return;
  const bool skull = sprite == kSpriteHideout;
  if (LateEnough() && skull != b->skull) {
    Log("blip %d at %.1f %.1f: %s", handle, b->x, b->y, skull ? "skull set" : "sprite changed from skull");
  } else if (LateEnough() && skull) {
    Log("blip %d at %.1f %.1f: skull set again", handle, b->x, b->y);
  }
  b->skull = skull;  // another sprite is not ours to hide or show
  if (skull) b->sprite = sprite;
  b->shown = false;  // a new sprite may reset modifiers: settle on the next tick
}

// Bandit Hideouts removes the skull of a cleared hideout; the log shows when, and when a skull
// comes back, so a skull that stays after a clear can be traced
void Removed(int handle) {
  Blip* b = Find(handle);
  if (b && b->skull) Log("skull blip %d at %.1f %.1f removed by Bandit Hideouts", handle, b->x, b->y);
}

void Modifier(int handle, uint32_t modifier, bool add) {
  Blip* b = Find(handle);
  if (b && b->skull && LateEnough()) {
    Log("Bandit Hideouts %s modifier 0x%08X on skull blip %d at %.1f %.1f", add ? "adds" : "removes", modifier, handle,
        b->x, b->y);
  }
}

// ---------------------------------------------------------------- hooks on the target's imports

struct CallState {
  int kind = 0;  // 1 = blip add, 2 = SET_BLIP_SPRITE, 3 = REMOVE_BLIP, 4/5 = modifier add/remove
  int index = 0;
  uint64_t args[4] = {};
};
thread_local CallState t_call;

float ArgFloat(uint64_t v) {
  float f;
  uint32_t lo = (uint32_t)v;
  memcpy(&f, &lo, 4);
  return f;
}

void HookInit(uint64_t hash) {
  t_call.kind = !g_enabled ? 0
                : (hash == kBlipAddForRadius || hash == kBlipAddForCoords) ? 1
                : hash == kSetBlipSprite      ? 2
                : hash == kRemoveBlip         ? 3
                : hash == kBlipAddModifier    ? 4
                : hash == kBlipRemoveModifier ? 5
                                              : 0;
  t_call.index = 0;
  // tail call: a plugin further down the chain (RuText) looks at the return address
  // to tell which mod is calling, so it must still point into Bandit_Hideouts.asi
  return g_nextInit(hash);
}

void HookPush(uint64_t value) {
  if (t_call.kind && t_call.index < 4) t_call.args[t_call.index] = value;
  ++t_call.index;
  return g_nextPush(value);
}

uint64_t* HookCall() {
  if (t_call.kind == 3 && t_call.args[0]) {
    int handle = 0;  // the game zeroes *blip, so read it before the call
    memcpy(&handle, (const void*)t_call.args[0], sizeof handle);
    Removed(handle);
  } else if (t_call.kind == 4 || t_call.kind == 5) {
    Modifier((int)(uint32_t)t_call.args[0], (uint32_t)t_call.args[1], t_call.kind == 4);
  }
  uint64_t* r = g_nextCall();
  if (t_call.kind == 1 && r) {
    int handle = 0;
    memcpy(&handle, r, sizeof handle);
    Track(handle, ArgFloat(t_call.args[1]), ArgFloat(t_call.args[2]), ArgFloat(t_call.args[3]),
          (uint32_t)t_call.args[0]);
  } else if (t_call.kind == 2) {
    Sprite((int)(uint32_t)t_call.args[0], (uint32_t)t_call.args[1]);
  }
  t_call.kind = 0;
  return r;
}

// Rewrite the target's three import entries. An entry that does not hold our hook
// yet is taken over and its old value becomes the next link in the chain.
int PatchTarget() {
  if (!g_target) return 0;
  auto base = (uint8_t*)g_target;
  auto dos = (IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
  auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return 0;
  int changed = 0;
  for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
    if (_stricmp((const char*)(base + d->Name), "ScriptHookRDR2.dll") != 0 || !d->OriginalFirstThunk) continue;
    auto names = (IMAGE_THUNK_DATA64*)(base + d->OriginalFirstThunk);
    auto slots = (IMAGE_THUNK_DATA64*)(base + d->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
      const char* nm = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
      uint64_t hook = 0;
      uint64_t* next = nullptr;
      if (!strcmp(nm, kExpInit)) hook = (uint64_t)&HookInit, next = (uint64_t*)&g_nextInit;
      else if (!strcmp(nm, kExpPush)) hook = (uint64_t)&HookPush, next = (uint64_t*)&g_nextPush;
      else if (!strcmp(nm, kExpCall)) hook = (uint64_t)&HookCall, next = (uint64_t*)&g_nextCall;
      if (!hook || slots->u1.Function == hook) continue;
      *next = slots->u1.Function;
      DWORD old;
      if (VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old)) {
        slots->u1.Function = hook;
        VirtualProtect(&slots->u1.Function, 8, old, &old);
        ++changed;
      }
    }
  }
  return changed;
}

const char* ChainName(uint64_t fn) {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR)fn, &m);
  static char name[MAX_PATH];
  name[0] = 0;
  if (m) GetModuleFileNameA(m, name, MAX_PATH);
  const char* s = strrchr(name, '\\');
  return s ? s + 1 : name;
}

void LogChain(const char* when) {
  Log("%s: calls go on to %s", when, ChainName((uint64_t)g_nextInit));
}

// ---------------------------------------------------------------- script

template <typename R = uint64_t, typename... A>
R Native(uint64_t hash, A... args) {
  g_shInit(hash);
  (g_shPush([](auto v) {
     uint64_t r = 0;
     memcpy(&r, &v, sizeof(v));
     return r;
   }(args)),
   ...);
  uint64_t* res = g_shCall();
  R out{};
  memcpy(&out, res, sizeof(R));
  return out;
}

void Hide(int handle) {
  // re-applied every tick: Bandit Hideouts may restyle its blip and drop modifiers
  Native<int>(kBlipRemoveModifier, handle, kModifierHidden);
  Native<int>(kBlipAddModifier, handle, kModifierHidden);
}

void RemoveOwn(int& own) {
  if (own && Native<int>(kDoesBlipExist, own)) {
    g_shInit(kRemoveBlip);
    g_shPush((uint64_t)&own);
    g_shCall();
  }
  own = 0;
}

// A point blip in the game's hideout style with the Bandit Hideouts skull on it and the game's
// hideout card: the map highlights it under the cursor and shows the card when it is selected
int AddOwn(const Blip& b) {
  int own = Native<int>(kBlipAddForCoords, kStyleHideout, b.x, b.y, b.z);
  if (!own) return 0;
  Native<int>(kSetBlipMapCardInfo, own, kMapCardHideout, 0);
  Native<int>(kSetBlipSprite, own, b.sprite ? b.sprite : kSpriteHideout, 1);
  Native<int>(kSetBlipNameFromTextFile, own, kHideoutLabel);
  return own;
}

void Tick() {
  for (int& o : g_orphans) RemoveOwn(o);
  g_orphans.clear();
  if (!g_enabled || g_blips.empty()) return;
  int ped = Native<int>(kPlayerPedId);
  g_shInit(kGetEntityCoords);
  g_shPush((uint64_t)(uint32_t)ped);
  g_shPush(1);
  g_shPush(1);
  float v[6];
  memcpy(v, g_shCall(), sizeof v);  // x, pad, y, pad, z, pad
  float px = v[0], py = v[2];
  bool saved = false;
  int skulls = 0, hidden = 0, own = 0, created = 0;
  for (size_t i = 0; i < g_blips.size();) {
    Blip& b = g_blips[i];
    if (!Native<int>(kDoesBlipExist, b.handle)) {
      // Bandit Hideouts took its skull away (a cleared hideout): our stand-in goes with it
      if (b.own) Log("hideout at %.1f %.1f: skull gone, our blip removed", b.x, b.y);
      RemoveOwn(b.own);
      g_blips.erase(g_blips.begin() + i);
      continue;
    }
    ++i;
    if (!b.skull) {
      RemoveOwn(b.own);
      continue;
    }
    ++skulls;
    bool visited = Visited(b);
    float dx = px - b.x, dy = py - b.y;
    if (!visited && dx * dx + dy * dy < g_radius * g_radius) {
      g_visited.push_back({b.x, b.y, b.z});
      saved = visited = true;
      Log("visited the hideout at %.1f %.1f", b.x, b.y);
    }
    if (visited && g_ownBlips) {
      if (!b.own || !Native<int>(kDoesBlipExist, b.own)) {
        b.own = AddOwn(b);
        if (b.own) ++created;
      }
      if (b.own) {
        Hide(b.handle);
        ++own;
      } else {  // could not make ours: keep the Bandit Hideouts skull on the map
        Native<int>(kBlipRemoveModifier, b.handle, kModifierHidden);
      }
    } else if (visited) {
      RemoveOwn(b.own);
      if (!b.shown) {
        Native<int>(kBlipRemoveModifier, b.handle, kModifierHidden);
        b.shown = true;
      }
    } else {
      RemoveOwn(b.own);
      Hide(b.handle);
      b.shown = false;
      ++hidden;
    }
  }
  if (saved) SaveVisited();
  static int lastSkulls = -1, lastHidden = -1, lastOwn = -1;
  if (skulls != lastSkulls || hidden != lastHidden || own != lastOwn || created) {
    Log("%d hideout skulls, %d hidden, %d shown as our blips (%d made now), %d visited in total", skulls, hidden, own,
        created, (int)g_visited.size());
    lastSkulls = skulls;
    lastHidden = hidden;
    lastOwn = own;
  }
}

void ScriptMain() {
  Log("script started");
  g_scriptStart = GetTickCount();
  for (;;) {
    // another plugin may have rewritten the entries after us: take them over again
    if (PatchTarget()) LogChain("import entries changed, chained again");
    Tick();
    g_wait(2000);
  }
}

void ReadIni() {
  std::wstring ini = g_dir + L"\\HideoutMemory.ini";
  g_enabled = GetPrivateProfileIntW(L"HideoutMemory", L"Enabled", 1, ini.c_str()) != 0;
  g_radius = (float)GetPrivateProfileIntW(L"HideoutMemory", L"Radius", 150, ini.c_str());
  g_ownBlips = GetPrivateProfileIntW(L"HideoutMemory", L"OwnBlips", 1, ini.c_str()) != 0;
}

// Waits for Bandit_Hideouts.asi and for plugins that patch at load time (RuText),
// then takes over the target's import entries. Scripts start much later, when the
// story loads, so nothing calls natives while the entries change.
DWORD WINAPI Boot(LPVOID) {
  for (int i = 0; i < 600 && !(g_sh = GetModuleHandleW(L"ScriptHookRDR2.dll")); ++i) Sleep(100);
  if (!g_sh) {
    Log("ScriptHookRDR2.dll not loaded, inactive");
    return 0;
  }
  g_shInit = (FnInit)GetProcAddress(g_sh, kExpInit);
  g_shPush = (FnPush)GetProcAddress(g_sh, kExpPush);
  g_shCall = (FnCall)GetProcAddress(g_sh, kExpCall);
  g_wait = (FnScriptWait)GetProcAddress(g_sh, kExpScriptWait);
  auto reg = (FnScriptRegister)GetProcAddress(g_sh, kExpScriptRegister);
  if (!g_shInit || !g_shPush || !g_shCall || !g_wait || !reg) {
    Log("ScriptHookRDR2 exports not found, inactive");
    return 0;
  }
  for (int i = 0; i < 600 && !(g_target = GetModuleHandleW(L"Bandit_Hideouts.asi")); ++i) Sleep(100);
  if (!g_target) {
    Log("Bandit_Hideouts.asi not loaded, inactive");
    return 0;
  }
  Sleep(5000);  // let load-time patchers finish first
  int n = PatchTarget();
  Log("took over %d import entries of Bandit_Hideouts.asi", n);
  LogChain("chain");
  reg(g_self, ScriptMain);
  return 0;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  g_self = inst;
  DisableThreadLibraryCalls(inst);
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(inst, path, MAX_PATH);
  std::wstring p = path;
  g_dir = p.substr(0, p.find_last_of(L"\\/"));
  g_log = _wfopen((g_dir + L"\\HideoutMemory.log").c_str(), L"w");
  Log("HideoutMemory %s loaded", kVersion);
  ReadIni();
  LoadVisited();
  Log("Enabled=%d Radius=%.0f OwnBlips=%d, %d hideouts visited before", g_enabled, g_radius, g_ownBlips,
      (int)g_visited.size());
  HANDLE t = CreateThread(nullptr, 0, Boot, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
  return TRUE;
}
