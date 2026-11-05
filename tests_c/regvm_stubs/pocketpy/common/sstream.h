#pragma once
#include "tinypy/tinypy.h"
typedef struct c11_sbuf { int dummy; } c11_sbuf;
void c11_sbuf__ctor(c11_sbuf*); void c11_sbuf__dtor(c11_sbuf*); void c11_sbuf__write_sv(c11_sbuf*,c11_sv); void c11_sbuf__py_submit(c11_sbuf*,py_Ref);
