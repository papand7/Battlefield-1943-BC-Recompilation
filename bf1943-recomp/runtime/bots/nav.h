// The bots' navigation mesh: nav/<map>.nav (tools/navgen), routed with Detour.
// See nav.cpp.
#pragma once
#include <cstdint>
#include <string>

namespace rt::nav {

// Where a soldier last was on the mesh's main piece (the way off an island).
struct Memory {
  float last_main[3] = {};
  bool have = false;
};

bool Ready(const std::string& map);  // loads the map's mesh (once per map); false without one
// Up to max corners (x y z each) of a route from a to b; 0 without a way.
int Route(const float a[3], const float b[3], float* corners, int max, Memory* mem);
// The jeeps' and tanks' mesh (nav/<map>.vnav): loads it (false without one),
// and up to max corners of a vehicle's route from a to b (0: no mesh, no way).
bool VehiclesReady(const std::string& map);
int RouteVehicle(const float a[3], const float b[3], float* corners, int max);
// Learnt trails (<user>/nav/<map>.txt, as Bad Company's): the soldiers' places
// on foot (ids and x y z each) appended, a point a metre; a route along them
// from a to b (up to max corners; 0: no trail near both ends), for when the
// mesh gives none; whether the map has any.
void RecordTrails(const std::string& map, const uint32_t* ids, const float* pos, int count);
int RouteTrails(const std::string& map, const float a[3], const float b[3], float* corners, int max);
bool HaveTrails(const std::string& map);
void Block(const float at[3]);  // a polygon a bot can't get onto: off for routes, remembered
// The main piece: the one most of these places (the flags) are on, not the
// biggest (on an island map that is the sea floor). Once per map.
void ChooseMain(const float* points, int count);
// For the bots' advisor (bot_llm.h): whether a soldier can stand there, and
// up to max places (x y z each) r0 to r1 m around `at`, one in each direction.
bool Walkable(float x, float y, float z);
int Places(const float at[3], float r0, float r1, float* out, int max);

}  // namespace rt::nav
