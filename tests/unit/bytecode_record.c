#include "idiom/core.h"
#include "idiom/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *name) {
    fprintf(stderr, "bytecode_record: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void check_error(IdmError *err, const char *name) {
    if (err->present) fail(name);
}

static IdmCore *typed_record_core(IdmRuntime *rt, IdmValue value, IdmTypeTerm *contract, IdmError *err) {
    IdmSpan span = idm_span_unknown(NULL);
    IdmSymbol *type = idm_intern(&rt->intern, IDM_SYMBOL_ATOM, "test.TypedRecord");
    IdmSymbol *field = idm_intern(&rt->intern, IDM_SYMBOL_ATOM, "value");
    check(type && field, "typed record symbols");
    IdmCore *record = idm_core_record_construct(type, span);
    IdmCore *literal = idm_core_literal(value, span);
    check(record && literal, "typed record core");
    if (!idm_core_record_construct_add(record, field, contract, literal)) {
        idm_core_free(literal);
        idm_core_free(record);
        idm_error_oom(err, span);
        return NULL;
    }
    return record;
}

static void test_record_contract_type_terms(void) {
    IdmError err;
    idm_error_init(&err);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmTypeTerm int_contract;
    check(idm_type_con(&int_contract, "int"), "int contract term");
    IdmCore *record = typed_record_core(&rt, idm_int(7), &int_contract, &err);
    check_error(&err, "typed record core error");
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = UINT32_MAX;
    check(idm_core_compile_main(&rt, record, &module, &main_fn, &err), "compile typed record");
    idm_core_free(record);
    check_error(&err, "compile typed record error");
    check(module.type_count == 1u, "typed record type pool count");
    check(idm_type_term_equal(&module.types[0], &int_contract), "typed record type pool term");

    IdmBuffer bytes;
    idm_buf_init(&bytes);
    check(idm_ic_serialize(&module, &bytes, &err), "typed record serialize");
    check_error(&err, "typed record serialize error");

    IdmRuntime rt2;
    idm_runtime_init(&rt2);
    IdmBytecodeModule roundtrip;
    check(idm_ic_deserialize(&rt2, (const unsigned char *)bytes.data, bytes.len, &roundtrip, &err), "typed record deserialize");
    check_error(&err, "typed record deserialize error");
    check(roundtrip.type_count == 1u, "typed record roundtrip type pool count");
    check(idm_type_term_equal(&roundtrip.types[0], &int_contract), "typed record roundtrip type term");

    IdmValue out = idm_nil();
    check(idm_vm_run(&rt2, &roundtrip, main_fn, &out, &err), "run typed record");
    check_error(&err, "run typed record error");
    check(idm_value_tag(out) == IDM_VAL_RECORD, "typed record output");

    idm_bc_destroy(&roundtrip);
    idm_runtime_destroy(&rt2);
    idm_buf_destroy(&bytes);
    idm_bc_destroy(&module);

    IdmCore *bad_record = typed_record_core(&rt, idm_string(&rt, "bad", &err), &int_contract, &err);
    check_error(&err, "bad typed record core error");
    IdmBytecodeModule bad_module;
    idm_bc_init(&bad_module);
    uint32_t bad_main = UINT32_MAX;
    check(idm_core_compile_main(&rt, bad_record, &bad_module, &bad_main, &err), "compile bad typed record");
    idm_core_free(bad_record);
    check_error(&err, "compile bad typed record error");
    check(idm_bc_intern_literals(&rt, &bad_module, &err), "finalize bad typed record");
    check_error(&err, "finalize bad typed record error");
    out = idm_nil();
    check(!idm_vm_run(&rt, &bad_module, bad_main, &out, &err), "bad typed record rejected");
    check(err.present && err.message && strstr(err.message, "record field 'value' expects int, got string") != NULL, "bad typed record error text");
    idm_error_clear(&err);
    idm_bc_destroy(&bad_module);

    idm_type_term_destroy(&int_contract);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
}

int idm_unit_bytecode_record(void) {
    IdmError err;
    idm_error_init(&err);

    IdmRuntime rt;
    idm_runtime_init(&rt);

    IdmSymbol *type = idm_intern(&rt.intern, IDM_SYMBOL_ATOM, "test.Record");
    IdmSymbol *fields[2] = {
        idm_intern(&rt.intern, IDM_SYMBOL_ATOM, "left"),
        idm_intern(&rt.intern, IDM_SYMBOL_ATOM, "right")
    };
    check(type && fields[0] && fields[1], "intern source symbols");

    IdmRecordShape *shape = idm_record_shape_intern_symbols(&rt, type, fields, 2u, &err);
    check_error(&err, "shape error");
    check(shape != NULL, "shape");

    IdmValue values[2] = {
        idm_int(11),
        idm_string(&rt, "ok", &err)
    };
    check_error(&err, "string error");
    IdmValue record = idm_record_from_shape(&rt, shape, values, &err);
    check_error(&err, "record error");
    check(idm_value_tag(record) == IDM_VAL_RECORD, "record tag");

    IdmBytecodeModule module;
    idm_bc_init(&module);
    check(idm_bc_add_const(&module, record, NULL), "add const");
    check(idm_bc_add_function(&module, "main", 0u, 0u, 0u, NULL), "add function");
    check(idm_bc_emit_op(&module, IDM_OP_HALT, NULL), "emit halt");

    IdmBuffer bytes;
    idm_buf_init(&bytes);
    check(idm_ic_serialize(&module, &bytes, &err), "serialize");
    check_error(&err, "serialize error");

    IdmRuntime rt2;
    idm_runtime_init(&rt2);
    IdmBytecodeModule roundtrip;
    if (!idm_ic_deserialize(&rt2, (const unsigned char *)bytes.data, bytes.len, &roundtrip, &err)) {
        fprintf(stderr, "bytecode_record: deserialize: %s\n", err.message ? err.message : "<no error>");
        exit(1);
    }
    check_error(&err, "deserialize error");
    check(roundtrip.const_count == 1u, "const count");

    IdmValue got = roundtrip.constants[0];
    check(idm_value_tag(got) == IDM_VAL_RECORD, "roundtrip record tag");
    IdmSymbol *got_type = idm_record_type_symbol(got, &err);
    IdmSymbol *got_left = idm_record_field_name_symbol(got, 0u, &err);
    IdmSymbol *got_right = idm_record_field_name_symbol(got, 1u, &err);
    check_error(&err, "roundtrip metadata error");
    check(got_type && strcmp(idm_symbol_text(got_type), "test.Record") == 0, "type text");
    check(got_left && strcmp(idm_symbol_text(got_left), "left") == 0, "left text");
    check(got_right && strcmp(idm_symbol_text(got_right), "right") == 0, "right text");
    check(idm_intern_lookup(&rt2.intern, IDM_SYMBOL_ATOM, "test.Record") == got_type, "type interned");
    check(idm_intern_lookup(&rt2.intern, IDM_SYMBOL_ATOM, "left") == got_left, "left interned");
    check(idm_value_matches_type_symbol(got, got_type), "record type match");

    IdmSymbol *record_builtin = idm_intern(&rt2.intern, IDM_SYMBOL_ATOM, "record");
    check(record_builtin && idm_value_matches_type_symbol(got, record_builtin), "builtin record match");

    IdmTypeTerm record_term;
    IdmTypeTerm builtin_record_term;
    IdmTypeTerm wrong_record_term;
    check(idm_type_con(&record_term, "test.Record"), "record term");
    check(idm_type_con(&builtin_record_term, "record"), "builtin record term");
    check(idm_type_con(&wrong_record_term, "test.Other"), "wrong record term");
    check(idm_value_matches_type_term(got, &record_term), "record term match");
    check(idm_value_matches_type_term(got, &builtin_record_term), "builtin record term match");
    check(!idm_value_matches_type_term(got, &wrong_record_term), "wrong record term mismatch");
    idm_type_term_destroy(&record_term);
    idm_type_term_destroy(&builtin_record_term);
    idm_type_term_destroy(&wrong_record_term);

    IdmValue left = idm_nil();
    check(idm_record_field_project_symbols(got, got_type, got_left, 0u, &left, &err), "left project");
    check_error(&err, "left project error");
    check(idm_value_tag(left) == IDM_VAL_INT && idm_int_value(left) == 11, "left value");

    IdmValue right = idm_record_field_value(got, 1u, &err);
    check_error(&err, "right value error");
    check(idm_value_tag(right) == IDM_VAL_STRING && strcmp(idm_string_bytes(right), "ok") == 0, "right value");

    idm_bc_destroy(&roundtrip);
    idm_runtime_destroy(&rt2);
    idm_buf_destroy(&bytes);
    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
    test_record_contract_type_terms();
    return 0;
}
