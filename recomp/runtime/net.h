// The console's network (net_ip.c, net_ssl.c, net_routes.c): sockets that reach only the
// local server standing in for the services the console answers itself.
#pragma once
#include <stdint.h>

struct sockaddr_in;

// /dev/net/ip/top's requests, and its errors, negated as IOS answers them (net_ip.c, and
// net_win.c's stand-in).
enum {
  SO_ACCEPT = 1, SO_BIND = 2, SO_CLOSE = 3, SO_CONNECT = 4, SO_FCNTL = 5, SO_GETPEERNAME = 6, SO_GETSOCKNAME = 7,
  SO_GETSOCKOPT = 8, SO_SETSOCKOPT = 9, SO_LISTEN = 10, SO_POLL = 11, SO_RECVFROM = 12, SO_SENDTO = 13, SO_SHUTDOWN = 14,
  SO_SOCKET = 15, SO_GETHOSTID = 16, SO_GETHOSTBYNAME = 17, SO_GETADDRINFO = 24, SO_INETATON = 21, SO_INETPTON = 22, SO_INETNTOP = 23,
  SO_STARTUP = 26, SO_CLEANUP = 27, SO_GETINTERFACEOPT = 28, SO_SETINTERFACEOPT = 29, SO_INITINTERFACE = 31,
  E_AGAIN = 6, E_ALREADY = 7, E_BADF = 8, E_CONNREFUSED = 14, E_CONNRESET = 15, E_INPROGRESS = 26, E_INVAL = 28,
  E_ISCONN = 30, E_MFILE = 33, E_NETUNREACH = 40, E_NOTCONN = 56, E_NOPROTOOPT = 51,
};

// /dev/net/ip/top and /dev/net/ssl.
int net_ip_request(uint32_t command, uint32_t request);
int net_ssl_request(uint32_t command, uint32_t request);
void net_update(uint64_t now);  // Finishes held requests that can be.
int net_waiting(void);          // How many are held.

// Held requests, shared by both devices. SSL's kinds start at NET_SSL_KINDS.
enum { NET_SSL_KINDS = 100 };
void net_hold(uint32_t request, int kind, uint32_t fd, uint64_t deadline);
int net_ssl_retry(uint32_t request, int kind);  // IOS_NO_REPLY while it must still wait.
int net_socket_host(uint32_t fd);               // The computer's socket for a Wii one, or -1.
int net_socket_connected(uint32_t fd);

// Names and addresses (net_routes.c).
void net_remember_program(const char *program);  // Where the console runs from, to find the server.
uint32_t net_route_name(const char *name);       // A made-up address for a name served here, or 0.
int net_route_address(uint32_t address, uint16_t port, struct sockaddr_in *out);  // Where it really goes.
void net_stop_local_server(void);  // Ends the local server this console started, if it did. At exit and power-off.

// The console's own certificate authority (net_routes.c), made the first time it's
// needed, in WII_RECOMP_CERTIFICATES. Its entry for Opera's list, malloc'd, or 0.
uint8_t *net_opera_entry(long *bytes);
