#include "ui.h"

#include <ctype.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include <algorithm>

#include "imgui_internal.h"

namespace lhs::ui {
namespace {

// ---------------------------------------------------------------- wording

std::string Ago(double s) {
  if (s < 0) return "Not yet";
  if (s < 1.5) return "Now";
  if (s < 90) return F("%.0f s ago", s);
  if (s < 5400) return F("%.0f min ago", s / 60);
  return F("%.1f h ago", s / 3600);
}

std::string Dur(double s) {
  if (s < 90) return F("%.0f s", s);
  if (s < 5400) return F("%.0f min", s / 60);
  return F("%.1f h", s / 3600);
}

struct StateLine {
  const char *title;
  const char *sub;
  ImU32 color;
};

StateLine State(const Model &m) {
  const QlhsStatus &s = m.st;
  if (!m.have_status)
    return {"Driver not loaded",
            "SteamVR hasn't loaded the LighthouseSync driver. Check that it's turned on in SteamVR's settings, under "
            "Startup / Shutdown, Manage add-ons.",
            col::bad};
  if (m.stale) return {"Driver not running", "The driver isn't updating. Restart SteamVR.", col::bad};
  switch (s.state) {
    case QLHS_NO_HMD:
      return {"Waiting for a supported headset",
              s.hmd[0] ? "SteamVR's headset isn't a Quest Pro, 3, 3S or Steam Frame, so the lighthouse space is left "
                         "alone. Turn on Any headset in Settings to use it anyway."
                       : "Start your streaming app: Steam Link, Air Link, Virtual Desktop, ALVR or another.",
              col::dim};
    case QLHS_SEARCHING:
      return {"Looking for the headset",
              "No headset running lhsyncd answers on the network. Is it awake and on the same network? If your router "
              "drops broadcasts, enter the headset's address in Settings.",
              col::warn};
    case QLHS_CONNECTING: return {"Connecting", "Found the headset, opening the camera stream.", col::warn};
    case QLHS_NO_CAMERAS:
      return {"No camera frames", "Connected, but the tracking cameras send nothing. Put the headset on or wake it.",
              col::bad};
    case QLHS_NO_STATIONS:
      return {"Waiting for base stations",
              "SteamVR shows base stations only while a lighthouse device, such as a tracker or controller, is on. Turn "
              "one on.",
              col::warn};
    case QLHS_ACQUIRING:
      return {"Finding the base stations",
              s.head_still == 2 ? "SteamVR isn't getting your head's motion. Is SteamVR's view in the headset, not a "
                                  "desktop view?"
              : s.head_still    ? "The headset isn't moving, so its camera frames wait. Put it on and look around the room."
                                : "Look around the room so the cameras catch both base stations' flashes.",
              col::warn};
    case QLHS_LOCKED:
      if (s.paused) return {"Locked, corrections paused", "Lighthouse devices stay where they are until you resume.", col::warn};
      return {"Locked", "Lighthouse devices follow the headset's own tracking.", col::good};
    case QLHS_DISABLED:
      return {"Turned off", "The driver is turned off in SteamVR's settings (driver_questlhsync, enable).", col::dim};
    case QLHS_WIRED:
      return {"Wired headset", "SteamVR's headset is a wired one, so LighthouseSync stays out of the way.", col::dim};
    default: return {"Starting", "", col::dim};
  }
}

const char *StateName(int st) {
  static const char *n[] = {"Starting",  "No supported headset", "Searching", "Connecting", "No camera frames",
                            "No base stations", "Acquiring", "Locked", "Turned off", "Wired headset"};
  return st >= 0 && st < (int)(sizeof n / sizeof *n) ? n[st] : "Unknown";
}

bool Same(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
  return *a == *b;
}

const StationPower *PowerOf(const Model &m, const char *serial) {
  for (const auto &p : m.power)
    if (Same(p.name.c_str(), serial)) return &p;
  return nullptr;
}

// seen: the headset's cameras see it now, so it's on whatever it reads
const char *PowerText(const StationPower &p, ImU32 *c, bool seen = false) {
  if (p.busy) { *c = col::dim; return "Working"; }
  switch (seen && p.state == kPowerStarting ? kPowerOn : p.state) {
    case kPowerOn: *c = col::good; return "On";
    case kPowerStandby: *c = col::warn; return "Standby";
    case kPowerSleep: *c = col::dim; return "Sleeping";
    case kPowerStarting: *c = col::warn; return "Starting";
    default: *c = col::faint; return "Unknown";
  }
}

bool Contains(const std::vector<std::string> &v, const char *k) {
  return std::find(v.begin(), v.end(), std::string(k)) != v.end();
}

// ---------------------------------------------------------------- layout helpers

struct Row {
  ImVec2 top;
  float h;
};

// a page title with room for controls on the right (right_w wide); call TitleEnd after drawing them
Row TitleBegin(const char *title, float right_w) {
  Row r{ImGui::GetCursorScreenPos(), 0};
  Title(title);
  r.h = ImGui::GetItemRectSize().y;
  float w = ImGui::GetContentRegionAvail().x;
  ImGui::SetCursorScreenPos(ImVec2(r.top.x + w - right_w, r.top.y + (r.h - S(34)) * 0.5f));
  ImGui::BeginGroup();
  return r;
}

void TitleEnd(const Row &r) {
  ImGui::EndGroup();
  float h = std::max(r.h, ImGui::GetItemRectMax().y - r.top.y);
  ImGui::SetCursorScreenPos(r.top);
  ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, h));
  ImGui::Dummy(ImVec2(0, S(6)));
}

float Widths(std::initializer_list<const char *> labels) {
  float w = 0;
  for (const char *l : labels) w += ButtonWidth(l) + S(8);
  return w - S(8);
}

void Gap(float h) { ImGui::Dummy(ImVec2(0, S(h))); }

// a command to copy, in a box with a Copy button
void CodeLine(const std::string &code, Actions &a) {
  float w = InnerWidth(), bw = ButtonWidth("Copy"), pad = S(12);
  ImVec2 p = ImGui::GetCursorScreenPos();
  float tw = w - bw - pad * 3;
  ImVec2 ts = Measure(code.c_str(), g_fonts.mono, kSmall, tw);
  float h = std::max(ts.y + pad * 2, S(34) + S(10));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(IM_COL32(12, 14, 16, 255)), S(8));
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(col::line), S(8), 0, S(1));
  ImGui::PushFont(g_fonts.mono, kSmall);
  dl->AddText(nullptr, 0, ImVec2(p.x + pad, p.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(col::text), code.c_str(), nullptr, tw);
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - S(5), p.y + (h - S(34)) * 0.5f));
  if (Button("Copy##code", kNormal)) a.Copy(code);
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(w, h));
}

// a setting's note under its row: the driver's answer, or a restart to come
void SettingNote(const Model &m, const char *key) {
  if (Contains(m.restart_keys, key)) Text("Restart SteamVR to apply this change.", col::warn, g_fonts.body, kSmall);
  else if (m.ack_key == key && m.now - m.ack_at < 4 && !m.ack_text.empty())
    Text(m.ack_text.c_str(), col::faint, g_fonts.body, kSmall);
}

// a driver setting that's on or off
void BoolSetting(const Model &m, Actions &a, const char *key, int32_t value, const char *label, const char *desc,
                 bool enabled) {
  SettingBegin(label, desc, S(40));
  bool v = value != 0;
  if (Toggle(key, &v, enabled)) a.SetSetting(key, v ? "true" : "false");
  SettingEnd();
  SettingNote(m, key);
}

// a text setting in VR: its value, and Edit opens the SteamVR keyboard (the answer comes back through the Actions)
void VrText(Actions &a, const char *key, const char *label, const char *current, const char *placeholder, float cw,
            bool enabled) {
  std::string shown = current[0] ? current : placeholder;
  ImVec2 p = ImGui::GetCursorScreenPos();
  float bw = ButtonWidth("Edit");
  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (!enabled) ImGui::BeginDisabled();
  dl->AddRectFilled(p, ImVec2(p.x + cw - bw - S(8), p.y + S(34)), ImGui::GetColorU32(col::raised), S(7));
  ImGui::PushFont(g_fonts.body, kBody);
  dl->AddText(ImVec2(p.x + S(10), p.y + (S(34) - ImGui::GetFontSize()) * 0.5f),
              ImGui::GetColorU32(current[0] ? col::text : col::faint), shown.c_str());
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(p.x + cw - bw, p.y));
  if (Button(F("Edit##%s", key).c_str(), kNormal, enabled)) a.EditText(key, label, current);
  if (!enabled) ImGui::EndDisabled();
}

// a driver setting that's text: a field on the desktop, the SteamVR keyboard in VR
void TextSetting(const Model &m, View &v, Actions &a, Field &f, const char *key, const char *current,
                 const char *label, const char *desc, const char *placeholder, bool enabled) {
  float cw = S(340);
  SettingBegin(label, desc, cw);
  if (v.vr) {
    VrText(a, key, label, current, placeholder, cw, enabled);
  } else if (TextField(key, f, current, cw, enabled, placeholder)) {
    a.SetSetting(key, f.buf);
  }
  SettingEnd();
  SettingNote(m, key);
}

// ---------------------------------------------------------------- sidebar

void Sidebar(Model &m, View &v) {
  Text(LHS_NAME, col::text, g_fonts.semibold, 17.f);
  Text(F("Version %s", LHS_VERSION).c_str(), col::faint, g_fonts.body, kSmall);
  Gap(22);
  struct Item {
    int page;
    const char *label;
  };
  static const Item main[] = {{kOverview, "Overview"}, {kStations, "Base stations"}, {kTracking, "Tracking view"},
                              {kSettings, "Settings"}, {kUpdates, "Updates"}};
  static const Item dev[] = {{kCameras, "Cameras"}, {kDiagnostics, "Diagnostics"}, {kLog, "Log"}};
  for (const Item &it : main) {
    const char *note = it.page == kUpdates && (m.upd.state == kUpdAvailable || !m.panel.staged.empty()) ? "New" : nullptr;
    if (NavItem(it.label, v.page == it.page, note)) v.page = it.page;
  }
  if (m.prefs.developer) {
    Gap(18);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(12));
    Text("Developer", col::faint, g_fonts.medium, kSmall);
    Gap(2);
    for (const Item &it : dev)
      if (NavItem(it.label, v.page == it.page)) v.page = it.page;
  } else if (v.page >= kCameras) {
    v.page = kOverview;
  }
}

// ---------------------------------------------------------------- overview

void HeadsetCard(const Model &m) {
  const QlhsStatus &s = m.st;
  bool conn = s.headset_addr[0] != 0;
  CardTitle("Headset", m.stale ? "Last known" : conn ? "Connected" : "Not connected", conn && !m.stale ? col::good : col::faint);
  Text(s.headset[0] ? s.headset : "None yet", s.headset[0] ? col::text : col::faint, g_fonts.semibold, 21.f);
  Gap(4);
  KeyValue("Address", conn ? s.headset_addr : "None");
  bool frames = s.cam_fps > 0.5;
  KeyValue("Cameras", frames ? F("%.0f frames/s", s.cam_fps) : "No frames",
           frames || s.state < QLHS_NO_CAMERAS ? col::text : col::bad);
  KeyValue("Round trip", s.rtt_ms > 0 ? F("%.1f ms", s.rtt_ms) : "Not measured");
  KeyValue("Pose timing", conn ? F("%.1f ms, %s", s.expo_ms, s.expo_learned ? "learned" : "learning") : "Not measured",
           s.expo_learned || !conn ? col::text : col::warn);
  KeyValue("Bright spots", frames ? F("%.0f/s, %.0f/s used", s.spot_rate, s.sight_rate) : "None",
           s.state == QLHS_ACQUIRING && s.sight_rate < 0.5 ? col::warn : col::text);
  KeyValue("Streamer", s.hmd_system[0] ? s.hmd_system : "None");
  if (m.have_extra) KeyValue("Headset software", m.ex.module_version[0] ? m.ex.module_version : "Unknown");
}

void AlignmentCard(const Model &m) {
  const QlhsStatus &s = m.st;
  bool has = s.locked || s.nfit > 0 || s.yaw_deg != 0;
  CardTitle("Alignment");
  if (has) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    std::string big = F("%.2f°", s.med_deg >= 0 ? s.med_deg : 0.0);
    Text(big.c_str(), s.med_deg >= 0 && s.med_deg < 0.3 ? col::good : col::text, g_fonts.semibold, 21.f);
    ImVec2 bs = ImGui::GetItemRectSize();
    ImVec2 cs = Measure("median sighting error", g_fonts.body, kSmall);
    ImGui::PushFont(g_fonts.body, kSmall);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + bs.x + S(10), p.y + bs.y - cs.y - S(3)), ImGui::GetColorU32(col::dim),
                                        "median sighting error");
    ImGui::PopFont();
  } else {
    Text("Not aligned yet", col::faint, g_fonts.semibold, 21.f);
  }
  Gap(4);
  KeyValue("Yaw", has ? F("%+.2f°", s.yaw_deg) : "None");
  KeyValue("Offset", has ? F("%.2f, %.2f, %.2f m", s.t[0], s.t[1], s.t[2]) : "None");
  KeyValue("Sightings used", has ? F("%d", s.nfit) : "None");
  KeyValue("Since acquired", s.locked_for >= 0 ? Dur(s.locked_for) : "Not yet");
  KeyValue("Settling", !has ? "Not yet" : s.lag_cm > 0.05 ? F("%.1f cm to go", s.lag_cm) : "Done");
}

std::string StationLine(const Model &m, const QlhsStation &st, bool has) {
  std::string line = st.last_seen < 0 ? "Not seen yet" : st.last_seen < 1.5 ? "Seen now" : "Seen " + Ago(st.last_seen);
  if (has) line += F(", %d sightings", st.support);
  if (st.dist > 0) line += F(", %.1f m", st.dist);
  return line;
}

void StationsCard(const Model &m) {
  const QlhsStatus &s = m.st;
  bool has = s.locked || s.nfit > 0 || s.yaw_deg != 0;
  CardTitle("Base stations", F("%d in SteamVR", s.nst).c_str());
  if (s.nst == 0) {
    Wrapped("None in SteamVR yet. They show up while a tracker or controller is on.", col::dim, kBody);
    return;
  }
  int n = std::min(s.nst, 5);
  for (int i = 0; i < n; i++) {
    const QlhsStation &st = s.st[i];
    if (i) Gap(2);
    ImVec2 p = ImGui::GetCursorScreenPos();
    Text(st.serial, col::text, g_fonts.medium, kBody);
    const char *role = st.anchor ? "Anchor" : st.measured ? "Measured" : nullptr;
    if (role) {
      ImVec2 rs = Measure(role, g_fonts.body, kSmall);
      ImGui::PushFont(g_fonts.body, kSmall);
      ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + InnerWidth() - rs.x, p.y + S(2)), ImGui::GetColorU32(col::accent), role);
      ImGui::PopFont();
    }
    bool fresh = st.last_seen >= 0 && st.last_seen < 15;
    ImVec2 p2 = ImGui::GetCursorScreenPos();
    Text(StationLine(m, st, has).c_str(), fresh ? col::dim : col::faint, g_fonts.body, kSmall + 0.5f);
    // its power state, from Bluetooth, under the role
    if (const StationPower *pw = PowerOf(m, st.serial); pw && pw->state != kPowerUnknown) {
      ImU32 c;
      const char *t = PowerText(*pw, &c, fresh);
      ImVec2 ts = Measure(t, g_fonts.body, kSmall);
      ImGui::PushFont(g_fonts.body, kSmall);
      ImGui::GetWindowDrawList()->AddText(ImVec2(p2.x + InnerWidth() - ts.x, p2.y + S(1)), ImGui::GetColorU32(c), t);
      ImGui::PopFont();
    }
  }
  if (s.nst > n) Text(F("and %d more", s.nst - n).c_str(), col::faint, g_fonts.body, kSmall);
}

void PageOverview(Model &m, View &v, Actions &a) {
  const QlhsStatus &s = m.st;
  StateLine sl = State(m);
  bool live = m.have_status && !m.stale;
  bool confirm = m.now < v.confirm_until;
  const char *pause = s.paused ? "Resume corrections" : "Pause corrections";
  float right = confirm ? Widths({"Recalibrate now", "Cancel"}) : Widths({pause, "Recalibrate"});
  float W = ImGui::GetContentRegionAvail().x;

  ImVec2 top = ImGui::GetCursorScreenPos();
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + W - right - S(32));
  Text(sl.title, sl.color, g_fonts.semibold, kTitle);
  if (sl.sub[0]) Text(sl.sub, col::dim, g_fonts.body, kBody);
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
  float lh = ImGui::GetItemRectSize().y;
  ImGui::SetCursorScreenPos(ImVec2(top.x + W - right, top.y + S(2)));
  ImGui::BeginGroup();
  if (confirm) {
    if (Button("Recalibrate now", kDanger, live)) {
      a.Command(QLHS_CMD_RECALIBRATE);
      v.confirm_until = -1;
    }
    ImGui::SameLine(0, S(8));
    if (Button("Cancel")) v.confirm_until = -1;
  } else {
    if (Button(pause, s.paused ? kPrimary : kNormal, live && s.state == QLHS_LOCKED))
      a.Command(s.paused ? QLHS_CMD_RESUME : QLHS_CMD_PAUSE);
    ImGui::SameLine(0, S(8));
    if (Button("Recalibrate", kNormal, live)) v.confirm_until = m.now + 10;
  }
  ImGui::EndGroup();
  ImGui::SetCursorScreenPos(top);
  ImGui::Dummy(ImVec2(W, std::max(lh, S(36))));
  Gap(10);

  if (confirm) {
    Notice("Recalibrating forgets this headset's alignment and pose timing, and finds the base stations again. A "
           "reference frame you measured yourself stays.",
           col::warn);
    Gap(4);
  }
  if (m.upd.state == kUpdAvailable) {
    if (Notice(F("LighthouseSync %s is available.", m.upd.latest.c_str()).c_str(), col::accent, "See what's new"))
      v.page = kUpdates;
    Gap(4);
  } else if (m.upd.state == kUpdInstalled) {
    Notice("The update is installed. Restart SteamVR to finish it.", col::good);
    Gap(4);
  }
  if (!m.panel.staged.empty()) {
    if (Notice(F("The headset downloaded %s. It installs the next time the headset starts.", m.panel.staged.c_str()).c_str(),
               col::accent, "Details"))
      v.page = kUpdates;
    Gap(4);
  }
  if (!m.restart_keys.empty()) {
    Notice("Some settings you changed take effect after SteamVR restarts.", col::warn);
    Gap(4);
  }

  float gap = S(16), cw = IM_TRUNC((W - gap * 2) / 3);
  CardsBegin();
  CardBegin(cw);
  HeadsetCard(m);
  CardEnd();
  CardBegin(cw);
  AlignmentCard(m);
  CardEnd();
  CardBegin(W - (cw + gap) * 2);
  StationsCard(m);
  CardEnd();
  CardsEnd();

  CardBegin(W);
  CardTitle("Recent activity");
  uint32_t n = s.nlog;
  if (!n) Text("Nothing yet.", col::faint, g_fonts.body, kBody);
  for (uint32_t i = 0; i < 6 && i < n; i++) {
    uint32_t k = n - 1 - i;
    char line[121];
    memcpy(line, s.log[k % 8], 120);
    line[120] = 0;
    Mono(line, i == 0 ? col::text : col::dim, kSmall);
  }
  CardEnd();
}

// ---------------------------------------------------------------- base stations

const char *BleLine(const Model &m) {
  switch (m.ble) {
    case 1:
      return m.power.empty() ? "Bluetooth is on. Looking for Lighthouse 2.0 base stations." : "Bluetooth is on.";
    case -1: return "This PC has no Bluetooth adapter, so base stations can't be woken or put to sleep from here.";
    case -2: return "Bluetooth is off. Turn it on in Windows settings to control base station power.";
    case -3: return "This PC's Bluetooth doesn't support Bluetooth Low Energy, which base stations use.";
    default: return "Looking for this PC's Bluetooth adapter.";
  }
}

void PowerSettings(Model &m, View &v, Actions &a, bool enabled) {
  SettingBegin("Wake base stations with SteamVR",
               "Turns base stations on over Bluetooth when SteamVR starts, so they're ready when you put the headset on.",
               S(40));
  if (Toggle("wake_stations", &m.prefs.wake_stations, enabled)) a.PrefsChanged();
  SettingEnd();
  Divider();
  static const char *quit[] = {"Leave on", "Standby", "Sleep"};
  float qw = 0;
  for (const char *q : quit) qw += Measure(q, g_fonts.medium, kBody).x + S(24);
  SettingBegin("When SteamVR quits",
               "What base stations do after SteamVR closes. Standby wakes faster, sleep saves the most power.",
               qw + S(6));
  if (Segmented("on_quit", &m.prefs.on_quit, quit, 3, enabled)) a.PrefsChanged();
  SettingEnd();
  (void)v;
}

void PageStations(Model &m, View &v, Actions &a) {
  bool ble = m.ble == 1;
  Row r = TitleBegin("Base stations", Widths({"Scan", "Wake all", "Sleep all"}));
  if (Button("Scan", kNormal, ble && !m.ble_scanning)) a.Scan();
  ImGui::SameLine(0, S(8));
  if (Button("Wake all", kNormal, ble && !m.power.empty())) a.Power("", kCmdOn);
  ImGui::SameLine(0, S(8));
  if (Button("Sleep all", kNormal, ble && !m.power.empty())) a.Power("", kCmdSleep);
  TitleEnd(r);
  Wrapped(BleLine(m), col::dim, kBody);
  Gap(10);

  struct Line {
    std::string name;
    const QlhsStation *st = nullptr;
    const StationPower *pw = nullptr;
  };
  std::vector<Line> lines;
  for (int i = 0; i < m.st.nst && i < 8; i++) lines.push_back({m.st.st[i].serial, &m.st.st[i], PowerOf(m, m.st.st[i].serial)});
  for (const auto &p : m.power) {
    bool dup = false;
    for (const auto &l : lines) dup |= Same(l.name.c_str(), p.name.c_str());
    if (!dup) lines.push_back({p.name, nullptr, &p});
  }

  float W = ImGui::GetContentRegionAvail().x;
  CardBegin(W);
  if (lines.empty()) {
    CardTitle("No base stations yet");
    Wrapped("SteamVR lists base stations while a tracker or controller is on. Bluetooth finds Lighthouse 2.0 base "
            "stations near this PC, even asleep.",
            col::dim, kBody);
  } else {
    bool has = m.st.locked || m.st.nfit > 0 || m.st.yaw_deg != 0;
    float act_w = Widths({"Wake", "Standby", "Sleep"});
    ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
    if (ImGui::BeginTable("stations", 6, tf, ImVec2(InnerWidth(), 0))) {
      ImGui::TableSetupColumn("Base station", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Power", ImGuiTableColumnFlags_WidthFixed, S(96));
      ImGui::TableSetupColumn("Seen by the cameras", ImGuiTableColumnFlags_WidthFixed, S(160));
      ImGui::TableSetupColumn("Sightings", ImGuiTableColumnFlags_WidthFixed, S(86));
      ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed, S(80));
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, act_w);
      ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
      for (int c = 0; c < 6; c++) {
        ImGui::TableSetColumnIndex(c);
        Text(ImGui::TableGetColumnName(c), col::faint, g_fonts.medium, kSmall);
      }
      float rh = S(48);
      for (size_t i = 0; i < lines.size(); i++) {
        const Line &l = lines[i];
        ImGui::TableNextRow(0, rh);
        float y0 = 0;
        auto cell = [&](int c) {
          ImGui::TableSetColumnIndex(c);
          if (c == 0) y0 = ImGui::GetCursorPosY();
          ImGui::SetCursorPosY(y0 + (rh - S(20)) * 0.5f - S(4));
        };
        cell(0);
        ImVec2 np = ImGui::GetCursorScreenPos();
        Text(l.name.c_str(), col::text, g_fonts.medium, kBody);
        const char *role = l.st ? (l.st->anchor ? "Anchor" : l.st->measured ? "Measured" : nullptr) : nullptr;
        if (role) {
          ImVec2 ns = Measure(l.name.c_str(), g_fonts.medium, kBody);
          ImGui::PushFont(g_fonts.body, kSmall);
          ImGui::GetWindowDrawList()->AddText(ImVec2(np.x + ns.x + S(10), np.y + S(2)), ImGui::GetColorU32(col::accent), role);
          ImGui::PopFont();
        }
        cell(1);
        if (l.pw) {
          ImU32 c;
          const char *t = PowerText(*l.pw, &c, l.st && l.st->last_seen >= 0 && l.st->last_seen < 15);
          Text(t, c);
        } else {
          Text("Not found", col::faint);
        }
        cell(2);
        if (l.st) Text(Ago(l.st->last_seen).c_str(), l.st->last_seen >= 0 && l.st->last_seen < 15 ? col::text : col::dim);
        else Text("Not in SteamVR", col::faint);
        cell(3);
        if (l.st && has) Text(F("%d", l.st->support).c_str());
        cell(4);
        if (l.st && l.st->dist > 0) Text(F("%.1f m", l.st->dist).c_str());
        ImGui::TableSetColumnIndex(5);
        ImGui::SetCursorPosY(y0 + (rh - S(34)) * 0.5f - S(4));
        bool can = ble && l.pw && !l.pw->busy;
        ImGui::PushID((int)i);
        if (Button("Wake", kQuiet, can)) a.Power(l.name, kCmdOn);
        ImGui::SameLine(0, S(8));
        if (Button("Standby", kQuiet, can)) a.Power(l.name, kCmdStandby);
        ImGui::SameLine(0, S(8));
        if (Button("Sleep", kQuiet, can)) a.Power(l.name, kCmdSleep);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    for (const auto &l : lines)
      if (l.pw && !l.pw->error.empty())
        Text(F("%s: %s", l.name.c_str(), l.pw->error.c_str()).c_str(), col::bad, g_fonts.body, kSmall);
    if (m.prefs.developer)
      for (const auto &l : lines)
        if (l.pw) {
          std::string raw = l.pw->raw < 0 ? "none" : F("0x%02x", l.pw->raw);
          std::string read = l.pw->read > 0 ? Ago(m.now - l.pw->read) : "never";
          Mono(F("%s  signal %d dBm, heard %s, power byte %s, read %s", l.name.c_str(), l.pw->rssi,
                 Ago(m.now - l.pw->seen).c_str(), raw.c_str(), read.c_str())
                   .c_str(),
               col::faint, kSmall);
        }
  }
  CardEnd();

  CardBegin(W);
  CardTitle("Automatic power");
  PowerSettings(m, v, a, true);
  CardEnd();
}

// ---------------------------------------------------------------- tracking view

const char *KindName(int k) {
  switch (k) {
    case kDevHmd: return "Headset";
    case kDevStation: return "Base station";
    case kDevController: return "Controller";
    case kDevTracker: return "Tracker";
    default: return "Device";
  }
}

void PageTracking(Model &m, View &v, Actions &) {
  static const char *views[] = {"From above", "From the front"};
  float sw = 0;
  for (const char *s : views) sw += Measure(s, g_fonts.medium, kBody).x + S(24);
  Row r = TitleBegin("Tracking view", sw + S(6));
  Segmented("view", &v.track_view, views, 2);
  TitleEnd(r);
  if (!m.vr) {
    Notice("The tracking view shows SteamVR's devices once SteamVR has a headset.", col::accent);
    Gap(8);
  }

  float W = ImGui::GetContentRegionAvail().x, gap = S(16), lw = S(290);
  float H = std::max(S(320), ImGui::GetContentRegionAvail().y - S(4));
  ImVec2 o = ImGui::GetCursorScreenPos();
  float cw = W - lw - gap;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(o, ImVec2(o.x + cw, o.y + H), ImGui::GetColorU32(col::card), S(10));
  dl->AddRect(o, ImVec2(o.x + cw, o.y + H), ImGui::GetColorU32(Alpha(col::line, 0.55f)), S(10), 0, S(1));
  dl->PushClipRect(ImVec2(o.x + S(1), o.y + S(1)), ImVec2(o.x + cw - S(1), o.y + H - S(1)), true);

  bool top = v.track_view == 0;
  auto P = [&](const float *p) { return top ? ImVec2(p[0], p[2]) : ImVec2(p[0], -p[1]); };
  ImVec2 lo(-1.5f, top ? -1.5f : -2.2f), hi(1.5f, top ? 1.5f : 0.2f);
  auto grow = [&](ImVec2 q) {
    lo.x = std::min(lo.x, q.x); lo.y = std::min(lo.y, q.y);
    hi.x = std::max(hi.x, q.x); hi.y = std::max(hi.y, q.y);
  };
  if (m.have_play_area)
    for (auto &c : m.play_area) grow(P(c));
  for (const Device &d : m.devices) grow(P(d.p));
  float margin = 0.6f;
  lo.x -= margin; lo.y -= margin; hi.x += margin; hi.y += margin;
  float pad = S(24);
  float scale = std::min((cw - pad * 2) / (hi.x - lo.x), (H - pad * 2) / (hi.y - lo.y));
  ImVec2 mid((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f), cen(o.x + cw * 0.5f, o.y + H * 0.5f);
  auto Scr = [&](ImVec2 w) { return ImVec2(cen.x + (w.x - mid.x) * scale, cen.y + (w.y - mid.y) * scale); };
  auto World = [&](ImVec2 s) { return ImVec2(mid.x + (s.x - cen.x) / scale, mid.y + (s.y - cen.y) / scale); };

  // a metre grid, the origin brighter
  ImVec2 wl = World(o), wh = World(ImVec2(o.x + cw, o.y + H));
  for (int gx = (int)floorf(wl.x); gx <= (int)ceilf(wh.x); gx++) {
    float x = Scr(ImVec2((float)gx, 0)).x;
    dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + H), ImGui::GetColorU32(gx == 0 ? Alpha(col::faint, 0.5f) : Alpha(col::line, 0.7f)), S(1));
  }
  for (int gy = (int)floorf(wl.y); gy <= (int)ceilf(wh.y); gy++) {
    float y = Scr(ImVec2(0, (float)gy)).y;
    dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + cw, y), ImGui::GetColorU32(gy == 0 ? Alpha(col::faint, 0.5f) : Alpha(col::line, 0.7f)), S(1));
  }
  // the play area: its outline from above, the floor from the front
  if (m.have_play_area) {
    if (top) {
      ImVec2 pts[4];
      for (int i = 0; i < 4; i++) pts[i] = Scr(P(m.play_area[i]));
      dl->AddConvexPolyFilled(pts, 4, ImGui::GetColorU32(Alpha(col::accent, 0.06f)));
      dl->AddPolyline(pts, 4, ImGui::GetColorU32(Alpha(col::accent, 0.45f)), ImDrawFlags_Closed, S(1.5f));
    } else {
      float x0 = 1e9f, x1 = -1e9f;
      for (auto &c : m.play_area) { x0 = std::min(x0, c[0]); x1 = std::max(x1, c[0]); }
      dl->AddLine(Scr(ImVec2(x0, 0)), Scr(ImVec2(x1, 0)), ImGui::GetColorU32(Alpha(col::accent, 0.6f)), S(2));
    }
  }
  // base stations' fields of view first, under everything
  for (const Device &d : m.devices) {
    if (d.kind != kDevStation || !top) continue;
    float fx = -d.R[2], fz = -d.R[8];  // the station's -Z: where it looks
    float len = sqrtf(fx * fx + fz * fz);
    if (len < 0.2f) continue;
    float ang = atan2f(fz, fx), half = 75.f * 3.14159265f / 180.f, reach = 3.5f;
    ImVec2 c = Scr(P(d.p)), e[2];
    ImU32 tone = d.tracking ? col::accent : col::faint;
    dl->PathLineTo(c);
    for (int i = 0; i <= 24; i++) {
      float t = ang - half + 2 * half * i / 24.f;
      dl->PathLineTo(Scr(ImVec2(d.p[0] + cosf(t) * reach, d.p[2] + sinf(t) * reach)));
      if (i == 0) e[0] = dl->_Path.back();
      if (i == 24) e[1] = dl->_Path.back();
    }
    dl->PathFillConcave(ImGui::GetColorU32(Alpha(tone, 0.045f)));
    for (ImVec2 q : e) dl->AddLine(c, q, ImGui::GetColorU32(Alpha(tone, 0.16f)), S(1));
  }

  // the markers, then their names wherever they fit without covering another; the list on the right has them all
  struct Tag { ImVec2 at; float r; std::string text; ImU32 col; int prio; };
  std::vector<Tag> tags;
  std::vector<ImVec4> taken;  // x0 y0 x1 y1 on screen
  auto mark = [&](ImVec2 c, float r) { taken.push_back(ImVec4(c.x - r, c.y - r, c.x + r, c.y + r)); };
  auto facing = [&](const Device &d, ImVec2 c) {
    float f3[3] = {d.p[0] - d.R[2] * 0.3f, d.p[1] - d.R[5] * 0.3f, d.p[2] - d.R[8] * 0.3f};
    ImVec2 fdir = Scr(P(f3)), dir(fdir.x - c.x, fdir.y - c.y);
    float n = sqrtf(dir.x * dir.x + dir.y * dir.y);
    return n > 0.5f ? ImVec2(dir.x / n, dir.y / n) : ImVec2(0, 0);
  };
  for (const Device &d : m.devices) {
    if (d.kind == kDevStation) {
      ImVec2 c = Scr(P(d.p)), dir = facing(d, c);
      ImU32 tone = d.tracking ? col::text : col::faint;
      float hs = std::max(S(7), 0.08f * scale);
      ImVec2 ax(dir.y, -dir.x);  // across its face
      if (dir.x == 0 && dir.y == 0) { dir = ImVec2(0, 1); ax = ImVec2(1, 0); }
      ImVec2 q[4] = {ImVec2(c.x + (ax.x + dir.x) * hs, c.y + (ax.y + dir.y) * hs), ImVec2(c.x + (-ax.x + dir.x) * hs, c.y + (-ax.y + dir.y) * hs),
                     ImVec2(c.x + (-ax.x - dir.x) * hs, c.y + (-ax.y - dir.y) * hs), ImVec2(c.x + (ax.x - dir.x) * hs, c.y + (ax.y - dir.y) * hs)};
      dl->AddConvexPolyFilled(q, 4, ImGui::GetColorU32(d.tracking ? col::raised_hi : col::raised));
      dl->AddPolyline(q, 4, ImGui::GetColorU32(tone), ImDrawFlags_Closed, S(1.5f));
      dl->AddLine(q[0], q[1], ImGui::GetColorU32(d.tracking ? col::accent : col::faint), S(3));  // its lens side
      mark(c, hs * 1.42f);
      tags.push_back({c, hs * 1.42f, d.serial, d.tracking ? col::dim : col::faint, 1});
    } else if (d.kind != kDevHmd) {
      ImVec2 c = Scr(P(d.p));
      float rr = S(5.5f);
      dl->AddCircleFilled(c, rr, ImGui::GetColorU32(d.tracking ? col::good : col::faint), 20);
      mark(c, rr);
      tags.push_back({c, rr, d.serial.empty() ? KindName(d.kind) : d.serial, d.tracking ? col::dim : col::faint, 2});
    }
  }
  for (const Device &d : m.devices) {  // the headset on top
    if (d.kind != kDevHmd) continue;
    ImVec2 c = Scr(P(d.p)), dir = facing(d, c);
    ImU32 tone = d.tracking ? col::accent : col::faint;
    float rr = S(9);
    dl->AddCircleFilled(c, rr + S(2), ImGui::GetColorU32(col::card), 28);
    dl->AddCircleFilled(c, rr, ImGui::GetColorU32(tone), 28);
    if (dir.x != 0 || dir.y != 0) {
      ImVec2 tip(c.x + dir.x * (rr + S(14)), c.y + dir.y * (rr + S(14)));
      dl->AddLine(ImVec2(c.x + dir.x * rr, c.y + dir.y * rr), tip, ImGui::GetColorU32(tone), S(2.5f));
      mark(ImVec2((c.x + tip.x) * 0.5f, (c.y + tip.y) * 0.5f), S(8));
    }
    mark(c, rr + S(2));
    tags.push_back({c, rr + S(2), "Headset", d.tracking ? col::text : col::faint, 0});
  }
  std::stable_sort(tags.begin(), tags.end(), [](const Tag &x, const Tag &y) { return x.prio < y.prio; });
  ImGui::PushFont(g_fonts.body, kSmall);
  for (const Tag &t : tags) {
    ImVec2 ts = ImGui::CalcTextSize(t.text.c_str());
    float g = S(5);
    ImVec2 spots[4] = {ImVec2(t.at.x - ts.x * 0.5f, t.at.y + t.r + g), ImVec2(t.at.x - ts.x * 0.5f, t.at.y - t.r - g - ts.y),
                       ImVec2(t.at.x + t.r + g, t.at.y - ts.y * 0.5f), ImVec2(t.at.x - t.r - g - ts.x, t.at.y - ts.y * 0.5f)};
    for (ImVec2 sp : spots) {
      ImVec4 r(sp.x - S(2), sp.y, sp.x + ts.x + S(2), sp.y + ts.y);
      if (r.x < o.x + S(4) || r.z > o.x + cw - S(4) || r.y < o.y + S(4) || r.w > o.y + H - S(30)) continue;
      bool hit = false;
      for (const ImVec4 &k : taken)
        if (r.x < k.z && k.x < r.z && r.y < k.w && k.y < r.w) { hit = true; break; }
      if (hit) continue;
      dl->AddText(ImVec2(IM_TRUNC(sp.x), IM_TRUNC(sp.y)), ImGui::GetColorU32(t.col), t.text.c_str());
      taken.push_back(r);
      break;
    }
  }
  ImGui::PopFont();
  // scale and orientation
  {
    ImVec2 b(o.x + S(16), o.y + H - S(18));
    dl->AddLine(b, ImVec2(b.x + scale, b.y), ImGui::GetColorU32(col::dim), S(2));
    dl->AddLine(ImVec2(b.x, b.y - S(4)), ImVec2(b.x, b.y + S(4)), ImGui::GetColorU32(col::dim), S(2));
    dl->AddLine(ImVec2(b.x + scale, b.y - S(4)), ImVec2(b.x + scale, b.y + S(4)), ImGui::GetColorU32(col::dim), S(2));
    ImGui::PushFont(g_fonts.body, kSmall);
    dl->AddText(ImVec2(b.x + scale + S(8), b.y - ImGui::GetFontSize() * 0.5f), ImGui::GetColorU32(col::dim), "1 m");
    const char *hint = top ? "Forward is up" : "Floor at the line";
    ImVec2 hs = ImGui::CalcTextSize(hint);
    dl->AddText(ImVec2(o.x + cw - S(16) - hs.x, b.y - hs.y * 0.5f), ImGui::GetColorU32(col::faint), hint);
    ImGui::PopFont();
  }
  dl->PopClipRect();

  // the devices, listed
  ImGui::SetCursorScreenPos(ImVec2(o.x + cw + gap, o.y));
  ImGui::BeginGroup();
  CardsBegin();
  CardBegin(lw);
  CardTitle("Devices", F("%d", (int)m.devices.size()).c_str());
  if (m.devices.empty()) Wrapped("None yet.", col::faint, kBody);
  int order[] = {kDevHmd, kDevStation, kDevController, kDevTracker, kDevOther};
  bool first = true;
  for (int k : order)
    for (const Device &d : m.devices) {
      if (d.kind != k) continue;
      if (!first) Gap(4);
      first = false;
      std::string name = d.kind == kDevHmd ? (d.model.empty() ? "Headset" : d.model) : d.serial;
      Text(name.c_str(), d.tracking ? col::text : col::dim, g_fonts.medium, kBody);
      std::string sub = std::string(KindName(d.kind)) + ", " +
                        (d.tracking ? F("%.2f, %.2f, %.2f m", d.p[0], d.p[1], d.p[2]) : std::string("not tracking"));
      Text(sub.c_str(), d.tracking ? col::dim : col::faint, g_fonts.body, kSmall);
    }
  CardEnd();
  CardsEnd();
  ImGui::EndGroup();
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(W, H));
}

// ---------------------------------------------------------------- settings

void PageSettings(Model &m, View &v, Actions &a) {
  Row r = TitleBegin("Settings", 0);
  TitleEnd(r);
  bool drv = m.have_extra && !m.stale;
  float W = ImGui::GetContentRegionAvail().x;
  if (!drv) {
    Notice("The headset and tracking settings need SteamVR running with the LighthouseSync driver.", col::warn);
    Gap(8);
  }
  const LhsExtra &x = m.ex;

  CardBegin(W);
  CardTitle("Headset connection");
  Gap(4);
  TextSetting(m, v, a, v.host, "host", x.host, "Headset address",
              "Leave it empty to find the headset on the network by itself. If your router drops broadcasts, enter the "
              "headset's IP address. Separate several with commas.",
              "Found automatically", drv);
  Divider();
  TextSetting(m, v, a, v.headset_pref, "headset", x.headset_pref, "Preferred headset",
              "When several headsets answer, use the one with this serial number. Leave it empty to take the first.",
              "Any", drv);
  Divider();
  BoolSetting(m, a, "anyHmd", x.any_hmd, "Any headset",
              "Align to SteamVR's headset even when it isn't a Quest Pro, 3, 3S or Steam Frame, such as another rooted "
              "headset running lhsyncd. Never a lighthouse headset. Takes effect after SteamVR restarts.",
              drv);
  CardEnd();

  CardBegin(W);
  CardTitle("Tracking");
  Gap(4);
  BoolSetting(m, a, "gravity", x.gravity, "Gravity levelling",
              "Levels the lighthouse space using the accelerometers of lighthouse devices while they rest.", drv);
  Divider();
  BoolSetting(m, a, "steadyStations", x.steady, "Steady base stations",
              "Places base stations at the average of their measurements instead of moving them with each new one. "
              "Takes effect after SteamVR restarts.",
              drv);
  CardEnd();

  CardBegin(W);
  CardTitle("Base stations");
  Gap(4);
  PowerSettings(m, v, a, true);
  CardEnd();

  CardBegin(W);
  CardTitle("Diagnostics");
  Gap(4);
  BoolSetting(m, a, "record", x.record, "Record every session",
              "Saves what the driver receives to a file in the data folder each session, for bug reports.", drv);
  Divider();
  SettingBegin("Developer pages", "Shows the Cameras, Diagnostics and Log pages.", S(40));
  if (Toggle("developer", &m.prefs.developer)) a.PrefsChanged();
  SettingEnd();
  Divider();
  SettingBegin("Data folder", "Logs, recordings and what LighthouseSync has learned about your headsets and base stations.",
               ButtonWidth("Open"));
  if (Button("Open##data")) a.OpenDataFolder();
  SettingEnd();
  CardEnd();

  CardBegin(W);
  CardTitle("Dashboard");
  Gap(4);
  static const char *sizes[] = {"90%", "100%", "115%", "130%", "150%"};
  static const float vals[] = {0.9f, 1.0f, 1.15f, 1.3f, 1.5f};
  int cur = 1;
  for (int i = 0; i < 5; i++)
    if (fabsf(vals[i] - m.prefs.ui_scale) < fabsf(vals[cur] - m.prefs.ui_scale)) cur = i;
  float sw = 0;
  for (const char *s : sizes) sw += Measure(s, g_fonts.medium, kBody).x + S(24);
  SettingBegin("Interface size", "How large text and controls are, here and in VR.", sw + S(6));
  if (Segmented("ui_scale", &cur, sizes, 5)) {
    m.prefs.ui_scale = vals[cur];
    a.PrefsChanged();
  }
  SettingEnd();
  Divider();
  SettingBegin("Open this window with SteamVR",
               "Shows this window on the desktop each time SteamVR starts. The page in SteamVR's dashboard is always there.",
               S(40));
  if (Toggle("open_window", &m.prefs.open_window)) a.PrefsChanged();
  SettingEnd();
  CardEnd();
}

// ---------------------------------------------------------------- updates

// release notes, as plain text: no markdown marks
std::string Plain(const std::string &md, size_t max) {
  std::string out, line;
  auto flush = [&] {
    size_t i = 0;
    while (i < line.size() && (line[i] == '#' || line[i] == ' ')) i++;
    std::string l = line.substr(i);
    if (l.size() >= 2 && (l[0] == '-' || l[0] == '*') && l[1] == ' ') l = "\xE2\x80\xA2 " + l.substr(2);  // a bullet
    std::string clean;
    for (char c : l)
      if (c != '`' && c != '*') clean += c;
    out += clean + "\n";
    line.clear();
  };
  for (char c : md) {
    if (c == '\r') continue;
    if (c == '\n') flush();
    else line += c;
  }
  if (!line.empty()) flush();
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  if (out.size() > max) out = out.substr(0, max) + "\xE2\x80\xA6";
  return out;
}

// the headset's updater (lhs/install-frame.sh's timer), through its panel
void HeadsetUpdates(const Model &m, View &v, Actions &a) {
  const PanelInfo &p = m.panel;
  KeyValue("Automatic updates", p.auto_update ? "On, every 6 hours" : "Off");
  KeyValue("Last checked", p.checking ? "Checking now" : p.checked > 0 ? Ago((double)time(nullptr) - p.checked) : "Never");
  if (!p.result.empty() && !p.checking) Text(p.result.c_str(), col::faint, g_fonts.body, kSmall + 0.5f);
  Gap(6);
  if (!p.staged.empty()) {
    Text(F("%s is downloaded. It installs the next time the headset starts.", p.staged.c_str()).c_str(), col::text,
         g_fonts.medium, kBody);
    Gap(2);
    if (Button("Install it now", kPrimary)) a.HeadsetInstall();
    Text("Installing now restarts SteamVR on the headset.", col::dim, g_fonts.body, kSmall);
  } else if (p.auto_update) {
    if (Button("Check now##headset", kNormal, !p.checking)) a.HeadsetCheck();
  } else {
    Wrapped("Run the installer on the headset again to update it, or to turn automatic updates back on:", col::dim, kBody);
    Gap(2);
    CodeLine(F("curl -fsSL https://github.com/%s/releases/latest/download/install-frame.sh | bash", m.prefs.repo.c_str()), a);
  }
}

void PageUpdates(Model &m, View &v, Actions &a) {
  const UpdateInfo &u = m.upd;
  bool busy = u.state == kUpdChecking || u.state == kUpdDownloading;
  Row r = TitleBegin("Updates", ButtonWidth("Check now"));
  if (Button("Check now", kNormal, !busy && !u.dev_build)) a.CheckUpdates();
  TitleEnd(r);
  float W = ImGui::GetContentRegionAvail().x;

  CardBegin(W);
  CardTitle("This PC");
  Gap(2);
  KeyValue("Installed", LHS_VERSION);
  if (m.have_extra && m.ex.driver_version[0] && strcmp(m.ex.driver_version, LHS_VERSION) != 0)
    KeyValue("Running in SteamVR", F("%s, until SteamVR restarts", m.ex.driver_version), col::warn);
  KeyValue("Latest", !u.latest.empty() ? (u.published.empty() ? u.latest : F("%s, released %s", u.latest.c_str(), u.published.c_str()))
                     : u.state == kUpdChecking ? "Checking" : "Not checked yet");
  KeyValue("Last checked", u.checked.empty() ? "Never" : u.checked);
  Gap(6);
  if (u.dev_build) {
    Notice("This is a development build, so it updates from git and your own builds, not from releases.", col::accent);
  } else {
    switch (u.state) {
      case kUpdChecking: Text("Checking GitHub for a new version.", col::dim); break;
      case kUpdCurrent: Text("You have the latest version.", col::good); break;
      case kUpdAvailable:
        Text(F("Version %s is available.", u.latest.c_str()).c_str(), col::text, g_fonts.medium, kBody);
        Gap(2);
        if (Button("Download and install", kPrimary)) a.InstallUpdate();
        break;
      case kUpdDownloading:
        Text(F("Downloading %s", u.latest.c_str()).c_str(), col::dim);
        Progress(u.progress, std::min(InnerWidth(), S(420)));
        break;
      case kUpdInstalled: Notice("Installed. Restart SteamVR to finish the update.", col::good); break;
      case kUpdFailed:
        Wrapped(F("Couldn't update: %s", u.error.c_str()).c_str(), col::bad, kBody);
        Gap(2);
        if (Button("Try again")) a.CheckUpdates();
        break;
      default: Text("Not checked yet.", col::dim); break;
    }
  }
  if (!u.notes.empty() && (u.state == kUpdAvailable || u.state == kUpdDownloading || u.state == kUpdInstalled)) {
    Gap(6);
    Divider();
    Text(F("What's new in %s", u.latest.c_str()).c_str(), col::text, g_fonts.medium, kBody);
    Gap(2);
    Wrapped(Plain(u.notes, 1600).c_str(), col::dim, kBody);
  }
  CardEnd();

  CardBegin(W);
  CardTitle("Headset");
  Gap(2);
  const char *model = m.st.headset[0] ? m.st.headset : nullptr;
  KeyValue("Headset", model ? model : "Not connected");
  if (m.panel.connected && !m.panel.installed.empty()) KeyValue("Installed", m.panel.installed);
  else KeyValue("Headset software", m.have_extra && m.ex.module_version[0] ? m.ex.module_version : "Unknown");
  if (!u.latest.empty()) KeyValue("Latest", u.latest);
  bool quest = model && strstr(model, "Quest");
  bool frame = model && strstr(model, "Frame");
  if (m.panel.connected) {
    HeadsetUpdates(m, v, a);
    frame = true;  // nothing about Quests here
  } else if (!quest) {
    Gap(6);
    Wrapped(frame ? "On the Steam Frame, open a terminal in desktop mode and run this:"
                  : "On a Steam Frame, open a terminal in desktop mode and run this:",
            col::dim, kBody);
    Gap(2);
    CodeLine(F("curl -fsSL https://github.com/%s/releases/latest/download/install-frame.sh | bash", m.prefs.repo.c_str()), a);
    Gap(2);
    Wrapped("It installs the latest release, checked against the published checksums, and restarts SteamVR on the "
            "headset. After that the headset keeps itself up to date: new releases download in the background and "
            "install the next time it starts.",
            col::faint, kSmall + 0.5f);
  }
  if (!frame) {
    Gap(6);
    Wrapped(quest ? "Magisk updates the module: open Magisk, go to Modules and tap Update on LighthouseSync."
                  : "On a Quest, Magisk updates the module: open Magisk, go to Modules and tap Update on LighthouseSync.",
            col::dim, kBody);
  }
  CardEnd();

  CardBegin(W);
  CardTitle("Update settings");
  Gap(4);
  SettingBegin("Check for updates automatically",
               "Looks for a new release when SteamVR starts and every 6 hours. Nothing installs until you choose to.", S(40));
  if (Toggle("auto_check", &m.prefs.auto_check)) a.PrefsChanged();
  SettingEnd();
  Divider();
  SettingBegin("Update source",
               "The GitHub repository releases come from, as owner/name. Change it to follow a fork of LighthouseSync.",
               S(340));
  if (v.vr) {
    VrText(a, "repo", "Update source", m.prefs.repo.c_str(), LHS_REPO, S(340), true);
  } else if (TextField("repo", v.repo, m.prefs.repo, S(340))) {
    if (ValidRepo(v.repo.buf)) {
      m.prefs.repo = v.repo.buf;
      v.repo_error.clear();
      a.PrefsChanged();
      a.CheckUpdates();
    } else {
      v.repo_error = "That isn't owner/name, like " LHS_REPO ".";
      v.repo.base.clear();
    }
  }
  SettingEnd();
  if (!v.repo_error.empty()) Text(v.repo_error.c_str(), col::bad, g_fonts.body, kSmall);
  SettingNote(m, "repo");
  CardEnd();
}

// ---------------------------------------------------------------- developer pages

void CameraCard(const Model &m, const LhsCam &c, float w) {
  CardBegin(w);
  CardTitle(F("Camera %d", c.id).c_str(), c.w > 0 ? F("%d x %d", c.w, c.h).c_str() : "Size not known yet");
  const char *use = c.used == 1 ? "Used for the alignment" : c.used == 0 ? "Left out of the alignment" : "Use not known yet";
  Text(use, c.used == 1 ? col::good : col::dim, g_fonts.body, kSmall + 0.5f);
  Gap(4);
  float iw = InnerWidth();
  float aspect = c.w > 0 && c.h > 0 ? (float)c.h / c.w : 0.75f;
  float ih = std::min(iw * aspect, S(300));
  float dw = ih / aspect;
  ImVec2 p = ImGui::GetCursorScreenPos();
  p.x += (iw - dw) * 0.5f;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + dw, p.y + ih), ImGui::GetColorU32(IM_COL32(8, 9, 10, 255)), S(6));
  double age = m.ex.updated - c.last;
  bool live = c.last > 0 && age < 2;
  if (c.w > 0 && c.h > 0 && live) {
    for (int i = 0; i < c.nb && i < LHS_MAX_BLOBS; i++) {
      const LhsBlob &b = c.b[i];
      ImVec2 q(p.x + (b.x10 / 10.f) / c.w * dw, p.y + (b.y10 / 10.f) / c.h * ih);
      float rr = std::max(S(2.5f), sqrtf((float)std::max<int>(b.npx, 1)) * dw / c.w * 0.9f);
      float a = 0.35f + 0.65f * std::min(1.f, b.peak / 255.f);
      dl->AddCircleFilled(q, rr, ImGui::GetColorU32(Alpha(IM_COL32(255, 244, 214, 255), a)), 16);
      dl->AddCircle(q, rr + S(3), ImGui::GetColorU32(Alpha(col::accent, 0.6f)), 20, S(1));
    }
  }
  if (!live) {
    const char *t = c.last > 0 ? "No new frames" : "No frames yet";
    ImVec2 ts = Measure(t, g_fonts.body, kSmall);
    ImGui::PushFont(g_fonts.body, kSmall);
    dl->AddText(ImVec2(p.x + (dw - ts.x) * 0.5f, p.y + (ih - ts.y) * 0.5f), ImGui::GetColorU32(col::faint), t);
    ImGui::PopFont();
  }
  ImGui::Dummy(ImVec2(iw, ih));
  Gap(4);
  KeyValue("Frames", live ? F("%.0f/s", c.fps) : "None");
  KeyValue("Bright spots", live ? F("%.0f/s, %d in the newest frame", c.blob_rate, c.nblobs < 0 ? 0 : c.nblobs) : "None");
  KeyValue("Brightness", live ? F("%d of 255", c.mean) : "Unknown");
  KeyValue("Newest frame", c.last > 0 ? Ago(age) : "Not yet");
  CardEnd();
}

void PageCameras(Model &m, View &, Actions &) {
  Row r = TitleBegin("Cameras", 0);
  TitleEnd(r);
  Wrapped("The bright spots in each tracking camera's newest frame, where base station flashes show up. The camera "
          "images stay on the headset.",
          col::dim, kBody);
  Gap(10);
  if (!m.have_extra || m.ex.ncam == 0) {
    Notice("No camera data yet. It comes in while the headset is connected and tracking.", col::accent);
    return;
  }
  float W = ImGui::GetContentRegionAvail().x, gap = S(16), cw = IM_TRUNC((W - gap) / 2);
  for (int i = 0; i < m.ex.ncam && i < LHS_MAX_CAM; i += 2) {
    CardsBegin();
    CameraCard(m, m.ex.cam[i], cw);
    if (i + 1 < m.ex.ncam) CameraCard(m, m.ex.cam[i + 1], W - cw - gap);
    CardsEnd();
  }
}

void FunnelRow(const char *label, int64_t n, int64_t of, ImU32 tone) {
  float w = InnerWidth();
  KeyValue(label, of > 0 ? F("%lld  (%.0f%%)", (long long)n, 100.0 * n / of) : F("%lld", (long long)n));
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(p.x, p.y - S(4)), ImVec2(p.x + w, p.y - S(1)), ImGui::GetColorU32(col::raised_hi), S(2));
  if (of > 0 && n > 0)
    dl->AddRectFilled(ImVec2(p.x, p.y - S(4)), ImVec2(p.x + w * (float)std::min<double>(1.0, (double)n / of), p.y - S(1)),
                      ImGui::GetColorU32(tone), S(2));
  ImGui::Dummy(ImVec2(w, S(2)));
}

void PageDiagnostics(Model &m, View &v, Actions &a) {
  Row r = TitleBegin("Diagnostics", 0);
  TitleEnd(r);
  const LhsExtra &x = m.ex;
  const QlhsStatus &s = m.st;
  float W = ImGui::GetContentRegionAvail().x, gap = S(16), cw = IM_TRUNC((W - gap) / 2);
  if (!m.have_extra) {
    Notice("The driver running in SteamVR doesn't report these details. Restart SteamVR after updating.", col::warn);
    Gap(8);
  }

  CardsBegin();
  CardBegin(cw);
  CardTitle("Bright spots", "since SteamVR started");
  Gap(2);
  KeyValue("Camera frames", F("%lld", (long long)x.spot_frames));
  FunnelRow("Spots seen", x.spot_seen, x.spot_seen, col::faint);
  FunnelRow("Used for the alignment", x.spot_used, x.spot_seen, col::accent);
  Gap(4);
  Text("Left out because", col::faint, g_fonts.medium, kSmall);
  FunnelRow("The headset was still", x.spot_still, x.spot_seen, col::faint);
  FunnelRow("The headset moved fast", x.spot_fast, x.spot_seen, col::faint);
  FunnelRow("Too large", x.spot_big, x.spot_seen, col::faint);
  FunnelRow("Not a base station", x.spot_other, x.spot_seen, col::faint);
  FunnelRow("Too dim", x.spot_dim, x.spot_seen, col::faint);
  CardEnd();
  CardBegin(W - cw - gap);
  CardTitle("Link to the headset");
  Gap(2);
  KeyValue("Connected for", x.connected_for >= 0 ? Dur(x.connected_for) : "Not connected");
  KeyValue("Lines per second", F("%.0f", x.lines_per_s));
  KeyValue("Round trip", s.rtt_ms > 0 ? F("%.1f ms", s.rtt_ms) : "Not measured");
  KeyValue("Camera frames", s.cam_fps > 0.5 ? F("%.0f/s", s.cam_fps) : "None");
  KeyValue("Last heartbeat", x.heartbeat_age >= 0 ? Ago(x.heartbeat_age) : "None yet");
  if (x.heartbeat[0]) {
    Gap(2);
    Mono(x.heartbeat, col::dim, kSmall);
  }
  if (x.camera_info[0]) {
    Gap(4);
    Text("Cameras, as the headset reports them", col::faint, g_fonts.medium, kSmall);
    Mono(x.camera_info, col::dim, kSmall);
  }
  CardEnd();
  CardsEnd();

  CardsBegin();
  CardBegin(cw);
  CardTitle("Alignment details");
  Gap(2);
  KeyValue("State", StateName(s.state));
  KeyValue("Fit condition", F("%d", s.cond));
  KeyValue("Yaw", F("%+.3f°", s.yaw_deg));
  KeyValue("Offset", F("%.3f, %.3f, %.3f m", s.t[0], s.t[1], s.t[2]));
  KeyValue("Median sighting error", s.med_deg >= 0 ? F("%.3f°", s.med_deg) : "None");
  KeyValue("Sightings in the fit", F("%d", s.nfit));
  KeyValue("Applied vs solved", F("%.2f cm", s.lag_cm));
  KeyValue("Pose timing", F("%.2f ms, %s", s.expo_ms, s.expo_learned ? "learned" : "learning"));
  KeyValue("Headset still", s.head_still == 2 ? "Yes, while its cameras see motion" : s.head_still ? "Yes" : "No");
  CardEnd();
  CardBegin(W - cw - gap);
  CardTitle("Session");
  Gap(2);
  KeyValue("Driver", m.have_extra && x.driver_version[0] ? x.driver_version : "Unknown");
  KeyValue("Dashboard", LHS_VERSION);
  KeyValue("Headset software", m.have_extra && x.module_version[0] ? x.module_version : "Unknown");
  KeyValue("Update source", m.prefs.repo);
  KeyValue("SteamVR headset", s.hmd[0] ? s.hmd : "None");
  Gap(8);
  bool live = m.have_status && !m.stale;
  if (Button(s.recording ? "Stop recording" : "Record session", s.recording ? kDanger : kNormal, live))
    a.Command(s.recording ? QLHS_CMD_RECORD_OFF : QLHS_CMD_RECORD_ON);
  ImGui::SameLine(0, S(8));
  if (Button("Open data folder")) a.OpenDataFolder();
  Gap(2);
  Wrapped(s.recording ? "Recording what the driver receives, for a bug report."
                      : "A recording saves what the driver receives to the data folder, for bug reports.",
          s.recording ? col::warn : col::faint, kSmall + 0.5f);
  CardEnd();
  CardsEnd();
}

void PageLog(Model &m, View &v, Actions &a) {
  Row r = TitleBegin("Log", Widths({"Copy all", "Open data folder"}));
  if (Button("Copy all")) {
    std::string all;
    for (const auto &l : m.log) all += l + "\n";
    a.Copy(all);
  }
  ImGui::SameLine(0, S(8));
  if (Button("Open data folder")) a.OpenDataFolder();
  TitleEnd(r);
  float W = ImGui::GetContentRegionAvail().x, H = std::max(S(240), ImGui::GetContentRegionAvail().y - S(4));
  ImVec2 o = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), ImGui::GetColorU32(IM_COL32(12, 14, 16, 255)), S(10));
  dl->AddRect(o, ImVec2(o.x + W, o.y + H), ImGui::GetColorU32(Alpha(col::line, 0.55f)), S(10), 0, S(1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(12)));
  ImGui::BeginChild("logtext", ImVec2(W, H), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
  bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - S(4);
  ImGui::PushFont(g_fonts.mono, kSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
  if (m.log.empty()) {
    for (uint32_t i = m.st.nlog > 8 ? m.st.nlog - 8 : 0; i < m.st.nlog; i++) {
      char line[121];
      memcpy(line, m.st.log[i % 8], 120);
      line[120] = 0;
      ImGui::TextUnformatted(line);
    }
  } else {
    ImGuiListClipper clip;
    clip.Begin((int)m.log.size());
    while (clip.Step())
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) ImGui::TextUnformatted(m.log[i].c_str());
  }
  ImGui::PopStyleColor();
  ImGui::PopFont();
  if (follow) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

}  // namespace

bool ValidRepo(const char *s) {
  int slash = 0;
  if (!*s || *s == '/') return false;
  for (const char *p = s; *p; p++) {
    if (*p == '/') { slash++; if (!p[1]) return false; continue; }
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') return false;
  }
  return slash == 1;
}

void Draw(Model &m, View &v, Actions &a) {
  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("##root", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
  float sw = S(214), H = io.DisplaySize.y, W = io.DisplaySize.x;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(sw, H), col::panel);
  dl->AddLine(ImVec2(sw - 0.5f, 0), ImVec2(sw - 0.5f, H), col::line);

  ImGui::SetCursorPos(ImVec2(S(14), S(22)));
  ImGui::BeginChild("nav", ImVec2(sw - S(28), H - S(44)), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
  Sidebar(m, v);
  ImGui::EndChild();

  ImGui::SetCursorPos(ImVec2(sw, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(32), S(26)));
  ImGui::BeginChild("page", ImVec2(W - sw, H), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar();
  switch (v.page) {
    case kStations: PageStations(m, v, a); break;
    case kTracking: PageTracking(m, v, a); break;
    case kSettings: PageSettings(m, v, a); break;
    case kUpdates: PageUpdates(m, v, a); break;
    case kCameras: PageCameras(m, v, a); break;
    case kDiagnostics: PageDiagnostics(m, v, a); break;
    case kLog: PageLog(m, v, a); break;
    default: PageOverview(m, v, a);
  }
  Gap(4);
  ImGui::EndChild();
  ImGui::End();
}

}  // namespace lhs::ui
