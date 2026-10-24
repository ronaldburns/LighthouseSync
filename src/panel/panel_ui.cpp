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

void Headset(const State &s) {
  CardTitle("This headset");
  KeyValue("Headset", s.model.empty() ? "Unknown" : s.model);
  KeyValue("Serial number", s.serial.empty() ? "Unknown" : s.serial);
  KeyValue("lhsyncd", !s.daemon                         ? "Not answering"
                      : s.daemon_state == "capturing" ? "Sending cameras to the PC"
                                                      : "Waiting for the PC",
           s.daemon ? col::text : col::bad);
  KeyValue("PC", s.pcs.empty() ? "None" : Join(s.pcs), s.pcs.empty() ? col::dim : col::text);
}

void Updates(const State &s, Actions &a) {
  CardTitle("Updates");
  KeyValue("Installed", s.installed.empty() ? "Unknown" : s.installed);
  KeyValue("Automatic updates", s.auto_update ? "On, every 6 hours" : "Off");
  if (s.updater)
    KeyValue("Last checked", s.checking ? "Checking now" : s.checked > 0 ? Ago((double)time(nullptr) - s.checked) : "Not yet");
  if (!s.result.empty() && !s.checking) Text(s.result.c_str(), col::faint, g_fonts.body, kSmall + 0.5f);
  Gap(8);
  if (!s.staged.empty()) {
    Wrapped(F("%s is downloaded. It installs the next time the headset starts.", s.staged.c_str()).c_str(), col::text);
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
  Gap(22);

  std::string pc = s.pcs.empty() ? "" : s.pcs[0];
  const char *title = "Waiting for the PC";
  std::string body =
      "Start SteamVR on the PC with LighthouseSync installed. Its dashboard shows here once it's synced with this "
      "headset.";
  ImU32 tone = col::text;
  switch (s.link) {
    case kConnecting:
      title = "Connecting to the PC";
      body = F("The PC at %s is synced with this headset. Its dashboard shows here in a moment.", pc.c_str());
      break;
    case kNoLink:
      title = "The PC's dashboard isn't connecting";
      body = F("The PC at %s is synced with this headset, but its dashboard isn't showing here. Check that "
               "LighthouseSync is running on the PC and up to date. QuestLHSync and older LighthouseSync releases "
               "don't have the headset panel.",
               pc.c_str());
      tone = col::warn;
      break;
    case kOtherProto:
      title = "The PC has a different LighthouseSync";
      body = F("The PC has LighthouseSync %s, which shows its dashboard here differently. Update the PC and this "
               "headset to the latest release.",
               s.pc_version.empty() ? "of another version" : s.pc_version.c_str());
      tone = col::warn;
      break;
    case kLinked:
      title = "Connected to the PC";
      body = "Loading its dashboard.";
      tone = col::good;
      break;
    default: break;
  }
  Text(title, tone, g_fonts.semibold, kTitle);
  Gap(4);
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(W, S(760)));
  Wrapped(body.c_str(), col::dim, kBody);
  ImGui::PopTextWrapPos();
  Gap(18);

  if (!s.daemon) {
    if (Notice("lhsyncd isn't answering, so the PC can't sync with this headset.", col::bad, "Restart it"))
      a.RestartDaemon();
    Gap(8);
  }

  float gap = S(16), cw = (float)(int)((W - gap) / 2);
  CardsBegin();
  CardBegin(cw);
  Headset(s);
  CardEnd();
  CardBegin(W - cw - gap);
  Updates(s, a);
  CardEnd();
  CardsEnd();
  ImGui::End();
}

}  // namespace lhs::panel_ui
