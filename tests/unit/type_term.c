#include "idiom/value.h"
#include "idiom/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static void check(bool cond, const char *msg) {
    if (!cond) { fprintf(stderr, "type_term test failed: %s\n", msg); failures++; }
}

int idm_unit_type_term(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmTypeTerm v;
    check(idm_type_var(&rt, &v, "a", 1u, false), "make var");
    check(v.kind == IDM_TYPE_VAR && v.var_id == 1u && strcmp(idm_type_term_text(&v), "a") == 0, "var fields");

    IdmTypeTerm c;
    check(idm_type_con(&rt, &c, "int"), "make con");
    check(c.kind == IDM_TYPE_CON && c.arg_count == 0 && strcmp(idm_type_term_text(&c), "int") == 0, "con fields");

    IdmTypeTerm *args = calloc(1, sizeof(*args));
    idm_type_var(&rt, &args[0], "a", 1u, false);
    IdmTypeTerm box;
    check(idm_type_con_take(&rt, &box, "Box", args, 1u), "applied con");
    check(box.arg_count == 1u && idm_type_term_mentions(&box, "a"), "box mentions a");
    check(!idm_type_term_mentions(&box, "b"), "box not mentions b");

    IdmTypeTerm box2;
    check(idm_type_term_copy(&box2, &box), "copy");
    check(idm_type_term_equal(&box, &box2), "copy equal");

    IdmTypeTerm *u = calloc(2, sizeof(*u));
    idm_type_con(&rt, &u[0], "int");
    idm_type_con(&rt, &u[1], "string");
    IdmTypeTerm un;
    check(idm_type_compound(&un, IDM_TYPE_UNION, u, 2u), "union");
    IdmBuffer b;
    idm_buf_init(&b);
    check(idm_type_term_write(&b, &un) && b.data && strcmp(b.data, "(int | string)") == 0, "union write");
    idm_buf_destroy(&b);

    IdmBuffer b2;
    idm_buf_init(&b2);
    check(idm_type_term_write(&b2, &box) && b2.data && strcmp(b2.data, "Box<a>") == 0, "applied write");
    idm_buf_destroy(&b2);

    IdmTypeTerm v2;
    idm_type_var(&rt, &v2, "a", 2u, false);
    check(!idm_type_term_equal(&v, &v2), "var id distinguishes");

    IdmError err;
    idm_error_init(&err);
    IdmBuffer wire;
    idm_buf_init(&wire);
    check(idm_type_term_serialize(&wire, &box, &err), "serialize applied type term");
    check(!err.present, "serialize no error");
    IdmByteReader reader;
    idm_byte_reader_init(&reader, (const unsigned char *)(wire.data ? wire.data : ""), wire.len);
    IdmTypeTerm box3;
    check(idm_type_term_deserialize(&rt, &reader, &box3, &err), "deserialize applied type term");
    check(reader.ok && reader.pos == reader.len, "deserialize consumes type term");
    check(idm_type_term_equal(&box, &box3), "serialized type term equal");
    idm_type_term_destroy(&box3);
    idm_buf_destroy(&wire);
    idm_error_clear(&err);

    unsigned char identity_hash[32];
    memset(identity_hash, 0x5a, sizeof(identity_hash));
    IdmSymbol *identity = idm_intern_identity(&rt.intern, "Box", identity_hash);
    IdmTypeTerm nominal;
    check(idm_type_con_symbol(&nominal, identity), "make nominal identity term");
    idm_buf_init(&wire);
    check(idm_type_term_serialize(&wire, &nominal, &err), "serialize nominal identity term");
    idm_byte_reader_init(&reader, (const unsigned char *)(wire.data ? wire.data : ""), wire.len);
    IdmTypeTerm nominal2;
    check(idm_type_term_deserialize(&rt, &reader, &nominal2, &err), "deserialize nominal identity term");
    check(nominal2.symbol == identity, "nominal identity survives wire");
    idm_type_term_destroy(&nominal2);
    idm_buf_destroy(&wire);
    idm_error_clear(&err);

    IdmCallableContract ct;
    memset(&ct, 0, sizeof(ct));
    IdmContractSig *ctsig = idm_contract_add_sig(&ct);
    check(ctsig != NULL, "contract sig");
    ctsig->args = calloc(1, sizeof(*ctsig->args));
    idm_type_var(&rt, &ctsig->args[0], "a", 1u, false);
    ctsig->arg_count = 1u;
    idm_type_var(&rt, &ctsig->result, "a", 1u, false);
    ctsig->has_result = true;
    ct.context = calloc(1u, sizeof(*ct.context));
    ct.context_count = 1u;
    ct.context_cap = 1u;
    ct.context[0].kind = IDM_CONSTR_CLASS;
    ct.context[0].trait = identity;
    idm_type_var(&rt, &ct.context[0].lhs, "a", 1u, false);
    IdmCallableContract ct2;
    check(idm_callable_contract_copy(&ct2, &ct), "contract copy");
    check(ct2.sig_count == 1u && ct2.sigs[0].arg_count == 1u && ct2.sigs[0].has_result, "contract copy fields");
    idm_buf_init(&wire);
    check(idm_callable_contract_serialize(&wire, &ct, &err), "serialize identity constraint");
    idm_byte_reader_init(&reader, (const unsigned char *)(wire.data ? wire.data : ""), wire.len);
    IdmCallableContract ct3;
    check(idm_callable_contract_deserialize(&rt, &reader, &ct3, &err), "deserialize identity constraint");
    check(ct3.context_count == 1u && ct3.context[0].trait == identity, "constraint identity survives wire");
    idm_callable_contract_destroy(&ct3);
    idm_buf_destroy(&wire);
    idm_error_clear(&err);
    idm_callable_contract_destroy(&ct);
    idm_callable_contract_destroy(&ct2);

    IdmConstraint structural;
    memset(&structural, 0, sizeof(structural));
    structural.kind = IDM_CONSTR_STRUCTURAL;
    structural.structural.field = idm_intern(&rt.intern, IDM_SYMBOL_ATOM, "size");
    structural.structural.has_type = true;
    idm_type_con(&rt, &structural.structural.type, "int");
    idm_type_var(&rt, &structural.lhs, "a", 1u, false);
    IdmConstraint structural_copy;
    check(idm_constraint_copy(&structural_copy, &structural), "copy structural constraint");
    check(idm_structural_head_equal(&structural.structural, &structural_copy.structural), "structural head copy equal");
    idm_buf_init(&wire);
    check(idm_constraint_serialize(&wire, &structural, &err), "serialize structural constraint");
    idm_byte_reader_init(&reader, (const unsigned char *)(wire.data ? wire.data : ""), wire.len);
    IdmConstraint structural_wire;
    check(idm_constraint_deserialize(&rt, &reader, &structural_wire, &err), "deserialize structural constraint");
    check(structural_wire.kind == IDM_CONSTR_STRUCTURAL && idm_structural_head_equal(&structural.structural, &structural_wire.structural), "structural constraint survives wire");
    idm_constraint_destroy(&structural_wire);
    idm_buf_destroy(&wire);
    idm_error_clear(&err);
    idm_constraint_destroy(&structural_copy);
    idm_constraint_destroy(&structural);

    IdmValue one = idm_fixnum(1);
    IdmValue atom = idm_atom(&rt, "x");
    IdmTypeTerm any;
    IdmTypeTerm int_term;
    IdmTypeTerm integer_term;
    IdmTypeTerm atom_term;
    IdmTypeTerm atomx_term;
    check(idm_type_con(&rt, &any, "Any"), "make Any");
    check(idm_type_con(&rt, &int_term, "int"), "make int term");
    check(idm_type_con(&rt, &integer_term, "integer"), "make integer term");
    check(idm_type_con(&rt, &atom_term, "atom"), "make atom term");
    check(idm_type_con(&rt, &atomx_term, "atomx"), "make atomx term");
    check(idm_value_matches_type_term(one, &any), "Any matches int");
    check(idm_value_matches_type_term(one, &int_term), "int term matches int");
    check(!idm_value_matches_type_term(one, &integer_term), "integer term is not int prefix");
    check(idm_value_matches_type_term(atom, &atom_term), "atom term matches atom");
    check(!idm_value_matches_type_term(atom, &atomx_term), "atomx term is not atom prefix");
    idm_type_term_destroy(&any);
    idm_type_term_destroy(&int_term);
    idm_type_term_destroy(&integer_term);
    idm_type_term_destroy(&atom_term);
    idm_type_term_destroy(&atomx_term);
    idm_runtime_destroy(&rt);

    idm_type_term_destroy(&v);
    idm_type_term_destroy(&c);
    idm_type_term_destroy(&box);
    idm_type_term_destroy(&box2);
    idm_type_term_destroy(&un);
    idm_type_term_destroy(&v2);
    idm_type_term_destroy(&nominal);
    return failures;
}
