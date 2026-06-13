#include "test_util.h"

static void test_protocol_methods_runtime_dispatch(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt,
        "protocol ShowDefault do\n"
        "  method show x -> str \"default:\" x\n"
        "end\n"
        "implement ShowDefault\n"
        "show 42\n",
        "\"default:42\"");
    check_value_written(&rt,
        "protocol ShowOverride do\n"
        "  method show x -> str \"default:\" x\n"
        "end\n"
        "implement ShowOverride\n"
        "extend int with ShowOverride do\n"
        "  method show x -> str \"int:\" x\n"
        "end\n"
        "show 42\n",
        "\"int:42\"");
    check_value_written(&rt,
        "protocol ShowDot do\n"
        "  method show x\n"
        "end\n"
        "implement ShowDot\n"
        "extend int with ShowDot do\n"
        "  method show x -> str \"dot:\" x\n"
        "end\n"
        "42.show\n",
        "\"dot:42\"");
    check_value_written(&rt,
        "protocol Eqish do\n"
        "  method same a b -> eq? a b\n"
        "end\n"
        "implement Eqish\n"
        "same 4 4\n",
        ":true");
    check_value_written(&rt,
        "protocol Pair do\n"
        "  method label x -> \"label\"\n"
        "  method describe x -> str (label x) \":\" x\n"
        "end\n"
        "implement Pair\n"
        "extend int with Pair do\n"
        "  method label _n -> \"int\"\n"
        "end\n"
        "{(describe 3) (describe \"s\")}\n",
        "{\"int:3\" \"label:s\"}");
    check_value_written(&rt,
        "implement tests/pkg/protomethod\n"
        "extend int with tests/pkg/protomethod do\n"
        "  method tag x -> :int\n"
        "end\n"
        "{(describe 7) (tag 7)}\n",
        "{\"pkg:7\" :int}");

    expect_runtime_error_contains(&rt, "<signature-only-requires-extend>",
        "protocol NeedsExtend do\n"
        "  method show x\n"
        "end\n"
        "implement NeedsExtend\n"
        "show 42\n",
        "does not extend protocol 'NeedsExtend#");
    check_value_written(&rt,
        "protocol Rebind do\n"
        "  method show x\n"
        "end\n"
        "implement Rebind\n"
        "extend int with Rebind do\n"
        "  method show x -> :first\n"
        "end\n"
        "extend int with Rebind do\n"
        "  method show x -> :second\n"
        "end\n"
        "show 1\n",
        ":second");
    expect_runtime_error_contains(&rt, "<cross-provider-extend-conflict>",
        "use tests/pkg/labelext\n"
        "implement tests/pkg/labelproto\n"
        "extend int with tests/pkg/labelproto do\n"
        "  method point-label n -> :mine\n"
        "end\n",
        "already extends protocol 'tests/pkg/labelproto'");
    expect_runtime_error_contains(&rt, "<cross-provider-extend-names-units>",
        "use tests/pkg/labelext\n"
        "implement tests/pkg/labelproto\n"
        "extend int with tests/pkg/labelproto do\n"
        "  method point-label n -> :mine\n"
        "end\n",
        "via 'tests/pkg/labelext'");
    idm_runtime_destroy(&rt);
}

static void test_protocol_method_expansion_boundaries(void) {
    expect_expand_result("<protocol-missing-required-method>",
        "protocol NeedsImpl do\n"
        "  method show x\n"
        "end\n"
        "extend int with NeedsImpl do\n"
        "end\n",
        false);
    expect_expand_result("<protocol-unknown-method-impl>",
        "protocol KnownOnly do\n"
        "  method show x -> str x\n"
        "end\n"
        "extend int with KnownOnly do\n"
        "  method nope x -> x\n"
        "end\n",
        false);
    expect_expand_result("<protocol-duplicate-method-decl>",
        "protocol Dups do\n"
        "  method show x -> x\n"
        "  method show x -> x\n"
        "end\n",
        false);
    expect_expand_result("<protocol-method-activation-scoped>",
        "protocol ScopedShow do\n"
        "  method show x -> str x\n"
        "end\n"
        "do\n"
        "  implement ScopedShow\n"
        "  show 1\n"
        "end\n"
        "show 1\n",
        false);
    expect_expand_result("<import-does-not-activate-methods>",
        "import tests/pkg/protomethod as P\n"
        "extend int with tests/pkg/protomethod do\n"
        "  method tag x -> :int\n"
        "end\n"
        "describe 1\n",
        false);
}

static void test_protocol_bytecode_roundtrip(void) {
    const char *src =
        "protocol RoundShow do\n"
        "  method show x -> str \"default:\" x\n"
        "end\n"
        "implement RoundShow\n"
        "extend int with RoundShow do\n"
        "  method show x -> str \"round:\" x\n"
        "end\n"
        "show 5\n";
    IdmRuntime rt1;
    idm_runtime_init(&rt1);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_string(&rt1, "<protocol-ishc>", src, &core, &err));
    CHECK(!err.present);
    IdmBytecodeModule m1;
    idm_bc_init(&m1);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &m1, &main_fn, &err));
    CHECK(!err.present);
    IdmBuffer dis;
    idm_buf_init(&dis);
    CHECK(idm_bc_disassemble(&dis, &m1));
    CHECK(strstr(dis.data, "DEFINE_PROTOCOL") != NULL);
    CHECK(strstr(dis.data, "EXTEND_PROTOCOL") != NULL);
    CHECK(strstr(dis.data, "CALL_METHOD") != NULL);
    idm_buf_destroy(&dis);
    IdmValue out1 = idm_nil();
    CHECK(idm_vm_run(&rt1, &m1, main_fn, &out1, &err));
    CHECK(!err.present);
    IdmBuffer written;
    idm_buf_init(&written);
    CHECK(idm_value_write(&written, out1));
    CHECK_STR(written.data, "\"round:5\"");
    idm_buf_destroy(&written);
    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_ic_serialize(&m1, &blob, &err));
    CHECK(!err.present);

    IdmRuntime rt2;
    idm_runtime_init(&rt2);
    IdmBytecodeModule m2;
    idm_bc_init(&m2);
    CHECK(idm_ic_deserialize(&rt2, (const unsigned char *)blob.data, blob.len, &m2, &err));
    CHECK(!err.present);
    IdmValue out2 = idm_nil();
    CHECK(idm_vm_run(&rt2, &m2, main_fn, &out2, &err));
    CHECK(!err.present);
    idm_buf_init(&written);
    CHECK(idm_value_write(&written, out2));
    CHECK_STR(written.data, "\"round:5\"");
    idm_buf_destroy(&written);

    idm_bc_destroy(&m2);
    idm_runtime_destroy(&rt2);
    idm_buf_destroy(&blob);
    idm_bc_destroy(&m1);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt1);
}

static void test_records_on_protocol_dispatch(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt,
        "record Point do\n"
        "  field x\n"
        "  field y\n"
        "end\n"
        "p = Point 3 4\n"
        "{(p.x) (y p) (Point? p) (Point? 12)}\n",
        "{3 4 :true :false}");
    check_value_written(&rt,
        "protocol LabelFor do\n"
        "  method label-of x\n"
        "end\n"
        "record PointLabel do\n"
        "  field x\n"
        "end\n"
        "record UserLabel do\n"
        "  field name\n"
        "end\n"
        "extend PointLabel with LabelFor do\n"
        "  method label-of p -> str \"point:\" (p.x)\n"
        "end\n"
        "extend UserLabel with LabelFor do\n"
        "  method label-of u -> str \"user:\" (u.name)\n"
        "end\n"
        "implement LabelFor\n"
        "{(label-of (PointLabel 7)) (label-of (UserLabel \"ada\"))}\n",
        "{\"point:7\" \"user:ada\"}");
    check_value_written(&rt,
        "use tests/pkg/recordbox\n"
        "implement tests/pkg/recordbox\n"
        "b = Box 42 \"answer\"\n"
        "{(b.value) (label b) (Box? b)}\n",
        "{42 \"answer\" :true}");
    expect_runtime_error_contains(&rt, "<record-unextended-generic-protocol>",
        "protocol NeedsRecordExtend do\n"
        "  method label x\n"
        "end\n"
        "record NeedsRecord do\n"
        "  field x\n"
        "end\n"
        "implement NeedsRecordExtend\n"
        "label (NeedsRecord 1)\n",
        "does not extend protocol 'NeedsRecordExtend#");
    idm_runtime_destroy(&rt);
}

static void test_record_expansion_boundaries(void) {
    expect_expand_result("<record-duplicate-field>",
        "record BadRecord do\n"
        "  field x\n"
        "  field x\n"
        "end\n",
        false);
    expect_expand_result("<record-body-invalid>",
        "record BadBody do\n"
        "  method nope x -> x\n"
        "end\n",
        false);
    expect_expand_result("<record-scope-leak>",
        "do\n"
        "  record LocalRecord do\n"
        "    field x\n"
        "  end\n"
        "  LocalRecord 1\n"
        "end\n"
        "LocalRecord 2\n",
        false);
}

static void test_record_ishc_roundtrip(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmDictEntry entries[1];
    entries[0].key = idm_atom(&rt, "x");
    entries[0].value = idm_int(42);
    IdmValue fields = idm_dict(&rt, entries, 1u, &err);
    CHECK(!err.present);
    IdmValue record = idm_record(&rt, "ConstRecord", fields, &err);
    CHECK(!err.present);

    IdmBytecodeModule m1;
    idm_bc_init(&m1);
    uint32_t main_fn = 0;
    uint32_t record_const = 0;
    CHECK(idm_bc_add_function(&m1, "main", 0, 0, 0, &main_fn));
    CHECK(idm_bc_add_const(&m1, record, &record_const));
    CHECK(idm_bc_set_function_entry(&m1, main_fn, m1.code_count));
    CHECK(idm_bc_emit_u32(&m1, IDM_OP_LOAD_CONST, record_const, NULL));
    CHECK(idm_bc_emit_op(&m1, IDM_OP_RETURN, NULL));
    IdmValue out1 = idm_nil();
    CHECK(idm_vm_run(&rt, &m1, main_fn, &out1, &err));
    CHECK(idm_value_equal(record, out1));
    CHECK(!err.present);
    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_ic_serialize(&m1, &blob, &err));
    CHECK(!err.present);

    IdmRuntime rt2;
    idm_runtime_init(&rt2);
    IdmBytecodeModule m2;
    idm_bc_init(&m2);
    CHECK(idm_ic_deserialize(&rt2, (const unsigned char *)blob.data, blob.len, &m2, &err));
    CHECK(!err.present);
    IdmValue out2 = idm_nil();
    CHECK(idm_vm_run(&rt2, &m2, main_fn, &out2, &err));
    CHECK(!err.present);
    CHECK(out2.tag == IDM_VAL_RECORD);
    CHECK_STR(idm_record_type(out2, &err), "ConstRecord");
    IdmValue field = idm_nil();
    CHECK(idm_record_field(out2, idm_atom(&rt2, "x"), &field, &err));
    CHECK(field.tag == IDM_VAL_INT && field.as.i == 42);
    CHECK(!err.present);

    idm_bc_destroy(&m2);
    idm_runtime_destroy(&rt2);
    idm_buf_destroy(&blob);
    idm_bc_destroy(&m1);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_protocol_identity_semantics(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_runtime_error_contains(&rt, "<local-protocols-do-not-share-dispatch>",
        "mk-a = fn _ig do\n"
        "  protocol P do\n"
        "    method size x\n"
        "  end\n"
        "  implement P\n"
        "  extend int with P do\n"
        "    method size n -> 111\n"
        "  end\n"
        "  size 5\n"
        "end\n"
        "mk-b = fn _ig do\n"
        "  protocol P do\n"
        "    method size x\n"
        "  end\n"
        "  implement P\n"
        "  extend string with P do\n"
        "    method size s -> 222\n"
        "  end\n"
        "  add (size \"hello\") (size 5)\n"
        "end\n"
        "add (mk-a 0) (mk-b 0)\n",
        "type 'int' does not extend protocol 'P#");
    check_value_written(&rt,
        "protocol Q do\n"
        "  method qa x\n"
        "end\n"
        "implement Q\n"
        "extend int with Q do\n"
        "  method qa x -> 7\n"
        "end\n"
        "qa 1\n",
        "7");
    expect_runtime_error_contains(&rt, "<changed-contract-is-a-distinct-protocol>",
        "protocol Q do\n"
        "  method qa x y\n"
        "end\n"
        "implement Q\n"
        "qa 1 2\n",
        "type 'int' does not extend protocol 'Q#");
    check_value_written(&rt,
        "implement tests/pkg/protomethod\n"
        "implement tests/pkg/protomethod\n"
        "extend int with tests/pkg/protomethod do\n"
        "  method tag x -> :int\n"
        "end\n"
        "describe 7\n",
        "\"pkg:7\"");
    IdmError err;
    idm_error_init(&err);
    IdmProtocolMethodSpec contract_v1 = {"m", 1u, false, idm_nil()};
    IdmProtocolMethodSpec contract_v2 = {"m", 2u, false, idm_nil()};
    CHECK(idm_protocol_define(&rt, "redef/X#1", &contract_v1, 1u, &err));
    CHECK(!err.present);
    CHECK(!idm_protocol_define(&rt, "redef/X#1", &contract_v2, 1u, &err));
    IdmBuffer reason;
    idm_buf_init(&reason);
    CHECK(idm_value_write(&reason, idm_error_reason_value(&rt, &err)));
    CHECK_STR(reason.data, "{:error {:protocol-redefinition :redef/X#1}}");
    idm_buf_destroy(&reason);
    idm_error_clear(&err);
    expect_expand_error_rt(&rt, "<shadowed-protocol-activation-collides>",
        "protocol G do\n"
        "  method gee x -> 1\n"
        "end\n"
        "implement G\n"
        "protocol G do\n"
        "  method gee x -> 2\n"
        "end\n"
        "implement G\n"
        "gee 1\n",
        "' is already active in this context; activating 'G#");
    expect_runtime_error_contains(&rt, "<make-record-rejects-reserved-hash>",
        "make-record \"x#1\" (dict)\n",
        "make-record type must not contain '#'");
    check_value_written(&rt,
        "record Plain do\n"
        "  field v\n"
        "end\n"
        "raw = make-record \"Plain\" (dict :v 9)\n"
        "{(Plain? (Plain 9)) (Plain? raw)}\n",
        "{:true :false}");
    idm_runtime_destroy(&rt);
}

static void test_dsl_protocol_scoping(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_error_rt(&rt, "<html-needs-implement>",
        "h1 \"Title\"\n",
        "unbound identifier 'h1'");
    expect_expand_error_rt(&rt, "<html-scoped-deactivation>",
        "x = do\n"
        "  implement tests/pkg/htmldsl\n"
        "  h1 \"inside\"\n"
        "end\n"
        "h1 \"outside\"\n",
        "unbound identifier 'h1'");
    check_value_written(&rt,
        "implement tests/pkg/htmldsl\n"
        "div 10 2\n",
        "5");
    idm_runtime_destroy(&rt);
}

void run_protocol_suite(void) {
    test_protocol_methods_runtime_dispatch();
    test_protocol_method_expansion_boundaries();
    test_protocol_bytecode_roundtrip();
    test_records_on_protocol_dispatch();
    test_record_expansion_boundaries();
    test_record_ishc_roundtrip();
    test_protocol_identity_semantics();
    test_dsl_protocol_scoping();
}
