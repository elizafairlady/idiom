#ifndef ISH_READER_H
#define ISH_READER_H

#include "ish/syntax.h"

typedef struct {
    const char *file;
    const char *source;
    size_t length;
} IshReaderInput;

bool ish_reader_read_string(const char *file, const char *source, IshSyntax **out, IshError *err);
bool ish_reader_read_file(const char *path, IshSyntax **out, IshError *err);

#endif
