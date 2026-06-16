#ifndef IDM_CORE_H
#define IDM_CORE_H

#include "idiom/bytecode.h"
#include "idiom/pattern.h"

typedef enum {
    IDM_CORE_LITERAL,
    IDM_CORE_ARG_REF,
    IDM_CORE_LOCAL_REF,
    IDM_CORE_CAPTURE_REF,
    IDM_CORE_PRIMITIVE,
    IDM_CORE_APP,
    IDM_CORE_COND,
    IDM_CORE_DO,
    IDM_CORE_BIND_LOCAL,
    IDM_CORE_FN,
    IDM_CORE_FN_MULTI,
    IDM_CORE_LETREC,
    IDM_CORE_RECEIVE,
    IDM_CORE_GUARD,
    IDM_CORE_GLOBAL_REF,
    IDM_CORE_USE_PACKAGE,
    IDM_CORE_DEFINE_TRAIT,
    IDM_CORE_IMPLEMENT_TRAIT,
    IDM_CORE_METHOD_CALL
} IdmCoreKind;

typedef enum {
    IDM_PRIM_ADD,
    IDM_PRIM_SUB,
    IDM_PRIM_MUL,
    IDM_PRIM_DIV,
    IDM_PRIM_MOD,
    IDM_PRIM_POW,
    IDM_PRIM_NEG,
    IDM_PRIM_EQ,
    IDM_PRIM_NEQ,
    IDM_PRIM_LT,
    IDM_PRIM_GT,
    IDM_PRIM_LTE,
    IDM_PRIM_GTE,
    IDM_PRIM_OK,
    IDM_PRIM_TRAIT_IMPLEMENTED_P,
    IDM_PRIM_CONS,
    IDM_PRIM_FIRST,
    IDM_PRIM_REST,
    IDM_PRIM_LIST,
    IDM_PRIM_TUPLE,
    IDM_PRIM_VECTOR,
    IDM_PRIM_DICT,
    IDM_PRIM_TUPLE_GET,
    IDM_PRIM_APPEND,
    IDM_PRIM_STR_TO_LIST,
    IDM_PRIM_DICT_TO_LIST,
    IDM_PRIM_VECTOR_TO_LIST,
    IDM_PRIM_TUPLE_TO_LIST,
    IDM_PRIM_APPLY,
    IDM_PRIM_SYNTAX_KIND,
    IDM_PRIM_SYNTAX_TO_DATUM,
    IDM_PRIM_DATUM_TO_SYNTAX,
    IDM_PRIM_SYNTAX_PROPERTY,
    IDM_PRIM_SYNTAX_SET_PROPERTY,
    IDM_PRIM_SYNTAX_ORIGIN,
    IDM_PRIM_SYNTAX_LIST_PRED,
    IDM_PRIM_SYNTAX_LENGTH,
    IDM_PRIM_SYNTAX_NTH,
    IDM_PRIM_SYNTAX_SLICE,
    IDM_PRIM_SYNTAX_WORD_PRED,
    IDM_PRIM_SYNTAX_WORD_TEXT,
    IDM_PRIM_SYNTAX_ATOM_PRED,
    IDM_PRIM_SYNTAX_ATOM_TEXT,
    IDM_PRIM_SYNTAX_INT_PRED,
    IDM_PRIM_SYNTAX_INT_VALUE,
    IDM_PRIM_MAKE_SYNTAX_WORD,
    IDM_PRIM_MAKE_SYNTAX_ATOM,
    IDM_PRIM_MAKE_SYNTAX_INT,
    IDM_PRIM_MAKE_SYNTAX_STRING,
    IDM_PRIM_MAKE_SYNTAX_LIST,
    IDM_PRIM_MAKE_SYNTAX_VECTOR,
    IDM_PRIM_MAKE_SYNTAX_TUPLE,
    IDM_PRIM_MAKE_SYNTAX_EXPR,
    IDM_PRIM_MAKE_SYNTAX_BODY,
    IDM_PRIM_MAKE_SYNTAX_GROUP,
    IDM_PRIM_SYNTAX_ERROR,
    IDM_PRIM_LOCAL_EXPAND,
    IDM_PRIM_FREE_IDENTIFIER_EQ,
    IDM_PRIM_BOUND_IDENTIFIER_EQ,
    IDM_PRIM_BIND_BANG,
    IDM_PRIM_SELF,
    IDM_PRIM_SPAWN,
    IDM_PRIM_SEND,
    IDM_PRIM_EXIT,
    IDM_PRIM_LINK,
    IDM_PRIM_UNLINK,
    IDM_PRIM_MONITOR,
    IDM_PRIM_DEMONITOR,
    IDM_PRIM_TRAP_EXIT,
    IDM_PRIM_STR,
    IDM_PRIM_CHOMP,
    IDM_PRIM_CAPTURE_STDOUT,
    IDM_PRIM_EXEC,
    IDM_PRIM_AWAIT,
    IDM_PRIM_PRINT,
    IDM_PRIM_PRINTLN,
    IDM_PRIM_CD,
    IDM_PRIM_CHDIR,
    IDM_PRIM_PWD,
    IDM_PRIM_WRITE_PROCSUB_TEMP,
    IDM_PRIM_MAKE_PROCSUB_TEMP,
    IDM_PRIM_MAKE_RECORD,
    IDM_PRIM_RECORD_PRED,
    IDM_PRIM_RECORD_TYPE,
    IDM_PRIM_RECORD_FIELD,
    IDM_PRIM_ENV_GET,
    IDM_PRIM_ENV_SET,
    IDM_PRIM_SYNTAX_ADJACENT_PRED,
    IDM_PRIM_SYNTAX_STRING_TEXT,
    IDM_PRIM_STR_CONTAINS,
    IDM_PRIM_EXPANDER_REGISTER_OPERATOR,
    IDM_PRIM_EXPANDER_REGISTER_MACRO,
    IDM_PRIM_EXPANDER_SURFACE,
    IDM_PRIM_EXPAND_CHECK,
    IDM_PRIM_INSPECT,
    IDM_PRIM_STR_LEN,
    IDM_PRIM_STR_SLICE,
    IDM_PRIM_STR_FIND,
    IDM_PRIM_STR_BYTE,
    IDM_PRIM_BYTE_STR,
    IDM_PRIM_REGEX_COMPILE,
    IDM_PRIM_REGEX_PRED,
    IDM_PRIM_REGEX_SOURCE,
    IDM_PRIM_REGEX_OPTIONS,
    IDM_PRIM_REGEX_GROUP_COUNT,
    IDM_PRIM_REGEX_GROUP_NAMES,
    IDM_PRIM_REGEX_RESULT_PRED,
    IDM_PRIM_REGEX_SCAN_AT,
    IDM_PRIM_REGEX_SCAN_FROM,
    IDM_PRIM_REGEX_SCAN_FULL,
    IDM_PRIM_REGEX_TEST,
    IDM_PRIM_REGEX_RESULT_START,
    IDM_PRIM_REGEX_RESULT_END,
    IDM_PRIM_REGEX_RESULT_TEXT,
    IDM_PRIM_REGEX_CAPTURE,
    IDM_PRIM_REGEX_CAPTURE_RANGE,
    IDM_PRIM_REGEX_CAPTURE_NAMED,
    IDM_PRIM_REGEX_CAPTURES,
    IDM_PRIM_REGEX_SCAN_ALL,
    IDM_PRIM_REGEX_REPLACE,
    IDM_PRIM_REGEX_REPLACE_ALL,
    IDM_PRIM_REGEX_SPLIT_ON,
    IDM_PRIM_REGEX_ESCAPE,
    IDM_PRIM_FILE_READ,
    IDM_PRIM_FILE_WRITE,
    IDM_PRIM_FILE_EXISTS,
    IDM_PRIM_FILE_STAT,
    IDM_PRIM_FILE_LIST,
    IDM_PRIM_FILE_REMOVE,
    IDM_PRIM_ARGS,
    IDM_PRIM_TIME_MS,
    IDM_PRIM_RANDOM,
    IDM_PRIM_DICT_GET,
    IDM_PRIM_DICT_PUT,
    IDM_PRIM_DICT_DEL,
    IDM_PRIM_DICT_KEYS,
    IDM_PRIM_DICT_VALS,
    IDM_PRIM_DICT_HAS,
    IDM_PRIM_DICT_SIZE,
    IDM_PRIM_RECORD_NEW,
    IDM_PRIM_ABS,
    IDM_PRIM_FLOOR,
    IDM_PRIM_ROUND,
    IDM_PRIM_SQRT,
    IDM_PRIM_FLOOR_DIV,
    IDM_PRIM_FLOOR_MOD,
    IDM_PRIM_PARSE_INT,
    IDM_PRIM_PARSE_FLOAT,
    IDM_PRIM_FILE_MKDIR,
    IDM_PRIM_FILE_APPEND,
    IDM_PRIM_ORD_STR,
    IDM_PRIM_STR_ORD,
    IDM_PRIM_FROM_RUNES,
    IDM_PRIM_REPL_COMPILE,
    IDM_PRIM_REPL_ABORT,
    IDM_PRIM_REPL_SPAWN,
    IDM_PRIM_REPL_DIAGNOSTIC,
    IDM_PRIM_ISH_SESSION,
    IDM_PRIM_TTY_PRED,
    IDM_PRIM_TTY_RAW,
    IDM_PRIM_TTY_RESTORE,
    IDM_PRIM_TTY_READ,
    IDM_PRIM_TTY_READ_LINE,
    IDM_PRIM_TTY_WRITE,
    IDM_PRIM_TTY_SIZE,
    IDM_PRIM_EPRINTLN,
    IDM_PRIM_PORT_STATUS,
    IDM_PRIM_JOB_RESUME,
    IDM_PRIM_JOB_SIGNAL,
    IDM_PRIM_ERROR_MESSAGE,
    IDM_PRIM_MAKE_ERROR,
    IDM_PRIM_SPAWN_LINK,
    IDM_PRIM_SPAWN_MONITOR,
    IDM_PRIM_PORT_READ,
    IDM_PRIM_PORT_WRITE,
    IDM_PRIM_PORT_CLOSE_INPUT,
    IDM_PRIM_RAISE,
    IDM_PRIM_IS_A_P,
    IDM_PRIM_NIL_P,
    IDM_PRIM_ATOM_P,
    IDM_PRIM_WORD_P,
    IDM_PRIM_INT_P,
    IDM_PRIM_FLOAT_P,
    IDM_PRIM_STRING_P,
    IDM_PRIM_PAIR_P,
    IDM_PRIM_EMPTY_LIST_P,
    IDM_PRIM_LIST_P,
    IDM_PRIM_TUPLE_P,
    IDM_PRIM_VECTOR_P,
    IDM_PRIM_DICT_P,
    IDM_PRIM_SYNTAX_P,
    IDM_PRIM_CELL_P,
    IDM_PRIM_CLOSURE_P,
    IDM_PRIM_PID_P,
    IDM_PRIM_REF_P,
    IDM_PRIM_PORT_P,
    IDM_PRIM_PRIMITIVE_P,
    IDM_PRIM_REGEX_P,
    IDM_PRIM_REGEX_RESULT_P,
    IDM_PRIM_COMPARE,
    IDM_PRIM_CEIL,
    IDM_PRIM_TRUNCATE,
    IDM_PRIM_SIN,
    IDM_PRIM_COS,
    IDM_PRIM_TAN,
    IDM_PRIM_ASIN,
    IDM_PRIM_ACOS,
    IDM_PRIM_ATAN,
    IDM_PRIM_ATAN2,
    IDM_PRIM_EXP,
    IDM_PRIM_LOG,
    IDM_PRIM_LOG2,
    IDM_PRIM_LOG10,
    IDM_PRIM_HYPOT,
    IDM_PRIM_NAN_P,
    IDM_PRIM_FINITE_P,
    IDM_PRIM_INFINITE_P,
    IDM_PRIM_NAN,
    IDM_PRIM_INF,
    IDM_PRIM_DIVMOD,
    IDM_PRIM_BIT_AND,
    IDM_PRIM_BIT_OR,
    IDM_PRIM_BIT_XOR,
    IDM_PRIM_BIT_NOT,
    IDM_PRIM_SHIFT_LEFT,
    IDM_PRIM_SHIFT_RIGHT,
    IDM_PRIM_BIT_COUNT,
    IDM_PRIM_BIT_LENGTH,
    IDM_PRIM_TO_INT,
    IDM_PRIM_TO_FLOAT
} IdmPrimitive;

typedef struct {
    const char *name;
    uint32_t min_arity;
    uint32_t max_arity;
} IdmPrimitiveInfo;

typedef struct IdmCore IdmCore;

typedef struct {
    IdmValue name;
} IdmCoreTraitRequirement;

typedef struct {
    IdmValue name;
    uint32_t arity;
    bool has_default;
    IdmCore *default_fn;
} IdmCoreTraitMethod;

typedef struct {
    IdmValue name;
    uint32_t arity;
    IdmCore *impl_fn;
} IdmCoreTraitImpl;

typedef enum {
    IDM_CAP_LOCAL,
    IDM_CAP_ARG,
    IDM_CAP_UPVALUE
} IdmCaptureKind;

typedef struct {
    IdmCaptureKind kind;
    uint32_t index;
    bool celled;
} IdmCapture;

typedef struct {
    uint32_t arity;
    IdmPattern **param_patterns;
    uint32_t pattern_count;
    IdmPatternLocal *pattern_locals;
    uint32_t pattern_local_count;
    IdmCore *guard;
    IdmCore *body;
} IdmFnClause;

typedef struct {
    char *name;
    uint32_t slot;
    IdmCore *value;
} IdmLetRecBinding;

struct IdmCore {
    IdmCoreKind kind;
    IdmSpan span;
    bool local_celled;
    union {
        IdmValue literal;
        uint32_t local_slot;
        IdmPrimitive primitive;
        struct {
            IdmCore *callee;
            IdmCore **args;
            size_t arg_count;
            size_t arg_cap;
        } app;
        struct {
            IdmCore *cond;
            IdmCore *then_branch;
            IdmCore *else_branch;
        } cond_expr;
        struct {
            IdmCore **items;
            size_t count;
            size_t cap;
        } do_expr;
        struct {
            uint32_t slot;
            IdmCore *value;
            IdmCore *body;
        } bind_local;
        struct {
            char *name;
            uint32_t arity;
            IdmCapture *captures;
            size_t capture_count;
            size_t capture_cap;
            IdmPattern **param_patterns;
            uint32_t pattern_count;
            IdmPatternLocal *pattern_locals;
            uint32_t pattern_local_count;
            IdmCore *guard;
            IdmCore *body;
        } fn;
        struct {
            char *name;
            IdmCapture *captures;
            size_t capture_count;
            size_t capture_cap;
            IdmFnClause *clauses;
            size_t count;
            size_t cap;
        } fn_multi;
        struct {
            IdmLetRecBinding *bindings;
            size_t count;
            size_t cap;
            IdmCore *body;
            bool global;
            bool fill_only;
        } letrec;
        struct {
            IdmCore *receiver;
            IdmCore *timeout;
            IdmCore *timeout_body;
        } receive;
        struct {
            IdmCore *body;
            IdmCore *handler;
            IdmCore *cleanup;
            uint32_t rescue_slot;
            uint32_t ensure_slot;
        } guard;
        struct {
            IdmValue name;
            IdmBytecodeModule *module;
            uint32_t init_fn;
            uint32_t *export_src;
            uint32_t *export_dst;
            size_t export_count;
            IdmCore *cont;
        } use_package;
        struct {
            IdmValue name;
            IdmCoreTraitRequirement *requirements;
            size_t requirement_count;
            size_t requirement_cap;
            IdmCoreTraitMethod *methods;
            size_t count;
            size_t cap;
        } define_trait;
        struct {
            IdmValue trait;
            IdmValue type;
            IdmValue provider;
            IdmValue provider_key;
            IdmCoreTraitImpl *impls;
            size_t count;
            size_t cap;
        } implement_trait;
        struct {
            IdmValue trait;
            IdmValue method;
            IdmCore **args;
            size_t arg_count;
            size_t arg_cap;
        } method_call;
    } as;
};

IdmCore *idm_core_literal(IdmValue value, IdmSpan span);
IdmCore *idm_core_arg_ref(uint32_t slot, IdmSpan span);
IdmCore *idm_core_local_ref(uint32_t slot, IdmSpan span);
IdmCore *idm_core_capture_ref(uint32_t slot, IdmSpan span);
IdmCore *idm_core_primitive(IdmPrimitive primitive, IdmSpan span);
IdmCore *idm_core_app(IdmCore *callee, IdmSpan span);
bool idm_core_app_add_arg(IdmCore *app, IdmCore *arg);
IdmCore *idm_core_cond(IdmCore *cond, IdmCore *then_branch, IdmCore *else_branch, IdmSpan span);
IdmCore *idm_core_do(IdmSpan span);
bool idm_core_do_add(IdmCore *do_expr, IdmCore *item);
IdmCore *idm_core_bind_local(uint32_t slot, IdmCore *value, IdmCore *body, IdmSpan span);
IdmCore *idm_core_fn(const char *name, uint32_t arity, IdmCore *body, IdmSpan span);
bool idm_core_fn_add_capture(IdmCore *fn, IdmCaptureKind kind, uint32_t index);
bool idm_core_fn_set_param_patterns_take(IdmCore *fn, IdmPattern **patterns, uint32_t pattern_count);
bool idm_core_fn_set_pattern_locals_take(IdmCore *fn, IdmPatternLocal *locals, uint32_t local_count);
bool idm_core_fn_set_guard_take(IdmCore *fn, IdmCore *guard);
IdmCore *idm_core_fn_multi(const char *name, IdmSpan span);
bool idm_core_fn_multi_add_capture(IdmCore *multi, IdmCaptureKind kind, uint32_t index);
bool idm_core_fn_multi_add_clause_take(IdmCore *multi, uint32_t arity, IdmPattern **patterns, uint32_t pattern_count, IdmPatternLocal *locals, uint32_t local_count, IdmCore *guard, IdmCore *body);
IdmCore *idm_core_letrec(IdmSpan span);
bool idm_core_letrec_add(IdmCore *letrec, const char *name, uint32_t slot, IdmCore *value);
bool idm_core_letrec_set_body(IdmCore *letrec, IdmCore *body);
void idm_core_letrec_set_global(IdmCore *letrec);
void idm_core_letrec_set_fill_only(IdmCore *letrec);
IdmCore *idm_core_global_ref(uint32_t id, IdmSpan span);
IdmCore *idm_core_receive(IdmCore *receiver, IdmCore *timeout, IdmCore *timeout_body, IdmSpan span);
IdmCore *idm_core_guard(IdmCore *body, IdmCore *handler, uint32_t rescue_slot, IdmCore *cleanup, uint32_t ensure_slot, IdmSpan span);
IdmCore *idm_core_use_package(IdmValue name, IdmBytecodeModule *module, uint32_t init_fn, uint32_t *export_src, uint32_t *export_dst, size_t export_count, IdmCore *cont, IdmSpan span);
IdmCore *idm_core_define_trait(IdmValue name, IdmSpan span);
bool idm_core_define_trait_add_requirement(IdmCore *core, IdmValue requirement);
bool idm_core_define_trait_add_method(IdmCore *core, IdmValue method, uint32_t arity, IdmCore *default_fn);
IdmCore *idm_core_implement_trait(IdmValue trait, IdmValue type, IdmValue provider, IdmValue provider_key, IdmSpan span);
bool idm_core_implement_trait_add_impl(IdmCore *core, IdmValue method, uint32_t arity, IdmCore *impl_fn);
IdmCore *idm_core_method_call(IdmValue trait, IdmValue method, IdmSpan span);
bool idm_core_method_call_add_arg(IdmCore *core, IdmCore *arg);
void idm_core_free(IdmCore *core);
bool idm_core_compile_expression(IdmCore *core, IdmBytecodeModule *module, IdmError *err);
bool idm_core_compile_function_body(IdmCore *body, const char *name, uint32_t arity, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err);
bool idm_core_compile_main(IdmCore *core, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err);
bool idm_core_dump(IdmBuffer *buf, const IdmCore *core);
const char *idm_primitive_name(IdmPrimitive primitive);
bool idm_checked_add(int64_t a, int64_t b, int64_t *out);
bool idm_checked_sub(int64_t a, int64_t b, int64_t *out);
bool idm_checked_mul(int64_t a, int64_t b, int64_t *out);
bool idm_checked_pow(int64_t base, int64_t exponent, int64_t *out);
size_t idm_primitive_count(void);
const IdmPrimitiveInfo *idm_primitive_info(IdmPrimitive primitive);

#endif
