// LighthouseSync's icon, drawn by the dashboard itself: the window's, the SteamVR dashboard thumbnail on the PC and the
// headset, and lhs/make_icon.py's files (lhs/icon.ico, the headset's PNGs).
#pragma once
#include "imgui.h"
#include "swr.h"

namespace lhs::ui {

// a base station's sweep reaching a headset, n pixels square at the draw list's origin
void DrawIcon(ImDrawList *dl, float n);

// the icon n pixels square, its rounded corners see-through, drawn with ctx (any context on the atlas)
swr::Image IconImage(ImGuiContext *ctx, ImFontAtlas *atlas, int n);

}  // namespace lhs::ui
