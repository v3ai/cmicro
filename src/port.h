/* file: port.h  (universal C portability layer, copied unchanged from
   https://github.com/v3ai/universal-c UNIVERSAL_C.md section 4) */
#ifndef P_PORT_H
#define P_PORT_H

#include <limits.h>
#include <stddef.h>

/* ---- language version ------------------------------------------------ */
#if defined(__cplusplus)
#  define P_STDC 0L
#elif defined(__STDC_VERSION__)
#  define P_STDC __STDC_VERSION__
#else
#  define P_STDC 198900L
#endif

/* ---- hard platform assumptions --------------------------------------- */
#if CHAR_BIT != 8
#  error "universal C requires 8-bit bytes"
#endif

/* ---- inline / restrict ----------------------------------------------- */
#if defined(__cplusplus) || P_STDC >= 199901L
#  define P_INLINE inline
#elif defined(__GNUC__)
#  define P_INLINE __inline__
#elif defined(_MSC_VER)
#  define P_INLINE __inline
#else
#  define P_INLINE
#endif

#if !defined(__cplusplus) && P_STDC >= 199901L
#  define P_RESTRICT restrict
#elif defined(__GNUC__)
#  define P_RESTRICT __restrict__
#elif defined(_MSC_VER)
#  define P_RESTRICT __restrict
#else
#  define P_RESTRICT
#endif

/* ---- boolean (never collides with C23/C++ `bool`) --------------------- */
typedef int p_bool;
#define P_TRUE  1
#define P_FALSE 0

/* ---- exact-width integers without <stdint.h> -------------------------- */
typedef unsigned char p_u8;
typedef signed char   p_s8;

#if USHRT_MAX == 0xFFFFU
typedef unsigned short p_u16;
typedef short          p_s16;
#elif UINT_MAX == 0xFFFFU
typedef unsigned int   p_u16;
typedef int            p_s16;
#else
#  error "no 16-bit integer type"
#endif

#if UINT_MAX == 0xFFFFFFFFUL
typedef unsigned int   p_u32;
typedef int            p_s32;
#elif ULONG_MAX == 0xFFFFFFFFUL
typedef unsigned long  p_u32;
typedef long           p_s32;
#else
#  error "no 32-bit integer type"
#endif

/* 64-bit: optional. Code MUST check P_HAS_64 before using p_u64/p_s64. */
#if (!defined(__cplusplus) && P_STDC >= 199901L) || \
    (defined(__cplusplus) && __cplusplus >= 201103L)
#  include <stdint.h>
typedef uint64_t p_u64;
typedef int64_t  p_s64;
#  define P_U64_C(c) UINT64_C(c)
#  define P_HAS_64 1
#elif ((ULONG_MAX >> 31) >> 31) == 3
typedef unsigned long p_u64;
typedef long          p_s64;
#  define P_U64_C(c) c##UL
#  define P_HAS_64 1
#elif defined(_MSC_VER)
typedef unsigned __int64 p_u64;
typedef __int64          p_s64;
#  define P_U64_C(c) c##ui64
#  define P_HAS_64 1
#elif defined(__GNUC__)
/* GCC/Clang in C89 mode on ILP32 or LLP64 (32-bit Linux, MinGW): long long exists
   as an extension; __extension__ keeps -pedantic-errors quiet. */
__extension__ typedef unsigned long long p_u64;
__extension__ typedef long long          p_s64;
#  define P_U64_C(c) (__extension__ c##ULL)
#  define P_HAS_64 1
#else
#  define P_HAS_64 0
#endif

#define P_SIZE_MAX ((size_t)-1)

/* ---- utilities --------------------------------------------------------- */
#define P_CAT_(a, b) a##b
#define P_CAT(a, b)  P_CAT_(a, b)
/* File scope only. `tag` must be a unique identifier fragment. */
#define P_STATIC_ASSERT(cond, tag) \
    typedef char P_CAT(p_static_assert_, tag)[(cond) ? 1 : -1]
#define P_UNUSED(x)    ((void)(x))
#define P_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Wrap public headers: P_BEGIN_DECLS ... P_END_DECLS */
#ifdef __cplusplus
#  define P_BEGIN_DECLS extern "C" {
#  define P_END_DECLS   }
#else
#  define P_BEGIN_DECLS
#  define P_END_DECLS
#endif

P_STATIC_ASSERT(sizeof(p_u16) == 2, u16_size);
P_STATIC_ASSERT(sizeof(p_u32) == 4, u32_size);
P_STATIC_ASSERT((-1 & 3) == 3, twos_complement);

#endif /* P_PORT_H */
