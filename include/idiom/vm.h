#ifndef IDM_VM_H
#define IDM_VM_H

#include "idiom/bytecode.h"

typedef struct IdmScheduler IdmScheduler;
typedef struct IdmActor IdmActor;
typedef struct IdmExec IdmExec;

typedef struct {
    size_t max_registers;
    size_t max_frames;
} IdmVmLimits;

typedef enum {
    IDM_EXEC_DONE,
    IDM_EXEC_YIELD,
    IDM_EXEC_BLOCK_RECEIVE,
    IDM_EXEC_LAUNCH_PORT,
    IDM_EXEC_BLOCK_AWAIT,
    IDM_EXEC_BLOCK_PORT_READ,
    IDM_EXEC_BLOCK_PORT_WRITE,
    IDM_EXEC_BLOCK_TTY,
    IDM_EXEC_EXIT
} IdmExecStatus;

IdmVmLimits idm_vm_default_limits(void);
bool idm_vm_run(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, IdmValue *out, IdmError *err);
bool idm_vm_run_limited(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, IdmVmLimits limits, IdmValue *out, IdmError *err);
bool idm_vm_run_in_env(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, IdmEnv *env, IdmValue *out, IdmError *err);
bool idm_vm_call_function(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err);
bool idm_vm_call_closure(IdmRuntime *rt, IdmValue closure, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err);

IdmExec *idm_exec_create(IdmRuntime *rt, const IdmBytecodeModule *module, IdmScheduler *sched, IdmActor *self, IdmVmLimits limits, IdmError *err);
void idm_exec_destroy(IdmExec *exec);
bool idm_exec_setup_function(IdmExec *exec, uint32_t function_index, IdmError *err);
bool idm_exec_setup_thunk(IdmExec *exec, IdmValue closure, IdmError *err);
bool idm_exec_step(IdmExec *exec, int64_t budget, IdmExecStatus *status, IdmValue *out_result, IdmValue *out_reason, IdmError *err);
bool idm_exec_take_port_request(IdmExec *exec, IdmValue *out_graph);
bool idm_exec_take_await(IdmExec *exec, IdmValue *out_port);
bool idm_exec_take_port_read(IdmExec *exec, IdmValue *out_port, const char **out_stream, size_t *out_max);
bool idm_exec_take_port_write(IdmExec *exec, IdmValue *out_port, IdmValue *out_data);
bool idm_exec_take_tty(IdmExec *exec, bool *out_line_mode, bool *out_has_timeout, int64_t *out_timeout_ms);
bool idm_exec_push_result(IdmExec *exec, IdmValue value, IdmError *err);
void idm_exec_inject_raise(IdmExec *exec, IdmValue reason);
IdmScheduler *idm_exec_scheduler(const IdmExec *exec);
IdmExec *idm_current_exec(void);

typedef struct {
    size_t frame;
    uint32_t section;
    size_t index;
} IdmExecRootCursor;
void idm_exec_root_cursor_init(IdmExecRootCursor *cursor);
bool idm_exec_mark_roots_step(const IdmExec *exec, IdmExecRootCursor *cursor, int64_t *budget, IdmHeap *heap);

const char *idm_exec_cwd(const IdmExec *exec);
bool idm_exec_set_cwd(IdmExec *exec, const char *cwd);
const char *idm_exec_env_get(const IdmExec *exec, const char *name);
bool idm_exec_env_set(IdmExec *exec, const char *name, const char *value);
size_t idm_exec_env_count(const IdmExec *exec);
bool idm_exec_env_entry(const IdmExec *exec, size_t index, const char **out_name, const char **out_value);
bool idm_exec_copy_context(IdmExec *dst, const IdmExec *src);

#endif
