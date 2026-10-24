#include "panel_ui.h"

#include <time.h>

#include <algorithm>

#include "imgui.h"
#include "lhs_fork.h"
#include "theme.h"

namespace lhs::panel_ui {

using namespace ui;

namespace {

void Gap(float h) { ImGui::Dummy(ImVec2(0, S(h))); }

std::string Ago(double s) {
  if (s < 90) return "Just now";
  if (s < 5400) return F("%.0f min ago", s / 60);
  if (s < 2 * 86400) return F("%.1f h ago", s / 3600);
  return F("%.0f days ago", s / 86400);
}

std::string Join(const std::vector<std::string> &v) {
  std::string out;
  for (const std::string &s : v) out += (out.empty() ? "" : ", ") + s;
  return out;
}

// a command to type: there's no clipboard here, so no Copy button, and it wraps
void Code(const char *code) {
  float w = InnerWidth(), pad = S(12);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImVec2 ts = Measure(code, g_fonts.mono, kSmall, w - pad * 2);
  float h = ts.y + pad * 2;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(IM_COL32(12, 14, 16, 255)), S(8));
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(col::line), S(8), 0, S(1));
  ImGui::PushFont(g_fonts.mono, kSmall);
  dl->AddText(nullptr, 0, ImVec2(p.x + pad, p.y + pad), ImGui::GetColorU32(col::text), code, nullptr, w - pad * 2);
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(w, h));
}

// s cut to fit maxw, with an ellipsis
std::string Fit(const std::string &s, ImFont *f, float size, float maxw) {
  if (Measure(s.c_str(), f, size).x <= maxw) return s;
  size_t lo = 0, hi = s.size();  // the longest start that fits with the ellipsis, in whole UTF-8 characters
  while (lo < hi) {
    size_t mid = (lo + hi + 1) / 2;
    while (mid > lo && ((unsigned char)s[mid] & 0xC0) == 0x80) mid--;
    if (mid == lo) break;
    if (Measure((s.substr(0, mid) + "\xE2\x80\xA6").c_str(), f, size).x <= maxw) lo = mid;
    else hi = mid - 1;
  }
  return s.substr(0, lo) + "\xE2\x80\xA6";
}

// the big line at the top, and what it means
void Status(const State &s, Actions &a, float w) {
  std::string pc = s.pcs.empty() ? "" : s.pcs[0];
  const char *title;
  std::string body;
  ImU32 tone = col::text;
  if (!s.daemon) {
    title = "The camera service isn't answering";
    body = "lhsyncd sends what the tracking cameras see of the base stations to the PC. Without it the PC can't sync "
           "with this headset.";
    tone = col::bad;
  } else if (s.pcs.empty()) {
    title = "Waiting for the PC";
    body = "Start SteamVR on a PC with LighthouseSync, on the same network as this headset. It finds the headset by "
           "itself.";
  } else if (s.daemon_state == "capturing") {
    title = "Synced with the PC";
    body = F("Sending what the tracking cameras see of the base stations to the PC at %s.", pc.c_str());
    tone = col::good;
  } else {
    title = "Connected to the PC, cameras stopped";
    body = F("The PC at %s is connected, but the camera reader isn't running. The log says why.", pc.c_str());
    tone = col::warn;
  }
  Text(title, tone, g_fonts.semibold, kTitle);
  Gap(4);
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(w, S(820)));
  Wrapped(body.c_str(), col::dim, kBody);
  ImGui::PopTextWrapPos();
  if (!s.daemon) {
    Gap(8);
    if (Button("Restart it", kPrimary)) a.RestartDaemon();
  }
}

void Headset(const State &s) {
  CardTitle("This headset");
  KeyValue("Headset", s.model.empty() ? "Unknown" : s.model);
  KeyValue("Serial number", s.serial.empty() ? "Unknown" : s.serial);
  KeyValue("Camera service", !s.daemon                         ? "Not answering"
                             : s.daemon_state == "capturing" ? "Sending to the PC"
                                                             : "Running, cameras stopped",
           s.daemon ? col::text : col::bad);
  KeyValue("PC", s.pcs.empty() ? "None" : Join(s.pcs), s.pcs.empty() ? col::dim : col::text);
  KeyValue("LighthouseSync on the PC", s.pc_other           ? "Another version"
                                       : s.pc_app.empty() ? "Not connected"
                                                          : s.pc_app,
           s.pc_other ? col::warn : s.pc_app.empty() ? col::dim : col::text);
}

void Updates(const State &s, Actions &a) {
  CardTitle("Updates");
  KeyValue("Installed", s.installed.empty() ? "Unknown" : s.installed);
  if (s.updater)
    KeyValue("Last checked", s.checking ? "Checking now" : s.checked > 0 ? Ago((double)time(nullptr) - s.checked) : "Not yet");
  if (!s.result.empty() && !s.checking) Text(s.result.c_str(), col::faint, g_fonts.body, kSmall + 0.5f);
  Gap(8);
  if (!s.staged.empty()) {
    Wrapped((s.auto_update ? F("%s is downloaded. It installs the next time the headset starts.", s.staged.c_str())
                           : F("%s is downloaded.", s.staged.c_str()))
                .c_str(),
            col::text);
    Gap(4);
    if (Button("Install it now", kPrimary)) a.Install();
    Gap(2);
    Text("Installing now restarts SteamVR on the headset.", col::dim, g_fonts.body, kSmall);
  } else if (s.updater) {
    if (Button("Check now", kNormal, !s.checking)) a.Check();
  } else {
    Wrapped("Run the installer again in desktop mode's terminal to update, or to turn automatic updates back on:");
    Gap(4);
    Code("curl -fsSL https://github.com/" LHS_REPO "/releases/latest/download/install-frame.sh | bash");
  }
}

void Settings(const State &s, Actions &a) {
  CardTitle("Settings");
  SettingBegin("Automatic updates",
               s.updater ? "Looks for a new release every 6 hours and installs it the next time the headset starts."
                         : "Installed without them. Run the installer again to turn them on.",
               S(40));
  bool on = s.auto_update;
  if (Toggle("auto_update", &on, s.updater)) a.SetAutoUpdate(on);
  SettingEnd();
  Divider();
  SettingBegin("Camera service", "Restarting lhsyncd reconnects the PC in a few seconds.", ButtonWidth("Restart"));
  if (Button("Restart")) a.RestartDaemon();
  SettingEnd();
}

// lhsyncd's last lines, as many as fit in height
void Log(const State &s, float height) {
  CardTitle("Camera service log");
  ImDrawList *dl = ImGui::GetWindowDrawList();
  float w = InnerWidth(), lh = S(19), tw = Measure("00:00:00", g_fonts.mono, kSmall).x + S(12);
  int n = std::max(1, (int)(height / lh));
  if (s.log.empty()) {
    Text("Nothing yet.", col::faint, g_fonts.body, kSmall + 0.5f);
    ImGui::Dummy(ImVec2(w, std::max(0.f, n * lh - S(20))));
    return;
  }
  ImVec2 p = ImGui::GetCursorScreenPos();
  size_t first = s.log.size() > (size_t)n ? s.log.size() - n : 0;
  ImGui::PushFont(g_fonts.mono, kSmall);
  for (size_t i = first; i < s.log.size(); i++) {
    const LogLine &l = s.log[i];
    float y = p.y + (float)(i - first) * lh;
    dl->AddText(ImVec2(p.x, y), ImGui::GetColorU32(col::faint), l.time.c_str());
    ImU32 c = l.level == 2 ? col::bad : l.level == 1 ? col::warn : col::dim;
    dl->AddText(ImVec2(p.x + tw, y), ImGui::GetColorU32(c), Fit(l.text, g_fonts.mono, kSmall, w - tw).c_str());
  }
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(w, n * lh));
}

}  // namespace

void Draw(const State &s, Actions &a) {
  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(40), S(32)));
  ImGui::Begin("##panel", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar();
  float W = ImGui::GetContentRegionAvail().x;

  Text("LighthouseSync", col::text, g_fonts.semibold, kHeading);
  const char *version = s.installed.empty() ? LHS_VERSION : s.installed.c_str();
  Text(F("Version %s on this headset", version + (version[0] == 'v')).c_str(), col::faint, g_fonts.body, kSmall);
  Gap(18);
  Status(s, a, W);
  Gap(18);

  float gap = S(16), cw = (float)(int)((W - gap) * 0.45f);
  CardsBegin();
  CardBegin(cw);
  Headset(s);
  CardEnd();
  CardBegin(W - cw - gap);
  Updates(s, a);
  CardEnd();
  CardsEnd();

  // the log fills what's left: the card's padding and title come off its lines
  float left = ImGui::GetWindowPos().y + io.DisplaySize.y - S(32) - ImGui::GetCursorScreenPos().y;
  float lines = left - S(18) * 2 - S(30);
  CardsBegin();
  CardBegin(cw);
  Settings(s, a);
  CardEnd();
  CardBegin(W - cw - gap);
  Log(s, lines);
  CardEnd();
  CardsEnd();
  ImGui::End();
}

}  // namespace lhs::panel_ui
