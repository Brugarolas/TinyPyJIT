#pragma once
#include <stdint.h>
typedef struct py_Frame {
    struct py_Frame* f_back;
    void* co;
    void* p0;
    void* module;
    void* globals;
    void* locals;
    int ip;
    uint8_t storage_kind;
    unsigned char payload[40];
} py_Frame;
