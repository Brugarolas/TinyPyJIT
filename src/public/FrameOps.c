#include "tinypy/interpreter/frame.h"
#include "tinypy/interpreter/jitbridge.h"
#include "tinypy/interpreter/vm.h"

const char* py_Frame_sourceloc(py_Frame* self, int* lineno) {
    SourceLocation loc = Frame__source_location(self);
    *lineno = loc.lineno;
    return loc.src->filename->data;
}

void py_Frame_newglobals(py_Frame* frame, py_OutRef out) {
    if(!frame) {
        pk_mappingproxy__namedict(out, pk_current_vm->main);
        return;
    }
    if(frame->globals->type == tp_module) {
        pk_mappingproxy__namedict(out, frame->globals);
    } else {
        *out = *frame->globals;  // dict
    }
}

void py_Frame_newlocals(py_Frame* frame, py_OutRef out) {
    /* Module frames (and outlined module loops) have no fast locals. */
    if(!frame || frame->co->is_outline) {
        py_newdict(out);
        return;
    }
    if(frame->is_locals_special) {
        switch(frame->locals->type) {
            case tp_locals: frame = frame->locals->_ptr; break;
            case tp_dict: *out = *frame->locals; return;
            case tp_nil: py_newdict(out); return;
            default: c11__unreachable();
        }
    }
    /* Native code may hold scalar-replaced objects for this frame's locals. */
    RegJit__force_frame(pk_current_vm, frame);
    FastLocals__to_dict(frame->locals, frame->co);
    py_assign(out, py_retval());
}

py_StackRef py_Frame_function(py_Frame* self) {
    if(self->is_locals_special) return NULL;
    assert(self->p0->type == tp_function);
    return self->p0;
}
