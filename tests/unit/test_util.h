#ifndef IDM_TEST_UTIL_H
#define IDM_TEST_UTIL_H

#include "idiom/actor.h"
#include "idiom/common.h"
#include "idiom/core.h"
#include "idiom/expand.h"
#include "idiom/ports.h"
#include "idiom/reader.h"
#include "idiom/scope.h"
#include "idiom/value.h"
#include "idiom/vm.h"

#include <errno.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int failures;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define CHECK_STR(actual, expected) do { if (strcmp((actual), (expected)) != 0) { fprintf(stderr, "CHECK_STR failed at %s:%d:\nactual:   %s\nexpected: %s\n", __FILE__, __LINE__, (actual), (expected)); failures++; } } while (0)

static inline bool test_emit_load_const(IdmBytecodeModule *module, uint32_t dst, uint32_t constant) {
    return idm_bc_emit_u32(module, IDM_OP_LOAD_CONST, dst, NULL) &&
           idm_bc_emit(module, constant, NULL);
}

static inline bool test_emit_make_closure(IdmBytecodeModule *module, uint32_t dst, uint32_t function) {
    return idm_bc_emit_u32(module, IDM_OP_MAKE_CLOSURE, dst, NULL) &&
           idm_bc_emit(module, function, NULL);
}

static inline bool test_emit_call(IdmBytecodeModule *module, uint32_t dst, uint32_t callee, uint32_t first_arg, uint32_t argc, bool tail) {
    return idm_bc_emit_u32(module, IDM_OP_CALL, dst, NULL) &&
           idm_bc_emit(module, callee, NULL) &&
           idm_bc_emit(module, first_arg, NULL) &&
           idm_bc_emit(module, argc, NULL) &&
           idm_bc_emit(module, tail ? 1u : 0u, NULL);
}

static inline bool test_emit_return(IdmBytecodeModule *module, uint32_t src) {
    return idm_bc_emit_u32(module, IDM_OP_RETURN, src, NULL);
}

static inline bool test_emit_move(IdmBytecodeModule *module, uint32_t dst, uint32_t src) {
    return idm_bc_emit_u32(module, IDM_OP_MOVE, dst, NULL) &&
           idm_bc_emit(module, src, NULL);
}

char *dump_reader(const char *src);
void check_operator_eval(IdmRuntime *rt, const char *source, const char *expect_dump, int64_t expect_value);
void check_value_written(IdmRuntime *rt, const char *source, const char *expect_written);
void expect_runtime_error_contains(IdmRuntime *rt, const char *label, const char *source, const char *expect_substring);
void expect_expand_result_rt(IdmRuntime *rt, const char *label, const char *source, bool should_succeed);
void expect_expand_error_rt(IdmRuntime *rt, const char *label, const char *source, const char *expect_substring);
void expect_expand_error_note_rt(IdmRuntime *rt, const char *label, const char *source, const char *expect_message, const char *expect_note);
void expect_runtime_error_note(IdmRuntime *rt, const char *label, const char *source, const char *expect_message, const char *expect_note);
void expect_expand_result(const char *label, const char *source, bool should_succeed);
void check_sched_value_written(IdmRuntime *rt, const char *source, const char *expect_written);
size_t count_procsub_temps(void);
size_t count_glob_matches(const char *pattern);
bool write_text_file(const char *path, const char *content);
void check_pkg_value(const char *source, const char *expect);

void run_substrate_suite(void);
void run_surface_suite(void);
void run_macro_suite(void);
void run_traits_suite(void);
void run_runtime_suite(void);
void run_package_suite(void);
void run_diagnostics_suite(void);
void run_session_suite(void);
void run_tty_suite(void);

#endif
