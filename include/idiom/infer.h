#ifndef IDIOM_INFER_H
#define IDIOM_INFER_H

#include "idiom/scope.h"

typedef struct {
    uint32_t var_id;
    IdmTypeTerm term;
} IdmSubstEntry;

typedef struct {
    IdmSubstEntry *items;
    size_t count;
    size_t cap;
} IdmSubst;

typedef struct {
    bool (*name_eq)(void *user, const char *a, const char *b);
    bool (*trait_eq)(void *user, const char *a, const char *b);
    bool (*given_matches)(void *user, const IdmConstraint *given, const char *trait, const IdmTypeTerm *lhs);
    void *user;
} IdmTypeCmp;

void idm_subst_init(IdmSubst *s);
void idm_subst_destroy(IdmSubst *s);
bool idm_subst_apply(const IdmSubst *s, const IdmTypeTerm *in, IdmTypeTerm *out);
bool idm_subst_occurs(const IdmSubst *s, uint32_t var_id, const IdmTypeTerm *term);
bool idm_subst_widen(IdmSubst *s, const IdmTypeTerm *var, const IdmTypeTerm *widened);

bool idm_unify(IdmSubst *s, const IdmTypeCmp *cmp, const IdmTypeTerm *a, const IdmTypeTerm *b, IdmError *err, IdmSpan span);

typedef enum { IDM_INST_YES, IDM_INST_NO, IDM_INST_UNKNOWN } IdmInstanceResult;
typedef IdmInstanceResult (*IdmInstanceOracle)(void *user, const char *trait, const IdmTypeTerm *ty);

typedef struct {
    IdmConstraint *items;
    size_t count;
    size_t cap;
} IdmConstraintSet;

void idm_constraint_set_init(IdmConstraintSet *cs);
void idm_constraint_set_destroy(IdmConstraintSet *cs);
bool idm_constraint_set_add(IdmConstraintSet *cs, const IdmConstraint *c);
bool idm_constraint_set_add_eq(IdmConstraintSet *cs, const IdmTypeTerm *a, const IdmTypeTerm *b);
bool idm_constraint_set_add_class(IdmConstraintSet *cs, const char *trait, const IdmTypeTerm *ty);

bool idm_solve(IdmSubst *s,
               const IdmTypeCmp *cmp,
               const IdmConstraintSet *given,
               const IdmConstraintSet *wanted,
               IdmInstanceOracle oracle, void *oracle_user,
               IdmConstraintSet *residual,
               IdmError *err, IdmSpan span);

bool idm_free_flex_vars(const IdmSubst *s, const IdmTypeTerm *term,
                        const uint32_t *bound, size_t bound_count,
                        uint32_t **out_ids, size_t *out_count, size_t *out_cap);

#endif
