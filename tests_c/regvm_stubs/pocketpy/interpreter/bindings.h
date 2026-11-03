#pragma once
#include "pocketpy/pocketpy.h"
int generator__iternext(py_Ref); int array2d_like_iterator__iternext(py_Ref); int list_iterator__iternext(py_Ref); int tuple_iterator__iternext(py_Ref); int dict_items__iternext(py_Ref); int range_iterator__iternext(py_Ref); int str_iterator__iternext(py_Ref); int py_next(py_Ref);
