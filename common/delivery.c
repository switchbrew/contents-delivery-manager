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
static void shutdownSocket(int *socket) {
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

static void socket_error(const char *msg) {
#ifndef _WIN32
    perror(msg);
#else
    wchar_t *s = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
               NULL, WSAGetLastError(),
               MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
               (LPWSTR)&s, 0, NULL);
    fprintf(stderr, "%S\n", s);
    LocalFree(s);
#endif
}

// This implements the socket setup done by nim cmd76 (SendSystemUpdate task-creation).
static Result _deliveryManagerCreateServerSocket(DeliveryManager *d) {
    int sockfd=-1;
    int ret=0;
    Result res=0;

    ret = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ret >= 0) {
        sockfd = ret;
        ret = 0;
    }
    else socket_error("socket");

    if (ret == 0) {
        ret = set_socket_nonblocking(sockfd, true);
        if (ret != 0) socket_error("set_socket_nonblocking");
    }

    #ifdef __SWITCH__
    if (ret == 0) {
        u64 tmpval=1;
        ret = setsockopt(sockfd, SOL_SOCKET, SO_VENDOR + 0x1, &tmpval, sizeof(tmpval));
        if (ret != 0) socket_error("setsockopt");
    }
    #endif

    if (ret == 0) {
        u32 tmpval2=1;
        ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &tmpval2, sizeof(tmpval2));
        if (ret != 0) socket_error("setsockopt");
    }

    if (ret == 0) {
        struct sockaddr_in serv_addr;

        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = d->addr.s_addr;
        serv_addr.sin_port = htons(d->port);

        ret = bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        if (ret != 0) socket_error("bind");
    }

    if (ret == 0) {
        ret = listen(sockfd, 1);
        if (ret != 0) socket_error("listen");
    }

    if (ret == 0) d->listen_sockfd = sockfd;

    if (ret != 0) shutdownSocket(&sockfd);

    if (ret != 0 && res==0) {
        if (d->cancel_flag) res = MAKERESULT(Module_Nim, NimError_DeliveryOperationCancelled); // TODO: mutex
        else if (errno==ENETDOWN || errno==ECONNRESET || errno==EHOSTDOWN || errno==EHOSTUNREACH || errno==EPIPE) res = MAKERESULT(Module_Nim, NimError_DeliverySocketError); // TODO: windows
        else res = MAKERESULT(Module_Nim, NimError_UnknownError);
    }

    return res;
}

// This implements the socket setup done by nim cmd69 (ReceiveSystemUpdate task-creation).
static Result _deliveryManagerCreateClientSocket(DeliveryManager *d) {
    int sockfd=-1;
    int ret=0;
    Result res=0;

    ret = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ret >= 0) {
        sockfd = ret;
        ret = 0;
    }
    else socket_error("socket");

    if (ret == 0) {
        ret = set_socket_nonblocking(sockfd, true);
        if (ret != 0) socket_error("set_socket_nonblocking(true)");
    }

    if (ret == 0) {
        struct sockaddr_in serv_addr;

        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = d->addr.s_addr;
        serv_addr.sin_port = htons(d->port);

        ret = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

        // nim uses select(), we use poll() instead. TODO: windows
        if (ret != 0) {
            if (errno != EINPROGRESS) socket_error("connect");
            else {
                struct pollfd fds = {.fd = sockfd, .events = POLLOUT, .revents = 0};
                ret = poll(&fds, 1, 5000);
                if (ret < 0) socket_error("poll");
                else if (ret == 0 || (fds.revents & (POLLERR|POLLHUP))) {
                    ret = -1;
                    res = MAKERESULT(Module_Nim, NimError_DeliveryConnectionTimeout);
                    fprintf(stderr, "connection timeout/reset by peer.\n");
                }
            }
        }
    }

    #ifdef __SWITCH__
    if (ret == 0) {
        u64 tmpval=1;
        ret = setsockopt(sockfd, SOL_SOCKET, SO_VENDOR + 0x1, &tmpval, sizeof(tmpval));
        if (ret != 0) socket_error("setsockopt");
    }
    #endif

    if (ret == 0) {
        ret = set_socket_nonblocking(sockfd, false);
        if (ret != 0) socket_error("set_socket_nonblocking(false)");
    }

    if (ret == 0) d->conn_sockfd = sockfd;

    if (ret != 0) shutdownSocket(&sockfd);

    if (ret != 0 && res==0) {
        if (d->cancel_flag) res = MAKERESULT(Module_Nim, NimError_DeliveryOperationCancelled); // TODO: mutex
        else if (errno==ENETDOWN || errno==ECONNRESET || errno==EHOSTDOWN || errno==EHOSTUNREACH || errno==EPIPE) res = MAKERESULT(Module_Nim, NimError_DeliverySocketError); // TODO: windows
        else res = MAKERESULT(Module_Nim, NimError_UnknownError);
    }

    return res;
}

static void* _deliveryManagerServerTask(void* arg) {
    DeliveryManager *d = arg;

    return NULL;
}

Result deliveryManagerCreate(DeliveryManager *d, bool server, const struct in_addr *addr, u16 port) {
    memset(d, 0, sizeof(*d));

    d->server = server;
    d->addr = *addr;
    d->port = port;
    d->listen_sockfd = -1;
    d->conn_sockfd = -1;

    int ret = pthread_mutex_init(&d->mutex, NULL);
    if (ret != 0) {
        printf("pthread_mutex_init() failed: %d\n", ret);
        return MAKERESULT(Module_Nim, NimError_UnknownError);
    }

    if (server) return _deliveryManagerCreateServerSocket(d);
    else return _deliveryManagerCreateClientSocket(d);
}

void deliveryManagerClose(DeliveryManager *d) {
    shutdownSocket(&d->listen_sockfd);
    shutdownSocket(&d->conn_sockfd);
    pthread_join(d->thread, NULL);
    pthread_mutex_destroy(&d->mutex);
    memset(d, 0, sizeof(*d));
}

// We only support this with server=true.
Result deliveryManagerRequestRun(DeliveryManager *d) {
    int ret=0;

    if (!d->server) return MAKERESULT(Module_Nim, NimError_BadInput);

    ret = pthread_create(&d->thread, NULL, _deliveryManagerServerTask, d);

    if (ret != 0) {
        printf("pthread_create() failed: %d\n", ret);
        return MAKERESULT(Module_Nim, NimError_UnknownError);
    }

    return 0;
}

