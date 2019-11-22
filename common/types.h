#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef uint64_t dword_t;
typedef uint32_t word_t;
typedef uint16_t hword_t;
typedef uint8_t byte_t;
typedef int64_t dlong_t;
typedef int32_t long_t;
typedef int16_t short_t;
typedef int8_t char_t;
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t s64;

typedef u32 Result;                 ///< Function error code result type.

#define BIT(n) (1U << (n))

