#ifndef ISH_CORE_H
#define ISH_CORE_H

#include "ish/bytecode.h"
#include "ish/pattern.h"

typedef enum {
    ISH_CORE_LITERAL,
    ISH_CORE_ARG_REF,
    ISH_CORE_LOCAL_REF,
    ISH_CORE_CAPTURE_REF,
    ISH_CORE_PRIMITIVE,
    ISH_CORE_APP,
    ISH_CORE_COND,
    ISH_CORE_DO,
    ISH_CORE_BIND_LOCAL,
    ISH_CORE_FN,
    ISH_CORE_FN_MULTI,
    ISH_CORE_LETREC,
    ISH_CORE_RECEIVE,
    ISH_CORE_RAISE,
    ISH_CORE_RAISED,
    ISH_CORE_RESCUE,
    ISH_CORE_ENSURE,
    ISH_CORE_GLOBAL_REF,
    ISH_CORE_USE_PACKAGE,
    ISH_CORE_DEFINE_PROTOCOL,
    ISH_CORE_EXTEND_PROTOCOL,
    ISH_CORE_METHOD_CALL
} IshCoreKind;

typedef enum {
    ISH_PRIM_ADD,
    ISH_PRIM_SUB,
    ISH_PRIM_MUL,
    ISH_PRIM_DIV,
    ISH_PRIM_MOD,
    ISH_PRIM_POW,
    ISH_PRIM_NEG,
    ISH_PRIM_EQ,
    ISH_PRIM_NEQ,
    ISH_PRIM_LT,
    ISH_PRIM_GT,
    ISH_PRIM_LTE,
    ISH_PRIM_GTE,
    ISH_PRIM_CONS,
    ISH_PRIM_FIRST,
    ISH_PRIM_REST,
    ISH_PRIM_LIST,
    ISH_PRIM_TUPLE,
    ISH_PRIM_VECTOR,
    ISH_PRIM_DICT,
    ISH_PRIM_TUPLE_GET,
    ISH_PRIM_APPEND,
    ISH_PRIM_TO_LIST,
    ISH_PRIM_APPLY,
    ISH_PRIM_SYNTAX_KIND,
    ISH_PRIM_SYNTAX_TO_DATUM,
    ISH_PRIM_DATUM_TO_SYNTAX,
    ISH_PRIM_SYNTAX_PROPERTY,
    ISH_PRIM_SYNTAX_SET_PROPERTY,
    ISH_PRIM_SYNTAX_ORIGIN,
    ISH_PRIM_SYNTAX_LIST_PRED,
    ISH_PRIM_SYNTAX_LENGTH,
    ISH_PRIM_SYNTAX_NTH,
    ISH_PRIM_SYNTAX_SLICE,
    ISH_PRIM_SYNTAX_WORD_PRED,
    ISH_PRIM_SYNTAX_WORD_TEXT,
    ISH_PRIM_SYNTAX_ATOM_PRED,
    ISH_PRIM_SYNTAX_ATOM_TEXT,
    ISH_PRIM_SYNTAX_INT_PRED,
    ISH_PRIM_SYNTAX_INT_VALUE,
    ISH_PRIM_MAKE_SYNTAX_WORD,
    ISH_PRIM_MAKE_SYNTAX_ATOM,
    ISH_PRIM_MAKE_SYNTAX_INT,
    ISH_PRIM_MAKE_SYNTAX_STRING,
    ISH_PRIM_MAKE_SYNTAX_LIST,
    ISH_PRIM_MAKE_SYNTAX_VECTOR,
    ISH_PRIM_MAKE_SYNTAX_TUPLE,
    ISH_PRIM_MAKE_SYNTAX_EXPR,
    ISH_PRIM_MAKE_SYNTAX_BODY,
    ISH_PRIM_MAKE_SYNTAX_GROUP,
    ISH_PRIM_SYNTAX_ERROR,
    ISH_PRIM_LOCAL_EXPAND,
    ISH_PRIM_FREE_IDENTIFIER_EQ,
    ISH_PRIM_BOUND_IDENTIFIER_EQ,
    ISH_PRIM_BIND_BANG,
    ISH_PRIM_SELF,
    ISH_PRIM_SPAWN,
    ISH_PRIM_SEND,
    ISH_PRIM_EXIT,
    ISH_PRIM_LINK,
    ISH_PRIM_UNLINK,
    ISH_PRIM_MONITOR,
    ISH_PRIM_DEMONITOR,
    ISH_PRIM_TRAP_EXIT,
    ISH_PRIM_STR,
    ISH_PRIM_CHOMP,
    ISH_PRIM_CAPTURE_STDOUT,
    ISH_PRIM_EXEC,
    ISH_PRIM_AWAIT,
    ISH_PRIM_PRINT,
    ISH_PRIM_PRINTLN,
    ISH_PRIM_CD,
    ISH_PRIM_PWD,
    ISH_PRIM_WRITE_PROCSUB_TEMP,
    ISH_PRIM_MAKE_PROCSUB_TEMP,
    ISH_PRIM_MAKE_RECORD,
    ISH_PRIM_RECORD_PRED,
    ISH_PRIM_RECORD_TYPE,
    ISH_PRIM_RECORD_FIELD,
    ISH_PRIM_ENV_GET,
    ISH_PRIM_ENV_SET,
    ISH_PRIM_SYNTAX_ADJACENT_PRED,
    ISH_PRIM_SYNTAX_STRING_TEXT,
    ISH_PRIM_STR_CONTAINS,
    ISH_PRIM_EXPANDER_REGISTER_OPERATOR,
    ISH_PRIM_EXPANDER_REGISTER_MACRO,
    ISH_PRIM_EXPANDER_SURFACE
} IshPrimitive;

typedef struct {
    const char *name;
    uint32_t min_arity;
    uint32_t max_arity;
} IshPrimitiveInfo;

typedef struct IshCore IshCore;

typedef struct {
    IshValue name;
    uint32_t arity;
    bool has_default;
    IshCore *default_fn;
} IshCoreProtocolMethod;

typedef struct {
    IshValue name;
    uint32_t arity;
    IshCore *impl_fn;
} IshCoreProtocolImpl;

typedef enum {
    ISH_CAP_LOCAL,
    ISH_CAP_ARG,
    ISH_CAP_UPVALUE
} IshCaptureKind;

typedef struct {
    IshCaptureKind kind;
    uint32_t index;
} IshCapture;

typedef struct {
    uint32_t arity;
    IshPattern **param_patterns;
    uint32_t pattern_count;
    IshPatternLocal *pattern_locals;
    uint32_t pattern_local_count;
    IshCore *guard;
    IshCore *body;
} IshFnClause;

typedef struct {
    char *name;
    uint32_t slot;
    IshCore *value;
} IshLetRecBinding;

struct IshCore {
    IshCoreKind kind;
    IshSpan span;
    union {
        IshValue literal;
        uint32_t local_slot;
        IshPrimitive primitive;
        struct {
            IshCore *callee;
            IshCore **args;
            size_t arg_count;
            size_t arg_cap;
        } app;
        struct {
            IshCore *cond;
            IshCore *then_branch;
            IshCore *else_branch;
        } cond_expr;
        struct {
            IshCore **items;
            size_t count;
            size_t cap;
        } do_expr;
        struct {
            uint32_t slot;
            IshCore *value;
            IshCore *body;
        } bind_local;
        struct {
            char *name;
            uint32_t arity;
            IshCapture *captures;
            size_t capture_count;
            size_t capture_cap;
            IshPattern **param_patterns;
            uint32_t pattern_count;
            IshPatternLocal *pattern_locals;
            uint32_t pattern_local_count;
            IshCore *guard;
            IshCore *body;
        } fn;
        struct {
            char *name;
            IshCapture *captures;
            size_t capture_count;
            size_t capture_cap;
            IshFnClause *clauses;
            size_t count;
            size_t cap;
        } fn_multi;
        struct {
            IshLetRecBinding *bindings;
            size_t count;
            size_t cap;
            IshCore *body;
            bool global;
        } letrec;
        struct {
            IshCore *receiver;
            IshCore *timeout;
            IshCore *timeout_body;
        } receive;
        struct {
            IshCore *value;
        } raise;
        struct {
            IshCore *body;
            IshCore *handler;
        } rescue;
        struct {
            IshCore *body;
            IshCore *cleanup;
            uint32_t tmp_slot;
        } ensure;
        struct {
            IshValue name;
            IshBytecodeModule *module;
            uint32_t init_fn;
            uint32_t *export_src;
            uint32_t *export_dst;
            size_t export_count;
            IshCore *cont;
        } use_package;
        struct {
            IshValue name;
            IshCoreProtocolMethod *methods;
            size_t count;
            size_t cap;
        } define_protocol;
        struct {
            IshValue protocol;
            IshValue type;
            IshCoreProtocolImpl *impls;
            size_t count;
            size_t cap;
        } extend_protocol;
        struct {
            IshValue protocol;
            IshValue method;
            IshCore **args;
            size_t arg_count;
            size_t arg_cap;
        } method_call;
    } as;
};

IshCore *ish_core_literal(IshValue value, IshSpan span);
IshCore *ish_core_arg_ref(uint32_t slot, IshSpan span);
IshCore *ish_core_local_ref(uint32_t slot, IshSpan span);
IshCore *ish_core_capture_ref(uint32_t slot, IshSpan span);
IshCore *ish_core_primitive(IshPrimitive primitive, IshSpan span);
IshCore *ish_core_app(IshCore *callee, IshSpan span);
bool ish_core_app_add_arg(IshCore *app, IshCore *arg);
IshCore *ish_core_cond(IshCore *cond, IshCore *then_branch, IshCore *else_branch, IshSpan span);
IshCore *ish_core_do(IshSpan span);
bool ish_core_do_add(IshCore *do_expr, IshCore *item);
IshCore *ish_core_bind_local(uint32_t slot, IshCore *value, IshCore *body, IshSpan span);
IshCore *ish_core_fn(const char *name, uint32_t arity, IshCore *body, IshSpan span);
bool ish_core_fn_add_capture(IshCore *fn, IshCaptureKind kind, uint32_t index);
bool ish_core_fn_set_param_patterns_take(IshCore *fn, IshPattern **patterns, uint32_t pattern_count);
bool ish_core_fn_set_pattern_locals_take(IshCore *fn, IshPatternLocal *locals, uint32_t local_count);
bool ish_core_fn_set_guard_take(IshCore *fn, IshCore *guard);
IshCore *ish_core_fn_multi(const char *name, IshSpan span);
bool ish_core_fn_multi_add_capture(IshCore *multi, IshCaptureKind kind, uint32_t index);
bool ish_core_fn_multi_add_clause_take(IshCore *multi, uint32_t arity, IshPattern **patterns, uint32_t pattern_count, IshPatternLocal *locals, uint32_t local_count, IshCore *guard, IshCore *body);
IshCore *ish_core_letrec(IshSpan span);
bool ish_core_letrec_add(IshCore *letrec, const char *name, uint32_t slot, IshCore *value);
bool ish_core_letrec_set_body(IshCore *letrec, IshCore *body);
void ish_core_letrec_set_global(IshCore *letrec);
IshCore *ish_core_global_ref(uint32_t id, IshSpan span);
IshCore *ish_core_receive(IshCore *receiver, IshCore *timeout, IshCore *timeout_body, IshSpan span);
IshCore *ish_core_raise(IshCore *value, IshSpan span);
IshCore *ish_core_raised(IshSpan span);
IshCore *ish_core_rescue(IshCore *body, IshCore *handler, IshSpan span);
IshCore *ish_core_ensure(IshCore *body, IshCore *cleanup, uint32_t tmp_slot, IshSpan span);
IshCore *ish_core_use_package(IshValue name, IshBytecodeModule *module, uint32_t init_fn, uint32_t *export_src, uint32_t *export_dst, size_t export_count, IshCore *cont, IshSpan span);
IshCore *ish_core_define_protocol(IshValue name, IshSpan span);
bool ish_core_define_protocol_add_method(IshCore *core, IshValue method, uint32_t arity, IshCore *default_fn);
IshCore *ish_core_extend_protocol(IshValue protocol, IshValue type, IshSpan span);
bool ish_core_extend_protocol_add_impl(IshCore *core, IshValue method, uint32_t arity, IshCore *impl_fn);
IshCore *ish_core_method_call(IshValue protocol, IshValue method, IshSpan span);
bool ish_core_method_call_add_arg(IshCore *core, IshCore *arg);
void ish_core_free(IshCore *core);
bool ish_core_compile_expression(IshCore *core, IshBytecodeModule *module, IshError *err);
bool ish_core_compile_function_body(IshCore *body, const char *name, uint32_t arity, IshBytecodeModule *module, uint32_t *out_function, IshError *err);
bool ish_core_compile_main(IshCore *core, IshBytecodeModule *module, uint32_t *out_function, IshError *err);
bool ish_core_dump(IshBuffer *buf, const IshCore *core);
const char *ish_primitive_name(IshPrimitive primitive);
size_t ish_primitive_count(void);
const IshPrimitiveInfo *ish_primitive_info(IshPrimitive primitive);

#endif
