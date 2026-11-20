#pragma once
#include "Core/Planner.h"
#include "PluginProcessor.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace ui {
struct Look;
class SpectrumView;
class StripPanel;
class SuggestPanel;
class HelpPanel;
} // namespace ui

// One row of what the shared window knows about a participating track.
struct TrackView {
    int slot = -1;
    uint64_t uid = 0;
    juce::String name;
    int role = 0, layer = 0, section = -1;
    float priority = 0.4f, amount = 1, movement = 1;
    bool locked = false, mine = false;
    std::array<float, clr::kBands> live {}, cap {}, capPeak {};
    std::array<clr::EqBandData, clr::kEqBands> plan {};
    clr::TrimPlan trim {};
    clr::HarmPlan harm {};
    float lufsI = -120, tpMax = -120; // master: from the last listen
    int mode = 0;          // 1 = master (stereo out)
    float trimParam = 0;
    float activeSec = 0, rmsDb = -120.0f, capRmsDb = -120.0f;
};

struct Suggestion {
    clr::Change change;
    int slot = -1;
    bool skip = false;
};

class ClearingEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit ClearingEditor(ClearingProcessor&);
    ~ClearingEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

    // Shared state read by the child views.
    std::vector<TrackView> tracks;
    std::vector<Suggestion> suggestions;
    std::vector<juce::String> notes;
    std::vector<juce::String> sectionNames;
    std::vector<uint64_t> sectionLeads;
    int selectedSlot = -1, hoverSuggestion = -1;
    bool listening = false;

    const TrackView* find(int slot) const;
    const TrackView* findUid(uint64_t uid) const;
    juce::String sectionsText(uint32_t mask) const;
    void select(int slot);
    void writeCtrl(int slot, const juce::String& name, int role, float prio, bool locked,
                   int layer = -1, float amount = -1, float movement = -1);

    // Also driven directly by the snapshot tool.
    void refreshNow();
    void refreshLlm();
    void startListening();
    void stopListening();
    void applySuggestions();
    void undoApply();
    void showHelp(bool show);
    void showHelpPage(const juce::String& key);

    ClearingProcessor& proc;

private:
    void timerCallback() override { refreshNow(); }
    void runPlan();
    void updateButtons();
    void setAllTracks(float amount, float movement);

    struct Snapshot { int slot; clr::EqBandData bands[clr::kEqBands]; };
    std::vector<std::vector<Snapshot>> undoStack;

    std::unique_ptr<ui::Look> look;
    juce::TooltipWindow tooltips { this, 500 };
    juce::TextButton listenBtn { "LISTEN" }, applyBtn { "APPLY" }, undoBtn { "UNDO" }, helpBtn { "?  HELP" }, llmBtn { "WORK WITH CLAUDE" };
    juce::ToggleButton compareBtn { "COMPARE" }, boldBtn { "BOLDER" };
    juce::ComboBox styleBox, groupBox, modeBox;
    juce::Slider amountSlider, movementSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> styleAtt, groupAtt, modeAtt;
    std::unique_ptr<ui::SpectrumView> spectrum;
    std::unique_ptr<ui::StripPanel> strips;
    std::unique_ptr<ui::SuggestPanel> suggest;
    std::unique_ptr<ui::HelpPanel> help;
    juce::String sectionName, sectionFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClearingEditor)
};
