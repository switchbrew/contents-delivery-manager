#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/time.h>
#include <stdint.h>
#include <inttypes.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

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

#ifndef __SWITCH__
#include "sha2.h"
#else
#include "switch/crypto/sha256.h"
#endif

#include "delivery.h"
#include "utils.h"

#define TRACE(_d,fmt,...) if (_d->log_file) fprintf(_d->log_file, "%s: " fmt "\n", __PRETTY_FUNCTION__, ## __VA_ARGS__)

static void _deliveryManagerCreateRequestMessageHeader(DeliveryMessageHeader *hdr, DeliveryMessageId id, s64 size1);
static void _deliveryManagerCreateReplyMessageHeader(DeliveryMessageHeader *hdr, DeliveryMessageId id, s64 size1);

static Result _deliveryManagerMessageSend(DeliveryManager *d, const DeliveryMessageHeader *hdr, const void* buf, size_t bufsize, DeliveryDataTransfer *transfer);
static Result _deliveryManagerMessageSendHeader(DeliveryManager *d, const DeliveryMessageHeader *hdr);
static Result _deliveryManagerMessageSendData(DeliveryManager *d, const void* buf, size_t bufsize, s64 total_transfer_size, DeliveryDataTransfer *transfer);

static Result _deliveryManagerMessageReceiveHeader(DeliveryManager *d, DeliveryMessageHeader *hdr);
static Result _deliveryManagerMessageReceiveData(DeliveryManager *d, void* buf, size_t bufsize, s64 total_transfer_size, DeliveryDataTransfer *transfer);

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

static Result _deliveryManagerGetSocketError(DeliveryManager *d, const char *msg) {
    Result rc=0;

    if (_deliveryManagerGetCancelled(d)) rc = MAKERESULT(Module_Nim, NimError_DeliveryOperationCancelled);
    else {
        if (msg) socket_error(msg);

        #ifndef _WIN32
        if (errno==ENETDOWN || errno==ECONNRESET || errno==EHOSTDOWN || errno==EHOSTUNREACH || errno==EPIPE)
        #else
        if (WSAGetLastError()==WSAENETDOWN || WSAGetLastError()==WSAECONNRESET || WSAGetLastError()==WSAEHOSTDOWN || WSAGetLastError()==WSAEHOSTUNREACH || WSAGetLastError()==WSAECONNABORTED)
        #endif
            rc = MAKERESULT(Module_Nim, NimError_DeliverySocketError);
        else rc = MAKERESULT(Module_Nim, NimError_UnknownError);
    }

    return rc;
}

// This implements the socket setup done by nim cmd76 (SendSystemUpdate task-creation).
static Result _deliveryManagerCreateServerSocket(DeliveryManager *d) {
    int sockfd=-1;
    int ret=0;
    Result rc=0;
    const char *msg = NULL;

    ret = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ret >= 0) {
        sockfd = ret;
        ret = 0;
    }
    else msg = "socket";

    if (ret == 0) {
        ret = set_socket_nonblocking(sockfd, true);
        if (ret != 0) msg = "set_socket_nonblocking";
    }

    #ifdef __SWITCH__
    if (ret == 0) {
        u64 tmpval=1;
        ret = setsockopt(sockfd, SOL_SOCKET, SO_VENDOR + 0x1, &tmpval, sizeof(tmpval));
        if (ret != 0) msg = "setsockopt";
    }
    #endif

    if (ret == 0) {
        u32 tmpval2=1;
        ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&tmpval2, sizeof(tmpval2));
        if (ret != 0) msg = "setsockopt";
    }

    if (ret == 0) {
        struct sockaddr_in serv_addr;

        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = d->addr.s_addr;
        serv_addr.sin_port = htons(d->port);

        ret = bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        if (ret != 0) msg = "bind";
    }

    if (ret == 0) {
        ret = listen(sockfd, 1);
        if (ret != 0) msg = "listen";
    }

    if (ret != 0 && R_SUCCEEDED(rc)) {
        rc = _deliveryManagerGetSocketError(d, msg);
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
    const char *msg = NULL;

    ret = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ret >= 0) {
        sockfd = ret;
        ret = 0;
    }
    else msg = "socket";

    if (ret == 0) {
        ret = set_socket_nonblocking(sockfd, true);
        if (ret != 0) msg = "set_socket_nonblocking(true)";
    }

    if (ret == 0) {
        struct sockaddr_in serv_addr;

        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = d->addr.s_addr;
        serv_addr.sin_port = htons(d->port);

        ret = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

        // nim uses select(), we use poll() instead.
        if (ret != 0) {
            #ifndef _WIN32
            if (errno != EINPROGRESS)
            #else
            if (WSAGetLastError() != WSAEWOULDBLOCK)
            #endif
                msg = "connect";
            else {
                struct pollfd fds = {.fd = sockfd, .events = POLLOUT, .revents = 0};
                #ifndef _WIN32
                ret = poll(&fds, 1, 5000);
                #else
                ret = WSAPoll(&fds, 1, 5000);
                #endif
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
        ret = setsockopt(sockfd, SOL_SOCKET, SO_VENDOR + 0x1, (const char*)&tmpval, sizeof(tmpval));
        if (ret != 0) msg = "setsockopt";
    }
    #endif

    if (ret == 0) {
        ret = set_socket_nonblocking(sockfd, false);
        if (ret != 0) msg = "set_socket_nonblocking(false)";
    }

    if (ret != 0 && R_SUCCEEDED(rc)) {
        rc = _deliveryManagerGetSocketError(d, msg);
    }

    if (ret == 0) d->conn_sockfd = sockfd;

    if (ret != 0) shutdownSocket(&sockfd);

    return rc;
}

// This implements the func called from the socket-setup loop from the nim Send async thread.
// nim uses select(), we use poll() instead.
static Result _deliveryManagerServerTaskWaitConnection(DeliveryManager *d) {
    int ret=0;
    Result rc=0;

    while (ret==0) {
        if (_deliveryManagerGetCancelled(d)) return MAKERESULT(Module_Nim, NimError_DeliveryOperationCancelled);

        struct pollfd fds = {.fd = d->listen_sockfd, .events = POLLIN, .revents = 0};
        #ifndef _WIN32
        ret = poll(&fds, 1, 1000);
        #else
        ret = WSAPoll(&fds, 1, 1000);
        #endif
        if (ret < 0) return _deliveryManagerGetSocketError(d, NULL);
        if (ret>0 && !(fds.revents & POLLIN)) ret = 0;
    }

    return rc;
}

// This implements the socket setup done by the nim Send async thread.
static Result _deliveryManagerServerTaskSocketSetup(DeliveryManager *d) {
    int sockfd=-1;
    int ret=0;
    Result rc=0;
    const char *msg = NULL;

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
        if (ret != 0) msg = "setsockopt";
    }
    #endif

    // nim would use nifm cmd GetInternetConnectionStatus here, returning an error when successful. We won't do that.

    if (ret == 0) {
        ret = set_socket_nonblocking(sockfd, false);
        if (ret != 0) msg = "set_socket_nonblocking(false)";
    }

    // nim ignores errors from this.
    if (ret == 0) {
        u32 tmpval=0x4000;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (const char*)&tmpval, sizeof(tmpval));
    }

    // nim ignores errors from this.
    if (ret == 0) {
        u32 tmpval=0x20000;
        setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (const char*)&tmpval, sizeof(tmpval));
    }

    if (ret != 0 && R_SUCCEEDED(rc)) {
        rc = _deliveryManagerGetSocketError(d, msg);
    }

    if (ret == 0) d->conn_sockfd = sockfd;

    if (ret != 0) shutdownSocket(&sockfd);

    return rc;
}

static Result _deliveryManagerGetContentTransferHandler(void* userdata, void* buf, u64 size, s64 offset) {
    Result rc=0;
    struct DeliveryGetContentDataTransferState *transfer_state = (struct DeliveryGetContentDataTransferState*)userdata;
    DeliveryManager *d = transfer_state->manager;

    if (d->handler_get_content.transfer_handler) {
        rc = d->handler_get_content.transfer_handler(transfer_state, buf, size, offset);
        if (R_FAILED(rc)) TRACE(d, "handler_get_content.transfer_handler() failed: 0x%x.", rc);
    }
    else
        rc = MAKERESULT(Module_Nim, NimError_BadInput);

    if (R_SUCCEEDED(rc) && transfer_state->arg->flag == 0) {
        pthread_mutex_lock(&d->mutex);
        d->progress_total_size+=size;
        pthread_mutex_unlock(&d->mutex);
    }

    return rc;
}

static void _deliveryManagerPrintContentId(char *outstr, const NcmContentId *content_id) {
    for (u32 pos=0; pos<16; pos++) snprintf(&outstr[pos*2], 3, "%02x", content_id->c[pos]);
}

static Result _deliveryManagerServerTaskMessageHandler(DeliveryManager *d) {
    Result rc=0;
    DeliveryMessageHeader recvhdr={0}, sendhdr={0};

    NcmContentMetaKey content_meta_key;
    NcmPackagedContentInfo meta_content_record;
    DeliveryMessageGetContentArg arg;
    s64 content_size;
    s64 progress_value;
    struct DeliveryGetContentDataTransferState transfer_state = {.manager = d, .arg = &arg, .userdata = d->handler_get_content.userdata};
    DeliveryDataTransfer transfer = {.userdata = &transfer_state, .transfer_handler = _deliveryManagerGetContentTransferHandler};
    char contentid_str[33];

    TRACE(d, "Entering message handler loop.");

    while (R_SUCCEEDED(rc)) {
        rc = _deliveryManagerMessageReceiveHeader(d, &recvhdr);
        if (R_FAILED(rc)) break;

        switch (recvhdr.id) {
            case DeliveryMessageId_Exit:
                TRACE(d, "Exiting as requested by the client.");
                return 0;
            break;

            case DeliveryMessageId_GetMetaContentRecord:
                TRACE(d, "Processing DeliveryMessageId_GetMetaContentRecord...");

                memset(&content_meta_key, 0, sizeof(content_meta_key));
                memset(&meta_content_record, 0, sizeof(meta_content_record));

                if (recvhdr.data_size != sizeof(content_meta_key))
                    rc = MAKERESULT(Module_Nim, NimError_DeliveryBadMessageDataSize);

                if (R_SUCCEEDED(rc))
                    rc = _deliveryManagerMessageReceiveData(d, &content_meta_key, sizeof(content_meta_key), sizeof(content_meta_key), NULL);

                if (R_SUCCEEDED(rc)) {
                    TRACE(d, "content_meta_key: id = %016"PRIX64", version = v%u, type = 0x%x", content_meta_key.id, content_meta_key.version, content_meta_key.type);

                    if (d->handler_get_meta_content_record) {
                        rc = d->handler_get_meta_content_record(d->handler_get_meta_content_record_userdata, &meta_content_record, &content_meta_key);
                        if (R_FAILED(rc)) TRACE(d, "handler_get_meta_content_record() failed: 0x%x.", rc);
                    }
                }

                if (R_SUCCEEDED(rc)) {
                    _deliveryManagerCreateReplyMessageHeader(&sendhdr, recvhdr.id, sizeof(meta_content_record));
                    rc = _deliveryManagerMessageSend(d, &sendhdr, &meta_content_record, sizeof(meta_content_record), NULL);
                }
            break;

            case DeliveryMessageId_GetContent:
                TRACE(d, "Processing DeliveryMessageId_GetContent...");

                memset(&arg, 0, sizeof(arg));
                content_size=0;

                if (recvhdr.data_size != sizeof(arg))
                    rc = MAKERESULT(Module_Nim, NimError_DeliveryBadMessageDataSize);

                if (R_SUCCEEDED(rc))
                    rc = _deliveryManagerMessageReceiveData(d, &arg, sizeof(arg), sizeof(arg), NULL);

                if (R_SUCCEEDED(rc)) {
                    memset(contentid_str, 0, sizeof(contentid_str));
                    _deliveryManagerPrintContentId(contentid_str, &arg.content_id);
                    TRACE(d, "arg: content_id = %s, flag = %d", contentid_str, arg.flag);

                    if (d->handler_get_content.init_handler) {
                        rc = d->handler_get_content.init_handler(&transfer_state, &content_size);
                        if (R_FAILED(rc)) TRACE(d, "handler_get_content.init_handler() failed: 0x%x.", rc);
                    }
                }

                if (R_SUCCEEDED(rc)) {
                    _deliveryManagerCreateReplyMessageHeader(&sendhdr, recvhdr.id, content_size);
                    rc = _deliveryManagerMessageSend(d, &sendhdr, d->workbuf, d->workbuf_size, &transfer);
                    if (d->handler_get_content.exit_handler) d->handler_get_content.exit_handler(&transfer_state);
                }

                if (R_SUCCEEDED(rc) && arg.flag == 0) {
                    pthread_mutex_lock(&d->mutex);
                    TRACE(d, "progress_total_size = 0x%"PRIx64, d->progress_total_size);
                    pthread_mutex_unlock(&d->mutex);
                }
            break;

            // We don't support DeliveryMessageId_GetCommonTicket. nim server supports it for SystemUpdate/Application, but the nim client only uses it for Application (game-updates).

            case DeliveryMessageId_UpdateProgress:
                TRACE(d, "Processing DeliveryMessageId_UpdateProgress...");

                progress_value = 0;

                if (recvhdr.data_size != sizeof(progress_value))
                    rc = MAKERESULT(Module_Nim, NimError_DeliveryBadMessageId); // This error is used by nim.

                if (R_SUCCEEDED(rc))
                    rc = _deliveryManagerMessageReceiveData(d, &progress_value, sizeof(progress_value), sizeof(progress_value), NULL);

                if (R_SUCCEEDED(rc)) {
                    pthread_mutex_lock(&d->mutex);
                    d->progress_current_size = le_dword(progress_value);
                    TRACE(d, "progress_current_size = 0x%"PRIx64, d->progress_current_size);
                    pthread_mutex_unlock(&d->mutex);
                }

                if (R_SUCCEEDED(rc)) {
                    _deliveryManagerCreateReplyMessageHeader(&sendhdr, recvhdr.id, 0);
                    rc = _deliveryManagerMessageSendHeader(d, &sendhdr);
                }
            break;

            default:
                TRACE(d, "Bad MessageId: 0x%x.", recvhdr.id);
                rc = MAKERESULT(Module_Nim, NimError_DeliveryBadMessageId);
            break;
        }

        TRACE(d, "Finshed processing for this message.");
    }

    return rc;
}

static void* _deliveryManagerServerTask(void* arg) {
    DeliveryManager *d = arg;
    Result rc=0;

    // nim would load "nim.errorsimulate!error_localcommunication_result" into rc here, we won't do an equivalent.

    rc = _deliveryManagerServerTaskSocketSetup(d);
    if (R_SUCCEEDED(rc)) rc = _deliveryManagerServerTaskMessageHandler(d);

    TRACE(d, "Returning Result 0x%x.", rc);

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
static Result _deliveryManagerMessageSend(DeliveryManager *d, const DeliveryMessageHeader *hdr, const void* buf, size_t bufsize, DeliveryDataTransfer *transfer) {
    Result rc=0;

    rc = _deliveryManagerMessageSendHeader(d, hdr);
    if (R_SUCCEEDED(rc)) rc = _deliveryManagerMessageSendData(d, buf, bufsize, hdr->data_size, transfer);

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

    ret = send(d->conn_sockfd, (const char*)&tmphdr, sizeof(tmphdr), 0); // nim doesn't verify returned size for this.

    if (ret < 0 && R_SUCCEEDED(rc)) {
        rc = _deliveryManagerGetSocketError(d, NULL);
    }

    return rc;
}

// This implements the equivalent func in nim.
static Result _deliveryManagerMessageSendData(DeliveryManager *d, const void* buf, size_t bufsize, s64 total_transfer_size, DeliveryDataTransfer *transfer) {
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

        if (transfer) {
            if (transfer->transfer_handler) {
                rc = transfer->transfer_handler(transfer->userdata, (void*)buf, cur_size, offset); // nim doesn't pass the buf param, it's passed via the object instead. nim passes ptrs for the last 2 params.
                if (R_FAILED(rc)) break;
            }
        }

        ret = send(d->conn_sockfd, buf, cur_size, 0);
        if (ret < 0) break; // nim ignores ret besides this.
    }

    if (ret < 0 && R_SUCCEEDED(rc)) {
        rc = _deliveryManagerGetSocketError(d, NULL);
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

    ret = recv(d->conn_sockfd, (char*)&tmphdr, sizeof(tmphdr), MSG_WAITALL); // nim doesn't verify returned size for this.
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
        rc = _deliveryManagerGetSocketError(d, NULL);
    }

    return rc;
}

// This implements the equivalent func in nim.
static Result _deliveryManagerMessageReceiveData(DeliveryManager *d, void* buf, size_t bufsize, s64 total_transfer_size, DeliveryDataTransfer *transfer) {
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

        if (transfer) {
            if (transfer->transfer_handler) {
                rc = transfer->transfer_handler(transfer->userdata, buf, ret, offset); // nim doesn't pass the buf param, it's passed via the object instead. nim passes ptrs for the last 2 params.
                if (R_FAILED(rc)) break;
            }
        }
    }

    if (ret < 0 && R_SUCCEEDED(rc)) {
        rc = _deliveryManagerGetSocketError(d, NULL);
    }

    return rc;
}

Result deliveryManagerCreate(DeliveryManager *d, bool server, const struct in_addr *addr, u16 port) {
    Result rc=0;

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

    d->workbuf_size = 0x20000;
    d->workbuf = malloc(d->workbuf_size);
    if (d->workbuf==NULL)
        rc = MAKERESULT(Module_Nim, NimError_BadInput);
    else memset(d->workbuf, 0, d->workbuf_size);

    if (R_SUCCEEDED(rc)) {
        if (server) rc = _deliveryManagerCreateServerSocket(d);
        else rc = _deliveryManagerCreateClientSocket(d);
    }

    if (R_FAILED(rc)) {
        free(d->workbuf);
        pthread_mutex_destroy(&d->mutex);
        memset(d, 0, sizeof(*d));
    }

    return rc;
}

void deliveryManagerClose(DeliveryManager *d) {
    deliveryManagerCancel(d);
    deliveryManagerGetResult(d);
    pthread_mutex_destroy(&d->mutex);
    free(d->workbuf);

    struct DeliveryContentEntry *entry_cur = d->content_list_first;
    struct DeliveryContentEntry *entry_next;
    while (entry_cur) {
        entry_next = entry_cur->next;
        memset(entry_cur, 0, sizeof(struct DeliveryContentEntry));
        free(entry_cur);
        entry_cur = entry_next;
    }

    memset(d, 0, sizeof(*d));
}

static Result _deliveryManagerAddContentEntry(DeliveryManager *d, struct DeliveryContentEntry *entry) {
    struct DeliveryContentEntry *alloc_entry = (struct DeliveryContentEntry*)malloc(sizeof(struct DeliveryContentEntry));
    if (alloc_entry==NULL) return MAKERESULT(Module_Nim, NimError_BadInput);
    memcpy(alloc_entry, entry, sizeof(struct DeliveryContentEntry));

    struct DeliveryContentEntry **entry_cur = &d->content_list_first;
    struct DeliveryContentEntry *entry_tmp;
    while (*entry_cur) {
        entry_tmp = *entry_cur;
        entry_cur = &entry_tmp->next;
    }
    *entry_cur = alloc_entry;

    return 0;
}

Result deliveryManagerGetContentEntry(DeliveryManager *d, struct DeliveryContentEntry **entry, const NcmContentMetaKey *content_meta_key, const NcmContentId *content_id) {
    struct DeliveryContentEntry *entry_cur = d->content_list_first;
    bool found=0;
    while (entry_cur) {
        if (content_meta_key && entry_cur->is_meta) {
            if (entry_cur->content_meta_key.id==content_meta_key->id && entry_cur->content_meta_key.version==content_meta_key->version && entry_cur->content_meta_key.type==content_meta_key->type)
                found = 1;
            if (found && content_meta_key->type == NcmContentMetaType_SystemUpdate && entry_cur->content_meta_key.install_type!=content_meta_key->install_type) {
                found = 0;
            }
        }
        else if (content_id && memcmp(&entry_cur->content_info.info.content_id, content_id, sizeof(NcmContentId))==0)
            found = 1;
        if (found) {
            *entry = entry_cur;
            return 0;
        }
        entry_cur = entry_cur->next;
    }
    return MAKERESULT(Module_Nim, NimError_DeliveryBadContentMetaKey);
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

void deliveryManagerGetProgress(DeliveryManager *d, s64 *progress_current_size, s64 *progress_total_size) {
    pthread_mutex_lock(&d->mutex);
    *progress_current_size = d->progress_current_size;
    *progress_total_size = d->progress_total_size;
    pthread_mutex_unlock(&d->mutex);
}

void deliveryManagerSetHandlerGetMetaContentRecord(DeliveryManager *d, DeliveryFnGetMetaContentRecord fn, void* userdata) {
    d->handler_get_meta_content_record = fn;
    d->handler_get_meta_content_record_userdata = userdata;
}

void deliveryManagerSetHandlersGetContent(DeliveryManager *d, void* userdata, DeliveryFnContentTransferInit init_handler, DeliveryFnContentTransferExit exit_handler, DeliveryFnContentTransfer transfer_handler) {
    d->handler_get_content.userdata = userdata;
    d->handler_get_content.init_handler = init_handler;
    d->handler_get_content.exit_handler = exit_handler;
    d->handler_get_content.transfer_handler = transfer_handler;
}

Result deliveryManagerClientRequestExit(DeliveryManager *d) {
    if (d->server) return MAKERESULT(Module_Nim, NimError_BadInput);

    DeliveryMessageHeader sendhdr={0};
    _deliveryManagerCreateRequestMessageHeader(&sendhdr, DeliveryMessageId_Exit, 0);
    return _deliveryManagerMessageSendHeader(d, &sendhdr);
}

Result deliveryManagerClientGetMetaContentRecord(DeliveryManager *d, DeliveryContentInfo *out, const NcmContentMetaKey *content_meta_key) {
    Result rc=0;
    DeliveryMessageHeader sendhdr={0}, recvhdr={0};
    NcmPackagedContentInfo record={0};

    memset(out, 0, sizeof(*out));
    if (d->server) return MAKERESULT(Module_Nim, NimError_BadInput);

    _deliveryManagerCreateRequestMessageHeader(&sendhdr, DeliveryMessageId_GetMetaContentRecord, sizeof(NcmContentMetaKey));
    rc = _deliveryManagerMessageSend(d, &sendhdr, content_meta_key, sizeof(NcmContentMetaKey), NULL);
    if (R_SUCCEEDED(rc)) rc = _deliveryManagerMessageReceiveHeader(d, &recvhdr);
    if (R_SUCCEEDED(rc) && recvhdr.id != sendhdr.id) rc = MAKERESULT(Module_Nim, NimError_DeliveryBadMessageId);
    if (R_SUCCEEDED(rc) && recvhdr.data_size != sizeof(record)) rc = MAKERESULT(Module_Nim, NimError_DeliveryBadMessageDataSize);
    if (R_SUCCEEDED(rc)) rc = _deliveryManagerMessageReceiveData(d, &record, sizeof(record), sizeof(record), NULL);

    if (R_SUCCEEDED(rc)) {
        memcpy(&out->content_id, &record.info.content_id, sizeof(NcmContentId));
        out->content_size = (s64)record.info.size[0x0] | ((s64)record.info.size[0x1]<<8) | ((s64)record.info.size[0x2]<<16) | ((s64)record.info.size[0x3]<<24) | ((s64)record.info.size[0x4]<<32) | ((s64)record.info.size[0x5]<<40);
        out->unk_x28 = 0x101;
        memcpy(&out->content_meta_key, content_meta_key, sizeof(*content_meta_key));
        memcpy(out->hash, record.hash, sizeof(record.hash));
    }

    return rc;
}

Result deliveryManagerClientGetContent(DeliveryManager *d, const DeliveryContentInfo *info) {
    Result rc=0;
    DeliveryMessageHeader sendhdr={0}, recvhdr={0};
    DeliveryMessageGetContentArg arg = {.content_id = info->content_id, .flag = info->progress_flag, .pad = {0}};
    struct DeliveryGetContentDataTransferState transfer_state = {.manager = d, .arg = &arg, .userdata = d->handler_get_content.userdata};
    DeliveryDataTransfer transfer = {.userdata = &transfer_state, .transfer_handler = _deliveryManagerGetContentTransferHandler};

    if (d->server) return MAKERESULT(Module_Nim, NimError_BadInput);

    _deliveryManagerCreateRequestMessageHeader(&sendhdr, DeliveryMessageId_GetContent, sizeof(DeliveryMessageGetContentArg));
    rc = _deliveryManagerMessageSend(d, &sendhdr, &arg, sizeof(DeliveryMessageGetContentArg), NULL);
    if (R_SUCCEEDED(rc)) rc = _deliveryManagerMessageReceiveHeader(d, &recvhdr);
    if (R_SUCCEEDED(rc) && recvhdr.id != sendhdr.id) rc = MAKERESULT(Module_Nim, NimError_DeliveryBadMessageId);
    if (R_SUCCEEDED(rc) && recvhdr.data_size != info->content_size) rc = MAKERESULT(Module_Nim, NimError_DeliveryBadMessageDataSize);
    if (R_SUCCEEDED(rc)) {
        if (d->handler_get_content.init_handler) rc = d->handler_get_content.init_handler(&transfer_state, &recvhdr.data_size);

        if (R_SUCCEEDED(rc)) rc = _deliveryManagerMessageReceiveData(d, d->workbuf, d->workbuf_size, recvhdr.data_size, &transfer);
        if (d->handler_get_content.exit_handler) d->handler_get_content.exit_handler(&transfer_state);
    }

    return rc;
}

// We don't support a client impl for DeliveryMessageId_GetCommonTicket.

Result deliveryManagerClientUpdateProgress(DeliveryManager *d, s64 progress_current_size) {
    Result rc=0;
    DeliveryMessageHeader sendhdr={0}, recvhdr={0};
    if (d->server) return MAKERESULT(Module_Nim, NimError_BadInput);

    // nim would load "nim.errorsimulate!error_localcommunication_result" into rc here and return it if needed, we won't do an equivalent.

    _deliveryManagerCreateRequestMessageHeader(&sendhdr, DeliveryMessageId_UpdateProgress, sizeof(progress_current_size));
    progress_current_size = le_dword(progress_current_size);
    rc = _deliveryManagerMessageSend(d, &sendhdr, &progress_current_size, sizeof(progress_current_size), NULL); // nim loads progress_current_size from state.
    if (R_SUCCEEDED(rc)) rc = _deliveryManagerMessageReceiveHeader(d, &recvhdr);
    if (R_SUCCEEDED(rc) && recvhdr.id != sendhdr.id) rc = MAKERESULT(Module_Nim, NimError_DeliveryBadMessageId);
    if (R_SUCCEEDED(rc) && recvhdr.data_size != 0) rc = MAKERESULT(Module_Nim, NimError_DeliveryBadMessageDataSize);

    return rc;
}

static Result _deliveryManagerParseMeta(DeliveryManager *d, const void* meta_buf, size_t meta_size, struct DeliveryContentEntry *entry) {
    Result rc = MAKERESULT(Module_Nim, NimError_BadInput); // Note: not the official error but whatever.
    u8 *meta_bufdata = (u8*)meta_buf;

    if (meta_size < 0x20) return rc;

    memcpy(&entry->content_meta_key, meta_bufdata, sizeof(NcmContentMetaKey));
    entry->content_meta_key.install_type = NcmContentInstallType_Full;
    memset(entry->content_meta_key.padding, 0, sizeof(entry->content_meta_key.install_type));

    return 0;
}

Result deliveryManagerLoadMetaFromFs(const char *dirpath, void** outbuf_ptr, size_t *out_filesize) {
    Result rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    char tmpstr[PATH_MAX];

    DIR* dir;
    struct dirent* dp;
    dir = opendir(dirpath);
    if (!dir) return rc;

    while ((dp = readdir(dir))) {
        if (dp->d_name[0]=='.')
            continue;

        bool entrytype=0;

        memset(tmpstr, 0, sizeof(tmpstr));
        snprintf(tmpstr, sizeof(tmpstr)-1, "%s%s%s", dirpath, "/", dp->d_name);

        struct stat tmpstat;
        if(stat(tmpstr, &tmpstat)==-1)
            continue;

        entrytype = (tmpstat.st_mode & S_IFMT) != S_IFREG;

        if (entrytype) continue;

        if (strncmp(&dp->d_name[strlen(dp->d_name)-5], ".cnmt", 5)!=0) continue;

        rc = 0;

        *out_filesize = tmpstat.st_size;
        *outbuf_ptr = malloc(tmpstat.st_size);
        if (*outbuf_ptr == NULL) {
            rc = MAKERESULT(Module_Nim, NimError_BadInput);
            break;
        }

        FILE *f = fopen(tmpstr, "rb");
        if (f == NULL) {
            free(*outbuf_ptr);
            *outbuf_ptr = NULL;
            rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
            break;
        }
        if (fread(*outbuf_ptr, 1, tmpstat.st_size, f) != tmpstat.st_size) rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
        fclose(f);

        unlink(tmpstr);

        break;
    }

    closedir(dir);
    return rc;
}

Result deliveryManagerScanDataDir(DeliveryManager *d, const char *dirpath, s32 depth, DeliveryFnMetaLoad meta_load, void* meta_load_userdata) {
    Result rc=0;
    int pos;
    char dirsep[8];
    struct DeliveryContentEntry entry={0};
    void* meta_buf = NULL;
    size_t meta_size = 0;
    size_t remaining_size, cur_size;
    FILE *f = NULL;
    char contentid_str[33];

    #ifndef __SWITCH__
    struct sha256_ctx hash_ctx;
    #else
    Sha256Context hash_ctx;
    #endif

    if (!d->server) return MAKERESULT(Module_Nim, NimError_BadInput);

    memset(dirsep, 0, sizeof(dirsep));
    dirsep[0] = '/';

    // Don't append a '/' if the path already has one.
    pos = strlen(dirpath);
    if (pos > 0) {
        if (dirpath[pos-1] == '/') dirsep[0] = 0;
    }

    DIR* dir;
    struct dirent* dp;
    char tmp_path[PATH_MAX];
    dir = opendir(dirpath);
    if (!dir) return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    while ((dp = readdir(dir))) {
        if (dp->d_name[0]=='.')
            continue;

        bool entrytype=0;

        memset(tmp_path, 0, sizeof(tmp_path));
        snprintf(tmp_path, sizeof(tmp_path)-1, "%s%s%s", dirpath, dirsep, dp->d_name);

        struct stat tmpstat;
        if(stat(tmp_path, &tmpstat)==-1)
            continue;

        entrytype = (tmpstat.st_mode & S_IFMT) != S_IFREG;

        if (entrytype) {
            if (depth > 0) {
                rc = deliveryManagerScanDataDir(d, tmp_path, depth-1, meta_load, meta_load_userdata);
                if (R_FAILED(rc)) break;
            }
        }
        else {
            if (strlen(dp->d_name) >= 32) {
                bool found=0;
                u32 name_type=0;
                bool is_meta=0;

                pos = strlen(dp->d_name)-4;
                if (strlen(dp->d_name) == 32+4 && strncmp(&dp->d_name[pos], ".nca", 4)==0) {
                    found = 1;
                    name_type = 0;
                }
                else {
                    pos = strlen(dp->d_name)-9;
                    if (strlen(dp->d_name) == 32+9 && strncmp(&dp->d_name[pos], ".cnmt.nca", 9)==0) {
                        found = 1;
                        name_type = 1;
                    }
                }
                if (!found) {
                    pos = strlen(dp->d_name);
                    found = 1;
                    name_type = 2;
                }

                int pos2=0;
                pos--;
                for (; pos>=0 && pos2<32; pos--, pos2++) {
                    if (!isxdigit((int)dp->d_name[pos])) {
                        found = 0;
                        break;
                    }
                }

                if (!found) continue;
                pos++;

                memset(&entry, 0, sizeof(entry));
                entry.filesize = tmpstat.st_size;
                strncpy(entry.filepath, tmp_path, sizeof(entry.filepath));
                entry.filepath[sizeof(entry.filepath)-1] = 0;

                for (pos2=0; pos2<32; pos2+=2) {
                    u32 tmp=0;
                    sscanf(&dp->d_name[pos+pos2], "%02x", &tmp);
                    entry.content_info.info.content_id.c[pos2/2] = tmp;
                }

                entry.content_info.info.size[0] = (u8)(tmpstat.st_size);
                entry.content_info.info.size[1] = (u8)(tmpstat.st_size>>8);
                entry.content_info.info.size[2] = (u8)(tmpstat.st_size>>16);
                entry.content_info.info.size[3] = (u8)(tmpstat.st_size>>24);
                entry.content_info.info.size[4] = (u8)(tmpstat.st_size>>32);
                entry.content_info.info.size[5] = (u8)(tmpstat.st_size>>40);

                is_meta = name_type == 1 || (name_type == 2 && strncmp(dp->d_name, "ncatype0_", 9)==0);
                if (is_meta) rc = meta_load(meta_load_userdata, &entry, tmp_path, &meta_buf, &meta_size);
                if (R_SUCCEEDED(rc) && is_meta) {
                    rc = _deliveryManagerParseMeta(d, meta_buf, meta_size, &entry);
                    if (R_FAILED(rc)) {
                        TRACE(d, "_deliveryManagerParseMeta() failed (0x%x) with path: %s", rc, tmp_path);
                        break;
                    }
                    entry.is_meta = 1;

                    // The Meta doesn't include the content_info for the Meta itself even for non-SystemUpdate, so generate it here.
                    entry.content_info.info.content_type = NcmContentType_Meta;

                    if (tmpstat.st_size > 0) { // Calculate the hash.
                        f = fopen(tmp_path, "rb");
                        if (f==NULL) {
                            rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
                            TRACE(d, "Failed to open content file: %s", tmp_path);
                        }
                        else {
                            memset(d->workbuf, 0, d->workbuf_size);

                            #ifndef __SWITCH__
                            sha256_init(&hash_ctx);
                            #else
                            sha256ContextCreate(&hash_ctx);
                            #endif

                            cur_size = d->workbuf_size;
                            for (remaining_size=tmpstat.st_size; remaining_size>0; remaining_size-=cur_size) {
                                if (cur_size > remaining_size) cur_size = remaining_size;

                                if (fread(d->workbuf, 1, cur_size, f) != cur_size) {
                                    rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
                                    TRACE(d, "Reading the content file failed: %s", tmp_path);
                                    break;
                                }

                                #ifndef __SWITCH__
                                sha256_update(&hash_ctx, cur_size, d->workbuf);
                                #else
                                sha256ContextUpdate(&hash_ctx, d->workbuf, cur_size);
                                #endif
                            }

                            memset(d->workbuf, 0, d->workbuf_size);
                            fclose(f);

                            if (R_SUCCEEDED(rc)) {
                                #ifndef __SWITCH__
                                sha256_digest(&hash_ctx, sizeof(entry.content_info.hash), entry.content_info.hash);
                                #else
                                sha256ContextGetHash(&hash_ctx, entry.content_info.hash);
                                #endif

                                // nim doesn't do this server-side, but we will.
                                if (memcmp(&entry.content_info.info.content_id, entry.content_info.hash, sizeof(entry.content_info.info.content_id))!=0) {
                                    rc = MAKERESULT(Module_Nim, NimError_BadInput);
                                    TRACE(d, "ContentId/hash mismatch for the following Meta content path: %s", tmp_path);
                                }
                            }
                        }

                        if (R_FAILED(rc)) break;
                    }
                }

                memset(contentid_str, 0, sizeof(contentid_str));
                _deliveryManagerPrintContentId(contentid_str, &entry.content_info.info.content_id);
                TRACE(d, "Adding (is_meta=%d, entry.is_meta=%d) content entry with ContentId %s. rc = 0x%x. Path: %s", is_meta, entry.is_meta, contentid_str, rc, tmp_path);

                rc = _deliveryManagerAddContentEntry(d, &entry);
                if (R_FAILED(rc)) break;
            }
        }
    }

    closedir(dir);
    return rc;
}

