#pragma once
#include <pthread.h>

#include "switch/result.h"
#include "switch/services/ncm_types.h"

/// Default port.
#define DELIVERY_PORT_DEFAULT 55556

/// Magicnum for request messages.
#define DELIVERY_MESSAGE_MAGICNUM_REQUEST 0x7152444c

/// Magicnum for reply messages.
#define DELIVERY_MESSAGE_MAGICNUM_REPLY 0x7352444c

struct DeliveryGetContentDataTransferState;

/// Handler func for GetMetaContentRecord, params: (userdata, {0x38-byte output ContentRecord}, {0x10-byte input ContentMetaKey}).
typedef Result (*DeliveryFnGetMetaContentRecord)(void*, void*, const void*);

/// Data transfer func, params: (userdata, buffer, size, offset)
typedef Result (*DeliveryFnDataTransfer)(void*, void*, u64, s64);

/// Data transfer cleanup func, params: (userdata)
typedef Result (*DeliveryFnDataTransferExit)(void*);

/// Content data transfer init func, params: (state, output_content_size)
typedef Result (*DeliveryFnContentTransferInit)(struct DeliveryGetContentDataTransferState*, s64*);

/// Content data transfer cleanup func, params: (state)
typedef void (*DeliveryFnContentTransferExit)(struct DeliveryGetContentDataTransferState*);

/// Content data transfer func, params: (state, buffer, size, offset)
typedef Result (*DeliveryFnContentTransfer)(struct DeliveryGetContentDataTransferState*, void*, u64, s64);

/// Result module values
enum {
    Module_Nim=137,
};

/// Nim error codes
enum {
    NimError_BadInput=40,                     ///< Memory allocation failed / bad input.
    NimError_BadContentMetaType=330,          ///< ContentMetaType doesn't match SystemUpdate.
    NimError_DeliverySocketError=5001,        ///< One of the following socket errors occurred: ENETDOWN, ECONNRESET, EHOSTDOWN, EHOSTUNREACH, or EPIPE. Also occurs when the received size doesn't match the expected size (recvfrom() ret with size0 data receiving).
    NimError_DeliveryOperationCancelled=5010, ///< Socket was shutdown() due to the async operation being cancelled.
    NimError_UnknownError=5020,               ///< Too many internal output entries with nim cmd42, system is Internet-connected, or an unrecognized socket error occured.
    NimError_DeliveryConnectionTimeout=5100,  ///< Connection timeout.
    NimError_DeliveryBadMessageId=5410,       ///< Invalid ID.
    NimError_DeliveryBadMessageMagicnum=5420, ///< Invalid magicnum.
    NimError_DeliveryBadMessageDataSize=5430, ///< Invalid data_size.
    NimError_DeliveryBadContentMetaKey=5440,  ///< The input ContentMetaKey doesn't match the ContentMetaKey in state.
    NimError_DeliveryBadMessageMetaSize=5450, ///< Invalid meta_size.
};

/// DeliveryMessageId
typedef enum {
    DeliveryMessageId_Exit                 = 0,
    DeliveryMessageId_GetMetaContentRecord = 1,
    DeliveryMessageId_GetContent           = 2,
    DeliveryMessageId_GetCommonTicket      = 3,
    DeliveryMessageId_UpdateProgress       = 4,
} DeliveryMessageId;

/// DeliveryDataTransfer
typedef struct {
    void* userdata;
    DeliveryFnDataTransfer transfer_handler;
    DeliveryFnDataTransferExit exit_handler;
} DeliveryDataTransfer;

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

    void* workbuf;
    size_t workbuf_size;

    DeliveryFnGetMetaContentRecord handler_get_meta_content_record;
    void* handler_get_meta_content_record_userdata;

    struct {
        void* userdata;
        DeliveryFnContentTransferInit init_handler;
        DeliveryFnContentTransferExit exit_handler;
        DeliveryFnContentTransfer transfer_handler;
    } handler_get_content;
} DeliveryManager;

/// DeliveryMessageHeader
typedef struct {
    u32 magicnum;         ///< Magicnum, must match \ref DELIVERY_MESSAGE_MAGICNUM_REQUEST or \ref DELIVERY_MESSAGE_MAGICNUM_REPLY.
    u8 id;                ///< \ref DeliveryMessageId
    u8 pad;               ///< Padding.
    u16 meta_size;        ///< Must be <=0x1000.
    s64 data_size;        ///< Must not be negative.
} DeliveryMessageHeader;

/// DeliveryMessageGetContentArg
typedef struct {
    u8 content_id[0x10];  ///< NcmContentId
    u8 flag;              ///< When zero, server updates the progress total_size by the content_size, during the transfer.
    u8 pad[7];            ///< Padding.
} DeliveryMessageGetContentArg;

/// DeliveryGetContentDataTransferState
struct DeliveryGetContentDataTransferState {
    DeliveryManager *manager;
    DeliveryMessageGetContentArg *arg;
    void* userdata;
};

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

/// Get the progress, only valid for server-mode.
void deliveryManagerGetProgress(DeliveryManager *d, s64 *progress_current_size, s64 *progress_total_size);

/// Sets the server handler for GetMetaContentRecord.
void deliveryManagerSetHandlerGetMetaContentRecord(DeliveryManager *d, DeliveryFnGetMetaContentRecord fn, void* userdata);

/// Sets the handlers for GetContent.
void deliveryManagerSetHandlersGetContent(DeliveryManager *d, void* userdata, DeliveryFnContentTransferInit init_handler, DeliveryFnContentTransferExit exit_handler, DeliveryFnContentTransfer transfer_handler);

/// Client-mode only. Tells the server to exit.
Result deliveryManagerClientRequestExit(DeliveryManager *d);

/// Client-mode only. Update the server progress_current_size.
Result deliveryManagerClientUpdateProgress(DeliveryManager *d, s64 progress_current_size);

