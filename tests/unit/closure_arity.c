#include "idiom/bytecode.h"

#include <stdio.h>
#include <stdlib.h>

static void fail(const char *name) {
    fprintf(stderr, "closure_arity: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void check_error(IdmError *err, const char *name) {
    if (err->present) fail(name);
}

static void check_arity(IdmValue closure, IdmArity want, const char *name) {
    IdmArity got = idm_arity_unknown();
    check(idm_closure_arity(closure, &got), name);
    check(idm_arity_equal(&got, &want), name);
}

int idm_unit_closure_arity(void) {
    IdmError err;
    idm_error_init(&err);

    IdmRuntime rt;
    idm_runtime_init(&rt);

    IdmBytecodeModule module;
    idm_bc_init(&module);

    uint32_t zero = 0;
    uint32_t two = 0;
    uint32_t variadic = 0;
    check(idm_bc_add_primitive_function(&module, "zero", idm_arity_exact(0u), 0u, &zero), "add zero");
    check(idm_bc_add_primitive_function(&module, "two", idm_arity_exact(2u), 0u, &two), "add two");
    check(idm_bc_add_primitive_function(&module, "variadic", idm_arity_range(0u, UINT32_MAX), 0u, &variadic), "add variadic");
    check(idm_bc_intern_literals(&rt, &module, &err), "finalize");
    check_error(&err, "finalize error");

    IdmValue zero_closure = idm_closure_in_module(&rt, &module, zero, NULL, 0u, rt.main_env, &err);
    check_error(&err, "zero closure error");
    check_arity(zero_closure, idm_arity_exact(0u), "zero arity");

    IdmValue variadic_closure = idm_closure_in_module(&rt, &module, variadic, NULL, 0u, rt.main_env, &err);
    check_error(&err, "variadic closure error");
    check_arity(variadic_closure, idm_arity_range(0u, UINT32_MAX), "variadic arity");

    uint32_t entries[2] = { zero, two };
    IdmPatternSelector *selector = NULL;
    check(idm_bc_build_selector_for_entries(&rt, &module, entries, 2u, &selector, &err), "multi selector");
    check_error(&err, "multi selector error");
    IdmValue multi = idm_closure_multi_selectable_in_module(&rt, &module, entries, 2u, selector, NULL, 0u, rt.main_env, &err);
    idm_pattern_selector_free(selector);
    check_error(&err, "multi closure error");
    IdmArity multi_arity = idm_arity_unknown();
    check(idm_arity_add_exact(&multi_arity, 0u), "multi add zero");
    check(idm_arity_add_exact(&multi_arity, 2u), "multi add two");
    check_arity(multi, multi_arity, "multi arity");

    IdmArity nonclosure_arity = idm_arity_exact(7u);
    check(!idm_closure_arity(idm_int(1), &nonclosure_arity), "nonclosure arity");
    check(nonclosure_arity.kind == IDM_ARITY_UNKNOWN, "nonclosure unknown");

    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
    return 0;
}
