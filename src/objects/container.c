#include "tinypy/objects/container.h"

#define FIXEDHASH_T__SOURCE
#define K py_Name
#define V py_TValue
#define NAME CachedNames
#include "tinypy/xmacros/fixedhash.h"
#undef FIXEDHASH_T__SOURCE
