#include "idiom/artifact.h"

#include "idiom/core.h"
#include "idiom/vm.h"

#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

IdmModuleRef *idm_module_ref_create(IdmRuntime *rt) {
    IdmModuleRef *ref = calloc(1u, sizeof(*ref));
    if (!ref) return NULL;
    idm_bc_init(&ref->module);
    ref->refs = 1;
    ref->rt = rt;
    return ref;
}

IdmModuleRef *idm_module_ref_retain(IdmModuleRef *ref) {
    if (ref) ref->refs++;
    return ref;
}

void idm_module_ref_release(IdmModuleRef *ref) {
    if (!ref) return;
    if (--ref->refs == 0) {
        idm_bc_destroy(&ref->module);
        free(ref);
    }
}

IdmNamespace *idm_fresh_phase_namespace(IdmRuntime *rt, IdmError *err) {
    IdmBuffer name;
    idm_buf_init(&name);
    if (!idm_buf_appendf(&name, "__phase_%zu", rt->ns_count)) {
        idm_buf_destroy(&name);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    IdmNamespace *ns = idm_namespace_get_or_create(rt, name.data ? name.data : "__phase");
    idm_buf_destroy(&name);
    if (!ns) idm_error_oom(err, idm_span_unknown(NULL));
    return ns;
}

IdmPhaseEnv *idm_phase_env_create(IdmRuntime *rt, IdmNamespace *ns) {
    IdmPhaseEnv *env = calloc(1u, sizeof(*env));
    if (!env) return NULL;
    env->rt = rt;
    env->ns = ns;
    env->refs = 1u;
    return env;
}

IdmPhaseEnv *idm_phase_env_retain(IdmPhaseEnv *env) {
    if (env) env->refs++;
    return env;
}

void idm_phase_env_release(IdmPhaseEnv *env) {
    if (!env) return;
    if (--env->refs != 0) return;
    for (size_t i = 0; i < env->module_count; i++) {
        if (env->modules[i]) {
            idm_bc_destroy(env->modules[i]);
            free(env->modules[i]);
        }
    }
    free(env->modules);
    free(env->module_main_fns);
    free(env);
}

bool idm_phase_env_add_module(IdmPhaseEnv *env, IdmBytecodeModule *module, uint32_t main_fn, IdmError *err) {
    if (!env || !module) return false;
    if (!idm_bc_intern_literals(env->rt, module, err)) return false;
    if (env->module_count == env->module_cap) {
        size_t cap = env->module_cap ? env->module_cap * 2u : 4u;
        IdmBytecodeModule **next = realloc(env->modules, cap * sizeof(*next));
        if (!next) return false;
        env->modules = next;
        uint32_t *fns = realloc(env->module_main_fns, cap * sizeof(*fns));
        if (!fns) return false;
        env->module_main_fns = fns;
        env->module_cap = cap;
    }
    env->module_main_fns[env->module_count] = main_fn;
    env->modules[env->module_count++] = module;
    return true;
}

void idm_operator_def_destroy(IdmOperatorDef *op) {
    if (!op) return;
    free(op->name);
    free(op->capture);
    free(op->target_name);
    idm_module_ref_release(op->target_module);
    idm_phase_env_release(op->target_phase_env);
    idm_scope_set_destroy(&op->scopes);
    memset(op, 0, sizeof(*op));
}

void idm_trait_method_def_destroy(IdmTraitMethodDef *method) {
    if (!method) return;
    free(method->name);
    idm_core_free(method->default_fn);
    idm_scope_set_destroy(&method->scopes);
    memset(method, 0, sizeof(*method));
}

void idm_trait_requirement_def_destroy(IdmTraitRequirementDef *requirement) {
    if (!requirement) return;
    free(requirement->name);
    memset(requirement, 0, sizeof(*requirement));
}

void idm_pkg_macro_destroy(IdmPkgMacro *macro) {
    if (!macro) return;
    free(macro->name);
    idm_module_ref_release(macro->module);
    macro->module = NULL;
    macro->name = NULL;
    macro->function_index = 0;
    macro->phase_ns = NULL;
    idm_phase_env_release(macro->phase_env);
    macro->phase_env = NULL;
}

void idm_pkg_global_destroy(IdmPkgGlobal *global) {
    if (!global) return;
    free(global->name);
    idm_scope_set_destroy(&global->scopes);
    global->name = NULL;
    global->slot = 0;
    global->arity = idm_arity_unknown();
}

void idm_pkg_trait_destroy(IdmPkgTrait *trait) {
    if (!trait) return;
    free(trait->name);
    free(trait->identity);
    for (size_t i = 0; i < trait->requirement_count; i++) idm_trait_requirement_def_destroy(&trait->requirements[i]);
    free(trait->requirements);
    for (size_t i = 0; i < trait->method_count; i++) idm_trait_method_def_destroy(&trait->methods[i]);
    free(trait->methods);
    memset(trait, 0, sizeof(*trait));
}

void idm_pkg_type_destroy(IdmPkgType *type) {
    if (!type) return;
    free(type->name);
    free(type->identity);
    idm_scope_set_destroy(&type->scopes);
    for (size_t i = 0; i < type->field_count; i++) free(type->fields[i]);
    free(type->fields);
    memset(type, 0, sizeof(*type));
}

bool idm_trait_method_defs_copy(const IdmTraitMethodDef *src, size_t count, IdmTraitMethodDef **out) {
    *out = NULL;
    if (count == 0) return true;
    IdmTraitMethodDef *methods = calloc(count, sizeof(*methods));
    if (!methods) return false;
    for (size_t i = 0; i < count; i++) {
        methods[i].name = idm_strdup(src[i].name);
        methods[i].arity = src[i].arity;
        methods[i].has_default = src[i].has_default;
        methods[i].seen_decl = src[i].seen_decl;
        methods[i].exported = true;
        if (!methods[i].name || !idm_scope_set_copy(&methods[i].scopes, &src[i].scopes)) {
            for (size_t j = 0; j <= i; j++) idm_trait_method_def_destroy(&methods[j]);
            free(methods);
            return false;
        }
    }
    *out = methods;
    return true;
}

bool idm_trait_requirement_defs_copy(const IdmTraitRequirementDef *src, size_t count, IdmTraitRequirementDef **out) {
    *out = NULL;
    if (count == 0) return true;
    IdmTraitRequirementDef *requirements = calloc(count, sizeof(*requirements));
    if (!requirements) return false;
    for (size_t i = 0; i < count; i++) {
        requirements[i].name = idm_strdup(src[i].name);
        if (!requirements[i].name) {
            for (size_t j = 0; j <= i; j++) idm_trait_requirement_def_destroy(&requirements[j]);
            free(requirements);
            return false;
        }
    }
    *out = requirements;
    return true;
}

void idm_artifact_destroy(IdmArtifact *art) {
    if (art->module) { idm_bc_destroy(art->module); free(art->module); }
    free(art->name);
    if (art->exports) {
        for (size_t i = 0; i < art->export_count; i++) free(art->exports[i].name);
        free(art->exports);
    }
    if (art->globals) {
        for (size_t i = 0; i < art->global_count; i++) idm_pkg_global_destroy(&art->globals[i]);
        free(art->globals);
    }
    if (art->macros) {
        for (size_t i = 0; i < art->macro_count; i++) idm_pkg_macro_destroy(&art->macros[i]);
        free(art->macros);
    }
    if (art->operators) {
        for (size_t i = 0; i < art->operator_count; i++) idm_operator_def_destroy(&art->operators[i]);
        free(art->operators);
    }
    if (art->types) {
        for (size_t i = 0; i < art->type_count; i++) idm_pkg_type_destroy(&art->types[i]);
        free(art->types);
    }
    if (art->traits) {
        for (size_t i = 0; i < art->trait_count; i++) idm_pkg_trait_destroy(&art->traits[i]);
        free(art->traits);
    }
    idm_module_ref_release(art->resolver_module);
    idm_phase_env_release(art->resolver_phase_env);
    idm_phase_env_release(art->phase_env);
    for (size_t i = 0; i < art->dep_count; i++) free(art->deps[i].path);
    free(art->deps);
    memset(art, 0, sizeof(*art));
}

static int package_name_compare(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static bool package_read_directory_source(const char *path, DIR *dir, IdmBuffer *out_src, IdmSpan span, IdmError *err) {
    char **names = NULL;
    size_t count = 0;
    size_t cap = 0;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 3, ".id") != 0) continue;
        if (count == cap) {
            size_t next = cap ? cap * 2u : 8u;
            char **grown = realloc(names, next * sizeof(*grown));
            if (!grown) { ok = false; break; }
            names = grown;
            cap = next;
        }
        names[count] = idm_strdup(entry->d_name);
        if (!names[count]) { ok = false; break; }
        count++;
    }
    closedir(dir);
    if (!ok) { for (size_t i = 0; i < count; i++) free(names[i]); free(names); return idm_error_oom(err, span); }
    if (count == 0) { free(names); return idm_error_set(err, span, "package directory '%s' contains no .id source files", path); }
    qsort(names, count, sizeof(*names), package_name_compare);
    for (size_t i = 0; i < count && ok; i++) {
        IdmBuffer full;
        idm_buf_init(&full);
        if (!idm_buf_append(&full, path) || !idm_buf_append_char(&full, '/') || !idm_buf_append(&full, names[i])) { idm_buf_destroy(&full); ok = false; break; }
        char *src = NULL;
        size_t src_len = 0;
        bool read_ok = idm_read_file(full.data, &src, &src_len, err);
        idm_buf_destroy(&full);
        if (!read_ok) { ok = false; break; }
        bool appended = idm_buf_append_n(out_src, src, src_len) && idm_buf_append_char(out_src, '\n');
        free(src);
        if (!appended) { ok = false; idm_error_oom(err, span); break; }
    }
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    return ok;
}

static bool package_read_at(IdmRuntime *rt, const char *resolved, IdmBuffer *out_src, const char **out_label, bool *out_found, IdmSpan span, IdmError *err) {
    *out_found = false;
    DIR *dir = opendir(resolved);
    if (dir) {
        if (!package_read_directory_source(resolved, dir, out_src, span, err)) return false;
        const char *label = idm_symbol_text(idm_intern(&rt->intern, IDM_SYMBOL_WORD, resolved));
        if (!label) return idm_error_oom(err, span);
        *out_label = label;
        *out_found = true;
        return true;
    }
    IdmBuffer fn;
    idm_buf_init(&fn);
    if (!idm_buf_append(&fn, resolved) || !idm_buf_append(&fn, ".id")) { idm_buf_destroy(&fn); return idm_error_oom(err, span); }
    if (access(fn.data, R_OK) != 0) {
        idm_buf_destroy(&fn);
        return true;
    }
    const char *file = idm_symbol_text(idm_intern(&rt->intern, IDM_SYMBOL_WORD, fn.data));
    idm_buf_destroy(&fn);
    if (!file) return idm_error_oom(err, span);
    char *src = NULL;
    size_t src_len = 0;
    if (!idm_read_file(file, &src, &src_len, err)) return false;
    bool appended = idm_buf_append_n(out_src, src, src_len);
    free(src);
    if (!appended) return idm_error_oom(err, span);
    *out_label = file;
    *out_found = true;
    return true;
}

bool idm_package_read_source(IdmRuntime *rt, const char *path, IdmBuffer *out_src, const char **out_label, IdmSpan span, IdmError *err) {
    bool found = false;
    if (strncmp(path, "std/", 4u) == 0) {
        const char *root = getenv("IDIOMROOT");
        if (root && root[0]) {
            IdmBuffer rooted;
            idm_buf_init(&rooted);
            if (!idm_buf_append(&rooted, root) || !idm_buf_append(&rooted, "/") || !idm_buf_append(&rooted, path + 4u)) {
                idm_buf_destroy(&rooted);
                return idm_error_oom(err, span);
            }
            bool ok = package_read_at(rt, rooted.data, out_src, out_label, &found, span, err);
            idm_buf_destroy(&rooted);
            if (!ok) return false;
            if (found) return true;
        }
    }
    if (!package_read_at(rt, path, out_src, out_label, &found, span, err)) return false;
    if (found) return true;
    const char *search = getenv("IDIOMPATH");
    if (search && search[0]) {
        const char *cursor = search;
        while (*cursor) {
            const char *colon = strchr(cursor, ':');
            size_t seg_len = colon ? (size_t)(colon - cursor) : strlen(cursor);
            if (seg_len != 0) {
                IdmBuffer cand;
                idm_buf_init(&cand);
                if (!idm_buf_append_n(&cand, cursor, seg_len) || !idm_buf_append(&cand, "/") || !idm_buf_append(&cand, path)) {
                    idm_buf_destroy(&cand);
                    return idm_error_oom(err, span);
                }
                bool ok = package_read_at(rt, cand.data, out_src, out_label, &found, span, err);
                idm_buf_destroy(&cand);
                if (!ok) return false;
                if (found) return true;
            }
            if (!colon) break;
            cursor = colon + 1u;
        }
    }
    return idm_error_set(err, span, "package '%s' not found (searched cwd%s%s)", path,
                         strncmp(path, "std/", 4u) == 0 ? ", IDIOMROOT" : "",
                         search && search[0] ? ", IDIOMPATH" : "");
}

#define IDM_ARTIFACT_VERSION 29u

static bool artifact_noop_register_operator(void *user, IdmRuntime *rt, const IdmSyntax *name, int64_t precedence, const char *assoc, const char *capture, const IdmSyntax *target, IdmError *err) {
    (void)user; (void)rt; (void)name; (void)precedence; (void)assoc; (void)capture; (void)target; (void)err;
    return true;
}

static bool artifact_noop_register_macro(void *user, IdmRuntime *rt, const IdmSyntax *name, IdmValue transformer, IdmError *err) {
    (void)user; (void)rt; (void)name; (void)transformer; (void)err;
    return true;
}

static bool put_scope_set(IdmBuffer *out, const IdmScopeSet *set) {
    if (!idm_buf_put_u32(out, (uint32_t)set->count)) return false;
    for (size_t i = 0; i < set->count; i++) {
        if (!idm_buf_put_u32(out, set->items[i])) return false;
    }
    return true;
}

static bool read_scope_set(IdmByteReader *r, IdmScopeSet *set) {
    uint32_t n = idm_rd_u32(r);
    if (!r->ok) return false;
    for (uint32_t i = 0; i < n; i++) {
        IdmScopeId id = idm_rd_u32(r);
        if (!r->ok || !idm_scope_set_add(set, id)) return false;
    }
    return true;
}

static bool artifact_read_str(IdmByteReader *r, char **out, IdmError *err);

static bool put_arity(IdmBuffer *out, IdmArity arity) {
    return idm_buf_put_u32(out, (uint32_t)arity.kind) &&
           idm_buf_put_u32(out, arity.min) &&
           idm_buf_put_u32(out, arity.max) &&
           idm_buf_put_u64(out, arity.mask);
}

static IdmArity read_arity(IdmByteReader *r) {
    IdmArity arity;
    arity.kind = (IdmArityKind)idm_rd_u32(r);
    arity.min = idm_rd_u32(r);
    arity.max = idm_rd_u32(r);
    arity.mask = idm_rd_u64(r);
    return arity;
}

static bool put_method_defs(IdmBuffer *out, const IdmTraitMethodDef *methods, size_t count) {
    if (!idm_buf_put_u32(out, (uint32_t)count)) return false;
    for (size_t i = 0; i < count; i++) {
        const IdmTraitMethodDef *m = &methods[i];
        if (!idm_buf_put_str(out, m->name, strlen(m->name)) || !put_arity(out, m->arity)) return false;
        if (!idm_buf_put_u8(out, m->has_default ? 1u : 0u) || !idm_buf_put_u8(out, m->seen_decl ? 1u : 0u)) return false;
        if (!put_scope_set(out, &m->scopes)) return false;
    }
    return true;
}

static bool read_method_defs(IdmByteReader *r, IdmTraitMethodDef **out_methods, size_t *out_count, IdmError *err) {
    uint32_t count = idm_rd_u32(r);
    if (!r->ok) return false;
    if (count == 0) return true;
    IdmTraitMethodDef *methods = calloc(count, sizeof(*methods));
    if (!methods) return false;
    *out_methods = methods;
    for (uint32_t i = 0; i < count; i++) {
        IdmTraitMethodDef *m = &methods[i];
        *out_count = i + 1u;
        if (!artifact_read_str(r, &m->name, err)) return false;
        m->arity = read_arity(r);
        m->has_default = r->ok && idm_rd_u8(r) != 0;
        m->seen_decl = r->ok && idm_rd_u8(r) != 0;
        if (!r->ok) return false;
        m->exported = true;
        idm_scope_set_init(&m->scopes);
        if (!read_scope_set(r, &m->scopes)) return false;
    }
    return true;
}

static bool put_requirement_defs(IdmBuffer *out, const IdmTraitRequirementDef *requirements, size_t count) {
    if (!idm_buf_put_u32(out, (uint32_t)count)) return false;
    for (size_t i = 0; i < count; i++) {
        if (!idm_buf_put_str(out, requirements[i].name, strlen(requirements[i].name))) return false;
    }
    return true;
}

static bool read_requirement_defs(IdmByteReader *r, IdmTraitRequirementDef **out_requirements, size_t *out_count, IdmError *err) {
    uint32_t count = idm_rd_u32(r);
    if (!r->ok) return false;
    if (count == 0) return true;
    IdmTraitRequirementDef *requirements = calloc(count, sizeof(*requirements));
    if (!requirements) return false;
    *out_requirements = requirements;
    for (uint32_t i = 0; i < count; i++) {
        *out_count = i + 1u;
        if (!artifact_read_str(r, &requirements[i].name, err)) return false;
    }
    return true;
}

static bool put_module_blob(IdmBuffer *out, const IdmBytecodeModule *module, IdmError *err) {
    IdmBuffer blob;
    idm_buf_init(&blob);
    if (!idm_ic_serialize(module, &blob, err)) {
        idm_buf_destroy(&blob);
        return false;
    }
    bool ok = idm_buf_put_u64(out, (uint64_t)blob.len) && idm_buf_append_n(out, blob.data ? blob.data : "", blob.len);
    idm_buf_destroy(&blob);
    return ok ? true : idm_error_oom(err, idm_span_unknown(NULL));
}

static bool read_module_blob(IdmRuntime *rt, IdmByteReader *r, IdmBytecodeModule *module, IdmError *err) {
    uint64_t len = idm_rd_u64(r);
    if (!r->ok || len > r->len - r->pos) return idm_error_set(err, idm_span_unknown(NULL), "truncated artifact module");
    bool ok = idm_ic_deserialize(rt, r->data + r->pos, (size_t)len, module, err);
    r->pos += (size_t)len;
    return ok;
}

bool idm_artifact_serialize(const IdmArtifact *art, IdmBuffer *out, IdmError *err) {
    bool ok = idm_buf_append_n(out, "IDMA", 4u) && idm_buf_put_u32(out, IDM_ARTIFACT_VERSION);
    ok = ok && idm_buf_append_n(out, (const char *)art->src_hash, 32u);
    ok = ok && idm_buf_put_u32(out, (uint32_t)art->dep_count);
    for (size_t i = 0; ok && i < art->dep_count; i++) {
        ok = idm_buf_put_str(out, art->deps[i].path, strlen(art->deps[i].path));
        ok = ok && idm_buf_append_n(out, (const char *)art->deps[i].hash, 32u);
        ok = ok && idm_buf_put_u8(out, art->deps[i].kind);
    }
    ok = ok && idm_buf_put_u32(out, art->scope_base) && idm_buf_put_u32(out, art->scope_end);
    ok = ok && idm_buf_put_u8(out, art->name ? 1u : 0u);
    if (ok && art->name) ok = idm_buf_put_str(out, art->name, strlen(art->name));
    if (!ok) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!idm_buf_put_u32(out, art->init_fn)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!put_module_blob(out, art->module, err)) return false;
    ok = idm_buf_put_u32(out, (uint32_t)art->export_count);
    for (size_t i = 0; ok && i < art->export_count; i++) {
        ok = idm_buf_put_str(out, art->exports[i].name, strlen(art->exports[i].name)) &&
             idm_buf_put_u32(out, art->exports[i].slot) &&
             put_arity(out, art->exports[i].arity);
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)art->global_count);
    for (size_t i = 0; ok && i < art->global_count; i++) {
        ok = idm_buf_put_str(out, art->globals[i].name, strlen(art->globals[i].name)) &&
             idm_buf_put_u32(out, art->globals[i].slot) &&
             put_arity(out, art->globals[i].arity) &&
             put_scope_set(out, &art->globals[i].scopes);
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)art->operator_count);
    for (size_t i = 0; ok && i < art->operator_count; i++) {
        const IdmOperatorDef *op = &art->operators[i];
        ok = idm_buf_put_str(out, op->name, strlen(op->name));
        ok = ok && idm_buf_put_str(out, op->capture ? op->capture : "infix", strlen(op->capture ? op->capture : "infix"));
        ok = ok && idm_buf_put_u8(out, op->precedence) && idm_buf_put_u8(out, (uint8_t)op->assoc);
        ok = ok && idm_buf_put_str(out, op->target_name, strlen(op->target_name));
        ok = ok && put_scope_set(out, &op->scopes);
        ok = ok && idm_buf_put_u8(out, op->target_module ? 1u : 0u);
        if (ok && op->target_module) {
            ok = ok && idm_buf_put_u32(out, op->target_function_index);
            if (ok && !put_module_blob(out, &op->target_module->module, err)) return false;
        }
    }
    if (!ok) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!idm_buf_put_u32(out, (uint32_t)art->macro_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < art->macro_count; i++) {
        if (!idm_buf_put_str(out, art->macros[i].name, strlen(art->macros[i].name)) || !idm_buf_put_u32(out, art->macros[i].function_index)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!put_module_blob(out, &art->macros[i].module->module, err)) return false;
    }
    ok = idm_buf_put_u32(out, (uint32_t)art->type_count);
    for (size_t i = 0; ok && i < art->type_count; i++) {
        const IdmPkgType *t = &art->types[i];
        ok = idm_buf_put_str(out, t->name, strlen(t->name)) && idm_buf_put_str(out, t->identity, strlen(t->identity)) && put_scope_set(out, &t->scopes);
        ok = ok && idm_buf_put_u32(out, (uint32_t)t->field_count);
        for (size_t f = 0; ok && f < t->field_count; f++) ok = idm_buf_put_str(out, t->fields[f], strlen(t->fields[f]));
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)art->trait_count);
    for (size_t i = 0; ok && i < art->trait_count; i++) {
        const IdmPkgTrait *t = &art->traits[i];
        ok = idm_buf_put_str(out, t->name, strlen(t->name)) && idm_buf_put_str(out, t->identity, strlen(t->identity));
        ok = ok && put_requirement_defs(out, t->requirements, t->requirement_count);
        ok = ok && put_method_defs(out, t->methods, t->method_count);
    }
    if (!ok) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!idm_buf_put_u8(out, art->resolver_module ? 1u : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (art->resolver_module) {
        if (!idm_buf_put_u32(out, art->resolver_fn)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!put_module_blob(out, &art->resolver_module->module, err)) return false;
    }
    size_t phase_count = art->phase_env ? art->phase_env->module_count : 0;
    if (!idm_buf_put_u32(out, (uint32_t)phase_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < phase_count; i++) {
        if (!idm_buf_put_u32(out, art->phase_env->module_main_fns[i])) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!put_module_blob(out, art->phase_env->modules[i], err)) return false;
    }
    return true;
}

static bool artifact_read_str(IdmByteReader *r, char **out, IdmError *err) {
    char *s = idm_rd_string(r, NULL);
    if (!s) return idm_error_set(err, idm_span_unknown(NULL), "truncated artifact string");
    *out = s;
    return true;
}

bool idm_artifact_deserialize(IdmRuntime *rt, const unsigned char *data, size_t len, IdmArtifact *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    IdmByteReader r;
    idm_byte_reader_init(&r, data, len);
    if (len < 8u || memcmp(data, "IDMA", 4u) != 0) return idm_error_set(err, idm_span_unknown(NULL), "not an .ic artifact");
    r.pos = 4u;
    uint32_t version = idm_rd_u32(&r);
    if (version != IDM_ARTIFACT_VERSION) return idm_error_set(err, idm_span_unknown(NULL), ".ic artifact version %u unsupported", version);
    if (r.pos + 32u > r.len) return idm_error_set(err, idm_span_unknown(NULL), "truncated artifact hash");
    memcpy(out->src_hash, r.data + r.pos, 32u);
    r.pos += 32u;
    bool ok = true;
    uint32_t dep_count = idm_rd_u32(&r);
    if (!r.ok) ok = false;
    if (ok && dep_count != 0) {
        out->deps = calloc(dep_count, sizeof(*out->deps));
        if (!out->deps) ok = false;
        for (uint32_t i = 0; ok && i < dep_count; i++) {
            out->dep_count = i + 1u;
            ok = artifact_read_str(&r, &out->deps[i].path, err);
            if (ok && r.pos + 32u > r.len) ok = false;
            if (ok) {
                memcpy(out->deps[i].hash, r.data + r.pos, 32u);
                r.pos += 32u;
            }
            out->deps[i].kind = ok ? idm_rd_u8(&r) : 0;
            if (ok && !r.ok) ok = false;
        }
    }
    out->scope_base = ok ? idm_rd_u32(&r) : 0;
    out->scope_end = ok ? idm_rd_u32(&r) : 0;
    uint8_t has_name = ok ? idm_rd_u8(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && has_name) ok = artifact_read_str(&r, &out->name, err);
    out->init_fn = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok) {
        out->module = malloc(sizeof(*out->module));
        if (!out->module) ok = false;
        else {
            idm_bc_init(out->module);
            if (!read_module_blob(rt, &r, out->module, err)) {
                free(out->module);
                out->module = NULL;
                ok = false;
            }
        }
    }
    uint32_t export_count = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && export_count != 0) {
        out->exports = calloc(export_count, sizeof(*out->exports));
        if (!out->exports) ok = false;
        for (uint32_t i = 0; ok && i < export_count; i++) {
            out->export_count = i + 1u;
            ok = artifact_read_str(&r, &out->exports[i].name, err);
            out->exports[i].slot = ok ? idm_rd_u32(&r) : 0;
            out->exports[i].arity = ok ? read_arity(&r) : idm_arity_unknown();
            if (ok && !r.ok) ok = false;
        }
    }
    uint32_t global_count = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && global_count != 0) {
        out->globals = calloc(global_count, sizeof(*out->globals));
        if (!out->globals) ok = false;
        for (uint32_t i = 0; ok && i < global_count; i++) {
            out->global_count = i + 1u;
            ok = artifact_read_str(&r, &out->globals[i].name, err);
            out->globals[i].slot = ok ? idm_rd_u32(&r) : 0;
            out->globals[i].arity = ok ? read_arity(&r) : idm_arity_unknown();
            if (ok && !r.ok) ok = false;
            idm_scope_set_init(&out->globals[i].scopes);
            if (ok) ok = read_scope_set(&r, &out->globals[i].scopes);
        }
    }
    uint32_t operator_count = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && operator_count != 0) {
        out->operators = calloc(operator_count, sizeof(*out->operators));
        if (!out->operators) ok = false;
        for (uint32_t i = 0; ok && i < operator_count; i++) {
            IdmOperatorDef *op = &out->operators[i];
            out->operator_count = i + 1u;
            ok = artifact_read_str(&r, &op->name, err) && artifact_read_str(&r, &op->capture, err);
            op->precedence = ok ? idm_rd_u8(&r) : 0;
            op->assoc = (IdmOpAssoc)(ok ? idm_rd_u8(&r) : 0);
            if (ok && !r.ok) ok = false;
            if (ok) ok = artifact_read_str(&r, &op->target_name, err);
            op->exported = true;
            idm_scope_set_init(&op->scopes);
            if (ok) ok = read_scope_set(&r, &op->scopes);
            uint8_t has_target_module = ok ? idm_rd_u8(&r) : 0;
            if (ok && !r.ok) ok = false;
            if (ok && has_target_module) {
                op->target_function_index = idm_rd_u32(&r);
                if (!r.ok) ok = false;
                if (ok) {
                    op->target_module = idm_module_ref_create(rt);
                    if (!op->target_module) ok = false;
                    else if (!read_module_blob(rt, &r, &op->target_module->module, err)) ok = false;
                }
            }
        }
    }
    uint32_t macro_count = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && macro_count != 0) {
        out->macros = calloc(macro_count, sizeof(*out->macros));
        if (!out->macros) ok = false;
        for (uint32_t i = 0; ok && i < macro_count; i++) {
            out->macro_count = i + 1u;
            ok = artifact_read_str(&r, &out->macros[i].name, err);
            out->macros[i].function_index = ok ? idm_rd_u32(&r) : 0;
            if (ok && !r.ok) ok = false;
            if (ok) {
                out->macros[i].module = idm_module_ref_create(rt);
                if (!out->macros[i].module) ok = false;
                else if (!read_module_blob(rt, &r, &out->macros[i].module->module, err)) ok = false;
            }
        }
    }
    uint32_t type_count = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && type_count != 0) {
        out->types = calloc(type_count, sizeof(*out->types));
        if (!out->types) ok = false;
        for (uint32_t i = 0; ok && i < type_count; i++) {
            IdmPkgType *t = &out->types[i];
            out->type_count = i + 1u;
            idm_scope_set_init(&t->scopes);
            ok = artifact_read_str(&r, &t->name, err) && artifact_read_str(&r, &t->identity, err) && read_scope_set(&r, &t->scopes);
            uint32_t field_count = ok ? idm_rd_u32(&r) : 0;
            if (ok && !r.ok) ok = false;
            if (ok && field_count != 0) {
                t->fields = calloc(field_count, sizeof(*t->fields));
                if (!t->fields) ok = false;
                for (uint32_t f = 0; ok && f < field_count; f++) {
                    t->field_count = f + 1u;
                    ok = artifact_read_str(&r, &t->fields[f], err);
                }
            }
        }
    }
    uint32_t trait_count = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && trait_count != 0) {
        out->traits = calloc(trait_count, sizeof(*out->traits));
        if (!out->traits) ok = false;
        for (uint32_t i = 0; ok && i < trait_count; i++) {
            IdmPkgTrait *t = &out->traits[i];
            out->trait_count = i + 1u;
            ok = artifact_read_str(&r, &t->name, err) && artifact_read_str(&r, &t->identity, err);
            if (ok) ok = read_requirement_defs(&r, &t->requirements, &t->requirement_count, err);
            if (ok) ok = read_method_defs(&r, &t->methods, &t->method_count, err);
        }
    }
    uint8_t has_resolver = ok ? idm_rd_u8(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && has_resolver) {
        out->resolver_fn = idm_rd_u32(&r);
        if (!r.ok) ok = false;
        if (ok) {
            out->resolver_module = idm_module_ref_create(rt);
            if (!out->resolver_module) ok = false;
            else if (!read_module_blob(rt, &r, &out->resolver_module->module, err)) ok = false;
        }
    }
    IdmNamespace *phase_ns = NULL;
    IdmPhaseEnv *phase_env = NULL;
    if (ok) {
        phase_ns = idm_fresh_phase_namespace(rt, err);
        phase_env = phase_ns ? idm_phase_env_create(rt, phase_ns) : NULL;
        if (!phase_env) ok = false;
    }
    uint32_t phase_count = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    for (uint32_t i = 0; ok && i < phase_count; i++) {
        uint32_t main_fn = idm_rd_u32(&r);
        if (!r.ok) { ok = false; break; }
        IdmBytecodeModule *module = malloc(sizeof(*module));
        if (!module) { ok = false; break; }
        idm_bc_init(module);
        if (!read_module_blob(rt, &r, module, err) || !idm_phase_env_add_module(phase_env, module, main_fn, err)) {
            idm_bc_destroy(module);
            free(module);
            ok = false;
            break;
        }
    }
    if (ok) {
        out->phase_env = phase_env;
        for (size_t i = 0; i < out->macro_count; i++) {
            out->macros[i].phase_ns = phase_ns;
            out->macros[i].phase_env = idm_phase_env_retain(phase_env);
        }
        for (size_t i = 0; i < out->operator_count; i++) {
            if (!out->operators[i].target_module) continue;
            out->operators[i].target_phase_ns = phase_ns;
            out->operators[i].target_phase_env = idm_phase_env_retain(phase_env);
        }
        if (out->resolver_module) {
            out->resolver_phase_ns = phase_ns;
            out->resolver_phase_env = idm_phase_env_retain(phase_env);
        }
        if (err && !err->present && r.pos != r.len) ok = idm_error_set(err, idm_span_unknown(NULL), ".ic artifact has trailing data");
    } else {
        idm_phase_env_release(phase_env);
    }
    if (!ok) {
        if (err && !err->present) idm_error_set(err, idm_span_unknown(NULL), "truncated .ic artifact");
        idm_artifact_destroy(out);
        return false;
    }
    return true;
}

static bool artifact_run_phase_inits(IdmRuntime *rt, const IdmArtifact *art, IdmError *err) {
    const IdmPhaseEnv *env = art->phase_env;
    if (!env || env->module_count == 0) return true;
    IdmNamespace *old_main_ns = rt->main_ns;
    int old_trait_phase = rt->trait_phase;
    void *old_op_user = rt->register_operator_user;
    IdmRegisterOperatorFn old_op = rt->register_operator;
    void *old_mac_user = rt->register_macro_user;
    IdmRegisterMacroFn old_mac = rt->register_macro;
    rt->main_ns = env->ns;
    rt->trait_phase = 1;
    rt->register_operator_user = NULL;
    rt->register_operator = artifact_noop_register_operator;
    rt->register_macro_user = NULL;
    rt->register_macro = artifact_noop_register_macro;
    bool ok = true;
    for (size_t i = 0; ok && i < env->module_count; i++) {
        IdmValue ignored = idm_nil();
        ok = idm_vm_run(rt, env->modules[i], env->module_main_fns[i], &ignored, err);
    }
    rt->main_ns = old_main_ns;
    rt->trait_phase = old_trait_phase;
    rt->register_operator_user = old_op_user;
    rt->register_operator = old_op;
    rt->register_macro_user = old_mac_user;
    rt->register_macro = old_mac;
    return ok;
}

bool idm_artifact_cache_disabled(void) {
    const char *v = getenv("IDIOMCACHE");
    return v && strcmp(v, "0") == 0;
}

void idm_artifact_cache_write(const char *path, const IdmArtifact *art) {
    if (idm_artifact_cache_disabled()) return;
    IdmError werr;
    idm_error_init(&werr);
    IdmBuffer blob;
    idm_buf_init(&blob);
    bool ok = idm_artifact_serialize(art, &blob, &werr);
    if (ok) {
        IdmBuffer ishc_path;
        IdmBuffer tmp_path;
        idm_buf_init(&ishc_path);
        idm_buf_init(&tmp_path);
        ok = idm_buf_append(&ishc_path, path) && idm_buf_append(&ishc_path, ".ic");
        ok = ok && idm_buf_append(&tmp_path, ishc_path.data) && idm_buf_appendf(&tmp_path, ".tmp.%ld", (long)getpid());
        if (ok) {
            FILE *f = fopen(tmp_path.data, "wb");
            if (f) {
                ok = fwrite(blob.data, 1u, blob.len, f) == blob.len;
                ok = fclose(f) == 0 && ok;
                if (ok) ok = rename(tmp_path.data, ishc_path.data) == 0;
                if (!ok) remove(tmp_path.data);
            }
        }
        idm_buf_destroy(&ishc_path);
        idm_buf_destroy(&tmp_path);
    }
    idm_buf_destroy(&blob);
    idm_error_clear(&werr);
}

static pthread_mutex_t g_phase_reads_mutex = PTHREAD_MUTEX_INITIALIZER;

static void file_state_observe(const char *path, unsigned char hash[32], uint8_t *kind) {
    char *data = NULL;
    size_t len = 0;
    IdmError rerr;
    idm_error_init(&rerr);
    bool readable = idm_read_file(path, &data, &len, &rerr);
    idm_error_clear(&rerr);
    memset(hash, 0, 32u);
    if (readable) {
        idm_sha256(data ? data : "", len, hash);
        free(data);
        *kind = IDM_DEP_FILE_HASH;
    } else {
        *kind = access(path, F_OK) == 0 ? IDM_DEP_FILE_PRESENT : IDM_DEP_FILE_ABSENT;
    }
}

void idm_phase_io_record(IdmRuntime *rt, const char *path) {
    IdmPhaseReads *reads = rt->phase_reads;
    if (!reads || !rt->trait_phase) return;
    char abs[PATH_MAX];
    if (path[0] != '/') {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)) || snprintf(abs, sizeof(abs), "%s/%s", cwd, path) >= (int)sizeof(abs)) {
            reads->failed = true;
            return;
        }
        path = abs;
    }
    unsigned char hash[32];
    uint8_t kind = 0;
    file_state_observe(path, hash, &kind);
    pthread_mutex_lock(&g_phase_reads_mutex);
    for (size_t i = 0; i < reads->count; i++) {
        if (strcmp(reads->items[i].path, path) == 0) {
            pthread_mutex_unlock(&g_phase_reads_mutex);
            return;
        }
    }
    if (reads->count == reads->cap) {
        size_t cap = reads->cap ? reads->cap * 2u : 8u;
        IdmArtifactDep *grown = realloc(reads->items, cap * sizeof(*grown));
        if (!grown) {
            reads->failed = true;
            pthread_mutex_unlock(&g_phase_reads_mutex);
            return;
        }
        reads->items = grown;
        reads->cap = cap;
    }
    IdmArtifactDep *dep = &reads->items[reads->count];
    dep->path = idm_strdup(path);
    if (!dep->path) {
        reads->failed = true;
        pthread_mutex_unlock(&g_phase_reads_mutex);
        return;
    }
    memcpy(dep->hash, hash, 32u);
    dep->kind = kind;
    reads->count++;
    pthread_mutex_unlock(&g_phase_reads_mutex);
}

void idm_phase_reads_destroy(IdmPhaseReads *reads) {
    for (size_t i = 0; i < reads->count; i++) free(reads->items[i].path);
    free(reads->items);
    memset(reads, 0, sizeof(*reads));
}

bool idm_artifact_dep_verified(IdmRuntime *rt, const IdmArtifactDep *dep) {
    if (dep->kind == IDM_DEP_PACKAGE) return idm_artifact_path_verified(rt, dep->path, dep->hash);
    unsigned char hash[32];
    uint8_t kind = 0;
    file_state_observe(dep->path, hash, &kind);
    if (kind != dep->kind) return false;
    return kind != IDM_DEP_FILE_HASH || memcmp(hash, dep->hash, 32u) == 0;
}

bool idm_artifact_path_verified(IdmRuntime *rt, const char *path, const unsigned char want[32]) {
    IdmBuffer src;
    idm_buf_init(&src);
    const char *label = NULL;
    IdmError rerr;
    idm_error_init(&rerr);
    bool ok = idm_package_read_source(rt, path, &src, &label, idm_span_unknown(NULL), &rerr);
    unsigned char got[32];
    if (ok) idm_sha256(src.data ? src.data : "", src.len, got);
    idm_buf_destroy(&src);
    idm_error_clear(&rerr);
    return ok && memcmp(got, want, 32u) == 0;
}

bool idm_artifact_cache_load(IdmRuntime *rt, const char *path, const unsigned char src_hash[32], IdmArtifact *out) {
    if (idm_artifact_cache_disabled()) return false;
    IdmBuffer ishc_path;
    idm_buf_init(&ishc_path);
    if (!idm_buf_append(&ishc_path, path) || !idm_buf_append(&ishc_path, ".ic")) {
        idm_buf_destroy(&ishc_path);
        return false;
    }
    char *data = NULL;
    size_t len = 0;
    IdmError rerr;
    idm_error_init(&rerr);
    bool ok = idm_read_file(ishc_path.data, &data, &len, &rerr);
    idm_buf_destroy(&ishc_path);
    idm_error_clear(&rerr);
    if (!ok) return false;
    IdmError derr;
    idm_error_init(&derr);
    ok = idm_artifact_deserialize(rt, (const unsigned char *)data, len, out, &derr);
    free(data);
    if (ok) {
        ok = memcmp(out->src_hash, src_hash, 32u) == 0;
        for (size_t i = 0; ok && i < out->dep_count; i++) {
            ok = idm_artifact_dep_verified(rt, &out->deps[i]);
        }
        if (ok) ok = artifact_run_phase_inits(rt, out, &derr);
        if (!ok) idm_artifact_destroy(out);
    }
    idm_error_clear(&derr);
    return ok;
}
