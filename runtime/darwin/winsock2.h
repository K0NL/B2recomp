/* [Recomps#1] Darwin only: Winsock names on POSIX sockets (nnnet.c). */
#ifndef RECOMPS_DARWIN_WINSOCK2_H
#define RECOMPS_DARWIN_WINSOCK2_H
#include <windows.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#define ioctlsocket ioctl
#define WSAEWOULDBLOCK EWOULDBLOCK
/* Winsock error names = the Linux errno values (WSAGetLastError returns errno); name lookups: getaddrinfo's EAI_ codes */
#define WSAEINTR EINTR
#define WSAEBADF EBADF
#define WSAEACCES EACCES
#define WSAEFAULT EFAULT
#define WSAEINVAL EINVAL
#define WSAEMFILE EMFILE
#define WSAEALREADY EALREADY
#define WSAENOTSOCK ENOTSOCK
#define WSAEDESTADDRREQ EDESTADDRREQ
#define WSAEMSGSIZE EMSGSIZE
#define WSAEPROTOTYPE EPROTOTYPE
#define WSAENOPROTOOPT ENOPROTOOPT
#define WSAEPROTONOSUPPORT EPROTONOSUPPORT
#define WSAESOCKTNOSUPPORT ESOCKTNOSUPPORT
#define WSAEOPNOTSUPP EOPNOTSUPP
#define WSAEPFNOSUPPORT EPFNOSUPPORT
#define WSAEAFNOSUPPORT EAFNOSUPPORT
#define WSAEADDRINUSE EADDRINUSE
#define WSAEADDRNOTAVAIL EADDRNOTAVAIL
#define WSAENETDOWN ENETDOWN
#define WSAENETUNREACH ENETUNREACH
#define WSAENETRESET ENETRESET
#define WSAECONNABORTED ECONNABORTED
#define WSAENOBUFS ENOBUFS
#define WSAEISCONN EISCONN
#define WSAENOTCONN ENOTCONN
#define WSAESHUTDOWN ESHUTDOWN
#define WSAETIMEDOUT ETIMEDOUT
#define WSAECONNREFUSED ECONNREFUSED
#define WSAEHOSTDOWN EHOSTDOWN
#define WSAEHOSTUNREACH EHOSTUNREACH
#define WSAHOST_NOT_FOUND EAI_NONAME
#define WSATRY_AGAIN EAI_AGAIN
#define WSANO_RECOVERY EAI_FAIL
#define WSATYPE_NOT_FOUND EAI_SERVICE
#define WSA_NOT_ENOUGH_MEMORY EAI_MEMORY
typedef struct pollfd WSAPOLLFD;
typedef struct linger LINGER;
#define InetPtonA inet_pton
#define InetNtopA inet_ntop
#define WSAEINPROGRESS EINPROGRESS
#define WSAECONNRESET ECONNRESET
#define WSAPoll poll
#define SD_RECEIVE SHUT_RD
#define SD_SEND SHUT_WR
#define SD_BOTH SHUT_RDWR
typedef struct { WORD wVersion, wHighVersion; char szDescription[257], szSystemStatus[129]; unsigned short iMaxSockets, iMaxUdpDg; char *lpVendorInfo; } WSADATA;
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
static inline int WSAStartup(WORD v, WSADATA *d) { if (d) { memset(d, 0, sizeof *d); d->wVersion = d->wHighVersion = v; } return 0; }
static inline int WSACleanup(void) { return 0; }
static inline int WSAGetLastError(void) { return errno; }
#endif
