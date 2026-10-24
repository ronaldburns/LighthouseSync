// Lighthouse 2.0 base stations over Bluetooth LE, with C++/WinRT. One worker thread (multithreaded apartment) checks
// the adapter, runs the advertisement watcher and does the GATT reads and writes one station at a time; the watcher's
// callbacks only record what they hear.
// A base station advertises as "LHB-XXXXXXXX" (its SteamVR serial) and has one power characteristic: write 0x00 to
// sleep, 0x01 to turn on, 0x02 for standby; it reads 0x00 asleep, 0x02 standby, 0x0b on, 0x01/0x08/0x09 starting.
// Some stations keep reading 0x01 long after they're on (lighthouse_pm issue 13), so a 0x01 that lasts counts as on.
#include "ble.h"

#include <windows.h>

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace lhs::ble {
namespace {

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Storage::Streams;
using winrt::Windows::Devices::Radios::RadioState;

const guid kService{0x00001523, 0x1212, 0xefde, {0x15, 0x23, 0x78, 0x5f, 0xea, 0xbc, 0xd1, 0x24}};
const guid kPower{0x00001525, 0x1212, 0xefde, {0x15, 0x23, 0x78, 0x5f, 0xea, 0xbc, 0xd1, 0x24}};

double Now() {  // Model::now's clock
  static LARGE_INTEGER f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)f.QuadPart;
}

struct Station {
  std::string name;
  uint64_t addr = 0;
  int rssi = 0;
  double seen = 0;
  int state = kPowerUnknown;
  int raw = -1;
  double read = 0;
  double ones_since = 0;  // when it started reading 0x01, 0 not
  bool busy = false;
  std::string error;
  double read_at = 0;  // when to read its state next, 0 never
  int want = -1;       // a PowerCmd to send
};

std::mutex g_mu;
std::condition_variable g_cv;
std::map<uint64_t, Station> g_st;     // by Bluetooth address
std::map<uint64_t, std::string> g_names;  // names from scan responses, for adverts without one
int g_state = 0;
bool g_scanning = false, g_stop = false;
int g_all_cmd = -1;      // Power(""): stations found before g_all_until get it too
double g_all_until = 0;
std::thread g_thread;
std::atomic<bool> g_exited{false};

int AdapterState() {
  try {
    BluetoothAdapter ad = BluetoothAdapter::GetDefaultAsync().get();
    if (!ad) return -1;
    if (!ad.IsLowEnergySupported()) return -3;
    auto radio = ad.GetRadioAsync().get();
    if (radio && radio.State() != RadioState::On) return -2;
    return 1;
  } catch (...) {
    return -1;
  }
}

int StateOf(uint8_t b) {
  switch (b) {
    case 0x00: return kPowerSleep;
    case 0x02: return kPowerStandby;
    case 0x0b: return kPowerOn;
    case 0x01:
    case 0x08:
    case 0x09: return kPowerStarting;
    default: return kPowerUnknown;
  }
}

struct Failed {
  std::string what;
};

const char *Why(GattCommunicationStatus s) {
  switch (s) {
    case GattCommunicationStatus::Unreachable: return "Couldn't reach it. It may be too far away, or another app has it.";
    case GattCommunicationStatus::AccessDenied: return "Windows denied access to it";
    case GattCommunicationStatus::ProtocolError: return "It answered with an error";
    default: return "Couldn't connect to it";
  }
}

// the first connection to a station often comes back unreachable while Windows sets the link up: try a few times,
// from Windows' cache first (the services never change), then from the station
template <class F>
auto Retry(F f) {
  auto r = f(BluetoothCacheMode::Cached);
  for (int i = 0; i < 3 && r.Status() != GattCommunicationStatus::Success; i++) {
    if (i) Sleep(400);
    r = f(BluetoothCacheMode::Uncached);
  }
  return r;
}

struct Read {
  int state = kPowerUnknown, raw = -1;
};

// connects, sends want (if any), reads the state back
Read Gatt(uint64_t addr, int want) {
  BluetoothLEDevice dev = BluetoothLEDevice::FromBluetoothAddressAsync(addr).get();
  if (!dev) throw Failed{"Couldn't connect to it"};
  struct Closer {
    BluetoothLEDevice &d;
    ~Closer() { d.Close(); }  // free the connection for SteamVR and other apps
  } closer{dev};
  GattDeviceServicesResult sr = Retry([&](BluetoothCacheMode m) { return dev.GetGattServicesForUuidAsync(kService, m).get(); });
  if (sr.Status() != GattCommunicationStatus::Success) throw Failed{Why(sr.Status())};
  if (sr.Services().Size() == 0) throw Failed{"It has no power control. Is it a 2.0 base station?"};
  GattDeviceService svc = sr.Services().GetAt(0);
  struct SvcCloser {
    GattDeviceService &s;
    ~SvcCloser() { s.Close(); }
  } svc_closer{svc};
  GattCharacteristicsResult cr = Retry([&](BluetoothCacheMode m) { return svc.GetCharacteristicsForUuidAsync(kPower, m).get(); });
  if (cr.Status() != GattCommunicationStatus::Success) throw Failed{Why(cr.Status())};
  if (cr.Characteristics().Size() == 0) throw Failed{"It has no power control. Is it a 2.0 base station?"};
  GattCharacteristic ch = cr.Characteristics().GetAt(0);
  if (want >= 0) {
    DataWriter w;
    w.WriteByte((uint8_t)want);
    bool ack = (ch.CharacteristicProperties() & GattCharacteristicProperties::Write) != GattCharacteristicProperties::None;
    GattCommunicationStatus ws =
        ch.WriteValueAsync(w.DetachBuffer(), ack ? GattWriteOption::WriteWithResponse : GattWriteOption::WriteWithoutResponse).get();
    if (ws != GattCommunicationStatus::Success) throw Failed{"It didn't take the command"};
    if (!ack) Sleep(300);  // nothing confirms it; give it a moment before reading back
  }
  Read r;
  GattReadResult rr = ch.ReadValueAsync(BluetoothCacheMode::Uncached).get();
  if (rr.Status() != GattCommunicationStatus::Success) throw Failed{Why(rr.Status())};
  if (rr.Value().Length() >= 1) {
    r.raw = DataReader::FromBuffer(rr.Value()).ReadByte();
    r.state = StateOf((uint8_t)r.raw);
  }
  return r;
}

void OnAdvert(const BluetoothLEAdvertisementReceivedEventArgs &e) {
  std::wstring wn(e.Advertisement().LocalName());
  uint64_t addr = e.BluetoothAddress();
  std::lock_guard lk(g_mu);
  std::string name;
  if (!wn.empty()) {
    for (wchar_t c : wn) name += c < 128 ? (char)c : '?';
    g_names[addr] = name;
  } else if (auto it = g_names.find(addr); it != g_names.end()) {
    name = it->second;
  }
  if (name.rfind("LHB-", 0) != 0) return;
  double now = Now();
  auto [it, fresh] = g_st.try_emplace(addr);
  Station &s = it->second;
  s.name = name;
  s.addr = addr;
  s.rssi = e.RawSignalStrengthInDBm();
  s.seen = now;
  if (fresh) {
    s.read_at = now;
    if (g_all_cmd >= 0 && now < g_all_until) s.want = g_all_cmd;
    g_cv.notify_all();
  }
}

void Worker() {
  init_apartment(apartment_type::multi_threaded);
  BluetoothLEAdvertisementWatcher watcher{nullptr};
  double adapter_at = -1e9;
  std::unique_lock lk(g_mu);
  while (!g_stop) {
    double now = Now();
    if ((g_state != 1 || !g_scanning) && now - adapter_at > 15) {  // no adapter, radio off, or the watcher stopped
      adapter_at = now;
      lk.unlock();
      int st = AdapterState();
      bool start = false;
      if (st == 1) {
        try {
          if (!watcher || watcher.Status() == BluetoothLEAdvertisementWatcherStatus::Aborted) {
            watcher = BluetoothLEAdvertisementWatcher();
            watcher.ScanningMode(BluetoothLEScanningMode::Active);  // the name comes in the scan response
            watcher.Received([](const BluetoothLEAdvertisementWatcher &, const BluetoothLEAdvertisementReceivedEventArgs &e) {
              OnAdvert(e);
            });
            watcher.Stopped([](const BluetoothLEAdvertisementWatcher &, const BluetoothLEAdvertisementWatcherStoppedEventArgs &) {
              std::lock_guard g(g_mu);
              g_scanning = false;
            });
          }
          watcher.Start();
          start = true;
        } catch (...) {
          st = -1;
        }
      }
      lk.lock();
      g_state = st;
      g_scanning = start;
      continue;
    }
    Station *job = nullptr;
    for (auto &kv : g_st) {
      Station &s = kv.second;
      if (!s.busy && (s.want >= 0 || (s.read_at > 0 && now >= s.read_at))) {
        job = &s;
        break;
      }
    }
    if (!job || g_state != 1) {
      g_cv.wait_for(lk, std::chrono::milliseconds(500));
      continue;
    }
    uint64_t addr = job->addr;
    int want = job->want;
    job->want = -1;
    job->busy = true;
    lk.unlock();
    Read rd;
    std::string err;
    try {
      rd = Gatt(addr, want);
    } catch (const Failed &f) {
      err = f.what;
    } catch (const hresult_error &) {
      err = "Couldn't connect to it";
    } catch (...) {
      err = "Couldn't connect to it";
    }
    lk.lock();
    if (auto it = g_st.find(addr); it != g_st.end()) {
      Station &s = it->second;
      s.busy = false;
      if (err.empty()) {
        double t = Now();
        s.ones_since = rd.raw != 0x01 ? 0 : s.ones_since > 0 ? s.ones_since : t;
        if (s.ones_since > 0 && t - s.ones_since > 30 && want < 0) rd.state = kPowerOn;
        s.state = rd.state;
        s.raw = rd.raw;
        s.read = t;
        s.error.clear();
      } else {
        s.error = err;
      }
      // after a command, or while it starts, look again soon; otherwise now and then
      s.read_at = Now() + (want >= 0 || (err.empty() && rd.state == kPowerStarting) ? 3 : err.empty() ? 60 : 20);
    }
    g_cv.notify_all();
  }
  lk.unlock();
  try {
    if (watcher) watcher.Stop();
  } catch (...) {
  }
  g_exited = true;
}

}  // namespace

void Start() {
  if (g_thread.joinable()) return;
  g_thread = std::thread(Worker);
}

void Stop() {
  {
    std::lock_guard lk(g_mu);
    g_stop = true;
  }
  g_cv.notify_all();
  if (!g_thread.joinable()) return;
  // a GATT call can take its time to give up; the process is leaving anyway
  for (int i = 0; i < 40 && !g_exited; i++) Sleep(50);
  if (g_exited) g_thread.join();
  else g_thread.detach();
}

void Scan() {
  std::lock_guard lk(g_mu);
  for (auto it = g_st.begin(); it != g_st.end();) it = it->second.busy ? std::next(it) : g_st.erase(it);
  g_cv.notify_all();
}

void Power(const std::string &name, int cmd) {
  std::lock_guard lk(g_mu);
  if (name.empty()) {
    g_all_cmd = cmd;
    g_all_until = Now() + 60;
  }
  for (auto &kv : g_st)
    if (name.empty() || kv.second.name == name) kv.second.want = cmd;
  g_cv.notify_all();
}

void Fill(int &state, bool &scanning, std::vector<StationPower> &out) {
  std::lock_guard lk(g_mu);
  state = g_state;
  scanning = g_scanning;
  out.clear();
  for (auto &kv : g_st) {
    const Station &s = kv.second;
    StationPower p;
    p.name = s.name;
    p.state = s.state;
    p.rssi = s.rssi;
    p.seen = s.seen;
    p.busy = s.busy || s.want >= 0;
    p.error = s.error;
    p.raw = s.raw;
    p.read = s.read;
    out.push_back(std::move(p));
  }
  std::sort(out.begin(), out.end(), [](const StationPower &a, const StationPower &b) { return a.name < b.name; });
}

void PowerAllAndWait(int cmd, const std::vector<std::string> &known, double timeout_s) {
  Power("", cmd);
  double end = Now() + timeout_s;
  std::unique_lock lk(g_mu);
  while (Now() < end) {
    if (g_state < 0) return;  // no Bluetooth: nothing to wait for
    bool done = g_state == 1;
    for (auto &kv : g_st)
      if (kv.second.busy || kv.second.want >= 0) done = false;
    for (const std::string &k : known) {
      bool found = false;
      for (auto &kv : g_st) found |= kv.second.name == k;
      if (!found) done = false;
    }
    if (done) return;
    g_cv.wait_for(lk, std::chrono::milliseconds(200));
  }
}

}  // namespace lhs::ble
