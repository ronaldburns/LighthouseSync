#pragma once
#include "swr.h"

namespace lhs {
// an RGBA image as a PNG (stored, not compressed)
bool WritePng(const char *path, const swr::Image &img);
// every page from sample data ("locked", "acquiring", "searching", "stale") into <dir>/<kind>-<page>.png
int RunPreview(const char *dir, const char *kind, int w, int h, float scale);
}
