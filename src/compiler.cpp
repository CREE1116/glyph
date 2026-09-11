#include "glyph/compiler.hpp"
#include <functional>
#include <iomanip>
#include <regex>
#include <sstream>
namespace glyph {
static std::set<std::string> values(const Node &n, const std::string &key) {
    std::set<std::string> s;
    auto i = n.sections.find(key);
    if (i != n.sections.end())
        for (auto &l : i->second)
            s.insert(l.text);
    return s;
}
static std::string load(const std::string &s) {
    return "LOAD " + quote(s) + "\n";
}
static std::vector<std::string> split_args(const std::string &s) {
    std::vector<std::string> r;
    int depth = 0;
    bool str = false, esc = false;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (str) {
            if (esc)
                esc = false;
            else if (c == '\\')
                esc = true;
            else if (c == '"')
                str = false;
        } else if (c == '"')
            str = true;
        else if (c == '(')
            ++depth;
        else if (c == ')')
            --depth;
        else if (c == ',' && depth == 0) {
            r.push_back(trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (!trim(s.substr(start)).empty())
        r.push_back(trim(s.substr(start)));
    return r;
}
Compiled compile(Program p) {
    Compiled c;
    c.program = std::move(p);
    auto &pg = c.program;
    std::set<std::string> types = {"Int", "Float", "Bool", "Text", "Unit", "Tensor", "Weight"};
    for (auto &[name, r] : pg.records) {
        if (types.count(name))
            fail(r.line, "reserved type " + name);
        types.insert(name);
    }
    auto typecheck = [&](const std::string &t, int l) {
        if (!types.count(t))
            fail(l, "unknown type " + t);
    };
    std::map<std::string, int> visited;
    std::function<void(const std::string &)> visit = [&](const std::string &n) {
        if (visited[n] == 1)
            fail(pg.records.at(n).line, "recursive value type " + n);
        if (visited[n] == 2)
            return;
        visited[n] = 1;
        for (auto &f : pg.records.at(n).fields) {
            typecheck(f.type, f.line);
            if (pg.records.count(f.type))
                visit(f.type);
        }
        visited[n] = 2;
    };
    for (auto &[n, r] : pg.records) {
        visit(n);
        if (!r.storage.empty()) {
            if (r.fields.empty())
                fail(r.line, "state requires fields");
            for (auto &f : r.fields)
                if (f.type != "Int" && f.type != "Text" && f.type != "Float" && f.type != "Bool")
                    fail(f.line, "SQLite state fields must be primitive");
        }
    }
    const std::set<std::string> supported_effects = {"none", "IO", "DB.Write", "File.Read",
                                                    "Model.Cache"};
    for (auto &[name, n] : pg.nodes) {
        typecheck(n.output, n.line);
        const std::string node_name = name;
        Env env;
        std::string code;
        for (auto &f : n.inputs) {
            typecheck(f.type, f.line);
            if (f.name == "output")
                fail(f.line, "output is reserved for the result");
            env[f.name] = {f.type, load(f.name)};
        }
        auto effects = values(n, "effect");
        for (auto &e : effects)
            if (!supported_effects.count(e))
                fail(n.line, "unsupported effect " + e);
        if (effects.count("none") && effects.size() > 1)
            fail(n.line, "none cannot be combined with effects");
        for (auto &key : {"read", "where", "allow"})
            if (n.sections.count(key))
                fail(n.line, std::string(key) + " is not implemented in this runtime");
        auto assertion = [&](const std::string &source, int line, const std::string &label) {
            auto x = expression(source, env, pg, line);
            if (x.type != "Bool" || !x.effects.empty())
                fail(line, "contract must be a pure Bool expression");
            code += x.code + "ASSERT " + quote(node_name + ": " + label + " " + source) + "\n";
        };
        for (auto &f : n.inputs)
            if (!f.refinement.empty())
                assertion(f.name + " " + f.refinement, f.line, "input refinement");
        for (auto &l : n.sections["require"])
            assertion(l.text, l.number, "require");
        auto writes = values(n, "write");
        if (writes.size() > 1)
            fail(n.line, "a node may write one state");
        std::string state = writes.empty() ? "" : *writes.begin();
        if (!state.empty()) {
            if (!pg.records.count(state) || pg.records.at(state).storage.empty())
                fail(n.line, "unknown state " + state);
            if (n.inputs.size() != 1 || n.inputs[0].type != state || n.output != state)
                fail(n.line, "write node must have State -> State signature");
            if (!effects.count("DB.Write"))
                fail(n.line, "write requires effect DB.Write");
            if (!values(n, "fail").count("DBError"))
                fail(n.line, "write requires fail DBError");
        }
        auto &impl = n.sections["impl"];
        if (impl.empty() && state.empty()) {
            c.unresolved.push_back(name);
            env["output"] = {n.output, load("output")};
            for (auto &l : n.sections["ensure"])
                assertion(l.text, l.number, "ensure");
            continue;
        }
        if (impl.size() > 1)
            fail(n.line, "impl must contain one typed expression");
        Expr x = impl.empty() ? Expr{state, load(n.inputs[0].name), {}}
                              : expression(impl[0].text, env, pg, impl[0].number);
        if (x.type != n.output)
            fail(n.line, "node " + name + " returns " + x.type + ", expected " + n.output);
        for (auto &e : x.effects)
            if (!effects.count(e))
                fail(n.line, "undeclared effect " + e + " in " + name);
        code += x.code + "STORE \"output\"\n";
        env["output"] = {n.output, load("output")};
        for (auto &l : n.sections["ensure"])
            assertion(l.text, l.number, "ensure");
        code += load("output");
        if (!state.empty())
            code += "WRITE " + quote(state) + "\n";
        code += "RET \"\"\n";
        c.implementations[name] = code;
    }
    for (auto &[name, f] : pg.flows) {
        typecheck(f.output, f.line);
        ResolvedFlow rf;
        rf.flow = f;
        Env env;
        for (auto &i : f.inputs) {
            typecheck(i.type, i.line);
            if (!i.refinement.empty())
                fail(i.line, "flow refinements belong on entry node");
            env[i.name] = {i.type, load(i.name)};
        }
        int index = 0;
        std::string last;
        for (auto &l : f.steps) {
            std::smatch m;
            std::string nn, binding;
            std::vector<std::string> args;
            bool explicit_args = false;
            if (std::regex_match(
                    l.text, m,
                    std::regex(
                        "([A-Za-z][A-Za-z0-9_]*)\\((.*)\\)(?: *-> *([A-Za-z][A-Za-z0-9_]*))?"))) {
                nn = m[1];
                args = split_args(m[2]);
                binding = m[3];
                explicit_args = true;
            } else if (std::regex_match(
                           l.text, m,
                           std::regex(
                               "([A-Za-z][A-Za-z0-9_]*)(?: *-> *([A-Za-z][A-Za-z0-9_]*))?"))) {
                nn = m[1];
                binding = m[2];
            } else
                fail(l.number, "expected Node, Node -> binding, or Node(arguments) -> binding");
            if (!pg.nodes.count(nn))
                fail(l.number, "unknown node " + nn);
            auto &n = pg.nodes.at(nn);
            if (binding.empty())
                binding = "_step" + std::to_string(index);
            if (env.count(binding))
                fail(l.number, "immutable binding reassignment: " + binding);
            Step step{nn, binding, {}, l.number};
            if (explicit_args) {
                if (args.size() != n.inputs.size())
                    fail(l.number, "wrong input count for " + nn);
                for (size_t i = 0; i < args.size(); ++i) {
                    auto x = expression(args[i], env, pg, l.number);
                    if (x.type != n.inputs[i].type)
                        fail(l.number, "input type mismatch for " + nn);
                    if (!x.effects.empty())
                        fail(l.number, "flow arguments must be pure");
                    step.args.push_back(x.code);
                }
            } else
                for (auto &port : n.inputs) {
                    std::vector<Binding> candidates;
                    for (auto &[k, v] : env)
                        if (v.type == port.type)
                            candidates.push_back(v);
                    if (candidates.size() != 1)
                        fail(l.number, (candidates.empty() ? "missing" : "ambiguous") +
                                           std::string(" input ") + port.type + " for " + nn +
                                           "; use explicit arguments");
                    step.args.push_back(candidates[0].code);
                }
            for (auto &failure : values(n, "fail"))
                if (!f.failures.count(failure))
                    fail(l.number, "unhandled failure " + failure + "; declare it in flow fail:");
            for (auto &e : values(n, "effect"))
                if (e != "none")
                    rf.effects.insert(e);
            env[binding] = {n.output, load(binding)};
            last = binding;
            rf.steps.push_back(step);
            ++index;
        }
        if (f.atomic && rf.effects.count("IO"))
            fail(f.line, "atomic flow cannot contain uncompensated IO");
        if (last.empty()) {
            if (f.output != "Unit")
                fail(f.line, "empty flow must return Unit");
            rf.result = "UNIT \"\"\n";
        } else {
            if (env.at(last).type != f.output)
                fail(f.line,
                     "flow output mismatch: expected " + f.output + ", got " + env.at(last).type);
            rf.result = load(last);
        }
        c.flows.push_back(rf);
    }
    return c;
}
std::string emit(const Compiled &c, const std::string &entry) {
    if (!c.unresolved.empty())
        throw Error("unresolved node " + c.unresolved.front() +
                    "; write its impl, or pass --model MODEL_DIR to synthesize it");
    std::string out = "GLYPH-BC 1\n";
    auto line = [&](const std::string &op, const std::string &arg) {
        out += op + " " + quote(arg) + "\n";
    };
    for (auto &[n, r] : c.program.records) {
        line("TYPE", n);
        line("STORAGE", r.storage);
        for (auto &f : r.fields) {
            line("FIELDNAME", f.name);
            line("FIELDTYPE", f.type);
            if (r.unique.count(f.name))
                line("UNIQUE", f.name);
        }
        line("ENDTYPE", "");
    }
    for (auto &[n, node] : c.program.nodes) {
        line("FUNC", n);
        for (auto &p : node.inputs)
            line("PARAM", p.name);
        out += c.implementations.at(n);
        line("ENDFUNC", "");
    }
    bool found = false;
    for (auto &f : c.flows) {
        if (f.flow.name != entry)
            continue;
        found = true;
        line("ENTRY", entry);
        for (auto &i : f.flow.inputs) {
            line("ARGNAME", i.name);
            line("ARGTYPE", i.type);
        }
        line("ATOMIC", f.flow.atomic ? "1" : "0");
        line("FUNC", "@entry");
        for (auto &i : f.flow.inputs)
            line("PARAM", i.name);
        // Count uses in resolved data edges. Values release their shared storage
        // immediately after the last use, rather than at the end of a model flow.
        auto loads = [](const std::string &code) {
            std::vector<std::string> names;
            std::istringstream stream(code);
            std::string instruction;
            while (std::getline(stream, instruction)) {
                std::istringstream row(instruction);
                std::string op, name;
                if (row >> op >> std::quoted(name); op == "LOAD")
                    names.push_back(name);
            }
            return names;
        };
        std::map<std::string, size_t> uses;
        for (const auto &step : f.steps)
            for (const auto &arg : step.args)
                for (const auto &name : loads(arg))
                    ++uses[name];
        for (const auto &name : loads(f.result))
            ++uses[name];
        for (const auto &input : f.flow.inputs)
            if (!uses[input.name])
                line("DROP", input.name);
        for (auto &step : f.steps) {
            for (auto &arg : step.args)
                out += arg;
            line("CALL", step.node);
            line("STORE", step.binding);
            for (const auto &arg : step.args)
                for (const auto &name : loads(arg))
                    if (--uses[name] == 0)
                        line("DROP", name);
            if (!uses[step.binding])
                line("DROP", step.binding);
        }
        out += f.result;
        line("RET", "");
        line("ENDFUNC", "");
    }
    if (!found)
        throw Error("unknown entry flow " + entry);
    return out;
}
std::string synthesis_units(const Compiled &c) {
    std::string s;
    for (auto &name : c.unresolved) {
        auto &n = c.program.nodes.at(name);
        s += "TARGET " + name + "\nINPUT\n";
        std::set<std::string> needed;
        std::function<void(std::string)> add = [&](std::string t) {
            if (!c.program.records.count(t) || !needed.insert(t).second)
                return;
            for (auto &f : c.program.records.at(t).fields)
                add(f.type);
        };
        for (auto &p : n.inputs) {
            s += p.name + ": " + p.type + " " + p.refinement + "\n";
            add(p.type);
        }
        s += "OUTPUT " + n.output + "\n";
        add(n.output);
        for (auto &t : needed) {
            s += "TYPE " + t + "(";
            for (auto &f : c.program.records.at(t).fields)
                s += f.name + ": " + f.type + ", ";
            s += ")\n";
        }
        for (auto key : {"require", "ensure", "effect", "fail", "intent"}) {
            auto it = n.sections.find(key);
            if (it != n.sections.end()) {
                s += std::string(key) + ":\n";
                for (auto &l : it->second)
                    s += l.text + "\n";
            }
        }
        s += "AVAILABLE: typed arithmetic, comparisons, Text.length(Text)->Int, "
             "Text.concat(Text,Text)->Text, Text.fromInt(Int)->Text, select(Bool,T,T)->T, record "
             "constructors\nEND TARGET\n";
    }
    return s;
}
} // namespace glyph
