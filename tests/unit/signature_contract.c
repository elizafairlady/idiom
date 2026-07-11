#include "idiom/artifact.h"
#include "idiom/expand.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(const char *name) {
    fprintf(stderr, "signature_contract: %s\n", name);
    exit(1);
}

static void fail_error(IdmError *err, const char *name) {
    if (err->present) idm_error_fprint(stderr, err);
    fail(name);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void check_ok(bool ok, IdmError *err, const char *name) {
    if (!ok || err->present) fail_error(err, name);
}

static void check_truncated_rejects(IdmRuntime *rt, const char *data, size_t cut) {
    IdmError err;
    idm_error_init(&err);
    IdmArtifact art;
    if (idm_artifact_deserialize(rt, (const unsigned char *)data, cut, &art, &err)) {
        idm_artifact_destroy(&art);
        fprintf(stderr, "signature_contract: truncated artifact accepted at cut %zu\n", cut);
        exit(1);
    }
    if (!err.present) {
        fprintf(stderr, "signature_contract: truncated artifact rejection reports nothing at cut %zu\n", cut);
        exit(1);
    }
    idm_error_clear(&err);
}

static size_t wire_diff_u32_at(const IdmBuffer *a, const IdmBuffer *b) {
    check(a->len == b->len, "probe wires share shape");
    size_t diff_at = 0;
    size_t diff_count = 0;
    for (size_t i = 0; i < a->len; i++) {
        if (a->data[i] != b->data[i]) {
            diff_at = i;
            diff_count++;
        }
    }
    check(diff_count == 1u && diff_at >= 3u, "probe wires differ only in the primitive id");
    return diff_at - 3u;
}

static void check_deserialize_rejects(IdmRuntime *rt, const IdmBuffer *wire, const char *needle, const char *name) {
    IdmError err;
    idm_error_init(&err);
    IdmArtifact art;
    if (idm_artifact_deserialize(rt, (const unsigned char *)wire->data, wire->len, &art, &err)) {
        idm_artifact_destroy(&art);
        fail(name);
    }
    check(err.present && err.message && strstr(err.message, needle) != NULL, name);
    idm_error_clear(&err);
}

static char *write_package_dir(const char *source, IdmError *err) {
    char template[] = "/tmp/idiom_signature_contract_XXXXXX";
    char *dir = mkdtemp(template);
    if (!dir) fail("mkdtemp");
    char *owned = idm_strdup(dir);
    if (!owned) fail("dir copy");

    IdmBuffer path;
    idm_buf_init(&path);
    if (!idm_buf_append(&path, owned) || !idm_buf_append(&path, "/pkg.id")) {
        idm_buf_destroy(&path);
        fail_error(err, "path");
    }
    FILE *f = fopen(path.data, "wb");
    if (!f) fail("open package");
    if (fwrite(source, 1u, strlen(source), f) != strlen(source)) fail("write package");
    if (fclose(f) != 0) fail("close package");
    idm_buf_destroy(&path);
    return owned;
}

static char *write_package_subdir(const char *root, const char *name, const char *source, IdmError *err) {
    IdmBuffer dir;
    idm_buf_init(&dir);
    if (!idm_buf_append(&dir, root) || !idm_buf_append(&dir, "/") || !idm_buf_append(&dir, name)) {
        idm_buf_destroy(&dir);
        fail_error(err, "subdir path");
    }
    if (mkdir(dir.data, 0700) != 0) {
        idm_buf_destroy(&dir);
        fail("mkdir package");
    }
    IdmBuffer path;
    idm_buf_init(&path);
    if (!idm_buf_append(&path, dir.data) || !idm_buf_append(&path, "/pkg.id")) {
        idm_buf_destroy(&path);
        idm_buf_destroy(&dir);
        fail_error(err, "subdir package path");
    }
    FILE *f = fopen(path.data, "wb");
    if (!f) fail("open subdir package");
    if (fwrite(source, 1u, strlen(source), f) != strlen(source)) fail("write subdir package");
    if (fclose(f) != 0) fail("close subdir package");
    idm_buf_destroy(&path);
    return idm_buf_take(&dir);
}

static void remove_package_dir(const char *dir) {
    if (!dir) return;
    IdmBuffer path;
    idm_buf_init(&path);
    if (idm_buf_append(&path, dir) && idm_buf_append(&path, "/pkg.id")) unlink(path.data);
    idm_buf_destroy(&path);
    rmdir(dir);
}

static const IdmPkgSlot *find_slot(const IdmArtifact *art, const char *name) {
    for (size_t i = 0; i < art->slot_count; i++) {
        if (strcmp(art->slots[i].name, name) == 0) return &art->slots[i];
    }
    return NULL;
}

static const IdmPkgType *find_type(const IdmArtifact *art, const char *name) {
    for (size_t i = 0; i < art->typed.entity_count; i++) {
        const IdmPkgTypedEntity *entity = &art->typed.entities[i];
        if (entity->kind == IDM_TYPED_ENTITY_TYPE && strcmp(entity->as.type.name, name) == 0) return &entity->as.type;
    }
    return NULL;
}

static const IdmPkgTrait *find_trait(const IdmArtifact *art, const char *name) {
    for (size_t i = 0; i < art->typed.entity_count; i++) {
        const IdmPkgTypedEntity *entity = &art->typed.entities[i];
        if (entity->kind == IDM_TYPED_ENTITY_TRAIT && strcmp(entity->as.trait.name, name) == 0) return &entity->as.trait;
    }
    return NULL;
}

static const IdmPkgMethodImpl *find_method_impl(const IdmArtifact *art, const char *method, const char *type) {
    for (size_t i = 0; i < art->typed.method_impl_count; i++) {
        const IdmPkgMethodImpl *impl = &art->typed.method_impls[i];
        if (strcmp(impl->method, method) == 0 && strcmp(idm_symbol_text(impl->type), type) == 0) return impl;
    }
    return NULL;
}

static bool type_con_named(const IdmTypeTerm *term, const char *name) {
    return term && term->kind == IDM_TYPE_CON && term->symbol && strcmp(idm_type_term_text(term), name) == 0;
}

static bool type_con_identity(const IdmTypeTerm *term, IdmSymbol *identity) {
    return term && term->kind == IDM_TYPE_CON && term->symbol == identity;
}

static bool type_var_named(const IdmTypeTerm *term, const char *name) {
    return term && term->kind == IDM_TYPE_VAR && term->symbol && strcmp(idm_type_term_text(term), name) == 0;
}

static bool wide_sparse_source(IdmBuffer *out, bool hole) {
    idm_buf_init(out);
    if (!idm_buf_append(out, "defn sparse do\n  -> 0\n  ")) return false;
    for (size_t i = 0; i < 64u; i++) {
        if (!idm_buf_appendf(out, "a%zu%s", i, i == 63u ? " -> 64\n" : " ")) return false;
    }
    if (!idm_buf_append(out, "end\n")) return false;
    if (hole) return idm_buf_append(out, "sparse 1\n");
    if (!idm_buf_append(out, "sparse")) return false;
    for (size_t i = 0; i < 64u; i++) if (!idm_buf_appendf(out, " %zu", i)) return false;
    return idm_buf_append_char(out, '\n');
}

int idm_unit_signature_contract(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    const char *source =
        "package signature_contract\n"
        "\n"
        "export record Box do\n"
        "  field value :: int | string\n"
        "end\n"
        "\n"
        "export type Num :: int | float\n"
        "\n"
        "export type MaybeInt :: SomeInt | NoInt\n"
        "\n"
        "export record SomeInt do\n"
        "  field value :: int\n"
        "end\n"
        "\n"
        "export record NoInt do\n"
        "end\n"
        "\n"
        "export trait Marker do\n"
        "  spec marker-value :: Marker a => a -> a\n"
        "  method marker-value x\n"
        "end\n"
        "\n"
        "export trait Adder do\n"
        "  method same a b -> eq? a b\n"
        "end\n"
        "\n"
        "implement Adder on int\n"
        "\n"
        "implement Adder on string do\n"
        "  method same a b -> :nope\n"
        "end\n"
        "\n"
        "export trait Ident do\n"
        "  method whoami -> self\n"
        "end\n"
        "\n"
        "implement Ident on int do\n"
        "  method whoami -> self\n"
        "end\n"
        "\n"
        "implement Marker on int do\n"
        "  method marker-value x -> x\n"
        "end\n"
        "\n"
        "spec id :: int -> int\n"
        "export defn id x -> x\n"
        "\n"
        "spec overt-int :: Num\n"
        "overt-int = 1\n"
        "\n"
        "spec overt-float :: Num\n"
        "overt-float = 1.5\n"
        "\n"
        "spec some :: MaybeInt\n"
        "some = SomeInt 1\n"
        "\n"
        "spec none :: MaybeInt\n"
        "none = NoInt\n"
        "\n"
        "spec marker-id :: Marker a => a -> a\n"
        "defn marker-id x -> x\n"
        "\n"
        "spec marker-int :: int\n"
        "marker-int = marker-id 1\n"
        "\n"
        "spec showish :: Showable a => a -> string\n"
        "export defn showish x -> inspect x\n"
        "\n"
        "spec sumish :: Number a => a a -> a\n"
        "export defn sumish x y -> add x y\n"
        "\n"
        "spec choose :: a b -> a\n"
        "export defn choose x y -> x\n"
        "\n"
        "spec poly-id :: a -> a\n"
        "export defn poly-id x -> x\n"
        "\n"
        "spec poly-int :: int\n"
        "poly-int = poly-id 1\n"
        "\n"
        "spec poly-string :: string\n"
        "poly-string = poly-id \"x\"\n"
        "\n"
        "spec constish :: int\n"
        "constish = 42\n"
        "\n"
        "spec applyish :: int -> int\n"
        "applyish = fn x -> x\n";
    char *dir = write_package_dir(source, &err);

    IdmBuffer wire;
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "compile package artifact");

    IdmArtifact art;
    check_ok(idm_artifact_deserialize(&rt, (const unsigned char *)(wire.data ? wire.data : ""), wire.len, &art, &err), &err, "deserialize artifact");

    const IdmPkgSlot *id = find_slot(&art, "id");
    check(id && id->has_contract, "id slot contract");
    check(id->contract.sigs[0].arg_count == 1u && id->contract.sigs[0].has_result, "id contract shape");
    check(type_con_named(&id->contract.sigs[0].args[0], "int"), "id arg int");
    check(type_con_named(&id->contract.sigs[0].result, "int"), "id result int");

    const IdmPkgSlot *showish = find_slot(&art, "showish");
    check(showish && showish->has_contract, "showish slot contract");
    check(showish->contract.quantified_count == 1u && strcmp(showish->contract.quantified[0], "a") == 0, "showish quantifier");
    check(showish->contract.context_count == 1u, "showish context");
    check(showish->contract.context[0].kind == IDM_CONSTR_CLASS, "showish class constraint");
    check(strcmp(idm_symbol_text(showish->contract.context[0].trait), "Showable") == 0, "showish trait");
    check(type_var_named(&showish->contract.context[0].lhs, "a"), "showish constraint arg");
    check(showish->contract.sigs[0].arg_count == 1u && showish->contract.sigs[0].has_result, "showish contract shape");
    check(type_var_named(&showish->contract.sigs[0].args[0], "a"), "showish arg var");
    check(type_con_named(&showish->contract.sigs[0].result, "string"), "showish result string");

    const IdmPkgSlot *sumish = find_slot(&art, "sumish");
    check(sumish && sumish->has_contract, "sumish slot contract");
    check(sumish->contract.context_count == 1u, "sumish context");
    check(idm_symbol_kind(sumish->contract.context[0].trait) == IDM_SYMBOL_IDENTITY &&
          idm_symbol_identity_hash(sumish->contract.context[0].trait), "sumish trait identity");
    check(sumish->contract.sigs[0].arg_count == 2u && sumish->contract.sigs[0].has_result, "sumish contract shape");
    check(type_var_named(&sumish->contract.sigs[0].args[0], "a"), "sumish arg a0");
    check(type_var_named(&sumish->contract.sigs[0].args[1], "a"), "sumish arg a1");
    check(type_var_named(&sumish->contract.sigs[0].result, "a"), "sumish result a");

    const IdmPkgSlot *choose = find_slot(&art, "choose");
    check(choose && choose->has_contract, "choose slot contract");
    check(choose->contract.quantified_count == 2u, "choose quantifiers");
    check(strcmp(choose->contract.quantified[0], "a") == 0, "choose quantifier a");
    check(strcmp(choose->contract.quantified[1], "b") == 0, "choose quantifier b");
    check(choose->contract.sigs[0].arg_count == 2u && choose->contract.sigs[0].has_result, "choose contract shape");
    check(type_var_named(&choose->contract.sigs[0].args[0], "a"), "choose arg a");
    check(type_var_named(&choose->contract.sigs[0].args[1], "b"), "choose arg b");
    check(type_var_named(&choose->contract.sigs[0].result, "a"), "choose result a");
    check(choose->contract.sigs[0].args[0].var_id != 0u, "choose a var id");
    check(choose->contract.sigs[0].args[1].var_id != 0u, "choose b var id");
    check(choose->contract.sigs[0].args[0].var_id != choose->contract.sigs[0].args[1].var_id, "choose distinct var ids");
    check(choose->contract.sigs[0].result.var_id == choose->contract.sigs[0].args[0].var_id, "choose result shares a");

    const IdmPkgSlot *poly_int = find_slot(&art, "poly-int");
    check(poly_int && poly_int->has_contract, "poly-int slot contract");
    check(type_con_named(&poly_int->contract.sigs[0].result, "int"), "poly-int result int");

    const IdmPkgSlot *poly_string = find_slot(&art, "poly-string");
    check(poly_string && poly_string->has_contract, "poly-string slot contract");
    check(type_con_named(&poly_string->contract.sigs[0].result, "string"), "poly-string result string");

    const IdmPkgSlot *constish = find_slot(&art, "constish");
    check(constish && constish->has_contract, "constish slot contract");
    check(constish->contract.sigs[0].arg_count == 0u && constish->contract.sigs[0].has_result, "constish contract shape");
    check(type_con_named(&constish->contract.sigs[0].result, "int"), "constish result int");

    const IdmPkgSlot *applyish = find_slot(&art, "applyish");
    check(applyish && applyish->has_contract, "applyish slot contract");
    check(applyish->contract.sigs[0].arg_count == 1u && applyish->contract.sigs[0].has_result, "applyish contract shape");
    check(idm_arity_accepts(&applyish->arity, 1u) && !idm_arity_accepts(&applyish->arity, 0u), "applyish arity from signature");
    check(type_con_named(&applyish->contract.sigs[0].args[0], "int"), "applyish arg int");
    check(type_con_named(&applyish->contract.sigs[0].result, "int"), "applyish result int");

    const IdmPkgType *box = find_type(&art, "Box");
    check(box && box->field_count == 1u, "Box type field");
    check(box->fields[0].has_contract, "Box field contract");
    check(box->fields[0].contract.kind == IDM_TYPE_UNION && box->fields[0].contract.arg_count == 2u, "Box field union");
    check(type_con_named(&box->fields[0].contract.args[0], "int"), "Box field union int");
    check(type_con_named(&box->fields[0].contract.args[1], "string"), "Box field union string");

    const IdmPkgType *num = find_type(&art, "Num");
    check(num && num->member_count == 2u, "Num overtype members");
    check(type_con_named(&num->members[0].term, "int"), "Num member int");
    check(type_con_named(&num->members[1].term, "float"), "Num member float");

    const IdmPkgSlot *overt_int = find_slot(&art, "overt-int");
    check(overt_int && overt_int->has_contract, "overt-int slot contract");
    check(type_con_identity(&overt_int->contract.sigs[0].result, num->identity), "overt-int result Num");

    const IdmPkgSlot *overt_float = find_slot(&art, "overt-float");
    check(overt_float && overt_float->has_contract, "overt-float slot contract");
    check(type_con_identity(&overt_float->contract.sigs[0].result, num->identity), "overt-float result Num");

    const IdmPkgType *maybe_int = find_type(&art, "MaybeInt");
    const IdmPkgType *some_int = find_type(&art, "SomeInt");
    const IdmPkgType *no_int = find_type(&art, "NoInt");
    check(maybe_int && some_int && no_int, "MaybeInt record member types");
    check(maybe_int->member_count == 2u, "MaybeInt overtype members");
    check(type_con_identity(&maybe_int->members[0].term, some_int->identity), "MaybeInt member SomeInt");
    check(type_con_identity(&maybe_int->members[1].term, no_int->identity), "MaybeInt member NoInt");

    const IdmPkgSlot *some = find_slot(&art, "some");
    check(some && some->has_contract, "some slot contract");
    check(type_con_identity(&some->contract.sigs[0].result, maybe_int->identity), "some result MaybeInt");

    const IdmPkgSlot *none = find_slot(&art, "none");
    check(none && none->has_contract, "none slot contract");
    check(type_con_identity(&none->contract.sigs[0].result, maybe_int->identity), "none result MaybeInt");

    const IdmPkgSlot *marker_int = find_slot(&art, "marker-int");
    check(marker_int && marker_int->has_contract, "marker-int slot contract");
    check(type_con_named(&marker_int->contract.sigs[0].result, "int"), "marker-int result int");

    const IdmPkgTrait *marker_trait = find_trait(&art, "Marker");
    check(marker_trait && marker_trait->method_count == 1u, "Marker trait artifact");
    check(marker_trait->methods[0].has_contract, "Marker method contract");
    check(marker_trait->methods[0].contract.context_count == 1u, "Marker method context");
    check(marker_trait->methods[0].contract.context[0].trait &&
          marker_trait->methods[0].contract.context[0].trait == marker_trait->identity,
          "Marker method context identity");
    check(!marker_trait->methods[0].contract.passthrough, "Marker method is not a passthrough");

    const IdmPkgTrait *adder_trait = find_trait(&art, "Adder");
    check(adder_trait && adder_trait->method_count == 1u, "Adder trait artifact");
    check(adder_trait->methods[0].has_default, "Adder default method");
    check(adder_trait->methods[0].has_contract, "Adder method contract");
    check(adder_trait->methods[0].contract.passthrough, "Adder default passthrough survives the artifact");
    check(adder_trait->methods[0].contract.primitive == (uint32_t)IDM_PRIM_EQ, "Adder default passthrough primitive is eq?");

    const IdmPkgMethodImpl *adder_impl = find_method_impl(&art, "same", "int");
    check(adder_impl != NULL, "Adder int impl artifact");
    check(adder_impl->has_contract && adder_impl->contract.passthrough, "Adder int impl passthrough survives the artifact");
    check(adder_impl->contract.primitive == (uint32_t)IDM_PRIM_EQ, "Adder int impl passthrough primitive is eq?");

    const IdmPkgMethodImpl *adder_override = find_method_impl(&art, "same", "string");
    check(adder_override != NULL, "Adder string impl artifact");
    check(adder_override->has_contract && !adder_override->contract.passthrough, "provided non-passthrough impl carries its own contract without the default's passthrough");
    check(adder_override->contract.sig_count > 0, "provided impl contract carries its signatures");

    const IdmPkgTrait *ident_trait = find_trait(&art, "Ident");
    check(ident_trait && ident_trait->method_count == 1u, "Ident trait artifact");
    check(ident_trait->methods[0].has_contract && ident_trait->methods[0].contract.passthrough, "zero-arg default carries passthrough contract");
    check(ident_trait->methods[0].contract.sig_count == 1u && ident_trait->methods[0].contract.sigs[0].arg_count == 0u, "zero-arg passthrough contract has exact signature");

    const IdmPkgSlot *box_ctor = find_slot(&art, "Box");
    check(box_ctor && box_ctor->has_contract, "Box constructor contract");
    check(box_ctor->contract.sigs[0].arg_count == 1u && box_ctor->contract.sigs[0].has_result, "Box constructor contract shape");
    check(box_ctor->contract.sigs[0].args[0].kind == IDM_TYPE_UNION && box_ctor->contract.sigs[0].args[0].arg_count == 2u, "Box constructor arg union");

    const IdmPkgSlot *box_pred = find_slot(&art, "Box?");
    check(box_pred && box_pred->has_contract, "Box predicate contract");
    check(box_pred->contract.sigs[0].arg_count == 1u && box_pred->contract.sigs[0].has_result, "Box predicate contract shape");
    check(type_con_named(&box_pred->contract.sigs[0].result, "atom"), "Box predicate result atom");

    size_t stride = wire.len > 256u ? wire.len / 256u : 1u;
    for (size_t cut = 0; cut < wire.len; cut += (cut < 128u ? 1u : stride)) check_truncated_rejects(&rt, wire.data, cut);
    check_truncated_rejects(&rt, wire.data, wire.len - 1u);

    check(idm_buf_put_u8(&wire, 0u), "extend wire");
    IdmError tail_err;
    idm_error_init(&tail_err);
    IdmArtifact tail_art;
    if (idm_artifact_deserialize(&rt, (const unsigned char *)wire.data, wire.len, &tail_art, &tail_err)) {
        idm_artifact_destroy(&tail_art);
        fail("trailing byte accepted");
    }
    check(tail_err.present && tail_err.message && strstr(tail_err.message, "trailing data") != NULL, "trailing byte rejection names the fact");
    idm_error_clear(&tail_err);

    check(idm_primitive_info((IdmPrimitive)1u) && idm_primitive_info((IdmPrimitive)2u), "probe primitives exist");
    check(!idm_primitive_info((IdmPrimitive)0xFFFFFFFFu), "probe garbage primitive absent");
    IdmPkgSlot probe_slot;
    memset(&probe_slot, 0, sizeof(probe_slot));
    probe_slot.name = (char *)"p";
    probe_slot.arity = idm_arity_unknown();
    probe_slot.has_contract = true;
    probe_slot.contract.passthrough = true;
    probe_slot.exported = true;
    idm_scope_set_init(&probe_slot.scopes);
    IdmArtifact probe;
    memset(&probe, 0, sizeof(probe));
    probe.slots = &probe_slot;
    probe.slot_count = 1u;
    IdmBuffer probe_wire;
    IdmBuffer probe_wire_alt;
    idm_buf_init(&probe_wire);
    idm_buf_init(&probe_wire_alt);
    probe_slot.contract.primitive = 1u;
    check_ok(idm_artifact_serialize(&probe, &probe_wire, &err), &err, "probe serialize");
    probe_slot.contract.primitive = 2u;
    check_ok(idm_artifact_serialize(&probe, &probe_wire_alt, &err), &err, "probe serialize alt");
    size_t prim_at = wire_diff_u32_at(&probe_wire, &probe_wire_alt);

    IdmArtifact probe_art;
    check_ok(idm_artifact_deserialize(&rt, (const unsigned char *)probe_wire.data, probe_wire.len, &probe_art, &err), &err, "probe deserialize");
    check(probe_art.slot_count == 1u && probe_art.slots[0].has_contract && probe_art.slots[0].contract.passthrough && probe_art.slots[0].contract.primitive == 1u,
          "slot passthrough primitive survives the round trip on its contract");
    idm_artifact_destroy(&probe_art);

    memset(probe_wire.data + prim_at, 0xFF, 4u);
    check_deserialize_rejects(&rt, &probe_wire, "package slot primitive 4294967295 out of bounds", "slot primitive rejection names the bound");
    probe_wire_alt.data[prim_at - 1u] = 2;
    check_deserialize_rejects(&rt, &probe_wire_alt, "invalid callable contract passthrough flag", "passthrough flag rejection names the flag");
    idm_buf_destroy(&probe_wire);
    idm_buf_destroy(&probe_wire_alt);

    IdmPkgImport probe_import;
    memset(&probe_import, 0, sizeof(probe_import));
    probe_import.name = (char *)"q";
    probe_import.env_key = (char *)"k";
    probe_import.arity = idm_arity_unknown();
    probe_import.has_contract = true;
    probe_import.contract.passthrough = true;
    idm_scope_set_init(&probe_import.scopes);
    memset(&probe, 0, sizeof(probe));
    probe.imports = &probe_import;
    probe.import_count = 1u;
    idm_buf_init(&probe_wire);
    idm_buf_init(&probe_wire_alt);
    probe_import.contract.primitive = 1u;
    check_ok(idm_artifact_serialize(&probe, &probe_wire, &err), &err, "import probe serialize");
    probe_import.contract.primitive = 2u;
    check_ok(idm_artifact_serialize(&probe, &probe_wire_alt, &err), &err, "import probe serialize alt");
    prim_at = wire_diff_u32_at(&probe_wire, &probe_wire_alt);
    check_ok(idm_artifact_deserialize(&rt, (const unsigned char *)probe_wire.data, probe_wire.len, &probe_art, &err), &err, "import probe deserialize");
    check(probe_art.import_count == 1u && probe_art.imports[0].has_contract && probe_art.imports[0].contract.passthrough && probe_art.imports[0].contract.primitive == 1u,
          "import passthrough primitive survives the round trip on its contract");
    idm_artifact_destroy(&probe_art);
    memset(probe_wire.data + prim_at, 0xFF, 4u);
    check_deserialize_rejects(&rt, &probe_wire, "package import primitive 4294967295 out of bounds", "import primitive rejection names the bound");
    probe_wire_alt.data[prim_at - 1u] = 2;
    check_deserialize_rejects(&rt, &probe_wire_alt, "invalid callable contract passthrough flag", "import passthrough flag rejection names the flag");
    idm_buf_destroy(&probe_wire);
    idm_buf_destroy(&probe_wire_alt);

    idm_artifact_destroy(&art);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_declared_trait_source =
        "package bad_declared_trait_signature_contract\n"
        "\n"
        "trait Marker do\n"
        "  method marker-value x\n"
        "end\n"
        "\n"
        "implement Marker on int do\n"
        "  method marker-value x -> x\n"
        "end\n"
        "\n"
        "spec marker-id :: Marker a => a -> a\n"
        "defn marker-id x -> x\n"
        "\n"
        "spec bad :: string\n"
        "bad = marker-id \"x\"\n";
    dir = write_package_dir(bad_declared_trait_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad declared trait rejected");
    check(err.present && err.message && strstr(err.message, "not implemented for string") != NULL, "bad declared trait message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    char root_template[] = "/tmp/idiom_signature_cross_XXXXXX";
    char *root = mkdtemp(root_template);
    if (!root) fail("mkdtemp cross");
    char *root_owned = idm_strdup(root);
    if (!root_owned) fail("cross root copy");
    const char *old_idiompath = getenv("IDIOMPATH");
    char *old_idiompath_owned = old_idiompath ? idm_strdup(old_idiompath) : NULL;
    if (old_idiompath && !old_idiompath_owned) fail("IDIOMPATH copy");
    if (setenv("IDIOMPATH", root_owned, 1) != 0) fail("setenv IDIOMPATH");
    char *trait_dir = write_package_subdir(root_owned, "trait", "package cross_trait\n\nexport trait CrossTrait do\n  spec cross-value :: CrossTrait a => a -> int\n  method cross-value x\nend\n", &err);
    IdmBuffer impl_src;
    idm_buf_init(&impl_src);
    check(idm_buf_appendf(&impl_src,
        "package cross_impl\n"
        "use trait\n"
        "\n"
        "export record CrossBox do\n"
        "  field value :: int\n"
        "end\n"
        "\n"
        "implement CrossTrait on CrossBox do\n"
        "  method cross-value x -> x.value\n"
        "end\n"), "cross impl source");
    char *impl_dir = write_package_subdir(root_owned, "impl", impl_src.data, &err);
    idm_buf_destroy(&impl_src);
    IdmBuffer consumer_src;
    idm_buf_init(&consumer_src);
    check(idm_buf_appendf(&consumer_src,
        "package cross_consumer\n"
        "use impl\n"
        "use trait\n"
        "\n"
        "spec cross-id :: CrossTrait a => a -> a\n"
        "defn cross-id x -> x\n"
        "\n"
        "spec got :: CrossBox\n"
        "got = cross-id (CrossBox 7)\n"), "cross consumer source");
    char *consumer_dir = write_package_subdir(root_owned, "consumer", consumer_src.data, &err);
    idm_buf_destroy(&consumer_src);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, consumer_dir, &wire, &err), &err, "cross-file trait impl compile");
    idm_buf_destroy(&wire);
    remove_package_dir(consumer_dir);
    remove_package_dir(impl_dir);
    remove_package_dir(trait_dir);
    if (old_idiompath_owned) {
        if (setenv("IDIOMPATH", old_idiompath_owned, 1) != 0) fail("restore IDIOMPATH");
    } else {
        unsetenv("IDIOMPATH");
    }
    rmdir(root_owned);
    free(consumer_dir);
    free(impl_dir);
    free(trait_dir);
    free(old_idiompath_owned);
    free(root_owned);

    char fold_root_template[] = "/tmp/idiom_signature_crossfold_XXXXXX";
    char *fold_root = mkdtemp(fold_root_template);
    if (!fold_root) fail("mkdtemp crossfold");
    char *fold_root_owned = idm_strdup(fold_root);
    if (!fold_root_owned) fail("crossfold root copy");
    const char *fold_old_path = getenv("IDIOMPATH");
    char *fold_old_path_owned = fold_old_path ? idm_strdup(fold_old_path) : NULL;
    if (fold_old_path && !fold_old_path_owned) fail("crossfold IDIOMPATH copy");
    if (setenv("IDIOMPATH", fold_root_owned, 1) != 0) fail("crossfold setenv IDIOMPATH");
    char *provider_dir = write_package_subdir(fold_root_owned, "crossfold",
        "package crossfold\n"
        "\n"
        "## Quadruples the input.\n"
        "export defn quad x -> mul (twice x) 2\n"
        "\n"
        "defn twice x -> mul x 2\n", &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, provider_dir, &wire, &err), &err, "crossfold provider compiles");
    IdmArtifact fold_art;
    memset(&fold_art, 0, sizeof(fold_art));
    check_ok(idm_artifact_deserialize(&rt, (const unsigned char *)wire.data, wire.len, &fold_art, &err), &err, "crossfold artifact deserialize");
    size_t const_slot_count = 0;
    bool quad_doc_seen = false;
    for (size_t i = 0; i < fold_art.slot_count; i++) {
        if (fold_art.slots[i].const_entry_count != 0) const_slot_count++;
        if (!fold_art.slots[i].name || strcmp(fold_art.slots[i].name, "quad") != 0) continue;
        quad_doc_seen = fold_art.slots[i].has_contract && fold_art.slots[i].contract.doc &&
                        strcmp(fold_art.slots[i].contract.doc, "Quadruples the input.") == 0;
        check(fold_art.slots[i].has_contract && fold_art.slots[i].contract.purity == IDM_PURITY_CONST, "quad contract is const");
    }
    check(quad_doc_seen, "doc comment rides the artifact slot contract");
    check(const_slot_count == 2u, "crossfold artifact carries clause entries for both const slots");
    for (size_t i = 0; i < fold_art.slot_count; i++) {
        if (fold_art.slots[i].const_entry_count == 0) continue;
        check(fold_art.slots[i].const_entries != NULL, "crossfold const slot entries");
        for (uint32_t k = 0; k < fold_art.slots[i].const_entry_count; k++) {
            check(fold_art.module && fold_art.slots[i].const_entries[k] < fold_art.module->function_count, "crossfold const entry in module range");
        }
    }
    idm_artifact_destroy(&fold_art);
    idm_buf_destroy(&wire);
    char *wide_dir = write_package_subdir(fold_root_owned, "widefold",
        "package widefold\n"
        "\n"
        "export defn wide -> list 1 2 3 4 5 6 7 8 (nine)\n"
        "\n"
        "defn nine -> 9\n", &err);
    IdmBuffer wide_wire;
    idm_buf_init(&wide_wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, wide_dir, &wide_wire, &err), &err, "widefold provider compiles");
    IdmArtifact wide_art;
    memset(&wide_art, 0, sizeof(wide_art));
    check_ok(idm_artifact_deserialize(&rt, (const unsigned char *)wide_wire.data, wide_wire.len, &wide_art, &err), &err, "wide const slot survives the codec round trip");
    size_t wide_const_count = 0;
    for (size_t i = 0; i < wide_art.slot_count; i++) {
        if (wide_art.slots[i].const_entry_count != 0) wide_const_count++;
    }
    check(wide_const_count >= 1u, "widefold artifact carries the wide const slot");
    idm_artifact_destroy(&wide_art);
    idm_buf_destroy(&wide_wire);
    free(wide_dir);

    IdmCore *cf_core = NULL;
    check_ok(idm_expand_source_string(&rt, "<crossfold>", "use crossfold\ngot = quad 5\ngot\n", &cf_core, &err), &err, "crossfold consumer expand");
    IdmBuffer cf_dump;
    idm_buf_init(&cf_dump);
    check(idm_core_dump(&cf_dump, cf_core), "crossfold dump");
    check(cf_dump.data && strstr(cf_dump.data, "20") != NULL, "cross-package fold literal");
    check(!cf_dump.data || strstr(cf_dump.data, "quad") == NULL, "cross-package call erased");
    idm_buf_destroy(&cf_dump);
    idm_core_free(cf_core);
    remove_package_dir(provider_dir);
    if (fold_old_path_owned) {
        if (setenv("IDIOMPATH", fold_old_path_owned, 1) != 0) fail("crossfold restore IDIOMPATH");
    } else {
        unsetenv("IDIOMPATH");
    }
    rmdir(fold_root_owned);
    free(provider_dir);
    free(fold_old_path_owned);
    free(fold_root_owned);

    const char *implements_static_source =
        "trait FoldProbe do\n"
        "  method fold-probe x -> x\n"
        "end\n"
        "\n"
        "implement FoldProbe on int do\n"
        "  method fold-probe x -> x\n"
        "end\n"
        "\n"
        "implements? FoldProbe 1\n";
    IdmCore *core = NULL;
    check_ok(idm_expand_source_string(&rt, "<implements-static>", implements_static_source, &core, &err), &err, "implements static expand");
    IdmBuffer core_dump;
    idm_buf_init(&core_dump);
    check(idm_core_dump(&core_dump, core), "implements static core dump");
    check(core_dump.data && strstr(core_dump.data, ":true") != NULL, "implements static literal");
    check(!core_dump.data || strstr(core_dump.data, "fn-multi implements?") == NULL, "implements static no selector");
    idm_buf_destroy(&core_dump);
    idm_core_free(core);

    const char *fold_source =
        "a = add 2 3\n"
        "b = mul (add 1 2) (sub 10 4)\n"
        "c = div 84 4\n"
        "d = div 1 0\n"
        "b\n";
    IdmCore *fold_core = NULL;
    check_ok(idm_expand_source_string(&rt, "<fold>", fold_source, &fold_core, &err), &err, "fold expand");
    IdmBuffer fold_dump;
    idm_buf_init(&fold_dump);
    check(idm_core_dump(&fold_dump, fold_core), "fold core dump");
    check(fold_dump.data && strstr(fold_dump.data, "5") != NULL, "fold add literal");
    check(fold_dump.data && strstr(fold_dump.data, "18") != NULL, "fold nested literal");
    check(fold_dump.data && strstr(fold_dump.data, "21") != NULL, "fold partial prim with safe literal args");
    check(fold_dump.data && strstr(fold_dump.data, "primitive div") != NULL, "fold preserves crashing call");
    check(!fold_dump.data || strstr(fold_dump.data, "primitive add") == NULL, "fold erased pure add");
    idm_buf_destroy(&fold_dump);
    idm_core_free(fold_core);

    const char *user_fold_source =
        "defn triple x -> mul x 3\n"
        "defn fact n -> cond (lte? n 1) 1 (mul n (fact (sub n 1)))\n"
        "a = triple 7\n"
        "b = fact 6\n"
        "b\n";
    IdmCore *uf_core = NULL;
    check_ok(idm_expand_source_string(&rt, "<userfold>", user_fold_source, &uf_core, &err), &err, "user fold expand");
    IdmBuffer uf_dump;
    idm_buf_init(&uf_dump);
    check(idm_core_dump(&uf_dump, uf_core), "user fold dump");
    check(uf_dump.data && strstr(uf_dump.data, "21") != NULL, "user fold simple literal");
    check(uf_dump.data && strstr(uf_dump.data, "720") != NULL, "user fold recursive literal");
    idm_buf_destroy(&uf_dump);
    idm_core_free(uf_core);

    const char *wide_user_fold_source =
        "defn ninth a b c d e f g h i -> i\n"
        "c = ninth 1 2 3 4 5 6 7 8 9\n"
        "c\n";
    IdmCore *wuf_core = NULL;
    check_ok(idm_expand_source_string(&rt, "<wide-userfold>", wide_user_fold_source, &wuf_core, &err), &err, "wide user fold expand");
    IdmBuffer wuf_dump;
    idm_buf_init(&wuf_dump);
    check(idm_core_dump(&wuf_dump, wuf_core), "wide user fold dump");
    check(wuf_dump.data && strstr(wuf_dump.data, "(env ninth") == NULL, "user fold has no fixed argument cap");
    idm_buf_destroy(&wuf_dump);
    idm_core_free(wuf_core);

    IdmBuffer deep_fold_source;
    idm_buf_init(&deep_fold_source);
    check(idm_buf_append(&deep_fold_source, "defn keep-deep x -> x\nv = keep-deep "), "deep fold head");
    for (size_t i = 0; i < 80u; i++) check(idm_buf_append_char(&deep_fold_source, '{'), "deep fold open");
    check(idm_buf_append_char(&deep_fold_source, '0'), "deep fold value");
    for (size_t i = 0; i < 80u; i++) check(idm_buf_append_char(&deep_fold_source, '}'), "deep fold close");
    check(idm_buf_append(&deep_fold_source, "\nv\n"), "deep fold tail");
    IdmCore *deep_fold_core = NULL;
    check_ok(idm_expand_source_string(&rt, "<deep-userfold>", deep_fold_source.data, &deep_fold_core, &err), &err, "deep user fold expand");
    IdmBuffer deep_fold_dump;
    idm_buf_init(&deep_fold_dump);
    check(idm_core_dump(&deep_fold_dump, deep_fold_core), "deep user fold dump");
    check(deep_fold_dump.data && strstr(deep_fold_dump.data, "(env keep-deep") == NULL, "user fold has no nesting cap");
    idm_buf_destroy(&deep_fold_dump);
    idm_core_free(deep_fold_core);
    idm_buf_destroy(&deep_fold_source);

    IdmBuffer sparse_source;
    check(wide_sparse_source(&sparse_source, true), "wide sparse hole source");
    IdmCore *sparse_core = NULL;
    check(!idm_expand_source_string(&rt, "<wide-sparse-hole>", sparse_source.data, &sparse_core, &err), "wide sparse arity hole rejected");
    check(err.present && err.message && strstr(err.message, "no contract signature for 1 arguments") != NULL, "wide sparse arity rejection names contract boundary");
    idm_error_clear(&err);
    idm_buf_destroy(&sparse_source);
    check(wide_sparse_source(&sparse_source, false), "wide sparse exact source");
    check_ok(idm_expand_source_string(&rt, "<wide-sparse-exact>", sparse_source.data, &sparse_core, &err), &err, "wide sparse exact arity expands");
    idm_core_free(sparse_core);
    idm_buf_destroy(&sparse_source);

    const char *dead_source =
        "defn keepy x -> do\n"
        "  x\n"
        "  \"discard\"\n"
        "  mul x 2\n"
        "end\n"
        "keepy 4\n";
    IdmCore *dead_core = NULL;
    check_ok(idm_expand_source_string(&rt, "<dead>", dead_source, &dead_core, &err), &err, "dead expand");
    IdmBuffer dead_dump;
    idm_buf_init(&dead_dump);
    check(idm_core_dump(&dead_dump, dead_core), "dead dump");
    check(!dead_dump.data || strstr(dead_dump.data, "discard") == NULL, "dead literal statement eliminated");
    check(dead_dump.data && strstr(dead_dump.data, "primitive mul") != NULL, "dead keeps result expression");
    idm_buf_destroy(&dead_dump);
    idm_core_free(dead_core);

    const char *dead_call_source =
        "defn quiet x -> do\n"
        "  int? x\n"
        "  div x 0\n"
        "  mul x 2\n"
        "end\n"
        "quiet 4\n";
    IdmCore *dc_core = NULL;
    check_ok(idm_expand_source_string(&rt, "<deadcall>", dead_call_source, &dc_core, &err), &err, "dead call expand");
    IdmBuffer dc_dump;
    idm_buf_init(&dc_dump);
    check(idm_core_dump(&dc_dump, dc_core), "dead call dump");
    check(!dc_dump.data || strstr(dc_dump.data, "primitive int?") == NULL, "total prim statement eliminated");
    check(dc_dump.data && strstr(dc_dump.data, "primitive div") != NULL, "partial prim statement kept");
    idm_buf_destroy(&dc_dump);
    idm_core_free(dc_core);

    const char *generic_method_source =
        "package generic_method_signature_contract\n"
        "\n"
        "trait GenericMethod do\n"
        "  spec generic-value :: GenericMethod a => a -> int\n"
        "  method generic-value x\n"
        "end\n"
        "\n"
        "record GenericBox do\n"
        "  field value :: int\n"
        "end\n"
        "\n"
        "implement GenericMethod on GenericBox do\n"
        "  method generic-value x -> x.value\n"
        "end\n"
        "\n"
        "spec generic-use :: GenericMethod a => a -> int\n"
        "defn generic-use x -> generic-value x\n"
        "\n"
        "spec generic-direct :: int\n"
        "generic-direct = generic-value (GenericBox 12)\n"
        "\n"
        "spec generic-got :: int\n"
        "generic-got = generic-use (GenericBox 11)\n";
    dir = write_package_dir(generic_method_source, &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "generic method call given compiles");
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_method_impl_source =
        "package bad_method_impl_signature_contract\n"
        "\n"
        "trait BadMethod do\n"
        "  spec bad-method :: BadMethod a => a -> int\n"
        "  method bad-method x\n"
        "end\n"
        "\n"
        "record BadMethodBox do\n"
        "  field value :: string\n"
        "end\n"
        "\n"
        "implement BadMethod on BadMethodBox do\n"
        "  method bad-method x -> x.value\n"
        "end\n";
    dir = write_package_dir(bad_method_impl_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad method impl rejected");
    check(err.present && err.message && strstr(err.message, "type mismatch for 'bad-method': expected int, got string") != NULL, "bad method impl message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_method_default_source =
        "package bad_method_default_signature_contract\n"
        "\n"
        "trait BadDefault do\n"
        "  spec bad-default :: a -> int\n"
        "  method bad-default x -> \"x\"\n"
        "end\n";
    dir = write_package_dir(bad_method_default_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad method default rejected");
    check(err.present && err.message && strstr(err.message, "type mismatch for 'bad-default': expected int, got string") != NULL, "bad method default message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_method_call_source =
        "package bad_method_call_signature_contract\n"
        "\n"
        "trait CallSpec do\n"
        "  spec call-spec :: CallSpec a => a int -> int\n"
        "  method call-spec x y\n"
        "end\n"
        "\n"
        "record CallSpecBox do\n"
        "  field value :: int\n"
        "end\n"
        "\n"
        "implement CallSpec on CallSpecBox do\n"
        "  method call-spec x y -> add x.value y\n"
        "end\n"
        "\n"
        "spec bad :: int\n"
        "bad = call-spec (CallSpecBox 1) \"x\"\n";
    dir = write_package_dir(bad_method_call_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad method call rejected");
    check(err.present && err.message && strstr(err.message, "type mismatch for 'call-spec': expected int, got string") != NULL, "bad method call message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_method_no_impl_source =
        "package bad_method_no_impl_signature_contract\n"
        "\n"
        "trait StaticOnly do\n"
        "  spec static-only :: StaticOnly a => a -> int\n"
        "  method static-only x\n"
        "end\n"
        "\n"
        "record StaticBox do\n"
        "  field value :: int\n"
        "end\n"
        "\n"
        "record StaticOther do\n"
        "end\n"
        "\n"
        "implement StaticOnly on StaticBox do\n"
        "  method static-only x -> x.value\n"
        "end\n"
        "\n"
        "spec bad :: int\n"
        "bad = static-only StaticOther\n";
    dir = write_package_dir(bad_method_no_impl_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad method no impl rejected");
    check(err.present && err.message && strstr(err.message, "method 'static-only' is not implemented on receiver type") != NULL, "bad method no impl message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_method_unsolved_source =
        "package bad_method_unsolved_signature_contract\n"
        "\n"
        "trait MissingGiven do\n"
        "  spec missing-given :: MissingGiven a => a -> int\n"
        "  method missing-given x\n"
        "end\n"
        "\n"
        "record MissingGivenBox do\n"
        "  field value :: int\n"
        "end\n"
        "\n"
        "implement MissingGiven on MissingGivenBox do\n"
        "  method missing-given x -> x.value\n"
        "end\n"
        "\n"
        "spec bad :: a -> int\n"
        "defn bad x -> missing-given x\n";
    dir = write_package_dir(bad_method_unsolved_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad method unsolved rejected");
    check(err.present && err.message && strstr(err.message, "unsolved typeclass 'MissingGiven' for a") != NULL, "bad method unsolved message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_overtype_source =
        "package bad_overtype_signature_contract\n"
        "\n"
        "type Num :: int | float\n"
        "\n"
        "spec bad :: Num\n"
        "bad = \"x\"\n";
    dir = write_package_dir(bad_overtype_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad overtype rejected");
    check(err.present && err.message && strstr(err.message, "got string") != NULL, "bad overtype message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_source =
        "package bad_signature_contract\n"
        "\n"
        "spec bad :: int -> int\n"
        "export defn bad -> 1\n";
    dir = write_package_dir(bad_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad arity rejected");
    check(err.present && err.message && strstr(err.message, "signature for 'bad' expects arity 1") != NULL, "bad arity message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_value_source =
        "package bad_value_signature_contract\n"
        "\n"
        "spec bad-value :: int -> int\n"
        "bad-value = fn x y -> x\n";
    dir = write_package_dir(bad_value_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad value arity rejected");
    check(err.present && err.message && strstr(err.message, "signature for 'bad-value' expects arity 1") != NULL, "bad value arity message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_type_source =
        "package bad_type_signature_contract\n"
        "\n"
        "spec bad :: int\n"
        "bad = \"no\"\n";
    dir = write_package_dir(bad_type_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad value type rejected");
    check(err.present && err.message && strstr(err.message, "type mismatch for 'bad': expected int, got string") != NULL, "bad value type message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bits_value_source =
        "package bits_value_signature_contract\n"
        "\n"
        "first-byte = fn %<a:8> -> a\n"
        "second = first-byte %<65:8>\n";
    dir = write_package_dir(bits_value_source, &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "bits value fn accepted");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_bits_result_source =
        "package bad_bits_result_signature_contract\n"
        "\n"
        "spec bad :: bitstring -> string\n"
        "bad = fn %<a:8> -> a\n";
    dir = write_package_dir(bad_bits_result_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad bits result rejected");
    check(err.present && err.message && strstr(err.message, "type mismatch for 'bad': expected string, got int") != NULL, "bad bits result message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_bits_rigid_source =
        "package bad_bits_rigid_signature_contract\n"
        "\n"
        "spec bad :: a -> int\n"
        "bad = fn %<x:8> -> x\n";
    dir = write_package_dir(bad_bits_rigid_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad bits rigid rejected");
    check(err.present && err.message && strstr(err.message, "expected bitstring, got a") != NULL, "bad bits rigid message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_primitive_source =
        "package bad_primitive_signature_contract\n"
        "\n"
        "spec bad :: int\n"
        "bad = inspect 1\n";
    dir = write_package_dir(bad_primitive_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad primitive result rejected");
    check(err.present && err.message && strstr(err.message, "type mismatch for 'bad': expected int, got string") != NULL, "bad primitive result message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_number_source =
        "package bad_number_signature_contract\n"
        "\n"
        "spec bad :: string\n"
        "bad = add \"a\" \"b\"\n";
    dir = write_package_dir(bad_number_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad numeric class rejected");
    check(err.present && err.message && strstr(err.message, "typeclass 'Number' is not implemented for string") != NULL, "bad numeric class message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_unsolved_source =
        "package bad_unsolved_signature_contract\n"
        "\n"
        "spec bad :: a -> a\n"
        "export defn bad x -> add x x\n";
    dir = write_package_dir(bad_unsolved_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad unsolved class rejected");
    check(err.present && err.message && strstr(err.message, "unsolved typeclass 'Number' for a") != NULL, "bad unsolved class message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_return_source =
        "package bad_return_signature_contract\n"
        "\n"
        "spec bad :: int -> int\n"
        "export defn bad x -> \"no\"\n";
    dir = write_package_dir(bad_return_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad defn return rejected");
    check(err.present && err.message && strstr(err.message, "type mismatch for 'bad': expected int, got string") != NULL, "bad defn return message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_poly_source =
        "package bad_poly_signature_contract\n"
        "\n"
        "spec bad :: a -> b\n"
        "export defn bad x -> x\n";
    dir = write_package_dir(bad_poly_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad polymorphic return rejected");
    check(err.present && err.message && strstr(err.message, "type mismatch for 'bad': expected b, got a") != NULL, "bad polymorphic return message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *bad_record_source =
        "package bad_record_signature_contract\n"
        "\n"
        "record Box do\n"
        "  field value :: int\n"
        "end\n"
        "\n"
        "bad = Box \"no\"\n";
    dir = write_package_dir(bad_record_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "bad record field rejected");
    check(err.present && err.message && strstr(err.message, "type mismatch for 'Box': expected int, got string") != NULL, "bad record field message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *inferred_scheme_source =
        "package inferred_scheme_signature_contract\n"
        "\n"
        "export defn dub x -> add x x\n"
        "\n"
        "export defn myid x -> x\n"
        "\n"
        "export defn pickfirst x y -> x\n";
    dir = write_package_dir(inferred_scheme_source, &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "inferred scheme compiles");
    IdmArtifact inf_art;
    check_ok(idm_artifact_deserialize(&rt, (const unsigned char *)(wire.data ? wire.data : ""), wire.len, &inf_art, &err), &err, "inferred scheme artifact");
    const IdmPkgSlot *dub = find_slot(&inf_art, "dub");
    check(dub && dub->has_contract, "dub inferred contract");
    check(dub->contract.sigs[0].arg_count == 1u && dub->contract.sigs[0].has_result, "dub inferred shape");
    check(dub->contract.quantified_count == 1u, "dub inferred quantifier");
    check(dub->contract.context_count == 1u, "dub inferred context");
    check(dub->contract.context[0].trait && strncmp(idm_symbol_text(dub->contract.context[0].trait), "Number", 6u) == 0, "dub inferred Number");
    check(type_var_named(&dub->contract.sigs[0].args[0], "a"), "dub inferred arg a");
    check(type_var_named(&dub->contract.sigs[0].result, "a"), "dub inferred result a");
    check(dub->contract.sigs[0].args[0].var_id == dub->contract.sigs[0].result.var_id, "dub inferred shared var");
    const IdmPkgSlot *myid = find_slot(&inf_art, "myid");
    check(myid && myid->has_contract, "myid inferred contract");
    check(myid->contract.quantified_count == 1u && myid->contract.context_count == 0u, "myid inferred principal");
    check(type_var_named(&myid->contract.sigs[0].args[0], "a") && type_var_named(&myid->contract.sigs[0].result, "a"), "myid inferred a to a");
    const IdmPkgSlot *pickfirst = find_slot(&inf_art, "pickfirst");
    check(pickfirst && pickfirst->has_contract, "pickfirst inferred contract");
    check(pickfirst->contract.quantified_count == 2u, "pickfirst inferred two vars");
    check(pickfirst->contract.sigs[0].args[0].var_id == pickfirst->contract.sigs[0].result.var_id, "pickfirst result shares first");
    check(pickfirst->contract.sigs[0].args[0].var_id != pickfirst->contract.sigs[0].args[1].var_id, "pickfirst distinct vars");
    idm_artifact_destroy(&inf_art);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *match_refine_source =
        "package match_refine_signature_contract\n"
        "\n"
        "record Hit do\n"
        "  field pos :: int\n"
        "end\n"
        "\n"
        "spec find-hit :: int -> Hit | nil\n"
        "export defn find-hit n -> cond (lt? n 0) :nil (Hit n)\n"
        "\n"
        "spec must-hit :: int -> Hit\n"
        "export defn must-hit n -> match (find-hit n) do\n"
        "  :nil -> raise :no-hit\n"
        "  h -> h\n"
        "end\n";
    dir = write_package_dir(match_refine_source, &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "match negative refinement compiles");
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *match_refine_bad_source =
        "package match_refine_bad_signature_contract\n"
        "\n"
        "record Hit do\n"
        "  field pos :: int\n"
        "end\n"
        "\n"
        "spec find-hit :: int -> Hit | nil\n"
        "export defn find-hit n -> cond (lt? n 0) :nil (Hit n)\n"
        "\n"
        "spec must-hit :: int -> Hit\n"
        "export defn must-hit n -> match (find-hit n) do\n"
        "  h -> h\n"
        "end\n";
    dir = write_package_dir(match_refine_bad_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "match without refinement rejected");
    check(err.present && err.message && strstr(err.message, "type mismatch for 'must-hit'") != NULL, "match without refinement message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *macro_spec_source =
        "package macro_spec_signature_contract\n"
        "\n"
        "defmacro def-typed do\n"
        "  %`(def-typed %,name %,maker) -> %`(do\n"
        "    record %,name do\n"
        "      field value :: int\n"
        "    end\n"
        "    spec %,maker :: int -> %,name\n"
        "    defn %,maker n -> %,name n\n"
        "  end)\n"
        "end\n"
        "\n"
        "def-typed Boxy make-it\n"
        "\n"
        "spec bad :: string\n"
        "bad = make-it 3\n";
    dir = write_package_dir(macro_spec_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "macro-generated spec enforced");
    check(err.present && err.message && strstr(err.message, "type mismatch for 'bad': expected string, got Boxy") != NULL, "macro-generated spec message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *multi_arity_source =
        "package multi_arity_signature_contract\n"
        "\n"
        "spec scale :: int -> int\n"
        "spec scale :: int int -> int\n"
        "export defn scale do\n"
        "  n -> mul n 2\n"
        "  n f -> mul n f\n"
        "end\n"
        "\n"
        "export defn pick do\n"
        "  a -> a\n"
        "  a _b -> a\n"
        "end\n"
        "\n"
        "spec uses :: int\n"
        "uses = add (scale 3) (scale 3 4)\n";
    dir = write_package_dir(multi_arity_source, &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "multi-arity spec compiles");
    IdmArtifact ma_art;
    check_ok(idm_artifact_deserialize(&rt, (const unsigned char *)(wire.data ? wire.data : ""), wire.len, &ma_art, &err), &err, "multi-arity artifact");
    const IdmPkgSlot *scale = find_slot(&ma_art, "scale");
    check(scale && scale->has_contract, "scale contract");
    check(scale->contract.sig_count == 2u, "scale two sigs");
    check(idm_contract_sig_for(&scale->contract, 1u) != NULL, "scale sig/1");
    check(idm_contract_sig_for(&scale->contract, 2u) != NULL, "scale sig/2");
    const IdmPkgSlot *pick = find_slot(&ma_art, "pick");
    check(pick && pick->has_contract, "pick inferred contract");
    check(pick->contract.sig_count == 2u, "pick inferred two sigs");
    idm_artifact_destroy(&ma_art);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *multi_method_source =
        "package multi_method_signature_contract\n"
        "\n"
        "record MultiBox do\n"
        "end\n"
        "\n"
        "trait MultiMethod do\n"
        "  method choose do\n"
        "    _x -> 1\n"
        "    _x _y _z -> 3\n"
        "  end\n"
        "end\n"
        "\n"
        "implement MultiMethod on MultiBox\n"
        "\n"
        "one = choose (MultiBox)\n"
        "three = choose (MultiBox) 2 3\n";
    dir = write_package_dir(multi_method_source, &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "multi-arity method exact calls compile");
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *multi_method_hole_source =
        "package multi_method_hole_signature_contract\n"
        "\n"
        "record MultiBox do\n"
        "end\n"
        "\n"
        "trait MultiMethod do\n"
        "  method choose do\n"
        "    _x -> 1\n"
        "    _x _y _z -> 3\n"
        "  end\n"
        "end\n"
        "\n"
        "implement MultiMethod on MultiBox\n"
        "\n"
        "bad = choose (MultiBox) 2\n";
    dir = write_package_dir(multi_method_hole_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "multi-arity method hole rejected");
    check(err.present && err.message && strstr(err.message, "no contract signature for 2 arguments") != NULL, "multi-arity method hole message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *multi_arity_bad_source =
        "package multi_arity_bad_signature_contract\n"
        "\n"
        "spec scale :: int -> int\n"
        "spec scale :: int int -> int\n"
        "export defn scale do\n"
        "  n -> mul n 2\n"
        "  n f -> mul n f\n"
        "end\n"
        "\n"
        "spec bad :: int\n"
        "bad = scale 1 \"x\"\n";
    dir = write_package_dir(multi_arity_bad_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "multi-arity bad arg rejected");
    check(err.present && err.message && strstr(err.message, "type mismatch for 'scale': expected int, got string") != NULL, "multi-arity bad arg message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *structural_source =
        "package structural_signature_contract\n"
        "\n"
        "record SBox do\n"
        "  field size :: int\n"
        "end\n"
        "\n"
        "trait Sized do\n"
        "  method sized x -> x.size\n"
        "end\n"
        "\n"
        "implement Sized on _.size :: int\n"
        "\n"
        "spec got :: int\n"
        "got = sized (SBox 3)\n";
    dir = write_package_dir(structural_source, &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "structural impl compiles");
    IdmArtifact structural_art;
    check_ok(idm_artifact_deserialize(&rt, (const unsigned char *)(wire.data ? wire.data : ""), wire.len, &structural_art, &err), &err, "structural impl artifact deserialize");
    const IdmPkgMethodImpl *structural_impl = NULL;
    for (size_t i = 0; i < structural_art.typed.method_impl_count; i++) {
        if (structural_art.typed.method_impls[i].structural) { structural_impl = &structural_art.typed.method_impls[i]; break; }
    }
    check(structural_impl != NULL, "structural impl survives artifact wire");
    check(strcmp(idm_symbol_text(structural_impl->structural_head.field), "size") == 0 &&
          structural_impl->structural_head.has_type &&
          strcmp(idm_type_term_text(&structural_impl->structural_head.type), "int") == 0,
          "structural impl head survives artifact wire");
    idm_artifact_destroy(&structural_art);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *structural_bad_source =
        "package structural_bad_signature_contract\n"
        "\n"
        "record SName do\n"
        "  field size :: string\n"
        "end\n"
        "\n"
        "trait Sized do\n"
        "  method sized x -> x.size\n"
        "end\n"
        "\n"
        "implement Sized on _.size :: int\n"
        "\n"
        "bad = sized (SName \"no\")\n";
    dir = write_package_dir(structural_bad_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "structural field type mismatch rejected");
    check(err.present && err.message && strstr(err.message, "not implemented on receiver type") != NULL, "structural mismatch message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *structural_ctx_source =
        "package structural_ctx_signature_contract\n"
        "\n"
        "record CBox do\n"
        "  field size :: int\n"
        "end\n"
        "\n"
        "spec sizeof :: _.size :: int a => a -> int\n"
        "defn sizeof x -> x.size\n"
        "\n"
        "spec wrap :: _.size :: int a => a -> int\n"
        "defn wrap x -> sizeof x\n"
        "\n"
        "spec got :: int\n"
        "got = sizeof (CBox 3)\n"
        "\n"
        "spec got2 :: int\n"
        "got2 = wrap (CBox 4)\n";
    dir = write_package_dir(structural_ctx_source, &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "structural spec constraint compiles");
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *structural_ctx_bad_source =
        "package structural_ctx_bad_signature_contract\n"
        "\n"
        "record CName do\n"
        "  field label :: string\n"
        "end\n"
        "\n"
        "spec sizeof :: _.size :: int a => a -> int\n"
        "defn sizeof x -> 0\n"
        "\n"
        "bad = sizeof (CName \"no\")\n";
    dir = write_package_dir(structural_ctx_bad_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "structural spec constraint rejects");
    check(err.present && err.message && strstr(err.message, "structural constraint '_.size::int' is not satisfied by") != NULL, "structural spec constraint message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *clause_local_source =
        "package clause_local_signature_contract\n"
        "\n"
        "defn shift p from to delta -> cond (lt? p from) p (cond (gte? p to) (p + delta) from)\n"
        "\n"
        "defn shift-mark do\n"
        "  :nil _from _to _delta -> :nil\n"
        "  m from to delta -> shift m from to delta\n"
        "end\n"
        "\n"
        "defn shift-match m from to delta -> match m do\n"
        "  :nil -> :nil\n"
        "  v -> shift v from to delta\n"
        "end\n"
        "\n"
        "spec moved :: int\n"
        "moved = shift 4 1 2 3\n"
        "\n"
        "kept = shift-mark :nil 1 2 3\n"
        "\n"
        "also = shift-match 9 1 2 3\n";
    dir = write_package_dir(clause_local_source, &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "nil clause does not poison sibling numeric params");
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *purity_source =
        "package purity_signature_contract\n"
        "\n"
        "export defn calm x -> add x 1\n"
        "\n"
        "export defn chatty x -> do\n"
        "  println x\n"
        "  x\n"
        "end\n"
        "\n"
        "export defn relay x -> chatty x\n"
        "\n"
        "export defn croaky x -> cond (lt? x 0) (raise :neg) x\n"
        "\n"
        "export defn stacked x -> calm (calm x)\n"
        "\n"
        "export defn twice f x -> f (f x)\n"
        "\n"
        "export defn resolved x -> twice (fn v -> add v 1) x\n"
        "\n"
        "export defn tainted x -> twice (fn v -> do\n"
        "  println v\n"
        "  v\n"
        "end) x\n"
        "\n"
        "export defn mapish do\n"
        "  '() _f -> '()\n"
        "  '(h . t) f -> cons (f &h) (mapish t f)\n"
        "end\n"
        "\n"
        "export defn doubled xs -> mapish xs (fn v -> mul v 2)\n";
    IdmBuffer purity_src;
    idm_buf_init(&purity_src);
    check(idm_buf_append(&purity_src, purity_source), "purity source base");
    check(idm_buf_append(&purity_src, "\nexport defn invokes65 "), "wide purity head");
    for (size_t i = 0; i < 64u; i++) check(idm_buf_appendf(&purity_src, "a%zu ", i), "wide purity arg");
    check(idm_buf_append(&purity_src, "f -> f 1\n"), "wide purity body");
    dir = write_package_dir(purity_src.data, &err);
    idm_buf_destroy(&purity_src);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "purity package compiles");
    IdmArtifact pur_art;
    check_ok(idm_artifact_deserialize(&rt, (const unsigned char *)(wire.data ? wire.data : ""), wire.len, &pur_art, &err), &err, "purity artifact");
    const IdmPkgSlot *calm = find_slot(&pur_art, "calm");
    check(calm && calm->has_contract && calm->contract.purity == IDM_PURITY_CONST, "calm is const");
    const IdmPkgSlot *chatty = find_slot(&pur_art, "chatty");
    check(chatty && chatty->has_contract && chatty->contract.purity == IDM_PURITY_IMPURE, "chatty is impure");
    const IdmPkgSlot *relay = find_slot(&pur_art, "relay");
    check(relay && relay->has_contract && relay->contract.purity == IDM_PURITY_IMPURE, "relay impurity is transitive");
    const IdmPkgSlot *croaky = find_slot(&pur_art, "croaky");
    check(croaky && croaky->has_contract && croaky->contract.purity == IDM_PURITY_IMPURE, "croaky crash home is impure");
    const IdmPkgSlot *stacked = find_slot(&pur_art, "stacked");
    check(stacked && stacked->has_contract && stacked->contract.purity == IDM_PURITY_CONST, "stacked const purity is transitive");
    const IdmPkgSlot *twice = find_slot(&pur_art, "twice");
    check(twice && twice->has_contract && twice->contract.purity == IDM_PURITY_ARGS, "twice is pure-if-args-pure");
    const IdmPkgSlot *resolved = find_slot(&pur_art, "resolved");
    check(resolved && resolved->has_contract && resolved->contract.purity >= IDM_PURITY_ARGS, "resolved higher-order call is not impure");
    const IdmPkgSlot *tainted = find_slot(&pur_art, "tainted");
    check(tainted && tainted->has_contract && tainted->contract.purity == IDM_PURITY_IMPURE, "tainted higher-order call is impure");
    const IdmPkgSlot *mapish = find_slot(&pur_art, "mapish");
    check(mapish && mapish->has_contract && mapish->contract.purity == IDM_PURITY_ARGS, "mapish is args-pure");
    check(mapish->contract.sig_count && !idm_arg_mask_test(&mapish->contract.sigs[0].invoked, 0u) && idm_arg_mask_test(&mapish->contract.sigs[0].invoked, 1u), "mapish invokes f not xs");
    const IdmPkgSlot *doubled = find_slot(&pur_art, "doubled");
    check(doubled && doubled->has_contract && doubled->contract.purity == IDM_PURITY_PURE, "doubled with pure lambda is fully pure");
    const IdmPkgSlot *invokes65 = find_slot(&pur_art, "invokes65");
    check(invokes65 && invokes65->has_contract && invokes65->contract.sig_count != 0, "wide invocation contract");
    check(idm_arg_mask_test(&invokes65->contract.sigs[0].invoked, 64u) && !idm_arg_mask_test(&invokes65->contract.sigs[0].invoked, 63u), "invocation tracking has no 64-argument boundary");
    idm_artifact_destroy(&pur_art);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *match_dead_source =
        "package match_dead_signature_contract\n"
        "\n"
        "spec pick :: string -> int\n"
        "export defn pick s -> match s do\n"
        "  '() -> 0\n"
        "  _ -> 1\n"
        "end\n";
    dir = write_package_dir(match_dead_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "dead match clause rejected");
    check(err.present && err.message && strstr(err.message, "unreachable for scrutinee type string") != NULL, "dead match clause message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *match_missing_source =
        "package match_missing_signature_contract\n"
        "\n"
        "spec count :: list -> int\n"
        "export defn count xs -> match xs do\n"
        "  '() -> 0\n"
        "end\n";
    dir = write_package_dir(match_missing_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "non-exhaustive type-case rejected");
    check(err.present && err.message && strstr(err.message, "non-exhaustive match") != NULL, "non-exhaustive type-case message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *match_total_source =
        "package match_total_signature_contract\n"
        "\n"
        "spec count :: list -> int\n"
        "export defn count xs -> match xs do\n"
        "  '() -> 0\n"
        "  '(_h . t) -> add 1 (count t)\n"
        "end\n";
    dir = write_package_dir(match_total_source, &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "total type-case compiles");
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *match_after_catchall_source =
        "package match_after_catchall_signature_contract\n"
        "\n"
        "spec pick :: int -> int\n"
        "export defn pick n -> match n do\n"
        "  _ -> 0\n"
        "  1 -> 1\n"
        "end\n";
    dir = write_package_dir(match_after_catchall_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "clause after catchall rejected");
    check(err.present && err.message && strstr(err.message, "match clause is unreachable") != NULL, "clause after catchall message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *match_value_partial_source =
        "package match_value_partial_signature_contract\n"
        "\n"
        "spec status :: (tuple | atom) -> int\n"
        "export defn status r -> match r do\n"
        "  {:ok n} -> n\n"
        "  :eof -> 0\n"
        "end\n";
    dir = write_package_dir(match_value_partial_source, &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "value-case partiality compiles");
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *match_branch_ok_source =
        "package match_branch_ok_signature_contract\n"
        "\n"
        "spec pick :: int -> int\n"
        "export defn pick n -> match n do\n"
        "  0 -> 100\n"
        "  other -> other\n"
        "end\n";
    dir = write_package_dir(match_branch_ok_source, &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "match branch consistent compiles");
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    const char *match_branch_bad_source =
        "package match_branch_bad_signature_contract\n"
        "\n"
        "spec pick :: int -> int\n"
        "export defn pick n -> match n do\n"
        "  0 -> \"no\"\n"
        "  other -> other\n"
        "end\n";
    dir = write_package_dir(match_branch_bad_source, &err);
    idm_buf_init(&wire);
    check(!idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), "match branch mismatch rejected");
    check(err.present && err.message && strstr(err.message, "type mismatch for 'pick': expected int, got string") != NULL, "match branch mismatch message");
    idm_error_clear(&err);
    idm_buf_destroy(&wire);
    remove_package_dir(dir);
    free(dir);

    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    return 0;
}
