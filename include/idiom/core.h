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
    IDM_CORE_RAISE,
    IDM_CORE_RAISED,
    IDM_CORE_RESCUE,
    IDM_CORE_ENSURE,
    IDM_CORE_GLOBAL_REF,
    IDM_CORE_USE_PACKAGE,
    IDM_CORE_DEFINE_PROTOCOL,
    IDM_CORE_EXTEND_PROTOCOL,
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
    IDM_PRIM_CONS,
    IDM_PRIM_FIRST,
    IDM_PRIM_REST,
    IDM_PRIM_LIST,
    IDM_PRIM_TUPLE,
    IDM_PRIM_VECTOR,
    IDM_PRIM_DICT,
    IDM_PRIM_TUPLE_GET,
    IDM_PRIM_APPEND,
    IDM_PRIM_TO_LIST,
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
    IDM_PRIM_RECORD_NEW
} IdmPrimitive;

typedef struct {
    const char *name;
    uint32_t min_arity;
    uint32_t max_arity;
} IdmPrimitiveInfo;

typedef struct IdmCore IdmCore;

typedef struct {
    IdmValue name;
    uint32_t arity;
    bool has_default;
    IdmCore *default_fn;
} IdmCoreProtocolMethod;

typedef struct {
    IdmValue name;
    uint32_t arity;
    IdmCore *impl_fn;
} IdmCoreProtocolImpl;

typedef enum {
    IDM_CAP_LOCAL,
    IDM_CAP_ARG,
    IDM_CAP_UPVALUE
} IdmCaptureKind;

typedef struct {
    IdmCaptureKind kind;
    uint32_t index;
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
        } letrec;
        struct {
            IdmCore *receiver;
            IdmCore *timeout;
            IdmCore *timeout_body;
        } receive;
        struct {
            IdmCore *value;
        } raise;
        struct {
            IdmCore *body;
            IdmCore *handler;
        } rescue;
        struct {
            IdmCore *body;
            IdmCore *cleanup;
            uint32_t tmp_slot;
        } ensure;
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
            IdmCoreProtocolMethod *methods;
            size_t count;
            size_t cap;
        } define_protocol;
        struct {
            IdmValue protocol;
            IdmValue type;
            IdmCoreProtocolImpl *impls;
            size_t count;
            size_t cap;
        } extend_protocol;
        struct {
            IdmValue protocol;
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
IdmCore *idm_core_global_ref(uint32_t id, IdmSpan span);
IdmCore *idm_core_receive(IdmCore *receiver, IdmCore *timeout, IdmCore *timeout_body, IdmSpan span);
IdmCore *idm_core_raise(IdmCore *value, IdmSpan span);
IdmCore *idm_core_raised(IdmSpan span);
IdmCore *idm_core_rescue(IdmCore *body, IdmCore *handler, IdmSpan span);
IdmCore *idm_core_ensure(IdmCore *body, IdmCore *cleanup, uint32_t tmp_slot, IdmSpan span);
IdmCore *idm_core_use_package(IdmValue name, IdmBytecodeModule *module, uint32_t init_fn, uint32_t *export_src, uint32_t *export_dst, size_t export_count, IdmCore *cont, IdmSpan span);
IdmCore *idm_core_define_protocol(IdmValue name, IdmSpan span);
bool idm_core_define_protocol_add_method(IdmCore *core, IdmValue method, uint32_t arity, IdmCore *default_fn);
IdmCore *idm_core_extend_protocol(IdmValue protocol, IdmValue type, IdmSpan span);
bool idm_core_extend_protocol_add_impl(IdmCore *core, IdmValue method, uint32_t arity, IdmCore *impl_fn);
IdmCore *idm_core_method_call(IdmValue protocol, IdmValue method, IdmSpan span);
bool idm_core_method_call_add_arg(IdmCore *core, IdmCore *arg);
void idm_core_free(IdmCore *core);
bool idm_core_compile_expression(IdmCore *core, IdmBytecodeModule *module, IdmError *err);
bool idm_core_compile_function_body(IdmCore *body, const char *name, uint32_t arity, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err);
bool idm_core_compile_main(IdmCore *core, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err);
bool idm_core_dump(IdmBuffer *buf, const IdmCore *core);
const char *idm_primitive_name(IdmPrimitive primitive);
size_t idm_primitive_count(void);
const IdmPrimitiveInfo *idm_primitive_info(IdmPrimitive primitive);

#endif
