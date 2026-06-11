#ifndef ISH_PORTS_H
#define ISH_PORTS_H

#include "ish/value.h"
#include "ish/vm.h"

typedef struct IshPort IshPort;

IshPort *ish_port_launch(IshRuntime *rt, IshValue graph, const IshExec *exec_ctx, IshError *err);
size_t ish_port_live_fds(const IshPort *port, int *out_fds, size_t max);
void ish_port_drain(IshPort *port);
bool ish_port_try_complete(IshPort *port);
IshValue ish_port_result(IshPort *port, IshRuntime *rt, IshError *err);
void ish_port_free(IshPort *port);

#endif
