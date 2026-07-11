#ifndef IDM_PRIMS_H
#define IDM_PRIMS_H

#include "idiom/core.h"

bool idm_primitive_require_arity(IdmRuntime *rt, IdmPrimitive prim, uint32_t argc, IdmError *err);
bool idm_primitive_type_error(IdmRuntime *rt, IdmError *err, const char *name, IdmValue got, const char *what);
bool idm_primitive_require_string_arg(IdmRuntime *rt, IdmValue value, const char **out_s, size_t *out_len, const char *name, IdmError *err);
bool idm_prim_invoke(IdmRuntime *rt, IdmPrimitive prim, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err);

#endif
