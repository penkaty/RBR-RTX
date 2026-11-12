// Shared declarations of the RBR-RTX d3d9 proxy.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

// Appends one timestamped line to <RBRRTX_LOG_DIR>\<file>; does nothing when RBRRTX_LOG_DIR is unset.
void rbrrtx_log_to(const wchar_t* file, const wchar_t* fmt, ...);

// d3d9_proxy.cpp: full path of the loaded D3D9 backend (Remix bridge or system d3d9).
const wchar_t* rbrrtx_backend_path();

// report.cpp: per-stage player report, zipped to <logs>\reports\ (key report_mark marks a problem). Per frame: a few
// additions; files and zip on a background thread after the stage.
struct RbrReportFrame { unsigned draws, xformed, ff, btb; float carPos[3]; };
void rbrrtx_report_frame(HWND wnd, const RbrReportFrame& f);   // once per Present
void rbrrtx_report_event(const wchar_t* text);                  // hotkey toggles, sweeps (log_frame reasons)
void rbrrtx_report_log_line(const wchar_t* line);               // every comp log line; warnings go to problems.txt
void rbrrtx_report_sweep(const wchar_t* dir);                   // an Alt+K sweep folder finished during the stage
extern unsigned g_rbrrtxCreates;                                // textures + shaders created (hitch causes)
extern unsigned g_rbrrtxRemixSets;                              // Remix settings sent by rbrrtx_remix_set (menu hitch causes)

// True when the environment variable is set to something other than "0" (default when unset).
bool rbrrtx_env_flag(const wchar_t* name, bool default_value);

// comp.cpp: hooks IDirect3D9::CreateDevice on `d3d9` (an IDirect3D9*) to install the compatibility layer.
void rbrrtx_comp_install(void* d3d9, HMODULE backend);

// harvest.cpp: dev texture harvest (RBRRTX_HARVEST=<folder>); hooks CreateTexture on the device (an IDirect3DDevice9*).
void rbrrtx_harvest_install(void* device);
// harvest.cpp: notes which shader (file name) draws the texture bound to stage 0 (usage.tsv, once per pair).
void rbrrtx_harvest_note_draw(void* device, const wchar_t* shader);
// harvest.cpp: Remix hash (XXH3 of mip 0) of a texture uploaded while the hooks ran, 0 if unknown. Kept for every
// texture even without RBRRTX_HARVEST (hash only, no files; RBRRTX_TEXHASH=0 turns it off) for the stage sweep.
unsigned long long rbrrtx_texture_hash(void* base_texture);
unsigned long long rbrrtx_xxh3(const void* data, size_t size);   // Remix's texture hash = XXH3_64 of mip 0
// Second ground layer (harvest.cpp): a twin of a game texture with its own Remix hash (nullptr = no source held).
void* rbrrtx_layer_twin(void* texture);
void rbrrtx_layer_release_other(int track);
void rbrrtx_layer_enable(bool on);                            // off: release every source and twin, hold no more                   // stage driven: other stages' sources and twins go
void rbrrtx_layer_stats(unsigned* held, unsigned* twins, double* mb);
// harvest.cpp: smallest alpha (0..255) of that texture's mip 0, -1 if unknown (same hooks, alpha_min.hpp).
int rbrrtx_texture_min_alpha(void* base_texture);
// Average colour of a texture's mip 0 (sampled when the game uploaded it), 0..1; false = unknown.
bool rbrrtx_texture_avg_rgb(void* base_texture, float rgb[3]);
// harvest.cpp: writes mip 0 of a 2D texture to a .dds through a READONLY lock (frame dump; through the Remix bridge
// that is the client's copy, the bytes it sent). False if the type or format is not handled or the lock fails.
bool rbrrtx_save_texture_dds(void* base_texture, const wchar_t* path);
bool rbrrtx_dds_is_texture(const wchar_t* path, unsigned long long hash);       // saved DDS (DXT) really holds that texture's mip 0
bool rbrrtx_save_texture_dds_mips(void* base_texture, const wchar_t* path);   // ... the whole mip chain (a replacement file)

// gaugeprobe.cpp: RBRHUD gauge / display probe (dev tool; RBRRTX_GAUGE_PROBE=1 / launcher -GaugeProbe). Every call site
// checks rbrrtx_gauge_probe_on first, so nothing runs when it is off. Log: rbrrtx_gauge_probe.log in the run logs.
extern bool rbrrtx_gauge_probe_on;
void rbrrtx_gauge_probe_init(void* device);                   // patches SetTexture / SetPixelShaderConstantB / CreateRenderTarget
void rbrrtx_gauge_probe_texture(void* tex, unsigned w, unsigned h, unsigned levels, DWORD usage, int format, int pool);   // CreateTexture result
void rbrrtx_gauge_probe_update(void* src, void* dst);         // UpdateTexture(src, dst) succeeded
void rbrrtx_gauge_probe_vs(void* vs, const void* bytecode, const wchar_t* gameName);   // CreateVertexShader result
void rbrrtx_gauge_probe_rt(void* surface);                    // SetRenderTarget(0, surface)
void rbrrtx_gauge_probe_draw(void* vs, bool primaryRT);       // top of every draw
void rbrrtx_gauge_probe_skip(const wchar_t* why);             // the compat layer skipped the draw just announced
void rbrrtx_gauge_probe_event(const wchar_t* text);
void rbrrtx_gauge_probe_mark();                               // stage-report mark key (9): dump everything tracked
void rbrrtx_gauge_probe_frame();                              // each Present

// alphacut.cpp: alpha-card cut-outs (adr-001 tiers 2-3). For the current alpha-tested DrawIndexedPrimitive
// (TRIANGLELIST), our copy of its cards clipped to the stage-0 texture's outline: draw `tris` triangles from
// vb/ib (stream 0 at offset 0 with `stride`, base vertex 0), alpha test off when alphaTestOff. False = draw as is.
struct RbrAlphaCut { void* vb; void* ib; unsigned stride, verts, tris; bool alphaTestOff; };
bool rbrrtx_alphacut_find(void* device, int base, unsigned minIdx, unsigned numV, unsigned startIdx, unsigned prims,
                          RbrAlphaCut* out);
void rbrrtx_alphacut_forget(const void* buffer);              // the game created a buffer at this address
void rbrrtx_alphacut_init();
void rbrrtx_alphacut_frame(unsigned frame);
void rbrrtx_alphacut_summary(wchar_t* out, size_t n, unsigned frames);   // stats since the last call

// radnormals.cpp: radial foliage normals (prototype). For the current fixed-function foliage DrawIndexedPrimitive
// (TRIANGLELIST, stream 0 world-space FLOAT3 positions), a vertex buffer of FLOAT3 normals numbered like stream 0
// (bind with stride 12, offset 0 as a NORMAL stream); nullptr = draw as is. Settings: config\foliage-normals.ini.
void* rbrrtx_radnorm_find(void* device, int base, unsigned minIdx, unsigned numV, unsigned startIdx, unsigned prims);
void rbrrtx_radnorm_forget(const void* buffer);               // the game created a buffer at this address
void rbrrtx_radnorm_load();                                   // (re)reads foliage-normals.ini
bool rbrrtx_radnorm_enabled();
bool rbrrtx_radnorm_toggle();                                 // reloads the ini, flips on/off, returns the new state
void rbrrtx_radnorm_frame(unsigned frame);
void rbrrtx_radnorm_summary(wchar_t* out, size_t n, unsigned frames);

// game_state.cpp: what the player picked, read from RBR's globals (NGPCarMenu RBRAPI.h offsets) and RSF's
// rsfdata\cache\stages_data.json (name, country, surface). -1 / empty = unknown.
struct RbrStageInfo {
  int trackId, carId, tyreType, weatherType, damage;          // RBRMapSettings 0x1660800
  int clouds, wetness, surfaceAge, timeOfDay, skyType;        // RBRMapSettingsEx 0x8938F8
  int gameMode;                                               // 1 driving, 2 pause, 3 menu, 5 loading, 8 replay, 10 pre-stage
  wchar_t name[80];
  char country[8];                                            // ISO code from RSF ("PL"), "" if unknown
  int tarmac, gravel, snow;                                   // surface percent from RSF
};
bool rbrrtx_stage_info(RbrStageInfo* out);
// RBR's car movement object (0x8EF660) +0x100, 20 floats; car position = out[16..18] in physics space (= render world
// with Y and Z swapped). Pre-stage/stage/replay only; false if not readable.
bool rbrrtx_player_car_raw(float out[20]);
bool rbrrtx_player_car_gear(int* gear);                        // 0 reverse, 1 neutral, 2.. forward (0x165FC68 +0x170)
bool rbrrtx_player_car_brake(float* brake);                    // brake input 0..1 (car object +0x860)
const wchar_t* rbrrtx_sky_name(int skyType);                  // Crisp, Hazy, NoRain, LightRain, ...
const wchar_t* rbrrtx_cloud_name(int clouds);
const wchar_t* rbrrtx_time_name(int timeOfDay);
const wchar_t* rbrrtx_wetness_name(int wetness);
// weather.cpp: the stage's TrackSettings section for this weather (its description: Comment + text after the section
// name; its fog) and the weather a description names (any of several languages; -1 = not named).
struct StageWeather {
  bool found = false;
  std::wstring section, description;
  std::wstring descriptionAbove;                               // comment lines right above the section header (<= 2 lines, <= 60 chars)
  bool descAbove = false;                                      // description is that comment (the section has no words of its own)
  bool useFog = true, haveUseFog = false;
  int haveFog = 0;                                             // bit 1 FogStart, bit 2 FogEnd
  float fogStart = 0.0f, fogEnd = 0.0f, fogCol[3] = { -1.0f, -1.0f, -1.0f };
};
struct WeatherText { int sky = -1, clouds = -1, time = -1; bool night = false; };   // time: 0..5, rbrrtx_weather_time_name
bool rbrrtx_stage_weather(const RbrStageInfo& si, StageWeather* out);
float rbrrtx_stage_fog_end();                                  // comp.cpp: this stage's fog end (m), 0 = none
bool rbrrtx_weather_from_text(const std::wstring& text, int gameSky, WeatherText* out);
const wchar_t* rbrrtx_weather_time_name(int t);               // Morning Noon Evening Night Dawn Sunset
// Copies the window's client area from the screen into a PNG (what Remix presented). mean_luma = 0..255 (0 = black:
// exclusive full screen hides the image from GDI).
bool rbrrtx_screenshot(HWND wnd, const wchar_t* png_path, double* mean_luma);

// exception_trace.cpp
void rbrrtx_exception_trace_install();

// remixplus_api.cpp: the Remix API on Remix Plus (API 1.0.0) behind NVIDIA's table layout (third_party/remix_c.h).
// Fills `nvTable` (a remixapi_Interface, `size` bytes); returns the remixapi_ErrorCode (0 = success).
int rbrrtx_remixplus_init(HMODULE backend, void* nvTable, size_t size);
unsigned rbrrtx_remixplus_converted();                         // pNext chains rebuilt for Remix Plus so far
// Remix Plus calls beyond NVIDIA's table (Kim's header types; null = the runtime/bridge lacks it). For the API world.
struct RbrApiExt {
  void *createRetained, *updateRetained, *destroyRetained, *drawRetained, *importD3D9Texture, *createMeshBatched,
       *setupCamera, *createMaterial, *drawInstance, *destroyMesh, *destroyMaterial, *destroyTexture, *setGameValue, *getGameValue;
  unsigned long long version;                                  // the API version the library accepted
};
const RbrApiExt* rbrrtx_api_ext();                              // nullptr before init / without Remix Plus
void rbrrtx_remixplus_frame();                                 // after each Present: a new frame for the light hashes
unsigned rbrrtx_remixplus_renames();                           // light creates renamed (their hash destroyed that frame)
bool rbrrtx_remix_plus();                                      // comp.cpp: the API runs through remixplus_api.cpp

// headlights.cpp: car headlights as Remix API spot lights (headlights key: two lamps -> brights -> off;
// day running lights, brights below brights_below_sun; rbr-rtx\config\headlights.ini). Lamp positions from the car model's nodes.
struct remixapi_Interface;
void rbrrtx_headlights_init(const remixapi_Interface* api);   // nullptr = no Remix API: headlights off
// each car draw on the primary target (row-major W); cockpitGlass = CarGlassInternal.vso (the windscreen seen from the cockpit)
void rbrrtx_headlights_car_draw(const float* world, unsigned prims, bool blended, bool cockpitGlass);
void rbrrtx_headlights_emit();                                 // once per frame at the first BeginScene (with the sun)
void rbrrtx_headlights_refine();                               // before Remix takes the frame's lights (UI trigger, else Present)
void rbrrtx_headlights_frame_end(float sunElevation, bool layerOn);   // per Present: key, automatic switch
void rbrrtx_headlights_destroy();
int rbrrtx_headlights_history_frames();                          // NUMOS froxel history while car lamps are lit (0 = none)
bool rbrrtx_headlights_keep_fallback();                        // lights on and keep_fallback_light: Remix fallback light Always
unsigned long long rbrrtx_headlights_pop_texture();            // Remix hash of the backfire sprite to watch for (0 = none now)
unsigned long long rbrrtx_headlights_halo_texture();            // Remix hash of the lamp halo sprite (light.dds), 0 = leave it
void* rbrrtx_headlights_halo_twin(void* device);               // its thinned twin texture to bind (nullptr = none)
void rbrrtx_headlights_pop_drawn();                            // a draw with that texture: backfire flash
void rbrrtx_headlights_write_lit_mod();                         // before Remix starts: lit lamp textures made emissive (genmod)
int rbrrtx_headlights_game_lamps();                             // the game's lamp switch as last sent: 1 on, 0 off, -1 unknown
int rbrrtx_headlights_on();                                     // 1 while the car's lamps are on for the player (L, automatic), whatever the pulse sends
int rbrrtx_headlights_lamp_end(const float worldPos[3]);        // a point on the car: +1 nearer the head lamps, -1 nearer the rear lamps, 0 unknown
bool rbrrtx_headlights_has_lens();                              // this car's model has lamp glass / lens / cover meshes of its own
bool rbrrtx_headlights_forward(float out[3]);                   // the car's forward direction in the world, false before its first pose
bool rbrrtx_headlights_is_lamp_glass(unsigned numV);            // a draw of this vertex count is one of the model's front lamp glass meshes
bool rbrrtx_headlights_is_aux_glass(unsigned numV);             // the same for the auxiliary lamps' glass
bool rbrrtx_headlights_has_aux_glass();                         // this model's auxiliary lamps have glass of their own
bool rbrrtx_dds_neutral_mask(const wchar_t* src, const wchar_t* dst);   // custom.cpp: an emission mask of a DDS's brightness, near white
bool rbrrtx_headlights_is_aux(unsigned numV);                   // a draw of this vertex count is one of the model's auxiliary lamp meshes
bool rbrrtx_headlights_aux_on();                                // the auxiliary lamps are on now
bool rbrrtx_headlights_is_brake_lens(unsigned numV);            // a draw of this vertex count is one of the model's unlit brake lamp lenses
const wchar_t* rbrrtx_car_model_path();                         // the player's car model (.sgc), empty off a stage
bool rbrrtx_menu_sun_elevation(float* el);                     // menu sun height under NUMOS (comp.cpp); false off the menu
float rbrrtx_sun_shown_elevation();                             // the sun's height as lit now (comp.cpp; debug sun, night hold)
bool rbrrtx_car_body_draw(float m[16]);                         // the matrix the car body was drawn with this frame (headlights.cpp)
float rbrrtx_numos_lux_per_unit();                              // lux per unit of API light irradiance under NUMOS; 0 without
void rbrrtx_headlights_status(wchar_t* out, size_t n);
int rbrrtx_car_body_visible();                                 // 1 outside view (body drawn), 0 cockpit, -1 not known yet
void rbrrtx_headlights_camera(const float camToWorld[16]);     // comp.cpp: this frame's camera (cockpit = camera in the cabin)

// worldlights.cpp: lamps the game only paints (street lamps, the menu's lamp post) as Remix sphere lights from dusk to
// dawn; rbr-rtx\config\lights.ini [lamp.<name>] texture = <Remix hash>.
void rbrrtx_world_lights_init(const remixapi_Interface* api);
bool rbrrtx_world_lights_wanted();                              // any lamp texture listed (else no per-draw work)
bool rbrrtx_world_lights_texture(unsigned long long hash);
// Lamp in an atlas: its TEXCOORD0 rectangles (count, up to max); poleMinHeight > 0 = keep only groups on a pole;
// anchor: 0 top, 1 centre, 2 bottom of each group.
int rbrrtx_world_lights_kinds(unsigned long long hash);          // lamp kinds on this texture (F3: one per emissive that casts light)
int rbrrtx_world_lights_uvs(unsigned long long hash, int kind, float rects[][4], int max, float* poleMinHeight, int* anchor);
void rbrrtx_world_lights_seen(unsigned long long hash, int kind, const float c[3], float r, bool inMenu);   // world bounding sphere
void rbrrtx_world_lights_frame(float sunElevation, const float cam[3], bool layerOn);           // per Present
void rbrrtx_world_lights_reset();                               // menu <-> stage: the last scene's lamps go
void rbrrtx_world_lights_reload();
// rbrhud.cpp: the cockpit windscreen with RBRHUD's F4 opacity / threshold (rbrhud.ini [windscreen]); nullptr = unchanged.
bool rbrrtx_rbrhud_windscreen(float* opacity, float* threshold, bool* regionsOnly);   // RBRHUD F4 windscreen settings, while opacity < 1
// An F3 emissive that casts light (custom.cpp): a lamp kind of its own on its texture; lumens <= 0 removes it. Lamps go and are
// found again from the next draws.
void rbrrtx_world_lights_set_emit(int id, unsigned long long tex, const float rgb[3], float lumens, float cone, float softness,
                                  const float dir[3], int aim, const float aimAt[3], int always, const float offset[3],
                                  const float (*rects)[4] = nullptr, int nrects = 0, int anchor = 1);                              // lights*.ini read again (after the lamp_mark key; lamps go)
float rbrrtx_world_lights_mark_glow();                         // lights.ini [lights] mark_glow: the glow a new lamp mark gets
void rbrrtx_world_lights_startup();                             // before the backend loads, after rbrrtx_genmod_init: glowing lamp materials of earlier runs
// A draw with a marked lamp texture (kind with glow > 0) while the lamps are on: the twin texture to bind (its generated
// material = the texture + an emissive mask on the marked rectangles), else nullptr. baseTexture = what is bound now.
void* rbrrtx_world_lights_glow(void* device, unsigned long long hash, void* baseTexture, bool inMenu);
// apiworld.cpp: static stage draws as retained Remix API instances on Kim's runtime (materials.ini [api_world]).
struct RbrWorldDraw { int base; unsigned minIdx, numV, startIdx, prims; bool sway; };
void rbrrtx_apiworld_init(const RbrApiExt* ext);                // after the API is up (null = unavailable)
bool rbrrtx_apiworld_wants(bool sway);                          // this class is taken now
bool rbrrtx_apiworld_draw(void* device, const RbrWorldDraw& d); // true = its instance draws it: skip the D3D9 draw
bool rbrrtx_apiworld_object(void* device, const RbrWorldDraw& d, const float world[16]);   // drivers: API mesh drawn now with W (row-major); true = skip the D3D9 draw
void rbrrtx_apiworld_replay();                                  // first BeginScene of a frame
void rbrrtx_apiworld_frame_end(const float* viewProj, bool active);   // Present (viewProj null = no world camera)
void rbrrtx_apiworld_reset(const wchar_t* why);                 // menu <-> stage, device released
void rbrrtx_apiworld_forget(const void* buffer);                // the game created a buffer at this address
void rbrrtx_apiworld_written(const void* buffer);               // the game locked a buffer for writing
void rbrrtx_watch_buffer_writes(void* vb, void* ib);           // comp.cpp: report write locks of these buffers' kinds
// hudglow.cpp: RBRHUD gauge displays (s0 render target + s15 emissive mask) glow through Kim's material protocol.
void rbrrtx_hudglow_init(void* device, bool runtimeHasProtocol); // materials.ini [hud_glow]; patches SetTexture / SetPixelShaderConstantB
void rbrrtx_hudglow_car_draw(void* device);                     // car draw: render target layers over the gauges become world-space UI
void* rbrrtx_hudglow_layer_bind(void* device, unsigned frame);  // binds a snapshot of a gauge render target; returns the game's texture to restore
void rbrrtx_hudglow_device_lost();                              // releases the snapshots
float rbrrtx_hudglow_emissive_gain(float sunElevation);         // rtx.emissiveIntensity for the sun's height
float rbrrtx_hudglow_ui_emissive(bool lampsOn);                 // rtx.worldSpaceUiEmissiveIntensity for the lamp state
const char* rbrrtx_hudglow_ui_list();                           // the hashes for rtx.worldSpaceUiTextures once they grew, else null
void rbrrtx_hudglow_before(void* device);                       // around each D3D9 draw sent to Remix
void rbrrtx_hudglow_after(void* device);
void rbrrtx_hudglow_forget();                                   // stage / device change: texture pointers may be reused
void rbrrtx_hudglow_log();                                      // periodic count line
const wchar_t* rbrrtx_apiworld_cycle();                         // key: all -> foliage only -> off

// genmod.cpp: the generated Remix mod <mods>\rbr_rtx_lit\mod.usda (sections "lit", "litparts", "foliage"); Remix reloads
// it when it changes. init before the backend loads; flush writes now; tick (per Present) writes a change at most
// every few seconds.
void rbrrtx_genmod_init(const wchar_t* modsDir);
const wchar_t* rbrrtx_genmod_dir();                            // <mods>\rbr_rtx_lit ("" before init)
void rbrrtx_genmod_set(const char* section, const std::string& materials);
void rbrrtx_genmod_flush(const wchar_t* why);
void rbrrtx_genmod_tick();
std::string rbrrtx_genmod_material(unsigned long long hash, const std::string& comment, const std::string& inputs);
std::string rbrrtx_genmod_asset(const std::wstring& path);     // a file path as a USD asset path (UTF-8, forward slashes)
// litparts.cpp: lit car parts (hot brake discs, lit lamp lenses) glow: learnt per car, drawn with a twin texture whose
// generated material is emissive. Settings: config\materials.ini [lit_parts].
void rbrrtx_litparts_startup();                                // before the backend loads, after rbrrtx_genmod_init
void rbrrtx_litparts_frame(const wchar_t* carFile, int lamps); // per Present
void* rbrrtx_litparts_draw(void* device, int base, unsigned minIdx, unsigned numV, unsigned startIdx, unsigned prims, const float* world, bool glassShader);
bool rbrrtx_litparts_nudge(float out[3]);                       // world offset for the twin just returned, off a coplanar surface
bool rbrrtx_litparts_lamps_done();                              // the lamp signal gave a clean switch for this car: no more learning pulses
void rbrrtx_litparts_status(wchar_t* out, size_t n);
void rbrrtx_litparts_destroy();
// A car's <name>_lit.dds (hash) and its unlit <name>.dds file: with the lamps off that mesh is drawn with the unlit one.
void rbrrtx_litparts_note_lit_atlas(unsigned long long litHash, const wchar_t* unlitFile);
// foliage.cpp: swaying foliage textures get Remix thin-opaque (subsurface) materials. Settings: materials.ini [foliage].
void rbrrtx_foliage_startup();                                 // before the backend loads, after rbrrtx_genmod_init
void rbrrtx_foliage_draw(void* texture);                       // a swaying foliage draw's stage-0 texture
void rbrrtx_foliage_frame(bool inMenu);                        // per Present; new foliage reaches the mod in the menu only
void rbrrtx_foliage_status(wchar_t* out, size_t n);
// comp.cpp: set a Remix option through the API (logged to rbrrtx_api_options.log; dropped from user.conf at the next start
// if listed in d3d9_proxy.cpp proxy_owned_option). False before the API is up.
bool rbrrtx_remix_set(const char* key, const char* value);
// wetness.cpp: rtx.wet.globalWetness from the stage (track Dry/Damp/Wet, the game's rain over time); wetness.ini
void rbrrtx_wetness_frame(bool hold);                         // per Present; hold = a sweep owns the options
float rbrrtx_wetness_now();

// particles.cpp: Remix particles test build (docs/research/particles-remix.md): RBRRTX_EMIT_PROBE logs RBR's
// CEmitter_Emit calls, RBRRTX_PARTICLE_TEST = Remix dust at the wheels (key dust = 7) and the game's dust never spawned.
void rbrrtx_particles_init(const remixapi_Interface* api);    // nullptr = no Remix API: probe only
void rbrrtx_particles_menu_rain(float percent);                // rain in the main menu's weather, percent as the game's intensity; 0 = none
void rbrrtx_particles_emit(bool sweeping);                     // once per frame at the first BeginScene; none during a sweep
void rbrrtx_particles_frame_end();                            // per Present: key, probe report
void rbrrtx_particles_destroy();
// effects.cpp: the camera for the distance cull of the game-emitter rows.
void rbrrtx_particles_camera(const float camToWorld[16]);       // comp.cpp: this frame's camera (distance cull)
bool rbrrtx_particles_active();                               // Remix dust in this run: the dust key toggles it
// Ground lookup for a wheel's new surface (particles.cpp asks, comp.cpp answers during the next draws). The flag is
// false almost always: then the draw hooks do nothing for it.
extern bool rbrrtx_surface_lookup_on;
bool rbrrtx_surface_lookup_point(float out[3]);               // wheel contact point, render space (Y up)
void rbrrtx_surface_lookup_hit(float dh, const float rgb[3], unsigned long long textureHash);
void rbrrtx_particles_toggle();
// particles.cpp rain/snow probe (RBRRTX_RAIN_PROBE=1): game particle draws per texture, rain key cuts the game's rain.
bool rbrrtx_rain_probe_on();
void rbrrtx_rain_note_draw(unsigned long long hash, unsigned prims, bool ff);
void rbrrtx_rain_toggle();
void rbrrtx_particles_raster(bool raster);                   // key 0: raster = the game's own rain/snow, back = Remix's
bool rbrrtx_rain_cutting();                                  // Remix rain/snow replaces the game's (its emitters live)
bool rbrrtx_rain_skip_texture(unsigned long long hash);       // a game rain/snow texture: skip the draw
int rbrrtx_precip_state(float* intensity);                    // 1 rain, 2 snow, 0 none; intensity in percent
// windscreen.cpp: rain drops and snow on the windscreen as Remix API meshes (particles.ini [windscreen]).
void rbrrtx_windscreen_init(const remixapi_Interface* api);
void rbrrtx_windscreen_emit(bool sweeping);                   // once per frame with the particles
void rbrrtx_windscreen_destroy();
void rbrrtx_windscreen_car_drawn(const float M[16]);           // headlights.cpp: this frame's drawn model root (rows x,y,z,pos)
void rbrrtx_windscreen_camera(const float camToWorld[16], float fovY);   // comp.cpp: this frame's camera
void rbrrtx_windscreen_cockpit_drawn(const float M[16]);      // headlights.cpp: the car as drawn from the cockpit, mid-frame
void rbrrtx_windscreen_frame_end();                           // comp.cpp per Present, after headlights: cockpit drops
// headlights.cpp: the car's model root in render space (rows x, y, z, position), head lamp nodes, model forward.
bool rbrrtx_car_model_frame(float M[16], float lf[3], float rf[3], float fwd[3]);
bool rbrrtx_car_object_frame(float M[16]);                     // RBR's car object + model offset, no drift
bool rbrrtx_car_windscreen_glass(float bottom[3], float top[3], float n[3], float* widthBottom, float* widthTop);   // model frame
int rbrrtx_car_windscreen_source();                            // 2 interior glass (i_window_f), 1 e_window_f, 0 none
float rbrrtx_car_internal_fov();                               // [Cam_internal] FOV of the car's ini (radians), 0 unknown
const wchar_t* rbrrtx_car_model_name();                       // model file name without folder/extension (BMW_M3_E30)
bool rbrrtx_camera_in_cabin();                                  // this frame's camera is inside the car's cabin
// The windscreen RBR drew from the cockpit in the frame that just ended (world space; headlights.cpp), and how often
// it was found / not found in cockpit frames.
bool rbrrtx_car_cockpit_quad(float c[4][3]);                   // RBR's cockpit windscreen quad in driving-camera space
bool rbrrtx_cockpit_root(float M[16]);                         // the car as drawn from the cockpit (CarGlassInternal matrix)
void rbrrtx_cockpit_glass_stats(unsigned* found, unsigned* missed);

// keys.cpp: hotkeys from rbr-rtx\config\keys.ini (defaults in keys.cpp). Order = g_actions in keys.cpp.
enum RbrKey {
  RBRKEY_DUMP, RBRKEY_LAYER, RBRKEY_SUN, RBRKEY_FOLIAGE, RBRKEY_DUST, RBRKEY_WINDSCREEN, RBRKEY_SKY,
  RBRKEY_DEBUG_SUN, RBRKEY_SUN_LEFT, RBRKEY_SUN_RIGHT, RBRKEY_SUN_DOWN, RBRKEY_SUN_UP, RBRKEY_EXPERIMENT, RBRKEY_CULLING,
  RBRKEY_SUN_BRIGHTER, RBRKEY_SUN_DIMMER, RBRKEY_FF_SCENERY, RBRKEY_SWEEP, RBRKEY_RAYTRACING, RBRKEY_OPAQUE_ALPHA, RBRKEY_ALPHACUT, RBRKEY_REPORT, RBRKEY_SUN_SAVE, RBRKEY_GROUND_LAYER, RBRKEY_HEADLIGHTS, RBRKEY_FOLIAGE_NORMALS, RBRKEY_RAIN, RBRKEY_BOUNCES, RBRKEY_API_WORLD, RBRKEY_LAMP_MARK, RBRKEY_MENU, RBRKEY_FF_CAR, RBRKEY_COUNT
};
// <env> if set, else <proxy dir>\rbr-rtx\config\<file>.
void rbrrtx_config_path(const wchar_t* env, const wchar_t* file, wchar_t* out, size_t size);
void rbrrtx_lights_marked_path(wchar_t* out, size_t size, bool create);  // lights-marked.ini next to lights.ini
void rbrrtx_keys_load();
// True once per press (call every frame), only while the game window has the focus, Ctrl is up and Alt is held
// only for Alt+ keys.
bool rbrrtx_key_pressed(int action);
// True while held (same conditions).
bool rbrrtx_key_held(int action);
const wchar_t* rbrrtx_key_name(int action);
const wchar_t* rbrrtx_key_label(int action);                   // what the action does (Info page of the F3 menu)
// Windows key code of the action (0 = none). withAlt false: 0 as well for an Alt+ key (the F3 menu reads plain key messages).
int rbrrtx_key_vk(int action, bool withAlt);

// menu/menu.cpp (the F3 menu): hooks pass their calls through while it draws; the wetness driver leaves options alone
// that the user moved in the menu (key such as "rtx.wet.globalWetness"; value = the menu's).
extern "C" int rbrrtx_menu_is_drawing(void);
extern "C" int rbrrtx_menu_option_locked(const char* key, float* value);
extern "C" void rbrrtx_menu_release_stage_locks(void);
// comp.cpp: DLL_PROCESS_DETACH through FreeLibrary: the menu and its window subclass go.
void rbrrtx_comp_shutdown();

// dinput_block.cpp: neutral DirectInput state while the F3 menu is open (vtable patch of GetDeviceState / GetDeviceData;
// RBRRTX_MENU_BLOCK_INPUT=0 leaves it out). Install after the menu exists; remove at exit.
void rbrrtx_dinput_block_install();

// custom.cpp (+ custom.hpp): lights and emissives made in the F3 menu (pages Lights, Windows); menu.ini [lights.<stage>], [emissive], [windows].
void rbrrtx_custom_init(const remixapi_Interface* api);
void rbrrtx_custom_frame(int stageKey, float sunElevation, bool on);               // per Present: the stage's custom lights
bool rbrrtx_emit_on_stage(unsigned long long tex);             // F3 lists: this texture belongs to the loaded stage
bool rbrrtx_texture_on_stage(unsigned long long hash);         // uploaded for the stage being driven (cached for a second)
bool rbrrtx_emit_wanted();                                                         // any emissive texture set (else no per-draw work)
void* rbrrtx_emit_twin(void* device, unsigned long long hash, void* baseTexture, float night);   // twin texture to draw instead, or null
void rbrrtx_custom_ini_set(const char* section, const char* key, const char* value);
int rbrrtx_custom_ini_dump(void (*emit)(const char* line, void* user), void* user);
float rbrrtx_world_lights_night_fraction(float sunElevation);                     // 0..1: how far the street lamps are lit at that sun height
void rbrrtx_dinput_block_remove();
