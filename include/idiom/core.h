#ifndef IDM_CORE_H
#define IDM_CORE_H

#include "idiom/bytecode.h"
#include "idiom/pattern.h"
#include "idiom/scope.h"

typedef enum {
    IDM_CORE_LITERAL,
    IDM_CORE_ARG_REF,
    IDM_CORE_LOCAL_REF,
    IDM_CORE_CAPTURE_REF,
    IDM_CORE_CALL,
    IDM_CORE_LIST_CONS,
    IDM_CORE_LIST_APPEND,
    IDM_CORE_VALUE_SEQUENCE,
    IDM_CORE_FORM_BUILD,
    IDM_CORE_STRING_CONCAT,
    IDM_CORE_COND,
    IDM_CORE_MATCH,
    IDM_CORE_DO,
    IDM_CORE_BIND_LOCAL,
    IDM_CORE_MATCH_BIND,
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
    IDM_CORE_RECORD_IS,
    IDM_CORE_DISPATCH
} IdmCoreKind;

typedef enum {
    IDM_DISPATCH_METHOD,
    IDM_DISPATCH_FIELD,
    IDM_DISPATCH_IMPLEMENTS
} IdmDispatchKind;

typedef enum {
    IDM_DISPATCH_REF_OK,
    IDM_DISPATCH_REF_NONE,
    IDM_DISPATCH_REF_INVISIBLE
} IdmDispatchRefState;

typedef enum {
    IDM_DISPATCH_ROUTE_NONE,
    IDM_DISPATCH_ROUTE_DEVIRT,
    IDM_DISPATCH_ROUTE_EVIDENCE,
    IDM_DISPATCH_ROUTE_FIELD_STATIC,
    IDM_DISPATCH_ROUTE_FALLBACK,
    IDM_DISPATCH_ROUTE_FOLD_TRUE,
    IDM_DISPATCH_ROUTE_FOLD_FALSE,
    IDM_DISPATCH_ROUTE_DYNAMIC
} IdmDispatchRoute;

#define IDM_PRIMITIVE_LIST(X) \
    X(ADD, "add", 2, 2, "kernel", 1, 0) \
    X(SUB, "sub", 2, 2, "kernel", 1, 0) \
    X(MUL, "mul", 2, 2, "kernel", 1, 0) \
    X(DIV, "div", 2, 2, "kernel", 1, 0) \
    X(MOD, "mod", 2, 2, "kernel", 1, 0) \
    X(POW, "pow", 2, 2, "kernel", 1, 0) \
    X(NEG, "neg", 1, 1, "kernel", 1, 0) \
    X(EQ, "eq?", 2, 2, "kernel", 1, 1) \
    X(NEQ, "neq?", 2, 2, "kernel", 1, 1) \
    X(EQUAL, "equal?", 2, 2, "kernel", 1, 1) \
    X(NOT_EQUAL, "not-equal?", 2, 2, "kernel", 1, 1) \
    X(LT, "lt?", 2, 2, "kernel", 1, 0) \
    X(GT, "gt?", 2, 2, "kernel", 1, 0) \
    X(LTE, "lte?", 2, 2, "kernel", 1, 0) \
    X(GTE, "gte?", 2, 2, "kernel", 1, 0) \
    X(COND, "cond", 2, 3, "kernel", 1, 1) \
    X(OK, "ok?", 1, 1, "result", 1, 1) \
    X(CONS, "cons", 2, 2, "kernel", 1, 0) \
    X(FIRST, "first", 1, 1, "kernel", 1, 0) \
    X(REST, "rest", 1, 1, "kernel", 1, 0) \
    X(LIST, "list", 0, UINT32_MAX, "kernel", 1, 0) \
    X(TUPLE, "tuple", 0, UINT32_MAX, "kernel", 1, 0) \
    X(VECTOR, "vector", 0, UINT32_MAX, "kernel", 1, 0) \
    X(DICT, "dict", 0, UINT32_MAX, "kernel", 1, 0) \
    X(TUPLE_GET, "tuple-get", 2, 2, "kernel", 1, 0) \
    X(APPEND, "append", 2, 2, "kernel", 1, 0) \
    X(STR_TO_LIST, "runes-lossy", 1, 1, "string", 1, 0) \
    X(DICT_TO_LIST, "dict-to-list", 1, 1, "kernel", 1, 0) \
    X(SEQ_COUNT, "seq-count", 1, 1, "kernel", 1, 0) \
    X(SEQ_NTH, "seq-nth", 2, 2, "kernel", 1, 0) \
    X(APPLY, "apply", 2, 2, "kernel", 0, 0) \
    X(SYNTAX_KIND, "syntax-kind", 1, 1, "kernel", 1, 0) \
    X(SYNTAX_PROPERTY, "syntax-property", 2, 2, "kernel", 1, 0) \
    X(SYNTAX_SET_PROPERTY, "syntax-set-property", 3, 3, "kernel", 1, 0) \
    X(SYNTAX_ORIGIN, "syntax-origin", 1, 1, "kernel", 1, 0) \
    X(SYNTAX_LIST_PRED, "syntax-list?", 1, 1, "kernel", 1, 0) \
    X(SYNTAX_LENGTH, "syntax-length", 1, 1, "kernel", 1, 0) \
    X(SYNTAX_NTH, "syntax-nth", 2, 2, "kernel", 1, 0) \
    X(SYNTAX_SLICE, "syntax-slice", 3, 3, "kernel", 1, 0) \
    X(SYNTAX_WORD_PRED, "syntax-word?", 1, 1, "kernel", 1, 0) \
    X(SYNTAX_WORD_TEXT, "syntax-word-text", 1, 1, "kernel", 1, 0) \
    X(SYNTAX_ATOM_PRED, "syntax-atom?", 1, 1, "kernel", 1, 0) \
    X(SYNTAX_ATOM_TEXT, "syntax-atom-text", 1, 1, "kernel", 1, 0) \
    X(SYNTAX_INT_PRED, "syntax-int?", 1, 1, "kernel", 1, 0) \
    X(SYNTAX_INT_VALUE, "syntax-int-value", 1, 1, "kernel", 1, 0) \
    X(MAKE_SYNTAX_WORD, "make-syntax-word", 2, 2, "kernel", 1, 0) \
    X(MAKE_SYNTAX_ATOM, "make-syntax-atom", 2, 2, "kernel", 1, 0) \
    X(MAKE_SYNTAX_INT, "make-syntax-int", 2, 2, "kernel", 1, 0) \
    X(MAKE_SYNTAX_STRING, "make-syntax-string", 2, 2, "kernel", 1, 0) \
    X(MAKE_SYNTAX_LIST, "make-syntax-list", 2, 2, "kernel", 1, 0) \
    X(MAKE_SYNTAX_VECTOR, "make-syntax-vector", 2, 2, "kernel", 1, 0) \
    X(MAKE_SYNTAX_TUPLE, "make-syntax-tuple", 2, 2, "kernel", 1, 0) \
    X(MAKE_SYNTAX_DICT, "make-syntax-dict", 2, 2, "kernel", 1, 0) \
    X(MAKE_SYNTAX_EXPR, "make-syntax-expr", 2, 2, "kernel", 1, 0) \
    X(MAKE_SYNTAX_BODY, "make-syntax-body", 2, 2, "kernel", 1, 0) \
    X(MAKE_SYNTAX_GROUP, "make-syntax-group", 2, 2, "kernel", 1, 0) \
    X(SYNTAX_ERROR, "syntax-error", 2, 2, "kernel", 0, 0) \
    X(LOCAL_EXPAND, "local-expand", 1, 1, "kernel", 0, 0) \
    X(FREE_IDENTIFIER_EQ, "free-identifier=?", 2, 2, "kernel", 1, 0) \
    X(IDENTIFIER_BOUND, "identifier-bound?", 1, 1, "kernel", 0, 0) \
    X(BOUND_IDENTIFIER_EQ, "bound-identifier=?", 2, 2, "kernel", 1, 0) \
    X(BIND_BANG, "bind!", 2, 2, "kernel", 0, 0) \
    X(SELF, "self", 0, 0, "kernel", 0, 0) \
    X(SPAWN, "spawn", 1, 1, "kernel", 0, 0) \
    X(SEND, "send", 2, 2, "kernel", 0, 0) \
    X(EXIT, "exit", 0, 2, "kernel", 0, 0) \
    X(LINK, "link", 1, 1, "kernel", 0, 0) \
    X(UNLINK, "unlink", 1, 1, "kernel", 0, 0) \
    X(MONITOR, "monitor", 1, 1, "kernel", 0, 0) \
    X(DEMONITOR, "demonitor", 1, 1, "kernel", 0, 0) \
    X(TRAP_EXIT, "trap-exit", 1, 1, "kernel", 0, 0) \
    X(STR, "str", 1, UINT32_MAX, "kernel", 1, 0) \
    X(CHOMP, "chomp", 1, 1, "string", 1, 0) \
    X(PRINT, "print", 0, UINT32_MAX, "kernel", 0, 0) \
    X(PRINTLN, "println", 0, UINT32_MAX, "kernel", 0, 0) \
    X(CD, "cd", 1, 1, "system", 0, 0) \
    X(CHDIR, "chdir", 1, 1, "system", 0, 0) \
    X(PWD, "pwd", 0, 0, "system", 0, 0) \
    X(ENV_GET, "env-get", 1, 1, "system", 0, 0) \
    X(ENV_SET, "env-set", 2, 2, "system", 0, 0) \
    X(SYNTAX_ADJACENT_PRED, "syntax-adjacent?", 1, 1, "kernel", 1, 0) \
    X(SYNTAX_STRING_TEXT, "syntax-string-text", 1, 1, "kernel", 1, 0) \
    X(STR_CONTAINS, "contains?", 2, 2, "string", 1, 0) \
    X(EXPAND_CHECK, "expand-check", 1, 1, "compile", 0, 0) \
    X(SELECTOR_INFO, "selector-info", 1, 1, "compile", 1, 0) \
    X(PROCESSES, "processes", 0, 0, "kernel", 0, 0) \
    X(PROCESS_INFO, "process-info", 1, 1, "kernel", 0, 0) \
    X(PORT_INFO, "port-info", 1, 1, "port", 0, 0) \
    X(PORT_RESIZE, "port-resize", 3, 3, "port", 0, 0) \
    X(INSPECT, "inspect", 1, 1, "kernel", 1, 0) \
    X(JSON_STRING, "json-string", 1, 1, "string", 1, 0) \
    X(STR_LEN, "byte-len", 1, 1, "string", 1, 0) \
    X(STR_SLICE, "byte-slice", 3, 3, "string", 1, 0) \
    X(STR_FIND, "byte-find", 3, 3, "string", 1, 0) \
    X(STR_FIND_RANGE, "byte-find-range", 4, 4, "string", 1, 0) \
    X(STR_BYTE, "byte", 2, 2, "string", 1, 0) \
    X(BYTE_STR, "byte-str", 1, 1, "string", 1, 0) \
    X(STR_LINE_OF, "line-of", 2, 2, "string", 1, 0) \
    X(STR_LINE_START, "line-start", 2, 2, "string", 1, 0) \
    X(REGEX_COMPILE, "raw-compile", 2, 2, "regex", 1, 0) \
    X(REGEX_PRED, "raw-regex?", 1, 1, "regex", 1, 1) \
    X(REGEX_SOURCE, "raw-source", 1, 1, "regex", 1, 0) \
    X(REGEX_OPTIONS, "raw-options", 1, 1, "regex", 1, 0) \
    X(REGEX_GROUP_COUNT, "raw-group-count", 1, 1, "regex", 1, 0) \
    X(REGEX_GROUP_NAMES, "raw-group-names", 1, 1, "regex", 1, 0) \
    X(REGEX_RESULT_PRED, "raw-result?", 1, 1, "regex", 1, 1) \
    X(REGEX_SCAN_AT, "raw-scan-at", 3, 3, "regex", 1, 0) \
    X(REGEX_SCAN_FROM, "raw-scan-from", 3, 3, "regex", 1, 0) \
    X(REGEX_SCAN_FULL, "raw-scan-full", 2, 2, "regex", 1, 0) \
    X(REGEX_TEST, "raw-test?", 2, 2, "regex", 1, 0) \
    X(REGEX_RESULT_START, "raw-result-start", 1, 1, "regex", 1, 0) \
    X(REGEX_RESULT_END, "raw-result-end", 1, 1, "regex", 1, 0) \
    X(REGEX_RESULT_TEXT, "raw-result-text", 1, 1, "regex", 1, 0) \
    X(REGEX_CAPTURE, "raw-capture", 2, 2, "regex", 1, 0) \
    X(REGEX_CAPTURE_RANGE, "raw-capture-range", 2, 2, "regex", 1, 0) \
    X(REGEX_CAPTURE_NAMED, "raw-capture-named", 2, 2, "regex", 1, 0) \
    X(REGEX_CAPTURES, "raw-captures", 1, 1, "regex", 1, 0) \
    X(REGEX_SCAN_ALL, "raw-scan-all", 2, 2, "regex", 1, 0) \
    X(REGEX_REPLACE, "raw-replace", 3, 3, "regex", 1, 0) \
    X(REGEX_REPLACE_ALL, "raw-replace-all", 3, 3, "regex", 1, 0) \
    X(REGEX_SPLIT_ON, "raw-split-on", 2, 2, "regex", 1, 0) \
    X(REGEX_ESCAPE, "raw-escape", 1, 1, "regex", 1, 0) \
    X(FILE_READ, "read", 1, 1, "file", 0, 0) \
    X(FILE_WRITE, "write", 2, 2, "file", 0, 0) \
    X(FILE_EXISTS, "exists?", 1, 1, "file", 0, 0) \
    X(FILE_STAT, "stat", 1, 1, "file", 0, 0) \
    X(FILE_LIST, "list", 1, 1, "file", 0, 0) \
    X(FILE_REMOVE, "remove", 1, 1, "file", 0, 0) \
    X(ARGS, "args", 0, 0, "system", 0, 0) \
    X(TIME_MS, "time-ms", 0, 0, "system", 0, 0) \
    X(TIME_NS, "time-ns", 0, 0, "system", 0, 0) \
    X(RANDOM, "random", 1, 1, "system", 0, 0) \
    X(TOTAL_ALLOCS, "total-allocs", 0, 0, "system", 0, 0) \
    X(TOTAL_ALLOC_BYTES, "total-alloc-bytes", 0, 0, "system", 0, 0) \
    X(DICT_GET, "dict-get", 3, 3, "kernel", 1, 0) \
    X(DICT_PUT, "dict-put", 3, 3, "kernel", 1, 0) \
    X(DICT_DEL, "dict-del", 2, 2, "kernel", 1, 0) \
    X(DICT_KEYS, "dict-keys", 1, 1, "kernel", 1, 0) \
    X(DICT_VALS, "dict-vals", 1, 1, "kernel", 1, 0) \
    X(DICT_HAS, "dict-has?", 2, 2, "kernel", 1, 0) \
    X(RECORD_UPDATE, "record-update", 2, 2, "kernel", 1, 0) \
    X(HELP, "help", 1, 1, "kernel", 1, 0) \
    X(DICT_SIZE, "dict-size", 1, 1, "kernel", 1, 0) \
    X(ABS, "abs", 1, 1, "math", 1, 0) \
    X(FLOOR, "floor", 1, 1, "math", 1, 0) \
    X(ROUND, "round", 1, 1, "math", 1, 0) \
    X(SQRT, "sqrt", 1, 1, "math", 1, 0) \
    X(FLOOR_DIV, "floor-div", 2, 2, "kernel", 1, 0) \
    X(FLOOR_MOD, "floor-mod", 2, 2, "kernel", 1, 0) \
    X(PARSE_INT, "parse-int", 1, 1, "string", 1, 0) \
    X(PARSE_FLOAT, "parse-float", 1, 1, "string", 1, 0) \
    X(FILE_MKDIR, "mkdir", 1, 1, "file", 0, 0) \
    X(FILE_APPEND, "append", 2, 2, "file", 0, 0) \
    X(ORD_STR, "rune-str", 1, 1, "string", 1, 0) \
    X(FROM_RUNES, "from-runes", 1, 1, "string", 1, 0) \
    X(COMPILE, "compile", 1, 1, "compile", 0, 0) \
    X(ABORT, "abort", 1, 1, "compile", 0, 0) \
    X(SESSION, "session", 0, 0, "kernel", 0, 0) \
    X(TTY_PRED, "tty?", 0, 0, "term", 0, 0) \
    X(TTY_RAW, "tty-raw!", 0, 0, "term", 0, 0) \
    X(TTY_RESTORE, "tty-restore!", 0, 0, "term", 0, 0) \
    X(TTY_READ, "tty-read", 1, 1, "term", 0, 0) \
    X(TTY_READ_LINE, "tty-read-line", 0, 0, "term", 0, 0) \
    X(TTY_WRITE, "tty-write", 1, 1, "term", 0, 0) \
    X(TTY_SIZE, "tty-size", 0, 0, "term", 0, 0) \
    X(EPRINTLN, "eprintln", 0, UINT32_MAX, "kernel", 0, 0) \
    X(PORT_STATUS, "port-status", 1, 1, "port", 0, 0) \
    X(JOB_RESUME, "job-resume", 2, 2, "kernel", 0, 0) \
    X(JOB_SIGNAL, "job-signal", 2, 2, "kernel", 0, 0) \
    X(ERROR_MESSAGE, "error-message", 1, 1, "kernel", 1, 0) \
    X(MAKE_ERROR, "make-error", 1, 1, "kernel", 1, 0) \
    X(SPAWN_LINK, "spawn-link", 1, 1, "kernel", 0, 0) \
    X(SPAWN_MONITOR, "spawn-monitor", 1, 1, "kernel", 0, 0) \
    X(PORT_READ, "port-read", 3, 3, "port", 0, 0) \
    X(PORT_WRITE, "port-write", 2, 2, "port", 0, 0) \
    X(PORT_CLOSE_INPUT, "port-close-input", 1, 1, "port", 0, 0) \
    X(RAISE, "raise", 1, 1, "kernel", 0, 0) \
    X(IS_A_P, "is-a?", 2, 2, "kernel", 1, 0) \
    X(NIL_P, "nil?", 1, 1, "kernel", 1, 1) \
    X(ATOM_P, "atom?", 1, 1, "kernel", 1, 1) \
    X(WORD_P, "word?", 1, 1, "kernel", 1, 1) \
    X(INT_P, "int?", 1, 1, "kernel", 1, 1) \
    X(FLOAT_P, "float?", 1, 1, "kernel", 1, 1) \
    X(STRING_P, "string?", 1, 1, "kernel", 1, 1) \
    X(PAIR_P, "pair?", 1, 1, "kernel", 1, 1) \
    X(EMPTY_LIST_P, "empty-list?", 1, 1, "kernel", 1, 1) \
    X(LIST_P, "list?", 1, 1, "kernel", 1, 1) \
    X(TUPLE_P, "tuple?", 1, 1, "kernel", 1, 1) \
    X(VECTOR_P, "vector?", 1, 1, "kernel", 1, 1) \
    X(DICT_P, "dict?", 1, 1, "kernel", 1, 1) \
    X(SYNTAX_P, "syntax?", 1, 1, "kernel", 1, 1) \
    X(CELL_P, "cell?", 1, 1, "kernel", 1, 1) \
    X(CLOSURE_P, "closure?", 1, 1, "kernel", 1, 1) \
    X(PID_P, "pid?", 1, 1, "kernel", 1, 1) \
    X(REF_P, "ref?", 1, 1, "kernel", 1, 1) \
    X(PORT_P, "port?", 1, 1, "kernel", 1, 1) \
    X(REGEX_P, "regex?", 1, 1, "kernel", 1, 1) \
    X(REGEX_RESULT_P, "regex-result?", 1, 1, "kernel", 1, 1) \
    X(COMPARE, "compare", 2, 2, "kernel", 1, 1) \
    X(CEIL, "ceil", 1, 1, "math", 1, 0) \
    X(TRUNCATE, "truncate", 1, 1, "math", 1, 0) \
    X(SIN, "sin", 1, 1, "math", 1, 0) \
    X(COS, "cos", 1, 1, "math", 1, 0) \
    X(TAN, "tan", 1, 1, "math", 1, 0) \
    X(ASIN, "asin", 1, 1, "math", 1, 0) \
    X(ACOS, "acos", 1, 1, "math", 1, 0) \
    X(ATAN, "atan", 1, 1, "math", 1, 0) \
    X(ATAN2, "atan2", 2, 2, "math", 1, 0) \
    X(EXP, "exp", 1, 1, "math", 1, 0) \
    X(LOG, "log", 1, 1, "math", 1, 0) \
    X(LOG2, "log2", 1, 1, "math", 1, 0) \
    X(LOG10, "log10", 1, 1, "math", 1, 0) \
    X(HYPOT, "hypot", 2, 2, "math", 1, 0) \
    X(NAN_P, "nan?", 1, 1, "math", 1, 0) \
    X(FINITE_P, "finite?", 1, 1, "math", 1, 0) \
    X(INFINITE_P, "infinite?", 1, 1, "math", 1, 0) \
    X(NAN, "nan", 0, 0, "math", 1, 1) \
    X(INF, "inf", 0, 0, "math", 1, 1) \
    X(DIVMOD, "divmod", 2, 2, "math", 1, 0) \
    X(BIT_AND, "bit-and", 2, 2, "math", 1, 0) \
    X(BIT_OR, "bit-or", 2, 2, "math", 1, 0) \
    X(BIT_XOR, "bit-xor", 2, 2, "math", 1, 0) \
    X(BIT_NOT, "bit-not", 1, 1, "math", 1, 0) \
    X(SHIFT_LEFT, "shift-left", 2, 2, "math", 1, 0) \
    X(SHIFT_RIGHT, "shift-right", 2, 2, "math", 1, 0) \
    X(BIT_COUNT, "bit-count", 1, 1, "math", 1, 0) \
    X(BIT_LENGTH, "bit-length", 1, 1, "math", 1, 0) \
    X(TO_INT, "to-int", 1, 1, "math", 1, 0) \
    X(TO_FLOAT, "to-float", 1, 1, "math", 1, 0) \
    X(FILE_OPEN, "open", 2, 2, "file", 0, 0) \
    X(SYNTAX_FLOAT_VALUE, "syntax-float-value", 1, 1, "kernel", 1, 0) \
    X(MAKE_SYNTAX_NIL, "make-syntax-nil", 1, 1, "kernel", 1, 0) \
    X(BITS_P, "bits?", 1, 1, "kernel", 1, 1) \
    X(BITS_LEN, "bits-len", 1, 1, "kernel", 1, 0) \
    X(BITS_SLICE, "bits-slice", 3, 3, "kernel", 1, 0) \
    X(BITS_INT, "bits-int", 4, 4, "kernel", 1, 0) \
    X(BITS_FLOAT, "bits-float", 4, 4, "kernel", 1, 0) \
    X(BITS_OF_INT, "bits-of-int", 3, 3, "kernel", 1, 0) \
    X(BITS_OF_FLOAT, "bits-of-float", 3, 3, "kernel", 1, 0) \
    X(BITS_APPEND, "bits-append", 2, 2, "kernel", 1, 0) \
    X(STRING_BITS, "string-bits", 1, 1, "kernel", 1, 0) \
    X(BITS_STRING, "bits-string", 1, 1, "kernel", 1, 0)

typedef enum {
#define IDM_PRIMITIVE_ENUM(id, name, min_arity, max_arity, home, pure, total) IDM_PRIM_##id,
    IDM_PRIMITIVE_LIST(IDM_PRIMITIVE_ENUM)
#undef IDM_PRIMITIVE_ENUM
    IDM_PRIM_COUNT
} IdmPrimitive;

typedef struct {
    const char *name;
    uint32_t min_arity;
    uint32_t max_arity;
    bool pure;
    bool total;
} IdmPrimitiveInfo;

extern const IdmPrimitiveInfo idm_primitive_infos[IDM_PRIM_COUNT];

static inline const IdmPrimitiveInfo *idm_primitive_info(IdmPrimitive primitive) {
    if ((size_t)primitive >= (size_t)IDM_PRIM_COUNT || !idm_primitive_infos[(size_t)primitive].name) return NULL;
    return &idm_primitive_infos[(size_t)primitive];
}

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
    IdmBindingId binding_id;
    bool celled;
} IdmCapture;

typedef struct {
    char *name;
    uint32_t slot;
    bool has_contract;
    IdmCallableContract contract;
} IdmCoreSlot;

typedef struct {
    char *name;
    IdmValue env_key;
    uint32_t slot;
    bool has_contract;
    IdmCallableContract contract;
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

typedef struct {
    IdmSymbol *trait;
    IdmCore *evidence;
    uint8_t evidence_state;
} IdmDispatchMethodDef;

typedef struct {
    uint32_t method;
    IdmSymbol *type;
    bool structural;
    IdmStructuralHead structural_head;
    IdmArity arity;
    bool passthrough;
    uint32_t primitive;
    IdmCore *ref;
    uint8_t ref_state;
} IdmDispatchImplDef;

typedef struct {
    IdmSymbol *type;
    IdmSymbol *field;
    uint32_t field_index;
    bool has_contract;
    IdmTypeTerm contract;
} IdmDispatchFieldDef;

struct IdmCore {
    IdmCoreKind kind;
    IdmSpan span;
    bool local_celled;
    bool lowered;
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
            IdmCore *pattern_fn;
            IdmCore *value;
            IdmCore *body;
            uint32_t first_slot;
            uint32_t count;
        } match_bind;
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
            char *doc;
        } fn;
        struct {
            char *name;
            IdmCapture *captures;
            size_t capture_count;
            size_t capture_cap;
            IdmFnClause *clauses;
            size_t count;
            size_t cap;
            char *doc;
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
            IdmTypeTerm *field_contracts;
            bool *field_has_contracts;
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
        struct {
            IdmDispatchKind kind;
            char *name;
            IdmSymbol *identity;
            IdmCore **args;
            size_t arg_count;
            size_t arg_cap;
            IdmDispatchMethodDef *methods;
            size_t method_count;
            IdmDispatchImplDef *impls;
            size_t impl_count;
            IdmDispatchFieldDef *fields;
            size_t field_count;
            IdmCore *fallback;
            uint8_t fallback_state;
            uint8_t route;
            uint32_t route_index;
        } dispatch;
    } as;
};

uint32_t idm_core_max_local_plus_one(IdmCore *core);
IdmCore *idm_core_literal(IdmValue value, IdmSpan span);
IdmCore *idm_core_arg_ref(const char *name, uint32_t slot, IdmSpan span);
IdmCore *idm_core_local_ref(const char *name, uint32_t slot, IdmSpan span);
IdmCore *idm_core_capture_ref(const char *name, uint32_t slot, IdmSpan span);
bool idm_core_ref_set_contract(IdmCore *core, const IdmCallableContract *contract);
IdmCore *idm_core_primitive_backed_fn(const char *name, IdmPrimitive primitive, IdmArity arity, IdmSpan span);
IdmCore *idm_core_call(IdmCore *callee, IdmSpan span);
bool idm_core_call_add_arg(IdmCore *call, IdmCore *arg);
IdmCore *idm_core_list_cons(IdmCore *head, IdmCore *tail, IdmSpan span);
IdmCore *idm_core_list_append(IdmCore *head, IdmCore *tail, IdmSpan span);
IdmCore *idm_core_value_sequence(IdmValueSequenceKind kind, IdmSpan span);
bool idm_core_value_sequence_add(IdmCore *core, IdmCore *item);
IdmCore *idm_core_form_build(IdmSyntaxBuildKind kind, IdmCore *ctx, IdmCore *payload, IdmSpan span);
IdmCore *idm_core_string_concat(IdmSpan span);
bool idm_core_string_concat_add(IdmCore *core, IdmCore *item);
IdmCore *idm_core_cond(IdmCore *cond, IdmCore *then_branch, IdmCore *else_branch, IdmSpan span);
IdmCore *idm_core_match(IdmSpan span);
bool idm_core_match_add_scrutinee(IdmCore *match, IdmCore *scrutinee);
bool idm_core_match_add_capture(IdmCore *match, IdmCaptureKind kind, const char *name, uint32_t index, IdmBindingId binding_id);
bool idm_core_match_add_clause_take(IdmCore *match, uint32_t arity, IdmPattern **patterns, uint32_t pattern_count, IdmPatternLocal *locals, uint32_t local_count, IdmCore *guard, IdmCore *body);
IdmCore *idm_core_do(IdmSpan span);
bool idm_core_do_add(IdmCore *do_expr, IdmCore *item);
IdmCore *idm_core_bind_local(const char *name, uint32_t slot, IdmCore *value, IdmCore *body, IdmSpan span);
IdmCore *idm_core_match_bind(IdmCore *pattern_fn, IdmCore *value, IdmCore *body, uint32_t first_slot, uint32_t count, IdmSpan span);
IdmCore *idm_core_fn(const char *name, uint32_t arity, IdmCore *body, IdmSpan span);
bool idm_core_fn_add_capture(IdmCore *fn, IdmCaptureKind kind, const char *name, uint32_t index, IdmBindingId binding_id);
bool idm_core_fn_set_param_patterns_take(IdmCore *fn, IdmPattern **patterns, uint32_t pattern_count);
bool idm_core_fn_set_pattern_locals_take(IdmCore *fn, IdmPatternLocal *locals, uint32_t local_count);
bool idm_core_fn_set_guard_take(IdmCore *fn, IdmCore *guard);
IdmCore *idm_core_fn_multi(const char *name, IdmSpan span);
bool idm_core_fn_set_doc(IdmCore *core, const char *doc);
bool idm_core_fn_multi_add_capture(IdmCore *multi, IdmCaptureKind kind, const char *name, uint32_t index, IdmBindingId binding_id);
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
bool idm_core_record_construct_add(IdmCore *core, IdmSymbol *field, const IdmTypeTerm *contract, IdmCore *value);
IdmCore *idm_core_record_field(IdmCore *receiver, IdmSymbol *type, IdmSymbol *field, uint32_t field_index, IdmSpan span);
IdmCore *idm_core_record_is(IdmCore *value, IdmSymbol *type, IdmSpan span);
IdmCore *idm_core_dispatch(IdmDispatchKind kind, const char *name, IdmSymbol *identity, IdmSpan span);
bool idm_core_dispatch_add_arg(IdmCore *core, IdmCore *arg);
bool idm_core_dispatch_add_method(IdmCore *core, IdmSymbol *trait, IdmCore *evidence, uint8_t evidence_state);
bool idm_core_dispatch_add_impl(IdmCore *core, uint32_t method, IdmSymbol *type, const IdmStructuralHead *structural, IdmArity arity, bool passthrough, uint32_t primitive, IdmCore *ref, uint8_t ref_state);
bool idm_core_dispatch_add_field(IdmCore *core, IdmSymbol *type, IdmSymbol *field, uint32_t field_index, const IdmTypeTerm *contract);
void idm_core_dispatch_set_fallback(IdmCore *core, IdmCore *fallback, uint8_t fallback_state);
void idm_core_free(IdmCore *core);
bool idm_core_statement_discardable(const IdmCore *core);
bool idm_core_compile_function(IdmRuntime *rt, IdmCore *fn, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err);
bool idm_core_compile_main(IdmRuntime *rt, IdmCore *core, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err);
bool idm_core_dump(IdmBuffer *buf, const IdmCore *core);
bool idm_core_dump_pretty(IdmBuffer *buf, const IdmCore *core);
const char *idm_primitive_name(IdmPrimitive primitive);
bool idm_primitive_pure(IdmPrimitive primitive);
size_t idm_primitive_count(void);
IdmArity idm_primitive_arity(IdmPrimitive primitive);
bool idm_primitive_contract(IdmRuntime *rt, IdmPrimitive primitive, size_t argc, IdmCallableContract *out, bool *has_contract, IdmError *err, IdmSpan span);
const char *idm_primitive_home(IdmPrimitive primitive);
bool idm_primitive_home_exists(const char *home);
bool idm_primitive_lookup(const char *home, const char *name, IdmPrimitive *out);

#endif
