#include "glyph/runtime.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    const char *program = "GLYPH-BC 1\nENTRY \"Main\"\nATOMIC \"0\"\n"
                          "FUNC \"@entry\"\nINT \"42\"\nRET \"\"\nENDFUNC \"\"\n";
    char *error = NULL;
    glyph_module *module = glyph_module_load(program, &error);
    if (!module) {
        fprintf(stderr, "%s\n", error);
        glyph_string_free(error);
        return 1;
    }
    char *result = NULL;
    int status = glyph_module_run(module, 0, NULL, NULL, &result, &error);
    int failed = status || !result || strcmp(result, "42");
    glyph_string_free(result);
    glyph_string_free(error);
    glyph_module_free(module);
    error = NULL;
    module = glyph_module_load("broken", &error);
    failed |= module != NULL || error == NULL;
    glyph_module_free(module);
    glyph_string_free(error);
    if (!failed)
        puts("C ABI passed");
    return failed;
}
