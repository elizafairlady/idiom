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
#define IDM_FRAME_GLOBAL 0u
#define IDM_FRAME_TOP 1u

typedef struct {
    char *name;
    IdmScopeSet scopes;
    IdmCaptureKind kind;
    uint32_t source_index;
    uint32_t capture_index;
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
    size_t *indices;
    size_t count;
    size_t cap;
    bool exported;
} DefnGroup;
typedef struct {
    char *name;
    IdmModuleRef *module;
    uint32_t function_index;
    IdmNamespace *phase_ns;
    IdmPhaseEnv *phase_env;
    bool exported;
    bool closure_backed;
    IdmValue transformer;
    IdmRuntime *rt;
} MacroDef;
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
    char *protocol;
    char *name;
    uint32_t arity;
    IdmScopeSet scopes;
} MethodSurfaceDef;
typedef struct {
    char *name;
    bool exported;
    IdmArtifact art;
} ProtocolDef;
typedef struct {
    char *name;
    char *provider;
    char *provider_key;
    IdmBindingSpace space;
    int phase;
    IdmScopeSet scopes;
} SurfaceInstall;
typedef struct {
    void *local_expand_user;
    IdmLocalExpandFn local_expand;
    void *free_identifier_eq_user;
    IdmFreeIdentifierEqFn free_identifier_eq;
    void *register_operator_user;
    IdmRegisterOperatorFn register_operator;
    void *register_macro_user;
    IdmRegisterMacroFn register_macro;
    void *expander_surface_user;
    IdmExpanderSurfaceFn expander_surface;
} SavedHooks;
typedef struct {
    size_t binding_count;
    size_t macro_count;
    size_t operator_count;
    size_t protocol_count;
    size_t method_surface_count;
    size_t decl_method_count;
    bool decl_resolver;
    size_t activation_count;
    size_t surface_install_count;
    size_t artifact_base_count;
} SurfaceCheckpoint;
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
    uint32_t global_seq;
    bool in_package;
    const char *package_name;
    struct { char *name; uint32_t global_id; } *exports;
    size_t export_count;
    size_t export_cap;
    IdmPkgGlobal *package_globals;
    size_t package_global_count;
    size_t package_global_cap;
    MacroDef *macros;
    size_t macro_count;
    size_t macro_cap;
    IdmOperatorDef *operators;
    size_t operator_count;
    size_t operator_cap;
    ProtocolDef *protocols;
    size_t protocol_count;
    size_t protocol_cap;
    MethodSurfaceDef *method_surfaces;
    size_t method_surface_count;
    size_t method_surface_cap;
    IdmProtocolMethodDef *decl_methods;
    size_t decl_method_count;
    size_t decl_method_cap;
    IdmMacroRunner *runner;
    IdmMacroRunner local_runner;
    IdmScopeStore scope_store;
    uint32_t macro_depth;
    int phase;
    bool value_context;
    bool command_sub_context;
    struct BodyDefCtx *def_ctx;
    const IdmScopeSet *op_fallback;
    bool repl_global_binds;
    struct { char *name; IdmSpan span; } *activations;
    size_t activation_count;
    size_t activation_cap;
    SurfaceInstall *surface_installs;
    size_t surface_install_count;
    size_t surface_install_cap;
    bool decl_resolver;
    IdmModuleRef *decl_resolver_module;
    uint32_t decl_resolver_fn;
    IdmNamespace *decl_resolver_phase_ns;
    IdmPhaseEnv *decl_resolver_phase_env;
    SavedFunctionContext *enclosing;
    IdmNamespace *phase_ns;
    IdmPhaseEnv *phase_env;
    const char *protocol_name;
    uint32_t *kernel_import_src;
    uint32_t *kernel_import_dst;
    size_t kernel_import_count;
    bool kernel_wrap;
    struct { const void *art; IdmScopeId base; bool init_emitted[2]; } *artifact_bases;
    size_t artifact_base_count;
    size_t artifact_base_cap;
    IdmArtifactDep *deps;
    size_t dep_count;
    size_t dep_cap;
    const char *unit;
    char unit_key[17];
    const IdmSyntax **pat_binders;
    size_t pat_binder_count;
    size_t pat_binder_cap;
    bool pat_binder_collect;
} ExpandContext;
typedef struct BodyDefCtx {
    IdmScopeSet use_site;
    struct BodyDefCtx *prev;
} BodyDefCtx;
typedef struct {
    ExpandContext *ctx;
    IdmSyntax *const *items;
    size_t end;
    size_t pos;
    IdmError *err;
} EnforestParser;
typedef struct {
    char *name;
    uint32_t arity;
    IdmCore *fn;
} ExtendMethodImpl;
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

bool activate_artifact(ExpandContext *ctx, const char *protocol_name, const IdmArtifact *art, IdmScopeId base, const IdmScopeSet *act_scopes, IdmSpan span, IdmError *err);
bool arg_push(ExpandContext *ctx, const IdmSyntax *word, uint32_t *out_slot);
bool arg_push_slot(ExpandContext *ctx, const IdmSyntax *word, uint32_t slot);
bool artifact_base(ExpandContext *ctx, const IdmArtifact *art, IdmScopeId *out_base, IdmError *err);
bool artifact_init_pending(ExpandContext *ctx, const IdmArtifact *art);
const IdmArtifact *artifact_get(ExpandContext *ctx, const char *path, IdmSpan span, IdmError *err);
void begin_clause_context(ExpandContext *ctx, SavedClauseContext *saved);
void begin_function_context(ExpandContext *ctx, SavedFunctionContext *saved);
bool capture_lookup_existing(const CaptureBinding *captures, size_t count, const IdmSyntax *word, uint32_t *out);
bool compile_package_module(ExpandContext *parent, const IdmSyntax *pkg, const char *protocol_name_hint, const unsigned char src_hash[32], IdmArtifact *out, IdmError *err);
bool ctx_activate_kernel(ExpandContext *ctx, IdmError *err);
void ctx_destroy(ExpandContext *ctx);
void ctx_init(ExpandContext *ctx, IdmRuntime *rt);
bool ctx_seed(ExpandContext *ctx, IdmError *err);
void end_clause_context(ExpandContext *ctx, SavedClauseContext *saved);
void end_function_context(ExpandContext *ctx, SavedFunctionContext *saved);
IdmCore *expand_body_items(ExpandContext *ctx, IdmSyntax *const *items, size_t index, size_t count, bool def_scope, IdmError *err);
IdmCore *expand_error(IdmError *err, IdmSpan span, const char *fmt, ...);
IdmCore *expand_extend_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);
IdmCore *expand_fn_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
IdmCore *expand_function_literal(ExpandContext *ctx, const char *debug_name, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmError *err);
IdmCore *expand_implement(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmSyntax *const *items, size_t index, size_t count, IdmSpan span, IdmError *err);
IdmCore *expand_implement_protocol(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);
IdmCore *expand_macro_use(ExpandContext *ctx, const IdmSyntax *use_syntax, const IdmSyntax *head, uint32_t payload, IdmError *err);
IdmCore *expand_macro_use_from_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, uint32_t payload, IdmError *err);
IdmCore *expand_method_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);
IdmCore *expand_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
IdmCore *expand_protocol_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);
IdmCore *expand_protocol_quasiquote(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
IdmCore *expand_protocol_quasisyntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
IdmCore *expand_protocol_quote(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
IdmCore *expand_protocol_string(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
IdmCore *expand_receive_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
IdmCore *expand_record_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);
IdmCore *expand_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
IdmCore *expand_use(ExpandContext *ctx, const char *path, const char *qualifier, IdmSyntax *const *items, size_t cont_index, size_t cont_count, IdmSpan span, IdmError *err);
bool free_identifier_eq_callback(void *user, IdmRuntime *rt, const IdmSyntax *a, const IdmSyntax *b, bool *out_equal, IdmError *err);
bool global_push_def_binder(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, uint32_t *out_id);
bool install_imported_macro(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, IdmModuleRef *module, uint32_t function_index, IdmNamespace *phase_ns, IdmPhaseEnv *phase_env, const char *provider, const char *provider_key, IdmError *err);
bool install_artifact_protocols(ExpandContext *ctx, const IdmPkgProtocol *protocols, size_t protocol_count, const IdmScopeSet *scopes, const char *qualifier, const char *provider, const char *provider_key, IdmError *err);
bool install_imported_operator(ExpandContext *ctx, const IdmOperatorDef *op, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, IdmError *err);
bool install_method_surface(ExpandContext *ctx, const char *protocol, const char *name, uint32_t arity, const IdmScopeSet *scopes, const char *provider, const char *provider_key, IdmError *err);
bool invoke_macro_to_syntax(ExpandContext *ctx, const IdmSyntax *use_syntax, const IdmSyntax *head, uint32_t payload, IdmSyntax **out_syntax, IdmError *err);
IdmCore *literal_from_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
bool local_expand_callback(void *user, IdmRuntime *rt, const IdmSyntax *syntax, IdmSyntax **out_syntax, IdmError *err);
IdmSyntax *make_qualified_word(IdmSyntax *const *items, size_t start, size_t *inout_end, IdmError *err);
bool local_macro_invoke(void *user, IdmRuntime *rt, uint32_t payload, const IdmSyntax *use_syntax, IdmSyntax **out_syntax, IdmError *err);
void local_pop_to(ExpandContext *ctx, size_t table_base, uint32_t next_slot);
bool local_push_def_binder(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, uint32_t *out_slot);
bool local_push_scoped(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, uint32_t *out_slot);
void macro_def_destroy(MacroDef *macro);
bool materialize_capture(ExpandContext *ctx, const IdmSyntax *word, const IdmBinding *b, uint32_t *out);
const IdmOperatorDef *op_lookup(const ExpandContext *ctx, const IdmSyntax *syn, bool want_prefix);
const char *package_path_text(const IdmSyntax *syn);
char *protocol_identity(ExpandContext *ctx, const char *name, const IdmSyntax *form, unsigned char out_hash[32], IdmError *err);
bool predeclare_protocol_methods(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, IdmError *err);
void protocol_def_destroy(ProtocolDef *protocol);
bool record_export(ExpandContext *ctx, const char *name, uint32_t global_id);
bool record_package_global(ExpandContext *ctx, const char *name, uint32_t global_id, const IdmScopeSet *scopes);
bool register_macro(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmCore *fn, IdmSpan span, bool exported, IdmError *err);
bool register_macro_callback(void *user, IdmRuntime *rt, const IdmSyntax *name_syntax, IdmValue transformer, IdmError *err);
bool register_operator(ExpandContext *ctx, const char *name, uint8_t precedence, IdmOpAssoc assoc, IdmOpFixity fixity, IdmOpTargetKind target_kind, IdmPrimitive primitive, const char *target_name, const IdmScopeSet *scopes, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, bool exported, IdmError *err);
bool register_operator_callback(void *user, IdmRuntime *rt, const IdmSyntax *name_syntax, int64_t precedence, const char *assoc_text, const char *fixity_text, const IdmSyntax *target_syntax, IdmError *err);
bool register_resolver(ExpandContext *ctx, IdmCore *fn, IdmSpan span, IdmError *err);
IdmBytecodeModule *relocated_module_copy(ExpandContext *ctx, const IdmBytecodeModule *src, IdmScopeId min_id, int64_t delta, uint32_t *out_fn_off, IdmError *err);
const IdmBinding *resolve_default(const ExpandContext *ctx, const IdmSyntax *word, IdmResolveStatus *out_status);
bool resolve_head_resolver(ExpandContext *ctx, const IdmSyntax *head, uint32_t *out_payload, IdmError *err);
const MethodSurfaceDef *resolve_method_surface(ExpandContext *ctx, const IdmSyntax *word, IdmResolveStatus *out_status);
ProtocolDef *resolve_protocol_def(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmResolveStatus *out_status);
bool resolve_transformer(ExpandContext *ctx, const IdmSyntax *head, uint32_t *out_payload, IdmError *err);
bool run_phase_core(ExpandContext *ctx, IdmCore *core, IdmError *err);
void surface_checkpoint(ExpandContext *ctx, SurfaceCheckpoint *checkpoint);
void surface_rollback(ExpandContext *ctx, const SurfaceCheckpoint *checkpoint);
bool syn_is_protocol(const IdmSyntax *syn, const char *head);
bool syn_is_word(const IdmSyntax *syn, const char *word);
bool syntax_scopes_copy(IdmScopeSet *dst, const IdmSyntax *syn);
bool value_from_literal_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmValue *out, IdmError *err);
bool expand_quote_datum(ExpandContext *ctx, const IdmSyntax *t, IdmValue *out, IdmError *err);
IdmCore *wrap_kernel_use(ExpandContext *ctx, IdmCore *body, IdmError *err);

bool expander_surface_callback(void *user, IdmRuntime *rt, const char *kind, IdmValue *out, IdmError *err);
void hooks_install(IdmRuntime *rt, ExpandContext *ctx, SavedHooks *saved);
void hooks_restore(IdmRuntime *rt, const SavedHooks *saved);
void ctx_set_unit(ExpandContext *ctx, const char *name, const unsigned char hash[32]);
bool record_activation(ExpandContext *ctx, const char *name, IdmSpan span, IdmError *err);
int surface_install_guard(ExpandContext *ctx, const char *provider, const char *provider_key, const char *key, const char *display, IdmBindingSpace space, const IdmScopeSet *scopes, IdmError *err);
void artifact_provider_key(const unsigned char hash[32], char out[17]);
char *operator_binding_key(const char *name, IdmOpFixity fixity);
bool scopes_subset_for_ref(const IdmScopeSet *binding_scopes, const IdmSyntax *ref);
bool binder_scopes_pruned(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmScopeSet *out);

#endif
