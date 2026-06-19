#ifndef IDM_ARTIFACT_H
#define IDM_ARTIFACT_H

#include "idiom/bytecode.h"
#include "idiom/prims.h"
#include "idiom/scope.h"
#include "idiom/value.h"

typedef struct {
    IdmBytecodeModule module;
    size_t refs;
    IdmRuntime *rt;
} IdmModuleRef;

typedef struct {
    IdmRuntime *rt;
    IdmNamespace *ns;
    IdmBytecodeModule **modules;
    uint32_t *module_main_fns;
    size_t module_count;
    size_t module_cap;
    size_t refs;
} IdmPhaseEnv;

typedef enum { IDM_OP_ASSOC_LEFT, IDM_OP_ASSOC_RIGHT, IDM_OP_ASSOC_NONE } IdmOpAssoc;

typedef struct {
    char *name;
    char *capture;
    uint8_t precedence;
    IdmOpAssoc assoc;
    char *target_name;
    IdmModuleRef *target_module;
    uint32_t target_function_index;
    IdmNamespace *target_phase_ns;
    IdmPhaseEnv *target_phase_env;
    IdmScopeSet scopes;
    bool exported;
} IdmOperatorDef;

typedef struct IdmCore IdmCore;

typedef struct {
    char *name;
    IdmArity arity;
    bool has_default;
    bool seen_decl;
    IdmCore *default_fn;
    IdmScopeSet scopes;
    bool exported;
} IdmTraitMethodDef;

typedef struct {
    char *name;
} IdmTraitRequirementDef;

typedef struct {
    char *name;
    IdmModuleRef *module;
    uint32_t function_index;
    IdmNamespace *phase_ns;
    IdmPhaseEnv *phase_env;
} IdmPkgMacro;

typedef struct {
    char *name;
    uint32_t slot;
    IdmScopeSet scopes;
    IdmArity arity;
} IdmPkgGlobal;

typedef struct {
    char *name;
    uint32_t slot;
    IdmArity arity;
} IdmPkgExport;

typedef enum {
    IDM_DEP_PACKAGE,
    IDM_DEP_FILE_HASH,
    IDM_DEP_FILE_PRESENT,
    IDM_DEP_FILE_ABSENT
} IdmArtifactDepKind;

typedef struct {
    char *path;
    unsigned char hash[32];
    uint8_t kind;
} IdmArtifactDep;

struct IdmPhaseReads {
    IdmArtifactDep *items;
    size_t count;
    size_t cap;
    bool failed;
};

typedef struct {
    char *name;
    char *identity;
    IdmTraitRequirementDef *requirements;
    size_t requirement_count;
    IdmTraitMethodDef *methods;
    size_t method_count;
} IdmPkgTrait;

typedef struct {
    char *name;
    char *identity;
    IdmScopeSet scopes;
    char **fields;
    size_t field_count;
} IdmPkgType;

typedef struct {
    IdmBytecodeModule *module;
    uint32_t init_fn;
    char *name;
    IdmPkgExport *exports;
    size_t export_count;
    IdmPkgGlobal *globals;
    size_t global_count;
    IdmPkgMacro *macros;
    size_t macro_count;
    IdmOperatorDef *operators;
    size_t operator_count;
    IdmModuleRef *resolver_module;
    uint32_t resolver_fn;
    IdmNamespace *resolver_phase_ns;
    IdmPhaseEnv *resolver_phase_env;
    IdmPkgType *types;
    size_t type_count;
    IdmPkgTrait *traits;
    size_t trait_count;
    IdmScopeId scope_base;
    IdmScopeId scope_end;
    IdmPhaseEnv *phase_env;
    unsigned char src_hash[32];
    IdmArtifactDep *deps;
    size_t dep_count;
} IdmArtifact;

IdmModuleRef *idm_module_ref_create(IdmRuntime *rt);
IdmModuleRef *idm_module_ref_retain(IdmModuleRef *ref);
void idm_module_ref_release(IdmModuleRef *ref);

IdmNamespace *idm_fresh_phase_namespace(IdmRuntime *rt, IdmError *err);
IdmPhaseEnv *idm_phase_env_create(IdmRuntime *rt, IdmNamespace *ns);
IdmPhaseEnv *idm_phase_env_retain(IdmPhaseEnv *env);
void idm_phase_env_release(IdmPhaseEnv *env);
bool idm_phase_env_add_module(IdmPhaseEnv *env, IdmBytecodeModule *module, uint32_t main_fn, IdmError *err);

void idm_operator_def_destroy(IdmOperatorDef *op);
void idm_trait_method_def_destroy(IdmTraitMethodDef *method);
void idm_trait_requirement_def_destroy(IdmTraitRequirementDef *requirement);
void idm_pkg_macro_destroy(IdmPkgMacro *macro);
void idm_pkg_global_destroy(IdmPkgGlobal *global);
void idm_pkg_trait_destroy(IdmPkgTrait *trait);
void idm_pkg_type_destroy(IdmPkgType *type);
bool idm_trait_method_defs_copy(const IdmTraitMethodDef *src, size_t count, IdmTraitMethodDef **out);
bool idm_trait_requirement_defs_copy(const IdmTraitRequirementDef *src, size_t count, IdmTraitRequirementDef **out);
void idm_artifact_destroy(IdmArtifact *art);

bool idm_package_read_source(IdmRuntime *rt, const char *path, IdmBuffer *out_src, const char **out_label, IdmSpan span, IdmError *err);

bool idm_artifact_serialize(const IdmArtifact *art, IdmBuffer *out, IdmError *err);
bool idm_artifact_deserialize(IdmRuntime *rt, const unsigned char *data, size_t len, IdmArtifact *out, IdmError *err);

bool idm_artifact_cache_disabled(void);
bool idm_artifact_cache_load(IdmRuntime *rt, const char *path, const unsigned char src_hash[32], IdmArtifact *out);
void idm_artifact_cache_write(const char *path, const IdmArtifact *art);
bool idm_artifact_path_verified(IdmRuntime *rt, const char *path, const unsigned char want[32]);
bool idm_artifact_dep_verified(IdmRuntime *rt, const IdmArtifactDep *dep);
void idm_phase_reads_destroy(IdmPhaseReads *reads);

#endif
