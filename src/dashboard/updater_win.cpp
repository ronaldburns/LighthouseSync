// Updates from GitHub releases, on a background thread each: the check reads the API's latest release, the install
// downloads the SteamVR zip, checks its SHA-256 (the asset's digest from GitHub, else the release's SHA256SUMS),
// unpacks it with Windows' tar and moves its questlhsync/ files over the driver's. Files in use (the driver DLL, this
// exe) can't be replaced but can be renamed, so each old file becomes <name>.lhs-old and the driver deletes those at
// its next start; SteamVR picks the new files up when it restarts.
#include "updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "../driver/json.h"

namespace fs = std::filesystem;

namespace lhs::upd {
namespace {

struct Release {
  std::string tag, zip_url, zip_name, zip_sha, sums_url;
};

std::mutex g_mu;
UpdateInfo g_info;
Release g_rel;
fs::path g_root, g_data;
std::atomic<bool> g_busy{false};

struct Failed {
  std::string what;
};

std::wstring Wide(const std::string &s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
}

void Set(const std::function<void(UpdateInfo &)> &f) {
  std::lock_guard lk(g_mu);
  f(g_info);
}

// ---------------------------------------------------------------- HTTPS

struct H {
  HINTERNET h;
  explicit H(HINTERNET v) : h(v) {}
  ~H() {
    if (h) WinHttpCloseHandle(h);
  }
};

// GET url; the body goes to sink (total: Content-Length, 0 unknown). Redirects are followed (release assets live on
// another host). Returns the HTTP status.
DWORD Get(const std::string &url, const std::function<void(const uint8_t *, size_t, uint64_t)> &sink,
          const wchar_t *accept = nullptr) {
  std::wstring wurl = Wide(url);
  URL_COMPONENTS uc{sizeof uc};
  wchar_t host[256], path[2048];
  uc.lpszHostName = host;
  uc.dwHostNameLength = _countof(host);
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = _countof(path);
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) throw Failed{"Bad address " + url};
  H ses(WinHttpOpen(LHS_W(LHS_NAME) L"/" LHS_W(LHS_VERSION), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0));
  if (!ses.h) throw Failed{"Couldn't start a download"};
  WinHttpSetTimeouts(ses.h, 15000, 15000, 15000, 30000);
  H con(WinHttpConnect(ses.h, host, uc.nPort, 0));
  if (!con.h) throw Failed{"Can't reach GitHub"};
  H req(WinHttpOpenRequest(con.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!req.h) throw Failed{"Can't reach GitHub"};
  std::wstring hdr = L"User-Agent: " LHS_W(LHS_NAME) L"\r\n";
  if (accept) hdr += std::wstring(L"Accept: ") + accept + L"\r\n";
  if (!WinHttpSendRequest(req.h, hdr.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(req.h, nullptr))
    throw Failed{"Can't reach GitHub. Is this PC online?"};
  DWORD status = 0, sz = sizeof status;
  WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                      &sz, WINHTTP_NO_HEADER_INDEX);
  uint64_t total = 0;
  {
    wchar_t b[32];
    DWORD bl = sizeof b;
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, b, &bl, WINHTTP_NO_HEADER_INDEX))
      total = _wcstoui64(b, nullptr, 10);
  }
  std::vector<uint8_t> buf(64 * 1024);
  for (;;) {
    DWORD got = 0;
    if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &got)) throw Failed{"The download stopped"};
    if (!got) break;
    sink(buf.data(), got, total);
  }
  return status;
}

std::string GetText(const std::string &url, DWORD *status, const wchar_t *accept = nullptr) {
  std::string body;
  DWORD st = Get(url, [&](const uint8_t *d, size_t n, uint64_t) {
    if (body.size() + n > (8u << 20)) throw Failed{"The answer from GitHub was too large"};
    body.append((const char *)d, n);
  }, accept);
  if (status) *status = st;
  return body;
}

// ---------------------------------------------------------------- SHA-256

struct Sha256 {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0)
      throw Failed{"Couldn't check the download"};
  }
  ~Sha256() {
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  }
  void Add(const uint8_t *d, size_t n) { BCryptHashData(h, (PUCHAR)d, (ULONG)n, 0); }
  std::string Hex() {
    uint8_t out[32];
    BCryptFinishHash(h, out, sizeof out, 0);
    static const char *x = "0123456789abcdef";
    std::string s;
    for (uint8_t b : out) s += x[b >> 4], s += x[b & 15];
    return s;
  }
};

// ---------------------------------------------------------------- versions

// "v1.26.3" < "v1.26.10"; a suffix ("1.26-dev") sorts before the plain number
int Compare(const std::string &a, const std::string &b) {
  auto parts = [](std::string s) {
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    std::vector<long> n;
    size_t i = 0;
    while (i < s.size() && isdigit((unsigned char)s[i])) {
      n.push_back(strtol(s.c_str() + i, nullptr, 10));
      while (i < s.size() && isdigit((unsigned char)s[i])) i++;
      if (i < s.size() && s[i] == '.') i++;
      else break;
    }
    bool suffix = i < s.size();
    return std::make_pair(n, suffix);
  };
  auto [na, sa] = parts(a);
  auto [nb, sb] = parts(b);
  for (size_t i = 0; i < std::max(na.size(), nb.size()); i++) {
    long x = i < na.size() ? na[i] : 0, y = i < nb.size() ? nb[i] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  if (sa != sb) return sa ? -1 : 1;
  return 0;
}

std::string Slurp(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool DevBuild() {  // running from a git checkout: updates come from git there
  std::error_code ec;
  for (fs::path p = g_root; !p.empty(); p = p.parent_path()) {
    if (fs::exists(p / ".git", ec)) return true;
    if (p == p.parent_path()) break;
  }
  return false;
}

// ---------------------------------------------------------------- check

void DoCheck(std::string repo) {
  Set([](UpdateInfo &u) {
    u.state = kUpdChecking;
    u.error.clear();
  });
  try {
    DWORD status = 0;
    std::string body = GetText("https://api.github.com/repos/" + repo + "/releases/latest", &status,
                               L"application/vnd.github+json");
    if (status == 404) throw Failed{repo + " has no releases yet"};
    if (status == 403 || status == 429) throw Failed{"GitHub is limiting requests from this network. Try again later."};
    if (status != 200) throw Failed{"GitHub answered " + std::to_string(status)};
    JVal v;
    if (!JParse(body, v) || v.t != JVal::Obj) throw Failed{"Couldn't read GitHub's answer"};
    auto str = [](const JVal *j) { return j && j->t == JVal::Str ? j->s : std::string(); };
    Release rel;
    rel.tag = str(v.get("tag_name"));
    if (rel.tag.empty()) throw Failed{"Couldn't read GitHub's answer"};
    if (const JVal *assets = v.get("assets"); assets && assets->t == JVal::Arr) {
      for (const JVal &a : assets->a) {
        std::string name = str(a.get("name")), url = str(a.get("browser_download_url"));
        bool zip = name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0 &&
                   (name.rfind("LighthouseSync-steamvr-", 0) == 0 || name.rfind("QuestLHSync-steamvr-", 0) == 0);
        if (zip && rel.zip_url.empty()) {
          rel.zip_url = url;
          rel.zip_name = name;
          std::string d = str(a.get("digest"));  // "sha256:<hex>", on assets uploaded since mid 2025
          if (d.rfind("sha256:", 0) == 0) rel.zip_sha = d.substr(7);
        }
        if (name == "SHA256SUMS") rel.sums_url = url;
      }
    }
    std::string published = str(v.get("published_at")).substr(0, 10);
    std::string notes = str(v.get("body"));
    std::string installed = Installed();
    bool newer = Compare(rel.tag, installed) > 0;
    char hm[16];
    time_t t = time(nullptr);
    tm lt;
    localtime_s(&lt, &t);
    strftime(hm, sizeof hm, "%H:%M", &lt);
    {
      std::lock_guard lk(g_mu);
      g_rel = rel;
      g_info.latest = rel.tag;
      g_info.published = published;
      g_info.notes = notes;
      g_info.checked = hm;
      if (g_info.state != kUpdInstalled) g_info.state = newer ? kUpdAvailable : kUpdCurrent;
      if (newer && rel.zip_url.empty()) {
        g_info.state = kUpdFailed;
        g_info.error = rel.tag + " has no SteamVR download for Windows";
      }
    }
  } catch (const Failed &f) {
    Set([&](UpdateInfo &u) {
      u.state = kUpdFailed;
      u.error = f.what;
    });
  } catch (...) {
    Set([](UpdateInfo &u) {
      u.state = kUpdFailed;
      u.error = "The check failed";
    });
  }
  g_busy = false;
}

// ---------------------------------------------------------------- install

void Run(const std::wstring &exe, const std::wstring &args, const fs::path &cwd) {
  std::wstring cmd = L"\"" + exe + L"\" " + args;
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi))
    throw Failed{"Couldn't unpack the download"};
  WaitForSingleObject(pi.hProcess, 120000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0) throw Failed{"Couldn't unpack the download"};
}

// moves new over old file by file; what's in use is renamed out of the way first. On a failure, puts back what it moved
void Swap(const fs::path &src, const fs::path &dst) {
  std::vector<std::pair<fs::path, fs::path>> moved;  // (dest, its .lhs-old)
  std::vector<fs::path> added;
  auto undo = [&] {
    std::error_code ec;
    for (auto &p : added) fs::remove(p, ec);
    for (auto it = moved.rbegin(); it != moved.rend(); ++it) MoveFileExW(it->second.c_str(), it->first.c_str(), MOVEFILE_REPLACE_EXISTING);
  };
  std::error_code ec;
  std::vector<fs::path> files;  // listed first: the loop moves them out of src
  for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    if (it->is_regular_file(ec)) files.push_back(it->path());
  for (const fs::path &from : files) {
    fs::path rel = fs::relative(from, src, ec);
    for (const auto &part : rel)
      if (part == "..") {
        undo();
        throw Failed{"The download has a file outside its folder"};
      }
    fs::path to = dst / rel;
    fs::create_directories(to.parent_path(), ec);
    if (fs::exists(to, ec)) {
      fs::path old = to;
      old += L".lhs-old";
      if (fs::exists(old, ec) && !DeleteFileW(old.c_str())) {  // an older one still in use
        for (int i = 2; i < 100 && fs::exists(old, ec); i++) {
          old = to;
          old += L".lhs-old" + std::to_wstring(i);
        }
      }
      if (!MoveFileExW(to.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        undo();
        throw Failed{"Couldn't replace " + rel.string()};
      }
      moved.emplace_back(to, old);
    } else {
      added.push_back(to);
    }
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
      undo();
      throw Failed{"Couldn't write " + rel.string()};
    }
  }
}

void DoInstall() {
  Release rel;
  {
    std::lock_guard lk(g_mu);
    rel = g_rel;
    g_info.state = kUpdDownloading;
    g_info.progress = 0;
    g_info.error.clear();
  }
  try {
    if (DevBuild()) throw Failed{"This is a build from a git checkout. Update it with git."};
    if (rel.zip_url.empty()) throw Failed{"Check for updates first"};
    std::string want = rel.zip_sha;
    if (want.empty() && !rel.sums_url.empty()) {  // "<hex>  <name>" lines
      std::istringstream sums(GetText(rel.sums_url, nullptr));
      std::string line;
      while (std::getline(sums, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string name = line.substr(line.find_first_not_of(" *", sp));
        if (name == rel.zip_name) want = line.substr(0, sp);
      }
    }
    for (char &c : want) c = (char)tolower((unsigned char)c);
    if (want.size() != 64) throw Failed{"The release has no checksum to check the download against"};

    fs::path dir = g_data / "update";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::path zip = dir / Wide(rel.zip_name);
    {
      std::ofstream f(zip, std::ios::binary | std::ios::trunc);
      if (!f) throw Failed{"Couldn't save the download"};
      Sha256 sha;
      uint64_t got = 0;
      DWORD status = Get(rel.zip_url, [&](const uint8_t *d, size_t n, uint64_t total) {
        f.write((const char *)d, n);
        sha.Add(d, n);
        got += n;
        if (total) Set([&](UpdateInfo &u) { u.progress = (float)got / (float)total; });
      });
      f.close();
      if (status != 200) throw Failed{"The download failed (HTTP " + std::to_string(status) + ")"};
      if (!f) throw Failed{"Couldn't save the download"};
      if (sha.Hex() != want) throw Failed{"The download doesn't match its checksum, so it wasn't installed"};
    }
    fs::path out = dir / "unpacked";
    fs::create_directories(out, ec);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    Run(std::wstring(sys) + L"\\tar.exe", L"-xf \"" + zip.wstring() + L"\"", out);
    fs::path src = out / "questlhsync";
    if (!fs::exists(src / "driver.vrdrivermanifest", ec) || !fs::exists(src / "bin" / "win64" / "driver_questlhsync.dll", ec))
      throw Failed{"The download has no SteamVR driver in it"};
    Swap(src, g_root);
    std::ofstream(g_data / "installed.json", std::ios::trunc) << "{\"version\":\"" << rel.tag << "\"}";
    fs::remove_all(dir, ec);
    Set([](UpdateInfo &u) {
      u.state = kUpdInstalled;
      u.progress = 1;
    });
  } catch (const Failed &f) {
    Set([&](UpdateInfo &u) {
      u.state = kUpdFailed;
      u.error = f.what;
    });
  } catch (...) {
    Set([](UpdateInfo &u) {
      u.state = kUpdFailed;
      u.error = "The update failed";
    });
  }
  g_busy = false;
}

}  // namespace

void Start(const std::wstring &driver_root, const std::wstring &data_dir) {
  g_root = driver_root;
  g_data = data_dir;
  std::lock_guard lk(g_mu);
  g_info.dev_build = DevBuild();
}

void Check(const std::string &repo) {
  if (g_busy.exchange(true)) return;
  Set([](UpdateInfo &u) {  // now, so a Fill straight after doesn't see the last check's result
    u.state = kUpdChecking;
    u.error.clear();
  });
  std::thread(DoCheck, repo).detach();
}

void Install() {
  if (g_busy.exchange(true)) return;
  std::thread(DoInstall).detach();
}

void Fill(UpdateInfo &u) {
  std::lock_guard lk(g_mu);
  u = g_info;
}

// what the last install recorded (an update this session), or this dashboard's own version if that's newer (an older
// installer's record)
std::string Installed() {
  std::string t = Slurp(g_data / "installed.json");
  JVal v;
  if (JParse(t, v) && v.t == JVal::Obj)
    if (const JVal *e = v.get("version"); e && e->t == JVal::Str && !e->s.empty() && Compare(e->s, LHS_VERSION) > 0)
      return e->s;
  return LHS_VERSION;
}

}  // namespace lhs::upd
