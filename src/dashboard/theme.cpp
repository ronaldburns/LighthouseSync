#include "theme.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "fonts_data.h"
#include "imgui_internal.h"

namespace lhs::ui {

Fonts g_fonts;
// per context: the desktop window and the SteamVR overlay draw at different scales
float S(float v) { return v * ImGui::GetStyle().FontScaleMain; }

ImU32 Alpha(ImU32 c, float a) {
  ImU32 al = (ImU32)((float)((c >> IM_COL32_A_SHIFT) & 255) * a + 0.5f);
  return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

// with the style's alpha (disabled controls)
static ImU32 C(ImU32 c) { return ImGui::GetColorU32(c); }

std::string F(const char *fmt, ...) {
  char b[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  return b;
}

void LoadFonts(ImFontAtlas *atlas) {
  auto add = [&](const unsigned char *data, int size, const char *name) {
    ImFontConfig c;
    c.FontDataOwnedByAtlas = false;
    c.PixelSnapH = true;  // whole-pixel glyph positions: text lands on texel centres and stays sharp
    c.OversampleH = 1;
    snprintf(c.Name, sizeof c.Name, "%s", name);
    return atlas->AddFontFromMemoryTTF((void *)data, size, kBody, &c);
  };
  g_fonts.body = add(kIBMPlexSansRegular, (int)sizeof kIBMPlexSansRegular, "IBM Plex Sans");
  g_fonts.medium = add(kIBMPlexSansMedium, (int)sizeof kIBMPlexSansMedium, "IBM Plex Sans Medium");
  g_fonts.semibold = add(kIBMPlexSansSemiBold, (int)sizeof kIBMPlexSansSemiBold, "IBM Plex Sans SemiBold");
  g_fonts.mono = add(kIBMPlexMonoRegular, (int)sizeof kIBMPlexMonoRegular, "IBM Plex Mono");
}

void AtlasNewFrame(ImFontAtlas *atlas) {
  static int frame = 0;
  ImFontAtlasUpdateNewFrame(atlas, ++frame, true);
}

void ApplyStyle(float scale) {
  ImGuiStyle &st = ImGui::GetStyle();
  st = ImGuiStyle();
  ImVec4 *c = st.Colors;
  auto v = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
  c[ImGuiCol_Text] = v(col::text);
  c[ImGuiCol_TextDisabled] = v(col::faint);
  c[ImGuiCol_WindowBg] = v(col::bg);
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = v(col::card);
  c[ImGuiCol_Border] = v(col::line);
  c[ImGuiCol_FrameBg] = v(col::raised);
  c[ImGuiCol_FrameBgHovered] = v(col::raised_hi);
  c[ImGuiCol_FrameBgActive] = v(col::raised_hi);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = v(col::raised_hi);
  c[ImGuiCol_ScrollbarGrabHovered] = v(col::line);
  c[ImGuiCol_ScrollbarGrabActive] = v(col::faint);
  c[ImGuiCol_CheckMark] = v(col::accent);
  c[ImGuiCol_SliderGrab] = v(col::accent);
  c[ImGuiCol_SliderGrabActive] = v(col::accent);
  c[ImGuiCol_Button] = v(col::raised);
  c[ImGuiCol_ButtonHovered] = v(col::raised_hi);
  c[ImGuiCol_ButtonActive] = v(col::line);
  c[ImGuiCol_Header] = v(col::raised);
  c[ImGuiCol_HeaderHovered] = v(col::raised_hi);
  c[ImGuiCol_HeaderActive] = v(col::raised_hi);
  c[ImGuiCol_Separator] = v(col::line);
  c[ImGuiCol_TextSelectedBg] = v(Alpha(col::accent, 0.35f));
  c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableHeaderBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableBorderLight] = v(col::line);
  c[ImGuiCol_TableBorderStrong] = v(col::line);
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
  st.WindowPadding = ImVec2(0, 0);
  st.WindowBorderSize = 0;
  st.ChildBorderSize = 0;
  st.FrameRounding = 7;
  st.FramePadding = ImVec2(10, 8);
  st.ItemSpacing = ImVec2(10, 8);
  st.ScrollbarSize = 10;
  st.ScrollbarRounding = 6;
  st.GrabRounding = 6;
  st.PopupRounding = 8;
  st.CellPadding = ImVec2(10, 9);
  st.DisabledAlpha = 0.45f;
  st.FontSizeBase = kBody;
  st.FontScaleMain = scale;
  st.ScaleAllSizes(scale);
}

// ---------------------------------------------------------------- text

ImVec2 Measure(const char *s, ImFont *f, float size, float wrap) {
  ImGui::PushFont(f ? f : g_fonts.body, size);
  ImVec2 r = ImGui::CalcTextSize(s, ImGui::FindRenderedTextEnd(s), false, wrap);
  ImGui::PopFont();
  return r;
}

void Text(const char *s, ImU32 c, ImFont *f, float size) {
  ImGui::PushFont(f ? f : g_fonts.body, size);
  ImGui::PushStyleColor(ImGuiCol_Text, c);
  ImGui::TextUnformatted(s);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

void Title(const char *s) { Text(s, col::text, g_fonts.semibold, kTitle); }
void Heading(const char *s) { Text(s, col::text, g_fonts.medium, kHeading); }

void Wrapped(const char *s, ImU32 c, float size) {
  ImGuiWindow *w = ImGui::GetCurrentWindow();
  bool own = w->DC.TextWrapPos < 0;
  if (own) ImGui::PushTextWrapPos(0.f);
  Text(s, c, g_fonts.body, size);
  if (own) ImGui::PopTextWrapPos();
}

void Mono(const char *s, ImU32 c, float size) { Text(s, c, g_fonts.mono, size); }

static std::string Ellipsize(const char *s, ImFont *f, float size, float maxw) {
  std::string t = s;
  if (Measure(t.c_str(), f, size).x <= maxw) return t;
  while (t.size() > 1) {
    t.pop_back();
    while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();  // whole UTF-8 characters
    if (Measure((t + "\xE2\x80\xA6").c_str(), f, size).x <= maxw) break;
  }
  return t + "\xE2\x80\xA6";
}

// ---------------------------------------------------------------- controls

float ButtonWidth(const char *label) { return Measure(label, g_fonts.medium, kBody).x + S(28); }

bool Button(const char *label, ButtonKind kind, bool enabled, float width) {
  ImVec2 ts = Measure(label, g_fonts.medium, kBody);
  float h = S(34), w = width > 0 ? width : ts.x + S(28);
  if (!enabled) ImGui::BeginDisabled();
  bool pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImU32 bg = 0, fg = col::text, border = 0;
  switch (kind) {
    case kPrimary:
      bg = act ? IM_COL32(52, 96, 190, 255) : hov ? IM_COL32(78, 126, 226, 255) : col::accent_bg;
      fg = IM_COL32(255, 255, 255, 255);
      break;
    case kQuiet:
      bg = act ? col::raised_hi : hov ? col::raised : 0;
      fg = col::accent;
      break;
    case kDanger:
      bg = act ? IM_COL32(170, 62, 52, 255) : hov ? IM_COL32(214, 88, 76, 255) : IM_COL32(196, 76, 64, 255);
      fg = IM_COL32(255, 255, 255, 255);
      break;
    default:
      bg = act ? col::line : hov ? col::raised_hi : col::raised;
      border = col::line;
  }
  if (bg) dl->AddRectFilled(a, b, C(bg), S(7));
  if (border) dl->AddRect(a, b, C(border), S(7), 0, S(1));
  ImGui::PushFont(g_fonts.medium, kBody);
  dl->AddText(ImVec2(IM_TRUNC(a.x + (w - ts.x) * 0.5f), IM_TRUNC(a.y + (h - ts.y) * 0.5f)), C(fg), label,
              ImGui::FindRenderedTextEnd(label));
  ImGui::PopFont();
  if (hov && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (!enabled) ImGui::EndDisabled();
  return pressed && enabled;
}

bool Toggle(const char *id, bool *v, bool enabled) {
  float w = S(40), h = S(22);
  if (!enabled) ImGui::BeginDisabled();
  ImGui::PushID(id);
  bool pressed = ImGui::InvisibleButton("##toggle", ImVec2(w, h));
  ImGui::PopID();
  if (pressed) *v = !*v;
  bool hov = ImGui::IsItemHovered();
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImU32 track = *v ? (hov ? IM_COL32(78, 126, 226, 255) : col::accent_bg) : (hov ? col::line : col::raised_hi);
  dl->AddRectFilled(a, b, C(track), h * 0.5f);
  float r = h * 0.5f - S(3);
  ImVec2 k(*v ? b.x - h * 0.5f : a.x + h * 0.5f, a.y + h * 0.5f);
  dl->AddCircleFilled(k, r, C(*v ? IM_COL32(255, 255, 255, 255) : col::dim), 24);
  if (hov && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (!enabled) ImGui::EndDisabled();
  return pressed;
}

bool Segmented(const char *id, int *v, const char *const *items, int n, bool enabled) {
  float h = S(34), pad = S(3);
  std::vector<float> ws(n);
  float total = pad * 2;
  for (int i = 0; i < n; i++) total += ws[i] = Measure(items[i], g_fonts.medium, kBody).x + S(24);
  if (!enabled) ImGui::BeginDisabled();
  ImVec2 o = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(o, ImVec2(o.x + total, o.y + h), C(col::raised), S(7));
  bool changed = false;
  float x = o.x + pad;
  ImGui::PushID(id);
  for (int i = 0; i < n; i++) {
    ImGui::SetCursorScreenPos(ImVec2(x, o.y + pad));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##seg", ImVec2(ws[i], h - pad * 2)) && *v != i) { *v = i; changed = true; }
    ImGui::PopID();
    bool hov = ImGui::IsItemHovered(), sel = *v == i;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (sel) dl->AddRectFilled(a, b, C(col::line), S(5));
    else if (hov) dl->AddRectFilled(a, b, C(Alpha(col::raised_hi, 0.8f)), S(5));
    ImVec2 ts = Measure(items[i], g_fonts.medium, kBody);
    ImGui::PushFont(g_fonts.medium, kBody);
    dl->AddText(ImVec2(IM_TRUNC(a.x + (ws[i] - ts.x) * 0.5f), IM_TRUNC(a.y + (b.y - a.y - ts.y) * 0.5f)),
                C(sel || hov ? col::text : col::dim), items[i]);
    ImGui::PopFont();
    if (hov && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    x += ws[i];
  }
  ImGui::PopID();
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(total, h));
  if (!enabled) ImGui::EndDisabled();
  return changed;
}

bool NavItem(const char *label, bool selected, const char *note) {
  float w = ImGui::GetContentRegionAvail().x, h = S(36);
  bool pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
  bool hov = ImGui::IsItemHovered();
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (selected) dl->AddRectFilled(a, b, C(col::raised), S(7));
  else if (hov) dl->AddRectFilled(a, b, C(Alpha(col::raised, 0.55f)), S(7));
  ImVec2 ts = Measure(label, g_fonts.medium, kBody);
  ImGui::PushFont(g_fonts.medium, kBody);
  dl->AddText(ImVec2(a.x + S(12), IM_TRUNC(a.y + (h - ts.y) * 0.5f)), C(selected || hov ? col::text : col::dim), label,
              ImGui::FindRenderedTextEnd(label));
  ImGui::PopFont();
  if (note) {
    ImVec2 ns = Measure(note, g_fonts.medium, kSmall);
    ImGui::PushFont(g_fonts.medium, kSmall);
    dl->AddText(ImVec2(b.x - S(12) - ns.x, IM_TRUNC(a.y + (h - ns.y) * 0.5f)), C(col::accent), note);
    ImGui::PopFont();
  }
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  return pressed;
}

bool TextField(const char *id, Field &f, const std::string &current, float width, bool enabled, const char *hint) {
  ImGui::PushID(id);
  if (f.base != current && strcmp(f.buf, f.base.c_str()) == 0) {  // not edited: follow the setting
    snprintf(f.buf, sizeof f.buf, "%s", current.c_str());
    f.base = current;
  }
  bool dirty = current != f.buf;
  float bw = dirty ? ButtonWidth("Save") + ButtonWidth("Cancel") + S(16) : 0;
  if (!enabled) ImGui::BeginDisabled();
  ImGui::PushFont(g_fonts.body, kBody);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), IM_TRUNC((S(34) - ImGui::GetFontSize()) * 0.5f)));
  ImGui::SetNextItemWidth(width - bw);
  bool enter = ImGui::InputTextWithHint("##field", hint ? hint : "", f.buf, sizeof f.buf, ImGuiInputTextFlags_EnterReturnsTrue);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  if (ImGui::IsItemActive()) ImGui::GetWindowDrawList()->AddRect(a, b, C(Alpha(col::accent, 0.7f)), S(7), 0, S(1));
  ImGui::PopStyleVar();
  ImGui::PopFont();
  bool save = enter && dirty;
  if (dirty) {
    ImGui::SameLine(0, S(8));
    if (Button("Save", kPrimary, enabled)) save = true;
    ImGui::SameLine(0, S(8));
    if (Button("Cancel", kNormal, enabled)) snprintf(f.buf, sizeof f.buf, "%s", current.c_str());
  }
  if (!enabled) ImGui::EndDisabled();
  ImGui::PopID();
  if (save) f.base = f.buf;
  return save;
}

void Progress(float frac, float width) {
  ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + width, a.y + S(6));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, C(col::raised_hi), S(3));
  frac = std::clamp(frac, 0.f, 1.f);
  if (frac > 0) dl->AddRectFilled(a, ImVec2(a.x + width * frac, b.y), C(col::accent), S(3));
  ImGui::Dummy(ImVec2(width, S(6)));
}

// ---------------------------------------------------------------- cards

static struct {
  bool in_row = false, in_card = false;
  ImVec2 origin;
  float x = 0, maxh = 0, w = 0;
  ImVec2 card_min;
  std::vector<ImRect> rects;
} g_row;

static float Pad() { return S(18); }

void CardsBegin() {
  g_row.in_row = true;
  g_row.origin = ImGui::GetCursorScreenPos();
  g_row.x = g_row.origin.x;
  g_row.maxh = 0;
  g_row.rects.clear();
  ImGui::GetWindowDrawList()->ChannelsSplit(2);
  ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
}

void CardBegin(float width) {
  bool solo = !g_row.in_row;
  if (solo) CardsBegin();
  g_row.in_card = true;
  g_row.w = width;
  g_row.card_min = ImVec2(g_row.x, g_row.origin.y);
  ImGui::SetCursorScreenPos(ImVec2(g_row.card_min.x + Pad(), g_row.card_min.y + Pad()));
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + width - Pad() * 2);
  if (solo) g_row.in_row = false;  // CardEnd closes the row too
}

void CardEnd() {
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
  float h = ImGui::GetItemRectMax().y - g_row.card_min.y + Pad();
  g_row.rects.push_back(ImRect(g_row.card_min, ImVec2(g_row.card_min.x + g_row.w, g_row.card_min.y + h)));
  g_row.maxh = std::max(g_row.maxh, h);
  g_row.x += g_row.w + S(16);
  g_row.in_card = false;
  if (!g_row.in_row) {
    g_row.in_row = true;
    CardsEnd();
  }
}

void CardsEnd() {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->ChannelsSetCurrent(0);
  for (const ImRect &r : g_row.rects) {
    ImVec2 b(r.Max.x, r.Min.y + g_row.maxh);
    dl->AddRectFilled(r.Min, b, C(col::card), S(10));
    dl->AddRect(r.Min, b, C(Alpha(col::line, 0.55f)), S(10), 0, S(1));
  }
  dl->ChannelsMerge();
  float right = g_row.rects.empty() ? g_row.origin.x : g_row.rects.back().Max.x;
  ImGui::SetCursorScreenPos(g_row.origin);
  ImGui::Dummy(ImVec2(right - g_row.origin.x, g_row.maxh));
  ImGui::Dummy(ImVec2(0, S(8)));
  g_row.in_row = false;
}

float InnerWidth() { return g_row.in_card ? g_row.w - Pad() * 2 : ImGui::GetContentRegionAvail().x; }

void CardTitle(const char *s, const char *right, ImU32 right_col) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  Heading(s);
  if (right) {
    ImVec2 rs = Measure(right, g_fonts.body, kSmall);
    ImVec2 hs = Measure(s, g_fonts.medium, kHeading);
    ImGui::PushFont(g_fonts.body, kSmall);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + InnerWidth() - rs.x, p.y + (hs.y - rs.y) * 0.5f + S(1)), C(right_col), right);
    ImGui::PopFont();
  }
  ImGui::Dummy(ImVec2(0, S(2)));
}

void KeyValue(const char *k, const std::string &v, ImU32 vc) {
  float w = InnerWidth(), h = S(20);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 ks = Measure(k, g_fonts.body, kBody);
  std::string vs = Ellipsize(v.c_str(), g_fonts.body, kBody, w - ks.x - S(16));
  ImVec2 sz = Measure(vs.c_str(), g_fonts.body, kBody);
  ImGui::PushFont(g_fonts.body, kBody);
  dl->AddText(ImVec2(p.x, p.y + (h - ks.y) * 0.5f), C(col::dim), k);
  dl->AddText(ImVec2(IM_TRUNC(p.x + w - sz.x), p.y + (h - sz.y) * 0.5f), C(vc), vs.c_str());
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(w, h));
}

void Divider() {
  float w = InnerWidth();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + S(4)), ImVec2(p.x + w, p.y + S(4)), C(col::line), S(1));
  ImGui::Dummy(ImVec2(w, S(8)));
}

static struct {
  ImVec2 p;
  float w = 0, cw = 0, left_h = 0;
} g_set;

void SettingBegin(const char *label, const char *desc, float control_w) {
  g_set.p = ImGui::GetCursorScreenPos();
  g_set.w = InnerWidth();
  g_set.cw = control_w;
  float left_w = g_set.w - control_w - S(32);
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + left_w);
  Text(label, col::text, g_fonts.medium, kBody);
  if (desc && *desc) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(6));  // label and description read as one block
    Text(desc, col::dim, g_fonts.body, kSmall + 0.5f);
  }
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
  g_set.left_h = ImGui::GetItemRectSize().y;
  ImGui::SetCursorScreenPos(ImVec2(g_set.p.x + g_set.w - control_w, g_set.p.y));
  ImGui::BeginGroup();
}

void SettingEnd() {
  ImGui::EndGroup();
  float h = std::max(g_set.left_h, ImGui::GetItemRectSize().y);
  ImGui::SetCursorScreenPos(g_set.p);
  ImGui::Dummy(ImVec2(g_set.w, h));
}

bool Notice(const char *text, ImU32 tone, const char *button) {
  float w = InnerWidth(), pad = S(14);
  float bw = button ? ButtonWidth(button) : 0;
  float tw = w - pad * 2 - (button ? bw + S(16) : 0);
  ImVec2 ts = Measure(text, g_fonts.body, kBody, tw);
  float h = std::max(ts.y, button ? S(34) : 0.f) + S(12) * 2;
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), C(Alpha(tone, 0.13f)), S(8));
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), C(Alpha(tone, 0.30f)), S(8), 0, S(1));
  ImGui::PushFont(g_fonts.body, kBody);
  dl->AddText(nullptr, 0, ImVec2(p.x + pad, p.y + (h - ts.y) * 0.5f), C(col::text), text, nullptr, tw);
  ImGui::PopFont();
  bool pressed = false;
  if (button) {
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - pad - bw + S(4), p.y + (h - S(34)) * 0.5f));
    pressed = Button(button, kNormal);
  }
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(w, h));
  return pressed;
}

}  // namespace lhs::ui
