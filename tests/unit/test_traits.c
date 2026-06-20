#include "test_util.h"

static void test_trait_methods_runtime_dispatch(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt,
        "trait ShowDefault do\n"
        "  method show x -> str \"default:\" x\n"
        "end\n"
        "implement ShowDefault on int\n"
        "show 42\n",
        "\"default:42\"");
    check_value_written(&rt,
        "trait ShowOverride do\n"
        "  method show x -> str \"default:\" x\n"
        "end\n"
        "implement ShowOverride on int do\n"
        "  method show x -> str \"int:\" x\n"
        "end\n"
        "show 42\n",
        "\"int:42\"");
    check_value_written(&rt,
        "trait ShowDot do\n"
        "  method show x\n"
        "end\n"
        "implement ShowDot on int do\n"
        "  method show x -> str \"dot:\" x\n"
        "end\n"
        "42.show\n",
        "\"dot:42\"");
    check_value_written(&rt,
        "trait MultiDefault do\n"
        "  method pick do\n"
        "    x -> x\n"
        "    x y -> y\n"
        "  end\n"
        "end\n"
        "implement MultiDefault on int\n"
        "{(pick 4) (pick 4 9) (4.pick) (4.pick 9)}\n",
        "{4 9 4 9}");
    check_value_written(&rt,
        "trait MultiImpl do\n"
        "  method pick do\n"
        "    x -> x\n"
        "    x y -> y\n"
        "  end\n"
        "end\n"
        "implement MultiImpl on int do\n"
        "  method pick do\n"
        "    x -> x\n"
        "    x y -> add x y\n"
        "  end\n"
        "end\n"
        "{(pick 4) (pick 4 9) (4.pick) (4.pick 9)}\n",
        "{4 13 4 13}");
    expect_expand_result("<trait-method-arity-set-mismatch>",
        "trait MultiContract do\n"
        "  method pick do\n"
        "    x -> x\n"
        "    x y -> y\n"
        "  end\n"
        "end\n"
        "implement MultiContract on int do\n"
        "  method pick x -> x\n"
        "end\n",
        false);
    check_value_written(&rt,
        "trait Eqish do\n"
        "  method same a b -> eq? a b\n"
        "end\n"
        "implement Eqish on int\n"
        "same 4 4\n",
        ":true");
    check_value_written(&rt,
        "trait Pair do\n"
        "  method label x -> \"label\"\n"
        "  method describe x -> str (label x) \":\" x\n"
        "end\n"
        "implement Pair on int do\n"
        "  method label _n -> \"int\"\n"
        "end\n"
        "implement Pair on string\n"
        "{(describe 3) (describe \"s\")}\n",
        "{\"int:3\" \"label:s\"}");
    check_value_written(&rt,
        "use tests/pkg/protomethod\n"
        "implement ProtoMethod on int do\n"
        "  method tag x -> :int\n"
        "end\n"
        "{(describe 7) (tag 7)}\n",
        "{\"pkg:7\" :int}");
    expect_runtime_error_contains(&rt, "<used-trait-method-needs-conformance>",
        "use tests/pkg/protomethod\n"
        "describe \"s\"\n",
        "does not implement trait 'ProtoMethod#");

    expect_runtime_error_contains(&rt, "<signature-only-requires-implement>",
        "trait NeedsConformance do\n"
        "  method show x -> :default\n"
        "end\n"
        "implement NeedsConformance on string\n"
        "show 42\n",
        "does not implement trait 'NeedsConformance#");
    check_value_written(&rt,
        "trait Rebind do\n"
        "  method show x\n"
        "end\n"
        "implement Rebind on int do\n"
        "  method show x -> :first\n"
        "end\n"
        "implement Rebind on int do\n"
        "  method show x -> :second\n"
        "end\n"
        "show 1\n",
        ":second");
    expect_runtime_error_contains(&rt, "<cross-provider-implement-conflict>",
        "use tests/pkg/labelext\n"
        "use tests/pkg/labelproto\n"
        "implement Label on int do\n"
        "  method point-label n -> :mine\n"
        "end\n",
        "already implements trait 'Label#");
    expect_runtime_error_contains(&rt, "<cross-provider-implement-names-units>",
        "use tests/pkg/labelext\n"
        "use tests/pkg/labelproto\n"
        "implement Label on int do\n"
        "  method point-label n -> :mine\n"
        "end\n",
        "via 'tests/pkg/labelext'");
    idm_runtime_destroy(&rt);
}

static void test_trait_method_expansion_boundaries(void) {
    expect_expand_result("<trait-missing-required-method>",
        "trait NeedsImpl do\n"
        "  method show x\n"
        "end\n"
        "implement NeedsImpl on int do\n"
        "end\n",
        false);
    expect_expand_result("<trait-unknown-method-impl>",
        "trait KnownOnly do\n"
        "  method show x -> str x\n"
        "end\n"
        "implement KnownOnly on int do\n"
        "  method nope x -> x\n"
        "end\n",
        false);
    expect_expand_result("<trait-duplicate-method-decl>",
        "trait Dups do\n"
        "  method show x -> x\n"
        "  method show x -> x\n"
        "end\n",
        false);
    expect_expand_result("<trait-declaration-scoped>",
        "do\n"
        "  trait ScopedShow do\n"
        "    method show x -> str x\n"
        "  end\n"
        "  show 1\n"
        "end\n"
        "show 1\n",
        false);
    expect_expand_result("<import-does-not-activate-methods>",
        "import tests/pkg/protomethod as P\n"
        "describe 1\n",
        false);
}

static void test_trait_requirements(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt,
        "trait BaseReq do\n"
        "  method base x\n"
        "end\n"
        "trait ChildReq do\n"
        "  require BaseReq\n"
        "  method child x -> str x.base \":child\"\n"
        "end\n"
        "implement BaseReq on int do\n"
        "  method base x -> :base\n"
        "end\n"
        "implement ChildReq on int\n"
        "5.child\n",
        "\"base:child\"");
    expect_runtime_error_contains(&rt, "<trait-requirement-before-implement>",
        "trait LowReq do\n"
        "  method low x -> :low\n"
        "end\n"
        "trait HighReq do\n"
        "  require LowReq\n"
        "  method high x -> :high\n"
        "end\n"
        "implement HighReq on int\n",
        "required trait 'LowReq#");
    check_value_written(&rt,
        "import std/enum as E\n"
        "trait NeedsImportedIter do\n"
        "  require E.Iter\n"
        "  method imported-sum xs -> xs.foldl 0 &add\n"
        "end\n"
        "implement NeedsImportedIter on pair\n"
        "imported-sum (list 1 2 3)\n",
        "6");
    check_value_written(&rt,
        "import std/enum as E\n"
        "{(implements? E.Iter (list 1)) (implements? E.Iter 1)}\n",
        "{:true :false}");
    expect_expand_result("<trait-requirement-unknown>",
        "trait MissingReq do\n"
        "  require NopeReq\n"
        "  method missing x -> x\n"
        "end\n",
        false);
    idm_runtime_destroy(&rt);
}

static void test_trait_bytecode_roundtrip(void) {
    const char *src =
        "trait RoundShow do\n"
        "  method show do\n"
        "    x -> str \"default:\" x\n"
        "    x y -> str \"default:\" (add x y)\n"
        "  end\n"
        "end\n"
        "implement RoundShow on int do\n"
        "  method show do\n"
        "    x -> str \"round:\" x\n"
        "    x y -> str \"round:\" (add x y)\n"
        "  end\n"
        "end\n"
        "{(show 5) (show 5 6)}\n";
    IdmRuntime rt1;
    idm_runtime_init(&rt1);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_string(&rt1, "<trait-ishc>", src, &core, &err));
    CHECK(!err.present);
    IdmBytecodeModule m1;
    idm_bc_init(&m1);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &m1, &main_fn, &err));
    CHECK(!err.present);
    IdmBuffer dis;
    idm_buf_init(&dis);
    CHECK(idm_bc_disassemble(&dis, &m1));
    CHECK(strstr(dis.data, "DEFINE_TRAIT") != NULL);
    CHECK(strstr(dis.data, "IMPLEMENT_TRAIT") != NULL);
    CHECK(strstr(dis.data, "CALL_METHOD") != NULL);
    idm_buf_destroy(&dis);
    IdmValue out1 = idm_nil();
    CHECK(idm_vm_run(&rt1, &m1, main_fn, &out1, &err));
    CHECK(!err.present);
    IdmBuffer written;
    idm_buf_init(&written);
    CHECK(idm_value_write(&written, out1));
    CHECK_STR(written.data, "{\"round:5\" \"round:11\"}");
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
    CHECK_STR(written.data, "{\"round:5\" \"round:11\"}");
    idm_buf_destroy(&written);

    idm_bc_destroy(&m2);
    idm_runtime_destroy(&rt2);
    idm_buf_destroy(&blob);
    idm_bc_destroy(&m1);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt1);
}

static void test_records_on_trait_dispatch(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt,
        "record Point do\n"
        "  field x\n"
        "  field y\n"
        "end\n"
        "p = Point 3 4\n"
        "{p.x (y p) (Point? p) (Point? 12)}\n",
        "{3 4 :true :false}");
    check_value_written(&rt,
        "trait LabelFor do\n"
        "  method label-of x\n"
        "end\n"
        "record PointLabel do\n"
        "  field x\n"
        "end\n"
        "record UserLabel do\n"
        "  field name\n"
        "end\n"
        "implement LabelFor on PointLabel do\n"
        "  method label-of p -> str \"point:\" p.x\n"
        "end\n"
        "implement LabelFor on UserLabel do\n"
        "  method label-of u -> str \"user:\" u.name\n"
        "end\n"
        "{(label-of (PointLabel 7)) (label-of (UserLabel \"ada\"))}\n",
        "{\"point:7\" \"user:ada\"}");
    check_value_written(&rt,
        "use tests/pkg/recordbox\n"
        "b = Box 42 \"answer\"\n"
        "{b.value (label b) (Box? b)}\n",
        "{42 \"answer\" :true}");
    expect_runtime_error_contains(&rt, "<record-unimplemented-generic-trait>",
        "trait NeedsRecordTrait do\n"
        "  method label x -> :default\n"
        "end\n"
        "record NeedsRecord do\n"
        "  field x\n"
        "end\n"
        "implement NeedsRecordTrait on int\n"
        "label (NeedsRecord 1)\n",
        "does not implement trait 'NeedsRecordTrait#");
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

static void test_trait_identity_semantics(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_runtime_error_contains(&rt, "<local-traits-do-not-share-dispatch>",
        "mk-a = fn do\n"
        "  trait P do\n"
        "    method size x\n"
        "  end\n"
        "  implement P on int do\n"
        "    method size n -> 111\n"
        "  end\n"
        "  size 5\n"
        "end\n"
        "mk-b = fn do\n"
        "  trait P do\n"
        "    method size x\n"
        "  end\n"
        "  implement P on string do\n"
        "    method size s -> 222\n"
        "  end\n"
        "  add (size \"hello\") (size 5)\n"
        "end\n"
        "add (mk-a) (mk-b)\n",
        "type 'int' does not implement trait 'P#");
    check_value_written(&rt,
        "trait Q do\n"
        "  method qa x\n"
        "end\n"
        "implement Q on int do\n"
        "  method qa x -> 7\n"
        "end\n"
        "qa 1\n",
        "7");
    expect_runtime_error_contains(&rt, "<changed-contract-is-a-distinct-trait>",
        "trait Q do\n"
        "  method qa x y -> :default\n"
        "end\n"
        "implement Q on string\n"
        "qa 1 2\n",
        "type 'int' does not implement trait 'Q#");
    check_value_written(&rt,
        "use tests/pkg/protomethod\n"
        "use tests/pkg/protomethod\n"
        "implement ProtoMethod on int do\n"
        "  method tag x -> :int\n"
        "end\n"
        "describe 7\n",
        "\"pkg:7\"");
    IdmError err;
    idm_error_init(&err);
    IdmTraitMethodSpec contract_v1 = {"m", idm_arity_exact(1u), false, idm_nil()};
    IdmTraitMethodSpec contract_v2 = {"m", idm_arity_exact(2u), false, idm_nil()};
    CHECK(idm_trait_define(&rt, "redef/X#1", NULL, 0, &contract_v1, 1u, &err));
    CHECK(!err.present);
    CHECK(!idm_trait_define(&rt, "redef/X#1", NULL, 0, &contract_v2, 1u, &err));
    IdmBuffer reason;
    idm_buf_init(&reason);
    CHECK(idm_value_write(&reason, idm_error_reason_value(&rt, &err)));
    CHECK_STR(reason.data, "{:error {:trait-redefinition :redef/X#1}}");
    idm_buf_destroy(&reason);
    idm_error_clear(&err);
    check_value_written(&rt,
        "trait G do\n"
        "  method gee x -> 1\n"
        "end\n"
        "implement G on int\n"
        "trait G do\n"
        "  method gee x -> 2\n"
        "end\n"
        "implement G on int\n"
        "gee 1\n",
        "2");
    check_value_written(&rt,
        "record Plain do\n"
        "  field v\n"
        "end\n"
        "raw = make-record \"Plain\" (dict :v 9)\n"
        "declared = Plain 9\n"
        "same = make-record (record-type declared) (dict :v 9)\n"
        "{(Plain? declared) (Plain? raw) (Plain? same) same.v}\n",
        "{:true :false :true 9}");
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
        "  use tests/pkg/htmldsl\n"
        "  activate HtmlDsl\n"
        "  h1 \"inside\"\n"
        "end\n"
        "h1 \"outside\"\n",
        "unbound identifier 'h1'");
    check_value_written(&rt,
        "use tests/pkg/htmldsl\n"
        "activate HtmlDsl\n"
        "div 10 2\n",
        "5");
    idm_runtime_destroy(&rt);
}

void run_traits_suite(void) {
    test_trait_methods_runtime_dispatch();
    test_trait_method_expansion_boundaries();
    test_trait_requirements();
    test_trait_bytecode_roundtrip();
    test_records_on_trait_dispatch();
    test_record_expansion_boundaries();
    test_record_ishc_roundtrip();
    test_trait_identity_semantics();
    test_dsl_protocol_scoping();
}
