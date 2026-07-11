#ifndef IDM_READER_H
#define IDM_READER_H

#include "idiom/syntax.h"

typedef struct IdmPkgGrammar IdmPkgGrammar;
typedef struct IdmRuntime IdmRuntime;

typedef struct {
    const char *file;
    const char *source;
    size_t length;
} IdmReaderInput;

typedef struct IdmReaderArtifact IdmReaderArtifact;

typedef struct {
    const IdmPkgGrammar *grammar;
    const char *provider;
    const char *provider_key;
    const IdmScopeSet *binding_scopes;
    int phase;
} IdmReaderGrammarSource;

bool idm_reader_artifact_from_grammars(const char *surface, const IdmPkgGrammar *grammars, size_t grammar_count, IdmReaderArtifact **out, IdmError *err);
bool idm_reader_artifact_from_sources(const char *surface, const IdmReaderGrammarSource *sources, size_t source_count, IdmReaderArtifact **out, IdmError *err);
void idm_reader_artifact_destroy(IdmReaderArtifact *artifact);
bool idm_reader_read_artifact_string(const IdmReaderArtifact *artifact, const char *file, const char *source, size_t offset, IdmSyntax **out, bool *out_incomplete, IdmError *err);
bool idm_reader_read_terms_string(const char *file, const char *source, size_t offset, IdmSyntax **out, bool *out_incomplete, IdmError *err);
bool idm_reader_read_terms_file(const char *path, IdmSyntax **out, IdmError *err);
bool idm_reader_string_escape(char e, char *out);

#endif
