/**
 * @file result.h
 * @brief Switch result code tools.
 * @copyright libnx Authors
 */
#pragma once

// Imported from libnx, with adjustments.

/// Checks whether a result code indicates success.
#define R_SUCCEEDED(res)   ((res)==0)
/// Checks whether a result code indicates failure.
#define R_FAILED(res)      ((res)!=0)
/// Returns the module ID of a result code.
#define R_MODULE(res)      ((res)&0x1FF)
/// Returns the description of a result code.
#define R_DESCRIPTION(res) (((res)>>9)&0x1FFF)
/// Masks out unused bits in a result code, retrieving the actual value for use in comparisons.
#define R_VALUE(res)       ((res)&0x3FFFFF)

/// Builds a result code from its constituent components.
#define MAKERESULT(module,description) \
    ((((module)&0x1FF)) | ((description)&0x1FFF)<<9)

/// Builds a kernel error result code.
#define KERNELRESULT(description) \
    MAKERESULT(Module_Kernel, KernelError_##description)

/// Module values
enum {
    Module_Kernel=1,
    Module_Nim=137,
    Module_Libnx=345,
    Module_HomebrewAbi=346,
    Module_HomebrewLoader=347,
    Module_LibnxNvidia=348,
    Module_LibnxBinder=349,
};

/// Nim error codes
enum {
    NimError_BadInput=40,                     ///< Memory allocation failed / bad input.
    NimError_BadContentMetaType=330,          ///< ContentMetaType doesn't match SystemUpdate.
    NimError_DeliverySocketError=5001,        ///< One of the following socket errors occurred: ENETDOWN, ECONNRESET, EHOSTDOWN, EHOSTUNREACH, or EPIPE. Also occurs when the received size doesn't match the expected size (recvfrom() ret with size0 data receiving). May also occur when {same cause as NimError_DeliveryOperationCancelled} occurs.
    NimError_DeliveryOperationCancelled=5010, ///< Socket was shutdown() due to the async operation being cancelled.
    NimError_UnknownError=5020,               ///< Too many internal output entries with nim cmd42, system is Internet-connected, or an unrecognized socket error occured.
    NimError_DeliveryConnectionTimeout=5100,  ///< Connection timeout.
    NimError_DeliveryBadMessageId=5410,       ///< Invalid ID.
    NimError_DeliveryBadMessageMagicnum=5420, ///< Invalid magicnum.
    NimError_DeliveryBadMessageSize1=5430,    ///< Invalid size1.
    NimError_DeliveryBadContentMetaKey=5440,  ///< The input ContentMetaKey doesn't match the ContentMetaKey in state.
    NimError_DeliveryBadMessageSize0=5450,    ///< Invalid size0.
};

