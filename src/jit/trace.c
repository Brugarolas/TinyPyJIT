#include "pocketpy/jit/trace.h"

#include <limits.h>
#include <string.h>

static uint16_t saturating_inc_u16(uint16_t value) {
    return value == UINT16_MAX ? UINT16_MAX : (uint16_t)(value + 1);
}

static uint32_t saturating_inc_u32(uint32_t value) {
    return value == UINT32_MAX ? UINT32_MAX : value + 1;
}

void jit_code_profile_init(JitCodeProfile* profile) {
    memset(profile, 0, sizeof(*profile));
    profile->function_state = JIT_HOT_COLD;
    c11_vector__ctor(&profile->loops, sizeof(JitLoopProfile));
}

void jit_code_profile_destroy(JitCodeProfile* profile) {
    c11_vector__dtor(&profile->loops);
    memset(profile, 0, sizeof(*profile));
}

bool jit_profile_function_entry(JitCodeProfile* profile) {
    profile->calls = saturating_inc_u32(profile->calls);
    if(profile->function_state != JIT_HOT_COLD) return false;
    uint16_t before = profile->function_counter;
    profile->function_counter = saturating_inc_u16(profile->function_counter);
    if(before < PK_JIT_FUNCTION_THRESHOLD && profile->function_counter >= PK_JIT_FUNCTION_THRESHOLD) {
        profile->function_state = JIT_HOT_COMPILING;
        return true;
    }
    return false;
}

JitLoopProfile* jit_profile_find_loop(JitCodeProfile* profile, int32_t header_ip) {
    c11__foreach(JitLoopProfile, &profile->loops, loop) {
        if(loop->header_ip == header_ip) return loop;
    }
    return NULL;
}

static JitLoopProfile* get_or_add_loop(JitCodeProfile* profile, int32_t header_ip) {
    JitLoopProfile* loop = jit_profile_find_loop(profile, header_ip);
    if(loop != NULL) return loop;
    loop = c11_vector__emplace(&profile->loops);
    memset(loop, 0, sizeof(*loop));
    loop->header_ip = header_ip;
    loop->state = JIT_HOT_COLD;
    return loop;
}

bool jit_profile_loop_backedge(JitCodeProfile* profile, int32_t header_ip) {
    JitLoopProfile* loop = get_or_add_loop(profile, header_ip);
    loop->backedges = saturating_inc_u32(loop->backedges);
    if(loop->state != JIT_HOT_COLD) return false;
    uint16_t before = loop->counter;
    loop->counter = saturating_inc_u16(loop->counter);
    if(before < PK_JIT_LOOP_THRESHOLD && loop->counter >= PK_JIT_LOOP_THRESHOLD) {
        loop->state = JIT_HOT_COMPILING;
        return true;
    }
    return false;
}

void jit_branch_profile_record(JitBranchProfile* profile, bool taken) {
    if(taken)
        profile->taken = saturating_inc_u32(profile->taken);
    else
        profile->not_taken = saturating_inc_u32(profile->not_taken);
}

void jit_type_profile_record(JitTypeProfile* profile, int32_t type) {
    if(profile->first_count == 0 || profile->first_type == type) {
        profile->first_type = type;
        profile->first_count = saturating_inc_u32(profile->first_count);
        return;
    }
    if(profile->second_count == 0 || profile->second_type == type) {
        profile->second_type = type;
        profile->second_count = saturating_inc_u32(profile->second_count);
        if(profile->second_count > profile->first_count) {
            int32_t t = profile->first_type;
            uint32_t c = profile->first_count;
            profile->first_type = profile->second_type;
            profile->first_count = profile->second_count;
            profile->second_type = t;
            profile->second_count = c;
        }
        return;
    }
    profile->other_count = saturating_inc_u32(profile->other_count);
}

void jit_call_profile_record(JitCallProfile* profile, uintptr_t target) {
    if(profile->first_count == 0 || profile->first_target == target) {
        profile->first_target = target;
        profile->first_count = saturating_inc_u32(profile->first_count);
        return;
    }
    if(profile->second_count == 0 || profile->second_target == target) {
        profile->second_target = target;
        profile->second_count = saturating_inc_u32(profile->second_count);
        if(profile->second_count > profile->first_count) {
            uintptr_t t = profile->first_target;
            uint32_t c = profile->first_count;
            profile->first_target = profile->second_target;
            profile->first_count = profile->second_count;
            profile->second_target = t;
            profile->second_count = c;
        }
        return;
    }
    profile->other_count = saturating_inc_u32(profile->other_count);
}

static uint32_t confidence_16(uint32_t winner, uint32_t total) {
    if(total == 0) return 0;
    uint64_t scaled = ((uint64_t)winner << 16) / total;
    return scaled > UINT32_MAX ? UINT32_MAX : (uint32_t)scaled;
}

uint32_t jit_branch_probability_16(const JitBranchProfile* profile) {
    uint64_t total64 = (uint64_t)profile->taken + profile->not_taken;
    uint32_t total = total64 > UINT32_MAX ? UINT32_MAX : (uint32_t)total64;
    return confidence_16(profile->taken, total);
}

int32_t jit_type_profile_dominant(const JitTypeProfile* profile, uint32_t* confidence) {
    uint64_t total64 = (uint64_t)profile->first_count + profile->second_count + profile->other_count;
    uint32_t total = total64 > UINT32_MAX ? UINT32_MAX : (uint32_t)total64;
    if(confidence != NULL) *confidence = confidence_16(profile->first_count, total);
    return profile->first_count == 0 ? -1 : profile->first_type;
}

uintptr_t jit_call_profile_dominant(const JitCallProfile* profile, uint32_t* confidence) {
    uint64_t total64 = (uint64_t)profile->first_count + profile->second_count + profile->other_count;
    uint32_t total = total64 > UINT32_MAX ? UINT32_MAX : (uint32_t)total64;
    if(confidence != NULL) *confidence = confidence_16(profile->first_count, total);
    return profile->first_count == 0 ? (uintptr_t)0 : profile->first_target;
}
