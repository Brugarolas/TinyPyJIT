#include "pocketpy/interpreter/regbytecode.h"
#include <assert.h>
#include <stdio.h>

static void init_co(CodeObject* co, int nlocals) {
    c11_vector__ctor(&co->codes, sizeof(Bytecode));
    c11_vector__ctor(&co->codes_ex, sizeof(BytecodeEx));
    c11_vector__ctor(&co->consts, sizeof(py_TValue));
    c11_vector__ctor(&co->varnames, sizeof(py_Name));
    c11_vector__ctor(&co->names, sizeof(py_Name));
    c11_vector__ctor(&co->blocks, 1);
    c11_vector__ctor(&co->func_decls, sizeof(void*));
    co->nlocals = nlocals;
}

static void free_co(CodeObject* co) {
    c11_vector__dtor(&co->codes);
    c11_vector__dtor(&co->codes_ex);
    c11_vector__dtor(&co->consts);
    c11_vector__dtor(&co->varnames);
    c11_vector__dtor(&co->names);
    c11_vector__dtor(&co->blocks);
    c11_vector__dtor(&co->func_decls);
}

static void emit(CodeObject* co, Opcode op, uint16_t arg) {
    Bytecode bc = {(uint16_t)op, arg};
    c11_vector__push(Bytecode, &co->codes, bc);
}

static void test_add_store_fuses(void) {
    CodeObject co;
    init_co(&co, 3);
    emit(&co, OP_LOAD_FAST, 0);
    emit(&co, OP_LOAD_FAST, 1);
    emit(&co, OP_BINARY_ADD, 0);
    emit(&co, OP_STORE_FAST, 2);
    emit(&co, OP_LOAD_FAST, 2);
    emit(&co, OP_RETURN_VALUE, BC_NOARG);

    FuncDecl decl;
    memset(&decl, 0, sizeof(decl));
    decl.code = co;
    c11_vector__ctor(&decl.args, sizeof(int32_t));
    c11_vector__ctor(&decl.kwargs, sizeof(FuncDeclKwArg));
    decl.starred_arg = -1;
    decl.starred_kwarg = -1;
    int32_t a0 = 0, a1 = 1;
    c11_vector__push(int32_t, &decl.args, a0);
    c11_vector__push(int32_t, &decl.args, a1);

    RegCode rc;
    RegCode__ctor(&rc);
    RegLowerError error;
    assert(RegCode__lower_function(&rc, &decl, &error));
    assert(rc.code.length == 2);

    RegBytecode add = c11__getitem(RegBytecode, &rc.code, 0);
    assert(RBC_OP(add) == RBC_ADD);
    assert(RBC_A(add) == 2);
    assert(RBC_B(add) == 0);
    assert(RBC_C(add) == 1);

    RegBytecode ret = c11__getitem(RegBytecode, &rc.code, 1);
    assert(RBC_OP(ret) == RBC_RETURN);
    assert(RBC_A(ret) == 2);
    assert(rc.nregs >= 3);

    RegCode__dtor(&rc);
    c11_vector__dtor(&decl.args);
    c11_vector__dtor(&decl.kwargs);
    free_co(&co);
}

static void test_loop(void) {
    CodeObject co;
    init_co(&co, 2);

    /* i = 0 */
    emit(&co, OP_LOAD_SMALL_INT, 0);       /* 0 */
    emit(&co, OP_STORE_FAST, 1);           /* 1 */
    /* header */
    emit(&co, OP_LOAD_FAST, 1);            /* 2 */
    emit(&co, OP_LOAD_FAST, 0);            /* 3 */
    emit(&co, OP_COMPARE_LT, 0);           /* 4 */
    emit(&co, OP_POP_JUMP_IF_FALSE, 6);    /* 5 -> 11 */
    emit(&co, OP_LOAD_FAST, 1);            /* 6 */
    emit(&co, OP_LOAD_SMALL_INT, 1);       /* 7 */
    emit(&co, OP_BINARY_ADD, 0);           /* 8 */
    emit(&co, OP_STORE_FAST, 1);           /* 9 */
    emit(&co, OP_LOOP_CONTINUE, (uint16_t)(int16_t)-8); /* 10 -> 2 */
    emit(&co, OP_LOAD_FAST, 1);            /* 11 */
    emit(&co, OP_RETURN_VALUE, BC_NOARG);  /* 12 */

    RegCode rc;
    RegCode__ctor(&rc);
    RegLowerError error;
    assert(RegCode__lower(&rc, &co, &error));
    assert(rc.code.length > 0);

    bool saw_branch = false;
    bool saw_backedge = false;
    for(int i = 0; i < rc.code.length; i++) {
        RegBytecode inst = c11__getitem(RegBytecode, &rc.code, i);
        if(RBC_OP(inst) == RBC_JUMP_IF_FALSE || RBC_OP(inst) == RBC_CMP_BRANCH || RBC_OP(inst) == RBC_CMP_I8_BRANCH) saw_branch = true;
        if(RBC_OP(inst) == RBC_JUMP && RBC_SBX(inst) < 0) saw_backedge = true;
    }
    assert(saw_branch);
    assert(saw_backedge);

    RegCode__dtor(&rc);
    free_co(&co);
}

static void test_unsupported_falls_back(void) {
    CodeObject co;
    init_co(&co, 0);
    emit(&co, OP_BEGIN_TRY, 0);
    emit(&co, OP_RETURN_VALUE, 1);

    RegCode rc;
    RegCode__ctor(&rc);
    RegLowerError error;
    assert(!RegCode__lower(&rc, &co, &error));
    assert(error.status == REG_LOWER_UNSUPPORTED);
    assert(rc.code.length == 0);

    RegCode__dtor(&rc);
    free_co(&co);
}


static void test_local_alias_snapshot(void) {
    CodeObject co;
    init_co(&co, 1);
    emit(&co, OP_LOAD_FAST, 0);            /* keep old a on virtual stack */
    emit(&co, OP_LOAD_SMALL_INT, 7);
    emit(&co, OP_STORE_FAST, 0);           /* overwrite a */
    emit(&co, OP_RETURN_VALUE, BC_NOARG);  /* must return old a */

    FuncDecl decl;
    memset(&decl, 0, sizeof(decl));
    decl.code = co;
    c11_vector__ctor(&decl.args, sizeof(int32_t));
    c11_vector__ctor(&decl.kwargs, sizeof(FuncDeclKwArg));
    decl.starred_arg = -1;
    decl.starred_kwarg = -1;
    int32_t a0 = 0;
    c11_vector__push(int32_t, &decl.args, a0);

    RegCode rc;
    RegCode__ctor(&rc);
    RegLowerError error;
    assert(RegCode__lower_function(&rc, &decl, &error));

    bool saw_snapshot = false;
    bool return_uses_nonlocal0 = false;
    for(int i = 0; i < rc.code.length; i++) {
        RegBytecode inst = c11__getitem(RegBytecode, &rc.code, i);
        if(RBC_OP(inst) == RBC_MOVE && RBC_B(inst) == 0 && RBC_A(inst) != 0) saw_snapshot = true;
        if(RBC_OP(inst) == RBC_RETURN && RBC_A(inst) != 0) return_uses_nonlocal0 = true;
    }
    assert(saw_snapshot);
    assert(return_uses_nonlocal0);

    RegCode__dtor(&rc);
    c11_vector__dtor(&decl.args);
    c11_vector__dtor(&decl.kwargs);
    free_co(&co);
}

static void test_unbound_check(void) {
    CodeObject co;
    init_co(&co, 1);
    emit(&co, OP_LOAD_FAST, 0);
    emit(&co, OP_RETURN_VALUE, BC_NOARG);

    RegCode rc;
    RegCode__ctor(&rc);
    RegLowerError error;
    assert(RegCode__lower(&rc, &co, &error));
    assert(rc.code.length == 2);
    assert(RBC_OP(c11__getitem(RegBytecode, &rc.code, 0)) == RBC_CHECK_BOUND);
    assert(RBC_OP(c11__getitem(RegBytecode, &rc.code, 1)) == RBC_RETURN);
    RegCode__dtor(&rc);
    free_co(&co);
}


static void test_immediate_superinstruction(void) {
    CodeObject co;
    init_co(&co, 1);
    emit(&co, OP_LOAD_FAST, 0);
    emit(&co, OP_LOAD_SMALL_INT, 1);
    emit(&co, OP_BINARY_ADD, 0);
    emit(&co, OP_RETURN_VALUE, BC_NOARG);

    FuncDecl decl;
    memset(&decl, 0, sizeof(decl));
    decl.code = co;
    c11_vector__ctor(&decl.args, sizeof(int32_t));
    c11_vector__ctor(&decl.kwargs, sizeof(FuncDeclKwArg));
    decl.starred_arg = -1;
    decl.starred_kwarg = -1;
    int32_t arg0 = 0;
    c11_vector__push(int32_t, &decl.args, arg0);

    RegCode rc;
    RegCode__ctor(&rc);
    RegLowerError error;
    assert(RegCode__lower_function(&rc, &decl, &error));
    assert(rc.code.length == 3);
    assert(RBC_OP(c11__getitem(RegBytecode, &rc.code, 0)) == RBC_NOP);
    RegBytecode add = c11__getitem(RegBytecode, &rc.code, 1);
    assert(RBC_OP(add) == RBC_ADD_I8);
    assert(RBC_B(add) == 0);
    assert(RBC_I8(add) == 1);
    assert(rc.superinstruction_count >= 1);
    assert(rc.runtime_sites.length == rc.code.length);

    RegCode__dtor(&rc);
    c11_vector__dtor(&decl.args);
    c11_vector__dtor(&decl.kwargs);
    free_co(&co);
}

static void test_immediate_fold_rejects_duplicated_temp(void) {
    CodeObject co;
    init_co(&co, 0);
    emit(&co, OP_LOAD_SMALL_INT, 3);
    emit(&co, OP_DUP_TOP, 0);
    emit(&co, OP_BINARY_ADD, 0);
    emit(&co, OP_RETURN_VALUE, BC_NOARG);

    RegCode rc;
    RegCode__ctor(&rc);
    RegLowerError error;
    assert(RegCode__lower(&rc, &co, &error));

    bool saw_load = false;
    bool saw_plain_add = false;
    for(int i = 0; i < rc.code.length; i++) {
        RegOpcode op = RBC_OP(c11__getitem(RegBytecode, &rc.code, i));
        if(op == RBC_LOAD_I16) saw_load = true;
        if(op == RBC_ADD) saw_plain_add = true;
        assert(op != RBC_ADD_I8);
    }
    assert(saw_load);
    assert(saw_plain_add);

    RegCode__dtor(&rc);
    free_co(&co);
}

static void test_immediate_fold_rejects_later_temp_use(void) {
    CodeObject co;
    init_co(&co, 1);
    emit(&co, OP_LOAD_SMALL_INT, 3);       /* [3] */
    emit(&co, OP_DUP_TOP, 0);              /* [3, 3] */
    emit(&co, OP_LOAD_FAST, 0);            /* [3, 3, a] */
    emit(&co, OP_BINARY_ADD, 0);            /* [3, 3 + a] */
    emit(&co, OP_POP_TOP, 0);               /* [3] */
    emit(&co, OP_RETURN_VALUE, BC_NOARG);   /* return 3 */

    FuncDecl decl;
    memset(&decl, 0, sizeof(decl));
    decl.code = co;
    c11_vector__ctor(&decl.args, sizeof(int32_t));
    c11_vector__ctor(&decl.kwargs, sizeof(FuncDeclKwArg));
    decl.starred_arg = -1;
    decl.starred_kwarg = -1;
    int32_t arg0 = 0;
    c11_vector__push(int32_t, &decl.args, arg0);

    RegCode rc;
    RegCode__ctor(&rc);
    RegLowerError error;
    assert(RegCode__lower_function(&rc, &decl, &error));

    bool saw_load = false;
    for(int i = 0; i < rc.code.length; i++) {
        RegOpcode op = RBC_OP(c11__getitem(RegBytecode, &rc.code, i));
        if(op == RBC_LOAD_I16) saw_load = true;
        assert(op != RBC_ADD_I8);
    }
    assert(saw_load);

    RegCode__dtor(&rc);
    c11_vector__dtor(&decl.args);
    c11_vector__dtor(&decl.kwargs);
    free_co(&co);
}

static void test_compare_branch_superinstruction(void) {
    CodeObject co;
    init_co(&co, 2);
    emit(&co, OP_LOAD_FAST, 0);            /* 0 */
    emit(&co, OP_LOAD_FAST, 1);            /* 1 */
    emit(&co, OP_COMPARE_LT, 0);           /* 2 */
    emit(&co, OP_POP_JUMP_IF_FALSE, 3);    /* 3 -> 6 */
    emit(&co, OP_LOAD_TRUE, 0);            /* 4 */
    emit(&co, OP_RETURN_VALUE, BC_NOARG);  /* 5 */
    emit(&co, OP_LOAD_FALSE, 0);           /* 6 */
    emit(&co, OP_RETURN_VALUE, BC_NOARG);  /* 7 */

    FuncDecl decl;
    memset(&decl, 0, sizeof(decl));
    decl.code = co;
    c11_vector__ctor(&decl.args, sizeof(int32_t));
    c11_vector__ctor(&decl.kwargs, sizeof(FuncDeclKwArg));
    decl.starred_arg = -1;
    decl.starred_kwarg = -1;
    int32_t a0 = 0, a1 = 1;
    c11_vector__push(int32_t, &decl.args, a0);
    c11_vector__push(int32_t, &decl.args, a1);

    RegCode rc;
    RegCode__ctor(&rc);
    RegLowerError error;
    assert(RegCode__lower_function(&rc, &decl, &error));

    bool saw_fused = false;
    for(int i = 0; i + 1 < rc.code.length; i++) {
        RegOpcode op = RBC_OP(c11__getitem(RegBytecode, &rc.code, i));
        if(op == RBC_CMP_BRANCH) {
            assert(RBC_OP(c11__getitem(RegBytecode, &rc.code, i + 1)) == RBC_SUPER_DATA);
            saw_fused = true;
            break;
        }
    }
    assert(saw_fused);
    assert(rc.runtime_sites.length == rc.code.length);

    RegCode__dtor(&rc);
    c11_vector__dtor(&decl.args);
    c11_vector__dtor(&decl.kwargs);
    free_co(&co);
}

int main(void) {
    test_add_store_fuses();
    test_loop();
    test_unsupported_falls_back();
    test_local_alias_snapshot();
    test_unbound_check();
    test_immediate_superinstruction();
    test_immediate_fold_rejects_duplicated_temp();
    test_immediate_fold_rejects_later_temp_use();
    test_compare_branch_superinstruction();
    puts("reglower smoke: ok");
    return 0;
}
