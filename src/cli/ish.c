#include "idiom/actor.h"
#include "idiom/bytecode.h"
#include "idiom/common.h"
#include "idiom/core.h"
#include "idiom/expand.h"
#include "idiom/reader.h"
#include "idiom/syntax.h"
#include "idiom/value.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IDM_VERSION
#define IDM_VERSION "0.0.0-dev"
#endif


static char **g_cli_args = NULL;
static size_t g_cli_arg_count = 0;

static int masked_status(long n) {
    int code = (int)(((n % 256) + 256) % 256);
    return code;
}

static bool parse_whole_long(const char *text, const char *end, long *out) {
    char *stop = NULL;
    errno = 0;
    long n = strtol(text, &stop, 10);
    if (errno != 0 || stop != end || stop == text) return false;
    *out = n;
    return true;
}

static int shell_report(const IdmError *err) {
    static const char prefix[] = "main actor exited with reason ";
    static const char command_prefix[] = "{:error {:command \"";
    const char *msg = err->message ? err->message : "";
    if (strncmp(msg, prefix, sizeof(prefix) - 1u) == 0) {
        const char *reason = msg + sizeof(prefix) - 1u;
        const char *end = reason + strlen(reason);
        long n = 0;
        if (parse_whole_long(reason, end, &n)) return masked_status(n);
        if (strncmp(reason, command_prefix, sizeof(command_prefix) - 1u) == 0) {
            const char *name = reason + sizeof(command_prefix) - 1u;
            const char *cursor = name;
            while (*cursor != '\0' && *cursor != '"') {
                if (*cursor == '\\' && cursor[1] != '\0') cursor++;
                cursor++;
            }
            size_t tail = strlen(cursor);
            if (*cursor == '"' && cursor[1] == ' ' && tail >= 4u && strcmp(cursor + tail - 2u, "}}") == 0) {
                const char *status_text = cursor + 2u;
                const char *status_end = cursor + tail - 2u;
                if (parse_whole_long(status_text, status_end, &n)) {
                    fprintf(stderr, "ish: %.*s: exit status %ld\n", (int)(cursor - name), name, n);
                    int code = masked_status(n);
                    return code == 0 ? 1 : code;
                }
                if (strncmp(status_text, ":capture-overflow", (size_t)(status_end - status_text)) == 0 &&
                    status_end - status_text == (ptrdiff_t)strlen(":capture-overflow")) {
                    fprintf(stderr, "ish: %.*s: capture overflow\n", (int)(cursor - name), name);
                    return 1;
                }
            }
        }
    }
    idm_error_fprint(stderr, err);
    return 1;
}

static int run_shell_source(const char *file, const char *source, bool print_result) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    rt.cli_args = g_cli_args;
    rt.cli_arg_count = g_cli_arg_count;
    IdmError err;
    idm_error_init(&err);
    int status = 1;
    IdmSyntax *program = NULL;
    IdmSyntax *wrapped = NULL;
    IdmCore *core = NULL;
    IdmBytecodeModule module;
    idm_bc_init(&module);
    IdmScheduler *sched = NULL;
    if (!idm_reader_read_string(file, source, &program, &err)) goto done;
    wrapped = idm_syn_program_prepend_implement(program, "std/shell", file);
    if (!wrapped) {
        idm_error_oom(&err, idm_span_unknown(file));
        goto done;
    }
    if (!idm_expand_syntax(&rt, wrapped, &core, &err)) goto done;
    uint32_t main_fn = 0;
    if (!idm_core_compile_main(core, &module, &main_fn, &err)) goto done;
    sched = idm_sched_create(&rt, &module, &err);
    if (!sched) goto done;
    IdmValue out = idm_nil();
    if (!idm_sched_run_main(sched, main_fn, &out, &err)) goto done;
    if (print_result) {
        IdmBuffer buf;
        idm_buf_init(&buf);
        if (idm_value_write(&buf, out) && idm_buf_append_char(&buf, '\n')) fputs(buf.data, stdout);
        idm_buf_destroy(&buf);
    }
    status = 0;
done:
    if (err.present) status = shell_report(&err);
    idm_error_clear(&err);
    idm_sched_destroy(sched);
    idm_core_free(core);
    idm_syn_free(wrapped);
    idm_syn_free(program);
    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    return status;
}

static int run_shell_file(const char *path) {
    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    size_t len = 0;
    if (!idm_read_file(path, &source, &len, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        return 1;
    }
    int status = run_shell_source(path, source, false);
    free(source);
    return status;
}

static void usage(FILE *out) {
    fprintf(out, "ish — the idiom shell\n");
    fprintf(out, "  ish FILE.ish        run a shell script\n");
    fprintf(out, "  ish -c 'COMMANDS'   run shell commands\n");
    fprintf(out, "  ish --version\n");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("ish %s (idiom)\n", IDM_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "-c") == 0) return run_shell_source("<command>", argv[2], false);
    if (argc >= 2 && argv[1][0] != '-') {
        g_cli_args = argv + 2;
        g_cli_arg_count = (size_t)(argc - 2);
        return run_shell_file(argv[1]);
    }
    usage(stderr);
    return 64;
}
