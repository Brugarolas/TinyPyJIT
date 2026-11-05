#include "tinypy/interpreter/regvm_slow.h"

#include "tinypy/common/sstream.h"
#include "tinypy/interpreter/fastpath.h"
#include "tinypy/objects/error.h"
#include "tinypy/objects/exception.h"
#include "tinypy/tinypy.h"

#include <assert.h>

PK_CEVAL_COLD bool RegVM__slow_binary(RegOpcode op, py_Ref out, py_Ref lhs, py_Ref rhs) {
    bool ok;
    switch(op) {
        case RBC_ADD: ok = py_binaryadd(lhs, rhs); break;
        case RBC_SUB: ok = py_binarysub(lhs, rhs); break;
        case RBC_MUL: ok = py_binarymul(lhs, rhs); break;
        case RBC_TRUEDIV: ok = py_binarytruediv(lhs, rhs); break;
        case RBC_FLOORDIV: ok = py_binaryfloordiv(lhs, rhs); break;
        case RBC_MOD: ok = py_binarymod(lhs, rhs); break;
        case RBC_POW: ok = py_binarypow(lhs, rhs); break;
        case RBC_LSHIFT: ok = py_binarylshift(lhs, rhs); break;
        case RBC_RSHIFT: ok = py_binaryrshift(lhs, rhs); break;
        case RBC_BITAND: ok = py_binaryand(lhs, rhs); break;
        case RBC_BITOR: ok = py_binaryor(lhs, rhs); break;
        case RBC_BITXOR: ok = py_binaryxor(lhs, rhs); break;
        case RBC_MATMUL: ok = py_binarymatmul(lhs, rhs); break;
        case RBC_CMP_LT: ok = py_lt(lhs, rhs); break;
        case RBC_CMP_LE: ok = py_le(lhs, rhs); break;
        case RBC_CMP_EQ: ok = py_eq(lhs, rhs); break;
        case RBC_CMP_NE: ok = py_ne(lhs, rhs); break;
        case RBC_CMP_GT: ok = py_gt(lhs, rhs); break;
        case RBC_CMP_GE: ok = py_ge(lhs, rhs); break;
        default: c11__unreachable();
    }
    if(!ok) return false;
    *out = *py_retval();
    return true;
}

PK_CEVAL_COLD bool RegVM__slow_contains(py_Ref out, py_Ref container, py_Ref needle, bool invert) {
    py_Ref magic = py_tpfindmagic(container->type, __contains__);
    if(magic == NULL) return TypeError("'%t' type does not support '__contains__'", container->type);

    py_TValue args[2] = {*container, *needle};
    if(!py_call(magic, 2, args)) return false;
    int truth = pk_ceval_bool(py_retval());
    if(truth < 0) return false;
    py_newbool(out, invert ? !truth : truth);
    return true;
}

PK_CEVAL_COLD bool RegVM__slow_build_imag(VM* self, py_Ref out, py_Ref value) {
    py_Ref f = py_getdict(self->builtins, py_name("complex"));
    assert(f != NULL);
    py_TValue args[2];
    py_newint(&args[0], 0);
    args[1] = *value;
    if(!py_call(f, 2, args)) return false;
    *out = *py_retval();
    return true;
}

PK_CEVAL_COLD bool RegVM__slow_build_string(VM* self, py_Ref out, py_Ref begin, uint8_t count) {
    c11_sbuf ss;
    c11_sbuf__ctor(&ss);
    for(uint8_t i = 0; i < count; i++) {
        if(!py_str(begin + i)) {
            c11_sbuf__dtor(&ss);
            return false;
        }
        c11_sbuf__write_sv(&ss, py_tosv(&self->last_retval));
    }
    c11_sbuf__py_submit(&ss, out);
    return true;
}

PK_CEVAL_COLD bool RegVM__slow_unpack_sequence(py_Ref src, py_Ref dst, uint8_t expected) {
    py_TValue* p;
    int length;
    switch(src->type) {
        case tp_tuple:
            length = py_tuple_len(src);
            p = py_tuple_data(src);
            break;
        case tp_list:
            length = py_list_len(src);
            p = py_list_data(src);
            break;
        case tp_vec2i: {
            length = 2;
            if(expected != length) break;
            c11_vec2i val = py_tovec2i(src);
            py_newint(dst + 0, val.x);
            py_newint(dst + 1, val.y);
            return true;
        }
        case tp_vec2: {
            length = 2;
            if(expected != length) break;
            c11_vec2 val = py_tovec2(src);
            py_newfloat(dst + 0, val.x);
            py_newfloat(dst + 1, val.y);
            return true;
        }
        case tp_vec3i: {
            length = 3;
            if(expected != length) break;
            c11_vec3i val = py_tovec3i(src);
            py_newint(dst + 0, val.x);
            py_newint(dst + 1, val.y);
            py_newint(dst + 2, val.z);
            return true;
        }
        case tp_vec3: {
            length = 3;
            if(expected != length) break;
            c11_vec3 val = py_tovec3(src);
            py_newfloat(dst + 0, val.x);
            py_newfloat(dst + 1, val.y);
            py_newfloat(dst + 2, val.z);
            return true;
        }
        default: return TypeError("expected list or tuple to unpack, got %t", src->type);
    }

    if(length != expected) return ValueError("expected %d values to unpack, got %d", expected, length);
    for(int i = 0; i < length; i++) dst[i] = p[i];
    return true;
}

PK_CEVAL_COLD bool RegVM__slow_unpack_ex(py_Ref src, py_Ref dst, uint8_t prefix) {
    py_TValue* p;
    int length = pk_arrayview(src, &p);
    if(length == -1) return TypeError("expected list or tuple to unpack, got %t", src->type);
    int exceed = length - prefix;
    if(exceed < 0) return ValueError("not enough values to unpack");

    for(int i = 0; i < prefix; i++) dst[i] = p[i];
    py_newlistn(dst + prefix, exceed);
    for(int i = 0; i < exceed; i++) py_list_setitem(dst + prefix, i, p + prefix + i);
    return true;
}

PK_CEVAL_COLD FrameResult RegVM__slow_error(VM* self, py_Frame* frame, int32_t source_ip) {
    frame->ip = source_ip;
    assert(!py_isnil(&self->unhandled_exc));
    py_BaseException__stpush(frame,
                             &self->unhandled_exc,
                             frame->co->src,
                             Frame__lineno(frame),
                             frame->co->name->data);
    self->curr_class = NULL;
    self->curr_function = NULL;
    VM__pop_frame(self);
    return RES_ERROR;
}
