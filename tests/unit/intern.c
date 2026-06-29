#include "idiom/value.h"

#include <stdio.h>
#include <stdlib.h>

static void fail(const char *name) {
    fprintf(stderr, "intern: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

int idm_unit_intern(void) {
    IdmIntern intern;
    idm_intern_init(&intern);

    check(idm_intern_lookup(&intern, IDM_SYMBOL_ATOM, "alpha") == NULL, "missing atom");
    check(intern.count == 0u, "missing lookup allocates nothing");

    IdmSymbol *atom = idm_intern(&intern, IDM_SYMBOL_ATOM, "alpha");
    check(atom != NULL, "intern atom");
    check(idm_intern_lookup(&intern, IDM_SYMBOL_ATOM, "alpha") == atom, "atom lookup");
    check(intern.count == 1u, "atom lookup keeps count");

    check(idm_intern_lookup(&intern, IDM_SYMBOL_WORD, "alpha") == NULL, "kind separated");
    check(intern.count == 1u, "kind miss allocates nothing");

    IdmSymbol *word = idm_intern(&intern, IDM_SYMBOL_WORD, "alpha");
    check(word != NULL, "intern word");
    check(word != atom, "word atom differ");
    check(idm_intern_lookup(&intern, IDM_SYMBOL_WORD, "alpha") == word, "word lookup");
    check(idm_intern_lookup(&intern, IDM_SYMBOL_ATOM, "beta") == NULL, "second miss");
    check(intern.count == 2u, "final count");

    idm_intern_destroy(&intern);
    return 0;
}
