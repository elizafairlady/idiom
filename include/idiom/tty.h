#ifndef IDM_TTY_H
#define IDM_TTY_H

#include "idiom/value.h"

bool idm_prim_tty_pred(IdmRuntime *rt, IdmValue *out, IdmError *err);
bool idm_prim_tty_raw(IdmRuntime *rt, IdmValue *out, IdmError *err);
bool idm_prim_tty_restore(IdmRuntime *rt, IdmValue *out, IdmError *err);
bool idm_prim_tty_write(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err);
bool idm_prim_tty_size(IdmRuntime *rt, IdmValue *out, IdmError *err);
int idm_tty_in_fd(void);

#endif
