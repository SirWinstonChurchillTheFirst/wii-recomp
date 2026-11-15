// The console's network on Windows, where net_ip.c, net_ssl.c and net_routes.c aren't
// built yet: they use POSIX sockets, socket pairs and fork. The console has no link here
// (main.c sets WII_RECOMP_SHOP_SERVER=off, which net.c reads as an unplugged cable), and a
// program that opens a socket anyway is told the network can't be reached, and the
// request is named in gaps.txt.
#ifdef _WIN32
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "gaps.h"
#include "ios.h"
#include "net.h"

enum {
  SO_CLOSE = 3, SO_SOCKET = 15, SO_GETHOSTID = 16, SO_STARTUP = 26, SO_CLEANUP = 27, SO_SETINTERFACEOPT = 29,
  SO_INITINTERFACE = 31, E_BADF = 8, E_NETUNREACH = 40,
};

int net_ip_request(uint32_t command, uint32_t request) {
  uint32_t code = rd32(request + 12);
  if (command == IOS_IOCTLV) {
    if (code == SO_STARTUP || code == SO_CLEANUP || code == SO_SETINTERFACEOPT) return 0;
    gap_hit("ios.dev_net_ip_top", code);
    return -E_NETUNREACH;
  }
  if (command != IOS_IOCTL) return IOS_UNHANDLED;
  switch (code) {
    case SO_INITINTERFACE: return 0;
    case SO_GETHOSTID: return 0;  // No address: no link.
    case SO_CLOSE: return -E_BADF;
    default:
      gap_hit("ios.dev_net_ip_top", code);
      return code == SO_SOCKET ? -E_NETUNREACH : -E_BADF;
  }
}

int net_ssl_request(uint32_t command, uint32_t request) {
  (void)command;
  gap_hit("ios.dev_net_ssl", rd32(request + 12));
  return IOS_UNHANDLED;
}

void net_update(uint64_t now) { (void)now; }
int net_waiting(void) { return 0; }
void net_hold(uint32_t request, int kind, uint32_t fd, uint64_t deadline) { (void)request; (void)kind; (void)fd; (void)deadline; }
int net_ssl_retry(uint32_t request, int kind) { (void)request; (void)kind; return -1; }
int net_socket_host(uint32_t fd) { (void)fd; return -1; }
int net_socket_connected(uint32_t fd) { (void)fd; return 0; }
void net_remember_program(const char *program) { (void)program; }
uint32_t net_route_name(const char *name) { (void)name; return 0; }
int net_route_address(uint32_t address, uint16_t port, struct sockaddr_in *out) { (void)address; (void)port; (void)out; return 0; }
uint8_t *net_opera_entry(long *bytes) { (void)bytes; return 0; }
#endif
