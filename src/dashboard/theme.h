// LighthouseSync dashboard: colours, fonts and the few widgets the pages are made of, on top of Dear ImGui.
// Sizes are written at 100% and go through S() for the interface size and the monitor's scale.
#pragma once
#include <string>

#include "imgui.h"

namespace lhs::ui {

namespace col {
constexpr ImU32 bg = IM_COL32(15, 17, 19, 255);       // page
constexpr ImU32 panel = IM_COL32(21, 24, 27, 255);    // sidebar
constexpr ImU32 card = IM_COL32(25, 28, 32, 255);
constexpr ImU32 raised = IM_COL32(34, 38, 43, 255);   // buttons, fields
constexpr ImU32 raised_hi = IM_COL32(43, 48, 54, 255);
constexpr ImU32 line = IM_COL32(44, 48, 53, 255);
constexpr ImU32 text = IM_COL32(232, 234, 237, 255);
constexpr ImU32 dim = IM_COL32(160, 166, 173, 255);
constexpr ImU32 faint = IM_COL32(111, 118, 126, 255);
constexpr ImU32 accent = IM_COL32(121, 166, 242, 255);
constexpr ImU32 accent_bg = IM_COL32(64, 112, 214, 255);
constexpr ImU32 good = IM_COL32(92, 201, 138, 255);
constexpr ImU32 warn = IM_COL32(232, 176, 74, 255);
constexpr ImU32 bad = IM_COL32(239, 107, 94, 255);
}  // namespace col

ImU32 Alpha(ImU32 c, float a);

struct Fonts {
  ImFont *body = nullptr, *medium = nullptr, *semibold = nullptr, *mono = nullptr;
};
extern Fonts g_fonts;
constexpr float kBody = 15.f, kSmall = 13.f, kHeading = 16.f, kTitle = 24.f, kMetric = 28.f;

// once, into the atlas every context shares
void LoadFonts(ImFontAtlas *atlas);
// before each NewFrame of any context: the shared atlas has no owner context to advance it
void AtlasNewFrame(ImFontAtlas *atlas);
// the context's style at this scale (interface size x monitor scale)
void ApplyStyle(float scale);
float S(float v);  // v at the current scale

std::string F(const char *fmt, ...);

// text
void Title(const char *s);
void Heading(const char *s);
void Text(const char *s, ImU32 c = col::text, ImFont *f = nullptr, float size = kBody);
void Wrapped(const char *s, ImU32 c = col::dim, float size = kBody);
void Mono(const char *s, ImU32 c = col::dim, float size = kSmall);
ImVec2 Measure(const char *s, ImFont *f, float size, float wrap = 0);

// controls
enum ButtonKind { kNormal, kPrimary, kQuiet, kDanger };
bool Button(const char *label, ButtonKind kind = kNormal, bool enabled = true, float width = 0);
float ButtonWidth(const char *label);
bool Toggle(const char *id, bool *v, bool enabled = true);
bool Segmented(const char *id, int *v, const char *const *items, int n, bool enabled = true);
bool NavItem(const char *label, bool selected, const char *note = nullptr);
// a text setting being edited: buf follows the current value until the user changes it
struct Field {
  char buf[256] = "";
  std::string base;  // the value buf started from
};
// the field, with Save and Cancel while it differs from current. Returns true on Save (f.buf holds the value).
// hint shows while it's empty
bool TextField(const char *id, Field &f, const std::string &current, float width, bool enabled = true,
               const char *hint = nullptr);
void Progress(float frac, float width);

// cards: a row of them shares one height. Inside, the cursor starts at the padding and Inner() is the content box
void CardsBegin();
void CardBegin(float width);
void CardEnd();
void CardsEnd();
float InnerWidth();
void CardTitle(const char *s, const char *right = nullptr, ImU32 right_col = col::faint);
void KeyValue(const char *k, const std::string &v, ImU32 vc = col::text);
void Divider();
// a setting: label and description on the left, its control on the right (control_w wide). Call SettingEnd after
// drawing the control at the cursor
void SettingBegin(const char *label, const char *desc, float control_w);
void SettingEnd();

// a line on its own band: info (accent), warn or bad. Returns true when its button is pressed
bool Notice(const char *text, ImU32 tone, const char *button = nullptr);

}  // namespace lhs::ui
