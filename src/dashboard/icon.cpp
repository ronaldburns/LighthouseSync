#include "icon.h"

#include <algorithm>

#include "theme.h"

namespace lhs::ui {

void DrawIcon(ImDrawList *dl, float n) {
  float u = n / 256.f;
  auto P = [&](float x, float y) { return ImVec2(x * u, y * u); };
  dl->AddRectFilled(P(8, 8), P(248, 248), col::card, 56 * u);
  dl->AddRect(P(8, 8), P(248, 248), col::line, 56 * u, 0, 4 * u);
  ImVec2 lens = P(84, 92);
  dl->AddTriangleFilled(lens, P(214, 146), P(156, 214), Alpha(col::accent, 0.22f));
  dl->AddLine(lens, P(214, 146), Alpha(col::accent, 0.75f), 4 * u);
  dl->AddLine(lens, P(156, 214), Alpha(col::accent, 0.75f), 4 * u);
  dl->AddRectFilled(P(46, 54), P(122, 130), col::raised_hi, 20 * u);
  dl->AddCircleFilled(lens, 21 * u, col::bg, 40);
  dl->AddCircleFilled(lens, 11 * u, col::accent, 32);
  dl->AddRectFilled(P(144, 156), P(224, 208), col::text, 24 * u);
  dl->AddCircleFilled(P(170, 182), 8.5f * u, col::card, 24);
  dl->AddCircleFilled(P(198, 182), 8.5f * u, col::card, 24);
}

// drawn over black and over white: the difference is the alpha
swr::Image IconImage(ImGuiContext *ctx, ImFontAtlas *atlas, int n) {
  swr::Image b, w, out;
  for (swr::Image *img : {&b, &w}) {
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)n, (float)n);
    io.DeltaTime = 1.f / 60;
    AtlasNewFrame(atlas);
    ImGui::NewFrame();
    DrawIcon(ImGui::GetForegroundDrawList(), (float)n);
    ImGui::Render();
    swr::UpdateTextures(ImGui::GetDrawData());
    img->Resize(n, n);
    swr::Render(ImGui::GetDrawData(), *img, img == &b ? 0xFF000000u : 0xFFFFFFFFu);
  }
  out.Resize(n, n);
  for (size_t i = 0; i < out.px.size(); i++) {
    uint32_t pb = b.px[i], pw = w.px[i];
    int a = 255 - (int)((pw & 255) - (pb & 255));
    a = std::clamp(a, 0, 255);
    uint32_t o = (uint32_t)a << 24;
    for (int s = 0; s < 24; s += 8) {
      int c = a ? std::min(255, (int)((pb >> s) & 255) * 255 / a) : 0;
      o |= (uint32_t)c << s;
    }
    out.px[i] = o;
  }
  return out;
}

}  // namespace lhs::ui
