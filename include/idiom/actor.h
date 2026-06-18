#ifndef IDM_ACTOR_H
#define IDM_ACTOR_H

#include "idiom/value.h"
#include "idiom/vm.h"

typedef struct IdmPort IdmPort;

typedef enum {
    IDM_RECV_TIMEOUT,
    IDM_RECV_BLOCK
} IdmRecvDecision;

IdmScheduler *idm_sched_create(IdmRuntime *rt, const IdmBytecodeModule *module, IdmError *err);
void idm_sched_destroy(IdmScheduler *sched);
bool idm_sched_run_main(IdmScheduler *sched, uint32_t main_fn, IdmValue *out_result, IdmError *err);
bool idm_sched_eval(IdmScheduler *sched, IdmValue thunk, IdmValue *out_value, IdmError *err);
bool idm_sched_run_session(IdmScheduler *sched, IdmValue thunk, bool register_session, IdmValue *out_value, IdmValue *out_reason, IdmError *err);
int idm_sched_session_status(IdmScheduler *sched, IdmValue value, IdmValue reason);
void idm_sched_watch(IdmScheduler *sched, uint64_t pid);
char *idm_sched_take_diagnostic(IdmScheduler *sched);
bool idm_signals_install(IdmError *err);
bool idm_sched_port_status(IdmScheduler *sched, uint64_t port_id, int *out_state);
bool idm_sched_register_port(IdmScheduler *sched, IdmPort *port, IdmValue *out_port, IdmError *err);
bool idm_sched_port_read(IdmScheduler *sched, uint64_t port_id, const char *stream, size_t max, IdmValue *out, bool *out_found, IdmError *err);
bool idm_sched_port_write(IdmScheduler *sched, uint64_t port_id, const char *data, size_t len, IdmValue *out, bool *out_found, IdmError *err);
bool idm_sched_port_close_input(IdmScheduler *sched, uint64_t port_id, IdmValue *out, bool *out_found, IdmError *err);
bool idm_sched_job_resume(IdmScheduler *sched, uint64_t port_id, bool fg);
bool idm_sched_job_signal(IdmScheduler *sched, uint64_t port_id, int signo);

uint64_t idm_actor_pid(const IdmActor *actor);
bool idm_actor_trap_exit_get(const IdmActor *actor);
void idm_actor_trap_exit_set(IdmActor *actor, bool on);

size_t idm_actor_mailbox_count(const IdmActor *actor);
bool idm_actor_mailbox_peek(const IdmActor *actor, size_t index, IdmValue *out);
bool idm_actor_mailbox_remove(IdmActor *actor, size_t index, IdmValue *out);

bool idm_actor_recv_no_match(IdmActor *actor, IdmValue timeout, IdmRecvDecision *out, IdmError *err);
void idm_actor_recv_set_cursor(IdmActor *actor, size_t cursor);
size_t idm_actor_recv_start(IdmActor *actor, const void *module, size_t site);
void idm_actor_recv_matched(IdmActor *actor, size_t cursor);

bool idm_sched_spawn(IdmScheduler *sched, IdmValue thunk, const IdmExec *parent, IdmValue *out_pid, IdmError *err);
bool idm_sched_spawn_link(IdmScheduler *sched, IdmValue thunk, const IdmExec *parent, IdmActor *self, IdmValue *out_pid, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err);
bool idm_sched_spawn_monitor(IdmScheduler *sched, IdmValue thunk, const IdmExec *parent, IdmActor *self, IdmValue *out_pid, IdmValue *out_ref, IdmError *err);
void idm_sched_send(IdmScheduler *sched, uint64_t target_pid, IdmValue msg);
bool idm_sched_link(IdmScheduler *sched, IdmActor *self, uint64_t target_pid, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err);
bool idm_sched_exit_signal(IdmScheduler *sched, IdmActor *self, uint64_t target_pid, IdmValue reason, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err);
bool idm_sched_unlink(IdmScheduler *sched, IdmActor *self, uint64_t target_pid);
bool idm_sched_monitor(IdmScheduler *sched, IdmActor *self, uint64_t target_pid, IdmValue *out_ref, IdmError *err);
bool idm_sched_demonitor(IdmScheduler *sched, IdmActor *self, IdmValue ref);

#endif
