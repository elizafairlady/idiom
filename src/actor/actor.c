#include "idiom/actor.h"

#include "idiom/bytecode.h"
#include "idiom/expand.h"
#include "idiom/ports.h"
#include "idiom/tty.h"

#include <assert.h>
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
#define IDM_GC_STEPS_PER_REDUCTION 10

typedef enum {
    ACT_READY,
    ACT_RUNNING,
    ACT_WAITING_RECEIVE,
    ACT_WAITING_PORT_READ,
    ACT_WAITING_PORT_WRITE,
    ACT_WAITING_TTY,
    ACT_EXITED
} ActorStatus;

typedef enum {
    GC_ROOT_EXEC,
    GC_ROOT_MAILBOX,
    GC_ROOT_DONE
} ActorGcRootStage;

typedef struct {
    enum { PROC_ACTOR, PROC_PORT } kind;
    uint64_t id;
} ProcId;

typedef struct {
    uint64_t ref;
    ProcId peer;
} Monitor;

typedef struct {
    struct IdmActor *target;
    struct IdmActor *source;
    const char *tag;
    IdmValue a;
    IdmValue b;
    IdmValue c;
    size_t count;
    bool port_reason;
    bool terminate;
    int port_exit_code;
} PendingSignal;

typedef struct {
    uint64_t id;
    uint32_t gen;
    uint32_t refs;
    bool live;
    bool out_readable;
    bool err_readable;
    bool eof_out;
    bool eof_err;
    IdmPort *port;
    bool done;
    bool is_process;
    bool pending_completion;
    bool notified_stopped;
    bool has_result;
    int exit_code;
    ProcId *links;
    size_t link_count;
    size_t link_cap;
    Monitor *mon_in;
    size_t mon_in_count;
    size_t mon_in_cap;
} PortEntry;

typedef enum {
    PORT_IO_READ,
    PORT_IO_WRITE
} PortIoOp;

struct IdmActor {
    uint64_t pid;
    IdmRuntime *rt;
    IdmExec *exec;
    ActorStatus status;
    _Atomic bool trap_exit;

    IdmHeap heap;
    size_t gc_threshold;
    IdmExecRootCursor gc_exec_roots;
    size_t gc_mailbox_index;
    size_t gc_mailbox_limit;
    size_t gc_mailbox_base;
    bool gc_mailbox_marking;
    ActorGcRootStage gc_root_stage;

    pthread_mutex_t mbox_mu;
    IdmValue *mailbox;
    size_t mb_head;
    size_t mb_count;
    size_t mb_cap;

    ProcId *links;
    size_t link_count;
    size_t link_cap;

    Monitor *mon_out;
    size_t mon_out_count;
    size_t mon_out_cap;

    Monitor *mon_in;
    size_t mon_in_count;
    size_t mon_in_cap;

    bool recv_waiting;
    bool wait_has_deadline;
    uint64_t wait_deadline_ms;
    size_t recv_cursor;
    const void *recv_ctx_module;
    size_t recv_ctx_site;

    uint64_t port_io_port_id;
    const char *port_io_stream;
    size_t port_io_max;
    IdmValue port_io_data;

    bool tty_line;
    char *tty_buf;
    size_t tty_buf_len;
    size_t tty_buf_cap;

    bool exited;
    bool reap_queued;
    bool detached;
    uint32_t refs;
    bool mbox_closed;
    IdmValue exit_reason;
};

typedef struct {
    pthread_mutex_t mu;
    uint64_t *items;
    size_t head;
    _Atomic size_t count;
    size_t cap;
} RunQueue;

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
    _Atomic size_t reap_count;
    size_t reap_cap;

    RunQueue *queues;
    size_t queue_count;
    bool queue_locking;
    _Atomic size_t ready_total;
    _Atomic size_t place_next;
    _Atomic size_t worker_seq;

    uint64_t next_ref;
    uint64_t main_pid;

    PortEntry *ports;
    size_t port_count;
    size_t port_cap;
    uint32_t *port_free_slots;
    size_t port_free_count;
    size_t port_free_cap;
    size_t ports_pending;
    PendingSignal *pending_signals;
    size_t pending_count;
    size_t pending_cap;
    uint64_t deadline_hint;

    _Atomic bool main_terminated;
    bool main_abnormal;
    IdmValue main_value;
    IdmValue main_reason;

    size_t tty_waiting;
    uint64_t eval_pid;
    _Atomic bool eval_done;
    IdmValue eval_value;
    IdmValue eval_reason;
    uint64_t interrupt_pid;
    bool interrupt_struck;
    bool sigchld_pending;
    bool resize_pending;

    size_t nworkers;
    int wake_pipe[2];
    pthread_mutex_t mu;
    pthread_cond_t work_cv;
    size_t parked;
    bool poller_active;
    _Atomic bool shutdown;
    bool fatal;
    IdmError fatal_err;
};

static _Thread_local IdmScheduler *tls_worker_sched;
static _Thread_local size_t tls_worker_index;
static _Thread_local unsigned tls_sigcheck_slices;

static _Thread_local int tls_sched_lock_depth;

static void sched_lock(IdmScheduler *sched) { pthread_mutex_lock(&sched->mu); tls_sched_lock_depth++; }
static void sched_unlock(IdmScheduler *sched) { tls_sched_lock_depth--; pthread_mutex_unlock(&sched->mu); }

static void sched_poke_poller(IdmScheduler *sched) {
    if (sched->poller_active && sched->wake_pipe[1] >= 0) {
        ssize_t ignored = write(sched->wake_pipe[1], "w", 1u);
        (void)ignored;
    }
}

static void sched_wake(IdmScheduler *sched, bool all) {
    if (sched->nworkers <= 1u) return;
    assert(pthread_mutex_trylock(&sched->mu) == EBUSY);
    if (all) {
        pthread_cond_broadcast(&sched->work_cv);
        sched_poke_poller(sched);
        return;
    }
    if (sched->parked != 0) pthread_cond_signal(&sched->work_cv);
}

static int g_sig_pipe[2] = {-1, -1};
static bool g_sigchld_installed = false;
static bool g_interactive_signals_installed = false;

static void sig_handler(int signo) {
    char tag = signo == SIGWINCH ? 'r' : (signo == SIGCHLD ? 'c' : 'i');
    ssize_t ignored = write(g_sig_pipe[1], &tag, 1u);
    (void)ignored;
}

static bool sig_pipe_install(IdmError *err) {
    if (g_sig_pipe[0] >= 0) return true;
    int fds[2];
    if (pipe(fds) != 0) return idm_error_set(err, idm_span_unknown(NULL), "cannot create the signal pipe");
    for (int i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFL, O_NONBLOCK);
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    g_sig_pipe[0] = fds[0];
    g_sig_pipe[1] = fds[1];
    return true;
}

static bool signal_install(int signo, IdmError *err) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(signo, &sa, NULL) != 0) return idm_error_set(err, idm_span_unknown(NULL), "cannot install signal handler");
    return true;
}

static bool sigchld_install(IdmError *err) {
    if (!sig_pipe_install(err)) return false;
    if (g_sigchld_installed) return true;
    if (!signal_install(SIGCHLD, err)) return false;
    g_sigchld_installed = true;
    return true;
}

static bool idm_signals_install(IdmError *err) {
    if (!sigchld_install(err)) return false;
    if (g_interactive_signals_installed) return true;
    if (!signal_install(SIGINT, err)) return false;
    if (!signal_install(SIGWINCH, err)) return false;
    g_interactive_signals_installed = true;
    return true;
}

static void sig_drain_pending(IdmScheduler *sched) {
    char drain[64];
    ssize_t n;
    while (g_sig_pipe[0] >= 0 && (n = read(g_sig_pipe[0], drain, sizeof(drain))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (drain[i] == 'c') sched->sigchld_pending = true;
            else if (drain[i] == 'r') sched->resize_pending = true;
        }
    }
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t now_ms_ceil(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + ((uint64_t)ts.tv_nsec + 999999u) / 1000000u;
}

static bool reason_is_atom(IdmValue reason, const char *name) {
    return idm_value_tag(reason) == IDM_VAL_ATOM && strcmp(idm_symbol_text(idm_value_symbol(reason)), name) == 0;
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

static void gc_threshold_reset(IdmHeap *heap, size_t *threshold) {
    size_t live = idm_heap_bytes(heap);
    size_t doubled = live > SIZE_MAX / 2u ? SIZE_MAX : live * 2u;
    size_t floor = gc_threshold_from_env();
    *threshold = doubled > floor ? doubled : floor;
}

static void actor_gc_begin(IdmActor *actor) {
    pthread_mutex_lock(&actor->mbox_mu);
    idm_heap_gc_begin(&actor->heap);
    idm_gc_mark_value(&actor->heap, actor->exit_reason);
    idm_exec_root_cursor_init(&actor->gc_exec_roots);
    actor->gc_mailbox_index = 0;
    actor->gc_mailbox_limit = actor->mb_count;
    actor->gc_mailbox_base = actor->mb_head;
    actor->gc_mailbox_marking = true;
    pthread_mutex_unlock(&actor->mbox_mu);
    actor->gc_root_stage = GC_ROOT_EXEC;
}

static bool actor_mark_mailbox_root(IdmActor *actor, IdmValue *out) {
    pthread_mutex_lock(&actor->mbox_mu);
    *out = actor->mailbox[actor->gc_mailbox_base + actor->gc_mailbox_index];
    pthread_mutex_unlock(&actor->mbox_mu);
    actor->gc_mailbox_index++;
    return true;
}

static bool actor_mark_roots_step(IdmActor *actor, int64_t *budget) {
    IdmHeap *heap = &actor->heap;
    while (*budget > 0) {
        if (actor->gc_root_stage == GC_ROOT_EXEC) {
            if (actor->exec) {
                bool done = idm_exec_mark_roots_step(actor->exec, &actor->gc_exec_roots, budget, heap);
                if (!done) return false;
            }
            actor->gc_root_stage = GC_ROOT_MAILBOX;
            continue;
        }
        if (actor->gc_root_stage == GC_ROOT_MAILBOX) {
            while (*budget > 0 && actor->gc_mailbox_index < actor->gc_mailbox_limit) {
                IdmValue value = idm_nil();
                if (actor_mark_mailbox_root(actor, &value)) idm_gc_mark_value(heap, value);
                (*budget)--;
            }
            if (actor->gc_mailbox_index < actor->gc_mailbox_limit) return false;
            pthread_mutex_lock(&actor->mbox_mu);
            actor->gc_mailbox_marking = false;
            pthread_mutex_unlock(&actor->mbox_mu);
            actor->gc_root_stage = GC_ROOT_DONE;
            continue;
        }
        return true;
    }
    return actor->gc_root_stage == GC_ROOT_DONE;
}

static bool actor_gc_wants(IdmActor *actor) {
    IdmHeap *heap = &actor->heap;
    return idm_heap_gc_active(heap) || idm_heap_bytes(heap) > actor->gc_threshold;
}

static bool actor_gc_step(IdmActor *actor, int64_t *budget) {
    if (!actor || !budget || *budget <= 0) return true;
    IdmHeap *heap = &actor->heap;
    if (!idm_heap_gc_active(heap) && idm_heap_bytes(heap) <= actor->gc_threshold) return true;
    if (!idm_heap_gc_active(heap)) actor_gc_begin(actor);
    if (actor->gc_root_stage != GC_ROOT_DONE && !actor_mark_roots_step(actor, budget)) return false;
    if (*budget <= 0) return false;
    bool done = idm_heap_gc_step(heap, budget);
    if (done) {
        gc_threshold_reset(heap, &actor->gc_threshold);
        actor->gc_root_stage = GC_ROOT_DONE;
        return true;
    }
    return false;
}

IdmScheduler *idm_sched_create(IdmRuntime *rt, const IdmBytecodeModule *module, IdmError *err) {
    if (!sigchld_install(err)) return NULL;
    if (rt && rt->interactive && !idm_signals_install(err)) return NULL;
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
    sched->deadline_hint = UINT64_MAX;
    sched->main_value = idm_nil();
    sched->main_reason = idm_nil();
    sched->eval_value = idm_nil();
    sched->eval_reason = idm_nil();
    pthread_mutex_init(&sched->mu, NULL);
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
    rt->immortal.locking = sched->nworkers > 1u;
    sched->queue_count = sched->nworkers;
    sched->queue_locking = sched->nworkers > 1u;
    sched->queues = calloc(sched->queue_count, sizeof(*sched->queues));
    if (!sched->queues) {
        if (sched->wake_pipe[0] >= 0) close(sched->wake_pipe[0]);
        if (sched->wake_pipe[1] >= 0) close(sched->wake_pipe[1]);
        pthread_mutex_destroy(&sched->mu);
        pthread_cond_destroy(&sched->work_cv);
        free(sched);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    for (size_t i = 0; i < sched->queue_count; i++) pthread_mutex_init(&sched->queues[i].mu, NULL);
    return sched;
}

static PortEntry *sched_alloc_port_slot(IdmScheduler *sched, IdmError *err) {
    size_t slot;
    if (sched->port_free_count > 0) {
        slot = sched->port_free_slots[--sched->port_free_count];
    } else {
        if (sched->port_count == sched->port_cap) {
            if (!idm_grow((void **)&sched->ports, &sched->port_cap, sizeof(*sched->ports), 8u, sched->port_count + 1u)) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
        }
        slot = sched->port_count++;
        sched->ports[slot].gen = 0;
    }
    PortEntry *entry = &sched->ports[slot];
    entry->id = ((uint64_t)entry->gen << 32) | (uint64_t)(slot + 1u);
    return entry;
}

static bool sched_register_port(IdmScheduler *sched, IdmPort *port, uint64_t *out_id, IdmError *err) {
    PortEntry *entry = sched_alloc_port_slot(sched, err);
    if (!entry) return false;
    entry->live = true;
    entry->refs = 0;
    entry->out_readable = idm_port_stdout_readable(port);
    entry->err_readable = idm_port_stderr_readable(port);
    entry->eof_out = false;
    entry->eof_err = false;
    entry->port = port;
    entry->done = false;
    entry->is_process = idm_port_waits_completion(port);
    entry->pending_completion = entry->is_process;
    if (entry->pending_completion) sched->ports_pending++;
    entry->notified_stopped = false;
    entry->has_result = false;
    entry->exit_code = 0;
    entry->links = NULL;
    entry->link_count = 0;
    entry->link_cap = 0;
    entry->mon_in = NULL;
    entry->mon_in_count = 0;
    entry->mon_in_cap = 0;
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

static bool sched_link_locked(IdmScheduler *sched, IdmActor *self, IdmValue target_value, bool spawned, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err);
static bool sched_monitor_locked(IdmScheduler *sched, IdmActor *self, IdmValue target_value, bool spawned, IdmValue *out_ref, IdmError *err);
static void port_entry_release(PortEntry *entry);
static PortEntry *sched_find_port(IdmScheduler *sched, uint64_t id);

static void sched_reclaim_port_slot(IdmScheduler *sched, PortEntry *entry) {
    if (entry->pending_completion && sched->ports_pending != 0) {
        sched->ports_pending--;
        entry->pending_completion = false;
    }
    port_entry_release(entry);
    size_t slot = (size_t)(entry - sched->ports);
    entry->gen++;
    entry->live = false;
    entry->done = true;
    entry->has_result = false;
    entry->id = 0;
    if (sched->port_free_count == sched->port_free_cap) {
        if (!idm_grow((void **)&sched->port_free_slots, &sched->port_free_cap, sizeof(*sched->port_free_slots), 8u, sched->port_free_count + 1u)) return;
    }
    sched->port_free_slots[sched->port_free_count++] = (uint32_t)slot;
}

static void sched_unregister_port(IdmScheduler *sched, uint64_t port_id) {
    PortEntry *entry = sched_find_port(sched, port_id);
    if (entry) sched_reclaim_port_slot(sched, entry);
}

bool idm_sched_register_port_link(IdmScheduler *sched, IdmPort *port, IdmActor *self, IdmValue *out_port, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err) {
    sched_lock(sched);
    *self_should_exit = false;
    uint64_t port_id = 0;
    bool ok = sched_register_port(sched, port, &port_id, err);
    bool consumed = ok;
    if (ok) {
        IdmValue port_value = idm_port(port_id);
        *out_port = port_value;
        ok = sched_link_locked(sched, self, port_value, false, self_should_exit, self_exit_reason, err);
        if (!ok) sched_unregister_port(sched, port_id);
    }
    sched_unlock(sched);
    if (!ok && !consumed) idm_port_free(port);
    return ok;
}

bool idm_sched_register_port_monitor(IdmScheduler *sched, IdmPort *port, IdmActor *self, IdmValue *out_port, IdmValue *out_ref, IdmError *err) {
    sched_lock(sched);
    uint64_t port_id = 0;
    bool ok = sched_register_port(sched, port, &port_id, err);
    bool consumed = ok;
    if (ok) {
        IdmValue port_value = idm_port(port_id);
        *out_port = port_value;
        ok = sched_monitor_locked(sched, self, port_value, false, out_ref, err);
        if (!ok) sched_unregister_port(sched, port_id);
    }
    sched_unlock(sched);
    if (!ok && !consumed) idm_port_free(port);
    return ok;
}

static PortEntry *sched_find_port(IdmScheduler *sched, uint64_t id) {
    uint32_t low = (uint32_t)(id & 0xFFFFFFFFu);
    if (low == 0) return NULL;
    size_t slot = (size_t)low - 1u;
    if (slot >= sched->port_count) return NULL;
    PortEntry *entry = &sched->ports[slot];
    if (!entry->live || entry->gen != (uint32_t)(id >> 32)) return NULL;
    return entry;
}

static IdmPort *sched_port_pin(IdmScheduler *sched, uint64_t port_id) {
    PortEntry *e = sched_find_port(sched, port_id);
    if (!e || e->done || !e->port) return NULL;
    e->refs++;
    return e->port;
}

static void sched_port_unpin(IdmScheduler *sched, uint64_t port_id) {
    PortEntry *e = sched_find_port(sched, port_id);
    if (e) e->refs--;
}

static ProcId proc_actor(uint64_t pid) {
    ProcId proc = {PROC_ACTOR, pid};
    return proc;
}

static ProcId proc_port(uint64_t port_id) {
    ProcId proc = {PROC_PORT, port_id};
    return proc;
}

static bool proc_equal(ProcId a, ProcId b) {
    return a.kind == b.kind && a.id == b.id;
}

static bool value_to_proc(IdmValue value, ProcId *out) {
    if (idm_value_tag(value) == IDM_VAL_PID) {
        *out = proc_actor(idm_value_id(value));
        return true;
    }
    if (idm_value_tag(value) == IDM_VAL_PORT) {
        *out = proc_port(idm_value_id(value));
        return true;
    }
    return false;
}

static IdmValue proc_value(ProcId proc) {
    return proc.kind == PROC_PORT ? idm_port(proc.id) : idm_pid(proc.id);
}

static bool proc_is_process_target(IdmScheduler *sched, ProcId proc) {
    if (proc.kind == PROC_ACTOR) return true;
    PortEntry *entry = sched_find_port(sched, proc.id);
    return !entry || entry->is_process;
}

static bool value_to_process(IdmScheduler *sched, IdmValue value, ProcId *out) {
    if (!value_to_proc(value, out)) return false;
    return proc_is_process_target(sched, *out);
}

static bool port_entry_ensure_result(PortEntry *entry, IdmError *err) {
    (void)err;
    if (entry->has_result) return true;
    IDM_ASSERT_PROVED(entry->port);
    entry->exit_code = idm_port_exit_code(entry->port);
    entry->has_result = true;
    return true;
}

static IdmValue reason_from_exit_code(IdmScheduler *sched, IdmHeap *heap, int exit_code) {
    if (exit_code == 0) return idm_atom(sched->rt, "normal");
    IdmValue items[2] = { idm_atom(sched->rt, "exit-code"), idm_int((int64_t)exit_code) };
    IdmError err;
    idm_error_init(&err);
    IdmHeap *prev_heap = idm_swap_active_heap(heap);
    IdmValue reason = idm_tuple(sched->rt, items, 2u, &err);
    idm_swap_active_heap(prev_heap);
    if (err.present) {
        idm_error_clear(&err);
        reason = idm_atom(sched->rt, "oom");
    }
    return reason;
}

static IdmValue port_exit_reason(IdmScheduler *sched, IdmHeap *heap, PortEntry *entry) {
    return reason_from_exit_code(sched, heap, entry->exit_code);
}

static void port_entry_release(PortEntry *entry) {
    if (entry->port) {
        idm_port_free(entry->port);
        entry->port = NULL;
    }
    free(entry->links);
    entry->links = NULL;
    entry->link_count = 0;
    entry->link_cap = 0;
    free(entry->mon_in);
    entry->mon_in = NULL;
    entry->mon_in_count = 0;
    entry->mon_in_cap = 0;
}

static void actor_release_resources(IdmActor *actor) {
    pthread_mutex_lock(&actor->mbox_mu);
    actor->mbox_closed = true;
    free(actor->mailbox);
    actor->mailbox = NULL;
    actor->mb_head = 0;
    actor->mb_count = 0;
    actor->mb_cap = 0;
    actor->gc_mailbox_marking = false;
    pthread_mutex_unlock(&actor->mbox_mu);
    idm_heap_gc_cancel(&actor->heap);
    if (actor->exec) { idm_exec_destroy(actor->exec); actor->exec = NULL; }
    free(actor->tty_buf);
    actor->tty_buf = NULL;
    actor->tty_buf_len = 0;
    actor->tty_buf_cap = 0;
    actor->port_io_data = idm_nil();
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
    actor_release_resources(actor);
    pthread_mutex_destroy(&actor->mbox_mu);
    idm_heap_destroy(&actor->heap);
    free(actor);
}

void idm_sched_destroy(IdmScheduler *sched) {
    if (!sched) return;
    if (sched->rt && sched->rt->repl && idm_repl_scheduler(sched->rt->repl) == sched) idm_repl_destroy(sched->rt->repl);
    if (sched->wake_pipe[0] >= 0) close(sched->wake_pipe[0]);
    if (sched->wake_pipe[1] >= 0) close(sched->wake_pipe[1]);
    pthread_mutex_destroy(&sched->mu);
    pthread_cond_destroy(&sched->work_cv);
    idm_error_clear(&sched->fatal_err);
    for (size_t i = 0; i < sched->actor_count; i++) actor_free(sched->actors[i]);
    for (size_t i = 0; i < sched->port_count; i++) port_entry_release(&sched->ports[i]);
    free(sched->ports);
    free(sched->port_free_slots);
    free(sched->pending_signals);
    free(sched->actors);
    free(sched->slot_gen);
    free(sched->free_slots);
    free(sched->reap_slots);
    for (size_t i = 0; i < sched->queue_count; i++) {
        pthread_mutex_destroy(&sched->queues[i].mu);
        free(sched->queues[i].items);
    }
    free(sched->queues);
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
    (void)sched;
    return actor->exited && !actor->reap_queued;
}

static void reap_queue_push(IdmScheduler *sched, IdmActor *actor) {
    if (actor->reap_queued) return;
    if (sched->reap_count == sched->reap_cap) {
        if (!idm_grow((void **)&sched->reap_slots, &sched->reap_cap, sizeof(*sched->reap_slots), 16u, sched->reap_count + 1u)) return;
    }
    sched->reap_slots[sched->reap_count++] = (uint32_t)((actor->pid & 0xFFFFFFFFu) - 1u);
    actor->reap_queued = true;
}

static IdmActor **drain_reap(IdmScheduler *sched, size_t *out_count) {
    *out_count = 0;
    if (sched->reap_count == 0) return NULL;
    IdmActor **dead = calloc(sched->reap_count, sizeof(*dead));
    if (!dead) return NULL;
    for (size_t i = 0; i < sched->reap_count; i++) {
        uint32_t slot = sched->reap_slots[i];
        IdmActor *a = sched->actors[slot];
        if (!a) continue;
        sched->actors[slot] = NULL;
        sched->slot_gen[slot]++;
        a->detached = true;
        if (a->refs == 0) {
            dead[*out_count] = a;
            (*out_count)++;
        }
        if (sched->free_count == sched->free_cap) {
            if (!idm_grow((void **)&sched->free_slots, &sched->free_cap, sizeof(*sched->free_slots), 16u, sched->free_count + 1u)) continue;
        }
        sched->free_slots[sched->free_count++] = slot;
    }
    sched->reap_count = 0;
    return dead;
}

static void reap_free_detached(IdmActor **dead, size_t count) {
    for (size_t i = 0; i < count; i++) actor_free(dead[i]);
    free(dead);
}

static void runq_lock(IdmScheduler *sched, RunQueue *q) {
    if (sched->queue_locking) pthread_mutex_lock(&q->mu);
}

static void runq_unlock(IdmScheduler *sched, RunQueue *q) {
    if (sched->queue_locking) pthread_mutex_unlock(&q->mu);
}

static bool runq_push(IdmScheduler *sched, RunQueue *q, uint64_t pid, IdmError *err) {
    runq_lock(sched, q);
    if (q->head + q->count == q->cap) {
        if (q->head > 0) {
            memmove(q->items, q->items + q->head, q->count * sizeof(*q->items));
            q->head = 0;
        } else {
            if (!idm_grow((void **)&q->items, &q->cap, sizeof(*q->items), 16u, q->count + 1u)) {
                runq_unlock(sched, q);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
        }
    }
    q->items[q->head + q->count] = pid;
    q->count++;
    atomic_fetch_add(&sched->ready_total, 1u);
    runq_unlock(sched, q);
    return true;
}

static bool runq_pop(IdmScheduler *sched, RunQueue *q, uint64_t *out) {
    runq_lock(sched, q);
    if (q->count == 0) {
        runq_unlock(sched, q);
        return false;
    }
    *out = q->items[q->head];
    q->head++;
    q->count--;
    if (q->count == 0) q->head = 0;
    atomic_fetch_sub(&sched->ready_total, 1u);
    runq_unlock(sched, q);
    return true;
}

static size_t ready_home(IdmScheduler *sched) {
    if (sched->queue_count == 1u) return 0;
    if (tls_worker_sched == sched && tls_worker_index < sched->queue_count) return tls_worker_index;
    return atomic_fetch_add(&sched->place_next, 1u) % sched->queue_count;
}

static bool ready_push(IdmScheduler *sched, uint64_t pid, IdmError *err) {
    if (!runq_push(sched, &sched->queues[ready_home(sched)], pid, err)) return false;
    sched_wake(sched, false);
    return true;
}

static bool ready_take(IdmScheduler *sched, size_t w, uint64_t *out) {
    for (size_t i = 0; i < sched->queue_count; i++) {
        RunQueue *q = &sched->queues[(w + i) % sched->queue_count];
        if (atomic_load_explicit(&q->count, memory_order_relaxed) == 0) continue;
        if (runq_pop(sched, q, out)) return true;
    }
    return false;
}

static bool ready_none(IdmScheduler *sched) {
    if (atomic_load(&sched->ready_total) != 0) return false;
    for (size_t i = 0; i < sched->queue_count; i++) {
        RunQueue *q = &sched->queues[i];
        runq_lock(sched, q);
        size_t count = q->count;
        runq_unlock(sched, q);
        if (count != 0) return false;
    }
    return true;
}

static void actor_wait_deadline_clear(IdmActor *actor) {
    actor->wait_has_deadline = false;
    actor->wait_deadline_ms = 0;
}

static void actor_wait_deadline_set(IdmActor *actor, uint64_t deadline_ms) {
    actor->wait_has_deadline = true;
    actor->wait_deadline_ms = deadline_ms;
}

static bool actor_resume_with(IdmScheduler *sched, IdmActor *actor, IdmValue result, IdmError *err) {
    if (idm_value_heap(result) != &actor->heap) {
        result = idm_value_copy(sched->rt, &actor->heap, result, err);
        if (err->present) return false;
    }
    if (!idm_exec_push_result(actor->exec, result, err)) return false;
    if (!ready_push(sched, actor->pid, err)) return false;
    actor->status = ACT_READY;
    return true;
}

static bool copy_to_runtime(IdmScheduler *sched, IdmValue value, IdmValue *out, IdmError *err) {
    IdmHeap *heap = idm_value_heap(value);
    if (heap && heap != &sched->rt->immortal) {
        value = idm_value_copy(sched->rt, &sched->rt->immortal, value, err);
        if (err && err->present) return false;
    }
    *out = value;
    return true;
}

static IdmValue copy_to_runtime_or_oom(IdmScheduler *sched, IdmValue value) {
    IdmError err;
    idm_error_init(&err);
    IdmValue out = idm_nil();
    if (!copy_to_runtime(sched, value, &out, &err)) {
        idm_error_clear(&err);
        return idm_atom(sched->rt, "oom");
    }
    return out;
}

static IdmActor *actor_create(IdmScheduler *sched, IdmError *err) {
    IdmActor *actor = calloc(1u, sizeof(*actor));
    if (!actor) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    idm_heap_init(&actor->heap);
    actor->heap.locking = sched->nworkers > 1u;
    actor->rt = sched->rt;
    actor->gc_threshold = gc_threshold_from_env();
    pthread_mutex_init(&actor->mbox_mu, NULL);
    size_t slot;
    if (sched->free_count > 0) {
        slot = sched->free_slots[--sched->free_count];
    } else {
        if (sched->actor_count == sched->actor_cap) {
            IdmGrowItem items[] = {
                { .base = (void **)&sched->actors, .elem_size = sizeof(*sched->actors) },
                { .base = (void **)&sched->slot_gen, .elem_size = sizeof(*sched->slot_gen) },
            };
            if (!idm_growv(items, 2u, &sched->actor_cap, 8u, sched->actor_count + 1u)) {
                pthread_mutex_destroy(&actor->mbox_mu);
                idm_heap_destroy(&actor->heap);
                free(actor);
                return (IdmActor *)(uintptr_t)idm_error_oom(err, idm_span_unknown(NULL));
            }
        }
        slot = sched->actor_count++;
        sched->slot_gen[slot] = 0;
    }
    actor->pid = ((uint64_t)sched->slot_gen[slot] << 32) | (uint64_t)(slot + 1u);
    actor->status = ACT_READY;
    actor->exit_reason = idm_nil();
    actor->gc_root_stage = GC_ROOT_DONE;
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

static bool mailbox_push_unlocked(IdmActor *actor, IdmValue msg, IdmError *err) {
    if (actor->mb_head + actor->mb_count == actor->mb_cap) {
        if (actor->mb_head > 0 && !actor->gc_mailbox_marking) {
            memmove(actor->mailbox, actor->mailbox + actor->mb_head, actor->mb_count * sizeof(*actor->mailbox));
            actor->mb_head = 0;
        } else if (!idm_grow((void **)&actor->mailbox, &actor->mb_cap, sizeof(*actor->mailbox), 8u, actor->mb_head + actor->mb_count + 1u)) {
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    actor->mailbox[actor->mb_head + actor->mb_count] = msg;
    actor->mb_count++;
    return true;
}

static void terminate(IdmScheduler *sched, IdmActor *actor, IdmValue reason);

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

static void sched_release_actor(IdmScheduler *sched, IdmActor *actor) {
    (void)sched;
    if (--actor->refs == 0 && actor->detached) actor_free(actor);
}

void idm_sched_send(IdmScheduler *sched, uint64_t target_pid, IdmValue msg) {
    sched_lock(sched);
    IdmActor *actor = sched_lookup(sched, target_pid);
    if (!actor || actor->exited) {
        sched_unlock(sched);
        return;
    }
    actor->refs++;
    sched_unlock(sched);

    IdmError err;
    idm_error_init(&err);
    bool delivered = false;
    bool oom = false;
    pthread_mutex_lock(&actor->mbox_mu);
    if (!actor->mbox_closed) {
        pthread_mutex_lock(&actor->heap.lock);
        IdmValue copied = idm_value_copy_locked(actor->rt, &actor->heap, msg, &err);
        if (!err.present) delivered = mailbox_push_unlocked(actor, copied, &err);
        pthread_mutex_unlock(&actor->heap.lock);
        oom = err.present;
    }
    pthread_mutex_unlock(&actor->mbox_mu);
    idm_error_clear(&err);

    sched_lock(sched);
    if (delivered) wake_for_message(sched, actor);
    else if (oom && !actor->exited) terminate(sched, actor, idm_atom(sched->rt, "oom"));
    sched_release_actor(sched, actor);
    sched_unlock(sched);
}

static IdmValue make_signal(IdmScheduler *sched, IdmHeap *heap, const char *tag, IdmValue a, IdmValue b, IdmValue c, size_t count, IdmError *err) {
    IdmValue items[4];
    items[0] = idm_atom(sched->rt, tag);
    items[1] = a;
    items[2] = b;
    items[3] = c;
    IdmHeap *prev_heap = idm_swap_active_heap(heap);
    IdmValue tuple = idm_tuple(sched->rt, items, count, err);
    idm_swap_active_heap(prev_heap);
    return tuple;
}

static bool post_signal(IdmScheduler *sched, IdmActor *target, IdmActor *source, const char *tag, IdmValue a, IdmValue b, IdmValue c, size_t count, IdmError *err) {
    if (!target || target->exited) return true;
    target->refs++;
    if (source) source->refs++;
    sched_unlock(sched);

    bool delivered = false;
    bool oom = false;
    pthread_mutex_lock(&target->mbox_mu);
    if (!target->mbox_closed) {
        assert(tls_sched_lock_depth == 0);
        IdmValue ca = idm_value_copy(sched->rt, &target->heap, a, err);
        IdmValue cb = err->present ? idm_nil() : idm_value_copy(sched->rt, &target->heap, b, err);
        IdmValue cc = err->present ? idm_nil() : idm_value_copy(sched->rt, &target->heap, c, err);
        IdmValue signal = err->present ? idm_nil() : make_signal(sched, &target->heap, tag, ca, cb, cc, count, err);
        if (!err->present) delivered = mailbox_push_unlocked(target, signal, err);
        oom = err->present;
    }
    pthread_mutex_unlock(&target->mbox_mu);

    sched_lock(sched);
    if (delivered) wake_for_message(sched, target);
    else if (oom && !target->exited) terminate(sched, target, idm_atom(sched->rt, "oom"));
    if (source) sched_release_actor(sched, source);
    sched_release_actor(sched, target);
    return !oom;
}

static PendingSignal *sched_signal_enqueue(IdmScheduler *sched, IdmActor *target, IdmActor *source, const char *tag, IdmValue a, IdmValue b, IdmValue c, size_t count) {
    if (!target || target->exited) return NULL;
    if (sched->pending_count == sched->pending_cap
        && !idm_grow((void **)&sched->pending_signals, &sched->pending_cap, sizeof(*sched->pending_signals), 8u, sched->pending_count + 1u)) {
        terminate(sched, target, idm_atom(sched->rt, "oom"));
        return NULL;
    }
    target->refs++;
    if (source) source->refs++;
    PendingSignal *ps = &sched->pending_signals[sched->pending_count++];
    ps->target = target;
    ps->source = source;
    ps->tag = tag;
    ps->a = a;
    ps->b = b;
    ps->c = c;
    ps->count = count;
    ps->port_reason = false;
    ps->terminate = false;
    ps->port_exit_code = 0;
    return ps;
}

static void sched_drain_signals(IdmScheduler *sched) {
    for (;;) {
        sched_lock(sched);
        size_t n = sched->pending_count;
        if (n == 0) {
            sched_unlock(sched);
            return;
        }
        PendingSignal *batch = sched->pending_signals;
        sched->pending_signals = NULL;
        sched->pending_count = 0;
        sched->pending_cap = 0;
        sched_unlock(sched);
        for (size_t i = 0; i < n; i++) {
            IdmActor *target = batch[i].target;
            IdmError err;
            idm_error_init(&err);
            bool delivered = false;
            bool do_terminate = false;
            IdmValue term_reason = idm_nil();
            pthread_mutex_lock(&target->mbox_mu);
            if (batch[i].terminate) {
                if (!target->exited) {
                    assert(tls_sched_lock_depth == 0);
                    term_reason = idm_value_copy(sched->rt, &target->heap, batch[i].a, &err);
                    do_terminate = !err.present;
                }
            } else if (!target->mbox_closed) {
                assert(tls_sched_lock_depth == 0);
                IdmValue a = idm_value_copy(sched->rt, &target->heap, batch[i].a, &err);
                IdmValue b = err.present ? idm_nil() : idm_value_copy(sched->rt, &target->heap, batch[i].b, &err);
                IdmValue c = err.present ? idm_nil() : idm_value_copy(sched->rt, &target->heap, batch[i].c, &err);
                if (!err.present && batch[i].port_reason) {
                    IdmValue r = reason_from_exit_code(sched, &target->heap, batch[i].port_exit_code);
                    if (batch[i].count == 4) c = r; else b = r;
                }
                IdmValue signal = err.present ? idm_nil() : make_signal(sched, &target->heap, batch[i].tag, a, b, c, batch[i].count, &err);
                if (!err.present) delivered = mailbox_push_unlocked(target, signal, &err);
            }
            pthread_mutex_unlock(&target->mbox_mu);
            bool oom = err.present;
            idm_error_clear(&err);
            sched_lock(sched);
            if (delivered) wake_for_message(sched, target);
            else if (do_terminate && !target->exited) terminate(sched, target, term_reason);
            else if (oom && !target->exited) terminate(sched, target, idm_atom(sched->rt, "oom"));
            sched_release_actor(sched, target);
            if (batch[i].source) sched_release_actor(sched, batch[i].source);
            sched_unlock(sched);
        }
        free(batch);
    }
}

static bool proc_swap_remove(ProcId *items, size_t *count, ProcId value) {
    for (size_t i = 0; i < *count; i++) {
        if (!proc_equal(items[i], value)) continue;
        items[i] = items[*count - 1u];
        (*count)--;
        return true;
    }
    return false;
}

static void actor_links_remove(IdmActor *actor, ProcId peer) {
    proc_swap_remove(actor->links, &actor->link_count, peer);
}

static void port_links_remove(PortEntry *port, ProcId peer) {
    proc_swap_remove(port->links, &port->link_count, peer);
}

static bool proc_links_add(ProcId **items, size_t *count, size_t *cap, ProcId peer, IdmError *err) {
    for (size_t i = 0; i < *count; i++) {
        if (proc_equal((*items)[i], peer)) return true;
    }
    if (*count == *cap) {
        if (!idm_grow((void **)items, cap, sizeof(**items), 4u, *count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    (*items)[(*count)++] = peer;
    return true;
}

static bool sched_spawn_locked(IdmScheduler *sched, IdmValue thunk, const IdmExec *parent, IdmValue *out_pid, IdmError *err);

static bool sched_abnormal_error(IdmScheduler *sched, IdmValue reason, IdmError *err) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    idm_buf_append(&buf, "main actor exited with reason ");
    idm_error_describe(sched->rt, reason, &buf);
    if (buf.data) {
        idm_error_set(err, idm_span_unknown(NULL), "%s", buf.data);
    } else {
        idm_error_set(err, idm_span_unknown(NULL), "main actor exited abnormally");
    }
    if (!err->reason) err->reason = malloc(sizeof(IdmValue));
    if (err->reason) *(IdmValue *)err->reason = reason;
    idm_buf_destroy(&buf);
    return false;
}

static const char *actor_status_name(ActorStatus status) {
    switch (status) {
        case ACT_READY: return "ready";
        case ACT_RUNNING: return "running";
        case ACT_WAITING_RECEIVE: return "waiting-receive";
        case ACT_WAITING_PORT_READ: return "waiting-port-read";
        case ACT_WAITING_PORT_WRITE: return "waiting-port-write";
        case ACT_WAITING_TTY: return "waiting-tty";
        case ACT_EXITED: return "exited";
    }
    return "unknown";
}

static IdmValue proc_id_value(ProcId proc) {
    return proc.kind == PROC_ACTOR ? idm_pid(proc.id) : idm_port(proc.id);
}

static IdmSpan actor_recv_span(const IdmActor *actor) {
    if (actor->status != ACT_WAITING_RECEIVE || !actor->recv_ctx_module) return idm_span_unknown(NULL);
    return idm_bc_span_at((const IdmBytecodeModule *)actor->recv_ctx_module, actor->recv_ctx_site);
}

bool idm_sched_processes_value(IdmScheduler *sched, IdmRuntime *rt, IdmValue *out, IdmError *err) {
    (void)rt;
    sched_lock(sched);
    size_t actor_total = 0;
    size_t port_total = 0;
    for (size_t i = 0; i < sched->actor_count; i++) {
        if (sched->actors[i] && !sched->actors[i]->exited) actor_total++;
    }
    for (size_t i = 0; i < sched->port_count; i++) {
        if (!sched->ports[i].done) port_total++;
    }
    uint64_t *ids = actor_total + port_total ? malloc((actor_total + port_total) * sizeof(*ids)) : NULL;
    bool *is_port = actor_total + port_total ? malloc((actor_total + port_total) * sizeof(*is_port)) : NULL;
    size_t count = 0;
    if ((ids && is_port) || actor_total + port_total == 0) {
        for (size_t i = 0; i < sched->actor_count; i++) {
            if (sched->actors[i] && !sched->actors[i]->exited) {
                ids[count] = sched->actors[i]->pid;
                is_port[count++] = false;
            }
        }
        for (size_t i = 0; i < sched->port_count; i++) {
            if (!sched->ports[i].done) {
                ids[count] = sched->ports[i].id;
                is_port[count++] = true;
            }
        }
    }
    sched_unlock(sched);
    if ((actor_total + port_total) != 0 && (!ids || !is_port)) {
        free(ids);
        free(is_port);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    IdmValue acc = idm_empty_list();
    for (size_t i = count; i > 0; i--) {
        acc = idm_cons(rt, is_port[i - 1u] ? idm_port(ids[i - 1u]) : idm_pid(ids[i - 1u]), acc, err);
        if (err && err->present) break;
    }
    free(ids);
    free(is_port);
    if (err && err->present) return false;
    *out = acc;
    return true;
}

bool idm_sched_process_info_value(IdmScheduler *sched, IdmRuntime *rt, uint64_t pid, IdmValue *out, IdmError *err) {
    sched_lock(sched);
    IdmActor *actor = sched_lookup(sched, pid);
    if (!actor || actor->exited) {
        sched_unlock(sched);
        *out = idm_nil();
        return true;
    }
    ActorStatus status = actor->status;
    IdmSpan recv_span = actor_recv_span(actor);
    bool trap = atomic_load(&actor->trap_exit);
    size_t link_count = actor->link_count;
    ProcId *links = link_count ? malloc(link_count * sizeof(*links)) : NULL;
    if (links) memcpy(links, actor->links, link_count * sizeof(*links));
    size_t mon_out = actor->mon_out_count;
    size_t mon_in = actor->mon_in_count;
    bool has_deadline = actor->wait_has_deadline;
    uint64_t deadline = actor->wait_deadline_ms;
    actor->refs++;
    sched_unlock(sched);
    pthread_mutex_lock(&actor->mbox_mu);
    size_t mailbox = actor->mb_count;
    IdmValue head = idm_nil();
    bool head_ok = true;
    if (mailbox != 0) {
        head = idm_value_copy(rt, idm_active_heap(rt), actor->mailbox[actor->mb_head], err);
        head_ok = !(err && err->present);
    }
    pthread_mutex_unlock(&actor->mbox_mu);
    sched_lock(sched);
    sched_release_actor(sched, actor);
    sched_unlock(sched);
    if (link_count != 0 && !links) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!head_ok) {
        free(links);
        return false;
    }
    IdmValue link_list = idm_empty_list();
    for (size_t i = link_count; i > 0; i--) {
        link_list = idm_cons(rt, proc_id_value(links[i - 1u]), link_list, err);
        if (err && err->present) {
            free(links);
            return false;
        }
    }
    free(links);
    IdmDictEntry items[11];
    size_t count = 0;
    items[count].key = idm_atom(rt, "pid");
    items[count++].value = idm_pid(pid);
    items[count].key = idm_atom(rt, "status");
    items[count++].value = idm_atom(rt, actor_status_name(status));
    items[count].key = idm_atom(rt, "trap-exit");
    items[count++].value = idm_bool(rt, trap);
    items[count].key = idm_atom(rt, "mailbox");
    items[count++].value = idm_int((int64_t)mailbox);
    if (mailbox != 0) {
        items[count].key = idm_atom(rt, "mailbox-head");
        items[count++].value = head;
    }
    items[count].key = idm_atom(rt, "links");
    items[count++].value = link_list;
    items[count].key = idm_atom(rt, "monitors");
    items[count++].value = idm_int((int64_t)mon_out);
    items[count].key = idm_atom(rt, "monitored-by");
    items[count++].value = idm_int((int64_t)mon_in);
    if (status == ACT_WAITING_RECEIVE && has_deadline) {
        items[count].key = idm_atom(rt, "deadline-ms");
        items[count++].value = idm_int((int64_t)deadline);
    }
    if (recv_span.line != 0 && recv_span.file) {
        IdmDictEntry at[2];
        at[0].key = idm_atom(rt, "file");
        at[0].value = idm_string(rt, recv_span.file, err);
        if (err && err->present) return false;
        at[1].key = idm_atom(rt, "line");
        at[1].value = idm_int((int64_t)recv_span.line);
        items[count].key = idm_atom(rt, "waiting-at");
        items[count++].value = idm_dict(rt, at, 2u, err);
        if (err && err->present) return false;
    }
    *out = idm_dict(rt, items, count, err);
    return !(err && err->present);
}

bool idm_sched_port_info_value(IdmScheduler *sched, IdmRuntime *rt, uint64_t port_id, IdmValue *out, IdmError *err) {
    sched_lock(sched);
    PortEntry *entry = sched_find_port(sched, port_id);
    if (!entry) {
        sched_unlock(sched);
        *out = idm_nil();
        return true;
    }
    IDM_ASSERT_PROVED(entry->port);
    bool done = entry->done;
    bool has_result = entry->has_result;
    int exit_code = entry->exit_code;
    size_t link_count = entry->link_count;
    size_t mon_in = entry->mon_in_count;
    IdmPort *port = entry->port;
    entry->refs++;
    sched_unlock(sched);
    IdmValue detail = idm_nil();
    bool detail_ok = idm_port_info_value(rt, port, &detail, err);
    sched_lock(sched);
    sched_port_unpin(sched, port_id);
    sched_unlock(sched);
    if (!detail_ok) return false;
    IdmDictEntry items[6];
    size_t count = 0;
    items[count].key = idm_atom(rt, "port");
    items[count++].value = idm_port(port_id);
    items[count].key = idm_atom(rt, "done");
    items[count++].value = idm_bool(rt, done);
    if (has_result) {
        items[count].key = idm_atom(rt, "exit-code");
        items[count++].value = idm_int((int64_t)exit_code);
    }
    items[count].key = idm_atom(rt, "links");
    items[count++].value = idm_int((int64_t)link_count);
    items[count].key = idm_atom(rt, "monitored-by");
    items[count++].value = idm_int((int64_t)mon_in);
    if (!idm_is_nil(detail)) {
        items[count].key = idm_atom(rt, "command");
        items[count++].value = detail;
    }
    *out = idm_dict(rt, items, count, err);
    return !(err && err->present);
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
        idm_error_set(err, idm_span_unknown(NULL), "deadlock: all live actors are blocked with no way to make progress");
        size_t shown = 0;
        size_t skipped = 0;
        for (size_t i = 0; i < sched->actor_count; i++) {
            IdmActor *a = sched->actors[i];
            if (!a || a->exited) continue;
            if (shown == 12u) {
                skipped++;
                continue;
            }
            IdmSpan where = actor_recv_span(a);
            pthread_mutex_lock(&a->mbox_mu);
            size_t depth = a->mb_count;
            pthread_mutex_unlock(&a->mbox_mu);
            if (a->status == ACT_WAITING_RECEIVE) {
                const char *wait = a->wait_has_deadline ? "deadline pending" : "no timeout";
                if (where.line != 0 && where.file) {
                    idm_error_note_at(err, where, "process %llu %s (mailbox %zu, %s)", (unsigned long long)a->pid, actor_status_name(a->status), depth, wait);
                } else {
                    idm_error_note(err, "process %llu %s (mailbox %zu, %s)", (unsigned long long)a->pid, actor_status_name(a->status), depth, wait);
                }
            } else {
                idm_error_note(err, "process %llu %s (mailbox %zu)", (unsigned long long)a->pid, actor_status_name(a->status), depth);
            }
            shown++;
        }
        if (skipped != 0) idm_error_note(err, "... (%zu more blocked process(es))", skipped);
        return false;
    }
    idm_error_set(err, idm_span_unknown(NULL), "%s; the remaining actor(s) are blocked with no way to make progress",
                  buf.data ? buf.data : "a peer crashed");
    idm_buf_destroy(&buf);
    if (!err->reason) err->reason = malloc(sizeof(IdmValue));
    if (err->reason) *(IdmValue *)err->reason = first_reason;
    return false;
}

static bool mon_add(Monitor **items, size_t *count, size_t *cap, uint64_t ref, ProcId peer, IdmError *err);
static bool mon_remove_by_ref(Monitor *items, size_t *count, uint64_t ref, ProcId *out_peer);

static void proc_links_remove(IdmScheduler *sched, ProcId proc, ProcId peer) {
    if (proc.kind == PROC_ACTOR) {
        IdmActor *actor = sched_lookup(sched, proc.id);
        if (actor && !actor->exited) actor_links_remove(actor, peer);
        return;
    }
    PortEntry *port = sched_find_port(sched, proc.id);
    if (port) port_links_remove(port, peer);
}

static bool proc_links_add_to(IdmScheduler *sched, ProcId proc, ProcId peer, IdmError *err) {
    if (proc.kind == PROC_ACTOR) {
        IdmActor *actor = sched_lookup(sched, proc.id);
        if (!actor || actor->exited) return false;
        return proc_links_add(&actor->links, &actor->link_count, &actor->link_cap, peer, err);
    }
    PortEntry *port = sched_find_port(sched, proc.id);
    if (!port) return false;
    return proc_links_add(&port->links, &port->link_count, &port->link_cap, peer, err);
}

static void proc_mon_in_remove(IdmScheduler *sched, ProcId proc, uint64_t ref) {
    if (proc.kind == PROC_ACTOR) {
        IdmActor *actor = sched_lookup(sched, proc.id);
        if (actor) mon_remove_by_ref(actor->mon_in, &actor->mon_in_count, ref, NULL);
        return;
    }
    PortEntry *port = sched_find_port(sched, proc.id);
    if (port) mon_remove_by_ref(port->mon_in, &port->mon_in_count, ref, NULL);
}

static bool proc_mon_in_add(IdmScheduler *sched, ProcId proc, uint64_t ref, ProcId watcher, IdmError *err) {
    if (proc.kind == PROC_ACTOR) {
        IdmActor *actor = sched_lookup(sched, proc.id);
        if (!actor || actor->exited) return false;
        return mon_add(&actor->mon_in, &actor->mon_in_count, &actor->mon_in_cap, ref, watcher, err);
    }
    PortEntry *port = sched_find_port(sched, proc.id);
    if (!port) return false;
    return mon_add(&port->mon_in, &port->mon_in_count, &port->mon_in_cap, ref, watcher, err);
}

static void apply_exit_signal(IdmScheduler *sched, IdmActor *target, ProcId from, IdmValue reason) {
    if (target->trap_exit) {
        IdmActor *src = from.kind == PROC_ACTOR ? sched_lookup(sched, from.id) : NULL;
        IdmError err;
        idm_error_init(&err);
        post_signal(sched, target, src, "exit", proc_value(from), reason, idm_nil(), 3, &err);
        idm_error_clear(&err);
        return;
    }
    if (reason_is_normal(reason)) return;
    bool boxed = reason.bits != 0u && (reason.bits & 1u) == 0u && !idm_is_immediate(reason);
    if (!boxed) {
        terminate(sched, target, reason);
        return;
    }
    target->refs++;
    sched_unlock(sched);
    IdmError err;
    idm_error_init(&err);
    pthread_mutex_lock(&target->mbox_mu);
    bool exited = target->exited;
    IdmValue copied = idm_nil();
    if (!exited) {
        assert(tls_sched_lock_depth == 0);
        copied = idm_value_copy(sched->rt, &target->heap, reason, &err);
    }
    pthread_mutex_unlock(&target->mbox_mu);
    bool oom = err.present;
    idm_error_clear(&err);
    sched_lock(sched);
    if (!target->exited) terminate(sched, target, oom ? idm_atom(sched->rt, "oom") : copied);
    sched_release_actor(sched, target);
}

static void apply_port_exit_signal(PortEntry *port, IdmValue reason) {
    if (!port || !port->port || reason_is_normal(reason)) return;
    int signo = reason_is_atom(reason, "kill") ? SIGKILL : SIGTERM;
    idm_port_signal_group(port->port, signo);
}

static void proc_apply_exit_signal(IdmScheduler *sched, ProcId target, ProcId from, IdmValue reason) {
    if (target.kind == PROC_ACTOR) {
        IdmActor *actor = sched_lookup(sched, target.id);
        if (actor && !actor->exited) apply_exit_signal(sched, actor, from, reason);
        return;
    }
    apply_port_exit_signal(sched_find_port(sched, target.id), reason);
}

static bool mon_add(Monitor **items, size_t *count, size_t *cap, uint64_t ref, ProcId peer, IdmError *err) {
    if (*count == *cap) {
        if (!idm_grow((void **)items, cap, sizeof(**items), 4u, *count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    (*items)[*count].ref = ref;
    (*items)[*count].peer = peer;
    (*count)++;
    return true;
}

static bool mon_remove_by_ref(Monitor *items, size_t *count, uint64_t ref, ProcId *out_peer) {
    for (size_t i = 0; i < *count; i++) {
        if (items[i].ref != ref) continue;
        if (out_peer) *out_peer = items[i].peer;
        items[i] = items[*count - 1u];
        (*count)--;
        return true;
    }
    return false;
}

static void terminate(IdmScheduler *sched, IdmActor *actor, IdmValue reason) {
    if (actor->exited) return;
    IdmHeap *prev_heap = idm_swap_active_heap(&actor->heap);
    bool running = actor->status == ACT_RUNNING;
    bool finished = false;
    if (actor->status == ACT_WAITING_TTY) sched->tty_waiting--;
    actor->exited = true;
    actor->status = ACT_EXITED;
    pthread_mutex_lock(&actor->mbox_mu);
    idm_gc_write_barrier(actor->exit_reason);
    actor->exit_reason = reason;
    pthread_mutex_unlock(&actor->mbox_mu);
    IdmValue stored = reason;

    for (size_t i = 0; i < actor->mon_out_count; i++) {
        proc_mon_in_remove(sched, actor->mon_out[i].peer, actor->mon_out[i].ref);
    }
    actor->mon_out_count = 0;

    Monitor *mons = actor->mon_in;
    size_t mon_count = actor->mon_in_count;
    actor->mon_in = NULL;
    actor->mon_in_count = 0;
    actor->mon_in_cap = 0;
    for (size_t i = 0; i < mon_count; i++) {
        if (mons[i].peer.kind != PROC_ACTOR) continue;
        IdmActor *watcher = sched_lookup(sched, mons[i].peer.id);
        if (!watcher || watcher->exited) continue;
        mon_remove_by_ref(watcher->mon_out, &watcher->mon_out_count, mons[i].ref, NULL);
        sched_signal_enqueue(sched, watcher, actor, "down", idm_ref(mons[i].ref), proc_value(proc_actor(actor->pid)), stored, 4);
    }
    free(mons);

    size_t link_count = actor->link_count;
    ProcId *links = NULL;
    if (link_count > 0) {
        links = malloc(link_count * sizeof(*links));
        if (links) memcpy(links, actor->links, link_count * sizeof(*links));
    }
    for (size_t i = 0; links && i < link_count; i++) {
        proc_links_remove(sched, links[i], proc_actor(actor->pid));
        if (links[i].kind == PROC_ACTOR) {
            IdmActor *peer = sched_lookup(sched, links[i].id);
            if (peer && !peer->exited) {
                if (peer->trap_exit) {
                    sched_signal_enqueue(sched, peer, actor, "exit", proc_value(proc_actor(actor->pid)), stored, idm_nil(), 3);
                } else if (!reason_is_normal(stored)) {
                    PendingSignal *ps = sched_signal_enqueue(sched, peer, actor, "exit", stored, idm_nil(), idm_nil(), 1);
                    if (ps) ps->terminate = true;
                }
            }
        } else {
            apply_port_exit_signal(sched_find_port(sched, links[i].id), stored);
        }
    }
    free(links);

    if (actor->pid == sched->main_pid && !sched->main_terminated) {
        sched->main_terminated = true;
        sched->main_value = idm_nil();
        sched->main_reason = copy_to_runtime_or_oom(sched, stored);
        sched->main_abnormal = !reason_is_normal(stored);
        finished = true;
    }
    if (actor->pid == sched->interrupt_pid) {
        sched->interrupt_pid = 0;
        sched->interrupt_struck = false;
    }
    if (actor->pid == sched->eval_pid && !sched->eval_done) {
        sched->eval_done = true;
        sched->eval_reason = copy_to_runtime_or_oom(sched, stored);
        finished = true;
    }
    if (finished) sched_wake(sched, true);
    idm_swap_active_heap(prev_heap);
    if (!running) {
        actor_release_resources(actor);
        if (reapable(sched, actor)) reap_queue_push(sched, actor);
    }
}

static bool port_entry_finish(IdmScheduler *sched, PortEntry *entry, IdmError *err) {
    if (!port_entry_ensure_result(entry, err)) return false;
    ProcId self = proc_port(entry->id);

    Monitor *mons = entry->mon_in;
    size_t mon_count = entry->mon_in_count;
    entry->mon_in = NULL;
    entry->mon_in_count = 0;
    entry->mon_in_cap = 0;
    for (size_t i = 0; i < mon_count; i++) {
        if (mons[i].peer.kind != PROC_ACTOR) continue;
        IdmActor *watcher = sched_lookup(sched, mons[i].peer.id);
        if (!watcher || watcher->exited) continue;
        mon_remove_by_ref(watcher->mon_out, &watcher->mon_out_count, mons[i].ref, NULL);
        PendingSignal *ps = sched_signal_enqueue(sched, watcher, NULL, "down", idm_ref(mons[i].ref), proc_value(self), idm_nil(), 4);
        if (ps) { ps->port_reason = true; ps->port_exit_code = entry->exit_code; }
    }
    free(mons);

    size_t link_count = entry->link_count;
    ProcId *links = NULL;
    if (link_count > 0) {
        links = malloc(link_count * sizeof(*links));
        if (links) memcpy(links, entry->links, link_count * sizeof(*links));
    }
    entry->link_count = 0;
    for (size_t i = 0; links && i < link_count; i++) {
        proc_links_remove(sched, links[i], self);
        if (links[i].kind == PROC_ACTOR) {
            IdmActor *target = sched_lookup(sched, links[i].id);
            if (!target || target->exited) continue;
            if (target->trap_exit) {
                PendingSignal *ps = sched_signal_enqueue(sched, target, NULL, "exit", proc_value(self), idm_nil(), idm_nil(), 3);
                if (ps) { ps->port_reason = true; ps->port_exit_code = entry->exit_code; }
            } else if (entry->exit_code != 0) {
                terminate(sched, target, reason_from_exit_code(sched, &target->heap, entry->exit_code));
            }
        } else if (entry->exit_code != 0) {
            apply_port_exit_signal(sched_find_port(sched, links[i].id), idm_atom(sched->rt, "exit-code"));
        }
    }
    free(links);
    if (entry->port) idm_port_release_process_state(entry->port);
    return true;
}

static bool tty_finish(IdmScheduler *sched, IdmActor *actor, IdmValue result, IdmError *err) {
    free(actor->tty_buf);
    actor->tty_buf = NULL;
    actor->tty_buf_len = 0;
    actor->tty_buf_cap = 0;
    actor_wait_deadline_clear(actor);
    sched->tty_waiting--;
    return actor_resume_with(sched, actor, result, err);
}

static bool tty_line_finish(IdmScheduler *sched, IdmActor *actor, IdmError *err) {
    IdmHeap *prev_heap = idm_swap_active_heap(&actor->heap);
    IdmValue text = idm_string_n(sched->rt, actor->tty_buf ? actor->tty_buf : "", actor->tty_buf_len, err);
    idm_swap_active_heap(prev_heap);
    if (err->present) return false;
    IdmValue signal = make_signal(sched, &actor->heap, "line", text, idm_nil(), idm_nil(), 2, err);
    if (err->present) return false;
    return tty_finish(sched, actor, signal, err);
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
        if (!idm_grow((void **)&actor->tty_buf, &actor->tty_buf_cap, sizeof(*actor->tty_buf), 64u, actor->tty_buf_len + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
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
            if (n == 1) {
                IdmValue signal = make_signal(sched, &actor->heap, "byte", idm_int((int64_t)b), idm_nil(), idm_nil(), 2, err);
                if (err->present) return false;
                return tty_finish(sched, actor, signal, err);
            }
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
    if (sched->resize_pending && !actor->wait_has_deadline && actor->tty_buf_len == 0) {
        sched->resize_pending = false;
        IdmValue signal = make_signal(sched, &actor->heap, "signal", idm_atom(sched->rt, "resize"), idm_nil(), idm_nil(), 2, err);
        if (err->present) return false;
        return tty_finish(sched, actor, signal, err);
    }
    if (actor->wait_has_deadline && now_ms() >= actor->wait_deadline_ms) {
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
    bool waiter_interrupted = false;
    while ((n = read(g_sig_pipe[0], buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == 'c') { sched->sigchld_pending = true; continue; }
            if (buf[i] == 'r') { sched->resize_pending = true; continue; }
            IdmActor *soft = tty_signal_target(sched);
            if (soft) {
                sched->interrupt_pid = 0;
                sched->interrupt_struck = false;
            }
            IdmActor *target = sched->interrupt_pid != 0 ? sched_lookup(sched, sched->interrupt_pid) : NULL;
            if (target && !target->exited) {
                if (sched->interrupt_struck) {
                    terminate(sched, target, idm_atom(sched->rt, "killed"));
                } else {
                    sched->interrupt_struck = true;
                    apply_exit_signal(sched, target, proc_actor(0), idm_atom(sched->rt, "interrupt"));
                }
                continue;
            }
            sched->interrupt_pid = 0;
            sched->interrupt_struck = false;
            IdmActor *waiter = tty_signal_target(sched);
            if (!waiter) {
                if (waiter_interrupted) continue;
                uint64_t session = idm_sched_session_pid(sched);
                IdmActor *root = session != 0 ? sched_lookup(sched, session) : NULL;
                if (root && !root->exited) {
                    sched->interrupt_pid = session;
                    sched->interrupt_struck = true;
                    apply_exit_signal(sched, root, proc_actor(0), idm_atom(sched->rt, "interrupt"));
                }
                continue;
            }
            waiter_interrupted = true;
            IdmError ignore;
            idm_error_init(&ignore);
            IdmValue signal = make_signal(sched, &waiter->heap, "signal", idm_atom(sched->rt, "interrupt"), idm_nil(), idm_nil(), 2, &ignore);
            if (!ignore.present) tty_finish(sched, waiter, signal, &ignore);
            idm_error_clear(&ignore);
        }
    }
}

static IdmValue crash_reason_from_err(IdmScheduler *sched, IdmError *err) {
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
    *out_pid = idm_pid(actor->pid);
    pthread_mutex_lock(&actor->mbox_mu);
    sched_unlock(sched);
    bool oom = !idm_exec_copy_context(actor->exec, parent);
    bool setup_failed = false;
    IdmValue setup_reason = idm_nil();
    if (!oom) {
        IdmValue tk = idm_value_copy(sched->rt, &actor->heap, thunk, err);
        if (err->present) oom = true;
        else if (!idm_exec_setup_thunk(actor->exec, tk, err)) {
            IdmHeap *prev_heap = idm_swap_active_heap(&actor->heap);
            setup_reason = crash_reason_from_err(sched, err);
            idm_swap_active_heap(prev_heap);
            setup_failed = true;
        }
    }
    pthread_mutex_unlock(&actor->mbox_mu);
    sched_lock(sched);
    if (oom) {
        if (!err->present) idm_error_oom(err, idm_span_unknown(NULL));
        return false;
    }
    if (setup_failed) {
        terminate(sched, actor, setup_reason);
        return true;
    }
    if (!ready_push(sched, actor->pid, err)) return false;
    actor->status = ACT_READY;
    return true;
}

static bool proc_dead_reason(IdmScheduler *sched, IdmHeap *heap, ProcId proc, bool spawned, bool *out_live, IdmValue *out_reason, IdmError *err) {
    *out_live = false;
    *out_reason = idm_atom(sched->rt, "noproc");
    if (proc.kind == PROC_ACTOR) {
        IdmActor *target = sched_lookup(sched, proc.id);
        if (!target) return true;
        if (!target->exited) {
            *out_live = true;
            return true;
        }
        if (spawned) *out_reason = target->exit_reason;
        return true;
    }
    PortEntry *port = sched_find_port(sched, proc.id);
    if (!port) return true;
    if (!port->done) {
        *out_live = true;
        return true;
    }
    if (!port_entry_ensure_result(port, err)) return false;
    *out_reason = port_exit_reason(sched, heap, port);
    return true;
}

static bool sched_link_locked(IdmScheduler *sched, IdmActor *self, IdmValue target_value, bool spawned, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err) {
    *self_should_exit = false;
    ProcId target_proc;
    if (!value_to_process(sched, target_value, &target_proc)) return idm_error_set(err, idm_span_unknown(NULL), "link expects a process");
    ProcId self_proc = proc_actor(self->pid);
    if (proc_equal(target_proc, self_proc)) return true;
    bool live = false;
    IdmValue reason = idm_nil();
    if (!proc_dead_reason(sched, &self->heap, target_proc, spawned, &live, &reason, err)) return false;
    if (!live) {
        if (self->trap_exit) {
            IdmActor *src = target_proc.kind == PROC_ACTOR ? sched_lookup(sched, target_proc.id) : NULL;
            return post_signal(sched, self, src, "exit", proc_value(target_proc), reason, idm_nil(), 3, err);
        }
        if (!reason_is_normal(reason)) {
            *self_should_exit = true;
            *self_exit_reason = reason;
        }
        return true;
    }
    if (!proc_links_add(&self->links, &self->link_count, &self->link_cap, target_proc, err)) return false;
    if (!proc_links_add_to(sched, target_proc, self_proc, err)) {
        actor_links_remove(self, target_proc);
        return false;
    }
    return true;
}

bool idm_sched_link(IdmScheduler *sched, IdmActor *self, IdmValue target, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err) {
    sched_lock(sched);
    bool ok = sched_link_locked(sched, self, target, false, self_should_exit, self_exit_reason, err);
    sched_unlock(sched);
    return ok;
}

bool idm_sched_exit_signal(IdmScheduler *sched, IdmActor *self, IdmValue target_value, IdmValue reason, bool *self_should_exit, IdmValue *self_exit_reason, IdmError *err) {
    sched_lock(sched);
    *self_should_exit = false;
    ProcId target_proc;
    if (!value_to_process(sched, target_value, &target_proc)) {
        sched_unlock(sched);
        return idm_error_set(err, idm_span_unknown(NULL), "exit expects a process");
    }
    ProcId self_proc = proc_actor(self->pid);
    bool untrappable = reason_is_atom(reason, "kill");
    IdmValue applied = untrappable ? idm_atom(sched->rt, "killed") : reason;
    if (proc_equal(target_proc, self_proc)) {
        if (untrappable) {
            *self_should_exit = true;
            *self_exit_reason = applied;
        } else if (self->trap_exit) {
            bool ok = post_signal(sched, self, self, "exit", proc_value(self_proc), reason, idm_nil(), 3, err);
            sched_unlock(sched);
            return ok;
        } else {
            *self_should_exit = true;
            *self_exit_reason = reason;
        }
        sched_unlock(sched);
        return true;
    }
    if (target_proc.kind == PROC_ACTOR) {
        IdmActor *target = sched_lookup(sched, target_proc.id);
        if (target && !target->exited) {
            if (untrappable) terminate(sched, target, applied);
            else apply_exit_signal(sched, target, self_proc, reason);
        }
    } else {
        proc_apply_exit_signal(sched, target_proc, self_proc, reason);
    }
    sched_unlock(sched);
    return true;
}

bool idm_sched_unlink(IdmScheduler *sched, IdmActor *self, IdmValue target, IdmError *err) {
    sched_lock(sched);
    ProcId target_proc;
    if (!value_to_process(sched, target, &target_proc)) {
        sched_unlock(sched);
        return idm_error_set(err, idm_span_unknown(NULL), "unlink expects a process");
    }
    ProcId self_proc = proc_actor(self->pid);
    actor_links_remove(self, target_proc);
    proc_links_remove(sched, target_proc, self_proc);
    sched_unlock(sched);
    return true;
}

static bool sched_monitor_locked(IdmScheduler *sched, IdmActor *self, IdmValue target_value, bool spawned, IdmValue *out_ref, IdmError *err) {
    uint64_t ref_id = sched->next_ref++;
    IdmValue ref = idm_ref(ref_id);
    ProcId target_proc;
    if (!value_to_process(sched, target_value, &target_proc)) return idm_error_set(err, idm_span_unknown(NULL), "monitor expects a process");
    bool live = false;
    IdmValue reason = idm_nil();
    if (!proc_dead_reason(sched, &self->heap, target_proc, spawned, &live, &reason, err)) return false;
    if (!live) {
        IdmActor *src = target_proc.kind == PROC_ACTOR ? sched_lookup(sched, target_proc.id) : NULL;
        if (!post_signal(sched, self, src, "down", ref, proc_value(target_proc), reason, 4, err)) return false;
        *out_ref = ref;
        return true;
    }
    if (!mon_add(&self->mon_out, &self->mon_out_count, &self->mon_out_cap, ref_id, target_proc, err)) return false;
    if (!proc_mon_in_add(sched, target_proc, ref_id, proc_actor(self->pid), err)) {
        mon_remove_by_ref(self->mon_out, &self->mon_out_count, ref_id, NULL);
        return false;
    }
    *out_ref = ref;
    return true;
}

bool idm_sched_monitor(IdmScheduler *sched, IdmActor *self, IdmValue target, IdmValue *out_ref, IdmError *err) {
    sched_lock(sched);
    bool ok = sched_monitor_locked(sched, self, target, false, out_ref, err);
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
        ok = sched_link_locked(sched, self, pid, true, self_should_exit, self_exit_reason, err);
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
        ok = sched_monitor_locked(sched, self, pid, true, out_ref, err);
    }
    sched_unlock(sched);
    return ok;
}

bool idm_sched_demonitor(IdmScheduler *sched, IdmActor *self, IdmValue ref) {
    sched_lock(sched);
    if (idm_value_tag(ref) != IDM_VAL_REF) { sched_unlock(sched); return false; }
    uint64_t ref_id = idm_value_id(ref);
    ProcId target = {0};
    bool found = false;
    for (size_t i = 0; i < self->mon_out_count; i++) {
        if (self->mon_out[i].ref == ref_id) {
            target = self->mon_out[i].peer;
            found = true;
            break;
        }
    }
    mon_remove_by_ref(self->mon_out, &self->mon_out_count, ref_id, NULL);
    if (found) proc_mon_in_remove(sched, target, ref_id);
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
    if (ok) *out = actor->mailbox[actor->mb_head + index];
    pthread_mutex_unlock(&a->mbox_mu);
    return ok;
}

bool idm_actor_mailbox_remove(IdmActor *actor, size_t index, IdmValue *out) {
    pthread_mutex_lock(&actor->mbox_mu);
    bool ok = index < actor->mb_count;
    if (ok) {
        size_t phys = actor->mb_head + index;
        *out = actor->mailbox[phys];
        if (index == 0) {
            idm_gc_write_barrier(actor->mailbox[phys]);
            actor->mb_head++;
        } else {
            size_t end = actor->mb_head + actor->mb_count;
            for (size_t j = phys; j < end; j++) idm_gc_write_barrier(actor->mailbox[j]);
            memmove(actor->mailbox + phys, actor->mailbox + phys + 1u, (actor->mb_count - index - 1u) * sizeof(*actor->mailbox));
        }
        actor->mb_count--;
        if (actor->mb_count == 0) actor->mb_head = 0;
    }
    pthread_mutex_unlock(&actor->mbox_mu);
    return ok;
}

bool idm_actor_recv_no_match(IdmActor *actor, IdmValue timeout, IdmRecvDecision *out, IdmError *err) {
    if (!actor->recv_waiting) {
        actor->recv_waiting = true;
        if (idm_value_tag(timeout) == IDM_VAL_ATOM && strcmp(idm_symbol_text(idm_value_symbol(timeout)), "infinity") == 0) {
            actor_wait_deadline_clear(actor);
        } else {
            int64_t timeout_ms = 0;
            if (!idm_value_is_int(timeout) || !idm_int_to_i64(timeout, &timeout_ms) || timeout_ms < 0) {
                actor->recv_waiting = false;
                return idm_error_set(err, idm_span_unknown(NULL), "receive timeout must be a non-negative integer or :infinity");
            }
            actor_wait_deadline_set(actor, now_ms_ceil() + (uint64_t)timeout_ms);
        }
    }
    if (actor->wait_has_deadline && now_ms() >= actor->wait_deadline_ms) {
        actor->recv_waiting = false;
        actor_wait_deadline_clear(actor);
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
    actor_wait_deadline_clear(actor);
    actor->recv_cursor = cursor;
}

static bool sched_port_data_value(IdmScheduler *sched, const char *data, size_t len, IdmValue *out, IdmError *err);
static bool sched_port_wrote_value(IdmScheduler *sched, size_t n, IdmValue *out, IdmError *err);
static IdmValue sched_port_io_atom(IdmScheduler *sched, IdmPortIoStatus status);
static bool port_io_once(IdmScheduler *sched, uint64_t port_id, PortIoOp op, bool missing_is_closed, const char *stream, size_t max, const char *data, size_t len, IdmValue *out, bool *out_found, bool *out_blocked, IdmError *err);

static bool port_io_finish(IdmScheduler *sched, IdmActor *actor, IdmValue result, IdmError *err) {
    actor->port_io_port_id = 0;
    actor->port_io_stream = NULL;
    actor->port_io_max = 0;
    idm_gc_write_barrier(actor->port_io_data);
    actor->port_io_data = idm_nil();
    return actor_resume_with(sched, actor, result, err);
}

static bool service_port_io(IdmScheduler *sched, IdmActor *actor, PortIoOp op, IdmError *err) {
    actor->refs++;
    const char *data = op == PORT_IO_WRITE ? idm_string_bytes(actor->port_io_data) : NULL;
    size_t len = op == PORT_IO_WRITE ? idm_string_length(actor->port_io_data) : 0;
    IdmValue result = idm_nil();
    bool found = false;
    bool blocked = false;
    IdmHeap *prev_heap = idm_swap_active_heap(&actor->heap);
    bool ok = port_io_once(sched, actor->port_io_port_id, op, true, actor->port_io_stream, actor->port_io_max, data, len, &result, &found, &blocked, err);
    idm_swap_active_heap(prev_heap);
    (void)found;
    bool result_ok = !ok ? false : (blocked ? true : port_io_finish(sched, actor, result, err));
    sched_release_actor(sched, actor);
    return result_ok;
}

static bool sched_service_port_io(IdmScheduler *sched, IdmError *err) {
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (!a || a->exited) continue;
        if (a->status == ACT_WAITING_PORT_READ) {
            if (!service_port_io(sched, a, PORT_IO_READ, err)) return false;
        } else if (a->status == ACT_WAITING_PORT_WRITE) {
            if (!service_port_io(sched, a, PORT_IO_WRITE, err)) return false;
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

static bool sched_has_port_read_waiter(IdmScheduler *sched, uint64_t port_id, const char *stream) {
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (a && !a->exited && a->status == ACT_WAITING_PORT_READ && a->port_io_port_id == port_id && a->port_io_stream && strcmp(a->port_io_stream, stream) == 0) return true;
    }
    return false;
}

static bool sched_has_port_io_waiter(IdmScheduler *sched) {
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (a && !a->exited && (a->status == ACT_WAITING_PORT_READ || a->status == ACT_WAITING_PORT_WRITE)) return true;
    }
    return false;
}

static bool port_reclaimable(IdmScheduler *sched, PortEntry *entry) {
    return entry->live && entry->done && entry->port && entry->refs == 0
        && (!entry->out_readable || entry->eof_out)
        && (!entry->err_readable || entry->eof_err)
        && entry->link_count == 0 && entry->mon_in_count == 0
        && idm_port_io_drained(entry->port)
        && !sched_has_port_read_waiter(sched, entry->id, "stdout")
        && !sched_has_port_read_waiter(sched, entry->id, "stderr")
        && !sched_has_port_write_waiter(sched, entry->id);
}

static void sched_reclaim_done_ports(IdmScheduler *sched) {
    for (size_t i = 0; i < sched->port_count; i++) {
        PortEntry *entry = &sched->ports[i];
        if (port_reclaimable(sched, entry)) sched_reclaim_port_slot(sched, entry);
    }
}

static bool pollfd_push(struct pollfd **fds, size_t *count, size_t *cap, int fd, short events, IdmError *err) {
    if (*count == *cap) {
        if (!idm_grow((void **)fds, cap, sizeof(**fds), 8u, *count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    (*fds)[*count].fd = fd;
    (*fds)[*count].events = events;
    (*fds)[*count].revents = 0;
    (*count)++;
    return true;
}

static bool poll_ready(short revents) {
    return (revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
}

static bool sched_pidfd_ready(IdmScheduler *sched, const struct pollfd *fds, size_t nfds) {
    for (size_t i = 0; i < sched->port_count; i++) {
        PortEntry *e = &sched->ports[i];
        if (!e->live || e->done) continue;
        int pf[3];
        size_t n = idm_port_live_fds(e->port, pf, 3u);
        for (size_t k = 0; k < n; k++) {
            for (size_t j = 0; j < nfds; j++) {
                if (fds[j].fd == pf[k] && poll_ready(fds[j].revents)) return true;
            }
        }
    }
    return false;
}

static bool sched_poll(IdmScheduler *sched, bool block, IdmError *err) {
    if (!sched_service_port_io(sched, err)) return false;
    if (block && atomic_load(&sched->ready_total) != 0) return true;
    bool any_live = false;
    bool any_deadline = false;
    uint64_t nearest = 0;
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (!a) continue;
        if (a->exited) continue;
        any_live = true;
        if ((a->status == ACT_WAITING_RECEIVE || a->status == ACT_WAITING_TTY) && a->wait_has_deadline) {
            if (!any_deadline || a->wait_deadline_ms < nearest) {
                nearest = a->wait_deadline_ms;
                any_deadline = true;
            }
        }
    }
    sched->deadline_hint = any_deadline ? nearest : UINT64_MAX;
    bool any_port = sched->ports_pending != 0 || sched_has_port_io_waiter(sched);
    bool any_tty = sched->tty_waiting != 0;
    if (block) {
        if (!any_live) return true;
        if (!any_port && !any_deadline && !any_tty) {
            bool has_sig = g_sig_pipe[0] >= 0 && sched->interrupt_pid != 0;
            if (sched->parked == sched->nworkers - 1u && !has_sig) {
                if (ready_none(sched)) return sched_crash_stall_error(sched, err);
                return true;
            }
            struct pollfd waitfds[2];
            nfds_t wait_count = 0;
            if (has_sig) {
                waitfds[wait_count].fd = g_sig_pipe[0];
                waitfds[wait_count].events = POLLIN;
                waitfds[wait_count].revents = 0;
                wait_count++;
            }
            if (sched->wake_pipe[0] >= 0) {
                waitfds[wait_count].fd = sched->wake_pipe[0];
                waitfds[wait_count].events = POLLIN;
                waitfds[wait_count].revents = 0;
                wait_count++;
            }
            sched_unlock(sched);
            poll(wait_count != 0 ? waitfds : NULL, wait_count, -1);
            sched_lock(sched);
            char drain[64];
            while (sched->wake_pipe[0] >= 0 && read(sched->wake_pipe[0], drain, sizeof(drain)) > 0) {}
            sched_check_signals(sched);
            return true;
        }
        size_t fd_cap = 0;
        size_t nfds = 0;
        struct pollfd *fds = NULL;
        if (sched->wake_pipe[0] >= 0) {
            if (!pollfd_push(&fds, &nfds, &fd_cap, sched->wake_pipe[0], POLLIN, err)) return false;
        }
        if (g_sig_pipe[0] >= 0) {
            if (!pollfd_push(&fds, &nfds, &fd_cap, g_sig_pipe[0], POLLIN, err)) { free(fds); return false; }
        }
        if (any_tty) {
            if (!pollfd_push(&fds, &nfds, &fd_cap, idm_tty_in_fd(), POLLIN, err)) { free(fds); return false; }
        }
        for (size_t i = 0; i < sched->port_count; i++) {
            PortEntry *e = &sched->ports[i];
            if (!e->live) continue;
            int pf[3];
            if (!e->done) {
                size_t n = idm_port_live_fds(e->port, pf, 3u);
                for (size_t k = 0; k < n; k++) {
                    if (!pollfd_push(&fds, &nfds, &fd_cap, pf[k], POLLIN, err)) { free(fds); return false; }
                }
            }
            int out_fd = sched_has_port_read_waiter(sched, e->id, "stdout") ? idm_port_output_fd(e->port, "stdout") : -1;
            if (out_fd >= 0) {
                if (!pollfd_push(&fds, &nfds, &fd_cap, out_fd, POLLIN, err)) { free(fds); return false; }
            }
            int err_fd = sched_has_port_read_waiter(sched, e->id, "stderr") ? idm_port_output_fd(e->port, "stderr") : -1;
            if (err_fd >= 0) {
                if (!pollfd_push(&fds, &nfds, &fd_cap, err_fd, POLLIN, err)) { free(fds); return false; }
            }
            int input_fd = sched_has_port_write_waiter(sched, e->id) ? idm_port_input_fd(e->port) : -1;
            if (input_fd >= 0) {
                if (!pollfd_push(&fds, &nfds, &fd_cap, input_fd, POLLOUT, err)) { free(fds); return false; }
            }
        }
        int timeout = -1;
        if (any_deadline) {
            uint64_t now = now_ms();
            int delta = nearest > now ? (int)(nearest - now) : 0;
            if (timeout < 0 || delta < timeout) timeout = delta;
        }
        if (nfds > 0 || timeout >= 0) {
            sched_unlock(sched);
            poll(nfds > 0 ? fds : NULL, (nfds_t)nfds, timeout);
            sched_lock(sched);
            if (sched->wake_pipe[0] >= 0) {
                char drain[64];
                while (read(sched->wake_pipe[0], drain, sizeof(drain)) > 0) {}
            }
            if (sched_pidfd_ready(sched, fds, nfds)) sched->sigchld_pending = true;
            sched_check_signals(sched);
        }
        free(fds);
    }

    if (!sched_service_tty(sched, err)) return false;
    if (sched->sigchld_pending) {
        sched->sigchld_pending = false;
        for (size_t i = 0; i < sched->port_count; i++) {
            PortEntry *e = &sched->ports[i];
            if (!e->live || e->done) continue;
            if (e->refs != 0) { sched->sigchld_pending = true; continue; }
            idm_port_reap(e->port);
            if (idm_port_done(e->port)) {
                e->done = true;
                if (e->pending_completion) {
                    sched->ports_pending--;
                    e->pending_completion = false;
                }
                if (!port_entry_finish(sched, e, err)) return false;
            } else if (idm_port_stopped(e->port)) {
                if (!e->notified_stopped) {
                    e->notified_stopped = true;
                    ProcId self = proc_port(e->id);
                    for (size_t m = 0; m < e->mon_in_count; m++) {
                        if (e->mon_in[m].peer.kind != PROC_ACTOR) continue;
                        IdmActor *watcher = sched_lookup(sched, e->mon_in[m].peer.id);
                        if (!watcher || watcher->exited) continue;
                        sched_signal_enqueue(sched, watcher, NULL, "stopped", idm_ref(e->mon_in[m].ref), proc_value(self), idm_nil(), 3);
                    }
                }
            } else {
                e->notified_stopped = false;
            }
        }
    }
    if (!sched_service_port_io(sched, err)) return false;

    uint64_t reached = now_ms();
    for (size_t i = 0; i < sched->actor_count; i++) {
        IdmActor *a = sched->actors[i];
        if (!a) continue;
        if (a->exited || a->status != ACT_WAITING_RECEIVE) continue;
        if (a->wait_has_deadline && reached >= a->wait_deadline_ms) {
            if (!ready_push(sched, a->pid, err)) return false;
            a->status = ACT_READY;
        }
    }
    sched_reclaim_done_ports(sched);
    return true;
}

static bool run_slice(IdmScheduler *sched, uint64_t pid, IdmError *err) {
    IdmActor **dead = NULL;
    size_t dead_count = 0;
    sched_lock(sched);
    if (atomic_load_explicit(&sched->reap_count, memory_order_relaxed) != 0) dead = drain_reap(sched, &dead_count);
    IdmActor *actor = sched_lookup(sched, pid);
    if (!actor || actor->exited || actor->status != ACT_READY) {
        sched_unlock(sched);
        if (dead) reap_free_detached(dead, dead_count);
        return true;
    }
    actor->status = ACT_RUNNING;
    sched_unlock(sched);
    if (dead) reap_free_detached(dead, dead_count);
    IdmExecStatus status = IDM_EXEC_DONE;
    IdmValue result = idm_nil();
    IdmValue reason = idm_nil();
    int64_t budget = IDM_ACTOR_REDUCTIONS;
    if (actor_gc_wants(actor)) {
        int64_t gc_budget = (budget * IDM_GC_STEPS_PER_REDUCTION) / 8;
        if (gc_budget < 1) gc_budget = 1;
        int64_t before = gc_budget;
        actor_gc_step(actor, &gc_budget);
        budget -= (before - gc_budget + IDM_GC_STEPS_PER_REDUCTION - 1) / IDM_GC_STEPS_PER_REDUCTION;
        if (actor->gc_root_stage != GC_ROOT_DONE) budget = 0;
    }
    if (budget == 0) status = IDM_EXEC_YIELD;
    idm_set_active_heap(&actor->heap);
    bool stepped = budget == 0 ? true : idm_exec_step(actor->exec, budget, &status, &result, &reason, err);
    idm_set_active_heap(NULL);
    if (stepped && idm_heap_gc_active(&actor->heap)) {
        if (status == IDM_EXEC_DONE) {
            idm_gc_mark_value(&actor->heap, result);
        } else if (status == IDM_EXEC_EXIT) {
            idm_gc_mark_value(&actor->heap, reason);
        }
    }
    sched_lock(sched);
    if (actor->exited) {
        actor_release_resources(actor);
        if (!stepped) idm_error_clear(err);
    } else if (!stepped) {
        actor->status = ACT_READY;
        IdmHeap *prev_heap = idm_swap_active_heap(&actor->heap);
        IdmValue crash_reason = crash_reason_from_err(sched, err);
        idm_swap_active_heap(prev_heap);
        terminate(sched, actor, crash_reason);
    } else {
        actor->status = ACT_READY;
        switch (status) {
                case IDM_EXEC_DONE:
                    if (actor->pid == sched->main_pid && !sched->main_terminated) {
                        IdmValue copied = idm_nil();
                        if (!copy_to_runtime(sched, result, &copied, err)) { sched_unlock(sched); return false; }
                        sched->main_terminated = true;
                        sched->main_value = copied;
                        sched->main_abnormal = false;
                        sched_wake(sched, true);
                    }
                    if (actor->pid == sched->eval_pid && !sched->eval_done) {
                        IdmValue copied = idm_nil();
                        if (!copy_to_runtime(sched, result, &copied, err)) { sched_unlock(sched); return false; }
                        sched->eval_value = copied;
                    }
                    terminate(sched, actor, idm_atom(sched->rt, "normal"));
                    break;
                case IDM_EXEC_YIELD:
                    if (!ready_push(sched, actor->pid, err)) { sched_unlock(sched); return false; }
                    break;
                case IDM_EXEC_BLOCK_RECEIVE:
                    actor->status = ACT_WAITING_RECEIVE;
                    if (actor->wait_has_deadline && actor->wait_deadline_ms < sched->deadline_hint) sched->deadline_hint = actor->wait_deadline_ms;
                    if (idm_actor_mailbox_count(actor) > actor->recv_cursor) {
                        actor->status = ACT_READY;
                        if (!ready_push(sched, actor->pid, err)) { sched_unlock(sched); return false; }
                    }
                    break;
                case IDM_EXEC_BLOCK_PORT_READ: {
                    IdmValue port_val = idm_nil();
                    const char *stream = NULL;
                    size_t max = 0;
                    if (!idm_exec_take_port_read(actor->exec, &port_val, &stream, &max)) {
                        idm_error_set(err, idm_span_unknown(NULL), "VM blocked for port read without pending port read");
                        sched_unlock(sched);
                        return false;
                    }
                    PortEntry *entry = sched_find_port(sched, idm_value_id(port_val));
                    if (!entry) {
                        if (!actor_resume_with(sched, actor, idm_atom(sched->rt, "closed"), err)) { sched_unlock(sched); return false; }
                    } else {
                        IDM_ASSERT_PROVED(entry->port);
                        actor->port_io_port_id = idm_value_id(port_val);
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
                        idm_error_set(err, idm_span_unknown(NULL), "VM blocked for port write without pending port write");
                        sched_unlock(sched);
                        return false;
                    }
                    PortEntry *entry = sched_find_port(sched, idm_value_id(port_val));
                    if (!entry) {
                        if (!actor_resume_with(sched, actor, idm_atom(sched->rt, "closed"), err)) { sched_unlock(sched); return false; }
                    } else {
                        IDM_ASSERT_PROVED(entry->port);
                        actor->port_io_port_id = idm_value_id(port_val);
                        idm_gc_write_barrier(actor->port_io_data);
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
                    if (has_timeout) actor_wait_deadline_set(actor, now_ms_ceil() + (uint64_t)timeout_ms);
                    else actor_wait_deadline_clear(actor);
                    actor->tty_buf_len = 0;
                    actor->status = ACT_WAITING_TTY;
                    sched->tty_waiting++;
                    if (has_timeout && actor->wait_deadline_ms < sched->deadline_hint) sched->deadline_hint = actor->wait_deadline_ms;
                    break;
                }
                case IDM_EXEC_EXIT:
                    terminate(sched, actor, reason);
                    break;
            }
    }
    bool ok = true;
    if (sched->nworkers == 1u || (++tls_sigcheck_slices & 63u) == 0) sched_check_signals(sched);
    if (sched->ports_pending != 0 || sched_has_port_io_waiter(sched) || sched->tty_waiting != 0 || now_ms() >= sched->deadline_hint) ok = sched_poll(sched, false, err);
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
    size_t w = atomic_fetch_add(&sched->worker_seq, 1u) % sched->queue_count;
    tls_worker_sched = sched;
    tls_worker_index = w;
    IdmError err;
    idm_error_init(&err);
    for (;;) {
        if (sched->shutdown) break;
        if (sched->main_terminated || sched->eval_done) {
            sched_lock(sched);
            if (!sched->shutdown) {
                sched->shutdown = true;
                sched_wake(sched, true);
            }
            sched_unlock(sched);
            break;
        }
        sched_drain_signals(sched);
        uint64_t pid = 0;
        if (ready_take(sched, w, &pid)) {
            bool ok = run_slice(sched, pid, &err);
            if (!ok || err.present) {
                sched_lock(sched);
                worker_fatal(sched, &err);
                sched_unlock(sched);
                break;
            }
            continue;
        }
        sched_lock(sched);
        if (sched->shutdown || sched->main_terminated || sched->eval_done || atomic_load(&sched->ready_total) != 0) {
            sched_unlock(sched);
            continue;
        }
        if (!sched->poller_active) {
            sched->poller_active = true;
            bool ok = sched_poll(sched, true, &err);
            sched->poller_active = false;
            if (!ok || err.present) {
                worker_fatal(sched, &err);
                sched_unlock(sched);
                break;
            }
            sched_unlock(sched);
            continue;
        }
        sched->parked++;
        sched_poke_poller(sched);
        pthread_cond_wait(&sched->work_cv, &sched->mu);
        sched->parked--;
        sched_unlock(sched);
    }
    sched_lock(sched);
    pthread_cond_broadcast(&sched->work_cv);
    sched_unlock(sched);
    idm_error_clear(&err);
    return NULL;
}

typedef void (*SchedFailureFn)(IdmScheduler *, IdmError *);

static bool sched_drive(IdmScheduler *sched, SchedFailureFn failure, IdmError *err) {
    size_t want = sched->nworkers - 1u;
    pthread_t *threads = want ? calloc(want, sizeof(*threads)) : NULL;
    if (want && !threads) {
        if (!failure) return idm_error_oom(err, idm_span_unknown(NULL));
        IdmError oom;
        idm_error_init(&oom);
        idm_error_oom(&oom, idm_span_unknown(NULL));
        failure(sched, &oom);
        idm_error_clear(&oom);
        return true;
    }
    size_t started = 0;
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
        if (!failure) {
            *err = fatal;
            return false;
        }
        failure(sched, &fatal);
        idm_error_clear(&fatal);
        return true;
    }
    sched_unlock(sched);
    return true;
}

bool idm_sched_run_main(IdmScheduler *sched, uint32_t main_fn, IdmValue *out_result, bool *out_interrupted, IdmError *err) {
    if (out_interrupted) *out_interrupted = false;
    IdmActor *main_actor = actor_create(sched, err);
    if (!main_actor) return false;
    sched->main_pid = main_actor->pid;
    if (!idm_exec_setup_function(main_actor->exec, main_fn, err)) return false;
    sched_lock(sched);
    bool pushed = ready_push(sched, main_actor->pid, err);
    sched_unlock(sched);
    if (!pushed) return false;

    if (!sched_drive(sched, NULL, err)) return false;
    if (!sched->main_terminated) return idm_error_set(err, idm_span_unknown(NULL), "scheduler stopped before the main actor finished");

    if (sched->main_abnormal) {
        if (out_interrupted && reason_is_atom(sched->main_reason, "interrupt")) {
            *out_interrupted = true;
            return true;
        }
        return sched_abnormal_error(sched, sched->main_reason, err);
    }
    *out_result = sched->main_value;
    return true;
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

static bool sched_port_wrote_value(IdmScheduler *sched, size_t n, IdmValue *out, IdmError *err) {
    IdmValue items[2];
    items[0] = idm_atom(sched->rt, "wrote");
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

static bool port_io_once(IdmScheduler *sched, uint64_t port_id, PortIoOp op, bool missing_is_closed, const char *stream, size_t max, const char *data, size_t len, IdmValue *out, bool *out_found, bool *out_blocked, IdmError *err) {
    *out = idm_nil();
    *out_found = false;
    *out_blocked = false;
    PortEntry *e = sched_find_port(sched, port_id);
    if (!e) {
        if (missing_is_closed) { *out_found = true; *out = idm_atom(sched->rt, "closed"); }
        return true;
    }
    *out_found = true;
    IDM_ASSERT_PROVED(e->port);
    if (e->refs != 0) { *out_blocked = true; return true; }
    IdmPort *port = e->port;
    e->refs++;
    sched_unlock(sched);

    IdmPortIoStatus status = IDM_PORT_IO_CLOSED;
    IdmValue result = idm_nil();
    bool blocked = false;
    bool ok;
    if (op == PORT_IO_READ) {
        char *read_data = NULL;
        size_t read_len = 0;
        ok = idm_port_read(port, stream, max, &read_data, &read_len, &status, err);
        if (ok) {
            blocked = status == IDM_PORT_IO_AGAIN;
            if (status == IDM_PORT_IO_OK) {
                ok = sched_port_data_value(sched, read_data, read_len, &result, err);
            } else {
                result = sched_port_io_atom(sched, status);
            }
        }
        free(read_data);
    } else {
        size_t written = 0;
        ok = idm_port_write(port, data, len, &written, &status, err);
        if (ok) {
            blocked = status == IDM_PORT_IO_AGAIN;
            if (status == IDM_PORT_IO_OK) {
                ok = sched_port_wrote_value(sched, written, &result, err);
            } else {
                result = sched_port_io_atom(sched, status);
            }
        }
    }

    sched_lock(sched);
    PortEntry *e2 = sched_find_port(sched, port_id);
    IDM_ASSERT_PROVED(e2);
    if (op == PORT_IO_READ && (status == IDM_PORT_IO_EOF || status == IDM_PORT_IO_CLOSED)) {
        if (strcmp(stream, "stderr") == 0) e2->eof_err = true;
        else e2->eof_out = true;
    }
    e2->refs--;
    *out = result;
    *out_blocked = blocked;
    return ok;
}

bool idm_sched_port_read(IdmScheduler *sched, uint64_t port_id, const char *stream, size_t max, IdmValue *out, bool *out_found, IdmError *err) {
    sched_lock(sched);
    bool blocked = false;
    bool ok = port_io_once(sched, port_id, PORT_IO_READ, false, stream, max, NULL, 0, out, out_found, &blocked, err);
    (void)blocked;
    sched_unlock(sched);
    return ok;
}

bool idm_sched_port_stderr_merged(IdmScheduler *sched, uint64_t port_id) {
    sched_lock(sched);
    PortEntry *e = sched_find_port(sched, port_id);
    bool merged = e && e->port && idm_port_stderr_on_pty(e->port);
    sched_unlock(sched);
    return merged;
}

bool idm_sched_port_write(IdmScheduler *sched, uint64_t port_id, const char *data, size_t len, IdmValue *out, bool *out_found, IdmError *err) {
    sched_lock(sched);
    bool blocked = false;
    bool ok = port_io_once(sched, port_id, PORT_IO_WRITE, false, NULL, 0, data, len, out, out_found, &blocked, err);
    (void)blocked;
    sched_unlock(sched);
    return ok;
}

bool idm_sched_port_close_input(IdmScheduler *sched, uint64_t port_id, IdmValue *out, bool *out_found, IdmError *err) {
    sched_lock(sched);
    *out_found = false;
    IdmPort *port = sched_port_pin(sched, port_id);
    if (!port) {
        sched_unlock(sched);
        return true;
    }
    sched_unlock(sched);

    IdmPortIoStatus status = idm_port_close_input(port);
    idm_port_reap(port);
    bool now_done = idm_port_done(port);

    sched_lock(sched);
    PortEntry *e = sched_find_port(sched, port_id);
    if (e && now_done && !e->done) {
        e->done = true;
        if (e->pending_completion) {
            sched->ports_pending--;
            e->pending_completion = false;
        }
        if (!port_entry_finish(sched, e, err)) { sched_port_unpin(sched, port_id); sched_unlock(sched); return false; }
    }
    sched_port_unpin(sched, port_id);
    *out_found = true;
    *out = sched_port_io_atom(sched, status);
    sched_unlock(sched);
    return true;
}

bool idm_sched_job_resume(IdmScheduler *sched, uint64_t port_id, bool fg) {
    sched_lock(sched);
    IdmPort *port = sched_port_pin(sched, port_id);
    sched_unlock(sched);
    if (!port) return false;
    idm_port_resume(port, fg);
    sched_lock(sched);
    sched_port_unpin(sched, port_id);
    sched_unlock(sched);
    return true;
}

bool idm_sched_port_resize(IdmScheduler *sched, uint64_t port_id, int cols, int rows, bool *out_live, IdmError *err) {
    sched_lock(sched);
    IdmPort *port = sched_port_pin(sched, port_id);
    *out_live = port != NULL;
    sched_unlock(sched);
    if (!port) return true;
    bool ok = idm_port_resize(port, cols, rows, err);
    sched_lock(sched);
    sched_port_unpin(sched, port_id);
    sched_unlock(sched);
    return ok;
}

bool idm_sched_job_signal(IdmScheduler *sched, uint64_t port_id, int signo) {
    sched_lock(sched);
    IdmPort *port = sched_port_pin(sched, port_id);
    sched_unlock(sched);
    if (!port) return false;
    idm_port_signal_group(port, signo);
    sched_lock(sched);
    sched_port_unpin(sched, port_id);
    sched_unlock(sched);
    return true;
}

static void eval_contain_failure(IdmScheduler *sched, IdmError *err) {
    sched_lock(sched);
    IdmActor *actor = sched_lookup(sched, sched->eval_pid);
    bool live = actor && !actor->exited;
    IdmHeap *prev_heap = idm_swap_active_heap(live ? &actor->heap : NULL);
    IdmValue reason = crash_reason_from_err(sched, err);
    idm_swap_active_heap(prev_heap);
    if (live) {
        terminate(sched, actor, reason);
    } else if (!sched->eval_done) {
        sched->eval_done = true;
        sched->eval_reason = reason;
    }
    sched_unlock(sched);
}

static bool sched_run_thunk(IdmScheduler *sched, IdmValue thunk, bool interrupt_target, IdmValue *out_value, IdmValue *out_reason, IdmError *err) {
    sched_lock(sched);
    sig_drain_pending(sched);
    sched->eval_value = idm_nil();
    sched->eval_reason = idm_nil();
    sched->eval_done = false;
    sched->eval_pid = 0;
    IdmValue pid = idm_nil();
    if (!sched_spawn_locked(sched, thunk, NULL, &pid, err)) {
        sched_unlock(sched);
        return false;
    }
    sched->eval_pid = idm_value_id(pid);
    IdmActor *actor = sched_lookup(sched, idm_value_id(pid));
    if (actor->exited) {
        sched->eval_done = true;
        sched->eval_reason = copy_to_runtime_or_oom(sched, actor->exit_reason);
    } else if (interrupt_target) {
        sched->interrupt_pid = idm_value_id(pid);
        sched->interrupt_struck = false;
    }
    sched_unlock(sched);

    if (!sched_drive(sched, eval_contain_failure, err)) return false;

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
    if (!sched_run_thunk(sched, thunk, true, &value, &reason, err)) return false;
    if (!reason_is_normal(reason)) return sched_abnormal_error(sched, reason, err);
    *out_value = value;
    return true;
}

uint64_t idm_sched_session_pid(const IdmScheduler *sched) {
    return sched->eval_pid != 0 ? sched->eval_pid : sched->main_pid;
}

bool idm_sched_run_session(IdmScheduler *sched, IdmValue thunk, IdmValue *out_value, IdmValue *out_reason, IdmError *err) {
    return sched_run_thunk(sched, thunk, false, out_value, out_reason, err);
}

int idm_sched_session_status(IdmScheduler *sched, IdmValue value, IdmValue reason) {
    if (reason_is_normal(reason)) {
        int64_t status = 0;
        return idm_value_is_int(value) && idm_int_to_i64(value, &status) ? (int)(((status % 256) + 256) % 256) : 0;
    }
    if (reason_is_atom(reason, "interrupt")) return 130;
    int64_t status = 0;
    if (idm_value_is_int(reason) && idm_int_to_i64(reason, &status)) return (int)(((status % 256) + 256) % 256);
    if (idm_value_tag(reason) == IDM_VAL_TUPLE && idm_sequence_count(reason) == 2u) {
        IdmError probe;
        idm_error_init(&probe);
        IdmValue tag = idm_sequence_item(reason, 0, &probe);
        IdmValue code = idm_sequence_item(reason, 1, &probe);
        bool exit_tag = !probe.present && idm_value_tag(tag) == IDM_VAL_ATOM &&
                        strcmp(idm_symbol_text(idm_value_symbol(tag)), "exit-code") == 0;
        idm_error_clear(&probe);
        if (exit_tag && idm_int_to_i64(code, &status)) return (int)(((status % 256) + 256) % 256);
    }
    IdmError err;
    idm_error_init(&err);
    sched_abnormal_error(sched, reason, &err);
    idm_error_fprint(stderr, &err);
    idm_error_clear(&err);
    return 70;
}
