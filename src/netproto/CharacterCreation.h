#ifndef KENSHICOOP_CHARACTER_CREATION_H
#define KENSHICOOP_CHARACTER_CREATION_H

#include "Wire.h"
#include <string>
#include <vector>
#include <set>
#include <cstring>
#include <float.h>

namespace coop {

const unsigned int CHARACTER_DRAFT_MAX = 65536;
enum AppearanceValueKind { AV_BOOL = 1, AV_INT, AV_FLOAT, AV_STRING, AV_FILE, AV_VECTOR, AV_QUATERNION, AV_REFS };
struct AppearanceValue {
    u8 kind;
    std::string key;
    std::vector<u8> bytes;
};
struct CharacterDraft {
    std::string startSid, templateSid, name;
    std::vector<AppearanceValue> values;
};

inline void draftAppend(std::vector<u8>& out, const void* p, unsigned int n) {
    const u8* first = static_cast<const u8*>(p);
    if (n) out.insert(out.end(), first, first + n);
}
inline void draftString(std::vector<u8>& out, const std::string& s) {
    u16 len = (u16)s.size(); draftAppend(out, &len, 2); draftAppend(out, s.data(), len);
}
inline bool draftRead(const u8*& p, const u8* end, void* out, unsigned int n) {
    if ((size_t)(end - p) < n) return false;
    if (n) memcpy(out, p, n);
    p += n; return true;
}
inline bool draftReadString(const u8*& p, const u8* end, std::string& s, unsigned int max) {
    u16 n = 0;
    if (!draftRead(p, end, &n, 2) || n > max || (size_t)(end - p) < n) return false;
    s.assign(reinterpret_cast<const char*>(p), n); p += n;
    return s.find('\0') == std::string::npos;
}
inline bool validCharacterDraft(const CharacterDraft& d) {
    if (d.startSid.empty() || d.startSid.size() > 127 || d.templateSid.empty() || d.templateSid.size() > 127 ||
        d.name.empty() || d.name.size() > 63 || d.values.empty() || d.values.size() > 512 ||
        d.startSid.find('\0') != std::string::npos || d.templateSid.find('\0') != std::string::npos ||
        d.name.find('\0') != std::string::npos) return false;
    std::set<std::pair<u8, std::string> > seen;
    unsigned int total = 10 + (unsigned int)(d.startSid.size() + d.templateSid.size() + d.name.size());
    bool race = false;
    for (size_t i = 0; i < d.values.size(); ++i) {
        const AppearanceValue& v = d.values[i];
        if (v.key.empty() || v.key.size() > 96 || v.key.find('\0') != std::string::npos || v.bytes.size() > 4096 ||
            !seen.insert(std::make_pair(v.kind, v.key)).second) return false;
        unsigned int n = (unsigned int)v.bytes.size();
        total += 5 + (unsigned int)v.key.size() + n;
        if (total > CHARACTER_DRAFT_MAX) return false;
        if (v.kind == AV_BOOL) { if (n != 1 || v.bytes[0] > 1) return false; }
        else if (v.kind == AV_INT) { if (n != 4) return false; }
        else if (v.kind == AV_FLOAT || v.kind == AV_VECTOR || v.kind == AV_QUATERNION) {
            unsigned int wanted = v.kind == AV_FLOAT ? 4 : v.kind == AV_VECTOR ? 12 : 16;
            if (n != wanted) return false;
            for (unsigned int j = 0; j < n; j += 4) {
                float f; memcpy(&f, &v.bytes[j], 4);
                if (!_finite(f) || f < -1000000.0f || f > 1000000.0f) return false;
            }
        } else if (v.kind == AV_STRING || v.kind == AV_FILE) {
            for (unsigned int j = 0; j < n; ++j) if (!v.bytes[j]) return false;
        } else if (v.kind == AV_REFS) {
            if (n < 2) return false;
            const u8* p = &v.bytes[0]; const u8* end = p + n;
            u16 count; if (!draftRead(p, end, &count, 2) || count > 64) return false;
            if (v.key == "race") { if (count != 1) return false; race = true; }
            for (u16 j = 0; j < count; ++j) {
                std::string sid; u32 vals[3];
                if (!draftReadString(p, end, sid, 127) || sid.empty() || !draftRead(p, end, vals, 12)) return false;
            }
            if (p != end) return false;
        } else return false;
    }
    return race;
}
inline bool encodeCharacterDraft(const CharacterDraft& d, std::vector<u8>& out) {
    out.clear();
    if (!validCharacterDraft(d)) return false;
    u16 version = 1, count = (u16)d.values.size();
    draftAppend(out, &version, 2); draftAppend(out, &count, 2);
    draftString(out, d.startSid); draftString(out, d.templateSid); draftString(out, d.name);
    for (size_t i = 0; i < d.values.size(); ++i) {
        const AppearanceValue& v = d.values[i]; u16 n = (u16)v.bytes.size();
        draftAppend(out, &v.kind, 1); draftString(out, v.key); draftAppend(out, &n, 2);
        if (n) draftAppend(out, &v.bytes[0], n);
        if (out.size() > CHARACTER_DRAFT_MAX) { out.clear(); return false; }
    }
    return true;
}
inline bool decodeCharacterDraft(const u8* data, unsigned int size, CharacterDraft& out) {
    if (!data || size < 4 || size > CHARACTER_DRAFT_MAX) return false;
    const u8* p = data; const u8* end = p + size; u16 version, count;
    CharacterDraft d;
    if (!draftRead(p, end, &version, 2) || version != 1 || !draftRead(p, end, &count, 2) || count > 512 ||
        !draftReadString(p, end, d.startSid, 127) || !draftReadString(p, end, d.templateSid, 127) ||
        !draftReadString(p, end, d.name, 63)) return false;
    for (u16 i = 0; i < count; ++i) {
        AppearanceValue v; u16 n;
        if (!draftRead(p, end, &v.kind, 1) || !draftReadString(p, end, v.key, 96) ||
            !draftRead(p, end, &n, 2) || n > 4096 || (size_t)(end - p) < n) return false;
        v.bytes.assign(p, p + n); p += n; d.values.push_back(v);
    }
    if (p != end || !validCharacterDraft(d)) return false;
    out = d; return true;
}

} // namespace coop
#endif
