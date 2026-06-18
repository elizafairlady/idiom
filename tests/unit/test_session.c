#include "test_util.h"

#include <pthread.h>
#include <signal.h>

static bool session_eval(IdmRepl *repl, const char *source, IdmValue *out, uint64_t *out_token, IdmError *err) {
    IdmValue thunk = idm_nil();
    uint64_t token = 0;
    IdmReplStatus status = idm_repl_compile(repl, source, &thunk, &token, err);
    if (out_token) *out_token = token;
    if (status != IDM_REPL_OK) return false;
    return idm_repl_run(repl, thunk, out, err);
}

static void check_eval_value(IdmRepl *repl, const char *source, const char *expect) {
    IdmError err;
    idm_error_init(&err);
    IdmValue out = idm_nil();
    bool ok = session_eval(repl, source, &out, NULL, &err);
    if (!ok) {
        fprintf(stderr, "eval failed for '%s': %s\n", source, err.message ? err.message : "?");
        failures++;
        idm_error_clear(&err);
        return;
    }
    IdmBuffer buf;
    idm_buf_init(&buf);
    CHECK(idm_value_write(&buf, out));
    CHECK_STR(buf.data ? buf.data : "", expect);
    idm_buf_destroy(&buf);
    idm_error_clear(&err);
}

static void check_eval_fails(IdmRepl *repl, const char *source, const char *expect_substring) {
    IdmError err;
    idm_error_init(&err);
    IdmValue out = idm_nil();
    uint64_t token = 0;
    bool ok = session_eval(repl, source, &out, &token, &err);
    CHECK(!ok);
    if (!ok) {
        CHECK(err.message && strstr(err.message, expect_substring));
        if (!(err.message && strstr(err.message, expect_substring))) {
            fprintf(stderr, "eval of '%s' failed with '%s', expected '%s'\n", source, err.message ? err.message : "?", expect_substring);
        }
    }
    idm_repl_abort(repl, token);
    idm_error_clear(&err);
}

static void test_session_persistent_bindings(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmRepl *repl = idm_repl_create(&rt, &err);
    CHECK(repl != NULL);
    if (!repl) {
        idm_error_clear(&err);
        idm_runtime_destroy(&rt);
        return;
    }
    check_eval_value(repl, "x = 41", ":nil");
    check_eval_value(repl, "x + 1", "42");
    check_eval_fails(repl, "raise :boom", "main actor exited with reason :boom");
    check_eval_value(repl, "x + 1", "42");
    idm_repl_destroy(repl);
    idm_runtime_destroy(&rt);
}

static void test_session_rollback_on_down(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmRepl *repl = idm_repl_create(&rt, &err);
    CHECK(repl != NULL);
    if (!repl) {
        idm_error_clear(&err);
        idm_runtime_destroy(&rt);
        return;
    }
    check_eval_fails(repl, "y = 9\nraise :late", "main actor exited with reason :late");
    IdmValue thunk = idm_nil();
    uint64_t token = 0;
    IdmReplStatus status = idm_repl_compile(repl, "y", &thunk, &token, &err);
    CHECK(status == IDM_REPL_ERROR);
    CHECK(err.message && strstr(err.message, "unbound identifier 'y'"));
    idm_error_clear(&err);
    char *diag = idm_sched_take_diagnostic(idm_repl_scheduler(repl));
    CHECK(diag && strstr(diag, ":late"));
    free(diag);
    CHECK(idm_sched_take_diagnostic(idm_repl_scheduler(repl)) == NULL);
    idm_repl_destroy(repl);
    idm_runtime_destroy(&rt);
}

static void test_session_actors_survive_between_evals(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmRepl *repl = idm_repl_create(&rt, &err);
    CHECK(repl != NULL);
    if (!repl) {
        idm_error_clear(&err);
        idm_runtime_destroy(&rt);
        return;
    }
    check_eval_value(repl,
                     "counter = spawn (fn do\n"
                     "  defn serve total -> receive do\n"
                     "    {:add n} -> serve (total + n)\n"
                     "    {:get from} -> do\n"
                     "      send from (tuple :total total)\n"
                     "      serve total\n"
                     "    end\n"
                     "  end\n"
                     "  serve 0\n"
                     "end)",
                     ":nil");
    check_eval_value(repl, "send counter (tuple :add 5)", "{:add 5}");
    check_eval_value(repl, "send counter (tuple :add 2)", "{:add 2}");
    check_eval_value(repl,
                     "do\n"
                     "  send counter (tuple :get (self))\n"
                     "  receive do {:total n} -> n end\n"
                     "end",
                     "7");
    idm_repl_destroy(repl);
    idm_runtime_destroy(&rt);
}

static void *send_sigints(void *arg) {
    size_t count = (size_t)(uintptr_t)arg;
    for (size_t i = 0; i < count; i++) {
        struct timespec delay = {0, 100 * 1000000L};
        nanosleep(&delay, NULL);
        kill(getpid(), SIGINT);
    }
    return NULL;
}

static void test_session_interrupt_under_load(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    rt.interactive = true;
    IdmError err;
    idm_error_init(&err);
    IdmRepl *repl = idm_repl_create(&rt, &err);
    CHECK(repl != NULL);
    if (!repl) {
        idm_error_clear(&err);
        idm_runtime_destroy(&rt);
        return;
    }
    pthread_t sender;
    CHECK(pthread_create(&sender, NULL, send_sigints, (void *)(uintptr_t)1) == 0);
    check_eval_fails(repl, "defn spin n -> spin (n + 1)\nspin 0", "main actor exited with reason :interrupt");
    pthread_join(sender, NULL);
    CHECK(pthread_create(&sender, NULL, send_sigints, (void *)(uintptr_t)2) == 0);
    check_eval_fails(repl, "trap-exit :true\ndefn spin2 n -> spin2 (n + 1)\nspin2 0", "main actor exited with reason :killed");
    pthread_join(sender, NULL);
    check_eval_value(repl, "40 + 2", "42");
    idm_repl_destroy(repl);
    idm_runtime_destroy(&rt);
}

static void test_session_windows_in_session(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmRepl *repl = idm_repl_create(&rt, &err);
    CHECK(repl != NULL);
    if (!repl) {
        idm_error_clear(&err);
        idm_runtime_destroy(&rt);
        return;
    }
    check_eval_value(repl, "{:ok f tok} = repl-compile \"receive do :go -> 42 end\"", ":nil");
    check_eval_value(repl,
                     "do\n"
                     "  w = repl-spawn &f\n"
                     "  monitor w\n"
                     "  send w :go\n"
                     "  receive do {:down _ p r} -> r end\n"
                     "end",
                     ":normal");
    check_eval_value(repl, "repl-compile \"do\"", ":incomplete");
    check_eval_value(repl, "activate app/ish", ":nil");
    check_eval_value(repl, "repl-compile \"echo hi |\"", ":incomplete");
    IdmValue thunk = idm_nil();
    uint64_t token = 0;
    IdmReplStatus status = idm_repl_compile(repl, "echo hi |\nwc -c", &thunk, &token, &err);
    CHECK(status == IDM_REPL_OK);
    idm_repl_abort(repl, token);
    idm_error_clear(&err);
    check_eval_value(repl, "ish-session", ":nil");
    idm_repl_destroy(repl);
    idm_runtime_destroy(&rt);
}

void run_session_suite(void) {
    test_session_persistent_bindings();
    test_session_rollback_on_down();
    test_session_actors_survive_between_evals();
    test_session_interrupt_under_load();
    test_session_windows_in_session();
}
