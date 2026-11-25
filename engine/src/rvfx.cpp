#include "rvfx.hpp"

#include <cerrno>
#include <cstdlib>

namespace gh {

namespace {

std::string trimView(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return std::string(s);
}

bool parseDouble(std::string_view s, double& out) {
    if (s.empty()) return false;
    std::string tmp(s);
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() + tmp.size() || errno == ERANGE) return false;
    out = v;
    return true;
}

}  // namespace

bool parseRvfx(const std::string& text, const std::string& sourceName,
               std::vector<VisualClip>& out, std::string& err) {
    size_t pos = 0;
    int lineNo = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = trimView(std::string_view(text).substr(pos, eol - pos));
        pos = eol + 1;
        ++lineNo;
        // Inline comments: '#' starts a comment anywhere on the line.
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line = trimView(std::string_view(line).substr(0, hash));
        if (line.empty()) {
            if (eol == text.size()) break;
            continue;
        }

        // Tokenize on whitespace.
        std::vector<std::string> tok;
        size_t p = 0;
        while (p < line.size()) {
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
            size_t s = p;
            while (p < line.size() && line[p] != ' ' && line[p] != '\t') ++p;
            if (p > s) tok.push_back(line.substr(s, p - s));
        }
        if (tok.empty()) continue;

        auto fail = [&](const std::string& msg) {
            err = sourceName + ":" + std::to_string(lineNo) + ": " + msg;
            return false;
        };

        VisualClip c;
        size_t i = 0;
        if (!parseDouble(tok[i], c.startMs) || c.startMs < 0)
            return fail("expected start time in ms, got '" + tok[i] + "'");
        ++i;

        // Optional end time: a number, or "-" (explicit until-end).
        c.endMs = 0;
        if (i < tok.size() && (tok[i] == "-" || tok[i][0] == '-' ||
                               (tok[i][0] >= '0' && tok[i][0] <= '9') ||
                               tok[i][0] == '.')) {
            if (tok[i] != "-") {
                if (!parseDouble(tok[i], c.endMs))
                    return fail("bad end time '" + tok[i] + "'");
            }
            ++i;
        }

        // Optional @target.
        if (i < tok.size() && !tok[i].empty() && tok[i][0] == '@') {
            const std::string t = tok[i].substr(1);
            if (t != "game" && t != "hud" && t != "both")
                return fail("unknown target '@" + t + "' (game|hud|both)");
            c.target = t;
            ++i;
        }

        if (i >= tok.size()) return fail("missing fx name");
        c.fx = tok[i++];

        for (; i < tok.size(); ++i) {
            const size_t eq = tok[i].find('=');
            if (eq == std::string::npos || eq == 0)
                return fail("bad param '" + tok[i] + "' (expected k=v)");
            double v = 0;
            if (!parseDouble(std::string_view(tok[i]).substr(eq + 1), v))
                return fail("bad param value in '" + tok[i] + "'");
            c.params[tok[i].substr(0, eq)] = v;
        }

        out.push_back(std::move(c));
        if (eol == text.size()) break;
    }
    return true;
}

}  // namespace gh
