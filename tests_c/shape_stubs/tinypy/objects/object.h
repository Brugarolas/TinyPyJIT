#pragma once
#include "tinypy/tinypy.h"
typedef struct PyObject { int slots; unsigned char flex[sizeof(void*)]; } PyObject;
