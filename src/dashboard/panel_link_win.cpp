#include "panel_link.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdlib.h>
#include <string.h>

#include <algorithm>

namespace lhs {

namespace {
const SOCKET kNone = INVALID_SOCKET;
}  // namespace

void PanelLink::Start() {
  WSADATA w;
  WSAStartup(MAKEWORD(2, 2), &w);
  ev_ = WSACreateEvent();
}

void PanelLink::Stop() {
  Close(0, 0);
  if (ev_) WSACloseEvent(ev_), ev_ = nullptr;
  WSACleanup();
}

void PanelLink::Close(double now, double retry) {
  if ((SOCKET)s_ != kNone) closesocket((SOCKET)s_);
  s_ = (uintptr_t)kNone;
  connecting_ = hello_ = broken_ = false;
  in_.clear();
  out_.clear();
  out_off_ = 0;
  info = PanelInfo();
  retry_at_ = now + retry;
}

void PanelLink::Fail(double now) {  // soon enough for a panel that starts after SteamVR on the PC
  backoff_ = std::min(10.0, backoff_ ? backoff_ * 2 : 2);
  Close(now, backoff_);
}

void PanelLink::Connect(double now) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(panel::kPort);
  if (inet_pton(AF_INET, host_.c_str(), &a.sin_addr) != 1) {
    retry_at_ = now + 60;
    return;
  }
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kNone) {
    retry_at_ = now + 5;
    return;
  }
  BOOL nodelay = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);
  WSAEventSelect(s, (WSAEVENT)ev_, FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE);  // also makes it non-blocking
  if (connect(s, (sockaddr *)&a, sizeof a) && WSAGetLastError() != WSAEWOULDBLOCK) {
    closesocket(s);
    Fail(now);
    return;
  }
  s_ = (uintptr_t)s;
  connecting_ = true;
  connect_at_ = last_rx_ = now;
}

void PanelLink::Poll(double now, const std::string &host) {
  if (host != host_) {
    Close(now, 0);
    host_ = host;
    backoff_ = 0;
  }
  if (host_.empty()) return;
  SOCKET s = (SOCKET)s_;
  if (s == kNone) {
    if (now >= retry_at_) Connect(now);
    return;
  }
  WSANETWORKEVENTS ne{};
  WSAEnumNetworkEvents(s, (WSAEVENT)ev_, &ne);  // and resets the event
  if (connecting_) {
    if (ne.lNetworkEvents & FD_CONNECT) {
      if (ne.iErrorCode[FD_CONNECT_BIT]) return Fail(now);  // refused: no panel there
      connecting_ = false;
      last_rx_ = now;
      std::string hello = "proto=" + std::to_string(panel::kProto) + " version=" LHS_VERSION;
      Put(panel::kHello, hello.data(), (uint32_t)hello.size());
    } else {
      if (now - connect_at_ > 5) Fail(now);
      return;
    }
  }
  char buf[16384];
  for (;;) {
    int n = recv(s, buf, sizeof buf, 0);
    if (n > 0) {
      in_.append(buf, (size_t)n);
      last_rx_ = now;
      continue;
    }
    if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) break;
    return Close(now, 2);  // closed or broken: it was a panel, so soon again
  }
  size_t at = 0;
  while (in_.size() - at >= sizeof(panel::Header)) {
    panel::Header h;
    memcpy(&h, in_.data() + at, sizeof h);
    if (h.len > panel::kMaxPayload) return Fail(now);
    if (in_.size() - at - sizeof h < h.len) break;
    Handle(h.type, in_.data() + at + sizeof h, h.len, now);
    if ((SOCKET)s_ == kNone) return;
    at += sizeof h + h.len;
  }
  in_.erase(0, at);
  if (!hello_ && now - connect_at_ > 5) return Fail(now);  // something else listens there
  if (now - last_rx_ > 15) return Close(now, 2);           // gone quiet: the headset slept or left
  if (now - last_tx_ > 5) {
    Put(panel::kPing, nullptr, 0);
    last_tx_ = now;
  }
  Flush();
  if (broken_) Close(now, 2);
}

void PanelLink::Handle(uint8_t type, const char *p, uint32_t n, double now) {
  std::string text(p, n);
  switch (type) {
    case panel::kHello: {
      int proto = 0;
      char version[64] = "";
      sscanf_s(text.c_str(), "proto=%d version=%63s", &proto, version, (unsigned)sizeof version);
      if (proto != panel::kProto) {  // another LighthouseSync: try again much later
        backoff_ = 300;
        Close(now, backoff_);
        return;
      }
      hello_ = true;
      backoff_ = 0;
      info.connected = true;
      info.version = version;
      break;
    }
    case panel::kStatus: {
      size_t at = 0;
      while (at < text.size()) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(at, e - at), k = line.substr(0, line.find('=')),
                    v = line.find('=') == std::string::npos ? "" : line.substr(line.find('=') + 1);
        if (k == "installed") info.installed = v;
        else if (k == "staged") info.staged = v;
        else if (k == "auto") info.auto_update = v == "1";
        else if (k == "checking") info.checking = v == "1";
        else if (k == "checked") info.checked = atof(v.c_str());
        else if (k == "result") info.result = v;
        at = e + 1;
      }
      break;
    }
    default: break;  // kPing, v1.26.3's pointer and page messages, and what a newer panel adds
  }
}

void PanelLink::Put(uint8_t type, const void *p, uint32_t n) {
  if ((SOCKET)s_ == kNone || connecting_) return;
  panel::Header h{type, 0, 0, n};
  out_.append((const char *)&h, sizeof h);
  if (n) out_.append((const char *)p, n);
}

void PanelLink::Flush() {
  SOCKET s = (SOCKET)s_;
  while (s != kNone && out_off_ < out_.size()) {
    int n = send(s, out_.data() + out_off_, (int)std::min<size_t>(out_.size() - out_off_, 1 << 20), 0);
    if (n > 0) {
      out_off_ += (size_t)n;
      continue;
    }
    if (WSAGetLastError() != WSAEWOULDBLOCK) broken_ = true;
    break;
  }
  if (out_off_ == out_.size()) {
    out_.clear();
    out_off_ = 0;
  } else if (out_off_ > (1u << 20)) {
    out_.erase(0, out_off_);
    out_off_ = 0;
  }
}

}  // namespace lhs
