#include "tinypy/interpreter/regprofile.h"

#include <limits.h>
#include <string.h>

static uint16_t sat_inc_u16(uint16_t value) {
    return value == UINT16_MAX ? UINT16_MAX : (uint16_t)(value + 1u);
}

static uint32_t sat_inc_u32(uint32_t value) {
    return value == UINT32_MAX ? UINT32_MAX : value + 1u;
}

static uint32_t confidence_16(uint32_t winner, uint64_t total) {
    if(total == 0) return 0;
    uint64_t scaled = ((uint64_t)winner << 16) / total;
    return scaled > UINT32_MAX ? UINT32_MAX : (uint32_t)scaled;
}

static RegRuntimeSite* runtime_site(RegCode* code, int32_t pc) {
    if(code == NULL || pc < 0 || pc >= code->runtime_sites.length) return NULL;
    return c11__at(RegRuntimeSite, &code->runtime_sites, pc);
}

static const RegRuntimeSite* runtime_site_const(const RegCode* code, int32_t pc) {
    if(code == NULL || pc < 0 || pc >= code->runtime_sites.length) return NULL;
    return c11__at(RegRuntimeSite, &code->runtime_sites, pc);
}

static RegProfileSite* get_site(RegCode* code, int32_t pc, bool create) {
    RegRuntimeSite* runtime = runtime_site(code, pc);
    if(runtime == NULL) return NULL;

    if(runtime->feedback_index != REG_PROFILE_NONE) {
        if(runtime->feedback_index >= (uint32_t)code->profile_sites.length) return NULL;
        return c11__at(RegProfileSite, &code->profile_sites, runtime->feedback_index);
    }

    if(!create || code->profile_sites.length >= (int)UINT16_MAX) return NULL;

    RegProfileSite* site = c11_vector__emplace(&code->profile_sites);
    memset(site, 0, sizeof(*site));
    site->pc = pc;
    site->source_ip = pc < code->source_ip.length
                          ? c11__getitem(int32_t, &code->source_ip, pc)
                          : -1;
    runtime->feedback_index = (uint16_t)(code->profile_sites.length - 1);
    return site;
}

static void record_u32_heavy(uint32_t value,
                             uint32_t* first,
                             uint32_t* second,
                             uint32_t* first_count,
                             uint32_t* second_count,
                             uint32_t* other_count) {
    if(*first_count == 0 || *first == value) {
        *first = value;
        *first_count = sat_inc_u32(*first_count);
        return;
    }
    if(*second_count == 0 || *second == value) {
        *second = value;
        *second_count = sat_inc_u32(*second_count);
        if(*second_count > *first_count) {
            uint32_t tmp_value = *first;
            uint32_t tmp_count = *first_count;
            *first = *second;
            *first_count = *second_count;
            *second = tmp_value;
            *second_count = tmp_count;
        }
        return;
    }
    *other_count = sat_inc_u32(*other_count);
}

static void record_ptr_heavy(uintptr_t value,
                             uintptr_t* first,
                             uintptr_t* second,
                             uint32_t* first_count,
                             uint32_t* second_count,
                             uint32_t* other_count) {
    if(*first_count == 0 || *first == value) {
        *first = value;
        *first_count = sat_inc_u32(*first_count);
        return;
    }
    if(*second_count == 0 || *second == value) {
        *second = value;
        *second_count = sat_inc_u32(*second_count);
        if(*second_count > *first_count) {
            uintptr_t tmp_value = *first;
            uint32_t tmp_count = *first_count;
            *first = *second;
            *first_count = *second_count;
            *second = tmp_value;
            *second_count = tmp_count;
        }
        return;
    }
    *other_count = sat_inc_u32(*other_count);
}

void RegProfile__reset(RegCode* code) {
    if(code == NULL) return;
    c11_vector__clear(&code->profile_sites);
    c11_vector__clear(&code->loop_profiles);
    code->function_entries = 0;
    code->function_hotness = 0;
    code->function_jit_state = (uint8_t)REG_JIT_COLD;

    c11__foreach(RegRuntimeSite, &code->runtime_sites, site) {
        site->feedback_index = REG_PROFILE_NONE;
        site->loop_index = REG_PROFILE_NONE;
    }
}

bool RegProfile__function_entry(RegCode* code) {
    if(code == NULL) return false;
    code->function_entries = sat_inc_u32(code->function_entries);
    if(code->function_jit_state != (uint8_t)REG_JIT_COLD) return false;
    code->function_hotness = sat_inc_u16(code->function_hotness);
    return code->function_hotness >= PK_JIT_FUNCTION_THRESHOLD;
}

RegLoopProfile* RegProfile__loop_backedge(RegCode* code,
                                          int32_t branch_pc,
                                          int32_t header_pc,
                                          int32_t source_ip) {
    if(code == NULL || header_pc < 0 || header_pc >= code->code.length) return NULL;

    RegRuntimeSite* branch = runtime_site(code, branch_pc);
    RegLoopProfile* loop = NULL;

    if(branch != NULL && branch->loop_index != REG_PROFILE_NONE &&
       branch->loop_index < (uint32_t)code->loop_profiles.length) {
        loop = c11__at(RegLoopProfile, &code->loop_profiles, branch->loop_index);
        if(loop->header_pc != header_pc) loop = NULL;
    }

    if(loop == NULL) {
        c11__foreach(RegLoopProfile, &code->loop_profiles, candidate) {
            if(candidate->header_pc == header_pc) {
                loop = candidate;
                break;
            }
        }
    }

    if(loop == NULL) {
        if(code->loop_profiles.length >= (int)UINT16_MAX) return NULL;
        loop = c11_vector__emplace(&code->loop_profiles);
        memset(loop, 0, sizeof(*loop));
        loop->header_pc = header_pc;
        loop->source_ip = source_ip;
        loop->state = REG_JIT_COLD;
    }

    if(branch != NULL) {
        ptrdiff_t index = loop - (RegLoopProfile*)code->loop_profiles.data;
        if(index >= 0 && index <= UINT16_MAX) branch->loop_index = (uint16_t)index;
    }

    loop->entries = sat_inc_u32(loop->entries);
    loop->backedges = sat_inc_u32(loop->backedges);
    if(loop->state == REG_JIT_COLD) loop->hotness = sat_inc_u16(loop->hotness);
    return loop;
}

void RegProfile__record_type_pair(RegCode* code, int32_t pc, py_Type lhs, py_Type rhs) {
    RegProfileSite* site = get_site(code, pc, true);
    if(site == NULL) return;
    site->executions = sat_inc_u32(site->executions);
    uint32_t pair = (uint32_t)(uint16_t)lhs | ((uint32_t)(uint16_t)rhs << 16);
    record_u32_heavy(pair,
                     &site->types.first_pair,
                     &site->types.second_pair,
                     &site->types.first_count,
                     &site->types.second_count,
                     &site->types.other_count);
}

void RegProfile__record_branch(RegCode* code, int32_t pc, bool taken) {
    RegProfileSite* site = get_site(code, pc, true);
    if(site == NULL) return;
    site->executions = sat_inc_u32(site->executions);
    if(taken)
        site->branch.taken = sat_inc_u32(site->branch.taken);
    else
        site->branch.not_taken = sat_inc_u32(site->branch.not_taken);
}

void RegProfile__record_call(RegCode* code, int32_t pc, uintptr_t target) {
    RegProfileSite* site = get_site(code, pc, true);
    if(site == NULL) return;
    site->executions = sat_inc_u32(site->executions);
    record_ptr_heavy(target,
                     &site->calls.first_target,
                     &site->calls.second_target,
                     &site->calls.first_count,
                     &site->calls.second_count,
                     &site->calls.other_count);
}

void RegProfile__record_shape(RegCode* code, int32_t pc, py_Type type, uintptr_t shape) {
    RegProfileSite* site = get_site(code, pc, true);
    if(site == NULL) return;
    site->executions = sat_inc_u32(site->executions);

    RegShapeObservation* first = &site->shapes.first;
    RegShapeObservation* second = &site->shapes.second;
    if(first->count == 0 || (first->shape == shape && first->type == type)) {
        first->shape = shape;
        first->type = type;
        first->count = sat_inc_u32(first->count);
        return;
    }
    if(second->count == 0 || (second->shape == shape && second->type == type)) {
        second->shape = shape;
        second->type = type;
        second->count = sat_inc_u32(second->count);
        if(second->count > first->count) {
            RegShapeObservation tmp = *first;
            *first = *second;
            *second = tmp;
        }
        return;
    }
    site->shapes.other_count = sat_inc_u32(site->shapes.other_count);
}

void RegProfile__record_ic_hit(RegCode* code, int32_t pc) {
    RegProfileSite* site = get_site(code, pc, true);
    if(site == NULL) return;
    site->ic_hits = sat_inc_u32(site->ic_hits);
}

void RegProfile__record_ic_miss(RegCode* code, int32_t pc) {
    RegProfileSite* site = get_site(code, pc, true);
    if(site == NULL) return;
    site->ic_misses = sat_inc_u32(site->ic_misses);
}

const RegProfileSite* RegProfile__site(const RegCode* code, int32_t pc) {
    const RegRuntimeSite* runtime = runtime_site_const(code, pc);
    if(runtime == NULL || runtime->feedback_index == REG_PROFILE_NONE ||
       runtime->feedback_index >= (uint32_t)code->profile_sites.length)
        return NULL;
    return c11__at(RegProfileSite, &code->profile_sites, runtime->feedback_index);
}

const RegLoopProfile* RegProfile__loop(const RegCode* code, int32_t header_pc) {
    if(code == NULL) return NULL;
    c11__foreach(RegLoopProfile, &code->loop_profiles, loop) {
        if(loop->header_pc == header_pc) return loop;
    }
    return NULL;
}

const RegInlineCache* RegProfile__inline_cache(const RegCode* code, int32_t pc) {
    const RegRuntimeSite* runtime = runtime_site_const(code, pc);
    if(runtime == NULL || runtime->cache_index == REG_IC_NONE ||
       runtime->cache_index >= (uint32_t)code->inline_caches.length)
        return NULL;
    return c11__at(RegInlineCache, &code->inline_caches, runtime->cache_index);
}

void RegProfile__view(const RegCode* code, RegProfileView* out) {
    memset(out, 0, sizeof(*out));
    if(code == NULL) return;
    out->code = code;
    out->sites = (const RegProfileSite*)code->profile_sites.data;
    out->site_count = code->profile_sites.length;
    out->loops = (const RegLoopProfile*)code->loop_profiles.data;
    out->loop_count = code->loop_profiles.length;
    out->function_entries = code->function_entries;
    out->function_hotness = code->function_hotness;
    out->function_state = (RegJitState)code->function_jit_state;
}

uint32_t RegProfile__branch_probability_16(const RegBranchProfile* profile) {
    uint64_t total = (uint64_t)profile->taken + profile->not_taken;
    return confidence_16(profile->taken, total);
}

uint32_t RegProfile__type_pair_confidence_16(const RegTypePairProfile* profile,
                                             uint32_t* pair_out) {
    if(pair_out != NULL) *pair_out = profile->first_pair;
    uint64_t total = (uint64_t)profile->first_count + profile->second_count + profile->other_count;
    return confidence_16(profile->first_count, total);
}

uint32_t RegProfile__call_confidence_16(const RegCallTargetProfile* profile,
                                       uintptr_t* target_out) {
    if(target_out != NULL) *target_out = profile->first_target;
    uint64_t total = (uint64_t)profile->first_count + profile->second_count + profile->other_count;
    return confidence_16(profile->first_count, total);
}

uint32_t RegProfile__shape_confidence_16(const RegShapeProfile* profile,
                                        py_Type* type_out,
                                        uintptr_t* shape_out) {
    if(type_out != NULL) *type_out = profile->first.type;
    if(shape_out != NULL) *shape_out = profile->first.shape;
    uint64_t total = (uint64_t)profile->first.count + profile->second.count + profile->other_count;
    return confidence_16(profile->first.count, total);
}
