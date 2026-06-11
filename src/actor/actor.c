#include "ish/actor.h"

#include "ish/ports.h"

#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ISH_ACTOR_REDUCTIONS 4096

typedef enum {
    ACT_READY,
    ACT_RUNNING,
    ACT_WAITING_RECEIVE,
    ACT_WAITING_PORT,
    ACT_WAITING_AWAIT,
    ACT_EXITED
} ActorStatus;

typedef struct {
    uint64_t id;
    IshPort *port;
    bool done;
    bool has_result;
    IshValue result;
} PortEntry;

typedef struct {
    uint64_t ref;
    uint64_t target;
} MonitorOut;

typedef struct {
    uint64_t ref;
    uint64_t watcher;
} MonitorIn;

struct IshActor {
    uint64_t pid;
    IshExec *exec;
    ActorStatus status;
    bool trap_exit;

    IshValue *mailbox;
    size_t mb_count;
    size_t mb_cap;

    uint64_t *links;
    size_t link_count;
    size_t link_cap;

    MonitorOut *mon_out;
    size_t mon_out_count;
    size_t mon_out_cap;

    MonitorIn *mon_in;
    size_t mon_in_count;
    size_t mon_in_cap;

    bool recv_waiting;
    bool recv_has_deadline;
    uint64_t recv_deadline_ms;
    size_t recv_cursor;

    IshPort *port;
    uint64_t await_port_id;

    bool exited;
    IshValue exit_reason;
};

struct IshScheduler {
    IshRuntime *rt;
    const IshBytecodeModule *module;
    IshVmLimits limits;

    IshActor **actors;
    size_t actor_count;
    size_t actor_cap;

    uint64_t *ready;
    size_t ready_head;
    size_t ready_count;
    size_t ready_cap;

    uint64_t next_ref;
    uint64_t main_pid;

    PortEntry *ports;
    size_t port_count;
    size_t port_cap;
    uint64_t next_port_id;

    bool main_terminated;
    bool main_abnormal;
    IshValue main_value;
    IshValue main_reason;
    char *crash_notes;
    size_t gc_threshold;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool reason_is_normal(IshValue reason) {
    return reason.tag == ISH_VAL_ATOM && strcmp(ish_symbol_text(reason.as.symbol), "normal") == 0;
}

static size_t gc_threshold_from_env(void) {
    const char *text = getenv("ISH_GC_THRESHOLD");
    if (text && *text) {
        char *end = NULL;
        unsigned long long value = strtoull(text, &end, 10);
        if (end != text && value > 0) return (size_t)value;
    }
    return 1u << 20;
}

static void gc_mark_root(void *user, IshValue value) {
    (void)user;
    ish_gc_mark_value(value);
}

static void gc_mark_pattern(const IshPattern *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case ISH_PAT_LITERAL: ish_gc_mark_value(pat->as.literal); break;
        case ISH_PAT_PAIR: gc_mark_pattern(pat->as.pair.left); gc_mark_pattern(pat->as.pair.right); break;
        case ISH_PAT_LIST:
        case ISH_PAT_VECTOR:
        case ISH_PAT_TUPLE:
            for (size_t i = 0; i < pat->as.seq.count; i++) gc_mark_pattern(pat->as.seq.items[i]);
            break;
        case ISH_PAT_VECTOR_REST:
        case ISH_PAT_TUPLE_REST:
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) gc_mark_pattern(pat->as.seq_rest.items[i]);
            gc_mark_pattern(pat->as.seq_rest.rest);
            break;
        case ISH_PAT_DICT:
            for (size_t i = 0; i < pat->as.dict.count; i++) { ish_gc_mark_value(pat->as.dict.entries[i].key); gc_mark_pattern(pat->as.dict.entries[i].pattern); }
            break;
        default: break;
    }
}

static void gc_mark_module(const IshBytecodeModule *module) {
    if (!module) return;
    for (size_t i = 0; i < module->const_count; i++) ish_gc_mark_value(module->constants[i]);
    for (size_t i = 0; i < module->function_count; i++) {
        const IshBcFunction *fn = &module->functions[i];
        for (uint32_t p = 0; p < fn->pattern_count; p++) gc_mark_pattern(fn->param_patterns[p]);
    }
}

static void sched_collect(IshScheduler *sched) {
    gc_mark_module(sched->module);
    for (size_t i = 0; i < sched->rt->gc_module_count; i++) gc_mark_module(sched->rt->gc_modules[i]);
    for (size_t i = 0; i < sched->rt->gc_value_count; i++) ish_gc_mark_value(sched->rt->gc_values[i]);
    for (size_t i = 0; i < sched->rt->ns_count; i++) {
        IshNamespace *ns = sched->rt->namespaces[i];
        for (size_t s = 0; s < ns->slot_count; s++) ish_gc_mark_value(ns->slots[s]);
    }
    ish_runtime_mark_protocol_roots(sched->rt);
    for (size_t i = 0; i < sched->actor_count; i++) {
        IshActor *a = sched->actors[i];
        ish_exec_visit_roots(a->exec, gc_mark_root, NULL);
        for (size_t m = 0; m < a->mb_count; m++) ish_gc_mark_value(a->mailbox[m]);
        if (a->exited) ish_gc_mark_value(a->exit_reason);
    }
    for (size_t i = 0; i < sched->port_count; i++) {
        if (sched->ports[i].has_result) ish_gc_mark_value(sched->ports[i].result);
    }
    ish_gc_mark_value(sched->main_value);
    ish_gc_mark_value(sched->main_reason);
    ish_heap_sweep(&sched->rt->heap);
}

static void sched_maybe_collect(IshScheduler *sched) {
    if (sched->rt->heap.bytes_allocated <= sched->gc_threshold) return;
    sched_collect(sched);
    size_t live = sched->rt->heap.bytes_allocated;
    size_t doubled = live * 2u;
    size_t floor = gc_threshold_from_env();
    sched->gc_threshold = doubled > floor ? doubled : floor;
}

IshScheduler *ish_sched_create(IshRuntime *rt, const IshBytecodeModule *module, IshError *err) {
    IshScheduler *sched = calloc(1u, sizeof(*sched));
    if (!sched) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return NULL;
    }
    sched->rt = rt;
    sched->module = module;
    sched->limits = ish_vm_default_limits();
    sched->next_ref = 1u;
    sched->next_port_id = 1u;
    sched->main_value = ish_nil();
    sched->main_reason = ish_nil();
    sched->crash_notes = NULL;
    sched->gc_threshold = gc_threshold_from_env();
    return sched;
}

static bool sched_register_port(IshScheduler *sched, IshPort *port, uint64_t *out_id, IshError *err) {
    if (sched->port_count == sched->port_cap) {
        size_t cap = sched->port_cap ? sched->port_cap * 2u : 8u;
        PortEntry *next = realloc(sched->ports, cap * sizeof(*next));
        if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
        sched->ports = next;
        sched->port_cap = cap;
    }
    PortEntry *entry = &sched->ports[sched->port_count++];
    entry->id = sched->next_port_id++;
    entry->port = port;
    entry->done = false;
    entry->has_result = false;
    entry->result = ish_nil();
    *out_id = entry->id;
    return true;
}

static PortEntry *sched_find_port(IshScheduler *sched, uint64_t id) {
    for (size_t i = 0; i < sched->port_count; i++) {
        if (sched->ports[i].id == id) return &sched->ports[i];
    }
    return NULL;
}

static void actor_release_resources(IshActor *actor) {
    if (actor->exec) { ish_exec_destroy(actor->exec); actor->exec = NULL; }
    free(actor->mailbox);
    actor->mailbox = NULL;
    actor->mb_count = 0;
    actor->mb_cap = 0;
    free(actor->links);
    actor->links = NULL;
    actor->link_count = 0;
    actor->link_cap = 0;
    free(actor->mon_out);
    actor->mon_out = NULL;
    actor->mon_out_count = 0;
    actor->mon_out_cap = 0;
    free(actor->mon_in);
    actor->mon_in = NULL;
    actor->mon_in_count = 0;
    actor->mon_in_cap = 0;
}

static void actor_free(IshActor *actor) {
    if (!actor) return;
    if (actor->port) ish_port_free(actor->port);
    actor_release_resources(actor);
    free(actor);
}

void ish_sched_destroy(IshScheduler *sched) {
    if (!sched) return;
    free(sched->crash_notes);
    for (size_t i = 0; i < sched->actor_count; i++) actor_free(sched->actors[i]);
    for (size_t i = 0; i < sched->port_count; i++) {
        if (sched->ports[i].port) ish_port_free(sched->ports[i].port);
    }
    free(sched->ports);
    free(sched->actors);
    free(sched->ready);
    free(sched);
}

static IshActor *sched_lookup(IshScheduler *sched, uint64_t pid) {
    if (pid == 0 || pid > sched->actor_count) return NULL;
    return sched->actors[pid - 1u];
}

static bool ready_push(IshScheduler *sched, uint64_t pid, IshError *err) {
    if (sched->ready_head + sched->ready_count == sched->ready_cap) {
        if (sched->ready_head > 0) {
            memmove(sched->ready, sched->ready + sched->ready_head, sched->ready_count * sizeof(*sched->ready));
            sched->ready_head = 0;
        } else {
            size_t cap = sched->ready_cap ? sched->ready_cap * 2u : 16u;
            uint64_t *next = realloc(sched->ready, cap * sizeof(*next));
            if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
            sched->ready = next;
            sched->ready_cap = cap;
        }
    }
    sched->ready[sched->ready_head + sched->ready_count] = pid;
    sched->ready_count++;
    return true;
}

static bool ready_pop(IshScheduler *sched, uint64_t *out) {
    if (sched->ready_count == 0) return false;
    *out = sched->ready[sched->ready_head];
    sched->ready_head++;
    sched->ready_count--;
    if (sched->ready_count == 0) sched->ready_head = 0;
    return true;
}

static IshActor *actor_create(IshScheduler *sched, IshError *err) {
    if (sched->actor_count == sched->actor_cap) {
        size_t cap = sched->actor_cap ? sched->actor_cap * 2u : 8u;
        IshActor **next = realloc(sched->actors, cap * sizeof(*next));
        if (!next) { ish_error_oom(err, ish_span_unknown(NULL)); return NULL; }
        sched->actors = next;
        sched->actor_cap = cap;
    }
    IshActor *actor = calloc(1u, sizeof(*actor));
    if (!actor) { ish_error_oom(err, ish_span_unknown(NULL)); return NULL; }
    actor->pid = (uint64_t)sched->actor_count + 1u;
    actor->status = ACT_READY;
    actor->exit_reason = ish_nil();
    actor->exec = ish_exec_create(sched->rt, sched->module, sched, actor, sched->limits, err);
    if (!actor->exec) { free(actor); return NULL; }
    sched->actors[sched->actor_count++] = actor;
    return actor;
}

static bool mailbox_push(IshActor *actor, IshValue msg, IshError *err) {
    if (actor->mb_count == actor->mb_cap) {
        size_t cap = actor->mb_cap ? actor->mb_cap * 2u : 8u;
        IshValue *next = realloc(actor->mailbox, cap * sizeof(*next));
        if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
        actor->mailbox = next;
        actor->mb_cap = cap;
    }
    actor->mailbox[actor->mb_count++] = msg;
    return true;
}

static void wake_for_message(IshScheduler *sched, IshActor *actor) {
    if (actor->status == ACT_WAITING_RECEIVE) {
        IshError ignore;
        ish_error_init(&ignore);
        if (ready_push(sched, actor->pid, &ignore)) {
            actor->status = ACT_READY;
        }
        ish_error_clear(&ignore);
    }
}

static void deliver(IshScheduler *sched, uint64_t target_pid, IshValue msg) {
    IshActor *actor = sched_lookup(sched, target_pid);
    if (!actor || actor->exited) return;
    IshError ignore;
    ish_error_init(&ignore);
    if (mailbox_push(actor, msg, &ignore)) wake_for_message(sched, actor);
    ish_error_clear(&ignore);
}

void ish_sched_send(IshScheduler *sched, uint64_t target_pid, IshValue msg) {
    deliver(sched, target_pid, msg);
}

static IshValue make_signal(IshScheduler *sched, const char *tag, IshValue a, IshValue b, IshValue c, size_t count) {
    IshValue items[4];
    items[0] = ish_atom(sched->rt, tag);
    items[1] = a;
    items[2] = b;
    items[3] = c;
    IshError ignore;
    ish_error_init(&ignore);
    IshValue tuple = ish_tuple(sched->rt, items, count, &ignore);
    ish_error_clear(&ignore);
    return tuple;
}

static void links_remove(IshActor *actor, uint64_t pid) {
    for (size_t i = 0; i < actor->link_count; i++) {
        if (actor->links[i] == pid) {
            actor->links[i] = actor->links[actor->link_count - 1u];
            actor->link_count--;
            return;
        }
    }
}

static bool links_add(IshActor *actor, uint64_t pid, IshError *err) {
    for (size_t i = 0; i < actor->link_count; i++) {
        if (actor->links[i] == pid) return true;
    }
    if (actor->link_count == actor->link_cap) {
        size_t cap = actor->link_cap ? actor->link_cap * 2u : 4u;
        uint64_t *next = realloc(actor->links, cap * sizeof(*next));
        if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
        actor->links = next;
        actor->link_cap = cap;
    }
    actor->links[actor->link_count++] = pid;
    return true;
}

static void terminate(IshScheduler *sched, IshActor *actor, IshValue reason);

static void apply_link_signal(IshScheduler *sched, IshActor *target, uint64_t from_pid, IshValue reason) {
    if (target->trap_exit) {
        IshValue signal = make_signal(sched, "exit", ish_pid(from_pid), reason, ish_nil(), 3);
        IshError ignore;
        ish_error_init(&ignore);
        if (mailbox_push(target, signal, &ignore)) wake_for_message(sched, target);
        ish_error_clear(&ignore);
        return;
    }
    if (reason_is_normal(reason)) return;
    terminate(sched, target, reason);
}

static void terminate(IshScheduler *sched, IshActor *actor, IshValue reason) {
    if (actor->exited) return;
    actor->exited = true;
    actor->status = ACT_EXITED;
    actor->exit_reason = reason;
    if (actor->port) { ish_port_free(actor->port); actor->port = NULL; }

    for (size_t i = 0; i < actor->mon_in_count; i++) {
        IshActor *watcher = sched_lookup(sched, actor->mon_in[i].watcher);
        if (!watcher || watcher->exited) continue;
        IshValue down = make_signal(sched, "down", ish_ref(actor->mon_in[i].ref), ish_pid(actor->pid), reason, 4);
        IshError ignore;
        ish_error_init(&ignore);
        if (mailbox_push(watcher, down, &ignore)) wake_for_message(sched, watcher);
        ish_error_clear(&ignore);
    }

    size_t link_count = actor->link_count;
    uint64_t *links = NULL;
    if (link_count > 0) {
        links = malloc(link_count * sizeof(*links));
        if (links) memcpy(links, actor->links, link_count * sizeof(*links));
    }
    for (size_t i = 0; links && i < link_count; i++) {
        IshActor *partner = sched_lookup(sched, links[i]);
        if (!partner || partner->exited) continue;
        links_remove(partner, actor->pid);
        apply_link_signal(sched, partner, actor->pid, reason);
    }
    free(links);

    if (actor->pid == sched->main_pid && !sched->main_terminated) {
        sched->main_terminated = true;
        sched->main_value = ish_nil();
        sched->main_reason = reason;
        sched->main_abnormal = !reason_is_normal(reason);
    }
    actor_release_resources(actor);
}

static IshValue crash_reason_from_err(IshScheduler *sched, IshError *err) {
    IshValue message;
    if (err->present && err->message) {
        message = ish_string(sched->rt, err->message, NULL);
    } else {
        message = ish_string(sched->rt, "actor crashed", NULL);
    }
    if (err->present && err->notes) {
        free(sched->crash_notes);
        sched->crash_notes = ish_strdup(err->notes);
    }
    ish_error_clear(err);
    IshValue items[2];
    items[0] = ish_atom(sched->rt, "error");
    items[1] = message;
    IshError ignore;
    ish_error_init(&ignore);
    IshValue reason = ish_tuple(sched->rt, items, 2u, &ignore);
    ish_error_clear(&ignore);
    return reason;
}

bool ish_sched_spawn(IshScheduler *sched, IshValue thunk, const IshExec *parent, IshValue *out_pid, IshError *err) {
    IshActor *actor = actor_create(sched, err);
    if (!actor) return false;
    if (!ish_exec_copy_context(actor->exec, parent)) return ish_error_oom(err, ish_span_unknown(NULL));
    if (!ish_exec_setup_thunk(actor->exec, thunk, err)) {
        IshValue reason = crash_reason_from_err(sched, err);
        terminate(sched, actor, reason);
        *out_pid = ish_pid(actor->pid);
        return true;
    }
    if (!ready_push(sched, actor->pid, err)) return false;
    actor->status = ACT_READY;
    *out_pid = ish_pid(actor->pid);
    return true;
}

bool ish_sched_link(IshScheduler *sched, IshActor *self, uint64_t target_pid, bool *self_should_exit, IshValue *self_exit_reason, IshError *err) {
    *self_should_exit = false;
    if (target_pid == self->pid) return true;
    IshActor *target = sched_lookup(sched, target_pid);
    if (!target || target->exited) {
        IshValue reason = (target && target->exited) ? target->exit_reason : ish_atom(sched->rt, "noproc");
        if (self->trap_exit) {
            IshValue signal = make_signal(sched, "exit", ish_pid(target_pid), reason, ish_nil(), 3);
            return mailbox_push(self, signal, err);
        }
        if (reason_is_normal(reason)) return true;
        *self_should_exit = true;
        *self_exit_reason = reason;
        return true;
    }
    if (!links_add(self, target_pid, err)) return false;
    if (!links_add(target, self->pid, err)) return false;
    return true;
}

bool ish_sched_unlink(IshScheduler *sched, IshActor *self, uint64_t target_pid) {
    links_remove(self, target_pid);
    IshActor *target = sched_lookup(sched, target_pid);
    if (target && !target->exited) links_remove(target, self->pid);
    return true;
}

static bool mon_out_add(IshActor *actor, uint64_t ref, uint64_t target, IshError *err) {
    if (actor->mon_out_count == actor->mon_out_cap) {
        size_t cap = actor->mon_out_cap ? actor->mon_out_cap * 2u : 4u;
        MonitorOut *next = realloc(actor->mon_out, cap * sizeof(*next));
        if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
        actor->mon_out = next;
        actor->mon_out_cap = cap;
    }
    actor->mon_out[actor->mon_out_count].ref = ref;
    actor->mon_out[actor->mon_out_count].target = target;
    actor->mon_out_count++;
    return true;
}

static bool mon_in_add(IshActor *actor, uint64_t ref, uint64_t watcher, IshError *err) {
    if (actor->mon_in_count == actor->mon_in_cap) {
        size_t cap = actor->mon_in_cap ? actor->mon_in_cap * 2u : 4u;
        MonitorIn *next = realloc(actor->mon_in, cap * sizeof(*next));
        if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
        actor->mon_in = next;
        actor->mon_in_cap = cap;
    }
    actor->mon_in[actor->mon_in_count].ref = ref;
    actor->mon_in[actor->mon_in_count].watcher = watcher;
    actor->mon_in_count++;
    return true;
}

bool ish_sched_monitor(IshScheduler *sched, IshActor *self, uint64_t target_pid, IshValue *out_ref, IshError *err) {
    uint64_t ref_id = sched->next_ref++;
    IshValue ref = ish_ref(ref_id);
    IshActor *target = sched_lookup(sched, target_pid);
    if (!target || target->exited) {
        IshValue reason = ish_atom(sched->rt, "noproc");
        IshValue down = make_signal(sched, "down", ref, ish_pid(target_pid), reason, 4);
        if (!mailbox_push(self, down, err)) return false;
        *out_ref = ref;
        return true;
    }
    if (!mon_out_add(self, ref_id, target_pid, err)) return false;
    if (!mon_in_add(target, ref_id, self->pid, err)) return false;
    *out_ref = ref;
    return true;
}

bool ish_sched_demonitor(IshScheduler *sched, IshActor *self, IshValue ref) {
    if (ref.tag != ISH_VAL_REF) return false;
    uint64_t ref_id = ref.as.id;
    uint64_t target_pid = 0;
    for (size_t i = 0; i < self->mon_out_count; i++) {
        if (self->mon_out[i].ref == ref_id) {
            target_pid = self->mon_out[i].target;
            self->mon_out[i] = self->mon_out[self->mon_out_count - 1u];
            self->mon_out_count--;
            break;
        }
    }
    if (target_pid != 0) {
        IshActor *target = sched_lookup(sched, target_pid);
        if (target) {
            for (size_t i = 0; i < target->mon_in_count; i++) {
                if (target->mon_in[i].ref == ref_id) {
                    target->mon_in[i] = target->mon_in[target->mon_in_count - 1u];
                    target->mon_in_count--;
                    break;
                }
            }
        }
    }
    return true;
}

uint64_t ish_actor_pid(const IshActor *actor) {
    return actor->pid;
}

bool ish_actor_trap_exit_get(const IshActor *actor) {
    return actor->trap_exit;
}

void ish_actor_trap_exit_set(IshActor *actor, bool on) {
    actor->trap_exit = on;
}

size_t ish_actor_mailbox_count(const IshActor *actor) {
    return actor->mb_count;
}

bool ish_actor_mailbox_peek(const IshActor *actor, size_t index, IshValue *out) {
    if (index >= actor->mb_count) return false;
    *out = actor->mailbox[index];
    return true;
}

bool ish_actor_mailbox_remove(IshActor *actor, size_t index, IshValue *out) {
    if (index >= actor->mb_count) return false;
    *out = actor->mailbox[index];
    memmove(actor->mailbox + index, actor->mailbox + index + 1u, (actor->mb_count - index - 1u) * sizeof(*actor->mailbox));
    actor->mb_count--;
    return true;
}

bool ish_actor_recv_no_match(IshActor *actor, IshValue timeout, IshRecvDecision *out, IshError *err) {
    if (!actor->recv_waiting) {
        actor->recv_waiting = true;
        if (timeout.tag == ISH_VAL_ATOM && strcmp(ish_symbol_text(timeout.as.symbol), "infinity") == 0) {
            actor->recv_has_deadline = false;
        } else if (timeout.tag == ISH_VAL_INT && timeout.as.i >= 0) {
            actor->recv_has_deadline = true;
            actor->recv_deadline_ms = now_ms() + (uint64_t)timeout.as.i;
        } else {
            actor->recv_waiting = false;
            return ish_error_set(err, ish_span_unknown(NULL), "receive timeout must be a non-negative integer or :infinity");
        }
    }
    if (actor->recv_has_deadline && now_ms() >= actor->recv_deadline_ms) {
        actor->recv_waiting = false;
        actor->recv_has_deadline = false;
        actor->recv_cursor = 0;
        *out = ISH_RECV_TIMEOUT;
        return true;
    }
    *out = ISH_RECV_BLOCK;
    return true;
}

size_t ish_actor_recv_cursor(const IshActor *actor) {
    return actor->recv_cursor;
}

void ish_actor_recv_set_cursor(IshActor *actor, size_t cursor) {
    actor->recv_cursor = cursor;
}

void ish_actor_recv_reset(IshActor *actor) {
    actor->recv_waiting = false;
    actor->recv_has_deadline = false;
    actor->recv_cursor = 0;
}

static bool sched_idle(IshScheduler *sched, IshError *err) {
    bool any_live = false;
    bool any_port = false;
    bool any_deadline = false;
    uint64_t nearest = 0;
    for (size_t i = 0; i < sched->actor_count; i++) {
        IshActor *a = sched->actors[i];
        if (a->exited) continue;
        any_live = true;
        if (a->status == ACT_WAITING_RECEIVE && a->recv_has_deadline) {
            if (!any_deadline || a->recv_deadline_ms < nearest) {
                nearest = a->recv_deadline_ms;
                any_deadline = true;
            }
        }
    }
    if (!any_live) return true;
    size_t fd_cap = 0;
    for (size_t i = 0; i < sched->port_count; i++) {
        if (!sched->ports[i].done && sched->ports[i].port) { any_port = true; fd_cap += 2u; }
    }
    if (!any_port && !any_deadline) {
        return ish_error_set(err, ish_span_unknown(NULL), "deadlock: all actors are blocked with no way to make progress");
    }
    struct pollfd *fds = fd_cap != 0 ? calloc(fd_cap, sizeof(*fds)) : NULL;
    if (fd_cap != 0 && !fds) return ish_error_oom(err, ish_span_unknown(NULL));
    nfds_t nfds = 0;
    for (size_t i = 0; i < sched->port_count; i++) {
        PortEntry *e = &sched->ports[i];
        if (e->done || !e->port) continue;
        int pf[2];
        size_t n = ish_port_live_fds(e->port, pf, 2u);
        for (size_t k = 0; k < n; k++) {
            fds[nfds].fd = pf[k];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
    }
    int timeout = -1;
    if (any_port) timeout = 20;
    if (any_deadline) {
        uint64_t now = now_ms();
        int delta = nearest > now ? (int)(nearest - now) : 0;
        if (timeout < 0 || delta < timeout) timeout = delta;
    }
    if (nfds > 0 || timeout >= 0) poll(nfds > 0 ? fds : NULL, nfds, timeout);
    free(fds);

    for (size_t i = 0; i < sched->port_count; i++) {
        PortEntry *e = &sched->ports[i];
        if (e->done || !e->port) continue;
        if (ish_port_try_complete(e->port)) {
            IshValue result = ish_port_result(e->port, sched->rt, err);
            if (err->present) return false;
            ish_port_free(e->port);
            e->port = NULL;
            e->result = result;
            e->has_result = true;
            e->done = true;
        }
    }
    for (size_t i = 0; i < sched->actor_count; i++) {
        IshActor *a = sched->actors[i];
        if (a->exited || a->status != ACT_WAITING_AWAIT) continue;
        PortEntry *e = sched_find_port(sched, a->await_port_id);
        if (e && e->done) {
            if (!ish_exec_push_result(a->exec, e->result, err)) return false;
            if (!ready_push(sched, a->pid, err)) return false;
            a->status = ACT_READY;
        }
    }

    uint64_t reached = now_ms();
    for (size_t i = 0; i < sched->actor_count; i++) {
        IshActor *a = sched->actors[i];
        if (a->exited || a->status != ACT_WAITING_RECEIVE) continue;
        if (a->recv_has_deadline && reached >= a->recv_deadline_ms) {
            if (!ready_push(sched, a->pid, err)) return false;
            a->status = ACT_READY;
        }
    }
    return true;
}

bool ish_sched_run_main(IshScheduler *sched, uint32_t main_fn, IshValue *out_result, IshError *err) {
    IshActor *main_actor = actor_create(sched, err);
    if (!main_actor) return false;
    sched->main_pid = main_actor->pid;
    if (!ish_exec_setup_function(main_actor->exec, main_fn, err)) return false;
    if (!ready_push(sched, main_actor->pid, err)) return false;

    while (!sched->main_terminated) {
        sched_maybe_collect(sched);
        uint64_t pid = 0;
        if (ready_pop(sched, &pid)) {
            IshActor *actor = sched_lookup(sched, pid);
            if (!actor || actor->exited || actor->status != ACT_READY) continue;
            actor->status = ACT_RUNNING;
            IshExecStatus status = ISH_EXEC_DONE;
            IshValue result = ish_nil();
            IshValue reason = ish_nil();
            if (!ish_exec_step(actor->exec, ISH_ACTOR_REDUCTIONS, &status, &result, &reason, err)) {
                IshValue crash_reason = crash_reason_from_err(sched, err);
                terminate(sched, actor, crash_reason);
                continue;
            }
            switch (status) {
                case ISH_EXEC_DONE:
                    if (actor->pid == sched->main_pid && !sched->main_terminated) {
                        sched->main_terminated = true;
                        sched->main_value = result;
                        sched->main_abnormal = false;
                    }
                    terminate(sched, actor, ish_atom(sched->rt, "normal"));
                    break;
                case ISH_EXEC_YIELD:
                    actor->status = ACT_READY;
                    if (!ready_push(sched, actor->pid, err)) return false;
                    break;
                case ISH_EXEC_BLOCK_RECEIVE:
                    actor->status = ACT_WAITING_RECEIVE;
                    break;
                case ISH_EXEC_LAUNCH_PORT: {
                    IshValue graph = ish_nil();
                    if (!ish_exec_take_port_request(actor->exec, &graph)) {
                        terminate(sched, actor, ish_atom(sched->rt, "noproc"));
                        break;
                    }
                    IshError perr;
                    ish_error_init(&perr);
                    IshPort *p = ish_port_launch(sched->rt, graph, actor->exec, &perr);
                    if (!p) {
                        IshValue crash = crash_reason_from_err(sched, &perr);
                        terminate(sched, actor, crash);
                        break;
                    }
                    uint64_t port_id = 0;
                    if (!sched_register_port(sched, p, &port_id, err)) { ish_port_free(p); return false; }
                    if (!ish_exec_push_result(actor->exec, ish_port(port_id), err)) return false;
                    actor->status = ACT_READY;
                    if (!ready_push(sched, actor->pid, err)) return false;
                    break;
                }
                case ISH_EXEC_BLOCK_AWAIT: {
                    IshValue port_val = ish_nil();
                    if (!ish_exec_take_await(actor->exec, &port_val)) {
                        terminate(sched, actor, ish_atom(sched->rt, "noproc"));
                        break;
                    }
                    PortEntry *entry = sched_find_port(sched, port_val.as.id);
                    if (!entry) {
                        terminate(sched, actor, ish_atom(sched->rt, "noproc"));
                        break;
                    }
                    if (entry->done) {
                        if (!ish_exec_push_result(actor->exec, entry->result, err)) return false;
                        actor->status = ACT_READY;
                        if (!ready_push(sched, actor->pid, err)) return false;
                    } else {
                        actor->await_port_id = port_val.as.id;
                        actor->status = ACT_WAITING_AWAIT;
                    }
                    break;
                }
                case ISH_EXEC_EXIT:
                    terminate(sched, actor, reason);
                    break;
            }
        } else {
            if (!sched_idle(sched, err)) return false;
            if (err->present) return false;
        }
    }

    if (sched->main_abnormal) {
        IshBuffer buf;
        ish_buf_init(&buf);
        if (ish_buf_append(&buf, "main actor exited with reason ") && ish_value_write(&buf, sched->main_reason)) {
            ish_error_set(err, ish_span_unknown(NULL), "%s", buf.data);
        } else {
            ish_error_set(err, ish_span_unknown(NULL), "main actor exited abnormally");
        }
        if (sched->crash_notes && err->notes == NULL) {
            err->notes = ish_strdup(sched->crash_notes);
        }
        ish_buf_destroy(&buf);
        return false;
    }
    *out_result = sched->main_value;
    return true;
}
