#pragma once
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
namespace glyph {
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Line {
    int number;
    std::string text;
};
struct Port {
    std::string name, type, refinement;
    int line;
};
struct Record {
    std::string name, storage;
    std::vector<Port> fields;
    std::set<std::string> unique;
    int line;
};
struct Node {
    std::string name;
    std::vector<Port> inputs;
    std::string output = "Unit";
    std::map<std::string, std::vector<Line>> sections;
    int line;
};
struct Flow {
    std::string name;
    std::vector<Port> inputs;
    std::string output = "Unit";
    std::vector<Line> steps;
    std::set<std::string> failures;
    bool atomic = false;
    int line;
};
struct Program {
    std::map<std::string, Record> records;
    std::map<std::string, Node> nodes;
    std::map<std::string, Flow> flows;
    std::string path;
};
struct Expr {
    std::string type, code;
    std::set<std::string> effects;
};
struct Binding {
    std::string type, code;
};
using Env = std::map<std::string, Binding>;
struct Step {
    std::string node, binding;
    std::vector<std::string> args;
    int line;
};
struct ResolvedFlow {
    Flow flow;
    std::vector<Step> steps;
    std::string result;
    std::set<std::string> effects;
};
struct Compiled {
    Program program;
    std::vector<ResolvedFlow> flows;
    std::map<std::string, std::string> implementations;
    std::vector<std::string> unresolved;
};
std::string trim(std::string s);
std::string quote(const std::string &s);
[[noreturn]] void fail(int line, const std::string &message);
Program parse(const std::string &source, const std::string &path);
Expr expression(const std::string &source, const Env &env, const Program &p, int line);
Compiled compile(Program p);
std::string graph(const Compiled &c);
std::string graph_html(const Compiled &c);
std::string emit(const Compiled &c, const std::string &flow);
std::string synthesize(const Compiled &c, const std::string &model_path,
                       const std::string &trace_path);
std::string synthesis_units(const Compiled &c);
std::string read_file(const std::string &path);
void write_file(const std::string &path, const std::string &content);
} // namespace glyph
