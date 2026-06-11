#include "internal.h"

static void expand_cache_destroy(void *p);
static ExpandCache *expand_cache_get(IshRuntime *rt);
static ExpandCache *kernel_artifact_get(IshRuntime *rt, IshError *err);
static bool ctx_record_dep(ExpandContext *ctx, const char *path, const unsigned char hash[32], IshError *err);
static bool artifact_record_consumer_deps(ExpandContext *ctx, const char *path, const IshArtifact *art, IshError *err);
static IshModuleRef *relocated_module_ref(ExpandContext *ctx, IshModuleRef *src, IshScopeId min_id, int64_t delta, IshError *err);
static bool install_relocated_operator(ExpandContext *ctx, const IshOperatorDef *op, IshScopeId min_id, int64_t delta, IshError *err);
static const char *ish_std_root(void);
static bool compile_package_artifact(IshRuntime *rt, IshScopeStore *store, const IshSyntax *pkg, const char *protocol_name_hint, bool kernel_mode, IshArtifact *out, IshError *err);

IshCore *expand_use(ExpandContext *ctx, const char *path, const char *qualifier, IshSyntax *const *items, size_t cont_index, size_t cont_count, IshSpan span, IshError *err) {
    const IshArtifact *art = artifact_get(ctx, path, span, err);
    if (!art) {
        if (err && err->present && span.line != 0) {
            ish_error_note(err, "while loading package '%s' (%s:%u:%u)", path, span.file ? span.file : "<unknown>", span.line, span.column);
        } else if (err && err->present) {
            ish_error_note(err, "while loading package '%s'", path);
        }
        return NULL;
    }
    IshScopeId base = 0;
    if (!artifact_base(ctx, art, &base, err)) return NULL;
    IshScopeId min_id = art->scope_base;
    int64_t delta = (int64_t)base - (int64_t)art->scope_base;

    size_t import_count = art->export_count + art->global_count;
    uint32_t *export_src = NULL;
    uint32_t *export_dst = NULL;
    if (import_count != 0) {
        export_src = malloc(import_count * sizeof(*export_src));
        export_dst = malloc(import_count * sizeof(*export_dst));
        if (!export_src || !export_dst) { free(export_src); free(export_dst); return (IshCore *)(uintptr_t)ish_error_oom(err, span); }
    }
    bool ok = true;
    size_t import_index = 0;
    for (size_t i = 0; i < art->export_count && ok; i++) {
        uint32_t consumer_slot = ctx->global_seq++;
        export_src[import_index] = art->exports[i].slot;
        export_dst[import_index] = consumer_slot;
        import_index++;
        const char *bind_name = art->exports[i].name;
        char *qualified = NULL;
        if (qualifier) {
            IshBuffer qb;
            ish_buf_init(&qb);
            if (!ish_buf_append(&qb, qualifier) || !ish_buf_append_char(&qb, '.') || !ish_buf_append(&qb, art->exports[i].name)) { ish_buf_destroy(&qb); ok = false; break; }
            qualified = ish_buf_take(&qb);
            bind_name = qualified;
        }
        ok = ish_binding_table_add(&ctx->bindings, bind_name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_GLOBAL, &ctx->empty_scopes, consumer_slot, ISH_FRAME_GLOBAL, NULL);
        free(qualified);
    }
    for (size_t i = 0; i < art->global_count && ok; i++) {
        uint32_t consumer_slot = ctx->global_seq++;
        export_src[import_index] = art->globals[i].slot;
        export_dst[import_index] = consumer_slot;
        import_index++;
        IshScopeSet gscopes;
        ish_scope_set_init(&gscopes);
        if (!ish_scope_set_copy(&gscopes, &art->globals[i].scopes)) { ok = false; break; }
        ish_scope_set_relocate(&gscopes, min_id, delta);
        ok = ish_binding_table_add(&ctx->bindings, art->globals[i].name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_GLOBAL, &gscopes, consumer_slot, ISH_FRAME_GLOBAL, NULL);
        ish_scope_set_destroy(&gscopes);
    }
    for (size_t i = 0; i < art->macro_count && ok; i++) {
        char *qualified = NULL;
        const char *bind_name = art->macros[i].name;
        if (qualifier) {
            IshBuffer qb;
            ish_buf_init(&qb);
            if (!ish_buf_append(&qb, qualifier) || !ish_buf_append_char(&qb, '.') || !ish_buf_append(&qb, art->macros[i].name)) {
                ish_buf_destroy(&qb);
                ish_error_oom(err, span);
                ok = false;
                break;
            }
            qualified = ish_buf_take(&qb);
            bind_name = qualified;
        }
        IshModuleRef *module = relocated_module_ref(ctx, art->macros[i].module, min_id, delta, err);
        if (!module) {
            free(qualified);
            ok = false;
            break;
        }
        ok = install_imported_macro(ctx, bind_name, &ctx->empty_scopes, module, art->macros[i].function_index, art->macros[i].phase_ns, art->macros[i].phase_env, err);
        ish_module_ref_release(module);
        free(qualified);
    }
    if (!qualifier) {
        for (size_t i = 0; i < art->operator_count && ok; i++) {
            ok = install_relocated_operator(ctx, &art->operators[i], min_id, delta, err);
        }
        for (size_t i = 0; i < art->method_count && ok; i++) {
            ok = install_method_surface(ctx, path, art->methods[i].name, art->methods[i].arity, &ctx->empty_scopes, err);
        }
    }
    if (!ok) {
        free(export_src);
        free(export_dst);
        if (err && !err->present) ish_error_oom(err, span);
        return NULL;
    }

    IshValue name_value = ish_atom(ctx->rt, art->name ? art->name : path);
    IshCore *cont = expand_body_items(ctx, items, cont_index, cont_count, err);
    if (!cont) { free(export_src); free(export_dst); return NULL; }

    uint32_t fn_off = 0;
    IshBytecodeModule *module = relocated_module_copy(ctx, art->module, min_id, delta, &fn_off, err);
    if (!module) {
        free(export_src);
        free(export_dst);
        ish_core_free(cont);
        return NULL;
    }
    IshCore *use_core = ish_core_use_package(name_value, module, art->init_fn + fn_off, export_src, export_dst, import_count, cont, span);
    if (!use_core) {
        free(export_src);
        free(export_dst);
        ish_bc_destroy(module);
        free(module);
        ish_core_free(cont);
        return (IshCore *)(uintptr_t)ish_error_oom(err, span);
    }
    return use_core;
}

static void expand_cache_destroy(void *p) {
    ExpandCache *cache = p;
    if (!cache) return;
    if (cache->kernel_ready) {
        if (cache->kernel.module) ish_runtime_unregister_gc_module(cache->rt, cache->kernel.module);
        ish_artifact_destroy(&cache->kernel);
    }
    for (size_t i = 0; i < cache->pkg_count; i++) {
        if (cache->pkgs[i]->art.module) ish_runtime_unregister_gc_module(cache->rt, cache->pkgs[i]->art.module);
        ish_artifact_destroy(&cache->pkgs[i]->art);
        free(cache->pkgs[i]->path);
        free(cache->pkgs[i]);
    }
    free(cache->pkgs);
    free(cache->kernel_path);
    free(cache->compiling);
    free(cache);
}

static ExpandCache *expand_cache_get(IshRuntime *rt) {
    if (!rt->expand_cache) {
        ExpandCache *cache = calloc(1u, sizeof(*cache));
        if (!cache) return NULL;
        cache->rt = rt;
        rt->expand_cache = cache;
        rt->expand_cache_free = expand_cache_destroy;
    }
    return rt->expand_cache;
}

static ExpandCache *kernel_artifact_get(IshRuntime *rt, IshError *err) {
    ExpandCache *cache = expand_cache_get(rt);
    if (!cache) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return NULL;
    }
    if (cache->kernel_ready) return cache;
    IshBuffer path;
    ish_buf_init(&path);
    if (!ish_buf_append(&path, ish_std_root()) || !ish_buf_append(&path, "/kernel")) {
        ish_buf_destroy(&path);
        ish_error_oom(err, ish_span_unknown(NULL));
        return NULL;
    }
    IshBuffer src;
    ish_buf_init(&src);
    const char *label = NULL;
    if (!ish_package_read_source(rt, path.data, &src, &label, ish_span_unknown(NULL), err)) {
        ish_buf_destroy(&src);
        ish_buf_destroy(&path);
        return NULL;
    }
    unsigned char src_hash[32];
    ish_sha256(src.data ? src.data : "", src.len, src_hash);
    cache->kernel_path = ish_buf_take(&path);
    if (!cache->kernel_path) {
        ish_buf_destroy(&src);
        ish_error_oom(err, ish_span_unknown(NULL));
        return NULL;
    }
    if (ish_artifact_cache_load(rt, cache->kernel_path, src_hash, &cache->kernel)) {
        ish_buf_destroy(&src);
        cache->kernel_scope_end = cache->kernel.scope_end;
        if (cache->kernel.module && !ish_runtime_register_gc_module(rt, cache->kernel.module)) {
            ish_artifact_destroy(&cache->kernel);
            ish_error_oom(err, ish_span_unknown(NULL));
            return NULL;
        }
        cache->kernel_ready = true;
        return cache;
    }
    IshSyntax *pkg = NULL;
    bool read_ok = ish_reader_read_string(label, src.data ? src.data : "", &pkg, err);
    ish_buf_destroy(&src);
    if (!read_ok) return NULL;
    if (!syn_is_protocol(pkg, "%-package-begin")) {
        ish_syn_free(pkg);
        ish_error_set(err, ish_span_unknown(NULL), "kernel source must be a program");
        return NULL;
    }
    IshScopeStore store;
    ish_scope_store_init(&store);
    IshScopeId base = store.next_scope;
    bool compiled = compile_package_artifact(rt, &store, pkg, "std/kernel", true, &cache->kernel, err);
    ish_syn_free(pkg);
    if (!compiled) return NULL;
    cache->kernel.scope_base = base;
    cache->kernel.scope_end = store.next_scope;
    memcpy(cache->kernel.src_hash, src_hash, 32u);
    cache->kernel_scope_end = store.next_scope;
    if (cache->kernel.module && !ish_runtime_register_gc_module(rt, cache->kernel.module)) {
        ish_artifact_destroy(&cache->kernel);
        ish_error_oom(err, ish_span_unknown(NULL));
        return NULL;
    }
    ish_artifact_cache_write(cache->kernel_path, &cache->kernel);
    cache->kernel_ready = true;
    return cache;
}

bool ctx_activate_kernel(ExpandContext *ctx, IshError *err) {
    ExpandCache *cache = kernel_artifact_get(ctx->rt, err);
    if (!cache) return false;
    IshArtifact *k = &cache->kernel;
    if (!ctx_record_dep(ctx, cache->kernel_path, k->src_hash, err)) return false;
    if (!record_activation(ctx, "std/kernel", ish_span_unknown(NULL), err)) return false;
    if (ctx->scope_store.next_scope < cache->kernel_scope_end) ctx->scope_store.next_scope = cache->kernel_scope_end;
    for (size_t i = 0; i < k->operator_count; i++) {
        if (!install_imported_operator(ctx, &k->operators[i], err)) return false;
    }
    for (size_t i = 0; i < k->macro_count; i++) {
        if (!install_imported_macro(ctx, k->macros[i].name, &ctx->empty_scopes, k->macros[i].module, k->macros[i].function_index, k->macros[i].phase_ns, k->macros[i].phase_env, err)) return false;
    }
    const char *kernel_name = k->name ? k->name : "std/kernel";
    for (size_t i = 0; i < k->method_count; i++) {
        if (!install_method_surface(ctx, kernel_name, k->methods[i].name, k->methods[i].arity, &ctx->empty_scopes, err)) return false;
    }
    if (k->resolver_module) {
        if (!install_imported_macro(ctx, "%resolver", &ctx->empty_scopes, k->resolver_module, k->resolver_fn, k->resolver_phase_ns, k->resolver_phase_env, err)) return false;
    }
    size_t import_count = k->export_count + k->global_count;
    if (import_count != 0) {
        ctx->kernel_import_src = malloc(import_count * sizeof(*ctx->kernel_import_src));
        ctx->kernel_import_dst = malloc(import_count * sizeof(*ctx->kernel_import_dst));
        if (!ctx->kernel_import_src || !ctx->kernel_import_dst) return ish_error_oom(err, ish_span_unknown(NULL));
        size_t n = 0;
        for (size_t i = 0; i < k->export_count; i++) {
            uint32_t consumer_slot = ctx->global_seq++;
            ctx->kernel_import_src[n] = k->exports[i].slot;
            ctx->kernel_import_dst[n] = consumer_slot;
            n++;
            if (!ish_binding_table_add(&ctx->bindings, k->exports[i].name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_GLOBAL, &ctx->empty_scopes, consumer_slot, ISH_FRAME_GLOBAL, NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
        }
        for (size_t i = 0; i < k->global_count; i++) {
            uint32_t consumer_slot = ctx->global_seq++;
            ctx->kernel_import_src[n] = k->globals[i].slot;
            ctx->kernel_import_dst[n] = consumer_slot;
            n++;
            if (!ish_binding_table_add(&ctx->bindings, k->globals[i].name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_GLOBAL, &k->globals[i].scopes, consumer_slot, ISH_FRAME_GLOBAL, NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
        }
        ctx->kernel_import_count = import_count;
    }
    ctx->kernel_wrap = import_count != 0 || k->method_count != 0;
    return true;
}

IshCore *wrap_kernel_use(ExpandContext *ctx, IshCore *body, IshError *err) {
    if (!ctx->kernel_wrap || !body) return body;
    ExpandCache *cache = expand_cache_get(ctx->rt);
    IshArtifact *k = &cache->kernel;
    IshBytecodeModule *module = malloc(sizeof(*module));
    if (!module) {
        ish_core_free(body);
        return (IshCore *)(uintptr_t)ish_error_oom(err, ish_span_unknown(NULL));
    }
    ish_bc_init(module);
    uint32_t const_off = 0, fn_off = 0, code_off = 0;
    if (!ish_bc_link(module, k->module, &const_off, &fn_off, &code_off, err)) {
        ish_bc_destroy(module);
        free(module);
        ish_core_free(body);
        return NULL;
    }
    size_t n = ctx->kernel_import_count;
    uint32_t *src = NULL;
    uint32_t *dst = NULL;
    if (n != 0) {
        src = malloc(n * sizeof(*src));
        dst = malloc(n * sizeof(*dst));
        if (!src || !dst) {
            free(src);
            free(dst);
            ish_bc_destroy(module);
            free(module);
            ish_core_free(body);
            return (IshCore *)(uintptr_t)ish_error_oom(err, ish_span_unknown(NULL));
        }
        memcpy(src, ctx->kernel_import_src, n * sizeof(*src));
        memcpy(dst, ctx->kernel_import_dst, n * sizeof(*dst));
    }
    IshValue name_value = ish_atom(ctx->rt, k->name ? k->name : "std/kernel");
    IshCore *wrapped = ish_core_use_package(name_value, module, k->init_fn + fn_off, src, dst, n, body, ish_span_unknown(NULL));
    if (!wrapped) {
        free(src);
        free(dst);
        ish_bc_destroy(module);
        free(module);
        ish_core_free(body);
        return (IshCore *)(uintptr_t)ish_error_oom(err, ish_span_unknown(NULL));
    }
    return wrapped;
}

static bool ctx_record_dep(ExpandContext *ctx, const char *path, const unsigned char hash[32], IshError *err) {
    for (size_t i = 0; i < ctx->dep_count; i++) {
        if (strcmp(ctx->deps[i].path, path) == 0) return true;
    }
    if (ctx->dep_count == ctx->dep_cap) {
        size_t cap = ctx->dep_cap ? ctx->dep_cap * 2u : 8u;
        IshArtifactDep *grown = realloc(ctx->deps, cap * sizeof(*grown));
        if (!grown) return ish_error_oom(err, ish_span_unknown(NULL));
        ctx->deps = grown;
        ctx->dep_cap = cap;
    }
    ctx->deps[ctx->dep_count].path = ish_strdup(path);
    if (!ctx->deps[ctx->dep_count].path) return ish_error_oom(err, ish_span_unknown(NULL));
    memcpy(ctx->deps[ctx->dep_count].hash, hash, 32u);
    ctx->dep_count++;
    return true;
}

static bool artifact_record_consumer_deps(ExpandContext *ctx, const char *path, const IshArtifact *art, IshError *err) {
    if (!ctx_record_dep(ctx, path, art->src_hash, err)) return false;
    for (size_t i = 0; i < art->dep_count; i++) {
        if (!ctx_record_dep(ctx, art->deps[i].path, art->deps[i].hash, err)) return false;
    }
    return true;
}

const IshArtifact *artifact_get(ExpandContext *ctx, const char *path, IshSpan span, IshError *err) {
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
            ish_error_set(err, span, "package dependency cycle involving '%s'", path);
            return NULL;
        }
    }
    CachedPackage *entry = calloc(1u, sizeof(*entry));
    if (!entry) {
        ish_error_oom(err, span);
        return NULL;
    }
    entry->path = ish_strdup(path);
    if (!entry->path) {
        free(entry);
        ish_error_oom(err, span);
        return NULL;
    }
    IshBuffer src;
    ish_buf_init(&src);
    const char *label = NULL;
    if (!ish_package_read_source(ctx->rt, path, &src, &label, span, err)) {
        ish_buf_destroy(&src);
        free(entry->path);
        free(entry);
        return NULL;
    }
    unsigned char src_hash[32];
    ish_sha256(src.data ? src.data : "", src.len, src_hash);
    bool loaded = ish_artifact_cache_load(ctx->rt, path, src_hash, &entry->art);
    if (loaded) {
        bool kernel_matches = false;
        for (size_t i = 0; i < entry->art.dep_count; i++) {
            if (strcmp(entry->art.deps[i].path, cache->kernel_path) == 0 && memcmp(entry->art.deps[i].hash, cache->kernel.src_hash, 32u) == 0) {
                kernel_matches = true;
                break;
            }
        }
        if (!kernel_matches) {
            ish_artifact_destroy(&entry->art);
            loaded = false;
        }
    }
    if (!loaded) {
        IshSyntax *pkg = NULL;
        bool read_ok = ish_reader_read_string(label, src.data ? src.data : "", &pkg, err);
        if (read_ok && !syn_is_protocol(pkg, "%-package-begin")) {
            ish_syn_free(pkg);
            ish_error_set(err, span, "package source must be a program");
            read_ok = false;
        }
        if (!read_ok) {
            ish_buf_destroy(&src);
            free(entry->path);
            free(entry);
            return NULL;
        }
        if (cache->compiling_count == cache->compiling_cap) {
            size_t cap = cache->compiling_cap ? cache->compiling_cap * 2u : 8u;
            const char **grown = realloc(cache->compiling, cap * sizeof(*grown));
            if (!grown) {
                ish_syn_free(pkg);
                ish_buf_destroy(&src);
                free(entry->path);
                free(entry);
                ish_error_oom(err, span);
                return NULL;
            }
            cache->compiling = grown;
            cache->compiling_cap = cap;
        }
        cache->compiling[cache->compiling_count++] = entry->path;
        IshScopeStore store;
        store.next_scope = cache->kernel_scope_end;
        bool compiled = compile_package_artifact(ctx->rt, &store, pkg, path, false, &entry->art, err);
        cache->compiling_count--;
        ish_syn_free(pkg);
        if (!compiled) {
            ish_buf_destroy(&src);
            free(entry->path);
            free(entry);
            return NULL;
        }
        entry->art.scope_base = cache->kernel_scope_end;
        entry->art.scope_end = store.next_scope;
        memcpy(entry->art.src_hash, src_hash, 32u);
        ish_artifact_cache_write(path, &entry->art);
    }
    ish_buf_destroy(&src);
    if (cache->pkg_count == cache->pkg_cap) {
        size_t cap = cache->pkg_cap ? cache->pkg_cap * 2u : 8u;
        CachedPackage **grown = realloc(cache->pkgs, cap * sizeof(*grown));
        if (!grown) {
            ish_artifact_destroy(&entry->art);
            free(entry->path);
            free(entry);
            ish_error_oom(err, span);
            return NULL;
        }
        cache->pkgs = grown;
        cache->pkg_cap = cap;
    }
    if (entry->art.module && !ish_runtime_register_gc_module(ctx->rt, entry->art.module)) {
        ish_artifact_destroy(&entry->art);
        free(entry->path);
        free(entry);
        ish_error_oom(err, span);
        return NULL;
    }
    cache->pkgs[cache->pkg_count++] = entry;
    if (!artifact_record_consumer_deps(ctx, path, &entry->art, err)) return NULL;
    return &entry->art;
}

bool artifact_base(ExpandContext *ctx, const IshArtifact *art, IshScopeId *out_base, IshError *err) {
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
        if (!grown) return ish_error_oom(err, ish_span_unknown(NULL));
        ctx->artifact_bases = grown;
        ctx->artifact_base_cap = cap;
    }
    IshScopeId base = ctx->scope_store.next_scope;
    ctx->scope_store.next_scope += art->scope_end - art->scope_base;
    ctx->artifact_bases[ctx->artifact_base_count].art = art;
    ctx->artifact_bases[ctx->artifact_base_count].base = base;
    ctx->artifact_base_count++;
    *out_base = base;
    return true;
}

static IshModuleRef *relocated_module_ref(ExpandContext *ctx, IshModuleRef *src, IshScopeId min_id, int64_t delta, IshError *err) {
    if (delta == 0) return ish_module_ref_retain(src);
    IshModuleRef *clone = ish_module_ref_create(ctx->rt);
    if (!clone) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return NULL;
    }
    uint32_t const_off = 0, fn_off = 0, code_off = 0;
    if (!ish_bc_link(&clone->module, &src->module, &const_off, &fn_off, &code_off, err) ||
        !ish_bc_relocate_syntax_scopes(ctx->rt, &clone->module, min_id, delta, err)) {
        ish_module_ref_release(clone);
        return NULL;
    }
    return clone;
}

IshBytecodeModule *relocated_module_copy(ExpandContext *ctx, const IshBytecodeModule *src, IshScopeId min_id, int64_t delta, uint32_t *out_fn_off, IshError *err) {
    IshBytecodeModule *module = malloc(sizeof(*module));
    if (!module) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return NULL;
    }
    ish_bc_init(module);
    uint32_t const_off = 0, fn_off = 0, code_off = 0;
    if (!ish_bc_link(module, src, &const_off, &fn_off, &code_off, err) ||
        (delta != 0 && !ish_bc_relocate_syntax_scopes(ctx->rt, module, min_id, delta, err))) {
        ish_bc_destroy(module);
        free(module);
        return NULL;
    }
    if (out_fn_off) *out_fn_off = fn_off;
    return module;
}

static bool install_relocated_operator(ExpandContext *ctx, const IshOperatorDef *op, IshScopeId min_id, int64_t delta, IshError *err) {
    if (delta == 0) return install_imported_operator(ctx, op, err);
    IshOperatorDef local = *op;
    ish_scope_set_init(&local.scopes);
    if (!ish_scope_set_copy(&local.scopes, &op->scopes)) return ish_error_oom(err, ish_span_unknown(NULL));
    ish_scope_set_relocate(&local.scopes, min_id, delta);
    bool ok = install_imported_operator(ctx, &local, err);
    ish_scope_set_destroy(&local.scopes);
    return ok;
}

bool activate_artifact(ExpandContext *ctx, const char *protocol_name, const IshArtifact *art, IshScopeId base, IshSpan span, IshError *err) {
    (void)span;
    IshScopeId min_id = art->scope_base;
    int64_t delta = (int64_t)base - (int64_t)art->scope_base;
    for (size_t i = 0; i < art->operator_count; i++) {
        if (!install_relocated_operator(ctx, &art->operators[i], min_id, delta, err)) return false;
    }
    for (size_t i = 0; i < art->macro_count; i++) {
        IshModuleRef *module = relocated_module_ref(ctx, art->macros[i].module, min_id, delta, err);
        if (!module) return false;
        bool ok = install_imported_macro(ctx, art->macros[i].name, &ctx->empty_scopes, module, art->macros[i].function_index, art->macros[i].phase_ns, art->macros[i].phase_env, err);
        ish_module_ref_release(module);
        if (!ok) return false;
    }
    for (size_t i = 0; i < art->method_count; i++) {
        if (!install_method_surface(ctx, protocol_name, art->methods[i].name, art->methods[i].arity, &ctx->empty_scopes, err)) return false;
    }
    if (art->resolver_module) {
        IshModuleRef *module = relocated_module_ref(ctx, art->resolver_module, min_id, delta, err);
        if (!module) return false;
        bool ok = install_imported_macro(ctx, "%resolver", &ctx->empty_scopes, module, art->resolver_fn, art->resolver_phase_ns, art->resolver_phase_env, err);
        ish_module_ref_release(module);
        if (!ok) return false;
    }
    return true;
}

static const char *ish_std_root(void) {
    const char *root = getenv("ISH_STD");
    return (root && *root) ? root : "std";
}

static bool compile_package_artifact(IshRuntime *rt, IshScopeStore *store, const IshSyntax *pkg, const char *protocol_name_hint, bool kernel_mode, IshArtifact *out, IshError *err) {
    ExpandContext ctx;
    ctx_init(&ctx, rt);
    ctx.scope_store = *store;
    ctx.runner = &ctx.local_runner;
    const char *protocol_name = NULL;
    if (protocol_name_hint && *protocol_name_hint) {
        protocol_name = ish_symbol_text(ish_intern(&rt->intern, ISH_SYMBOL_ATOM, protocol_name_hint));
        if (!protocol_name) {
            ish_error_oom(err, pkg ? pkg->span : ish_span_unknown(NULL));
            *store = ctx.scope_store;
            ctx_destroy(&ctx);
            return false;
        }
        ctx.protocol_name = protocol_name;
    }
    ctx.phase_ns = ish_fresh_phase_namespace(rt, err);
    if (!ctx.phase_ns) {
        *store = ctx.scope_store;
        ctx_destroy(&ctx);
        return false;
    }
    ctx.phase_env = ish_phase_env_create(rt, ctx.phase_ns);
    if (!ctx.phase_env) {
        ish_error_oom(err, ish_span_unknown(NULL));
        *store = ctx.scope_store;
        ctx_destroy(&ctx);
        return false;
    }
    void *old_user = rt->local_expand_user;
    IshLocalExpandFn old_fn = rt->local_expand;
    void *old_free_identifier_eq_user = rt->free_identifier_eq_user;
    IshFreeIdentifierEqFn old_free_identifier_eq = rt->free_identifier_eq;
    rt->local_expand_user = &ctx;
    rt->local_expand = local_expand_callback;
    rt->free_identifier_eq_user = &ctx;
    rt->free_identifier_eq = free_identifier_eq_callback;
    void *old_register_operator_user = rt->register_operator_user;
    IshRegisterOperatorFn old_register_operator = rt->register_operator;
    rt->register_operator_user = &ctx;
    rt->register_operator = register_operator_callback;
    void *old_register_macro_user = rt->register_macro_user;
    IshRegisterMacroFn old_register_macro = rt->register_macro;
    rt->register_macro_user = &ctx;
    rt->register_macro = register_macro_callback;
    void *old_expander_surface_user = rt->expander_surface_user;
    IshExpanderSurfaceFn old_expander_surface = rt->expander_surface;
    rt->expander_surface_user = &ctx;
    rt->expander_surface = expander_surface_callback;
    bool ok = ctx_seed(&ctx, err) && (kernel_mode || ctx_activate_kernel(&ctx, err));
    size_t macro_base = ctx.macro_count;
    size_t op_base = ctx.operator_count;
    if (ok) ctx.in_package = true;
    IshSyntax *scoped_pkg = NULL;
    if (ok) {
        scoped_pkg = ish_syn_clone(pkg);
        if (!scoped_pkg) {
            ish_error_oom(err, pkg ? pkg->span : ish_span_unknown(NULL));
            ok = false;
        } else {
            IshScopeId package_scope = ish_scope_fresh(&ctx.scope_store);
            if (!ish_syn_scope_add_tree(scoped_pkg, 0, package_scope)) {
                ish_error_oom(err, scoped_pkg->span);
                ok = false;
            }
        }
    }
    if (ok && ctx.protocol_name && !predeclare_protocol_methods(&ctx, scoped_pkg->as.seq.items, 1, scoped_pkg->as.seq.count, err)) ok = false;
    IshCore *body = ok ? expand_body_items(&ctx, scoped_pkg->as.seq.items, 1, scoped_pkg->as.seq.count, err) : NULL;
    ish_syn_free(scoped_pkg);
    if (!body) {
        rt->local_expand_user = old_user;
        rt->local_expand = old_fn;
        rt->free_identifier_eq_user = old_free_identifier_eq_user;
        rt->free_identifier_eq = old_free_identifier_eq;
    rt->register_operator_user = old_register_operator_user;
    rt->register_operator = old_register_operator;
    rt->register_macro_user = old_register_macro_user;
    rt->register_macro = old_register_macro;
    rt->expander_surface_user = old_expander_surface_user;
    rt->expander_surface = old_expander_surface;
        rt->register_operator_user = old_register_operator_user;
        rt->register_operator = old_register_operator;
        rt->register_macro_user = old_register_macro_user;
        rt->register_macro = old_register_macro;
        rt->expander_surface_user = old_expander_surface_user;
        rt->expander_surface = old_expander_surface;
        *store = ctx.scope_store;
        ctx_destroy(&ctx);
        return false;
    }
    char *name = ctx.package_name ? ish_strdup(ctx.package_name) : NULL;
    size_t export_count = ctx.export_count;
    IshPkgExport *exports = NULL;
    bool copy_ok = true;
    if (export_count != 0) {
        exports = malloc(export_count * sizeof(*exports));
        if (!exports) copy_ok = false;
        else {
            for (size_t i = 0; i < export_count; i++) {
                exports[i].name = ish_strdup(ctx.exports[i].name);
                exports[i].slot = ctx.exports[i].global_id;
                if (!exports[i].name) { for (size_t j = 0; j < i; j++) free(exports[j].name); free(exports); exports = NULL; copy_ok = false; break; }
            }
        }
    }
    size_t global_count = ctx.package_global_count;
    IshPkgGlobal *globals = NULL;
    if (copy_ok && global_count != 0) {
        globals = calloc(global_count, sizeof(*globals));
        if (!globals) copy_ok = false;
        else {
            for (size_t i = 0; i < global_count; i++) {
                globals[i].name = ish_strdup(ctx.package_globals[i].name);
                globals[i].slot = ctx.package_globals[i].slot;
                if (!globals[i].name || !ish_scope_set_copy(&globals[i].scopes, &ctx.package_globals[i].scopes)) {
                    for (size_t j = 0; j <= i; j++) ish_pkg_global_destroy(&globals[j]);
                    free(globals);
                    globals = NULL;
                    copy_ok = false;
                    break;
                }
            }
        }
    }
    size_t method_count = ctx.decl_method_count;
    IshProtocolMethodDef *methods = NULL;
    if (copy_ok && method_count != 0) {
        methods = calloc(method_count, sizeof(*methods));
        if (!methods) copy_ok = false;
        else {
            for (size_t i = 0; i < method_count; i++) {
                methods[i].name = ish_strdup(ctx.decl_methods[i].name);
                methods[i].arity = ctx.decl_methods[i].arity;
                methods[i].has_default = ctx.decl_methods[i].has_default;
                methods[i].seen_decl = ctx.decl_methods[i].seen_decl;
                methods[i].exported = true;
                if (!methods[i].name || !ish_scope_set_copy(&methods[i].scopes, &ctx.decl_methods[i].scopes)) {
                    for (size_t j = 0; j <= i; j++) ish_protocol_method_def_destroy(&methods[j]);
                    free(methods);
                    methods = NULL;
                    copy_ok = false;
                    break;
                }
            }
        }
    }
    if (copy_ok && method_count != 0) {
        IshCore *define = ish_core_define_protocol(ish_atom(rt, ctx.protocol_name ? ctx.protocol_name : (name ? name : "<protocol>")), pkg ? pkg->span : ish_span_unknown(NULL));
        IshCore *seq = define ? ish_core_do(pkg ? pkg->span : ish_span_unknown(NULL)) : NULL;
        if (!define || !seq || !ish_core_do_add(seq, define)) {
            ish_core_free(define);
            ish_core_free(seq);
            copy_ok = false;
        } else {
            for (size_t i = 0; i < method_count && copy_ok; i++) {
                IshCore *default_fn = ctx.decl_methods[i].default_fn;
                ctx.decl_methods[i].default_fn = NULL;
                if (!ish_core_define_protocol_add_method(define, ish_atom(rt, ctx.decl_methods[i].name), ctx.decl_methods[i].arity, default_fn)) {
                    ish_core_free(default_fn);
                    copy_ok = false;
                    break;
                }
            }
            if (copy_ok && !ish_core_do_add(seq, body)) copy_ok = false;
            if (copy_ok) body = seq;
            else ish_core_free(seq);
        }
    }
    size_t macro_count = 0;
    size_t op_count = 0;
    if (copy_ok) {
        for (size_t i = macro_base; i < ctx.macro_count; i++) if (ctx.macros[i].exported) macro_count++;
        for (size_t i = op_base; i < ctx.operator_count; i++) if (ctx.operators[i].exported) op_count++;
    }
    IshPkgMacro *macros = NULL;
    IshOperatorDef *operators = NULL;
    if (copy_ok && macro_count != 0) {
        macros = calloc(macro_count, sizeof(*macros));
        if (!macros) copy_ok = false;
        else {
            size_t k = 0;
            for (size_t i = macro_base; i < ctx.macro_count && copy_ok; i++) {
                if (!ctx.macros[i].exported) continue;
                macros[k].name = ish_strdup(ctx.macros[i].name);
                if (!macros[k].name) { copy_ok = false; break; }
                macros[k].module = ish_module_ref_retain(ctx.macros[i].module);
                macros[k].function_index = ctx.macros[i].function_index;
                macros[k].phase_ns = ctx.macros[i].phase_ns;
                macros[k].phase_env = ish_phase_env_retain(ctx.macros[i].phase_env);
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
                IshOperatorDef *d = &operators[k];
                d->name = ish_strdup(ctx.operators[i].name);
                d->precedence = ctx.operators[i].precedence;
                d->assoc = ctx.operators[i].assoc;
                d->fixity = ctx.operators[i].fixity;
                d->target_kind = ctx.operators[i].target_kind;
                d->primitive = ctx.operators[i].primitive;
                d->target_name = ctx.operators[i].target_name ? ish_strdup(ctx.operators[i].target_name) : NULL;
                d->exported = true;
                ish_scope_set_init(&d->scopes);
                bool sok = ish_scope_set_copy(&d->scopes, &ctx.operators[i].scopes);
                if (!d->name || (ctx.operators[i].target_name && !d->target_name) || !sok) { copy_ok = false; break; }
                k++;
            }
        }
    }
    IshModuleRef *res_module = NULL;
    uint32_t res_fn = 0;
    IshNamespace *res_phase_ns = NULL;
    IshPhaseEnv *res_phase_env = NULL;
    if (copy_ok && ctx.decl_resolver) {
        res_module = ish_module_ref_retain(ctx.decl_resolver_module);
        res_fn = ctx.decl_resolver_fn;
        res_phase_ns = ctx.decl_resolver_phase_ns;
        res_phase_env = ish_phase_env_retain(ctx.decl_resolver_phase_env);
    }
    IshPhaseEnv *pkg_phase_env = copy_ok ? ish_phase_env_retain(ctx.phase_env) : NULL;
    IshArtifactDep *pkg_deps = NULL;
    size_t pkg_dep_count = 0;
    if (copy_ok) {
        pkg_deps = ctx.deps;
        pkg_dep_count = ctx.dep_count;
        ctx.deps = NULL;
        ctx.dep_count = 0;
        ctx.dep_cap = 0;
    }
    IshBytecodeModule *module = NULL;
    uint32_t init_fn = 0;
    if (copy_ok) {
        body = wrap_kernel_use(&ctx, body, err);
        if (!body) copy_ok = false;
    }
    if (copy_ok) {
        module = malloc(sizeof(*module));
        if (module) {
            ish_bc_init(module);
            if (!ish_core_compile_main(body, module, &init_fn, err)) { ish_bc_destroy(module); free(module); module = NULL; }
        }
    }
    ish_core_free(body);
    rt->local_expand_user = old_user;
    rt->local_expand = old_fn;
    rt->free_identifier_eq_user = old_free_identifier_eq_user;
    rt->free_identifier_eq = old_free_identifier_eq;
    rt->register_operator_user = old_register_operator_user;
    rt->register_operator = old_register_operator;
    rt->register_macro_user = old_register_macro_user;
    rt->register_macro = old_register_macro;
    rt->expander_surface_user = old_expander_surface_user;
    rt->expander_surface = old_expander_surface;
    *store = ctx.scope_store;
    ctx_destroy(&ctx);
    if (!copy_ok || !module) {
        free(name);
        if (exports) { for (size_t i = 0; i < export_count; i++) free(exports[i].name); free(exports); }
        if (globals) { for (size_t i = 0; i < global_count; i++) ish_pkg_global_destroy(&globals[i]); free(globals); }
        if (macros) { for (size_t i = 0; i < macro_count; i++) ish_pkg_macro_destroy(&macros[i]); free(macros); }
        if (operators) { for (size_t i = 0; i < op_count; i++) { free(operators[i].name); free(operators[i].target_name); ish_scope_set_destroy(&operators[i].scopes); } free(operators); }
        if (methods) { for (size_t i = 0; i < method_count; i++) ish_protocol_method_def_destroy(&methods[i]); free(methods); }
        ish_module_ref_release(res_module);
        ish_phase_env_release(res_phase_env);
        ish_phase_env_release(pkg_phase_env);
        for (size_t i = 0; i < pkg_dep_count; i++) free(pkg_deps[i].path);
        free(pkg_deps);
        if (err && !err->present) ish_error_oom(err, ish_span_unknown(NULL));
        return false;
    }
    out->module = module;
    out->init_fn = init_fn;
    out->name = name;
    out->exports = exports;
    out->export_count = export_count;
    out->globals = globals;
    out->global_count = global_count;
    out->macros = macros;
    out->macro_count = macro_count;
    out->operators = operators;
    out->operator_count = op_count;
    out->methods = methods;
    out->method_count = method_count;
    out->resolver_module = res_module;
    out->resolver_fn = res_fn;
    out->resolver_phase_ns = res_phase_ns;
    out->resolver_phase_env = res_phase_env;
    out->phase_env = pkg_phase_env;
    out->deps = pkg_deps;
    out->dep_count = pkg_dep_count;
    return true;
}

bool compile_package_module(ExpandContext *parent, const IshSyntax *pkg, const char *protocol_name_hint, IshArtifact *out, IshError *err) {
    return compile_package_artifact(parent->rt, &parent->scope_store, pkg, protocol_name_hint, false, out, err);
}
