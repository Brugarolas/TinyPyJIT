#pragma once

#include <stdbool.h>
#include "tinypy/tinypy.h"
#include "tinypy/common/str.h"
#include "tinypy/common/sstream.h"
#include "tinypy/common/vector.h"

struct SourceData {
    RefCounted rc;
    enum py_CompileMode mode;
    bool is_dynamic;  // for exec() and eval()

    c11_string* filename;
    c11_string* source;

    c11_vector /*T=const char* */ line_starts;
};

typedef struct SourceData* SourceData_;

SourceData_ SourceData__rcnew(const char* source,
                                    const char* filename,
                                    enum py_CompileMode mode,
                                    bool is_dynamic);
bool SourceData__get_line(const struct SourceData* self,
                             int lineno,
                             const char** st,
                             const char** ed);
void SourceData__snapshot(const struct SourceData* self,
                             c11_sbuf* ss,
                             int lineno,
                             const char* cursor,
                             const char* name);
