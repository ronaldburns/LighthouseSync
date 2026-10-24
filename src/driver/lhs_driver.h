// LighthouseSync's additions to the driver, kept out of upstream's files apart from the calls into them: the extra
// status for the dashboard (lhs_fork.h: versions, each camera's bright spots, the link's heartbeat), settings changed
// from the dashboard, and the files a dashboard update leaves behind.
#pragma once
#include <functional>
#include <string>

class Sync;
class HeadsetLink;

namespace lhs {
using LogFn = std::function<void(const std::string &)>;
// Provider::Init: maps the shared memory and removes the old files an update renamed aside
void Init(LogFn log);
// net.cpp, the headset link's thread: every line from the headset, and the camera calibration once it's read
void OnLine(double now, const std::string &line);
void OnCalibration(const std::string &json);
// Provider::Worker, every pass: handles a setting change from the dashboard (reload re-reads the settings, as
// ReadSettings at start) and publishes the extra status
void Tick(double now, Sync &sync, const HeadsetLink &link, const std::function<void()> &reload);
}  // namespace lhs
