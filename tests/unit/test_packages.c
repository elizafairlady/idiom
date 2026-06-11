#include "test_util.h"

static void test_kernel_runtime_exports(void) {
    const char *old = getenv("ISH_STD");
    char *saved = old ? ish_strdup(old) : NULL;
    setenv("ISH_STD", "tests/fixtures/kernelx", 1);
    IshRuntime rt;
    ish_runtime_init(&rt);
    check_value_written(&rt, "kernel-answer 41\n", "42");
    check_value_written(&rt, "kernel-answer (kernel-answer 40)\n", "42");
    expect_expand_error_rt(&rt, "<kernel-private-hidden>", "kernel-hidden 1\n", "unbound identifier 'kernel-hidden'");
    ish_runtime_destroy(&rt);
    if (saved) {
        setenv("ISH_STD", saved, 1);
        free(saved);
    } else {
        unsetenv("ISH_STD");
    }
}

static void test_surface_forms_are_kernel_sourced(void) {
    const char *old = getenv("ISH_STD");
    char *saved = old ? ish_strdup(old) : NULL;
    setenv("ISH_STD", "tests/fixtures/kernelx", 1);
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_expand_error_rt(&rt, "<match-is-kernel>", "match 1 do\n  1 -> :one\nend\n", "unbound identifier 'match'");
    expect_expand_error_rt(&rt, "<error-is-kernel>", "error :boom\n", "unbound identifier 'error'");
    expect_expand_error_rt(&rt, "<if-is-kernel>", "if :true do 1 else do 2 end\n", "unbound identifier 'if'");
    expect_expand_error_rt(&rt, "<and-is-kernel>", "and 1 2\n", "unbound identifier 'and'");
    expect_expand_error_rt(&rt, "<operator-is-kernel>", "operator glue precedence: 50 -> add\n", "unbound identifier 'operator'");
    expect_expand_error_rt(&rt, "<infix-ops-are-kernel>", "1 + 2\n", "unbound identifier '+'");
    ish_runtime_destroy(&rt);
    if (saved) {
        setenv("ISH_STD", saved, 1);
        free(saved);
    } else {
        unsetenv("ISH_STD");
    }
}

static void test_artifact_cache_relocation(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    check_value_written(&rt, "use tests/pkg/macropriv\nphase-answer x\n", "77");
    check_value_written(&rt, "use tests/pkg/macropriv\ninc-private 41\n", "42");
    expect_expand_error_rt(&rt, "<cached-private-still-hidden>",
        "use tests/pkg/macropriv\n"
        "hidden 1\n",
        "unbound identifier 'hidden'");
    check_value_written(&rt, "use tests/pkg/exporter\n3 <+> 4\nanswer x\n", "99");
    check_value_written(&rt, "use tests/pkg/exporter\n3 <+> 4\n", "7");
    check_value_written(&rt,
        "implements std/shell\n"
        "x = do\n"
        "  implements std/shell\n"
        "  1\n"
        "end\n"
        "add x 1\n",
        "2");
    ish_runtime_destroy(&rt);
}

static void test_ishc_cache_invalidation(void) {
    remove("build/cachepkg.ishc");
    CHECK(write_text_file("build/cachepkg.ish", "package cachepkg\nexport defn cval x -> add x 1\n"));
    check_pkg_value("use build/cachepkg\ncval 41\n", "42");
    FILE *probe = fopen("build/cachepkg.ishc", "rb");
    CHECK(probe != NULL);
    if (probe) fclose(probe);
    check_pkg_value("use build/cachepkg\ncval 41\n", "42");

    CHECK(write_text_file("build/cachepkg.ishc", "garbage that is not an artifact"));
    check_pkg_value("use build/cachepkg\ncval 41\n", "42");

    CHECK(write_text_file("build/cachepkg.ish", "package cachepkg\nexport defn cval x -> add x 2\n"));
    check_pkg_value("use build/cachepkg\ncval 41\n", "43");

    remove("build/cacheinner.ishc");
    remove("build/cacheouter.ishc");
    CHECK(write_text_file("build/cacheinner.ish", "package cacheinner\nexport defn ival x -> 1\n"));
    CHECK(write_text_file("build/cacheouter.ish", "package cacheouter\nuse build/cacheinner\nexport defn oval x -> add x (ival x)\n"));
    check_pkg_value("use build/cacheouter\noval 41\n", "42");
    CHECK(write_text_file("build/cacheinner.ish", "package cacheinner\nexport defn ival x -> 10\n"));
    check_pkg_value("use build/cacheouter\noval 41\n", "51");

    remove("build/cachepkg.ishc");
    setenv("ISH_ISHC", "0", 1);
    check_pkg_value("use build/cachepkg\ncval 41\n", "43");
    probe = fopen("build/cachepkg.ishc", "rb");
    CHECK(probe == NULL);
    if (probe) fclose(probe);
    unsetenv("ISH_ISHC");

    const char *old_std = getenv("ISH_STD");
    char *saved = old_std ? ish_strdup(old_std) : NULL;
    CHECK(mkdir("build/kernelfix", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir("build/kernelfix/kernel", 0755) == 0 || errno == EEXIST);
    remove("build/kernelfix/kernel.ishc");
    remove("build/cachek.ishc");
    CHECK(write_text_file("build/kernelfix/kernel/00-base.ish", "export defn kfix x -> add x 1\n"));
    CHECK(write_text_file("build/cachek.ish", "package cachek\nexport defn kval x -> kfix x\n"));
    setenv("ISH_STD", "build/kernelfix", 1);
    check_pkg_value("use build/cachek\nkval 41\n", "42");
    CHECK(write_text_file("build/kernelfix/kernel/00-base.ish", "export defn kfix x -> add x 100\n"));
    check_pkg_value("use build/cachek\nkval 41\n", "141");
    if (saved) {
        setenv("ISH_STD", saved, 1);
        free(saved);
    } else {
        unsetenv("ISH_STD");
    }
}

static void test_ishc_std_root_identity(void) {
    const char *old_std = getenv("ISH_STD");
    char *saved = old_std ? ish_strdup(old_std) : NULL;
    CHECK(mkdir("build/stdA", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir("build/stdA/kernel", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir("build/stdB", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir("build/stdB/kernel", 0755) == 0 || errno == EEXIST);
    CHECK(write_text_file("build/stdA/kernel/00-base.ish", "export defn kfix x -> add x 1\n"));
    CHECK(write_text_file("build/stdB/kernel/00-base.ish", "export defn kfix x -> add x 100\n"));
    CHECK(write_text_file("build/stdpkg.ish", "package stdpkg\nexport defn sval x -> kfix x\n"));
    remove("build/stdpkg.ishc");
    remove("build/stdA/kernel.ishc");
    remove("build/stdB/kernel.ishc");
    setenv("ISH_STD", "build/stdA", 1);
    check_pkg_value("use build/stdpkg\nsval 41\n", "42");
    setenv("ISH_STD", "build/stdB", 1);
    check_pkg_value("use build/stdpkg\nsval 41\n", "141");
    setenv("ISH_STD", "build/stdA", 1);
    check_pkg_value("use build/stdpkg\nsval 41\n", "42");
    if (saved) {
        setenv("ISH_STD", saved, 1);
        free(saved);
    } else {
        unsetenv("ISH_STD");
    }

    remove("build/cachenamed.ishc");
    CHECK(write_text_file("build/cachenamed.ish", "package cachenamed\nexport defn nval x -> add x 5\nexport defmacro nmac s -> (make-syntax-int s 77)\n"));
    check_pkg_value("use build/cachenamed\nnval 37\n", "42");
    check_pkg_value("import build/cachenamed as N\nN.nval 37\n", "42");
    check_pkg_value("import build/cachenamed as N\nN.nmac x\n", "77");
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_expand_error_rt(&rt, "<cached-import-no-unqualified-leak>",
        "import build/cachenamed as N\n"
        "nval 37\n",
        "unbound identifier 'nval'");
    expect_expand_error_rt(&rt, "<cached-import-no-unqualified-macro-leak>",
        "import build/cachenamed as N\n"
        "nmac x\n",
        "unbound identifier 'nmac'");
    ish_runtime_destroy(&rt);
}

static void test_package_cycle_detected(void) {
    CHECK(write_text_file("build/cyclea.ish", "package cyclea\nuse build/cycleb\nexport defn aval x -> x\n"));
    CHECK(write_text_file("build/cycleb.ish", "package cycleb\nuse build/cyclea\nexport defn bval x -> x\n"));
    remove("build/cyclea.ishc");
    remove("build/cycleb.ishc");
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_expand_error_rt(&rt, "<package-cycle>", "use build/cyclea\naval 1\n", "package dependency cycle");
    ish_runtime_destroy(&rt);
}

void run_package_suite(void) {
    test_kernel_runtime_exports();
    test_surface_forms_are_kernel_sourced();
    test_artifact_cache_relocation();
    test_ishc_cache_invalidation();
    test_ishc_std_root_identity();
    test_package_cycle_detected();
}
