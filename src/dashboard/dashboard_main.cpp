// QuestLHSync.exe: LighthouseSync's dashboard, a desktop window and a SteamVR dashboard page. The driver starts it
// each session by upstream's file name and it exits with SteamVR; started by hand while SteamVR isn't running it's a
// plain window (updates, base stations) that closes like any other.
// The pages are ui.cpp's, drawn by Dear ImGui into pixels on the CPU (swr.cpp): one ImGui context for the window, one
// for the SteamVR page and one for the headset's panel (panel_link.h, which sends it the pixels), sharing the fonts.
//   QuestLHSync.exe --preview <dir> [locked|acquiring|searching|stale|headset] [width height scale]   sample pages
//   QuestLHSync.exe --icon <file.png> [size]                     the icon (lhs/make_icon.py makes lhs/icon.ico)
//   QuestLHSync.exe --selftest <file.txt>     checks what it can without showing anything or touching SteamVR
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#undef small  // rpcndr.h (via d3d11.h): "#define small char"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <openvr.h>

#include "../driver/json.h"
#include "ble.h"
#include "icon.h"
#include "imgui.h"
#include "panel_link.h"
#include "preview.h"
#include "swr.h"
#include "theme.h"
#include "ui.h"
#include "updater.h"

using namespace lhs;

namespace {

const int kVrW = 1600, kVrH = 1000;  // the SteamVR page, in pixels
const float kVrScale = 1.45f;        // its interface size at 100%
const wchar_t *kClass = L"LighthouseSyncWindow";
const UINT WM_APP_SHOW = WM_APP + 1;  // another start while this one runs: show the window

double Now() {
  static LARGE_INTEGER f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)f.QuadPart;
}

std::wstring Wide(const std::string &s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
}

std::string Utf8(const std::wstring &w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

bool ServerUp() {  // vrserver.exe is running
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return true;
  PROCESSENTRY32W pe{sizeof pe};
  bool up = false;
  for (BOOL ok = Process32FirstW(snap, &pe); ok && !up; ok = Process32NextW(snap, &pe))
    up = !_wcsicmp(pe.szExeFile, L"vrserver.exe");
  CloseHandle(snap);
  return up;
}

std::wstring ExeDir() {
  wchar_t b[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH);
  std::wstring p(b, n);
  return p.substr(0, p.find_last_of(L"\\/"));
}

// where driver.vrdrivermanifest is: two up from bin\win64, where the driver starts us
std::wstring DriverRoot() {
  std::wstring d = ExeDir();
  for (int i = 0; i < 2; i++) d = d.substr(0, d.find_last_of(L"\\/"));
  return GetFileAttributesW((d + L"\\driver.vrdrivermanifest").c_str()) != INVALID_FILE_ATTRIBUTES ? d : ExeDir();
}

std::wstring DataDir() {  // the driver's (driver_main.cpp DataDir)
  wchar_t b[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", b, _countof(b));
  std::wstring d = (n ? std::wstring(b, n) : ExeDir()) + L"\\QuestLHSync";
  CreateDirectoryW(d.c_str(), nullptr);
  return d;
}

// ---------------------------------------------------------------- the driver's shared memory

template <class T>
struct Mapped {
  const wchar_t *name;
  T *p = nullptr;
  HANDLE h = nullptr;
  explicit Mapped(const wchar_t *n) : name(n) {}
  bool Open() {
    if (p) return true;
    h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!h) return false;
    p = (T *)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(T));
    if (!p) {
      CloseHandle(h);
      h = nullptr;
    }
    return p != nullptr;
  }
  // a consistent copy, under the seqlock
  bool Read(T &out) {
    if (!Open()) return false;
    for (int i = 0; i < 100; i++) {
      int32_t s = p->seq;
      if (s & 1) {
        Sleep(0);
        continue;
      }
      MemoryBarrier();
      memcpy(&out, (const void *)p, sizeof out);
      MemoryBarrier();
      if (p->seq == s) return true;
    }
    return false;
  }
};

// ---------------------------------------------------------------- the driver's log, its end

struct LogTail {
  std::wstring path;
  uint64_t size = ~0ull;
  FILETIME written{};

  void Poll(std::vector<std::string> &lines) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return;
    uint64_t sz = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    if (sz == size && !CompareFileTime(&a.ftLastWriteTime, &written)) return;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    uint64_t from = sz > (1u << 17) ? sz - (1u << 17) : 0;
    LARGE_INTEGER off;
    off.QuadPart = (LONGLONG)from;
    SetFilePointerEx(f, off, nullptr, FILE_BEGIN);
    std::string buf((size_t)(sz - from), '\0');
    DWORD got = 0;
    if (!buf.empty()) ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr);
    CloseHandle(f);
    buf.resize(got);
    size = sz;
    written = a.ftLastWriteTime;
    lines.clear();
    size_t at = 0;
    if (from) at = buf.find('\n') == std::string::npos ? buf.size() : buf.find('\n') + 1;  // a cut line
    while (at < buf.size()) {
      size_t e = buf.find('\n', at);
      if (e == std::string::npos) e = buf.size();
      std::string l = buf.substr(at, e - at);
      while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
      if (!l.empty()) lines.push_back(std::move(l));
      at = e + 1;
    }
    if (lines.size() > 1000) lines.erase(lines.begin(), lines.end() - 1000);
  }
};

// ---------------------------------------------------------------- dashboard.json

std::string Esc(const std::string &s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += (char)c;
    else if (c < 0x20) o += ui::F("\\u%04x", c);
    else o += (char)c;
  }
  return o;
}

void LoadPrefs(const std::wstring &path, Prefs &p) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  JVal v;
  if (!JParse(ss.str(), v) || v.t != JVal::Obj) return;
  auto flag = [&](const char *k, bool &out) {
    if (const JVal *e = v.get(k); e && e->t == JVal::Bool) out = e->b;
  };
  flag("open_window", p.open_window);
  flag("developer", p.developer);
  flag("auto_check", p.auto_check);
  flag("wake_stations", p.wake_stations);
  if (const JVal *e = v.get("ui_scale"); e && e->t == JVal::Num) p.ui_scale = std::clamp((float)e->n, 0.75f, 2.f);
  if (const JVal *e = v.get("on_quit"); e && e->t == JVal::Num) p.on_quit = std::clamp((int)e->n, 0, 2);
  if (const JVal *e = v.get("repo"); e && e->t == JVal::Str && ui::ValidRepo(e->s.c_str())) p.repo = e->s;
  if (const JVal *e = v.get("known_stations"); e && e->t == JVal::Arr)
    for (const JVal &s : e->a)
      if (s.t == JVal::Str && !s.s.empty() && s.s.size() < 32) p.known_stations.push_back(s.s);
}

void SavePrefs(const std::wstring &path, const Prefs &p) {
  std::string o = "{\n";
  o += ui::F("  \"ui_scale\": %.2f,\n", p.ui_scale);
  o += ui::F("  \"open_window\": %s,\n", p.open_window ? "true" : "false");
  o += ui::F("  \"developer\": %s,\n", p.developer ? "true" : "false");
  o += ui::F("  \"auto_check\": %s,\n", p.auto_check ? "true" : "false");
  o += "  \"repo\": \"" + Esc(p.repo) + "\",\n";
  o += ui::F("  \"wake_stations\": %s,\n", p.wake_stations ? "true" : "false");
  o += ui::F("  \"on_quit\": %d,\n", p.on_quit);
  o += "  \"known_stations\": [";
  for (size_t i = 0; i < p.known_stations.size(); i++) o += (i ? ", \"" : "\"") + Esc(p.known_stations[i]) + "\"";
  o += "]\n}\n";
  std::wstring tmp = path + L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << o;
    if (!f) return;
  }
  MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ---------------------------------------------------------------- clipboard

HWND g_hwnd;

std::string GetClipboard() {
  std::string out;
  if (!OpenClipboard(g_hwnd)) return out;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    if (const wchar_t *w = (const wchar_t *)GlobalLock(h)) {
      out = Utf8(w);
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;
}

void SetClipboard(const std::string &s) {
  std::wstring w = Wide(s);
  if (!OpenClipboard(g_hwnd)) return;
  EmptyClipboard();
  if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
    memcpy(GlobalLock(g), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(g);
    if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
  }
  CloseClipboard();
}

const char *ImGuiGetClipboard(ImGuiContext *) {
  static std::string buf;
  buf = GetClipboard();
  return buf.c_str();
}

void ImGuiSetClipboard(ImGuiContext *, const char *text) { SetClipboard(text ? text : ""); }

// ---------------------------------------------------------------- keys

ImGuiKey KeyOf(WPARAM vk) {
  switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case 'A': return ImGuiKey_A;
    case 'C': return ImGuiKey_C;
    case 'V': return ImGuiKey_V;
    case 'X': return ImGuiKey_X;
    case 'Y': return ImGuiKey_Y;
    case 'Z': return ImGuiKey_Z;
    default: return ImGuiKey_None;
  }
}

// ---------------------------------------------------------------- SteamVR

struct VrPage {
  vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid, thumb = vr::k_ulOverlayHandleInvalid;
  // the page goes up as a D3D11 texture, two of them in turn: SetOverlayRaw makes a new texture on every call and
  // the page blinks while SteamVR swaps it in
  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  ID3D11Texture2D *tex[2] = {};
  int flip = 0;

  bool Start(swr::Image &icon) {
    if (vr::VROverlay()->CreateDashboardOverlay("lighthousesync.dashboard", LHS_NAME, &main, &thumb) !=
        vr::VROverlayError_None)
      return false;
    vr::VROverlay()->SetOverlayWidthInMeters(main, 2.4f);
    vr::VROverlay()->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse);
    vr::VROverlay()->SetOverlayFlag(main, vr::VROverlayFlags_SendVRSmoothScrollEvents, true);
    vr::HmdVector2_t scale{(float)kVrW, (float)kVrH};
    vr::VROverlay()->SetOverlayMouseScale(main, &scale);
    vr::VROverlay()->SetOverlayRaw(thumb, icon.px.data(), icon.w, icon.h, 4);
    int32_t adapter = -1;
    vr::VRSystem()->GetDXGIOutputInfo(&adapter);
    IDXGIFactory1 *fac = nullptr;
    IDXGIAdapter1 *ad = nullptr;
    if (adapter >= 0 && SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac))) fac->EnumAdapters1(adapter, &ad);
    D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                      D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (ad) ad->Release();
    if (fac) fac->Release();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kVrW;
    td.Height = kVrH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // swr's byte order
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (auto &t : tex)
      if (dev) dev->CreateTexture2D(&td, nullptr, &t);
    return true;
  }

  void Show(swr::Image &img) {
    if (tex[0] && tex[1]) {
      ID3D11Texture2D *t = tex[flip ^= 1];
      ctx->UpdateSubresource(t, 0, nullptr, img.px.data(), img.w * 4, 0);
      ctx->Flush();
      vr::Texture_t vt{t, vr::TextureType_DirectX, vr::ColorSpace_Gamma};
      vr::VROverlay()->SetOverlayTexture(main, &vt);
    } else {
      vr::VROverlay()->SetOverlayRaw(main, img.px.data(), img.w, img.h, 4);
    }
  }

  void Release() {  // after VR_Shutdown, which takes the overlays with it
    for (auto &t : tex)
      if (t) t->Release(), t = nullptr;
    if (ctx) ctx->Release(), ctx = nullptr;
    if (dev) dev->Release(), dev = nullptr;
    main = thumb = vr::k_ulOverlayHandleInvalid;
  }
};

std::string Prop(vr::TrackedDeviceIndex_t i, vr::ETrackedDeviceProperty p) {
  char b[256] = "";
  vr::VRSystem()->GetStringTrackedDeviceProperty(i, p, b, sizeof b);
  return b;
}

// ---------------------------------------------------------------- the app

struct App : Actions {
  Model m;
  ui::View desk_view, vr_view;
  ImFontAtlas *atlas = nullptr;
  ImGuiContext *desk_ctx = nullptr, *vr_ctx = nullptr, *remote_ctx = nullptr;
  float desk_applied = 0, vr_applied = 0;  // the scale each context's style was made for
  swr::Image desk_img, vr_img, icon;
  std::vector<uint32_t> bgra;  // desk_img for GDI
  HWND hwnd = nullptr;
  float dpi = 1;
  bool managed = false;  // started while SteamVR runs: lives as long as it, and closing only hides the window
  bool quit = false, mouse_in = false;
  ImGuiMouseCursor cursor = ImGuiMouseCursor_Arrow;
  double desk_input = -1, vr_input = -1, desk_drawn = -1, vr_drawn = -1;

  Mapped<QlhsStatus> shm{QLHS_SHM_NAME};
  Mapped<LhsExtra> xshm{LHS_SHM_NAME};
  double last_upd = -1, changed_at = 0, xlast_upd = -1, xchanged_at = 0;
  std::deque<std::pair<std::string, std::string>> requests;  // setting changes waiting for the driver
  std::string req_key;  // the one it's handling
  int32_t req_seq = 0;
  double req_at = 0;

  bool server_up = false, vr_up = false;
  double vr_retry = 0, devices_at = -1, server_at = -1;
  bool devices_refresh = true;
  struct Cached { int cls = -1; std::string serial, model; } cache[vr::k_unMaxTrackedDeviceCount];
  VrPage page;
  std::string kb_key;  // the setting the SteamVR keyboard is open for

  LogTail log;
  std::wstring data_dir, prefs_path;
  double log_at = -1, check_at = 0;
  bool woke = false;

  // the headset's panel: the same pages, drawn here at its size and sent to it
  struct Remote : Actions {  // what its pages ask for, done here as for the others
    App &a;
    explicit Remote(App &app) : a(app) {}
    void Command(int cmd) override { a.Command(cmd); }
    void SetSetting(const char *key, const std::string &value) override { a.SetSetting(key, value); }
    void PrefsChanged() override { a.PrefsChanged(); }
    void Power(const std::string &name, int cmd) override { a.Power(name, cmd); }
    void Scan() override { a.Scan(); }
    void CheckUpdates() override { a.CheckUpdates(); }
    void InstallUpdate() override { a.InstallUpdate(); }
    void HeadsetCheck() override { a.HeadsetCheck(); }
    void HeadsetInstall() override { a.HeadsetInstall(); }
    void OpenDataFolder() override {}  // folders, links and the clipboard are the PC's: the headset view hides them
    void OpenUrl(const std::string &) override {}
    void Copy(const std::string &) override {}
    void EditText(const char *key, const std::string &title, const std::string &value) override {
      a.remote_kb_key = key;  // the headset's keyboard; its text comes back through the link
      a.link.Keyboard(title, value);
    }
  } remote{*this};
  PanelLink link;
  ui::View remote_view;
  swr::Image remote_img;
  float remote_applied = 0;
  double remote_input = -1, remote_drawn = -1;
  std::string remote_kb_key;

  // ---- Actions
  void Command(int cmd) override {
    if (!shm.p) return;
    shm.p->cmd = cmd;
    MemoryBarrier();
    InterlockedIncrement((volatile LONG *)&shm.p->cmd_seq);
  }

  void SetSetting(const char *key, const std::string &value) override {
    if (!m.have_extra || m.stale) {
      Ack(key, "The driver isn't running, so it can't be saved now.");
      return;
    }
    requests.emplace_back(key, value);
  }

  void PrefsChanged() override {
    SavePrefs(prefs_path, m.prefs);
    desk_applied = vr_applied = remote_applied = 0;  // the interface size may have changed
  }

  void Power(const std::string &name, int cmd) override { ble::Power(name, cmd); }
  void Scan() override { ble::Scan(); }
  void CheckUpdates() override { upd::Check(m.prefs.repo); }
  void InstallUpdate() override { upd::Install(); }
  void HeadsetCheck() override { link.Check(); }
  void HeadsetInstall() override { link.Install(); }

  void OpenDataFolder() override {
    CreateDirectoryW(data_dir.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", data_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }

  void OpenUrl(const std::string &url) override {
    if (url.rfind("https://", 0) == 0) ShellExecuteW(nullptr, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }

  void Copy(const std::string &text) override { SetClipboard(text); }

  void EditText(const char *key, const std::string &title, const std::string &value) override {
    if (!vr_up) return;
    kb_key = key;
    vr::VROverlay()->ShowKeyboardForOverlay(page.main, vr::k_EGamepadTextInputModeNormal,
                                            vr::k_EGamepadTextInputLineModeSingleLine,
                                            vr::KeyboardFlag_Minimal | vr::KeyboardFlag_Modal, title.c_str(), 255,
                                            value.c_str(), 0);
  }

  void Ack(const std::string &key, const std::string &text) {
    m.ack_key = key;
    m.ack_text = text;
    m.ack_at = m.now;
  }

  // the SteamVR keyboard's answer
  void KeyboardText(const std::string &key, std::string text) {
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text[0] == ' ') text.erase(0, 1);
    if (key == "repo") {
      if (!ui::ValidRepo(text.c_str())) {
        Ack(key, "That isn't owner/name, like " LHS_REPO ".");
        return;
      }
      m.prefs.repo = text;
      PrefsChanged();
      CheckUpdates();
      Ack(key, "Saved");
    } else {
      SetSetting(key.c_str(), text);
    }
  }

  // ---- the driver
  void ReadDriver() {
    bool have = shm.Read(m.st) && m.st.magic == QLHS_MAGIC && m.st.version == QLHS_VERSION;
    if (!have) memset(&m.st, 0, sizeof m.st);
    if (m.st.updated != last_upd) last_upd = m.st.updated, changed_at = m.now;
    m.have_status = have;
    m.stale = !have || m.now - changed_at > 3;

    bool xhave = xshm.Read(m.ex) && m.ex.magic == LHS_MAGIC && m.ex.version == LHS_SHM_VERSION;
    if (m.ex.updated != xlast_upd) xlast_upd = m.ex.updated, xchanged_at = m.now;
    m.have_extra = xhave && m.now - xchanged_at <= 3;
    if (!xhave) memset(&m.ex, 0, sizeof m.ex);

    // setting changes, one at a time: the driver reads one request per tick
    LhsExtra *x = xshm.p;
    if (!req_key.empty()) {
      if (x && x->ack_seq == req_seq) {
        MemoryBarrier();
        char a[sizeof x->ack];
        memcpy(a, x->ack, sizeof a);
        a[sizeof a - 1] = 0;
        if (strstr(a, "Restart") && std::find(m.restart_keys.begin(), m.restart_keys.end(), req_key) == m.restart_keys.end())
          m.restart_keys.push_back(req_key);
        Ack(req_key, a);
        req_key.clear();
      } else if (!x || m.now - req_at > 3) {
        Ack(req_key, "The driver didn't answer. Is SteamVR still running?");
        req_key.clear();
      }
    }
    if (req_key.empty() && !requests.empty() && x && m.have_extra) {
      auto [key, value] = requests.front();
      requests.pop_front();
      strncpy_s(x->req_key, key.c_str(), _TRUNCATE);
      strncpy_s(x->req_value, value.c_str(), _TRUNCATE);
      MemoryBarrier();
      req_seq = InterlockedIncrement((volatile LONG *)&x->req_seq);
      req_key = key;
      req_at = m.now;
    }

    // base stations SteamVR has shown here, for waking them before it shows them again
    bool added = false;
    for (int i = 0; i < m.st.nst && i < 8; i++) {
      std::string s(m.st.st[i].serial, strnlen(m.st.st[i].serial, sizeof m.st.st[i].serial));
      if (s.rfind("LHB-", 0) == 0 && std::find(m.prefs.known_stations.begin(), m.prefs.known_stations.end(), s) ==
                                         m.prefs.known_stations.end()) {
        m.prefs.known_stations.push_back(s);
        added = true;
      }
    }
    if (added) SavePrefs(prefs_path, m.prefs);
  }

  // ---- SteamVR
  void StartVr() {
    // as upstream: only once the driver reports SteamVR's headset (VR_Init without one is refused), and never while
    // SteamVR is down (VR_Init would start it)
    if (vr_up || m.now < vr_retry || !server_up || m.stale || !m.st.hmd[0]) return;
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Overlay);
    if (err == vr::VRInitError_None && page.Start(icon)) {
      vr_up = true;
      devices_refresh = true;
      vr_drawn = -1;
    } else {
      if (err == vr::VRInitError_None) vr::VR_Shutdown();
      page.Release();
      vr_retry = m.now + 5;
    }
  }

  void StopVr() {
    if (!vr_up) return;
    vr::VR_Shutdown();
    page.Release();
    vr_up = false;
    kb_key.clear();
    m.devices.clear();
    m.have_play_area = false;
  }

  void PollVr() {
    vr::VREvent_t ev;
    while (vr_up && vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
      switch (ev.eventType) {
        case vr::VREvent_Quit:
          vr::VRSystem()->AcknowledgeQuit_Exiting();
          StopVr();
          if (managed) quit = true;
          return;
        case vr::VREvent_TrackedDeviceActivated:
        case vr::VREvent_TrackedDeviceDeactivated:
        case vr::VREvent_TrackedDeviceUpdated: devices_refresh = true; break;
        case vr::VREvent_KeyboardDone: KeyboardDone(); break;
      }
    }
    if (!vr_up) return;
    ImGui::SetCurrentContext(vr_ctx);
    ImGuiIO &io = ImGui::GetIO();
    while (vr::VROverlay()->PollNextOverlayEvent(page.main, &ev, sizeof ev)) {
      float mx = ev.data.mouse.x, my = kVrH - ev.data.mouse.y;  // overlay mouse y is from the bottom
      switch (ev.eventType) {
        case vr::VREvent_MouseMove:
          io.AddMousePosEvent(mx, my);
          vr_input = m.now;
          break;
        case vr::VREvent_MouseButtonDown:
        case vr::VREvent_MouseButtonUp:
          io.AddMousePosEvent(mx, my);
          io.AddMouseButtonEvent(ev.data.mouse.button == vr::VRMouseButton_Right ? 1 : 0,
                                 ev.eventType == vr::VREvent_MouseButtonDown);
          vr_input = m.now;
          break;
        case vr::VREvent_ScrollSmooth:
          io.AddMouseWheelEvent(ev.data.scroll.xdelta, ev.data.scroll.ydelta);
          vr_input = m.now;
          break;
        case vr::VREvent_FocusLeave:
        case vr::VREvent_OverlayHidden:
          io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
          vr_input = m.now;
          break;
        case vr::VREvent_OverlayShown: vr_drawn = -1; break;
        case vr::VREvent_KeyboardDone: KeyboardDone(); break;
      }
    }
  }

  void KeyboardDone() {  // it comes both as an overlay event and a global one
    if (kb_key.empty()) return;
    char b[256] = "";
    vr::VROverlay()->GetKeyboardText(b, sizeof b);
    std::string key = kb_key;
    kb_key.clear();
    KeyboardText(key, b);
  }

  // the headset's panel: its pointer and keyboard into its context, its updater into the model
  void TakeRemote() {
    ImGui::SetCurrentContext(remote_ctx);
    ImGuiIO &io = ImGui::GetIO();
    for (const panel::Mouse &e : link.mouse) {
      switch (e.kind) {
        case panel::kMove: io.AddMousePosEvent(e.x, e.y); break;
        case panel::kDown:
        case panel::kUp:
          io.AddMousePosEvent(e.x, e.y);
          io.AddMouseButtonEvent(e.button ? 1 : 0, e.kind == panel::kDown);
          break;
        case panel::kScroll: io.AddMouseWheelEvent(e.x, e.y); break;
        default: io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); break;
      }
      remote_input = m.now;
    }
    link.mouse.clear();
    for (const std::string &t : link.texts) {
      if (remote_kb_key.empty()) continue;
      std::string key = remote_kb_key;
      remote_kb_key.clear();
      KeyboardText(key, t);
    }
    link.texts.clear();
    if (!link.info.connected) remote_kb_key.clear();
    m.panel = link.info;
  }

  void ReadDevices() {
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
    m.devices.clear();
    for (uint32_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
      Cached &c = cache[i];
      if (!poses[i].bDeviceIsConnected) {
        c.cls = -1;
        continue;
      }
      if (c.cls < 0 || devices_refresh) {
        c.cls = vr::VRSystem()->GetTrackedDeviceClass(i);
        c.serial = Prop(i, vr::Prop_SerialNumber_String);
        c.model = Prop(i, vr::Prop_ModelNumber_String);
      }
      Device d;
      switch (c.cls) {
        case vr::TrackedDeviceClass_HMD: d.kind = kDevHmd; break;
        case vr::TrackedDeviceClass_TrackingReference: d.kind = kDevStation; break;
        case vr::TrackedDeviceClass_Controller: d.kind = kDevController; break;
        case vr::TrackedDeviceClass_GenericTracker: d.kind = kDevTracker; break;
        default: continue;
      }
      d.serial = c.serial;
      d.model = c.model;
      d.tracking = poses[i].bPoseIsValid && poses[i].eTrackingResult == vr::TrackingResult_Running_OK;
      const auto &mm = poses[i].mDeviceToAbsoluteTracking.m;
      for (int r = 0; r < 3; r++) {
        d.p[r] = mm[r][3];
        for (int k = 0; k < 3; k++) d.R[r * 3 + k] = mm[r][k];
      }
      m.devices.push_back(std::move(d));
    }
    devices_refresh = false;
    vr::HmdQuad_t q;
    m.have_play_area = vr::VRChaperone() && vr::VRChaperone()->GetPlayAreaRect(&q);
    if (m.have_play_area)
      for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++) m.play_area[i][k] = q.vCorners[i].v[k];
  }

  // ---- drawing
  ImGuiContext *MakeContext() {
    ImGuiContext *c = ImGui::CreateContext(atlas);
    ImGui::SetCurrentContext(c);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.ConfigInputTextCursorBlink = false;  // the window draws a few times a second when idle
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    io.ConfigDebugHighlightIdConflicts = false;
    ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
    pio.Platform_GetClipboardTextFn = ImGuiGetClipboard;
    pio.Platform_SetClipboardTextFn = ImGuiSetClipboard;
    return c;
  }

  void Frame(ImGuiContext *ctx, ui::View &v, swr::Image &img, float scale, float &applied, double &drawn, Actions &acts) {
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)img.w, (float)img.h);
    io.DeltaTime = drawn > 0 ? (float)std::clamp(m.now - drawn, 0.001, 1.0) : 1.f / 60;
    if (applied != scale) {
      ui::ApplyStyle(scale);
      applied = scale;
    }
    ui::AtlasNewFrame(atlas);
    ImGui::NewFrame();
    ui::Draw(m, v, acts);
    ImGui::Render();
    swr::UpdateTextures(ImGui::GetDrawData());
    swr::Render(ImGui::GetDrawData(), img, ui::col::bg);
    drawn = m.now;
  }

  HICON MakeHIcon(int n) {
    swr::Image img = ui::IconImage(desk_ctx, atlas, n);
    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof bh;
    bh.bV5Width = n;
    bh.bV5Height = -n;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    void *bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    uint32_t *px = (uint32_t *)bits;
    for (size_t i = 0; i < img.px.size(); i++) {
      uint32_t v = img.px[i];
      px[i] = (v & 0xFF00FF00u) | ((v & 255) << 16) | ((v >> 16) & 255);
    }
    std::vector<uint8_t> mask((size_t)((n + 15) / 16) * 2 * n, 0);
    ICONINFO ii{TRUE, 0, 0, CreateBitmap(n, n, 1, 1, mask.data()), color};
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmMask);
    DeleteObject(color);
    return ic;
  }

  float DeskScale() const { return dpi * m.prefs.ui_scale; }

  void DrawDesktop() {
    if (desk_img.w <= 0 || desk_img.h <= 0) return;
    Frame(desk_ctx, desk_view, desk_img, DeskScale(), desk_applied, desk_drawn, *this);
    cursor = ImGui::GetMouseCursor();
    if (mouse_in) SetCursorNow();
    bgra.resize(desk_img.px.size());
    const uint32_t *src = desk_img.px.data();
    for (size_t i = 0; i < bgra.size(); i++) {
      uint32_t v = src[i];
      bgra[i] = (v & 0xFF00FF00u) | ((v & 255) << 16) | ((v >> 16) & 255);
    }
    HDC dc = GetDC(hwnd);
    Blit(dc);
    ReleaseDC(hwnd, dc);
  }

  void Blit(HDC dc) {
    if (bgra.size() != (size_t)desk_img.w * desk_img.h || bgra.empty()) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = desk_img.w;
    bi.bmiHeader.biHeight = -desk_img.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    SetDIBitsToDevice(dc, 0, 0, desk_img.w, desk_img.h, 0, 0, 0, desk_img.h, bgra.data(), &bi, DIB_RGB_COLORS);
  }

  void SetCursorNow() {
    LPCWSTR c = IDC_ARROW;
    switch (cursor) {
      case ImGuiMouseCursor_TextInput: c = IDC_IBEAM; break;
      case ImGuiMouseCursor_Hand: c = IDC_HAND; break;
      case ImGuiMouseCursor_ResizeEW: c = IDC_SIZEWE; break;
      case ImGuiMouseCursor_ResizeNS: c = IDC_SIZENS; break;
      case ImGuiMouseCursor_NotAllowed: c = IDC_NO; break;
      default: break;
    }
    SetCursor(LoadCursorW(nullptr, c));
  }

  ImGuiIO &DeskIO() {
    ImGui::SetCurrentContext(desk_ctx);
    return ImGui::GetIO();
  }

  void ShowWindowNow() {
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    else ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    desk_drawn = -1;
  }

  LRESULT Proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (!hwnd) hwnd = h;  // messages during CreateWindowExW
    switch (msg) {
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (bgra.size() == (size_t)desk_img.w * desk_img.h && !bgra.empty()) Blit(dc);
        else FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(h, &ps);
        return 0;
      }
      case WM_ERASEBKGND: return 1;
      case WM_SIZE:
        if (w != SIZE_MINIMIZED && LOWORD(l) > 0 && HIWORD(l) > 0 && (LOWORD(l) != desk_img.w || HIWORD(l) != desk_img.h)) {
          desk_img.Resize(LOWORD(l), HIWORD(l));
          m.now = Now();
          DrawDesktop();  // also while the edge is dragged: the main loop waits for the sizing to end
        }
        return 0;
      case WM_GETMINMAXINFO: {
        MINMAXINFO *mi = (MINMAXINFO *)l;
        mi->ptMinTrackSize.x = (LONG)(860 * dpi);
        mi->ptMinTrackSize.y = (LONG)(560 * dpi);
        return 0;
      }
      case WM_DPICHANGED: {
        dpi = HIWORD(w) / 96.f;
        RECT *r = (RECT *)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        desk_drawn = -1;
        return 0;
      }
      case WM_MOUSEMOVE:
        if (!mouse_in) {
          TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
          TrackMouseEvent(&t);
          mouse_in = true;
        }
        DeskIO().AddMousePosEvent((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
        desk_input = Now();
        return 0;
      case WM_MOUSELEAVE:
        mouse_in = false;
        DeskIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        desk_input = Now();
        return 0;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONDBLCLK:
      case WM_RBUTTONDOWN:
      case WM_RBUTTONDBLCLK:
      case WM_LBUTTONUP:
      case WM_RBUTTONUP: {
        bool down = msg != WM_LBUTTONUP && msg != WM_RBUTTONUP;
        int b = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_LBUTTONUP ? 0 : 1;
        if (down && !GetCapture()) SetCapture(h);
        ImGuiIO &io = DeskIO();
        io.AddMouseButtonEvent(b, down);
        if (!down && !(w & (MK_LBUTTON | MK_RBUTTON)) && GetCapture() == h) ReleaseCapture();
        desk_input = Now();
        return 0;
      }
      case WM_MOUSEWHEEL:
        DeskIO().AddMouseWheelEvent(0, (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA);
        desk_input = Now();
        return 0;
      case WM_MOUSEHWHEEL:
        DeskIO().AddMouseWheelEvent(-(float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA, 0);
        desk_input = Now();
        return 0;
      case WM_KEYDOWN:
      case WM_KEYUP:
      case WM_SYSKEYDOWN:
      case WM_SYSKEYUP: {
        bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        ImGuiIO &io = DeskIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, GetKeyState(VK_CONTROL) < 0);
        io.AddKeyEvent(ImGuiMod_Shift, GetKeyState(VK_SHIFT) < 0);
        io.AddKeyEvent(ImGuiMod_Alt, GetKeyState(VK_MENU) < 0);
        ImGuiKey k = KeyOf(w);
        if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
        desk_input = Now();
        if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) break;  // Alt+F4 and the system menu
        return 0;
      }
      case WM_CHAR:
        if (w > 0 && w < 0x10000) DeskIO().AddInputCharacterUTF16((unsigned short)w);
        desk_input = Now();
        return 0;
      case WM_SETFOCUS:
      case WM_KILLFOCUS:
        DeskIO().AddFocusEvent(msg == WM_SETFOCUS);
        desk_input = Now();
        return 0;
      case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
          SetCursorNow();
          return TRUE;
        }
        break;
      case WM_APP_SHOW: ShowWindowNow(); return 0;
      case WM_CLOSE:
        if (managed) ShowWindow(h, SW_HIDE);  // the SteamVR page stays; a second start shows the window again
        else quit = true;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
  }

  static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)l)->lpCreateParams);
    App *a = (App *)GetWindowLongPtrW(h, GWLP_USERDATA);
    return a && a->desk_ctx ? a->Proc(h, msg, w, l) : DefWindowProcW(h, msg, w, l);
  }

  void MakeWindow(HINSTANCE inst, bool show) {
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = nullptr;
    wc.hIcon = MakeHIcon(GetSystemMetrics(SM_CXICON));
    wc.hIconSm = MakeHIcon(GetSystemMetrics(SM_CXSMICON));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    UINT sdpi = GetDpiForSystem();
    dpi = sdpi / 96.f;
    RECT r{0, 0, MulDiv(1180, sdpi, 96), MulDiv(760, sdpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, sdpi);
    hwnd = CreateWindowExW(0, kClass, LHS_W(LHS_NAME), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, this);
    if (!hwnd) return;
    g_hwnd = hwnd;
    dpi = GetDpiForWindow(hwnd) / 96.f;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    COLORREF cap = RGB(21, 24, 27);  // the sidebar's colour, so the title bar joins it
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR, Windows 11 */, &cap, sizeof cap);
    RECT cr;
    GetClientRect(hwnd, &cr);
    if (cr.right > 0 && cr.bottom > 0) desk_img.Resize(cr.right, cr.bottom);
    // started with SteamVR, maybe under a game: don't take the focus
    if (show) ShowWindow(hwnd, managed ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
  }

  int Run(HINSTANCE inst) {
    managed = server_up = ServerUp();
    data_dir = DataDir();
    prefs_path = data_dir + L"\\dashboard.json";
    LoadPrefs(prefs_path, m.prefs);
    m.data_dir = Utf8(data_dir);
    log.path = data_dir + L"\\questlhsync.log";
    m.now = Now();

    atlas = IM_NEW(ImFontAtlas)();
    ui::LoadFonts(atlas);
    vr_ctx = MakeContext();
    desk_ctx = MakeContext();
    remote_ctx = MakeContext();
    vr_view.vr = true;
    remote_view.vr = remote_view.headset = true;
    icon = ui::IconImage(desk_ctx, atlas, 256);
    MakeWindow(inst, !managed || m.prefs.open_window);
    upd::Start(DriverRoot(), data_dir);
    ble::Start();
    link.Start();
    check_at = m.now + 8;

    while (!quit) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (quit) break;
      m.now = Now();
      if (m.now - server_at >= 1) {  // also when vrserver crashed or was killed, with no Quit sent
        server_at = m.now;
        server_up = ServerUp();
        if (!server_up) {
          if (vr_up) StopVr();
          if (managed) break;
        }
      }
      ReadDriver();
      if (m.st.state == QLHS_WIRED && managed) break;  // a wired headset: the driver is off, nothing to show
      StartVr();
      if (vr_up) PollVr();
      if (quit) break;
      m.vr = vr_up;
      if (vr_up && m.now - devices_at >= 0.1) {
        devices_at = m.now;
        ReadDevices();
      }
      if (m.now - log_at >= 1) {
        log_at = m.now;
        log.Poll(m.log);
      }
      ble::Fill(m.ble, m.ble_scanning, m.power);
      upd::Fill(m.upd);
      if (managed && m.prefs.wake_stations && !woke && m.ble == 1) {
        woke = true;
        ble::Power("", kCmdOn);
      }
      if (m.prefs.auto_check && m.now >= check_at) {
        check_at = m.now + 6 * 3600;
        upd::Check(m.prefs.repo);
      }
      // the panel on the headset the driver is connected to ("ip:port")
      std::string host = m.have_status && !m.stale ? std::string(m.st.headset_addr, strnlen(m.st.headset_addr, sizeof m.st.headset_addr)) : "";
      link.Poll(m.now, host.substr(0, host.find(':')));
      TakeRemote();

      // draw what's visible: quickly after input (hover, presses), a few times a second otherwise
      bool desk_busy = m.now - desk_input < 0.6, vr_busy = m.now - vr_input < 0.6;
      bool desk_vis = hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd);
      if (desk_vis && (desk_drawn < 0 || m.now - desk_drawn >= (desk_busy ? 1 / 60. : 0.2))) DrawDesktop();
      bool vr_vis = vr_up && vr::VROverlay()->IsOverlayVisible(page.main);
      if (vr_vis && (vr_drawn < 0 || m.now - vr_drawn >= (vr_busy ? 1 / 45. : 0.2))) {
        if (vr_img.w != kVrW) vr_img.Resize(kVrW, kVrH);
        Frame(vr_ctx, vr_view, vr_img, kVrScale * m.prefs.ui_scale, vr_applied, vr_drawn, *this);
        page.Show(vr_img);
      }
      bool remote_busy = m.now - remote_input < 0.6;
      if (link.shown) link.shown = false, remote_drawn = -1;
      if (link.CanSend() && (remote_drawn < 0 || m.now - remote_drawn >= (remote_busy ? 1 / 30. : 0.25))) {
        if (remote_img.w != panel::kW) remote_img.Resize(panel::kW, panel::kH);
        Frame(remote_ctx, remote_view, remote_img, kVrScale * m.prefs.ui_scale, remote_applied, remote_drawn, remote);
        link.SendFrame(remote_img);
      }
      DWORD wait = desk_busy || vr_busy || remote_busy ? 5 : vr_vis || link.Visible() ? 16 : 50;
      HANDLE ev = link.Event();
      MsgWaitForMultipleObjects(ev ? 1 : 0, &ev, FALSE, wait, QS_ALLINPUT);
    }

    // SteamVR quit: the base stations as the user asked
    bool vr_gone = managed && !ServerUp();
    StopVr();
    if (hwnd) ShowWindow(hwnd, SW_HIDE);
    if (vr_gone && m.prefs.on_quit)
      ble::PowerAllAndWait(m.prefs.on_quit == 1 ? kCmdStandby : kCmdSleep, m.prefs.known_stations, 12);
    ble::Stop();
    link.Stop();
    if (hwnd) DestroyWindow(hwnd);
    ImGui::DestroyContext(desk_ctx);
    ImGui::DestroyContext(vr_ctx);
    ImGui::DestroyContext(remote_ctx);
    IM_DELETE(atlas);
    return 0;
  }
};

std::string Arg(LPWSTR *argv, int argc, int i, const char *def) { return i < argc ? Utf8(argv[i]) : def; }

// --selftest: the window (hidden), the driver's status, both contexts on every page, the update check and Bluetooth,
// with no SteamVR page, no commands, no setting changes and nothing written outside `out`
int SelfTest(App &app, HINSTANCE inst, const std::string &out) {
  using ui::F;
  std::string r;
  auto line = [&](const std::string &s) { r += s + "\n"; };
  Model &m = app.m;
  app.managed = true;
  app.data_dir = DataDir();
  LoadPrefs(app.data_dir + L"\\dashboard.json", m.prefs);
  app.prefs_path = Wide(out) + L".prefs.json";  // ReadDriver saves new base stations; not into the real one
  m.data_dir = Utf8(app.data_dir);
  app.log.path = app.data_dir + L"\\questlhsync.log";
  app.atlas = IM_NEW(ImFontAtlas)();
  ui::LoadFonts(app.atlas);
  app.vr_ctx = app.MakeContext();
  app.desk_ctx = app.MakeContext();
  app.remote_ctx = app.MakeContext();
  app.vr_view.vr = true;
  app.remote_view.vr = app.remote_view.headset = true;
  app.icon = ui::IconImage(app.desk_ctx, app.atlas, 256);
  app.MakeWindow(inst, false);
  line(app.hwnd ? F("window: made, hidden, %dx%d, dpi scale %.2f", app.desk_img.w, app.desk_img.h, app.dpi) : "window: FAILED");

  for (double t0 = Now(); Now() - t0 < 1.5; Sleep(50)) {
    m.now = Now();
    app.ReadDriver();
  }
  line(F("driver: status %d, stale %d, state %d, SteamVR headset \"%s\", headset \"%s\" at \"%s\", %d base stations",
         m.have_status, m.stale, m.st.state, m.st.hmd, m.st.headset, m.st.headset_addr, m.st.nst));
  line(F("driver extra (LighthouseSync driver): %d, driver %s, module %s", m.have_extra, m.ex.driver_version, m.ex.module_version));
  app.log.Poll(m.log);
  line(F("log: %zu lines%s%s", m.log.size(), m.log.empty() ? "" : ", last: ", m.log.empty() ? "" : m.log.back().c_str()));

  double a = Now();
  for (int i = 0; i < 30; i++) {
    m.now = Now();
    app.DrawDesktop();
  }
  line(F("desktop frame %dx%d at %.2f: %.1f ms", app.desk_img.w, app.desk_img.h, app.DeskScale(), (Now() - a) / 30 * 1000));
  app.vr_img.Resize(kVrW, kVrH);
  a = Now();
  for (int i = 0; i < 10; i++) {
    m.now = Now();
    app.Frame(app.vr_ctx, app.vr_view, app.vr_img, kVrScale * m.prefs.ui_scale, app.vr_applied, app.vr_drawn, app);
  }
  line(F("SteamVR page frame %dx%d: %.1f ms", kVrW, kVrH, (Now() - a) / 10 * 1000));
  app.remote_img.Resize(panel::kW, panel::kH);
  m.prefs.developer = true;
  for (int p = 0; p < ui::kPageCount; p++) {
    app.desk_view.page = app.vr_view.page = app.remote_view.page = p;
    for (int i = 0; i < 2; i++) {
      m.now = Now();
      app.DrawDesktop();
      app.Frame(app.vr_ctx, app.vr_view, app.vr_img, kVrScale, app.vr_applied, app.vr_drawn, app);
      app.Frame(app.remote_ctx, app.remote_view, app.remote_img, kVrScale, app.remote_applied, app.remote_drawn, app.remote);
    }
  }
  line("pages: every page drawn in all three (window, SteamVR, headset panel)");

  upd::Start(DriverRoot(), app.data_dir);
  for (std::string repo : {m.prefs.repo, std::string("CreoleVR/QuestLHSync")}) {
    upd::Check(repo);
    UpdateInfo u;
    for (double t0 = Now(); Now() - t0 < 25; Sleep(100)) {
      upd::Fill(u);
      if (u.state != kUpdChecking && u.state != kUpdIdle) break;
    }
    line(F("update check %s: state %d, latest \"%s\" (%s), notes %zu chars, error \"%s\", dev build %d, installed %s",
           repo.c_str(), u.state, u.latest.c_str(), u.published.c_str(), u.notes.size(), u.error.c_str(), u.dev_build,
           upd::Installed().c_str()));
  }

  ble::Start();
  int bs = 0;
  bool scanning = false;
  for (double t0 = Now(); Now() - t0 < 40; Sleep(200)) ble::Fill(bs, scanning, m.power);
  line(F("bluetooth: state %d, scanning %d, %zu base stations", bs, scanning, m.power.size()));
  for (const StationPower &p : m.power)
    line(F("  %s: power %d (byte %d), %d dBm, busy %d, error \"%s\"", p.name.c_str(), p.state, p.raw, p.rssi, p.busy,
           p.error.c_str()));
  ble::Stop();

  std::ofstream(Wide(out), std::ios::binary | std::ios::trunc) << r;
  DestroyWindow(app.hwnd);
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argc >= 3 && !wcscmp(argv[1], L"--preview")) {
    std::string dir = Arg(argv, argc, 2, "."), kind = Arg(argv, argc, 3, "locked");
    int w = atoi(Arg(argv, argc, 4, "1280").c_str()), h = atoi(Arg(argv, argc, 5, "800").c_str());
    float s = (float)atof(Arg(argv, argc, 6, "1").c_str());
    return RunPreview(dir.c_str(), kind.c_str(), w, h, s);
  }
  if (argc >= 3 && !wcscmp(argv[1], L"--selftest")) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    static App app;
    return SelfTest(app, inst, Arg(argv, argc, 2, "selftest.txt"));
  }
  if (argc >= 3 && !wcscmp(argv[1], L"--icon")) {
    static App app;
    app.atlas = IM_NEW(ImFontAtlas)();
    ui::LoadFonts(app.atlas);
    app.desk_ctx = app.MakeContext();
    swr::Image img = ui::IconImage(app.desk_ctx, app.atlas, std::clamp(atoi(Arg(argv, argc, 3, "256").c_str()), 16, 1024));
    return WritePng(Arg(argv, argc, 2, "icon.png").c_str(), img) ? 0 : 1;
  }

  HANDLE mutex = CreateMutexW(nullptr, TRUE, QLHS_OVERLAY_MUTEX);
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // the driver doesn't start a second one, so this is the user: bring the first one's window up
    if (HWND w = FindWindowW(kClass, nullptr)) {
      DWORD pid = 0;
      GetWindowThreadProcessId(w, &pid);
      AllowSetForegroundWindow(pid);
      PostMessageW(w, WM_APP_SHOW, 0, 0);
    }
    return 0;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  static App app;  // big: the device cache and images
  int r = app.Run(inst);
  ReleaseMutex(mutex);
  return r;
}
