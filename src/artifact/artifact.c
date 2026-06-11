#include "ish/artifact.h"

#include "ish/core.h"
#include "ish/vm.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

IshModuleRef *ish_module_ref_create(IshRuntime *rt) {
    IshModuleRef *ref = calloc(1u, sizeof(*ref));
    if (!ref) return NULL;
    ish_bc_init(&ref->module);
    ref->refs = 1;
    ref->rt = rt;
    if (!ish_runtime_register_gc_module(rt, &ref->module)) {
        free(ref);
        return NULL;
    }
    return ref;
}

IshModuleRef *ish_module_ref_retain(IshModuleRef *ref) {
    if (ref) ref->refs++;
    return ref;
}

void ish_module_ref_release(IshModuleRef *ref) {
    if (!ref) return;
    if (--ref->refs == 0) {
        ish_runtime_unregister_gc_module(ref->rt, &ref->module);
        ish_bc_destroy(&ref->module);
        free(ref);
    }
}

IshNamespace *ish_fresh_phase_namespace(IshRuntime *rt, IshError *err) {
    IshBuffer name;
    ish_buf_init(&name);
    if (!ish_buf_appendf(&name, "__phase_%zu", rt->ns_count)) {
        ish_buf_destroy(&name);
        ish_error_oom(err, ish_span_unknown(NULL));
        return NULL;
    }
    IshNamespace *ns = ish_namespace_get_or_create(rt, name.data ? name.data : "__phase");
    ish_buf_destroy(&name);
    if (!ns) ish_error_oom(err, ish_span_unknown(NULL));
    return ns;
}

IshPhaseEnv *ish_phase_env_create(IshRuntime *rt, IshNamespace *ns) {
    IshPhaseEnv *env = calloc(1u, sizeof(*env));
    if (!env) return NULL;
    env->rt = rt;
    env->ns = ns;
    env->refs = 1u;
    return env;
}

IshPhaseEnv *ish_phase_env_retain(IshPhaseEnv *env) {
    if (env) env->refs++;
    return env;
}

void ish_phase_env_release(IshPhaseEnv *env) {
    if (!env) return;
    if (--env->refs != 0) return;
    for (size_t i = 0; i < env->module_count; i++) {
        if (env->modules[i]) {
            ish_runtime_unregister_gc_module(env->rt, env->modules[i]);
            ish_bc_destroy(env->modules[i]);
            free(env->modules[i]);
        }
    }
    free(env->modules);
    free(env->module_main_fns);
    free(env);
}

bool ish_phase_env_add_module(IshPhaseEnv *env, IshBytecodeModule *module, uint32_t main_fn) {
    if (!env || !module) return false;
    if (env->module_count == env->module_cap) {
        size_t cap = env->module_cap ? env->module_cap * 2u : 4u;
        IshBytecodeModule **next = realloc(env->modules, cap * sizeof(*next));
        if (!next) return false;
        env->modules = next;
        uint32_t *fns = realloc(env->module_main_fns, cap * sizeof(*fns));
        if (!fns) return false;
        env->module_main_fns = fns;
        env->module_cap = cap;
    }
    if (!ish_runtime_register_gc_module(env->rt, module)) return false;
    env->module_main_fns[env->module_count] = main_fn;
    env->modules[env->module_count++] = module;
    return true;
}

void ish_operator_def_destroy(IshOperatorDef *op) {
    if (!op) return;
    free(op->name);
    free(op->target_name);
    ish_scope_set_destroy(&op->scopes);
    memset(op, 0, sizeof(*op));
}

void ish_protocol_method_def_destroy(IshProtocolMethodDef *method) {
    if (!method) return;
    free(method->name);
    ish_core_free(method->default_fn);
    ish_scope_set_destroy(&method->scopes);
    memset(method, 0, sizeof(*method));
}

void ish_pkg_macro_destroy(IshPkgMacro *macro) {
    if (!macro) return;
    free(macro->name);
    ish_module_ref_release(macro->module);
    macro->module = NULL;
    macro->name = NULL;
    macro->function_index = 0;
    macro->phase_ns = NULL;
    ish_phase_env_release(macro->phase_env);
    macro->phase_env = NULL;
}

void ish_pkg_global_destroy(IshPkgGlobal *global) {
    if (!global) return;
    free(global->name);
    ish_scope_set_destroy(&global->scopes);
    global->name = NULL;
    global->slot = 0;
}

void ish_artifact_destroy(IshArtifact *art) {
    if (art->module) { ish_bc_destroy(art->module); free(art->module); }
    free(art->name);
    if (art->exports) {
        for (size_t i = 0; i < art->export_count; i++) free(art->exports[i].name);
        free(art->exports);
    }
    if (art->globals) {
        for (size_t i = 0; i < art->global_count; i++) ish_pkg_global_destroy(&art->globals[i]);
        free(art->globals);
    }
    if (art->macros) {
        for (size_t i = 0; i < art->macro_count; i++) ish_pkg_macro_destroy(&art->macros[i]);
        free(art->macros);
    }
    if (art->operators) {
        for (size_t i = 0; i < art->operator_count; i++) ish_operator_def_destroy(&art->operators[i]);
        free(art->operators);
    }
    if (art->methods) {
        for (size_t i = 0; i < art->method_count; i++) ish_protocol_method_def_destroy(&art->methods[i]);
        free(art->methods);
    }
    ish_module_ref_release(art->resolver_module);
    ish_phase_env_release(art->resolver_phase_env);
    ish_phase_env_release(art->phase_env);
    for (size_t i = 0; i < art->dep_count; i++) free(art->deps[i].path);
    free(art->deps);
    memset(art, 0, sizeof(*art));
}

static int package_name_compare(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static bool package_read_directory_source(const char *path, DIR *dir, IshBuffer *out_src, IshSpan span, IshError *err) {
    char **names = NULL;
    size_t count = 0;
    size_t cap = 0;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 5 || strcmp(entry->d_name + len - 4, ".ish") != 0) continue;
        if (count == cap) {
            size_t next = cap ? cap * 2u : 8u;
            char **grown = realloc(names, next * sizeof(*grown));
            if (!grown) { ok = false; break; }
            names = grown;
            cap = next;
        }
        names[count] = ish_strdup(entry->d_name);
        if (!names[count]) { ok = false; break; }
        count++;
    }
    closedir(dir);
    if (!ok) { for (size_t i = 0; i < count; i++) free(names[i]); free(names); return ish_error_oom(err, span); }
    if (count == 0) { free(names); return ish_error_set(err, span, "package directory '%s' contains no .ish source files", path); }
    qsort(names, count, sizeof(*names), package_name_compare);
    for (size_t i = 0; i < count && ok; i++) {
        IshBuffer full;
        ish_buf_init(&full);
        if (!ish_buf_append(&full, path) || !ish_buf_append_char(&full, '/') || !ish_buf_append(&full, names[i])) { ish_buf_destroy(&full); ok = false; break; }
        char *src = NULL;
        size_t src_len = 0;
        bool read_ok = ish_read_file(full.data, &src, &src_len, err);
        ish_buf_destroy(&full);
        if (!read_ok) { ok = false; break; }
        bool appended = ish_buf_append_n(out_src, src, src_len) && ish_buf_append_char(out_src, '\n');
        free(src);
        if (!appended) { ok = false; ish_error_oom(err, span); break; }
    }
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    return ok;
}

bool ish_package_read_source(IshRuntime *rt, const char *path, IshBuffer *out_src, const char **out_label, IshSpan span, IshError *err) {
    DIR *dir = opendir(path);
    if (dir) {
        if (!package_read_directory_source(path, dir, out_src, span, err)) return false;
        const char *label = ish_symbol_text(ish_intern(&rt->intern, ISH_SYMBOL_WORD, path));
        if (!label) return ish_error_oom(err, span);
        *out_label = label;
        return true;
    }
    IshBuffer fn;
    ish_buf_init(&fn);
    if (!ish_buf_append(&fn, path) || !ish_buf_append(&fn, ".ish")) { ish_buf_destroy(&fn); return ish_error_oom(err, span); }
    const char *file = ish_symbol_text(ish_intern(&rt->intern, ISH_SYMBOL_WORD, fn.data));
    ish_buf_destroy(&fn);
    if (!file) return ish_error_oom(err, span);
    char *src = NULL;
    size_t src_len = 0;
    if (!ish_read_file(file, &src, &src_len, err)) return false;
    bool appended = ish_buf_append_n(out_src, src, src_len);
    free(src);
    if (!appended) return ish_error_oom(err, span);
    *out_label = file;
    return true;
}

#define ISHA_VERSION 7u

static bool artifact_noop_register_operator(void *user, IshRuntime *rt, const IshSyntax *name, int64_t precedence, const char *assoc, const char *fixity, const IshSyntax *target, IshError *err) {
    (void)user; (void)rt; (void)name; (void)precedence; (void)assoc; (void)fixity; (void)target; (void)err;
    return true;
}

static bool artifact_noop_register_macro(void *user, IshRuntime *rt, const IshSyntax *name, IshValue transformer, IshError *err) {
    (void)user; (void)rt; (void)name; (void)transformer; (void)err;
    return true;
}

static bool put_scope_set(IshBuffer *out, const IshScopeSet *set) {
    if (!ish_buf_put_u32(out, (uint32_t)set->count)) return false;
    for (size_t i = 0; i < set->count; i++) {
        if (!ish_buf_put_u32(out, set->items[i])) return false;
    }
    return true;
}

static bool read_scope_set(IshByteReader *r, IshScopeSet *set) {
    uint32_t n = ish_rd_u32(r);
    if (!r->ok) return false;
    for (uint32_t i = 0; i < n; i++) {
        IshScopeId id = ish_rd_u32(r);
        if (!r->ok || !ish_scope_set_add(set, id)) return false;
    }
    return true;
}

static bool put_module_blob(IshBuffer *out, const IshBytecodeModule *module, IshError *err) {
    IshBuffer blob;
    ish_buf_init(&blob);
    if (!ish_ishc_serialize(module, &blob, err)) {
        ish_buf_destroy(&blob);
        return false;
    }
    bool ok = ish_buf_put_u64(out, (uint64_t)blob.len) && ish_buf_append_n(out, blob.data ? blob.data : "", blob.len);
    ish_buf_destroy(&blob);
    return ok ? true : ish_error_oom(err, ish_span_unknown(NULL));
}

static bool read_module_blob(IshRuntime *rt, IshByteReader *r, IshBytecodeModule *module, IshError *err) {
    uint64_t len = ish_rd_u64(r);
    if (!r->ok || len > r->len - r->pos) return ish_error_set(err, ish_span_unknown(NULL), "truncated artifact module");
    bool ok = ish_ishc_deserialize(rt, r->data + r->pos, (size_t)len, module, err);
    r->pos += (size_t)len;
    return ok;
}

bool ish_artifact_serialize(const IshArtifact *art, IshBuffer *out, IshError *err) {
    bool ok = ish_buf_append_n(out, "ISHA", 4u) && ish_buf_put_u32(out, ISHA_VERSION);
    ok = ok && ish_buf_append_n(out, (const char *)art->src_hash, 32u);
    ok = ok && ish_buf_put_u32(out, (uint32_t)art->dep_count);
    for (size_t i = 0; ok && i < art->dep_count; i++) {
        ok = ish_buf_put_str(out, art->deps[i].path, strlen(art->deps[i].path));
        ok = ok && ish_buf_append_n(out, (const char *)art->deps[i].hash, 32u);
    }
    ok = ok && ish_buf_put_u32(out, art->scope_base) && ish_buf_put_u32(out, art->scope_end);
    ok = ok && ish_buf_put_u8(out, art->name ? 1u : 0u);
    if (ok && art->name) ok = ish_buf_put_str(out, art->name, strlen(art->name));
    if (!ok) return ish_error_oom(err, ish_span_unknown(NULL));
    if (!ish_buf_put_u32(out, art->init_fn)) return ish_error_oom(err, ish_span_unknown(NULL));
    if (!put_module_blob(out, art->module, err)) return false;
    ok = ish_buf_put_u32(out, (uint32_t)art->export_count);
    for (size_t i = 0; ok && i < art->export_count; i++) {
        ok = ish_buf_put_str(out, art->exports[i].name, strlen(art->exports[i].name)) && ish_buf_put_u32(out, art->exports[i].slot);
    }
    ok = ok && ish_buf_put_u32(out, (uint32_t)art->global_count);
    for (size_t i = 0; ok && i < art->global_count; i++) {
        ok = ish_buf_put_str(out, art->globals[i].name, strlen(art->globals[i].name)) && ish_buf_put_u32(out, art->globals[i].slot) && put_scope_set(out, &art->globals[i].scopes);
    }
    ok = ok && ish_buf_put_u32(out, (uint32_t)art->operator_count);
    for (size_t i = 0; ok && i < art->operator_count; i++) {
        const IshOperatorDef *op = &art->operators[i];
        ok = ish_buf_put_str(out, op->name, strlen(op->name));
        ok = ok && ish_buf_put_u8(out, op->precedence) && ish_buf_put_u8(out, (uint8_t)op->assoc) && ish_buf_put_u8(out, (uint8_t)op->fixity) && ish_buf_put_u8(out, (uint8_t)op->target_kind);
        ok = ok && ish_buf_put_u32(out, (uint32_t)op->primitive);
        ok = ok && ish_buf_put_u8(out, op->target_name ? 1u : 0u);
        if (ok && op->target_name) ok = ish_buf_put_str(out, op->target_name, strlen(op->target_name));
        ok = ok && put_scope_set(out, &op->scopes);
    }
    if (!ok) return ish_error_oom(err, ish_span_unknown(NULL));
    if (!ish_buf_put_u32(out, (uint32_t)art->macro_count)) return ish_error_oom(err, ish_span_unknown(NULL));
    for (size_t i = 0; i < art->macro_count; i++) {
        if (!ish_buf_put_str(out, art->macros[i].name, strlen(art->macros[i].name)) || !ish_buf_put_u32(out, art->macros[i].function_index)) return ish_error_oom(err, ish_span_unknown(NULL));
        if (!put_module_blob(out, &art->macros[i].module->module, err)) return false;
    }
    ok = ish_buf_put_u32(out, (uint32_t)art->method_count);
    for (size_t i = 0; ok && i < art->method_count; i++) {
        const IshProtocolMethodDef *m = &art->methods[i];
        ok = ish_buf_put_str(out, m->name, strlen(m->name)) && ish_buf_put_u32(out, m->arity);
        ok = ok && ish_buf_put_u8(out, m->has_default ? 1u : 0u) && ish_buf_put_u8(out, m->seen_decl ? 1u : 0u);
        ok = ok && put_scope_set(out, &m->scopes);
    }
    if (!ok) return ish_error_oom(err, ish_span_unknown(NULL));
    if (!ish_buf_put_u8(out, art->resolver_module ? 1u : 0u)) return ish_error_oom(err, ish_span_unknown(NULL));
    if (art->resolver_module) {
        if (!ish_buf_put_u32(out, art->resolver_fn)) return ish_error_oom(err, ish_span_unknown(NULL));
        if (!put_module_blob(out, &art->resolver_module->module, err)) return false;
    }
    size_t phase_count = art->phase_env ? art->phase_env->module_count : 0;
    if (!ish_buf_put_u32(out, (uint32_t)phase_count)) return ish_error_oom(err, ish_span_unknown(NULL));
    for (size_t i = 0; i < phase_count; i++) {
        if (!ish_buf_put_u32(out, art->phase_env->module_main_fns[i])) return ish_error_oom(err, ish_span_unknown(NULL));
        if (!put_module_blob(out, art->phase_env->modules[i], err)) return false;
    }
    return true;
}

static bool artifact_read_str(IshByteReader *r, char **out, IshError *err) {
    char *s = ish_rd_string(r);
    if (!s) return ish_error_set(err, ish_span_unknown(NULL), "truncated artifact string");
    *out = s;
    return true;
}

bool ish_artifact_deserialize(IshRuntime *rt, const unsigned char *data, size_t len, IshArtifact *out, IshError *err) {
    memset(out, 0, sizeof(*out));
    IshByteReader r;
    ish_byte_reader_init(&r, data, len);
    if (len < 8u || memcmp(data, "ISHA", 4u) != 0) return ish_error_set(err, ish_span_unknown(NULL), "not an .ishc artifact");
    r.pos = 4u;
    uint32_t version = ish_rd_u32(&r);
    if (version != ISHA_VERSION) return ish_error_set(err, ish_span_unknown(NULL), ".ishc artifact version %u unsupported", version);
    if (r.pos + 32u > r.len) return ish_error_set(err, ish_span_unknown(NULL), "truncated artifact hash");
    memcpy(out->src_hash, r.data + r.pos, 32u);
    r.pos += 32u;
    bool ok = true;
    uint32_t dep_count = ish_rd_u32(&r);
    if (!r.ok) ok = false;
    if (ok && dep_count != 0) {
        out->deps = calloc(dep_count, sizeof(*out->deps));
        if (!out->deps) ok = false;
        for (uint32_t i = 0; ok && i < dep_count; i++) {
            ok = artifact_read_str(&r, &out->deps[i].path, err);
            if (ok && r.pos + 32u > r.len) ok = false;
            if (ok) {
                memcpy(out->deps[i].hash, r.data + r.pos, 32u);
                r.pos += 32u;
                out->dep_count = i + 1u;
            }
        }
    }
    out->scope_base = ok ? ish_rd_u32(&r) : 0;
    out->scope_end = ok ? ish_rd_u32(&r) : 0;
    uint8_t has_name = ok ? ish_rd_u8(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && has_name) ok = artifact_read_str(&r, &out->name, err);
    out->init_fn = ok ? ish_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok) {
        out->module = malloc(sizeof(*out->module));
        if (!out->module) ok = false;
        else {
            ish_bc_init(out->module);
            if (!read_module_blob(rt, &r, out->module, err)) {
                free(out->module);
                out->module = NULL;
                ok = false;
            }
        }
    }
    uint32_t export_count = ok ? ish_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && export_count != 0) {
        out->exports = calloc(export_count, sizeof(*out->exports));
        if (!out->exports) ok = false;
        for (uint32_t i = 0; ok && i < export_count; i++) {
            ok = artifact_read_str(&r, &out->exports[i].name, err);
            out->exports[i].slot = ok ? ish_rd_u32(&r) : 0;
            if (ok && !r.ok) ok = false;
            if (ok) out->export_count = i + 1u;
        }
    }
    uint32_t global_count = ok ? ish_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && global_count != 0) {
        out->globals = calloc(global_count, sizeof(*out->globals));
        if (!out->globals) ok = false;
        for (uint32_t i = 0; ok && i < global_count; i++) {
            ok = artifact_read_str(&r, &out->globals[i].name, err);
            out->globals[i].slot = ok ? ish_rd_u32(&r) : 0;
            if (ok && !r.ok) ok = false;
            ish_scope_set_init(&out->globals[i].scopes);
            if (ok) ok = read_scope_set(&r, &out->globals[i].scopes);
            if (ok) out->global_count = i + 1u;
        }
    }
    uint32_t operator_count = ok ? ish_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && operator_count != 0) {
        out->operators = calloc(operator_count, sizeof(*out->operators));
        if (!out->operators) ok = false;
        for (uint32_t i = 0; ok && i < operator_count; i++) {
            IshOperatorDef *op = &out->operators[i];
            ok = artifact_read_str(&r, &op->name, err);
            op->precedence = ok ? ish_rd_u8(&r) : 0;
            op->assoc = (IshOpAssoc)(ok ? ish_rd_u8(&r) : 0);
            op->fixity = (IshOpFixity)(ok ? ish_rd_u8(&r) : 0);
            op->target_kind = (IshOpTargetKind)(ok ? ish_rd_u8(&r) : 0);
            op->primitive = (IshPrimitive)(ok ? ish_rd_u32(&r) : 0);
            uint8_t has_target = ok ? ish_rd_u8(&r) : 0;
            if (ok && !r.ok) ok = false;
            if (ok && has_target) ok = artifact_read_str(&r, &op->target_name, err);
            op->exported = true;
            ish_scope_set_init(&op->scopes);
            if (ok) ok = read_scope_set(&r, &op->scopes);
            if (ok) out->operator_count = i + 1u;
        }
    }
    uint32_t macro_count = ok ? ish_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && macro_count != 0) {
        out->macros = calloc(macro_count, sizeof(*out->macros));
        if (!out->macros) ok = false;
        for (uint32_t i = 0; ok && i < macro_count; i++) {
            ok = artifact_read_str(&r, &out->macros[i].name, err);
            out->macros[i].function_index = ok ? ish_rd_u32(&r) : 0;
            if (ok && !r.ok) ok = false;
            if (ok) {
                out->macros[i].module = ish_module_ref_create(rt);
                if (!out->macros[i].module) ok = false;
                else if (!read_module_blob(rt, &r, &out->macros[i].module->module, err)) ok = false;
            }
            if (ok) out->macro_count = i + 1u;
            else if (out->macros[i].name) out->macro_count = i + 1u;
        }
    }
    uint32_t method_count = ok ? ish_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && method_count != 0) {
        out->methods = calloc(method_count, sizeof(*out->methods));
        if (!out->methods) ok = false;
        for (uint32_t i = 0; ok && i < method_count; i++) {
            IshProtocolMethodDef *m = &out->methods[i];
            ok = artifact_read_str(&r, &m->name, err);
            m->arity = ok ? ish_rd_u32(&r) : 0;
            m->has_default = ok && ish_rd_u8(&r) != 0;
            m->seen_decl = ok && ish_rd_u8(&r) != 0;
            if (ok && !r.ok) ok = false;
            m->exported = true;
            ish_scope_set_init(&m->scopes);
            if (ok) ok = read_scope_set(&r, &m->scopes);
            if (ok) out->method_count = i + 1u;
        }
    }
    uint8_t has_resolver = ok ? ish_rd_u8(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && has_resolver) {
        out->resolver_fn = ish_rd_u32(&r);
        if (!r.ok) ok = false;
        if (ok) {
            out->resolver_module = ish_module_ref_create(rt);
            if (!out->resolver_module) ok = false;
            else if (!read_module_blob(rt, &r, &out->resolver_module->module, err)) ok = false;
        }
    }
    IshNamespace *phase_ns = NULL;
    IshPhaseEnv *phase_env = NULL;
    if (ok) {
        phase_ns = ish_fresh_phase_namespace(rt, err);
        phase_env = phase_ns ? ish_phase_env_create(rt, phase_ns) : NULL;
        if (!phase_env) ok = false;
    }
    uint32_t phase_count = ok ? ish_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    for (uint32_t i = 0; ok && i < phase_count; i++) {
        uint32_t main_fn = ish_rd_u32(&r);
        if (!r.ok) { ok = false; break; }
        IshBytecodeModule *module = malloc(sizeof(*module));
        if (!module) { ok = false; break; }
        ish_bc_init(module);
        if (!read_module_blob(rt, &r, module, err)) {
            free(module);
            ok = false;
            break;
        }
        IshNamespace *old_main_ns = rt->main_ns;
        void *old_op_user = rt->register_operator_user;
        IshRegisterOperatorFn old_op = rt->register_operator;
        void *old_mac_user = rt->register_macro_user;
        IshRegisterMacroFn old_mac = rt->register_macro;
        rt->main_ns = phase_ns;
        rt->register_operator_user = NULL;
        rt->register_operator = artifact_noop_register_operator;
        rt->register_macro_user = NULL;
        rt->register_macro = artifact_noop_register_macro;
        IshValue ignored = ish_nil();
        bool ran = ish_vm_run(rt, module, main_fn, &ignored, err);
        rt->main_ns = old_main_ns;
        rt->register_operator_user = old_op_user;
        rt->register_operator = old_op;
        rt->register_macro_user = old_mac_user;
        rt->register_macro = old_mac;
        if (!ran || !ish_phase_env_add_module(phase_env, module, main_fn)) {
            ish_bc_destroy(module);
            free(module);
            ok = false;
            break;
        }
    }
    if (ok) {
        out->phase_env = phase_env;
        for (size_t i = 0; i < out->macro_count; i++) {
            out->macros[i].phase_ns = phase_ns;
            out->macros[i].phase_env = ish_phase_env_retain(phase_env);
        }
        if (out->resolver_module) {
            out->resolver_phase_ns = phase_ns;
            out->resolver_phase_env = ish_phase_env_retain(phase_env);
        }
        if (err && !err->present && r.pos != r.len) ok = ish_error_set(err, ish_span_unknown(NULL), ".ishc artifact has trailing data");
    } else {
        ish_phase_env_release(phase_env);
    }
    if (!ok) {
        if (err && !err->present) ish_error_set(err, ish_span_unknown(NULL), "truncated .ishc artifact");
        ish_artifact_destroy(out);
        return false;
    }
    return true;
}

bool ish_artifact_cache_disabled(void) {
    const char *v = getenv("ISH_ISHC");
    return v && strcmp(v, "0") == 0;
}

void ish_artifact_cache_write(const char *path, const IshArtifact *art) {
    if (ish_artifact_cache_disabled()) return;
    IshError werr;
    ish_error_init(&werr);
    IshBuffer blob;
    ish_buf_init(&blob);
    bool ok = ish_artifact_serialize(art, &blob, &werr);
    if (ok) {
        IshBuffer ishc_path;
        IshBuffer tmp_path;
        ish_buf_init(&ishc_path);
        ish_buf_init(&tmp_path);
        ok = ish_buf_append(&ishc_path, path) && ish_buf_append(&ishc_path, ".ishc");
        ok = ok && ish_buf_append(&tmp_path, ishc_path.data) && ish_buf_appendf(&tmp_path, ".tmp.%ld", (long)getpid());
        if (ok) {
            FILE *f = fopen(tmp_path.data, "wb");
            if (f) {
                ok = fwrite(blob.data, 1u, blob.len, f) == blob.len;
                ok = fclose(f) == 0 && ok;
                if (ok) ok = rename(tmp_path.data, ishc_path.data) == 0;
                if (!ok) remove(tmp_path.data);
            }
        }
        ish_buf_destroy(&ishc_path);
        ish_buf_destroy(&tmp_path);
    }
    ish_buf_destroy(&blob);
    ish_error_clear(&werr);
}

bool ish_artifact_path_verified(IshRuntime *rt, const char *path, const unsigned char want[32]) {
    IshBuffer src;
    ish_buf_init(&src);
    const char *label = NULL;
    IshError rerr;
    ish_error_init(&rerr);
    bool ok = ish_package_read_source(rt, path, &src, &label, ish_span_unknown(NULL), &rerr);
    unsigned char got[32];
    if (ok) ish_sha256(src.data ? src.data : "", src.len, got);
    ish_buf_destroy(&src);
    ish_error_clear(&rerr);
    return ok && memcmp(got, want, 32u) == 0;
}

bool ish_artifact_cache_load(IshRuntime *rt, const char *path, const unsigned char src_hash[32], IshArtifact *out) {
    if (ish_artifact_cache_disabled()) return false;
    IshBuffer ishc_path;
    ish_buf_init(&ishc_path);
    if (!ish_buf_append(&ishc_path, path) || !ish_buf_append(&ishc_path, ".ishc")) {
        ish_buf_destroy(&ishc_path);
        return false;
    }
    char *data = NULL;
    size_t len = 0;
    IshError rerr;
    ish_error_init(&rerr);
    bool ok = ish_read_file(ishc_path.data, &data, &len, &rerr);
    ish_buf_destroy(&ishc_path);
    ish_error_clear(&rerr);
    if (!ok) return false;
    IshError derr;
    ish_error_init(&derr);
    ok = ish_artifact_deserialize(rt, (const unsigned char *)data, len, out, &derr);
    free(data);
    ish_error_clear(&derr);
    if (!ok) return false;
    if (memcmp(out->src_hash, src_hash, 32u) != 0) {
        ish_artifact_destroy(out);
        return false;
    }
    for (size_t i = 0; i < out->dep_count; i++) {
        if (!ish_artifact_path_verified(rt, out->deps[i].path, out->deps[i].hash)) {
            ish_artifact_destroy(out);
            return false;
        }
    }
    return true;
}
