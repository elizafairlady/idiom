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
    bool has_default;
    bool seen_decl;
    IdmCore *default_fn;
    IdmScopeSet scopes;
    bool exported;
    uint32_t dispatch_slot;
    bool has_dispatch;
    uint32_t default_slot;
    bool has_default_slot;
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

typedef struct IdmReaderLiteral {
    uint8_t kind;
    char *text;
    int64_t integer;
    double real;
    struct IdmReaderLiteral *items;
    size_t count;
} IdmReaderLiteral;

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
    char *name;
    bool has_literal;
    size_t literal_index;
    IdmReaderLiteral literal;
    size_t child_count;
    uint8_t target_kind;
    size_t target_index;
    size_t capture_slot;
} IdmReaderPatternInst;

typedef struct {
    IdmReaderPatternInst *items;
    size_t count;
    size_t cap;
} IdmReaderPatternProgram;

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

typedef struct {
    uint8_t op;
    char *text;
    int64_t integer;
    bool has_literal;
    size_t literal_index;
    IdmReaderLiteral literal;
    size_t child_count;
    size_t capture_slot;
} IdmReaderCtorInst;

typedef struct {
    IdmReaderCtorInst *items;
    size_t count;
    size_t cap;
} IdmReaderCtorProgram;

typedef struct {
    char *name;
    uint8_t kind;
    IdmGrammarTerminal terminal;
    IdmReaderPatternProgram pattern;
    IdmReaderCtorProgram constructor;
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
} IdmPkgSlot;

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
    char *contract;
} IdmPkgTypeField;

typedef struct {
    char *name;
    char *identity;
    IdmScopeSet scopes;
    IdmPkgTypeField *fields;
    size_t field_count;
} IdmPkgType;

typedef struct {
    char *name;
    char *identity;
    IdmArtifact *art;
} IdmPkgProtocol;

struct IdmArtifact {
    IdmBytecodeModule *module;
    uint32_t init_fn;
    char *name;
    IdmPkgExport *exports;
    size_t export_count;
    IdmPkgSlot *slots;
    size_t slot_count;
    IdmPkgMacro *macros;
    size_t macro_count;
    IdmOperatorDef *operators;
    size_t operator_count;
    IdmPkgCoreSyntax *core_syntax;
    size_t core_syntax_count;
    IdmPkgGrammar *grammars;
    size_t grammar_count;
    char *source_reader;
    IdmPkgType *types;
    size_t type_count;
    IdmPkgTrait *traits;
    size_t trait_count;
    IdmPkgMethodImpl *method_impls;
    size_t method_impl_count;
    IdmPkgProtocol *protocols;
    size_t protocol_count;
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
void idm_reader_pattern_program_destroy(IdmReaderPatternProgram *program);
void idm_reader_ctor_program_destroy(IdmReaderCtorProgram *program);
void idm_reader_literal_destroy(IdmReaderLiteral *literal);
bool idm_reader_literal_copy(IdmReaderLiteral *dst, const IdmReaderLiteral *src, IdmError *err, IdmSpan span);
bool idm_reader_literal_from_syntax(IdmReaderLiteral *dst, const IdmSyntax *src, IdmError *err);
IdmSyntax *idm_reader_literal_to_syntax(const IdmReaderLiteral *literal, IdmSpan span, IdmError *err);
bool idm_reader_literal_equal_syntax(const IdmReaderLiteral *literal, const IdmSyntax *syntax);
bool idm_reader_literal_serialize(IdmBuffer *out, const IdmReaderLiteral *literal, IdmError *err);
bool idm_reader_literal_deserialize(IdmByteReader *r, IdmReaderLiteral *literal, IdmError *err);
bool idm_reader_pattern_program_copy(IdmReaderPatternProgram *dst, const IdmReaderPatternProgram *src, IdmError *err, IdmSpan span);
bool idm_reader_ctor_program_copy(IdmReaderCtorProgram *dst, const IdmReaderCtorProgram *src, IdmError *err, IdmSpan span);
void idm_reader_pattern_program_relocate(IdmReaderPatternProgram *program, IdmScopeId min_id, int64_t delta);
void idm_reader_ctor_program_relocate(IdmReaderCtorProgram *program, IdmScopeId min_id, int64_t delta);
bool idm_reader_pattern_program_validate(const IdmReaderPatternProgram *program, IdmError *err, IdmSpan span);
bool idm_reader_ctor_program_validate(const IdmReaderCtorProgram *program, IdmError *err, IdmSpan span);
bool idm_reader_pattern_program_serialize(IdmBuffer *out, const IdmReaderPatternProgram *program, IdmError *err);
bool idm_reader_pattern_program_deserialize(IdmByteReader *r, IdmReaderPatternProgram *program, IdmError *err);
bool idm_reader_ctor_program_serialize(IdmBuffer *out, const IdmReaderCtorProgram *program, IdmError *err);
bool idm_reader_ctor_program_deserialize(IdmByteReader *r, IdmReaderCtorProgram *program, IdmError *err);
bool idm_grammar_terminal_from_ir(const IdmSyntax *pattern, IdmGrammarTerminal *out, IdmError *err);
bool idm_reader_pattern_compile_ir(const IdmSyntax *src, IdmReaderPatternProgram *out, IdmError *err);
bool idm_reader_ctor_compile_ir(const IdmSyntax *src, IdmReaderCtorProgram *out, IdmError *err);
bool idm_pkg_grammar_from_ir(const IdmSyntax *form, IdmPkgGrammar *out, IdmError *err);
bool idm_pkg_source_reader_from_ir(const IdmSyntax *form, char **out, IdmError *err);
void idm_grammar_rule_destroy(IdmGrammarRule *rule);
void idm_pkg_grammar_destroy(IdmPkgGrammar *grammar);
void idm_pkg_slot_destroy(IdmPkgSlot *slot);
bool idm_grammar_rule_validate(const IdmGrammarRule *rule, IdmError *err, IdmSpan span);
void idm_pkg_trait_destroy(IdmPkgTrait *trait);
void idm_pkg_type_destroy(IdmPkgType *type);
void idm_pkg_protocol_destroy(IdmPkgProtocol *protocol);
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
