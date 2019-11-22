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

