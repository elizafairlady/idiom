#ifndef IDM_PORTS_H
#define IDM_PORTS_H

#include "idiom/value.h"
#include "idiom/vm.h"

typedef struct IdmPort IdmPort;

typedef enum {
    IDM_PORT_IO_OK,
    IDM_PORT_IO_AGAIN,
    IDM_PORT_IO_EOF,
    IDM_PORT_IO_CLOSED
} IdmPortIoStatus;

void idm_job_control_init(void);
IdmPort *idm_port_launch(IdmRuntime *rt, IdmValue graph, const IdmExec *exec_ctx, IdmError *err);
IdmPort *idm_port_open_file(const char *path, const char *mode, bool readable, bool writable, IdmError *err);
bool idm_port_waits_completion(const IdmPort *port);
size_t idm_port_live_fds(const IdmPort *port, int *out_fds, size_t max);
int idm_port_input_fd(const IdmPort *port);
int idm_port_output_fd(const IdmPort *port, const char *stream);
bool idm_port_read(IdmPort *port, const char *stream, size_t max, char **out_data, size_t *out_len, IdmPortIoStatus *out_status, IdmError *err);
bool idm_port_write(IdmPort *port, const char *data, size_t len, size_t *out_written, IdmPortIoStatus *out_status, IdmError *err);
IdmPortIoStatus idm_port_close_input(IdmPort *port);
void idm_port_reap(IdmPort *port);
bool idm_port_done(const IdmPort *port);
bool idm_port_io_drained(const IdmPort *port);
bool idm_port_stdout_readable(const IdmPort *port);
bool idm_port_stderr_readable(const IdmPort *port);
bool idm_port_stderr_on_pty(const IdmPort *port);
bool idm_port_stopped(const IdmPort *port);
bool idm_port_resize(IdmPort *port, int cols, int rows, IdmError *err);
bool idm_port_info_value(IdmRuntime *rt, const IdmPort *port, IdmValue *out, IdmError *err);
void idm_port_resume(IdmPort *port, bool fg);
void idm_port_signal_group(IdmPort *port, int signo);
int idm_port_exit_code(IdmPort *port);
void idm_port_release_process_state(IdmPort *port);
void idm_port_free(IdmPort *port);

#endif
