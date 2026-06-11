#ifndef IDM_PRIMS_H
#define IDM_PRIMS_H

#include "idiom/core.h"

bool idm_prim_invoke(IdmRuntime *rt, IdmPrimitive prim, IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err);
bool idm_prim_lookup_by_name(const char *name, IdmPrimitive *out);

#endif
