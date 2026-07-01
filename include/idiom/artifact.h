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

typedef struct IdmArtifact IdmArtifact;

typedef struct {
    IdmRuntime *rt;
    IdmEnv *env;
    IdmBytecodeModule **modules;
    uint32_t *module_main_fns;
    size_t module_count;
    size_t module_cap;
    size_t refs;
} IdmPhaseEnv;

typedef enum { IDM_OP_ASSOC_LEFT, IDM_OP_ASSOC_RIGHT, IDM_OP_ASSOC_NONE } IdmOpAssoc;

typedef enum {
    IDM_OPERATOR_CAPTURE_INVALID,
    IDM_OPERATOR_CAPTURE_PREFIX,
    IDM_OPERATOR_CAPTURE_INFIX,
    IDM_OPERATOR_CAPTURE_POSTFIX,
    IDM_OPERATOR_CAPTURE_INDENTED,
    IDM_OPERATOR_CAPTURE_SENTINEL,
    IDM_OPERATOR_CAPTURE_COUNT,
    IDM_OPERATOR_CAPTURE_EXPRESSION
} IdmOperatorCaptureKind;

typedef struct {
    char *name;
    char *capture;
    uint8_t capture_kind;
    bool capture_left;
    uint32_t capture_count;
    uint8_t precedence;
    IdmOpAssoc assoc;
    char *target_name;
    IdmModuleRef *target_module;
    uint32_t target_function_index;
    IdmPhaseEnv *target_phase_env;
    IdmScopeSet scopes;
    bool exported;
} IdmOperatorDef;

typedef struct IdmCore IdmCore;

typedef struct {
    char *name;
    IdmArity arity;
    bool has_contract;
    IdmCallableContract contract;
    bool has_default;
    bool seen_decl;
    IdmCore *default_fn;
    IdmScopeSet scopes;
    bool exported;
    uint32_t default_slot;
    bool has_default_slot;
    bool has_dispatch;
    uint32_t dispatch_slot;
} IdmTraitMethodDef;

typedef struct {
    char *name;
} IdmTraitRequirementDef;

typedef struct {
    char *name;
    IdmModuleRef *module;
    uint32_t function_index;
    IdmPhaseEnv *phase_env;
} IdmPkgMacro;

typedef struct {
    char *name;
    IdmModuleRef *module;
    uint32_t function_index;
    IdmPhaseEnv *phase_env;
} IdmPkgCoreSyntax;

typedef enum {
    IDM_GRAMMAR_MODE_EXTEND,
    IDM_GRAMMAR_MODE_EXCLUSIVE
} IdmGrammarMode;

typedef enum {
    IDM_GRAMMAR_RULE_TOKEN,
    IDM_GRAMMAR_RULE_FORM,
    IDM_GRAMMAR_RULE_SKIP
} IdmGrammarRuleKind;

typedef enum {
    IDM_GRAMMAR_TERMINAL_NONE,
    IDM_GRAMMAR_TERMINAL_REGEX,
    IDM_GRAMMAR_TERMINAL_LITERAL,
    IDM_GRAMMAR_TERMINAL_STRING
} IdmGrammarTerminalKind;

typedef struct {
    uint8_t kind;
    char *text;
    uint32_t flags;
} IdmGrammarTerminal;

typedef enum {
    IDM_READER_PATTERN_EMPTY,
    IDM_READER_PATTERN_REF,
    IDM_READER_PATTERN_TOKEN,
    IDM_READER_PATTERN_LITERAL,
    IDM_READER_PATTERN_SEQ,
    IDM_READER_PATTERN_ALT,
    IDM_READER_PATTERN_REPEAT,
    IDM_READER_PATTERN_OPTIONAL,
    IDM_READER_PATTERN_CAPTURE,
    IDM_READER_PATTERN_INDENT_GT,
    IDM_READER_PATTERN_INDENT_EQ,
    IDM_READER_PATTERN_ADJACENT,
    IDM_READER_PATTERN_NOT_ADJACENT,
    IDM_READER_PATTERN_PEEK
} IdmReaderPatternOp;

typedef enum {
    IDM_READER_PATTERN_TARGET_NONE,
    IDM_READER_PATTERN_TARGET_TOKEN,
    IDM_READER_PATTERN_TARGET_FORM
} IdmReaderPatternTarget;

typedef struct {
    uint8_t op;
    char *text;
    int64_t integer;
    bool has_literal;
    size_t literal_index;
    IdmSyntax *literal;
    size_t child_count;
    uint8_t target_kind;
    size_t target_index;
    size_t capture_slot;
} IdmReaderInst;

typedef struct {
    IdmReaderInst *items;
    size_t count;
    size_t cap;
} IdmReaderProgram;

typedef enum {
    IDM_READER_CTOR_CAPTURE,
    IDM_READER_CTOR_SPLICE,
    IDM_READER_CTOR_CAPTURE_ATOM,
    IDM_READER_CTOR_CAPTURE_WORD,
    IDM_READER_CTOR_CAPTURE_STRING,
    IDM_READER_CTOR_LITERAL,
    IDM_READER_CTOR_EMIT_ATOM,
    IDM_READER_CTOR_EMIT_WORD,
    IDM_READER_CTOR_EMIT_STRING,
    IDM_READER_CTOR_EMIT_INT,
    IDM_READER_CTOR_INTERP_STRING,
    IDM_READER_CTOR_FORM,
    IDM_READER_CTOR_LIST,
    IDM_READER_CTOR_VECTOR,
    IDM_READER_CTOR_TUPLE,
    IDM_READER_CTOR_DICT
} IdmReaderCtorOp;

typedef enum {
    IDM_READER_PROGRAM_PATTERN,
    IDM_READER_PROGRAM_CTOR
} IdmReaderProgramKind;

typedef struct {
    char *name;
    uint8_t kind;
    IdmGrammarTerminal terminal;
    IdmReaderProgram pattern;
    IdmReaderProgram constructor;
} IdmGrammarRule;

typedef struct IdmPkgGrammar {
    char *name;
    uint8_t mode;
    IdmGrammarRule *rules;
    size_t rule_count;
    IdmScopeSet scopes;
    bool exported;
} IdmPkgGrammar;

typedef struct {
    char *name;
    uint32_t slot;
    IdmScopeSet scopes;
    IdmArity arity;
    bool has_contract;
    IdmCallableContract contract;
    bool exported;
} IdmPkgSlot;


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
    char *trait;
    char *type;
    char *method;
    IdmArity arity;
    bool impl_env;
    char *impl_env_key;
    uint32_t impl_slot;
} IdmPkgMethodImpl;

typedef struct {
    char *name;
    bool has_contract;
    IdmTypeTerm contract;
} IdmPkgTypeField;

typedef struct {
    IdmTypeTerm term;
} IdmPkgTypeMember;

typedef struct {
    char *name;
    char *identity;
    IdmScopeSet scopes;
    IdmPkgTypeMember *members;
    size_t member_count;
    IdmPkgTypeField *fields;
    size_t field_count;
} IdmPkgType;

typedef struct {
    char *name;
    char *identity;
    IdmArtifact *art;
} IdmPkgProtocol;

typedef enum {
    IDM_TYPED_ENTITY_PROTOCOL,
    IDM_TYPED_ENTITY_TRAIT,
    IDM_TYPED_ENTITY_TYPE
} IdmTypedEntityKind;

typedef struct {
    IdmTypedEntityKind kind;
    union {
        IdmPkgProtocol protocol;
        IdmPkgTrait trait;
        IdmPkgType type;
    } as;
} IdmPkgTypedEntity;

typedef struct {
    IdmPkgTypedEntity *entities;
    size_t entity_count;
    IdmPkgMethodImpl *method_impls;
    size_t method_impl_count;
} IdmArtifactTypedRegistry;

struct IdmArtifact {
    IdmBytecodeModule *module;
    uint32_t init_fn;
    char *name;
    IdmPkgSlot *slots;
    size_t slot_count;
    struct { char *name; char *env_key; uint32_t slot; } *field_selectors;
    size_t field_selector_count;
    IdmPkgMacro *macros;
    size_t macro_count;
    IdmOperatorDef *operators;
    size_t operator_count;
    IdmPkgCoreSyntax *core_syntax;
    size_t core_syntax_count;
    IdmPkgGrammar *grammars;
    size_t grammar_count;
    char *source_reader;
    IdmArtifactTypedRegistry typed;
    IdmScopeId scope_base;
    IdmScopeId scope_end;
    IdmPhaseEnv *phase_env;
    unsigned char src_hash[32];
    IdmArtifactDep *deps;
    size_t dep_count;
};

IdmModuleRef *idm_module_ref_create(IdmRuntime *rt);
IdmModuleRef *idm_module_ref_retain(IdmModuleRef *ref);
void idm_module_ref_release(IdmModuleRef *ref);

IdmEnv *idm_fresh_phase_runtime_env(IdmRuntime *rt, IdmError *err);
IdmPhaseEnv *idm_phase_env_create(IdmRuntime *rt, IdmEnv *storage);
IdmPhaseEnv *idm_phase_env_retain(IdmPhaseEnv *env);
void idm_phase_env_release(IdmPhaseEnv *env);
bool idm_phase_env_add_module(IdmPhaseEnv *env, IdmBytecodeModule *module, uint32_t main_fn, IdmError *err);
bool idm_artifact_run_phase_inits(IdmRuntime *rt, const IdmArtifact *art, IdmError *err);

void idm_operator_def_destroy(IdmOperatorDef *op);
void idm_trait_method_def_destroy(IdmTraitMethodDef *method);
void idm_trait_requirement_def_destroy(IdmTraitRequirementDef *requirement);
void idm_pkg_macro_destroy(IdmPkgMacro *macro);
void idm_pkg_core_syntax_destroy(IdmPkgCoreSyntax *core_syntax);
void idm_pkg_method_impl_destroy(IdmPkgMethodImpl *impl);
void idm_grammar_terminal_destroy(IdmGrammarTerminal *terminal);
void idm_reader_program_destroy(IdmReaderProgram *program);
bool idm_reader_program_copy(IdmReaderProgram *dst, const IdmReaderProgram *src, IdmError *err, IdmSpan span);
void idm_reader_program_relocate(IdmReaderProgram *program, IdmScopeId min_id, int64_t delta);
bool idm_reader_program_validate(const IdmReaderProgram *program, IdmReaderProgramKind kind, IdmError *err, IdmSpan span);
bool idm_reader_program_serialize(IdmBuffer *out, const IdmReaderProgram *program, IdmReaderProgramKind kind, IdmError *err);
bool idm_reader_program_deserialize(IdmRuntime *rt, IdmByteReader *r, IdmReaderProgram *program, IdmReaderProgramKind kind, IdmError *err);
bool idm_grammar_terminal_from_ir(const IdmSyntax *pattern, IdmGrammarTerminal *out, IdmError *err);
bool idm_reader_pattern_compile_ir(const IdmSyntax *src, IdmReaderProgram *out, IdmError *err);
bool idm_reader_ctor_compile_ir(const IdmSyntax *src, IdmReaderProgram *out, IdmError *err);
bool idm_pkg_grammar_from_ir(const IdmSyntax *form, IdmPkgGrammar *out, IdmError *err);
bool idm_pkg_source_reader_from_ir(const IdmSyntax *form, char **out, IdmError *err);
void idm_grammar_rule_destroy(IdmGrammarRule *rule);
void idm_pkg_grammar_destroy(IdmPkgGrammar *grammar);
void idm_pkg_slot_destroy(IdmPkgSlot *slot);
bool idm_grammar_rule_validate(const IdmGrammarRule *rule, IdmError *err, IdmSpan span);
void idm_pkg_trait_destroy(IdmPkgTrait *trait);
void idm_pkg_type_destroy(IdmPkgType *type);
void idm_pkg_protocol_destroy(IdmPkgProtocol *protocol);
void idm_pkg_typed_entity_destroy(IdmPkgTypedEntity *entity);
bool idm_trait_method_defs_copy(const IdmTraitMethodDef *src, size_t count, IdmTraitMethodDef **out);
bool idm_trait_requirement_defs_copy(const IdmTraitRequirementDef *src, size_t count, IdmTraitRequirementDef **out);
void idm_artifact_destroy(IdmArtifact *art);

bool idm_package_read_source(IdmRuntime *rt, const char *path, IdmBuffer *out_src, const char **out_label, IdmSpan span, IdmError *err);

bool idm_artifact_serialize(const IdmArtifact *art, IdmBuffer *out, IdmError *err);
bool idm_artifact_deserialize(IdmRuntime *rt, const unsigned char *data, size_t len, IdmArtifact *out, IdmError *err);
const char *idm_grammar_mode_name(uint8_t mode);
const char *idm_grammar_rule_kind_name(uint8_t kind);
const char *idm_grammar_terminal_kind_name(uint8_t kind);

bool idm_artifact_cache_disabled(void);
typedef bool (*IdmArtifactPackageDepVerifier)(IdmRuntime *rt, const char *path, const unsigned char want[32], void *user);
bool idm_artifact_cache_load(IdmRuntime *rt, const char *path, const unsigned char src_hash[32], IdmArtifact *out, IdmArtifactPackageDepVerifier package_dep_verified, void *package_dep_user);
void idm_artifact_cache_write(const char *path, const IdmArtifact *art);
bool idm_artifact_dep_verified(IdmRuntime *rt, const IdmArtifactDep *dep);
void idm_phase_reads_destroy(IdmPhaseReads *reads);

#endif
