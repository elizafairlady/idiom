#include "idiom/value.h"

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
    IDM_OBJ_RECORD
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
    bool marked;
    size_t bytes;
    struct IdmObject *next;
    union {
        IdmStringObj string;
        IdmPairObj pair;
        IdmSequenceObj sequence;
        IdmDictObj dict;
        IdmSyntax *syntax;
        IdmCellObj cell;
        IdmClosureObj closure;
        IdmRecordObj record;
    } as;
};

struct IdmSymbol {
    char *text;
    uint32_t id;
    IdmSymbolKind kind;
};

static pthread_mutex_t g_alloc_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_intern_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_ns_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_gcreg_mu = PTHREAD_MUTEX_INITIALIZER;

static IdmSymbol *idm_intern_unlocked(IdmIntern *intern, IdmSymbolKind kind, const char *text);
static IdmNamespace *namespace_get_or_create_unlocked(IdmRuntime *rt, const char *name);
static bool register_gc_module_unlocked(IdmRuntime *rt, const IdmBytecodeModule *module);

static IdmObject *heap_alloc_unlocked(IdmHeap *heap, IdmObjectKind kind) {
    IdmObject *obj = calloc(1u, sizeof(*obj));
    if (!obj) return NULL;
    obj->kind = kind;
    obj->marked = false;
    obj->bytes = sizeof(*obj);
    obj->next = heap->objects;
    heap->objects = obj;
    heap->object_count++;
    heap->bytes_allocated += sizeof(*obj);
    return obj;
}

static IdmObject *heap_alloc(IdmHeap *heap, IdmObjectKind kind) {
    pthread_mutex_lock(&g_alloc_mu);
    IdmObject *obj = heap_alloc_unlocked(heap, kind);
    pthread_mutex_unlock(&g_alloc_mu);
    return obj;
}

static void heap_account(IdmHeap *heap, IdmObject *obj, size_t extra) {
    pthread_mutex_lock(&g_alloc_mu);
    obj->bytes += extra;
    heap->bytes_allocated += extra;
    pthread_mutex_unlock(&g_alloc_mu);
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
    free(obj);
}

void idm_runtime_init(IdmRuntime *rt) {
    idm_intern_init(&rt->intern);
    idm_heap_init(&rt->heap);
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
    memset(rt->protocol_worlds, 0, sizeof(rt->protocol_worlds));
    rt->protocol_phase = 0;
    rt->gc_modules = NULL;
    rt->gc_module_count = 0;
    rt->gc_module_cap = 0;
    rt->gc_values = NULL;
    rt->gc_value_count = 0;
    rt->gc_value_cap = 0;
    rt->expand_cache = NULL;
    rt->expand_cache_free = NULL;
    rt->owned_temps = NULL;
    rt->owned_temp_count = 0;
    rt->owned_temp_cap = 0;
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

bool idm_runtime_register_gc_module(IdmRuntime *rt, const IdmBytecodeModule *module) {
    pthread_mutex_lock(&g_gcreg_mu);
    bool ok = register_gc_module_unlocked(rt, module);
    pthread_mutex_unlock(&g_gcreg_mu);
    return ok;
}

static bool register_gc_module_unlocked(IdmRuntime *rt, const IdmBytecodeModule *module) {
    if (!module) return false;
    if (rt->gc_module_count == rt->gc_module_cap) {
        size_t cap = rt->gc_module_cap ? rt->gc_module_cap * 2u : 16u;
        const IdmBytecodeModule **grown = realloc(rt->gc_modules, cap * sizeof(*grown));
        if (!grown) return false;
        rt->gc_modules = grown;
        rt->gc_module_cap = cap;
    }
    rt->gc_modules[rt->gc_module_count++] = module;
    return true;
}

void idm_runtime_unregister_gc_module(IdmRuntime *rt, const IdmBytecodeModule *module) {
    for (size_t i = 0; i < rt->gc_module_count; i++) {
        if (rt->gc_modules[i] == module) {
            rt->gc_modules[i] = rt->gc_modules[rt->gc_module_count - 1u];
            rt->gc_module_count--;
            return;
        }
    }
}

bool idm_runtime_register_gc_value(IdmRuntime *rt, IdmValue value) {
    if (rt->gc_value_count == rt->gc_value_cap) {
        size_t cap = rt->gc_value_cap ? rt->gc_value_cap * 2u : 8u;
        IdmValue *grown = realloc(rt->gc_values, cap * sizeof(*grown));
        if (!grown) return false;
        rt->gc_values = grown;
        rt->gc_value_cap = cap;
    }
    rt->gc_values[rt->gc_value_count++] = value;
    return true;
}

void idm_runtime_unregister_gc_value(IdmRuntime *rt, IdmValue value) {
    for (size_t i = 0; i < rt->gc_value_count; i++) {
        if (rt->gc_values[i].tag == value.tag && rt->gc_values[i].as.obj == value.as.obj) {
            rt->gc_values[i] = rt->gc_values[rt->gc_value_count - 1u];
            rt->gc_value_count--;
            return;
        }
    }
}

static void runtime_protocol_method_destroy(IdmRuntimeProtocolMethod *method) {
    if (!method) return;
    free(method->protocol);
    free(method->method);
    memset(method, 0, sizeof(*method));
}

static void runtime_protocol_impl_destroy(IdmRuntimeProtocolImpl *impl) {
    if (!impl) return;
    free(impl->protocol);
    free(impl->method);
    free(impl->type);
    memset(impl, 0, sizeof(*impl));
}

static void runtime_protocol_conformance_destroy(IdmRuntimeProtocolConformance *conf) {
    if (!conf) return;
    free(conf->protocol);
    free(conf->type);
    memset(conf, 0, sizeof(*conf));
}

void idm_runtime_destroy(IdmRuntime *rt) {
    for (size_t w = 0; w < 2u; w++) {
        IdmProtocolWorld *world = &rt->protocol_worlds[w];
        for (size_t i = 0; i < world->method_count; i++) runtime_protocol_method_destroy(&world->methods[i]);
        free(world->methods);
        for (size_t i = 0; i < world->impl_count; i++) runtime_protocol_impl_destroy(&world->impls[i]);
        free(world->impls);
        for (size_t i = 0; i < world->conformance_count; i++) runtime_protocol_conformance_destroy(&world->conformances[i]);
        free(world->conformances);
    }
    memset(rt->protocol_worlds, 0, sizeof(rt->protocol_worlds));
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
    free(rt->gc_modules);
    rt->gc_modules = NULL;
    rt->gc_module_count = 0;
    rt->gc_module_cap = 0;
    free(rt->gc_values);
    rt->gc_values = NULL;
    rt->gc_value_count = 0;
    rt->gc_value_cap = 0;
    idm_heap_destroy(&rt->heap);
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
}

void idm_intern_destroy(IdmIntern *intern) {
    if (!intern) return;
    for (size_t i = 0; i < intern->count; i++) { free(intern->symbols[i]->text); free(intern->symbols[i]); }
    free(intern->symbols);
    intern->symbols = NULL;
    intern->count = 0;
    intern->cap = 0;
    intern->next_id = 1u;
}

IdmSymbol *idm_intern(IdmIntern *intern, IdmSymbolKind kind, const char *text) {
    pthread_mutex_lock(&g_intern_mu);
    IdmSymbol *sym = idm_intern_unlocked(intern, kind, text);
    pthread_mutex_unlock(&g_intern_mu);
    return sym;
}

static IdmSymbol *idm_intern_unlocked(IdmIntern *intern, IdmSymbolKind kind, const char *text) {
    for (size_t i = 0; i < intern->count; i++) {
        if (intern->symbols[i]->kind == kind && strcmp(intern->symbols[i]->text, text) == 0) return intern->symbols[i];
    }
    if (intern->count == intern->cap) {
        size_t cap = intern->cap ? intern->cap * 2u : 32u;
        IdmSymbol **next = realloc(intern->symbols, cap * sizeof(*next));
        if (!next) return NULL;
        intern->symbols = next;
        intern->cap = cap;
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
    intern->symbols[intern->count++] = sym;
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
    heap->object_count = 0;
    heap->bytes_allocated = 0;
}

void idm_heap_destroy(IdmHeap *heap) {
    IdmObject *obj = heap->objects;
    while (obj) {
        IdmObject *next = obj->next;
        object_free(obj);
        obj = next;
    }
    heap->objects = NULL;
    heap->object_count = 0;
    heap->bytes_allocated = 0;
}

static void value_mark(IdmValue value) {
    if (value.tag != IDM_VAL_STRING && value.tag != IDM_VAL_PAIR && value.tag != IDM_VAL_TUPLE && value.tag != IDM_VAL_VECTOR && value.tag != IDM_VAL_DICT && value.tag != IDM_VAL_SYNTAX && value.tag != IDM_VAL_CELL && value.tag != IDM_VAL_CLOSURE && value.tag != IDM_VAL_RECORD) return;
    IdmObject *obj = value.as.obj;
    if (!obj || obj->marked) return;
    obj->marked = true;
    if (obj->kind == IDM_OBJ_PAIR) {
        value_mark(obj->as.pair.car);
        value_mark(obj->as.pair.cdr);
    } else if (obj->kind == IDM_OBJ_TUPLE || obj->kind == IDM_OBJ_VECTOR) {
        for (size_t i = 0; i < obj->as.sequence.count; i++) value_mark(obj->as.sequence.items[i]);
    } else if (obj->kind == IDM_OBJ_DICT) {
        for (size_t i = 0; i < obj->as.dict.count; i++) {
            value_mark(obj->as.dict.entries[i].key);
            value_mark(obj->as.dict.entries[i].value);
        }
    } else if (obj->kind == IDM_OBJ_CELL) {
        value_mark(obj->as.cell.value);
    } else if (obj->kind == IDM_OBJ_CLOSURE) {
        for (size_t i = 0; i < obj->as.closure.capture_count; i++) value_mark(obj->as.closure.captures[i]);
    } else if (obj->kind == IDM_OBJ_RECORD) {
        value_mark(obj->as.record.fields);
    }
}

void idm_gc_mark_value(IdmValue value) {
    value_mark(value);
}

void idm_heap_sweep(IdmHeap *heap) {
    IdmObject **cursor = &heap->objects;
    while (*cursor) {
        IdmObject *obj = *cursor;
        if (obj->marked) {
            obj->marked = false;
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
    pthread_mutex_lock(&g_alloc_mu);
    size_t n = heap->bytes_allocated;
    pthread_mutex_unlock(&g_alloc_mu);
    return n;
}


void idm_heap_collect(IdmHeap *heap, const IdmValue *roots, size_t root_count) {
    for (size_t i = 0; i < root_count; i++) value_mark(roots[i]);
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

IdmValue idm_string_n(IdmRuntime *rt, const char *text, size_t len, IdmError *err) {
    IdmObject *obj = heap_alloc(&rt->heap, IDM_OBJ_STRING);
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
    heap_account(&rt->heap, obj, len + 1u);
    IdmValue v;
    v.tag = IDM_VAL_STRING;
    v.as.obj = obj;
    return v;
}

IdmValue idm_string(IdmRuntime *rt, const char *text, IdmError *err) {
    return idm_string_n(rt, text, strlen(text), err);
}

IdmValue idm_cons(IdmRuntime *rt, IdmValue car, IdmValue cdr, IdmError *err) {
    IdmObject *obj = heap_alloc(&rt->heap, IDM_OBJ_PAIR);
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
    IdmObject *obj = heap_alloc(&rt->heap, obj_kind);
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
        heap_account(&rt->heap, obj, count * sizeof(*items));
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
    IdmObject *obj = heap_alloc(&rt->heap, IDM_OBJ_DICT);
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
        heap_account(&rt->heap, obj, count * sizeof(*entries));
    }
    IdmValue v;
    v.tag = IDM_VAL_DICT;
    v.as.obj = obj;
    return v;
}

IdmValue idm_syntax_value(IdmRuntime *rt, const IdmSyntax *syntax, IdmError *err) {
    IdmObject *obj = heap_alloc(&rt->heap, IDM_OBJ_SYNTAX);
    if (!obj) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return idm_nil();
    }
    obj->as.syntax = idm_syn_clone(syntax);
    if (!obj->as.syntax) {
        idm_error_oom(err, syntax ? syntax->span : idm_span_unknown(NULL));
        return idm_nil();
    }
    heap_account(&rt->heap, obj, syn_footprint(obj->as.syntax));
    IdmValue v;
    v.tag = IDM_VAL_SYNTAX;
    v.as.obj = obj;
    return v;
}

IdmValue idm_cell(IdmRuntime *rt, IdmValue initial, IdmError *err) {
    IdmObject *obj = heap_alloc(&rt->heap, IDM_OBJ_CELL);
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
    IdmObject *obj = heap_alloc(&rt->heap, IDM_OBJ_CLOSURE);
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
        heap_account(&rt->heap, obj, function_count * sizeof(*function_indexes));
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
        heap_account(&rt->heap, obj, capture_count * sizeof(*captures));
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
    IdmObject *obj = heap_alloc(&rt->heap, IDM_OBJ_RECORD);
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
    heap_account(&rt->heap, obj, strlen(obj->as.record.type) + 1u);
    IdmValue v;
    v.tag = IDM_VAL_RECORD;
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
    }
    return "<bad-type>";
}

const char *idm_value_dispatch_type_name(IdmValue value) {
    if (value.tag == IDM_VAL_RECORD) return value.as.obj->as.record.type;
    return idm_value_type_name(value.tag);
}

bool idm_value_type_from_name(const char *name, IdmValueTag *out) {
    if (!name) return false;
    for (int i = (int)IDM_VAL_NIL; i <= (int)IDM_VAL_PRIMITIVE; i++) {
        IdmValueTag tag = (IdmValueTag)i;
        if (strcmp(name, idm_value_type_name(tag)) == 0) {
            if (out) *out = tag;
            return true;
        }
    }
    return false;
}

static bool protocol_name_eq(const char *a_protocol, const char *a_method, const char *b_protocol, const char *b_method) {
    return strcmp(a_protocol, b_protocol) == 0 && strcmp(a_method, b_method) == 0;
}

static IdmProtocolWorld *protocol_world(IdmRuntime *rt) {
    return &rt->protocol_worlds[rt->protocol_phase ? 1 : 0];
}

static IdmRuntimeProtocolMethod *runtime_protocol_find_method(IdmRuntime *rt, const char *protocol, const char *method) {
    IdmProtocolWorld *w = protocol_world(rt);
    for (size_t i = 0; i < w->method_count; i++) {
        if (protocol_name_eq(w->methods[i].protocol, w->methods[i].method, protocol, method)) return &w->methods[i];
    }
    return NULL;
}

static IdmRuntimeProtocolImpl *runtime_protocol_find_impl(IdmRuntime *rt, const char *protocol, const char *method, const char *type) {
    IdmProtocolWorld *w = protocol_world(rt);
    for (size_t i = 0; i < w->impl_count; i++) {
        if (strcmp(w->impls[i].type, type) == 0 && protocol_name_eq(w->impls[i].protocol, w->impls[i].method, protocol, method)) return &w->impls[i];
    }
    return NULL;
}

static IdmRuntimeProtocolConformance *runtime_protocol_find_conformance(IdmRuntime *rt, const char *protocol, const char *type) {
    IdmProtocolWorld *w = protocol_world(rt);
    for (size_t i = 0; i < w->conformance_count; i++) {
        if (strcmp(w->conformances[i].type, type) == 0 && strcmp(w->conformances[i].protocol, protocol) == 0) return &w->conformances[i];
    }
    return NULL;
}

static bool protocol_methods_reserve(IdmProtocolWorld *w, size_t needed, IdmError *err) {
    if (needed <= w->method_cap) return true;
    size_t cap = w->method_cap ? w->method_cap * 2u : 8u;
    while (cap < needed) cap *= 2u;
    IdmRuntimeProtocolMethod *next = realloc(w->methods, cap * sizeof(*next));
    if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
    w->methods = next;
    w->method_cap = cap;
    return true;
}

static bool protocol_impls_reserve(IdmProtocolWorld *w, size_t needed, IdmError *err) {
    if (needed <= w->impl_cap) return true;
    size_t cap = w->impl_cap ? w->impl_cap * 2u : 8u;
    while (cap < needed) cap *= 2u;
    IdmRuntimeProtocolImpl *next = realloc(w->impls, cap * sizeof(*next));
    if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
    w->impls = next;
    w->impl_cap = cap;
    return true;
}

static bool protocol_conformances_reserve(IdmProtocolWorld *w, size_t needed, IdmError *err) {
    if (needed <= w->conformance_cap) return true;
    size_t cap = w->conformance_cap ? w->conformance_cap * 2u : 8u;
    while (cap < needed) cap *= 2u;
    IdmRuntimeProtocolConformance *next = realloc(w->conformances, cap * sizeof(*next));
    if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
    w->conformances = next;
    w->conformance_cap = cap;
    return true;
}

static size_t runtime_protocol_method_count_for(IdmRuntime *rt, const char *protocol) {
    IdmProtocolWorld *w = protocol_world(rt);
    size_t count = 0;
    for (size_t i = 0; i < w->method_count; i++) if (strcmp(w->methods[i].protocol, protocol) == 0) count++;
    return count;
}

static bool runtime_protocol_contract_compatible(IdmRuntime *rt, const char *protocol, const IdmProtocolMethodSpec *methods, size_t method_count) {
    if (runtime_protocol_method_count_for(rt, protocol) != method_count) return false;
    for (size_t i = 0; i < method_count; i++) {
        IdmRuntimeProtocolMethod *existing = runtime_protocol_find_method(rt, protocol, methods[i].name);
        if (!existing || existing->arity != methods[i].arity) return false;
    }
    return true;
}

static void staged_protocol_methods_destroy(IdmRuntimeProtocolMethod *methods, size_t count) {
    for (size_t i = 0; i < count; i++) runtime_protocol_method_destroy(&methods[i]);
    free(methods);
}

bool idm_protocol_define(IdmRuntime *rt, const char *protocol, const IdmProtocolMethodSpec *methods, size_t method_count, IdmError *err) {
    if (!protocol || !*protocol) return idm_error_set(err, idm_span_unknown(NULL), "protocol definition requires a name");
    for (size_t i = 0; i < method_count; i++) {
        if (!methods[i].name || !*methods[i].name) return idm_error_set(err, idm_span_unknown(NULL), "protocol '%s' has an unnamed method", protocol);
        if (methods[i].has_default && !idm_is_closure(methods[i].default_impl)) return idm_error_set(err, idm_span_unknown(NULL), "protocol '%s.%s' default is not a function", protocol, methods[i].name);
        for (size_t j = i + 1u; j < method_count; j++) {
            if (strcmp(methods[i].name, methods[j].name) == 0) return idm_error_set(err, idm_span_unknown(NULL), "protocol '%s' declares method '%s' more than once", protocol, methods[i].name);
        }
    }

    if (runtime_protocol_method_count_for(rt, protocol) != 0) {
        if (!runtime_protocol_contract_compatible(rt, protocol, methods, method_count)) {
            idm_error_set(err, idm_span_unknown(NULL), "protocol '%s' is already defined with an incompatible contract", protocol);
            return idm_error_reason(rt, err, "protocol-redefinition", 1, idm_atom(rt, protocol));
        }
        for (size_t i = 0; i < method_count; i++) {
            IdmRuntimeProtocolMethod *existing = runtime_protocol_find_method(rt, protocol, methods[i].name);
            existing->has_default = methods[i].has_default;
            existing->default_impl = methods[i].has_default ? methods[i].default_impl : idm_nil();
        }
        return true;
    }

    IdmRuntimeProtocolMethod *staged = method_count == 0 ? NULL : calloc(method_count, sizeof(*staged));
    if (method_count != 0 && !staged) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < method_count; i++) {
        staged[i].protocol = idm_strdup(protocol);
        staged[i].method = idm_strdup(methods[i].name);
        if (!staged[i].protocol || !staged[i].method) {
            staged_protocol_methods_destroy(staged, method_count);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        staged[i].arity = methods[i].arity;
        staged[i].has_default = methods[i].has_default;
        staged[i].default_impl = methods[i].has_default ? methods[i].default_impl : idm_nil();
    }

    IdmProtocolWorld *w = protocol_world(rt);
    if (!protocol_methods_reserve(w, w->method_count + method_count, err)) {
        staged_protocol_methods_destroy(staged, method_count);
        return false;
    }
    for (size_t i = 0; i < method_count; i++) w->methods[w->method_count++] = staged[i];
    free(staged);
    return true;
}

static void staged_protocol_impls_destroy(IdmRuntimeProtocolImpl *impls, size_t count) {
    for (size_t i = 0; i < count; i++) runtime_protocol_impl_destroy(&impls[i]);
    free(impls);
}

bool idm_protocol_extend(IdmRuntime *rt, const char *protocol, const char *type, const IdmProtocolImplSpec *impls, size_t impl_count, IdmError *err) {
    if (!protocol || !*protocol) return idm_error_set(err, idm_span_unknown(NULL), "extend requires a protocol name");
    if (!type || !*type) return idm_error_set(err, idm_span_unknown(NULL), "extend requires a value type name");
    if (runtime_protocol_method_count_for(rt, protocol) == 0) return idm_error_set(err, idm_span_unknown(NULL), "protocol '%s' is not defined at runtime", protocol);
    if (runtime_protocol_find_conformance(rt, protocol, type)) return idm_error_set(err, idm_span_unknown(NULL), "type '%s' already extends protocol '%s'", type, protocol);
    for (size_t i = 0; i < impl_count; i++) {
        if (!impls[i].name || !*impls[i].name) return idm_error_set(err, idm_span_unknown(NULL), "extend for protocol '%s' has an unnamed method", protocol);
        IdmRuntimeProtocolMethod *method = runtime_protocol_find_method(rt, protocol, impls[i].name);
        if (!method) return idm_error_set(err, idm_span_unknown(NULL), "protocol '%s' has no method '%s'", protocol, impls[i].name);
        if (method->arity != impls[i].arity) return idm_error_set(err, idm_span_unknown(NULL), "method '%s.%s' arity mismatch: expected %u got %u", protocol, impls[i].name, method->arity, impls[i].arity);
        if (!idm_is_closure(impls[i].impl)) return idm_error_set(err, idm_span_unknown(NULL), "method '%s.%s' implementation is not a function", protocol, impls[i].name);
        if (runtime_protocol_find_impl(rt, protocol, impls[i].name, type)) return idm_error_set(err, idm_span_unknown(NULL), "method '%s.%s' already has an implementation for type '%s'", protocol, impls[i].name, type);
        for (size_t j = i + 1u; j < impl_count; j++) {
            if (strcmp(impls[i].name, impls[j].name) == 0) return idm_error_set(err, idm_span_unknown(NULL), "extend for '%s' provides method '%s' more than once", protocol, impls[i].name);
        }
    }

    IdmRuntimeProtocolImpl *staged = impl_count == 0 ? NULL : calloc(impl_count, sizeof(*staged));
    if (impl_count != 0 && !staged) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < impl_count; i++) {
        staged[i].protocol = idm_strdup(protocol);
        staged[i].method = idm_strdup(impls[i].name);
        staged[i].type = idm_strdup(type);
        if (!staged[i].protocol || !staged[i].method || !staged[i].type) {
            staged_protocol_impls_destroy(staged, impl_count);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        staged[i].impl = impls[i].impl;
    }
    IdmProtocolWorld *w = protocol_world(rt);
    if (!protocol_impls_reserve(w, w->impl_count + impl_count, err)) {
        staged_protocol_impls_destroy(staged, impl_count);
        return false;
    }
    if (!protocol_conformances_reserve(w, w->conformance_count + 1u, err)) {
        staged_protocol_impls_destroy(staged, impl_count);
        return false;
    }
    IdmRuntimeProtocolConformance conf;
    conf.protocol = idm_strdup(protocol);
    conf.type = idm_strdup(type);
    if (!conf.protocol || !conf.type) {
        runtime_protocol_conformance_destroy(&conf);
        staged_protocol_impls_destroy(staged, impl_count);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t i = 0; i < impl_count; i++) w->impls[w->impl_count++] = staged[i];
    w->conformances[w->conformance_count++] = conf;
    free(staged);
    return true;
}

bool idm_protocol_lookup(IdmRuntime *rt, const char *protocol, const char *method, const char *type, uint32_t argc, IdmValue *out_impl, IdmError *err) {
    IdmRuntimeProtocolMethod *contract = runtime_protocol_find_method(rt, protocol, method);
    if (!contract) return idm_error_set(err, idm_span_unknown(NULL), "protocol method '%s.%s' is not defined", protocol, method);
    if (contract->arity != argc) return idm_error_set(err, idm_span_unknown(NULL), "method '%s.%s' arity mismatch: expected %u got %u", protocol, method, contract->arity, argc);
    IdmRuntimeProtocolImpl *impl = runtime_protocol_find_impl(rt, protocol, method, type);
    if (impl) {
        *out_impl = impl->impl;
        return true;
    }
    if (!runtime_protocol_find_conformance(rt, protocol, type)) {
        return idm_error_set(err, idm_span_unknown(NULL), "type '%s' does not extend protocol '%s'", type, protocol);
    }
    if (contract->has_default) {
        *out_impl = contract->default_impl;
        return true;
    }
    return idm_error_set(err, idm_span_unknown(NULL), "no implementation for method '%s.%s' on type '%s'", protocol, method, type);
}

void idm_runtime_mark_protocol_roots(IdmRuntime *rt) {
    for (size_t p = 0; p < 2u; p++) {
        IdmProtocolWorld *w = &rt->protocol_worlds[p];
        for (size_t i = 0; i < w->method_count; i++) {
            if (w->methods[i].has_default) idm_gc_mark_value(w->methods[i].default_impl);
        }
        for (size_t i = 0; i < w->impl_count; i++) idm_gc_mark_value(w->impls[i].impl);
    }
}

bool idm_is_nil(IdmValue value) {
    return value.tag == IDM_VAL_NIL;
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
        case IDM_VAL_NIL: return true;
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
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR: {
            const char *open = value.tag == IDM_VAL_TUPLE ? "{" : "%[";
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
            if (cur.tag != IDM_VAL_NIL) {
                if (!idm_buf_append(buf, " . ")) return false;
                if (!idm_value_write(buf, cur)) return false;
            }
            return idm_buf_append_char(buf, ')');
        }
    }
    return false;
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
    IdmValue items[2];
    items[0] = idm_atom(rt, "error");
    items[1] = detail;
    return idm_tuple(rt, items, 2u, NULL);
}
