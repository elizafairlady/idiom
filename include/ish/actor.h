#ifndef ISH_ACTOR_H
#define ISH_ACTOR_H

#include "ish/value.h"
#include "ish/vm.h"

typedef enum {
    ISH_RECV_TIMEOUT,
    ISH_RECV_BLOCK
} IshRecvDecision;

IshScheduler *ish_sched_create(IshRuntime *rt, const IshBytecodeModule *module, IshError *err);
void ish_sched_destroy(IshScheduler *sched);
bool ish_sched_run_main(IshScheduler *sched, uint32_t main_fn, IshValue *out_result, IshError *err);

uint64_t ish_actor_pid(const IshActor *actor);
bool ish_actor_trap_exit_get(const IshActor *actor);
void ish_actor_trap_exit_set(IshActor *actor, bool on);

size_t ish_actor_mailbox_count(const IshActor *actor);
bool ish_actor_mailbox_peek(const IshActor *actor, size_t index, IshValue *out);
bool ish_actor_mailbox_remove(IshActor *actor, size_t index, IshValue *out);

bool ish_actor_recv_no_match(IshActor *actor, IshValue timeout, IshRecvDecision *out, IshError *err);
void ish_actor_recv_reset(IshActor *actor);
size_t ish_actor_recv_cursor(const IshActor *actor);
void ish_actor_recv_set_cursor(IshActor *actor, size_t cursor);

bool ish_sched_spawn(IshScheduler *sched, IshValue thunk, const IshExec *parent, IshValue *out_pid, IshError *err);
void ish_sched_send(IshScheduler *sched, uint64_t target_pid, IshValue msg);
bool ish_sched_link(IshScheduler *sched, IshActor *self, uint64_t target_pid, bool *self_should_exit, IshValue *self_exit_reason, IshError *err);
bool ish_sched_unlink(IshScheduler *sched, IshActor *self, uint64_t target_pid);
bool ish_sched_monitor(IshScheduler *sched, IshActor *self, uint64_t target_pid, IshValue *out_ref, IshError *err);
bool ish_sched_demonitor(IshScheduler *sched, IshActor *self, IshValue ref);

#endif
