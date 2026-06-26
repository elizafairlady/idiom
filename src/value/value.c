#include "idiom/value.h"

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

typedef enum {
    IDM_GC_WHITE,
    IDM_GC_GREY,
    IDM_GC_BLACK
} IdmGcColor;

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
    char *type;
    char **field_names;
    IdmValue *field_values;
    size_t field_count;
} IdmRecordObj;

typedef struct {
    IdmValue value;
} IdmCellObj;

struct IdmObject {
    IdmObjectKind kind;
    IdmGcColor color;
    IdmHeap *heap;
    size_t bytes;
    struct IdmObject *next;
    struct IdmObject *grey_next;
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
    } as;
};

struct IdmSymbol {
    char *text;
    uint32_t id;
    IdmSymbolKind kind;
    uint32_t hash;
    struct IdmSymbol *hnext;
};

static pthread_mutex_t g_intern_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_env_mu = PTHREAD_MUTEX_INITIALIZER;

static IdmSymbol *idm_intern_unlocked(IdmIntern *intern, IdmSymbolKind kind, const char *text);
static IdmEnv *env_get_or_create_unlocked(IdmRuntime *rt, const char *package_key);

static void heap_lock(IdmHeap *heap) {
    if (heap->locking) pthread_mutex_lock(&heap->lock);
}

static void heap_unlock(IdmHeap *heap) {
    if (heap->locking) pthread_mutex_unlock(&heap->lock);
}

static IdmObject *heap_alloc_extra_unlocked(IdmHeap *heap, IdmObjectKind kind, size_t extra) {
    if (extra > SIZE_MAX - sizeof(IdmObject)) return NULL;
    IdmObject *obj = calloc(1u, sizeof(*obj) + extra);
    if (!obj) return NULL;
    obj->kind = kind;
    obj->color = IDM_GC_WHITE;
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
        free(obj->as.record.type);
        for (size_t i = 0; i < obj->as.record.field_count; i++) free(obj->as.record.field_names[i]);
        free(obj->as.record.field_names);
        free(obj->as.record.field_values);
    }
    if (obj->kind == IDM_OBJ_REGEX) idm_regex_free(obj->as.regex);
    if (obj->kind == IDM_OBJ_REGEX_RESULT) idm_regex_result_free(obj->as.regex_result);
    free(obj);
}

void idm_runtime_init(IdmRuntime *rt) {
    idm_intern_init(&rt->intern);
    rt->cached_true = idm_atom(rt, "true");
    rt->cached_false = idm_atom(rt, "false");
    idm_heap_init(&rt->heap);
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
    rt->owned_temps = NULL;
    rt->owned_temp_count = 0;
    rt->owned_temp_cap = 0;
    rt->repl = NULL;
    rt->interactive = false;
}

bool idm_runtime_own_temp(IdmRuntime *rt, const char *path) {
    if (rt->owned_temp_count == rt->owned_temp_cap) {
        size_t cap = rt->owned_temp_cap ? rt->owned_temp_cap * 2u : 8u;
        char **grown = realloc(rt->owned_temps, cap * sizeof(*grown));
        if (!grown) return false;
        rt->owned_temps = grown;
        rt->owned_temp_cap = cap;
    }
    rt->owned_temps[rt->owned_temp_count] = idm_strdup(path);
    if (!rt->owned_temps[rt->owned_temp_count]) return false;
    rt->owned_temp_count++;
    return true;
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
    for (size_t i = 0; i < rt->owned_temp_count; i++) {
        remove(rt->owned_temps[i]);
        free(rt->owned_temps[i]);
    }
    free(rt->owned_temps);
    rt->owned_temps = NULL;
    rt->owned_temp_count = 0;
    rt->owned_temp_cap = 0;
    idm_heap_destroy(&rt->heap);
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
        size_t cap = rt->env_cap ? rt->env_cap * 2u : 8u;
        IdmEnv **grown = realloc(rt->envs, cap * sizeof(*grown));
        if (!grown) { free(env->package_key); free(env); return NULL; }
        rt->envs = grown;
        rt->env_cap = cap;
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
        size_t cap = env->slot_cap ? env->slot_cap * 2u : 16u;
        while (cap < needed) cap *= 2u;
        IdmValue *grown = realloc(env->slots, cap * sizeof(*grown));
        if (!grown) {
            pthread_mutex_unlock(&g_env_mu);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        env->slots = grown;
        env->slot_cap = cap;
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
    if (intern->bucket_count) {
        for (IdmSymbol *s = intern->buckets[h & (intern->bucket_count - 1u)]; s; s = s->hnext) {
            if (s->hash == h && s->kind == kind && strcmp(s->text, text) == 0) return s;
        }
    }
    if (intern->count == intern->cap) {
        size_t cap = intern->cap ? intern->cap * 2u : 32u;
        IdmSymbol **next = realloc(intern->symbols, cap * sizeof(*next));
        if (!next) return NULL;
        intern->symbols = next;
        intern->cap = cap;
    }
    if (intern->bucket_count == 0u || intern->count + 1u > intern->bucket_count - (intern->bucket_count >> 2)) {
        if (!intern_rehash(intern, intern->bucket_count ? intern->bucket_count * 2u : 64u)) return NULL;
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
    sym->hash = h;
    intern->symbols[intern->count++] = sym;
    size_t b = h & (intern->bucket_count - 1u);
    sym->hnext = intern->buckets[b];
    intern->buckets[b] = sym;
    return sym;
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
    heap->object_count = 0;
    heap->bytes_allocated = 0;
    heap->locking = false;
    pthread_mutex_init(&heap->lock, NULL);
}

static _Thread_local IdmHeap *t_active_heap = NULL;

IdmHeap *idm_active_heap(IdmRuntime *rt) {
    return t_active_heap ? t_active_heap : &rt->heap;
}

void idm_set_active_heap(IdmHeap *heap) {
    t_active_heap = heap;
}

void idm_heap_destroy(IdmHeap *heap) {
    IdmObject *obj = heap->objects;
    while (obj) {
        IdmObject *next = obj->next;
        object_free(obj);
        obj = next;
    }
    heap->objects = NULL;
    heap->grey = NULL;
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

static void gc_push_grey(IdmHeap *heap, IdmObject *obj) {
    obj->grey_next = heap->grey;
    heap->grey = obj;
}

static void gc_grey_value(IdmHeap *heap, IdmValue value) {
    IdmObject *obj = value_object(value);
    if (!obj || obj->heap != heap || obj->color != IDM_GC_WHITE) return;
    obj->color = IDM_GC_GREY;
    gc_push_grey(heap, obj);
}

static void gc_trace_object(IdmHeap *heap, IdmObject *obj) {
    if (!obj || obj->color != IDM_GC_GREY) return;
    obj->color = IDM_GC_BLACK;
    if (obj->kind == IDM_OBJ_PAIR) {
        gc_grey_value(heap, obj->as.pair.car);
        gc_grey_value(heap, obj->as.pair.cdr);
    } else if (obj->kind == IDM_OBJ_TUPLE || obj->kind == IDM_OBJ_VECTOR) {
        for (size_t i = 0; i < obj->as.sequence.count; i++) gc_grey_value(heap, obj->as.sequence.items[i]);
    } else if (obj->kind == IDM_OBJ_DICT) {
        for (size_t i = 0; i < obj->as.dict.count; i++) {
            gc_grey_value(heap, obj->as.dict.entries[i].key);
            gc_grey_value(heap, obj->as.dict.entries[i].value);
        }
    } else if (obj->kind == IDM_OBJ_CELL) {
        gc_grey_value(heap, obj->as.cell.value);
    } else if (obj->kind == IDM_OBJ_CLOSURE) {
        for (size_t i = 0; i < obj->as.closure.capture_count; i++) gc_grey_value(heap, obj->as.closure.captures[i]);
    } else if (obj->kind == IDM_OBJ_RECORD) {
        for (size_t i = 0; i < obj->as.record.field_count; i++) gc_grey_value(heap, obj->as.record.field_values[i]);
    } else if (obj->kind == IDM_OBJ_REGEX_RESULT) {
        gc_grey_value(heap, idm_regex_result_subject_value(obj->as.regex_result));
    }
}

static void gc_drain_grey(IdmHeap *heap) {
    while (heap->grey) {
        IdmObject *obj = heap->grey;
        heap->grey = obj->grey_next;
        obj->grey_next = NULL;
        gc_trace_object(heap, obj);
    }
}

void idm_gc_mark_value(IdmHeap *heap, IdmValue value) {
    gc_grey_value(heap, value);
}

void idm_heap_sweep(IdmHeap *heap) {
    gc_drain_grey(heap);
    IdmObject **cursor = &heap->objects;
    while (*cursor) {
        IdmObject *obj = *cursor;
        if (obj->color == IDM_GC_BLACK) {
            obj->color = IDM_GC_WHITE;
            obj->grey_next = NULL;
            cursor = &obj->next;
        } else {
            *cursor = obj->next;
            heap->object_count--;
            heap->bytes_allocated -= obj->bytes < heap->bytes_allocated ? obj->bytes : heap->bytes_allocated;
            object_free(obj);
        }
    }
}

size_t idm_heap_bytes(const IdmHeap *heap) {
    IdmHeap *h = (IdmHeap *)heap;
    heap_lock(h);
    size_t n = h->bytes_allocated;
    heap_unlock(h);
    return n;
}


void idm_heap_collect(IdmHeap *heap, const IdmValue *roots, size_t root_count) {
    for (size_t i = 0; i < root_count; i++) idm_gc_mark_value(heap, roots[i]);
    idm_heap_sweep(heap);
}

size_t idm_heap_object_count(const IdmHeap *heap) {
    return heap->object_count;
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
    switch (kind) {
        case IDM_SYNTAX_BUILD_LIST: return idm_syn_list(span);
        case IDM_SYNTAX_BUILD_VECTOR: return idm_syn_vector(span);
        case IDM_SYNTAX_BUILD_TUPLE: return idm_syn_tuple(span);
        case IDM_SYNTAX_BUILD_DICT: return idm_syn_dict(span);
        default: return NULL;
    }
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
            if (idm_value_tag(payload) != IDM_VAL_INT) return idm_error_set(err, ctx->span, "make-syntax-int expects int");
            syn = idm_syn_int(idm_int_value(payload), ctx->span);
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

IdmValue idm_record(IdmRuntime *rt, const char *type, const char *const *field_names, const IdmValue *field_values, size_t field_count, IdmError *err) {
    if (!type || !*type) {
        idm_error_set(err, idm_span_unknown(NULL), "record type must be a non-empty name");
        return idm_nil();
    }
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_RECORD);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.record.type = idm_strdup(type);
    if (!obj->as.record.type) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.record.field_count = field_count;
    if (field_count != 0) {
        obj->as.record.field_names = calloc(field_count, sizeof(*obj->as.record.field_names));
        obj->as.record.field_values = calloc(field_count, sizeof(*obj->as.record.field_values));
        if (!obj->as.record.field_names || !obj->as.record.field_values) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return idm_nil();
        }
        for (size_t i = 0; i < field_count; i++) {
            if (!field_names[i] || !*field_names[i]) {
                idm_error_set(err, idm_span_unknown(NULL), "record field must be a non-empty name");
                return idm_nil();
            }
            obj->as.record.field_names[i] = idm_strdup(field_names[i]);
            if (!obj->as.record.field_names[i]) {
                idm_error_oom(err, idm_span_unknown(NULL));
                return idm_nil();
            }
            obj->as.record.field_values[i] = field_values[i];
        }
        heap_account(idm_active_heap(rt), obj, field_count * (sizeof(*obj->as.record.field_names) + sizeof(*obj->as.record.field_values)));
        for (size_t i = 0; i < field_count; i++) heap_account(idm_active_heap(rt), obj, strlen(obj->as.record.field_names[i]) + 1u);
    }
    heap_account(idm_active_heap(rt), obj, strlen(obj->as.record.type) + 1u);
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

const char *idm_value_type_name(IdmValueTag tag) {
    switch (tag) {
        case IDM_VAL_NIL: return "nil";
        case IDM_VAL_ATOM: return "atom";
        case IDM_VAL_WORD: return "word";
        case IDM_VAL_INT: return "int";
        case IDM_VAL_FLOAT: return "float";
        case IDM_VAL_STRING: return "string";
        case IDM_VAL_PAIR: return "pair";
        case IDM_VAL_TUPLE: return "tuple";
        case IDM_VAL_VECTOR: return "vector";
        case IDM_VAL_DICT: return "dict";
        case IDM_VAL_SYNTAX: return "syntax";
        case IDM_VAL_CELL: return "cell";
        case IDM_VAL_CLOSURE: return "closure";
        case IDM_VAL_PID: return "pid";
        case IDM_VAL_REF: return "ref";
        case IDM_VAL_PORT: return "port";
        case IDM_VAL_RECORD: return "record";
        case IDM_VAL_REGEX: return "regex";
        case IDM_VAL_REGEX_RESULT: return "regex-result";
        case IDM_VAL_BIGNUM: return "int";
    }
    return "<bad-type>";
}

const char *idm_value_dispatch_type_name(IdmValue value) {
    if (idm_value_tag(value) == IDM_VAL_RECORD) return idm_boxed_object(value)->as.record.type;
    return idm_value_type_name(idm_value_tag(value));
}

bool idm_value_type_from_name(const char *name, IdmValueTag *out) {
    if (!name) return false;
    for (int i = (int)IDM_VAL_NIL; i <= (int)IDM_VAL_REGEX_RESULT; i++) {
        IdmValueTag tag = (IdmValueTag)i;
        if (tag == IDM_VAL_RECORD) continue;
        if (strcmp(name, idm_value_type_name(tag)) == 0) {
            if (out) *out = tag;
            return true;
        }
    }
    return false;
}

bool idm_value_matches_type_name(IdmValue value, const char *type) {
    if (!type) return false;
    IdmValueTag vt = idm_value_tag(value);
    if (strcmp(type, "record") == 0) return vt == IDM_VAL_RECORD;
    if (vt == IDM_VAL_RECORD) return idm_record_is_a(value, type);
    if (strcmp(type, "int") == 0) return vt == IDM_VAL_INT || vt == IDM_VAL_BIGNUM;
    if (strcmp(type, "empty-list") == 0) return idm_is_empty_list(value);
    if (strcmp(type, "list") == 0) {
        IdmValue cur = value;
        while (idm_is_pair(cur)) cur = idm_cdr(cur, NULL);
        return idm_is_empty_list(cur);
    }
    IdmValueTag tag = IDM_VAL_NIL;
    return idm_value_type_from_name(type, &tag) && vt == tag;
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
    idm_boxed_object(cell)->as.cell.value = value;
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
    return idm_boxed_object(value)->as.record.type;
}

size_t idm_record_field_count(IdmValue value, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_RECORD) {
        idm_error_set(err, idm_span_unknown(NULL), "expected record");
        return 0;
    }
    return idm_boxed_object(value)->as.record.field_count;
}

const char *idm_record_field_name(IdmValue value, size_t index, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_RECORD) {
        idm_error_set(err, idm_span_unknown(NULL), "expected record");
        return NULL;
    }
    if (index >= idm_boxed_object(value)->as.record.field_count) {
        idm_error_set(err, idm_span_unknown(NULL), "record field index out of bounds");
        return NULL;
    }
    return idm_boxed_object(value)->as.record.field_names[index];
}

IdmValue idm_record_field_value(IdmValue value, size_t index, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_RECORD) {
        idm_error_set(err, idm_span_unknown(NULL), "expected record");
        return idm_nil();
    }
    if (index >= idm_boxed_object(value)->as.record.field_count) {
        idm_error_set(err, idm_span_unknown(NULL), "record field index out of bounds");
        return idm_nil();
    }
    return idm_boxed_object(value)->as.record.field_values[index];
}

bool idm_record_field_named(IdmValue value, const char *field, IdmValue *out, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_RECORD) return idm_error_set(err, idm_span_unknown(NULL), "record field access expects a record");
    if (!field || !*field) return idm_error_set(err, idm_span_unknown(NULL), "record field must be a non-empty name");
    for (size_t i = 0; i < idm_boxed_object(value)->as.record.field_count; i++) {
        if (strcmp(idm_boxed_object(value)->as.record.field_names[i], field) == 0) {
            if (out) *out = idm_boxed_object(value)->as.record.field_values[i];
            return true;
        }
    }
    return idm_error_set(err, idm_span_unknown(NULL), "record '%s' has no field '%s'", idm_boxed_object(value)->as.record.type, field);
}

bool idm_record_field_project(IdmValue value, const char *type, const char *field, uint32_t field_index, IdmValue *out, IdmError *err) {
    if (idm_value_tag(value) != IDM_VAL_RECORD) return idm_error_set(err, idm_span_unknown(NULL), "record field access expects a record");
    if (!type || !*type) return idm_error_set(err, idm_span_unknown(NULL), "record field type must be a non-empty name");
    if (!field || !*field) return idm_error_set(err, idm_span_unknown(NULL), "record field must be a non-empty name");
    if (strcmp(idm_boxed_object(value)->as.record.type, type) != 0) {
        return idm_error_set(err, idm_span_unknown(NULL), "field '%s' expects record type '%s', got '%s'", field, type, idm_boxed_object(value)->as.record.type);
    }
    if ((size_t)field_index >= idm_boxed_object(value)->as.record.field_count) {
        return idm_error_set(err, idm_span_unknown(NULL), "record '%s' has no field slot %u for '%s'", type, field_index, field);
    }
    const char *actual = idm_boxed_object(value)->as.record.field_names[field_index];
    if (strcmp(actual, field) != 0) {
        return idm_error_set(err, idm_span_unknown(NULL), "record '%s' field slot %u is '%s', not '%s'", type, field_index, actual, field);
    }
    if (out) *out = idm_boxed_object(value)->as.record.field_values[field_index];
    return true;
}

bool idm_record_is_a(IdmValue value, const char *type) {
    if (!type) return false;
    if (strcmp(type, "record") == 0) return idm_value_tag(value) == IDM_VAL_RECORD;
    return idm_value_tag(value) == IDM_VAL_RECORD && strcmp(idm_boxed_object(value)->as.record.type, type) == 0;
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
            if (strcmp(idm_boxed_object(a)->as.record.type, idm_boxed_object(b)->as.record.type) != 0) return false;
            if (idm_boxed_object(a)->as.record.field_count != idm_boxed_object(b)->as.record.field_count) return false;
            for (size_t i = 0; i < idm_boxed_object(a)->as.record.field_count; i++) {
                if (strcmp(idm_boxed_object(a)->as.record.field_names[i], idm_boxed_object(b)->as.record.field_names[i]) != 0) return false;
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
            return false;
    }
    return false;
}

static bool write_escaped(IdmBuffer *buf, const char *text, size_t len) {
    if (!idm_buf_append_char(buf, '"')) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        switch (ch) {
            case '\\': if (!idm_buf_append(buf, "\\\\")) return false; break;
            case '"': if (!idm_buf_append(buf, "\\\"")) return false; break;
            case '\n': if (!idm_buf_append(buf, "\\n")) return false; break;
            case '\r': if (!idm_buf_append(buf, "\\r")) return false; break;
            case '\t': if (!idm_buf_append(buf, "\\t")) return false; break;
            default:
                if (ch < 32u) {
                    if (!idm_buf_appendf(buf, "\\x%02x", ch)) return false;
                } else if (!idm_buf_append_char(buf, (char)ch)) return false;
        }
    }
    return idm_buf_append_char(buf, '"');
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
        case IDM_VAL_STRING: return write_escaped(buf, idm_boxed_object(value)->as.string.bytes, idm_boxed_object(value)->as.string.len);
        case IDM_VAL_PID: return idm_buf_appendf(buf, "#<pid:%llu>", (unsigned long long)idm_value_id(value));
        case IDM_VAL_REF: return idm_buf_appendf(buf, "#<ref:%llu>", (unsigned long long)idm_value_id(value));
        case IDM_VAL_PORT: return idm_buf_appendf(buf, "#<port:%llu>", (unsigned long long)idm_value_id(value));
        case IDM_VAL_RECORD: return idm_buf_appendf(buf, "#<record:%s>", idm_boxed_object(value)->as.record.type);
        case IDM_VAL_REGEX: {
            size_t len = 0;
            const char *source = idm_regex_source(idm_boxed_object(value)->as.regex, &len);
            return idm_buf_append_char(buf, 'r') && write_escaped(buf, source, len);
        }
        case IDM_VAL_REGEX_RESULT:
            return idm_buf_append(buf, "#<regex-result>");
        case IDM_VAL_BIGNUM:
            return idm_buf_append(buf, "#<bignum>");
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR: {
            const char *open = idm_value_tag(value) == IDM_VAL_TUPLE ? "{" : "[";
            const char *close = idm_value_tag(value) == IDM_VAL_TUPLE ? "}" : "]";
            if (!idm_buf_append(buf, open)) return false;
            for (size_t i = 0; i < idm_boxed_object(value)->as.sequence.count; i++) {
                if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
                if (!idm_value_write(buf, idm_boxed_object(value)->as.sequence.items[i])) return false;
            }
            return idm_buf_append(buf, close);
        }
        case IDM_VAL_DICT: {
            if (!idm_buf_append(buf, "%{")) return false;
            for (size_t i = 0; i < idm_boxed_object(value)->as.dict.count; i++) {
                if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
                if (!idm_value_write(buf, idm_boxed_object(value)->as.dict.entries[i].key)) return false;
                if (!idm_buf_append_char(buf, ' ')) return false;
                if (!idm_value_write(buf, idm_boxed_object(value)->as.dict.entries[i].value)) return false;
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
        size_t ncap = m->cap ? m->cap * 2u : 16u;
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
        size_t ncap = s->cap ? s->cap * 2u : 64u;
        CopyWork *next = realloc(s->items, ncap * sizeof(*next));
        if (!next) return false;
        s->items = next;
        s->cap = ncap;
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
            dst->as.record.type = idm_strdup(src->as.record.type);
            if (!dst->as.record.type) return idm_error_oom(err, idm_span_unknown(NULL));
            heap_account_unlocked(target, dst, strlen(dst->as.record.type) + 1u);
            dst->as.record.field_count = src->as.record.field_count;
            if (src->as.record.field_count != 0) {
                dst->as.record.field_names = calloc(src->as.record.field_count, sizeof(*dst->as.record.field_names));
                dst->as.record.field_values = calloc(src->as.record.field_count, sizeof(*dst->as.record.field_values));
                if (!dst->as.record.field_names || !dst->as.record.field_values) return idm_error_oom(err, idm_span_unknown(NULL));
                heap_account_unlocked(target, dst, src->as.record.field_count * (sizeof(*dst->as.record.field_names) + sizeof(*dst->as.record.field_values)));
                for (size_t i = 0; i < src->as.record.field_count; i++) {
                    dst->as.record.field_names[i] = idm_strdup(src->as.record.field_names[i]);
                    if (!dst->as.record.field_names[i]) return idm_error_oom(err, idm_span_unknown(NULL));
                    heap_account_unlocked(target, dst, strlen(dst->as.record.field_names[i]) + 1u);
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
        case IDM_OBJ_BIGNUM:
            return true;
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
