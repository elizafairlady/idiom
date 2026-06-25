#ifndef IDM_EXPAND_INTERNAL_H
#define IDM_EXPAND_INTERNAL_H

#include "idiom/actor.h"
#include "idiom/expand.h"
#include "idiom/artifact.h"
#include "idiom/prims.h"
#include "idiom/vm.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define IDM_FRAME_ENV 0u
#define IDM_FRAME_TOP 1u

typedef struct {
    char *name;
    IdmScopeSet scopes;
    IdmCaptureKind kind;
    uint32_t source_index;
    uint32_t capture_index;
    IdmArity arity;
} CaptureBinding;
typedef struct {
    size_t table_base;
    uint32_t next_slot;
    uint32_t arg_slots;
} SavedClauseContext;
typedef struct {
    const char *name;
    const IdmSyntax *name_syntax;
    IdmScopeSet scopes;
    uint32_t slot;
    IdmArity arity;
    size_t *indices;
    size_t count;
    size_t cap;
    bool exported;
} DefnGroup;
typedef struct {
    const char **names;
    IdmSpan *spans;
    bool *matched;
    size_t count;
} UseSelection;
typedef struct {
    IdmModuleRef *module;
    uint32_t function_index;
    IdmPhaseEnv *phase_env;
    bool closure_backed;
    IdmValue closure;
} PhaseSyntaxFn;
typedef struct {
    char *name;
    PhaseSyntaxFn fn;
    bool exported;
    bool hidden;
} MacroDef;
typedef struct {
    char *name;
    IdmScopeSet scopes;
    PhaseSyntaxFn fn;
} CoreSyntaxDef;
typedef struct SavedFunctionContext_s {
    size_t table_base;
    uint32_t frame;
    uint32_t next_slot;
    uint32_t arg_slots;
    CaptureBinding *captures;
    size_t capture_count;
    size_t capture_cap;
    struct SavedFunctionContext_s *prev;
} SavedFunctionContext;
typedef struct {
    char *trait;
    char *name;
    char *provider;
    char *provider_key;
    IdmArity arity;
    bool is_field;
    IdmScopeSet scopes;
} MethodSurfaceDef;
typedef struct {
    char *name;
    char *public_name;
    bool exported;
    bool external;
    IdmScopeId provider_scope_base;
    int64_t provider_scope_delta;
    IdmArtifact art;
} ProtocolDef;
typedef struct {
    char *name;
    bool exported;
    IdmTraitRequirementDef *requirements;
    size_t requirement_count;
    IdmTraitMethodDef *methods;
    size_t method_count;
} TraitDef;
typedef struct {
    char *name;
    char *identity;
    bool exported;
    IdmScopeSet scopes;
    char **fields;
    size_t field_count;
} TypeDef;
typedef struct {
    IdmPkgGrammar artifact;
    IdmScopeSet binding_scopes;
    char *provider;
    char *provider_key;
} GrammarDef;
typedef struct {
    char *name;
    char *provider;
    char *provider_key;
    IdmBindingSpace space;
    int phase;
    IdmScopeSet scopes;
} SurfaceInstall;
typedef struct {
    char *name;
    IdmSpan span;
} SourceReaderDecl;
typedef struct {
    void *local_expand_user;
    IdmLocalExpandFn local_expand;
    void *free_identifier_eq_user;
    IdmFreeIdentifierEqFn free_identifier_eq;
    void *register_macro_user;
    IdmRegisterMacroFn register_macro;
} SavedHooks;
typedef struct {
    const void *art;
    IdmScopeId base;
    size_t init_emit_mark[2];
    size_t runtime_init_mark[2];
} ArtifactRuntimeState;
typedef struct {
    IdmScopeId next_scope;
    size_t binding_count;
    size_t macro_count;
    size_t core_syntax_count;
    size_t grammar_count;
    size_t decl_grammar_count;
    size_t operator_count;
    size_t protocol_count;
    size_t trait_count;
    size_t type_count;
    size_t method_surface_count;
    size_t decl_method_count;
    size_t decl_core_syntax_count;
    size_t decl_source_reader_count;
    size_t activation_count;
    size_t surface_install_count;
    size_t artifact_base_count;
    size_t init_emit_mark_count;
    size_t runtime_init_mark_count;
    size_t phase_runtime_init_mark_count;
    size_t package_slot_ref_count;
} SurfaceCheckpoint;
typedef struct {
    char *env_key;
    uint32_t slot;
} PackageSlotRef;
typedef struct {
    IdmRuntime *rt;
    IdmBindingTable bindings;
    IdmScopeSet empty_scopes;
    uint32_t next_slot;
    uint32_t arg_slots;
    CaptureBinding *captures;
    size_t capture_count;
    size_t capture_cap;
    uint32_t frame;
    uint32_t frame_seq;
    uint32_t env_slot_seq;
    bool in_package;
    const char *package_name;
    struct { char *name; uint32_t slot; IdmArity arity; } *exports;
    size_t export_count;
    size_t export_cap;
    IdmPkgSlot *package_slots;
    size_t package_slot_count;
    size_t package_slot_cap;
    MacroDef *macros;
    size_t macro_count;
    size_t macro_cap;
    CoreSyntaxDef *core_syntax;
    size_t core_syntax_count;
    size_t core_syntax_cap;
    GrammarDef *grammars;
    size_t grammar_count;
    size_t grammar_cap;
    IdmOperatorDef *operators;
    size_t operator_count;
    size_t operator_cap;
    ProtocolDef *protocols;
    size_t protocol_count;
    size_t protocol_cap;
    TraitDef *traits;
    size_t trait_count;
    size_t trait_cap;
    TypeDef *types;
    size_t type_count;
    size_t type_cap;
    MethodSurfaceDef *method_surfaces;
    size_t method_surface_count;
    size_t method_surface_cap;
    IdmTraitMethodDef *decl_methods;
    size_t decl_method_count;
    size_t decl_method_cap;
    IdmMacroRunner *runner;
    IdmMacroRunner local_runner;
    IdmScopeStore scope_store;
    uint32_t surface_depth;
    int phase;
    int surface_phase;
    bool value_context;
    bool command_sub_context;
    struct BodyDefCtx *def_ctx;
    struct ScopePropagation_s *scope_propagation;
    const IdmScopeSet *op_fallback;
    bool repl_env_binds;
    struct { char *name; char *provider; char *provider_key; IdmSpan span; } *activations;
    size_t activation_count;
    size_t activation_cap;
    SurfaceInstall *surface_installs;
    size_t surface_install_count;
    size_t surface_install_cap;
    CoreSyntaxDef *decl_core_syntax;
    size_t decl_core_syntax_count;
    size_t decl_core_syntax_cap;
    IdmPkgGrammar *decl_grammars;
    size_t decl_grammar_count;
    size_t decl_grammar_cap;
    SourceReaderDecl *decl_source_readers;
    size_t decl_source_reader_count;
    size_t decl_source_reader_cap;
    SavedFunctionContext *enclosing;
    IdmPhaseEnv *phase_env;
    const char *trait_name;
    bool kernel_wrap;
    bool kernel_phase_seeded;
    ArtifactRuntimeState *artifact_bases;
    size_t artifact_base_count;
    size_t artifact_base_cap;
    size_t init_emit_mark_count;
    size_t runtime_init_mark_count;
    size_t phase_runtime_init_mark_count;
    PackageSlotRef *package_slot_refs;
    size_t package_slot_ref_count;
    size_t package_slot_ref_cap;
    IdmArtifactDep *deps;
    size_t dep_count;
    size_t dep_cap;
    const char *unit;
    char unit_key[17];
    const char *primitive_home;
    const IdmSyntax **pat_binders;
    size_t pat_binder_count;
    size_t pat_binder_cap;
    bool pat_binder_collect;
} ExpandContext;
typedef struct BodyDefCtx {
    IdmScopeSet use_site;
    struct BodyDefCtx *prev;
} BodyDefCtx;
typedef struct ScopePropagation_s {
    IdmSyntax **work;
    size_t start;
    size_t count;
    struct ScopePropagation_s *prev;
} ScopePropagation;
typedef struct {
    ExpandContext *ctx;
    IdmSyntax *const *items;
    size_t end;
    size_t pos;
    IdmError *err;
} EnforestParser;
typedef struct {
    char *name;
    IdmArity arity;
    IdmCore *fn;
} TraitMethodImpl;
typedef struct {
    char *path;
    IdmArtifact art;
} CachedPackage;
typedef struct {
    IdmRuntime *rt;
    IdmArtifact kernel;
    char *kernel_path;
    IdmScopeId kernel_scope_end;
    bool kernel_ready;
    IdmReaderArtifact *source_reader;
    unsigned char source_reader_hash[32];
    bool source_reader_hash_ready;
    CachedPackage **pkgs;
    size_t pkg_count;
    size_t pkg_cap;
    const char **compiling;
    size_t compiling_count;
    size_t compiling_cap;
} ExpandCache;
typedef enum { BODY_REC_BIND, BODY_REC_BIND_PATTERN, BODY_REC_EXPR, BODY_REC_GROUPS } BodyRecKind;
typedef struct {
    BodyRecKind kind;
    const IdmSyntax *form;
    const IdmSyntax *bind_name;
    size_t rhs_start;
    uint32_t bind_slot;
    IdmArity bind_arity;
    const IdmSyntax *pattern_syntax;
    IdmPattern *pattern;
    const IdmSyntax **pattern_names;
    size_t pattern_name_count;
    uint32_t *pattern_slots;
    uint32_t pattern_tmp_slot;
    DefnGroup *groups;
    size_t group_count;
    IdmCore *core;
} BodyRec;

bool activate_artifact(ExpandContext *ctx, const char *activation_name, const IdmArtifact *art, IdmScopeId base, const IdmScopeSet *act_scopes, IdmSpan span, IdmError *err);
bool arg_push_slot(ExpandContext *ctx, const IdmSyntax *word, uint32_t slot);
bool artifact_base(ExpandContext *ctx, const IdmArtifact *art, IdmScopeId *out_base, IdmError *err);
bool artifact_init_pending(ExpandContext *ctx, const IdmArtifact *art);
const IdmArtifact *artifact_get(ExpandContext *ctx, const char *path, IdmSpan span, IdmError *err);
bool install_artifact_runtime_slots(ExpandContext *ctx, const IdmArtifact *art, const char *primitive_home, IdmScopeId min_id, int64_t delta, bool once, IdmSpan span, IdmError *err);
void begin_clause_context(ExpandContext *ctx, SavedClauseContext *saved);
void begin_function_context(ExpandContext *ctx, SavedFunctionContext *saved);
const CaptureBinding *capture_lookup_existing(const CaptureBinding *captures, size_t count, const IdmSyntax *word);
bool compile_package_module(ExpandContext *parent, const IdmSyntax *pkg, const char *unit_name_hint, const unsigned char src_hash[32], const IdmPkgSlot *inherited_slots, size_t inherited_count, IdmArtifact *out, IdmError *err);
bool ctx_activate_kernel(ExpandContext *ctx, IdmError *err);
void ctx_destroy(ExpandContext *ctx);
void ctx_init(ExpandContext *ctx, IdmRuntime *rt);
bool ctx_seed(ExpandContext *ctx, IdmError *err);
void end_clause_context(ExpandContext *ctx, SavedClauseContext *saved);
void end_function_context(ExpandContext *ctx, SavedFunctionContext *saved);
IdmCore *expand_body_items(ExpandContext *ctx, IdmSyntax *const *items, size_t index, size_t count, bool def_scope, IdmError *err);
IdmCore *expand_package_body_items(ExpandContext *ctx, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);
IdmCore *expand_error(IdmError *err, IdmSpan span, const char *fmt, ...);
IdmCore *expand_activate(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmSyntax *const *items, size_t index, size_t count, IdmSpan span, IdmError *err);
IdmCore *expand_fn_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
IdmCore *expand_function_literal(ExpandContext *ctx, const char *debug_name, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmError *err);
bool function_literal_arity(const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmArity *out_arity, IdmError *err);
IdmCore *expand_implement_trait_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);
IdmCore *expand_macro_use(ExpandContext *ctx, const IdmSyntax *use_syntax, const IdmSyntax *head, uint32_t payload, IdmError *err);
IdmCore *expand_macro_use_from_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, uint32_t payload, IdmError *err);
IdmCore *expand_core_syntax_use_from_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, uint32_t core_syntax_index, IdmError *err);
IdmCore *expand_method_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);
IdmCore *expand_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
IdmCore *expand_primitive_clause_call(IdmPrimitive primitive, IdmSpan span, IdmError *err);
bool core_call_add_arg_or_free(IdmCore *call, IdmCore *arg, IdmError *err, IdmSpan span);
IdmCore *expand_protocol_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);
IdmCore *expand_trait_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);
IdmCore *expand_form_quasiquote(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
IdmCore *expand_form_quasisyntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
IdmCore *expand_form_quote(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
IdmCore *expand_form_string(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
IdmCore *expand_receive_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
IdmCore *expand_record_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);
IdmCore *expand_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
IdmCore *expand_use(ExpandContext *ctx, const char *path, const char *qualifier, UseSelection *selection, IdmSyntax *const *items, size_t cont_index, size_t cont_count, IdmSpan span, IdmError *err);
bool free_identifier_eq_callback(void *user, IdmRuntime *rt, const IdmSyntax *a, const IdmSyntax *b, bool *out_equal, IdmError *err);
bool env_push_def_binder(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, uint32_t *out_id);
bool env_push_def_binder_with_arity(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, IdmArity arity, uint32_t *out_id);
bool package_slot_ref_add(ExpandContext *ctx, const char *env_key, uint32_t slot, uint32_t *out_id);
const PackageSlotRef *package_slot_ref_get(const ExpandContext *ctx, uint32_t id);
bool install_imported_macro(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, IdmModuleRef *module, uint32_t function_index, IdmPhaseEnv *phase_env, const char *provider, const char *provider_key, IdmError *err);
bool install_imported_core_syntax(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, IdmModuleRef *module, uint32_t function_index, IdmPhaseEnv *phase_env, const char *provider, const char *provider_key, IdmError *err);
bool install_imported_grammar(ExpandContext *ctx, const IdmPkgGrammar *grammar, const IdmScopeSet *scopes, const char *name, const char *provider, const char *provider_key, IdmScopeId min_id, int64_t delta, IdmError *err);
bool install_artifact_traits(ExpandContext *ctx, const IdmPkgTrait *traits, size_t trait_count, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err);
bool install_artifact_types(ExpandContext *ctx, const IdmPkgType *types, size_t type_count, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err);
bool install_artifact_protocols(ExpandContext *ctx, const IdmPkgProtocol *protocols, size_t protocol_count, const IdmScopeSet *scopes, const char *qualifier, IdmScopeId provider_scope_base, int64_t provider_scope_delta, const char *provider, const char *provider_key, IdmError *err);
bool install_artifact_grammars(ExpandContext *ctx, const IdmPkgGrammar *grammars, size_t grammar_count, const IdmScopeSet *scopes, const char *qualifier, IdmScopeId min_id, int64_t delta, const char *provider, const char *provider_key, IdmError *err);
bool ctx_reader_artifact_from_active_grammars(ExpandContext *ctx, const char *surface, IdmReaderArtifact **out, IdmError *err);
bool install_imported_operator(ExpandContext *ctx, const IdmOperatorDef *op, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, IdmError *err);
bool install_method_surface(ExpandContext *ctx, const char *trait, const char *name, IdmArity arity, bool is_field, const IdmScopeSet *scopes, const char *provider, const char *provider_key, IdmError *err);
IdmCore *expand_record_field_core(ExpandContext *ctx, IdmCore *receiver, const char *field, IdmSpan span, IdmError *err);
bool invoke_macro_to_syntax(ExpandContext *ctx, const IdmSyntax *use_syntax, const IdmSyntax *head, uint32_t payload, IdmSyntax **out_syntax, IdmError *err);
IdmCore *literal_from_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
bool local_expand_callback(void *user, IdmRuntime *rt, const IdmSyntax *syntax, IdmSyntax **out_syntax, IdmError *err);
bool dict_rest_index(const IdmSyntax *syn, size_t *out_index, IdmError *err);
bool try_qualified_word_at(IdmSyntax *const *items, size_t start, size_t end, IdmSyntax **out_word, size_t *out_end, IdmError *err);
IdmSyntax *expect_qualified_word_at(IdmSyntax *const *items, size_t start, size_t *inout_end, const char *message, IdmError *err);
bool local_macro_invoke(void *user, IdmRuntime *rt, uint32_t payload, const IdmSyntax *use_syntax, IdmSyntax **out_syntax, IdmError *err);
void local_pop_to(ExpandContext *ctx, size_t table_base, uint32_t next_slot);
bool local_push_def_binder(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, uint32_t *out_slot);
bool local_push_def_binder_with_arity(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, IdmArity arity, uint32_t *out_slot);
bool local_push_scoped(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, uint32_t *out_slot);
void grammar_def_destroy(GrammarDef *grammar);
void macro_def_destroy(MacroDef *macro);
void phase_syntax_fn_destroy(PhaseSyntaxFn *fn);
bool materialize_capture(ExpandContext *ctx, const IdmSyntax *word, const IdmBinding *b, uint32_t *out);
const IdmOperatorDef *op_lookup(const ExpandContext *ctx, const IdmSyntax *syn, bool want_prefix);
const IdmOperatorDef *op_lookup_syntax_capture(const ExpandContext *ctx, const IdmSyntax *syn, bool want_left, IdmError *err);
bool operator_capture_compile(const char *capture, uint8_t *out_kind, bool *out_left, uint32_t *out_count);
const char *package_path_text(const IdmSyntax *syn);
char *scoped_identity(ExpandContext *ctx, const char *name, const IdmSyntax *form, unsigned char out_hash[32], IdmError *err);
bool predeclare_trait_methods(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, IdmError *err);
void protocol_def_destroy(ProtocolDef *protocol);
void trait_def_destroy(TraitDef *trait);
void type_def_destroy(TypeDef *type);
bool record_export(ExpandContext *ctx, const char *name, uint32_t slot, IdmArity arity);
bool record_package_slot(ExpandContext *ctx, const char *name, uint32_t slot_id, const IdmScopeSet *scopes, IdmArity arity);
bool register_macro(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmCore *fn, IdmSpan span, bool exported, IdmError *err);
bool register_macro_callback(void *user, IdmRuntime *rt, const IdmSyntax *name_syntax, IdmValue transformer, IdmError *err);
bool register_operator(ExpandContext *ctx, const char *name, const char *capture, uint8_t precedence, IdmOpAssoc assoc, const char *target_name, const IdmScopeSet *scopes, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, bool exported, IdmError *err);
bool register_core_syntax(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmCore *fn, IdmSpan span, IdmError *err);
bool register_grammar(ExpandContext *ctx, const IdmSyntax *name_syntax, uint8_t mode, IdmGrammarRule *rules, size_t rule_count, bool exported, IdmError *err);
IdmBytecodeModule *relocated_module_copy(ExpandContext *ctx, const IdmBytecodeModule *src, IdmScopeId min_id, int64_t delta, uint32_t *out_fn_off, IdmError *err);
const IdmBinding *resolve_default(const ExpandContext *ctx, const IdmSyntax *word, IdmResolveStatus *out_status);
bool resolve_head_core_syntax(ExpandContext *ctx, const IdmSyntax *head, uint32_t *out_core_syntax_index, IdmError *err);
ProtocolDef *resolve_protocol_def(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmResolveStatus *out_status);
TraitDef *resolve_trait_def(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmResolveStatus *out_status);
TypeDef *resolve_type_def(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmResolveStatus *out_status);
bool resolve_transformer(ExpandContext *ctx, const IdmSyntax *head, uint32_t *out_payload, IdmError *err);
bool run_phase_core(ExpandContext *ctx, IdmCore *core, IdmError *err);
void surface_checkpoint(ExpandContext *ctx, SurfaceCheckpoint *checkpoint);
void surface_rollback(ExpandContext *ctx, const SurfaceCheckpoint *checkpoint);
bool ctx_validate_source_reader_decls(ExpandContext *ctx, IdmError *err);
IdmSyntax *syntax_use_from_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
bool syn_is_form(const IdmSyntax *syn, const char *head);
bool syn_is_word(const IdmSyntax *syn, const char *word);
bool syntax_scopes_copy(IdmScopeSet *dst, const IdmSyntax *syn);
bool value_from_literal_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmValue *out, IdmError *err);
bool expand_quote_datum(ExpandContext *ctx, const IdmSyntax *t, IdmValue *out, IdmError *err);
IdmCore *wrap_kernel_use(ExpandContext *ctx, IdmCore *body, IdmError *err);
IdmCore *wrap_phase_runtime_inits(ExpandContext *ctx, IdmCore *body, IdmError *err);
IdmCore *wrap_phase_runtime_inits_since(ExpandContext *ctx, IdmCore *body, size_t first_mark, IdmError *err);

bool expand_surface_value(ExpandContext *ctx, IdmRuntime *rt, const char *kind, IdmValue *out, IdmError *err);
void hooks_install(IdmRuntime *rt, ExpandContext *ctx, SavedHooks *saved);
void hooks_restore(IdmRuntime *rt, const SavedHooks *saved);
void ctx_set_unit(ExpandContext *ctx, const char *name, const unsigned char hash[32]);
bool record_activation(ExpandContext *ctx, const char *name, const char *provider, const char *provider_key, IdmSpan span, IdmError *err);
int surface_install_guard(ExpandContext *ctx, const char *provider, const char *provider_key, const char *key, const char *display, IdmBindingSpace space, const IdmScopeSet *scopes, IdmError *err);
int surface_bind_payload(ExpandContext *ctx, const char *provider, const char *provider_key, const char *key, const char *display, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, IdmSpan span, IdmError *err);
void artifact_provider_key(const unsigned char hash[32], char out[17]);
char *operator_binding_key(const char *name, const char *capture);
bool scopes_subset_for_ref(const IdmScopeSet *binding_scopes, const IdmSyntax *ref);
bool binder_scopes_pruned(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmScopeSet *out);
bool surface_decl_scopes_pruned(ExpandContext *ctx, const IdmSyntax *name_syntax, const IdmScopeSet *extent_scopes, IdmScopeSet *out);

#endif
