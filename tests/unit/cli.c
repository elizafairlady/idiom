#include "idiom/common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void fail(const char *name) {
    fprintf(stderr, "cli: %s\n", name);
    exit(1);
}

static void fail_error(IdmError *err, const char *name) {
    if (err->present) idm_error_fprint(stderr, err);
    fail(name);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void close_fd(int fd, const char *name) {
    if (close(fd) != 0) fail(name);
}

static void write_all_fd(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail("write pipe");
        off += (size_t)n;
    }
}

static char *read_all_fd(int fd, size_t *out_len) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    char tmp[4096];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            idm_buf_destroy(&buf);
            fail("read pipe");
        }
        if (n == 0) break;
        if (!idm_buf_append_n(&buf, tmp, (size_t)n)) {
            idm_buf_destroy(&buf);
            fail("read alloc");
        }
    }
    *out_len = buf.len;
    return idm_buf_take(&buf);
}

static const char *idiomc_under_test(void) {
    const char *idiomc = getenv("IDIOMC_UNDER_TEST");
    if (idiomc == NULL || idiomc[0] == '\0') idiomc = "build/idiomc";
    return idiomc;
}

static int run_cli_capture_all(const char *const *args, const char *input, size_t input_len, char **out, size_t *out_len, char **err_out, size_t *err_out_len) {
    const char *idiomc = idiomc_under_test();
    size_t argc = 0;
    while (args[argc]) argc++;
    char **child_argv = calloc(argc + 2u, sizeof(*child_argv));
    if (!child_argv) fail("argv alloc");
    child_argv[0] = (char *)idiomc;
    for (size_t i = 0; i < argc; i++) child_argv[i + 1u] = (char *)args[i];

    int in_pipe[2];
    int out_pipe[2];
    int err_pipe[2];
    check(pipe(in_pipe) == 0, "pipe stdin");
    check(pipe(out_pipe) == 0, "pipe stdout");
    if (err_out) check(pipe(err_pipe) == 0, "pipe stderr");

    pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        if (dup2(in_pipe[0], STDIN_FILENO) < 0) _exit(126);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(126);
        if (err_out && dup2(err_pipe[1], STDERR_FILENO) < 0) _exit(126);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        if (err_out) {
            close(err_pipe[0]);
            close(err_pipe[1]);
        }
        execv(idiomc, child_argv);
        _exit(127);
    }

    free(child_argv);
    close_fd(in_pipe[0], "close stdin read");
    close_fd(out_pipe[1], "close stdout write");
    if (err_out) close_fd(err_pipe[1], "close stderr write");
    if (input && input_len != 0) write_all_fd(in_pipe[1], input, input_len);
    close_fd(in_pipe[1], "close stdin write");
    *out = read_all_fd(out_pipe[0], out_len);
    close_fd(out_pipe[0], "close stdout read");
    if (err_out) {
        *err_out = read_all_fd(err_pipe[0], err_out_len);
        close_fd(err_pipe[0], "close stderr read");
    }

    int status = 0;
    check(waitpid(pid, &status, 0) == pid, "wait");
    check(WIFEXITED(status), "exit kind");
    return WEXITSTATUS(status);
}

static int run_cli_capture(const char *const *args, const char *input, size_t input_len, char **out, size_t *out_len) {
    return run_cli_capture_all(args, input, input_len, out, out_len, NULL, NULL);
}

static bool contains_text(const char *haystack, const char *needle) {
    return haystack && strstr(haystack, needle) != NULL;
}

static size_t count_text(const char *haystack, const char *needle) {
    size_t n = 0;
    for (const char *p = haystack; p && (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

static void test_eval_stdin(void) {
    const char *args[] = {"eval", "-", NULL};

    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    char *expected = NULL;
    size_t source_len = 0;
    size_t expected_len = 0;
    if (!idm_read_file("tests/cli/eval_stdin.id", &source, &source_len, &err)) fail_error(&err, "read eval stdin source");
    if (!idm_read_file("tests/cli/eval_stdin.out", &expected, &expected_len, &err)) fail_error(&err, "read eval stdin expected");

    size_t actual_len = 0;
    char *actual = NULL;
    int status = run_cli_capture(args, source, source_len, &actual, &actual_len);
    check(status == 0, "eval stdin status");
    check(actual_len == expected_len && memcmp(actual, expected, expected_len) == 0, "eval stdin output");

    free(actual);
    free(source);
    free(expected);
    idm_error_clear(&err);
}

static void test_test_run_filter(void) {
    const char *args[] = {"test", "-run", "pass-only", "tests/cli/test_filter.id", NULL};
    char *out = NULL;
    size_t out_len = 0;
    int status = run_cli_capture(args, NULL, 0, &out, &out_len);
    (void)out_len;
    check(status == 0, "test -run status");
    check(contains_text(out, "pass tests/cli/test_filter.id"), "test -run pass output");
    free(out);
}

static void test_bench_plain(void) {
    const char *args[] = {"test", "-bench", "core/arithmetic", "-count", "2", "-benchtime", "4x", "tests/bench/core_matrix.id", NULL};
    char *out = NULL;
    size_t out_len = 0;
    int status = run_cli_capture(args, NULL, 0, &out, &out_len);
    (void)out_len;
    check(status == 0, "bench plain status");
    check(contains_text(out, "bench name=core/arithmetic/int-add"), "bench plain row");
    check(contains_text(out, "n=4"), "bench plain fixed iterations");
    check(count_text(out, "bench name=core/arithmetic/int-add") == 2, "bench plain one line per rep");
    check(contains_text(out, "benchfile file=tests/bench/core_matrix.id"), "bench plain file timing");
    free(out);
}

static void test_bench_json(void) {
    const char *args[] = {"test", "-json", "-bench", "pattern/value-selector/list-rest", "-count", "1", "-benchtime", "2x", "tests/bench/pattern_matrix.id", NULL};
    char *out = NULL;
    size_t out_len = 0;
    int status = run_cli_capture(args, NULL, 0, &out, &out_len);
    (void)out_len;
    check(status == 0, "bench json status");
    check(contains_text(out, "\"action\":\"bench\""), "bench json row");
    check(contains_text(out, "\"name\":\"pattern/value-selector/list-rest\""), "bench json name");
    check(contains_text(out, "\"n\":2"), "bench json fixed iterations");
    check(contains_text(out, "\"action\":\"benchfile\""), "bench json file timing");
    check(contains_text(out, "\"compile_ns\":"), "bench json compile timing");
    free(out);
}

static void test_diag_json(void) {
    const char *args[] = {"-json", "eval", "undefined-name-xyz", NULL};
    char *out = NULL;
    char *err = NULL;
    size_t out_len = 0;
    size_t err_len = 0;
    int status = run_cli_capture_all(args, NULL, 0, &out, &out_len, &err, &err_len);
    check(status != 0, "diag json status");
    check(contains_text(err, "\"action\":\"diagnostic\""), "diag json action");
    check(contains_text(err, "\"stage\":\"expand\""), "diag json stage");
    check(contains_text(err, "\"code\":\"unbound-identifier\""), "diag json code");
    check(contains_text(err, "\"line\":1,\"column\":1"), "diag json position");
    check(contains_text(err, "\"start\":0,\"end\":18"), "diag json byte range");
    check(contains_text(err, "\"reason\":[\"unbound-identifier\",\"undefined-name-xyz\"]"), "diag json reason");
    free(out);
    free(err);
}

static void test_diag_excerpt(void) {
    const char *args[] = {"run", "tests/cli/diag_excerpt.id", NULL};
    char *out = NULL;
    char *err = NULL;
    size_t out_len = 0;
    size_t err_len = 0;
    int status = run_cli_capture_all(args, NULL, 0, &out, &out_len, &err, &err_len);
    check(status != 0, "diag excerpt status");
    check(contains_text(err, "tests/cli/diag_excerpt.id:2:1: error: unbound identifier 'undefined-name-xyz'"), "diag excerpt header");
    check(contains_text(err, "      2 | undefined-name-xyz"), "diag excerpt line");
    check(contains_text(err, "        | ^^^^^^^^^^^^^^^^^^"), "diag excerpt caret range");
    free(out);
    free(err);
}

static void test_explain_ish(void) {
    const char *args[] = {"-json", "explain", "tests/cli/explain_ish.id:7", NULL};
    char *out = NULL;
    char *err = NULL;
    size_t out_len = 0;
    size_t err_len = 0;
    int status = run_cli_capture_all(args, NULL, 0, &out, &out_len, &err, &err_len);
    check(status == 0, "explain ish status");
    check(contains_text(out, "\"name\":\"suite\""), "explain ish suite edge");
    check(contains_text(out, "\"name\":\"test\""), "explain ish test edge");
    check(contains_text(out, "\"name\":\"<\""), "explain ish redirect operator edge");
    check(contains_text(out, "\"name\":\"_\""), "explain ish fallback core-form edge");
    check(contains_text(out, "\"provider\":\"Test#"), "explain ish Test provider");
    check(contains_text(out, "\"provider\":\"Shell#"), "explain ish Shell provider");
    check(contains_text(out, "\"kind\":\"operator\""), "explain ish operator kind");
    check(contains_text(out, "\"kind\":\"core-form\""), "explain ish core-form kind");
    check(contains_text(out, "\"value-context\":true"), "explain ish value context");
    free(out);
    free(err);
}

static void test_test_events_json(void) {
    const char *pass_args[] = {"-json", "test", "-run", "pass-only", "tests/cli/test_filter.id", NULL};
    char *out = NULL;
    size_t out_len = 0;
    int status = run_cli_capture(pass_args, NULL, 0, &out, &out_len);
    check(status == 0, "test events pass status");
    check(contains_text(out, "\"action\":\"test\""), "test events action");
    check(contains_text(out, "\"event\":\"start\""), "test events start");
    check(contains_text(out, "\"event\":\"pass\""), "test events pass");
    check(contains_text(out, "\"suite\":\"cli-filter\""), "test events suite");
    check(contains_text(out, "\"test\":\"pass-only\""), "test events test name");
    check(contains_text(out, "\"ms\":"), "test events duration");
    free(out);

    const char *fail_args[] = {"-json", "test", "tests/cli/test_filter.id", NULL};
    out = NULL;
    char *errout = NULL;
    size_t errout_len = 0;
    status = run_cli_capture_all(fail_args, NULL, 0, &out, &out_len, &errout, &errout_len);
    check(status != 0, "test events fail status");
    check(contains_text(out, "\"event\":\"fail\""), "test events fail");
    check(contains_text(out, "\"reason\":[\"error\",\"filtered\"]"), "test events reason value");
    check(contains_text(errout, "suite-failed"), "expected failure diagnostic stays on stderr");
    free(errout);
    free(out);
}

static void test_deadlock_report(void) {
    const char *args[] = {"eval", "receive do :never -> :ok end", NULL};
    char *out = NULL;
    char *err = NULL;
    size_t out_len = 0;
    size_t err_len = 0;
    int status = run_cli_capture_all(args, NULL, 0, &out, &out_len, &err, &err_len);
    check(status != 0, "deadlock status");
    check(contains_text(err, "deadlock: all live actors are blocked"), "deadlock message");
    check(contains_text(err, "waiting-receive (mailbox 0, no timeout)"), "deadlock per-actor note");
    check(contains_text(err, "(<eval>:1:"), "deadlock note carries the receive site");
    free(out);
    free(err);
}

static void test_diag_json_notes(void) {
    const char *args[] = {"-json", "run", "tests/cli/macro_raise.id", NULL};
    char *out = NULL;
    char *err = NULL;
    size_t out_len = 0;
    size_t err_len = 0;
    int status = run_cli_capture_all(args, NULL, 0, &out, &out_len, &err, &err_len);
    check(status != 0, "diag json notes status");
    check(contains_text(err, "\"file\":\"tests/cli/macro_raise.id\""), "diag json top-level span file");
    check(contains_text(err, "\"message\":\"in boom\",\"file\":\"tests/cli/macro_raise.id\",\"line\":1,\"column\":28},"
                              "{\"message\":\"in expansion of 'boom'\",\"file\":\"tests/cli/macro_raise.id\""),
          "diag json per-note span files, innermost frame before macro use site");
    free(out);
    free(err);
}

static void absolute_path(const char *path, char *out, size_t out_len, const char *name) {
    if (path[0] == '/') {
        if (strlen(path) >= out_len) fail(name);
        strcpy(out, path);
        return;
    }
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) fail(name);
    int n = snprintf(out, out_len, "%s/%s", cwd, path);
    if (n <= 0 || (size_t)n >= out_len) fail(name);
}

static int run_cli_status_at(const char *dir, const char *root_env, const char *const *args) {
    char idiomc_abs[4352];
    absolute_path(idiomc_under_test(), idiomc_abs, sizeof(idiomc_abs), "root idiomc path");
    pid_t pid = fork();
    check(pid >= 0, "root fork");
    if (pid == 0) {
        if (chdir(dir) != 0) _exit(127);
        if (root_env) setenv("IDIOMROOT", root_env, 1);
        else unsetenv("IDIOMROOT");
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        size_t argc = 0;
        while (args[argc]) argc++;
        char **child_argv = calloc(argc + 2u, sizeof(*child_argv));
        if (!child_argv) _exit(127);
        child_argv[0] = idiomc_abs;
        for (size_t i = 0; i < argc; i++) child_argv[i + 1u] = (char *)args[i];
        execv(idiomc_abs, child_argv);
        _exit(127);
    }
    int status = 0;
    check(waitpid(pid, &status, 0) == pid, "root wait");
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void test_root_resolution(void) {
    char cwd_template[] = "/tmp/idiom_cli_cwd_XXXXXX";
    char *cwd = mkdtemp(cwd_template);
    if (!cwd) fail("root mkdtemp cwd");
    char root_template[] = "/tmp/idiom_cli_root_XXXXXX";
    char *root = mkdtemp(root_template);
    if (!root) fail("root mkdtemp root");
    char repo_std[4352];
    absolute_path("std", repo_std, sizeof(repo_std), "root std path");
    char link_path[4352];
    int n = snprintf(link_path, sizeof(link_path), "%s/std", root);
    check(n > 0 && (size_t)n < sizeof(link_path), "root link path");
    check(symlink(repo_std, link_path) == 0, "root symlink");
    const char *args[] = {"eval", "use std/tea\n:ok", NULL};
    check(run_cli_status_at(cwd, NULL, args) == 0, "root derived from executable");
    check(run_cli_status_at(cwd, root, args) == 0, "root from IDIOMROOT");
    check(run_cli_status_at(cwd, "/nonexistent-idiom-root", args) != 0, "bogus IDIOMROOT fails");
}

int idm_unit_cli(void) {
    test_eval_stdin();
    test_test_run_filter();
    test_bench_plain();
    test_bench_json();
    test_diag_json();
    test_diag_json_notes();
    test_diag_excerpt();
    test_explain_ish();
    test_test_events_json();
    test_deadlock_report();
    test_root_resolution();
    return 0;
}
