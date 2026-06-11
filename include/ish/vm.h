#ifndef ISH_VM_H
#define ISH_VM_H

#include "ish/bytecode.h"

typedef struct IshScheduler IshScheduler;
typedef struct IshActor IshActor;
typedef struct IshExec IshExec;

typedef struct {
    size_t max_stack;
    size_t max_frames;
} IshVmLimits;

typedef enum {
    ISH_EXEC_DONE,
    ISH_EXEC_YIELD,
    ISH_EXEC_BLOCK_RECEIVE,
    ISH_EXEC_LAUNCH_PORT,
    ISH_EXEC_BLOCK_AWAIT,
    ISH_EXEC_EXIT
} IshExecStatus;

IshVmLimits ish_vm_default_limits(void);
bool ish_vm_run(IshRuntime *rt, const IshBytecodeModule *module, uint32_t function_index, IshValue *out, IshError *err);
bool ish_vm_run_limited(IshRuntime *rt, const IshBytecodeModule *module, uint32_t function_index, IshVmLimits limits, IshValue *out, IshError *err);
bool ish_vm_call_function(IshRuntime *rt, const IshBytecodeModule *module, uint32_t function_index, const IshValue *args, uint32_t argc, IshValue *out, IshError *err);
bool ish_vm_call_closure(IshRuntime *rt, IshValue closure, const IshValue *args, uint32_t argc, IshValue *out, IshError *err);
bool ish_vm_truthy(IshValue value);

IshExec *ish_exec_create(IshRuntime *rt, const IshBytecodeModule *module, IshScheduler *sched, IshActor *self, IshVmLimits limits, IshError *err);
void ish_exec_destroy(IshExec *exec);
bool ish_exec_setup_function(IshExec *exec, uint32_t function_index, IshError *err);
bool ish_exec_setup_thunk(IshExec *exec, IshValue closure, IshError *err);
bool ish_exec_step(IshExec *exec, int64_t budget, IshExecStatus *status, IshValue *out_result, IshValue *out_reason, IshError *err);
bool ish_exec_take_port_request(IshExec *exec, IshValue *out_graph);
bool ish_exec_take_await(IshExec *exec, IshValue *out_port);
bool ish_exec_push_result(IshExec *exec, IshValue value, IshError *err);

typedef void (*IshRootVisitor)(void *user, IshValue value);
void ish_exec_visit_roots(const IshExec *exec, IshRootVisitor visit, void *user);

const char *ish_exec_cwd(const IshExec *exec);
bool ish_exec_set_cwd(IshExec *exec, const char *cwd);
const char *ish_exec_env_get(const IshExec *exec, const char *name);
bool ish_exec_env_set(IshExec *exec, const char *name, const char *value);
size_t ish_exec_env_count(const IshExec *exec);
bool ish_exec_env_entry(const IshExec *exec, size_t index, const char **out_name, const char **out_value);
bool ish_exec_copy_context(IshExec *dst, const IshExec *src);

#endif
