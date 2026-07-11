#define _XOPEN_SOURCE 700

#include "idiom/artifact.h"

#include "idiom/core.h"
#include "idiom/reader.h"
#include "idiom/regex.h"
#include "idiom/vm.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

IdmEnv *idm_fresh_phase_runtime_env(IdmRuntime *rt, IdmError *err) {
    IdmEnv *storage = idm_env_fresh(rt);
    if (!storage) idm_error_oom(err, idm_span_unknown(NULL));
    return storage;
}

IdmPhaseEnv *idm_phase_env_create(IdmRuntime *rt, IdmEnv *storage) {
    IdmPhaseEnv *env = calloc(1u, sizeof(*env));
    if (!env) return NULL;
    env->rt = rt;
    env->env = storage;
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
        if (env->modules[i]) idm_runtime_retire_module(env->rt, env->modules[i]);
    }
    free(env->modules);
    free(env->module_main_fns);
    free(env);
}

bool idm_phase_env_add_module(IdmPhaseEnv *env, IdmBytecodeModule *module, uint32_t main_fn, IdmError *err) {
    if (!env || !module) return false;
    if (!idm_bc_is_finalized(module)) return idm_error_set(err, idm_span_unknown(NULL), "phase environment module is not finalized");
    if (env->module_count == env->module_cap) {
        IdmGrowItem items[] = {
            { .base = (void **)&env->modules, .elem_size = sizeof(*env->modules) },
            { .base = (void **)&env->module_main_fns, .elem_size = sizeof(*env->module_main_fns) },
        };
        if (!idm_growv(items, 2u, &env->module_cap, 4u, env->module_count + 1u)) return false;
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
    if (method->has_contract) idm_callable_contract_destroy(&method->contract);
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
    idm_phase_env_release(macro->phase_env);
    macro->phase_env = NULL;
}

void idm_pkg_core_form_destroy(IdmPkgCoreForm *core_form) {
    if (!core_form) return;
    free(core_form->name);
    idm_module_ref_release(core_form->module);
    idm_phase_env_release(core_form->phase_env);
    memset(core_form, 0, sizeof(*core_form));
}

IdmEnforestNode *idm_enforest_clone(const IdmEnforestNode *node) {
    if (!node) return NULL;
    IdmEnforestNode *copy = calloc(1u, sizeof(*copy));
    if (!copy) return NULL;
    copy->op = node->op;
    copy->mode = node->mode;
    copy->index = node->index;
    if (node->child) {
        copy->child = idm_enforest_clone(node->child);
        if (!copy->child) {
            free(copy);
            return NULL;
        }
    }
    return copy;
}

void idm_enforest_free(IdmEnforestNode *node) {
    while (node) {
        IdmEnforestNode *child = node->child;
        free(node);
        node = child;
    }
}

void idm_pkg_reader_form_destroy(IdmPkgReaderForm *form) {
    if (!form) return;
    free(form->name);
    idm_enforest_free(form->node);
    idm_module_ref_release(form->module);
    idm_phase_env_release(form->phase_env);
    memset(form, 0, sizeof(*form));
}

static bool enforest_node_serialize(IdmBuffer *out, const IdmEnforestNode *node, IdmError *err) {
    if (!idm_buf_put_u8(out, node->op) || !idm_buf_put_u8(out, node->mode) || !idm_buf_put_u32(out, node->index) ||
        !idm_buf_put_u8(out, node->child ? 1u : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
    return node->child ? enforest_node_serialize(out, node->child, err) : true;
}

static bool enforest_node_deserialize(IdmByteReader *r, IdmEnforestNode **out, uint32_t depth, IdmError *err) {
    *out = NULL;
    if (depth > 64u) return idm_error_set(err, idm_span_unknown(NULL), "enforest instruction nesting is too deep");
    IdmEnforestNode *node = calloc(1u, sizeof(*node));
    if (!node) return idm_error_oom(err, idm_span_unknown(NULL));
    node->op = idm_rd_u8(r);
    node->mode = idm_rd_u8(r);
    node->index = idm_rd_u32(r);
    uint8_t has_child = idm_rd_u8(r);
    if (!r->ok || node->op > (uint8_t)IDM_ENFOREST_SYNTAX) {
        free(node);
        return idm_error_set(err, idm_span_unknown(NULL), "reader-form artifact entry is malformed");
    }
    if (has_child && !enforest_node_deserialize(r, &node->child, depth + 1u, err)) {
        free(node);
        return false;
    }
    *out = node;
    return true;
}

void idm_grammar_terminal_destroy(IdmGrammarTerminal *terminal) {
    if (!terminal) return;
    free(terminal->text);
    memset(terminal, 0, sizeof(*terminal));
}

static void idm_reader_inst_destroy(IdmReaderInst *inst) {
    if (!inst) return;
    free(inst->text);
    idm_syn_free(inst->literal);
    memset(inst, 0, sizeof(*inst));
}

void idm_reader_program_destroy(IdmReaderProgram *program) {
    if (!program) return;
    for (size_t i = 0; i < program->count; i++) idm_reader_inst_destroy(&program->items[i]);
    free(program->items);
    memset(program, 0, sizeof(*program));
}

static bool idm_reader_inst_copy(IdmReaderInst *dst, const IdmReaderInst *src, IdmError *err, IdmSpan span) {
    memset(dst, 0, sizeof(*dst));
    dst->op = src->op;
    dst->integer = src->integer;
    dst->child_count = src->child_count;
    dst->capture_slot = src->capture_slot;
    dst->target_kind = IDM_READER_PATTERN_TARGET_NONE;
    dst->target_index = 0;
    dst->literal_index = SIZE_MAX;
    if (src->text) {
        dst->text = idm_strdup(src->text);
        if (!dst->text) {
            idm_reader_inst_destroy(dst);
            return idm_error_oom(err, span);
        }
    }
    dst->has_literal = src->has_literal;
    if (src->has_literal) {
        dst->literal = idm_syn_clone(src->literal);
        if (!dst->literal) {
            idm_reader_inst_destroy(dst);
            return idm_error_oom(err, span);
        }
    }
    return true;
}

bool idm_reader_program_copy(IdmReaderProgram *dst, const IdmReaderProgram *src, IdmError *err, IdmSpan span) {
    memset(dst, 0, sizeof(*dst));
    if (!src || src->count == 0) return true;
    dst->items = calloc(src->count, sizeof(*dst->items));
    if (!dst->items) return idm_error_oom(err, span);
    dst->cap = src->count;
    for (size_t i = 0; i < src->count; i++) {
        if (!idm_reader_inst_copy(&dst->items[i], &src->items[i], err, span)) {
            dst->count = i + 1u;
            idm_reader_program_destroy(dst);
            return false;
        }
        dst->count = i + 1u;
    }
    return true;
}

void idm_reader_program_relocate(IdmReaderProgram *program, IdmScopeId min_id, int64_t delta) {
    (void)min_id;
    (void)delta;
    for (size_t i = 0; program && i < program->count; i++) {
        program->items[i].target_kind = IDM_READER_PATTERN_TARGET_NONE;
        program->items[i].target_index = 0;
    }
}

void idm_grammar_rule_destroy(IdmGrammarRule *rule) {
    for (size_t i = 0; i < rule->param_count; i++) free(rule->params[i]);
    free(rule->params);
    rule->params = NULL;
    rule->param_count = 0;
    if (!rule) return;
    free(rule->name);
    idm_grammar_terminal_destroy(&rule->terminal);
    idm_reader_program_destroy(&rule->pattern);
    idm_reader_program_destroy(&rule->constructor);
    memset(rule, 0, sizeof(*rule));
}

void idm_grammar_pair_destroy(IdmGrammarPair *pair) {
    if (!pair) return;
    free(pair->open);
    free(pair->close);
    memset(pair, 0, sizeof(*pair));
}

bool idm_grammar_pairs_copy(const IdmGrammarPair *src, size_t count, IdmGrammarPair **out) {
    *out = NULL;
    if (count == 0) return true;
    IdmGrammarPair *pairs = calloc(count, sizeof(*pairs));
    if (!pairs) return false;
    for (size_t i = 0; i < count; i++) {
        pairs[i].open = idm_strdup(src[i].open);
        pairs[i].close = idm_strdup(src[i].close);
        if (!pairs[i].open || !pairs[i].close) {
            for (size_t j = 0; j <= i; j++) idm_grammar_pair_destroy(&pairs[j]);
            free(pairs);
            return false;
        }
    }
    *out = pairs;
    return true;
}

void idm_pkg_grammar_destroy(IdmPkgGrammar *grammar) {
    if (!grammar) return;
    free(grammar->name);
    for (size_t i = 0; i < grammar->rule_count; i++) idm_grammar_rule_destroy(&grammar->rules[i]);
    free(grammar->rules);
    for (size_t i = 0; i < grammar->pair_count; i++) idm_grammar_pair_destroy(&grammar->pairs[i]);
    free(grammar->pairs);
    idm_scope_set_destroy(&grammar->scopes);
    memset(grammar, 0, sizeof(*grammar));
}

void idm_pkg_slot_destroy(IdmPkgSlot *slot) {
    if (!slot) return;
    free(slot->name);
    idm_scope_set_destroy(&slot->scopes);
    if (slot->has_contract) idm_callable_contract_destroy(&slot->contract);
    free(slot->const_entries);
    slot->const_entries = NULL;
    slot->const_entry_count = 0;
    slot->name = NULL;
    slot->slot = 0;
    slot->arity = idm_arity_unknown();
    slot->has_contract = false;
}

void idm_pkg_import_destroy(IdmPkgImport *imp) {
    if (!imp) return;
    free(imp->name);
    free(imp->env_key);
    idm_scope_set_destroy(&imp->scopes);
    if (imp->has_contract) idm_callable_contract_destroy(&imp->contract);
    memset(imp, 0, sizeof(*imp));
    imp->arity = idm_arity_unknown();
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

void idm_pkg_method_impl_destroy(IdmPkgMethodImpl *impl) {
    if (!impl) return;
    free(impl->trait);
    free(impl->type);
    free(impl->method);
    free(impl->impl_env_key);
    if (impl->has_contract) idm_callable_contract_destroy(&impl->contract);
    memset(impl, 0, sizeof(*impl));
}

void idm_pkg_type_destroy(IdmPkgType *type) {
    if (!type) return;
    free(type->name);
    free(type->identity);
    idm_scope_set_destroy(&type->scopes);
    for (size_t i = 0; i < type->member_count; i++) idm_type_term_destroy(&type->members[i].term);
    free(type->members);
    for (size_t i = 0; i < type->field_count; i++) {
        free(type->fields[i].name);
        if (type->fields[i].has_contract) idm_type_term_destroy(&type->fields[i].contract);
    }
    free(type->fields);
    memset(type, 0, sizeof(*type));
}

void idm_pkg_protocol_destroy(IdmPkgProtocol *protocol) {
    if (!protocol) return;
    free(protocol->name);
    free(protocol->identity);
    if (protocol->art && !protocol->art->rt_owned) {
        idm_artifact_destroy(protocol->art);
        free(protocol->art);
    }
    memset(protocol, 0, sizeof(*protocol));
}

void idm_pkg_typed_entity_destroy(IdmPkgTypedEntity *entity) {
    if (!entity) return;
    switch (entity->kind) {
        case IDM_TYPED_ENTITY_PROTOCOL:
            idm_pkg_protocol_destroy(&entity->as.protocol);
            break;
        case IDM_TYPED_ENTITY_TRAIT:
            idm_pkg_trait_destroy(&entity->as.trait);
            break;
        case IDM_TYPED_ENTITY_TYPE:
            idm_pkg_type_destroy(&entity->as.type);
            break;
    }
    memset(entity, 0, sizeof(*entity));
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
    free(art->source_reader);
    if (art->slots) {
        for (size_t i = 0; i < art->slot_count; i++) idm_pkg_slot_destroy(&art->slots[i]);
        free(art->slots);
    }
    if (art->imports) {
        for (size_t i = 0; i < art->import_count; i++) idm_pkg_import_destroy(&art->imports[i]);
        free(art->imports);
    }
    if (art->macros) {
        for (size_t i = 0; i < art->macro_count; i++) idm_pkg_macro_destroy(&art->macros[i]);
        free(art->macros);
    }
    if (art->field_selectors) {
        for (size_t i = 0; i < art->field_selector_count; i++) { free(art->field_selectors[i].name); free(art->field_selectors[i].env_key); }
        free(art->field_selectors);
    }
    if (art->operators) {
        for (size_t i = 0; i < art->operator_count; i++) idm_operator_def_destroy(&art->operators[i]);
        free(art->operators);
    }
    if (art->typed.entities) {
        for (size_t i = 0; i < art->typed.entity_count; i++) idm_pkg_typed_entity_destroy(&art->typed.entities[i]);
        free(art->typed.entities);
    }
    if (art->typed.method_impls) {
        for (size_t i = 0; i < art->typed.method_impl_count; i++) idm_pkg_method_impl_destroy(&art->typed.method_impls[i]);
        free(art->typed.method_impls);
    }
    if (art->core_form) {
        for (size_t i = 0; i < art->core_form_count; i++) idm_pkg_core_form_destroy(&art->core_form[i]);
        free(art->core_form);
    }
    if (art->protocol_requires) {
        for (size_t i = 0; i < art->protocol_require_count; i++) free(art->protocol_requires[i]);
        free(art->protocol_requires);
        art->protocol_requires = NULL;
        art->protocol_require_count = 0;
    }
    if (art->reader_forms) {
        for (size_t i = 0; i < art->reader_forms_count; i++) idm_pkg_reader_form_destroy(&art->reader_forms[i]);
        free(art->reader_forms);
    }
    if (art->grammars) {
        for (size_t i = 0; i < art->grammar_count; i++) idm_pkg_grammar_destroy(&art->grammars[i]);
        free(art->grammars);
    }
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
            if (!idm_grow((void **)&names, &cap, sizeof(*names), 8u, count + 1u)) { ok = false; break; }
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
    (void)rt;
    (void)out_src;
    (void)out_label;
    (void)span;
    (void)err;
    return true;
}

static const char *package_canonical(const char *cand, char real[PATH_MAX]) {
    if (realpath(cand, real)) return real;
    return cand;
}

static bool package_units_at(IdmRuntime *rt, const char *resolved_in, IdmPackageUnit **out_units, size_t *out_count, const char **out_label, bool *out_found, IdmSpan span, IdmError *err) {
    *out_found = false;
    char real[PATH_MAX];
    const char *resolved = package_canonical(resolved_in, real);
    DIR *dir = opendir(resolved);
    if (dir) {
        char **names = NULL;
        size_t count = 0;
        size_t cap = 0;
        bool ok = true;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            size_t len = strlen(entry->d_name);
            if (len < 4 || strcmp(entry->d_name + len - 3, ".id") != 0) continue;
            if (count == cap && !idm_grow((void **)&names, &cap, sizeof(*names), 8u, count + 1u)) { ok = false; break; }
            names[count] = idm_strdup(entry->d_name);
            if (!names[count]) { ok = false; break; }
            count++;
        }
        closedir(dir);
        if (!ok) {
            for (size_t i = 0; i < count; i++) free(names[i]);
            free(names);
            return idm_error_oom(err, span);
        }
        if (count == 0) {
            free(names);
            return idm_error_set(err, span, "package directory '%s' contains no .id source files", resolved);
        }
        qsort(names, count, sizeof(*names), package_name_compare);
        IdmPackageUnit *units = calloc(count, sizeof(*units));
        if (!units) {
            for (size_t i = 0; i < count; i++) free(names[i]);
            free(names);
            return idm_error_oom(err, span);
        }
        for (size_t i = 0; i < count && ok; i++) {
            IdmBuffer full;
            idm_buf_init(&full);
            if (!idm_buf_append(&full, resolved) || !idm_buf_append_char(&full, '/') || !idm_buf_append(&full, names[i])) { idm_buf_destroy(&full); ok = false; break; }
            const char *label = idm_symbol_text(idm_intern(&rt->intern, IDM_SYMBOL_WORD, full.data));
            size_t src_len = 0;
            bool read_ok = label != NULL && idm_read_file(full.data, &units[i].source, &src_len, err);
            idm_buf_destroy(&full);
            if (!read_ok) { ok = false; break; }
            units[i].label = label;
        }
        for (size_t i = 0; i < count; i++) free(names[i]);
        free(names);
        if (!ok) {
            idm_package_units_free(units, count);
            return err && err->present ? false : idm_error_oom(err, span);
        }
        const char *pkg_label = idm_symbol_text(idm_intern(&rt->intern, IDM_SYMBOL_WORD, resolved));
        if (!pkg_label) {
            idm_package_units_free(units, count);
            return idm_error_oom(err, span);
        }
        *out_units = units;
        *out_count = count;
        *out_label = pkg_label;
        *out_found = true;
        return true;
    }
    (void)rt;
    (void)out_units;
    (void)out_count;
    (void)out_label;
    (void)span;
    (void)err;
    return true;
}

void idm_package_units_free(IdmPackageUnit *units, size_t count) {
    if (!units) return;
    for (size_t i = 0; i < count; i++) free(units[i].source);
    free(units);
}

static bool package_resolved_take(IdmRuntime *rt, const char *resolved_in, const char **out_resolved, IdmSpan span, IdmError *err) {
    if (!out_resolved) return true;
    char real[PATH_MAX];
    const char *resolved = package_canonical(resolved_in, real);
    const char *interned = idm_symbol_text(idm_intern(&rt->intern, IDM_SYMBOL_WORD, resolved));
    if (!interned) return idm_error_oom(err, span);
    *out_resolved = interned;
    return true;
}

static bool package_root_candidate(const char *path, bool env_pass, IdmBuffer *out, bool *out_have, IdmSpan span, IdmError *err) {
    *out_have = false;
    const char *env = getenv("IDIOMROOT");
    const char *root = env_pass ? env : ((env && env[0]) ? NULL : idm_root());
    if (!root || !root[0]) return true;
    idm_buf_init(out);
    if (!idm_buf_append(out, root) || !idm_buf_append(out, "/") || !idm_buf_append(out, path)) {
        idm_buf_destroy(out);
        return idm_error_oom(err, span);
    }
    *out_have = true;
    return true;
}

static bool package_not_found(const char *path, IdmSpan span, IdmError *err) {
    const char *search = getenv("IDIOMPATH");
    const char *tail = (search && search[0]) ? ", IDIOMPATH" : "";
    const char *root = idm_root();
    if (root) return idm_error_set(err, span, "package '%s' not found (searched IDIOMROOT '%s', cwd%s)", path, root, tail);
    return idm_error_set(err, span, "package '%s' not found (searched cwd%s; IDIOMROOT is unset and no std/kernel sits above this executable)", path, tail);
}

typedef bool (*PackageTryAt)(IdmRuntime *rt, const char *at, void *user, bool *out_found, IdmSpan span, IdmError *err);

static bool package_try_candidate(IdmRuntime *rt, const char *at, PackageTryAt try_at, void *user, const char **out_resolved, bool *out_found, IdmSpan span, IdmError *err) {
    if (!try_at(rt, at, user, out_found, span, err)) return false;
    if (*out_found) return package_resolved_take(rt, at, out_resolved, span, err);
    return true;
}

static bool package_resolve_ladder(IdmRuntime *rt, const char *path, PackageTryAt try_at, void *user, const char **out_resolved, IdmSpan span, IdmError *err) {
    bool found = false;
    IdmBuffer rooted;
    bool have_rooted = false;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            if (!package_try_candidate(rt, path, try_at, user, out_resolved, &found, span, err)) return false;
            if (found) return true;
        }
        if (!package_root_candidate(path, pass == 0, &rooted, &have_rooted, span, err)) return false;
        if (have_rooted) {
            bool ok = package_try_candidate(rt, rooted.data, try_at, user, out_resolved, &found, span, err);
            idm_buf_destroy(&rooted);
            if (!ok) return false;
            if (found) return true;
        }
    }
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
                bool ok = package_try_candidate(rt, cand.data, try_at, user, out_resolved, &found, span, err);
                idm_buf_destroy(&cand);
                if (!ok) return false;
                if (found) return true;
            }
            if (!colon) break;
            cursor = colon + 1u;
        }
    }
    return package_not_found(path, span, err);
}

typedef struct {
    IdmPackageUnit **units;
    size_t *count;
    const char **label;
} PackageUnitsAt;

static bool package_units_try(IdmRuntime *rt, const char *at, void *user, bool *out_found, IdmSpan span, IdmError *err) {
    PackageUnitsAt *u = user;
    return package_units_at(rt, at, u->units, u->count, u->label, out_found, span, err);
}

typedef struct {
    IdmBuffer *src;
    const char **label;
} PackageReadAt;

static bool package_read_try(IdmRuntime *rt, const char *at, void *user, bool *out_found, IdmSpan span, IdmError *err) {
    PackageReadAt *u = user;
    return package_read_at(rt, at, u->src, u->label, out_found, span, err);
}

bool idm_package_read_units(IdmRuntime *rt, const char *path, IdmPackageUnit **out_units, size_t *out_count, const char **out_label, const char **out_resolved, IdmSpan span, IdmError *err) {
    PackageUnitsAt u = { out_units, out_count, out_label };
    return package_resolve_ladder(rt, path, package_units_try, &u, out_resolved, span, err);
}

bool idm_package_read_source(IdmRuntime *rt, const char *path, IdmBuffer *out_src, const char **out_label, const char **out_resolved, IdmSpan span, IdmError *err) {
    PackageReadAt u = { out_src, out_label };
    return package_resolve_ladder(rt, path, package_read_try, &u, out_resolved, span, err);
}

const char *idm_grammar_mode_name(uint8_t mode) {
    switch ((IdmGrammarMode)mode) {
        case IDM_GRAMMAR_MODE_EXTEND: return "extend";
        case IDM_GRAMMAR_MODE_EXCLUSIVE: return "exclusive";
    }
    return "<bad-grammar-mode>";
}

const char *idm_grammar_rule_kind_name(uint8_t kind) {
    switch ((IdmGrammarRuleKind)kind) {
        case IDM_GRAMMAR_RULE_TOKEN: return "token";
        case IDM_GRAMMAR_RULE_FORM: return "form";
        case IDM_GRAMMAR_RULE_SKIP: return "skip";
    }
    return "<bad-grammar-rule-kind>";
}

const char *idm_grammar_terminal_kind_name(uint8_t kind) {
    switch ((IdmGrammarTerminalKind)kind) {
        case IDM_GRAMMAR_TERMINAL_NONE: return "none";
        case IDM_GRAMMAR_TERMINAL_REGEX: return "regex";
        case IDM_GRAMMAR_TERMINAL_LITERAL: return "literal";
        case IDM_GRAMMAR_TERMINAL_STRING: return "string";
        case IDM_GRAMMAR_TERMINAL_BITSTRING: return "bitstring";
    }
    return "<bad-grammar-terminal-kind>";
}

static bool reader_ir_collection(const IdmSyntax *syn) {
    return syn && (syn->kind == IDM_SYN_LIST || syn->kind == IDM_SYN_VECTOR);
}

static bool reader_ir_seq(const IdmSyntax *syn) {
    return syn && (syn->kind == IDM_SYN_LIST || syn->kind == IDM_SYN_VECTOR || syn->kind == IDM_SYN_TUPLE);
}

static bool reader_ir_signature(const IdmSyntax *syn) {
    return syn && syn->kind == IDM_SYN_TUPLE;
}

static const char *reader_ir_tag(const IdmSyntax *syn) {
    return syn && syn->kind == IDM_SYN_ATOM ? syn->as.text : NULL;
}

static bool reader_ir_tag_is(const IdmSyntax *syn, const char *want) {
    const char *tag = reader_ir_tag(syn);
    return tag && strcmp(tag, want) == 0;
}

static const char *reader_ir_text(const IdmSyntax *syn) {
    return syn && (syn->kind == IDM_SYN_WORD || syn->kind == IDM_SYN_ATOM || syn->kind == IDM_SYN_STRING) ? syn->as.text : NULL;
}

static bool reader_ir_name(const IdmSyntax *syn, const char *what, IdmError *err, IdmSpan span) {
    if (syn && syn->kind == IDM_SYN_WORD && syn->as.text && syn->as.text[0] != '\0') return true;
    return idm_error_set(err, syn ? syn->span : span, "%s expects a name", what);
}

static bool reader_ir_regex_flag(const IdmSyntax *syn, uint32_t *flags, IdmError *err, IdmSpan span) {
    const char *tag = reader_ir_tag(syn);
    if (!tag) return idm_error_set(err, syn ? syn->span : span, "regex option must be an atom");
    if (strcmp(tag, "caseless") == 0 || strcmp(tag, "ignore-case") == 0) { *flags |= IDM_REGEX_CASELESS; return true; }
    if (strcmp(tag, "multiline") == 0) { *flags |= IDM_REGEX_MULTILINE; return true; }
    if (strcmp(tag, "dotall") == 0) { *flags |= IDM_REGEX_DOTALL; return true; }
    return idm_error_set(err, syn->span, "unknown regex option '%s'", tag);
}

bool idm_grammar_terminal_from_ir(const IdmSyntax *pattern, IdmGrammarTerminal *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    if (!reader_ir_signature(pattern) || pattern->as.seq.count < 1u) {
        return idm_error_set(err, pattern ? pattern->span : idm_span_unknown(NULL), "token pattern expects {:regex STRING ...}, {:literal STRING}, or {:string}");
    }
    if (reader_ir_tag_is(pattern->as.seq.items[0], "string")) {
        if (pattern->as.seq.count != 1u) return idm_error_set(err, pattern->span, "token string terminal takes no options");
        out->kind = (uint8_t)IDM_GRAMMAR_TERMINAL_STRING;
        out->text = idm_strdup("");
        if (!out->text) return idm_error_oom(err, pattern->span);
        return true;
    }
    if (reader_ir_tag_is(pattern->as.seq.items[0], "bitstring")) {
        if (pattern->as.seq.count != 1u) return idm_error_set(err, pattern->span, "token bitstring terminal takes no options");
        out->kind = (uint8_t)IDM_GRAMMAR_TERMINAL_BITSTRING;
        out->text = idm_strdup("");
        if (!out->text) return idm_error_oom(err, pattern->span);
        return true;
    }
    if (reader_ir_tag_is(pattern->as.seq.items[0], "literal")) {
        if (pattern->as.seq.count != 2u || pattern->as.seq.items[1]->kind != IDM_SYN_STRING) {
            return idm_error_set(err, pattern->span, "token literal expects one string");
        }
        if (!pattern->as.seq.items[1]->as.text || pattern->as.seq.items[1]->as.text[0] == '\0') {
            return idm_error_set(err, pattern->as.seq.items[1]->span, "token literal must not be empty");
        }
        out->kind = (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL;
        out->text = idm_strdup(pattern->as.seq.items[1]->as.text);
        if (!out->text) return idm_error_oom(err, pattern->span);
        return true;
    }
    if (!reader_ir_tag_is(pattern->as.seq.items[0], "regex")) {
        const char *tag = reader_ir_tag(pattern->as.seq.items[0]);
        return idm_error_set(err, pattern->span, "unknown token pattern '%s'", tag ? tag : "<non-atom>");
    }
    if (pattern->as.seq.items[1]->kind != IDM_SYN_STRING) return idm_error_set(err, pattern->as.seq.items[1]->span, "token regex expects a string source");
    uint32_t flags = 0;
    for (size_t i = 2u; i < pattern->as.seq.count; i++) {
        if (!reader_ir_regex_flag(pattern->as.seq.items[i], &flags, err, pattern->span)) return false;
    }
    IdmRegex *rx = NULL;
    IdmError inner;
    idm_error_init(&inner);
    bool ok = idm_regex_compile(pattern->as.seq.items[1]->as.text, strlen(pattern->as.seq.items[1]->as.text), flags, &rx, &inner);
    if (ok && idm_regex_nullable(rx)) {
        ok = idm_error_set(err, pattern->as.seq.items[1]->span, "token regex must not match empty input");
    }
    idm_regex_free(rx);
    if (!ok) {
        const char *msg = inner.present && inner.message ? inner.message : "invalid regex";
        bool reported = err && err->present ? false : idm_error_set(err, pattern->as.seq.items[1]->span, "%s", msg);
        idm_error_clear(&inner);
        return reported;
    }
    idm_error_clear(&inner);
    out->kind = (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX;
    out->flags = flags;
    out->text = idm_strdup(pattern->as.seq.items[1]->as.text);
    if (!out->text) return idm_error_oom(err, pattern->span);
    return true;
}

static bool reader_program_push(IdmReaderProgram *program, IdmReaderInst inst, IdmError *err, IdmSpan span) {
    if (program->count == program->cap) {
        if (!idm_grow((void **)&program->items, &program->cap, sizeof(*program->items), 16u, program->count + 1u)) {
            idm_reader_inst_destroy(&inst);
            return idm_error_oom(err, span);
        }
    }
    program->items[program->count++] = inst;
    return true;
}

static bool reader_pattern_set_text(IdmReaderInst *inst, const IdmSyntax *src, size_t index, const char *what, IdmError *err) {
    const IdmSyntax *name = index < src->as.seq.count ? src->as.seq.items[index] : NULL;
    if (!reader_ir_name(name, what, err, src->span)) return false;
    inst->text = idm_strdup(name->as.text);
    if (!inst->text) return idm_error_oom(err, src->span);
    return true;
}

static bool reader_pattern_compile_into(const IdmSyntax *src, IdmReaderProgram *program, IdmError *err) {
    if (!reader_ir_signature(src) || src->as.seq.count == 0) return idm_error_set(err, src ? src->span : idm_span_unknown(NULL), "form pattern expects an iridium pattern node");
    const char *tag = reader_ir_tag(src->as.seq.items[0]);
    if (!tag) return idm_error_set(err, src->as.seq.items[0]->span, "form pattern head must be an atom");
    IdmReaderInst inst;
    memset(&inst, 0, sizeof(inst));
    if (strcmp(tag, "empty") == 0) {
        if (src->as.seq.count != 1u) return idm_error_set(err, src->span, ":empty takes no children");
        inst.op = IDM_READER_PATTERN_EMPTY;
        return reader_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "ref") == 0 || strcmp(tag, "token") == 0) {
        if (strcmp(tag, "token") == 0 && src->as.seq.count != 2u) return idm_error_set(err, src->span, ":%s expects one name", tag);
        if (src->as.seq.count < 2u) return idm_error_set(err, src->span, ":%s expects one name", tag);
        inst.op = strcmp(tag, "ref") == 0 ? IDM_READER_PATTERN_REF : IDM_READER_PATTERN_TOKEN;
        inst.child_count = src->as.seq.count - 2u;
        if (!reader_pattern_set_text(&inst, src, 1u, "grammar reference", err)) return false;
        if (!reader_program_push(program, inst, err, src->span)) return false;
        for (size_t i = 2u; i < src->as.seq.count; i++) {
            if (!reader_pattern_compile_into(src->as.seq.items[i], program, err)) return false;
        }
        return true;
    }
    if (strcmp(tag, "literal") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":literal expects one syntax value");
        inst.op = IDM_READER_PATTERN_LITERAL;
        inst.has_literal = true;
        inst.literal = idm_syn_clone(src->as.seq.items[1]);
        if (!inst.literal) return idm_error_oom(err, src->as.seq.items[1]->span);
        return reader_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "seq") == 0 || strcmp(tag, "alt") == 0) {
        if (src->as.seq.count < 2u) return idm_error_set(err, src->span, ":%s expects at least one child", tag);
        inst.op = strcmp(tag, "seq") == 0 ? IDM_READER_PATTERN_SEQ : IDM_READER_PATTERN_ALT;
        inst.child_count = src->as.seq.count - 1u;
        if (!reader_program_push(program, inst, err, src->span)) return false;
        for (size_t i = 1u; i < src->as.seq.count; i++) if (!reader_pattern_compile_into(src->as.seq.items[i], program, err)) return false;
        return true;
    }
    if (strcmp(tag, "param") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":param expects a name");
        inst.op = IDM_READER_PATTERN_PARAM;
        inst.child_count = 0u;
        if (!reader_pattern_set_text(&inst, src, 1u, tag, err)) return false;
        return reader_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "repeat") == 0 || strcmp(tag, "optional") == 0 || strcmp(tag, "repeat1") == 0 || strcmp(tag, "not") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":%s expects one child", tag);
        inst.op = strcmp(tag, "repeat") == 0 ? IDM_READER_PATTERN_REPEAT
                : strcmp(tag, "optional") == 0 ? IDM_READER_PATTERN_OPTIONAL
                : strcmp(tag, "repeat1") == 0 ? IDM_READER_PATTERN_REPEAT1
                : IDM_READER_PATTERN_NOT;
        inst.child_count = 1u;
        return reader_program_push(program, inst, err, src->span) && reader_pattern_compile_into(src->as.seq.items[1], program, err);
    }
    if (strcmp(tag, "indent-gt") == 0 || strcmp(tag, "indent-eq") == 0) {
        if (src->as.seq.count != 3u) return idm_error_set(err, src->span, ":%s expects a capture name and one child", tag);
        inst.op = strcmp(tag, "indent-gt") == 0 ? IDM_READER_PATTERN_INDENT_GT : IDM_READER_PATTERN_INDENT_EQ;
        inst.child_count = 1u;
        inst.capture_slot = SIZE_MAX;
        if (!reader_pattern_set_text(&inst, src, 1u, tag, err)) return false;
        return reader_program_push(program, inst, err, src->span) && reader_pattern_compile_into(src->as.seq.items[2], program, err);
    }
    if (strcmp(tag, "adjacent") == 0 || strcmp(tag, "not-adjacent") == 0 || strcmp(tag, "peek") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":%s expects one child", tag);
        inst.op = strcmp(tag, "adjacent") == 0 ? IDM_READER_PATTERN_ADJACENT :
                  strcmp(tag, "not-adjacent") == 0 ? IDM_READER_PATTERN_NOT_ADJACENT :
                  IDM_READER_PATTERN_PEEK;
        inst.child_count = 1u;
        return reader_program_push(program, inst, err, src->span) && reader_pattern_compile_into(src->as.seq.items[1], program, err);
    }
    if (strcmp(tag, "capture") == 0) {
        if (src->as.seq.count != 3u) return idm_error_set(err, src->span, ":capture expects a name and one child");
        inst.op = IDM_READER_PATTERN_CAPTURE;
        inst.child_count = 1u;
        inst.capture_slot = SIZE_MAX;
        if (!reader_pattern_set_text(&inst, src, 1u, "capture", err)) return false;
        return reader_program_push(program, inst, err, src->span) && reader_pattern_compile_into(src->as.seq.items[2], program, err);
    }
    return idm_error_set(err, src->span, "unknown form pattern ':%s'", tag);
}

bool idm_reader_pattern_compile_ir(const IdmSyntax *src, IdmReaderProgram *out, IdmError *err) {
    IdmReaderProgram program;
    memset(&program, 0, sizeof(program));
    if (!reader_pattern_compile_into(src, &program, err)) {
        idm_reader_program_destroy(&program);
        return false;
    }
    *out = program;
    return true;
}

static bool reader_ctor_set_text(IdmReaderInst *inst, const IdmSyntax *src, size_t index, const char *what, IdmError *err) {
    const char *text = index < src->as.seq.count ? reader_ir_text(src->as.seq.items[index]) : NULL;
    if (!text || text[0] == '\0') return idm_error_set(err, index < src->as.seq.count ? src->as.seq.items[index]->span : src->span, "%s expects a name", what);
    inst->text = idm_strdup(text);
    if (!inst->text) return idm_error_oom(err, src->span);
    return true;
}

static bool reader_ctor_compile_into(const IdmSyntax *src, IdmReaderProgram *program, IdmError *err);

static bool reader_ctor_compile_children(const IdmSyntax *args, IdmReaderProgram *program, IdmError *err, IdmSpan span) {
    if (!reader_ir_collection(args)) return idm_error_set(err, args ? args->span : span, "constructor list expects a list or vector");
    for (size_t i = 0; i < args->as.seq.count; i++) if (!reader_ctor_compile_into(args->as.seq.items[i], program, err)) return false;
    return true;
}

static bool reader_ctor_compile_into(const IdmSyntax *src, IdmReaderProgram *program, IdmError *err) {
    if (!reader_ir_signature(src) || src->as.seq.count == 0) return idm_error_set(err, src ? src->span : idm_span_unknown(NULL), "constructor expects an iridium constructor node");
    const char *tag = reader_ir_tag(src->as.seq.items[0]);
    if (!tag) return idm_error_set(err, src->as.seq.items[0]->span, "constructor head must be an atom");
    IdmReaderInst inst;
    memset(&inst, 0, sizeof(inst));
    if (strcmp(tag, "capture") == 0 || strcmp(tag, "splice") == 0 ||
        strcmp(tag, "capture-atom") == 0 || strcmp(tag, "capture-word") == 0 || strcmp(tag, "capture-string") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":%s expects one capture name", tag);
        if (strcmp(tag, "capture") == 0) inst.op = IDM_READER_CTOR_CAPTURE;
        else if (strcmp(tag, "splice") == 0) inst.op = IDM_READER_CTOR_SPLICE;
        else {
            inst.op = IDM_READER_CTOR_CAPTURE_TEXT;
            if (strcmp(tag, "capture-atom") == 0) inst.integer = IDM_SYN_ATOM;
            else if (strcmp(tag, "capture-word") == 0) inst.integer = IDM_SYN_WORD;
            else inst.integer = IDM_SYN_STRING;
        }
        inst.capture_slot = SIZE_MAX;
        if (!reader_ir_name(src->as.seq.items[1], tag, err, src->span) || !reader_ctor_set_text(&inst, src, 1u, tag, err)) return false;
        return reader_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "literal") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":literal expects one syntax value");
        inst.op = IDM_READER_CTOR_LITERAL;
        inst.has_literal = true;
        inst.literal = idm_syn_clone(src->as.seq.items[1]);
        if (!inst.literal) return idm_error_oom(err, src->as.seq.items[1]->span);
        return reader_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "interp-string") == 0 || strcmp(tag, "bitstring") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":%s expects one capture name", tag);
        inst.op = strcmp(tag, "interp-string") == 0 ? IDM_READER_CTOR_INTERP_STRING : IDM_READER_CTOR_BITSTRING;
        inst.capture_slot = SIZE_MAX;
        if (!reader_ir_name(src->as.seq.items[1], tag, err, src->span) || !reader_ctor_set_text(&inst, src, 1u, tag, err)) return false;
        return reader_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "emit-atom") == 0 || strcmp(tag, "emit-word") == 0 || strcmp(tag, "emit-string") == 0 || strcmp(tag, "emit-int") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":%s expects one value", tag);
        if (strcmp(tag, "emit-int") == 0) {
            if (src->as.seq.items[1]->kind != IDM_SYN_INT) return idm_error_set(err, src->as.seq.items[1]->span, ":emit-int expects an integer within machine range");
            inst.op = IDM_READER_CTOR_EMIT_INT;
            inst.integer = src->as.seq.items[1]->as.integer;
        } else {
            inst.op = IDM_READER_CTOR_EMIT_TEXT;
            if (strcmp(tag, "emit-atom") == 0) inst.integer = IDM_SYN_ATOM;
            else if (strcmp(tag, "emit-word") == 0) inst.integer = IDM_SYN_WORD;
            else inst.integer = IDM_SYN_STRING;
            if (!reader_ctor_set_text(&inst, src, 1u, tag, err)) return false;
        }
        return reader_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "emit-form") == 0) {
        if (src->as.seq.count != 3u) return idm_error_set(err, src->span, ":emit-form expects a head and a constructor list");
        const IdmSyntax *args = src->as.seq.items[2];
        if (!reader_ir_collection(args)) return idm_error_set(err, args ? args->span : src->span, "constructor list expects a list or vector");
        inst.op = IDM_READER_CTOR_FORM;
        inst.child_count = args->as.seq.count;
        if (!reader_ctor_set_text(&inst, src, 1u, ":emit-form head", err)) return false;
        return reader_program_push(program, inst, err, src->span) && reader_ctor_compile_children(args, program, err, src->span);
    }
    if (strcmp(tag, "emit-list") == 0 || strcmp(tag, "emit-vector") == 0 || strcmp(tag, "emit-tuple") == 0 || strcmp(tag, "emit-dict") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":%s expects a constructor list", tag);
        const IdmSyntax *args = src->as.seq.items[1];
        if (!reader_ir_collection(args)) return idm_error_set(err, args ? args->span : src->span, "constructor list expects a list or vector");
        inst.op = IDM_READER_CTOR_SEQ;
        if (strcmp(tag, "emit-list") == 0) inst.integer = IDM_SYN_LIST;
        else if (strcmp(tag, "emit-vector") == 0) inst.integer = IDM_SYN_VECTOR;
        else if (strcmp(tag, "emit-tuple") == 0) inst.integer = IDM_SYN_TUPLE;
        else inst.integer = IDM_SYN_DICT;
        inst.child_count = args->as.seq.count;
        return reader_program_push(program, inst, err, src->span) && reader_ctor_compile_children(args, program, err, src->span);
    }
    return idm_error_set(err, src->span, "unknown constructor ':%s'", tag);
}

bool idm_reader_ctor_compile_ir(const IdmSyntax *src, IdmReaderProgram *out, IdmError *err) {
    IdmReaderProgram program;
    memset(&program, 0, sizeof(program));
    if (!reader_ctor_compile_into(src, &program, err)) {
        idm_reader_program_destroy(&program);
        return false;
    }
    *out = program;
    return true;
}

static bool core_grammar_rule_from_ir(const IdmSyntax *rule, IdmGrammarRule *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    if (!reader_ir_signature(rule) || (rule->as.seq.count != 4u && rule->as.seq.count != 3u)) {
        return idm_error_set(err, rule ? rule->span : idm_span_unknown(NULL), "core-grammar rule expects {:skip NAME PATTERN}, {:token NAME PATTERN CONSTRUCTOR}, or {:form NAME PATTERN CONSTRUCTOR}");
    }
    if (reader_ir_tag_is(rule->as.seq.items[0], "token")) {
        if (rule->as.seq.count != 4u) return idm_error_set(err, rule->span, "core-grammar token rule expects {:token NAME PATTERN CONSTRUCTOR}");
        out->kind = (uint8_t)IDM_GRAMMAR_RULE_TOKEN;
        if (!idm_grammar_terminal_from_ir(rule->as.seq.items[2], &out->terminal, err)) return false;
    } else if (reader_ir_tag_is(rule->as.seq.items[0], "form")) {
        if (rule->as.seq.count != 4u) return idm_error_set(err, rule->span, "core-grammar form rule expects {:form NAME PATTERN CONSTRUCTOR}");
        out->kind = (uint8_t)IDM_GRAMMAR_RULE_FORM;
        const IdmSyntax *name_syn = rule->as.seq.items[1];
        if (name_syn->kind == IDM_SYN_LIST && name_syn->as.seq.count >= 2u) {
            size_t pc = name_syn->as.seq.count - 1u;
            out->params = calloc(pc, sizeof(*out->params));
            if (!out->params) return idm_error_oom(err, rule->span);
            for (size_t pi = 0; pi < pc; pi++) {
                const IdmSyntax *pw = name_syn->as.seq.items[pi + 1u];
                if (pw->kind != IDM_SYN_WORD) return idm_error_set(err, pw->span, "core-grammar rule parameter must be a word");
                out->params[pi] = idm_strdup(pw->as.text);
                if (!out->params[pi]) return idm_error_oom(err, rule->span);
                out->param_count = pi + 1u;
            }
        }
        if (!idm_reader_pattern_compile_ir(rule->as.seq.items[2], &out->pattern, err)) return false;
    } else if (reader_ir_tag_is(rule->as.seq.items[0], "skip")) {
        if (rule->as.seq.count != 3u) return idm_error_set(err, rule->span, "core-grammar skip rule expects {:skip NAME PATTERN}");
        out->kind = (uint8_t)IDM_GRAMMAR_RULE_SKIP;
        if (!idm_grammar_terminal_from_ir(rule->as.seq.items[2], &out->terminal, err)) return false;
    } else {
        const char *tag = reader_ir_tag(rule->as.seq.items[0]);
        return idm_error_set(err, rule->as.seq.items[0]->span, "core-grammar rule kind must be :skip, :token, or :form, got '%s'", tag ? tag : "<non-atom>");
    }
    const IdmSyntax *rule_name = rule->as.seq.items[1];
    if (rule_name->kind == IDM_SYN_LIST && rule_name->as.seq.count >= 2u) rule_name = rule_name->as.seq.items[0];
    if (!reader_ir_name(rule_name, "core-grammar rule", err, rule->span)) {
        idm_grammar_rule_destroy(out);
        return false;
    }
    out->name = idm_strdup(rule_name->as.text);
    if (!out->name) {
        idm_grammar_rule_destroy(out);
        return idm_error_oom(err, rule->span);
    }
    if (out->kind != (uint8_t)IDM_GRAMMAR_RULE_SKIP && !idm_reader_ctor_compile_ir(rule->as.seq.items[3], &out->constructor, err)) {
        idm_grammar_rule_destroy(out);
        return false;
    }
    return true;
}

bool idm_pkg_grammar_from_ir(const IdmSyntax *form, IdmPkgGrammar *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    if (!reader_ir_seq(form) || form->as.seq.count != 4u || !form->as.seq.items[0] ||
        form->as.seq.items[0]->kind != IDM_SYN_WORD || strcmp(form->as.seq.items[0]->as.text, "core-grammar") != 0) {
        return idm_error_set(err, form ? form->span : idm_span_unknown(NULL), "core-grammar expects: core-grammar NAME :extend|:exclusive [RULE ...]");
    }
    const IdmSyntax *name = form->as.seq.items[1];
    const IdmSyntax *mode_syntax = form->as.seq.items[2];
    const IdmSyntax *rules_syntax = form->as.seq.items[3];
    if (!reader_ir_name(name, "core-grammar", err, form->span)) return false;
    if (reader_ir_tag_is(mode_syntax, "extend")) out->mode = (uint8_t)IDM_GRAMMAR_MODE_EXTEND;
    else if (reader_ir_tag_is(mode_syntax, "exclusive")) out->mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    else return idm_error_set(err, mode_syntax ? mode_syntax->span : form->span, "core-grammar mode must be :extend or :exclusive");
    if (!reader_ir_collection(rules_syntax) || rules_syntax->as.seq.count == 0) {
        return idm_error_set(err, rules_syntax ? rules_syntax->span : form->span, "core-grammar requires a non-empty rule list or vector");
    }
    out->name = idm_strdup(name->as.text);
    out->rules = calloc(rules_syntax->as.seq.count, sizeof(*out->rules));
    out->pairs = calloc(rules_syntax->as.seq.count, sizeof(*out->pairs));
    out->exported = true;
    if (!out->name || !out->rules || !out->pairs) {
        idm_pkg_grammar_destroy(out);
        return idm_error_oom(err, form->span);
    }
    for (size_t i = 0; i < rules_syntax->as.seq.count; i++) {
        const IdmSyntax *entry = rules_syntax->as.seq.items[i];
        if (reader_ir_signature(entry) && entry->as.seq.count == 3u && reader_ir_tag_is(entry->as.seq.items[0], "pair")) {
            if (!reader_ir_name(entry->as.seq.items[1], "core-grammar pair", err, entry->span) ||
                !reader_ir_name(entry->as.seq.items[2], "core-grammar pair", err, entry->span)) {
                idm_pkg_grammar_destroy(out);
                return false;
            }
            IdmGrammarPair *pair = &out->pairs[out->pair_count];
            pair->open = idm_strdup(entry->as.seq.items[1]->as.text);
            pair->close = idm_strdup(entry->as.seq.items[2]->as.text);
            if (!pair->open || !pair->close) {
                idm_pkg_grammar_destroy(out);
                return idm_error_oom(err, entry->span);
            }
            out->pair_count++;
            continue;
        }
        if (!core_grammar_rule_from_ir(entry, &out->rules[out->rule_count], err)) {
            idm_pkg_grammar_destroy(out);
            return false;
        }
        out->rule_count++;
    }
    if (out->rule_count == 0) {
        idm_pkg_grammar_destroy(out);
        return idm_error_set(err, rules_syntax->span, "core-grammar requires at least one rule");
    }
    return true;
}

bool idm_pkg_source_reader_from_ir(const IdmSyntax *form, char **out, IdmError *err) {
    *out = NULL;
    if (!reader_ir_seq(form) || form->as.seq.count != 3u || !form->as.seq.items[0] ||
        form->as.seq.items[0]->kind != IDM_SYN_WORD || strcmp(form->as.seq.items[0]->as.text, "core-reader") != 0) {
        return idm_error_set(err, form ? form->span : idm_span_unknown(NULL), "core-reader expects: core-reader :source NAME");
    }
    if (!reader_ir_tag_is(form->as.seq.items[1], "source") || !reader_ir_name(form->as.seq.items[2], "core-reader", err, form->span)) {
        return idm_error_set(err, form->span, "core-reader expects: core-reader :source NAME");
    }
    *out = idm_strdup(form->as.seq.items[2]->as.text);
    if (!*out) return idm_error_oom(err, form->span);
    return true;
}

typedef struct {
    size_t min_child_count;
    size_t max_child_count;
    bool text;
    bool literal;
    const char *message;
} ReaderProgramSpec;

#define IDM_READER_OP_SPEC_ROW(op, min_children, max_children, text, literal, message) { min_children, max_children, text, literal, message },

static const ReaderProgramSpec READER_PATTERN_SPECS[IDM_READER_PATTERN_OP_COUNT] = {
    IDM_READER_PATTERN_OP_LIST(IDM_READER_OP_SPEC_ROW)
};

static const ReaderProgramSpec READER_CTOR_SPECS[IDM_READER_CTOR_OP_COUNT] = {
    IDM_READER_CTOR_OP_LIST(IDM_READER_OP_SPEC_ROW)
};

#undef IDM_READER_OP_SPEC_ROW

static const ReaderProgramSpec *reader_program_spec(IdmReaderProgramKind kind, uint8_t op) {
    if (kind == IDM_READER_PROGRAM_PATTERN) return op < IDM_READER_PATTERN_OP_COUNT ? &READER_PATTERN_SPECS[op] : NULL;
    return op < IDM_READER_CTOR_OP_COUNT ? &READER_CTOR_SPECS[op] : NULL;
}

static bool reader_program_validate_at(const IdmReaderProgram *program, IdmReaderProgramKind kind, size_t pc, size_t *out_next, IdmError *err, IdmSpan span) {
    const char *label = kind == IDM_READER_PROGRAM_PATTERN ? "pattern" : "constructor";
    if (!program || pc >= program->count) return idm_error_set(err, span, "reader %s program ended early", label);
    const IdmReaderInst *inst = &program->items[pc];
    const ReaderProgramSpec *spec = reader_program_spec(kind, inst->op);
    if (!spec) return idm_error_set(err, span, "unknown reader %s opcode %u", label, inst->op);
    bool has_text = inst->text && inst->text[0] != '\0';
    bool bad_child_count = inst->child_count < spec->min_child_count || inst->child_count > spec->max_child_count;
    bool bad_text = has_text != spec->text;
    bool bad_literal = inst->has_literal != spec->literal;
    if (bad_child_count || bad_text || bad_literal) return idm_error_set(err, span, "%s", spec->message);
    size_t next = pc + 1u;
    for (size_t i = 0; i < inst->child_count; i++) if (!reader_program_validate_at(program, kind, next, &next, err, span)) return false;
    *out_next = next;
    return true;
}

static bool idm_reader_program_validate(const IdmReaderProgram *program, IdmReaderProgramKind kind, IdmError *err, IdmSpan span) {
    const char *label = kind == IDM_READER_PROGRAM_PATTERN ? "pattern" : "constructor";
    if (!program || program->count == 0 || !program->items) return idm_error_set(err, span, "reader %s program is empty", label);
    size_t next = 0;
    if (!reader_program_validate_at(program, kind, 0, &next, err, span)) return false;
    if (next != program->count) return idm_error_set(err, span, "reader %s program has trailing instructions", label);
    return true;
}

static bool grammar_terminal_validate(const IdmGrammarRule *rule, IdmError *err, IdmSpan span) {
    if (rule->terminal.kind == IDM_GRAMMAR_TERMINAL_LITERAL) {
        if (!rule->terminal.text || rule->terminal.flags != 0u) return idm_error_set(err, span, "grammar literal terminal descriptor is incomplete");
        if (rule->terminal.text[0] == '\0') return idm_error_set(err, span, "grammar literal terminal '%s' must not be empty", rule->name ? rule->name : "<bad>");
        return true;
    }
    if (rule->terminal.kind == IDM_GRAMMAR_TERMINAL_STRING) {
        if (!rule->terminal.text || rule->terminal.text[0] != '\0' || rule->terminal.flags != 0u) return idm_error_set(err, span, "grammar string terminal descriptor is invalid");
        return true;
    }
    if (rule->terminal.kind == IDM_GRAMMAR_TERMINAL_BITSTRING) {
        if (!rule->terminal.text || rule->terminal.text[0] != '\0' || rule->terminal.flags != 0u) return idm_error_set(err, span, "grammar bitstring terminal descriptor is invalid");
        return true;
    }
    if (rule->terminal.kind != IDM_GRAMMAR_TERMINAL_REGEX) return idm_error_set(err, span, "invalid grammar terminal kind");
    if (!rule->terminal.text) return idm_error_set(err, span, "grammar regex terminal descriptor is incomplete");
    IdmRegex *rx = NULL;
    bool ok = idm_regex_compile(rule->terminal.text, strlen(rule->terminal.text), rule->terminal.flags, &rx, err);
    if (ok && rx) {
        if (idm_regex_nullable(rx)) {
            ok = idm_error_set(err, span, "grammar regex terminal '%s' must not match empty input", rule->name ? rule->name : "<bad>");
        }
        for (size_t i = 1u; i <= idm_regex_group_count(rx); i++) {
            const char *group = idm_regex_group_name(rx, i);
            if (group && strcmp(group, rule->name) == 0) {
                ok = idm_error_set(err, span, "grammar terminal '%s' capture group conflicts with rule name", rule->name);
                break;
            }
        }
    }
    idm_regex_free(rx);
    return ok;
}

bool idm_grammar_rule_validate(const IdmGrammarRule *rule, IdmError *err, IdmSpan span) {
    if (!rule || !rule->name || !*rule->name) return idm_error_set(err, span, "grammar rule artifact entry is incomplete");
    if (rule->kind > (uint8_t)IDM_GRAMMAR_RULE_SKIP) return idm_error_set(err, span, "invalid grammar rule kind");
    if (rule->terminal.kind > (uint8_t)IDM_GRAMMAR_TERMINAL_BITSTRING) return idm_error_set(err, span, "invalid grammar terminal kind");
    if (rule->kind == IDM_GRAMMAR_RULE_TOKEN || rule->kind == IDM_GRAMMAR_RULE_SKIP) {
        if (rule->terminal.kind == IDM_GRAMMAR_TERMINAL_NONE) return idm_error_set(err, span, "grammar terminal rule artifact is missing terminal descriptor");
        if (rule->pattern.count != 0) return idm_error_set(err, span, "grammar terminal rule artifact has reduction program");
        if (!grammar_terminal_validate(rule, err, span)) return false;
    } else {
        if (rule->terminal.kind != IDM_GRAMMAR_TERMINAL_NONE) return idm_error_set(err, span, "grammar form rule artifact has terminal descriptor");
        if (!idm_reader_program_validate(&rule->pattern, IDM_READER_PROGRAM_PATTERN, err, span)) return false;
    }
    if (rule->kind == (uint8_t)IDM_GRAMMAR_RULE_SKIP) {
        if (rule->constructor.count != 0) return idm_error_set(err, span, "grammar skip rule artifact has constructor");
        return true;
    }
    return idm_reader_program_validate(&rule->constructor, IDM_READER_PROGRAM_CTOR, err, span);
}

static bool grammar_artifact_validate(const IdmPkgGrammar *grammar, IdmError *err, IdmSpan span) {
    if (!grammar || !grammar->name || !*grammar->name) return idm_error_set(err, span, "grammar artifact entry is incomplete");
    if (grammar->mode > (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE) return idm_error_set(err, span, "invalid grammar mode in artifact");
    if (grammar->rule_count == 0 || !grammar->rules) return idm_error_set(err, span, "grammar artifact entry has no rules");
    for (size_t i = 0; i < grammar->rule_count; i++) {
        if (!idm_grammar_rule_validate(&grammar->rules[i], err, span)) return false;
        for (size_t j = 0; j < i; j++) {
            if (strcmp(grammar->rules[i].name, grammar->rules[j].name) == 0) {
                return idm_error_set(err, span, "grammar artifact '%s' declares rule '%s' more than once", grammar->name, grammar->rules[i].name);
            }
        }
    }
    for (size_t i = 0; i < grammar->pair_count; i++) {
        const IdmGrammarPair *pair = &grammar->pairs[i];
        if (!pair->open || !*pair->open || !pair->close || !*pair->close || strcmp(pair->open, pair->close) == 0) {
            return idm_error_set(err, span, "grammar artifact '%s' pair must name two distinct tokens", grammar->name);
        }
    }
    return true;
}

static bool grammar_artifacts_validate(const IdmPkgGrammar *grammars, size_t grammar_count, IdmError *err, IdmSpan span) {
    if (grammar_count != 0 && !grammars) return idm_error_set(err, span, "grammar artifact table is incomplete");
    for (size_t i = 0; i < grammar_count; i++) {
        if (!grammar_artifact_validate(&grammars[i], err, span)) return false;
        for (size_t j = 0; j < i; j++) {
            if (strcmp(grammars[i].name, grammars[j].name) == 0) {
                return idm_error_set(err, span, "grammar artifact declares surface '%s' more than once", grammars[i].name);
            }
        }
    }
    return true;
}

static bool source_reader_artifact_validate(const char *source_reader, const IdmPkgGrammar *grammars, size_t grammar_count, IdmError *err, IdmSpan span) {
    if (!source_reader) return true;
    if (!*source_reader) return idm_error_set(err, span, "core-reader source reader name is empty");
    for (size_t i = 0; i < grammar_count; i++) {
        if (grammars[i].name && strcmp(grammars[i].name, source_reader) == 0) {
            IdmReaderArtifact *reader = NULL;
            bool ok = idm_reader_artifact_from_grammars(source_reader, grammars, grammar_count, &reader, err);
            idm_reader_artifact_destroy(reader);
            return ok;
        }
    }
    return idm_error_set(err, span, "core-reader source reader '%s' has no core-grammar declaration", source_reader);
}

static bool artifact_read_str(IdmByteReader *r, char **out, IdmError *err);

static bool put_method_defs(IdmBuffer *out, const IdmTraitMethodDef *methods, size_t count) {
    if (!idm_buf_put_u32(out, (uint32_t)count)) return false;
    for (size_t i = 0; i < count; i++) {
        const IdmTraitMethodDef *m = &methods[i];
        if (!idm_buf_put_str(out, m->name, strlen(m->name)) || !idm_arity_serialize(out, m->arity, NULL)) return false;
        if (!idm_buf_put_u8(out, m->has_contract ? 1u : 0u)) return false;
        if (m->has_contract && !idm_callable_contract_serialize(out, &m->contract, NULL)) return false;
        if (!idm_buf_put_u8(out, m->has_default ? 1u : 0u) ||
            !idm_buf_put_u8(out, m->seen_decl ? 1u : 0u) ||
            !idm_buf_put_u8(out, m->has_default_slot ? 1u : 0u) ||
            !idm_buf_put_u32(out, m->default_slot) ||
            !idm_buf_put_u8(out, m->has_dispatch ? 1u : 0u) ||
            !idm_buf_put_u32(out, m->dispatch_slot)) return false;
        if (!idm_scope_set_serialize(out, &m->scopes, NULL)) return false;
    }
    return true;
}

static bool read_method_defs(IdmRuntime *rt, IdmByteReader *r, IdmTraitMethodDef **out_methods, size_t *out_count, IdmError *err) {
    uint32_t count = idm_rd_u32(r);
    if (!r->ok || count > (r->len - r->pos) / 4u) return false;
    if (count == 0) return true;
    IdmTraitMethodDef *methods = calloc(count, sizeof(*methods));
    if (!methods) return idm_error_oom(err, idm_span_unknown(NULL));
    *out_methods = methods;
    for (uint32_t i = 0; i < count; i++) {
        IdmTraitMethodDef *m = &methods[i];
        *out_count = i + 1u;
        if (!artifact_read_str(r, &m->name, err)) return false;
        if (!idm_arity_deserialize(r, &m->arity, err)) return false;
        m->has_contract = r->ok && idm_rd_u8(r) != 0;
        if (m->has_contract && !idm_callable_contract_deserialize(rt, r, &m->contract, err)) return false;
        if (m->has_contract && m->contract.passthrough && !idm_primitive_info((IdmPrimitive)m->contract.primitive)) {
            r->ok = false;
            return idm_error_set(err, idm_span_unknown(NULL), "trait method primitive %u out of bounds", m->contract.primitive);
        }
        m->has_default = r->ok && idm_rd_u8(r) != 0;
        m->seen_decl = r->ok && idm_rd_u8(r) != 0;
        m->has_default_slot = r->ok && idm_rd_u8(r) != 0;
        m->default_slot = r->ok ? idm_rd_u32(r) : 0;
        m->has_dispatch = r->ok && idm_rd_u8(r) != 0;
        m->dispatch_slot = r->ok ? idm_rd_u32(r) : 0;
        if (!r->ok) return false;
        m->exported = true;
        idm_scope_set_init(&m->scopes);
        if (!idm_scope_set_deserialize(r, &m->scopes, NULL)) return false;
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
    if (!r->ok || count > (r->len - r->pos) / 4u) return false;
    if (count == 0) return true;
    IdmTraitRequirementDef *requirements = calloc(count, sizeof(*requirements));
    if (!requirements) return idm_error_oom(err, idm_span_unknown(NULL));
    *out_requirements = requirements;
    for (uint32_t i = 0; i < count; i++) {
        *out_count = i + 1u;
        if (!artifact_read_str(r, &requirements[i].name, err)) return false;
    }
    return true;
}

static size_t artifact_typed_entity_count(const IdmArtifactTypedRegistry *typed, IdmTypedEntityKind kind) {
    size_t count = 0;
    for (size_t i = 0; i < typed->entity_count; i++) {
        if (typed->entities[i].kind == kind) count++;
    }
    return count;
}

static IdmPkgTypedEntity *artifact_typed_append_entity(IdmArtifactTypedRegistry *typed, size_t *cap, IdmTypedEntityKind kind, IdmError *err) {
    if (typed->entity_count == *cap) {
        if (!idm_grow((void **)&typed->entities, cap, sizeof(*typed->entities), 8u, typed->entity_count + 1u)) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return NULL;
        }
    }
    IdmPkgTypedEntity *entity = &typed->entities[typed->entity_count++];
    memset(entity, 0, sizeof(*entity));
    entity->kind = kind;
    return entity;
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

static bool idm_reader_program_serialize(IdmBuffer *out, const IdmReaderProgram *program, IdmReaderProgramKind kind, IdmError *err) {
    if (!idm_buf_put_u32(out, (uint32_t)program->count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < program->count; i++) {
        const IdmReaderInst *inst = &program->items[i];
        if (!idm_buf_put_u8(out, inst->op) ||
            !idm_buf_put_u32(out, (uint32_t)inst->child_count) ||
            !idm_buf_put_u64(out, (uint64_t)inst->capture_slot)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (kind == IDM_READER_PROGRAM_CTOR && !idm_buf_put_u64(out, (uint64_t)inst->integer)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!idm_buf_put_opt_str(out, inst->text)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!idm_buf_put_u8(out, inst->has_literal ? 1u : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (inst->has_literal && !idm_syn_serialize(out, inst->literal, err)) return false;
    }
    return true;
}

static bool idm_reader_program_deserialize(IdmRuntime *rt, IdmByteReader *r, IdmReaderProgram *program, IdmReaderProgramKind kind, IdmError *err) {
    uint32_t count = idm_rd_u32(r);
    if (!r->ok || count > (r->len - r->pos) / 4u) return false;
    if (count == 0) return true;
    program->items = calloc(count, sizeof(*program->items));
    if (!program->items) return idm_error_oom(err, idm_span_unknown(NULL));
    program->cap = count;
    for (uint32_t i = 0; i < count; i++) {
        IdmReaderInst *inst = &program->items[i];
        program->count = i + 1u;
        inst->op = idm_rd_u8(r);
        inst->child_count = idm_rd_u32(r);
        inst->capture_slot = (size_t)idm_rd_u64(r);
        inst->literal_index = SIZE_MAX;
        if (kind == IDM_READER_PROGRAM_CTOR) inst->integer = (int64_t)idm_rd_u64(r);
        if (!idm_rd_opt_str(r, &inst->text, err)) return false;
        uint8_t has_literal = idm_rd_u8(r);
        if (!r->ok) return false;
        if (has_literal) {
            inst->has_literal = true;
            inst->literal = idm_syn_deserialize(rt, r, err);
            if (!inst->literal) return false;
        }
    }
    return true;
}

static int artifact_dep_order_compare(const void *a, const void *b) {
    const IdmArtifactDep *left = *(const IdmArtifactDep *const *)a;
    const IdmArtifactDep *right = *(const IdmArtifactDep *const *)b;
    int path = strcmp(left->path, right->path);
    if (path != 0) return path;
    if (left->kind != right->kind) return left->kind < right->kind ? -1 : 1;
    return memcmp(left->hash, right->hash, 32u);
}

bool idm_artifact_compute_action_hash(IdmArtifact *art, IdmError *err) {
    if (art->dep_count > UINT32_MAX) return idm_error_set(err, idm_span_unknown(NULL), "artifact has too many dependencies");
    const IdmArtifactDep **order = art->dep_count ? malloc(art->dep_count * sizeof(*order)) : NULL;
    if (art->dep_count && !order) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < art->dep_count; i++) order[i] = &art->deps[i];
    if (art->dep_count > 1u) qsort(order, art->dep_count, sizeof(*order), artifact_dep_order_compare);
    IdmBuffer data;
    idm_buf_init(&data);
    bool ok = idm_buf_append(&data, "IDM-ACTION-v1") &&
              idm_buf_append_n(&data, (const char *)art->src_hash, 32u) &&
              idm_buf_put_u32(&data, (uint32_t)art->dep_count);
    for (size_t i = 0; ok && i < art->dep_count; i++) {
        const IdmArtifactDep *dep = order[i];
        ok = idm_buf_put_u8(&data, dep->kind) &&
             idm_buf_put_str(&data, dep->path, strlen(dep->path)) &&
             idm_buf_append_n(&data, (const char *)dep->hash, 32u);
    }
    free(order);
    if (!ok) {
        idm_buf_destroy(&data);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    idm_sha256(data.data ? data.data : "", data.len, art->action_hash);
    idm_buf_destroy(&data);
    return true;
}

bool idm_artifact_serialize(const IdmArtifact *art, IdmBuffer *out, IdmError *err) {
    bool ok = idm_buf_append_n(out, (const char *)art->src_hash, 32u) &&
              idm_buf_append_n(out, (const char *)art->action_hash, 32u);
    ok = ok && idm_buf_put_u32(out, (uint32_t)art->dep_count);
    for (size_t i = 0; ok && i < art->dep_count; i++) {
        ok = idm_buf_put_str(out, art->deps[i].path, strlen(art->deps[i].path));
        ok = ok && idm_buf_append_n(out, (const char *)art->deps[i].hash, 32u);
        ok = ok && idm_buf_put_u8(out, art->deps[i].kind);
    }
    ok = ok && idm_buf_put_u32(out, art->scope_base) && idm_buf_put_u32(out, art->scope_end);
    ok = ok && idm_buf_put_opt_str(out, art->name);
    ok = ok && idm_buf_put_opt_str(out, art->source_reader);
    if (!ok) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!idm_buf_put_u32(out, art->init_fn)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!idm_buf_put_u8(out, art->module ? 1u : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (art->module && !put_module_blob(out, art->module, err)) return false;
    ok = idm_buf_put_u32(out, (uint32_t)art->slot_count);
    for (size_t i = 0; ok && i < art->slot_count; i++) {
        ok = idm_buf_put_str(out, art->slots[i].name, strlen(art->slots[i].name)) &&
             idm_buf_put_u32(out, art->slots[i].slot) &&
             idm_arity_serialize(out, art->slots[i].arity, err) &&
             idm_buf_put_u8(out, art->slots[i].has_contract ? 1u : 0u);
        if (ok && art->slots[i].has_contract) ok = idm_callable_contract_serialize(out, &art->slots[i].contract, err);
        ok = ok &&
             idm_buf_put_u8(out, art->slots[i].exported ? 1u : 0u) &&
             idm_scope_set_serialize(out, &art->slots[i].scopes, NULL);
        ok = ok && idm_buf_put_u32(out, art->slots[i].const_entry_count);
        for (uint32_t k = 0; ok && k < art->slots[i].const_entry_count; k++) ok = idm_buf_put_u32(out, art->slots[i].const_entries[k]);
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)art->import_count);
    for (size_t i = 0; ok && i < art->import_count; i++) {
        const IdmPkgImport *imp = &art->imports[i];
        ok = idm_buf_put_str(out, imp->name, strlen(imp->name)) &&
             idm_buf_put_str(out, imp->env_key ? imp->env_key : "", strlen(imp->env_key ? imp->env_key : "")) &&
             idm_buf_put_u32(out, imp->slot) &&
             idm_arity_serialize(out, imp->arity, err) &&
             idm_buf_put_u8(out, imp->has_contract ? 1u : 0u);
        if (ok && imp->has_contract) ok = idm_callable_contract_serialize(out, &imp->contract, err);
        ok = ok && idm_scope_set_serialize(out, &imp->scopes, NULL);
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)art->field_selector_count);
    for (size_t i = 0; ok && i < art->field_selector_count; i++) {
        ok = idm_buf_put_str(out, art->field_selectors[i].name, strlen(art->field_selectors[i].name)) &&
             idm_buf_put_str(out, art->field_selectors[i].env_key ? art->field_selectors[i].env_key : "", strlen(art->field_selectors[i].env_key ? art->field_selectors[i].env_key : "")) &&
             idm_buf_put_u32(out, art->field_selectors[i].slot);
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)art->operator_count);
    for (size_t i = 0; ok && i < art->operator_count; i++) {
        const IdmOperatorDef *op = &art->operators[i];
        ok = idm_buf_put_str(out, op->name, strlen(op->name));
        ok = ok && idm_buf_put_str(out, op->capture ? op->capture : "infix", strlen(op->capture ? op->capture : "infix"));
        ok = ok && idm_buf_put_u8(out, op->precedence) && idm_buf_put_u8(out, (uint8_t)op->assoc);
        ok = ok && idm_buf_put_u8(out, op->action);
        ok = ok && idm_buf_put_str(out, op->target_name, strlen(op->target_name));
        ok = ok && idm_scope_set_serialize(out, &op->scopes, NULL);
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
    ok = idm_buf_put_u32(out, (uint32_t)artifact_typed_entity_count(&art->typed, IDM_TYPED_ENTITY_TYPE));
    for (size_t i = 0; ok && i < art->typed.entity_count; i++) {
        if (art->typed.entities[i].kind != IDM_TYPED_ENTITY_TYPE) continue;
        const IdmPkgType *t = &art->typed.entities[i].as.type;
        ok = idm_buf_put_str(out, t->name, strlen(t->name)) && idm_buf_put_str(out, t->identity, strlen(t->identity)) && idm_scope_set_serialize(out, &t->scopes, NULL);
        ok = ok && idm_buf_put_u32(out, (uint32_t)t->member_count);
        for (size_t m = 0; ok && m < t->member_count; m++) ok = idm_type_term_serialize(out, &t->members[m].term, err);
        ok = ok && idm_buf_put_u32(out, (uint32_t)t->field_count);
        for (size_t f = 0; ok && f < t->field_count; f++) {
            ok = idm_buf_put_str(out, t->fields[f].name, strlen(t->fields[f].name)) &&
                 idm_buf_put_u8(out, t->fields[f].has_contract ? 1u : 0u);
            if (ok && t->fields[f].has_contract) ok = idm_type_term_serialize(out, &t->fields[f].contract, err);
        }
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)artifact_typed_entity_count(&art->typed, IDM_TYPED_ENTITY_TRAIT));
    for (size_t i = 0; ok && i < art->typed.entity_count; i++) {
        if (art->typed.entities[i].kind != IDM_TYPED_ENTITY_TRAIT) continue;
        const IdmPkgTrait *t = &art->typed.entities[i].as.trait;
        ok = idm_buf_put_str(out, t->name, strlen(t->name)) && idm_buf_put_str(out, t->identity, strlen(t->identity));
        ok = ok && put_requirement_defs(out, t->requirements, t->requirement_count);
        ok = ok && put_method_defs(out, t->methods, t->method_count);
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)art->typed.method_impl_count);
    for (size_t i = 0; ok && i < art->typed.method_impl_count; i++) {
        const IdmPkgMethodImpl *impl = &art->typed.method_impls[i];
        ok = idm_buf_put_str(out, impl->trait, strlen(impl->trait)) &&
             idm_buf_put_str(out, impl->type, strlen(impl->type)) &&
             idm_buf_put_str(out, impl->method, strlen(impl->method)) &&
             idm_arity_serialize(out, impl->arity, err) &&
             idm_buf_put_u8(out, impl->impl_env ? 1u : 0u) &&
             idm_buf_put_str(out, impl->impl_env_key ? impl->impl_env_key : "", strlen(impl->impl_env_key ? impl->impl_env_key : "")) &&
             idm_buf_put_u32(out, impl->impl_slot) &&
             idm_buf_put_u8(out, impl->has_contract ? 1u : 0u);
        if (ok && impl->has_contract) ok = idm_callable_contract_serialize(out, &impl->contract, err);
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)artifact_typed_entity_count(&art->typed, IDM_TYPED_ENTITY_PROTOCOL));
    for (size_t i = 0; ok && i < art->typed.entity_count; i++) {
        if (art->typed.entities[i].kind != IDM_TYPED_ENTITY_PROTOCOL) continue;
        const IdmPkgProtocol *p = &art->typed.entities[i].as.protocol;
        ok = idm_buf_put_str(out, p->name, strlen(p->name)) && idm_buf_put_str(out, p->identity, strlen(p->identity));
        if (ok) {
            IdmBuffer blob;
            idm_buf_init(&blob);
            if (!p->art || !idm_artifact_serialize(p->art, &blob, err)) {
                idm_buf_destroy(&blob);
                return p->art ? false : idm_error_set(err, idm_span_unknown(NULL), "protocol artifact missing body");
            }
            ok = idm_buf_put_u64(out, (uint64_t)blob.len) && idm_buf_append_n(out, blob.data ? blob.data : "", blob.len);
            idm_buf_destroy(&blob);
        }
    }
    if (!ok) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!idm_buf_put_u32(out, (uint32_t)art->core_form_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < art->core_form_count; i++) {
        const IdmPkgCoreForm *core_form = &art->core_form[i];
        if (!core_form->name || !core_form->module) return idm_error_set(err, idm_span_unknown(NULL), "core-form artifact entry is incomplete");
        if (!idm_buf_put_str(out, core_form->name, strlen(core_form->name)) ||
            !idm_buf_put_u32(out, core_form->function_index)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!put_module_blob(out, &core_form->module->module, err)) return false;
    }
    if (!idm_buf_put_u32(out, (uint32_t)art->protocol_require_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < art->protocol_require_count; i++) {
        if (!art->protocol_requires[i]) return idm_error_set(err, idm_span_unknown(NULL), "protocol requirement entry is incomplete");
        if (!idm_buf_put_str(out, art->protocol_requires[i], strlen(art->protocol_requires[i]))) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (!idm_buf_put_u32(out, (uint32_t)art->reader_forms_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < art->reader_forms_count; i++) {
        const IdmPkgReaderForm *rf = &art->reader_forms[i];
        if (!rf->name || (rf->transformer ? rf->node != NULL || !rf->module : rf->node == NULL)) return idm_error_set(err, idm_span_unknown(NULL), "reader-form artifact entry is incomplete");
        if (!idm_buf_put_str(out, rf->name, strlen(rf->name)) || !idm_buf_put_u8(out, rf->transformer ? 1u : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (rf->transformer) {
            if (!idm_buf_put_u32(out, rf->function_index)) return idm_error_oom(err, idm_span_unknown(NULL));
            if (!put_module_blob(out, &rf->module->module, err)) return false;
        } else if (!enforest_node_serialize(out, rf->node, err)) {
            return false;
        }
    }
    if (!grammar_artifacts_validate(art->grammars, art->grammar_count, err, idm_span_unknown(NULL)) ||
        !source_reader_artifact_validate(art->source_reader, art->grammars, art->grammar_count, err, idm_span_unknown(NULL))) return false;
    if (!idm_buf_put_u32(out, (uint32_t)art->grammar_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < art->grammar_count; i++) {
        const IdmPkgGrammar *grammar = &art->grammars[i];
        if (!idm_buf_put_str(out, grammar->name, strlen(grammar->name)) ||
            !idm_buf_put_u8(out, grammar->mode) ||
            !idm_scope_set_serialize(out, &grammar->scopes, NULL) ||
            !idm_buf_put_u32(out, (uint32_t)grammar->rule_count)) return idm_error_oom(err, idm_span_unknown(NULL));
        for (size_t r = 0; r < grammar->rule_count; r++) {
            const IdmGrammarRule *rule = &grammar->rules[r];
            if (!idm_buf_put_str(out, rule->name, strlen(rule->name)) ||
                !idm_buf_put_u8(out, rule->kind) ||
                !idm_buf_put_u32(out, (uint32_t)rule->param_count)) return idm_error_oom(err, idm_span_unknown(NULL));
            for (size_t pi = 0; pi < rule->param_count; pi++) {
                if (!idm_buf_put_str(out, rule->params[pi], strlen(rule->params[pi]))) return idm_error_oom(err, idm_span_unknown(NULL));
            }
            if (!idm_buf_put_u8(out, rule->terminal.kind)) return idm_error_oom(err, idm_span_unknown(NULL));
            if (rule->terminal.kind != IDM_GRAMMAR_TERMINAL_NONE) {
                if (!rule->terminal.text) return idm_error_set(err, idm_span_unknown(NULL), "grammar terminal descriptor is incomplete");
                if (!idm_buf_put_str(out, rule->terminal.text, strlen(rule->terminal.text)) ||
                    !idm_buf_put_u32(out, rule->terminal.flags)) return idm_error_oom(err, idm_span_unknown(NULL));
            }
            if (!idm_reader_program_serialize(out, &rule->pattern, IDM_READER_PROGRAM_PATTERN, err)) return false;
            if (!idm_reader_program_serialize(out, &rule->constructor, IDM_READER_PROGRAM_CTOR, err)) return false;
        }
        if (!idm_buf_put_u32(out, (uint32_t)grammar->pair_count)) return idm_error_oom(err, idm_span_unknown(NULL));
        for (size_t p = 0; p < grammar->pair_count; p++) {
            if (!idm_buf_put_str(out, grammar->pairs[p].open, strlen(grammar->pairs[p].open)) ||
                !idm_buf_put_str(out, grammar->pairs[p].close, strlen(grammar->pairs[p].close))) return idm_error_oom(err, idm_span_unknown(NULL));
        }
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

static bool read_module_ref(IdmRuntime *rt, IdmByteReader *r, IdmModuleRef **out, IdmError *err) {
    *out = idm_module_ref_create(rt);
    return *out != NULL && read_module_blob(rt, r, &(*out)->module, err);
}

#define RD_U8(dst) do { (dst) = ok ? idm_rd_u8(&r) : 0u; ok = ok && r.ok; } while (0)
#define RD_U32(dst) do { (dst) = ok ? idm_rd_u32(&r) : 0u; ok = ok && r.ok; } while (0)
#define RD_STR(dst) do { if (ok) ok = artifact_read_str(&r, &(dst), err); } while (0)
#define RD_OPT_STR(dst) do { if (ok) ok = idm_rd_opt_str(&r, &(dst), err); } while (0)
#define RD_HASH32(dst) \
    do { \
        if (ok && r.pos + 32u > r.len) ok = idm_error_set(err, idm_span_unknown(NULL), "truncated artifact hash"); \
        if (ok) { \
            memcpy((dst), r.data + r.pos, 32u); \
            r.pos += 32u; \
        } \
    } while (0)
#define RD_FLAG(dst, what) \
    do { \
        uint8_t rd_flag_; \
        RD_U8(rd_flag_); \
        if (ok && rd_flag_ > 1u) { \
            r.ok = false; \
            ok = idm_error_set(err, idm_span_unknown(NULL), "invalid " what " flag"); \
        } \
        (dst) = ok && rd_flag_ != 0u; \
    } while (0)
#define RD_CONTRACT_PRIMITIVE(contract, what) \
    do { \
        if (ok && (contract).passthrough && !idm_primitive_info((IdmPrimitive)(contract).primitive)) { \
            r.ok = false; \
            ok = idm_error_set(err, idm_span_unknown(NULL), what " %u out of bounds", (contract).primitive); \
        } \
    } while (0)
#define RD_CALLOC(arr, count) \
    do { \
        if (ok && (count) > (r.len - r.pos) / 4u) ok = idm_error_set(err, idm_span_unknown(NULL), "truncated .ic artifact"); \
        if (ok && (count) != 0u) { \
            (arr) = calloc((count), sizeof(*(arr))); \
            if (!(arr)) ok = idm_error_oom(err, idm_span_unknown(NULL)); \
        } \
    } while (0)

bool idm_artifact_deserialize(IdmRuntime *rt, const unsigned char *data, size_t len, IdmArtifact *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    IdmByteReader r;
    idm_byte_reader_init(&r, data, len);
    bool ok = true;
    RD_HASH32(out->src_hash);
    RD_HASH32(out->action_hash);
    uint32_t dep_count;
    RD_U32(dep_count);
    RD_CALLOC(out->deps, dep_count);
    for (uint32_t i = 0; ok && i < dep_count; i++) {
        out->dep_count = i + 1u;
        RD_STR(out->deps[i].path);
        RD_HASH32(out->deps[i].hash);
        RD_U8(out->deps[i].kind);
    }
    RD_U32(out->scope_base);
    RD_U32(out->scope_end);
    RD_OPT_STR(out->name);
    RD_OPT_STR(out->source_reader);
    RD_U32(out->init_fn);
    bool has_module;
    RD_FLAG(has_module, "artifact module");
    if (ok && has_module) {
        out->module = malloc(sizeof(*out->module));
        if (!out->module) ok = false;
        else {
            idm_bc_init(out->module);
            if (!read_module_blob(rt, &r, out->module, err)) {
                idm_bc_destroy(out->module);
                free(out->module);
                out->module = NULL;
                ok = false;
            }
        }
    }
    uint32_t slot_count;
    RD_U32(slot_count);
    RD_CALLOC(out->slots, slot_count);
    for (uint32_t i = 0; ok && i < slot_count; i++) {
        IdmPkgSlot *slot = &out->slots[i];
        out->slot_count = i + 1u;
        RD_STR(slot->name);
        RD_U32(slot->slot);
        slot->arity = idm_arity_unknown();
        if (ok) ok = idm_arity_deserialize(&r, &slot->arity, err);
        RD_FLAG(slot->has_contract, "package slot contract");
        if (ok && slot->has_contract) ok = idm_callable_contract_deserialize(rt, &r, &slot->contract, err);
        if (ok && slot->has_contract) RD_CONTRACT_PRIMITIVE(slot->contract, "package slot primitive");
        RD_FLAG(slot->exported, "package slot export");
        idm_scope_set_init(&slot->scopes);
        if (ok) ok = idm_scope_set_deserialize(&r, &slot->scopes, NULL);
        if (ok) {
            uint32_t entry_count = idm_rd_u32(&r);
            if (!r.ok) ok = false;
            else if (entry_count != 0) {
                slot->const_entries = calloc(entry_count, sizeof(*slot->const_entries));
                if (!slot->const_entries) ok = idm_error_oom(err, idm_span_unknown(NULL));
                for (uint32_t k = 0; ok && k < entry_count; k++) {
                    slot->const_entries[k] = idm_rd_u32(&r);
                    if (!r.ok) ok = false;
                }
                if (ok) slot->const_entry_count = entry_count;
            }
        }
    }
    uint32_t import_count;
    RD_U32(import_count);
    RD_CALLOC(out->imports, import_count);
    for (uint32_t i = 0; ok && i < import_count; i++) {
        IdmPkgImport *imp = &out->imports[i];
        out->import_count = i + 1u;
        RD_STR(imp->name);
        RD_STR(imp->env_key);
        RD_U32(imp->slot);
        imp->arity = idm_arity_unknown();
        if (ok) ok = idm_arity_deserialize(&r, &imp->arity, err);
        RD_FLAG(imp->has_contract, "package import contract");
        if (ok && imp->has_contract) ok = idm_callable_contract_deserialize(rt, &r, &imp->contract, err);
        if (ok && imp->has_contract) RD_CONTRACT_PRIMITIVE(imp->contract, "package import primitive");
        idm_scope_set_init(&imp->scopes);
        if (ok) ok = idm_scope_set_deserialize(&r, &imp->scopes, NULL);
    }
    uint32_t fsel_count;
    RD_U32(fsel_count);
    RD_CALLOC(out->field_selectors, fsel_count);
    for (uint32_t i = 0; ok && i < fsel_count; i++) {
        out->field_selector_count = i + 1u;
        RD_STR(out->field_selectors[i].name);
        RD_STR(out->field_selectors[i].env_key);
        RD_U32(out->field_selectors[i].slot);
    }
    uint32_t operator_count;
    RD_U32(operator_count);
    RD_CALLOC(out->operators, operator_count);
    for (uint32_t i = 0; ok && i < operator_count; i++) {
        IdmOperatorDef *op = &out->operators[i];
        out->operator_count = i + 1u;
        RD_STR(op->name);
        RD_STR(op->capture);
        RD_U8(op->precedence);
        uint8_t assoc;
        RD_U8(assoc);
        op->assoc = (IdmOpAssoc)assoc;
        RD_U8(op->action);
        RD_STR(op->target_name);
        op->exported = true;
        idm_scope_set_init(&op->scopes);
        if (ok) ok = idm_scope_set_deserialize(&r, &op->scopes, NULL);
        bool has_target_module;
        RD_FLAG(has_target_module, "operator target module");
        if (ok && has_target_module) {
            RD_U32(op->target_function_index);
            if (ok) ok = read_module_ref(rt, &r, &op->target_module, err);
        }
    }
    uint32_t macro_count;
    RD_U32(macro_count);
    RD_CALLOC(out->macros, macro_count);
    for (uint32_t i = 0; ok && i < macro_count; i++) {
        out->macro_count = i + 1u;
        RD_STR(out->macros[i].name);
        RD_U32(out->macros[i].function_index);
        if (ok) ok = read_module_ref(rt, &r, &out->macros[i].module, err);
    }
    size_t typed_entity_cap = 0;
    uint32_t type_count;
    RD_U32(type_count);
    for (uint32_t i = 0; ok && i < type_count; i++) {
        IdmPkgTypedEntity *entity = artifact_typed_append_entity(&out->typed, &typed_entity_cap, IDM_TYPED_ENTITY_TYPE, err);
        if (!entity) { ok = false; break; }
        IdmPkgType *t = &entity->as.type;
        idm_scope_set_init(&t->scopes);
        RD_STR(t->name);
        RD_STR(t->identity);
        if (ok) ok = idm_scope_set_deserialize(&r, &t->scopes, NULL);
        uint32_t member_count;
        RD_U32(member_count);
        RD_CALLOC(t->members, member_count);
        for (uint32_t m = 0; ok && m < member_count; m++) {
            t->member_count = m + 1u;
            ok = idm_type_term_deserialize(rt, &r, &t->members[m].term, err);
        }
        uint32_t field_count;
        RD_U32(field_count);
        RD_CALLOC(t->fields, field_count);
        for (uint32_t f = 0; ok && f < field_count; f++) {
            t->field_count = f + 1u;
            RD_STR(t->fields[f].name);
            RD_FLAG(t->fields[f].has_contract, "type field contract");
            if (ok && t->fields[f].has_contract) ok = idm_type_term_deserialize(rt, &r, &t->fields[f].contract, err);
        }
    }
    uint32_t trait_count;
    RD_U32(trait_count);
    for (uint32_t i = 0; ok && i < trait_count; i++) {
        IdmPkgTypedEntity *entity = artifact_typed_append_entity(&out->typed, &typed_entity_cap, IDM_TYPED_ENTITY_TRAIT, err);
        if (!entity) { ok = false; break; }
        IdmPkgTrait *t = &entity->as.trait;
        RD_STR(t->name);
        RD_STR(t->identity);
        if (ok) ok = read_requirement_defs(&r, &t->requirements, &t->requirement_count, err);
        if (ok) ok = read_method_defs(rt, &r, &t->methods, &t->method_count, err);
    }
    uint32_t method_impl_count;
    RD_U32(method_impl_count);
    RD_CALLOC(out->typed.method_impls, method_impl_count);
    for (uint32_t i = 0; ok && i < method_impl_count; i++) {
        IdmPkgMethodImpl *impl = &out->typed.method_impls[i];
        out->typed.method_impl_count = i + 1u;
        RD_STR(impl->trait);
        RD_STR(impl->type);
        RD_STR(impl->method);
        impl->arity = idm_arity_unknown();
        if (ok) ok = idm_arity_deserialize(&r, &impl->arity, err);
        RD_FLAG(impl->impl_env, "method impl env");
        RD_STR(impl->impl_env_key);
        RD_U32(impl->impl_slot);
        RD_FLAG(impl->has_contract, "method impl contract");
        if (ok && impl->has_contract) ok = idm_callable_contract_deserialize(rt, &r, &impl->contract, err);
        if (ok && impl->has_contract) RD_CONTRACT_PRIMITIVE(impl->contract, "method impl primitive");
    }
    uint32_t protocol_count;
    RD_U32(protocol_count);
    for (uint32_t i = 0; ok && i < protocol_count; i++) {
        IdmPkgTypedEntity *entity = artifact_typed_append_entity(&out->typed, &typed_entity_cap, IDM_TYPED_ENTITY_PROTOCOL, err);
        if (!entity) { ok = false; break; }
        IdmPkgProtocol *p = &entity->as.protocol;
        RD_STR(p->name);
        RD_STR(p->identity);
        uint64_t blob_len = ok ? idm_rd_u64(&r) : 0u;
        if (ok && (!r.ok || blob_len > r.len - r.pos)) ok = idm_error_set(err, idm_span_unknown(NULL), "truncated protocol artifact");
        if (ok) {
            p->art = calloc(1u, sizeof(*p->art));
            if (!p->art) ok = false;
            else if (!idm_artifact_deserialize(rt, r.data + r.pos, (size_t)blob_len, p->art, err)) ok = false;
            r.pos += (size_t)blob_len;
        }
    }
    uint32_t core_form_count;
    RD_U32(core_form_count);
    RD_CALLOC(out->core_form, core_form_count);
    for (uint32_t i = 0; ok && i < core_form_count; i++) {
        IdmPkgCoreForm *core_form = &out->core_form[i];
        out->core_form_count = i + 1u;
        RD_STR(core_form->name);
        RD_U32(core_form->function_index);
        if (ok) ok = read_module_ref(rt, &r, &core_form->module, err);
    }
    uint32_t protocol_require_count;
    RD_U32(protocol_require_count);
    RD_CALLOC(out->protocol_requires, protocol_require_count);
    for (uint32_t i = 0; ok && i < protocol_require_count; i++) {
        out->protocol_require_count = i + 1u;
        RD_STR(out->protocol_requires[i]);
    }
    uint32_t reader_forms_count;
    RD_U32(reader_forms_count);
    RD_CALLOC(out->reader_forms, reader_forms_count);
    for (uint32_t i = 0; ok && i < reader_forms_count; i++) {
        IdmPkgReaderForm *rf = &out->reader_forms[i];
        out->reader_forms_count = i + 1u;
        RD_STR(rf->name);
        bool transformer;
        RD_FLAG(transformer, "reader-form transformer");
        if (ok && transformer) {
            rf->transformer = true;
            RD_U32(rf->function_index);
            if (ok) ok = read_module_ref(rt, &r, &rf->module, err);
        } else if (ok) {
            ok = enforest_node_deserialize(&r, &rf->node, 0u, err);
        }
    }
    uint32_t grammar_count;
    RD_U32(grammar_count);
    RD_CALLOC(out->grammars, grammar_count);
    for (uint32_t i = 0; ok && i < grammar_count; i++) {
        IdmPkgGrammar *grammar = &out->grammars[i];
        out->grammar_count = i + 1u;
        idm_scope_set_init(&grammar->scopes);
        RD_STR(grammar->name);
        RD_U8(grammar->mode);
        if (ok) ok = idm_scope_set_deserialize(&r, &grammar->scopes, NULL);
        uint32_t rule_count;
        RD_U32(rule_count);
        RD_CALLOC(grammar->rules, rule_count);
        for (uint32_t j = 0; ok && j < rule_count; j++) {
            IdmGrammarRule *rule = &grammar->rules[j];
            grammar->rule_count = j + 1u;
            RD_STR(rule->name);
            RD_U8(rule->kind);
            uint32_t rule_param_count;
            RD_U32(rule_param_count);
            if (ok && rule_param_count) {
                RD_CALLOC(rule->params, rule_param_count);
                for (uint32_t pi = 0; ok && pi < rule_param_count; pi++) {
                    rule->param_count = pi + 1u;
                    RD_STR(rule->params[pi]);
                }
            }
            RD_U8(rule->terminal.kind);
            if (ok && rule->terminal.kind != IDM_GRAMMAR_TERMINAL_NONE) {
                RD_STR(rule->terminal.text);
                RD_U32(rule->terminal.flags);
            }
            if (ok) ok = idm_reader_program_deserialize(rt, &r, &rule->pattern, IDM_READER_PROGRAM_PATTERN, err);
            if (ok) ok = idm_reader_program_deserialize(rt, &r, &rule->constructor, IDM_READER_PROGRAM_CTOR, err);
        }
        uint32_t pair_count;
        RD_U32(pair_count);
        RD_CALLOC(grammar->pairs, pair_count);
        for (uint32_t p = 0; ok && p < pair_count; p++) {
            IdmGrammarPair *pair = &grammar->pairs[p];
            grammar->pair_count = p + 1u;
            RD_STR(pair->open);
            RD_STR(pair->close);
        }
    }
    if (ok) ok = grammar_artifacts_validate(out->grammars, out->grammar_count, err, idm_span_unknown(NULL));
    if (ok) ok = source_reader_artifact_validate(out->source_reader, out->grammars, out->grammar_count, err, idm_span_unknown(NULL));
    IdmEnv *phase_runtime_env = NULL;
    IdmPhaseEnv *phase_env = NULL;
    if (ok) {
        phase_runtime_env = idm_fresh_phase_runtime_env(rt, err);
        phase_env = phase_runtime_env ? idm_phase_env_create(rt, phase_runtime_env) : NULL;
        if (!phase_env) ok = false;
    }
    uint32_t phase_count;
    RD_U32(phase_count);
    for (uint32_t i = 0; ok && i < phase_count; i++) {
        uint32_t main_fn;
        RD_U32(main_fn);
        if (!ok) break;
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
            out->macros[i].phase_env = idm_phase_env_retain(phase_env);
        }
        for (size_t i = 0; i < out->operator_count; i++) {
            if (!out->operators[i].target_module) continue;
            out->operators[i].target_phase_env = idm_phase_env_retain(phase_env);
        }
        for (size_t i = 0; i < out->core_form_count; i++) {
            out->core_form[i].phase_env = idm_phase_env_retain(phase_env);
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

#undef RD_U8
#undef RD_U32
#undef RD_STR
#undef RD_OPT_STR
#undef RD_HASH32
#undef RD_FLAG
#undef RD_CONTRACT_PRIMITIVE
#undef RD_CALLOC

bool idm_artifact_run_phase_inits(IdmRuntime *rt, const IdmArtifact *art, IdmError *err) {
    const IdmPhaseEnv *env = art->phase_env;
    bool ok = true;
    if (env && env->module_count != 0) {
        for (size_t i = 0; ok && i < env->module_count; i++) {
            IdmValue ignored = idm_nil();
            ok = idm_vm_run_in_env(rt, env->modules[i], env->module_main_fns[i], env->env, &ignored, err);
        }
    }
    for (size_t i = 0; ok && i < art->typed.entity_count; i++) {
        const IdmPkgTypedEntity *entity = &art->typed.entities[i];
        if (entity->kind == IDM_TYPED_ENTITY_PROTOCOL && entity->as.protocol.art) {
            ok = idm_artifact_run_phase_inits(rt, entity->as.protocol.art, err);
        }
    }
    return ok;
}

bool idm_artifact_cache_disabled(void) {
    const char *v = getenv("IDIOMCACHE");
    return v && strcmp(v, "0") == 0;
}

static bool artifact_cache_root(IdmBuffer *out) {
    idm_buf_init(out);
    const char *configured = getenv("IDIOMCACHE");
    if (configured && configured[0] && strcmp(configured, "1") != 0) {
        return idm_buf_append(out, configured);
    }
    const char *base = getenv("XDG_CACHE_HOME");
    if (base && base[0]) return idm_buf_append(out, base) && idm_buf_append(out, "/idiom");
    const char *home = getenv("HOME");
    if (home && home[0]) return idm_buf_append(out, home) && idm_buf_append(out, "/.cache/idiom");
    return idm_buf_appendf(out, "/tmp/idiom-%lu", (unsigned long)getuid());
}

static bool artifact_cache_mkdir_all(const char *path) {
    char *copy = idm_strdup(path);
    if (!copy) return false;
    bool ok = true;
    for (char *p = copy + 1; ok && *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        ok = mkdir(copy, 0777) == 0 || errno == EEXIST;
        *p = '/';
    }
    if (ok) ok = mkdir(copy, 0777) == 0 || errno == EEXIST;
    free(copy);
    return ok;
}

static void artifact_cache_hash_hex(const unsigned char hash[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32u; i++) {
        out[i * 2u] = digits[hash[i] >> 4u];
        out[i * 2u + 1u] = digits[hash[i] & 15u];
    }
    out[64] = '\0';
}

static unsigned char artifact_cache_hex_digit(unsigned char c) {
    if (c >= (unsigned char)'0' && c <= (unsigned char)'9') return (unsigned char)(c - (unsigned char)'0');
    if (c >= (unsigned char)'a' && c <= (unsigned char)'f') return (unsigned char)(c - (unsigned char)'a' + 10u);
    return 255u;
}

static bool artifact_cache_hex_hash(const char *text, size_t len, unsigned char out[32]) {
    if (len < 64u) return false;
    for (size_t i = 0; i < 32u; i++) {
        unsigned char hi = (unsigned char)text[i * 2u];
        unsigned char lo = (unsigned char)text[i * 2u + 1u];
        unsigned char hv = artifact_cache_hex_digit(hi);
        unsigned char lv = artifact_cache_hex_digit(lo);
        if (hv > 15u || lv > 15u) return false;
        out[i] = (unsigned char)((hv << 4u) | lv);
    }
    return true;
}

static bool artifact_cache_path(const char *kind, const unsigned char hash[32], IdmBuffer *out) {
    idm_buf_init(out);
    IdmBuffer root;
    if (!artifact_cache_root(&root)) {
        idm_buf_destroy(&root);
        return false;
    }
    char hex[65];
    artifact_cache_hash_hex(hash, hex);
    bool ok = idm_buf_append(out, root.data) &&
              idm_buf_append(out, "/v1/") &&
              idm_buf_append(out, kind) &&
              idm_buf_append_char(out, '/') &&
              idm_buf_append_n(out, hex, 2u) &&
              idm_buf_append_char(out, '/') &&
              idm_buf_append(out, hex);
    idm_buf_destroy(&root);
    return ok;
}

static bool artifact_cache_prepare_parent(const char *path) {
    char *copy = idm_strdup(path);
    if (!copy) return false;
    char *slash = strrchr(copy, '/');
    bool ok = slash != NULL;
    if (slash) {
        *slash = '\0';
        ok = artifact_cache_mkdir_all(copy);
    }
    free(copy);
    return ok;
}

static bool artifact_cache_atomic_write(const char *path, const void *data, size_t len) {
    if (!artifact_cache_prepare_parent(path)) return false;
    IdmBuffer tmp;
    idm_buf_init(&tmp);
    bool ok = idm_buf_append(&tmp, path) && idm_buf_append(&tmp, ".tmp.XXXXXX");
    int fd = ok ? mkstemp(tmp.data) : -1;
    FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!f) {
        if (fd >= 0) close(fd);
        ok = false;
    }
    if (ok) ok = fwrite(data, 1u, len, f) == len;
    if (ok) ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f && fclose(f) != 0) ok = false;
    if (ok) ok = rename(tmp.data, path) == 0;
    if (!ok && tmp.data) remove(tmp.data);
    idm_buf_destroy(&tmp);
    return ok;
}

bool idm_tool_cache_store(const char *kind, const unsigned char key[32], const void *data, size_t len) {
    if (idm_artifact_cache_disabled()) return false;
    unsigned char output_hash[32];
    idm_sha256(data, len, output_hash);
    IdmBuffer object_path;
    IdmBuffer index_path;
    idm_buf_init(&object_path);
    idm_buf_init(&index_path);
    bool ok = artifact_cache_path("objects", output_hash, &object_path) &&
              artifact_cache_path(kind, key, &index_path);
    if (ok) ok = artifact_cache_atomic_write(object_path.data, data, len);
    char output_hex[66];
    artifact_cache_hash_hex(output_hash, output_hex);
    output_hex[64] = '\n';
    output_hex[65] = '\0';
    if (ok) ok = artifact_cache_atomic_write(index_path.data, output_hex, 65u);
    idm_buf_destroy(&object_path);
    idm_buf_destroy(&index_path);
    return ok;
}

static void artifact_action_cache_write(const IdmArtifact *art) {
    static const char magic[] = "IDM-ACTION-META-v1";
    if (art->dep_count > UINT32_MAX) return;
    IdmBuffer data;
    idm_buf_init(&data);
    bool ok = idm_buf_append_n(&data, magic, sizeof(magic) - 1u) &&
              idm_buf_append_n(&data, (const char *)art->src_hash, 32u) &&
              idm_buf_append_n(&data, (const char *)art->action_hash, 32u) &&
              idm_buf_put_u32(&data, (uint32_t)art->dep_count);
    for (size_t i = 0; ok && i < art->dep_count; i++) {
        ok = idm_buf_put_u8(&data, art->deps[i].kind) &&
             idm_buf_put_str(&data, art->deps[i].path, strlen(art->deps[i].path)) &&
             idm_buf_append_n(&data, (const char *)art->deps[i].hash, 32u);
    }
    if (ok) (void)idm_tool_cache_store("action-meta", art->src_hash, data.data, data.len);
    idm_buf_destroy(&data);
}

static bool artifact_action_read_bytes(IdmByteReader *r, size_t len, const unsigned char **out) {
    if (!r->ok || len > r->len - r->pos) {
        r->ok = false;
        return false;
    }
    *out = r->data + r->pos;
    r->pos += len;
    return true;
}

bool idm_artifact_action_cache_load(const unsigned char src_hash[32], IdmArtifact *out) {
    static const char magic[] = "IDM-ACTION-META-v1";
    memset(out, 0, sizeof(*out));
    char *data = NULL;
    size_t len = 0;
    if (!idm_tool_cache_load("action-meta", src_hash, &data, &len)) return false;
    IdmByteReader r;
    idm_byte_reader_init(&r, (const unsigned char *)data, len);
    const unsigned char *bytes = NULL;
    bool ok = artifact_action_read_bytes(&r, sizeof(magic) - 1u, &bytes) &&
              memcmp(bytes, magic, sizeof(magic) - 1u) == 0 &&
              artifact_action_read_bytes(&r, 32u, &bytes);
    if (ok) memcpy(out->src_hash, bytes, 32u);
    ok = ok && memcmp(out->src_hash, src_hash, 32u) == 0 && artifact_action_read_bytes(&r, 32u, &bytes);
    if (ok) memcpy(out->action_hash, bytes, 32u);
    uint32_t dep_count = ok ? idm_rd_u32(&r) : 0u;
    if (!r.ok || dep_count > 1048576u) ok = false;
    if (ok && dep_count != 0u) {
        out->deps = calloc(dep_count, sizeof(*out->deps));
        if (!out->deps) ok = false;
    }
    for (uint32_t i = 0; ok && i < dep_count; i++) {
        IdmArtifactDep *dep = &out->deps[i];
        dep->kind = idm_rd_u8(&r);
        dep->path = idm_rd_string(&r, NULL);
        if (dep->path) out->dep_count = i + 1u;
        ok = r.ok && dep->path && artifact_action_read_bytes(&r, 32u, &bytes);
        if (ok) memcpy(dep->hash, bytes, 32u);
    }
    if (ok) {
        unsigned char stored[32];
        memcpy(stored, out->action_hash, 32u);
        IdmError err;
        idm_error_init(&err);
        ok = r.pos == r.len && idm_artifact_compute_action_hash(out, &err) && memcmp(stored, out->action_hash, 32u) == 0;
        idm_error_clear(&err);
    }
    free(data);
    if (!ok) idm_artifact_destroy(out);
    return ok;
}

bool idm_tool_cache_load(const char *kind, const unsigned char key[32], char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (idm_artifact_cache_disabled()) return false;
    IdmBuffer index_path;
    if (!artifact_cache_path(kind, key, &index_path)) {
        idm_buf_destroy(&index_path);
        return false;
    }
    IdmError err;
    idm_error_init(&err);
    char *index = NULL;
    size_t index_len = 0;
    bool ok = idm_read_file(index_path.data, &index, &index_len, &err);
    idm_buf_destroy(&index_path);
    idm_error_clear(&err);
    unsigned char output_hash[32];
    if (ok) ok = artifact_cache_hex_hash(index, index_len, output_hash);
    free(index);
    IdmBuffer object_path;
    if (!ok || !artifact_cache_path("objects", output_hash, &object_path)) {
        if (ok) idm_buf_destroy(&object_path);
        return false;
    }
    idm_error_init(&err);
    ok = idm_read_file(object_path.data, out, out_len, &err);
    idm_buf_destroy(&object_path);
    idm_error_clear(&err);
    if (!ok) return false;
    unsigned char actual_hash[32];
    idm_sha256(*out, *out_len, actual_hash);
    if (memcmp(actual_hash, output_hash, 32u) == 0) return true;
    free(*out);
    *out = NULL;
    *out_len = 0;
    return false;
}

void idm_artifact_cache_write(const char *path, const IdmArtifact *art) {
    (void)path;
    if (idm_artifact_cache_disabled()) return;
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "artifact.cache.write");
    IdmError werr;
    idm_error_init(&werr);
    IdmBuffer blob;
    idm_buf_init(&blob);
    IdmBuffer wire;
    idm_buf_init(&wire);
    bool ok = idm_artifact_serialize(art, &blob, &werr) &&
              idm_wire_begin(&wire, 1u, &werr) &&
              idm_wire_section(&wire, IDM_WIRE_SECTION_PACKAGE, blob.data, blob.len, &werr);
    if (ok) idm_profile_count("artifact.cache.write_bytes", (uint64_t)wire.len);
    if (ok) ok = idm_tool_cache_store("actions", art->src_hash, wire.data, wire.len);
    if (ok) artifact_action_cache_write(art);
    idm_buf_destroy(&blob);
    idm_buf_destroy(&wire);
    idm_error_clear(&werr);
    idm_profile_scope_end(&prof);
}

static pthread_mutex_t g_phase_reads_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool phase_dep_append(IdmPhaseReads *reads, const char *path, const unsigned char hash[32], uint8_t kind) {
    for (size_t i = 0; i < reads->count; i++) {
        if (reads->items[i].kind == kind && strcmp(reads->items[i].path, path) == 0) return true;
    }
    if (reads->count == reads->cap &&
        !idm_grow((void **)&reads->items, &reads->cap, sizeof(*reads->items), 8u, reads->count + 1u)) return false;
    IdmArtifactDep *dep = &reads->items[reads->count];
    memset(dep, 0, sizeof(*dep));
    dep->path = idm_strdup(path);
    if (!dep->path) return false;
    memcpy(dep->hash, hash, 32u);
    dep->kind = kind;
    reads->count++;
    return true;
}

static void env_state_observe(const char *value, unsigned char hash[32]) {
    IdmBuffer input;
    idm_buf_init(&input);
    bool ok = idm_buf_append(&input, value ? "set" : "unset");
    if (ok && value) ok = idm_buf_put_str(&input, value, strlen(value));
    if (ok) idm_sha256(input.data ? input.data : "", input.len, hash);
    else memset(hash, 0, 32u);
    idm_buf_destroy(&input);
}

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

static void stat_state_observe(const char *path, unsigned char hash[32]) {
    struct stat st;
    IdmBuffer data;
    idm_buf_init(&data);
    bool ok;
    if (stat(path, &st) == 0) {
        uint8_t type = S_ISDIR(st.st_mode) ? 1u : (S_ISREG(st.st_mode) ? 2u : 3u);
        ok = idm_buf_put_u8(&data, 1u) &&
             idm_buf_put_u8(&data, type) &&
             idm_buf_put_u64(&data, (uint64_t)st.st_size) &&
             idm_buf_put_u64(&data, (uint64_t)st.st_mtime);
    } else {
        ok = idm_buf_put_u8(&data, 0u) && idm_buf_put_u32(&data, (uint32_t)errno);
    }
    if (ok) idm_sha256(data.data ? data.data : "", data.len, hash);
    else memset(hash, 0, 32u);
    idm_buf_destroy(&data);
}

static void directory_state_observe(const char *path, unsigned char hash[32]) {
    IdmBuffer data;
    idm_buf_init(&data);
    DIR *dir = opendir(path);
    bool ok = idm_buf_put_u8(&data, dir ? 1u : 0u);
    if (!dir) {
        ok = ok && idm_buf_put_u32(&data, (uint32_t)errno);
    } else {
        errno = 0;
        struct dirent *entry;
        while (ok && (entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            ok = idm_buf_put_str(&data, entry->d_name, strlen(entry->d_name));
        }
        ok = ok && idm_buf_put_u32(&data, (uint32_t)errno);
        closedir(dir);
    }
    if (ok) idm_sha256(data.data ? data.data : "", data.len, hash);
    else memset(hash, 0, 32u);
    idm_buf_destroy(&data);
}

static const char *phase_absolute_path(IdmPhaseReads *reads, const char *path, char abs[PATH_MAX]) {
    if (path[0] == '/') return path;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)) || snprintf(abs, PATH_MAX, "%s/%s", cwd, path) >= PATH_MAX) {
        reads->failed = true;
        return NULL;
    }
    return abs;
}

static void phase_path_record(IdmRuntime *rt, const char *path, const unsigned char hash[32], uint8_t kind) {
    IdmPhaseReads *reads = rt->phase_reads;
    if (!reads) return;
    char abs[PATH_MAX];
    path = phase_absolute_path(reads, path, abs);
    if (!path) return;
    pthread_mutex_lock(&g_phase_reads_mutex);
    if (!phase_dep_append(reads, path, hash, kind)) reads->failed = true;
    pthread_mutex_unlock(&g_phase_reads_mutex);
}

void idm_phase_io_record(IdmRuntime *rt, const char *path) {
    IdmPhaseReads *reads = rt->phase_reads;
    if (!reads) return;
    char abs[PATH_MAX];
    path = phase_absolute_path(reads, path, abs);
    if (!path) return;
    unsigned char hash[32];
    uint8_t kind = 0;
    file_state_observe(path, hash, &kind);
    phase_path_record(rt, path, hash, kind);
}

void idm_phase_stat_record(IdmRuntime *rt, const char *path) {
    IdmPhaseReads *reads = rt->phase_reads;
    if (!reads) return;
    char abs[PATH_MAX];
    path = phase_absolute_path(reads, path, abs);
    if (!path) return;
    unsigned char hash[32];
    stat_state_observe(path, hash);
    phase_path_record(rt, path, hash, IDM_DEP_FILE_STAT);
}

void idm_phase_directory_record(IdmRuntime *rt, const char *path) {
    IdmPhaseReads *reads = rt->phase_reads;
    if (!reads) return;
    char abs[PATH_MAX];
    path = phase_absolute_path(reads, path, abs);
    if (!path) return;
    unsigned char hash[32];
    directory_state_observe(path, hash);
    phase_path_record(rt, path, hash, IDM_DEP_DIRECTORY);
}

void idm_phase_write_record(IdmRuntime *rt, const char *path) {
    IdmPhaseReads *reads = rt->phase_reads;
    if (!reads) return;
    const char *events = getenv("IDIOM_TEST_EVENTS");
    if (events && strcmp(events, path) == 0) return;
    pthread_mutex_lock(&g_phase_reads_mutex);
    reads->uncacheable = true;
    pthread_mutex_unlock(&g_phase_reads_mutex);
}

void idm_phase_nondeterministic_record(IdmRuntime *rt) {
    IdmPhaseReads *reads = rt->phase_reads;
    if (!reads) return;
    pthread_mutex_lock(&g_phase_reads_mutex);
    reads->uncacheable = true;
    pthread_mutex_unlock(&g_phase_reads_mutex);
}

void idm_phase_env_record(IdmRuntime *rt, const char *name, const char *value) {
    IdmPhaseReads *reads = rt->phase_reads;
    if (!reads || strcmp(name, "IDIOM_TEST_EVENTS") == 0) return;
    unsigned char hash[32];
    env_state_observe(value, hash);
    pthread_mutex_lock(&g_phase_reads_mutex);
    if (!phase_dep_append(reads, name, hash, IDM_DEP_ENV)) reads->failed = true;
    pthread_mutex_unlock(&g_phase_reads_mutex);
}

void idm_phase_reads_destroy(IdmPhaseReads *reads) {
    for (size_t i = 0; i < reads->count; i++) free(reads->items[i].path);
    free(reads->items);
    memset(reads, 0, sizeof(*reads));
}

bool idm_artifact_dep_verified(IdmRuntime *rt, const IdmArtifactDep *dep) {
    (void)rt;
    if (dep->kind == IDM_DEP_PACKAGE) return false;
    if (dep->kind == IDM_DEP_ENV) {
        unsigned char hash[32];
        env_state_observe(getenv(dep->path), hash);
        return memcmp(hash, dep->hash, 32u) == 0;
    }
    if (dep->kind == IDM_DEP_FILE_STAT) {
        unsigned char hash[32];
        stat_state_observe(dep->path, hash);
        return memcmp(hash, dep->hash, 32u) == 0;
    }
    if (dep->kind == IDM_DEP_DIRECTORY) {
        unsigned char hash[32];
        directory_state_observe(dep->path, hash);
        return memcmp(hash, dep->hash, 32u) == 0;
    }
    unsigned char hash[32];
    uint8_t kind = 0;
    file_state_observe(dep->path, hash, &kind);
    if (kind != dep->kind) return false;
    return kind != IDM_DEP_FILE_HASH || memcmp(hash, dep->hash, 32u) == 0;
}

static bool artifact_dep_verified_with(IdmRuntime *rt, const IdmArtifactDep *dep, IdmArtifactPackageDepVerifier package_dep_verified, void *package_dep_user) {
    if (dep->kind == IDM_DEP_PACKAGE && package_dep_verified) return package_dep_verified(rt, dep->path, dep->hash, package_dep_user);
    return idm_artifact_dep_verified(rt, dep);
}

bool idm_artifact_cache_load(IdmRuntime *rt, const char *path, const unsigned char src_hash[32], IdmArtifact *out, IdmArtifactPackageDepVerifier package_dep_verified, void *package_dep_user) {
    (void)path;
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "artifact.cache.load");
    if (idm_artifact_cache_disabled()) {
        idm_profile_count("artifact.cache.disabled", 1u);
        idm_profile_scope_end(&prof);
        return false;
    }
    IdmArtifact meta;
    if (!idm_artifact_action_cache_load(src_hash, &meta)) {
        idm_profile_count("artifact.cache.miss", 1u);
        idm_profile_scope_end(&prof);
        return false;
    }
    bool deps_ok = true;
    for (size_t i = 0; deps_ok && i < meta.dep_count; i++) {
        deps_ok = artifact_dep_verified_with(rt, &meta.deps[i], package_dep_verified, package_dep_user);
    }
    unsigned char expected_action[32];
    memcpy(expected_action, meta.action_hash, 32u);
    idm_artifact_destroy(&meta);
    if (!deps_ok) {
        idm_profile_count("artifact.cache.stale", 1u);
        idm_profile_scope_end(&prof);
        return false;
    }
    IdmBuffer index_path;
    if (!artifact_cache_path("actions", src_hash, &index_path)) {
        idm_buf_destroy(&index_path);
        idm_profile_scope_end(&prof);
        return false;
    }
    char *index = NULL;
    size_t index_len = 0;
    IdmError rerr;
    idm_error_init(&rerr);
    bool ok = idm_read_file(index_path.data, &index, &index_len, &rerr);
    idm_buf_destroy(&index_path);
    idm_error_clear(&rerr);
    if (!ok) {
        idm_profile_count("artifact.cache.miss", 1u);
        idm_profile_scope_end(&prof);
        return false;
    }
    unsigned char output_hash[32];
    ok = artifact_cache_hex_hash(index, index_len, output_hash);
    free(index);
    IdmBuffer object_path;
    if (!ok || !artifact_cache_path("objects", output_hash, &object_path)) {
        if (ok) idm_buf_destroy(&object_path);
        idm_profile_count("artifact.cache.miss", 1u);
        idm_profile_scope_end(&prof);
        return false;
    }
    char *data = NULL;
    size_t len = 0;
    idm_error_init(&rerr);
    ok = idm_read_file(object_path.data, &data, &len, &rerr);
    idm_buf_destroy(&object_path);
    idm_error_clear(&rerr);
    if (!ok) {
        idm_profile_count("artifact.cache.miss", 1u);
        idm_profile_scope_end(&prof);
        return false;
    }
    idm_profile_count("artifact.cache.read_bytes", (uint64_t)len);
    unsigned char actual_hash[32];
    idm_sha256(data, len, actual_hash);
    ok = memcmp(actual_hash, output_hash, 32u) == 0;
    IdmError derr;
    idm_error_init(&derr);
    const unsigned char *payload = NULL;
    size_t payload_len = 0;
    ok = ok && idm_wire_find((const unsigned char *)data, len, IDM_WIRE_SECTION_PACKAGE, &payload, &payload_len, &derr) &&
         idm_artifact_deserialize(rt, payload, payload_len, out, &derr);
    free(data);
    if (ok) {
        unsigned char stored_action[32];
        memcpy(stored_action, out->action_hash, 32u);
        ok = memcmp(out->src_hash, src_hash, 32u) == 0 &&
             idm_artifact_compute_action_hash(out, &derr) &&
             memcmp(out->action_hash, stored_action, 32u) == 0 &&
             memcmp(out->action_hash, expected_action, 32u) == 0;
        if (ok) ok = idm_artifact_run_phase_inits(rt, out, &derr);
        if (!ok) idm_artifact_destroy(out);
    }
    idm_error_clear(&derr);
    idm_profile_count(ok ? "artifact.cache.hit" : "artifact.cache.stale", 1u);
    idm_profile_scope_end(&prof);
    return ok;
}
