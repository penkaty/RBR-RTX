// texvariants.cpp - texture variants (see texvariants.hpp).
//
// File (rbr-rtx\config\texture-variants.txt, dev: remix\config): "s <texture> <sig>..." geometry signatures of a
// texture with an entry, "a <variant> <texture>" learnt variants, "n <key>" variant / texture pairs seen together in
// one scene (never variants; the pair's mixed key). Written when something changed, at a scene change or after 30 s.
#include "common.hpp"
#include "texvariants.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr size_t kSigs = 8;          // signatures kept per texture with an entry
constexpr unsigned kTries = 24;      // geometry reads per texture and scene while looking for its original
constexpr unsigned kBudget = 48;     // geometry reads per frame (a stage start reads hundreds of draws)

struct Tex { uint32_t scene = 0; uint8_t tries = 0; uint8_t recorded = 0; };
struct Alias { uint64_t canonical = 0; uint32_t scene = 0; };

std::unordered_map<uint64_t, Tex> g_tex;                                   // every texture drawn: last scene, reads
std::unordered_map<uint64_t, std::vector<uint64_t>> g_sigsOf;              // texture with an entry -> its signatures
std::unordered_map<uint64_t, uint64_t> g_texOfSig;                         // signature -> texture with an entry
std::unordered_map<uint64_t, Alias> g_alias;                               // variant -> texture
std::unordered_set<uint64_t> g_never;                                      // variant ^ texture pairs seen together
bool g_loaded = false, g_dirty = false;
uint32_t g_scene = 1;
int g_sceneKey = -1000;
unsigned g_gen = 1, g_budget = kBudget, g_learnt = 0, g_revoked = 0;
ULONGLONG g_savedAt = 0;
std::wstring g_path;

uint64_t pair_key(uint64_t a, uint64_t b) { return a * 0x9E3779B97F4A7C15ULL ^ b; }

void log(const wchar_t* fmt, ...) {
  wchar_t b[512];
  va_list a;
  va_start(a, fmt);
  _vsnwprintf_s(b, _TRUNCATE, fmt, a);
  va_end(a);
  rbrrtx_log_to(L"rbrrtx_comp.log", L"texture variants: %s", b);
}

void save() {
  if (!g_dirty || g_path.empty()) return;
  g_dirty = false;
  g_savedAt = GetTickCount64();
  std::string out =
      "; RBR-RTX texture variants (texvariants.cpp). Written by the game; delete the file to start over.\r\n"
      "; s <texture> <geometry signatures>: draws of a texture that carries an entry (F3, Alt+X lists)\r\n"
      "; a <variant> <texture>: the same object in another texture set (dry / damp / wet): it takes the texture's entries\r\n"
      "; n <key>: a texture and a candidate drawn in one scene, so never variants\r\n";
  char b[64];
  std::vector<uint64_t> keys;
  for (const auto& kv : g_sigsOf) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  for (uint64_t t : keys) {
    snprintf(b, sizeof(b), "s %016llX", static_cast<unsigned long long>(t));
    out += b;
    for (uint64_t s : g_sigsOf[t]) { snprintf(b, sizeof(b), " %016llX", static_cast<unsigned long long>(s)); out += b; }
    out += "\r\n";
  }
  keys.clear();
  for (const auto& kv : g_alias) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  for (uint64_t v : keys) {
    snprintf(b, sizeof(b), "a %016llX %016llX\r\n", static_cast<unsigned long long>(v), static_cast<unsigned long long>(g_alias[v].canonical));
    out += b;
  }
  for (uint64_t k : g_never) {                               // kept as the pair's mixed key only
    snprintf(b, sizeof(b), "n %016llX\r\n", static_cast<unsigned long long>(k));
    out += b;
  }
  const std::wstring tmp = g_path + L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { log(L"could not write %s", tmp.c_str()); return; }
    f << out;
  }
  if (!MoveFileExW(tmp.c_str(), g_path.c_str(), MOVEFILE_REPLACE_EXISTING)) log(L"could not replace %s", g_path.c_str());
}

void add_sig(uint64_t tex, uint64_t sig) {
  auto& v = g_sigsOf[tex];
  if (v.size() >= kSigs || std::find(v.begin(), v.end(), sig) != v.end()) return;
  const auto other = g_texOfSig.find(sig);
  if (other != g_texOfSig.end() && other->second != tex) return;   // one geometry, two textures with entries: keep the first
  v.push_back(sig);
  g_texOfSig[sig] = tex;
  g_dirty = true;
}

void revoke(uint64_t variant, const wchar_t* why) {
  const auto it = g_alias.find(variant);
  if (it == g_alias.end()) return;
  g_never.insert(pair_key(variant, it->second.canonical));
  log(L"%016llX is no variant of %016llX any more (%s)", static_cast<unsigned long long>(variant),
      static_cast<unsigned long long>(it->second.canonical), why);
  g_alias.erase(it);
  ++g_gen;
  ++g_revoked;
  g_dirty = true;
}

}  // namespace

void rbrrtx_texvar_load() {
  if (g_loaded) return;
  g_loaded = true;
  wchar_t p[MAX_PATH * 2];
  rbrrtx_config_path(L"RBRRTX_TEXVARIANTS", L"texture-variants.txt", p, MAX_PATH * 2);
  g_path = p;
  std::ifstream in(g_path, std::ios::binary);
  size_t sigs = 0;
  for (std::string l; std::getline(in, l);) {
    if (l.empty() || l[0] == ';') continue;
    std::istringstream s(l);
    std::string kind, a, b;
    s >> kind >> a;
    if (a.empty()) continue;
    const uint64_t x = std::strtoull(a.c_str(), nullptr, 16);
    if (kind == "s") {
      while (s >> b) { add_sig(x, std::strtoull(b.c_str(), nullptr, 16)); ++sigs; }
    } else if (kind == "a" && (s >> b)) {
      g_alias[x] = { std::strtoull(b.c_str(), nullptr, 16), 0 };
    } else if (kind == "n") {
      g_never.insert(x);
    }
  }
  g_dirty = false;
  log(L"%s: %zu textures with %zu geometry signatures, %zu variants, %zu pairs that are none", g_path.c_str(), g_sigsOf.size(),
      sigs, g_alias.size(), g_never.size());
}

void rbrrtx_texvar_frame(int sceneKey) {
  if (!g_loaded) rbrrtx_texvar_load();
  g_budget = kBudget;
  if (sceneKey != g_sceneKey) {
    if (g_sceneKey != -1000 && (g_learnt || g_revoked))
      log(L"scene %d left: %u variants learnt, %u taken back; %zu known", g_sceneKey, g_learnt, g_revoked, g_alias.size());
    g_sceneKey = sceneKey;
    ++g_scene;
    g_learnt = g_revoked = 0;
    save();
  } else if (g_dirty && GetTickCount64() - g_savedAt > 30000) {
    save();
  }
}

uint64_t rbrrtx_texvar_canonical(uint64_t h) {
  const auto it = g_alias.find(h);
  return it == g_alias.end() ? h : it->second.canonical;
}

void rbrrtx_texvar_draw(uint64_t h, bool watched, uint64_t (*sig)(void*), void* ctx) {
  if (!h) return;
  Tex& t = g_tex[h];
  if (t.scene != g_scene) {                                  // first draw of h in this scene
    t.scene = g_scene;
    t.tries = 0;
    t.recorded = 0;
    // A texture and its variant are never drawn in one scene: both here means two textures of one object.
    const auto a = g_alias.find(h);
    if (a != g_alias.end()) {
      const auto c = g_tex.find(a->second.canonical);
      if (c != g_tex.end() && c->second.scene == g_scene) revoke(h, L"both drawn in one scene");
    }
    if (watched) {
      std::vector<uint64_t> drawnVariants;
      for (const auto& kv : g_alias)
        if (kv.second.canonical == h) {
          const auto v = g_tex.find(kv.first);
          if (v != g_tex.end() && v->second.scene == g_scene) drawnVariants.push_back(kv.first);
        }
      for (uint64_t v : drawnVariants) revoke(v, L"both drawn in one scene");
    }
  }
  if (watched) {
    if (g_alias.count(h)) revoke(h, L"it has entries of its own now");
    if (t.recorded >= kSigs || g_budget == 0) return;
    const auto have = g_sigsOf.find(h);
    if (have != g_sigsOf.end() && have->second.size() >= kSigs) return;
    --g_budget;
    ++t.recorded;
    if (const uint64_t s = sig(ctx)) add_sig(h, s);
    return;
  }
  if (g_texOfSig.empty() || g_alias.count(h) || t.tries >= kTries || g_budget == 0) return;
  --g_budget;
  ++t.tries;
  const uint64_t s = sig(ctx);
  if (!s) return;
  const auto it = g_texOfSig.find(s);
  if (it == g_texOfSig.end() || it->second == h) return;
  const uint64_t c = it->second;
  if (g_never.count(pair_key(h, c))) { t.tries = kTries; return; }
  const auto ct = g_tex.find(c);
  if (ct != g_tex.end() && ct->second.scene == g_scene) {    // drawn together: a second texture of that object
    g_never.insert(pair_key(h, c));
    g_dirty = true;
    t.tries = kTries;
    return;
  }
  g_alias[h] = { c, g_scene };
  ++g_gen;
  ++g_learnt;
  g_dirty = true;
  t.tries = kTries;
  log(L"%016llX is a variant of %016llX (same geometry %016llX; scene %d): it takes that texture's entries",
      static_cast<unsigned long long>(h), static_cast<unsigned long long>(c), static_cast<unsigned long long>(s), g_sceneKey);
}

unsigned rbrrtx_texvar_generation() { return g_gen; }

void rbrrtx_texvar_aliases(std::vector<std::pair<uint64_t, uint64_t>>* out) {
  out->clear();
  for (const auto& kv : g_alias) out->push_back({ kv.first, kv.second.canonical });
}

std::wstring rbrrtx_texvar_status() {
  wchar_t b[200];
  swprintf_s(b, L"%zu textures with entries hold %zu geometry signatures; %zu variants known", g_sigsOf.size(), g_texOfSig.size(), g_alias.size());
  return b;
}
