#ifndef ISH_VALUE_H
#define ISH_VALUE_H

#include "ish/common.h"

typedef struct IshObject IshObject;
typedef struct IshSymbol IshSymbol;

typedef enum {
    ISH_SYMBOL_WORD,
    ISH_SYMBOL_ATOM
} IshSymbolKind;

typedef enum {
    ISH_VAL_NIL,
    ISH_VAL_ATOM,
    ISH_VAL_WORD,
    ISH_VAL_INT,
    ISH_VAL_FLOAT,
    ISH_VAL_STRING,
    ISH_VAL_PAIR,
    ISH_VAL_TUPLE,
    ISH_VAL_VECTOR,
    ISH_VAL_DICT,
    ISH_VAL_CELL,
    ISH_VAL_CLOSURE,
    ISH_VAL_PID,
    ISH_VAL_REF,
    ISH_VAL_PORT
} IshValueTag;

typedef struct {
    IshValueTag tag;
    union {
        int64_t i;
        double f;
        IshObject *obj;
        IshSymbol *symbol;
        uint64_t id;
    } as;
} IshValue;

typedef struct {
    IshValue key;
    IshValue value;
} IshDictEntry;

typedef struct {
    IshSymbol *symbols;
    size_t count;
    size_t cap;
    uint32_t next_id;
} IshIntern;

typedef struct {
    IshObject *objects;
    size_t object_count;
    size_t bytes_allocated;
} IshHeap;

typedef struct {
    IshIntern intern;
    IshHeap heap;
} IshRuntime;

void ish_runtime_init(IshRuntime *rt);
void ish_runtime_destroy(IshRuntime *rt);

void ish_intern_init(IshIntern *intern);
void ish_intern_destroy(IshIntern *intern);
IshSymbol *ish_intern(IshIntern *intern, IshSymbolKind kind, const char *text);
const char *ish_symbol_text(const IshSymbol *sym);
uint32_t ish_symbol_id(const IshSymbol *sym);
IshSymbolKind ish_symbol_kind(const IshSymbol *sym);

void ish_heap_init(IshHeap *heap);
void ish_heap_destroy(IshHeap *heap);
void ish_heap_collect(IshHeap *heap, const IshValue *roots, size_t root_count);
size_t ish_heap_object_count(const IshHeap *heap);

IshValue ish_nil(void);
IshValue ish_int(int64_t value);
IshValue ish_float(double value);
IshValue ish_word(IshRuntime *rt, const char *text);
IshValue ish_atom(IshRuntime *rt, const char *text);
IshValue ish_string(IshRuntime *rt, const char *text, IshError *err);
IshValue ish_string_n(IshRuntime *rt, const char *text, size_t len, IshError *err);
IshValue ish_cons(IshRuntime *rt, IshValue car, IshValue cdr, IshError *err);
IshValue ish_tuple(IshRuntime *rt, const IshValue *items, size_t count, IshError *err);
IshValue ish_vector(IshRuntime *rt, const IshValue *items, size_t count, IshError *err);
IshValue ish_dict(IshRuntime *rt, const IshDictEntry *entries, size_t count, IshError *err);
IshValue ish_cell(IshRuntime *rt, IshValue initial, IshError *err);
IshValue ish_closure(IshRuntime *rt, uint32_t function_index, const IshValue *captures, size_t capture_count, IshError *err);
IshValue ish_closure_multi(IshRuntime *rt, const uint32_t *function_indexes, size_t function_count, const IshValue *captures, size_t capture_count, IshError *err);
IshValue ish_pid(uint64_t id);
IshValue ish_ref(uint64_t id);
IshValue ish_port(uint64_t id);

bool ish_is_nil(IshValue value);
bool ish_is_pair(IshValue value);
bool ish_is_tuple(IshValue value);
bool ish_is_vector(IshValue value);
bool ish_is_dict(IshValue value);
bool ish_is_cell(IshValue value);
bool ish_is_closure(IshValue value);
IshValue ish_cell_get(IshValue cell, IshError *err);
bool ish_cell_set(IshValue cell, IshValue value, IshError *err);
uint32_t ish_closure_function_index(IshValue value);
size_t ish_closure_entry_count(IshValue value);
uint32_t ish_closure_entry(IshValue value, size_t index, IshError *err);
size_t ish_closure_capture_count(IshValue value);
IshValue ish_closure_capture(IshValue value, size_t index, IshError *err);
IshValue ish_car(IshValue pair, IshError *err);
IshValue ish_cdr(IshValue pair, IshError *err);
size_t ish_sequence_count(IshValue value);
IshValue ish_sequence_item(IshValue value, size_t index, IshError *err);
size_t ish_dict_count(IshValue value);
bool ish_dict_get(IshValue dict, IshValue key, IshValue *out);
bool ish_value_equal(IshValue a, IshValue b);
bool ish_value_write(IshBuffer *buf, IshValue value);

#endif
