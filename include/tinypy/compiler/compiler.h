#pragma once

#include "tinypy/common/vector.h"
#include "tinypy/compiler/lexer.h"
#include "tinypy/objects/sourcedata.h"
#include "tinypy/objects/codeobject.h"

Error* pk_compile(SourceData_ src, CodeObject* out);
