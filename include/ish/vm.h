#ifndef ISH_VM_H
#define ISH_VM_H

#include "ish/bytecode.h"

typedef struct {
    size_t max_stack;
    size_t max_frames;
} IshVmLimits;

IshVmLimits ish_vm_default_limits(void);
bool ish_vm_run(IshRuntime *rt, const IshBytecodeModule *module, uint32_t function_index, IshValue *out, IshError *err);
bool ish_vm_run_limited(IshRuntime *rt, const IshBytecodeModule *module, uint32_t function_index, IshVmLimits limits, IshValue *out, IshError *err);
bool ish_vm_truthy(IshValue value);

#endif
