// LighthouseSync dashboard: the pages, drawn with Dear ImGui from a Model. One View per place the page shows
// (the desktop window, the SteamVR overlay, the headset's panel), each with its own page and edits.
#pragma once
#include "model.h"
#include "theme.h"

namespace lhs::ui {

enum Page { kOverview, kStations, kTracking, kSettings, kUpdates, kCameras, kDiagnostics, kLog, kPageCount };

struct View {
  int page = kOverview;
  bool vr = false;            // text settings go through the SteamVR keyboard
  Field host, headset_pref, repo;
  std::string repo_error;
  double confirm_until = -1;  // Recalibrate waits for a second press until then (Model::now)
  int track_view = 0;         // 0 from above, 1 from the front
};

// the frame's whole window, filling the display
void Draw(Model &m, View &v, Actions &a);

// "owner/name", as GitHub allows them
bool ValidRepo(const char *s);

}  // namespace lhs::ui
