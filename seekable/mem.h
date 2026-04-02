/*
** Minimal shim for zstd internal mem.h types used by seekable format.
** Provides U32, BYTE, MEM_writeLE32, MEM_readLE32.
*/
#ifndef MEM_H_SHIM
#define MEM_H_SHIM

#include <stdint.h>
#include <string.h>

typedef uint8_t  BYTE;
typedef uint16_t U16;
typedef uint32_t U32;
typedef uint64_t U64;
typedef int32_t  S32;
typedef int64_t  S64;

static inline U32 MEM_readLE32(const void *ptr) {
    U32 val;
    memcpy(&val, ptr, sizeof(val));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    val = __builtin_bswap32(val);
#endif
    return val;
}

static inline void MEM_writeLE32(void *ptr, U32 val) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    val = __builtin_bswap32(val);
#endif
    memcpy(ptr, &val, sizeof(val));
}

#endif /* MEM_H_SHIM */
