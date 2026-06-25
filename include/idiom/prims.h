#ifndef IDM_PRIMS_H
#define IDM_PRIMS_H

#include "idiom/core.h"

bool idm_prim_invoke(IdmRuntime *rt, IdmPrimitive prim, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err);

#endif
