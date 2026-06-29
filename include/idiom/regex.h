#ifndef IDM_REGEX_H
#define IDM_REGEX_H

#include "idiom/pattern.h"

typedef enum {
    IDM_REGEX_CASELESS = 1u << 0,
    IDM_REGEX_MULTILINE = 1u << 1,
    IDM_REGEX_DOTALL = 1u << 2
} IdmRegexFlags;

typedef struct IdmRegex IdmRegex;
typedef struct IdmRegexSet IdmRegexSet;
typedef struct IdmRegexResult IdmRegexResult;

bool idm_regex_compile(const char *source, size_t source_len, uint32_t flags, IdmRegex **out, IdmError *err);
IdmRegex *idm_regex_clone(const IdmRegex *rx, IdmError *err);
void idm_regex_free(IdmRegex *rx);
bool idm_regex_set_compile(const IdmRegex *const *items, size_t count, IdmRegexSet **out, IdmError *err);
void idm_regex_set_free(IdmRegexSet *set);
size_t idm_regex_set_count(const IdmRegexSet *set);
size_t idm_regex_footprint(const IdmRegex *rx);
const char *idm_regex_source(const IdmRegex *rx, size_t *out_len);
uint32_t idm_regex_flags(const IdmRegex *rx);
size_t idm_regex_group_count(const IdmRegex *rx);
const char *idm_regex_group_name(const IdmRegex *rx, size_t index);
bool idm_regex_nullable(const IdmRegex *rx);
size_t idm_regex_set_group_count(const IdmRegexSet *set, size_t item_index);
const char *idm_regex_set_group_name(const IdmRegexSet *set, size_t item_index, size_t group_index);
bool idm_regex_set_matches_empty(const IdmRegexSet *set, bool *out, IdmError *err);
bool idm_regex_set_serialize(IdmBuffer *out, const IdmRegexSet *set, IdmError *err);
bool idm_regex_set_deserialize(IdmByteReader *r, IdmRegexSet **out, IdmError *err);

void idm_regex_result_free(IdmRegexResult *result);
IdmRegexResult *idm_regex_result_clone(const IdmRegexResult *result);
IdmRegexResult *idm_regex_result_clone_with_subject(const IdmRegexResult *result, IdmValue subject, IdmError *err);
size_t idm_regex_result_footprint(const IdmRegexResult *result);
IdmValue idm_regex_result_subject_value(const IdmRegexResult *result);

bool idm_regex_test_bytes(const IdmRegex *rx, const char *input, size_t input_len, bool *out_matched, IdmError *err);
bool idm_regex_match_at(const IdmRegex *rx, const char *input, size_t input_len, size_t offset, size_t *out_end, IdmError *err);
bool idm_regex_set_match_at(const IdmRegexSet *set, const char *input, size_t input_len, size_t offset, size_t *out_index, size_t *out_end, bool *out_matched, IdmError *err);
bool idm_regex_set_exec_at(const IdmRegexSet *set, const char *input, size_t input_len, size_t offset, IdmByteMatch *out, IdmError *err);
bool idm_regex_exec_at_subject(const IdmRegex *rx, IdmValue subject, const char *input, size_t input_len, size_t offset, bool full, IdmRegexResult **out, IdmError *err);
bool idm_regex_scan_subject(const IdmRegex *rx, IdmValue subject, const char *input, size_t input_len, size_t offset, IdmRegexResult **out, IdmError *err);

bool idm_regex_compile_value(IdmRuntime *rt, IdmValue source, IdmValue options, IdmValue *out, IdmError *err);
bool idm_regex_options_value(IdmRuntime *rt, IdmValue regex, IdmValue *out, IdmError *err);
bool idm_regex_group_names_value(IdmRuntime *rt, IdmValue regex, IdmValue *out, IdmError *err);

bool idm_regex_scan_at(IdmRuntime *rt, IdmValue regex, IdmValue input, size_t offset, IdmValue *out, IdmError *err);
bool idm_regex_scan_from(IdmRuntime *rt, IdmValue regex, IdmValue input, size_t offset, IdmValue *out, IdmError *err);
bool idm_regex_scan_full(IdmRuntime *rt, IdmValue regex, IdmValue input, IdmValue *out, IdmError *err);
bool idm_regex_test(IdmRuntime *rt, IdmValue regex, IdmValue input, IdmValue *out, IdmError *err);
bool idm_regex_scan_all(IdmRuntime *rt, IdmValue regex, IdmValue input, IdmValue *out, IdmError *err);

bool idm_regex_result_start_value(IdmRuntime *rt, IdmValue result, IdmValue *out, IdmError *err);
bool idm_regex_result_end_value(IdmRuntime *rt, IdmValue result, IdmValue *out, IdmError *err);
bool idm_regex_result_text_value(IdmRuntime *rt, IdmValue result, IdmValue *out, IdmError *err);
bool idm_regex_capture_value(IdmRuntime *rt, IdmValue result, IdmValue index, IdmValue *out, IdmError *err);
bool idm_regex_capture_range_value(IdmRuntime *rt, IdmValue result, IdmValue index, IdmValue *out, IdmError *err);
bool idm_regex_capture_named_value(IdmRuntime *rt, IdmValue result, IdmValue name, IdmValue *out, IdmError *err);
bool idm_regex_captures_value(IdmRuntime *rt, IdmValue result, IdmValue *out, IdmError *err);

bool idm_regex_replace(IdmRuntime *rt, IdmValue regex, IdmValue input, IdmValue replacement, bool all, IdmValue *out, IdmError *err);
bool idm_regex_split_on(IdmRuntime *rt, IdmValue regex, IdmValue input, IdmValue *out, IdmError *err);
bool idm_regex_escape(IdmRuntime *rt, IdmValue input, IdmValue *out, IdmError *err);

#endif
