// LighthouseSync dashboard: the PC's end of the headset panel's link (src/common/lhs_panel.h). Connects to the panel on
// the headset the driver is synced with, sends it the pages drawn for it, and collects its pointer, the keyboard's text
// and its updater's state for the dashboard to act on. A headset without the panel (a Quest, upstream's package)
// refuses the connection, and it tries again every 10 seconds.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "../common/lhs_panel.h"
#include "model.h"
#include "swr.h"

namespace lhs {

struct PanelLink {
  // what the panel sent since the dashboard last took it
  std::vector<panel::Mouse> mouse;
  std::vector<std::string> texts;
  bool shown = false;  // opened in the headset's dashboard: draw it now
  PanelInfo info;      // connected, and its updater

  void Start();
  void Stop();
  void *Event() const { return ev_; }  // set when the socket has news, for MsgWaitForMultipleObjects
  // host: the headset's IP address, "" while there's none. Connects, reads, and writes what's waiting
  void Poll(double now, const std::string &host);
  bool Visible() const { return hello_ && visible_; }
  bool CanSend() const { return Visible() && out_.size() - out_off_ < (256u << 10); }
  // the rectangles of img that changed since the last frame, then Present
  void SendFrame(const swr::Image &img);
  void Keyboard(const std::string &title, const std::string &value);
  void Check() { Put(panel::kCheck, nullptr, 0); }
  void Install() { Put(panel::kInstall, nullptr, 0); }

 private:
  void Connect(double now);
  void Close(double now, double retry);
  void Fail(double now);  // couldn't connect, or it isn't a panel: wait longer each time
  void Handle(uint8_t type, const char *p, uint32_t n, double now);
  void Put(uint8_t type, const void *p, uint32_t n, const void *p2 = nullptr, uint32_t n2 = 0);
  void Flush();

  uintptr_t s_ = ~(uintptr_t)0;
  void *ev_ = nullptr;
  std::string host_;
  bool connecting_ = false, hello_ = false, visible_ = false, full_ = true, broken_ = false;  // broken_: a send failed
  double connect_at_ = 0, retry_at_ = 0, last_rx_ = 0, last_tx_ = 0, backoff_ = 0;
  std::string in_, out_;
  size_t out_off_ = 0;
  swr::Image prev_;             // what the panel has
  std::vector<uint32_t> tile_;  // one rectangle, packed for the encoder
};

}  // namespace lhs
