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

typedef struct {
    const char *surface;
    int phase;
    uint32_t format_version;
    uint32_t compiler_version;
    uint8_t mode;
    size_t contributor_count;
    size_t token_count;
    size_t form_count;
    size_t skip_count;
} IdmReaderArtifactInfo;

typedef struct {
    const char *provider;
    const char *provider_key;
    const IdmScopeSet *binding_scopes;
    uint8_t mode;
    size_t first_rule_order;
    size_t rule_count;
} IdmReaderArtifactContributorInfo;

bool idm_reader_artifact_from_grammars(const char *surface, const IdmPkgGrammar *grammars, size_t grammar_count, IdmReaderArtifact **out, IdmError *err);
bool idm_reader_artifact_from_sources(const char *surface, const IdmReaderGrammarSource *sources, size_t source_count, IdmReaderArtifact **out, IdmError *err);
void idm_reader_artifact_destroy(IdmReaderArtifact *artifact);
bool idm_reader_artifact_info(const IdmReaderArtifact *artifact, IdmReaderArtifactInfo *out);
bool idm_reader_artifact_contributor_info(const IdmReaderArtifact *artifact, size_t index, IdmReaderArtifactContributorInfo *out);
bool idm_reader_artifact_serialize(const IdmReaderArtifact *artifact, IdmBuffer *out, IdmError *err);
bool idm_reader_artifact_deserialize(IdmRuntime *rt, const unsigned char *data, size_t len, IdmReaderArtifact **out, IdmError *err);
bool idm_reader_read_artifact_string(const IdmReaderArtifact *artifact, const char *file, const char *source, IdmSyntax **out, IdmError *err);
bool idm_reader_read_terms_string(const char *file, const char *source, IdmSyntax **out, IdmError *err);
bool idm_reader_read_terms_file(const char *path, IdmSyntax **out, IdmError *err);
bool idm_reader_string_escape(char e, char *out);

#endif
