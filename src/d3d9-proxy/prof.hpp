// prof.hpp - cheap always-on CPU zones for the game thread (user 2026-11-05: "what made the CPU thread jump").
// Each zone adds its wall time (QueryPerformanceCounter, ~25 ns) to a counter; every 300 Presents prof.cpp writes
// rbrrtx_prof.log: ms per frame per zone, the game thread's real CPU time and what is left for the game and its plugins.
// Inclusive: a zone contains the zones and backend calls inside it. RBRRTX_PROF=0 switches the log off.
#pragma once

enum RbrProfZone {
  PZ_DRAW_HOOK,      // whole draw hooks (comp_draw + backend)
  PZ_DRAW_BACKEND,   // ...of which the bridge's Draw* call
  PZ_DRAW_EXTRA,     // ...of which extra draws: decal layer, dirt
  PZ_DRAW_CULL,      // cull_draw
  PZ_DRAW_WORLDLIGHT,// world_light_draw
  PZ_DRAW_API_WORLD, // API world lookup
  PZ_VSCONST,        // SetVertexShaderConstantF hook
  PZ_PSCONST,        // SetPixelShaderConstantF hook
  PZ_RSTATE,         // SetRenderState hook
  PZ_TSS,            // SetTextureStageState hook
  PZ_SHADER,         // SetVertexShader / SetVertexDeclaration / SetFVF hooks
  PZ_XFORM,          // SetTransform hook
  PZ_RT,             // SetRenderTarget hook
  PZ_SCENE,          // BeginScene hook
  PZ_STATEBLOCK,     // CreateStateBlock hook (a blocking bridge round trip)
  PZ_BUFLOCK,        // vertex/index buffer Lock + Unlock hooks
  PZ_END_FRAME,      // end_frame, everything
  PZ_EF_LITPARTS,
  PZ_EF_CARPAINT,
  PZ_EF_FOLIAGE,
  PZ_EF_GENMOD,
  PZ_EF_ATMO,
  PZ_EF_EXP,
  PZ_EF_KEYS,        // the key block of end_frame
  PZ_EF_LAMPS,       // headlights
  PZ_EF_WORLDLIGHTS,
  PZ_EF_CUSTOM,
  PZ_EF_GROUND,      // numos_ground + ground_frame
  PZ_EF_WINDSCREEN,
  PZ_EF_PARTICLES,
  PZ_EF_WETNESS,
  PZ_F3,             // f3_present
  PZ_PRESENT,        // the bridge's Present call
  PZ_F3_PROBE,       // F3 look-at check, per draw, inside draw; never runs with F3 closed (not in the hook total)
  PZ_COUNT
};

void rbrrtx_prof_add(RbrProfZone z, long long ticks);
long long rbrrtx_prof_now();
void rbrrtx_prof_frame();                               // once per Present

struct RbrZone {
  RbrProfZone z;
  long long t0;
  explicit RbrZone(RbrProfZone zone) : z(zone), t0(rbrrtx_prof_now()) {}
  ~RbrZone() { rbrrtx_prof_add(z, rbrrtx_prof_now() - t0); }
  RbrZone(const RbrZone&) = delete;
  RbrZone& operator=(const RbrZone&) = delete;
};
#define RBR_ZONE(z) RbrZone rbrZone_##z(z)
