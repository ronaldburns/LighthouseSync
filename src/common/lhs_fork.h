// LighthouseSync: this fork's name, version and repository, and what its driver shares with its dashboard on top of
// qlhs_status.h. A second shared memory "Local\LighthouseSyncExtra", so upstream's QlhsStatus stays as it is.
// The driver publishes under a seqlock (seq odd while writing); the dashboard asks for a setting change by writing
// req_key/req_value and then incrementing req_seq, and the driver answers in ack_seq/ack.
#pragma once
#include <stdint.h>

#include "qlhs_status.h"

// lhs/stamp.py writes lhs_build.h for releases (version, repository, commit). Without it: a local build
#if defined(__has_include)
#if __has_include("lhs_build.h")
#include "lhs_build.h"
#endif
#endif

#define LHS_NAME "LighthouseSync"
#ifndef LHS_VERSION
#define LHS_VERSION QLHS_RELEASE "-dev"
#endif
// where updates come from (owner/name on GitHub): a fork's own releases build in its own
#ifndef LHS_REPO
#define LHS_REPO "ronaldburns/LighthouseSync"
#endif
#ifndef LHS_COMMIT
#define LHS_COMMIT ""
#endif

#define LHS_W_(s) L##s
#define LHS_W(s) LHS_W_(s)

#define LHS_SHM_NAME L"Local\\LighthouseSyncExtra"
#define LHS_MAGIC 0x53484C4Cu  // "LLHS"
#define LHS_SHM_VERSION 1
#define LHS_MAX_CAM 8
#define LHS_MAX_BLOBS 48

#pragma pack(push, 8)
struct LhsBlob {
  int16_t x10, y10;  // pixels * 10, as lhsight sends them
  int16_t npx, peak;
};

struct LhsCam {
  int32_t id;         // lhsight's camera number
  int32_t w, h;       // image size from the calibration, 0: not known yet
  int32_t used;       // 1: its sightings count for the alignment, 0: left out (the Frame's lower pair), -1: not known
  double last;        // driver clock (s) of its newest frame line
  double fps;         // frame lines/s
  double blob_rate;   // bright spots/s
  int32_t mean;       // the newest frame's mean brightness (0-255)
  int32_t nblobs;     // bright spots in it; b holds the first LHS_MAX_BLOBS
  int32_t nb, pad;
  LhsBlob b[LHS_MAX_BLOBS];
};

struct LhsExtra {
  uint32_t magic, version;
  volatile int32_t seq;
  int32_t pad0;
  double updated;            // driver clock (s)
  char driver_version[32];   // the running driver's LHS_VERSION
  char repo[64];             // LHS_REPO it was built with
  char module_version[32];   // the headset's lhsyncd, from its hello ("v1.26", "dev"); "" before one
  double connected_for;      // s since the headset's stream started, < 0: no stream
  double lines_per_s;        // lines from the headset per second
  double heartbeat_age;      // s since lhsight's last heartbeat (T line), < 0: none yet
  char heartbeat[128];       // that heartbeat, as sent
  char camera_info[256];     // lhsight's camera line ("0 /dev/video2 640x480 ...")
  // the cameras' bright spots since the driver started, and what became of them (Sync::Spots)
  int64_t spot_frames, spot_seen, spot_used, spot_still, spot_fast, spot_big, spot_other, spot_dim;
  int32_t ncam, pad1;
  LhsCam cam[LHS_MAX_CAM];
  // the settings as the driver has them (steamvr.vrsettings, section driver_questlhsync)
  int32_t any_hmd, gravity, steady, record;
  char host[256];
  char headset_pref[64];
  // dashboard -> driver: a setting to change
  char req_key[32];
  char req_value[256];
  volatile int32_t req_seq;
  volatile int32_t ack_seq;  // the req_seq the driver last handled
  char ack[160];             // what it did ("saved", "saved: restart SteamVR to apply", or why not)
};
#pragma pack(pop)
