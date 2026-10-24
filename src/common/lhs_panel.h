// LighthouseSync: the link between the PC's dashboard and the headset's panel (src/panel), over TCP.
// The panel is the headset's own page, drawn on the headset. The dashboard connects to the panel on the headset its
// driver is synced with only for the headset's updater: the panel reports it, and the dashboard's Updates page can ask
// it to check or install.
// A message is an 8-byte header (type, flags, zero, payload length; little-endian) and its payload.
// The panel takes a connection only from an address with a connection to lhsyncd, so only a PC synced with it.
#pragma once
#include <stdint.h>

namespace lhs::panel {

constexpr int kPort = 47282;  // lhsyncd has 47280 (TCP) and 47281 (UDP)
constexpr int kProto = 1;     // in both HELLOs: a different one is a different LighthouseSync
constexpr uint32_t kMaxPayload = 8u << 20;

#pragma pack(push, 1)
struct Header {
  uint8_t type, flags;
  uint16_t zero;
  uint32_t len;
};
#pragma pack(pop)

// dashboard to panel. 2 to 4 were v1.26.3's page images and keyboard: a panel ignores them
enum ToPanel : uint8_t {
  kHello = 1,    // "proto=1 version=1.26.3"
  kCheck = 5,    // the headset's updater looks for a release now
  kInstall = 6,  // the headset installs the release it downloaded now, restarting its SteamVR
  kPing = 7,
};

// panel to dashboard (kHello and kPing as above). 2 to 4 were v1.26.3's visibility, pointer and keyboard text: the
// dashboard ignores them
enum FromPanel : uint8_t {
  kStatus = 5,  // "key=value" lines: the headset's updater (see PanelInfo in src/dashboard/model.h)
};

}  // namespace lhs::panel
