#ifndef IDM_ACTOR_H
#define IDM_ACTOR_H

#include "idiom/value.h"
#include "idiom/vm.h"

typedef enum {
    IDM_RECV_TIMEOUT,
    IDM_RECV_BLOCK
} IdmRecvDecision;

IdmScheduler *idm_sched_create(IdmRuntime *rt, const IdmBytecodeModule *module, IdmError *err);
void idm_sched_destroy(IdmScheduler *sched);
bool idm_sched_run_main(IdmScheduler *sched, uint32_t main_fn, IdmValue *out_result, IdmError *err);

uint64_t idm_actor_pid(const IdmActor *actor);
bool idm_actor_trap_exit_get(const IdmActor *actor);
void idm_actor_trap_exit_set(IdmActor *actor, bool on);

size_t idm_actor_mailbox_count(const IdmActor *actor);
bool idm_actor_mailbox_peek(const IdmActor *actor, size_t index, IdmValue *out);
bool idm_actor_mailbox_remove(IdmActor *actor, size_t index, IdmValue *out);

bool idm_actor_recv_no_match(IdmActor *actor, IdmValue timeout, IdmRecvDecision *out, IdmError *err);
void idm_actor_recv_reset(IdmActor *actor);
size_t idm_actor_recv_cursor(const IdmActor *actor);
void idm_actor_recv_set_cursor(IdmActor *actor, size_t cursor);

bool idm_sched_spawn(IdmScheduler *sched, IdmValue thunk, const IdmExec *parent, IdmValue *out_pid, IdmError *err);
void idm_sched_send(IdmScheduler *sched, uint64_t target_pid, IdmValue msg);
bool idm_sched_link(IdmScheduler *sched, IdmActor *self, uint64_t target_pid, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err);
bool idm_sched_unlink(IdmScheduler *sched, IdmActor *self, uint64_t target_pid);
bool idm_sched_monitor(IdmScheduler *sched, IdmActor *self, uint64_t target_pid, IdmValue *out_ref, IdmError *err);
bool idm_sched_demonitor(IdmScheduler *sched, IdmActor *self, IdmValue ref);

#endif
