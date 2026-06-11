#ifndef ISH_ARTIFACT_H
#define ISH_ARTIFACT_H

#include "ish/bytecode.h"
#include "ish/prims.h"
#include "ish/scope.h"
#include "ish/value.h"

typedef struct {
    IshBytecodeModule module;
    size_t refs;
    IshRuntime *rt;
} IshModuleRef;

typedef struct {
    IshRuntime *rt;
    IshNamespace *ns;
    IshBytecodeModule **modules;
    uint32_t *module_main_fns;
    size_t module_count;
    size_t module_cap;
    size_t refs;
} IshPhaseEnv;

typedef enum { ISH_OP_ASSOC_LEFT, ISH_OP_ASSOC_RIGHT, ISH_OP_ASSOC_NONE } IshOpAssoc;
typedef enum { ISH_OP_FIX_INFIX, ISH_OP_FIX_PREFIX, ISH_OP_FIX_POSTFIX } IshOpFixity;
typedef enum { ISH_OP_TGT_PRIMITIVE, ISH_OP_TGT_THREAD_FIRST, ISH_OP_TGT_THREAD_LAST, ISH_OP_TGT_NAMED } IshOpTargetKind;

typedef struct {
    char *name;
    uint8_t precedence;
    IshOpAssoc assoc;
    IshOpFixity fixity;
    IshOpTargetKind target_kind;
    IshPrimitive primitive;
    char *target_name;
    IshScopeSet scopes;
    bool exported;
} IshOperatorDef;

typedef struct IshCore IshCore;

typedef struct {
    char *name;
    uint32_t arity;
    bool has_default;
    bool seen_decl;
    IshCore *default_fn;
    IshScopeSet scopes;
    bool exported;
} IshProtocolMethodDef;

typedef struct {
    char *name;
    IshModuleRef *module;
    uint32_t function_index;
    IshNamespace *phase_ns;
    IshPhaseEnv *phase_env;
} IshPkgMacro;

typedef struct {
    char *name;
    uint32_t slot;
    IshScopeSet scopes;
} IshPkgGlobal;

typedef struct {
    char *name;
    uint32_t slot;
} IshPkgExport;

typedef struct {
    char *path;
    unsigned char hash[32];
} IshArtifactDep;

typedef struct {
    IshBytecodeModule *module;
    uint32_t init_fn;
    char *name;
    IshPkgExport *exports;
    size_t export_count;
    IshPkgGlobal *globals;
    size_t global_count;
    IshPkgMacro *macros;
    size_t macro_count;
    IshOperatorDef *operators;
    size_t operator_count;
    IshModuleRef *resolver_module;
    uint32_t resolver_fn;
    IshNamespace *resolver_phase_ns;
    IshPhaseEnv *resolver_phase_env;
    IshProtocolMethodDef *methods;
    size_t method_count;
    IshScopeId scope_base;
    IshScopeId scope_end;
    IshPhaseEnv *phase_env;
    unsigned char src_hash[32];
    IshArtifactDep *deps;
    size_t dep_count;
} IshArtifact;

IshModuleRef *ish_module_ref_create(IshRuntime *rt);
IshModuleRef *ish_module_ref_retain(IshModuleRef *ref);
void ish_module_ref_release(IshModuleRef *ref);

IshNamespace *ish_fresh_phase_namespace(IshRuntime *rt, IshError *err);
IshPhaseEnv *ish_phase_env_create(IshRuntime *rt, IshNamespace *ns);
IshPhaseEnv *ish_phase_env_retain(IshPhaseEnv *env);
void ish_phase_env_release(IshPhaseEnv *env);
bool ish_phase_env_add_module(IshPhaseEnv *env, IshBytecodeModule *module, uint32_t main_fn);

void ish_operator_def_destroy(IshOperatorDef *op);
void ish_protocol_method_def_destroy(IshProtocolMethodDef *method);
void ish_pkg_macro_destroy(IshPkgMacro *macro);
void ish_pkg_global_destroy(IshPkgGlobal *global);
void ish_artifact_destroy(IshArtifact *art);

bool ish_package_read_source(IshRuntime *rt, const char *path, IshBuffer *out_src, const char **out_label, IshSpan span, IshError *err);

bool ish_artifact_serialize(const IshArtifact *art, IshBuffer *out, IshError *err);
bool ish_artifact_deserialize(IshRuntime *rt, const unsigned char *data, size_t len, IshArtifact *out, IshError *err);

bool ish_artifact_cache_disabled(void);
bool ish_artifact_cache_load(IshRuntime *rt, const char *path, const unsigned char src_hash[32], IshArtifact *out);
void ish_artifact_cache_write(const char *path, const IshArtifact *art);
bool ish_artifact_path_verified(IshRuntime *rt, const char *path, const unsigned char want[32]);

#endif
