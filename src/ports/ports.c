#include "idiom/ports.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
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
    bool stopped;
} Stage;

typedef enum {
    STDIO_INHERIT,
    STDIO_PIPE,
    STDIO_NULL
} StdioPolicy;

typedef enum {
    PORT_PROCESS,
    PORT_FILE
} PortKind;

static bool stage_own_temp(Stage *stage, const char *path) {
    if (stage->owned_temp_count == stage->owned_temp_cap) {
        size_t cap = stage->owned_temp_cap ? stage->owned_temp_cap * 2u : 4u;
        char **grown = realloc(stage->owned_temps, cap * sizeof(*grown));
        if (!grown) return false;
        stage->owned_temps = grown;
        stage->owned_temp_cap = cap;
    }
    stage->owned_temps[stage->owned_temp_count] = idm_strdup(path);
    if (!stage->owned_temps[stage->owned_temp_count]) return false;
    stage->owned_temp_count++;
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
    IdmBuffer out_buf;
    IdmBuffer err_buf;
    bool launched;
    char *launch_error;
    size_t capture_limit;
    bool overflow;
    bool interactive;
    bool fg;
    bool owns_terminal;
    pid_t pgid;
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

static size_t capture_limit_from_env(void) {
    const char *text = getenv("IDM_CAPTURE_LIMIT");
    if (text && *text) {
        char *end = NULL;
        unsigned long long value = strtoull(text, &end, 10);
        if (end != text && value > 0) return (size_t)value;
    }
    return 64u * 1024u * 1024u;
}

static bool value_is_atom(IdmValue v, const char *name) {
    return v.tag == IDM_VAL_ATOM && strcmp(idm_symbol_text(v.as.symbol), name) == 0;
}

static bool parse_stdio_policy(IdmValue v, StdioPolicy *out) {
    if (value_is_atom(v, "inherit")) { *out = STDIO_INHERIT; return true; }
    if (value_is_atom(v, "pipe") || value_is_atom(v, "capture")) { *out = STDIO_PIPE; return true; }
    if (value_is_atom(v, "null")) { *out = STDIO_NULL; return true; }
    return false;
}

static bool parse_stdio_tuple(IdmValue v, StdioPolicy *in, StdioPolicy *out, StdioPolicy *err) {
    if (!idm_is_tuple(v) || idm_sequence_count(v) < 4) return false;
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

static void parse_legacy_capture(IdmValue cap, StdioPolicy *in, StdioPolicy *out, StdioPolicy *err) {
    bool capture = value_is_atom(cap, "true");
    *in = STDIO_INHERIT;
    *out = capture ? STDIO_PIPE : STDIO_INHERIT;
    *err = capture ? STDIO_PIPE : STDIO_INHERIT;
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
    if (v.tag != IDM_VAL_STRING) return idm_strdup("");
    return idm_strndup(idm_string_bytes(v), idm_string_length(v));
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

static bool part_append_text(IdmValue part, const IdmExec *exec_ctx, Stage *stage, IdmBuffer *out, bool *is_glob) {
    if (!idm_is_tuple(part) || idm_sequence_count(part) < 2) return false;
    IdmError ignore;
    idm_error_init(&ignore);
    IdmValue tag = idm_sequence_item(part, 0, &ignore);
    IdmValue payload = idm_sequence_item(part, 1, &ignore);
    idm_error_clear(&ignore);
    if (value_is_atom(tag, "lit") || value_is_atom(tag, "glob") || value_is_atom(tag, "temp")) {
        if (value_is_atom(tag, "glob")) *is_glob = true;
        const char *text = payload.tag == IDM_VAL_STRING ? idm_string_bytes(payload) : "";
        size_t len = payload.tag == IDM_VAL_STRING ? idm_string_length(payload) : 0;
        if (value_is_atom(tag, "temp") && !stage_own_temp(stage, text)) return false;
        return idm_buf_append_n(out, text, len);
    }
    if (value_is_atom(tag, "env")) {
        const char *name = payload.tag == IDM_VAL_STRING ? idm_string_bytes(payload) : "";
        const char *value = idm_exec_env_get(exec_ctx, name);
        if (!value) value = getenv(name);
        return idm_buf_append(out, value ? value : "");
    }
    if (value_is_atom(tag, "cat")) {
        IdmValue cur = payload;
        while (idm_is_pair(cur)) {
            idm_error_init(&ignore);
            IdmValue sub = idm_car(cur, &ignore);
            cur = idm_cdr(cur, &ignore);
            idm_error_clear(&ignore);
            if (!part_append_text(sub, exec_ctx, stage, out, is_glob)) return false;
        }
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
        for (size_t i = 0; i < g.gl_pathc && ok; i++) ok = argv_push(argv, count, cap, idm_strdup(g.gl_pathv[i] + skip));
        globfree(&g);
        free(pattern);
        return ok;
    }
    if (rc == GLOB_NOSPACE || rc == GLOB_ABORTED) { globfree(&g); free(pattern); return false; }
    globfree(&g);
    return argv_push(argv, count, cap, pattern);
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
    return argv_push(argv, count, cap, word);
}

static bool parse_stage(IdmValue stage_value, const IdmExec *exec_ctx, Stage *stage) {
    memset(stage, 0, sizeof(*stage));
    stage->pid = -1;
    if (!idm_is_tuple(stage_value) || idm_sequence_count(stage_value) < 4) return false;
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
        if (!resolve_argv_part(parts[i], exec_ctx, stage, &stage->argv, &stage->argc, &cap)) { free(parts); return false; }
    }
    free(parts);
    if (!argv_push(&stage->argv, &stage->argc, &cap, NULL)) return false;
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
        if (!idm_is_tuple(r) || idm_sequence_count(r) < 3) { free(redirs); return false; }
        IdmValue rtag = idm_sequence_item(r, 0, &ignore);
        Redir *out = &stage->redirs[stage->redir_count];
        if (value_is_atom(rtag, "redir")) {
            if (idm_sequence_count(r) < 4) { free(redirs); return false; }
            IdmValue op = idm_sequence_item(r, 1, &ignore);
            IdmValue fd = idm_sequence_item(r, 2, &ignore);
            IdmValue target = idm_sequence_item(r, 3, &ignore);
            out->kind = REDIR_FILE;
            out->fd = fd.tag == IDM_VAL_INT ? (int)fd.as.i : 1;
            const char *ops = op.tag == IDM_VAL_ATOM ? idm_symbol_text(op.as.symbol) : ">";
            out->op = strcmp(ops, "<") == 0 ? '<'
                    : strcmp(ops, ">>") == 0 ? 'a'
                    : strcmp(ops, "&>") == 0 ? 'b'
                    : strcmp(ops, "&>>") == 0 ? 'B'
                    : '>';
            char **tav = NULL;
            size_t tac = 0, tcap = 0;
            if (!resolve_argv_part(target, exec_ctx, stage, &tav, &tac, &tcap) || tac < 1) { free(tav); free(redirs); return false; }
            out->target = tav[0];
            for (size_t j = 1; j < tac; j++) free(tav[j]);
            free(tav);
        } else if (value_is_atom(rtag, "dup")) {
            IdmValue a = idm_sequence_item(r, 1, &ignore);
            IdmValue b = idm_sequence_item(r, 2, &ignore);
            out->kind = REDIR_DUP;
            out->fd = a.tag == IDM_VAL_INT ? (int)a.as.i : 2;
            out->dup_to = b.tag == IDM_VAL_INT ? (int)b.as.i : 1;
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
        if (!idm_is_tuple(pair) || idm_sequence_count(pair) < 2) { free(envs); return false; }
        IdmValue name = idm_sequence_item(pair, 0, &ignore);
        IdmValue valpart = idm_sequence_item(pair, 1, &ignore);
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

static void port_reap_stages(IdmPort *port) {
    for (size_t i = 0; i < port->stage_count; i++) {
        Stage *stage = &port->stages[i];
        if (stage->reaped || stage->pid <= 0) continue;
        kill(stage->pid, SIGKILL);
        waitpid(stage->pid, NULL, 0);
        stage->reaped = true;
    }
}

void idm_port_free(IdmPort *port) {
    if (!port) return;
    port_reap_stages(port);
    if (port->file) fclose(port->file);
    for (size_t i = 0; i < port->stage_count; i++) stage_destroy(&port->stages[i]);
    free(port->stages);
    if (port->in_fd >= 0) close(port->in_fd);
    if (port->out_fd >= 0) close(port->out_fd);
    if (port->err_fd >= 0) close(port->err_fd);
    idm_buf_destroy(&port->out_buf);
    idm_buf_destroy(&port->err_buf);
    free(port->launch_error);
    free(port);
}

IdmPort *idm_port_open_file(const char *path, const char *mode, bool readable, bool writable, IdmError *err) {
    FILE *file = fopen(path, mode);
    if (!file) return NULL;
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
    return port;
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void child_fail(const char *subject, const char *what) {
    dprintf(2, "idiom: %s: %s: %s\n", subject, what, strerror(errno));
    _exit(126);
}

static void child_apply_redirs(const Stage *stage) {
    for (size_t i = 0; i < stage->redir_count; i++) {
        const Redir *r = &stage->redirs[i];
        if (r->kind == REDIR_DUP) {
            if (dup2(r->dup_to, r->fd) < 0) child_fail("fd", "redirect failed");
            continue;
        }
        if (r->op == 'b' || r->op == 'B') {
            int both_flags = r->op == 'B' ? (O_WRONLY | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC);
            int both = open(r->target, both_flags, 0644);
            if (both < 0) child_fail(r->target, "redirect failed");
            if (dup2(both, 1) < 0 || dup2(both, 2) < 0) child_fail(r->target, "redirect failed");
            close(both);
            continue;
        }
        int flags = r->op == '<' ? O_RDONLY : (r->op == 'a' ? (O_WRONLY | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC));
        int opened = open(r->target, flags, 0644);
        if (opened < 0) child_fail(r->target, "redirect failed");
        if (dup2(opened, r->fd) < 0) child_fail(r->target, "redirect failed");
        close(opened);
    }
}

IdmPort *idm_port_launch(IdmRuntime *rt, IdmValue graph, const IdmExec *exec_ctx, IdmError *err) {
    const char *launch_cwd = idm_exec_cwd(exec_ctx);
    IdmPort *port = calloc(1u, sizeof(*port));
    if (!port) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    port->kind = PORT_PROCESS;
    port->in_fd = -1;
    port->out_fd = -1;
    port->err_fd = -1;
    port->stdin_policy = STDIO_INHERIT;
    port->stdout_policy = STDIO_INHERIT;
    port->stderr_policy = STDIO_INHERIT;
    port->capture_limit = capture_limit_from_env();
    idm_buf_init(&port->out_buf);
    idm_buf_init(&port->err_buf);

    if (!idm_is_tuple(graph) || idm_sequence_count(graph) < 3) { idm_error_set(err, idm_span_unknown(NULL), "invalid command graph"); idm_port_free(port); return NULL; }
    IdmError ignore;
    idm_error_init(&ignore);
    IdmValue tag = idm_sequence_item(graph, 0, &ignore);
    if (value_is_atom(tag, "exec")) {
        IdmValue stage = idm_sequence_item(graph, 1, &ignore);
        IdmValue io = idm_sequence_item(graph, 2, &ignore);
        if (!parse_stdio_tuple(io, &port->stdin_policy, &port->stdout_policy, &port->stderr_policy)) {
            parse_legacy_capture(io, &port->stdin_policy, &port->stdout_policy, &port->stderr_policy);
        }
        port->stage_count = 1;
        port->stages = calloc(1u, sizeof(*port->stages));
        if (!port->stages || !parse_stage(stage, exec_ctx, &port->stages[0])) { idm_error_clear(&ignore); idm_error_set(err, idm_span_unknown(NULL), "invalid command stage"); idm_port_free(port); return NULL; }
    } else if (value_is_atom(tag, "pipeline")) {
        IdmValue list = idm_sequence_item(graph, 1, &ignore);
        IdmValue io = idm_sequence_item(graph, 2, &ignore);
        IdmValue pf = idm_sequence_item(graph, 3, &ignore);
        if (!parse_stdio_tuple(io, &port->stdin_policy, &port->stdout_policy, &port->stderr_policy)) {
            parse_legacy_capture(io, &port->stdin_policy, &port->stdout_policy, &port->stderr_policy);
        }
        port->pipefail = value_is_atom(pf, "true");
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
    port->interactive = rt->interactive;
    bool foreground = port->interactive && port->stdout_policy == STDIO_INHERIT && port->stderr_policy == STDIO_INHERIT;

    int cap_out[2] = {-1, -1};
    int cap_err[2] = {-1, -1};
    int input_pipe[2] = {-1, -1};
    int null_in = -1;
    int null_out = -1;
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

    int prev_read = -1;
    bool failed = false;
    for (size_t i = 0; i < port->stage_count && !failed; i++) {
        int stage_pipe[2] = {-1, -1};
        bool last = i + 1u == port->stage_count;
        if (!last) {
            if (pipe(stage_pipe) < 0) { idm_error_set(err, idm_span_unknown(NULL), "pipe failed: %s", strerror(errno)); failed = true; break; }
        }
        pid_t pid = fork();
        if (pid < 0) { idm_error_set(err, idm_span_unknown(NULL), "fork failed: %s", strerror(errno)); if (stage_pipe[0] >= 0) { close(stage_pipe[0]); close(stage_pipe[1]); } failed = true; break; }
        if (pid == 0) {
            if (port->interactive) {
                pid_t grp = i == 0 ? getpid() : port->stages[0].pid;
                setpgid(0, grp);
                if (foreground && g_job_tty >= 0) tcsetpgrp(g_job_tty, grp);
                signal(SIGTSTP, SIG_DFL);
                signal(SIGTTOU, SIG_DFL);
                signal(SIGTTIN, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);
            }
            if (launch_cwd && chdir(launch_cwd) != 0) child_fail(launch_cwd, "chdir failed");
            if (prev_read >= 0) { dup2(prev_read, 0); }
            else if (port->stdin_policy == STDIO_PIPE && i == 0 && input_pipe[0] >= 0) { dup2(input_pipe[0], 0); }
            else if (port->stdin_policy == STDIO_NULL && i == 0 && null_in >= 0) { dup2(null_in, 0); }
            if (!last) { dup2(stage_pipe[1], 1); }
            else {
                if (port->stdout_policy == STDIO_PIPE && cap_out[1] >= 0) dup2(cap_out[1], 1);
                else if (port->stdout_policy == STDIO_NULL && null_out >= 0) dup2(null_out, 1);
                if (port->stderr_policy == STDIO_PIPE && cap_err[1] >= 0) dup2(cap_err[1], 2);
                else if (port->stderr_policy == STDIO_NULL && null_out >= 0) dup2(null_out, 2);
            }
            if (prev_read >= 0) close(prev_read);
            if (input_pipe[0] >= 0) close(input_pipe[0]);
            if (input_pipe[1] >= 0) close(input_pipe[1]);
            if (null_in >= 0) close(null_in);
            if (null_out >= 0) close(null_out);
            if (stage_pipe[0] >= 0) close(stage_pipe[0]);
            if (stage_pipe[1] >= 0) close(stage_pipe[1]);
            if (cap_out[0] >= 0) close(cap_out[0]);
            if (cap_out[1] >= 0) close(cap_out[1]);
            if (cap_err[0] >= 0) close(cap_err[0]);
            if (cap_err[1] >= 0) close(cap_err[1]);
            child_apply_redirs(&port->stages[i]);
            for (size_t e = 0; e < idm_exec_env_count(exec_ctx); e++) {
                const char *ename = NULL;
                const char *evalue = NULL;
                if (idm_exec_env_entry(exec_ctx, e, &ename, &evalue)) setenv(ename, evalue, 1);
            }
            for (size_t e = 0; e < port->stages[i].env_count; e++) setenv(port->stages[i].env_names[e], port->stages[i].env_values[e], 1);
            execvp(port->stages[i].argv[0], port->stages[i].argv);
            if (errno == ENOENT) {
                dprintf(2, "idiom: %s: command not found\n", port->stages[i].argv[0]);
                _exit(127);
            }
            dprintf(2, "idiom: %s: %s\n", port->stages[i].argv[0], strerror(errno));
            _exit(126);
        }
        port->stages[i].pid = pid;
        if (port->interactive) {
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
    if (port->out_fd >= 0 && n < max) out_fds[n++] = port->out_fd;
    if (port->err_fd >= 0 && n < max) out_fds[n++] = port->err_fd;
    return n;
}

int idm_port_input_fd(const IdmPort *port) {
    if (port->kind == PORT_FILE) return -1;
    return port->stdin_policy == STDIO_PIPE ? port->in_fd : -1;
}

static void forward_bytes(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        off += (size_t)w;
    }
}

static void drain_fd(IdmPort *port, int *fd, IdmBuffer *buf, int forward_to) {
    if (*fd < 0) return;
    char tmp[4096];
    for (;;) {
        ssize_t r = read(*fd, tmp, sizeof(tmp));
        if (r > 0) {
            if (forward_to >= 0) forward_bytes(forward_to, tmp, (size_t)r);
            size_t room = buf->len < port->capture_limit ? port->capture_limit - buf->len : 0;
            size_t take = (size_t)r < room ? (size_t)r : room;
            if (take > 0) idm_buf_append_n(buf, tmp, take);
            if ((size_t)r > room) port->overflow = true;
            continue;
        }
        if (r == 0) { close(*fd); *fd = -1; return; }
        if (errno == EINTR) continue;
        return;
    }
}

void idm_port_drain(IdmPort *port) {
    if (port->kind == PORT_FILE) return;
    drain_fd(port, &port->out_fd, &port->out_buf, -1);
    drain_fd(port, &port->err_fd, &port->err_buf, 2);
}

static bool take_buffer_prefix(IdmBuffer *buf, size_t max, char **out_data, size_t *out_len, IdmError *err) {
    size_t take = buf->len < max ? buf->len : max;
    char *copy = idm_strndup(buf->data ? buf->data : "", take);
    if (!copy) return idm_error_oom(err, idm_span_unknown(NULL));
    if (take < buf->len) {
        memmove(buf->data, buf->data + take, buf->len - take);
        buf->len -= take;
        buf->data[buf->len] = '\0';
    } else {
        buf->len = 0;
        if (buf->data) buf->data[0] = '\0';
    }
    *out_data = copy;
    *out_len = take;
    return true;
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
        size_t n = fread(buf, 1u, max, port->file);
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
    IdmBuffer *buf = NULL;
    int *fd = NULL;
    bool has_stream = false;
    if (strcmp(stream, "stdout") == 0) {
        buf = &port->out_buf;
        fd = &port->out_fd;
        has_stream = port->stdout_policy == STDIO_PIPE;
    } else if (strcmp(stream, "stderr") == 0) {
        buf = &port->err_buf;
        fd = &port->err_fd;
        has_stream = port->stderr_policy == STDIO_PIPE;
    } else {
        return idm_error_set(err, idm_span_unknown(NULL), "unknown port stream '%s'", stream);
    }
    idm_port_drain(port);
    if (buf->len == 0) {
        *out_status = !has_stream ? IDM_PORT_IO_CLOSED : (*fd >= 0 ? IDM_PORT_IO_AGAIN : IDM_PORT_IO_EOF);
        return true;
    }
    if (!take_buffer_prefix(buf, max, out_data, out_len, err)) return false;
    *out_status = IDM_PORT_IO_OK;
    return true;
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
        size_t n = fwrite(data, 1u, len, port->file);
        if (n < len && ferror(port->file)) {
            return idm_error_set(err, idm_span_unknown(NULL), "file port write failed: %s", strerror(errno));
        }
        if (fflush(port->file) != 0) {
            return idm_error_set(err, idm_span_unknown(NULL), "file port flush failed: %s", strerror(errno));
        }
        *out_written = n;
        *out_status = IDM_PORT_IO_OK;
        return true;
    }
    if (port->stdin_policy != STDIO_PIPE || port->in_fd < 0) {
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
        if (errno == EPIPE || errno == EBADF) {
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
    if (port->stdin_policy != STDIO_PIPE || port->in_fd < 0) return IDM_PORT_IO_CLOSED;
    close(port->in_fd);
    port->in_fd = -1;
    return IDM_PORT_IO_OK;
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
    if (port->pgid > 0) {
        kill(-port->pgid, signo);
        return;
    }
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

bool idm_port_try_complete(IdmPort *port) {
    if (port->kind == PORT_FILE) return port->file_closed || port->file == NULL;
    idm_port_drain(port);
    bool all_reaped = true;
    int flags = WNOHANG;
    if (port->interactive) flags |= WUNTRACED | WCONTINUED;
    for (size_t i = 0; i < port->stage_count; i++) {
        Stage *stage = &port->stages[i];
        if (stage->reaped) continue;
        int status = 0;
        pid_t r = waitpid(stage->pid, &status, flags);
        if (r == stage->pid) {
            if (WIFSTOPPED(status)) { stage->stopped = true; all_reaped = false; }
            else if (WIFCONTINUED(status)) { stage->stopped = false; all_reaped = false; }
            else { stage->status = status; stage->reaped = true; }
        }
        else if (r < 0 && errno == ECHILD) { stage->status = 0; stage->reaped = true; }
        else all_reaped = false;
    }
    if (port->owns_terminal && (all_reaped || idm_port_stopped(port))) {
        if (g_job_tty >= 0 && g_shell_pgid > 0) tcsetpgrp(g_job_tty, g_shell_pgid);
        port->owns_terminal = false;
    }
    if (all_reaped && port->in_fd >= 0) {
        close(port->in_fd);
        port->in_fd = -1;
    }
    bool fds_open = port->out_fd >= 0 || port->err_fd >= 0;
    return all_reaped && !fds_open;
}

static int stage_exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

IdmValue idm_port_result(IdmPort *port, IdmRuntime *rt, IdmError *err) {
    if (port->kind == PORT_FILE) {
        IdmValue empty = idm_string_n(rt, "", 0, err);
        if (err->present) return idm_nil();
        IdmValue items[4] = { idm_atom(rt, "ok"), idm_int(0), empty, empty };
        return idm_tuple(rt, items, 4u, err);
    }
    int final_code = 0;
    bool any_failure = false;
    int rightmost_failure = 0;
    for (size_t i = 0; i < port->stage_count; i++) {
        int code = stage_exit_code(port->stages[i].status);
        if (code != 0) {
            any_failure = true;
            rightmost_failure = code;
        }
        if (i + 1u == port->stage_count) final_code = code;
    }
    bool ok = port->pipefail ? !any_failure : final_code == 0;
    if (!ok && port->pipefail && final_code == 0) final_code = rightmost_failure;
    IdmValue out_str = idm_string_n(rt, port->out_buf.data ? port->out_buf.data : "", port->out_buf.len, err);
    if (err->present) return idm_nil();
    IdmValue err_str = idm_string_n(rt, port->err_buf.data ? port->err_buf.data : "", port->err_buf.len, err);
    if (err->present) return idm_nil();
    IdmValue items[4];
    if (port->overflow) {
        items[0] = idm_atom(rt, "error");
        items[1] = idm_atom(rt, "capture-overflow");
    } else {
        items[0] = idm_atom(rt, ok ? "ok" : "error");
        items[1] = idm_int(final_code);
    }
    items[2] = out_str;
    items[3] = err_str;
    return idm_tuple(rt, items, 4u, err);
}
