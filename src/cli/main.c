#include "idiom/actor.h"
#include "idiom/common.h"
#include "idiom/expand.h"
#include "idiom/reader.h"
#include "idiom/vm.h"

#include <stdlib.h>
#include <string.h>

static void usage(FILE *out) {
    fputs("usage:\n", out);
    fputs("  idiomc <file|->            run a script (\"-\" reads stdin)\n", out);
    fputs("  idiomc --eval <source>     run source text and print its value\n", out);
    fputs("  idiomc --dump-reader <file|->\n", out);
    fputs("  idiomc --dump-core <file|->\n", out);
    fputs("  idiomc --dump-bytecode <file|->\n", out);
}

static bool read_input_arg(const char *arg, char **source, IdmError *err) {
    size_t len = 0;
    if (strcmp(arg, "-") == 0) return idm_read_stream(stdin, "<stdin>", source, &len, err);
    return idm_read_file(arg, source, &len, err);
}

static int dump_reader(const char *path) {
    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    if (!read_input_arg(path, &source, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        return 1;
    }
    IdmSyntax *program = NULL;
    const char *file = strcmp(path, "-") == 0 ? "<stdin>" : path;
    if (!idm_reader_read_string(file, source, &program, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        free(source);
        return 1;
    }
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_syn_dump(&buf, program) || !idm_buf_append_char(&buf, '\n')) {
        fputs("idiomc: out of memory while dumping reader syntax\n", stderr);
        idm_buf_destroy(&buf);
        idm_syn_free(program);
        free(source);
        return 1;
    }
    fputs(buf.data, stdout);
    idm_buf_destroy(&buf);
    idm_syn_free(program);
    free(source);
    return 0;
}

static bool expand_input(const char *path, IdmRuntime *rt, IdmCore **out, IdmError *err) {
    char *source = NULL;
    if (!read_input_arg(path, &source, err)) return false;
    const char *file = strcmp(path, "-") == 0 ? "<stdin>" : path;
    bool ok = idm_expand_string(rt, file, source, out, err);
    free(source);
    return ok;
}

static int dump_core(const char *path) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    if (!expand_input(path, &rt, &core, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        idm_runtime_destroy(&rt);
        return 1;
    }
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_core_dump(&buf, core) || !idm_buf_append_char(&buf, '\n')) {
        fputs("idiomc: out of memory while dumping core\n", stderr);
        idm_buf_destroy(&buf);
        idm_core_free(core);
        idm_runtime_destroy(&rt);
        return 1;
    }
    fputs(buf.data, stdout);
    idm_buf_destroy(&buf);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    return 0;
}

static bool compile_input(const char *path, IdmRuntime *rt, IdmBytecodeModule *module, uint32_t *main_fn, IdmError *err) {
    IdmCore *core = NULL;
    if (!expand_input(path, rt, &core, err)) return false;
    bool ok = idm_core_compile_main(core, module, main_fn, err);
    idm_core_free(core);
    return ok;
}

static int dump_bytecode(const char *path) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    if (!compile_input(path, &rt, &module, &main_fn, &err)) {
        (void)main_fn;
        idm_error_fprint(stderr, &err);
        idm_bc_destroy(&module);
        idm_error_clear(&err);
        idm_runtime_destroy(&rt);
        return 1;
    }
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_bc_disassemble(&buf, &module)) {
        fputs("idiomc: out of memory while dumping bytecode\n", stderr);
        idm_buf_destroy(&buf);
        idm_bc_destroy(&module);
        idm_runtime_destroy(&rt);
        return 1;
    }
    fputs(buf.data, stdout);
    idm_buf_destroy(&buf);
    idm_bc_destroy(&module);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    return 0;
}

static int run_source(const char *file, const char *source, bool print_result) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    int status = 1;
    IdmCore *core = NULL;
    IdmBytecodeModule module;
    idm_bc_init(&module);
    IdmScheduler *sched = NULL;
    if (!idm_expand_string(&rt, file, source, &core, &err)) goto done;
    uint32_t main_fn = 0;
    if (!idm_core_compile_main(core, &module, &main_fn, &err)) goto done;
    sched = idm_sched_create(&rt, &module, &err);
    if (!sched) goto done;
    IdmValue out = idm_nil();
    if (!idm_sched_run_main(sched, main_fn, &out, &err)) goto done;
    if (print_result) {
        IdmBuffer buf;
        idm_buf_init(&buf);
        if (!idm_value_write(&buf, out) || !idm_buf_append_char(&buf, '\n')) {
            idm_buf_destroy(&buf);
            idm_error_set(&err, idm_span_unknown(NULL), "out of memory while printing result");
            goto done;
        }
        fputs(buf.data, stdout);
        idm_buf_destroy(&buf);
    }
    status = 0;
done:
    if (err.present) idm_error_fprint(stderr, &err);
    idm_error_clear(&err);
    idm_sched_destroy(sched);
    idm_core_free(core);
    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    return status;
}

static int run_file(const char *path) {
    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    if (!read_input_arg(path, &source, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        return 1;
    }
    const char *file = strcmp(path, "-") == 0 ? "<stdin>" : path;
    int status = run_source(file, source, false);
    free(source);
    return status;
}

#include <dirent.h>

static int collect_test_files(const char *dir_path, char ***out_files, size_t *out_count, size_t *out_cap) {
    DIR *dir = opendir(dir_path);
    if (!dir) return -1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 3, ".id") != 0) continue;
        if (*out_count == *out_cap) {
            size_t cap = *out_cap ? *out_cap * 2u : 16u;
            char **grown = realloc(*out_files, cap * sizeof(*grown));
            if (!grown) { closedir(dir); return -1; }
            *out_files = grown;
            *out_cap = cap;
        }
        size_t full_len = strlen(dir_path) + 1u + len + 1u;
        char *full = malloc(full_len);
        if (!full) { closedir(dir); return -1; }
        snprintf(full, full_len, "%s/%s", dir_path, entry->d_name);
        (*out_files)[(*out_count)++] = full;
    }
    closedir(dir);
    return 0;
}

static int name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int run_tests(int argc, char **argv) {
    char **files = NULL;
    size_t count = 0;
    size_t cap = 0;
    if (argc == 0) {
        if (collect_test_files("tests/lang", &files, &count, &cap) != 0) {
            fprintf(stderr, "idiomc test: cannot read tests/lang\n");
            return 1;
        }
    } else {
        for (int i = 0; i < argc; i++) {
            if (collect_test_files(argv[i], &files, &count, &cap) == 0) continue;
            if (count == cap) {
                size_t next = cap ? cap * 2u : 16u;
                char **grown = realloc(files, next * sizeof(*grown));
                if (!grown) return 1;
                files = grown;
                cap = next;
            }
            files[count++] = strdup(argv[i]);
        }
    }
    if (count == 0) {
        fprintf(stderr, "idiomc test: no test files found\n");
        return 1;
    }
    qsort(files, count, sizeof(*files), name_cmp);
    size_t failed = 0;
    for (size_t i = 0; i < count; i++) {
        IdmError err;
        idm_error_init(&err);
        char *source = NULL;
        size_t len = 0;
        int status = 1;
        if (idm_read_file(files[i], &source, &len, &err)) {
            status = run_source(files[i], source, false);
            free(source);
        } else {
            idm_error_fprint(stderr, &err);
        }
        idm_error_clear(&err);
        if (status != 0) {
            fprintf(stderr, "FAIL %s\n", files[i]);
            failed++;
        } else {
            printf("pass %s\n", files[i]);
        }
    }
    if (failed == 0) {
        printf("idiomc test: %zu file%s passed\n", count, count == 1 ? "" : "s");
    } else {
        printf("idiomc test: %zu of %zu file%s FAILED\n", failed, count, count == 1 ? "" : "s");
    }
    for (size_t i = 0; i < count; i++) free(files[i]);
    free(files);
    return failed == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "test") == 0) return run_tests(argc - 2, argv + 2);
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
#ifndef IDM_VERSION
#define IDM_VERSION "0.0.0-dev"
#endif
        printf("idiomc %s (idiom)\n", IDM_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--eval") == 0) return run_source("<eval>", argv[2], true);
    if (argc == 3 && strcmp(argv[1], "--dump-reader") == 0) return dump_reader(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--dump-core") == 0) return dump_core(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--dump-bytecode") == 0) return dump_bytecode(argv[2]);
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "--dump-surface") == 0) {
        const char *prelude = argc == 3 ? argv[2] : "";
        char source[4096];
        snprintf(source, sizeof(source),
                 "%s\n"
                 "for-syntax do\n"
                 "  println (tuple :operators (expander-surface :operators))\n"
                 "  println (tuple :macros (expander-surface :macros))\n"
                 "  println (tuple :protocols (expander-surface :protocols))\n"
                 "  println (tuple :resolvers (expander-surface :resolvers))\n"
                 "  println (tuple :methods (expander-surface :methods))\n"
                 "  println (tuple :active (expander-surface :active))\n"
                 "end\n"
                 ":ok\n", prelude);
        return run_source("<dump-surface>", source, false);
    }
    if (argc == 2 && argv[1][0] != '-') return run_file(argv[1]);
    if (argc == 2 && strcmp(argv[1], "-") == 0) return run_file("-");
    usage(stderr);
    return 64;
}
