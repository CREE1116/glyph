#include "glyph/compiler.hpp"
#include "glyph/runtime.h"
#include "kernel.hpp"
#include <cstdlib>
#include <cstring>
#include <new>
struct glyph_module {
    glyph::runtime::Module value;
};
namespace {
char *copy_string(const std::string &value) {
    auto *result = static_cast<char *>(std::malloc(value.size() + 1));
    if (!result)
        throw std::bad_alloc();
    std::memcpy(result, value.c_str(), value.size() + 1);
    return result;
}
void diagnostic(char **output, const char *message) noexcept {
    if (!output)
        return;
    try {
        *output = copy_string(message);
    } catch (...) {
        *output = nullptr;
    }
}
} // namespace
extern "C" {
glyph_module *glyph_module_load(const char *bytecode, char **error) {
    if (error)
        *error = nullptr;
    try {
        if (!bytecode)
            throw glyph::Error("bytecode is null");
        return new glyph_module{glyph::runtime::decode(bytecode)};
    } catch (const std::exception &e) {
        diagnostic(error, e.what());
        return nullptr;
    } catch (...) {
        diagnostic(error, "unknown runtime error");
        return nullptr;
    }
}
int glyph_module_run(glyph_module *module, size_t argc, const char *const *argv,
                     const char *database, char **result_json, char **error) {
    if (error)
        *error = nullptr;
    if (result_json)
        *result_json = nullptr;
    try {
        if (!module || !result_json || (argc && !argv))
            throw glyph::Error("null runtime argument");
        if (argc != module->value.inputs.size())
            throw glyph::Error("entry argument count mismatch");
        std::vector<glyph::runtime::Value> values;
        for (size_t i = 0; i < argc; ++i) {
            if (!argv[i])
                throw glyph::Error("null entry argument");
            values.push_back(glyph::runtime::parse_value(argv[i], module->value.inputs[i].second));
        }
        auto value = glyph::runtime::run(module->value, values, database ? database : ":memory:");
        *result_json = copy_string(glyph::runtime::display(value));
        return 0;
    } catch (const std::exception &e) {
        diagnostic(error, e.what());
        return 1;
    } catch (...) {
        diagnostic(error, "unknown runtime error");
        return 1;
    }
}
void glyph_module_free(glyph_module *module) {
    delete module;
}
void glyph_string_free(char *text) {
    std::free(text);
}
const char *glyph_runtime_version(void) {
    return "0.1-dev bytecode-v1";
}
}
