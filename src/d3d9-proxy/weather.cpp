// weather.cpp - the stage's own weather section: its description and its fog (user 2026-10-24).
//
// RBR picks a stage's look from Maps\<id>...\TrackSettings<id>.ini, section [<id><T>_<sky>_<clouds>] or
// [<id>O_<T>_<sky>_<clouds>] (T = M/N/E). Many stage makers describe what the section really shows: a Comment line
// ("Rainy night", "Noche Despejada", "Poca Lluvia", "PINK DAWN", "CALIMA") or text after the section name
// ("[131O_E_lightfog_heavycloud] ; Night"). The user: when a description names a time of day or weather, in English or
// another language, it wins over the three-part name (the game's sky type often says HeavyCloud where the stage was
// lit for night or fog). The section also holds the game's own fog (UseFog, FogStart, FogEnd, FogRed/Green/Blue): how
// far one sees in that weather on the original raster image, which comp.cpp's atmosphere uses for rain, snow and fog.
//
// rbrrtx_weather_from_text reads words, not phrases: CamelCase split ("HeavyCloud LightSnow"), accents folded, a word
// matches a stem when it starts with it (stems of 3 letters or less must match the whole word). "heavy"/"light"
// before a cloud word set the cloud cover, before rain/snow/fog the strength; "no"/"sin"/"bez"/"kein"/"sans"/"senza"
// before rain/snow make it dry. Precipitation and fog win over haze, haze over cloud words, cloud words over clear.
#include "common.hpp"
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Lower case, accents dropped (é -> e, ñ -> n, ł -> l), CamelCase split, then words of letters only.
std::vector<std::wstring> words_of(const std::wstring& text) {
  std::wstring split;
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t c = text[i];
    if (i > 0 && iswupper(c) && iswlower(text[i - 1])) split += L' ';
    split += c;
  }
  wchar_t folded[1024] = {};
  const int n = FoldStringW(MAP_COMPOSITE, split.c_str(), static_cast<int>((std::min)(split.size(), static_cast<size_t>(400))),
                            folded, 1023);
  std::wstring s = n > 0 ? std::wstring(folded, n) : split;
  std::vector<std::wstring> out;
  std::wstring w;
  for (wchar_t c : s) {
    if (c >= 0x300 && c <= 0x36F) continue;            // combining accents left by MAP_COMPOSITE
    if (c == 0x142 || c == 0x141) c = L'l';            // ł has no decomposition
    if (iswalpha(c)) { w += static_cast<wchar_t>(towlower(c)); continue; }
    if (!w.empty()) { out.push_back(w); w.clear(); }
  }
  if (!w.empty()) out.push_back(w);
  return out;
}

bool is(const std::wstring& w, std::initializer_list<const wchar_t*> stems) {
  for (const wchar_t* s : stems) {
    const size_t l = wcslen(s);
    if (l <= 3 ? w == s : w.compare(0, l, s) == 0) return true;
  }
  return false;
}

enum Kind { kNone, kRain, kSnow, kFog, kHaze, kCloud, kClear };

Kind kind_of(const std::wstring& w) {
  if (is(w, { L"snow", L"nieve", L"nevad", L"nevis", L"neige", L"neve", L"schnee", L"snieg", L"snih", L"snez", L"lumi",
              L"snjeg", L"blizzard", L"ventisca" }))
    return kSnow;
  if (is(w, { L"rain", L"lluvi", L"llov", L"pluie", L"pluv", L"pioggi", L"piov", L"regen", L"deszcz", L"dest", L"sade",
              L"storm", L"torment", L"burza", L"gewitter", L"orage", L"temporal", L"aguacero", L"chuva", L"drizzle",
              L"shower", L"downpour" }))
    return kRain;
  if (is(w, { L"fog", L"foggy", L"mist", L"misty", L"niebla", L"neblin", L"brum", L"brouillard", L"nebbia", L"nebel",
              L"mgla", L"mlha", L"sumu", L"nevoeiro" }))
    return kFog;
  if (is(w, { L"haze", L"hazy", L"calima", L"dust", L"dusty", L"polvo", L"smog", L"dunst" })) return kHaze;
  if (is(w, { L"cloud", L"overcast", L"nubl", L"nubos", L"nuage", L"couvert", L"nuvol", L"bewolk", L"wolkig",
              L"pochmurn", L"zachmur", L"zatazen", L"oblac", L"pilvi", L"grey", L"gray", L"gris" }))
    return kCloud;
  if (is(w, { L"clear", L"sunny", L"despejad", L"soleado", L"sereno", L"sonnig", L"soleil", L"ensoleill", L"sloneczn",
              L"jasn", L"selkea", L"aurinko", L"cloudless", L"crisp",
              L"beau", L"good", L"fine" }))
    return kClear;
  return kNone;
}

int time_of(const std::wstring& w) {                   // 0 Morning, 1 Noon, 2 Evening, 3 Night, 4 Dawn, 5 Sunset
  if (is(w, { L"night", L"noche", L"nocturn", L"nuit", L"notte", L"nacht", L"noc", L"nocn", L"dark", L"oscur",
              L"starry", L"estrell", L"midnight", L"yo" }))
    return 3;
  if (is(w, { L"dawn", L"amanecer", L"alba", L"aube", L"sunrise", L"daybreak", L"aurora", L"morgengrauen", L"svitani",
              L"swit", L"madrugada" }))
    return 4;
  if (is(w, { L"sunset", L"dusk", L"atardecer", L"ocaso", L"crepusc", L"tramonto", L"twilight", L"zmierzch",
              L"zachod", L"soumrak", L"golden", L"abenddammerung", L"hamara" }))
    return 5;
  if (is(w, { L"morning", L"manana", L"manhana", L"matin", L"mattin", L"morgen", L"rano", L"aamu", L"dopoledne", L"ochtend" }))
    return 0;
  if (is(w, { L"noon", L"midday", L"mediodia", L"midi", L"mezzogiorno", L"mittag", L"poludnie", L"poledne",
              L"keskipaiva", L"afternoon" }))
    return 1;
  if (is(w, { L"evening", L"tarde", L"soir", L"soiree", L"sera", L"abend", L"wieczor", L"vecer", L"ilta", L"avond" }))
    return 2;
  return -1;
}

int strength_of(const std::wstring& w) {               // 2 heavy, 1 light, 0 neither
  if (is(w, { L"heavy", L"strong", L"fuerte", L"intens", L"tempest", L"ulewa", L"silny", L"stark", L"forte", L"rankka",
              L"kova", L"mucha", L"mucho", L"much", L"torrential", L"diluvio", L"thick", L"dense", L"denso", L"densa", L"dicht", L"gesta", L"fitto" }))
    return 2;
  if (is(w, { L"light", L"poca", L"poco", L"debil", L"devil", L"leve", L"ligera", L"ligero", L"leicht", L"lekk", L"slab",
              L"leggera", L"leggero", L"heikko", L"some", L"scattered", L"flurr", L"partly", L"parcial", L"part",
              L"faible", L"little" }))
    return 1;
  return 0;
}

bool negation(const std::wstring& w) { return is(w, { L"no", L"sin", L"bez", L"kein", L"keine", L"sans", L"senza", L"without", L"dry", L"seco" }); }

}  // namespace

// Sky (rbrrtx_sky_name index), clouds (rbrrtx_cloud_name index) and time (0..5, rbrrtx_weather_time_name) named by a
// stage's weather description; -1 = not named. gameSky keeps the game's strength when the text names rain/snow/fog
// without one. False when the text names nothing.
bool rbrrtx_weather_from_text(const std::wstring& text, int gameSky, WeatherText* out) {
  *out = WeatherText();
  const std::vector<std::wstring> w = words_of(text);
  Kind precip = kNone;
  int precipStrength = 0, cloudStrength = 0;
  bool haze = false, cloud = false, clear = false, dry = false;
  for (size_t i = 0; i < w.size(); ++i) {
    int t = i + 1 < w.size() ? time_of(w[i] + w[i + 1]) : -1;   // "MedioDia" was split into "medio dia"
    if (t < 0) t = time_of(w[i]);
    if (t >= 0 && out->time < 0) out->time = t;
    const Kind k = kind_of(w[i]);
    const int sPrev = i > 0 ? strength_of(w[i - 1]) : 0;
    const bool negPrev = (i > 0 && negation(w[i - 1])) || (i > 1 && negation(w[i - 2]) && strength_of(w[i - 1]));
    // "Peklo Snow", "Uhorna snow II": a stage name (snow surface), not snowfall.
    if (k == kSnow && i == 1 && kind_of(w[0]) == kNone && time_of(w[0]) < 0 && !strength_of(w[0]) && !negation(w[0])) continue;
    if (k == kRain || k == kSnow || k == kFog) {
      if (negPrev && k != kFog) { dry = true; if (precip == kNone) precip = k; continue; }
      if (precip == kNone || precip == kFog || dry) {   // "heavy fog & snowing": the fog's strength is not the snow's
        if (precip != k) precipStrength = 0;
        precip = k;
        dry = false;
      }
      if (precip == k) {
        int s = sPrev;
        for (size_t j = i + 1; j < w.size() && j <= i + 1 && !s; ++j) s = strength_of(w[j]);   // "lluvia fuerte"
        if (s) precipStrength = s;
        if (is(w[i], { L"storm", L"torment", L"burza", L"gewitter", L"orage", L"temporal", L"aguacero", L"downpour",
                       L"blizzard", L"ventisca" }))
          precipStrength = 2;
        if (is(w[i], { L"drizzle", L"llov", L"shower", L"flurr" })) precipStrength = 1;
      }
    } else if (k == kHaze) {
      haze = true;
    } else if (k == kCloud) {
      cloud = true;                                    // PartCloud 1, LightCloud 2, HeavyCloud 3 (no word: 3)
      if (i > 0 && is(w[i - 1], { L"part", L"partly", L"parcial", L"some", L"scattered", L"algo", L"teilweise" })) cloudStrength = 1;
      else if (sPrev == 1) cloudStrength = 2;
      else if (sPrev == 2) cloudStrength = 3;
      if (negPrev) { cloud = false; clear = true; }    // "sin nubes"
    } else if (k == kClear) {
      clear = true;
    }
  }
  const bool gameRain = gameSky == 3 || gameSky == 4, gameSnow = gameSky == 6 || gameSky == 7, gameFog = gameSky == 8 || gameSky == 9;
  if (precip == kRain) out->sky = dry ? 2 : precipStrength == 2 ? 4 : precipStrength == 1 ? 3 : gameRain ? gameSky : 3;
  else if (precip == kSnow) out->sky = dry ? 5 : precipStrength == 2 ? 7 : precipStrength == 1 ? 6 : gameSnow ? gameSky : 6;
  else if (precip == kFog) out->sky = precipStrength == 2 ? 9 : precipStrength == 1 ? 8 : gameFog ? gameSky : 8;
  else if (haze) out->sky = 1;
  else if (cloud) out->sky = gameSky >= 2 ? gameSky : 2;   // overcast: dry overcast unless the game says more
  else if (clear) out->sky = 0;
  if (cloud) out->clouds = cloudStrength ? cloudStrength : 3;
  else if (clear && precip == kNone && !haze) out->clouds = 0;
  else if (precip == kRain || precip == kSnow) out->clouds = 3;
  out->night = out->time == 3;
  return out->sky >= 0 || out->clouds >= 0 || out->time >= 0;
}

const wchar_t* rbrrtx_weather_time_name(int t) {
  static const wchar_t* n[] = { L"Morning", L"Noon", L"Evening", L"Night", L"Dawn", L"Sunset" };
  return t >= 0 && t < 6 ? n[t] : L"?";
}

// The stage's TrackSettings section for this time of day and weather: its description (Comment + the text after the
// section name) and fog. Both section forms are tried: <id><T>_<sky>_<clouds> and <id>O_<T>_<sky>_<clouds>.
bool rbrrtx_stage_weather(const RbrStageInfo& si, StageWeather* out) {
  *out = StageWeather();
  wchar_t exe[MAX_PATH] = {};
  DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (n == 0 || n >= MAX_PATH || si.timeOfDay < 0 || si.timeOfDay > 2) return false;
  const fs::path maps = fs::path(exe).parent_path() / L"Maps";
  const std::wstring id = std::to_wstring(si.trackId);
  fs::path file;
  std::error_code ec;
  for (auto it = fs::directory_iterator(maps, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const std::wstring dir = it->path().filename().wstring();
    if (dir.compare(0, id.size(), id) || (dir.size() > id.size() && iswdigit(dir[id.size()]))) continue;
    const fs::path f = it->path() / (L"TrackSettings" + id + L".ini");
    if (fs::exists(f, ec)) { file = f; break; }
  }
  if (file.empty()) return false;
  const wchar_t tod = L"MNE"[si.timeOfDay];
  std::wstring sky = rbrrtx_sky_name(si.skyType), clouds = rbrrtx_cloud_name(si.clouds);
  for (auto& c : sky) c = towlower(c);
  for (auto& c : clouds) c = towlower(c);
  const std::wstring want[2] = { id + tod + L"_" + sky + L"_" + clouds, id + L"O_" + tod + L"_" + sky + L"_" + clouds };
  std::ifstream in(file, std::ios::binary);
  const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  // UTF-8 when it decodes as such, else the system code page (Spanish/Polish stage makers use both).
  auto widen = [](const std::string& s) {
    if (s.empty()) return std::wstring();
    for (UINT cp : { static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP) }) {
      const int k = MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
      if (k <= 0) continue;
      std::wstring w(k, 0);
      MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, s.data(), static_cast<int>(s.size()), w.data(), k);
      return w;
    }
    return std::wstring();
  };
  const std::wstring text = widen(all);
  bool inWanted = false;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t e = text.find(L'\n', pos);
    if (e == std::wstring::npos) e = text.size();
    std::wstring l = text.substr(pos, e - pos);
    pos = e + 1;
    if (!l.empty() && l.back() == L'\r') l.pop_back();
    const size_t b = l.find_first_not_of(L" \t\xFEFF");
    if (b == std::wstring::npos) continue;
    if (l[b] == L'[') {
      if (inWanted) break;                             // the wanted section ended
      const size_t c = l.find(L']', b);
      const std::wstring name = l.substr(b + 1, c == std::wstring::npos ? std::wstring::npos : c - b - 1);
      inWanted = !_wcsicmp(name.c_str(), want[0].c_str()) || !_wcsicmp(name.c_str(), want[1].c_str());
      if (!inWanted) continue;
      out->found = true;
      out->section = name;
      if (c != std::wstring::npos) {                   // "[131O_E_lightfog_heavycloud] ; Night"
        const size_t t = l.find_first_not_of(L" \t;/#", c + 1);
        if (t != std::wstring::npos) out->description = l.substr(t);
      }
      continue;
    }
    if (!inWanted || l[b] == L';' || l[b] == L'#') continue;
    const size_t eq = l.find(L'=', b);
    if (eq == std::wstring::npos) continue;
    std::wstring key = l.substr(b, eq - b);
    while (!key.empty() && (key.back() == L' ' || key.back() == L'\t')) key.pop_back();
    std::wstring val = l.substr(eq + 1);
    const size_t v0 = val.find_first_not_of(L" \t");
    val = v0 == std::wstring::npos ? L"" : val.substr(v0);
    while (!val.empty() && (val.back() == L' ' || val.back() == L'\t')) val.pop_back();
    const float f = static_cast<float>(_wtof(val.c_str()));
    if (!_wcsicmp(key.c_str(), L"Comment")) {
      if (!val.empty()) out->description = out->description.empty() ? val : val + L" / " + out->description;
    } else if (!_wcsicmp(key.c_str(), L"UseFog")) {
      out->useFog = !_wcsicmp(val.c_str(), L"true") || val == L"1";
      out->haveUseFog = true;
    } else if (!_wcsicmp(key.c_str(), L"FogStart")) { out->fogStart = f; out->haveFog |= 1; }
    else if (!_wcsicmp(key.c_str(), L"FogEnd")) { out->fogEnd = f; out->haveFog |= 2; }
    else if (!_wcsicmp(key.c_str(), L"FogRed")) out->fogCol[0] = f;
    else if (!_wcsicmp(key.c_str(), L"FogGreen")) out->fogCol[1] = f;
    else if (!_wcsicmp(key.c_str(), L"FogBlue")) out->fogCol[2] = f;
  }
  return out->found;
}
