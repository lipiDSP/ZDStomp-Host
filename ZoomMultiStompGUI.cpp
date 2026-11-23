#include "DistrhoUI.hpp"
#include "DistrhoStandaloneUtils.hpp"
#include "ZDLparser.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#endif

START_NAMESPACE_DISTRHO

static constexpr uint32_t kMaxSlots = 15;
static constexpr uint32_t kParamsPerSlot = 10;
static constexpr uint32_t kSlotParamsCount = 1 + kMaxSlots * kParamsPerSlot; // 131[cite: 1]

static constexpr uint32_t kParamInputLevel  = kSlotParamsCount + 0;
static constexpr uint32_t kParamOutputLevel = kSlotParamsCount + 1;
static constexpr uint32_t kParamTunerActive = kSlotParamsCount + 2;
static constexpr uint32_t kParamDualMode    = kSlotParamsCount + 3;
static constexpr uint32_t kParamDualMix     = kSlotParamsCount + 4;
static constexpr uint32_t kParamPresetPrev  = kSlotParamsCount + 5;
static constexpr uint32_t kParamPresetNext  = kSlotParamsCount + 6;
static constexpr uint32_t kParamCabEnabled  = kSlotParamsCount + 7;
static constexpr uint32_t kParamCabParams   = kSlotParamsCount + 8;
static constexpr uint32_t kParamTunerFreq   = kSlotParamsCount + 24;
static constexpr uint32_t kParamMidiLearn   = kSlotParamsCount + 25;
static constexpr uint32_t kParamLastMidiCC  = kSlotParamsCount + 26;
static constexpr uint32_t kParamMidiMonitor = kSlotParamsCount + 27;
static constexpr uint32_t kTotalParameters  = kSlotParamsCount + 28;
static constexpr uint32_t kControllableParams = kSlotParamsCount + 24;
class ZoomMultiStompUI : public UI {
public:
    enum BrowserAction {
        kActionNone = 0,
        kActionLoadZdl,
        kActionSaveRig,
        kActionLoadRig,
        kActionLoadCabIr
    };

    ZoomMultiStompUI()
        : UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT),
          fSelectedSlot(0),
          fPendingBrowserSlot(0),
          fBrowserAction(kActionNone),
          fDraggingParam(-1),
          fDraggingLevel(-1),
          fFontId(-1),
          fSaveStatus(0),
          fLoadStatus(0),
          fClearAfterSave(false),
          fMidiLearnSelectMode(false),
          fLearnTarget(-1),
          fLastMonitoredId(-1),
          fLastMonitoredVal(-1) {
        fParamValues.fill(0.0f);
        fParamMidiCC.fill(-1);
        fLastCCValueUI.fill(-1);

        for (uint32_t s = 0; s < kMaxSlots; ++s) {
            uint32_t base = 1 + s * kParamsPerSlot;
            for (uint32_t p = 0; p < 9; ++p) {
                fParamValues[base + 1 + p] = 55.0f;
            }
        }

        fParamValues[kParamInputLevel]  = 25.0f;
        fParamValues[kParamOutputLevel] = 25.0f;
        fParamValues[kParamTunerActive] = 0.0f;
        fParamValues[kParamDualMode] = 0.0f; fParamValues[kParamDualMix] = 50.0f;
        fParamValues[kParamPresetPrev] = 0.0f; fParamValues[kParamPresetNext] = 0.0f;
        fCabEditMode = false; fFocusedParam = -1; fPendingCabIrSlot = 0;
        if (isUsingNativeAudio()) {
            if (supportsAudioInput() && !isAudioInputEnabled()) requestAudioInput();
            if (supportsMIDI() && !isMIDIEnabled()) requestMIDI();
        }
        fParamValues[kParamCabEnabled] = 1.0f;
        for (uint32_t c = 0; c < 4; ++c) {
            uint32_t cb = kParamCabParams + c * 4;
            fParamValues[cb + 0] = (c == 0) ? 88.2f : 0.0f;
            fParamValues[cb + 1] = 50.0f; fParamValues[cb + 2] = 0.0f; fParamValues[cb + 3] = 100.0f;
        }
        fParamValues[kParamTunerFreq]   = 0.0f;
        fParamValues[kParamMidiLearn]   = 0.0f;
        fParamValues[kParamLastMidiCC]  = 0.0f;
        fParamValues[kParamMidiMonitor] = 0.0f;

#ifdef _WIN32
        fFontId = createFontFromFile("sans", "C:\\Windows\\Fonts\\consola.ttf");
        if (fFontId < 0) fFontId = createFontFromFile("sans", "C:\\Windows\\Fonts\\arial.ttf");
        if (fFontId < 0) fFontId = createFontFromFile("sans", "C:\\Windows\\Fonts\\segoeui.ttf");
#elif defined(__APPLE__)
        fFontId = createFontFromFile("sans", "/System/Library/Fonts/Supplemental/Arial.ttf");
        if (fFontId < 0) fFontId = createFontFromFile("sans", "/System/Library/Fonts/Menlo.ttc");
        if (fFontId < 0) fFontId = createFontFromFile("sans", "/System/Library/Fonts/Helvetica.ttc");
        if (fFontId < 0) fFontId = createFontFromFile("sans", "/System/Library/Fonts/SFNS.ttf");
#else
        fFontId = createFontFromFile("sans", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf");
        if (fFontId < 0) fFontId = createFontFromFile("sans", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        if (fFontId < 0) fFontId = createFontFromFile("sans", "/usr/share/fonts/TTF/DejaVuSans.ttf");
#endif
    }

protected:
    void parameterChanged(uint32_t index, float value) override {
        if (index >= kTotalParameters) return;
        fParamValues[index] = value;

        if (index == kParamLastMidiCC && value > 0.5f) {
            int raw = static_cast<int>(std::round(value));
            int learnCode = raw % 8192;
            int paramIdx = learnCode / 256;
            int cc = learnCode % 256;
            if (paramIdx >= 0 && paramIdx < static_cast<int>(kControllableParams) && cc >= 0 && cc < 256) {
                fParamMidiCC[static_cast<size_t>(paramIdx)] = cc;
                fLearnTarget = -1;
                fMidiLearnSelectMode = false;
            }
        } else if (index == kParamMidiMonitor && value > 0.5f) {
            int raw = static_cast<int>(std::round(value));
            int baseCode = raw % 8192;
            int ccId  = baseCode / 128;
            int ccVal = baseCode % 128;
            fLastMonitoredId  = ccId;
            fLastMonitoredVal = ccVal;

            if (fLearnTarget >= 0 && fLearnTarget < static_cast<int>(kControllableParams) && ccId >= 0 && ccId < 256) {
                if (ccVal > 0 || ccId < 128) {
                    fParamMidiCC[static_cast<size_t>(fLearnTarget)] = ccId;
                    fLearnTarget = -1;
                    fMidiLearnSelectMode = false;
                    setLearnTarget(-1);
                    syncMidiMapToDSP();
                }
                repaint();
                return;
            }

            if (ccId >= 0 && ccId < 256) {
                int prevVal = fLastCCValueUI[static_cast<size_t>(ccId)];
                fLastCCValueUI[static_cast<size_t>(ccId)] = ccVal;
                for (uint32_t idx = 0; idx < kControllableParams; ++idx) {
                    if (fParamMidiCC[idx] == ccId) {
                        if (isBooleanParam(idx)) {
                            if (ccVal >= 64 && prevVal < 64) {
                                if (idx == kParamPresetPrev) { stepPreset(-1); }
                                else if (idx == kParamPresetNext) { stepPreset(1); }
                                else { float next = (fParamValues[idx] > 0.5f) ? 0.0f : 1.0f; setParamAndNotify(idx, next); }
                            } else if (ccVal < 64 && prevVal >= 64 && ccId < 128) {
                                /* Latching switch off -> toggle as well if needed */
                            }
                        } else {
                            float normVal = (static_cast<float>(ccVal) / 127.0f) * 100.0f;
                            setParamAndNotify(idx, normVal);
                        }
                    }
                }
            }
        }
        repaint();
    }

    void stateChanged(const char* key, const char* value) override {
        if (key == nullptr) return;

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
            repaint();
            return;
        }

        unsigned int slot = 0;
        if (std::sscanf(key, "slot_%u_zdl", &slot) == 1 && slot < kMaxSlots) {
            std::string path = (value != nullptr) ? value : "";
            fSlotPaths[slot] = path;
            if (path.empty()) {
                fSlotMeta[slot] = ZdlEffectMetadata{};
            } else {
                fSlotMeta[slot] = ZdlParser::parseFile(path);
            }
            repaint();
        }
    }

    void uiFileBrowserSelected(const char* filename) override {
        const BrowserAction action = fBrowserAction;
        fBrowserAction = kActionNone;

        if (filename == nullptr || filename[0] == '\0') {
            fClearAfterSave = false;
            return;
        }

        if (action == kActionSaveRig) {
            std::string path = filename;
            if (path.size() < 5 || path.substr(path.size() - 5) != ".zrig") {
                path += ".zrig";
            }
            saveRigToFile(path);
            if (fClearAfterSave) {
                fClearAfterSave = false;
                clearAllSlots();
            }
            repaint();
            return;
        }

        if (action == kActionLoadRig) {
            loadRigFromFile(filename);
            repaint();
            return;
        }

        if (action == kActionLoadCabIr) {
            fCabIrPaths[fPendingCabIrSlot] = filename;
            char kb[32]; std::snprintf(kb, sizeof(kb), "cab_ir_%u_wav", fPendingCabIrSlot);
            setState(kb, filename);
            uint32_t cb = kParamCabParams + fPendingCabIrSlot * 4;
            if (fParamValues[cb + 0] <= 1.0f) setParamAndNotify(cb + 0, 88.2f);
            if (fParamValues[cb + 3] <= 1.0f) setParamAndNotify(cb + 3, 100.0f);
            repaint(); return;
        }
        if (action == kActionLoadZdl) {
            const uint32_t targetSlot = fPendingBrowserSlot;
            ZdlEffectMetadata meta = ZdlParser::parseFile(filename);
            if (meta.valid) {
                fSelectedSlot = targetSlot;
                fSlotPaths[targetSlot] = filename;
                fSlotMeta[targetSlot]  = meta;

                uint32_t base = 1 + targetSlot * kParamsPerSlot;

                setParamAndNotify(base + 0, 1.0f);
                for (uint32_t p = 0; p < 9; ++p) {
                    if (p < meta.parameters.size()) {
                        const auto& pm = meta.parameters[p];
                        float span = static_cast<float>(pm.maxVal - pm.minVal);
                        float norm = (span > 0.0f) ? ((pm.defaultVal - pm.minVal) * 100.0f / span) : 55.0f;
                        setParamAndNotify(base + 1 + p, norm);
                    }
                }

                char keyBuf[32];
                std::snprintf(keyBuf, sizeof(keyBuf), "slot_%u_zdl", targetSlot);
                setState(keyBuf, filename);

                repaint();
            }
        }
    }

    void onNanoDisplay() override {
        const float w = getWidth();
        const float h = getHeight();

        if (fFontId >= 0) {
            fontFaceId(fFontId);
        }

        const int learnTarget = fLearnTarget;
        const bool isLearningAny = fMidiLearnSelectMode || (learnTarget >= 0);

        // Background
        beginPath();
        rect(0, 0, w, h);
        fillColor(Color(24, 26, 30));
        fill();

        // Top Header
        beginPath();
        rect(0, 0, w, 48);
        fillColor(Color(15, 17, 20));
        fill();

        fontSize(15.0f);
        fillColor(Color(235, 240, 245));
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        text(14.0f, 24.0f, "ZD STOMP HOST RACK", nullptr);

        // Header Buttons: NEW, SAVE, LOAD, TUNER, MIDI LEARN
        if (isUsingNativeAudio() && supportsBufferSizeChanges() && getBufferSize() > 0) {
            char bufLbl[24]; std::snprintf(bufLbl, sizeof(bufLbl), "BUF:%u", getBufferSize());
            drawButton(148.0f, 9.0f, 56.0f, 30.0f, bufLbl, Color(45, 95, 95));
        }
        drawButton(208.0f, 9.0f, 68.0f, 30.0f, "NEW RIG", Color(130, 75, 45));

        const char* saveLabel = (fSaveStatus == 1) ? "SAVED!" : ((fSaveStatus == -1) ? "ERR" : "SAVE RIG");
        Color saveColor = (fSaveStatus == 1) ? Color(60, 180, 90) : ((fSaveStatus == -1) ? Color(180, 50, 50) : Color(45, 110, 80));
        drawButton(281.0f, 9.0f, 78.0f, 30.0f, saveLabel, saveColor);

        const char* loadLabel = (fLoadStatus == 1) ? "LOADED!" : ((fLoadStatus == -1) ? "ERR" : "LOAD RIG");
        Color loadColor = (fLoadStatus == 1) ? Color(60, 160, 210) : ((fLoadStatus == -1) ? Color(180, 50, 50) : Color(45, 90, 130));
        drawButton(364.0f, 9.0f, 78.0f, 30.0f, loadLabel, loadColor);

        const bool tunerOn = (fParamValues[kParamTunerActive] > 0.5f);
        std::string tunerLbl = formatWithCC(tunerOn ? "TUNER:ON" : "TUNER", kParamTunerActive);
        drawButton(447.0f, 9.0f, 74.0f, 30.0f,
                   tunerLbl.c_str(),
                   tunerOn ? Color(215, 135, 25) : Color(65, 72, 85),
                   learnTarget == static_cast<int>(kParamTunerActive));

        const char* midiLbl = (learnTarget >= 0) ? "WAIT CC.." : (fMidiLearnSelectMode ? "CLICK PRM" : "MIDI LRN");
        drawButton(526.0f, 9.0f, 82.0f, 30.0f,
                   midiLbl,
                   isLearningAny ? Color(225, 125, 20) : Color(85, 60, 120));

        std::string prevLbl = formatWithCC("< PRV", kParamPresetPrev);
        std::string nextLbl = formatWithCC("NXT >", kParamPresetNext);
        drawButton(612.0f, 9.0f, 52.0f, 30.0f, prevLbl.c_str(), Color(55, 85, 125), learnTarget == static_cast<int>(kParamPresetPrev));
        drawButton(668.0f, 9.0f, 52.0f, 30.0f, nextLbl.c_str(), Color(55, 85, 125), learnTarget == static_cast<int>(kParamPresetNext));
        char infoBuf[128];
        if (fLastMonitoredId >= 0) {
            if (fLastMonitoredId < 128) std::snprintf(infoBuf, sizeof(infoBuf), "MIDI: CC%d=%d", fLastMonitoredId, fLastMonitoredVal);
            else std::snprintf(infoBuf, sizeof(infoBuf), "MIDI: N%d=%d", fLastMonitoredId - 128, fLastMonitoredVal);
            fontSize(10.5f); fillColor(Color(120, 235, 150)); textAlign(ALIGN_LEFT | ALIGN_MIDDLE); text(726.0f, 16.0f, infoBuf, nullptr);
        }
        if (!fCurrentRigName.empty()) {
            fontSize(10.5f); fillColor(Color(180, 210, 240)); textAlign(ALIGN_LEFT | ALIGN_MIDDLE); text(726.0f, fLastMonitoredId >= 0 ? 33.0f : 24.0f, fCurrentRigName.c_str(), nullptr);
        }
        // Master Bypass button
        const bool masterBypass = (fParamValues[0] > 0.5f);
        std::string bypLbl = formatWithCC(masterBypass ? "BYPASSED" : "ACTIVE", 0);
        drawButton(w - 122.0f, 9.0f, 110.0f, 30.0f,
                   bypLbl.c_str(),
                   masterBypass ? Color(180, 50, 50) : Color(40, 130, 70),
                   learnTarget == 0);

        // 15 Stompbox tabs (Serial: 1 row of 15, Dual: Pre 1..5 + stacked Row A 6..10 / Row B 11..15)
        const bool dualModeOn = (fParamValues[kParamDualMode] > 0.5f);
        for (uint32_t s = 0; s < kMaxSlots; ++s) {
            float sx = 0.0f, slotSy = 0.0f, sw = 0.0f, slotSh = 0.0f;
            getSlotRect(s, w, dualModeOn, sx, slotSy, sw, slotSh);
            const uint32_t slotEnableIdx = 1 + s * kParamsPerSlot;
            const bool isSel = (s == fSelectedSlot);
            const bool isLoaded = fSlotMeta[s].valid;
            const bool isOn = (fParamValues[slotEnableIdx] > 0.5f);

            beginPath();
            roundedRect(sx, slotSy, sw, slotSh, 5.0f);
            if (isSel) fillColor(Color(55, 115, 200));
            else if (isLoaded && isOn) fillColor((dualModeOn && s >= 10) ? Color(45, 95, 115) : Color(40, 100, 60));
            else if (isLoaded) fillColor(Color(75, 65, 45));
            else fillColor((dualModeOn && s >= 5 && s < 10) ? Color(36, 44, 58) : ((dualModeOn && s >= 10) ? Color(46, 38, 56) : Color(42, 46, 54)));
            fill();

            if (learnTarget == static_cast<int>(slotEnableIdx)) {
                beginPath(); roundedRect(sx, slotSy, sw, slotSh, 5.0f);
                strokeColor(Color(255, 165, 30)); strokeWidth(2.5f); stroke();
            }

            const bool compact = (dualModeOn && s >= 5);
            beginPath();
            if (compact) roundedRect(sx + sw - 14.0f, slotSy + 5.0f, 9.0f, 9.0f, 3.0f);
            else roundedRect(sx + sw * 0.22f, slotSy + slotSh - 16.0f, sw * 0.56f, 10.0f, 4.0f);
            fillColor((isLoaded && isOn) ? Color(80, 220, 105) : Color(28, 31, 38));
            fill();

            char tabTitle[32];
            const char* chainSuffix = (!dualModeOn || s < 5) ? "" : ((s < 10) ? "A" : "B");
            if (fParamMidiCC[slotEnableIdx] >= 0) {
                int id = fParamMidiCC[slotEnableIdx];
                if (id < 128) std::snprintf(tabTitle, sizeof(tabTitle), "S%u%s C%d", s + 1, chainSuffix, id);
                else std::snprintf(tabTitle, sizeof(tabTitle), "S%u%s N%d", s + 1, chainSuffix, id - 128);
            } else {
                std::snprintf(tabTitle, sizeof(tabTitle), "S%u%s", s + 1, chainSuffix);
            }
            fontSize(compact ? 9.5f : 10.0f);
            fillColor(Color(255, 255, 255));
            if (compact) {
                textAlign(ALIGN_LEFT | ALIGN_TOP);
                text(sx + 5.0f, slotSy + 4.0f, tabTitle, nullptr);
            } else {
                textAlign(ALIGN_CENTER | ALIGN_TOP);
                text(sx + sw * 0.5f, slotSy + 6.0f, tabTitle, nullptr);
            }

            const std::string& fxName = isLoaded ? fSlotMeta[s].effectName : "Empty";
            char shortName[12];
            std::snprintf(shortName, sizeof(shortName), "%.9s", fxName.c_str());
            fontSize(compact ? 9.5f : 10.0f);
            fillColor(isLoaded ? Color(220, 255, 225) : Color(140, 145, 155));
            textAlign(ALIGN_CENTER | ALIGN_TOP);
            text(sx + sw * 0.5f, slotSy + (compact ? 19.0f : 24.0f), shortName, nullptr);
        }
        const uint32_t base = 1 + fSelectedSlot * kParamsPerSlot;
        const bool slotOn = (fParamValues[base] > 0.5f);
        const auto& meta = fSlotMeta[fSelectedSlot];
        const float controlsY = 138.0f;

        // 1. Slot ON/OFF button
        std::string fxBtnLbl = formatWithCC(slotOn ? "FX: ON" : "FX: OFF", base);
        drawButton(12.0f, controlsY, 100.0f, 38.0f,
                   fxBtnLbl.c_str(),
                   slotOn ? Color(46, 150, 75) : Color(90, 95, 105),
                   learnTarget == static_cast<int>(base));

        // 2. Zoom LCD Display
        drawZoomLcd(120.0f, controlsY - 8.0f, meta);

        // 3. Effect info box
        beginPath();
        roundedRect(260.0f, controlsY, 158.0f, 38.0f, 5.0f);
        fillColor(Color(18, 20, 24));
        fill();

        char slotHeader[128];
        if (meta.valid) {
            std::snprintf(slotHeader, sizeof(slotHeader), "[S%u] %s (v%s)",
                          fSelectedSlot + 1, meta.effectName.c_str(), meta.version.c_str());
        } else {
            std::snprintf(slotHeader, sizeof(slotHeader), "[S%u] No ZDL loaded", fSelectedSlot + 1);
        }
        fontSize(12.0f);
        fillColor(Color(235, 240, 245));
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        text(268.0f, controlsY + 19.0f, slotHeader, nullptr);

        // 4. LOAD FX...LE button
        drawButton(422.0f, controlsY, 96.0f, 38.0f, "LOAD FX...", Color(180, 95, 30));
        drawButton(522.0f, controlsY, 58.0f, 38.0f, "CLEAR", Color(120, 45, 45));
        drawButton(584.0f, controlsY, 46.0f, 38.0f, "< MOV", Color(60, 75, 95));
        drawButton(633.0f, controlsY, 46.0f, 38.0f, "MOV >", Color(60, 75, 95));
        const bool cabOn = (fParamValues[kParamCabEnabled] > 0.5f);
        drawButton(684.0f, controlsY, 64.0f, 18.0f, cabOn ? "CAB:ON" : "CAB:OFF", cabOn ? Color(35, 135, 120) : Color(65, 50, 50), learnTarget == static_cast<int>(kParamCabEnabled));
        drawButton(684.0f, controlsY + 20.0f, 64.0f, 18.0f, fCabEditMode ? "FX VIEW" : "CAB EDIT", fCabEditMode ? Color(210, 130, 35) : Color(55, 85, 115));
        const bool dualMode = (fParamValues[kParamDualMode] > 0.5f);
        std::string dualLbl = formatWithCC(dualMode ? "DUAL A/B" : "SERIAL", kParamDualMode);
        drawButton(753.0f, controlsY, 82.0f, 18.0f, dualLbl.c_str(), dualMode ? Color(155, 70, 185) : Color(60, 70, 85), learnTarget == static_cast<int>(kParamDualMode));
        drawLevelSlider(753.0f, controlsY + 21.0f, 82.0f, 17.0f, "MIX", fParamValues[kParamDualMix], kParamDualMix, learnTarget);
        drawLevelSlider(840.0f, controlsY, 108.0f, 17.0f, "IN", fParamValues[kParamInputLevel], kParamInputLevel, learnTarget);
        drawLevelSlider(840.0f, controlsY + 21.0f, 108.0f, 17.0f, "OUT", fParamValues[kParamOutputLevel], kParamOutputLevel, learnTarget);
        if (tunerOn) {
            drawTuner(18.0f, 205.0f, w - 36.0f, h - 225.0f);
            return;
        }
        if (fCabEditMode) {
            drawQuadCabPanel(18.0f, 196.0f, w - 36.0f, h - 240.0f, learnTarget);
            drawFineTuneBar(w, h);
            return;
        }
        const float startY = 205.0f;
        const float rowH = 26.0f;
        const size_t numParams = meta.valid ? std::min<size_t>(meta.parameters.size(), 9) : 0;

        if (!meta.valid) {
            fontSize(15.0f);
            fillColor(Color(160, 168, 180));
            textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
            char emptyMsg[128];
            std::snprintf(emptyMsg, sizeof(emptyMsg),
                          "Slot %u is empty. Click 'LOAD FX...' to load an effect!",
                          fSelectedSlot + 1);
            text(w * 0.5f, 330.0f, emptyMsg, nullptr);
        }

        for (size_t p = 0; p < numParams; ++p) {
            const float py = startY + p * rowH;
            const auto& pm = meta.parameters[p];
            const uint32_t paramIdx = base + 1 + p;
            const float norm = fParamValues[paramIdx];
            const float ratio = std::max(0.0f, std::min(1.0f, norm / 100.0f));
            const int32_t realVal = pm.minVal + static_cast<int32_t>(std::round(ratio * (pm.maxVal - pm.minVal)));
            const bool isWaitingCC = (learnTarget == static_cast<int>(paramIdx));
            const int mappedCC = fParamMidiCC[paramIdx];

            fontSize(14.0f);
            fillColor(isWaitingCC ? Color(255, 180, 50) : Color(230, 235, 240));
            textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
            text(24.0f, py + 10.0f, pm.name.c_str(), nullptr);

            const float barX = 170.0f;
            const float barW = 470.0f;
            beginPath();
            roundedRect(barX, py, barW, 20.0f, 4.0f);
            fillColor(Color(15, 17, 20));
            fill();

            beginPath();
            roundedRect(barX, py, barW * ratio, 20.0f, 4.0f);
            fillColor(isWaitingCC ? Color(225, 130, 25) : (slotOn ? Color(55, 145, 235) : Color(90, 105, 120)));
            fill();

            if (isWaitingCC) {
                beginPath();
                roundedRect(barX, py, barW, 20.0f, 4.0f);
                strokeColor(Color(255, 185, 50));
                strokeWidth(2.0f);
                stroke();
            }

            char valBuf[96];
            if (isWaitingCC) {
                std::snprintf(valBuf, sizeof(valBuf), "%d [%d..%d] [WAIT MIDI...]", realVal, pm.minVal, pm.maxVal);
            } else if (mappedCC >= 0) {
                if (mappedCC < 128) std::snprintf(valBuf, sizeof(valBuf), "%d [%d..%d] [CC%d]", realVal, pm.minVal, pm.maxVal, mappedCC);
                else std::snprintf(valBuf, sizeof(valBuf), "%d [%d..%d] [NOTE %d]", realVal, pm.minVal, pm.maxVal, mappedCC - 128);
            } else {
                std::snprintf(valBuf, sizeof(valBuf), "%d [%d..%d]", realVal, pm.minVal, pm.maxVal);
            }
            fontSize(12.5f);
            fillColor(mappedCC >= 0 ? Color(130, 230, 155) : Color(210, 220, 230));
            textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
            text(barX + barW + 14.0f, py + 10.0f, valBuf, nullptr);
        }

        fontSize(11.0f);
        fillColor(Color(135, 145, 160));
        textAlign(ALIGN_CENTER | ALIGN_BOTTOM);
        text(w * 0.5f, h - 6.0f,
             "Tip: Right-click any slider/button (or click 'MIDI LRN' then click a control) and press your MIDI button/pedal.",
             nullptr);
    }

    bool onMouse(const MouseEvent& ev) override {
        const float mx = ev.pos.getX();
        const float my = ev.pos.getY();
        const float w  = getWidth();

        if (!ev.press) {
            fDraggingParam = -1;
            fDraggingLevel = -1;
            return false;
        }

        const bool isRightClick = (ev.button == 2 || ev.button == 3);
        const bool isLeftClick  = (ev.button == 1);
        const float h = getHeight();
        if (isLeftClick && fFocusedParam >= 0 && my >= h - 38.0f && my <= h - 6.0f) {
            float by = h - 38.0f; uint32_t idx = (uint32_t)fFocusedParam; float cur = fParamValues[idx];
            if (hitTest(mx, my, 250.0f, by + 4.0f, 450.0f, 24.0f)) { fDraggingLevel = -100; updateLevelFromMouse(mx, 250.0f, 450.0f, idx); return true; }
            if (hitTest(mx, my, 710.0f, by + 4.0f, 38.0f, 24.0f)) { setParamAndNotify(idx, std::max(0.0f, cur - 1.0f)); repaint(); return true; }
            if (hitTest(mx, my, 752.0f, by + 4.0f, 44.0f, 24.0f)) { setParamAndNotify(idx, std::max(0.0f, cur - 0.1f)); repaint(); return true; }
            if (hitTest(mx, my, 800.0f, by + 4.0f, 44.0f, 24.0f)) { setParamAndNotify(idx, std::min(100.0f, cur + 0.1f)); repaint(); return true; }
            if (hitTest(mx, my, 848.0f, by + 4.0f, 38.0f, 24.0f)) { setParamAndNotify(idx, std::min(100.0f, cur + 1.0f)); repaint(); return true; }
            if (hitTest(mx, my, 892.0f, by + 4.0f, 44.0f, 24.0f)) {
                float defVal = (idx == kParamInputLevel || idx == kParamOutputLevel) ? 25.0f : 50.0f;
                if (idx >= kParamCabParams && idx < kParamCabParams + 16) {
                    uint32_t pt = (idx - kParamCabParams) % 4;
                    defVal = (pt == 0) ? 88.8f : (pt == 1 ? 50.0f : (pt == 2 ? 0.0f : 100.0f));
                }
                setParamAndNotify(idx, defVal); repaint(); return true;
            }
        }
        if (!isLeftClick && !isRightClick) return false;

        // MIDI LEARN button in header
        if (isLeftClick && hitTest(mx, my, 526.0f, 9.0f, 82.0f, 30.0f)) {
            if (fMidiLearnSelectMode || fLearnTarget >= 0) {
                fMidiLearnSelectMode = false;
                setLearnTarget(-1);
            } else {
                fMidiLearnSelectMode = true;
            }
            repaint();
            return true;
        }

        int clickedControllableParam = findControllableParamAt(mx, my, w);
        if (clickedControllableParam >= 0 && (isRightClick || fMidiLearnSelectMode)) {
            fDraggingParam = -1;
            fDraggingLevel = -1;
            handleMidiLearnClick(static_cast<uint32_t>(clickedControllableParam), isRightClick);
            return true;
        }

        if (!isLeftClick) return false;

        // NEW RIG button
        if (isUsingNativeAudio() && supportsBufferSizeChanges() && getBufferSize() > 0 && hitTest(mx, my, 148.0f, 9.0f, 56.0f, 30.0f)) {
            uint32_t cur = getBufferSize();
            uint32_t nxt = (cur <= 128) ? 256 : ((cur <= 256) ? 512 : 128);
            requestBufferSizeChange(nxt);
            repaint();
            return true;
        }
        if (hitTest(mx, my, 208.0f, 9.0f, 68.0f, 30.0f)) {
            fSaveStatus = 0;
            fLoadStatus = 0;
#ifdef _WIN32
            int res = MessageBoxA(NULL,
                                  "Do you want to save the current rig before clearing all slots?",
                                  "New Rig",
                                  MB_YESNOCANCEL | MB_ICONQUESTION | MB_TOPMOST);
            if (res == IDYES) {
                fClearAfterSave = true;
                fBrowserAction = kActionSaveRig;
                openBrowser(true);
            } else if (res == IDNO) {
                fClearAfterSave = false;
                clearAllSlots();
                repaint();
            }
#else
            clearAllSlots();
            repaint();
#endif
            return true;
        }

        // SAVE RIG... button
        if (hitTest(mx, my, 281.0f, 9.0f, 78.0f, 30.0f)) {
            fSaveStatus = 0;
            fLoadStatus = 0;
            fClearAfterSave = false;
            fBrowserAction = kActionSaveRig;
            openBrowser(true);
            return true;
        }

        // LOAD RIG... button
        if (hitTest(mx, my, 364.0f, 9.0f, 78.0f, 30.0f)) {
            fSaveStatus = 0;
            fLoadStatus = 0;
            fClearAfterSave = false;
            fBrowserAction = kActionLoadRig;
            openBrowser(false);
            return true;
        }

        // TUNER button
        if (hitTest(mx, my, 447.0f, 9.0f, 74.0f, 30.0f)) {
            float next = (fParamValues[kParamTunerActive] > 0.5f) ? 0.0f : 1.0f;
            setParamAndNotify(kParamTunerActive, next);
            repaint();
            return true;
        }

        fSaveStatus = 0;
        fLoadStatus = 0;

        // Master Bypass
        if (hitTest(mx, my, w - 122.0f, 9.0f, 110.0f, 30.0f)) {
            float next = (fParamValues[0] > 0.5f) ? 0.0f : 1.0f;
            setParamAndNotify(0, next);
            repaint();
            return true;
        }

        const float controlsY = 138.0f;

        if (hitTest(mx, my, 612.0f, 9.0f, 52.0f, 30.0f)) { stepPreset(-1); return true; }
        if (hitTest(mx, my, 668.0f, 9.0f, 52.0f, 30.0f)) { stepPreset(1); return true; }
        if (hitTest(mx, my, 684.0f, controlsY, 64.0f, 18.0f)) { setParamAndNotify(kParamCabEnabled, (fParamValues[kParamCabEnabled] > 0.5f) ? 0.0f : 1.0f); repaint(); return true; }
        if (hitTest(mx, my, 684.0f, controlsY + 20.0f, 64.0f, 18.0f)) { fCabEditMode = !fCabEditMode; repaint(); return true; }
        if (hitTest(mx, my, 753.0f, controlsY, 82.0f, 18.0f)) { setParamAndNotify(kParamDualMode, (fParamValues[kParamDualMode] > 0.5f) ? 0.0f : 1.0f); repaint(); return true; }
        if (hitTest(mx, my, 753.0f, controlsY + 21.0f, 82.0f, 17.0f)) { fFocusedParam = fDraggingLevel = static_cast<int>(kParamDualMix); updateLevelFromMouse(mx, 785.0f, 48.0f, kParamDualMix); return true; }
        if (hitTest(mx, my, 840.0f, controlsY, 108.0f, 17.0f)) { fFocusedParam = fDraggingLevel = static_cast<int>(kParamInputLevel); updateLevelFromMouse(mx, 872.0f, 74.0f, kParamInputLevel); return true; }
        if (hitTest(mx, my, 840.0f, controlsY + 21.0f, 108.0f, 17.0f)) { fFocusedParam = fDraggingLevel = static_cast<int>(kParamOutputLevel); updateLevelFromMouse(mx, 872.0f, 74.0f, kParamOutputLevel); return true; }
        const bool dualModeClick = (fParamValues[kParamDualMode] > 0.5f);
        for (uint32_t s = 0; s < kMaxSlots; ++s) {
            float sx = 0.0f, sy = 0.0f, sw = 0.0f, sh = 0.0f;
            getSlotRect(s, w, dualModeClick, sx, sy, sw, sh);
            if (hitTest(mx, my, sx, sy, sw, sh)) {
                if (fSelectedSlot == s && fSlotMeta[s].valid) {
                    uint32_t sBase = 1 + s * kParamsPerSlot;
                    float next = (fParamValues[sBase] > 0.5f) ? 0.0f : 1.0f;
                    setParamAndNotify(sBase, next);
                } else {
                    fSelectedSlot = s;
                }
                fDraggingParam = -1;
                repaint();
                return true;
            }
        }

        const uint32_t base = 1 + fSelectedSlot * kParamsPerSlot;

        // Slot ON/OFF switch
        if (hitTest(mx, my, 12.0f, controlsY, 100.0f, 38.0f)) {
            float next = (fParamValues[base] > 0.5f) ? 0.0f : 1.0f;
            setParamAndNotify(base, next);
            repaint();
            return true;
        }

        // LOAD FX...LE button
        if (hitTest(mx, my, 422.0f, controlsY, 96.0f, 38.0f)) {
            fPendingBrowserSlot = fSelectedSlot;
            fBrowserAction = kActionLoadZdl;
            FileBrowserOptions opts;
            opts.saving = false;
            opts.title = "Select Zoom .ZDL or .ZD2 Effect File";
            openFileBrowser(opts);
            return true;
        }

        // CLEAR SLOT button
        if (hitTest(mx, my, 522.0f, controlsY, 58.0f, 38.0f)) {
            fSlotPaths[fSelectedSlot].clear();
            fSlotMeta[fSelectedSlot] = ZdlEffectMetadata{};
            char keyBuf[32];
            std::snprintf(keyBuf, sizeof(keyBuf), "slot_%u_zdl", fSelectedSlot);
            setState(keyBuf, "");
            setParamAndNotify(base, 0.0f);
            repaint();
            return true;
        }

        // < MOV (Swap Left) button
        if (hitTest(mx, my, 584.0f, controlsY, 46.0f, 38.0f)) { if (fSelectedSlot > 0) { swapSlots(fSelectedSlot, fSelectedSlot - 1); fSelectedSlot--; repaint(); } return true; }
        if (hitTest(mx, my, 633.0f, controlsY, 46.0f, 38.0f)) { if (fSelectedSlot + 1 < kMaxSlots) { swapSlots(fSelectedSlot, fSelectedSlot + 1); fSelectedSlot++; repaint(); } return true; }
        if (fParamValues[kParamTunerActive] > 0.5f) {
            if (my >= 200.0f) {
                float nextMode = (std::round(fParamValues[kParamTunerActive]) == 1.0f) ? 2.0f : 1.0f;
                setParamAndNotify(kParamTunerActive, nextMode);
                repaint();
            }
            return true;
        }
        if (fCabEditMode) {
            for (uint32_t c = 0; c < 4; ++c) {
                float ry = 232.0f + c * 52.0f;
                if (hitTest(mx, my, 30.0f, ry, 86.0f, 24.0f)) {
                    fPendingCabIrSlot = c; fBrowserAction = kActionLoadCabIr;
                    FileBrowserOptions opts; opts.title = "Select Cabinet Impulse Response (.wav)"; openFileBrowser(opts); return true;
                }
                if (hitTest(mx, my, 120.0f, ry, 42.0f, 24.0f)) {
                    fCabIrPaths[c].clear(); char kb[32]; std::snprintf(kb, sizeof(kb), "cab_ir_%u_wav", c); setState(kb, ""); repaint(); return true;
                }
                for (uint32_t pt = 0; pt < 4; ++pt) {
                    float sx = 310.0f + pt * 155.0f;
                    if (hitTest(mx, my, sx, ry - 4.0f, 145.0f, 32.0f)) {
                        uint32_t pIdx = kParamCabParams + c * 4 + pt;
                        fFocusedParam = fDraggingLevel = static_cast<int>(pIdx);
                        updateLevelFromMouse(mx, sx + 34.0f, 108.0f, pIdx); return true;
                    }
                }
            }
            return true;
        }
        const auto& meta = fSlotMeta[fSelectedSlot];
        if (!meta.valid) return true;
        const size_t numParams = std::min<size_t>(meta.parameters.size(), 9);
        const float startY = 205.0f, rowH = 26.0f;
        for (size_t p = 0; p < numParams; ++p) {
            const float py = startY + p * rowH;
            if (hitTest(mx, my, 170.0f, py, 470.0f, 20.0f)) {
                fDraggingParam = static_cast<int>(p);
                updateSliderFromMouse(mx);
                return true;
            }
        }
        return false;
    }
    bool onMotion(const MotionEvent& ev) override {
        if (fDraggingParam >= 0) { updateSliderFromMouse(ev.pos.getX()); return true; }
        if (fDraggingLevel == -100 && fFocusedParam >= 0) { updateLevelFromMouse(ev.pos.getX(), 250.0f, 450.0f, (uint32_t)fFocusedParam); return true; }
        if (fDraggingLevel >= 0) {
            if (fDraggingLevel == static_cast<int>(kParamDualMix)) updateLevelFromMouse(ev.pos.getX(), 785.0f, 48.0f, kParamDualMix);
            else if (fDraggingLevel >= static_cast<int>(kParamCabParams) && fDraggingLevel < static_cast<int>(kParamCabParams + 16)) {
                uint32_t pt = (fDraggingLevel - kParamCabParams) % 4;
                updateLevelFromMouse(ev.pos.getX(), 310.0f + pt * 155.0f + 34.0f, 108.0f, static_cast<uint32_t>(fDraggingLevel));
            } else updateLevelFromMouse(ev.pos.getX(), 872.0f, 74.0f, static_cast<uint32_t>(fDraggingLevel));
            return true;
        }
        return false;
    }
    bool onScroll(const ScrollEvent& ev) override {
        int p = findControllableParamAt(ev.pos.getX(), ev.pos.getY(), getWidth());
        if (p < 0 && fFocusedParam >= 0 && ev.pos.getY() >= getHeight() - 38.0f) p = fFocusedParam;
        if (p >= 0 && !isBooleanParam((uint32_t)p)) {
            fFocusedParam = p; float step = (ev.mod & kModifierShift) ? 0.1f : 0.5f;
            setParamAndNotify((uint32_t)p, std::max(0.0f, std::min(100.0f, fParamValues[p] + static_cast<float>(ev.delta.getY()) * step)));
            repaint(); return true;
        }
        return false;
    }
private:
    static bool isBooleanParam(uint32_t index) {
        if (index == 0 || index == kParamTunerActive || index == kParamDualMode || index == kParamPresetPrev || index == kParamPresetNext || index == kParamCabEnabled) return true;
        if (index >= 1 && index < kSlotParamsCount) return ((index - 1) % kParamsPerSlot) == 0;
        return false;
    }
    static void getSlotRect(uint32_t s, float w, bool dualMode, float& sx, float& sy, float& sw, float& sh) {
        const float margin = 12.0f, totalW = w - margin * 2.0f;
        if (!dualMode) { float slotW = totalW / kMaxSlots; sx = margin + s * slotW; sy = 52.0f; sw = slotW - 3.0f; sh = 72.0f; }
        else {
            float colW = totalW / 10.0f;
            if (s < 5) { sx = margin + s * colW; sy = 52.0f; sw = colW - 4.0f; sh = 74.0f; }
            else if (s < 10) { sx = margin + (s - 5 + 5) * colW + 4.0f; sy = 50.0f; sw = colW - 4.0f; sh = 36.0f; }
            else { sx = margin + (s - 10 + 5) * colW + 4.0f; sy = 90.0f; sw = colW - 4.0f; sh = 36.0f; }
        }
    }
    static bool hitTest(float mx, float my, float x, float y, float w, float h) { return (mx >= x && mx <= (x + w) && my >= y && my <= (y + h)); }
    static void trimCrLf(std::string& s) { while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back(); }
    static std::string extractFilename(const std::string& path) { size_t pos = path.find_last_of("/\\"); return (pos == std::string::npos) ? path : path.substr(pos + 1); }

    std::string formatWithCC(const char* baseLabel, uint32_t paramIdx) const {
        if (paramIdx < kControllableParams && fParamMidiCC[paramIdx] >= 0) {
            char buf[64]; int id = fParamMidiCC[paramIdx];
            if (id < 128) std::snprintf(buf, sizeof(buf), "%s [C%d]", baseLabel, id);
            else std::snprintf(buf, sizeof(buf), "%s [N%d]", baseLabel, id - 128);
            return std::string(buf);
        }
        return std::string(baseLabel);
    }
    int findControllableParamAt(float mx, float my, float w) const {
        if (hitTest(mx, my, 447.0f, 9.0f, 74.0f, 30.0f)) return static_cast<int>(kParamTunerActive);
        if (hitTest(mx, my, 612.0f, 9.0f, 52.0f, 30.0f)) return static_cast<int>(kParamPresetPrev);
        if (hitTest(mx, my, 668.0f, 9.0f, 52.0f, 30.0f)) return static_cast<int>(kParamPresetNext);
        if (hitTest(mx, my, w - 122.0f, 9.0f, 110.0f, 30.0f)) return 0;
        const bool dualModeHit = (fParamValues[kParamDualMode] > 0.5f);
        for (uint32_t s = 0; s < kMaxSlots; ++s) {
            float sx = 0, sy = 0, sw = 0, sh = 0; getSlotRect(s, w, dualModeHit, sx, sy, sw, sh);
            if (hitTest(mx, my, sx, sy, sw, sh)) return static_cast<int>(1 + s * kParamsPerSlot);
        }
        const float controlsY = 138.0f; const uint32_t base = 1 + fSelectedSlot * kParamsPerSlot;
        if (hitTest(mx, my, 12.0f, controlsY, 100.0f, 38.0f)) return static_cast<int>(base);
        if (hitTest(mx, my, 684.0f, controlsY, 64.0f, 18.0f)) return static_cast<int>(kParamCabEnabled);
        if (hitTest(mx, my, 753.0f, controlsY, 82.0f, 18.0f)) return static_cast<int>(kParamDualMode);
        if (hitTest(mx, my, 753.0f, controlsY + 21.0f, 82.0f, 17.0f)) return static_cast<int>(kParamDualMix);
        if (hitTest(mx, my, 840.0f, controlsY, 108.0f, 17.0f)) return static_cast<int>(kParamInputLevel);
        if (hitTest(mx, my, 840.0f, controlsY + 21.0f, 108.0f, 17.0f)) return static_cast<int>(kParamOutputLevel);
        if (fCabEditMode) {
            for (uint32_t c = 0; c < 4; ++c) {
                float ry = 232.0f + c * 52.0f;
                for (uint32_t pt = 0; pt < 4; ++pt) if (hitTest(mx, my, 310.0f + pt * 155.0f, ry - 4.0f, 145.0f, 32.0f)) return static_cast<int>(kParamCabParams + c * 4 + pt);
            }
            return -1;
        }
        if (fParamValues[kParamTunerActive] <= 0.5f && fSlotMeta[fSelectedSlot].valid) {
            const size_t numParams = std::min<size_t>(fSlotMeta[fSelectedSlot].parameters.size(), 9);
            for (size_t p = 0; p < numParams; ++p) if (hitTest(mx, my, 20.0f, 205.0f + p * 26.0f - 2.0f, 700.0f, 24.0f)) return static_cast<int>(base + 1 + p);
        }
        return -1;
    }
    void setLearnTarget(int targetParam) {
        fLearnTarget = targetParam; setParamAndNotify(kParamMidiLearn, (targetParam >= 0) ? static_cast<float>(targetParam + 1) : 0.0f);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%d", targetParam); setState("midi_learn_target", buf);
    }
    void handleMidiLearnClick(uint32_t paramIdx, bool isRightClick) {
        if (isRightClick && fParamMidiCC[paramIdx] >= 0 && fLearnTarget != static_cast<int>(paramIdx)) {
            fParamMidiCC[paramIdx] = -1; fMidiLearnSelectMode = false; setLearnTarget(-1); syncMidiMapToDSP(); repaint(); return;
        }
        fMidiLearnSelectMode = false; setLearnTarget(static_cast<int>(paramIdx)); repaint();
    }
    void syncMidiMapToDSP() {
        std::ostringstream oss; for (size_t i = 0; i < kControllableParams; ++i) oss << fParamMidiCC[i] << (i + 1 == kControllableParams ? "" : " ");
        setState("midi_cc_map", oss.str().c_str());
    }
    void setParamAndNotify(uint32_t index, float value) {
        if (index < kTotalParameters) fParamValues[index] = value;
        editParameter(index, true);
        setParameterValue(index, value);
        editParameter(index, false);
    }
    static bool pathExists(const std::string& p) {
#ifdef _WIN32
        return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
        struct stat st;
        return stat(p.c_str(), &st) == 0;
#endif
    }
    static std::string getPresetDirectory() {
#ifdef _WIN32
        DWORD a = GetFileAttributesA("H:/Programok/ZoomMultiStompVST/Presets");
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return "H:/Programok/ZoomMultiStompVST/Presets";
        const char* up = std::getenv("USERPROFILE");
        std::string d = up ? (std::string(up) + "/Documents/ZDStompHost") : ".";
        CreateDirectoryA(d.c_str(), nullptr);
        std::string p = d + "/Presets"; CreateDirectoryA(p.c_str(), nullptr);
        return p;
#else
        const char* home = std::getenv("HOME");
        std::string d = home ? (std::string(home) + "/Documents/ZDStompHost") : ".";
        mkdir(d.c_str(), 0755);
        std::string p = d + "/Presets";
        mkdir(p.c_str(), 0755);
        return p;
#endif
    }
    static std::string resolvePortablePath(const std::string& orig, const std::string& rigFile) {
        if (orig.empty() || orig == "EMPTY") return "";
        if (pathExists(orig)) return orig;
        std::string fn = extractFilename(orig);
        size_t sl = rigFile.find_last_of("/\\");
        std::string rDir = (sl == std::string::npos) ? "." : rigFile.substr(0, sl);
        std::string pDir = getPresetDirectory();
        const std::string cands[6] = { rDir + "/" + fn, rDir + "/Effects/" + fn, rDir + "/IRs/" + fn, pDir + "/" + fn, pDir + "/../Effects/" + fn, pDir + "/../IRs/" + fn };
        for (int i = 0; i < 6; ++i) if (pathExists(cands[i])) return cands[i];
        return orig;
    }
    void stepPreset(int dir) {
        std::string path = fCurrentRigPath;
        if (path.empty()) path = getPresetDirectory() + "/dummy.zrig";
        size_t sl = path.find_last_of("/\\");
        std::string d = (sl == std::string::npos) ? getPresetDirectory() : path.substr(0, sl);
        std::vector<std::string> files;
        auto toLowerStr = [](const std::string& s) { std::string r = s; for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return r; };
#ifdef _WIN32
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((d + "/*.zrig").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName); } while (FindNextFileA(h, &fd)); FindClose(h); }
#else
        if (DIR* dirp = opendir(d.c_str())) {
            while (struct dirent* ent = readdir(dirp)) {
                std::string name(ent->d_name);
                if (name.size() >= 5 && toLowerStr(name.substr(name.size() - 5)) == ".zrig") {
                    struct stat st;
                    std::string full = d + "/" + name;
                    if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                        files.push_back(name);
                    }
                }
            }
            closedir(dirp);
        }
#endif
        if (files.empty()) return;
        std::sort(files.begin(), files.end(), [&](const std::string& a, const std::string& b) { return toLowerStr(a) < toLowerStr(b); });
        int cur = -1;
        std::string curLower = toLowerStr(extractFilename(path));
        for (size_t i = 0; i < files.size(); ++i) if (toLowerStr(files[i]) == curLower) { cur = (int)i; break; }
        int nxt = (cur < 0) ? 0 : (cur + dir + (int)files.size()) % (int)files.size();
        loadRigFromFile(d + "/" + files[nxt]);
        setParamAndNotify(kParamPresetPrev, 0.0f);
        setParamAndNotify(kParamPresetNext, 0.0f);
        repaint();
    }
    void openBrowser(bool isSave) {
        std::string d = getPresetDirectory(); FileBrowserOptions o; o.saving = isSave;
        o.title = isSave ? "Save Rig (.zrig)" : "Load Rig (.zrig)"; o.defaultName = "my_rig.zrig"; o.startDir = d.c_str(); openFileBrowser(o);
    }
    void swapSlots(uint32_t a, uint32_t b) {
        std::swap(fSlotPaths[a], fSlotPaths[b]); std::swap(fSlotMeta[a], fSlotMeta[b]);
        char ka[32], kb[32]; std::snprintf(ka, 32, "slot_%u_zdl", a); std::snprintf(kb, 32, "slot_%u_zdl", b);
        setState(ka, fSlotPaths[a].c_str()); setState(kb, fSlotPaths[b].c_str());
        uint32_t ba = 1 + a * kParamsPerSlot, bb = 1 + b * kParamsPerSlot;
        for (uint32_t p = 0; p < kParamsPerSlot; ++p) {
            float va = fParamValues[ba + p], vb = fParamValues[bb + p];
            setParamAndNotify(ba + p, vb); setParamAndNotify(bb + p, va); std::swap(fParamMidiCC[ba + p], fParamMidiCC[bb + p]);
        }
        syncMidiMapToDSP();
    }
    void updateSliderFromMouse(float mx) {
        if (fDraggingParam < 0 || fDraggingParam >= 9) return;
        float r = std::max(0.0f, std::min(1.0f, (mx - 170.0f) / 470.0f));
        setParamAndNotify(1 + fSelectedSlot * kParamsPerSlot + 1 + (uint32_t)fDraggingParam, r * 100.0f); repaint();
    }
    void updateLevelFromMouse(float mx, float bx, float bw, uint32_t idx) {
        float r = std::max(0.0f, std::min(1.0f, (mx - bx) / bw));
        setParamAndNotify(idx, r * 100.0f); repaint();
    }
    void drawButton(float x, float y, float w, float h, const char* lbl, Color bg, bool hl = false) {
        beginPath(); roundedRect(x, y, w, h, 5.0f); fillColor(bg); fill();
        if (hl) { beginPath(); roundedRect(x, y, w, h, 5.0f); strokeColor(Color(255, 175, 35)); strokeWidth(2.5f); stroke(); }
        fontSize(11.0f); fillColor(Color(255, 255, 255)); textAlign(ALIGN_CENTER | ALIGN_MIDDLE); text(x + w * 0.5f, y + h * 0.5f, lbl, nullptr);
    }
    static void formatParamDisplay(uint32_t idx, float val, char* out, size_t sz) {
        float r = std::max(0.0f, std::min(1.0f, val / 100.0f));
        if (idx >= kParamCabParams && idx < kParamCabParams + 16) {
            uint32_t pt = (idx - kParamCabParams) % 4;
            if (pt == 0) { if (r <= 0.01f) std::snprintf(out, sz, "OFF"); else std::snprintf(out, sz, "%+.1fdB", -48.0f + r * 54.0f); return; }
            if (pt == 1) { float p = -100.0f + r * 200.0f; if (std::fabs(p) < 1.0f) std::snprintf(out, sz, "C"); else std::snprintf(out, sz, "%c%.0f%%", p < 0 ? 'L' : 'R', std::fabs(p)); return; }
            if (pt == 2) { std::snprintf(out, sz, "%.0fHz", 20.0f + r * 1980.0f); return; }
            if (pt == 3) { std::snprintf(out, sz, "%.0fHz", 200.0f + r * 19800.0f); return; }
        }
        std::snprintf(out, sz, "%.1f%%", val);
    }
    void drawLevelSlider(float x, float y, float w, float h, const char* lbl, float val, uint32_t idx, int lt) {
        int cc = fParamMidiCC[idx]; char b[32];
        if (cc >= 0) std::snprintf(b, 32, "%s:%d", lbl, cc < 128 ? cc : cc - 128); else std::snprintf(b, 32, "%s", lbl);
        fontSize(9.5f); fillColor(lt == (int)idx ? Color(255, 180, 50) : (cc >= 0 ? Color(130, 230, 155) : Color(190, 200, 210)));
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE); text(x, y + h * 0.5f, b, nullptr);
        float bx = x + 32.0f, bw = w - 34.0f, r = std::max(0.0f, std::min(1.0f, val / 100.0f));
        beginPath(); roundedRect(bx, y + 1.0f, bw, h - 2.0f, 3.0f); fillColor(Color(15, 17, 20)); fill();
        beginPath(); roundedRect(bx, y + 1.0f, bw * r, h - 2.0f, 3.0f); fillColor(fFocusedParam == (int)idx ? Color(235, 145, 45) : Color(55, 145, 215)); fill();
        char vb[32]; formatParamDisplay(idx, val, vb, sizeof(vb));
        fontSize(9.0f); fillColor(Color(255, 255, 255)); textAlign(ALIGN_CENTER | ALIGN_MIDDLE); text(bx + bw * 0.5f, y + h * 0.5f, vb, nullptr);
    }
    void drawFineTuneBar(float w, float h) {
        if (fFocusedParam < 0 || fFocusedParam >= (int)kTotalParameters) return;
        float by = h - 38.0f; uint32_t idx = (uint32_t)fFocusedParam; float val = fParamValues[idx];
        beginPath(); roundedRect(18.0f, by, w - 36.0f, 32.0f, 6.0f); fillColor(Color(28, 38, 48)); fill();
        beginPath(); roundedRect(18.0f, by, w - 36.0f, 32.0f, 6.0f); strokeColor(Color(235, 155, 50)); strokeWidth(1.5f); stroke();
        char vstr[32], title[64]; formatParamDisplay(idx, val, vstr, sizeof(vstr));
        if (idx == kParamInputLevel) std::snprintf(title, 64, "INPUT LEVEL: %s (%.1f%%)", vstr, val);
        else if (idx == kParamOutputLevel) std::snprintf(title, 64, "OUTPUT LEVEL: %s (%.1f%%)", vstr, val);
        else if (idx == kParamDualMix) std::snprintf(title, 64, "DUAL A/B MIX: %s", vstr);
        else if (idx >= kParamCabParams && idx < kParamCabParams + 16) {
            static const char* kN[4] = {"LVL", "PAN", "HP", "LP"}; uint32_t sub = idx - kParamCabParams;
            std::snprintf(title, 64, "IR %c %s: %s (%.1f%%)", 'A' + (sub / 4), kN[sub % 4], vstr, val);
        } else std::snprintf(title, 64, "PARAM: %s", vstr);
        fontSize(11.5f); fillColor(Color(255, 220, 110)); textAlign(ALIGN_LEFT | ALIGN_MIDDLE); text(28.0f, by + 16.0f, title, nullptr);
        float sx = 250.0f, sw = 450.0f, r = std::max(0.0f, std::min(1.0f, val / 100.0f));
        beginPath(); roundedRect(sx, by + 6.0f, sw, 20.0f, 4.0f); fillColor(Color(12, 15, 18)); fill();
        beginPath(); roundedRect(sx, by + 6.0f, sw * r, 20.0f, 4.0f); fillColor(Color(235, 145, 45)); fill();
        fontSize(11.0f); fillColor(Color(255, 255, 255)); textAlign(ALIGN_CENTER | ALIGN_MIDDLE); text(sx + sw * 0.5f, by + 16.0f, vstr, nullptr);
        drawButton(710.0f, by + 4.0f, 38.0f, 24.0f, "-1", Color(55, 70, 85));
        drawButton(752.0f, by + 4.0f, 44.0f, 24.0f, "-0.1", Color(55, 70, 85));
        drawButton(800.0f, by + 4.0f, 44.0f, 24.0f, "+0.1", Color(55, 70, 85));
        drawButton(848.0f, by + 4.0f, 38.0f, 24.0f, "+1", Color(55, 70, 85));
        drawButton(892.0f, by + 4.0f, 44.0f, 24.0f, "RST", Color(110, 65, 45));
    }
    void drawZoomLcd(float x, float y, const ZdlEffectMetadata& m) {
        beginPath(); roundedRect(x - 4.0f, y - 4.0f, 132.0f, 64.0f, 4.0f); fillColor(Color(135, 175, 45)); fill();
        if (!m.valid) { fontSize(12.0f); fillColor(Color(25, 40, 10)); textAlign(ALIGN_CENTER | ALIGN_MIDDLE); text(x + 62.0f, y + 28.0f, "NO EFFECT", nullptr); return; }
        fontSize(10.0f); fillColor(Color(40, 65, 15)); textAlign(ALIGN_CENTER | ALIGN_TOP); text(x + 62.0f, y + 6.0f, "ZD STOMP HOST", nullptr);
        fontSize(15.0f); fillColor(Color(15, 30, 5)); textAlign(ALIGN_CENTER | ALIGN_MIDDLE); text(x + 62.0f, y + 32.0f, m.effectName.c_str(), nullptr);
    }
    void drawQuadCabPanel(float x, float y, float w, float h, int lt) {
        beginPath(); roundedRect(x, y, w, h, 8.0f); fillColor(Color(18, 26, 28)); fill();
        fontSize(14.0f); fillColor(Color(64, 224, 208)); textAlign(ALIGN_LEFT | ALIGN_TOP);
        text(x + 14.0f, y + 10.0f, "MASTER QUAD IR CABINET LOADER (FFT CONVOLUTION - 4 CHANNELS)", nullptr);
        static const char* kC[4] = {"LVL", "PAN", "HP", "LP"};
        for (uint32_t c = 0; c < 4; ++c) {
            float ry = y + 32.0f + c * 52.0f;
            beginPath(); roundedRect(x + 8.0f, ry - 6.0f, w - 16.0f, 44.0f, 6.0f); fillColor(Color(26, 36, 40)); fill();
            char cl[64]; std::snprintf(cl, 64, "IR %c: %s", 'A' + c, fCabIrPaths[c].empty() ? "(Empty)" : extractFilename(fCabIrPaths[c]).c_str());
            fontSize(11.0f); fillColor(fCabIrPaths[c].empty() ? Color(140, 150, 160) : Color(130, 240, 190)); textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
            text(x + 154.0f, ry + 12.0f, cl, nullptr);
            drawButton(30.0f, ry, 86.0f, 24.0f, "LOAD WAV", Color(40, 110, 110));
            drawButton(120.0f, ry, 42.0f, 24.0f, "CLR", Color(115, 50, 50));
            for (uint32_t pt = 0; pt < 4; ++pt) {
                uint32_t pi = kParamCabParams + c * 4 + pt;
                drawLevelSlider(310.0f + pt * 155.0f, ry + 3.0f, 142.0f, 18.0f, kC[pt], fParamValues[pi], pi, lt);
            }
        }
    }
    void uiIdle() override {
        if (fParamValues[kParamTunerActive] > 0.5f) repaint();
    }
    void drawTuner(float x, float y, float w, float h) {
        beginPath(); roundedRect(x, y, w, h, 8.0f); fillColor(Color(16, 18, 22)); fill();
        float cx = x + w * 0.5f, cy = y + h * 0.5f, rawFq = fParamValues[kParamTunerFreq];
        const bool isMuted = (std::round(fParamValues[kParamTunerActive]) == 1.0f);
        fontSize(16.0f); fillColor(isMuted ? Color(215, 155, 45) : Color(80, 220, 120)); textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        text(cx, y + 22.0f, isMuted ? "CHROMATIC TUNER (MUTED - CLICK HERE FOR AUDIO)" : "CHROMATIC TUNER (AUDIO ON - CLICK HERE TO MUTE)", nullptr);

        static float sHeldFreq = 0.0f;
        static float sSmoothCents = 0.0f;
        static int sHoldFrames = 0;
        if (rawFq >= 20.0f) {
            if (sHeldFreq < 20.0f || std::fabs(rawFq - sHeldFreq) / sHeldFreq > 0.08f) sHeldFreq = rawFq;
            else sHeldFreq = sHeldFreq * 0.75f + rawFq * 0.25f;
            sHoldFrames = 120;
        } else if (sHoldFrames > 0) {
            --sHoldFrames;
        } else {
            sHeldFreq = 0.0f;
        }

        const float meterW = std::min(w - 80.0f, 560.0f);
        const float meterX = cx - meterW * 0.5f;
        const float meterY = cy + 18.0f;
        const float meterH = 26.0f;

        beginPath(); roundedRect(meterX, meterY, meterW, meterH, 6.0f); fillColor(Color(26, 30, 36)); fill();
        for (int c = -50; c <= 50; c += 5) {
            float tx = cx + (c / 50.0f) * (meterW * 0.46f);
            float th = (c == 0) ? 36.0f : ((c % 10 == 0) ? 18.0f : 10.0f);
            beginPath(); moveTo(tx, meterY + (meterH - th) * 0.5f); lineTo(tx, meterY + (meterH + th) * 0.5f);
            strokeColor(c == 0 ? Color(100, 240, 130) : Color(90, 100, 115));
            strokeWidth(c == 0 ? 2.5f : 1.5f); stroke();
        }
        fontSize(12.0f); fillColor(Color(140, 150, 165)); textAlign(ALIGN_CENTER | ALIGN_TOP);
        text(cx - meterW * 0.46f, meterY + meterH + 8.0f, "-50c", nullptr);
        text(cx, meterY + meterH + 8.0f, "0c", nullptr);
        text(cx + meterW * 0.46f, meterY + meterH + 8.0f, "+50c", nullptr);

        if (sHeldFreq < 20.0f) {
            fontSize(26.0f); fillColor(Color(110, 118, 130)); textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
            text(cx, cy - 38.0f, "PLAY A STRING...", nullptr);
            return;
        }

        static const char* kN[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        float mf = 69.0f + 12.0f * std::log2(sHeldFreq / 440.0f);
        int mn = static_cast<int>(std::round(mf));
        float cents = std::max(-50.0f, std::min(50.0f, (mf - static_cast<float>(mn)) * 100.0f));
        sSmoothCents = sSmoothCents * 0.72f + cents * 0.28f;

        float alpha = (sHoldFrames < 30) ? (static_cast<float>(sHoldFrames) / 30.0f) : 1.0f;
        bool inTune = (std::fabs(sSmoothCents) <= 3.5f);
        Color indCol = inTune ? Color(70, 240, 115, alpha) : (std::fabs(sSmoothCents) <= 12.0f ? Color(245, 200, 65, alpha) : Color(240, 85, 75, alpha));

        char noteBuf[16]; std::snprintf(noteBuf, sizeof(noteBuf), "%s%d", kN[((mn % 12) + 12) % 12], (mn / 12) - 1);
        fontSize(52.0f); fillColor(indCol); textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        text(cx, cy - 52.0f, noteBuf, nullptr);

        char infoBuf[64]; std::snprintf(infoBuf, sizeof(infoBuf), "%.1f Hz   (%+.1f cents)%s", sHeldFreq, sSmoothCents, inTune ? "  [IN TUNE]" : "");
        fontSize(17.0f); fillColor(Color(210, 220, 230, alpha));
        text(cx, cy - 12.0f, infoBuf, nullptr);

        float needleX = cx + (sSmoothCents / 50.0f) * (meterW * 0.46f);
        beginPath(); roundedRect(needleX - 5.0f, meterY - 7.0f, 10.0f, meterH + 14.0f, 3.0f);
        fillColor(indCol); fill();
    }
    void clearAllSlots() {
        fParamMidiCC.fill(-1); syncMidiMapToDSP();
        for (uint32_t s = 0; s < kMaxSlots; ++s) {
            fSlotPaths[s].clear(); fSlotMeta[s] = ZdlEffectMetadata{};
            char kb[32]; std::snprintf(kb, 32, "slot_%u_zdl", s); setState(kb, "");
            uint32_t b = 1 + s * kParamsPerSlot; setParamAndNotify(b, 0.0f);
            for (uint32_t p = 1; p < kParamsPerSlot; ++p) setParamAndNotify(b + p, 50.0f);
        }
        fSelectedSlot = 0; fCurrentRigName.clear(); fCurrentRigPath.clear();
    }
    void saveRigToFile(const std::string& path) {
        std::ofstream f(path, std::ios::out | std::ios::trunc); if (!f.is_open()) { fSaveStatus = -1; return; }
        f << "ZOOM_RIG_PRESET_V1\n" << fParamValues[0] << " " << fParamValues[kParamInputLevel] << " " << fParamValues[kParamOutputLevel] << " " << fParamValues[kParamDualMode] << " " << fParamValues[kParamDualMix] << " " << fParamValues[kParamCabEnabled] << "\n";
        for (uint32_t s = 0; s < kMaxSlots; ++s) {
            f << (fSlotPaths[s].empty() ? "EMPTY" : fSlotPaths[s]) << "\n";
            uint32_t b = 1 + s * kParamsPerSlot;
            for (uint32_t p = 0; p < kParamsPerSlot; ++p) f << fParamValues[b + p] << (p + 1 == kParamsPerSlot ? "\n" : " ");
        }
        for (size_t i = 0; i < kControllableParams; ++i) f << fParamMidiCC[i] << (i + 1 == kControllableParams ? "" : " ");
        f << "\n";
        for (uint32_t c = 0; c < 4; ++c) {
            f << (fCabIrPaths[c].empty() ? "EMPTY" : fCabIrPaths[c]) << "\n";
            for (uint32_t pt = 0; pt < 4; ++pt) f << fParamValues[kParamCabParams + c * 4 + pt] << (pt == 3 ? "\n" : " ");
        }
        f.close(); fCurrentRigName = extractFilename(path); fCurrentRigPath = path; fSaveStatus = 1;
    }
    void loadRigFromFile(const std::string& path) {
        std::ifstream f(path); if (!f.is_open()) { fLoadStatus = -1; return; }
        std::string ln; if (!std::getline(f, ln)) { fLoadStatus = -1; return; }
        trimCrLf(ln); if (ln != "ZOOM_RIG_PRESET_V1" || !std::getline(f, ln)) { fLoadStatus = -1; return; }
        float bp=0, il=25, ol=25, dm=0, dx=50, ce=1;
        { std::istringstream g(ln); g >> bp >> il >> ol; if (!(g >> dm)) dm=0; if (!(g >> dx)) dx=50; if (!(g >> ce)) ce=1; }
        std::array<std::string, kMaxSlots> newSlotPaths;
        std::array<std::array<float, kParamsPerSlot>, kMaxSlots> newSlotParams;
        for (auto& arr : newSlotParams) arr.fill(55.0f);
        for (uint32_t s = 0; s < kMaxSlots; ++s) {
            std::string sp; if (!std::getline(f, sp)) break;
            trimCrLf(sp); newSlotPaths[s] = resolvePortablePath(sp, path);
            if (!std::getline(f, ln)) break;
            std::istringstream ps(ln);
            for (uint32_t p = 0; p < kParamsPerSlot; ++p) { float v = (p == 0) ? 0.0f : 55.0f; if (ps >> v) newSlotParams[s][p] = v; }
        }
        std::array<std::string, 4> newCabPaths;
        std::array<std::array<float, 4>, 4> newCabParams;
        for (auto& arr : newCabParams) arr.fill(0.0f);
        if (std::getline(f, ln)) {
            std::istringstream ms(ln); for (size_t i = 0; i < kControllableParams; ++i) ms >> fParamMidiCC[i]; syncMidiMapToDSP();
            for (uint32_t c = 0; c < 4; ++c) {
                std::string cp; if (!std::getline(f, cp)) break;
                trimCrLf(cp); newCabPaths[c] = resolvePortablePath(cp, path);
                if (std::getline(f, ln)) { std::istringstream cs(ln); for (uint32_t pt = 0; pt < 4; ++pt) { float v=0; if (cs >> v) newCabParams[c][pt] = v; } }
            }
        }
        setParamAndNotify(0, bp);
        setParamAndNotify(kParamInputLevel, (std::fabs(il - 50.0f) < 0.01f || il <= 0.0f) ? 25.0f : il);
        setParamAndNotify(kParamOutputLevel, (std::fabs(ol - 50.0f) < 0.01f || ol <= 0.0f) ? 25.0f : ol);
        setParamAndNotify(kParamDualMode, dm);
        setParamAndNotify(kParamDualMix, dx);
        setParamAndNotify(kParamCabEnabled, ce);
        for (uint32_t s = 0; s < kMaxSlots; ++s) {
            uint32_t b = 1 + s * kParamsPerSlot;
            for (uint32_t p = 0; p < kParamsPerSlot; ++p) setParamAndNotify(b + p, newSlotParams[s][p]);
            fSlotPaths[s] = newSlotPaths[s];
            char kb[32]; std::snprintf(kb, 32, "slot_%u_zdl", s);
            if (newSlotPaths[s].empty()) { fSlotMeta[s] = ZdlEffectMetadata{}; setState(kb, ""); }
            else { fSlotMeta[s] = ZdlParser::parseFile(newSlotPaths[s]); setState(kb, newSlotPaths[s].c_str()); }
            setParamAndNotify(b, newSlotParams[s][0]);
        }
        for (uint32_t c = 0; c < 4; ++c) {
            if (!newCabPaths[c].empty() && newCabParams[c][0] <= 0.01f && newCabParams[c][3] <= 0.01f) {
                newCabParams[c][0] = 88.2f; newCabParams[c][1] = 50.0f; newCabParams[c][2] = 0.0f; newCabParams[c][3] = 100.0f;
            }
            for (uint32_t pt = 0; pt < 4; ++pt) setParamAndNotify(kParamCabParams + c * 4 + pt, newCabParams[c][pt]);
            fCabIrPaths[c] = newCabPaths[c];
            char kb[32]; std::snprintf(kb, 32, "cab_ir_%u_wav", c);
            setState(kb, newCabPaths[c].c_str());
        }
        for (uint32_t s = 0; s < kMaxSlots; ++s) {
            uint32_t b = 1 + s * kParamsPerSlot;
            setParamAndNotify(b, fParamValues[b]);
        }
        setParamAndNotify(0, fParamValues[0]);
        setParamAndNotify(kParamInputLevel, (std::fabs(il - 50.0f) < 0.01f || il <= 0.0f) ? 25.0f : il);
        setParamAndNotify(kParamOutputLevel, (std::fabs(ol - 50.0f) < 0.01f || ol <= 0.0f) ? 25.0f : ol);
                fCurrentRigName = extractFilename(path); fCurrentRigPath = path; fLoadStatus = 1;
    }
    uint32_t fSelectedSlot, fPendingBrowserSlot; BrowserAction fBrowserAction;
    int fDraggingParam, fDraggingLevel; FontId fFontId; int fSaveStatus, fLoadStatus;
    bool fClearAfterSave, fMidiLearnSelectMode; int fLearnTarget, fLastMonitoredId, fLastMonitoredVal;
    std::string fCurrentRigName;
    std::string fCurrentRigPath;
    std::array<float, kTotalParameters> fParamValues;
    std::array<int, kControllableParams> fParamMidiCC;
    std::array<int, 256> fLastCCValueUI;
    std::array<std::string, kMaxSlots> fSlotPaths;
    std::array<ZdlEffectMetadata, kMaxSlots> fSlotMeta;
    bool fCabEditMode; int fFocusedParam; uint32_t fPendingCabIrSlot; std::array<std::string, 4> fCabIrPaths;
    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZoomMultiStompUI)
};
UI* createUI() { return new ZoomMultiStompUI(); }
END_NAMESPACE_DISTRHO
