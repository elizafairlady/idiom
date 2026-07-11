#include "idiom/expand.h"
#include "idiom/actor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int kind;
    unsigned mark;
    bool greyed;
    IdmHeap *heap;
    size_t bytes;
    _Atomic uint32_t dict_hash;
    void *next;
    void *grey_next;
    size_t scan;
    struct {
        IdmValue parent;
        unsigned char *bytes;
        uint64_t bit_off;
        uint64_t bit_len;
        uint64_t bit_cap;
        uint64_t bit_used;
    } as;
} TestBitObject;

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

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static void check_exit_code(IdmValue value, int64_t code, const char *name) {
    check(idm_value_tag(value) == IDM_VAL_TUPLE && idm_sequence_count(value) == 2u, name);
    IdmError err;
    idm_error_init(&err);
    IdmValue tag = idm_sequence_item(value, 0, &err);
    IdmValue status = idm_sequence_item(value, 1, &err);
    check_error_clear(&err, name);
    check(idm_value_tag(tag) == IDM_VAL_ATOM && strcmp(idm_symbol_text(idm_value_symbol(tag)), "exit-code") == 0, name);
    check(idm_value_is_int(status) && idm_int_value(status) == code, name);
}

static IdmValue oversized_bitstring(IdmRuntime *rt, IdmHeap *heap, IdmError *err) {
    unsigned char byte = 0;
    IdmHeap *prev = idm_swap_active_heap(heap);
    IdmValue bits = idm_bits_from_bytes(rt, &byte, 8u, err);
    idm_swap_active_heap(prev);
    check(!err->present, "oversized bitstring seed");
    check(idm_value_tag(bits) == IDM_VAL_BITSTRING, "oversized bitstring tag");
    TestBitObject *obj = (TestBitObject *)idm_boxed_object(bits);
    obj->as.bit_len = UINT64_MAX;
    obj->as.bit_cap = UINT64_MAX;
    obj->as.bit_used = UINT64_MAX;
    return bits;
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
    bool ok = idm_sched_run_session(idm_repl_scheduler(repl), thunk, &value, &reason, err);
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

    thunk = compile_ok(repl, "use std/cmd\npidfd_wakeup_port = spawn (command (list \"sh\" \"-c\" \"sleep 0.05; exit 7\") (stdio :null :null :null))\n:ok", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_tag(out) == IDM_VAL_ATOM, "pidfd wakeup spawn");
    sleep_ms(120);
    thunk = compile_ok(repl, "pidfd_wakeup_ref = monitor pidfd_wakeup_port\nreceive do\n  {:down r target reason} when (and (eq? r pidfd_wakeup_ref) (eq? target pidfd_wakeup_port)) -> reason\n  after 500 -> :timeout\nend", &err);
    out = run_ok(repl, thunk, &err);
    check_exit_code(out, 7, "pidfd-ready port posts down across scheduler runs");

    thunk = compile_ok(repl, "\"survive\"", &err);
    IdmValue survivor = run_ok(repl, thunk, &err);
    check(idm_value_heap(survivor) == &rt.immortal, "eval result copied to runtime heap");
    thunk = compile_ok(repl, ":ok", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_tag(out) == IDM_VAL_ATOM, "reap driver");
    check(idm_value_tag(survivor) == IDM_VAL_STRING && strcmp(idm_string_bytes(survivor), "survive") == 0,
          "eval result survives actor reap");

    thunk = compile_ok(repl, "spawn (fn do\n  receive do\n    :stop -> :ok\n    after 5000 -> :late\n  end\nend)", &err);
    IdmValue child = run_ok(repl, thunk, &err);
    check(idm_value_tag(child) == IDM_VAL_PID, "copy oom child pid");
    IdmHeap source_heap;
    idm_heap_init(&source_heap);
    IdmValue bomb = oversized_bitstring(&rt, &source_heap, &err);
    idm_sched_send(idm_repl_scheduler(repl), idm_value_id(child), bomb);
    IdmValue info = idm_nil();
    check(idm_sched_process_info_value(idm_repl_scheduler(repl), &rt, idm_value_id(child), &info, &err),
          "copy oom process info");
    check_error_clear(&err, "copy oom process info error");
    check(idm_is_nil(info), "deliver copy oom terminates target");
    idm_heap_destroy(&source_heap);

    IdmValue ignored = idm_nil();
    uint64_t token = 0;
    IdmReplStatus status = idm_repl_compile(repl, "fn x ->", &ignored, &token, &err);
    check(status == IDM_REPL_INCOMPLETE, "incomplete");
    check(!err.present, "incomplete clears error");
    check(token == 0, "incomplete token");

    status = idm_repl_compile(repl, "defn fold-probe n -> add n 1; nosuch-name", &ignored, &token, &err);
    check(status == IDM_REPL_ERROR, "failed unit reports error");
    check(err.present, "failed unit sets error");
    idm_error_clear(&err);
    thunk = compile_ok(repl, "defn fold-probe n -> add n 100\nfold-probe 1", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_is_int(out) && idm_int_value(out) == 101, "rolled-back const defn does not fold stale body");

    thunk = compile_ok(repl, "defn arity-probe n -> add n 1\narity-probe 1", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_is_int(out) && idm_int_value(out) == 2, "arity witness baseline");
    IdmValue aborted = idm_nil();
    token = 0;
    status = idm_repl_compile(repl, "defn arity-probe a b -> add a b\narity-probe 1 2", &aborted, &token, &err);
    check(status == IDM_REPL_OK && token != 0, "arity update compiles");
    check_error_clear(&err, "arity update error");
    idm_repl_abort(repl, token);
    thunk = compile_ok(repl, "arity-probe 41", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_is_int(out) && idm_int_value(out) == 42, "aborted arity update leaves the pre-abort binding resolving");
    thunk = compile_ok(repl, "defn arity-probe a b -> add a b\narity-probe 1 2", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_is_int(out) && idm_int_value(out) == 3, "committed redefinition shadows the old binding");

    IdmValue aborted2 = idm_nil();
    thunk = compile_ok(repl, "defn leak-probe _s -> \"old\"\nleak-probe \"x\"", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_tag(out) == IDM_VAL_STRING && strcmp(idm_string_bytes(out), "old") == 0, "leak witness baseline");
    status = idm_repl_compile(repl, "defn leak-probe a _b -> a\nleak-probe \"x\" \"y\"", &aborted2, &token, &err);
    check(status == IDM_REPL_OK && token != 0, "leak candidate compiles");
    check_error_clear(&err, "leak candidate error");
    idm_repl_abort(repl, token);
    thunk = compile_ok(repl, "defmacro leak-m %`(leak-m %,s) -> make-syntax-string s (leak-probe \"x\")\nleak-m :k", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_tag(out) == IDM_VAL_STRING && strcmp(idm_string_bytes(out), "old") == 0,
          "aborted redefinition does not leak into phase-1 demand");

    thunk = compile_ok(repl, "spec cpin :: int -> int\ndefn cpin n -> add n 1\ncpin 1", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_is_int(out) && idm_int_value(out) == 2, "contract witness baseline");
    status = idm_repl_compile(repl, "defn cpin-bad -> cpin :nope", &ignored, &token, &err);
    check(status == IDM_REPL_ERROR, "committed contract enforced");
    idm_error_clear(&err);
    status = idm_repl_compile(repl, "spec cpin :: atom -> atom\ndefn cpin a -> a\ncpin :ok", &aborted2, &token, &err);
    check(status == IDM_REPL_OK && token != 0, "contract update compiles");
    check_error_clear(&err, "contract update error");
    idm_repl_abort(repl, token);
    status = idm_repl_compile(repl, "defn cpin-bad2 -> cpin :nope", &ignored, &token, &err);
    check(status == IDM_REPL_ERROR, "aborted contract update restores the old contract");
    idm_error_clear(&err);
    thunk = compile_ok(repl, "defn cpin a -> a\ndefn cpin-ok -> cpin :ok\ncpin-ok", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_tag(out) == IDM_VAL_ATOM, "redefinition without a spec drops the old contract");

    thunk = compile_ok(repl, "rebind-probe = 1\nrebind-probe", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_is_int(out) && idm_int_value(out) == 1, "value bind baseline");
    thunk = compile_ok(repl, "rebind-probe = 2\nrebind-probe", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_is_int(out) && idm_int_value(out) == 2, "value rebind shadows the old binding");
    status = idm_repl_compile(repl, "rebind-probe = 3", &aborted2, &token, &err);
    check(status == IDM_REPL_OK && token != 0, "value rebind compiles");
    check_error_clear(&err, "value rebind error");
    idm_repl_abort(repl, token);
    thunk = compile_ok(repl, "rebind-probe", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_is_int(out) && idm_int_value(out) == 2, "aborted value rebind leaves the old value");

    thunk = compile_ok(repl, "use std/compile with [expand-check]\nexpand-check \":ok\"", &err);
    out = run_ok(repl, thunk, &err);
    for (int i = 0; i < 64; i++) {
        IdmValue probe = idm_nil();
        check(idm_repl_compile(repl, ":ok", &probe, &token, &err) == IDM_REPL_OK, "repeated compile");
        check_error_clear(&err, "repeated compile error");
    }
    thunk = compile_ok(repl, "defn dyn-not x -> not? x\ndyn-not (add 1 2)", &err);
    out = run_ok(repl, thunk, &err);
    check(idm_value_tag(out) == IDM_VAL_ATOM, "kernel closure survives expand-check and repeated compiles");

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
