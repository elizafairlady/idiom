#include "idiom/bytecode.h"
#include "idiom/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *name) {
    fprintf(stderr, "bits: %s\n", name);
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

static void test_view_root_equality_and_hash(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    unsigned char lone[] = {0xA0u};
    unsigned char shifted[] = {0x14u};
    IdmValue root = idm_bits_from_bytes(&rt, lone, 3u, &err);
    IdmValue host = idm_bits_from_bytes(&rt, shifted, 6u, &err);
    IdmValue view = idm_bits_slice(&rt, host, 3u, 3u, &err);
    check(!err.present, "setup");
    check(idm_value_equal(root, view), "same bits at offset 0 and 3 are equal");
    check(!idm_value_equal(root, idm_bits_slice(&rt, host, 2u, 3u, &err)), "different bits are not equal");
    check(!idm_value_equal(root, idm_bits_slice(&rt, host, 3u, 2u, &err)), "different lengths are not equal");
    IdmDictEntry entry = {view, idm_atom(&rt, "hit")};
    IdmValue dict = idm_dict(&rt, &entry, 1u, &err);
    IdmValue found = idm_nil();
    check(!err.present && idm_dict_get(dict, root, &found), "view key found through equal root");
    check(idm_value_equal(found, idm_atom(&rt, "hit")), "dict value round-trips");
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_gc_view_keeps_root(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmHeap heap;
    idm_heap_init(&heap);
    idm_set_active_heap(&heap);
    IdmError err;
    idm_error_init(&err);
    unsigned char shifted[] = {0x14u};
    IdmValue root = idm_bits_from_bytes(&rt, shifted, 6u, &err);
    IdmValue view = idm_bits_slice(&rt, root, 3u, 3u, &err);
    check(!err.present && idm_heap_object_count(&heap) == 2u, "setup");
    root = idm_nil();
    (void)root;

    idm_heap_gc_begin(&heap);
    idm_gc_mark_value(&heap, view);
    drain(&heap);
    check(idm_heap_object_count(&heap) == 2u, "view keeps otherwise dead root alive");
    unsigned char got[1] = {0};
    check(idm_bits_read(view, 0u, 3u, got) && got[0] == 0xA0u, "view bits stay readable after gc");

    idm_heap_gc_begin(&heap);
    drain(&heap);
    check(idm_heap_object_count(&heap) == 0u, "unrooted view and root are collected");

    idm_set_active_heap(NULL);
    idm_heap_destroy(&heap);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_cross_heap_copy_flattens(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmHeap source, target;
    idm_heap_init(&source);
    idm_heap_init(&target);
    idm_set_active_heap(&source);
    IdmError err;
    idm_error_init(&err);
    unsigned char shifted[] = {0x14u};
    IdmValue root = idm_bits_from_bytes(&rt, shifted, 6u, &err);
    IdmValue view = idm_bits_slice(&rt, root, 3u, 3u, &err);
    check(!err.present && idm_heap_object_count(&source) == 2u, "setup");
    IdmValue copied = idm_value_copy(&rt, &target, view, &err);
    check(!err.present && idm_heap_object_count(&target) == 1u, "copy flattens view without dragging parent");
    idm_set_active_heap(NULL);
    idm_heap_destroy(&source);
    unsigned char got[1] = {0};
    check(idm_bits_len(copied) == 3u && idm_bits_read(copied, 0u, 3u, got) && got[0] == 0xA0u, "copied bits survive source heap teardown");
    idm_heap_destroy(&target);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void roundtrip(IdmRuntime *rt, IdmValue value, const char *name) {
    IdmError err;
    idm_error_init(&err);
    IdmBuffer buf;
    idm_buf_init(&buf);
    check(idm_value_serialize(&buf, value, &err) && !err.present, name);
    IdmByteReader r;
    idm_byte_reader_init(&r, (const unsigned char *)buf.data, buf.len);
    IdmValue back = idm_nil();
    check(idm_value_deserialize(rt, &r, &back, &err) && !err.present, name);
    check(idm_value_equal(value, back), name);
    idm_buf_destroy(&buf);
    idm_error_clear(&err);
}

static void test_serialization_roundtrip(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    unsigned char hel[] = {0x48u, 0x65u, 0x6Cu};
    IdmValue whole = idm_bits_from_bytes(&rt, hel, 24u, &err);
    roundtrip(&rt, whole, "whole-byte roundtrip");
    roundtrip(&rt, idm_bits_from_bytes(&rt, hel, 11u, &err), "partial-byte roundtrip");
    roundtrip(&rt, idm_bits_from_bytes(&rt, hel, 0u, &err), "empty roundtrip");
    roundtrip(&rt, idm_bits_slice(&rt, whole, 3u, 13u, &err), "unaligned view roundtrip flattened");
    check(!err.present, "roundtrip setup");
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_int_extracts(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    unsigned char wide[16];
    for (size_t i = 0; i < 16u; i++) wide[i] = (unsigned char)(i + 1u);
    IdmValue big = idm_bits_from_bytes(&rt, wide, 128u, &err);
    uint32_t limbs[4] = {0x0D0E0F10u, 0x090A0B0Cu, 0x05060708u, 0x01020304u};
    IdmValue expected = idm_int_from_limbs(&rt, limbs, 4u, 1, &err);
    check(!err.present, "bignum setup");
    check(idm_value_equal(idm_bits_int(&rt, big, 0u, 128u, false, false, &err), expected), "128-bit unsigned big extract");
    check(idm_value_equal(idm_bits_int(&rt, big, 0u, 128u, false, true, &err), expected), "128-bit signed extract with clear sign bit");

    unsigned char lone[] = {0xA0u};
    IdmValue three = idm_bits_from_bytes(&rt, lone, 3u, &err);
    check(idm_value_equal(idm_bits_int(&rt, three, 0u, 3u, false, false, &err), idm_int(5)), "unsigned big 101 is 5");
    check(idm_value_equal(idm_bits_int(&rt, three, 0u, 3u, false, true, &err), idm_int(-3)), "signed big 101 is -3");

    IdmValue eleven = idm_bits_of_int(&rt, idm_int(5), 11u, true, &err);
    check(idm_value_equal(idm_bits_int(&rt, eleven, 0u, 11u, true, false, &err), idm_int(5)), "little 11-bit roundtrip");
    check(idm_value_equal(idm_bits_int(&rt, eleven, 0u, 11u, false, false, &err), idm_int(40)), "little bytes read big give 40");

    IdmValue minus = idm_bits_of_int(&rt, idm_int(-1), 16u, true, &err);
    check(idm_value_equal(idm_bits_int(&rt, minus, 0u, 16u, true, true, &err), idm_int(-1)), "little signed -1 roundtrip");
    check(idm_value_equal(idm_bits_int(&rt, minus, 0u, 16u, false, false, &err), idm_int(65535)), "-1 in 16 bits is 65535 unsigned");
    check(!err.present, "extract vectors");
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void expect_write(IdmValue value, const char *expected, const char *name) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    check(idm_value_write(&buf, value), name);
    check(buf.len == strlen(expected) && memcmp(buf.data, expected, buf.len) == 0, name);
    idm_buf_destroy(&buf);
}

static void test_inspect_format(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    unsigned char hel[] = {0x48u, 0x65u, 0x6Cu};
    expect_write(idm_bits_from_bytes(&rt, hel, 24u, &err), "%<72 101 108>", "whole bytes");
    expect_write(idm_bits_of_int(&rt, idm_int(5), 3u, false, &err), "%<5:3>", "partial byte");
    expect_write(idm_bits_from_bytes(&rt, hel, 11u, &err), "%<72 3:3>", "trailing partial byte");
    expect_write(idm_bits_from_bytes(&rt, hel, 0u, &err), "%<>", "empty");
    check(!err.present, "inspect setup");
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

int idm_unit_bits(void) {
    test_view_root_equality_and_hash();
    test_gc_view_keeps_root();
    test_cross_heap_copy_flattens();
    test_serialization_roundtrip();
    test_int_extracts();
    test_inspect_format();
    return 0;
}
