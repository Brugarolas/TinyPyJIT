#include "pocketpy/interpreter/regprofile.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void append_runtime_site(RegCode* code, int32_t source_ip) {
    RegRuntimeSite site;
    memset(&site, 0, sizeof(site));
    site.cache_index = REG_IC_NONE;
    site.feedback_index = REG_PROFILE_NONE;
    site.loop_index = REG_PROFILE_NONE;
    c11_vector__push(RegRuntimeSite, &code->runtime_sites, site);
    c11_vector__push(int32_t, &code->source_ip, source_ip);
    c11_vector__push(RegBytecode, &code->code, RBC_ABC(RBC_NOP, 0, 0, 0));
}

int main(void) {
    RegCode code;
    RegCode__ctor(&code);
    append_runtime_site(&code, 10);
    append_runtime_site(&code, 11);
    append_runtime_site(&code, 12);

    for(unsigned i = 1; i < PK_JIT_FUNCTION_THRESHOLD; i++) {
        assert(!RegProfile__function_entry(&code));
    }
    assert(RegProfile__function_entry(&code));
    assert(code.function_hotness == PK_JIT_FUNCTION_THRESHOLD);

    for(int i = 0; i < 8; i++) RegProfile__record_type_pair(&code, 0, 1, 2);
    for(int i = 0; i < 2; i++) RegProfile__record_type_pair(&code, 0, 2, 2);
    RegProfile__record_type_pair(&code, 0, 3, 4);

    const RegProfileSite* site = RegProfile__site(&code, 0);
    assert(site != NULL);
    uint32_t pair = 0;
    uint32_t confidence = RegProfile__type_pair_confidence_16(&site->types, &pair);
    assert(pair == ((uint32_t)1u | ((uint32_t)2u << 16)));
    assert(confidence > 40000u);

    for(int i = 0; i < 9; i++) RegProfile__record_branch(&code, 1, true);
    RegProfile__record_branch(&code, 1, false);
    site = RegProfile__site(&code, 1);
    assert(site != NULL);
    assert(RegProfile__branch_probability_16(&site->branch) > 58000u);

    uintptr_t target_a = (uintptr_t)0x1000u;
    uintptr_t target_b = (uintptr_t)0x2000u;
    for(int i = 0; i < 7; i++) RegProfile__record_call(&code, 2, target_a);
    for(int i = 0; i < 2; i++) RegProfile__record_call(&code, 2, target_b);
    site = RegProfile__site(&code, 2);
    uintptr_t target = 0;
    confidence = RegProfile__call_confidence_16(&site->calls, &target);
    assert(target == target_a);
    assert(confidence > 45000u);

    for(unsigned i = 0; i < PK_JIT_LOOP_THRESHOLD; i++) {
        RegLoopProfile* loop = RegProfile__loop_backedge(&code, 2, 0, 10);
        assert(loop != NULL);
    }
    const RegLoopProfile* loop = RegProfile__loop(&code, 0);
    assert(loop != NULL);
    assert(loop->hotness == PK_JIT_LOOP_THRESHOLD);
    assert(loop->backedges == PK_JIT_LOOP_THRESHOLD);

    RegProfileView view;
    RegProfile__view(&code, &view);
    assert(view.code == &code);
    assert(view.site_count == 3);
    assert(view.loop_count == 1);

    RegCode__reset_profile(&code);
    assert(code.profile_sites.length == 0);
    assert(code.loop_profiles.length == 0);
    assert(code.function_hotness == 0);
    assert(RegProfile__site(&code, 0) == NULL);

    RegCode__dtor(&code);
    puts("profile smoke: ok");
    return 0;
}
