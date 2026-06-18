#include "idiom/actor.h"
#include "idiom/artifact.h"
#include "idiom/common.h"
#include "idiom/expand.h"
#include "idiom/ports.h"
#include "idiom/reader.h"
#include "idiom/vm.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *out) {
    fputs("usage:\n", out);
    fputs("  idiomc run <file|package|-> [--] [args...]\n", out);
    fputs("  idiomc <file|package|-> [args...]          run shorthand\n", out);
    fputs("  idiomc eval <source> [--] [args...]        evaluate source and print its value\n", out);
    fputs("  idiomc build <file|package|-> [-o OUT]\n", out);
    fputs("  idiomc test [path...]\n", out);
    fputs("  idiomc repl\n", out);
    fputs("  idiomc dump reader|core|bytecode <file|package|->\n", out);
    fputs("  idiomc dump surface [prelude]\n", out);
    fputs("  idiomc version\n", out);
    fputs("  idiomc help\n", out);
}

static bool read_source_arg(const char *arg, IdmRuntime *rt, char **source, const char **label, IdmError *err) {
    *source = NULL;
    *label = strcmp(arg, "-") == 0 ? "<stdin>" : arg;
    if (strcmp(arg, "-") == 0) {
        size_t len = 0;
        return idm_read_stream(stdin, "<stdin>", source, &len, err);
    }
    struct stat st;
    bool is_path = stat(arg, &st) == 0;
    if (!is_path || S_ISDIR(st.st_mode)) {
        IdmBuffer src;
        idm_buf_init(&src);
        const char *pkg_label = NULL;
        bool ok = idm_package_read_source(rt, arg, &src, &pkg_label, idm_span_unknown(arg), err);
        if (!ok) {
            idm_buf_destroy(&src);
            return false;
        }
        *source = idm_buf_take(&src);
        if (!*source) return idm_error_oom(err, idm_span_unknown(arg));
        *label = pkg_label ? pkg_label : arg;
        return true;
    }
    size_t len = 0;
    return idm_read_file(arg, source, &len, err);
}

static int dump_reader(const char *path) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    const char *file = NULL;
    if (!read_source_arg(path, &rt, &source, &file, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        idm_runtime_destroy(&rt);
        return 1;
    }
    IdmSyntax *program = NULL;
    if (!idm_reader_read_string(file, source, &program, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        free(source);
        idm_runtime_destroy(&rt);
        return 1;
    }
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_syn_dump(&buf, program) || !idm_buf_append_char(&buf, '\n')) {
        fputs("idiomc: out of memory while dumping reader syntax\n", stderr);
        idm_buf_destroy(&buf);
        idm_syn_free(program);
        free(source);
        idm_runtime_destroy(&rt);
        return 1;
    }
    fputs(buf.data, stdout);
    idm_buf_destroy(&buf);
    idm_syn_free(program);
    free(source);
    idm_runtime_destroy(&rt);
    return 0;
}

static bool expand_input(const char *path, IdmRuntime *rt, IdmCore **out, IdmError *err) {
    char *source = NULL;
    const char *file = NULL;
    if (!read_source_arg(path, rt, &source, &file, err)) return false;
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
    if (!idm_core_dump_pretty(&buf, core) || !idm_buf_append_char(&buf, '\n')) {
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

static char **g_cli_args = NULL;
static size_t g_cli_arg_count = 0;

static int run_sealed(const char *file, const unsigned char *data, size_t len);

static void set_cli_args(int argc, char **argv, int first) {
    if (first < argc && strcmp(argv[first], "--") == 0) first++;
    if (first < argc) {
        g_cli_args = argv + first;
        g_cli_arg_count = (size_t)(argc - first);
    } else {
        g_cli_args = NULL;
        g_cli_arg_count = 0;
    }
}

static int run_source(const char *file, const char *source, bool print_result) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    rt.cli_args = g_cli_args;
    rt.cli_arg_count = g_cli_arg_count;
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

static int run_source_arg(const char *path) {
    IdmRuntime loader;
    idm_runtime_init(&loader);
    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    const char *label = NULL;
    int status = 1;
    if (!read_source_arg(path, &loader, &source, &label, &err)) {
        idm_error_fprint(stderr, &err);
        goto done;
    }
    char *owned_label = idm_strdup(label ? label : path);
    if (!owned_label) {
        idm_error_oom(&err, idm_span_unknown(path));
        idm_error_fprint(stderr, &err);
        goto done;
    }
    status = run_source(owned_label, source, false);
    free(owned_label);
done:
    idm_error_clear(&err);
    free(source);
    idm_runtime_destroy(&loader);
    return status;
}

static int run_file(const char *path) {
    if (strcmp(path, "-") == 0) return run_source_arg(path);
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) return run_source_arg(path);

    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    size_t len = 0;
    if (!idm_read_file(path, &source, &len, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        return 1;
    }
    const unsigned char *p = (const unsigned char *)source;
    size_t remaining = len;
    if (remaining >= 2 && p[0] == '#' && p[1] == '!') {
        while (remaining > 0 && *p != '\n') { p++; remaining--; }
        if (remaining > 0) { p++; remaining--; }
    }
    if (remaining >= 20 && memcmp(p, "IDMX", 4u) == 0) {
        int status = run_sealed(path, p, remaining);
        free(source);
        return status;
    }
    int status = run_source(path, source, false);
    free(source);
    idm_error_clear(&err);
    return status;
}

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
            files[count] = idm_strdup(argv[i]);
            if (!files[count]) return 1;
            count++;
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

static int run_sealed(const char *file, const unsigned char *data, size_t len) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        rt.interactive = true;
        idm_job_control_init();
    }
    rt.cli_args = g_cli_args;
    rt.cli_arg_count = g_cli_arg_count;
    IdmError err;
    idm_error_init(&err);
    int status = 1;
    IdmBytecodeModule module;
    idm_bc_init(&module);
    IdmRepl *repl = NULL;
    char *package_path = NULL;
    IdmByteReader r = { data, len, 0u, true };
    r.pos = 4u;
    uint32_t version = idm_rd_u32(&r);
    uint32_t main_fn = idm_rd_u32(&r);
    uint64_t blob_len = idm_rd_u64(&r);
    uint64_t package_len = 0;
    uint64_t artifact_len = 0;
    if (version == 2u) {
        package_len = idm_rd_u64(&r);
        artifact_len = idm_rd_u64(&r);
    }
    if (!r.ok || (version != 1u && version != 2u) || blob_len > (uint64_t)(len - r.pos)) {
        idm_error_set(&err, idm_span_unknown(file), "corrupt sealed program header");
        goto done;
    }
    const unsigned char *blob_data = data + r.pos;
    r.pos += (size_t)blob_len;
    if (version == 2u && (package_len > (uint64_t)(len - r.pos) || artifact_len > (uint64_t)(len - r.pos - (size_t)package_len))) {
        idm_error_set(&err, idm_span_unknown(file), "corrupt sealed program header");
        goto done;
    }
    const unsigned char *package_data = data + r.pos;
    r.pos += (size_t)package_len;
    const unsigned char *artifact_data = data + r.pos;
    if (!idm_ic_deserialize(&rt, blob_data, (size_t)blob_len, &module, &err)) goto done;
    if (main_fn >= module.function_count) {
        idm_error_set(&err, idm_span_unknown(file), "sealed program main function is out of bounds");
        goto done;
    }
    repl = idm_repl_create(&rt, &err);
    if (!repl) goto done;
    if (version == 2u && package_len != 0u) {
        package_path = malloc((size_t)package_len + 1u);
        if (!package_path) {
            idm_error_oom(&err, idm_span_unknown(file));
            goto done;
        }
        memcpy(package_path, package_data, (size_t)package_len);
        package_path[package_len] = '\0';
        if (!idm_expand_preload_package_artifact(&rt, package_path, artifact_data, (size_t)artifact_len, &err)) goto done;
        IdmBuffer seed;
        idm_buf_init(&seed);
        bool seed_ok = idm_buf_append(&seed, "use ") && idm_buf_append(&seed, package_path) &&
            idm_buf_append(&seed, "\nactivate ") && idm_buf_append(&seed, package_path) && idm_buf_append(&seed, "\n");
        if (!seed_ok) {
            idm_buf_destroy(&seed);
            idm_error_oom(&err, idm_span_unknown(file));
            goto done;
        }
        bool seeded = idm_repl_seed_source(repl, seed.data, &err);
        idm_buf_destroy(&seed);
        if (!seeded) goto done;
    }
    IdmScheduler *sched = idm_repl_scheduler(repl);
    IdmValue thunk = idm_closure_in_module(&rt, &module, main_fn, NULL, 0, rt.main_ns, &err);
    if (err.present) goto done;
    IdmValue out = idm_nil();
    IdmValue reason = idm_nil();
    if (!idm_sched_run_session(sched, thunk, rt.interactive, &out, &reason, &err)) goto done;
    status = idm_sched_session_status(sched, out, reason);
done:
    if (err.present) idm_error_fprint(stderr, &err);
    idm_error_clear(&err);
    idm_repl_destroy(repl);
    idm_bc_destroy(&module);
    free(package_path);
    idm_runtime_destroy(&rt);
    return status;
}

static bool source_arg_package_like(const char *arg) {
    if (strcmp(arg, "-") == 0) return false;
    struct stat st;
    if (stat(arg, &st) != 0) return true;
    return S_ISDIR(st.st_mode);
}

static bool build_package_entry_source(const char *package_path, char **out_source, IdmError *err) {
    IdmBuffer src;
    idm_buf_init(&src);
    bool ok = idm_buf_append(&src, "use ")
        && idm_buf_append(&src, package_path)
        && idm_buf_append(&src, "\nmain args\n");
    if (!ok) {
        idm_buf_destroy(&src);
        return idm_error_oom(err, idm_span_unknown(package_path));
    }
    *out_source = idm_buf_take(&src);
    if (!*out_source) return idm_error_oom(err, idm_span_unknown(package_path));
    return true;
}

static int build_sealed(const char *src_path, const char *out_path) {
    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    const char *file = NULL;
    int status = 1;
    IdmCore *core = NULL;
    IdmSyntax *program = NULL;
    IdmSyntax *wrapped = NULL;
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    IdmBuffer blob;
    idm_buf_init(&blob);
    IdmBuffer artifact_blob;
    idm_buf_init(&artifact_blob);
    IdmBuffer out;
    idm_buf_init(&out);
    bool package_entry = source_arg_package_like(src_path);
    if (package_entry) {
        file = src_path;
        if (!build_package_entry_source(src_path, &source, &err)) goto done;
    } else if (!read_source_arg(src_path, &rt, &source, &file, &err)) goto done;
    if (!idm_reader_read_string(file, source, &program, &err)) goto done;
    size_t src_len = strlen(src_path);
    if (src_len >= 4 && strcmp(src_path + src_len - 4, ".ish") == 0) {
        wrapped = idm_syn_program_prepend_activate(program, "app/ish", file);
        if (!wrapped) { idm_error_oom(&err, idm_span_unknown(file)); goto done; }
    }
    if (!idm_expand_syntax(&rt, wrapped ? wrapped : program, &core, &err)) goto done;
    uint32_t main_fn = 0;
    if (!idm_core_compile_main(core, &module, &main_fn, &err)) goto done;
    if (!idm_ic_serialize(&module, &blob, &err)) goto done;
    if (package_entry && !idm_expand_package_artifact_serialize(&rt, src_path, &artifact_blob, &err)) goto done;
    uint32_t sealed_version = package_entry ? 2u : 1u;
    if (!idm_buf_append(&out, "#!/usr/bin/env idiomc\n") ||
        !idm_buf_append_n(&out, "IDMX", 4u) ||
        !idm_buf_put_u32(&out, sealed_version) ||
        !idm_buf_put_u32(&out, main_fn) ||
        !idm_buf_put_u64(&out, (uint64_t)blob.len)) {
        idm_error_oom(&err, idm_span_unknown(src_path));
        goto done;
    }
    if (package_entry &&
        (!idm_buf_put_u64(&out, (uint64_t)strlen(src_path)) ||
         !idm_buf_put_u64(&out, (uint64_t)artifact_blob.len))) {
        idm_error_oom(&err, idm_span_unknown(src_path));
        goto done;
    }
    if (!idm_buf_append_n(&out, blob.data, blob.len) ||
        (package_entry && (!idm_buf_append_n(&out, src_path, strlen(src_path)) ||
                           !idm_buf_append_n(&out, artifact_blob.data, artifact_blob.len)))) {
        idm_error_oom(&err, idm_span_unknown(src_path));
        goto done;
    }
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        idm_error_set(&err, idm_span_unknown(out_path), "cannot write '%s'", out_path);
        goto done;
    }
    bool wrote = fwrite(out.data, 1u, out.len, f) == out.len;
    if (fclose(f) != 0) wrote = false;
    if (!wrote) {
        idm_error_set(&err, idm_span_unknown(out_path), "cannot write '%s'", out_path);
        goto done;
    }
    chmod(out_path, 0755);
    printf("sealed %s (%zu bytes)\n", out_path, out.len);
    status = 0;
done:
    if (err.present) idm_error_fprint(stderr, &err);
    idm_error_clear(&err);
    idm_buf_destroy(&out);
    idm_buf_destroy(&artifact_blob);
    idm_buf_destroy(&blob);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_syn_free(wrapped);
    idm_syn_free(program);
    free(source);
    idm_runtime_destroy(&rt);
    return status;
}

static int dump_surface(const char *prelude) {
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

#ifndef IDM_VERSION
#define IDM_VERSION "0.0.0-dev"
#endif

static int run_repl(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    rt.interactive = isatty(0) != 0;
    IdmError err;
    idm_error_init(&err);
    int status = 1;
    IdmRepl *repl = idm_repl_create(&rt, &err);
    IdmValue thunk = idm_nil();
    if (repl && idm_repl_loop_thunk(repl, "use app/ish\nmain :plain", &thunk, &err)) {
        if (rt.interactive) printf("idiom %s — :quit or ^D to exit\n", IDM_VERSION);
        IdmValue value = idm_nil();
        IdmValue reason = idm_nil();
        IdmScheduler *sched = idm_repl_scheduler(repl);
        if (idm_sched_run_session(sched, thunk, false, &value, &reason, &err)) {
            status = idm_sched_session_status(sched, value, reason);
        }
    }
    if (err.present) idm_error_fprint(stderr, &err);
    idm_error_clear(&err);
    idm_repl_destroy(repl);
    idm_runtime_destroy(&rt);
    return status;
}

static int command_run(int argc, char **argv) {
    int path_index = 0;
    if (argc > 0 && strcmp(argv[0], "--") == 0) path_index = 1;
    if (path_index >= argc) {
        fprintf(stderr, "usage: idiomc run <file|package|-> [--] [args...]\n");
        return 64;
    }
    set_cli_args(argc, argv, path_index + 1);
    return run_file(argv[path_index]);
}

static int command_eval(int argc, char **argv) {
    int source_index = 0;
    if (argc > 0 && strcmp(argv[0], "--") == 0) source_index = 1;
    if (source_index >= argc) {
        fprintf(stderr, "usage: idiomc eval <source> [--] [args...]\n");
        return 64;
    }
    set_cli_args(argc, argv, source_index + 1);
    return run_source("<eval>", argv[source_index], true);
}

static int command_build(int argc, char **argv) {
    const char *src_path = NULL;
    const char *out_path = NULL;
    bool parse_options = true;
    for (int i = 0; i < argc; i++) {
        if (parse_options && strcmp(argv[i], "--") == 0) {
            parse_options = false;
            continue;
        }
        if (parse_options && strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "usage: idiomc build <file|package|-> [-o OUT]\n");
                return 64;
            }
            out_path = argv[++i];
            continue;
        }
        if (parse_options && argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
            fprintf(stderr, "idiomc build: unknown option '%s'\n", argv[i]);
            return 64;
        }
        if (src_path) {
            fprintf(stderr, "idiomc build: expected one source input\n");
            return 64;
        }
        src_path = argv[i];
    }
    if (!src_path) {
        fprintf(stderr, "usage: idiomc build <file|package|-> [-o OUT]\n");
        return 64;
    }
    char derived[1024];
    if (!out_path) {
        if (strcmp(src_path, "-") == 0) {
            fprintf(stderr, "idiomc build: stdin requires -o OUT\n");
            return 64;
        }
        snprintf(derived, sizeof(derived), "%s", src_path);
        char *dot = strrchr(derived, '.');
        if (dot && dot != derived) *dot = '\0';
        out_path = derived;
    }
    return build_sealed(src_path, out_path);
}

static int command_dump(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: idiomc dump reader|core|bytecode <file|package|->\n       idiomc dump surface [prelude]\n");
        return 64;
    }
    if (strcmp(argv[0], "surface") == 0) {
        if (argc > 2) {
            fprintf(stderr, "usage: idiomc dump surface [prelude]\n");
            return 64;
        }
        return dump_surface(argc == 2 ? argv[1] : "");
    }
    if (argc != 2) {
        fprintf(stderr, "usage: idiomc dump reader|core|bytecode <file|package|->\n");
        return 64;
    }
    if (strcmp(argv[0], "reader") == 0) return dump_reader(argv[1]);
    if (strcmp(argv[0], "core") == 0) return dump_core(argv[1]);
    if (strcmp(argv[0], "bytecode") == 0) return dump_bytecode(argv[1]);
    fprintf(stderr, "idiomc dump: unknown target '%s'\n", argv[0]);
    return 64;
}

int main(int argc, char **argv) {
    if (argc <= 1) {
        usage(stderr);
        return 64;
    }
    const char *cmd = argv[1];
    if ((strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0) && argc == 2) {
        usage(stdout);
        return 0;
    }
    if ((strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) && argc == 2) {
        printf("idiomc %s (idiom)\n", IDM_VERSION);
        return 0;
    }
    if (strcmp(cmd, "repl") == 0 && argc == 2) return run_repl();
    if (strcmp(cmd, "test") == 0) return run_tests(argc - 2, argv + 2);
    if (strcmp(cmd, "build") == 0) return command_build(argc - 2, argv + 2);
    if (strcmp(cmd, "run") == 0) return command_run(argc - 2, argv + 2);
    if (strcmp(cmd, "eval") == 0) return command_eval(argc - 2, argv + 2);
    if (strcmp(cmd, "dump") == 0) return command_dump(argc - 2, argv + 2);
    if (cmd[0] != '-' || strcmp(cmd, "-") == 0) {
        set_cli_args(argc, argv, 2);
        return run_file(cmd);
    }
    usage(stderr);
    return 64;
}
