#pragma once

#include "tinypy/objects/base.h"
#include "tinypy/common/chunkedvector.h"
#include "tinypy/config.h"
#include <stdint.h>

#define FIXEDHASH_T__HEADER
#define K py_Name
#define V py_TValue
#define NAME CachedNames
#include "tinypy/xmacros/fixedhash.h"
#undef FIXEDHASH_T__HEADER