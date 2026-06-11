#include "ish/ports.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum { REDIR_FILE, REDIR_DUP } RedirKind;

typedef struct {
    RedirKind kind;
    int fd;
    char *target;
    int op;
    int dup_to;
} Redir;

typedef struct {
    char **argv;
    size_t argc;
    Redir *redirs;
    size_t redir_count;
    char **env_names;
    char **env_values;
    size_t env_count;
    char **owned_temps;
    size_t owned_temp_count;
    size_t owned_temp_cap;
    pid_t pid;
    int status;
    bool reaped;
} Stage;

static bool stage_own_temp(Stage *stage, const char *path) {
    if (stage->owned_temp_count == stage->owned_temp_cap) {
        size_t cap = stage->owned_temp_cap ? stage->owned_temp_cap * 2u : 4u;
        char **grown = realloc(stage->owned_temps, cap * sizeof(*grown));
        if (!grown) return false;
        stage->owned_temps = grown;
        stage->owned_temp_cap = cap;
    }
    stage->owned_temps[stage->owned_temp_count] = ish_strdup(path);
    if (!stage->owned_temps[stage->owned_temp_count]) return false;
    stage->owned_temp_count++;
    return true;
}

struct IshPort {
    Stage *stages;
    size_t stage_count;
    bool capture;
    bool pipefail;
    int out_fd;
    int err_fd;
    IshBuffer out_buf;
    IshBuffer err_buf;
    bool launched;
    char *launch_error;
    size_t capture_limit;
    bool overflow;
};

static size_t capture_limit_from_env(void) {
    const char *text = getenv("ISH_CAPTURE_LIMIT");
    if (text && *text) {
        char *end = NULL;
        unsigned long long value = strtoull(text, &end, 10);
        if (end != text && value > 0) return (size_t)value;
    }
    return 64u * 1024u * 1024u;
}

static bool value_is_atom(IshValue v, const char *name) {
    return v.tag == ISH_VAL_ATOM && strcmp(ish_symbol_text(v.as.symbol), name) == 0;
}

static bool list_to_array(IshValue list, IshValue **out_items, size_t *out_count) {
    size_t count = 0;
    IshValue cur = list;
    while (ish_is_pair(cur)) {
        count++;
        IshError ignore;
        ish_error_init(&ignore);
        cur = ish_cdr(cur, &ignore);
        ish_error_clear(&ignore);
    }
    IshValue *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    if (count != 0 && !items) return false;
    cur = list;
    for (size_t i = 0; i < count; i++) {
        IshError ignore;
        ish_error_init(&ignore);
        items[i] = ish_car(cur, &ignore);
        cur = ish_cdr(cur, &ignore);
        ish_error_clear(&ignore);
    }
    *out_items = items;
    *out_count = count;
    return true;
}

static char *dup_string_value(IshValue v) {
    if (v.tag != ISH_VAL_STRING) return ish_strdup("");
    return ish_strndup(ish_string_bytes(v), ish_string_length(v));
}

static bool argv_push(char ***argv, size_t *count, size_t *cap, char *item) {
    if (*count == *cap) {
        size_t next = *cap ? *cap * 2u : 8u;
        char **grown = realloc(*argv, next * sizeof(**argv));
        if (!grown) { free(item); return false; }
        *argv = grown;
        *cap = next;
    }
    (*argv)[(*count)++] = item;
    return true;
}

static bool resolve_argv_part(IshValue part, const IshExec *exec_ctx, Stage *stage, char ***argv, size_t *count, size_t *cap) {
    if (!ish_is_tuple(part) || ish_sequence_count(part) < 2) return false;
    IshError ignore;
    ish_error_init(&ignore);
    IshValue tag = ish_sequence_item(part, 0, &ignore);
    IshValue payload = ish_sequence_item(part, 1, &ignore);
    ish_error_clear(&ignore);
    if (value_is_atom(tag, "lit")) {
        return argv_push(argv, count, cap, dup_string_value(payload));
    }
    if (value_is_atom(tag, "temp")) {
        char *path = dup_string_value(payload);
        if (!path) return false;
        if (!stage_own_temp(stage, path)) { free(path); return false; }
        return argv_push(argv, count, cap, path);
    }
    if (value_is_atom(tag, "env")) {
        const char *name = payload.tag == ISH_VAL_STRING ? ish_string_bytes(payload) : "";
        const char *value = ish_exec_env_get(exec_ctx, name);
        if (!value) value = getenv(name);
        return argv_push(argv, count, cap, ish_strdup(value ? value : ""));
    }
    if (value_is_atom(tag, "glob")) {
        char *pattern = dup_string_value(payload);
        if (!pattern) return false;
        glob_t g;
        memset(&g, 0, sizeof(g));
        int rc = glob(pattern, 0, NULL, &g);
        if (rc == 0 && g.gl_pathc > 0) {
            bool ok = true;
            for (size_t i = 0; i < g.gl_pathc && ok; i++) ok = argv_push(argv, count, cap, ish_strdup(g.gl_pathv[i]));
            globfree(&g);
            free(pattern);
            return ok;
        }
        if (rc == GLOB_NOSPACE || rc == GLOB_ABORTED) { globfree(&g); free(pattern); return false; }
        globfree(&g);
        return argv_push(argv, count, cap, pattern);
    }
    return false;
}

static bool parse_stage(IshValue stage_value, const IshExec *exec_ctx, Stage *stage) {
    memset(stage, 0, sizeof(*stage));
    stage->pid = -1;
    if (!ish_is_tuple(stage_value) || ish_sequence_count(stage_value) < 4) return false;
    IshError ignore;
    ish_error_init(&ignore);
    IshValue tag = ish_sequence_item(stage_value, 0, &ignore);
    IshValue argv_list = ish_sequence_item(stage_value, 1, &ignore);
    IshValue redir_list = ish_sequence_item(stage_value, 2, &ignore);
    IshValue env_list = ish_sequence_item(stage_value, 3, &ignore);
    ish_error_clear(&ignore);
    if (!value_is_atom(tag, "stage")) return false;

    IshValue *parts = NULL;
    size_t part_count = 0;
    if (!list_to_array(argv_list, &parts, &part_count)) return false;
    size_t cap = 0;
    for (size_t i = 0; i < part_count; i++) {
        if (!resolve_argv_part(parts[i], exec_ctx, stage, &stage->argv, &stage->argc, &cap)) { free(parts); return false; }
    }
    free(parts);
    if (!argv_push(&stage->argv, &stage->argc, &cap, NULL)) return false;
    stage->argc--;

    IshValue *redirs = NULL;
    size_t redir_count = 0;
    if (!list_to_array(redir_list, &redirs, &redir_count)) return false;
    if (redir_count > 0) {
        stage->redirs = calloc(redir_count, sizeof(*stage->redirs));
        if (!stage->redirs) { free(redirs); return false; }
    }
    for (size_t i = 0; i < redir_count; i++) {
        IshValue r = redirs[i];
        if (!ish_is_tuple(r) || ish_sequence_count(r) < 3) { free(redirs); return false; }
        IshValue rtag = ish_sequence_item(r, 0, &ignore);
        Redir *out = &stage->redirs[stage->redir_count];
        if (value_is_atom(rtag, "redir")) {
            if (ish_sequence_count(r) < 4) { free(redirs); return false; }
            IshValue op = ish_sequence_item(r, 1, &ignore);
            IshValue fd = ish_sequence_item(r, 2, &ignore);
            IshValue target = ish_sequence_item(r, 3, &ignore);
            out->kind = REDIR_FILE;
            out->fd = fd.tag == ISH_VAL_INT ? (int)fd.as.i : 1;
            const char *ops = op.tag == ISH_VAL_ATOM ? ish_symbol_text(op.as.symbol) : ">";
            out->op = strcmp(ops, "<") == 0 ? '<'
                    : strcmp(ops, ">>") == 0 ? 'a'
                    : strcmp(ops, "&>") == 0 ? 'b'
                    : strcmp(ops, "&>>") == 0 ? 'B'
                    : strcmp(ops, "heredoc") == 0 ? 'h'
                    : '>';
            char **tav = NULL;
            size_t tac = 0, tcap = 0;
            if (!resolve_argv_part(target, exec_ctx, stage, &tav, &tac, &tcap) || tac < 1) { free(tav); free(redirs); return false; }
            out->target = tav[0];
            for (size_t j = 1; j < tac; j++) free(tav[j]);
            free(tav);
        } else if (value_is_atom(rtag, "dup")) {
            IshValue a = ish_sequence_item(r, 1, &ignore);
            IshValue b = ish_sequence_item(r, 2, &ignore);
            out->kind = REDIR_DUP;
            out->fd = a.tag == ISH_VAL_INT ? (int)a.as.i : 2;
            out->dup_to = b.tag == ISH_VAL_INT ? (int)b.as.i : 1;
        } else {
            free(redirs);
            return false;
        }
        stage->redir_count++;
    }
    free(redirs);

    IshValue *envs = NULL;
    size_t env_count = 0;
    if (!list_to_array(env_list, &envs, &env_count)) return false;
    if (env_count > 0) {
        stage->env_names = calloc(env_count, sizeof(*stage->env_names));
        stage->env_values = calloc(env_count, sizeof(*stage->env_values));
        if (!stage->env_names || !stage->env_values) { free(envs); return false; }
    }
    for (size_t i = 0; i < env_count; i++) {
        IshValue pair = envs[i];
        if (!ish_is_tuple(pair) || ish_sequence_count(pair) < 2) { free(envs); return false; }
        IshValue name = ish_sequence_item(pair, 0, &ignore);
        IshValue valpart = ish_sequence_item(pair, 1, &ignore);
        char **vav = NULL;
        size_t vac = 0, vcap = 0;
        if (!resolve_argv_part(valpart, exec_ctx, stage, &vav, &vac, &vcap) || vac < 1) { free(vav); free(envs); return false; }
        stage->env_names[stage->env_count] = dup_string_value(name);
        stage->env_values[stage->env_count] = vav[0];
        for (size_t j = 1; j < vac; j++) free(vav[j]);
        free(vav);
        stage->env_count++;
    }
    free(envs);
    return stage->argc >= 1;
}

static void stage_destroy(Stage *stage) {
    for (size_t i = 0; i < stage->owned_temp_count; i++) {
        remove(stage->owned_temps[i]);
        free(stage->owned_temps[i]);
    }
    free(stage->owned_temps);
    for (size_t i = 0; i < stage->argc; i++) free(stage->argv[i]);
    free(stage->argv);
    for (size_t i = 0; i < stage->redir_count; i++) free(stage->redirs[i].target);
    free(stage->redirs);
    for (size_t i = 0; i < stage->env_count; i++) {
        free(stage->env_names[i]);
        free(stage->env_values[i]);
    }
    free(stage->env_names);
    free(stage->env_values);
}

void ish_port_free(IshPort *port) {
    if (!port) return;
    for (size_t i = 0; i < port->stage_count; i++) stage_destroy(&port->stages[i]);
    free(port->stages);
    if (port->out_fd >= 0) close(port->out_fd);
    if (port->err_fd >= 0) close(port->err_fd);
    ish_buf_destroy(&port->out_buf);
    ish_buf_destroy(&port->err_buf);
    free(port->launch_error);
    free(port);
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void child_apply_redirs(const Stage *stage) {
    for (size_t i = 0; i < stage->redir_count; i++) {
        const Redir *r = &stage->redirs[i];
        if (r->kind == REDIR_DUP) {
            if (dup2(r->dup_to, r->fd) < 0) _exit(126);
            continue;
        }
        if (r->op == 'b' || r->op == 'B') {
            int both_flags = r->op == 'B' ? (O_WRONLY | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC);
            int both = open(r->target, both_flags, 0644);
            if (both < 0) _exit(126);
            if (dup2(both, 1) < 0 || dup2(both, 2) < 0) _exit(126);
            close(both);
            continue;
        }
        if (r->op == 'h') {
            char tmpl[] = "/tmp/ish_heredoc_XXXXXX";
            int hf = mkstemp(tmpl);
            if (hf < 0) _exit(126);
            unlink(tmpl);
            size_t blen = strlen(r->target);
            size_t off = 0;
            while (off < blen) {
                ssize_t w = write(hf, r->target + off, blen - off);
                if (w < 0) _exit(126);
                off += (size_t)w;
            }
            if (lseek(hf, 0, SEEK_SET) < 0) _exit(126);
            if (dup2(hf, r->fd) < 0) _exit(126);
            close(hf);
            continue;
        }
        int flags = r->op == '<' ? O_RDONLY : (r->op == 'a' ? (O_WRONLY | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC));
        int opened = open(r->target, flags, 0644);
        if (opened < 0) _exit(126);
        if (dup2(opened, r->fd) < 0) _exit(126);
        close(opened);
    }
}

IshPort *ish_port_launch(IshRuntime *rt, IshValue graph, const IshExec *exec_ctx, IshError *err) {
    (void)rt;
    char saved_cwd[4096];
    bool restore_cwd = false;
    const char *launch_cwd = ish_exec_cwd(exec_ctx);
    if (launch_cwd) {
        if (!getcwd(saved_cwd, sizeof(saved_cwd)) || chdir(launch_cwd) != 0) {
            ish_error_set(err, ish_span_unknown(NULL), "cwd '%s' is not accessible: %s", launch_cwd, strerror(errno));
            return NULL;
        }
        restore_cwd = true;
    }
    IshPort *port = calloc(1u, sizeof(*port));
    if (!port) { ish_error_oom(err, ish_span_unknown(NULL)); return NULL; }
    port->out_fd = -1;
    port->err_fd = -1;
    port->capture_limit = capture_limit_from_env();
    ish_buf_init(&port->out_buf);
    ish_buf_init(&port->err_buf);

    if (!ish_is_tuple(graph) || ish_sequence_count(graph) < 3) { ish_error_set(err, ish_span_unknown(NULL), "invalid command graph"); if (restore_cwd && chdir(saved_cwd) != 0) {} ish_port_free(port); return NULL; }
    IshError ignore;
    ish_error_init(&ignore);
    IshValue tag = ish_sequence_item(graph, 0, &ignore);
    if (value_is_atom(tag, "exec")) {
        IshValue stage = ish_sequence_item(graph, 1, &ignore);
        IshValue cap = ish_sequence_item(graph, 2, &ignore);
        port->capture = value_is_atom(cap, "true");
        port->stage_count = 1;
        port->stages = calloc(1u, sizeof(*port->stages));
        if (!port->stages || !parse_stage(stage, exec_ctx, &port->stages[0])) { ish_error_clear(&ignore); ish_error_set(err, ish_span_unknown(NULL), "invalid command stage"); if (restore_cwd && chdir(saved_cwd) != 0) {} ish_port_free(port); return NULL; }
    } else if (value_is_atom(tag, "pipeline")) {
        IshValue list = ish_sequence_item(graph, 1, &ignore);
        IshValue cap = ish_sequence_item(graph, 2, &ignore);
        IshValue pf = ish_sequence_item(graph, 3, &ignore);
        port->capture = value_is_atom(cap, "true");
        port->pipefail = value_is_atom(pf, "true");
        IshValue *sv = NULL;
        size_t sc = 0;
        if (!list_to_array(list, &sv, &sc) || sc == 0) { ish_error_clear(&ignore); ish_error_set(err, ish_span_unknown(NULL), "empty pipeline"); ish_port_free(port); return NULL; }
        port->stages = calloc(sc, sizeof(*port->stages));
        if (!port->stages) { free(sv); ish_error_clear(&ignore); ish_error_oom(err, ish_span_unknown(NULL)); ish_port_free(port); return NULL; }
        port->stage_count = sc;
        for (size_t i = 0; i < sc; i++) {
            if (!parse_stage(sv[i], exec_ctx, &port->stages[i])) { free(sv); ish_error_clear(&ignore); ish_error_set(err, ish_span_unknown(NULL), "invalid pipeline stage"); if (restore_cwd && chdir(saved_cwd) != 0) {} ish_port_free(port); return NULL; }
        }
        free(sv);
    } else {
        ish_error_clear(&ignore);
        ish_error_set(err, ish_span_unknown(NULL), "unknown command graph kind");
        if (restore_cwd && chdir(saved_cwd) != 0) {}
        ish_port_free(port);
        return NULL;
    }
    ish_error_clear(&ignore);

    int cap_out[2] = {-1, -1};
    int cap_err[2] = {-1, -1};
    if (port->capture) {
        if (pipe(cap_out) < 0 || pipe(cap_err) < 0) { ish_error_set(err, ish_span_unknown(NULL), "pipe failed: %s", strerror(errno)); if (restore_cwd && chdir(saved_cwd) != 0) {} ish_port_free(port); return NULL; }
    }

    int prev_read = -1;
    bool failed = false;
    for (size_t i = 0; i < port->stage_count && !failed; i++) {
        int stage_pipe[2] = {-1, -1};
        bool last = i + 1u == port->stage_count;
        if (!last) {
            if (pipe(stage_pipe) < 0) { ish_error_set(err, ish_span_unknown(NULL), "pipe failed: %s", strerror(errno)); failed = true; break; }
        }
        pid_t pid = fork();
        if (pid < 0) { ish_error_set(err, ish_span_unknown(NULL), "fork failed: %s", strerror(errno)); if (stage_pipe[0] >= 0) { close(stage_pipe[0]); close(stage_pipe[1]); } failed = true; break; }
        if (pid == 0) {
            if (prev_read >= 0) { dup2(prev_read, 0); }
            if (!last) { dup2(stage_pipe[1], 1); }
            else if (port->capture) { dup2(cap_out[1], 1); dup2(cap_err[1], 2); }
            if (prev_read >= 0) close(prev_read);
            if (stage_pipe[0] >= 0) close(stage_pipe[0]);
            if (stage_pipe[1] >= 0) close(stage_pipe[1]);
            if (cap_out[0] >= 0) close(cap_out[0]);
            if (cap_out[1] >= 0) close(cap_out[1]);
            if (cap_err[0] >= 0) close(cap_err[0]);
            if (cap_err[1] >= 0) close(cap_err[1]);
            child_apply_redirs(&port->stages[i]);
            for (size_t e = 0; e < ish_exec_env_count(exec_ctx); e++) {
                const char *ename = NULL;
                const char *evalue = NULL;
                if (ish_exec_env_entry(exec_ctx, e, &ename, &evalue)) setenv(ename, evalue, 1);
            }
            for (size_t e = 0; e < port->stages[i].env_count; e++) setenv(port->stages[i].env_names[e], port->stages[i].env_values[e], 1);
            execvp(port->stages[i].argv[0], port->stages[i].argv);
            _exit(127);
        }
        port->stages[i].pid = pid;
        if (prev_read >= 0) close(prev_read);
        if (!last) { close(stage_pipe[1]); prev_read = stage_pipe[0]; }
    }
    if (prev_read >= 0) close(prev_read);
    if (port->capture) {
        close(cap_out[1]);
        close(cap_err[1]);
        if (failed) { close(cap_out[0]); close(cap_err[0]); }
        else {
            port->out_fd = cap_out[0];
            port->err_fd = cap_err[0];
            set_nonblocking(port->out_fd);
            set_nonblocking(port->err_fd);
        }
    }
    if (restore_cwd && chdir(saved_cwd) != 0) {
        ish_error_set(err, ish_span_unknown(NULL), "failed to restore working directory: %s", strerror(errno));
        failed = true;
    }
    if (failed) {
        for (size_t i = 0; i < port->stage_count; i++) {
            if (port->stages[i].pid > 0) { kill(port->stages[i].pid, SIGKILL); waitpid(port->stages[i].pid, NULL, 0); port->stages[i].reaped = true; }
        }
        ish_port_free(port);
        return NULL;
    }
    port->launched = true;
    return port;
}

size_t ish_port_live_fds(const IshPort *port, int *out_fds, size_t max) {
    size_t n = 0;
    if (port->out_fd >= 0 && n < max) out_fds[n++] = port->out_fd;
    if (port->err_fd >= 0 && n < max) out_fds[n++] = port->err_fd;
    return n;
}

static void drain_fd(IshPort *port, int *fd, IshBuffer *buf) {
    if (*fd < 0) return;
    char tmp[4096];
    for (;;) {
        ssize_t r = read(*fd, tmp, sizeof(tmp));
        if (r > 0) {
            size_t room = buf->len < port->capture_limit ? port->capture_limit - buf->len : 0;
            size_t take = (size_t)r < room ? (size_t)r : room;
            if (take > 0) ish_buf_append_n(buf, tmp, take);
            if ((size_t)r > room) port->overflow = true;
            continue;
        }
        if (r == 0) { close(*fd); *fd = -1; return; }
        if (errno == EINTR) continue;
        return;
    }
}

void ish_port_drain(IshPort *port) {
    drain_fd(port, &port->out_fd, &port->out_buf);
    drain_fd(port, &port->err_fd, &port->err_buf);
}

bool ish_port_try_complete(IshPort *port) {
    ish_port_drain(port);
    bool all_reaped = true;
    for (size_t i = 0; i < port->stage_count; i++) {
        if (port->stages[i].reaped) continue;
        int status = 0;
        pid_t r = waitpid(port->stages[i].pid, &status, WNOHANG);
        if (r == port->stages[i].pid) { port->stages[i].status = status; port->stages[i].reaped = true; }
        else if (r == 0) all_reaped = false;
        else if (r < 0 && errno == ECHILD) { port->stages[i].status = 0; port->stages[i].reaped = true; }
        else all_reaped = false;
    }
    bool fds_open = port->out_fd >= 0 || port->err_fd >= 0;
    return all_reaped && !fds_open;
}

static int stage_exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

IshValue ish_port_result(IshPort *port, IshRuntime *rt, IshError *err) {
    int final_code = 0;
    bool any_failure = false;
    for (size_t i = 0; i < port->stage_count; i++) {
        int code = stage_exit_code(port->stages[i].status);
        if (code != 0) any_failure = true;
        if (i + 1u == port->stage_count) final_code = code;
    }
    bool ok = port->pipefail ? !any_failure : final_code == 0;
    IshValue out_str = ish_string_n(rt, port->out_buf.data ? port->out_buf.data : "", port->out_buf.len, err);
    if (err->present) return ish_nil();
    IshValue err_str = ish_string_n(rt, port->err_buf.data ? port->err_buf.data : "", port->err_buf.len, err);
    if (err->present) return ish_nil();
    IshValue items[4];
    if (port->overflow) {
        items[0] = ish_atom(rt, "error");
        items[1] = ish_atom(rt, "capture-overflow");
    } else {
        items[0] = ish_atom(rt, ok ? "ok" : "error");
        items[1] = ish_int(final_code);
    }
    items[2] = out_str;
    items[3] = err_str;
    return ish_tuple(rt, items, 4u, err);
}
