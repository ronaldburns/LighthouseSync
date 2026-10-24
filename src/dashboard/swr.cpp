#include "swr.h"

#include <math.h>
#include <string.h>

#include <algorithm>

#include "imgui.h"

namespace swr {
namespace {

// what a draw command samples: the font atlas (Dear ImGui's texture data), an Image the page passed as its
// ImTextureID, or nothing (white)
struct Tex {
  const uint8_t *px = nullptr;
  int w = 1, h = 1, bpp = 4;
};

inline uint32_t Fetch(const Tex &t, int x, int y) {
  x = x < 0 ? 0 : x >= t.w ? t.w - 1 : x;
  y = y < 0 ? 0 : y >= t.h ? t.h - 1 : y;
  if (t.bpp == 4) return ((const uint32_t *)t.px)[(size_t)y * t.w + x];
  return ((uint32_t)t.px[(size_t)y * t.w + x] << 24) | 0x00FFFFFFu;
}

// bilinear, as the GPU backends' linear samplers: at texel centres (text drawn at its own size) it's the texel
inline uint32_t Sample(const Tex &t, float u, float v) {
  if (!t.px) return 0xFFFFFFFFu;
  float x = u * t.w - 0.5f, y = v * t.h - 0.5f;
  float fx0 = floorf(x), fy0 = floorf(y);
  int x0 = (int)fx0, y0 = (int)fy0;
  int fx = (int)((x - fx0) * 256.f + 0.5f), fy = (int)((y - fy0) * 256.f + 0.5f);
  if (fx >= 256) { x0++; fx = 0; }
  if (fy >= 256) { y0++; fy = 0; }
  uint32_t a = Fetch(t, x0, y0);
  if (!fx && !fy) return a;
  uint32_t b = Fetch(t, x0 + 1, y0), c = Fetch(t, x0, y0 + 1), d = Fetch(t, x0 + 1, y0 + 1);
  uint32_t out = 0;
  for (int s = 0; s < 32; s += 8) {
    int ca = (a >> s) & 255, cb = (b >> s) & 255, cc = (c >> s) & 255, cd = (d >> s) & 255;
    int top = ca * 256 + (cb - ca) * fx, bot = cc * 256 + (cd - cc) * fx;
    int v8 = (top * 256 + (bot - top) * fy + 32768) >> 16;
    out |= (uint32_t)v8 << s;
  }
  return out;
}

inline uint32_t Mul8(uint32_t a, uint32_t b) {  // (a * b + 127) / 255, close enough
  uint32_t t = a * b + 128;
  return (t + (t >> 8)) >> 8;
}

inline uint32_t Modulate(uint32_t c, uint32_t t) {
  if (t == 0xFFFFFFFFu) return c;
  return Mul8(c & 255, t & 255) | Mul8((c >> 8) & 255, (t >> 8) & 255) << 8 | Mul8((c >> 16) & 255, (t >> 16) & 255) << 16 |
         Mul8(c >> 24, t >> 24) << 24;
}

// straight alpha over an opaque target
inline void Blend(uint32_t &dst, uint32_t src) {
  uint32_t a = src >> 24;
  if (!a) return;
  if (a == 255) { dst = src; return; }
  uint32_t ia = 255 - a;
  uint32_t rb = (src & 0x00FF00FFu) * a + (dst & 0x00FF00FFu) * ia + 0x00800080u;
  rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
  uint32_t g = ((src >> 8) & 255) * a + ((dst >> 8) & 255) * ia + 128;
  g = ((g + (g >> 8)) >> 8) & 255;
  dst = 0xFF000000u | rb | g << 8;
}

inline void Span(uint32_t *row, int x0, int x1, uint32_t c) {
  if ((c >> 24) == 255)
    std::fill(row + x0, row + x1, c);
  else
    for (int x = x0; x < x1; x++) Blend(row[x], c);
}

struct Clip {
  int x0, y0, x1, y1;
};

// a quad Dear ImGui wrote with PrimRect/PrimRectUV (filled rectangles, text glyphs): corners TL TR BR BL, one
// colour, texture coordinates aligned with the axes
bool Rect(Image &img, const Clip &cl, const Tex &t, const ImDrawVert *v, const ImDrawIdx *ix, const ImVec2 &off,
          const ImVec2 &sc) {
  if (ix[3] != ix[0] || ix[4] != ix[2]) return false;
  const ImDrawVert &a = v[ix[0]], &b = v[ix[1]], &c = v[ix[2]], &d = v[ix[5]];
  if (a.pos.y != b.pos.y || b.pos.x != c.pos.x || c.pos.y != d.pos.y || d.pos.x != a.pos.x) return false;
  if (a.col != b.col || a.col != c.col || a.col != d.col) return false;
  if (a.uv.y != b.uv.y || b.uv.x != c.uv.x || c.uv.y != d.uv.y || d.uv.x != a.uv.x) return false;
  if (!(a.pos.x < b.pos.x && a.pos.y < d.pos.y)) return false;
  float x0 = (a.pos.x - off.x) * sc.x, x1 = (c.pos.x - off.x) * sc.x;
  float y0 = (a.pos.y - off.y) * sc.y, y1 = (c.pos.y - off.y) * sc.y;
  // the pixels whose centres are inside, as the triangles would cover them
  int px0 = std::max(cl.x0, (int)ceilf(x0 - 0.5f)), px1 = std::min(cl.x1, (int)ceilf(x1 - 0.5f));
  int py0 = std::max(cl.y0, (int)ceilf(y0 - 0.5f)), py1 = std::min(cl.y1, (int)ceilf(y1 - 0.5f));
  if (px0 >= px1 || py0 >= py1) return true;
  bool solid = a.uv.x == c.uv.x && a.uv.y == c.uv.y;
  if (solid) {
    uint32_t col = Modulate(a.col, Sample(t, a.uv.x, a.uv.y));
    if (!(col >> 24)) return true;
    for (int y = py0; y < py1; y++) Span(&img.px[(size_t)y * img.w], px0, px1, col);
    return true;
  }
  float du = (c.uv.x - a.uv.x) / (x1 - x0), dv = (c.uv.y - a.uv.y) / (y1 - y0);
  for (int y = py0; y < py1; y++) {
    uint32_t *row = &img.px[(size_t)y * img.w];
    float vv = a.uv.y + (y + 0.5f - y0) * dv;
    for (int x = px0; x < px1; x++) {
      uint32_t col = Modulate(a.col, Sample(t, a.uv.x + (x + 0.5f - x0) * du, vv));
      Blend(row[x], col);
    }
  }
  return true;
}

// any other triangle: edge functions in 24.8 fixed point (exact, so the top-left rule never draws a shared edge
// twice), colour and texture coordinates interpolated
void Tri(Image &img, const Clip &cl, const Tex &t, const ImDrawVert *p0, const ImDrawVert *p1, const ImDrawVert *p2,
         const ImVec2 &off, const ImVec2 &sc) {
  auto fx = [&](float x) { return (int64_t)llroundf((x - off.x) * sc.x * 256.f); };
  auto fy = [&](float y) { return (int64_t)llroundf((y - off.y) * sc.y * 256.f); };
  int64_t x0 = fx(p0->pos.x), y0 = fy(p0->pos.y), x1 = fx(p1->pos.x), y1 = fy(p1->pos.y), x2 = fx(p2->pos.x),
          y2 = fy(p2->pos.y);
  int64_t area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
  if (area == 0) return;
  if (area < 0) {
    std::swap(p1, p2);
    std::swap(x1, x2);
    std::swap(y1, y2);
    area = -area;
  }
  int bx0 = std::max(cl.x0, (int)(std::min({x0, x1, x2}) >> 8)), bx1 = std::min(cl.x1, (int)((std::max({x0, x1, x2}) + 255) >> 8) + 1);
  int by0 = std::max(cl.y0, (int)(std::min({y0, y1, y2}) >> 8)), by1 = std::min(cl.y1, (int)((std::max({y0, y1, y2}) + 255) >> 8) + 1);
  if (bx0 >= bx1 || by0 >= by1) return;
  // E(P,Q)(p) = (Q.x-P.x)(p.y-P.y) - (Q.y-P.y)(p.x-P.x): > 0 inside; on the edge counts for top and left edges
  struct Edge {
    int64_t dx, dy, px, py;
    bool tl;
    int64_t At(int64_t x, int64_t y) const { return dx * (y - py) - dy * (x - px); }
  };
  Edge e0{x2 - x1, y2 - y1, x1, y1, false}, e1{x0 - x2, y0 - y2, x2, y2, false}, e2{x1 - x0, y1 - y0, x0, y0, false};
  for (Edge *e : {&e0, &e1, &e2}) e->tl = e->dy < 0 || (e->dy == 0 && e->dx > 0);
  const ImU32 c0 = p0->col, c1 = p1->col, c2 = p2->col;
  bool flat_col = c0 == c1 && c0 == c2;
  bool flat_uv = p0->uv.x == p1->uv.x && p0->uv.x == p2->uv.x && p0->uv.y == p1->uv.y && p0->uv.y == p2->uv.y;
  uint32_t flat = 0;
  if (flat_col && flat_uv) {
    flat = Modulate(c0, Sample(t, p0->uv.x, p0->uv.y));
    if (!(flat >> 24)) return;
  }
  float inv = 1.f / (float)area;
  float ch0[4], ch1[4], ch2[4];
  for (int k = 0; k < 4; k++) {
    ch0[k] = (float)((c0 >> (8 * k)) & 255);
    ch1[k] = (float)((c1 >> (8 * k)) & 255);
    ch2[k] = (float)((c2 >> (8 * k)) & 255);
  }
  for (int y = by0; y < by1; y++) {
    int64_t sy = (int64_t)y * 256 + 128;
    uint32_t *row = &img.px[(size_t)y * img.w];
    int64_t sx = (int64_t)bx0 * 256 + 128;
    int64_t w0 = e0.At(sx, sy), w1 = e1.At(sx, sy), w2 = e2.At(sx, sy);
    const int64_t s0 = -e0.dy * 256, s1 = -e1.dy * 256, s2 = -e2.dy * 256;
    for (int x = bx0; x < bx1; x++, w0 += s0, w1 += s1, w2 += s2) {
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      if ((w0 == 0 && !e0.tl) || (w1 == 0 && !e1.tl) || (w2 == 0 && !e2.tl)) continue;
      if (flat_col && flat_uv) {
        Blend(row[x], flat);
        continue;
      }
      float l0 = (float)w0 * inv, l1 = (float)w1 * inv, l2 = 1.f - l0 - l1;
      uint32_t col;
      if (flat_col)
        col = c0;
      else {
        col = 0;
        for (int k = 0; k < 4; k++) {
          int v8 = (int)(ch0[k] * l0 + ch1[k] * l1 + ch2[k] * l2 + 0.5f);
          col |= (uint32_t)(v8 < 0 ? 0 : v8 > 255 ? 255 : v8) << (8 * k);
        }
      }
      if (!flat_uv)
        col = Modulate(col, Sample(t, p0->uv.x * l0 + p1->uv.x * l1 + p2->uv.x * l2, p0->uv.y * l0 + p1->uv.y * l1 + p2->uv.y * l2));
      else
        col = Modulate(col, Sample(t, p0->uv.x, p0->uv.y));
      Blend(row[x], col);
    }
  }
}

}  // namespace

void UpdateTextures(ImDrawData *dd) {
  if (!dd || !dd->Textures) return;
  for (ImTextureData *t : *dd->Textures) {
    switch (t->Status) {
      case ImTextureStatus_WantCreate:
        t->SetTexID((ImTextureID)(intptr_t)t);
        t->SetStatus(ImTextureStatus_OK);
        break;
      case ImTextureStatus_WantUpdates: t->SetStatus(ImTextureStatus_OK); break;
      case ImTextureStatus_WantDestroy:
        if (t->UnusedFrames > 0) {
          t->SetTexID(ImTextureID_Invalid);
          t->SetStatus(ImTextureStatus_Destroyed);
        }
        break;
      default: break;
    }
  }
}

void Render(ImDrawData *dd, Image &img, uint32_t clear) {
  std::fill(img.px.begin(), img.px.end(), clear | 0xFF000000u);
  if (!dd || !dd->Valid || img.w <= 0 || img.h <= 0) return;
  const ImVec2 off = dd->DisplayPos, sc = dd->FramebufferScale;
  for (const ImDrawList *dl : dd->CmdLists) {
    const ImDrawVert *vtx = dl->VtxBuffer.Data;
    const ImDrawIdx *idx = dl->IdxBuffer.Data;
    for (const ImDrawCmd &cmd : dl->CmdBuffer) {
      if (cmd.UserCallback) {
        if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(dl, &cmd);
        continue;
      }
      Clip cl{std::max(0, (int)floorf((cmd.ClipRect.x - off.x) * sc.x)), std::max(0, (int)floorf((cmd.ClipRect.y - off.y) * sc.y)),
              std::min(img.w, (int)ceilf((cmd.ClipRect.z - off.x) * sc.x)), std::min(img.h, (int)ceilf((cmd.ClipRect.w - off.y) * sc.y))};
      if (cl.x0 >= cl.x1 || cl.y0 >= cl.y1) continue;
      Tex t;
      if (ImTextureData *td = cmd.TexRef._TexData) {
        if (td->Pixels) t = Tex{td->Pixels, td->Width, td->Height, td->BytesPerPixel};
      } else if (cmd.TexRef._TexID != ImTextureID_Invalid) {
        const Image *im = (const Image *)(intptr_t)cmd.TexRef._TexID;
        if (im->w > 0 && im->h > 0) t = Tex{(const uint8_t *)im->px.data(), im->w, im->h, 4};
      }
      const ImDrawVert *v = vtx + cmd.VtxOffset;
      const ImDrawIdx *ix = idx + cmd.IdxOffset;
      for (unsigned i = 0; i + 2 < cmd.ElemCount;) {
        if (i + 5 < cmd.ElemCount && Rect(img, cl, t, v, ix + i, off, sc)) {
          i += 6;
          continue;
        }
        Tri(img, cl, t, &v[ix[i]], &v[ix[i + 1]], &v[ix[i + 2]], off, sc);
        i += 3;
      }
    }
  }
}

}  // namespace swr
