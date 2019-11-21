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

static void _deliveryManagerCreateRequestMessageHeader(DeliveryMessageHeader *hdr, DeliveryMessageId id, s64 size1);
static void _deliveryManagerCreateReplyMessageHeader(DeliveryMessageHeader *hdr, DeliveryMessageId id, s64 size1);

static Result _deliveryManagerMessageSend(DeliveryManager *d, const DeliveryMessageHeader *hdr, const void* buf, size_t bufsize);
static Result _deliveryManagerMessageSendHeader(DeliveryManager *d, const DeliveryMessageHeader *hdr);
static Result _deliveryManagerMessageSendData(DeliveryManager *d, const void* buf, size_t bufsize, s64 total_transfer_size);

static Result _deliveryManagerMessageReceiveHeader(DeliveryManager *d, DeliveryMessageHeader *hdr);
static Result _deliveryManagerMessageReceiveData(DeliveryManager *d, void* buf, size_t bufsize, s64 total_transfer_size);

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

static bool _deliveryManagerGetCancelled(DeliveryManager *d) {
    bool flag=0;
    pthread_mutex_lock(&d->mutex);
    flag = d->cancel_flag;
    pthread_mutex_unlock(&d->mutex);
    return flag;
}

static Result _deliveryManagerGetSocketError(DeliveryManager *d) {
    Result rc=0;

    if (_deliveryManagerGetCancelled(d)) rc = MAKERESULT(Module_Nim, NimError_DeliveryOperationCancelled);
    else if (errno==ENETDOWN || errno==ECONNRESET || errno==EHOSTDOWN || errno==EHOSTUNREACH || errno==EPIPE) rc = MAKERESULT(Module_Nim, NimError_DeliverySocketError); // TODO: windows
    else rc = MAKERESULT(Module_Nim, NimError_UnknownError);

    return rc;
}

// This implements the socket setup done by nim cmd76 (SendSystemUpdate task-creation).
static Result _deliveryManagerCreateServerSocket(DeliveryManager *d) {
    int sockfd=-1;
    int ret=0;
    Result rc=0;

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

    if (ret != 0 && R_SUCCEEDED(rc)) {
        rc = _deliveryManagerGetSocketError(d);
    }

    if (ret == 0) d->listen_sockfd = sockfd;

    if (ret != 0) shutdownSocket(&sockfd);

    return rc;
}

// This implements the socket setup done by nim cmd69 (ReceiveSystemUpdate task-creation).
static Result _deliveryManagerCreateClientSocket(DeliveryManager *d) {
    int sockfd=-1;
    int ret=0;
    Result rc=0;

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
                    rc = MAKERESULT(Module_Nim, NimError_DeliveryConnectionTimeout);
                    fprintf(stderr, "connection timeout/reset by peer.\n");
                }
                else ret=0;
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

    if (ret != 0 && R_SUCCEEDED(rc)) {
        rc = _deliveryManagerGetSocketError(d);
    }

    if (ret == 0) d->conn_sockfd = sockfd;

    if (ret != 0) shutdownSocket(&sockfd);

    return rc;
}

// This implements the func called from the socket-setup loop from the nim Send async thread.
static Result _deliveryManagerServerTaskWaitConnection(DeliveryManager *d) {
    int ret=0;
    Result rc=0;

    while (ret==0) {
        if (_deliveryManagerGetCancelled(d)) return MAKERESULT(Module_Nim, NimError_DeliveryOperationCancelled);

        struct pollfd fds = {.fd = d->listen_sockfd, .events = POLLIN, .revents = 0};
        ret = poll(&fds, 1, 1000);
        if (ret < 0) return _deliveryManagerGetSocketError(d);
        if (ret>0 && !(fds.revents & POLLIN)) ret = 0;
    }

    return rc;
}

// This implements the socket setup done by the nim Send async thread.
static Result _deliveryManagerServerTaskSocketSetup(DeliveryManager *d) {
    int sockfd=-1;
    int ret=0;
    Result rc=0;

    while (R_SUCCEEDED(rc = _deliveryManagerServerTaskWaitConnection(d))) {
        struct sockaddr_in sa_remote={0};
        socklen_t addrlen = sizeof(sa_remote);
        ret = accept(d->listen_sockfd, (struct sockaddr*)&sa_remote, &addrlen);
        if (ret < 0) break;
        sockfd = ret;
        ret = 0;
        // nim would validate that the address from sa_remote matches the address previously used with bind() here, however we won't do that.
        break;
    }
    if (R_FAILED(rc)) return rc;

    #ifdef __SWITCH__
    if (ret == 0) {
        u64 tmpval=1;
        ret = setsockopt(sockfd, SOL_SOCKET, SO_VENDOR + 0x1, &tmpval, sizeof(tmpval));
        if (ret != 0) socket_error("setsockopt");
    }
    #endif

    // nim would use nifm cmd GetInternetConnectionStatus here, returning an error when successful. We won't do that.

    if (ret == 0) {
        ret = set_socket_nonblocking(sockfd, false);
        if (ret != 0) socket_error("set_socket_nonblocking(false)");
    }

    // nim ignores errors from this.
    if (ret == 0) {
        u32 tmpval=0x4000;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &tmpval, sizeof(tmpval));
    }

    // nim ignores errors from this.
    if (ret == 0) {
        u32 tmpval=0x20000;
        setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &tmpval, sizeof(tmpval));
    }

    if (ret != 0 && R_SUCCEEDED(rc)) {
        rc = _deliveryManagerGetSocketError(d);
    }

    if (ret == 0) d->conn_sockfd = sockfd;

    if (ret != 0) shutdownSocket(&sockfd);

    return rc;
}

static Result _deliveryManagerServerTaskMessageHandler(DeliveryManager *d) {
    Result rc=0;
    DeliveryMessageHeader recvhdr={0}, sendhdr={0};

    while (R_SUCCEEDED(rc)) {
        rc = _deliveryManagerMessageReceiveHeader(d, &recvhdr);
        if (R_FAILED(rc)) break;

        switch (recvhdr.id) {
            case DeliveryMessageId_Exit:
                return 0;
            break;

            // TODO: DeliveryMessageId_GetMetaContentRecord

            // TODO: DeliveryMessageId_GetContent

            // We don't support DeliveryMessageId_GetCommonTicket.

            // TODO: DeliveryMessageId_UpdateProgress

            default:
                rc = MAKERESULT(Module_Nim, NimError_DeliveryBadMessageId);
            break;
        }
    }

    return rc;
}

static void* _deliveryManagerServerTask(void* arg) {
    DeliveryManager *d = arg;
    Result rc=0;

    // nim would load "nim.errorsimulate!error_localcommunication_result" into rc here, we won't do an equivalent.

    rc = _deliveryManagerServerTaskSocketSetup(d);
    if (R_SUCCEEDED(rc)) rc = _deliveryManagerServerTaskMessageHandler(d);

    pthread_mutex_lock(&d->mutex);
    d->rc = rc;
    pthread_mutex_unlock(&d->mutex);
    return NULL;
}

static void _deliveryManagerCreateRequestMessageHeader(DeliveryMessageHeader *hdr, DeliveryMessageId id, s64 data_size) {
    *hdr = (DeliveryMessageHeader){.magicnum = DELIVERY_MESSAGE_MAGICNUM_REQUEST, .id = id, .data_size = data_size};
    return;
}

static void _deliveryManagerCreateReplyMessageHeader(DeliveryMessageHeader *hdr, DeliveryMessageId id, s64 data_size) {
    *hdr = (DeliveryMessageHeader){.magicnum = DELIVERY_MESSAGE_MAGICNUM_REPLY, .id = id, .data_size = data_size};
    return;
}

static void _deliveryManagerMessageHeaderConvertEndian(DeliveryMessageHeader *hdr) {
    hdr->magicnum = le_word(hdr->magicnum);
    hdr->meta_size = le_hword(hdr->meta_size);
    hdr->data_size = le_dword(hdr->data_size);
}

// This implements the equivalent func in nim.
static Result _deliveryManagerMessageSend(DeliveryManager *d, const DeliveryMessageHeader *hdr, const void* buf, size_t bufsize) {
    Result rc=0;

    rc = _deliveryManagerMessageSendHeader(d, hdr);
    if (R_SUCCEEDED(rc)) rc = _deliveryManagerMessageSendData(d, buf, bufsize, hdr->data_size); // TODO: Pass params for the funcptr once impl'd.

    return rc;
}

// This implements the equivalent func in nim.
static Result _deliveryManagerMessageSendHeader(DeliveryManager *d, const DeliveryMessageHeader *hdr) {
    ssize_t ret=0;
    Result rc=0;
    DeliveryMessageHeader tmphdr={0};

    if (_deliveryManagerGetCancelled(d))
        return MAKERESULT(Module_Nim, NimError_DeliveryOperationCancelled);

    tmphdr = *hdr;

    if (tmphdr.magicnum != DELIVERY_MESSAGE_MAGICNUM_REQUEST && tmphdr.magicnum != DELIVERY_MESSAGE_MAGICNUM_REPLY)
        return MAKERESULT(Module_Nim, NimError_DeliveryBadMessageMagicnum);

    _deliveryManagerMessageHeaderConvertEndian(&tmphdr);

    ret = send(d->conn_sockfd, &tmphdr, sizeof(tmphdr), 0); // nim doesn't verify returned size for this.

    if (ret < 0 && R_SUCCEEDED(rc)) {
        rc = _deliveryManagerGetSocketError(d);
    }

    return rc;
}

// This implements the equivalent func in nim.
static Result _deliveryManagerMessageSendData(DeliveryManager *d, const void* buf, size_t bufsize, s64 total_transfer_size) {
    ssize_t ret=0;
    Result rc=0;
    size_t cur_size=0;

    if (_deliveryManagerGetCancelled(d))
        return MAKERESULT(Module_Nim, NimError_DeliveryOperationCancelled);

    if (total_transfer_size < 1) return 0;

    for (s64 offset=0; offset<total_transfer_size; offset+=cur_size) {
        if (_deliveryManagerGetCancelled(d))
            return MAKERESULT(Module_Nim, NimError_DeliveryOperationCancelled);

        cur_size = total_transfer_size-offset;
        cur_size = cur_size > bufsize ? bufsize : cur_size;

        // TODO: call funcptr with cur_size and offset, returning Result from there on failure.

        ret = send(d->conn_sockfd, buf, cur_size, 0);
        if (ret < 0) break; // nim ignores ret besides this.
    }

    if (ret < 0 && R_SUCCEEDED(rc)) {
        rc = _deliveryManagerGetSocketError(d);
    }

    return rc;
}

// This implements the equivalent func in nim.
static Result _deliveryManagerMessageReceiveHeader(DeliveryManager *d, DeliveryMessageHeader *hdr) {
    ssize_t ret=0;
    Result rc=0;
    size_t cur_size, total_size;
    DeliveryMessageHeader tmphdr={0};
    u8 tmpdata[0x80]={0};

    if (_deliveryManagerGetCancelled(d))
        return MAKERESULT(Module_Nim, NimError_DeliveryOperationCancelled);

    ret = recv(d->conn_sockfd, &tmphdr, sizeof(tmphdr), MSG_WAITALL); // nim doesn't verify returned size for this.
    if (ret >= 0) _deliveryManagerMessageHeaderConvertEndian(&tmphdr);

    if (ret >= 0) {
        if (tmphdr.magicnum != DELIVERY_MESSAGE_MAGICNUM_REQUEST && tmphdr.magicnum != DELIVERY_MESSAGE_MAGICNUM_REPLY)
            return MAKERESULT(Module_Nim, NimError_DeliveryBadMessageMagicnum);

        if (tmphdr.data_size < 0)
            return MAKERESULT(Module_Nim, NimError_DeliveryBadMessageDataSize);
    }

    if (ret >= 0) {
        total_size = tmphdr.meta_size;
        if (total_size > 0x1000)
            return MAKERESULT(Module_Nim, NimError_DeliveryBadMessageMetaSize);

        while (total_size) { // Received data from this is ignored by nim.
            cur_size = total_size < sizeof(tmpdata) ? total_size : sizeof(tmpdata);
            ret = recv(d->conn_sockfd, tmpdata, cur_size, MSG_WAITALL);
            if (ret < 0) break;
            if (ret!=cur_size)
                return MAKERESULT(Module_Nim, NimError_DeliverySocketError);
            total_size-=cur_size;
        }
    }

    if (ret >= 0) *hdr = tmphdr;

    if (ret < 0 && R_SUCCEEDED(rc)) {
        rc = _deliveryManagerGetSocketError(d);
    }

    return rc;
}

// This implements the equivalent func in nim.
static Result _deliveryManagerMessageReceiveData(DeliveryManager *d, void* buf, size_t bufsize, s64 total_transfer_size) {
    ssize_t ret=0;
    Result rc=0;
    size_t cur_size=0;

    if (total_transfer_size < 1) return 0;

    for (s64 offset=0; offset<total_transfer_size; offset+=ret) {
        if (_deliveryManagerGetCancelled(d))
            return MAKERESULT(Module_Nim, NimError_DeliveryOperationCancelled);

        cur_size = total_transfer_size-offset;
        cur_size = cur_size > bufsize ? bufsize : cur_size;

        ret = recv(d->conn_sockfd, buf, cur_size, MSG_WAITALL);
        if (ret < 0) break; // nim ignores ret value 0.

        // TODO: call funcptr with ret and offset, returning Result from there on failure.
    }

    if (ret < 0 && R_SUCCEEDED(rc)) {
        rc = _deliveryManagerGetSocketError(d);
    }

    return rc;
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
    deliveryManagerCancel(d);
    deliveryManagerGetResult(d);
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

void deliveryManagerCancel(DeliveryManager *d) {
    pthread_mutex_lock(&d->mutex);
    shutdownSocket(&d->listen_sockfd);
    shutdownSocket(&d->conn_sockfd);
    d->cancel_flag = true;
    pthread_mutex_unlock(&d->mutex);
}

Result deliveryManagerGetResult(DeliveryManager *d) {
    Result rc=0;
    pthread_join(d->thread, NULL);
    pthread_mutex_lock(&d->mutex);
    rc = d->rc;
    pthread_mutex_unlock(&d->mutex);
    return rc;
}

