#include "internal.h"

static void expand_cache_destroy(void *p);
static ExpandCache *expand_cache_get(IdmRuntime *rt);
static ExpandCache *kernel_artifact_get(IdmRuntime *rt, IdmError *err);
static bool ctx_record_dep(ExpandContext *ctx, const char *path, const unsigned char hash[32], uint8_t kind, IdmError *err);
static bool artifact_record_consumer_deps(ExpandContext *ctx, const char *path, const IdmArtifact *art, IdmError *err);
static IdmModuleRef *relocated_module_ref(ExpandContext *ctx, IdmModuleRef *src, IdmScopeId min_id, int64_t delta, IdmError *err);
static bool install_macro_twin(ExpandContext *ctx, const char *name, IdmScopeId base, size_t macros_before, IdmError *err);
static bool install_relocated_operator(ExpandContext *ctx, const IdmOperatorDef *op, IdmScopeId min_id, int64_t delta, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, IdmError *err);
static const char *idm_std_root(void);
static bool compile_package_artifact(IdmRuntime *rt, IdmScopeStore *store, const IdmSyntax *pkg, const char *unit_name_hint, bool kernel_mode, const unsigned char src_hash[32], IdmArtifact *out, IdmError *err);

static void import_names_free(char **names, size_t count) {
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}

bool runtime_globals_imported(const ExpandContext *ctx, const IdmArtifact *art) {
    for (size_t i = 0; i < ctx->runtime_import_count; i++) {
        if (ctx->runtime_imports[i].art == art && ctx->runtime_imports[i].phase == ctx->phase) return true;
    }
    return false;
}

bool record_runtime_globals_import(ExpandContext *ctx, const IdmArtifact *art, IdmSpan span, IdmError *err) {
    if (runtime_globals_imported(ctx, art)) return true;
    if (ctx->runtime_import_count == ctx->runtime_import_cap) {
        size_t cap = ctx->runtime_import_cap ? ctx->runtime_import_cap * 2u : 8u;
        void *grown = realloc(ctx->runtime_imports, cap * sizeof(*ctx->runtime_imports));
        if (!grown) return idm_error_oom(err, span);
        ctx->runtime_imports = grown;
        ctx->runtime_import_cap = cap;
    }
    ctx->runtime_imports[ctx->runtime_import_count].art = art;
    ctx->runtime_imports[ctx->runtime_import_count].phase = ctx->phase;
    ctx->runtime_import_count++;
    return true;
}

IdmCore *expand_use(ExpandContext *ctx, const char *path, const char *qualifier, IdmSyntax *const *items, size_t cont_index, size_t cont_count, IdmSpan span, IdmError *err) {
    const IdmArtifact *art = artifact_get(ctx, path, span, err);
    if (!art) {
        if (err && err->present && span.line != 0) {
            idm_error_note(err, "while loading package '%s' (%s:%u:%u)", path, span.file ? span.file : "<unknown>", span.line, span.column);
        } else if (err && err->present) {
            idm_error_note(err, "while loading package '%s'", path);
        }
        return NULL;
    }
    IdmScopeId base = 0;
    if (!artifact_base(ctx, art, &base, err)) return NULL;
    IdmScopeId min_id = art->scope_base;
    int64_t delta = (int64_t)base - (int64_t)art->scope_base;
    IdmScopeSet act_scopes;
    if (!binder_scopes_pruned(ctx, items[cont_index - 1u], &act_scopes)) return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    const char *provider = art->name ? art->name : path;
    char provider_key[17];
    artifact_provider_key(art->src_hash, provider_key);

    bool import_globals = art->global_count != 0 && !runtime_globals_imported(ctx, art);
    size_t import_count = art->export_count + (import_globals ? art->global_count : 0u);
    uint32_t *export_src = NULL;
    uint32_t *export_dst = NULL;
    char **export_names = NULL;
    if (import_count != 0) {
        export_src = malloc(import_count * sizeof(*export_src));
        export_dst = malloc(import_count * sizeof(*export_dst));
        export_names = calloc(import_count, sizeof(*export_names));
        if (!export_src || !export_dst || !export_names) {
            free(export_src);
            free(export_dst);
            free(export_names);
            idm_scope_set_destroy(&act_scopes);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
    }
    bool ok = true;
    size_t import_index = 0;
    for (size_t i = 0; i < art->export_count && ok; i++) {
        uint32_t consumer_slot = ctx->global_seq++;
        export_src[import_index] = art->exports[i].slot;
        export_dst[import_index] = consumer_slot;
        const char *bind_name = art->exports[i].name;
        char *qualified = NULL;
        if (qualifier) {
            IdmBuffer qb;
            idm_buf_init(&qb);
            if (!idm_buf_append(&qb, qualifier) || !idm_buf_append_char(&qb, '.') || !idm_buf_append(&qb, art->exports[i].name)) { idm_buf_destroy(&qb); ok = false; break; }
            qualified = idm_buf_take(&qb);
            bind_name = qualified;
        }
        export_names[import_index] = idm_strdup(bind_name);
        if (!export_names[import_index]) ok = false;
        import_index++;
        ok = ok && idm_binding_table_add_with_arity(&ctx->bindings, bind_name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_GLOBAL, &act_scopes, consumer_slot, IDM_FRAME_GLOBAL, art->exports[i].arity, NULL);
        free(qualified);
    }
    for (size_t i = 0; i < (import_globals ? art->global_count : 0u) && ok; i++) {
        uint32_t consumer_slot = ctx->global_seq++;
        export_src[import_index] = art->globals[i].slot;
        export_dst[import_index] = consumer_slot;
        export_names[import_index] = idm_strdup(art->globals[i].name);
        if (!export_names[import_index]) { ok = false; break; }
        import_index++;
        IdmScopeSet gscopes;
        idm_scope_set_init(&gscopes);
        if (!idm_scope_set_copy(&gscopes, &art->globals[i].scopes)) { ok = false; break; }
        idm_scope_set_relocate(&gscopes, min_id, delta);
        ok = idm_binding_table_add_with_arity(&ctx->bindings, art->globals[i].name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_GLOBAL, &gscopes, consumer_slot, IDM_FRAME_GLOBAL, art->globals[i].arity, NULL);
        idm_scope_set_destroy(&gscopes);
    }
    for (size_t i = 0; qualifier && i < art->macro_count && ok; i++) {
        IdmBuffer qb;
        idm_buf_init(&qb);
        if (!idm_buf_append(&qb, qualifier) || !idm_buf_append_char(&qb, '.') || !idm_buf_append(&qb, art->macros[i].name)) {
            idm_buf_destroy(&qb);
            idm_error_oom(err, span);
            ok = false;
            break;
        }
        char *qualified = idm_buf_take(&qb);
        IdmModuleRef *module = relocated_module_ref(ctx, art->macros[i].module, min_id, delta, err);
        if (!module) {
            free(qualified);
            ok = false;
            break;
        }
        size_t before = ctx->macro_count;
        ok = install_imported_macro(ctx, qualified, &act_scopes, module, art->macros[i].function_index, art->macros[i].phase_ns, art->macros[i].phase_env, provider, provider_key, err) &&
             install_macro_twin(ctx, art->macros[i].name, base, before, err);
        idm_module_ref_release(module);
        free(qualified);
    }
    if (ok) ok = install_artifact_traits(ctx, art->traits, art->trait_count, &act_scopes, qualifier, provider, provider_key, err);
    if (ok) ok = install_artifact_types(ctx, art->types, art->type_count, &act_scopes, qualifier, provider, provider_key, err);
    idm_scope_set_destroy(&act_scopes);
    if (!ok) {
        free(export_src);
        free(export_dst);
        import_names_free(export_names, import_count);
        if (err && !err->present) idm_error_oom(err, span);
        return NULL;
    }
    if (import_globals && !record_runtime_globals_import(ctx, art, span, err)) {
        free(export_src);
        free(export_dst);
        import_names_free(export_names, import_count);
        return NULL;
    }

    IdmValue name_value = idm_atom(ctx->rt, art->name ? art->name : path);
    bool init_pending = artifact_init_pending(ctx, art);
    IdmCore *cont = expand_body_items(ctx, items, cont_index, cont_count, false, err);
    if (!cont) { free(export_src); free(export_dst); import_names_free(export_names, import_count); return NULL; }

    uint32_t fn_off = 0;
    IdmBytecodeModule *module = NULL;
    if (init_pending) {
        module = relocated_module_copy(ctx, art->module, min_id, delta, &fn_off, err);
        if (!module) {
            free(export_src);
            free(export_dst);
            import_names_free(export_names, import_count);
            idm_core_free(cont);
            return NULL;
        }
    }
    IdmCore *use_core = idm_core_use_package(name_value, module, art->init_fn + fn_off, export_src, export_dst, export_names, import_count, cont, span);
    if (!use_core) {
        free(export_src);
        free(export_dst);
        import_names_free(export_names, import_count);
        idm_bc_destroy(module);
        free(module);
        idm_core_free(cont);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
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

static ExpandCache *kernel_artifact_get(IdmRuntime *rt, IdmError *err) {
    ExpandCache *cache = expand_cache_get(rt);
    if (!cache) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    if (cache->kernel_ready) return cache;
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
    idm_sha256(src.data ? src.data : "", src.len, src_hash);
    cache->kernel_path = idm_buf_take(&path);
    if (!cache->kernel_path) {
        idm_buf_destroy(&src);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    if (idm_artifact_cache_load(rt, cache->kernel_path, src_hash, &cache->kernel)) {
        idm_buf_destroy(&src);
        cache->kernel_scope_end = cache->kernel.scope_end;
        cache->kernel_ready = true;
        return cache;
    }
    IdmSyntax *pkg = NULL;
    bool read_ok = idm_reader_read_string(label, src.data ? src.data : "", &pkg, err);
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
    bool compiled = compile_package_artifact(rt, &store, pkg, "std/kernel", true, src_hash, &cache->kernel, err);
    idm_syn_free(pkg);
    if (!compiled) return NULL;
    cache->kernel.scope_base = base;
    cache->kernel.scope_end = store.next_scope;
    memcpy(cache->kernel.src_hash, src_hash, 32u);
    cache->kernel_scope_end = store.next_scope;
    if (cache->kernel.module && !idm_bc_intern_literals(rt, cache->kernel.module, err)) {
        idm_artifact_destroy(&cache->kernel);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    idm_artifact_cache_write(cache->kernel_path, &cache->kernel);
    cache->kernel_ready = true;
    return cache;
}

bool ctx_activate_kernel(ExpandContext *ctx, IdmError *err) {
    ExpandCache *cache = kernel_artifact_get(ctx->rt, err);
    if (!cache) return false;
    IdmArtifact *k = &cache->kernel;
    if (!artifact_record_consumer_deps(ctx, cache->kernel_path, k, err)) return false;
    if (!record_activation(ctx, "std/kernel", idm_span_unknown(NULL), err)) return false;
    if (ctx->scope_store.next_scope < cache->kernel_scope_end) ctx->scope_store.next_scope = cache->kernel_scope_end;
    const char *kernel_name = k->name ? k->name : "std/kernel";
    char kernel_key[17];
    artifact_provider_key(k->src_hash, kernel_key);
    for (size_t i = 0; i < k->operator_count; i++) {
        if (!install_imported_operator(ctx, &k->operators[i], &ctx->empty_scopes, kernel_name, kernel_key, err)) return false;
    }
    for (size_t i = 0; i < k->macro_count; i++) {
        if (!install_imported_macro(ctx, k->macros[i].name, &ctx->empty_scopes, k->macros[i].module, k->macros[i].function_index, k->macros[i].phase_ns, k->macros[i].phase_env, kernel_name, kernel_key, err)) return false;
    }
    if (k->resolver_module) {
        if (!install_imported_resolver(ctx, &ctx->empty_scopes, k->resolver_module, k->resolver_fn, k->resolver_phase_ns, k->resolver_phase_env, kernel_name, kernel_key, err)) return false;
    }
    if (!install_artifact_traits(ctx, k->traits, k->trait_count, &ctx->empty_scopes, NULL, kernel_name, kernel_key, err)) return false;
    if (!install_artifact_types(ctx, k->types, k->type_count, &ctx->empty_scopes, NULL, kernel_name, kernel_key, err)) return false;
    size_t import_count = k->export_count + k->global_count;
    if (import_count != 0) {
        ctx->kernel_import_src = malloc(import_count * sizeof(*ctx->kernel_import_src));
        ctx->kernel_import_dst = malloc(import_count * sizeof(*ctx->kernel_import_dst));
        ctx->kernel_import_names = calloc(import_count, sizeof(*ctx->kernel_import_names));
        ctx->kernel_import_count = import_count;
        if (!ctx->kernel_import_src || !ctx->kernel_import_dst || !ctx->kernel_import_names) return idm_error_oom(err, idm_span_unknown(NULL));
        size_t n = 0;
        for (size_t i = 0; i < k->export_count; i++) {
            uint32_t consumer_slot = ctx->global_seq++;
            ctx->kernel_import_src[n] = k->exports[i].slot;
            ctx->kernel_import_dst[n] = consumer_slot;
            ctx->kernel_import_names[n] = idm_strdup(k->exports[i].name);
            if (!ctx->kernel_import_names[n]) return idm_error_oom(err, idm_span_unknown(NULL));
            n++;
            if (!idm_binding_table_add_with_arity(&ctx->bindings, k->exports[i].name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_GLOBAL, &ctx->empty_scopes, consumer_slot, IDM_FRAME_GLOBAL, k->exports[i].arity, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
        }
        for (size_t i = 0; i < k->global_count; i++) {
            uint32_t consumer_slot = ctx->global_seq++;
            ctx->kernel_import_src[n] = k->globals[i].slot;
            ctx->kernel_import_dst[n] = consumer_slot;
            ctx->kernel_import_names[n] = idm_strdup(k->globals[i].name);
            if (!ctx->kernel_import_names[n]) return idm_error_oom(err, idm_span_unknown(NULL));
            n++;
            if (!idm_binding_table_add_with_arity(&ctx->bindings, k->globals[i].name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_GLOBAL, &k->globals[i].scopes, consumer_slot, IDM_FRAME_GLOBAL, k->globals[i].arity, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    ctx->kernel_wrap = import_count != 0;
    return true;
}

IdmCore *wrap_kernel_use(ExpandContext *ctx, IdmCore *body, IdmError *err) {
    if (!ctx->kernel_wrap || !body) return body;
    ExpandCache *cache = expand_cache_get(ctx->rt);
    IdmArtifact *k = &cache->kernel;
    IdmBytecodeModule *module = malloc(sizeof(*module));
    if (!module) {
        idm_core_free(body);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, idm_span_unknown(NULL));
    }
    idm_bc_init(module);
    uint32_t const_off = 0, fn_off = 0, code_off = 0;
    if (!idm_bc_link(module, k->module, &const_off, &fn_off, &code_off, err)) {
        idm_bc_destroy(module);
        free(module);
        idm_core_free(body);
        return NULL;
    }
    size_t n = ctx->kernel_import_count;
    uint32_t *src = NULL;
    uint32_t *dst = NULL;
    char **names = NULL;
    if (n != 0) {
        src = malloc(n * sizeof(*src));
        dst = malloc(n * sizeof(*dst));
        names = calloc(n, sizeof(*names));
        if (!src || !dst || !names) {
            free(src);
            free(dst);
            free(names);
            idm_bc_destroy(module);
            free(module);
            idm_core_free(body);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, idm_span_unknown(NULL));
        }
        memcpy(src, ctx->kernel_import_src, n * sizeof(*src));
        memcpy(dst, ctx->kernel_import_dst, n * sizeof(*dst));
        for (size_t i = 0; i < n; i++) {
            names[i] = idm_strdup(ctx->kernel_import_names[i]);
            if (!names[i]) {
                free(src);
                free(dst);
                import_names_free(names, n);
                idm_bc_destroy(module);
                free(module);
                idm_core_free(body);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, idm_span_unknown(NULL));
            }
        }
    }
    IdmValue name_value = idm_atom(ctx->rt, k->name ? k->name : "std/kernel");
    IdmCore *wrapped = idm_core_use_package(name_value, module, k->init_fn + fn_off, src, dst, names, n, body, idm_span_unknown(NULL));
    if (!wrapped) {
        free(src);
        free(dst);
        import_names_free(names, n);
        idm_bc_destroy(module);
        free(module);
        idm_core_free(body);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, idm_span_unknown(NULL));
    }
    return wrapped;
}

void artifact_provider_key(const unsigned char hash[32], char out[17]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 8u; i++) {
        out[i * 2u] = hex[hash[i] >> 4];
        out[i * 2u + 1u] = hex[hash[i] & 0xfu];
    }
    out[16] = '\0';
}

bool install_artifact_traits(ExpandContext *ctx, const IdmPkgTrait *traits, size_t trait_count, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err) {
    if (!scopes) scopes = &ctx->empty_scopes;
    for (size_t i = 0; i < trait_count; i++) {
        const IdmPkgTrait *entry = &traits[i];
        SurfaceCheckpoint checkpoint;
        surface_checkpoint(ctx, &checkpoint);
        char *qualified = NULL;
        const char *bind_name = entry->name;
        if (qualifier) {
            IdmBuffer qb;
            idm_buf_init(&qb);
            if (!idm_buf_append(&qb, qualifier) || !idm_buf_append_char(&qb, '.') || !idm_buf_append(&qb, entry->name)) {
                idm_buf_destroy(&qb);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            qualified = idm_buf_take(&qb);
            bind_name = qualified;
        }
        int guard = surface_install_guard(ctx, provider, provider_key, bind_name, bind_name, IDM_BIND_SPACE_TRAIT, scopes, err);
        if (guard < 0) {
            surface_rollback(ctx, &checkpoint);
            free(qualified);
            return false;
        }
        if (guard == 0) {
            free(qualified);
            continue;
        }
        if (ctx->trait_count == ctx->trait_cap) {
            size_t cap = ctx->trait_cap ? ctx->trait_cap * 2u : 4u;
            TraitDef *next = realloc(ctx->traits, cap * sizeof(*next));
            if (!next) {
                surface_rollback(ctx, &checkpoint);
                free(qualified);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            ctx->traits = next;
            ctx->trait_cap = cap;
        }
        TraitDef *t = &ctx->traits[ctx->trait_count];
        memset(t, 0, sizeof(*t));
        t->name = idm_strdup(entry->identity);
        if (!t->name ||
            !idm_trait_requirement_defs_copy(entry->requirements, entry->requirement_count, &t->requirements) ||
            !idm_trait_method_defs_copy(entry->methods, entry->method_count, &t->methods)) {
            trait_def_destroy(t);
            surface_rollback(ctx, &checkpoint);
            free(qualified);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        t->requirement_count = entry->requirement_count;
        t->method_count = entry->method_count;
        uint32_t payload = (uint32_t)ctx->trait_count;
        if (!idm_binding_table_add(&ctx->bindings, bind_name, IDM_PHASE_ANY, IDM_BIND_SPACE_TRAIT, IDM_BIND_TRAIT, scopes, payload, ctx->frame, NULL)) {
            trait_def_destroy(t);
            surface_rollback(ctx, &checkpoint);
            free(qualified);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        ctx->trait_count++;
        if (!qualifier) {
            for (size_t m = 0; m < entry->method_count; m++) {
                if (!install_method_surface(ctx, entry->identity, entry->methods[m].name, entry->methods[m].arity, false, scopes, provider, provider_key, err)) {
                    surface_rollback(ctx, &checkpoint);
                    free(qualified);
                    return false;
                }
            }
        }
        free(qualified);
    }
    return true;
}

bool install_artifact_types(ExpandContext *ctx, const IdmPkgType *types, size_t type_count, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err) {
    if (!scopes) scopes = &ctx->empty_scopes;
    for (size_t i = 0; i < type_count; i++) {
        const IdmPkgType *entry = &types[i];
        char *qualified = NULL;
        const char *bind_name = entry->name;
        if (qualifier) {
            IdmBuffer qb;
            idm_buf_init(&qb);
            if (!idm_buf_append(&qb, qualifier) || !idm_buf_append_char(&qb, '.') || !idm_buf_append(&qb, entry->name)) {
                idm_buf_destroy(&qb);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            qualified = idm_buf_take(&qb);
            bind_name = qualified;
        }
        int guard = surface_install_guard(ctx, provider, provider_key, bind_name, bind_name, IDM_BIND_SPACE_TYPE, scopes, err);
        if (guard < 0) {
            free(qualified);
            return false;
        }
        if (guard == 0) {
            free(qualified);
            continue;
        }
        SurfaceCheckpoint checkpoint;
        surface_checkpoint(ctx, &checkpoint);
        if (ctx->type_count == ctx->type_cap) {
            size_t cap = ctx->type_cap ? ctx->type_cap * 2u : 4u;
            TypeDef *next = realloc(ctx->types, cap * sizeof(*next));
            if (!next) {
                surface_rollback(ctx, &checkpoint);
                free(qualified);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            ctx->types = next;
            ctx->type_cap = cap;
        }
        TypeDef *t = &ctx->types[ctx->type_count];
        memset(t, 0, sizeof(*t));
        t->name = idm_strdup(bind_name);
        t->identity = idm_strdup(entry->identity);
        if (!t->name || !t->identity || !idm_scope_set_copy(&t->scopes, scopes)) {
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
                t->fields[f] = idm_strdup(entry->fields[f]);
                if (!t->fields[f]) {
                    type_def_destroy(t);
                    surface_rollback(ctx, &checkpoint);
                    free(qualified);
                    return idm_error_oom(err, idm_span_unknown(NULL));
                }
            }
        }
        uint32_t payload = (uint32_t)ctx->type_count;
        if (!idm_binding_table_add(&ctx->bindings, bind_name, IDM_PHASE_ANY, IDM_BIND_SPACE_TYPE, IDM_BIND_TYPE, scopes, payload, ctx->frame, NULL)) {
            type_def_destroy(t);
            surface_rollback(ctx, &checkpoint);
            free(qualified);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        ctx->type_count++;
        if (!qualifier) {
            for (size_t f = 0; f < entry->field_count; f++) {
                if (!install_method_surface(ctx, entry->identity, entry->fields[f], idm_arity_exact(1u), true, scopes, provider, provider_key, err)) {
                    surface_rollback(ctx, &checkpoint);
                    free(qualified);
                    return false;
                }
            }
        }
        free(qualified);
    }
    return true;
}

static bool ctx_record_dep(ExpandContext *ctx, const char *path, const unsigned char hash[32], uint8_t kind, IdmError *err) {
    for (size_t i = 0; i < ctx->dep_count; i++) {
        if (strcmp(ctx->deps[i].path, path) == 0) return true;
    }
    if (ctx->dep_count == ctx->dep_cap) {
        size_t cap = ctx->dep_cap ? ctx->dep_cap * 2u : 8u;
        IdmArtifactDep *grown = realloc(ctx->deps, cap * sizeof(*grown));
        if (!grown) return idm_error_oom(err, idm_span_unknown(NULL));
        ctx->deps = grown;
        ctx->dep_cap = cap;
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
    ExpandCache *cache = kernel_artifact_get(ctx->rt, err);
    if (!cache) return NULL;
    for (size_t i = 0; i < cache->pkg_count; i++) {
        if (strcmp(cache->pkgs[i]->path, path) == 0) {
            if (!artifact_record_consumer_deps(ctx, path, &cache->pkgs[i]->art, err)) return NULL;
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
    idm_sha256(src.data ? src.data : "", src.len, src_hash);
    bool loaded = idm_artifact_cache_load(ctx->rt, path, src_hash, &entry->art);
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
        bool read_ok = idm_reader_read_string(label, src.data ? src.data : "", &pkg, err);
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
            size_t cap = cache->compiling_cap ? cache->compiling_cap * 2u : 8u;
            const char **grown = realloc(cache->compiling, cap * sizeof(*grown));
            if (!grown) {
                idm_syn_free(pkg);
                idm_buf_destroy(&src);
                free(entry->path);
                free(entry);
                idm_error_oom(err, span);
                return NULL;
            }
            cache->compiling = grown;
            cache->compiling_cap = cap;
        }
        cache->compiling[cache->compiling_count++] = entry->path;
        IdmScopeStore store;
        store.next_scope = cache->kernel_scope_end;
        bool compiled = compile_package_artifact(ctx->rt, &store, pkg, path, false, src_hash, &entry->art, err);
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
        idm_artifact_cache_write(path, &entry->art);
    }
    idm_buf_destroy(&src);
    if (cache->pkg_count == cache->pkg_cap) {
        size_t cap = cache->pkg_cap ? cache->pkg_cap * 2u : 8u;
        CachedPackage **grown = realloc(cache->pkgs, cap * sizeof(*grown));
        if (!grown) {
            idm_artifact_destroy(&entry->art);
            free(entry->path);
            free(entry);
            idm_error_oom(err, span);
            return NULL;
        }
        cache->pkgs = grown;
        cache->pkg_cap = cap;
    }
    if (entry->art.module && !idm_bc_intern_literals(ctx->rt, entry->art.module, err)) {
        idm_artifact_destroy(&entry->art);
        free(entry->path);
        free(entry);
        idm_error_oom(err, span);
        return NULL;
    }
    cache->pkgs[cache->pkg_count++] = entry;
    if (!artifact_record_consumer_deps(ctx, path, &entry->art, err)) return NULL;
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
    if (entry->art.module && !idm_bc_intern_literals(rt, entry->art.module, err)) {
        idm_artifact_destroy(&entry->art);
        free(entry->path);
        free(entry);
        return idm_error_oom(err, idm_span_unknown(path));
    }
    if (cache->pkg_count == cache->pkg_cap) {
        size_t cap = cache->pkg_cap ? cache->pkg_cap * 2u : 8u;
        CachedPackage **grown = realloc(cache->pkgs, cap * sizeof(*grown));
        if (!grown) {
            idm_artifact_destroy(&entry->art);
            free(entry->path);
            free(entry);
            return idm_error_oom(err, idm_span_unknown(path));
        }
        cache->pkgs = grown;
        cache->pkg_cap = cap;
    }
    cache->pkgs[cache->pkg_count++] = entry;
    return true;
}

bool artifact_base(ExpandContext *ctx, const IdmArtifact *art, IdmScopeId *out_base, IdmError *err) {
    if (art->scope_base == art->scope_end) {
        *out_base = art->scope_base;
        return true;
    }
    for (size_t i = 0; i < ctx->artifact_base_count; i++) {
        if (ctx->artifact_bases[i].art == art) {
            *out_base = ctx->artifact_bases[i].base;
            return true;
        }
    }
    if (ctx->artifact_base_count == ctx->artifact_base_cap) {
        size_t cap = ctx->artifact_base_cap ? ctx->artifact_base_cap * 2u : 8u;
        void *grown = realloc(ctx->artifact_bases, cap * sizeof(*ctx->artifact_bases));
        if (!grown) return idm_error_oom(err, idm_span_unknown(NULL));
        ctx->artifact_bases = grown;
        ctx->artifact_base_cap = cap;
    }
    IdmScopeId base = ctx->scope_store.next_scope;
    ctx->scope_store.next_scope += art->scope_end - art->scope_base;
    ctx->artifact_bases[ctx->artifact_base_count].art = art;
    ctx->artifact_bases[ctx->artifact_base_count].base = base;
    ctx->artifact_bases[ctx->artifact_base_count].init_emitted[0] = false;
    ctx->artifact_bases[ctx->artifact_base_count].init_emitted[1] = false;
    ctx->artifact_base_count++;
    *out_base = base;
    return true;
}

bool artifact_init_pending(ExpandContext *ctx, const IdmArtifact *art) {
    for (size_t i = 0; i < ctx->artifact_base_count; i++) {
        if (ctx->artifact_bases[i].art == art) {
            bool pending = !ctx->artifact_bases[i].init_emitted[ctx->phase];
            ctx->artifact_bases[i].init_emitted[ctx->phase] = true;
            return pending;
        }
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
    IdmBytecodeModule *module = malloc(sizeof(*module));
    if (!module) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    idm_bc_init(module);
    uint32_t const_off = 0, fn_off = 0, code_off = 0;
    if (!idm_bc_link(module, src, &const_off, &fn_off, &code_off, err) ||
        (delta != 0 && !idm_bc_relocate_syntax_scopes(ctx->rt, module, min_id, delta, err)) ||
        !idm_bc_intern_literals(ctx->rt, module, err)) {
        idm_bc_destroy(module);
        free(module);
        return NULL;
    }
    if (out_fn_off) *out_fn_off = fn_off;
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
    dst->target_phase_ns = macro->fn.phase_ns;
    dst->target_phase_env = idm_phase_env_retain(macro->fn.phase_env);
    return true;
}

bool activate_artifact(ExpandContext *ctx, const char *activation_name, const IdmArtifact *art, IdmScopeId base, const IdmScopeSet *act_scopes, IdmSpan span, IdmError *err) {
    (void)span;
    IdmScopeId min_id = art->scope_base;
    int64_t delta = (int64_t)base - (int64_t)art->scope_base;
    char provider_key[17];
    artifact_provider_key(art->src_hash, provider_key);
    for (size_t i = 0; i < art->operator_count; i++) {
        if (!install_relocated_operator(ctx, &art->operators[i], min_id, delta, act_scopes, activation_name, provider_key, err)) return false;
    }
    for (size_t i = 0; i < art->macro_count; i++) {
        IdmModuleRef *module = relocated_module_ref(ctx, art->macros[i].module, min_id, delta, err);
        if (!module) return false;
        size_t before = ctx->macro_count;
        bool ok = install_imported_macro(ctx, art->macros[i].name, act_scopes, module, art->macros[i].function_index, art->macros[i].phase_ns, art->macros[i].phase_env, activation_name, provider_key, err);
        idm_module_ref_release(module);
        if (!ok || !install_macro_twin(ctx, art->macros[i].name, base, before, err)) return false;
    }
    if (art->resolver_module) {
        IdmModuleRef *module = relocated_module_ref(ctx, art->resolver_module, min_id, delta, err);
        if (!module) return false;
        bool ok = install_imported_resolver(ctx, act_scopes, module, art->resolver_fn, art->resolver_phase_ns ? art->resolver_phase_ns : ctx->phase_ns, art->resolver_phase_env ? art->resolver_phase_env : ctx->phase_env, activation_name, provider_key, err);
        idm_module_ref_release(module);
        if (!ok) return false;
    }
    return true;
}

static const char *idm_std_root(void) {
    const char *root = getenv("IDIOMROOT");
    return (root && *root) ? root : "std";
}

static char *trait_spelling_dup(const char *identity) {
    const char *start = strrchr(identity, '/');
    start = start ? start + 1u : identity;
    const char *suffix = strrchr(start, '#');
    return suffix ? idm_strndup(start, (size_t)(suffix - start)) : idm_strdup(start);
}

static bool compile_package_artifact(IdmRuntime *rt, IdmScopeStore *store, const IdmSyntax *pkg, const char *unit_name_hint, bool kernel_mode, const unsigned char src_hash[32], IdmArtifact *out, IdmError *err) {
    ExpandContext ctx;
    ctx_init(&ctx, rt);
    ctx.scope_store = *store;
    ctx.runner = &ctx.local_runner;
    const char *unit_name = NULL;
    if (unit_name_hint && *unit_name_hint) {
        unit_name = idm_symbol_text(idm_intern(&rt->intern, IDM_SYMBOL_ATOM, unit_name_hint));
        if (!unit_name) {
            idm_error_oom(err, pkg ? pkg->span : idm_span_unknown(NULL));
            *store = ctx.scope_store;
            ctx_destroy(&ctx);
            return false;
        }
        ctx_set_unit(&ctx, unit_name, src_hash);
    }
    ctx.phase_ns = idm_fresh_phase_namespace(rt, err);
    if (!ctx.phase_ns) {
        *store = ctx.scope_store;
        ctx_destroy(&ctx);
        return false;
    }
    ctx.phase_env = idm_phase_env_create(rt, ctx.phase_ns);
    if (!ctx.phase_env) {
        idm_error_oom(err, idm_span_unknown(NULL));
        *store = ctx.scope_store;
        ctx_destroy(&ctx);
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
        *store = ctx.scope_store;
        ctx_destroy(&ctx);
        return false;
    }
    char *name = ctx.package_name ? idm_strdup(ctx.package_name) : NULL;
    size_t export_count = ctx.export_count;
    IdmPkgExport *exports = NULL;
    bool copy_ok = true;
    if (export_count != 0) {
        exports = malloc(export_count * sizeof(*exports));
        if (!exports) copy_ok = false;
        else {
            for (size_t i = 0; i < export_count; i++) {
                exports[i].name = idm_strdup(ctx.exports[i].name);
                exports[i].slot = ctx.exports[i].global_id;
                exports[i].arity = ctx.exports[i].arity;
                if (!exports[i].name) { for (size_t j = 0; j < i; j++) free(exports[j].name); free(exports); exports = NULL; copy_ok = false; break; }
            }
        }
    }
    size_t global_count = ctx.package_global_count;
    IdmPkgGlobal *globals = NULL;
    if (copy_ok && global_count != 0) {
        globals = calloc(global_count, sizeof(*globals));
        if (!globals) copy_ok = false;
        else {
            for (size_t i = 0; i < global_count; i++) {
                globals[i].name = idm_strdup(ctx.package_globals[i].name);
                globals[i].slot = ctx.package_globals[i].slot;
                globals[i].arity = ctx.package_globals[i].arity;
                if (!globals[i].name || !idm_scope_set_copy(&globals[i].scopes, &ctx.package_globals[i].scopes)) {
                    for (size_t j = 0; j <= i; j++) idm_pkg_global_destroy(&globals[j]);
                    free(globals);
                    globals = NULL;
                    copy_ok = false;
                    break;
                }
            }
        }
    }
    size_t type_count = 0;
    IdmPkgType *types = NULL;
    if (copy_ok) {
        for (size_t i = 0; i < ctx.type_count; i++) if (ctx.types[i].exported) type_count++;
    }
    if (copy_ok && type_count != 0) {
        types = calloc(type_count, sizeof(*types));
        if (!types) copy_ok = false;
        size_t k = 0;
        for (size_t i = 0; copy_ok && i < ctx.type_count; i++) {
            const TypeDef *t = &ctx.types[i];
            if (!t->exported) continue;
            types[k].name = idm_strdup(t->name);
            types[k].identity = idm_strdup(t->identity);
            if (!types[k].name || !types[k].identity || !idm_scope_set_copy(&types[k].scopes, &t->scopes)) copy_ok = false;
            if (copy_ok && t->field_count != 0) {
                types[k].fields = calloc(t->field_count, sizeof(*types[k].fields));
                if (!types[k].fields) copy_ok = false;
                else {
                    types[k].field_count = t->field_count;
                    for (size_t f = 0; f < t->field_count; f++) {
                        types[k].fields[f] = idm_strdup(t->fields[f]);
                        if (!types[k].fields[f]) { copy_ok = false; break; }
                    }
                }
            }
            k++;
        }
        if (!copy_ok && types) {
            for (size_t j = 0; j < type_count; j++) idm_pkg_type_destroy(&types[j]);
            free(types);
            types = NULL;
        }
        if (!copy_ok) type_count = 0;
    }
    size_t pkg_trait_count = 0;
    IdmPkgTrait *pkg_traits = NULL;
    if (copy_ok) {
        for (size_t i = 0; i < ctx.trait_count; i++) if (ctx.traits[i].exported) pkg_trait_count++;
    }
    if (copy_ok && pkg_trait_count != 0) {
        pkg_traits = calloc(pkg_trait_count, sizeof(*pkg_traits));
        if (!pkg_traits) copy_ok = false;
        size_t k = 0;
        for (size_t i = 0; copy_ok && i < ctx.trait_count; i++) {
            const TraitDef *t = &ctx.traits[i];
            if (!t->exported) continue;
            pkg_traits[k].name = trait_spelling_dup(t->name);
            pkg_traits[k].identity = idm_strdup(t->name);
            if (!pkg_traits[k].name || !pkg_traits[k].identity ||
                !idm_trait_requirement_defs_copy(t->requirements, t->requirement_count, &pkg_traits[k].requirements) ||
                !idm_trait_method_defs_copy(t->methods, t->method_count, &pkg_traits[k].methods)) {
                copy_ok = false;
            } else {
                pkg_traits[k].requirement_count = t->requirement_count;
                pkg_traits[k].method_count = t->method_count;
            }
            k++;
        }
        if (!copy_ok && pkg_traits) {
            for (size_t j = 0; j < pkg_trait_count; j++) idm_pkg_trait_destroy(&pkg_traits[j]);
            free(pkg_traits);
            pkg_traits = NULL;
        }
        if (!copy_ok) pkg_trait_count = 0;
    }
    size_t macro_count = 0;
    size_t op_count = 0;
    if (copy_ok) {
        for (size_t i = macro_base; i < ctx.macro_count; i++) if (ctx.macros[i].exported) macro_count++;
        for (size_t i = op_base; i < ctx.operator_count; i++) if (ctx.operators[i].exported) op_count++;
    }
    IdmPkgMacro *macros = NULL;
    IdmOperatorDef *operators = NULL;
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
                macros[k].phase_ns = ctx.macros[i].fn.phase_ns;
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
    IdmModuleRef *res_module = NULL;
    uint32_t res_fn = 0;
    IdmNamespace *res_phase_ns = NULL;
    IdmPhaseEnv *res_phase_env = NULL;
    if (copy_ok && ctx.decl_resolver) {
        res_module = idm_module_ref_retain(ctx.decl_resolver_impl.module);
        res_fn = ctx.decl_resolver_impl.function_index;
        res_phase_ns = ctx.decl_resolver_impl.phase_ns;
        res_phase_env = idm_phase_env_retain(ctx.decl_resolver_impl.phase_env);
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
            if (!idm_core_compile_main(body, module, &init_fn, err)) { idm_bc_destroy(module); free(module); module = NULL; }
        }
    }
    idm_core_free(body);
    hooks_restore(rt, &saved);
    *store = ctx.scope_store;
    ctx_destroy(&ctx);
    if (!copy_ok || !module) {
        free(name);
        if (exports) { for (size_t i = 0; i < export_count; i++) free(exports[i].name); free(exports); }
        if (globals) { for (size_t i = 0; i < global_count; i++) idm_pkg_global_destroy(&globals[i]); free(globals); }
        if (macros) { for (size_t i = 0; i < macro_count; i++) idm_pkg_macro_destroy(&macros[i]); free(macros); }
        if (operators) { for (size_t i = 0; i < op_count; i++) idm_operator_def_destroy(&operators[i]); free(operators); }
        if (types) { for (size_t i = 0; i < type_count; i++) idm_pkg_type_destroy(&types[i]); free(types); }
        if (pkg_traits) { for (size_t i = 0; i < pkg_trait_count; i++) idm_pkg_trait_destroy(&pkg_traits[i]); free(pkg_traits); }
        idm_module_ref_release(res_module);
        idm_phase_env_release(res_phase_env);
        idm_phase_env_release(pkg_phase_env);
        for (size_t i = 0; i < pkg_dep_count; i++) free(pkg_deps[i].path);
        free(pkg_deps);
        if (err && !err->present) idm_error_oom(err, idm_span_unknown(NULL));
        return false;
    }
    out->module = module;
    out->init_fn = init_fn;
    out->scope_base = package_scope;
    out->scope_end = package_scope;
    out->name = name;
    out->exports = exports;
    out->export_count = export_count;
    out->globals = globals;
    out->global_count = global_count;
    out->macros = macros;
    out->macro_count = macro_count;
    out->operators = operators;
    out->operator_count = op_count;
    out->types = types;
    out->type_count = type_count;
    out->traits = pkg_traits;
    out->trait_count = pkg_trait_count;
    out->resolver_module = res_module;
    out->resolver_fn = res_fn;
    out->resolver_phase_ns = res_phase_ns;
    out->resolver_phase_env = res_phase_env;
    out->phase_env = pkg_phase_env;
    out->deps = pkg_deps;
    out->dep_count = pkg_dep_count;
    return true;
}

bool compile_package_module(ExpandContext *parent, const IdmSyntax *pkg, const char *unit_name_hint, const unsigned char src_hash[32], IdmArtifact *out, IdmError *err) {
    return compile_package_artifact(parent->rt, &parent->scope_store, pkg, unit_name_hint, false, src_hash, out, err);
}
