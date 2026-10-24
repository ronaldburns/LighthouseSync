// LighthouseSync dashboard: the PC's end of the headset panel's link (src/common/lhs_panel.h). Connects to the panel on
// the headset the driver is synced with and collects its updater's state for the Updates page, which can ask it to
// check or install. A headset without the panel (a Quest, upstream's package) refuses the connection, and it tries
// again every 10 seconds.
#pragma once
#include <stdint.h>

#include <string>

#include "../common/lhs_panel.h"
#include "model.h"

namespace lhs {

struct PanelLink {
  PanelInfo info;  // connected, and its updater

  void Start();
  void Stop();
  void *Event() const { return ev_; }  // set when the socket has news, for MsgWaitForMultipleObjects
  // host: the headset's IP address, "" while there's none. Connects, reads, and writes what's waiting
  void Poll(double now, const std::string &host);
  void Check() { Put(panel::kCheck, nullptr, 0); }
  void Install() { Put(panel::kInstall, nullptr, 0); }

 private:
  void Connect(double now);
  void Close(double now, double retry);
  void Fail(double now);  // couldn't connect, or it isn't a panel: wait longer each time
  void Handle(uint8_t type, const char *p, uint32_t n, double now);
  void Put(uint8_t type, const void *p, uint32_t n);
  void Flush();

  uintptr_t s_ = ~(uintptr_t)0;
  void *ev_ = nullptr;
  std::string host_;
  bool connecting_ = false, hello_ = false, broken_ = false;  // broken_: a send failed
  double connect_at_ = 0, retry_at_ = 0, last_rx_ = 0, last_tx_ = 0, backoff_ = 0;
  std::string in_, out_;
  size_t out_off_ = 0;
};

}  // namespace lhs
