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
struct DeliveryContentEntry;

/// Handler func for GetMetaPackagedContentInfo, params: (userdata, output NcmPackagedContentInfo, input NcmContentMetaKey).
typedef Result (*DeliveryFnGetMetaPackagedContentInfo)(void*, NcmPackagedContentInfo*, const NcmContentMetaKey*);

/// Data transfer func, params: (userdata, buffer, size, offset)
typedef Result (*DeliveryFnDataTransfer)(void*, void*, u64, s64);

/// Data transfer cleanup func, params: (userdata)
typedef Result (*DeliveryFnDataTransferExit)(void*);

/// Content data transfer init func, params: (state, output_content_size). With client-mode, the latter is an input ptr to the content_size from recvhdr.data_size.
typedef Result (*DeliveryFnContentTransferInit)(struct DeliveryGetContentDataTransferState*, s64*);

/// Content data transfer cleanup func, params: (state)
typedef void (*DeliveryFnContentTransferExit)(struct DeliveryGetContentDataTransferState*);

/// Content data transfer func, params: (state, buffer, size, offset)
typedef Result (*DeliveryFnContentTransfer)(struct DeliveryGetContentDataTransferState*, void*, u64, s64);

/// .cnmt loading func, params: (userdata, DeliveryContentEntry*, filepath, ptr to output buffer, ptr to output size)
typedef Result (*DeliveryFnMetaLoad)(void*, struct DeliveryContentEntry*, const char*, void**, size_t*);

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
    NimError_DeliveryBadMessageMagicnum=5420, ///< Invalid magicnum. Can also be caused by the connection being closed by the peer, since non-negative return values from recv() are ignored in this case.
    NimError_DeliveryBadMessageDataSize=5430, ///< Invalid data_size.
    NimError_DeliveryBadContentMetaKey=5440,  ///< The input ContentMetaKey doesn't match the ContentMetaKey in state.
    NimError_DeliveryBadMessageMetaSize=5450, ///< Invalid meta_size.
};

/// DeliveryMessageId
typedef enum {
    DeliveryMessageId_Exit                       = 0,
    DeliveryMessageId_GetMetaPackagedContentInfo = 1,
    DeliveryMessageId_GetContent                 = 2,
    DeliveryMessageId_GetCommonTicket            = 3,
    DeliveryMessageId_UpdateProgress             = 4,
} DeliveryMessageId;

/// DeliveryDataTransfer
typedef struct {
    void* userdata;
    DeliveryFnDataTransfer transfer_handler;
    DeliveryFnDataTransferExit exit_handler;
} DeliveryDataTransfer;

/// DeliveryContentEntry. Data loaded by deliveryManagerScanDataDir.
struct DeliveryContentEntry {
    struct DeliveryContentEntry *next;
    bool is_meta;
    NcmContentMetaKey content_meta_key;
    NcmPackagedContentInfo content_info;

    size_t filesize;
    char filepath[PATH_MAX];
};

/// DeliveryManager
typedef struct {
    pthread_mutex_t mutex;
    pthread_t thread;
    bool initialized;
    bool thread_started;
    bool thread_finished;
    bool cancel_flag;
    bool server;
    struct in_addr addr;
    u16 port;
    int listen_sockfd;
    int conn_sockfd;
    Result rc;
    FILE *log_file;

    s64 progress_current_size;
    s64 progress_total_size;

    void* workbuf;
    size_t workbuf_size;

    DeliveryFnGetMetaPackagedContentInfo handler_get_meta_packaged_content_info;
    void* handler_get_meta_packaged_content_info_userdata;

    struct {
        void* userdata;
        DeliveryFnContentTransferInit init_handler;
        DeliveryFnContentTransferExit exit_handler;
        DeliveryFnContentTransfer transfer_handler;
    } handler_get_content;

    struct DeliveryContentEntry *content_list_first;
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
    NcmContentId content_id;  ///< NcmContentId
    u8 flag;                  ///< When zero, server updates the progress total_size by the content_size, during the transfer.
    u8 pad[7];                ///< Padding.
} DeliveryMessageGetContentArg;

/// DeliveryGetContentDataTransferState
struct DeliveryGetContentDataTransferState {
    DeliveryManager *manager;
    DeliveryMessageGetContentArg *arg;
    void* userdata;
};

/// Internal struct used by nim, proper name unknown.
typedef struct {
    NcmContentId content_id;                 ///< NcmContentId. Loaded from NcmPackagedContentInfo and used with DeliveryMessageGetContentArg::content_id.
    s64 content_size;                        ///< Content size. Loaded from NcmPackagedContentInfo.
    NcmContentMetaKey content_meta_key;      ///< NcmContentMetaKey. Loaded from deliveryManagerClientGetMetaPackagedContentInfo input.
    u16 unk_x28;
    u8 unk_x2a[2];
    u8 progress_flag;                        ///< Used with DeliveryMessageGetContentArg::flag.
    u8 unk_x2d[3];
    u8 hash[0x20];                           ///< Content hash. Loaded from NcmPackagedContentInfo.
} DeliveryContentInfo;

/// Create a \ref DeliveryManager.
Result deliveryManagerCreate(DeliveryManager *d, bool server, const struct in_addr *addr, u16 port);

/// Close a \ref DeliveryManager.
void deliveryManagerClose(DeliveryManager *d);

/// Set the FILE* for logging.
static inline void deliveryManagerSetLogFile(DeliveryManager *d, FILE *f) {
    d->log_file = f;
}

/// Gets the DeliveryContentEntry which has data matching the input.
Result deliveryManagerGetContentEntry(DeliveryManager *d, struct DeliveryContentEntry **entry, const NcmContentMetaKey *content_meta_key, const NcmContentId *content_id);

/// Start the task thread, only available when \ref deliveryManagerCreate was used with server=true.
Result deliveryManagerRequestRun(DeliveryManager *d);

/// Cancel the server task. Used by \ref deliveryManagerClose.
void deliveryManagerCancel(DeliveryManager *d);

/// Wait for the server task to finish and returns the Result. Used by \ref deliveryManagerClose.
Result deliveryManagerGetResult(DeliveryManager *d);

/// Gets whether the server task thread finished running.
bool deliveryManagerCheckFinished(DeliveryManager *d);

/// Get the progress, only valid for server-mode.
void deliveryManagerGetProgress(DeliveryManager *d, s64 *progress_current_size, s64 *progress_total_size);

/// Sets the server handler for GetMetaPackagedContentInfo.
void deliveryManagerSetHandlerGetMetaPackagedContentInfo(DeliveryManager *d, DeliveryFnGetMetaPackagedContentInfo fn, void* userdata);

/// Sets the server/client handlers for GetContent.
void deliveryManagerSetHandlersGetContent(DeliveryManager *d, void* userdata, DeliveryFnContentTransferInit init_handler, DeliveryFnContentTransferExit exit_handler, DeliveryFnContentTransfer transfer_handler);

/// Client-mode only. Tells the server to exit.
Result deliveryManagerClientRequestExit(DeliveryManager *d);

/// Client-mode only. Gets the DeliveryContentInfo for the specified ContentMetaKey.
Result deliveryManagerClientGetMetaPackagedContentInfo(DeliveryManager *d, DeliveryContentInfo *out, const NcmContentMetaKey *content_meta_key);

/// Client-mode only. Gets the content data using the specified DeliveryContentInfo. See deliveryManagerSetHandlersGetContent.
Result deliveryManagerClientGetContent(DeliveryManager *d, const DeliveryContentInfo *info);

/// Client-mode only. Update the server progress_total_size.
Result deliveryManagerClientUpdateProgress(DeliveryManager *d, s64 progress_total_size);

/// Helper func for use by \ref DeliveryFnMetaLoad. dirpath is the dirpath for the Meta content fs.
Result deliveryManagerLoadMetaFromFs(const char *dirpath, void** outbuf_ptr, size_t *out_filesize, bool deleteflag);

/// Server-mode only. Scan the specified sysupdate data-dir.
Result deliveryManagerScanDataDir(DeliveryManager *d, const char *dirpath, s32 depth, DeliveryFnMetaLoad meta_load, void* meta_load_userdata);

