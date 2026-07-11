#include "idiom/value.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static void fail(const char *name) {
    fprintf(stderr, "env_slots: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

#define ENV_SLOTS_MAX 4096u

static IdmRuntime g_rt;
static IdmEnv *g_env;
static atomic_bool g_stop;

static void test_package_identity(void) {
    IdmSymbol *a = idm_intern(&g_rt.intern, IDM_SYMBOL_ATOM, "a");
    IdmSymbol *same = idm_intern(&g_rt.intern, IDM_SYMBOL_ATOM, "a");
    IdmSymbol *b = idm_intern(&g_rt.intern, IDM_SYMBOL_ATOM, "b");
    check(a && same && b, "intern package identities");
    IdmEnv *first = idm_package_env_get_or_create(&g_rt, a);
    check(first != NULL, "create package env");
    check(first == idm_package_env_get_or_create(&g_rt, same), "same package identity reuses env");
    check(first != idm_package_env_get_or_create(&g_rt, b), "different package identity gets different env");
}

static void *racing_reader(void *arg) {
    uintptr_t seed = (uintptr_t)arg;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        for (uint32_t k = 0; k < 1024u; k++) {
            uint32_t id = (uint32_t)((seed = seed * 6364136223846793005u + 1442695040888963407u) >> 33) % ENV_SLOTS_MAX;
            IdmValue v = idm_env_slot_get(g_env, id);
            if (!(v.bits == idm_nil().bits || v.bits == idm_int((int64_t)id).bits)) {
                fail("reader saw a torn or foreign slot value");
            }
        }
    }
    return NULL;
}

static void test_grow_race(void) {
    IdmError err;
    idm_error_init(&err);
    g_env = idm_env_fresh(&g_rt);
    check(g_env != NULL, "fresh env");
    atomic_store(&g_stop, false);
    pthread_t readers[4];
    for (size_t i = 0; i < 4; i++) {
        check(pthread_create(&readers[i], NULL, racing_reader, (void *)(uintptr_t)(i + 1u)) == 0, "spawn reader");
    }
    for (uint32_t id = 0; id < ENV_SLOTS_MAX; id++) {
        check(idm_env_slot_ensure(g_env, id, &err) && !err.present, "ensure");
        check(idm_env_slot_set(&g_rt, g_env, id, idm_int((int64_t)id), &err) && !err.present, "set");
    }
    atomic_store(&g_stop, true);
    for (size_t i = 0; i < 4; i++) pthread_join(readers[i], NULL);
    for (uint32_t id = 0; id < ENV_SLOTS_MAX; id++) {
        check(idm_env_slot_get(g_env, id).bits == idm_int((int64_t)id).bits, "slot survives growth");
    }
    idm_error_clear(&err);
}

typedef struct {
    size_t reads;
} ReaderCount;

static void *counting_reader(void *arg) {
    ReaderCount *out = arg;
    size_t local = 0;
    uint32_t id = 0;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        for (uint32_t k = 0; k < 1024u; k++) {
            idm_env_slot_get(g_env, id);
            id = (id + 1u) % ENV_SLOTS_MAX;
        }
        local += 1024u;
    }
    out->reads = local;
    return NULL;
}

static size_t throughput(size_t nthreads) {
    atomic_store(&g_stop, false);
    pthread_t threads[4];
    ReaderCount counts[4] = {{0}, {0}, {0}, {0}};
    for (size_t i = 0; i < nthreads; i++) {
        check(pthread_create(&threads[i], NULL, counting_reader, &counts[i]) == 0, "spawn counter");
    }
    struct timespec ts = {0, 200000000L};
    nanosleep(&ts, NULL);
    atomic_store(&g_stop, true);
    size_t total = 0;
    for (size_t i = 0; i < nthreads; i++) {
        pthread_join(threads[i], NULL);
        total += counts[i].reads;
    }
    return total;
}

#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define ENV_SLOTS_SANITIZED 1
#endif
#endif
#if !defined(ENV_SLOTS_SANITIZED) && (defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__))
#define ENV_SLOTS_SANITIZED 1
#endif

static void test_reads_scale(void) {
#ifdef ENV_SLOTS_SANITIZED
    return;
#endif
    if (sysconf(_SC_NPROCESSORS_ONLN) < 6) return;
    size_t one = throughput(1u);
    size_t four = throughput(4u);
    check(one > 0 && four > 0, "throughput measured");
    check(four * 2u > one * 3u, "4-thread slot reads outpace 1 thread by 1.5x: reads take no global lock");
}

int idm_unit_env_slots(void) {
    idm_runtime_init(&g_rt);
    test_package_identity();
    test_grow_race();
    test_reads_scale();
    idm_runtime_destroy(&g_rt);
    return 0;
}
