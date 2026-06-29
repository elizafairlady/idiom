#include "idiom/expand.h"
#include "idiom/actor.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(const char *name) {
    fprintf(stderr, "repl: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void check_error_clear(IdmError *err, const char *name) {
    if (err->present) fail(name);
}

static IdmValue compile_ok(IdmRepl *repl, const char *source, IdmError *err) {
    IdmValue thunk = idm_nil();
    uint64_t token = 0;
    IdmReplStatus status = idm_repl_compile(repl, source, &thunk, &token, err);
    check(status == IDM_REPL_OK, source);
    check(token != 0, "compile token");
    check_error_clear(err, source);
    return thunk;
}

static IdmValue run_ok(IdmRepl *repl, IdmValue thunk, IdmError *err) {
    IdmValue out = idm_nil();
    check(idm_repl_run(repl, thunk, &out, err), "run");
    check_error_clear(err, "run error");
    return out;
}

static void write_all_fd(int fd, const char *text) {
    size_t len = strlen(text);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n <= 0) fail("write pipe");
        off += (size_t)n;
    }
}

static int run_script(IdmRepl *repl, IdmValue thunk, const char *input, IdmError *err) {
    int in_pipe[2];
    if (pipe(in_pipe) != 0) fail("pipe stdin");
    int saved_in = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);
    int null_out = open("/dev/null", O_WRONLY);
    if (saved_in < 0 || saved_out < 0 || null_out < 0) fail("dup stdio");
    write_all_fd(in_pipe[1], input);
    if (close(in_pipe[1]) != 0) fail("close stdin write");
    if (fflush(stdout) != 0) fail("flush stdout");
    if (dup2(in_pipe[0], STDIN_FILENO) < 0) fail("redirect stdin");
    if (dup2(null_out, STDOUT_FILENO) < 0) fail("redirect stdout");
    if (close(in_pipe[0]) != 0) fail("close stdin read");
    if (close(null_out) != 0) fail("close null");

    IdmValue value = idm_nil();
    IdmValue reason = idm_nil();
    bool ok = idm_sched_run_session(idm_repl_scheduler(repl), thunk, false, &value, &reason, err);
    int status = ok ? idm_sched_session_status(idm_repl_scheduler(repl), value, reason) : 1;

    if (fflush(stdout) != 0) fail("flush redirected stdout");
    int restore_in = dup2(saved_in, STDIN_FILENO);
    int restore_out = dup2(saved_out, STDOUT_FILENO);
    if (close(saved_in) != 0) fail("close saved stdin");
    if (close(saved_out) != 0) fail("close saved stdout");
    if (restore_in < 0 || restore_out < 0) fail("restore stdio");
    check(ok, "std/repl run");
    check_error_clear(err, "std/repl run error");
    return status;
}

int idm_unit_repl(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmRepl *repl = idm_repl_create(&rt, &err);
    check(repl != NULL, "create");
    check(rt.repl == repl, "runtime session");

    IdmValue thunk = compile_ok(repl, "x = 41\nx + 1", &err);
    IdmValue out = run_ok(repl, thunk, &err);
    check(idm_value_is_int(out) && idm_int_value(out) == 42, "run value");

    thunk = compile_ok(repl, "x + 2", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_is_int(out) && idm_int_value(out) == 43, "persistent env");

    IdmValue ignored = idm_nil();
    uint64_t token = 0;
    IdmReplStatus status = idm_repl_compile(repl, "fn x ->", &ignored, &token, &err);
    check(status == IDM_REPL_INCOMPLETE, "incomplete");
    check(!err.present, "incomplete clears error");
    check(token == 0, "incomplete token");

    IdmValue startup = idm_nil();
    check(idm_repl_loop_thunk(repl, "use std/repl\nmain\n", &startup, &err), "std/repl startup");
    check(idm_is_closure(startup), "std/repl startup thunk");
    check_error_clear(&err, "std/repl startup error");
    check(run_script(repl, startup, "1\n:q\n", &err) == 0, "std/repl script status");

    idm_repl_destroy(repl);
    check(rt.repl == NULL, "runtime session cleared");
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    return 0;
}
