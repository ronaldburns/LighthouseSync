// LighthouseSync dashboard: what the pages show (Model) and what they can ask for (Actions). Filled by the Windows
// dashboard from the driver's shared memory, SteamVR, Bluetooth and GitHub; by the headset's panel from the PC.
// No platform headers here.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "../common/lhs_fork.h"

namespace lhs {

enum DeviceKind { kDevHmd, kDevStation, kDevController, kDevTracker, kDevOther };

// a SteamVR device's pose (standing space, metres), for the tracking view
struct Device {
  int kind = kDevOther;
  std::string serial, model;
  bool tracking = false;  // pose valid and running OK
  float p[3] = {};
  float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // rotation, rows
};

enum PowerState { kPowerUnknown = -1, kPowerSleep = 0, kPowerStarting = 1, kPowerStandby = 2, kPowerOn = 3 };
enum PowerCmd { kCmdSleep = 0, kCmdOn = 1, kCmdStandby = 2 };

// a Lighthouse 2.0 base station found over Bluetooth
struct StationPower {
  std::string name;  // "LHB-1A2B3C4D", as SteamVR's serial
  int state = kPowerUnknown;
  int rssi = 0;
  double seen = 0;   // Model::now of its last advertisement
  bool busy = false; // a command or read is under way
  std::string error;
  int raw = -1;      // the power byte it last read, -1 none
  double read = 0;   // Model::now of that read
};

enum UpdateState { kUpdIdle, kUpdChecking, kUpdCurrent, kUpdAvailable, kUpdDownloading, kUpdInstalled, kUpdFailed };

struct UpdateInfo {
  int state = kUpdIdle;
  std::string latest;      // "v1.26.2"
  std::string published;   // "2026-10-23"
  std::string notes;       // the release's text
  std::string error;
  std::string checked;     // when it last checked ("14:02"), "" never
  float progress = 0;      // 0..1 while downloading
  bool dev_build = false;  // running from a build folder: updates come from git instead
};

// the dashboard's own settings (dashboard.json next to the driver's data)
struct Prefs {
  float ui_scale = 1.0f;
  bool open_window = true;   // show the desktop window when SteamVR starts
  bool developer = false;    // show the developer pages
  bool auto_check = true;    // look for updates at start and every 6 hours
  std::string repo = LHS_REPO;
  bool wake_stations = false;  // wake base stations when SteamVR starts
  int on_quit = 0;             // when SteamVR quits: 0 leave them, 1 standby, 2 sleep
  std::vector<std::string> known_stations;  // base stations SteamVR has shown on this PC
};

struct Model {
  double now = 0;  // seconds, monotonic

  // driver
  bool have_status = false;  // the driver's shared memory is there
  bool stale = true;         // ...but not updating
  QlhsStatus st{};
  bool have_extra = false;   // the LighthouseSync part of it (an upstream driver hasn't got it)
  LhsExtra ex{};

  // SteamVR
  bool vr = false;
  std::vector<Device> devices;
  bool have_play_area = false;
  float play_area[4][3] = {};  // corners, standing space

  // Bluetooth
  int ble = 0;  // 0 starting, 1 ready, -1 no adapter, -2 off, -3 not supported
  bool ble_scanning = false;
  std::vector<StationPower> power;

  UpdateInfo upd;
  Prefs prefs;
  std::vector<std::string> log;  // tail of questlhsync.log, oldest first
  std::string data_dir;          // %LOCALAPPDATA%\QuestLHSync

  // the last setting change and the driver's answer
  std::string ack_key, ack_text;
  double ack_at = -1;
  std::vector<std::string> restart_keys;  // changed this session, applied at the next SteamVR start
};

struct Actions {
  virtual ~Actions() = default;
  virtual void Command(int cmd) = 0;                                       // QLHS_CMD_*
  virtual void SetSetting(const char *key, const std::string &value) = 0;  // driver_questlhsync.<key>
  virtual void PrefsChanged() = 0;                                         // Model::prefs changed: save
  virtual void Power(const std::string &name, int cmd) = 0;                // name "": every station found
  virtual void Scan() = 0;                                                 // look for base stations again
  virtual void CheckUpdates() = 0;
  virtual void InstallUpdate() = 0;
  virtual void OpenDataFolder() = 0;
  virtual void OpenUrl(const std::string &url) = 0;
  virtual void Copy(const std::string &text) = 0;
  // VR only: the SteamVR keyboard for a text setting; the result goes to SetSetting(key)
  virtual void EditText(const char *key, const std::string &title, const std::string &value) = 0;
};

}  // namespace lhs
