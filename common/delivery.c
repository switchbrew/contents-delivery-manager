#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/time.h>
#include <stdint.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>

#ifndef __WIN32__
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
typedef uint32_t in_addr_t;
#endif

#include "delivery.h"

//---------------------------------------------------------------------------------
void shutdownSocket(int *socket) {
//---------------------------------------------------------------------------------
    if (*socket == -1) return;
#ifdef __WIN32__
    shutdown (*socket, SD_BOTH);
    closesocket (*socket);
#else
    shutdown (*socket, SHUT_RDWR);
    close(*socket);
#endif
    *socket = -1;
}

//---------------------------------------------------------------------------------
static int set_socket_nonblocking(int sock, bool nonblock) {
//---------------------------------------------------------------------------------

#ifndef __WIN32__
    int flags = fcntl(sock, F_GETFL);

    if(flags == -1) return -1;

    int rc = fcntl(sock, F_SETFL, nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));

    if(rc != 0) return -1;

#else
    u_long opt = nonblock;
    ioctlsocket(sock, FIONBIO, &opt);
#endif

    return 0;
}

// This implements the socket setup done by nim cmd76 (SendSystemUpdate task-creation).
int deliveryManagerCreateServerSocket(DeliveryManager *d) {
    int sockfd=-1;
    int ret=0;

    ret = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ret >= 0) sockfd = ret;

    if (ret >= 0) ret = set_socket_nonblocking(sockfd, true);

    #ifdef __SWITCH__
    if (ret >= 0) {
        u64 tmpval=1;
        ret = setsockopt(sockfd, SOL_SOCKET, SO_VENDOR + 0x1, &tmpval, sizeof(tmpval));
    }
    #endif

    if (ret >= 0) {
        u32 tmpval2=1;
        ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &tmpval2, sizeof(tmpval2));
    }

    if (ret >= 0) {
        struct sockaddr_in serv_addr;

        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = d->addr.s_addr;
        serv_addr.sin_port = htons(d->port);

        ret = bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    }

    if (ret >= 0) ret = listen(sockfd, 1);

    if (ret >= 0) d->listen_sockfd = sockfd;

    // TODO: error handling
    if (ret < 0) shutdownSocket(&sockfd);
    return ret;
}

// This implements the socket setup done by nim cmd69 (ReceiveSystemUpdate task-creation).
int deliveryManagerCreateClientSocket(DeliveryManager *d) {
    int sockfd=-1;
    int ret=0;

    ret = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ret >= 0) sockfd = ret;

    if (ret >= 0) ret = set_socket_nonblocking(sockfd, true);

    if (ret >= 0) {
        struct sockaddr_in serv_addr;

        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = d->addr.s_addr;
        serv_addr.sin_port = htons(d->port);

        ret = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

        // nim uses select(), we use poll() instead. TODO: windows
        if (ret < 0 && errno == EINPROGRESS) {
            struct pollfd fds = {.fd = sockfd, .events = POLLOUT, .revents = 0};
            ret = poll(&fds, 1, 5000);
            if (ret == 0 || (fds.revents & (POLLERR|POLLHUP))) ret = -1; // TODO: handle timeout error
        }
    }

    #ifdef __SWITCH__
    if (ret >= 0) {
        u64 tmpval=1;
        ret = setsockopt(sockfd, SOL_SOCKET, SO_VENDOR + 0x1, &tmpval, sizeof(tmpval));
    }
    #endif

    if (ret >= 0) ret = set_socket_nonblocking(sockfd, false);

    if (ret >= 0) d->conn_sockfd = sockfd;

    // TODO: error handling
    if (ret < 0) shutdownSocket(&sockfd);
    return ret;
}

int deliveryManagerCreate(DeliveryManager *d, bool server, const struct in_addr *addr, u16 port) {
    memset(d, 0, sizeof(*d));

    d->server = server;
    d->addr = *addr;
    d->port = port;
    d->listen_sockfd = -1;
    d->conn_sockfd = -1;

    if (server) return deliveryManagerCreateServerSocket(d);
    else return deliveryManagerCreateClientSocket(d);
}

void deliveryManagerClose(DeliveryManager *d) {
    shutdownSocket(&d->listen_sockfd);
    shutdownSocket(&d->conn_sockfd);
    memset(d, 0, sizeof(*d));
}

