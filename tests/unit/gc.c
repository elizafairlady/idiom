#include "idiom/value.h"

#include <stdio.h>
#include <stdlib.h>

static void fail(const char *name) {
    fprintf(stderr, "gc: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void drain(IdmHeap *heap) {
    for (;;) {
        int64_t budget = 1;
        if (idm_heap_gc_step(heap, &budget)) return;
    }
}

static void test_root_deletion_barrier(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmHeap heap;
    idm_heap_init(&heap);
    idm_set_active_heap(&heap);
    IdmError err;
    idm_error_init(&err);
    IdmValue root = idm_string(&rt, "live", &err);
    check(!err.present && idm_heap_object_count(&heap) == 1u, "root setup");

    idm_heap_gc_begin(&heap);
    idm_gc_write_barrier(root);
    root = idm_nil();
    drain(&heap);
    check(idm_heap_object_count(&heap) == 1u, "barrier keeps snapshot root");

    idm_heap_gc_begin(&heap);
    drain(&heap);
    check(idm_heap_object_count(&heap) == 0u, "next cycle frees unrooted value");

    (void)root;
    idm_set_active_heap(NULL);
    idm_heap_destroy(&heap);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_active_allocation_survives_cycle(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmHeap heap;
    idm_heap_init(&heap);
    idm_set_active_heap(&heap);
    IdmError err;
    idm_error_init(&err);

    idm_heap_gc_begin(&heap);
    int64_t budget = 1;
    check(!idm_heap_gc_step(&heap, &budget), "sweep started");
    IdmValue value = idm_string(&rt, "new", &err);
    check(!err.present && idm_value_tag(value) == IDM_VAL_STRING, "active allocation");
    drain(&heap);
    check(idm_heap_object_count(&heap) == 1u, "active allocation survives");

    idm_heap_gc_begin(&heap);
    drain(&heap);
    check(idm_heap_object_count(&heap) == 0u, "unrooted allocation later frees");

    (void)value;
    idm_set_active_heap(NULL);
    idm_heap_destroy(&heap);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_bounded_vector_scan(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmHeap heap;
    idm_heap_init(&heap);
    idm_set_active_heap(&heap);
    IdmError err;
    idm_error_init(&err);

    IdmValue items[8];
    for (size_t i = 0; i < 8u; i++) items[i] = idm_string(&rt, "x", &err);
    IdmValue vector = idm_vector(&rt, items, 8u, &err);
    check(!err.present && idm_heap_object_count(&heap) == 9u, "vector setup");

    idm_heap_gc_begin(&heap);
    idm_gc_mark_value(&heap, vector);
    int64_t budget = 1;
    check(!idm_heap_gc_step(&heap, &budget), "vector scan bounded");
    drain(&heap);
    check(idm_heap_object_count(&heap) == 9u, "vector children survive");

    idm_set_active_heap(NULL);
    idm_heap_destroy(&heap);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

int idm_unit_gc(void) {
    test_root_deletion_barrier();
    test_active_allocation_survives_cycle();
    test_bounded_vector_scan();
    return 0;
}
