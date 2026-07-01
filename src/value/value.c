#include "idiom/value.h"

#include "idiom/bignum.h"
#include "idiom/bytecode.h"
#include "idiom/pattern.h"
#include "idiom/regex.h"
#include "idiom/syntax.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    IDM_OBJ_STRING,
    IDM_OBJ_PAIR,
    IDM_OBJ_TUPLE,
    IDM_OBJ_VECTOR,
    IDM_OBJ_DICT,
    IDM_OBJ_SYNTAX,
    IDM_OBJ_CELL,
    IDM_OBJ_CLOSURE,
    IDM_OBJ_RECORD,
    IDM_OBJ_REGEX,
    IDM_OBJ_REGEX_RESULT,
    IDM_OBJ_FLONUM,
    IDM_OBJ_BIGNUM
} IdmObjectKind;

typedef struct {
    char *bytes;
    size_t len;
} IdmStringObj;

typedef struct {
    IdmValue car;
    IdmValue cdr;
} IdmPairObj;

typedef struct {
    IdmValue *items;
    size_t count;
} IdmSequenceObj;

typedef struct {
    IdmDictEntry *entries;
    size_t count;
} IdmDictObj;

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
};

typedef struct {
    IdmValue value;
} IdmCellObj;

typedef struct {
    uint32_t *limbs;
    uint32_t count;
    int32_t sign;
} IdmBignumObj;

struct IdmObject {
    IdmObjectKind kind;
    unsigned mark;
    bool greyed;
    IdmHeap *heap;
    size_t bytes;
    struct IdmObject *next;
    struct IdmObject *grey_next;
    size_t scan;
    union {
        IdmStringObj string;
        IdmPairObj pair;
        IdmSequenceObj sequence;
        IdmDictObj dict;
        IdmSyntax *syntax;
        IdmCellObj cell;
        IdmClosureObj closure;
        IdmRecordObj record;
        IdmRegex *regex;
        IdmRegexResult *regex_result;
        double flonum;
        IdmBignumObj bignum;
    } as;
};

struct IdmSymbol {
    char *text;
    uint32_t id;
    IdmSymbolKind kind;
    IdmBuiltinType builtin_type;
    uint32_t hash;
    struct IdmSymbol *hnext;
};

static pthread_mutex_t g_intern_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_env_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_record_shape_mu = PTHREAD_MUTEX_INITIALIZER;
atomic_uint idm_gc_marking_heap_count = 0;

static IdmSymbol *idm_intern_unlocked(IdmIntern *intern, IdmSymbolKind kind, const char *text);
static IdmSymbol *intern_find_unlocked(IdmIntern *intern, IdmSymbolKind kind, const char *text, uint32_t h);
static uint32_t intern_hash(IdmSymbolKind kind, const char *text);
static IdmEnv *env_get_or_create_unlocked(IdmRuntime *rt, const char *package_key);

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

static IdmObject *heap_alloc_extra_unlocked(IdmHeap *heap, IdmObjectKind kind, size_t extra) {
    if (extra > SIZE_MAX - sizeof(IdmObject)) return NULL;
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
    if (obj->kind == IDM_OBJ_STRING) free(obj->as.string.bytes);
    if (obj->kind == IDM_OBJ_TUPLE || obj->kind == IDM_OBJ_VECTOR) free(obj->as.sequence.items);
    if (obj->kind == IDM_OBJ_DICT && object_payload_external(obj, obj->as.dict.entries)) free(obj->as.dict.entries);
    if (obj->kind == IDM_OBJ_SYNTAX) idm_syn_free(obj->as.syntax);
    if (obj->kind == IDM_OBJ_CLOSURE) {
        idm_pattern_selector_free(obj->as.closure.selector);
        free(obj->as.closure.entries);
        free(obj->as.closure.captures);
    }
    if (obj->kind == IDM_OBJ_RECORD) {
        free(obj->as.record.field_values);
    }
    if (obj->kind == IDM_OBJ_REGEX) idm_regex_free(obj->as.regex);
    if (obj->kind == IDM_OBJ_REGEX_RESULT) idm_regex_result_free(obj->as.regex_result);
    if (obj->kind == IDM_OBJ_BIGNUM) free(obj->as.bignum.limbs);
    free(obj);
}

static void record_shape_destroy(IdmRecordShape *shape) {
    if (!shape) return;
    free(shape->field_names);
    free(shape);
}

static const char *record_shape_type_text(const IdmRecordShape *shape) {
    return idm_symbol_text(shape->type);
}

static const char *record_shape_field_text(const IdmRecordShape *shape, size_t index) {
    return idm_symbol_text(shape->field_names[index]);
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

void idm_runtime_init(IdmRuntime *rt) {
    idm_intern_init(&rt->intern);
    rt->cached_true = idm_atom(rt, "true");
    rt->cached_false = idm_atom(rt, "false");
    idm_heap_init(&rt->immortal);
    rt->macro_intro_active = false;
    rt->macro_intro_scope = 0;
    rt->local_expand_user = NULL;
    rt->local_expand = NULL;
    rt->free_identifier_eq_user = NULL;
    rt->free_identifier_eq = NULL;
    rt->register_macro_user = NULL;
    rt->register_macro = NULL;
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
}

void idm_runtime_destroy(IdmRuntime *rt) {
    for (size_t i = 0; i < rt->env_count; i++) {
        free(rt->envs[i]->package_key);
        free(rt->envs[i]->slots);
        free(rt->envs[i]);
    }
    free(rt->envs);
    rt->envs = NULL;
    rt->env_count = 0;
    rt->env_cap = 0;
    rt->main_env = NULL;
    if (rt->expand_cache && rt->expand_cache_free) rt->expand_cache_free(rt->expand_cache);
    rt->expand_cache = NULL;
    rt->expand_cache_free = NULL;
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

IdmEnv *idm_package_env_get_or_create(IdmRuntime *rt, const char *key) {
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

static IdmEnv *env_get_or_create_unlocked(IdmRuntime *rt, const char *package_key) {
    if (package_key) {
        for (size_t i = 0; i < rt->env_count; i++) {
            if (rt->envs[i]->package_key && strcmp(rt->envs[i]->package_key, package_key) == 0) return rt->envs[i];
        }
    }
    IdmEnv *env = calloc(1u, sizeof(*env));
    if (!env) return NULL;
    if (package_key) {
        env->package_key = idm_strdup(package_key);
        if (!env->package_key) { free(env); return NULL; }
    }
    if (rt->env_count == rt->env_cap) {
        if (!idm_grow((void **)&rt->envs, &rt->env_cap, sizeof(*rt->envs), 8u, rt->env_count + 1u)) { free(env->package_key); free(env); return NULL; }
    }
    rt->envs[rt->env_count++] = env;
    return env;
}

bool idm_env_slot_ensure(IdmEnv *env, uint32_t id, IdmError *err) {
    pthread_mutex_lock(&g_env_mu);
    size_t needed = (size_t)id + 1u;
    if (needed <= env->slot_count) {
        pthread_mutex_unlock(&g_env_mu);
        return true;
    }
    if (needed > env->slot_cap) {
        if (!idm_grow((void **)&env->slots, &env->slot_cap, sizeof(*env->slots), 16u, needed)) {
            pthread_mutex_unlock(&g_env_mu);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    for (size_t i = env->slot_count; i < needed; i++) env->slots[i] = idm_nil();
    env->slot_count = needed;
    pthread_mutex_unlock(&g_env_mu);
    return true;
}

bool idm_env_slot_set(IdmRuntime *rt, IdmEnv *env, uint32_t id, IdmValue value, IdmError *err) {
    value = idm_value_copy(rt, &rt->immortal, value, err);
    if (err && err->present) return false;
    pthread_mutex_lock(&g_env_mu);
    if ((size_t)id < env->slot_count) env->slots[id] = value;
    pthread_mutex_unlock(&g_env_mu);
    return true;
}

IdmValue idm_env_slot_get(const IdmEnv *env, uint32_t id) {
    pthread_mutex_lock(&g_env_mu);
    IdmValue out = (size_t)id < env->slot_count ? env->slots[id] : idm_nil();
    pthread_mutex_unlock(&g_env_mu);
    return out;
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
    pthread_mutex_lock(&g_intern_mu);
    IdmSymbol *sym = idm_intern_unlocked(intern, kind, text);
    pthread_mutex_unlock(&g_intern_mu);
    return sym;
}

IdmSymbol *idm_intern_lookup(IdmIntern *intern, IdmSymbolKind kind, const char *text) {
    if (!intern || !text) return NULL;
    uint32_t h = intern_hash(kind, text);
    pthread_mutex_lock(&g_intern_mu);
    IdmSymbol *sym = intern_find_unlocked(intern, kind, text, h);
    pthread_mutex_unlock(&g_intern_mu);
    return sym;
}

static uint32_t intern_hash(IdmSymbolKind kind, const char *text) {
    uint32_t h = 2166136261u;
    h ^= (uint32_t)kind;
    h *= 16777619u;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static IdmBuiltinType builtin_type_from_text(const char *text) {
    if (strcmp(text, "nil") == 0) return IDM_BUILTIN_TYPE_NIL;
    if (strcmp(text, "atom") == 0) return IDM_BUILTIN_TYPE_ATOM;
    if (strcmp(text, "word") == 0) return IDM_BUILTIN_TYPE_WORD;
    if (strcmp(text, "int") == 0) return IDM_BUILTIN_TYPE_INT;
    if (strcmp(text, "fixnum") == 0) return IDM_BUILTIN_TYPE_FIXNUM;
    if (strcmp(text, "bignum") == 0) return IDM_BUILTIN_TYPE_BIGNUM;
    if (strcmp(text, "float") == 0) return IDM_BUILTIN_TYPE_FLOAT;
    if (strcmp(text, "string") == 0) return IDM_BUILTIN_TYPE_STRING;
    if (strcmp(text, "pair") == 0) return IDM_BUILTIN_TYPE_PAIR;
    if (strcmp(text, "empty-list") == 0) return IDM_BUILTIN_TYPE_EMPTY_LIST;
    if (strcmp(text, "list") == 0) return IDM_BUILTIN_TYPE_LIST;
    if (strcmp(text, "tuple") == 0) return IDM_BUILTIN_TYPE_TUPLE;
    if (strcmp(text, "vector") == 0) return IDM_BUILTIN_TYPE_VECTOR;
    if (strcmp(text, "dict") == 0) return IDM_BUILTIN_TYPE_DICT;
    if (strcmp(text, "syntax") == 0) return IDM_BUILTIN_TYPE_SYNTAX;
    if (strcmp(text, "cell") == 0) return IDM_BUILTIN_TYPE_CELL;
    if (strcmp(text, "closure") == 0) return IDM_BUILTIN_TYPE_CLOSURE;
    if (strcmp(text, "pid") == 0) return IDM_BUILTIN_TYPE_PID;
    if (strcmp(text, "ref") == 0) return IDM_BUILTIN_TYPE_REF;
    if (strcmp(text, "port") == 0) return IDM_BUILTIN_TYPE_PORT;
    if (strcmp(text, "record") == 0) return IDM_BUILTIN_TYPE_RECORD;
    if (strcmp(text, "regex") == 0) return IDM_BUILTIN_TYPE_REGEX;
    if (strcmp(text, "regex-result") == 0) return IDM_BUILTIN_TYPE_REGEX_RESULT;
    return IDM_BUILTIN_TYPE_NONE;
}

bool idm_type_name_is_builtin(const char *text) {
    return text && builtin_type_from_text(text) != IDM_BUILTIN_TYPE_NONE;
}

size_t idm_builtin_overtype_members(const char *parent, const char *const **out_names) {
    static const char *const list_members[] = { "empty-list", "pair" };
    static const char *const int_members[] = { "fixnum", "bignum" };
    *out_names = NULL;
    if (!parent) return 0;
    switch (builtin_type_from_text(parent)) {
        case IDM_BUILTIN_TYPE_LIST:
            *out_names = list_members;
            return 2;
        case IDM_BUILTIN_TYPE_INT:
            *out_names = int_members;
            return 2;
        default:
            return 0;
    }
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

static IdmSymbol *idm_intern_unlocked(IdmIntern *intern, IdmSymbolKind kind, const char *text) {
    uint32_t h = intern_hash(kind, text);
    IdmSymbol *found = intern_find_unlocked(intern, kind, text, h);
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
    sym->builtin_type = builtin_type_from_text(text);
    sym->hash = h;
    intern->symbols[intern->count++] = sym;
    size_t b = h & (intern->bucket_count - 1u);
    sym->hnext = intern->buckets[b];
    intern->buckets[b] = sym;
    return sym;
}

static IdmSymbol *intern_find_unlocked(IdmIntern *intern, IdmSymbolKind kind, const char *text, uint32_t h) {
    if (!intern->bucket_count) return NULL;
    for (IdmSymbol *s = intern->buckets[h & (intern->bucket_count - 1u)]; s; s = s->hnext) {
        if (s->hash == h && s->kind == kind && strcmp(s->text, text) == 0) return s;
    }
    return NULL;
}

const char *idm_symbol_text(const IdmSymbol *sym) {
    return sym ? sym->text : "";
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
        case IDM_OBJ_PAIR: return IDM_VAL_PAIR;
        case IDM_OBJ_TUPLE: return IDM_VAL_TUPLE;
        case IDM_OBJ_VECTOR: return IDM_VAL_VECTOR;
        case IDM_OBJ_DICT: return IDM_VAL_DICT;
        case IDM_OBJ_SYNTAX: return IDM_VAL_SYNTAX;
        case IDM_OBJ_CELL: return IDM_VAL_CELL;
        case IDM_OBJ_CLOSURE: return IDM_VAL_CLOSURE;
        case IDM_OBJ_RECORD: return IDM_VAL_RECORD;
        case IDM_OBJ_REGEX: return IDM_VAL_REGEX;
        case IDM_OBJ_REGEX_RESULT: return IDM_VAL_REGEX_RESULT;
        case IDM_OBJ_FLONUM: return IDM_VAL_FLOAT;
        case IDM_OBJ_BIGNUM: return IDM_VAL_BIGNUM;
    }
    return IDM_VAL_NIL;
}

int64_t idm_int_value(IdmValue value) {
    return idm_fixnum_value(value);
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
            return 2u;
        case IDM_OBJ_TUPLE:
        case IDM_OBJ_VECTOR:
            return obj->as.sequence.count;
        case IDM_OBJ_DICT:
            return obj->as.dict.count > SIZE_MAX / 2u ? SIZE_MAX : obj->as.dict.count * 2u;
        case IDM_OBJ_CELL:
            return 1u;
        case IDM_OBJ_CLOSURE:
            return obj->as.closure.capture_count;
        case IDM_OBJ_RECORD:
            return obj->as.record.shape ? obj->as.record.shape->field_count : 0u;
        case IDM_OBJ_REGEX_RESULT:
            return 1u;
        default:
            return 0;
    }
}

static IdmValue gc_object_child_at(const IdmObject *obj, size_t index) {
    if (obj->kind == IDM_OBJ_PAIR) return index == 0 ? obj->as.pair.car : obj->as.pair.cdr;
    if (obj->kind == IDM_OBJ_TUPLE || obj->kind == IDM_OBJ_VECTOR) return obj->as.sequence.items[index];
    if (obj->kind == IDM_OBJ_DICT) {
        size_t entry = index / 2u;
        return (index & 1u) == 0 ? obj->as.dict.entries[entry].key : obj->as.dict.entries[entry].value;
    }
    if (obj->kind == IDM_OBJ_CELL) return obj->as.cell.value;
    if (obj->kind == IDM_OBJ_CLOSURE) return obj->as.closure.captures[index];
    if (obj->kind == IDM_OBJ_RECORD) return obj->as.record.field_values[index];
    if (obj->kind == IDM_OBJ_REGEX_RESULT) return idm_regex_result_subject_value(obj->as.regex_result);
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

IdmValue idm_nil(void) {
    return idm_immediate(IDM_IMM_EMPTY, 0);
}

IdmValue idm_empty_list(void) {
    return idm_nil();
}

IdmValue idm_int(int64_t value) {
    return idm_fixnum(value);
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
    uint32_t *copy = bytes == 0u ? NULL : malloc(bytes);
    if (bytes != 0u && !copy) { idm_error_oom(err, idm_span_unknown(NULL)); return idm_nil(); }
    if (bytes != 0u) memcpy(copy, limbs, bytes);
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_BIGNUM);
    if (!obj) { free(copy); idm_error_oom(err, idm_span_unknown(NULL)); return idm_nil(); }
    obj->as.bignum.limbs = copy;
    obj->as.bignum.count = (uint32_t)count;
    obj->as.bignum.sign = (int32_t)sign;
    heap_account(idm_active_heap(rt), obj, bytes);
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
        return idm_error_set(err, idm_span_unknown(NULL), "division by zero");
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

IdmValue idm_bool(IdmRuntime *rt, bool value) {
    return value ? rt->cached_true : rt->cached_false;
}

IdmValue idm_string_n(IdmRuntime *rt, const char *text, size_t len, IdmError *err) {
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_STRING);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.string.bytes = idm_strndup(text, len);
    if (!obj->as.string.bytes) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.string.len = len;
    heap_account(idm_active_heap(rt), obj, len + 1u);
    IdmValue v;
    v = idm_from_boxed(obj);
    return v;
}

IdmValue idm_string(IdmRuntime *rt, const char *text, IdmError *err) {
    return idm_string_n(rt, text, strlen(text), err);
}

static bool value_append_display(IdmBuffer *buf, IdmValue v) {
    if (idm_value_tag(v) == IDM_VAL_STRING) return idm_buf_append_n(buf, idm_string_bytes(v), idm_string_length(v));
    if (idm_value_tag(v) == IDM_VAL_ATOM || idm_value_tag(v) == IDM_VAL_WORD) return idm_buf_append(buf, idm_symbol_text(idm_value_symbol(v)));
    if (idm_value_tag(v) == IDM_VAL_NIL) return true;
    return idm_value_write(buf, v);
}

bool idm_string_concat_display(IdmRuntime *rt, const IdmValue *items, size_t count, IdmValue *out, IdmError *err) {
    if (count == 1u && idm_value_tag(items[0]) == IDM_VAL_STRING) { *out = items[0]; return true; }
    IdmBuffer buf;
    idm_buf_init(&buf);
    for (size_t i = 0; i < count; i++) {
        if (!value_append_display(&buf, items[i])) {
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
    if (!idm_is_pair(head)) return idm_error_set(err, idm_span_unknown(NULL), "append expects a list as first argument");
    size_t count = 0;
    IdmValue cur = head;
    while (idm_is_pair(cur)) {
        count++;
        cur = idm_cdr(cur, err);
        if (err && err->present) return false;
    }
    if (!idm_is_empty_list(cur)) return idm_error_set(err, idm_span_unknown(NULL), "append expects a proper list as first argument");
    IdmValue *items = count == 0 ? NULL : calloc(count, sizeof(*items));
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
    IdmObject *obj = heap_alloc(idm_active_heap(rt), obj_kind);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.sequence.count = count;
    obj->as.sequence.items = NULL;
    if (count != 0) {
        obj->as.sequence.items = malloc(count * sizeof(*obj->as.sequence.items));
        if (!obj->as.sequence.items) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return idm_nil();
        }
        memcpy(obj->as.sequence.items, items, count * sizeof(*items));
        heap_account(idm_active_heap(rt), obj, count * sizeof(*items));
    }
    return idm_from_boxed(obj);
}

IdmValue idm_tuple(IdmRuntime *rt, const IdmValue *items, size_t count, IdmError *err) {
    return sequence_value(rt, items, count, IDM_OBJ_TUPLE, err);
}

IdmValue idm_vector(IdmRuntime *rt, const IdmValue *items, size_t count, IdmError *err) {
    return sequence_value(rt, items, count, IDM_OBJ_VECTOR, err);
}

static IdmValue dict_value_with_cap(IdmRuntime *rt, size_t cap, IdmError *err) {
    size_t extra = 0;
    if (cap > SIZE_MAX / sizeof(IdmDictEntry)) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    extra = cap * sizeof(IdmDictEntry);
    IdmObject *obj = heap_alloc_extra(idm_active_heap(rt), IDM_OBJ_DICT, extra);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.dict.count = 0;
    obj->as.dict.entries = cap == 0 ? NULL : (IdmDictEntry *)(obj + 1);
    IdmValue v;
    v = idm_from_boxed(obj);
    return v;
}

static bool dict_key_equal(IdmValue a, IdmValue b) {
    IdmValueTag ta = idm_value_tag(a), tb = idm_value_tag(b);
    if (idm_value_is_int(a) && idm_value_is_int(b)) return idm_int_compare(a, b) == 0;
    if (ta != tb) return false;
    switch (ta) {
        case IDM_VAL_NIL:
            return true;
        case IDM_VAL_ATOM:
        case IDM_VAL_WORD:
            return idm_value_symbol(a) == idm_value_symbol(b);
        case IDM_VAL_INT:
            return idm_int_value(a) == idm_int_value(b);
        case IDM_VAL_FLOAT:
            return idm_float_value(a) == idm_float_value(b);
        case IDM_VAL_STRING:
            return idm_boxed_object(a)->as.string.len == idm_boxed_object(b)->as.string.len &&
                   memcmp(idm_boxed_object(a)->as.string.bytes, idm_boxed_object(b)->as.string.bytes, idm_boxed_object(a)->as.string.len) == 0;
        case IDM_VAL_SYNTAX:
        case IDM_VAL_CELL:
        case IDM_VAL_CLOSURE:
        case IDM_VAL_REGEX:
        case IDM_VAL_REGEX_RESULT:
            return idm_boxed_object(a) == idm_boxed_object(b);
        case IDM_VAL_PID:
        case IDM_VAL_REF:
        case IDM_VAL_PORT:
            return idm_value_id(a) == idm_value_id(b);
        default:
            return idm_value_equal(a, b);
    }
}

IdmValue idm_dict(IdmRuntime *rt, const IdmDictEntry *entries, size_t count, IdmError *err) {
    IdmValue v = dict_value_with_cap(rt, count, err);
    if (idm_value_tag(v) != IDM_VAL_DICT) return v;
    IdmObject *obj = idm_boxed_object(v);
    if (count != 0) {
        for (size_t i = 0; i < count; i++) {
            bool replaced = false;
            for (size_t j = 0; j < obj->as.dict.count; j++) {
                if (dict_key_equal(obj->as.dict.entries[j].key, entries[i].key)) {
                    obj->as.dict.entries[j].value = entries[i].value;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                obj->as.dict.entries[obj->as.dict.count++] = entries[i];
            }
        }
    }
    return v;
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

IdmValue idm_closure_multi_selectable_in_module(IdmRuntime *rt, const IdmBytecodeModule *module, const uint32_t *function_indexes, size_t function_count, IdmPatternSelector *selector, const IdmValue *captures, size_t capture_count, IdmEnv *env, IdmError *err) {
    if (function_count == 0) {
        idm_error_set(err, idm_span_unknown(NULL), "closure must have at least one function entry");
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
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_CLOSURE);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.closure.module = module;
    obj->as.closure.env = env;
    obj->as.closure.function_index = function_indexes[0];
    obj->as.closure.entry_count = function_count;
    obj->as.closure.entries = NULL;
    if (function_count > 1) {
        obj->as.closure.entries = malloc(function_count * sizeof(*obj->as.closure.entries));
        if (!obj->as.closure.entries) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return idm_nil();
        }
        memcpy(obj->as.closure.entries, function_indexes, function_count * sizeof(*function_indexes));
        heap_account(idm_active_heap(rt), obj, function_count * sizeof(*function_indexes));
    }
    obj->as.closure.capture_count = capture_count;
    obj->as.closure.captures = NULL;
    if (capture_count != 0) {
        obj->as.closure.captures = malloc(capture_count * sizeof(*obj->as.closure.captures));
        if (!obj->as.closure.captures) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return idm_nil();
        }
        memcpy(obj->as.closure.captures, captures, capture_count * sizeof(*captures));
        heap_account(idm_active_heap(rt), obj, capture_count * sizeof(*captures));
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
    IdmPatternSelector *selector = module->functions[function_index].selector;
    if (!selector) {
        idm_error_set(err, idm_span_unknown(NULL), "closure function selector is missing");
        return idm_nil();
    }
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
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_RECORD);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.record.shape = shape;
    if (shape->field_count != 0) {
        obj->as.record.field_values = calloc(shape->field_count, sizeof(*obj->as.record.field_values));
        if (!obj->as.record.field_values) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return idm_nil();
        }
        for (size_t i = 0; i < shape->field_count; i++) {
            obj->as.record.field_values[i] = field_values[i];
        }
        heap_account(idm_active_heap(rt), obj, shape->field_count * sizeof(*obj->as.record.field_values));
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
    IdmSymbol *record_type = idm_boxed_object(value)->as.record.shape->type;
    if (symbol) return record_type == symbol;
    const char *record_name = idm_symbol_text(record_type);
    return record_name && strcmp(record_name, name) == 0;
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

bool idm_is_closure(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_CLOSURE;
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

static pthread_mutex_t g_cell_mu = PTHREAD_MUTEX_INITIALIZER;

static IdmValue cell_get_unlocked(IdmValue cell, IdmError *err) {
    if (idm_value_tag(cell) != IDM_VAL_CELL) {
        idm_error_set(err, idm_span_unknown(NULL), "expected cell");
        return idm_nil();
    }
    return idm_boxed_object(cell)->as.cell.value;
}
IdmValue idm_cell_get(IdmValue cell, IdmError *err) {
    pthread_mutex_lock(&g_cell_mu);
    IdmValue out = cell_get_unlocked(cell, err);
    pthread_mutex_unlock(&g_cell_mu);
    return out;
}

static bool cell_set_unlocked(IdmValue cell, IdmValue value, IdmError *err) {
    if (idm_value_tag(cell) != IDM_VAL_CELL) return idm_error_set(err, idm_span_unknown(NULL), "expected cell");
    IdmObject *obj = idm_boxed_object(cell);
    heap_lock(obj->heap);
    if (atomic_load_explicit(&obj->heap->gc_marking, memory_order_acquire)) {
        gc_grey_value_unlocked(obj->heap, obj->as.cell.value);
    }
    obj->as.cell.value = value;
    heap_unlock(obj->heap);
    return true;
}
bool idm_cell_set(IdmValue cell, IdmValue value, IdmError *err) {
    pthread_mutex_lock(&g_cell_mu);
    bool ok = cell_set_unlocked(cell, value, err);
    pthread_mutex_unlock(&g_cell_mu);
    return ok;
}


uint32_t idm_closure_function_index(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_CLOSURE ? idm_boxed_object(value)->as.closure.function_index : UINT32_MAX;
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

bool idm_closure_arity(IdmValue value, IdmArity *out) {
    if (out) *out = idm_arity_unknown();
    if (!out || idm_value_tag(value) != IDM_VAL_CLOSURE) return false;
    const IdmClosureObj *closure = &idm_boxed_object(value)->as.closure;
    const IdmBytecodeModule *module = closure->module;
    if (!idm_bc_is_finalized(module)) return false;
    IdmArity arity = idm_arity_unknown();
    size_t count = closure->entry_count;
    if (count == 0) return false;
    for (size_t i = 0; i < count; i++) {
        uint32_t index = count == 1u ? closure->function_index : closure->entries[i];
        if (index >= module->function_count) return false;
        if (!idm_arity_merge(&arity, &module->functions[index].call_arity)) return false;
    }
    *out = arity;
    return true;
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
    return idm_value_tag(tag) == IDM_VAL_ATOM && strcmp(idm_symbol_text(idm_value_symbol(tag)), "error") == 0;
}

bool idm_value_ok(IdmValue value) {
    if (idm_value_tag(value) == IDM_VAL_NIL) return false;
    if (idm_value_tag(value) == IDM_VAL_ATOM) {
        const char *text = idm_symbol_text(idm_value_symbol(value));
        if (strcmp(text, "false") == 0 || strcmp(text, "nil") == 0) return false;
    }
    if (idm_value_is_error(value)) return false;
    return true;
}

size_t idm_dict_count(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_DICT ? idm_boxed_object(value)->as.dict.count : 0;
}

bool idm_dict_get(IdmValue dict, IdmValue key, IdmValue *out) {
    if (idm_value_tag(dict) != IDM_VAL_DICT) return false;
    for (size_t i = 0; i < idm_boxed_object(dict)->as.dict.count; i++) {
        if (dict_key_equal(idm_boxed_object(dict)->as.dict.entries[i].key, key)) {
            *out = idm_boxed_object(dict)->as.dict.entries[i].value;
            return true;
        }
    }
    return false;
}

bool idm_dict_entry(IdmValue dict, size_t index, IdmValue *out_key, IdmValue *out_value) {
    if (idm_value_tag(dict) != IDM_VAL_DICT || index >= idm_boxed_object(dict)->as.dict.count) return false;
    *out_key = idm_boxed_object(dict)->as.dict.entries[index].key;
    *out_value = idm_boxed_object(dict)->as.dict.entries[index].value;
    return true;
}

IdmValue idm_dict_put(IdmRuntime *rt, IdmValue dict, IdmValue key, IdmValue value, IdmError *err) {
    if (idm_value_tag(dict) != IDM_VAL_DICT) {
        idm_error_set(err, idm_span_unknown(NULL), "dict-put expects a dict");
        return idm_nil();
    }
    size_t n = idm_boxed_object(dict)->as.dict.count;
    IdmValue out = dict_value_with_cap(rt, n + 1u, err);
    if (idm_value_tag(out) != IDM_VAL_DICT) return out;
    IdmObject *obj = idm_boxed_object(out);
    bool replaced = false;
    for (size_t i = 0; i < n; i++) {
        IdmDictEntry entry = idm_boxed_object(dict)->as.dict.entries[i];
        if (dict_key_equal(entry.key, key)) {
            entry.value = value;
            replaced = true;
        }
        obj->as.dict.entries[obj->as.dict.count++] = entry;
    }
    if (!replaced) {
        obj->as.dict.entries[obj->as.dict.count].key = key;
        obj->as.dict.entries[obj->as.dict.count].value = value;
        obj->as.dict.count++;
    }
    return out;
}

IdmValue idm_dict_del(IdmRuntime *rt, IdmValue dict, IdmValue key, IdmError *err) {
    if (idm_value_tag(dict) != IDM_VAL_DICT) {
        idm_error_set(err, idm_span_unknown(NULL), "dict-del expects a dict");
        return idm_nil();
    }
    size_t n = idm_boxed_object(dict)->as.dict.count;
    IdmValue out = dict_value_with_cap(rt, n, err);
    if (idm_value_tag(out) != IDM_VAL_DICT) return out;
    IdmObject *obj = idm_boxed_object(out);
    for (size_t i = 0; i < n; i++) {
        IdmDictEntry entry = idm_boxed_object(dict)->as.dict.entries[i];
        if (!dict_key_equal(entry.key, key)) obj->as.dict.entries[obj->as.dict.count++] = entry;
    }
    return out;
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
    return idm_value_tag(value) == IDM_VAL_STRING ? idm_boxed_object(value)->as.string.bytes : "";
}

size_t idm_string_length(IdmValue value) {
    return idm_value_tag(value) == IDM_VAL_STRING ? idm_boxed_object(value)->as.string.len : 0;
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
            return idm_boxed_object(a)->as.string.len == idm_boxed_object(b)->as.string.len &&
                   memcmp(idm_boxed_object(a)->as.string.bytes, idm_boxed_object(b)->as.string.bytes, idm_boxed_object(a)->as.string.len) == 0;
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
        case IDM_VAL_DICT:
            if (idm_boxed_object(a)->as.dict.count != idm_boxed_object(b)->as.dict.count) return false;
            for (size_t i = 0; i < idm_boxed_object(a)->as.dict.count; i++) {
                IdmValue other;
                if (!idm_dict_get(b, idm_boxed_object(a)->as.dict.entries[i].key, &other)) return false;
                if (!idm_value_equal(idm_boxed_object(a)->as.dict.entries[i].value, other)) return false;
            }
            return true;
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
    }
    return false;
}

static bool write_sequence_item(IdmBuffer *buf, size_t index, void *user) {
    IdmObject *obj = user;
    return idm_value_write(buf, obj->as.sequence.items[index]);
}

static bool write_dict_item(IdmBuffer *buf, size_t index, void *user) {
    IdmObject *obj = user;
    return idm_value_write(buf, obj->as.dict.entries[index].key) &&
           idm_buf_append_char(buf, ' ') &&
           idm_value_write(buf, obj->as.dict.entries[index].value);
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
        case IDM_VAL_STRING: return idm_surface_write_escaped(buf, idm_boxed_object(value)->as.string.bytes, idm_boxed_object(value)->as.string.len);
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
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR: {
            const char *open = idm_value_tag(value) == IDM_VAL_TUPLE ? "{" : "[";
            const char *close = idm_value_tag(value) == IDM_VAL_TUPLE ? "}" : "]";
            IdmObject *obj = idm_boxed_object(value);
            return idm_surface_write_sequence(buf, open, close, obj->as.sequence.count, write_sequence_item, obj);
        }
        case IDM_VAL_DICT: {
            IdmObject *obj = idm_boxed_object(value);
            return idm_surface_write_sequence(buf, "%{", "}", obj->as.dict.count, write_dict_item, obj);
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
        case IDM_VAL_FLOAT: case IDM_VAL_BIGNUM:
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
    IdmObject *existing = copymap_find(map, src);
    if (existing) { *out = idm_from_boxed(existing); return true; }
    IdmObject *dst = heap_alloc_unlocked(target, src->kind);
    if (!dst) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!copymap_put(map, src, dst)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!copystack_push(stack, src, dst)) return idm_error_oom(err, idm_span_unknown(NULL));
    *out = idm_from_boxed(dst);
    return true;
}

static bool copy_fill(IdmRuntime *rt, IdmHeap *target, IdmObject *src, IdmObject *dst, CopyMap *map, CopyStack *stack, IdmError *err) {
    switch (src->kind) {
        case IDM_OBJ_STRING:
            dst->as.string.bytes = idm_strndup(src->as.string.bytes ? src->as.string.bytes : "", src->as.string.len);
            if (!dst->as.string.bytes) return idm_error_oom(err, idm_span_unknown(NULL));
            dst->as.string.len = src->as.string.len;
            heap_account_unlocked(target, dst, src->as.string.len + 1u);
            return true;
        case IDM_OBJ_PAIR:
            if (!copy_intern(rt, target, src->as.pair.car, &dst->as.pair.car, map, stack, err)) return false;
            return copy_intern(rt, target, src->as.pair.cdr, &dst->as.pair.cdr, map, stack, err);
        case IDM_OBJ_TUPLE:
        case IDM_OBJ_VECTOR: {
            size_t n = src->as.sequence.count;
            dst->as.sequence.count = n;
            dst->as.sequence.items = NULL;
            if (n != 0) {
                dst->as.sequence.items = malloc(n * sizeof(IdmValue));
                if (!dst->as.sequence.items) return idm_error_oom(err, idm_span_unknown(NULL));
                for (size_t i = 0; i < n; i++) dst->as.sequence.items[i] = idm_nil();
                heap_account_unlocked(target, dst, n * sizeof(IdmValue));
                for (size_t i = 0; i < n; i++)
                    if (!copy_intern(rt, target, src->as.sequence.items[i], &dst->as.sequence.items[i], map, stack, err)) return false;
            }
            return true;
        }
        case IDM_OBJ_DICT: {
            size_t n = src->as.dict.count;
            dst->as.dict.count = n;
            dst->as.dict.entries = NULL;
            if (n != 0) {
                dst->as.dict.entries = malloc(n * sizeof(IdmDictEntry));
                if (!dst->as.dict.entries) return idm_error_oom(err, idm_span_unknown(NULL));
                for (size_t i = 0; i < n; i++) { dst->as.dict.entries[i].key = idm_nil(); dst->as.dict.entries[i].value = idm_nil(); }
                heap_account_unlocked(target, dst, n * sizeof(IdmDictEntry));
                for (size_t i = 0; i < n; i++) {
                    if (!copy_intern(rt, target, src->as.dict.entries[i].key, &dst->as.dict.entries[i].key, map, stack, err)) return false;
                    if (!copy_intern(rt, target, src->as.dict.entries[i].value, &dst->as.dict.entries[i].value, map, stack, err)) return false;
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
            if (src->as.closure.entry_count > 1) {
                dst->as.closure.entries = malloc(src->as.closure.entry_count * sizeof(uint32_t));
                if (!dst->as.closure.entries) return idm_error_oom(err, idm_span_unknown(NULL));
                memcpy(dst->as.closure.entries, src->as.closure.entries, src->as.closure.entry_count * sizeof(uint32_t));
                heap_account_unlocked(target, dst, src->as.closure.entry_count * sizeof(uint32_t));
            }
            dst->as.closure.capture_count = src->as.closure.capture_count;
            dst->as.closure.captures = NULL;
            if (src->as.closure.capture_count != 0) {
                dst->as.closure.captures = malloc(src->as.closure.capture_count * sizeof(IdmValue));
                if (!dst->as.closure.captures) return idm_error_oom(err, idm_span_unknown(NULL));
                for (size_t i = 0; i < src->as.closure.capture_count; i++) dst->as.closure.captures[i] = idm_nil();
                heap_account_unlocked(target, dst, src->as.closure.capture_count * sizeof(IdmValue));
                for (size_t i = 0; i < src->as.closure.capture_count; i++)
                    if (!copy_intern(rt, target, src->as.closure.captures[i], &dst->as.closure.captures[i], map, stack, err)) return false;
            }
            return true;
        }
        case IDM_OBJ_RECORD:
            dst->as.record.shape = src->as.record.shape;
            if (src->as.record.shape->field_count != 0) {
                dst->as.record.field_values = calloc(src->as.record.shape->field_count, sizeof(*dst->as.record.field_values));
                if (!dst->as.record.field_values) return idm_error_oom(err, idm_span_unknown(NULL));
                heap_account_unlocked(target, dst, src->as.record.shape->field_count * sizeof(*dst->as.record.field_values));
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
            size_t count = src->as.bignum.count;
            size_t bytes = 0;
            if (!bignum_limb_bytes(count, &bytes, err)) return false;
            dst->as.bignum.limbs = bytes == 0u ? NULL : malloc(bytes);
            if (bytes != 0u && !dst->as.bignum.limbs) return idm_error_oom(err, idm_span_unknown(NULL));
            if (bytes != 0u) memcpy(dst->as.bignum.limbs, src->as.bignum.limbs, bytes);
            dst->as.bignum.count = src->as.bignum.count;
            dst->as.bignum.sign = src->as.bignum.sign;
            heap_account_unlocked(target, dst, bytes);
            return true;
        }
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

IdmValue idm_error_reason_value(IdmRuntime *rt, IdmError *err) {
    IdmValue detail;
    if (!idm_error_take_reason(err, &detail)) {
        IdmValue inner[2];
        inner[0] = idm_atom(rt, "runtime");
        inner[1] = idm_string(rt, (err && err->present && err->message) ? err->message : "error", NULL);
        detail = idm_tuple(rt, inner, 2u, NULL);
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

void idm_error_describe(IdmRuntime *rt, IdmValue reason, IdmBuffer *out) {
    (void)rt;
    if (!idm_value_is_error(reason)) { idm_value_write(out, reason); return; }
    IdmValue detail = idm_sequence_item(reason, 1, NULL);
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
        case IDM_TYPE_CON: {
            if (!term->name) return false;
            return idm_value_matches_type_name(value, NULL, term->name, true);
        }
    }
    return false;
}
