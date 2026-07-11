#include "internal.h"
#include "idiom/regex.h"
#include <assert.h>

static IdmPattern *pattern_from_param_depth(ExpandContext *ctx, const IdmSyntax *syn, uint32_t arg_index, bool allow_bind, IdmError *err);
static IdmPattern *pattern_from_param(ExpandContext *ctx, const IdmSyntax *syn, uint32_t arg_index, IdmError *err);
static bool copy_pattern_locals(ExpandContext *ctx, size_t table_base, IdmPatternLocal **out_locals, uint32_t *out_count);
typedef struct {
    size_t param_start;
    size_t param_end;
    bool has_guard;
    size_t guard_start;
    size_t guard_end;
    bool has_arrow;
    size_t body_start;
    size_t body_end;
    size_t body_index;
    uint32_t arity;
} FunctionClauseShape;
static bool bind_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_rhs_start);
typedef struct {
    const IdmSyntax *name;
    IdmScopeSet scopes;
    IdmCallableContract contract;
    bool used;
} BodySignature;
static bool signature_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_contract_start);
static void body_signatures_destroy(BodySignature *signatures, size_t count);
static bool body_signature_add(ExpandContext *ctx, BodySignature **signatures, size_t *count, size_t *cap, const IdmSyntax *form, IdmError *err);
static bool body_signature_take_contract(const IdmSyntax *name, const IdmScopeSet *scopes, BodySignature *signatures, size_t signature_count, IdmCallableContract *out, bool *out_has, IdmError *err);
static bool defn_group_take_signature(DefnGroup *group, BodySignature *signatures, size_t signature_count, IdmError *err);
static bool body_signatures_all_used(const BodySignature *signatures, size_t count, IdmError *err);
static bool definition_like_form(const IdmSyntax *form, const char **out_head);
static bool phase_eval_body(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *phase_body, IdmError *err);
static bool defn_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_param_start, bool *out_export);
static bool expand_core_grammar_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err);
static bool expand_surface_grammar_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err);
static void defn_groups_destroy(DefnGroup *groups, size_t count);
static DefnGroup *find_or_add_group(DefnGroup **groups, size_t *count, size_t *cap, const IdmSyntax *name_syntax);
static bool group_add_index(DefnGroup *group, size_t index);
static void body_recs_destroy(BodyRec *recs, size_t count);
static bool body_recs_schedule(ExpandContext *ctx, BodyRec *recs, size_t count, size_t **out_order, IdmError *err);
static bool body_work_splice(IdmSyntax ***work, size_t *work_count, size_t *work_cap, size_t at, const IdmSyntax *body, IdmError *err);
size_t phase_candidate_next_run(ExpandContext *ctx) {
    return ++ctx->phase_candidate_run_seq;
}

bool phase_candidate_stage(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, IdmSyntax *const *items, size_t count, size_t run_id, IdmError *err) {
    if (ctx->phase != 0 || ctx->frame != IDM_FRAME_TOP || count == 0) return true;
    if (ctx->phase_candidate_count == ctx->phase_candidate_cap) {
        if (!idm_grow((void **)&ctx->phase_candidates, &ctx->phase_candidate_cap, sizeof(*ctx->phase_candidates), 8u, ctx->phase_candidate_count + 1u)) return idm_error_oom(err, name_syntax->span);
    }
    PhaseCandidate *cand = &ctx->phase_candidates[ctx->phase_candidate_count];
    memset(cand, 0, sizeof(*cand));
    cand->name = idm_strdup(name);
    if (!cand->name) return idm_error_oom(err, name_syntax->span);
    if (!binder_scopes_pruned(ctx, name_syntax, &cand->scopes)) {
        free(cand->name);
        return idm_error_oom(err, name_syntax->span);
    }
    cand->items = calloc(count, sizeof(*cand->items));
    if (!cand->items) {
        free(cand->name);
        idm_scope_set_destroy(&cand->scopes);
        return idm_error_oom(err, name_syntax->span);
    }
    for (size_t i = 0; i < count; i++) {
        cand->items[i] = idm_syn_clone(items[i]);
        if (!cand->items[i]) {
            for (size_t k = 0; k < i; k++) idm_syn_free(cand->items[k]);
            free(cand->items);
            free(cand->name);
            idm_scope_set_destroy(&cand->scopes);
            return idm_error_oom(err, name_syntax->span);
        }
        cand->count = i + 1u;
    }
    cand->run_id = run_id;
    cand->arity = idm_arity_unknown();
    ctx->phase_candidate_count++;
    return true;
}

static bool phase_candidate_record(ExpandContext *ctx, PhaseCandidate *cand, size_t table_mark, IdmError *err) {
    const IdmBinding *binding = NULL;
    for (size_t i = ctx->bindings.count; i > table_mark; i--) {
        const IdmBinding *b = &ctx->bindings.items[i - 1u];
        if (b->phase == 1 && b->space == IDM_BIND_SPACE_DEFAULT && b->kind == IDM_BIND_ENV && strcmp(b->name, cand->name) == 0) {
            binding = b;
            break;
        }
    }
    if (!binding) {
        return idm_error_set(err, cand->items[0]->span, "phase-1 compilation of '%s' did not produce a binding", cand->name);
    }
    cand->slot = binding->payload;
    cand->arity = binding->arity;
    if (binding->has_contract && idm_callable_contract_copy(&cand->contract, &binding->contract)) cand->has_contract = true;
    cand->done = true;
    return true;
}

static bool phase_candidate_compile(ExpandContext *ctx, PhaseCandidate *cand, IdmError *err) {
    IdmSpan span = cand->items[0]->span;
    IdmSyntax *body = idm_syn_list(span);
    IdmSyntax *head = idm_syn_word("%-body", span);
    bool ok = body && head && idm_syn_append(body, head);
    if (!ok) {
        if (!body) idm_syn_free(head);
        idm_syn_free(body);
        return idm_error_oom(err, span);
    }
    for (size_t i = 0; ok && i < cand->count; i++) {
        IdmSyntax *item = idm_syn_clone(cand->items[i]);
        ok = item != NULL && idm_syn_append(body, item);
        if (!ok) idm_syn_free(item);
    }
    if (!ok) {
        idm_syn_free(body);
        return idm_error_oom(err, span);
    }
    SavedFunctionContext saved;
    begin_function_context(ctx, &saved);
    ctx->frame = IDM_FRAME_TOP;
    ctx->enclosing = NULL;
    size_t table_mark = ctx->bindings.count;
    bool ran = phase_eval_body(ctx, body, body, err);
    size_t run = cand->run_id;
    for (size_t i = 0; ran && i < ctx->phase_candidate_count; i++) {
        PhaseCandidate *peer = &ctx->phase_candidates[i];
        if (peer->run_id == run && !peer->done) ran = phase_candidate_record(ctx, peer, table_mark, err);
    }
    end_function_context(ctx, &saved);
    idm_syn_free(body);
    return ran;
}

static bool phase_candidate_bind(ExpandContext *ctx, const PhaseCandidate *cand, IdmError *err) {
    IdmBindingId binding_id = 0;
    if (!idm_binding_table_add_with_arity(&ctx->bindings, cand->name, 1, IDM_BIND_SPACE_DEFAULT, IDM_BIND_ENV, &cand->scopes, cand->slot, IDM_FRAME_ENV, cand->arity, &binding_id)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (cand->has_contract && !idm_binding_table_set_contract(&ctx->bindings, binding_id, &cand->contract)) return idm_error_oom(err, idm_span_unknown(NULL));
    return true;
}

static bool body_existing_env_binding(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, const IdmBinding **out_binding);

bool phase_demand_local(ExpandContext *ctx, const IdmSyntax *word, bool *out_compiled, IdmError *err) {
    *out_compiled = false;
    if (ctx->phase != 1) return true;
    const IdmScopeSet *ref = idm_syn_scope_set(word, 0);
    for (size_t i = ctx->phase_candidate_count; i > 0; i--) {
        PhaseCandidate *cand = &ctx->phase_candidates[i - 1u];
        if (strcmp(cand->name, word->as.text) != 0) continue;
        if (!idm_scope_set_subset(&cand->scopes, ref ? ref : &ctx->empty_scopes)) continue;
        if (cand->in_progress) return idm_error_set(err, word->span, "phase-1 compilation of '%s' depends on itself across statements; declare the cycle adjacently", cand->name);
        if (cand->done && body_existing_env_binding(ctx, cand->name, &cand->scopes, NULL)) {
            *out_compiled = true;
            return true;
        }
        if (!cand->done) {
            cand->in_progress = true;
            bool ok = phase_candidate_compile(ctx, cand, err);
            cand->in_progress = false;
            if (!ok) return false;
        }
        if (!phase_candidate_bind(ctx, cand, err)) return false;
        *out_compiled = true;
        return true;
    }
    const IdmBinding *runtime = NULL;
    IdmResolveStatus status = idm_binding_resolve(&ctx->bindings, word->as.text, 0, IDM_BIND_SPACE_DEFAULT, ref ? ref : &ctx->empty_scopes, &runtime);
    if (status == IDM_RESOLVE_OK && runtime && runtime->kind == IDM_BIND_PACKAGE_SLOT && !(runtime->has_contract && runtime->contract.passthrough)) {
        IdmScopeSet scopes;
        if (!idm_scope_set_copy(&scopes, &runtime->scopes)) return idm_error_oom(err, word->span);
        uint32_t payload = runtime->payload;
        IdmArity arity = runtime->arity;
        bool has_contract = runtime->has_contract;
        IdmCallableContract contract;
        memset(&contract, 0, sizeof(contract));
        if (has_contract && !idm_callable_contract_copy(&contract, &runtime->contract)) {
            idm_scope_set_destroy(&scopes);
            return idm_error_oom(err, word->span);
        }
        IdmBindingId binding_id = 0;
        bool ok = idm_binding_table_add_with_arity(&ctx->bindings, word->as.text, 1, IDM_BIND_SPACE_DEFAULT, IDM_BIND_PACKAGE_SLOT, &scopes, payload, IDM_FRAME_ENV, arity, &binding_id);
        if (ok && has_contract) ok = idm_binding_table_set_contract(&ctx->bindings, binding_id, &contract);
        idm_scope_set_destroy(&scopes);
        if (has_contract) idm_callable_contract_destroy(&contract);
        if (!ok) return idm_error_oom(err, word->span);
        if (payload < ctx->package_slot_ref_count && ctx->package_slot_refs[payload].env_key) {
            const IdmArtifact *provider = expand_cache_artifact_by_key(ctx->rt, ctx->package_slot_refs[payload].env_key);
            if (provider && !record_runtime_init(ctx, provider, word->span, err)) return false;
        }
        *out_compiled = true;
        return true;
    }
    return true;
}

static bool owned_syntax_push(IdmSyntax ***owned, size_t *count, size_t *cap, IdmSyntax *syn, IdmError *err);
static bool body_install_expanded_syntax(IdmSyntax ***work, size_t *work_count, size_t *work_cap, IdmSyntax ***owned, size_t *owned_count, size_t *owned_cap, IdmScopeId extra_scope, size_t index, IdmSyntax *expanded, IdmError *err);
static bool body_scope_add_range(ExpandContext *ctx, IdmSyntax **work, size_t start, size_t count, IdmScopeId scope, IdmSpan span, IdmError *err);
static bool body_definition_scope_add(ExpandContext *ctx, IdmSyntax **work, size_t start, size_t count, IdmScopeId scope, IdmSpan span, IdmError *err);
static bool use_selection_parse(const IdmSyntax *form, size_t start, size_t end, size_t *out_pos, UseSelection *selection, IdmError *err);
static void use_selection_destroy(UseSelection *selection);
static bool parse_function_clause_shape(ExpandContext *ctx, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, bool allow_bodyless, FunctionClauseShape *out, IdmError *err);
static bool function_parts_arity(ExpandContext *ctx, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmArity *out_arity, IdmError *err);
static bool defn_group_arity(ExpandContext *ctx, const DefnGroup *group, IdmSyntax *const *items, IdmArity *out_arity, IdmError *err);
static IdmCore *expand_function_clause(ExpandContext *ctx, const char *debug_name, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmPattern ***out_patterns, uint32_t *out_pattern_count, IdmPatternLocal **out_locals, uint32_t *out_local_count, IdmCore **out_guard, uint32_t *out_arity, IdmError *err);
static bool defn_form_clause_block(const IdmSyntax *def_form, size_t param_start, const IdmSyntax **out_body);
static IdmCore *expand_defn_group(ExpandContext *ctx, const DefnGroup *group, IdmSyntax *const *items, IdmError *err);
static IdmCore *expand_match_clause(ExpandContext *ctx, const IdmSyntax *clause, IdmPattern ***out_patterns, uint32_t *out_pattern_count, IdmPatternLocal **out_locals, uint32_t *out_local_count, IdmCore **out_guard, uint32_t *out_arity, IdmError *err);
static bool body_is_clauses(const IdmSyntax *body, bool allow_empty_pattern);
static IdmCore *build_clause_fn_styled(ExpandContext *ctx, const IdmSyntax *body, size_t clause_end, const char *debug_name, bool defn_style, const IdmCallableContract *contract, IdmError *err);
static IdmCore *build_clause_fn(ExpandContext *ctx, const IdmSyntax *body, size_t clause_end, const char *debug_name, IdmError *err);

IdmCore *literal_from_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    IdmValue value = idm_nil();
    if (!value_from_literal_syntax(ctx, syn, &value, err)) return NULL;
    return idm_core_literal(value, syn->span);
}

bool value_from_literal_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmValue *out, IdmError *err) {
    switch (syn->kind) {
        case IDM_SYN_NIL:
            *out = idm_nil();
            return true;
        case IDM_SYN_INT:
            *out = idm_int_promote(ctx->rt, syn->as.integer, err);
            return !(err && err->present);
        case IDM_SYN_BIGINT: {
            bool ok = false;
            *out = idm_int_from_decimal(ctx->rt, syn->as.text, strlen(syn->as.text), &ok, err);
            if (err && err->present) return false;
            if (!ok) return idm_error_set(err, syn->span, "invalid integer literal");
            return true;
        }
        case IDM_SYN_FLOAT:
            *out = idm_float(ctx->rt, syn->as.real, err);
            return !(err && err->present);
        case IDM_SYN_ATOM:
            if (strcmp(syn->as.text, "nil") == 0) {
                *out = idm_nil();
                return true;
            }
            *out = idm_atom(ctx->rt, syn->as.text);
            return true;
        case IDM_SYN_STRING:
            *out = idm_string(ctx->rt, syn->as.text, err);
            return !(err && err->present);
        case IDM_SYN_LIST: {
            IdmValue list = idm_empty_list();
            for (size_t i = syn->as.seq.count; i > 0; i--) {
                IdmValue item = idm_nil();
                if (!value_from_literal_syntax(ctx, syn->as.seq.items[i - 1u], &item, err)) return false;
                list = idm_cons(ctx->rt, item, list, err);
                if (err && err->present) return false;
            }
            *out = list;
            return true;
        }
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE: {
            IdmValue *items = NULL;
            if (syn->as.seq.count != 0) {
                items = calloc(syn->as.seq.count, sizeof(*items));
                if (!items) {
                    idm_error_oom(err, syn->span);
                    return false;
                }
            }
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (!value_from_literal_syntax(ctx, syn->as.seq.items[i], &items[i], err)) {
                    free(items);
                    return false;
                }
            }
            *out = syn->kind == IDM_SYN_VECTOR ? idm_vector(ctx->rt, items, syn->as.seq.count, err) : idm_tuple(ctx->rt, items, syn->as.seq.count, err);
            free(items);
            return !(err && err->present);
        }
        case IDM_SYN_DICT: {
            size_t elem_count = syn->as.seq.count;
            size_t tail_index = SIZE_MAX;
            if (!dict_rest_index(syn, &tail_index, err)) return false;
            IdmValue tail = idm_nil();
            if (tail_index != SIZE_MAX) {
                if (tail_index + 2u != syn->as.seq.count) return false;
                if (!value_from_literal_syntax(ctx, syn->as.seq.items[tail_index + 1u], &tail, err)) return false;
                if (!idm_is_dict(tail)) return false;
                elem_count = tail_index;
            }
            if (elem_count % 2u != 0) {
                idm_error_set(err, syn->span, "dict literal requires key/value pairs");
                return false;
            }
            size_t pair_count = elem_count / 2u;
            size_t tail_count = tail_index == SIZE_MAX ? 0u : idm_dict_count(tail);
            size_t count = tail_count + pair_count;
            IdmDictEntry *entries = count == 0 ? NULL : calloc(count, sizeof(*entries));
            if (count != 0 && !entries) {
                idm_error_oom(err, syn->span);
                return false;
            }
            IdmDictIter tail_it;
            idm_dict_iter_init(tail, &tail_it);
            for (size_t i = 0; i < tail_count; i++) {
                if (!idm_dict_iter_next(&tail_it, &entries[i].key, &entries[i].value)) {
                    free(entries);
                    return idm_error_set(err, syn->span, "dict rest literal conversion failed");
                }
            }
            for (size_t i = 0; i < pair_count; i++) {
                size_t out_i = tail_count + i;
                if (!value_from_literal_syntax(ctx, syn->as.seq.items[i * 2u], &entries[out_i].key, err) ||
                    !value_from_literal_syntax(ctx, syn->as.seq.items[i * 2u + 1u], &entries[out_i].value, err)) {
                    free(entries);
                    return false;
                }
            }
            *out = idm_dict(ctx->rt, entries, count, err);
            free(entries);
            return !(err && err->present);
        }
        default:
            return false;
    }
}

static bool pattern_binder_note(ExpandContext *ctx, const IdmSyntax *syn) {
    if (!ctx->pat_binder_collect) return true;
    if (ctx->pat_binder_count == ctx->pat_binder_cap) {
        if (!idm_grow((void **)&ctx->pat_binders, &ctx->pat_binder_cap, sizeof(*ctx->pat_binders), 4u, ctx->pat_binder_count + 1u)) return false;
    }
    ctx->pat_binders[ctx->pat_binder_count++] = syn;
    return true;
}

static bool syntax_pattern_note_local(ExpandContext *ctx, const IdmSyntax *name, IdmError *err) {
    const IdmBinding *existing = resolve_default(ctx, name, NULL);
    bool have = existing && existing->kind == IDM_BIND_LOCAL && existing->frame_id == ctx->frame;
    if (!have && (!local_push_scoped(ctx, name->as.text, name, NULL) || !pattern_binder_note(ctx, name))) {
        idm_error_oom(err, name->span);
        return false;
    }
    return true;
}

static const IdmTypeTerm *current_function_arg_type(const ExpandContext *ctx, uint32_t arg_index) {
    const IdmCallableContract *contract = ctx->function_contract;
    if (!contract || arg_index >= contract->sigs[0].arg_count) return NULL;
    return &contract->sigs[0].args[arg_index];
}

static IdmSyntaxPattern *syntax_pattern_hole(ExpandContext *ctx, const IdmSyntax *hole, IdmError *err) {
    if (!idm_syn_is_form_id(hole, IDM_FORM_UNSYNTAX) || hole->as.seq.count != 2) {
        idm_error_set(err, hole->span, "syntax pattern hole expects %,name");
        return NULL;
    }
    const IdmSyntax *name = hole->as.seq.items[1];
    if (name->kind != IDM_SYN_WORD) {
        idm_error_set(err, hole->span, "syntax pattern hole must bind an identifier");
        return NULL;
    }
    if (strcmp(name->as.text, "_") == 0) return idm_syn_pat_wildcard(hole->span);
    if (!syntax_pattern_note_local(ctx, name, err)) return NULL;
    IdmSyntaxPattern *pat = idm_syn_pat_bind(name->as.text, hole->span);
    if (!pat) idm_error_oom(err, hole->span);
    return pat;
}

static const IdmSyntax *syntax_pattern_splice_form(const IdmSyntax *item) {
    if (idm_syn_is_form_id(item, IDM_FORM_EXPR) && item->as.seq.count == 2 && idm_syn_is_form_id(item->as.seq.items[1], IDM_FORM_UNSYNTAX_SPLICING)) {
        return item->as.seq.items[1];
    }
    return item;
}

static bool syntax_pattern_rest_name(ExpandContext *ctx, const IdmSyntax *splice, const char **out_name, IdmError *err) {
    *out_name = NULL;
    if (!idm_syn_is_form_id(splice, IDM_FORM_UNSYNTAX_SPLICING) || splice->as.seq.count != 2) {
        idm_error_set(err, splice->span, "syntax rest pattern expects %,@name");
        return false;
    }
    const IdmSyntax *name = splice->as.seq.items[1];
    if (name->kind != IDM_SYN_WORD) {
        idm_error_set(err, splice->span, "syntax rest pattern must bind an identifier");
        return false;
    }
    if (strcmp(name->as.text, "_") == 0) return true;
    if (!syntax_pattern_note_local(ctx, name, err)) return false;
    *out_name = name->as.text;
    return true;
}

static IdmSyntaxPattern *syntax_pattern_from_template(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);

static IdmSyntaxPattern *syntax_pattern_sequence(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    IdmSyntaxPattern **items = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t rest_index = IDM_SYN_PAT_NO_REST;
    const char *rest_name = NULL;
    for (size_t i = 0; i < syn->as.seq.count; i++) {
        const IdmSyntax *splice = syntax_pattern_splice_form(syn->as.seq.items[i]);
        if (idm_syn_is_form_id(splice, IDM_FORM_UNSYNTAX_SPLICING)) {
            if (rest_index != IDM_SYN_PAT_NO_REST) {
                idm_error_set(err, splice->span, "syntax pattern may contain only one rest hole");
                goto fail;
            }
            rest_index = count;
            if (!syntax_pattern_rest_name(ctx, splice, &rest_name, err)) goto fail;
            continue;
        }
        IdmSyntaxPattern *item = syntax_pattern_from_template(ctx, syn->as.seq.items[i], err);
        if (!item) goto fail;
        if (count == cap) {
            if (!idm_grow((void **)&items, &cap, sizeof(*items), 8u, count + 1u)) {
                idm_syn_pat_free(item);
                idm_error_oom(err, syn->span);
                goto fail;
            }
        }
        items[count++] = item;
    }
    IdmSyntaxPattern *pat = idm_syn_pat_sequence(syn->kind, items, count, rest_index, rest_name, syn->span);
    if (!pat) {
        idm_error_oom(err, syn->span);
        goto fail;
    }
    return pat;

fail:
    for (size_t i = 0; i < count; i++) idm_syn_pat_free(items[i]);
    free(items);
    return NULL;
}

static IdmSyntaxPattern *syntax_pattern_from_template(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if ((idm_syn_is_form_id(syn, IDM_FORM_GROUP) || idm_syn_is_form_id(syn, IDM_FORM_LAYOUT_GROUP)) && syn->as.seq.count == 2) return syntax_pattern_from_template(ctx, syn->as.seq.items[1], err);
    if (idm_syn_is_form_id(syn, IDM_FORM_UNSYNTAX)) return syntax_pattern_hole(ctx, syn, err);
    if (idm_syn_is_form_id(syn, IDM_FORM_UNSYNTAX_SPLICING)) {
        idm_error_set(err, syn->span, "syntax rest pattern is only valid inside sequence patterns");
        return NULL;
    }
    if (syn->kind == IDM_SYN_WORD && strcmp(syn->as.text, "_") == 0) return idm_syn_pat_wildcard(syn->span);
    if (syn->kind == IDM_SYN_LIST || syn->kind == IDM_SYN_VECTOR || syn->kind == IDM_SYN_TUPLE || syn->kind == IDM_SYN_DICT) {
        return syntax_pattern_sequence(ctx, syn, err);
    }
    IdmSyntax *literal = idm_syn_clone(syn);
    if (!literal) {
        idm_error_oom(err, syn->span);
        return NULL;
    }
    IdmSyntaxPattern *pat = idm_syn_pat_literal_take(literal, syn->span);
    if (!pat) {
        idm_syn_free(literal);
        idm_error_oom(err, syn->span);
    }
    return pat;
}

static IdmPattern *pattern_from_quasisyntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (!idm_syn_is_form_id(syn, IDM_FORM_QUASISYNTAX) || syn->as.seq.count != 2) {
        idm_error_set(err, syn->span, "%-quasisyntax pattern expects one child");
        return NULL;
    }
    IdmSyntaxPattern *syntax = syntax_pattern_from_template(ctx, syn->as.seq.items[1], err);
    if (!syntax) return NULL;
    IdmPattern *pat = idm_pat_syntax_take(syntax, syn->span);
    if (!pat) {
        idm_syn_pat_free(syntax);
        idm_error_oom(err, syn->span);
    }
    return pat;
}

static bool quoted_list_pattern_items(const IdmSyntax *syn, IdmSyntax *const **out_items, size_t *out_count, IdmSpan *out_span) {
    if (!idm_syn_is_form_id(syn, IDM_FORM_QUOTE) || syn->as.seq.count != 2) return false;
    const IdmSyntax *quoted = syn->as.seq.items[1];
    if (quoted->kind == IDM_SYN_LIST && quoted->as.seq.count == 0) {
        *out_items = NULL;
        *out_count = 0;
        *out_span = quoted->span;
        return true;
    }
    if ((idm_syn_is_form_id(quoted, IDM_FORM_GROUP) || idm_syn_is_form_id(quoted, IDM_FORM_LAYOUT_GROUP)) && quoted->as.seq.count == 2) {
        const IdmSyntax *expr = quoted->as.seq.items[1];
        if (idm_syn_is_form_id(expr, IDM_FORM_EXPR)) {
            *out_items = expr->as.seq.items + 1;
            *out_count = expr->as.seq.count - 1u;
            *out_span = quoted->span;
            return true;
        }
    }
    return false;
}

static IdmPattern *pattern_from_list_items(ExpandContext *ctx, IdmSyntax *const *items, size_t count, IdmSpan span, IdmError *err) {
    size_t dot_index = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        if (items[i]->kind == IDM_SYN_WORD && strcmp(items[i]->as.text, ".") == 0) {
            if (dot_index != SIZE_MAX) {
                idm_error_set(err, items[i]->span, "list rest pattern may contain only one dot");
                return NULL;
            }
            dot_index = i;
        }
    }
    if (dot_index != SIZE_MAX) {
        if (dot_index == 0 || dot_index + 2u != count) {
            idm_error_set(err, span, "list rest pattern must have form '(head ... . rest)");
            return NULL;
        }
        IdmPattern *tail = pattern_from_param_depth(ctx, items[dot_index + 1u], (uint32_t)(dot_index + 1u), false, err);
        if (!tail) return NULL;
        for (size_t i = dot_index; i > 0; i--) {
            IdmPattern *head = pattern_from_param_depth(ctx, items[i - 1u], (uint32_t)(i - 1u), false, err);
            if (!head) {
                idm_pat_free(tail);
                return NULL;
            }
            tail = idm_pat_pair(head, tail, span);
            if (!tail) {
                idm_pat_free(head);
                return (IdmPattern *)(uintptr_t)idm_error_oom(err, span);
            }
        }
        return tail;
    }
    IdmPattern **pats = NULL;
    if (count != 0) {
        pats = calloc(count, sizeof(*pats));
        if (!pats) {
            idm_error_oom(err, span);
            return NULL;
        }
    }
    for (size_t i = 0; i < count; i++) {
        pats[i] = pattern_from_param_depth(ctx, items[i], (uint32_t)i, false, err);
        if (!pats[i]) {
            for (size_t j = 0; j < i; j++) idm_pat_free(pats[j]);
            free(pats);
            return NULL;
        }
    }
    IdmPattern *pat = idm_pat_sequence(IDM_PAT_LIST, pats, count, span);
    if (!pat) {
        for (size_t i = 0; i < count; i++) idm_pat_free(pats[i]);
        free(pats);
        idm_error_oom(err, span);
        return NULL;
    }
    return pat;
}

static bool binding_is_current_clause(const ExpandContext *ctx, const IdmBinding *binding) {
    for (size_t i = ctx->clause_table_base; i < ctx->bindings.count; i++) {
        if (&ctx->bindings.items[i] == binding) return true;
    }
    return false;
}

static bool pin_collect_add(ExpandContext *ctx, const IdmSyntax *target, const char **out_name, IdmError *err) {
    PinCollect *pins = ctx->pin_collect;
    if (!pins) {
        return idm_error_set(err, target->span, "pin ^%s targets an enclosing binding; that is supported in match clauses and '=' bindings — in a fn, defn, or receive head bind a name and guard: when (eq? x %s)", target->as.text, target->as.text);
    }
    if (pins->count == pins->cap) {
        IdmGrowItem items[3] = {
            { (void **)&pins->outer, sizeof(*pins->outer) },
            { (void **)&pins->fresh, sizeof(*pins->fresh) },
            { (void **)&pins->clause, sizeof(*pins->clause) },
        };
        if (!idm_growv(items, 3u, &pins->cap, 4u, pins->count + 1u)) return idm_error_oom(err, target->span);
    }
    char fresh[32];
    snprintf(fresh, sizeof fresh, "%%pin.value.%u", ctx->pin_value_seq++);
    IdmSyntax *outer = idm_syn_clone(target);
    char *fresh_copy = outer ? idm_strdup(fresh) : NULL;
    if (!outer || !fresh_copy) {
        idm_syn_free(outer);
        free(fresh_copy);
        return idm_error_oom(err, target->span);
    }
    pins->outer[pins->count] = outer;
    pins->fresh[pins->count] = fresh_copy;
    pins->clause[pins->count] = ctx->match_size_clause_index;
    pins->count++;
    *out_name = fresh_copy;
    return true;
}

static bool pattern_pin_name(ExpandContext *ctx, const IdmSyntax *target, const char **out_name, IdmError *err) {
    *out_name = target->as.text;
    const IdmBinding *binding = resolve_default(ctx, target, NULL);
    if (binding && binding_is_current_clause(ctx, binding)) return true;
    return pin_collect_add(ctx, target, out_name, err);
}

static IdmPattern *pattern_from_param_depth(ExpandContext *ctx, const IdmSyntax *syn, uint32_t arg_index, bool allow_bind, IdmError *err) {
    if (syn->kind == IDM_SYN_WORD) {
        if (strcmp(syn->as.text, "_") == 0) return idm_pat_wildcard(syn->span);
        if (!allow_bind) {
            const IdmBinding *existing = resolve_default(ctx, syn, NULL);
            bool have = existing && existing->kind == IDM_BIND_LOCAL && existing->frame_id == ctx->frame;
            if (!have && (!local_push_scoped(ctx, syn->as.text, syn, NULL) || !pattern_binder_note(ctx, syn))) {
                idm_error_oom(err, syn->span);
                return NULL;
            }
            return idm_pat_bind(syn->as.text, syn->span);
        }
        const IdmBinding *existing = resolve_default(ctx, syn, NULL);
        bool have = existing && existing->kind == IDM_BIND_ARG && existing->frame_id == ctx->frame;
        if (!have && (!arg_push_slot_with_type(ctx, syn, arg_index, current_function_arg_type(ctx, arg_index)) || !pattern_binder_note(ctx, syn))) {
            idm_error_oom(err, syn->span);
            return NULL;
        }
        if (have && current_function_arg_type(ctx, arg_index) && !arg_push_slot_with_type(ctx, syn, arg_index, current_function_arg_type(ctx, arg_index))) {
            idm_error_oom(err, syn->span);
            return NULL;
        }
        return idm_pat_bind(syn->as.text, syn->span);
    }
    IdmSyntax *const *quoted_items = NULL;
    size_t quoted_count = 0;
    IdmSpan quoted_span = syn->span;
    if (quoted_list_pattern_items(syn, &quoted_items, &quoted_count, &quoted_span)) {
        return pattern_from_list_items(ctx, quoted_items, quoted_count, quoted_span, err);
    }
    if (idm_syn_is_form_id(syn, IDM_FORM_QUOTE) && syn->as.seq.count == 2) {
        IdmValue datum = idm_nil();
        if (!expand_quote_datum(ctx, syn->as.seq.items[1], &datum, err)) return NULL;
        return idm_pat_literal(datum, syn->span);
    }
    if (idm_syn_is_form_id(syn, IDM_FORM_PIN) && syn->as.seq.count == 2) {
        const IdmSyntax *target = syn->as.seq.items[1];
        if (target->kind != IDM_SYN_WORD) {
            idm_error_set(err, syn->span, "pin pattern currently supports only a variable: ^name");
            return NULL;
        }
        const char *name = NULL;
        if (!pattern_pin_name(ctx, target, &name, err)) return NULL;
        return idm_pat_pin(name, syn->span);
    }
    if (idm_syn_is_form_id(syn, IDM_FORM_QUASISYNTAX)) return pattern_from_quasisyntax(ctx, syn, err);
    if (idm_syn_is_form_id(syn, IDM_FORM_BITSTRING)) return pattern_from_bitstring(ctx, syn, err);
    if (idm_syn_is_form_id(syn, IDM_FORM_GROUP) && syn->as.seq.count == 2u) {
        const IdmSyntax *inner = syn->as.seq.items[1];
        if (idm_syn_is_form_id(inner, IDM_FORM_EXPR) && inner->as.seq.count == 4u &&
            syn_is_word(inner->as.seq.items[2], "::") &&
            inner->as.seq.items[3]->kind == IDM_SYN_WORD) {
            const IdmSyntax *type_word = inner->as.seq.items[3];
            if (!idm_type_name_is_builtin(type_word->as.text)) {
                const IdmBinding *type_binding = NULL;
                IdmResolveStatus status = resolve_scoped(ctx, type_word->as.text, IDM_BIND_SPACE_TYPE, idm_syn_scope_set(type_word, 0), NULL, &type_binding);
                if (status != IDM_RESOLVE_OK) {
                    idm_error_set(err, type_word->span, "unknown type '%s' in pattern", type_word->as.text);
                    return NULL;
                }
            }
            IdmPattern *sub = pattern_from_param_depth(ctx, inner->as.seq.items[1], arg_index, allow_bind, err);
            if (!sub) return NULL;
            IdmPattern *pat = idm_pat_type_sub_take(type_word->as.text, sub, syn->span);
            if (!pat) {
                idm_error_oom(err, syn->span);
                return NULL;
            }
            return pat;
        }
    }
    if (syn->kind == IDM_SYN_LIST) {
        idm_error_set(err, syn->span, "unsupported pattern");
        return NULL;
    }
    IdmValue literal = idm_nil();
    if (syn->kind != IDM_SYN_DICT && value_from_literal_syntax(ctx, syn, &literal, err)) return idm_pat_literal(literal, syn->span);
    if (syn->kind == IDM_SYN_VECTOR || syn->kind == IDM_SYN_TUPLE) {
        {
            size_t dot_index = SIZE_MAX;
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (syn->as.seq.items[i]->kind == IDM_SYN_WORD && strcmp(syn->as.seq.items[i]->as.text, ".") == 0) {
                    if (dot_index != SIZE_MAX) {
                        idm_error_set(err, syn->as.seq.items[i]->span, "sequence rest pattern may contain only one dot");
                        return NULL;
                    }
                    dot_index = i;
                }
            }
            if (dot_index != SIZE_MAX) {
                if (dot_index == 0 || dot_index + 2u != syn->as.seq.count) {
                    idm_error_set(err, syn->span, "sequence rest pattern must have form [head ... . rest] or {head ... . rest}");
                    return NULL;
                }
                IdmPattern **items = calloc(dot_index, sizeof(*items));
                if (!items) {
                    idm_error_oom(err, syn->span);
                    return NULL;
                }
                for (size_t i = 0; i < dot_index; i++) {
                    items[i] = pattern_from_param_depth(ctx, syn->as.seq.items[i], (uint32_t)i, false, err);
                    if (!items[i]) {
                        for (size_t j = 0; j < i; j++) idm_pat_free(items[j]);
                        free(items);
                        return NULL;
                    }
                }
                IdmPattern *rest = pattern_from_param_depth(ctx, syn->as.seq.items[dot_index + 1u], (uint32_t)(dot_index + 1u), false, err);
                if (!rest) {
                    for (size_t i = 0; i < dot_index; i++) idm_pat_free(items[i]);
                    free(items);
                    return NULL;
                }
                IdmPatternKind kind = syn->kind == IDM_SYN_VECTOR ? IDM_PAT_VECTOR_REST : IDM_PAT_TUPLE_REST;
                IdmPattern *pat = idm_pat_sequence_rest(kind, items, dot_index, rest, syn->span);
                if (!pat) {
                    for (size_t i = 0; i < dot_index; i++) idm_pat_free(items[i]);
                    free(items);
                    idm_pat_free(rest);
                    idm_error_oom(err, syn->span);
                    return NULL;
                }
                return pat;
            }
        }
        IdmPattern **items = NULL;
        if (syn->as.seq.count != 0) {
            items = calloc(syn->as.seq.count, sizeof(*items));
            if (!items) {
                idm_error_oom(err, syn->span);
                return NULL;
            }
        }
        for (size_t i = 0; i < syn->as.seq.count; i++) {
            items[i] = pattern_from_param_depth(ctx, syn->as.seq.items[i], (uint32_t)i, false, err);
            if (!items[i]) {
                for (size_t j = 0; j < i; j++) idm_pat_free(items[j]);
                free(items);
                return NULL;
            }
        }
        IdmPatternKind kind = syn->kind == IDM_SYN_VECTOR ? IDM_PAT_VECTOR : IDM_PAT_TUPLE;
        IdmPattern *pat = idm_pat_sequence(kind, items, syn->as.seq.count, syn->span);
        if (!pat) {
            for (size_t i = 0; i < syn->as.seq.count; i++) idm_pat_free(items[i]);
            free(items);
            idm_error_oom(err, syn->span);
            return NULL;
        }
        return pat;
    }
    if (syn->kind == IDM_SYN_DICT) {
        size_t elem_count = syn->as.seq.count;
        size_t dot_index = SIZE_MAX;
        for (size_t i = 0; i < elem_count; i++) {
            if (syn->as.seq.items[i]->kind == IDM_SYN_WORD && strcmp(syn->as.seq.items[i]->as.text, ".") == 0) {
                if (dot_index != SIZE_MAX) {
                    idm_error_set(err, syn->as.seq.items[i]->span, "dict rest pattern may contain only one dot");
                    return NULL;
                }
                dot_index = i;
            }
        }
        IdmPattern *rest = NULL;
        if (dot_index != SIZE_MAX) {
            if ((dot_index % 2u) != 0 || dot_index + 2u != elem_count) {
                idm_error_set(err, syn->span, "dict rest pattern must have form %{key value ... . rest}");
                return NULL;
            }
            rest = pattern_from_param_depth(ctx, syn->as.seq.items[dot_index + 1u], (uint32_t)(dot_index / 2u), false, err);
            if (!rest) return NULL;
            elem_count = dot_index;
        }
        if (elem_count % 2u != 0) {
            idm_pat_free(rest);
            idm_error_set(err, syn->span, "dict pattern requires key/value pairs");
            return NULL;
        }
        size_t count = elem_count / 2u;
        IdmDictPatternEntry *entries = count == 0 ? NULL : calloc(count, sizeof(*entries));
        if (count != 0 && !entries) {
            idm_pat_free(rest);
            idm_error_oom(err, syn->span);
            return NULL;
        }
        for (size_t i = 0; i < count; i++) {
            if (!value_from_literal_syntax(ctx, syn->as.seq.items[i * 2u], &entries[i].key, err)) {
                for (size_t j = 0; j < i; j++) idm_pat_free(entries[j].pattern);
                free(entries);
                idm_pat_free(rest);
                if (!err->present) idm_error_set(err, syn->as.seq.items[i * 2u]->span, "dict pattern key must be a literal; a computed key binds with ^name in a '=' binding");
                return NULL;
            }
            entries[i].pattern = pattern_from_param_depth(ctx, syn->as.seq.items[i * 2u + 1u], (uint32_t)i, false, err);
            if (!entries[i].pattern) {
                for (size_t j = 0; j < i; j++) idm_pat_free(entries[j].pattern);
                free(entries);
                idm_pat_free(rest);
                return NULL;
            }
        }
        IdmPattern *pat = idm_pat_dict(entries, count, rest, syn->span);
        if (!pat) {
            for (size_t i = 0; i < count; i++) idm_pat_free(entries[i].pattern);
            free(entries);
            idm_pat_free(rest);
            idm_error_oom(err, syn->span);
            return NULL;
        }
        return pat;
    }
    idm_error_set(err, syn->span, "unsupported pattern");
    return NULL;
}

static IdmPattern *pattern_from_param(ExpandContext *ctx, const IdmSyntax *syn, uint32_t arg_index, IdmError *err) {
    return pattern_from_param_depth(ctx, syn, arg_index, true, err);
}

static bool bitseg_opt(const IdmSyntax *seg, size_t opt_start, const char *name) {
    for (size_t i = opt_start; i < seg->as.seq.count; i++) {
        const IdmSyntax *o = seg->as.seq.items[i];
        if (o->kind == IDM_SYN_ATOM && o->as.text && strcmp(o->as.text, name) == 0) return true;
    }
    return false;
}

static bool bitseg_parse_opts(const IdmSyntax *seg, size_t opt_start, IdmBitSeg *out, IdmError *err) {
    for (size_t i = opt_start; i < seg->as.seq.count; i++) {
        const IdmSyntax *o = seg->as.seq.items[i];
        if (o->kind != IDM_SYN_ATOM || !o->as.text) return idm_error_set(err, o->span, "bitstring segment options must be atoms");
        if (strcmp(o->as.text, "little") == 0) out->little = true;
        else if (strcmp(o->as.text, "big") == 0) out->little = false;
        else if (strcmp(o->as.text, "signed") == 0) out->is_signed = true;
        else if (strcmp(o->as.text, "unsigned") == 0) out->is_signed = false;
        else if (strcmp(o->as.text, "float") == 0 || strcmp(o->as.text, "bits") == 0) continue;
        else return idm_error_set(err, o->span, "unknown bitstring option '%s'", o->as.text);
    }
    return true;
}

static bool bits_binder_lookup(const ExpandContext *ctx, size_t base, const char *name, uint32_t *out_slot) {
    for (size_t i = base; i < ctx->bits_binder_count; i++) {
        if (strcmp(ctx->bits_binder_names[i], name) == 0) {
            *out_slot = ctx->bits_binder_slots[i];
            return true;
        }
    }
    return false;
}

static bool bits_binder_push(ExpandContext *ctx, const char *name, uint32_t slot, IdmError *err, IdmSpan span) {
    if (ctx->bits_binder_count == ctx->bits_binder_cap) {
        IdmGrowItem items[2] = {
            { (void **)&ctx->bits_binder_names, sizeof(*ctx->bits_binder_names) },
            { (void **)&ctx->bits_binder_slots, sizeof(*ctx->bits_binder_slots) },
        };
        if (!idm_growv(items, 2u, &ctx->bits_binder_cap, 4u, ctx->bits_binder_count + 1u)) return idm_error_oom(err, span);
    }
    char *copy = idm_strdup(name);
    if (!copy) return idm_error_oom(err, span);
    ctx->bits_binder_names[ctx->bits_binder_count] = copy;
    ctx->bits_binder_slots[ctx->bits_binder_count] = slot;
    ctx->bits_binder_count++;
    return true;
}

static bool bitseg_sub(ExpandContext *ctx, size_t binder_base, const IdmSyntax *value, IdmBitSeg *seg, IdmError *err) {
    if (value->kind == IDM_SYN_WORD) {
        if (strcmp(value->as.text, "_") == 0) {
            seg->sub = idm_pat_wildcard(value->span);
            return seg->sub != NULL || idm_error_oom(err, value->span);
        }
        uint32_t dup_slot = 0;
        if (bits_binder_lookup(ctx, binder_base, value->as.text, &dup_slot)) {
            seg->sub = idm_pat_pin(value->as.text, value->span);
            return seg->sub != NULL || idm_error_oom(err, value->span);
        }
        uint32_t slot = 0;
        if (!local_push_scoped(ctx, value->as.text, value, &slot) || !pattern_binder_note(ctx, value)) return idm_error_oom(err, value->span);
        if (!bits_binder_push(ctx, value->as.text, slot, err, value->span)) return false;
        seg->sub = idm_pat_bind(value->as.text, value->span);
        return seg->sub != NULL || idm_error_oom(err, value->span);
    }
    if (seg->kind == IDM_BITSEG_REST || seg->kind == IDM_BITSEG_BITS) {
        return idm_error_set(err, value->span, "bitstring rest pattern must bind a variable or '_'");
    }
    if (value->kind == IDM_SYN_INT || value->kind == IDM_SYN_FLOAT) {
        IdmValue literal = idm_nil();
        if (!value_from_literal_syntax(ctx, value, &literal, err)) {
            if (err && !err->present) idm_error_set(err, value->span, "bitstring pattern segment must be a binder or a literal");
            return false;
        }
        seg->sub = idm_pat_literal(literal, value->span);
        return seg->sub != NULL || idm_error_oom(err, value->span);
    }
    return idm_error_set(err, value->span, "bitstring pattern segment must be a binder or a literal");
}

static const IdmSyntax *bits_size_unwrap(const IdmSyntax *syn) {
    while (syn && (idm_syn_is_form_id(syn, IDM_FORM_GROUP) || idm_syn_is_form_id(syn, IDM_FORM_LAYOUT_GROUP)) && syn->as.seq.count == 2) syn = syn->as.seq.items[1];
    return syn;
}

static bool bits_size_binder(ExpandContext *ctx, size_t binder_base, const IdmSyntax *word, IdmBitSeg *seg) {
    uint32_t slot = 0;
    if (bits_binder_lookup(ctx, binder_base, word->as.text, &slot)) {
        seg->size_is_slot = true;
        seg->size_slot = slot;
        return true;
    }
    const IdmBinding *existing = resolve_default(ctx, word, NULL);
    if (existing && existing->frame_id == ctx->frame && existing->kind == IDM_BIND_ARG) {
        seg->size_is_slot = true;
        seg->size_slot = UINT32_MAX;
        seg->size_name = idm_strdup(word->as.text);
        return seg->size_name != NULL;
    }
    if (existing && existing->frame_id == ctx->frame && existing->kind == IDM_BIND_LOCAL) {
        for (size_t i = ctx->clause_table_base; i < ctx->bindings.count; i++) {
            if (&ctx->bindings.items[i] == existing) {
                seg->size_is_slot = true;
                seg->size_slot = existing->payload;
                return true;
            }
        }
    }
    return false;
}

static bool bits_size_outer(ExpandContext *ctx, const IdmSyntax *size_syn, IdmBitSeg *seg, IdmError *err) {
    if (ctx->match_size_depth != 0) {
        char fresh[32];
        snprintf(fresh, sizeof fresh, "%%bits.size.%u", ctx->bits_size_seq++);
        IdmSyntax *word_syn = idm_syn_word(fresh, size_syn->span);
        if (!word_syn) return idm_error_oom(err, size_syn->span);
        uint32_t fresh_slot = 0;
        bool ok = local_push_scoped(ctx, fresh, word_syn, &fresh_slot);
        idm_syn_free(word_syn);
        if (!ok) return idm_error_oom(err, size_syn->span);
        if (ctx->match_size_count == ctx->match_size_cap) {
            IdmGrowItem items[4] = {
                { (void **)&ctx->match_size_outer, sizeof(*ctx->match_size_outer) },
                { (void **)&ctx->match_size_fresh, sizeof(*ctx->match_size_fresh) },
                { (void **)&ctx->match_size_slots, sizeof(*ctx->match_size_slots) },
                { (void **)&ctx->match_size_clause, sizeof(*ctx->match_size_clause) },
            };
            if (!idm_growv(items, 4u, &ctx->match_size_cap, 4u, ctx->match_size_count + 1u)) return idm_error_oom(err, size_syn->span);
        }
        char *fresh_copy = idm_strdup(fresh);
        if (!fresh_copy) return idm_error_oom(err, size_syn->span);
        ctx->match_size_outer[ctx->match_size_count] = idm_syn_clone(size_syn);
        if (!ctx->match_size_outer[ctx->match_size_count]) { free(fresh_copy); return idm_error_oom(err, size_syn->span); }
        ctx->match_size_fresh[ctx->match_size_count] = fresh_copy;
        ctx->match_size_slots[ctx->match_size_count] = fresh_slot;
        ctx->match_size_clause[ctx->match_size_count] = ctx->match_size_clause_index;
        ctx->match_size_count++;
        seg->size_is_slot = true;
        seg->size_slot = fresh_slot;
        return true;
    }
    IdmCore *probe = expand_syntax(ctx, size_syn, err);
    if (!probe) return false;
    idm_core_free(probe);
    return idm_error_set(err, size_syn->span, "bitstring segment size here must be an integer literal, a pattern binder, or BINDER * LITERAL");
}

static bool bitseg_parse_size(ExpandContext *ctx, size_t binder_base, const IdmSyntax *size_syn, IdmBitSeg *seg, IdmError *err) {
    const IdmSyntax *core = bits_size_unwrap(size_syn);
    if (core->kind == IDM_SYN_INT) {
        if (core->as.integer < 0) return idm_error_set(err, core->span, "bitstring segment size must be non-negative");
        seg->size = (uint64_t)core->as.integer;
        return true;
    }
    seg->size = 1u;
    if (core->kind == IDM_SYN_WORD && bits_size_binder(ctx, binder_base, core, seg)) return true;
    if (idm_syn_is_form_id(core, IDM_FORM_EXPR) && core->as.seq.count == 4 && syn_is_word(core->as.seq.items[2], "*")) {
        const IdmSyntax *a = bits_size_unwrap(core->as.seq.items[1]);
        const IdmSyntax *b = bits_size_unwrap(core->as.seq.items[3]);
        const IdmSyntax *word = a->kind == IDM_SYN_WORD ? a : b->kind == IDM_SYN_WORD ? b : NULL;
        const IdmSyntax *unit = a->kind == IDM_SYN_INT ? a : b->kind == IDM_SYN_INT ? b : NULL;
        if (word && unit && unit->as.integer > 0 && bits_size_binder(ctx, binder_base, word, seg)) {
            seg->size = (uint64_t)unit->as.integer;
            return true;
        }
        seg->size = 1u;
    }
    return bits_size_outer(ctx, core, seg, err);
}

static void bits_segs_free(IdmBitSeg *segs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        idm_pat_free(segs[i].sub);
        free(segs[i].size_name);
    }
    free(segs);
}

IdmPattern *pattern_from_bitstring(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    size_t binder_base = ctx->bits_binder_count;
    IdmBitSeg *segs = NULL;
    size_t count = 0, cap = 0;
    bool saw_rest = false;
    for (size_t i = 1; i < syn->as.seq.count; i++) {
        const IdmSyntax *segf = syn->as.seq.items[i];
        if (saw_rest) {
            bits_segs_free(segs, count);
            return (IdmPattern *)(uintptr_t)expand_error(err, segf->span, "bitstring rest segment must be last");
        }
        IdmBitSeg s;
        memset(&s, 0, sizeof s);
        s.size_slot = UINT32_MAX;
        const IdmSyntax *value_syn = segf->as.seq.items[1];
        bool ok = true;
        if (idm_syn_is_form_id(segf, IDM_FORM_BITREST)) {
            s.kind = IDM_BITSEG_REST;
            saw_rest = true;
            ok = bitseg_sub(ctx, binder_base, value_syn, &s, err);
        } else if (idm_syn_is_form_id(segf, IDM_FORM_BITSEG_BARE)) {
            if (bitseg_opt(segf, 2u, "float")) ok = expand_error(err, segf->span, "bitstring float segment requires a size");
            else if (bitseg_opt(segf, 2u, "bits")) ok = expand_error(err, segf->span, "bitstring pattern segment requires a size");
            else if (value_syn->kind == IDM_SYN_STRING) {
                s.kind = IDM_BITSEG_BITS;
                s.size = 8u * (uint64_t)strlen(value_syn->as.text);
                IdmValue bits = idm_bits_from_bytes(ctx->rt, (const unsigned char *)value_syn->as.text, s.size, err);
                ok = !(err && err->present);
                if (ok) {
                    s.sub = idm_pat_literal(bits, value_syn->span);
                    ok = s.sub != NULL || idm_error_oom(err, value_syn->span);
                }
            } else {
                s.kind = IDM_BITSEG_INT;
                s.size = 8u;
                ok = bitseg_parse_opts(segf, 2u, &s, err) && bitseg_sub(ctx, binder_base, value_syn, &s, err);
            }
        } else {
            s.kind = bitseg_opt(segf, 3u, "float") ? IDM_BITSEG_FLOAT : bitseg_opt(segf, 3u, "bits") ? IDM_BITSEG_BITS : IDM_BITSEG_INT;
            ok = bitseg_parse_opts(segf, 3u, &s, err) &&
                 bitseg_parse_size(ctx, binder_base, segf->as.seq.items[2], &s, err) &&
                 bitseg_sub(ctx, binder_base, value_syn, &s, err);
            if (ok && s.kind == IDM_BITSEG_FLOAT && !s.size_is_slot && s.size != 32u && s.size != 64u) {
                ok = expand_error(err, segf->span, "bitstring float segment size must be 32 or 64");
            }
        }
        if (!ok) {
            idm_pat_free(s.sub);
            free(s.size_name);
            bits_segs_free(segs, count);
            return NULL;
        }
        if (count == cap && !idm_grow((void **)&segs, &cap, sizeof(*segs), 4u, count + 1u)) {
            idm_pat_free(s.sub);
            free(s.size_name);
            bits_segs_free(segs, count);
            return (IdmPattern *)(uintptr_t)idm_error_oom(err, syn->span);
        }
        segs[count++] = s;
    }
    IdmPattern *pat = idm_pat_bits_take(segs, count, syn->span);
    if (!pat) {
        bits_segs_free(segs, count);
        idm_error_oom(err, syn->span);
    }
    return pat;
}

static bool copy_pattern_locals(ExpandContext *ctx, size_t table_base, IdmPatternLocal **out_locals, uint32_t *out_count) {
    const IdmBindingTable *table = &ctx->bindings;
    size_t total = 0;
    for (size_t i = table_base; i < table->count; i++) {
        if (table->items[i].kind == IDM_BIND_LOCAL && table->items[i].frame_id == ctx->frame) total++;
    }
    if (total == 0) {
        *out_locals = NULL;
        *out_count = 0;
        return true;
    }
    IdmPatternLocal *locals = calloc(total, sizeof(*locals));
    if (!locals) return false;
    size_t n = 0;
    for (size_t i = table_base; i < table->count; i++) {
        if (table->items[i].kind != IDM_BIND_LOCAL || table->items[i].frame_id != ctx->frame) continue;
        locals[n].name = idm_strdup(table->items[i].name);
        if (!locals[n].name) {
            for (size_t j = 0; j < n; j++) free(locals[j].name);
            free(locals);
            return false;
        }
        locals[n].slot = table->items[i].payload;
        n++;
    }
    *out_locals = locals;
    *out_count = (uint32_t)total;
    return true;
}

static size_t syn_pattern_bind_count(const IdmSyntaxPattern *pat, const char *name) {
    if (!pat) return 0;
    switch (pat->kind) {
        case IDM_SYN_PAT_BIND:
            return strcmp(pat->as.bind.name, name) == 0 ? 1u : 0u;
        case IDM_SYN_PAT_SEQUENCE: {
            size_t n = pat->as.seq.rest_name && strcmp(pat->as.seq.rest_name, name) == 0 ? 1u : 0u;
            for (size_t i = 0; i < pat->as.seq.count; i++) n += syn_pattern_bind_count(pat->as.seq.items[i], name);
            return n;
        }
        default:
            return 0;
    }
}

static size_t pattern_bind_count(const IdmPattern *pat, const char *name) {
    if (!pat) return 0;
    switch (pat->kind) {
        case IDM_PAT_BIND:
            return strcmp(pat->as.name, name) == 0 ? 1u : 0u;
        case IDM_PAT_PAIR:
            return pattern_bind_count(pat->as.pair.left, name) + pattern_bind_count(pat->as.pair.right, name);
        case IDM_PAT_LIST:
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE: {
            size_t n = 0;
            for (size_t i = 0; i < pat->as.seq.count; i++) n += pattern_bind_count(pat->as.seq.items[i], name);
            return n;
        }
        case IDM_PAT_VECTOR_REST:
        case IDM_PAT_TUPLE_REST: {
            size_t n = pattern_bind_count(pat->as.seq_rest.rest, name);
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) n += pattern_bind_count(pat->as.seq_rest.items[i], name);
            return n;
        }
        case IDM_PAT_DICT: {
            size_t n = pattern_bind_count(pat->as.dict.rest, name);
            for (size_t i = 0; i < pat->as.dict.count; i++) n += pattern_bind_count(pat->as.dict.entries[i].pattern, name);
            return n;
        }
        case IDM_PAT_SYNTAX:
            return syn_pattern_bind_count(pat->as.syntax, name);
        case IDM_PAT_BITS: {
            size_t n = 0;
            for (size_t i = 0; i < pat->as.bits.count; i++) n += pattern_bind_count(pat->as.bits.segs[i].sub, name);
            return n;
        }
        case IDM_PAT_TYPE:
            return pattern_bind_count(pat->as.type_test.sub, name);
        default:
            return 0;
    }
}

static bool pattern_uses_name(const IdmPattern *pat, const char *name, uint32_t slot) {
    if (!pat) return false;
    switch (pat->kind) {
        case IDM_PAT_PIN:
            return strcmp(pat->as.name, name) == 0;
        case IDM_PAT_PAIR:
            return pattern_uses_name(pat->as.pair.left, name, slot) || pattern_uses_name(pat->as.pair.right, name, slot);
        case IDM_PAT_LIST:
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE:
            for (size_t i = 0; i < pat->as.seq.count; i++) {
                if (pattern_uses_name(pat->as.seq.items[i], name, slot)) return true;
            }
            return false;
        case IDM_PAT_VECTOR_REST:
        case IDM_PAT_TUPLE_REST:
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) {
                if (pattern_uses_name(pat->as.seq_rest.items[i], name, slot)) return true;
            }
            return pattern_uses_name(pat->as.seq_rest.rest, name, slot);
        case IDM_PAT_DICT:
            for (size_t i = 0; i < pat->as.dict.count; i++) {
                if (pattern_uses_name(pat->as.dict.entries[i].pattern, name, slot)) return true;
            }
            return pattern_uses_name(pat->as.dict.rest, name, slot);
        case IDM_PAT_BITS:
            for (size_t i = 0; i < pat->as.bits.count; i++) {
                const IdmBitSeg *seg = &pat->as.bits.segs[i];
                if (seg->size_name && strcmp(seg->size_name, name) == 0) return true;
                if (seg->size_is_slot && !seg->size_name && seg->size_slot == slot) return true;
                if (pattern_uses_name(seg->sub, name, slot)) return true;
            }
            return false;
        case IDM_PAT_TYPE:
            return pattern_uses_name(pat->as.type_test.sub, name, slot);
        default:
            return false;
    }
}

static bool pattern_lower_bind(IdmPattern *pat, const char *name) {
    if (!pat) return false;
    switch (pat->kind) {
        case IDM_PAT_BIND:
            if (strcmp(pat->as.name, name) != 0) return false;
            free(pat->as.name);
            pat->as.name = NULL;
            pat->kind = IDM_PAT_WILDCARD;
            return true;
        case IDM_PAT_PAIR:
            return pattern_lower_bind(pat->as.pair.left, name) || pattern_lower_bind(pat->as.pair.right, name);
        case IDM_PAT_LIST:
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE:
            for (size_t i = 0; i < pat->as.seq.count; i++) {
                if (pattern_lower_bind(pat->as.seq.items[i], name)) return true;
            }
            return false;
        case IDM_PAT_VECTOR_REST:
        case IDM_PAT_TUPLE_REST:
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) {
                if (pattern_lower_bind(pat->as.seq_rest.items[i], name)) return true;
            }
            return pattern_lower_bind(pat->as.seq_rest.rest, name);
        case IDM_PAT_DICT:
            for (size_t i = 0; i < pat->as.dict.count; i++) {
                if (pattern_lower_bind(pat->as.dict.entries[i].pattern, name)) return true;
            }
            return pattern_lower_bind(pat->as.dict.rest, name);
        case IDM_PAT_BITS:
            for (size_t i = 0; i < pat->as.bits.count; i++) {
                if (pattern_lower_bind(pat->as.bits.segs[i].sub, name)) return true;
            }
            return false;
        case IDM_PAT_TYPE:
            return pattern_lower_bind(pat->as.type_test.sub, name);
        default:
            return false;
    }
}

static bool clause_bind_unreferenced(const ExpandContext *ctx, size_t table_base, const char *name) {
    const IdmBindingTable *table = &ctx->bindings;
    for (size_t i = table->count; i > table_base; i--) {
        const IdmBinding *b = &table->items[i - 1u];
        if (b->kind != IDM_BIND_LOCAL || b->frame_id != ctx->frame || strcmp(b->name, name) != 0) continue;
        return !b->referenced;
    }
    return false;
}

static void prune_unused_clause_binds(ExpandContext *ctx, size_t table_base, IdmPattern **patterns, size_t pattern_count, IdmPatternLocal *locals, uint32_t *local_count) {
    uint32_t n = *local_count;
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; i++) {
        IdmPatternLocal *local = &locals[i];
        bool prune = clause_bind_unreferenced(ctx, table_base, local->name);
        if (prune) {
            size_t binds = 0;
            bool used = false;
            for (size_t p = 0; p < pattern_count && !used; p++) {
                binds += pattern_bind_count(patterns[p], local->name);
                used = pattern_uses_name(patterns[p], local->name, local->slot);
            }
            prune = binds == 1u && !used;
        }
        if (prune) {
            bool lowered = false;
            for (size_t p = 0; p < pattern_count && !lowered; p++) lowered = pattern_lower_bind(patterns[p], local->name);
            if (lowered) {
                free(local->name);
                continue;
            }
        }
        locals[w++] = locals[i];
    }
    *local_count = w;
}

static bool bind_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_rhs_start) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR)) return false;
    if (form->as.seq.count < 4) return false;
    if (form->as.seq.items[1]->kind != IDM_SYN_WORD) return false;
    if (!syn_is_word(form->as.seq.items[2], "=")) return false;
    if (form->as.seq.count > 4 && form->as.seq.items[2]->token_adjacent_previous && form->as.seq.items[3]->token_adjacent_previous) return false;
    *out_name = form->as.seq.items[1];
    *out_rhs_start = 3;
    return true;
}

static bool signature_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_contract_start) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR)) return false;
    if (form->as.seq.count < 5u) return false;
    if (!syn_is_word(form->as.seq.items[1], "spec")) return false;
    if (form->as.seq.items[2]->kind != IDM_SYN_WORD) return false;
    const char *op = surface_token_text(form->as.seq.items[3]);
    if (!op || strcmp(op, "::") != 0) return false;
    if (out_name) *out_name = form->as.seq.items[2];
    if (out_contract_start) *out_contract_start = 4u;
    return true;
}

static void body_signatures_destroy(BodySignature *signatures, size_t count) {
    for (size_t i = 0; i < count; i++) {
        idm_scope_set_destroy(&signatures[i].scopes);
        idm_callable_contract_destroy(&signatures[i].contract);
    }
    free(signatures);
}

static bool body_signature_add(ExpandContext *ctx, BodySignature **signatures, size_t *count, size_t *cap, const IdmSyntax *form, IdmError *err) {
    const IdmSyntax *name = NULL;
    size_t contract_start = 0;
    if (!signature_form_parts(form, &name, &contract_start)) return true;
    IdmScopeSet scopes;
    idm_scope_set_init(&scopes);
    if (!syntax_scopes_copy(&scopes, name)) return idm_error_oom(err, name->span);
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*signatures)[i].name->as.text, name->as.text) == 0 && idm_scope_set_equal(&(*signatures)[i].scopes, &scopes)) {
            idm_scope_set_destroy(&scopes);
            IdmCallableContract extra;
            if (!parse_callable_contract_parts(ctx, (IdmSyntax *const *)form->as.seq.items, contract_start, form->as.seq.count, &extra, err)) return false;
            IdmCallableContract *base = &(*signatures)[i].contract;
            bool ok = true;
            for (size_t k = 0; ok && k < extra.sig_count; k++) {
                if (idm_contract_sig_for(base, extra.sigs[k].arg_count)) {
                    ok = idm_error_set(err, name->span, "duplicate signature arity %zu for '%s'", extra.sigs[k].arg_count, name->as.text);
                    break;
                }
                IdmContractSig *dst = idm_contract_add_sig(base);
                if (!dst || !idm_contract_sig_copy(dst, &extra.sigs[k])) ok = idm_error_oom(err, name->span);
            }
            for (size_t k = 0; ok && k < extra.quantified_count; k++) {
                bool seen = false;
                for (size_t q = 0; q < base->quantified_count; q++) {
                    if (base->quantified[q] && extra.quantified[k] && strcmp(base->quantified[q], extra.quantified[k]) == 0) { seen = true; break; }
                }
                if (seen) continue;
                if (!idm_grow((void **)&base->quantified, &base->quantified_cap, sizeof(*base->quantified), 4u, base->quantified_count + 1u)) { ok = idm_error_oom(err, name->span); break; }
                base->quantified[base->quantified_count] = idm_strdup(extra.quantified[k] ? extra.quantified[k] : "");
                if (!base->quantified[base->quantified_count]) { ok = idm_error_oom(err, name->span); break; }
                base->quantified_count++;
            }
            for (size_t k = 0; ok && k < extra.context_count; k++) {
                bool seen = false;
                for (size_t q = 0; q < base->context_count; q++) {
                    const IdmConstraint *bc = &base->context[q];
                    const IdmConstraint *ec = &extra.context[k];
                    if (bc->kind == ec->kind && bc->trait == ec->trait && idm_type_term_equal(&bc->lhs, &ec->lhs)) { seen = true; break; }
                }
                if (seen) continue;
                if (!idm_grow((void **)&base->context, &base->context_cap, sizeof(*base->context), 4u, base->context_count + 1u)) { ok = idm_error_oom(err, name->span); break; }
                memset(&base->context[base->context_count], 0, sizeof(base->context[base->context_count]));
                if (!idm_constraint_copy(&base->context[base->context_count], &extra.context[k])) { ok = idm_error_oom(err, name->span); break; }
                base->context_count++;
            }
            idm_callable_contract_destroy(&extra);
            return ok;
        }
    }
    if (*count == *cap) {
        if (!idm_grow((void **)signatures, cap, sizeof(**signatures), 4u, *count + 1u)) {
            idm_scope_set_destroy(&scopes);
            return idm_error_oom(err, name->span);
        }
    }
    BodySignature *sig = &(*signatures)[(*count)++];
    memset(sig, 0, sizeof(*sig));
    sig->name = name;
    sig->scopes = scopes;
    if (!parse_callable_contract_parts(ctx, (IdmSyntax *const *)form->as.seq.items, contract_start, form->as.seq.count, &sig->contract, err)) {
        idm_scope_set_destroy(&sig->scopes);
        (*count)--;
        return false;
    }
    return true;
}

static bool defn_group_take_signature(DefnGroup *group, BodySignature *signatures, size_t signature_count, IdmError *err) {
    if (group->has_contract) return true;
    return body_signature_take_contract(group->name_syntax, &group->scopes, signatures, signature_count, &group->contract, &group->has_contract, err);
}

static bool body_signatures_all_used(const BodySignature *signatures, size_t count, IdmError *err) {
    for (size_t i = 0; i < count; i++) {
        if (!signatures[i].used) return idm_error_set(err, signatures[i].name->span, "signature for '%s' has no definition", signatures[i].name->as.text);
    }
    return true;
}

static bool body_signature_take_contract(const IdmSyntax *name, const IdmScopeSet *scopes, BodySignature *signatures, size_t signature_count, IdmCallableContract *out, bool *out_has, IdmError *err) {
    *out_has = false;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < signature_count; i++) {
        BodySignature *sig = &signatures[i];
        if (strcmp(sig->name->as.text, name->as.text) != 0 || !idm_scope_set_subset(&sig->scopes, scopes)) continue;
        if (sig->used) return idm_error_set(err, name->span, "duplicate signature use for '%s'", name->as.text);
        if (!idm_callable_contract_copy(out, &sig->contract)) return idm_error_oom(err, name->span);
        sig->used = true;
        *out_has = true;
        return true;
    }
    return true;
}

static bool contract_arity_expected(const IdmCallableContract *contract, IdmArity *out) {
    *out = idm_arity_unknown();
    for (size_t i = 0; i < contract->sig_count; i++) {
        if (contract->sigs[i].arg_count > UINT32_MAX) return false;
        if (!idm_arity_add_exact(out, (uint32_t)contract->sigs[i].arg_count)) return false;
    }
    return true;
}

static bool contract_arity_describe_exact(IdmBuffer *buf, const IdmCallableContract *contract) {
    bool first = true;
    for (size_t i = 0; i < contract->sig_count; i++) {
        bool duplicate = false;
        for (size_t j = 0; j < i; j++) {
            if (contract->sigs[j].arg_count == contract->sigs[i].arg_count) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        if (!first && !idm_buf_append_char(buf, '|')) return false;
        if (!idm_buf_appendf(buf, "%zu", contract->sigs[i].arg_count)) return false;
        first = false;
    }
    return !first || idm_buf_append(buf, "<none>");
}

static bool callable_contract_apply_value_arity(const IdmSyntax *name, const IdmCallableContract *contract, IdmArity *arity, IdmError *err) {
    if (!contract || contract->sig_count == 0) return true;
    if (contract->sig_count == 1 && contract->sigs[0].arg_count == 0u) return true;
    IdmArity expected;
    if (!contract_arity_expected(contract, &expected)) {
        return idm_error_set(err, name->span, "signature for '%s' has too many arguments", name->as.text);
    }
    if (arity->kind == IDM_ARITY_UNKNOWN) {
        *arity = expected;
        return true;
    }
    if (idm_arity_equal(arity, &expected)) return true;
    bool covered = true;
    for (size_t i = 0; covered && i < contract->sig_count; i++) {
        covered = idm_arity_accepts(arity, (uint32_t)contract->sigs[i].arg_count);
    }
    if (covered) return true;
    IdmBuffer want;
    IdmBuffer got;
    idm_buf_init(&want);
    idm_buf_init(&got);
    bool ok = idm_arity_describe(&want, &expected) && idm_arity_describe(&got, arity);
    if (!ok) {
        idm_buf_destroy(&want);
        idm_buf_destroy(&got);
        return idm_error_oom(err, name->span);
    }
    bool set = idm_error_set(err, name->span, "signature for '%s' expects arity %s, value has %s", name->as.text, want.data ? want.data : "?", got.data ? got.data : "?");
    idm_buf_destroy(&want);
    idm_buf_destroy(&got);
    return set;
}

static bool pattern_bind_form_parts(const IdmSyntax *form, const IdmSyntax **out_pattern, size_t *out_rhs_start) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR)) return false;
    if (form->as.seq.count < 4) return false;
    const IdmSyntax *lhs = form->as.seq.items[1];
    if (lhs->kind == IDM_SYN_WORD) return false;
    if (lhs->kind == IDM_SYN_LIST && !idm_syn_is_form_id(lhs, IDM_FORM_QUOTE) && !idm_syn_is_form_id(lhs, IDM_FORM_QUASISYNTAX) && !idm_syn_is_form_id(lhs, IDM_FORM_BITSTRING)) {
        return false;
    }
    if (!syn_is_word(form->as.seq.items[2], "=")) return false;
    if (form->as.seq.count > 4 && form->as.seq.items[2]->token_adjacent_previous && form->as.seq.items[3]->token_adjacent_previous) return false;
    *out_pattern = lhs;
    *out_rhs_start = 3;
    return true;
}

static bool body_bind_is_env(ExpandContext *ctx) {
    return ctx->frame == IDM_FRAME_TOP && (ctx->repl_env_binds || ctx->in_package);
}

static bool dict_syntax_pin_parts(const IdmSyntax *syn, size_t *out_entries, size_t *out_dot, bool *out_has_pin) {
    if (syn->kind != IDM_SYN_DICT) return false;
    size_t count = syn->as.seq.count;
    size_t dot = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        if (syn->as.seq.items[i]->kind == IDM_SYN_WORD && strcmp(syn->as.seq.items[i]->as.text, ".") == 0) {
            if (dot != SIZE_MAX) return false;
            dot = i;
        }
    }
    size_t entries = count;
    if (dot != SIZE_MAX) {
        if ((dot % 2u) != 0 || dot + 2u != count) return false;
        entries = dot;
    }
    if (entries % 2u != 0) return false;
    bool pin = false;
    for (size_t i = 0; i < entries; i += 2u) {
        if (idm_syn_is_form_id(syn->as.seq.items[i], IDM_FORM_PIN)) { pin = true; break; }
    }
    *out_entries = entries;
    *out_dot = dot;
    *out_has_pin = pin;
    return true;
}

static bool dict_syntax_has_pin_keys(const IdmSyntax *syn) {
    size_t entries = 0, dot = SIZE_MAX;
    bool pin = false;
    return syn->kind == IDM_SYN_DICT && dict_syntax_pin_parts(syn, &entries, &dot, &pin) && pin;
}

static bool pin_walk_stops(const IdmSyntax *syn) {
    return syn->kind == IDM_SYN_LIST &&
           (idm_syn_is_form_id(syn, IDM_FORM_QUOTE) || idm_syn_is_form_id(syn, IDM_FORM_QUASISYNTAX) || idm_syn_is_form_id(syn, IDM_FORM_BITSTRING) || idm_syn_is_form_id(syn, IDM_FORM_PIN));
}

static bool pattern_syntax_has_pinned_dict_key(const IdmSyntax *syn) {
    if (!syn) return false;
    if (dict_syntax_has_pin_keys(syn)) return true;
    if (pin_walk_stops(syn)) return false;
    if (syn->kind != IDM_SYN_LIST && syn->kind != IDM_SYN_VECTOR && syn->kind != IDM_SYN_TUPLE && syn->kind != IDM_SYN_DICT) return false;
    for (size_t i = 0; i < syn->as.seq.count; i++) {
        if (pattern_syntax_has_pinned_dict_key(syn->as.seq.items[i])) return true;
    }
    return false;
}


static bool pin_append(IdmSyntax *seq, IdmSyntax *item) {
    if (!item) return false;
    if (!idm_syn_append(seq, item)) {
        idm_syn_free(item);
        return false;
    }
    return true;
}

static IdmSyntax *pin_stmt(IdmSyntax *body, IdmSpan span) {
    IdmSyntax *stmt = idm_syn_list(span);
    if (!pin_append(body, stmt)) return NULL;
    if (!pin_append(stmt, idm_syn_word("%-expr", span))) return NULL;
    return stmt;
}

static IdmSyntax *pin_body_begin(IdmSpan span) {
    IdmSyntax *body = idm_syn_list(span);
    if (!body) return NULL;
    if (!pin_append(body, idm_syn_word("%-body", span))) {
        idm_syn_free(body);
        return NULL;
    }
    return body;
}

static IdmSyntax *pinned_key_dict_rewrite(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *lhs, size_t rhs_start, IdmError *err) {
    size_t entries = 0, dot = SIZE_MAX;
    bool has_pin = false;
    dict_syntax_pin_parts(lhs, &entries, &dot, &has_pin);
    for (size_t i = 0; i < entries; i += 2u) {
        const IdmSyntax *key = lhs->as.seq.items[i];
        if (!idm_syn_is_form_id(key, IDM_FORM_PIN)) continue;
        if (key->as.seq.count != 2 || key->as.seq.items[1]->kind != IDM_SYN_WORD) {
            idm_error_set(err, key->span, "pin pattern currently supports only a variable: ^name");
            return NULL;
        }
    }
    char dict_name[32];
    snprintf(dict_name, sizeof dict_name, "%%pin.dict.%u", ctx->pin_key_seq++);
    bool has_rest = dot != SIZE_MAX;
    char rest_cur[32];
    if (has_rest) snprintf(rest_cur, sizeof rest_cur, "%%pin.rest.%u", ctx->pin_key_seq++);
    IdmSyntax *body = pin_body_begin(form->span);
    if (!body) return (IdmSyntax *)(uintptr_t)idm_error_oom(err, form->span);
    IdmSyntax *stmt = pin_stmt(body, form->span);
    bool ok = stmt && pin_append(stmt, idm_syn_word(dict_name, lhs->span)) && pin_append(stmt, idm_syn_word("=", form->span));
    for (size_t i = rhs_start; ok && i < form->as.seq.count; i++) ok = pin_append(stmt, idm_syn_clone(form->as.seq.items[i]));
    if (ok) {
        stmt = pin_stmt(body, lhs->span);
        IdmSyntax *stripped = stmt ? idm_syn_dict(lhs->span) : NULL;
        ok = stmt && pin_append(stmt, stripped);
        for (size_t i = 0; ok && i < entries; i += 2u) {
            if (idm_syn_is_form_id(lhs->as.seq.items[i], IDM_FORM_PIN)) continue;
            ok = pin_append(stripped, idm_syn_clone(lhs->as.seq.items[i])) && pin_append(stripped, idm_syn_clone(lhs->as.seq.items[i + 1u]));
        }
        if (ok && has_rest) {
            ok = pin_append(stripped, idm_syn_word(".", lhs->span)) && pin_append(stripped, idm_syn_word(rest_cur, lhs->as.seq.items[dot + 1u]->span));
        }
        ok = ok && pin_append(stmt, idm_syn_word("=", form->span)) && pin_append(stmt, idm_syn_word(dict_name, lhs->span));
    }
    for (size_t i = 0; ok && i < entries; i += 2u) {
        const IdmSyntax *key = lhs->as.seq.items[i];
        if (!idm_syn_is_form_id(key, IDM_FORM_PIN)) continue;
        const IdmSyntax *kword = key->as.seq.items[1];
        const IdmSyntax *val = lhs->as.seq.items[i + 1u];
        char has_name[32];
        snprintf(has_name, sizeof has_name, "%%pin.has.%u", ctx->pin_key_seq++);
        stmt = pin_stmt(body, key->span);
        ok = stmt && pin_append(stmt, idm_syn_word(has_name, key->span)) && pin_append(stmt, idm_syn_word("=", key->span)) &&
             pin_append(stmt, idm_syn_word("dict-has?", key->span)) && pin_append(stmt, idm_syn_word(dict_name, key->span)) &&
             pin_append(stmt, idm_syn_clone(kword));
        if (!ok) break;
        stmt = pin_stmt(body, key->span);
        IdmSyntax *want = stmt ? idm_syn_tuple(key->span) : NULL;
        ok = stmt && pin_append(stmt, want) &&
             pin_append(want, idm_syn_atom("dict-key", key->span)) && pin_append(want, idm_syn_word("_", key->span)) &&
             pin_append(want, idm_syn_atom("true", key->span)) &&
             pin_append(stmt, idm_syn_word("=", key->span));
        IdmSyntax *got = ok ? idm_syn_tuple(key->span) : NULL;
        ok = ok && pin_append(stmt, got) &&
             pin_append(got, idm_syn_atom("dict-key", key->span)) && pin_append(got, idm_syn_clone(kword)) &&
             pin_append(got, idm_syn_word(has_name, key->span));
        if (!ok) break;
        stmt = pin_stmt(body, val->span);
        ok = stmt && pin_append(stmt, idm_syn_clone(val)) && pin_append(stmt, idm_syn_word("=", val->span)) &&
             pin_append(stmt, idm_syn_word("dict-get", key->span)) && pin_append(stmt, idm_syn_word(dict_name, key->span)) &&
             pin_append(stmt, idm_syn_clone(kword)) && pin_append(stmt, idm_syn_int(0, key->span));
    }
    if (ok && has_rest) {
        const IdmSyntax *rest_pat = lhs->as.seq.items[dot + 1u];
        for (size_t i = 0; ok && i < entries; i += 2u) {
            const IdmSyntax *key = lhs->as.seq.items[i];
            if (!idm_syn_is_form_id(key, IDM_FORM_PIN)) continue;
            char rest_next[32];
            snprintf(rest_next, sizeof rest_next, "%%pin.rest.%u", ctx->pin_key_seq++);
            stmt = pin_stmt(body, rest_pat->span);
            ok = stmt && pin_append(stmt, idm_syn_word(rest_next, rest_pat->span)) && pin_append(stmt, idm_syn_word("=", rest_pat->span)) &&
                 pin_append(stmt, idm_syn_word("dict-del", rest_pat->span)) && pin_append(stmt, idm_syn_word(rest_cur, rest_pat->span)) &&
                 pin_append(stmt, idm_syn_clone(key->as.seq.items[1]));
            memcpy(rest_cur, rest_next, sizeof rest_cur);
        }
        stmt = ok ? pin_stmt(body, rest_pat->span) : NULL;
        ok = stmt && pin_append(stmt, idm_syn_clone(rest_pat)) && pin_append(stmt, idm_syn_word("=", rest_pat->span)) &&
             pin_append(stmt, idm_syn_word(rest_cur, rest_pat->span));
    }
    if (!ok) {
        idm_syn_free(body);
        if (err && !err->present) idm_error_oom(err, form->span);
        return NULL;
    }
    return body;
}

static bool pin_subtree_extract(ExpandContext *ctx, IdmSyntax *syn, IdmSyntax ***subs, char ***names, size_t *count, size_t *cap, IdmError *err) {
    if (pin_walk_stops(syn)) return true;
    if (syn->kind != IDM_SYN_LIST && syn->kind != IDM_SYN_VECTOR && syn->kind != IDM_SYN_TUPLE && syn->kind != IDM_SYN_DICT) return true;
    for (size_t i = 0; i < syn->as.seq.count; i++) {
        IdmSyntax *child = syn->as.seq.items[i];
        if (dict_syntax_has_pin_keys(child)) {
            if (*count == *cap) {
                IdmGrowItem items[2] = {
                    { (void **)subs, sizeof(**subs) },
                    { (void **)names, sizeof(**names) },
                };
                if (!idm_growv(items, 2u, cap, 4u, *count + 1u)) return idm_error_oom(err, child->span);
            }
            char name[32];
            snprintf(name, sizeof name, "%%pin.sub.%u", ctx->pin_key_seq++);
            char *name_copy = idm_strdup(name);
            IdmSyntax *fresh = name_copy ? idm_syn_word(name, child->span) : NULL;
            if (!fresh) {
                free(name_copy);
                return idm_error_oom(err, child->span);
            }
            (*subs)[*count] = child;
            (*names)[*count] = name_copy;
            (*count)++;
            syn->as.seq.items[i] = fresh;
            continue;
        }
        if (!pin_subtree_extract(ctx, child, subs, names, count, cap, err)) return false;
    }
    return true;
}

static IdmSyntax *pinned_key_nested_rewrite(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *lhs, size_t rhs_start, IdmError *err) {
    IdmSyntax *lhs2 = idm_syn_clone(lhs);
    if (!lhs2) return (IdmSyntax *)(uintptr_t)idm_error_oom(err, form->span);
    IdmSyntax **subs = NULL;
    char **names = NULL;
    size_t sub_count = 0, sub_cap = 0;
    IdmSyntax *body = NULL;
    bool ok = pin_subtree_extract(ctx, lhs2, &subs, &names, &sub_count, &sub_cap, err);
    if (ok) {
        body = pin_body_begin(form->span);
        IdmSyntax *stmt = body ? pin_stmt(body, form->span) : NULL;
        ok = stmt && pin_append(stmt, lhs2) && pin_append(stmt, idm_syn_word("=", form->span));
        if (stmt) lhs2 = NULL;
        for (size_t i = rhs_start; ok && i < form->as.seq.count; i++) ok = pin_append(stmt, idm_syn_clone(form->as.seq.items[i]));
        for (size_t s = 0; ok && s < sub_count; s++) {
            stmt = pin_stmt(body, subs[s]->span);
            ok = stmt && pin_append(stmt, subs[s]) && pin_append(stmt, idm_syn_word("=", subs[s]->span)) &&
                 pin_append(stmt, idm_syn_word(names[s], subs[s]->span));
            if (stmt) subs[s] = NULL;
        }
    }
    for (size_t s = 0; s < sub_count; s++) {
        idm_syn_free(subs[s]);
        free(names[s]);
    }
    free(subs);
    free(names);
    idm_syn_free(lhs2);
    if (!ok) {
        idm_syn_free(body);
        if (err && !err->present) idm_error_oom(err, form->span);
        return NULL;
    }
    return body;
}

static IdmSyntax *pinned_key_bind_rewrite(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *lhs, size_t rhs_start, IdmError *err) {
    if (dict_syntax_has_pin_keys(lhs)) return pinned_key_dict_rewrite(ctx, form, lhs, rhs_start, err);
    return pinned_key_nested_rewrite(ctx, form, lhs, rhs_start, err);
}

static void pin_deps_free(IdmSyntax **outer, char **fresh, size_t count) {
    for (size_t i = 0; i < count; i++) {
        idm_syn_free(outer[i]);
        free(fresh[i]);
    }
    free(outer);
    free(fresh);
}

static bool scan_pattern_bind(ExpandContext *ctx, BodyRec *rec, const IdmSyntax *pattern_syntax, size_t rhs_start, IdmError *err) {
    SavedFunctionContext probe;
    begin_function_context(ctx, &probe);
    ctx->pat_binder_collect = true;
    ctx->pat_binder_count = 0;
    PinCollect pins = {0};
    ctx->pin_collect = &pins;
    size_t saved_clause_base = ctx->clause_table_base;
    ctx->clause_table_base = ctx->bindings.count;
    IdmPattern *pattern = pattern_from_param_depth(ctx, pattern_syntax, 0, false, err);
    ctx->clause_table_base = saved_clause_base;
    ctx->pin_collect = NULL;
    ctx->pat_binder_collect = false;
    size_t count = ctx->pat_binder_count;
    const IdmSyntax **names = NULL;
    uint32_t *slots = NULL;
    IdmBindingId *binding_ids = NULL;
    CallableBindingInfo *bindings = NULL;
    bool ok = pattern != NULL;
    if (ok && count != 0) {
        names = malloc(count * sizeof(*names));
        slots = malloc(count * sizeof(*slots));
        binding_ids = calloc(count, sizeof(*binding_ids));
        bindings = malloc(count * sizeof(*bindings));
        ok = names != NULL && slots != NULL && binding_ids != NULL && bindings != NULL;
        if (ok) {
            memcpy(names, ctx->pat_binders, count * sizeof(*names));
            for (size_t i = 0; i < count; i++) {
                bindings[i].arity = idm_arity_unknown();
            }
        }
    }
    end_function_context(ctx, &probe);
    if (!ok) {
        free(names);
        free(slots);
        free(binding_ids);
        free(bindings);
        pin_deps_free(pins.outer, pins.fresh, pins.count);
        free(pins.clause);
        idm_pat_free(pattern);
        if (err && !err->present) idm_error_oom(err, pattern_syntax->span);
        return false;
    }
    bool env_bind = body_bind_is_env(ctx);
    for (size_t i = 0; i < count; i++) slots[i] = env_bind ? ctx->env_slot_seq++ : ctx->next_slot++;
    rec->kind = BODY_REC_BIND_PATTERN;
    rec->rhs_start = rhs_start;
    rec->pattern_syntax = pattern_syntax;
    rec->pattern = pattern;
    rec->pattern_names = names;
    rec->pattern_name_count = count;
    rec->pattern_slots = slots;
    rec->pattern_binding_ids = binding_ids;
    rec->pattern_bindings = bindings;
    rec->pin_outer = pins.outer;
    rec->pin_fresh = pins.fresh;
    rec->pin_count = pins.count;
    rec->pin_cap = pins.cap;
    free(pins.clause);
    rec->pattern_tmp_slot = ctx->next_slot++;
    return true;
}

static bool definition_like_form(const IdmSyntax *form, const char **out_head) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR)) return false;
    if (form->as.seq.count < 2 || form->as.seq.items[1]->kind != IDM_SYN_WORD) return false;
    size_t base = 1;
    if (syn_is_word(form->as.seq.items[1], "export") && form->as.seq.count >= 3 && (syn_is_word(form->as.seq.items[2], "defn") || syn_is_word(form->as.seq.items[2], "defmacro"))) base = 2;
    const char *head = form->as.seq.items[base]->as.text;
    if (strcmp(head, "def") != 0 && strcmp(head, "defn") != 0 && strcmp(head, "defmacro") != 0) return false;
    *out_head = head;
    return true;
}

static const char *core_operator_text_arg(const IdmSyntax *syn) {
    if (!syn) return NULL;
    if (syn->kind == IDM_SYN_WORD || syn->kind == IDM_SYN_ATOM || syn->kind == IDM_SYN_STRING) return syn->as.text;
    return NULL;
}

static bool core_operator_parts(const IdmSyntax *form, IdmSyntax *const **out_items, size_t *out_base, size_t *out_count) {
    if (!form || form->kind != IDM_SYN_LIST || form->as.seq.count == 0) return false;
    if ((idm_syn_is_form_id(form, IDM_FORM_GROUP) || idm_syn_is_form_id(form, IDM_FORM_LAYOUT_GROUP)) && form->as.seq.count == 2u) {
        return core_operator_parts(form->as.seq.items[1], out_items, out_base, out_count);
    }
    if (idm_syn_is_form_id(form, IDM_FORM_EXPR) && form->as.seq.count >= 2u && syn_is_word(form->as.seq.items[1], "core-operator")) {
        if (out_items) *out_items = form->as.seq.items;
        if (out_base) *out_base = 2u;
        if (out_count) *out_count = form->as.seq.count;
        return true;
    }
    if (form->as.seq.items[0]->kind == IDM_SYN_WORD && syn_is_word(form->as.seq.items[0], "core-operator")) {
        if (out_items) *out_items = form->as.seq.items;
        if (out_base) *out_base = 1u;
        if (out_count) *out_count = form->as.seq.count;
        return true;
    }
    return false;
}

static bool expand_core_operator_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    IdmSyntax *const *items = NULL;
    size_t base = 0;
    size_t count = 0;
    if (!core_operator_parts(form, &items, &base, &count)) return true;
    if (count != base + 5u) {
        return idm_error_set(err, form->span, "core-operator expects name, precedence, assoc, capture, and target");
    }
    const IdmSyntax *name_syntax = items[base];
    const IdmSyntax *precedence_syntax = items[base + 1u];
    const IdmSyntax *assoc_syntax = items[base + 2u];
    const IdmSyntax *capture_syntax = items[base + 3u];
    const IdmSyntax *target_syntax = items[base + 4u];
    char *name = name_syntax->kind == IDM_SYN_STRING ? idm_strdup(name_syntax->as.text) : surface_token_text_dup(name_syntax);
    if (!name) return idm_error_set(err, name_syntax->span, "operator name must be a symbol");
    if (precedence_syntax->kind != IDM_SYN_INT || precedence_syntax->as.integer < 0 || precedence_syntax->as.integer > 255) {
        free(name);
        return idm_error_set(err, precedence_syntax->span, "operator precedence must be an integer 0..255");
    }
    const char *assoc_text = core_operator_text_arg(assoc_syntax);
    IdmOpAssoc assoc;
    if (assoc_text && strcmp(assoc_text, "left") == 0) assoc = IDM_OP_ASSOC_LEFT;
    else if (assoc_text && strcmp(assoc_text, "right") == 0) assoc = IDM_OP_ASSOC_RIGHT;
    else if (assoc_text && strcmp(assoc_text, "none") == 0) assoc = IDM_OP_ASSOC_NONE;
    else {
        free(name);
        return idm_error_set(err, assoc_syntax->span, "operator assoc must be left, right, or none");
    }
    const char *capture = core_operator_text_arg(capture_syntax);
    if (!capture || !*capture) {
        free(name);
        return idm_error_set(err, capture_syntax->span, "operator capture must be non-empty");
    }
    char *target = surface_token_text_dup(target_syntax);
    if (!target) {
        free(name);
        return idm_error_set(err, target_syntax->span, "operator target must be an identifier");
    }

    IdmScopeSet decl_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &decl_scopes)) {
        free(name);
        free(target);
        return idm_error_oom(err, name_syntax->span);
    }
    IdmScopeSet target_scopes;
    if (!syntax_scopes_copy(&target_scopes, target_syntax)) {
        idm_scope_set_destroy(&decl_scopes);
        free(name);
        free(target);
        return idm_error_oom(err, target_syntax->span);
    }
    if (ctx->rt->macro_intro_active && !idm_scope_set_flip(&target_scopes, ctx->rt->macro_intro_scope)) {
        idm_scope_set_destroy(&decl_scopes);
        idm_scope_set_destroy(&target_scopes);
        free(name);
        free(target);
        return idm_error_oom(err, target_syntax->span);
    }
    bool ok = register_operator(ctx, name, capture, (uint8_t)precedence_syntax->as.integer, assoc, IDM_OPERATOR_ACTION_WORD, target, &target_scopes, &decl_scopes, ctx->unit, ctx->unit_key, true, form->span, err);
    idm_scope_set_destroy(&decl_scopes);
    idm_scope_set_destroy(&target_scopes);
    free(name);
    free(target);
    return ok;
}

static bool core_grammar_args(const IdmSyntax *form, size_t *out_head) {
    if (idm_syn_is_form_id(form, IDM_FORM_EXPR) && form->as.seq.count >= 2u && syn_is_word(form->as.seq.items[1], "core-grammar")) {
        if (out_head) *out_head = 1u;
        return true;
    }
    if (form && (form->kind == IDM_SYN_LIST || form->kind == IDM_SYN_VECTOR || form->kind == IDM_SYN_TUPLE) && form->as.seq.count >= 1u && syn_is_word(form->as.seq.items[0], "core-grammar")) {
        if (out_head) *out_head = 0u;
        return true;
    }
    return false;
}

static bool grammar_append_take(IdmSyntax *seq, IdmSyntax *item, IdmSpan span, IdmError *err) {
    if (item && idm_syn_append(seq, item)) return true;
    idm_syn_free(item);
    return idm_error_oom(err, span);
}

static bool surface_grammar_mode_atom(const IdmSyntax *mode, IdmSyntax **out, IdmError *err) {
    const char *text = NULL;
    if (mode && mode->kind == IDM_SYN_WORD) text = mode->as.text;
    if (!text || (strcmp(text, "extend") != 0 && strcmp(text, "exclusive") != 0)) {
        return idm_error_set(err, mode ? mode->span : idm_span_unknown(NULL), "grammar mode must be extend or exclusive");
    }
    *out = idm_syn_atom(text, mode->span);
    return *out != NULL || idm_error_oom(err, mode->span);
}

static bool surface_grammar_args(const IdmSyntax *form, size_t *out_head) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR) || form->as.seq.count < 2u) return false;
    if (syn_is_word(form->as.seq.items[1], "grammar")) {
        if (out_head) *out_head = 1u;
        return true;
    }
    if (form->as.seq.count >= 3u && syn_is_word(form->as.seq.items[1], "export") && syn_is_word(form->as.seq.items[2], "grammar")) {
        if (out_head) *out_head = 2u;
        return true;
    }
    return false;
}

static bool surface_grammar_arrow(const IdmSyntax *syn) {
    return syn_is_word(syn, "->");
}

static bool surface_grammar_tuple_rule(const char *kind, const IdmSyntax *name, const IdmSyntax *pattern, const IdmSyntax *constructor, IdmSpan span, IdmSyntax **out, IdmError *err) {
    IdmSyntax *rule = idm_syn_tuple(span);
    if (!rule) return idm_error_oom(err, span);
    if (!grammar_append_take(rule, idm_syn_atom(kind, span), span, err) ||
        !grammar_append_take(rule, idm_syn_clone(name), name ? name->span : span, err) ||
        !grammar_append_take(rule, idm_syn_clone(pattern), pattern ? pattern->span : span, err) ||
        (constructor && !grammar_append_take(rule, idm_syn_clone(constructor), constructor->span, err))) {
        idm_syn_free(rule);
        return false;
    }
    *out = rule;
    return true;
}

static bool surface_grammar_rule_from_body_item(const IdmSyntax *item, IdmSyntax **out, IdmError *err) {
    *out = NULL;
    if (!item) return idm_error_set(err, idm_span_unknown(NULL), "grammar body expects rule tuples or skip/token/form rule lines");
    if (item->kind == IDM_SYN_TUPLE) {
        *out = idm_syn_clone(item);
        return *out != NULL || idm_error_oom(err, item->span);
    }
    if (idm_syn_is_form_id(item, IDM_FORM_EXPR) && item->as.seq.count == 2u && item->as.seq.items[1]->kind == IDM_SYN_TUPLE) {
        *out = idm_syn_clone(item->as.seq.items[1]);
        return *out != NULL || idm_error_oom(err, item->as.seq.items[1]->span);
    }
    if (!idm_syn_is_form_id(item, IDM_FORM_EXPR) || item->as.seq.count < 2u) {
        return idm_error_set(err, item->span, "grammar body expects rule tuples or skip/token/form rule lines");
    }
    const IdmSyntax *kind = item->as.seq.items[1];
    if (syn_is_word(kind, "skip")) {
        if (item->as.seq.count != 4u) {
            return idm_error_set(err, item->span, "grammar skip rule expects: skip NAME PATTERN");
        }
        return surface_grammar_tuple_rule("skip", item->as.seq.items[2], item->as.seq.items[3], NULL, item->span, out, err);
    }
    if (syn_is_word(kind, "pair")) {
        if (item->as.seq.count != 4u) {
            return idm_error_set(err, item->span, "grammar pair expects: pair OPEN CLOSE");
        }
        return surface_grammar_tuple_rule("pair", item->as.seq.items[2], item->as.seq.items[3], NULL, item->span, out, err);
    }
    if (syn_is_word(kind, "token") || syn_is_word(kind, "form")) {
        if (item->as.seq.count != 6u || !surface_grammar_arrow(item->as.seq.items[4])) {
            return idm_error_set(err, item->span, syn_is_word(kind, "token") ? "grammar token rule expects: token NAME PATTERN -> CONSTRUCTOR" : "grammar form rule expects: form NAME PATTERN -> CONSTRUCTOR");
        }
        return surface_grammar_tuple_rule(kind->as.text, item->as.seq.items[2], item->as.seq.items[3], item->as.seq.items[5], item->span, out, err);
    }
    return idm_error_set(err, item->span, "grammar body expects rule tuples or skip/token/form rule lines");
}

static bool expand_core_grammar_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    size_t head = 0;
    if (!core_grammar_args(form, &head)) {
        return idm_error_set(err, form->span, "core-grammar expects: core-grammar NAME :extend|:exclusive [RULE ...]");
    }
    if (ctx->phase != 0) {
        return idm_error_set(err, form->span, "core-grammar declarations are phase-0 reader artifact declarations");
    }
    IdmSyntax shifted;
    const IdmSyntax *ir = form;
    if (head != 0) {
        shifted = *form;
        shifted.as.seq.items = form->as.seq.items + head;
        shifted.as.seq.count = form->as.seq.count - head;
        ir = &shifted;
    }
    IdmPkgGrammar grammar;
    if (!idm_pkg_grammar_from_ir(ir, &grammar, err)) return false;
    if (grammar.mode == (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE) {
        IdmReaderArtifact *probe = NULL;
        if (!idm_reader_artifact_from_grammars(grammar.name, &grammar, 1u, &probe, err)) {
            idm_pkg_grammar_destroy(&grammar);
            if (err && err->present && err->span.line == 0) idm_error_set_span(err, form->span);
            return false;
        }
        idm_reader_artifact_destroy(probe);
    }
    const IdmSyntax *name = ir->as.seq.items[1];
    IdmGrammarRule *rules = grammar.rules;
    size_t rule_count = grammar.rule_count;
    IdmGrammarPair *pairs = grammar.pairs;
    size_t pair_count = grammar.pair_count;
    uint8_t mode = grammar.mode;
    grammar.rules = NULL;
    grammar.rule_count = 0;
    grammar.pairs = NULL;
    grammar.pair_count = 0;
    bool ok = register_grammar(ctx, name, mode, rules, rule_count, pairs, pair_count, true, err);
    idm_pkg_grammar_destroy(&grammar);
    return ok;
}

static bool expand_surface_grammar_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    size_t head = 0;
    if (!surface_grammar_args(form, &head) || form->as.seq.count != head + 4u || form->as.seq.items[head + 1u]->kind != IDM_SYN_WORD ||
        !idm_syn_is_form_id(form->as.seq.items[head + 3u], IDM_FORM_BODY)) {
        return idm_error_set(err, form ? form->span : idm_span_unknown(NULL), "grammar expects: grammar NAME extend|exclusive do RULE ... end");
    }
    if (ctx->phase != 0) {
        return idm_error_set(err, form->span, "core-grammar declarations are phase-0 reader artifact declarations");
    }
    IdmSyntax *ir = idm_syn_list(form->span);
    IdmSyntax *rules = idm_syn_vector(form->as.seq.items[head + 3u]->span);
    if (!ir || !rules) {
        idm_syn_free(ir);
        idm_syn_free(rules);
        return idm_error_oom(err, form->span);
    }
    IdmSyntax *mode = NULL;
    bool ok = grammar_append_take(ir, idm_syn_word("core-grammar", form->as.seq.items[head]->span), form->span, err) &&
              grammar_append_take(ir, idm_syn_clone(form->as.seq.items[head + 1u]), form->as.seq.items[head + 1u]->span, err) &&
              surface_grammar_mode_atom(form->as.seq.items[head + 2u], &mode, err);
    if (ok) {
        ok = grammar_append_take(ir, mode, form->as.seq.items[head + 2u]->span, err);
        mode = NULL;
    }
    if (!ok) {
        idm_syn_free(mode);
        idm_syn_free(rules);
        idm_syn_free(ir);
        return false;
    }
    const IdmSyntax *body = form->as.seq.items[head + 3u];
    for (size_t i = 1u; i < body->as.seq.count; i++) {
        IdmSyntax *rule = NULL;
        if (!surface_grammar_rule_from_body_item(body->as.seq.items[i], &rule, err) ||
            !grammar_append_take(rules, rule, body->as.seq.items[i]->span, err)) {
            idm_syn_free(rules);
            idm_syn_free(ir);
            return false;
        }
    }
    if (!grammar_append_take(ir, rules, rules->span, err)) {
        idm_syn_free(ir);
        return false;
    }
    ok = expand_core_grammar_decl(ctx, ir, err);
    idm_syn_free(ir);
    return ok;
}

static bool defn_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_param_start, bool *out_export) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR)) return false;
    size_t base = 1;
    bool exported = false;
    if (form->as.seq.count > 1 && syn_is_word(form->as.seq.items[1], "export")) { exported = true; base = 2; }
    if (form->as.seq.count < base + 3u) return false;
    if (!syn_is_word(form->as.seq.items[base], "defn")) return false;
    if (form->as.seq.items[base + 1u]->kind != IDM_SYN_WORD) return false;
    *out_name = form->as.seq.items[base + 1u];
    *out_param_start = base + 2u;
    if (out_export) *out_export = exported;
    return true;
}

static void defn_groups_destroy(DefnGroup *groups, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(groups[i].indices);
        free(groups[i].doc);
        idm_scope_set_destroy(&groups[i].scopes);
        if (groups[i].has_contract) idm_callable_contract_destroy(&groups[i].contract);
    }
    free(groups);
}

static DefnGroup *find_or_add_group(DefnGroup **groups, size_t *count, size_t *cap, const IdmSyntax *name_syntax) {
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*groups)[i].name, name_syntax->as.text) == 0) {
            IdmScopeSet temp;
            if (!syntax_scopes_copy(&temp, name_syntax)) return NULL;
            bool same = idm_scope_set_equal(&(*groups)[i].scopes, &temp);
            idm_scope_set_destroy(&temp);
            if (same) return &(*groups)[i];
        }
    }
    if (*count == *cap) {
        if (!idm_grow((void **)groups, cap, sizeof(**groups), 4u, *count + 1u)) return NULL;
    }
    DefnGroup *group = &(*groups)[(*count)++];
    memset(group, 0, sizeof(*group));
    group->name = name_syntax->as.text;
    group->name_syntax = name_syntax;
    if (!syntax_scopes_copy(&group->scopes, name_syntax)) return NULL;
    group->slot = UINT32_MAX;
    group->arity = idm_arity_unknown();
    group->indices = NULL;
    group->count = 0;
    group->cap = 0;
    group->exported = false;
    group->has_contract = false;
    memset(&group->contract, 0, sizeof(group->contract));
    group->doc = NULL;
    return group;
}

static bool group_add_index(DefnGroup *group, size_t index) {
    if (group->count == group->cap) {
        if (!idm_grow((void **)&group->indices, &group->cap, sizeof(*group->indices), 4u, group->count + 1u)) return false;
    }
    group->indices[group->count++] = index;
    return true;
}

bool record_package_slot(ExpandContext *ctx, const char *name, uint32_t slot_id, const IdmScopeSet *scopes, IdmArity arity, const IdmCallableContract *contract, bool exported) {
    if (!ctx->in_package || ctx->phase != 0) return true;
    size_t prior_index = SIZE_MAX;
    for (size_t i = ctx->package_slot_count; i > 0; i--) {
        if (ctx->package_slots[i - 1u].slot == slot_id && strcmp(ctx->package_slots[i - 1u].name, name) == 0) {
            prior_index = i - 1u;
            break;
        }
    }
    if (ctx->package_slot_count == ctx->package_slot_cap) {
        if (!idm_grow((void **)&ctx->package_slots, &ctx->package_slot_cap, sizeof(*ctx->package_slots), 8u, ctx->package_slot_count + 1u)) return false;
    }
    const IdmPkgSlot *prior = prior_index != SIZE_MAX ? &ctx->package_slots[prior_index] : NULL;
    IdmPkgSlot *entry = &ctx->package_slots[ctx->package_slot_count];
    memset(entry, 0, sizeof(*entry));
    entry->name = idm_strdup(name);
    if (!entry->name) return false;
    entry->slot = slot_id;
    entry->arity = arity.kind != IDM_ARITY_UNKNOWN ? arity : (prior ? prior->arity : arity);
    entry->has_contract = false;
    memset(&entry->contract, 0, sizeof(entry->contract));
    const IdmCallableContract *kept = contract ? contract : (prior && prior->has_contract ? &prior->contract : NULL);
    if (kept) {
        if (!idm_callable_contract_copy(&entry->contract, kept)) {
            free(entry->name);
            entry->name = NULL;
            return false;
        }
        entry->has_contract = true;
    }
    entry->exported = exported || (prior && prior->exported);
    if (!idm_scope_set_copy(&entry->scopes, scopes)) {
        free(entry->name);
        entry->name = NULL;
        if (entry->has_contract) idm_callable_contract_destroy(&entry->contract);
        return false;
    }
    ctx->package_slot_count++;
    return true;
}

static bool body_existing_env_binding(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, const IdmBinding **out_binding) {
    for (size_t i = ctx->bindings.count; i > 0; i--) {
        const IdmBinding *binding = &ctx->bindings.items[i - 1u];
        if ((binding->phase != ctx->phase && binding->phase != IDM_PHASE_ANY) || binding->space != IDM_BIND_SPACE_DEFAULT ||
            binding->kind != IDM_BIND_ENV || strcmp(binding->name, name) != 0) {
            continue;
        }
        if (!idm_scope_set_equal(&binding->scopes, scopes)) continue;
        if (out_binding) *out_binding = binding;
        return true;
    }
    return false;
}

static bool body_env_def_binder_with_arity(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, IdmArity arity, const IdmCallableContract *contract, bool reuse_existing, uint32_t *out_id, bool *out_created, IdmError *err) {
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return false;
    const IdmBinding *existing = NULL;
    bool reused = reuse_existing && body_existing_env_binding(ctx, name, &scopes, &existing);
    uint32_t id = reused ? existing->payload : ctx->env_slot_seq++;
    IdmBindingId binding_id = 0;
    bool ok = idm_binding_table_add_with_arity(&ctx->bindings, name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_ENV, &scopes, id, IDM_FRAME_ENV, arity, &binding_id);
    if (ok && contract) ok = idm_binding_table_set_contract(&ctx->bindings, binding_id, contract);
    idm_scope_set_destroy(&scopes);
    if (!ok) return idm_error_oom(err, name_syntax->span);
    if (out_id) *out_id = id;
    if (out_created) *out_created = !reused;
    return true;
}

static void body_recs_destroy(BodyRec *recs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (recs[i].kind == BODY_REC_GROUPS) defn_groups_destroy(recs[i].groups, recs[i].group_count);
        if (recs[i].kind == BODY_REC_BIND_PATTERN) {
            idm_pat_free(recs[i].pattern);
            free(recs[i].pattern_names);
            free(recs[i].pattern_slots);
            free(recs[i].pattern_binding_ids);
            free(recs[i].pattern_bindings);
            pin_deps_free(recs[i].pin_outer, recs[i].pin_fresh, recs[i].pin_count);
        }
        if (recs[i].kind == BODY_REC_BIND && recs[i].bind_has_contract) idm_callable_contract_destroy(&recs[i].bind_contract);
        free(recs[i].deps);
        idm_core_free(recs[i].core);
    }
    free(recs);
}

static IdmArity core_callable_arity(const IdmCore *core) {
    if (!core) return idm_arity_unknown();
    if (core->kind == IDM_CORE_FN) return idm_arity_exact(core->as.fn.arity);
    if (core->kind != IDM_CORE_FN_MULTI) return idm_arity_unknown();
    IdmArity arity = idm_arity_unknown();
    for (size_t i = 0; i < core->as.fn_multi.count; i++) {
        const IdmFnClause *clause = &core->as.fn_multi.clauses[i];
        const IdmArity *clause_arity = clause->primitive_backed ? &clause->call_arity : NULL;
        if (clause_arity && clause_arity->kind != IDM_ARITY_UNKNOWN) {
            if (!idm_arity_merge(&arity, clause_arity)) return idm_arity_unknown();
            continue;
        }
        if (!idm_arity_add_exact(&arity, clause->arity)) return idm_arity_unknown();
    }
    return arity;
}

static bool contract_has_arity(const IdmCallableContract *contract, uint32_t arity) {
    for (size_t i = 0; i < contract->sig_count; i++) {
        if (contract->sigs[i].arg_count == arity) return true;
    }
    return false;
}

static bool core_accepts_arity(const IdmCore *core, uint32_t arity) {
    if (core->kind == IDM_CORE_FN) return core->as.fn.arity == arity;
    if (core->kind != IDM_CORE_FN_MULTI) return false;
    for (size_t i = 0; i < core->as.fn_multi.count; i++) {
        const IdmFnClause *clause = &core->as.fn_multi.clauses[i];
        if (clause->primitive_backed) {
            if (idm_arity_accepts(&clause->call_arity, arity)) return true;
        } else if (clause->arity == arity) {
            return true;
        }
    }
    return false;
}

static bool callable_contract_validate_core_arities(const IdmSyntax *name, const IdmCallableContract *contract, const IdmCore *core, IdmError *err) {
    if (!contract || contract->sig_count == 0 || !core || (core->kind != IDM_CORE_FN && core->kind != IDM_CORE_FN_MULTI)) return true;
    IdmBuffer expected;
    idm_buf_init(&expected);
    if (!contract_arity_describe_exact(&expected, contract)) {
        idm_buf_destroy(&expected);
        return idm_error_oom(err, name->span);
    }
    if (core->kind == IDM_CORE_FN && !contract_has_arity(contract, core->as.fn.arity)) {
        bool set = idm_error_set(err, name->span, "signature for '%s' expects arity %s, value has %u", name->as.text, expected.data, core->as.fn.arity);
        idm_buf_destroy(&expected);
        return set;
    }
    if (core->kind == IDM_CORE_FN_MULTI) {
        for (size_t i = 0; i < core->as.fn_multi.count; i++) {
            const IdmFnClause *clause = &core->as.fn_multi.clauses[i];
            if (clause->primitive_backed || contract_has_arity(contract, clause->arity)) continue;
            bool set = idm_error_set(err, name->span, "signature for '%s' expects arity %s, value has %u", name->as.text, expected.data, clause->arity);
            idm_buf_destroy(&expected);
            return set;
        }
    }
    for (size_t i = 0; i < contract->sig_count; i++) {
        size_t arity = contract->sigs[i].arg_count;
        if (arity <= UINT32_MAX && core_accepts_arity(core, (uint32_t)arity)) continue;
        bool set = idm_error_set(err, name->span, "signature for '%s' expects arity %zu, value has no matching clause", name->as.text, arity);
        idm_buf_destroy(&expected);
        return set;
    }
    idm_buf_destroy(&expected);
    return true;
}

static bool syntax_forces_callable_value(const IdmSyntax *syn) {
    if (!syn) return false;
    if (idm_syn_is_form_id(syn, IDM_FORM_EXPRESSION) && syn->as.seq.count == 2u) {
        const IdmSyntax *child = syn->as.seq.items[1];
        return child->kind == IDM_SYN_WORD && child->as.text[0] == '&' && child->as.text[1] != '\0';
    }
    if (idm_syn_is_form_id(syn, IDM_FORM_LAYOUT_GROUP) && syn->as.seq.count == 2u) {
        const IdmSyntax *inner = syn->as.seq.items[1];
        if (idm_syn_is_form_id(inner, IDM_FORM_EXPR) && inner->as.seq.count >= 2u) return syntax_forces_callable_value(inner->as.seq.items[1]);
    }
    return false;
}

static bool syntax_callable_value_binding_info(ExpandContext *ctx, const IdmSyntax *syn, IdmArity *out, IdmError *err) {
    if (idm_syn_is_form_id(syn, IDM_FORM_EXPR) && syn->as.seq.count >= 2u && syn_is_word(syn->as.seq.items[1], "fn")) {
        return function_literal_arity(ctx, syn->as.seq.items[1], syn->as.seq.items, 2u, syn->as.seq.count, out, err);
    }
    IdmArity arity = idm_arity_unknown();
    if (!expand_syntax_call_arity(ctx, syn, &arity, err)) return false;
    if (arity.kind == IDM_ARITY_UNKNOWN) return false;
    if (!syntax_forces_callable_value(syn) && expand_arity_auto_calls_zero(&arity)) return false;
    *out = arity;
    return true;
}

static bool rhs_callable_value_binding_info(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmArity *out, IdmError *err) {
    if (start < end && syn_is_word(items[start], "fn")) {
        return function_literal_arity(ctx, items[start], items, start + 1u, end, out, err);
    }
    if (start + 1u == end) return syntax_callable_value_binding_info(ctx, items[start], out, err);
    return false;
}

static IdmCore *expand_value_rhs(ExpandContext *ctx, const BodyRec *rec, IdmError *err) {
    IdmSyntax *const *items = rec->form->as.seq.items;
    size_t end = rec->form->as.seq.count;
    if (rec->bind_has_contract && rec->rhs_start < end && syn_is_word(items[rec->rhs_start], "fn")) {
        return expand_function_literal_with_contract(ctx, "<lambda>", items[rec->rhs_start], items, rec->rhs_start + 1u, end, &rec->bind_contract, err);
    }
    return expand_parts(ctx, items, rec->rhs_start, end, err);
}

static bool typecheck_transformer(ExpandContext *ctx, const char *name, IdmCore *fn, IdmError *err) {
    IdmCallableContract contract;
    memset(&contract, 0, sizeof(contract));
    IdmContractSig *sig = idm_contract_add_sig(&contract);
    if (!sig) return idm_error_oom(err, idm_span_unknown(NULL));
    sig->args = calloc(1u, sizeof(*sig->args));
    if (!sig->args) { idm_callable_contract_destroy(&contract); return idm_error_oom(err, idm_span_unknown(NULL)); }
    sig->arg_count = 1u;
    if (!idm_type_con(ctx->rt, &sig->args[0], "syntax") || !idm_type_con(ctx->rt, &sig->result, "syntax")) {
        idm_callable_contract_destroy(&contract);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    sig->has_result = true;
    bool has = true;
    bool ok = expand_typecheck_value(ctx, name, &fn, &contract, &has, true, err);
    idm_callable_contract_destroy(&contract);
    return ok;
}

static bool infer_bind_value_contract(ExpandContext *ctx, BodyRec *rec, IdmError *err) {
    if (rec->bind_has_contract || !rec->core) return true;
    return expand_typecheck_infer_scheme(ctx, rec->core, rec->bind_name->as.text, &rec->bind_contract, &rec->bind_has_contract, err);
}

static bool pattern_bind_infos_walk(ExpandContext *ctx, const IdmSyntax *pattern, const IdmSyntax *rhs, CallableBindingInfo *bindings, size_t count, size_t *index, IdmError *err) {
    if (pattern->kind == IDM_SYN_WORD) {
        if (strcmp(pattern->as.text, "_") != 0 && *index < count) {
            IdmArity arity = idm_arity_unknown();
            if (syntax_callable_value_binding_info(ctx, rhs, &arity, err)) {
                bindings[*index].arity = arity;
            }
            *index += 1u;
        }
        return true;
    }
    if ((pattern->kind == IDM_SYN_TUPLE || pattern->kind == IDM_SYN_VECTOR || pattern->kind == IDM_SYN_LIST) && pattern->kind == rhs->kind && pattern->as.seq.count == rhs->as.seq.count) {
        for (size_t i = 0; i < pattern->as.seq.count; i++) {
            if (!pattern_bind_infos_walk(ctx, pattern->as.seq.items[i], rhs->as.seq.items[i], bindings, count, index, err)) return false;
        }
    }
    return true;
}

static bool infer_pattern_bind_infos(ExpandContext *ctx, BodyRec *rec, IdmError *err) {
    if (!rec->pattern_bindings || rec->rhs_start + 1u != rec->form->as.seq.count) return true;
    size_t index = 0;
    return pattern_bind_infos_walk(ctx, rec->pattern_syntax, rec->form->as.seq.items[rec->rhs_start], rec->pattern_bindings, rec->pattern_name_count, &index, err);
}

static size_t core_callable_capture_count(const IdmCore *core) {
    if (!core) return 0;
    if (core->kind == IDM_CORE_FN) return core->as.fn.capture_count;
    if (core->kind == IDM_CORE_FN_MULTI) return core->as.fn_multi.capture_count;
    return 0;
}

static bool push_bind_binder(ExpandContext *ctx, const IdmSyntax *name_syntax, uint32_t slot, IdmArity arity, const IdmCallableContract *contract, IdmBindingId *out_binding_id, IdmError *err) {
    bool env_bind = body_bind_is_env(ctx);
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return false;
    const IdmBinding *existing = NULL;
    if (env_bind && body_existing_env_binding(ctx, name_syntax->as.text, &scopes, &existing) && existing->payload != slot) {
        idm_scope_set_destroy(&scopes);
        return idm_error_set(err, name_syntax->span, "environment binding '%s' already uses a different slot", name_syntax->as.text);
    }
    IdmBindingId binding_id = 0;
    bool ok = idm_binding_table_add_with_arity(&ctx->bindings, name_syntax->as.text, ctx->phase, IDM_BIND_SPACE_DEFAULT, env_bind ? IDM_BIND_ENV : IDM_BIND_LOCAL, &scopes, slot, env_bind ? IDM_FRAME_ENV : ctx->frame, arity, &binding_id);
    if (ok && contract) ok = idm_binding_table_set_contract(&ctx->bindings, binding_id, contract);
    if (ok && env_bind && ctx->in_package) ok = record_package_slot(ctx, name_syntax->as.text, slot, &scopes, arity, contract, false);
    idm_scope_set_destroy(&scopes);
    if (ok && out_binding_id) *out_binding_id = binding_id;
    return ok || idm_error_oom(err, name_syntax->span);
}

static IdmCore *pattern_extract_value(BodyRec *rec, size_t index, IdmError *err) {
    IdmSpan span = rec->pattern_names[index]->span;
    IdmCore *call = expand_primitive_clause_call(IDM_PRIM_TUPLE_GET, span, err);
    if (!call) return NULL;
    if (!core_call_add_arg_or_free(call, idm_core_local_ref("_match", rec->pattern_tmp_slot, span), err, span)) return NULL;
    if (!core_call_add_arg_or_free(call, idm_core_literal(idm_int((int64_t)index), span), err, span)) return NULL;
    return call;
}

static IdmCore *pattern_carrier_fn(BodyRec *rec, IdmCore *body, IdmError *err);

static bool core_value_sequence_add_or_free(IdmCore *seq, IdmCore *item, IdmError *err, IdmSpan span) {
    if (!item) return false;
    if (idm_core_value_sequence_add(seq, item)) return true;
    idm_core_free(item);
    return idm_error_oom(err, span);
}

static IdmCore *pattern_bind_scrutinee(ExpandContext *ctx, BodyRec *rec, IdmCore *rhs, IdmError *err) {
    if (rec->pin_count == 0) return rhs;
    IdmCore *tuple = idm_core_value_sequence(IDM_VALUE_SEQ_TUPLE, rec->form->span);
    if (!tuple) {
        idm_core_free(rhs);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, rec->form->span);
    }
    for (size_t i = 0; i < rec->pin_count; i++) {
        IdmCore *pin = expand_syntax(ctx, rec->pin_outer[i], err);
        if (!core_value_sequence_add_or_free(tuple, pin, err, rec->pin_outer[i]->span)) {
            idm_core_free(tuple);
            idm_core_free(rhs);
            return NULL;
        }
    }
    if (!core_value_sequence_add_or_free(tuple, rhs, err, rec->form->span)) {
        idm_core_free(tuple);
        return NULL;
    }
    return tuple;
}

static IdmCore *pattern_bind_match(ExpandContext *ctx, BodyRec *rec, IdmCore *rhs, IdmError *err) {
    IdmSpan span = rec->pattern_syntax->span;
    IdmCore *body = idm_core_literal(idm_nil(), span);
    if (!body) {
        idm_core_free(rhs);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    IdmCore *fn = pattern_carrier_fn(rec, body, err);
    if (!fn) {
        idm_core_free(rhs);
        return NULL;
    }
    rhs = pattern_bind_scrutinee(ctx, rec, rhs, err);
    if (!rhs) {
        idm_core_free(fn);
        return NULL;
    }
    uint32_t first = rec->pattern_name_count != 0 ? rec->pattern_slots[0] : 0;
    IdmCore *core = idm_core_match_bind(fn, rhs, NULL, first, (uint32_t)rec->pattern_name_count, rec->form->span);
    if (!core) {
        idm_core_free(fn);
        idm_core_free(rhs);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, rec->form->span);
    }
    return core;
}

static IdmCore *pattern_bind_call(ExpandContext *ctx, BodyRec *rec, IdmCore *rhs, IdmError *err) {
    IdmSpan span = rec->pattern_syntax->span;
    IdmCore *body = expand_primitive_clause_call(IDM_PRIM_TUPLE, span, err);
    if (!body) {
        idm_core_free(rhs);
        return NULL;
    }
    for (size_t i = 0; i < rec->pattern_name_count; i++) {
        IdmCore *ref = idm_core_local_ref(rec->pattern_names[i]->as.text, (uint32_t)i, rec->pattern_names[i]->span);
        if (!core_call_add_arg_or_free(body, ref, err, span)) {
            idm_core_free(rhs);
            return NULL;
        }
    }
    IdmCore *fn = pattern_carrier_fn(rec, body, err);
    if (!fn) {
        idm_core_free(rhs);
        return NULL;
    }
    rhs = pattern_bind_scrutinee(ctx, rec, rhs, err);
    if (!rhs) {
        idm_core_free(fn);
        return NULL;
    }
    IdmCore *call = idm_core_call(fn, rec->form->span);
    if (!call) {
        idm_core_free(fn);
        idm_core_free(rhs);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, rec->form->span);
    }
    if (!core_call_add_arg_or_free(call, rhs, err, rec->form->span)) return NULL;
    return call;
}

static IdmCore *pattern_carrier_fn(BodyRec *rec, IdmCore *body, IdmError *err) {
    IdmSpan span = rec->pattern_syntax->span;
    IdmBuffer name_buf;
    idm_buf_init(&name_buf);
    char *name = idm_syn_dump(&name_buf, rec->pattern_syntax) ? idm_buf_take(&name_buf) : NULL;
    idm_buf_destroy(&name_buf);
    IdmCore *fn = idm_core_fn(name ? name : "=", 1, body, span);
    free(name);
    if (!fn) {
        idm_core_free(body);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    IdmPattern **patterns = malloc(sizeof(*patterns));
    if (!patterns) {
        idm_core_free(fn);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    if (rec->pin_count == 0) {
        patterns[0] = rec->pattern;
        rec->pattern = NULL;
    } else {
        size_t count = rec->pin_count + 1u;
        IdmPattern **items = calloc(count, sizeof(*items));
        if (!items) {
            free(patterns);
            idm_core_free(fn);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
        bool ok = true;
        for (size_t i = 0; ok && i < rec->pin_count; i++) {
            items[i] = idm_pat_bind(rec->pin_fresh[i], rec->pin_outer[i]->span);
            ok = items[i] != NULL;
        }
        items[rec->pin_count] = rec->pattern;
        patterns[0] = ok ? idm_pat_sequence(IDM_PAT_TUPLE, items, count, span) : NULL;
        if (!patterns[0]) {
            for (size_t i = 0; i < rec->pin_count; i++) idm_pat_free(items[i]);
            free(items);
            free(patterns);
            idm_core_free(fn);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
        rec->pattern = NULL;
    }
    if (!idm_core_fn_set_param_patterns_take(fn, patterns, 1)) {
        idm_pat_free(patterns[0]);
        free(patterns);
        idm_core_free(fn);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    IdmPatternLocal *locals = NULL;
    if (rec->pattern_name_count != 0) {
        locals = calloc(rec->pattern_name_count, sizeof(*locals));
        if (!locals) {
            idm_core_free(fn);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
        for (size_t i = 0; i < rec->pattern_name_count; i++) {
            locals[i].name = idm_strdup(rec->pattern_names[i]->as.text);
            if (!locals[i].name) {
                for (size_t j = 0; j < i; j++) free(locals[j].name);
                free(locals);
                idm_core_free(fn);
                        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
            }
            locals[i].slot = (uint32_t)i;
        }
    }
    if (!idm_core_fn_set_pattern_locals_take(fn, locals, (uint32_t)rec->pattern_name_count)) {
        for (size_t i = 0; i < rec->pattern_name_count; i++) free(locals[i].name);
        free(locals);
        idm_core_free(fn);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    return fn;
}

static bool body_work_splice(IdmSyntax ***work, size_t *work_count, size_t *work_cap, size_t at, const IdmSyntax *body, IdmError *err) {
    size_t add = body->as.seq.count - 1u;
    size_t needed = *work_count - 1u + add;
    if (needed > *work_cap) {
        if (!idm_grow((void **)work, work_cap, sizeof(**work), 8u, needed)) return idm_error_oom(err, body->span);
    }
    memmove(*work + at + add, *work + at + 1u, (*work_count - at - 1u) * sizeof(**work));
    for (size_t k = 0; k < add; k++) (*work)[at + k] = body->as.seq.items[1u + k];
    *work_count = needed;
    return true;
}

static bool body_scope_add_range(ExpandContext *ctx, IdmSyntax **work, size_t start, size_t count, IdmScopeId scope, IdmSpan span, IdmError *err) {
    (void)ctx;
    for (size_t i = start; i < count; i++) {
        if (idm_syn_scope_contains(work[i], 0, scope)) continue;
        if (!idm_syn_scope_add_tree(work[i], 0, scope)) return idm_error_oom(err, span);
    }
    return true;
}

static bool body_definition_scope_add(ExpandContext *ctx, IdmSyntax **work, size_t start, size_t count, IdmScopeId scope, IdmSpan span, IdmError *err) {
    return body_scope_add_range(ctx, work, start, count, scope, span, err);
}

static void use_selection_destroy(UseSelection *selection) {
    if (!selection) return;
    free(selection->names);
    free(selection->spans);
    free(selection->matched);
    selection->names = NULL;
    selection->spans = NULL;
    selection->matched = NULL;
    selection->count = 0;
}

static bool use_selection_parse(const IdmSyntax *form, size_t start, size_t end, size_t *out_pos, UseSelection *selection, IdmError *err) {
    selection->names = NULL;
    selection->spans = NULL;
    selection->matched = NULL;
    selection->count = 0;
    *out_pos = start;
    if (start >= end || !syn_is_word(form->as.seq.items[start], "with")) return true;
    if (start + 2u > end || form->as.seq.items[start + 1u]->kind != IDM_SYN_VECTOR) {
        return idm_error_set(err, form->span, "use selection expects: with [name ...]");
    }
    const IdmSyntax *items = form->as.seq.items[start + 1u];
    if (items->as.seq.count == 0) {
        return idm_error_set(err, items->span, "use selection requires at least one name");
    }
    const char **names = calloc(items->as.seq.count, sizeof(*names));
    IdmSpan *spans = calloc(items->as.seq.count, sizeof(*spans));
    bool *matched = calloc(items->as.seq.count, sizeof(*matched));
    if (!names || !spans || !matched) {
        free(names);
        free(spans);
        free(matched);
        return idm_error_oom(err, items->span);
    }
    for (size_t i = 0; i < items->as.seq.count; i++) {
        const IdmSyntax *name = items->as.seq.items[i];
        if (name->kind != IDM_SYN_WORD) {
            free(names);
            free(spans);
            free(matched);
            return idm_error_set(err, name->span, "use selection entries must be names");
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(names[j], name->as.text) == 0) {
                free(names);
                free(spans);
                free(matched);
                return idm_error_set(err, name->span, "use selection repeats name '%s'", name->as.text);
            }
        }
        names[i] = name->as.text;
        spans[i] = name->span;
    }
    selection->names = names;
    selection->spans = spans;
    selection->matched = matched;
    selection->count = items->as.seq.count;
    *out_pos = start + 2u;
    return true;
}

typedef enum {
    BODY_DECL_NONE,
    BODY_DECL_PACKAGE,
    BODY_DECL_USE,
    BODY_DECL_IMPORT,
    BODY_DECL_PROTOCOL,
    BODY_DECL_ACTIVATE,
        BODY_DECL_CORE_FORM,
    BODY_DECL_CORE_READER_FORM,
    BODY_DECL_CORE_OPERATOR,
    BODY_DECL_CORE_GRAMMAR,
    BODY_DECL_GRAMMAR,
    BODY_DECL_TYPE,
    BODY_DECL_RECORD,
    BODY_DECL_TRAIT,
    BODY_DECL_IMPLEMENT,
    BODY_DECL_METHOD
} BodyDeclKind;

enum {
    BODY_DECL_EXPORT = 1u << 0
};

typedef struct {
    const char *name;
    BodyDeclKind kind;
    unsigned flags;
    IdmCore *(*handler)(ExpandContext *ctx, const IdmSyntax *form, IdmError *err);
} BodyDeclDesc;

static IdmCore *body_activate_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err);
static IdmCore *body_core_form_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err);
static IdmCore *body_core_reader_form_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err);
static IdmCore *body_core_operator_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err);
static IdmCore *body_core_grammar_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err);
static IdmCore *body_surface_grammar_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err);
static IdmCore *body_use_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err);
static IdmCore *body_import_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err);

static const BodyDeclDesc BODY_DECLS[] = {
    { "package", BODY_DECL_PACKAGE, 0, NULL },
    { "use", BODY_DECL_USE, 0, body_use_decl },
    { "import", BODY_DECL_IMPORT, 0, body_import_decl },
    { "protocol", BODY_DECL_PROTOCOL, BODY_DECL_EXPORT, expand_protocol_decl },
    { "activate", BODY_DECL_ACTIVATE, 0, body_activate_decl },
    { "core-form", BODY_DECL_CORE_FORM, 0, body_core_form_decl },
    { "core-reader-form", BODY_DECL_CORE_READER_FORM, 0, body_core_reader_form_decl },
    { "core-operator", BODY_DECL_CORE_OPERATOR, 0, body_core_operator_decl },
    { "core-grammar", BODY_DECL_CORE_GRAMMAR, 0, body_core_grammar_decl },
    { "grammar", BODY_DECL_GRAMMAR, BODY_DECL_EXPORT, body_surface_grammar_decl },
    { "type", BODY_DECL_TYPE, BODY_DECL_EXPORT, expand_type_decl },
    { "record", BODY_DECL_RECORD, BODY_DECL_EXPORT, expand_record_decl },
    { "trait", BODY_DECL_TRAIT, BODY_DECL_EXPORT, expand_trait_decl },
    { "implement", BODY_DECL_IMPLEMENT, 0, expand_implement_trait_decl },
    { "method", BODY_DECL_METHOD, 0, expand_method_decl },
};

static const BodyDeclDesc *body_decl_find(const char *name) {
    for (size_t i = 0; i < sizeof(BODY_DECLS) / sizeof(BODY_DECLS[0]); i++) {
        if (strcmp(BODY_DECLS[i].name, name) == 0) return &BODY_DECLS[i];
    }
    return NULL;
}

static const BodyDeclDesc *package_expr_head_index(const IdmSyntax *form, size_t *out_head_index) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR) || form->as.seq.count < 2u || form->as.seq.items[1]->kind != IDM_SYN_WORD) return NULL;
    size_t head_index = 1u;
    const IdmSyntax *head = form->as.seq.items[1];
    bool exported = false;
    if (syn_is_word(head, "export")) {
        if (form->as.seq.count < 3u || form->as.seq.items[2]->kind != IDM_SYN_WORD) return NULL;
        head_index = 2u;
        head = form->as.seq.items[2];
        exported = true;
    }
    const BodyDeclDesc *desc = body_decl_find(head->as.text);
    if (!desc) return NULL;
    if (exported && (desc->flags & BODY_DECL_EXPORT) == 0) return NULL;
    if (out_head_index) *out_head_index = head_index;
    return desc;
}

static const BodyDeclDesc *body_decl_form(const IdmSyntax *form) {
    const BodyDeclDesc *desc = package_expr_head_index(form, NULL);
    if (desc) return desc;
    if (core_operator_parts(form, NULL, NULL, NULL)) return body_decl_find("core-operator");
    return NULL;
}

static unsigned syn_max_line(const IdmSyntax *syn) {
    if (!syn) return 0;
    unsigned best = syn->span.line;
    if (syn->kind == IDM_SYN_LIST || syn->kind == IDM_SYN_VECTOR || syn->kind == IDM_SYN_TUPLE || syn->kind == IDM_SYN_DICT) {
        for (size_t i = 0; i < syn->as.seq.count; i++) {
            unsigned m = syn_max_line(syn->as.seq.items[i]);
            if (m > best) best = m;
        }
    }
    return best;
}

static bool phase_eval_body(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *phase_body, IdmError *err) {
    int saved_phase = ctx->phase;
    int saved_surface_phase = ctx->surface_phase;
    ctx->phase = 1;
    if (ctx->surface_phase < 0) ctx->surface_phase = saved_phase;
    IdmCore *ignored = expand_body_items(ctx, phase_body->as.seq.items, 1, phase_body->as.seq.count, false, err);
    if (ignored) ignored = wrap_kernel_use(ctx, ignored, err);
    if (ignored) ignored = wrap_phase_runtime_inits(ctx, ignored, err);
    bool ran = ignored != NULL && run_phase_core(ctx, ignored, err);
    if (ran) ctx->phase_runtime_init_mark_count = ctx->runtime_init_mark_count;
    ctx->phase = saved_phase;
    ctx->surface_phase = saved_surface_phase;
    idm_core_free(ignored);
    if (!ran && err && err->present) {
        idm_error_note_at(err, form->span, "during phase-1 evaluation");
    }
    return ran;
}

static bool owned_syntax_push(IdmSyntax ***owned, size_t *count, size_t *cap, IdmSyntax *syn, IdmError *err) {
    if (*count == *cap) {
        if (!idm_grow((void **)owned, cap, sizeof(**owned), 8u, *count + 1u)) return idm_error_oom(err, syn ? syn->span : idm_span_unknown(NULL));
    }
    (*owned)[(*count)++] = syn;
    return true;
}

static const IdmSyntax *expanded_splice_body(const IdmSyntax *expanded) {
    if (idm_syn_is_form_id(expanded, IDM_FORM_BODY)) return expanded;
    return NULL;
}

static bool body_install_expanded_syntax(IdmSyntax ***work, size_t *work_count, size_t *work_cap, IdmSyntax ***owned, size_t *owned_count, size_t *owned_cap, IdmScopeId extra_scope, size_t index, IdmSyntax *expanded, IdmError *err) {
    if (extra_scope != 0 && !idm_syn_scope_add_tree(expanded, 0, extra_scope)) {
        IdmSpan span = expanded ? expanded->span : idm_span_unknown(NULL);
        idm_syn_free(expanded);
        return idm_error_oom(err, span);
    }
    if (!owned_syntax_push(owned, owned_count, owned_cap, expanded, err)) {
        idm_syn_free(expanded);
        return false;
    }
    const IdmSyntax *splice_body = expanded_splice_body(expanded);
    if (splice_body) return body_work_splice(work, work_count, work_cap, index, splice_body, err);
    (*work)[index] = expanded;
    return true;
}

static IdmSyntax *flatten_degenerate_adjacent_expr(const IdmSyntax *form, IdmError *err) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR) || form->as.seq.count != 2u ||
        !idm_syn_is_form_id(form->as.seq.items[1], IDM_FORM_ADJACENT)) {
        return NULL;
    }
    IdmSyntax *flat = idm_syn_clone(form);
    if (!flat) {
        idm_error_oom(err, form->span);
        return NULL;
    }
    IdmSyntax *run = flat->as.seq.items[1];
    if (run->as.seq.count < 3u) return flat;

    size_t new_count = run->as.seq.count;
    IdmSyntax **new_items = malloc(new_count * sizeof(*new_items));
    if (!new_items) {
        idm_syn_free(flat);
        idm_error_oom(err, form->span);
        return NULL;
    }

    IdmSyntax **old_items = flat->as.seq.items;
    IdmSyntax **run_items = run->as.seq.items;
    new_items[0] = old_items[0];
    for (size_t i = 1; i < run->as.seq.count; i++) new_items[i] = run_items[i];

    IdmSyntax *adjacent_head = run_items[0];
    run->as.seq.items = NULL;
    run->as.seq.count = 0;
    run->as.seq.cap = 0;
    idm_syn_free(adjacent_head);
    idm_syn_free(run);
    free(run_items);
    free(old_items);

    flat->as.seq.items = new_items;
    flat->as.seq.count = new_count;
    flat->as.seq.cap = new_count;
    return flat;
}

static bool normalize_degenerate_adjacent_exprs(IdmSyntax **work, size_t work_count, IdmSyntax ***owned, size_t *owned_count, size_t *owned_cap, IdmError *err) {
    for (size_t i = 0; i < work_count; i++) {
        IdmSyntax *flat = flatten_degenerate_adjacent_expr(work[i], err);
        if (!flat) {
            if (err && err->present) return false;
            continue;
        }
        if (!owned_syntax_push(owned, owned_count, owned_cap, flat, err)) {
            idm_syn_free(flat);
            return false;
        }
        work[i] = flat;
    }
    return true;
}


typedef struct {
    const char *name;
    IdmPrimitive primitive;
    IdmArity arity;
    uint32_t slot;
} PrimitiveSeed;

static const char *declared_package_name(ExpandContext *ctx, IdmSyntax *const *work, size_t work_count, IdmError *err) {
    if (ctx->package_name) return ctx->package_name;
    for (size_t i = 0; i < work_count; i++) {
        const IdmSyntax *form = work[i];
        if (!idm_syn_is_form_id(form, IDM_FORM_EXPR) || form->as.seq.count != 3u ||
            !syn_is_word(form->as.seq.items[1], "package") ||
            form->as.seq.items[2]->kind != IDM_SYN_WORD) {
            continue;
        }
        const char *interned = idm_symbol_text(idm_intern(&ctx->rt->intern, IDM_SYMBOL_WORD, form->as.seq.items[2]->as.text));
        if (!interned) {
            idm_error_oom(err, form->span);
            return NULL;
        }
        ctx->package_name = interned;
        return interned;
    }
    if (ctx->primitive_home && strcmp(ctx->primitive_home, "kernel") == 0) return ctx->primitive_home;
    return NULL;
}

static bool primitive_seeds_push(PrimitiveSeed **seeds, size_t *count, size_t *cap, const char *name, IdmPrimitive primitive, IdmArity arity, uint32_t slot, IdmError *err, IdmSpan span) {
    if (*count == *cap) {
        if (!idm_grow((void **)seeds, cap, sizeof(**seeds), 16u, *count + 1u)) return idm_error_oom(err, span);
    }
    (*seeds)[*count].name = name;
    (*seeds)[*count].primitive = primitive;
    (*seeds)[*count].arity = arity;
    (*seeds)[*count].slot = slot;
    (*count)++;
    return true;
}

static bool primitive_seed_exports(const char *home) {
    return !home || strcmp(home, "regex") != 0;
}

static bool primitive_contract_for_slot(ExpandContext *ctx, IdmPrimitive primitive, IdmArity arity, IdmCallableContract *contract, IdmError *err, IdmSpan span) {
    memset(contract, 0, sizeof(*contract));
    bool typed = false;
    if (arity.kind != IDM_ARITY_UNKNOWN && arity.min == arity.max && idm_arity_accepts(&arity, arity.min) &&
        !idm_primitive_contract(ctx->rt, primitive, arity.min, contract, &typed, err, span)) {
        return false;
    }
    contract->passthrough = true;
    contract->primitive = (uint32_t)primitive;
    return true;
}

static bool seed_virtual_package_primitives(ExpandContext *ctx, IdmSyntax *const *work, size_t work_count, IdmScopeId package_scope, PrimitiveSeed **out_seeds, size_t *out_count, IdmError *err) {
    *out_seeds = NULL;
    *out_count = 0;
    const char *declared = declared_package_name(ctx, work, work_count, err);
    if (!declared) return !(err && err->present);
    const char *home = ctx->primitive_home;
    if (!home) return true;
    if (strcmp(declared, home) != 0) {
        return idm_error_set(err, idm_span_unknown(NULL), "package '%s' is not the canonical home for '%s' primitives", declared, home);
    }
    PrimitiveSeed *seeds = NULL;
    size_t seed_count = 0;
    size_t seed_cap = 0;
    for (size_t i = 0; i < idm_primitive_count(); i++) {
        IdmPrimitive primitive = (IdmPrimitive)i;
        const IdmPrimitiveInfo *info = idm_primitive_info(primitive);
        if (!info || strcmp(idm_primitive_home(primitive), home) != 0) continue;
        IdmSyntax *name_syntax = idm_syn_word(info->name, idm_span_unknown(NULL));
        if (!name_syntax) {
            free(seeds);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        bool ok = package_scope == 0 || idm_syn_scope_add(name_syntax, 0, package_scope);
        IdmArity arity = idm_primitive_arity(primitive);
        IdmScopeSet scopes;
        idm_scope_set_init(&scopes);
        uint32_t slot = 0;
        IdmBindingId binding_id = 0;
        IdmCallableContract contract;
        memset(&contract, 0, sizeof(contract));
        if (ok) ok = binder_scopes_pruned(ctx, name_syntax, &scopes);
        if (ok) ok = primitive_contract_for_slot(ctx, primitive, arity, &contract, err, name_syntax->span);
        if (ok) {
            const IdmBinding *existing = NULL;
            if (body_existing_env_binding(ctx, info->name, &scopes, &existing)) {
                slot = existing->payload;
                binding_id = existing->id;
            } else {
                slot = ctx->env_slot_seq++;
                ok = idm_binding_table_add_with_arity(&ctx->bindings, info->name, IDM_PHASE_ANY, IDM_BIND_SPACE_DEFAULT, IDM_BIND_ENV, &scopes, slot, IDM_FRAME_ENV, arity, &binding_id);
                if (!ok) idm_error_oom(err, name_syntax->span);
            }
        }
        if (ok) ok = idm_binding_table_set_contract(&ctx->bindings, binding_id, &contract);
        if (ok) ok = record_package_slot(ctx, info->name, slot, &scopes, arity, &contract, primitive_seed_exports(home));
        if (ok) ok = primitive_seeds_push(&seeds, &seed_count, &seed_cap, info->name, primitive, arity, slot, err, name_syntax->span);
        idm_callable_contract_destroy(&contract);
        idm_scope_set_destroy(&scopes);
        idm_syn_free(name_syntax);
        if (!ok) {
            free(seeds);
            if (err && !err->present) idm_error_oom(err, idm_span_unknown(NULL));
            return false;
        }
    }
    *out_seeds = seeds;
    *out_count = seed_count;
    return true;
}

static IdmCore *wrap_virtual_primitive_seeds(IdmCore *body, const PrimitiveSeed *seeds, size_t seed_count, IdmError *err) {
    if (!body || seed_count == 0) return body;
    IdmCore *prelude = idm_core_letrec(idm_span_unknown(NULL));
    bool ok = prelude != NULL;
    if (ok) idm_core_letrec_set_env(prelude);
    for (size_t i = 0; ok && i < seed_count; i++) {
        IdmCore *value = idm_core_primitive_backed_fn(seeds[i].name, seeds[i].primitive, seeds[i].arity, idm_span_unknown(NULL));
        ok = value != NULL && idm_core_letrec_add(prelude, seeds[i].name, seeds[i].slot, value);
        if (!ok && value) idm_core_free(value);
    }
    if (ok) ok = idm_core_letrec_set_body(prelude, body);
    if (!ok) {
        idm_core_free(prelude);
        idm_core_free(body);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    return prelude;
}

static bool run_virtual_primitive_seed_phase(ExpandContext *ctx, const PrimitiveSeed *seeds, size_t seed_count, IdmError *err) {
    if (seed_count == 0) return true;
    IdmCore *body = idm_core_literal(idm_nil(), idm_span_unknown(NULL));
    IdmCore *init = body ? wrap_virtual_primitive_seeds(body, seeds, seed_count, err) : NULL;
    if (!init) {
        if (!body && err && !err->present) idm_error_oom(err, idm_span_unknown(NULL));
        return false;
    }
    bool ok = run_phase_core(ctx, init, err);
    idm_core_free(init);
    return ok;
}

IdmCore *expand_package_body_items(ExpandContext *ctx, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    size_t work_count = count > index ? count - index : 0u;
    size_t work_cap = work_count ? work_count : 1u;
    IdmSyntax **work = malloc(work_cap * sizeof(*work));
    if (!work) return (IdmCore *)(uintptr_t)idm_error_oom(err, index < count ? items[index]->span : idm_span_unknown(NULL));
    for (size_t i = 0; i < work_count; i++) work[i] = items[index + i];
    IdmSyntax **owned = NULL;
    size_t owned_count = 0;
    size_t owned_cap = 0;

    if (!normalize_degenerate_adjacent_exprs(work, work_count, &owned, &owned_count, &owned_cap, err)) {
        for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
        free(owned);
        free(work);
        return NULL;
    }

    IdmScopeId package_scope = idm_scope_fresh(&ctx->scope_store);
    ctx->unit_package_scope = package_scope;
    if (work_count != 0 && !body_scope_add_range(ctx, work, 0, work_count, package_scope, work[0]->span, err)) {
        for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
        free(owned);
        free(work);
        return NULL;
    }

    PrimitiveSeed *primitive_seeds = NULL;
    size_t primitive_seed_count = 0;
    if (!seed_virtual_package_primitives(ctx, work, work_count, package_scope, &primitive_seeds, &primitive_seed_count, err)) {
        for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
        free(owned);
        free(work);
        return NULL;
    }
    if (!run_virtual_primitive_seed_phase(ctx, primitive_seeds, primitive_seed_count, err)) {
        free(primitive_seeds);
        for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
        free(owned);
        free(work);
        return NULL;
    }

    IdmCore *body = expand_body_items(ctx, work, 0, work_count, false, err);
    if (body) body = wrap_virtual_primitive_seeds(body, primitive_seeds, primitive_seed_count, err);
    free(primitive_seeds);
    for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
    free(owned);
    free(work);
    return body;
}

static IdmCore *expand_package_units_bootstrap(ExpandContext *ctx, const IdmPackageUnit *units, size_t unit_count, IdmError *err) {
    IdmScopeId package_scope = idm_scope_fresh(&ctx->scope_store);
    ctx->unit_package_scope = package_scope;
    IdmCore *acc = NULL;
    for (size_t u = 0; u < unit_count; u++) {
        ctx->unit_source = units[u].source;
        ctx->unit_label = units[u].label;
        ctx->unit_reader_generation = ctx->reader_generation;
        IdmSyntax *pkg = NULL;
        if (!expand_unit_read(ctx, units[u].label, units[u].source, 0, &pkg, err)) {
            idm_core_free(acc);
            return NULL;
        }
        if (!idm_syn_is_form_id(pkg, IDM_FORM_PACKAGE_BEGIN)) {
            idm_syn_free(pkg);
            idm_core_free(acc);
            return (IdmCore *)(uintptr_t)idm_error_set(err, idm_span_unknown(units[u].label), "package source must be a program");
        }
        if (!idm_syn_scope_add_tree(pkg, 0, package_scope)) {
            idm_syn_free(pkg);
            idm_core_free(acc);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, idm_span_unknown(units[u].label));
        }
        PrimitiveSeed *seeds = NULL;
        size_t seed_count = 0;
        bool seeded = seed_virtual_package_primitives(ctx, pkg->as.seq.items + 1, pkg->as.seq.count - 1u, package_scope, &seeds, &seed_count, err) &&
                      run_virtual_primitive_seed_phase(ctx, seeds, seed_count, err);
        IdmCore *seg = seeded ? expand_body_items(ctx, pkg->as.seq.items, 1, pkg->as.seq.count, false, err) : NULL;
        if (seg) seg = wrap_virtual_primitive_seeds(seg, seeds, seed_count, err);
        free(seeds);
        idm_syn_free(pkg);
        if (!seg) {
            idm_core_free(acc);
            return NULL;
        }
        if (!acc) {
            acc = seg;
        } else {
            IdmCore *joined = idm_core_do(idm_span_unknown(units[u].label));
            if (!joined || !idm_core_do_add(joined, acc) || !idm_core_do_add(joined, seg)) {
                idm_core_free(joined);
                idm_core_free(acc);
                idm_core_free(seg);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, idm_span_unknown(units[u].label));
            }
            acc = joined;
        }
    }
    ctx->unit_source = NULL;
    ctx->unit_label = NULL;
    return acc ? acc : idm_core_literal(idm_nil(), idm_span_unknown(NULL));
}

IdmCore *expand_package_units(ExpandContext *ctx, const IdmPackageUnit *units, size_t unit_count, IdmError *err) {
    if (ctx->kernel_mode) return expand_package_units_bootstrap(ctx, units, unit_count, err);
    IdmSyntax **programs = calloc(unit_count ? unit_count : 1u, sizeof(*programs));
    if (!programs) return (IdmCore *)(uintptr_t)idm_error_oom(err, idm_span_unknown(NULL));
    size_t item_count = 0;
    bool ok = true;
    for (size_t u = 0; ok && u < unit_count; u++) {
        ctx->unit_source = units[u].source;
        ctx->unit_label = units[u].label;
        ctx->unit_reader_generation = ctx->reader_generation;
        ok = expand_unit_read(ctx, units[u].label, units[u].source, 0, &programs[u], err);
        if (!ok) break;
        if (!idm_syn_is_form_id(programs[u], IDM_FORM_PACKAGE_BEGIN)) {
            idm_error_set(err, idm_span_unknown(units[u].label), "package source must be a program");
            ok = false;
            break;
        }
        size_t count = programs[u]->as.seq.count - 1u;
        if (count > SIZE_MAX - item_count) {
            idm_error_set(err, idm_span_unknown(units[u].label), "package contains too many forms");
            ok = false;
            break;
        }
        item_count += count;
    }
    IdmSyntax **items = ok && item_count ? malloc(item_count * sizeof(*items)) : NULL;
    if (ok && item_count && !items) {
        idm_error_oom(err, idm_span_unknown(NULL));
        ok = false;
    }
    size_t at = 0;
    for (size_t u = 0; ok && u < unit_count; u++) {
        for (size_t i = 1u; i < programs[u]->as.seq.count; i++) items[at++] = programs[u]->as.seq.items[i];
    }
    IdmCore *body = ok ? expand_package_body_items(ctx, items, 0u, item_count, err) : NULL;
    free(items);
    for (size_t u = 0; u < unit_count; u++) idm_syn_free(programs[u]);
    free(programs);
    ctx->unit_source = NULL;
    ctx->unit_label = NULL;
    return body;
}

static IdmCore *expand_decl_fallback(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err) {
    IdmCore *core = idm_syn_is_form_id(bform, IDM_FORM_EXPR)
        ? expand_parts(ctx, bform->as.seq.items, 1, bform->as.seq.count, err)
        : expand_syntax(ctx, bform, err);
    if (core && ctx->frame == IDM_FRAME_TOP && !expand_typecheck_statement(ctx, &core, err)) {
        idm_core_free(core);
        return NULL;
    }
    return core;
}

IdmCore *decl_done(IdmError *err) {
    IdmCore *core = idm_core_literal(idm_nil(), idm_span_unknown(NULL));
    if (!core) idm_error_oom(err, idm_span_unknown(NULL));
    return core;
}

static IdmCore *body_activate_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err) {
    if (bform->as.seq.count < 3u) return expand_decl_fallback(ctx, bform, err);
    size_t name_end = bform->as.seq.count;
    IdmSyntax *name = expect_qualified_word_at(bform->as.seq.items, 2u, &name_end, "activate expects a single protocol name", err);
    if (!name) return NULL;
    if (name_end != bform->as.seq.count) {
        idm_syn_free(name);
        expand_error(err, bform->span, "activate expects a single protocol name");
        return NULL;
    }
    IdmCore *core = expand_activate(ctx, name, bform->span, err);
    idm_syn_free(name);
    return core;
}

static IdmCore *body_core_form_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err) {
    size_t head = 0;
    const BodyDeclDesc *desc = package_expr_head_index(bform, &head);
    if (!desc || desc->kind != BODY_DECL_CORE_FORM) return expand_decl_fallback(ctx, bform, err);
    if (bform->as.seq.count != head + 3u || bform->as.seq.items[head + 1u]->kind != IDM_SYN_WORD ||
        !idm_syn_is_form_id(bform->as.seq.items[head + 2u], IDM_FORM_BODY)) {
        return expand_error(err, bform->span, "core-form expects a head name before do; use core-form _ do for unresolved-head fallback");
    }
    const IdmSyntax *name_syntax = bform->as.seq.items[head + 1u];
    const IdmSyntax *body = bform->as.seq.items[head + 2u];
    int saved_phase = ctx->phase;
    ctx->phase = 1;
    IdmCore *fn = build_clause_fn(ctx, body, body->as.seq.count, name_syntax->as.text, err);
    ctx->phase = saved_phase;
    if (!fn) return NULL;
    if (!typecheck_transformer(ctx, name_syntax->as.text, fn, err)) {
        idm_core_free(fn);
        return NULL;
    }
    bool ok = register_core_form(ctx, name_syntax, fn, bform->span, err);
    idm_core_free(fn);
    return ok ? decl_done(err) : NULL;
}

static IdmCore *body_core_reader_form_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err) {
    size_t head = 0;
    const BodyDeclDesc *desc = package_expr_head_index(bform, &head);
    if (!desc || desc->kind != BODY_DECL_CORE_READER_FORM) return expand_decl_fallback(ctx, bform, err);
    if (bform->as.seq.count != head + 3u || bform->as.seq.items[head + 1u]->kind != IDM_SYN_STRING) {
        return expand_error(err, bform->span, "core-reader-form expects: core-reader-form \"form-name\" INSTRUCTION or a transformer body");
    }
    const char *name = bform->as.seq.items[head + 1u]->as.text;
    const IdmSyntax *spec = bform->as.seq.items[head + 2u];
    const IdmScopeSet *decl_scopes = idm_syn_scope_set(bform->as.seq.items[0], 0);
    if (idm_syn_is_form_id(spec, IDM_FORM_BODY)) {
        int saved_phase = ctx->phase;
        ctx->phase = 1;
        IdmCore *fn = build_clause_fn(ctx, spec, spec->as.seq.count, name, err);
        ctx->phase = saved_phase;
        if (!fn) return NULL;
        if (!typecheck_transformer(ctx, name, fn, err)) {
            idm_core_free(fn);
            return NULL;
        }
        bool ok = register_reader_form_transformer(ctx, name, decl_scopes, fn, bform->span, err);
        idm_core_free(fn);
        if (!ok) return NULL;
        return decl_done(err);
    }
    IdmEnforestNode *node = idm_enforest_parse(spec, err);
    if (!node) return NULL;
    bool ok = register_reader_form(ctx, name, decl_scopes, node, err);
    idm_enforest_free(node);
    if (!ok) return NULL;
    return decl_done(err);
}

static IdmCore *body_core_operator_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err) {
    return expand_core_operator_decl(ctx, bform, err) ? decl_done(err) : NULL;
}

static IdmCore *body_core_grammar_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err) {
    return expand_core_grammar_decl(ctx, bform, err) ? decl_done(err) : NULL;
}

static IdmCore *body_surface_grammar_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err) {
    return expand_surface_grammar_decl(ctx, bform, err) ? decl_done(err) : NULL;
}

static IdmCore *body_use_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err) {
    if (bform->as.seq.count < 3u || package_path_text(bform->as.seq.items[2]) == NULL) return expand_decl_fallback(ctx, bform, err);
    UseSelection selection;
    size_t pos = 3u;
    if (!use_selection_parse(bform, pos, bform->as.seq.count, &pos, &selection, err)) return NULL;
    if (pos != bform->as.seq.count) {
        use_selection_destroy(&selection);
        return expand_error(err, bform->span, "use expects 'use path' or 'use path with [name ...]'");
    }
    IdmCore *core = expand_use(ctx, package_path_text(bform->as.seq.items[2]), NULL, &selection, bform, bform->span, err);
    use_selection_destroy(&selection);
    return core;
}

static IdmCore *body_import_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmError *err) {
    if (bform->as.seq.count < 3u || package_path_text(bform->as.seq.items[2]) == NULL) return expand_decl_fallback(ctx, bform, err);
    const char *path = package_path_text(bform->as.seq.items[2]);
    const char *alias = NULL;
    UseSelection selection;
    size_t pos = 3u;
    if (!use_selection_parse(bform, pos, bform->as.seq.count, &pos, &selection, err)) return NULL;
    if (pos == bform->as.seq.count) {
        const char *slash = strrchr(path, '/');
        alias = slash ? slash + 1u : path;
    } else if (pos + 2u == bform->as.seq.count && syn_is_word(bform->as.seq.items[pos], "as") && bform->as.seq.items[pos + 1u]->kind == IDM_SYN_WORD) {
        alias = bform->as.seq.items[pos + 1u]->as.text;
    } else {
        use_selection_destroy(&selection);
        return expand_error(err, bform->span, "import expects 'import path', 'import path as alias', or 'import path with [name ...] as alias'");
    }
    IdmCore *core = expand_use(ctx, path, alias, &selection, bform, bform->span, err);
    use_selection_destroy(&selection);
    return core;
}

static bool body_prepare_declarations(ExpandContext *ctx, IdmSyntax **work, size_t *work_count, bool imports, IdmCore ***prepared, size_t *prepared_count, size_t *prepared_cap, IdmError *err) {
    size_t i = 0;
    while (i < *work_count) {
        const BodyDeclDesc *desc = body_decl_form(work[i]);
        bool import_decl = desc && (desc->kind == BODY_DECL_USE || desc->kind == BODY_DECL_IMPORT);
        if (!desc || !desc->handler || import_decl != imports) {
            i++;
            continue;
        }
        IdmCore *core = desc->handler(ctx, work[i], err);
        if (!core) return false;
        if (*prepared_count == *prepared_cap &&
            !idm_grow((void **)prepared, prepared_cap, sizeof(**prepared), 4u, *prepared_count + 1u)) {
            idm_core_free(core);
            return idm_error_oom(err, work[i]->span);
        }
        (*prepared)[(*prepared_count)++] = core;
        memmove(work + i, work + i + 1u, (*work_count - i - 1u) * sizeof(*work));
        (*work_count)--;
    }
    return true;
}

static bool body_def_binding_between(ExpandContext *ctx, size_t start, size_t end, const char *name, const IdmSyntax *name_syntax, const IdmBinding **out, IdmError *err) {
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return idm_error_oom(err, name_syntax->span);
    for (size_t i = end; i > start; i--) {
        const IdmBinding *binding = &ctx->bindings.items[i - 1u];
        if ((binding->phase == ctx->phase || binding->phase == IDM_PHASE_ANY) &&
            binding->space == IDM_BIND_SPACE_DEFAULT &&
            (binding->kind == IDM_BIND_ENV || binding->kind == IDM_BIND_LOCAL) &&
            strcmp(binding->name, name) == 0 && idm_scope_set_equal(&binding->scopes, &scopes)) {
            idm_scope_set_destroy(&scopes);
            *out = binding;
            return true;
        }
    }
    idm_scope_set_destroy(&scopes);
    *out = NULL;
    return true;
}

static bool phase_candidate_matches(const ExpandContext *ctx, const char *name, const IdmSyntax *form) {
    for (size_t i = 0; i < ctx->phase_candidate_count; i++) {
        const PhaseCandidate *cand = &ctx->phase_candidates[i];
        if (strcmp(cand->name, name) != 0 || cand->count == 0) continue;
        IdmSpan a = cand->items[0]->span;
        IdmSpan b = form->span;
        bool same_file = a.file == b.file || (a.file && b.file && strcmp(a.file, b.file) == 0);
        if (same_file && a.start == b.start) return true;
    }
    return false;
}

static bool body_prebind_defns(ExpandContext *ctx, IdmSyntax *const *work, size_t work_count, IdmError *err) {
    DefnGroup *groups = NULL;
    size_t group_count = 0;
    size_t group_cap = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < work_count; i++) {
        const IdmSyntax *name = NULL;
        size_t ignored = 0;
        bool exported = false;
        if (!defn_form_parts(work[i], &name, &ignored, &exported)) continue;
        DefnGroup *group = find_or_add_group(&groups, &group_count, &group_cap, name);
        ok = group != NULL && group_add_index(group, i);
        if (group && exported) group->exported = true;
        if (!ok) idm_error_oom(err, work[i]->span);
    }
    for (size_t i = 0; ok && i < group_count; i++) {
        ok = defn_group_arity(ctx, &groups[i], work, &groups[i].arity, err);
        if (!ok) continue;
        if (ctx->frame == IDM_FRAME_TOP) {
            ok = ctx->in_package
                ? body_env_def_binder_with_arity(ctx, groups[i].name, groups[i].name_syntax, groups[i].arity, NULL, true, &groups[i].slot, NULL, err)
                : env_push_def_binder_with_arity(ctx, groups[i].name, groups[i].name_syntax, groups[i].arity, NULL, &groups[i].slot);
        } else {
            ok = local_push_def_binder_with_arity(ctx, groups[i].name, groups[i].name_syntax, groups[i].arity, NULL, &groups[i].slot);
        }
        if (ok && ctx->in_package && ctx->frame == IDM_FRAME_TOP) {
            ok = record_package_slot(ctx, groups[i].name, groups[i].slot, &groups[i].scopes, groups[i].arity, NULL, groups[i].exported);
        }
        if (!ok && !(err && err->present)) idm_error_oom(err, groups[i].name_syntax->span);
    }
    if (ok && ctx->frame == IDM_FRAME_TOP && ctx->phase == 0) {
        size_t i = 0;
        while (ok && i < work_count) {
            const IdmSyntax *name = NULL;
            size_t ignored = 0;
            bool exported = false;
            if (!defn_form_parts(work[i], &name, &ignored, &exported)) { i++; continue; }
            size_t j = i;
            DefnGroup *run = NULL;
            size_t run_count = 0;
            size_t run_cap = 0;
            while (j < work_count) {
                const IdmSyntax *run_name = NULL;
                if (!defn_form_parts(work[j], &run_name, &ignored, &exported)) break;
                DefnGroup *group = find_or_add_group(&run, &run_count, &run_cap, run_name);
                if (!group || !group_add_index(group, j)) { ok = idm_error_oom(err, work[j]->span); break; }
                j++;
            }
            size_t run_id = phase_candidate_next_run(ctx);
            for (size_t k = 0; ok && k < run_count; k++) {
                if (!phase_candidate_stage(ctx, run[k].name, run[k].name_syntax, work + i, j - i, run_id, err)) ok = false;
            }
            defn_groups_destroy(run, run_count);
            i = j;
        }
    }
    defn_groups_destroy(groups, group_count);
    return ok;
}

typedef struct {
    BodyRec *recs;
    size_t count;
    size_t *locals;
    size_t local_count;
    size_t *envs;
    size_t env_count;
    size_t function_depth;
} BodyDepScan;

static bool body_rec_add_dep(BodyRec *rec, size_t dep) {
    for (size_t i = 0; i < rec->dep_count; i++) if (rec->deps[i] == dep) return true;
    if (rec->dep_count == rec->dep_cap &&
        !idm_grow((void **)&rec->deps, &rec->dep_cap, sizeof(*rec->deps), 4u, rec->dep_count + 1u)) return false;
    rec->deps[rec->dep_count++] = dep;
    return true;
}

static bool body_dep_slot(BodyDepScan *scan, size_t owner, bool env, uint32_t slot) {
    size_t dep = env ? (slot < scan->env_count ? scan->envs[slot] : SIZE_MAX)
                     : (slot < scan->local_count ? scan->locals[slot] : SIZE_MAX);
    return dep == SIZE_MAX || body_rec_add_dep(&scan->recs[owner], dep);
}

static bool body_dep_name(BodyDepScan *scan, size_t owner, const char *name) {
    if (!name) return true;
    for (size_t i = scan->count; i > 0; i--) {
        BodyRec *rec = &scan->recs[i - 1u];
        if (rec->kind == BODY_REC_BIND && rec->bind_name && strcmp(rec->bind_name->as.text, name) == 0) return body_rec_add_dep(&scan->recs[owner], i - 1u);
        if (rec->kind == BODY_REC_BIND_PATTERN) {
            for (size_t n = 0; n < rec->pattern_name_count; n++) if (strcmp(rec->pattern_names[n]->as.text, name) == 0) return body_rec_add_dep(&scan->recs[owner], i - 1u);
        }
        if (rec->kind == BODY_REC_GROUPS) {
            for (size_t n = 0; n < rec->group_count; n++) if (strcmp(rec->groups[n].name, name) == 0) return body_rec_add_dep(&scan->recs[owner], i - 1u);
        }
    }
    return true;
}

static bool body_dep_binding(BodyDepScan *scan, size_t owner, IdmBindingId binding_id, const char *name) {
    if (binding_id != 0) {
        for (size_t i = 0; i < scan->count; i++) {
            BodyRec *rec = &scan->recs[i];
            if (rec->kind == BODY_REC_BIND && rec->bind_binding_id == binding_id) return body_rec_add_dep(&scan->recs[owner], i);
            if (rec->kind == BODY_REC_BIND_PATTERN) {
                for (size_t n = 0; n < rec->pattern_name_count; n++) if (rec->pattern_binding_ids[n] == binding_id) return body_rec_add_dep(&scan->recs[owner], i);
            }
        }
    }
    return body_dep_name(scan, owner, name);
}

static bool body_dep_captures(BodyDepScan *scan, size_t owner, const IdmCapture *captures, size_t count) {
    if (scan->function_depth != 0) return true;
    for (size_t i = 0; i < count; i++) {
        if (captures[i].kind == IDM_CAP_LOCAL && !body_dep_slot(scan, owner, false, captures[i].index)) return false;
        if (captures[i].kind == IDM_CAP_UPVALUE && !body_dep_binding(scan, owner, captures[i].binding_id, captures[i].name)) return false;
    }
    return true;
}

static bool body_core_deps(BodyDepScan *scan, size_t owner, const IdmCore *core) {
    if (!core) return true;
    switch (core->kind) {
        case IDM_CORE_DISPATCH:
            for (size_t i = 0; i < core->as.dispatch.arg_count; i++) {
                if (!body_core_deps(scan, owner, core->as.dispatch.args[i])) return false;
            }
            for (size_t i = 0; i < core->as.dispatch.method_count; i++) {
                if (!body_core_deps(scan, owner, core->as.dispatch.methods[i].evidence)) return false;
            }
            for (size_t i = 0; i < core->as.dispatch.impl_count; i++) {
                if (!body_core_deps(scan, owner, core->as.dispatch.impls[i].ref)) return false;
            }
            return body_core_deps(scan, owner, core->as.dispatch.fallback);
        case IDM_CORE_LOCAL_REF:
            return scan->function_depth != 0 || body_dep_slot(scan, owner, false, core->as.slot_ref.slot);
        case IDM_CORE_ENV_REF:
            return body_dep_slot(scan, owner, true, core->as.slot_ref.slot);
        case IDM_CORE_CALL:
            if (!body_core_deps(scan, owner, core->as.call.callee)) return false;
            for (size_t i = 0; i < core->as.call.arg_count; i++) if (!body_core_deps(scan, owner, core->as.call.args[i])) return false;
            return true;
        case IDM_CORE_LIST_CONS:
        case IDM_CORE_LIST_APPEND:
            return body_core_deps(scan, owner, core->as.list_pair.head) && body_core_deps(scan, owner, core->as.list_pair.tail);
        case IDM_CORE_VALUE_SEQUENCE:
            for (size_t i = 0; i < core->as.value_sequence.count; i++) if (!body_core_deps(scan, owner, core->as.value_sequence.items[i])) return false;
            return true;
        case IDM_CORE_FORM_BUILD:
            return body_core_deps(scan, owner, core->as.syntax_build.ctx) && body_core_deps(scan, owner, core->as.syntax_build.payload);
        case IDM_CORE_STRING_CONCAT:
            for (size_t i = 0; i < core->as.string_concat.count; i++) if (!body_core_deps(scan, owner, core->as.string_concat.items[i])) return false;
            return true;
        case IDM_CORE_COND:
            return body_core_deps(scan, owner, core->as.cond_expr.cond) &&
                   body_core_deps(scan, owner, core->as.cond_expr.then_branch) &&
                   body_core_deps(scan, owner, core->as.cond_expr.else_branch);
        case IDM_CORE_MATCH:
            if (!body_dep_captures(scan, owner, core->as.match_expr.captures, core->as.match_expr.capture_count)) return false;
            for (size_t i = 0; i < core->as.match_expr.scrutinee_count; i++) if (!body_core_deps(scan, owner, core->as.match_expr.scrutinees[i])) return false;
            scan->function_depth++;
            for (size_t i = 0; i < core->as.match_expr.count; i++) {
                if (!body_core_deps(scan, owner, core->as.match_expr.clauses[i].guard) ||
                    !body_core_deps(scan, owner, core->as.match_expr.clauses[i].body)) {
                    scan->function_depth--;
                    return false;
                }
            }
            scan->function_depth--;
            return true;
        case IDM_CORE_DO:
            for (size_t i = 0; i < core->as.do_expr.count; i++) if (!body_core_deps(scan, owner, core->as.do_expr.items[i])) return false;
            return true;
        case IDM_CORE_BIND_LOCAL:
            return body_core_deps(scan, owner, core->as.bind_local.value) && body_core_deps(scan, owner, core->as.bind_local.body);
        case IDM_CORE_MATCH_BIND:
            return body_core_deps(scan, owner, core->as.match_bind.pattern_fn) &&
                   body_core_deps(scan, owner, core->as.match_bind.value) &&
                   body_core_deps(scan, owner, core->as.match_bind.body);
        case IDM_CORE_FN:
            if (!body_dep_captures(scan, owner, core->as.fn.captures, core->as.fn.capture_count)) return false;
            scan->function_depth++;
            {
                bool ok = body_core_deps(scan, owner, core->as.fn.guard) && body_core_deps(scan, owner, core->as.fn.body);
                scan->function_depth--;
                return ok;
            }
        case IDM_CORE_FN_MULTI:
            if (!body_dep_captures(scan, owner, core->as.fn_multi.captures, core->as.fn_multi.capture_count)) return false;
            scan->function_depth++;
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                if (!body_core_deps(scan, owner, core->as.fn_multi.clauses[i].guard) ||
                    !body_core_deps(scan, owner, core->as.fn_multi.clauses[i].body)) {
                    scan->function_depth--;
                    return false;
                }
            }
            scan->function_depth--;
            return true;
        case IDM_CORE_LETREC:
            for (size_t i = 0; i < core->as.letrec.count; i++) if (!body_core_deps(scan, owner, core->as.letrec.bindings[i].value)) return false;
            return body_core_deps(scan, owner, core->as.letrec.body);
        case IDM_CORE_RECEIVE:
            return body_core_deps(scan, owner, core->as.receive.receiver) &&
                   body_core_deps(scan, owner, core->as.receive.timeout) &&
                   body_core_deps(scan, owner, core->as.receive.timeout_body);
        case IDM_CORE_GUARD:
            return body_core_deps(scan, owner, core->as.guard.body) &&
                   body_core_deps(scan, owner, core->as.guard.handler) &&
                   body_core_deps(scan, owner, core->as.guard.cleanup);
        case IDM_CORE_USE_PACKAGE:
            return body_core_deps(scan, owner, core->as.use_package.cont);
        case IDM_CORE_RECORD_CONSTRUCT:
            for (size_t i = 0; i < core->as.record_construct.count; i++) if (!body_core_deps(scan, owner, core->as.record_construct.field_values[i])) return false;
            return true;
        case IDM_CORE_RECORD_FIELD:
            return body_core_deps(scan, owner, core->as.record_field.receiver);
        case IDM_CORE_RECORD_IS:
            return body_core_deps(scan, owner, core->as.record_is.value);
        case IDM_CORE_LITERAL:
        case IDM_CORE_ARG_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_PACKAGE_REF:
            return true;
    }
    return true;
}

static const char *body_rec_key(const BodyRec *rec) {
    if (rec->kind == BODY_REC_BIND && rec->bind_name) return rec->bind_name->as.text;
    if (rec->kind == BODY_REC_BIND_PATTERN && rec->pattern_name_count != 0) return rec->pattern_names[0]->as.text;
    if (rec->kind == BODY_REC_GROUPS && rec->group_count != 0) return rec->groups[0].name;
    return "~expression";
}

typedef struct {
    BodyRec *recs;
    size_t count;
    size_t next;
    size_t comp_count;
    size_t stack_count;
    size_t *index;
    size_t *low;
    size_t *stack;
    size_t *comp;
    bool *on_stack;
} BodyTarjan;

static void body_tarjan_visit(BodyTarjan *t, size_t node) {
    t->index[node] = t->next;
    t->low[node] = t->next++;
    t->stack[t->stack_count++] = node;
    t->on_stack[node] = true;
    for (size_t i = 0; i < t->recs[node].dep_count; i++) {
        size_t dep = t->recs[node].deps[i];
        if (t->index[dep] == SIZE_MAX) {
            body_tarjan_visit(t, dep);
            if (t->low[dep] < t->low[node]) t->low[node] = t->low[dep];
        } else if (t->on_stack[dep] && t->index[dep] < t->low[node]) {
            t->low[node] = t->index[dep];
        }
    }
    if (t->low[node] != t->index[node]) return;
    for (;;) {
        size_t member = t->stack[--t->stack_count];
        t->on_stack[member] = false;
        t->comp[member] = t->comp_count;
        if (member == node) break;
    }
    t->comp_count++;
}

static bool body_component_less(const BodyRec *recs, const size_t *rep, const size_t *epoch, const bool *effect, size_t a, size_t b) {
    if (epoch[a] != epoch[b]) return epoch[a] < epoch[b];
    if (effect[a] != effect[b]) return !effect[a];
    int cmp = strcmp(body_rec_key(&recs[rep[a]]), body_rec_key(&recs[rep[b]]));
    return cmp < 0 || (cmp == 0 && rep[a] < rep[b]);
}

static void body_heap_push(size_t *heap, size_t *count, size_t value, const BodyRec *recs, const size_t *rep, const size_t *epoch, const bool *effect) {
    size_t at = (*count)++;
    heap[at] = value;
    while (at != 0) {
        size_t parent = (at - 1u) / 2u;
        if (!body_component_less(recs, rep, epoch, effect, heap[at], heap[parent])) break;
        size_t tmp = heap[parent];
        heap[parent] = heap[at];
        heap[at] = tmp;
        at = parent;
    }
}

static size_t body_heap_pop(size_t *heap, size_t *count, const BodyRec *recs, const size_t *rep, const size_t *epoch, const bool *effect) {
    size_t out = heap[0];
    heap[0] = heap[--(*count)];
    size_t at = 0;
    for (;;) {
        size_t left = at * 2u + 1u;
        if (left >= *count) break;
        size_t right = left + 1u;
        size_t child = right < *count && body_component_less(recs, rep, epoch, effect, heap[right], heap[left]) ? right : left;
        if (!body_component_less(recs, rep, epoch, effect, heap[child], heap[at])) break;
        size_t tmp = heap[at];
        heap[at] = heap[child];
        heap[child] = tmp;
        at = child;
    }
    return out;
}

typedef struct {
    size_t comp;
    size_t node;
    const char *key;
} BodyMember;

static int body_member_compare(const void *a_ptr, const void *b_ptr) {
    const BodyMember *a = a_ptr;
    const BodyMember *b = b_ptr;
    if (a->comp != b->comp) return a->comp < b->comp ? -1 : 1;
    int cmp = strcmp(a->key, b->key);
    if (cmp != 0) return cmp;
    if (a->node == b->node) return 0;
    return a->node < b->node ? -1 : 1;
}

static bool body_rec_env(ExpandContext *ctx, const BodyRec *rec) {
    return rec->kind == BODY_REC_GROUPS ? ctx->frame == IDM_FRAME_TOP : body_bind_is_env(ctx);
}

static bool body_recs_schedule(ExpandContext *ctx, BodyRec *recs, size_t count, size_t **out_order, IdmError *err) {
    *out_order = NULL;
    if (count == 0) return true;
    size_t local_count = 0;
    size_t env_count = 0;
    for (size_t i = 0; i < count; i++) {
        recs[i].callable = recs[i].kind == BODY_REC_GROUPS ||
                           (recs[i].kind == BODY_REC_BIND && recs[i].core &&
                            (recs[i].core->kind == IDM_CORE_FN || recs[i].core->kind == IDM_CORE_FN_MULTI));
        recs[i].pure = recs[i].callable;
        if (!recs[i].callable) {
            IdmArgMask invoked;
            uint8_t purity;
            if (!expand_typecheck_core_purity(ctx, recs[i].core, &purity, &invoked, err)) return false;
            idm_arg_mask_destroy(&invoked);
            recs[i].pure = purity >= IDM_PURITY_PURE;
        }
        bool rec_env = body_rec_env(ctx, &recs[i]);
        if (recs[i].kind == BODY_REC_BIND) {
            size_t *limit = rec_env ? &env_count : &local_count;
            if ((size_t)recs[i].bind_slot + 1u > *limit) *limit = (size_t)recs[i].bind_slot + 1u;
        } else if (recs[i].kind == BODY_REC_BIND_PATTERN) {
            for (size_t n = 0; n < recs[i].pattern_name_count; n++) {
                size_t *limit = rec_env ? &env_count : &local_count;
                if ((size_t)recs[i].pattern_slots[n] + 1u > *limit) *limit = (size_t)recs[i].pattern_slots[n] + 1u;
            }
        } else if (recs[i].kind == BODY_REC_GROUPS) {
            for (size_t n = 0; n < recs[i].group_count; n++) {
                size_t *limit = rec_env ? &env_count : &local_count;
                if ((size_t)recs[i].groups[n].slot + 1u > *limit) *limit = (size_t)recs[i].groups[n].slot + 1u;
            }
        }
    }
    size_t *locals = local_count ? malloc(local_count * sizeof(*locals)) : NULL;
    size_t *envs = env_count ? malloc(env_count * sizeof(*envs)) : NULL;
    if ((local_count && !locals) || (env_count && !envs)) {
        free(locals);
        free(envs);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t i = 0; i < local_count; i++) locals[i] = SIZE_MAX;
    for (size_t i = 0; i < env_count; i++) envs[i] = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        size_t *owners = body_rec_env(ctx, &recs[i]) ? envs : locals;
        if (recs[i].kind == BODY_REC_BIND) owners[recs[i].bind_slot] = i;
        else if (recs[i].kind == BODY_REC_BIND_PATTERN) {
            for (size_t n = 0; n < recs[i].pattern_name_count; n++) owners[recs[i].pattern_slots[n]] = i;
        } else if (recs[i].kind == BODY_REC_GROUPS) {
            for (size_t n = 0; n < recs[i].group_count; n++) owners[recs[i].groups[n].slot] = i;
        }
    }
    BodyDepScan scan = { recs, count, locals, local_count, envs, env_count, 0u };
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++) ok = body_core_deps(&scan, i, recs[i].core);
    size_t previous_effect = SIZE_MAX;
    size_t order_epoch = 0;
    for (size_t i = 0; ok && i < count; i++) {
        recs[i].order_epoch = order_epoch;
        recs[i].effect = !recs[i].pure;
        if (recs[i].effect) {
            if (previous_effect != SIZE_MAX) ok = body_rec_add_dep(&recs[i], previous_effect);
            previous_effect = i;
            order_epoch++;
        }
        if (recs[i].kind == BODY_REC_EXPR && i + 1u == count) {
            for (size_t j = 0; ok && j < i; j++) ok = body_rec_add_dep(&recs[i], j);
        }
    }
    free(locals);
    free(envs);
    if (!ok) return idm_error_oom(err, idm_span_unknown(NULL));

    BodyTarjan t;
    memset(&t, 0, sizeof(t));
    t.recs = recs;
    t.count = count;
    t.index = malloc(count * sizeof(*t.index));
    t.low = malloc(count * sizeof(*t.low));
    t.stack = malloc(count * sizeof(*t.stack));
    t.comp = malloc(count * sizeof(*t.comp));
    t.on_stack = calloc(count, sizeof(*t.on_stack));
    if (!t.index || !t.low || !t.stack || !t.comp || !t.on_stack) {
        free(t.index); free(t.low); free(t.stack); free(t.comp); free(t.on_stack);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t i = 0; i < count; i++) t.index[i] = SIZE_MAX;
    for (size_t i = 0; i < count; i++) if (t.index[i] == SIZE_MAX) body_tarjan_visit(&t, i);

    size_t *comp_size = calloc(t.comp_count, sizeof(*comp_size));
    size_t *rep = malloc(t.comp_count * sizeof(*rep));
    bool *self = calloc(t.comp_count, sizeof(*self));
    size_t *indegree = calloc(t.comp_count, sizeof(*indegree));
    size_t *heads = malloc(t.comp_count * sizeof(*heads));
    size_t edge_count = 0;
    for (size_t i = 0; i < count; i++) edge_count += recs[i].dep_count;
    size_t *edge_from = edge_count ? malloc(edge_count * sizeof(*edge_from)) : NULL;
    size_t *edge_next = edge_count ? malloc(edge_count * sizeof(*edge_next)) : NULL;
    size_t *heap = malloc(t.comp_count * sizeof(*heap));
    size_t *order = malloc(count * sizeof(*order));
    BodyMember *members = malloc(count * sizeof(*members));
    size_t *member_start = malloc(t.comp_count * sizeof(*member_start));
    size_t *comp_epoch = malloc(t.comp_count * sizeof(*comp_epoch));
    bool *comp_effect = calloc(t.comp_count, sizeof(*comp_effect));
    size_t *topo = malloc(t.comp_count * sizeof(*topo));
    size_t *pending = malloc(t.comp_count * sizeof(*pending));
    if (!comp_size || !rep || !self || !indegree || !heads || (edge_count && (!edge_from || !edge_next)) || !heap || !order || !members || !member_start || !comp_epoch || !comp_effect || !topo || !pending) {
        free(comp_size); free(rep); free(self); free(indegree); free(heads); free(edge_from); free(edge_next); free(heap); free(order); free(members); free(member_start); free(comp_epoch); free(comp_effect); free(topo); free(pending);
        free(t.index); free(t.low); free(t.stack); free(t.comp); free(t.on_stack);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t c = 0; c < t.comp_count; c++) { rep[c] = SIZE_MAX; heads[c] = SIZE_MAX; member_start[c] = SIZE_MAX; comp_epoch[c] = SIZE_MAX; }
    for (size_t i = 0; i < count; i++) {
        size_t c = t.comp[i];
        members[i].comp = c;
        members[i].node = i;
        members[i].key = body_rec_key(&recs[i]);
        comp_size[c]++;
        if (recs[i].order_epoch < comp_epoch[c]) comp_epoch[c] = recs[i].order_epoch;
        if (recs[i].effect) comp_effect[c] = true;
        if (rep[c] == SIZE_MAX || strcmp(body_rec_key(&recs[i]), body_rec_key(&recs[rep[c]])) < 0) rep[c] = i;
        for (size_t d = 0; d < recs[i].dep_count; d++) if (recs[i].deps[d] == i) self[c] = true;
    }
    for (size_t c = 0; c < t.comp_count; c++) {
        if (comp_size[c] == 1u && !self[c]) continue;
        for (size_t i = 0; i < count; i++) {
            if (t.comp[i] != c) continue;
            if (!recs[i].callable) {
                const char *name = body_rec_key(&recs[i]);
                free(comp_size); free(rep); free(self); free(indegree); free(heads); free(edge_from); free(edge_next); free(heap); free(order); free(members); free(member_start); free(comp_epoch); free(comp_effect); free(topo); free(pending);
                free(t.index); free(t.low); free(t.stack); free(t.comp); free(t.on_stack);
                return idm_error_set(err, recs[i].form->span, "initialization cycle involving '%s'", name);
            }
            if (!body_rec_env(ctx, &recs[i])) recs[i].prealloc = true;
        }
    }
    qsort(members, count, sizeof(*members), body_member_compare);
    for (size_t i = 0; i < count; i++) if (member_start[members[i].comp] == SIZE_MAX) member_start[members[i].comp] = i;
    size_t edge_at = 0;
    for (size_t i = 0; i < count; i++) {
        size_t from = t.comp[i];
        for (size_t d = 0; d < recs[i].dep_count; d++) {
            size_t to = t.comp[recs[i].deps[d]];
            if (from == to) continue;
            indegree[from]++;
            edge_from[edge_at] = from;
            edge_next[edge_at] = heads[to];
            heads[to] = edge_at++;
        }
    }
    memcpy(pending, indegree, t.comp_count * sizeof(*pending));
    size_t topo_read = 0;
    size_t topo_count = 0;
    for (size_t c = 0; c < t.comp_count; c++) if (pending[c] == 0) topo[topo_count++] = c;
    while (topo_read < topo_count) {
        size_t c = topo[topo_read++];
        for (size_t e = heads[c]; e != SIZE_MAX; e = edge_next[e]) {
            size_t dependent = edge_from[e];
            if (--pending[dependent] == 0) topo[topo_count++] = dependent;
        }
    }
    for (size_t pos = topo_count; pos > 0; pos--) {
        size_t c = topo[pos - 1u];
        size_t start = member_start[c];
        for (size_t m = start; m < count && members[m].comp == c; m++) {
            size_t node = members[m].node;
            for (size_t d = 0; d < recs[node].dep_count; d++) {
                size_t dependency = t.comp[recs[node].deps[d]];
                if (dependency != c && comp_epoch[c] < comp_epoch[dependency]) comp_epoch[dependency] = comp_epoch[c];
            }
        }
    }
    size_t heap_count = 0;
    for (size_t c = 0; c < t.comp_count; c++) if (indegree[c] == 0) body_heap_push(heap, &heap_count, c, recs, rep, comp_epoch, comp_effect);
    size_t order_count = 0;
    while (heap_count != 0) {
        size_t c = body_heap_pop(heap, &heap_count, recs, rep, comp_epoch, comp_effect);
        size_t start = member_start[c];
        for (size_t i = start; i < count && members[i].comp == c; i++) order[order_count++] = members[i].node;
        for (size_t e = heads[c]; e != SIZE_MAX; e = edge_next[e]) {
            size_t dependent = edge_from[e];
            if (--indegree[dependent] == 0) body_heap_push(heap, &heap_count, dependent, recs, rep, comp_epoch, comp_effect);
        }
    }
    if (order_count != count) {
        free(order);
        order = NULL;
        idm_error_set(err, idm_span_unknown(NULL), "body dependency graph could not be scheduled");
    }
    free(comp_size); free(rep); free(self); free(indegree); free(heads); free(edge_from); free(edge_next); free(heap); free(members); free(member_start); free(comp_epoch); free(comp_effect); free(topo); free(pending);
    free(t.index); free(t.low); free(t.stack); free(t.comp); free(t.on_stack);
    *out_order = order;
    return order != NULL;
}

IdmCore *expand_body_items(ExpandContext *ctx, IdmSyntax *const *items, size_t index, size_t count, bool def_scope, IdmError *err) {
    if (index >= count) return idm_core_literal(idm_nil(), idm_span_unknown(NULL));
    size_t work_count = count - index;
    size_t work_cap = work_count;
    IdmSyntax **work = malloc(work_cap * sizeof(*work));
    if (!work) return (IdmCore *)(uintptr_t)idm_error_oom(err, items[index]->span);
    for (size_t i = 0; i < work_count; i++) work[i] = items[index + i];
    IdmSyntax **owned = NULL;
    size_t owned_count = 0;
    size_t owned_cap = 0;
    if (!normalize_degenerate_adjacent_exprs(work, work_count, &owned, &owned_count, &owned_cap, err)) {
        for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
        free(owned);
        free(work);
        return NULL;
    }
    if (def_scope) {
        IdmScopeId body_scope = idm_scope_fresh(&ctx->scope_store);
        if (!body_scope_add_range(ctx, work, 0, work_count, body_scope, items[index]->span, err)) {
            for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
            free(owned);
            free(work);
            return NULL;
        }
    }
    IdmScopeSet body_ctx_scopes;
    if (!syntax_scopes_copy(&body_ctx_scopes, work[0])) {
        for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
        free(owned);
        free(work);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, items[index]->span);
    }
    IdmCore **prepared = NULL;
    size_t prepared_count = 0;
    size_t prepared_cap = 0;
    if (!body_prepare_declarations(ctx, work, &work_count, true, &prepared, &prepared_count, &prepared_cap, err)) {
        for (size_t p = 0; p < prepared_count; p++) idm_core_free(prepared[p]);
        free(prepared);
        idm_scope_set_destroy(&body_ctx_scopes);
        for (size_t o = 0; o < owned_count; o++) idm_syn_free(owned[o]);
        free(owned);
        free(work);
        return NULL;
    }
    const IdmScopeSet *saved_op_fallback = ctx->op_fallback;
    uint32_t saved_body_depth = ctx->body_depth;
    ctx->body_depth = saved_body_depth + 1u;
    ctx->op_fallback = &body_ctx_scopes;
    BodyRec *recs = NULL;
    size_t rec_count = 0;
    size_t rec_cap = 0;
    BodyDefCtx def_ctx;
    def_ctx.prev = ctx->def_ctx;
    ctx->def_ctx = &def_ctx;
    bool failed = false;
    IdmScopeId definition_scope = 0;
    size_t i = 0;
    BodySignature *signatures = NULL;
    size_t signature_count = 0;
    size_t signature_cap = 0;
    char *pending_doc = NULL;
    unsigned prev_stmt_max_line = 0;

    bool persistent_repl_env = ctx->repl_env_binds && ctx->frame == IDM_FRAME_TOP;
    bool package_top = ctx->in_package && ctx->frame == IDM_FRAME_TOP;
    bool has_defn = false;
    for (size_t d = 0; d < work_count && !has_defn; d++) {
        const IdmSyntax *ignored_name = NULL;
        size_t ignored_start = 0;
        has_defn = defn_form_parts(work[d], &ignored_name, &ignored_start, NULL);
    }
    if (has_defn && !persistent_repl_env && !package_top) {
        definition_scope = idm_scope_fresh(&ctx->scope_store);
        if (!body_definition_scope_add(ctx, work, 0, work_count, definition_scope, items[index]->span, err)) failed = true;
    }
    size_t body_binding_start = ctx->bindings.count;
    if (!failed && !body_prebind_defns(ctx, work, work_count, err)) failed = true;
    size_t body_binding_end = ctx->bindings.count;
    if (!failed && !body_prepare_declarations(ctx, work, &work_count, false, &prepared, &prepared_count, &prepared_cap, err)) failed = true;
    while (i < work_count && !failed) {
        const IdmSyntax *form = work[i];
        if (idm_syn_is_form_id(form, IDM_FORM_DOC) && form->as.seq.count == 2u && form->as.seq.items[1]->kind == IDM_SYN_STRING) {
            bool shares_line = prev_stmt_max_line != 0 && prev_stmt_max_line >= form->span.line;
            if (shares_line) {
                expand_error(err, form->span, "a doc comment cannot share a line with code");
                failed = true;
                break;
            }
            const char *line = form->as.seq.items[1]->as.text ? form->as.seq.items[1]->as.text : "";
            if (!pending_doc) {
                pending_doc = idm_strdup(line);
            } else {
                size_t have = strlen(pending_doc);
                char *grown = realloc(pending_doc, have + strlen(line) + 2u);
                if (grown) {
                    grown[have] = '\n';
                    strcpy(grown + have + 1u, line);
                    pending_doc = grown;
                } else {
                    free(pending_doc);
                    pending_doc = NULL;
                }
            }
            if (!pending_doc) { idm_error_oom(err, form->span); failed = true; break; }
            prev_stmt_max_line = syn_max_line(form);
            i++;
            continue;
        }
        prev_stmt_max_line = syn_max_line(form);
        if (idm_syn_is_form_id(form, IDM_FORM_EXPR) && form->as.seq.count == 3 &&
            syn_is_word(form->as.seq.items[1], "package") && form->as.seq.items[2]->kind == IDM_SYN_WORD) {
            const char *declared = form->as.seq.items[2]->as.text;
            if (ctx->package_name && strcmp(ctx->package_name, declared) != 0) {
                expand_error(err, form->span, "conflicting package declarations '%s' and '%s' in one package", ctx->package_name, declared);
                failed = true;
                break;
            }
            const char *interned = idm_symbol_text(idm_intern(&ctx->rt->intern, IDM_SYMBOL_WORD, declared));
            if (!interned) { idm_error_oom(err, form->span); failed = true; break; }
            ctx->package_name = interned;
            i++;
            continue;
        }
        if (signature_form_parts(form, NULL, NULL)) {
            if (!body_signature_add(ctx, &signatures, &signature_count, &signature_cap, form, err)) {
                failed = true;
                break;
            }
            i++;
            continue;
        }
        const char *definition_head = NULL;
        if (definition_like_form(form, &definition_head)) {
            if (strcmp(definition_head, "def") == 0) {
                expand_error(err, form->as.seq.items[1]->span, "source 'def' is not a function declaration; use 'defn'");
                failed = true;
                break;
            }
            if (strcmp(definition_head, "defmacro") == 0) {
                if (pending_doc) {
                    expand_error(err, form->span, "a doc comment cannot attach to 'defmacro' yet; document a defn instead");
                    failed = true;
                    break;
                }
                size_t mbase = (form->as.seq.count > 1 && syn_is_word(form->as.seq.items[1], "export")) ? 2u : 1u;
                if (form->as.seq.count < mbase + 3u || form->as.seq.items[mbase + 1u]->kind != IDM_SYN_WORD) {
                    expand_error(err, form->span, "defmacro expects a name, one syntax parameter, and a body");
                    failed = true;
                    break;
                }
                const char *macro_name = form->as.seq.items[mbase + 1u]->as.text;
                int saved_phase = ctx->phase;
                ctx->phase = 1;
                IdmCore *fn = expand_function_literal(ctx, macro_name, form->as.seq.items[mbase], form->as.seq.items, mbase + 2u, form->as.seq.count, err);
                ctx->phase = saved_phase;
                if (!fn) { failed = true; break; }
                if (core_callable_capture_count(fn) != 0) {
                    idm_core_free(fn);
                    expand_error(err, form->span, "defmacro '%s' cannot capture phase-1 locals", macro_name);
                    idm_error_note(err, "a transformer is capture-free; computed transformation belongs to a form declaration or a top-level defn helper");
                    failed = true;
                    break;
                }
                if (!typecheck_transformer(ctx, form->as.seq.items[mbase + 1u]->as.text, fn, err)) { idm_core_free(fn); failed = true; break; }
                bool ok = register_macro(ctx, form->as.seq.items[mbase + 1u], fn, form->span, mbase == 2u, err);
                idm_core_free(fn);
                if (!ok) { failed = true; break; }
                i++;
                continue;
            }
            size_t j = i;
            DefnGroup *groups = NULL;
            size_t group_count = 0;
            size_t group_cap = 0;
            bool persistent_repl_env = ctx->repl_env_binds && ctx->frame == IDM_FRAME_TOP;
            bool package_top = ctx->in_package && ctx->frame == IDM_FRAME_TOP;
            if (!persistent_repl_env && !package_top) {
                if (definition_scope == 0) definition_scope = idm_scope_fresh(&ctx->scope_store);
                if (!body_definition_scope_add(ctx, work, i, work_count, definition_scope, form->span, err)) {
                    failed = true;
                    break;
                }
                form = work[i];
            }
            while (j < work_count) {
                const IdmSyntax *def_name = NULL;
                size_t ignored_start = 0;
                bool form_export = false;
                if (!defn_form_parts(work[j], &def_name, &ignored_start, &form_export)) break;
                DefnGroup *group = find_or_add_group(&groups, &group_count, &group_cap, def_name);
                if (group && j == i && pending_doc) {
                    group->doc = pending_doc;
                    pending_doc = NULL;
                }
                if (group && group->count == 0 && !defn_group_take_signature(group, signatures, signature_count, err)) {
                    defn_groups_destroy(groups, group_count);
                    failed = true;
                    break;
                }
                if (!group || !group_add_index(group, j)) {
                    defn_groups_destroy(groups, group_count);
                    idm_error_oom(err, work[j]->span);
                    failed = true;
                    break;
                }
                if (form_export) group->exported = true;
                j++;
            }
            if (group_count == 0) {
                expand_error(err, form->span, "defn expects a name, parameters, and a body");
                failed = true;
                break;
            }
            if (failed) break;
            bool top_level = ctx->frame == IDM_FRAME_TOP;
            for (size_t k = 0; k < group_count && !failed; k++) {
                if (!defn_group_arity(ctx, &groups[k], work, &groups[k].arity, err)) {
                    failed = true;
                    break;
                }
                bool ok = true;
                const IdmBinding *existing = NULL;
                ok = body_def_binding_between(ctx, body_binding_start, body_binding_end, groups[k].name, groups[k].name_syntax, &existing, err);
                if (ok && existing) {
                    groups[k].slot = existing->payload;
                    ok = idm_binding_table_set_arity(&ctx->bindings, existing->id, groups[k].arity);
                    if (ok && groups[k].has_contract) ok = idm_binding_table_set_contract(&ctx->bindings, existing->id, &groups[k].contract);
                } else if (ok && top_level && ctx->in_package) {
                    ok = body_env_def_binder_with_arity(ctx, groups[k].name, groups[k].name_syntax, groups[k].arity, groups[k].has_contract ? &groups[k].contract : NULL, true, &groups[k].slot, NULL, err);
                } else if (ok) {
                    ok = top_level ? env_push_def_binder_with_arity(ctx, groups[k].name, groups[k].name_syntax, groups[k].arity, groups[k].has_contract ? &groups[k].contract : NULL, &groups[k].slot)
                                   : local_push_def_binder_with_arity(ctx, groups[k].name, groups[k].name_syntax, groups[k].arity, groups[k].has_contract ? &groups[k].contract : NULL, &groups[k].slot);
                }
                if (!ok) { idm_error_oom(err, form->span); failed = true; break; }
                if (top_level && ctx->in_package && !record_package_slot(ctx, groups[k].name, groups[k].slot, &groups[k].scopes, groups[k].arity, groups[k].has_contract ? &groups[k].contract : NULL, groups[k].exported)) {
                    idm_error_oom(err, form->span);
                    failed = true;
                    break;
                }
            }
            if (failed) { defn_groups_destroy(groups, group_count); break; }
            if (top_level && ctx->phase == 0) {
                size_t run_id = phase_candidate_next_run(ctx);
                for (size_t k = 0; k < group_count && !failed; k++) {
                    if (!phase_candidate_matches(ctx, groups[k].name, work[i]) &&
                        !phase_candidate_stage(ctx, groups[k].name, groups[k].name_syntax, work + i, j - i, run_id, err)) failed = true;
                }
                if (failed) { defn_groups_destroy(groups, group_count); break; }
            }
            if (rec_count == rec_cap) {
                if (!idm_grow((void **)&recs, &rec_cap, sizeof(*recs), 8u, rec_count + 1u)) { defn_groups_destroy(groups, group_count); idm_error_oom(err, form->span); failed = true; break; }
            }
            memset(&recs[rec_count], 0, sizeof(recs[rec_count]));
            recs[rec_count].kind = BODY_REC_GROUPS;
            recs[rec_count].form = form;
            recs[rec_count].groups = groups;
            recs[rec_count].group_count = group_count;
            rec_count++;
            i = j;
            continue;
        }
        if (idm_syn_is_form_id(form, IDM_FORM_EXPR) && form->as.seq.count >= 2 && form->as.seq.items[1]->kind == IDM_SYN_WORD &&
            !bind_form_parts(form, &(const IdmSyntax *){NULL}, &(size_t){0})) {
            const BodyDeclDesc *decl_desc = body_decl_form(form);
            bool is_decl_word = decl_desc && decl_desc->handler;
            if (is_decl_word && pending_doc) {
                expand_error(err, form->span, "a doc comment cannot attach to this declaration yet; document a defn instead");
                failed = true;
                break;
            }
            if (!is_decl_word) {
                IdmSyntax *expanded = NULL;
                if (!expand_body_macro_form_to_syntax(ctx, form, &expanded, err)) {
                    failed = true;
                    break;
                }
                if (expanded) {
                    if (!body_install_expanded_syntax(&work, &work_count, &work_cap, &owned, &owned_count, &owned_cap, 0, i, expanded, err)) { failed = true; break; }
                    continue;
                }
                if (err && err->present) { failed = true; break; }
            } else {
                IdmCore *decl_core = decl_desc->handler(ctx, work[i], err);
                if (!decl_core) {
                    failed = true;
                    break;
                }
                if (prepared_count == prepared_cap &&
                    !idm_grow((void **)&prepared, &prepared_cap, sizeof(*prepared), 4u, prepared_count + 1u)) {
                    idm_core_free(decl_core);
                    idm_error_oom(err, work[i]->span);
                    failed = true;
                    break;
                }
                prepared[prepared_count++] = decl_core;
                i++;
                continue;
            }
        }
        {
            const IdmSyntax *pin_pattern = NULL;
            size_t pin_rhs_start = 0;
            if (pattern_bind_form_parts(form, &pin_pattern, &pin_rhs_start) && pattern_syntax_has_pinned_dict_key(pin_pattern)) {
                IdmSyntax *expanded = pinned_key_bind_rewrite(ctx, form, pin_pattern, pin_rhs_start, err);
                if (!expanded) { failed = true; break; }
                if (!body_install_expanded_syntax(&work, &work_count, &work_cap, &owned, &owned_count, &owned_cap, 0, i, expanded, err)) { failed = true; break; }
                continue;
            }
        }
        if (pending_doc) {
            expand_error(err, form->span, "a doc comment must precede a definition (a spec may sit between)");
            failed = true;
            break;
        }
        if (rec_count == rec_cap) {
            if (!idm_grow((void **)&recs, &rec_cap, sizeof(*recs), 8u, rec_count + 1u)) { idm_error_oom(err, form->span); failed = true; break; }
        }
        memset(&recs[rec_count], 0, sizeof(recs[rec_count]));
        const IdmSyntax *bind_name = NULL;
        const IdmSyntax *bind_pattern = NULL;
        size_t rhs_start = 0;
        if (bind_form_parts(form, &bind_name, &rhs_start)) {
            IdmArity bind_arity = idm_arity_unknown();
            if (!rhs_callable_value_binding_info(ctx, form->as.seq.items, rhs_start, form->as.seq.count, &bind_arity, err) &&
                err && err->present) {
                failed = true;
                break;
            }
            IdmScopeSet bind_scopes;
            idm_scope_set_init(&bind_scopes);
            if (!binder_scopes_pruned(ctx, bind_name, &bind_scopes)) {
                idm_error_oom(err, bind_name->span);
                failed = true;
                break;
            }
            IdmCallableContract bind_contract;
            bool bind_has_contract = false;
            bool sig_ok = body_signature_take_contract(bind_name, &bind_scopes, signatures, signature_count, &bind_contract, &bind_has_contract, err);
            idm_scope_set_destroy(&bind_scopes);
            if (!sig_ok) {
                failed = true;
                break;
            }
            if (bind_has_contract && !callable_contract_apply_value_arity(bind_name, &bind_contract, &bind_arity, err)) {
                idm_callable_contract_destroy(&bind_contract);
                failed = true;
                break;
            }
            recs[rec_count].kind = BODY_REC_BIND;
            recs[rec_count].bind_name = bind_name;
            recs[rec_count].rhs_start = rhs_start;
            recs[rec_count].bind_has_contract = bind_has_contract;
            if (bind_has_contract) recs[rec_count].bind_contract = bind_contract;
            if (ctx->in_package && ctx->frame == IDM_FRAME_TOP) {
                if (!body_env_def_binder_with_arity(ctx, bind_name->as.text, bind_name, bind_arity, bind_has_contract ? &bind_contract : NULL, true, &recs[rec_count].bind_slot, NULL, err)) {
                    if (bind_has_contract) idm_callable_contract_destroy(&bind_contract);
                    recs[rec_count].bind_has_contract = false;
                    failed = true;
                    break;
                }
            } else if (ctx->repl_env_binds && ctx->frame == IDM_FRAME_TOP) {
                IdmScopeSet reuse_scopes;
                if (!binder_scopes_pruned(ctx, bind_name, &reuse_scopes)) {
                    if (bind_has_contract) idm_callable_contract_destroy(&bind_contract);
                    recs[rec_count].bind_has_contract = false;
                    idm_error_oom(err, bind_name->span);
                    failed = true;
                    break;
                }
                const IdmBinding *existing = NULL;
                recs[rec_count].bind_slot = body_existing_env_binding(ctx, bind_name->as.text, &reuse_scopes, &existing) ? existing->payload : ctx->env_slot_seq++;
                idm_scope_set_destroy(&reuse_scopes);
            } else {
                recs[rec_count].bind_slot = ctx->next_slot++;
            }
            recs[rec_count].bind_arity = bind_arity;
            if (!push_bind_binder(ctx, bind_name, recs[rec_count].bind_slot, bind_arity,
                                  bind_has_contract ? &bind_contract : NULL,
                                  &recs[rec_count].bind_binding_id, err)) {
                failed = true;
                break;
            }
        } else if (pattern_bind_form_parts(form, &bind_pattern, &rhs_start)) {
            if (!scan_pattern_bind(ctx, &recs[rec_count], bind_pattern, rhs_start, err)) {
                failed = true;
                break;
            }
            for (size_t n = 0; n < recs[rec_count].pattern_name_count; n++) {
                if (!push_bind_binder(ctx, recs[rec_count].pattern_names[n], recs[rec_count].pattern_slots[n],
                                      idm_arity_unknown(), NULL,
                                      &recs[rec_count].pattern_binding_ids[n], err)) {
                    failed = true;
                    break;
                }
            }
            if (failed) break;
        } else {
            recs[rec_count].kind = BODY_REC_EXPR;
        }
        recs[rec_count].form = form;
        rec_count++;
        i++;
    }

    if (!failed && pending_doc) {
        expand_error(err, idm_span_unknown(NULL), "a doc comment at the end of a body documents nothing");
        failed = true;
    }
    free(pending_doc);
    pending_doc = NULL;
    if (!failed && !body_signatures_all_used(signatures, signature_count, err)) failed = true;

    for (size_t r = 0; r < rec_count && !failed; r++) {
        BodyRec *rec = &recs[r];
        if (rec->kind == BODY_REC_BIND) {
            bool saved_vc = ctx->value_context;
            ctx->value_context = true;
            rec->core = expand_value_rhs(ctx, rec, err);
            ctx->value_context = saved_vc;
            if (!rec->core) { failed = true; break; }
            rec->bind_arity = core_callable_arity(rec->core);
            if (rec->bind_arity.kind == IDM_ARITY_UNKNOWN) {
                IdmArity syntax_arity = idm_arity_unknown();
                if (rhs_callable_value_binding_info(ctx, rec->form->as.seq.items, rec->rhs_start, rec->form->as.seq.count, &syntax_arity, err)) {
                    rec->bind_arity = syntax_arity;
                } else if (err && err->present) {
                    failed = true;
                    break;
                }
            }
            if (rec->bind_has_contract && !callable_contract_validate_core_arities(rec->bind_name, &rec->bind_contract, rec->core, err)) {
                failed = true;
                break;
            }
            if (rec->bind_has_contract && !callable_contract_apply_value_arity(rec->bind_name, &rec->bind_contract, &rec->bind_arity, err)) {
                failed = true;
                break;
            }
            bool bind_declared = rec->bind_has_contract;
            if (!infer_bind_value_contract(ctx, rec, err)) {
                failed = true;
                break;
            }
            if (ctx->frame == IDM_FRAME_TOP && !expand_typecheck_value(ctx, rec->bind_name->as.text, &rec->core, &rec->bind_contract, &rec->bind_has_contract, bind_declared, err)) {
                failed = true;
                break;
            }
            if (!idm_binding_table_set_arity(&ctx->bindings, rec->bind_binding_id, rec->bind_arity) ||
                (rec->bind_has_contract && !idm_binding_table_set_contract(&ctx->bindings, rec->bind_binding_id, &rec->bind_contract))) {
                idm_error_oom(err, rec->bind_name->span);
                failed = true;
                break;
            }
        } else if (rec->kind == BODY_REC_BIND_PATTERN) {
            bool saved_vc = ctx->value_context;
            ctx->value_context = true;
            IdmCore *rhs = expand_parts(ctx, rec->form->as.seq.items, rec->rhs_start, rec->form->as.seq.count, err);
            ctx->value_context = saved_vc;
            if (!rhs) { failed = true; break; }
            if (!infer_pattern_bind_infos(ctx, rec, err)) {
                failed = true;
                break;
            }
            rec->core = body_bind_is_env(ctx) ? pattern_bind_call(ctx, rec, rhs, err) : pattern_bind_match(ctx, rec, rhs, err);
            if (!rec->core) { failed = true; break; }
            if (ctx->frame == IDM_FRAME_TOP && !expand_typecheck_statement(ctx, &rec->core, err)) {
                failed = true;
                break;
            }
            for (size_t n = 0; n < rec->pattern_name_count; n++) {
                CallableBindingInfo info = rec->pattern_bindings ? rec->pattern_bindings[n] : (CallableBindingInfo){ idm_arity_unknown() };
                if (!idm_binding_table_set_arity(&ctx->bindings, rec->pattern_binding_ids[n], info.arity)) {
                    idm_error_oom(err, rec->pattern_names[n]->span);
                    failed = true;
                    break;
                }
            }
            if (failed) break;
        } else if (rec->kind == BODY_REC_GROUPS) {
            bool top_level = ctx->frame == IDM_FRAME_TOP;
            IdmCore *letrec = idm_core_letrec(rec->form->span);
            if (!letrec) { idm_error_oom(err, rec->form->span); failed = true; break; }
            if (top_level) idm_core_letrec_set_env(letrec);
            IdmCore **values = rec->group_count == 0 ? NULL : calloc(rec->group_count, sizeof(*values));
            if (rec->group_count != 0 && !values) {
                idm_error_oom(err, rec->form->span);
                idm_core_free(letrec);
                failed = true;
                break;
            }
            for (size_t k = 0; k < rec->group_count; k++) {
                values[k] = expand_defn_group(ctx, &rec->groups[k], work, err);
                if (values[k] && rec->groups[k].doc && !idm_core_fn_set_doc(values[k], rec->groups[k].doc)) {
                    idm_error_oom(err, idm_span_unknown(NULL));
                    idm_core_free(values[k]);
                    values[k] = NULL;
                }
                if (!values[k]) {
                    idm_core_free(letrec);
                    letrec = NULL;
                    failed = true;
                    break;
                }
            }
            if (!failed && !expand_typecheck_defn_groups(ctx, rec->groups, values, rec->group_count, err)) {
                idm_core_free(letrec);
                letrec = NULL;
                failed = true;
            }
            for (size_t k = 0; k < rec->group_count && !failed; k++) {
                if (!idm_core_letrec_add(letrec, rec->groups[k].name, rec->groups[k].slot, values[k])) {
                    idm_core_free(letrec);
                    letrec = NULL;
                    if (err && !err->present) idm_error_oom(err, rec->form->span);
                    failed = true;
                    break;
                }
                values[k] = NULL;
            }
            for (size_t k = 0; k < rec->group_count; k++) idm_core_free(values[k]);
            free(values);
            rec->core = letrec;
        } else {
            rec->core = idm_syn_is_form_id(rec->form, IDM_FORM_EXPR)
                ? expand_parts(ctx, rec->form->as.seq.items, 1, rec->form->as.seq.count, err)
                : expand_syntax(ctx, rec->form, err);
            if (!rec->core) { failed = true; break; }
            if (ctx->frame == IDM_FRAME_TOP && !expand_typecheck_statement(ctx, &rec->core, err)) {
                failed = true;
                break;
            }
        }
    }

    size_t *schedule = NULL;
    if (!failed && !body_recs_schedule(ctx, recs, rec_count, &schedule, err)) failed = true;
    if (!failed && ctx->frame != IDM_FRAME_TOP) {
        for (size_t r = 0; r < rec_count; r++) {
            if (recs[r].prealloc && recs[r].kind == BODY_REC_GROUPS) idm_core_letrec_set_fill_only(recs[r].core);
        }
    }

    IdmCore *core = NULL;
    size_t result_rec = SIZE_MAX;
    if (!failed) {
        if (rec_count != 0 && recs[rec_count - 1u].kind == BODY_REC_EXPR) {
            result_rec = rec_count - 1u;
        } else {
            core = idm_core_literal(idm_nil(), idm_span_unknown(NULL));
            if (!core) { idm_error_oom(err, idm_span_unknown(NULL)); failed = true; }
        }
    }

    if (!failed) {
        for (size_t pos = rec_count; pos > 0 && !failed; pos--) {
            size_t r = schedule[pos - 1u];
            BodyRec *rec = &recs[r];
            IdmCore *inner = rec->core;
            rec->core = NULL;
            if (r == result_rec) {
                core = inner;
                continue;
            }
            if (rec->kind == BODY_REC_BIND) {
                if (body_bind_is_env(ctx)) {
                    IdmCore *letrec = idm_core_letrec(rec->form->span);
                    if (!letrec || !idm_core_letrec_add(letrec, rec->bind_name->as.text, rec->bind_slot, inner) || !idm_core_letrec_set_body(letrec, core)) {
                        idm_core_free(letrec);
                        idm_core_free(inner);
                        idm_core_free(core);
                        core = NULL;
                        idm_error_oom(err, rec->form->span);
                        failed = true;
                    } else {
                        idm_core_letrec_set_env(letrec);
                        core = letrec;
                    }
                } else if (rec->prealloc) {
                    IdmCore *letrec = idm_core_letrec(rec->form->span);
                    if (!letrec || !idm_core_letrec_add(letrec, rec->bind_name->as.text, rec->bind_slot, inner) || !idm_core_letrec_set_body(letrec, core)) {
                        idm_core_free(letrec);
                        idm_core_free(inner);
                        idm_core_free(core);
                        core = NULL;
                        idm_error_oom(err, rec->form->span);
                        failed = true;
                    } else {
                        idm_core_letrec_set_fill_only(letrec);
                        core = letrec;
                    }
                } else {
                    core = idm_core_bind_local(rec->bind_name->as.text, rec->bind_slot, inner, core, rec->form->span);
                    if (!core) { idm_core_free(inner); idm_error_oom(err, rec->form->span); failed = true; }
                }
            } else if (rec->kind == BODY_REC_BIND_PATTERN && inner && inner->kind == IDM_CORE_MATCH_BIND) {
                inner->as.match_bind.body = core;
                core = inner;
            } else if (rec->kind == BODY_REC_BIND_PATTERN) {
                bool env_bind = body_bind_is_env(ctx);
                for (size_t n = rec->pattern_name_count; n > 0 && !failed; n--) {
                    IdmCore *value = pattern_extract_value(rec, n - 1u, err);
                    if (!value) {
                        idm_core_free(core);
                        core = NULL;
                        failed = true;
                        break;
                    }
                    IdmSpan nspan = rec->pattern_names[n - 1u]->span;
                    if (env_bind) {
                        IdmCore *letrec = idm_core_letrec(nspan);
                        bool ok = letrec != NULL && idm_core_letrec_add(letrec, rec->pattern_names[n - 1u]->as.text, rec->pattern_slots[n - 1u], value);
                        if (ok) ok = idm_core_letrec_set_body(letrec, core);
                        else idm_core_free(value);
                        if (!ok) {
                            idm_core_free(letrec);
                            idm_core_free(core);
                            core = NULL;
                            idm_error_oom(err, rec->form->span);
                            failed = true;
                        } else {
                            idm_core_letrec_set_env(letrec);
                            core = letrec;
                        }
                    } else {
                        core = idm_core_bind_local(rec->pattern_names[n - 1u]->as.text, rec->pattern_slots[n - 1u], value, core, nspan);
                        if (!core) {
                            idm_core_free(value);
                            idm_error_oom(err, rec->form->span);
                            failed = true;
                        }
                    }
                }
                if (!failed) {
                    core = idm_core_bind_local("_match", rec->pattern_tmp_slot, inner, core, rec->form->span);
                    if (!core) { idm_core_free(inner); idm_error_oom(err, rec->form->span); failed = true; }
                } else {
                    idm_core_free(inner);
                }
            } else if (rec->kind == BODY_REC_GROUPS) {
                if (!idm_core_letrec_set_body(inner, core)) {
                    idm_core_free(inner);
                    idm_core_free(core);
                    core = NULL;
                    idm_error_oom(err, rec->form->span);
                    failed = true;
                } else {
                    core = inner;
                }
            } else {
                IdmCore *do_expr = idm_core_do(rec->form->span);
                if (!do_expr || !idm_core_do_add(do_expr, inner) || !idm_core_do_add(do_expr, core)) {
                    idm_core_free(inner);
                    idm_core_free(core);
                    idm_core_free(do_expr);
                    core = NULL;
                    idm_error_oom(err, rec->form->span);
                    failed = true;
                } else {
                    core = do_expr;
                }
            }
        }
    }

    if (!failed) {
        size_t prealloc_count = 0;
        for (size_t r = 0; r < rec_count; r++) if (recs[r].prealloc) prealloc_count++;
        if (prealloc_count != 0) {
            IdmCore *prelude = idm_core_letrec(idm_span_unknown(NULL));
            bool ok = prelude != NULL;
            for (size_t r = 0; ok && r < rec_count; r++) {
                if (!recs[r].prealloc) continue;
                if (recs[r].kind == BODY_REC_GROUPS) {
                    for (size_t k = 0; ok && k < recs[r].group_count; k++) {
                        IdmCore *nil = idm_core_literal(idm_nil(), idm_span_unknown(NULL));
                        ok = nil != NULL && idm_core_letrec_add(prelude, recs[r].groups[k].name, recs[r].groups[k].slot, nil);
                        if (!ok && nil) idm_core_free(nil);
                    }
                } else if (recs[r].kind == BODY_REC_BIND) {
                    IdmCore *nil = idm_core_literal(idm_nil(), idm_span_unknown(NULL));
                    ok = nil != NULL && idm_core_letrec_add(prelude, recs[r].bind_name->as.text, recs[r].bind_slot, nil);
                    if (!ok && nil) idm_core_free(nil);
                }
            }
            ok = ok && idm_core_letrec_set_body(prelude, core);
            if (!ok) {
                idm_core_free(prelude);
                idm_core_free(core);
                core = NULL;
                idm_error_oom(err, idm_span_unknown(NULL));
                failed = true;
            } else {
                core = prelude;
            }
        }
    }

    if (!failed && prepared_count != 0) {
        IdmCore *seq = idm_core_do(idm_span_unknown(NULL));
        bool ok = seq != NULL;
        for (size_t p = 0; ok && p < prepared_count; p++) {
            ok = idm_core_do_add(seq, prepared[p]);
            if (ok) prepared[p] = NULL;
        }
        if (ok) {
            ok = idm_core_do_add(seq, core);
            if (ok) core = seq;
        }
        if (!ok) {
            idm_core_free(seq);
            idm_core_free(core);
            core = NULL;
            idm_error_oom(err, idm_span_unknown(NULL));
            failed = true;
        }
    }

    ctx->def_ctx = def_ctx.prev;
    ctx->op_fallback = saved_op_fallback;
    ctx->body_depth = saved_body_depth;
    idm_scope_set_destroy(&body_ctx_scopes);
    body_signatures_destroy(signatures, signature_count);
    body_recs_destroy(recs, rec_count);
    free(schedule);
    for (size_t p = 0; p < prepared_count; p++) idm_core_free(prepared[p]);
    free(prepared);
    for (size_t o = 0; o < owned_count; o++) idm_syn_free(owned[o]);
    free(owned);
    free(work);
    if (failed) {
        idm_core_free(core);
        return NULL;
    }
    return core;
}

IdmCore *expand_fn_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (start >= end) {
        return expand_error(err, idm_span_unknown(NULL), "expected fn literal");
    }
    return expand_function_literal(ctx, "<lambda>", items[start], items, start + 1u, end, err);
}

IdmCore *expand_function_literal_with_contract(ExpandContext *ctx, const char *debug_name, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, const IdmCallableContract *contract, IdmError *err) {
    if (param_start + 1u == end && body_is_clauses(items[param_start], true)) {
        return build_clause_fn_styled(ctx, items[param_start], items[param_start]->as.seq.count, debug_name, true, contract, err);
    }
    SavedFunctionContext saved;
    begin_function_context(ctx, &saved);
    ctx->function_contract = contract;

    IdmPattern **patterns = NULL;
    uint32_t pattern_count = 0;
    IdmPatternLocal *pattern_locals = NULL;
    uint32_t pattern_local_count = 0;
    IdmCore *guard = NULL;
    uint32_t arity = 0;
    IdmCore *body = expand_function_clause(ctx, debug_name, head, items, param_start, end, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err);
    if (!body) {
        end_function_context(ctx, &saved);
        return NULL;
    }

    IdmCore *fn = idm_core_fn(debug_name, arity, body, head->span);
    if (!fn
        || !idm_core_fn_set_param_patterns_take(fn, patterns, pattern_count)
        || !idm_core_fn_set_pattern_locals_take(fn, pattern_locals, pattern_local_count)
        || (guard && !idm_core_fn_set_guard_take(fn, guard))) {
        for (uint32_t p = 0; p < pattern_count; p++) idm_pat_free(patterns[p]);
        free(patterns);
        for (uint32_t p = 0; p < pattern_local_count; p++) free(pattern_locals[p].name);
        free(pattern_locals);
        idm_core_free(guard);
        idm_core_free(body);
        idm_core_free(fn);
        end_function_context(ctx, &saved);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, head->span);
    }
    for (size_t i = 0; i < ctx->capture_count; i++) {
        if (!idm_core_fn_add_capture(fn, ctx->captures[i].kind, ctx->captures[i].name, ctx->captures[i].source_index, ctx->captures[i].binding_id)) {
            idm_core_free(fn);
            end_function_context(ctx, &saved);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, head->span);
        }
    }
    end_function_context(ctx, &saved);
    return fn;
}

IdmCore *expand_function_literal(ExpandContext *ctx, const char *debug_name, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmError *err) {
    return expand_function_literal_with_contract(ctx, debug_name, head, items, param_start, end, NULL, err);
}

bool expand_unit_open_at(const ExpandContext *ctx, IdmSpan span) {
    const char *source = ctx->unit_source;
    if (!source || span.end == 0) return false;
    if (span.file && ctx->unit_label && strcmp(span.file, ctx->unit_label) != 0) return false;
    size_t len = strlen(source);
    if (span.end > len) return false;
    for (size_t i = span.end; i < len;) {
        char ch = source[i];
        if (ch == '#') {
            while (i < len && source[i] != '\n') i++;
            continue;
        }
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') return false;
        i++;
    }
    return true;
}

static bool parse_function_clause_shape(ExpandContext *ctx, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, bool allow_bodyless, FunctionClauseShape *out, IdmError *err) {
    FunctionClauseShape shape;
    memset(&shape, 0, sizeof(shape));
    shape.param_start = param_start;
    shape.param_end = param_start;
    shape.guard_start = SIZE_MAX;
    shape.guard_end = SIZE_MAX;
    shape.body_start = SIZE_MAX;
    shape.body_end = end;
    shape.body_index = SIZE_MAX;

    size_t cursor = param_start;
    size_t count = 0;
    while (cursor < end) {
        if (syn_is_word(items[cursor], "->")) {
            shape.has_arrow = true;
            shape.body_start = cursor + 1u;
            shape.param_end = cursor;
            cursor++;
            break;
        }
        if (idm_syn_is_form_id(items[cursor], IDM_FORM_BODY)) {
            shape.body_index = cursor;
            shape.param_end = cursor;
            break;
        }
        if (syn_is_word(items[cursor], "when")) {
            shape.has_guard = true;
            shape.guard_start = cursor + 1u;
            shape.param_end = cursor;
            cursor++;
            while (cursor < end) {
                if (syn_is_word(items[cursor], "->")) {
                    shape.has_arrow = true;
                    shape.guard_end = cursor;
                    shape.body_start = cursor + 1u;
                    cursor++;
                    break;
                }
                if (idm_syn_is_form_id(items[cursor], IDM_FORM_BODY)) {
                    shape.guard_end = cursor;
                    shape.body_index = cursor;
                    break;
                }
                cursor++;
            }
            if (shape.guard_end == SIZE_MAX) shape.guard_end = cursor;
            if (shape.guard_start == shape.guard_end) {
                expand_error(err, items[shape.guard_start - 1u]->span, "guard requires an expression before the body");
                return false;
            }
            break;
        }
        count++;
        cursor++;
    }
    if (!shape.has_guard && !shape.has_arrow && shape.body_index == SIZE_MAX) shape.param_end = cursor;
    if (count > UINT32_MAX) {
        expand_error(err, head->span, "function clause has too many parameters");
        return false;
    }
    shape.arity = (uint32_t)count;

    if (shape.has_arrow) {
        if (shape.body_start >= end) {
            expand_error(err, head->span, "function clause arrow requires a body");
            if (expand_unit_open_at(ctx, items[end - 1u]->span)) idm_error_reason(ctx->rt, err, "incomplete", 0);
            return false;
        }
        shape.body_end = end;
    } else if (shape.body_index != SIZE_MAX) {
        if (shape.body_index + 1u != end) {
            expand_error(err, items[shape.body_index]->span, "function clause do/end body must be final");
            return false;
        }
    } else if (allow_bodyless) {
        shape.body_start = end;
    } else {
        expand_error(err, head->span, "function clause requires -> or do/end body");
        return false;
    }
    *out = shape;
    return true;
}

static const char *guard_predicate_type(ExpandContext *ctx, const IdmCore *guard, uint32_t *out_slot) {
    if (!guard) return NULL;
    if (guard->kind == IDM_CORE_RECORD_IS && guard->as.record_is.value && guard->as.record_is.value->kind == IDM_CORE_ARG_REF) {
        *out_slot = guard->as.record_is.value->as.slot_ref.slot;
        return idm_symbol_text(guard->as.record_is.type);
    }
    if (guard->kind != IDM_CORE_CALL || guard->as.call.arg_count != 1) return NULL;
    const IdmCore *arg = guard->as.call.args[0];
    if (!arg || arg->kind != IDM_CORE_ARG_REF) return NULL;
    const IdmCore *callee = guard->as.call.callee;
    const char *pname = NULL;
    if (callee) {
        switch (callee->kind) {
            case IDM_CORE_ARG_REF:
            case IDM_CORE_LOCAL_REF:
            case IDM_CORE_CAPTURE_REF:
            case IDM_CORE_ENV_REF:
                pname = callee->as.slot_ref.name;
                break;
            case IDM_CORE_PACKAGE_REF:
                pname = callee->as.package_ref.name;
                break;
            default:
                break;
        }
    }
    if (!pname) return NULL;
    size_t n = strcspn(pname, "#");
    if (n < 2 || pname[n - 1u] != '?') return NULL;
    static _Thread_local char base[96];
    if (n - 1u >= sizeof(base)) return NULL;
    memcpy(base, pname, n - 1u);
    base[n - 1u] = '\0';
    const TypeDef *td = type_def_lookup_name(ctx, base);
    if (td) {
        *out_slot = arg->as.slot_ref.slot;
        const char *id = type_def_identity_text(td);
        return id ? id : base;
    }
    if (idm_type_name_is_builtin(base)) {
        *out_slot = arg->as.slot_ref.slot;
        return base;
    }
    return NULL;
}

static bool clause_guard_refine(ExpandContext *ctx, const IdmCore *guard) {
    uint32_t slot = 0;
    const char *tname = guard_predicate_type(ctx, guard, &slot);
    if (!tname) return true;
    for (size_t i = ctx->bindings.count; i > 0; i--) {
        IdmBinding *b = &ctx->bindings.items[i - 1u];
        if (b->kind != IDM_BIND_ARG || b->frame_id != ctx->frame || b->payload != slot) continue;
        if (b->has_contract) return true;
        IdmTypeTerm con;
        if (!idm_type_con(ctx->rt, &con, tname)) return false;
        IdmCallableContract contract;
        bool ok = callable_contract_from_value_type(&con, &contract);
        if (ok) ok = idm_binding_table_set_contract(&ctx->bindings, b->id, &contract);
        idm_callable_contract_destroy(&contract);
        idm_type_term_destroy(&con);
        return ok;
    }
    return true;
}

static IdmCore *expand_function_clause(ExpandContext *ctx, const char *debug_name, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmPattern ***out_patterns, uint32_t *out_pattern_count, IdmPatternLocal **out_locals, uint32_t *out_local_count, IdmCore **out_guard, uint32_t *out_arity, IdmError *err) {
    FunctionClauseShape shape;
    if (!parse_function_clause_shape(ctx, head, items, param_start, end, false, &shape, err)) return NULL;
    SavedClauseContext saved;
    begin_clause_context(ctx, &saved);

    IdmPattern **patterns = NULL;
    size_t pattern_count = 0;
    size_t pattern_cap = 0;
    for (size_t cursor = shape.param_start; cursor < shape.param_end; cursor++) {
        if (pattern_count == pattern_cap) {
            if (!idm_grow((void **)&patterns, &pattern_cap, sizeof(*patterns), 4u, pattern_count + 1u)) {
                for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
                free(patterns);
                end_clause_context(ctx, &saved);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, items[cursor]->span);
            }
        }
        IdmPattern *pat = pattern_from_param(ctx, items[cursor], (uint32_t)pattern_count, err);
        if (!pat) {
            for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
            free(patterns);
            end_clause_context(ctx, &saved);
            return NULL;
        }
        patterns[pattern_count++] = pat;
    }

    IdmCore *guard = NULL;
    if (shape.has_guard) {
        guard = expand_parts(ctx, items, shape.guard_start, shape.guard_end, err);
        if (!guard) {
            for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
            free(patterns);
            end_clause_context(ctx, &saved);
            return NULL;
        }
        if (!clause_guard_refine(ctx, guard)) {
            idm_core_free(guard);
            for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
            free(patterns);
            end_clause_context(ctx, &saved);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, head->span);
        }
    }

    IdmCore *body = NULL;
    if (shape.has_arrow) {
        body = expand_parts(ctx, items, shape.body_start, shape.body_end, err);
    } else {
        body = expand_syntax(ctx, items[shape.body_index], err);
    }
    if (!body) {
        idm_core_free(guard);
        end_clause_context(ctx, &saved);
        for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
        free(patterns);
        return NULL;
    }
    if (!copy_pattern_locals(ctx, saved.table_base, out_locals, out_local_count)) {
        idm_core_free(guard);
        end_clause_context(ctx, &saved);
        for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
        free(patterns);
        idm_core_free(body);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, head->span);
    }
    prune_unused_clause_binds(ctx, saved.table_base, patterns, pattern_count, *out_locals, out_local_count);
    end_clause_context(ctx, &saved);
    *out_patterns = patterns;
    *out_pattern_count = (uint32_t)pattern_count;
    *out_guard = guard;
    *out_arity = shape.arity;
    (void)debug_name;
    return body;
}

static bool defn_form_clause_block(const IdmSyntax *def_form, size_t param_start, const IdmSyntax **out_body) {
    if (param_start + 1u != def_form->as.seq.count) return false;
    const IdmSyntax *body = def_form->as.seq.items[param_start];
    if (!body_is_clauses(body, true)) return false;
    *out_body = body;
    return true;
}

static const IdmSyntax *function_clause_head(const IdmSyntax *clause) {
    return clause->as.seq.count > 1u ? clause->as.seq.items[1] : clause;
}

static bool defn_contract_mark_arity(const DefnGroup *group, uint32_t arity, bool *seen, IdmError *err) {
    if (!group) return true;
    bool matched = false;
    for (size_t i = 0; i < group->contract.sig_count; i++) {
        if (group->contract.sigs[i].arg_count != arity) continue;
        seen[i] = true;
        matched = true;
    }
    if (matched) return true;
    IdmBuffer expected;
    idm_buf_init(&expected);
    if (!contract_arity_describe_exact(&expected, &group->contract)) {
        idm_buf_destroy(&expected);
        return idm_error_oom(err, group->name_syntax->span);
    }
    bool set = idm_error_set(err, group->name_syntax->span, "signature for '%s' expects arity %s, definition has %u", group->name, expected.data, arity);
    idm_buf_destroy(&expected);
    return set;
}

static bool add_function_clause_arity(ExpandContext *ctx, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmArity *arity, const DefnGroup *group, bool *seen, IdmError *err) {
    FunctionClauseShape shape;
    if (!parse_function_clause_shape(ctx, head, items, param_start, end, true, &shape, err)) return false;
    return idm_arity_add_exact(arity, shape.arity) && defn_contract_mark_arity(group, shape.arity, seen, err);
}

static bool function_clause_body_arity(ExpandContext *ctx, const IdmSyntax *body, IdmArity *arity, const DefnGroup *group, bool *seen, IdmError *err) {
    for (size_t c = 1; c < body->as.seq.count; c++) {
        const IdmSyntax *clause = body->as.seq.items[c];
        if (!add_function_clause_arity(ctx, function_clause_head(clause), clause->as.seq.items, 1u, clause->as.seq.count, arity, group, seen, err)) return false;
    }
    return true;
}

static bool add_function_parts_arity(ExpandContext *ctx, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmArity *arity, const DefnGroup *group, bool *seen, IdmError *err) {
    if (param_start + 1u == end && body_is_clauses(items[param_start], true)) {
        return function_clause_body_arity(ctx, items[param_start], arity, group, seen, err);
    }
    return add_function_clause_arity(ctx, head, items, param_start, end, arity, group, seen, err);
}

static bool function_parts_arity(ExpandContext *ctx, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmArity *out_arity, IdmError *err) {
    IdmArity arity = idm_arity_unknown();
    if (!add_function_parts_arity(ctx, head, items, param_start, end, &arity, NULL, NULL, err)) return false;
    *out_arity = arity;
    return true;
}

static bool defn_group_arity(ExpandContext *ctx, const DefnGroup *group, IdmSyntax *const *items, IdmArity *out_arity, IdmError *err) {
    IdmArity arity = idm_arity_unknown();
    bool *seen = NULL;
    if (group->has_contract) {
        for (size_t i = 0; i < group->contract.sig_count; i++) {
            if (group->contract.sigs[i].arg_count > UINT32_MAX) return idm_error_set(err, group->name_syntax->span, "signature for '%s' has too many arguments", group->name);
        }
        if (group->contract.sig_count != 0) {
            seen = calloc(group->contract.sig_count, sizeof(*seen));
            if (!seen) return idm_error_oom(err, group->name_syntax->span);
        }
    }
    for (size_t i = 0; i < group->count; i++) {
        const IdmSyntax *def_form = items[group->indices[i]];
        const IdmSyntax *def_name = NULL;
        size_t param_start = 0;
        (void)defn_form_parts(def_form, &def_name, &param_start, NULL);
        if (!add_function_parts_arity(ctx, def_form->as.seq.items[1], def_form->as.seq.items, param_start, def_form->as.seq.count, &arity, group->has_contract ? group : NULL, seen, err)) {
            free(seen);
            return false;
        }
    }
    for (size_t i = 0; i < group->contract.sig_count; i++) {
        if (seen[i]) continue;
        size_t expected = group->contract.sigs[i].arg_count;
        free(seen);
        return idm_error_set(err, group->name_syntax->span, "signature for '%s' expects arity %zu, definition has no matching clause", group->name, expected);
    }
    free(seen);
    *out_arity = arity;
    return true;
}

bool function_literal_arity(ExpandContext *ctx, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmArity *out_arity, IdmError *err) {
    return function_parts_arity(ctx, head, items, param_start, end, out_arity, err);
}

static IdmCore *expand_defn_group(ExpandContext *ctx, const DefnGroup *group, IdmSyntax *const *items, IdmError *err) {
    size_t total_clauses = 0;
    for (size_t i = 0; i < group->count; i++) {
        const IdmSyntax *def_form = items[group->indices[i]];
        const IdmSyntax *def_name = NULL;
        size_t param_start = 0;
        (void)defn_form_parts(def_form, &def_name, &param_start, NULL);
        const IdmSyntax *clause_body = NULL;
        total_clauses += defn_form_clause_block(def_form, param_start, &clause_body) ? clause_body->as.seq.count - 1u : 1u;
    }
    bool single = total_clauses == 1;
    SavedFunctionContext saved;
    begin_function_context(ctx, &saved);
    ctx->function_contract = group->has_contract ? &group->contract : NULL;
    IdmCore *result = single ? NULL : idm_core_fn_multi(group->name, items[group->indices[0]]->span);
    if (!single && !result) {
        end_function_context(ctx, &saved);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, items[group->indices[0]]->span);
    }
    IdmCore *single_body = NULL;
    IdmPattern **single_patterns = NULL;
    uint32_t single_pattern_count = 0;
    IdmPatternLocal *single_pattern_locals = NULL;
    uint32_t single_pattern_local_count = 0;
    IdmCore *single_guard = NULL;
    uint32_t single_arity = 0;

    for (size_t i = 0; i < group->count; i++) {
        const IdmSyntax *def_form = items[group->indices[i]];
        const IdmSyntax *def_name = NULL;
        size_t param_start = 0;
        (void)defn_form_parts(def_form, &def_name, &param_start, NULL);
        const IdmSyntax *clause_block = NULL;
        if (defn_form_clause_block(def_form, param_start, &clause_block)) {
            bool block_ok = true;
            for (size_t c = 1; c < clause_block->as.seq.count && block_ok; c++) {
                const IdmSyntax *clause = clause_block->as.seq.items[c];
                IdmPattern **patterns = NULL;
                uint32_t pattern_count = 0;
                IdmPatternLocal *pattern_locals = NULL;
                uint32_t pattern_local_count = 0;
                IdmCore *guard = NULL;
                uint32_t arity = 0;
                IdmCore *body = expand_function_clause(ctx, def_name ? def_name->as.text : group->name, clause->as.seq.count > 1 ? clause->as.seq.items[1] : clause, clause->as.seq.items, 1, clause->as.seq.count, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err);
                if (!body) { block_ok = false; break; }
                if (single) {
                    single_body = body;
                    single_patterns = patterns;
                    single_pattern_count = pattern_count;
                    single_pattern_locals = pattern_locals;
                    single_pattern_local_count = pattern_local_count;
                    single_guard = guard;
                    single_arity = arity;
                } else if (!idm_core_fn_multi_add_clause_take(result, arity, patterns, pattern_count, pattern_locals, pattern_local_count, guard, body)) {
                    for (uint32_t p = 0; p < pattern_count; p++) idm_pat_free(patterns[p]);
                    free(patterns);
                    for (uint32_t p = 0; p < pattern_local_count; p++) free(pattern_locals[p].name);
                    free(pattern_locals);
                    idm_core_free(guard);
                    idm_core_free(body);
                    block_ok = false;
                    if (err && !err->present) idm_error_oom(err, clause->span);
                }
            }
            if (!block_ok) {
                idm_core_free(result);
                end_function_context(ctx, &saved);
                return NULL;
            }
            continue;
        }
        IdmPattern **patterns = NULL;
        uint32_t pattern_count = 0;
        IdmPatternLocal *pattern_locals = NULL;
        uint32_t pattern_local_count = 0;
        IdmCore *guard = NULL;
        uint32_t arity = 0;
        IdmCore *body = expand_function_clause(ctx, def_name ? def_name->as.text : group->name, def_form->as.seq.items[1], def_form->as.seq.items, param_start, def_form->as.seq.count, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err);
        if (!body) {
            idm_core_free(result);
            end_function_context(ctx, &saved);
            return NULL;
        }
        if (single) {
            single_body = body;
            single_patterns = patterns;
            single_pattern_count = pattern_count;
            single_pattern_locals = pattern_locals;
            single_pattern_local_count = pattern_local_count;
            single_guard = guard;
            single_arity = arity;
        } else if (!idm_core_fn_multi_add_clause_take(result, arity, patterns, pattern_count, pattern_locals, pattern_local_count, guard, body)) {
            for (uint32_t p = 0; p < pattern_count; p++) idm_pat_free(patterns[p]);
            free(patterns);
            for (uint32_t p = 0; p < pattern_local_count; p++) free(pattern_locals[p].name);
            free(pattern_locals);
            idm_core_free(guard);
            idm_core_free(body);
            idm_core_free(result);
            end_function_context(ctx, &saved);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, def_form->span);
        }
    }

    if (single) {
        result = idm_core_fn(group->name, single_arity, single_body, items[group->indices[0]]->span);
        if (!result || !idm_core_fn_set_param_patterns_take(result, single_patterns, single_pattern_count) || !idm_core_fn_set_pattern_locals_take(result, single_pattern_locals, single_pattern_local_count) || (single_guard && !idm_core_fn_set_guard_take(result, single_guard))) {
            for (uint32_t p = 0; p < single_pattern_count; p++) idm_pat_free(single_patterns[p]);
            free(single_patterns);
            for (uint32_t p = 0; p < single_pattern_local_count; p++) free(single_pattern_locals[p].name);
            free(single_pattern_locals);
            idm_core_free(single_guard);
            idm_core_free(single_body);
            idm_core_free(result);
            end_function_context(ctx, &saved);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, items[group->indices[0]]->span);
        }
    }

    for (size_t i = 0; i < ctx->capture_count; i++) {
        bool ok = single ? idm_core_fn_add_capture(result, ctx->captures[i].kind, ctx->captures[i].name, ctx->captures[i].source_index, ctx->captures[i].binding_id) : idm_core_fn_multi_add_capture(result, ctx->captures[i].kind, ctx->captures[i].name, ctx->captures[i].source_index, ctx->captures[i].binding_id);
        if (!ok) {
            idm_core_free(result);
            end_function_context(ctx, &saved);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, items[group->indices[0]]->span);
        }
    }
    end_function_context(ctx, &saved);
    return result;
}

static IdmCore *expand_match_clause(ExpandContext *ctx, const IdmSyntax *clause, IdmPattern ***out_patterns, uint32_t *out_pattern_count, IdmPatternLocal **out_locals, uint32_t *out_local_count, IdmCore **out_guard, uint32_t *out_arity, IdmError *err) {
    if (!idm_syn_is_form_id(clause, IDM_FORM_EXPR)) {
        expand_error(err, clause->span, "match clause must be an expression");
        return NULL;
    }
    FunctionClauseShape shape;
    const IdmSyntax *head = clause->as.seq.count > 1u ? clause->as.seq.items[1] : clause;
    if (!parse_function_clause_shape(ctx, head, clause->as.seq.items, 1u, clause->as.seq.count, false, &shape, err)) return NULL;
    if (!shape.has_arrow || shape.body_index != SIZE_MAX) {
        expand_error(err, clause->span, "match clause must have form pattern -> body");
        return NULL;
    }
    if (shape.arity != 1u) {
        expand_error(err, clause->span, shape.arity == 0u ? "match clause must have form pattern -> body" : "match currently expects one pattern per clause");
        return NULL;
    }

    SavedClauseContext saved;
    begin_clause_context(ctx, &saved);
    IdmPattern **patterns = calloc(1u, sizeof(*patterns));
    if (!patterns) {
        end_clause_context(ctx, &saved);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, clause->span);
    }
    patterns[0] = pattern_from_param(ctx, clause->as.seq.items[shape.param_start], 0, err);
    if (!patterns[0]) {
        free(patterns);
        end_clause_context(ctx, &saved);
        return NULL;
    }
    IdmCore *guard = NULL;
    if (shape.has_guard) {
        guard = expand_parts(ctx, clause->as.seq.items, shape.guard_start, shape.guard_end, err);
        if (!guard) {
            idm_pat_free(patterns[0]);
            free(patterns);
            end_clause_context(ctx, &saved);
            return NULL;
        }
    }
    IdmCore *body = expand_parts(ctx, clause->as.seq.items, shape.body_start, shape.body_end, err);
    if (!body) {
        idm_core_free(guard);
        end_clause_context(ctx, &saved);
        idm_pat_free(patterns[0]);
        free(patterns);
        return NULL;
    }
    if (!copy_pattern_locals(ctx, saved.table_base, out_locals, out_local_count)) {
        idm_core_free(guard);
        end_clause_context(ctx, &saved);
        idm_pat_free(patterns[0]);
        free(patterns);
        idm_core_free(body);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, clause->span);
    }
    prune_unused_clause_binds(ctx, saved.table_base, patterns, 1u, *out_locals, out_local_count);
    end_clause_context(ctx, &saved);
    *out_patterns = patterns;
    *out_pattern_count = 1;
    *out_guard = guard;
    *out_arity = shape.arity;
    return body;
}

static bool body_is_clauses(const IdmSyntax *body, bool allow_empty_pattern) {
    if (!idm_syn_is_form_id(body, IDM_FORM_BODY) || body->as.seq.count < 2) return false;
    const IdmSyntax *first = body->as.seq.items[1];
    if (!idm_syn_is_form_id(first, IDM_FORM_EXPR)) return false;
    if (signature_form_parts(first, NULL, NULL)) return false;
    for (size_t i = 1; i < first->as.seq.count; i++) {
        const IdmSyntax *tok = first->as.seq.items[i];
        if (syn_is_word(tok, "->")) return allow_empty_pattern || i > 1u;
        if (syn_is_word(tok, "=") || syn_is_word(tok, "fn") || syn_is_word(tok, "defn") ||
            syn_is_word(tok, "def") || syn_is_word(tok, "defmacro")) return false;
    }
    return false;
}

typedef struct {
    IdmCore *body;
    IdmPattern **patterns;
    uint32_t pattern_count;
    IdmPatternLocal *locals;
    uint32_t local_count;
    IdmCore *guard;
    uint32_t arity;
    size_t req_lo;
    size_t req_hi;
    bool taken;
} MatchClauseParts;

IdmCore *expand_match_form(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    if (!idm_syn_is_form_id(form, IDM_FORM_MATCH) || form->as.seq.count != 3u) return expand_error(err, form ? form->span : idm_span_unknown(NULL), "%-match expects a scrutinee and clause body");
    const IdmSyntax *body = form->as.seq.items[2];
    if (!body_is_clauses(body, false)) return expand_error(err, body ? body->span : form->span, "match requires a do/end clause body");
    IdmCore *scrutinee = expand_syntax(ctx, form->as.seq.items[1], err);
    if (!scrutinee) return NULL;
    bool scrutinee_taken = false;
    SavedFunctionContext saved;
    begin_function_context(ctx, &saved);
    IdmCore *match = idm_core_match(form->span);
    if (!match) {
        idm_core_free(match);
        idm_core_free(scrutinee);
        end_function_context(ctx, &saved);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    size_t req_base = ctx->match_size_count;
    size_t saved_clause_index = ctx->match_size_clause_index;
    ctx->match_size_depth++;
    size_t clause_count = body->as.seq.count - 1u;
    MatchClauseParts *cls = clause_count == 0 ? NULL : calloc(clause_count, sizeof(*cls));
    PinCollect pins = {0};
    ctx->pin_collect = &pins;
    bool ok = clause_count == 0 || cls != NULL;
    if (!ok) idm_error_oom(err, form->span);
    for (size_t i = 0; ok && i < clause_count; i++) {
        ctx->match_size_clause_index = i;
        cls[i].req_lo = ctx->match_size_count;
        cls[i].body = expand_match_clause(ctx, body->as.seq.items[i + 1u], &cls[i].patterns, &cls[i].pattern_count, &cls[i].locals, &cls[i].local_count, &cls[i].guard, &cls[i].arity, err);
        cls[i].req_hi = ctx->match_size_count;
        if (!cls[i].body) ok = false;
        else if (cls[i].arity != 1u) {
            expand_error(err, body->as.seq.items[i + 1u]->span, "match clause arity does not match scrutinee count");
            ok = false;
        }
    }
    ctx->pin_collect = NULL;
    ctx->match_size_depth--;
    ctx->match_size_clause_index = saved_clause_index;
    size_t req_count = ctx->match_size_count - req_base;
    for (size_t i = 0; ok && i < clause_count; i++) {
        size_t extra_count = req_count + pins.count;
        if (extra_count > UINT32_MAX - 1u) {
            ok = idm_error_set(err, form->span, "match has too many hidden pattern dependencies");
            break;
        }
        uint32_t width = (uint32_t)(1u + extra_count);
        if (extra_count != 0) {
            IdmPattern **wide = calloc(width, sizeof(*wide));
            if (!wide) { idm_error_oom(err, form->span); ok = false; break; }
            bool built = true;
            for (size_t r = 0; built && r < pins.count; r++) {
                if (pins.clause[r] == i) {
                    wide[r] = idm_pat_bind(pins.fresh[r], form->span);
                } else {
                    wide[r] = idm_pat_wildcard(form->span);
                }
                built = wide[r] != NULL;
            }
            wide[pins.count] = cls[i].patterns[0];
            for (size_t r = 0; built && r < req_count; r++) {
                size_t abs = req_base + r;
                if (abs >= cls[i].req_lo && abs < cls[i].req_hi) {
                    wide[pins.count + 1u + r] = idm_pat_bind(ctx->match_size_fresh[abs], form->span);
                } else {
                    wide[pins.count + 1u + r] = idm_pat_wildcard(form->span);
                }
                built = wide[pins.count + 1u + r] != NULL;
            }
            if (!built) {
                for (size_t r = 0; r < width; r++) {
                    if (r != pins.count) idm_pat_free(wide[r]);
                }
                free(wide);
                idm_error_oom(err, form->span);
                ok = false;
                break;
            }
            free(cls[i].patterns);
            cls[i].patterns = wide;
            cls[i].pattern_count = width;
        }
        if (!idm_core_match_add_clause_take(match, width, cls[i].patterns, cls[i].pattern_count, cls[i].locals, cls[i].local_count, cls[i].guard, cls[i].body)) {
            if (!err->present) idm_error_oom(err, body->as.seq.items[i + 1u]->span);
            ok = false;
            break;
        }
        cls[i].taken = true;
    }
    if (ok) {
        for (size_t i = 0; i < ctx->capture_count; i++) {
            if (!idm_core_match_add_capture(match, ctx->captures[i].kind, ctx->captures[i].name, ctx->captures[i].source_index, ctx->captures[i].binding_id)) {
                ok = idm_error_oom(err, form->span);
                break;
            }
        }
    }
    for (size_t i = 0; cls && i < clause_count; i++) {
        if (cls[i].taken) continue;
        idm_core_free(cls[i].body);
        idm_core_free(cls[i].guard);
        for (uint32_t p = 0; p < cls[i].pattern_count; p++) if (cls[i].patterns) idm_pat_free(cls[i].patterns[p]);
        free(cls[i].patterns);
        for (uint32_t p = 0; p < cls[i].local_count; p++) free(cls[i].locals[p].name);
        free(cls[i].locals);
    }
    free(cls);
    end_function_context(ctx, &saved);
    for (size_t r = 0; ok && r < pins.count; r++) {
        IdmCore *extra = expand_syntax(ctx, pins.outer[r], err);
        if (!extra || !idm_core_match_add_scrutinee(match, extra)) {
            idm_core_free(extra);
            if (err && !err->present) idm_error_oom(err, pins.outer[r]->span);
            ok = false;
        }
    }
    if (ok) {
        if (!idm_core_match_add_scrutinee(match, scrutinee)) {
            if (err && !err->present) idm_error_oom(err, form->span);
            ok = false;
        } else {
            scrutinee_taken = true;
        }
    }
    for (size_t r = req_base; ok && r < ctx->match_size_count; r++) {
        IdmCore *extra = expand_syntax(ctx, ctx->match_size_outer[r], err);
        if (!extra || !idm_core_match_add_scrutinee(match, extra)) {
            idm_core_free(extra);
            if (err && !err->present) idm_error_oom(err, form->span);
            ok = false;
        }
    }
    while (ctx->match_size_count > req_base) {
        size_t r = --ctx->match_size_count;
        idm_syn_free(ctx->match_size_outer[r]);
        free(ctx->match_size_fresh[r]);
    }
    pin_deps_free(pins.outer, pins.fresh, pins.count);
    free(pins.clause);
    if (!ok) {
        if (!scrutinee_taken) idm_core_free(scrutinee);
        idm_core_free(match);
        return NULL;
    }
    return match;
}

static IdmCore *build_clause_fn_styled(ExpandContext *ctx, const IdmSyntax *body, size_t clause_end, const char *debug_name, bool defn_style, const IdmCallableContract *contract, IdmError *err) {
    if (!idm_syn_is_form_id(body, IDM_FORM_BODY) || clause_end < 2u || body->as.seq.count < clause_end) return expand_error(err, body->span, "clause body requires at least one clause");
    SavedFunctionContext saved;
    begin_function_context(ctx, &saved);
    ctx->function_contract = contract;
    IdmCore *multi = idm_core_fn_multi(debug_name, body->span);
    if (!multi) {
        end_function_context(ctx, &saved);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, body->span);
    }
    for (size_t i = 1; i < clause_end; i++) {
        IdmPattern **patterns = NULL;
        uint32_t pattern_count = 0;
        IdmPatternLocal *pattern_locals = NULL;
        uint32_t pattern_local_count = 0;
        IdmCore *guard = NULL;
        uint32_t arity = 0;
        const IdmSyntax *clause = body->as.seq.items[i];
        IdmCore *clause_body = defn_style
            ? expand_function_clause(ctx, debug_name, clause->as.seq.count > 1 ? clause->as.seq.items[1] : clause, clause->as.seq.items, 1, clause->as.seq.count, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err)
            : expand_match_clause(ctx, clause, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err);
        if (!clause_body || !idm_core_fn_multi_add_clause_take(multi, arity, patterns, pattern_count, pattern_locals, pattern_local_count, guard, clause_body)) {
            if (clause_body) idm_core_free(clause_body);
            if (guard) idm_core_free(guard);
            if (patterns) {
                for (uint32_t p = 0; p < pattern_count; p++) idm_pat_free(patterns[p]);
                free(patterns);
            }
            if (pattern_locals) {
                for (uint32_t p = 0; p < pattern_local_count; p++) free(pattern_locals[p].name);
                free(pattern_locals);
            }
            idm_core_free(multi);
            end_function_context(ctx, &saved);
            if (!err->present) idm_error_oom(err, body->as.seq.items[i]->span);
            return NULL;
        }
    }
    for (size_t i = 0; i < ctx->capture_count; i++) {
        if (!idm_core_fn_multi_add_capture(multi, ctx->captures[i].kind, ctx->captures[i].name, ctx->captures[i].source_index, ctx->captures[i].binding_id)) {
            idm_core_free(multi);
            end_function_context(ctx, &saved);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, body->span);
        }
    }
    end_function_context(ctx, &saved);
    return multi;
}

static IdmCore *build_clause_fn(ExpandContext *ctx, const IdmSyntax *body, size_t clause_end, const char *debug_name, IdmError *err) {
    return build_clause_fn_styled(ctx, body, clause_end, debug_name, false, NULL, err);
}

IdmCore *expand_receive_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    size_t body_index = SIZE_MAX;
    for (size_t i = start + 1u; i < end; i++) {
        if (idm_syn_is_form_id(items[i], IDM_FORM_BODY)) {
            body_index = i;
            break;
        }
    }
    if (body_index == SIZE_MAX || body_index + 1u != end) return expand_error(err, items[start]->span, "receive requires a final do/end clause body");
    const IdmSyntax *body = items[body_index];
    if (body->as.seq.count < 2) return expand_error(err, body->span, "receive requires at least one clause");

    size_t clause_total = body->as.seq.count;
    const IdmSyntax *after_clause = NULL;
    size_t message_end = clause_total;
    for (size_t i = 1; i < clause_total; i++) {
        const IdmSyntax *clause = body->as.seq.items[i];
        if (idm_syn_is_form_id(clause, IDM_FORM_EXPR) && clause->as.seq.count >= 2 && syn_is_word(clause->as.seq.items[1], "after")) {
            if (i + 1u != clause_total) return expand_error(err, clause->span, "receive 'after' clause must be last");
            after_clause = clause;
            message_end = i;
            break;
        }
    }
    if (message_end < 2) return expand_error(err, body->span, "receive requires at least one message clause");

    IdmCore *timeout = NULL;
    IdmCore *timeout_body = NULL;
    if (after_clause) {
        size_t arrow = SIZE_MAX;
        for (size_t i = 2; i < after_clause->as.seq.count; i++) {
            if (syn_is_word(after_clause->as.seq.items[i], "->")) {
                arrow = i;
                break;
            }
        }
        if (arrow == SIZE_MAX || arrow == 2 || arrow + 1u >= after_clause->as.seq.count) return expand_error(err, after_clause->span, "receive 'after' clause must have form after timeout -> body");
        timeout = expand_parts(ctx, after_clause->as.seq.items, 2, arrow, err);
        if (!timeout) return NULL;
        timeout_body = expand_parts(ctx, after_clause->as.seq.items, arrow + 1u, after_clause->as.seq.count, err);
        if (!timeout_body) {
            idm_core_free(timeout);
            return NULL;
        }
    } else {
        timeout = idm_core_literal(idm_atom(ctx->rt, "infinity"), items[start]->span);
        timeout_body = idm_core_literal(idm_nil(), items[start]->span);
        if (!timeout || !timeout_body) {
            idm_core_free(timeout);
            idm_core_free(timeout_body);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
        }
    }

    IdmCore *multi = build_clause_fn(ctx, body, message_end, "<receive>", err);
    if (!multi) {
        idm_core_free(timeout);
        idm_core_free(timeout_body);
        return NULL;
    }

    IdmCore *recv = idm_core_receive(multi, timeout, timeout_body, items[start]->span);
    if (!recv) {
        idm_core_free(multi);
        idm_core_free(timeout);
        idm_core_free(timeout_body);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
    }
    return recv;
}
