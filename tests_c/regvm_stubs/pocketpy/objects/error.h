#pragma once
#include "pocketpy/pocketpy.h"
bool TypeError(const char*,...); bool ValueError(const char*,...); bool NameError(py_Name); bool UnboundLocalError(py_Name);
