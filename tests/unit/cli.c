#include "idiom/common.h"

#include <errno.h>
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

static int run_cli_capture(const char *const *args, const char *input, size_t input_len, char **out, size_t *out_len) {
    const char *idiomc = idiomc_under_test();
    size_t argc = 0;
    while (args[argc]) argc++;
    char **child_argv = calloc(argc + 2u, sizeof(*child_argv));
    if (!child_argv) fail("argv alloc");
    child_argv[0] = (char *)idiomc;
    for (size_t i = 0; i < argc; i++) child_argv[i + 1u] = (char *)args[i];

    int in_pipe[2];
    int out_pipe[2];
    check(pipe(in_pipe) == 0, "pipe stdin");
    check(pipe(out_pipe) == 0, "pipe stdout");

    pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        if (dup2(in_pipe[0], STDIN_FILENO) < 0) _exit(126);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(126);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        execv(idiomc, child_argv);
        _exit(127);
    }

    free(child_argv);
    close_fd(in_pipe[0], "close stdin read");
    close_fd(out_pipe[1], "close stdout write");
    if (input && input_len != 0) write_all_fd(in_pipe[1], input, input_len);
    close_fd(in_pipe[1], "close stdin write");
    *out = read_all_fd(out_pipe[0], out_len);
    close_fd(out_pipe[0], "close stdout read");

    int status = 0;
    check(waitpid(pid, &status, 0) == pid, "wait");
    check(WIFEXITED(status), "exit kind");
    return WEXITSTATUS(status);
}

static bool contains_text(const char *haystack, const char *needle) {
    return haystack && strstr(haystack, needle) != NULL;
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
    const char *args[] = {"test", "-bench", "core/arithmetic", "-count", "4", "tests/bench/core_matrix.id", NULL};
    char *out = NULL;
    size_t out_len = 0;
    int status = run_cli_capture(args, NULL, 0, &out, &out_len);
    (void)out_len;
    check(status == 0, "bench plain status");
    check(contains_text(out, "bench name=core/arithmetic/int-add"), "bench plain row");
    check(contains_text(out, "n=4"), "bench plain count");
    check(contains_text(out, "benchfile file=tests/bench/core_matrix.id"), "bench plain file timing");
    free(out);
}

static void test_bench_json(void) {
    const char *args[] = {"test", "-json", "-bench", "pattern/value-selector/list-rest", "-count", "2", "tests/bench/pattern_matrix.id", NULL};
    char *out = NULL;
    size_t out_len = 0;
    int status = run_cli_capture(args, NULL, 0, &out, &out_len);
    (void)out_len;
    check(status == 0, "bench json status");
    check(contains_text(out, "\"action\":\"bench\""), "bench json row");
    check(contains_text(out, "\"name\":\"pattern/value-selector/list-rest\""), "bench json name");
    check(contains_text(out, "\"n\":2"), "bench json count");
    check(contains_text(out, "\"action\":\"benchfile\""), "bench json file timing");
    check(contains_text(out, "\"compile_ns\":"), "bench json compile timing");
    free(out);
}

int idm_unit_cli(void) {
    test_eval_stdin();
    test_test_run_filter();
    test_bench_plain();
    test_bench_json();
    return 0;
}
