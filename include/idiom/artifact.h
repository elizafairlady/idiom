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

typedef enum {
    IDM_OPERATOR_ACTION_WORD,
    IDM_OPERATOR_ACTION_METHOD
} IdmOperatorAction;

typedef struct {
    char *name;
    char *capture;
    uint8_t capture_kind;
    bool capture_left;
    uint32_t capture_count;
    uint8_t precedence;
    IdmOpAssoc assoc;
    uint8_t action;
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
    IdmSymbol *identity;
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
} IdmPkgCoreForm;

typedef enum {
    IDM_ENFOREST_CHILD,
    IDM_ENFOREST_EACH,
    IDM_ENFOREST_VALUE,
    IDM_ENFOREST_SCOPE,
    IDM_ENFOREST_CONCAT,
    IDM_ENFOREST_PARTS,
    IDM_ENFOREST_BODY,
    IDM_ENFOREST_MATCH,
    IDM_ENFOREST_REGEX,
    IDM_ENFOREST_BITSTRING,
    IDM_ENFOREST_TRY,
    IDM_ENFOREST_RECEIVE,
    IDM_ENFOREST_IMPLEMENTS,
    IDM_ENFOREST_PROTOCOL_INFO,
    IDM_ENFOREST_TEMPLATE,
    IDM_ENFOREST_SYNTAX
} IdmEnforestOp;

typedef enum {
    IDM_ENFOREST_TEMPLATE_QUOTE,
    IDM_ENFOREST_TEMPLATE_QUASIQUOTE,
    IDM_ENFOREST_TEMPLATE_QUASISYNTAX
} IdmEnforestTemplateMode;

typedef struct IdmEnforestNode_s {
    uint8_t op;
    uint8_t mode;
    uint32_t index;
    struct IdmEnforestNode_s *child;
} IdmEnforestNode;

IdmEnforestNode *idm_enforest_clone(const IdmEnforestNode *node);
void idm_enforest_free(IdmEnforestNode *node);

typedef struct {
    char *name;
    IdmEnforestNode *node;
    bool transformer;
    IdmModuleRef *module;
    uint32_t function_index;
    IdmPhaseEnv *phase_env;
} IdmPkgReaderForm;

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
    IDM_GRAMMAR_TERMINAL_STRING,
    IDM_GRAMMAR_TERMINAL_BITSTRING
} IdmGrammarTerminalKind;

typedef struct {
    uint8_t kind;
    char *text;
    uint32_t flags;
} IdmGrammarTerminal;

#define IDM_READER_PATTERN_OP_LIST(X) \
    X(IDM_READER_PATTERN_EMPTY,        0u, 0u,       false, false, "invalid empty reader pattern instruction") \
    X(IDM_READER_PATTERN_REF,          0u, 0u,       true,  false, "invalid reader pattern reference instruction") \
    X(IDM_READER_PATTERN_TOKEN,        0u, 0u,       true,  false, "invalid reader pattern reference instruction") \
    X(IDM_READER_PATTERN_LITERAL,      0u, 0u,       false, true,  "invalid literal reader pattern instruction") \
    X(IDM_READER_PATTERN_SEQ,          1u, SIZE_MAX, false, false, "invalid compound reader pattern instruction") \
    X(IDM_READER_PATTERN_ALT,          1u, SIZE_MAX, false, false, "invalid compound reader pattern instruction") \
    X(IDM_READER_PATTERN_REPEAT,       1u, 1u,       false, false, "invalid unary reader pattern instruction") \
    X(IDM_READER_PATTERN_OPTIONAL,     1u, 1u,       false, false, "invalid unary reader pattern instruction") \
    X(IDM_READER_PATTERN_CAPTURE,      1u, 1u,       true,  false, "invalid capture reader pattern instruction") \
    X(IDM_READER_PATTERN_INDENT_GT,    1u, 1u,       true,  false, "invalid indentation reader pattern instruction") \
    X(IDM_READER_PATTERN_INDENT_EQ,    1u, 1u,       true,  false, "invalid indentation reader pattern instruction") \
    X(IDM_READER_PATTERN_ADJACENT,     1u, 1u,       false, false, "invalid unary reader pattern instruction") \
    X(IDM_READER_PATTERN_NOT_ADJACENT, 1u, 1u,       false, false, "invalid unary reader pattern instruction") \
    X(IDM_READER_PATTERN_PEEK,         1u, 1u,       false, false, "invalid unary reader pattern instruction") \
    X(IDM_READER_PATTERN_NOT,          1u, 1u,       false, false, "invalid unary reader pattern instruction") \
    X(IDM_READER_PATTERN_REPEAT1,      1u, 1u,       false, false, "invalid unary reader pattern instruction") \
    X(IDM_READER_PATTERN_PARAM,        0u, 0u,       true,  false, "invalid parameter reader pattern instruction")

typedef enum {
#define IDM_READER_OP_ENUM(op, min_children, max_children, text, literal, message) op,
    IDM_READER_PATTERN_OP_LIST(IDM_READER_OP_ENUM)
#undef IDM_READER_OP_ENUM
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

#define IDM_READER_CTOR_OP_LIST(X) \
    X(IDM_READER_CTOR_CAPTURE,       0u, 0u,       true,  false, "invalid reader constructor text instruction") \
    X(IDM_READER_CTOR_SPLICE,        0u, 0u,       true,  false, "invalid reader constructor text instruction") \
    X(IDM_READER_CTOR_CAPTURE_TEXT,  0u, 0u,       true,  false, "invalid reader constructor text instruction") \
    X(IDM_READER_CTOR_LITERAL,       0u, 0u,       false, true,  "invalid reader constructor literal instruction") \
    X(IDM_READER_CTOR_EMIT_TEXT,     0u, 0u,       true,  false, "invalid reader constructor text instruction") \
    X(IDM_READER_CTOR_EMIT_INT,      0u, 0u,       false, false, "invalid reader constructor integer instruction") \
    X(IDM_READER_CTOR_INTERP_STRING, 0u, 0u,       true,  false, "invalid reader constructor text instruction") \
    X(IDM_READER_CTOR_BITSTRING,     0u, 0u,       true,  false, "invalid reader constructor text instruction") \
    X(IDM_READER_CTOR_FORM,          0u, SIZE_MAX, true,  false, "invalid reader constructor form instruction") \
    X(IDM_READER_CTOR_SEQ,           0u, SIZE_MAX, false, false, "invalid reader constructor sequence instruction")

typedef enum {
#define IDM_READER_OP_ENUM(op, min_children, max_children, text, literal, message) op,
    IDM_READER_CTOR_OP_LIST(IDM_READER_OP_ENUM)
#undef IDM_READER_OP_ENUM
} IdmReaderCtorOp;

#define IDM_READER_OP_COUNT_ROW(op, min_children, max_children, text, literal, message) + 1
enum {
    IDM_READER_PATTERN_OP_COUNT = 0 IDM_READER_PATTERN_OP_LIST(IDM_READER_OP_COUNT_ROW),
    IDM_READER_CTOR_OP_COUNT = 0 IDM_READER_CTOR_OP_LIST(IDM_READER_OP_COUNT_ROW)
};
#undef IDM_READER_OP_COUNT_ROW

typedef enum {
    IDM_READER_PROGRAM_PATTERN,
    IDM_READER_PROGRAM_CTOR
} IdmReaderProgramKind;

typedef struct {
    char *name;
    uint8_t kind;
    char **params;
    size_t param_count;
    IdmGrammarTerminal terminal;
    IdmReaderProgram pattern;
    IdmReaderProgram constructor;
} IdmGrammarRule;

typedef struct {
    char *open;
    char *close;
} IdmGrammarPair;

typedef struct IdmPkgGrammar {
    char *name;
    uint8_t mode;
    IdmGrammarRule *rules;
    size_t rule_count;
    IdmGrammarPair *pairs;
    size_t pair_count;
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
    uint32_t *const_entries;
    uint32_t const_entry_count;
} IdmPkgSlot;

typedef struct {
    char *name;
    IdmSymbol *env_key;
    uint32_t slot;
    IdmScopeSet scopes;
    IdmArity arity;
    bool has_contract;
    IdmCallableContract contract;
} IdmPkgImport;


typedef enum {
    IDM_DEP_PACKAGE,
    IDM_DEP_FILE_HASH,
    IDM_DEP_FILE_PRESENT,
    IDM_DEP_FILE_ABSENT,
    IDM_DEP_FILE_STAT,
    IDM_DEP_DIRECTORY,
    IDM_DEP_ENV
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
    bool uncacheable;
};

typedef struct {
    char *name;
    IdmSymbol *identity;
    IdmTraitRequirementDef *requirements;
    size_t requirement_count;
    IdmTraitMethodDef *methods;
    size_t method_count;
} IdmPkgTrait;

typedef struct {
    IdmSymbol *trait;
    IdmSymbol *type;
    bool structural;
    IdmStructuralHead structural_head;
    char *method;
    IdmArity arity;
    bool impl_env;
    IdmSymbol *impl_env_key;
    uint32_t impl_slot;
    bool has_contract;
    IdmCallableContract contract;
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
    IdmSymbol *identity;
    IdmScopeSet scopes;
    IdmPkgTypeMember *members;
    size_t member_count;
    IdmPkgTypeField *fields;
    size_t field_count;
} IdmPkgType;

typedef struct {
    char *name;
    IdmSymbol *identity;
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
    IdmPkgImport *imports;
    size_t import_count;
    struct { char *name; IdmSymbol *env_key; uint32_t slot; } *field_selectors;
    size_t field_selector_count;
    IdmPkgMacro *macros;
    size_t macro_count;
    IdmOperatorDef *operators;
    size_t operator_count;
    IdmPkgCoreForm *core_form;
    size_t core_form_count;
    IdmPkgReaderForm *reader_forms;
    size_t reader_forms_count;
    IdmPkgGrammar *grammars;
    size_t grammar_count;
    char *source_reader;
    IdmArtifactTypedRegistry typed;
    IdmScopeId scope_base;
    IdmScopeId scope_end;
    IdmPhaseEnv *phase_env;
    IdmSymbol **protocol_requires;
    size_t protocol_require_count;
    unsigned char src_hash[32];
    unsigned char action_hash[32];
    IdmArtifactDep *deps;
    size_t dep_count;
    bool rt_owned;
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
void idm_pkg_core_form_destroy(IdmPkgCoreForm *core_form);
void idm_pkg_method_impl_destroy(IdmPkgMethodImpl *impl);
void idm_grammar_terminal_destroy(IdmGrammarTerminal *terminal);
void idm_reader_program_destroy(IdmReaderProgram *program);
bool idm_reader_program_copy(IdmReaderProgram *dst, const IdmReaderProgram *src, IdmError *err, IdmSpan span);
void idm_reader_program_relocate(IdmReaderProgram *program, IdmScopeId min_id, int64_t delta);
bool idm_pkg_grammar_from_ir(const IdmSyntax *form, IdmPkgGrammar *out, IdmError *err);
void idm_grammar_rule_destroy(IdmGrammarRule *rule);
void idm_grammar_pair_destroy(IdmGrammarPair *pair);
bool idm_grammar_pairs_copy(const IdmGrammarPair *src, size_t count, IdmGrammarPair **out);
void idm_pkg_grammar_destroy(IdmPkgGrammar *grammar);
void idm_pkg_slot_destroy(IdmPkgSlot *slot);
void idm_pkg_import_destroy(IdmPkgImport *imp);
bool idm_grammar_rule_validate(const IdmGrammarRule *rule, IdmError *err, IdmSpan span);
void idm_pkg_typed_entity_destroy(IdmPkgTypedEntity *entity);
bool idm_trait_requirement_defs_copy(const IdmTraitRequirementDef *src, size_t count, IdmTraitRequirementDef **out);
void idm_artifact_destroy(IdmArtifact *art);

bool idm_package_read_source(IdmRuntime *rt, const char *path, IdmBuffer *out_src, const char **out_label, const char **out_resolved, IdmSpan span, IdmError *err);

typedef struct {
    const char *label;
    char *source;
} IdmPackageUnit;

bool idm_package_read_units(IdmRuntime *rt, const char *path, IdmPackageUnit **out_units, size_t *out_count, const char **out_label, const char **out_resolved, IdmSpan span, IdmError *err);
void idm_package_units_free(IdmPackageUnit *units, size_t count);

bool idm_artifact_serialize(const IdmArtifact *art, IdmBuffer *out, IdmError *err);
bool idm_artifact_deserialize(IdmRuntime *rt, const unsigned char *data, size_t len, IdmArtifact *out, IdmError *err);
bool idm_artifact_compute_action_hash(IdmArtifact *art, IdmError *err);
const char *idm_grammar_mode_name(uint8_t mode);
const char *idm_grammar_rule_kind_name(uint8_t kind);
const char *idm_grammar_terminal_kind_name(uint8_t kind);

bool idm_tool_cache_load(const char *kind, const unsigned char key[32], char **out, size_t *out_len);
bool idm_tool_cache_store(const char *kind, const unsigned char key[32], const void *data, size_t len);
bool idm_artifact_action_cache_load(const unsigned char src_hash[32], IdmArtifact *out);
typedef bool (*IdmArtifactPackageDepVerifier)(IdmRuntime *rt, const char *path, const unsigned char want[32], void *user);
bool idm_artifact_cache_load(IdmRuntime *rt, const char *path, const unsigned char src_hash[32], IdmArtifact *out, IdmArtifactPackageDepVerifier package_dep_verified, void *package_dep_user);
void idm_artifact_cache_write(const char *path, const IdmArtifact *art);
bool idm_artifact_dep_verified(IdmRuntime *rt, const IdmArtifactDep *dep);
void idm_phase_env_record(IdmRuntime *rt, const char *name, const char *value);
void idm_phase_stat_record(IdmRuntime *rt, const char *path);
void idm_phase_directory_record(IdmRuntime *rt, const char *path);
void idm_phase_write_record(IdmRuntime *rt, const char *path);
void idm_phase_nondeterministic_record(IdmRuntime *rt);
void idm_phase_reads_destroy(IdmPhaseReads *reads);

#endif
