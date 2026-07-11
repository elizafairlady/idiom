#ifndef IDM_VALUE_H
#define IDM_VALUE_H

#include <pthread.h>
#include <stdatomic.h>
#include "idiom/common.h"
#include "idiom/scope.h"

typedef struct IdmObject IdmObject;
typedef struct IdmSymbol IdmSymbol;
typedef struct IdmSyntax IdmSyntax;
typedef struct IdmRuntime IdmRuntime;
typedef struct IdmBytecodeModule IdmBytecodeModule;
typedef struct IdmPatternSelector IdmPatternSelector;
typedef struct IdmExec IdmExec;
typedef struct IdmRepl IdmRepl;
typedef struct IdmRegex IdmRegex;
typedef struct IdmRegexResult IdmRegexResult;
typedef struct IdmRecordShape IdmRecordShape;
typedef struct IdmEnv IdmEnv;

typedef bool (*IdmLocalExpandFn)(void *user, IdmRuntime *rt, const IdmSyntax *syntax, IdmSyntax **out_syntax, IdmError *err);
typedef bool (*IdmFreeIdentifierEqFn)(void *user, IdmRuntime *rt, const IdmSyntax *a, const IdmSyntax *b, bool *out_equal, IdmError *err);
typedef bool (*IdmIdentifierBoundFn)(void *user, IdmRuntime *rt, const IdmSyntax *word, bool *out_bound, IdmError *err);

typedef enum {
    IDM_SYMBOL_WORD,
    IDM_SYMBOL_ATOM
} IdmSymbolKind;

typedef enum {
    IDM_VAL_NIL,
    IDM_VAL_ATOM,
    IDM_VAL_WORD,
    IDM_VAL_INT,
    IDM_VAL_FLOAT,
    IDM_VAL_STRING,
    IDM_VAL_PAIR,
    IDM_VAL_TUPLE,
    IDM_VAL_VECTOR,
    IDM_VAL_DICT,
    IDM_VAL_SYNTAX,
    IDM_VAL_CELL,
    IDM_VAL_CLOSURE,
    IDM_VAL_PID,
    IDM_VAL_REF,
    IDM_VAL_PORT,
    IDM_VAL_RECORD,
    IDM_VAL_REGEX,
    IDM_VAL_REGEX_RESULT,
    IDM_VAL_BIGNUM,
    IDM_VAL_BITSTRING
} IdmValueTag;

typedef struct {
    uint64_t bits;
} IdmValue;

typedef enum {
    IDM_IMM_EMPTY = 0,
    IDM_IMM_ATOM,
    IDM_IMM_WORD,
    IDM_IMM_PID,
    IDM_IMM_REF,
    IDM_IMM_PORT
} IdmImmCtor;

#define IDM_FIXNUM_MAX ((int64_t)0x3FFFFFFFFFFFFFFFLL)
#define IDM_FIXNUM_MIN ((int64_t)0xC000000000000000LL)

static inline bool idm_is_fixnum(IdmValue v) { return (v.bits & 1u) != 0u; }
static inline int64_t idm_fixnum_value(IdmValue v) { return (int64_t)v.bits >> 1; }
static inline bool idm_fixnum_fits(int64_t i) { return i >= IDM_FIXNUM_MIN && i <= IDM_FIXNUM_MAX; }
static inline IdmValue idm_fixnum(int64_t i) { IdmValue v; v.bits = ((uint64_t)i << 1) | 1u; return v; }
static inline bool idm_is_immediate(IdmValue v) { return (v.bits & 3u) == 2u; }
static inline IdmImmCtor idm_immediate_ctor(IdmValue v) { return (IdmImmCtor)((v.bits >> 2) & 0x3Fu); }
static inline uint64_t idm_immediate_payload(IdmValue v) { return v.bits >> 8; }
static inline IdmValue idm_immediate(IdmImmCtor ctor, uint64_t payload) {
    IdmValue v;
    v.bits = (payload << 8) | ((uint64_t)(ctor & 0x3Fu) << 2) | 2u;
    return v;
}
static inline bool idm_is_boxed(IdmValue v) { return v.bits != 0u && (v.bits & 7u) == 0u; }
static inline IdmObject *idm_boxed_object(IdmValue v) { return (IdmObject *)(uintptr_t)v.bits; }
static inline IdmValue idm_from_boxed(IdmObject *obj) { IdmValue v; v.bits = (uint64_t)(uintptr_t)obj; return v; }

IdmValueTag idm_boxed_value_tag(IdmValue value);

static inline IdmValueTag idm_value_tag(IdmValue v) {
    if (v.bits == 0u) return IDM_VAL_NIL;
    if (v.bits & 1u) return IDM_VAL_INT;
    if ((v.bits & 3u) == 2u) {
        switch (idm_immediate_ctor(v)) {
            case IDM_IMM_EMPTY: return IDM_VAL_NIL;
            case IDM_IMM_ATOM: return IDM_VAL_ATOM;
            case IDM_IMM_WORD: return IDM_VAL_WORD;
            case IDM_IMM_PID: return IDM_VAL_PID;
            case IDM_IMM_REF: return IDM_VAL_REF;
            case IDM_IMM_PORT: return IDM_VAL_PORT;
        }
        return IDM_VAL_NIL;
    }
    return idm_boxed_value_tag(v);
}

typedef struct {
    IdmValue key;
    IdmValue value;
} IdmDictEntry;

typedef struct {
    const struct IdmObject *stack[16];
    size_t cursor[16];
    int depth;
} IdmDictIter;

bool idm_dict_iter_init(IdmValue dict, IdmDictIter *it);
bool idm_dict_iter_next(IdmDictIter *it, IdmValue *out_key, IdmValue *out_value);

typedef struct {
    const IdmBytecodeModule *module;
    uint32_t function_index;
    const uint32_t *entries;
    size_t entry_count;
    const IdmValue *captures;
    size_t capture_count;
    IdmEnv *env;
    IdmPatternSelector *selector;
    uint64_t selector_generation;
} IdmClosureInfo;

typedef struct {
    const IdmObject *stack[64];
    int depth;
} IdmStringIter;

void idm_string_iter_init(IdmValue v, IdmStringIter *it);
bool idm_string_iter_next(IdmStringIter *it, const char **bytes, size_t *len);

typedef enum {
    IDM_VALUE_SEQ_VECTOR,
    IDM_VALUE_SEQ_TUPLE,
    IDM_VALUE_SEQ_DICT
} IdmValueSequenceKind;

typedef enum {
    IDM_SYNTAX_BUILD_NIL,
    IDM_SYNTAX_BUILD_WORD,
    IDM_SYNTAX_BUILD_ATOM,
    IDM_SYNTAX_BUILD_INT,
    IDM_SYNTAX_BUILD_FLOAT,
    IDM_SYNTAX_BUILD_STRING,
    IDM_SYNTAX_BUILD_LIST,
    IDM_SYNTAX_BUILD_VECTOR,
    IDM_SYNTAX_BUILD_TUPLE,
    IDM_SYNTAX_BUILD_DICT,
    IDM_SYNTAX_BUILD_EXPR,
    IDM_SYNTAX_BUILD_BODY,
    IDM_SYNTAX_BUILD_GROUP
} IdmSyntaxBuildKind;


typedef struct {
    IdmSymbol **symbols;
    size_t count;
    size_t cap;
    uint32_t next_id;
    IdmSymbol **buckets;
    size_t bucket_count;
} IdmIntern;

typedef struct {
    IdmObject *objects;
    IdmObject *grey;
    IdmObject **sweep;
    size_t object_count;
    size_t bytes_allocated;
    size_t total_allocs;
    size_t total_alloc_bytes;
    unsigned mark;
    atomic_bool gc_marking;
    atomic_bool gc_sweeping;
    bool locking;
    pthread_mutex_t lock;
} IdmHeap;

typedef struct IdmEnvSlots {
    size_t count;
    struct IdmEnvSlots *retired;
    _Atomic uint64_t values[];
} IdmEnvSlots;

typedef struct IdmEnv {
    char *package_key;
    _Atomic(IdmEnvSlots *) slots;
} IdmEnv;

typedef struct IdmPhaseReads IdmPhaseReads;

struct IdmRuntime {
    IdmIntern intern;
    IdmValue cached_true;
    IdmValue cached_false;
    IdmHeap immortal;
    bool macro_intro_active;
    IdmScopeId macro_intro_scope;
    void *local_expand_user;
    IdmLocalExpandFn local_expand;
    void *free_identifier_eq_user;
    IdmFreeIdentifierEqFn free_identifier_eq;
    void *identifier_bound_user;
    IdmIdentifierBoundFn identifier_bound;
    char **cli_args;
    size_t cli_arg_count;
    IdmScopeId scope_next;
    IdmEnv **envs;
    size_t env_count;
    size_t env_cap;
    IdmEnv *main_env;
    void *expand_cache;
    void (*expand_cache_free)(void *cache);
    IdmPhaseReads *phase_reads;
    IdmRecordShape **record_shapes;
    size_t record_shape_count;
    size_t record_shape_cap;
    IdmRepl *repl;
    bool interactive;
    IdmSymbol *bits_sym_big;
    IdmSymbol *bits_sym_little;
    IdmSymbol *bits_sym_signed;
    IdmSymbol *bits_sym_unsigned;
    IdmBytecodeModule **retired_modules;
    size_t retired_module_count;
    size_t retired_module_cap;
};

void idm_runtime_retire_module(IdmRuntime *rt, IdmBytecodeModule *module);

IdmEnv *idm_package_env_get_or_create(IdmRuntime *rt, const char *key);
IdmEnv *idm_env_fresh(IdmRuntime *rt);
bool idm_env_slot_ensure(IdmEnv *env, uint32_t id, IdmError *err);
bool idm_env_slot_set(IdmRuntime *rt, IdmEnv *env, uint32_t id, IdmValue value, IdmError *err);
static inline IdmValue idm_env_slot_get(const IdmEnv *env, uint32_t id) {
    const IdmEnvSlots *slots = atomic_load_explicit(&((IdmEnv *)env)->slots, memory_order_acquire);
    if (!slots || (size_t)id >= slots->count) return idm_immediate(IDM_IMM_EMPTY, 0);
    IdmValue out;
    out.bits = atomic_load_explicit(&((IdmEnvSlots *)slots)->values[id], memory_order_acquire);
    return out;
}

void idm_runtime_init(IdmRuntime *rt);
void idm_runtime_destroy(IdmRuntime *rt);
void idm_phase_io_record(IdmRuntime *rt, const char *path);
void idm_phase_env_record(IdmRuntime *rt, const char *name, const char *value);
void idm_phase_stat_record(IdmRuntime *rt, const char *path);
void idm_phase_directory_record(IdmRuntime *rt, const char *path);
void idm_phase_write_record(IdmRuntime *rt, const char *path);
void idm_phase_nondeterministic_record(IdmRuntime *rt);

void idm_intern_init(IdmIntern *intern);
void idm_intern_destroy(IdmIntern *intern);
IdmSymbol *idm_intern(IdmIntern *intern, IdmSymbolKind kind, const char *text);
IdmSymbol *idm_intern_lookup(IdmIntern *intern, IdmSymbolKind kind, const char *text);
const char *idm_symbol_text(const IdmSymbol *sym);
uint32_t idm_symbol_id(const IdmSymbol *sym);
IdmSymbolKind idm_symbol_kind(const IdmSymbol *sym);

void idm_heap_init(IdmHeap *heap);
void idm_heap_destroy(IdmHeap *heap);
IdmHeap *idm_active_heap(IdmRuntime *rt);
void idm_set_active_heap(IdmHeap *heap);
IdmHeap *idm_swap_active_heap(IdmHeap *heap);
void idm_heap_gc_begin(IdmHeap *heap);
void idm_heap_gc_cancel(IdmHeap *heap);
void idm_gc_mark_value(IdmHeap *heap, IdmValue value);
bool idm_heap_gc_step(IdmHeap *heap, int64_t *budget);
bool idm_heap_gc_active(const IdmHeap *heap);
extern atomic_uint idm_gc_marking_heap_count;
void idm_gc_write_barrier_slow(IdmValue old_value);
static inline void idm_gc_write_barrier(IdmValue old_value) {
    if (atomic_load_explicit(&idm_gc_marking_heap_count, memory_order_acquire) != 0) idm_gc_write_barrier_slow(old_value);
}
IdmValue idm_value_copy(IdmRuntime *rt, IdmHeap *target, IdmValue value, IdmError *err);
IdmValue idm_value_copy_locked(IdmRuntime *rt, IdmHeap *target, IdmValue value, IdmError *err);
IdmHeap *idm_value_heap(IdmValue value);
size_t idm_heap_object_count(const IdmHeap *heap);
size_t idm_heap_bytes(const IdmHeap *heap);
size_t idm_heap_total_allocs(const IdmHeap *heap);
size_t idm_heap_total_alloc_bytes(const IdmHeap *heap);

static inline IdmValue idm_nil(void) { return idm_immediate(IDM_IMM_EMPTY, 0); }
static inline IdmValue idm_empty_list(void) { return idm_nil(); }
static inline IdmValue idm_int(int64_t value) { return idm_fixnum(value); }
IdmValue idm_int_promote(IdmRuntime *rt, int64_t value, IdmError *err);
bool idm_value_is_int(IdmValue v);
bool idm_int_add(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err);
bool idm_int_sub(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err);
bool idm_int_mul(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err);
bool idm_int_divmod(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *q_out, IdmValue *r_out, IdmError *err);
bool idm_int_pow(IdmRuntime *rt, IdmValue base, int64_t exponent, IdmValue *out, IdmError *err);
bool idm_int_neg(IdmRuntime *rt, IdmValue v, IdmValue *out, IdmError *err);
IdmValue idm_int_shl(IdmRuntime *rt, IdmValue v, int64_t bits, IdmError *err);
bool idm_int_bit_and(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err);
bool idm_int_bit_or(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err);
bool idm_int_bit_xor(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err);
bool idm_int_bit_not(IdmRuntime *rt, IdmValue v, IdmValue *out, IdmError *err);
bool idm_int_bit_count_nonnegative(IdmRuntime *rt, IdmValue v, IdmValue *out, IdmError *err);
bool idm_int_bit_length_nonnegative(IdmRuntime *rt, IdmValue v, IdmValue *out, IdmError *err);
IdmValue idm_int_from_decimal(IdmRuntime *rt, const char *text, size_t len, bool *ok, IdmError *err);
bool idm_bignum_view(IdmValue value, const uint32_t **limbs, size_t *count, int *sign);
IdmValue idm_int_from_limbs(IdmRuntime *rt, const uint32_t *limbs, size_t count, int sign, IdmError *err);
int idm_int_compare(IdmValue a, IdmValue b);
double idm_int_to_double(IdmValue v);
bool idm_int_to_i64(IdmValue v, int64_t *out);
IdmValue idm_float(IdmRuntime *rt, double value, IdmError *err);
static inline int64_t idm_int_value(IdmValue value) { return idm_fixnum_value(value); }
double idm_float_value(IdmValue value);
IdmSymbol *idm_value_symbol(IdmValue value);
uint64_t idm_value_id(IdmValue value);
IdmValue idm_word(IdmRuntime *rt, const char *text);
IdmValue idm_atom(IdmRuntime *rt, const char *text);
IdmValue idm_bool(IdmRuntime *rt, bool value);
IdmValue idm_string(IdmRuntime *rt, const char *text, IdmError *err);
IdmValue idm_string_n(IdmRuntime *rt, const char *text, size_t len, IdmError *err);
bool idm_string_concat2(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err);
IdmValue idm_string_slice_value(IdmRuntime *rt, IdmValue v, size_t start, size_t end, IdmError *err);
int idm_string_byte_at(IdmValue v, size_t index);
bool idm_string_append_range(IdmBuffer *buf, IdmValue v, size_t start, size_t end);
size_t idm_string_newlines_before(IdmValue v, size_t off);
bool idm_string_line_start(IdmValue v, size_t line, size_t *out_off);
bool idm_string_concat_display(IdmRuntime *rt, const IdmValue *items, size_t count, IdmValue *out, IdmError *err);
bool idm_value_append_display(IdmBuffer *buf, IdmValue v);
IdmValue idm_cons(IdmRuntime *rt, IdmValue car, IdmValue cdr, IdmError *err);
bool idm_list_append(IdmRuntime *rt, IdmValue head, IdmValue tail, IdmValue *out, IdmError *err);
IdmValue idm_tuple(IdmRuntime *rt, const IdmValue *items, size_t count, IdmError *err);
IdmValue idm_vector(IdmRuntime *rt, const IdmValue *items, size_t count, IdmError *err);
IdmValue idm_dict(IdmRuntime *rt, const IdmDictEntry *entries, size_t count, IdmError *err);
IdmValue idm_syntax_value(IdmRuntime *rt, const IdmSyntax *syntax, IdmError *err);
bool idm_syntax_build(IdmRuntime *rt, IdmSyntaxBuildKind kind, IdmValue ctx_value, IdmValue payload, IdmValue *out, IdmError *err);
IdmValue idm_cell(IdmRuntime *rt, IdmValue initial, IdmError *err);
IdmValue idm_closure_in_module(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, const IdmValue *captures, size_t capture_count, IdmEnv *env, IdmError *err);
IdmValue idm_closure_multi_selectable_in_module(IdmRuntime *rt, const IdmBytecodeModule *module, const uint32_t *function_indexes, size_t function_count, IdmPatternSelector *selector, const IdmValue *captures, size_t capture_count, IdmEnv *env, IdmError *err);
bool idm_closure_info(IdmValue value, IdmClosureInfo *out);
IdmRecordShape *idm_record_shape_intern_symbols(IdmRuntime *rt, IdmSymbol *type, IdmSymbol *const *field_names, size_t field_count, IdmError *err);
bool idm_record_shape_fill_contracts(IdmRecordShape *shape, const IdmTypeTerm *const *contracts, IdmError *err);
IdmValue idm_record_from_shape(IdmRuntime *rt, IdmRecordShape *shape, const IdmValue *field_values, IdmError *err);
IdmValue idm_record_update_value(IdmRuntime *rt, IdmValue rec, IdmValue dict, IdmError *err);
IdmSymbol *idm_record_shape_field_symbol(IdmRecordShape *shape, size_t index);
IdmValue idm_regex_value(IdmRuntime *rt, IdmRegex *regex, IdmError *err);
IdmValue idm_regex_result_value(IdmRuntime *rt, IdmRegexResult *result, IdmError *err);
IdmValue idm_pid(uint64_t id);
IdmValue idm_ref(uint64_t id);
IdmValue idm_port(uint64_t id);
const char *idm_value_type_name(IdmValueTag tag);
const char *idm_value_dispatch_type_name(IdmValue value);
const char *idm_value_sequence_kind_name(IdmValueSequenceKind kind);
const char *idm_syntax_build_kind_name(IdmSyntaxBuildKind kind);
#define IDM_BUILTIN_TYPE_ROWS(X) \
    X(NIL, "nil", LIST, 0) \
    X(ATOM, "atom", NONE, 0) \
    X(WORD, "word", NONE, 0) \
    X(INT, "int", NONE, 0) \
    X(FIXNUM, "fixnum", INT, 1) \
    X(BIGNUM, "bignum", INT, 1) \
    X(FLOAT, "float", NONE, 0) \
    X(STRING, "string", NONE, 0) \
    X(PAIR, "pair", LIST, 1) \
    X(EMPTY_LIST, "empty-list", LIST, 1) \
    X(LIST, "list", NONE, 0) \
    X(TUPLE, "tuple", NONE, 0) \
    X(VECTOR, "vector", NONE, 0) \
    X(DICT, "dict", NONE, 0) \
    X(SYNTAX, "syntax", NONE, 0) \
    X(CELL, "cell", NONE, 0) \
    X(CLOSURE, "closure", NONE, 0) \
    X(PID, "pid", PROC, 1) \
    X(REF, "ref", NONE, 0) \
    X(PORT, "port", PROC, 1) \
    X(RECORD, "record", NONE, 0) \
    X(REGEX, "regex", NONE, 0) \
    X(REGEX_RESULT, "regex-result", NONE, 0) \
    X(BITSTRING, "bitstring", NONE, 0) \
    X(PROC, "proc", NONE, 0)

typedef enum {
    IDM_BUILTIN_TYPE_NONE,
#define IDM_BUILTIN_TYPE_ENUM(id, text, parent, member) IDM_BUILTIN_TYPE_##id,
    IDM_BUILTIN_TYPE_ROWS(IDM_BUILTIN_TYPE_ENUM)
#undef IDM_BUILTIN_TYPE_ENUM
    IDM_BUILTIN_TYPE_COUNT
} IdmBuiltinType;
IdmBuiltinType idm_value_builtin_type_kind(IdmSymbol *type);
bool idm_type_name_is_builtin(const char *text);
size_t idm_builtin_overtype_members(const char *parent, const char **out_names, size_t capacity);
bool idm_value_builtin_type_symbol(IdmSymbol *type);
bool idm_value_matches_builtin_type(IdmValue value, IdmBuiltinType type);
bool idm_builtin_type_includes(IdmBuiltinType outer, IdmBuiltinType member);
bool idm_value_matches_type_symbol(IdmValue value, IdmSymbol *type);
bool idm_value_matches_type_term(IdmValue value, const IdmTypeTerm *term);
bool idm_is_nil(IdmValue value);
bool idm_is_empty_list(IdmValue value);
bool idm_is_pair(IdmValue value);
bool idm_is_tuple(IdmValue value);
bool idm_is_vector(IdmValue value);
bool idm_is_dict(IdmValue value);
bool idm_is_syntax(IdmValue value);
bool idm_is_string(IdmValue value);
bool idm_is_cell(IdmValue value);
static inline bool idm_is_closure(IdmValue value) { return idm_value_tag(value) == IDM_VAL_CLOSURE; }
bool idm_is_record(IdmValue value);
bool idm_is_regex(IdmValue value);
bool idm_is_regex_result(IdmValue value);
IdmValue idm_cell_get(IdmValue cell, IdmError *err);
bool idm_cell_set(IdmValue cell, IdmValue value, IdmError *err);
uint32_t idm_closure_function_index(IdmValue value);
const IdmBytecodeModule *idm_closure_module(IdmValue value);
size_t idm_closure_entry_count(IdmValue value);
uint32_t idm_closure_entry(IdmValue value, size_t index, IdmError *err);
size_t idm_closure_capture_count(IdmValue value);
IdmValue idm_closure_capture(IdmValue value, size_t index, IdmError *err);
IdmEnv *idm_closure_env(IdmValue value);
IdmPatternSelector *idm_closure_selector(IdmValue value);
uint64_t idm_closure_selector_generation(IdmValue value);
const char *idm_record_type(IdmValue value, IdmError *err);
IdmSymbol *idm_record_type_symbol(IdmValue value, IdmError *err);
size_t idm_record_field_count(IdmValue value, IdmError *err);
const char *idm_record_field_name(IdmValue value, size_t index, IdmError *err);
IdmSymbol *idm_record_field_name_symbol(IdmValue value, size_t index, IdmError *err);
IdmValue idm_record_field_value(IdmValue value, size_t index, IdmError *err);
bool idm_record_field_project_symbols(IdmValue value, IdmSymbol *type, IdmSymbol *field, uint32_t field_index, IdmValue *out, IdmError *err);
bool idm_record_is_symbol(IdmValue value, IdmSymbol *type);
IdmRegex *idm_regex_value_get(IdmValue value, IdmError *err);
IdmRegexResult *idm_regex_result_value_get(IdmValue value, IdmError *err);
IdmValue idm_car(IdmValue pair, IdmError *err);
IdmValue idm_cdr(IdmValue pair, IdmError *err);
size_t idm_sequence_count(IdmValue value);
IdmValue idm_sequence_item(IdmValue value, size_t index, IdmError *err);
bool idm_value_is_error(IdmValue value);
bool idm_value_ok(IdmValue value);
size_t idm_dict_count(IdmValue value);
bool idm_dict_get(IdmValue dict, IdmValue key, IdmValue *out);
IdmValue idm_dict_put(IdmRuntime *rt, IdmValue dict, IdmValue key, IdmValue value, IdmError *err);
IdmValue idm_dict_del(IdmRuntime *rt, IdmValue dict, IdmValue key, IdmError *err);
const IdmSyntax *idm_syntax_get(IdmValue value, IdmError *err);
IdmSyntax *idm_syntax_value_get(IdmValue value);
const char *idm_string_bytes(IdmValue value);
size_t idm_string_length(IdmValue value);
bool idm_is_bits(IdmValue value);
uint64_t idm_bits_len(IdmValue value);
IdmValue idm_bits_from_bytes(IdmRuntime *rt, const unsigned char *bytes, uint64_t bit_len, IdmError *err);
IdmValue idm_bits_slice(IdmRuntime *rt, IdmValue bits, uint64_t off, uint64_t len, IdmError *err);
IdmValue idm_bits_int(IdmRuntime *rt, IdmValue bits, uint64_t off, uint64_t len, bool little, bool is_signed, IdmError *err);
IdmValue idm_bits_float(IdmRuntime *rt, IdmValue bits, uint64_t off, uint64_t len, bool little, IdmError *err);
IdmValue idm_bits_of_int(IdmRuntime *rt, IdmValue n, uint64_t len, bool little, IdmError *err);
IdmValue idm_bits_append(IdmRuntime *rt, IdmValue a, IdmValue b, IdmError *err);
bool idm_bits_read(IdmValue bits, uint64_t off, uint64_t len, unsigned char *dst);
int idm_bits_compare(IdmValue a, IdmValue b);
bool idm_value_equal(IdmValue a, IdmValue b);
bool idm_value_write(IdmBuffer *buf, IdmValue value);
bool idm_value_write_json(IdmBuffer *buf, IdmValue value);
IdmValue idm_error_value(IdmRuntime *rt, IdmValue detail);
bool idm_error_reason(IdmRuntime *rt, IdmError *err, const char *kind, size_t count, ...);
bool idm_error_reason_is(const IdmError *err, const char *kind);
bool idm_error_take_reason(IdmError *err, IdmValue *out);
IdmValue idm_error_reason_value(IdmRuntime *rt, IdmError *err);
IdmValue idm_span_value(IdmRuntime *rt, IdmSpan span);
void idm_error_describe(IdmRuntime *rt, IdmValue reason, IdmBuffer *out);

#endif
