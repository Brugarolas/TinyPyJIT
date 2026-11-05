#pragma once

#include "tinypy/tinypy.h"

#ifdef __cplusplus
extern "C" {
#endif

void pk_names_initialize();
void pk_names_finalize();

#define MAGIC_METHOD(x) extern py_Name x;
#include "tinypy/xmacros/magics.h"
#undef MAGIC_METHOD

py_Name py_namev(c11_sv name);
c11_sv py_name2sv(py_Name index);
py_Name py_name(const char* name);
const char* py_name2str(py_Name index);

/*
 * Return a stable hash for an interned name.
 *
 * The built-in name implementation already computes and stores a 64-bit string
 * hash when the name is interned, so dictionary lookups should reuse it instead
 * of hashing the py_Name pointer again. Advanced hosts using a custom name
 * implementation are not required to expose internal storage: the fallback
 * below hashes the stable interned-name pointer cheaply.
 */
#if PK_ENABLE_CUSTOM_SNAME == 0
uint64_t pk_name_hash(py_Name index);
#else
static inline uint64_t pk_name_hash(py_Name index) {
    uintptr_t x = (uintptr_t)index;
    x >>= 3;
    x ^= x >> 11;
    x *= (uintptr_t)0x9E3779B1u;
    x ^= x >> 7;
    return (uint64_t)x;
}
#endif

#ifdef __cplusplus
}  // extern "C"
#endif
