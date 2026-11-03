#pragma once
#include "pocketpy/pocketpy.h"
typedef struct PyObject { int slots; unsigned char flex[sizeof(void*)]; } PyObject;
