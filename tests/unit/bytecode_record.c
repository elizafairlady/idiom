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

static size_t find_op(const IdmBytecodeModule *module, IdmOpcode op) {
    for (size_t ip = 0; ip < module->code_count; ) {
        if ((IdmOpcode)module->code[ip] == op) return ip;
        size_t width = 0;
        IdmError err;
        idm_error_init(&err);
        check(idm_bc_instruction_width(module, ip, &width, &err), "scan instruction width");
        idm_error_clear(&err);
        ip += width;
    }
    return SIZE_MAX;
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
    check(idm_type_con(&rt, &int_contract, "int"), "int contract term");
    IdmCore *record = typed_record_core(&rt, idm_int(7), &int_contract, &err);
    check_error(&err, "typed record core error");
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = UINT32_MAX;
    check(idm_core_compile_main(&rt, record, &module, &main_fn, &err), "compile typed record");
    idm_core_free(record);
    check_error(&err, "compile typed record error");
    check(module.type_count == 2u, "typed record type pool count");
    check(strcmp(idm_type_term_text(&module.types[0]), "test.TypedRecord") == 0, "typed record identity term");
    check(idm_type_term_equal(&module.types[1], &int_contract), "typed record contract term");

    IdmBuffer bytes;
    idm_buf_init(&bytes);
    check(idm_ic_serialize(&module, &bytes, &err), "typed record serialize");
    check_error(&err, "typed record serialize error");

    IdmRuntime rt2;
    idm_runtime_init(&rt2);
    IdmBytecodeModule roundtrip;
    check(idm_ic_deserialize(&rt2, (const unsigned char *)bytes.data, bytes.len, &roundtrip, &err), "typed record deserialize");
    check_error(&err, "typed record deserialize error");
    check(roundtrip.type_count == 2u, "typed record roundtrip type pool count");
    check(strcmp(idm_type_term_text(&roundtrip.types[0]), "test.TypedRecord") == 0, "typed record roundtrip identity text");
    check(strcmp(idm_type_term_text(&roundtrip.types[1]), idm_type_term_text(&int_contract)) == 0, "typed record roundtrip contract text");
    check(!idm_type_term_equal(&roundtrip.types[1], &int_contract), "type symbol handles differ across runtimes");

    IdmValue out = idm_nil();
    check(idm_bc_intern_literals(&rt2, &roundtrip, &err), "roundtrip finalize");
    check_error(&err, "roundtrip finalize error");
    size_t make_record = find_op(&roundtrip, IDM_OP_MAKE_RECORD);
    check(make_record != SIZE_MAX, "typed record make op");
    check(roundtrip.code_sites[make_record].record.shape != NULL, "typed record shape cached");
    check(roundtrip.code_sites[make_record].record.field_count == 1u, "typed record field count cached");
    check(roundtrip.code_sites[make_record].record.contracts && roundtrip.code_sites[make_record].record.contracts[0] == &roundtrip.types[1], "typed record contract cached");
    check(roundtrip.types[0].symbol == idm_intern_lookup(&rt2.intern, IDM_SYMBOL_ATOM, "test.TypedRecord"), "typed record identity linked");
    check(roundtrip.types[1].symbol == idm_intern_lookup(&rt2.intern, IDM_SYMBOL_ATOM, "int"), "typed record contract linked");
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

static void test_backward_jump_rejected(void) {
    IdmError err;
    idm_error_init(&err);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t const_index = 0;
    check(idm_bc_add_const(&module, idm_int(1), &const_index), "backward jump const");
    check(idm_bc_add_function(&module, "main", 0u, 1u, 0u, NULL), "backward jump fn");
    size_t load_off = 0;
    check(idm_bc_emit_op(&module, IDM_OP_LOAD_CONST, &load_off), "emit load");
    check(idm_bc_emit(&module, 0u, NULL), "emit load dst");
    check(idm_bc_emit(&module, const_index, NULL), "emit load const");
    check(idm_bc_emit_u32(&module, IDM_OP_JUMP, (uint32_t)load_off, NULL), "emit backward jump");
    check(!idm_bc_verify(&module, &err), "backward jump rejected by verifier");
    check(err.present && err.message && strstr(err.message, "is not forward") != NULL, "backward jump rejection names the rule");
    idm_error_clear(&err);

    IdmBuffer wire;
    idm_buf_init(&wire);
    check(idm_ic_serialize(&module, &wire, &err), "backward jump serialize");
    IdmRuntime rt2;
    idm_runtime_init(&rt2);
    IdmBytecodeModule loaded;
    check(!idm_ic_deserialize(&rt2, (const unsigned char *)wire.data, wire.len, &loaded, &err), "deserialize verifies and rejects the backward jump");
    idm_error_clear(&err);
    idm_runtime_destroy(&rt2);
    idm_buf_destroy(&wire);

    IdmBytecodeModule tail_module;
    idm_bc_init(&tail_module);
    check(idm_bc_add_function(&tail_module, "main", 0u, 1u, 0u, NULL), "tail fallthrough fn");
    size_t call_off = 0;
    check(idm_bc_emit_op(&tail_module, IDM_OP_CALL, &call_off), "emit tail call");
    check(idm_bc_emit(&tail_module, 0u, NULL), "tail call dst");
    check(idm_bc_emit(&tail_module, 0u, NULL), "tail call callee");
    check(idm_bc_emit(&tail_module, 0u, NULL), "tail call first arg");
    check(idm_bc_emit(&tail_module, 0u, NULL), "tail call argc");
    check(idm_bc_emit(&tail_module, 1u, NULL), "tail call tail flag");
    check(!idm_bc_verify(&tail_module, &err), "tail fallthrough past code end rejected");
    idm_error_clear(&err);
    idm_buf_init(&wire);
    check(idm_ic_serialize(&tail_module, &wire, &err), "tail fallthrough serialize");
    IdmRuntime rt3;
    idm_runtime_init(&rt3);
    IdmBytecodeModule tail_loaded;
    check(!idm_ic_deserialize(&rt3, (const unsigned char *)wire.data, wire.len, &tail_loaded, &err), "deserialize rejects the tail fallthrough");
    idm_error_clear(&err);
    idm_runtime_destroy(&rt3);
    idm_buf_destroy(&wire);
    idm_bc_destroy(&tail_module);

    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
}

static void test_invalid_opcode_rejected(void) {
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    check(idm_bc_add_function(&module, "main", 0u, 0u, 0u, NULL), "invalid opcode fn");
    check(idm_bc_emit(&module, (uint32_t)IDM_OP_COUNT, NULL), "emit invalid opcode");
    check(!idm_bc_verify(&module, &err), "invalid opcode rejected by verifier");
    check(err.present && err.message && strstr(err.message, "invalid opcode") != NULL, "invalid opcode rejection text");
    idm_error_clear(&err);
    idm_bc_destroy(&module);
}

static void test_selectors_prebuilt(void) {
    IdmError err;
    idm_error_init(&err);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = UINT32_MAX;
    uint32_t bind_fn = UINT32_MAX;
    uint32_t other_fn = UINT32_MAX;
    check(idm_bc_add_function(&module, "main", 0u, 2u, 0u, &main_fn), "prebuild main");
    check(idm_bc_add_function(&module, "bind", 1u, 0u, 0u, &bind_fn), "prebuild bind");
    check(idm_bc_add_function(&module, "other", 0u, 0u, 0u, &other_fn), "prebuild other");
    size_t match_bind_off = 0;
    check(idm_bc_emit_op(&module, IDM_OP_MATCH_BIND, &match_bind_off), "emit match bind");
    check(idm_bc_emit(&module, bind_fn, NULL), "emit match bind fn");
    check(idm_bc_emit(&module, 0u, NULL), "emit match bind scrutinee");
    check(idm_bc_emit(&module, 1u, NULL), "emit match bind dst");
    check(idm_bc_emit(&module, 0u, NULL), "emit match bind count");
    size_t multi_off = 0;
    check(idm_bc_emit_op(&module, IDM_OP_MAKE_MULTI_CLOSURE, &multi_off), "emit multi closure");
    check(idm_bc_emit(&module, 0u, NULL), "emit multi dst");
    check(idm_bc_emit(&module, 2u, NULL), "emit multi count");
    check(idm_bc_emit(&module, 0u, NULL), "emit multi captures");
    check(idm_bc_emit(&module, 0u, NULL), "emit multi first capture");
    check(idm_bc_emit(&module, bind_fn, NULL), "emit multi bind entry");
    check(idm_bc_emit(&module, other_fn, NULL), "emit multi other entry");
    size_t halt_off = 0;
    check(idm_bc_emit_op(&module, IDM_OP_HALT, &halt_off), "emit halt");
    check(idm_bc_set_function_entry(&module, bind_fn, halt_off), "bind entry");
    check(idm_bc_set_function_entry(&module, other_fn, halt_off), "other entry");
    check(idm_bc_intern_literals(&rt, &module, &err), "prebuild finalize");
    check_error(&err, "prebuild finalize error");
    check(module.code_sites && module.code_site_count == module.code_count, "one code-site table");
    check(idm_bc_function_selector_at(&module, bind_fn) != NULL, "match bind function selector prebuilt");
    check(idm_bc_selector_at(&module, match_bind_off) == NULL, "match bind has no lazy site selector");
    check(idm_bc_selector_at(&module, multi_off) != NULL, "multi closure selector prebuilt");
    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
    (void)main_fn;
}

static void test_match_bind_rejects_pattern_local_slot_drift(void) {
    IdmError err;
    idm_error_init(&err);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmBytecodeModule module;
    idm_bc_init(&module);

    IdmValue tuple_items[2] = { idm_int(11), idm_int(22) };
    IdmValue tuple = idm_tuple(&rt, tuple_items, 2u, &err);
    check_error(&err, "match bind tuple error");
    uint32_t tuple_const = UINT32_MAX;
    check(idm_bc_add_const(&module, tuple, &tuple_const), "match bind tuple const");

    uint32_t main_fn = UINT32_MAX;
    uint32_t bind_fn = UINT32_MAX;
    check(idm_bc_add_function(&module, "main", 0u, 3u, 0u, &main_fn), "match bind main");
    check(idm_bc_set_function_register_count(&module, main_fn, 4u), "match bind main regs");
    check(idm_bc_add_function(&module, "bind", 1u, 2u, 0u, &bind_fn), "match bind carrier");

    IdmPattern **patterns = calloc(1u, sizeof(*patterns));
    IdmPattern **items = calloc(2u, sizeof(*items));
    IdmPatternLocal *locals = calloc(2u, sizeof(*locals));
    check(patterns && items && locals, "match bind pattern alloc");
    items[0] = idm_pat_bind("a", idm_span_unknown(NULL));
    items[1] = idm_pat_bind("b", idm_span_unknown(NULL));
    check(items[0] && items[1], "match bind item patterns");
    patterns[0] = idm_pat_sequence(IDM_PAT_TUPLE, items, 2u, idm_span_unknown(NULL));
    check(patterns[0], "match bind tuple pattern");
    locals[0].name = idm_strdup("b");
    locals[0].slot = 1u;
    locals[1].name = idm_strdup("a");
    locals[1].slot = 0u;
    check(locals[0].name && locals[1].name, "match bind locals");
    check(idm_bc_set_function_patterns_take(&module, bind_fn, patterns, 1u), "match bind set patterns");
    check(idm_bc_set_function_pattern_locals_take(&module, bind_fn, locals, 2u), "match bind set locals");

    size_t main_entry = module.code_count;
    check(idm_bc_set_function_entry(&module, main_fn, main_entry), "match bind main entry");
    check(idm_bc_emit_op(&module, IDM_OP_LOAD_CONST, NULL), "match bind load op");
    check(idm_bc_emit(&module, 0u, NULL), "match bind load dst");
    check(idm_bc_emit(&module, tuple_const, NULL), "match bind load const");
    check(idm_bc_emit_op(&module, IDM_OP_MATCH_BIND, NULL), "match bind op");
    check(idm_bc_emit(&module, bind_fn, NULL), "match bind fn");
    check(idm_bc_emit(&module, 0u, NULL), "match bind scrutinee");
    check(idm_bc_emit(&module, 1u, NULL), "match bind first dst");
    check(idm_bc_emit(&module, 2u, NULL), "match bind count");
    check(idm_bc_emit_op(&module, IDM_OP_MAKE_VALUE_SEQUENCE, NULL), "match bind result op");
    check(idm_bc_emit(&module, 3u, NULL), "match bind result dst");
    check(idm_bc_emit(&module, (uint32_t)IDM_VALUE_SEQ_TUPLE, NULL), "match bind result kind");
    check(idm_bc_emit(&module, 1u, NULL), "match bind result first");
    check(idm_bc_emit(&module, 2u, NULL), "match bind result count");
    check(idm_bc_emit_op(&module, IDM_OP_RETURN, NULL), "match bind return op");
    check(idm_bc_emit(&module, 3u, NULL), "match bind return reg");

    size_t bind_entry = module.code_count;
    check(idm_bc_set_function_entry(&module, bind_fn, bind_entry), "match bind carrier entry");
    check(idm_bc_emit_op(&module, IDM_OP_HALT, NULL), "match bind carrier halt");
    check(!idm_bc_intern_literals(&rt, &module, &err), "match bind slot drift rejected");
    check(err.present && err.message && strstr(err.message, "MATCH_BIND pattern local slot 1 does not match binding index 0") != NULL, "match bind slot drift message");

    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
}

static void test_match_bind_guard_budget_exhaustion(void) {
    IdmError err;
    idm_error_init(&err);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmBytecodeModule module;
    idm_bc_init(&module);

    IdmValue tuple_items[1] = { idm_int(11) };
    IdmValue tuple = idm_tuple(&rt, tuple_items, 1u, &err);
    check_error(&err, "match bind guard tuple error");
    uint32_t tuple_const = UINT32_MAX;
    check(idm_bc_add_const(&module, tuple, &tuple_const), "match bind guard tuple const");

    uint32_t main_fn = UINT32_MAX;
    uint32_t bind_fn = UINT32_MAX;
    uint32_t guard_fn = UINT32_MAX;
    check(idm_bc_add_function(&module, "main", 0u, 2u, 0u, &main_fn), "match bind guard main");
    check(idm_bc_add_function(&module, "bind", 1u, 1u, 0u, &bind_fn), "match bind guard carrier");
    check(idm_bc_add_function(&module, "guard", 1u, 0u, 0u, &guard_fn), "match bind guard fn");
    check(idm_bc_set_function_register_count(&module, guard_fn, 2u), "match bind guard regs");

    IdmPattern **patterns = calloc(1u, sizeof(*patterns));
    IdmPatternLocal *locals = calloc(1u, sizeof(*locals));
    check(patterns && locals, "match bind guard pattern alloc");
    patterns[0] = idm_pat_bind("a", idm_span_unknown(NULL));
    check(patterns[0], "match bind guard pattern");
    locals[0].name = idm_strdup("a");
    locals[0].slot = 0u;
    check(locals[0].name, "match bind guard local");
    check(idm_bc_set_function_patterns_take(&module, bind_fn, patterns, 1u), "match bind guard set patterns");
    check(idm_bc_set_function_pattern_locals_take(&module, bind_fn, locals, 1u), "match bind guard set locals");
    check(idm_bc_set_function_guard(&module, bind_fn, guard_fn), "match bind guard attach");

    size_t main_entry = module.code_count;
    check(idm_bc_set_function_entry(&module, main_fn, main_entry), "match bind guard main entry");
    check(idm_bc_emit_op(&module, IDM_OP_LOAD_CONST, NULL), "match bind guard load op");
    check(idm_bc_emit(&module, 0u, NULL), "match bind guard load dst");
    check(idm_bc_emit(&module, tuple_const, NULL), "match bind guard load const");
    check(idm_bc_emit_op(&module, IDM_OP_MATCH_BIND, NULL), "match bind guard op");
    check(idm_bc_emit(&module, bind_fn, NULL), "match bind guard carrier operand");
    check(idm_bc_emit(&module, 0u, NULL), "match bind guard scrutinee");
    check(idm_bc_emit(&module, 1u, NULL), "match bind guard first dst");
    check(idm_bc_emit(&module, 1u, NULL), "match bind guard count");
    check(idm_bc_emit_op(&module, IDM_OP_RETURN, NULL), "match bind guard return op");
    check(idm_bc_emit(&module, 1u, NULL), "match bind guard return reg");

    size_t bind_entry = module.code_count;
    check(idm_bc_set_function_entry(&module, bind_fn, bind_entry), "match bind guard carrier entry");
    check(idm_bc_emit_op(&module, IDM_OP_HALT, NULL), "match bind guard carrier halt");

    size_t guard_entry = module.code_count;
    check(idm_bc_set_function_entry(&module, guard_fn, guard_entry), "match bind guard fn entry");
    check(idm_bc_emit_op(&module, IDM_OP_MAKE_CLOSURE, NULL), "match bind guard closure op");
    check(idm_bc_emit(&module, 1u, NULL), "match bind guard closure dst");
    check(idm_bc_emit(&module, guard_fn, NULL), "match bind guard closure fn");
    check(idm_bc_emit_op(&module, IDM_OP_CALL, NULL), "match bind guard call op");
    check(idm_bc_emit(&module, 0u, NULL), "match bind guard call dst");
    check(idm_bc_emit(&module, 1u, NULL), "match bind guard call callee");
    check(idm_bc_emit(&module, 0u, NULL), "match bind guard call first arg");
    check(idm_bc_emit(&module, 1u, NULL), "match bind guard call argc");
    check(idm_bc_emit(&module, 1u, NULL), "match bind guard call tail");
    check(idm_bc_emit_op(&module, IDM_OP_HALT, NULL), "match bind guard call fallthrough");

    check(idm_bc_intern_literals(&rt, &module, &err), "match bind guard finalize");
    check_error(&err, "match bind guard finalize error");
    IdmValue out = idm_nil();
    check(!idm_vm_run(&rt, &module, main_fn, &out, &err), "match bind guard run fails");
    check(err.present && err.message && strstr(err.message, "guard of 'bind' exceeded its budget") != NULL, "match bind guard budget message");

    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
}

static void test_reduction_budget_yields_before_callee_body(void) {
    IdmError err;
    idm_error_init(&err);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmBytecodeModule module;
    idm_bc_init(&module);

    uint32_t value_const = UINT32_MAX;
    check(idm_bc_add_const(&module, idm_int(42), &value_const), "reduction const");
    uint32_t main_fn = UINT32_MAX;
    uint32_t callee_fn = UINT32_MAX;
    check(idm_bc_add_function(&module, "main", 0u, 2u, 0u, &main_fn), "reduction main fn");
    check(idm_bc_set_function_register_count(&module, main_fn, 2u), "reduction main regs");
    check(idm_bc_add_function(&module, "callee", 0u, 1u, 0u, &callee_fn), "reduction callee fn");
    check(idm_bc_set_function_register_count(&module, callee_fn, 1u), "reduction callee regs");

    size_t main_entry = module.code_count;
    check(idm_bc_set_function_entry(&module, main_fn, main_entry), "reduction main entry");
    check(idm_bc_emit_op(&module, IDM_OP_MAKE_CLOSURE, NULL), "reduction closure op");
    check(idm_bc_emit(&module, 0u, NULL), "reduction closure dst");
    check(idm_bc_emit(&module, callee_fn, NULL), "reduction closure fn");
    check(idm_bc_emit_op(&module, IDM_OP_CALL, NULL), "reduction call op");
    check(idm_bc_emit(&module, 1u, NULL), "reduction call dst");
    check(idm_bc_emit(&module, 0u, NULL), "reduction call callee");
    check(idm_bc_emit(&module, 0u, NULL), "reduction call first arg");
    check(idm_bc_emit(&module, 0u, NULL), "reduction call argc");
    check(idm_bc_emit(&module, 0u, NULL), "reduction call tail");
    check(idm_bc_emit_op(&module, IDM_OP_RETURN, NULL), "reduction main return op");
    check(idm_bc_emit(&module, 1u, NULL), "reduction main return reg");

    size_t callee_entry = module.code_count;
    check(idm_bc_set_function_entry(&module, callee_fn, callee_entry), "reduction callee entry");
    check(idm_bc_emit_op(&module, IDM_OP_LOAD_CONST, NULL), "reduction load op");
    check(idm_bc_emit(&module, 0u, NULL), "reduction load dst");
    check(idm_bc_emit(&module, value_const, NULL), "reduction load const");
    check(idm_bc_emit_op(&module, IDM_OP_RETURN, NULL), "reduction callee return op");
    check(idm_bc_emit(&module, 0u, NULL), "reduction callee return reg");

    check(idm_bc_intern_literals(&rt, &module, &err), "reduction finalize");
    check_error(&err, "reduction finalize error");
    IdmExec *exec = idm_exec_create(&rt, &module, NULL, NULL, idm_vm_default_limits(), &err);
    check(exec != NULL, "reduction exec");
    check_error(&err, "reduction exec error");
    check(idm_exec_setup_function(exec, main_fn, &err), "reduction setup");
    check_error(&err, "reduction setup error");

    IdmExecStatus status = IDM_EXEC_DONE;
    IdmValue out = idm_nil();
    IdmValue reason = idm_nil();
    check(idm_exec_step(exec, 1, &status, &out, &reason, &err), "reduction first step");
    check_error(&err, "reduction first step error");
    check(status == IDM_EXEC_YIELD, "reduction first step yields");
    check(idm_exec_step(exec, 1, &status, &out, &reason, &err), "reduction second step");
    check_error(&err, "reduction second step error");
    check(status == IDM_EXEC_DONE, "reduction second step done");
    check(idm_value_tag(out) == IDM_VAL_INT && idm_int_value(out) == 42, "reduction result");

    idm_exec_destroy(exec);
    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
}

int idm_unit_bytecode_record(void) {
    IdmError err;
    idm_error_init(&err);
    test_backward_jump_rejected();
    test_invalid_opcode_rejected();
    test_selectors_prebuilt();
    test_match_bind_rejects_pattern_local_slot_drift();
    test_match_bind_guard_budget_exhaustion();
    test_reduction_budget_yields_before_callee_body();

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
    check(idm_type_con(&rt2, &record_term, "test.Record"), "record term");
    check(idm_type_con(&rt2, &builtin_record_term, "record"), "builtin record term");
    check(idm_type_con(&rt2, &wrong_record_term, "test.Other"), "wrong record term");
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
