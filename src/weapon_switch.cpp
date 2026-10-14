#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "MinHook.h"
#include "wml.h"

namespace {

// ---------------------------------------------------------------- addresses

constexpr uint32_t PLAYER_PTR   = 0x8309ABEC;  // -> player
constexpr uint32_t MP_FLAG      = 0x8370E9F6;  // byte, non-zero in any multiplayer session
constexpr uint32_t OFF_TYPE     = 72;          // object type; 1 == player
constexpr uint32_t OFF_LIST     = 2324;        // head of the carry list
constexpr uint32_t OFF_EQUIPPED = 3548;        // node in your hands, 0 = fists
constexpr uint32_t OFF_TAGGING  = 3700;        // tagging spot, -1 when not tagging
constexpr uint32_t OFF_N_NEXT   = 0;           // node -> next (circular)
constexpr uint32_t OFF_N_DEF    = 8;           // node -> weapon definition
constexpr uint32_t OFF_N_COUNT  = 12;          // node -> how many you have
constexpr uint32_t OFF_D_NAME   = 0;           // def  -> name, char*

constexpr uint32_t HOOK_FRAME = 0x82167690;    // per-frame player method, r3 = player
constexpr uint32_t HOOK_INPUT = 0x827166D0;    // XamInputGetState wrapper (user, state)
constexpr uint32_t HOOK_AXIS  = 0x8216F128;    // axis value (axis) -> f1
constexpr uint32_t EQUIP_FN   = 0x82446280;    // equip(player, node)

constexpr uint32_t ACTION_TABLE   = 0x829CC818;  // 16 bytes per action
constexpr uint32_t ACTION_WEAPON_MENU = 0;
constexpr int      AXIS_WHEEL_Y   = 4;           // the wheel's pointer, up / down
constexpr int      AXIS_WHEEL_X   = 5;           // and left / right
constexpr int      AXIS_CAR_WHEEL_Y = 12;        // the same in a vehicle
constexpr int      AXIS_CAR_WHEEL_X = 13;

constexpr uint32_t UI_ACTIVE      = 0x8281C7EC;  // open UI elements: +4 array, +12 count
constexpr uint32_t UI_WEAPON_MENU = 0x8281C8F0;  // the weapon wheel
constexpr uint32_t PAUSE_ON_WHEEL = 0x8370DED5;  // byte pause_on_weapon_switch

// "Is something other than gameplay on screen", the same tests the port's
// keyboard code uses (project/src/kbm.cpp): two menu slots and two more menu
// words, and the two cutscene flags.
constexpr uint32_t MENU_WORDS[] = {0x839E0DF8, 0x839E0FB0, 0x82FFE434, 0x82FFE43C};
constexpr uint32_t CUTSCENE_FLAGS[] = {0x8370D991, 0x8370D990};
constexpr uint32_t OBJECT_TABLE = 0x830866C8;

// XINPUT_STATE in guest memory: packet number, then the gamepad.
constexpr uint32_t PAD_PACKET  = 0;
constexpr uint32_t PAD_BUTTONS = 4;
constexpr uint32_t PAD_LX      = 8;
constexpr uint32_t PAD_LY      = 10;
constexpr uint32_t PAD_RX      = 12;
constexpr uint32_t PAD_RY      = 14;

constexpr uint16_t BTN_B = 0x2000;

// The game's button numbers (the action table) as XInput bits. 6 and 7 are the
// triggers, which are not bits.
constexpr uint16_t kButtonBits[16] = {
    0x1000, 0x2000, 0x4000, 0x8000,   // A B X Y
    0x0100, 0x0200, 0, 0,             // LB RB LT RT
    0x0008, 0x0001, 0x0004, 0x0002,   // D-pad right up left down
    0x0010, 0x0020, 0x0040, 0x0080};  // Start Back LS RS

// A sanity filter for pointers about to be read, not a real memory boundary.
constexpr uint32_t GUEST_LO = 0x00100000u;
constexpr uint32_t GUEST_HI = 0xF0000000u;

constexpr int MAX_NODES  = 64;   // the carry list is circular; this bounds the walk
constexpr int CATEGORIES = 8;    // 0 fists, 1 melee ... 7 thrown
constexpr int NAME_MAX   = 32;
constexpr int KEYS_PER   = 4;    // keys per setting in mod.ini

const char* const kCategoryNames[CATEGORIES] = {
    "fists", "melee", "pistol", "SMG", "shotgun", "rifle", "special", "thrown"};

// Every weapon in weapons.xtbl, by the name the game uses, and its category.
struct WeaponRow { const char* name; int category; };
constexpr WeaponRow kWeapons[] = {
    {"baseball_bat", 1}, {"kabob", 1}, {"knife", 1}, {"knifel", 1}, {"lighterL", 1},
    {"nightstick", 1}, {"pimp_slap", 1}, {"spraycancut", 1}, {"tire_iron", 1},
    {"watchcase", 1},
    {"Tanya_pistol", 2}, {"beretta", 2}, {"briefcaseL", 2}, {"camera", 2},
    {"cellphone", 2}, {"cellphone1", 2}, {"cellphone1l", 2}, {"cellphoneL", 2},
    {"desert eagle", 2}, {"desert_eagle_gold", 2}, {"glock", 2}, {"glockL", 2},
    {"liquorbottle", 2}, {"magnum", 2}, {"magnumL", 2}, {"pimpcup", 2},
    {"mac10", 3}, {"mac10L", 3}, {"tec9", 3}, {"tec9b", 3},
    {"pimpcane", 4}, {"pump_action_shotgun", 4}, {"spas12", 4}, {"spas12b", 4},
    {"twelve_gauge", 4},
    {"ak47", 5}, {"ak47L", 5}, {"m16", 5}, {"sniper_rifle", 5},
    {"rpg_laser", 6}, {"rpg_launcher", 6},
    {"40oz", 7}, {"bombsml", 7}, {"gin_n_juice", 7}, {"grenade", 7}, {"hennessy", 7},
    {"krystal", 7}, {"large_softdrink", 7}, {"molotov", 7}, {"pcp", 7}, {"pipe_bomb", 7},
};

// ---------------------------------------------------------------- settings

struct Settings {
  int keys[CATEGORIES][KEYS_PER] = {};  // virtual-key codes, 0 = unused
  int next_keys[KEYS_PER] = {};         // keyboard next / previous weapon
  int prev_keys[KEYS_PER] = {};
  uint16_t pad_next = 0x0008;           // D-pad right
  uint16_t pad_prev = 0x0004;           // D-pad left
  uint16_t pad_wheel = 0;               // 0 = follow the game's controller layout
  int wheel_key = 'Q';                  // keyboard weapon wheel key
  bool wheel = true;
  int wheel_dir = -1;                   // -1: scrolling down goes to the next weapon
  bool tap_switch = true;
  int hold_ms = 200;
  bool move_with_wheel = true;
  int debug_key = VK_F8;
};

std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string Upper(std::string s) {
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return s;
}

// Named keys, as the loader accepts them for Lua mods. Letters, digits, F1-F24
// and NUMPAD0-9 are handled in code.
struct KeyNameRow { const char* name; int vk; };
constexpr KeyNameRow kKeyNames[] = {
    {"SPACE", VK_SPACE},    {"ENTER", VK_RETURN},  {"RETURN", VK_RETURN}, {"ESC", VK_ESCAPE},
    {"ESCAPE", VK_ESCAPE},  {"TAB", VK_TAB},       {"BACKSPACE", VK_BACK}, {"SHIFT", VK_SHIFT},
    {"CTRL", VK_CONTROL},   {"CONTROL", VK_CONTROL}, {"ALT", VK_MENU},    {"UP", VK_UP},
    {"DOWN", VK_DOWN},      {"LEFT", VK_LEFT},     {"RIGHT", VK_RIGHT},   {"INSERT", VK_INSERT},
    {"DELETE", VK_DELETE},  {"HOME", VK_HOME},     {"END", VK_END},       {"PAGEUP", VK_PRIOR},
    {"PAGEDOWN", VK_NEXT},  {"PLUS", VK_OEM_PLUS}, {"MINUS", VK_OEM_MINUS},
    {"MOUSE4", VK_XBUTTON1}, {"MOUSE5", VK_XBUTTON2}, {"MMB", VK_MBUTTON},
};

// Key name -> Windows virtual-key code, 0 if unknown.
int KeyFromName(const std::string& raw) {
  const std::string n = Upper(Trim(raw));
  if (n.size() == 1 && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9'))) return n[0];
  if (n.size() >= 2 && n[0] == 'F') {
    const int f = std::atoi(n.c_str() + 1);
    if (f >= 1 && f <= 24) return VK_F1 + f - 1;
  }
  if (n.size() == 7 && n.rfind("NUMPAD", 0) == 0 && n[6] >= '0' && n[6] <= '9') {
    return VK_NUMPAD0 + (n[6] - '0');
  }
  for (const auto& k : kKeyNames) {
    if (n == k.name) return k.vk;
  }
  return 0;
}

// The other way, for the log.
std::string KeyName(int vk) {
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
  if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "NUMPAD" + std::to_string(vk - VK_NUMPAD0);
  for (const auto& k : kKeyNames) {
    if (vk == k.vk) return k.name;
  }
  return "?";
}

// Controller button names -> XInput bits. Triggers are not supported.
uint16_t ButtonFromName(const std::string& raw) {
  const std::string n = Upper(Trim(raw));
  static const struct { const char* name; uint16_t bit; } kNames[] = {
      {"A", 0x1000},  {"B", 0x2000},  {"X", 0x4000},  {"Y", 0x8000},
      {"LB", 0x0100}, {"RB", 0x0200}, {"LS", 0x0040}, {"RS", 0x0080},
      {"START", 0x0010}, {"BACK", 0x0020},
      {"DPAD_UP", 0x0001}, {"DPAD_DOWN", 0x0002}, {"DPAD_LEFT", 0x0004}, {"DPAD_RIGHT", 0x0008},
      {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
  };
  for (const auto& b : kNames) {
    if (n == b.name) return b.bit;
  }
  return 0;
}

bool AsBool(const std::string& v, bool fallback) {
  const std::string s = Upper(Trim(v));
  if (s == "TRUE" || s == "1" || s == "YES" || s == "ON") return true;
  if (s == "FALSE" || s == "0" || s == "NO" || s == "OFF") return false;
  return fallback;
}

// "3, NUMPAD3" -> up to KEYS_PER key codes. An empty value means none; an
// unknown name is skipped (the startup log shows what was read).
void ParseKeyList(const std::string& v, int out[KEYS_PER]) {
  for (int i = 0; i < KEYS_PER; ++i) out[i] = 0;
  int n = 0;
  size_t start = 0;
  while (start <= v.size() && n < KEYS_PER) {
    size_t comma = v.find(',', start);
    if (comma == std::string::npos) comma = v.size();
    if (const int vk = KeyFromName(v.substr(start, comma - start))) out[n++] = vk;
    start = comma + 1;
  }
}

// The [settings] section of mod.ini, read directly: the loader gives native
// mods the folder but (in released builds) no settings accessor. A missing
// file or line keeps the default.
Settings ReadSettings(const char* folder) {
  Settings s;
  for (int c = 0; c < CATEGORIES; ++c) {
    s.keys[c][0] = '1' + c;
    s.keys[c][1] = VK_NUMPAD1 + c;
  }
  if (folder == nullptr) return s;

  std::FILE* f = std::fopen((std::string(folder) + "/mod.ini").c_str(), "rb");
  if (f == nullptr) return s;
  char line[512];
  bool in_settings = false;
  while (std::fgets(line, sizeof line, f) != nullptr) {
    std::string l = Trim(line);
    if (l.empty() || l[0] == ';' || l[0] == '#') continue;
    if (l[0] == '[') {
      in_settings = Upper(l) == "[SETTINGS]";
      continue;
    }
    const size_t eq = l.find('=');
    if (!in_settings || eq == std::string::npos) continue;
    const std::string key = Upper(Trim(l.substr(0, eq)));
    std::string val = l.substr(eq + 1);
    const size_t comment = val.find(';');
    if (comment != std::string::npos) val = val.substr(0, comment);
    val = Trim(val);

    if (key.size() == 5 && key.rfind("KEY_", 0) == 0 && key[4] >= '1' && key[4] <= '8') {
      ParseKeyList(val, s.keys[key[4] - '1']);
    } else if (key == "NEXT_KEY") {
      ParseKeyList(val, s.next_keys);
    } else if (key == "PREV_KEY") {
      ParseKeyList(val, s.prev_keys);
    } else if (key == "PAD_NEXT") {
      s.pad_next = ButtonFromName(val);   // empty = off
    } else if (key == "PAD_PREV") {
      s.pad_prev = ButtonFromName(val);
    } else if (key == "PAD_WHEEL") {
      s.pad_wheel = Upper(val) == "AUTO" ? 0 : ButtonFromName(val);
    } else if (key == "WHEEL_KEY") {
      s.wheel_key = KeyFromName(val);     // empty = off
    } else if (key == "WHEEL") {
      s.wheel = AsBool(val, s.wheel);
    } else if (key == "WHEEL_DIR") {
      const int d = std::atoi(val.c_str());
      if (d != 0) s.wheel_dir = d < 0 ? -1 : 1;   // a sign; 0 would freeze the wheel
    } else if (key == "TAP_SWITCH") {
      s.tap_switch = AsBool(val, s.tap_switch);
    } else if (key == "HOLD_MS") {
      const int ms = std::atoi(val.c_str());
      if (ms >= 50 && ms <= 1000) s.hold_ms = ms;
    } else if (key == "MOVE_WITH_WHEEL") {
      s.move_with_wheel = AsBool(val, s.move_with_wheel);
    } else if (key == "DEBUG_KEY") {
      s.debug_key = KeyFromName(val);     // empty or unknown = off
    }
  }
  std::fclose(f);
  return s;
}

// ---------------------------------------------------------------- state

const WmlApi* g_api = nullptr;
const WmlMod* g_mod = nullptr;
Settings g_set;
bool g_can_take_keys = false;   // the loader offers take_key (newer loaders)

WmlGuestFunction g_real_frame = nullptr;
WmlGuestFunction g_real_input = nullptr;
WmlGuestFunction g_real_axis = nullptr;
using AddWheelFn = void (*)(int delta);
AddWheelFn g_real_add_wheel = nullptr;

// Requests, applied in the frame hook (the only place with a context to call
// equip from). They arrive from three threads: the window's (mouse wheel), the
// game's (controller) and the render thread (keys).
std::atomic<int> g_steps{0};          // + next, - previous
std::atomic<int> g_category{-1};      // number key pressed, -1 = none
std::atomic<bool> g_dump{false};      // debug key pressed

// Where the wheel points while it is open, -32767..32767, from the controller
// hook; read back by the axis hook. Both run on the game's thread.
std::atomic<bool> g_pick_active{false};
std::atomic<int> g_pick_x{0}, g_pick_y{0};

void Log(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (g_api) g_api->log(g_mod, buf);
}

bool PtrOk(uint32_t p) { return p >= GUEST_LO && p < GUEST_HI && (p % 4) == 0; }
uint32_t Rd32(uint32_t a) { return PtrOk(a) ? g_api->read_u32(a) : 0u; }

// ---------------------------------------------------------------- where the player is

// The player, or 0 in multiplayer / before a game is loaded.
uint32_t Player() {
  if (g_api->read_u8(MP_FLAG) != 0) return 0;
  const uint32_t p = g_api->read_u32(PLAYER_PTR);
  if (!PtrOk(p) || g_api->read_u32(p + OFF_TYPE) != 1) return 0;
  return p;
}

bool MenuOrCutscene() {
  for (uint32_t a : MENU_WORDS) {
    if (g_api->read_u32(a) != 0) return true;
  }
  for (uint32_t a : CUTSCENE_FLAGS) {
    if (g_api->read_u8(a) != 0) return true;
  }
  return false;
}

// The game's own "is the player in a vehicle" test, as kbm.cpp does it.
bool InVehicle(uint32_t player) {
  if (g_api->read_u32(player + 3456) != 0) return false;
  const uint32_t handle = g_api->read_u32(player + 2496);
  if (handle == 0 || (g_api->read_u8(player + 2569) & 0x10)) return false;   // getting in / out
  const uint32_t state = g_api->read_u32(player + 508);
  if (state == 9 || state == 12) return false;
  const uint32_t index = handle & 0xFFFF;
  if (index >= 4096) return false;
  const uint32_t object = g_api->read_u32(OBJECT_TABLE + 12 + index * 16);
  if (!PtrOk(object)) return false;
  return g_api->read_u32(object + 68) == handle && g_api->read_u32(object + 72) == 5;
}

// Plain gameplay: a player, no menu, no cutscene. Vehicles count.
uint32_t PlayerInGameplay() {
  const uint32_t p = Player();
  return (p != 0 && !MenuOrCutscene()) ? p : 0;
}

// On foot, not tagging: when the wheel button and the D-pad are free for this
// mod. In a vehicle they keep their normal jobs (the radio, drive-by).
bool OnFoot(uint32_t gameplay_player) {
  return gameplay_player != 0 && !InVehicle(gameplay_player) &&
         g_api->read_u32(gameplay_player + OFF_TAGGING) == 0xFFFFFFFFu;
}

// The weapon wheel is open (the game's own test, sub_8229B898).
bool WheelOpen() {
  const int32_t count = static_cast<int32_t>(g_api->read_u32(UI_ACTIVE + 12));
  const uint32_t list = g_api->read_u32(UI_ACTIVE + 4);
  if (count <= 0 || count > 64 || !PtrOk(list)) return false;
  for (int32_t i = 0; i < count; ++i) {
    if (g_api->read_u32(list + 4 * static_cast<uint32_t>(i)) == UI_WEAPON_MENU) return true;
  }
  return false;
}

// The controller button(s) the game's current layout uses for the weapon menu.
uint16_t LayoutWheelButton() {
  const uint32_t entry = ACTION_TABLE + ACTION_WEAPON_MENU * 16;
  const int32_t b[2] = {static_cast<int32_t>(g_api->read_u32(entry)),
                        static_cast<int32_t>(g_api->read_u32(entry + 4))};
  // Before the game fills the table in, it is all zeros ("A and A").
  if (b[0] == 0 && b[1] == 0) return BTN_B;
  uint16_t bits = 0;
  for (int32_t v : b) {
    if (v >= 0 && v < 16) bits |= kButtonBits[v];
  }
  return bits ? bits : BTN_B;
}

// ---------------------------------------------------------------- the carry list

struct Carried {
  uint32_t nodes[CATEGORIES][MAX_NODES];
  int count[CATEGORIES];
};
Carried g_carried;
int g_cursor[CATEGORIES] = {};   // where a number key resumes cycling
int g_shape = -1;                // per-category counts last frame

bool ReadName(uint32_t addr, char* out) {
  if (!PtrOk(addr)) return false;
  int n = 0;
  for (; n < NAME_MAX - 1; ++n) {
    const uint8_t c = g_api->read_u8(addr + static_cast<uint32_t>(n));
    if (c == 0) break;
    if (c < 32 || c > 126) return false;   // not text, so not a name
    out[n] = static_cast<char>(c);
  }
  out[n] = '\0';
  return n > 0;
}

// The category of a carried node, 0 when its name is not in kWeapons.
int CategoryOf(uint32_t node, char* name_out = nullptr) {
  char name[NAME_MAX];
  if (!ReadName(Rd32(Rd32(node + OFF_N_DEF) + OFF_D_NAME), name)) return 0;
  if (name_out) std::strcpy(name_out, name);
  for (const auto& w : kWeapons) {
    if (std::strcmp(name, w.name) == 0) return w.category;
  }
  return 0;
}

// Rebuilt every time it is used: a pickup or a weapon running out changes the
// list without anything else changing.
void WalkCarryList(uint32_t player) {
  for (int c = 0; c < CATEGORIES; ++c) g_carried.count[c] = 0;
  const uint32_t head = Rd32(player + OFF_LIST);
  uint32_t node = head;
  for (int i = 0; i < MAX_NODES && PtrOk(node); ++i) {
    const int c = CategoryOf(node);
    if (c >= 1) g_carried.nodes[c][g_carried.count[c]++] = node;
    const uint32_t next = Rd32(node + OFF_N_NEXT);
    if (next == 0 || next == head) break;
    node = next;
  }
  // Something picked up or used up renumbers the lists; start cycling afresh.
  int shape = 0;
  for (int c = 1; c < CATEGORIES; ++c) shape = shape * 17 + g_carried.count[c];
  if (shape != g_shape) {
    for (int& cur : g_cursor) cur = 0;
    g_shape = shape;
  }
}

void Equip(WmlContext* ctx, uint32_t player, uint32_t node) {
  g_api->set_r(ctx, 3, player);
  g_api->set_r(ctx, 4, node);   // 0 puts the weapon away: fists
  g_api->set_r(ctx, 5, 0);
  g_api->call(ctx, EQUIP_FN);
}

// One step along fists, then every carried weapon by category. +1 = next.
void Step(WmlContext* ctx, uint32_t player, int dir) {
  WalkCarryList(player);
  // Flatten into the switching order. Position 0 is fists, which has no node.
  uint32_t order[1 + CATEGORIES * MAX_NODES];
  int order_cat[1 + CATEGORIES * MAX_NODES];
  int order_idx[1 + CATEGORIES * MAX_NODES];
  int len = 1;
  order[0] = 0;
  order_cat[0] = 0;
  order_idx[0] = 0;
  for (int c = 1; c < CATEGORIES; ++c) {
    for (int i = 0; i < g_carried.count[c]; ++i, ++len) {
      order[len] = g_carried.nodes[c][i];
      order_cat[len] = c;
      order_idx[len] = i;
    }
  }
  if (len < 2) return;   // fists and nothing else

  // Not finding the weapon in hand is normal right after a pickup; start from fists.
  const uint32_t equipped = g_api->read_u32(player + OFF_EQUIPPED);
  int at = 0;
  for (int i = 1; i < len; ++i) {
    if (order[i] == equipped) at = i;
  }
  const int to = ((at + dir) % len + len) % len;
  if (order_cat[to] != 0) g_cursor[order_cat[to]] = order_idx[to] + 1;
  Equip(ctx, player, order[to]);
}

// A number key: fists, or the next weapon of that category that is not
// already in your hands.
void EquipCategory(WmlContext* ctx, uint32_t player, int category) {
  const uint32_t equipped = g_api->read_u32(player + OFF_EQUIPPED);
  if (category == 0) {
    if (equipped != 0) Equip(ctx, player, 0);
    return;
  }
  WalkCarryList(player);
  const int n = g_carried.count[category];
  for (int t = 0; t < n; ++t) {
    const int i = (g_cursor[category] + t) % n;
    if (g_carried.nodes[category][i] != equipped) {
      g_cursor[category] = i + 1;
      Equip(ctx, player, g_carried.nodes[category][i]);
      return;
    }
  }
}

// The debug key: what you carry and where each weapon sits, to wml.log.
void Dump(uint32_t player) {
  Log("---- carried weapons (category, name, count) ----");
  const uint32_t head = Rd32(player + OFF_LIST);
  uint32_t node = head;
  int shown = 0;
  for (int i = 0; i < MAX_NODES && PtrOk(node); ++i, ++shown) {
    char name[NAME_MAX] = "?";
    const int c = CategoryOf(node, name);
    Log("  %-8s %-22s x%d%s", c ? kCategoryNames[c] : "-", name,
        static_cast<int>(g_api->read_u32(node + OFF_N_COUNT)),
        c ? "" : "   <- not in the mod's weapon table, so it is skipped");
    const uint32_t next = Rd32(node + OFF_N_NEXT);
    if (next == 0 || next == head) break;
    node = next;
  }
  if (shown == 0) Log("  (nothing - pick a weapon up first)");
  Log("weapon wheel button for this controller layout: %04X", LayoutWheelButton());
  Log("---- end ----");
}

// ---------------------------------------------------------------- the controller

// Everything the controller hook decides, as a function of what it is given.
struct PadFilter {
  enum State { kIdle, kPending, kWheel, kPassThrough, kSwallow } state = kIdle;
  uint64_t pressed_at = 0;
  uint16_t last_raw = 0;
};

struct PadEnv {
  bool gameplay = false;     // player, no menu, no cutscene
  bool on_foot = false;      // and on foot, not tagging
  bool wheel_open = false;   // the game's weapon wheel is up
  uint16_t wheel_button = BTN_B;  // the layout's (or mod.ini's) wheel button
  bool kb_wheel = false;     // the keyboard wheel key is held
  bool q_held = false;       // Q is held: Saints Reborn adds B for it on foot
  int wasd_x = 0, wasd_y = 0;  // -1..1 from W A S D
  bool wasd_taken = false;   // W A S D are currently taken from the game
  uint64_t now_ms = 0;
};

struct Pad {
  uint16_t buttons = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

struct PadResult {
  int steps = 0;             // + next, - previous
  bool take_wasd = false;    // W A S D should be taken from the game
  bool pick_active = false;  // the wheel's pointer comes from pick_x / pick_y
};

PadResult FilterPad(PadFilter& f, const Settings& s, const PadEnv& env, Pad& pad,
                    int& pick_x, int& pick_y) {
  PadResult r;
  const uint16_t raw = pad.buttons;
  const uint16_t wb = env.wheel_button;
  const bool down = (raw & wb) != 0 || env.kb_wheel;
  const bool wasd = env.wasd_x != 0 || env.wasd_y != 0;

  switch (f.state) {
    case PadFilter::kIdle:
      if (down) {
        if (s.tap_switch && env.on_foot && !env.wheel_open) {
          f.state = PadFilter::kPending;
          f.pressed_at = env.now_ms;
        } else {
          f.state = PadFilter::kPassThrough;   // not ours: a car, tagging, a menu
        }
      }
      break;
    case PadFilter::kPending:
      if (!down) {
        f.state = PadFilter::kIdle;
        r.steps += 1;                          // a tap: next weapon
      } else if (!env.on_foot) {
        f.state = PadFilter::kSwallow;         // a menu or car arrived mid-press
      } else if (env.now_ms - f.pressed_at >= static_cast<uint64_t>(s.hold_ms)) {
        f.state = PadFilter::kWheel;           // held: let the game open the wheel
      }
      break;
    case PadFilter::kWheel:
    case PadFilter::kPassThrough:
    case PadFilter::kSwallow:
      if (!down) f.state = PadFilter::kIdle;
      break;
  }

  // Saints Reborn turns Q into B on foot. When B is not the wheel button (the
  // third layout uses RB) that B would be something else, so it goes.
  if (env.q_held && env.gameplay && wb != BTN_B &&
      (env.kb_wheel || f.state == PadFilter::kPending)) {
    pad.buttons &= static_cast<uint16_t>(~BTN_B);
  }

  if (f.state == PadFilter::kPending || f.state == PadFilter::kSwallow) {
    pad.buttons &= static_cast<uint16_t>(~wb);
    // While Q is held the port points the wheel with the mouse through the
    // left stick. The wheel is not open yet, so that would walk you towards
    // the mouse; WASD are left alone so running continues through a tap.
    if (f.state == PadFilter::kPending && env.q_held && !wasd) pad.lx = pad.ly = 0;
  } else if (env.kb_wheel && env.gameplay) {
    pad.buttons |= wb;                         // the keyboard key held: the wheel button
  }

  if (env.wheel_open && env.kb_wheel) {
    // The port puts the mouse pointer on the left stick, or WASD when one of
    // them is held. With W A S D taken from the game it is always the mouse.
    const bool stick_is_mouse = !wasd || env.wasd_taken;
    if (s.move_with_wheel) {
      if (stick_is_mouse) {
        pick_x = pad.lx;
        pick_y = pad.ly;
      }                                        // else: keep pointing where it was
      pad.lx = static_cast<int16_t>(env.wasd_x * 32767);
      pad.ly = static_cast<int16_t>(env.wasd_y * 32767);
      r.pick_active = true;
    } else if (!stick_is_mouse) {
      pad.lx = pad.ly = 0;                     // WASD never move the pointer
    }
    r.take_wasd = true;
  } else if (env.wheel_open && s.move_with_wheel) {
    // A controller: the right stick points, the left stick walks, and the
    // camera stays put.
    pick_x = pad.rx;
    pick_y = pad.ry;
    pad.rx = pad.ry = 0;
    r.pick_active = true;
  }

  const uint16_t step_bits = s.pad_next | s.pad_prev;
  if (step_bits && env.on_foot && !env.wheel_open && f.state != PadFilter::kWheel) {
    const uint16_t pressed = raw & static_cast<uint16_t>(~f.last_raw);
    if (pressed & s.pad_next) r.steps += 1;
    if (pressed & s.pad_prev) r.steps -= 1;
    pad.buttons &= static_cast<uint16_t>(~step_bits);
  }
  f.last_raw = raw;
  return r;
}

PadFilter g_pad;
bool g_wasd_taken = false;
uint32_t g_out_packet = 1;
uint8_t g_last_out[12] = {};

uint64_t NowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

void TakeWasd(bool take) {
  if (take == g_wasd_taken || !g_can_take_keys) return;
  for (int vk : {'W', 'A', 'S', 'D'}) g_api->take_key(vk, take ? 1 : 0);
  g_wasd_taken = take;
}

int16_t Rd16s(uint32_t a) { return static_cast<int16_t>(g_api->read_u16(a)); }

void Hook_Input(WmlContext* ctx, uint8_t* base) {
  const uint32_t user = static_cast<uint32_t>(g_api->get_r(ctx, 3));
  const uint32_t state = static_cast<uint32_t>(g_api->get_r(ctx, 4));
  g_real_input(ctx, base);
  if (g_api->get_r(ctx, 3) != 0 || (user & 0xFF) != 0 || !PtrOk(state)) return;

  Pad pad;
  pad.buttons = g_api->read_u16(state + PAD_BUTTONS);
  pad.lx = Rd16s(state + PAD_LX);
  pad.ly = Rd16s(state + PAD_LY);
  pad.rx = Rd16s(state + PAD_RX);
  pad.ry = Rd16s(state + PAD_RY);

  const auto key = [](int vk) { return vk != 0 && g_api->key_down(vk) != 0; };
  PadEnv env;
  const uint32_t player = PlayerInGameplay();
  env.gameplay = player != 0;
  env.on_foot = OnFoot(player);
  env.wheel_open = env.gameplay && WheelOpen();
  env.wheel_button = g_set.pad_wheel ? g_set.pad_wheel : LayoutWheelButton();
  env.kb_wheel = key(g_set.wheel_key);
  env.q_held = key('Q');
  env.wasd_x = (key('D') ? 1 : 0) - (key('A') ? 1 : 0);
  env.wasd_y = (key('W') ? 1 : 0) - (key('S') ? 1 : 0);
  env.wasd_taken = g_wasd_taken;
  env.now_ms = NowMs();

  int pick_x = g_pick_x.load(), pick_y = g_pick_y.load();
  const PadResult r = FilterPad(g_pad, g_set, env, pad, pick_x, pick_y);
  g_pick_x.store(pick_x);
  g_pick_y.store(pick_y);
  g_pick_active.store(r.pick_active);
  TakeWasd(r.take_wasd);
  if (r.steps) g_steps.fetch_add(r.steps);

  g_api->write_u16(state + PAD_BUTTONS, pad.buttons);
  g_api->write_u16(state + PAD_LX, static_cast<uint16_t>(pad.lx));
  g_api->write_u16(state + PAD_LY, static_cast<uint16_t>(pad.ly));
  g_api->write_u16(state + PAD_RX, static_cast<uint16_t>(pad.rx));
  g_api->write_u16(state + PAD_RY, static_cast<uint16_t>(pad.ry));

  // The game skips a state whose packet number has not changed. Ours changes
  // when what the game is given changes, however that came about.
  uint8_t out[12];
  for (uint32_t i = 0; i < 12; ++i) out[i] = g_api->read_u8(state + PAD_BUTTONS + i);
  if (std::memcmp(out, g_last_out, sizeof out) != 0) {
    std::memcpy(g_last_out, out, sizeof out);
    ++g_out_packet;
  }
  g_api->write_u32(state + PAD_PACKET, g_out_packet);
}

// The wheel's pointer axes, while it is open, from the mouse / right stick.
void Hook_Axis(WmlContext* ctx, uint8_t* base) {
  const int axis = static_cast<int>(g_api->get_r(ctx, 3));
  g_real_axis(ctx, base);
  const bool x = axis == AXIS_WHEEL_X || axis == AXIS_CAR_WHEEL_X;
  const bool y = axis == AXIS_WHEEL_Y || axis == AXIS_CAR_WHEEL_Y;
  if (!g_pick_active.load() || (!x && !y)) return;
  const int v = x ? g_pick_x.load() : g_pick_y.load();
  g_api->set_f(ctx, 1, (std::max)(-1.0, (std::min)(1.0, v / 32767.0)));
}

// ---------------------------------------------------------------- per frame

void Hook_Frame(WmlContext* ctx, uint8_t* base) {
  // The game's own code first: skipping it would replace the function.
  if (g_real_frame) g_real_frame(ctx, base);

  // No pause while the wheel is up. Only changed while it is shut, so the
  // pause and unpause around one opening always match.
  if (g_set.move_with_wheel && g_api->read_u8(PAUSE_ON_WHEEL) != 0 && !WheelOpen()) {
    g_api->write_u8(PAUSE_ON_WHEEL, 0);
  }

  const int steps = g_steps.exchange(0);
  const int category = g_category.exchange(-1);
  const bool dump = g_dump.exchange(false);

  if (dump) {
    if (const uint32_t p = Player()) Dump(p);
  }
  const uint32_t player = PlayerInGameplay();
  if (player == 0) return;
  if (category >= 0) {
    EquipCategory(ctx, player, category);
  } else if (steps != 0) {
    Step(ctx, player, steps > 0 ? 1 : -1);   // one per frame, so a flick is not five
  }
}

// Number keys, next / previous keys and the debug key, on the render thread.
void OnFrame(void*) {
  const auto pressed = [](const int* keys) {
    bool hit = false;
    for (int k = 0; k < KEYS_PER && keys[k]; ++k) hit |= g_api->key_pressed(keys[k]) != 0;
    return hit;
  };
  for (int c = 0; c < CATEGORIES; ++c) {
    if (pressed(g_set.keys[c])) g_category.store(c);
  }
  if (pressed(g_set.next_keys)) g_steps.fetch_add(1);
  if (pressed(g_set.prev_keys)) g_steps.fetch_add(-1);
  if (g_set.debug_key && g_api->key_pressed(g_set.debug_key)) g_dump.store(true);
}

// ---------------------------------------------------------------- the mouse wheel

void Hook_AddMouseWheel(int delta) {
  if (delta != 0) g_steps.fetch_add((delta > 0 ? 1 : -1) * g_set.wheel_dir);
  g_real_add_wheel(delta);
}

// sr::AddMouseWheel(int) begins:
//   56 57 48 83 EC 28   push rsi / push rdi / sub rsp,28h
//   85 C9 74 ??         test ecx,ecx / je      (if (!delta) return;)
//   89 CE 48 8D 3D      mov esi,ecx / lea rdi,[rip+...]
const uint8_t* FindAddMouseWheel() {
  static const uint8_t kHead[] = {0x56, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x85, 0xC9, 0x74};
  static const uint8_t kTail[] = {0x89, 0xCE, 0x48, 0x8D, 0x3D};
  constexpr size_t kTailAt = 10, kNeed = kTailAt + sizeof kTail;

  const auto* image = reinterpret_cast<const uint8_t*>(GetModuleHandleW(L"saintsrow.exe"));
  if (image == nullptr) return nullptr;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

  const uint8_t* hit = nullptr;
  int matches = 0;
  const auto* sec = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    if (sec->VirtualAddress >= nt->OptionalHeader.SizeOfImage) continue;
    const size_t size = (std::min)(static_cast<size_t>(sec->Misc.VirtualSize),
                                   static_cast<size_t>(nt->OptionalHeader.SizeOfImage -
                                                       sec->VirtualAddress));
    const uint8_t* p = image + sec->VirtualAddress;
    for (size_t off = 0; off + kNeed <= size; ++off) {
      if (std::memcmp(p + off, kHead, sizeof kHead) == 0 &&
          std::memcmp(p + off + kTailAt, kTail, sizeof kTail) == 0 && ++matches == 1) {
        hit = p + off;
      }
    }
  }
  return matches == 1 ? hit : nullptr;   // never guess between two
}

// Returns why the wheel is off, or nullptr when it is on.
const char* InstallWheel() {
  if (!g_set.wheel) return "wheel = false in mod.ini";
  void* target = const_cast<uint8_t*>(FindAddMouseWheel());
  if (target == nullptr) return "this saintsrow.exe is not one the mod knows";
  const MH_STATUS init = MH_Initialize();
  if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return "MinHook would not start";
  if (MH_CreateHook(target, reinterpret_cast<void*>(&Hook_AddMouseWheel),
                    reinterpret_cast<void**>(&g_real_add_wheel)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    return "the hook would not go in";
  }
  return nullptr;
}

// "3/NUMPAD3", or "off", for the startup log.
std::string KeyNames(const int* keys) {
  std::string out;
  for (int k = 0; k < KEYS_PER && keys[k]; ++k) {
    if (!out.empty()) out += "/";
    out += KeyName(keys[k]);
  }
  return out.empty() ? "off" : out;
}

std::string KeyList() {
  std::string out;
  for (int c = 0; c < CATEGORIES; ++c) {
    out += (c ? "  " : "") + std::to_string(c + 1) + "=" + KeyNames(g_set.keys[c]);
  }
  return out;
}

}  // namespace

extern "C" WML_EXPORT int wml_mod_init(const WmlApi* api, const WmlMod* mod) {
  // Everything used unconditionally is in the original table. take_key is a
  // later addition, used only when the loader has it.
  if (api == nullptr || api->version != WML_API_VERSION ||
      api->size < offsetof(WmlApi, overlay_text)) {
    return 2;
  }
  g_api = api;
  g_mod = mod;
  g_can_take_keys = api->size >= offsetof(WmlApi, take_key) + sizeof(void*) && api->take_key;
  g_set = ReadSettings(mod ? mod->folder : nullptr);

  if (api->hook(HOOK_FRAME, &Hook_Frame, &g_real_frame) != 0) {
    Log("could not hook the player frame function (%08X); the mod is off", HOOK_FRAME);
    return 0;
  }
  if (api->hook(HOOK_INPUT, &Hook_Input, &g_real_input) != 0) {
    Log("could not hook controller input (%08X); tap, D-pad and wheel moving are off", HOOK_INPUT);
  }
  if (api->hook(HOOK_AXIS, &Hook_Axis, &g_real_axis) != 0) {
    Log("could not hook the axis reader (%08X); moving with the wheel open is off", HOOK_AXIS);
    g_set.move_with_wheel = false;
  }
  api->on_frame(&OnFrame, nullptr);

  const char* wheel_off = InstallWheel();
  int wheel_key[KEYS_PER] = {g_set.wheel_key};
  Log("keys: %s  next=%s  prev=%s  wheel=%s", KeyList().c_str(), KeyNames(g_set.next_keys).c_str(),
      KeyNames(g_set.prev_keys).c_str(), KeyNames(wheel_key).c_str());
  Log("mouse wheel %s%s | tap to switch %s (hold %d ms) | move with the wheel open %s",
      wheel_off ? "off: " : (g_set.wheel_dir < 0 ? "on, down = next" : "on, up = next"),
      wheel_off ? wheel_off : "", g_set.tap_switch ? "on" : "off", g_set.hold_ms,
      g_set.move_with_wheel ? "on" : "off");
  return 0;
}
