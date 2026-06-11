#ifndef ISH_PRIMS_H
#define ISH_PRIMS_H

#include "ish/core.h"

bool ish_prim_invoke(IshRuntime *rt, IshPrimitive prim, IshValue *args, uint32_t argc, IshValue *out, IshError *err);
bool ish_prim_lookup_by_name(const char *name, IshPrimitive *out);

#endif
