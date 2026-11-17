// The bots' navigation mesh (from Bad Company's bots, bfbc1-recomp/runtime/
// bc_bots.cpp): nav/<map>.nav made by tools/navgen from the level's terrain and
// collision ("BFNV", version 2: dtNavMeshParams, the tile count, then each
// tile's x, y, size, Detour data), routed with Detour. Looked for next to the
// exe, then in the working folder; made on this PC by tools/make_navs.py (the
// launcher, or a hosting server without one: nav_autogen.h). Polygons a bot couldn't get onto are off
// for a minute (as Bad Company's: a soldier in the way looks the same, and
// blocks kept for good cut routes off); BF_BOT_NAV_KEEP_BLOCKS=1 keeps them in
// <user>/nav/<map>.blocked for later matches, as before.
#include "bots/nav.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "kernel.h"  // (before windows.h, as the rest of the runtime)
#include "nav_autogen.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "DetourNavMesh.h"
#include "DetourCommon.h"
#include "DetourNavMeshQuery.h"

namespace rt::nav {
namespace {

// Paths under the user folder: opened by their wide name on Windows and
// logged in UTF-8 (path::string() throws for a folder name outside the ANSI
// code page there).
FILE* Open(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
  std::wstring wmode(mode, mode + std::strlen(mode));
  return _wfopen(p.c_str(), wmode.c_str());
#else
  return fopen(p.c_str(), mode);
#endif
}
std::string Utf8(const std::filesystem::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

std::mutex g_mutex;  // Detour's query isn't thread safe; routes come from the director's thread
dtNavMesh* g_navmesh = nullptr;
dtNavMeshQuery* g_navquery = nullptr;
std::string g_map;  // the mesh loaded (or tried) for
std::filesystem::path g_blocked_path;
// Blocks: when they open again, and their flags before.
std::map<dtPolyRef, std::pair<std::chrono::steady_clock::time_point, unsigned short>> g_blocked_until;

// The mesh's joined pieces (polygons reached through their links). Most of a
// map is one; the rest are islands Recast leaves on roofs, ledges and the like.
// Routes run on the main piece.
std::unordered_map<dtPolyRef, int> g_piece;
int g_main_piece = -1;
bool g_main_chosen = false;  // from the flags (ChooseMain)

void FindPieces() {
  g_piece.clear();
  std::vector<int> sizes;
  int n = 0;
  for (int ti = 0; ti < g_navmesh->getMaxTiles(); ++ti) {
    const dtMeshTile* tile = static_cast<const dtNavMesh*>(g_navmesh)->getTile(ti);
    if (!tile || !tile->header) continue;
    dtPolyRef base = g_navmesh->getPolyRefBase(tile);
    for (int pi = 0; pi < tile->header->polyCount; ++pi) {
      ++n;
      dtPolyRef start = base | dtPolyRef(pi);
      if (g_piece.count(start)) continue;
      int id = int(sizes.size());
      sizes.push_back(0);
      std::vector<dtPolyRef> stack{start};
      g_piece[start] = id;
      while (!stack.empty()) {
        dtPolyRef r = stack.back();
        stack.pop_back();
        ++sizes[size_t(id)];
        const dtMeshTile* t = nullptr;
        const dtPoly* poly = nullptr;
        if (dtStatusFailed(g_navmesh->getTileAndPolyByRef(r, &t, &poly))) continue;
        for (unsigned l = poly->firstLink; l != DT_NULL_LINK; l = t->links[l].next) {
          dtPolyRef o = t->links[l].ref;
          if (o && !g_piece.count(o)) g_piece[o] = id, stack.push_back(o);
        }
      }
    }
  }
  if (sizes.empty()) return;
  g_main_piece = int(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
  RT_INFO("bot nav: %d polygons in %zu pieces, the main one %d", n, sizes.size(), sizes[size_t(g_main_piece)]);
}
bool InMainPiece(dtPolyRef r) {
  auto it = g_piece.find(r);
  return it != g_piece.end() && it->second == g_main_piece;
}
// The main piece's nearest place to p within the box ext (half sizes).
bool NearestMain(const float p[3], const float ext[3], const dtQueryFilter& filter, dtPolyRef* ref, float out[3]) {
  static dtPolyRef polys[2048];
  int n = 0;
  g_navquery->queryPolygons(p, ext, &filter, polys, &n, 2048);
  float best = 1e30f;
  *ref = 0;
  for (int i = 0; i < n; ++i) {
    if (!InMainPiece(polys[i])) continue;
    float c[3];
    if (dtStatusFailed(g_navquery->closestPointOnPoly(polys[i], p, c, nullptr))) continue;
    float d = dtVdistSqr(c, p);
    if (d < best) best = d, *ref = polys[i], dtVcopy(out, c);
  }
  return *ref != 0;
}

void LoadBlocked() {
  FILE* f = Open(g_blocked_path, "r");
  if (!f) return;
  unsigned long long ref;
  int n = 0;
  while (fscanf(f, "%llx", &ref) == 1)
    if (dtStatusSucceed(g_navmesh->setPolyFlags(dtPolyRef(ref), 0))) ++n;
  fclose(f);
  RT_INFO("bot nav: %d blocked polygons learnt before", n);
}

std::filesystem::path ExeDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf));
  return n > 0 ? std::filesystem::path(std::string(buf, size_t(n))).parent_path() : std::filesystem::path(".");
#endif
}

void Unload() {
  if (g_navquery) dtFreeNavMeshQuery(g_navquery);
  if (g_navmesh) dtFreeNavMesh(g_navmesh);
  g_navquery = nullptr, g_navmesh = nullptr;
  g_piece.clear();
  g_main_piece = -1;
  g_main_chosen = false;
}

// A map without its mesh: looked for again every 15 s (tools/make_navs.py,
// started by nav_autogen or the launcher, puts it there when it is made).
std::chrono::steady_clock::time_point g_look_again{};
bool g_missing = false;

// A navigation file of the map (<exe dir>/nav, then nav/): empty if neither has it.
std::filesystem::path FindFile(const std::string& file) {
  for (const std::filesystem::path& dir : {ExeDir() / "nav", std::filesystem::path("nav")}) {
    std::error_code ec;
    if (std::filesystem::exists(dir / file, ec)) return dir / file;
  }
  return {};
}

// Reads a "BFNV" file (navgen's) into a new mesh; null if it can't.
dtNavMesh* ReadMesh(const std::filesystem::path& path, int* bytes, int* tile_count) {
  FILE* f = Open(path, "rb");
  if (!f) return nullptr;
  char magic[4];
  int version = 0, size = 0, tiles = 0;
  bool ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "BFNV", 4) && fread(&version, 4, 1, f) == 1 && version == 2;
  dtNavMesh* mesh = dtAllocNavMesh();
  dtNavMeshParams params;
  int count = 0;
  ok = ok && fread(&params, sizeof(params), 1, f) == 1 && fread(&count, 4, 1, f) == 1 &&
       dtStatusSucceed(mesh->init(&params));
  for (int i = 0; ok && i < count; ++i) {
    int tx, ty, ts;
    ok = fread(&tx, 4, 1, f) == 1 && fread(&ty, 4, 1, f) == 1 && fread(&ts, 4, 1, f) == 1 && ts > 0;
    unsigned char* data = ok ? static_cast<unsigned char*>(dtAlloc(ts, DT_ALLOC_PERM)) : nullptr;
    ok = ok && data && fread(data, 1, size_t(ts), f) == size_t(ts);
    if (ok && dtStatusFailed(mesh->addTile(data, ts, DT_TILE_FREE_DATA, 0, nullptr))) ok = false;
    if (!ok && data) dtFree(data);
    size += ts, ++tiles;
  }
  fclose(f);
  if (!ok) {
    RT_WARN("bot nav: %s isn't a navigation mesh Detour can read", Utf8(path).c_str());
    dtFreeNavMesh(mesh);
    return nullptr;
  }
  *bytes = size, *tile_count = tiles;
  return mesh;
}

bool Load(const std::string& map) {
  if (map == g_map && (g_navquery || !g_missing || std::chrono::steady_clock::now() < g_look_again))
    return g_navquery != nullptr;
  const bool again = map == g_map;
  Unload();
  g_map = map;
  g_missing = false;
  g_blocked_path = g_user_root / "nav" / (map + ".blocked");
  std::filesystem::path path = FindFile(map + ".nav");
  if (path.empty()) {
    if (!again) RT_INFO("bot nav: no navigation mesh for %s (nav/%s.nav)", map.c_str(), map.c_str());
    g_missing = true;
    g_look_again = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    navauto::Request("bf1943", map);
    return false;
  }
  int size = 0, tiles = 0;
  g_navmesh = ReadMesh(path, &size, &tiles);
  if (!g_navmesh) return false;
  g_navquery = dtAllocNavMeshQuery();
  g_navquery->init(g_navmesh, 4096);
  RT_INFO("bot nav: navigation mesh %s (%d bytes, %d tiles)", Utf8(path).c_str(), size, tiles);
  FindPieces();
  if (getenv("BF_BOT_NAV_KEEP_BLOCKS")) LoadBlocked();
  return true;
}

// The jeeps' and tanks' mesh (nav/<map>.vnav, as Bad Company's RouteVehicle in
// bc_bots.cpp: navgen with a vehicle's size), for the map in g_vmap; looked
// for again every 15 s while missing (tools/make_navs.py makes it).
dtNavMesh* g_vmesh = nullptr;
dtNavMeshQuery* g_vquery = nullptr;
std::string g_vmap;
std::chrono::steady_clock::time_point g_vlook_again{};
bool g_vbad = false;  // there but unreadable: not again for this map

bool LoadVehicles(const std::string& map) {
  if (map != g_vmap) {
    if (g_vquery) dtFreeNavMeshQuery(g_vquery);
    if (g_vmesh) dtFreeNavMesh(g_vmesh);
    g_vquery = nullptr, g_vmesh = nullptr, g_vbad = false;
    g_vmap = map;
    g_vlook_again = {};
  }
  if (g_vquery) return true;
  if (g_vbad || std::chrono::steady_clock::now() < g_vlook_again) return false;
  std::filesystem::path path = FindFile(map + ".vnav");
  if (path.empty()) {
    if (g_vlook_again == std::chrono::steady_clock::time_point{})
      RT_INFO("bot nav: no vehicles' navigation mesh for %s (nav/%s.vnav): they drive straight", map.c_str(),
              map.c_str());
    g_vlook_again = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    navauto::Request("bf1943", map);
    return false;
  }
  int size = 0, tiles = 0;
  g_vmesh = ReadMesh(path, &size, &tiles);
  if (!g_vmesh) return !(g_vbad = true);
  g_vquery = dtAllocNavMeshQuery();
  g_vquery->init(g_vmesh, 4096);
  RT_INFO("bot nav: vehicles' navigation mesh %s (%d bytes, %d tiles)", Utf8(path).c_str(), size, tiles);
  return true;
}

}  // namespace

bool VehiclesReady(const std::string& map) {
  std::lock_guard lock(g_mutex);
  return LoadVehicles(map);
}

// A vehicle's route from a to b on the vehicles' mesh: the nearest polygons
// to both ends (the goal's within 20 m across), A* (a partial route is driven
// too), the straight path's corners. 0 without the mesh or a way.
int RouteVehicle(const float a[3], const float b[3], float* corners, int max) {
  std::lock_guard lock(g_mutex);
  if (!g_vquery || max <= 0) return 0;
  dtQueryFilter filter;
  const float ext[3] = {6, 10, 6}, goal_ext[3] = {20, 60, 20};
  dtPolyRef sa = 0, sb = 0;
  float pa[3], pb[3];
  g_vquery->findNearestPoly(a, ext, &filter, &sa, pa);
  g_vquery->findNearestPoly(b, goal_ext, &filter, &sb, pb);
  if (!sa || !sb) return 0;
  dtPolyRef polys[512];
  int n = 0;
  g_vquery->findPath(sa, sb, pa, pb, &filter, polys, &n, 512);
  if (!n) return 0;
  float end[3];
  dtVcopy(end, pb);
  if (polys[n - 1] != sb) g_vquery->closestPointOnPoly(polys[n - 1], pb, end, nullptr);
  static float c[128 * 3];
  unsigned char flags[128];
  dtPolyRef refs[128];
  int nc = 0;
  g_vquery->findStraightPath(pa, end, polys, n, c, flags, refs, &nc, 128);
  int k = std::min(nc, max);
  memcpy(corners, c, size_t(k) * 3 * sizeof(float));
  static std::string routed;
  if (k > 0 && routed != g_vmap) {
    routed = g_vmap;
    RT_INFO("bot nav: driving on %s's vehicles' mesh (the first route: %d corners, %.0f m straight)", g_vmap.c_str(),
            k, dtVdist(a, b));
  }
  return k;
}

void ChooseMain(const float* points, int count) {
  std::lock_guard lock(g_mutex);
  if (!g_navquery || g_main_chosen || count <= 0) return;
  g_main_chosen = true;
  dtQueryFilter filter;
  const float ext[3] = {8, 60, 8};
  std::unordered_map<int, int> votes;
  for (int i = 0; i < count; ++i) {
    dtPolyRef ref = 0;
    float p[3];
    g_navquery->findNearestPoly(points + 3 * i, ext, &filter, &ref, p);
    auto it = ref ? g_piece.find(ref) : g_piece.end();
    if (it != g_piece.end()) ++votes[it->second];
  }
  int best = -1, n = 0;
  for (auto& [piece, v] : votes)
    if (v > n) n = v, best = piece;
  if (best >= 0 && best != g_main_piece) {
    RT_INFO("bot nav: main piece %d (%d of %d flags on it) instead of the biggest, %d", best, n, count, g_main_piece);
    g_main_piece = best;
  }
}

bool Ready(const std::string& map) {
  std::lock_guard lock(g_mutex);
  return Load(map);
}

void Block(const float at[3]) {
  std::lock_guard lock(g_mutex);
  if (!g_navquery) return;
  dtQueryFilter filter;
  const float ext[3] = {1.5f, 3, 1.5f};
  dtPolyRef ref = 0;
  float p[3];
  g_navquery->findNearestPoly(at, ext, &filter, &ref, p);
  if (!ref) return;
  unsigned short was = 1;
  g_navmesh->getPolyFlags(ref, &was);
  g_navmesh->setPolyFlags(ref, 0);
  static const bool keep = getenv("BF_BOT_NAV_KEEP_BLOCKS") != nullptr;
  if (!keep) {
    if (!g_blocked_until.count(ref) && was) g_blocked_until[ref] = {std::chrono::steady_clock::now() + std::chrono::seconds(60), was};
    RT_INFO("bot nav: blocked polygon %llx at %.1f %.1f %.1f for a minute", (unsigned long long)ref, p[0], p[1], p[2]);
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(g_blocked_path.parent_path(), ec);
  if (FILE* f = Open(g_blocked_path, "a")) {
    fprintf(f, "%llx\n", (unsigned long long)ref);
    fclose(f);
  }
  RT_INFO("bot nav: blocked polygon %llx at %.1f %.1f %.1f", (unsigned long long)ref, p[0], p[1], p[2]);
}

// The corners of a route from a to b (none if there's no mesh or no way).
int Route(const float a[3], const float b[3], float* corners, int max, Memory* mem) {
  std::lock_guard lock(g_mutex);
  if (!g_navquery || max <= 0) return 0;
  // Blocks whose minute is over open again.
  auto now = std::chrono::steady_clock::now();
  for (auto it = g_blocked_until.begin(); it != g_blocked_until.end();)
    if (now > it->second.first) g_navmesh->setPolyFlags(it->first, it->second.second), it = g_blocked_until.erase(it);
    else ++it;
  std::vector<std::array<float, 3>> out;
  dtQueryFilter filter;
  const float ext[3] = {3, 6, 3};
  dtPolyRef sa = 0, sb = 0;
  float pa[3], pb[3];
  g_navquery->findNearestPoly(a, ext, &filter, &sa, pa);
  // The goal (a flag, a vehicle) can be up on a structure: the nearest
  // walkable place within 8 m across and 60 m up or down.
  const float goal_ext[3] = {8, 60, 8};
  g_navquery->findNearestPoly(b, goal_ext, &filter, &sb, pb);
  if (sb && !InMainPiece(sb)) {  // the goal on an island: the main piece's nearest place
    float q[3];
    dtPolyRef r = 0;
    if (NearestMain(b, goal_ext, filter, &r, q)) sb = r, dtVcopy(pb, q);
  }
  // An island the main piece touches: on the main piece already. The soldier
  // on one: first back the way it came, or to the nearest main place.
  static Memory shared;
  Memory& memory = mem ? *mem : shared;
  if (sa && !InMainPiece(sa)) {
    const float touch_ext[3] = {1.5f, 1, 1.5f};
    float q[3];
    dtPolyRef r = 0;
    if (NearestMain(a, touch_ext, filter, &r, q)) sa = r, dtVcopy(pa, q);
  }
  if (sa && InMainPiece(sa)) dtVcopy(memory.last_main, pa), memory.have = true;
  if (sa && !InMainPiece(sa)) {
    const float near_ext[3] = {6, 4, 6};
    const float back_ext[3] = {1, 2, 1};
    float q[3];
    dtPolyRef r = 0;
    bool back = memory.have && dtVdist(memory.last_main, a) < 30.0f &&
                (g_navquery->findNearestPoly(memory.last_main, back_ext, &filter, &r, q), r && InMainPiece(r));
    // (From the water: the nearest land on the main piece, up to 40 m off.)
    const float far_ext[3] = {40, 10, 40};
    if (back || NearestMain(a, near_ext, filter, &r, q) || NearestMain(a, far_ext, filter, &r, q)) {
      out.push_back({q[0], q[1], q[2]});
      sa = r, dtVcopy(pa, q);
    }
  }
  if (!sa || !sb) return 0;
  dtPolyRef polys[512];
  int npolys = 0;
  dtStatus s3 = g_navquery->findPath(sa, sb, pa, pb, &filter, polys, &npolys, 512);
  if (!npolys) return 0;
  // Partial (up on a ledge it can drop from: the mesh has no links down):
  // first to the nearest place around, up to 5 m away and 4 m lower, with a
  // whole way.
  if (s3 & DT_PARTIAL_RESULT) {
    const float near_ext[3] = {0.5f, 4, 0.5f};
    for (float r : {1.5f, 3.0f, 5.0f}) {
      for (int k = 0; k < 16; ++k) {
        float q[3] = {a[0] + r * std::sin(k * 0.3927f), a[1] - 2, a[2] + r * std::cos(k * 0.3927f)}, pq[3];
        dtPolyRef sq = 0;
        g_navquery->findNearestPoly(q, near_ext, &filter, &sq, pq);
        if (!sq || sq == sa) continue;
        int nq = 0;
        dtStatus s4 = g_navquery->findPath(sq, sb, pq, pb, &filter, polys, &nq, 512);
        if (!nq || (s4 & DT_PARTIAL_RESULT)) continue;
        out.push_back({pq[0], pq[1], pq[2]});
        npolys = nq;
        memcpy(pa, pq, sizeof(pa));
        break;
      }
      if (out.size() > 0 && npolys) break;
    }
  }
  float c[256 * 3];
  unsigned char flags[256];
  dtPolyRef refs[256];
  int n = 0;
  g_navquery->findStraightPath(pa, pb, polys, npolys, c, flags, refs, &n, 256);
  for (int i = 0; i < n; ++i) out.push_back({c[3 * i], c[3 * i + 1], c[3 * i + 2]});
  int k = std::min(int(out.size()), max);
  for (int i = 0; i < k; ++i) memcpy(corners + 3 * i, out[size_t(i)].data(), 12);
  static std::string routed;  // (the map the first route was logged for)
  if (k > 0 && routed != g_map) {
    routed = g_map;
    RT_INFO("bot nav: routing on %s's mesh (the first route: %d corners, %.0f m straight)", g_map.c_str(), k,
            dtVdist(a, b));
  }
  return k;
}

// ---- learnt trails (as Bad Company's RecordTrails and RouteNav, bc_bots.cpp) ----
// <user>/nav/<map>.txt: "x y z" a line, a point a metre along a real
// player's way on foot, "-" starting a new trail (a respawn, a vehicle, a
// jump of over 5 m). Loaded as a graph: points linked along each trail and
// to other trails' points within 2.5 m (height within 1.5 m).
namespace {
std::mutex g_trail_mutex;
struct Trails {
  std::string map;
  std::vector<std::array<float, 3>> p;
  std::vector<std::vector<int>> next;
  uintmax_t file_size = 0;
  std::chrono::steady_clock::time_point checked{};
};
Trails g_trails;
std::string g_trail_map;

std::filesystem::path TrailPath(const std::string& map) { return g_user_root / "nav" / (map + ".txt"); }

// (Again when the file has grown, at most every 30 s.)
void LoadTrails(const std::string& map) {
  auto now = std::chrono::steady_clock::now();
  if (map == g_trails.map && now < g_trails.checked) return;
  std::error_code ec;
  std::filesystem::path path = TrailPath(map);
  uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) size = 0;
  if (map == g_trails.map && size == g_trails.file_size) {
    g_trails.checked = now + std::chrono::seconds(30);
    return;
  }
  Trails t;
  t.map = map, t.file_size = size, t.checked = now + std::chrono::seconds(30);
  if (FILE* f = size ? Open(path, "r") : nullptr) {
    char line[96];
    int prev = -1;
    while (fgets(line, sizeof(line), f)) {
      if (line[0] == '-' && (line[1] == '\n' || line[1] == '\r' || line[1] == 0)) {  // a lone "-" (not -12.3)
        prev = -1;
        continue;
      }
      std::array<float, 3> q;
      if (sscanf(line, "%f %f %f", &q[0], &q[1], &q[2]) != 3) continue;
      t.p.push_back(q);
      t.next.emplace_back();
      int i = int(t.p.size()) - 1;
      if (prev >= 0) t.next[size_t(i)].push_back(prev), t.next[size_t(prev)].push_back(i);
      prev = i;
    }
    fclose(f);
    // Trails joined where they pass close (a grid of 4 m cells).
    std::map<std::pair<int, int>, std::vector<int>> cells;
    auto cell = [&](int i) {
      return std::make_pair(int(std::floor(t.p[size_t(i)][0] / 4)), int(std::floor(t.p[size_t(i)][2] / 4)));
    };
    for (int i = 0; i < int(t.p.size()); ++i) cells[cell(i)].push_back(i);
    for (int i = 0; i < int(t.p.size()); ++i) {
      auto [cx, cz] = cell(i);
      for (int ox = -1; ox <= 1; ++ox)
        for (int oz = -1; oz <= 1; ++oz) {
          auto it = cells.find({cx + ox, cz + oz});
          if (it == cells.end()) continue;
          for (int j : it->second) {
            const auto &a = t.p[size_t(i)], &b = t.p[size_t(j)];
            if (j > i && std::hypot(a[0] - b[0], a[2] - b[2]) < 2.5f && std::fabs(a[1] - b[1]) < 1.5f)
              t.next[size_t(i)].push_back(j), t.next[size_t(j)].push_back(i);
          }
        }
    }
    if (!t.p.empty()) RT_INFO("bot nav: %zu trail points learnt on %s (%s)", t.p.size(), map.c_str(), Utf8(path).c_str());
  }
  g_trails = std::move(t);
}

int NearestTrail(const float at[3], float within) {
  int best = -1;
  float bd = within;
  for (int i = 0; i < int(g_trails.p.size()); ++i) {
    const auto& q = g_trails.p[size_t(i)];
    float d = std::hypot(q[0] - at[0], q[2] - at[2]);
    if (d < bd && std::fabs(q[1] - at[1]) < 6.0f) bd = d, best = i;
  }
  return best;
}
}  // namespace

// Each soldier's trail is kept here and written out whole ("-", then its
// points) when it ends or has 8 points (the next part starts at its last
// point; little is lost when the server stops): several soldiers' points
// written as they come would interleave.
static std::map<uint32_t, std::vector<std::array<float, 3>>> g_trail_open;
static void FlushTrail(FILE* f, std::vector<std::array<float, 3>>& t, bool keep_last) {
  if (t.size() >= 2) {
    fprintf(f, "-\n");
    for (const auto& q : t) fprintf(f, "%.2f %.2f %.2f\n", q[0], q[1], q[2]);
  }
  if (keep_last && !t.empty()) t.erase(t.begin(), t.end() - 1);
  else t.clear();
}

void RecordTrails(const std::string& map, const uint32_t* ids, const float* pos, int count) {
  std::lock_guard lock(g_trail_mutex);
  if (map != g_trail_map) g_trail_open.clear(), g_trail_map = map;
  FILE* f = nullptr;
  auto file = [&]() {
    if (!f) {
      std::error_code ec;
      std::filesystem::create_directories(TrailPath(map).parent_path(), ec);
      f = Open(TrailPath(map), "a");
    }
    return f;
  };
  std::map<uint32_t, bool> here;
  for (int k = 0; k < count; ++k) {
    float x = pos[3 * k], y = pos[3 * k + 1], z = pos[3 * k + 2];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
    here[ids[k]] = true;
    auto& t = g_trail_open[ids[k]];
    if (!t.empty()) {
      float d = std::hypot(x - t.back()[0], z - t.back()[2]);
      if (d < 1.0f) continue;
      if (d > 5.0f && file()) FlushTrail(f, t, false);  // a jump (a respawn, a teleport): a new trail
    }
    t.push_back({x, y, z});
    if (t.size() >= 8 && file()) FlushTrail(f, t, true);
  }
  // One not here this time (dead, in a vehicle): its trail ends.
  for (auto it = g_trail_open.begin(); it != g_trail_open.end();) {
    if (here.count(it->first)) {
      ++it;
      continue;
    }
    if (file()) FlushTrail(f, it->second, false);
    it = g_trail_open.erase(it);
  }
  if (f) fclose(f);
}

int RouteTrails(const std::string& map, const float a[3], const float b[3], float* corners, int max) {
  std::lock_guard lock(g_trail_mutex);
  LoadTrails(map);
  if (g_trails.p.empty() || max <= 0) return 0;
  int from = NearestTrail(a, 15.0f), to = NearestTrail(b, 30.0f);
  if (from < 0 || to < 0) return 0;
  const auto& P = g_trails.p;
  std::vector<float> cost(P.size(), 1e30f);
  std::vector<int> came(P.size(), -1);
  using Item = std::pair<float, int>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
  auto h = [&](int i) { return std::hypot(P[size_t(i)][0] - P[size_t(to)][0], P[size_t(i)][2] - P[size_t(to)][2]); };
  cost[size_t(from)] = 0;
  open.push({h(from), from});
  while (!open.empty()) {
    auto [f, i] = open.top();
    open.pop();
    if (i == to) break;
    if (f - h(i) > cost[size_t(i)] + 1e-3f) continue;  // (a stale entry)
    for (int j : g_trails.next[size_t(i)]) {
      float c = cost[size_t(i)] + std::hypot(P[size_t(i)][0] - P[size_t(j)][0], P[size_t(i)][2] - P[size_t(j)][2]);
      if (c < cost[size_t(j)]) cost[size_t(j)] = c, came[size_t(j)] = i, open.push({c + h(j), j});
    }
  }
  if (came[size_t(to)] < 0 && from != to) return 0;
  std::vector<int> route;
  for (int i = to; i >= 0; i = came[size_t(i)]) route.push_back(i);
  std::reverse(route.begin(), route.end());
  // Every fourth point (about 4 m apart) and the last, as corners.
  int k = 0;
  for (size_t r = 0; r < route.size() && k < max; ++r) {
    if (r % 4 != 0 && r + 1 != route.size()) continue;
    memcpy(corners + 3 * k++, P[size_t(route[r])].data(), 12);
  }
  static std::string routed;
  if (k > 0 && routed != map) {
    routed = map;
    RT_INFO("bot nav: routing on the trails learnt on %s (the first route: %zu points, %.0f m straight)", map.c_str(),
            route.size(), dtVdist(a, b));
  }
  return k;
}

bool HaveTrails(const std::string& map) {
  std::lock_guard lock(g_trail_mutex);
  LoadTrails(map);
  return !g_trails.p.empty();
}

// Whether a soldier can stand there: on the main piece, within 1 m (near height y).
bool Walkable(float x, float y, float z) {
  std::lock_guard lock(g_mutex);
  if (!g_navquery) return true;
  dtQueryFilter filter;
  const float ext[3] = {1.0f, 3.0f, 1.0f};
  float p[3] = {x, y, z}, q[3];
  dtPolyRef r = 0;
  g_navquery->findNearestPoly(p, ext, &filter, &r, q);
  return r && InMainPiece(r) && std::hypot(q[0] - x, q[2] - z) < 1.0f;
}

// Up to max places r0 to r1 m around `at` on the main piece, one in each of
// max directions, clockwise from the north.
int Places(const float at[3], float r0, float r1, float* out, int max) {
  std::lock_guard lock(g_mutex);
  if (!g_navquery || max <= 0) return 0;
  dtQueryFilter filter;
  const float ext[3] = {2, 4, 2};
  float onmesh[3];
  dtPolyRef start = 0;
  g_navquery->findNearestPoly(at, ext, &filter, &start, onmesh);
  std::vector<std::array<float, 4>> found;  // angle, x y z
  std::vector<bool> taken(size_t(max), false);
  for (int tries = 0; start && tries < 24 * max && int(found.size()) < max; ++tries) {
    dtPolyRef ref = 0;
    float pt[3];
    if (dtStatusFailed(g_navquery->findRandomPointAroundCircle(start, onmesh, r1, &filter,
                                                               [] { return float(rand()) / RAND_MAX; }, &ref, pt)))
      break;
    float d = std::hypot(pt[0] - at[0], pt[2] - at[2]);
    if (d < r0 || d > r1 || !InMainPiece(ref)) continue;
    float a = std::atan2(pt[0] - at[0], pt[2] - at[2]);
    if (a < 0) a += 6.2831853f;
    int sector = std::min(int(a / 6.2831853f * max), max - 1);
    if (taken[size_t(sector)]) continue;
    taken[size_t(sector)] = true;
    found.push_back({a, pt[0], pt[1], pt[2]});
  }
  std::sort(found.begin(), found.end());
  for (size_t i = 0; i < found.size(); ++i) memcpy(out + 3 * i, &found[i][1], 3 * sizeof(float));
  return int(found.size());
}

}  // namespace rt::nav
