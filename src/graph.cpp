#include "glyph/compiler.hpp"
#include "graph_view.hpp"
#include <iomanip>
#include <sstream>
namespace glyph {
namespace {
std::string ports(const std::vector<Port> &list) {
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i)
            out += ",";
        out += "{\"name\":" + quote(list[i].name) + ",\"type\":" + quote(list[i].type) + "}";
    }
    return out + "]";
}
std::string strings(const std::set<std::string> &list) {
    std::string out = "[";
    bool first = true;
    for (const auto &s : list) {
        if (!first)
            out += ",";
        first = false;
        out += quote(s);
    }
    return out + "]";
}
std::set<std::string> dependencies(const std::string &code) {
    std::set<std::string> names;
    std::istringstream stream(code);
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream row(line);
        std::string op, name;
        if (row >> op >> std::quoted(name); op == "LOAD")
            names.insert(name);
    }
    return names;
}
} // namespace
std::string graph(const Compiled &c) {
    std::string out =
        "{\"format\":\"glyph.graph.v1\",\"source\":" + quote(c.program.path) + ",\"nodes\":{";
    bool first = true;
    for (const auto &[name, n] : c.program.nodes) {
        if (!first)
            out += ",";
        first = false;
        out += quote(name) + ":{\"name\":" + quote(name) + ",\"line\":" + std::to_string(n.line) +
               ",\"inputs\":" + ports(n.inputs) + ",\"output\":" + quote(n.output) +
               ",\"sections\":{";
        bool section_first = true;
        for (const auto &[key, lines] : n.sections) {
            if (lines.empty())
                continue;
            if (!section_first)
                out += ",";
            section_first = false;
            out += quote(key) + ":[";
            for (size_t i = 0; i < lines.size(); ++i) {
                if (i)
                    out += ",";
                out += quote(lines[i].text);
            }
            out += "]";
        }
        out += "}}";
    }
    out += "},\"flows\":[";
    first = true;
    for (const auto &f : c.flows) {
        if (!first)
            out += ",";
        first = false;
        out += "{\"name\":" + quote(f.flow.name) +
               ",\"atomic\":" + (f.flow.atomic ? "true" : "false") +
               ",\"inputs\":" + ports(f.flow.inputs) + ",\"output\":" + quote(f.flow.output) +
               ",\"failures\":" + strings(f.flow.failures) + ",\"steps\":[";
        std::map<std::string, std::pair<std::string, std::string>> producers;
        for (const auto &p : f.flow.inputs)
            producers[p.name] = {"input", p.type};
        std::string edges = "[";
        bool edge_first = true;
        auto edge = [&](const std::string &from, const std::string &to, const std::string &kind,
                        const std::string &label) {
            if (!edge_first)
                edges += ",";
            edge_first = false;
            edges += "{\"from\":" + quote(from) + ",\"to\":" + quote(to) +
                     ",\"kind\":" + quote(kind) + ",\"label\":" + quote(label) + "}";
        };
        std::string previous = "input";
        for (size_t i = 0; i < f.steps.size(); ++i) {
            const auto &st = f.steps[i];
            const auto &node = c.program.nodes.at(st.node);
            std::string id = "step" + std::to_string(i);
            if (i)
                out += ",";
            out += "{\"id\":" + quote(id) + ",\"node\":" + quote(st.node) +
                   ",\"binding\":" + quote(st.binding) + ",\"line\":" + std::to_string(st.line) +
                   ",\"inputs\":[";
            for (size_t j = 0; j < st.args.size(); ++j) {
                if (j)
                    out += ",";
                out += quote(st.args[j]);
                for (const auto &dep : dependencies(st.args[j])) {
                    const auto &producer = producers.at(dep);
                    edge(producer.first, id, "data",
                         dep + " → " + node.inputs[j].name + ": " + producer.second);
                }
            }
            out += "]}";
            edge(previous, id, "control", "");
            previous = id;
            producers[st.binding] = {id, node.output};
        }
        edge(previous, "output", "control", "");
        for (const auto &dep : dependencies(f.result)) {
            const auto &p = producers.at(dep);
            edge(p.first, "output", "data", dep + ": " + p.second);
        }
        out += "],\"edges\":" + edges + "],\"effects\":" + strings(f.effects) + "}";
    }
    std::set<std::string> unresolved(c.unresolved.begin(), c.unresolved.end());
    return out + "],\"unresolved\":" + strings(unresolved) + "}\n";
}
std::string graph_html(const Compiled &c) {
    // Escape raw-text HTML delimiters even inside JSON strings. All UI content uses textContent.
    auto data = graph(c);
    std::string safe;
    for (char ch : data) {
        if (ch == '<')
            safe += "\\u003c";
        else if (ch == '>')
            safe += "\\u003e";
        else if (ch == '&')
            safe += "\\u0026";
        else
            safe += ch;
    }
    return std::string(view_head) + safe + view_tail;
}
} // namespace glyph
