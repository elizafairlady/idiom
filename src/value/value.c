#include "idiom/value.h"

#include "idiom/bignum.h"
#include "idiom/bytecode.h"
#include "idiom/pattern.h"
#include "idiom/regex.h"
#include "idiom/syntax.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    IDM_OBJ_STRING,
    IDM_OBJ_ROPE,
    IDM_OBJ_PAIR,
    IDM_OBJ_TUPLE,
    IDM_OBJ_VECTOR,
    IDM_OBJ_DICT,
    IDM_OBJ_DICT_NODE,
    IDM_OBJ_SYNTAX,
    IDM_OBJ_CELL,
    IDM_OBJ_CLOSURE,
    IDM_OBJ_RECORD,
    IDM_OBJ_REGEX,
    IDM_OBJ_REGEX_RESULT,
    IDM_OBJ_FLONUM,
    IDM_OBJ_BIGNUM,
    IDM_OBJ_BITSTRING,
    IDM_OBJ_COUNT
} IdmObjectKind;

typedef struct {
    char *bytes;
    size_t len;
} IdmStringObj;

typedef struct {
    IdmValue left;
    IdmValue right;
    size_t len;
    size_t newlines;
    uint16_t height;
    char *_Atomic flat;
} IdmRopeObj;

typedef struct {
    IdmValue car;
    IdmValue cdr;
} IdmPairObj;

typedef struct {
    IdmValue *items;
    size_t count;
} IdmSequenceObj;

typedef struct {
    size_t count;
    IdmValue root;
} IdmDictObj;

typedef struct {
    uint32_t datamap;
    uint32_t nodemap;
    uint16_t collision_count;
    IdmValue *slots;
} IdmDictNodeObj;

typedef struct {
    const IdmBytecodeModule *module;
    uint32_t function_index;
    uint32_t *entries;
    size_t entry_count;
    IdmValue *captures;
    size_t capture_count;
    IdmEnv *env;
    IdmPatternSelector *selector;
    uint64_t selector_generation;
} IdmClosureObj;

typedef struct {
    IdmRecordShape *shape;
    IdmValue *field_values;
} IdmRecordObj;

struct IdmRecordShape {
    IdmSymbol *type;
    IdmSymbol **field_names;
    size_t field_count;
    IdmTypeTerm *contracts;
    bool *has_contract;
    bool contracts_set;
};

typedef struct {
    IdmValue value;
} IdmCellObj;

typedef struct {
    uint32_t *limbs;
    uint32_t count;
    int32_t sign;
} IdmBignumObj;

typedef struct {
    IdmValue parent;
    unsigned char *bytes;
    uint64_t bit_off;
    uint64_t bit_len;
    uint64_t bit_cap;
    uint64_t bit_used;
} IdmBitstringObj;

struct IdmObject {
    IdmObjectKind kind;
    unsigned mark;
    bool greyed;
    IdmHeap *heap;
    size_t bytes;
    _Atomic uint32_t dict_hash;
    struct IdmObject *next;
    struct IdmObject *grey_next;
    size_t scan;
    union {
        IdmStringObj string;
        IdmRopeObj rope;
        IdmPairObj pair;
        IdmSequenceObj sequence;
        IdmDictObj dict;
        IdmDictNodeObj dict_node;
        IdmSyntax *syntax;
        IdmCellObj cell;
        IdmClosureObj closure;
        IdmRecordObj record;
        IdmRegex *regex;
        IdmRegexResult *regex_result;
        double flonum;
        IdmBignumObj bignum;
        IdmBitstringObj bits;
    } as;
};

struct IdmSymbol {
    char *text;
    unsigned char identity_hash[32];
    uint32_t id;
    IdmSymbolKind kind;
    IdmBuiltinType builtin_type;
    uint32_t hash;
    bool falsy;
    bool error_atom;
    struct IdmSymbol *hnext;
};

static pthread_mutex_t g_intern_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_env_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_record_shape_mu = PTHREAD_MUTEX_INITIALIZER;
atomic_uint idm_gc_marking_heap_count = 0;

static const char *rope_flatten(IdmObject *obj);
static IdmSymbol *idm_intern_unlocked(IdmIntern *intern, IdmSymbolKind kind, const char *text, const unsigned char identity_hash[32]);
static IdmSymbol *intern_find_unlocked(IdmIntern *intern, IdmSymbolKind kind, const char *text, const unsigned char identity_hash[32], uint32_t h);
static uint32_t intern_hash(IdmSymbolKind kind, const char *text, const unsigned char identity_hash[32]);
static IdmEnv *env_get_or_create_unlocked(IdmRuntime *rt, IdmSymbol *package_key);

static void heap_lock(IdmHeap *heap) {
    if (heap->locking) pthread_mutex_lock(&heap->lock);
}

static void heap_unlock(IdmHeap *heap) {
    if (heap->locking) pthread_mutex_unlock(&heap->lock);
}

static void heap_push_grey_unlocked(IdmHeap *heap, IdmObject *obj) {
    obj->grey_next = heap->grey;
    heap->grey = obj;
}

static const char *const heap_kind_profile_names[] = {
    "alloc.string", "alloc.rope", "alloc.pair", "alloc.tuple", "alloc.vector", "alloc.dict",
    "alloc.dict_node", "alloc.syntax", "alloc.cell", "alloc.closure", "alloc.record",
    "alloc.regex", "alloc.regex_result", "alloc.flonum", "alloc.bignum", "alloc.bitstring"
};

static _Atomic IdmProfileMetricHandle heap_kind_profile_handles[IDM_OBJ_COUNT];

static void heap_profile_count(IdmObjectKind kind, size_t amount) {
    if (!idm_profile_enabled()) return;
    IdmProfileMetricHandle handle = atomic_load_explicit(&heap_kind_profile_handles[kind], memory_order_acquire);
    if (handle == 0) {
        IdmProfileMetricHandle made = idm_profile_metric_handle(heap_kind_profile_names[kind]);
        if (made == 0) return;
        IdmProfileMetricHandle expected = 0;
        if (atomic_compare_exchange_strong_explicit(&heap_kind_profile_handles[kind], &expected, made, memory_order_acq_rel, memory_order_acquire)) {
            handle = made;
        } else {
            handle = expected;
        }
    }
    idm_profile_count_handle(handle, (uint64_t)amount);
}

static IdmObject *heap_alloc_extra_unlocked(IdmHeap *heap, IdmObjectKind kind, size_t extra) {
    if (extra > SIZE_MAX - sizeof(IdmObject)) return NULL;
    heap_profile_count(kind, sizeof(IdmObject) + extra);
    IdmObject *obj = calloc(1u, sizeof(*obj) + extra);
    if (!obj) return NULL;
    obj->kind = kind;
    obj->mark = heap->mark;
    obj->greyed = false;
    obj->scan = 0;
    obj->heap = heap;
    obj->bytes = sizeof(*obj) + extra;
    obj->next = heap->objects;
    heap->objects = obj;
    heap->object_count++;
    heap->bytes_allocated += obj->bytes;
    heap->total_allocs++;
    heap->total_alloc_bytes += obj->bytes;
    return obj;
}

static IdmObject *heap_alloc_payload_unlocked(IdmHeap *heap, IdmObjectKind kind, size_t extra) {
    return heap_alloc_extra_unlocked(heap, kind, extra);
}

static IdmObject *heap_alloc_payload(IdmHeap *heap, IdmObjectKind kind, size_t extra) {
    heap_lock(heap);
    IdmObject *obj = heap_alloc_payload_unlocked(heap, kind, extra);
    heap_unlock(heap);
    return obj;
}

static IdmObject *heap_alloc_unlocked(IdmHeap *heap, IdmObjectKind kind) {
    return heap_alloc_extra_unlocked(heap, kind, 0u);
}

static IdmObject *heap_alloc(IdmHeap *heap, IdmObjectKind kind) {
    heap_lock(heap);
    IdmObject *obj = heap_alloc_unlocked(heap, kind);
    heap_unlock(heap);
    return obj;
}

static IdmObject *heap_alloc_extra(IdmHeap *heap, IdmObjectKind kind, size_t extra) {
    heap_lock(heap);
    IdmObject *obj = heap_alloc_extra_unlocked(heap, kind, extra);
    heap_unlock(heap);
    return obj;
}

static bool object_payload_external(const IdmObject *obj, const void *ptr) {
    if (!obj || !ptr) return false;
    const char *p = ptr;
    const char *start = (const char *)(obj + 1);
    const char *end = (const char *)obj + obj->bytes;
    return p < start || p >= end;
}

static void heap_account_unlocked(IdmHeap *heap, IdmObject *obj, size_t extra) {
    obj->bytes += extra;
    heap->bytes_allocated += extra;
    heap->total_alloc_bytes += extra;
}

static void heap_account(IdmHeap *heap, IdmObject *obj, size_t extra) {
    heap_lock(heap);
    heap_account_unlocked(heap, obj, extra);
    heap_unlock(heap);
}

static size_t syn_footprint(const IdmSyntax *syn) {
    if (!syn) return 0;
    size_t total = sizeof(*syn);
    if (syn->token_raw) total += strlen(syn->token_raw) + 1u;
    for (size_t i = 0; i < syn->scopes.count; i++) total += sizeof(syn->scopes.items[i]) + syn->scopes.items[i].scopes.count * sizeof(IdmScopeId);
    for (size_t i = 0; i < syn->property_count; i++) total += strlen(syn->properties[i].key) + strlen(syn->properties[i].value) + 2u;
    for (size_t i = 0; i < syn->origins.count; i++) total += strlen(syn->origins.items[i]) + 1u;
    switch (syn->kind) {
        case IDM_SYN_WORD:
        case IDM_SYN_ATOM:
        case IDM_SYN_BIGINT:
        case IDM_SYN_STRING:
            total += strlen(syn->as.text) + 1u;
            break;
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
            for (size_t i = 0; i < syn->as.seq.count; i++) total += syn_footprint(syn->as.seq.items[i]);
            break;
        default:
            break;
    }
    return total;
}

static void object_free(IdmObject *obj) {
    if (!obj) return;
    if (obj->kind == IDM_OBJ_STRING && object_payload_external(obj, obj->as.string.bytes)) free(obj->as.string.bytes);
    if (obj->kind == IDM_OBJ_ROPE) free(atomic_load_explicit(&obj->as.rope.flat, memory_order_acquire));
    if ((obj->kind == IDM_OBJ_TUPLE || obj->kind == IDM_OBJ_VECTOR) && object_payload_external(obj, obj->as.sequence.items)) free(obj->as.sequence.items);
    if (obj->kind == IDM_OBJ_DICT_NODE && object_payload_external(obj, obj->as.dict_node.slots)) free(obj->as.dict_node.slots);
    if (obj->kind == IDM_OBJ_SYNTAX) idm_syn_free(obj->as.syntax);
    if (obj->kind == IDM_OBJ_CLOSURE) {
        idm_pattern_selector_free(obj->as.closure.selector);
        if (object_payload_external(obj, obj->as.closure.entries)) free(obj->as.closure.entries);
        if (object_payload_external(obj, obj->as.closure.captures)) free(obj->as.closure.captures);
    }
    if (obj->kind == IDM_OBJ_RECORD) {
        if (object_payload_external(obj, obj->as.record.field_values)) free(obj->as.record.field_values);
    }
    if (obj->kind == IDM_OBJ_REGEX) idm_regex_free(obj->as.regex);
    if (obj->kind == IDM_OBJ_REGEX_RESULT) idm_regex_result_free(obj->as.regex_result);
    if (obj->kind == IDM_OBJ_BIGNUM && object_payload_external(obj, obj->as.bignum.limbs)) free(obj->as.bignum.limbs);
    if (obj->kind == IDM_OBJ_BITSTRING && idm_is_nil(obj->as.bits.parent)) free(obj->as.bits.bytes);
    free(obj);
}

static void record_shape_destroy(IdmRecordShape *shape) {
    if (!shape) return;
    if (shape->contracts) {
        for (size_t i = 0; i < shape->field_count; i++) idm_type_term_destroy(&shape->contracts[i]);
        free(shape->contracts);
    }
    free(shape->has_contract);
    free(shape->field_names);
    free(shape);
}

static const char *record_shape_type_text(const IdmRecordShape *shape) {
    return idm_symbol_text(shape->type);
}

static const char *record_shape_field_text(const IdmRecordShape *shape, size_t index) {
    return idm_symbol_text(shape->field_names[index]);
}

IdmSymbol *idm_record_shape_field_symbol(IdmRecordShape *shape, size_t index) {
    if (!shape || index >= shape->field_count) return NULL;
    return shape->field_names[index];
}

static bool record_shape_matches(const IdmRecordShape *shape, IdmSymbol *type, IdmSymbol *const *field_names, size_t field_count) {
    if (shape->type != type || shape->field_count != field_count) return false;
    for (size_t i = 0; i < field_count; i++) {
        if (shape->field_names[i] != field_names[i]) return false;
    }
    return true;
}

static IdmRecordShape *record_shape_create(IdmSymbol *type, IdmSymbol *const *field_names, size_t field_count, IdmError *err) {
    IdmRecordShape *shape = calloc(1u, sizeof(*shape));
    if (!shape) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    shape->type = type;
    if (field_count != 0) {
        shape->field_names = calloc(field_count, sizeof(*shape->field_names));
        if (!shape->field_names) {
            record_shape_destroy(shape);
            idm_error_oom(err, idm_span_unknown(NULL));
            return NULL;
        }
        shape->field_count = field_count;
        for (size_t i = 0; i < field_count; i++) {
            shape->field_names[i] = field_names[i];
        }
    }
    return shape;
}

IdmRecordShape *idm_record_shape_intern_symbols(IdmRuntime *rt, IdmSymbol *type, IdmSymbol *const *field_names, size_t field_count, IdmError *err) {
    if (!rt || !type) {
        idm_error_set(err, idm_span_unknown(NULL), "record shape requires a type");
        return NULL;
    }
    if (field_count != 0 && !field_names) {
        idm_error_set(err, idm_span_unknown(NULL), "record shape fields require names");
        return NULL;
    }
    for (size_t i = 0; i < field_count; i++) {
        if (!field_names[i]) {
            idm_error_set(err, idm_span_unknown(NULL), "record field must be a non-empty name");
            return NULL;
        }
    }
    pthread_mutex_lock(&g_record_shape_mu);
    for (size_t i = 0; i < rt->record_shape_count; i++) {
        if (record_shape_matches(rt->record_shapes[i], type, field_names, field_count)) {
            IdmRecordShape *shape = rt->record_shapes[i];
            pthread_mutex_unlock(&g_record_shape_mu);
            return shape;
        }
    }
    IdmRecordShape *shape = record_shape_create(type, field_names, field_count, err);
    if (!shape) {
        pthread_mutex_unlock(&g_record_shape_mu);
        return NULL;
    }
    if (rt->record_shape_count == rt->record_shape_cap) {
        if (!idm_grow((void **)&rt->record_shapes, &rt->record_shape_cap, sizeof(*rt->record_shapes), 8u, rt->record_shape_count + 1u)) {
            pthread_mutex_unlock(&g_record_shape_mu);
            record_shape_destroy(shape);
            idm_error_oom(err, idm_span_unknown(NULL));
            return NULL;
        }
    }
    rt->record_shapes[rt->record_shape_count++] = shape;
    pthread_mutex_unlock(&g_record_shape_mu);
    return shape;
}

bool idm_record_shape_fill_contracts(IdmRecordShape *shape, const IdmTypeTerm *const *contracts, IdmError *err) {
    if (!shape || shape->field_count == 0) return true;
    pthread_mutex_lock(&g_record_shape_mu);
    if (shape->contracts_set) {
        pthread_mutex_unlock(&g_record_shape_mu);
        return true;
    }
    IdmTypeTerm *terms = calloc(shape->field_count, sizeof(*terms));
    bool *flags = calloc(shape->field_count, sizeof(*flags));
    bool ok = terms && flags;
    for (size_t i = 0; ok && i < shape->field_count; i++) {
        if (!contracts || !contracts[i]) continue;
        ok = idm_type_term_copy(&terms[i], contracts[i]);
        flags[i] = ok;
    }
    if (!ok) {
        if (terms) {
            for (size_t i = 0; i < shape->field_count; i++) idm_type_term_destroy(&terms[i]);
            free(terms);
        }
        free(flags);
        pthread_mutex_unlock(&g_record_shape_mu);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    shape->contracts = terms;
    shape->has_contract = flags;
    shape->contracts_set = true;
    pthread_mutex_unlock(&g_record_shape_mu);
    return true;
}

IdmValue idm_record_update_value(IdmRuntime *rt, IdmValue rec, IdmValue dict, IdmError *err) {
    if (idm_value_tag(rec) != IDM_VAL_RECORD) {
        idm_error_set(err, idm_span_unknown(NULL), "record-update expects a record, got %s", idm_value_dispatch_type_name(rec));
        return idm_nil();
    }
    if (idm_value_tag(dict) != IDM_VAL_DICT) {
        idm_error_set(err, idm_span_unknown(NULL), "record-update expects a dict of fields, got %s", idm_value_dispatch_type_name(dict));
        return idm_nil();
    }
    const IdmRecordObj *obj = &idm_boxed_object(rec)->as.record;
    IdmRecordShape *shape = obj->shape;
    IdmDictIter it;
    if (!idm_dict_iter_init(dict, &it)) return rec;
    IdmValue *values = shape->field_count ? malloc(shape->field_count * sizeof(*values)) : NULL;
    if (shape->field_count && !values) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    for (size_t i = 0; i < shape->field_count; i++) values[i] = obj->field_values[i];
    IdmValue key;
    IdmValue value;
    size_t replaced = 0;
    while (idm_dict_iter_next(&it, &key, &value)) {
        IdmSymbol *name = idm_value_tag(key) == IDM_VAL_ATOM ? idm_value_symbol(key) : NULL;
        size_t at = SIZE_MAX;
        for (size_t i = 0; name && i < shape->field_count; i++) {
            if (shape->field_names[i] == name) { at = i; break; }
        }
        if (at == SIZE_MAX) {
            IdmBuffer text;
            idm_buf_init(&text);
            bool wrote = idm_value_write(&text, key);
            idm_error_set(err, idm_span_unknown(NULL), "record '%s' has no field %s", record_shape_type_text(shape), wrote && text.data ? text.data : "?");
            idm_buf_destroy(&text);
            free(values);
            return idm_nil();
        }
        if (shape->contracts_set && shape->has_contract[at] && !idm_value_matches_type_term(value, &shape->contracts[at])) {
            IdmBuffer expected;
            idm_buf_init(&expected);
            if (idm_type_term_write(&expected, &shape->contracts[at])) {
                idm_error_set(err, idm_span_unknown(NULL), "record field '%s' expects %s, got %s", record_shape_field_text(shape, at), expected.data ? expected.data : "_", idm_value_dispatch_type_name(value));
            } else {
                idm_error_oom(err, idm_span_unknown(NULL));
            }
            idm_buf_destroy(&expected);
            free(values);
            return idm_nil();
        }
        values[at] = value;
        replaced++;
    }
    if (replaced == 0) {
        free(values);
        return rec;
    }
    IdmValue out = idm_record_from_shape(rt, shape, values, err);
    free(values);
    return out;
}

void idm_runtime_init(IdmRuntime *rt) {
    idm_intern_init(&rt->intern);
    rt->cached_true = idm_atom(rt, "true");
    rt->cached_false = idm_atom(rt, "false");
    rt->bits_sym_big = idm_intern(&rt->intern, IDM_SYMBOL_ATOM, "big");
    rt->bits_sym_little = idm_intern(&rt->intern, IDM_SYMBOL_ATOM, "little");
    rt->bits_sym_signed = idm_intern(&rt->intern, IDM_SYMBOL_ATOM, "signed");
    rt->bits_sym_unsigned = idm_intern(&rt->intern, IDM_SYMBOL_ATOM, "unsigned");
    idm_heap_init(&rt->immortal);
    rt->macro_intro_active = false;
    rt->macro_intro_scope = 0;
    rt->scope_next = 1u;
    rt->local_expand_user = NULL;
    rt->local_expand = NULL;
    rt->free_identifier_eq_user = NULL;
    rt->free_identifier_eq = NULL;
    rt->cli_args = NULL;
    rt->cli_arg_count = 0;

    rt->envs = NULL;
    rt->env_count = 0;
    rt->env_cap = 0;
    rt->main_env = idm_env_fresh(rt);
    rt->expand_cache = NULL;
    rt->expand_cache_free = NULL;
    rt->phase_reads = NULL;
    rt->record_shapes = NULL;
    rt->record_shape_count = 0;
    rt->record_shape_cap = 0;
    rt->repl = NULL;
    rt->interactive = false;
    rt->retired_modules = NULL;
    rt->retired_module_count = 0;
    rt->retired_module_cap = 0;
}

void idm_runtime_retire_module(IdmRuntime *rt, IdmBytecodeModule *module) {
    if (!module) return;
    if (!rt) {
        idm_bc_destroy(module);
        free(module);
        return;
    }
    pthread_mutex_lock(&g_env_mu);
    bool stored = idm_grow((void **)&rt->retired_modules, &rt->retired_module_cap, sizeof(*rt->retired_modules), 8u, rt->retired_module_count + 1u);
    if (stored) {
        rt->retired_modules[rt->retired_module_count++] = module;
    }
    pthread_mutex_unlock(&g_env_mu);
    if (!stored) {
        idm_bc_destroy(module);
        free(module);
    }
}

void idm_runtime_destroy(IdmRuntime *rt) {
    if (rt->expand_cache && rt->expand_cache_free) rt->expand_cache_free(rt->expand_cache);
    rt->expand_cache = NULL;
    rt->expand_cache_free = NULL;
    for (size_t i = 0; i < rt->retired_module_count; i++) {
        idm_bc_destroy(rt->retired_modules[i]);
        free(rt->retired_modules[i]);
    }
    free(rt->retired_modules);
    rt->retired_modules = NULL;
    rt->retired_module_count = 0;
    rt->retired_module_cap = 0;
    for (size_t i = 0; i < rt->env_count; i++) {
        IdmEnvSlots *slots = atomic_load_explicit(&rt->envs[i]->slots, memory_order_relaxed);
        while (slots) {
            IdmEnvSlots *retired = slots->retired;
            free(slots);
            slots = retired;
        }
        free(rt->envs[i]);
    }
    free(rt->envs);
    rt->envs = NULL;
    rt->env_count = 0;
    rt->env_cap = 0;
    rt->main_env = NULL;
    pthread_mutex_lock(&g_record_shape_mu);
    for (size_t i = 0; i < rt->record_shape_count; i++) record_shape_destroy(rt->record_shapes[i]);
    free(rt->record_shapes);
    rt->record_shapes = NULL;
    rt->record_shape_count = 0;
    rt->record_shape_cap = 0;
    pthread_mutex_unlock(&g_record_shape_mu);
    idm_heap_destroy(&rt->immortal);
    idm_intern_destroy(&rt->intern);
}

IdmEnv *idm_package_env_get_or_create(IdmRuntime *rt, IdmSymbol *key) {
    if (!key) return NULL;
    pthread_mutex_lock(&g_env_mu);
    IdmEnv *found = env_get_or_create_unlocked(rt, key);
    pthread_mutex_unlock(&g_env_mu);
    return found;
}

IdmEnv *idm_env_fresh(IdmRuntime *rt) {
    pthread_mutex_lock(&g_env_mu);
    IdmEnv *env = env_get_or_create_unlocked(rt, NULL);
    pthread_mutex_unlock(&g_env_mu);
    return env;
}

static IdmEnv *env_get_or_create_unlocked(IdmRuntime *rt, IdmSymbol *package_key) {
    if (package_key) {
        for (size_t i = 0; i < rt->env_count; i++) {
            if (rt->envs[i]->package_key == package_key) return rt->envs[i];
        }
    }
    IdmEnv *env = calloc(1u, sizeof(*env));
    if (!env) return NULL;
    atomic_init(&env->slots, NULL);
    env->package_key = package_key;
    if (rt->env_count == rt->env_cap) {
        if (!idm_grow((void **)&rt->envs, &rt->env_cap, sizeof(*rt->envs), 8u, rt->env_count + 1u)) { free(env); return NULL; }
    }
    rt->envs[rt->env_count++] = env;
    return env;
}

bool idm_env_slot_ensure(IdmEnv *env, uint32_t id, IdmError *err) {
    size_t needed = (size_t)id + 1u;
    IdmEnvSlots *cur = atomic_load_explicit(&env->slots, memory_order_acquire);
    if (cur && needed <= cur->count) return true;
    pthread_mutex_lock(&g_env_mu);
    cur = atomic_load_explicit(&env->slots, memory_order_relaxed);
    size_t have = cur ? cur->count : 0;
    if (needed <= have) {
        pthread_mutex_unlock(&g_env_mu);
        return true;
    }
    size_t cap = have ? have : 16u;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2u) {
            pthread_mutex_unlock(&g_env_mu);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        cap *= 2u;
    }
    IdmEnvSlots *next = malloc(sizeof(*next) + cap * sizeof(next->values[0]));
    if (!next) {
        pthread_mutex_unlock(&g_env_mu);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    next->count = cap;
    next->retired = cur;
    for (size_t i = 0; i < have; i++) {
        atomic_init(&next->values[i], atomic_load_explicit(&cur->values[i], memory_order_relaxed));
    }
    for (size_t i = have; i < cap; i++) atomic_init(&next->values[i], idm_nil().bits);
    atomic_store_explicit(&env->slots, next, memory_order_release);
    pthread_mutex_unlock(&g_env_mu);
    return true;
}

bool idm_env_slot_set(IdmRuntime *rt, IdmEnv *env, uint32_t id, IdmValue value, IdmError *err) {
    value = idm_value_copy(rt, &rt->immortal, value, err);
    if (err && err->present) return false;
    pthread_mutex_lock(&g_env_mu);
    IdmEnvSlots *cur = atomic_load_explicit(&env->slots, memory_order_relaxed);
    if (!cur || (size_t)id >= cur->count) {
        pthread_mutex_unlock(&g_env_mu);
        return idm_error_set(err, idm_span_unknown(NULL), "env slot %u set before ensure", id);
    }
    atomic_store_explicit(&cur->values[id], value.bits, memory_order_release);
    pthread_mutex_unlock(&g_env_mu);
    return true;
}


void idm_intern_init(IdmIntern *intern) {
    intern->symbols = NULL;
    intern->count = 0;
    intern->cap = 0;
    intern->next_id = 1u;
    intern->buckets = NULL;
    intern->bucket_count = 0;
}

void idm_intern_destroy(IdmIntern *intern) {
    if (!intern) return;
    for (size_t i = 0; i < intern->count; i++) { free(intern->symbols[i]->text); free(intern->symbols[i]); }
    free(intern->symbols);
    free(intern->buckets);
    intern->symbols = NULL;
    intern->count = 0;
    intern->cap = 0;
    intern->next_id = 1u;
    intern->buckets = NULL;
    intern->bucket_count = 0;
}

IdmSymbol *idm_intern(IdmIntern *intern, IdmSymbolKind kind, const char *text) {
    if (!intern || !text || kind == IDM_SYMBOL_IDENTITY) return NULL;
    pthread_mutex_lock(&g_intern_mu);
    IdmSymbol *sym = idm_intern_unlocked(intern, kind, text, NULL);
    pthread_mutex_unlock(&g_intern_mu);
    return sym;
}

IdmSymbol *idm_intern_lookup(IdmIntern *intern, IdmSymbolKind kind, const char *text) {
    if (!intern || !text || kind == IDM_SYMBOL_IDENTITY) return NULL;
    uint32_t h = intern_hash(kind, text, NULL);
    pthread_mutex_lock(&g_intern_mu);
    IdmSymbol *sym = intern_find_unlocked(intern, kind, text, NULL, h);
    pthread_mutex_unlock(&g_intern_mu);
    return sym;
}

IdmSymbol *idm_intern_identity(IdmIntern *intern, const char *text, const unsigned char identity_hash[32]) {
    if (!intern || !text || !identity_hash) return NULL;
    pthread_mutex_lock(&g_intern_mu);
    IdmSymbol *sym = idm_intern_unlocked(intern, IDM_SYMBOL_IDENTITY, text, identity_hash);
    pthread_mutex_unlock(&g_intern_mu);
    return sym;
}

static uint32_t intern_hash(IdmSymbolKind kind, const char *text, const unsigned char identity_hash[32]) {
    uint32_t h = 2166136261u;
    h ^= (uint32_t)kind;
    h *= 16777619u;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    if (kind == IDM_SYMBOL_IDENTITY) {
        for (size_t i = 0; i < 32u; i++) {
            h ^= identity_hash[i];
            h *= 16777619u;
        }
    }
    return h;
}

static IdmBuiltinType builtin_type_from_text(const char *text) {
    static const char *const names[IDM_BUILTIN_TYPE_COUNT] = {
#define IDM_BUILTIN_TYPE_NAME(id, text, parent, member) [IDM_BUILTIN_TYPE_##id] = text,
        IDM_BUILTIN_TYPE_ROWS(IDM_BUILTIN_TYPE_NAME)
#undef IDM_BUILTIN_TYPE_NAME
    };
    for (IdmBuiltinType type = IDM_BUILTIN_TYPE_NIL; type < IDM_BUILTIN_TYPE_COUNT; type++) {
        if (strcmp(text, names[type]) == 0) return type;
    }
    return IDM_BUILTIN_TYPE_NONE;
}

bool idm_type_name_is_builtin(const char *text) {
    return text && builtin_type_from_text(text) != IDM_BUILTIN_TYPE_NONE;
}

size_t idm_builtin_overtype_members(const char *parent, const char **out_names, size_t capacity) {
    IdmBuiltinType parent_type = parent ? builtin_type_from_text(parent) : IDM_BUILTIN_TYPE_NONE;
    size_t count = 0;
#define IDM_BUILTIN_TYPE_MEMBER(id, text, row_parent, member) \
    if (member && parent_type == IDM_BUILTIN_TYPE_##row_parent) { \
        if (count < capacity) out_names[count] = text; \
        count++; \
    }
    IDM_BUILTIN_TYPE_ROWS(IDM_BUILTIN_TYPE_MEMBER)
#undef IDM_BUILTIN_TYPE_MEMBER
    return count;
}

static bool intern_rehash(IdmIntern *intern, size_t new_count) {
    IdmSymbol **buckets = calloc(new_count, sizeof(*buckets));
    if (!buckets) return false;
    for (size_t i = 0; i < intern->count; i++) {
        IdmSymbol *s = intern->symbols[i];
        size_t b = s->hash & (new_count - 1u);
        s->hnext = buckets[b];
        buckets[b] = s;
    }
    free(intern->buckets);
    intern->buckets = buckets;
    intern->bucket_count = new_count;
    return true;
}

static IdmSymbol *idm_intern_unlocked(IdmIntern *intern, IdmSymbolKind kind, const char *text, const unsigned char identity_hash[32]) {
    uint32_t h = intern_hash(kind, text, identity_hash);
    IdmSymbol *found = intern_find_unlocked(intern, kind, text, identity_hash, h);
    if (found) return found;
    if (intern->count == intern->cap) {
        if (!idm_grow((void **)&intern->symbols, &intern->cap, sizeof(*intern->symbols), 32u, intern->count + 1u)) return NULL;
    }
    if (intern->bucket_count == 0u || intern->count + 1u > intern->bucket_count - (intern->bucket_count >> 2)) {
        size_t bucket_count = 0;
        if (intern->bucket_count == SIZE_MAX ||
            !idm_next_capacity(intern->bucket_count, 64u, intern->bucket_count + 1u, &bucket_count) ||
            !intern_rehash(intern, bucket_count)) return NULL;
    }
    IdmSymbol *sym = malloc(sizeof(*sym));
    if (!sym) return NULL;
    sym->text = idm_strdup(text);
    if (!sym->text) {
        free(sym);
        return NULL;
    }
    sym->id = intern->next_id++;
    sym->kind = kind;
    if (kind == IDM_SYMBOL_IDENTITY) memcpy(sym->identity_hash, identity_hash, sizeof(sym->identity_hash));
    else memset(sym->identity_hash, 0, sizeof(sym->identity_hash));
    sym->builtin_type = builtin_type_from_text(text);
    sym->hash = h;
    sym->falsy = strcmp(text, "false") == 0 || strcmp(text, "nil") == 0;
    sym->error_atom = kind == IDM_SYMBOL_ATOM && strcmp(text, "error") == 0;
    intern->symbols[intern->count++] = sym;
    size_t b = h & (intern->bucket_count - 1u);
    sym->hnext = intern->buckets[b];
    intern->buckets[b] = sym;
    return sym;
}

static IdmSymbol *intern_find_unlocked(IdmIntern *intern, IdmSymbolKind kind, const char *text, const unsigned char identity_hash[32], uint32_t h) {
    if (!intern->bucket_count) return NULL;
    for (IdmSymbol *s = intern->buckets[h & (intern->bucket_count - 1u)]; s; s = s->hnext) {
        if (s->hash == h && s->kind == kind && strcmp(s->text, text) == 0 &&
            (kind != IDM_SYMBOL_IDENTITY || memcmp(s->identity_hash, identity_hash, sizeof(s->identity_hash)) == 0)) return s;
    }
    return NULL;
}

const char *idm_symbol_text(const IdmSymbol *sym) {
    return sym ? sym->text : "";
}

const unsigned char *idm_symbol_identity_hash(const IdmSymbol *sym) {
    return sym && sym->kind == IDM_SYMBOL_IDENTITY ? sym->identity_hash : NULL;
}

uint32_t idm_symbol_id(const IdmSymbol *sym) {
    return sym ? sym->id : 0u;
}

IdmSymbolKind idm_symbol_kind(const IdmSymbol *sym) {
    return sym ? sym->kind : IDM_SYMBOL_WORD;
}

void idm_heap_init(IdmHeap *heap) {
    heap->objects = NULL;
    heap->grey = NULL;
    heap->sweep = NULL;
    heap->object_count = 0;
    heap->bytes_allocated = 0;
    heap->total_allocs = 0;
    heap->total_alloc_bytes = 0;
    heap->mark = 0;
    atomic_init(&heap->gc_marking, false);
    atomic_init(&heap->gc_sweeping, false);
    heap->locking = false;
    pthread_mutex_init(&heap->lock, NULL);
}

static _Thread_local IdmHeap *t_active_heap = NULL;

IdmHeap *idm_active_heap(IdmRuntime *rt) {
    return t_active_heap ? t_active_heap : &rt->immortal;
}

void idm_set_active_heap(IdmHeap *heap) {
    t_active_heap = heap;
}

IdmHeap *idm_swap_active_heap(IdmHeap *heap) {
    IdmHeap *prev = t_active_heap;
    t_active_heap = heap;
    return prev;
}

void idm_heap_destroy(IdmHeap *heap) {
    bool marking = atomic_exchange_explicit(&heap->gc_marking, false, memory_order_acq_rel);
    atomic_store_explicit(&heap->gc_sweeping, false, memory_order_release);
    if (marking) atomic_fetch_sub_explicit(&idm_gc_marking_heap_count, 1u, memory_order_acq_rel);
    IdmObject *obj = heap->objects;
    while (obj) {
        IdmObject *next = obj->next;
        object_free(obj);
        obj = next;
    }
    heap->objects = NULL;
    heap->grey = NULL;
    heap->sweep = NULL;
    heap->object_count = 0;
    heap->bytes_allocated = 0;
    pthread_mutex_destroy(&heap->lock);
}

static IdmObject *value_object(IdmValue value) {
    return idm_is_boxed(value) ? idm_boxed_object(value) : NULL;
}

IdmValueTag idm_boxed_value_tag(IdmValue value) {
    switch (idm_boxed_object(value)->kind) {
        case IDM_OBJ_STRING: return IDM_VAL_STRING;
        case IDM_OBJ_ROPE: return IDM_VAL_STRING;
        case IDM_OBJ_PAIR: return IDM_VAL_PAIR;
        case IDM_OBJ_TUPLE: return IDM_VAL_TUPLE;
        case IDM_OBJ_VECTOR: return IDM_VAL_VECTOR;
        case IDM_OBJ_DICT: return IDM_VAL_DICT;
        case IDM_OBJ_DICT_NODE: return IDM_VAL_DICT;
        case IDM_OBJ_SYNTAX: return IDM_VAL_SYNTAX;
        case IDM_OBJ_CELL: return IDM_VAL_CELL;
        case IDM_OBJ_CLOSURE: return IDM_VAL_CLOSURE;
        case IDM_OBJ_RECORD: return IDM_VAL_RECORD;
        case IDM_OBJ_REGEX: return IDM_VAL_REGEX;
        case IDM_OBJ_REGEX_RESULT: return IDM_VAL_REGEX_RESULT;
        case IDM_OBJ_FLONUM: return IDM_VAL_FLOAT;
        case IDM_OBJ_BIGNUM: return IDM_VAL_BIGNUM;
        case IDM_OBJ_BITSTRING: return IDM_VAL_BITSTRING;
        case IDM_OBJ_COUNT: break;
    }
    return IDM_VAL_NIL;
}

double idm_float_value(IdmValue value) {
    return idm_boxed_object(value)->as.flonum;
}

IdmSymbol *idm_value_symbol(IdmValue value) {
    return (IdmSymbol *)(uintptr_t)(idm_immediate_payload(value) << 3);
}

uint64_t idm_value_id(IdmValue value) {
    return idm_immediate_payload(value);
}

static bool gc_grey_value_unlocked(IdmHeap *heap, IdmValue value) {
    IdmObject *obj = value_object(value);
    if (!obj || obj->heap != heap || obj->mark == heap->mark) return false;
    obj->mark = heap->mark;
    obj->greyed = true;
    obj->scan = 0;
    heap_push_grey_unlocked(heap, obj);
    return true;
}

static size_t gc_object_child_count(const IdmObject *obj) {
    if (!obj) return 0;
    switch (obj->kind) {
        case IDM_OBJ_PAIR:
        case IDM_OBJ_ROPE:
            return 2u;
        case IDM_OBJ_TUPLE:
        case IDM_OBJ_VECTOR:
            return obj->as.sequence.count;
        case IDM_OBJ_DICT:
            return 1u;
        case IDM_OBJ_DICT_NODE:
            return obj->as.dict_node.collision_count != 0
                ? (size_t)obj->as.dict_node.collision_count * 2u
                : (size_t)__builtin_popcount(obj->as.dict_node.datamap) * 2u + (size_t)__builtin_popcount(obj->as.dict_node.nodemap);
        case IDM_OBJ_CELL:
            return 1u;
        case IDM_OBJ_CLOSURE:
            return obj->as.closure.capture_count;
        case IDM_OBJ_RECORD:
            return obj->as.record.shape ? obj->as.record.shape->field_count : 0u;
        case IDM_OBJ_REGEX_RESULT:
            return 1u;
        case IDM_OBJ_BITSTRING:
            return idm_is_nil(obj->as.bits.parent) ? 0u : 1u;
        default:
            return 0;
    }
}

static IdmValue gc_object_child_at(const IdmObject *obj, size_t index) {
    if (obj->kind == IDM_OBJ_PAIR) return index == 0 ? obj->as.pair.car : obj->as.pair.cdr;
    if (obj->kind == IDM_OBJ_ROPE) return index == 0 ? obj->as.rope.left : obj->as.rope.right;
    if (obj->kind == IDM_OBJ_TUPLE || obj->kind == IDM_OBJ_VECTOR) return obj->as.sequence.items[index];
    if (obj->kind == IDM_OBJ_DICT) return obj->as.dict.root;
    if (obj->kind == IDM_OBJ_DICT_NODE) return obj->as.dict_node.slots[index];
    if (obj->kind == IDM_OBJ_CELL) return obj->as.cell.value;
    if (obj->kind == IDM_OBJ_CLOSURE) return obj->as.closure.captures[index];
    if (obj->kind == IDM_OBJ_RECORD) return obj->as.record.field_values[index];
    if (obj->kind == IDM_OBJ_REGEX_RESULT) return idm_regex_result_subject_value(obj->as.regex_result);
    if (obj->kind == IDM_OBJ_BITSTRING) return obj->as.bits.parent;
    return idm_nil();
}

static void gc_trace_object(IdmHeap *heap, IdmObject *obj, int64_t *budget) {
    if (!obj || !budget || *budget <= 0 || obj->mark != heap->mark || !obj->greyed) return;
    size_t child_count = gc_object_child_count(obj);
    if (child_count == 0) {
        obj->greyed = false;
        obj->scan = 0;
        (*budget)--;
        return;
    }
    while (obj->scan < child_count && *budget > 0) {
        gc_grey_value_unlocked(heap, gc_object_child_at(obj, obj->scan));
        obj->scan++;
        (*budget)--;
    }
    if (obj->scan == child_count) {
        obj->greyed = false;
        obj->scan = 0;
    } else {
        heap_push_grey_unlocked(heap, obj);
    }
}

void idm_gc_mark_value(IdmHeap *heap, IdmValue value) {
    heap_lock(heap);
    gc_grey_value_unlocked(heap, value);
    heap_unlock(heap);
}

size_t idm_heap_bytes(const IdmHeap *heap) {
    IdmHeap *h = (IdmHeap *)heap;
    heap_lock(h);
    size_t n = h->bytes_allocated;
    heap_unlock(h);
    return n;
}

size_t idm_heap_total_allocs(const IdmHeap *heap) {
    IdmHeap *h = (IdmHeap *)heap;
    heap_lock(h);
    size_t n = h->total_allocs;
    heap_unlock(h);
    return n;
}

size_t idm_heap_total_alloc_bytes(const IdmHeap *heap) {
    IdmHeap *h = (IdmHeap *)heap;
    heap_lock(h);
    size_t n = h->total_alloc_bytes;
    heap_unlock(h);
    return n;
}


void idm_heap_gc_begin(IdmHeap *heap) {
    heap_lock(heap);
    if (!atomic_load_explicit(&heap->gc_marking, memory_order_acquire) && !atomic_load_explicit(&heap->gc_sweeping, memory_order_acquire)) {
        atomic_fetch_add_explicit(&idm_gc_marking_heap_count, 1u, memory_order_acq_rel);
        heap->mark ^= 1u;
        heap->grey = NULL;
        heap->sweep = NULL;
        atomic_store_explicit(&heap->gc_marking, true, memory_order_release);
        atomic_store_explicit(&heap->gc_sweeping, false, memory_order_release);
    }
    heap_unlock(heap);
}

void idm_heap_gc_cancel(IdmHeap *heap) {
    heap_lock(heap);
    bool marking = atomic_exchange_explicit(&heap->gc_marking, false, memory_order_acq_rel);
    atomic_store_explicit(&heap->gc_sweeping, false, memory_order_release);
    heap->grey = NULL;
    heap->sweep = NULL;
    if (marking) atomic_fetch_sub_explicit(&idm_gc_marking_heap_count, 1u, memory_order_acq_rel);
    heap_unlock(heap);
}

bool idm_heap_gc_step(IdmHeap *heap, int64_t *budget) {
    if (!budget || *budget <= 0) return !idm_heap_gc_active(heap);
    heap_lock(heap);
    while (*budget > 0) {
        if (heap->grey) {
            IdmObject *obj = heap->grey;
            heap->grey = obj->grey_next;
            obj->grey_next = NULL;
            gc_trace_object(heap, obj, budget);
            continue;
        }
        if (atomic_load_explicit(&heap->gc_marking, memory_order_acquire)) {
            atomic_store_explicit(&heap->gc_marking, false, memory_order_release);
            atomic_fetch_sub_explicit(&idm_gc_marking_heap_count, 1u, memory_order_acq_rel);
            atomic_store_explicit(&heap->gc_sweeping, true, memory_order_release);
            heap->sweep = &heap->objects;
            (*budget)--;
            continue;
        }
        if (!atomic_load_explicit(&heap->gc_sweeping, memory_order_acquire)) {
            heap_unlock(heap);
            return true;
        }
        if (!heap->sweep || !*heap->sweep) {
            heap->sweep = NULL;
            atomic_store_explicit(&heap->gc_sweeping, false, memory_order_release);
            heap_unlock(heap);
            return true;
        }
        IdmObject *obj = *heap->sweep;
        if (obj->mark == heap->mark) {
            obj->greyed = false;
            obj->grey_next = NULL;
            obj->scan = 0;
            heap->sweep = &obj->next;
        } else {
            *heap->sweep = obj->next;
            heap->object_count--;
            heap->bytes_allocated -= obj->bytes < heap->bytes_allocated ? obj->bytes : heap->bytes_allocated;
            object_free(obj);
        }
        (*budget)--;
    }
    bool done = !atomic_load_explicit(&heap->gc_marking, memory_order_acquire) && !atomic_load_explicit(&heap->gc_sweeping, memory_order_acquire);
    heap_unlock(heap);
    return done;
}

bool idm_heap_gc_active(const IdmHeap *heap) {
    return atomic_load_explicit(&heap->gc_marking, memory_order_acquire) || atomic_load_explicit(&heap->gc_sweeping, memory_order_acquire);
}

void idm_gc_write_barrier_slow(IdmValue old_value) {
    IdmObject *obj = value_object(old_value);
    if (!obj || !obj->heap) return;
    IdmHeap *heap = obj->heap;
    if (!atomic_load_explicit(&heap->gc_marking, memory_order_acquire)) return;
    heap_lock(heap);
    if (atomic_load_explicit(&heap->gc_marking, memory_order_acquire)) gc_grey_value_unlocked(heap, old_value);
    heap_unlock(heap);
}

size_t idm_heap_object_count(const IdmHeap *heap) {
    IdmHeap *h = (IdmHeap *)heap;
    heap_lock(h);
    size_t n = h->object_count;
    heap_unlock(h);
    return n;
}


typedef struct {
    uint32_t buf[IDM_BIGNUM_I64_LIMBS];
    const uint32_t *limbs;
    size_t count;
    int sign;
} IntView;

static bool size_mul(size_t a, size_t b, size_t *out) {
    if (a != 0u && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

static bool size_add(size_t a, size_t b, size_t *out) {
    if (b > SIZE_MAX - a) return false;
    *out = a + b;
    return true;
}

static bool bignum_count_ok(size_t count, IdmError *err) {
    if (count <= UINT32_MAX) return true;
    return idm_error_set(err, idm_span_unknown(NULL), "integer is too large");
}

static bool bignum_limb_bytes(size_t count, size_t *out, IdmError *err) {
    if (!bignum_count_ok(count, err)) return false;
    if (size_mul(count, sizeof(uint32_t), out)) return true;
    return idm_error_set(err, idm_span_unknown(NULL), "integer is too large");
}

static void int_view(IdmValue v, IntView *iv) {
    if (idm_value_tag(v) == IDM_VAL_BIGNUM) {
        const IdmObject *o = idm_boxed_object(v);
        iv->limbs = o->as.bignum.limbs;
        iv->count = o->as.bignum.count;
        iv->sign = (int)o->as.bignum.sign;
    } else {
        iv->count = idm_bignum_from_i64(idm_int_value(v), iv->buf, &iv->sign);
        iv->limbs = iv->buf;
    }
}

static IdmValue make_bignum_from_mag(IdmRuntime *rt, const uint32_t *limbs, size_t count, int sign, IdmError *err) {
    int64_t v = 0;
    if (idm_bignum_fits_i64(limbs, count, sign, &v) && idm_fixnum_fits(v)) return idm_int(v);
    size_t bytes = 0;
    if (!bignum_limb_bytes(count, &bytes, err)) return idm_nil();
    IdmObject *obj = heap_alloc_payload(idm_active_heap(rt), IDM_OBJ_BIGNUM, bytes);
    if (!obj) { idm_error_oom(err, idm_span_unknown(NULL)); return idm_nil(); }
    uint32_t *copy = bytes == 0u ? NULL : (uint32_t *)(obj + 1);
    if (bytes != 0u) memcpy(copy, limbs, bytes);
    obj->as.bignum.limbs = copy;
    obj->as.bignum.count = (uint32_t)count;
    obj->as.bignum.sign = (int32_t)sign;
    return idm_from_boxed(obj);
}

static IdmValue make_bignum_adopt(IdmRuntime *rt, uint32_t *limbs, size_t count, int sign, IdmError *err) {
    int64_t v = 0;
    if (idm_bignum_fits_i64(limbs, count, sign, &v) && idm_fixnum_fits(v)) { free(limbs); return idm_int(v); }
    size_t bytes = 0;
    if (!bignum_limb_bytes(count, &bytes, err)) { free(limbs); return idm_nil(); }
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_BIGNUM);
    if (!obj) { free(limbs); idm_error_oom(err, idm_span_unknown(NULL)); return idm_nil(); }
    obj->as.bignum.limbs = limbs;
    obj->as.bignum.count = (uint32_t)count;
    obj->as.bignum.sign = (int32_t)sign;
    heap_account(idm_active_heap(rt), obj, bytes);
    return idm_from_boxed(obj);
}

IdmValue idm_int_promote(IdmRuntime *rt, int64_t value, IdmError *err) {
    if (idm_fixnum_fits(value)) return idm_int(value);
    uint32_t buf[IDM_BIGNUM_I64_LIMBS];
    int sign = 0;
    size_t n = idm_bignum_from_i64(value, buf, &sign);
    return make_bignum_from_mag(rt, buf, n, sign, err);
}

bool idm_value_is_int(IdmValue v) {
    IdmValueTag t = idm_value_tag(v);
    return t == IDM_VAL_INT || t == IDM_VAL_BIGNUM;
}

static bool int_binop(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err, int kind) {
    IntView x, y;
    int_view(a, &x);
    int_view(b, &y);
    size_t cap = 0;
    if (kind == 2) {
        if (!size_add(x.count, y.count, &cap)) return idm_error_set(err, idm_span_unknown(NULL), "integer is too large");
    } else if (!size_add(x.count > y.count ? x.count : y.count, 1u, &cap)) {
        return idm_error_set(err, idm_span_unknown(NULL), "integer is too large");
    }
    if (cap == 0u) cap = 1u;
    size_t bytes = 0;
    if (!bignum_limb_bytes(cap, &bytes, err)) return false;
    uint32_t *r = malloc(bytes);
    if (!r) { idm_error_oom(err, idm_span_unknown(NULL)); return false; }
    size_t rn = 0u;
    int rs = 0;
    if (kind == 0) idm_bignum_add(x.limbs, x.count, x.sign, y.limbs, y.count, y.sign, r, &rn, &rs);
    else if (kind == 1) idm_bignum_sub(x.limbs, x.count, x.sign, y.limbs, y.count, y.sign, r, &rn, &rs);
    else idm_bignum_mul(x.limbs, x.count, x.sign, y.limbs, y.count, y.sign, r, &rn, &rs);
    *out = make_bignum_adopt(rt, r, rn, rs, err);
    return !(err && err->present);
}

bool idm_int_add(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err) { return int_binop(rt, a, b, out, err, 0); }
bool idm_int_sub(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err) { return int_binop(rt, a, b, out, err, 1); }
bool idm_int_mul(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err) { return int_binop(rt, a, b, out, err, 2); }

bool idm_int_divmod(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *q_out, IdmValue *r_out, IdmError *err) {
    IntView x, y;
    int_view(a, &x);
    int_view(b, &y);
    if (y.sign == 0) return idm_error_set(err, idm_span_unknown(NULL), "division by zero");
    size_t qcap = x.count ? x.count : 1u, rcap = 0u;
    if (!size_add(y.count, 1u, &rcap)) return idm_error_set(err, idm_span_unknown(NULL), "integer is too large");
    size_t qbytes = 0, rbytes = 0;
    if (!bignum_limb_bytes(qcap, &qbytes, err) || !bignum_limb_bytes(rcap, &rbytes, err)) return false;
    uint32_t *q = malloc(qbytes);
    uint32_t *r = malloc(rbytes);
    if (!q || !r) { free(q); free(r); idm_error_oom(err, idm_span_unknown(NULL)); return false; }
    size_t qn = 0u, rn = 0u;
    int qs = 0, rs = 0;
    if (!idm_bignum_divmod(x.limbs, x.count, x.sign, y.limbs, y.count, y.sign, q, &qn, &qs, r, &rn, &rs)) {
        free(q);
        free(r);
        idm_error_oom(err, idm_span_unknown(NULL));
        return false;
    }
    if (q_out) *q_out = make_bignum_adopt(rt, q, qn, qs, err); else free(q);
    if (r_out && !(err && err->present)) *r_out = make_bignum_adopt(rt, r, rn, rs, err); else free(r);
    return !(err && err->present);
}

bool idm_int_pow(IdmRuntime *rt, IdmValue base, int64_t exponent, IdmValue *out, IdmError *err) {
    IdmValue result = idm_int(1);
    IdmValue b = base;
    while (exponent > 0) {
        if (exponent & 1) { if (!idm_int_mul(rt, result, b, &result, err)) return false; }
        exponent >>= 1;
        if (exponent > 0) { if (!idm_int_mul(rt, b, b, &b, err)) return false; }
    }
    *out = result;
    return true;
}

bool idm_int_neg(IdmRuntime *rt, IdmValue v, IdmValue *out, IdmError *err) {
    IntView x;
    int_view(v, &x);
    *out = make_bignum_from_mag(rt, x.limbs, x.count, -x.sign, err);
    return !(err && err->present);
}

int idm_int_compare(IdmValue a, IdmValue b) {
    IntView x, y;
    int_view(a, &x);
    int_view(b, &y);
    return idm_bignum_cmp(x.limbs, x.count, x.sign, y.limbs, y.count, y.sign);
}

double idm_int_to_double(IdmValue v) {
    if (idm_value_tag(v) == IDM_VAL_BIGNUM) {
        const IdmObject *o = idm_boxed_object(v);
        return idm_bignum_to_double(o->as.bignum.limbs, o->as.bignum.count, (int)o->as.bignum.sign);
    }
    return (double)idm_int_value(v);
}

bool idm_int_to_i64(IdmValue v, int64_t *out) {
    if (idm_value_tag(v) == IDM_VAL_INT) { *out = idm_int_value(v); return true; }
    if (idm_value_tag(v) == IDM_VAL_BIGNUM) {
        const IdmObject *o = idm_boxed_object(v);
        return idm_bignum_fits_i64(o->as.bignum.limbs, o->as.bignum.count, (int)o->as.bignum.sign, out);
    }
    return false;
}

bool idm_bignum_view(IdmValue value, const uint32_t **limbs, size_t *count, int *sign) {
    if (idm_value_tag(value) != IDM_VAL_BIGNUM) return false;
    const IdmObject *o = idm_boxed_object(value);
    if (limbs) *limbs = o->as.bignum.limbs;
    if (count) *count = o->as.bignum.count;
    if (sign) *sign = (int)o->as.bignum.sign;
    return true;
}

IdmValue idm_int_shl(IdmRuntime *rt, IdmValue v, int64_t bits, IdmError *err) {
    if (bits < 0) { idm_error_set(err, idm_span_unknown(NULL), "negative integer shift"); return idm_nil(); }
    if (bits == 0) return v;
    if (idm_is_fixnum(v) && bits < 62) {
        int64_t i = idm_fixnum_value(v);
        if (i == 0) return idm_int(0);
        int64_t shifted = (int64_t)((uint64_t)i << bits);
        if ((shifted >> bits) == i && idm_fixnum_fits(shifted)) return idm_fixnum(shifted);
    }
    IntView x;
    int_view(v, &x);
    if (x.count == 0u) return idm_int(0);
    size_t lshift = (size_t)(bits / 32), bshift = (size_t)(bits % 32);
    size_t out_count = 0;
    if (!size_add(x.count, lshift, &out_count) || !size_add(out_count, 1u, &out_count)) {
        idm_error_set(err, idm_span_unknown(NULL), "integer is too large");
        return idm_nil();
    }
    size_t bytes = 0;
    if (!bignum_limb_bytes(out_count, &bytes, err)) return idm_nil();
    uint32_t *out = calloc(out_count, sizeof(uint32_t));
    if (!out) { idm_error_oom(err, idm_span_unknown(NULL)); return idm_nil(); }
    for (size_t i = 0u; i < x.count; i++) {
        uint64_t val = (uint64_t)x.limbs[i] << bshift;
        out[i + lshift] |= (uint32_t)val;
        out[i + lshift + 1u] |= (uint32_t)(val >> 32);
    }
    while (out_count > 0u && out[out_count - 1u] == 0u) out_count--;
    return make_bignum_adopt(rt, out, out_count, x.sign, err);
}

static size_t limb_normalize(const uint32_t *limbs, size_t count) {
    while (count > 0u && limbs[count - 1u] == 0u) count--;
    return count;
}

IdmValue idm_int_from_limbs(IdmRuntime *rt, const uint32_t *limbs, size_t count, int sign, IdmError *err) {
    if (count != 0u && !limbs) {
        idm_error_set(err, idm_span_unknown(NULL), "integer limbs are missing");
        return idm_nil();
    }
    if (sign < -1 || sign > 1) {
        idm_error_set(err, idm_span_unknown(NULL), "invalid integer sign");
        return idm_nil();
    }
    count = limb_normalize(limbs, count);
    if (count == 0u) return idm_int(0);
    if (sign == 0) {
        idm_error_set(err, idm_span_unknown(NULL), "nonzero integer magnitude requires a sign");
        return idm_nil();
    }
    return make_bignum_from_mag(rt, limbs, count, sign, err);
}

static bool alloc_zero_limbs(size_t count, uint32_t **out, IdmError *err) {
    size_t bytes = 0;
    if (!bignum_limb_bytes(count, &bytes, err)) return false;
    uint32_t *limbs = calloc(count ? count : 1u, sizeof(uint32_t));
    if (!limbs) { idm_error_oom(err, idm_span_unknown(NULL)); return false; }
    *out = limbs;
    return true;
}

static IdmValue make_bignum_adopt_trim(IdmRuntime *rt, uint32_t *limbs, size_t count, int sign, size_t alloc_count, IdmError *err) {
    int64_t v = 0;
    if (idm_bignum_fits_i64(limbs, count, sign, &v) && idm_fixnum_fits(v)) { free(limbs); return idm_int(v); }
    if (count < alloc_count) {
        size_t bytes = 0;
        if (!bignum_limb_bytes(count, &bytes, err)) { free(limbs); return idm_nil(); }
        uint32_t *copy = malloc(bytes);
        if (!copy) { free(limbs); idm_error_oom(err, idm_span_unknown(NULL)); return idm_nil(); }
        memcpy(copy, limbs, bytes);
        free(limbs);
        limbs = copy;
    }
    return make_bignum_adopt(rt, limbs, count, sign, err);
}

static void twos_from_int(IdmValue value, uint32_t *out, size_t width) {
    IntView x;
    int_view(value, &x);
    size_t n = x.count < width ? x.count : width;
    if (n != 0u) memcpy(out, x.limbs, n * sizeof(uint32_t));
    if (x.sign >= 0) return;
    for (size_t i = 0u; i < width; i++) out[i] = ~out[i];
    uint64_t carry = 1u;
    for (size_t i = 0u; i < width && carry != 0u; i++) {
        uint64_t v = (uint64_t)out[i] + carry;
        out[i] = (uint32_t)v;
        carry = v >> 32;
    }
}

static IdmValue int_from_twos(IdmRuntime *rt, uint32_t *limbs, size_t width, IdmError *err) {
    if (width == 0u) { free(limbs); return idm_int(0); }
    int sign = (limbs[width - 1u] & 0x80000000u) != 0u ? -1 : 1;
    if (sign < 0) {
        for (size_t i = 0u; i < width; i++) limbs[i] = ~limbs[i];
        uint64_t carry = 1u;
        for (size_t i = 0u; i < width && carry != 0u; i++) {
            uint64_t v = (uint64_t)limbs[i] + carry;
            limbs[i] = (uint32_t)v;
            carry = v >> 32;
        }
    }
    size_t count = limb_normalize(limbs, width);
    if (count == 0u) sign = 0;
    return make_bignum_adopt_trim(rt, limbs, count, sign, width, err);
}

static bool int_bit_binary(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err, int kind) {
    if (idm_is_fixnum(a) && idm_is_fixnum(b)) {
        int64_t xa = idm_fixnum_value(a), yb = idm_fixnum_value(b);
        int64_t r = kind == 0 ? (xa & yb) : kind == 1 ? (xa | yb) : (xa ^ yb);
        *out = idm_fixnum(r);
        return true;
    }
    IntView x, y;
    int_view(a, &x);
    int_view(b, &y);
    size_t max = x.count > y.count ? x.count : y.count;
    size_t width = 0u;
    if (!size_add(max, 1u, &width)) return idm_error_set(err, idm_span_unknown(NULL), "integer is too large");
    uint32_t *xa = NULL, *yb = NULL, *result = NULL;
    if (!alloc_zero_limbs(width, &xa, err) || !alloc_zero_limbs(width, &yb, err) || !alloc_zero_limbs(width, &result, err)) {
        free(xa);
        free(yb);
        free(result);
        return false;
    }
    twos_from_int(a, xa, width);
    twos_from_int(b, yb, width);
    for (size_t i = 0u; i < width; i++) {
        if (kind == 0) result[i] = xa[i] & yb[i];
        else if (kind == 1) result[i] = xa[i] | yb[i];
        else result[i] = xa[i] ^ yb[i];
    }
    free(xa);
    free(yb);
    *out = int_from_twos(rt, result, width, err);
    return !(err && err->present);
}

bool idm_int_bit_and(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err) { return int_bit_binary(rt, a, b, out, err, 0); }
bool idm_int_bit_or(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err) { return int_bit_binary(rt, a, b, out, err, 1); }
bool idm_int_bit_xor(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err) { return int_bit_binary(rt, a, b, out, err, 2); }

bool idm_int_bit_not(IdmRuntime *rt, IdmValue v, IdmValue *out, IdmError *err) {
    IntView x;
    int_view(v, &x);
    size_t width = 0u;
    if (!size_add(x.count, 1u, &width)) return idm_error_set(err, idm_span_unknown(NULL), "integer is too large");
    uint32_t *bits = NULL;
    if (!alloc_zero_limbs(width, &bits, err)) return false;
    twos_from_int(v, bits, width);
    for (size_t i = 0u; i < width; i++) bits[i] = ~bits[i];
    *out = int_from_twos(rt, bits, width, err);
    return !(err && err->present);
}

static uint32_t popcount_u32(uint32_t value) {
    uint32_t count = 0u;
    while (value != 0u) {
        count += value & 1u;
        value >>= 1;
    }
    return count;
}

bool idm_int_bit_count_nonnegative(IdmRuntime *rt, IdmValue v, IdmValue *out, IdmError *err) {
    IntView x;
    int_view(v, &x);
    uint64_t count = 0u;
    for (size_t i = 0u; i < x.count; i++) count += popcount_u32(x.limbs[i]);
    *out = idm_int_promote(rt, (int64_t)count, err);
    return !(err && err->present);
}

bool idm_int_bit_length_nonnegative(IdmRuntime *rt, IdmValue v, IdmValue *out, IdmError *err) {
    IntView x;
    int_view(v, &x);
    if (x.count == 0u) {
        *out = idm_int(0);
        return true;
    }
    uint32_t hi = x.limbs[x.count - 1u];
    uint64_t bits = (uint64_t)(x.count - 1u) * 32u;
    while (hi != 0u) {
        bits++;
        hi >>= 1;
    }
    *out = idm_int_promote(rt, (int64_t)bits, err);
    return !(err && err->present);
}

IdmValue idm_int_from_decimal(IdmRuntime *rt, const char *text, size_t len, bool *ok, IdmError *err) {
    size_t cap = idm_bignum_decimal_limb_cap(len);
    size_t bytes = 0;
    if (!bignum_limb_bytes(cap, &bytes, err)) { if (ok) *ok = false; return idm_nil(); }
    uint32_t *limbs = malloc(bytes);
    if (!limbs) { idm_error_oom(err, idm_span_unknown(NULL)); if (ok) *ok = false; return idm_nil(); }
    size_t count = 0u;
    int sign = 0;
    bool parsed = idm_bignum_from_decimal(text, len, limbs, &count, &sign);
    IdmValue v = idm_nil();
    if (parsed) v = make_bignum_adopt(rt, limbs, count, sign, err);
    else free(limbs);
    if (ok) *ok = parsed;
    return v;
}

static bool bignum_write(IdmBuffer *buf, IdmValue value) {
    const IdmObject *o = idm_boxed_object(value);
    size_t count = o->as.bignum.count;
    size_t bytes = 0;
    if (!size_mul(count, sizeof(uint32_t), &bytes)) return false;
    uint32_t *scratch = bytes == 0u ? NULL : malloc(bytes);
    if (bytes != 0u && !scratch) return false;
    if (bytes != 0u) memcpy(scratch, o->as.bignum.limbs, bytes);
    size_t digit_cap = idm_bignum_decimal_digit_cap(count);
    size_t alloc = 0;
    if (!size_add(digit_cap, 2u, &alloc)) { free(scratch); return false; }
    char *digits = malloc(alloc);
    if (!digits) { free(scratch); return false; }
    size_t len = idm_bignum_to_decimal(scratch, count, (int)o->as.bignum.sign, digits);
    bool ok = idm_buf_append_n(buf, digits, len);
    free(scratch);
    free(digits);
    return ok;
}

IdmValue idm_float(IdmRuntime *rt, double value, IdmError *err) {
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_FLONUM);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.flonum = value;
    return idm_from_boxed(obj);
}

IdmValue idm_word(IdmRuntime *rt, const char *text) {
    return idm_immediate(IDM_IMM_WORD, (uint64_t)((uintptr_t)idm_intern(&rt->intern, IDM_SYMBOL_WORD, text) >> 3));
}

IdmValue idm_atom(IdmRuntime *rt, const char *text) {
    return idm_immediate(IDM_IMM_ATOM, (uint64_t)((uintptr_t)idm_intern(&rt->intern, IDM_SYMBOL_ATOM, text) >> 3));
}

IdmValue idm_atom_symbol(IdmSymbol *symbol) {
    return symbol && (symbol->kind == IDM_SYMBOL_ATOM || symbol->kind == IDM_SYMBOL_IDENTITY)
        ? idm_immediate(IDM_IMM_ATOM, (uint64_t)((uintptr_t)symbol >> 3))
        : idm_nil();
}

IdmValue idm_bool(IdmRuntime *rt, bool value) {
    return value ? rt->cached_true : rt->cached_false;
}

#define IDM_STRING_LEAF_MAX 2048u
#define IDM_ROPE_MAX_HEIGHT 48u
#define IDM_ROPE_ITER_DEPTH 64

static size_t string_val_length(IdmValue v) {
    const IdmObject *obj = idm_boxed_object(v);
    return obj->kind == IDM_OBJ_STRING ? obj->as.string.len : obj->as.rope.len;
}

static uint16_t string_val_height(IdmValue v) {
    const IdmObject *obj = idm_boxed_object(v);
    return obj->kind == IDM_OBJ_STRING ? 0u : obj->as.rope.height;
}

void idm_string_iter_init(IdmValue v, IdmStringIter *it) {
    it->depth = 0;
    if (idm_value_tag(v) != IDM_VAL_STRING) return;
    it->stack[it->depth++] = idm_boxed_object(v);
}

bool idm_string_iter_next(IdmStringIter *it, const char **bytes, size_t *len) {
    while (it->depth > 0) {
        const IdmObject *obj = it->stack[--it->depth];
        if (obj->kind == IDM_OBJ_ROPE) {
            if (it->depth + 2 > IDM_ROPE_ITER_DEPTH) {
                it->depth = 0;
                return false;
            }
            it->stack[it->depth++] = idm_boxed_object(obj->as.rope.right);
            it->stack[it->depth++] = idm_boxed_object(obj->as.rope.left);
            continue;
        }
        if (obj->as.string.len == 0) continue;
        *bytes = obj->as.string.bytes;
        *len = obj->as.string.len;
        return true;
    }
    return false;
}

static const char *rope_flatten(IdmObject *obj) {
    char *cur = atomic_load_explicit(&obj->as.rope.flat, memory_order_acquire);
    if (cur) return cur;
    char *buf = malloc(obj->as.rope.len + 1u);
    if (!buf) return NULL;
    IdmStringIter it;
    idm_string_iter_init(idm_from_boxed(obj), &it);
    size_t at = 0;
    const char *chunk = NULL;
    size_t chunk_len = 0;
    while (idm_string_iter_next(&it, &chunk, &chunk_len)) {
        memcpy(buf + at, chunk, chunk_len);
        at += chunk_len;
    }
    buf[obj->as.rope.len] = '\0';
    char *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(&obj->as.rope.flat, &expected, buf, memory_order_acq_rel, memory_order_acquire)) {
        free(buf);
        return expected;
    }
    heap_account(obj->heap, obj, obj->as.rope.len + 1u);
    return buf;
}

static IdmValue string_leaf_n(IdmRuntime *rt, const char *text, size_t len, IdmError *err) {
    IdmObject *obj = heap_alloc_payload(idm_active_heap(rt), IDM_OBJ_STRING, len + 1u);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    char *bytes = (char *)(obj + 1);
    memcpy(bytes, text, len);
    bytes[len] = '\0';
    obj->as.string.bytes = bytes;
    obj->as.string.len = len;
    return idm_from_boxed(obj);
}

static size_t leaf_newline_count(const char *bytes, size_t len) {
    size_t count = 0;
    const char *at = bytes;
    const char *end = bytes + len;
    while (at < end) {
        const char *hit = memchr(at, '\n', (size_t)(end - at));
        if (!hit) break;
        count++;
        at = hit + 1;
    }
    return count;
}

static size_t string_val_newlines(IdmValue v) {
    const IdmObject *obj = idm_boxed_object(v);
    if (obj->kind == IDM_OBJ_ROPE) return obj->as.rope.newlines;
    return leaf_newline_count(obj->as.string.bytes, obj->as.string.len);
}

static IdmValue rope_node(IdmRuntime *rt, IdmValue left, IdmValue right, IdmError *err) {
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_ROPE);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.rope.left = left;
    obj->as.rope.right = right;
    obj->as.rope.len = string_val_length(left) + string_val_length(right);
    obj->as.rope.newlines = string_val_newlines(left) + string_val_newlines(right);
    uint16_t lh = string_val_height(left);
    uint16_t rh = string_val_height(right);
    obj->as.rope.height = (lh > rh ? lh : rh) + 1u;
    atomic_init(&obj->as.rope.flat, NULL);
    return idm_from_boxed(obj);
}

static IdmValue string_build_chunked(IdmRuntime *rt, const char *text, size_t len, IdmError *err) {
    if (len <= IDM_STRING_LEAF_MAX) return string_leaf_n(rt, text, len, err);
    size_t half = len / 2u;
    IdmValue left = string_build_chunked(rt, text, half, err);
    if (err && err->present) return idm_nil();
    IdmValue right = string_build_chunked(rt, text + half, len - half, err);
    if (err && err->present) return idm_nil();
    return rope_node(rt, left, right, err);
}

IdmValue idm_string_n(IdmRuntime *rt, const char *text, size_t len, IdmError *err) {
    if (len > IDM_STRING_LEAF_MAX) return string_build_chunked(rt, text, len, err);
    return string_leaf_n(rt, text, len, err);
}

static IdmValue rope_rebuild_range(IdmRuntime *rt, IdmValue *leaves, size_t count, IdmError *err) {
    if (count == 1u) return leaves[0];
    size_t mid = count / 2u;
    IdmValue left = rope_rebuild_range(rt, leaves, mid, err);
    if (err && err->present) return idm_nil();
    IdmValue right = rope_rebuild_range(rt, leaves + mid, count - mid, err);
    if (err && err->present) return idm_nil();
    return rope_node(rt, left, right, err);
}

static bool rope_rebalance(IdmRuntime *rt, IdmValue v, IdmValue *out, IdmError *err) {
    IdmValue *leaves = NULL;
    size_t count = 0;
    size_t cap = 0;
    IdmStringIter it;
    idm_string_iter_init(v, &it);
    const char *chunk = NULL;
    size_t chunk_len = 0;
    IdmBuffer pending;
    idm_buf_init(&pending);
    bool ok = true;
    while (ok && idm_string_iter_next(&it, &chunk, &chunk_len)) {
        if (pending.len != 0 && pending.len + chunk_len > IDM_STRING_LEAF_MAX) {
            IdmValue merged = string_leaf_n(rt, pending.data, pending.len, err);
            ok = !(err && err->present);
            if (ok && count == cap) ok = idm_grow((void **)&leaves, &cap, sizeof(*leaves), 32u, count + 1u);
            if (ok) leaves[count++] = merged;
            pending.len = 0;
        }
        if (!ok) break;
        if (chunk_len <= IDM_STRING_LEAF_MAX / 2u || pending.len != 0) {
            ok = idm_buf_append_n(&pending, chunk, chunk_len);
        } else {
            if (count == cap) ok = idm_grow((void **)&leaves, &cap, sizeof(*leaves), 32u, count + 1u);
            if (ok) {
                IdmValue leaf = string_leaf_n(rt, chunk, chunk_len, err);
                ok = !(err && err->present);
                if (ok) leaves[count++] = leaf;
            }
        }
    }
    if (ok && pending.len != 0) {
        IdmValue merged = string_leaf_n(rt, pending.data, pending.len, err);
        ok = !(err && err->present);
        if (ok && count == cap) ok = idm_grow((void **)&leaves, &cap, sizeof(*leaves), 32u, count + 1u);
        if (ok) leaves[count++] = merged;
    }
    idm_buf_destroy(&pending);
    if (ok && count == 0) {
        *out = string_leaf_n(rt, "", 0u, err);
        ok = !(err && err->present);
    } else if (ok) {
        *out = rope_rebuild_range(rt, leaves, count, err);
        ok = !(err && err->present);
    }
    free(leaves);
    if (!ok && err && !err->present) idm_error_oom(err, idm_span_unknown(NULL));
    return ok;
}

static IdmValue flat_join(IdmRuntime *rt, IdmValue a, IdmValue b, IdmError *err) {
    size_t la = string_val_length(a);
    size_t lb = string_val_length(b);
    IdmObject *obj = heap_alloc_payload(idm_active_heap(rt), IDM_OBJ_STRING, la + lb + 1u);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    char *bytes = (char *)(obj + 1);
    memcpy(bytes, idm_string_bytes(a), la);
    memcpy(bytes + la, idm_string_bytes(b), lb);
    bytes[la + lb] = '\0';
    obj->as.string.bytes = bytes;
    obj->as.string.len = la + lb;
    return idm_from_boxed(obj);
}

static size_t rope_edge_leaf_len(IdmValue v, bool rightmost) {
    const IdmObject *obj = idm_boxed_object(v);
    while (obj->kind == IDM_OBJ_ROPE) obj = idm_boxed_object(rightmost ? obj->as.rope.right : obj->as.rope.left);
    return obj->as.string.len;
}

static IdmValue rope_merge_edge(IdmRuntime *rt, IdmValue v, IdmValue small, bool rightmost, IdmError *err) {
    IdmObject *obj = idm_boxed_object(v);
    if (obj->kind == IDM_OBJ_STRING) {
        return rightmost ? flat_join(rt, v, small, err) : flat_join(rt, small, v, err);
    }
    if (rightmost) {
        IdmValue nr = rope_merge_edge(rt, obj->as.rope.right, small, true, err);
        if (err && err->present) return idm_nil();
        return rope_node(rt, obj->as.rope.left, nr, err);
    }
    IdmValue nl = rope_merge_edge(rt, obj->as.rope.left, small, false, err);
    if (err && err->present) return idm_nil();
    return rope_node(rt, nl, obj->as.rope.right, err);
}

bool idm_string_concat2(IdmRuntime *rt, IdmValue a, IdmValue b, IdmValue *out, IdmError *err) {
    size_t la = string_val_length(a);
    size_t lb = string_val_length(b);
    if (la == 0) { *out = b; return true; }
    if (lb == 0) { *out = a; return true; }
    if (la + lb <= IDM_STRING_LEAF_MAX) {
        *out = flat_join(rt, a, b, err);
        return !(err && err->present);
    }
    if (lb <= IDM_STRING_LEAF_MAX && string_val_height(a) != 0 && rope_edge_leaf_len(a, true) + lb <= IDM_STRING_LEAF_MAX) {
        *out = rope_merge_edge(rt, a, b, true, err);
        return !(err && err->present);
    }
    if (la <= IDM_STRING_LEAF_MAX && string_val_height(b) != 0 && rope_edge_leaf_len(b, false) + la <= IDM_STRING_LEAF_MAX) {
        *out = rope_merge_edge(rt, b, a, false, err);
        return !(err && err->present);
    }
    IdmValue node = rope_node(rt, a, b, err);
    if (err && err->present) return false;
    if (idm_boxed_object(node)->as.rope.height > IDM_ROPE_MAX_HEIGHT) return rope_rebalance(rt, node, out, err);
    *out = node;
    return true;
}

IdmValue idm_string_slice_value(IdmRuntime *rt, IdmValue v, size_t start, size_t end, IdmError *err) {
    IdmObject *obj = idm_boxed_object(v);
    size_t len = string_val_length(v);
    if (start == 0 && end == len) return v;
    if (obj->kind == IDM_OBJ_STRING) return string_leaf_n(rt, obj->as.string.bytes + start, end - start, err);
    size_t left_len = string_val_length(obj->as.rope.left);
    if (end <= left_len) return idm_string_slice_value(rt, obj->as.rope.left, start, end, err);
    if (start >= left_len) return idm_string_slice_value(rt, obj->as.rope.right, start - left_len, end - left_len, err);
    IdmValue left = idm_string_slice_value(rt, obj->as.rope.left, start, left_len, err);
    if (err && err->present) return idm_nil();
    IdmValue right = idm_string_slice_value(rt, obj->as.rope.right, 0u, end - left_len, err);
    if (err && err->present) return idm_nil();
    IdmValue out = idm_nil();
    if (!idm_string_concat2(rt, left, right, &out, err)) return idm_nil();
    return out;
}

bool idm_string_append_range(IdmBuffer *buf, IdmValue v, size_t start, size_t end) {
    if (idm_value_tag(v) != IDM_VAL_STRING || end < start) return false;
    IdmStringIter it;
    idm_string_iter_init(v, &it);
    const char *chunk = NULL;
    size_t chunk_len = 0;
    size_t at = 0;
    while (at < end && idm_string_iter_next(&it, &chunk, &chunk_len)) {
        size_t lo = start > at ? start - at : 0u;
        size_t hi = end - at < chunk_len ? end - at : chunk_len;
        if (lo < hi && !idm_buf_append_n(buf, chunk + lo, hi - lo)) return false;
        at += chunk_len;
    }
    return at >= end || start == end;
}

size_t idm_string_newlines_before(IdmValue v, size_t off) {
    if (idm_value_tag(v) != IDM_VAL_STRING) return 0;
    size_t count = 0;
    const IdmObject *obj = idm_boxed_object(v);
    for (;;) {
        if (obj->kind == IDM_OBJ_STRING) {
            size_t n = off < obj->as.string.len ? off : obj->as.string.len;
            return count + leaf_newline_count(obj->as.string.bytes, n);
        }
        if (off >= obj->as.rope.len) return count + obj->as.rope.newlines;
        size_t left_len = string_val_length(obj->as.rope.left);
        if (off <= left_len) {
            obj = idm_boxed_object(obj->as.rope.left);
        } else {
            count += string_val_newlines(obj->as.rope.left);
            off -= left_len;
            obj = idm_boxed_object(obj->as.rope.right);
        }
    }
}

bool idm_string_line_start(IdmValue v, size_t line, size_t *out_off) {
    if (idm_value_tag(v) != IDM_VAL_STRING) return false;
    if (line == 0) {
        *out_off = 0;
        return true;
    }
    size_t nth = line;
    size_t base = 0;
    const IdmObject *obj = idm_boxed_object(v);
    for (;;) {
        if (obj->kind == IDM_OBJ_STRING) {
            const char *at = obj->as.string.bytes;
            const char *end = at + obj->as.string.len;
            while (nth > 0 && at < end) {
                const char *hit = memchr(at, '\n', (size_t)(end - at));
                if (!hit) return false;
                nth--;
                at = hit + 1;
            }
            if (nth != 0) return false;
            *out_off = base + (size_t)(at - obj->as.string.bytes);
            return true;
        }
        if (nth > obj->as.rope.newlines) return false;
        size_t left_newlines = string_val_newlines(obj->as.rope.left);
        if (nth <= left_newlines) {
            obj = idm_boxed_object(obj->as.rope.left);
        } else {
            nth -= left_newlines;
            base += string_val_length(obj->as.rope.left);
            obj = idm_boxed_object(obj->as.rope.right);
        }
    }
}

int idm_string_byte_at(IdmValue v, size_t index) {
    if (idm_value_tag(v) != IDM_VAL_STRING) return -1;
    const IdmObject *obj = idm_boxed_object(v);
    for (;;) {
        if (obj->kind == IDM_OBJ_STRING) {
            if (index >= obj->as.string.len) return -1;
            return (unsigned char)obj->as.string.bytes[index];
        }
        size_t left_len = string_val_length(obj->as.rope.left);
        if (index < left_len) {
            obj = idm_boxed_object(obj->as.rope.left);
        } else {
            index -= left_len;
            obj = idm_boxed_object(obj->as.rope.right);
        }
    }
}

static bool string_equal_val(IdmValue a, IdmValue b) {
    size_t la = string_val_length(a);
    if (la != string_val_length(b)) return false;
    const IdmObject *oa = idm_boxed_object(a);
    const IdmObject *ob = idm_boxed_object(b);
    if (oa->kind == IDM_OBJ_STRING && ob->kind == IDM_OBJ_STRING) {
        return memcmp(oa->as.string.bytes, ob->as.string.bytes, la) == 0;
    }
    IdmStringIter ia, ib;
    idm_string_iter_init(a, &ia);
    idm_string_iter_init(b, &ib);
    const char *ca = NULL, *cb = NULL;
    size_t na = 0, nb = 0;
    for (;;) {
        if (na == 0 && !idm_string_iter_next(&ia, &ca, &na)) na = 0;
        if (nb == 0 && !idm_string_iter_next(&ib, &cb, &nb)) nb = 0;
        if (na == 0 || nb == 0) return na == 0 && nb == 0;
        size_t run = na < nb ? na : nb;
        if (memcmp(ca, cb, run) != 0) return false;
        ca += run;
        cb += run;
        na -= run;
        nb -= run;
    }
}

IdmValue idm_string(IdmRuntime *rt, const char *text, IdmError *err) {
    return idm_string_n(rt, text, strlen(text), err);
}

bool idm_value_append_display(IdmBuffer *buf, IdmValue v) {
    if (idm_value_tag(v) == IDM_VAL_STRING) return idm_buf_append_n(buf, idm_string_bytes(v), idm_string_length(v));
    if (idm_value_tag(v) == IDM_VAL_ATOM || idm_value_tag(v) == IDM_VAL_WORD) return idm_buf_append(buf, idm_symbol_text(idm_value_symbol(v)));
    if (idm_value_tag(v) == IDM_VAL_NIL) return true;
    return idm_value_write(buf, v);
}

bool idm_string_concat_display(IdmRuntime *rt, const IdmValue *items, size_t count, IdmValue *out, IdmError *err) {
    if (count == 1u && idm_value_tag(items[0]) == IDM_VAL_STRING) { *out = items[0]; return true; }
    bool all_strings = count != 0;
    size_t total = 0;
    for (size_t i = 0; all_strings && i < count; i++) {
        if (idm_value_tag(items[i]) != IDM_VAL_STRING) all_strings = false;
        else total += idm_string_length(items[i]);
    }
    if (all_strings && total > IDM_STRING_LEAF_MAX) {
        IdmValue acc = items[0];
        for (size_t i = 1; i < count; i++) {
            if (!idm_string_concat2(rt, acc, items[i], &acc, err)) return false;
        }
        *out = acc;
        return true;
    }
    IdmBuffer buf;
    idm_buf_init(&buf);
    for (size_t i = 0; i < count; i++) {
        if (!idm_value_append_display(&buf, items[i])) {
            idm_buf_destroy(&buf);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    *out = idm_string_n(rt, buf.data ? buf.data : "", buf.len, err);
    idm_buf_destroy(&buf);
    return !(err && err->present);
}

IdmValue idm_cons(IdmRuntime *rt, IdmValue car, IdmValue cdr, IdmError *err) {
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_PAIR);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.pair.car = car;
    obj->as.pair.cdr = cdr;
    IdmValue v;
    v = idm_from_boxed(obj);
    return v;
}

bool idm_list_append(IdmRuntime *rt, IdmValue head, IdmValue tail, IdmValue *out, IdmError *err) {
    if (idm_is_empty_list(head)) { *out = tail; return true; }
    if (!idm_is_pair(head)) {
        idm_error_set(err, idm_span_unknown(NULL), "append expects a list as first argument");
        return idm_error_reason(rt, err, "type-error", 2, idm_atom(rt, "append"), head);
    }
    size_t count = 0;
    IdmValue cur = head;
    while (idm_is_pair(cur)) {
        count++;
        cur = idm_cdr(cur, err);
        if (err && err->present) return false;
    }
    if (!idm_is_empty_list(cur)) {
        idm_error_set(err, idm_span_unknown(NULL), "append expects a proper list as first argument");
        return idm_error_reason(rt, err, "type-error", 2, idm_atom(rt, "append"), head);
    }
    IdmValue *items = count == 0 ? NULL : malloc(count * sizeof(*items));
    if (count != 0 && !items) return idm_error_oom(err, idm_span_unknown(NULL));
    cur = head;
    for (size_t i = 0; i < count; i++) {
        items[i] = idm_car(cur, err);
        if (err && err->present) { free(items); return false; }
        cur = idm_cdr(cur, err);
        if (err && err->present) { free(items); return false; }
    }
    IdmValue result = tail;
    for (size_t i = count; i > 0; i--) {
        result = idm_cons(rt, items[i - 1u], result, err);
        if (err && err->present) { free(items); return false; }
    }
    free(items);
    *out = result;
    return true;
}

static IdmValue sequence_value(IdmRuntime *rt, const IdmValue *items, size_t count, IdmObjectKind obj_kind, IdmError *err) {
    size_t bytes = 0;
    if (count != 0 && !size_mul(count, sizeof(IdmValue), &bytes)) {
        idm_error_set(err, idm_span_unknown(NULL), "sequence is too large");
        return idm_nil();
    }
    IdmObject *obj = heap_alloc_extra(idm_active_heap(rt), obj_kind, bytes);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.sequence.count = count;
    obj->as.sequence.items = NULL;
    if (count != 0) {
        obj->as.sequence.items = (IdmValue *)(obj + 1);
        memcpy(obj->as.sequence.items, items, count * sizeof(*items));
    }
    return idm_from_boxed(obj);
}

IdmValue idm_tuple(IdmRuntime *rt, const IdmValue *items, size_t count, IdmError *err) {
    return sequence_value(rt, items, count, IDM_OBJ_TUPLE, err);
}

IdmValue idm_vector(IdmRuntime *rt, const IdmValue *items, size_t count, IdmError *err) {
    return sequence_value(rt, items, count, IDM_OBJ_VECTOR, err);
}

static bool dict_key_equal(IdmValue a, IdmValue b);

static bool dict_key_equal(IdmValue a, IdmValue b) {
    return idm_value_equal(a, b);
}

static uint32_t dict_hash_mix(uint32_t h, uint32_t v) {
    h ^= v;
    h *= 0x9e3779b1u;
    h ^= h >> 15;
    return h;
}

static uint32_t dict_hash_finish(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h == 0 ? 1u : h;
}

static uint32_t dict_hash_rotl(uint32_t v, unsigned n) {
    n &= 31u;
    return n == 0 ? v : (uint32_t)((v << n) | (v >> (32u - n)));
}

static bool dict_hash_memo_kind(IdmObjectKind kind) {
    switch (kind) {
        case IDM_OBJ_STRING:
        case IDM_OBJ_ROPE:
        case IDM_OBJ_PAIR:
        case IDM_OBJ_TUPLE:
        case IDM_OBJ_VECTOR:
        case IDM_OBJ_DICT:
        case IDM_OBJ_RECORD:
        case IDM_OBJ_FLONUM:
        case IDM_OBJ_BIGNUM:
        case IDM_OBJ_BITSTRING:
            return true;
        default:
            return false;
    }
}

static uint32_t dict_key_hash_compute(IdmValue v);

static uint32_t dict_key_hash(IdmValue v) {
    IdmObject *obj = value_object(v);
    if (obj && dict_hash_memo_kind(obj->kind)) {
        uint32_t cached = atomic_load_explicit(&obj->dict_hash, memory_order_acquire);
        if (cached != 0) return cached;
        uint32_t h = dict_hash_finish(dict_key_hash_compute(v));
        atomic_store_explicit(&obj->dict_hash, h, memory_order_release);
        return h;
    }
    return dict_hash_finish(dict_key_hash_compute(v));
}

static uint8_t bits_chunk(const unsigned char *bytes, uint64_t off, unsigned n) {
    size_t i = (size_t)(off >> 3);
    unsigned sh = (unsigned)(off & 7u);
    uint8_t out = (uint8_t)(bytes[i] << sh);
    if (n > 8u - sh) out |= (uint8_t)(bytes[i + 1u] >> (8u - sh));
    if (n < 8u) out &= (uint8_t)(0xFFu << (8u - n));
    return out;
}

static void bits_blit(unsigned char *dst, uint64_t doff, const unsigned char *src, uint64_t soff, uint64_t len) {
    uint64_t done = 0;
    if ((doff & 7u) == 0u && (soff & 7u) == 0u && len >= 8u) {
        size_t whole = (size_t)(len >> 3);
        memcpy(dst + (doff >> 3), src + (soff >> 3), whole);
        done = (uint64_t)whole << 3;
    }
    for (; done < len; done += 8u) {
        unsigned n = len - done >= 8u ? 8u : (unsigned)(len - done);
        uint8_t c = bits_chunk(src, soff + done, n);
        uint64_t d = doff + done;
        size_t i = (size_t)(d >> 3);
        unsigned sh = (unsigned)(d & 7u);
        dst[i] |= (uint8_t)(c >> sh);
        if (sh + n > 8u) dst[i + 1u] |= (uint8_t)(c << (8u - sh));
    }
}

static int bits_order(const unsigned char *a, uint64_t ao, uint64_t alen, const unsigned char *b, uint64_t bo, uint64_t blen) {
    uint64_t n = alen < blen ? alen : blen;
    for (uint64_t done = 0; done < n; done += 8u) {
        unsigned c = n - done >= 8u ? 8u : (unsigned)(n - done);
        uint8_t x = bits_chunk(a, ao + done, c);
        uint8_t y = bits_chunk(b, bo + done, c);
        if (x != y) return x < y ? -1 : 1;
    }
    return alen < blen ? -1 : (alen > blen ? 1 : 0);
}

static uint32_t dict_hash_entry(IdmValue key, IdmValue value) {
    uint32_t h = dict_hash_mix(0x8f1bbcdcu, dict_key_hash(key));
    return dict_hash_mix(h, dict_key_hash(value));
}

static void dict_hash_fold_entry(uint32_t *sum, uint32_t *xors, uint32_t *squares, uint32_t entry) {
    *sum += entry;
    *xors ^= dict_hash_rotl(entry, entry >> 27);
    *squares += entry * entry + 0x9e3779b9u;
}

static uint32_t dict_key_hash_compute(IdmValue v) {
    if (idm_value_is_int(v)) {
        int64_t iv = 0;
        if (idm_int_to_i64(v, &iv)) return dict_hash_mix(0x811c9dc5u, (uint32_t)iv ^ (uint32_t)((uint64_t)iv >> 32));
        const IdmBignumObj *bn = &idm_boxed_object(v)->as.bignum;
        uint32_t h = dict_hash_mix(0x811c9dc5u, (uint32_t)bn->sign);
        for (uint32_t i = 0; i < bn->count; i++) h = dict_hash_mix(h, bn->limbs[i]);
        return h;
    }
    IdmValueTag t = idm_value_tag(v);
    switch (t) {
        case IDM_VAL_NIL:
            return 0x27d4eb2fu;
        case IDM_VAL_ATOM:
        case IDM_VAL_WORD:
            return dict_hash_mix(0x165667b1u, idm_value_symbol(v)->hash);
        case IDM_VAL_FLOAT: {
            double d = idm_float_value(v);
            if (d == 0.0) d = 0.0;
            uint64_t bits;
            memcpy(&bits, &d, sizeof(bits));
            return dict_hash_mix(0x85ebca77u, (uint32_t)bits ^ (uint32_t)(bits >> 32));
        }
        case IDM_VAL_STRING: {
            uint32_t h = 0x811c9dc5u;
            IdmStringIter it;
            idm_string_iter_init(v, &it);
            const char *chunk = NULL;
            size_t chunk_len = 0;
            while (idm_string_iter_next(&it, &chunk, &chunk_len)) {
                for (size_t i = 0; i < chunk_len; i++) {
                    h ^= (unsigned char)chunk[i];
                    h *= 16777619u;
                }
            }
            return h;
        }
        case IDM_VAL_SYNTAX:
        case IDM_VAL_CELL:
        case IDM_VAL_CLOSURE:
        case IDM_VAL_REGEX:
        case IDM_VAL_REGEX_RESULT: {
            uintptr_t p = (uintptr_t)idm_boxed_object(v);
            return dict_hash_mix(0xc2b2ae3du, (uint32_t)p ^ (uint32_t)((uint64_t)p >> 32));
        }
        case IDM_VAL_PID:
        case IDM_VAL_REF:
        case IDM_VAL_PORT: {
            uint64_t id = idm_value_id(v);
            return dict_hash_mix(0x94d049bbu, (uint32_t)id ^ (uint32_t)(id >> 32));
        }
        case IDM_VAL_PAIR: {
            uint32_t h = 0x2545f491u;
            IdmValue cur = v;
            while (idm_is_pair(cur)) {
                h = dict_hash_mix(h, dict_key_hash(idm_car(cur, NULL)));
                cur = idm_cdr(cur, NULL);
            }
            return dict_hash_mix(h, dict_key_hash(cur));
        }
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR: {
            const IdmSequenceObj *seq = &idm_boxed_object(v)->as.sequence;
            uint32_t h = t == IDM_VAL_TUPLE ? 0x3c6ef372u : 0xdaa66d2bu;
            for (size_t i = 0; i < seq->count; i++) h = dict_hash_mix(h, dict_key_hash(seq->items[i]));
            return h;
        }
        case IDM_VAL_RECORD: {
            const IdmRecordObj *rec = &idm_boxed_object(v)->as.record;
            uint32_t h = dict_hash_mix(0x6a09e667u, rec->shape ? rec->shape->type->hash : 0u);
            size_t n = rec->shape ? rec->shape->field_count : 0u;
            for (size_t i = 0; i < n; i++) h = dict_hash_mix(h, rec->shape->field_names[i]->hash);
            for (size_t i = 0; i < n; i++) h = dict_hash_mix(h, dict_key_hash(rec->field_values[i]));
            return h;
        }
        case IDM_VAL_DICT: {
            const IdmObject *dict = idm_boxed_object(v);
            uint32_t sum = 0x243f6a88u;
            uint32_t xors = 0x85a308d3u;
            uint32_t squares = 0x13198a2eu;
            IdmDictIter it;
            IdmValue key, value;
            idm_dict_iter_init(v, &it);
            while (idm_dict_iter_next(&it, &key, &value)) {
                dict_hash_fold_entry(&sum, &xors, &squares, dict_hash_entry(key, value));
            }
            uint32_t h = dict_hash_mix(0xbb67ae85u, (uint32_t)dict->as.dict.count);
            h = dict_hash_mix(h, sum);
            h = dict_hash_mix(h, xors);
            h = dict_hash_mix(h, squares);
            return h;
        }
        case IDM_VAL_BITSTRING: {
            const IdmBitstringObj *bs = &idm_boxed_object(v)->as.bits;
            uint32_t h = dict_hash_mix(0x5be0cd19u, (uint32_t)bs->bit_len ^ (uint32_t)(bs->bit_len >> 32));
            for (uint64_t done = 0; done < bs->bit_len; done += 8u) {
                unsigned n = bs->bit_len - done >= 8u ? 8u : (unsigned)(bs->bit_len - done);
                h ^= bits_chunk(bs->bytes, bs->bit_off + done, n);
                h *= 16777619u;
            }
            return h;
        }
        default:
            return 0x510e527fu;
    }
}

static size_t dict_node_slot_count(const IdmObject *node) {
    if (node->as.dict_node.collision_count != 0) return (size_t)node->as.dict_node.collision_count * 2u;
    return (size_t)__builtin_popcount(node->as.dict_node.datamap) * 2u + (size_t)__builtin_popcount(node->as.dict_node.nodemap);
}

static IdmObject *dict_node_alloc(IdmRuntime *rt, uint32_t datamap, uint32_t nodemap, uint16_t collisions, size_t slot_count, IdmError *err) {
    if (slot_count > SIZE_MAX / sizeof(IdmValue)) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    IdmObject *obj = heap_alloc_extra(idm_active_heap(rt), IDM_OBJ_DICT_NODE, slot_count * sizeof(IdmValue));
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    obj->as.dict_node.datamap = datamap;
    obj->as.dict_node.nodemap = nodemap;
    obj->as.dict_node.collision_count = collisions;
    obj->as.dict_node.slots = slot_count == 0 ? NULL : (IdmValue *)(obj + 1);
    for (size_t i = 0; i < slot_count; i++) obj->as.dict_node.slots[i] = idm_nil();
    return obj;
}

static IdmValue dict_empty(IdmRuntime *rt, IdmError *err) {
    IdmObject *obj = heap_alloc_extra(idm_active_heap(rt), IDM_OBJ_DICT, 0);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.dict.count = 0;
    obj->as.dict.root = idm_nil();
    return idm_from_boxed(obj);
}

static bool dict_node_get(const IdmObject *node, IdmValue key, uint32_t hash, unsigned shift, IdmValue *out) {
    if (node->as.dict_node.collision_count != 0) {
        for (uint16_t i = 0; i < node->as.dict_node.collision_count; i++) {
            if (dict_key_equal(node->as.dict_node.slots[2u * i], key)) {
                *out = node->as.dict_node.slots[2u * i + 1u];
                return true;
            }
        }
        return false;
    }
    uint32_t bit = 1u << ((hash >> shift) & 31u);
    if (node->as.dict_node.datamap & bit) {
        size_t di = (size_t)__builtin_popcount(node->as.dict_node.datamap & (bit - 1u));
        if (!dict_key_equal(node->as.dict_node.slots[2u * di], key)) return false;
        *out = node->as.dict_node.slots[2u * di + 1u];
        return true;
    }
    if (node->as.dict_node.nodemap & bit) {
        size_t base = (size_t)__builtin_popcount(node->as.dict_node.datamap) * 2u;
        size_t ni = (size_t)__builtin_popcount(node->as.dict_node.nodemap & (bit - 1u));
        return dict_node_get(idm_boxed_object(node->as.dict_node.slots[base + ni]), key, hash, shift + 5u, out);
    }
    return false;
}

static IdmObject *dict_node_pair(IdmRuntime *rt, IdmValue k1, IdmValue v1, uint32_t h1, IdmValue k2, IdmValue v2, uint32_t h2, unsigned shift, IdmError *err) {
    if (shift >= 30u) {
        IdmObject *node = dict_node_alloc(rt, 0, 0, 2u, 4u, err);
        if (!node) return NULL;
        node->as.dict_node.slots[0] = k1;
        node->as.dict_node.slots[1] = v1;
        node->as.dict_node.slots[2] = k2;
        node->as.dict_node.slots[3] = v2;
        return node;
    }
    uint32_t b1 = 1u << ((h1 >> shift) & 31u);
    uint32_t b2 = 1u << ((h2 >> shift) & 31u);
    if (b1 == b2) {
        IdmObject *child = dict_node_pair(rt, k1, v1, h1, k2, v2, h2, shift + 5u, err);
        if (!child) return NULL;
        IdmObject *node = dict_node_alloc(rt, 0, b1, 0, 1u, err);
        if (!node) return NULL;
        node->as.dict_node.slots[0] = idm_from_boxed(child);
        return node;
    }
    IdmObject *node = dict_node_alloc(rt, b1 | b2, 0, 0, 4u, err);
    if (!node) return NULL;
    if (b1 < b2) {
        node->as.dict_node.slots[0] = k1;
        node->as.dict_node.slots[1] = v1;
        node->as.dict_node.slots[2] = k2;
        node->as.dict_node.slots[3] = v2;
    } else {
        node->as.dict_node.slots[0] = k2;
        node->as.dict_node.slots[1] = v2;
        node->as.dict_node.slots[2] = k1;
        node->as.dict_node.slots[3] = v1;
    }
    return node;
}

static IdmObject *dict_node_put(IdmRuntime *rt, const IdmObject *node, IdmValue key, IdmValue value, uint32_t hash, unsigned shift, bool *added, IdmError *err) {
    size_t slot_count = dict_node_slot_count(node);
    if (node->as.dict_node.collision_count != 0) {
        for (uint16_t i = 0; i < node->as.dict_node.collision_count; i++) {
            if (dict_key_equal(node->as.dict_node.slots[2u * i], key)) {
                IdmObject *copy = dict_node_alloc(rt, 0, 0, node->as.dict_node.collision_count, slot_count, err);
                if (!copy) return NULL;
                memcpy(copy->as.dict_node.slots, node->as.dict_node.slots, slot_count * sizeof(IdmValue));
                copy->as.dict_node.slots[2u * i + 1u] = value;
                *added = false;
                return copy;
            }
        }
        if (node->as.dict_node.collision_count >= UINT16_MAX) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return NULL;
        }
        IdmObject *copy = dict_node_alloc(rt, 0, 0, (uint16_t)(node->as.dict_node.collision_count + 1u), slot_count + 2u, err);
        if (!copy) return NULL;
        memcpy(copy->as.dict_node.slots, node->as.dict_node.slots, slot_count * sizeof(IdmValue));
        copy->as.dict_node.slots[slot_count] = key;
        copy->as.dict_node.slots[slot_count + 1u] = value;
        *added = true;
        return copy;
    }
    uint32_t bit = 1u << ((hash >> shift) & 31u);
    size_t data_slots = (size_t)__builtin_popcount(node->as.dict_node.datamap) * 2u;
    if (node->as.dict_node.datamap & bit) {
        size_t di = (size_t)__builtin_popcount(node->as.dict_node.datamap & (bit - 1u));
        if (dict_key_equal(node->as.dict_node.slots[2u * di], key)) {
            IdmObject *copy = dict_node_alloc(rt, node->as.dict_node.datamap, node->as.dict_node.nodemap, 0, slot_count, err);
            if (!copy) return NULL;
            memcpy(copy->as.dict_node.slots, node->as.dict_node.slots, slot_count * sizeof(IdmValue));
            copy->as.dict_node.slots[2u * di + 1u] = value;
            *added = false;
            return copy;
        }
        IdmValue ek = node->as.dict_node.slots[2u * di];
        IdmValue ev = node->as.dict_node.slots[2u * di + 1u];
        IdmObject *child = dict_node_pair(rt, ek, ev, dict_key_hash(ek), key, value, hash, shift + 5u, err);
        if (!child) return NULL;
        size_t ni = (size_t)__builtin_popcount(node->as.dict_node.nodemap & (bit - 1u));
        IdmObject *copy = dict_node_alloc(rt, node->as.dict_node.datamap & ~bit, node->as.dict_node.nodemap | bit, 0, slot_count - 1u, err);
        if (!copy) return NULL;
        size_t w = 0;
        for (size_t i = 0; i < data_slots; i += 2u) {
            if (i == 2u * di) continue;
            copy->as.dict_node.slots[w++] = node->as.dict_node.slots[i];
            copy->as.dict_node.slots[w++] = node->as.dict_node.slots[i + 1u];
        }
        size_t old_nodes = (size_t)__builtin_popcount(node->as.dict_node.nodemap);
        for (size_t i = 0; i < old_nodes; i++) {
            if (i == ni) copy->as.dict_node.slots[w++] = idm_from_boxed(child);
            copy->as.dict_node.slots[w++] = node->as.dict_node.slots[data_slots + i];
        }
        if (ni == old_nodes) copy->as.dict_node.slots[w++] = idm_from_boxed(child);
        *added = true;
        return copy;
    }
    if (node->as.dict_node.nodemap & bit) {
        size_t ni = (size_t)__builtin_popcount(node->as.dict_node.nodemap & (bit - 1u));
        IdmObject *child = dict_node_put(rt, idm_boxed_object(node->as.dict_node.slots[data_slots + ni]), key, value, hash, shift + 5u, added, err);
        if (!child) return NULL;
        IdmObject *copy = dict_node_alloc(rt, node->as.dict_node.datamap, node->as.dict_node.nodemap, 0, slot_count, err);
        if (!copy) return NULL;
        memcpy(copy->as.dict_node.slots, node->as.dict_node.slots, slot_count * sizeof(IdmValue));
        copy->as.dict_node.slots[data_slots + ni] = idm_from_boxed(child);
        return copy;
    }
    size_t di = (size_t)__builtin_popcount(node->as.dict_node.datamap & (bit - 1u));
    IdmObject *copy = dict_node_alloc(rt, node->as.dict_node.datamap | bit, node->as.dict_node.nodemap, 0, slot_count + 2u, err);
    if (!copy) return NULL;
    memcpy(copy->as.dict_node.slots, node->as.dict_node.slots, 2u * di * sizeof(IdmValue));
    copy->as.dict_node.slots[2u * di] = key;
    copy->as.dict_node.slots[2u * di + 1u] = value;
    memcpy(copy->as.dict_node.slots + 2u * di + 2u, node->as.dict_node.slots + 2u * di, (slot_count - 2u * di) * sizeof(IdmValue));
    *added = true;
    return copy;
}

static IdmObject *dict_node_del(IdmRuntime *rt, const IdmObject *node, IdmValue key, uint32_t hash, unsigned shift, bool *removed, bool *emptied, IdmError *err) {
    size_t slot_count = dict_node_slot_count(node);
    *emptied = false;
    if (node->as.dict_node.collision_count != 0) {
        for (uint16_t i = 0; i < node->as.dict_node.collision_count; i++) {
            if (dict_key_equal(node->as.dict_node.slots[2u * i], key)) {
                *removed = true;
                if (node->as.dict_node.collision_count == 1u) {
                    *emptied = true;
                    return (IdmObject *)node;
                }
                IdmObject *copy = dict_node_alloc(rt, 0, 0, (uint16_t)(node->as.dict_node.collision_count - 1u), slot_count - 2u, err);
                if (!copy) return NULL;
                size_t w = 0;
                for (uint16_t j = 0; j < node->as.dict_node.collision_count; j++) {
                    if (j == i) continue;
                    copy->as.dict_node.slots[w++] = node->as.dict_node.slots[2u * j];
                    copy->as.dict_node.slots[w++] = node->as.dict_node.slots[2u * j + 1u];
                }
                return copy;
            }
        }
        *removed = false;
        return (IdmObject *)node;
    }
    uint32_t bit = 1u << ((hash >> shift) & 31u);
    size_t data_slots = (size_t)__builtin_popcount(node->as.dict_node.datamap) * 2u;
    if (node->as.dict_node.datamap & bit) {
        size_t di = (size_t)__builtin_popcount(node->as.dict_node.datamap & (bit - 1u));
        if (!dict_key_equal(node->as.dict_node.slots[2u * di], key)) {
            *removed = false;
            return (IdmObject *)node;
        }
        *removed = true;
        if (slot_count == 2u) {
            *emptied = true;
            return (IdmObject *)node;
        }
        IdmObject *copy = dict_node_alloc(rt, node->as.dict_node.datamap & ~bit, node->as.dict_node.nodemap, 0, slot_count - 2u, err);
        if (!copy) return NULL;
        memcpy(copy->as.dict_node.slots, node->as.dict_node.slots, 2u * di * sizeof(IdmValue));
        memcpy(copy->as.dict_node.slots + 2u * di, node->as.dict_node.slots + 2u * di + 2u, (slot_count - 2u * di - 2u) * sizeof(IdmValue));
        return copy;
    }
    if (node->as.dict_node.nodemap & bit) {
        size_t ni = (size_t)__builtin_popcount(node->as.dict_node.nodemap & (bit - 1u));
        bool child_emptied = false;
        IdmObject *child = dict_node_del(rt, idm_boxed_object(node->as.dict_node.slots[data_slots + ni]), key, hash, shift + 5u, removed, &child_emptied, err);
        if (!child) return NULL;
        if (!*removed) return (IdmObject *)node;
        if (child_emptied) {
            if (slot_count == 1u) {
                *emptied = true;
                return (IdmObject *)node;
            }
            IdmObject *copy = dict_node_alloc(rt, node->as.dict_node.datamap, node->as.dict_node.nodemap & ~bit, 0, slot_count - 1u, err);
            if (!copy) return NULL;
            memcpy(copy->as.dict_node.slots, node->as.dict_node.slots, (data_slots + ni) * sizeof(IdmValue));
            memcpy(copy->as.dict_node.slots + data_slots + ni, node->as.dict_node.slots + data_slots + ni + 1u, (slot_count - data_slots - ni - 1u) * sizeof(IdmValue));
            return copy;
        }
        IdmObject *copy = dict_node_alloc(rt, node->as.dict_node.datamap, node->as.dict_node.nodemap, 0, slot_count, err);
        if (!copy) return NULL;
        memcpy(copy->as.dict_node.slots, node->as.dict_node.slots, slot_count * sizeof(IdmValue));
        copy->as.dict_node.slots[data_slots + ni] = idm_from_boxed(child);
        return copy;
    }
    *removed = false;
    return (IdmObject *)node;
}

IdmValue idm_syntax_value(IdmRuntime *rt, const IdmSyntax *syntax, IdmError *err) {
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_SYNTAX);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.syntax = idm_syn_clone(syntax);
    if (!obj->as.syntax) {
        idm_error_oom(err, syntax ? syntax->span : idm_span_unknown(NULL));
        return idm_nil();
    }
    heap_account(idm_active_heap(rt), obj, syn_footprint(obj->as.syntax));
    IdmValue v;
    v = idm_from_boxed(obj);
    return v;
}

static bool syntax_copy_scopes_from(IdmSyntax *dst, const IdmSyntax *src) {
    if (!src) return true;
    for (size_t i = 0; i < src->scopes.count; i++) {
        const IdmSyntaxPhaseScope *p = &src->scopes.items[i];
        for (size_t j = 0; j < p->scopes.count; j++) {
            if (!idm_syn_scope_add(dst, p->phase, p->scopes.items[j])) return false;
        }
    }
    return true;
}

static bool syntax_add_active_intro_scope(IdmRuntime *rt, IdmSyntax *syn) {
    if (!rt->macro_intro_active) return true;
    const IdmScopeSet *set = idm_syn_scope_set(syn, 0);
    if (!set || !idm_scope_set_contains(set, rt->macro_intro_scope)) return true;
    return idm_syn_scope_flip(syn, 0, rt->macro_intro_scope);
}

static bool syntax_collect_list(IdmValue list, IdmSyntax ***out_items, size_t *out_count, IdmError *err) {
    size_t count = 0;
    IdmValue cur = list;
    while (idm_is_pair(cur)) {
        count++;
        cur = idm_cdr(cur, err);
        if (err && err->present) return false;
    }
    if (!idm_is_empty_list(cur)) return idm_error_set(err, idm_span_unknown(NULL), "expected proper list of syntax");
    IdmSyntax **items = count == 0 ? NULL : calloc(count, sizeof(*items));
    if (count != 0 && !items) return idm_error_oom(err, idm_span_unknown(NULL));
    cur = list;
    for (size_t i = 0; i < count; i++) {
        IdmValue car = idm_car(cur, err);
        if (err && err->present) { free(items); return false; }
        IdmSyntax *inner = idm_syntax_value_get(car);
        if (!inner) {
            free(items);
            return idm_error_set(err, idm_span_unknown(NULL), "list item must be a syntax value");
        }
        items[i] = idm_syn_clone(inner);
        if (!items[i]) {
            for (size_t j = 0; j < i; j++) idm_syn_free(items[j]);
            free(items);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        cur = idm_cdr(cur, err);
        if (err && err->present) {
            for (size_t j = 0; j <= i; j++) idm_syn_free(items[j]);
            free(items);
            return false;
        }
    }
    *out_items = items;
    *out_count = count;
    return true;
}

static void syntax_items_free(IdmSyntax **items, size_t count) {
    for (size_t i = 0; i < count; i++) idm_syn_free(items[i]);
    free(items);
}

static void syntax_items_free_nodes(IdmSyntax **items, size_t count) {
    for (size_t i = 0; i < count; i++) idm_syn_free(items[i]);
}

static IdmSyntax *syntax_sequence_node(IdmSyntaxBuildKind kind, IdmSpan span) {
    IdmSyntaxKind seq_kind = IDM_SYN_NIL;
    switch (kind) {
        case IDM_SYNTAX_BUILD_LIST: seq_kind = IDM_SYN_LIST; break;
        case IDM_SYNTAX_BUILD_VECTOR: seq_kind = IDM_SYN_VECTOR; break;
        case IDM_SYNTAX_BUILD_TUPLE: seq_kind = IDM_SYN_TUPLE; break;
        case IDM_SYNTAX_BUILD_DICT: seq_kind = IDM_SYN_DICT; break;
        default: return NULL;
    }
    return idm_syn_seq(seq_kind, span);
}

static const char *syntax_form_head(IdmSyntaxBuildKind kind) {
    switch (kind) {
        case IDM_SYNTAX_BUILD_EXPR: return "%-expr";
        case IDM_SYNTAX_BUILD_BODY: return "%-body";
        case IDM_SYNTAX_BUILD_GROUP: return "%-group";
        default: return NULL;
    }
}

static IdmSyntax *syntax_form_sequence(IdmRuntime *rt, IdmSyntax *ctx, IdmSyntaxBuildKind kind, IdmSyntax **items, size_t count, IdmError *err) {
    const char *head = syntax_form_head(kind);
    if (!head) return NULL;
    IdmSyntax *syn = idm_syn_list(ctx->span);
    if (!syn || !syntax_copy_scopes_from(syn, ctx)) {
        idm_syn_free(syn);
        syntax_items_free_nodes(items, count);
        return NULL;
    }
    IdmSyntax *head_syn = idm_syn_word(head, ctx->span);
    if (!head_syn || !syntax_copy_scopes_from(head_syn, ctx) || !idm_syn_append(syn, head_syn)) {
        idm_syn_free(head_syn);
        idm_syn_free(syn);
        syntax_items_free_nodes(items, count);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (!idm_syn_append(syn, items[i])) {
            for (size_t j = i; j < count; j++) idm_syn_free(items[j]);
            idm_syn_free(syn);
            return NULL;
        }
    }
    if (!syntax_add_active_intro_scope(rt, syn)) {
        idm_syn_free(syn);
        idm_error_oom(err, ctx->span);
        return NULL;
    }
    return syn;
}

static bool syntax_wrap_owned(IdmRuntime *rt, IdmSyntax *syn, IdmValue *out, IdmError *err) {
    if (!syn) return false;
    *out = idm_syntax_value(rt, syn, err);
    idm_syn_free(syn);
    return !(err && err->present);
}

bool idm_syntax_build(IdmRuntime *rt, IdmSyntaxBuildKind kind, IdmValue ctx_value, IdmValue payload, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = idm_syntax_value_get(ctx_value);
    if (!ctx) return idm_error_set(err, idm_span_unknown(NULL), "expected a syntax value");
    IdmSyntax *syn = NULL;
    switch (kind) {
        case IDM_SYNTAX_BUILD_NIL:
            syn = idm_syn_nil(ctx->span);
            break;
        case IDM_SYNTAX_BUILD_WORD:
            if (idm_value_tag(payload) != IDM_VAL_STRING) return idm_error_set(err, ctx->span, "make-syntax-word expects string");
            syn = idm_syn_word(idm_string_bytes(payload), ctx->span);
            break;
        case IDM_SYNTAX_BUILD_ATOM:
            if (idm_value_tag(payload) != IDM_VAL_STRING) return idm_error_set(err, ctx->span, "make-syntax-atom expects string");
            syn = idm_syn_atom(idm_string_bytes(payload), ctx->span);
            break;
        case IDM_SYNTAX_BUILD_INT:
            if (idm_value_tag(payload) == IDM_VAL_BIGNUM) {
                IdmBuffer tmp;
                idm_buf_init(&tmp);
                if (!idm_value_write(&tmp, payload) || !idm_buf_append_char(&tmp, '\0')) { idm_buf_destroy(&tmp); return idm_error_oom(err, ctx->span); }
                syn = idm_syn_bigint(tmp.data, ctx->span);
                idm_buf_destroy(&tmp);
            } else if (idm_value_tag(payload) == IDM_VAL_INT) {
                syn = idm_syn_int(idm_int_value(payload), ctx->span);
            } else {
                return idm_error_set(err, ctx->span, "make-syntax-int expects int");
            }
            break;
        case IDM_SYNTAX_BUILD_FLOAT:
            if (idm_value_tag(payload) != IDM_VAL_FLOAT && idm_value_tag(payload) != IDM_VAL_INT) return idm_error_set(err, ctx->span, "make-syntax-float expects number");
            syn = idm_syn_float(idm_value_tag(payload) == IDM_VAL_FLOAT ? idm_float_value(payload) : (double)idm_int_value(payload), ctx->span);
            break;
        case IDM_SYNTAX_BUILD_STRING:
            if (idm_value_tag(payload) != IDM_VAL_STRING) return idm_error_set(err, ctx->span, "make-syntax-string expects string");
            syn = idm_syn_string_n(idm_string_bytes(payload), idm_string_length(payload), ctx->span);
            break;
        case IDM_SYNTAX_BUILD_LIST:
        case IDM_SYNTAX_BUILD_VECTOR:
        case IDM_SYNTAX_BUILD_TUPLE:
        case IDM_SYNTAX_BUILD_DICT: {
            IdmSyntax **items = NULL;
            size_t count = 0;
            if (!syntax_collect_list(payload, &items, &count, err)) return false;
            syn = syntax_sequence_node(kind, ctx->span);
            if (!syn || !syntax_copy_scopes_from(syn, ctx)) {
                idm_syn_free(syn);
                syntax_items_free(items, count);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            if (!syntax_add_active_intro_scope(rt, syn)) {
                idm_syn_free(syn);
                syntax_items_free(items, count);
                return idm_error_oom(err, ctx->span);
            }
            for (size_t i = 0; i < count; i++) {
                if (!idm_syn_append(syn, items[i])) {
                    for (size_t j = i; j < count; j++) idm_syn_free(items[j]);
                    free(items);
                    idm_syn_free(syn);
                    return idm_error_oom(err, idm_span_unknown(NULL));
                }
            }
            free(items);
            return syntax_wrap_owned(rt, syn, out, err);
        }
        case IDM_SYNTAX_BUILD_EXPR:
        case IDM_SYNTAX_BUILD_BODY: {
            IdmSyntax **items = NULL;
            size_t count = 0;
            if (!syntax_collect_list(payload, &items, &count, err)) return false;
            syn = syntax_form_sequence(rt, ctx, kind, items, count, err);
            free(items);
            if (!syn) return idm_error_oom(err, ctx->span);
            return syntax_wrap_owned(rt, syn, out, err);
        }
        case IDM_SYNTAX_BUILD_GROUP: {
            IdmSyntax *inner = idm_syntax_value_get(payload);
            if (!inner) return idm_error_set(err, ctx->span, "make-syntax-group expects syntax");
            IdmSyntax *item = idm_syn_clone(inner);
            if (!item) return idm_error_oom(err, ctx->span);
            IdmSyntax *items[1] = { item };
            syn = syntax_form_sequence(rt, ctx, kind, items, 1u, err);
            if (!syn) return idm_error_oom(err, ctx->span);
            return syntax_wrap_owned(rt, syn, out, err);
        }
    }
    if (!syn) return idm_error_oom(err, ctx->span);
    if (!syntax_copy_scopes_from(syn, ctx)) {
        idm_syn_free(syn);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (!syntax_add_active_intro_scope(rt, syn)) {
        idm_syn_free(syn);
        return idm_error_oom(err, ctx->span);
    }
    return syntax_wrap_owned(rt, syn, out, err);
}

IdmValue idm_cell(IdmRuntime *rt, IdmValue initial, IdmError *err) {
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_CELL);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.cell.value = initial;
    IdmValue v;
    v = idm_from_boxed(obj);
    return v;
}

static bool closure_payload_size(size_t entry_count, size_t capture_count, size_t *out) {
    size_t capture_bytes = 0;
    size_t entry_bytes = 0;
    if (capture_count != 0 && !size_mul(capture_count, sizeof(IdmValue), &capture_bytes)) return false;
    if (entry_count > 1u && !size_mul(entry_count, sizeof(uint32_t), &entry_bytes)) return false;
    return size_add(capture_bytes, entry_bytes, out);
}

IdmValue idm_closure_multi_selectable_in_module(IdmRuntime *rt, const IdmBytecodeModule *module, const uint32_t *function_indexes, size_t function_count, IdmPatternSelector *selector, const IdmValue *captures, size_t capture_count, IdmEnv *env, IdmError *err) {
    if (function_count == 0) {
        idm_error_set(err, idm_span_unknown(NULL), "closure must have at least one function entry");
        return idm_nil();
    }
    if (!function_indexes) {
        idm_error_set(err, idm_span_unknown(NULL), "closure entries are missing");
        return idm_nil();
    }
    if (capture_count != 0 && !captures) {
        idm_error_set(err, idm_span_unknown(NULL), "closure captures are missing");
        return idm_nil();
    }
    if (!selector) {
        idm_error_set(err, idm_span_unknown(NULL), "closure requires a prepared selector");
        return idm_nil();
    }
    if (!idm_bc_is_finalized(module)) {
        idm_error_set(err, idm_span_unknown(NULL), "closure module is not finalized");
        return idm_nil();
    }
    if (!env) {
        idm_error_set(err, idm_span_unknown(NULL), "closure requires an explicit runtime environment");
        return idm_nil();
    }
    size_t extra = 0;
    if (!closure_payload_size(function_count, capture_count, &extra)) {
        idm_error_set(err, idm_span_unknown(NULL), "closure payload is too large");
        return idm_nil();
    }
    IdmObject *obj = heap_alloc_extra(idm_active_heap(rt), IDM_OBJ_CLOSURE, extra);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    unsigned char *payload = (unsigned char *)(obj + 1);
    obj->as.closure.module = module;
    obj->as.closure.env = env;
    obj->as.closure.function_index = function_indexes[0];
    obj->as.closure.entry_count = function_count;
    obj->as.closure.entries = NULL;
    obj->as.closure.capture_count = capture_count;
    obj->as.closure.captures = NULL;
    if (capture_count != 0) {
        obj->as.closure.captures = (IdmValue *)payload;
        memcpy(obj->as.closure.captures, captures, capture_count * sizeof(*captures));
        payload += capture_count * sizeof(*captures);
    }
    if (function_count > 1) {
        obj->as.closure.entries = (uint32_t *)payload;
        memcpy(obj->as.closure.entries, function_indexes, function_count * sizeof(*function_indexes));
    }
    obj->as.closure.selector = selector;
    idm_pattern_selector_retain(selector);
    obj->as.closure.selector_generation = module->selector_generation;
    IdmValue v;
    v = idm_from_boxed(obj);
    return v;
}

IdmValue idm_closure_in_module(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, const IdmValue *captures, size_t capture_count, IdmEnv *env, IdmError *err) {
    if (!idm_bc_is_finalized(module)) {
        idm_error_set(err, idm_span_unknown(NULL), "closure module is not finalized");
        return idm_nil();
    }
    if (function_index >= module->function_count) {
        idm_error_set(err, idm_span_unknown(NULL), "closure references invalid function %u", function_index);
        return idm_nil();
    }
    IdmPatternSelector *selector = idm_bc_function_selector_at(module, function_index);
    IDM_ASSERT_PROVED(selector);
    return idm_closure_multi_selectable_in_module(rt, module, &function_index, 1, selector, captures, capture_count, env, err);
}

IdmValue idm_record_from_shape(IdmRuntime *rt, IdmRecordShape *shape, const IdmValue *field_values, IdmError *err) {
    if (!shape) {
        idm_error_set(err, idm_span_unknown(NULL), "record shape is required");
        return idm_nil();
    }
    if (shape->field_count != 0 && !field_values) {
        idm_error_set(err, idm_span_unknown(NULL), "record fields require values");
        return idm_nil();
    }
    size_t bytes = 0;
    if (shape->field_count != 0 && !size_mul(shape->field_count, sizeof(IdmValue), &bytes)) {
        idm_error_set(err, idm_span_unknown(NULL), "record is too large");
        return idm_nil();
    }
    IdmObject *obj = heap_alloc_extra(idm_active_heap(rt), IDM_OBJ_RECORD, bytes);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.record.shape = shape;
    if (shape->field_count != 0) {
        obj->as.record.field_values = (IdmValue *)(obj + 1);
        for (size_t i = 0; i < shape->field_count; i++) {
            obj->as.record.field_values[i] = field_values[i];
        }
    }
    IdmValue v;
    v = idm_from_boxed(obj);
    return v;
}

IdmValue idm_regex_value(IdmRuntime *rt, IdmRegex *regex, IdmError *err) {
    if (!regex) {
        idm_error_set(err, idm_span_unknown(NULL), "cannot wrap null regex");
        return idm_nil();
    }
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_REGEX);
    if (!obj) {
        idm_regex_free(regex);
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.regex = regex;
    heap_account(idm_active_heap(rt), obj, idm_regex_footprint(regex));
    IdmValue v;
    v = idm_from_boxed(obj);
    return v;
}

IdmValue idm_regex_result_value(IdmRuntime *rt, IdmRegexResult *result, IdmError *err) {
    if (!result) {
        idm_error_set(err, idm_span_unknown(NULL), "cannot wrap null regex result");
        return idm_nil();
    }
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_REGEX_RESULT);
    if (!obj) {
        idm_regex_result_free(result);
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.regex_result = result;
    heap_account(idm_active_heap(rt), obj, idm_regex_result_footprint(result));
    IdmValue v;
    v = idm_from_boxed(obj);
    return v;
}

IdmValue idm_pid(uint64_t id) {
    return idm_immediate(IDM_IMM_PID, id);
}

IdmValue idm_ref(uint64_t id) {
    return idm_immediate(IDM_IMM_REF, id);
}

IdmValue idm_port(uint64_t id) {
    return idm_immediate(IDM_IMM_PORT, id);
}

static bool value_tag_datum_kind(IdmValueTag tag, IdmDatumKind *out) {
    switch (tag) {
        case IDM_VAL_NIL: *out = IDM_DATUM_NIL; return true;
        case IDM_VAL_ATOM: *out = IDM_DATUM_ATOM; return true;
        case IDM_VAL_WORD: *out = IDM_DATUM_WORD; return true;
        case IDM_VAL_INT: *out = IDM_DATUM_INT; return true;
        case IDM_VAL_FLOAT: *out = IDM_DATUM_FLOAT; return true;
        case IDM_VAL_STRING: *out = IDM_DATUM_STRING; return true;
        case IDM_VAL_TUPLE: *out = IDM_DATUM_TUPLE; return true;
        case IDM_VAL_VECTOR: *out = IDM_DATUM_VECTOR; return true;
        case IDM_VAL_DICT: *out = IDM_DATUM_DICT; return true;
        case IDM_VAL_BIGNUM: *out = IDM_DATUM_INT; return true;
        default: return false;
    }
}

const char *idm_value_type_name(IdmValueTag tag) {
    IdmDatumKind datum_kind;
    if (value_tag_datum_kind(tag, &datum_kind)) return idm_datum_kind_name(datum_kind);
    switch (tag) {
        case IDM_VAL_PAIR: return "pair";
        case IDM_VAL_SYNTAX: return "syntax";
        case IDM_VAL_CELL: return "cell";
        case IDM_VAL_CLOSURE: return "closure";
        case IDM_VAL_PID: return "pid";
        case IDM_VAL_REF: return "ref";
        case IDM_VAL_PORT: return "port";
        case IDM_VAL_RECORD: return "record";
        case IDM_VAL_REGEX: return "regex";
        case IDM_VAL_REGEX_RESULT: return "regex-result";
        case IDM_VAL_BITSTRING: return "bitstring";
        default: break;
    }
    return "<bad-type>";
}

const char *idm_value_sequence_kind_name(IdmValueSequenceKind kind) {
    static const IdmDatumKind datum_kinds[] = {
        [IDM_VALUE_SEQ_VECTOR] = IDM_DATUM_VECTOR,
        [IDM_VALUE_SEQ_TUPLE] = IDM_DATUM_TUPLE,
        [IDM_VALUE_SEQ_DICT] = IDM_DATUM_DICT,
    };
    return (size_t)kind < sizeof(datum_kinds) / sizeof(datum_kinds[0]) ? idm_datum_kind_name(datum_kinds[kind]) : "unknown";
}

const char *idm_syntax_build_kind_name(IdmSyntaxBuildKind kind) {
    static const IdmDatumKind datum_kinds[] = {
        [IDM_SYNTAX_BUILD_NIL] = IDM_DATUM_NIL,
        [IDM_SYNTAX_BUILD_WORD] = IDM_DATUM_WORD,
        [IDM_SYNTAX_BUILD_ATOM] = IDM_DATUM_ATOM,
        [IDM_SYNTAX_BUILD_INT] = IDM_DATUM_INT,
        [IDM_SYNTAX_BUILD_FLOAT] = IDM_DATUM_FLOAT,
        [IDM_SYNTAX_BUILD_STRING] = IDM_DATUM_STRING,
        [IDM_SYNTAX_BUILD_LIST] = IDM_DATUM_LIST,
        [IDM_SYNTAX_BUILD_VECTOR] = IDM_DATUM_VECTOR,
        [IDM_SYNTAX_BUILD_TUPLE] = IDM_DATUM_TUPLE,
        [IDM_SYNTAX_BUILD_DICT] = IDM_DATUM_DICT,
    };
    if ((size_t)kind < sizeof(datum_kinds) / sizeof(datum_kinds[0])) return idm_datum_kind_name(datum_kinds[kind]);
    switch (kind) {
        case IDM_SYNTAX_BUILD_EXPR: return "expr";
        case IDM_SYNTAX_BUILD_BODY: return "body";
        case IDM_SYNTAX_BUILD_GROUP: return "group";
        default: return "unknown";
    }
}

const char *idm_value_dispatch_type_name(IdmValue value) {
    if (idm_value_tag(value) == IDM_VAL_RECORD) return record_shape_type_text(idm_boxed_object(value)->as.record.shape);
    return idm_value_type_name(idm_value_tag(value));
}

IdmBuiltinType idm_value_builtin_type_kind(IdmSymbol *type) {
    return type ? type->builtin_type : IDM_BUILTIN_TYPE_NONE;
}

bool idm_value_builtin_type_symbol(IdmSymbol *type) {
    return idm_value_builtin_type_kind(type) != IDM_BUILTIN_TYPE_NONE;
}

bool idm_builtin_type_includes(IdmBuiltinType outer, IdmBuiltinType member) {
    if (outer == member) return outer != IDM_BUILTIN_TYPE_NONE;
    static const IdmBuiltinType parents[IDM_BUILTIN_TYPE_COUNT] = {
#define IDM_BUILTIN_TYPE_PARENT(id, text, parent, listed) [IDM_BUILTIN_TYPE_##id] = IDM_BUILTIN_TYPE_##parent,
        IDM_BUILTIN_TYPE_ROWS(IDM_BUILTIN_TYPE_PARENT)
#undef IDM_BUILTIN_TYPE_PARENT
    };
    return member > IDM_BUILTIN_TYPE_NONE && member < IDM_BUILTIN_TYPE_COUNT && parents[member] == outer;
}

bool idm_value_matches_builtin_type(IdmValue value, IdmBuiltinType type) {
    IdmValueTag vt = idm_value_tag(value);
    switch (type) {
        case IDM_BUILTIN_TYPE_NIL: return vt == IDM_VAL_NIL;
        case IDM_BUILTIN_TYPE_ATOM: return vt == IDM_VAL_ATOM;
        case IDM_BUILTIN_TYPE_WORD: return vt == IDM_VAL_WORD;
        case IDM_BUILTIN_TYPE_INT: return vt == IDM_VAL_INT || vt == IDM_VAL_BIGNUM;
        case IDM_BUILTIN_TYPE_FIXNUM: return vt == IDM_VAL_INT;
        case IDM_BUILTIN_TYPE_BIGNUM: return vt == IDM_VAL_BIGNUM;
        case IDM_BUILTIN_TYPE_FLOAT: return vt == IDM_VAL_FLOAT;
        case IDM_BUILTIN_TYPE_STRING: return vt == IDM_VAL_STRING;
        case IDM_BUILTIN_TYPE_PAIR: return vt == IDM_VAL_PAIR;
        case IDM_BUILTIN_TYPE_EMPTY_LIST: return idm_is_empty_list(value);
        case IDM_BUILTIN_TYPE_LIST: {
            IdmValue cur = value;
            while (idm_is_pair(cur)) cur = idm_cdr(cur, NULL);
            return idm_is_empty_list(cur);
        }
        case IDM_BUILTIN_TYPE_TUPLE: return vt == IDM_VAL_TUPLE;
        case IDM_BUILTIN_TYPE_VECTOR: return vt == IDM_VAL_VECTOR;
        case IDM_BUILTIN_TYPE_DICT: return vt == IDM_VAL_DICT;
        case IDM_BUILTIN_TYPE_SYNTAX: return vt == IDM_VAL_SYNTAX;
        case IDM_BUILTIN_TYPE_CELL: return vt == IDM_VAL_CELL;
        case IDM_BUILTIN_TYPE_CLOSURE: return vt == IDM_VAL_CLOSURE;
        case IDM_BUILTIN_TYPE_PID: return vt == IDM_VAL_PID;
        case IDM_BUILTIN_TYPE_REF: return vt == IDM_VAL_REF;
        case IDM_BUILTIN_TYPE_PORT: return vt == IDM_VAL_PORT;
        case IDM_BUILTIN_TYPE_RECORD: return vt == IDM_VAL_RECORD;
        case IDM_BUILTIN_TYPE_REGEX: return vt == IDM_VAL_REGEX;
        case IDM_BUILTIN_TYPE_REGEX_RESULT: return vt == IDM_VAL_REGEX_RESULT;
        case IDM_BUILTIN_TYPE_BITSTRING: return vt == IDM_VAL_BITSTRING;
        case IDM_BUILTIN_TYPE_PROC: return vt == IDM_VAL_PID || vt == IDM_VAL_PORT;
        case IDM_BUILTIN_TYPE_COUNT:
        case IDM_BUILTIN_TYPE_NONE: break;
    }
    return false;
}

static bool idm_value_matches_type_name(IdmValue value, IdmSymbol *symbol, const char *name, bool allow_any) {
    if (!name && symbol) name = idm_symbol_text(symbol);
    if (!name) return false;
    if (allow_any && strcmp(name, "Any") == 0) return true;
    IdmBuiltinType builtin = symbol ? symbol->builtin_type : builtin_type_from_text(name);
    if (builtin != IDM_BUILTIN_TYPE_NONE) return idm_value_matches_builtin_type(value, builtin);
    if (idm_value_tag(value) != IDM_VAL_RECORD) return false;
    return symbol && idm_boxed_object(value)->as.record.shape->type == symbol;
}

bool idm_value_matches_type_symbol(IdmValue value, IdmSymbol *type) {
    return idm_value_matches_type_name(value, type, NULL, false);
}

bool idm_is_nil(IdmValue value) {
    return idm_is_immediate(value) && idm_immediate_ctor(value) == IDM_IMM_EMPTY;
}

bool idm_is_empty_list(IdmValue value) {
    return idm_is_nil(value);
}

bool idm_is_pair(IdmValue value) {
    return idm_is_boxed(value) && idm_boxed_object(value)->kind == IDM_OBJ_PAIR;
}

bool idm_is_tuple(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_TUPLE;
}

bool idm_is_vector(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_VECTOR;
}

bool idm_is_dict(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_DICT;
}

bool idm_is_syntax(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_SYNTAX;
}

bool idm_is_string(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_STRING;
}

bool idm_is_cell(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_CELL;
}

bool idm_is_record(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_RECORD;
}

bool idm_is_regex(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_REGEX;
}

bool idm_is_regex_result(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_REGEX_RESULT;
}

IdmValue idm_cell_get(IdmValue cell, IdmError *err) {
    if (idm_value_tag(cell) != IDM_VAL_CELL) {
        idm_error_set(err, idm_span_unknown(NULL), "expected cell");
        return idm_nil();
    }
    IdmValue out;
    out.bits = __atomic_load_n(&idm_boxed_object(cell)->as.cell.value.bits, __ATOMIC_ACQUIRE);
    return out;
}

bool idm_cell_set(IdmValue cell, IdmValue value, IdmError *err) {
    if (idm_value_tag(cell) != IDM_VAL_CELL) return idm_error_set(err, idm_span_unknown(NULL), "expected cell");
    IdmObject *obj = idm_boxed_object(cell);
    heap_lock(obj->heap);
    if (atomic_load_explicit(&obj->heap->gc_marking, memory_order_acquire)) {
        gc_grey_value_unlocked(obj->heap, obj->as.cell.value);
    }
    __atomic_store_n(&obj->as.cell.value.bits, value.bits, __ATOMIC_RELEASE);
    heap_unlock(obj->heap);
    return true;
}


uint32_t idm_closure_function_index(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_CLOSURE ? idm_boxed_object(value)->as.closure.function_index : UINT32_MAX;
}

bool idm_closure_info(IdmValue value, IdmClosureInfo *out) {
    if (idm_value_tag(value) != IDM_VAL_CLOSURE) return false;
    if (out) {
        const IdmClosureObj *closure = &idm_boxed_object(value)->as.closure;
        out->module = closure->module;
        out->function_index = closure->function_index;
        out->entries = closure->entries;
        out->entry_count = closure->entry_count;
        out->captures = closure->captures;
        out->capture_count = closure->capture_count;
        out->env = closure->env;
        out->selector = closure->selector;
        out->selector_generation = closure->selector_generation;
    }
    return true;
}

const IdmBytecodeModule *idm_closure_module(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_CLOSURE ? idm_boxed_object(value)->as.closure.module : NULL;
}

size_t idm_closure_entry_count(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_CLOSURE ? idm_boxed_object(value)->as.closure.entry_count : 0;
}

uint32_t idm_closure_entry(IdmValue value, size_t index, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_CLOSURE) {
        idm_error_set(err, idm_span_unknown(NULL), "expected closure");
        return UINT32_MAX;
    }
    if (index >= idm_boxed_object(value)->as.closure.entry_count) {
        idm_error_set(err, idm_span_unknown(NULL), "closure entry index out of bounds");
        return UINT32_MAX;
    }
    if (idm_boxed_object(value)->as.closure.entry_count == 1) return idm_boxed_object(value)->as.closure.function_index;
    return idm_boxed_object(value)->as.closure.entries[index];
}

size_t idm_closure_capture_count(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_CLOSURE ? idm_boxed_object(value)->as.closure.capture_count : 0;
}

IdmEnv *idm_closure_env(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_CLOSURE ? idm_boxed_object(value)->as.closure.env : NULL;
}

IdmPatternSelector *idm_closure_selector(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_CLOSURE ? idm_boxed_object(value)->as.closure.selector : NULL;
}

uint64_t idm_closure_selector_generation(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_CLOSURE ? idm_boxed_object(value)->as.closure.selector_generation : 0;
}

IdmValue idm_closure_capture(IdmValue value, size_t index, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_CLOSURE) {
        idm_error_set(err, idm_span_unknown(NULL), "expected closure");
        return idm_nil();
    }
    if (index >= idm_boxed_object(value)->as.closure.capture_count) {
        idm_error_set(err, idm_span_unknown(NULL), "closure capture index out of bounds");
        return idm_nil();
    }
    return idm_boxed_object(value)->as.closure.captures[index];
}

const char *idm_record_type(IdmValue value, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_RECORD) {
        idm_error_set(err, idm_span_unknown(NULL), "expected record");
        return NULL;
    }
    return record_shape_type_text(idm_boxed_object(value)->as.record.shape);
}

IdmSymbol *idm_record_type_symbol(IdmValue value, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_RECORD) {
        idm_error_set(err, idm_span_unknown(NULL), "expected record");
        return NULL;
    }
    return idm_boxed_object(value)->as.record.shape->type;
}

size_t idm_record_field_count(IdmValue value, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_RECORD) {
        idm_error_set(err, idm_span_unknown(NULL), "expected record");
        return 0;
    }
    return idm_boxed_object(value)->as.record.shape->field_count;
}

const char *idm_record_field_name(IdmValue value, size_t index, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_RECORD) {
        idm_error_set(err, idm_span_unknown(NULL), "expected record");
        return NULL;
    }
    if (index >= idm_boxed_object(value)->as.record.shape->field_count) {
        idm_error_set(err, idm_span_unknown(NULL), "record field index out of bounds");
        return NULL;
    }
    return record_shape_field_text(idm_boxed_object(value)->as.record.shape, index);
}

IdmSymbol *idm_record_field_name_symbol(IdmValue value, size_t index, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_RECORD) {
        idm_error_set(err, idm_span_unknown(NULL), "expected record");
        return NULL;
    }
    if (index >= idm_boxed_object(value)->as.record.shape->field_count) {
        idm_error_set(err, idm_span_unknown(NULL), "record field index out of bounds");
        return NULL;
    }
    return idm_boxed_object(value)->as.record.shape->field_names[index];
}

IdmValue idm_record_field_value(IdmValue value, size_t index, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_RECORD) {
        idm_error_set(err, idm_span_unknown(NULL), "expected record");
        return idm_nil();
    }
    if (index >= idm_boxed_object(value)->as.record.shape->field_count) {
        idm_error_set(err, idm_span_unknown(NULL), "record field index out of bounds");
        return idm_nil();
    }
    return idm_boxed_object(value)->as.record.field_values[index];
}

bool idm_record_field_project_symbols(IdmValue value, IdmSymbol *type, IdmSymbol *field, uint32_t field_index, IdmValue *out, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_RECORD) return idm_error_set(err, idm_span_unknown(NULL), "record field access expects a record");
    if (!type) return idm_error_set(err, idm_span_unknown(NULL), "record field type must be a non-empty name");
    if (!field) return idm_error_set(err, idm_span_unknown(NULL), "record field must be a non-empty name");
    IdmRecordShape *shape = idm_boxed_object(value)->as.record.shape;
    const char *field_text = idm_symbol_text(field);
    const char *type_text = idm_symbol_text(type);
    if (shape->type != type) {
        return idm_error_set(err, idm_span_unknown(NULL), "field '%s' expects record type '%s', got '%s'", field_text, type_text, record_shape_type_text(shape));
    }
    if ((size_t)field_index >= shape->field_count) {
        return idm_error_set(err, idm_span_unknown(NULL), "record '%s' has no field slot %u for '%s'", type_text, field_index, field_text);
    }
    IdmSymbol *actual = shape->field_names[field_index];
    if (actual != field) {
        return idm_error_set(err, idm_span_unknown(NULL), "record '%s' field slot %u is '%s', not '%s'", type_text, field_index, idm_symbol_text(actual), field_text);
    }
    if (out) *out = idm_boxed_object(value)->as.record.field_values[field_index];
    return true;
}

bool idm_record_is_symbol(IdmValue value, IdmSymbol *type) {
    if (!type) return false;
    if (type->builtin_type == IDM_BUILTIN_TYPE_RECORD) return idm_value_tag(value) == IDM_VAL_RECORD;
    return idm_value_tag(value) == IDM_VAL_RECORD && idm_boxed_object(value)->as.record.shape->type == type;
}

IdmRegex *idm_regex_value_get(IdmValue value, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_REGEX) {
        idm_error_set(err, idm_span_unknown(NULL), "expected regex");
        return NULL;
    }
    return idm_boxed_object(value)->as.regex;
}

IdmRegexResult *idm_regex_result_value_get(IdmValue value, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_REGEX_RESULT) {
        idm_error_set(err, idm_span_unknown(NULL), "expected regex result");
        return NULL;
    }
    return idm_boxed_object(value)->as.regex_result;
}

IdmValue idm_car(IdmValue pair, IdmError *err) {
    if (idm_value_tag(pair) != IDM_VAL_PAIR) {
        idm_error_set(err, idm_span_unknown(NULL), "first expects a pair");
        return idm_nil();
    }
    return idm_boxed_object(pair)->as.pair.car;
}

IdmValue idm_cdr(IdmValue pair, IdmError *err) {
    if (idm_value_tag(pair) != IDM_VAL_PAIR) {
        idm_error_set(err, idm_span_unknown(NULL), "rest expects a pair");
        return idm_nil();
    }
    return idm_boxed_object(pair)->as.pair.cdr;
}

size_t idm_sequence_count(IdmValue value) {
    if (idm_value_tag(value) != IDM_VAL_TUPLE && idm_value_tag(value) != IDM_VAL_VECTOR) return 0;
    return idm_boxed_object(value)->as.sequence.count;
}

IdmValue idm_sequence_item(IdmValue value, size_t index, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_TUPLE && idm_value_tag(value) != IDM_VAL_VECTOR) {
        idm_error_set(err, idm_span_unknown(NULL), "expected tuple or vector");
        return idm_nil();
    }
    if (index >= idm_boxed_object(value)->as.sequence.count) {
        idm_error_set(err, idm_span_unknown(NULL), "sequence index out of bounds");
        return idm_nil();
    }
    return idm_boxed_object(value)->as.sequence.items[index];
}

bool idm_value_is_error(IdmValue value) {
    if (idm_value_tag(value) != IDM_VAL_TUPLE || idm_boxed_object(value)->as.sequence.count == 0) return false;
    IdmValue tag = idm_boxed_object(value)->as.sequence.items[0];
    return idm_value_tag(tag) == IDM_VAL_ATOM && idm_value_symbol(tag)->error_atom;
}

bool idm_value_ok(IdmValue value) {
    if (idm_value_tag(value) == IDM_VAL_NIL) return false;
    if (idm_value_tag(value) == IDM_VAL_ATOM && idm_value_symbol(value)->falsy) return false;
    if (idm_value_is_error(value)) return false;
    return true;
}

size_t idm_dict_count(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_DICT ? idm_boxed_object(value)->as.dict.count : 0;
}

bool idm_dict_get(IdmValue dict, IdmValue key, IdmValue *out) {
    if (idm_value_tag(dict) != IDM_VAL_DICT) return false;
    const IdmObject *obj = idm_boxed_object(dict);
    if (obj->as.dict.count == 0) return false;
    const IdmObject *root = idm_boxed_object(obj->as.dict.root);
    if (root->as.dict_node.collision_count != 0) {
        for (uint16_t i = 0; i < root->as.dict_node.collision_count; i++) {
            if (dict_key_equal(root->as.dict_node.slots[2u * i], key)) {
                *out = root->as.dict_node.slots[2u * i + 1u];
                return true;
            }
        }
        return false;
    }
    return dict_node_get(root, key, dict_key_hash(key), 0u, out);
}

bool idm_dict_iter_init(IdmValue dict, IdmDictIter *it) {
    memset(it, 0, sizeof(*it));
    if (idm_value_tag(dict) != IDM_VAL_DICT) return false;
    const IdmObject *obj = idm_boxed_object(dict);
    if (obj->as.dict.count == 0) return true;
    it->stack[0] = idm_boxed_object(obj->as.dict.root);
    it->cursor[0] = 0;
    it->depth = 1;
    return true;
}

bool idm_dict_iter_next(IdmDictIter *it, IdmValue *out_key, IdmValue *out_value) {
    while (it->depth > 0) {
        const IdmObject *node = it->stack[it->depth - 1];
        size_t cur = it->cursor[it->depth - 1];
        size_t data_slots = node->as.dict_node.collision_count != 0
            ? (size_t)node->as.dict_node.collision_count * 2u
            : (size_t)__builtin_popcount(node->as.dict_node.datamap) * 2u;
        size_t total = data_slots + (node->as.dict_node.collision_count != 0 ? 0u : (size_t)__builtin_popcount(node->as.dict_node.nodemap));
        if (cur >= total) {
            it->depth--;
            continue;
        }
        if (cur < data_slots) {
            *out_key = node->as.dict_node.slots[cur];
            *out_value = node->as.dict_node.slots[cur + 1u];
            it->cursor[it->depth - 1] = cur + 2u;
            return true;
        }
        it->cursor[it->depth - 1] = cur + 1u;
        if (it->depth >= (int)(sizeof(it->stack) / sizeof(it->stack[0]))) return false;
        it->stack[it->depth] = idm_boxed_object(node->as.dict_node.slots[cur]);
        it->cursor[it->depth] = 0;
        it->depth++;
    }
    return false;
}

IdmValue idm_dict_put(IdmRuntime *rt, IdmValue dict, IdmValue key, IdmValue value, IdmError *err) {
    if (idm_value_tag(dict) != IDM_VAL_DICT) {
        idm_error_set(err, idm_span_unknown(NULL), "dict-put expects a dict");
        return idm_nil();
    }
    const IdmObject *src = idm_boxed_object(dict);
    IdmObject *new_root = NULL;
    bool added = false;
    const IdmObject *root_node = src->as.dict.count == 0 ? NULL : idm_boxed_object(src->as.dict.root);
    if (src->as.dict.count == 0) {
        new_root = dict_node_alloc(rt, 0, 0, 1u, 2u, err);
        if (!new_root) return idm_nil();
        new_root->as.dict_node.slots[0] = key;
        new_root->as.dict_node.slots[1] = value;
        added = true;
    } else if (root_node->as.dict_node.collision_count != 0) {
        uint16_t cc = root_node->as.dict_node.collision_count;
        uint16_t found = UINT16_MAX;
        for (uint16_t i = 0; i < cc; i++) {
            if (dict_key_equal(root_node->as.dict_node.slots[2u * i], key)) { found = i; break; }
        }
        if (found != UINT16_MAX) {
            new_root = dict_node_alloc(rt, 0, 0, cc, (size_t)cc * 2u, err);
            if (!new_root) return idm_nil();
            memcpy(new_root->as.dict_node.slots, root_node->as.dict_node.slots, (size_t)cc * 2u * sizeof(IdmValue));
            new_root->as.dict_node.slots[2u * found + 1u] = value;
        } else if (cc < 8u) {
            new_root = dict_node_alloc(rt, 0, 0, (uint16_t)(cc + 1u), (size_t)(cc + 1u) * 2u, err);
            if (!new_root) return idm_nil();
            memcpy(new_root->as.dict_node.slots, root_node->as.dict_node.slots, (size_t)cc * 2u * sizeof(IdmValue));
            new_root->as.dict_node.slots[2u * cc] = key;
            new_root->as.dict_node.slots[2u * cc + 1u] = value;
            added = true;
        } else {
            uint32_t first_hash = dict_key_hash(root_node->as.dict_node.slots[0]);
            new_root = dict_node_alloc(rt, 1u << (first_hash & 31u), 0, 0, 2u, err);
            if (!new_root) return idm_nil();
            new_root->as.dict_node.slots[0] = root_node->as.dict_node.slots[0];
            new_root->as.dict_node.slots[1] = root_node->as.dict_node.slots[1];
            for (uint16_t i = 1u; i < cc; i++) {
                bool ignored = false;
                IdmValue ik = root_node->as.dict_node.slots[2u * i];
                new_root = dict_node_put(rt, new_root, ik, root_node->as.dict_node.slots[2u * i + 1u], dict_key_hash(ik), 0u, &ignored, err);
                if (!new_root) return idm_nil();
            }
            new_root = dict_node_put(rt, new_root, key, value, dict_key_hash(key), 0u, &added, err);
            if (!new_root) return idm_nil();
        }
    } else {
        new_root = dict_node_put(rt, root_node, key, value, dict_key_hash(key), 0u, &added, err);
        if (!new_root) return idm_nil();
    }
    IdmValue out = dict_empty(rt, err);
    if (idm_value_tag(out) != IDM_VAL_DICT) return out;
    idm_boxed_object(out)->as.dict.count = src->as.dict.count + (added ? 1u : 0u);
    idm_boxed_object(out)->as.dict.root = idm_from_boxed(new_root);
    return out;
}

IdmValue idm_dict_del(IdmRuntime *rt, IdmValue dict, IdmValue key, IdmError *err) {
    if (idm_value_tag(dict) != IDM_VAL_DICT) {
        idm_error_set(err, idm_span_unknown(NULL), "dict-del expects a dict");
        return idm_nil();
    }
    const IdmObject *src = idm_boxed_object(dict);
    if (src->as.dict.count == 0) return dict;
    bool removed = false;
    bool emptied = false;
    IdmObject *new_root = dict_node_del(rt, idm_boxed_object(src->as.dict.root), key, dict_key_hash(key), 0u, &removed, &emptied, err);
    if (!new_root) return idm_nil();
    if (!removed) return dict;
    IdmValue out = dict_empty(rt, err);
    if (idm_value_tag(out) != IDM_VAL_DICT) return out;
    idm_boxed_object(out)->as.dict.count = src->as.dict.count - 1u;
    idm_boxed_object(out)->as.dict.root = emptied ? idm_nil() : idm_from_boxed(new_root);
    return out;
}

IdmValue idm_dict(IdmRuntime *rt, const IdmDictEntry *entries, size_t count, IdmError *err) {
    IdmValue v = dict_empty(rt, err);
    if (idm_value_tag(v) != IDM_VAL_DICT) return v;
    for (size_t i = 0; i < count; i++) {
        v = idm_dict_put(rt, v, entries[i].key, entries[i].value, err);
        if (idm_value_tag(v) != IDM_VAL_DICT) return v;
    }
    return v;
}

const IdmSyntax *idm_syntax_get(IdmValue value, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_SYNTAX) {
        idm_error_set(err, idm_span_unknown(NULL), "expected syntax");
        return NULL;
    }
    return idm_boxed_object(value)->as.syntax;
}

IdmSyntax *idm_syntax_value_get(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_SYNTAX ? idm_boxed_object(value)->as.syntax : NULL;
}

const char *idm_string_bytes(IdmValue value) {
    if (idm_value_tag(value) != IDM_VAL_STRING) return "";
    IdmObject *obj = idm_boxed_object(value);
    if (obj->kind == IDM_OBJ_STRING) return obj->as.string.bytes;
    return rope_flatten(obj);
}

size_t idm_string_length(IdmValue value) {
    if (idm_value_tag(value) != IDM_VAL_STRING) return 0;
    const IdmObject *obj = idm_boxed_object(value);
    return obj->kind == IDM_OBJ_STRING ? obj->as.string.len : obj->as.rope.len;
}

bool idm_is_bits(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_BITSTRING;
}

uint64_t idm_bits_len(IdmValue value) {
    return idm_is_bits(value) ? idm_boxed_object(value)->as.bits.bit_len : 0u;
}

static IdmValue bits_adopt_root_cap(IdmRuntime *rt, unsigned char *bytes, uint64_t bit_len, uint64_t bit_cap, IdmError *err) {
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_BITSTRING);
    if (!obj) {
        free(bytes);
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.bits.parent = idm_nil();
    obj->as.bits.bytes = bytes;
    obj->as.bits.bit_off = 0;
    obj->as.bits.bit_len = bit_len;
    obj->as.bits.bit_cap = bit_cap;
    obj->as.bits.bit_used = bit_len;
    heap_account(idm_active_heap(rt), obj, (size_t)((bit_cap + 7u) / 8u));
    return idm_from_boxed(obj);
}

static IdmValue bits_adopt_root(IdmRuntime *rt, unsigned char *bytes, uint64_t bit_len, IdmError *err) {
    return bits_adopt_root_cap(rt, bytes, bit_len, bit_len, err);
}

static bool bits_byte_count(uint64_t bit_len, size_t *out, IdmError *err) {
    if (bit_len > UINT64_MAX - 7u) return idm_error_set(err, idm_span_unknown(NULL), "bitstring is too large");
    uint64_t nbytes = (bit_len + 7u) / 8u;
    if (nbytes > SIZE_MAX) return idm_error_set(err, idm_span_unknown(NULL), "bitstring is too large");
    *out = (size_t)nbytes;
    return true;
}

IdmValue idm_bits_from_bytes(IdmRuntime *rt, const unsigned char *bytes, uint64_t bit_len, IdmError *err) {
    size_t nbytes = 0;
    if (!bits_byte_count(bit_len, &nbytes, err)) return idm_nil();
    unsigned char *copy = NULL;
    if (nbytes != 0u) {
        copy = malloc(nbytes);
        if (!copy) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return idm_nil();
        }
        memcpy(copy, bytes, nbytes);
        if ((bit_len & 7u) != 0u) copy[nbytes - 1u] &= (unsigned char)(0xFFu << (8u - (unsigned)(bit_len & 7u)));
    }
    return bits_adopt_root(rt, copy, bit_len, err);
}

IdmValue idm_bits_slice(IdmRuntime *rt, IdmValue bits, uint64_t off, uint64_t len, IdmError *err) {
    if (!idm_is_bits(bits)) {
        idm_error_set(err, idm_span_unknown(NULL), "expected bitstring");
        return idm_nil();
    }
    const IdmBitstringObj *src = &idm_boxed_object(bits)->as.bits;
    if (off > src->bit_len || len > src->bit_len - off) {
        idm_error_set(err, idm_span_unknown(NULL), "bitstring slice out of range");
        return idm_nil();
    }
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_BITSTRING);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.bits.parent = idm_is_nil(src->parent) ? bits : src->parent;
    obj->as.bits.bytes = src->bytes;
    obj->as.bits.bit_off = src->bit_off + off;
    obj->as.bits.bit_len = len;
    obj->as.bits.bit_cap = 0;
    obj->as.bits.bit_used = 0;
    return idm_from_boxed(obj);
}

static void bits_read_limbs(const unsigned char *bytes, uint64_t base, uint64_t len, bool little, uint32_t *limbs) {
    uint64_t k = 0;
    if ((base & 7u) == 0u && (len & 7u) == 0u) {
        const unsigned char *p = bytes + (base >> 3);
        uint64_t nbytes = len >> 3;
        if (little) {
            for (uint64_t i = 0; i < nbytes; i++, k += 8u) limbs[k >> 5] |= (uint32_t)p[i] << (k & 31u);
        } else {
            for (uint64_t i = nbytes; i > 0; i--, k += 8u) limbs[k >> 5] |= (uint32_t)p[i - 1u] << (k & 31u);
        }
        return;
    }
    if (little) {
        uint64_t pos = 0;
        while (len - pos >= 8u) {
            limbs[k >> 5] |= (uint32_t)bits_chunk(bytes, base + pos, 8u) << (k & 31u);
            pos += 8u;
            k += 8u;
        }
        unsigned rem = (unsigned)(len - pos);
        if (rem != 0u) limbs[k >> 5] |= (uint32_t)(bits_chunk(bytes, base + pos, rem) >> (8u - rem)) << (k & 31u);
        return;
    }
    uint64_t pos = len;
    while (pos >= 8u) {
        pos -= 8u;
        limbs[k >> 5] |= (uint32_t)bits_chunk(bytes, base + pos, 8u) << (k & 31u);
        k += 8u;
    }
    if (pos != 0u) limbs[k >> 5] |= (uint32_t)(bits_chunk(bytes, base, (unsigned)pos) >> (8u - (unsigned)pos)) << (k & 31u);
}

IdmValue idm_bits_int(IdmRuntime *rt, IdmValue bits, uint64_t off, uint64_t len, bool little, bool is_signed, IdmError *err) {
    if (!idm_is_bits(bits)) {
        idm_error_set(err, idm_span_unknown(NULL), "expected bitstring");
        return idm_nil();
    }
    const IdmBitstringObj *bs = &idm_boxed_object(bits)->as.bits;
    if (off > bs->bit_len || len > bs->bit_len - off) {
        idm_error_set(err, idm_span_unknown(NULL), "bitstring read out of range");
        return idm_nil();
    }
    if (len == 0u) return idm_int(0);
    size_t width = (size_t)((len + 31u) / 32u);
    uint32_t stack_limbs[64];
    uint32_t *limbs = stack_limbs;
    if (len > 2048u) {
        limbs = calloc(width, sizeof(*limbs));
        if (!limbs) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return idm_nil();
        }
    } else {
        memset(stack_limbs, 0, width * sizeof(*stack_limbs));
    }
    bits_read_limbs(bs->bytes, bs->bit_off + off, len, little, limbs);
    bool neg = false;
    if (is_signed) {
        uint64_t top = len - 1u;
        neg = ((limbs[top >> 5] >> (top & 31u)) & 1u) != 0u;
    }
    if (neg) {
        for (size_t w = 0; w < width; w++) limbs[w] = ~limbs[w];
        unsigned rem = (unsigned)(len & 31u);
        if (rem != 0u) limbs[width - 1u] &= 0xFFFFFFFFu >> (32u - rem);
        uint64_t carry = 1u;
        for (size_t w = 0; w < width && carry != 0u; w++) {
            uint64_t v = (uint64_t)limbs[w] + carry;
            limbs[w] = (uint32_t)v;
            carry = v >> 32;
        }
    }
    size_t count = limb_normalize(limbs, width);
    int sign = count == 0u ? 0 : (neg ? -1 : 1);
    if (limbs == stack_limbs) return idm_int_from_limbs(rt, limbs, count, sign, err);
    return make_bignum_adopt(rt, limbs, count, sign, err);
}

static void bits_put_chunk(unsigned char *bytes, uint64_t off, unsigned n, uint8_t c) {
    size_t i = (size_t)(off >> 3);
    unsigned sh = (unsigned)(off & 7u);
    c &= (uint8_t)(0xFFu << (8u - n));
    bytes[i] |= (uint8_t)(c >> sh);
    if (sh + n > 8u) bytes[i + 1u] |= (uint8_t)(c << (8u - sh));
}

static void bits_write_chunks(unsigned char *bytes, uint64_t len, bool little, const uint32_t *limbs) {
    uint64_t k = 0;
    if (little) {
        uint64_t pos = 0;
        while (len - pos >= 8u) {
            bits_put_chunk(bytes, pos, 8u, (uint8_t)(limbs[k >> 5] >> (k & 31u)));
            pos += 8u;
            k += 8u;
        }
        unsigned rem = (unsigned)(len - pos);
        if (rem != 0u) bits_put_chunk(bytes, pos, rem, (uint8_t)((limbs[k >> 5] >> (k & 31u)) << (8u - rem)));
        return;
    }
    uint64_t pos = len;
    while (pos >= 8u) {
        pos -= 8u;
        bits_put_chunk(bytes, pos, 8u, (uint8_t)(limbs[k >> 5] >> (k & 31u)));
        k += 8u;
    }
    if (pos != 0u) bits_put_chunk(bytes, 0u, (unsigned)pos, (uint8_t)((limbs[k >> 5] >> (k & 31u)) << (8u - (unsigned)pos)));
}

IdmValue idm_bits_of_int(IdmRuntime *rt, IdmValue n, uint64_t len, bool little, IdmError *err) {
    if (len == 0u) return bits_adopt_root(rt, NULL, 0u, err);
    size_t nbytes = 0;
    if (!bits_byte_count(len, &nbytes, err)) return idm_nil();
    size_t width = (size_t)((len + 31u) / 32u);
    uint32_t stack_limbs[4] = {0};
    uint32_t *limbs = stack_limbs;
    if (len > 128u) {
        limbs = calloc(width, sizeof(*limbs));
        if (!limbs) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return idm_nil();
        }
    }
    unsigned char *bytes = calloc(nbytes, 1u);
    if (!bytes) {
        if (limbs != stack_limbs) free(limbs);
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    twos_from_int(n, limbs, width);
    bits_write_chunks(bytes, len, little, limbs);
    if (limbs != stack_limbs) free(limbs);
    return bits_adopt_root(rt, bytes, len, err);
}

IdmValue idm_bits_append(IdmRuntime *rt, IdmValue a, IdmValue b, IdmError *err) {
    if (!idm_is_bits(a) || !idm_is_bits(b)) {
        idm_error_set(err, idm_span_unknown(NULL), "expected bitstring");
        return idm_nil();
    }
    const IdmBitstringObj *x = &idm_boxed_object(a)->as.bits;
    const IdmBitstringObj *y = &idm_boxed_object(b)->as.bits;
    if (x->bit_len > UINT64_MAX - y->bit_len) {
        idm_error_set(err, idm_span_unknown(NULL), "bitstring is too large");
        return idm_nil();
    }
    uint64_t len = x->bit_len + y->bit_len;
    IdmObject *root_obj = idm_is_nil(x->parent) ? idm_boxed_object(a) : idm_boxed_object(x->parent);
    IdmBitstringObj *root = &root_obj->as.bits;
    uint64_t end = x->bit_off + x->bit_len;
    if (idm_is_nil(root->parent) && root_obj->heap != &rt->immortal &&
        end == root->bit_used && y->bit_len <= root->bit_cap - root->bit_used) {
        bits_blit(root->bytes, end, y->bytes, y->bit_off, y->bit_len);
        root->bit_used = end + y->bit_len;
        IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_BITSTRING);
        if (!obj) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return idm_nil();
        }
        obj->as.bits.parent = idm_is_nil(x->parent) ? a : x->parent;
        obj->as.bits.bytes = root->bytes;
        obj->as.bits.bit_off = x->bit_off;
        obj->as.bits.bit_len = len;
        obj->as.bits.bit_cap = 0;
        obj->as.bits.bit_used = 0;
        return idm_from_boxed(obj);
    }
    uint64_t cap = len < UINT64_MAX / 2u ? len * 2u : len;
    if (cap < 256u) cap = 256u;
    size_t nbytes = 0;
    if (!bits_byte_count(len, &nbytes, err)) return idm_nil();
    if ((cap + 7u) / 8u > (uint64_t)SIZE_MAX) cap = len;
    unsigned char *bytes = calloc((size_t)((cap + 7u) / 8u), 1u);
    if (!bytes) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    bits_blit(bytes, 0u, x->bytes, x->bit_off, x->bit_len);
    bits_blit(bytes, x->bit_len, y->bytes, y->bit_off, y->bit_len);
    return bits_adopt_root_cap(rt, bytes, len, cap, err);
}

IdmValue idm_bits_float(IdmRuntime *rt, IdmValue bits, uint64_t off, uint64_t len, bool little, IdmError *err) {
    if (!idm_is_bits(bits)) {
        idm_error_set(err, idm_span_unknown(NULL), "expected bitstring");
        return idm_nil();
    }
    const IdmBitstringObj *bs = &idm_boxed_object(bits)->as.bits;
    if (len != 32u && len != 64u) {
        idm_error_set(err, idm_span_unknown(NULL), "bitstring float read requires a bit length of 32 or 64");
        return idm_nil();
    }
    if (off > bs->bit_len || len > bs->bit_len - off) {
        idm_error_set(err, idm_span_unknown(NULL), "bitstring read out of range");
        return idm_nil();
    }
    unsigned char buf[8] = {0};
    bits_blit(buf, 0u, bs->bytes, bs->bit_off + off, len);
    size_t nbytes = (size_t)(len / 8u);
    if (little) {
        for (size_t i = 0; i < nbytes / 2u; i++) {
            unsigned char t = buf[i];
            buf[i] = buf[nbytes - 1u - i];
            buf[nbytes - 1u - i] = t;
        }
    }
    double d = 0.0;
    if (len == 32u) {
        uint32_t u = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
        float f = 0.0f;
        memcpy(&f, &u, 4u);
        d = (double)f;
    } else {
        uint64_t u = 0;
        for (size_t i = 0; i < 8u; i++) u = (u << 8) | buf[i];
        memcpy(&d, &u, 8u);
    }
    return idm_float(rt, d, err);
}

bool idm_bits_read(IdmValue bits, uint64_t off, uint64_t len, unsigned char *dst) {
    if (!idm_is_bits(bits)) return false;
    const IdmBitstringObj *bs = &idm_boxed_object(bits)->as.bits;
    if (off > bs->bit_len || len > bs->bit_len - off) return false;
    memset(dst, 0, (size_t)((len + 7u) / 8u));
    bits_blit(dst, 0u, bs->bytes, bs->bit_off + off, len);
    return true;
}

int idm_bits_compare(IdmValue a, IdmValue b) {
    const IdmBitstringObj *x = &idm_boxed_object(a)->as.bits;
    const IdmBitstringObj *y = &idm_boxed_object(b)->as.bits;
    return bits_order(x->bytes, x->bit_off, x->bit_len, y->bytes, y->bit_off, y->bit_len);
}

bool idm_value_equal(IdmValue a, IdmValue b) {
    IdmValueTag ta = idm_value_tag(a), tb = idm_value_tag(b);
    if (idm_value_is_int(a) && idm_value_is_int(b)) return idm_int_compare(a, b) == 0;
    if (ta != tb) return false;
    switch (ta) {
        case IDM_VAL_NIL: return true;
        case IDM_VAL_ATOM:
        case IDM_VAL_WORD: return idm_value_symbol(a) == idm_value_symbol(b);
        case IDM_VAL_INT: return idm_int_value(a) == idm_int_value(b);
        case IDM_VAL_FLOAT: return idm_float_value(a) == idm_float_value(b);
        case IDM_VAL_STRING:
            return string_equal_val(a, b);
        case IDM_VAL_PAIR:
            return idm_value_equal(idm_boxed_object(a)->as.pair.car, idm_boxed_object(b)->as.pair.car) &&
                   idm_value_equal(idm_boxed_object(a)->as.pair.cdr, idm_boxed_object(b)->as.pair.cdr);
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR:
            if (idm_boxed_object(a)->as.sequence.count != idm_boxed_object(b)->as.sequence.count) return false;
            for (size_t i = 0; i < idm_boxed_object(a)->as.sequence.count; i++) {
                if (!idm_value_equal(idm_boxed_object(a)->as.sequence.items[i], idm_boxed_object(b)->as.sequence.items[i])) return false;
            }
            return true;
        case IDM_VAL_DICT: {
            if (idm_boxed_object(a)->as.dict.count != idm_boxed_object(b)->as.dict.count) return false;
            IdmDictIter it;
            IdmValue k, v;
            idm_dict_iter_init(a, &it);
            while (idm_dict_iter_next(&it, &k, &v)) {
                IdmValue other;
                if (!idm_dict_get(b, k, &other)) return false;
                if (!idm_value_equal(v, other)) return false;
            }
            return true;
        }
        case IDM_VAL_SYNTAX:
            return idm_boxed_object(a) == idm_boxed_object(b);
        case IDM_VAL_CELL:
            return idm_boxed_object(a) == idm_boxed_object(b);
        case IDM_VAL_CLOSURE:
            return idm_boxed_object(a) == idm_boxed_object(b);
        case IDM_VAL_RECORD:
            if (idm_boxed_object(a)->as.record.shape != idm_boxed_object(b)->as.record.shape) return false;
            for (size_t i = 0; i < idm_boxed_object(a)->as.record.shape->field_count; i++) {
                if (!idm_value_equal(idm_boxed_object(a)->as.record.field_values[i], idm_boxed_object(b)->as.record.field_values[i])) return false;
            }
            return true;
        case IDM_VAL_REGEX:
            return idm_boxed_object(a) == idm_boxed_object(b);
        case IDM_VAL_REGEX_RESULT:
            return idm_boxed_object(a) == idm_boxed_object(b);
        case IDM_VAL_PID:
        case IDM_VAL_REF:
        case IDM_VAL_PORT:
            return idm_value_id(a) == idm_value_id(b);
        case IDM_VAL_BIGNUM:
            return idm_int_compare(a, b) == 0;
        case IDM_VAL_BITSTRING: {
            const IdmBitstringObj *x = &idm_boxed_object(a)->as.bits;
            const IdmBitstringObj *y = &idm_boxed_object(b)->as.bits;
            return x->bit_len == y->bit_len &&
                   bits_order(x->bytes, x->bit_off, x->bit_len, y->bytes, y->bit_off, y->bit_len) == 0;
        }
    }
    return false;
}

static bool write_sequence_item(IdmBuffer *buf, size_t index, void *user) {
    IdmObject *obj = user;
    return idm_value_write(buf, obj->as.sequence.items[index]);
}



bool idm_value_write(IdmBuffer *buf, IdmValue value) {
    switch (idm_value_tag(value)) {
        case IDM_VAL_NIL: return idm_buf_append(buf, ":nil");
        case IDM_VAL_ATOM: return idm_buf_append_char(buf, ':') && idm_buf_append(buf, idm_symbol_text(idm_value_symbol(value)));
        case IDM_VAL_WORD: return idm_buf_append(buf, idm_symbol_text(idm_value_symbol(value)));
        case IDM_VAL_INT: return idm_buf_appendf(buf, "%lld", (long long)idm_int_value(value));
        case IDM_VAL_FLOAT: {
            char text[40];
            snprintf(text, sizeof(text), "%g", idm_float_value(value));
            return idm_buf_append(buf, text) && (strpbrk(text, ".eEn") != NULL || idm_buf_append(buf, ".0"));
        }
        case IDM_VAL_STRING: return idm_surface_write_escaped(buf, idm_string_bytes(value), idm_string_length(value));
        case IDM_VAL_PID: return idm_buf_appendf(buf, "#<pid:%llu>", (unsigned long long)idm_value_id(value));
        case IDM_VAL_REF: return idm_buf_appendf(buf, "#<ref:%llu>", (unsigned long long)idm_value_id(value));
        case IDM_VAL_PORT: return idm_buf_appendf(buf, "#<port:%llu>", (unsigned long long)idm_value_id(value));
        case IDM_VAL_RECORD: return idm_buf_appendf(buf, "#<record:%s>", record_shape_type_text(idm_boxed_object(value)->as.record.shape));
        case IDM_VAL_REGEX: {
            size_t len = 0;
            const char *source = idm_regex_source(idm_boxed_object(value)->as.regex, &len);
            return idm_buf_append_char(buf, 'r') && idm_surface_write_escaped(buf, source, len);
        }
        case IDM_VAL_REGEX_RESULT:
            return idm_buf_append(buf, "#<regex-result>");
        case IDM_VAL_BIGNUM:
            return bignum_write(buf, value);
        case IDM_VAL_BITSTRING: {
            const IdmBitstringObj *bs = &idm_boxed_object(value)->as.bits;
            if (!idm_buf_append(buf, "%<")) return false;
            for (uint64_t done = 0; done < bs->bit_len; done += 8u) {
                unsigned n = bs->bit_len - done >= 8u ? 8u : (unsigned)(bs->bit_len - done);
                uint8_t c = bits_chunk(bs->bytes, bs->bit_off + done, n);
                if (done != 0 && !idm_buf_append_char(buf, ' ')) return false;
                if (n == 8u) {
                    if (!idm_buf_appendf(buf, "%u", (unsigned)c)) return false;
                } else if (!idm_buf_appendf(buf, "%u:%u", (unsigned)(c >> (8u - n)), n)) {
                    return false;
                }
            }
            return idm_buf_append_char(buf, '>');
        }
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR: {
            const char *open = idm_value_tag(value) == IDM_VAL_TUPLE ? "{" : "[";
            const char *close = idm_value_tag(value) == IDM_VAL_TUPLE ? "}" : "]";
            IdmObject *obj = idm_boxed_object(value);
            return idm_surface_write_sequence(buf, open, close, obj->as.sequence.count, write_sequence_item, obj);
        }
        case IDM_VAL_DICT: {
            if (!idm_buf_append(buf, "%{")) return false;
            IdmDictIter it;
            IdmValue k, v;
            idm_dict_iter_init(value, &it);
            bool first = true;
            while (idm_dict_iter_next(&it, &k, &v)) {
                if (!first && !idm_buf_append_char(buf, ' ')) return false;
                first = false;
                if (!idm_value_write(buf, k) || !idm_buf_append_char(buf, ' ') || !idm_value_write(buf, v)) return false;
            }
            return idm_buf_append_char(buf, '}');
        }
        case IDM_VAL_SYNTAX: return idm_buf_append(buf, "#<syntax>");
        case IDM_VAL_CELL: return idm_buf_append(buf, "#<cell>");
        case IDM_VAL_CLOSURE: return idm_buf_appendf(buf, "#<closure:%u>", idm_closure_function_index(value));
        case IDM_VAL_PAIR: {
            if (!idm_buf_append_char(buf, '(')) return false;
            IdmValue cur = value;
            bool first = true;
            while (idm_value_tag(cur) == IDM_VAL_PAIR) {
                if (!first && !idm_buf_append_char(buf, ' ')) return false;
                if (!idm_value_write(buf, idm_boxed_object(cur)->as.pair.car)) return false;
                cur = idm_boxed_object(cur)->as.pair.cdr;
                first = false;
            }
            if (idm_value_tag(cur) != IDM_VAL_NIL) {
                if (!idm_buf_append(buf, " . ")) return false;
                if (!idm_value_write(buf, cur)) return false;
            }
            return idm_buf_append_char(buf, ')');
        }
    }
    return false;
}

static bool value_write_json_opaque(IdmBuffer *buf, IdmValue value) {
    IdmBuffer scratch;
    idm_buf_init(&scratch);
    bool ok = idm_value_write(&scratch, value) &&
              idm_buf_append_json_string(buf, scratch.data ? scratch.data : "", scratch.len);
    idm_buf_destroy(&scratch);
    return ok;
}

static bool value_json_symbol_key(IdmBuffer *buf, IdmValue key) {
    switch (idm_value_tag(key)) {
        case IDM_VAL_ATOM:
        case IDM_VAL_WORD: {
            const char *text = idm_symbol_text(idm_value_symbol(key));
            return idm_buf_append_json_string(buf, text, strlen(text));
        }
        case IDM_VAL_STRING:
            return idm_buf_append_json_string(buf, idm_string_bytes(key), idm_string_length(key));
        default: return false;
    }
}

static bool value_json_key_is_name(IdmValue key) {
    IdmValueTag tag = idm_value_tag(key);
    return tag == IDM_VAL_ATOM || tag == IDM_VAL_WORD || tag == IDM_VAL_STRING;
}

bool idm_value_write_json(IdmBuffer *buf, IdmValue value) {
    switch (idm_value_tag(value)) {
        case IDM_VAL_NIL: return idm_buf_append(buf, "null");
        case IDM_VAL_ATOM: {
            const char *text = idm_symbol_text(idm_value_symbol(value));
            if (strcmp(text, "true") == 0) return idm_buf_append(buf, "true");
            if (strcmp(text, "false") == 0) return idm_buf_append(buf, "false");
            return idm_buf_append_json_string(buf, text, strlen(text));
        }
        case IDM_VAL_WORD: {
            const char *text = idm_symbol_text(idm_value_symbol(value));
            return idm_buf_append_json_string(buf, text, strlen(text));
        }
        case IDM_VAL_INT: return idm_buf_appendf(buf, "%lld", (long long)idm_int_value(value));
        case IDM_VAL_BIGNUM: return bignum_write(buf, value);
        case IDM_VAL_FLOAT: {
            double d = idm_float_value(value);
            if (!isfinite(d)) return idm_buf_append_json_string(buf, d != d ? "nan" : (d > 0 ? "inf" : "-inf"), d != d ? 3u : (d > 0 ? 3u : 4u));
            char text[40];
            snprintf(text, sizeof(text), "%.17g", d);
            return idm_buf_append(buf, text);
        }
        case IDM_VAL_STRING:
            return idm_buf_append_json_string(buf, idm_string_bytes(value), idm_string_length(value));
        case IDM_VAL_PAIR: {
            if (!idm_buf_append_char(buf, '[')) return false;
            IdmValue cur = value;
            bool first = true;
            while (idm_value_tag(cur) == IDM_VAL_PAIR) {
                if (!first && !idm_buf_append_char(buf, ',')) return false;
                if (!idm_value_write_json(buf, idm_boxed_object(cur)->as.pair.car)) return false;
                cur = idm_boxed_object(cur)->as.pair.cdr;
                first = false;
            }
            if (idm_value_tag(cur) != IDM_VAL_NIL) {
                if (!idm_buf_append_char(buf, ',')) return false;
                if (!idm_value_write_json(buf, cur)) return false;
            }
            return idm_buf_append_char(buf, ']');
        }
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR: {
            const IdmObject *obj = idm_boxed_object(value);
            if (!idm_buf_append_char(buf, '[')) return false;
            for (size_t i = 0; i < obj->as.sequence.count; i++) {
                if (i != 0 && !idm_buf_append_char(buf, ',')) return false;
                if (!idm_value_write_json(buf, obj->as.sequence.items[i])) return false;
            }
            return idm_buf_append_char(buf, ']');
        }
        case IDM_VAL_DICT: {
            IdmDictIter it;
            IdmValue k, v;
            bool names = true;
            idm_dict_iter_init(value, &it);
            while (names && idm_dict_iter_next(&it, &k, &v)) {
                if (!value_json_key_is_name(k)) {
                    names = false;
                    break;
                }
                IdmDictIter other;
                IdmValue k2, v2;
                idm_dict_iter_init(value, &other);
                while (idm_dict_iter_next(&other, &k2, &v2)) {
                    if (k2.bits == k.bits || idm_value_tag(k2) == idm_value_tag(k) || !value_json_key_is_name(k2)) continue;
                    IdmBuffer a, b;
                    idm_buf_init(&a);
                    idm_buf_init(&b);
                    bool same = value_json_symbol_key(&a, k) && value_json_symbol_key(&b, k2) &&
                                a.len == b.len && a.data && b.data && memcmp(a.data, b.data, a.len) == 0;
                    idm_buf_destroy(&a);
                    idm_buf_destroy(&b);
                    if (same) {
                        names = false;
                        break;
                    }
                }
            }
            idm_dict_iter_init(value, &it);
            bool first = true;
            if (names) {
                if (!idm_buf_append_char(buf, '{')) return false;
                while (idm_dict_iter_next(&it, &k, &v)) {
                    if (!first && !idm_buf_append_char(buf, ',')) return false;
                    first = false;
                    if (!value_json_symbol_key(buf, k) || !idm_buf_append_char(buf, ':') || !idm_value_write_json(buf, v)) return false;
                }
                return idm_buf_append_char(buf, '}');
            }
            if (!idm_buf_append_char(buf, '[')) return false;
            while (idm_dict_iter_next(&it, &k, &v)) {
                if (!first && !idm_buf_append_char(buf, ',')) return false;
                first = false;
                if (!idm_buf_append_char(buf, '[') || !idm_value_write_json(buf, k) || !idm_buf_append_char(buf, ',') ||
                    !idm_value_write_json(buf, v) || !idm_buf_append_char(buf, ']')) return false;
            }
            return idm_buf_append_char(buf, ']');
        }
        case IDM_VAL_RECORD: {
            const IdmRecordObj *rec = &idm_boxed_object(value)->as.record;
            const char *type = rec->shape ? idm_symbol_text(rec->shape->type) : "record";
            if (!idm_buf_append(buf, "{\"%type\":") || !idm_buf_append_json_string(buf, type, strlen(type))) return false;
            size_t count = rec->shape ? rec->shape->field_count : 0u;
            for (size_t i = 0; i < count; i++) {
                const char *name = idm_symbol_text(rec->shape->field_names[i]);
                if (!idm_buf_append_char(buf, ',') || !idm_buf_append_json_string(buf, name, strlen(name)) ||
                    !idm_buf_append_char(buf, ':') || !idm_value_write_json(buf, rec->field_values[i])) return false;
            }
            return idm_buf_append_char(buf, '}');
        }
        default: return value_write_json_opaque(buf, value);
    }
}

typedef struct {
    IdmObject **keys;
    IdmObject **vals;
    size_t count;
    size_t cap;
} CopyMap;

static IdmObject *copymap_find(const CopyMap *m, IdmObject *key) {
    if (m->cap == 0) return NULL;
    size_t mask = m->cap - 1u;
    size_t i = ((uintptr_t)key >> 4) & mask;
    while (m->keys[i]) {
        if (m->keys[i] == key) return m->vals[i];
        i = (i + 1u) & mask;
    }
    return NULL;
}

static bool copymap_put(CopyMap *m, IdmObject *key, IdmObject *val) {
    if (m->cap == 0 || m->count + 1u > m->cap - (m->cap >> 2)) {
        size_t ncap = 0;
        if (m->cap == SIZE_MAX || !idm_next_capacity(m->cap, 16u, m->cap + 1u, &ncap)) return false;
        IdmObject **nk = calloc(ncap, sizeof(*nk));
        IdmObject **nv = calloc(ncap, sizeof(*nv));
        if (!nk || !nv) { free(nk); free(nv); return false; }
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->keys[i]) continue;
            size_t j = ((uintptr_t)m->keys[i] >> 4) & (ncap - 1u);
            while (nk[j]) j = (j + 1u) & (ncap - 1u);
            nk[j] = m->keys[i];
            nv[j] = m->vals[i];
        }
        free(m->keys);
        free(m->vals);
        m->keys = nk;
        m->vals = nv;
        m->cap = ncap;
    }
    size_t mask = m->cap - 1u;
    size_t i = ((uintptr_t)key >> 4) & mask;
    while (m->keys[i]) i = (i + 1u) & mask;
    m->keys[i] = key;
    m->vals[i] = val;
    m->count++;
    return true;
}

static bool value_is_heap_obj(IdmValueTag tag) {
    switch (tag) {
        case IDM_VAL_STRING: case IDM_VAL_PAIR: case IDM_VAL_TUPLE: case IDM_VAL_VECTOR:
        case IDM_VAL_DICT: case IDM_VAL_SYNTAX: case IDM_VAL_CELL: case IDM_VAL_CLOSURE:
        case IDM_VAL_RECORD: case IDM_VAL_REGEX: case IDM_VAL_REGEX_RESULT:
        case IDM_VAL_FLOAT: case IDM_VAL_BIGNUM: case IDM_VAL_BITSTRING:
            return true;
        default:
            return false;
    }
}

typedef struct { IdmObject *src; IdmObject *dst; } CopyWork;
typedef struct { CopyWork *items; size_t count; size_t cap; } CopyStack;

static bool copystack_push(CopyStack *s, IdmObject *src, IdmObject *dst) {
    if (s->count == s->cap) {
        if (!idm_grow((void **)&s->items, &s->cap, sizeof(*s->items), 64u, s->count + 1u)) return false;
    }
    s->items[s->count].src = src;
    s->items[s->count].dst = dst;
    s->count++;
    return true;
}

static bool copy_intern(IdmRuntime *rt, IdmHeap *target, IdmValue in, IdmValue *out, CopyMap *map, CopyStack *stack, IdmError *err) {
    if (!value_is_heap_obj(idm_value_tag(in))) { *out = in; return true; }
    IdmObject *src = idm_boxed_object(in);
    if (!src || src->heap == &rt->immortal) { *out = in; return true; }
    bool identity = src->kind == IDM_OBJ_CELL;
    if (identity) {
        IdmObject *existing = copymap_find(map, src);
        if (existing) { *out = idm_from_boxed(existing); return true; }
    }
    size_t extra = 0;
    if (src->kind == IDM_OBJ_STRING) extra = src->as.string.len + 1u;
    else if (src->kind == IDM_OBJ_BIGNUM && !bignum_limb_bytes(src->as.bignum.count, &extra, err)) return false;
    else if ((src->kind == IDM_OBJ_TUPLE || src->kind == IDM_OBJ_VECTOR) && !size_mul(src->as.sequence.count, sizeof(IdmValue), &extra)) return false;
    else if (src->kind == IDM_OBJ_DICT_NODE && !size_mul(dict_node_slot_count(src), sizeof(IdmValue), &extra)) return false;
    else if (src->kind == IDM_OBJ_CLOSURE && !closure_payload_size(src->as.closure.entry_count, src->as.closure.capture_count, &extra)) return false;
    else if (src->kind == IDM_OBJ_RECORD) {
        size_t count = src->as.record.shape ? src->as.record.shape->field_count : 0u;
        if (!size_mul(count, sizeof(IdmValue), &extra)) return false;
    }
    IdmObject *dst = extra != 0u ? heap_alloc_payload_unlocked(target, src->kind, extra) : heap_alloc_extra_unlocked(target, src->kind, 0u);
    if (!dst) return idm_error_oom(err, idm_span_unknown(NULL));
    if (identity && !copymap_put(map, src, dst)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!copystack_push(stack, src, dst)) return idm_error_oom(err, idm_span_unknown(NULL));
    *out = idm_from_boxed(dst);
    return true;
}

static bool copy_fill(IdmRuntime *rt, IdmHeap *target, IdmObject *src, IdmObject *dst, CopyMap *map, CopyStack *stack, IdmError *err) {
    switch (src->kind) {
        case IDM_OBJ_STRING: {
            char *bytes = (char *)(dst + 1);
            memcpy(bytes, src->as.string.bytes, src->as.string.len);
            bytes[src->as.string.len] = '\0';
            dst->as.string.bytes = bytes;
            dst->as.string.len = src->as.string.len;
            return true;
        }
        case IDM_OBJ_ROPE:
            dst->as.rope.len = src->as.rope.len;
            dst->as.rope.newlines = src->as.rope.newlines;
            dst->as.rope.height = src->as.rope.height;
            atomic_init(&dst->as.rope.flat, NULL);
            dst->as.rope.left = idm_nil();
            dst->as.rope.right = idm_nil();
            if (!copy_intern(rt, target, src->as.rope.left, &dst->as.rope.left, map, stack, err)) return false;
            return copy_intern(rt, target, src->as.rope.right, &dst->as.rope.right, map, stack, err);
        case IDM_OBJ_PAIR:
            if (!copy_intern(rt, target, src->as.pair.car, &dst->as.pair.car, map, stack, err)) return false;
            return copy_intern(rt, target, src->as.pair.cdr, &dst->as.pair.cdr, map, stack, err);
        case IDM_OBJ_TUPLE:
        case IDM_OBJ_VECTOR: {
            size_t n = src->as.sequence.count;
            dst->as.sequence.count = n;
            dst->as.sequence.items = NULL;
            if (n != 0) {
                dst->as.sequence.items = (IdmValue *)(dst + 1);
                for (size_t i = 0; i < n; i++) dst->as.sequence.items[i] = idm_nil();
                for (size_t i = 0; i < n; i++)
                    if (!copy_intern(rt, target, src->as.sequence.items[i], &dst->as.sequence.items[i], map, stack, err)) return false;
            }
            return true;
        }
        case IDM_OBJ_DICT: {
            dst->as.dict.count = src->as.dict.count;
            dst->as.dict.root = idm_nil();
            if (src->as.dict.count != 0 && !copy_intern(rt, target, src->as.dict.root, &dst->as.dict.root, map, stack, err)) return false;
            return true;
        }
        case IDM_OBJ_DICT_NODE: {
            size_t n = dict_node_slot_count(src);
            dst->as.dict_node.datamap = src->as.dict_node.datamap;
            dst->as.dict_node.nodemap = src->as.dict_node.nodemap;
            dst->as.dict_node.collision_count = src->as.dict_node.collision_count;
            dst->as.dict_node.slots = NULL;
            if (n != 0) {
                dst->as.dict_node.slots = (IdmValue *)(dst + 1);
                for (size_t i = 0; i < n; i++) dst->as.dict_node.slots[i] = idm_nil();
                for (size_t i = 0; i < n; i++) {
                    if (!copy_intern(rt, target, src->as.dict_node.slots[i], &dst->as.dict_node.slots[i], map, stack, err)) return false;
                }
            }
            return true;
        }
        case IDM_OBJ_CELL:
            return copy_intern(rt, target, src->as.cell.value, &dst->as.cell.value, map, stack, err);
        case IDM_OBJ_CLOSURE: {
            dst->as.closure.module = src->as.closure.module;
            dst->as.closure.function_index = src->as.closure.function_index;
            dst->as.closure.env = src->as.closure.env;
            dst->as.closure.selector = src->as.closure.selector;
            idm_pattern_selector_retain(dst->as.closure.selector);
            dst->as.closure.selector_generation = src->as.closure.selector_generation;
            dst->as.closure.entry_count = src->as.closure.entry_count;
            dst->as.closure.entries = NULL;
            dst->as.closure.capture_count = src->as.closure.capture_count;
            dst->as.closure.captures = NULL;
            unsigned char *payload = (unsigned char *)(dst + 1);
            if (src->as.closure.capture_count != 0) {
                dst->as.closure.captures = (IdmValue *)payload;
                for (size_t i = 0; i < src->as.closure.capture_count; i++) dst->as.closure.captures[i] = idm_nil();
                payload += src->as.closure.capture_count * sizeof(IdmValue);
                for (size_t i = 0; i < src->as.closure.capture_count; i++)
                    if (!copy_intern(rt, target, src->as.closure.captures[i], &dst->as.closure.captures[i], map, stack, err)) return false;
            }
            if (src->as.closure.entry_count > 1) {
                dst->as.closure.entries = (uint32_t *)payload;
                memcpy(dst->as.closure.entries, src->as.closure.entries, src->as.closure.entry_count * sizeof(uint32_t));
            }
            return true;
        }
        case IDM_OBJ_RECORD:
            dst->as.record.shape = src->as.record.shape;
            if (src->as.record.shape->field_count != 0) {
                dst->as.record.field_values = (IdmValue *)(dst + 1);
                for (size_t i = 0; i < src->as.record.shape->field_count; i++) dst->as.record.field_values[i] = idm_nil();
                for (size_t i = 0; i < src->as.record.shape->field_count; i++) {
                    if (!copy_intern(rt, target, src->as.record.field_values[i], &dst->as.record.field_values[i], map, stack, err)) return false;
                }
            }
            return true;
        case IDM_OBJ_SYNTAX:
            dst->as.syntax = idm_syn_clone(src->as.syntax);
            if (!dst->as.syntax && src->as.syntax) return idm_error_oom(err, idm_span_unknown(NULL));
            heap_account_unlocked(target, dst, syn_footprint(dst->as.syntax));
            return true;
        case IDM_OBJ_REGEX:
            dst->as.regex = idm_regex_clone(src->as.regex, err);
            if (err && err->present) return false;
            heap_account_unlocked(target, dst, idm_regex_footprint(dst->as.regex));
            return true;
        case IDM_OBJ_REGEX_RESULT: {
            IdmValue subject = idm_nil();
            if (!copy_intern(rt, target, idm_regex_result_subject_value(src->as.regex_result), &subject, map, stack, err)) return false;
            dst->as.regex_result = idm_regex_result_clone_with_subject(src->as.regex_result, subject, err);
            if (!dst->as.regex_result && src->as.regex_result) return false;
            heap_account_unlocked(target, dst, idm_regex_result_footprint(dst->as.regex_result));
            return true;
        }
        case IDM_OBJ_FLONUM:
            dst->as.flonum = src->as.flonum;
            return true;
        case IDM_OBJ_BIGNUM: {
            size_t bytes = 0;
            if (!bignum_limb_bytes(src->as.bignum.count, &bytes, err)) return false;
            dst->as.bignum.limbs = bytes == 0u ? NULL : (uint32_t *)(dst + 1);
            if (bytes != 0u) memcpy(dst->as.bignum.limbs, src->as.bignum.limbs, bytes);
            dst->as.bignum.count = src->as.bignum.count;
            dst->as.bignum.sign = src->as.bignum.sign;
            return true;
        }
        case IDM_OBJ_BITSTRING: {
            uint64_t len = src->as.bits.bit_len;
            size_t nbytes = 0;
            if (!bits_byte_count(len, &nbytes, err)) return false;
            dst->as.bits.parent = idm_nil();
            dst->as.bits.bit_off = 0;
            dst->as.bits.bit_len = len;
            dst->as.bits.bit_cap = len;
            dst->as.bits.bit_used = len;
            dst->as.bits.bytes = NULL;
            if (nbytes != 0u) {
                dst->as.bits.bytes = calloc(nbytes, 1u);
                if (!dst->as.bits.bytes) return idm_error_oom(err, idm_span_unknown(NULL));
                bits_blit(dst->as.bits.bytes, 0u, src->as.bits.bytes, src->as.bits.bit_off, len);
                heap_account_unlocked(target, dst, nbytes);
            }
            return true;
        }
        case IDM_OBJ_COUNT:
            break;
    }
    return idm_error_set(err, idm_span_unknown(NULL), "value copy: unknown object kind");
}

IdmValue idm_value_copy_locked(IdmRuntime *rt, IdmHeap *target, IdmValue value, IdmError *err) {
    if (!value_is_heap_obj(idm_value_tag(value)) || !idm_boxed_object(value) || idm_boxed_object(value)->heap == &rt->immortal) return value;
    CopyMap map = {0};
    CopyStack stack = {0};
    IdmValue out = idm_nil();
    bool ok = copy_intern(rt, target, value, &out, &map, &stack, err);
    while (ok && stack.count != 0) {
        CopyWork w = stack.items[--stack.count];
        ok = copy_fill(rt, target, w.src, w.dst, &map, &stack, err);
    }
    free(map.keys);
    free(map.vals);
    free(stack.items);
    return out;
}

IdmValue idm_value_copy(IdmRuntime *rt, IdmHeap *target, IdmValue value, IdmError *err) {
    heap_lock(target);
    IdmValue out = idm_value_copy_locked(rt, target, value, err);
    heap_unlock(target);
    return out;
}

IdmHeap *idm_value_heap(IdmValue value) {
    IdmObject *obj = value_object(value);
    return obj ? obj->heap : NULL;
}

IdmValue idm_error_value(IdmRuntime *rt, IdmValue detail) {
    IdmValue items[2];
    items[0] = idm_atom(rt, "error");
    items[1] = detail;
    return idm_tuple(rt, items, 2u, NULL);
}

bool idm_error_reason(IdmRuntime *rt, IdmError *err, const char *kind, size_t count, ...) {
    if (!err) return false;
    IdmValue items[5];
    if (count > 4u) count = 4u;
    items[0] = idm_atom(rt, kind);
    va_list ap;
    va_start(ap, count);
    for (size_t i = 0; i < count; i++) items[i + 1u] = va_arg(ap, IdmValue);
    va_end(ap);
    IdmValue detail = idm_tuple(rt, items, count + 1u, NULL);
    if (idm_value_tag(detail) != IDM_VAL_TUPLE) return false;
    IdmValue *slot = err->reason ? err->reason : malloc(sizeof(*slot));
    if (!slot) return false;
    *slot = detail;
    err->reason = slot;
    return false;
}

bool idm_error_reason_is(const IdmError *err, const char *kind) {
    if (!err || !err->reason || !kind) return false;
    IdmValue detail = *(const IdmValue *)err->reason;
    if (!idm_is_tuple(detail) || idm_sequence_count(detail) == 0u) return false;
    IdmValue head = idm_sequence_item(detail, 0u, NULL);
    return idm_value_tag(head) == IDM_VAL_ATOM && strcmp(idm_symbol_text(idm_value_symbol(head)), kind) == 0;
}

bool idm_error_take_reason(IdmError *err, IdmValue *out) {
    if (!err || !err->reason) return false;
    *out = *(IdmValue *)err->reason;
    free(err->reason);
    err->reason = NULL;
    return true;
}

IdmValue idm_span_value(IdmRuntime *rt, IdmSpan span) {
    IdmValue items[6];
    items[0] = idm_atom(rt, "span");
    items[1] = span.file ? idm_string(rt, span.file, NULL) : idm_nil();
    items[2] = idm_int((int64_t)span.line);
    items[3] = idm_int((int64_t)span.column);
    items[4] = idm_int((int64_t)span.start);
    items[5] = idm_int((int64_t)span.end);
    return idm_tuple(rt, items, 6u, NULL);
}

static IdmValue error_trace_value(IdmRuntime *rt, const IdmErrorNote *notes, size_t count) {
    IdmValue list = idm_nil();
    for (size_t i = count; i > 0u; i--) {
        const IdmErrorNote *n = &notes[i - 1u];
        IdmValue frame[3];
        frame[0] = idm_atom(rt, "frame");
        frame[1] = idm_string(rt, n->message ? n->message : "", NULL);
        frame[2] = idm_span_value(rt, n->span);
        IdmValue fv = idm_tuple(rt, frame, 3u, NULL);
        list = idm_cons(rt, fv, list, NULL);
    }
    return list;
}

IdmValue idm_error_reason_value(IdmRuntime *rt, IdmError *err) {
    IdmValue detail;
    if (!idm_error_take_reason(err, &detail)) {
        IdmValue inner[2];
        inner[0] = idm_atom(rt, "runtime");
        inner[1] = idm_string(rt, (err && err->present && err->message) ? err->message : "error", NULL);
        detail = idm_tuple(rt, inner, 2u, NULL);
    }
    if (err && err->note_count != 0u) {
        IdmValue items[3];
        items[0] = idm_atom(rt, "error");
        items[1] = detail;
        items[2] = error_trace_value(rt, err->notes, err->note_count);
        return idm_tuple(rt, items, 3u, NULL);
    }
    return idm_error_value(rt, detail);
}

static void describe_field(IdmBuffer *out, IdmValue v) {
    if (idm_value_tag(v) == IDM_VAL_ATOM) idm_buf_append(out, idm_symbol_text(idm_value_symbol(v)));
    else idm_value_write(out, v);
}

static void describe_tail(IdmBuffer *out, IdmValue tuple, size_t from) {
    size_t n = idm_is_tuple(tuple) ? idm_sequence_count(tuple) : 0u;
    for (size_t i = from; i < n; i++) {
        if (i > from) idm_buf_append(out, " ");
        idm_value_write(out, idm_sequence_item(tuple, i, NULL));
    }
}

static void error_describe_detail(IdmBuffer *out, IdmValue detail) {
    if (idm_value_tag(detail) == IDM_VAL_STRING) { idm_buf_append(out, idm_string_bytes(detail)); return; }
    if (!idm_is_tuple(detail) || idm_sequence_count(detail) == 0 ||
        idm_value_tag(idm_sequence_item(detail, 0, NULL)) != IDM_VAL_ATOM) {
        idm_buf_append(out, "error: ");
        idm_value_write(out, detail);
        return;
    }
    const char *kind = idm_symbol_text(idm_value_symbol(idm_sequence_item(detail, 0, NULL)));
    size_t n = idm_sequence_count(detail);
    if (strcmp(kind, "runtime") == 0 && n >= 2 && idm_value_tag(idm_sequence_item(detail, 1, NULL)) == IDM_VAL_STRING) {
        idm_buf_append(out, idm_string_bytes(idm_sequence_item(detail, 1, NULL)));
        return;
    }
    if (strcmp(kind, "no-clause") == 0 && n >= 3) {
        IdmValue nm = idm_sequence_item(detail, 1, NULL);
        bool anon = idm_value_tag(nm) == IDM_VAL_ATOM && strcmp(idm_symbol_text(idm_value_symbol(nm)), "fn") == 0;
        idm_buf_append(out, "no clause of '");
        if (anon) idm_buf_append(out, "<fn>"); else describe_field(out, nm);
        idm_buf_append(out, "' matches (");
        describe_tail(out, idm_sequence_item(detail, 2, NULL), 0);
        idm_buf_append(out, ")");
        return;
    }
    if (strcmp(kind, "arity") == 0 && n >= 4) {
        idm_buf_append(out, "'");
        describe_field(out, idm_sequence_item(detail, 1, NULL));
        idm_buf_append(out, "' expects ");
        idm_value_write(out, idm_sequence_item(detail, 3, NULL));
        idm_buf_append(out, " argument(s), got ");
        idm_value_write(out, idm_sequence_item(detail, 2, NULL));
        return;
    }
    if (strcmp(kind, "type-error") == 0 && n >= 2) {
        idm_buf_append(out, "type error in '");
        describe_field(out, idm_sequence_item(detail, 1, NULL));
        idm_buf_append(out, "'");
        if (n > 2) { idm_buf_append(out, ": "); describe_tail(out, detail, 2); }
        return;
    }
    if (strcmp(kind, "div-by-zero") == 0) { idm_buf_append(out, "division by zero"); return; }
    if (strcmp(kind, "overflow") == 0) {
        idm_buf_append(out, "integer overflow");
        if (n > 1) { idm_buf_append(out, " in '"); describe_field(out, idm_sequence_item(detail, 1, NULL)); idm_buf_append(out, "'"); }
        return;
    }
    if (strcmp(kind, "not-callable") == 0 && n >= 2) {
        idm_buf_append(out, "value is not callable: ");
        idm_value_write(out, idm_sequence_item(detail, 1, NULL));
        return;
    }
    if (strcmp(kind, "key-not-found") == 0 && n >= 2) {
        idm_buf_append(out, "key not found: ");
        idm_value_write(out, idm_sequence_item(detail, 1, NULL));
        return;
    }
    if ((strcmp(kind, "index-out-of-range") == 0 || strcmp(kind, "slice-out-of-range") == 0) && n >= 2) {
        idm_buf_append(out, kind[0] == 'i' ? "index out of range: " : "slice out of range: ");
        describe_tail(out, detail, 1);
        return;
    }
    idm_buf_append(out, kind);
    if (n > 1) { idm_buf_append(out, ": "); describe_tail(out, detail, 1); }
}

static void error_describe_trace(IdmBuffer *out, IdmValue trace) {
    IdmValue node = trace;
    while (idm_value_tag(node) == IDM_VAL_PAIR) {
        IdmValue frame = idm_boxed_object(node)->as.pair.car;
        node = idm_boxed_object(node)->as.pair.cdr;
        if (!idm_is_tuple(frame) || idm_sequence_count(frame) < 3u) continue;
        IdmValue msg = idm_sequence_item(frame, 1, NULL);
        IdmValue span = idm_sequence_item(frame, 2, NULL);
        idm_buf_append(out, "\n  ");
        if (idm_value_tag(msg) == IDM_VAL_STRING) idm_buf_append(out, idm_string_bytes(msg));
        IdmValue file = idm_is_tuple(span) && idm_sequence_count(span) >= 3u ? idm_sequence_item(span, 1, NULL) : idm_nil();
        int64_t line = 0;
        if (idm_is_tuple(span) && idm_sequence_count(span) >= 3u) idm_int_to_i64(idm_sequence_item(span, 2, NULL), &line);
        if (idm_value_tag(file) == IDM_VAL_STRING && line != 0) {
            int64_t col = 0;
            idm_int_to_i64(idm_sequence_item(span, 3, NULL), &col);
            idm_buf_appendf(out, " (%s:%lld:%lld)", idm_string_bytes(file), (long long)line, (long long)col);
        }
    }
}

void idm_error_describe(IdmRuntime *rt, IdmValue reason, IdmBuffer *out) {
    (void)rt;
    if (!idm_value_is_error(reason)) { idm_value_write(out, reason); return; }
    error_describe_detail(out, idm_sequence_item(reason, 1, NULL));
    if (idm_sequence_count(reason) >= 3u) error_describe_trace(out, idm_sequence_item(reason, 2, NULL));
}

bool idm_value_matches_type_term(IdmValue value, const IdmTypeTerm *term) {
    if (!term) return false;
    switch (term->kind) {
        case IDM_TYPE_VAR: return true;
        case IDM_TYPE_UNION:
            for (size_t i = 0; i < term->arg_count; i++)
                if (idm_value_matches_type_term(value, &term->args[i])) return true;
            return false;
        case IDM_TYPE_TUPLE: return idm_value_tag(value) == IDM_VAL_TUPLE;
        case IDM_TYPE_VECTOR: return idm_value_tag(value) == IDM_VAL_VECTOR;
        case IDM_TYPE_CON: return idm_value_matches_type_name(value, term->symbol, NULL, true);
    }
    return false;
}
