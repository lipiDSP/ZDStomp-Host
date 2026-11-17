#include "DistrhoPlugin.hpp"
#include "ZoomRackEngine.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>

START_NAMESPACE_DISTRHO

static constexpr uint32_t kMaxSlots = 15;
static constexpr uint32_t kParamsPerSlot = 10;
static constexpr uint32_t kSlotParamsCount = 1 + kMaxSlots * kParamsPerSlot; // 131 (0..130)[cite: 1]

static constexpr uint32_t kParamInputLevel  = kSlotParamsCount + 0;
static constexpr uint32_t kParamOutputLevel = kSlotParamsCount + 1;
static constexpr uint32_t kParamTunerActive = kSlotParamsCount + 2;
static constexpr uint32_t kParamDualMode    = kSlotParamsCount + 3;
static constexpr uint32_t kParamDualMix     = kSlotParamsCount + 4;
static constexpr uint32_t kParamPresetPrev  = kSlotParamsCount + 5;
static constexpr uint32_t kParamPresetNext  = kSlotParamsCount + 6;
static constexpr uint32_t kParamCabEnabled  = kSlotParamsCount + 7;
static constexpr uint32_t kParamCabParams   = kSlotParamsCount + 8; // 16 params: 4 chans x (Level,Pan,HP,LP)
static constexpr uint32_t kParamTunerFreq   = kSlotParamsCount + 24;
static constexpr uint32_t kParamMidiLearn   = kSlotParamsCount + 25;
static constexpr uint32_t kParamLastMidiCC  = kSlotParamsCount + 26;
static constexpr uint32_t kParamMidiMonitor = kSlotParamsCount + 27;
static constexpr uint32_t kTotalParameters  = kSlotParamsCount + 28;
static constexpr uint32_t kControllableParams = kSlotParamsCount + 24;
class ZoomMultiStompPlugin : public Plugin {
public:
    ZoomMultiStompPlugin()
        : Plugin(kTotalParameters, 0, kMaxSlots + 6),
          fTunerWritePos(0),
          fTunerHopCounter(0),
          fLearnTarget(-1),
          fMidiEventSeq(0) {
        fParamValues.fill(0.0f);
        fTunerBuffer.fill(0.0f);
        fParamMidiCC.fill(-1);
        fLastCCValue.fill(-1);

        for (uint32_t s = 0; s < kMaxSlots; ++s) {
            uint32_t base = 1 + s * kParamsPerSlot;
            fParamValues[base] = 0.0f;
            for (uint32_t p = 0; p < 9; ++p) {
                fParamValues[base + 1 + p] = 55.0f;
            }
        }

        fParamValues[kParamInputLevel]  = 25.0f;
        fParamValues[kParamOutputLevel] = 25.0f;
        fParamValues[kParamCabEnabled] = 1.0f;
        for (uint32_t c = 0; c < 4; ++c) {
            uint32_t cb = kParamCabParams + c * 4;
            fParamValues[cb + 0] = (c == 0) ? 88.2f : 0.0f;
            fParamValues[cb + 1] = 50.0f;
            fParamValues[cb + 2] = 0.0f;
            fParamValues[cb + 3] = 100.0f;
            for (uint32_t pt = 0; pt < 4; ++pt) fEngine.getCab().setChannelParam(c, pt, fParamValues[cb + pt]);
        }
        fParamValues[kParamTunerActive] = 0.0f;
        fParamValues[kParamTunerFreq]   = 0.0f;
        fParamValues[kParamMidiLearn]   = 0.0f;
        fParamValues[kParamLastMidiCC]  = 0.0f;
        fParamValues[kParamMidiMonitor] = 0.0f;
    }

protected:
    const char* getLabel() const override { return "ZoomMultiStomp"; }
    const char* getDescription() const override { return "Zoom MultiStomp 13-Slot ZDL Rack"; }
    const char* getMaker() const override { return "Zoom Emulator Project"; }
    const char* getHomePage() const override { return "https://example.com"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 6, 0); }
    int64_t getUniqueId() const override { return d_cconst('Z', 'M', 'S', 'R'); }

    void initParameter(uint32_t index, Parameter& parameter) override {
        parameter.hints = kParameterIsAutomatable;

        if (index == 0) {
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Master Bypass";
            parameter.symbol = "master_bypass";
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 1.0f;
            return;
        }

        if (index == kParamInputLevel) {
            parameter.name = "Input Level";
            parameter.symbol = "input_level";
            parameter.ranges.def = 25.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 100.0f;
            return;
        }

        if (index == kParamOutputLevel) {
            parameter.name = "Output Level";
            parameter.symbol = "output_level";
            parameter.ranges.def = 25.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 100.0f;
            return;
        }

        if (index == kParamDualMode) {
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Dual Chain Mode"; parameter.symbol = "dual_chain_mode";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            return;
        }
        if (index == kParamDualMix) {
            parameter.name = "Dual A/B Mix"; parameter.symbol = "dual_ab_mix";
            parameter.ranges.def = 50.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 100.0f;
            return;
        }
        if (index == kParamPresetPrev) {
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Preset Previous"; parameter.symbol = "preset_prev";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            return;
        }
        if (index == kParamCabEnabled) { parameter.hints |= kParameterIsBoolean; parameter.name = "Master Cab IR Enable"; parameter.symbol = "cab_ir_enable"; parameter.ranges.def = 1.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f; return; }
        if (index >= kParamCabParams && index < kParamCabParams + 16) {
            uint32_t sub = index - kParamCabParams; uint32_t ch = sub / 4, pt = sub % 4;
            static const char* kPNames[4] = {"Level", "Pan", "HighPass", "LowPass"};
            char nb[64], sb[64];
            std::snprintf(nb, sizeof(nb), "Cab IR %c %s", 'A' + ch, kPNames[pt]);
            std::snprintf(sb, sizeof(sb), "cab_ir_%c_%u", 'a' + ch, pt);
            parameter.name = nb; parameter.symbol = sb;
            parameter.ranges.def = (pt == 0) ? (ch == 0 ? 88.2f : 0.0f) : (pt == 1 ? 50.0f : (pt == 2 ? 0.0f : 100.0f));
            parameter.ranges.min = 0.0f; parameter.ranges.max = 100.0f; return;
        }
        if (index == kParamPresetNext) {
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Preset Next"; parameter.symbol = "preset_next";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            return;
        }
        if (index == kParamTunerActive) {
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Tuner Active";
            parameter.symbol = "tuner_active";
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 2.0f;
            return;
        }

        if (index == kParamTunerFreq) {
            parameter.hints = kParameterIsOutput;
            parameter.name = "Tuner Frequency";
            parameter.symbol = "tuner_freq";
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 1200.0f;
            return;
        }

        if (index == kParamMidiLearn) {
            parameter.hints = kParameterIsAutomatable | kParameterIsInteger;
            parameter.name = "MIDI Learn Target";
            parameter.symbol = "midi_learn_target";
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = static_cast<float>(kControllableParams + 1);
            return;
        }

        if (index == kParamLastMidiCC) {
            parameter.hints = kParameterIsOutput | kParameterIsInteger;
            parameter.name = "Last Learned MIDI CC";
            parameter.symbol = "last_learned_midi_cc";
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 65535.0f;
            return;
        }

        if (index == kParamMidiMonitor) {
            parameter.hints = kParameterIsOutput | kParameterIsInteger;
            parameter.name = "MIDI Monitor";
            parameter.symbol = "midi_monitor";
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 65535.0f;
            return;
        }

        uint32_t slot = (index - 1) / kParamsPerSlot;
        uint32_t sub  = (index - 1) % kParamsPerSlot;
        char nameBuf[64];
        char symBuf[64];

        if (sub == 0) {
            parameter.hints |= kParameterIsBoolean;
            std::snprintf(nameBuf, sizeof(nameBuf), "Slot %u Enable", slot + 1);
            std::snprintf(symBuf, sizeof(symBuf), "slot_%u_enable", slot + 1);
            parameter.name = nameBuf;
            parameter.symbol = symBuf;
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 1.0f;
        } else {
            std::snprintf(nameBuf, sizeof(nameBuf), "Slot %u Param %u", slot + 1, sub);
            std::snprintf(symBuf, sizeof(symBuf), "slot_%u_param_%u", slot + 1, sub);
            parameter.name = nameBuf;
            parameter.symbol = symBuf;
            parameter.ranges.def = 55.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 100.0f;
        }
    }

    float getParameterValue(uint32_t index) const override {
        return (index < kTotalParameters) ? fParamValues[index] : 0.0f;
    }

    void setParameterValue(uint32_t index, float value) override {
        if (index < kTotalParameters) fParamValues[index] = value;
        if (index == kParamCabEnabled) { fEngine.getCab().setEnabled(value > 0.5f); return; }
        if (index >= kParamCabParams && index < kParamCabParams + 16) {
            uint32_t sub = index - kParamCabParams;
            fEngine.getCab().setChannelParam(static_cast<int>(sub / 4), static_cast<int>(sub % 4), value);
            return;
        }
        fParamValues[index] = value;

        if (index == kParamMidiLearn) {
            int raw = static_cast<int>(std::round(value));
            fLearnTarget = (raw > 0 && raw <= static_cast<int>(kControllableParams)) ? (raw - 1) : -1;
            return;
        }

        if (index == 0 || index >= kSlotParamsCount) return;

        uint32_t slot = (index - 1) / kParamsPerSlot;
        uint32_t sub  = (index - 1) % kParamsPerSlot;

        if (sub == 0) {
            fEngine.setSlotEnabled(static_cast<int>(slot), value > 0.5f);
        } else {
            fEngine.setSlotNormalizedParameter(static_cast<int>(slot), static_cast<int>(sub - 1), value);
        }
    }

    void initState(uint32_t index, State& state) override {
        if (index >= kMaxSlots + 2 && index < kMaxSlots + 6) {
            uint32_t irIdx = index - (kMaxSlots + 2);
            char kb[32]; std::snprintf(kb, sizeof(kb), "cab_ir_%u_wav", irIdx);
            state.key = kb; state.defaultValue = ""; return;
        }
        if (index < kMaxSlots) {
            char keyBuf[32];
            char labelBuf[32];
            std::snprintf(keyBuf, sizeof(keyBuf), "slot_%u_zdl", index);
            std::snprintf(labelBuf, sizeof(labelBuf), "Slot %u ZDL Path", index + 1);
            state.key = keyBuf;
            state.label = labelBuf;
            state.defaultValue = "";
            state.hints = kStateIsFilenamePath;
            return;
        }
        if (index == kMaxSlots) {
            state.key = "midi_cc_map";
            state.label = "MIDI CC Map";
            state.defaultValue = "";
            state.hints = 0x0;
            return;
        }
        if (index == kMaxSlots + 1) {
            state.key = "midi_learn_target";
            state.label = "MIDI Learn Target";
            state.defaultValue = "-1";
            state.hints = 0x0;
        }
    }

    void setState(const char* key, const char* value) override {
        if (key == nullptr) return;
        if (std::strncmp(key, "cab_ir_", 7) == 0) {
            int irIdx = std::atoi(key + 7);
            if (irIdx >= 0 && irIdx < 4) {
                fCabPaths[irIdx] = value ? value : "";
                fEngine.getCab().loadWav(irIdx, fCabPaths[irIdx]);
            }
            return;
        }

        if (std::strcmp(key, "midi_learn_target") == 0) {
            int t = (value != nullptr) ? std::atoi(value) : -1;
            fLearnTarget = (t >= 0 && t < static_cast<int>(kControllableParams)) ? t : -1;
            fParamValues[kParamMidiLearn] = static_cast<float>(fLearnTarget + 1);
            return;
        }

        if (std::strcmp(key, "midi_cc_map") == 0) {
            fParamMidiCC.fill(-1);
            if (value != nullptr && value[0] != '\0') {
                std::istringstream iss(value);
                for (size_t i = 0; i < kControllableParams; ++i) {
                    int cc = -1;
                    if (iss >> cc) {
                        fParamMidiCC[i] = (cc >= 0 && cc < 256) ? cc : -1;
                    }
                }
            }
            return;
        }

        unsigned int slot = 0;
        if (std::sscanf(key, "slot_%u_zdl", &slot) == 1 && slot < kMaxSlots) {
            std::string path = (value != nullptr) ? value : "";
            fSlotPaths[slot] = path;

            if (path.empty()) {
                fEngine.clearSlot(static_cast<int>(slot));
                fParamValues[1 + slot * kParamsPerSlot] = 0.0f;
            } else {
                if (fEngine.loadZdlIntoSlot(static_cast<int>(slot), path)) {
                    uint32_t base = 1 + slot * kParamsPerSlot;
                    bool isOn = (fParamValues[base] > 0.5f);
                    fEngine.setSlotEnabled(static_cast<int>(slot), isOn);
                    for (uint32_t p = 0; p < 9; ++p) {
                        fEngine.setSlotNormalizedParameter(static_cast<int>(slot), static_cast<int>(p), fParamValues[base + 1 + p]);
                    }
                }
            }
        }
    }

    String getState(const char* key) const override {
        if (key == nullptr) return String("");
        if (std::strncmp(key, "cab_ir_", 7) == 0) {
            int irIdx = std::atoi(key + 7);
            if (irIdx >= 0 && irIdx < 4) return String(fCabPaths[irIdx].c_str());
        }
        if (std::strcmp(key, "midi_learn_target") == 0) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", fLearnTarget);
            return String(buf);
        }
        if (std::strcmp(key, "midi_cc_map") == 0) {
            std::ostringstream oss;
            for (size_t i = 0; i < kControllableParams; ++i) {
                oss << fParamMidiCC[i] << (i + 1 == kControllableParams ? "" : " ");
            }
            return String(oss.str().c_str());
        }
        unsigned int slot = 0;
        if (std::sscanf(key, "slot_%u_zdl", &slot) == 1 && slot < kMaxSlots) {
            return String(fSlotPaths[slot].c_str());
        }
        return String("");
    }

    void activate() override {
        fEngine.prepare(getSampleRate());
        fTunerBuffer.fill(0.0f);
        fTunerWritePos = 0;
        fTunerHopCounter = 0;
    }

    void sampleRateChanged(double newSampleRate) override {
        fEngine.prepare(newSampleRate);
    }

    void run(const float** inputs, float** outputs, uint32_t frames,
             const MidiEvent* midiEvents, uint32_t midiEventCount) override {
        for (uint32_t m = 0; m < midiEventCount; ++m) {
            const MidiEvent& ev = midiEvents[m];
            if (ev.size < 2) continue;

            const uint8_t status = ev.data[0] & 0xF0;
            int controllerId = -1, controllerVal = -1;
            if (status == 0xB0 && ev.size >= 3) { controllerId = ev.data[1] & 0x7F; controllerVal = ev.data[2] & 0x7F; }
            else if (status == 0x90 && ev.size >= 3) { controllerId = 128 + (ev.data[1] & 0x7F); controllerVal = (ev.data[2] & 0x7F) > 0 ? 127 : 0; }
            else if (status == 0x80 && ev.size >= 3) { controllerId = 128 + (ev.data[1] & 0x7F); controllerVal = 0; }
            else if (status == 0xC0 && ev.size >= 2) { controllerId = 192 + (ev.data[1] & 0x3F); controllerVal = 127; }
            if (controllerId >= 0) {
                fMidiEventSeq = (fMidiEventSeq % 7) + 1;
                fParamValues[kParamMidiMonitor] = static_cast<float>(fMidiEventSeq * 8192 + controllerId * 128 + controllerVal);
            }
        }

        if (frames == 0) return;

        const float* inL = inputs[0];
        const float* inR = inputs[1];
        float* outL = outputs[0];
        float* outR = outputs[1];

        const float inGain  = std::max(0.0f, fParamValues[kParamInputLevel] / 50.0f);
        const float outGain = std::max(0.0f, fParamValues[kParamOutputLevel] / 50.0f);
        const int tunerMode = static_cast<int>(std::round(fParamValues[kParamTunerActive]));
        const bool tunerOn  = (tunerMode >= 1);
        const bool tunerMute = (tunerMode == 1);
        for (uint32_t i = 0; i < frames; ++i) {
            float sL = inL[i] * inGain;
            float sR = inR[i] * inGain;
            outL[i] = sL;
            outR[i] = sR;
            if (tunerOn) {
                float rawIn = (std::fabs(inL[i]) >= std::fabs(inR[i])) ? inL[i] : inR[i];
                fTunerBuffer[fTunerWritePos] = rawIn;
                fTunerWritePos = (fTunerWritePos + 1) % kTunerBufSize;
                if (++fTunerHopCounter >= 1024) {
                    fTunerHopCounter = 0;
                    fParamValues[kParamTunerFreq] = detectPitch(getSampleRate());
                }
            }
        }
        if (!tunerOn) {
            fParamValues[kParamTunerFreq] = 0.0f;
        } else if (tunerMute) {
            std::memset(outL, 0, sizeof(float) * frames);
            std::memset(outR, 0, sizeof(float) * frames);
            return;
        }
        if (fParamValues[0] <= 0.5f) {
            const bool dualOn = (fParamValues[kParamDualMode] > 0.5f);
            const float abMix = fParamValues[kParamDualMix];
            fEngine.processBlock(outL, outR, static_cast<int>(frames), dualOn, abMix);
        }
        if (std::abs(outGain - 1.0f) > 0.001f) {
            for (uint32_t i = 0; i < frames; ++i) { outL[i] *= outGain; outR[i] *= outGain; }
        }
    }
private:
    static constexpr size_t kTunerBufSize = 4096;
    static bool isBooleanParam(uint32_t index) {
        if (index == 0 || index == kParamTunerActive || index == kParamDualMode || index == kParamPresetPrev || index == kParamPresetNext || index == kParamCabEnabled) return true;
        if (index >= 1 && index < kSlotParamsCount) return ((index - 1) % kParamsPerSlot) == 0;
        return false;
    }
    float detectPitch(double sampleRate) const {
        const size_t W = 1536;
        std::array<float, kTunerBufSize> lin;
        for (size_t i = 0; i < kTunerBufSize; ++i) lin[i] = fTunerBuffer[(fTunerWritePos + i) % kTunerBufSize];
        float rms = 0.0f;
        for (size_t i = kTunerBufSize - W; i < kTunerBufSize; ++i) rms += lin[i] * lin[i];
        rms = std::sqrt(rms / static_cast<float>(W));
        if (rms < 0.0008f) return 0.0f;
        const size_t minLag = static_cast<size_t>(sampleRate / 1000.0);
        const size_t maxLag = std::min<size_t>(kTunerBufSize / 2 - 2, static_cast<size_t>(sampleRate / 55.0));
        std::vector<float> d(maxLag + 2, 0.0f), cmndf(maxLag + 2, 1.0f);
        const float* x = &lin[kTunerBufSize - maxLag - W - 1];
        float runningSum = 0.0f;
        for (size_t tau = 1; tau <= maxLag + 1; ++tau) {
            float diffSum = 0.0f;
            for (size_t i = 0; i < W; ++i) { float delta = x[i] - x[i + tau]; diffSum += delta * delta; }
            d[tau] = diffSum; runningSum += diffSum;
            if (runningSum > 0.0f) cmndf[tau] = diffSum * static_cast<float>(tau) / runningSum;
        }
        size_t bestTau = 0;
        for (size_t tau = minLag; tau <= maxLag; ++tau) {
            if (cmndf[tau] < 0.15f) { while (tau + 1 <= maxLag && cmndf[tau + 1] < cmndf[tau]) ++tau; bestTau = tau; break; }
        }
        if (bestTau == 0) {
            float minVal = 1e9f;
            for (size_t tau = minLag; tau <= maxLag; ++tau) if (cmndf[tau] < minVal) { minVal = cmndf[tau]; bestTau = tau; }
            if (minVal > 0.45f) return 0.0f;
        }
        float exactTau = static_cast<float>(bestTau);
        if (bestTau > minLag && bestTau < maxLag) {
            float s0 = cmndf[bestTau - 1], s1 = cmndf[bestTau], s2 = cmndf[bestTau + 1];
            float denom = 2.0f * (2.0f * s1 - s2 - s0);
            if (std::abs(denom) > 1e-6f) exactTau += (s2 - s0) / denom;
        }
        return (exactTau > 0.0f) ? static_cast<float>(sampleRate / exactTau) : 0.0f;
    }
    ZoomRackEngine fEngine;
    std::array<float, kTotalParameters> fParamValues;
    std::array<std::string, kMaxSlots> fSlotPaths;
    std::array<std::string, 4> fCabPaths;
    std::array<int, kControllableParams> fParamMidiCC;
    std::array<int, 256> fLastCCValue;
    std::array<float, kTunerBufSize> fTunerBuffer;
    size_t fTunerWritePos;
    uint32_t fTunerHopCounter;
    int fLearnTarget;
    int fMidiEventSeq;
    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZoomMultiStompPlugin)
};
Plugin* createPlugin() { return new ZoomMultiStompPlugin(); }
END_NAMESPACE_DISTRHO
