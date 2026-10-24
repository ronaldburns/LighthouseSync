// lighthousesync-panel: LighthouseSync's page in the Steam Frame's SteamVR dashboard, all from the headset itself
// (panel_ui.cpp): whether a PC is synced with lhsyncd, lhsyncd's log, the headset's updater and its settings. The PC's
// dashboard connects to it over the panel link (src/common/lhs_panel.h) only for the updater, for its Updates page.
// lhs/install-frame.sh installs it with a systemd user unit that runs it alongside the headset's SteamVR, and a
// launcher for the SteamVR dashboard's "Launch program", which opens the panel when it's already running.
//   lighthousesync-panel             the panel (exits 3 after opening the one already running)
//   lighthousesync-panel --version
//   lighthousesync-panel --probe     whether the headset's SteamVR has the panel, without changing anything
//   lighthousesync-panel --page <file>   the page as it is now, as RGBA pixels into file, and how long it takes to
//                                    draw, without SteamVR: safe alongside the running panel
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

extern char **environ;

namespace lhs {

namespace {

const char *const kKey = "lighthousesync.panel";
const int kW = 1600, kH = 1000;  // the page's pixels
const float kWidthM = 2.4f;      // as the PC's SteamVR page
const float kScale = 1.45f;      // its interface size
const int kDaemonPort = 47280, kDiscoveryPort = 47281;

volatile sig_atomic_t g_quit = 0, g_show = 0;

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

// stdout_fd: where the child's output goes (its errors go nowhere then), -1 for the panel's own
pid_t Spawn(std::initializer_list<const char *> args, int stdout_fd = -1) {
  std::vector<char *> argv;
  for (const char *a : args) argv.push_back((char *)a);
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (stdout_fd >= 0) {
    posix_spawn_file_actions_adddup2(&fa, stdout_fd, 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  }
  pid_t pid = -1;
  if (posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ)) pid = -1;
  posix_spawn_file_actions_destroy(&fa);
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
  bool vr_up = false, visible = false;
  int vrserver = -1;

  // the PC's dashboard
  int lfd = -1, cfd = -1;
  bool hello = false;
  std::string in, out, pc_version, status_sent;
  double accepted = 0, last_rx = 0, last_tx = 0, other_until = 0;

  // what the headset knows
  int udp = -1;
  panel_ui::State st;
  std::vector<uint32_t> peers;
  double started = 0, refreshed = -100, daemon_seen = 0;
  pid_t check_pid = -1, auto_pid = -1;

  // lhsyncd's journal, read while the page is open
  int log_fd = -1;
  std::string log_in, log_text;
  double log_asked = -100;

  // the page
  ImFontAtlas *atlas = nullptr;
  ImGuiContext *ctx = nullptr;
  swr::Image page;
  double drawn = -1, input = -100;
  bool dirty = true;
  float applied = 0;

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
    vr::HmdVector2_t scale{(float)kW, (float)kH};
    o->SetOverlayMouseScale(main, &scale);
    if (!tex.Create(dev, kW, kH, err)) {
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

  void Upload() {
    std::string err;
    if (!tex.Set(main, page.px.data(), err)) fprintf(stderr, "the panel: %s\n", err.c_str());
  }

  void VrEvents(double now) {
    vr::VREvent_t ev;
    while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
      if (ev.eventType == vr::VREvent_Quit) {
        vr::VRSystem()->AcknowledgeQuit_Exiting();
        g_quit = 1;
      }
    }
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    while (vr::VROverlay()->PollNextOverlayEvent(main, &ev, sizeof ev)) {
      float x = ev.data.mouse.x, y = kH - ev.data.mouse.y;  // SteamVR's y is from the bottom
      switch (ev.eventType) {
        case vr::VREvent_MouseMove: io.AddMousePosEvent(x, y); break;
        case vr::VREvent_MouseButtonDown:
        case vr::VREvent_MouseButtonUp:
          if (ev.data.mouse.button == vr::VRMouseButton_Middle) continue;
          io.AddMousePosEvent(x, y);
          io.AddMouseButtonEvent(ev.data.mouse.button == vr::VRMouseButton_Right ? 1 : 0,
                                 ev.eventType == vr::VREvent_MouseButtonDown);
          break;
        case vr::VREvent_ScrollSmooth:
        case vr::VREvent_ScrollDiscrete: io.AddMouseWheelEvent(ev.data.scroll.xdelta, ev.data.scroll.ydelta); break;
        case vr::VREvent_FocusLeave: io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); break;
        default: continue;
      }
      input = now;
      dirty = true;
    }
  }

  // ---------------------------------------------------------------- the PC's dashboard

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
    close(cfd);
    cfd = -1;
    hello = false;
    in.clear();
    out.clear();
    status_sent.clear();
    dirty = true;
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
        other_until = now + 330;  // it tries again in 300 s
        fprintf(stderr, "the PC's link is version %d (%s), this panel's %d\n", proto, version, panel::kProto);
        CloseClient();
        return;
      }
      hello = true;
      pc_version = version;
      other_until = 0;
      SendStatus();
      dirty = true;
      return;
    }
    if (!hello) return;
    switch (type) {
      case panel::kCheck: Check(); break;
      case panel::kInstall: Install(); break;
      default: break;  // kPing, v1.26.3's page images and keyboard, and what a newer PC adds
    }
  }

  void SendStatus() {
    std::string s = "installed=" + st.installed + "\nstaged=" + st.staged + "\nauto=" + (st.auto_update ? "1" : "0") +
                    "\nchecking=" + (st.checking ? "1" : "0") + "\nchecked=" + std::to_string((long long)st.checked) +
                    "\nresult=" + st.result + "\n";
    if (hello && s != status_sent) {
      Put(panel::kStatus, s.data(), (uint32_t)s.size());
      status_sent = s;
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
      if (st.serial != serial || st.model != model || st.daemon_state != state) dirty = true;
      st.serial = serial;
      st.model = model;
      st.daemon_state = state;
      daemon_seen = Now();
    }
  }

  void Refresh(double now) {
    refreshed = now;
    peers = DaemonPeers();
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
    st.pc_app = cfd >= 0 && hello ? pc_version : "";
    st.pc_other = now < other_until && !peers.empty();

    st.installed = Line(lhs + "/version");
    st.staged = Line(lhs + "/staged/version");
    st.updater = Exists(units + "/lighthousesync-update.service");
    if (auto_pid <= 0) st.auto_update = Exists(units + "/timers.target.wants/lighthousesync-update.timer");
    std::vector<std::string> c = Lines(lhs + "/checked", 2);  // install-frame.sh --stage: when, and what came of it
    st.checked = c.size() > 0 ? atof(c[0].c_str()) : 0;
    st.result = c.size() > 1 ? c[1] : "";
    st.checking = check_pid > 0;
    SendStatus();
  }

  void Reap(double now) {
    int status;
    for (pid_t pid; (pid = waitpid(-1, &status, WNOHANG)) > 0;) {
      if (pid == check_pid) check_pid = -1;
      else if (pid == auto_pid) auto_pid = -1;
      else continue;  // journalctl, systemctl restart, systemd-run
      Refresh(now);
      dirty = true;
    }
  }

  void Check() override {
    if (check_pid > 0 || !st.updater) return;
    check_pid = Spawn({"systemctl", "--user", "start", "lighthousesync-update.service"});
    Refresh(Now());
    dirty = true;
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

  // what install-frame.sh turns on and off, but its units stay, for Check now. Not --now for the apply service: with a
  // release downloaded, that would install it, and restart SteamVR, right away
  void SetAutoUpdate(bool on) override {
    if (!st.updater || auto_pid > 0) return;
    auto_pid = Spawn({"sh", "-c",
                      on ? "systemctl --user enable --now lighthousesync-update.timer; "
                           "systemctl --user enable lighthousesync-apply.service"
                         : "systemctl --user disable --now lighthousesync-update.timer; "
                           "systemctl --user disable lighthousesync-apply.service"});
    if (auto_pid > 0) st.auto_update = on;  // Refresh reads it back once it's done
    dirty = true;
  }

  // ---------------------------------------------------------------- lhsyncd's journal

  void LogStart() {
    int p[2];
    if (log_fd >= 0 || pipe2(p, O_CLOEXEC)) return;
    pid_t pid = Spawn({"journalctl", "--user", "--unit=questlhsync.service", "--boot=0", "--lines=40",
                       "--output=short-iso", "--no-hostname", "--no-pager", "--quiet"},
                      p[1]);
    close(p[1]);
    if (pid < 0) {
      close(p[0]);
      return;
    }
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    log_fd = p[0];
    log_in.clear();
    log_asked = Now();
  }

  void LogRead() {
    char b[16384];
    for (;;) {
      ssize_t k = read(log_fd, b, sizeof b);
      if (k > 0) {
        if (log_in.size() < (1u << 20)) log_in.append(b, (size_t)k);
        continue;
      }
      if (k < 0 && (errno == EAGAIN || errno == EINTR)) return;
      break;
    }
    close(log_fd);
    log_fd = -1;
    if (log_in == log_text) return;
    log_text = log_in;
    // "2026-10-24T01:43:00-05:00 lhsyncd[1899]: I lhsyncd: lhsight started"
    st.log.clear();
    for (size_t at = 0; at < log_text.size();) {
      size_t e = log_text.find('\n', at);
      if (e == std::string::npos) e = log_text.size();
      std::string l = log_text.substr(at, e - at);
      at = e + 1;
      size_t c = l.find("]: ");
      if (l.size() < 19 || l[10] != 'T' || c == std::string::npos) continue;
      panel_ui::LogLine x;
      x.time = l.substr(11, 8);
      x.text = l.substr(c + 3, 300);
      auto starts = [&](const char *p) { return x.text.compare(0, strlen(p), p) == 0; };
      size_t sd = l.find(" systemd[");
      if (sd != std::string::npos && sd < c) {  // the service's own lines, with its long description
        const char *unit = "questlhsync.service: ";
        if (starts("Started ")) x.text = "Service started";
        else if (starts("Stopping ")) x.text = "Service stopping";
        else if (starts("Stopped ")) x.text = "Service stopped";
        else if (starts("Failed to start ")) x.text = "Service failed to start", x.level = 2;
        else if (starts(unit) && x.text.find("Consumed ") != std::string::npos) continue;
        else if (starts(unit)) x.text.erase(0, strlen(unit)), x.level = 1;
      } else if (x.text.size() > 2 && x.text[1] == ' ' && strchr("IWE", x.text[0])) {  // lhsyncd's and lhsight's
        x.level = x.text[0] == 'E' ? 2 : x.text[0] == 'W' ? 1 : 0;
        x.text.erase(0, 2);
        if (starts("lhsyncd: ")) x.text.erase(0, 9);
      }
      st.log.push_back(x);
    }
    dirty = true;
  }

  // ---------------------------------------------------------------- the page

  void Draw(double now) {
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)kW, (float)kH);
    io.DeltaTime = drawn > 0 ? (float)std::clamp(now - drawn, 0.001, 1.0) : 1.f / 60;
    if (applied != kScale) {
      ui::ApplyStyle(kScale);
      applied = kScale;
    }
    ui::AtlasNewFrame(atlas);
    ImGui::NewFrame();
    panel_ui::Draw(st, *this);
    ImGui::Render();
    swr::UpdateTextures(ImGui::GetDrawData());
    swr::Render(ImGui::GetDrawData(), page, ui::col::bg);
    drawn = now;
    dirty = false;
  }

  void Setup() {
    started = Now();
    std::string home = Env("HOME", "/home/steamos");
    lhs = home + "/.local/share/questlhsync/lhs";
    units = Env("XDG_CONFIG_HOME", home + "/.config") + "/systemd/user";
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
    page.Resize(kW, kH);
    udp = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  }

  // ---------------------------------------------------------------- the loop

  int Run() {
    Setup();
    if (!StartVr()) {
      StopVr();
      return 1;
    }
    Listen();
    Refresh(Now());
    Draw(Now());  // an image from the start: the dashboard doesn't open an overlay without one
    Upload();
    double alive_checked = 0;
    while (!g_quit) {
      bool busy = Now() - input < 0.5;
      pollfd pf[4] = {{lfd, POLLIN, 0},
                      {cfd, (short)(POLLIN | (out.empty() ? 0 : POLLOUT)), 0},
                      {udp, POLLIN, 0},
                      {log_fd, POLLIN, 0}};
      poll(pf, 4, !visible ? 100 : busy ? 4 : 16);  // SteamVR's pointer has nothing to wait on
      double now = Now();
      if (lfd >= 0 && (pf[0].revents & POLLIN)) Accept(now);
      if (cfd >= 0 && pf[1].revents) Read(now);
      if (udp >= 0 && (pf[2].revents & POLLIN)) Discovery();
      if (log_fd >= 0 && pf[3].revents) LogRead();

      if (now - alive_checked > 1) {
        alive_checked = now;
        if (!SteamVrAlive()) {
          fprintf(stderr, "SteamVR went away\n");
          break;
        }
      }
      VrEvents(now);
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
      if (now - refreshed > (visible ? 1 : 2)) {
        if (lfd < 0) Listen();
        Refresh(now);
      }

      bool v = vr::VROverlay()->IsOverlayVisible(main);
      if (v != visible) {
        visible = v;
        dirty = true;
      }
      if (visible) {
        if (now - log_asked > 2) LogStart();
        // at once for the pointer, then while it settles, and every second for what changes by itself
        if (dirty || now - drawn > (busy ? 1.0 / 60 : 1.0)) {
          Draw(now);
          Upload();
        }
      }
      Flush();
    }
    if (cfd >= 0) CloseClient();
    StopVr();
    return 0;
  }

  // --page: as Run draws it, without SteamVR or the PC's dashboard
  int Page(const std::string &path) {
    Setup();
    Refresh(Now());
    LogStart();
    for (double end = Now() + 0.5; Now() < end;) {
      pollfd pf[2] = {{udp, POLLIN, 0}, {log_fd, POLLIN, 0}};
      poll(pf, 2, 20);
      if (udp >= 0 && (pf[0].revents & POLLIN)) Discovery();
      if (log_fd >= 0 && pf[1].revents) LogRead();
    }
    Reap(Now());
    Refresh(Now());
    const int n = 30;
    double t0 = Now();
    for (int i = 0; i < n; i++) Draw(Now());
    double ms = (Now() - t0) / n * 1000;
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
      fprintf(stderr, "can't write %s\n", path.c_str());
      return 1;
    }
    fwrite(page.px.data(), 4, page.px.size(), f);
    fclose(f);
    printf("%dx%d RGBA into %s, %.1f ms a frame\n", kW, kH, path.c_str(), ms);
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
  if (argc > 2 && !strcmp(argv[1], "--page")) {
    static Panel p;
    return p.Page(argv[2]);
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, OnQuit);
  signal(SIGINT, OnQuit);
  signal(SIGUSR1, OnShow);
  int lock = -1;
  if (!OnlyOne(lock)) return 3;
  bool systemd = getenv("INVOCATION_ID") != nullptr;
  if (!systemd) g_show = 1;  // started from the dashboard's "Launch program": open when ready
  if (!WaitForSteamVr(systemd)) return 1;
  static Panel p;
  return p.Run();
}
