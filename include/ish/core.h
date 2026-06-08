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
    ISH_CORE_LETREC
} IshCoreKind;

typedef enum {
    ISH_PRIM_ADD,
    ISH_PRIM_SUB,
    ISH_PRIM_MUL,
    ISH_PRIM_EQ,
    ISH_PRIM_LT
} IshPrimitive;

typedef struct IshCore IshCore;

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
            uint32_t *capture_slots;
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
            uint32_t *capture_slots;
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
        } letrec;
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
bool ish_core_fn_add_capture(IshCore *fn, uint32_t local_slot);
bool ish_core_fn_set_param_patterns_take(IshCore *fn, IshPattern **patterns, uint32_t pattern_count);
bool ish_core_fn_set_pattern_locals_take(IshCore *fn, IshPatternLocal *locals, uint32_t local_count);
bool ish_core_fn_set_guard_take(IshCore *fn, IshCore *guard);
IshCore *ish_core_fn_multi(const char *name, IshSpan span);
bool ish_core_fn_multi_add_capture(IshCore *multi, uint32_t local_slot);
bool ish_core_fn_multi_add_clause_take(IshCore *multi, uint32_t arity, IshPattern **patterns, uint32_t pattern_count, IshPatternLocal *locals, uint32_t local_count, IshCore *guard, IshCore *body);
IshCore *ish_core_letrec(IshSpan span);
bool ish_core_letrec_add(IshCore *letrec, const char *name, uint32_t slot, IshCore *value);
bool ish_core_letrec_set_body(IshCore *letrec, IshCore *body);
void ish_core_free(IshCore *core);
bool ish_core_compile_expression(IshCore *core, IshBytecodeModule *module, IshError *err);
bool ish_core_compile_main(IshCore *core, IshBytecodeModule *module, uint32_t *out_function, IshError *err);
bool ish_core_dump(IshBuffer *buf, const IshCore *core);
const char *ish_primitive_name(IshPrimitive primitive);

#endif
