// lighthousesync-panel: LighthouseSync's page in the Steam Frame's SteamVR dashboard. It shows the PC dashboard's pages,
// which the PC draws for it and sends over the panel link (src/common/lhs_panel.h), and sends the pointer and the
// keyboard's text back, so everything runs on the PC as it does from the PC's own SteamVR dashboard. With no PC
// dashboard to show it shows its own page (panel_ui.cpp): lhsyncd and the headset's updater.
// lhs/install-frame.sh installs it with a systemd user unit that runs it alongside the headset's SteamVR, and a
// launcher for the SteamVR dashboard's "Launch program", which opens the panel when it's already running.
//   lighthousesync-panel             the panel (exits 3 after opening the one already running)
//   lighthousesync-panel --version
//   lighthousesync-panel --probe     whether the headset's SteamVR has the panel, without changing anything
//   lighthousesync-panel --pretend-visible [file]   for working on the link: acts as if it's open in the dashboard,
//                                    and writes the PC's last page to file (RGBA) when it exits
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

#include "icon.h"
#include "imgui.h"
#include "lhs_fork.h"
#include "lhs_panel.h"
#include "openvr.h"
#include "panel_ui.h"
#include "swr.h"
#include "theme.h"
#include "vk.h"

#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "../../third_party/qoi/qoi.h"

extern char **environ;

namespace lhs {

namespace {

const char *const kKey = "lighthousesync.panel";
const float kWidthM = 2.4f;  // as the PC's SteamVR page
const float kScale = 1.45f;  // its interface size
const int kDaemonPort = 47280, kDiscoveryPort = 47281;

volatile sig_atomic_t g_quit = 0, g_show = 0;
bool g_pretend = false;  // --pretend-visible
std::string g_dump;

void OnQuit(int) { g_quit = 1; }
void OnShow(int) { g_show = 1; }

double Now() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

void Sleep(double s) {
  for (double end = Now() + s; !g_quit && Now() < end;) usleep(50000);
}

std::string Env(const char *k, const std::string &def = "") {
  const char *v = getenv(k);
  return v && *v ? v : def;
}

bool Exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

// the file's first lines, without their line ends
std::vector<std::string> Lines(const std::string &path, int max) {
  std::vector<std::string> out;
  FILE *f = fopen(path.c_str(), "r");
  if (!f) return out;
  char b[512];
  while ((int)out.size() < max && fgets(b, sizeof b, f)) {
    std::string s = b;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    out.push_back(s);
  }
  fclose(f);
  return out;
}

std::string Line(const std::string &path) {
  std::vector<std::string> l = Lines(path, 1);
  return l.empty() ? "" : l[0];
}

// the addresses with a connection to lhsyncd (/proc/net/tcp has them in network byte order, as hex)
std::vector<uint32_t> DaemonPeers() {
  std::vector<uint32_t> out;
  FILE *f = fopen("/proc/net/tcp", "r");
  if (!f) return out;
  char line[512];
  if (!fgets(line, sizeof line, f)) line[0] = 0;  // the header
  while (fgets(line, sizeof line, f)) {
    unsigned la, lp, ra, rp, st;
    if (sscanf(line, " %*u: %8X:%4X %8X:%4X %2X", &la, &lp, &ra, &rp, &st) == 5 && lp == kDaemonPort && st == 1 &&
        std::find(out.begin(), out.end(), ra) == out.end())
      out.push_back(ra);
  }
  fclose(f);
  return out;
}

std::string Ip(uint32_t a) {
  char b[INET_ADDRSTRLEN] = "";
  in_addr x{a};
  inet_ntop(AF_INET, &x, b, sizeof b);
  return b;
}

pid_t Spawn(std::initializer_list<const char *> args) {
  std::vector<char *> argv;
  for (const char *a : args) argv.push_back((char *)a);
  argv.push_back(nullptr);
  pid_t pid = -1;
  if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ)) return -1;
  return pid;
}

int VrserverPid() {  // to notice SteamVR going away without saying so
  DIR *d = opendir("/proc");
  if (!d) return -1;
  int pid = -1;
  while (dirent *e = readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    if (Line(std::string("/proc/") + e->d_name + "/comm") == "vrserver") {
      pid = atoi(e->d_name);
      break;
    }
  }
  closedir(d);
  return pid;
}

struct Panel : panel_ui::Actions {
  // paths
  std::string lhs, units;

  // SteamVR
  vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid, thumb = vr::k_ulOverlayHandleInvalid;
  vk::Device dev;
  vk::Texture tex, thumb_tex;
  bool vr_up = false, visible = false, keyboard = false;
  int vrserver = -1;

  // the PC
  int lfd = -1, cfd = -1;
  bool hello = false, pc_frame = false;
  std::string in, out;
  double accepted = 0, last_rx = 0, last_tx = 0;
  swr::Image fb;  // the PC's page as far as it has sent it
  std::string other_version;
  double other_until = 0;

  // what the headset knows
  int udp = -1;
  panel_ui::State st;
  std::vector<uint32_t> peers;
  double started = 0, peers_since = 0, refreshed = -100, daemon_seen = 0;
  std::string status_sent;
  pid_t check_pid = -1;

  // its own page
  ImFontAtlas *atlas = nullptr;
  ImGuiContext *ctx = nullptr;
  swr::Image local;
  double local_drawn = -1, local_input = -100;
  bool local_dirty = true;
  float applied = 0;

  bool ShowingPc() const { return cfd >= 0 && hello && pc_frame; }

  // ---------------------------------------------------------------- SteamVR

  bool StartVr() {
    vr::EVRInitError e = vr::VRInitError_None;
    vr::VR_Init(&e, vr::VRApplication_Overlay);
    if (e != vr::VRInitError_None) {
      fprintf(stderr, "can't connect to SteamVR as an overlay: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
      return false;
    }
    vr_up = true;
    std::string err;
    if (!dev.Init(err)) {
      fprintf(stderr, "Vulkan: %s\n", err.c_str());
      return false;
    }
    vr::IVROverlay *o = vr::VROverlay();
    if (vr::EVROverlayError oe = o->CreateDashboardOverlay(kKey, LHS_NAME, &main, &thumb)) {
      fprintf(stderr, "CreateDashboardOverlay: %s\n", o->GetOverlayErrorNameFromEnum(oe));
      main = thumb = vr::k_ulOverlayHandleInvalid;
      return false;
    }
    o->SetOverlayWidthInMeters(main, kWidthM);
    o->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse);
    o->SetOverlayFlag(main, vr::VROverlayFlags_SendVRSmoothScrollEvents, true);
    vr::HmdVector2_t scale{(float)panel::kW, (float)panel::kH};
    o->SetOverlayMouseScale(main, &scale);
    if (!tex.Create(dev, panel::kW, panel::kH, err)) {
      fprintf(stderr, "the panel's texture: %s\n", err.c_str());
      return false;
    }
    swr::Image icon = ui::IconImage(ctx, atlas, 256);
    if (!thumb_tex.Create(dev, icon.w, icon.h, err) || !thumb_tex.Set(thumb, icon.px.data(), err))
      fprintf(stderr, "the icon: %s\n", err.c_str());
    vrserver = VrserverPid();
    fprintf(stderr, "in the SteamVR dashboard as %s\n", kKey);
    return true;
  }

  bool SteamVrAlive() const { return vrserver <= 0 || kill(vrserver, 0) == 0 || errno == EPERM; }

  void StopVr() {
    if (!vr_up) return;
    if (SteamVrAlive()) {
      if (main != vr::k_ulOverlayHandleInvalid) {
        vr::VROverlay()->ClearOverlayTexture(main);
        vr::VROverlay()->ClearOverlayTexture(thumb);
        vr::VROverlay()->DestroyOverlay(main);
        usleep(400000);  // a few frames for the compositor to let go of the images
      }
      vr::VR_Shutdown();
    }
    vr_up = false;
    tex.Destroy();  // only after VR_Shutdown
    thumb_tex.Destroy();
    dev.Destroy();
  }

  void Upload(const swr::Image &img) {
    std::string err;
    if (!tex.Set(main, img.px.data(), err)) fprintf(stderr, "the panel: %s\n", err.c_str());
  }

  void Pointer(uint8_t kind, uint8_t button, float x, float y) {
    if (ShowingPc()) {
      panel::Mouse m{kind, button, 0, x, y};
      Put(panel::kMouse, &m, sizeof m);
      return;
    }
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    switch (kind) {
      case panel::kMove: io.AddMousePosEvent(x, y); break;
      case panel::kDown:
      case panel::kUp:
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(button, kind == panel::kDown);
        break;
      case panel::kScroll: io.AddMouseWheelEvent(x, y); break;
      default: io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    local_input = Now();
  }

  void KeyboardDone() {  // it comes both as an overlay event and a global one
    if (!keyboard) return;
    keyboard = false;
    char b[256] = "";
    vr::VROverlay()->GetKeyboardText(b, sizeof b);
    Put(panel::kText, b, (uint32_t)strlen(b));
  }

  void VrEvents() {
    vr::VREvent_t ev;
    while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
      if (ev.eventType == vr::VREvent_Quit) {
        vr::VRSystem()->AcknowledgeQuit_Exiting();
        g_quit = 1;
      } else if (ev.eventType == vr::VREvent_KeyboardDone) {
        KeyboardDone();
      } else if (ev.eventType == vr::VREvent_KeyboardClosed) {
        keyboard = false;
      }
    }
    while (vr::VROverlay()->PollNextOverlayEvent(main, &ev, sizeof ev)) {
      float x = ev.data.mouse.x, y = panel::kH - ev.data.mouse.y;  // SteamVR's y is from the bottom
      uint8_t button = ev.data.mouse.button == vr::VRMouseButton_Right ? 1 : 0;
      switch (ev.eventType) {
        case vr::VREvent_MouseMove: Pointer(panel::kMove, 0, x, y); break;
        case vr::VREvent_MouseButtonDown:
        case vr::VREvent_MouseButtonUp:
          if (ev.data.mouse.button == vr::VRMouseButton_Middle) break;
          Pointer(ev.eventType == vr::VREvent_MouseButtonDown ? panel::kDown : panel::kUp, button, x, y);
          break;
        case vr::VREvent_ScrollSmooth:
        case vr::VREvent_ScrollDiscrete: Pointer(panel::kScroll, 0, ev.data.scroll.xdelta, ev.data.scroll.ydelta); break;
        case vr::VREvent_FocusLeave: Pointer(panel::kLeave, 0, 0, 0); break;
        case vr::VREvent_KeyboardDone: KeyboardDone(); break;
        case vr::VREvent_KeyboardClosed: keyboard = false; break;
        default: break;
      }
    }
  }

  // ---------------------------------------------------------------- the PC

  void Listen() {
    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(panel::kPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s < 0 || bind(s, (sockaddr *)&a, sizeof a) || listen(s, 2)) {
      if (s >= 0) close(s);
      return;  // tried again with the next refresh
    }
    lfd = s;
  }

  void Accept(double now) {
    sockaddr_in a{};
    socklen_t al = sizeof a;
    int s = accept4(lfd, (sockaddr *)&a, &al, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (s < 0) return;
    uint32_t ip = a.sin_addr.s_addr;
    std::vector<uint32_t> p = DaemonPeers();
    bool loopback = (ntohl(ip) >> 24) == 127;
    if (!loopback && std::find(p.begin(), p.end(), ip) == p.end()) {  // only a PC that lhsyncd serves
      fprintf(stderr, "refused %s: not connected to lhsyncd\n", Ip(ip).c_str());
      close(s);
      return;
    }
    if (cfd >= 0) CloseClient();  // the newest wins: the old one is likely gone
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    cfd = s;
    accepted = last_rx = last_tx = now;
    fprintf(stderr, "%s connected\n", Ip(ip).c_str());
  }

  void CloseClient() {
    if (cfd < 0) return;
    if (pc_frame) fprintf(stderr, "showing its own page\n");
    close(cfd);
    cfd = -1;
    hello = pc_frame = keyboard = false;
    in.clear();
    out.clear();
    status_sent.clear();
    local_dirty = true;
  }

  void Put(uint8_t type, const void *p, uint32_t n) {
    if (cfd < 0) return;
    panel::Header h{type, 0, 0, n};
    out.append((const char *)&h, sizeof h);
    if (n) out.append((const char *)p, n);
  }

  void Flush() {
    while (cfd >= 0 && !out.empty()) {
      ssize_t k = send(cfd, out.data(), out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
      if (k > 0) {
        out.erase(0, (size_t)k);
        last_tx = Now();
        continue;
      }
      if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      CloseClient();
    }
  }

  void Read(double now) {
    char buf[65536];
    for (;;) {
      ssize_t k = recv(cfd, buf, sizeof buf, MSG_DONTWAIT);
      if (k > 0) {
        in.append(buf, (size_t)k);
        last_rx = now;
        continue;
      }
      if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      return CloseClient();
    }
    size_t at = 0;
    while (in.size() - at >= sizeof(panel::Header)) {
      panel::Header h;
      memcpy(&h, in.data() + at, sizeof h);
      if (h.len > panel::kMaxPayload) return CloseClient();
      if (in.size() - at - sizeof h < h.len) break;
      Handle(h.type, in.data() + at + sizeof h, h.len, now);
      if (cfd < 0) return;
      at += sizeof h + h.len;
    }
    in.erase(0, at);
  }

  void Rect(const char *p, uint32_t n) {
    if (n <= sizeof(panel::Rect)) return;
    panel::Rect r;
    memcpy(&r, p, sizeof r);
    if (!r.w || !r.h || r.x + r.w > panel::kW || r.y + r.h > panel::kH) return;
    // its size before decoding: qoi_decode allocates whatever the header says, up to gigabytes
    const uint8_t *q = (const uint8_t *)p + sizeof r;
    auto be32 = [](const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; };
    if (n - sizeof r < 14 || be32(q + 4) != r.w || be32(q + 8) != r.h) return;
    qoi_desc d;
    uint8_t *px = (uint8_t *)qoi_decode(p + sizeof r, (int)(n - sizeof r), &d, 4);
    if (!px) return;
    if (d.width == r.w && d.height == r.h)
      for (int y = 0; y < r.h; y++)
        memcpy(&fb.px[(size_t)(r.y + y) * fb.w + r.x], px + (size_t)y * r.w * 4, (size_t)r.w * 4);
    free(px);
  }

  void Handle(uint8_t type, const char *p, uint32_t n, double now) {
    if (type == panel::kHello) {
      std::string text(p, n);
      int proto = 0;
      char version[64] = "";
      sscanf(text.c_str(), "proto=%d version=%63s", &proto, version);
      std::string mine = "proto=" + std::to_string(panel::kProto) + " version=" +
                         (st.installed.empty() ? std::string(LHS_VERSION) : st.installed);
      Put(panel::kHello, mine.data(), (uint32_t)mine.size());
      if (proto != panel::kProto) {  // another LighthouseSync: tell it, and say so on the page
        Flush();
        other_version = version;
        other_until = now + 330;  // it tries again in 300 s
        fprintf(stderr, "the PC's link is version %d (%s), this panel's %d\n", proto, version, panel::kProto);
        CloseClient();
        return;
      }
      hello = true;
      other_until = 0;
      uint8_t v = visible;
      Put(panel::kVisible, &v, 1);
      local_dirty = true;
      return;
    }
    if (!hello) return;
    switch (type) {
      case panel::kRect: Rect(p, n); break;
      case panel::kPresent:
        if (!pc_frame) {
          local_dirty = true;
          fprintf(stderr, "showing the PC's dashboard\n");
        }
        pc_frame = true;
        if (visible) Upload(fb);
        break;
      case panel::kKeyboard: {
        std::string title(p, strnlen(p, n)), value = title.size() < n ? std::string(p + title.size() + 1, n - title.size() - 1) : "";
        keyboard = vr::VROverlay()->ShowKeyboardForOverlay(main, vr::k_EGamepadTextInputModeNormal,
                                                           vr::k_EGamepadTextInputLineModeSingleLine,
                                                           vr::KeyboardFlag_Minimal | vr::KeyboardFlag_Modal,
                                                           title.c_str(), 255, value.c_str(), 0) == vr::VROverlayError_None;
        break;
      }
      case panel::kCheck: Check(); break;
      case panel::kInstall: Install(); break;
      default: break;  // kPing, and what a newer PC adds
    }
  }

  // ---------------------------------------------------------------- what the headset knows

  void Discovery() {
    char b[512];
    for (;;) {
      ssize_t k = recv(udp, b, sizeof b - 1, MSG_DONTWAIT);
      if (k <= 0) break;
      b[k] = 0;
      int v = 0, port = 0;
      char serial[128] = "", model[128] = "", state[32] = "";
      if (sscanf(b, "QLHS %d %127s %d %127s %31s", &v, serial, &port, model, state) != 5) continue;
      std::replace(model, model + strlen(model), '_', ' ');  // lhsyncd sends spaces as underscores
      st.serial = serial;
      st.model = model;
      st.daemon_state = state;
      daemon_seen = Now();
    }
  }

  void Refresh(double now) {
    refreshed = now;
    if (lfd < 0) Listen();
    peers = DaemonPeers();
    peers_since = peers.empty() ? 0 : peers_since ? peers_since : now;
    st.pcs.clear();
    for (uint32_t a : peers) st.pcs.push_back(Ip(a));
    if (udp >= 0) {
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(kDiscoveryPort);
      a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      sendto(udp, "QLHS?", 5, MSG_DONTWAIT, (sockaddr *)&a, sizeof a);
    }
    st.daemon = daemon_seen > 0 ? now - daemon_seen < 5 : now - started < 3;  // its first answer is on its way

    st.installed = Line(lhs + "/version");
    st.staged = Line(lhs + "/staged/version");
    st.updater = Exists(units + "/lighthousesync-update.service");
    st.auto_update = Exists(units + "/timers.target.wants/lighthousesync-update.timer");
    std::vector<std::string> c = Lines(lhs + "/checked", 2);  // install-frame.sh --stage: when, and what came of it
    st.checked = c.size() > 0 ? atof(c[0].c_str()) : 0;
    st.result = c.size() > 1 ? c[1] : "";
    st.checking = check_pid > 0;
    Link(now);

    std::string s = "installed=" + st.installed + "\nstaged=" + st.staged + "\nauto=" + (st.auto_update ? "1" : "0") +
                    "\nchecking=" + (st.checking ? "1" : "0") + "\nchecked=" + std::to_string((long long)st.checked) +
                    "\nresult=" + st.result + "\n";
    if (hello && s != status_sent) {
      Put(panel::kStatus, s.data(), (uint32_t)s.size());
      status_sent = s;
    }
  }

  void Link(double now) {
    int l = panel_ui::kNoPc;
    if (cfd >= 0 && hello) l = panel_ui::kLinked;
    else if (now < other_until && !peers.empty()) l = panel_ui::kOtherProto;
    else if (!peers.empty()) l = now - std::max(peers_since, started) < 20 ? panel_ui::kConnecting : panel_ui::kNoLink;
    st.link = l;
    st.pc_version = other_version;
  }

  void Reap(double now) {
    int status;
    for (pid_t pid; (pid = waitpid(-1, &status, WNOHANG)) > 0;)
      if (pid == check_pid) {
        check_pid = -1;
        Refresh(now);
        local_dirty = true;
      }
  }

  void Check() override {
    if (check_pid > 0 || !st.updater) return;
    check_pid = Spawn({"systemctl", "--user", "start", "lighthousesync-update.service"});
    Refresh(Now());
    local_dirty = true;
  }

  // install-frame.sh --apply, which restarts the headset's SteamVR and this with it, in a unit of its own: not
  // lighthousesync-apply.service, which is ordered before steamvr.service, so SteamVR's restart would wait for it to
  // finish while it waits for the restart (SteamVR stopped, never started again)
  void Install() override {
    if (st.staged.empty()) return;
    std::string script = lhs + "/install-frame.sh";
    Spawn({"systemd-run", "--user", "--no-block", "--collect", "--quiet", "--unit=lighthousesync-install",
           script.c_str(), "--apply"});
  }

  void RestartDaemon() override {
    Spawn({"systemctl", "--user", "restart", "questlhsync.service"});
    daemon_seen = Now();  // a few seconds to come back before it says it isn't answering
  }

  // ---------------------------------------------------------------- its own page

  void DrawLocal(double now) {
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)panel::kW, (float)panel::kH);
    io.DeltaTime = local_drawn > 0 ? (float)std::clamp(now - local_drawn, 0.001, 1.0) : 1.f / 60;
    if (applied != kScale) {
      ui::ApplyStyle(kScale);
      applied = kScale;
    }
    ui::AtlasNewFrame(atlas);
    ImGui::NewFrame();
    panel_ui::Draw(st, *this);
    ImGui::Render();
    swr::UpdateTextures(ImGui::GetDrawData());
    swr::Render(ImGui::GetDrawData(), local, ui::col::bg);
    local_drawn = now;
    local_dirty = false;
  }

  void MakeContext() {
    atlas = IM_NEW(ImFontAtlas)();
    ui::LoadFonts(atlas);
    ctx = ImGui::CreateContext(atlas);
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.ConfigInputTextCursorBlink = false;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    io.ConfigDebugHighlightIdConflicts = false;
    local.Resize(panel::kW, panel::kH);
    fb.Resize(panel::kW, panel::kH);
  }

  // ---------------------------------------------------------------- the loop

  int Run() {
    started = Now();
    std::string home = Env("HOME", "/home/steamos");
    lhs = home + "/.local/share/questlhsync/lhs";
    units = Env("XDG_CONFIG_HOME", home + "/.config") + "/systemd/user";
    MakeContext();
    if (!StartVr()) {
      StopVr();
      return 1;
    }
    udp = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    Refresh(Now());
    DrawLocal(Now());  // an image from the start: the dashboard doesn't open an overlay without one
    Upload(local);
    double alive_checked = 0;
    while (!g_quit) {
      pollfd pf[3] = {{lfd, POLLIN, 0}, {cfd, (short)(POLLIN | (out.empty() ? 0 : POLLOUT)), 0}, {udp, POLLIN, 0}};
      poll(pf, 3, visible ? 11 : 100);
      double now = Now();
      if (lfd >= 0 && (pf[0].revents & POLLIN)) Accept(now);
      if (cfd >= 0 && pf[1].revents) Read(now);
      if (udp >= 0 && (pf[2].revents & POLLIN)) Discovery();

      if (now - alive_checked > 1) {
        alive_checked = now;
        if (!SteamVrAlive()) {
          fprintf(stderr, "SteamVR went away\n");
          break;
        }
      }
      VrEvents();
      if (g_show) {
        g_show = 0;
        vr::VROverlay()->ShowDashboard(kKey);
      }

      if (cfd >= 0) {
        if (!hello && now - accepted > 5) CloseClient();  // not a LighthouseSync dashboard
        else if (now - last_rx > 15) CloseClient();       // gone quiet: the PC slept or left
        else if (now - last_tx > 5) Put(panel::kPing, nullptr, 0);
      }
      Reap(now);
      if (now - refreshed > 2) Refresh(now);  // the page redraws every second anyway

      bool v = g_pretend || vr::VROverlay()->IsOverlayVisible(main);
      if (v != visible) {
        visible = v;
        uint8_t b = v;
        Put(panel::kVisible, &b, 1);
        local_dirty = true;
        if (v && ShowingPc()) Upload(fb);
      }
      if (visible && !ShowingPc()) {
        bool busy = now - local_input < 0.5;
        if (local_dirty || now - local_drawn > (busy ? 1.0 / 30 : 1.0)) {
          Link(now);
          DrawLocal(now);
          Upload(local);
        }
      }
      Flush();
    }
    if (!g_dump.empty()) {
      if (FILE *f = fopen(g_dump.c_str(), "wb")) {
        fwrite(fb.px.data(), 4, fb.px.size(), f);
        fclose(f);
      }
    }
    if (cfd >= 0) CloseClient();
    StopVr();
    return 0;
  }
};

// one panel: a second start asks the running one to open (SIGUSR1) and exits 3. Returns false for the second
bool OnlyOne(int &fd) {
  std::string path = Env("XDG_RUNTIME_DIR", "/tmp") + "/lighthousesync-panel.lock";
  fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return true;
  if (flock(fd, LOCK_EX | LOCK_NB)) {
    char b[32] = "";
    if (pread(fd, b, sizeof b - 1, 0) > 0 && atoi(b) > 0) kill(atoi(b), SIGUSR1);
    close(fd);
    return false;
  }
  std::string pid = std::to_string(getpid());
  if (ftruncate(fd, 0) == 0) {
    ssize_t n = pwrite(fd, pid.data(), pid.size(), 0);
    (void)n;
  }
  return true;
}

// SteamVR is running: a Background app can't start it, unlike an Overlay one
bool WaitForSteamVr(bool forever) {
  double t0 = Now();
  int last = -1;
  while (!g_quit) {
    vr::EVRInitError e = vr::VRInitError_None;
    vr::VR_Init(&e, vr::VRApplication_Background);
    if (e == vr::VRInitError_None) {
      vr::VR_Shutdown();
      return true;
    }
    if (e != last) fprintf(stderr, "waiting for SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
    last = e;
    if (!forever && Now() - t0 > 20) return false;
    Sleep(2);
  }
  return false;
}

int Probe() {
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Background);
  if (e != vr::VRInitError_None) {
    printf("SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
    return 1;
  }
  vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
  vr::EVROverlayError oe = vr::VROverlay()->FindOverlay(kKey, &h);
  printf("panel: %s", vr::VROverlay()->GetOverlayErrorNameFromEnum(oe));
  if (oe == vr::VROverlayError_None) {  // not GetOverlayImageData: it crashes the caller on a Vulkan texture
    uint32_t w = 0, hh = 0;
    vr::VROverlay()->GetOverlayTextureSize(h, &w, &hh);
    printf(", image %ux%u, %s, dashboard %s", w, hh, vr::VROverlay()->IsOverlayVisible(h) ? "open" : "not open",
           vr::VROverlay()->IsDashboardVisible() ? "shown" : "hidden");
  }
  printf("\n");
  vr::VR_Shutdown();
  return oe == vr::VROverlayError_None ? 0 : 1;
}

}  // namespace

}  // namespace lhs

int main(int argc, char **argv) {
  using namespace lhs;
  if (argc > 1 && !strcmp(argv[1], "--version")) {
    printf("%s %s\n", LHS_NAME, LHS_VERSION);
    return 0;
  }
  // desktop mode's terminal points it at a nested session, where systemctl --user finds no systemd
  setenv("XDG_RUNTIME_DIR", ("/run/user/" + std::to_string(getuid())).c_str(), 1);
  if (argc > 1 && !strcmp(argv[1], "--probe")) return Probe();
  if (argc > 1 && !strcmp(argv[1], "--pretend-visible")) {
    g_pretend = true;
    g_dump = argc > 2 ? argv[2] : "";
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, OnQuit);
  signal(SIGINT, OnQuit);
  signal(SIGUSR1, OnShow);
  int lock = -1;
  if (!OnlyOne(lock)) return 3;
  bool systemd = getenv("INVOCATION_ID") != nullptr;
  if (!systemd && !g_pretend) g_show = 1;  // started from the dashboard's "Launch program": open when ready
  if (!WaitForSteamVr(systemd)) return 1;
  static Panel p;
  return p.Run();
}
