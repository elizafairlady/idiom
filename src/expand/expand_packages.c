#include "internal.h"


static void expand_cache_destroy(void *p);
static ExpandCache *expand_cache_get(IdmRuntime *rt);
static ExpandCache *kernel_artifact_get(IdmRuntime *rt, IdmError *err);
static bool source_reader_get(IdmRuntime *rt, const IdmReaderArtifact **out, IdmError *err);
static bool ctx_record_dep(ExpandContext *ctx, const char *path, const unsigned char hash[32], uint8_t kind, IdmError *err);
static bool artifact_record_consumer_deps(ExpandContext *ctx, const char *path, const IdmArtifact *art, IdmError *err);
static bool artifact_intern_literals_recursive(IdmRuntime *rt, IdmArtifact *art, IdmError *err);
static IdmModuleRef *relocated_module_ref(ExpandContext *ctx, IdmModuleRef *src, IdmScopeId min_id, int64_t delta, IdmError *err);
static bool install_macro_twin(ExpandContext *ctx, const char *name, IdmScopeId base, size_t macros_before, IdmError *err);
static bool install_relocated_operator(ExpandContext *ctx, const IdmOperatorDef *op, IdmScopeId min_id, int64_t delta, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, IdmError *err);
static bool install_artifact_protocol(ExpandContext *ctx, const IdmPkgProtocol *entry, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err);
static bool install_artifact_trait(ExpandContext *ctx, const IdmPkgTrait *entry, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err);
static bool install_artifact_type(ExpandContext *ctx, const IdmPkgType *entry, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err);
static bool install_artifact_method_impls(ExpandContext *ctx, const IdmPkgMethodImpl *impls, size_t count, const char *provider_key, IdmError *err);
static bool pkg_grammar_copy(IdmPkgGrammar *dst, const IdmPkgGrammar *src, IdmError *err, IdmSpan span);
static const char *idm_std_root(void);
static const char *canonical_primitive_home_for_unit(const char *unit);
static bool compile_package_artifact(IdmRuntime *rt, IdmScopeStore *store, const IdmSyntax *pkg, const char *unit_name_hint, bool kernel_mode, const unsigned char src_hash[32], const char *inherited_env_key, const IdmPkgSlot *inherited_slots, size_t inherited_count, IdmArtifact *out, IdmError *err);

static bool install_inherited_package_slot_refs(ExpandContext *ctx, const char *env_key, const IdmPkgSlot *slots, size_t count, IdmError *err) {
    if (count == 0) return true;
    if (!env_key) return idm_error_set(err, idm_span_unknown(NULL), "inherited package slots require a provider key");
    for (size_t i = 0; i < count; i++) {
        uint32_t ref_id = 0;
        if (!package_slot_ref_add(ctx, env_key, slots[i].slot, &ref_id) ||
            !idm_binding_table_add_with_arity(&ctx->bindings, slots[i].name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_PACKAGE_SLOT, &slots[i].scopes, ref_id, IDM_FRAME_ENV, slots[i].arity, NULL)) {
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    return true;
}

static bool use_selection_allows(UseSelection *selection, const char *name) {
    if (!selection || selection->count == 0) return true;
    for (size_t i = 0; i < selection->count; i++) {
        if (strcmp(selection->names[i], name) == 0) {
            selection->matched[i] = true;
            return true;
        }
    }
    return false;
}

static bool use_selection_matched_all(const UseSelection *selection, const char *path, IdmError *err) {
    if (!selection || selection->count == 0) return true;
    for (size_t i = 0; i < selection->count; i++) {
        if (!selection->matched[i]) {
            return idm_error_set(err, selection->spans[i], "package '%s' does not export selected name '%s'", path, selection->names[i]);
        }
    }
    return true;
}

static bool install_artifact_typed_entities(ExpandContext *ctx, const IdmArtifact *art, UseSelection *selection, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err) {
    for (size_t i = 0; i < art->typed.entity_count; i++) {
        const IdmPkgTypedEntity *entity = &art->typed.entities[i];
        switch (entity->kind) {
            case IDM_TYPED_ENTITY_TRAIT:
                if (use_selection_allows(selection, entity->as.trait.name) &&
                    !install_artifact_trait(ctx, &entity->as.trait, scopes, qualifier, provider, provider_key, err)) {
                    return false;
                }
                break;
            case IDM_TYPED_ENTITY_TYPE:
                if (use_selection_allows(selection, entity->as.type.name) &&
                    !install_artifact_type(ctx, &entity->as.type, scopes, qualifier, provider, provider_key, err)) {
                    return false;
                }
                break;
            case IDM_TYPED_ENTITY_PROTOCOL:
                if (use_selection_allows(selection, entity->as.protocol.name) &&
                    !install_artifact_protocol(ctx, &entity->as.protocol, scopes, qualifier, provider, provider_key, err)) {
                    return false;
                }
                break;
        }
    }
    return true;
}

static bool install_artifact_typed_registry(ExpandContext *ctx, const IdmArtifact *art, UseSelection *selection, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err) {
    return install_artifact_typed_entities(ctx, art, selection, scopes, qualifier, provider, provider_key, err) &&
           install_artifact_method_impls(ctx, art->typed.method_impls, art->typed.method_impl_count, provider_key, err);
}

static size_t phase_index(const ExpandContext *ctx) {
    return ctx->phase == 0 ? 0u : 1u;
}

static ArtifactRuntimeState *artifact_runtime_state(ExpandContext *ctx, const IdmArtifact *art) {
    if (!art) return NULL;
    for (size_t i = 0; i < ctx->artifact_base_count; i++) {
        if (memcmp(ctx->artifact_bases[i].hash, art->src_hash, 32u) == 0) return &ctx->artifact_bases[i];
    }
    return NULL;
}

static const ArtifactRuntimeState *artifact_runtime_state_const(const ExpandContext *ctx, const IdmArtifact *art) {
    if (!art) return NULL;
    for (size_t i = 0; i < ctx->artifact_base_count; i++) {
        if (memcmp(ctx->artifact_bases[i].hash, art->src_hash, 32u) == 0) return &ctx->artifact_bases[i];
    }
    return NULL;
}

static bool runtime_init_recorded(const ExpandContext *ctx, const IdmArtifact *art) {
    const ArtifactRuntimeState *state = artifact_runtime_state_const(ctx, art);
    if (!state) return false;
    return state->runtime_init_mark[phase_index(ctx)] != 0;
}

static bool record_runtime_init(ExpandContext *ctx, const IdmArtifact *art, IdmSpan span, IdmError *err) {
    if (runtime_init_recorded(ctx, art)) return true;
    ArtifactRuntimeState *state = artifact_runtime_state(ctx, art);
    if (!state) {
        IdmScopeId ignored = 0;
        if (!artifact_base(ctx, art, &ignored, err)) return false;
        state = artifact_runtime_state(ctx, art);
        if (!state) return idm_error_set(err, span, "package artifact state was not recorded");
    }
    state->runtime_init_mark[phase_index(ctx)] = ++ctx->runtime_init_mark_count;
    return true;
}

static bool bind_package_slot_ref(ExpandContext *ctx, const char *bind_name, int phase, const IdmScopeSet *scopes, IdmArity arity, const char *env_key, uint32_t source_slot, bool primitive_backed, IdmPrimitive primitive, IdmSpan span, IdmError *err) {
    uint32_t ref_id = 0;
    if (!package_slot_ref_add(ctx, env_key, source_slot, &ref_id)) return idm_error_oom(err, span);
    if (primitive_backed) {
        return idm_binding_table_add_primitive_with_arity(&ctx->bindings, bind_name, phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_PACKAGE_SLOT, scopes, ref_id, IDM_FRAME_ENV, arity, (uint32_t)primitive, NULL) ||
               idm_error_oom(err, span);
    }
    return idm_binding_table_add_with_arity(&ctx->bindings, bind_name, phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_PACKAGE_SLOT, scopes, ref_id, IDM_FRAME_ENV, arity, NULL) ||
           idm_error_oom(err, span);
}

bool install_artifact_runtime_slots(ExpandContext *ctx, const IdmArtifact *art, const char *primitive_home, IdmScopeId min_id, int64_t delta, bool once, IdmSpan span, IdmError *err) {
    if (!art || art->slot_count == 0) return true;
    if (once && runtime_init_recorded(ctx, art)) return true;
    char provider_key[17];
    artifact_provider_key(art->src_hash, provider_key);
    for (size_t i = 0; i < art->slot_count; i++) {
        const IdmPkgSlot *slot = &art->slots[i];
        IdmScopeSet scopes;
        idm_scope_set_init(&scopes);
        bool ok = idm_scope_set_copy(&scopes, &slot->scopes);
        if (ok) {
            idm_scope_set_relocate(&scopes, min_id, delta);
            IdmPrimitive primitive = 0;
            bool primitive_backed = idm_primitive_lookup(primitive_home, slot->name, &primitive);
            ok = bind_package_slot_ref(ctx, slot->name, primitive_backed ? IDM_PHASE_ANY : ctx->phase, &scopes, slot->arity, provider_key, slot->slot, primitive_backed, primitive, span, err);
        }
        idm_scope_set_destroy(&scopes);
        if (!ok) return false;
    }
    return !once || record_runtime_init(ctx, art, span, err);
}

IdmCore *wrap_artifact_runtime_init(ExpandContext *ctx, const IdmArtifact *art, IdmScopeId min_id, int64_t delta, bool borrow_module, bool allow_empty_module, IdmCore *body, IdmSpan span, IdmError *err) {
    if (!body) return NULL;
    uint32_t fn_off = 0;
    IdmBytecodeModule *module = NULL;
    bool module_owned = false;
    if (art && art->module) {
        if (borrow_module) {
            module = art->module;
        } else {
            module = relocated_module_copy(ctx, art->module, min_id, delta, &fn_off, err);
            if (!module) {
                idm_core_free(body);
                return NULL;
            }
            module_owned = true;
        }
    } else if (!allow_empty_module) {
        return body;
    }
    char package_key[17];
    artifact_provider_key(art->src_hash, package_key);
    IdmCore *wrapped = idm_core_use_package(idm_atom(ctx->rt, package_key), module, module_owned, art->init_fn + fn_off, body, span);
    if (!wrapped) {
        if (module_owned) {
            idm_bc_destroy(module);
            free(module);
        }
        idm_core_free(body);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    return wrapped;
}

IdmCore *expand_use(ExpandContext *ctx, const char *path, const char *qualifier, UseSelection *selection, IdmSyntax *const *items, size_t cont_index, size_t cont_count, IdmSpan span, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "expand.use");
    const IdmArtifact *art = artifact_get(ctx, path, span, err);
    if (!art) {
        if (err && err->present && span.line != 0) {
            idm_error_note(err, "while loading package '%s' (%s:%u:%u)", path, span.file ? span.file : "<unknown>", span.line, span.column);
        } else if (err && err->present) {
            idm_error_note(err, "while loading package '%s'", path);
        }
        idm_profile_scope_end(&prof);
        return NULL;
    }
    IdmScopeId base = 0;
    if (!artifact_base(ctx, art, &base, err)) {
        idm_profile_scope_end(&prof);
        return NULL;
    }
    IdmScopeId min_id = art->scope_base;
    int64_t delta = (int64_t)base - (int64_t)art->scope_base;
    IdmScopeSet act_scopes;
    if (!binder_scopes_pruned(ctx, items[cont_index - 1u], &act_scopes)) {
        idm_profile_scope_end(&prof);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    const char *provider = art->name ? art->name : path;
    const char *primitive_home = canonical_primitive_home_for_unit(path);
    char provider_key[17];
    artifact_provider_key(art->src_hash, provider_key);

    bool ok = true;
    for (size_t i = 0; i < art->slot_count && ok; i++) {
        if (!art->slots[i].exported) continue;
        if (!use_selection_allows(selection, art->slots[i].name)) continue;
        const char *bind_name = art->slots[i].name;
        char *qualified = NULL;
        if (!artifact_bind_name(qualifier, art->slots[i].name, &bind_name, &qualified, err, span)) { ok = false; break; }
        IdmPrimitive primitive = 0;
        bool primitive_backed = idm_primitive_lookup(primitive_home, art->slots[i].name, &primitive);
        int phase = primitive_backed ? IDM_PHASE_ANY : ctx->phase;
        ok = bind_package_slot_ref(ctx, bind_name, phase, &act_scopes, art->slots[i].arity, provider_key, art->slots[i].slot, primitive_backed, primitive, span, err);
        free(qualified);
    }
    for (size_t i = 0; qualifier && i < art->macro_count && ok; i++) {
        if (!use_selection_allows(selection, art->macros[i].name)) continue;
        const char *bind_name = art->macros[i].name;
        char *qualified = NULL;
        if (!artifact_bind_name(qualifier, art->macros[i].name, &bind_name, &qualified, err, span)) { ok = false; break; }
        IdmModuleRef *module = relocated_module_ref(ctx, art->macros[i].module, min_id, delta, err);
        if (!module) {
            free(qualified);
            ok = false;
            break;
        }
        size_t before = ctx->macro_count;
        ok = install_imported_macro(ctx, bind_name, &act_scopes, module, art->macros[i].function_index, art->macros[i].phase_env, provider, provider_key, err) &&
             install_macro_twin(ctx, art->macros[i].name, base, before, err);
        idm_module_ref_release(module);
        free(qualified);
    }
    if (ok) ok = install_artifact_typed_registry(ctx, art, selection, &act_scopes, qualifier, provider, provider_key, err);
    if (ok) ok = use_selection_matched_all(selection, path, err);
    if (ok) ok = install_artifact_runtime_slots(ctx, art, primitive_home, min_id, delta, true, span, err);
    idm_scope_set_destroy(&act_scopes);
    if (!ok) {
        if (err && !err->present) idm_error_oom(err, span);
        idm_profile_scope_end(&prof);
        return NULL;
    }

    IdmCore *refresh = build_method_dispatch_refresh_core(ctx, span, err);
    if (!refresh && err && err->present) {
        idm_profile_scope_end(&prof);
        return NULL;
    }
    bool init_pending = artifact_init_pending(ctx, art);
    IdmCore *cont = expand_body_items(ctx, items, cont_index, cont_count, false, err);
    if (!cont) {
        idm_core_free(refresh);
        idm_profile_scope_end(&prof);
        return NULL;
    }
    if (refresh) {
        IdmCore *seq = idm_core_do(span);
        if (!seq || !idm_core_do_add(seq, refresh) || !idm_core_do_add(seq, cont)) {
            idm_core_free(seq);
            idm_core_free(refresh);
            idm_core_free(cont);
            idm_profile_scope_end(&prof);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
        refresh = NULL;
        cont = seq;
    }

    if (init_pending && art->module) {
        idm_profile_count(delta == 0 ? "expand.use.borrowed_runtime_init" : "expand.use.borrowed_runtime_init_relocated_surface", 1u);
    }
    if (!init_pending || !art->module) {
        idm_profile_count("expand.use.no_runtime_init", 1u);
        idm_profile_scope_end(&prof);
        return cont;
    }
    IdmCore *use_core = wrap_artifact_runtime_init(ctx, art, min_id, delta, true, false, cont, span, err);
    if (!use_core) {
        idm_profile_scope_end(&prof);
        return NULL;
    }
    idm_profile_count("expand.use.runtime_init", 1u);
    idm_profile_scope_end(&prof);
    return use_core;
}

static void expand_cache_destroy(void *p) {
    ExpandCache *cache = p;
    if (!cache) return;
    if (cache->kernel_ready) {
        idm_artifact_destroy(&cache->kernel);
    }
    for (size_t i = 0; i < cache->pkg_count; i++) {
        idm_artifact_destroy(&cache->pkgs[i]->art);
        free(cache->pkgs[i]->path);
        free(cache->pkgs[i]);
    }
    free(cache->pkgs);
    idm_reader_artifact_destroy(cache->source_reader);
    free(cache->kernel_path);
    free(cache->compiling);
    free(cache);
}

static ExpandCache *expand_cache_get(IdmRuntime *rt) {
    if (!rt->expand_cache) {
        ExpandCache *cache = calloc(1u, sizeof(*cache));
        if (!cache) return NULL;
        cache->rt = rt;
        rt->expand_cache = cache;
        rt->expand_cache_free = expand_cache_destroy;
    }
    return rt->expand_cache;
}

static bool bootstrap_word_is(const IdmSyntax *syn, const char *text) {
    return syn && syn->kind == IDM_SYN_WORD && strcmp(syn->as.text, text) == 0;
}

static bool copy_bootstrap_form_slice(const IdmSyntax *terms, size_t start, size_t count, IdmSyntax **out, IdmError *err) {
    *out = NULL;
    if (!terms || terms->kind != IDM_SYN_LIST) {
        return idm_error_set(err, terms ? terms->span : idm_span_unknown(NULL), "bootstrap source must read as lower terms");
    }
    if (start + count > terms->as.seq.count) {
        return idm_error_set(err, terms->span, "bootstrap declaration is incomplete");
    }
    IdmSyntax *copy = idm_syn_list(terms->as.seq.items[start]->span);
    if (!copy) return idm_error_oom(err, terms->as.seq.items[start]->span);
    for (size_t j = 0; j < count; j++) {
        IdmSyntax *item = idm_syn_clone(terms->as.seq.items[start + j]);
        if (!item || !idm_syn_append(copy, item)) {
            idm_syn_free(item);
            idm_syn_free(copy);
            return idm_error_oom(err, terms->as.seq.items[start]->span);
        }
    }
    *out = copy;
    return true;
}

static void bootstrap_grammars_destroy(IdmPkgGrammar *grammars, size_t count) {
    for (size_t i = 0; i < count; i++) idm_pkg_grammar_destroy(&grammars[i]);
    free(grammars);
}

static bool bootstrap_grammar_append(IdmPkgGrammar **grammars, size_t *count, size_t *cap, IdmPkgGrammar *grammar, IdmError *err, IdmSpan span) {
    if (*count == *cap) {
        if (!idm_grow((void **)grammars, cap, sizeof(**grammars), 4u, *count + 1u)) return idm_error_oom(err, span);
    }
    (*grammars)[*count] = *grammar;
    memset(grammar, 0, sizeof(*grammar));
    (*count)++;
    return true;
}

static bool bootstrap_source_reader_artifact(const IdmSyntax *terms, IdmReaderArtifact **out, IdmError *err) {
    *out = NULL;
    if (!terms || terms->kind != IDM_SYN_LIST) {
        return idm_error_set(err, terms ? terms->span : idm_span_unknown(NULL), "bootstrap source must read as lower terms");
    }
    char *surface = NULL;
    IdmSpan surface_span = terms->span;
    IdmPkgGrammar *grammars = NULL;
    size_t grammar_count = 0;
    size_t grammar_cap = 0;
    bool ok = true;
    for (size_t i = 0; i < terms->as.seq.count && ok; i++) {
        const IdmSyntax *head = terms->as.seq.items[i];
        if (bootstrap_word_is(head, "core-grammar")) {
            IdmSyntax *form = NULL;
            IdmPkgGrammar grammar;
            memset(&grammar, 0, sizeof(grammar));
            ok = copy_bootstrap_form_slice(terms, i, 4u, &form, err) &&
                 idm_pkg_grammar_from_ir(form, &grammar, err);
            idm_syn_free(form);
            if (ok) {
                ok = bootstrap_grammar_append(&grammars, &grammar_count, &grammar_cap, &grammar, err, head->span);
            }
            idm_pkg_grammar_destroy(&grammar);
            i += 3u;
        } else if (bootstrap_word_is(head, "core-reader")) {
            IdmSyntax *form = NULL;
            char *next = NULL;
            ok = copy_bootstrap_form_slice(terms, i, 3u, &form, err) &&
                 idm_pkg_source_reader_from_ir(form, &next, err);
            idm_syn_free(form);
            if (ok) {
                free(surface);
                surface = next;
                surface_span = terms->as.seq.items[i + 2u]->span;
            }
            i += 2u;
        } else {
            ok = idm_error_set(err, head ? head->span : terms->span, "bootstrap declaration must be core-grammar or core-reader");
        }
    }
    if (ok && !surface) {
        ok = idm_error_set(err, terms->span, "bootstrap core-reader :source declaration not found");
    }
    bool selected_found = false;
    for (size_t i = 0; ok && i < grammar_count; i++) {
        if (grammars[i].name && strcmp(grammars[i].name, surface) == 0) {
            selected_found = true;
            break;
        }
    }
    if (ok && !selected_found) {
        ok = idm_error_set(err, surface_span, "core-reader source reader '%s' has no core-grammar declaration", surface);
    }
    if (ok) ok = idm_reader_artifact_from_grammars(surface, grammars, grammar_count, out, err);
    free(surface);
    bootstrap_grammars_destroy(grammars, grammar_count);
    return ok;
}

static bool bootstrap_path(IdmBuffer *path, IdmError *err) {
    idm_buf_init(path);
    const char *root = idm_std_root();
    if (!idm_buf_append(path, root) || !idm_buf_append(path, "/kernel/bootstrap.id")) {
        idm_buf_destroy(path);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (access(path->data, R_OK) == 0 || strcmp(root, "std") == 0) return true;
    idm_buf_destroy(path);
    idm_buf_init(path);
    if (!idm_buf_append(path, "std/kernel/bootstrap.id")) {
        idm_buf_destroy(path);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    return true;
}

static bool source_reader_get(IdmRuntime *rt, const IdmReaderArtifact **out, IdmError *err) {
    *out = NULL;
    ExpandCache *cache = expand_cache_get(rt);
    if (!cache) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!cache->source_reader) {
        IdmBuffer path;
        if (!bootstrap_path(&path, err)) return false;
        IdmSyntax *terms = NULL;
        bool read = idm_reader_read_terms_file(path.data, &terms, err);
        idm_buf_destroy(&path);
        if (!read) return false;
        bool ok = bootstrap_source_reader_artifact(terms, &cache->source_reader, err);
        idm_syn_free(terms);
        if (!ok) return false;
    }
    *out = cache->source_reader;
    return true;
}

static bool source_reader_hash_get(IdmRuntime *rt, unsigned char out[32], IdmError *err) {
    ExpandCache *cache = expand_cache_get(rt);
    if (!cache) return idm_error_oom(err, idm_span_unknown(NULL));
    const IdmReaderArtifact *reader = NULL;
    if (!source_reader_get(rt, &reader, err)) return false;
    if (!cache->source_reader_hash_ready) {
        IdmBuffer wire;
        idm_buf_init(&wire);
        if (!idm_reader_artifact_serialize(reader, &wire, err)) {
            idm_buf_destroy(&wire);
            return false;
        }
        idm_sha256(wire.data ? wire.data : "", wire.len, cache->source_reader_hash);
        idm_buf_destroy(&wire);
        cache->source_reader_hash_ready = true;
    }
    memcpy(out, cache->source_reader_hash, 32u);
    return true;
}

bool idm_expand_source_reader(IdmRuntime *rt, const IdmReaderArtifact **out, IdmError *err) {
    return source_reader_get(rt, out, err);
}

bool idm_expand_read_source_string(IdmRuntime *rt, const char *file, const char *source, IdmSyntax **out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "expand.read_source_string");
    *out = NULL;
    const IdmReaderArtifact *reader = NULL;
    if (!source_reader_get(rt, &reader, err)) {
        idm_profile_scope_end(&prof);
        return false;
    }
    bool ok = idm_reader_read_artifact_string(reader, file, source, out, err);
    if (ok && source) idm_profile_count("expand.read_source_string.bytes", (uint64_t)strlen(source));
    idm_profile_scope_end(&prof);
    return ok;
}

static bool source_cache_hash(IdmRuntime *rt, const char *source, size_t len, unsigned char out[32], IdmError *err) {
    unsigned char reader_hash[32];
    if (!source_reader_hash_get(rt, reader_hash, err)) return false;
    IdmBuffer data;
    idm_buf_init(&data);
    bool ok = idm_buf_append(&data, "IDM-SOURCE-CACHE-v3") &&
              idm_buf_append_n(&data, (const char *)reader_hash, 32u) &&
              idm_buf_append_n(&data, source ? source : "", len);
    if (!ok) {
        idm_buf_destroy(&data);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    idm_sha256(data.data ? data.data : "", data.len, out);
    idm_buf_destroy(&data);
    return true;
}

static bool package_dep_verified(IdmRuntime *rt, const char *path, const unsigned char want[32], void *user) {
    (void)user;
    IdmBuffer src;
    idm_buf_init(&src);
    const char *label = NULL;
    IdmError err;
    idm_error_init(&err);
    bool ok = idm_package_read_source(rt, path, &src, &label, idm_span_unknown(NULL), &err);
    unsigned char got[32];
    if (ok) ok = source_cache_hash(rt, src.data ? src.data : "", src.len, got, &err);
    idm_buf_destroy(&src);
    idm_error_clear(&err);
    return ok && memcmp(got, want, 32u) == 0;
}

static ExpandCache *kernel_artifact_get(IdmRuntime *rt, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "expand.kernel_artifact_get");
    ExpandCache *cache = expand_cache_get(rt);
    if (!cache) {
        idm_error_oom(err, idm_span_unknown(NULL));
        idm_profile_scope_end(&prof);
        return NULL;
    }
    if (cache->kernel_ready) {
        idm_profile_count("expand.kernel_artifact.memory_hit", 1u);
        idm_profile_scope_end(&prof);
        return cache;
    }
    IdmBuffer path;
    idm_buf_init(&path);
    if (!idm_buf_append(&path, idm_std_root()) || !idm_buf_append(&path, "/kernel")) {
        idm_buf_destroy(&path);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    IdmBuffer src;
    idm_buf_init(&src);
    const char *label = NULL;
    if (!idm_package_read_source(rt, path.data, &src, &label, idm_span_unknown(NULL), err)) {
        idm_buf_destroy(&src);
        idm_buf_destroy(&path);
        return NULL;
    }
    unsigned char src_hash[32];
    if (!source_cache_hash(rt, src.data ? src.data : "", src.len, src_hash, err)) {
        idm_buf_destroy(&src);
        idm_buf_destroy(&path);
        return NULL;
    }
    cache->kernel_path = idm_buf_take(&path);
    if (!cache->kernel_path) {
        idm_buf_destroy(&src);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    if (idm_artifact_cache_load(rt, cache->kernel_path, src_hash, &cache->kernel, package_dep_verified, NULL)) {
        idm_buf_destroy(&src);
        cache->kernel_scope_end = cache->kernel.scope_end;
        cache->kernel_ready = true;
        idm_profile_count("expand.kernel_artifact.cache_hit", 1u);
        idm_profile_scope_end(&prof);
        return cache;
    }
    IdmSyntax *pkg = NULL;
    bool read_ok = idm_expand_read_source_string(rt, label, src.data ? src.data : "", &pkg, err);
    idm_buf_destroy(&src);
    if (!read_ok) return NULL;
    if (!syn_is_form(pkg, "%-package-begin")) {
        idm_syn_free(pkg);
        idm_error_set(err, idm_span_unknown(NULL), "kernel source must be a program");
        return NULL;
    }
    IdmScopeStore store;
    idm_scope_store_init(&store);
    IdmScopeId base = store.next_scope;
    bool compiled = compile_package_artifact(rt, &store, pkg, "std/kernel", true, src_hash, NULL, NULL, 0u, &cache->kernel, err);
    idm_syn_free(pkg);
    if (!compiled) return NULL;
    cache->kernel.scope_base = base;
    cache->kernel.scope_end = store.next_scope;
    memcpy(cache->kernel.src_hash, src_hash, 32u);
    cache->kernel_scope_end = store.next_scope;
    if (!artifact_intern_literals_recursive(rt, &cache->kernel, err)) {
        idm_artifact_destroy(&cache->kernel);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    idm_artifact_cache_write(cache->kernel_path, &cache->kernel);
    cache->kernel_ready = true;
    idm_profile_count("expand.kernel_artifact.compiled", 1u);
    idm_profile_scope_end(&prof);
    return cache;
}

static bool seed_kernel_phase_env(ExpandContext *ctx, IdmError *err) {
    if (!ctx->kernel_wrap || ctx->kernel_phase_seeded || !ctx->phase_env) return true;
    IdmCore *body = idm_core_literal(idm_nil(), idm_span_unknown(NULL));
    IdmCore *init = body ? wrap_kernel_use(ctx, body, err) : NULL;
    if (!init) {
        if (!body && err && !err->present) idm_error_oom(err, idm_span_unknown(NULL));
        return false;
    }
    bool ok = run_phase_core(ctx, init, err);
    idm_core_free(init);
    if (ok) ctx->kernel_phase_seeded = true;
    return ok;
}

bool ctx_activate_kernel(ExpandContext *ctx, IdmError *err) {
    ExpandCache *cache = kernel_artifact_get(ctx->rt, err);
    if (!cache) return false;
    IdmArtifact *k = &cache->kernel;
    if (!artifact_record_consumer_deps(ctx, cache->kernel_path, k, err)) return false;
    if (ctx->scope_store.next_scope < cache->kernel_scope_end) ctx->scope_store.next_scope = cache->kernel_scope_end;
    const char *kernel_name = k->name ? k->name : "std/kernel";
    char kernel_key[17];
    artifact_provider_key(k->src_hash, kernel_key);
    if (!record_activation(ctx, "std/kernel", kernel_name, kernel_key, idm_span_unknown(NULL), err)) return false;
    for (size_t i = 0; i < k->operator_count; i++) {
        if (!install_imported_operator(ctx, &k->operators[i], &ctx->empty_scopes, kernel_name, kernel_key, err)) return false;
    }
    for (size_t i = 0; i < k->macro_count; i++) {
        if (!install_imported_macro(ctx, k->macros[i].name, &ctx->empty_scopes, k->macros[i].module, k->macros[i].function_index, k->macros[i].phase_env, kernel_name, kernel_key, err)) return false;
    }
    for (size_t i = 0; i < k->core_syntax_count; i++) {
        IdmPkgCoreSyntax *core_syntax = &k->core_syntax[i];
        if (!install_imported_core_syntax(ctx, core_syntax->name, &ctx->empty_scopes, core_syntax->module, core_syntax->function_index, core_syntax->phase_env, kernel_name, kernel_key, err)) return false;
    }
    if (!install_artifact_grammars(ctx, k->grammars, k->grammar_count, &ctx->empty_scopes, k->scope_base, 0, kernel_name, kernel_key, err)) return false;
    if (!install_artifact_typed_registry(ctx, k, NULL, &ctx->empty_scopes, NULL, kernel_name, kernel_key, err)) return false;
    {
        for (size_t i = 0; i < k->slot_count; i++) {
            IdmPrimitive primitive = 0;
            bool primitive_backed = idm_primitive_lookup("kernel", k->slots[i].name, &primitive);
            const IdmScopeSet *bind_scopes = k->slots[i].exported ? &ctx->empty_scopes : &k->slots[i].scopes;
            if (!bind_package_slot_ref(ctx, k->slots[i].name, IDM_PHASE_ANY, bind_scopes, k->slots[i].arity, kernel_key, k->slots[i].slot, primitive_backed, primitive, idm_span_unknown(NULL), err)) return false;
        }
    }
    ctx->kernel_wrap = k->slot_count != 0;
    return seed_kernel_phase_env(ctx, err);
}

IdmCore *wrap_kernel_use(ExpandContext *ctx, IdmCore *body, IdmError *err) {
    if (!ctx->kernel_wrap || !body) return body;
    ExpandCache *cache = expand_cache_get(ctx->rt);
    IdmArtifact *k = &cache->kernel;
    if (!k->module) {
        idm_core_free(body);
        return NULL;
    }
    return wrap_artifact_runtime_init(ctx, k, k->scope_base, 0, true, false, body, idm_span_unknown(NULL), err);
}

static const ArtifactRuntimeState *artifact_runtime_state_by_mark(const ExpandContext *ctx, size_t mark) {
    if (mark == 0) return NULL;
    for (size_t i = 0; i < ctx->artifact_base_count; i++) {
        const ArtifactRuntimeState *state = &ctx->artifact_bases[i];
        if (state->runtime_init_mark[0] == mark || state->runtime_init_mark[1] == mark) return state;
    }
    return NULL;
}

IdmCore *wrap_phase_runtime_inits_since(ExpandContext *ctx, IdmCore *body, size_t first_mark, IdmError *err) {
    if (!body || ctx->runtime_init_mark_count == 0) return body;
    if (first_mark > ctx->runtime_init_mark_count) first_mark = ctx->runtime_init_mark_count;
    for (size_t mark = ctx->runtime_init_mark_count; mark > first_mark; mark--) {
        const ArtifactRuntimeState *state = artifact_runtime_state_by_mark(ctx, mark);
        const IdmArtifact *art = state ? state->art : NULL;
        if (!art || !artifact_init_pending(ctx, art) || !art->module) continue;
        body = wrap_artifact_runtime_init(ctx, art, art->scope_base, 0, false, false, body, idm_span_unknown(NULL), err);
        if (!body) return NULL;
    }
    return body;
}

IdmCore *wrap_phase_runtime_inits(ExpandContext *ctx, IdmCore *body, IdmError *err) {
    return wrap_phase_runtime_inits_since(ctx, body, 0, err);
}

void artifact_provider_key(const unsigned char hash[32], char out[17]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 8u; i++) {
        out[i * 2u] = hex[hash[i] >> 4];
        out[i * 2u + 1u] = hex[hash[i] & 0xfu];
    }
    out[16] = '\0';
}

static bool artifact_clone(IdmRuntime *rt, const IdmArtifact *src, IdmArtifact *dst, IdmError *err) {
    IdmBuffer blob;
    idm_buf_init(&blob);
    bool ok = idm_artifact_serialize(src, &blob, err);
    if (ok) ok = idm_artifact_deserialize(rt, (const unsigned char *)(blob.data ? blob.data : ""), blob.len, dst, err);
    if (ok) ok = idm_artifact_run_phase_inits(rt, dst, err);
    idm_buf_destroy(&blob);
    return ok;
}

static bool module_ref_intern_literals(IdmRuntime *rt, IdmModuleRef *ref, IdmError *err) {
    return !ref || idm_bc_intern_literals(rt, &ref->module, err);
}

static bool artifact_intern_literals_recursive(IdmRuntime *rt, IdmArtifact *art, IdmError *err) {
    if (!art) return true;
    if (art->module && !idm_bc_intern_literals(rt, art->module, err)) return false;
    for (size_t i = 0; i < art->macro_count; i++) {
        if (!module_ref_intern_literals(rt, art->macros[i].module, err)) return false;
    }
    for (size_t i = 0; i < art->operator_count; i++) {
        if (!module_ref_intern_literals(rt, art->operators[i].target_module, err)) return false;
    }
    for (size_t i = 0; i < art->core_syntax_count; i++) {
        if (!module_ref_intern_literals(rt, art->core_syntax[i].module, err)) return false;
    }
    for (size_t i = 0; i < art->typed.entity_count; i++) {
        const IdmPkgTypedEntity *entity = &art->typed.entities[i];
        if (entity->kind == IDM_TYPED_ENTITY_PROTOCOL && !artifact_intern_literals_recursive(rt, entity->as.protocol.art, err)) return false;
    }
    return true;
}

static bool install_artifact_protocol(ExpandContext *ctx, const IdmPkgProtocol *entry, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err) {
    if (!scopes) scopes = &ctx->empty_scopes;
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    char *qualified = NULL;
    const char *bind_name = entry->name;
    if (!artifact_bind_name(qualifier, entry->name, &bind_name, &qualified, err, idm_span_unknown(NULL))) return false;
    int guard = surface_install_guard(ctx, provider, provider_key, bind_name, bind_name, IDM_BIND_SPACE_PROTOCOL, scopes, err);
    if (guard < 0) {
        surface_rollback(ctx, &checkpoint);
        free(qualified);
        return false;
    }
    if (guard == 0) {
        free(qualified);
        return true;
    }
    uint32_t payload = 0;
    ProtocolDef *p = typed_registry_add_protocol(ctx, &payload, err, idm_span_unknown(NULL));
    if (!p) {
        surface_rollback(ctx, &checkpoint);
        free(qualified);
        return false;
    }
    p->name = idm_strdup(bind_name);
    p->art = calloc(1u, sizeof(*p->art));
    if (!p->name || !protocol_def_set_identity(ctx, p, entry->identity, err, idm_span_unknown(NULL)) || !entry->art || !p->art || !artifact_clone(ctx->rt, entry->art, p->art, err)) {
        protocol_def_destroy(p);
        surface_rollback(ctx, &checkpoint);
        free(qualified);
        if (err && err->present) return false;
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (!idm_binding_table_add(&ctx->bindings, bind_name, IDM_PHASE_ANY, IDM_BIND_SPACE_PROTOCOL, IDM_BIND_PROTOCOL, scopes, payload, ctx->frame, NULL)) {
        surface_rollback(ctx, &checkpoint);
        free(qualified);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    free(qualified);
    return true;
}

static bool install_artifact_trait(ExpandContext *ctx, const IdmPkgTrait *entry, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err) {
    if (!scopes) scopes = &ctx->empty_scopes;
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    char *qualified = NULL;
    const char *bind_name = entry->name;
    if (!artifact_bind_name(qualifier, entry->name, &bind_name, &qualified, err, idm_span_unknown(NULL))) return false;
    int guard = surface_install_guard(ctx, provider, provider_key, bind_name, bind_name, IDM_BIND_SPACE_TRAIT, scopes, err);
    if (guard < 0) {
        surface_rollback(ctx, &checkpoint);
        free(qualified);
        return false;
    }
    if (guard == 0) {
        free(qualified);
        return true;
    }
    uint32_t payload = 0;
    TraitDef *t = typed_registry_add_trait(ctx, &payload, err, idm_span_unknown(NULL));
    if (!t) {
        surface_rollback(ctx, &checkpoint);
        free(qualified);
        return false;
    }
    if (!trait_def_set_identity(ctx, t, entry->identity, err, idm_span_unknown(NULL)) ||
        !idm_trait_requirement_defs_copy(entry->requirements, entry->requirement_count, &t->requirements) ||
        !trait_method_defs_import(ctx, entry->methods, entry->method_count, &t->methods, err, idm_span_unknown(NULL))) {
        trait_def_destroy(t);
        surface_rollback(ctx, &checkpoint);
        free(qualified);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    t->requirement_count = entry->requirement_count;
    t->method_count = entry->method_count;
    t->dispatch_env = true;
    t->dispatch_env_key = idm_strdup(provider_key ? provider_key : "");
    if (!t->dispatch_env_key) {
        trait_def_destroy(t);
        surface_rollback(ctx, &checkpoint);
        free(qualified);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (!idm_binding_table_add(&ctx->bindings, bind_name, IDM_PHASE_ANY, IDM_BIND_SPACE_TRAIT, IDM_BIND_TRAIT, scopes, payload, ctx->frame, NULL)) {
        surface_rollback(ctx, &checkpoint);
        free(qualified);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t m = 0; m < entry->method_count; m++) {
        const TraitMethodDef *method = &t->methods[m];
        if (!install_method_surface(ctx, entry->identity, bind_name, trait_method_name_text(method), method->arity, scopes, provider, provider_key, method->has_dispatch, true, provider_key, method->dispatch_slot, err)) {
            surface_rollback(ctx, &checkpoint);
            free(qualified);
            return false;
        }
    }
    free(qualified);
    return true;
}

static bool install_artifact_method_impls(ExpandContext *ctx, const IdmPkgMethodImpl *impls, size_t count, const char *provider_key, IdmError *err) {
    for (size_t i = 0; i < count; i++) {
        const IdmPkgMethodImpl *src = &impls[i];
        const char *key = (src->impl_env && (!src->impl_env_key || !src->impl_env_key[0])) ? provider_key : src->impl_env_key;
        uint32_t method_surface = 0;
        if (!method_surface_index_by_identity(ctx, src->trait, src->method, &method_surface)) continue;
        const MethodSurfaceDef *surface = method_surface_by_index(ctx, method_surface);
        if (!surface) return idm_error_set(err, idm_span_unknown(NULL), "method implementation references missing surface");
        bool updated = false;
        for (size_t j = 0; j < ctx->typed.method_impl_count; j++) {
            MethodImplDef *dst = &ctx->typed.method_impls[j];
            if (!method_impl_matches_identity(ctx, dst, method_surface_trait_text(surface), method_surface_name_text(surface), src->type)) continue;
            char *next_key = idm_strdup(key ? key : "");
            if (!next_key) return idm_error_oom(err, idm_span_unknown(NULL));
            dst->arity = src->arity;
            dst->impl_env = src->impl_env;
            dst->impl_slot = src->impl_slot;
            free(dst->impl_env_key);
            dst->impl_env_key = next_key;
            updated = true;
            break;
        }
        if (updated) continue;
        if (ctx->typed.method_impl_count == ctx->typed.method_impl_cap) {
            if (!idm_grow((void **)&ctx->typed.method_impls, &ctx->typed.method_impl_cap, sizeof(*ctx->typed.method_impls), 8u, ctx->typed.method_impl_count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
        }
        MethodImplDef *dst = &ctx->typed.method_impls[ctx->typed.method_impl_count];
        memset(dst, 0, sizeof(*dst));
        dst->method_surface = method_surface;
        dst->arity = src->arity;
        dst->impl_env = src->impl_env;
        dst->impl_env_key = idm_strdup(key ? key : "");
        dst->impl_slot = src->impl_slot;
        if (!dst->impl_env_key || !method_impl_set_type(ctx, dst, src->type, err, idm_span_unknown(NULL))) {
            free(dst->impl_env_key);
            memset(dst, 0, sizeof(*dst));
            return false;
        }
        ctx->typed.method_impl_count++;
    }
    return true;
}

static bool install_artifact_type(ExpandContext *ctx, const IdmPkgType *entry, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err) {
    if (!scopes) scopes = &ctx->empty_scopes;
    char *qualified = NULL;
    const char *bind_name = entry->name;
    if (!artifact_bind_name(qualifier, entry->name, &bind_name, &qualified, err, idm_span_unknown(NULL))) return false;
    int guard = surface_install_guard(ctx, provider, provider_key, bind_name, bind_name, IDM_BIND_SPACE_TYPE, scopes, err);
    if (guard < 0) {
        free(qualified);
        return false;
    }
    if (guard == 0) {
        free(qualified);
        return true;
    }
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    uint32_t payload = 0;
    TypeDef *t = typed_registry_add_type(ctx, &payload, err, idm_span_unknown(NULL));
    if (!t) {
        surface_rollback(ctx, &checkpoint);
        free(qualified);
        return false;
    }
    t->name = idm_strdup(bind_name);
    if (!t->name || !type_def_set_identity(ctx, t, entry->identity, err, idm_span_unknown(NULL)) || !idm_scope_set_copy(&t->scopes, scopes)) {
        type_def_destroy(t);
        surface_rollback(ctx, &checkpoint);
        free(qualified);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (entry->field_count != 0) {
        t->fields = calloc(entry->field_count, sizeof(*t->fields));
        if (!t->fields) {
            type_def_destroy(t);
            surface_rollback(ctx, &checkpoint);
            free(qualified);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        t->field_count = entry->field_count;
        for (size_t f = 0; f < entry->field_count; f++) {
            if (!type_field_set(ctx, &t->fields[f], entry->fields[f].name, entry->fields[f].contract, err, idm_span_unknown(NULL))) {
                type_def_destroy(t);
                surface_rollback(ctx, &checkpoint);
                free(qualified);
                return false;
            }
        }
    }
    if (!idm_binding_table_add(&ctx->bindings, bind_name, IDM_PHASE_ANY, IDM_BIND_SPACE_TYPE, IDM_BIND_TYPE, scopes, payload, ctx->frame, NULL)) {
        surface_rollback(ctx, &checkpoint);
        free(qualified);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (!qualifier && !install_field_surfaces(ctx, t, payload, scopes, err)) {
        surface_rollback(ctx, &checkpoint);
        free(qualified);
        return false;
    }
    free(qualified);
    return true;
}

static bool ctx_record_dep(ExpandContext *ctx, const char *path, const unsigned char hash[32], uint8_t kind, IdmError *err) {
    for (size_t i = 0; i < ctx->dep_count; i++) {
        if (strcmp(ctx->deps[i].path, path) == 0) return true;
    }
    if (ctx->dep_count == ctx->dep_cap) {
        if (!idm_grow((void **)&ctx->deps, &ctx->dep_cap, sizeof(*ctx->deps), 8u, ctx->dep_count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    ctx->deps[ctx->dep_count].path = idm_strdup(path);
    if (!ctx->deps[ctx->dep_count].path) return idm_error_oom(err, idm_span_unknown(NULL));
    memcpy(ctx->deps[ctx->dep_count].hash, hash, 32u);
    ctx->deps[ctx->dep_count].kind = kind;
    ctx->dep_count++;
    return true;
}

static bool artifact_record_consumer_deps(ExpandContext *ctx, const char *path, const IdmArtifact *art, IdmError *err) {
    if (!ctx_record_dep(ctx, path, art->src_hash, IDM_DEP_PACKAGE, err)) return false;
    for (size_t i = 0; i < art->dep_count; i++) {
        if (!ctx_record_dep(ctx, art->deps[i].path, art->deps[i].hash, art->deps[i].kind, err)) return false;
    }
    return true;
}

const IdmArtifact *artifact_get(ExpandContext *ctx, const char *path, IdmSpan span, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "expand.artifact_get");
    ExpandCache *cache = kernel_artifact_get(ctx->rt, err);
    if (!cache) {
        idm_profile_scope_end(&prof);
        return NULL;
    }
    for (size_t i = 0; i < cache->pkg_count; i++) {
        if (strcmp(cache->pkgs[i]->path, path) == 0) {
            if (!artifact_record_consumer_deps(ctx, path, &cache->pkgs[i]->art, err)) return NULL;
            idm_profile_count("expand.artifact.memory_hit", 1u);
            idm_profile_scope_end(&prof);
            return &cache->pkgs[i]->art;
        }
    }
    for (size_t i = 0; i < cache->compiling_count; i++) {
        if (strcmp(cache->compiling[i], path) == 0) {
            idm_error_set(err, span, "package dependency cycle involving '%s'", path);
            return NULL;
        }
    }
    CachedPackage *entry = calloc(1u, sizeof(*entry));
    if (!entry) {
        idm_error_oom(err, span);
        return NULL;
    }
    entry->path = idm_strdup(path);
    if (!entry->path) {
        free(entry);
        idm_error_oom(err, span);
        return NULL;
    }
    IdmBuffer src;
    idm_buf_init(&src);
    const char *label = NULL;
    if (!idm_package_read_source(ctx->rt, path, &src, &label, span, err)) {
        idm_buf_destroy(&src);
        free(entry->path);
        free(entry);
        return NULL;
    }
    unsigned char src_hash[32];
    if (!source_cache_hash(ctx->rt, src.data ? src.data : "", src.len, src_hash, err)) {
        idm_buf_destroy(&src);
        free(entry->path);
        free(entry);
        return NULL;
    }
    bool loaded = idm_artifact_cache_load(ctx->rt, path, src_hash, &entry->art, package_dep_verified, NULL);
    if (loaded) {
        bool kernel_matches = false;
        for (size_t i = 0; i < entry->art.dep_count; i++) {
            if (strcmp(entry->art.deps[i].path, cache->kernel_path) == 0 && memcmp(entry->art.deps[i].hash, cache->kernel.src_hash, 32u) == 0) {
                kernel_matches = true;
                break;
            }
        }
        if (!kernel_matches) {
            idm_artifact_destroy(&entry->art);
            loaded = false;
        }
    }
    if (!loaded) {
        IdmSyntax *pkg = NULL;
        bool read_ok = idm_expand_read_source_string(ctx->rt, label, src.data ? src.data : "", &pkg, err);
        if (read_ok && !syn_is_form(pkg, "%-package-begin")) {
            idm_syn_free(pkg);
            idm_error_set(err, span, "package source must be a program");
            read_ok = false;
        }
        if (!read_ok) {
            idm_buf_destroy(&src);
            free(entry->path);
            free(entry);
            return NULL;
        }
        if (cache->compiling_count == cache->compiling_cap) {
            if (!idm_grow((void **)&cache->compiling, &cache->compiling_cap, sizeof(*cache->compiling), 8u, cache->compiling_count + 1u)) {
                idm_syn_free(pkg);
                idm_buf_destroy(&src);
                free(entry->path);
                free(entry);
                idm_error_oom(err, span);
                return NULL;
            }
        }
        cache->compiling[cache->compiling_count++] = entry->path;
        IdmScopeStore store;
        store.next_scope = cache->kernel_scope_end;
        bool compiled = compile_package_artifact(ctx->rt, &store, pkg, path, false, src_hash, NULL, NULL, 0u, &entry->art, err);
        cache->compiling_count--;
        idm_syn_free(pkg);
        if (!compiled) {
            idm_buf_destroy(&src);
            free(entry->path);
            free(entry);
            return NULL;
        }
        entry->art.scope_base = cache->kernel_scope_end;
        entry->art.scope_end = store.next_scope;
        memcpy(entry->art.src_hash, src_hash, 32u);
        if (!artifact_intern_literals_recursive(ctx->rt, &entry->art, err)) {
            idm_buf_destroy(&src);
            idm_artifact_destroy(&entry->art);
            free(entry->path);
            free(entry);
            idm_error_oom(err, span);
            return NULL;
        }
        idm_artifact_cache_write(path, &entry->art);
        idm_profile_count("expand.artifact.compiled", 1u);
    } else {
        idm_profile_count("expand.artifact.cache_hit", 1u);
    }
    idm_buf_destroy(&src);
    if (cache->pkg_count == cache->pkg_cap) {
        if (!idm_grow((void **)&cache->pkgs, &cache->pkg_cap, sizeof(*cache->pkgs), 8u, cache->pkg_count + 1u)) {
            idm_artifact_destroy(&entry->art);
            free(entry->path);
            free(entry);
            idm_error_oom(err, span);
            return NULL;
        }
    }
    cache->pkgs[cache->pkg_count++] = entry;
    if (!artifact_record_consumer_deps(ctx, path, &entry->art, err)) return NULL;
    idm_profile_count("expand.artifact.slots", (uint64_t)entry->art.slot_count);
    idm_profile_scope_end(&prof);
    return &entry->art;
}

bool idm_expand_package_artifact_serialize(IdmRuntime *rt, const char *path, IdmBuffer *out, IdmError *err) {
    ExpandContext ctx;
    ctx_init(&ctx, rt);
    const IdmArtifact *art = artifact_get(&ctx, path, idm_span_unknown(path), err);
    bool ok = art && idm_artifact_serialize(art, out, err);
    ctx_destroy(&ctx);
    return ok;
}

bool idm_expand_preload_package_artifact(IdmRuntime *rt, const char *path, const unsigned char *data, size_t len, IdmError *err) {
    ExpandCache *cache = kernel_artifact_get(rt, err);
    if (!cache) return false;
    for (size_t i = 0; i < cache->pkg_count; i++) {
        if (strcmp(cache->pkgs[i]->path, path) == 0) return true;
    }
    CachedPackage *entry = calloc(1u, sizeof(*entry));
    if (!entry) return idm_error_oom(err, idm_span_unknown(path));
    entry->path = idm_strdup(path);
    if (!entry->path) {
        free(entry);
        return idm_error_oom(err, idm_span_unknown(path));
    }
    if (!idm_artifact_deserialize(rt, data, len, &entry->art, err)) {
        free(entry->path);
        free(entry);
        return false;
    }
    if (!artifact_intern_literals_recursive(rt, &entry->art, err)) {
        idm_artifact_destroy(&entry->art);
        free(entry->path);
        free(entry);
        return false;
    }
    if (!idm_artifact_run_phase_inits(rt, &entry->art, err)) {
        idm_artifact_destroy(&entry->art);
        free(entry->path);
        free(entry);
        return false;
    }
    if (cache->pkg_count == cache->pkg_cap) {
        if (!idm_grow((void **)&cache->pkgs, &cache->pkg_cap, sizeof(*cache->pkgs), 8u, cache->pkg_count + 1u)) {
            idm_artifact_destroy(&entry->art);
            free(entry->path);
            free(entry);
            return idm_error_oom(err, idm_span_unknown(path));
        }
    }
    cache->pkgs[cache->pkg_count++] = entry;
    return true;
}

bool artifact_base(ExpandContext *ctx, const IdmArtifact *art, IdmScopeId *out_base, IdmError *err) {
    for (size_t i = 0; i < ctx->artifact_base_count; i++) {
        if (memcmp(ctx->artifact_bases[i].hash, art->src_hash, 32u) == 0) {
            *out_base = ctx->artifact_bases[i].base;
            return true;
        }
    }
    if (ctx->artifact_base_count == ctx->artifact_base_cap) {
        if (!idm_grow((void **)&ctx->artifact_bases, &ctx->artifact_base_cap, sizeof(*ctx->artifact_bases), 8u, ctx->artifact_base_count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    IdmScopeId base = art->scope_base;
    if (art->scope_base != art->scope_end) {
        base = ctx->scope_store.next_scope;
        ctx->scope_store.next_scope += art->scope_end - art->scope_base;
    }
    ArtifactRuntimeState *state = &ctx->artifact_bases[ctx->artifact_base_count];
    memset(state, 0, sizeof(*state));
    state->art = art;
    memcpy(state->hash, art->src_hash, 32u);
    state->base = base;
    ctx->artifact_base_count++;
    *out_base = base;
    return true;
}

bool artifact_init_pending(ExpandContext *ctx, const IdmArtifact *art) {
    ArtifactRuntimeState *state = artifact_runtime_state(ctx, art);
    if (state) {
        size_t phase = phase_index(ctx);
        bool pending = state->init_emit_mark[phase] == 0;
        if (pending) state->init_emit_mark[phase] = ++ctx->init_emit_mark_count;
        return pending;
    }
    return true;
}

static IdmModuleRef *relocated_module_ref(ExpandContext *ctx, IdmModuleRef *src, IdmScopeId min_id, int64_t delta, IdmError *err) {
    if (delta == 0) return idm_module_ref_retain(src);
    IdmModuleRef *clone = idm_module_ref_create(ctx->rt);
    if (!clone) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    uint32_t const_off = 0, fn_off = 0, code_off = 0;
    if (!idm_bc_link(&clone->module, &src->module, &const_off, &fn_off, &code_off, err) ||
        !idm_bc_relocate_syntax_scopes(ctx->rt, &clone->module, min_id, delta, err) ||
        !idm_bc_intern_literals(ctx->rt, &clone->module, err)) {
        idm_module_ref_release(clone);
        return NULL;
    }
    return clone;
}

IdmBytecodeModule *relocated_module_copy(ExpandContext *ctx, const IdmBytecodeModule *src, IdmScopeId min_id, int64_t delta, uint32_t *out_fn_off, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "expand.relocated_module_copy");
    IdmBytecodeModule *module = malloc(sizeof(*module));
    if (!module) {
        idm_error_oom(err, idm_span_unknown(NULL));
        idm_profile_scope_end(&prof);
        return NULL;
    }
    idm_bc_init(module);
    uint32_t const_off = 0, fn_off = 0, code_off = 0;
    if (!idm_bc_link(module, src, &const_off, &fn_off, &code_off, err) ||
        (delta != 0 && !idm_bc_relocate_syntax_scopes(ctx->rt, module, min_id, delta, err)) ||
        !idm_bc_intern_literals(ctx->rt, module, err)) {
        idm_bc_destroy(module);
        free(module);
        idm_profile_scope_end(&prof);
        return NULL;
    }
    if (out_fn_off) *out_fn_off = fn_off;
    idm_profile_count("expand.relocated_module_copy.functions", (uint64_t)src->function_count);
    idm_profile_count("expand.relocated_module_copy.code_words", (uint64_t)src->code_count);
    if (delta != 0) idm_profile_count("expand.relocated_module_copy.relocated", 1u);
    idm_profile_scope_end(&prof);
    return module;
}

static bool install_macro_twin(ExpandContext *ctx, const char *name, IdmScopeId base, size_t macros_before, IdmError *err) {
    if (ctx->macro_count == macros_before || base == 0) return true;
    IdmScopeSet scopes;
    idm_scope_set_init(&scopes);
    bool ok = idm_scope_set_add(&scopes, base) && idm_binding_table_add(&ctx->bindings, name, IDM_PHASE_ANY, IDM_BIND_SPACE_DEFAULT, IDM_BIND_TRANSFORMER, &scopes, (uint32_t)(ctx->macro_count - 1u), ctx->frame, NULL);
    idm_scope_set_destroy(&scopes);
    return ok || idm_error_oom(err, idm_span_unknown(NULL));
}

static bool install_relocated_operator(ExpandContext *ctx, const IdmOperatorDef *op, IdmScopeId min_id, int64_t delta, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, IdmError *err) {
    if (delta == 0) return install_imported_operator(ctx, op, binding_scopes, provider, provider_key, err);
    IdmOperatorDef local = *op;
    idm_scope_set_init(&local.scopes);
    IdmModuleRef *target_module = NULL;
    if (!idm_scope_set_copy(&local.scopes, &op->scopes)) return idm_error_oom(err, idm_span_unknown(NULL));
    idm_scope_set_relocate(&local.scopes, min_id, delta);
    if (op->target_module) {
        target_module = relocated_module_ref(ctx, op->target_module, min_id, delta, err);
        if (!target_module) {
            idm_scope_set_destroy(&local.scopes);
            return false;
        }
        local.target_module = target_module;
    }
    bool ok = install_imported_operator(ctx, &local, binding_scopes, provider, provider_key, err);
    idm_module_ref_release(target_module);
    idm_scope_set_destroy(&local.scopes);
    return ok;
}

static bool copy_operator_target_macro(ExpandContext *ctx, IdmOperatorDef *dst, const IdmOperatorDef *src, IdmError *err) {
    if (!src->target_name) return true;
    IdmSyntax *word = idm_syn_word(src->target_name, idm_span_unknown(NULL));
    if (!word) return idm_error_oom(err, idm_span_unknown(NULL));
    bool scoped = true;
    for (size_t i = 0; scoped && i < src->scopes.count; i++) scoped = idm_syn_scope_add(word, 0, src->scopes.items[i]);
    if (!scoped) {
        idm_syn_free(word);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    const IdmBinding *binding = resolve_default(ctx, word, &status);
    idm_syn_free(word);
    if (status != IDM_RESOLVE_OK || !binding || binding->kind != IDM_BIND_TRANSFORMER) return true;
    if (binding->payload >= ctx->macro_count) {
        return idm_error_set(err, idm_span_unknown(NULL), "operator target macro payload %u is out of bounds", binding->payload);
    }
    MacroDef *macro = &ctx->macros[binding->payload];
    if (macro->fn.closure_backed || !macro->fn.module) {
        return idm_error_set(err, idm_span_unknown(NULL), "exported operator '%s' target macro '%s' cannot be serialized", src->name, src->target_name);
    }
    dst->target_module = idm_module_ref_retain(macro->fn.module);
    dst->target_function_index = macro->fn.function_index;
    dst->target_phase_env = idm_phase_env_retain(macro->fn.phase_env);
    return true;
}

static bool pkg_grammar_rule_copy(IdmGrammarRule *dst, const IdmGrammarRule *src, IdmError *err, IdmSpan span) {
    memset(dst, 0, sizeof(*dst));
    dst->name = idm_strdup(src->name);
    dst->kind = src->kind;
    dst->terminal.kind = src->terminal.kind;
    dst->terminal.flags = src->terminal.flags;
    if (src->terminal.text) {
        dst->terminal.text = idm_strdup(src->terminal.text);
        if (!dst->terminal.text) {
            idm_grammar_rule_destroy(dst);
            return idm_error_oom(err, span);
        }
    }
    if (!idm_reader_program_copy(&dst->pattern, &src->pattern, err, span) ||
        !idm_reader_program_copy(&dst->constructor, &src->constructor, err, span)) {
        idm_grammar_rule_destroy(dst);
        return false;
    }
    if (!dst->name) {
        idm_grammar_rule_destroy(dst);
        return idm_error_oom(err, span);
    }
    return true;
}

static bool pkg_grammar_copy(IdmPkgGrammar *dst, const IdmPkgGrammar *src, IdmError *err, IdmSpan span) {
    memset(dst, 0, sizeof(*dst));
    dst->name = idm_strdup(src->name);
    dst->mode = src->mode;
    dst->exported = true;
    bool ok = dst->name && idm_scope_set_copy(&dst->scopes, &src->scopes);
    if (ok && src->rule_count != 0) {
        dst->rules = calloc(src->rule_count, sizeof(*dst->rules));
        ok = dst->rules != NULL;
        for (size_t i = 0; ok && i < src->rule_count; i++) {
            dst->rule_count = i + 1u;
            ok = pkg_grammar_rule_copy(&dst->rules[i], &src->rules[i], err, span);
        }
    }
    if (!ok) {
        idm_pkg_grammar_destroy(dst);
        if (err && err->present) return false;
        return idm_error_oom(err, span);
    }
    dst->rule_count = src->rule_count;
    return true;
}

bool activate_artifact(ExpandContext *ctx, const char *activation_name, const IdmArtifact *art, IdmScopeId base, const IdmScopeSet *act_scopes, IdmSpan span, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "expand.activate_artifact");
    (void)span;
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    IdmScopeId min_id = art->scope_base;
    int64_t delta = (int64_t)base - (int64_t)art->scope_base;
    char provider_key[17];
    artifact_provider_key(art->src_hash, provider_key);
    bool ok = true;
    for (size_t i = 0; i < art->operator_count; i++) {
        if (!install_relocated_operator(ctx, &art->operators[i], min_id, delta, act_scopes, activation_name, provider_key, err)) {
            ok = false;
            break;
        }
    }
    for (size_t i = 0; ok && i < art->macro_count; i++) {
        IdmModuleRef *module = relocated_module_ref(ctx, art->macros[i].module, min_id, delta, err);
        if (!module) {
            ok = false;
            break;
        }
        size_t before = ctx->macro_count;
        bool installed = install_imported_macro(ctx, art->macros[i].name, act_scopes, module, art->macros[i].function_index, art->macros[i].phase_env, activation_name, provider_key, err);
        idm_module_ref_release(module);
        if (!installed || !install_macro_twin(ctx, art->macros[i].name, base, before, err)) ok = false;
    }
    for (size_t i = 0; ok && i < art->core_syntax_count; i++) {
        const IdmPkgCoreSyntax *core_syntax = &art->core_syntax[i];
        IdmModuleRef *module = relocated_module_ref(ctx, core_syntax->module, min_id, delta, err);
        if (!module) {
            ok = false;
            break;
        }
        bool installed = install_imported_core_syntax(ctx, core_syntax->name, act_scopes, module, core_syntax->function_index, core_syntax->phase_env ? core_syntax->phase_env : ctx->phase_env, activation_name, provider_key, err);
        idm_module_ref_release(module);
        if (!installed) ok = false;
    }
    if (ok && !install_artifact_grammars(ctx, art->grammars, art->grammar_count, act_scopes, min_id, delta, activation_name, provider_key, err)) ok = false;
    if (!ok) surface_rollback(ctx, &checkpoint);
    if (ok) {
        idm_profile_count("expand.activate_artifact.macros", (uint64_t)art->macro_count);
        idm_profile_count("expand.activate_artifact.core_syntax", (uint64_t)art->core_syntax_count);
        idm_profile_count("expand.activate_artifact.operators", (uint64_t)art->operator_count);
        idm_profile_count("expand.activate_artifact.grammars", (uint64_t)art->grammar_count);
    }
    idm_profile_scope_end(&prof);
    return ok;
}

static const char *idm_std_root(void) {
    const char *root = getenv("IDIOMROOT");
    return (root && *root) ? root : "std";
}

static const char *primitive_home_for_std_suffix(const char *suffix) {
    if (!suffix) return NULL;
    return idm_primitive_home_exists(suffix) ? suffix : NULL;
}

static const char *canonical_primitive_home_for_unit(const char *unit) {
    if (!unit || unit[0] == '\0') return NULL;
    if (strcmp(unit, "kernel") == 0) return "kernel";
    if (strncmp(unit, "std/", 4u) == 0) return primitive_home_for_std_suffix(unit + 4u);
    const char *root = idm_std_root();
    size_t root_len = root ? strlen(root) : 0u;
    if (root_len != 0 && strncmp(unit, root, root_len) == 0 && unit[root_len] == '/') {
        return primitive_home_for_std_suffix(unit + root_len + 1u);
    }
    return NULL;
}

static char *trait_spelling_dup(const char *identity) {
    const char *start = strrchr(identity, '/');
    start = start ? start + 1u : identity;
    const char *suffix = strrchr(start, '#');
    return suffix ? idm_strndup(start, (size_t)(suffix - start)) : idm_strdup(start);
}

static bool compile_package_artifact(IdmRuntime *rt, IdmScopeStore *store, const IdmSyntax *pkg, const char *unit_name_hint, bool kernel_mode, const unsigned char src_hash[32], const char *inherited_env_key, const IdmPkgSlot *inherited_slots, size_t inherited_count, IdmArtifact *out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, kernel_mode ? "expand.compile_kernel_artifact" : "expand.compile_package_artifact");
    IdmScopeStore input_store = *store;
    ExpandContext ctx;
    ctx_init(&ctx, rt);
    ctx.scope_store = *store;
    ctx.runner = &ctx.local_runner;
    const char *unit_name = NULL;
    if (unit_name_hint && *unit_name_hint) {
        unit_name = idm_symbol_text(idm_intern(&rt->intern, IDM_SYMBOL_ATOM, unit_name_hint));
        if (!unit_name) {
            idm_error_oom(err, pkg ? pkg->span : idm_span_unknown(NULL));
            *store = input_store;
            ctx_destroy(&ctx);
            idm_profile_scope_end(&prof);
            return false;
        }
        ctx_set_unit(&ctx, unit_name, src_hash);
        ctx.primitive_home = canonical_primitive_home_for_unit(unit_name);
    }
    IdmEnv *phase_runtime_env = idm_fresh_phase_runtime_env(rt, err);
    if (!phase_runtime_env) {
        *store = input_store;
        ctx_destroy(&ctx);
        idm_profile_scope_end(&prof);
        return false;
    }
    ctx.phase_env = idm_phase_env_create(rt, phase_runtime_env);
    if (!ctx.phase_env) {
        idm_error_oom(err, idm_span_unknown(NULL));
        *store = input_store;
        ctx_destroy(&ctx);
        idm_profile_scope_end(&prof);
        return false;
    }
    IdmPhaseReads reads;
    memset(&reads, 0, sizeof(reads));
    IdmPhaseReads *old_reads = rt->phase_reads;
    rt->phase_reads = &reads;
    SavedHooks saved;
    hooks_install(rt, &ctx, &saved);
    bool ok = ctx_seed(&ctx, err) && (kernel_mode || ctx_activate_kernel(&ctx, err));
    size_t macro_base = ctx.macro_count;
    size_t op_base = ctx.operator_count;
    if (ok) ctx.in_package = true;
    if (ok) ok = install_inherited_package_slot_refs(&ctx, inherited_env_key, inherited_slots, inherited_count, err);
    IdmSyntax *scoped_pkg = NULL;
    IdmScopeId package_scope = 0;
    if (ok) {
        scoped_pkg = idm_syn_clone(pkg);
        if (!scoped_pkg) {
            idm_error_oom(err, pkg ? pkg->span : idm_span_unknown(NULL));
            ok = false;
        } else {
            package_scope = idm_scope_fresh(&ctx.scope_store);
            if (!idm_syn_scope_add_tree(scoped_pkg, 0, package_scope)) {
                idm_error_oom(err, scoped_pkg->span);
                ok = false;
            }
        }
    }
    IdmCore *body = ok ? expand_package_body_items(&ctx, scoped_pkg->as.seq.items, 1, scoped_pkg->as.seq.count, err) : NULL;
    idm_syn_free(scoped_pkg);
    rt->phase_reads = old_reads;
    if (body && reads.failed) {
        idm_error_oom(err, idm_span_unknown(NULL));
        idm_core_free(body);
        body = NULL;
    }
    for (size_t i = 0; body && i < reads.count; i++) {
        if (!ctx_record_dep(&ctx, reads.items[i].path, reads.items[i].hash, reads.items[i].kind, err)) {
            idm_core_free(body);
            body = NULL;
        }
    }
    idm_phase_reads_destroy(&reads);
    if (!body) {
        hooks_restore(rt, &saved);
        *store = input_store;
        ctx_destroy(&ctx);
        return false;
    }
    char *name = ctx.package_name ? idm_strdup(ctx.package_name) : NULL;
    bool copy_ok = true;
    size_t slot_count = ctx.package_slot_count;
    IdmPkgSlot *slots = NULL;
    if (copy_ok && slot_count != 0) {
        slots = calloc(slot_count, sizeof(*slots));
        if (!slots) copy_ok = false;
        else {
            for (size_t i = 0; i < slot_count; i++) {
                slots[i].name = idm_strdup(ctx.package_slots[i].name);
                slots[i].slot = ctx.package_slots[i].slot;
                slots[i].arity = ctx.package_slots[i].arity;
                slots[i].exported = ctx.package_slots[i].exported;
                if (!slots[i].name || !idm_scope_set_copy(&slots[i].scopes, &ctx.package_slots[i].scopes)) {
                    for (size_t j = 0; j <= i; j++) idm_pkg_slot_destroy(&slots[j]);
                    free(slots);
                    slots = NULL;
                    copy_ok = false;
                    break;
                }
            }
        }
    }
    size_t pkg_entity_count = 0;
    IdmPkgTypedEntity *pkg_entities = NULL;
    if (copy_ok) {
        for (size_t i = 0; i < ctx.typed.entity_count; i++) {
            const TypedEntity *entity = &ctx.typed.entities[i];
            if ((entity->kind == IDM_TYPED_ENTITY_TYPE && entity->as.type.exported) ||
                (entity->kind == IDM_TYPED_ENTITY_TRAIT && entity->as.trait.exported) ||
                (entity->kind == IDM_TYPED_ENTITY_PROTOCOL && entity->as.protocol.exported)) {
                pkg_entity_count++;
            }
        }
    }
    if (copy_ok && pkg_entity_count != 0) {
        pkg_entities = calloc(pkg_entity_count, sizeof(*pkg_entities));
        if (!pkg_entities) copy_ok = false;
        size_t k = 0;
        for (size_t i = 0; copy_ok && i < ctx.typed.entity_count; i++) {
            TypedEntity *entity = &ctx.typed.entities[i];
            IdmPkgTypedEntity *dst = &pkg_entities[k];
            switch (entity->kind) {
                case IDM_TYPED_ENTITY_TYPE: {
                    if (!entity->as.type.exported) continue;
                    const TypeDef *t = &entity->as.type;
                    IdmPkgType *type = &dst->as.type;
                    dst->kind = IDM_TYPED_ENTITY_TYPE;
                    type->name = idm_strdup(t->name);
                    type->identity = idm_strdup(type_def_identity_text(t));
                    if (!type->name || !type->identity || !idm_scope_set_copy(&type->scopes, &t->scopes)) copy_ok = false;
                    if (copy_ok && t->field_count != 0) {
                        type->fields = calloc(t->field_count, sizeof(*type->fields));
                        if (!type->fields) copy_ok = false;
                        else {
                            type->field_count = t->field_count;
                            for (size_t f = 0; f < t->field_count; f++) {
                                const char *name = type_field_name_text(&t->fields[f]);
                                const char *contract = type_field_contract_text(&t->fields[f]);
                                type->fields[f].name = idm_strdup(name);
                                type->fields[f].contract = contract ? idm_strdup(contract) : NULL;
                                if (!type->fields[f].name || (contract && !type->fields[f].contract)) { copy_ok = false; break; }
                            }
                        }
                    }
                    break;
                }
                case IDM_TYPED_ENTITY_TRAIT: {
                    if (!entity->as.trait.exported) continue;
                    const TraitDef *t = &entity->as.trait;
                    IdmPkgTrait *trait = &dst->as.trait;
                    dst->kind = IDM_TYPED_ENTITY_TRAIT;
                    const char *identity = trait_def_identity_text(t);
                    trait->name = trait_spelling_dup(identity);
                    trait->identity = idm_strdup(identity);
                    if (!trait->name || !trait->identity ||
                        !idm_trait_requirement_defs_copy(t->requirements, t->requirement_count, &trait->requirements) ||
                        !trait_method_defs_export(t->methods, t->method_count, &trait->methods)) {
                        copy_ok = false;
                    } else {
                        trait->requirement_count = t->requirement_count;
                        trait->method_count = t->method_count;
                    }
                    break;
                }
                case IDM_TYPED_ENTITY_PROTOCOL: {
                    if (!entity->as.protocol.exported) continue;
                    ProtocolDef *p = &entity->as.protocol;
                    IdmPkgProtocol *protocol = &dst->as.protocol;
                    dst->kind = IDM_TYPED_ENTITY_PROTOCOL;
                    protocol->name = idm_strdup(p->name);
                    protocol->identity = idm_strdup(protocol_def_identity_text(p));
                    protocol->art = p->art;
                    if (!protocol->name || !protocol->identity || !protocol->art) {
                        copy_ok = false;
                    } else {
                        p->art = NULL;
                    }
                    break;
                }
            }
            k++;
        }
        if (!copy_ok && pkg_entities) {
            for (size_t j = 0; j < pkg_entity_count; j++) idm_pkg_typed_entity_destroy(&pkg_entities[j]);
            free(pkg_entities);
            pkg_entities = NULL;
        }
        if (!copy_ok) pkg_entity_count = 0;
    }
    size_t pkg_method_impl_count = 0;
    IdmPkgMethodImpl *pkg_method_impls = NULL;
    if (copy_ok) pkg_method_impl_count = ctx.typed.method_impl_count;
    if (copy_ok && pkg_method_impl_count != 0) {
        pkg_method_impls = calloc(pkg_method_impl_count, sizeof(*pkg_method_impls));
        if (!pkg_method_impls) copy_ok = false;
        for (size_t i = 0; copy_ok && i < pkg_method_impl_count; i++) {
            const MethodImplDef *src = &ctx.typed.method_impls[i];
            IdmPkgMethodImpl *dst = &pkg_method_impls[i];
            const MethodSurfaceDef *surface = method_surface_by_index(&ctx, src->method_surface);
            if (!surface) {
                copy_ok = false;
                break;
            }
            const char *type = method_impl_type_text(src);
            if (!type) {
                copy_ok = false;
                break;
            }
            dst->trait = idm_strdup(method_surface_trait_text(surface));
            dst->type = idm_strdup(type);
            dst->method = idm_strdup(method_surface_name_text(surface));
            dst->arity = src->arity;
            dst->impl_env = src->impl_env;
            dst->impl_env_key = idm_strdup(src->impl_env_key ? src->impl_env_key : "");
            dst->impl_slot = src->impl_slot;
            if (!dst->trait || !dst->type || !dst->method || !dst->impl_env_key) copy_ok = false;
        }
        if (!copy_ok && pkg_method_impls) {
            for (size_t i = 0; i < pkg_method_impl_count; i++) idm_pkg_method_impl_destroy(&pkg_method_impls[i]);
            free(pkg_method_impls);
            pkg_method_impls = NULL;
        }
        if (!copy_ok) pkg_method_impl_count = 0;
    }
    size_t macro_count = 0;
    size_t op_count = 0;
    if (copy_ok) {
        for (size_t i = macro_base; i < ctx.macro_count; i++) if (ctx.macros[i].exported) macro_count++;
        for (size_t i = op_base; i < ctx.operator_count; i++) if (ctx.operators[i].exported) op_count++;
    }
    IdmPkgMacro *macros = NULL;
    IdmOperatorDef *operators = NULL;
    size_t grammar_count = copy_ok ? ctx.decl_grammar_count : 0;
    IdmPkgGrammar *grammars = NULL;
    if (copy_ok && macro_count != 0) {
        macros = calloc(macro_count, sizeof(*macros));
        if (!macros) copy_ok = false;
        else {
            size_t k = 0;
            for (size_t i = macro_base; i < ctx.macro_count && copy_ok; i++) {
                if (!ctx.macros[i].exported) continue;
                macros[k].name = idm_strdup(ctx.macros[i].name);
                if (!macros[k].name) { copy_ok = false; break; }
                macros[k].module = idm_module_ref_retain(ctx.macros[i].fn.module);
                macros[k].function_index = ctx.macros[i].fn.function_index;
                macros[k].phase_env = idm_phase_env_retain(ctx.macros[i].fn.phase_env);
                k++;
            }
        }
    }
    if (copy_ok && op_count != 0) {
        operators = calloc(op_count, sizeof(*operators));
        if (!operators) copy_ok = false;
        else {
            size_t k = 0;
            for (size_t i = op_base; i < ctx.operator_count && copy_ok; i++) {
                if (!ctx.operators[i].exported) continue;
                IdmOperatorDef *d = &operators[k];
                d->name = idm_strdup(ctx.operators[i].name);
                d->capture = ctx.operators[i].capture ? idm_strdup(ctx.operators[i].capture) : NULL;
                d->capture_kind = ctx.operators[i].capture_kind;
                d->capture_left = ctx.operators[i].capture_left;
                d->capture_count = ctx.operators[i].capture_count;
                d->precedence = ctx.operators[i].precedence;
                d->assoc = ctx.operators[i].assoc;
                d->target_name = ctx.operators[i].target_name ? idm_strdup(ctx.operators[i].target_name) : NULL;
                d->exported = true;
                idm_scope_set_init(&d->scopes);
                bool sok = idm_scope_set_copy(&d->scopes, &ctx.operators[i].scopes);
                if (!d->name || (ctx.operators[i].capture && !d->capture) || (ctx.operators[i].target_name && !d->target_name) || !sok ||
                    !copy_operator_target_macro(&ctx, d, &ctx.operators[i], err)) { copy_ok = false; break; }
                k++;
            }
        }
    }
    if (copy_ok && grammar_count != 0) {
        grammars = calloc(grammar_count, sizeof(*grammars));
        if (!grammars) copy_ok = false;
        for (size_t i = 0; copy_ok && i < grammar_count; i++) {
            copy_ok = pkg_grammar_copy(&grammars[i], &ctx.decl_grammars[i], err, idm_span_unknown(unit_name_hint));
        }
    }
    if (copy_ok) copy_ok = ctx_validate_source_reader_decls(&ctx, err);
    char *source_reader = NULL;
    if (copy_ok && ctx.decl_source_reader_count != 0) {
        SourceReaderDecl *decl = &ctx.decl_source_readers[ctx.decl_source_reader_count - 1u];
        bool found = false;
        for (size_t i = 0; i < grammar_count; i++) {
            if (grammars[i].name && strcmp(grammars[i].name, decl->name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            copy_ok = idm_error_set(err, decl->span, "core-reader source reader '%s' has no core-grammar declaration", decl->name);
        } else {
            source_reader = idm_strdup(decl->name);
            if (!source_reader) copy_ok = idm_error_oom(err, decl->span);
        }
    }
    size_t core_syntax_count = copy_ok ? ctx.decl_core_syntax_count : 0;
    IdmPkgCoreSyntax *core_syntax = NULL;
    if (copy_ok && core_syntax_count != 0) {
        core_syntax = calloc(core_syntax_count, sizeof(*core_syntax));
        if (!core_syntax) copy_ok = false;
        for (size_t i = 0; copy_ok && i < core_syntax_count; i++) {
            core_syntax[i].name = idm_strdup(ctx.decl_core_syntax[i].name);
            core_syntax[i].module = idm_module_ref_retain(ctx.decl_core_syntax[i].fn.module);
            core_syntax[i].function_index = ctx.decl_core_syntax[i].fn.function_index;
            core_syntax[i].phase_env = idm_phase_env_retain(ctx.decl_core_syntax[i].fn.phase_env);
            if (!core_syntax[i].name || !core_syntax[i].module) copy_ok = false;
        }
    }
    IdmPhaseEnv *pkg_phase_env = copy_ok ? idm_phase_env_retain(ctx.phase_env) : NULL;
    IdmArtifactDep *pkg_deps = NULL;
    size_t pkg_dep_count = 0;
    if (copy_ok) {
        pkg_deps = ctx.deps;
        pkg_dep_count = ctx.dep_count;
        ctx.deps = NULL;
        ctx.dep_count = 0;
        ctx.dep_cap = 0;
    }
    IdmBytecodeModule *module = NULL;
    uint32_t init_fn = 0;
    if (copy_ok) {
        body = wrap_kernel_use(&ctx, body, err);
        if (!body) copy_ok = false;
    }
    if (copy_ok && !idm_core_normalize(rt, &body, err)) copy_ok = false;
    if (copy_ok) {
        module = malloc(sizeof(*module));
        if (module) {
            idm_bc_init(module);
            if (!idm_core_compile_main(rt, body, module, &init_fn, err)) { idm_bc_destroy(module); free(module); module = NULL; }
        }
    }
    idm_core_free(body);
    hooks_restore(rt, &saved);
    IdmScopeStore output_store = ctx.scope_store;
    *store = output_store;
    ctx_destroy(&ctx);
    if (!copy_ok || !module) {
        *store = input_store;
        free(name);
        if (slots) { for (size_t i = 0; i < slot_count; i++) idm_pkg_slot_destroy(&slots[i]); free(slots); }
        if (macros) { for (size_t i = 0; i < macro_count; i++) idm_pkg_macro_destroy(&macros[i]); free(macros); }
        if (operators) { for (size_t i = 0; i < op_count; i++) idm_operator_def_destroy(&operators[i]); free(operators); }
        if (grammars) { for (size_t i = 0; i < grammar_count; i++) idm_pkg_grammar_destroy(&grammars[i]); free(grammars); }
        free(source_reader);
        if (pkg_entities) { for (size_t i = 0; i < pkg_entity_count; i++) idm_pkg_typed_entity_destroy(&pkg_entities[i]); free(pkg_entities); }
        if (pkg_method_impls) { for (size_t i = 0; i < pkg_method_impl_count; i++) idm_pkg_method_impl_destroy(&pkg_method_impls[i]); free(pkg_method_impls); }
        if (core_syntax) { for (size_t i = 0; i < core_syntax_count; i++) idm_pkg_core_syntax_destroy(&core_syntax[i]); free(core_syntax); }
        idm_phase_env_release(pkg_phase_env);
        for (size_t i = 0; i < pkg_dep_count; i++) free(pkg_deps[i].path);
        free(pkg_deps);
        if (err && !err->present) idm_error_oom(err, idm_span_unknown(NULL));
        idm_profile_scope_end(&prof);
        return false;
    }
    out->module = module;
    out->init_fn = init_fn;
    out->scope_base = package_scope;
    out->scope_end = package_scope;
    out->name = name;
    out->slots = slots;
    out->slot_count = slot_count;
    out->macros = macros;
    out->macro_count = macro_count;
    out->operators = operators;
    out->operator_count = op_count;
    out->grammars = grammars;
    out->grammar_count = grammar_count;
    out->source_reader = source_reader;
    out->typed.entities = pkg_entities;
    out->typed.entity_count = pkg_entity_count;
    out->typed.method_impls = pkg_method_impls;
    out->typed.method_impl_count = pkg_method_impl_count;
    out->core_syntax = core_syntax;
    out->core_syntax_count = core_syntax_count;
    out->phase_env = pkg_phase_env;
    out->deps = pkg_deps;
    out->dep_count = pkg_dep_count;
    idm_profile_count(kernel_mode ? "expand.compile_kernel_artifact.slots" : "expand.compile_package_artifact.slots", (uint64_t)slot_count);
    idm_profile_count(kernel_mode ? "expand.compile_kernel_artifact.functions" : "expand.compile_package_artifact.functions", module ? (uint64_t)module->function_count : 0u);
    idm_profile_count(kernel_mode ? "expand.compile_kernel_artifact.code_words" : "expand.compile_package_artifact.code_words", module ? (uint64_t)module->code_count : 0u);
    idm_profile_scope_end(&prof);
    return true;
}

bool compile_package_module(ExpandContext *parent, const IdmSyntax *pkg, const char *unit_name_hint, const unsigned char src_hash[32], const IdmPkgSlot *inherited_slots, size_t inherited_count, IdmArtifact *out, IdmError *err) {
    return compile_package_artifact(parent->rt, &parent->scope_store, pkg, unit_name_hint, false, src_hash, parent->unit_key, inherited_slots, inherited_count, out, err);
}
