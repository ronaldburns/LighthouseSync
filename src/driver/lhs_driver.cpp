// LighthouseSync's additions to the driver (lhs_driver.h).
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../common/lhs_fork.h"
#include "json.h"
#include "lhs_driver.h"
#include "net.h"
#include "openvr_driver.h"
#include "sync.h"

namespace lhs {
namespace {

const char *kSection = "driver_questlhsync";

LogFn g_log;
HANDLE g_map;
LhsExtra *g_x;

// from the headset link's thread, under g_m
std::mutex g_m;
struct Cam {
  int w = 0, h = 0, used = -1;
  double last = 0;
  int mean = 0, nblobs = 0;
  std::vector<LhsBlob> b;
  long frames = 0, blobs = 0;            // so far
  long frames_at = 0, blobs_at = 0;      // at the last rate update
  double fps = 0, blob_rate = 0;
};
std::map<int, Cam> g_cams;
std::string g_module, g_heartbeat, g_caminfo;
double g_heartbeat_t = -1;
long g_lines = 0, g_lines_at = 0;
double g_lps = 0, g_rate_t = 0;

void Copy(char *dst, size_t n, const std::string &s) {
  strncpy(dst, s.c_str(), n - 1);
  dst[n - 1] = 0;
}

void Log(const std::string &s) {
  if (g_log) g_log(s);
}

// a field of the headset's hello: "module" in "H QuestLHSync 1 serial=... module=v1.26"
std::string Field(const std::string &h, const char *k) {
  size_t i = h.find(std::string(" ") + k + "=");
  if (i == std::string::npos) return "";
  i += strlen(k) + 2;
  size_t j = h.find(' ', i);
  return h.substr(i, j == std::string::npos ? std::string::npos : j - i);
}

// "F cam seq t_us mean nblobs [x10 y10 npx peak]..." (nblobs -1: a frame without spots, the Quest's long ones)
void OnFrame(double now, const std::string &line) {
  int cam, mean, nb, off = 0;
  unsigned seq;
  long long tus;
  if (sscanf(line.c_str(), "F %d %u %lld %d %d%n", &cam, &seq, &tus, &mean, &nb, &off) < 5) return;
  if (cam < 0 || cam >= 64) return;
  Cam &c = g_cams[cam];
  c.last = now;
  c.mean = mean;
  c.nblobs = nb;
  c.b.clear();
  c.frames++;
  if (nb <= 0) return;
  c.blobs += nb;
  const char *s = line.c_str() + off;
  for (int i = 0; i < nb && (int)c.b.size() < LHS_MAX_BLOBS; i++) {
    int x, y, npx, peak, used = 0;
    if (sscanf(s, " %d %d %d %d%n", &x, &y, &npx, &peak, &used) < 4) break;
    s += used;
    c.b.push_back({(int16_t)std::clamp(x, 0, 32767), (int16_t)std::clamp(y, 0, 32767),
                   (int16_t)std::clamp(npx, 0, 32767), (int16_t)std::clamp(peak, 0, 32767)});
  }
}

// lhsight's start message on the Frame: "I lhsight: XRService 1234, cameras 0 /dev/video2 640x480 bpl 640 ..., 1 ..."
void OnInfo(const std::string &line) {
  size_t k = line.find("lhsight: XRService");
  size_t c = k == std::string::npos ? k : line.find("cameras ", k);
  if (c == std::string::npos) return;
  g_caminfo = line.substr(c + 8);
  size_t a = 0;
  while (a < g_caminfo.size()) {
    size_t b = g_caminfo.find(", ", a);
    std::string one = g_caminfo.substr(a, b == std::string::npos ? std::string::npos : b - a);
    int id, w, h;
    char dev[64];
    if (sscanf(one.c_str(), "%d %63s %dx%d", &id, dev, &w, &h) == 4 && id >= 0 && id < 64 && w > 0 && h > 0) {
      g_cams[id].w = w;
      g_cams[id].h = h;
    }
    if (b == std::string::npos) break;
    a = b + 2;
  }
}

struct Settings {
  bool any_hmd = false, gravity = true, steady = true, record = false;
  std::string host, headset;
};

// as the driver's ReadSettings takes them: gravity and steadyStations are on when unset
Settings ReadSettings() {
  Settings r;
  auto *s = vr::VRSettings();
  if (!s) return r;
  vr::EVRSettingsError e = vr::VRSettingsError_None;
  bool v = s->GetBool(kSection, "anyHmd", &e);
  r.any_hmd = e == vr::VRSettingsError_None && v;
  v = s->GetBool(kSection, "gravity", &e);
  r.gravity = e != vr::VRSettingsError_None || v;
  v = s->GetBool(kSection, "steadyStations", &e);
  r.steady = e != vr::VRSettingsError_None || v;
  v = s->GetBool(kSection, "record", &e);
  r.record = e == vr::VRSettingsError_None && v;
  char b[512] = "";
  s->GetString(kSection, "host", b, sizeof b, &e);
  if (e == vr::VRSettingsError_None) r.host = b;
  b[0] = 0;
  s->GetString(kSection, "headset", b, sizeof b, &e);
  if (e == vr::VRSettingsError_None) r.headset = b;
  return r;
}

// a setting the dashboard changed: saved to steamvr.vrsettings, and applied now where the driver can
void HandleRequest(const std::function<void()> &reload) {
  int seq = g_x->req_seq;
  if (seq == g_x->ack_seq) return;
  MemoryBarrier();
  char key[sizeof g_x->req_key], value[sizeof g_x->req_value];
  memcpy(key, g_x->req_key, sizeof key);
  memcpy(value, g_x->req_value, sizeof value);
  key[sizeof key - 1] = 0;
  value[sizeof value - 1] = 0;
  // live: what ReadSettings applies while running. anyHmd is read when SteamVR's headset turns up, and steadyStations
  // hooks SteamVR's lighthouse driver at start
  static const struct { const char *key; bool is_bool, live; } kKeys[] = {
      {"host", false, true},  {"headset", false, true},       {"gravity", true, true},
      {"record", true, true}, {"steadyStations", true, false}, {"anyHmd", true, false}};
  std::string ack;
  auto *s = vr::VRSettings();
  const auto *k = std::find_if(std::begin(kKeys), std::end(kKeys), [&](const auto &e) { return !strcmp(e.key, key); });
  if (k == std::end(kKeys)) {
    ack = std::string("Unknown setting ") + key;
  } else if (!s) {
    ack = "SteamVR's settings aren't available";
  } else {
    vr::EVRSettingsError e = vr::VRSettingsError_None;
    if (k->is_bool) s->SetBool(kSection, key, !strcmp(value, "1") || !strcmp(value, "true"), &e);
    else s->SetString(kSection, key, value, &e);
    if (e != vr::VRSettingsError_None) {
      ack = "SteamVR didn't save it (" + std::string(s->GetSettingsErrorNameFromEnum(e)) + ")";
    } else {
      if (k->live) reload();
      ack = k->live ? "Saved" : "Saved. Restart SteamVR to apply it";
      Log(std::string("setting ") + key + " = \"" + value + "\" (from the dashboard)");
    }
  }
  Copy(g_x->ack, sizeof g_x->ack, ack);
  MemoryBarrier();
  g_x->ack_seq = seq;
}

}  // namespace

void Init(LogFn log) {
  g_log = std::move(log);
  g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LhsExtra), LHS_SHM_NAME);
  if (g_map) g_x = (LhsExtra *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LhsExtra));
  if (g_x) {
    memset((void *)g_x, 0, sizeof(LhsExtra));
    g_x->magic = LHS_MAGIC;
    g_x->version = LHS_SHM_VERSION;
    Copy(g_x->driver_version, sizeof g_x->driver_version, LHS_VERSION);
    Copy(g_x->repo, sizeof g_x->repo, LHS_REPO);
    g_x->connected_for = g_x->heartbeat_age = -1;
  }
  // An update from the dashboard renames the files SteamVR has open aside (*.lhs-old), as they can't be replaced
  // while in use. Nothing has them open any more
  HMODULE mod = nullptr;
  wchar_t path[MAX_PATH];
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR)&Init, &mod) &&
      GetModuleFileNameW(mod, path, MAX_PATH)) {
    std::error_code ec;
    for (auto &f : std::filesystem::directory_iterator(std::filesystem::path(path).parent_path(), ec))
      if (f.path().extension().wstring().rfind(L".lhs-old", 0) == 0) std::filesystem::remove(f.path(), ec);  // and .lhs-old2...
  }
  Log(std::string(LHS_NAME " ") + LHS_VERSION + " (updates from github.com/" + LHS_REPO + ")");
}

void OnLine(double now, const std::string &line) {
  if (line.size() < 2 || line[1] != ' ') return;
  std::lock_guard<std::mutex> g(g_m);
  g_lines++;
  switch (line[0]) {
    case 'F': OnFrame(now, line); break;
    case 'T':
      g_heartbeat = line.substr(2);
      g_heartbeat_t = now;
      break;
    case 'I': OnInfo(line); break;
    case 'H':  // a new stream, maybe another headset
      g_module = Field(line, "module");
      g_cams.clear();
      g_heartbeat.clear();
      g_caminfo.clear();
      g_heartbeat_t = -1;
      break;
  }
}

void OnCalibration(const std::string &json) {
  JVal root;
  if (!JParse(json, root)) return;
  Optics optics;
  bool have = optics.Load(json, nullptr);
  std::lock_guard<std::mutex> g(g_m);
  auto size = [&](int id, int w, int h) {
    if (id < 0 || id >= kMaxCam || w <= 0 || h <= 0) return;
    g_cams[id].w = w;
    g_cams[id].h = h;
    if (have) g_cams[id].used = optics.Used(id) ? 1 : 0;
  };
  // the Steam Frame's: xrservice.json's cameras, in camera order
  const JVal *xr = root.get("xrservice"), *cams = xr ? xr->get("cameras") : nullptr;
  if (cams && cams->t == JVal::Arr)
    for (size_t i = 0; i < cams->a.size(); i++) {
      const JVal *w = cams->a[i].get("width"), *h = cams->a[i].get("height");
      if (w && h) size((int)i, (int)w->num(), (int)h->num());
    }
  // Meta's and the Galaxy XR's: CameraCalibration entries with an Id and an ImageSize
  for (const char *key : {"CameraCalibration", "GxrCameraCalibration"}) {
    const JVal *list = root.get(key);
    if (!list || list->t != JVal::Arr) continue;
    for (auto &c : list->a) {
      const JVal *id = c.get("Id"), *sz = c.get("ImageSize");
      if (!id || !sz) continue;
      auto v = sz->nums();
      if (v.size() >= 2) size(id->t == JVal::Str ? atoi(id->s.c_str()) : (int)id->num(-1), (int)v[0], (int)v[1]);
    }
  }
}

void Tick(double now, Sync &sync, const HeadsetLink &link, const std::function<void()> &reload) {
  if (!g_x) return;
  HandleRequest(reload);
  static double last_pub = 0, last_read = -10;
  static Settings settings;
  if (now - last_pub < 0.25) return;
  last_pub = now;
  if (now - last_read >= 1) {  // also picks up edits to steamvr.vrsettings while SteamVR runs
    last_read = now;
    settings = ReadSettings();
  }
  Sync::Spots sp = sync.spots();
  bool connected = link.state() == HeadsetLink::kConnected;
  double since = link.connected_at();

  std::lock_guard<std::mutex> g(g_m);
  if (now - g_rate_t >= 1) {
    double dt = g_rate_t > 0 ? now - g_rate_t : 0;
    for (auto &kv : g_cams) {
      Cam &c = kv.second;
      c.fps = dt > 0 ? (c.frames - c.frames_at) / dt : 0;
      c.blob_rate = dt > 0 ? (c.blobs - c.blobs_at) / dt : 0;
      c.frames_at = c.frames;
      c.blobs_at = c.blobs;
    }
    g_lps = dt > 0 ? (g_lines - g_lines_at) / dt : 0;
    g_lines_at = g_lines;
    g_rate_t = now;
  }
  LhsExtra *x = g_x;
  InterlockedIncrement((volatile LONG *)&x->seq);  // odd: writing
  MemoryBarrier();
  x->updated = now;
  Copy(x->module_version, sizeof x->module_version, g_module);
  x->connected_for = connected && since > 0 ? now - since : -1;
  x->lines_per_s = connected ? g_lps : 0;
  x->heartbeat_age = g_heartbeat_t >= 0 ? now - g_heartbeat_t : -1;
  Copy(x->heartbeat, sizeof x->heartbeat, g_heartbeat);
  Copy(x->camera_info, sizeof x->camera_info, g_caminfo);
  x->spot_frames = sp.frames;
  x->spot_seen = sp.seen;
  x->spot_used = sp.used;
  x->spot_still = sp.still;
  x->spot_fast = sp.fast;
  x->spot_big = sp.big;
  x->spot_other = sp.other;
  x->spot_dim = sp.dim;
  int n = 0;
  for (auto &kv : g_cams) {
    if (n == LHS_MAX_CAM) break;
    const Cam &c = kv.second;
    LhsCam &o = x->cam[n++];
    o.id = kv.first;
    o.w = c.w;
    o.h = c.h;
    o.used = c.used;
    o.last = c.last;
    o.fps = c.fps;
    o.blob_rate = c.blob_rate;
    o.mean = c.mean;
    o.nblobs = c.nblobs;
    o.nb = (int)c.b.size();
    if (o.nb) memcpy(o.b, c.b.data(), o.nb * sizeof(LhsBlob));
  }
  x->ncam = n;
  x->any_hmd = settings.any_hmd;
  x->gravity = settings.gravity;
  x->steady = settings.steady;
  x->record = settings.record;
  Copy(x->host, sizeof x->host, settings.host);
  Copy(x->headset_pref, sizeof x->headset_pref, settings.headset);
  MemoryBarrier();
  InterlockedIncrement((volatile LONG *)&x->seq);
}

}  // namespace lhs
