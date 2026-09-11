#include "glyph/compiler.hpp"
#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
namespace glyph {
std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    return a == s.npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string quote(const std::string &s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            r += '\\';
            r += c;
        } else if (c == '\n')
            r += "\\n";
        else if (c == '\r')
            r += "\\r";
        else if (c == '\t')
            r += "\\t";
        else if (c < 32) {
            const char *h = "0123456789abcdef";
            r += "\\u00";
            r += h[c >> 4];
            r += h[c & 15];
        } else
            r += c;
    }
    return r + '"';
}
[[noreturn]] void fail(int line, const std::string &m) {
    throw Error("line " + std::to_string(line) + ": " + m);
}
std::string read_file(const std::string &path) {
    std::ifstream f(path);
    if (!f)
        throw Error("cannot read " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}
void write_file(const std::string &path, const std::string &s) {
    std::ofstream f(path);
    if (!f || !(f << s))
        throw Error("cannot write " + path);
}
static bool name_ok(const std::string &s) {
    return std::regex_match(s, std::regex("[A-Za-z][A-Za-z0-9_]*"));
}
static Port port(Line l, int index) {
    auto colon = l.text.find(':');
    std::string n = colon == l.text.npos
                        ? (index == 0 ? "input" : "input" + std::to_string(index + 1))
                        : trim(l.text.substr(0, colon));
    std::string t = trim(l.text.substr(colon == l.text.npos ? 0 : colon + 1));
    auto sep = t.find_first_of(" <>=!");
    std::string type = t.substr(0, sep), ref = sep == t.npos ? "" : trim(t.substr(sep));
    if (!name_ok(n) || !name_ok(type))
        fail(l.number, "invalid port declaration: " + l.text);
    return {n, type, ref, l.number};
}
Program parse(const std::string &source, const std::string &path) {
    Program p;
    p.path = path;
    std::istringstream in(source);
    std::string raw, section;
    int ln = 0;
    Node *node = nullptr;
    Flow *flow = nullptr;
    Record *record = nullptr;
    const std::set<std::string> allowed = {"in",     "out",   "ensure", "require",
                                           "effect", "fail",  "intent", "impl",
                                           "read",   "write", "where",  "allow"};
    std::set<std::string> symbols, seen_sections;
    bool has_output = false;
    while (std::getline(in, raw)) {
        ++ln;
        if (raw.find('\t') != raw.npos)
            fail(ln, "use spaces, not tabs");
        auto t = trim(raw);
        if (t.empty() || t.rfind("#", 0) == 0)
            continue;
        bool top = raw[0] != ' ';
        Line l{ln, t};
        if (record) {
            if (t == "}") {
                record = nullptr;
                continue;
            }
            if (top)
                fail(ln, "expected record field or }");
            bool unique = false;
            if (t.size() > 7 && t.substr(t.size() - 7) == " unique") {
                unique = true;
                l.text = trim(t.substr(0, t.size() - 7));
            }
            auto f = port(l, record->fields.size());
            if (l.text.find(':') == l.text.npos || !f.refinement.empty())
                fail(ln, "record fields require name: Type");
            for (auto &x : record->fields)
                if (x.name == f.name)
                    fail(ln, "duplicate field " + f.name);
            record->fields.push_back(f);
            if (unique)
                record->unique.insert(f.name);
            continue;
        }
        if (top && (t.rfind("node ", 0) == 0 || t.rfind("flow ", 0) == 0 ||
                    t.rfind("type ", 0) == 0 || t.rfind("state ", 0) == 0)) {
            node = nullptr;
            flow = nullptr;
            section = "";
            seen_sections.clear();
            has_output = false;
            std::istringstream h(t);
            std::string kind, name, extra;
            h >> kind >> name;
            if (!name_ok(name) || !symbols.insert(name).second)
                fail(ln, "invalid or duplicate declaration " + name);
            if (kind == "node") {
                if (h >> extra)
                    fail(ln, "unexpected node header suffix");
                Node n;
                n.name = name;
                n.line = ln;
                node = &p.nodes.emplace(name, n).first->second;
            } else if (kind == "flow") {
                Flow f;
                f.name = name;
                f.line = ln;
                if (h >> extra) {
                    if (extra != "atomic")
                        fail(ln, "expected atomic");
                    f.atomic = true;
                    if (h >> extra)
                        fail(ln, "unexpected flow suffix");
                }
                flow = &p.flows.emplace(name, f).first->second;
            } else {
                Record r;
                r.name = name;
                r.line = ln;
                h >> extra;
                if (kind == "state") {
                    if (extra != "persistent" && extra != "temporary")
                        fail(ln, "supported state kinds: persistent, temporary");
                    r.storage = extra;
                    h >> extra;
                }
                if (extra != "{" || (h >> extra))
                    fail(ln, "expected { at end of record header");
                record = &p.records.emplace(name, r).first->second;
            }
            continue;
        }
        if (top && t.back() == ':') {
            section = t.substr(0, t.size() - 1);
            if (!allowed.count(section) || (!node && !flow))
                fail(ln, "unsupported section " + section);
            if (flow && section != "in" && section != "out" && section != "fail")
                fail(ln, "unsupported flow section " + section);
            if (!seen_sections.insert(section).second)
                fail(ln, "duplicate section " + section);
            if (node)
                node->sections[section] = {};
            continue;
        }
        if (top) {
            if (!flow)
                fail(ln, "expected declaration or section");
            flow->steps.push_back(l);
            section = "";
            continue;
        }
        if (!node && !flow)
            fail(ln, "unexpected indentation");
        if (section.empty())
            fail(ln, "expected section before indented content");
        if (section == "in") {
            auto &ports = node ? node->inputs : flow->inputs;
            auto f = port(l, ports.size());
            for (auto &x : ports)
                if (x.name == f.name)
                    fail(ln, "duplicate input " + f.name);
            ports.push_back(f);
        } else if (section == "out") {
            auto f = port(l, 0);
            if (!f.refinement.empty())
                fail(ln, "use ensure for output refinements");
            auto &out = node ? node->output : flow->output;
            if (has_output)
                fail(ln, "only one output is supported");
            has_output = true;
            out = f.type;
        } else if (node)
            node->sections[section].push_back(l);
        else
            flow->failures.insert(t);
    }
    if (record)
        fail(ln, "unclosed record");
    if (p.flows.empty())
        fail(1, "program needs a flow");
    return p;
}
} // namespace glyph
