#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class QuadIrCabEngine {
public:
    static constexpr int kMaxIrLen  = 1024;
    static constexpr int kFftSize   = 2048;
    static constexpr int kFiltIrLen = 1536;
    static constexpr int kNumChans  = 4;

    struct Channel {
        bool loaded = false;
        bool specValid = false;
        std::string path;
        uint32_t length = 0;
        std::array<float, kMaxIrLen> data{};
        std::array<float, kFftSize> workRe{};
        std::array<float, kFftSize> workIm{};
        float levelDb = -90.0f;
        float pan     = 0.0f;
        float hpHz    = 20.0f;
        float lpHz    = 20000.0f;
        float specHp  = 0.0f;
        float specLp  = 0.0f;
        int   specLen = 0;
    };

    QuadIrCabEngine() {
        initTwiddles();
        fChans[0].levelDb = 0.0f;
        reset();
    }

    void prepare(double sr) {
        if (sr > 0.0 && sr != fSampleRate) {
            fSampleRate = sr;
            for (int c = 0; c < kNumChans; ++c) {
                if (!fChans[c].path.empty()) loadWav(c, fChans[c].path);
            }
        }
        reset();
    }

    void reset() {
        fOverlapL.fill(0.0f);
        fOverlapR.fill(0.0f);
    }

    void clearChannel(int idx) {
        if (idx < 0 || idx >= kNumChans) return;
        fChans[idx].loaded = false;
        fChans[idx].specValid = false;
        fChans[idx].path.clear();
        fChans[idx].length = 0;
        fChans[idx].data.fill(0.0f);
    }

    bool loadWav(int idx, const std::string& path) {
        if (idx < 0 || idx >= kNumChans) return false;
        if (path.empty()) { clearChannel(idx); return true; }
        uint32_t frames = 0; int fileSr = 0;
        float* mono = readWavMono(path.c_str(), &frames, &fileSr);
        if (!mono) return false;

        float* resampled = mono;
        uint32_t resFrames = frames;
        if (fileSr > 0 && fileSr != static_cast<int>(fSampleRate)) {
            uint64_t target = resampleF32(mono, nullptr, fileSr, static_cast<int>(fSampleRate), frames);
            float* buf = static_cast<float*>(std::malloc(sizeof(float) * (target ? target : 1)));
            uint64_t got = resampleF32(mono, buf, fileSr, static_cast<int>(fSampleRate), frames);
            std::free(mono);
            resampled = buf;
            resFrames = static_cast<uint32_t>(got);
        }

        auto& ch = fChans[idx];
        ch.data.fill(0.0f);
        ch.length = std::min<uint32_t>(resFrames, kMaxIrLen);
        std::memcpy(ch.data.data(), resampled, ch.length * sizeof(float));
        std::free(resampled);
        double sumSq = 0.0;
        for (uint32_t i = 0; i < ch.length; ++i) sumSq += (double)ch.data[i] * (double)ch.data[i];
        float rmsNorm = (sumSq > 1e-12) ? static_cast<float>(0.35 / std::sqrt(sumSq)) : 1.0f;
        for (uint32_t i = 0; i < ch.length; ++i) ch.data[i] *= rmsNorm;
        ch.path = path;
        ch.loaded = true;
        ch.specValid = false;
        return true;
    }
    void setChannelParam(int chIdx, int pType, float norm0to100) {
        if (chIdx < 0 || chIdx >= kNumChans) return;
        float r = std::max(0.0f, std::min(1.0f, norm0to100 / 100.0f));
        auto& ch = fChans[chIdx];
        if (pType == 0) ch.levelDb = (r <= 0.01f) ? -90.0f : (-48.0f + r * 54.0f); // -48..+6 dB
        else if (pType == 1) ch.pan = -100.0f + r * 200.0f;     // -100..+100 %
        else if (pType == 2) ch.hpHz = 20.0f + r * 1980.0f;     // 20..2000 Hz
        else if (pType == 3) ch.lpHz = 200.0f + r * 19800.0f;   // 200..20000 Hz
    }

    void process(float* left, float* right, int nFrames) {
        if (!fEnabled || nFrames <= 0) return;
        bool anyLoaded = false;
        for (int c = 0; c < kNumChans; ++c) if (fChans[c].loaded) anyLoaded = true;
        if (!anyLoaded) return;

        int pos = 0;
        while (pos < nFrames) {
            int blk = std::min(nFrames - pos, 512);
            processSubBlock(left + pos, right + pos, blk);
            pos += blk;
        }
    }

    void setEnabled(bool en) { fEnabled = en; }
    bool isEnabled() const { return fEnabled; }
    const Channel& getChannel(int idx) const { return fChans[idx]; }

private:
    void processSubBlock(float* left, float* right, int nFrames) {
        int filtLen = std::min(kFftSize - nFrames, kFiltIrLen);
        float combLRe[kFftSize]{}, combLIm[kFftSize]{};
        float combRRe[kFftSize]{}, combRIm[kFftSize]{};
        bool anyActive = false;
        for (int c = 0; c < kNumChans; ++c) {
            auto& ch = fChans[c];
            if (!ch.loaded) continue;
            if (!ch.specValid || ch.hpHz != ch.specHp || ch.lpHz != ch.specLp || filtLen != ch.specLen) {
                buildSpectrum(ch, filtLen);
            }
            float coef = (ch.levelDb > -89.5f) ? std::pow(10.0f, ch.levelDb * 0.05f) : 0.0f;
            if (coef <= 0.0f) continue;
            anyActive = true;
            float ang = ((ch.pan + 100.0f) / 200.0f) * 1.57079632679f;
            float gL = coef * std::cos(ang), gR = coef * std::sin(ang);
            for (int i = 0; i < kFftSize; ++i) {
                combLRe[i] += gL * ch.workRe[i]; combLIm[i] += gL * ch.workIm[i];
                combRRe[i] += gR * ch.workRe[i]; combRIm[i] += gR * ch.workIm[i];
            }
        }
        if (!anyActive) return;
        float bufLRe[kFftSize]{}, bufLIm[kFftSize]{};
        float bufRRe[kFftSize]{}, bufRIm[kFftSize]{};
        for (int i = 0; i < nFrames; ++i) { bufLRe[i] = left[i]; bufRRe[i] = right[i]; }
        fftDifForward(bufLRe, bufLIm);
        fftDifForward(bufRRe, bufRIm);
        float specLRe[kFftSize], specLIm[kFftSize], specRRe[kFftSize], specRIm[kFftSize];
        for (int i = 0; i < kFftSize; ++i) {
            specLRe[i] = bufLRe[i] * combLRe[i] - bufLIm[i] * combLIm[i];
            specLIm[i] = bufLRe[i] * combLIm[i] + bufLIm[i] * combLRe[i];
            specRRe[i] = bufRRe[i] * combRRe[i] - bufRIm[i] * combRIm[i];
            specRIm[i] = bufRRe[i] * combRIm[i] + bufRIm[i] * combRRe[i];
        }
        fftDitInverse(specLRe, specLIm);
        fftDitInverse(specRRe, specRIm);
        for (int i = 0; i < kFftSize; ++i) {
            fOverlapL[i] += specLRe[i];
            fOverlapR[i] += specRRe[i];
        }
        for (int i = 0; i < nFrames; ++i) {
            left[i]  = fOverlapL[i];
            right[i] = fOverlapR[i];
        }
        std::memmove(fOverlapL.data(), fOverlapL.data() + nFrames, (kFftSize - nFrames) * sizeof(float));
        std::memset(fOverlapL.data() + (kFftSize - nFrames), 0, nFrames * sizeof(float));
        std::memmove(fOverlapR.data(), fOverlapR.data() + nFrames, (kFftSize - nFrames) * sizeof(float));
        std::memset(fOverlapR.data() + (kFftSize - nFrames), 0, nFrames * sizeof(float));
    }
    void buildSpectrum(Channel& ch, int filtLen) {
        double ahp = std::exp(-2.0 * M_PI * static_cast<double>(ch.hpHz) / fSampleRate);
        double alp = std::exp(-2.0 * M_PI * static_cast<double>(ch.lpHz) / fSampleRate);
        double hpX1 = 0.0, hpY1 = 0.0, lpY1 = 0.0;
        ch.workRe.fill(0.0f); ch.workIm.fill(0.0f);
        for (int n = 0; n < filtLen; ++n) {
            double x = (n < static_cast<int>(ch.length)) ? static_cast<double>(ch.data[n]) : 0.0;
            double hy = ahp * (hpY1 + x - hpX1); hpX1 = x; hpY1 = hy;
            double ly = (1.0 - alp) * hy + alp * lpY1; lpY1 = ly;
            ch.workRe[n] = static_cast<float>(ly);
        }
        fftDifForward(ch.workRe.data(), ch.workIm.data());
        ch.specHp = ch.hpHz; ch.specLp = ch.lpHz; ch.specLen = filtLen; ch.specValid = true;
    }

    void initTwiddles() {
        for (int j = 0; j < kFftSize / 2; ++j) {
            double ang = -2.0 * M_PI * j / kFftSize;
            fTwRe[j] = static_cast<float>(std::cos(ang));
            fTwIm[j] = static_cast<float>(std::sin(ang));
        }
    }

    void fftDifForward(float* re, float* im) const {
        for (int len = kFftSize; len >= 2; len >>= 1) {
            int half = len / 2, stride = kFftSize / len;
            for (int i = 0; i < kFftSize; i += len) {
                for (int j = 0; j < half; ++j) {
                    float cwr = fTwRe[j * stride], cwi = fTwIm[j * stride];
                    float ur = re[i+j], ui = im[i+j], vr = re[i+j+half], vi = im[i+j+half];
                    re[i+j] = ur + vr; im[i+j] = ui + vi;
                    float dr = ur - vr, di = ui - vi;
                    re[i+j+half] = dr * cwr - di * cwi;
                    im[i+j+half] = dr * cwi + di * cwr;
                }
            }
        }
    }

    void fftDitInverse(float* re, float* im) const {
        for (int len = 2; len <= kFftSize; len <<= 1) {
            int half = len / 2, stride = kFftSize / len;
            for (int i = 0; i < kFftSize; i += len) {
                for (int j = 0; j < half; ++j) {
                    float cwr = fTwRe[j * stride], cwi = -fTwIm[j * stride];
                    float ur = re[i+j], ui = im[i+j], vr0 = re[i+j+half], vi0 = im[i+j+half];
                    float vr = vr0 * cwr - vi0 * cwi, vi = vr0 * cwi + vi0 * cwr;
                    re[i+j] = ur + vr; im[i+j] = ui + vi;
                    re[i+j+half] = ur - vr; im[i+j+half] = ui - vi;
                }
            }
        }
        for (int i = 0; i < kFftSize; ++i) { re[i] /= kFftSize; im[i] /= kFftSize; }
    }

    static uint32_t rdU32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
    static uint16_t rdU16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1]<<8)); }

    static float pcmToFloat(const uint8_t* p, int bps, int isFloat) {
        if (isFloat && bps == 4) { uint32_t b = rdU32(p); float f; std::memcpy(&f, &b, 4); return f; }
        if (bps == 1) return ((int)p[0] - 128) / 128.0f;
        if (bps == 2) return ((int16_t)rdU16(p)) / 32768.0f;
        if (bps == 3) { uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16); int32_t v = (u & 0x800000u) ? (int32_t)(u | 0xFF000000u) : (int32_t)u; return v / 8388608.0f; }
        if (bps == 4) return ((int32_t)rdU32(p)) / 2147483648.0f;
        return 0.0f;
    }

    static float* readWavMono(const char* path, uint32_t* outFrames, int* outSr) {
        FILE* f = std::fopen(path, "rb");
        if (!f) return nullptr;
        uint8_t hdr[12];
        if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) { std::fclose(f); return nullptr; }
        uint16_t fmt = 0, ch = 0, bits = 0; uint32_t sr = 0, dataSz = 0; long dataOff = 0; bool haveFmt = false;
        for (;;) {
            uint8_t chdr[8]; if (std::fread(chdr, 1, 8, f) != 8) break;
            uint32_t csz = rdU32(chdr + 4);
            if (std::memcmp(chdr, "fmt ", 4) == 0) {
                uint8_t fb[40]{}; uint32_t tr = std::min<uint32_t>(csz, 40);
                if (std::fread(fb, 1, tr, f) != tr) break;
                if (csz > tr) std::fseek(f, csz - tr, SEEK_CUR);
                fmt = rdU16(fb); ch = rdU16(fb + 2); sr = rdU32(fb + 4); bits = rdU16(fb + 14);
                if (fmt == 0xFFFE && csz >= 40) fmt = rdU16(fb + 24);
                haveFmt = true;
            } else if (std::memcmp(chdr, "data", 4) == 0) {
                dataOff = std::ftell(f); dataSz = csz; std::fseek(f, csz, SEEK_CUR);
            } else std::fseek(f, csz, SEEK_CUR);
            if (csz & 1) std::fseek(f, 1, SEEK_CUR);
            if (haveFmt && dataOff != 0) break;
        }
        if (!haveFmt || !dataOff || !ch || !bits || (fmt != 1 && fmt != 3)) { std::fclose(f); return nullptr; }
        int bps = bits / 8; uint32_t bpf = bps * ch, total = bpf ? (dataSz / bpf) : 0;
        if (!total) { std::fclose(f); return nullptr; }
        float* mono = static_cast<float*>(std::malloc(sizeof(float) * total));
        std::fseek(f, dataOff, SEEK_SET);
        std::vector<uint8_t> fbuf(bpf);
        for (uint32_t i = 0; i < total; ++i) {
            if (std::fread(fbuf.data(), 1, bpf, f) != bpf) { total = i; break; }
            mono[i] = pcmToFloat(fbuf.data(), bps, fmt == 3);
        }
        std::fclose(f); *outFrames = total; *outSr = static_cast<int>(sr);
        return mono;
    }

    static uint64_t resampleF32(const float* in, float* out, int inSr, int outSr, uint64_t inSz) {
        uint64_t outSz = inSz * outSr / inSr;
        if (!out) return outSz;
        double stepD = static_cast<double>(inSr) / static_cast<double>(outSr);
        const uint64_t fix = (1ULL << 32); const double norm = 1.0 / (1ULL << 32);
        uint64_t step = static_cast<uint64_t>(stepD * fix + 0.5), cur = 0;
        for (uint64_t i = 0; i < outSz; ++i) {
            (void)cur;
            float s0 = in[0];
            float s1 = (inSz > 1) ? in[1] : s0;
            *out++ = static_cast<float>(s0 + (s1 - s0) * ((cur & (fix - 1)) * norm));
            cur += step; in += (cur >> 32); if (inSz > (cur >> 32)) inSz -= (cur >> 32); else inSz = 1; cur &= (fix - 1);
        }
        return outSz;
    }

    double fSampleRate = 44100.0;
    bool fEnabled = true;
    std::array<float, kFftSize / 2> fTwRe{}, fTwIm{};
    std::array<float, kFftSize> fOverlapL{}, fOverlapR{};
    std::array<Channel, kNumChans> fChans{};
};
