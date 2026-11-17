#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

struct ZdlParameter {
    std::string name;
    int32_t maxVal;
    int32_t minVal;
    int32_t defaultVal;

    ZdlParameter() : name(""), maxVal(100), minVal(0), defaultVal(50) {}
    ZdlParameter(const std::string& n, int32_t mx, int32_t mn, int32_t def)
        : name(n), maxVal(mx), minVal(mn), defaultVal(def) {}
};

struct ZdlEffectMetadata {
    bool valid = false;
    uint32_t effectId = 0;
    std::string version;
    std::string effectName;
    std::vector<ZdlParameter> parameters;
    std::vector<uint8_t> iconBitmap;
};

class ZdlParser {
public:
    static ZdlEffectMetadata parseFile(const std::string& filePath) {
        ZdlEffectMetadata meta;
        if (filePath.empty()) return meta;
        FILE* fp = openUtf8File(filePath);
        if (!fp) return meta;
        std::fseek(fp, 0, SEEK_END);
        long sz = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (sz < 128) { std::fclose(fp); return meta; }
        std::vector<uint8_t> buf(static_cast<size_t>(sz));
        size_t readBytes = std::fread(buf.data(), 1, buf.size(), fp);
        std::fclose(fp);
        if (readBytes < 128) return meta;
        return parseBuffer(buf, filePath);
    }

    static ZdlEffectMetadata parseBuffer(const std::vector<uint8_t>& buf, const std::string& fallbackPath = "") {
        ZdlEffectMetadata meta;
        if (buf.size() < 128) return meta;

        meta.version = readCleanAscii(buf, 0x44, 4);
        if (meta.version.empty()) meta.version = findVersionString(buf);
        if (meta.version.empty()) meta.version = "1.00";

        meta.effectId = readU32LE(buf, 0x70);
        meta.effectName = readCleanAscii(buf, 0x74, 12);

        if (meta.effectName.size() < 2) {
            size_t onOffPos = findTag(buf, "OnOff");
            if (onOffPos != std::string::npos && onOffPos + 40 < buf.size()) {
                meta.effectName = readCleanAscii(buf, onOffPos + 40, 12);
            }
        }

        size_t iconPos = findTag(buf, "ICON");
        size_t txj1Pos = findTag(buf, "TXJ1");
        if (iconPos != std::string::npos && iconPos + 700 <= buf.size()) {
            meta.iconBitmap.assign(buf.begin() + iconPos + 8, buf.begin() + iconPos + 680);
        } else if (txj1Pos != std::string::npos && txj1Pos + 680 <= buf.size()) {
            meta.iconBitmap.assign(buf.begin() + txj1Pos + 8, buf.begin() + txj1Pos + 680);
        }

        if (meta.effectName.size() < 2 && !fallbackPath.empty()) {
            size_t slash = fallbackPath.find_last_of("/\\");
            std::string fname = (slash == std::string::npos) ? fallbackPath : fallbackPath.substr(slash + 1);
            size_t dot = fname.find_last_of('.');
            if (dot != std::string::npos) fname = fname.substr(0, dot);
            meta.effectName = fname.substr(0, 12);
        }
        if (meta.effectName.empty()) meta.effectName = "ZD FX";

        parsePrmeJsonParameters(buf, meta.parameters);
        if (!meta.parameters.empty()) {
            enrichBoundsFromBinaryTable(buf, meta.parameters);
        } else {
            parseParametersFromBuffer(buf, meta.effectName, meta.parameters);
        }

        if (meta.parameters.size() < 2) {
            meta.parameters.clear();
            meta.parameters.push_back(ZdlParameter("Gain", 100, 0, 60));
            meta.parameters.push_back(ZdlParameter("Tone", 100, 0, 50));
            meta.parameters.push_back(ZdlParameter("Level", 100, 0, 70));
            meta.parameters.push_back(ZdlParameter("Mix", 100, 0, 50));
        }

        meta.valid = true;
        return meta;
    }
private:
    static FILE* openUtf8File(const std::string& path) {
#ifdef _WIN32
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            std::vector<wchar_t> wpath(static_cast<size_t>(wlen));
            MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
            FILE* f = _wfopen(wpath.data(), L"rb");
            if (f) return f;
        }
#endif
        return std::fopen(path.c_str(), "rb");
    }
    static uint32_t readU32LE(const std::vector<uint8_t>& b, size_t off) {
        if (off + 4 > b.size()) return 0;
        return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
               (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
    }
    static std::string readCleanAscii(const std::vector<uint8_t>& b, size_t off, size_t maxLen) {
        std::string s;
        for (size_t i = 0; i < maxLen && (off + i) < b.size(); ++i) {
            uint8_t c = b[off + i];
            if (c == 0) break;
            if (c >= 32 && c <= 122) s.push_back(static_cast<char>(c));
            else break;
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
    static std::string findVersionString(const std::vector<uint8_t>& b) {
        for (size_t i = 0; i + 4 < std::min<size_t>(b.size(), 256); ++i) {
            if (b[i] >= '1' && b[i] <= '9' && b[i + 1] == '.' && std::isdigit(b[i + 2]) && std::isdigit(b[i + 3]))
                return std::string(reinterpret_cast<const char*>(&b[i]), 4);
        }
        return "";
    }
    static size_t findTag(const std::vector<uint8_t>& b, const char* tag) {
        size_t tagLen = std::strlen(tag);
        for (size_t i = 0; i + tagLen <= b.size(); ++i) {
            if (std::memcmp(&b[i], tag, tagLen) == 0) return i;
        }
        return std::string::npos;
    }
    static void parsePrmeJsonParameters(const std::vector<uint8_t>& b, std::vector<ZdlParameter>& outParams) {
        size_t prmPos = findTag(b, "PRME");
        if (prmPos == std::string::npos) prmPos = findTag(b, "PRMJ");
        if (prmPos == std::string::npos) prmPos = findTag(b, "\"Parameters\"");
        if (prmPos == std::string::npos) return;
        size_t endPos = std::min<size_t>(b.size(), prmPos + 8192);
        std::string jsonStr(reinterpret_cast<const char*>(&b[prmPos]), endPos - prmPos);
        size_t pos = 0;
        while (outParams.size() < 9) {
            size_t nameKey = jsonStr.find("\"name\"", pos);
            if (nameKey == std::string::npos) break;
            size_t colon = jsonStr.find(':', nameKey + 6);
            if (colon == std::string::npos) break;
            size_t q1 = jsonStr.find('"', colon + 1);
            if (q1 == std::string::npos) break;
            size_t q2 = jsonStr.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string pName = jsonStr.substr(q1 + 1, q2 - q1 - 1);
            if (!pName.empty() && pName != "OnOff") outParams.push_back(ZdlParameter(pName, 100, 0, 50));
            pos = q2 + 1;
        }
    }
    static void enrichBoundsFromBinaryTable(const std::vector<uint8_t>& b, std::vector<ZdlParameter>& params) {
        size_t onOffPos = findTag(b, "OnOff");
        size_t searchStart = (onOffPos != std::string::npos) ? onOffPos : 0;
        for (auto& p : params) {
            for (size_t i = searchStart; i + 24 < b.size(); ++i) {
                if (std::memcmp(&b[i], p.name.c_str(), p.name.size()) == 0 && b[i + p.name.size()] == 0) {
                    int32_t maxV = static_cast<int32_t>(readU32LE(b, i + 12));
                    int32_t defV = static_cast<int32_t>(readU32LE(b, i + 16));
                    if (maxV > 0 && maxV <= 3000 && defV >= 0 && defV <= maxV) {
                        p.minVal = 0; p.maxVal = maxV; p.defaultVal = defV; break;
                    }
                }
            }
        }
    }
    static std::string normalizeAlphaNum(const std::string& s) {
        std::string r;
        for (char c : s) {
            if (std::isalnum(static_cast<unsigned char>(c)))
                r.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
        return r;
    }
    static bool isBlockedWord(const std::string& s, const std::string& fxName) {
        if (s.size() < 2 || s.size() > 9) return true;
        std::string normS = normalizeAlphaNum(s), normFx = normalizeAlphaNum(fxName);
        if (normS.size() < 2) return true;
        if (!normFx.empty() && (normS == normFx || normFx.find(normS) != std::string::npos || normS.find(normFx) != std::string::npos)) return true;
        static const char* kSkip[] = { "ONOFF", "TXJ1", "TXE1", "DATA", "CODE", "INFO", "ZDLF", "PTBL", "STBL", "BMAP", "PRMS", "PRMJ", "PRME", "ICON", "VER", "ZOOM", "OFF", "PEDAL", "CAB", "ELF" };
        for (const char* w : kSkip) { if (normS == w) return true; }
        int letters = 0;
        for (char c : s) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) letters++;
            else if ((c >= '0' && c <= '9') || c == '-' || c == '.' || c == ' ' || c == '/') {}
            else return true;
        }
        return letters < 2;
    }
    static void parseParametersFromBuffer(const std::vector<uint8_t>& b, const std::string& fxName, std::vector<ZdlParameter>& outParams) {
        size_t onOffPos = findTag(b, "OnOff");
        size_t scanStart = (onOffPos != std::string::npos) ? (onOffPos + 5) : 0x80;
        for (size_t i = scanStart; i + 8 < b.size() && outParams.size() < 9; ++i) {
            if ((b[i] >= 'A' && b[i] <= 'Z') && (i == 0 || b[i - 1] == 0)) {
                std::string cand = readCleanAscii(b, i, 8);
                if (!isBlockedWord(cand, fxName) && (i + cand.size() < b.size()) && b[i + cand.size()] == 0) {
                    bool dup = false;
                    std::string normCand = normalizeAlphaNum(cand);
                    for (const auto& p : outParams) { if (normalizeAlphaNum(p.name) == normCand) { dup = true; break; } }
                    if (!dup) {
                        int32_t maxV = 100, defV = 55;
                        if (i + 20 <= b.size()) {
                            int32_t m = static_cast<int32_t>(readU32LE(b, i + 12));
                            int32_t d = static_cast<int32_t>(readU32LE(b, i + 16));
                            if (m > 0 && m <= 3000 && d >= 0 && d <= m) { maxV = m; defV = d; }
                        }
                        outParams.push_back(ZdlParameter(cand, maxV, 0, defV));
                        i += cand.size();
                    }
                }
            }
        }
    }
};
