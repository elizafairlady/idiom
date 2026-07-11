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

static void test_value_copy_cell_cycle(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmHeap src_heap, dst_heap;
    idm_heap_init(&src_heap);
    idm_heap_init(&dst_heap);
    idm_set_active_heap(&src_heap);
    IdmError err;
    idm_error_init(&err);

    IdmValue cell = idm_cell(&rt, idm_nil(), &err);
    IdmValue pair = idm_cons(&rt, idm_int(1), cell, &err);
    check(!err.present && idm_cell_set(cell, pair, &err), "cycle setup");
    IdmValue twice = idm_cons(&rt, cell, cell, &err);
    check(!err.present, "shared cell setup");

    IdmValue copied = idm_value_copy(&rt, &dst_heap, twice, &err);
    check(!err.present, "cycle copy terminates");
    IdmValue car = idm_car(copied, &err);
    IdmValue cdr = idm_cdr(copied, &err);
    check(!err.present && car.bits == cdr.bits, "cell identity preserved across copy");
    IdmValue inner = idm_cell_get(car, &err);
    check(!err.present && idm_cdr(inner, &err).bits == car.bits, "copied cycle closes on the copied cell");
    check(idm_value_heap(car) == &dst_heap, "copy landed in target heap");

    idm_set_active_heap(NULL);
    idm_heap_destroy(&src_heap);
    idm_heap_destroy(&dst_heap);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

int idm_unit_gc(void) {
    test_root_deletion_barrier();
    test_active_allocation_survives_cycle();
    test_bounded_vector_scan();
    test_value_copy_cell_cycle();
    return 0;
}
