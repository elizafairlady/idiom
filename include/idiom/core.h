#ifndef IDM_CORE_H
#define IDM_CORE_H

#include "idiom/bytecode.h"
#include "idiom/pattern.h"

typedef enum {
    IDM_CORE_LITERAL,
    IDM_CORE_ARG_REF,
    IDM_CORE_LOCAL_REF,
    IDM_CORE_CAPTURE_REF,
    IDM_CORE_CALL,
    IDM_CORE_LIST_CONS,
    IDM_CORE_LIST_APPEND,
    IDM_CORE_VALUE_SEQUENCE,
    IDM_CORE_SYNTAX_BUILD,
    IDM_CORE_STRING_CONCAT,
    IDM_CORE_COND,
    IDM_CORE_MATCH,
    IDM_CORE_DO,
    IDM_CORE_BIND_LOCAL,
    IDM_CORE_FN,
    IDM_CORE_FN_MULTI,
    IDM_CORE_LETREC,
    IDM_CORE_RECEIVE,
    IDM_CORE_GUARD,
    IDM_CORE_ENV_REF,
    IDM_CORE_PACKAGE_REF,
    IDM_CORE_USE_PACKAGE,
    IDM_CORE_RECORD_CONSTRUCT,
    IDM_CORE_RECORD_FIELD,
    IDM_CORE_RECORD_IS
} IdmCoreKind;

#define IDM_PRIMITIVE_LIST(X) \
    X(ADD, "add", 2, 2, "kernel") \
    X(SUB, "sub", 2, 2, "kernel") \
    X(MUL, "mul", 2, 2, "kernel") \
    X(DIV, "div", 2, 2, "kernel") \
    X(MOD, "mod", 2, 2, "kernel") \
    X(POW, "pow", 2, 2, "kernel") \
    X(NEG, "neg", 1, 1, "kernel") \
    X(EQ, "eq?", 2, 2, "kernel") \
    X(NEQ, "neq?", 2, 2, "kernel") \
    X(LT, "lt?", 2, 2, "kernel") \
    X(GT, "gt?", 2, 2, "kernel") \
    X(LTE, "lte?", 2, 2, "kernel") \
    X(GTE, "gte?", 2, 2, "kernel") \
    X(COND, "cond", 2, 3, "kernel") \
    X(OK, "ok?", 1, 1, "result") \
    X(CONS, "cons", 2, 2, "kernel") \
    X(FIRST, "first", 1, 1, "kernel") \
    X(REST, "rest", 1, 1, "kernel") \
    X(LIST, "list", 0, UINT32_MAX, "kernel") \
    X(TUPLE, "tuple", 0, UINT32_MAX, "kernel") \
    X(VECTOR, "vector", 0, UINT32_MAX, "kernel") \
    X(DICT, "dict", 0, UINT32_MAX, "kernel") \
    X(TUPLE_GET, "tuple-get", 2, 2, "kernel") \
    X(APPEND, "append", 2, 2, "kernel") \
    X(STR_TO_LIST, "str-to-list", 1, 1, "kernel") \
    X(DICT_TO_LIST, "dict-to-list", 1, 1, "kernel") \
    X(VECTOR_TO_LIST, "vector-to-list", 1, 1, "kernel") \
    X(TUPLE_TO_LIST, "tuple-to-list", 1, 1, "kernel") \
    X(APPLY, "apply", 2, 2, "kernel") \
    X(SYNTAX_KIND, "syntax-kind", 1, 1, "kernel") \
    X(SYNTAX_PROPERTY, "syntax-property", 2, 2, "kernel") \
    X(SYNTAX_SET_PROPERTY, "syntax-set-property", 3, 3, "kernel") \
    X(SYNTAX_ORIGIN, "syntax-origin", 1, 1, "kernel") \
    X(SYNTAX_LIST_PRED, "syntax-list?", 1, 1, "kernel") \
    X(SYNTAX_LENGTH, "syntax-length", 1, 1, "kernel") \
    X(SYNTAX_NTH, "syntax-nth", 2, 2, "kernel") \
    X(SYNTAX_SLICE, "syntax-slice", 3, 3, "kernel") \
    X(SYNTAX_WORD_PRED, "syntax-word?", 1, 1, "kernel") \
    X(SYNTAX_WORD_TEXT, "syntax-word-text", 1, 1, "kernel") \
    X(SYNTAX_ATOM_PRED, "syntax-atom?", 1, 1, "kernel") \
    X(SYNTAX_ATOM_TEXT, "syntax-atom-text", 1, 1, "kernel") \
    X(SYNTAX_INT_PRED, "syntax-int?", 1, 1, "kernel") \
    X(SYNTAX_INT_VALUE, "syntax-int-value", 1, 1, "kernel") \
    X(MAKE_SYNTAX_WORD, "make-syntax-word", 2, 2, "kernel") \
    X(MAKE_SYNTAX_ATOM, "make-syntax-atom", 2, 2, "kernel") \
    X(MAKE_SYNTAX_INT, "make-syntax-int", 2, 2, "kernel") \
    X(MAKE_SYNTAX_STRING, "make-syntax-string", 2, 2, "kernel") \
    X(MAKE_SYNTAX_LIST, "make-syntax-list", 2, 2, "kernel") \
    X(MAKE_SYNTAX_VECTOR, "make-syntax-vector", 2, 2, "kernel") \
    X(MAKE_SYNTAX_TUPLE, "make-syntax-tuple", 2, 2, "kernel") \
    X(MAKE_SYNTAX_DICT, "make-syntax-dict", 2, 2, "kernel") \
    X(MAKE_SYNTAX_EXPR, "make-syntax-expr", 2, 2, "kernel") \
    X(MAKE_SYNTAX_BODY, "make-syntax-body", 2, 2, "kernel") \
    X(MAKE_SYNTAX_GROUP, "make-syntax-group", 2, 2, "kernel") \
    X(SYNTAX_ERROR, "syntax-error", 2, 2, "kernel") \
    X(LOCAL_EXPAND, "local-expand", 1, 1, "kernel") \
    X(FREE_IDENTIFIER_EQ, "free-identifier=?", 2, 2, "kernel") \
    X(BOUND_IDENTIFIER_EQ, "bound-identifier=?", 2, 2, "kernel") \
    X(BIND_BANG, "bind!", 2, 2, "kernel") \
    X(SELF, "self", 0, 0, "kernel") \
    X(SPAWN, "spawn", 1, 1, "kernel") \
    X(SEND, "send", 2, 2, "kernel") \
    X(EXIT, "exit", 0, 2, "kernel") \
    X(LINK, "link", 1, 1, "kernel") \
    X(UNLINK, "unlink", 1, 1, "kernel") \
    X(MONITOR, "monitor", 1, 1, "kernel") \
    X(DEMONITOR, "demonitor", 1, 1, "kernel") \
    X(TRAP_EXIT, "trap-exit", 1, 1, "kernel") \
    X(STR, "str", 1, UINT32_MAX, "kernel") \
    X(CHOMP, "chomp", 1, 1, "string") \
    X(CAPTURE_STDOUT, "capture-stdout", 1, 1, "kernel") \
    X(EXEC, "exec", 1, 1, "kernel") \
    X(AWAIT, "await", 1, 1, "kernel") \
    X(PRINT, "print", 0, UINT32_MAX, "kernel") \
    X(PRINTLN, "println", 0, UINT32_MAX, "kernel") \
    X(CD, "cd", 1, 1, "system") \
    X(CHDIR, "chdir", 1, 1, "system") \
    X(PWD, "pwd", 0, 0, "system") \
    X(WRITE_PROCSUB_TEMP, "write-procsub-temp", 1, 1, "kernel") \
    X(MAKE_PROCSUB_TEMP, "make-procsub-temp", 0, 0, "kernel") \
    X(ENV_GET, "env-get", 1, 1, "system") \
    X(ENV_SET, "env-set", 2, 2, "system") \
    X(SYNTAX_ADJACENT_PRED, "syntax-adjacent?", 1, 1, "kernel") \
    X(SYNTAX_STRING_TEXT, "syntax-string-text", 1, 1, "kernel") \
    X(STR_CONTAINS, "contains?", 2, 2, "string") \
    X(INTERNAL_REGISTER_MACRO, "internal-register-macro", 2, 2, "") \
    X(EXPAND_CHECK, "expand-check", 1, 1, "compile") \
    X(INSPECT, "inspect", 1, 1, "kernel") \
    X(STR_LEN, "len", 1, 1, "string") \
    X(STR_SLICE, "slice", 3, 3, "string") \
    X(STR_FIND, "find", 3, 3, "string") \
    X(STR_BYTE, "byte", 2, 2, "string") \
    X(BYTE_STR, "byte-str", 1, 1, "string") \
    X(REGEX_COMPILE, "raw-compile", 2, 2, "regex") \
    X(REGEX_PRED, "raw-regex?", 1, 1, "regex") \
    X(REGEX_SOURCE, "raw-source", 1, 1, "regex") \
    X(REGEX_OPTIONS, "raw-options", 1, 1, "regex") \
    X(REGEX_GROUP_COUNT, "raw-group-count", 1, 1, "regex") \
    X(REGEX_GROUP_NAMES, "raw-group-names", 1, 1, "regex") \
    X(REGEX_RESULT_PRED, "raw-result?", 1, 1, "regex") \
    X(REGEX_SCAN_AT, "raw-scan-at", 3, 3, "regex") \
    X(REGEX_SCAN_FROM, "raw-scan-from", 3, 3, "regex") \
    X(REGEX_SCAN_FULL, "raw-scan-full", 2, 2, "regex") \
    X(REGEX_TEST, "raw-test?", 2, 2, "regex") \
    X(REGEX_RESULT_START, "raw-result-start", 1, 1, "regex") \
    X(REGEX_RESULT_END, "raw-result-end", 1, 1, "regex") \
    X(REGEX_RESULT_TEXT, "raw-result-text", 1, 1, "regex") \
    X(REGEX_CAPTURE, "raw-capture", 2, 2, "regex") \
    X(REGEX_CAPTURE_RANGE, "raw-capture-range", 2, 2, "regex") \
    X(REGEX_CAPTURE_NAMED, "raw-capture-named", 2, 2, "regex") \
    X(REGEX_CAPTURES, "raw-captures", 1, 1, "regex") \
    X(REGEX_SCAN_ALL, "raw-scan-all", 2, 2, "regex") \
    X(REGEX_REPLACE, "raw-replace", 3, 3, "regex") \
    X(REGEX_REPLACE_ALL, "raw-replace-all", 3, 3, "regex") \
    X(REGEX_SPLIT_ON, "raw-split-on", 2, 2, "regex") \
    X(REGEX_ESCAPE, "raw-escape", 1, 1, "regex") \
    X(FILE_READ, "read", 1, 1, "file") \
    X(FILE_WRITE, "write", 2, 2, "file") \
    X(FILE_EXISTS, "exists?", 1, 1, "file") \
    X(FILE_STAT, "stat", 1, 1, "file") \
    X(FILE_LIST, "list", 1, 1, "file") \
    X(FILE_REMOVE, "remove", 1, 1, "file") \
    X(ARGS, "args", 0, 0, "system") \
    X(TIME_MS, "time-ms", 0, 0, "system") \
    X(RANDOM, "random", 1, 1, "system") \
    X(DICT_GET, "dict-get", 3, 3, "kernel") \
    X(DICT_PUT, "dict-put", 3, 3, "kernel") \
    X(DICT_DEL, "dict-del", 2, 2, "kernel") \
    X(DICT_KEYS, "dict-keys", 1, 1, "kernel") \
    X(DICT_VALS, "dict-vals", 1, 1, "kernel") \
    X(DICT_HAS, "dict-has?", 2, 2, "kernel") \
    X(DICT_SIZE, "dict-size", 1, 1, "kernel") \
    X(ABS, "abs", 1, 1, "math") \
    X(FLOOR, "floor", 1, 1, "math") \
    X(ROUND, "round", 1, 1, "math") \
    X(SQRT, "sqrt", 1, 1, "math") \
    X(FLOOR_DIV, "floor-div", 2, 2, "kernel") \
    X(FLOOR_MOD, "floor-mod", 2, 2, "kernel") \
    X(PARSE_INT, "parse-int", 1, 1, "string") \
    X(PARSE_FLOAT, "parse-float", 1, 1, "string") \
    X(FILE_MKDIR, "mkdir", 1, 1, "file") \
    X(FILE_APPEND, "append", 2, 2, "file") \
    X(ORD_STR, "ord->str", 1, 1, "string") \
    X(STR_ORD, "str->ord", 1, 1, "string") \
    X(FROM_RUNES, "from-runes", 1, 1, "string") \
    X(REPL_COMPILE, "repl-compile", 1, 1, "kernel") \
    X(REPL_ABORT, "repl-abort", 1, 1, "kernel") \
    X(REPL_SPAWN, "repl-spawn", 1, 1, "kernel") \
    X(REPL_DIAGNOSTIC, "repl-diagnostic", 0, 0, "kernel") \
    X(ISH_SESSION, "ish-session", 0, 0, "kernel") \
    X(TTY_PRED, "tty?", 0, 0, "term") \
    X(TTY_RAW, "tty-raw!", 0, 0, "term") \
    X(TTY_RESTORE, "tty-restore!", 0, 0, "term") \
    X(TTY_READ, "tty-read", 1, 1, "term") \
    X(TTY_READ_LINE, "tty-read-line", 0, 0, "term") \
    X(TTY_WRITE, "tty-write", 1, 1, "term") \
    X(TTY_SIZE, "tty-size", 0, 0, "term") \
    X(EPRINTLN, "eprintln", 0, UINT32_MAX, "kernel") \
    X(PORT_STATUS, "port-status", 1, 1, "port") \
    X(JOB_RESUME, "job-resume", 2, 2, "kernel") \
    X(JOB_SIGNAL, "job-signal", 2, 2, "kernel") \
    X(ERROR_MESSAGE, "error-message", 1, 1, "kernel") \
    X(MAKE_ERROR, "make-error", 1, 1, "kernel") \
    X(SPAWN_LINK, "spawn-link", 1, 1, "kernel") \
    X(SPAWN_MONITOR, "spawn-monitor", 1, 1, "kernel") \
    X(PORT_READ, "port-read", 3, 3, "port") \
    X(PORT_WRITE, "port-write", 2, 2, "port") \
    X(PORT_CLOSE_INPUT, "port-close-input", 1, 1, "port") \
    X(RAISE, "raise", 1, 1, "kernel") \
    X(IS_A_P, "is-a?", 2, 2, "kernel") \
    X(NIL_P, "nil?", 1, 1, "kernel") \
    X(ATOM_P, "atom?", 1, 1, "kernel") \
    X(WORD_P, "word?", 1, 1, "kernel") \
    X(INT_P, "int?", 1, 1, "kernel") \
    X(FLOAT_P, "float?", 1, 1, "kernel") \
    X(STRING_P, "string?", 1, 1, "kernel") \
    X(PAIR_P, "pair?", 1, 1, "kernel") \
    X(EMPTY_LIST_P, "empty-list?", 1, 1, "kernel") \
    X(LIST_P, "list?", 1, 1, "kernel") \
    X(TUPLE_P, "tuple?", 1, 1, "kernel") \
    X(VECTOR_P, "vector?", 1, 1, "kernel") \
    X(DICT_P, "dict?", 1, 1, "kernel") \
    X(SYNTAX_P, "syntax?", 1, 1, "kernel") \
    X(CELL_P, "cell?", 1, 1, "kernel") \
    X(CLOSURE_P, "closure?", 1, 1, "kernel") \
    X(PID_P, "pid?", 1, 1, "kernel") \
    X(REF_P, "ref?", 1, 1, "kernel") \
    X(PORT_P, "port?", 1, 1, "kernel") \
    X(REGEX_P, "regex?", 1, 1, "kernel") \
    X(REGEX_RESULT_P, "regex-result?", 1, 1, "kernel") \
    X(COMPARE, "compare", 2, 2, "kernel") \
    X(CEIL, "ceil", 1, 1, "math") \
    X(TRUNCATE, "truncate", 1, 1, "math") \
    X(SIN, "sin", 1, 1, "math") \
    X(COS, "cos", 1, 1, "math") \
    X(TAN, "tan", 1, 1, "math") \
    X(ASIN, "asin", 1, 1, "math") \
    X(ACOS, "acos", 1, 1, "math") \
    X(ATAN, "atan", 1, 1, "math") \
    X(ATAN2, "atan2", 2, 2, "math") \
    X(EXP, "exp", 1, 1, "math") \
    X(LOG, "log", 1, 1, "math") \
    X(LOG2, "log2", 1, 1, "math") \
    X(LOG10, "log10", 1, 1, "math") \
    X(HYPOT, "hypot", 2, 2, "math") \
    X(NAN_P, "nan?", 1, 1, "math") \
    X(FINITE_P, "finite?", 1, 1, "math") \
    X(INFINITE_P, "infinite?", 1, 1, "math") \
    X(NAN, "nan", 0, 0, "math") \
    X(INF, "inf", 0, 0, "math") \
    X(DIVMOD, "divmod", 2, 2, "math") \
    X(BIT_AND, "bit-and", 2, 2, "math") \
    X(BIT_OR, "bit-or", 2, 2, "math") \
    X(BIT_XOR, "bit-xor", 2, 2, "math") \
    X(BIT_NOT, "bit-not", 1, 1, "math") \
    X(SHIFT_LEFT, "shift-left", 2, 2, "math") \
    X(SHIFT_RIGHT, "shift-right", 2, 2, "math") \
    X(BIT_COUNT, "bit-count", 1, 1, "math") \
    X(BIT_LENGTH, "bit-length", 1, 1, "math") \
    X(TO_INT, "to-int", 1, 1, "math") \
    X(TO_FLOAT, "to-float", 1, 1, "math") \
    X(FILE_OPEN, "open", 2, 2, "file") \
    X(SYNTAX_FLOAT_VALUE, "syntax-float-value", 1, 1, "kernel") \
    X(MAKE_SYNTAX_NIL, "make-syntax-nil", 1, 1, "kernel")

typedef enum {
#define IDM_PRIMITIVE_ENUM(id, name, min_arity, max_arity, home) IDM_PRIM_##id,
    IDM_PRIMITIVE_LIST(IDM_PRIMITIVE_ENUM)
#undef IDM_PRIMITIVE_ENUM
} IdmPrimitive;

typedef struct {
    const char *name;
    uint32_t min_arity;
    uint32_t max_arity;
} IdmPrimitiveInfo;

typedef struct IdmCore IdmCore;

typedef enum {
    IDM_CAP_LOCAL,
    IDM_CAP_ARG,
    IDM_CAP_UPVALUE
} IdmCaptureKind;

typedef struct {
    IdmCaptureKind kind;
    char *name;
    uint32_t index;
    bool celled;
} IdmCapture;

typedef struct {
    char *name;
    uint32_t slot;
} IdmCoreSlot;

typedef struct {
    char *name;
    IdmValue env_key;
    uint32_t slot;
} IdmCorePackageSlot;

typedef struct {
    uint32_t arity;
    IdmArity call_arity;
    IdmPattern **param_patterns;
    uint32_t pattern_count;
    IdmPatternLocal *pattern_locals;
    uint32_t pattern_local_count;
    IdmCore *guard;
    IdmCore *body;
    bool primitive_backed;
    IdmPrimitive primitive;
} IdmFnClause;

typedef struct {
    char *name;
    IdmValue env_key;
    bool has_env_key;
    uint32_t slot;
    IdmCore *value;
    bool fill_existing;
} IdmLetRecBinding;

struct IdmCore {
    IdmCoreKind kind;
    IdmSpan span;
    bool local_celled;
    union {
        IdmValue literal;
        IdmCoreSlot slot_ref;
        IdmCorePackageSlot package_ref;
        struct {
            IdmCore *callee;
            IdmCore **args;
            size_t arg_count;
            size_t arg_cap;
        } call;
        struct {
            IdmCore *head;
            IdmCore *tail;
        } list_pair;
        struct {
            IdmValueSequenceKind kind;
            IdmCore **items;
            size_t count;
            size_t cap;
        } value_sequence;
        struct {
            IdmSyntaxBuildKind kind;
            IdmCore *ctx;
            IdmCore *payload;
        } syntax_build;
        struct {
            IdmCore **items;
            size_t count;
            size_t cap;
        } string_concat;
        struct {
            IdmCore *cond;
            IdmCore *then_branch;
            IdmCore *else_branch;
        } cond_expr;
        struct {
            IdmCore **scrutinees;
            size_t scrutinee_count;
            size_t scrutinee_cap;
            IdmCapture *captures;
            size_t capture_count;
            size_t capture_cap;
            IdmFnClause *clauses;
            size_t count;
            size_t cap;
        } match_expr;
        struct {
            IdmCore **items;
            size_t count;
            size_t cap;
        } do_expr;
        struct {
            char *name;
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
            bool env;
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
            IdmValue env_key;
            IdmBytecodeModule *module;
            bool module_owned;
            uint32_t init_fn;
            IdmCore *cont;
        } use_package;
        struct {
            IdmSymbol *type;
            IdmSymbol **field_names;
            IdmSymbol **field_contracts;
            IdmCore **field_values;
            size_t count;
            size_t cap;
        } record_construct;
        struct {
            IdmCore *receiver;
            IdmSymbol *type;
            IdmSymbol *field;
            uint32_t field_index;
        } record_field;
        struct {
            IdmCore *value;
            IdmSymbol *type;
        } record_is;
    } as;
};

IdmCore *idm_core_literal(IdmValue value, IdmSpan span);
IdmCore *idm_core_arg_ref(const char *name, uint32_t slot, IdmSpan span);
IdmCore *idm_core_local_ref(const char *name, uint32_t slot, IdmSpan span);
IdmCore *idm_core_capture_ref(const char *name, uint32_t slot, IdmSpan span);
IdmCore *idm_core_primitive_backed_fn(const char *name, IdmPrimitive primitive, IdmArity arity, IdmSpan span);
IdmCore *idm_core_call(IdmCore *callee, IdmSpan span);
bool idm_core_call_add_arg(IdmCore *call, IdmCore *arg);
IdmCore *idm_core_list_cons(IdmCore *head, IdmCore *tail, IdmSpan span);
IdmCore *idm_core_list_append(IdmCore *head, IdmCore *tail, IdmSpan span);
IdmCore *idm_core_value_sequence(IdmValueSequenceKind kind, IdmSpan span);
bool idm_core_value_sequence_add(IdmCore *core, IdmCore *item);
IdmCore *idm_core_syntax_build(IdmSyntaxBuildKind kind, IdmCore *ctx, IdmCore *payload, IdmSpan span);
IdmCore *idm_core_string_concat(IdmSpan span);
bool idm_core_string_concat_add(IdmCore *core, IdmCore *item);
IdmCore *idm_core_cond(IdmCore *cond, IdmCore *then_branch, IdmCore *else_branch, IdmSpan span);
IdmCore *idm_core_match(IdmSpan span);
bool idm_core_match_add_scrutinee(IdmCore *match, IdmCore *scrutinee);
bool idm_core_match_add_capture(IdmCore *match, IdmCaptureKind kind, const char *name, uint32_t index);
bool idm_core_match_add_clause_take(IdmCore *match, uint32_t arity, IdmPattern **patterns, uint32_t pattern_count, IdmPatternLocal *locals, uint32_t local_count, IdmCore *guard, IdmCore *body);
IdmCore *idm_core_do(IdmSpan span);
bool idm_core_do_add(IdmCore *do_expr, IdmCore *item);
IdmCore *idm_core_bind_local(const char *name, uint32_t slot, IdmCore *value, IdmCore *body, IdmSpan span);
IdmCore *idm_core_fn(const char *name, uint32_t arity, IdmCore *body, IdmSpan span);
bool idm_core_fn_add_capture(IdmCore *fn, IdmCaptureKind kind, const char *name, uint32_t index);
bool idm_core_fn_set_param_patterns_take(IdmCore *fn, IdmPattern **patterns, uint32_t pattern_count);
bool idm_core_fn_set_pattern_locals_take(IdmCore *fn, IdmPatternLocal *locals, uint32_t local_count);
bool idm_core_fn_set_guard_take(IdmCore *fn, IdmCore *guard);
IdmCore *idm_core_fn_multi(const char *name, IdmSpan span);
bool idm_core_fn_multi_add_capture(IdmCore *multi, IdmCaptureKind kind, const char *name, uint32_t index);
bool idm_core_fn_multi_add_clause_take(IdmCore *multi, uint32_t arity, IdmPattern **patterns, uint32_t pattern_count, IdmPatternLocal *locals, uint32_t local_count, IdmCore *guard, IdmCore *body);
IdmCore *idm_core_letrec(IdmSpan span);
bool idm_core_letrec_add(IdmCore *letrec, const char *name, uint32_t slot, IdmCore *value);
bool idm_core_letrec_add_fill(IdmCore *letrec, const char *name, uint32_t slot, IdmCore *value, bool fill_existing);
bool idm_core_letrec_add_env_fill(IdmCore *letrec, const char *name, IdmValue env_key, uint32_t slot, IdmCore *value, bool fill_existing);
bool idm_core_letrec_set_body(IdmCore *letrec, IdmCore *body);
void idm_core_letrec_set_env(IdmCore *letrec);
void idm_core_letrec_set_fill_only(IdmCore *letrec);
IdmCore *idm_core_env_ref(const char *name, uint32_t id, IdmSpan span);
IdmCore *idm_core_package_ref(const char *name, IdmValue env_key, uint32_t slot, IdmSpan span);
IdmCore *idm_core_receive(IdmCore *receiver, IdmCore *timeout, IdmCore *timeout_body, IdmSpan span);
IdmCore *idm_core_guard(IdmCore *body, IdmCore *handler, uint32_t rescue_slot, IdmCore *cleanup, uint32_t ensure_slot, IdmSpan span);
IdmCore *idm_core_use_package(IdmValue env_key, IdmBytecodeModule *module, bool module_owned, uint32_t init_fn, IdmCore *cont, IdmSpan span);
IdmCore *idm_core_record_construct(IdmSymbol *type, IdmSpan span);
bool idm_core_record_construct_add(IdmCore *core, IdmSymbol *field, IdmSymbol *contract, IdmCore *value);
IdmCore *idm_core_record_field(IdmCore *receiver, IdmSymbol *type, IdmSymbol *field, uint32_t field_index, IdmSpan span);
IdmCore *idm_core_record_is(IdmCore *value, IdmSymbol *type, IdmSpan span);
void idm_core_free(IdmCore *core);
bool idm_core_normalize(IdmRuntime *rt, IdmCore **core, IdmError *err);
bool idm_core_compile_expression(IdmRuntime *rt, IdmCore *core, IdmBytecodeModule *module, IdmError *err);
bool idm_core_compile_function(IdmRuntime *rt, IdmCore *fn, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err);
bool idm_core_compile_function_body(IdmRuntime *rt, IdmCore *body, const char *name, uint32_t arity, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err);
bool idm_core_compile_main(IdmRuntime *rt, IdmCore *core, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err);
bool idm_core_dump(IdmBuffer *buf, const IdmCore *core);
bool idm_core_dump_pretty(IdmBuffer *buf, const IdmCore *core);
const char *idm_primitive_name(IdmPrimitive primitive);
bool idm_checked_add(int64_t a, int64_t b, int64_t *out);
bool idm_checked_sub(int64_t a, int64_t b, int64_t *out);
bool idm_checked_mul(int64_t a, int64_t b, int64_t *out);
bool idm_checked_pow(int64_t base, int64_t exponent, int64_t *out);
size_t idm_primitive_count(void);
const IdmPrimitiveInfo *idm_primitive_info(IdmPrimitive primitive);
IdmArity idm_primitive_arity(IdmPrimitive primitive);
const char *idm_primitive_home(IdmPrimitive primitive);
bool idm_primitive_home_exists(const char *home);
bool idm_primitive_lookup(const char *home, const char *name, IdmPrimitive *out);

#endif
