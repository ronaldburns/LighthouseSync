// LighthouseSync panel: its page, all from the headset itself: lhsyncd and whether a PC is synced with it, its recent
// log, the headset's updater and its settings. Drawn with the dashboard's widgets (src/dashboard/theme.h), so it looks
// like the PC's pages.
#pragma once
#include <string>
#include <vector>

namespace lhs::panel_ui {

struct LogLine {
  std::string time;  // "01:43:00"
  std::string text;
  int level = 0;     // 0 info, 1 warning, 2 error
};

struct State {
  // lhsyncd's discovery answer
  bool daemon = false;
  std::string model, serial, daemon_state;
  std::vector<std::string> pcs;  // addresses connected to lhsyncd
  double synced_for = 0;         // seconds since the first of them connected
  std::vector<LogLine> log;      // lhsyncd's journal since the headset started, newest last
  // the PC's dashboard, connected to this panel for its Updates page
  std::string pc_app;            // its version, "" while none is connected
  bool pc_other = false;         // it has another link version
  // the updater (lhs/install-frame.sh)
  std::string installed, staged;
  bool auto_update = false, updater = false, checking = false;
  double checked = 0;  // Unix time
  std::string result;
};

struct Actions {
  virtual void Check() = 0;                   // look for a release now
  virtual void Install() = 0;                 // install the downloaded one now, restarting SteamVR
  virtual void RestartDaemon() = 0;           // lhsyncd's service
  virtual void SetAutoUpdate(bool on) = 0;    // the timer that downloads releases, and installing them at start
};

void Draw(const State &s, Actions &a);  // inside an ImGui frame, the whole window

}  // namespace lhs::panel_ui
