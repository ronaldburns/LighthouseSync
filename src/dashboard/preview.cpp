// LighthouseSync dashboard: --preview renders every page from sample data into PNG files, without SteamVR or the
// driver (for working on the pages, and for screenshots).
#include "preview.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "imgui.h"
#include "swr.h"
#include "theme.h"
#include "ui.h"

namespace lhs {
namespace {

void Put(char *dst, size_t n, const char *s) { snprintf(dst, n, "%s", s); }

Model Sample(const char *kind) {
  Model m;
  m.now = 1000;
  m.have_status = true;
  m.stale = false;
  QlhsStatus &s = m.st;
  s.magic = QLHS_MAGIC;
  s.version = QLHS_VERSION;
  Put(s.hmd, sizeof s.hmd, "Steam Frame");
  Put(s.hmd_system, sizeof s.hmd_system, "steamlink");
  Put(s.headset, sizeof s.headset, "Steam Frame");
  Put(s.headset_addr, sizeof s.headset_addr, "192.168.1.107:47280");
  Put(s.headset_fw, sizeof s.headset_fw, "SteamOS 3.8");
  s.cam_fps = 60;
  s.spot_rate = 41;
  s.sight_rate = 38;
  s.rtt_ms = 4.2;
  s.expo_ms = 15.3;
  s.expo_learned = 1;
  s.nst = 2;
  Put(s.st[0].serial, sizeof s.st[0].serial, "LHB-1A2B3C4D");
  s.st[0].anchor = 1; s.st[0].support = 432; s.st[0].last_seen = 0.4; s.st[0].dist = 3.4;
  Put(s.st[1].serial, sizeof s.st[1].serial, "LHB-5E6F7A8B");
  s.st[1].measured = 1; s.st[1].support = 337; s.st[1].last_seen = 6.2; s.st[1].dist = 2.1;
  s.state = QLHS_LOCKED;
  s.locked = 1; s.cond = 1;
  s.yaw_deg = -5.76; s.t[0] = -1.071; s.t[1] = 2.124; s.t[2] = 2.432;
  s.med_deg = 0.14; s.nfit = 788; s.locked_for = 754; s.lag_cm = 0.3;
  const char *logs[] = {"21:53:10  seeded from saved stations: yaw -5.765 t [-1.0713, 2.1258, 2.4309] miss 0.0 cm",
                        "21:53:12  Steam Frame at 192.168.1.107: connected",
                        "21:53:12  camera calibration: xrservice",
                        "21:53:41  timing for steamlink: 15.3 ms (best fit 15.3 ms on 212 fast-head sightings)",
                        "21:55:02  lighthouse frame: SteamVR's is 0.75 deg from the reference"};
  for (auto l : logs) { Put(s.log[s.nlog % 8], 120, l); s.nlog++; }

  m.have_extra = true;
  LhsExtra &x = m.ex;
  Put(x.driver_version, sizeof x.driver_version, LHS_VERSION);
  Put(x.repo, sizeof x.repo, LHS_REPO);
  Put(x.module_version, sizeof x.module_version, "v1.26");
  x.updated = 500;
  x.connected_for = 2280;
  x.lines_per_s = 248;
  x.heartbeat_age = 0.6;
  Put(x.heartbeat, sizeof x.heartbeat, "T 1729713242 cams 4 fps 60.0 60.0 60.0 60.0 drop 0");
  Put(x.camera_info, sizeof x.camera_info,
      "XRService 2, cameras 0 /dev/video2 1280x960 bpl 1280, 1 /dev/video3 1280x960 bpl 1280, 2 /dev/video4 1280x960, 3 /dev/video5 1280x960");
  x.spot_frames = 548120; x.spot_seen = 92311; x.spot_used = 80544; x.spot_still = 6120; x.spot_fast = 2210;
  x.spot_big = 1508; x.spot_other = 1650; x.spot_dim = 279;
  x.ncam = 4;
  for (int i = 0; i < 4; i++) {
    LhsCam &c = x.cam[i];
    c.id = i; c.w = 1280; c.h = 960; c.used = i < 2 ? 1 : 0; c.last = 499.98; c.fps = 60;
    c.blob_rate = i < 2 ? 18 + i * 3 : 4; c.mean = 22 + i * 3;
    int n = i < 2 ? 9 + i * 4 : 3;
    c.nblobs = c.nb = n;
    for (int k = 0; k < n; k++) {
      float a = (float)k / n * 6.28f + i;
      c.b[k].x10 = (int16_t)((640 + cosf(a * 1.7f) * 380 + k * 13) * 10);
      c.b[k].y10 = (int16_t)((480 + sinf(a * 1.3f) * 300) * 10);
      c.b[k].npx = (int16_t)(6 + (k * 7) % 30);
      c.b[k].peak = (int16_t)(120 + (k * 37) % 135);
    }
  }
  x.gravity = 1; x.steady = 1; x.any_hmd = 0; x.record = 0;

  m.vr = true;
  auto dev = [&](int kind, const char *serial, const char *model, float px, float py, float pz, float yaw_deg, bool trk) {
    Device d;
    d.kind = kind; d.serial = serial; d.model = model; d.tracking = trk;
    d.p[0] = px; d.p[1] = py; d.p[2] = pz;
    float y = yaw_deg * 3.14159265f / 180.f, c = cosf(y), sn = sinf(y);
    float R[9] = {c, 0, sn, 0, 1, 0, -sn, 0, c};
    memcpy(d.R, R, sizeof R);
    m.devices.push_back(d);
  };
  dev(kDevHmd, "", "Steam Frame", 0.2f, 1.65f, 0.1f, 20, true);
  dev(kDevStation, "LHB-1A2B3C4D", "Valve SR Imp", -1.9f, 2.2f, -1.7f, -135, true);
  dev(kDevStation, "LHB-5E6F7A8B", "Valve SR Imp", 1.8f, 2.1f, 1.6f, 45, true);
  dev(kDevTracker, "LHR-1C266268", "VIVE Tracker", 0.1f, 1.0f, 0.15f, 0, true);
  dev(kDevTracker, "LHR-0F31A2B4", "VIVE Tracker", 0.3f, 0.1f, 0.3f, 0, true);
  dev(kDevTracker, "LHR-77D0E911", "VIVE Tracker", 0.05f, 0.1f, 0.2f, 0, false);
  m.have_play_area = true;
  float pa[4][3] = {{-1.5f, 0, -1.2f}, {1.5f, 0, -1.2f}, {1.5f, 0, 1.2f}, {-1.5f, 0, 1.2f}};
  memcpy(m.play_area, pa, sizeof pa);

  m.ble = 1;
  StationPower p1;
  p1.name = "LHB-1A2B3C4D"; p1.state = kPowerOn; p1.rssi = -58; p1.seen = 999; p1.raw = 0x0b; p1.read = 980;
  StationPower p2 = p1;
  p2.name = "LHB-5E6F7A8B"; p2.rssi = -71;
  StationPower p3 = p1;
  p3.name = "LHB-90AB12CD"; p3.state = kPowerSleep; p3.rssi = -84; p3.raw = 0x00;
  m.power = {p1, p2, p3};

  m.upd.state = kUpdAvailable;
  m.upd.latest = "v1.26.2";
  m.upd.published = "2026-10-23";
  m.upd.checked = "21:40";
  m.upd.notes =
      "## Fixes\n- The tracking view keeps base stations SteamVR lost for a moment.\n- Settings changed in VR save right "
      "away.\n\n## Upstream\n- QuestLHSync 1.27: faster first lock on the Steam Frame.";
  m.prefs.developer = true;
  m.data_dir = "C:\\Users\\you\\AppData\\Local\\QuestLHSync";
  for (int i = 0; i < 60; i++)
    m.log.push_back(ui::F("2026-10-23 21:%02d:%02d  sample log line %d: lighthouse frame, sightings, timing", 10 + i * 7 / 60, i * 7 % 60, i));

  if (!strcmp(kind, "acquiring")) {
    s.state = QLHS_ACQUIRING; s.locked = 0; s.med_deg = -1; s.nfit = 0; s.yaw_deg = 0; s.locked_for = -1;
    s.st[1].last_seen = -1; s.st[1].support = 0; s.expo_learned = 0;
    m.upd.state = kUpdCurrent;
  } else if (!strcmp(kind, "searching")) {
    s.state = QLHS_SEARCHING; s.headset_addr[0] = 0; s.cam_fps = 0; s.rtt_ms = 0; s.locked = 0; s.nfit = 0;
    s.med_deg = -1; s.yaw_deg = 0; s.locked_for = -1; x.module_version[0] = 0; x.ncam = 0;
    m.upd.state = kUpdIdle;
  } else if (!strcmp(kind, "stale")) {
    m.stale = true;
  }
  return m;
}

// ---------------------------------------------------------------- PNG (stored deflate)

uint32_t Crc(const uint8_t *d, size_t n, uint32_t c = 0xFFFFFFFFu) {
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return c;
}

struct NoActions : Actions {
  void Command(int) override {}
  void SetSetting(const char *, const std::string &) override {}
  void PrefsChanged() override {}
  void Power(const std::string &, int) override {}
  void Scan() override {}
  void CheckUpdates() override {}
  void InstallUpdate() override {}
  void OpenDataFolder() override {}
  void OpenUrl(const std::string &) override {}
  void Copy(const std::string &) override {}
  void EditText(const char *, const std::string &, const std::string &) override {}
};

}  // namespace

bool WritePng(const char *path, const swr::Image &img) {
  std::vector<uint8_t> raw;
  raw.reserve((size_t)img.h * (img.w * 4 + 1));
  for (int y = 0; y < img.h; y++) {
    raw.push_back(0);
    const uint8_t *row = (const uint8_t *)&img.px[(size_t)y * img.w];
    raw.insert(raw.end(), row, row + (size_t)img.w * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t o = 0; o < raw.size(); o += 65535) {
    size_t n = std::min<size_t>(65535, raw.size() - o);
    z.push_back((uint8_t)(o + n == raw.size()));
    z.push_back((uint8_t)(n & 255)); z.push_back((uint8_t)(n >> 8));
    z.push_back((uint8_t)(~n & 255)); z.push_back((uint8_t)((~n >> 8) & 255));
    z.insert(z.end(), raw.begin() + o, raw.begin() + o + n);
  }
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  uint32_t ad = (b << 16) | a;
  for (int i = 3; i >= 0; i--) z.push_back((ad >> (8 * i)) & 255);
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  auto be = [&](uint32_t v) { uint8_t q[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; fwrite(q, 1, 4, f); };
  auto chunk = [&](const char *t, const std::vector<uint8_t> &d) {
    be((uint32_t)d.size());
    std::vector<uint8_t> td(t, t + 4);
    td.insert(td.end(), d.begin(), d.end());
    fwrite(td.data(), 1, td.size(), f);
    be(Crc(td.data(), td.size()) ^ 0xFFFFFFFFu);
  };
  const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  int w = img.w, h = img.h;
  chunk("IHDR", {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16),
                 (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0});
  chunk("IDAT", z);
  chunk("IEND", {});
  fclose(f);
  return true;
}

int RunPreview(const char *dir, const char *kind, int w, int h, float scale) {
  ImFontAtlas *atlas = IM_NEW(ImFontAtlas)();
  ui::LoadFonts(atlas);
  ImGuiContext *ctx = ImGui::CreateContext(atlas);
  ImGui::SetCurrentContext(ctx);
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
  io.DisplaySize = ImVec2((float)w, (float)h);
  io.DeltaTime = 1.f / 30;
  ui::ApplyStyle(scale);
  Model m = Sample(kind);
  NoActions acts;
  swr::Image img;
  img.Resize(w, h);
  static const char *names[] = {"overview", "stations", "tracking", "settings", "updates", "cameras", "diagnostics", "log"};
  int fails = 0;
  for (int p = 0; p < ui::kPageCount; p++) {
    ui::View v;
    v.page = p;
    for (int f = 0; f < 3; f++) {
      ui::AtlasNewFrame(atlas);
      ImGui::NewFrame();
      ui::Draw(m, v, acts);
      ImGui::Render();
      swr::UpdateTextures(ImGui::GetDrawData());
    }
    swr::Render(ImGui::GetDrawData(), img, ui::col::bg);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s-%s.png", dir, kind, names[p]);
    if (!WritePng(path, img)) fails++;
  }
  ImGui::DestroyContext(ctx);
  IM_DELETE(atlas);
  return fails ? 1 : 0;
}

}  // namespace lhs
