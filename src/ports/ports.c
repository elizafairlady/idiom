#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include "idiom/ports.h"
#include "idiom/common.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#ifdef SYS_pidfd_open
extern long syscall(long number, ...);
#endif

typedef enum { REDIR_FILE, REDIR_DUP, REDIR_DATA } RedirKind;

typedef struct {
    RedirKind kind;
    int fd;
    char *target;
    int op;
    int dup_to;
} Redir;

typedef struct {
    bool is_redir;
    size_t index;
    IdmValue graph;
    bool write_dir;
} SubRef;

typedef struct {
    char **argv;
    size_t argc;
    Redir *redirs;
    size_t redir_count;
    char **env_names;
    char **env_values;
    size_t env_count;
    SubRef *subs;
    size_t sub_count;
    size_t sub_cap;
    pid_t pid;
    int pidfd;
    int status;
    bool reaped;
    bool stopped;
    bool unknown;
} Stage;

typedef enum {
    STDIO_INHERIT,
    STDIO_PIPE,
    STDIO_NULL,
    STDIO_PTY
} StdioPolicy;

typedef enum {
    PORT_PROCESS,
    PORT_FILE
} PortKind;

static bool int_value(IdmValue value, int *out_value) {
    int64_t out = 0;
    if (!idm_value_is_int(value) || !idm_int_to_i64(value, &out) || out < INT_MIN || out > INT_MAX) return false;
    *out_value = (int)out;
    return true;
}

struct IdmPort {
    PortKind kind;
    Stage *stages;
    size_t stage_count;
    bool pipefail;
    StdioPolicy stdin_policy;
    StdioPolicy stdout_policy;
    StdioPolicy stderr_policy;
    int in_fd;
    int out_fd;
    int err_fd;
    bool launched;
    char *launch_error;
    bool process_done;
    bool interactive;
    int job_policy;
    bool fg;
    bool owns_terminal;
    pid_t pgid;
    int pty_master;
    char *pty_slave;
    int eof_sent;
    struct IdmPort **subs;
    size_t sub_count;
    FILE *file;
    bool file_readable;
    bool file_writable;
    bool file_closed;
};

static pid_t g_shell_pgid = 0;
static int g_job_tty = -1;
static pthread_once_t g_sigpipe_once = PTHREAD_ONCE_INIT;

static void ignore_sigpipe(void) {
    signal(SIGPIPE, SIG_IGN);
}

static void ignore_sigpipe_once(void) {
    pthread_once(&g_sigpipe_once, ignore_sigpipe);
}

static int open_pidfd(pid_t pid) {
#ifdef SYS_pidfd_open
    int fd = (int)syscall(SYS_pidfd_open, pid, 0u);
    if (fd >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, O_NONBLOCK);
    }
    return fd;
#else
    (void)pid;
    return -1;
#endif
}

static void close_drained_done_stream(IdmPort *port, int *fd) {
    if (!port->process_done || *fd < 0) return;
    struct pollfd p;
    p.fd = *fd;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1u, 0) > 0 && (p.revents & POLLHUP) != 0 && (p.revents & POLLIN) == 0) {
        close(*fd);
        *fd = -1;
    }
}

void idm_job_control_init(void) {
    g_job_tty = STDIN_FILENO;
    if (getpgrp() != getpid()) setpgid(0, 0);
    g_shell_pgid = getpid();
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    tcsetpgrp(g_job_tty, g_shell_pgid);
}

static bool value_is_atom(IdmValue v, const char *name) {
    return idm_value_tag(v) == IDM_VAL_ATOM && strcmp(idm_symbol_text(idm_value_symbol(v)), name) == 0;
}

static bool parse_stdio_policy(IdmValue v, StdioPolicy *out) {
    if (value_is_atom(v, "inherit")) { *out = STDIO_INHERIT; return true; }
    if (value_is_atom(v, "pipe")) { *out = STDIO_PIPE; return true; }
    if (value_is_atom(v, "null")) { *out = STDIO_NULL; return true; }
    if (value_is_atom(v, "pty")) { *out = STDIO_PTY; return true; }
    return false;
}

static bool parse_stdio_tuple(IdmValue v, StdioPolicy *in, StdioPolicy *out, StdioPolicy *err) {
    if (!idm_is_tuple(v) || idm_sequence_count(v) != 4) return false;
    IdmError ignore;
    idm_error_init(&ignore);
    IdmValue tag = idm_sequence_item(v, 0, &ignore);
    IdmValue inv = idm_sequence_item(v, 1, &ignore);
    IdmValue outv = idm_sequence_item(v, 2, &ignore);
    IdmValue errv = idm_sequence_item(v, 3, &ignore);
    idm_error_clear(&ignore);
    return value_is_atom(tag, "stdio")
        && parse_stdio_policy(inv, in)
        && parse_stdio_policy(outv, out)
        && parse_stdio_policy(errv, err);
}

static bool list_to_array(IdmValue list, IdmValue **out_items, size_t *out_count) {
    size_t count = 0;
    IdmValue cur = list;
    while (idm_is_pair(cur)) {
        count++;
        IdmError ignore;
        idm_error_init(&ignore);
        cur = idm_cdr(cur, &ignore);
        idm_error_clear(&ignore);
    }
    if (!idm_is_empty_list(cur)) return false;
    IdmValue *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    if (count != 0 && !items) return false;
    cur = list;
    for (size_t i = 0; i < count; i++) {
        IdmError ignore;
        idm_error_init(&ignore);
        items[i] = idm_car(cur, &ignore);
        cur = idm_cdr(cur, &ignore);
        idm_error_clear(&ignore);
    }
    *out_items = items;
    *out_count = count;
    return true;
}

static char *dup_string_value(IdmValue v) {
    if (idm_value_tag(v) != IDM_VAL_STRING) return NULL;
    return idm_strndup(idm_string_bytes(v), idm_string_length(v));
}

static bool argv_take(char ***argv, size_t *count, size_t *cap, char *item) {
    if (*count == *cap) {
        if (!idm_grow((void **)argv, cap, sizeof(**argv), 8u, *count + 1u)) { free(item); return false; }
    }
    (*argv)[(*count)++] = item;
    return true;
}

static bool part_append_text(IdmValue part, const IdmExec *exec_ctx, Stage *stage, IdmBuffer *out, bool *is_glob) {
    if (!idm_is_tuple(part) || idm_sequence_count(part) != 2) return false;
    IdmError ignore;
    idm_error_init(&ignore);
    IdmValue tag = idm_sequence_item(part, 0, &ignore);
    IdmValue payload = idm_sequence_item(part, 1, &ignore);
    idm_error_clear(&ignore);
    if (value_is_atom(tag, "lit") || value_is_atom(tag, "glob")) {
        if (idm_value_tag(payload) != IDM_VAL_STRING) return false;
        if (value_is_atom(tag, "glob")) *is_glob = true;
        return idm_buf_append_n(out, idm_string_bytes(payload), idm_string_length(payload));
    }
    if (value_is_atom(tag, "env")) {
        if (idm_value_tag(payload) != IDM_VAL_STRING) return false;
        const char *name = idm_string_bytes(payload);
        const char *value = idm_exec_env_get(exec_ctx, name);
        if (!value) value = getenv(name);
        return idm_buf_append(out, value ? value : "");
    }
    if (value_is_atom(tag, "cat")) {
        IdmValue *parts = NULL;
        size_t part_count = 0;
        if (!list_to_array(payload, &parts, &part_count)) return false;
        for (size_t i = 0; i < part_count; i++) {
            IdmValue sub = parts[i];
            if (!part_append_text(sub, exec_ctx, stage, out, is_glob)) {
                free(parts);
                return false;
            }
        }
        free(parts);
        return true;
    }
    return false;
}

static bool push_glob_expansion(char *pattern, const IdmExec *exec_ctx, char ***argv, size_t *count, size_t *cap) {
    const char *base = idm_exec_cwd(exec_ctx);
    size_t skip = 0;
    char *lookup = pattern;
    if (base && pattern[0] != '/') {
        skip = strlen(base) + 1u;
        lookup = malloc(skip + strlen(pattern) + 1u);
        if (!lookup) { free(pattern); return false; }
        memcpy(lookup, base, skip - 1u);
        lookup[skip - 1u] = '/';
        strcpy(lookup + skip, pattern);
    }
    glob_t g;
    memset(&g, 0, sizeof(g));
    int rc = glob(lookup, 0, NULL, &g);
    if (lookup != pattern) free(lookup);
    if (rc == 0 && g.gl_pathc > 0) {
        bool ok = true;
        for (size_t i = 0; i < g.gl_pathc && ok; i++) {
            char *item = idm_strdup(g.gl_pathv[i] + skip);
            ok = item && argv_take(argv, count, cap, item);
        }
        globfree(&g);
        free(pattern);
        return ok;
    }
    if (rc == GLOB_NOSPACE || rc == GLOB_ABORTED) { globfree(&g); free(pattern); return false; }
    globfree(&g);
    return argv_take(argv, count, cap, pattern);
}

static bool value_is_sub(IdmValue part, IdmValue *out_graph, bool *out_write) {
    if (!idm_is_tuple(part) || idm_sequence_count(part) != 3) return false;
    IdmError ignore;
    idm_error_init(&ignore);
    IdmValue tag = idm_sequence_item(part, 0, &ignore);
    IdmValue graph = idm_sequence_item(part, 1, &ignore);
    IdmValue dir = idm_sequence_item(part, 2, &ignore);
    idm_error_clear(&ignore);
    if (!value_is_atom(tag, "sub")) return false;
    bool write_dir;
    if (value_is_atom(dir, "read")) write_dir = false;
    else if (value_is_atom(dir, "write")) write_dir = true;
    else return false;
    *out_graph = graph;
    *out_write = write_dir;
    return true;
}

static bool stage_sub_push(Stage *stage, bool is_redir, size_t index, IdmValue graph, bool write_dir) {
    if (!idm_grow((void **)&stage->subs, &stage->sub_cap, sizeof(*stage->subs), 4u, stage->sub_count + 1u)) return false;
    stage->subs[stage->sub_count] = (SubRef){ .is_redir = is_redir, .index = index, .graph = graph, .write_dir = write_dir };
    stage->sub_count++;
    return true;
}

static bool resolve_argv_part(IdmValue part, const IdmExec *exec_ctx, Stage *stage, char ***argv, size_t *count, size_t *cap) {
    IdmBuffer text;
    idm_buf_init(&text);
    bool is_glob = false;
    if (!part_append_text(part, exec_ctx, stage, &text, &is_glob)) {
        idm_buf_destroy(&text);
        return false;
    }
    if (!idm_buf_append_char(&text, '\0')) {
        idm_buf_destroy(&text);
        return false;
    }
    char *word = idm_buf_take(&text);
    if (!word) return false;
    if (is_glob) return push_glob_expansion(word, exec_ctx, argv, count, cap);
    return argv_take(argv, count, cap, word);
}

static bool parse_stage(IdmValue stage_value, const IdmExec *exec_ctx, Stage *stage) {
    memset(stage, 0, sizeof(*stage));
    stage->pid = -1;
    stage->pidfd = -1;
    if (!idm_is_tuple(stage_value) || idm_sequence_count(stage_value) != 4) return false;
    IdmError ignore;
    idm_error_init(&ignore);
    IdmValue tag = idm_sequence_item(stage_value, 0, &ignore);
    IdmValue argv_list = idm_sequence_item(stage_value, 1, &ignore);
    IdmValue redir_list = idm_sequence_item(stage_value, 2, &ignore);
    IdmValue env_list = idm_sequence_item(stage_value, 3, &ignore);
    idm_error_clear(&ignore);
    if (!value_is_atom(tag, "stage")) return false;

    IdmValue *parts = NULL;
    size_t part_count = 0;
    if (!list_to_array(argv_list, &parts, &part_count)) return false;
    size_t cap = 0;
    for (size_t i = 0; i < part_count; i++) {
        IdmValue sub_graph = idm_nil();
        bool sub_write = false;
        if (value_is_sub(parts[i], &sub_graph, &sub_write)) {
            char *slot = idm_strdup("");
            if (!slot || !argv_take(&stage->argv, &stage->argc, &cap, slot)) { free(parts); return false; }
            if (!stage_sub_push(stage, false, stage->argc - 1u, sub_graph, sub_write)) { free(parts); return false; }
            continue;
        }
        if (!resolve_argv_part(parts[i], exec_ctx, stage, &stage->argv, &stage->argc, &cap)) { free(parts); return false; }
    }
    free(parts);
    if (!argv_take(&stage->argv, &stage->argc, &cap, NULL)) return false;
    stage->argc--;

    IdmValue *redirs = NULL;
    size_t redir_count = 0;
    if (!list_to_array(redir_list, &redirs, &redir_count)) return false;
    if (redir_count > 0) {
        stage->redirs = calloc(redir_count, sizeof(*stage->redirs));
        if (!stage->redirs) { free(redirs); return false; }
    }
    for (size_t i = 0; i < redir_count; i++) {
        IdmValue r = redirs[i];
        if (!idm_is_tuple(r) || idm_sequence_count(r) < 1) { free(redirs); return false; }
        IdmValue rtag = idm_sequence_item(r, 0, &ignore);
        Redir *out = &stage->redirs[stage->redir_count];
        if (value_is_atom(rtag, "redir")) {
            if (idm_sequence_count(r) != 4) { free(redirs); return false; }
            IdmValue op = idm_sequence_item(r, 1, &ignore);
            IdmValue fd = idm_sequence_item(r, 2, &ignore);
            IdmValue target = idm_sequence_item(r, 3, &ignore);
            out->kind = REDIR_FILE;
            if (!int_value(fd, &out->fd)) { free(redirs); return false; }
            const char *ops = idm_value_tag(op) == IDM_VAL_ATOM ? idm_symbol_text(idm_value_symbol(op))
                            : idm_value_tag(op) == IDM_VAL_STRING ? idm_string_bytes(op)
                            : NULL;
            if (!ops) { free(redirs); return false; }
            out->op = strcmp(ops, "<") == 0 ? '<'
                    : strcmp(ops, "<<") == 0 ? 'h'
                    : strcmp(ops, ">>") == 0 ? 'a'
                    : strcmp(ops, "&>") == 0 ? 'b'
                    : strcmp(ops, "&>>") == 0 ? 'B'
                    : strcmp(ops, ">") == 0 ? '>'
                    : 0;
            if (!out->op) { free(redirs); return false; }
            if (out->op == 'h') out->kind = REDIR_DATA;
            out->dup_to = -1;
            IdmValue sub_graph = idm_nil();
            bool sub_write = false;
            if (out->kind == REDIR_FILE && value_is_sub(target, &sub_graph, &sub_write)) {
                if (!stage_sub_push(stage, true, stage->redir_count, sub_graph, sub_write)) { free(redirs); return false; }
            } else {
                char **tav = NULL;
                size_t tac = 0, tcap = 0;
                if (!resolve_argv_part(target, exec_ctx, stage, &tav, &tac, &tcap) || tac < 1) { free(tav); free(redirs); return false; }
                out->target = tav[0];
                for (size_t j = 1; j < tac; j++) free(tav[j]);
                free(tav);
            }
        } else if (value_is_atom(rtag, "dup")) {
            if (idm_sequence_count(r) != 3) { free(redirs); return false; }
            IdmValue a = idm_sequence_item(r, 1, &ignore);
            IdmValue b = idm_sequence_item(r, 2, &ignore);
            out->kind = REDIR_DUP;
            if (!int_value(a, &out->fd) || !int_value(b, &out->dup_to)) { free(redirs); return false; }
        } else {
            free(redirs);
            return false;
        }
        stage->redir_count++;
    }
    free(redirs);

    IdmValue *envs = NULL;
    size_t env_count = 0;
    if (!list_to_array(env_list, &envs, &env_count)) return false;
    if (env_count > 0) {
        stage->env_names = calloc(env_count, sizeof(*stage->env_names));
        stage->env_values = calloc(env_count, sizeof(*stage->env_values));
        if (!stage->env_names || !stage->env_values) { free(envs); return false; }
    }
    for (size_t i = 0; i < env_count; i++) {
        IdmValue pair = envs[i];
        if (!idm_is_tuple(pair) || idm_sequence_count(pair) != 2) { free(envs); return false; }
        IdmValue name = idm_sequence_item(pair, 0, &ignore);
        IdmValue valpart = idm_sequence_item(pair, 1, &ignore);
        char **vav = NULL;
        size_t vac = 0, vcap = 0;
        if (!resolve_argv_part(valpart, exec_ctx, stage, &vav, &vac, &vcap) || vac < 1) { free(vav); free(envs); return false; }
        char *env_name = dup_string_value(name);
        if (!env_name) {
            for (size_t j = 0; j < vac; j++) free(vav[j]);
            free(vav);
            free(envs);
            return false;
        }
        stage->env_names[stage->env_count] = env_name;
        stage->env_values[stage->env_count] = vav[0];
        for (size_t j = 1; j < vac; j++) free(vav[j]);
        free(vav);
        stage->env_count++;
    }
    free(envs);
    return stage->argc >= 1;
}

static void stage_destroy(Stage *stage) {
    if (stage->pidfd >= 0) {
        close(stage->pidfd);
        stage->pidfd = -1;
    }
    for (size_t i = 0; i < stage->argc; i++) free(stage->argv[i]);
    free(stage->argv);
    for (size_t i = 0; i < stage->redir_count; i++) {
        if (stage->redirs[i].kind == REDIR_DATA && stage->redirs[i].dup_to >= 0) close(stage->redirs[i].dup_to);
        free(stage->redirs[i].target);
    }
    free(stage->redirs);
    for (size_t i = 0; i < stage->env_count; i++) {
        free(stage->env_names[i]);
        free(stage->env_values[i]);
    }
    free(stage->env_names);
    free(stage->env_values);
    free(stage->subs);
}

static void port_reap_stages(IdmPort *port) {
    for (size_t i = 0; i < port->stage_count; i++) {
        Stage *stage = &port->stages[i];
        if (stage->reaped || stage->pid <= 0) continue;
        kill(stage->pid, SIGKILL);
        while (waitpid(stage->pid, NULL, 0) < 0 && errno == EINTR) {}
        stage->reaped = true;
        if (stage->pidfd >= 0) {
            close(stage->pidfd);
            stage->pidfd = -1;
        }
    }
}

bool idm_port_resize(IdmPort *port, int cols, int rows, IdmError *err) {
    if (!port || port->pty_master < 0) return idm_error_set(err, idm_span_unknown(NULL), "port has no terminal to resize");
    if (cols <= 0 || rows <= 0) return idm_error_set(err, idm_span_unknown(NULL), "port-resize expects positive columns and rows");
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    if (ioctl(port->pty_master, TIOCSWINSZ, &ws) != 0) return idm_error_set(err, idm_span_unknown(NULL), "port-resize failed: %s", strerror(errno));
    return true;
}

void idm_port_free(IdmPort *port) {
    if (!port) return;
    port_reap_stages(port);
    for (size_t i = 0; i < port->sub_count; i++) idm_port_free(port->subs[i]);
    free(port->subs);
    if (port->file) fclose(port->file);
    for (size_t i = 0; i < port->stage_count; i++) stage_destroy(&port->stages[i]);
    free(port->stages);
    if (port->in_fd >= 0) close(port->in_fd);
    if (port->out_fd >= 0) close(port->out_fd);
    if (port->err_fd >= 0) close(port->err_fd);
    if (port->pty_master >= 0) close(port->pty_master);
    free(port->pty_slave);
    free(port->launch_error);
    free(port);
}

IdmPort *idm_port_open_file(const char *path, const char *mode, bool readable, bool writable, IdmError *err) {
    FILE *file = fopen(path, mode);
    if (!file) return NULL;
    setvbuf(file, NULL, _IONBF, 0);
    IdmPort *port = calloc(1u, sizeof(*port));
    if (!port) {
        fclose(file);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    port->kind = PORT_FILE;
    port->file = file;
    port->file_readable = readable;
    port->file_writable = writable;
    port->in_fd = -1;
    port->out_fd = -1;
    port->err_fd = -1;
    port->pty_master = -1;
    return port;
}

bool idm_port_waits_completion(const IdmPort *port) {
    return port && port->kind == PORT_PROCESS;
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#define IDM_STAGE_FD_PTY_SLAVE (-2)

#if defined(__has_attribute)
#if __has_attribute(no_sanitize)
#define IDM_CHILD_FN __attribute__((no_sanitize("address", "thread", "undefined")))
#endif
#endif
#ifndef IDM_CHILD_FN
#define IDM_CHILD_FN
#endif

typedef struct {
    bool pty;
    const char *pty_slave;
    bool set_pgid;
    pid_t pgid;
    int tty_fd;
    const char *cwd;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    int close_fds[12];
    size_t close_count;
    const Redir *redirs;
    size_t redir_count;
    char **argv;
    char **shadow_argv;
    char **envp;
    char **paths;
    const char *name;
} StagePlan;

struct ChildSigaction {
    void *handler;
    unsigned long flags;
    void *restorer;
    unsigned long mask;
};

static IDM_CHILD_FN void child_sigdfl(int sig) {
    struct ChildSigaction sa;
    sa.handler = NULL;
    sa.flags = 0;
    sa.restorer = NULL;
    sa.mask = 0;
    (void)syscall(SYS_rt_sigaction, (long)sig, (long)(uintptr_t)&sa, 0L, (long)sizeof(unsigned long));
}

static IDM_CHILD_FN size_t child_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static IDM_CHILD_FN void child_werr(const char *const *parts, size_t count) {
    for (size_t i = 0; i < count; i++) {
        (void)syscall(SYS_write, 2L, (long)(uintptr_t)parts[i], (long)child_len(parts[i]));
    }
}

static IDM_CHILD_FN void child_exit(int code) {
    (void)syscall(SYS_exit_group, (long)code);
    for (;;) {}
}

static IDM_CHILD_FN void child_werrno(int err_no) {
    char digits[16];
    size_t end = sizeof(digits);
    unsigned value = err_no < 0 ? (unsigned)-err_no : (unsigned)err_no;
    do {
        digits[--end] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    if (err_no < 0) digits[--end] = '-';
    (void)syscall(SYS_write, 2L, (long)(uintptr_t)(digits + end), (long)(sizeof(digits) - end));
}

static IDM_CHILD_FN void child_fail3(const char *subject, const char *what, int err_no) {
    const char *parts[] = { "idiom: ", subject, ": ", what, ": errno " };
    child_werr(parts, 5);
    child_werrno(err_no);
    const char *newline[] = { "\n" };
    child_werr(newline, 1);
    child_exit(126);
}

static IDM_CHILD_FN long child_dup2(int from, int to) {
    return syscall(SYS_dup2, (long)from, (long)to);
}

static IDM_CHILD_FN long child_close(int fd) {
    return syscall(SYS_close, (long)fd);
}

static IDM_CHILD_FN int child_open(const char *path, long flags) {
    return (int)syscall(SYS_openat, (long)AT_FDCWD, (long)(uintptr_t)path, flags, 0644L);
}

static IDM_CHILD_FN void stage_child_run(StagePlan *p) {
    child_sigdfl(SIGPIPE);
    int slave = -1;
    if (p->pty) {
        (void)syscall(SYS_setsid);
        slave = child_open(p->pty_slave, O_RDWR);
        if (slave < 0) child_fail3("pty", "open slave failed", errno);
        if (syscall(SYS_ioctl, (long)slave, (long)TIOCSCTTY, 0L) != 0) child_fail3("pty", "set controlling terminal failed", errno);
        child_sigdfl(SIGTSTP);
        child_sigdfl(SIGTTOU);
        child_sigdfl(SIGTTIN);
        child_sigdfl(SIGQUIT);
    }
    if (p->set_pgid) {
        pid_t grp = p->pgid != 0 ? p->pgid : (pid_t)syscall(SYS_getpid);
        (void)syscall(SYS_setpgid, 0L, (long)grp);
        if (p->tty_fd >= 0) (void)syscall(SYS_ioctl, (long)p->tty_fd, (long)TIOCSPGRP, (long)(uintptr_t)&grp);
        child_sigdfl(SIGTSTP);
        child_sigdfl(SIGTTOU);
        child_sigdfl(SIGTTIN);
        child_sigdfl(SIGQUIT);
    }
    if (p->cwd && syscall(SYS_chdir, (long)(uintptr_t)p->cwd) != 0) child_fail3(p->cwd, "chdir failed", errno);
    int in = p->stdin_fd == IDM_STAGE_FD_PTY_SLAVE ? slave : p->stdin_fd;
    int out = p->stdout_fd == IDM_STAGE_FD_PTY_SLAVE ? slave : p->stdout_fd;
    int errfd = p->stderr_fd == IDM_STAGE_FD_PTY_SLAVE ? slave : p->stderr_fd;
    if (in >= 0) (void)child_dup2(in, 0);
    if (out >= 0) (void)child_dup2(out, 1);
    if (errfd >= 0) (void)child_dup2(errfd, 2);
    if (slave >= 0) (void)child_close(slave);
    for (size_t i = 0; i < p->close_count; i++) (void)child_close(p->close_fds[i]);
    for (size_t i = 0; i < p->redir_count; i++) {
        const Redir *r = &p->redirs[i];
        if (r->kind == REDIR_DUP) {
            if (child_dup2(r->dup_to, r->fd) < 0) child_fail3("fd", "redirect failed", errno);
            continue;
        }
        if (r->kind == REDIR_DATA) {
            if (r->dup_to < 0 || child_dup2(r->dup_to, r->fd) < 0) child_fail3("heredoc", "redirect failed", errno);
            (void)child_close(r->dup_to);
            continue;
        }
        if (r->op == 'b' || r->op == 'B') {
            long both_flags = r->op == 'B' ? (O_WRONLY | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC);
            int both = child_open(r->target, both_flags);
            if (both < 0) child_fail3(r->target, "redirect failed", errno);
            if (child_dup2(both, 1) < 0 || child_dup2(both, 2) < 0) child_fail3(r->target, "redirect failed", errno);
            (void)child_close(both);
            continue;
        }
        long flags = r->op == '<' ? O_RDONLY : (r->op == 'a' ? (O_WRONLY | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC));
        int opened = child_open(r->target, flags);
        if (opened < 0) child_fail3(r->target, "redirect failed", errno);
        if (child_dup2(opened, r->fd) < 0) child_fail3(r->target, "redirect failed", errno);
        (void)child_close(opened);
    }
    bool access_denied = false;
    int exec_errno = 0;
    for (size_t i = 0; p->paths[i]; i++) {
        (void)syscall(SYS_execve, (long)(uintptr_t)p->paths[i], (long)(uintptr_t)p->argv, (long)(uintptr_t)p->envp);
        int e = errno;
        if (e == ENOEXEC) {
            p->shadow_argv[1] = p->paths[i];
            (void)syscall(SYS_execve, (long)(uintptr_t)"/bin/sh", (long)(uintptr_t)p->shadow_argv, (long)(uintptr_t)p->envp);
            e = errno;
        }
        if (e == EACCES) {
            access_denied = true;
            continue;
        }
        if (e == ENOENT || e == ENOTDIR || e == ESTALE) continue;
        exec_errno = e;
        break;
    }
    if (exec_errno == 0) exec_errno = access_denied ? EACCES : ENOENT;
    if (exec_errno == ENOENT) {
        const char *parts[] = { "idiom: ", p->name, ": command not found\n" };
        child_werr(parts, 3);
        child_exit(127);
    }
    const char *parts[] = { "idiom: ", p->name, ": errno " };
    child_werr(parts, 3);
    child_werrno(exec_errno);
    const char *newline[] = { "\n" };
    child_werr(newline, 1);
    child_exit(126);
}

static void strv_free(char **v) {
    if (!v) return;
    for (char **p = v; *p; p++) free(*p);
    free(v);
}

static bool strv_push(char ***v, size_t *count, size_t *cap, char *s) {
    if (!s) return false;
    if (*count + 2u > *cap && !idm_grow((void **)v, cap, sizeof(**v), 16u, *count + 2u)) {
        free(s);
        return false;
    }
    (*v)[(*count)++] = s;
    (*v)[*count] = NULL;
    return true;
}

static bool envp_set(char ***envp, size_t *count, size_t *cap, const char *name, const char *value) {
    size_t nlen = strlen(name);
    size_t vlen = strlen(value);
    char *entry = malloc(nlen + vlen + 2u);
    if (!entry) return false;
    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(entry + nlen + 1u, value, vlen + 1u);
    for (size_t i = 0; i < *count; i++) {
        if (strncmp((*envp)[i], name, nlen) == 0 && (*envp)[i][nlen] == '=') {
            free((*envp)[i]);
            (*envp)[i] = entry;
            return true;
        }
    }
    return strv_push(envp, count, cap, entry);
}

static char **stage_envp_build(const IdmExec *exec_ctx, const Stage *stage) {
    char **envp = NULL;
    size_t count = 0, cap = 0;
    for (char **e = environ; e && *e; e++) {
        if (!strv_push(&envp, &count, &cap, idm_strdup(*e))) {
            strv_free(envp);
            return NULL;
        }
    }
    for (size_t i = 0; i < idm_exec_env_count(exec_ctx); i++) {
        const char *ename = NULL;
        const char *evalue = NULL;
        if (idm_exec_env_entry(exec_ctx, i, &ename, &evalue) && !envp_set(&envp, &count, &cap, ename, evalue)) {
            strv_free(envp);
            return NULL;
        }
    }
    for (size_t i = 0; i < stage->env_count; i++) {
        if (!envp_set(&envp, &count, &cap, stage->env_names[i], stage->env_values[i])) {
            strv_free(envp);
            return NULL;
        }
    }
    if (!envp) {
        envp = calloc(1u, sizeof(*envp));
    }
    return envp;
}

static char *path_join_cmd(const char *dir, size_t dlen, const char *file) {
    if (dlen == 0) {
        dir = ".";
        dlen = 1;
    }
    size_t flen = strlen(file);
    char *s = malloc(dlen + flen + 2u);
    if (!s) return NULL;
    memcpy(s, dir, dlen);
    s[dlen] = '/';
    memcpy(s + dlen + 1u, file, flen + 1u);
    return s;
}

static const char *envp_lookup(char **envp, const char *name) {
    size_t nlen = strlen(name);
    for (char **e = envp; *e; e++) {
        if (strncmp(*e, name, nlen) == 0 && (*e)[nlen] == '=') return *e + nlen + 1u;
    }
    return NULL;
}

static char **exec_paths_build(const char *file, char **envp) {
    char **paths = NULL;
    size_t count = 0, cap = 0;
    if (strchr(file, '/')) {
        if (!strv_push(&paths, &count, &cap, idm_strdup(file))) return NULL;
        return paths;
    }
    const char *path = envp_lookup(envp, "PATH");
    if (!path || !path[0]) path = "/bin:/usr/bin";
    const char *p = path;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (!strv_push(&paths, &count, &cap, path_join_cmd(p, dlen, file))) {
            strv_free(paths);
            return NULL;
        }
        if (!colon) break;
        p = colon + 1u;
    }
    return paths;
}

static char **shadow_argv_build(char **argv) {
    size_t argc = 0;
    while (argv[argc]) argc++;
    char **shadow = malloc((argc + 2u) * sizeof(*shadow));
    if (!shadow) return NULL;
    shadow[0] = (char *)"/bin/sh";
    shadow[1] = NULL;
    for (size_t i = 1; i < argc; i++) shadow[i + 1u] = argv[i];
    shadow[argc + 1u] = NULL;
    return shadow;
}

static pid_t stage_clone(StagePlan *plan, IdmError *err) {
    pid_t pid = fork();
    if (pid == 0) stage_child_run(plan);
    if (pid < 0) idm_error_set(err, idm_span_unknown(NULL), "spawn failed: %s", strerror(errno));
    return pid;
}

IdmPort *idm_port_launch(IdmRuntime *rt, IdmValue graph, const IdmExec *exec_ctx, IdmError *err) {
    idm_phase_nondeterministic_record(rt);
    const char *launch_cwd = idm_exec_cwd(exec_ctx);
    IdmPort *port = calloc(1u, sizeof(*port));
    if (!port) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    port->kind = PORT_PROCESS;
    port->in_fd = -1;
    port->out_fd = -1;
    port->err_fd = -1;
    port->pty_master = -1;
    port->stdin_policy = STDIO_INHERIT;
    port->stdout_policy = STDIO_INHERIT;
    port->stderr_policy = STDIO_INHERIT;
    if (!idm_is_tuple(graph) || idm_sequence_count(graph) < 1) { idm_error_set(err, idm_span_unknown(NULL), "invalid command graph"); idm_port_free(port); return NULL; }
    IdmError ignore;
    idm_error_init(&ignore);
    IdmValue tag = idm_sequence_item(graph, 0, &ignore);
    if (value_is_atom(tag, "exec")) {
        size_t exec_count = idm_sequence_count(graph);
        if (exec_count != 3 && exec_count != 4) { idm_error_clear(&ignore); idm_error_set(err, idm_span_unknown(NULL), "invalid command graph"); idm_port_free(port); return NULL; }
        if (exec_count == 4) {
            IdmValue job = idm_sequence_item(graph, 3, &ignore);
            if (value_is_atom(job, "fg")) port->job_policy = 1;
            else if (value_is_atom(job, "bg")) port->job_policy = 2;
            else { idm_error_clear(&ignore); idm_error_set(err, idm_span_unknown(NULL), "invalid command job policy"); idm_port_free(port); return NULL; }
        }
        IdmValue stage = idm_sequence_item(graph, 1, &ignore);
        IdmValue io = idm_sequence_item(graph, 2, &ignore);
        if (!parse_stdio_tuple(io, &port->stdin_policy, &port->stdout_policy, &port->stderr_policy)) {
            idm_error_clear(&ignore);
            idm_error_set(err, idm_span_unknown(NULL), "invalid command stdio policy");
            idm_port_free(port);
            return NULL;
        }
        port->stage_count = 1;
        port->stages = calloc(1u, sizeof(*port->stages));
        if (!port->stages || !parse_stage(stage, exec_ctx, &port->stages[0])) { idm_error_clear(&ignore); idm_error_set(err, idm_span_unknown(NULL), "invalid command stage"); idm_port_free(port); return NULL; }
    } else if (value_is_atom(tag, "pipeline")) {
        size_t pipeline_count = idm_sequence_count(graph);
        if (pipeline_count != 4 && pipeline_count != 5) { idm_error_clear(&ignore); idm_error_set(err, idm_span_unknown(NULL), "invalid command graph"); idm_port_free(port); return NULL; }
        if (pipeline_count == 5) {
            IdmValue job = idm_sequence_item(graph, 4, &ignore);
            if (value_is_atom(job, "fg")) port->job_policy = 1;
            else if (value_is_atom(job, "bg")) port->job_policy = 2;
            else { idm_error_clear(&ignore); idm_error_set(err, idm_span_unknown(NULL), "invalid command job policy"); idm_port_free(port); return NULL; }
        }
        IdmValue list = idm_sequence_item(graph, 1, &ignore);
        IdmValue io = idm_sequence_item(graph, 2, &ignore);
        IdmValue pf = idm_sequence_item(graph, 3, &ignore);
        if (!parse_stdio_tuple(io, &port->stdin_policy, &port->stdout_policy, &port->stderr_policy)) {
            idm_error_clear(&ignore);
            idm_error_set(err, idm_span_unknown(NULL), "invalid command stdio policy");
            idm_port_free(port);
            return NULL;
        }
        if (value_is_atom(pf, "true")) port->pipefail = true;
        else if (value_is_atom(pf, "false")) port->pipefail = false;
        else { idm_error_clear(&ignore); idm_error_set(err, idm_span_unknown(NULL), "invalid command pipefail policy"); idm_port_free(port); return NULL; }
        IdmValue *sv = NULL;
        size_t sc = 0;
        if (!list_to_array(list, &sv, &sc) || sc == 0) { idm_error_clear(&ignore); idm_error_set(err, idm_span_unknown(NULL), "empty pipeline"); idm_port_free(port); return NULL; }
        port->stages = calloc(sc, sizeof(*port->stages));
        if (!port->stages) { free(sv); idm_error_clear(&ignore); idm_error_oom(err, idm_span_unknown(NULL)); idm_port_free(port); return NULL; }
        port->stage_count = sc;
        for (size_t i = 0; i < sc; i++) {
            if (!parse_stage(sv[i], exec_ctx, &port->stages[i])) { free(sv); idm_error_clear(&ignore); idm_error_set(err, idm_span_unknown(NULL), "invalid pipeline stage"); idm_port_free(port); return NULL; }
        }
        free(sv);
    } else {
        idm_error_clear(&ignore);
        idm_error_set(err, idm_span_unknown(NULL), "unknown command graph kind");
        idm_port_free(port);
        return NULL;
    }
    idm_error_clear(&ignore);
    port->interactive = rt->interactive || port->job_policy != 0;
    bool foreground = port->job_policy == 1 ? true
                    : port->job_policy == 2 ? false
                    : port->interactive && port->stdout_policy == STDIO_INHERIT && port->stderr_policy == STDIO_INHERIT;

    int cap_out[2] = {-1, -1};
    int cap_err[2] = {-1, -1};
    int input_pipe[2] = {-1, -1};
    int null_in = -1;
    int null_out = -1;
    bool wants_pty = port->stdin_policy == STDIO_PTY || port->stdout_policy == STDIO_PTY || port->stderr_policy == STDIO_PTY;
    if (wants_pty && port->stage_count != 1u) {
        idm_error_set(err, idm_span_unknown(NULL), "pty stdio requires a single command");
        idm_port_free(port);
        return NULL;
    }
    if (wants_pty && port->stdout_policy != STDIO_PTY) {
        idm_error_set(err, idm_span_unknown(NULL), "pty stdio requires the pty on stdout");
        idm_port_free(port);
        return NULL;
    }
    if (wants_pty) {
        port->pty_master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        unsigned int pty_index = 0;
        if (port->pty_master >= 0 && grantpt(port->pty_master) == 0 && unlockpt(port->pty_master) == 0
            && ioctl(port->pty_master, TIOCGPTN, &pty_index) == 0) {
            char slave_path[32];
            snprintf(slave_path, sizeof(slave_path), "/dev/pts/%u", pty_index);
            port->pty_slave = idm_strdup(slave_path);
            if (!port->pty_slave) {
                idm_error_oom(err, idm_span_unknown(NULL));
                idm_port_free(port);
                return NULL;
            }
        }
        if (!port->pty_slave) {
            idm_error_set(err, idm_span_unknown(NULL), "pty allocation failed: %s", strerror(errno));
            idm_port_free(port);
            return NULL;
        }
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_col = 80;
        ws.ws_row = 24;
        struct termios tio;
        bool pty_ready = ioctl(port->pty_master, TIOCSWINSZ, &ws) == 0 && tcgetattr(port->pty_master, &tio) == 0;
        if (pty_ready) {
            tio.c_lflag &= ~(tcflag_t)(ECHO | IEXTEN);
            tio.c_lflag |= (tcflag_t)ISIG;
            tio.c_iflag &= ~(tcflag_t)(IXON | IXOFF);
            tio.c_oflag &= ~(tcflag_t)ONLCR;
            tio.c_cc[VERASE] = _POSIX_VDISABLE;
            tio.c_cc[VKILL] = _POSIX_VDISABLE;
            pty_ready = tcsetattr(port->pty_master, TCSANOW, &tio) == 0;
        }
        if (!pty_ready) {
            idm_error_set(err, idm_span_unknown(NULL), "pty setup failed: %s", strerror(errno));
            idm_port_free(port);
            return NULL;
        }
    }
    if (port->stdout_policy == STDIO_PIPE || port->stderr_policy == STDIO_PIPE) {
        bool out_pipe = port->stdout_policy == STDIO_PIPE;
        bool err_pipe = port->stderr_policy == STDIO_PIPE;
        if ((out_pipe && pipe(cap_out) < 0) || (err_pipe && pipe(cap_err) < 0)) {
            int saved = errno;
            if (cap_out[0] >= 0) close(cap_out[0]);
            if (cap_out[1] >= 0) close(cap_out[1]);
            if (cap_err[0] >= 0) close(cap_err[0]);
            if (cap_err[1] >= 0) close(cap_err[1]);
            idm_error_set(err, idm_span_unknown(NULL), "pipe failed: %s", strerror(saved));
            idm_port_free(port);
            return NULL;
        }
    }
    if (port->stdin_policy == STDIO_PIPE) {
        ignore_sigpipe_once();
        if (pipe(input_pipe) < 0) {
            int saved = errno;
            if (cap_out[0] >= 0) close(cap_out[0]);
            if (cap_out[1] >= 0) close(cap_out[1]);
            if (cap_err[0] >= 0) close(cap_err[0]);
            if (cap_err[1] >= 0) close(cap_err[1]);
            idm_error_set(err, idm_span_unknown(NULL), "pipe failed: %s", strerror(saved));
            idm_port_free(port);
            return NULL;
        }
        port->in_fd = input_pipe[1];
    }
    if (port->stdin_policy == STDIO_NULL) {
        null_in = open("/dev/null", O_RDONLY);
        if (null_in < 0) {
            int saved = errno;
            if (cap_out[0] >= 0) close(cap_out[0]);
            if (cap_out[1] >= 0) close(cap_out[1]);
            if (cap_err[0] >= 0) close(cap_err[0]);
            if (cap_err[1] >= 0) close(cap_err[1]);
            if (input_pipe[0] >= 0) close(input_pipe[0]);
            if (input_pipe[1] >= 0) close(input_pipe[1]);
            idm_error_set(err, idm_span_unknown(NULL), "open /dev/null failed: %s", strerror(saved));
            idm_port_free(port);
            return NULL;
        }
    }
    if (port->stdout_policy == STDIO_NULL || port->stderr_policy == STDIO_NULL) {
        null_out = open("/dev/null", O_WRONLY);
        if (null_out < 0) {
            int saved = errno;
            if (cap_out[0] >= 0) close(cap_out[0]);
            if (cap_out[1] >= 0) close(cap_out[1]);
            if (cap_err[0] >= 0) close(cap_err[0]);
            if (cap_err[1] >= 0) close(cap_err[1]);
            if (input_pipe[0] >= 0) close(input_pipe[0]);
            if (input_pipe[1] >= 0) close(input_pipe[1]);
            if (null_in >= 0) close(null_in);
            idm_error_set(err, idm_span_unknown(NULL), "open /dev/null failed: %s", strerror(saved));
            idm_port_free(port);
            return NULL;
        }
    }

    int *sub_fds = NULL;
    size_t sub_fd_count = 0;
    size_t sub_cap = 0;
    for (size_t i = 0; i < port->stage_count; i++) {
        Stage *stage = &port->stages[i];
        for (size_t s = 0; s < stage->sub_count; s++) {
            SubRef *ref = &stage->subs[s];
            IdmPort *sub = idm_port_launch(rt, ref->graph, exec_ctx, err);
            if (!sub) { free(sub_fds); idm_port_free(port); return NULL; }
            int fd = ref->write_dir ? sub->in_fd : sub->out_fd;
            if (fd < 0) {
                idm_error_set(err, idm_span_unknown(NULL), "process substitution graph must pipe %s", ref->write_dir ? "stdin" : "stdout");
                idm_port_free(sub);
                free(sub_fds);
                idm_port_free(port);
                return NULL;
            }
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
            char sub_path[32];
            snprintf(sub_path, sizeof(sub_path), "/dev/fd/%d", fd);
            char *sub_text = idm_strdup(sub_path);
            IdmGrowItem grow_items[2] = {
                { .base = (void **)&port->subs, .elem_size = sizeof(*port->subs) },
                { .base = (void **)&sub_fds, .elem_size = sizeof(*sub_fds) },
            };
            bool grown = idm_growv(grow_items, 2u, &sub_cap, 4u, port->sub_count + 1u);
            if (!sub_text || !grown) {
                free(sub_text);
                idm_port_free(sub);
                free(sub_fds);
                idm_error_oom(err, idm_span_unknown(NULL));
                idm_port_free(port);
                return NULL;
            }
            port->subs[port->sub_count++] = sub;
            sub_fds[sub_fd_count++] = fd;
            if (ref->is_redir) stage->redirs[ref->index].target = sub_text;
            else { free(stage->argv[ref->index]); stage->argv[ref->index] = sub_text; }
        }
    }
    for (size_t i = 0; i < port->stage_count; i++) {
        Stage *stage = &port->stages[i];
        for (size_t r = 0; r < stage->redir_count; r++) {
            Redir *rd = &stage->redirs[r];
            if (rd->kind != REDIR_DATA) continue;
            int mfd = memfd_create("idiom-heredoc", MFD_CLOEXEC);
            if (mfd < 0) {
                idm_error_set(err, idm_span_unknown(NULL), "heredoc memfd failed: %s", strerror(errno));
                free(sub_fds);
                idm_port_free(port);
                return NULL;
            }
            const char *data = rd->target ? rd->target : "";
            size_t len = strlen(data);
            size_t off = 0;
            while (off < len) {
                ssize_t w = write(mfd, data + off, len - off);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    idm_error_set(err, idm_span_unknown(NULL), "heredoc write failed: %s", strerror(errno));
                    close(mfd);
                    free(sub_fds);
                    idm_port_free(port);
                    return NULL;
                }
                off += (size_t)w;
            }
            if (lseek(mfd, 0, SEEK_SET) != 0) {
                idm_error_set(err, idm_span_unknown(NULL), "heredoc seek failed: %s", strerror(errno));
                close(mfd);
                free(sub_fds);
                idm_port_free(port);
                return NULL;
            }
            rd->dup_to = mfd;
        }
    }
    int prev_read = -1;
    bool failed = false;
    for (size_t i = 0; i < port->stage_count && !failed; i++) {
        int stage_pipe[2] = {-1, -1};
        bool last = i + 1u == port->stage_count;
        if (!last) {
            if (pipe(stage_pipe) < 0) { idm_error_set(err, idm_span_unknown(NULL), "pipe failed: %s", strerror(errno)); failed = true; break; }
        }
        StagePlan plan;
        memset(&plan, 0, sizeof(plan));
        plan.pty = port->pty_master >= 0;
        plan.pty_slave = port->pty_slave;
        plan.set_pgid = port->interactive && port->pty_master < 0;
        plan.pgid = i == 0 ? 0 : port->stages[0].pid;
        plan.tty_fd = plan.set_pgid && foreground && g_job_tty >= 0 ? g_job_tty : -1;
        plan.cwd = launch_cwd;
        if (prev_read >= 0) plan.stdin_fd = prev_read;
        else if (port->stdin_policy == STDIO_PIPE && i == 0 && input_pipe[0] >= 0) plan.stdin_fd = input_pipe[0];
        else if (port->stdin_policy == STDIO_NULL && i == 0 && null_in >= 0) plan.stdin_fd = null_in;
        else if (port->stdin_policy == STDIO_PTY && i == 0 && plan.pty) plan.stdin_fd = IDM_STAGE_FD_PTY_SLAVE;
        else plan.stdin_fd = -1;
        if (!last) plan.stdout_fd = stage_pipe[1];
        else if (port->stdout_policy == STDIO_PIPE && cap_out[1] >= 0) plan.stdout_fd = cap_out[1];
        else if (port->stdout_policy == STDIO_NULL && null_out >= 0) plan.stdout_fd = null_out;
        else if (port->stdout_policy == STDIO_PTY && plan.pty) plan.stdout_fd = IDM_STAGE_FD_PTY_SLAVE;
        else plan.stdout_fd = -1;
        if (!last) plan.stderr_fd = -1;
        else if (port->stderr_policy == STDIO_PIPE && cap_err[1] >= 0) plan.stderr_fd = cap_err[1];
        else if (port->stderr_policy == STDIO_NULL && null_out >= 0) plan.stderr_fd = null_out;
        else if (port->stderr_policy == STDIO_PTY && plan.pty) plan.stderr_fd = IDM_STAGE_FD_PTY_SLAVE;
        else plan.stderr_fd = -1;
        int close_candidates[12] = {
            port->pty_master, prev_read, input_pipe[0], input_pipe[1], null_in, null_out,
            stage_pipe[0], stage_pipe[1], cap_out[0], cap_out[1], cap_err[0], cap_err[1],
        };
        for (size_t c = 0; c < 12u; c++) {
            if (close_candidates[c] >= 0) plan.close_fds[plan.close_count++] = close_candidates[c];
        }
        plan.redirs = port->stages[i].redirs;
        plan.redir_count = port->stages[i].redir_count;
        plan.argv = port->stages[i].argv;
        plan.name = port->stages[i].argv[0];
        plan.envp = stage_envp_build(exec_ctx, &port->stages[i]);
        plan.paths = plan.envp ? exec_paths_build(plan.name, plan.envp) : NULL;
        plan.shadow_argv = plan.paths ? shadow_argv_build(plan.argv) : NULL;
        pid_t pid = -1;
        if (!plan.envp || !plan.paths || !plan.shadow_argv) idm_error_oom(err, idm_span_unknown(NULL));
        else pid = stage_clone(&plan, err);
        strv_free(plan.envp);
        strv_free(plan.paths);
        free(plan.shadow_argv);
        if (pid < 0) {
            if (stage_pipe[0] >= 0) {
                close(stage_pipe[0]);
                close(stage_pipe[1]);
            }
            failed = true;
            break;
        }
        port->stages[i].pid = pid;
        port->stages[i].pidfd = open_pidfd(pid);
        if (port->pty_master >= 0) {
            port->pgid = pid;
        } else if (port->interactive) {
            if (i == 0) port->pgid = pid;
            setpgid(pid, port->pgid);
            if (foreground && i == 0) {
                if (g_job_tty >= 0) tcsetpgrp(g_job_tty, port->pgid);
                port->fg = true;
                port->owns_terminal = true;
            }
        }
        if (prev_read >= 0) close(prev_read);
        if (!last) { close(stage_pipe[1]); prev_read = stage_pipe[0]; }
    }
    if (prev_read >= 0) close(prev_read);
    if (input_pipe[0] >= 0) close(input_pipe[0]);
    if (null_in >= 0) close(null_in);
    if (null_out >= 0) close(null_out);
    for (size_t i = 0; i < port->stage_count; i++) {
        for (size_t r = 0; r < port->stages[i].redir_count; r++) {
            Redir *rd = &port->stages[i].redirs[r];
            if (rd->kind == REDIR_DATA && rd->dup_to >= 0) {
                close(rd->dup_to);
                rd->dup_to = -1;
            }
        }
    }
    for (size_t f = 0; f < sub_fd_count; f++) {
        close(sub_fds[f]);
        if (port->subs[f]->in_fd == sub_fds[f]) port->subs[f]->in_fd = -1;
        if (port->subs[f]->out_fd == sub_fds[f]) port->subs[f]->out_fd = -1;
    }
    free(sub_fds);
    if (port->stdout_policy == STDIO_PIPE || port->stderr_policy == STDIO_PIPE) {
        if (cap_out[1] >= 0) close(cap_out[1]);
        if (cap_err[1] >= 0) close(cap_err[1]);
        if (failed) {
            if (cap_out[0] >= 0) close(cap_out[0]);
            if (cap_err[0] >= 0) close(cap_err[0]);
        } else {
            port->out_fd = cap_out[0];
            port->err_fd = cap_err[0];
            if (port->out_fd >= 0) set_nonblocking(port->out_fd);
            if (port->err_fd >= 0) set_nonblocking(port->err_fd);
        }
    }
    if (!failed && port->pty_master >= 0) {
        port->out_fd = fcntl(port->pty_master, F_DUPFD_CLOEXEC, 0);
        if (port->out_fd < 0) {
            idm_error_set(err, idm_span_unknown(NULL), "dup failed: %s", strerror(errno));
            failed = true;
        } else {
            set_nonblocking(port->out_fd);
        }
        if (!failed && port->stdin_policy == STDIO_PTY) {
            port->in_fd = fcntl(port->pty_master, F_DUPFD_CLOEXEC, 0);
            if (port->in_fd < 0) {
                idm_error_set(err, idm_span_unknown(NULL), "dup failed: %s", strerror(errno));
                failed = true;
            }
        }
    }
    if (!failed && port->in_fd >= 0) set_nonblocking(port->in_fd);
    if (failed) {
        port_reap_stages(port);
        if (port->owns_terminal && g_job_tty >= 0 && g_shell_pgid > 0) tcsetpgrp(g_job_tty, g_shell_pgid);
        idm_port_free(port);
        return NULL;
    }
    port->launched = true;
    return port;
}

size_t idm_port_live_fds(const IdmPort *port, int *out_fds, size_t max) {
    if (port->kind == PORT_FILE) return 0;
    size_t n = 0;
    for (size_t i = 0; i < port->stage_count && n < max; i++) {
        if (!port->stages[i].reaped && port->stages[i].pidfd >= 0) {
            out_fds[n++] = port->stages[i].pidfd;
        }
    }
    return n;
}

int idm_port_input_fd(const IdmPort *port) {
    if (port->kind == PORT_FILE) return -1;
    return port->stdin_policy == STDIO_PIPE || port->stdin_policy == STDIO_PTY ? port->in_fd : -1;
}

int idm_port_output_fd(const IdmPort *port, const char *stream) {
    if (port->kind == PORT_FILE) return -1;
    if (strcmp(stream, "stdout") == 0) return port->stdout_policy == STDIO_PIPE || port->stdout_policy == STDIO_PTY ? port->out_fd : -1;
    if (strcmp(stream, "stderr") == 0) return port->stderr_policy == STDIO_PIPE ? port->err_fd : -1;
    return -1;
}

bool idm_port_read(IdmPort *port, const char *stream, size_t max, char **out_data, size_t *out_len, IdmPortIoStatus *out_status, IdmError *err) {
    *out_data = NULL;
    *out_len = 0;
    if (port->kind == PORT_FILE) {
        if (strcmp(stream, "stdout") != 0 || !port->file || !port->file_readable || port->file_closed) {
            *out_status = IDM_PORT_IO_CLOSED;
            return true;
        }
        char *buf = malloc(max);
        if (!buf) return idm_error_oom(err, idm_span_unknown(NULL));
        size_t n;
        for (;;) {
            errno = 0;
            n = fread(buf, 1u, max, port->file);
            if (ferror(port->file) && errno == EINTR) {
                clearerr(port->file);
                if (n == 0) continue;
            }
            break;
        }
        if (n == 0 && ferror(port->file)) {
            int saved = errno;
            free(buf);
            return idm_error_set(err, idm_span_unknown(NULL), "file port read failed: %s", strerror(saved));
        }
        if (n == 0) {
            free(buf);
            *out_status = IDM_PORT_IO_EOF;
            return true;
        }
        *out_data = buf;
        *out_len = n;
        *out_status = IDM_PORT_IO_OK;
        return true;
    }
    int *fd = NULL;
    bool has_stream = false;
    if (strcmp(stream, "stdout") == 0) {
        fd = &port->out_fd;
        has_stream = port->stdout_policy == STDIO_PIPE || port->stdout_policy == STDIO_PTY;
    } else if (strcmp(stream, "stderr") == 0) {
        fd = &port->err_fd;
        has_stream = port->stderr_policy == STDIO_PIPE;
    } else {
        return idm_error_set(err, idm_span_unknown(NULL), "unknown port stream '%s'", stream);
    }
    if (!has_stream) {
        *out_status = IDM_PORT_IO_CLOSED;
        return true;
    }
    if (*fd < 0) {
        *out_status = IDM_PORT_IO_EOF;
        return true;
    }
    char *buf = malloc(max);
    if (!buf) return idm_error_oom(err, idm_span_unknown(NULL));
    for (;;) {
        ssize_t n = read(*fd, buf, max);
        if (n > 0) {
            *out_data = buf;
            *out_len = (size_t)n;
            *out_status = IDM_PORT_IO_OK;
            close_drained_done_stream(port, fd);
            return true;
        }
        if (n == 0) {
            close(*fd);
            *fd = -1;
            free(buf);
            *out_status = IDM_PORT_IO_EOF;
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            free(buf);
            *out_status = IDM_PORT_IO_AGAIN;
            return true;
        }
        if (errno == EIO && port->pty_master >= 0) {
            close(*fd);
            *fd = -1;
            free(buf);
            *out_status = IDM_PORT_IO_EOF;
            return true;
        }
        int saved = errno;
        free(buf);
        return idm_error_set(err, idm_span_unknown(NULL), "port read failed: %s", strerror(saved));
    }
}

bool idm_port_write(IdmPort *port, const char *data, size_t len, size_t *out_written, IdmPortIoStatus *out_status, IdmError *err) {
    *out_written = 0;
    if (port->kind == PORT_FILE) {
        if (!port->file || !port->file_writable || port->file_closed) {
            *out_status = IDM_PORT_IO_CLOSED;
            return true;
        }
        if (len == 0) {
            *out_status = IDM_PORT_IO_OK;
            return true;
        }
        size_t total = 0;
        while (total < len) {
            errno = 0;
            size_t n = fwrite(data + total, 1u, len - total, port->file);
            total += n;
            if (total == len) break;
            if (ferror(port->file) && errno == EINTR) {
                clearerr(port->file);
                continue;
            }
            if (ferror(port->file)) {
                return idm_error_set(err, idm_span_unknown(NULL), "file port write failed: %s", strerror(errno));
            }
            break;
        }
        *out_written = total;
        *out_status = IDM_PORT_IO_OK;
        return true;
    }
    if ((port->stdin_policy != STDIO_PIPE && port->stdin_policy != STDIO_PTY) || port->in_fd < 0) {
        *out_status = IDM_PORT_IO_CLOSED;
        return true;
    }
    if (port->stdin_policy == STDIO_PTY && port->out_fd < 0) {
        close(port->in_fd);
        port->in_fd = -1;
        *out_status = IDM_PORT_IO_CLOSED;
        return true;
    }
    if (len == 0) {
        *out_status = IDM_PORT_IO_OK;
        return true;
    }
    ignore_sigpipe_once();
    for (;;) {
        ssize_t w = write(port->in_fd, data, len);
        if (w >= 0) {
            *out_written = (size_t)w;
            *out_status = IDM_PORT_IO_OK;
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *out_status = IDM_PORT_IO_AGAIN;
            return true;
        }
        if (errno == EPIPE || errno == EBADF || (errno == EIO && port->pty_master >= 0)) {
            close(port->in_fd);
            port->in_fd = -1;
            *out_status = IDM_PORT_IO_CLOSED;
            return true;
        }
        return idm_error_set(err, idm_span_unknown(NULL), "port write failed: %s", strerror(errno));
    }
}

IdmPortIoStatus idm_port_close_input(IdmPort *port) {
    if (port->kind == PORT_FILE) {
        if (!port->file || port->file_closed) return IDM_PORT_IO_CLOSED;
        bool ok = fclose(port->file) == 0;
        port->file = NULL;
        port->file_closed = true;
        return ok ? IDM_PORT_IO_OK : IDM_PORT_IO_CLOSED;
    }
    if ((port->stdin_policy != STDIO_PIPE && port->stdin_policy != STDIO_PTY) || port->in_fd < 0) return IDM_PORT_IO_CLOSED;
    if (port->stdin_policy == STDIO_PTY) {
        struct termios tio;
        int rc = tcgetattr(port->pty_master, &tio);
        IDM_ASSERT_PROVED(rc == 0);
        (void)rc;
        if ((tio.c_lflag & ICANON) && tio.c_cc[VEOF] != _POSIX_VDISABLE) {
            char eofs[2] = {(char)tio.c_cc[VEOF], (char)tio.c_cc[VEOF]};
            while (port->eof_sent < 2) {
                ssize_t w = write(port->in_fd, eofs + port->eof_sent, 2u - (size_t)port->eof_sent);
                if (w > 0) { port->eof_sent += (int)w; continue; }
                if (w < 0 && errno == EINTR) continue;
                if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return IDM_PORT_IO_AGAIN;
                close(port->in_fd);
                port->in_fd = -1;
                return IDM_PORT_IO_CLOSED;
            }
        }
    }
    close(port->in_fd);
    port->in_fd = -1;
    return IDM_PORT_IO_OK;
}

bool idm_port_info_value(IdmRuntime *rt, const IdmPort *port, IdmValue *out, IdmError *err) {
    IdmValue stages = idm_empty_list();
    for (size_t i = port->stage_count; i > 0; i--) {
        const Stage *stage = &port->stages[i - 1u];
        IdmValue argv = idm_empty_list();
        for (size_t j = stage->argc; j > 0; j--) {
            IdmValue text = idm_string(rt, stage->argv[j - 1u] ? stage->argv[j - 1u] : "", err);
            if (err && err->present) return false;
            argv = idm_cons(rt, text, argv, err);
            if (err && err->present) return false;
        }
        IdmValue redirs = idm_empty_list();
        for (size_t j = stage->redir_count; j > 0; j--) {
            const Redir *redir = &stage->redirs[j - 1u];
            IdmDictEntry ritems[2];
            ritems[0].key = idm_atom(rt, "fd");
            ritems[0].value = idm_int((int64_t)redir->fd);
            ritems[1].key = idm_atom(rt, "target");
            ritems[1].value = redir->target ? idm_string(rt, redir->target, err) : idm_nil();
            if (err && err->present) return false;
            IdmValue entry = idm_dict(rt, ritems, 2u, err);
            if (err && err->present) return false;
            redirs = idm_cons(rt, entry, redirs, err);
            if (err && err->present) return false;
        }
        IdmDictEntry items[5];
        items[0].key = idm_atom(rt, "argv");
        items[0].value = argv;
        items[1].key = idm_atom(rt, "redirs");
        items[1].value = redirs;
        items[2].key = idm_atom(rt, "pid");
        items[2].value = stage->pid > 0 ? idm_int((int64_t)stage->pid) : idm_nil();
        items[3].key = idm_atom(rt, "stopped");
        items[3].value = idm_bool(rt, stage->stopped);
        items[4].key = idm_atom(rt, "reaped");
        items[4].value = idm_bool(rt, stage->reaped);
        IdmValue entry = idm_dict(rt, items, 5u, err);
        if (err && err->present) return false;
        stages = idm_cons(rt, entry, stages, err);
        if (err && err->present) return false;
    }
    IdmDictEntry items[5];
    items[0].key = idm_atom(rt, "kind");
    items[0].value = idm_atom(rt, port->kind == PORT_PROCESS ? "process" : "file");
    items[1].key = idm_atom(rt, "pgid");
    items[1].value = port->pgid > 0 ? idm_int((int64_t)port->pgid) : idm_nil();
    items[2].key = idm_atom(rt, "fg");
    items[2].value = idm_bool(rt, port->fg);
    items[3].key = idm_atom(rt, "done");
    items[3].value = idm_bool(rt, port->process_done);
    items[4].key = idm_atom(rt, "stages");
    items[4].value = stages;
    *out = idm_dict(rt, items, 5u, err);
    return !(err && err->present);
}

bool idm_port_stopped(const IdmPort *port) {
    if (port->kind == PORT_FILE) return false;
    bool any = false;
    for (size_t i = 0; i < port->stage_count; i++) {
        if (port->stages[i].reaped) continue;
        if (!port->stages[i].stopped) return false;
        any = true;
    }
    return any;
}

bool idm_port_foreground(const IdmPort *port) {
    if (port->kind == PORT_FILE) return false;
    return port->fg;
}

void idm_port_signal_group(IdmPort *port, int signo) {
    if (port->kind == PORT_FILE) return;
    if (port->pgid > 0 && kill(-port->pgid, signo) == 0) return;
    for (size_t i = 0; i < port->stage_count; i++) {
        if (!port->stages[i].reaped && port->stages[i].pid > 0) kill(port->stages[i].pid, signo);
    }
}

void idm_port_resume(IdmPort *port, bool fg) {
    if (port->kind == PORT_FILE) return;
    for (size_t i = 0; i < port->stage_count; i++) port->stages[i].stopped = false;
    port->fg = fg;
    if (fg && g_job_tty >= 0 && port->pgid > 0) {
        tcsetpgrp(g_job_tty, port->pgid);
        port->owns_terminal = true;
    }
    idm_port_signal_group(port, SIGCONT);
}

bool idm_port_done(const IdmPort *port) {
    if (port->kind == PORT_FILE) return port->file_closed || port->file == NULL;
    for (size_t i = 0; i < port->stage_count; i++) {
        if (!port->stages[i].reaped) return false;
    }
    return true;
}

bool idm_port_io_drained(const IdmPort *port) {
    if (port->kind == PORT_FILE) return port->file == NULL || port->file_closed;
    return port->in_fd < 0 && port->out_fd < 0 && port->err_fd < 0;
}

bool idm_port_stdout_readable(const IdmPort *port) {
    if (port->kind == PORT_FILE) return port->file_readable;
    return port->stdout_policy == STDIO_PIPE || port->stdout_policy == STDIO_PTY;
}

bool idm_port_stderr_readable(const IdmPort *port) {
    if (port->kind == PORT_FILE) return false;
    return port->stderr_policy == STDIO_PIPE;
}

bool idm_port_stderr_on_pty(const IdmPort *port) {
    return port->kind != PORT_FILE && port->stderr_policy == STDIO_PTY;
}

void idm_port_reap(IdmPort *port) {
    if (port->kind == PORT_FILE) return;
    for (size_t i = 0; i < port->sub_count; i++) {
        if (port->subs[i]) idm_port_reap(port->subs[i]);
    }
    int flags = WNOHANG;
    if (port->interactive) flags |= WUNTRACED | WCONTINUED;
    for (size_t i = 0; i < port->stage_count; i++) {
        Stage *stage = &port->stages[i];
        if (stage->reaped) continue;
        int status = 0;
        pid_t r = waitpid(stage->pid, &status, flags);
        if (r == stage->pid) {
            if (WIFSTOPPED(status)) stage->stopped = true;
            else if (WIFCONTINUED(status)) stage->stopped = false;
            else {
                stage->status = status;
                stage->reaped = true;
                if (stage->pidfd >= 0) { close(stage->pidfd); stage->pidfd = -1; }
            }
        } else if (r < 0 && errno == ECHILD) {
            stage->unknown = true;
            stage->reaped = true;
            if (stage->pidfd >= 0) { close(stage->pidfd); stage->pidfd = -1; }
        }
    }
    bool all_reaped = idm_port_done(port);
    if (port->owns_terminal && (all_reaped || idm_port_stopped(port))) {
        if (g_job_tty >= 0 && g_shell_pgid > 0) tcsetpgrp(g_job_tty, g_shell_pgid);
        port->owns_terminal = false;
    }
    if (all_reaped && port->in_fd >= 0) {
        close(port->in_fd);
        port->in_fd = -1;
    }
    if (all_reaped) {
        port->process_done = true;
        close_drained_done_stream(port, &port->out_fd);
        close_drained_done_stream(port, &port->err_fd);
    }
}

static int stage_exit_code(const Stage *stage) {
    if (stage->unknown) return 128;
    int status = stage->status;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

int idm_port_exit_code(IdmPort *port) {
    if (port->kind == PORT_FILE) return 0;
    int final_code = 0;
    bool any_failure = false;
    int rightmost_failure = 0;
    for (size_t i = 0; i < port->stage_count; i++) {
        int code = stage_exit_code(&port->stages[i]);
        if (code != 0) {
            any_failure = true;
            rightmost_failure = code;
        }
        if (i + 1u == port->stage_count) final_code = code;
    }
    bool ok = port->pipefail ? !any_failure : final_code == 0;
    if (ok) return 0;
    if (port->pipefail && final_code == 0) final_code = rightmost_failure;
    return final_code;
}

void idm_port_release_process_state(IdmPort *port) {
    if (!port || port->kind == PORT_FILE) return;
    for (size_t i = 0; i < port->stage_count; i++) stage_destroy(&port->stages[i]);
    free(port->stages);
    port->stages = NULL;
    port->stage_count = 0;
}
