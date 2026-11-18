// The game's own level pictures for the bots' maps: see bot_minimap.h.
//
// An FbRB archive (tools/fbrb.py): "FbRB", the table of contents' compressed
// size (big-endian u32), the table (gzip), then the data (one gzip stream, or
// stored). The texture is some 15 to 270 MB into the data, so the stream is
// inflated here as it is read, only the texture's bytes kept. The texture
// (tools/texpack/xtexture.py): a 92-byte big-endian header (format, width,
// height), then the top level, 2D-tiled for the Xenos GPU and byte-swapped in
// 16-bit words; DXT1 blocks.
#include "bot_minimap.h"

#ifndef BOT_MINIMAP_OFFLINE
#include "kernel.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "../thirdparty/xenia/third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../thirdparty/xenia/third_party/stb/stb_image_write.h"

namespace rt::botllm::minimap {
namespace {

// ---- where each picture lies ------------------------------------------------------
// Fitted per map (the picture's edges and land against the level's terrain
// heightfield and our own drawing of it; tools/minimap_fit.py): the world
// square's centre and half width, and which axis runs the other way. All are
// the level's detailed terrain tile (-1024..1024) to within a few metres.
struct Fit {
  const char* map;  // (lower case)
  float cx, cz, half;
  bool mirror_x, mirror_z;
};
constexpr Fit kFits[] = {
    // Battlefield 1943 (east on the picture's left).
    {"wake_island_s", 0.8f, -2.1f, 1022.5f, true, false},
    {"iwo_jima_s", 1.0f, 0.0f, 1020.8f, true, false},
    {"guadal_canal", 0.0f, 0.0f, 1022.5f, true, false},  // (its fits scatter by 15 m: the terrain tile's)
    {"coral_sea", 0.0f, 0.0f, 1022.5f, true, false},     // (air only; likewise)
    // Bad Company (north at the picture's bottom); a level's two variants share its terrain.
    {"mp_harbour_day1", -1.0f, -0.25f, 1022.75f, false, true},
    {"mp_harbour_day2", -1.0f, -0.25f, 1022.75f, false, true},
    {"mp_ignition_day1", -1.25f, -0.5f, 1022.5f, false, true},
    {"mp_ignition_day2", -1.25f, -0.5f, 1022.5f, false, true},
    {"mp_monastery_day1", -0.75f, -0.25f, 1022.75f, false, true},
    {"mp_monastery_day2", -0.75f, -0.25f, 1022.75f, false, true},
    {"mp_beachhead_day1", -1.5f, 2.0f, 1023.75f, false, true},
    {"mp_beachhead_day2", -1.5f, 2.0f, 1023.75f, false, true},
};

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const Fit* FitFor(const std::string& map) {
  std::string low = Lower(map);
  for (const Fit& f : kFits)
    if (low == f.map) return &f;
  return nullptr;
}

// ---- inflate (RFC 1951), streamed from a file ----------------------------------------
class Inflater {
 public:
  Inflater(std::ifstream& in, uint64_t start) : in_(in), buf_(1 << 20) { in_.seekg(std::streamoff(start)); }

  // Inflates a gzip member (members: and any following it), keeping output
  // bytes [lo, hi) in out (hi: UINT64_MAX for all of it); stops once past hi.
  // False on bad data, or the data ending before hi.
  bool Run(uint64_t lo, uint64_t hi, std::vector<uint8_t>& out, bool members) {
    lo_ = lo, hi_ = hi, out_ = &out;
    if (hi != UINT64_MAX) out.assign(size_t(hi - lo), 0);
    for (bool first = true;; first = false) {
      if (!first) {  // another member after this one's trailer?
        if (!members) return true;
        Drop(bc_ & 7);
        Get(32), Get(32);  // (CRC-32, size)
        if (over_ || Peek(16) != 0x8B1F) return pos_ >= (hi == UINT64_MAX ? 0 : hi);
      }
      if (!Header()) return false;
      for (bool last = false; !last;) {
        last = Get(1);
        int type = int(Get(2));
        bool ok = type == 0 ? Stored() : type == 1 ? Codes(Fixed(true), Fixed(false))
                : type == 2 ? Dynamic() : false;
        if (!ok || over_ > 8) return false;
        if (pos_ >= hi_) return true;
      }
    }
  }

 private:
  struct Huff {
    uint16_t count[16] = {};
    uint16_t symbol[288] = {};
    uint16_t fast[1 << 10] = {};  // the next 10 bits: length << 9 | symbol (0: a longer code)
    bool Build(const uint8_t* lens, int n) {
      std::fill(std::begin(count), std::end(count), 0);
      std::fill(std::begin(fast), std::end(fast), 0);
      for (int i = 0; i < n; ++i) ++count[lens[i]];
      count[0] = 0;
      int left = 1;
      for (int len = 1; len < 16; ++len) {
        left = (left << 1) - count[len];
        if (left < 0) return false;  // (over-subscribed; incomplete is allowed)
      }
      uint16_t offs[16] = {};
      for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + count[len]);
      for (int i = 0; i < n; ++i)
        if (lens[i]) symbol[offs[lens[i]]++] = uint16_t(i);
      int code = 0, idx = 0;
      for (int len = 1; len < 16; ++len) {
        for (int k = 0; k < count[len]; ++k, ++code) {
          int sym = symbol[idx++];
          if (len > 10) continue;
          int rev = 0;
          for (int b = 0; b < len; ++b) rev |= ((code >> b) & 1) << (len - 1 - b);
          for (int r = rev; r < (1 << 10); r += 1 << len) fast[r] = uint16_t(len << 9 | sym);
        }
        code <<= 1;
      }
      return true;
    }
  };

  int Byte() {
    if (p_ == n_) {
      in_.read(reinterpret_cast<char*>(buf_.data()), std::streamsize(buf_.size()));
      n_ = size_t(in_.gcount()), p_ = 0;
      if (!n_) return ++over_, 0;
    }
    return buf_[p_++];
  }
  void Need(int k) {
    while (bc_ < k) bb_ |= uint64_t(Byte()) << bc_, bc_ += 8;
  }
  uint32_t Peek(int k) { return Need(k), uint32_t(bb_ & ((uint64_t(1) << k) - 1)); }
  void Drop(int k) { bb_ >>= k, bc_ -= k; }
  uint32_t Get(int k) {
    if (!k) return 0;
    uint32_t v = Peek(k);
    Drop(k);
    return v;
  }

  void Put(uint8_t b) {
    win_[pos_ & 0xFFFF] = b;
    if (pos_ >= lo_ && pos_ < hi_) {
      if (hi_ == UINT64_MAX) out_->push_back(b);
      else (*out_)[size_t(pos_ - lo_)] = b;
    }
    ++pos_;
  }

  int Decode(const Huff& h) {
    if (uint16_t e = h.fast[Peek(10)]) return Drop(e >> 9), e & 511;
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      code |= int(Get(1));
      int count = h.count[len];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count, first += count;
      first <<= 1, code <<= 1;
    }
    return -1;
  }

  bool Header() {
    if (Get(16) != 0x8B1F || Get(8) != 8) return false;
    uint32_t flags = Get(8);
    Get(32), Get(16);  // (time, extra flags, system)
    if (flags & 4) {
      uint32_t n = Get(16);
      while (n--) Get(8);
    }
    for (uint32_t bit : {8u, 16u})
      if (flags & bit)
        while (Get(8) && !over_) {
        }
    if (flags & 2) Get(16);
    return !over_;
  }

  bool Stored() {
    Drop(bc_ & 7);
    uint32_t len = Get(16), nlen = Get(16);
    if ((len ^ 0xFFFF) != nlen) return false;
    while (len-- && !over_) Put(uint8_t(Get(8)));
    return true;
  }

  const Huff& Fixed(bool lit) {
    static Huff lh, dh;
    static std::once_flag once;
    std::call_once(once, [] {
      uint8_t l[288];
      for (int i = 0; i < 288; ++i) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
      lh.Build(l, 288);
      uint8_t d[30];
      std::fill(d, d + 30, 5);
      dh.Build(d, 30);
    });
    return lit ? lh : dh;
  }

  bool Dynamic() {
    int nlen = int(Get(5)) + 257, ndist = int(Get(5)) + 1, ncode = int(Get(4)) + 4;
    if (nlen > 286 || ndist > 30) return false;
    static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t lens[320] = {};
    for (int i = 0; i < ncode; ++i) lens[kOrder[i]] = uint8_t(Get(3));
    if (!lencode_.Build(lens, 19)) return false;
    std::fill(std::begin(lens), std::end(lens), 0);
    for (int i = 0; i < nlen + ndist;) {
      int sym = Decode(lencode_);
      if (sym < 0) return false;
      if (sym < 16) {
        lens[i++] = uint8_t(sym);
        continue;
      }
      int rep, len = 0;
      if (sym == 16) {
        if (!i) return false;
        len = lens[i - 1], rep = 3 + int(Get(2));
      } else {
        rep = sym == 17 ? 3 + int(Get(3)) : 11 + int(Get(7));
      }
      if (i + rep > nlen + ndist) return false;
      while (rep--) lens[i++] = uint8_t(len);
    }
    if (!lens[256]) return false;
    return lit_.Build(lens, nlen) && dist_.Build(lens + nlen, ndist) && Codes(lit_, dist_);
  }

  bool Codes(const Huff& lit, const Huff& dist) {
    static const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      int sym = Decode(lit);
      if (sym < 0 || over_ > 8) return false;
      if (sym < 256) {
        Put(uint8_t(sym));
        continue;
      }
      if (sym == 256) return true;
      sym -= 257;
      if (sym >= 29) return false;
      int len = kLenBase[sym] + int(Get(kLenExtra[sym]));
      int ds = Decode(dist);
      if (ds < 0 || ds >= 30) return false;
      uint32_t d = kDistBase[ds] + Get(kDistExtra[ds]);
      if (d > pos_ || d > 32768) return false;
      for (int k = 0; k < len; ++k) Put(win_[(pos_ - d) & 0xFFFF]);
      if (pos_ >= hi_) return true;  // (the caller stops)
    }
  }

  std::ifstream& in_;
  std::vector<uint8_t> buf_;
  size_t p_ = 0, n_ = 0;
  int over_ = 0;  // bytes asked for past the end
  uint64_t bb_ = 0;
  int bc_ = 0;
  uint8_t win_[1 << 16];
  uint64_t pos_ = 0, lo_ = 0, hi_ = 0;
  std::vector<uint8_t>* out_ = nullptr;
  Huff lencode_, lit_, dist_;
};

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// A path under root, each part matched without regard to case (Linux).
std::filesystem::path Find(const std::filesystem::path& root, std::initializer_list<std::string> parts) {
  std::filesystem::path p = root;
  std::error_code ec;
  for (const std::string& part : parts) {
    if (std::filesystem::exists(p / part, ec)) {
      p /= part;
      continue;
    }
    std::filesystem::path found;
    for (std::filesystem::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec))
      if (Lower(it->path().filename().string()) == Lower(part)) found = it->path();
    if (found.empty()) return {};
    p = found;
  }
  return p;
}

// The level's minimap texture's bytes, from its archive.
bool ReadTexture(const std::filesystem::path& archive, std::vector<uint8_t>& res, std::string* name, std::string* why) {
  std::ifstream in(archive, std::ios::binary);
  uint8_t head[8];
  if (!in.read(reinterpret_cast<char*>(head), 8) || memcmp(head, "FbRB", 4) != 0) return *why = "not an FbRB archive", false;
  const uint64_t data_at = 8 + uint64_t(Be32(head + 4));
  std::vector<uint8_t> toc;
  {
    Inflater z(in, 8);
    if (!z.Run(0, UINT64_MAX, toc, false) || toc.size() < 12) return *why = "its table of contents is damaged", false;
  }
  uint32_t slen = Be32(&toc[4]);
  if (12 + uint64_t(slen) > toc.size()) return *why = "its table of contents is damaged", false;
  const char* strings = reinterpret_cast<const char*>(&toc[8]);
  auto text = [&](uint32_t off) { return off < slen ? std::string(strings + off, strnlen(strings + off, slen - off)) : std::string(); };
  size_t at = 8 + slen;
  uint32_t count = Be32(&toc[at]);
  at += 4;
  if (at + uint64_t(count) * 24 > toc.size()) return *why = "its table of contents is damaged", false;
  uint64_t off = 0, size = 0;
  for (uint32_t i = 0; i < count; ++i, at += 24) {
    const uint8_t* e = &toc[at];
    if (text(Be32(e + 20)) != "XenonTexture") continue;
    std::string n = text(Be32(e)), low = Lower(n);
    // BF1943's "Textures/UI/Minimap/beach/wake.res", BC's "Terrains/MP_Harbour/Textures/MP_harbour_minimap.res".
    bool bf = low.rfind("textures/ui/minimap/beach/", 0) == 0;
    bool bc = low.size() > 12 && low.compare(low.size() - 12, 12, "_minimap.res") == 0 && low.rfind("terrains/", 0) == 0;
    if (!bf && !bc) continue;
    *name = n, off = Be32(e + 8), size = Be32(e + 12);
    break;
  }
  if (!size) return *why = "no minimap texture in it", false;
  in.clear();
  in.seekg(std::streamoff(data_at));
  uint8_t magic[2] = {};
  in.read(reinterpret_cast<char*>(magic), 2);
  if (magic[0] == 0x1F && magic[1] == 0x8B) {
    in.clear();
    Inflater z(in, data_at);
    if (!z.Run(off, off + size, res, true)) return *why = "its data is damaged", false;
  } else {  // (stored)
    res.resize(size_t(size));
    in.clear();
    in.seekg(std::streamoff(data_at + off));
    if (!in.read(reinterpret_cast<char*>(res.data()), std::streamsize(size))) return *why = "its data is cut short", false;
  }
  return true;
}

// The texture's top level as RGB (DXT1 only: what the minimaps are).
bool DecodeTexture(const std::vector<uint8_t>& res, Image& img, std::string* why) {
  if (res.size() < 92) return *why = "the texture is cut short", false;
  uint32_t format = Be32(&res[8]), w = Be32(&res[16]), h = Be32(&res[20]);
  if (format != 0) return *why = "the texture is not DXT1 (format " + std::to_string(format) + ")", false;
  if (w < 4 || h < 4 || w > 4096 || h > 4096) return *why = "the texture's size is odd", false;
  const uint8_t* src = &res[92];
  const size_t avail = res.size() - 92;
  const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, pitch = (bw + 31) & ~31u;
  img.w = int(w), img.h = int(h);
  img.rgb.assign(size_t(w) * h * 3, 0);
  for (uint32_t y = 0; y < bh; ++y)
    for (uint32_t x = 0; x < bw; ++x) {
      // (Xenos 2D tiling, 8 bytes a block: tools/texpack/xtexture.py's _tiled_offsets)
      uint64_t macro = uint64_t((x >> 5) + (y >> 5) * (pitch >> 5)) << 10;
      uint64_t micro = uint64_t((x & 7) + ((y & 0xE) << 2)) << 3;
      uint64_t o = macro + ((micro & ~uint64_t(0xF)) << 1) + (micro & 0xF) + ((y & 1) << 4);
      uint64_t a = ((o & ~uint64_t(0x1FF)) << 3) + ((y & 16) << 7) + ((o & 0x1C0) << 2) +
                   (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (o & 0x3F);
      if (a + 8 > avail) return *why = "the texture is cut short", false;
      uint8_t b[8];
      for (int k = 0; k < 8; k += 2) b[k] = src[a + k + 1], b[k + 1] = src[a + k];  // (16-bit words swapped)
      uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
      uint32_t bits = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
      int pal[4][3];
      for (int k = 0; k < 2; ++k) {
        uint32_t c = k ? c1 : c0;
        pal[k][0] = int(((c >> 11) & 31) * 255 + 15) / 31;
        pal[k][1] = int(((c >> 5) & 63) * 255 + 31) / 63;
        pal[k][2] = int((c & 31) * 255 + 15) / 31;
      }
      for (int ch = 0; ch < 3; ++ch) {
        if (c0 > c1) {
          pal[2][ch] = (2 * pal[0][ch] + pal[1][ch]) / 3;
          pal[3][ch] = (pal[0][ch] + 2 * pal[1][ch]) / 3;
        } else {
          pal[2][ch] = (pal[0][ch] + pal[1][ch]) / 2;
          pal[3][ch] = 0;
        }
      }
      for (int i = 0; i < 16; ++i) {
        uint32_t px = x * 4 + (i & 3), py = y * 4 + (i >> 2);
        if (px >= w || py >= h) continue;
        const int* c = pal[(bits >> (2 * i)) & 3];
        uint8_t* d = &img.rgb[(size_t(py) * w + px) * 3];
        d[0] = uint8_t(c[0]), d[1] = uint8_t(c[1]), d[2] = uint8_t(c[2]);
      }
    }
  return true;
}

bool ReadFile(const std::filesystem::path& p, std::vector<uint8_t>& out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  out.resize(size_t(in.tellg()));
  in.seekg(0);
  return bool(in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
}

}  // namespace

bool Image::At(float x, float z, float out[3]) const {
  if (w <= 0) return false;
  float fx = (x - (cx - half)) / (2 * half), fz = ((cz + half) - z) / (2 * half);  // (0..1: west to east, north to south)
  if (fx < 0 || fz < 0 || fx > 1 || fz > 1) return false;
  float u = (mirror_x ? 1 - fx : fx) * w - 0.5f, v = (mirror_z ? 1 - fz : fz) * h - 0.5f;
  int u0 = int(std::floor(u)), v0 = int(std::floor(v));
  float a = u - u0, b = v - v0;
  auto px = [&](int i, int j) {
    i = std::clamp(i, 0, w - 1), j = std::clamp(j, 0, h - 1);
    return &rgb[(size_t(j) * w + i) * 3];
  };
  const uint8_t *p00 = px(u0, v0), *p10 = px(u0 + 1, v0), *p01 = px(u0, v0 + 1), *p11 = px(u0 + 1, v0 + 1);
  for (int c = 0; c < 3; ++c)
    out[c] = (p00[c] * (1 - a) + p10[c] * a) * (1 - b) + (p01[c] * (1 - a) + p11[c] * a) * b;
  return true;
}

std::shared_ptr<Image> Load(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                            const std::string& map, std::string* why) {
  std::string scratch;
  if (!why) why = &scratch;
  const Fit* fit = FitFor(map);
  if (!fit) return *why = "no minimap known for this map", nullptr;
  auto img = std::make_shared<Image>();
  img->cx = fit->cx, img->cz = fit->cz, img->half = fit->half;
  img->mirror_x = fit->mirror_x, img->mirror_z = fit->mirror_z;
  const std::filesystem::path cached = cache_dir.empty() ? std::filesystem::path() : cache_dir / (fit->map + std::string(".png"));
  std::vector<uint8_t> bytes;
  bool have = false;
  if (!cached.empty() && ReadFile(cached, bytes)) {
    int w = 0, h = 0, n = 0;
    if (uint8_t* p = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 3)) {
      img->w = w, img->h = h;
      img->rgb.assign(p, p + size_t(w) * h * 3);
      stbi_image_free(p);
      have = w >= 64 && h >= 64;
    }
  }
  if (!have) {
    std::filesystem::path archive;
    for (const char* name : {"reduced-level-00.fbrb", "level-00.fbrb"})
      if (archive.empty()) archive = Find(game_root, {"dist", "xenon", "levels", fit->map, name});
    if (archive.empty()) return *why = "the level's archive is not in the game files", nullptr;
    std::vector<uint8_t> res;
    std::string texture;
    if (!ReadTexture(archive, res, &texture, why)) return *why = archive.filename().string() + ": " + *why, nullptr;
    if (!DecodeTexture(res, *img, why)) return *why = texture + ": " + *why, nullptr;
    if (!cached.empty()) {  // (written whole, then renamed: never half a file)
      std::vector<uint8_t> png;
      stbi_write_png_to_func([](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
      }, &png, img->w, img->h, 3, img->rgb.data(), img->w * 3);
      std::error_code ec;
      std::filesystem::create_directories(cache_dir, ec);
      std::filesystem::path tmp = cached;
      tmp += ".tmp";
      if (std::ofstream(tmp, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size())))
        std::filesystem::rename(tmp, cached, ec);
      if (ec) std::filesystem::remove(tmp, ec);
    }
  }
  // Grey (Bad Company's shading): hardly any colour anywhere. Its ground's
  // middle brightness (the water's left out).
  double sat = 0;
  std::vector<float> lums;
  for (size_t i = 0; i + 2 < img->rgb.size(); i += 3 * 97) {
    const uint8_t* p = &img->rgb[i];
    sat += std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]});
    float l = (p[0] + p[1] + p[2]) / 3.0f;
    if (l >= Image::kGreyWater) lums.push_back(l);
  }
  img->grey = sat / double(img->rgb.size() / (3 * 97) + 1) < 6.0;
  if (!lums.empty()) {
    std::nth_element(lums.begin(), lums.begin() + lums.size() / 2, lums.end());
    img->mid = std::max(lums[lums.size() / 2], 1.0f);
  }
  return img;
}

#ifndef BOT_MINIMAP_OFFLINE
namespace {
std::mutex g_mutex;
std::map<std::string, std::shared_future<std::shared_ptr<const Image>>> g_maps;  // (lower case)
}  // namespace

void Prepare(const std::string& map) {
  if (!FitFor(map)) return;
  std::string key = Lower(map);
  std::lock_guard lock(g_mutex);
  if (g_maps.count(key)) return;
  auto done = std::make_shared<std::promise<std::shared_ptr<const Image>>>();
  g_maps[key] = done->get_future().share();
  std::thread([done, key] {
    auto t0 = std::chrono::steady_clock::now();
    std::string why;
    std::shared_ptr<const Image> img = Load(g_game_root, g_user_root / "minimaps", key, &why);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (img)
      RT_INFO("bot llm: %s's minimap (the game's own, %dx%d, %.1f m a pixel) ready in %.1f s", key.c_str(), img->w,
              img->h, img->MetresPerPixel(), s);
    else
      RT_WARN("bot llm: no minimap for %s (%s): the bots' maps are drawn from the ground alone", key.c_str(), why.c_str());
    done->set_value(std::move(img));
  }).detach();
}

std::shared_ptr<const Image> Get(const std::string& map, bool wait) {
  Prepare(map);
  std::shared_future<std::shared_ptr<const Image>> f;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_maps.find(Lower(map));
    if (it == g_maps.end()) return nullptr;
    f = it->second;
  }
  if (!wait && f.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
  return f.get();
}
#endif

}  // namespace rt::botllm::minimap
