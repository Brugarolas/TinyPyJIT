#pragma once
#include "pocketpy/pocketpy.h"
static inline uint64_t pk_name_hash(py_Name name) { return name->hash; }
