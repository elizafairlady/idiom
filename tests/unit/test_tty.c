#include "test_util.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>

typedef struct {
    int saved_stdin;
    int write_fd;
} StdinPipe;

static bool stdin_pipe_install(StdinPipe *sp) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    sp->saved_stdin = dup(STDIN_FILENO);
    if (sp->saved_stdin < 0 || dup2(fds[0], STDIN_FILENO) < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    close(fds[0]);
    sp->write_fd = fds[1];
    return true;
}

static void stdin_pipe_remove(StdinPipe *sp) {
    if (sp->write_fd >= 0) close(sp->write_fd);
    dup2(sp->saved_stdin, STDIN_FILENO);
    close(sp->saved_stdin);
}

static void check_repl_value(IdmRepl *repl, const char *source, const char *expect) {
    IdmError err;
    idm_error_init(&err);
    IdmValue thunk = idm_nil();
    uint64_t token = 0;
    IdmValue out = idm_nil();
    IdmBuffer full;
    idm_buf_init(&full);
    bool built = idm_buf_append(&full, "use std/term with [tty? tty-raw! tty-restore! tty-read tty-read-line]\n") &&
                 idm_buf_append(&full, source);
    bool ok = built && idm_repl_compile(repl, full.data ? full.data : "", &thunk, &token, &err) == IDM_REPL_OK && idm_repl_run(repl, thunk, &out, &err);
    if (!ok) {
        fprintf(stderr, "tty eval failed for '%s': %s\n", source, err.message ? err.message : "?");
        failures++;
        idm_buf_destroy(&full);
        idm_error_clear(&err);
        return;
    }
    IdmBuffer buf;
    idm_buf_init(&buf);
    CHECK(idm_value_write(&buf, out));
    CHECK_STR(buf.data ? buf.data : "", expect);
    idm_buf_destroy(&buf);
    idm_buf_destroy(&full);
    idm_error_clear(&err);
}

static IdmRepl *tty_repl_create(IdmRuntime *rt) {
    IdmError err;
    idm_error_init(&err);
    IdmRepl *repl = idm_repl_create(rt, &err);
    CHECK(repl != NULL);
    idm_error_clear(&err);
    return repl;
}

static void test_tty_prims_on_pipe(void) {
    StdinPipe sp;
    if (!stdin_pipe_install(&sp)) {
        failures++;
        return;
    }
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmRepl *repl = tty_repl_create(&rt);
    if (repl) {
        check_repl_value(repl, "tty?", ":false");
        check_repl_value(repl, "try do tty-raw!; rescue r -> r end", "{:error {:tty :raw :enotty}}");
        check_repl_value(repl, "tty-restore!", ":ok");
        CHECK(write(sp.write_fd, "ab", 2u) == 2);
        check_repl_value(repl, "tty-read :nil", "{:byte 97}");
        check_repl_value(repl, "tty-read 0", "{:byte 98}");
        check_repl_value(repl, "tty-read 30", ":timeout");
        CHECK(write(sp.write_fd, "one\ntwo", 7u) == 7);
        check_repl_value(repl, "tty-read-line", "{:line \"one\"}");
        close(sp.write_fd);
        sp.write_fd = -1;
        check_repl_value(repl, "tty-read-line", "{:line \"two\"}");
        check_repl_value(repl, "tty-read-line", ":eof");
        check_repl_value(repl, "tty-read 100", ":eof");
        check_repl_value(repl, "try do tty-read :soon; rescue r -> r end", "{:error {:type-error :tty-read :soon}}");
        idm_repl_destroy(repl);
    }
    idm_runtime_destroy(&rt);
    stdin_pipe_remove(&sp);
}

static void *delayed_write(void *arg) {
    int fd = (int)(intptr_t)arg;
    struct timespec delay = {0, 150 * 1000000L};
    nanosleep(&delay, NULL);
    ssize_t ignored = write(fd, "x", 1u);
    (void)ignored;
    return NULL;
}

static void *delayed_winch(void *arg) {
    (void)arg;
    struct timespec delay = {0, 150 * 1000000L};
    nanosleep(&delay, NULL);
    kill(getpid(), SIGWINCH);
    return NULL;
}

static void test_tty_read_reports_resize(void) {
    StdinPipe sp;
    if (!stdin_pipe_install(&sp)) {
        failures++;
        return;
    }
    IdmRuntime rt;
    idm_runtime_init(&rt);
    rt.interactive = true;
    IdmRepl *repl = tty_repl_create(&rt);
    if (repl) {
        pthread_t sender;
        CHECK(pthread_create(&sender, NULL, delayed_winch, NULL) == 0);
        check_repl_value(repl,
                         "do\n"
                         "  parent = self\n"
                         "  spawn (fn -> send parent (tuple :got (tty-read :nil)))\n"
                         "  receive do\n"
                         "    {:got v} -> v\n"
                         "  end\n"
                         "end",
                         "{:signal :resize}");
        pthread_join(sender, NULL);
        idm_repl_destroy(repl);
    }
    idm_runtime_destroy(&rt);
    stdin_pipe_remove(&sp);
}

static void test_tty_read_parks_actor_not_worker(void) {
    StdinPipe sp;
    if (!stdin_pipe_install(&sp)) {
        failures++;
        return;
    }
    setenv("IDIOMMAXPROCS", "1", 1);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmRepl *repl = tty_repl_create(&rt);
    if (repl) {
        pthread_t writer;
        CHECK(pthread_create(&writer, NULL, delayed_write, (void *)(intptr_t)sp.write_fd) == 0);
        check_repl_value(repl,
                         "do\n"
                         "  parent = self\n"
                         "  spawn (fn -> send parent (tuple :bg :ran))\n"
                         "  r = tty-read :nil\n"
                         "  bg = receive do\n"
                         "    {:bg x} -> x\n"
                         "    after 0 -> :starved\n"
                         "  end\n"
                         "  tuple r bg\n"
                         "end",
                         "{{:byte 120} :ran}");
        pthread_join(writer, NULL);
        check_repl_value(repl,
                         "do\n"
                         "  spawn (fn do\n"
                         "    defn spin n -> spin (n + 1)\n"
                         "    spin 0\n"
                         "  end)\n"
                         "  tty-read 50\n"
                         "end",
                         ":timeout");
        idm_repl_destroy(repl);
    }
    idm_runtime_destroy(&rt);
    unsetenv("IDIOMMAXPROCS");
    stdin_pipe_remove(&sp);
}

void run_tty_suite(void) {
    test_tty_prims_on_pipe();
    test_tty_read_parks_actor_not_worker();
    test_tty_read_reports_resize();
}
