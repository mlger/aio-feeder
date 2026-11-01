/*
 * AIO Feeder — UDP link transport for the fake-node simulator and the host
 * e2e tests (PLAN.md §13 "fake node simulator", D25).
 *
 * Host-only (POSIX sockets); never compiled on-device. One proto frame per
 * UDP datagram, 1:1 link: every outbound frame goes to the configured
 * remote endpoint, every inbound datagram is attributed to `peer_mac`
 * (the MAC is ESP-NOW layer addressing; the proto itself carries node ids).
 *
 * drop_next() lets the e2e tests exercise the hub retry policy (§6.6) and
 * the node's RESULT repeat by black-holing exactly one outbound datagram.
 */
#ifndef AIO_SIM_LINK_UDP_H
#define AIO_SIM_LINK_UDP_H

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

#include "../../link_core/link.h"

namespace aio {

class UdpLink : public ILinkTransport {
 public:
  UdpLink(uint16_t bind_port, const char *remote_ip, uint16_t remote_port,
          const uint8_t peer_mac[6], uint8_t channel = 1)
      : channel_(channel) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(bind_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    ::bind(fd_, (sockaddr *)&a, sizeof(a)); /* fixed test ports; no EADDRINUSE in CI */
    int fl = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
    if (remote_ip) set_remote(remote_ip, remote_port);
    memcpy(peer_mac_, peer_mac, 6);
  }

  ~UdpLink() override { if (fd_ >= 0) ::close(fd_); }

  void set_remote(const char *ip, uint16_t port) {
    memset(&remote_, 0, sizeof(remote_));
    remote_.sin_family = AF_INET;
    remote_.sin_port = htons(port);
    remote_.sin_addr.s_addr = inet_addr(ip);
    remote_set_ = true;
  }

  bool send(const uint8_t mac[6], const uint8_t *data, size_t len) override {
    (void)mac;
    if (drops_ > 0) { drops_--; return true; } /* black-holed: retry/repeat fodder */
    if (!remote_set_) return false;
    ::sendto(fd_, data, len, 0, (sockaddr *)&remote_, sizeof(remote_));
    return true;
  }

  void set_channel(uint8_t channel) override { channel_ = channel; }
  uint8_t channel() const override { return channel_; }

  /* Drain inbound datagrams into the engine (non-blocking). */
  void pump(LinkEngine &engine, uint32_t now_ms) {
    uint8_t buf[512];
    for (;;) {
      sockaddr_in from{};
      socklen_t fl = sizeof(from);
      ssize_t n = ::recvfrom(fd_, buf, sizeof(buf), 0, (sockaddr *)&from, &fl);
      if (n <= 0) {
        /* ECONNREFUSED (stray ICMP from a dropped datagram) — clear and stop */
        break;
      }
      engine.on_rx(peer_mac_, buf, (size_t)n, -50, now_ms);
    }
  }

  void drop_next() { drops_ = 1; }

 private:
  int fd_ = -1;
  uint8_t channel_ = 1;
  uint8_t peer_mac_[6] = {};
  sockaddr_in remote_{};
  bool remote_set_ = false;
  int drops_ = 0;
};

}  // namespace aio

#endif /* AIO_SIM_LINK_UDP_H */
