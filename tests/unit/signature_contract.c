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

static bool type_con_named(const IdmTypeTerm *term, const char *name) {
    return term && term->kind == IDM_TYPE_CON && term->name && strcmp(term->name, name) == 0;
}

static bool type_var_named(const IdmTypeTerm *term, const char *name) {
    return term && term->kind == IDM_TYPE_VAR && term->name && strcmp(term->name, name) == 0;
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
        "spec showish :: Show a => a -> string\n"
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
    check(strcmp(showish->contract.context[0].trait, "Show") == 0, "showish trait");
    check(type_var_named(&showish->contract.context[0].lhs, "a"), "showish constraint arg");
    check(showish->contract.sigs[0].arg_count == 1u && showish->contract.sigs[0].has_result, "showish contract shape");
    check(type_var_named(&showish->contract.sigs[0].args[0], "a"), "showish arg var");
    check(type_con_named(&showish->contract.sigs[0].result, "string"), "showish result string");

    const IdmPkgSlot *sumish = find_slot(&art, "sumish");
    check(sumish && sumish->has_contract, "sumish slot contract");
    check(sumish->contract.context_count == 1u, "sumish context");
    check(sumish->contract.context[0].trait && strcmp(sumish->contract.context[0].trait, "Number") != 0, "sumish trait identity");
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
    check(type_con_named(&overt_int->contract.sigs[0].result, num->identity), "overt-int result Num");

    const IdmPkgSlot *overt_float = find_slot(&art, "overt-float");
    check(overt_float && overt_float->has_contract, "overt-float slot contract");
    check(type_con_named(&overt_float->contract.sigs[0].result, num->identity), "overt-float result Num");

    const IdmPkgType *maybe_int = find_type(&art, "MaybeInt");
    const IdmPkgType *some_int = find_type(&art, "SomeInt");
    const IdmPkgType *no_int = find_type(&art, "NoInt");
    check(maybe_int && some_int && no_int, "MaybeInt record member types");
    check(maybe_int->member_count == 2u, "MaybeInt overtype members");
    check(type_con_named(&maybe_int->members[0].term, some_int->identity), "MaybeInt member SomeInt");
    check(type_con_named(&maybe_int->members[1].term, no_int->identity), "MaybeInt member NoInt");

    const IdmPkgSlot *some = find_slot(&art, "some");
    check(some && some->has_contract, "some slot contract");
    check(type_con_named(&some->contract.sigs[0].result, maybe_int->identity), "some result MaybeInt");

    const IdmPkgSlot *none = find_slot(&art, "none");
    check(none && none->has_contract, "none slot contract");
    check(type_con_named(&none->contract.sigs[0].result, maybe_int->identity), "none result MaybeInt");

    const IdmPkgSlot *marker_int = find_slot(&art, "marker-int");
    check(marker_int && marker_int->has_contract, "marker-int slot contract");
    check(type_con_named(&marker_int->contract.sigs[0].result, "int"), "marker-int result int");

    const IdmPkgTrait *marker_trait = find_trait(&art, "Marker");
    check(marker_trait && marker_trait->method_count == 1u, "Marker trait artifact");
    check(marker_trait->methods[0].has_contract, "Marker method contract");
    check(marker_trait->methods[0].contract.context_count == 1u, "Marker method context");
    check(marker_trait->methods[0].contract.context[0].trait &&
          strcmp(marker_trait->methods[0].contract.context[0].trait, marker_trait->identity) == 0,
          "Marker method context identity");

    const IdmPkgSlot *box_ctor = find_slot(&art, "Box");
    check(box_ctor && box_ctor->has_contract, "Box constructor contract");
    check(box_ctor->contract.sigs[0].arg_count == 1u && box_ctor->contract.sigs[0].has_result, "Box constructor contract shape");
    check(box_ctor->contract.sigs[0].args[0].kind == IDM_TYPE_UNION && box_ctor->contract.sigs[0].args[0].arg_count == 2u, "Box constructor arg union");

    const IdmPkgSlot *box_pred = find_slot(&art, "Box?");
    check(box_pred && box_pred->has_contract, "Box predicate contract");
    check(box_pred->contract.sigs[0].arg_count == 1u && box_pred->contract.sigs[0].has_result, "Box predicate contract shape");
    check(type_con_named(&box_pred->contract.sigs[0].result, "atom"), "Box predicate result atom");

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

    const char *dead_source =
        "defn keepy x -> do\n"
        "  x\n"
        "  \"discard\"\n"
        "  mul x 2\n"
        "end\n"
        "keepy 4\n";
    IdmCore *dead_core = NULL;
    check_ok(idm_expand_source_string(&rt, "<dead>", dead_source, &dead_core, &err), &err, "dead expand");
    check_ok(idm_core_normalize(&rt, &dead_core, &err), &err, "dead normalize");
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
    check_ok(idm_core_normalize(&rt, &dc_core, &err), &err, "dead call normalize");
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
    check(dub->contract.context[0].trait && strncmp(dub->contract.context[0].trait, "Number", 6u) == 0, "dub inferred Number");
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
    dir = write_package_dir(purity_source, &err);
    idm_buf_init(&wire);
    check_ok(idm_expand_package_artifact_serialize(&rt, dir, &wire, &err), &err, "purity package compiles");
    IdmArtifact pur_art;
    check_ok(idm_artifact_deserialize(&rt, (const unsigned char *)(wire.data ? wire.data : ""), wire.len, &pur_art, &err), &err, "purity artifact");
    const IdmPkgSlot *calm = find_slot(&pur_art, "calm");
    check(calm && calm->has_contract && calm->contract.purity == IDM_PURITY_PURE, "calm is pure");
    const IdmPkgSlot *chatty = find_slot(&pur_art, "chatty");
    check(chatty && chatty->has_contract && chatty->contract.purity == IDM_PURITY_IMPURE, "chatty is impure");
    const IdmPkgSlot *relay = find_slot(&pur_art, "relay");
    check(relay && relay->has_contract && relay->contract.purity == IDM_PURITY_IMPURE, "relay impurity is transitive");
    const IdmPkgSlot *croaky = find_slot(&pur_art, "croaky");
    check(croaky && croaky->has_contract && croaky->contract.purity == IDM_PURITY_IMPURE, "croaky crash home is impure");
    const IdmPkgSlot *stacked = find_slot(&pur_art, "stacked");
    check(stacked && stacked->has_contract && stacked->contract.purity == IDM_PURITY_PURE, "stacked purity is transitive");
    const IdmPkgSlot *twice = find_slot(&pur_art, "twice");
    check(twice && twice->has_contract && twice->contract.purity == IDM_PURITY_ARGS, "twice is pure-if-args-pure");
    const IdmPkgSlot *resolved = find_slot(&pur_art, "resolved");
    check(resolved && resolved->has_contract && resolved->contract.purity >= IDM_PURITY_ARGS, "resolved higher-order call is not impure");
    const IdmPkgSlot *tainted = find_slot(&pur_art, "tainted");
    check(tainted && tainted->has_contract && tainted->contract.purity == IDM_PURITY_IMPURE, "tainted higher-order call is impure");
    const IdmPkgSlot *mapish = find_slot(&pur_art, "mapish");
    check(mapish && mapish->has_contract && mapish->contract.purity == IDM_PURITY_ARGS, "mapish is args-pure");
    check(mapish->contract.sig_count && (mapish->contract.sigs[0].invoked_mask & 1ull) == 0 && (mapish->contract.sigs[0].invoked_mask & 2ull) != 0, "mapish invokes f not xs");
    const IdmPkgSlot *doubled = find_slot(&pur_art, "doubled");
    check(doubled && doubled->has_contract && doubled->contract.purity == IDM_PURITY_PURE, "doubled with pure lambda is fully pure");
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
