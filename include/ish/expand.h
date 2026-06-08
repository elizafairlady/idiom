#ifndef ISH_EXPAND_H
#define ISH_EXPAND_H

#include "ish/core.h"
#include "ish/reader.h"

bool ish_expand_syntax(IshRuntime *rt, const IshSyntax *syntax, IshCore **out, IshError *err);
bool ish_expand_string(IshRuntime *rt, const char *file, const char *source, IshCore **out, IshError *err);

#endif
