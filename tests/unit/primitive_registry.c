#include "idiom/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *name) {
    fprintf(stderr, "primitive_registry: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void check_lookup(const char *home, const char *name, IdmPrimitive expected) {
    IdmPrimitive got = 0;
    check(idm_primitive_lookup(home, name, &got), "lookup");
    check(got == expected, "lookup identity");
    const IdmPrimitiveInfo *info = idm_primitive_info(got);
    check(info != NULL, "info");
    check(strcmp(info->name, name) == 0, "name");
    check(strcmp(idm_primitive_home(got), home) == 0, "home");
}

static bool primitive_variadic_allowed(IdmPrimitive primitive) {
    switch (primitive) {
        case IDM_PRIM_LIST:
        case IDM_PRIM_TUPLE:
        case IDM_PRIM_VECTOR:
        case IDM_PRIM_DICT:
        case IDM_PRIM_STR:
        case IDM_PRIM_PRINT:
        case IDM_PRIM_PRINTLN:
        case IDM_PRIM_EPRINTLN:
            return true;
        default:
            return false;
    }
}

static void check_registry_roundtrip(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    for (size_t i = 0; i < idm_primitive_count(); i++) {
        IdmPrimitive primitive = (IdmPrimitive)i;
        const IdmPrimitiveInfo *info = idm_primitive_info(primitive);
        if (!info) continue;
        check(info->min_arity <= info->max_arity, "arity bounds");
        IdmPrimitive found = 0;
        check(idm_primitive_lookup(idm_primitive_home(primitive), info->name, &found), "roundtrip lookup");
        check(found == primitive, "roundtrip identity");

        IdmArity arity = idm_primitive_arity(primitive);
        check(arity.kind != IDM_ARITY_UNKNOWN, "arity known");
        check(arity.min == info->min_arity, "arity min");
        check(arity.max == info->max_arity, "arity max");
        check(idm_arity_accepts(&arity, info->min_arity), "arity accepts min");
        check(idm_arity_accepts(&arity, info->max_arity), "arity accepts max");
        if (info->min_arity > 0) check(!idm_arity_accepts(&arity, info->min_arity - 1u), "arity rejects below");
        if (info->max_arity < UINT32_MAX) check(!idm_arity_accepts(&arity, info->max_arity + 1u), "arity rejects above");
        if (info->max_arity == UINT32_MAX) check(primitive_variadic_allowed(primitive), "unexpected variadic primitive");
        if (info->max_arity != UINT32_MAX) check(!primitive_variadic_allowed(primitive), "variadic primitive must be unbounded");

        IdmCallableContract contract;
        bool has_contract = false;
        check(idm_primitive_contract(&rt, primitive, info->min_arity, &contract, &has_contract, &err, idm_span_unknown(NULL)), "primitive contract min");
        check(!err.present, "primitive contract min no error");
        if (!has_contract) fprintf(stderr, "primitive missing contract: %s/%u\n", info->name, info->min_arity);
        check(has_contract, "primitive contract min present");
        check(contract.sigs[0].has_result, "primitive contract min result");
        idm_callable_contract_destroy(&contract);
        if (info->max_arity != info->min_arity && info->max_arity != UINT32_MAX) {
            has_contract = false;
            check(idm_primitive_contract(&rt, primitive, info->max_arity, &contract, &has_contract, &err, idm_span_unknown(NULL)), "primitive contract max");
            check(!err.present, "primitive contract max no error");
            if (!has_contract) fprintf(stderr, "primitive missing contract: %s/%u\n", info->name, info->max_arity);
            check(has_contract, "primitive contract max present");
            check(contract.sigs[0].has_result, "primitive contract max result");
            idm_callable_contract_destroy(&contract);
        }
    }
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

int idm_unit_primitive_registry(void) {
    check_lookup("kernel", "add", IDM_PRIM_ADD);
    check_lookup("port", "port-read", IDM_PRIM_PORT_READ);
    check_lookup("regex", "raw-compile", IDM_PRIM_REGEX_COMPILE);
    check_lookup("system", "env-get", IDM_PRIM_ENV_GET);
    check_lookup("math", "sqrt", IDM_PRIM_SQRT);
    check(!idm_primitive_lookup("kernel", "raw-compile", NULL), "home mismatch");
    check(!idm_primitive_lookup("missing", "add", NULL), "missing home");
    check(idm_primitive_home_exists("kernel"), "kernel home");
    check(idm_primitive_home_exists("port"), "port home");
    check(idm_primitive_home_exists("regex"), "regex home");
    check(idm_primitive_home_exists("system"), "system home");
    check(idm_primitive_home_exists("math"), "math home");
    check(!idm_primitive_home_exists("missing"), "missing home exists");
    check_registry_roundtrip();
    return 0;
}
