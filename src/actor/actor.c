#include "idiom/actor.h"

#include "idiom/expand.h"
#include "idiom/ports.h"
#include "idiom/tty.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>
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
    ACT_WAITING_PORT_READ,
    ACT_WAITING_PORT_WRITE,
    ACT_WAITING_TTY,
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
    IdmRuntime *rt;
    IdmExec *exec;
    ActorStatus status;
    _Atomic bool trap_exit;

    IdmHeap heap;
    size_t gc_threshold;

    pthread_mutex_t mbox_mu;
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
    const void *recv_ctx_module;
    size_t recv_ctx_site;

    uint64_t await_port_id;
    uint64_t port_io_port_id;
    const char *port_io_stream;
    size_t port_io_max;
    IdmValue port_io_data;

    bool tty_line;
    bool tty_has_deadline;
    uint64_t tty_deadline_ms;
    char *tty_buf;
    size_t tty_buf_len;
    size_t tty_buf_cap;

    bool diag_retain;
    bool exited;
    bool reap_queued;
    IdmValue exit_reason;
};

struct IdmScheduler {
    IdmRuntime *rt;
    const IdmBytecodeModule *module;
    IdmVmLimits limits;

    IdmActor **actors;
    size_t actor_count;
    size_t actor_cap;
    uint32_t *slot_gen;
    uint32_t *free_slots;
    size_t free_count;
    size_t free_cap;
    uint32_t *reap_slots;
    size_t reap_count;
    size_t reap_cap;

    uint64_t *ready;
    size_t ready_head;
    size_t ready_count;
    size_t ready_cap;

    uint64_t next_ref;
    uint64_t main_pid;

    PortEntry *ports;
    size_t port_count;
    size_t port_cap;
    size_t ports_pending;
    uint64_t next_port_id;
    uint64_t deadline_hint;

    bool main_terminated;
    bool main_abnormal;
    IdmValue main_value;
    IdmValue main_reason;
    char *crash_notes;
    IdmSpan crash_span;
    size_t gc_threshold;

    size_t tty_waiting;
    uint64_t eval_pid;
    bool eval_done;
    IdmValue eval_value;
    IdmValue eval_reason;
    uint64_t interrupt_pid;
    bool interrupt_struck;
    char *retained_diag;

    size_t nworkers;
    int wake_pipe[2];
    pthread_mutex_t mu;
    pthread_cond_t work_cv;
    size_t parked;
    bool poller_active;
    bool shutdown;
    bool fatal;
    IdmError fatal_err;
};

static void sched_lock(IdmScheduler *sched) { pthread_mutex_lock(&sched->mu); }
static void sched_unlock(IdmScheduler *sched) { pthread_mutex_unlock(&sched->mu); }

static void sched_wake(IdmScheduler *sched, bool all) {
    if (sched->nworkers <= 1u) return;
    if (all) pthread_cond_broadcast(&sched->work_cv);
    else pthread_cond_signal(&sched->work_cv);
    if (sched->poller_active && sched->wake_pipe[1] >= 0) {
        ssize_t ignored = write(sched->wake_pipe[1], "w", 1u);
        (void)ignored;
    }
}

static int g_sig_pipe[2] = {-1, -1};

static void sig_handler(int signo) {
    char tag = signo == SIGWINCH ? 'r' : 'i';
    ssize_t ignored = write(g_sig_pipe[1], &tag, 1u);
    (void)ignored;
}

bool idm_signals_install(IdmError *err) {
    if (g_sig_pipe[0] >= 0) return true;
    int fds[2];
    if (pipe(fds) != 0) return idm_error_set(err, idm_span_unknown(NULL), "cannot create the signal pipe");
    for (int i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFL, O_NONBLOCK);
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    g_sig_pipe[0] = fds[0];
    g_sig_pipe[1] = fds[1];
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);
    return true;
}

static void sig_drain_pending(void) {
    char drain[64];
    while (g_sig_pipe[0] >= 0 && read(g_sig_pipe[0], drain, sizeof(drain)) > 0) {}
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool reason_is_atom(IdmValue reason, const char *name) {
    return reason.tag == IDM_VAL_ATOM && strcmp(idm_symbol_text(reason.as.symbol), name) == 0;
}

static bool reason_is_normal(IdmValue reason) {
    return reason_is_atom(reason, "normal");
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
    idm_gc_mark_value(user, value);
}

static void compile_collect(IdmScheduler *sched) {
    IdmHeap *heap = &sched->rt->heap;
    pthread_mutex_lock(&heap->lock);
    for (size_t i = 0; i < sched->port_count; i++) {
        if (sched->ports[i].has_result) idm_gc_mark_value(heap, sched->ports[i].result);
    }
    idm_heap_sweep(heap);
    pthread_mutex_unlock(&heap->lock);
}

static void actor_collect(IdmActor *actor) {
    if (!actor->exec) return;
    IdmHeap *h = &actor->heap;
    pthread_mutex_lock(&h->lock);
    idm_exec_visit_roots(actor->exec, gc_mark_root, h);
    for (size_t m = 0; m < actor->mb_count; m++) idm_gc_mark_value(h, actor->mailbox[m]);
    if (actor->status == ACT_WAITING_PORT_WRITE) idm_gc_mark_value(h, actor->port_io_data);
    if (actor->exited) idm_gc_mark_value(h, actor->exit_reason);
    idm_heap_sweep(h);
    pthread_mutex_unlock(&h->lock);
}

static void actor_maybe_collect(IdmActor *actor) {
    if (idm_heap_bytes(&actor->heap) <= actor->gc_threshold) return;
    actor_collect(actor);
    size_t live = idm_heap_bytes(&actor->heap);
    size_t doubled = live * 2u;
    size_t floor = gc_threshold_from_env();
    actor->gc_threshold = doubled > floor ? doubled : floor;
}

static void sched_maybe_collect(IdmScheduler *sched) {
    if (idm_heap_bytes(&sched->rt->heap) <= sched->gc_threshold) return;
    compile_collect(sched);
    size_t live = idm_heap_bytes(&sched->rt->heap);
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
    if (module && !idm_bc_is_finalized(module)) {
        idm_error_set(err, idm_span_unknown(NULL), "scheduler module is not finalized");
        free(sched);
        return NULL;
    }
    sched->limits = idm_vm_default_limits();
    sched->next_ref = 1u;
    sched->next_port_id = 1u;
    sched->deadline_hint = UINT64_MAX;
    sched->main_value = idm_nil();
    sched->main_reason = idm_nil();
    sched->eval_value = idm_nil();
    sched->eval_reason = idm_nil();
    sched->crash_notes = NULL;
    sched->crash_span = idm_span_unknown(NULL);
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&sched->mu, &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_cond_init(&sched->work_cv, NULL);
    sched->wake_pipe[0] = -1;
    sched->wake_pipe[1] = -1;
    if (pipe(sched->wake_pipe) == 0) {
        fcntl(sched->wake_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(sched->wake_pipe[1], F_SETFL, O_NONBLOCK);
    }
    idm_error_init(&sched->fatal_err);
    sched->nworkers = 1u;
    const char *procs = getenv("IDIOMMAXPROCS");
    long n = procs && procs[0] ? strtol(procs, NULL, 10) : 1;
    if (n > 1 && sched->wake_pipe[0] >= 0 && sched->wake_pipe[1] >= 0) sched->nworkers = (size_t)(n > 64 ? 64 : n);
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
    sched->ports_pending++;
    entry->id = sched->next_port_id++;
    entry->port = port;
    entry->done = false;
    entry->has_result = false;
    entry->result = idm_nil();
    *out_id = entry->id;
    return true;
}

bool idm_sched_register_port(IdmScheduler *sched, IdmPort *port, IdmValue *out_port, IdmError *err) {
    sched_lock(sched);
    uint64_t port_id = 0;
    bool ok = sched_register_port(sched, port, &port_id, err);
    if (ok) *out_port = idm_port(port_id);
    sched_unlock(sched);
    return ok;
}

static PortEntry *sched_find_port(IdmScheduler *sched, uint64_t id) {
    for (size_t i = 0; i < sched->port_count; i++) {
        if (sched->ports[i].id == id) return &sched->ports[i];
    }
    return NULL;
}

static bool port_entry_ensure_result(IdmScheduler *sched, PortEntry *entry, IdmError *err) {
    if (entry->has_result) return true;
    if (!entry->port) return idm_error_set(err, idm_span_unknown(NULL), "port result requested after port state was released");
    IdmValue result = idm_port_result(entry->port, sched->rt, err);
    if (err->present) return false;
    idm_port_free(entry->port);
    entry->port = NULL;
    entry->result = result;
    entry->has_result = true;
    return true;
}

static void actor_release_resources(IdmActor *actor) {
    if (actor->exec) { idm_exec_destroy(actor->exec); actor->exec = NULL; }
    free(actor->tty_buf);
    actor->tty_buf = NULL;
    actor->tty_buf_len = 0;
    actor->tty_buf_cap = 0;
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
    pthread_mutex_destroy(&actor->mbox_mu);
    actor_release_resources(actor);
    idm_heap_destroy(&actor->heap);
    free(actor);
}

void idm_sched_destroy(IdmScheduler *sched) {
    if (!sched) return;
    if (sched->wake_pipe[0] >= 0) close(sched->wake_pipe[0]);
    if (sched->wake_pipe[1] >= 0) close(sched->wake_pipe[1]);
    pthread_mutex_destroy(&sched->mu);
    pthread_cond_destroy(&sched->work_cv);
    idm_error_clear(&sched->fatal_err);
    free(sched->crash_notes);
    free(sched->retained_diag);
    for (size_t i = 0; i < sched->actor_count; i++) actor_free(sched->actors[i]);
    for (size_t i = 0; i < sched->port_count; i++) {
        if (sched->ports[i].port) idm_port_free(sched->ports[i].port);
    }
    free(sched->ports);
    free(sched->actors);
    free(sched->slot_gen);
    free(sched->free_slots);
    free(sched->reap_slots);
    free(sched->ready);
    free(sched);
}

static IdmActor *sched_lookup(IdmScheduler *sched, uint64_t pid) {
    uint32_t low = (uint32_t)(pid & 0xFFFFFFFFu);
    if (low == 0) return NULL;
    size_t slot = (size_t)low - 1u;
    if (slot >= sched->actor_count || sched->slot_gen[slot] != (uint32_t)(pid >> 32)) return NULL;
    return sched->actors[slot];
}

static bool reapable(IdmScheduler *sched, IdmActor *actor) {
    return actor->exited && !actor->reap_queued
        && actor->pid != sched->main_pid
        && actor->pid != sched->eval_pid
        && actor->pid != sched->interrupt_pid;
}

static void reap_queue_push(IdmScheduler *sched, IdmActor *actor) {
    if (actor->reap_queued) return;
    if (sched->reap_count == sched->reap_cap) {
        size_t cap = sched->reap_cap ? sched->reap_cap * 2u : 16u;
        uint32_t *next = realloc(sched->reap_slots, cap * sizeof(*next));
        if (!next) return;
        sched->reap_slots = next;
        sched->reap_cap = cap;
    }
    sched->reap_slots[sched->reap_count++] = (uint32_t)((actor->pid & 0xFFFFFFFFu) - 1u);
    actor->reap_queued = true;
}

static void drain_reap(IdmScheduler *sched) {
    for (size_t i = 0; i < sched->reap_count; i++) {
        uint32_t slot = sched->reap_slots[i];
        IdmActor *a = sched->actors[slot];
        if (!a) continue;
        actor_free(a);
        sched->actors[slot] = NULL;
        sched->slot_gen[slot]++;
        if (sched->free_count == sched->free_cap) {
            size_t cap = sched->free_cap ? sched->free_cap * 2u : 16u;
            uint32_t *next = realloc(sched->free_slots, cap * sizeof(*next));
            if (!next) continue;
            sched->free_slots = next;
            sched->free_cap = cap;
        }
        sched->free_slots[sched->free_count++] = slot;
    }
    sched->reap_count = 0;
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
    sched_wake(sched, false);
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
    IdmActor *actor = calloc(1u, sizeof(*actor));
    if (!actor) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    idm_heap_init(&actor->heap);
    actor->rt = sched->rt;
    actor->gc_threshold = gc_threshold_from_env();
    pthread_mutex_init(&actor->mbox_mu, NULL);
    size_t slot;
    if (sched->free_count > 0) {
        slot = sched->free_slots[--sched->free_count];
    } else {
        if (sched->actor_count == sched->actor_cap) {
            size_t cap = sched->actor_cap ? sched->actor_cap * 2u : 8u;
            IdmActor **na = realloc(sched->actors, cap * sizeof(*na));
            if (na) sched->actors = na;
            uint32_t *ng = na ? realloc(sched->slot_gen, cap * sizeof(*ng)) : NULL;
            if (!na || !ng) {
                pthread_mutex_destroy(&actor->mbox_mu);
                idm_heap_destroy(&actor->heap);
                free(actor);
                return (IdmActor *)(uintptr_t)idm_error_oom(err, idm_span_unknown(NULL));
            }
            sched->slot_gen = ng;
            sched->actor_cap = cap;
        }
        slot = sched->actor_count++;
        sched->slot_gen[slot] = 0;
    }
    actor->pid = ((uint64_t)sched->slot_gen[slot] << 32) | (uint64_t)(slot + 1u);
    actor->status = ACT_READY;
    actor->exit_reason = idm_nil();
    actor->exec = idm_exec_create(sched->rt, sched->module, sched, actor, sched->limits, err);
    if (!actor->exec) {
        pthread_mutex_destroy(&actor->mbox_mu);
        idm_heap_destroy(&actor->heap);
        free(actor);
        return NULL;
    }
    sched->actors[slot] = actor;
    return actor;
}

static bool mailbox_push_unlocked(IdmActor *actor, IdmValue msg, IdmError *err);

static bool mailbox_push(IdmActor *actor, IdmValue msg, IdmError *err) {
    pthread_mutex_lock(&actor->heap.lock);
    IdmValue copied = idm_value_copy_locked(actor->rt, &actor->heap, msg, err);
    if (err->present) { pthread_mutex_unlock(&actor->heap.lock); return false; }
    pthread_mutex_lock(&actor->mbox_mu);
    bool ok = mailbox_push_unlocked(actor, copied, err);
    pthread_mutex_unlock(&actor->mbox_mu);
    pthread_mutex_unlock(&actor->heap.lock);
    return ok;
}

static bool mailbox_push_unlocked(IdmActor *actor, IdmValue msg, IdmError *err) {
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
    sched_lock(sched);
    deliver(sched, target_pid, msg);
    sched_unlock(sched);
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
static bool sched_spawn_locked(IdmScheduler *sched, IdmValue thunk, const IdmExec *parent, IdmValue *out_pid, IdmError *err);

static bool sched_abnormal_error(IdmScheduler *sched, IdmValue reason, IdmError *err) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    idm_buf_append(&buf, "main actor exited with reason ");
    idm_error_describe(sched->rt, reason, &buf);
    if (buf.data) {
        idm_error_set(err, sched->crash_span, "%s", buf.data);
    } else {
        idm_error_set(err, idm_span_unknown(NULL), "main actor exited abnormally");
    }
    if (sched->crash_notes && err->notes == NULL) {
        err->notes = idm_strdup(sched->crash_notes);
    }
    if (!err->reason) err->reason = malloc(sizeof(IdmValue));
    if (err->reason) *(IdmValue *)err->reason = reason;
    idm_buf_destroy(&buf);
    return false;
}

static bool sched_crash_stall_error(IdmScheduler *sched, IdmError *err) {
    IdmValue first_reason = idm_nil();
    size_t crashed = 0;
    IdmBuffer buf;
    idm_buf_init(&buf);
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (!a) continue;
        if (!a->exited || reason_is_normal(a->exit_reason)) continue;
        if (crashed == 0) first_reason = a->exit_reason;
        if (crashed != 0) idm_buf_append(&buf, "; ");
        idm_buf_appendf(&buf, "process %llu crashed (", (unsigned long long)a->pid);
        idm_error_describe(sched->rt, a->exit_reason, &buf);
        idm_buf_append(&buf, ")");
        crashed++;
    }
    if (crashed == 0) {
        idm_buf_destroy(&buf);
        return idm_error_set(err, idm_span_unknown(NULL), "deadlock: all live actors are blocked with no way to make progress");
    }
    idm_error_set(err, idm_span_unknown(NULL), "%s; the remaining actor(s) are blocked with no way to make progress",
                  buf.data ? buf.data : "a peer crashed");
    idm_buf_destroy(&buf);
    if (!err->reason) err->reason = malloc(sizeof(IdmValue));
    if (err->reason) *(IdmValue *)err->reason = first_reason;
    return false;
}

static void retain_diag(IdmScheduler *sched, IdmValue reason) {
    IdmError tmp;
    idm_error_init(&tmp);
    sched_abnormal_error(sched, reason, &tmp);
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (idm_error_render(&tmp, &buf)) {
        free(sched->retained_diag);
        sched->retained_diag = idm_buf_take(&buf);
    } else {
        idm_buf_destroy(&buf);
    }
    idm_error_clear(&tmp);
}

static void apply_exit_signal(IdmScheduler *sched, IdmActor *target, uint64_t from_pid, IdmValue reason) {
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

static void mon_out_remove(IdmActor *actor, uint64_t ref) {
    for (size_t i = 0; i < actor->mon_out_count; i++) {
        if (actor->mon_out[i].ref == ref) {
            actor->mon_out[i] = actor->mon_out[actor->mon_out_count - 1u];
            actor->mon_out_count--;
            return;
        }
    }
}

static void mon_in_remove(IdmActor *actor, uint64_t ref) {
    for (size_t i = 0; i < actor->mon_in_count; i++) {
        if (actor->mon_in[i].ref == ref) {
            actor->mon_in[i] = actor->mon_in[actor->mon_in_count - 1u];
            actor->mon_in_count--;
            return;
        }
    }
}

static void terminate(IdmScheduler *sched, IdmActor *actor, IdmValue reason) {
    if (actor->exited) return;
    bool running = actor->status == ACT_RUNNING;
    bool finished = false;
    if (actor->status == ACT_WAITING_TTY) sched->tty_waiting--;
    actor->exited = true;
    actor->status = ACT_EXITED;
    IdmError copy_err;
    idm_error_init(&copy_err);
    reason = idm_value_copy(sched->rt, &actor->heap, reason, &copy_err);
    if (copy_err.present) { idm_error_clear(&copy_err); reason = idm_atom(sched->rt, "exit"); }
    actor->exit_reason = reason;

    for (size_t i = 0; i < actor->mon_out_count; i++) {
        IdmActor *target = sched_lookup(sched, actor->mon_out[i].target);
        if (target && !target->exited) mon_in_remove(target, actor->mon_out[i].ref);
    }
    actor->mon_out_count = 0;

    for (size_t i = 0; i < actor->mon_in_count; i++) {
        IdmActor *watcher = sched_lookup(sched, actor->mon_in[i].watcher);
        if (!watcher || watcher->exited) continue;
        mon_out_remove(watcher, actor->mon_in[i].ref);
        IdmValue down = make_signal(sched, "down", idm_ref(actor->mon_in[i].ref), idm_pid(actor->pid), reason, 4);
        IdmError ignore;
        idm_error_init(&ignore);
        if (mailbox_push(watcher, down, &ignore)) wake_for_message(sched, watcher);
        idm_error_clear(&ignore);
    }
    actor->mon_in_count = 0;

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
        apply_exit_signal(sched, partner, actor->pid, reason);
    }
    free(links);

    if (actor->pid == sched->main_pid && !sched->main_terminated) {
        sched->main_terminated = true;
        sched->main_value = idm_nil();
        sched->main_reason = reason;
        sched->main_abnormal = !reason_is_normal(reason);
        finished = true;
    }
    if (actor->pid == sched->interrupt_pid) {
        sched->interrupt_pid = 0;
        sched->interrupt_struck = false;
    }
    if (actor->diag_retain && !reason_is_normal(reason)) retain_diag(sched, reason);
    if (actor->pid == sched->eval_pid && !sched->eval_done) {
        sched->eval_done = true;
        sched->eval_reason = reason;
        finished = true;
    }
    if (finished) sched_wake(sched, true);
    if (!running) {
        actor_release_resources(actor);
        if (reapable(sched, actor)) reap_queue_push(sched, actor);
    }
}

static bool tty_finish(IdmScheduler *sched, IdmActor *actor, IdmValue result, IdmError *err) {
    free(actor->tty_buf);
    actor->tty_buf = NULL;
    actor->tty_buf_len = 0;
    actor->tty_buf_cap = 0;
    sched->tty_waiting--;
    result = idm_value_copy(sched->rt, &actor->heap, result, err);
    if (err->present) return false;
    if (!idm_exec_push_result(actor->exec, result, err)) return false;
    if (!ready_push(sched, actor->pid, err)) return false;
    actor->status = ACT_READY;
    return true;
}

static bool tty_line_finish(IdmScheduler *sched, IdmActor *actor, IdmError *err) {
    IdmValue text = idm_string_n(sched->rt, actor->tty_buf ? actor->tty_buf : "", actor->tty_buf_len, err);
    if (err->present) return false;
    return tty_finish(sched, actor, make_signal(sched, "line", text, idm_nil(), idm_nil(), 2), err);
}

static bool tty_fd_readable(int fd) {
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1u, 0) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
}

static bool tty_buf_push(IdmActor *actor, char c, IdmError *err) {
    if (actor->tty_buf_len == actor->tty_buf_cap) {
        size_t cap = actor->tty_buf_cap ? actor->tty_buf_cap * 2u : 64u;
        char *next = realloc(actor->tty_buf, cap);
        if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
        actor->tty_buf = next;
        actor->tty_buf_cap = cap;
    }
    actor->tty_buf[actor->tty_buf_len++] = c;
    return true;
}

static bool tty_service(IdmScheduler *sched, IdmActor *actor, IdmError *err) {
    int fd = idm_tty_in_fd();
    if (!actor->tty_line) {
        if (tty_fd_readable(fd)) {
            unsigned char b = 0;
            ssize_t n = read(fd, &b, 1u);
            if (n == 1) return tty_finish(sched, actor, make_signal(sched, "byte", idm_int((int64_t)b), idm_nil(), idm_nil(), 2), err);
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                return tty_finish(sched, actor, idm_atom(sched->rt, "eof"), err);
            }
        }
    } else {
        while (tty_fd_readable(fd)) {
            unsigned char b = 0;
            ssize_t n = read(fd, &b, 1u);
            if (n == 1) {
                if (b == '\n') return tty_line_finish(sched, actor, err);
                if (!tty_buf_push(actor, (char)b, err)) return false;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0 && errno == EINTR) continue;
            if (actor->tty_buf_len != 0) return tty_line_finish(sched, actor, err);
            return tty_finish(sched, actor, idm_atom(sched->rt, "eof"), err);
        }
    }
    if (actor->tty_has_deadline && now_ms() >= actor->tty_deadline_ms) {
        return tty_finish(sched, actor, idm_atom(sched->rt, "timeout"), err);
    }
    return true;
}

static bool sched_service_tty(IdmScheduler *sched, IdmError *err) {
    if (sched->tty_waiting == 0) return true;
    for (size_t i = 0; i < sched->actor_count && sched->tty_waiting != 0; i++) {
        IdmActor *a = sched->actors[i];
        if (!a) continue;
        if (a->exited || a->status != ACT_WAITING_TTY) continue;
        if (!tty_service(sched, a, err)) return false;
    }
    return true;
}

static IdmActor *tty_signal_target(IdmScheduler *sched) {
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (!a) continue;
        if (!a->exited && a->status == ACT_WAITING_TTY) return a;
    }
    return NULL;
}

static void sched_check_signals(IdmScheduler *sched) {
    if (g_sig_pipe[0] < 0) return;
    char buf[16];
    ssize_t n;
    while ((n = read(g_sig_pipe[0], buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != 'r') {
                IdmActor *target = sched->interrupt_pid != 0 ? sched_lookup(sched, sched->interrupt_pid) : NULL;
                if (target && !target->exited) {
                    if (sched->interrupt_struck) {
                        terminate(sched, target, idm_atom(sched->rt, "killed"));
                    } else {
                        sched->interrupt_struck = true;
                        apply_exit_signal(sched, target, 0, idm_atom(sched->rt, "interrupt"));
                    }
                    continue;
                }
                sched->interrupt_pid = 0;
                sched->interrupt_struck = false;
            }
            IdmActor *waiter = tty_signal_target(sched);
            if (!waiter) continue;
            IdmValue signal = make_signal(sched, "signal", idm_atom(sched->rt, buf[i] == 'r' ? "resize" : "interrupt"), idm_nil(), idm_nil(), 2);
            IdmError ignore;
            idm_error_init(&ignore);
            tty_finish(sched, waiter, signal, &ignore);
            idm_error_clear(&ignore);
        }
    }
}

static IdmValue crash_reason_from_err(IdmScheduler *sched, IdmError *err) {
    if (err->present && err->notes) {
        free(sched->crash_notes);
        sched->crash_notes = idm_strdup(err->notes);
    }
    if (err->present && err->span.line != 0 && sched->crash_span.line == 0) {
        sched->crash_span = err->span;
    }
    IdmValue reason = idm_error_reason_value(sched->rt, err);
    idm_error_clear(err);
    return reason;
}

bool idm_sched_spawn(IdmScheduler *sched, IdmValue thunk, const IdmExec *parent, IdmValue *out_pid, IdmError *err) {
    sched_lock(sched);
    bool ok = sched_spawn_locked(sched, thunk, parent, out_pid, err);
    sched_unlock(sched);
    return ok;
}

static bool sched_spawn_locked(IdmScheduler *sched, IdmValue thunk, const IdmExec *parent, IdmValue *out_pid, IdmError *err) {
    IdmActor *actor = actor_create(sched, err);
    if (!actor) return false;
    if (!idm_exec_copy_context(actor->exec, parent)) return idm_error_oom(err, idm_span_unknown(NULL));
    thunk = idm_value_copy(sched->rt, &actor->heap, thunk, err);
    if (err->present) return false;
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

static bool sched_link_locked(IdmScheduler *sched, IdmActor *self, uint64_t target_pid, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err) {
    *self_should_exit = false;
    if (target_pid == self->pid) return true;
    IdmActor *target = sched_lookup(sched, target_pid);
    if (!target || target->exited) {
        IdmValue reason = (target && target->exited) ? target->exit_reason : idm_atom(sched->rt, "noproc");
        if (self->trap_exit) {
            IdmValue signal = make_signal(sched, "exit", idm_pid(target_pid), reason, idm_nil(), 3);
            return mailbox_push(self, signal, err);
        }
        if (!reason_is_normal(reason)) {
            *self_should_exit = true;
            *self_exit_reason = reason;
        }
        return true;
    }
    if (!links_add(self, target_pid, err)) return false;
    if (!links_add(target, self->pid, err)) return false;
    return true;
}

bool idm_sched_link(IdmScheduler *sched, IdmActor *self, uint64_t target_pid, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err) {
    sched_lock(sched);
    bool ok = sched_link_locked(sched, self, target_pid, self_should_exit, self_exit_reason, err);
    sched_unlock(sched);
    return ok;
}

bool idm_sched_exit_signal(IdmScheduler *sched, IdmActor *self, uint64_t target_pid, IdmValue reason, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err) {
    sched_lock(sched);
    *self_should_exit = false;
    bool untrappable = reason_is_atom(reason, "kill");
    IdmValue applied = untrappable ? idm_atom(sched->rt, "killed") : reason;
    if (target_pid == self->pid) {
        if (untrappable) {
            *self_should_exit = true;
            *self_exit_reason = applied;
        } else if (self->trap_exit) {
            IdmValue signal = make_signal(sched, "exit", idm_pid(self->pid), reason, idm_nil(), 3);
            bool ok = mailbox_push(self, signal, err);
            sched_unlock(sched);
            return ok;
        } else if (!reason_is_normal(reason)) {
            *self_should_exit = true;
            *self_exit_reason = reason;
        }
        sched_unlock(sched);
        return true;
    }
    IdmActor *target = sched_lookup(sched, target_pid);
    if (target && !target->exited) {
        if (untrappable) terminate(sched, target, applied);
        else apply_exit_signal(sched, target, self->pid, reason);
    }
    sched_unlock(sched);
    return true;
}

bool idm_sched_unlink(IdmScheduler *sched, IdmActor *self, uint64_t target_pid) {
    sched_lock(sched);
    links_remove(self, target_pid);
    IdmActor *target = sched_lookup(sched, target_pid);
    if (target && !target->exited) links_remove(target, self->pid);
    sched_unlock(sched);
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

static bool sched_monitor_locked(IdmScheduler *sched, IdmActor *self, uint64_t target_pid, IdmValue *out_ref, IdmError *err) {
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

bool idm_sched_monitor(IdmScheduler *sched, IdmActor *self, uint64_t target_pid, IdmValue *out_ref, IdmError *err) {
    sched_lock(sched);
    bool ok = sched_monitor_locked(sched, self, target_pid, out_ref, err);
    sched_unlock(sched);
    return ok;
}

bool idm_sched_spawn_link(IdmScheduler *sched, IdmValue thunk, const IdmExec *parent, IdmActor *self, IdmValue *out_pid, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err) {
    sched_lock(sched);
    *self_should_exit = false;
    IdmValue pid = idm_nil();
    bool ok = sched_spawn_locked(sched, thunk, parent, &pid, err);
    if (ok) {
        *out_pid = pid;
        ok = sched_link_locked(sched, self, pid.as.id, self_should_exit, self_exit_reason, err);
    }
    sched_unlock(sched);
    return ok;
}

bool idm_sched_spawn_monitor(IdmScheduler *sched, IdmValue thunk, const IdmExec *parent, IdmActor *self, IdmValue *out_pid, IdmValue *out_ref, IdmError *err) {
    sched_lock(sched);
    IdmValue pid = idm_nil();
    bool ok = sched_spawn_locked(sched, thunk, parent, &pid, err);
    if (ok) {
        *out_pid = pid;
        ok = sched_monitor_locked(sched, self, pid.as.id, out_ref, err);
    }
    sched_unlock(sched);
    return ok;
}

bool idm_sched_demonitor(IdmScheduler *sched, IdmActor *self, IdmValue ref) {
    sched_lock(sched);
    if (ref.tag != IDM_VAL_REF) { sched_unlock(sched); return false; }
    uint64_t ref_id = ref.as.id;
    uint64_t target_pid = 0;
    for (size_t i = 0; i < self->mon_out_count; i++) {
        if (self->mon_out[i].ref == ref_id) {
            target_pid = self->mon_out[i].target;
            break;
        }
    }
    mon_out_remove(self, ref_id);
    if (target_pid != 0) {
        IdmActor *target = sched_lookup(sched, target_pid);
        if (target) mon_in_remove(target, ref_id);
    }
    sched_unlock(sched);
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
    IdmActor *a = (IdmActor *)actor;
    pthread_mutex_lock(&a->mbox_mu);
    size_t n = actor->mb_count;
    pthread_mutex_unlock(&a->mbox_mu);
    return n;
}

bool idm_actor_mailbox_peek(const IdmActor *actor, size_t index, IdmValue *out) {
    IdmActor *a = (IdmActor *)actor;
    pthread_mutex_lock(&a->mbox_mu);
    bool ok = index < actor->mb_count;
    if (ok) *out = actor->mailbox[index];
    pthread_mutex_unlock(&a->mbox_mu);
    return ok;
}

bool idm_actor_mailbox_remove(IdmActor *actor, size_t index, IdmValue *out) {
    pthread_mutex_lock(&actor->mbox_mu);
    bool ok = index < actor->mb_count;
    if (ok) {
        *out = actor->mailbox[index];
        memmove(actor->mailbox + index, actor->mailbox + index + 1u, (actor->mb_count - index - 1u) * sizeof(*actor->mailbox));
        actor->mb_count--;
    }
    pthread_mutex_unlock(&actor->mbox_mu);
    return ok;
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

void idm_actor_recv_set_cursor(IdmActor *actor, size_t cursor) {
    actor->recv_cursor = cursor;
}

size_t idm_actor_recv_start(IdmActor *actor, const void *module, size_t site) {
    if (actor->recv_ctx_module != module || actor->recv_ctx_site != site) {
        actor->recv_ctx_module = module;
        actor->recv_ctx_site = site;
        actor->recv_cursor = 0;
    }
    return actor->recv_cursor;
}
void idm_actor_recv_matched(IdmActor *actor, size_t cursor) {
    actor->recv_waiting = false;
    actor->recv_has_deadline = false;
    actor->recv_cursor = cursor;
}

static bool sched_port_data_value(IdmScheduler *sched, const char *data, size_t len, IdmValue *out, IdmError *err);
static bool sched_port_ok_count_value(IdmScheduler *sched, size_t n, IdmValue *out, IdmError *err);
static IdmValue sched_port_io_atom(IdmScheduler *sched, IdmPortIoStatus status);

static bool port_io_finish(IdmScheduler *sched, IdmActor *actor, IdmValue result, IdmError *err) {
    result = idm_value_copy(sched->rt, &actor->heap, result, err);
    if (err->present) return false;
    if (!idm_exec_push_result(actor->exec, result, err)) return false;
    if (!ready_push(sched, actor->pid, err)) return false;
    actor->port_io_port_id = 0;
    actor->port_io_stream = NULL;
    actor->port_io_max = 0;
    actor->port_io_data = idm_nil();
    actor->status = ACT_READY;
    return true;
}

static bool service_port_read(IdmScheduler *sched, IdmActor *actor, IdmError *err) {
    PortEntry *entry = sched_find_port(sched, actor->port_io_port_id);
    if (!entry || !entry->port) {
        return port_io_finish(sched, actor, idm_atom(sched->rt, "closed"), err);
    }
    char *data = NULL;
    size_t len = 0;
    IdmPortIoStatus status = IDM_PORT_IO_CLOSED;
    bool ok = idm_port_read(entry->port, actor->port_io_stream, actor->port_io_max, &data, &len, &status, err);
    if (!ok) { free(data); return false; }
    if (status == IDM_PORT_IO_AGAIN) {
        free(data);
        return true;
    }
    IdmValue result = idm_nil();
    if (status == IDM_PORT_IO_OK) ok = sched_port_data_value(sched, data, len, &result, err);
    else result = sched_port_io_atom(sched, status);
    free(data);
    if (!ok) return false;
    return port_io_finish(sched, actor, result, err);
}

static bool service_port_write(IdmScheduler *sched, IdmActor *actor, IdmError *err) {
    PortEntry *entry = sched_find_port(sched, actor->port_io_port_id);
    if (!entry || !entry->port) {
        return port_io_finish(sched, actor, idm_atom(sched->rt, "closed"), err);
    }
    size_t len = idm_string_length(actor->port_io_data);
    const char *data = idm_string_bytes(actor->port_io_data);
    size_t written = 0;
    IdmPortIoStatus status = IDM_PORT_IO_CLOSED;
    bool ok = idm_port_write(entry->port, data, len, &written, &status, err);
    if (!ok) return false;
    if (status == IDM_PORT_IO_AGAIN) return true;
    IdmValue result = idm_nil();
    if (status == IDM_PORT_IO_OK) ok = sched_port_ok_count_value(sched, written, &result, err);
    else result = sched_port_io_atom(sched, status);
    if (!ok) return false;
    return port_io_finish(sched, actor, result, err);
}

static bool sched_service_port_io(IdmScheduler *sched, IdmError *err) {
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (!a || a->exited) continue;
        if (a->status == ACT_WAITING_PORT_READ) {
            if (!service_port_read(sched, a, err)) return false;
        } else if (a->status == ACT_WAITING_PORT_WRITE) {
            if (!service_port_write(sched, a, err)) return false;
        }
    }
    return true;
}

static bool sched_has_port_write_waiter(IdmScheduler *sched, uint64_t port_id) {
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (a && !a->exited && a->status == ACT_WAITING_PORT_WRITE && a->port_io_port_id == port_id) return true;
    }
    return false;
}

static bool sched_poll(IdmScheduler *sched, bool mt, bool block, IdmError *err) {
    if (!sched_service_port_io(sched, err)) return false;
    if (block && sched->ready_count != 0) return true;
    bool any_live = false;
    bool any_deadline = false;
    uint64_t nearest = 0;
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (!a) continue;
        if (a->exited) continue;
        any_live = true;
        if (a->status == ACT_WAITING_RECEIVE && a->recv_has_deadline) {
            if (!any_deadline || a->recv_deadline_ms < nearest) {
                nearest = a->recv_deadline_ms;
                any_deadline = true;
            }
        }
        if (a->status == ACT_WAITING_TTY && a->tty_has_deadline) {
            if (!any_deadline || a->tty_deadline_ms < nearest) {
                nearest = a->tty_deadline_ms;
                any_deadline = true;
            }
        }
    }
    sched->deadline_hint = any_deadline ? nearest : UINT64_MAX;
    bool any_port = sched->ports_pending != 0;
    bool any_tty = sched->tty_waiting != 0;
    if (block) {
        if (!any_live) return true;
        if (!any_port && !any_deadline && !any_tty) {
            bool has_sig = g_sig_pipe[0] >= 0 && sched->interrupt_pid != 0;
            if ((!mt || sched->parked == sched->nworkers - 1u) && !has_sig) {
                return sched_crash_stall_error(sched, err);
            }
            struct pollfd waitfds[2];
            nfds_t wait_count = 0;
            if (has_sig) {
                waitfds[wait_count].fd = g_sig_pipe[0];
                waitfds[wait_count].events = POLLIN;
                waitfds[wait_count].revents = 0;
                wait_count++;
            }
            if (mt && sched->wake_pipe[0] >= 0) {
                waitfds[wait_count].fd = sched->wake_pipe[0];
                waitfds[wait_count].events = POLLIN;
                waitfds[wait_count].revents = 0;
                wait_count++;
            }
            if (mt) sched_unlock(sched);
            poll(wait_count != 0 ? waitfds : NULL, wait_count, -1);
            if (mt) sched_lock(sched);
            char drain[64];
            while (mt && sched->wake_pipe[0] >= 0 && read(sched->wake_pipe[0], drain, sizeof(drain)) > 0) {}
            sched_check_signals(sched);
            return true;
        }
        size_t fd_cap = sched->ports_pending * 3u + (mt ? 1u : 0u) + 2u;
        struct pollfd *fds = fd_cap != 0 ? calloc(fd_cap, sizeof(*fds)) : NULL;
        if (fd_cap != 0 && !fds) return idm_error_oom(err, idm_span_unknown(NULL));
        nfds_t nfds = 0;
        if (mt && sched->wake_pipe[0] >= 0) {
            fds[nfds].fd = sched->wake_pipe[0];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        if (g_sig_pipe[0] >= 0) {
            fds[nfds].fd = g_sig_pipe[0];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        if (any_tty) {
            fds[nfds].fd = idm_tty_in_fd();
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
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
            int input_fd = sched_has_port_write_waiter(sched, e->id) ? idm_port_input_fd(e->port) : -1;
            if (input_fd >= 0) {
                fds[nfds].fd = input_fd;
                fds[nfds].events = POLLOUT;
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
        if (nfds > 0 || timeout >= 0) {
            if (mt) sched_unlock(sched);
            poll(nfds > 0 ? fds : NULL, nfds, timeout);
            if (mt) sched_lock(sched);
            if (mt && sched->wake_pipe[0] >= 0) {
                char drain[64];
                while (read(sched->wake_pipe[0], drain, sizeof(drain)) > 0) {}
            }
            sched_check_signals(sched);
        }
        free(fds);
    }

    if (!sched_service_tty(sched, err)) return false;
    for (size_t i = 0; i < sched->port_count; i++) {
        PortEntry *e = &sched->ports[i];
        if (e->done || !e->port) continue;
        if (idm_port_try_complete(e->port)) {
            e->done = true;
            sched->ports_pending--;
        }
    }
    if (!sched_service_port_io(sched, err)) return false;
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (!a) continue;
        if (a->exited || a->status != ACT_WAITING_AWAIT) continue;
        PortEntry *e = sched_find_port(sched, a->await_port_id);
        if (e && e->done) {
            if (!port_entry_ensure_result(sched, e, err)) return false;
            IdmValue r = idm_value_copy(sched->rt, &a->heap, e->result, err);
            if (err->present) return false;
            if (!idm_exec_push_result(a->exec, r, err)) return false;
            if (!ready_push(sched, a->pid, err)) return false;
            a->status = ACT_READY;
        } else if (e && e->port && idm_port_foreground(e->port) && idm_port_stopped(e->port)) {
            IdmValue detail[2];
            detail[0] = idm_atom(sched->rt, "suspended");
            detail[1] = idm_port(e->id);
            IdmValue error_detail = idm_tuple(sched->rt, detail, 2u, err);
            if (err->present) return false;
            IdmValue reason = idm_error_value(sched->rt, error_detail);
            idm_exec_inject_raise(a->exec, reason);
            if (!ready_push(sched, a->pid, err)) return false;
            a->status = ACT_READY;
        }
    }

    uint64_t reached = now_ms();
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (!a) continue;
        if (a->exited || a->status != ACT_WAITING_RECEIVE) continue;
        if (a->recv_has_deadline && reached >= a->recv_deadline_ms) {
            if (!ready_push(sched, a->pid, err)) return false;
            a->status = ACT_READY;
        }
    }
    return true;
}

static bool run_slice(IdmScheduler *sched, uint64_t pid, IdmError *err) {
    sched_lock(sched);
    drain_reap(sched);
    IdmActor *actor = sched_lookup(sched, pid);
    if (!actor || actor->exited || actor->status != ACT_READY) {
        sched_unlock(sched);
        return true;
    }
    actor->status = ACT_RUNNING;
    sched_unlock(sched);
    IdmExecStatus status = IDM_EXEC_DONE;
    IdmValue result = idm_nil();
    IdmValue reason = idm_nil();
    idm_set_active_heap(&actor->heap);
    bool stepped = idm_exec_step(actor->exec, IDM_ACTOR_REDUCTIONS, &status, &result, &reason, err);
    idm_set_active_heap(NULL);
    sched_lock(sched);
    if (actor->exited) {
        actor_release_resources(actor);
        if (!stepped) idm_error_clear(err);
    } else if (!stepped) {
        actor->status = ACT_READY;
        IdmValue crash_reason = crash_reason_from_err(sched, err);
        terminate(sched, actor, crash_reason);
    } else {
        actor->status = ACT_READY;
        switch (status) {
                case IDM_EXEC_DONE:
                    if (actor->pid == sched->main_pid && !sched->main_terminated) {
                        sched->main_terminated = true;
                        sched->main_value = result;
                        sched->main_abnormal = false;
                        sched_wake(sched, true);
                    }
                    if (actor->pid == sched->eval_pid && !sched->eval_done) sched->eval_value = result;
                    terminate(sched, actor, idm_atom(sched->rt, "normal"));
                    break;
                case IDM_EXEC_YIELD:
                    if (!ready_push(sched, actor->pid, err)) { sched_unlock(sched); return false; }
                    break;
                case IDM_EXEC_BLOCK_RECEIVE:
                    actor->status = ACT_WAITING_RECEIVE;
                    if (actor->recv_has_deadline && actor->recv_deadline_ms < sched->deadline_hint) sched->deadline_hint = actor->recv_deadline_ms;
                    if (idm_actor_mailbox_count(actor) > actor->recv_cursor) {
                        actor->status = ACT_READY;
                        if (!ready_push(sched, actor->pid, err)) { sched_unlock(sched); return false; }
                    }
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
                    if (!sched_register_port(sched, p, &port_id, err)) { idm_port_free(p); sched_unlock(sched); return false; }
                    if (!idm_exec_push_result(actor->exec, idm_port(port_id), err)) { sched_unlock(sched); return false; }
                    if (!ready_push(sched, actor->pid, err)) { sched_unlock(sched); return false; }
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
                        if (!port_entry_ensure_result(sched, entry, err)) { sched_unlock(sched); return false; }
                        IdmValue r = idm_value_copy(sched->rt, &actor->heap, entry->result, err);
                        if (err->present) { sched_unlock(sched); return false; }
                        if (!idm_exec_push_result(actor->exec, r, err)) { sched_unlock(sched); return false; }
                        if (!ready_push(sched, actor->pid, err)) { sched_unlock(sched); return false; }
                    } else {
                        actor->await_port_id = port_val.as.id;
                        actor->status = ACT_WAITING_AWAIT;
                    }
                    break;
                }
                case IDM_EXEC_BLOCK_PORT_READ: {
                    IdmValue port_val = idm_nil();
                    const char *stream = NULL;
                    size_t max = 0;
                    if (!idm_exec_take_port_read(actor->exec, &port_val, &stream, &max)) {
                        terminate(sched, actor, idm_atom(sched->rt, "noproc"));
                        break;
                    }
                    PortEntry *entry = sched_find_port(sched, port_val.as.id);
                    if (!entry || (!entry->port && !entry->has_result)) {
                        IdmValue closed = idm_atom(sched->rt, "closed");
                        if (!idm_exec_push_result(actor->exec, closed, err)) { sched_unlock(sched); return false; }
                        if (!ready_push(sched, actor->pid, err)) { sched_unlock(sched); return false; }
                    } else if (!entry->port) {
                        IdmValue closed = idm_atom(sched->rt, "closed");
                        if (!idm_exec_push_result(actor->exec, closed, err)) { sched_unlock(sched); return false; }
                        if (!ready_push(sched, actor->pid, err)) { sched_unlock(sched); return false; }
                    } else {
                        actor->port_io_port_id = port_val.as.id;
                        actor->port_io_stream = stream;
                        actor->port_io_max = max;
                        actor->status = ACT_WAITING_PORT_READ;
                        if (!sched_service_port_io(sched, err)) { sched_unlock(sched); return false; }
                    }
                    break;
                }
                case IDM_EXEC_BLOCK_PORT_WRITE: {
                    IdmValue port_val = idm_nil();
                    IdmValue data = idm_nil();
                    if (!idm_exec_take_port_write(actor->exec, &port_val, &data)) {
                        terminate(sched, actor, idm_atom(sched->rt, "noproc"));
                        break;
                    }
                    PortEntry *entry = sched_find_port(sched, port_val.as.id);
                    if (!entry || !entry->port) {
                        IdmValue closed = idm_atom(sched->rt, "closed");
                        if (!idm_exec_push_result(actor->exec, closed, err)) { sched_unlock(sched); return false; }
                        if (!ready_push(sched, actor->pid, err)) { sched_unlock(sched); return false; }
                    } else {
                        actor->port_io_port_id = port_val.as.id;
                        actor->port_io_data = data;
                        actor->status = ACT_WAITING_PORT_WRITE;
                        if (!sched_service_port_io(sched, err)) { sched_unlock(sched); return false; }
                    }
                    break;
                }
                case IDM_EXEC_BLOCK_TTY: {
                    bool line_mode = false;
                    bool has_timeout = false;
                    int64_t timeout_ms = 0;
                    if (!idm_exec_take_tty(actor->exec, &line_mode, &has_timeout, &timeout_ms)) {
                        terminate(sched, actor, idm_atom(sched->rt, "noproc"));
                        break;
                    }
                    actor->tty_line = line_mode;
                    actor->tty_has_deadline = has_timeout;
                    actor->tty_deadline_ms = has_timeout ? now_ms() + (uint64_t)timeout_ms : 0;
                    actor->tty_buf_len = 0;
                    actor->status = ACT_WAITING_TTY;
                    sched->tty_waiting++;
                    if (has_timeout && actor->tty_deadline_ms < sched->deadline_hint) sched->deadline_hint = actor->tty_deadline_ms;
                    break;
                }
                case IDM_EXEC_EXIT:
                    terminate(sched, actor, reason);
                    break;
            }
    }
    bool ok = true;
    sched_check_signals(sched);
    if (sched->ports_pending != 0 || sched->tty_waiting != 0 || now_ms() >= sched->deadline_hint) ok = sched_poll(sched, sched->nworkers > 1u, false, err);
    if (!actor->exited) actor_maybe_collect(actor);
    if (reapable(sched, actor)) reap_queue_push(sched, actor);
    sched_unlock(sched);
    return ok;
}


static void worker_fatal(IdmScheduler *sched, IdmError *err) {
    if (!sched->fatal) {
        sched->fatal = true;
        sched->fatal_err = *err;
        idm_error_init(err);
    } else {
        idm_error_clear(err);
    }
    if (!sched->shutdown) {
        sched->shutdown = true;
        sched_wake(sched, true);
    }
}

static void *worker_main(void *argp) {
    IdmScheduler *sched = argp;
    IdmError err;
    idm_error_init(&err);
    sched_lock(sched);
    for (;;) {
        if (sched->shutdown) break;
        if (sched->main_terminated || sched->eval_done) {
            if (!sched->shutdown) {
                sched->shutdown = true;
                sched_wake(sched, true);
            }
            break;
        }
        sched_maybe_collect(sched);
        uint64_t pid = 0;
        if (ready_pop(sched, &pid)) {
            sched_unlock(sched);
            bool ok = run_slice(sched, pid, &err);
            sched_lock(sched);
            if (!ok || err.present) {
                worker_fatal(sched, &err);
                break;
            }
            continue;
        }
        if (!sched->poller_active) {
            sched->poller_active = true;
            bool ok = sched_poll(sched, true, true, &err);
            sched->poller_active = false;
            if (!ok || err.present) {
                worker_fatal(sched, &err);
                break;
            }
            continue;
        }
        sched->parked++;
        sched_wake(sched, false);
        pthread_cond_wait(&sched->work_cv, &sched->mu);
        sched->parked--;
    }
    pthread_cond_broadcast(&sched->work_cv);
    sched_unlock(sched);
    idm_error_clear(&err);
    return NULL;
}

bool idm_sched_run_main(IdmScheduler *sched, uint32_t main_fn, IdmValue *out_result, IdmError *err) {
    IdmActor *main_actor = actor_create(sched, err);
    if (!main_actor) return false;
    sched->main_pid = main_actor->pid;
    if (!idm_exec_setup_function(main_actor->exec, main_fn, err)) return false;
    if (!ready_push(sched, main_actor->pid, err)) return false;

    if (sched->nworkers > 1u) {
        pthread_t *threads = calloc(sched->nworkers - 1u, sizeof(*threads));
        if (!threads) return idm_error_oom(err, idm_span_unknown(NULL));
        size_t started = 0;
        size_t want = sched->nworkers - 1u;
        for (size_t i = 0; i < want; i++) {
            if (pthread_create(&threads[i], NULL, worker_main, sched) != 0) {
                sched_lock(sched);
                sched->nworkers = started + 1u;
                sched_unlock(sched);
                break;
            }
            started++;
        }
        worker_main(sched);
        for (size_t i = 0; i < started; i++) pthread_join(threads[i], NULL);
        free(threads);
        if (sched->fatal) {
            *err = sched->fatal_err;
            idm_error_init(&sched->fatal_err);
            return false;
        }
        if (!sched->main_terminated) {
            return idm_error_set(err, idm_span_unknown(NULL), "scheduler stopped before the main actor finished");
        }
    } else {
        while (!sched->main_terminated) {
            sched_maybe_collect(sched);
            uint64_t pid = 0;
            if (ready_pop(sched, &pid)) {
                if (!run_slice(sched, pid, err)) return false;
                if (err->present) return false;
            } else {
                if (!sched_poll(sched, false, true, err)) return false;
                if (err->present) return false;
            }
        }
    }

    if (sched->main_abnormal) return sched_abnormal_error(sched, sched->main_reason, err);
    *out_result = sched->main_value;
    return true;
}

void idm_sched_watch(IdmScheduler *sched, uint64_t pid) {
    sched_lock(sched);
    IdmActor *actor = sched_lookup(sched, pid);
    if (actor && !actor->exited) {
        actor->diag_retain = true;
        sched->interrupt_pid = pid;
        sched->interrupt_struck = false;
        free(sched->crash_notes);
        sched->crash_notes = NULL;
        sched->crash_span = idm_span_unknown(NULL);
    }
    sched_unlock(sched);
}

char *idm_sched_take_diagnostic(IdmScheduler *sched) {
    sched_lock(sched);
    char *diag = sched->retained_diag;
    sched->retained_diag = NULL;
    sched_unlock(sched);
    return diag;
}

bool idm_sched_port_status(IdmScheduler *sched, uint64_t port_id, int *out_state) {
    sched_lock(sched);
    PortEntry *e = sched_find_port(sched, port_id);
    bool found = e != NULL;
    if (found) {
        if (e->done) *out_state = 2;
        else if (e->port && idm_port_stopped(e->port)) *out_state = 1;
        else *out_state = 0;
    }
    sched_unlock(sched);
    return found;
}

static bool sched_port_data_value(IdmScheduler *sched, const char *data, size_t len, IdmValue *out, IdmError *err) {
    IdmValue text = idm_string_n(sched->rt, data ? data : "", len, err);
    if (err->present) return false;
    IdmValue items[2];
    items[0] = idm_atom(sched->rt, "data");
    items[1] = text;
    *out = idm_tuple(sched->rt, items, 2u, err);
    return !(err && err->present);
}

static bool sched_port_ok_count_value(IdmScheduler *sched, size_t n, IdmValue *out, IdmError *err) {
    IdmValue items[2];
    items[0] = idm_atom(sched->rt, "ok");
    items[1] = idm_int((int64_t)n);
    *out = idm_tuple(sched->rt, items, 2u, err);
    return !(err && err->present);
}

static IdmValue sched_port_io_atom(IdmScheduler *sched, IdmPortIoStatus status) {
    switch (status) {
        case IDM_PORT_IO_AGAIN: return idm_atom(sched->rt, "again");
        case IDM_PORT_IO_EOF: return idm_atom(sched->rt, "eof");
        case IDM_PORT_IO_CLOSED: return idm_atom(sched->rt, "closed");
        case IDM_PORT_IO_OK: return idm_atom(sched->rt, "ok");
    }
    return idm_atom(sched->rt, "error");
}

bool idm_sched_port_read(IdmScheduler *sched, uint64_t port_id, const char *stream, size_t max, IdmValue *out, bool *out_found, IdmError *err) {
    sched_lock(sched);
    *out_found = false;
    PortEntry *e = sched_find_port(sched, port_id);
    if (!e || !e->port) {
        sched_unlock(sched);
        return true;
    }
    char *data = NULL;
    size_t len = 0;
    IdmPortIoStatus status = IDM_PORT_IO_CLOSED;
    bool ok = idm_port_read(e->port, stream, max, &data, &len, &status, err);
    if (ok) {
        *out_found = true;
        if (status == IDM_PORT_IO_OK) ok = sched_port_data_value(sched, data, len, out, err);
        else *out = sched_port_io_atom(sched, status);
    }
    free(data);
    sched_unlock(sched);
    return ok;
}

bool idm_sched_port_write(IdmScheduler *sched, uint64_t port_id, const char *data, size_t len, IdmValue *out, bool *out_found, IdmError *err) {
    sched_lock(sched);
    *out_found = false;
    PortEntry *e = sched_find_port(sched, port_id);
    if (!e || !e->port) {
        sched_unlock(sched);
        return true;
    }
    size_t written = 0;
    IdmPortIoStatus status = IDM_PORT_IO_CLOSED;
    bool ok = idm_port_write(e->port, data, len, &written, &status, err);
    if (ok) {
        *out_found = true;
        if (status == IDM_PORT_IO_OK) ok = sched_port_ok_count_value(sched, written, out, err);
        else *out = sched_port_io_atom(sched, status);
    }
    sched_unlock(sched);
    return ok;
}

bool idm_sched_port_close_input(IdmScheduler *sched, uint64_t port_id, IdmValue *out, bool *out_found, IdmError *err) {
    (void)err;
    sched_lock(sched);
    *out_found = false;
    PortEntry *e = sched_find_port(sched, port_id);
    if (!e || !e->port) {
        sched_unlock(sched);
        return true;
    }
    IdmPortIoStatus status = idm_port_close_input(e->port);
    if (!e->done && idm_port_try_complete(e->port)) {
        e->done = true;
        sched->ports_pending--;
    }
    *out_found = true;
    *out = sched_port_io_atom(sched, status);
    sched_unlock(sched);
    return true;
}

bool idm_sched_job_resume(IdmScheduler *sched, uint64_t port_id, bool fg) {
    sched_lock(sched);
    PortEntry *e = sched_find_port(sched, port_id);
    bool found = e != NULL;
    if (found && !e->done && e->port) idm_port_resume(e->port, fg);
    sched_unlock(sched);
    return found;
}

bool idm_sched_job_signal(IdmScheduler *sched, uint64_t port_id, int signo) {
    sched_lock(sched);
    PortEntry *e = sched_find_port(sched, port_id);
    bool found = e != NULL;
    if (found && !e->done && e->port) idm_port_signal_group(e->port, signo);
    sched_unlock(sched);
    return found;
}

static void eval_contain_failure(IdmScheduler *sched, IdmError *err) {
    sched_lock(sched);
    IdmValue reason = crash_reason_from_err(sched, err);
    IdmActor *actor = sched_lookup(sched, sched->eval_pid);
    if (actor && !actor->exited) {
        terminate(sched, actor, reason);
    } else if (!sched->eval_done) {
        sched->eval_done = true;
        sched->eval_reason = reason;
    }
    sched_unlock(sched);
}

static bool sched_run_thunk(IdmScheduler *sched, IdmValue thunk, bool interrupt_target, bool register_session, IdmValue *out_value, IdmValue *out_reason, IdmError *err) {
    sched_lock(sched);
    sig_drain_pending();
    free(sched->crash_notes);
    sched->crash_notes = NULL;
    sched->crash_span = idm_span_unknown(NULL);
    sched->eval_value = idm_nil();
    sched->eval_reason = idm_nil();
    sched->eval_done = false;
    sched->eval_pid = 0;
    IdmValue pid = idm_nil();
    if (!sched_spawn_locked(sched, thunk, NULL, &pid, err)) {
        sched_unlock(sched);
        return false;
    }
    sched->eval_pid = pid.as.id;
    if (register_session && sched->rt->repl) idm_repl_set_session_pid(sched->rt->repl, pid.as.id);
    IdmActor *actor = sched_lookup(sched, pid.as.id);
    actor->diag_retain = true;
    if (actor->exited) {
        sched->eval_done = true;
        sched->eval_reason = actor->exit_reason;
        if (!reason_is_normal(actor->exit_reason)) retain_diag(sched, actor->exit_reason);
    } else if (interrupt_target) {
        sched->interrupt_pid = pid.as.id;
        sched->interrupt_struck = false;
    }
    sched_unlock(sched);

    if (sched->nworkers > 1u) {
        pthread_t *threads = calloc(sched->nworkers - 1u, sizeof(*threads));
        if (!threads) {
            IdmError oom;
            idm_error_init(&oom);
            idm_error_oom(&oom, idm_span_unknown(NULL));
            eval_contain_failure(sched, &oom);
        } else {
            size_t started = 0;
            size_t want = sched->nworkers - 1u;
            for (size_t i = 0; i < want; i++) {
                if (pthread_create(&threads[i], NULL, worker_main, sched) != 0) {
                    sched_lock(sched);
                    sched->nworkers = started + 1u;
                    sched_unlock(sched);
                    break;
                }
                started++;
            }
            worker_main(sched);
            for (size_t i = 0; i < started; i++) pthread_join(threads[i], NULL);
            free(threads);
            sched_lock(sched);
            sched->shutdown = false;
            if (sched->fatal) {
                sched->fatal = false;
                IdmError fatal = sched->fatal_err;
                idm_error_init(&sched->fatal_err);
                sched_unlock(sched);
                eval_contain_failure(sched, &fatal);
                idm_error_clear(&fatal);
            } else {
                sched_unlock(sched);
            }
        }
    } else {
        while (!sched->eval_done) {
            sched_maybe_collect(sched);
            uint64_t next = 0;
            bool ok = ready_pop(sched, &next) ? run_slice(sched, next, err) : sched_poll(sched, false, true, err);
            if (!ok || err->present) eval_contain_failure(sched, err);
        }
    }

    sched_lock(sched);
    if (!sched->eval_done) {
        sched->eval_done = true;
        sched->eval_reason = idm_atom(sched->rt, "killed");
    }
    sched->eval_pid = 0;
    *out_value = sched->eval_value;
    *out_reason = sched->eval_reason;
    sched_unlock(sched);
    return true;
}

bool idm_sched_eval(IdmScheduler *sched, IdmValue thunk, IdmValue *out_value, IdmError *err) {
    IdmValue value = idm_nil();
    IdmValue reason = idm_nil();
    if (!sched_run_thunk(sched, thunk, true, false, &value, &reason, err)) return false;
    if (!reason_is_normal(reason)) return sched_abnormal_error(sched, reason, err);
    *out_value = value;
    return true;
}

bool idm_sched_run_session(IdmScheduler *sched, IdmValue thunk, bool register_session, IdmValue *out_value, IdmValue *out_reason, IdmError *err) {
    return sched_run_thunk(sched, thunk, false, register_session, out_value, out_reason, err);
}

int idm_sched_session_status(IdmScheduler *sched, IdmValue value, IdmValue reason) {
    if (reason_is_normal(reason)) {
        return value.tag == IDM_VAL_INT ? (int)(((value.as.i % 256) + 256) % 256) : 0;
    }
    if (reason.tag == IDM_VAL_INT) return (int)(((reason.as.i % 256) + 256) % 256);
    char *diag = idm_sched_take_diagnostic(sched);
    if (diag) {
        fprintf(stderr, "%s\n", diag);
        free(diag);
    } else {
        IdmError err;
        idm_error_init(&err);
        sched_abnormal_error(sched, reason, &err);
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
    }
    return 70;
}
