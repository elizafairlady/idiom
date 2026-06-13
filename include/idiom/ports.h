#ifndef IDM_PORTS_H
#define IDM_PORTS_H

#include "idiom/value.h"
#include "idiom/vm.h"

typedef struct IdmPort IdmPort;

void idm_job_control_init(void);
IdmPort *idm_port_launch(IdmRuntime *rt, IdmValue graph, const IdmExec *exec_ctx, IdmError *err);
size_t idm_port_live_fds(const IdmPort *port, int *out_fds, size_t max);
void idm_port_drain(IdmPort *port);
bool idm_port_try_complete(IdmPort *port);
bool idm_port_stopped(const IdmPort *port);
bool idm_port_foreground(const IdmPort *port);
void idm_port_resume(IdmPort *port, bool fg);
void idm_port_signal_group(IdmPort *port, int signo);
IdmValue idm_port_result(IdmPort *port, IdmRuntime *rt, IdmError *err);
void idm_port_free(IdmPort *port);

#endif
