#include "idiom/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    unsigned char hash_a[32];
    unsigned char hash_b[32];
    memset(hash_a, 0x11, sizeof(hash_a));
    memset(hash_b, 0x22, sizeof(hash_b));
    IdmSymbol *identity_a = idm_intern_identity(&intern, "Thing", hash_a);
    IdmSymbol *identity_a2 = idm_intern_identity(&intern, "Thing", hash_a);
    IdmSymbol *identity_b = idm_intern_identity(&intern, "Thing", hash_b);
    check(identity_a && identity_a == identity_a2, "same identity interns once");
    check(identity_b && identity_b != identity_a, "full hash distinguishes same spelling");
    check(idm_symbol_kind(identity_a) == IDM_SYMBOL_IDENTITY, "identity kind");
    check(memcmp(idm_symbol_identity_hash(identity_a), hash_a, sizeof(hash_a)) == 0, "identity hash retained");
    check(intern.count == 4u, "final count");

    idm_intern_destroy(&intern);
    return 0;
}
