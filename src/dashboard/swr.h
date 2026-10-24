// LighthouseSync dashboard: draws Dear ImGui's frames into an RGBA buffer on the CPU, so the same page goes to the
// desktop window, the SteamVR overlay texture and the headset's panel without a GPU renderer for each.
#pragma once
#include <stdint.h>

#include <vector>

struct ImDrawData;

namespace swr {

// RGBA8 (R in the lowest byte, as IM_COL32), rows top to bottom, opaque
struct Image {
  int w = 0, h = 0;
  std::vector<uint32_t> px;
  void Resize(int nw, int nh) {
    w = nw;
    h = nh;
    px.assign((size_t)w * h, 0xFF000000u);
  }
};

// Marks Dear ImGui's texture requests done: the atlas keeps its pixels, so draws read them from there. Call once
// per frame, before Render, for each context.
void UpdateTextures(ImDrawData *dd);

// Clears the image to `clear` (IM_COL32) and draws the frame into it.
void Render(ImDrawData *dd, Image &img, uint32_t clear);

}  // namespace swr
