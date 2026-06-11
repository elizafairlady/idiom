#ifndef IDM_READER_H
#define IDM_READER_H

#include "idiom/syntax.h"

typedef struct {
    const char *file;
    const char *source;
    size_t length;
} IdmReaderInput;

bool idm_reader_read_string(const char *file, const char *source, IdmSyntax **out, IdmError *err);
bool idm_reader_read_file(const char *path, IdmSyntax **out, IdmError *err);

#endif
