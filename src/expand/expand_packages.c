#include "internal.h"
#include "idm_build_id.h"


static void expand_cache_destroy(void *p);

static ExpandCache *kernel_artifact_get(IdmRuntime *rt, IdmError *err);
static bool source_reader_get(IdmRuntime *rt, const IdmReaderArtifact **out, IdmError *err);
static bool ctx_record_dep(ExpandContext *ctx, const char *path, const unsigned char hash[32], uint8_t kind, IdmError *err);
static bool artifact_record_consumer_deps(ExpandContext *ctx, const char *path, const IdmArtifact *art, IdmError *err);
static bool artifact_intern_literals_recursive(IdmRuntime *rt, IdmArtifact *art, IdmError *err);
static bool idm_artifact_relocate(IdmRuntime *rt, IdmArtifact *art, IdmScopeId min_id, int64_t delta, IdmError *err);
static IdmModuleRef *relocated_module_ref(ExpandContext *ctx, IdmModuleRef *src, IdmScopeId min_id, int64_t delta, IdmError *err);
static bool install_macro_twin(ExpandContext *ctx, const char *name, IdmScopeId base, size_t macros_before, const char *provider, IdmError *err);
static bool install_relocated_operator(ExpandContext *ctx, const IdmOperatorDef *op, IdmScopeId min_id, int64_t delta, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, IdmError *err);
static bool install_artifact_protocol(ExpandContext *ctx, const IdmPkgProtocol *entry, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err);
static bool install_artifact_trait(ExpandContext *ctx, const IdmPkgTrait *entry, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err);
static bool install_artifact_type(ExpandContext *ctx, const IdmPkgType *entry, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err);
static bool install_artifact_method_impls(ExpandContext *ctx, const IdmPkgMethodImpl *impls, size_t count, const char *provider_key, IdmError *err);
static bool pkg_grammar_copy(IdmPkgGrammar *dst, const IdmPkgGrammar *src, IdmError *err, IdmSpan span);
static const char *canonical_primitive_home_for_unit(const char *unit);

static bool compile_package_artifact(IdmRuntime *rt, IdmScopeStore *store, const IdmSyntax *pkg, const IdmPackageUnit *units, size_t unit_count, const char *unit_name_hint, bool kernel_mode, const unsigned char src_hash[32], const char *inherited_env_key, const IdmPkgSlot *inherited_slots, size_t inherited_count, const IdmPkgImport *inherited_imports, size_t inherited_import_count, const ProtocolPreActivation *preacts, size_t preact_count, IdmArtifact *out, IdmError *err);

static bool bind_package_slot_ref(ExpandContext *ctx, const char *bind_name, int phase, const IdmScopeSet *scopes, IdmArity arity, const IdmCallableContract *contract, const char *env_key, uint32_t source_slot, IdmSpan span, IdmError *err);

static int slot_bind_phase(const ExpandContext *ctx, bool has_contract, const IdmCallableContract *contract) {
    return has_contract && contract->passthrough ? IDM_PHASE_ANY : ctx->phase;
}

static bool collect_ctx_package_imports(const ExpandContext *ctx, IdmPkgImport **out_imports, size_t *out_count) {
    *out_imports = NULL;
    *out_count = 0;
    size_t candidate_count = 0;
    for (size_t i = 0; i < ctx->bindings.count; i++) {
        const IdmBinding *b = &ctx->bindings.items[i];
        if (b->kind == IDM_BIND_PACKAGE_SLOT && b->space == IDM_BIND_SPACE_DEFAULT && (b->phase == 0 || b->phase == IDM_PHASE_ANY) && b->payload < ctx->package_slot_ref_count) candidate_count++;
    }
    if (candidate_count == 0) return true;
    IdmPkgImport *imports = calloc(candidate_count, sizeof(*imports));
    if (!imports) return false;
    size_t count = 0;
    for (size_t i = 0; i < ctx->bindings.count; i++) {
        const IdmBinding *b = &ctx->bindings.items[i];
        if (!(b->kind == IDM_BIND_PACKAGE_SLOT && b->space == IDM_BIND_SPACE_DEFAULT && (b->phase == 0 || b->phase == IDM_PHASE_ANY) && b->payload < ctx->package_slot_ref_count)) continue;
        const PackageSlotRef *ref = &ctx->package_slot_refs[b->payload];
        IdmPkgImport *imp = &imports[count];
        imp->name = idm_strdup(b->name);
        imp->env_key = ref->env_key ? idm_strdup(ref->env_key) : NULL;
        imp->slot = ref->slot;
        imp->arity = b->arity;
        bool entry_ok = imp->name != NULL && (ref->env_key == NULL || imp->env_key != NULL);
        if (entry_ok && b->has_contract) {
            entry_ok = idm_callable_contract_copy(&imp->contract, &b->contract);
            imp->has_contract = entry_ok;
        }
        if (entry_ok) entry_ok = idm_scope_set_copy(&imp->scopes, &b->scopes);
        count++;
        if (!entry_ok) {
            for (size_t j = 0; j < count; j++) idm_pkg_import_destroy(&imports[j]);
            free(imports);
            return false;
        }
    }
    *out_imports = imports;
    *out_count = count;
    return true;
}

typedef bool (*ArtifactMatchFn)(const IdmArtifact *art, const void *needle);

static const IdmArtifact *cache_artifact_find(IdmRuntime *rt, ArtifactMatchFn match, const void *needle) {
    ExpandCache *cache = rt->expand_cache;
    if (!cache) return NULL;
    if (cache->kernel_ready && match(&cache->kernel, needle)) return &cache->kernel;
    for (size_t i = 0; i < cache->pkg_count; i++) {
        if (match(&cache->pkgs[i]->art, needle)) return &cache->pkgs[i]->art;
    }
    for (size_t i = 0; i < cache->interned_count; i++) {
        if (match(cache->interned[i], needle)) return cache->interned[i];
    }
    return NULL;
}

static bool artifact_match_hash(const IdmArtifact *art, const void *needle) {
    return memcmp(art->src_hash, needle, 32u) == 0 || memcmp(art->action_hash, needle, 32u) == 0;
}

static bool artifact_match_key(const IdmArtifact *art, const void *needle) {
    char key[17];
    artifact_provider_key(art->src_hash, key);
    return strcmp(key, (const char *)needle) == 0;
}

const IdmArtifact *expand_cache_artifact_by_key(IdmRuntime *rt, const char *env_key) {
    if (!env_key) return NULL;
    return cache_artifact_find(rt, artifact_match_key, env_key);
}

static bool install_inherited_import_refs(ExpandContext *ctx, const IdmPkgImport *imports, size_t count, IdmError *err) {
    for (size_t i = 0; i < count; i++) {
        const IdmPkgImport *imp = &imports[i];
        if (!bind_package_slot_ref(ctx, imp->name, slot_bind_phase(ctx, imp->has_contract, &imp->contract), &imp->scopes, imp->arity, imp->has_contract ? &imp->contract : NULL, imp->env_key, imp->slot, idm_span_unknown(NULL), err)) return false;
    }
    return true;
}

static bool install_inherited_package_slot_refs(ExpandContext *ctx, const char *env_key, const IdmPkgSlot *slots, size_t count, IdmError *err) {
    if (count == 0) return true;
    if (!env_key) return idm_error_set(err, idm_span_unknown(NULL), "inherited package slots require a provider key");
    for (size_t i = 0; i < count; i++) {
        if (!bind_package_slot_ref(ctx, slots[i].name, slot_bind_phase(ctx, slots[i].has_contract, &slots[i].contract), &slots[i].scopes, slots[i].arity, slots[i].has_contract ? &slots[i].contract : NULL, env_key, slots[i].slot, idm_span_unknown(NULL), err)) return false;
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

static bool install_artifact_field_selectors(ExpandContext *ctx, const IdmArtifact *art, const char *provider_key, IdmError *err) {
    for (size_t i = 0; i < art->field_selector_count; i++) {
        if (field_selector_lookup(ctx, art->field_selectors[i].name)) continue;
        FieldSelectorDef *sel = field_selector_ensure(ctx, art->field_selectors[i].name, err);
        if (!sel) return false;
        sel->slot = art->field_selectors[i].slot;
        sel->env = true;
        sel->emitted = true;
        sel->env_key = idm_strdup(art->field_selectors[i].env_key ? art->field_selectors[i].env_key : (provider_key ? provider_key : ""));
        if (!sel->env_key) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    return true;
}

static bool install_artifact_const_slots(ExpandContext *ctx, const IdmArtifact *art, const char *provider_key, IdmError *err) {
    if (!art->module || !provider_key || !provider_key[0]) return true;
    IdmEnv *env = NULL;
    for (size_t i = 0; i < art->slot_count; i++) {
        const IdmPkgSlot *slot = &art->slots[i];
        if (slot->const_entry_count == 0) continue;
        if (!env) {
            env = idm_package_env_get_or_create(ctx->rt, provider_key);
            if (!env) return idm_error_oom(err, idm_span_unknown(NULL));
        }
        if (!idm_bc_is_finalized(art->module) && !idm_bc_intern_literals(ctx->rt, art->module, err)) return false;
        IdmPatternSelector *selector = NULL;
        if (!idm_bc_build_selector_for_entries(ctx->rt, art->module, slot->const_entries, slot->const_entry_count, &selector, err)) return false;
        IdmValue closure = idm_closure_multi_selectable_in_module(ctx->rt, art->module, slot->const_entries, slot->const_entry_count, selector, NULL, 0, env, err);
        idm_pattern_selector_free(selector);
        if (err->present) return false;
        if (!idm_env_slot_ensure(env, slot->slot, err)) return false;
        if (!idm_env_slot_set(ctx->rt, env, slot->slot, closure, err)) return false;
    }
    return true;
}

static bool install_artifact_typed_registry(ExpandContext *ctx, const IdmArtifact *art, UseSelection *selection, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err) {
    return install_artifact_field_selectors(ctx, art, provider_key, err) &&
           install_artifact_typed_entities(ctx, art, selection, scopes, qualifier, provider, provider_key, err) &&
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

bool record_runtime_init(ExpandContext *ctx, const IdmArtifact *art, IdmSpan span, IdmError *err) {
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

static bool bind_package_slot_ref(ExpandContext *ctx, const char *bind_name, int phase, const IdmScopeSet *scopes, IdmArity arity, const IdmCallableContract *contract, const char *env_key, uint32_t source_slot, IdmSpan span, IdmError *err) {
    uint32_t ref_id = 0;
    if (!package_slot_ref_add(ctx, env_key, source_slot, &ref_id)) return idm_error_oom(err, span);
    IdmBindingId binding_id = 0;
    bool ok = idm_binding_table_add_with_arity(&ctx->bindings, bind_name, phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_PACKAGE_SLOT, scopes, ref_id, IDM_FRAME_ENV, arity, &binding_id);
    if (ok && contract) ok = idm_binding_table_set_contract(&ctx->bindings, binding_id, contract);
    return ok || idm_error_oom(err, span);
}

bool install_artifact_runtime_slots_mode(ExpandContext *ctx, const IdmArtifact *art, IdmScopeId min_id, int64_t delta, bool once, bool with_imports, IdmSpan span, IdmError *err) {
    if (!art || (art->slot_count == 0 && art->import_count == 0)) return true;
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
            ok = bind_package_slot_ref(ctx, slot->name, slot_bind_phase(ctx, slot->has_contract, &slot->contract), &scopes, slot->arity, slot->has_contract ? &slot->contract : NULL, provider_key, slot->slot, span, err);
        }
        idm_scope_set_destroy(&scopes);
        if (!ok) return false;
    }
    for (size_t i = 0; with_imports && i < art->import_count; i++) {
        const IdmPkgImport *imp = &art->imports[i];
        IdmScopeSet scopes;
        idm_scope_set_init(&scopes);
        bool ok = idm_scope_set_copy(&scopes, &imp->scopes);
        if (ok) {
            idm_scope_set_relocate(&scopes, min_id, delta);
            ok = bind_package_slot_ref(ctx, imp->name, slot_bind_phase(ctx, imp->has_contract, &imp->contract), &scopes, imp->arity, imp->has_contract ? &imp->contract : NULL, imp->env_key, imp->slot, span, err);
        }
        idm_scope_set_destroy(&scopes);
        if (!ok) return false;
    }
    return !once || record_runtime_init(ctx, art, span, err);
}

bool install_artifact_runtime_slots(ExpandContext *ctx, const IdmArtifact *art, IdmScopeId min_id, int64_t delta, bool once, IdmSpan span, IdmError *err) {
    return install_artifact_runtime_slots_mode(ctx, art, min_id, delta, once, false, span, err);
}

bool bind_artifact_exported_slots(ExpandContext *ctx, const IdmArtifact *art, const IdmScopeSet *act_scopes, IdmSpan span, IdmError *err) {
    if (!art || art->slot_count == 0) return true;
    char provider_key[17];
    artifact_provider_key(art->src_hash, provider_key);
    for (size_t i = 0; i < art->slot_count; i++) {
        const IdmPkgSlot *slot = &art->slots[i];
        if (!slot->exported) continue;
        if (!bind_package_slot_ref(ctx, slot->name, slot_bind_phase(ctx, slot->has_contract, &slot->contract), act_scopes ? act_scopes : &ctx->empty_scopes, slot->arity, slot->has_contract ? &slot->contract : NULL, provider_key, slot->slot, span, err)) return false;
    }
    return true;
}

const IdmArtifact *expand_cache_artifact_by_hash(IdmRuntime *rt, const unsigned char hash[32]) {
    return cache_artifact_find(rt, artifact_match_hash, hash);
}

IdmArtifact *expand_cache_intern_artifact(IdmRuntime *rt, IdmArtifact *moved) {
    if (!moved) return NULL;
    ExpandCache *cache = expand_cache_get(rt);
    if (!cache) return NULL;
    IdmArtifact *existing = (IdmArtifact *)expand_cache_artifact_by_hash(rt, moved->src_hash);
    if (existing) {
        idm_artifact_destroy(moved);
        free(moved);
        return existing;
    }
    if (cache->interned_count == cache->interned_cap) {
        if (!idm_grow((void **)&cache->interned, &cache->interned_cap, sizeof(*cache->interned), 8u, cache->interned_count + 1u)) return NULL;
    }
    moved->rt_owned = true;
    cache->interned[cache->interned_count++] = moved;
    return moved;
}

static IdmBytecodeModule *rt_cached_module_for(IdmRuntime *rt, const IdmArtifact *art) {
    if (!art) return NULL;
    if (art->rt_owned) return art->module;
    const IdmArtifact *cached = expand_cache_artifact_by_hash(rt, art->src_hash);
    return cached ? cached->module : NULL;
}

IdmCore *wrap_artifact_runtime_init(ExpandContext *ctx, const IdmArtifact *art, IdmScopeId min_id, int64_t delta, bool borrow_module, bool allow_empty_module, IdmCore *body, IdmSpan span, IdmError *err) {
    if (!body) return NULL;
    uint32_t fn_off = 0;
    IdmBytecodeModule *module = NULL;
    bool module_owned = false;
    IdmBytecodeModule *shared = delta == 0 ? rt_cached_module_for(ctx->rt, art) : NULL;
    if (art && art->module) {
        if (borrow_module || shared) {
            module = borrow_module ? art->module : shared;
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

IdmCore *expand_use(ExpandContext *ctx, const char *path, const char *qualifier, UseSelection *selection, const IdmSyntax *form, IdmSpan span, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "expand.use");
    const IdmArtifact *art = artifact_get(ctx, path, span, err);
    if (!art) {
        if (err && err->present) idm_error_note_at(err, span, "while loading package '%s'", path);
        if (idm_error_reason_is(err, "incomplete")) {
            IdmValue dropped;
            (void)idm_error_take_reason(err, &dropped);
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
    if (!binder_scopes_pruned(ctx, form, &act_scopes)) {
        idm_profile_scope_end(&prof);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    const char *provider = art->name ? art->name : path;
    char provider_key[17];
    artifact_provider_key(art->src_hash, provider_key);

    bool ok = true;
    for (size_t i = 0; i < art->slot_count && ok; i++) {
        if (!art->slots[i].exported) continue;
        if (!use_selection_allows(selection, art->slots[i].name)) continue;
        const char *bind_name = art->slots[i].name;
        char *qualified = NULL;
        if (!artifact_bind_name(qualifier, art->slots[i].name, &bind_name, &qualified, err, span)) { ok = false; break; }
        int phase = slot_bind_phase(ctx, art->slots[i].has_contract, &art->slots[i].contract);
        ok = bind_package_slot_ref(ctx, bind_name, phase, &act_scopes, art->slots[i].arity, art->slots[i].has_contract ? &art->slots[i].contract : NULL, provider_key, art->slots[i].slot, span, err);
        free(qualified);
    }
    for (size_t i = 0; i < art->macro_count && ok; i++) {
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
             install_macro_twin(ctx, art->macros[i].name, base, before, provider, err);
        idm_module_ref_release(module);
        free(qualified);
    }
    if (ok) ok = install_artifact_typed_registry(ctx, art, selection, &act_scopes, qualifier, provider, provider_key, err);
    if (ok) ok = use_selection_matched_all(selection, path, err);
    if (ok) ok = install_artifact_runtime_slots(ctx, art, min_id, delta, true, span, err);
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
    bool init_pending = !ctx->in_package && artifact_init_pending(ctx, art);
    IdmCore *cont = idm_core_literal(idm_nil(), idm_span_unknown(NULL));
    if (!cont) {
        idm_core_free(refresh);
        idm_profile_scope_end(&prof);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
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
    if (!init_pending || !art->module || ctx->in_package) {
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
    idm_phase_env_release(cache->kernel_seed_phase_env);
    for (size_t i = 0; i < cache->pkg_count; i++) {
        idm_artifact_destroy(&cache->pkgs[i]->art);
        free(cache->pkgs[i]->path);
        free(cache->pkgs[i]);
    }
    free(cache->pkgs);
    for (size_t i = 0; i < cache->interned_count; i++) {
        cache->interned[i]->rt_owned = false;
        idm_artifact_destroy(cache->interned[i]);
        free(cache->interned[i]);
    }
    free(cache->interned);
    idm_reader_artifact_destroy(cache->source_reader);
    free(cache->source_reader_path);
    free(cache->kernel_path);
    free(cache->compiling);
    for (size_t i = 0; i < cache->action_count; i++) free(cache->actions[i].path);
    free(cache->actions);
    free(cache);
}

ExpandCache *expand_cache_get(IdmRuntime *rt) {
    if (!rt->expand_cache) {
        ExpandCache *cache = calloc(1u, sizeof(*cache));
        if (!cache) return NULL;
        cache->rt = rt;
        rt->expand_cache = cache;
        rt->expand_cache_free = expand_cache_destroy;
    }
    return rt->expand_cache;
}






void ctx_reader_invalidate(ExpandContext *ctx) {
    if (ctx->active_reader) {
        idm_reader_artifact_destroy(ctx->active_reader);
        ctx->active_reader = NULL;
    }
    ctx->reader_generation++;
}

const IdmReaderArtifact *ctx_active_reader(ExpandContext *ctx, IdmError *err) {
    if (ctx->active_reader) return ctx->active_reader;
    if (ctx->grammar_count == 0) return NULL;
    const char *surface = NULL;
    for (size_t i = 0; i < ctx->grammar_count; i++) {
        if (ctx->grammars[i].artifact.mode == (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE) surface = ctx->grammars[i].artifact.name;
    }
    if (!surface) return NULL;
    IdmReaderGrammarSource *sources = calloc(ctx->grammar_count, sizeof(*sources));
    if (!sources) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    for (size_t i = 0; i < ctx->grammar_count; i++) {
        sources[i].grammar = &ctx->grammars[i].artifact;
        sources[i].provider = ctx->grammars[i].provider;
        sources[i].provider_key = ctx->grammars[i].provider_key;
        sources[i].binding_scopes = &ctx->grammars[i].binding_scopes;
        sources[i].phase = 0;
    }
    bool ok = idm_reader_artifact_from_sources(surface, sources, ctx->grammar_count, &ctx->active_reader, err);
    free(sources);
    return ok ? ctx->active_reader : NULL;
}

static bool seed_statement_wrap(IdmSyntax *program, IdmError *err);

static bool seed_wrap_body_children(IdmSyntax *list, IdmError *err) {
    for (size_t i = 1; i < list->as.seq.count; i++) {
        IdmSyntax *child = list->as.seq.items[i];
        if (child->kind != IDM_SYN_LIST || child->as.seq.count == 0) continue;
        if (idm_syn_is_form_id(child, IDM_FORM_BODY)) {
            if (!seed_wrap_body_children(child, err)) return false;
            continue;
        }
        IdmSyntax *wrapped = idm_syn_list(child->span);
        IdmSyntax *expr = idm_syn_word("%-expr", child->span);
        if (!wrapped || !expr || !idm_syn_append(wrapped, expr)) {
            idm_syn_free(wrapped);
            if (!wrapped) idm_syn_free(expr);
            return idm_error_oom(err, child->span);
        }
        for (size_t k = 0; k < child->as.seq.count; k++) {
            if (!idm_syn_append(wrapped, child->as.seq.items[k])) return idm_error_oom(err, child->span);
        }
        child->as.seq.count = 0;
        idm_syn_free(child);
        list->as.seq.items[i] = wrapped;
        for (size_t k = 1; k < wrapped->as.seq.count; k++) {
            IdmSyntax *inner = wrapped->as.seq.items[k];
            if (idm_syn_is_form_id(inner, IDM_FORM_BODY)) {
                if (!seed_wrap_body_children(inner, err)) return false;
            }
        }
    }
    return true;
}

static bool seed_statement_wrap(IdmSyntax *program, IdmError *err) {
    return seed_wrap_body_children(program, err);
}

bool expand_unit_read(ExpandContext *ctx, const char *label, const char *source, size_t offset, IdmSyntax **out, IdmError *err) {
    *out = NULL;
    bool incomplete = false;
    ctx->unit_read_incomplete = false;
    const IdmReaderArtifact *artifact = ctx_active_reader(ctx, err);
    if (err && err->present) return false;
    if (artifact) {
        bool ok = idm_reader_read_artifact_string(artifact, label, source, offset, out, &incomplete, err);
        ctx->unit_read_incomplete = incomplete;
        if (!ok && incomplete) idm_error_reason(ctx->rt, err, "incomplete", 0);
        return ok;
    }
    IdmSyntax *terms = NULL;
    if (!idm_reader_read_terms_string(label, source, offset, &terms, &incomplete, err)) {
        ctx->unit_read_incomplete = incomplete;
        if (incomplete) idm_error_reason(ctx->rt, err, "incomplete", 0);
        return false;
    }
    ctx->unit_read_incomplete = incomplete;
    IdmSyntax *head = idm_syn_word("%-package-begin", terms->span);
    if (!head || !idm_syn_append(terms, head)) {
        idm_syn_free(terms);
        if (terms) idm_syn_free(head);
        return idm_error_oom(err, idm_span_unknown(label));
    }
    IdmSyntax **items = terms->as.seq.items;
    IdmSyntax *moved = items[terms->as.seq.count - 1u];
    for (size_t i = terms->as.seq.count - 1u; i > 0; i--) items[i] = items[i - 1u];
    items[0] = moved;
    if (!seed_statement_wrap(terms, err)) {
        idm_syn_free(terms);
        return false;
    }
    *out = terms;
    return true;
}

static bool bootstrap_path_at(IdmBuffer *path, const char *root, IdmError *err) {
    idm_buf_init(path);
    if ((root && (!idm_buf_append(path, root) || !idm_buf_append(path, "/"))) || !idm_buf_append(path, "std/kernel/bootstrap.id")) {
        idm_buf_destroy(path);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    return true;
}

static bool bootstrap_path(IdmBuffer *path, IdmError *err) {
    const char *env = getenv("IDIOMROOT");
    if (env && env[0]) {
        if (!bootstrap_path_at(path, env, err)) return false;
        if (access(path->data, R_OK) == 0) return true;
        idm_buf_destroy(path);
    }
    if (!bootstrap_path_at(path, NULL, err)) return false;
    if (access(path->data, R_OK) == 0) return true;
    const char *root = (env && env[0]) ? NULL : idm_root();
    if (root) {
        idm_buf_destroy(path);
        return bootstrap_path_at(path, root, err);
    }
    return true;
}

static ExpandCache *kernel_artifact_get(IdmRuntime *rt, IdmError *err);

static const IdmArtifact *kernel_protocol_art(const IdmArtifact *kernel, const char *name) {
    for (size_t i = 0; i < kernel->typed.entity_count; i++) {
        const IdmPkgTypedEntity *entity = &kernel->typed.entities[i];
        if (entity->kind != IDM_TYPED_ENTITY_PROTOCOL) continue;
        if (entity->as.protocol.name && strcmp(entity->as.protocol.name, name) == 0 && entity->as.protocol.art) return entity->as.protocol.art;
    }
    return NULL;
}

static const IdmArtifact *kernel_protocol_art_by_identity(const IdmArtifact *kernel, const char *identity) {
    for (size_t i = 0; i < kernel->typed.entity_count; i++) {
        const IdmPkgTypedEntity *entity = &kernel->typed.entities[i];
        if (entity->kind != IDM_TYPED_ENTITY_PROTOCOL) continue;
        if (entity->as.protocol.identity && strcmp(entity->as.protocol.identity, identity) == 0 && entity->as.protocol.art) return entity->as.protocol.art;
    }
    return NULL;
}

static bool source_reader_collect_grammars(const IdmArtifact *kernel, const IdmArtifact *art, IdmReaderGrammarSource **sources, size_t *count, size_t *cap, IdmError *err) {
    for (size_t i = 0; i < art->protocol_require_count; i++) {
        const IdmArtifact *req = kernel_protocol_art_by_identity(kernel, art->protocol_requires[i]);
        if (!req) continue;
        if (!source_reader_collect_grammars(kernel, req, sources, count, cap, err)) return false;
    }
    for (size_t i = 0; i < art->grammar_count; i++) {
        if (*count == *cap && !idm_grow((void **)sources, cap, sizeof(**sources), 4u, *count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
        memset(&(*sources)[*count], 0, sizeof(**sources));
        (*sources)[*count].grammar = &art->grammars[i];
        (*count)++;
    }
    return true;
}

static bool source_reader_get(IdmRuntime *rt, const IdmReaderArtifact **out, IdmError *err) {
    *out = NULL;
    ExpandCache *cache = expand_cache_get(rt);
    if (!cache) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!cache->source_reader) {
        ExpandCache *k = kernel_artifact_get(rt, err);
        if (!k) return false;
        const IdmArtifact *idiom = kernel_protocol_art(&k->kernel, "Idiom");
        if (!idiom) return idm_error_set(err, idm_span_unknown(NULL), "the kernel does not declare protocol Idiom");
        IdmReaderGrammarSource *sources = NULL;
        size_t count = 0;
        size_t cap = 0;
        if (!source_reader_collect_grammars(&k->kernel, idiom, &sources, &count, &cap, err)) {
            free(sources);
            return false;
        }
        if (count == 0) {
            free(sources);
            return idm_error_set(err, idm_span_unknown(NULL), "the kernel's Idiom protocol carries no grammar");
        }
        bool ok = idm_reader_artifact_from_sources("Idiom", sources, count, &cache->source_reader, err);
        free(sources);
        if (!ok) return false;
    }
    *out = cache->source_reader;
    return true;
}

static bool source_reader_hash_get(IdmRuntime *rt, unsigned char out[32], IdmError *err) {
    ExpandCache *cache = expand_cache_get(rt);
    if (!cache) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!cache->source_reader_hash_ready) {
        IdmBuffer path;
        if (!bootstrap_path(&path, err)) return false;
        char *src = NULL;
        size_t src_len = 0;
        bool read_ok = idm_read_file(path.data, &src, &src_len, err);
        idm_buf_destroy(&path);
        if (!read_ok) return false;
        idm_sha256(src ? src : "", src_len, cache->source_reader_hash);
        free(src);
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
    bool ok = idm_reader_read_artifact_string(reader, file, source, 0, out, NULL, err);
    if (ok && source) idm_profile_count("expand.read_source_string.bytes", (uint64_t)strlen(source));
    idm_profile_scope_end(&prof);
    return ok;
}

static bool package_units_cache_hash(IdmRuntime *rt, const IdmPackageUnit *units, size_t count, unsigned char out[32], IdmError *err) {
    if (count > UINT32_MAX) return idm_error_set(err, idm_span_unknown(NULL), "package has too many source files");
    unsigned char reader_hash[32];
    if (!source_reader_hash_get(rt, reader_hash, err)) return false;
    IdmBuffer data;
    idm_buf_init(&data);
    bool ok = idm_buf_append(&data, "IDM-PACKAGE-CACHE-v1" IDM_BUILD_ID) &&
              idm_buf_append_n(&data, (const char *)reader_hash, 32u) &&
              idm_buf_put_u32(&data, (uint32_t)count);
    for (size_t i = 0; ok && i < count; i++) {
        const char *label = units[i].label ? units[i].label : "";
        const char *slash = strrchr(label, '/');
        const char *name = slash ? slash + 1u : label;
        size_t source_len = units[i].source ? strlen(units[i].source) : 0u;
        ok = idm_buf_put_str(&data, name, strlen(name)) &&
             idm_buf_put_u64(&data, (uint64_t)source_len) &&
             idm_buf_append_n(&data, units[i].source ? units[i].source : "", source_len);
    }
    if (!ok) {
        idm_buf_destroy(&data);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    idm_sha256(data.data ? data.data : "", data.len, out);
    idm_buf_destroy(&data);
    return true;
}

static bool package_action_hash_current(IdmRuntime *rt, const char *path, unsigned char out[32], unsigned depth, IdmError *err) {
    if (depth >= IDM_IC_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(path), "package dependency graph is too deep");
    ExpandCache *cache = expand_cache_get(rt);
    if (!cache) return idm_error_oom(err, idm_span_unknown(path));
    for (size_t i = 0; i < cache->action_count; i++) {
        if (strcmp(cache->actions[i].path, path) == 0) {
            memcpy(out, cache->actions[i].hash, 32u);
            return true;
        }
    }
    IdmPackageUnit *units = NULL;
    size_t unit_count = 0;
    const char *label = NULL;
    const char *resolved = NULL;
    if (!idm_package_read_units(rt, path, &units, &unit_count, &label, &resolved, idm_span_unknown(path), err)) return false;
    unsigned char src_hash[32];
    bool ok = package_units_cache_hash(rt, units, unit_count, src_hash, err);
    idm_package_units_free(units, unit_count);
    if (!ok) return false;
    IdmArtifact action;
    if (!idm_artifact_action_cache_load(src_hash, &action)) return false;
    for (size_t i = 0; ok && i < action.dep_count; i++) {
        const IdmArtifactDep *dep = &action.deps[i];
        if (dep->kind == IDM_DEP_PACKAGE) {
            unsigned char current[32];
            ok = package_action_hash_current(rt, dep->path, current, depth + 1u, err) && memcmp(current, dep->hash, 32u) == 0;
        } else {
            ok = idm_artifact_dep_verified(rt, dep);
        }
    }
    if (ok) {
        if (cache->action_count == cache->action_cap &&
            !idm_grow((void **)&cache->actions, &cache->action_cap, sizeof(*cache->actions), 8u, cache->action_count + 1u)) {
            ok = idm_error_oom(err, idm_span_unknown(path));
        } else {
            CachedPackageAction *cached = &cache->actions[cache->action_count];
            cached->path = idm_strdup(path);
            if (!cached->path) {
                ok = idm_error_oom(err, idm_span_unknown(path));
            } else {
                memcpy(cached->hash, action.action_hash, 32u);
                cache->action_count++;
                memcpy(out, action.action_hash, 32u);
            }
        }
    }
    idm_artifact_destroy(&action);
    (void)label;
    (void)resolved;
    return ok;
}

static bool package_dep_verified(IdmRuntime *rt, const char *path, const unsigned char want[32], void *user) {
    if (!user) return false;
    IdmError err;
    idm_error_init(&err);
    unsigned char current[32];
    bool ok = package_action_hash_current(rt, path, current, 0u, &err) && memcmp(current, want, 32u) == 0;
    idm_error_clear(&err);
    return ok;
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
    if (!idm_buf_append(&path, "std/kernel")) {
        idm_buf_destroy(&path);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    cache->kernel_path = idm_buf_take(&path);
    if (!cache->kernel_path) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    IdmPackageUnit *units = NULL;
    size_t unit_count = 0;
    const char *units_label = NULL;
    const char *kernel_resolved = NULL;
    if (!idm_package_read_units(rt, cache->kernel_path, &units, &unit_count, &units_label, &kernel_resolved, idm_span_unknown(NULL), err)) return NULL;
    unsigned char src_hash[32];
    if (!package_units_cache_hash(rt, units, unit_count, src_hash, err)) {
        idm_package_units_free(units, unit_count);
        return NULL;
    }
    if (idm_artifact_cache_load(rt, kernel_resolved, src_hash, &cache->kernel, package_dep_verified, NULL)) {
        if (cache->kernel.scope_base >= rt->scope_next) {
            rt->scope_next = cache->kernel.scope_end;
            cache->kernel.rt_owned = true;
        }
        idm_package_units_free(units, unit_count);
        cache->kernel_scope_end = cache->kernel.scope_end;
        cache->kernel_ready = true;
        idm_profile_count("expand.kernel_artifact.cache_hit", 1u);
        idm_profile_scope_end(&prof);
        return cache;
    }
    IdmScopeStore store;
    idm_scope_store_init_shared(&store, &rt->scope_next);
    IdmScopeId base = idm_scope_store_next(&store);
    bool compiled = compile_package_artifact(rt, &store, NULL, units, unit_count, "std/kernel", true, src_hash, NULL, NULL, 0u, NULL, 0u, NULL, 0u, &cache->kernel, err);
    cache->kernel.rt_owned = true;
    idm_package_units_free(units, unit_count);
    if (!compiled) return NULL;
    cache->kernel.scope_base = base;
    cache->kernel.scope_end = idm_scope_store_next(&store);
    memcpy(cache->kernel.src_hash, src_hash, 32u);
    cache->kernel_scope_end = idm_scope_store_next(&store);
    if (!artifact_intern_literals_recursive(rt, &cache->kernel, err)) {
        idm_artifact_destroy(&cache->kernel);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    idm_artifact_cache_write(kernel_resolved, &cache->kernel);
    cache->kernel_ready = true;
    idm_profile_count("expand.kernel_artifact.compiled", 1u);
    idm_profile_scope_end(&prof);
    return cache;
}

static bool seed_kernel_phase_env(ExpandContext *ctx, IdmError *err) {
    if (!ctx->kernel_wrap || ctx->kernel_phase_seeded || !ctx->phase_env) return true;
    ExpandCache *cache = expand_cache_get(ctx->rt);
    if (!cache) return idm_error_oom(err, idm_span_unknown(NULL));
    if (cache->kernel_seed_phase_env) {
        ctx->kernel_phase_seeded = true;
        return true;
    }
    IdmCore *body = idm_core_literal(idm_nil(), idm_span_unknown(NULL));
    IdmCore *init = body ? wrap_kernel_use(ctx, body, err) : NULL;
    if (!init) {
        if (!body && err && !err->present) idm_error_oom(err, idm_span_unknown(NULL));
        return false;
    }
    bool ok = run_phase_core(ctx, init, err);
    idm_core_free(init);
    if (ok) {
        ctx->kernel_phase_seeded = true;
        cache->kernel_seed_phase_env = idm_phase_env_retain(ctx->phase_env);
    }
    return ok;
}

bool ctx_activate_kernel(ExpandContext *ctx, IdmError *err) {
    ExpandCache *cache = kernel_artifact_get(ctx->rt, err);
    if (!cache) return false;
    IdmArtifact *k = &cache->kernel;
    if (!artifact_record_consumer_deps(ctx, cache->kernel_path, k, err)) return false;
    idm_scope_store_bump_to(&ctx->scope_store, cache->kernel_scope_end);
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
    for (size_t i = 0; i < k->core_form_count; i++) {
        IdmPkgCoreForm *core_form = &k->core_form[i];
        if (!install_imported_core_form(ctx, core_form->name, &ctx->empty_scopes, core_form->module, core_form->function_index, core_form->phase_env, kernel_name, kernel_key, err)) return false;
    }
    for (size_t i = 0; i < k->reader_forms_count; i++) {
        if (!install_imported_reader_form(ctx, &k->reader_forms[i], &ctx->empty_scopes, NULL, NULL, err)) return false;
    }
    if (!install_artifact_grammars(ctx, k->grammars, k->grammar_count, &ctx->empty_scopes, k->scope_base, 0, kernel_name, kernel_key, err)) return false;
    if (!install_artifact_typed_registry(ctx, k, NULL, &ctx->empty_scopes, NULL, kernel_name, kernel_key, err)) return false;
    for (size_t i = 0; i < ctx->typed.entity_count; i++) {
        TypedEntity *entity = &ctx->typed.entities[i];
        if (entity->kind != IDM_TYPED_ENTITY_PROTOCOL) continue;
        if (!entity->as.protocol.name || strcmp(entity->as.protocol.name, "Idiom") != 0 || !entity->as.protocol.art) continue;
        if (!ctx_activate_protocol_closure(ctx, &entity->as.protocol, err)) return false;
        break;
    }
    {
        for (size_t i = 0; i < k->slot_count; i++) {
            const IdmScopeSet *bind_scopes = k->slots[i].exported ? &ctx->empty_scopes : &k->slots[i].scopes;
            if (!bind_package_slot_ref(ctx, k->slots[i].name, IDM_PHASE_ANY, bind_scopes, k->slots[i].arity, k->slots[i].has_contract ? &k->slots[i].contract : NULL, kernel_key, k->slots[i].slot, idm_span_unknown(NULL), err)) return false;
        }
    }
    ctx->kernel_wrap = k->slot_count != 0;
    return seed_kernel_phase_env(ctx, err);
}

static const ArtifactRuntimeState *artifact_runtime_state_by_mark(const ExpandContext *ctx, size_t mark);

typedef struct {
    const IdmArtifact **arts;
    size_t count;
    size_t cap;
    unsigned char (*seen)[32];
    size_t seen_count;
    size_t seen_cap;
} InitClosure;

static bool init_closure_visit(ExpandContext *ctx, InitClosure *c, const unsigned char hash[32], IdmError *err) {
    for (size_t i = 0; i < c->seen_count; i++) if (memcmp(c->seen[i], hash, 32u) == 0) return true;
    if (c->seen_count == c->seen_cap && !idm_grow((void **)&c->seen, &c->seen_cap, sizeof(*c->seen), 16u, c->seen_count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
    memcpy(c->seen[c->seen_count++], hash, 32u);
    const IdmArtifact *art = expand_cache_artifact_by_hash(ctx->rt, hash);
    if (!art) return true;
    for (size_t i = 0; i < art->dep_count; i++) {
        if (art->deps[i].kind != IDM_DEP_PACKAGE) continue;
        if (!init_closure_visit(ctx, c, art->deps[i].hash, err)) return false;
    }
    if (c->count == c->cap && !idm_grow((void **)&c->arts, &c->cap, sizeof(*c->arts), 16u, c->count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
    c->arts[c->count++] = art;
    return true;
}

IdmCore *wrap_kernel_use(ExpandContext *ctx, IdmCore *body, IdmError *err) {
    if (!ctx->kernel_wrap || !body) return body;
    ExpandCache *cache = expand_cache_get(ctx->rt);
    IdmArtifact *k = &cache->kernel;
    if (!k->module) {
        idm_core_free(body);
        idm_error_set(err, idm_span_unknown(NULL), "kernel package has no runtime module");
        return NULL;
    }
    InitClosure clo;
    memset(&clo, 0, sizeof(clo));
    bool ok = true;
    for (size_t i = 0; ok && i < ctx->artifact_base_count; i++) {
        const ArtifactRuntimeState *st = &ctx->artifact_bases[i];
        if (st->runtime_init_mark[0] == 0 && st->runtime_init_mark[1] == 0) continue;
        ok = init_closure_visit(ctx, &clo, st->hash, err);
    }
    if (ok) ok = init_closure_visit(ctx, &clo, k->src_hash, err);
    for (size_t i = clo.count; ok && i-- > 0; ) {
        const IdmArtifact *art = clo.arts[i];
        if (!art->module || !artifact_init_pending(ctx, art)) continue;
        body = wrap_artifact_runtime_init(ctx, art, art->scope_base, 0, art == k, false, body, idm_span_unknown(NULL), err);
        if (!body) ok = false;
    }
    free(clo.arts);
    free(clo.seen);
    return ok ? body : NULL;
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
    for (size_t i = 0; i < art->core_form_count; i++) {
        if (!module_ref_intern_literals(rt, art->core_form[i].module, err)) return false;
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
    p->art = entry->art ? (IdmArtifact *)expand_cache_artifact_by_hash(ctx->rt, entry->art->src_hash) : NULL;
    if (!p->art && entry->art) {
        IdmArtifact *clone = calloc(1u, sizeof(*clone));
        if (clone && artifact_clone(ctx->rt, entry->art, clone, err)) {
            p->art = expand_cache_intern_artifact(ctx->rt, clone);
        } else if (clone) {
            idm_artifact_destroy(clone);
            free(clone);
        }
    }
    if (!p->name || !protocol_def_set_identity(ctx, p, entry->identity, err, idm_span_unknown(NULL)) || !entry->art || !p->art) {
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
        uint32_t method_surface = UINT32_MAX;
        (void)method_surface_index_by_identity(ctx, src->trait, src->method, &method_surface);
        bool updated = false;
        for (size_t j = 0; j < ctx->typed.method_impl_count; j++) {
            MethodImplDef *dst = &ctx->typed.method_impls[j];
            if (!method_impl_matches_identity(ctx, dst, src->trait, src->method, src->type)) continue;
            char *next_key = idm_strdup(key ? key : "");
            if (!next_key) return idm_error_oom(err, idm_span_unknown(NULL));
            IdmCallableContract next_contract;
            memset(&next_contract, 0, sizeof(next_contract));
            if (src->has_contract && !idm_callable_contract_copy(&next_contract, &src->contract)) {
                free(next_key);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            if (method_surface != UINT32_MAX) dst->method_surface = method_surface;
            dst->arity = src->arity;
            dst->impl_env = src->impl_env;
            dst->impl_frame = ctx->frame;
            dst->impl_slot = src->impl_slot;
            if (dst->has_contract) idm_callable_contract_destroy(&dst->contract);
            dst->contract = next_contract;
            dst->has_contract = src->has_contract;
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
        dst->impl_frame = ctx->frame;
        dst->impl_env_key = idm_strdup(key ? key : "");
        dst->impl_slot = src->impl_slot;
        if (src->has_contract && idm_callable_contract_copy(&dst->contract, &src->contract)) dst->has_contract = true;
        else if (src->has_contract) {
            free(dst->impl_env_key);
            memset(dst, 0, sizeof(*dst));
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        if (!dst->impl_env_key || !method_impl_set_identity(ctx, dst, src->trait, src->method, src->type, err, idm_span_unknown(NULL))) {
            free(dst->impl_env_key);
            if (dst->has_contract) idm_callable_contract_destroy(&dst->contract);
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
    if (entry->member_count != 0) {
        t->members = calloc(entry->member_count, sizeof(*t->members));
        if (!t->members) {
            type_def_destroy(t);
            surface_rollback(ctx, &checkpoint);
            free(qualified);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        t->member_count = entry->member_count;
        for (size_t m = 0; m < entry->member_count; m++) {
            if (!idm_type_term_copy(&t->members[m].term, &entry->members[m].term)) {
                type_def_destroy(t);
                surface_rollback(ctx, &checkpoint);
                free(qualified);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
        }
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
            const IdmTypeTerm *contract = entry->fields[f].has_contract ? &entry->fields[f].contract : NULL;
            if (!type_field_set(ctx, &t->fields[f], entry->fields[f].name, contract, err, idm_span_unknown(NULL))) {
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
    return ctx_record_dep(ctx, path, art->action_hash, IDM_DEP_PACKAGE, err);
}

bool artifact_record_nested_deps(ExpandContext *ctx, const IdmArtifact *art, IdmError *err) {
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
    IdmPackageUnit *units = NULL;
    size_t unit_count = 0;
    const char *label = NULL;
    const char *resolved = NULL;
    if (!idm_package_read_units(ctx->rt, path, &units, &unit_count, &label, &resolved, span, err)) {
        free(entry->path);
        free(entry);
        return NULL;
    }
    unsigned char src_hash[32];
    if (!package_units_cache_hash(ctx->rt, units, unit_count, src_hash, err)) {
        idm_package_units_free(units, unit_count);
        free(entry->path);
        free(entry);
        return NULL;
    }
    bool loaded = idm_artifact_cache_load(ctx->rt, resolved, src_hash, &entry->art, package_dep_verified, ctx);
    if (loaded) {
        IdmScopeId span_size = entry->art.scope_end - entry->art.scope_base;
        IdmScopeStore store;
        idm_scope_store_init_shared(&store, &ctx->rt->scope_next);
        idm_scope_store_bump_to(&store, cache->kernel_scope_end);
        IdmScopeId fresh_base = idm_scope_reserve(&store, span_size);
        int64_t reloc_delta = (int64_t)fresh_base - (int64_t)entry->art.scope_base;
        if (reloc_delta != 0 && (!idm_artifact_relocate(ctx->rt, &entry->art, entry->art.scope_base, reloc_delta, err) ||
                                 !artifact_intern_literals_recursive(ctx->rt, &entry->art, err))) {
            idm_package_units_free(units, unit_count);
            idm_artifact_destroy(&entry->art);
            free(entry->path);
            free(entry);
            return NULL;
        }
        entry->art.rt_owned = true;
    }
    if (loaded) {
        bool kernel_matches = false;
        for (size_t i = 0; i < entry->art.dep_count; i++) {
            if (strcmp(entry->art.deps[i].path, cache->kernel_path) == 0 && memcmp(entry->art.deps[i].hash, cache->kernel.action_hash, 32u) == 0) {
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
        if (cache->compiling_count == cache->compiling_cap) {
            if (!idm_grow((void **)&cache->compiling, &cache->compiling_cap, sizeof(*cache->compiling), 8u, cache->compiling_count + 1u)) {
                idm_package_units_free(units, unit_count);
                free(entry->path);
                free(entry);
                idm_error_oom(err, span);
                return NULL;
            }
        }
        cache->compiling[cache->compiling_count++] = entry->path;
        IdmScopeStore store;
        idm_scope_store_init_shared(&store, &ctx->rt->scope_next);
        idm_scope_store_bump_to(&store, cache->kernel_scope_end);
        IdmScopeId pkg_base = idm_scope_store_next(&store);
        bool compiled = compile_package_artifact(ctx->rt, &store, NULL, units, unit_count, path, false, src_hash, NULL, NULL, 0u, NULL, 0u, NULL, 0u, &entry->art, err);
        entry->art.rt_owned = true;
        cache->compiling_count--;
        idm_package_units_free(units, unit_count);
        units = NULL;
        unit_count = 0;
        if (!compiled) {
            free(entry->path);
            free(entry);
            return NULL;
        }
        entry->art.scope_base = pkg_base;
        entry->art.scope_end = idm_scope_store_next(&store);
        memcpy(entry->art.src_hash, src_hash, 32u);
        if (!artifact_intern_literals_recursive(ctx->rt, &entry->art, err)) {
            idm_artifact_destroy(&entry->art);
            free(entry->path);
            free(entry);
            idm_error_oom(err, span);
            return NULL;
        }
        idm_artifact_cache_write(resolved, &entry->art);
        idm_profile_count("expand.artifact.compiled", 1u);
    } else {
        idm_profile_count("expand.artifact.cache_hit", 1u);
    }
    idm_package_units_free(units, unit_count);
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
    for (size_t i = 0; i < entry->art.dep_count; i++) {
        if (entry->art.deps[i].kind != IDM_DEP_PACKAGE) continue;
        if (cache->kernel_path && strcmp(entry->art.deps[i].path, cache->kernel_path) == 0) continue;
        if (!artifact_get(ctx, entry->art.deps[i].path, span, err)) return NULL;
    }
    idm_profile_count("expand.artifact.slots", (uint64_t)entry->art.slot_count);
    idm_profile_scope_end(&prof);
    return &entry->art;
}

bool idm_expand_package_artifact_serialize(IdmRuntime *rt, const char *path, IdmBuffer *out, IdmError *err) {
    ExpandContext ctx;
    if (!ctx_init(&ctx, rt)) {
        idm_error_oom(err, idm_span_unknown(path));
        ctx_destroy(&ctx);
        return false;
    }
    const IdmArtifact *art = artifact_get(&ctx, path, idm_span_unknown(path), err);
    bool ok = art && idm_artifact_serialize(art, out, err);
    ctx_destroy(&ctx);
    return ok;
}

bool idm_expand_package_action_hash(IdmRuntime *rt, const char *path, unsigned char out[32], IdmError *err) {
    return package_action_hash_current(rt, path, out, 0u, err);
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
    if (!art->rt_owned && art->scope_base != art->scope_end) {
        base = idm_scope_reserve(&ctx->scope_store, art->scope_end - art->scope_base);
    }
    ArtifactRuntimeState *state = &ctx->artifact_bases[ctx->artifact_base_count];
    memset(state, 0, sizeof(*state));
    state->art = art;
    memcpy(state->hash, art->src_hash, 32u);
    state->base = base;
    ctx->artifact_base_count++;
    *out_base = base;
    char provider_key[17];
    artifact_provider_key(art->src_hash, provider_key);
    return install_artifact_const_slots(ctx, art, provider_key, err);
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

static bool idm_artifact_relocate(IdmRuntime *rt, IdmArtifact *art, IdmScopeId min_id, int64_t delta, IdmError *err) {
    if (!art || delta == 0) return true;
    art->scope_base = (IdmScopeId)((int64_t)art->scope_base + delta);
    art->scope_end = (IdmScopeId)((int64_t)art->scope_end + delta);
    if (art->module && !idm_bc_relocate_syntax_scopes(rt, art->module, min_id, delta, err)) return false;
    for (size_t i = 0; i < art->slot_count; i++) idm_scope_set_relocate(&art->slots[i].scopes, min_id, delta);
    for (size_t i = 0; i < art->import_count; i++) idm_scope_set_relocate(&art->imports[i].scopes, min_id, delta);
    for (size_t i = 0; i < art->macro_count; i++)
        if (art->macros[i].module && !idm_bc_relocate_syntax_scopes(rt, &art->macros[i].module->module, min_id, delta, err)) return false;
    for (size_t i = 0; i < art->operator_count; i++) {
        idm_scope_set_relocate(&art->operators[i].scopes, min_id, delta);
        if (art->operators[i].target_module && !idm_bc_relocate_syntax_scopes(rt, &art->operators[i].target_module->module, min_id, delta, err)) return false;
    }
    for (size_t i = 0; i < art->core_form_count; i++)
        if (art->core_form[i].module && !idm_bc_relocate_syntax_scopes(rt, &art->core_form[i].module->module, min_id, delta, err)) return false;
    for (size_t i = 0; i < art->reader_forms_count; i++)
        if (art->reader_forms[i].module && !idm_bc_relocate_syntax_scopes(rt, &art->reader_forms[i].module->module, min_id, delta, err)) return false;
    for (size_t i = 0; i < art->grammar_count; i++) idm_scope_set_relocate(&art->grammars[i].scopes, min_id, delta);
    for (size_t i = 0; i < art->typed.entity_count; i++) {
        IdmPkgTypedEntity *e = &art->typed.entities[i];
        if (e->kind == IDM_TYPED_ENTITY_TYPE) idm_scope_set_relocate(&e->as.type.scopes, min_id, delta);
        else if (e->kind == IDM_TYPED_ENTITY_PROTOCOL && e->as.protocol.art && !idm_artifact_relocate(rt, e->as.protocol.art, min_id, delta, err)) return false;
    }
    if (art->phase_env) {
        for (size_t i = 0; i < art->phase_env->module_count; i++)
            if (art->phase_env->modules[i] && !idm_bc_relocate_syntax_scopes(rt, art->phase_env->modules[i], min_id, delta, err)) return false;
    }
    return true;
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
        !idm_bc_intern_literals(ctx->rt, module, err) ||
        !idm_bc_prepare_selectors(ctx->rt, module, err)) {
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

static bool install_macro_twin(ExpandContext *ctx, const char *name, IdmScopeId base, size_t macros_before, const char *provider, IdmError *err) {
    if (ctx->macro_count == macros_before || base == 0) return true;
    IdmScopeSet scopes;
    idm_scope_set_init(&scopes);
    bool ok = idm_scope_set_add(&scopes, base) && idm_binding_table_add(&ctx->bindings, name, IDM_PHASE_ANY, IDM_BIND_SPACE_DEFAULT, IDM_BIND_TRANSFORMER, &scopes, (uint32_t)(ctx->macro_count - 1u), ctx->frame, NULL);
    if (ok && provider && provider[0]) {
        IdmSymbol *sym = idm_intern(&ctx->rt->intern, IDM_SYMBOL_WORD, provider);
        if (sym) ctx->bindings.items[ctx->bindings.count - 1u].provider = idm_symbol_text(sym);
    }
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
    if (!macro->fn.module) {
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
    if (ok) {
        ok = idm_grammar_pairs_copy(src->pairs, src->pair_count, &dst->pairs);
        if (ok) dst->pair_count = src->pair_count;
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
        if (!installed || !install_macro_twin(ctx, art->macros[i].name, base, before, activation_name, err)) ok = false;
    }
    for (size_t i = 0; ok && i < art->core_form_count; i++) {
        const IdmPkgCoreForm *core_form = &art->core_form[i];
        IdmModuleRef *module = relocated_module_ref(ctx, core_form->module, min_id, delta, err);
        if (!module) {
            ok = false;
            break;
        }
        bool installed = install_imported_core_form(ctx, core_form->name, act_scopes, module, core_form->function_index, core_form->phase_env ? core_form->phase_env : ctx->phase_env, activation_name, provider_key, err);
        idm_module_ref_release(module);
        if (!installed) ok = false;
    }
    for (size_t i = 0; ok && i < art->reader_forms_count; i++) {
        const IdmPkgReaderForm *form = &art->reader_forms[i];
        IdmModuleRef *module = form->transformer ? relocated_module_ref(ctx, form->module, min_id, delta, err) : NULL;
        if (form->transformer && !module) { ok = false; break; }
        bool installed = install_imported_reader_form(ctx, form, act_scopes, module, form->phase_env ? form->phase_env : ctx->phase_env, err);
        if (module) idm_module_ref_release(module);
        if (!installed) ok = false;
    }
    if (ok && !install_artifact_grammars(ctx, art->grammars, art->grammar_count, act_scopes, min_id, delta, activation_name, provider_key, err)) ok = false;
    if (!ok) surface_rollback(ctx, &checkpoint);
    if (ok) {
        idm_profile_count("expand.activate_artifact.macros", (uint64_t)art->macro_count);
        idm_profile_count("expand.activate_artifact.core_form", (uint64_t)art->core_form_count);
        idm_profile_count("expand.activate_artifact.operators", (uint64_t)art->operator_count);
        idm_profile_count("expand.activate_artifact.grammars", (uint64_t)art->grammar_count);
    }
    idm_profile_scope_end(&prof);
    return ok;
}

static const char *primitive_home_for_std_suffix(const char *suffix) {
    if (!suffix) return NULL;
    return idm_primitive_home_exists(suffix) ? suffix : NULL;
}

static const char *canonical_primitive_home_for_unit(const char *unit) {
    if (!unit || unit[0] == '\0') return NULL;
    if (strcmp(unit, "kernel") == 0) return "kernel";
    if (strncmp(unit, "std/", 4u) == 0) return primitive_home_for_std_suffix(unit + 4u);
    const char *root = idm_root();
    size_t root_len = root ? strlen(root) : 0u;
    if (root_len != 0 && strncmp(unit, root, root_len) == 0 && strncmp(unit + root_len, "/std/", 5u) == 0) {
        return primitive_home_for_std_suffix(unit + root_len + 5u);
    }
    return NULL;
}

static char *trait_spelling_dup(const char *identity) {
    const char *start = strrchr(identity, '/');
    start = start ? start + 1u : identity;
    const char *suffix = strrchr(start, '#');
    return suffix ? idm_strndup(start, (size_t)(suffix - start)) : idm_strdup(start);
}

static bool compile_package_artifact(IdmRuntime *rt, IdmScopeStore *store, const IdmSyntax *pkg, const IdmPackageUnit *units, size_t unit_count, const char *unit_name_hint, bool kernel_mode, const unsigned char src_hash[32], const char *inherited_env_key, const IdmPkgSlot *inherited_slots, size_t inherited_count, const IdmPkgImport *inherited_imports, size_t inherited_import_count, const ProtocolPreActivation *preacts, size_t preact_count, IdmArtifact *out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, kernel_mode ? "expand.compile_kernel_artifact" : "expand.compile_package_artifact");
    IdmScopeStore input_store = *store;
    ExpandContext ctx;
    if (!ctx_init(&ctx, rt)) {
        idm_error_oom(err, pkg ? pkg->span : idm_span_unknown(NULL));
        ctx_destroy(&ctx);
        idm_profile_scope_end(&prof);
        return false;
    }
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
    ctx.kernel_mode = kernel_mode;
    bool ok = kernel_mode || ctx_activate_kernel(&ctx, err);
    if (ok && kernel_mode) {
        IdmEnforestNode floor_parts;
        memset(&floor_parts, 0, sizeof(floor_parts));
        floor_parts.op = (uint8_t)IDM_ENFOREST_PARTS;
        IdmEnforestNode floor_body_inner;
        memset(&floor_body_inner, 0, sizeof(floor_body_inner));
        floor_body_inner.op = (uint8_t)IDM_ENFOREST_BODY;
        IdmEnforestNode floor_scope;
        memset(&floor_scope, 0, sizeof(floor_scope));
        floor_scope.op = (uint8_t)IDM_ENFOREST_SCOPE;
        floor_scope.child = &floor_body_inner;
        ok = bind_reader_form(&ctx, "%-expr", &ctx.empty_scopes, &floor_parts, NULL, err) &&
             bind_reader_form(&ctx, "%-body", &ctx.empty_scopes, &floor_scope, NULL, err);
    }
    for (size_t i = 0; ok && i < preact_count; i++) {
        IdmScopeId base = 0;
        char runtime_key[17];
        artifact_provider_key(preacts[i].art->src_hash, runtime_key);
        ok = artifact_base(&ctx, preacts[i].art, &base, err) &&
             activate_artifact(&ctx, preacts[i].identity, preacts[i].art, base, &ctx.empty_scopes, idm_span_unknown(NULL), err) &&
             record_activation(&ctx, preacts[i].identity, preacts[i].identity, runtime_key, idm_span_unknown(NULL), err) &&
             install_artifact_runtime_slots_mode(&ctx, preacts[i].art, preacts[i].art->scope_base, (int64_t)base - (int64_t)preacts[i].art->scope_base, true, true, idm_span_unknown(NULL), err) &&
             bind_artifact_exported_slots(&ctx, preacts[i].art, &ctx.empty_scopes, idm_span_unknown(NULL), err);
    }
    size_t macro_base = ctx.macro_count;
    size_t op_base = ctx.operator_count;
    if (ok) ctx.in_package = true;
    if (ok) ok = install_inherited_package_slot_refs(&ctx, inherited_env_key, inherited_slots, inherited_count, err);
    if (ok) ok = install_inherited_import_refs(&ctx, inherited_imports, inherited_import_count, err);
    IdmSyntax *scoped_pkg = NULL;
    IdmScopeId package_scope = 0;
    if (ok && !units) {
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
    IdmCore *body = NULL;
    if (ok) {
        body = units ? expand_package_units(&ctx, units, unit_count, err)
                     : expand_package_body_items(&ctx, scoped_pkg->as.seq.items, 1, scoped_pkg->as.seq.count, err);
    }
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
    size_t slot_count = 0;
    IdmPkgSlot *slots = NULL;
    if (copy_ok && ctx.package_slot_count != 0) {
        slots = calloc(ctx.package_slot_count, sizeof(*slots));
        if (!slots) copy_ok = false;
        else {
            for (size_t i = 0; i < ctx.package_slot_count; i++) {
                bool superseded = false;
                for (size_t j = i + 1u; !superseded && j < ctx.package_slot_count; j++) {
                    superseded = ctx.package_slots[j].slot == ctx.package_slots[i].slot && strcmp(ctx.package_slots[j].name, ctx.package_slots[i].name) == 0;
                }
                if (superseded) continue;
                size_t k = slot_count;
                slots[k].name = idm_strdup(ctx.package_slots[i].name);
                slots[k].slot = ctx.package_slots[i].slot;
                slots[k].arity = ctx.package_slots[i].arity;
                slots[k].has_contract = false;
                if (ctx.package_slots[i].has_contract) {
                    if (!idm_callable_contract_copy(&slots[k].contract, &ctx.package_slots[i].contract)) {
                        for (size_t j = 0; j <= k; j++) idm_pkg_slot_destroy(&slots[j]);
                        copy_ok = false;
                        break;
                    }
                    slots[k].has_contract = true;
                }
                slots[k].exported = ctx.package_slots[i].exported;
                if (!slots[k].name || !idm_scope_set_copy(&slots[k].scopes, &ctx.package_slots[i].scopes)) {
                    for (size_t j = 0; j <= k; j++) idm_pkg_slot_destroy(&slots[j]);
                    free(slots);
                    slots = NULL;
                    copy_ok = false;
                    break;
                }
                slot_count++;
            }
        }
    }
    size_t import_count = 0;
    IdmPkgImport *imports = NULL;
    if (copy_ok && !collect_ctx_package_imports(&ctx, &imports, &import_count)) copy_ok = false;
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
                    if (copy_ok && t->member_count != 0) {
                        type->members = calloc(t->member_count, sizeof(*type->members));
                        if (!type->members) copy_ok = false;
                        else {
                            type->member_count = t->member_count;
                            for (size_t m = 0; m < t->member_count; m++) {
                                if (!idm_type_term_copy(&type->members[m].term, &t->members[m].term)) { copy_ok = false; break; }
                            }
                        }
                    }
                    if (copy_ok && t->field_count != 0) {
                        type->fields = calloc(t->field_count, sizeof(*type->fields));
                        if (!type->fields) copy_ok = false;
                        else {
                            type->field_count = t->field_count;
                            for (size_t f = 0; f < t->field_count; f++) {
                                const char *name = type_field_name_text(&t->fields[f]);
                                type->fields[f].name = idm_strdup(name);
                                if (!type->fields[f].name) { copy_ok = false; break; }
                                const IdmTypeTerm *contract = type_field_contract_term(&t->fields[f]);
                                if (contract) {
                                    if (!idm_type_term_copy(&type->fields[f].contract, contract)) { copy_ok = false; break; }
                                    type->fields[f].has_contract = true;
                                }
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
                    } else if (!p->art->rt_owned) {
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
            const char *type = method_impl_type_text(src);
            const char *trait = method_impl_trait_text(src);
            const char *method = method_impl_name_text(src);
            if (surface) {
                trait = method_surface_trait_text(surface);
                method = method_surface_name_text(surface);
            }
            if (!trait || !method || !type) {
                copy_ok = false;
                break;
            }
            dst->trait = idm_strdup(trait);
            dst->type = idm_strdup(type);
            dst->method = idm_strdup(method);
            dst->arity = src->arity;
            dst->impl_env = src->impl_env;
            dst->impl_env_key = idm_strdup(src->impl_env_key ? src->impl_env_key : "");
            dst->impl_slot = src->impl_slot;
            if (src->has_contract && idm_callable_contract_copy(&dst->contract, &src->contract)) dst->has_contract = true;
            else if (src->has_contract) copy_ok = false;
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
                d->action = ctx.operators[i].action;
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
    char *source_reader = NULL;
    size_t core_form_count = copy_ok ? ctx.decl_core_form_count : 0;
    IdmPkgCoreForm *core_form = NULL;
    if (copy_ok && core_form_count != 0) {
        core_form = calloc(core_form_count, sizeof(*core_form));
        if (!core_form) copy_ok = false;
        for (size_t i = 0; copy_ok && i < core_form_count; i++) {
            core_form[i].name = idm_strdup(ctx.decl_core_form[i].name);
            core_form[i].module = idm_module_ref_retain(ctx.decl_core_form[i].fn.module);
            core_form[i].function_index = ctx.decl_core_form[i].fn.function_index;
            core_form[i].phase_env = idm_phase_env_retain(ctx.decl_core_form[i].fn.phase_env);
            if (!core_form[i].name || !core_form[i].module) copy_ok = false;
        }
    }
    size_t reader_forms_count = copy_ok ? ctx.decl_reader_forms_count : 0;
    IdmPkgReaderForm *reader_forms = NULL;
    if (copy_ok && reader_forms_count != 0) {
        reader_forms = calloc(reader_forms_count, sizeof(*reader_forms));
        if (!reader_forms) copy_ok = false;
        for (size_t i = 0; copy_ok && i < reader_forms_count; i++) {
            reader_forms[i].name = idm_strdup(ctx.decl_reader_forms[i].name);
            if (!reader_forms[i].name) copy_ok = false;
            if (copy_ok && ctx.decl_reader_forms[i].node) {
                reader_forms[i].node = idm_enforest_clone(ctx.decl_reader_forms[i].node);
                if (!reader_forms[i].node) copy_ok = false;
            }
            if (copy_ok && ctx.decl_reader_forms[i].transformer) {
                reader_forms[i].transformer = true;
                reader_forms[i].module = idm_module_ref_retain(ctx.decl_reader_forms[i].fn.module);
                reader_forms[i].function_index = ctx.decl_reader_forms[i].fn.function_index;
                reader_forms[i].phase_env = idm_phase_env_retain(ctx.decl_reader_forms[i].fn.phase_env);
                if (!reader_forms[i].module) copy_ok = false;
            }
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
    if (copy_ok && ctx.kernel_mode) {
        body = wrap_kernel_use(&ctx, body, err);
        if (!body) copy_ok = false;
    }
    if (copy_ok) {
        module = malloc(sizeof(*module));
        if (module) {
            idm_bc_init(module);
            if (!idm_core_compile_main(rt, body, module, &init_fn, err)) { idm_bc_destroy(module); free(module); module = NULL; }
        }
    }
    idm_core_free(body);
    hooks_restore(rt, &saved);
    if (ctx.field_selector_count != 0) {
        out->field_selectors = calloc(ctx.field_selector_count, sizeof(*out->field_selectors));
        if (out->field_selectors) {
            for (size_t i = 0; i < ctx.field_selector_count; i++) {
                if (!ctx.field_selectors[i].env || !ctx.field_selectors[i].emitted || ctx.field_selectors[i].env_key) continue;
                out->field_selectors[out->field_selector_count].name = idm_strdup(ctx.field_selectors[i].name);
                if (!out->field_selectors[out->field_selector_count].name) continue;
                out->field_selectors[out->field_selector_count].env_key = idm_strdup(ctx.unit_key);
                out->field_selectors[out->field_selector_count].slot = ctx.field_selectors[i].slot;
                out->field_selector_count++;
            }
        }
    }
    if (module) {
        for (size_t i = 0; i < slot_count; i++) {
            if (!slots[i].has_contract || slots[i].contract.purity != IDM_PURITY_CONST) continue;
            const IdmBcEnvClosure *ec = idm_bc_env_closure_for_slot(module, ctx.unit_key, slots[i].slot);
            if (!ec) continue;
            slots[i].const_entries = malloc(ec->entry_count * sizeof(*slots[i].const_entries));
            if (!slots[i].const_entries) continue;
            memcpy(slots[i].const_entries, ec->entries, ec->entry_count * sizeof(*ec->entries));
            slots[i].const_entry_count = ec->entry_count;
        }
    }
    IdmScopeStore output_store = ctx.scope_store;
    *store = output_store;
    ctx_destroy(&ctx);
    if (!copy_ok || !module) {
        *store = input_store;
        free(name);
        if (slots) { for (size_t i = 0; i < slot_count; i++) idm_pkg_slot_destroy(&slots[i]); free(slots); }
        if (imports) { for (size_t i = 0; i < import_count; i++) idm_pkg_import_destroy(&imports[i]); free(imports); }
        if (macros) { for (size_t i = 0; i < macro_count; i++) idm_pkg_macro_destroy(&macros[i]); free(macros); }
        if (operators) { for (size_t i = 0; i < op_count; i++) idm_operator_def_destroy(&operators[i]); free(operators); }
        if (grammars) { for (size_t i = 0; i < grammar_count; i++) idm_pkg_grammar_destroy(&grammars[i]); free(grammars); }
        free(source_reader);
        if (pkg_entities) { for (size_t i = 0; i < pkg_entity_count; i++) idm_pkg_typed_entity_destroy(&pkg_entities[i]); free(pkg_entities); }
        if (pkg_method_impls) { for (size_t i = 0; i < pkg_method_impl_count; i++) idm_pkg_method_impl_destroy(&pkg_method_impls[i]); free(pkg_method_impls); }
        if (core_form) { for (size_t i = 0; i < core_form_count; i++) idm_pkg_core_form_destroy(&core_form[i]); free(core_form); }
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
    out->imports = imports;
    out->import_count = import_count;
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
    out->core_form = core_form;
    out->core_form_count = core_form_count;
    out->reader_forms = reader_forms;
    out->reader_forms_count = reader_forms_count;
    out->phase_env = pkg_phase_env;
    out->deps = pkg_deps;
    out->dep_count = pkg_dep_count;
    memcpy(out->src_hash, src_hash, 32u);
    if (!idm_artifact_compute_action_hash(out, err)) {
        idm_artifact_destroy(out);
        idm_profile_scope_end(&prof);
        return false;
    }
    idm_profile_count(kernel_mode ? "expand.compile_kernel_artifact.slots" : "expand.compile_package_artifact.slots", (uint64_t)slot_count);
    idm_profile_count(kernel_mode ? "expand.compile_kernel_artifact.functions" : "expand.compile_package_artifact.functions", module ? (uint64_t)module->function_count : 0u);
    idm_profile_count(kernel_mode ? "expand.compile_kernel_artifact.code_words" : "expand.compile_package_artifact.code_words", module ? (uint64_t)module->code_count : 0u);
    idm_profile_scope_end(&prof);
    return true;
}

bool compile_package_module(ExpandContext *parent, const IdmSyntax *pkg, const char *unit_name_hint, const unsigned char src_hash[32], const IdmPkgSlot *inherited_slots, size_t inherited_count, const ProtocolPreActivation *preacts, size_t preact_count, IdmArtifact *out, IdmError *err) {
    IdmPkgImport *parent_imports = NULL;
    size_t parent_import_count = 0;
    if (!collect_ctx_package_imports(parent, &parent_imports, &parent_import_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    bool ok = compile_package_artifact(parent->rt, &parent->scope_store, pkg, NULL, 0u, unit_name_hint, parent->kernel_mode, src_hash, parent->unit_key, inherited_slots, inherited_count, parent_imports, parent_import_count, preacts, preact_count, out, err);
    for (size_t i = 0; i < parent_import_count; i++) idm_pkg_import_destroy(&parent_imports[i]);
    free(parent_imports);
    return ok;
}
