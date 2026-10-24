// LighthouseSync panel: its own page, for when there's no PC dashboard to show (none connected, or one without the
// panel link): what the headset knows by itself, lhsyncd and the headset's updater. Drawn with the dashboard's widgets
// (src/dashboard/theme.h), so it looks like the PC's pages.
#pragma once
#include <string>
#include <vector>

namespace lhs::panel_ui {

enum Link {
  kNoPc,        // no PC connected to lhsyncd
  kConnecting,  // a PC is, and the panel link should follow in a moment
  kNoLink,      // a PC is, but its LighthouseSync doesn't connect to the panel
  kOtherProto,  // its LighthouseSync connected with another link version
  kLinked,      // connected, waiting for the first page
};

struct State {
  int link = kNoPc;
  std::string pc_version;        // kOtherProto: the PC's LighthouseSync
  std::vector<std::string> pcs;  // addresses connected to lhsyncd
  // lhsyncd's discovery answer
  bool daemon = false;
  std::string model, serial, daemon_state;
  // the updater (lhs/install-frame.sh)
  std::string installed, staged;
  bool auto_update = false, updater = false, checking = false;
  double checked = 0;  // Unix time
  std::string result;
};

struct Actions {
  virtual void Check() = 0;          // look for a release now
  virtual void Install() = 0;        // install the downloaded one now, restarting SteamVR
  virtual void RestartDaemon() = 0;  // lhsyncd's service
};

void Draw(const State &s, Actions &a);  // inside an ImGui frame, the whole window

}  // namespace lhs::panel_ui
