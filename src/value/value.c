#include "ish/value.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    ISH_OBJ_STRING,
    ISH_OBJ_PAIR,
    ISH_OBJ_TUPLE,
    ISH_OBJ_VECTOR,
    ISH_OBJ_DICT,
    ISH_OBJ_CELL,
    ISH_OBJ_CLOSURE
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
    uint32_t function_index;
    uint32_t *entries;
    size_t entry_count;
    IshValue *captures;
    size_t capture_count;
} IshClosureObj;

typedef struct {
    IshValue value;
} IshCellObj;

struct IshObject {
    IshObjectKind kind;
    bool marked;
    struct IshObject *next;
    union {
        IshStringObj string;
        IshPairObj pair;
        IshSequenceObj sequence;
        IshDictObj dict;
        IshCellObj cell;
        IshClosureObj closure;
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
    obj->next = heap->objects;
    heap->objects = obj;
    heap->object_count++;
    heap->bytes_allocated += sizeof(*obj);
    return obj;
}

static void object_free(IshObject *obj) {
    if (!obj) return;
    if (obj->kind == ISH_OBJ_STRING) free(obj->as.string.bytes);
    if (obj->kind == ISH_OBJ_TUPLE || obj->kind == ISH_OBJ_VECTOR) free(obj->as.sequence.items);
    if (obj->kind == ISH_OBJ_DICT) free(obj->as.dict.entries);
    if (obj->kind == ISH_OBJ_CLOSURE) {
        free(obj->as.closure.entries);
        free(obj->as.closure.captures);
    }
    free(obj);
}

void ish_runtime_init(IshRuntime *rt) {
    ish_intern_init(&rt->intern);
    ish_heap_init(&rt->heap);
}

void ish_runtime_destroy(IshRuntime *rt) {
    ish_heap_destroy(&rt->heap);
    ish_intern_destroy(&rt->intern);
}

void ish_intern_init(IshIntern *intern) {
    intern->symbols = NULL;
    intern->count = 0;
    intern->cap = 0;
    intern->next_id = 1u;
}

void ish_intern_destroy(IshIntern *intern) {
    if (!intern) return;
    for (size_t i = 0; i < intern->count; i++) free(intern->symbols[i].text);
    free(intern->symbols);
    intern->symbols = NULL;
    intern->count = 0;
    intern->cap = 0;
    intern->next_id = 1u;
}

IshSymbol *ish_intern(IshIntern *intern, IshSymbolKind kind, const char *text) {
    for (size_t i = 0; i < intern->count; i++) {
        if (intern->symbols[i].kind == kind && strcmp(intern->symbols[i].text, text) == 0) return &intern->symbols[i];
    }
    if (intern->count == intern->cap) {
        size_t cap = intern->cap ? intern->cap * 2u : 32u;
        IshSymbol *next = realloc(intern->symbols, cap * sizeof(*next));
        if (!next) return NULL;
        intern->symbols = next;
        intern->cap = cap;
    }
    IshSymbol *sym = &intern->symbols[intern->count++];
    sym->text = ish_strdup(text);
    if (!sym->text) {
        intern->count--;
        return NULL;
    }
    sym->id = intern->next_id++;
    sym->kind = kind;
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
    if (value.tag != ISH_VAL_STRING && value.tag != ISH_VAL_PAIR && value.tag != ISH_VAL_TUPLE && value.tag != ISH_VAL_VECTOR && value.tag != ISH_VAL_DICT && value.tag != ISH_VAL_CELL && value.tag != ISH_VAL_CLOSURE) return;
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
    }
}

void ish_heap_collect(IshHeap *heap, const IshValue *roots, size_t root_count) {
    for (size_t i = 0; i < root_count; i++) value_mark(roots[i]);
    IshObject **cursor = &heap->objects;
    while (*cursor) {
        IshObject *obj = *cursor;
        if (obj->marked) {
            obj->marked = false;
            cursor = &obj->next;
        } else {
            *cursor = obj->next;
            heap->object_count--;
            object_free(obj);
        }
    }
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
    }
    IshValue v;
    v.tag = ISH_VAL_DICT;
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

IshValue ish_closure_multi(IshRuntime *rt, const uint32_t *function_indexes, size_t function_count, const IshValue *captures, size_t capture_count, IshError *err) {
    if (function_count == 0) {
        ish_error_set(err, ish_span_unknown(NULL), "closure must have at least one function entry");
        return ish_nil();
    }
    IshObject *obj = heap_alloc(&rt->heap, ISH_OBJ_CLOSURE);
    if (!obj) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return ish_nil();
    }
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
    }
    IshValue v;
    v.tag = ISH_VAL_CLOSURE;
    v.as.obj = obj;
    return v;
}

IshValue ish_closure(IshRuntime *rt, uint32_t function_index, const IshValue *captures, size_t capture_count, IshError *err) {
    return ish_closure_multi(rt, &function_index, 1, captures, capture_count, err);
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

bool ish_is_cell(IshValue value) {
    return value.tag == ISH_VAL_CELL;
}

bool ish_is_closure(IshValue value) {
    return value.tag == ISH_VAL_CLOSURE;
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
        case ISH_VAL_CELL:
            return a.as.obj == b.as.obj;
        case ISH_VAL_CLOSURE:
            return a.as.obj == b.as.obj;
        case ISH_VAL_PID:
        case ISH_VAL_REF:
        case ISH_VAL_PORT:
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
