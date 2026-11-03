#pragma once
#include "pocketpy/interpreter/frame.h"
typedef struct TraceInfo { void* func; } TraceInfo; typedef struct WatchdogInfo { long max_reset_time; } WatchdogInfo; typedef struct ValueStack { py_TValue* sp; py_TValue* end; } ValueStack;
struct RegJitHooks;
typedef struct VM { py_Frame* top_frame; uint64_t type_version; py_GlobalRef builtins; py_TValue last_retval; py_TValue unhandled_exc; int recursion_depth; int max_recursion_depth; py_StackRef curr_class; py_StackRef curr_function; TraceInfo trace_info; WatchdogInfo watchdog_info; ValueStack stack; const struct RegJitHooks* regjit_hooks; void* regjit_userdata; } VM;
typedef enum FrameResult { RES_ERROR=0,RES_RETURN=1,RES_CALL=2,RES_YIELD=3 } FrameResult;
FrameResult VM__run_top_frame(VM*); void VM__push_frame(VM*,py_Frame*); FrameResult VM__vectorcall(VM*,uint16_t,uint16_t,bool); void VM__pop_frame(VM*); int pk_arrayview(py_Ref,py_TValue**); bool pk_loadmethod(py_StackRef,py_Name); bool pk_callmagic(py_Name,int,py_Ref); bool pk_format_object(VM*,py_Ref,c11_sv);
typedef struct Function { FuncDecl* decl; py_GlobalRef module; py_TValue globals; void* closure; PyObject* clazz; py_CFunction cfunc; } Function;
static inline Function* py_touserdata(py_Ref x){ return (Function*)x->_ptr; }
void Function__ctor(Function*,FuncDecl*,py_GlobalRef,py_Ref);
