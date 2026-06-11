#include "ish/value.h"

#include "ish/syntax.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    ISH_OBJ_STRING,
    ISH_OBJ_PAIR,
    ISH_OBJ_TUPLE,
    ISH_OBJ_VECTOR,
    ISH_OBJ_DICT,
    ISH_OBJ_SYNTAX,
    ISH_OBJ_CELL,
    ISH_OBJ_CLOSURE,
    ISH_OBJ_RECORD
} IshObjectKind;

typedef struct {
    char *bytes;
    size_t len;
} IshStringObj;

typedef struct {
    IshValue car;
    IshValue cdr;
} IshPairObj;

typedef struct {
    IshValue *items;
    size_t count;
} IshSequenceObj;

typedef struct {
    IshDictEntry *entries;
    size_t count;
} IshDictObj;

typedef struct {
    const IshBytecodeModule *module;
    uint32_t function_index;
    uint32_t *entries;
    size_t entry_count;
    IshValue *captures;
    size_t capture_count;
    IshNamespace *ns;
} IshClosureObj;

typedef struct {
    char *type;
    IshValue fields;
} IshRecordObj;

typedef struct {
    IshValue value;
} IshCellObj;

struct IshObject {
    IshObjectKind kind;
    bool marked;
    size_t bytes;
    struct IshObject *next;
    union {
        IshStringObj string;
        IshPairObj pair;
        IshSequenceObj sequence;
        IshDictObj dict;
        IshSyntax *syntax;
        IshCellObj cell;
        IshClosureObj closure;
        IshRecordObj record;
    } as;
};

struct IshSymbol {
    char *text;
    uint32_t id;
    IshSymbolKind kind;
};

static IshObject *heap_alloc(IshHeap *heap, IshObjectKind kind) {
    IshObject *obj = calloc(1u, sizeof(*obj));
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

static void heap_account(IshHeap *heap, IshObject *obj, size_t extra) {
    obj->bytes += extra;
    heap->bytes_allocated += extra;
}

static size_t syn_footprint(const IshSyntax *syn) {
    if (!syn) return 0;
    size_t total = sizeof(*syn);
    if (syn->token_raw) total += strlen(syn->token_raw) + 1u;
    for (size_t i = 0; i < syn->scopes.count; i++) total += sizeof(syn->scopes.items[i]) + syn->scopes.items[i].scopes.count * sizeof(IshScopeId);
    for (size_t i = 0; i < syn->property_count; i++) total += strlen(syn->properties[i].key) + strlen(syn->properties[i].value) + 2u;
    for (size_t i = 0; i < syn->origins.count; i++) total += strlen(syn->origins.items[i]) + 1u;
    switch (syn->kind) {
        case ISH_SYN_WORD:
        case ISH_SYN_ATOM:
        case ISH_SYN_STRING:
            total += strlen(syn->as.text) + 1u;
            break;
        case ISH_SYN_LIST:
        case ISH_SYN_VECTOR:
        case ISH_SYN_TUPLE:
        case ISH_SYN_DICT:
            for (size_t i = 0; i < syn->as.seq.count; i++) total += syn_footprint(syn->as.seq.items[i]);
            break;
        default:
            break;
    }
    return total;
}

static void object_free(IshObject *obj) {
    if (!obj) return;
    if (obj->kind == ISH_OBJ_STRING) free(obj->as.string.bytes);
    if (obj->kind == ISH_OBJ_TUPLE || obj->kind == ISH_OBJ_VECTOR) free(obj->as.sequence.items);
    if (obj->kind == ISH_OBJ_DICT) free(obj->as.dict.entries);
    if (obj->kind == ISH_OBJ_SYNTAX) ish_syn_free(obj->as.syntax);
    if (obj->kind == ISH_OBJ_CLOSURE) {
        free(obj->as.closure.entries);
        free(obj->as.closure.captures);
    }
    if (obj->kind == ISH_OBJ_RECORD) free(obj->as.record.type);
    free(obj);
}

void ish_runtime_init(IshRuntime *rt) {
    ish_intern_init(&rt->intern);
    ish_heap_init(&rt->heap);
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
    rt->namespaces = NULL;
    rt->ns_count = 0;
    rt->ns_cap = 0;
    rt->main_ns = ish_namespace_get_or_create(rt, "main");
    rt->protocol_methods = NULL;
    rt->protocol_method_count = 0;
    rt->protocol_method_cap = 0;
    rt->protocol_impls = NULL;
    rt->protocol_impl_count = 0;
    rt->protocol_impl_cap = 0;
    rt->protocol_conformances = NULL;
    rt->protocol_conformance_count = 0;
    rt->protocol_conformance_cap = 0;
    rt->gc_modules = NULL;
    rt->gc_module_count = 0;
    rt->gc_module_cap = 0;
    rt->gc_values = NULL;
    rt->gc_value_count = 0;
    rt->gc_value_cap = 0;
    rt->expand_cache = NULL;
    rt->expand_cache_free = NULL;
    rt->current_exec = NULL;
    rt->owned_temps = NULL;
    rt->owned_temp_count = 0;
    rt->owned_temp_cap = 0;
}

bool ish_runtime_own_temp(IshRuntime *rt, const char *path) {
    if (rt->owned_temp_count == rt->owned_temp_cap) {
        size_t cap = rt->owned_temp_cap ? rt->owned_temp_cap * 2u : 8u;
        char **grown = realloc(rt->owned_temps, cap * sizeof(*grown));
        if (!grown) return false;
        rt->owned_temps = grown;
        rt->owned_temp_cap = cap;
    }
    rt->owned_temps[rt->owned_temp_count] = ish_strdup(path);
    if (!rt->owned_temps[rt->owned_temp_count]) return false;
    rt->owned_temp_count++;
    return true;
}

bool ish_runtime_register_gc_module(IshRuntime *rt, const IshBytecodeModule *module) {
    if (!module) return false;
    if (rt->gc_module_count == rt->gc_module_cap) {
        size_t cap = rt->gc_module_cap ? rt->gc_module_cap * 2u : 16u;
        const IshBytecodeModule **grown = realloc(rt->gc_modules, cap * sizeof(*grown));
        if (!grown) return false;
        rt->gc_modules = grown;
        rt->gc_module_cap = cap;
    }
    rt->gc_modules[rt->gc_module_count++] = module;
    return true;
}

void ish_runtime_unregister_gc_module(IshRuntime *rt, const IshBytecodeModule *module) {
    for (size_t i = 0; i < rt->gc_module_count; i++) {
        if (rt->gc_modules[i] == module) {
            rt->gc_modules[i] = rt->gc_modules[rt->gc_module_count - 1u];
            rt->gc_module_count--;
            return;
        }
    }
}

bool ish_runtime_register_gc_value(IshRuntime *rt, IshValue value) {
    if (rt->gc_value_count == rt->gc_value_cap) {
        size_t cap = rt->gc_value_cap ? rt->gc_value_cap * 2u : 8u;
        IshValue *grown = realloc(rt->gc_values, cap * sizeof(*grown));
        if (!grown) return false;
        rt->gc_values = grown;
        rt->gc_value_cap = cap;
    }
    rt->gc_values[rt->gc_value_count++] = value;
    return true;
}

void ish_runtime_unregister_gc_value(IshRuntime *rt, IshValue value) {
    for (size_t i = 0; i < rt->gc_value_count; i++) {
        if (rt->gc_values[i].tag == value.tag && rt->gc_values[i].as.obj == value.as.obj) {
            rt->gc_values[i] = rt->gc_values[rt->gc_value_count - 1u];
            rt->gc_value_count--;
            return;
        }
    }
}

static void runtime_protocol_method_destroy(IshRuntimeProtocolMethod *method) {
    if (!method) return;
    free(method->protocol);
    free(method->method);
    memset(method, 0, sizeof(*method));
}

static void runtime_protocol_impl_destroy(IshRuntimeProtocolImpl *impl) {
    if (!impl) return;
    free(impl->protocol);
    free(impl->method);
    free(impl->type);
    memset(impl, 0, sizeof(*impl));
}

static void runtime_protocol_conformance_destroy(IshRuntimeProtocolConformance *conf) {
    if (!conf) return;
    free(conf->protocol);
    free(conf->type);
    memset(conf, 0, sizeof(*conf));
}

void ish_runtime_destroy(IshRuntime *rt) {
    for (size_t i = 0; i < rt->protocol_method_count; i++) runtime_protocol_method_destroy(&rt->protocol_methods[i]);
    free(rt->protocol_methods);
    rt->protocol_methods = NULL;
    rt->protocol_method_count = 0;
    rt->protocol_method_cap = 0;
    for (size_t i = 0; i < rt->protocol_impl_count; i++) runtime_protocol_impl_destroy(&rt->protocol_impls[i]);
    free(rt->protocol_impls);
    rt->protocol_impls = NULL;
    rt->protocol_impl_count = 0;
    rt->protocol_impl_cap = 0;
    for (size_t i = 0; i < rt->protocol_conformance_count; i++) runtime_protocol_conformance_destroy(&rt->protocol_conformances[i]);
    free(rt->protocol_conformances);
    rt->protocol_conformances = NULL;
    rt->protocol_conformance_count = 0;
    rt->protocol_conformance_cap = 0;
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
    ish_heap_destroy(&rt->heap);
    ish_intern_destroy(&rt->intern);
}

IshNamespace *ish_namespace_get_or_create(IshRuntime *rt, const char *name) {
    for (size_t i = 0; i < rt->ns_count; i++) {
        if (strcmp(rt->namespaces[i]->name, name) == 0) return rt->namespaces[i];
    }
    IshNamespace *ns = calloc(1u, sizeof(*ns));
    if (!ns) return NULL;
    ns->name = ish_strdup(name);
    if (!ns->name) { free(ns); return NULL; }
    if (rt->ns_count == rt->ns_cap) {
        size_t cap = rt->ns_cap ? rt->ns_cap * 2u : 8u;
        IshNamespace **grown = realloc(rt->namespaces, cap * sizeof(*grown));
        if (!grown) { free(ns->name); free(ns); return NULL; }
        rt->namespaces = grown;
        rt->ns_cap = cap;
    }
    rt->namespaces[rt->ns_count++] = ns;
    return ns;
}

bool ish_ns_slot_ensure(IshNamespace *ns, uint32_t id, IshError *err) {
    size_t needed = (size_t)id + 1u;
    if (needed <= ns->slot_count) return true;
    if (needed > ns->slot_cap) {
        size_t cap = ns->slot_cap ? ns->slot_cap * 2u : 16u;
        while (cap < needed) cap *= 2u;
        IshValue *grown = realloc(ns->slots, cap * sizeof(*grown));
        if (!grown) return ish_error_oom(err, ish_span_unknown(NULL));
        ns->slots = grown;
        ns->slot_cap = cap;
    }
    for (size_t i = ns->slot_count; i < needed; i++) ns->slots[i] = ish_nil();
    ns->slot_count = needed;
    return true;
}

void ish_ns_slot_set(IshNamespace *ns, uint32_t id, IshValue value) {
    if ((size_t)id < ns->slot_count) ns->slots[id] = value;
}

IshValue ish_ns_slot_get(const IshNamespace *ns, uint32_t id) {
    return (size_t)id < ns->slot_count ? ns->slots[id] : ish_nil();
}

void ish_intern_init(IshIntern *intern) {
    intern->symbols = NULL;
    intern->count = 0;
    intern->cap = 0;
    intern->next_id = 1u;
}

void ish_intern_destroy(IshIntern *intern) {
    if (!intern) return;
    for (size_t i = 0; i < intern->count; i++) { free(intern->symbols[i]->text); free(intern->symbols[i]); }
    free(intern->symbols);
    intern->symbols = NULL;
    intern->count = 0;
    intern->cap = 0;
    intern->next_id = 1u;
}

IshSymbol *ish_intern(IshIntern *intern, IshSymbolKind kind, const char *text) {
    for (size_t i = 0; i < intern->count; i++) {
        if (intern->symbols[i]->kind == kind && strcmp(intern->symbols[i]->text, text) == 0) return intern->symbols[i];
    }
    if (intern->count == intern->cap) {
        size_t cap = intern->cap ? intern->cap * 2u : 32u;
        IshSymbol **next = realloc(intern->symbols, cap * sizeof(*next));
        if (!next) return NULL;
        intern->symbols = next;
        intern->cap = cap;
    }
    IshSymbol *sym = malloc(sizeof(*sym));
    if (!sym) return NULL;
    sym->text = ish_strdup(text);
    if (!sym->text) {
        free(sym);
        return NULL;
    }
    sym->id = intern->next_id++;
    sym->kind = kind;
    intern->symbols[intern->count++] = sym;
    return sym;
}

const char *ish_symbol_text(const IshSymbol *sym) {
    return sym ? sym->text : "";
}

uint32_t ish_symbol_id(const IshSymbol *sym) {
    return sym ? sym->id : 0u;
}

IshSymbolKind ish_symbol_kind(const IshSymbol *sym) {
    return sym ? sym->kind : ISH_SYMBOL_WORD;
}

void ish_heap_init(IshHeap *heap) {
    heap->objects = NULL;
    heap->object_count = 0;
    heap->bytes_allocated = 0;
}

void ish_heap_destroy(IshHeap *heap) {
    IshObject *obj = heap->objects;
    while (obj) {
        IshObject *next = obj->next;
        object_free(obj);
        obj = next;
    }
    heap->objects = NULL;
    heap->object_count = 0;
    heap->bytes_allocated = 0;
}

static void value_mark(IshValue value) {
    if (value.tag != ISH_VAL_STRING && value.tag != ISH_VAL_PAIR && value.tag != ISH_VAL_TUPLE && value.tag != ISH_VAL_VECTOR && value.tag != ISH_VAL_DICT && value.tag != ISH_VAL_SYNTAX && value.tag != ISH_VAL_CELL && value.tag != ISH_VAL_CLOSURE && value.tag != ISH_VAL_RECORD) return;
    IshObject *obj = value.as.obj;
    if (!obj || obj->marked) return;
    obj->marked = true;
    if (obj->kind == ISH_OBJ_PAIR) {
        value_mark(obj->as.pair.car);
        value_mark(obj->as.pair.cdr);
    } else if (obj->kind == ISH_OBJ_TUPLE || obj->kind == ISH_OBJ_VECTOR) {
        for (size_t i = 0; i < obj->as.sequence.count; i++) value_mark(obj->as.sequence.items[i]);
    } else if (obj->kind == ISH_OBJ_DICT) {
        for (size_t i = 0; i < obj->as.dict.count; i++) {
            value_mark(obj->as.dict.entries[i].key);
            value_mark(obj->as.dict.entries[i].value);
        }
    } else if (obj->kind == ISH_OBJ_CELL) {
        value_mark(obj->as.cell.value);
    } else if (obj->kind == ISH_OBJ_CLOSURE) {
        for (size_t i = 0; i < obj->as.closure.capture_count; i++) value_mark(obj->as.closure.captures[i]);
    } else if (obj->kind == ISH_OBJ_RECORD) {
        value_mark(obj->as.record.fields);
    }
}

void ish_gc_mark_value(IshValue value) {
    value_mark(value);
}

void ish_heap_sweep(IshHeap *heap) {
    IshObject **cursor = &heap->objects;
    while (*cursor) {
        IshObject *obj = *cursor;
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

size_t ish_heap_bytes(const IshHeap *heap) {
    return heap->bytes_allocated;
}

void ish_heap_collect(IshHeap *heap, const IshValue *roots, size_t root_count) {
    for (size_t i = 0; i < root_count; i++) value_mark(roots[i]);
    ish_heap_sweep(heap);
}

size_t ish_heap_object_count(const IshHeap *heap) {
    return heap->object_count;
}

IshValue ish_nil(void) {
    IshValue v;
    v.tag = ISH_VAL_NIL;
    v.as.id = 0;
    return v;
}

IshValue ish_int(int64_t value) {
    IshValue v;
    v.tag = ISH_VAL_INT;
    v.as.i = value;
    return v;
}

IshValue ish_float(double value) {
    IshValue v;
    v.tag = ISH_VAL_FLOAT;
    v.as.f = value;
    return v;
}

IshValue ish_word(IshRuntime *rt, const char *text) {
    IshValue v;
    v.tag = ISH_VAL_WORD;
    v.as.symbol = ish_intern(&rt->intern, ISH_SYMBOL_WORD, text);
    return v;
}

IshValue ish_atom(IshRuntime *rt, const char *text) {
    IshValue v;
    v.tag = ISH_VAL_ATOM;
    v.as.symbol = ish_intern(&rt->intern, ISH_SYMBOL_ATOM, text);
    return v;
}

IshValue ish_string_n(IshRuntime *rt, const char *text, size_t len, IshError *err) {
    IshObject *obj = heap_alloc(&rt->heap, ISH_OBJ_STRING);
    if (!obj) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return ish_nil();
    }
    obj->as.string.bytes = ish_strndup(text, len);
    if (!obj->as.string.bytes) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return ish_nil();
    }
    obj->as.string.len = len;
    heap_account(&rt->heap, obj, len + 1u);
    IshValue v;
    v.tag = ISH_VAL_STRING;
    v.as.obj = obj;
    return v;
}

IshValue ish_string(IshRuntime *rt, const char *text, IshError *err) {
    return ish_string_n(rt, text, strlen(text), err);
}

IshValue ish_cons(IshRuntime *rt, IshValue car, IshValue cdr, IshError *err) {
    IshObject *obj = heap_alloc(&rt->heap, ISH_OBJ_PAIR);
    if (!obj) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return ish_nil();
    }
    obj->as.pair.car = car;
    obj->as.pair.cdr = cdr;
    IshValue v;
    v.tag = ISH_VAL_PAIR;
    v.as.obj = obj;
    return v;
}

static IshValue sequence_value(IshRuntime *rt, const IshValue *items, size_t count, IshObjectKind obj_kind, IshValueTag tag, IshError *err) {
    IshObject *obj = heap_alloc(&rt->heap, obj_kind);
    if (!obj) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return ish_nil();
    }
    obj->as.sequence.count = count;
    obj->as.sequence.items = NULL;
    if (count != 0) {
        obj->as.sequence.items = malloc(count * sizeof(*obj->as.sequence.items));
        if (!obj->as.sequence.items) {
            ish_error_oom(err, ish_span_unknown(NULL));
            return ish_nil();
        }
        memcpy(obj->as.sequence.items, items, count * sizeof(*items));
        heap_account(&rt->heap, obj, count * sizeof(*items));
    }
    IshValue v;
    v.tag = tag;
    v.as.obj = obj;
    return v;
}

IshValue ish_tuple(IshRuntime *rt, const IshValue *items, size_t count, IshError *err) {
    return sequence_value(rt, items, count, ISH_OBJ_TUPLE, ISH_VAL_TUPLE, err);
}

IshValue ish_vector(IshRuntime *rt, const IshValue *items, size_t count, IshError *err) {
    return sequence_value(rt, items, count, ISH_OBJ_VECTOR, ISH_VAL_VECTOR, err);
}

IshValue ish_dict(IshRuntime *rt, const IshDictEntry *entries, size_t count, IshError *err) {
    IshObject *obj = heap_alloc(&rt->heap, ISH_OBJ_DICT);
    if (!obj) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return ish_nil();
    }
    obj->as.dict.count = count;
    obj->as.dict.entries = NULL;
    if (count != 0) {
        obj->as.dict.entries = malloc(count * sizeof(*obj->as.dict.entries));
        if (!obj->as.dict.entries) {
            ish_error_oom(err, ish_span_unknown(NULL));
            return ish_nil();
        }
        memcpy(obj->as.dict.entries, entries, count * sizeof(*entries));
        heap_account(&rt->heap, obj, count * sizeof(*entries));
    }
    IshValue v;
    v.tag = ISH_VAL_DICT;
    v.as.obj = obj;
    return v;
}

IshValue ish_syntax_value(IshRuntime *rt, const IshSyntax *syntax, IshError *err) {
    IshObject *obj = heap_alloc(&rt->heap, ISH_OBJ_SYNTAX);
    if (!obj) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return ish_nil();
    }
    obj->as.syntax = ish_syn_clone(syntax);
    if (!obj->as.syntax) {
        ish_error_oom(err, syntax ? syntax->span : ish_span_unknown(NULL));
        return ish_nil();
    }
    heap_account(&rt->heap, obj, syn_footprint(obj->as.syntax));
    IshValue v;
    v.tag = ISH_VAL_SYNTAX;
    v.as.obj = obj;
    return v;
}

IshValue ish_cell(IshRuntime *rt, IshValue initial, IshError *err) {
    IshObject *obj = heap_alloc(&rt->heap, ISH_OBJ_CELL);
    if (!obj) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return ish_nil();
    }
    obj->as.cell.value = initial;
    IshValue v;
    v.tag = ISH_VAL_CELL;
    v.as.obj = obj;
    return v;
}

IshValue ish_closure_multi_in_module(IshRuntime *rt, const IshBytecodeModule *module, const uint32_t *function_indexes, size_t function_count, const IshValue *captures, size_t capture_count, IshNamespace *ns, IshError *err) {
    if (function_count == 0) {
        ish_error_set(err, ish_span_unknown(NULL), "closure must have at least one function entry");
        return ish_nil();
    }
    IshObject *obj = heap_alloc(&rt->heap, ISH_OBJ_CLOSURE);
    if (!obj) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return ish_nil();
    }
    obj->as.closure.module = module;
    obj->as.closure.ns = ns;
    obj->as.closure.function_index = function_indexes[0];
    obj->as.closure.entry_count = function_count;
    obj->as.closure.entries = NULL;
    if (function_count > 1) {
        obj->as.closure.entries = malloc(function_count * sizeof(*obj->as.closure.entries));
        if (!obj->as.closure.entries) {
            ish_error_oom(err, ish_span_unknown(NULL));
            return ish_nil();
        }
        memcpy(obj->as.closure.entries, function_indexes, function_count * sizeof(*function_indexes));
        heap_account(&rt->heap, obj, function_count * sizeof(*function_indexes));
    }
    obj->as.closure.capture_count = capture_count;
    obj->as.closure.captures = NULL;
    if (capture_count != 0) {
        obj->as.closure.captures = malloc(capture_count * sizeof(*obj->as.closure.captures));
        if (!obj->as.closure.captures) {
            ish_error_oom(err, ish_span_unknown(NULL));
            return ish_nil();
        }
        memcpy(obj->as.closure.captures, captures, capture_count * sizeof(*captures));
        heap_account(&rt->heap, obj, capture_count * sizeof(*captures));
    }
    IshValue v;
    v.tag = ISH_VAL_CLOSURE;
    v.as.obj = obj;
    return v;
}

IshValue ish_closure_multi(IshRuntime *rt, const uint32_t *function_indexes, size_t function_count, const IshValue *captures, size_t capture_count, IshNamespace *ns, IshError *err) {
    return ish_closure_multi_in_module(rt, NULL, function_indexes, function_count, captures, capture_count, ns, err);
}

IshValue ish_closure(IshRuntime *rt, uint32_t function_index, const IshValue *captures, size_t capture_count, IshNamespace *ns, IshError *err) {
    return ish_closure_multi(rt, &function_index, 1, captures, capture_count, ns, err);
}

IshValue ish_closure_in_module(IshRuntime *rt, const IshBytecodeModule *module, uint32_t function_index, const IshValue *captures, size_t capture_count, IshNamespace *ns, IshError *err) {
    return ish_closure_multi_in_module(rt, module, &function_index, 1, captures, capture_count, ns, err);
}

IshValue ish_record(IshRuntime *rt, const char *type, IshValue fields, IshError *err) {
    if (!type || !*type) {
        ish_error_set(err, ish_span_unknown(NULL), "record type must be a non-empty name");
        return ish_nil();
    }
    if (!ish_is_dict(fields)) {
        ish_error_set(err, ish_span_unknown(NULL), "record fields must be a dict");
        return ish_nil();
    }
    IshObject *obj = heap_alloc(&rt->heap, ISH_OBJ_RECORD);
    if (!obj) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return ish_nil();
    }
    obj->as.record.type = ish_strdup(type);
    if (!obj->as.record.type) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return ish_nil();
    }
    obj->as.record.fields = fields;
    heap_account(&rt->heap, obj, strlen(obj->as.record.type) + 1u);
    IshValue v;
    v.tag = ISH_VAL_RECORD;
    v.as.obj = obj;
    return v;
}

IshValue ish_pid(uint64_t id) {
    IshValue v;
    v.tag = ISH_VAL_PID;
    v.as.id = id;
    return v;
}

IshValue ish_ref(uint64_t id) {
    IshValue v;
    v.tag = ISH_VAL_REF;
    v.as.id = id;
    return v;
}

IshValue ish_port(uint64_t id) {
    IshValue v;
    v.tag = ISH_VAL_PORT;
    v.as.id = id;
    return v;
}

IshValue ish_primitive_value(uint32_t primitive) {
    IshValue v;
    v.tag = ISH_VAL_PRIMITIVE;
    v.as.id = primitive;
    return v;
}

bool ish_is_primitive(IshValue value) {
    return value.tag == ISH_VAL_PRIMITIVE;
}

const char *ish_value_type_name(IshValueTag tag) {
    switch (tag) {
        case ISH_VAL_NIL: return "nil";
        case ISH_VAL_ATOM: return "atom";
        case ISH_VAL_WORD: return "word";
        case ISH_VAL_INT: return "int";
        case ISH_VAL_FLOAT: return "float";
        case ISH_VAL_STRING: return "string";
        case ISH_VAL_PAIR: return "pair";
        case ISH_VAL_TUPLE: return "tuple";
        case ISH_VAL_VECTOR: return "vector";
        case ISH_VAL_DICT: return "dict";
        case ISH_VAL_SYNTAX: return "syntax";
        case ISH_VAL_CELL: return "cell";
        case ISH_VAL_CLOSURE: return "closure";
        case ISH_VAL_PID: return "pid";
        case ISH_VAL_REF: return "ref";
        case ISH_VAL_PORT: return "port";
        case ISH_VAL_PRIMITIVE: return "primitive";
        case ISH_VAL_RECORD: return "record";
    }
    return "<bad-type>";
}

const char *ish_value_dispatch_type_name(IshValue value) {
    if (value.tag == ISH_VAL_RECORD) return value.as.obj->as.record.type;
    return ish_value_type_name(value.tag);
}

bool ish_value_type_from_name(const char *name, IshValueTag *out) {
    if (!name) return false;
    for (int i = (int)ISH_VAL_NIL; i <= (int)ISH_VAL_PRIMITIVE; i++) {
        IshValueTag tag = (IshValueTag)i;
        if (strcmp(name, ish_value_type_name(tag)) == 0) {
            if (out) *out = tag;
            return true;
        }
    }
    return false;
}

static bool protocol_name_eq(const char *a_protocol, const char *a_method, const char *b_protocol, const char *b_method) {
    return strcmp(a_protocol, b_protocol) == 0 && strcmp(a_method, b_method) == 0;
}

static IshRuntimeProtocolMethod *runtime_protocol_find_method(IshRuntime *rt, const char *protocol, const char *method) {
    for (size_t i = 0; i < rt->protocol_method_count; i++) {
        if (protocol_name_eq(rt->protocol_methods[i].protocol, rt->protocol_methods[i].method, protocol, method)) return &rt->protocol_methods[i];
    }
    return NULL;
}

static IshRuntimeProtocolImpl *runtime_protocol_find_impl(IshRuntime *rt, const char *protocol, const char *method, const char *type) {
    for (size_t i = 0; i < rt->protocol_impl_count; i++) {
        if (strcmp(rt->protocol_impls[i].type, type) == 0 && protocol_name_eq(rt->protocol_impls[i].protocol, rt->protocol_impls[i].method, protocol, method)) return &rt->protocol_impls[i];
    }
    return NULL;
}

static IshRuntimeProtocolConformance *runtime_protocol_find_conformance(IshRuntime *rt, const char *protocol, const char *type) {
    for (size_t i = 0; i < rt->protocol_conformance_count; i++) {
        if (strcmp(rt->protocol_conformances[i].type, type) == 0 && strcmp(rt->protocol_conformances[i].protocol, protocol) == 0) return &rt->protocol_conformances[i];
    }
    return NULL;
}

static bool protocol_methods_reserve(IshRuntime *rt, size_t needed, IshError *err) {
    if (needed <= rt->protocol_method_cap) return true;
    size_t cap = rt->protocol_method_cap ? rt->protocol_method_cap * 2u : 8u;
    while (cap < needed) cap *= 2u;
    IshRuntimeProtocolMethod *next = realloc(rt->protocol_methods, cap * sizeof(*next));
    if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
    rt->protocol_methods = next;
    rt->protocol_method_cap = cap;
    return true;
}

static bool protocol_impls_reserve(IshRuntime *rt, size_t needed, IshError *err) {
    if (needed <= rt->protocol_impl_cap) return true;
    size_t cap = rt->protocol_impl_cap ? rt->protocol_impl_cap * 2u : 8u;
    while (cap < needed) cap *= 2u;
    IshRuntimeProtocolImpl *next = realloc(rt->protocol_impls, cap * sizeof(*next));
    if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
    rt->protocol_impls = next;
    rt->protocol_impl_cap = cap;
    return true;
}

static bool protocol_conformances_reserve(IshRuntime *rt, size_t needed, IshError *err) {
    if (needed <= rt->protocol_conformance_cap) return true;
    size_t cap = rt->protocol_conformance_cap ? rt->protocol_conformance_cap * 2u : 8u;
    while (cap < needed) cap *= 2u;
    IshRuntimeProtocolConformance *next = realloc(rt->protocol_conformances, cap * sizeof(*next));
    if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
    rt->protocol_conformances = next;
    rt->protocol_conformance_cap = cap;
    return true;
}

static size_t runtime_protocol_method_count_for(IshRuntime *rt, const char *protocol) {
    size_t count = 0;
    for (size_t i = 0; i < rt->protocol_method_count; i++) if (strcmp(rt->protocol_methods[i].protocol, protocol) == 0) count++;
    return count;
}

static bool runtime_protocol_contract_compatible(IshRuntime *rt, const char *protocol, const IshProtocolMethodSpec *methods, size_t method_count) {
    if (runtime_protocol_method_count_for(rt, protocol) != method_count) return false;
    for (size_t i = 0; i < method_count; i++) {
        IshRuntimeProtocolMethod *existing = runtime_protocol_find_method(rt, protocol, methods[i].name);
        if (!existing || existing->arity != methods[i].arity) return false;
    }
    return true;
}

static void runtime_protocol_remove_impls_for(IshRuntime *rt, const char *protocol) {
    size_t out = 0;
    for (size_t i = 0; i < rt->protocol_impl_count; i++) {
        if (strcmp(rt->protocol_impls[i].protocol, protocol) == 0) {
            runtime_protocol_impl_destroy(&rt->protocol_impls[i]);
            continue;
        }
        if (out != i) rt->protocol_impls[out] = rt->protocol_impls[i];
        out++;
    }
    rt->protocol_impl_count = out;
}

static void runtime_protocol_remove_conformances_for(IshRuntime *rt, const char *protocol) {
    size_t out = 0;
    for (size_t i = 0; i < rt->protocol_conformance_count; i++) {
        if (strcmp(rt->protocol_conformances[i].protocol, protocol) == 0) {
            runtime_protocol_conformance_destroy(&rt->protocol_conformances[i]);
            continue;
        }
        if (out != i) rt->protocol_conformances[out] = rt->protocol_conformances[i];
        out++;
    }
    rt->protocol_conformance_count = out;
}

static void runtime_protocol_remove_methods_for(IshRuntime *rt, const char *protocol) {
    size_t out = 0;
    for (size_t i = 0; i < rt->protocol_method_count; i++) {
        if (strcmp(rt->protocol_methods[i].protocol, protocol) == 0) {
            runtime_protocol_method_destroy(&rt->protocol_methods[i]);
            continue;
        }
        if (out != i) rt->protocol_methods[out] = rt->protocol_methods[i];
        out++;
    }
    rt->protocol_method_count = out;
}

static void staged_protocol_methods_destroy(IshRuntimeProtocolMethod *methods, size_t count) {
    for (size_t i = 0; i < count; i++) runtime_protocol_method_destroy(&methods[i]);
    free(methods);
}

bool ish_protocol_define(IshRuntime *rt, const char *protocol, const IshProtocolMethodSpec *methods, size_t method_count, IshError *err) {
    if (!protocol || !*protocol) return ish_error_set(err, ish_span_unknown(NULL), "protocol definition requires a name");
    for (size_t i = 0; i < method_count; i++) {
        if (!methods[i].name || !*methods[i].name) return ish_error_set(err, ish_span_unknown(NULL), "protocol '%s' has an unnamed method", protocol);
        if (methods[i].has_default && !ish_is_closure(methods[i].default_impl)) return ish_error_set(err, ish_span_unknown(NULL), "protocol '%s.%s' default is not a function", protocol, methods[i].name);
        for (size_t j = i + 1u; j < method_count; j++) {
            if (strcmp(methods[i].name, methods[j].name) == 0) return ish_error_set(err, ish_span_unknown(NULL), "protocol '%s' declares method '%s' more than once", protocol, methods[i].name);
        }
    }

    if (runtime_protocol_contract_compatible(rt, protocol, methods, method_count)) {
        for (size_t i = 0; i < method_count; i++) {
            IshRuntimeProtocolMethod *existing = runtime_protocol_find_method(rt, protocol, methods[i].name);
            existing->has_default = methods[i].has_default;
            existing->default_impl = methods[i].has_default ? methods[i].default_impl : ish_nil();
        }
        return true;
    }

    IshRuntimeProtocolMethod *staged = method_count == 0 ? NULL : calloc(method_count, sizeof(*staged));
    if (method_count != 0 && !staged) return ish_error_oom(err, ish_span_unknown(NULL));
    for (size_t i = 0; i < method_count; i++) {
        staged[i].protocol = ish_strdup(protocol);
        staged[i].method = ish_strdup(methods[i].name);
        if (!staged[i].protocol || !staged[i].method) {
            staged_protocol_methods_destroy(staged, method_count);
            return ish_error_oom(err, ish_span_unknown(NULL));
        }
        staged[i].arity = methods[i].arity;
        staged[i].has_default = methods[i].has_default;
        staged[i].default_impl = methods[i].has_default ? methods[i].default_impl : ish_nil();
    }

    size_t old_count = runtime_protocol_method_count_for(rt, protocol);
    size_t needed = rt->protocol_method_count - old_count + method_count;
    if (!protocol_methods_reserve(rt, needed, err)) {
        staged_protocol_methods_destroy(staged, method_count);
        return false;
    }

    runtime_protocol_remove_methods_for(rt, protocol);
    runtime_protocol_remove_impls_for(rt, protocol);
    runtime_protocol_remove_conformances_for(rt, protocol);
    for (size_t i = 0; i < method_count; i++) rt->protocol_methods[rt->protocol_method_count++] = staged[i];
    free(staged);
    return true;
}

static void staged_protocol_impls_destroy(IshRuntimeProtocolImpl *impls, size_t count) {
    for (size_t i = 0; i < count; i++) runtime_protocol_impl_destroy(&impls[i]);
    free(impls);
}

bool ish_protocol_extend(IshRuntime *rt, const char *protocol, const char *type, const IshProtocolImplSpec *impls, size_t impl_count, IshError *err) {
    if (!protocol || !*protocol) return ish_error_set(err, ish_span_unknown(NULL), "extend requires a protocol name");
    if (!type || !*type) return ish_error_set(err, ish_span_unknown(NULL), "extend requires a value type name");
    if (runtime_protocol_method_count_for(rt, protocol) == 0) return ish_error_set(err, ish_span_unknown(NULL), "protocol '%s' is not defined at runtime", protocol);
    if (runtime_protocol_find_conformance(rt, protocol, type)) return ish_error_set(err, ish_span_unknown(NULL), "type '%s' already extends protocol '%s'", type, protocol);
    for (size_t i = 0; i < impl_count; i++) {
        if (!impls[i].name || !*impls[i].name) return ish_error_set(err, ish_span_unknown(NULL), "extend for protocol '%s' has an unnamed method", protocol);
        IshRuntimeProtocolMethod *method = runtime_protocol_find_method(rt, protocol, impls[i].name);
        if (!method) return ish_error_set(err, ish_span_unknown(NULL), "protocol '%s' has no method '%s'", protocol, impls[i].name);
        if (method->arity != impls[i].arity) return ish_error_set(err, ish_span_unknown(NULL), "method '%s.%s' arity mismatch: expected %u got %u", protocol, impls[i].name, method->arity, impls[i].arity);
        if (!ish_is_closure(impls[i].impl)) return ish_error_set(err, ish_span_unknown(NULL), "method '%s.%s' implementation is not a function", protocol, impls[i].name);
        if (runtime_protocol_find_impl(rt, protocol, impls[i].name, type)) return ish_error_set(err, ish_span_unknown(NULL), "method '%s.%s' already has an implementation for type '%s'", protocol, impls[i].name, type);
        for (size_t j = i + 1u; j < impl_count; j++) {
            if (strcmp(impls[i].name, impls[j].name) == 0) return ish_error_set(err, ish_span_unknown(NULL), "extend for '%s' implements method '%s' more than once", protocol, impls[i].name);
        }
    }

    IshRuntimeProtocolImpl *staged = impl_count == 0 ? NULL : calloc(impl_count, sizeof(*staged));
    if (impl_count != 0 && !staged) return ish_error_oom(err, ish_span_unknown(NULL));
    for (size_t i = 0; i < impl_count; i++) {
        staged[i].protocol = ish_strdup(protocol);
        staged[i].method = ish_strdup(impls[i].name);
        staged[i].type = ish_strdup(type);
        if (!staged[i].protocol || !staged[i].method || !staged[i].type) {
            staged_protocol_impls_destroy(staged, impl_count);
            return ish_error_oom(err, ish_span_unknown(NULL));
        }
        staged[i].impl = impls[i].impl;
    }
    if (!protocol_impls_reserve(rt, rt->protocol_impl_count + impl_count, err)) {
        staged_protocol_impls_destroy(staged, impl_count);
        return false;
    }
    if (!protocol_conformances_reserve(rt, rt->protocol_conformance_count + 1u, err)) {
        staged_protocol_impls_destroy(staged, impl_count);
        return false;
    }
    IshRuntimeProtocolConformance conf;
    conf.protocol = ish_strdup(protocol);
    conf.type = ish_strdup(type);
    if (!conf.protocol || !conf.type) {
        runtime_protocol_conformance_destroy(&conf);
        staged_protocol_impls_destroy(staged, impl_count);
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    for (size_t i = 0; i < impl_count; i++) rt->protocol_impls[rt->protocol_impl_count++] = staged[i];
    rt->protocol_conformances[rt->protocol_conformance_count++] = conf;
    free(staged);
    return true;
}

bool ish_protocol_lookup(IshRuntime *rt, const char *protocol, const char *method, const char *type, uint32_t argc, IshValue *out_impl, IshError *err) {
    IshRuntimeProtocolMethod *contract = runtime_protocol_find_method(rt, protocol, method);
    if (!contract) return ish_error_set(err, ish_span_unknown(NULL), "protocol method '%s.%s' is not defined", protocol, method);
    if (contract->arity != argc) return ish_error_set(err, ish_span_unknown(NULL), "method '%s.%s' arity mismatch: expected %u got %u", protocol, method, contract->arity, argc);
    IshRuntimeProtocolImpl *impl = runtime_protocol_find_impl(rt, protocol, method, type);
    if (impl) {
        *out_impl = impl->impl;
        return true;
    }
    if (!runtime_protocol_find_conformance(rt, protocol, type)) {
        return ish_error_set(err, ish_span_unknown(NULL), "type '%s' does not extend protocol '%s'", type, protocol);
    }
    if (contract->has_default) {
        *out_impl = contract->default_impl;
        return true;
    }
    return ish_error_set(err, ish_span_unknown(NULL), "no implementation for method '%s.%s' on type '%s'", protocol, method, type);
}

void ish_runtime_mark_protocol_roots(IshRuntime *rt) {
    for (size_t i = 0; i < rt->protocol_method_count; i++) {
        if (rt->protocol_methods[i].has_default) ish_gc_mark_value(rt->protocol_methods[i].default_impl);
    }
    for (size_t i = 0; i < rt->protocol_impl_count; i++) ish_gc_mark_value(rt->protocol_impls[i].impl);
}

bool ish_is_nil(IshValue value) {
    return value.tag == ISH_VAL_NIL;
}

bool ish_is_pair(IshValue value) {
    return value.tag == ISH_VAL_PAIR;
}

bool ish_is_tuple(IshValue value) {
    return value.tag == ISH_VAL_TUPLE;
}

bool ish_is_vector(IshValue value) {
    return value.tag == ISH_VAL_VECTOR;
}

bool ish_is_dict(IshValue value) {
    return value.tag == ISH_VAL_DICT;
}

bool ish_is_syntax(IshValue value) {
    return value.tag == ISH_VAL_SYNTAX;
}

bool ish_is_string(IshValue value) {
    return value.tag == ISH_VAL_STRING;
}

bool ish_is_cell(IshValue value) {
    return value.tag == ISH_VAL_CELL;
}

bool ish_is_closure(IshValue value) {
    return value.tag == ISH_VAL_CLOSURE;
}

bool ish_is_record(IshValue value) {
    return value.tag == ISH_VAL_RECORD;
}

IshValue ish_cell_get(IshValue cell, IshError *err) {
    if (cell.tag != ISH_VAL_CELL) {
        ish_error_set(err, ish_span_unknown(NULL), "expected cell");
        return ish_nil();
    }
    return cell.as.obj->as.cell.value;
}

bool ish_cell_set(IshValue cell, IshValue value, IshError *err) {
    if (cell.tag != ISH_VAL_CELL) return ish_error_set(err, ish_span_unknown(NULL), "expected cell");
    cell.as.obj->as.cell.value = value;
    return true;
}

uint32_t ish_closure_function_index(IshValue value) {
    return value.tag == ISH_VAL_CLOSURE ? value.as.obj->as.closure.function_index : UINT32_MAX;
}

const IshBytecodeModule *ish_closure_module(IshValue value) {
    return value.tag == ISH_VAL_CLOSURE ? value.as.obj->as.closure.module : NULL;
}

size_t ish_closure_entry_count(IshValue value) {
    return value.tag == ISH_VAL_CLOSURE ? value.as.obj->as.closure.entry_count : 0;
}

uint32_t ish_closure_entry(IshValue value, size_t index, IshError *err) {
    if (value.tag != ISH_VAL_CLOSURE) {
        ish_error_set(err, ish_span_unknown(NULL), "expected closure");
        return UINT32_MAX;
    }
    if (index >= value.as.obj->as.closure.entry_count) {
        ish_error_set(err, ish_span_unknown(NULL), "closure entry index out of bounds");
        return UINT32_MAX;
    }
    if (value.as.obj->as.closure.entry_count == 1) return value.as.obj->as.closure.function_index;
    return value.as.obj->as.closure.entries[index];
}

size_t ish_closure_capture_count(IshValue value) {
    return value.tag == ISH_VAL_CLOSURE ? value.as.obj->as.closure.capture_count : 0;
}

IshNamespace *ish_closure_namespace(IshValue value) {
    return value.tag == ISH_VAL_CLOSURE ? value.as.obj->as.closure.ns : NULL;
}

IshValue ish_closure_capture(IshValue value, size_t index, IshError *err) {
    if (value.tag != ISH_VAL_CLOSURE) {
        ish_error_set(err, ish_span_unknown(NULL), "expected closure");
        return ish_nil();
    }
    if (index >= value.as.obj->as.closure.capture_count) {
        ish_error_set(err, ish_span_unknown(NULL), "closure capture index out of bounds");
        return ish_nil();
    }
    return value.as.obj->as.closure.captures[index];
}

const char *ish_record_type(IshValue value, IshError *err) {
    if (value.tag != ISH_VAL_RECORD) {
        ish_error_set(err, ish_span_unknown(NULL), "expected record");
        return NULL;
    }
    return value.as.obj->as.record.type;
}

IshValue ish_record_fields(IshValue value, IshError *err) {
    if (value.tag != ISH_VAL_RECORD) {
        ish_error_set(err, ish_span_unknown(NULL), "expected record");
        return ish_nil();
    }
    return value.as.obj->as.record.fields;
}

static const char *record_field_name(IshValue field) {
    if (field.tag == ISH_VAL_ATOM || field.tag == ISH_VAL_WORD) return ish_symbol_text(field.as.symbol);
    if (field.tag == ISH_VAL_STRING) return ish_string_bytes(field);
    return NULL;
}

bool ish_record_field(IshValue value, IshValue field, IshValue *out, IshError *err) {
    if (value.tag != ISH_VAL_RECORD) return ish_error_set(err, ish_span_unknown(NULL), "record-field expects a record");
    const char *name = record_field_name(field);
    if (!name) return ish_error_set(err, ish_span_unknown(NULL), "record field name must be an atom, word, or string");
    if (ish_dict_get(value.as.obj->as.record.fields, field, out)) return true;
    return ish_error_set(err, ish_span_unknown(NULL), "record '%s' has no field '%s'", value.as.obj->as.record.type, name);
}

IshValue ish_car(IshValue pair, IshError *err) {
    if (pair.tag != ISH_VAL_PAIR) {
        ish_error_set(err, ish_span_unknown(NULL), "first expects a pair");
        return ish_nil();
    }
    return pair.as.obj->as.pair.car;
}

IshValue ish_cdr(IshValue pair, IshError *err) {
    if (pair.tag != ISH_VAL_PAIR) {
        ish_error_set(err, ish_span_unknown(NULL), "rest expects a pair");
        return ish_nil();
    }
    return pair.as.obj->as.pair.cdr;
}

size_t ish_sequence_count(IshValue value) {
    if (value.tag != ISH_VAL_TUPLE && value.tag != ISH_VAL_VECTOR) return 0;
    return value.as.obj->as.sequence.count;
}

IshValue ish_sequence_item(IshValue value, size_t index, IshError *err) {
    if (value.tag != ISH_VAL_TUPLE && value.tag != ISH_VAL_VECTOR) {
        ish_error_set(err, ish_span_unknown(NULL), "expected tuple or vector");
        return ish_nil();
    }
    if (index >= value.as.obj->as.sequence.count) {
        ish_error_set(err, ish_span_unknown(NULL), "sequence index out of bounds");
        return ish_nil();
    }
    return value.as.obj->as.sequence.items[index];
}

size_t ish_dict_count(IshValue value) {
    return value.tag == ISH_VAL_DICT ? value.as.obj->as.dict.count : 0;
}

bool ish_dict_get(IshValue dict, IshValue key, IshValue *out) {
    if (dict.tag != ISH_VAL_DICT) return false;
    for (size_t i = 0; i < dict.as.obj->as.dict.count; i++) {
        if (ish_value_equal(dict.as.obj->as.dict.entries[i].key, key)) {
            *out = dict.as.obj->as.dict.entries[i].value;
            return true;
        }
    }
    return false;
}

bool ish_dict_entry(IshValue dict, size_t index, IshValue *out_key, IshValue *out_value) {
    if (dict.tag != ISH_VAL_DICT || index >= dict.as.obj->as.dict.count) return false;
    *out_key = dict.as.obj->as.dict.entries[index].key;
    *out_value = dict.as.obj->as.dict.entries[index].value;
    return true;
}

const IshSyntax *ish_syntax_get(IshValue value, IshError *err) {
    if (value.tag != ISH_VAL_SYNTAX) {
        ish_error_set(err, ish_span_unknown(NULL), "expected syntax");
        return NULL;
    }
    return value.as.obj->as.syntax;
}

IshSyntax *ish_syntax_value_get(IshValue value) {
    return value.tag == ISH_VAL_SYNTAX ? value.as.obj->as.syntax : NULL;
}

const char *ish_string_bytes(IshValue value) {
    return value.tag == ISH_VAL_STRING ? value.as.obj->as.string.bytes : "";
}

size_t ish_string_length(IshValue value) {
    return value.tag == ISH_VAL_STRING ? value.as.obj->as.string.len : 0;
}

bool ish_value_equal(IshValue a, IshValue b) {
    if (a.tag != b.tag) return false;
    switch (a.tag) {
        case ISH_VAL_NIL: return true;
        case ISH_VAL_ATOM:
        case ISH_VAL_WORD: return a.as.symbol == b.as.symbol;
        case ISH_VAL_INT: return a.as.i == b.as.i;
        case ISH_VAL_FLOAT: return a.as.f == b.as.f;
        case ISH_VAL_STRING:
            return a.as.obj->as.string.len == b.as.obj->as.string.len &&
                   memcmp(a.as.obj->as.string.bytes, b.as.obj->as.string.bytes, a.as.obj->as.string.len) == 0;
        case ISH_VAL_PAIR:
            return ish_value_equal(a.as.obj->as.pair.car, b.as.obj->as.pair.car) &&
                   ish_value_equal(a.as.obj->as.pair.cdr, b.as.obj->as.pair.cdr);
        case ISH_VAL_TUPLE:
        case ISH_VAL_VECTOR:
            if (a.as.obj->as.sequence.count != b.as.obj->as.sequence.count) return false;
            for (size_t i = 0; i < a.as.obj->as.sequence.count; i++) {
                if (!ish_value_equal(a.as.obj->as.sequence.items[i], b.as.obj->as.sequence.items[i])) return false;
            }
            return true;
        case ISH_VAL_DICT:
            if (a.as.obj->as.dict.count != b.as.obj->as.dict.count) return false;
            for (size_t i = 0; i < a.as.obj->as.dict.count; i++) {
                if (!ish_value_equal(a.as.obj->as.dict.entries[i].key, b.as.obj->as.dict.entries[i].key)) return false;
                if (!ish_value_equal(a.as.obj->as.dict.entries[i].value, b.as.obj->as.dict.entries[i].value)) return false;
            }
            return true;
        case ISH_VAL_SYNTAX:
            return a.as.obj == b.as.obj;
        case ISH_VAL_CELL:
            return a.as.obj == b.as.obj;
        case ISH_VAL_CLOSURE:
            return a.as.obj == b.as.obj;
        case ISH_VAL_RECORD:
            return strcmp(a.as.obj->as.record.type, b.as.obj->as.record.type) == 0 &&
                   ish_value_equal(a.as.obj->as.record.fields, b.as.obj->as.record.fields);
        case ISH_VAL_PID:
        case ISH_VAL_REF:
        case ISH_VAL_PORT:
        case ISH_VAL_PRIMITIVE:
            return a.as.id == b.as.id;
    }
    return false;
}

static bool write_escaped(IshBuffer *buf, const char *text, size_t len) {
    if (!ish_buf_append_char(buf, '"')) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        switch (ch) {
            case '\\': if (!ish_buf_append(buf, "\\\\")) return false; break;
            case '"': if (!ish_buf_append(buf, "\\\"")) return false; break;
            case '\n': if (!ish_buf_append(buf, "\\n")) return false; break;
            case '\r': if (!ish_buf_append(buf, "\\r")) return false; break;
            case '\t': if (!ish_buf_append(buf, "\\t")) return false; break;
            default:
                if (ch < 32u) {
                    if (!ish_buf_appendf(buf, "\\x%02x", ch)) return false;
                } else if (!ish_buf_append_char(buf, (char)ch)) return false;
        }
    }
    return ish_buf_append_char(buf, '"');
}

bool ish_value_write(IshBuffer *buf, IshValue value) {
    switch (value.tag) {
        case ISH_VAL_NIL: return ish_buf_append(buf, ":nil");
        case ISH_VAL_ATOM: return ish_buf_append_char(buf, ':') && ish_buf_append(buf, ish_symbol_text(value.as.symbol));
        case ISH_VAL_WORD: return ish_buf_append(buf, ish_symbol_text(value.as.symbol));
        case ISH_VAL_INT: return ish_buf_appendf(buf, "%lld", (long long)value.as.i);
        case ISH_VAL_FLOAT: return ish_buf_appendf(buf, "%g", value.as.f);
        case ISH_VAL_STRING: return write_escaped(buf, value.as.obj->as.string.bytes, value.as.obj->as.string.len);
        case ISH_VAL_PID: return ish_buf_appendf(buf, "#<pid:%llu>", (unsigned long long)value.as.id);
        case ISH_VAL_REF: return ish_buf_appendf(buf, "#<ref:%llu>", (unsigned long long)value.as.id);
        case ISH_VAL_PORT: return ish_buf_appendf(buf, "#<port:%llu>", (unsigned long long)value.as.id);
        case ISH_VAL_PRIMITIVE: return ish_buf_appendf(buf, "#<primitive:%llu>", (unsigned long long)value.as.id);
        case ISH_VAL_RECORD: return ish_buf_appendf(buf, "#<record:%s>", value.as.obj->as.record.type);
        case ISH_VAL_TUPLE:
        case ISH_VAL_VECTOR: {
            const char *open = value.tag == ISH_VAL_TUPLE ? "{" : "%[";
            const char *close = value.tag == ISH_VAL_TUPLE ? "}" : "]";
            if (!ish_buf_append(buf, open)) return false;
            for (size_t i = 0; i < value.as.obj->as.sequence.count; i++) {
                if (i != 0 && !ish_buf_append_char(buf, ' ')) return false;
                if (!ish_value_write(buf, value.as.obj->as.sequence.items[i])) return false;
            }
            return ish_buf_append(buf, close);
        }
        case ISH_VAL_DICT: {
            if (!ish_buf_append(buf, "%{")) return false;
            for (size_t i = 0; i < value.as.obj->as.dict.count; i++) {
                if (i != 0 && !ish_buf_append_char(buf, ' ')) return false;
                if (!ish_value_write(buf, value.as.obj->as.dict.entries[i].key)) return false;
                if (!ish_buf_append_char(buf, ' ')) return false;
                if (!ish_value_write(buf, value.as.obj->as.dict.entries[i].value)) return false;
            }
            return ish_buf_append_char(buf, '}');
        }
        case ISH_VAL_SYNTAX: return ish_buf_append(buf, "#<syntax>");
        case ISH_VAL_CELL: return ish_buf_append(buf, "#<cell>");
        case ISH_VAL_CLOSURE: return ish_buf_appendf(buf, "#<closure:%u>", ish_closure_function_index(value));
        case ISH_VAL_PAIR: {
            if (!ish_buf_append_char(buf, '(')) return false;
            IshValue cur = value;
            bool first = true;
            while (cur.tag == ISH_VAL_PAIR) {
                if (!first && !ish_buf_append_char(buf, ' ')) return false;
                if (!ish_value_write(buf, cur.as.obj->as.pair.car)) return false;
                cur = cur.as.obj->as.pair.cdr;
                first = false;
            }
            if (cur.tag != ISH_VAL_NIL) {
                if (!ish_buf_append(buf, " . ")) return false;
                if (!ish_value_write(buf, cur)) return false;
            }
            return ish_buf_append_char(buf, ')');
        }
    }
    return false;
}
