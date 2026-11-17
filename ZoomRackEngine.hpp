#pragma once
#include "ZDLparser.hpp"
#include "QuadIrCab.hpp"
#include <array>
#include <cmath>
#include <cctype>
#include <memory>
#include <algorithm>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

template <typename T>
static inline T clampVal(T v, T lo, T hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static inline std::string toUpperStr(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

class IZoomEffectDSP {
public:
    virtual ~IZoomEffectDSP() = default;
    virtual void prepare(double sampleRate) = 0;
    virtual void reset() = 0;
    virtual void setParameter(int index, int32_t value) = 0;
    virtual void process(float* left, float* right, int numSamples) = 0;
};

class BypassEffectDSP : public IZoomEffectDSP {
public:
    void prepare(double) override {}
    void reset() override {}
    void setParameter(int, int32_t) override {}
    void process(float*, float*, int) override {}
};

// Univerzális ZDL DSP motor, amely a ZDL fájl paraméterei és neve alapján
// valós idejű erősítő/drive, EQ, vibrato/chorus/tremolo és delay/reverb feldolgozást végez.
class UniversalZdlDSP : public IZoomEffectDSP {
public:
    static constexpr int kDelayBufSize = 65536; // ~1.4 mp @ 44.1kHz

    explicit UniversalZdlDSP(const ZdlEffectMetadata& meta) : fMeta(meta) {
        std::string ename = toUpperStr(meta.effectName);

        fHasAmpOrDrive = (ename.find("FD ") != std::string::npos ||
                          ename.find("VIBRO") != std::string::npos ||
                          ename.find("AMP") != std::string::npos ||
                          ename.find("DRIVE") != std::string::npos ||
                          ename.find("DIST") != std::string::npos ||
                          ename.find("FUZZ") != std::string::npos ||
                          ename.find("MUFF") != std::string::npos ||
                          ename.find("DIRT") != std::string::npos ||
                          ename.find("OVERDRIVE") != std::string::npos ||
                          ename.find("BOOST") != std::string::npos ||
                          ename.find("SQUEAK") != std::string::npos ||
                          ename.find("BG ") != std::string::npos ||
                          ename.find("MS ") != std::string::npos ||
                          ename.find("TW ") != std::string::npos ||
                          ename.find("DZ ") != std::string::npos ||
                          ename.find("MATCH") != std::string::npos ||
                          ename.find("HW ") != std::string::npos);

        fHasModulation = (ename.find("VIBR") != std::string::npos ||
                          ename.find("CHOR") != std::string::npos ||
                          ename.find("FLANG") != std::string::npos ||
                          ename.find("PHAS") != std::string::npos ||
                          ename.find("TREM") != std::string::npos ||
                          ename.find("MOD") != std::string::npos ||
                          ename.find("CE-") != std::string::npos ||
                          ename.find("CLONE") != std::string::npos);

        fHasDelay = (ename.find("DELAY") != std::string::npos || ename.find("ECHO") != std::string::npos || ename.find("TAPE") != std::string::npos);
        fHasReverb = (ename.find("CHAMB") != std::string::npos || ename.find("CHURCH") != std::string::npos || ename.find("ARENA") != std::string::npos || ename.find("CAVE") != std::string::npos || ename.find("AIR") != std::string::npos || ename.find("EARLY") != std::string::npos || ename.find("TILED") != std::string::npos || ename.find("MODRV") != std::string::npos || ename.find("SPACE") != std::string::npos ||
                           ename.find("ECHO") != std::string::npos ||
                           ename.find("REV") != std::string::npos ||
                           ename.find("HALL") != std::string::npos ||
                           ename.find("ROOM") != std::string::npos ||
                           ename.find("SPRING") != std::string::npos ||
                           ename.find("PLATE") != std::string::npos ||
                           ename.find("HD ") != std::string::npos);

        fNormParams.fill(0.5f);
        for (size_t i = 0; i < meta.parameters.size() && i < 12; ++i) {
            std::string pname = toUpperStr(meta.parameters[i].name);
            if (pname.find("GAIN") != std::string::npos || pname.find("DRIVE") != std::string::npos ||
                pname.find("TUBE") != std::string::npos || pname.find("THRST") != std::string::npos) {
                fHasAmpOrDrive = true;
            }
            if (pname.find("DEPTH") != std::string::npos || pname.find("RATE") != std::string::npos ||
                pname.find("SPEED") != std::string::npos || pname.find("VIB") != std::string::npos ||
                pname.find("CHOIR") != std::string::npos || pname.find("PACE") != std::string::npos) {
                fHasModulation = true;
            }
            if (pname.find("TIME") != std::string::npos || pname.find("DECAY") != std::string::npos ||
                pname.find("F.B") != std::string::npos || pname.find("FB") != std::string::npos ||
                pname.find("DWELL") != std::string::npos) {
                if (pname.find("DECAY")!=std::string::npos || pname.find("DWELL")!=std::string::npos || pname.find("PREDLY")!=std::string::npos) fHasReverb = true; else if (!fHasReverb) fHasDelay = true;
            }
        }

        // Ha egyik kategóriába sem esett bele, alapból enyhe karakter + szűrő + moduláció engedélyezve
        if (!fHasAmpOrDrive && !fHasModulation && !fHasDelay && !fHasReverb) {
            fHasAmpOrDrive = true;
        }
    }

    void prepare(double sr) override {
        fSr = (sr > 0.0) ? static_cast<float>(sr) : 44100.0f;
        reset();
    }

    void reset() override {
        std::fill(std::begin(fDelayL), std::end(fDelayL), 0.0f);
        std::fill(std::begin(fDelayR), std::end(fDelayR), 0.0f);
        for(int c=0;c<4;++c){std::fill(std::begin(fRevCombL[c]),std::end(fRevCombL[c]),0.0f);std::fill(std::begin(fRevCombR[c]),std::end(fRevCombR[c]),0.0f);fRevCombPosL[c]=fRevCombPosR[c]=0;fRevCombLpL[c]=fRevCombLpR[c]=0.0f;}
        for(int a=0;a<2;++a){std::fill(std::begin(fRevApL[a]),std::end(fRevApL[a]),0.0f);std::fill(std::begin(fRevApR[a]),std::end(fRevApR[a]),0.0f);fRevApPosL[a]=fRevApPosR[a]=0;}
        fWritePos = 0;
        fLfoPhase = 0.0f;
        for (int c = 0; c < 2; ++c) {
            fLpTone[c] = fLowState[c] = fMidState[c] = fCabLp[c] = fCabHp[c] = fEnv[c] = 0.0f;
        }
    }

    void setParameter(int index, int32_t value) override {
        if (index < 0 || index >= static_cast<int>(fMeta.parameters.size()) || index >= 12) return;
        const auto& pm = fMeta.parameters[index];
        float span = static_cast<float>(pm.maxVal - pm.minVal);
        float norm = (span > 0.0f) ? clampVal((value - pm.minVal) / span, 0.0f, 1.0f) : 0.5f;
        fNormParams[index] = norm;
        updateInternalControls();
    }

    void process(float* left, float* right, int frames) override {
        float* chans[2] = { left, right };

        const float preGain = fHasAmpOrDrive ? (1.0f + fDrive * 12.0f + fTube * 6.0f) : (1.0f + fDrive * 2.0f);
        const float asymBias = 0.08f + fTube * 0.22f;
        const float toneCut = clampVal(0.06f + fTone * 0.65f, 0.03f, 0.85f);
        const float lowCoeff = clampVal(240.0f * 6.283185f / fSr, 0.005f, 0.4f);
        const float midCoeff = clampVal(1200.0f * 6.283185f / fSr, 0.02f, 0.7f);
        const float cabLpCoeff = clampVal(4200.0f * 6.283185f / fSr, 0.1f, 0.85f);
        const float cabHpCoeff = clampVal(85.0f * 6.283185f / fSr, 0.002f, 0.1f);

        const float lfoHz = 0.25f + fModRate * 8.5f;
        const float modDelayBase = (0.004f + fModDepth * 0.008f) * fSr;
        const float modDelaySwing = (fModDepth * 0.0035f) * fSr;

        const float echoSamples = (0.04f + fDelayTime * 0.55f) * fSr;
        const float feedback = clampVal(fDelayFb * 0.78f, 0.0f, 0.85f);
        const float revFb = clampVal(0.68f + fReverbDecay * 0.27f, 0.60f, 0.96f);
        const float revDamp = clampVal(0.18f + (1.0f - fTone) * 0.55f, 0.10f, 0.75f);
        const float preDlySamples = (0.002f + fPreDelay * 0.075f) * fSr;
        const float srScale = fSr / 44100.0f;
        const int combLenL[4] = { std::max(64, std::min(2040, (int)(1116*srScale))), std::max(64, std::min(2040, (int)(1188*srScale))), std::max(64, std::min(2040, (int)(1277*srScale))), std::max(64, std::min(2040, (int)(1356*srScale))) };
        const int combLenR[4] = { std::max(64, std::min(2040, (int)(1139*srScale))), std::max(64, std::min(2040, (int)(1211*srScale))), std::max(64, std::min(2040, (int)(1300*srScale))), std::max(64, std::min(2040, (int)(1379*srScale))) };
        const int apLenL[2] = { std::max(32, std::min(600, (int)(556*srScale))), std::max(32, std::min(600, (int)(441*srScale))) };
        const int apLenR[2] = { std::max(32, std::min(600, (int)(579*srScale))), std::max(32, std::min(600, (int)(464*srScale))) };
        const float outGain = fLevel * 1.8f;

        for (int i = 0; i < frames; ++i) {
            fLfoPhase += lfoHz / fSr;
            if (fLfoPhase >= 1.0f) fLfoPhase -= 1.0f;
            const float lfoL = std::sin(2.0f * static_cast<float>(M_PI) * fLfoPhase);
            const float lfoR = std::cos(2.0f * static_cast<float>(M_PI) * fLfoPhase);

            for (int c = 0; c < 2; ++c) {
                const float dry = chans[c][i];
                float x = dry;

                // 1. Erősítő / Csöves telítés / Drive szekció
                if (fHasAmpOrDrive) {
                    x *= preGain;
                    // Aszimmetrikus csöves karakterisztika (Tube + Gain)
                    float biased = x + asymBias;
                    x = std::tanh(biased * (1.0f + fTube * 1.2f)) - std::tanh(asymBias);
                    if (fDrive > 0.35f) {
                        x = std::atan(x * (1.2f + fDrive * 2.0f)) * (2.0f / static_cast<float>(M_PI));
                    }
                }

                // 2. Tone & 3-sávos EQ szekció
                fLpTone[c] += (x - fLpTone[c]) * toneCut;
                float eq = fLpTone[c];

                fLowState[c] += (eq - fLowState[c]) * lowCoeff;
                fMidState[c] += (eq - fMidState[c]) * midCoeff;

                const float lowBand  = fLowState[c];
                const float midBand  = fMidState[c] - fLowState[c];
                const float highBand = eq - fMidState[c];

                eq += lowBand  * (fBass - 0.5f) * 1.6f
                    + midBand  * (fMid  - 0.5f) * 1.6f
                    + highBand * (fTreble - 0.5f) * 1.6f;

                // Hangfal (Cabinet) szimuláció erősítő modellekhez
                if (fHasAmpOrDrive) {
                    fCabHp[c] += (eq - fCabHp[c]) * cabHpCoeff;
                    float hpOut = eq - fCabHp[c];
                    fCabLp[c] += (hpOut - fCabLp[c]) * cabLpCoeff;
                    eq = fCabLp[c];
                }

                chans[c][i] = eq;
            }

            // 3. Moduláció (Vibrato / Chorus / Tremolo) + Delay / Reverb vonal
            float sigL = chans[0][i];
            float sigR = chans[1][i];

            float delayedL = readInterp(fDelayL, fWritePos - echoSamples);
            float delayedR = readInterp(fDelayR, fWritePos - echoSamples * 1.07f);

            fDelayL[fWritePos] = sigL + (fHasDelay ? delayedR * feedback : 0.0f);
            fDelayR[fWritePos] = sigR + (fHasDelay ? delayedL * feedback : 0.0f);

            if (fHasModulation && fModDepth > 0.01f) {
                float modL = readInterp(fDelayL, fWritePos - (modDelayBase + lfoL * modDelaySwing));
                float modR = readInterp(fDelayR, fWritePos - (modDelayBase + lfoR * modDelaySwing));
                // Tremolo + Pitch/Chorus vibrato kombináció (pl. FD VIBRO-hoz)
                float trem = 1.0f - (fModDepth * 0.35f * (0.5f + 0.5f * lfoL));
                sigL = (sigL * (1.0f - fModDepth * 0.6f) + modL * (fModDepth * 0.6f)) * trem;
                sigR = (sigR * (1.0f - fModDepth * 0.6f) + modR * (fModDepth * 0.6f)) * trem;
            }

            if (fHasDelay) {
                sigL = sigL * (1.0f - fWetMix * 0.45f) + delayedL * (fWetMix * 0.75f);
                sigR = sigR * (1.0f - fWetMix * 0.45f) + delayedR * (fWetMix * 0.75f);
            }
            if (fHasReverb) {
                float revInL = readInterp(fDelayL, fWritePos - preDlySamples) * 0.25f;
                float revInR = readInterp(fDelayR, fWritePos - preDlySamples) * 0.25f;
                float sumL = 0.0f, sumR = 0.0f;
                for (int c = 0; c < 4; ++c) {
                    float oL = fRevCombL[c][fRevCombPosL[c]], oR = fRevCombR[c][fRevCombPosR[c]];
                    fRevCombLpL[c] = oL * (1.0f - revDamp) + fRevCombLpL[c] * revDamp;
                    fRevCombLpR[c] = oR * (1.0f - revDamp) + fRevCombLpR[c] * revDamp;
                    fRevCombL[c][fRevCombPosL[c]] = revInL + fRevCombLpL[c] * revFb;
                    fRevCombR[c][fRevCombPosR[c]] = revInR + fRevCombLpR[c] * revFb;
                    if (++fRevCombPosL[c] >= combLenL[c]) fRevCombPosL[c] = 0;
                    if (++fRevCombPosR[c] >= combLenR[c]) fRevCombPosR[c] = 0;
                    sumL += oL; sumR += oR;
                }
                for (int a = 0; a < 2; ++a) {
                    float bL = fRevApL[a][fRevApPosL[a]], iL = sumL; sumL = -iL * 0.5f + bL; fRevApL[a][fRevApPosL[a]] = iL + bL * 0.5f;
                    if (++fRevApPosL[a] >= apLenL[a]) fRevApPosL[a] = 0;
                    float bR = fRevApR[a][fRevApPosR[a]], iR = sumR; sumR = -iR * 0.5f + bR; fRevApR[a][fRevApPosR[a]] = iR + bR * 0.5f;
                    if (++fRevApPosR[a] >= apLenR[a]) fRevApPosR[a] = 0;
                }
                sigL = sigL * (1.0f - fWetMix * 0.45f) + (sumL * 0.85f + sumR * 0.15f) * (fWetMix * 0.85f);
                sigR = sigR * (1.0f - fWetMix * 0.45f) + (sumR * 0.85f + sumL * 0.15f) * (fWetMix * 0.85f);
            }

            chans[0][i] = clampVal(sigL * outGain, -1.5f, 1.5f);
            chans[1][i] = clampVal(sigR * outGain, -1.5f, 1.5f);

            fWritePos = (fWritePos + 1) & (kDelayBufSize - 1);
        }
    }

private:
    void updateInternalControls() {
        // Alapértékek, ha egy adott paraméter nincs külön elnevezve a ZDL-ben
        fDrive     = fNormParams[0];
        fTube      = fNormParams[1];
        fLevel     = 0.7f;
        fTone      = 0.65f;
        fBass      = 0.5f;
        fMid       = 0.5f;
        fTreble    = 0.5f;
        fModDepth  = fHasModulation ? 0.45f : 0.0f;
        fModRate   = 0.4f;
        fDelayTime = 0.35f; fReverbDecay = 0.55f; fPreDelay = 0.15f;
        fDelayFb   = 0.35f;
        fWetMix    = 0.5f;

        for (size_t i = 0; i < fMeta.parameters.size() && i < 12; ++i) {
            std::string p = toUpperStr(fMeta.parameters[i].name);
            float v = fNormParams[i];

            if (p.find("GAIN") != std::string::npos || p.find("DRIVE") != std::string::npos ||
                p.find("DIST") != std::string::npos || p.find("SUST") != std::string::npos ||
                p.find("COMP") != std::string::npos || p.find("SENS") != std::string::npos) {
                fDrive = v;
            } else if (p.find("TUBE") != std::string::npos || p.find("THRST") != std::string::npos ||
                       p.find("CHAR") != std::string::npos || p.find("BODY") != std::string::npos) {
                fTube = v;
            } else if (p.find("LEVEL") != std::string::npos || p.find("VOL") != std::string::npos ||
                       p.find("OUT") != std::string::npos) {
                fLevel = v;
            } else if (p.find("TONE") != std::string::npos || p.find("PRES") != std::string::npos ||
                       p.find("CUT") != std::string::npos || p.find("HI-DMP") != std::string::npos) {
                fTone = v;
            } else if (p.find("BASS") != std::string::npos || p.find("LOW") != std::string::npos ||
                       p.find("LO") != std::string::npos || p.find("BOTTOM") != std::string::npos) {
                fBass = v;
            } else if (p.find("MID") != std::string::npos) {
                fMid = v;
            } else if (p.find("TREB") != std::string::npos || p.find("HIGH") != std::string::npos ||
                       p.find("HI") != std::string::npos || p.find("TOP") != std::string::npos) {
                fTreble = v;
            } else if (p.find("DEPTH") != std::string::npos || p.find("VIB") != std::string::npos ||
                       p.find("CHOIR") != std::string::npos || p.find("INTENS") != std::string::npos ||
                       p.find("WIDTH") != std::string::npos) {
                fModDepth = v;
                fHasModulation = true;
            } else if (p.find("RATE") != std::string::npos || p.find("SPEED") != std::string::npos ||
                       p.find("PACE") != std::string::npos || p.find("FREQ") != std::string::npos) {
                fModRate = v;
                fHasModulation = true;
            } else if (p.find("TIME") != std::string::npos || p.find("DECAY") != std::string::npos ||
                       p.find("SIZE") != std::string::npos || p.find("PREDLY") != std::string::npos) {
                if (p.find("PREDLY")!=std::string::npos) { fPreDelay = v; fHasReverb = true; } else if (fHasReverb || p.find("DECAY")!=std::string::npos || p.find("SIZE")!=std::string::npos) { fReverbDecay = v; fHasReverb = true; } else { fDelayTime = v; }
            } else if (p.find("F.B") != std::string::npos || p.find("FB") != std::string::npos ||
                       p.find("FEED") != std::string::npos || p.find("TAIL") != std::string::npos) {
                fDelayFb = v;
            } else if (p.find("MIX") != std::string::npos || p.find("BAL") != std::string::npos ||
                       p.find("DWEL") != std::string::npos) {
                fWetMix = v;
            }
        }
    }

    static float readInterp(const float* buf, float pos) {
        while (pos < 0.0f) pos += kDelayBufSize;
        int i0 = static_cast<int>(pos) & (kDelayBufSize - 1);
        int i1 = (i0 + 1) & (kDelayBufSize - 1);
        float frac = pos - std::floor(pos);
        return buf[i0] + (buf[i1] - buf[i0]) * frac;
    }

    ZdlEffectMetadata fMeta;
    std::array<float, 12> fNormParams{};
    float fSr = 44100.0f;

    bool fHasAmpOrDrive = false;
    bool fHasModulation = false;
    bool fHasDelay = false;
    bool fHasReverb = false;
    float fReverbDecay = 0.55f, fPreDelay = 0.15f;
    float fRevCombL[4][2048]{}, fRevCombR[4][2048]{}, fRevCombLpL[4]{}, fRevCombLpR[4]{};
    int fRevCombPosL[4]{}, fRevCombPosR[4]{}, fRevApPosL[2]{}, fRevApPosR[2]{};
    float fRevApL[2][640]{}, fRevApR[2][640]{};

    float fDrive = 0.5f, fTube = 0.4f, fLevel = 0.7f, fTone = 0.65f;
    float fBass = 0.5f, fMid = 0.5f, fTreble = 0.5f;
    float fModDepth = 0.0f, fModRate = 0.4f;
    float fDelayTime = 0.3f, fDelayFb = 0.3f, fWetMix = 0.5f;

    float fDelayL[kDelayBufSize]{}, fDelayR[kDelayBufSize]{};
    int   fWritePos = 0;
    float fLfoPhase = 0.0f;
    float fLpTone[2]{}, fLowState[2]{}, fMidState[2]{}, fCabLp[2]{}, fCabHp[2]{}, fEnv[2]{};
};

struct ZoomSlot {
    bool enabled = false;
    std::string zdlPath;
    ZdlEffectMetadata metadata;
    std::vector<int32_t> currentValues;
    std::unique_ptr<IZoomEffectDSP> dsp = std::unique_ptr<IZoomEffectDSP>(new BypassEffectDSP());
};

class ZoomRackEngine {
public:
    static constexpr int kMaxSlots = 15;

    ZoomRackEngine() {
        prepare(44100.0);
    }

    void prepare(double sampleRate) {
        fSampleRate = sampleRate;
        fCab.prepare(sampleRate);
        for (auto& slot : fSlots) {
            if (slot.dsp) slot.dsp->prepare(sampleRate);
        }
    }

    void clearSlot(int slotIndex) {
        if (slotIndex < 0 || slotIndex >= kMaxSlots) return;
        auto& slot = fSlots[slotIndex];
        slot.enabled = false;
        slot.zdlPath.clear();
        slot.metadata = ZdlEffectMetadata{};
        slot.currentValues.clear();
        slot.dsp = std::unique_ptr<IZoomEffectDSP>(new BypassEffectDSP());
        slot.dsp->prepare(fSampleRate);
    }

    bool loadZdlIntoSlot(int slotIndex, const std::string& zdlPath) {
        if (slotIndex < 0 || slotIndex >= kMaxSlots) return false;
        if (zdlPath.empty()) {
            clearSlot(slotIndex);
            return true;
        }

        ZdlEffectMetadata meta = ZdlParser::parseFile(zdlPath);
        if (!meta.valid) return false;

        auto& slot = fSlots[slotIndex];
        slot.zdlPath  = zdlPath;
        slot.metadata = meta;
        slot.enabled  = true;
        slot.dsp      = std::unique_ptr<IZoomEffectDSP>(new UniversalZdlDSP(meta));
        slot.dsp->prepare(fSampleRate);

        slot.currentValues.resize(meta.parameters.size());
        for (size_t i = 0; i < meta.parameters.size(); ++i) {
            slot.currentValues[i] = meta.parameters[i].defaultVal;
            slot.dsp->setParameter(static_cast<int>(i), slot.currentValues[i]);
        }
        return true;
    }

    void setSlotNormalizedParameter(int slotIndex, int paramIndex, float norm0to100) {
        if (slotIndex < 0 || slotIndex >= kMaxSlots) return;
        auto& slot = fSlots[slotIndex];
        if (paramIndex < 0 || paramIndex >= static_cast<int>(slot.currentValues.size())) return;

        const auto& pMeta = slot.metadata.parameters[paramIndex];
        float ratio = clampVal(norm0to100 / 100.0f, 0.0f, 1.0f);
        int32_t val = pMeta.minVal + static_cast<int32_t>(std::round(ratio * (pMeta.maxVal - pMeta.minVal)));
        setSlotParameter(slotIndex, paramIndex, val);
    }

    void setSlotParameter(int slotIndex, int paramIndex, int32_t value) {
        if (slotIndex < 0 || slotIndex >= kMaxSlots) return;
        auto& slot = fSlots[slotIndex];
        if (paramIndex < 0 || paramIndex >= static_cast<int>(slot.currentValues.size())) return;

        const auto& pMeta = slot.metadata.parameters[paramIndex];
        int32_t clamped = clampVal(value, pMeta.minVal, pMeta.maxVal);
        slot.currentValues[paramIndex] = clamped;
        if (slot.dsp) slot.dsp->setParameter(paramIndex, clamped);
    }

    void setSlotEnabled(int slotIndex, bool enabled) {
        if (slotIndex >= 0 && slotIndex < kMaxSlots) {
            fSlots[slotIndex].enabled = enabled;
        }
    }

    void processBlock(float* left, float* right, int numSamples, bool dualMode = false, float abMix0to100 = 50.0f) {
        if (!dualMode) {
            for (auto& slot : fSlots) {
                if (slot.enabled && slot.dsp) slot.dsp->process(left, right, numSamples);
            }
            fCab.process(left, right, numSamples);
            return;
        }
        // Slot 1..5 (index 0..4): Közös Pre-FX szekció
        for (int i = 0; i < 5; ++i) {
            if (fSlots[i].enabled && fSlots[i].dsp) fSlots[i].dsp->process(left, right, numSamples);
        }
        if (static_cast<int>(fBufAL.size()) < numSamples) {
            fBufAL.resize(numSamples); fBufAR.resize(numSamples);
            fBufBL.resize(numSamples); fBufBR.resize(numSamples);
        }
        std::copy(left, left + numSamples, fBufAL.begin());
        std::copy(right, right + numSamples, fBufAR.begin());
        std::copy(left, left + numSamples, fBufBL.begin());
        std::copy(right, right + numSamples, fBufBR.begin());
        // Slot 6..10 (index 5..9): Chain A
        for (int i = 5; i < 10; ++i) {
            if (fSlots[i].enabled && fSlots[i].dsp) fSlots[i].dsp->process(fBufAL.data(), fBufAR.data(), numSamples);
        }
        // Slot 11..15 (index 10..14): Chain B
        for (int i = 10; i < kMaxSlots; ++i) {
            if (fSlots[i].enabled && fSlots[i].dsp) fSlots[i].dsp->process(fBufBL.data(), fBufBR.data(), numSamples);
        }
        float mix = clampVal(abMix0to100 / 100.0f, 0.0f, 1.0f);
        float gainA = (mix <= 0.5f) ? 1.0f : (1.0f - (mix - 0.5f) * 2.0f);
        float gainB = (mix >= 0.5f) ? 1.0f : (mix * 2.0f);
        for (int s = 0; s < numSamples; ++s) {
            left[s]  = fBufAL[s] * gainA + fBufBL[s] * gainB;
            right[s] = fBufAR[s] * gainA + fBufBR[s] * gainB;
        }
        fCab.process(left, right, numSamples);
    }
    QuadIrCabEngine& getCab() { return fCab; }
    const QuadIrCabEngine& getCab() const { return fCab; }
    const ZoomSlot& getSlot(int slotIndex) const { return fSlots[slotIndex]; }

private:
    double fSampleRate = 44100.0;
    QuadIrCabEngine fCab;
    std::array<ZoomSlot, kMaxSlots> fSlots;
    std::vector<float> fBufAL, fBufAR, fBufBL, fBufBR;
};
