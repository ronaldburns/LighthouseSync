// LighthouseSync: the link between the PC's dashboard and the headset's panel (src/panel), over TCP.
// The dashboard connects to the panel on the headset its driver is synced with, draws its pages for the panel at the
// panel's size and sends the rectangles that changed, QOI-compressed. The panel shows them in the headset's SteamVR
// dashboard and sends back its pointer, the keyboard's text and its own updater's state. Every setting, command and
// update still runs on the PC, so the panel needs no copy of the driver's state.
// A message is an 8-byte header (type, flags, zero, payload length; little-endian) and its payload.
// The panel takes a connection only from an address with a connection to lhsyncd, so only a PC synced with it.
#pragma once
#include <stdint.h>

namespace lhs::panel {

constexpr int kPort = 47282;   // lhsyncd has 47280 (TCP) and 47281 (UDP)
constexpr int kProto = 1;      // in both HELLOs: a different one is a different LighthouseSync
constexpr int kW = 1600, kH = 1000;  // the panel, in pixels (as the PC's SteamVR page)
constexpr uint32_t kMaxPayload = 8u << 20;

#pragma pack(push, 1)
struct Header {
  uint8_t type, flags;
  uint16_t zero;
  uint32_t len;
};
struct Rect {  // then the rectangle as QOI, 4 channels
  uint16_t x, y, w, h;
};
struct Mouse {
  uint8_t kind, button;  // button 0 left, 1 right
  uint16_t zero;
  float x, y;            // pixels from the top left; kScroll: the wheel's x and y
};
#pragma pack(pop)

// dashboard to panel
enum ToPanel : uint8_t {
  kHello = 1,     // "proto=1 version=1.26.3"
  kRect = 2,      // Rect, then QOI
  kPresent = 3,   // show the rectangles since the last one
  kKeyboard = 4,  // the SteamVR keyboard for a text setting: title, '\0', the current value
  kCheck = 5,     // the headset's updater looks for a release now
  kInstall = 6,   // the headset installs the release it downloaded now, restarting its SteamVR
  kPing = 7,
};

// panel to dashboard (kHello and kPing as above)
enum FromPanel : uint8_t {
  kVisible = 2,   // one byte: the panel is open in the headset's dashboard
  kMouse = 3,     // Mouse
  kText = 4,      // the keyboard's text
  kStatus = 5,    // "key=value" lines: the headset's updater (see PanelInfo in src/dashboard/model.h)
};

enum MouseKind : uint8_t { kMove, kDown, kUp, kScroll, kLeave };

}  // namespace lhs::panel
