#include "idiom/actor.h"
#include "idiom/artifact.h"
#include "idiom/common.h"
#include "idiom/expand.h"
#include "idiom/ports.h"
#include "idiom/reader.h"
#include "idiom/vm.h"

#include <dirent.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *out) {
    fputs("usage:\n", out);
    fputs("  idiomc run <file|package|-> [--] [args...]\n", out);
    fputs("  idiomc <file|package|-> [args...]          run shorthand\n", out);
    fputs("  idiomc eval <source|-> [--] [args...]      evaluate source and print its value\n", out);
    fputs("  idiomc build <file|package|-> [-o OUT]\n", out);
    fputs("  idiomc test [-run PATTERN] [-bench PATTERN] [-count N] [-json] [path...]\n", out);
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
    if (!idm_expand_read_source_string(&rt, file, source, &program, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        free(source);
        idm_runtime_destroy(&rt);
        return 1;
    }
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_syn_dump_pretty(&buf, program) || !idm_buf_append_char(&buf, '\n')) {
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
    bool ok = idm_expand_source_string(rt, file, source, out, err);
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
    bool ok = idm_core_compile_main(rt, core, module, main_fn, err);
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

typedef struct {
    uint64_t expand_ns;
    uint64_t compile_ns;
    uint64_t intern_ns;
    uint64_t run_ns;
} RunTimings;

static uint64_t run_timing_total(const RunTimings *timing) {
    return timing->expand_ns + timing->compile_ns + timing->intern_ns + timing->run_ns;
}

static uint64_t ns_since(uint64_t start) {
    uint64_t end = idm_profile_now_ns();
    return end >= start ? end - start : 0u;
}

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

static int run_source_timed(const char *file, const char *source, bool print_result, RunTimings *timing) {
    if (timing) memset(timing, 0, sizeof(*timing));
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
    uint64_t start = idm_profile_now_ns();
    if (!idm_expand_source_string(&rt, file, source, &core, &err)) {
        if (timing) timing->expand_ns = ns_since(start);
        goto done;
    }
    if (timing) timing->expand_ns = ns_since(start);
    uint32_t main_fn = 0;
    start = idm_profile_now_ns();
    if (!idm_core_compile_main(&rt, core, &module, &main_fn, &err)) {
        if (timing) timing->compile_ns = ns_since(start);
        goto done;
    }
    if (timing) timing->compile_ns = ns_since(start);
    start = idm_profile_now_ns();
    if (!idm_bc_intern_literals(&rt, &module, &err)) {
        if (timing) timing->intern_ns = ns_since(start);
        goto done;
    }
    if (timing) timing->intern_ns = ns_since(start);
    start = idm_profile_now_ns();
    sched = idm_sched_create(&rt, &module, &err);
    if (!sched) {
        if (timing) timing->run_ns = ns_since(start);
        goto done;
    }
    IdmValue out = idm_nil();
    if (!idm_sched_run_main(sched, main_fn, &out, &err)) {
        if (timing) timing->run_ns = ns_since(start);
        goto done;
    }
    if (print_result) {
        IdmBuffer buf;
        idm_buf_init(&buf);
        if (!idm_value_write(&buf, out) || !idm_buf_append_char(&buf, '\n')) {
            idm_buf_destroy(&buf);
            idm_error_set(&err, idm_span_unknown(NULL), "out of memory while printing result");
            if (timing) timing->run_ns = ns_since(start);
            goto done;
        }
        fputs(buf.data, stdout);
        idm_buf_destroy(&buf);
    }
    if (timing) timing->run_ns = ns_since(start);
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

static int run_source(const char *file, const char *source, bool print_result) {
    return run_source_timed(file, source, print_result, NULL);
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
    if (remaining >= 12 && memcmp(p, IDM_WIRE_MAGIC, 4u) == 0) {
        int status = run_sealed(path, p, remaining);
        free(source);
        return status;
    }
    int status = run_source(path, source, false);
    free(source);
    idm_error_clear(&err);
    return status;
}

typedef struct {
    const char *run;
    const char *bench;
    const char *count;
    bool json;
} TestOptions;

static bool test_filter_match(const char *text, const char *pattern) {
    if (!pattern || pattern[0] == '\0' || strcmp(pattern, ".") == 0) return true;
    return strstr(text, pattern) != NULL;
}

static bool positive_decimal(const char *s) {
    if (!s || s[0] == '\0') return false;
    bool nonzero = false;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        if (*p != '0') nonzero = true;
    }
    return nonzero;
}

static int run_source_capture(const char *file, const char *source, bool print_result, char **out, size_t *out_len, RunTimings *timing) {
    *out = NULL;
    *out_len = 0;
    FILE *tmp = tmpfile();
    if (!tmp) {
        fprintf(stderr, "idiomc test: cannot create stdout capture\n");
        return 1;
    }
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    if (saved < 0) {
        fclose(tmp);
        fprintf(stderr, "idiomc test: cannot capture stdout\n");
        return 1;
    }
    if (dup2(fileno(tmp), STDOUT_FILENO) < 0) {
        close(saved);
        fclose(tmp);
        fprintf(stderr, "idiomc test: cannot redirect stdout\n");
        return 1;
    }
    int status = run_source_timed(file, source, print_result, timing);
    fflush(stdout);
    if (dup2(saved, STDOUT_FILENO) < 0) {
        close(saved);
        fclose(tmp);
        fprintf(stderr, "idiomc test: cannot restore stdout\n");
        return 1;
    }
    close(saved);
    rewind(tmp);
    IdmError err;
    idm_error_init(&err);
    if (!idm_read_stream(tmp, file, out, out_len, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        fclose(tmp);
        return 1;
    }
    idm_error_clear(&err);
    fclose(tmp);
    return status;
}

static void json_string(FILE *out, const char *s) {
    fputc('"', out);
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
                case '"': fputs("\\\"", out); break;
                case '\\': fputs("\\\\", out); break;
                case '\n': fputs("\\n", out); break;
                case '\r': fputs("\\r", out); break;
                case '\t': fputs("\\t", out); break;
                default:
                    if (*p < 32u) fprintf(out, "\\u%04x", *p);
                    else fputc((int)*p, out);
                    break;
            }
        }
    }
    fputc('"', out);
}

static void json_file_event(const char *action, const char *file) {
    fputs("{\"action\":", stdout);
    json_string(stdout, action);
    fputs(",\"file\":", stdout);
    json_string(stdout, file);
    fputs("}\n", stdout);
}

static void json_output_event(const char *file, const char *output) {
    if (!output || output[0] == '\0') return;
    fputs("{\"action\":\"output\",\"file\":", stdout);
    json_string(stdout, file);
    fputs(",\"output\":", stdout);
    json_string(stdout, output);
    fputs("}\n", stdout);
}

static void json_bench_file_event(const char *file, const RunTimings *timing) {
    fprintf(stdout,
            "{\"action\":\"benchfile\",\"file\":");
    json_string(stdout, file);
    fprintf(stdout,
            ",\"expand_ns\":%" PRIu64 ",\"compile_ns\":%" PRIu64 ",\"intern_ns\":%" PRIu64 ",\"run_ns\":%" PRIu64 ",\"total_ns\":%" PRIu64 "}\n",
            timing->expand_ns,
            timing->compile_ns,
            timing->intern_ns,
            timing->run_ns,
            run_timing_total(timing));
}

static void plain_bench_file_event(const char *file, const RunTimings *timing) {
    printf("benchfile file=%s expand_ns=%" PRIu64 " compile_ns=%" PRIu64 " intern_ns=%" PRIu64 " run_ns=%" PRIu64 " total_ns=%" PRIu64 "\n",
           file,
           timing->expand_ns,
           timing->compile_ns,
           timing->intern_ns,
           timing->run_ns,
           run_timing_total(timing));
}

static void bench_field_copy(const char *line, const char *key, char *out, size_t out_cap) {
    if (out_cap == 0) return;
    out[0] = '\0';
    size_t key_len = strlen(key);
    const char *p = line;
    while (*p) {
        while (*p == ' ') p++;
        const char *tok = p;
        while (*p && *p != ' ') p++;
        if (strncmp(tok, key, key_len) == 0 && tok[key_len] == '=') {
            const char *value = tok + key_len + 1u;
            size_t len = (size_t)(p - value);
            if (len >= out_cap) len = out_cap - 1u;
            memcpy(out, value, len);
            out[len] = '\0';
            return;
        }
    }
}

static bool json_bench_event(const char *file, char *line) {
    if (strncmp(line, "bench ", 6u) != 0) return false;
    const char *fields = line + 6u;
    char name[256], suite[128], n[32], ms[32], result[128], expect[128];
    bench_field_copy(fields, "name", name, sizeof(name));
    bench_field_copy(fields, "suite", suite, sizeof(suite));
    bench_field_copy(fields, "n", n, sizeof(n));
    bench_field_copy(fields, "ms", ms, sizeof(ms));
    bench_field_copy(fields, "result", result, sizeof(result));
    bench_field_copy(fields, "expect", expect, sizeof(expect));
    fputs("{\"action\":\"bench\",\"file\":", stdout);
    json_string(stdout, file);
    fputs(",\"name\":", stdout);
    json_string(stdout, name);
    fputs(",\"suite\":", stdout);
    json_string(stdout, suite);
    fprintf(stdout, ",\"n\":%s,\"ms\":%s,\"result\":", n[0] ? n : "0", ms[0] ? ms : "0");
    json_string(stdout, result);
    fputs(",\"expect\":", stdout);
    json_string(stdout, expect);
    fputs("}\n", stdout);
    return true;
}

static size_t emit_bench_output(const char *file, char *output, bool json) {
    size_t count = 0;
    char *cursor = output;
    while (cursor && *cursor) {
        char *line = cursor;
        char *nl = strchr(cursor, '\n');
        if (nl) {
            *nl = '\0';
            cursor = nl + 1;
        } else {
            cursor = NULL;
        }
        if (strncmp(line, "bench ", 6u) == 0) {
            count++;
            if (json) {
                json_bench_event(file, line);
            } else {
                fputs(line, stdout);
                fputc('\n', stdout);
            }
        } else if (line[0] != '\0') {
            if (json) json_output_event(file, line);
            else {
                fputs(line, stdout);
                fputc('\n', stdout);
            }
        }
    }
    return count;
}

static int collect_test_files(const char *dir_path, char ***out_files, size_t *out_count, size_t *out_cap) {
    DIR *dir = opendir(dir_path);
    if (!dir) return -1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 3, ".id") != 0) continue;
        if (*out_count == *out_cap) {
            if (!idm_grow((void **)out_files, out_cap, sizeof(**out_files), 16u, *out_count + 1u)) { closedir(dir); return -1; }
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

static bool test_add_path(char ***paths, size_t *count, size_t *cap, char *path) {
    if (*count == *cap && !idm_grow((void **)paths, cap, sizeof(**paths), 8u, *count + 1u)) return false;
    (*paths)[(*count)++] = path;
    return true;
}

static int test_collect_path(const char *path, char ***files, size_t *count, size_t *cap, const char *file_filter) {
    size_t before = *count;
    if (collect_test_files(path, files, count, cap) == 0) {
        size_t write = before;
        for (size_t i = before; i < *count; i++) {
            if (test_filter_match((*files)[i], file_filter)) {
                (*files)[write++] = (*files)[i];
            } else {
                free((*files)[i]);
            }
        }
        *count = write;
        return 0;
    }
    if (!test_filter_match(path, file_filter)) return 0;
    char *copy = idm_strdup(path);
    if (!copy) return -1;
    if (!test_add_path(files, count, cap, copy)) {
        free(copy);
        return -1;
    }
    return 0;
}

static int run_tests(int argc, char **argv) {
    TestOptions opt = {0};
    char **paths = NULL;
    size_t path_count = 0;
    size_t path_cap = 0;
    bool parse_options = true;
    for (int i = 0; i < argc; i++) {
        if (parse_options && strcmp(argv[i], "--") == 0) {
            parse_options = false;
            continue;
        }
        if (parse_options && strcmp(argv[i], "-run") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "usage: idiomc test [-run PATTERN] [-bench PATTERN] [-count N] [-json] [path...]\n");
                free(paths);
                return 64;
            }
            opt.run = argv[++i];
            continue;
        }
        if (parse_options && strcmp(argv[i], "-bench") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "usage: idiomc test [-run PATTERN] [-bench PATTERN] [-count N] [-json] [path...]\n");
                free(paths);
                return 64;
            }
            opt.bench = argv[++i];
            continue;
        }
        if (parse_options && strcmp(argv[i], "-count") == 0) {
            if (i + 1 >= argc || !positive_decimal(argv[i + 1])) {
                fprintf(stderr, "usage: idiomc test [-run PATTERN] [-bench PATTERN] [-count N] [-json] [path...]\n");
                free(paths);
                return 64;
            }
            opt.count = argv[++i];
            continue;
        }
        if (parse_options && strcmp(argv[i], "-json") == 0) {
            opt.json = true;
            continue;
        }
        if (parse_options && argv[i][0] == '-') {
            fprintf(stderr, "idiomc test: unknown option '%s'\n", argv[i]);
            free(paths);
            return 64;
        }
        if (!test_add_path(&paths, &path_count, &path_cap, argv[i])) {
            free(paths);
            return 1;
        }
    }
    char **files = NULL;
    size_t count = 0;
    size_t cap = 0;
    const char *default_path = opt.bench ? "tests/bench" : "tests/lang";
    if (path_count == 0) {
        if (test_collect_path(default_path, &files, &count, &cap, NULL) != 0) {
            fprintf(stderr, "idiomc test: cannot read %s\n", default_path);
            free(paths);
            return 1;
        }
    } else {
        for (size_t i = 0; i < path_count; i++) {
            if (test_collect_path(paths[i], &files, &count, &cap, NULL) != 0) {
                fprintf(stderr, "idiomc test: cannot read %s\n", paths[i]);
                for (size_t j = 0; j < count; j++) free(files[j]);
                free(files);
                free(paths);
                return 1;
            }
        }
    }
    free(paths);
    if (count == 0) {
        fprintf(stderr, "idiomc test: no test files found\n");
        free(files);
        return 1;
    }
    qsort(files, count, sizeof(*files), name_cmp);

    const char *mode_value = opt.bench ? "bench" : "test";
    const char *run_value = opt.run ? opt.run : "";
    const char *bench_value = opt.bench ? opt.bench : "";
    const char *count_value = opt.count ? opt.count : (opt.bench ? "1000" : "1");
    (void)setenv("IDIOM_TEST_MODE", mode_value, 1);
    (void)setenv("IDIOM_TEST_RUN", run_value, 1);
    (void)setenv("IDIOM_TEST_BENCH", bench_value, 1);
    (void)setenv("IDIOM_TEST_COUNT", count_value, 1);

    size_t failed = 0;
    size_t benches = 0;
    for (size_t i = 0; i < count; i++) {
        IdmError err;
        idm_error_init(&err);
        char *source = NULL;
        char *output = NULL;
        size_t len = 0;
        size_t output_len = 0;
        RunTimings timing = {0};
        int status = 1;
        if (idm_read_file(files[i], &source, &len, &err)) {
            status = run_source_capture(files[i], source, false, &output, &output_len, &timing);
        } else {
            idm_error_fprint(stderr, &err);
        }
        if (opt.bench) {
            benches += emit_bench_output(files[i], output ? output : "", opt.json);
            if (opt.json) json_bench_file_event(files[i], &timing);
            else plain_bench_file_event(files[i], &timing);
        } else if (opt.json) {
            json_output_event(files[i], output ? output : "");
        } else if (output && output_len != 0) {
            fputs(output, stdout);
        }
        if (status != 0) {
            if (opt.json) json_file_event("fail", files[i]);
            else fprintf(stderr, "FAIL %s\n", files[i]);
            failed++;
        } else if (!opt.bench) {
            if (opt.json) json_file_event("pass", files[i]);
            else printf("pass %s\n", files[i]);
        }
        free(output);
        free(source);
        idm_error_clear(&err);
    }

    if (!opt.json) {
        if (failed == 0 && opt.bench) {
            printf("idiomc test: %zu benchmark%s passed in %zu file%s\n", benches, benches == 1 ? "" : "s", count, count == 1 ? "" : "s");
        } else if (failed == 0) {
            printf("idiomc test: %zu file%s passed\n", count, count == 1 ? "" : "s");
        } else {
            printf("idiomc test: %zu of %zu file%s FAILED\n", failed, count, count == 1 ? "" : "s");
        }
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
    IdmScheduler *direct_sched = NULL;
    char *package_path = NULL;
    IdmByteReader r;
    uint32_t section_count = 0;
    if (!idm_wire_open(&r, data, len, &section_count, &err)) goto done;
    const unsigned char *main_data = NULL;
    size_t main_len = 0;
    const unsigned char *blob_data = NULL;
    size_t blob_len = 0;
    const unsigned char *package_data = NULL;
    size_t package_len = 0;
    const unsigned char *artifact_data = NULL;
    size_t artifact_len = 0;
    for (uint32_t i = 0; i < section_count; i++) {
        uint32_t kind = 0;
        const unsigned char *payload = NULL;
        size_t plen = 0;
        if (!idm_wire_next(&r, &kind, &payload, &plen, &err)) goto done;
        switch (kind) {
            case IDM_WIRE_SECTION_MAIN: main_data = payload; main_len = plen; break;
            case IDM_WIRE_SECTION_BYTECODE: blob_data = payload; blob_len = plen; break;
            case IDM_WIRE_SECTION_PACKAGE_PATH: package_data = payload; package_len = plen; break;
            case IDM_WIRE_SECTION_PACKAGE: artifact_data = payload; artifact_len = plen; break;
            default: break;
        }
    }
    if (!main_data || main_len != 4u || !blob_data) {
        idm_error_set(&err, idm_span_unknown(file), "sealed program is missing its main or bytecode section");
        goto done;
    }
    IdmByteReader mr = { main_data, main_len, 0u, true };
    uint32_t main_fn = idm_rd_u32(&mr);
    bool package_mode = package_data != NULL;
    if (!idm_ic_deserialize(&rt, blob_data, blob_len, &module, &err)) goto done;
    if (main_fn >= module.function_count) {
        idm_error_set(&err, idm_span_unknown(file), "sealed program main function is out of bounds");
        goto done;
    }
    if (!package_mode) {
        direct_sched = idm_sched_create(&rt, &module, &err);
        if (!direct_sched) goto done;
        IdmValue thunk = idm_closure_in_module(&rt, &module, main_fn, NULL, 0, rt.main_env, &err);
        if (err.present) goto done;
        IdmValue out = idm_nil();
        IdmValue reason = idm_nil();
        if (!idm_sched_run_session(direct_sched, thunk, rt.interactive, &out, &reason, &err)) goto done;
        status = idm_sched_session_status(direct_sched, out, reason);
        goto done;
    }
    repl = idm_repl_create(&rt, &err);
    if (!repl) goto done;
    if (package_mode && package_len != 0u) {
        package_path = malloc(package_len + 1u);
        if (!package_path) {
            idm_error_oom(&err, idm_span_unknown(file));
            goto done;
        }
        memcpy(package_path, package_data, package_len);
        package_path[package_len] = '\0';
        if (!idm_expand_preload_package_artifact(&rt, package_path, artifact_data, artifact_len, &err)) goto done;
        IdmBuffer seed;
        idm_buf_init(&seed);
        bool seed_ok = idm_buf_append(&seed, "use ") && idm_buf_append(&seed, package_path) && idm_buf_append(&seed, "\n");
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
    IdmValue thunk = idm_closure_in_module(&rt, &module, main_fn, NULL, 0, rt.main_env, &err);
    if (err.present) goto done;
    IdmValue out = idm_nil();
    IdmValue reason = idm_nil();
    if (!idm_sched_run_session(sched, thunk, rt.interactive, &out, &reason, &err)) goto done;
    status = idm_sched_session_status(sched, out, reason);
done:
    if (err.present) idm_error_fprint(stderr, &err);
    idm_error_clear(&err);
    idm_sched_destroy(direct_sched);
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
        && idm_buf_append(&src, "\nuse std/system with [args]\nmain args\n");
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
    if (!idm_expand_read_source_string(&rt, file, source, &program, &err)) goto done;
    if (!idm_expand_syntax(&rt, program, &core, &err)) goto done;
    uint32_t main_fn = 0;
    if (!idm_core_compile_main(&rt, core, &module, &main_fn, &err)) goto done;
    if (!idm_ic_serialize(&module, &blob, &err)) goto done;
    if (package_entry && !idm_expand_package_artifact_serialize(&rt, src_path, &artifact_blob, &err)) goto done;
    IdmBuffer main_meta;
    idm_buf_init(&main_meta);
    bool sealed_ok = idm_buf_put_u32(&main_meta, main_fn) &&
                     idm_buf_append(&out, "#!/usr/bin/env idiomc\n") &&
                     idm_wire_begin(&out, package_entry ? 4u : 2u, &err) &&
                     idm_wire_section(&out, IDM_WIRE_SECTION_MAIN, main_meta.data, main_meta.len, &err) &&
                     idm_wire_section(&out, IDM_WIRE_SECTION_BYTECODE, blob.data, blob.len, &err) &&
                     (!package_entry ||
                      (idm_wire_section(&out, IDM_WIRE_SECTION_PACKAGE_PATH, src_path, strlen(src_path), &err) &&
                       idm_wire_section(&out, IDM_WIRE_SECTION_PACKAGE, artifact_blob.data, artifact_blob.len, &err)));
    idm_buf_destroy(&main_meta);
    if (!sealed_ok) {
        if (!err.present) idm_error_oom(&err, idm_span_unknown(src_path));
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
    idm_syn_free(program);
    free(source);
    idm_runtime_destroy(&rt);
    return status;
}

static int dump_surface(const char *prelude) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBuffer out;
    idm_buf_init(&out);
    int status = 1;
    if (idm_expand_surface_dump(&rt, prelude, &out, &err)) {
        fputs(out.data ? out.data : "", stdout);
        status = 0;
    } else if (err.present) {
        idm_error_fprint(stderr, &err);
    }
    idm_buf_destroy(&out);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    return status;
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
    if (!repl) goto done;
    IdmValue thunk = idm_nil();
    if (idm_repl_loop_thunk(repl, "use std/repl\nmain\n", &thunk, &err)) {
        IdmValue value = idm_nil();
        IdmValue reason = idm_nil();
        IdmScheduler *sched = idm_repl_scheduler(repl);
        if (idm_sched_run_session(sched, thunk, rt.interactive, &value, &reason, &err)) {
            status = idm_sched_session_status(sched, value, reason);
        }
    }
done:
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
        fprintf(stderr, "usage: idiomc eval <source|-> [--] [args...]\n");
        return 64;
    }
    set_cli_args(argc, argv, source_index + 1);
    if (strcmp(argv[source_index], "-") == 0) {
        IdmError err;
        idm_error_init(&err);
        char *source = NULL;
        size_t len = 0;
        if (!idm_read_stream(stdin, "<stdin>", &source, &len, &err)) {
            idm_error_fprint(stderr, &err);
            idm_error_clear(&err);
            return 1;
        }
        int status = run_source("<stdin>", source, true);
        free(source);
        idm_error_clear(&err);
        return status;
    }
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
