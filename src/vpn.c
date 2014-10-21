/**
  vpn.c

  Copyright (c) 2014 clowwindy

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.

*/

// TODO we want to put shadowvpn.h at the bottom of the imports
// but TARGET_* is defined in config.h
#include "shadowvpn.h"

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#ifndef TARGET_WIN32
#include <sys/select.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#endif

#ifdef TARGET_DARWIN
#include <sys/kern_control.h>
#include <net/if_utun.h>
#include <sys/sys_domain.h>
#include <netinet/ip.h>
#include <sys/uio.h>
#endif

#ifdef TARGET_LINUX
#include <linux/if_tun.h>
#endif

#ifdef TARGET_FREEBSD
#include <net/if.h>
#include <net/if_tun.h>
#endif

/*
 * Darwin & OpenBSD use utun which is slightly
 * different from standard tun device. It adds
 * a uint32 to the beginning of the IP header
 * to designate the protocol.
 *
 * We use utun_read to strip off the header
 * and utun_write to put it back.
 */
#ifdef TARGET_DARWIN
#define tun_read(...) utun_read(__VA_ARGS__)
#define tun_write(...) utun_write(__VA_ARGS__)
#elif !defined(TARGET_WIN32)
#define tun_read(...) read(__VA_ARGS__)
#define tun_write(...) write(__VA_ARGS__)
#endif

#ifdef TARGET_WIN32

#undef errno
#undef EWOULDBLOCK
#undef EAGAIN
#undef EINTR
#undef ENETDOWN
#undef ENETUNREACH
#undef EMSGSIZE

#define errno WSAGetLastError()
#define EWOULDBLOCK WSAEWOULDBLOCK
#define EAGAIN WSAEWOULDBLOCK
#define EINTR WSAEINTR
#define ENETUNREACH WSAENETUNREACH
#define ENETDOWN WSAENETDOWN
#define EMSGSIZE WSAEMSGSIZE
#define close(fd) closesocket(fd)

#endif

#ifdef TARGET_LINUX
int vpn_tun_alloc(const char *dev) {
  struct ifreq ifr;
  int fd, e;

  if ((fd = open("/dev/net/tun", O_RDWR)) < 0) {
    err("open");
    errf("can not open /dev/net/tun");
    return -1;
  }

  memset(&ifr, 0, sizeof(ifr));

  /* Flags: IFF_TUN   - TUN device (no Ethernet headers)
   *        IFF_TAP   - TAP device
   *
   *        IFF_NO_PI - Do not provide packet information
   */
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  if(*dev)
    strncpy(ifr.ifr_name, dev, IFNAMSIZ);

  if ((e = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0) {
    err("ioctl[TUNSETIFF]");
    errf("can not setup tun device: %s", dev);
    close(fd);
    return -1;
  }
  // strcpy(dev, ifr.ifr_name);
  return fd;
}
#endif

#ifdef TARGET_FREEBSD
int vpn_tun_alloc(const char *dev) {
  int fd;
  char devname[32]={0,};
  snprintf(devname, sizeof(devname), "/dev/%s", dev);
  if ((fd = open(devname, O_RDWR)) < 0) {
    err("open");
    errf("can not open %s", devname);
    return -1;
  }
  int i = IFF_POINTOPOINT | IFF_MULTICAST;
  if (ioctl(fd, TUNSIFMODE, &i) < 0) {
    err("ioctl[TUNSIFMODE]");
    errf("can not setup tun device: %s", dev);
    close(fd);
    return -1;
  }
  i = 0;
  if (ioctl(fd, TUNSIFHEAD, &i) < 0) {
    err("ioctl[TUNSIFHEAD]");
    errf("can not setup tun device: %s", dev);
    close(fd);
    return -1;
  }
  return fd;
}
#endif

#ifdef TARGET_DARWIN
static inline int utun_modified_len(int len) {
  if (len > 0)
    return (len > sizeof (u_int32_t)) ? len - sizeof (u_int32_t) : 0;
  else
    return len;
}

static int utun_write(int fd, void *buf, size_t len) {
  u_int32_t type;
  struct iovec iv[2];
  struct ip *iph;

  iph = (struct ip *) buf;

  if (iph->ip_v == 6)
    type = htonl(AF_INET6);
  else
    type = htonl(AF_INET);

  iv[0].iov_base = &type;
  iv[0].iov_len = sizeof(type);
  iv[1].iov_base = buf;
  iv[1].iov_len = len;

  return utun_modified_len(writev(fd, iv, 2));
}

static int utun_read(int fd, void *buf, size_t len) {
  u_int32_t type;
  struct iovec iv[2];

  iv[0].iov_base = &type;
  iv[0].iov_len = sizeof(type);
  iv[1].iov_base = buf;
  iv[1].iov_len = len;

  return utun_modified_len(readv(fd, iv, 2));
}

int vpn_tun_alloc(const char *dev) {
  struct ctl_info ctlInfo;
  struct sockaddr_ctl sc;
  int fd;
  int utunnum;

  if (dev == NULL) {
    errf("utun device name cannot be null");
    return -1;
  }
  if (sscanf(dev, "utun%d", &utunnum) != 1) {
    errf("invalid utun device name: %s", dev);
    return -1;
  }

  memset(&ctlInfo, 0, sizeof(ctlInfo));
  if (strlcpy(ctlInfo.ctl_name, UTUN_CONTROL_NAME, sizeof(ctlInfo.ctl_name)) >=
      sizeof(ctlInfo.ctl_name)) {
    errf("can not setup utun device: UTUN_CONTROL_NAME too long");
    return -1;
  }

  fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);

  if (fd == -1) {
    err("socket[SYSPROTO_CONTROL]");
    return -1;
  }

  if (ioctl(fd, CTLIOCGINFO, &ctlInfo) == -1) {
    close(fd);
    err("ioctl[CTLIOCGINFO]");
    return -1;
  }

  sc.sc_id = ctlInfo.ctl_id;
  sc.sc_len = sizeof(sc);
  sc.sc_family = AF_SYSTEM;
  sc.ss_sysaddr = AF_SYS_CONTROL;
  sc.sc_unit = utunnum + 1;

  if (connect(fd, (struct sockaddr *) &sc, sizeof(sc)) == -1) {
    close(fd);
    err("connect[AF_SYS_CONTROL]");
    return -1;
  }

  return fd;
}
#endif

#ifdef TARGET_WIN32
static int tun_write(int tun_fd, char *data, size_t len) {
  DWORD written;
  DWORD res;
  OVERLAPPED olpd;

  olpd.Offset = 0;
  olpd.OffsetHigh = 0;
  olpd.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
  res = WriteFile(dev_handle, data, len, &written, &olpd);
  if (!res && GetLastError() == ERROR_IO_PENDING) {
    WaitForSingleObject(olpd.hEvent, INFINITE);
    res = GetOverlappedResult(dev_handle, &olpd, &written, FALSE);
    if (written != len) {
      return -1;
    }
  }
  return 0;
}

static int tun_read(int tun_fd, char *buf, size_t len) {
  return recv(tun_fd, buf, len, 0);
}

int vpn_tun_alloc(const char *dev) {
  return tun_open(dev);
}
#endif

int vpn_udp_alloc(int if_bind, const char *host, int port,
                  struct sockaddr *addr, socklen_t* addrlen) {
  struct addrinfo hints;
  struct addrinfo *res;
  int sock, r, flags;

  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  if (0 != (r = getaddrinfo(host, NULL, &hints, &res))) {
    errf("getaddrinfo: %s", gai_strerror(r));
    return -1;
  }

  if (res->ai_family == AF_INET)
    ((struct sockaddr_in *)res->ai_addr)->sin_port = htons(port);
  else if (res->ai_family == AF_INET6)
    ((struct sockaddr_in6 *)res->ai_addr)->sin6_port = htons(port);
  else {
    errf("unknown ai_family %d", res->ai_family);
    return -1;
  }
  memcpy(addr, res->ai_addr, res->ai_addrlen);
  *addrlen = res->ai_addrlen;

  if (-1 == (sock = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP))) {
    err("socket");
    errf("can not create socket");
    return -1;
  }

  if (if_bind) {
    if (0 != bind(sock, res->ai_addr, res->ai_addrlen)) {
      err("bind");
      errf("can not bind %s:%d", host, port);
      return -1;
    }
    freeaddrinfo(res);
  }
#ifdef TARGET_WIN32
  u_long mode = 0;
  if (NO_ERROR == ioctlsocket(sock, FIONBIO, &mode))
    return disable_reset_report(sock);
  close(sock);
  err("ioctlsocket");
#else
  flags = fcntl(sock, F_GETFL, 0);
  if (flags != -1) {
    if (-1 != fcntl(sock, F_SETFL, flags | O_NONBLOCK))
      return sock;
  }
  close(sock);
  err("fcntl");
#endif
  return -1;
}

#ifndef TARGET_WIN32
static int max(int a, int b) {
  return a > b ? a : b;
}
#endif

int vpn_ctx_init(vpn_ctx_t *ctx, shadowvpn_args_t *args) {
#ifdef TARGET_WIN32
  WORD wVersionRequested;
  WSADATA wsaData;
  int ret;

  wVersionRequested = MAKEWORD(1, 1);
  ret = WSAStartup(wVersionRequested, &wsaData);
  if (ret != 0) {
    errf("can not initialize winsock");
    return -1;
  }
  if (LOBYTE(wsaData.wVersion) != 1 || HIBYTE(wsaData.wVersion) != 1) {
    WSACleanup();
    errf("can not find a usable version of winsock");
    return -1;
  }
#endif

  bzero(ctx, sizeof(vpn_ctx_t));
  ctx->remote_addrp = (struct sockaddr *)&ctx->remote_addr;

#ifndef TARGET_WIN32
  if (-1 == pipe(ctx->control_pipe)) {
    err("pipe");
    return -1;
  }
#endif
  if (-1 == (ctx->tun = vpn_tun_alloc(args->intf))) {
    errf("failed to create tun device");
    return -1;
  }
  if (-1 == (ctx->sock = vpn_udp_alloc(args->mode == SHADOWVPN_MODE_SERVER,
                                       args->server, args->port,
                                       ctx->remote_addrp,
                                       &ctx->remote_addrlen))) {
    errf("failed to create UDP socket");
    close(ctx->tun);
    return -1;
  }
  ctx->args = args;
  return 0;
}

int ifconfig_up(shadowvpn_args_t *args) {
#if defined(TARGET_LINUX)
  struct ifreq ifr;
  bzero(&ifr, sizeof(ifr));
#elif defined(TARGET_DARWIN) || defined (TARGET_FREEBSD)
  struct ifaliasreq      areq;
  bzero(&areq, sizeof(areq));
#endif
  struct in_addr sa_addr;
  struct in_addr sa_dstaddr;
  struct in_addr sa_netmask;
  int fd;
  inet_aton(args->tun_local_ip, &sa_addr);
  inet_aton(args->tun_netmask, &sa_netmask);
  inet_aton(args->tun_remote_ip, &sa_dstaddr);

  /* Create a channel to the NET kernel. */
  if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    err("socket");
    errf("can not open socket");
    return -1;
  }
#if defined(TARGET_DARWIN) || defined (TARGET_FREEBSD)
  strncpy(areq.ifra_name, args->intf, IFNAMSIZ);
  areq.ifra_name[IFNAMSIZ-1] = 0; /* Make sure to terminate */
  ((struct sockaddr_in*) &areq.ifra_addr)->sin_family = AF_INET;
  ((struct sockaddr_in*) &areq.ifra_addr)->sin_len = sizeof(areq.ifra_addr);
  ((struct sockaddr_in*) &areq.ifra_addr)->sin_addr.s_addr = sa_addr.s_addr;

  ((struct sockaddr_in*) &areq.ifra_mask)->sin_family = AF_INET;
  ((struct sockaddr_in*) &areq.ifra_mask)->sin_len  = sizeof(areq.ifra_mask);
  ((struct sockaddr_in*) &areq.ifra_mask)->sin_addr.s_addr = sa_netmask.s_addr;

  /* For some reason FreeBSD uses ifra_broadcast for specifying dstaddr */
  ((struct sockaddr_in*) &areq.ifra_broadaddr)->sin_family = AF_INET;
  ((struct sockaddr_in*) &areq.ifra_broadaddr)->sin_len =
    sizeof(areq.ifra_broadaddr);
  ((struct sockaddr_in*) &areq.ifra_broadaddr)->sin_addr.s_addr =
    sa_dstaddr.s_addr;
  if (ioctl(fd, SIOCAIFADDR, (void *) &areq) < 0) {
    err("ioctl[SIOCSIFADDR]");
    errf("can not setup tun device: %s", args->intf);
    close(fd);
    return -1;
  }
#endif

#if defined(TARGET_LINUX)
  ifr.ifr_addr.sa_family = AF_INET;
  ifr.ifr_dstaddr.sa_family = AF_INET;

  ifr.ifr_netmask.sa_family = AF_INET;


  strncpy(ifr.ifr_name, args->intf, IFNAMSIZ);
  ifr.ifr_name[IFNAMSIZ-1] = 0; /* Make sure to terminate */

  if (&sa_addr) { /* Set the interface address */
    memcpy(&((struct sockaddr_in *) &ifr.ifr_addr)->sin_addr, &sa_addr,
      sizeof(*addr));
    if (ioctl(fd, SIOCSIFADDR, (void *) &ifr) < 0) {
      if (errno != EEXIST) {
        err("ioctl(SIOCSIFADDR)");
        errf("can not setup tun device: %s", args->intf);
        close(fd);
        return -1;
      }
      else {
        err("ioctl(SIOCSIFADDR): Address already exists");
        close(fd);
        return -1;
      }
    }
    if (&dstaddr) { /* Set the destination address */
      memcpy(&((struct sockaddr_in *) &ifr.ifr_dstaddr)->sin_addr,
        &sa_dstaddr, sizeof(*dstaddr));
      if (ioctl(fd, SIOCSIFDSTADDR, (caddr_t) &ifr) < 0) {
        err("ioctl(SIOCSIFADDR)");
        errf("can not setup tun device: %s", args->intf);
        close(fd);
        return -1;
      }
    }

    if (&netmask) { /* Set the netmask */
      memcpy(&((struct sockaddr_in *) &ifr.ifr_netmask)->sin_addr, 
        &sa_netmask, sizeof(*netmask));


      if (ioctl(fd, SIOCSIFNETMASK, (void *) &ifr) < 0) {
          err("ioctl(SIOCSIFNETMASK)");
          errf("can not setup tun device: %s", args->intf);
        close(fd);
        return -1;
      }
    }
  }
#endif
  close(fd);
  logf("Device %s IP(%s) up", args->intf, args->tun_local_ip);
  return 0;
}


int vpn_run(vpn_ctx_t *ctx) {
  fd_set readset;
  int max_fd;
  ssize_t r;
  if (ctx->running) {
    errf("can not start, already running");
    return -1;
  }

  ctx->running = 1;
  ifconfig_up(ctx->args);
  shell_up(ctx->args);

  ctx->tun_buf = malloc(ctx->args->mtu + SHADOWVPN_ZERO_BYTES);
  ctx->udp_buf = malloc(ctx->args->mtu + SHADOWVPN_ZERO_BYTES);
  bzero(ctx->tun_buf, SHADOWVPN_ZERO_BYTES);
  bzero(ctx->udp_buf, SHADOWVPN_ZERO_BYTES);

  logf("VPN started");

  while (ctx->running) {
    FD_ZERO(&readset);
#ifndef TARGET_WIN32
    FD_SET(ctx->control_pipe[0], &readset);
#endif
    FD_SET(ctx->tun, &readset);
    FD_SET(ctx->sock, &readset);

    // we assume that pipe fd is always less than tun and sock fd which are
    // created later
    max_fd = max(ctx->tun, ctx->sock) + 1;

    if (-1 == select(max_fd, &readset, NULL, NULL, NULL)) {
      if (errno == EINTR)
        continue;
      err("select");
      break;
    }
#ifndef TARGET_WIN32
    if (FD_ISSET(ctx->control_pipe[0], &readset)) {
      char pipe_buf;
      (void)read(ctx->control_pipe[0], &pipe_buf, 1);
      break;
    }
#endif
    if (FD_ISSET(ctx->tun, &readset)) {
      r = tun_read(ctx->tun, ctx->tun_buf + SHADOWVPN_ZERO_BYTES, ctx->args->mtu);
      if (r == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // do nothing
        } else if (errno == EPERM || errno == EINTR) {
          // just log, do nothing
          err("read from tun");
        } else {
          err("read from tun");
          break;
        }
      }
      if (ctx->remote_addrlen) {
        crypto_encrypt(ctx->udp_buf, ctx->tun_buf, r);
        r = sendto(ctx->sock, ctx->udp_buf + SHADOWVPN_PACKET_OFFSET,
                   SHADOWVPN_OVERHEAD_LEN + r, 0,
                   ctx->remote_addrp, ctx->remote_addrlen);
        if (r == -1) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // do nothing
          } else if (errno == ENETUNREACH || errno == ENETDOWN ||
                     errno == EPERM || errno == EINTR || errno == EMSGSIZE) {
            // just log, do nothing
            err("sendto");
          } else {
            err("sendto");
            // TODO rebuild socket
            break;
          }
        }
      }
    }
    if (FD_ISSET(ctx->sock, &readset)) {
      // only change remote addr if decryption succeeds
      struct sockaddr_storage temp_remote_addr;
      socklen_t temp_remote_addrlen = sizeof(temp_remote_addr);
      r = recvfrom(ctx->sock, ctx->udp_buf + SHADOWVPN_PACKET_OFFSET,
                   SHADOWVPN_OVERHEAD_LEN + ctx->args->mtu, 0,
                   (struct sockaddr *)&temp_remote_addr,
                   &temp_remote_addrlen);
      if (r == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // do nothing
        } else if (errno == ENETUNREACH || errno == ENETDOWN ||
                   errno == EPERM || errno == EINTR) {
          // just log, do nothing
          err("recvfrom");
        } else {
          err("recvfrom");
          // TODO rebuild socket
          break;
        }
      }
      if (r == 0)
        continue;

      if (-1 == crypto_decrypt(ctx->tun_buf, ctx->udp_buf,
                               r - SHADOWVPN_OVERHEAD_LEN)) {
        errf("dropping invalid packet, maybe wrong password");
      } else {
        if (ctx->args->mode == SHADOWVPN_MODE_SERVER) {
          // if we are running a server, update server address from recv_from
          memcpy(ctx->remote_addrp, &temp_remote_addr, temp_remote_addrlen);
          ctx->remote_addrlen = temp_remote_addrlen;
        }

        if (-1 == tun_write(ctx->tun, ctx->tun_buf + SHADOWVPN_ZERO_BYTES,
              r - SHADOWVPN_OVERHEAD_LEN)) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // do nothing
          } else if (errno == EPERM || errno == EINTR || errno == EINVAL) {
            // just log, do nothing
            err("write to tun");
          } else {
            err("write to tun");
            break;
          }
        }
      }
    }
  }
  free(ctx->tun_buf);
  free(ctx->udp_buf);

  shell_down(ctx->args);

  close(ctx->tun);
  close(ctx->sock);

  ctx->running = 0;

#ifdef TARGET_WIN32
  WSACleanup();
#endif

  return -1;
}

int vpn_stop(vpn_ctx_t *ctx) {
  logf("shutting down by user");
  if (!ctx->running) {
    errf("can not stop, not running");
    return -1;
  }
#ifdef TARGET_WIN32
  shell_down(ctx->args);
  exit(0);
#else
  ctx->running = 0;
  char buf = 0;
  if (-1 == write(ctx->control_pipe[1], &buf, 1)) {
    err("write");
    return -1;
  }
#endif
  return 0;
}
