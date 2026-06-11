#ifndef ISH_EXPAND_H
#define ISH_EXPAND_H

#include "ish/core.h"
#include "ish/reader.h"

typedef struct IshMacroRunner IshMacroRunner;

typedef bool (*IshMacroInvokeFn)(void *user, IshRuntime *rt, uint32_t payload, const IshSyntax *use_syntax, IshSyntax **out_syntax, IshError *err);

struct IshMacroRunner {
    void *user;
    IshMacroInvokeFn invoke;
};

bool ish_expand_syntax(IshRuntime *rt, const IshSyntax *syntax, IshCore **out, IshError *err);
bool ish_expand_syntax_with_runner(IshRuntime *rt, const IshSyntax *syntax, IshMacroRunner *runner, IshCore **out, IshError *err);
bool ish_expand_string(IshRuntime *rt, const char *file, const char *source, IshCore **out, IshError *err);
bool ish_expand_string_with_runner(IshRuntime *rt, const char *file, const char *source, IshMacroRunner *runner, IshCore **out, IshError *err);

#endif
