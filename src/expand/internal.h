#ifndef ISH_EXPAND_INTERNAL_H
#define ISH_EXPAND_INTERNAL_H

#include "ish/expand.h"
#include "ish/artifact.h"
#include "ish/prims.h"
#include "ish/vm.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define ISH_FRAME_GLOBAL 0u
#define ISH_FRAME_TOP 1u

#define ISH_FRAME_GLOBAL 0u
#define ISH_FRAME_TOP 1u

typedef struct {
    char *name;
    IshScopeSet scopes;
    IshCaptureKind kind;
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
    const IshSyntax *name_syntax;
    IshScopeSet scopes;
    uint32_t slot;
    size_t *indices;
    size_t count;
    size_t cap;
    bool exported;
} DefnGroup;
typedef struct {
    char *name;
    IshModuleRef *module;
    uint32_t function_index;
    IshNamespace *phase_ns;
    IshPhaseEnv *phase_env;
    bool exported;
    bool closure_backed;
    IshValue transformer;
    IshRuntime *rt;
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
    IshScopeSet scopes;
} MethodSurfaceDef;
typedef struct {
    char *name;
    IshOperatorDef *operators;
    size_t operator_count;
    IshPkgMacro *macros;
    size_t macro_count;
    IshModuleRef *resolver_module;
    uint32_t resolver_fn;
    IshNamespace *resolver_phase_ns;
    IshPhaseEnv *resolver_phase_env;
    IshProtocolMethodDef *methods;
    size_t method_count;
} ProtocolDef;
typedef struct {
    size_t binding_count;
    size_t macro_count;
    size_t operator_count;
    size_t protocol_count;
    size_t method_surface_count;
    size_t decl_method_count;
    bool decl_resolver;
    size_t activation_count;
} SurfaceCheckpoint;
typedef struct {
    IshRuntime *rt;
    IshBindingTable bindings;
    IshScopeSet empty_scopes;
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
    IshPkgGlobal *package_globals;
    size_t package_global_count;
    size_t package_global_cap;
    MacroDef *macros;
    size_t macro_count;
    size_t macro_cap;
    IshOperatorDef *operators;
    size_t operator_count;
    size_t operator_cap;
    ProtocolDef *protocols;
    size_t protocol_count;
    size_t protocol_cap;
    MethodSurfaceDef *method_surfaces;
    size_t method_surface_count;
    size_t method_surface_cap;
    IshProtocolMethodDef *decl_methods;
    size_t decl_method_count;
    size_t decl_method_cap;
    IshMacroRunner *runner;
    IshMacroRunner local_runner;
    IshScopeStore scope_store;
    uint32_t macro_depth;
    bool value_context;
    bool command_sub_context;
    struct BodyDefCtx *def_ctx;
    struct { char *name; IshSpan span; } *activations;
    size_t activation_count;
    size_t activation_cap;
    bool decl_resolver;
    IshModuleRef *decl_resolver_module;
    uint32_t decl_resolver_fn;
    IshNamespace *decl_resolver_phase_ns;
    IshPhaseEnv *decl_resolver_phase_env;
    SavedFunctionContext *enclosing;
    IshNamespace *phase_ns;
    IshPhaseEnv *phase_env;
    const char *protocol_name;
    uint32_t *kernel_import_src;
    uint32_t *kernel_import_dst;
    size_t kernel_import_count;
    bool kernel_wrap;
    struct { const void *art; IshScopeId base; } *artifact_bases;
    size_t artifact_base_count;
    size_t artifact_base_cap;
    IshArtifactDep *deps;
    size_t dep_count;
    size_t dep_cap;
} ExpandContext;
typedef struct BodyDefCtx {
    IshScopeSet use_site;
    struct BodyDefCtx *prev;
} BodyDefCtx;
typedef struct {
    ExpandContext *ctx;
    IshSyntax *const *items;
    size_t end;
    size_t pos;
    IshError *err;
} EnforestParser;
typedef struct {
    char *name;
    uint32_t arity;
    IshCore *fn;
} ExtendMethodImpl;
typedef struct {
    char *path;
    IshArtifact art;
} CachedPackage;
typedef struct {
    IshRuntime *rt;
    IshArtifact kernel;
    char *kernel_path;
    IshScopeId kernel_scope_end;
    bool kernel_ready;
    CachedPackage **pkgs;
    size_t pkg_count;
    size_t pkg_cap;
    const char **compiling;
    size_t compiling_count;
    size_t compiling_cap;
} ExpandCache;
typedef enum { BODY_REC_BIND, BODY_REC_EXPR, BODY_REC_GROUPS } BodyRecKind;
typedef struct {
    BodyRecKind kind;
    const IshSyntax *form;
    const IshSyntax *bind_name;
    size_t rhs_start;
    uint32_t bind_slot;
    DefnGroup *groups;
    size_t group_count;
    IshCore *core;
} BodyRec;

bool activate_artifact(ExpandContext *ctx, const char *protocol_name, const IshArtifact *art, IshScopeId base, IshSpan span, IshError *err);
bool arg_push(ExpandContext *ctx, const IshSyntax *word, uint32_t *out_slot);
bool arg_push_slot(ExpandContext *ctx, const IshSyntax *word, uint32_t slot);
bool artifact_base(ExpandContext *ctx, const IshArtifact *art, IshScopeId *out_base, IshError *err);
const IshArtifact *artifact_get(ExpandContext *ctx, const char *path, IshSpan span, IshError *err);
void begin_clause_context(ExpandContext *ctx, SavedClauseContext *saved);
void begin_function_context(ExpandContext *ctx, SavedFunctionContext *saved);
bool capture_lookup_existing(const CaptureBinding *captures, size_t count, const IshSyntax *word, uint32_t *out);
bool compile_package_module(ExpandContext *parent, const IshSyntax *pkg, const char *protocol_name_hint, IshArtifact *out, IshError *err);
bool ctx_activate_kernel(ExpandContext *ctx, IshError *err);
void ctx_destroy(ExpandContext *ctx);
void ctx_init(ExpandContext *ctx, IshRuntime *rt);
bool ctx_seed(ExpandContext *ctx, IshError *err);
void end_clause_context(ExpandContext *ctx, SavedClauseContext *saved);
void end_function_context(ExpandContext *ctx, SavedFunctionContext *saved);
IshCore *expand_body_items(ExpandContext *ctx, IshSyntax *const *items, size_t index, size_t count, IshError *err);
IshCore *expand_error(IshError *err, IshSpan span, const char *fmt, ...);
IshCore *expand_extend_decl(ExpandContext *ctx, const IshSyntax *form, IshSyntax *const *items, size_t index, size_t count, IshError *err);
IshCore *expand_fn_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err);
IshCore *expand_function_literal(ExpandContext *ctx, const char *debug_name, const IshSyntax *head, IshSyntax *const *items, size_t param_start, size_t end, IshError *err);
IshCore *expand_implements(ExpandContext *ctx, const IshSyntax *name_syntax, IshSyntax *const *items, size_t index, size_t count, IshSpan span, IshError *err);
IshCore *expand_macro_use(ExpandContext *ctx, const IshSyntax *use_syntax, const IshSyntax *head, uint32_t payload, IshError *err);
IshCore *expand_macro_use_from_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, uint32_t payload, IshError *err);
IshCore *expand_method_decl(ExpandContext *ctx, const IshSyntax *form, IshSyntax *const *items, size_t index, size_t count, IshError *err);
IshCore *expand_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err);
IshCore *expand_protocol_decl(ExpandContext *ctx, const IshSyntax *form, IshSyntax *const *items, size_t index, size_t count, IshError *err);
IshCore *expand_protocol_quasiquote(ExpandContext *ctx, const IshSyntax *syn, IshError *err);
IshCore *expand_protocol_quasisyntax(ExpandContext *ctx, const IshSyntax *syn, IshError *err);
IshCore *expand_protocol_quote(ExpandContext *ctx, const IshSyntax *syn, IshError *err);
IshCore *expand_protocol_string(ExpandContext *ctx, const IshSyntax *syn, IshError *err);
IshCore *expand_receive_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err);
IshCore *expand_record_decl(ExpandContext *ctx, const IshSyntax *form, IshSyntax *const *items, size_t index, size_t count, IshError *err);
IshCore *expand_syntax(ExpandContext *ctx, const IshSyntax *syn, IshError *err);
IshCore *expand_use(ExpandContext *ctx, const char *path, const char *qualifier, IshSyntax *const *items, size_t cont_index, size_t cont_count, IshSpan span, IshError *err);
bool free_identifier_eq_callback(void *user, IshRuntime *rt, const IshSyntax *a, const IshSyntax *b, bool *out_equal, IshError *err);
bool global_push(ExpandContext *ctx, const char *name, const IshSyntax *name_syntax, uint32_t *out_id);
bool global_push_def_binder(ExpandContext *ctx, const char *name, const IshSyntax *name_syntax, uint32_t *out_id);
bool install_imported_macro(ExpandContext *ctx, const char *name, const IshScopeSet *scopes, IshModuleRef *module, uint32_t function_index, IshNamespace *phase_ns, IshPhaseEnv *phase_env, IshError *err);
bool install_imported_operator(ExpandContext *ctx, const IshOperatorDef *op, IshError *err);
bool install_method_surface(ExpandContext *ctx, const char *protocol, const char *name, uint32_t arity, const IshScopeSet *scopes, IshError *err);
bool invoke_macro_to_syntax(ExpandContext *ctx, const IshSyntax *use_syntax, const IshSyntax *head, uint32_t payload, IshSyntax **out_syntax, IshError *err);
IshCore *literal_from_syntax(ExpandContext *ctx, const IshSyntax *syn, IshError *err);
bool local_expand_callback(void *user, IshRuntime *rt, const IshSyntax *syntax, IshSyntax **out_syntax, IshError *err);
bool local_macro_invoke(void *user, IshRuntime *rt, uint32_t payload, const IshSyntax *use_syntax, IshSyntax **out_syntax, IshError *err);
void local_pop_to(ExpandContext *ctx, size_t table_base, uint32_t next_slot);
bool local_push_def_binder(ExpandContext *ctx, const char *name, const IshSyntax *name_syntax, uint32_t *out_slot);
bool local_push_scoped(ExpandContext *ctx, const char *name, const IshSyntax *name_syntax, uint32_t *out_slot);
void macro_def_destroy(MacroDef *macro);
bool materialize_capture(ExpandContext *ctx, const IshSyntax *word, const IshBinding *b, uint32_t *out);
const IshOperatorDef *op_lookup(const ExpandContext *ctx, const IshSyntax *syn, bool want_prefix);
const char *package_path_text(const IshSyntax *syn);
bool predeclare_protocol_methods(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t count, IshError *err);
void protocol_def_destroy(ProtocolDef *protocol);
bool record_export(ExpandContext *ctx, const char *name, uint32_t global_id);
bool record_package_global(ExpandContext *ctx, const char *name, uint32_t global_id, const IshScopeSet *scopes);
bool register_macro(ExpandContext *ctx, const IshSyntax *name_syntax, IshCore *fn, IshSpan span, bool exported, IshError *err);
bool register_macro_callback(void *user, IshRuntime *rt, const IshSyntax *name_syntax, IshValue transformer, IshError *err);
bool register_operator(ExpandContext *ctx, const char *name, uint8_t precedence, IshOpAssoc assoc, IshOpFixity fixity, IshOpTargetKind target_kind, IshPrimitive primitive, const char *target_name, const IshScopeSet *scopes, IshError *err);
bool register_operator_callback(void *user, IshRuntime *rt, const IshSyntax *name_syntax, int64_t precedence, const char *assoc_text, const char *fixity_text, const IshSyntax *target_syntax, IshError *err);
bool register_resolver(ExpandContext *ctx, IshCore *fn, IshSpan span, IshError *err);
IshBytecodeModule *relocated_module_copy(ExpandContext *ctx, const IshBytecodeModule *src, IshScopeId min_id, int64_t delta, uint32_t *out_fn_off, IshError *err);
const IshBinding *resolve_default(const ExpandContext *ctx, const IshSyntax *word, IshResolveStatus *out_status);
bool resolve_head_resolver(ExpandContext *ctx, const IshSyntax *head, uint32_t *out_payload, IshError *err);
const MethodSurfaceDef *resolve_method_surface(ExpandContext *ctx, const IshSyntax *word, IshResolveStatus *out_status);
ProtocolDef *resolve_protocol_def(ExpandContext *ctx, const IshSyntax *name_syntax, IshResolveStatus *out_status);
bool resolve_transformer(ExpandContext *ctx, const IshSyntax *head, uint32_t *out_payload, IshError *err);
bool run_phase_core(ExpandContext *ctx, IshCore *core, IshError *err);
void surface_checkpoint(ExpandContext *ctx, SurfaceCheckpoint *checkpoint);
void surface_rollback(ExpandContext *ctx, const SurfaceCheckpoint *checkpoint);
bool syn_is_protocol(const IshSyntax *syn, const char *head);
bool syn_is_word(const IshSyntax *syn, const char *word);
bool syntax_scopes_copy(IshScopeSet *dst, const IshSyntax *syn);
bool value_from_literal_syntax(ExpandContext *ctx, const IshSyntax *syn, IshValue *out, IshError *err);
IshCore *wrap_kernel_use(ExpandContext *ctx, IshCore *body, IshError *err);

bool expander_surface_callback(void *user, IshRuntime *rt, const char *kind, IshValue *out, IshError *err);
bool record_activation(ExpandContext *ctx, const char *name, IshSpan span, IshError *err);
bool scopes_subset_for_ref(const IshScopeSet *binding_scopes, const IshSyntax *ref);

#endif
