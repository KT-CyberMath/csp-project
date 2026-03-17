#ifndef ENDIAN_COMPAT_H
#define ENDIAN_COMPAT_H

#include <stdint.h>

#if defined(__linux__)
#include <endian.h>
#elif defined(__APPLE__)
#include <libkern/OSByteOrder.h>

#ifndef htobe64
#define htobe64(x) OSSwapHostToBigInt64((uint64_t)(x))
#endif
#ifndef be64toh
#define be64toh(x) OSSwapBigToHostInt64((uint64_t)(x))
#endif
#ifndef htobe32
#define htobe32(x) OSSwapHostToBigInt32((uint32_t)(x))
#endif
#ifndef be32toh
#define be32toh(x) OSSwapBigToHostInt32((uint32_t)(x))
#endif
#else
#error "Unsupported platform for endian conversion helpers"
#endif

#endif
