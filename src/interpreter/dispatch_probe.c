#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef PK_DISPATCH_PROBE_STEPS
#define PK_DISPATCH_PROBE_STEPS 20000000u
#endif

static uint64_t ns_now(void) {
#if defined(_WIN32)
    return (uint64_t)clock() * (1000000000ull / CLOCKS_PER_SEC);
#else
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t run_switch(const uint8_t* code, uint32_t size, uint32_t steps) {
    uint64_t acc = 1;
    uint32_t pc = 0;
    for(uint32_t i = 0; i < steps; i++) {
        switch(code[pc++]) {
            case 0: acc += 3; break;
            case 1: acc ^= acc << 7; break;
            case 2: acc += acc >> 3; break;
            case 3: acc *= 5; break;
            case 4: acc ^= 0x9e3779b97f4a7c15ull; break;
            case 5: acc += 11; break;
            case 6: acc = (acc << 1) | (acc >> 63); break;
            case 7: acc ^= acc >> 13; break;
            default: abort();
        }
        if(pc == size) pc = 0;
    }
    return acc;
}

#if defined(__GNUC__) || defined(__clang__)
static uint64_t run_threaded(const uint8_t* code, uint32_t size, uint32_t steps) {
    static void* table[] = {&&op0, &&op1, &&op2, &&op3, &&op4, &&op5, &&op6, &&op7};
    uint64_t acc = 1;
    uint32_t pc = 0;
    uint32_t left = steps;
#define NEXT()                                                                                      \
    do {                                                                                            \
        if(--left == 0) goto done;                                                                  \
        if(++pc == size) pc = 0;                                                                    \
        goto *table[code[pc]];                                                                      \
    } while(0)
    goto *table[code[0]];
op0: acc += 3; NEXT();
op1: acc ^= acc << 7; NEXT();
op2: acc += acc >> 3; NEXT();
op3: acc *= 5; NEXT();
op4: acc ^= 0x9e3779b97f4a7c15ull; NEXT();
op5: acc += 11; NEXT();
op6: acc = (acc << 1) | (acc >> 63); NEXT();
op7: acc ^= acc >> 13; NEXT();
done:
#undef NEXT
    return acc;
}
#endif

int main(void) {
    enum { N = 4096 };
    uint8_t* code = malloc(N);
    if(code == NULL) return 2;
    uint32_t state = 0x12345678u;
    for(uint32_t i = 0; i < N; i++) {
        state = state * 1664525u + 1013904223u;
        code[i] = (uint8_t)((state >> 28) & 7u);
    }

    uint64_t t0 = ns_now();
    uint64_t a = run_switch(code, N, PK_DISPATCH_PROBE_STEPS);
    uint64_t t1 = ns_now();
    printf("switch:   %.3f ms checksum=%llu\n", (double)(t1 - t0) / 1e6, (unsigned long long)a);

#if defined(__GNUC__) || defined(__clang__)
    t0 = ns_now();
    uint64_t b = run_threaded(code, N, PK_DISPATCH_PROBE_STEPS);
    t1 = ns_now();
    printf("threaded: %.3f ms checksum=%llu\n", (double)(t1 - t0) / 1e6, (unsigned long long)b);
    if(a != b) {
        fprintf(stderr, "dispatch implementations disagree\n");
        free(code);
        return 3;
    }
#else
    puts("threaded: unavailable on this compiler");
#endif

    free(code);
    return 0;
}
