#include "idiom/artifact.h"

#include "idiom/core.h"
#include "idiom/reader.h"
#include "idiom/regex.h"
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
    if (!idm_bc_is_finalized(module)) return idm_error_set(err, idm_span_unknown(NULL), "phase environment module is not finalized");
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
    idm_phase_env_release(macro->phase_env);
    macro->phase_env = NULL;
}

void idm_pkg_core_syntax_destroy(IdmPkgCoreSyntax *core_syntax) {
    if (!core_syntax) return;
    free(core_syntax->name);
    idm_module_ref_release(core_syntax->module);
    idm_phase_env_release(core_syntax->phase_env);
    memset(core_syntax, 0, sizeof(*core_syntax));
}

void idm_grammar_terminal_destroy(IdmGrammarTerminal *terminal) {
    if (!terminal) return;
    free(terminal->text);
    memset(terminal, 0, sizeof(*terminal));
}

void idm_reader_literal_destroy(IdmReaderLiteral *literal) {
    if (!literal) return;
    free(literal->text);
    for (size_t i = 0; i < literal->count; i++) idm_reader_literal_destroy(&literal->items[i]);
    free(literal->items);
    memset(literal, 0, sizeof(*literal));
}

static bool idm_reader_literal_copy_at(IdmReaderLiteral *dst, const IdmReaderLiteral *src, IdmError *err, IdmSpan span, unsigned depth) {
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, span, "reader literal nested too deeply");
    memset(dst, 0, sizeof(*dst));
    if (!src) return true;
    if (src->count > UINT32_MAX) return idm_error_set(err, span, "reader literal has too many children");
    dst->kind = src->kind;
    dst->integer = src->integer;
    dst->real = src->real;
    dst->count = src->count;
    if (src->text) {
        dst->text = idm_strdup(src->text);
        if (!dst->text) {
            idm_reader_literal_destroy(dst);
            return idm_error_oom(err, span);
        }
    }
    if (src->count != 0) {
        dst->items = calloc(src->count, sizeof(*dst->items));
        if (!dst->items) {
            idm_reader_literal_destroy(dst);
            return idm_error_oom(err, span);
        }
        for (size_t i = 0; i < src->count; i++) {
            if (!idm_reader_literal_copy_at(&dst->items[i], &src->items[i], err, span, depth + 1u)) {
                idm_reader_literal_destroy(dst);
                return false;
            }
        }
    }
    return true;
}

bool idm_reader_literal_copy(IdmReaderLiteral *dst, const IdmReaderLiteral *src, IdmError *err, IdmSpan span) {
    return idm_reader_literal_copy_at(dst, src, err, span, 0u);
}

static bool idm_reader_literal_from_syntax_at(IdmReaderLiteral *dst, const IdmSyntax *src, IdmError *err, unsigned depth) {
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, src ? src->span : idm_span_unknown(NULL), "reader literal nested too deeply");
    memset(dst, 0, sizeof(*dst));
    if (!src) return idm_error_set(err, idm_span_unknown(NULL), "reader literal requires syntax");
    dst->kind = (uint8_t)src->kind;
    switch (src->kind) {
        case IDM_SYN_NIL:
            return true;
        case IDM_SYN_WORD:
        case IDM_SYN_ATOM:
        case IDM_SYN_STRING:
            dst->text = idm_strdup(src->as.text ? src->as.text : "");
            if (!dst->text) return idm_error_oom(err, src->span);
            return true;
        case IDM_SYN_INT:
            dst->integer = src->as.integer;
            return true;
        case IDM_SYN_FLOAT:
            dst->real = src->as.real;
            return true;
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
            if (src->as.seq.count > UINT32_MAX) return idm_error_set(err, src->span, "reader literal has too many children");
            dst->count = src->as.seq.count;
            if (dst->count == 0) return true;
            dst->items = calloc(dst->count, sizeof(*dst->items));
            if (!dst->items) return idm_error_oom(err, src->span);
            for (size_t i = 0; i < dst->count; i++) {
                if (!idm_reader_literal_from_syntax_at(&dst->items[i], src->as.seq.items[i], err, depth + 1u)) {
                    idm_reader_literal_destroy(dst);
                    return false;
                }
            }
            return true;
    }
    return idm_error_set(err, src->span, "unsupported reader literal syntax");
}

bool idm_reader_literal_from_syntax(IdmReaderLiteral *dst, const IdmSyntax *src, IdmError *err) {
    return idm_reader_literal_from_syntax_at(dst, src, err, 0u);
}

static IdmSyntax *idm_reader_literal_to_syntax_at(const IdmReaderLiteral *literal, IdmSpan span, IdmError *err, unsigned depth) {
    if (depth > IDM_IC_MAX_DEPTH) {
        idm_error_set(err, span, "reader literal nested too deeply");
        return NULL;
    }
    if (!literal) {
        idm_error_set(err, span, "reader literal is missing");
        return NULL;
    }
    IdmSyntax *out = NULL;
    switch ((IdmSyntaxKind)literal->kind) {
        case IDM_SYN_NIL:
            out = idm_syn_nil(span);
            break;
        case IDM_SYN_WORD:
            out = idm_syn_word(literal->text ? literal->text : "", span);
            break;
        case IDM_SYN_ATOM:
            out = idm_syn_atom(literal->text ? literal->text : "", span);
            break;
        case IDM_SYN_STRING:
            out = idm_syn_string(literal->text ? literal->text : "", span);
            break;
        case IDM_SYN_INT:
            out = idm_syn_int(literal->integer, span);
            break;
        case IDM_SYN_FLOAT:
            out = idm_syn_float(literal->real, span);
            break;
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT: {
            out = literal->kind == (uint8_t)IDM_SYN_LIST ? idm_syn_list(span) :
                  literal->kind == (uint8_t)IDM_SYN_VECTOR ? idm_syn_vector(span) :
                  literal->kind == (uint8_t)IDM_SYN_TUPLE ? idm_syn_tuple(span) :
                  idm_syn_dict(span);
            if (!out) break;
            for (size_t i = 0; i < literal->count; i++) {
                IdmSyntax *child = idm_reader_literal_to_syntax_at(&literal->items[i], span, err, depth + 1u);
                if (!child || !idm_syn_append(out, child)) {
                    idm_syn_free(child);
                    idm_syn_free(out);
                    if (!(err && err->present)) idm_error_oom(err, span);
                    return NULL;
                }
            }
            break;
        }
        default:
            idm_error_set(err, span, "unknown reader literal kind %u", literal->kind);
            return NULL;
    }
    if (!out) idm_error_oom(err, span);
    return out;
}

IdmSyntax *idm_reader_literal_to_syntax(const IdmReaderLiteral *literal, IdmSpan span, IdmError *err) {
    return idm_reader_literal_to_syntax_at(literal, span, err, 0u);
}

static bool idm_reader_literal_equal_syntax_at(const IdmReaderLiteral *literal, const IdmSyntax *syntax, unsigned depth) {
    if (depth > IDM_IC_MAX_DEPTH) return false;
    if (!literal || !syntax || literal->kind != (uint8_t)syntax->kind) return false;
    switch (syntax->kind) {
        case IDM_SYN_NIL:
            return true;
        case IDM_SYN_WORD:
        case IDM_SYN_ATOM:
        case IDM_SYN_STRING:
            return literal->text && strcmp(literal->text, syntax->as.text) == 0;
        case IDM_SYN_INT:
            return literal->integer == syntax->as.integer;
        case IDM_SYN_FLOAT:
            return literal->real == syntax->as.real;
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
            if (literal->count != syntax->as.seq.count) return false;
            for (size_t i = 0; i < literal->count; i++) {
                if (!idm_reader_literal_equal_syntax_at(&literal->items[i], syntax->as.seq.items[i], depth + 1u)) return false;
            }
            return true;
    }
    return false;
}

bool idm_reader_literal_equal_syntax(const IdmReaderLiteral *literal, const IdmSyntax *syntax) {
    return idm_reader_literal_equal_syntax_at(literal, syntax, 0u);
}

static void idm_reader_pattern_inst_destroy(IdmReaderPatternInst *inst) {
    if (!inst) return;
    free(inst->name);
    idm_reader_literal_destroy(&inst->literal);
    memset(inst, 0, sizeof(*inst));
}

void idm_reader_pattern_program_destroy(IdmReaderPatternProgram *program) {
    if (!program) return;
    for (size_t i = 0; i < program->count; i++) idm_reader_pattern_inst_destroy(&program->items[i]);
    free(program->items);
    memset(program, 0, sizeof(*program));
}

static void idm_reader_ctor_inst_destroy(IdmReaderCtorInst *inst) {
    if (!inst) return;
    free(inst->text);
    idm_reader_literal_destroy(&inst->literal);
    memset(inst, 0, sizeof(*inst));
}

void idm_reader_ctor_program_destroy(IdmReaderCtorProgram *program) {
    if (!program) return;
    for (size_t i = 0; i < program->count; i++) idm_reader_ctor_inst_destroy(&program->items[i]);
    free(program->items);
    memset(program, 0, sizeof(*program));
}

static bool idm_reader_pattern_inst_copy(IdmReaderPatternInst *dst, const IdmReaderPatternInst *src, IdmError *err, IdmSpan span) {
    memset(dst, 0, sizeof(*dst));
    dst->op = src->op;
    dst->child_count = src->child_count;
    dst->target_kind = IDM_READER_PATTERN_TARGET_NONE;
    dst->capture_slot = src->capture_slot;
    if (src->name) {
        dst->name = idm_strdup(src->name);
        if (!dst->name) {
            idm_reader_pattern_inst_destroy(dst);
            return idm_error_oom(err, span);
        }
    }
    dst->has_literal = src->has_literal;
    if (src->has_literal) {
        if (!idm_reader_literal_copy(&dst->literal, &src->literal, err, span)) {
            idm_reader_pattern_inst_destroy(dst);
            return false;
        }
    }
    return true;
}

bool idm_reader_pattern_program_copy(IdmReaderPatternProgram *dst, const IdmReaderPatternProgram *src, IdmError *err, IdmSpan span) {
    memset(dst, 0, sizeof(*dst));
    if (!src || src->count == 0) return true;
    dst->items = calloc(src->count, sizeof(*dst->items));
    if (!dst->items) return idm_error_oom(err, span);
    dst->cap = src->count;
    for (size_t i = 0; i < src->count; i++) {
        if (!idm_reader_pattern_inst_copy(&dst->items[i], &src->items[i], err, span)) {
            dst->count = i + 1u;
            idm_reader_pattern_program_destroy(dst);
            return false;
        }
        dst->count = i + 1u;
    }
    return true;
}

static bool idm_reader_ctor_inst_copy(IdmReaderCtorInst *dst, const IdmReaderCtorInst *src, IdmError *err, IdmSpan span) {
    memset(dst, 0, sizeof(*dst));
    dst->op = src->op;
    dst->integer = src->integer;
    dst->child_count = src->child_count;
    dst->capture_slot = src->capture_slot;
    if (src->text) {
        dst->text = idm_strdup(src->text);
        if (!dst->text) {
            idm_reader_ctor_inst_destroy(dst);
            return idm_error_oom(err, span);
        }
    }
    dst->has_literal = src->has_literal;
    if (src->has_literal) {
        if (!idm_reader_literal_copy(&dst->literal, &src->literal, err, span)) {
            idm_reader_ctor_inst_destroy(dst);
            return false;
        }
    }
    return true;
}

bool idm_reader_ctor_program_copy(IdmReaderCtorProgram *dst, const IdmReaderCtorProgram *src, IdmError *err, IdmSpan span) {
    memset(dst, 0, sizeof(*dst));
    if (!src || src->count == 0) return true;
    dst->items = calloc(src->count, sizeof(*dst->items));
    if (!dst->items) return idm_error_oom(err, span);
    dst->cap = src->count;
    for (size_t i = 0; i < src->count; i++) {
        if (!idm_reader_ctor_inst_copy(&dst->items[i], &src->items[i], err, span)) {
            dst->count = i + 1u;
            idm_reader_ctor_program_destroy(dst);
            return false;
        }
        dst->count = i + 1u;
    }
    return true;
}

void idm_reader_pattern_program_relocate(IdmReaderPatternProgram *program, IdmScopeId min_id, int64_t delta) {
    (void)min_id;
    (void)delta;
    for (size_t i = 0; program && i < program->count; i++) {
        program->items[i].target_kind = IDM_READER_PATTERN_TARGET_NONE;
        program->items[i].target_index = 0;
    }
}

void idm_reader_ctor_program_relocate(IdmReaderCtorProgram *program, IdmScopeId min_id, int64_t delta) {
    (void)program;
    (void)min_id;
    (void)delta;
}

void idm_grammar_rule_destroy(IdmGrammarRule *rule) {
    if (!rule) return;
    free(rule->name);
    idm_grammar_terminal_destroy(&rule->terminal);
    idm_reader_pattern_program_destroy(&rule->pattern);
    idm_reader_ctor_program_destroy(&rule->constructor);
    memset(rule, 0, sizeof(*rule));
}

void idm_pkg_grammar_destroy(IdmPkgGrammar *grammar) {
    if (!grammar) return;
    free(grammar->name);
    for (size_t i = 0; i < grammar->rule_count; i++) idm_grammar_rule_destroy(&grammar->rules[i]);
    free(grammar->rules);
    idm_scope_set_destroy(&grammar->scopes);
    memset(grammar, 0, sizeof(*grammar));
}

void idm_pkg_slot_destroy(IdmPkgSlot *slot) {
    if (!slot) return;
    free(slot->name);
    idm_scope_set_destroy(&slot->scopes);
    slot->name = NULL;
    slot->slot = 0;
    slot->arity = idm_arity_unknown();
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
    memset(impl, 0, sizeof(*impl));
}

void idm_pkg_type_destroy(IdmPkgType *type) {
    if (!type) return;
    free(type->name);
    free(type->identity);
    idm_scope_set_destroy(&type->scopes);
    for (size_t i = 0; i < type->field_count; i++) {
        free(type->fields[i].name);
        free(type->fields[i].contract);
    }
    free(type->fields);
    memset(type, 0, sizeof(*type));
}

void idm_pkg_protocol_destroy(IdmPkgProtocol *protocol) {
    if (!protocol) return;
    free(protocol->name);
    free(protocol->identity);
    if (protocol->art) {
        idm_artifact_destroy(protocol->art);
        free(protocol->art);
    }
    memset(protocol, 0, sizeof(*protocol));
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
        methods[i].dispatch_slot = src[i].dispatch_slot;
        methods[i].has_dispatch = src[i].has_dispatch;
        methods[i].default_slot = src[i].default_slot;
        methods[i].has_default_slot = src[i].has_default_slot;
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
    free(art->source_reader);
    if (art->exports) {
        for (size_t i = 0; i < art->export_count; i++) free(art->exports[i].name);
        free(art->exports);
    }
    if (art->slots) {
        for (size_t i = 0; i < art->slot_count; i++) idm_pkg_slot_destroy(&art->slots[i]);
        free(art->slots);
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
    if (art->method_impls) {
        for (size_t i = 0; i < art->method_impl_count; i++) idm_pkg_method_impl_destroy(&art->method_impls[i]);
        free(art->method_impls);
    }
    if (art->protocols) {
        for (size_t i = 0; i < art->protocol_count; i++) idm_pkg_protocol_destroy(&art->protocols[i]);
        free(art->protocols);
    }
    if (art->core_syntax) {
        for (size_t i = 0; i < art->core_syntax_count; i++) idm_pkg_core_syntax_destroy(&art->core_syntax[i]);
        free(art->core_syntax);
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

#define IDM_ARTIFACT_VERSION 70u

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

static bool reader_pattern_program_push(IdmReaderPatternProgram *program, IdmReaderPatternInst inst, IdmError *err, IdmSpan span) {
    if (program->count == program->cap) {
        size_t cap = program->cap ? program->cap * 2u : 16u;
        IdmReaderPatternInst *items = realloc(program->items, cap * sizeof(*items));
        if (!items) {
            idm_reader_pattern_inst_destroy(&inst);
            return idm_error_oom(err, span);
        }
        program->items = items;
        program->cap = cap;
    }
    program->items[program->count++] = inst;
    return true;
}

static bool reader_pattern_set_name(IdmReaderPatternInst *inst, const IdmSyntax *src, size_t index, const char *what, IdmError *err) {
    const IdmSyntax *name = index < src->as.seq.count ? src->as.seq.items[index] : NULL;
    if (!reader_ir_name(name, what, err, src->span)) return false;
    inst->name = idm_strdup(name->as.text);
    if (!inst->name) return idm_error_oom(err, src->span);
    return true;
}

static bool reader_pattern_compile_into(const IdmSyntax *src, IdmReaderPatternProgram *program, IdmError *err) {
    if (!reader_ir_signature(src) || src->as.seq.count == 0) return idm_error_set(err, src ? src->span : idm_span_unknown(NULL), "form pattern expects an iridium pattern node");
    const char *tag = reader_ir_tag(src->as.seq.items[0]);
    if (!tag) return idm_error_set(err, src->as.seq.items[0]->span, "form pattern head must be an atom");
    IdmReaderPatternInst inst;
    memset(&inst, 0, sizeof(inst));
    if (strcmp(tag, "empty") == 0) {
        if (src->as.seq.count != 1u) return idm_error_set(err, src->span, ":empty takes no children");
        inst.op = IDM_READER_PATTERN_EMPTY;
        return reader_pattern_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "ref") == 0 || strcmp(tag, "token") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":%s expects one name", tag);
        inst.op = strcmp(tag, "ref") == 0 ? IDM_READER_PATTERN_REF : IDM_READER_PATTERN_TOKEN;
        if (!reader_pattern_set_name(&inst, src, 1u, "grammar reference", err)) return false;
        return reader_pattern_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "literal") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":literal expects one syntax value");
        inst.op = IDM_READER_PATTERN_LITERAL;
        inst.has_literal = true;
        if (!idm_reader_literal_from_syntax(&inst.literal, src->as.seq.items[1], err)) return false;
        return reader_pattern_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "seq") == 0 || strcmp(tag, "alt") == 0) {
        if (src->as.seq.count < 2u) return idm_error_set(err, src->span, ":%s expects at least one child", tag);
        inst.op = strcmp(tag, "seq") == 0 ? IDM_READER_PATTERN_SEQ : IDM_READER_PATTERN_ALT;
        inst.child_count = src->as.seq.count - 1u;
        if (!reader_pattern_program_push(program, inst, err, src->span)) return false;
        for (size_t i = 1u; i < src->as.seq.count; i++) if (!reader_pattern_compile_into(src->as.seq.items[i], program, err)) return false;
        return true;
    }
    if (strcmp(tag, "repeat") == 0 || strcmp(tag, "optional") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":%s expects one child", tag);
        inst.op = strcmp(tag, "repeat") == 0 ? IDM_READER_PATTERN_REPEAT : IDM_READER_PATTERN_OPTIONAL;
        inst.child_count = 1u;
        return reader_pattern_program_push(program, inst, err, src->span) && reader_pattern_compile_into(src->as.seq.items[1], program, err);
    }
    if (strcmp(tag, "indent-gt") == 0 || strcmp(tag, "indent-eq") == 0) {
        if (src->as.seq.count != 3u) return idm_error_set(err, src->span, ":%s expects a capture name and one child", tag);
        inst.op = strcmp(tag, "indent-gt") == 0 ? IDM_READER_PATTERN_INDENT_GT : IDM_READER_PATTERN_INDENT_EQ;
        inst.child_count = 1u;
        inst.capture_slot = SIZE_MAX;
        if (!reader_pattern_set_name(&inst, src, 1u, tag, err)) return false;
        return reader_pattern_program_push(program, inst, err, src->span) && reader_pattern_compile_into(src->as.seq.items[2], program, err);
    }
    if (strcmp(tag, "adjacent") == 0 || strcmp(tag, "not-adjacent") == 0 || strcmp(tag, "peek") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":%s expects one child", tag);
        inst.op = strcmp(tag, "adjacent") == 0 ? IDM_READER_PATTERN_ADJACENT :
                  strcmp(tag, "not-adjacent") == 0 ? IDM_READER_PATTERN_NOT_ADJACENT :
                  IDM_READER_PATTERN_PEEK;
        inst.child_count = 1u;
        return reader_pattern_program_push(program, inst, err, src->span) && reader_pattern_compile_into(src->as.seq.items[1], program, err);
    }
    if (strcmp(tag, "capture") == 0) {
        if (src->as.seq.count != 3u) return idm_error_set(err, src->span, ":capture expects a name and one child");
        inst.op = IDM_READER_PATTERN_CAPTURE;
        inst.child_count = 1u;
        inst.capture_slot = SIZE_MAX;
        if (!reader_pattern_set_name(&inst, src, 1u, "capture", err)) return false;
        return reader_pattern_program_push(program, inst, err, src->span) && reader_pattern_compile_into(src->as.seq.items[2], program, err);
    }
    return idm_error_set(err, src->span, "unknown form pattern ':%s'", tag);
}

bool idm_reader_pattern_compile_ir(const IdmSyntax *src, IdmReaderPatternProgram *out, IdmError *err) {
    IdmReaderPatternProgram program;
    memset(&program, 0, sizeof(program));
    if (!reader_pattern_compile_into(src, &program, err)) {
        idm_reader_pattern_program_destroy(&program);
        return false;
    }
    *out = program;
    return true;
}

static bool reader_ctor_program_push(IdmReaderCtorProgram *program, IdmReaderCtorInst inst, IdmError *err, IdmSpan span) {
    if (program->count == program->cap) {
        size_t cap = program->cap ? program->cap * 2u : 16u;
        IdmReaderCtorInst *items = realloc(program->items, cap * sizeof(*items));
        if (!items) {
            idm_reader_ctor_inst_destroy(&inst);
            return idm_error_oom(err, span);
        }
        program->items = items;
        program->cap = cap;
    }
    program->items[program->count++] = inst;
    return true;
}

static bool reader_ctor_set_text(IdmReaderCtorInst *inst, const IdmSyntax *src, size_t index, const char *what, IdmError *err) {
    const char *text = index < src->as.seq.count ? reader_ir_text(src->as.seq.items[index]) : NULL;
    if (!text || text[0] == '\0') return idm_error_set(err, index < src->as.seq.count ? src->as.seq.items[index]->span : src->span, "%s expects a name", what);
    inst->text = idm_strdup(text);
    if (!inst->text) return idm_error_oom(err, src->span);
    return true;
}

static bool reader_ctor_compile_into(const IdmSyntax *src, IdmReaderCtorProgram *program, IdmError *err);

static bool reader_ctor_compile_children(const IdmSyntax *args, IdmReaderCtorProgram *program, IdmError *err, IdmSpan span) {
    if (!reader_ir_collection(args)) return idm_error_set(err, args ? args->span : span, "constructor list expects a list or vector");
    for (size_t i = 0; i < args->as.seq.count; i++) if (!reader_ctor_compile_into(args->as.seq.items[i], program, err)) return false;
    return true;
}

static bool reader_ctor_compile_into(const IdmSyntax *src, IdmReaderCtorProgram *program, IdmError *err) {
    if (!reader_ir_signature(src) || src->as.seq.count == 0) return idm_error_set(err, src ? src->span : idm_span_unknown(NULL), "constructor expects an iridium constructor node");
    const char *tag = reader_ir_tag(src->as.seq.items[0]);
    if (!tag) return idm_error_set(err, src->as.seq.items[0]->span, "constructor head must be an atom");
    IdmReaderCtorInst inst;
    memset(&inst, 0, sizeof(inst));
    if (strcmp(tag, "capture") == 0 || strcmp(tag, "splice") == 0 ||
        strcmp(tag, "capture-atom") == 0 || strcmp(tag, "capture-word") == 0 || strcmp(tag, "capture-string") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":%s expects one capture name", tag);
        if (strcmp(tag, "capture") == 0) inst.op = IDM_READER_CTOR_CAPTURE;
        else if (strcmp(tag, "splice") == 0) inst.op = IDM_READER_CTOR_SPLICE;
        else if (strcmp(tag, "capture-atom") == 0) inst.op = IDM_READER_CTOR_CAPTURE_ATOM;
        else if (strcmp(tag, "capture-word") == 0) inst.op = IDM_READER_CTOR_CAPTURE_WORD;
        else inst.op = IDM_READER_CTOR_CAPTURE_STRING;
        inst.capture_slot = SIZE_MAX;
        if (!reader_ir_name(src->as.seq.items[1], tag, err, src->span) || !reader_ctor_set_text(&inst, src, 1u, tag, err)) return false;
        return reader_ctor_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "literal") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":literal expects one syntax value");
        inst.op = IDM_READER_CTOR_LITERAL;
        inst.has_literal = true;
        if (!idm_reader_literal_from_syntax(&inst.literal, src->as.seq.items[1], err)) return false;
        return reader_ctor_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "interp-string") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":interp-string expects one capture name");
        inst.op = IDM_READER_CTOR_INTERP_STRING;
        inst.capture_slot = SIZE_MAX;
        if (!reader_ir_name(src->as.seq.items[1], tag, err, src->span) || !reader_ctor_set_text(&inst, src, 1u, tag, err)) return false;
        return reader_ctor_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "emit-atom") == 0 || strcmp(tag, "emit-word") == 0 || strcmp(tag, "emit-string") == 0 || strcmp(tag, "emit-int") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":%s expects one value", tag);
        if (strcmp(tag, "emit-int") == 0) {
            if (src->as.seq.items[1]->kind != IDM_SYN_INT) return idm_error_set(err, src->as.seq.items[1]->span, ":emit-int expects an integer");
            inst.op = IDM_READER_CTOR_EMIT_INT;
            inst.integer = src->as.seq.items[1]->as.integer;
        } else {
            if (strcmp(tag, "emit-atom") == 0) inst.op = IDM_READER_CTOR_EMIT_ATOM;
            else if (strcmp(tag, "emit-word") == 0) inst.op = IDM_READER_CTOR_EMIT_WORD;
            else inst.op = IDM_READER_CTOR_EMIT_STRING;
            if (!reader_ctor_set_text(&inst, src, 1u, tag, err)) return false;
        }
        return reader_ctor_program_push(program, inst, err, src->span);
    }
    if (strcmp(tag, "emit-form") == 0) {
        if (src->as.seq.count != 3u) return idm_error_set(err, src->span, ":emit-form expects a head and a constructor list");
        const IdmSyntax *args = src->as.seq.items[2];
        if (!reader_ir_collection(args)) return idm_error_set(err, args ? args->span : src->span, "constructor list expects a list or vector");
        inst.op = IDM_READER_CTOR_FORM;
        inst.child_count = args->as.seq.count;
        if (!reader_ctor_set_text(&inst, src, 1u, ":emit-form head", err)) return false;
        return reader_ctor_program_push(program, inst, err, src->span) && reader_ctor_compile_children(args, program, err, src->span);
    }
    if (strcmp(tag, "emit-list") == 0 || strcmp(tag, "emit-vector") == 0 || strcmp(tag, "emit-tuple") == 0 || strcmp(tag, "emit-dict") == 0) {
        if (src->as.seq.count != 2u) return idm_error_set(err, src->span, ":%s expects a constructor list", tag);
        const IdmSyntax *args = src->as.seq.items[1];
        if (!reader_ir_collection(args)) return idm_error_set(err, args ? args->span : src->span, "constructor list expects a list or vector");
        if (strcmp(tag, "emit-list") == 0) inst.op = IDM_READER_CTOR_LIST;
        else if (strcmp(tag, "emit-vector") == 0) inst.op = IDM_READER_CTOR_VECTOR;
        else if (strcmp(tag, "emit-tuple") == 0) inst.op = IDM_READER_CTOR_TUPLE;
        else inst.op = IDM_READER_CTOR_DICT;
        inst.child_count = args->as.seq.count;
        return reader_ctor_program_push(program, inst, err, src->span) && reader_ctor_compile_children(args, program, err, src->span);
    }
    return idm_error_set(err, src->span, "unknown constructor ':%s'", tag);
}

bool idm_reader_ctor_compile_ir(const IdmSyntax *src, IdmReaderCtorProgram *out, IdmError *err) {
    IdmReaderCtorProgram program;
    memset(&program, 0, sizeof(program));
    if (!reader_ctor_compile_into(src, &program, err)) {
        idm_reader_ctor_program_destroy(&program);
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
        if (!idm_reader_pattern_compile_ir(rule->as.seq.items[2], &out->pattern, err)) return false;
    } else if (reader_ir_tag_is(rule->as.seq.items[0], "skip")) {
        if (rule->as.seq.count != 3u) return idm_error_set(err, rule->span, "core-grammar skip rule expects {:skip NAME PATTERN}");
        out->kind = (uint8_t)IDM_GRAMMAR_RULE_SKIP;
        if (!idm_grammar_terminal_from_ir(rule->as.seq.items[2], &out->terminal, err)) return false;
    } else {
        const char *tag = reader_ir_tag(rule->as.seq.items[0]);
        return idm_error_set(err, rule->as.seq.items[0]->span, "core-grammar rule kind must be :skip, :token, or :form, got '%s'", tag ? tag : "<non-atom>");
    }
    if (!reader_ir_name(rule->as.seq.items[1], "core-grammar rule", err, rule->span)) {
        idm_grammar_rule_destroy(out);
        return false;
    }
    out->name = idm_strdup(rule->as.seq.items[1]->as.text);
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
    out->rule_count = rules_syntax->as.seq.count;
    out->rules = calloc(out->rule_count, sizeof(*out->rules));
    out->exported = true;
    if (!out->name || !out->rules) {
        idm_pkg_grammar_destroy(out);
        return idm_error_oom(err, form->span);
    }
    for (size_t i = 0; i < out->rule_count; i++) {
        if (!core_grammar_rule_from_ir(rules_syntax->as.seq.items[i], &out->rules[i], err)) {
            idm_pkg_grammar_destroy(out);
            return false;
        }
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

static bool reader_pattern_validate_at(const IdmReaderPatternProgram *program, size_t pc, size_t *out_next, IdmError *err, IdmSpan span) {
    if (!program || pc >= program->count) return idm_error_set(err, span, "reader pattern program ended early");
    const IdmReaderPatternInst *inst = &program->items[pc];
    bool has_name = inst->name && inst->name[0] != '\0';
    switch ((IdmReaderPatternOp)inst->op) {
        case IDM_READER_PATTERN_EMPTY:
            if (inst->child_count != 0 || has_name || inst->has_literal) return idm_error_set(err, span, "invalid empty reader pattern instruction");
            *out_next = pc + 1u;
            return true;
        case IDM_READER_PATTERN_REF:
        case IDM_READER_PATTERN_TOKEN:
            if (inst->child_count != 0 || !has_name || inst->has_literal) return idm_error_set(err, span, "invalid reader pattern reference instruction");
            *out_next = pc + 1u;
            return true;
        case IDM_READER_PATTERN_LITERAL:
            if (inst->child_count != 0 || has_name || !inst->has_literal) return idm_error_set(err, span, "invalid literal reader pattern instruction");
            *out_next = pc + 1u;
            return true;
        case IDM_READER_PATTERN_SEQ:
        case IDM_READER_PATTERN_ALT:
            if (inst->child_count == 0 || has_name || inst->has_literal) return idm_error_set(err, span, "invalid compound reader pattern instruction");
            break;
        case IDM_READER_PATTERN_REPEAT:
        case IDM_READER_PATTERN_OPTIONAL:
            if (inst->child_count != 1u || has_name || inst->has_literal) return idm_error_set(err, span, "invalid unary reader pattern instruction");
            break;
        case IDM_READER_PATTERN_CAPTURE:
            if (inst->child_count != 1u || !has_name || inst->has_literal) return idm_error_set(err, span, "invalid capture reader pattern instruction");
            break;
        case IDM_READER_PATTERN_INDENT_GT:
        case IDM_READER_PATTERN_INDENT_EQ:
            if (inst->child_count != 1u || !has_name || inst->has_literal) return idm_error_set(err, span, "invalid indentation reader pattern instruction");
            break;
        case IDM_READER_PATTERN_ADJACENT:
        case IDM_READER_PATTERN_NOT_ADJACENT:
        case IDM_READER_PATTERN_PEEK:
            if (inst->child_count != 1u || has_name || inst->has_literal) return idm_error_set(err, span, "invalid unary reader pattern instruction");
            break;
        default:
            return idm_error_set(err, span, "unknown reader pattern opcode %u", inst->op);
    }
    size_t next = pc + 1u;
    for (size_t i = 0; i < inst->child_count; i++) if (!reader_pattern_validate_at(program, next, &next, err, span)) return false;
    *out_next = next;
    return true;
}

bool idm_reader_pattern_program_validate(const IdmReaderPatternProgram *program, IdmError *err, IdmSpan span) {
    if (!program || program->count == 0 || !program->items) return idm_error_set(err, span, "reader pattern program is empty");
    size_t next = 0;
    if (!reader_pattern_validate_at(program, 0, &next, err, span)) return false;
    if (next != program->count) return idm_error_set(err, span, "reader pattern program has trailing instructions");
    return true;
}

static bool reader_ctor_validate_at(const IdmReaderCtorProgram *program, size_t pc, size_t *out_next, IdmError *err, IdmSpan span) {
    if (!program || pc >= program->count) return idm_error_set(err, span, "reader constructor program ended early");
    const IdmReaderCtorInst *inst = &program->items[pc];
    bool has_text = inst->text && inst->text[0] != '\0';
    switch ((IdmReaderCtorOp)inst->op) {
        case IDM_READER_CTOR_CAPTURE:
        case IDM_READER_CTOR_SPLICE:
        case IDM_READER_CTOR_CAPTURE_ATOM:
        case IDM_READER_CTOR_CAPTURE_WORD:
        case IDM_READER_CTOR_CAPTURE_STRING:
        case IDM_READER_CTOR_INTERP_STRING:
        case IDM_READER_CTOR_EMIT_ATOM:
        case IDM_READER_CTOR_EMIT_WORD:
        case IDM_READER_CTOR_EMIT_STRING:
            if (inst->child_count != 0 || !has_text || inst->has_literal) return idm_error_set(err, span, "invalid reader constructor text instruction");
            *out_next = pc + 1u;
            return true;
        case IDM_READER_CTOR_LITERAL:
            if (inst->child_count != 0 || has_text || !inst->has_literal) return idm_error_set(err, span, "invalid reader constructor literal instruction");
            *out_next = pc + 1u;
            return true;
        case IDM_READER_CTOR_EMIT_INT:
            if (inst->child_count != 0 || has_text || inst->has_literal) return idm_error_set(err, span, "invalid reader constructor integer instruction");
            *out_next = pc + 1u;
            return true;
        case IDM_READER_CTOR_FORM:
            if (!has_text || inst->has_literal) return idm_error_set(err, span, "invalid reader constructor form instruction");
            break;
        case IDM_READER_CTOR_LIST:
        case IDM_READER_CTOR_VECTOR:
        case IDM_READER_CTOR_TUPLE:
        case IDM_READER_CTOR_DICT:
            if (has_text || inst->has_literal) return idm_error_set(err, span, "invalid reader constructor sequence instruction");
            break;
        default:
            return idm_error_set(err, span, "unknown reader constructor opcode %u", inst->op);
    }
    size_t next = pc + 1u;
    for (size_t i = 0; i < inst->child_count; i++) if (!reader_ctor_validate_at(program, next, &next, err, span)) return false;
    *out_next = next;
    return true;
}

bool idm_reader_ctor_program_validate(const IdmReaderCtorProgram *program, IdmError *err, IdmSpan span) {
    if (!program || program->count == 0 || !program->items) return idm_error_set(err, span, "reader constructor program is empty");
    size_t next = 0;
    if (!reader_ctor_validate_at(program, 0, &next, err, span)) return false;
    if (next != program->count) return idm_error_set(err, span, "reader constructor program has trailing instructions");
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
    if (rule->terminal.kind > (uint8_t)IDM_GRAMMAR_TERMINAL_STRING) return idm_error_set(err, span, "invalid grammar terminal kind");
    if (rule->kind == IDM_GRAMMAR_RULE_TOKEN || rule->kind == IDM_GRAMMAR_RULE_SKIP) {
        if (rule->terminal.kind == IDM_GRAMMAR_TERMINAL_NONE) return idm_error_set(err, span, "grammar terminal rule artifact is missing terminal descriptor");
        if (rule->pattern.count != 0) return idm_error_set(err, span, "grammar terminal rule artifact has reduction program");
        if (!grammar_terminal_validate(rule, err, span)) return false;
    } else {
        if (rule->terminal.kind != IDM_GRAMMAR_TERMINAL_NONE) return idm_error_set(err, span, "grammar form rule artifact has terminal descriptor");
        if (!idm_reader_pattern_program_validate(&rule->pattern, err, span)) return false;
    }
    if (rule->kind == (uint8_t)IDM_GRAMMAR_RULE_SKIP) {
        if (rule->constructor.count != 0) return idm_error_set(err, span, "grammar skip rule artifact has constructor");
        return true;
    }
    return idm_reader_ctor_program_validate(&rule->constructor, err, span);
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
        if (!idm_buf_put_u8(out, m->has_default ? 1u : 0u) ||
            !idm_buf_put_u8(out, m->seen_decl ? 1u : 0u) ||
            !idm_buf_put_u8(out, m->has_dispatch ? 1u : 0u) ||
            !idm_buf_put_u32(out, m->dispatch_slot) ||
            !idm_buf_put_u8(out, m->has_default_slot ? 1u : 0u) ||
            !idm_buf_put_u32(out, m->default_slot)) return false;
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
        m->has_dispatch = r->ok && idm_rd_u8(r) != 0;
        m->dispatch_slot = r->ok ? idm_rd_u32(r) : 0;
        m->has_default_slot = r->ok && idm_rd_u8(r) != 0;
        m->default_slot = r->ok ? idm_rd_u32(r) : 0;
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

static bool put_reader_literal_at(IdmBuffer *out, const IdmReaderLiteral *literal, IdmError *err, unsigned depth) {
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "reader literal nested too deeply for artifact");
    if (!literal) return idm_error_set(err, idm_span_unknown(NULL), "reader literal is missing");
    if (!idm_buf_put_u8(out, literal->kind)) return idm_error_oom(err, idm_span_unknown(NULL));
    switch ((IdmSyntaxKind)literal->kind) {
        case IDM_SYN_NIL:
            return true;
        case IDM_SYN_WORD:
        case IDM_SYN_ATOM:
        case IDM_SYN_STRING:
            return idm_buf_put_str(out, literal->text ? literal->text : "", literal->text ? strlen(literal->text) : 0u) ? true : idm_error_oom(err, idm_span_unknown(NULL));
        case IDM_SYN_INT:
            return idm_buf_put_u64(out, (uint64_t)literal->integer) ? true : idm_error_oom(err, idm_span_unknown(NULL));
        case IDM_SYN_FLOAT: {
            uint64_t bits = 0;
            double value = literal->real;
            memcpy(&bits, &value, sizeof(bits));
            return idm_buf_put_u64(out, bits) ? true : idm_error_oom(err, idm_span_unknown(NULL));
        }
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
            if (literal->count > UINT32_MAX) return idm_error_set(err, idm_span_unknown(NULL), "reader literal has too many children");
            if (!idm_buf_put_u32(out, (uint32_t)literal->count)) return idm_error_oom(err, idm_span_unknown(NULL));
            for (size_t i = 0; i < literal->count; i++) {
                if (!put_reader_literal_at(out, &literal->items[i], err, depth + 1u)) return false;
            }
            return true;
    }
    return idm_error_set(err, idm_span_unknown(NULL), "unknown reader literal kind %u", literal->kind);
}

static bool put_reader_literal(IdmBuffer *out, const IdmReaderLiteral *literal, IdmError *err) {
    return put_reader_literal_at(out, literal, err, 0u);
}

bool idm_reader_literal_serialize(IdmBuffer *out, const IdmReaderLiteral *literal, IdmError *err) {
    return put_reader_literal(out, literal, err);
}

static bool read_reader_literal_at(IdmByteReader *r, IdmReaderLiteral *literal, IdmError *err, unsigned depth) {
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "reader literal nested too deeply in artifact");
    memset(literal, 0, sizeof(*literal));
    literal->kind = idm_rd_u8(r);
    if (!r->ok) return false;
    switch ((IdmSyntaxKind)literal->kind) {
        case IDM_SYN_NIL:
            return true;
        case IDM_SYN_WORD:
        case IDM_SYN_ATOM:
        case IDM_SYN_STRING:
            return artifact_read_str(r, &literal->text, err);
        case IDM_SYN_INT:
            literal->integer = (int64_t)idm_rd_u64(r);
            return r->ok;
        case IDM_SYN_FLOAT: {
            uint64_t bits = idm_rd_u64(r);
            if (!r->ok) return false;
            double value = 0.0;
            memcpy(&value, &bits, sizeof(value));
            literal->real = value;
            return true;
        }
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT: {
            uint32_t count = idm_rd_u32(r);
            if (!r->ok) return false;
            if ((size_t)count > r->len - r->pos) return idm_error_set(err, idm_span_unknown(NULL), "reader literal child count exceeds artifact payload");
            literal->count = count;
            if (count == 0) return true;
            literal->items = calloc(count, sizeof(*literal->items));
            if (!literal->items) return idm_error_oom(err, idm_span_unknown(NULL));
            for (uint32_t i = 0; i < count; i++) {
                if (!read_reader_literal_at(r, &literal->items[i], err, depth + 1u)) {
                    idm_reader_literal_destroy(literal);
                    return false;
                }
            }
            return true;
        }
    }
    return idm_error_set(err, idm_span_unknown(NULL), "unknown reader literal kind %u", literal->kind);
}

static bool read_reader_literal(IdmByteReader *r, IdmReaderLiteral *literal, IdmError *err) {
    return read_reader_literal_at(r, literal, err, 0u);
}

bool idm_reader_literal_deserialize(IdmByteReader *r, IdmReaderLiteral *literal, IdmError *err) {
    return read_reader_literal(r, literal, err);
}

bool idm_reader_pattern_program_serialize(IdmBuffer *out, const IdmReaderPatternProgram *program, IdmError *err) {
    if (!idm_buf_put_u32(out, (uint32_t)program->count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < program->count; i++) {
        const IdmReaderPatternInst *inst = &program->items[i];
        if (!idm_buf_put_u8(out, inst->op) ||
            !idm_buf_put_u32(out, (uint32_t)inst->child_count) ||
            !idm_buf_put_u64(out, (uint64_t)inst->capture_slot) ||
            !idm_buf_put_u8(out, inst->name ? 1u : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (inst->name && !idm_buf_put_str(out, inst->name, strlen(inst->name))) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!idm_buf_put_u8(out, inst->has_literal ? 1u : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (inst->has_literal && !put_reader_literal(out, &inst->literal, err)) return false;
    }
    return true;
}

bool idm_reader_pattern_program_deserialize(IdmByteReader *r, IdmReaderPatternProgram *program, IdmError *err) {
    uint32_t count = idm_rd_u32(r);
    if (!r->ok) return false;
    if (count == 0) return true;
    program->items = calloc(count, sizeof(*program->items));
    if (!program->items) return idm_error_oom(err, idm_span_unknown(NULL));
    program->cap = count;
    for (uint32_t i = 0; i < count; i++) {
        IdmReaderPatternInst *inst = &program->items[i];
        program->count = i + 1u;
        inst->op = idm_rd_u8(r);
        inst->child_count = idm_rd_u32(r);
        inst->capture_slot = (size_t)idm_rd_u64(r);
        uint8_t has_name = idm_rd_u8(r);
        if (!r->ok) return false;
        if (has_name && !artifact_read_str(r, &inst->name, err)) return false;
        uint8_t has_literal = idm_rd_u8(r);
        if (!r->ok) return false;
        if (has_literal) {
            inst->has_literal = true;
            if (!read_reader_literal(r, &inst->literal, err)) return false;
        }
    }
    return true;
}

bool idm_reader_ctor_program_serialize(IdmBuffer *out, const IdmReaderCtorProgram *program, IdmError *err) {
    if (!idm_buf_put_u32(out, (uint32_t)program->count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < program->count; i++) {
        const IdmReaderCtorInst *inst = &program->items[i];
        if (!idm_buf_put_u8(out, inst->op) ||
            !idm_buf_put_u32(out, (uint32_t)inst->child_count) ||
            !idm_buf_put_u64(out, (uint64_t)inst->capture_slot) ||
            !idm_buf_put_u64(out, (uint64_t)inst->integer) ||
            !idm_buf_put_u8(out, inst->text ? 1u : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (inst->text && !idm_buf_put_str(out, inst->text, strlen(inst->text))) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!idm_buf_put_u8(out, inst->has_literal ? 1u : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (inst->has_literal && !put_reader_literal(out, &inst->literal, err)) return false;
    }
    return true;
}

bool idm_reader_ctor_program_deserialize(IdmByteReader *r, IdmReaderCtorProgram *program, IdmError *err) {
    uint32_t count = idm_rd_u32(r);
    if (!r->ok) return false;
    if (count == 0) return true;
    program->items = calloc(count, sizeof(*program->items));
    if (!program->items) return idm_error_oom(err, idm_span_unknown(NULL));
    program->cap = count;
    for (uint32_t i = 0; i < count; i++) {
        IdmReaderCtorInst *inst = &program->items[i];
        program->count = i + 1u;
        inst->op = idm_rd_u8(r);
        inst->child_count = idm_rd_u32(r);
        inst->capture_slot = (size_t)idm_rd_u64(r);
        inst->integer = (int64_t)idm_rd_u64(r);
        uint8_t has_text = idm_rd_u8(r);
        if (!r->ok) return false;
        if (has_text && !artifact_read_str(r, &inst->text, err)) return false;
        uint8_t has_literal = idm_rd_u8(r);
        if (!r->ok) return false;
        if (has_literal) {
            inst->has_literal = true;
            if (!read_reader_literal(r, &inst->literal, err)) return false;
        }
    }
    return true;
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
    ok = ok && idm_buf_put_u8(out, art->source_reader ? 1u : 0u);
    if (ok && art->source_reader) ok = idm_buf_put_str(out, art->source_reader, strlen(art->source_reader));
    if (!ok) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!idm_buf_put_u32(out, art->init_fn)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!idm_buf_put_u8(out, art->module ? 1u : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (art->module && !put_module_blob(out, art->module, err)) return false;
    ok = idm_buf_put_u32(out, (uint32_t)art->export_count);
    for (size_t i = 0; ok && i < art->export_count; i++) {
        ok = idm_buf_put_str(out, art->exports[i].name, strlen(art->exports[i].name)) &&
             idm_buf_put_u32(out, art->exports[i].slot) &&
             put_arity(out, art->exports[i].arity);
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)art->slot_count);
    for (size_t i = 0; ok && i < art->slot_count; i++) {
        ok = idm_buf_put_str(out, art->slots[i].name, strlen(art->slots[i].name)) &&
             idm_buf_put_u32(out, art->slots[i].slot) &&
             put_arity(out, art->slots[i].arity) &&
             put_scope_set(out, &art->slots[i].scopes);
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
        for (size_t f = 0; ok && f < t->field_count; f++) {
            const char *contract = t->fields[f].contract ? t->fields[f].contract : "";
            ok = idm_buf_put_str(out, t->fields[f].name, strlen(t->fields[f].name)) &&
                 idm_buf_put_str(out, contract, strlen(contract));
        }
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)art->trait_count);
    for (size_t i = 0; ok && i < art->trait_count; i++) {
        const IdmPkgTrait *t = &art->traits[i];
        ok = idm_buf_put_str(out, t->name, strlen(t->name)) && idm_buf_put_str(out, t->identity, strlen(t->identity));
        ok = ok && put_requirement_defs(out, t->requirements, t->requirement_count);
        ok = ok && put_method_defs(out, t->methods, t->method_count);
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)art->method_impl_count);
    for (size_t i = 0; ok && i < art->method_impl_count; i++) {
        const IdmPkgMethodImpl *impl = &art->method_impls[i];
        ok = idm_buf_put_str(out, impl->trait, strlen(impl->trait)) &&
             idm_buf_put_str(out, impl->type, strlen(impl->type)) &&
             idm_buf_put_str(out, impl->method, strlen(impl->method)) &&
             put_arity(out, impl->arity) &&
             idm_buf_put_u8(out, impl->impl_env ? 1u : 0u) &&
             idm_buf_put_str(out, impl->impl_env_key ? impl->impl_env_key : "", strlen(impl->impl_env_key ? impl->impl_env_key : "")) &&
             idm_buf_put_u32(out, impl->impl_slot);
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)art->protocol_count);
    for (size_t i = 0; ok && i < art->protocol_count; i++) {
        const IdmPkgProtocol *p = &art->protocols[i];
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
    if (!idm_buf_put_u32(out, (uint32_t)art->core_syntax_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < art->core_syntax_count; i++) {
        const IdmPkgCoreSyntax *core_syntax = &art->core_syntax[i];
        if (!core_syntax->name || !core_syntax->module) return idm_error_set(err, idm_span_unknown(NULL), "core-syntax artifact entry is incomplete");
        if (!idm_buf_put_str(out, core_syntax->name, strlen(core_syntax->name)) ||
            !idm_buf_put_u32(out, core_syntax->function_index)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!put_module_blob(out, &core_syntax->module->module, err)) return false;
    }
    if (!grammar_artifacts_validate(art->grammars, art->grammar_count, err, idm_span_unknown(NULL)) ||
        !source_reader_artifact_validate(art->source_reader, art->grammars, art->grammar_count, err, idm_span_unknown(NULL))) return false;
    if (!idm_buf_put_u32(out, (uint32_t)art->grammar_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < art->grammar_count; i++) {
        const IdmPkgGrammar *grammar = &art->grammars[i];
        if (!idm_buf_put_str(out, grammar->name, strlen(grammar->name)) ||
            !idm_buf_put_u8(out, grammar->mode) ||
            !put_scope_set(out, &grammar->scopes) ||
            !idm_buf_put_u32(out, (uint32_t)grammar->rule_count)) return idm_error_oom(err, idm_span_unknown(NULL));
        for (size_t r = 0; r < grammar->rule_count; r++) {
            const IdmGrammarRule *rule = &grammar->rules[r];
            if (!idm_buf_put_str(out, rule->name, strlen(rule->name)) ||
                !idm_buf_put_u8(out, rule->kind) ||
                !idm_buf_put_u8(out, rule->terminal.kind)) return idm_error_oom(err, idm_span_unknown(NULL));
            if (rule->terminal.kind != IDM_GRAMMAR_TERMINAL_NONE) {
                if (!rule->terminal.text) return idm_error_set(err, idm_span_unknown(NULL), "grammar terminal descriptor is incomplete");
                if (!idm_buf_put_str(out, rule->terminal.text, strlen(rule->terminal.text)) ||
                    !idm_buf_put_u32(out, rule->terminal.flags)) return idm_error_oom(err, idm_span_unknown(NULL));
            }
            if (!idm_reader_pattern_program_serialize(out, &rule->pattern, err)) return false;
            if (!idm_reader_ctor_program_serialize(out, &rule->constructor, err)) return false;
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
    uint8_t has_source_reader = ok ? idm_rd_u8(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && has_source_reader) ok = artifact_read_str(&r, &out->source_reader, err);
    out->init_fn = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    uint8_t has_module = ok ? idm_rd_u8(&r) : 0;
    if (ok && !r.ok) ok = false;
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
    uint32_t slot_count = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && slot_count != 0) {
        out->slots = calloc(slot_count, sizeof(*out->slots));
        if (!out->slots) ok = false;
        for (uint32_t i = 0; ok && i < slot_count; i++) {
            out->slot_count = i + 1u;
            ok = artifact_read_str(&r, &out->slots[i].name, err);
            out->slots[i].slot = ok ? idm_rd_u32(&r) : 0;
            out->slots[i].arity = ok ? read_arity(&r) : idm_arity_unknown();
            if (ok && !r.ok) ok = false;
            idm_scope_set_init(&out->slots[i].scopes);
            if (ok) ok = read_scope_set(&r, &out->slots[i].scopes);
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
                    ok = artifact_read_str(&r, &t->fields[f].name, err) &&
                         artifact_read_str(&r, &t->fields[f].contract, err);
                    if (ok && t->fields[f].contract && t->fields[f].contract[0] == '\0') {
                        free(t->fields[f].contract);
                        t->fields[f].contract = NULL;
                    }
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
    uint32_t method_impl_count = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && method_impl_count != 0) {
        out->method_impls = calloc(method_impl_count, sizeof(*out->method_impls));
        if (!out->method_impls) ok = false;
        for (uint32_t i = 0; ok && i < method_impl_count; i++) {
            IdmPkgMethodImpl *impl = &out->method_impls[i];
            out->method_impl_count = i + 1u;
            ok = artifact_read_str(&r, &impl->trait, err) &&
                 artifact_read_str(&r, &impl->type, err) &&
                 artifact_read_str(&r, &impl->method, err);
            impl->arity = ok ? read_arity(&r) : idm_arity_unknown();
            impl->impl_env = ok ? idm_rd_u8(&r) != 0 : false;
            ok = ok && artifact_read_str(&r, &impl->impl_env_key, err);
            impl->impl_slot = ok ? idm_rd_u32(&r) : 0u;
            if (ok && !r.ok) ok = false;
        }
    }
    uint32_t protocol_count = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && protocol_count != 0) {
        out->protocols = calloc(protocol_count, sizeof(*out->protocols));
        if (!out->protocols) ok = false;
        for (uint32_t i = 0; ok && i < protocol_count; i++) {
            IdmPkgProtocol *p = &out->protocols[i];
            out->protocol_count = i + 1u;
            ok = artifact_read_str(&r, &p->name, err) && artifact_read_str(&r, &p->identity, err);
            uint64_t blob_len = ok ? idm_rd_u64(&r) : 0u;
            if (ok && (!r.ok || blob_len > r.len - r.pos)) ok = idm_error_set(err, idm_span_unknown(NULL), "truncated protocol artifact");
            if (ok) {
                p->art = calloc(1u, sizeof(*p->art));
                if (!p->art) ok = false;
                else if (!idm_artifact_deserialize(rt, r.data + r.pos, (size_t)blob_len, p->art, err)) ok = false;
                r.pos += (size_t)blob_len;
            }
        }
    }
    uint32_t core_syntax_count = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && core_syntax_count != 0) {
        out->core_syntax = calloc(core_syntax_count, sizeof(*out->core_syntax));
        if (!out->core_syntax) ok = false;
        for (uint32_t i = 0; ok && i < core_syntax_count; i++) {
            IdmPkgCoreSyntax *core_syntax = &out->core_syntax[i];
            out->core_syntax_count = i + 1u;
            ok = artifact_read_str(&r, &core_syntax->name, err);
            core_syntax->function_index = ok ? idm_rd_u32(&r) : 0u;
            if (ok && !r.ok) ok = false;
            if (ok) {
                core_syntax->module = idm_module_ref_create(rt);
                if (!core_syntax->module) ok = false;
                else if (!read_module_blob(rt, &r, &core_syntax->module->module, err)) ok = false;
            }
        }
    }
    uint32_t grammar_count = ok ? idm_rd_u32(&r) : 0;
    if (ok && !r.ok) ok = false;
    if (ok && grammar_count != 0) {
        out->grammars = calloc(grammar_count, sizeof(*out->grammars));
        if (!out->grammars) ok = false;
        for (uint32_t i = 0; ok && i < grammar_count; i++) {
            IdmPkgGrammar *grammar = &out->grammars[i];
            out->grammar_count = i + 1u;
            idm_scope_set_init(&grammar->scopes);
            ok = artifact_read_str(&r, &grammar->name, err);
            grammar->mode = ok ? idm_rd_u8(&r) : 0u;
            if (ok && !r.ok) ok = false;
            if (ok && grammar->mode > (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE) {
                ok = idm_error_set(err, idm_span_unknown(NULL), "invalid grammar mode in artifact");
            }
            if (ok) ok = read_scope_set(&r, &grammar->scopes);
            uint32_t rule_count = ok ? idm_rd_u32(&r) : 0u;
            if (ok && !r.ok) ok = false;
            if (ok && rule_count != 0) {
                grammar->rules = calloc(rule_count, sizeof(*grammar->rules));
                if (!grammar->rules) ok = false;
                for (uint32_t j = 0; ok && j < rule_count; j++) {
                    IdmGrammarRule *rule = &grammar->rules[j];
                    grammar->rule_count = j + 1u;
                    ok = artifact_read_str(&r, &rule->name, err);
                    rule->kind = ok ? idm_rd_u8(&r) : 0u;
                    if (ok && !r.ok) ok = false;
                    if (ok && rule->kind > (uint8_t)IDM_GRAMMAR_RULE_SKIP) {
                        ok = idm_error_set(err, idm_span_unknown(NULL), "invalid grammar rule kind in artifact");
                    }
                    rule->terminal.kind = ok ? idm_rd_u8(&r) : 0u;
                    if (ok && !r.ok) ok = false;
                    if (ok && rule->terminal.kind > (uint8_t)IDM_GRAMMAR_TERMINAL_STRING) {
                        ok = idm_error_set(err, idm_span_unknown(NULL), "invalid grammar terminal kind in artifact");
                    }
                    if (ok && rule->terminal.kind != IDM_GRAMMAR_TERMINAL_NONE) {
                        ok = artifact_read_str(&r, &rule->terminal.text, err);
                        rule->terminal.flags = ok ? idm_rd_u32(&r) : 0u;
                        if (ok && !r.ok) ok = false;
                    }
                    if (ok && (rule->kind == IDM_GRAMMAR_RULE_TOKEN || rule->kind == IDM_GRAMMAR_RULE_SKIP) && rule->terminal.kind == IDM_GRAMMAR_TERMINAL_NONE) {
                        ok = idm_error_set(err, idm_span_unknown(NULL), "grammar terminal rule artifact is missing terminal descriptor");
                    }
                    if (ok && rule->kind == IDM_GRAMMAR_RULE_FORM && rule->terminal.kind != IDM_GRAMMAR_TERMINAL_NONE) {
                        ok = idm_error_set(err, idm_span_unknown(NULL), "grammar form rule artifact has terminal descriptor");
                    }
                    if (ok) ok = idm_reader_pattern_program_deserialize(&r, &rule->pattern, err);
                    if (ok) ok = idm_reader_ctor_program_deserialize(&r, &rule->constructor, err);
                    if (ok) ok = idm_grammar_rule_validate(rule, err, idm_span_unknown(NULL));
                }
            }
            if (ok) ok = grammar_artifact_validate(grammar, err, idm_span_unknown(NULL));
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
            out->macros[i].phase_env = idm_phase_env_retain(phase_env);
        }
        for (size_t i = 0; i < out->operator_count; i++) {
            if (!out->operators[i].target_module) continue;
            out->operators[i].target_phase_env = idm_phase_env_retain(phase_env);
        }
        for (size_t i = 0; i < out->core_syntax_count; i++) {
            out->core_syntax[i].phase_env = idm_phase_env_retain(phase_env);
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

bool idm_artifact_run_phase_inits(IdmRuntime *rt, const IdmArtifact *art, IdmError *err) {
    const IdmPhaseEnv *env = art->phase_env;
    bool ok = true;
    if (env && env->module_count != 0) {
        void *old_mac_user = rt->register_macro_user;
        IdmRegisterMacroFn old_mac = rt->register_macro;
        rt->register_macro_user = NULL;
        rt->register_macro = artifact_noop_register_macro;
        for (size_t i = 0; ok && i < env->module_count; i++) {
            IdmValue ignored = idm_nil();
            ok = idm_vm_run_in_env(rt, env->modules[i], env->module_main_fns[i], env->env, &ignored, err);
        }
        rt->register_macro_user = old_mac_user;
        rt->register_macro = old_mac;
    }
    for (size_t i = 0; ok && i < art->protocol_count; i++) {
        if (art->protocols[i].art) ok = idm_artifact_run_phase_inits(rt, art->protocols[i].art, err);
    }
    return ok;
}

bool idm_artifact_cache_disabled(void) {
    const char *v = getenv("IDIOMCACHE");
    return v && strcmp(v, "0") == 0;
}

void idm_artifact_cache_write(const char *path, const IdmArtifact *art) {
    if (idm_artifact_cache_disabled()) return;
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "artifact.cache.write");
    IdmError werr;
    idm_error_init(&werr);
    IdmBuffer blob;
    idm_buf_init(&blob);
    bool ok = idm_artifact_serialize(art, &blob, &werr);
    if (ok) idm_profile_count("artifact.cache.write_bytes", (uint64_t)blob.len);
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
    idm_profile_scope_end(&prof);
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
    if (!reads) return;
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
    (void)rt;
    if (dep->kind == IDM_DEP_PACKAGE) return false;
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
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "artifact.cache.load");
    if (idm_artifact_cache_disabled()) {
        idm_profile_count("artifact.cache.disabled", 1u);
        idm_profile_scope_end(&prof);
        return false;
    }
    IdmBuffer ishc_path;
    idm_buf_init(&ishc_path);
    if (!idm_buf_append(&ishc_path, path) || !idm_buf_append(&ishc_path, ".ic")) {
        idm_buf_destroy(&ishc_path);
        idm_profile_scope_end(&prof);
        return false;
    }
    char *data = NULL;
    size_t len = 0;
    IdmError rerr;
    idm_error_init(&rerr);
    bool ok = idm_read_file(ishc_path.data, &data, &len, &rerr);
    idm_buf_destroy(&ishc_path);
    idm_error_clear(&rerr);
    if (!ok) {
        idm_profile_count("artifact.cache.miss", 1u);
        idm_profile_scope_end(&prof);
        return false;
    }
    idm_profile_count("artifact.cache.read_bytes", (uint64_t)len);
    IdmError derr;
    idm_error_init(&derr);
    ok = idm_artifact_deserialize(rt, (const unsigned char *)data, len, out, &derr);
    free(data);
    if (ok) {
        ok = memcmp(out->src_hash, src_hash, 32u) == 0;
        for (size_t i = 0; ok && i < out->dep_count; i++) {
            ok = artifact_dep_verified_with(rt, &out->deps[i], package_dep_verified, package_dep_user);
        }
        if (ok) ok = idm_artifact_run_phase_inits(rt, out, &derr);
        if (!ok) idm_artifact_destroy(out);
    }
    idm_error_clear(&derr);
    idm_profile_count(ok ? "artifact.cache.hit" : "artifact.cache.stale", 1u);
    idm_profile_scope_end(&prof);
    return ok;
}
