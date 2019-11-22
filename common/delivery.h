#pragma once
#include <pthread.h>
#include "types.h"
#include "result.h"

/// Default port.
#define DELIVERY_PORT_DEFAULT 55556

/// Magicnum for request messages.
#define DELIVERY_MESSAGE_MAGICNUM_REQUEST 0x7152444c

/// Magicnum for reply messages.
#define DELIVERY_MESSAGE_MAGICNUM_REPLY 0x7352444c

/// DeliveryMessageId
typedef enum {
    DeliveryMessageId_Exit                 = 0,
    DeliveryMessageId_GetMetaContentRecord = 1,
    DeliveryMessageId_GetContent           = 2,
    DeliveryMessageId_GetCommonTicket      = 3,
    DeliveryMessageId_UpdateProgress       = 4,
} DeliveryMessageId;

/// DeliveryManager
typedef struct {
    pthread_mutex_t mutex;
    pthread_t thread;
    bool cancel_flag;
    bool server;
    struct in_addr addr;
    u16 port;
    int listen_sockfd;
    int conn_sockfd;
    Result rc;

    s64 progress_current_size;
    s64 progress_total_size;
} DeliveryManager;

/// DeliveryMessageHeader
typedef struct {
    u32 magicnum;         ///< Magicnum, must match \ref DELIVERY_MESSAGE_MAGICNUM_REQUEST or \ref DELIVERY_MESSAGE_MAGICNUM_REPLY.
    u8 id;                ///< \ref DeliveryMessageId
    u8 pad;               ///< Padding.
    u16 meta_size;        ///< Must be <=0x1000.
    s64 data_size;        ///< Must not be negative.
} DeliveryMessageHeader;

typedef struct {
    u8 content_id[0x10];  ///< NcmContentId
    u8 flag;              ///< When zero, server updates the progress total_size by the content_size, during the transfer.
    u8 pad[7];            ///< Padding.
} DeliveryMessageGetContentArg;

/// Create a \ref DeliveryManager.
Result deliveryManagerCreate(DeliveryManager *d, bool server, const struct in_addr *addr, u16 port);

/// Close a \ref DeliveryManager.
void deliveryManagerClose(DeliveryManager *d);

/// Start the task thread, only available when \ref deliveryManagerCreate was used with server=true.
Result deliveryManagerRequestRun(DeliveryManager *d);

/// Cancel the server task. Used by \ref deliveryManagerClose.
void deliveryManagerCancel(DeliveryManager *d);

/// Wait for the server task to finish and returns the Result. Used by \ref deliveryManagerClose.
Result deliveryManagerGetResult(DeliveryManager *d);

/// Client-mode only. Tells the server to exit.
Result deliveryManagerClientRequestExit(DeliveryManager *d);

Result deliveryManagerClientUpdateProgress(DeliveryManager *d, s64 progress_current_size);

