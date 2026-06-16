#include "idiom/value.h"

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
    IDM_OBJ_REGEX_RESULT
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
    IdmNamespace *ns;
} IdmClosureObj;

typedef struct {
    char *type;
    IdmValue fields;
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
static pthread_mutex_t g_ns_mu = PTHREAD_MUTEX_INITIALIZER;

static IdmSymbol *idm_intern_unlocked(IdmIntern *intern, IdmSymbolKind kind, const char *text);
static IdmNamespace *namespace_get_or_create_unlocked(IdmRuntime *rt, const char *name);

static IdmObject *heap_alloc_unlocked(IdmHeap *heap, IdmObjectKind kind) {
    IdmObject *obj = calloc(1u, sizeof(*obj));
    if (!obj) return NULL;
    obj->kind = kind;
    obj->color = IDM_GC_WHITE;
    obj->heap = heap;
    obj->bytes = sizeof(*obj);
    obj->next = heap->objects;
    heap->objects = obj;
    heap->object_count++;
    heap->bytes_allocated += sizeof(*obj);
    return obj;
}

static IdmObject *heap_alloc(IdmHeap *heap, IdmObjectKind kind) {
    pthread_mutex_lock(&heap->lock);
    IdmObject *obj = heap_alloc_unlocked(heap, kind);
    pthread_mutex_unlock(&heap->lock);
    return obj;
}

static void heap_account_unlocked(IdmHeap *heap, IdmObject *obj, size_t extra) {
    obj->bytes += extra;
    heap->bytes_allocated += extra;
}

static void heap_account(IdmHeap *heap, IdmObject *obj, size_t extra) {
    pthread_mutex_lock(&heap->lock);
    heap_account_unlocked(heap, obj, extra);
    pthread_mutex_unlock(&heap->lock);
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
    if (obj->kind == IDM_OBJ_DICT) free(obj->as.dict.entries);
    if (obj->kind == IDM_OBJ_SYNTAX) idm_syn_free(obj->as.syntax);
    if (obj->kind == IDM_OBJ_CLOSURE) {
        free(obj->as.closure.entries);
        free(obj->as.closure.captures);
    }
    if (obj->kind == IDM_OBJ_RECORD) free(obj->as.record.type);
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
    rt->register_operator_user = NULL;
    rt->register_operator = NULL;
    rt->register_macro_user = NULL;
    rt->register_macro = NULL;
    rt->expander_surface_user = NULL;
    rt->expander_surface = NULL;
    rt->cli_args = NULL;
    rt->cli_arg_count = 0;

    rt->namespaces = NULL;
    rt->ns_count = 0;
    rt->ns_cap = 0;
    rt->main_ns = idm_namespace_get_or_create(rt, "main");
    memset(rt->trait_worlds, 0, sizeof(rt->trait_worlds));
    rt->trait_phase = 0;
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

static void runtime_trait_method_destroy(IdmRuntimeTraitMethod *method) {
    if (!method) return;
    free(method->trait);
    free(method->method);
    memset(method, 0, sizeof(*method));
}

static void runtime_trait_contract_destroy(IdmRuntimeTraitContract *contract) {
    if (!contract) return;
    free(contract->name);
    for (size_t i = 0; i < contract->requirement_count; i++) free(contract->requirements[i]);
    free(contract->requirements);
    memset(contract, 0, sizeof(*contract));
}

static void runtime_trait_impl_destroy(IdmRuntimeTraitImpl *impl) {
    if (!impl) return;
    free(impl->trait);
    free(impl->method);
    free(impl->type);
    memset(impl, 0, sizeof(*impl));
}

static void runtime_trait_conformance_destroy(IdmRuntimeTraitConformance *conf) {
    if (!conf) return;
    free(conf->trait);
    free(conf->type);
    free(conf->provider);
    free(conf->provider_key);
    memset(conf, 0, sizeof(*conf));
}

void idm_runtime_destroy(IdmRuntime *rt) {
    for (size_t w = 0; w < 2u; w++) {
        IdmTraitWorld *world = &rt->trait_worlds[w];
        for (size_t i = 0; i < world->contract_count; i++) runtime_trait_contract_destroy(&world->contracts[i]);
        free(world->contracts);
        for (size_t i = 0; i < world->method_count; i++) runtime_trait_method_destroy(&world->methods[i]);
        free(world->methods);
        for (size_t i = 0; i < world->impl_count; i++) runtime_trait_impl_destroy(&world->impls[i]);
        free(world->impls);
        for (size_t i = 0; i < world->conformance_count; i++) runtime_trait_conformance_destroy(&world->conformances[i]);
        free(world->conformances);
    }
    memset(rt->trait_worlds, 0, sizeof(rt->trait_worlds));
    for (size_t i = 0; i < rt->ns_count; i++) {
        free(rt->namespaces[i]->name);
        free(rt->namespaces[i]->slots);
        free(rt->namespaces[i]);
    }
    free(rt->namespaces);
    rt->namespaces = NULL;
    rt->ns_count = 0;
    rt->ns_cap = 0;
    rt->main_ns = NULL;
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

IdmNamespace *idm_namespace_get_or_create(IdmRuntime *rt, const char *name) {
    pthread_mutex_lock(&g_ns_mu);
    IdmNamespace *found = namespace_get_or_create_unlocked(rt, name);
    pthread_mutex_unlock(&g_ns_mu);
    return found;
}

static IdmNamespace *namespace_get_or_create_unlocked(IdmRuntime *rt, const char *name) {
    for (size_t i = 0; i < rt->ns_count; i++) {
        if (strcmp(rt->namespaces[i]->name, name) == 0) return rt->namespaces[i];
    }
    IdmNamespace *ns = calloc(1u, sizeof(*ns));
    if (!ns) return NULL;
    ns->name = idm_strdup(name);
    if (!ns->name) { free(ns); return NULL; }
    if (rt->ns_count == rt->ns_cap) {
        size_t cap = rt->ns_cap ? rt->ns_cap * 2u : 8u;
        IdmNamespace **grown = realloc(rt->namespaces, cap * sizeof(*grown));
        if (!grown) { free(ns->name); free(ns); return NULL; }
        rt->namespaces = grown;
        rt->ns_cap = cap;
    }
    rt->namespaces[rt->ns_count++] = ns;
    return ns;
}

bool idm_ns_slot_ensure(IdmNamespace *ns, uint32_t id, IdmError *err) {
    pthread_mutex_lock(&g_ns_mu);
    size_t needed = (size_t)id + 1u;
    if (needed <= ns->slot_count) {
        pthread_mutex_unlock(&g_ns_mu);
        return true;
    }
    if (needed > ns->slot_cap) {
        size_t cap = ns->slot_cap ? ns->slot_cap * 2u : 16u;
        while (cap < needed) cap *= 2u;
        IdmValue *grown = realloc(ns->slots, cap * sizeof(*grown));
        if (!grown) {
            pthread_mutex_unlock(&g_ns_mu);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        ns->slots = grown;
        ns->slot_cap = cap;
    }
    for (size_t i = ns->slot_count; i < needed; i++) ns->slots[i] = idm_nil();
    ns->slot_count = needed;
    pthread_mutex_unlock(&g_ns_mu);
    return true;
}

void idm_ns_slot_set(IdmNamespace *ns, uint32_t id, IdmValue value) {
    pthread_mutex_lock(&g_ns_mu);
    if ((size_t)id < ns->slot_count) ns->slots[id] = value;
    pthread_mutex_unlock(&g_ns_mu);
}

IdmValue idm_ns_slot_get(const IdmNamespace *ns, uint32_t id) {
    pthread_mutex_lock(&g_ns_mu);
    IdmValue out = (size_t)id < ns->slot_count ? ns->slots[id] : idm_nil();
    pthread_mutex_unlock(&g_ns_mu);
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
    if (value.tag != IDM_VAL_STRING && value.tag != IDM_VAL_PAIR && value.tag != IDM_VAL_TUPLE && value.tag != IDM_VAL_VECTOR && value.tag != IDM_VAL_DICT && value.tag != IDM_VAL_SYNTAX && value.tag != IDM_VAL_CELL && value.tag != IDM_VAL_CLOSURE && value.tag != IDM_VAL_RECORD && value.tag != IDM_VAL_REGEX && value.tag != IDM_VAL_REGEX_RESULT) return NULL;
    return value.as.obj;
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
        gc_grey_value(heap, obj->as.record.fields);
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
    pthread_mutex_lock(&h->lock);
    size_t n = h->bytes_allocated;
    pthread_mutex_unlock(&h->lock);
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
    IdmValue v;
    v.tag = IDM_VAL_NIL;
    v.as.id = 0;
    return v;
}

IdmValue idm_empty_list(void) {
    IdmValue v;
    v.tag = IDM_VAL_EMPTY_LIST;
    v.as.id = 0;
    return v;
}

IdmValue idm_int(int64_t value) {
    IdmValue v;
    v.tag = IDM_VAL_INT;
    v.as.i = value;
    return v;
}

IdmValue idm_float(double value) {
    IdmValue v;
    v.tag = IDM_VAL_FLOAT;
    v.as.f = value;
    return v;
}

IdmValue idm_word(IdmRuntime *rt, const char *text) {
    IdmValue v;
    v.tag = IDM_VAL_WORD;
    v.as.symbol = idm_intern(&rt->intern, IDM_SYMBOL_WORD, text);
    return v;
}

IdmValue idm_atom(IdmRuntime *rt, const char *text) {
    IdmValue v;
    v.tag = IDM_VAL_ATOM;
    v.as.symbol = idm_intern(&rt->intern, IDM_SYMBOL_ATOM, text);
    return v;
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
    v.tag = IDM_VAL_STRING;
    v.as.obj = obj;
    return v;
}

IdmValue idm_string(IdmRuntime *rt, const char *text, IdmError *err) {
    return idm_string_n(rt, text, strlen(text), err);
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
    v.tag = IDM_VAL_PAIR;
    v.as.obj = obj;
    return v;
}

static IdmValue sequence_value(IdmRuntime *rt, const IdmValue *items, size_t count, IdmObjectKind obj_kind, IdmValueTag tag, IdmError *err) {
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
    IdmValue v;
    v.tag = tag;
    v.as.obj = obj;
    return v;
}

IdmValue idm_tuple(IdmRuntime *rt, const IdmValue *items, size_t count, IdmError *err) {
    return sequence_value(rt, items, count, IDM_OBJ_TUPLE, IDM_VAL_TUPLE, err);
}

IdmValue idm_vector(IdmRuntime *rt, const IdmValue *items, size_t count, IdmError *err) {
    return sequence_value(rt, items, count, IDM_OBJ_VECTOR, IDM_VAL_VECTOR, err);
}

IdmValue idm_dict(IdmRuntime *rt, const IdmDictEntry *entries, size_t count, IdmError *err) {
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_DICT);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.dict.count = count;
    obj->as.dict.entries = NULL;
    if (count != 0) {
        obj->as.dict.entries = malloc(count * sizeof(*obj->as.dict.entries));
        if (!obj->as.dict.entries) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return idm_nil();
        }
        memcpy(obj->as.dict.entries, entries, count * sizeof(*entries));
        heap_account(idm_active_heap(rt), obj, count * sizeof(*entries));
    }
    IdmValue v;
    v.tag = IDM_VAL_DICT;
    v.as.obj = obj;
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
    v.tag = IDM_VAL_SYNTAX;
    v.as.obj = obj;
    return v;
}

IdmValue idm_cell(IdmRuntime *rt, IdmValue initial, IdmError *err) {
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_CELL);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.cell.value = initial;
    IdmValue v;
    v.tag = IDM_VAL_CELL;
    v.as.obj = obj;
    return v;
}

IdmValue idm_closure_multi_in_module(IdmRuntime *rt, const IdmBytecodeModule *module, const uint32_t *function_indexes, size_t function_count, const IdmValue *captures, size_t capture_count, IdmNamespace *ns, IdmError *err) {
    if (function_count == 0) {
        idm_error_set(err, idm_span_unknown(NULL), "closure must have at least one function entry");
        return idm_nil();
    }
    IdmObject *obj = heap_alloc(idm_active_heap(rt), IDM_OBJ_CLOSURE);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.closure.module = module;
    obj->as.closure.ns = ns;
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
    IdmValue v;
    v.tag = IDM_VAL_CLOSURE;
    v.as.obj = obj;
    return v;
}

IdmValue idm_closure_multi(IdmRuntime *rt, const uint32_t *function_indexes, size_t function_count, const IdmValue *captures, size_t capture_count, IdmNamespace *ns, IdmError *err) {
    return idm_closure_multi_in_module(rt, NULL, function_indexes, function_count, captures, capture_count, ns, err);
}

IdmValue idm_closure(IdmRuntime *rt, uint32_t function_index, const IdmValue *captures, size_t capture_count, IdmNamespace *ns, IdmError *err) {
    return idm_closure_multi(rt, &function_index, 1, captures, capture_count, ns, err);
}

IdmValue idm_closure_in_module(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, const IdmValue *captures, size_t capture_count, IdmNamespace *ns, IdmError *err) {
    return idm_closure_multi_in_module(rt, module, &function_index, 1, captures, capture_count, ns, err);
}

IdmValue idm_record(IdmRuntime *rt, const char *type, IdmValue fields, IdmError *err) {
    if (!type || !*type) {
        idm_error_set(err, idm_span_unknown(NULL), "record type must be a non-empty name");
        return idm_nil();
    }
    if (!idm_is_dict(fields)) {
        idm_error_set(err, idm_span_unknown(NULL), "record fields must be a dict");
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
    obj->as.record.fields = fields;
    heap_account(idm_active_heap(rt), obj, strlen(obj->as.record.type) + 1u);
    IdmValue v;
    v.tag = IDM_VAL_RECORD;
    v.as.obj = obj;
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
    v.tag = IDM_VAL_REGEX;
    v.as.obj = obj;
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
    v.tag = IDM_VAL_REGEX_RESULT;
    v.as.obj = obj;
    return v;
}

IdmValue idm_pid(uint64_t id) {
    IdmValue v;
    v.tag = IDM_VAL_PID;
    v.as.id = id;
    return v;
}

IdmValue idm_ref(uint64_t id) {
    IdmValue v;
    v.tag = IDM_VAL_REF;
    v.as.id = id;
    return v;
}

IdmValue idm_port(uint64_t id) {
    IdmValue v;
    v.tag = IDM_VAL_PORT;
    v.as.id = id;
    return v;
}

IdmValue idm_primitive_value(uint32_t primitive) {
    IdmValue v;
    v.tag = IDM_VAL_PRIMITIVE;
    v.as.id = primitive;
    return v;
}

bool idm_is_primitive(IdmValue value) {
    return value.tag == IDM_VAL_PRIMITIVE;
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
        case IDM_VAL_EMPTY_LIST: return "empty-list";
        case IDM_VAL_TUPLE: return "tuple";
        case IDM_VAL_VECTOR: return "vector";
        case IDM_VAL_DICT: return "dict";
        case IDM_VAL_SYNTAX: return "syntax";
        case IDM_VAL_CELL: return "cell";
        case IDM_VAL_CLOSURE: return "closure";
        case IDM_VAL_PID: return "pid";
        case IDM_VAL_REF: return "ref";
        case IDM_VAL_PORT: return "port";
        case IDM_VAL_PRIMITIVE: return "primitive";
        case IDM_VAL_RECORD: return "record";
        case IDM_VAL_REGEX: return "regex";
        case IDM_VAL_REGEX_RESULT: return "regex-result";
    }
    return "<bad-type>";
}

const char *idm_value_dispatch_type_name(IdmValue value) {
    if (value.tag == IDM_VAL_RECORD) return value.as.obj->as.record.type;
    return idm_value_type_name(value.tag);
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

static bool trait_method_key_eq(const char *a_trait, const char *a_method, const char *b_trait, const char *b_method) {
    return strcmp(a_trait, b_trait) == 0 && strcmp(a_method, b_method) == 0;
}

static IdmTraitWorld *trait_world(IdmRuntime *rt) {
    return &rt->trait_worlds[rt->trait_phase ? 1 : 0];
}

static IdmRuntimeTraitContract *runtime_trait_find_contract(IdmRuntime *rt, const char *trait) {
    IdmTraitWorld *w = trait_world(rt);
    for (size_t i = 0; i < w->contract_count; i++) {
        if (strcmp(w->contracts[i].name, trait) == 0) return &w->contracts[i];
    }
    return NULL;
}

static IdmRuntimeTraitMethod *runtime_trait_find_method(IdmRuntime *rt, const char *trait, const char *method) {
    IdmTraitWorld *w = trait_world(rt);
    for (size_t i = 0; i < w->method_count; i++) {
        if (trait_method_key_eq(w->methods[i].trait, w->methods[i].method, trait, method)) return &w->methods[i];
    }
    return NULL;
}

static IdmRuntimeTraitImpl *runtime_trait_find_impl(IdmRuntime *rt, const char *trait, const char *method, const char *type) {
    IdmTraitWorld *w = trait_world(rt);
    for (size_t i = 0; i < w->impl_count; i++) {
        if (strcmp(w->impls[i].type, type) == 0 && trait_method_key_eq(w->impls[i].trait, w->impls[i].method, trait, method)) return &w->impls[i];
    }
    return NULL;
}

static IdmRuntimeTraitConformance *runtime_trait_find_conformance(IdmRuntime *rt, const char *trait, const char *type) {
    IdmTraitWorld *w = trait_world(rt);
    for (size_t i = 0; i < w->conformance_count; i++) {
        if (strcmp(w->conformances[i].type, type) == 0 && strcmp(w->conformances[i].trait, trait) == 0) return &w->conformances[i];
    }
    return NULL;
}

static bool trait_contracts_reserve(IdmTraitWorld *w, size_t needed, IdmError *err) {
    if (needed <= w->contract_cap) return true;
    size_t cap = w->contract_cap ? w->contract_cap * 2u : 8u;
    while (cap < needed) cap *= 2u;
    IdmRuntimeTraitContract *next = realloc(w->contracts, cap * sizeof(*next));
    if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
    w->contracts = next;
    w->contract_cap = cap;
    return true;
}

static bool trait_methods_reserve(IdmTraitWorld *w, size_t needed, IdmError *err) {
    if (needed <= w->method_cap) return true;
    size_t cap = w->method_cap ? w->method_cap * 2u : 8u;
    while (cap < needed) cap *= 2u;
    IdmRuntimeTraitMethod *next = realloc(w->methods, cap * sizeof(*next));
    if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
    w->methods = next;
    w->method_cap = cap;
    return true;
}

static bool trait_impls_reserve(IdmTraitWorld *w, size_t needed, IdmError *err) {
    if (needed <= w->impl_cap) return true;
    size_t cap = w->impl_cap ? w->impl_cap * 2u : 8u;
    while (cap < needed) cap *= 2u;
    IdmRuntimeTraitImpl *next = realloc(w->impls, cap * sizeof(*next));
    if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
    w->impls = next;
    w->impl_cap = cap;
    return true;
}

static bool trait_conformances_reserve(IdmTraitWorld *w, size_t needed, IdmError *err) {
    if (needed <= w->conformance_cap) return true;
    size_t cap = w->conformance_cap ? w->conformance_cap * 2u : 8u;
    while (cap < needed) cap *= 2u;
    IdmRuntimeTraitConformance *next = realloc(w->conformances, cap * sizeof(*next));
    if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
    w->conformances = next;
    w->conformance_cap = cap;
    return true;
}

static size_t runtime_trait_method_count_for(IdmRuntime *rt, const char *trait) {
    IdmTraitWorld *w = trait_world(rt);
    size_t count = 0;
    for (size_t i = 0; i < w->method_count; i++) if (strcmp(w->methods[i].trait, trait) == 0) count++;
    return count;
}

static bool requirement_specs_match_contract(const IdmRuntimeTraitContract *contract, const IdmTraitRequirementSpec *requirements, size_t requirement_count) {
    if (contract->requirement_count != requirement_count) return false;
    for (size_t i = 0; i < requirement_count; i++) {
        bool found = false;
        for (size_t j = 0; j < contract->requirement_count; j++) {
            if (strcmp(contract->requirements[j], requirements[i].name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static bool runtime_trait_contract_compatible(IdmRuntime *rt, const char *trait, const IdmTraitRequirementSpec *requirements, size_t requirement_count, const IdmTraitMethodSpec *methods, size_t method_count) {
    IdmRuntimeTraitContract *contract = runtime_trait_find_contract(rt, trait);
    if (!contract || !requirement_specs_match_contract(contract, requirements, requirement_count)) return false;
    if (runtime_trait_method_count_for(rt, trait) != method_count) return false;
    for (size_t i = 0; i < method_count; i++) {
        IdmRuntimeTraitMethod *existing = runtime_trait_find_method(rt, trait, methods[i].name);
        if (!existing || existing->arity != methods[i].arity) return false;
    }
    return true;
}

static bool runtime_trait_requires(IdmRuntime *rt, const char *trait, const char *required) {
    IdmRuntimeTraitContract *contract = runtime_trait_find_contract(rt, trait);
    if (!contract) return false;
    for (size_t i = 0; i < contract->requirement_count; i++) {
        if (strcmp(contract->requirements[i], required) == 0) return true;
        if (runtime_trait_requires(rt, contract->requirements[i], required)) return true;
    }
    return false;
}

static void staged_trait_contract_destroy(IdmRuntimeTraitContract *contract) {
    runtime_trait_contract_destroy(contract);
}

static void staged_trait_methods_destroy(IdmRuntimeTraitMethod *methods, size_t count) {
    for (size_t i = 0; i < count; i++) runtime_trait_method_destroy(&methods[i]);
    free(methods);
}

bool idm_trait_define(IdmRuntime *rt, const char *trait, const IdmTraitRequirementSpec *requirements, size_t requirement_count, const IdmTraitMethodSpec *methods, size_t method_count, IdmError *err) {
    if (!trait || !*trait) return idm_error_set(err, idm_span_unknown(NULL), "trait definition requires a name");
    for (size_t i = 0; i < requirement_count; i++) {
        if (!requirements[i].name || !*requirements[i].name) return idm_error_set(err, idm_span_unknown(NULL), "trait '%s' has an unnamed requirement", trait);
        if (strcmp(requirements[i].name, trait) == 0) return idm_error_set(err, idm_span_unknown(NULL), "trait '%s' cannot require itself", trait);
        if (!runtime_trait_find_contract(rt, requirements[i].name)) return idm_error_set(err, idm_span_unknown(NULL), "trait '%s' requires undefined trait '%s'", trait, requirements[i].name);
        if (runtime_trait_requires(rt, requirements[i].name, trait)) return idm_error_set(err, idm_span_unknown(NULL), "trait requirement cycle involving '%s' and '%s'", trait, requirements[i].name);
        for (size_t j = i + 1u; j < requirement_count; j++) {
            if (strcmp(requirements[i].name, requirements[j].name) == 0) return idm_error_set(err, idm_span_unknown(NULL), "trait '%s' requires trait '%s' more than once", trait, requirements[i].name);
        }
    }
    for (size_t i = 0; i < method_count; i++) {
        if (!methods[i].name || !*methods[i].name) return idm_error_set(err, idm_span_unknown(NULL), "trait '%s' has an unnamed method", trait);
        if (methods[i].has_default && !idm_is_closure(methods[i].default_impl)) return idm_error_set(err, idm_span_unknown(NULL), "trait '%s.%s' default is not a function", trait, methods[i].name);
        for (size_t j = i + 1u; j < method_count; j++) {
            if (strcmp(methods[i].name, methods[j].name) == 0) return idm_error_set(err, idm_span_unknown(NULL), "trait '%s' declares method '%s' more than once", trait, methods[i].name);
        }
    }

    if (runtime_trait_find_contract(rt, trait)) {
        if (!runtime_trait_contract_compatible(rt, trait, requirements, requirement_count, methods, method_count)) {
            idm_error_set(err, idm_span_unknown(NULL), "trait '%s' is already defined with an incompatible contract", trait);
            return idm_error_reason(rt, err, "trait-redefinition", 1, idm_atom(rt, trait));
        }
        for (size_t i = 0; i < method_count; i++) {
            IdmRuntimeTraitMethod *existing = runtime_trait_find_method(rt, trait, methods[i].name);
            existing->has_default = methods[i].has_default;
            existing->default_impl = methods[i].has_default ? idm_value_copy(rt, &rt->immortal, methods[i].default_impl, err) : idm_nil();
            if (err->present) return false;
        }
        return true;
    }

    IdmRuntimeTraitContract staged_contract;
    memset(&staged_contract, 0, sizeof(staged_contract));
    staged_contract.name = idm_strdup(trait);
    staged_contract.requirement_count = requirement_count;
    if (!staged_contract.name) {
        staged_trait_contract_destroy(&staged_contract);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (requirement_count != 0) {
        staged_contract.requirements = calloc(requirement_count, sizeof(*staged_contract.requirements));
        if (!staged_contract.requirements) {
            staged_trait_contract_destroy(&staged_contract);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        for (size_t i = 0; i < requirement_count; i++) {
            staged_contract.requirements[i] = idm_strdup(requirements[i].name);
            if (!staged_contract.requirements[i]) {
                staged_trait_contract_destroy(&staged_contract);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
        }
    }

    IdmRuntimeTraitMethod *staged = method_count == 0 ? NULL : calloc(method_count, sizeof(*staged));
    if (method_count != 0 && !staged) {
        staged_trait_contract_destroy(&staged_contract);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t i = 0; i < method_count; i++) {
        staged[i].trait = idm_strdup(trait);
        staged[i].method = idm_strdup(methods[i].name);
        if (!staged[i].trait || !staged[i].method) {
            staged_trait_contract_destroy(&staged_contract);
            staged_trait_methods_destroy(staged, method_count);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        staged[i].arity = methods[i].arity;
        staged[i].has_default = methods[i].has_default;
        staged[i].default_impl = methods[i].has_default ? idm_value_copy(rt, &rt->immortal, methods[i].default_impl, err) : idm_nil();
        if (err->present) {
            staged_trait_contract_destroy(&staged_contract);
            staged_trait_methods_destroy(staged, method_count);
            return false;
        }
    }

    IdmTraitWorld *w = trait_world(rt);
    if (!trait_contracts_reserve(w, w->contract_count + 1u, err)) {
        staged_trait_contract_destroy(&staged_contract);
        staged_trait_methods_destroy(staged, method_count);
        return false;
    }
    if (!trait_methods_reserve(w, w->method_count + method_count, err)) {
        staged_trait_contract_destroy(&staged_contract);
        staged_trait_methods_destroy(staged, method_count);
        return false;
    }
    w->contracts[w->contract_count++] = staged_contract;
    for (size_t i = 0; i < method_count; i++) w->methods[w->method_count++] = staged[i];
    free(staged);
    return true;
}

static void staged_trait_impls_destroy(IdmRuntimeTraitImpl *impls, size_t count) {
    for (size_t i = 0; i < count; i++) runtime_trait_impl_destroy(&impls[i]);
    free(impls);
}

static void runtime_trait_remove_impls(IdmTraitWorld *w, const char *trait, const char *type) {
    size_t kept = 0;
    for (size_t i = 0; i < w->impl_count; i++) {
        if (strcmp(w->impls[i].type, type) == 0 && strcmp(w->impls[i].trait, trait) == 0) runtime_trait_impl_destroy(&w->impls[i]);
        else w->impls[kept++] = w->impls[i];
    }
    w->impl_count = kept;
}

bool idm_trait_implement(IdmRuntime *rt, const char *trait, const char *type, const char *provider, const char *provider_key, const IdmTraitImplSpec *impls, size_t impl_count, IdmError *err) {
    if (!trait || !*trait) return idm_error_set(err, idm_span_unknown(NULL), "implement requires a trait name");
    if (!type || !*type) return idm_error_set(err, idm_span_unknown(NULL), "implement requires a type name");
    if (!provider || !provider_key) return idm_error_set(err, idm_span_unknown(NULL), "implement requires a provider identity");
    IdmRuntimeTraitContract *contract = runtime_trait_find_contract(rt, trait);
    if (!contract) return idm_error_set(err, idm_span_unknown(NULL), "trait '%s' is not defined at runtime", trait);
    for (size_t i = 0; i < contract->requirement_count; i++) {
        if (!runtime_trait_find_conformance(rt, contract->requirements[i], type)) {
            return idm_error_set(err, idm_span_unknown(NULL), "type '%s' cannot implement trait '%s' before it implements required trait '%s'", type, trait, contract->requirements[i]);
        }
    }
    IdmRuntimeTraitConformance *existing = runtime_trait_find_conformance(rt, trait, type);
    if (existing && strcmp(existing->provider_key, provider_key) != 0) {
        idm_error_set(err, idm_span_unknown(NULL), "type '%s' already implements trait '%s' via '%s'; the implement in '%s' conflicts", type, trait, existing->provider, provider);
        return idm_error_reason(rt, err, "trait-implement-conflict", 4, idm_atom(rt, trait), idm_atom(rt, type), idm_atom(rt, existing->provider), idm_atom(rt, provider));
    }
    for (size_t i = 0; i < impl_count; i++) {
        if (!impls[i].name || !*impls[i].name) return idm_error_set(err, idm_span_unknown(NULL), "implement for trait '%s' has an unnamed method", trait);
        IdmRuntimeTraitMethod *method = runtime_trait_find_method(rt, trait, impls[i].name);
        if (!method) return idm_error_set(err, idm_span_unknown(NULL), "trait '%s' has no method '%s'", trait, impls[i].name);
        if (method->arity != impls[i].arity) return idm_error_set(err, idm_span_unknown(NULL), "method '%s.%s' arity mismatch: expected %u got %u", trait, impls[i].name, method->arity, impls[i].arity);
        if (!idm_is_closure(impls[i].impl)) return idm_error_set(err, idm_span_unknown(NULL), "method '%s.%s' implementation is not a function", trait, impls[i].name);
        for (size_t j = i + 1u; j < impl_count; j++) {
            if (strcmp(impls[i].name, impls[j].name) == 0) return idm_error_set(err, idm_span_unknown(NULL), "implement for '%s' provides method '%s' more than once", trait, impls[i].name);
        }
    }

    IdmRuntimeTraitImpl *staged = impl_count == 0 ? NULL : calloc(impl_count, sizeof(*staged));
    if (impl_count != 0 && !staged) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < impl_count; i++) {
        staged[i].trait = idm_strdup(trait);
        staged[i].method = idm_strdup(impls[i].name);
        staged[i].type = idm_strdup(type);
        if (!staged[i].trait || !staged[i].method || !staged[i].type) {
            staged_trait_impls_destroy(staged, impl_count);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        staged[i].impl = idm_value_copy(rt, &rt->immortal, impls[i].impl, err);
        if (err->present) {
            staged_trait_impls_destroy(staged, impl_count);
            return false;
        }
    }
    IdmTraitWorld *w = trait_world(rt);
    if (!trait_impls_reserve(w, w->impl_count + impl_count, err)) {
        staged_trait_impls_destroy(staged, impl_count);
        return false;
    }
    if (!existing && !trait_conformances_reserve(w, w->conformance_count + 1u, err)) {
        staged_trait_impls_destroy(staged, impl_count);
        return false;
    }
    if (!existing) {
        IdmRuntimeTraitConformance conf;
        memset(&conf, 0, sizeof(conf));
        conf.trait = idm_strdup(trait);
        conf.type = idm_strdup(type);
        conf.provider = idm_strdup(provider);
        conf.provider_key = idm_strdup(provider_key);
        if (!conf.trait || !conf.type || !conf.provider || !conf.provider_key) {
            runtime_trait_conformance_destroy(&conf);
            staged_trait_impls_destroy(staged, impl_count);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        w->conformances[w->conformance_count++] = conf;
    } else {
        runtime_trait_remove_impls(w, trait, type);
    }
    for (size_t i = 0; i < impl_count; i++) w->impls[w->impl_count++] = staged[i];
    free(staged);
    return true;
}

bool idm_trait_implements(IdmRuntime *rt, const char *trait, const char *type) {
    return trait && type && runtime_trait_find_conformance(rt, trait, type) != NULL;
}

bool idm_trait_lookup(IdmRuntime *rt, const char *trait, const char *method, const char *type, uint32_t argc, IdmValue *out_impl, IdmError *err) {
    IdmRuntimeTraitMethod *contract = runtime_trait_find_method(rt, trait, method);
    if (!contract) return idm_error_set(err, idm_span_unknown(NULL), "trait method '%s.%s' is not defined", trait, method);
    if (contract->arity != argc) return idm_error_set(err, idm_span_unknown(NULL), "method '%s.%s' arity mismatch: expected %u got %u", trait, method, contract->arity, argc);
    IdmRuntimeTraitImpl *impl = runtime_trait_find_impl(rt, trait, method, type);
    if (impl) {
        *out_impl = impl->impl;
        return true;
    }
    if (!runtime_trait_find_conformance(rt, trait, type)) {
        return idm_error_set(err, idm_span_unknown(NULL), "type '%s' does not implement trait '%s'", type, trait);
    }
    if (contract->has_default) {
        *out_impl = contract->default_impl;
        return true;
    }
    return idm_error_set(err, idm_span_unknown(NULL), "no implementation for method '%s.%s' on type '%s'", trait, method, type);
}

bool idm_is_nil(IdmValue value) {
    return value.tag == IDM_VAL_NIL;
}

bool idm_is_empty_list(IdmValue value) {
    return value.tag == IDM_VAL_EMPTY_LIST;
}

bool idm_is_pair(IdmValue value) {
    return value.tag == IDM_VAL_PAIR;
}

bool idm_is_tuple(IdmValue value) {
    return value.tag == IDM_VAL_TUPLE;
}

bool idm_is_vector(IdmValue value) {
    return value.tag == IDM_VAL_VECTOR;
}

bool idm_is_dict(IdmValue value) {
    return value.tag == IDM_VAL_DICT;
}

bool idm_is_syntax(IdmValue value) {
    return value.tag == IDM_VAL_SYNTAX;
}

bool idm_is_string(IdmValue value) {
    return value.tag == IDM_VAL_STRING;
}

bool idm_is_cell(IdmValue value) {
    return value.tag == IDM_VAL_CELL;
}

bool idm_is_closure(IdmValue value) {
    return value.tag == IDM_VAL_CLOSURE;
}

bool idm_is_record(IdmValue value) {
    return value.tag == IDM_VAL_RECORD;
}

bool idm_is_regex(IdmValue value) {
    return value.tag == IDM_VAL_REGEX;
}

bool idm_is_regex_result(IdmValue value) {
    return value.tag == IDM_VAL_REGEX_RESULT;
}

static pthread_mutex_t g_cell_mu = PTHREAD_MUTEX_INITIALIZER;

static IdmValue cell_get_unlocked(IdmValue cell, IdmError *err) {
    if (cell.tag != IDM_VAL_CELL) {
        idm_error_set(err, idm_span_unknown(NULL), "expected cell");
        return idm_nil();
    }
    return cell.as.obj->as.cell.value;
}
IdmValue idm_cell_get(IdmValue cell, IdmError *err) {
    pthread_mutex_lock(&g_cell_mu);
    IdmValue out = cell_get_unlocked(cell, err);
    pthread_mutex_unlock(&g_cell_mu);
    return out;
}

static bool cell_set_unlocked(IdmValue cell, IdmValue value, IdmError *err) {
    if (cell.tag != IDM_VAL_CELL) return idm_error_set(err, idm_span_unknown(NULL), "expected cell");
    cell.as.obj->as.cell.value = value;
    return true;
}
bool idm_cell_set(IdmValue cell, IdmValue value, IdmError *err) {
    pthread_mutex_lock(&g_cell_mu);
    bool ok = cell_set_unlocked(cell, value, err);
    pthread_mutex_unlock(&g_cell_mu);
    return ok;
}


uint32_t idm_closure_function_index(IdmValue value) {
    return value.tag == IDM_VAL_CLOSURE ? value.as.obj->as.closure.function_index : UINT32_MAX;
}

const IdmBytecodeModule *idm_closure_module(IdmValue value) {
    return value.tag == IDM_VAL_CLOSURE ? value.as.obj->as.closure.module : NULL;
}

size_t idm_closure_entry_count(IdmValue value) {
    return value.tag == IDM_VAL_CLOSURE ? value.as.obj->as.closure.entry_count : 0;
}

uint32_t idm_closure_entry(IdmValue value, size_t index, IdmError *err) {
    if (value.tag != IDM_VAL_CLOSURE) {
        idm_error_set(err, idm_span_unknown(NULL), "expected closure");
        return UINT32_MAX;
    }
    if (index >= value.as.obj->as.closure.entry_count) {
        idm_error_set(err, idm_span_unknown(NULL), "closure entry index out of bounds");
        return UINT32_MAX;
    }
    if (value.as.obj->as.closure.entry_count == 1) return value.as.obj->as.closure.function_index;
    return value.as.obj->as.closure.entries[index];
}

size_t idm_closure_capture_count(IdmValue value) {
    return value.tag == IDM_VAL_CLOSURE ? value.as.obj->as.closure.capture_count : 0;
}

IdmNamespace *idm_closure_namespace(IdmValue value) {
    return value.tag == IDM_VAL_CLOSURE ? value.as.obj->as.closure.ns : NULL;
}

IdmValue idm_closure_capture(IdmValue value, size_t index, IdmError *err) {
    if (value.tag != IDM_VAL_CLOSURE) {
        idm_error_set(err, idm_span_unknown(NULL), "expected closure");
        return idm_nil();
    }
    if (index >= value.as.obj->as.closure.capture_count) {
        idm_error_set(err, idm_span_unknown(NULL), "closure capture index out of bounds");
        return idm_nil();
    }
    return value.as.obj->as.closure.captures[index];
}

const char *idm_record_type(IdmValue value, IdmError *err) {
    if (value.tag != IDM_VAL_RECORD) {
        idm_error_set(err, idm_span_unknown(NULL), "expected record");
        return NULL;
    }
    return value.as.obj->as.record.type;
}

IdmValue idm_record_fields(IdmValue value, IdmError *err) {
    if (value.tag != IDM_VAL_RECORD) {
        idm_error_set(err, idm_span_unknown(NULL), "expected record");
        return idm_nil();
    }
    return value.as.obj->as.record.fields;
}

static const char *record_field_name(IdmValue field) {
    if (field.tag == IDM_VAL_ATOM || field.tag == IDM_VAL_WORD) return idm_symbol_text(field.as.symbol);
    if (field.tag == IDM_VAL_STRING) return idm_string_bytes(field);
    return NULL;
}

bool idm_record_field(IdmValue value, IdmValue field, IdmValue *out, IdmError *err) {
    if (value.tag != IDM_VAL_RECORD) return idm_error_set(err, idm_span_unknown(NULL), "record-field expects a record");
    const char *name = record_field_name(field);
    if (!name) return idm_error_set(err, idm_span_unknown(NULL), "record field name must be an atom, word, or string");
    if (idm_dict_get(value.as.obj->as.record.fields, field, out)) return true;
    return idm_error_set(err, idm_span_unknown(NULL), "record '%s' has no field '%s'", value.as.obj->as.record.type, name);
}

IdmRegex *idm_regex_value_get(IdmValue value, IdmError *err) {
    if (value.tag != IDM_VAL_REGEX) {
        idm_error_set(err, idm_span_unknown(NULL), "expected regex");
        return NULL;
    }
    return value.as.obj->as.regex;
}

IdmRegexResult *idm_regex_result_value_get(IdmValue value, IdmError *err) {
    if (value.tag != IDM_VAL_REGEX_RESULT) {
        idm_error_set(err, idm_span_unknown(NULL), "expected regex result");
        return NULL;
    }
    return value.as.obj->as.regex_result;
}

IdmValue idm_car(IdmValue pair, IdmError *err) {
    if (pair.tag != IDM_VAL_PAIR) {
        idm_error_set(err, idm_span_unknown(NULL), "first expects a pair");
        return idm_nil();
    }
    return pair.as.obj->as.pair.car;
}

IdmValue idm_cdr(IdmValue pair, IdmError *err) {
    if (pair.tag != IDM_VAL_PAIR) {
        idm_error_set(err, idm_span_unknown(NULL), "rest expects a pair");
        return idm_nil();
    }
    return pair.as.obj->as.pair.cdr;
}

size_t idm_sequence_count(IdmValue value) {
    if (value.tag != IDM_VAL_TUPLE && value.tag != IDM_VAL_VECTOR) return 0;
    return value.as.obj->as.sequence.count;
}

IdmValue idm_sequence_item(IdmValue value, size_t index, IdmError *err) {
    if (value.tag != IDM_VAL_TUPLE && value.tag != IDM_VAL_VECTOR) {
        idm_error_set(err, idm_span_unknown(NULL), "expected tuple or vector");
        return idm_nil();
    }
    if (index >= value.as.obj->as.sequence.count) {
        idm_error_set(err, idm_span_unknown(NULL), "sequence index out of bounds");
        return idm_nil();
    }
    return value.as.obj->as.sequence.items[index];
}

bool idm_value_is_error(IdmValue value) {
    if (value.tag != IDM_VAL_TUPLE || value.as.obj->as.sequence.count == 0) return false;
    IdmValue tag = value.as.obj->as.sequence.items[0];
    return tag.tag == IDM_VAL_ATOM && strcmp(idm_symbol_text(tag.as.symbol), "error") == 0;
}

bool idm_value_ok(IdmValue value) {
    if (value.tag == IDM_VAL_NIL) return false;
    if (value.tag == IDM_VAL_ATOM) {
        const char *text = idm_symbol_text(value.as.symbol);
        if (strcmp(text, "false") == 0 || strcmp(text, "nil") == 0) return false;
    }
    if (idm_value_is_error(value)) return false;
    return true;
}

size_t idm_dict_count(IdmValue value) {
    return value.tag == IDM_VAL_DICT ? value.as.obj->as.dict.count : 0;
}

bool idm_dict_get(IdmValue dict, IdmValue key, IdmValue *out) {
    if (dict.tag != IDM_VAL_DICT) return false;
    for (size_t i = 0; i < dict.as.obj->as.dict.count; i++) {
        if (idm_value_equal(dict.as.obj->as.dict.entries[i].key, key)) {
            *out = dict.as.obj->as.dict.entries[i].value;
            return true;
        }
    }
    return false;
}

bool idm_dict_entry(IdmValue dict, size_t index, IdmValue *out_key, IdmValue *out_value) {
    if (dict.tag != IDM_VAL_DICT || index >= dict.as.obj->as.dict.count) return false;
    *out_key = dict.as.obj->as.dict.entries[index].key;
    *out_value = dict.as.obj->as.dict.entries[index].value;
    return true;
}

const IdmSyntax *idm_syntax_get(IdmValue value, IdmError *err) {
    if (value.tag != IDM_VAL_SYNTAX) {
        idm_error_set(err, idm_span_unknown(NULL), "expected syntax");
        return NULL;
    }
    return value.as.obj->as.syntax;
}

IdmSyntax *idm_syntax_value_get(IdmValue value) {
    return value.tag == IDM_VAL_SYNTAX ? value.as.obj->as.syntax : NULL;
}

const char *idm_string_bytes(IdmValue value) {
    return value.tag == IDM_VAL_STRING ? value.as.obj->as.string.bytes : "";
}

size_t idm_string_length(IdmValue value) {
    return value.tag == IDM_VAL_STRING ? value.as.obj->as.string.len : 0;
}

bool idm_value_equal(IdmValue a, IdmValue b) {
    if (a.tag != b.tag) return false;
    switch (a.tag) {
        case IDM_VAL_NIL:
        case IDM_VAL_EMPTY_LIST: return true;
        case IDM_VAL_ATOM:
        case IDM_VAL_WORD: return a.as.symbol == b.as.symbol;
        case IDM_VAL_INT: return a.as.i == b.as.i;
        case IDM_VAL_FLOAT: return a.as.f == b.as.f;
        case IDM_VAL_STRING:
            return a.as.obj->as.string.len == b.as.obj->as.string.len &&
                   memcmp(a.as.obj->as.string.bytes, b.as.obj->as.string.bytes, a.as.obj->as.string.len) == 0;
        case IDM_VAL_PAIR:
            return idm_value_equal(a.as.obj->as.pair.car, b.as.obj->as.pair.car) &&
                   idm_value_equal(a.as.obj->as.pair.cdr, b.as.obj->as.pair.cdr);
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR:
            if (a.as.obj->as.sequence.count != b.as.obj->as.sequence.count) return false;
            for (size_t i = 0; i < a.as.obj->as.sequence.count; i++) {
                if (!idm_value_equal(a.as.obj->as.sequence.items[i], b.as.obj->as.sequence.items[i])) return false;
            }
            return true;
        case IDM_VAL_DICT:
            if (a.as.obj->as.dict.count != b.as.obj->as.dict.count) return false;
            for (size_t i = 0; i < a.as.obj->as.dict.count; i++) {
                IdmValue other;
                if (!idm_dict_get(b, a.as.obj->as.dict.entries[i].key, &other)) return false;
                if (!idm_value_equal(a.as.obj->as.dict.entries[i].value, other)) return false;
            }
            return true;
        case IDM_VAL_SYNTAX:
            return a.as.obj == b.as.obj;
        case IDM_VAL_CELL:
            return a.as.obj == b.as.obj;
        case IDM_VAL_CLOSURE:
            return a.as.obj == b.as.obj;
        case IDM_VAL_RECORD:
            return strcmp(a.as.obj->as.record.type, b.as.obj->as.record.type) == 0 &&
                   idm_value_equal(a.as.obj->as.record.fields, b.as.obj->as.record.fields);
        case IDM_VAL_REGEX:
            return a.as.obj == b.as.obj;
        case IDM_VAL_REGEX_RESULT:
            return a.as.obj == b.as.obj;
        case IDM_VAL_PID:
        case IDM_VAL_REF:
        case IDM_VAL_PORT:
        case IDM_VAL_PRIMITIVE:
            return a.as.id == b.as.id;
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
    switch (value.tag) {
        case IDM_VAL_NIL: return idm_buf_append(buf, ":nil");
        case IDM_VAL_EMPTY_LIST: return idm_buf_append(buf, "()");
        case IDM_VAL_ATOM: return idm_buf_append_char(buf, ':') && idm_buf_append(buf, idm_symbol_text(value.as.symbol));
        case IDM_VAL_WORD: return idm_buf_append(buf, idm_symbol_text(value.as.symbol));
        case IDM_VAL_INT: return idm_buf_appendf(buf, "%lld", (long long)value.as.i);
        case IDM_VAL_FLOAT: {
            char text[40];
            snprintf(text, sizeof(text), "%g", value.as.f);
            return idm_buf_append(buf, text) && (strpbrk(text, ".eEn") != NULL || idm_buf_append(buf, ".0"));
        }
        case IDM_VAL_STRING: return write_escaped(buf, value.as.obj->as.string.bytes, value.as.obj->as.string.len);
        case IDM_VAL_PID: return idm_buf_appendf(buf, "#<pid:%llu>", (unsigned long long)value.as.id);
        case IDM_VAL_REF: return idm_buf_appendf(buf, "#<ref:%llu>", (unsigned long long)value.as.id);
        case IDM_VAL_PORT: return idm_buf_appendf(buf, "#<port:%llu>", (unsigned long long)value.as.id);
        case IDM_VAL_PRIMITIVE: return idm_buf_appendf(buf, "#<primitive:%llu>", (unsigned long long)value.as.id);
        case IDM_VAL_RECORD: return idm_buf_appendf(buf, "#<record:%s>", value.as.obj->as.record.type);
        case IDM_VAL_REGEX: {
            size_t len = 0;
            const char *source = idm_regex_source(value.as.obj->as.regex, &len);
            return idm_buf_append_char(buf, 'r') && write_escaped(buf, source, len);
        }
        case IDM_VAL_REGEX_RESULT:
            return idm_buf_append(buf, "#<regex-result>");
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR: {
            const char *open = value.tag == IDM_VAL_TUPLE ? "{" : "[";
            const char *close = value.tag == IDM_VAL_TUPLE ? "}" : "]";
            if (!idm_buf_append(buf, open)) return false;
            for (size_t i = 0; i < value.as.obj->as.sequence.count; i++) {
                if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
                if (!idm_value_write(buf, value.as.obj->as.sequence.items[i])) return false;
            }
            return idm_buf_append(buf, close);
        }
        case IDM_VAL_DICT: {
            if (!idm_buf_append(buf, "%{")) return false;
            for (size_t i = 0; i < value.as.obj->as.dict.count; i++) {
                if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
                if (!idm_value_write(buf, value.as.obj->as.dict.entries[i].key)) return false;
                if (!idm_buf_append_char(buf, ' ')) return false;
                if (!idm_value_write(buf, value.as.obj->as.dict.entries[i].value)) return false;
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
            while (cur.tag == IDM_VAL_PAIR) {
                if (!first && !idm_buf_append_char(buf, ' ')) return false;
                if (!idm_value_write(buf, cur.as.obj->as.pair.car)) return false;
                cur = cur.as.obj->as.pair.cdr;
                first = false;
            }
            if (cur.tag != IDM_VAL_EMPTY_LIST) {
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
    if (!value_is_heap_obj(in.tag)) { *out = in; return true; }
    IdmObject *src = in.as.obj;
    if (!src || src->heap == &rt->immortal) { *out = in; return true; }
    IdmObject *existing = copymap_find(map, src);
    if (existing) { out->tag = in.tag; out->as.obj = existing; return true; }
    IdmObject *dst = heap_alloc_unlocked(target, src->kind);
    if (!dst) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!copymap_put(map, src, dst)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!copystack_push(stack, src, dst)) return idm_error_oom(err, idm_span_unknown(NULL));
    out->tag = in.tag;
    out->as.obj = dst;
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
            dst->as.closure.ns = src->as.closure.ns;
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
            return copy_intern(rt, target, src->as.record.fields, &dst->as.record.fields, map, stack, err);
        case IDM_OBJ_SYNTAX:
            dst->as.syntax = idm_syn_clone(src->as.syntax);
            if (!dst->as.syntax && src->as.syntax) return idm_error_oom(err, idm_span_unknown(NULL));
            return true;
        case IDM_OBJ_REGEX:
            dst->as.regex = idm_regex_clone(src->as.regex, err);
            return !(err && err->present);
        case IDM_OBJ_REGEX_RESULT:
            dst->as.regex_result = idm_regex_result_clone(src->as.regex_result);
            if (!dst->as.regex_result && src->as.regex_result) return idm_error_oom(err, idm_span_unknown(NULL));
            heap_account_unlocked(target, dst, idm_regex_result_footprint(dst->as.regex_result));
            return true;
    }
    return idm_error_set(err, idm_span_unknown(NULL), "value copy: unknown object kind");
}

IdmValue idm_value_copy_locked(IdmRuntime *rt, IdmHeap *target, IdmValue value, IdmError *err) {
    if (!value_is_heap_obj(value.tag) || !value.as.obj || value.as.obj->heap == &rt->immortal) return value;
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
    pthread_mutex_lock(&target->lock);
    IdmValue out = idm_value_copy_locked(rt, target, value, err);
    pthread_mutex_unlock(&target->lock);
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
    if (detail.tag != IDM_VAL_TUPLE) return false;
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
    if (v.tag == IDM_VAL_ATOM) idm_buf_append(out, idm_symbol_text(v.as.symbol));
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
    if (detail.tag == IDM_VAL_STRING) { idm_buf_append(out, idm_string_bytes(detail)); return; }
    if (!idm_is_tuple(detail) || idm_sequence_count(detail) == 0 ||
        idm_sequence_item(detail, 0, NULL).tag != IDM_VAL_ATOM) {
        idm_buf_append(out, "error: ");
        idm_value_write(out, detail);
        return;
    }
    const char *kind = idm_symbol_text(idm_sequence_item(detail, 0, NULL).as.symbol);
    size_t n = idm_sequence_count(detail);
    if (strcmp(kind, "runtime") == 0 && n >= 2 && idm_sequence_item(detail, 1, NULL).tag == IDM_VAL_STRING) {
        idm_buf_append(out, idm_string_bytes(idm_sequence_item(detail, 1, NULL)));
        return;
    }
    if (strcmp(kind, "no-clause") == 0 && n >= 3) {
        IdmValue nm = idm_sequence_item(detail, 1, NULL);
        bool anon = nm.tag == IDM_VAL_ATOM && strcmp(idm_symbol_text(nm.as.symbol), "fn") == 0;
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
