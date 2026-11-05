#include "tinypy/common/smallmap.h"

#define SMALLMAP_T__SOURCE
#define K py_Name
#define V int
#define NAME c11_smallmap_n2d
#include "tinypy/xmacros/smallmap.h"
#undef SMALLMAP_T__SOURCE

#define SMALLMAP_T__SOURCE
#define K int
#define V int
#define NAME c11_smallmap_d2d
#include "tinypy/xmacros/smallmap.h"
#undef SMALLMAP_T__SOURCE

#define SMALLMAP_T__SOURCE
#define K c11_sv
#define V int
#define NAME c11_smallmap_v2d
#define less(a, b)      (c11_sv__cmp((a), (b)) <  0)
#define equal(a, b)     (c11_sv__cmp((a), (b)) == 0)
#include "tinypy/xmacros/smallmap.h"
#undef SMALLMAP_T__SOURCE


#define SMALLMAP_T__SOURCE
#define K void*
#define V py_i64
#define NAME c11_smallmap_p2i
#include "tinypy/xmacros/smallmap.h"
#undef SMALLMAP_T__SOURCE
