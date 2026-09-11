#ifndef GLYPH_RUNTIME_H
#define GLYPH_RUNTIME_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Stable ownership boundary. All returned strings must be freed with glyph_string_free.
   Pass a null error pointer when diagnostics are not needed. No C++ exceptions cross ABI. */
typedef struct glyph_module glyph_module;
glyph_module *glyph_module_load(const char *bytecode, char **error);
int glyph_module_run(glyph_module *module, size_t argc, const char *const *argv,
                     const char *database, char **result_json, char **error);
void glyph_module_free(glyph_module *module);
void glyph_string_free(char *text);
const char *glyph_runtime_version(void);
#ifdef __cplusplus
}
#endif
#endif
