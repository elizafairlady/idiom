#include "idiom/actor.h"

#include "idiom/ports.h"

#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IDM_ACTOR_REDUCTIONS 4096

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
    IdmPort *port;
    bool done;
    bool has_result;
    IdmValue result;
} PortEntry;

typedef struct {
    uint64_t ref;
    uint64_t target;
} MonitorOut;

typedef struct {
    uint64_t ref;
    uint64_t watcher;
} MonitorIn;

struct IdmActor {
    uint64_t pid;
    IdmExec *exec;
    ActorStatus status;
    bool trap_exit;

    IdmValue *mailbox;
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

    IdmPort *port;
    uint64_t await_port_id;

    bool exited;
    IdmValue exit_reason;
};

struct IdmScheduler {
    IdmRuntime *rt;
    const IdmBytecodeModule *module;
    IdmVmLimits limits;

    IdmActor **actors;
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
    IdmValue main_value;
    IdmValue main_reason;
    char *crash_notes;
    IdmSpan crash_span;
    size_t gc_threshold;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool reason_is_normal(IdmValue reason) {
    return reason.tag == IDM_VAL_ATOM && strcmp(idm_symbol_text(reason.as.symbol), "normal") == 0;
}

static size_t gc_threshold_from_env(void) {
    const char *text = getenv("IDIOMGC");
    if (text && *text) {
        char *end = NULL;
        unsigned long long value = strtoull(text, &end, 10);
        if (end != text && value > 0) return (size_t)value;
    }
    return 1u << 20;
}

static void gc_mark_root(void *user, IdmValue value) {
    (void)user;
    idm_gc_mark_value(value);
}

static void gc_mark_pattern(const IdmPattern *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case IDM_PAT_LITERAL: idm_gc_mark_value(pat->as.literal); break;
        case IDM_PAT_PAIR: gc_mark_pattern(pat->as.pair.left); gc_mark_pattern(pat->as.pair.right); break;
        case IDM_PAT_LIST:
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE:
            for (size_t i = 0; i < pat->as.seq.count; i++) gc_mark_pattern(pat->as.seq.items[i]);
            break;
        case IDM_PAT_VECTOR_REST:
        case IDM_PAT_TUPLE_REST:
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) gc_mark_pattern(pat->as.seq_rest.items[i]);
            gc_mark_pattern(pat->as.seq_rest.rest);
            break;
        case IDM_PAT_DICT:
            for (size_t i = 0; i < pat->as.dict.count; i++) { idm_gc_mark_value(pat->as.dict.entries[i].key); gc_mark_pattern(pat->as.dict.entries[i].pattern); }
            break;
        default: break;
    }
}

static void gc_mark_module(const IdmBytecodeModule *module) {
    if (!module) return;
    for (size_t i = 0; i < module->const_count; i++) idm_gc_mark_value(module->constants[i]);
    for (size_t i = 0; i < module->function_count; i++) {
        const IdmBcFunction *fn = &module->functions[i];
        for (uint32_t p = 0; p < fn->pattern_count; p++) gc_mark_pattern(fn->param_patterns[p]);
    }
}

static void sched_collect(IdmScheduler *sched) {
    gc_mark_module(sched->module);
    for (size_t i = 0; i < sched->rt->gc_module_count; i++) gc_mark_module(sched->rt->gc_modules[i]);
    for (size_t i = 0; i < sched->rt->gc_value_count; i++) idm_gc_mark_value(sched->rt->gc_values[i]);
    for (size_t i = 0; i < sched->rt->ns_count; i++) {
        IdmNamespace *ns = sched->rt->namespaces[i];
        for (size_t s = 0; s < ns->slot_count; s++) idm_gc_mark_value(ns->slots[s]);
    }
    idm_runtime_mark_protocol_roots(sched->rt);
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        idm_exec_visit_roots(a->exec, gc_mark_root, NULL);
        for (size_t m = 0; m < a->mb_count; m++) idm_gc_mark_value(a->mailbox[m]);
        if (a->exited) idm_gc_mark_value(a->exit_reason);
    }
    for (size_t i = 0; i < sched->port_count; i++) {
        if (sched->ports[i].has_result) idm_gc_mark_value(sched->ports[i].result);
    }
    idm_gc_mark_value(sched->main_value);
    idm_gc_mark_value(sched->main_reason);
    idm_heap_sweep(&sched->rt->heap);
}

static void sched_maybe_collect(IdmScheduler *sched) {
    if (sched->rt->heap.bytes_allocated <= sched->gc_threshold) return;
    sched_collect(sched);
    size_t live = sched->rt->heap.bytes_allocated;
    size_t doubled = live * 2u;
    size_t floor = gc_threshold_from_env();
    sched->gc_threshold = doubled > floor ? doubled : floor;
}

IdmScheduler *idm_sched_create(IdmRuntime *rt, const IdmBytecodeModule *module, IdmError *err) {
    IdmScheduler *sched = calloc(1u, sizeof(*sched));
    if (!sched) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    sched->rt = rt;
    sched->module = module;
    sched->limits = idm_vm_default_limits();
    sched->next_ref = 1u;
    sched->next_port_id = 1u;
    sched->main_value = idm_nil();
    sched->main_reason = idm_nil();
    sched->crash_notes = NULL;
    sched->crash_span = idm_span_unknown(NULL);
    sched->gc_threshold = gc_threshold_from_env();
    return sched;
}

static bool sched_register_port(IdmScheduler *sched, IdmPort *port, uint64_t *out_id, IdmError *err) {
    if (sched->port_count == sched->port_cap) {
        size_t cap = sched->port_cap ? sched->port_cap * 2u : 8u;
        PortEntry *next = realloc(sched->ports, cap * sizeof(*next));
        if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
        sched->ports = next;
        sched->port_cap = cap;
    }
    PortEntry *entry = &sched->ports[sched->port_count++];
    entry->id = sched->next_port_id++;
    entry->port = port;
    entry->done = false;
    entry->has_result = false;
    entry->result = idm_nil();
    *out_id = entry->id;
    return true;
}

static PortEntry *sched_find_port(IdmScheduler *sched, uint64_t id) {
    for (size_t i = 0; i < sched->port_count; i++) {
        if (sched->ports[i].id == id) return &sched->ports[i];
    }
    return NULL;
}

static void actor_release_resources(IdmActor *actor) {
    if (actor->exec) { idm_exec_destroy(actor->exec); actor->exec = NULL; }
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

static void actor_free(IdmActor *actor) {
    if (!actor) return;
    if (actor->port) idm_port_free(actor->port);
    actor_release_resources(actor);
    free(actor);
}

void idm_sched_destroy(IdmScheduler *sched) {
    if (!sched) return;
    free(sched->crash_notes);
    for (size_t i = 0; i < sched->actor_count; i++) actor_free(sched->actors[i]);
    for (size_t i = 0; i < sched->port_count; i++) {
        if (sched->ports[i].port) idm_port_free(sched->ports[i].port);
    }
    free(sched->ports);
    free(sched->actors);
    free(sched->ready);
    free(sched);
}

static IdmActor *sched_lookup(IdmScheduler *sched, uint64_t pid) {
    if (pid == 0 || pid > sched->actor_count) return NULL;
    return sched->actors[pid - 1u];
}

static bool ready_push(IdmScheduler *sched, uint64_t pid, IdmError *err) {
    if (sched->ready_head + sched->ready_count == sched->ready_cap) {
        if (sched->ready_head > 0) {
            memmove(sched->ready, sched->ready + sched->ready_head, sched->ready_count * sizeof(*sched->ready));
            sched->ready_head = 0;
        } else {
            size_t cap = sched->ready_cap ? sched->ready_cap * 2u : 16u;
            uint64_t *next = realloc(sched->ready, cap * sizeof(*next));
            if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
            sched->ready = next;
            sched->ready_cap = cap;
        }
    }
    sched->ready[sched->ready_head + sched->ready_count] = pid;
    sched->ready_count++;
    return true;
}

static bool ready_pop(IdmScheduler *sched, uint64_t *out) {
    if (sched->ready_count == 0) return false;
    *out = sched->ready[sched->ready_head];
    sched->ready_head++;
    sched->ready_count--;
    if (sched->ready_count == 0) sched->ready_head = 0;
    return true;
}

static IdmActor *actor_create(IdmScheduler *sched, IdmError *err) {
    if (sched->actor_count == sched->actor_cap) {
        size_t cap = sched->actor_cap ? sched->actor_cap * 2u : 8u;
        IdmActor **next = realloc(sched->actors, cap * sizeof(*next));
        if (!next) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
        sched->actors = next;
        sched->actor_cap = cap;
    }
    IdmActor *actor = calloc(1u, sizeof(*actor));
    if (!actor) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    actor->pid = (uint64_t)sched->actor_count + 1u;
    actor->status = ACT_READY;
    actor->exit_reason = idm_nil();
    actor->exec = idm_exec_create(sched->rt, sched->module, sched, actor, sched->limits, err);
    if (!actor->exec) { free(actor); return NULL; }
    sched->actors[sched->actor_count++] = actor;
    return actor;
}

static bool mailbox_push(IdmActor *actor, IdmValue msg, IdmError *err) {
    if (actor->mb_count == actor->mb_cap) {
        size_t cap = actor->mb_cap ? actor->mb_cap * 2u : 8u;
        IdmValue *next = realloc(actor->mailbox, cap * sizeof(*next));
        if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
        actor->mailbox = next;
        actor->mb_cap = cap;
    }
    actor->mailbox[actor->mb_count++] = msg;
    return true;
}

static void wake_for_message(IdmScheduler *sched, IdmActor *actor) {
    if (actor->status == ACT_WAITING_RECEIVE) {
        IdmError ignore;
        idm_error_init(&ignore);
        if (ready_push(sched, actor->pid, &ignore)) {
            actor->status = ACT_READY;
        }
        idm_error_clear(&ignore);
    }
}

static void deliver(IdmScheduler *sched, uint64_t target_pid, IdmValue msg) {
    IdmActor *actor = sched_lookup(sched, target_pid);
    if (!actor || actor->exited) return;
    IdmError ignore;
    idm_error_init(&ignore);
    if (mailbox_push(actor, msg, &ignore)) wake_for_message(sched, actor);
    idm_error_clear(&ignore);
}

void idm_sched_send(IdmScheduler *sched, uint64_t target_pid, IdmValue msg) {
    deliver(sched, target_pid, msg);
}

static IdmValue make_signal(IdmScheduler *sched, const char *tag, IdmValue a, IdmValue b, IdmValue c, size_t count) {
    IdmValue items[4];
    items[0] = idm_atom(sched->rt, tag);
    items[1] = a;
    items[2] = b;
    items[3] = c;
    IdmError ignore;
    idm_error_init(&ignore);
    IdmValue tuple = idm_tuple(sched->rt, items, count, &ignore);
    idm_error_clear(&ignore);
    return tuple;
}

static void links_remove(IdmActor *actor, uint64_t pid) {
    for (size_t i = 0; i < actor->link_count; i++) {
        if (actor->links[i] == pid) {
            actor->links[i] = actor->links[actor->link_count - 1u];
            actor->link_count--;
            return;
        }
    }
}

static bool links_add(IdmActor *actor, uint64_t pid, IdmError *err) {
    for (size_t i = 0; i < actor->link_count; i++) {
        if (actor->links[i] == pid) return true;
    }
    if (actor->link_count == actor->link_cap) {
        size_t cap = actor->link_cap ? actor->link_cap * 2u : 4u;
        uint64_t *next = realloc(actor->links, cap * sizeof(*next));
        if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
        actor->links = next;
        actor->link_cap = cap;
    }
    actor->links[actor->link_count++] = pid;
    return true;
}

static void terminate(IdmScheduler *sched, IdmActor *actor, IdmValue reason);

static void apply_link_signal(IdmScheduler *sched, IdmActor *target, uint64_t from_pid, IdmValue reason) {
    if (target->trap_exit) {
        IdmValue signal = make_signal(sched, "exit", idm_pid(from_pid), reason, idm_nil(), 3);
        IdmError ignore;
        idm_error_init(&ignore);
        if (mailbox_push(target, signal, &ignore)) wake_for_message(sched, target);
        idm_error_clear(&ignore);
        return;
    }
    if (reason_is_normal(reason)) return;
    terminate(sched, target, reason);
}

static void terminate(IdmScheduler *sched, IdmActor *actor, IdmValue reason) {
    if (actor->exited) return;
    actor->exited = true;
    actor->status = ACT_EXITED;
    actor->exit_reason = reason;
    if (actor->port) { idm_port_free(actor->port); actor->port = NULL; }

    for (size_t i = 0; i < actor->mon_in_count; i++) {
        IdmActor *watcher = sched_lookup(sched, actor->mon_in[i].watcher);
        if (!watcher || watcher->exited) continue;
        IdmValue down = make_signal(sched, "down", idm_ref(actor->mon_in[i].ref), idm_pid(actor->pid), reason, 4);
        IdmError ignore;
        idm_error_init(&ignore);
        if (mailbox_push(watcher, down, &ignore)) wake_for_message(sched, watcher);
        idm_error_clear(&ignore);
    }

    size_t link_count = actor->link_count;
    uint64_t *links = NULL;
    if (link_count > 0) {
        links = malloc(link_count * sizeof(*links));
        if (links) memcpy(links, actor->links, link_count * sizeof(*links));
    }
    for (size_t i = 0; links && i < link_count; i++) {
        IdmActor *partner = sched_lookup(sched, links[i]);
        if (!partner || partner->exited) continue;
        links_remove(partner, actor->pid);
        apply_link_signal(sched, partner, actor->pid, reason);
    }
    free(links);

    if (actor->pid == sched->main_pid && !sched->main_terminated) {
        sched->main_terminated = true;
        sched->main_value = idm_nil();
        sched->main_reason = reason;
        sched->main_abnormal = !reason_is_normal(reason);
    }
    actor_release_resources(actor);
}

static IdmValue crash_reason_from_err(IdmScheduler *sched, IdmError *err) {
    IdmValue message;
    if (err->present && err->message) {
        message = idm_string(sched->rt, err->message, NULL);
    } else {
        message = idm_string(sched->rt, "actor crashed", NULL);
    }
    if (err->present && err->notes) {
        free(sched->crash_notes);
        sched->crash_notes = idm_strdup(err->notes);
    }
    if (err->present && err->span.line != 0 && sched->crash_span.line == 0) {
        sched->crash_span = err->span;
    }
    idm_error_clear(err);
    IdmValue items[2];
    items[0] = idm_atom(sched->rt, "error");
    items[1] = message;
    IdmError ignore;
    idm_error_init(&ignore);
    IdmValue reason = idm_tuple(sched->rt, items, 2u, &ignore);
    idm_error_clear(&ignore);
    return reason;
}

bool idm_sched_spawn(IdmScheduler *sched, IdmValue thunk, const IdmExec *parent, IdmValue *out_pid, IdmError *err) {
    IdmActor *actor = actor_create(sched, err);
    if (!actor) return false;
    if (!idm_exec_copy_context(actor->exec, parent)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!idm_exec_setup_thunk(actor->exec, thunk, err)) {
        IdmValue reason = crash_reason_from_err(sched, err);
        terminate(sched, actor, reason);
        *out_pid = idm_pid(actor->pid);
        return true;
    }
    if (!ready_push(sched, actor->pid, err)) return false;
    actor->status = ACT_READY;
    *out_pid = idm_pid(actor->pid);
    return true;
}

bool idm_sched_link(IdmScheduler *sched, IdmActor *self, uint64_t target_pid, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err) {
    *self_should_exit = false;
    if (target_pid == self->pid) return true;
    IdmActor *target = sched_lookup(sched, target_pid);
    if (!target || target->exited) {
        IdmValue reason = (target && target->exited) ? target->exit_reason : idm_atom(sched->rt, "noproc");
        if (self->trap_exit) {
            IdmValue signal = make_signal(sched, "exit", idm_pid(target_pid), reason, idm_nil(), 3);
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

bool idm_sched_unlink(IdmScheduler *sched, IdmActor *self, uint64_t target_pid) {
    links_remove(self, target_pid);
    IdmActor *target = sched_lookup(sched, target_pid);
    if (target && !target->exited) links_remove(target, self->pid);
    return true;
}

static bool mon_out_add(IdmActor *actor, uint64_t ref, uint64_t target, IdmError *err) {
    if (actor->mon_out_count == actor->mon_out_cap) {
        size_t cap = actor->mon_out_cap ? actor->mon_out_cap * 2u : 4u;
        MonitorOut *next = realloc(actor->mon_out, cap * sizeof(*next));
        if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
        actor->mon_out = next;
        actor->mon_out_cap = cap;
    }
    actor->mon_out[actor->mon_out_count].ref = ref;
    actor->mon_out[actor->mon_out_count].target = target;
    actor->mon_out_count++;
    return true;
}

static bool mon_in_add(IdmActor *actor, uint64_t ref, uint64_t watcher, IdmError *err) {
    if (actor->mon_in_count == actor->mon_in_cap) {
        size_t cap = actor->mon_in_cap ? actor->mon_in_cap * 2u : 4u;
        MonitorIn *next = realloc(actor->mon_in, cap * sizeof(*next));
        if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
        actor->mon_in = next;
        actor->mon_in_cap = cap;
    }
    actor->mon_in[actor->mon_in_count].ref = ref;
    actor->mon_in[actor->mon_in_count].watcher = watcher;
    actor->mon_in_count++;
    return true;
}

bool idm_sched_monitor(IdmScheduler *sched, IdmActor *self, uint64_t target_pid, IdmValue *out_ref, IdmError *err) {
    uint64_t ref_id = sched->next_ref++;
    IdmValue ref = idm_ref(ref_id);
    IdmActor *target = sched_lookup(sched, target_pid);
    if (!target || target->exited) {
        IdmValue reason = idm_atom(sched->rt, "noproc");
        IdmValue down = make_signal(sched, "down", ref, idm_pid(target_pid), reason, 4);
        if (!mailbox_push(self, down, err)) return false;
        *out_ref = ref;
        return true;
    }
    if (!mon_out_add(self, ref_id, target_pid, err)) return false;
    if (!mon_in_add(target, ref_id, self->pid, err)) return false;
    *out_ref = ref;
    return true;
}

bool idm_sched_demonitor(IdmScheduler *sched, IdmActor *self, IdmValue ref) {
    if (ref.tag != IDM_VAL_REF) return false;
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
        IdmActor *target = sched_lookup(sched, target_pid);
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

uint64_t idm_actor_pid(const IdmActor *actor) {
    return actor->pid;
}

bool idm_actor_trap_exit_get(const IdmActor *actor) {
    return actor->trap_exit;
}

void idm_actor_trap_exit_set(IdmActor *actor, bool on) {
    actor->trap_exit = on;
}

size_t idm_actor_mailbox_count(const IdmActor *actor) {
    return actor->mb_count;
}

bool idm_actor_mailbox_peek(const IdmActor *actor, size_t index, IdmValue *out) {
    if (index >= actor->mb_count) return false;
    *out = actor->mailbox[index];
    return true;
}

bool idm_actor_mailbox_remove(IdmActor *actor, size_t index, IdmValue *out) {
    if (index >= actor->mb_count) return false;
    *out = actor->mailbox[index];
    memmove(actor->mailbox + index, actor->mailbox + index + 1u, (actor->mb_count - index - 1u) * sizeof(*actor->mailbox));
    actor->mb_count--;
    return true;
}

bool idm_actor_recv_no_match(IdmActor *actor, IdmValue timeout, IdmRecvDecision *out, IdmError *err) {
    if (!actor->recv_waiting) {
        actor->recv_waiting = true;
        if (timeout.tag == IDM_VAL_ATOM && strcmp(idm_symbol_text(timeout.as.symbol), "infinity") == 0) {
            actor->recv_has_deadline = false;
        } else if (timeout.tag == IDM_VAL_INT && timeout.as.i >= 0) {
            actor->recv_has_deadline = true;
            actor->recv_deadline_ms = now_ms() + (uint64_t)timeout.as.i;
        } else {
            actor->recv_waiting = false;
            return idm_error_set(err, idm_span_unknown(NULL), "receive timeout must be a non-negative integer or :infinity");
        }
    }
    if (actor->recv_has_deadline && now_ms() >= actor->recv_deadline_ms) {
        actor->recv_waiting = false;
        actor->recv_has_deadline = false;
        actor->recv_cursor = 0;
        *out = IDM_RECV_TIMEOUT;
        return true;
    }
    *out = IDM_RECV_BLOCK;
    return true;
}

size_t idm_actor_recv_cursor(const IdmActor *actor) {
    return actor->recv_cursor;
}

void idm_actor_recv_set_cursor(IdmActor *actor, size_t cursor) {
    actor->recv_cursor = cursor;
}

void idm_actor_recv_reset(IdmActor *actor) {
    actor->recv_waiting = false;
    actor->recv_has_deadline = false;
    actor->recv_cursor = 0;
}

static bool sched_idle(IdmScheduler *sched, IdmError *err) {
    bool any_live = false;
    bool any_port = false;
    bool any_deadline = false;
    uint64_t nearest = 0;
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
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
        return idm_error_set(err, idm_span_unknown(NULL), "deadlock: all actors are blocked with no way to make progress");
    }
    struct pollfd *fds = fd_cap != 0 ? calloc(fd_cap, sizeof(*fds)) : NULL;
    if (fd_cap != 0 && !fds) return idm_error_oom(err, idm_span_unknown(NULL));
    nfds_t nfds = 0;
    for (size_t i = 0; i < sched->port_count; i++) {
        PortEntry *e = &sched->ports[i];
        if (e->done || !e->port) continue;
        int pf[2];
        size_t n = idm_port_live_fds(e->port, pf, 2u);
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
        if (idm_port_try_complete(e->port)) {
            IdmValue result = idm_port_result(e->port, sched->rt, err);
            if (err->present) return false;
            idm_port_free(e->port);
            e->port = NULL;
            e->result = result;
            e->has_result = true;
            e->done = true;
        }
    }
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (a->exited || a->status != ACT_WAITING_AWAIT) continue;
        PortEntry *e = sched_find_port(sched, a->await_port_id);
        if (e && e->done) {
            if (!idm_exec_push_result(a->exec, e->result, err)) return false;
            if (!ready_push(sched, a->pid, err)) return false;
            a->status = ACT_READY;
        }
    }

    uint64_t reached = now_ms();
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (a->exited || a->status != ACT_WAITING_RECEIVE) continue;
        if (a->recv_has_deadline && reached >= a->recv_deadline_ms) {
            if (!ready_push(sched, a->pid, err)) return false;
            a->status = ACT_READY;
        }
    }
    return true;
}

bool idm_sched_run_main(IdmScheduler *sched, uint32_t main_fn, IdmValue *out_result, IdmError *err) {
    IdmActor *main_actor = actor_create(sched, err);
    if (!main_actor) return false;
    sched->main_pid = main_actor->pid;
    if (!idm_exec_setup_function(main_actor->exec, main_fn, err)) return false;
    if (!ready_push(sched, main_actor->pid, err)) return false;

    while (!sched->main_terminated) {
        sched_maybe_collect(sched);
        uint64_t pid = 0;
        if (ready_pop(sched, &pid)) {
            IdmActor *actor = sched_lookup(sched, pid);
            if (!actor || actor->exited || actor->status != ACT_READY) continue;
            actor->status = ACT_RUNNING;
            IdmExecStatus status = IDM_EXEC_DONE;
            IdmValue result = idm_nil();
            IdmValue reason = idm_nil();
            if (!idm_exec_step(actor->exec, IDM_ACTOR_REDUCTIONS, &status, &result, &reason, err)) {
                IdmValue crash_reason = crash_reason_from_err(sched, err);
                terminate(sched, actor, crash_reason);
                continue;
            }
            switch (status) {
                case IDM_EXEC_DONE:
                    if (actor->pid == sched->main_pid && !sched->main_terminated) {
                        sched->main_terminated = true;
                        sched->main_value = result;
                        sched->main_abnormal = false;
                    }
                    terminate(sched, actor, idm_atom(sched->rt, "normal"));
                    break;
                case IDM_EXEC_YIELD:
                    actor->status = ACT_READY;
                    if (!ready_push(sched, actor->pid, err)) return false;
                    break;
                case IDM_EXEC_BLOCK_RECEIVE:
                    actor->status = ACT_WAITING_RECEIVE;
                    break;
                case IDM_EXEC_LAUNCH_PORT: {
                    IdmValue graph = idm_nil();
                    if (!idm_exec_take_port_request(actor->exec, &graph)) {
                        terminate(sched, actor, idm_atom(sched->rt, "noproc"));
                        break;
                    }
                    IdmError perr;
                    idm_error_init(&perr);
                    IdmPort *p = idm_port_launch(sched->rt, graph, actor->exec, &perr);
                    if (!p) {
                        IdmValue crash = crash_reason_from_err(sched, &perr);
                        terminate(sched, actor, crash);
                        break;
                    }
                    uint64_t port_id = 0;
                    if (!sched_register_port(sched, p, &port_id, err)) { idm_port_free(p); return false; }
                    if (!idm_exec_push_result(actor->exec, idm_port(port_id), err)) return false;
                    actor->status = ACT_READY;
                    if (!ready_push(sched, actor->pid, err)) return false;
                    break;
                }
                case IDM_EXEC_BLOCK_AWAIT: {
                    IdmValue port_val = idm_nil();
                    if (!idm_exec_take_await(actor->exec, &port_val)) {
                        terminate(sched, actor, idm_atom(sched->rt, "noproc"));
                        break;
                    }
                    PortEntry *entry = sched_find_port(sched, port_val.as.id);
                    if (!entry) {
                        terminate(sched, actor, idm_atom(sched->rt, "noproc"));
                        break;
                    }
                    if (entry->done) {
                        if (!idm_exec_push_result(actor->exec, entry->result, err)) return false;
                        actor->status = ACT_READY;
                        if (!ready_push(sched, actor->pid, err)) return false;
                    } else {
                        actor->await_port_id = port_val.as.id;
                        actor->status = ACT_WAITING_AWAIT;
                    }
                    break;
                }
                case IDM_EXEC_EXIT:
                    terminate(sched, actor, reason);
                    break;
            }
        } else {
            if (!sched_idle(sched, err)) return false;
            if (err->present) return false;
        }
    }

    if (sched->main_abnormal) {
        IdmBuffer buf;
        idm_buf_init(&buf);
        if (idm_buf_append(&buf, "main actor exited with reason ") && idm_value_write(&buf, sched->main_reason)) {
            idm_error_set(err, sched->crash_span, "%s", buf.data);
        } else {
            idm_error_set(err, idm_span_unknown(NULL), "main actor exited abnormally");
        }
        if (sched->crash_notes && err->notes == NULL) {
            err->notes = idm_strdup(sched->crash_notes);
        }
        idm_buf_destroy(&buf);
        return false;
    }
    *out_result = sched->main_value;
    return true;
}
