// 아주 작은 JSON 파서/작성기 (layout.json 전용)
#pragma once
#include <string>
#include <vector>
#include <cstdlib>

struct JV {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    double n = 0; bool b = false;
    std::string s;
    std::vector<JV> a;
    std::vector<std::pair<std::string, JV>> o;
    const JV* get(const char* k) const {
        for (auto& p : o) if (p.first == k) return &p.second;
        return nullptr;
    }
    double num(const char* k, double d) const {
        auto v = get(k);
        if (!v) return d;
        if (v->t == Num) return v->n;
        if (v->t == Bool) return v->b ? 1 : 0;
        return d;
    }
    std::string str(const char* k) const {
        auto v = get(k);
        return (v && v->t == Str) ? v->s : std::string();
    }
};

struct JParser {
    const char* p; const char* e; bool ok = true;
    JParser(const char* b, size_t len) : p(b), e(b + len) {}
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    static void put8(std::string& s, unsigned cp) {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 63)); }
        else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 63)); s += (char)(0x80 | (cp & 63)); }
    }
    std::string strv() {
        std::string s; p++;
        while (p < e && *p != '"') {
            if (*p == '\\') {
                p++; if (p >= e) break;
                char c = *p;
                if (c == 'n') s += '\n';
                else if (c == 't') s += '\t';
                else if (c == 'r') s += '\r';
                else if (c == 'u' && e - p > 4) {
                    put8(s, (unsigned)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16)); p += 4;
                } else s += c;
                p++;
            } else s += *p++;
        }
        if (p < e) p++; else ok = false;
        return s;
    }
    JV val() {
        ws(); JV v;
        if (p >= e) { ok = false; return v; }
        char c = *p;
        if (c == '{') {
            p++; v.t = JV::Obj; ws();
            if (p < e && *p == '}') { p++; return v; }
            while (ok) {
                ws(); if (p >= e || *p != '"') { ok = false; break; }
                std::string k = strv(); ws();
                if (p >= e || *p != ':') { ok = false; break; }
                p++;
                JV x = val();
                v.o.emplace_back(k, std::move(x)); ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; break; }
                ok = false;
            }
            return v;
        }
        if (c == '[') {
            p++; v.t = JV::Arr; ws();
            if (p < e && *p == ']') { p++; return v; }
            while (ok) {
                v.a.push_back(val()); ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; break; }
                ok = false;
            }
            return v;
        }
        if (c == '"') { v.t = JV::Str; v.s = strv(); return v; }
        if (c == 't') { if (e - p < 4) ok = false; p += 4; v.t = JV::Bool; v.b = true; return v; }
        if (c == 'f') { if (e - p < 5) ok = false; p += 5; v.t = JV::Bool; return v; }
        if (c == 'n') { if (e - p < 4) ok = false; p += 4; return v; }
        char* end = nullptr;
        v.n = strtod(p, &end);
        if (end == p) ok = false;
        p = end; v.t = JV::Num;
        return v;
    }
};

inline std::string JEsc(const std::string& s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        if (c == '"') r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); r += b; }
        else r += (char)c;
    }
    return r + "\"";
}
