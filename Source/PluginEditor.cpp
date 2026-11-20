#include "PluginEditor.h"
#include <BinaryData.h>
#include <algorithm>
#include <cmath>
#include <functional>

using namespace clr;
using juce::Colour;
using juce::Graphics;
using juce::Justification;
using juce::Rectangle;

namespace ui {

namespace Pal {
const Colour bg { 0xff0a0d12 }, panel { 0xff10151c }, panel2 { 0xff161c25 }, line { 0xff242c38 },
    text { 0xffe7edf5 }, dim { 0xff7e8b9e }, faint { 0xff4a5566 }, accent { 0xff35e0c0 },
    warn { 0xffffb454 }, red { 0xffff5d6c };
}

static juce::Font F(float h, int style = 1)
{
    static juce::Typeface::Ptr med = juce::Typeface::createSystemTypefaceFor(BinaryData::RajdhaniMedium_ttf, BinaryData::RajdhaniMedium_ttfSize);
    static juce::Typeface::Ptr semi = juce::Typeface::createSystemTypefaceFor(BinaryData::RajdhaniSemiBold_ttf, BinaryData::RajdhaniSemiBold_ttfSize);
    static juce::Typeface::Ptr bold = juce::Typeface::createSystemTypefaceFor(BinaryData::RajdhaniBold_ttf, BinaryData::RajdhaniBold_ttfSize);
    static juce::Typeface::Ptr mono = juce::Typeface::createSystemTypefaceFor(BinaryData::ShareTechMonoRegular_ttf, BinaryData::ShareTechMonoRegular_ttfSize);
    auto& t = style == 0 ? med : style == 1 ? semi : style == 2 ? bold : mono;
    return juce::Font(juce::FontOptions(t).withHeight(h));
}

static Colour roleColour(int role)
{
    static const float hue[] = { 0.58f, 0.00f, 0.08f, 0.14f, 0.50f, 0.45f, 0.20f, 0.95f, 0.72f, 0.78f, 0.33f,
                                 0.52f, 0.60f, 0.64f, 0.68f, 0.74f, 0.28f, 0.11f, 0.05f, 0.02f, 0.40f, 0.16f, 0.88f, 0.55f };
    const float h = (role >= 0 && role < RoleCount) ? hue[role] : 0.58f;
    return Colour::fromHSV(h, role == 0 ? 0.25f : 0.62f, 0.95f, 1.0f);
}

static juce::String fmtHz(float f)
{
    return f >= 1000.0f ? juce::String(f / 1000.0f, f >= 10000.0f ? 0 : 1) + " kHz" : juce::String((int) std::round(f)) + " Hz";
}

struct Look : juce::LookAndFeel_V4 {
    Look()
    {
        setColour(juce::PopupMenu::backgroundColourId, Pal::panel2);
        setColour(juce::PopupMenu::textColourId, Pal::text);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, Pal::accent.withAlpha(0.25f));
        setColour(juce::PopupMenu::highlightedTextColourId, Pal::text);
        setColour(juce::ComboBox::textColourId, Pal::text);
        setColour(juce::Label::textColourId, Pal::text);
    }
    juce::Font getPopupMenuFont() override { return F(15, 1); }
    juce::Font getComboBoxFont(juce::ComboBox&) override { return F(14, 1); }
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return F(15, 2); }

    void drawButtonBackground(Graphics& g, juce::Button& b, const Colour&, bool hover, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced(0.5f);
        const bool primary = b.getToggleState() || b.getProperties()["primary"];
        Colour fill = primary ? Pal::accent : Pal::panel2;
        if (!b.isEnabled()) fill = Pal::panel;
        if (b.getProperties()["recording"]) fill = Pal::red;
        if (hover && b.isEnabled()) fill = fill.brighter(0.12f);
        if (down) fill = fill.darker(0.15f);
        g.setColour(fill);
        g.fillRoundedRectangle(r, 7.0f);
        g.setColour(primary || b.getProperties()["recording"] ? fill.brighter(0.2f) : Pal::line);
        g.drawRoundedRectangle(r, 7.0f, 1.0f);
    }
    void drawButtonText(Graphics& g, juce::TextButton& b, bool, bool) override
    {
        const bool onFill = b.getProperties()["primary"] || b.getProperties()["recording"];
        g.setFont(F(15, 2));
        g.setColour(!b.isEnabled() ? Pal::faint : onFill ? Pal::bg : Pal::text);
        g.drawText(b.getButtonText(), b.getLocalBounds(), Justification::centred);
    }
    void drawToggleButton(Graphics& g, juce::ToggleButton& b, bool hover, bool) override
    {
        auto r = b.getLocalBounds().toFloat().reduced(0.5f);
        const bool on = b.getToggleState();
        g.setColour(on ? Pal::accent.withAlpha(0.18f) : hover ? Pal::panel2.brighter(0.1f) : Pal::panel2);
        g.fillRoundedRectangle(r, 7.0f);
        g.setColour(on ? Pal::accent : Pal::line);
        g.drawRoundedRectangle(r, 7.0f, 1.0f);
        g.setFont(F(14, 2));
        g.setColour(on ? Pal::accent : Pal::dim);
        g.drawText(b.getButtonText(), b.getLocalBounds(), Justification::centred);
    }
    void drawComboBox(Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = Rectangle<float>(0, 0, (float) w, (float) h).reduced(0.5f);
        g.setColour(Pal::panel2); g.fillRoundedRectangle(r, 6.0f);
        g.setColour(box.hasKeyboardFocus(true) ? Pal::accent : Pal::line); g.drawRoundedRectangle(r, 6.0f, 1.0f);
        juce::Path p; const float cx = (float) w - 14.0f, cy = (float) h * 0.5f;
        p.addTriangle(cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
        g.setColour(Pal::dim); g.fillPath(p);
    }
    void positionComboBoxText(juce::ComboBox& box, juce::Label& l) override { l.setBounds(8, 1, box.getWidth() - 26, box.getHeight() - 2); l.setFont(F(14, 1)); }
    void drawLinearSlider(Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle, juce::Slider& s) override
    {
        const float cy = (float) y + (float) h * 0.5f;
        g.setColour(Pal::line); g.fillRoundedRectangle((float) x, cy - 2, (float) w, 4, 2);
        g.setColour(s.findColour(juce::Slider::trackColourId)); g.fillRoundedRectangle((float) x, cy - 2, pos - (float) x, 4, 2);
        g.setColour(Pal::text); g.fillEllipse(pos - 5, cy - 5, 10, 10);
    }
};

// ---------------------------------------------------------------------------------------------
class SpectrumView : public juce::Component {
public:
    explicit SpectrumView(ClearingEditor& e) : ed(e) { setInterceptsMouseClicks(false, false); }

    void paint(Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(Pal::panel); g.fillRoundedRectangle(b, 10.0f);
        g.setColour(Pal::line); g.drawRoundedRectangle(b.reduced(0.5f), 10.0f, 1.0f);

        plot = Rectangle<float>(50.0f, 34.0f, b.getWidth() - 68.0f, b.getHeight() - 62.0f);
        g.setColour(Pal::dim); g.setFont(F(13, 2));
        g.drawText("SPECTRUM", 18, 8, 200, 18, Justification::left);
        g.setColour(Pal::faint); g.setFont(F(12, 3));
        g.drawText("captured while listening, absolute level, before fader", 110, 8, 420, 18, Justification::left);

        // grid
        g.setFont(F(11, 3));
        for (float f : { 30.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f, 20000.f }) {
            const float x = fx(f);
            g.setColour(Pal::line.withAlpha(0.7f)); g.drawVerticalLine((int) x, plot.getY(), plot.getBottom());
            g.setColour(Pal::faint);
            g.drawText(f >= 1000 ? juce::String((int) (f / 1000)) + "k" : juce::String((int) f), (int) x - 20, (int) plot.getBottom() + 4, 40, 14, Justification::centred);
        }
        for (float d = -100; d <= -20; d += 20) {
            const float y = dy(d);
            g.setColour(Pal::line.withAlpha(0.7f)); g.drawHorizontalLine((int) y, plot.getX(), plot.getRight());
            g.setColour(Pal::faint); g.drawText(juce::String((int) d), 12, (int) y - 7, 32, 14, Justification::right);
        }

        if (ed.tracks.empty()) {
            g.setColour(Pal::dim); g.setFont(F(18, 1));
            g.drawFittedText("No tracks yet.\nInsert CLEARING on each track you want included, after its tone-shaping effects.",
                             plot.toNearestInt().reduced(80, 60), Justification::centred, 3);
            return;
        }

        // Draw unselected first, selected on top.
        std::vector<const TrackView*> order;
        for (auto& t : ed.tracks) order.push_back(&t);
        std::stable_sort(order.begin(), order.end(), [&](auto* a, auto* c) { return (a->slot == ed.selectedSlot) < (c->slot == ed.selectedSlot); });
        const bool anySel = ed.selectedSlot >= 0;

        for (auto* t : order) {
            const bool sel = t->slot == ed.selectedSlot;
            const float a = !anySel || sel ? 1.0f : 0.35f;
            const auto col = roleColour(t->role);
            const bool hasCap = t->activeSec > 0.0f;
            const auto& data = hasCap ? t->cap : t->live;

            juce::Path line = curve(data, nullptr);
            juce::Path fill(line);
            fill.lineTo(plot.getRight(), plot.getBottom()); fill.lineTo(plot.getX(), plot.getBottom()); fill.closeSubPath();
            g.setGradientFill(juce::ColourGradient::vertical(col.withAlpha(0.28f * a), plot.getY(), col.withAlpha(0.0f), plot.getBottom()));
            g.fillPath(fill);
            g.setColour(col.withAlpha((hasCap ? 0.95f : 0.5f) * a));
            g.strokePath(line, juce::PathStrokeType(sel ? 2.4f : 1.5f));

            // Predicted result: capture plus the accepted-to-be EQ, drawn dashed.
            std::vector<const Suggestion*> mine;
            for (auto& s : ed.suggestions) if (s.slot == t->slot && !s.skip) mine.push_back(&s);
            if (hasCap && !mine.empty()) {
                juce::Path after = curve(data, &mine);
                juce::Path dashed; const float dl[] = { 5.0f, 4.0f };
                juce::PathStrokeType(1.6f).createDashedStroke(dashed, after, dl, 2);
                g.setColour(Pal::text.withAlpha(0.9f * a)); g.fillPath(dashed);
            }
        }

        // Markers for each suggestion.
        for (size_t i = 0; i < ed.suggestions.size(); ++i) {
            auto& s = ed.suggestions[i];
            if (s.skip) continue;
            auto* t = ed.find(s.slot);
            if (!t) continue;
            const auto col = roleColour(t->role);
            const float x = fx(s.change.freq);
            const bool hov = (int) i == ed.hoverSuggestion;
            g.setColour(col.withAlpha(hov ? 0.9f : 0.45f));
            for (float y = plot.getY() + 22; y < plot.getBottom(); y += 7) g.drawVerticalLine((int) x, y, y + 3);
            auto pill = Rectangle<float>(x - 26, plot.getY() + 2, 52, 18);
            g.setColour(Pal::bg); g.fillRoundedRectangle(pill, 9);
            g.setColour(col.withAlpha(hov ? 1.0f : 0.8f)); g.drawRoundedRectangle(pill, 9, hov ? 2.0f : 1.2f);
            g.setFont(F(12, 3)); g.setColour(Pal::text);
            g.drawText((s.change.dynamic ? "~" : "") + (s.change.gainDb > 0 ? juce::String("+") : juce::String()) + juce::String(s.change.gainDb, 1), pill.toNearestInt(), Justification::centred);
        }
    }

private:
    float fx(float f) const { return plot.getX() + plot.getWidth() * std::log(f / 20.0f) / std::log(1000.0f); }
    float dy(float db) const { return plot.getY() + plot.getHeight() * (1.0f - (db + 100.0f) / 80.0f); }

    juce::Path curve(const std::array<float, kBands>& d, const std::vector<const Suggestion*>* eq) const
    {
        std::vector<juce::Point<float>> pts;
        auto val = [&](int b) {
            float v = std::fmax(-100.0f, d[(size_t) b]);
            if (eq) for (auto* s : *eq) {
                const auto c = makeCoeffs((BandType) s->change.type, 48000.0, s->change.freq, s->change.gainDb, s->change.q);
                v += (float) magnitudeDb(c, 48000.0, bandCentre(b));
            }
            return std::fmin(-20.0f, std::fmax(-100.0f, v));
        };
        pts.push_back({ plot.getX(), dy(val(0)) });
        for (int b = 0; b < kBands; ++b) pts.push_back({ fx(bandCentre(b)), dy(val(b)) });
        pts.push_back({ plot.getRight(), dy(val(kBands - 1)) });
        juce::Path p; p.startNewSubPath(pts[0]);
        for (size_t i = 0; i + 1 < pts.size(); ++i) { // Catmull-Rom to cubic Bezier
            auto p0 = pts[i > 0 ? i - 1 : 0], p1 = pts[i], p2 = pts[i + 1], p3 = pts[i + 2 < pts.size() ? i + 2 : i + 1];
            p.cubicTo(p1 + (p2 - p0) / 6.0f, p2 - (p3 - p1) / 6.0f, p2);
        }
        return p;
    }

    ClearingEditor& ed;
    Rectangle<float> plot;
};

// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
class StripRow : public juce::Component {
public:
    StripRow(ClearingEditor& e, int s) : ed(e), slot(s)
    {
        nameLabel.setFont(F(17, 2)); nameLabel.setEditable(false, true, true);
        nameLabel.setColour(juce::Label::textColourId, Pal::text);
        nameLabel.setTooltip("Double-click to rename. Leave empty to use Logic's track name.");
        nameLabel.onTextChange = [this] { push(); lockout(); };
        addAndMakeVisible(nameLabel);

        for (int i = 0; i < RoleCount; ++i) {
            if (i == 0) roleBox.addSectionHeading("BAND");
            if (i == RoleSolo) roleBox.addSectionHeading("ORCHESTRA");
            roleBox.addItem(roleName(i), i + 1);
        }
        roleBox.setTooltip("What this part is. Sets a starting priority.");
        roleBox.onChange = [this] {
            const int r = roleBox.getSelectedId() - 1;
            prio.setValue(roleDefaultPriority(r), juce::dontSendNotification);
            push(); lockout();
        };
        addAndMakeVisible(roleBox);

        layerBox.addItem("-", 1);
        for (int i = 1; i <= 8; ++i) layerBox.addItem("L" + juce::String(i), i + 1);
        layerBox.setTooltip("Layer: give tracks that make one sound together the same layer. They're judged and corrected as one instrument.");
        layerBox.onChange = [this] { push(true); lockout(); };
        addAndMakeVisible(layerBox);

        prio.setSliderStyle(juce::Slider::LinearHorizontal); prio.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        prio.setRange(0.0, 1.0, 0.01); prio.setColour(juce::Slider::trackColourId, Pal::accent);
        prio.onValueChange = [this] { push(); lockout(); };
        prio.setTooltip("Priority: how much this part needs to be heard. Lower-priority parts give way to higher ones.");
        addAndMakeVisible(prio);

        lockBtn.setClickingTogglesState(true);
        lockBtn.onClick = [this] { push(); lockout(); };
        lockBtn.setTooltip("Lock: never change this track's tone");
        addAndMakeVisible(lockBtn);
    }

    void update(const TrackView& t)
    {
        mine = t.mine; role = t.role; level = t.rmsDb; locked = t.locked; layer = t.layer; master = t.mode == 1; lufsI = t.lufsI; tpMax = t.tpMax;
        if (juce::Time::getMillisecondCounter() > lockUntil) {
            if (!nameLabel.isBeingEdited()) nameLabel.setText(t.name, juce::dontSendNotification);
            roleBox.setSelectedId(t.role + 1, juce::dontSendNotification);
            layerBox.setSelectedId(t.layer + 1, juce::dontSendNotification);
            prio.setValue(t.priority, juce::dontSendNotification);
            lockBtn.setToggleState(t.locked, juce::dontSendNotification);
        }
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(10, 6);
        auto top = r.removeFromTop(22);
        top.removeFromLeft(18);
        lockBtn.setBounds(top.removeFromRight(46));
        nameLabel.setBounds(top);
        r.removeFromTop(4);
        auto row = r.removeFromTop(22);
        roleBox.setBounds(row.removeFromLeft(120));
        row.removeFromLeft(6);
        layerBox.setBounds(row.removeFromLeft(54));
        row.removeFromLeft(8);
        prio.setBounds(row.removeFromLeft(row.getWidth() - 4));
    }

    void paint(Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced(2.0f);
        const bool sel = ed.selectedSlot == slot;
        g.setColour(sel ? Pal::panel2.brighter(0.05f) : Pal::panel2); g.fillRoundedRectangle(r, 8);
        g.setColour(sel ? roleColour(role) : Pal::line); g.drawRoundedRectangle(r, 8, sel ? 1.6f : 1.0f);
        g.setColour(roleColour(role)); g.fillEllipse(r.getX() + 11, r.getY() + 12, 10, 10);
        if (layer > 0) { // layer mates share a bracket colour on the left edge
            g.setColour(Colour::fromHSV((float) layer * 0.13f, 0.5f, 0.95f, 1.0f));
            g.fillRoundedRectangle(r.getX() + 1, r.getY() + 8, 3, r.getHeight() - 16, 1.5f);
        }
        const float m = juce::jlimit(0.0f, 1.0f, (level + 60.0f) / 60.0f);
        g.setColour(Pal::line); g.fillRoundedRectangle(r.getX() + 10, r.getBottom() - 6, r.getWidth() - 20, 2.5f, 1.2f);
        g.setColour(roleColour(role).withAlpha(0.9f)); g.fillRoundedRectangle(r.getX() + 10, r.getBottom() - 6, (r.getWidth() - 20) * m, 2.5f, 1.2f);
        if (master) {
            g.setFont(F(11, 3)); g.setColour(Pal::accent);
            juce::String tag = "MASTER OUT";
            if (lufsI > -100) tag << "   " << juce::String(lufsI, 1) << " LUFS  TP " << juce::String(tpMax, 1);
            g.setColour(tpMax > -1.0f ? Pal::warn : Pal::accent);
            g.drawText(tag, getLocalBounds().withTrimmedRight(66).withHeight(30).withTrimmedTop(6).withTrimmedRight(mine ? 80 : 0), Justification::centredRight);
        }
        if (mine) {
            g.setFont(F(11, 3)); g.setColour(Pal::dim);
            g.drawText("THIS TRACK", getLocalBounds().withTrimmedRight(66).withHeight(30).withTrimmedTop(6), Justification::centredRight);
        }
    }
    void mouseDown(const juce::MouseEvent&) override { ed.select(slot); }

    int slot;
private:
    void lockout() { lockUntil = juce::Time::getMillisecondCounter() + 700; }
    void push(bool withLayer = false)
    {
        ed.writeCtrl(slot, nameLabel.getText(), roleBox.getSelectedId() - 1, (float) prio.getValue(), lockBtn.getToggleState(),
                     withLayer ? layerBox.getSelectedId() - 1 : -1);
    }

    ClearingEditor& ed;
    juce::Label nameLabel;
    juce::ComboBox roleBox, layerBox;
    juce::Slider prio;
    juce::TextButton lockBtn { "LOCK" };
    int role = 0, layer = 0; float level = -120, lufsI = -120, tpMax = -120; bool mine = false, locked = false, master = false;
    juce::uint32 lockUntil = 0;
};

class StripPanel : public juce::Component {
public:
    explicit StripPanel(ClearingEditor& e) : ed(e)
    {
        view.setViewedComponent(&content, false);
        view.setScrollBarsShown(true, false);
        view.setScrollBarThickness(6);
        addAndMakeVisible(view);
    }
    void update()
    {
        bool rebuild = rows.size() != ed.tracks.size();
        for (size_t i = 0; !rebuild && i < rows.size(); ++i) rebuild = rows[i]->slot != ed.tracks[i].slot;
        if (rebuild) {
            rows.clear(); content.removeAllChildren();
            for (auto& t : ed.tracks) { rows.push_back(std::make_unique<StripRow>(ed, t.slot)); content.addAndMakeVisible(*rows.back()); }
            resized();
        }
        for (size_t i = 0; i < rows.size(); ++i) rows[i]->update(ed.tracks[i]);
    }
    void resized() override
    {
        view.setBounds(getLocalBounds().withTrimmedTop(30).reduced(6, 0).withTrimmedBottom(6));
        const int h = 64, w = view.getWidth() - (rows.size() * 64 > (size_t) view.getHeight() ? 8 : 0);
        content.setSize(w, (int) rows.size() * h);
        for (size_t i = 0; i < rows.size(); ++i) rows[i]->setBounds(0, (int) i * h, w, h);
    }
    void paint(Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(Pal::panel); g.fillRoundedRectangle(b, 10);
        g.setColour(Pal::line); g.drawRoundedRectangle(b.reduced(0.5f), 10, 1);
        g.setColour(Pal::dim); g.setFont(F(13, 2));
        g.drawText("TRACKS", 18, 8, 120, 18, Justification::left);
        g.setColour(Pal::faint); g.setFont(F(12, 3));
        g.drawText("ROLE  /  LAYER  /  PRIORITY      " + juce::String(ed.tracks.size()) + " in group", 90, 8, getWidth() - 106, 18, Justification::right);
    }
private:
    ClearingEditor& ed;
    juce::Viewport view;
    juce::Component content;
    std::vector<std::unique_ptr<StripRow>> rows;
};

// ---------------------------------------------------------------------------------------------
// Suggestions, preceded by the planner's notes (layer report etc.), which have no checkbox.
class SuggestPanel : public juce::Component {
public:
    explicit SuggestPanel(ClearingEditor& e) : ed(e) {}
    static constexpr int rowH = 46;

    int total() const { return (int) (ed.notes.size() + ed.suggestions.size()); }

    void paint(Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(Pal::panel); g.fillRoundedRectangle(b, 10);
        g.setColour(Pal::line); g.drawRoundedRectangle(b.reduced(0.5f), 10, 1);
        g.setColour(Pal::dim); g.setFont(F(13, 2));
        g.drawText("SUGGESTED CHANGES", 18, 8, 200, 18, Justification::left);

        if (total() == 0 && !ed.listening) {
            // What's applied on every track right now (from this window, another one, or Claude).
            std::vector<std::pair<const TrackView*, const EqBandData*>> rows;
            static EqBandData trimMark[kSlots][kSections + 1]; // trims shown as rows: type -1, gain = dB, q = section (-1 = whole song)
            for (auto& t : ed.tracks) {
                for (auto& b : t.plan) if (b.enabled) rows.push_back({ &t, &b });
                if (t.slot < 0 || t.slot >= kSlots) continue;
                auto addTrim = [&](float db, int sec) {
                    auto& m = trimMark[t.slot][sec + 1]; m = EqBandData {}; m.type = -1; m.gainDb = db; m.q = (float) sec; m.enabled = 1;
                    rows.push_back({ &t, &m });
                };
                if (std::fabs(t.trim.baseDb) > 0.01f) addTrim(t.trim.baseDb, -1);
                static EqBandData harmMark[kSlots][2]; // harmonics as rows: type -2 bass (freq lo, q hi), -3 presence (freq)
                if (t.harm.bassOn > 0) { auto& m = harmMark[t.slot][0]; m = EqBandData {}; m.type = -2; m.freq = t.harm.bassLo; m.q = t.harm.bassHi; m.gainDb = t.harm.bassMixDb; m.enabled = 1; rows.push_back({ &t, &m }); }
                if (t.harm.presOn > 0) { auto& m = harmMark[t.slot][1]; m = EqBandData {}; m.type = -3; m.freq = t.harm.presFreq; m.gainDb = t.harm.presMixDb; m.enabled = 1; rows.push_back({ &t, &m }); }
                for (int k = 0; k < kSections; ++k) if (std::fabs(t.trim.sectionDb[k]) > 0.01f) addTrim(t.trim.sectionDb[k], k);
            }
            if (!rows.empty()) {
                g.setColour(Pal::faint); g.setFont(F(12, 3));
                g.drawText("APPLIED NOW   " + juce::String((int) rows.size()) + " bands", getWidth() - 320, 8, 302, 18, Justification::right);
                const int cols = 2, rh = 22;
                const int perCol = std::max(1, (getHeight() - 40) / rh);
                const int colW = (getWidth() - 30) / cols;
                for (int i = 0; i < (int) rows.size() && i < perCol * cols; ++i) {
                    const auto* t = rows[(size_t) i].first; const auto& b = *rows[(size_t) i].second;
                    const int x = 14 + (i / perCol) * colW, y = 32 + (i % perCol) * rh;
                    g.setColour(roleColour(t->role)); g.fillEllipse((float) x, (float) y + 7, 8, 8);
                    g.setColour(Pal::text); g.setFont(F(14, 2));
                    g.drawText(t->name, x + 14, y, 150, rh, Justification::centredLeft);
                    g.setColour(b.gainDb < 0 ? Pal::warn : Pal::accent); g.setFont(F(13, 3));
                    g.drawText(juce::String(b.gainDb, 1) + " dB", x + 166, y, 60, rh, Justification::centredLeft);
                    juce::String what = b.type == -2 ? "HARMONIC BASS" : b.type == -3 ? "PRESENCE FILL" : b.type == -1 ? "LEVEL" : b.self ? "SELF" : b.dynamic ? (b.type == (int) BandType::LowShelf ? "DIP <" : "DIP") : b.type == (int) BandType::Bell ? "BELL" : "SHELF";
                    if (b.type == -2) what << " " << fmtHz(b.freq) << "-" << fmtHz(b.q) << " (harmonics)";
                    else if (b.type == -3) what << " " << fmtHz(b.freq) << " (harmonics)";
                    else if (b.type == -1) what << ((int) b.q < 0 ? "  whole song" : "  in " + ed.sectionsText(1u << (int) b.q));
                    else what << " " << fmtHz(b.freq);
                    if (b.dynamic && !b.self) if (auto* L = ed.findUid(b.leaderUid)) what << "  follows " << L->name;
                    if (b.sectionMask) what << "  in " << ed.sectionsText(b.sectionMask);
                    if (b.ruleId > 0) what << "  (rule " << b.ruleId << ")";
                    g.setColour(Pal::dim); g.setFont(F(12, 3));
                    g.drawText(what, x + 226, y, colW - 232, rh, Justification::centredLeft);
                }
                if ((int) rows.size() > perCol * cols) {
                    g.setColour(Pal::faint); g.drawText("+" + juce::String((int) rows.size() - perCol * cols) + " more", getWidth() - 120, getHeight() - 20, 100, 14, Justification::right);
                }
                return;
            }
        }
        if (total() == 0) {
            g.setColour(Pal::dim); g.setFont(F(17, 1));
            g.drawFittedText(ed.listening
                                 ? "Listening. Play the busiest passages, where everything plays at once.\nPress STOP when every part has 8 s or more (30 to 60 s overall)."
                                 : "Press LISTEN, play a representative section for 30 to 60 seconds, then STOP.\nIf nothing needs fixing, CLEARING leaves your tracks alone. Press ? HELP for the full guide.",
                             getLocalBounds().reduced(40, 34).withTrimmedTop(10), Justification::centred, 3);
            return;
        }
        g.setColour(Pal::faint); g.setFont(F(12, 3));
        g.drawText(juce::String((int) std::count_if(ed.suggestions.begin(), ed.suggestions.end(), [](auto& s) { return !s.skip; })) + " of "
                       + juce::String(ed.suggestions.size()) + " selected", getWidth() - 220, 8, 202, 18, Justification::right);

        const int vis = visibleRows();
        for (int i = 0; i < vis && first + i < total(); ++i) {
            const int idx = first + i;
            auto r = rowRect(i).toFloat();
            if (idx < (int) ed.notes.size()) { // a note
                g.setColour(Pal::panel2.withAlpha(0.6f)); g.fillRoundedRectangle(r, 8);
                g.setColour(Pal::warn.withAlpha(0.6f)); g.drawRoundedRectangle(r, 8, 1);
                g.setColour(Pal::warn); g.setFont(F(12, 3));
                g.drawText("NOTE", (int) r.getX() + 12, (int) r.getY(), 40, (int) r.getHeight(), Justification::centredLeft);
                g.setColour(Pal::text.withAlpha(0.85f)); g.setFont(F(14, 0));
                g.drawFittedText(ed.notes[(size_t) idx], Rectangle<int>((int) r.getX() + 60, (int) r.getY() + 3, (int) r.getWidth() - 72, (int) r.getHeight() - 6),
                                 Justification::centredLeft, 2, 0.9f);
                continue;
            }
            const int si = idx - (int) ed.notes.size();
            auto& s = ed.suggestions[(size_t) si];
            auto* t = ed.find(s.slot);
            const bool hov = si == ed.hoverSuggestion;
            g.setColour(hov ? Pal::panel2.brighter(0.06f) : Pal::panel2); g.fillRoundedRectangle(r, 8);
            const auto col = t ? roleColour(t->role) : Pal::dim;
            g.setColour(hov ? col : Pal::line); g.drawRoundedRectangle(r, 8, 1);

            auto cb = Rectangle<float>(r.getX() + 12, r.getCentreY() - 8, 16, 16);
            g.setColour(s.skip ? Pal::faint : Pal::accent); g.drawRoundedRectangle(cb, 4, 1.6f);
            if (!s.skip) g.fillRoundedRectangle(cb.reduced(4), 2);

            const float a = s.skip ? 0.4f : 1.0f;
            g.setColour(col.withAlpha(a)); g.fillEllipse(r.getX() + 40, r.getY() + 10, 9, 9);
            g.setColour(Pal::text.withAlpha(a)); g.setFont(F(16, 2));
            g.drawText(t ? t->name : "?", (int) r.getX() + 56, (int) r.getY() + 4, 150, 22, Justification::left);
            g.setFont(F(15, 3));
            g.setColour((s.change.gainDb < 0 ? Pal::warn : Pal::accent).withAlpha(a));
            g.drawText((s.change.gainDb > 0 ? "+" : juce::String()) + juce::String(s.change.gainDb, 1) + " dB", (int) r.getX() + 40, (int) r.getY() + 26, 80, 18, Justification::left);
            g.setColour((s.change.dynamic ? Pal::accent : Pal::dim).withAlpha(a)); g.setFont(F(12, 3));
            g.drawText(juce::String(s.change.self ? "SELF " : s.change.dynamic ? (s.change.type == BandType::LowShelf ? "DIP < " : "DIP ")
                                   : s.change.type == BandType::Bell ? "BELL " : "SHELF ") + fmtHz(s.change.freq)
                           + (s.change.sectionMask ? "  " + ed.sectionsText(s.change.sectionMask).toUpperCase() : juce::String()),
                       (int) r.getX() + 112, (int) r.getY() + 28, 240, 16, Justification::left);

            g.setColour(Pal::dim.withAlpha(a)); g.setFont(F(14, 0));
            g.drawFittedText(s.change.reason, Rectangle<int>((int) r.getX() + 250, (int) r.getY() + 3, (int) r.getWidth() - 250 - 130, (int) r.getHeight() - 6),
                             Justification::centredLeft, 2, 0.9f);
            auto cr = Rectangle<float>(r.getRight() - 112, r.getCentreY() - 12, 96, 24);
            g.setColour(Pal::faint); g.setFont(F(11, 3));
            g.drawText("CONFIDENCE", cr.removeFromTop(12).toNearestInt(), Justification::left);
            g.setColour(Pal::line); g.fillRoundedRectangle(cr.withHeight(5).withY(cr.getY() + 5), 2.5f);
            g.setColour((s.change.confidence > 0.66f ? Pal::accent : s.change.confidence > 0.4f ? Pal::warn : Pal::dim).withAlpha(a));
            g.fillRoundedRectangle(cr.withHeight(5).withY(cr.getY() + 5).withWidth(96 * juce::jlimit(0.05f, 1.0f, s.change.confidence)), 2.5f);
        }
        if (total() > vis) {
            g.setColour(Pal::faint); g.setFont(F(12, 3));
            g.drawText("scroll for " + juce::String(total() - vis - first) + " more", getWidth() - 220, getHeight() - 20, 202, 14, Justification::right);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override { setHover(hit(e.getPosition())); }
    void mouseExit(const juce::MouseEvent&) override { setHover(-1); }
    void mouseDown(const juce::MouseEvent& e) override
    {
        const int i = hit(e.getPosition());
        if (i < 0) return;
        auto& s = ed.suggestions[(size_t) i];
        const int rowIdx = i + (int) ed.notes.size() - first;
        if (e.x < rowRect(rowIdx).getX() + 36) s.skip = !s.skip; else ed.select(s.slot);
        repaint(); ed.repaint();
    }
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        const int maxFirst = std::max(0, total() - visibleRows());
        first = juce::jlimit(0, maxFirst, first + (w.deltaY < 0 ? 1 : -1));
        repaint();
    }
    void resetScroll() { first = 0; }

private:
    int visibleRows() const { return std::max(1, (getHeight() - 36) / rowH); }
    Rectangle<int> rowRect(int i) const { return Rectangle<int>(10, 30 + i * rowH, getWidth() - 20, rowH - 4); }
    // Index into ed.suggestions, or -1 (notes aren't clickable).
    int hit(juce::Point<int> p) const
    {
        for (int i = 0; i < visibleRows(); ++i) {
            const int idx = first + i - (int) ed.notes.size();
            if (rowRect(i).contains(p) && idx >= 0 && idx < (int) ed.suggestions.size()) return idx;
        }
        return -1;
    }
    void setHover(int i) { if (i != ed.hoverSuggestion) { ed.hoverSuggestion = i; repaint(); ed.repaint(); } }

    ClearingEditor& ed;
    int first = 0;
};

// ---------------------------------------------------------------------------------------------
// Help & support: a contents column on the left, the guide on the right. The "Work with Claude"
// page shows live connection status, what Claude has done, and prompts you can copy.
struct LlmState {
    bool connected = false, working = false;
    juce::String client, version;
    std::vector<juce::String> activity; // newest first, "21:14  applied 2 changes on Bass"
    bool operator==(const LlmState& o) const { return connected == o.connected && working == o.working && client == o.client && activity == o.activity; }
};

class HelpPanel : public juce::Component {
public:
    explicit HelpPanel(std::function<void()> onClose) : close(std::move(onClose))
    {
        closeBtn.getProperties().set("primary", true);
        closeBtn.onClick = [this] { close(); };
        addAndMakeVisible(closeBtn);
        view.setViewedComponent(&body, false);
        view.setScrollBarsShown(true, false);
        view.setScrollBarThickness(8);
        addAndMakeVisible(view);
        for (size_t i = 0; i < body.secs.size(); ++i) {
            auto* b = nav.add(new juce::TextButton(body.secs[i].nav));
            b->getProperties().set("nav", true);
            b->onClick = [this, i] { go((int) i); };
            addAndMakeVisible(b);
        }
        body.onCopied = [this](const juce::String& what) { copied = what; copiedAt = juce::Time::getMillisecondCounter(); repaint(); };
    }

    // Jump to a page by its key ("start", "claude", ...).
    void showPage(const juce::String& key)
    {
        for (size_t i = 0; i < body.secs.size(); ++i) if (body.secs[i].key == key) { go((int) i); return; }
    }
    void setLlm(const LlmState& s)
    {
        if (copied.isNotEmpty() && juce::Time::getMillisecondCounter() - copiedAt >= 2500) { copied.clear(); repaint(); }
        if (s == body.llm) return;
        body.llm = s;
        if (isVisible()) relayout();
    }

    void resized() override { relayout(); }
    void visibilityChanged() override { if (isVisible()) relayout(); }

    void paint(Graphics& g) override
    {
        g.fillAll(Pal::bg);
        g.setColour(Pal::accent); g.setFont(F(30, 2));
        g.drawText("HELP & SUPPORT", 40, 26, 500, 44, Justification::centredLeft);
        g.setColour(Pal::line); g.drawVerticalLine(navRight + 14, 96.0f, (float) getHeight() - 30);
        if (copied.isNotEmpty() && juce::Time::getMillisecondCounter() - copiedAt < 2500) {
            g.setColour(Pal::accent); g.setFont(F(15, 1));
            g.drawText("Copied: paste it into Claude", getWidth() - 480, 30, 320, 34, Justification::centredRight);
        }
    }

private:
    void go(int i)
    {
        view.setViewPosition(0, i < (int) body.tops.size() ? body.tops[(size_t) i] : 0);
        for (int k = 0; k < nav.size(); ++k) nav[k]->setToggleState(k == i, juce::dontSendNotification);
    }
    void relayout()
    {
        auto r = getLocalBounds().reduced(40, 26);
        auto top = r.removeFromTop(44);
        closeBtn.setBounds(top.removeFromRight(110).withSizeKeepingCentre(110, 34));
        r.removeFromTop(26);
        auto left = r.removeFromLeft(230);
        navRight = left.getRight();
        for (auto* b : nav) { b->setBounds(left.removeFromTop(30)); left.removeFromTop(2); }
        r.removeFromLeft(30);
        const auto pos = view.getViewPosition();
        view.setBounds(r);
        body.setSize(r.getWidth() - 12, body.layout(r.getWidth() - 12, r.getHeight()));
        view.setViewPosition(pos);
        repaint();
    }

    struct Body : juce::Component {
        struct Sec {
            juce::String key, nav, title, text;
            std::vector<std::pair<juce::String, juce::String>> prompts; // label, prompt text
            bool live = false; // the Claude status block
        };
        LlmState llm;
        std::function<void(const juce::String&)> onCopied;
        std::vector<Sec> secs {
            { "start", "Start here", "START HERE",
              "CLEARING listens to every track it's on and makes small, explained changes so the parts that matter come through: EQ cuts where parts crowd each other, dips that move with the lead, small level moves, and harmonics where a part won't translate. It never touches a locked track, and when nothing needs fixing it proposes nothing.\n\n"
              "There are two ways to work:\n"
              "  WITH CLAUDE (recommended): Claude reads the session through CLEARING, asks what each part is and how the song should feel, listens while you play, explains what it hears, draws pictures of the mix, and applies only what you agree to. See WORK WITH CLAUDE.\n"
              "  ON YOUR OWN: set roles and priorities in this window, press LISTEN, play, and APPLY what you like. See USING IT ON ITS OWN.\n\n"
              "Either way, put one more CLEARING on the Stereo Out and set Mode to Master out. It measures the finished mix (loudness, real levels, mono safety) and never changes the sound." },
            { "claude", "Work with Claude", "WORK WITH CLAUDE (AI)",
              "An AI assistant can run CLEARING for you, like a mix engineer sitting beside you. It sees every CLEARING instance in the session (not Logic itself), so it can inspect, ask, analyse and change things, and you hear the result in Logic as it happens. Everything it does is listed in the box above and can be undone with UNDO, and COMPARE always gives you the before and after.\n\n"
              "WHAT CLAUDE CAN DO\n"
              "  Inspect: every track's name, role, level and spectrum, and what CLEARING has applied.\n"
              "  Ask you: what each part is, which tracks are layered, the sections, how each should feel and land, and who leads.\n"
              "  Listen and analyse: capture while you play, read the arrangement bar by bar, find the sections, measure real levels after your faders, check loudness, true peak and mono safety, and spot parts that cancel or that CLEARING can't see.\n"
              "  Show you: pictures of the arrangement, who owns each frequency band, the spectrum over time, and the stereo image.\n"
              "  Change things (only when you agree): EQ, dips that follow the lead, level moves per section, harmonic bass and presence fill, section intensity. All small, all undoable.\n\n"
              "SET UP (ONCE)\n"
              "  1. Install the Claude desktop app and sign in.\n"
              "  2. CLEARING's installer adds the 'clearing' connector to Claude (Settings > Developer shows it). Quit and reopen Claude after installing.\n"
              "  3. Open Logic with CLEARING on your tracks. The status above turns green when Claude is connected.\n"
              "  4. In Claude, start a new chat and paste one of the prompts below.\n\n"
              "HOW A SESSION GOES\n"
              "  Claude checks what it can see, asks a few questions in small batches, asks you to press play, listens, and explains what it found. You say yes, no, or 'try it'; it applies, you listen with COMPARE, and keep or UNDO. Save the Logic project to keep it.",
              {
                  { "Start a session", "Look at my Logic session with CLEARING. Ask me about the parts and the arrangement first, then listen and suggest improvements. Explain each change before applying it." },
                  { "Quick mix check", "Use CLEARING to check my mix: listen while I play the whole song, then tell me about loudness, true peak, mono safety, and anything clashing or cancelling. Show me the pictures. Don't change anything yet." },
                  { "Fix the low end", "With CLEARING, focus on the low end: kick and bass. Check they're not fighting or cancelling, whether the bass will translate on small speakers, and suggest small changes." },
                  { "Make the lead come through", "Using CLEARING, make the lead part come through in every section without making it louder overall. Ask me which part leads where." },
                  { "Show me the arrangement", "Use CLEARING to listen while I play from the top, then draw the arrangement map and tell me where the energy peaks and where parts crowd each other." },
                  { "Balance the levels", "Use CLEARING to balance my mix. The style is industrial and the vocal should be blended: clear but part of it. Listen while I play the whole song, then show me the balance and propose fader moves, and check my gain staging." },
                  { "Undo and compare", "Turn CLEARING's compare on so I can hear before and after at matched loudness, then tell me what changed on each track." },
              }, true },
            { "own", "Using it on its own", "USING IT ON ITS OWN",
              "1. Insert CLEARING on every track you want included, after its tone-shaping effects (EQ, compression, saturation). Instances in the same Group find each other automatically, and any one window shows them all.\n"
              "2. In TRACKS, set each part's Role and Priority. Priority is how much the part needs to be heard: lower parts give way to higher ones. LOCK keeps a track's tone untouched.\n"
              "3. Layers: give tracks that make one sound together (four piano patches, a stack of synth pads) the same layer, L1 to L8. They're judged and corrected as one instrument.\n"
              "4. Pick Band / Pop, or Orchestral for gentler, broader moves.\n"
              "5. Press LISTEN and play the busiest 30 to 60 seconds. Every part needs at least 8 s of playing to be included. Press STOP.\n"
              "6. Review the suggestions, untick any you don't want, and press APPLY. COMPARE plays everything without CLEARING at matched loudness. UNDO reverts the whole step.\n"
              "7. Save the Logic project. Settings and changes are stored in each instance, so bounces don't need the window open.\n\n"
              "Amber NOTE rows are observations, not changes: a layer that adds almost nothing, two near-identical layers that may comb-filter, a part that wasn't heard long enough." },
            { "move", "Movement and leads", "MOVEMENT AND SECTION LEADS",
              "The highest-priority part is the focus. Supporting parts that crowd its range get a DIP: they ease back a little around the focus's frequencies only while it plays, and come back when it rests, the way players back off for a soloist. MOVEMENT sets how deep the dips go; at 0% the mix is static. Dips respond in about 40 ms and recover over about 350 ms: right for sustained parts, not kick-tight ducking.\n\n"
              "A section can name its own lead. There, the lead owns its range and everything else eases back only while it plays. A bass lead owns the low end below 100 to 220 Hz (kick and snare are left alone). Claude sets leads from your answers about the arrangement." },
            { "self", "Self-triggered EQ", "SELF-TRIGGERED DYNAMIC EQ",
              "Some parts build up only in bursts: stacked chords booming around 150 to 400 Hz, synth brass going harsh at 2 to 5 kHz in loud passages, sibilance. CLEARING spots bands that jump much further above their usual level than the rest of the part, and gives them a SELF band that cuts only during the build-up." },
            { "levels", "Levels and balance", "LEVELS AND BALANCE",
              "Most of a good mix is level. With a CLEARING on the Stereo Out, Claude can balance the whole mix: it measures how loud each part really is at the output, section by section (after your faders, pans and buses), compares that with the balance your style calls for, and proposes fader moves with a reason for each.\n\n"
              "Styles: industrial, rock, pop, electronic, hip hop, orchestral, ambient. Each sets how the groups sit against each other: in industrial the beat sits nearly level with the vocal and the guitars a few dB under; in pop the vocal sits clearly on top. You choose where the lead goes: ON TOP (forward, like pop or country), BLENDED (clear but part of the band), or INSIDE (half-buried in the wall).\n\n"
              "Moves are whole-song where a part is consistently off, with rides per section where it's off only in places, up to 6 dB, in half-dB steps. They go into each track's Trim (after the EQ, automatable), glide in, are saved with the project, print with a bounce, and UNDO takes them back. Locked tracks are never moved. Claude also checks gain staging: inserts peaking near 0 dBFS, very quiet sources, and faders pulled far down that would be better fixed at the source.\n\n"
              "Best results: CLEARING on every track that plays, and listen with COMPARE on (so the measurement is your faders, not CLEARING's own changes)." },
            { "harm", "Harmonics", "HARMONICS (SURGICAL SATURATION)",
              "Two tools that add harmonics instead of level.\n"
              "HARMONIC BASS saturates only a bass's fundamental (say 30 to 90 Hz) and keeps only the harmonics it makes, so the note still reads on earbuds and laptops. The sub can then come down for headroom.\n"
              "PRESENCE FILL saturates a narrow band around a lead's presence and adds only what's above it, so the lead reads clearer without getting louder.\n"
              "Both follow the part's dynamics, glide in and out, follow Amount, and are switched off by COMPARE. Check harmonic bass on small speakers." },
            { "master", "Master out and mix health", "MASTER OUT AND MIX HEALTH",
              "One CLEARING on the Stereo Out with Mode set to Master out changes nothing and measures the finished mix while listening: loudness (integrated and loudest 3 s), true peak, and per band how mono-safe and wide the mix is, bar by bar. By comparing the stereo out with every track it also works out each track's real level after faders, pans and buses, finds energy no CLEARING track accounts for (a part, a return, a bus effect), and low end lost where two low parts cancel. Its row in TRACKS shows LUFS and true peak." },
            { "arr", "Arrangement and pictures", "ARRANGEMENT AND PICTURES",
              "While listening, every instance records each bar: energy in seven bands and approximate loudness. Claude uses this to find the sections (who plays where) and to see where parts fight, and draws pictures: an arrangement map, who owns each band in a section, the spectrum over time, and the stereo image. They're saved in Music > CLEARING Views." },
            { "auto", "Automation and sections", "AUTOMATION AND SECTIONS",
              "In Logic, show automation on any CLEARING track and choose Trim (level after the EQ), Amount (how much of the EQ and harmonics), Movement (how deep the dips go), or Bypass. Changes glide over about 30 ms, so automation never clicks. Touch and Latch work, and the AMOUNT and MOVEMENT sliders record as gestures. Setup controls (role, priority, layer, lock, style, group, mode) aren't automatable on purpose.\n\n"
              "Sections give each part of the song its own intensity: an intimate verse with more movement around the lead, a huge chorus with less EQ so the full stack blooms. Each scales Amount and Movement for its bars, glides into the next, multiplies with your automation, and is saved with the project." },
            { "other", "Other AI tools", "OTHER AI TOOLS AND THE BRIDGE",
              "CLEARING speaks MCP, the standard way AI apps use tools. Besides the Claude desktop app:\n"
              "  Claude Code: claude mcp add clearing /path/to/clearing-mcp\n"
              "  Any MCP app (Cursor, others): add a server whose command is /path/to/clearing-mcp\n"
              "  Anything else (scripts, other assistants): the CLEARING bridge, a command and a local web endpoint.\n"
              "    clearing status: what's live and what to do next.\n"
              "    clearing tools: every tool, with what it does.\n"
              "    clearing call list_tracks: run a tool (arguments as JSON or key=value, e.g. clearing call set_track slot=2 role=bass).\n"
              "    clearing prompt: a ready-made briefing to paste into any assistant.\n"
              "    clearing serve --http 7811: also answer on http://127.0.0.1:7811 (this Mac only; GET /status, /tools, POST /call).\n"
              "The bridge starts itself when needed, keeps one connection so suggestions and UNDO carry across calls, and picks up updates by itself.\n\n"
              "Updates: when CLEARING is reinstalled, the connection in Claude updates itself (no restart of Claude needed). If a new version changes how instances talk to each other, quit and reopen Logic once." },
            { "limits", "Limits", "WHAT IT MEASURES (LIMITS)",
              "Each track is measured at its insert, before the fader, pan and buses; a CLEARING on the Stereo Out adds what the fader does. Moves are small by design: cuts up to 3 dB (2 dB in Orchestral), boosts up to 2 dB, dips up to 3 dB, balance moves up to 6 dB. Each track holds up to six EQ bands. Up to 32 tracks take part; tracks without CLEARING are invisible except as unexplained energy at the Stereo Out. CLEARING hears frequency bands, not notes or chords." },
            { "trouble", "Troubleshooting", "TROUBLESHOOTING",
              "A track is missing from the list: check it's in the same Group. The footer says whether the shared hub is connected; if it isn't, quit and reopen Logic.\n"
              "Nothing is proposed: check each part's captured time (8 s or more) and that priorities differ. Equal priorities never cut each other.\n"
              "It sounds thinner: lower AMOUNT, untick the moves you don't like, or LOCK the track.\n"
              "Dips don't move: the focus must be playing, and MOVEMENT must be above 0.\n"
              "Claude shows NOT CONNECTED: quit and reopen the Claude desktop app; check Settings > Developer lists 'clearing' as running. The status updates within a couple of seconds.\n"
              "Claude says it sees no tracks: Logic must be open with CLEARING on tracks. After an update, quit and reopen Logic once.\n"
              "Claude's change didn't take: look at APPLIED NOW, or ask it to list the tracks; everything it does is in the activity list on the WORK WITH CLAUDE page." },
            { "support", "Support", "SUPPORT",
              "CLEARING 0.8 by Rob Cawley. robcawley.com\nWorking build for testing in Logic Pro on Apple Silicon. Report anything odd with the project name, the track and what you heard." },
        };
        std::vector<int> tops, heights;
        juce::OwnedArray<juce::TextButton> copyBtns;

        juce::String liveText() const
        {
            juce::String s;
            if (llm.connected) {
                s << (llm.working ? "Working now: " : "Connected: ") << (llm.client.isNotEmpty() ? llm.client : juce::String("an AI assistant"))
                  << " can see this session" << (llm.version.isNotEmpty() ? " (connection " + llm.version + ")" : juce::String()) << ".";
            } else {
                s << "Not connected. Open the Claude desktop app (with the clearing connector installed) and start a chat; this turns green within a couple of seconds.";
            }
            if (!llm.activity.empty()) {
                s << "\n\nRECENT ACTIVITY\n";
                for (size_t i = 0; i < llm.activity.size() && i < 8; ++i) s << "  " << llm.activity[i] << "\n";
            }
            return s.trimEnd();
        }
        static juce::TextLayout para(const juce::String& t, float w, juce::Colour c)
        {
            juce::AttributedString as; as.setText(t); as.setFont(F(16, 0)); as.setColour(c); as.setLineSpacing(3.0f);
            juce::TextLayout tl; tl.createLayout(as, w); return tl;
        }
        int layout(int width, int viewH)
        {
            copyBtns.clear();
            tops.clear(); heights.clear();
            int y = 0;
            const float tw = (float) width - 20;
            for (size_t i = 0; i < secs.size(); ++i) {
                auto& s = secs[i];
                tops.push_back(y);
                y += 34;
                if (s.live) y += (int) std::ceil(para(liveText(), tw - 28, Pal::text).getHeight()) + 30 + 16;
                const int h = (int) std::ceil(para(s.text, tw, Pal::text).getHeight());
                heights.push_back(h);
                y += h;
                if (!s.prompts.empty()) {
                    y += 22 + 26; // "TRY THESE PROMPTS"
                    for (auto& [label, text] : s.prompts) {
                        const int ph = (int) std::ceil(para(text, tw - 150, Pal::text).getHeight()) + 34;
                        auto* b = copyBtns.add(new juce::TextButton("COPY"));
                        b->setTooltip("Copy this prompt, then paste it into a Claude chat");
                        const juce::String t = text, l = label;
                        b->onClick = [this, t, l] { juce::SystemClipboard::copyTextToClipboard(t); if (onCopied) onCopied(l); };
                        addAndMakeVisible(b);
                        b->setBounds((int) tw - 96, y + (ph - 28) / 2, 90, 28);
                        y += ph + 8;
                    }
                }
                y += 30;
            }
            // let the last page scroll to the top
            return std::max(y, tops.empty() ? y : tops.back() + viewH);
        }
        void paint(Graphics& g) override
        {
            const float tw = (float) getWidth() - 20;
            for (size_t i = 0; i < secs.size(); ++i) {
                auto& s = secs[i];
                int y = tops[i];
                g.setColour(Pal::accent); g.setFont(F(15, 2));
                g.drawText(s.title, 0, y, getWidth(), 24, Justification::centredLeft);
                g.setColour(Pal::line); g.drawHorizontalLine(y + 28, 0, (float) getWidth());
                y += 34;
                if (s.live) {
                    auto tl = para(liveText(), tw - 28, Pal::text);
                    const int h = (int) std::ceil(tl.getHeight()) + 30;
                    auto box = juce::Rectangle<float>(0, (float) y, tw, (float) h);
                    g.setColour(Pal::panel2); g.fillRoundedRectangle(box, 6);
                    g.setColour(llm.connected ? Pal::accent : Pal::warn); g.fillRoundedRectangle(box.removeFromLeft(5), 2);
                    tl.draw(g, juce::Rectangle<float>(18, (float) y + 15, tw - 28, (float) h - 30));
                    y += h + 16;
                }
                para(s.text, tw, Pal::text.withAlpha(0.88f)).draw(g, juce::Rectangle<float>(0, (float) y, tw, (float) heights[i]));
                y += heights[i];
                if (!s.prompts.empty()) {
                    y += 22;
                    g.setColour(Pal::accent); g.setFont(F(14, 2));
                    g.drawText("TRY THESE PROMPTS (COPY, THEN PASTE INTO CLAUDE)", 0, y, getWidth(), 20, Justification::centredLeft);
                    y += 26;
                    for (auto& [label, text] : s.prompts) {
                        auto tl = para(text, tw - 150, Pal::text);
                        const int ph = (int) std::ceil(tl.getHeight()) + 34;
                        g.setColour(Pal::panel); g.fillRoundedRectangle(0, (float) y, tw, (float) ph, 6);
                        g.setColour(Pal::dim); g.setFont(F(13, 2));
                        g.drawText(label.toUpperCase(), 14, y + 6, (int) tw - 150, 16, Justification::centredLeft);
                        tl.draw(g, juce::Rectangle<float>(14, (float) y + 24, tw - 150, (float) ph - 30));
                        y += ph + 8;
                    }
                }
            }
        }
    };

    std::function<void()> close;
    juce::TextButton closeBtn { "CLOSE" };
    juce::OwnedArray<juce::TextButton> nav;
    juce::Viewport view;
    Body body;
    int navRight = 270;
    juce::String copied;
    juce::uint32 copiedAt = 0;
};

} // namespace ui

// =============================================================================================
ClearingEditor::ClearingEditor(ClearingProcessor& p) : juce::AudioProcessorEditor(p), proc(p)
{
    look = std::make_unique<ui::Look>();
    setLookAndFeel(look.get());

    spectrum = std::make_unique<ui::SpectrumView>(*this);
    strips = std::make_unique<ui::StripPanel>(*this);
    suggest = std::make_unique<ui::SuggestPanel>(*this);
    help = std::make_unique<ui::HelpPanel>([this] { showHelp(false); });
    addAndMakeVisible(*spectrum); addAndMakeVisible(*strips); addAndMakeVisible(*suggest);
    addChildComponent(*help);

    listenBtn.getProperties().set("primary", true);
    listenBtn.setTooltip("Capture what every track plays. Play the busiest 30 to 60 seconds.");
    listenBtn.onClick = [this] { listening ? stopListening() : startListening(); };
    applyBtn.getProperties().set("primary", true);
    applyBtn.setTooltip("Apply the ticked suggestions to every track");
    applyBtn.onClick = [this] { applySuggestions(); };
    undoBtn.setTooltip("Revert the last APPLY on every track");
    undoBtn.onClick = [this] { undoApply(); };
    helpBtn.setTooltip("How to use CLEARING, working with Claude, automation, sections, and support");
    helpBtn.onClick = [this] { showHelp(!help->isVisible()); };
    llmBtn.setTooltip("CLEARING can be run by Claude (or another AI assistant): it inspects the session, asks about the parts and the arrangement, listens, analyses and applies what you agree to. Click for how to use it.");
    llmBtn.onClick = [this] { showHelp(true); help->showPage("claude"); };
    compareBtn.setClickingTogglesState(true);
    compareBtn.setTooltip("Hear every track with CLEARING bypassed");
    compareBtn.onClick = [this] { if (auto* h = proc.getHub().get()) h->bypassAll.store(compareBtn.getToggleState() ? 1 : 0); };
    boldBtn.setClickingTogglesState(true);
    boldBtn.setTooltip("Plan with larger moves. Subtle is the default.");

    styleBox.addItemList({ "Band / Pop", "Orchestral" }, 1);
    groupBox.addItemList({ "Group 1", "Group 2", "Group 3", "Group 4", "Group 5", "Group 6", "Group 7", "Group 8" }, 1);
    styleAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(proc.apvts, "style", styleBox);
    groupAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(proc.apvts, "group", groupBox);
    modeBox.addItemList({ "Track", "Master out" }, 1);
    modeBox.setTooltip("Master out: put this instance on the stereo out. It only measures the finished mix (loudness, real track levels).");
    modeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(proc.apvts, "mode", modeBox);

    for (auto* s : { &amountSlider, &movementSlider }) {
        s->setSliderStyle(juce::Slider::LinearHorizontal);
        s->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        s->setRange(0.0, 1.0, 0.01);
        s->setColour(juce::Slider::trackColourId, ui::Pal::accent);
    }
    amountSlider.setTooltip("Amount for every track in the group. Per-track Amount is automatable in Logic.");
    movementSlider.setTooltip("How deep the focus-following dips go, for every track. Per-track Movement is automatable in Logic.");
    amountSlider.setValue(proc.apvts.getRawParameterValue("amount")->load(), juce::dontSendNotification);
    movementSlider.setValue(proc.apvts.getRawParameterValue("movement")->load(), juce::dontSendNotification);
    amountSlider.onValueChange = [this] { setAllTracks((float) amountSlider.getValue(), -1); repaint(); };
    movementSlider.onValueChange = [this] { setAllTracks(-1, (float) movementSlider.getValue()); repaint(); };
    for (juce::Component* c : std::initializer_list<juce::Component*> { &listenBtn, &applyBtn, &undoBtn, &compareBtn, &boldBtn, &helpBtn,
                                                                        &styleBox, &groupBox, &modeBox, &amountSlider, &movementSlider, &llmBtn })
        addAndMakeVisible(*c);

    setSize(1180, 780);
    refreshNow();
    startTimerHz(30);
}

ClearingEditor::~ClearingEditor() { stopTimer(); setLookAndFeel(nullptr); }

void ClearingEditor::showHelpPage(const juce::String& key)
{
    showHelp(true);
    refreshLlm();
    help->showPage(key);
}

void ClearingEditor::showHelp(bool show)
{
    help->setVisible(show);
    if (show) help->toFront(true);
}

void ClearingEditor::resized()
{
    auto r = getLocalBounds().reduced(16);
    auto header = r.removeFromTop(78);
    auto row = header.removeFromTop(36).withTrimmedLeft(300);
    listenBtn.setBounds(row.removeFromLeft(124)); row.removeFromLeft(8);
    applyBtn.setBounds(row.removeFromLeft(90)); row.removeFromLeft(8);
    undoBtn.setBounds(row.removeFromLeft(76)); row.removeFromLeft(8);
    compareBtn.setBounds(row.removeFromLeft(90)); row.removeFromLeft(8);
    boldBtn.setBounds(row.removeFromLeft(76));
    groupBox.setBounds(row.removeFromRight(90)); row.removeFromRight(6);
    styleBox.setBounds(row.removeFromRight(112)); row.removeFromRight(6);
    modeBox.setBounds(row.removeFromRight(106));
    helpBtn.setBounds(150, 52, 84, 24);
    llmBtn.setBounds(150, 80, 300, 22);
    // AMOUNT and MOVEMENT sit on the status row, right side.
    amountSlider.setBounds(getWidth() - 16 - 330, 16 + 50, 150, 20);
    movementSlider.setBounds(getWidth() - 16 - 150, 16 + 50, 150, 20);

    r.removeFromTop(8);
    auto upper = r.removeFromTop(392);
    strips->setBounds(upper.removeFromLeft(380));
    upper.removeFromLeft(12);
    spectrum->setBounds(upper);
    r.removeFromTop(12);
    suggest->setBounds(r.removeFromTop(r.getHeight() - 22));
    help->setBounds(getLocalBounds());
}

void ClearingEditor::paint(Graphics& g)
{
    using namespace ui;
    g.fillAll(Pal::bg);
    g.setColour(Pal::accent); g.setFont(F(36, 2));
    g.drawText("CLEARING", 16, 12, 260, 40, Justification::left);
    g.setColour(Pal::dim); g.setFont(F(14, 1));
    g.drawText("automatic mix EQ", 18, 54, 130, 18, Justification::left);

    float maxAct = 0;
    for (auto& t : tracks) maxAct = std::max(maxAct, t.activeSec);
    juce::String status;
    if (listening) status = "Listening   " + juce::String(maxAct, 0) + " s captured. Aim for 30 to 60 s of the busiest material.";
    else if (!suggestions.empty()) status = juce::String(suggestions.size()) + " changes proposed. Review, untick any you don't want, then APPLY.";
    else if (maxAct > 0) status = "Capture complete (" + juce::String(maxAct, 0) + " s). Nothing needed fixing, so nothing is proposed.";
    else status = "Ready. Press LISTEN, then play your song.";
    g.setColour(listening ? Pal::red : Pal::dim); g.setFont(F(15, 1));
    g.drawText(status, 316, 16 + 40, getWidth() - 316 - 360, 18, Justification::left);
    if (sectionName.isNotEmpty()) {
        g.setColour(Pal::accent); g.setFont(F(13, 3));
        g.drawText("SECTION  " + sectionName.toUpperCase() + (sectionFeel.isNotEmpty() ? "   " + sectionFeel : juce::String()),
                   316, 16 + 58, getWidth() - 316 - 360, 16, Justification::left);
    }
    g.setColour(Pal::dim); g.setFont(F(12, 3));
    g.drawText("AMOUNT " + juce::String((int) std::round(amountSlider.getValue() * 100)) + "%", getWidth() - 16 - 330, 16 + 38, 150, 12, Justification::right);
    g.drawText("MOVEMENT " + juce::String((int) std::round(movementSlider.getValue() * 100)) + "%", getWidth() - 16 - 150, 16 + 38, 150, 12, Justification::right);

    g.setColour(Pal::faint); g.setFont(F(12, 3));
    const bool hubOk = proc.getHub().ok();
    g.drawText(juce::String("Insert-level analysis: measured before fader, pan and bus processing.   ")
                   + (hubOk ? "Hub: shared memory connected." : "Hub: unavailable, running stand-alone.") + "   CLEARING 0.8",
               16, getHeight() - 22, getWidth() - 32, 16, Justification::left);
}

const TrackView* ClearingEditor::find(int slot) const
{
    for (auto& t : tracks) if (t.slot == slot) return &t;
    return nullptr;
}

const TrackView* ClearingEditor::findUid(uint64_t uid) const
{
    for (auto& t : tracks) if (t.uid == uid) return &t;
    return nullptr;
}

juce::String ClearingEditor::sectionsText(uint32_t mask) const
{
    juce::StringArray s;
    for (int k = 0; k < 32; ++k)
        if (mask >> k & 1u) s.add(k < (int) sectionNames.size() ? sectionNames[(size_t) k] : "section " + juce::String(k + 1));
    return s.joinIntoString(", ");
}

void ClearingEditor::select(int slot)
{
    selectedSlot = selectedSlot == slot ? -1 : slot;
    repaint(); strips->repaint(); spectrum->repaint();
}

void ClearingEditor::writeCtrl(int slot, const juce::String& nm, int role, float prio, bool locked, int layer, float amount, float movement)
{
    auto* h = proc.getHub().get();
    if (!h || slot < 0) return;
    auto& s = h->slots[slot];
    seqWrite(s.ctrlSeq, [&] {
        std::strncpy(s.ctrlName, nm.toRawUTF8(), sizeof s.ctrlName - 1); s.ctrlName[sizeof s.ctrlName - 1] = 0;
        s.ctrlRole = role; s.ctrlPriority = prio; s.ctrlLocked = locked ? 1 : 0;
        s.ctrlLayer = layer; s.ctrlAmount = amount; s.ctrlMovement = movement;
    });
}

void ClearingEditor::setAllTracks(float amount, float movement)
{
    for (auto& t : tracks) writeCtrl(t.slot, t.name, t.role, t.priority, t.locked, -1, amount, movement);
}

void ClearingEditor::refreshLlm()
{
    auto* h = proc.getHub().get();
    ui::LlmState s;
    if (h) {
        const uint64_t now = Hub::nowNs();
        const uint64_t beat = h->llmBeatNs.load(), call = h->llmLastCallNs.load();
        s.connected = beat != 0 && now - beat < 4'000'000'000ull;
        s.working = s.connected && call != 0 && now - call < 20'000'000'000ull;
        char cl[48] {}, ver[16] {};
        HubData::LogEntry lg[HubData::kLog];
        uint32_t count = 0;
        seqRead(h->llmSeq, [&] { std::memcpy(cl, h->llmClient, sizeof cl); std::memcpy(ver, h->llmVersion, sizeof ver); std::memcpy(lg, h->log, sizeof lg); count = h->logCount; });
        cl[sizeof cl - 1] = 0; ver[sizeof ver - 1] = 0;
        s.client = juce::String::fromUTF8(cl); s.version = juce::String::fromUTF8(ver);
        const uint32_t n = std::min<uint32_t>(count, HubData::kLog);
        for (uint32_t i = 0; i < n; ++i) {
            const auto& e = lg[(count - 1 - i) % HubData::kLog];
            char t[113] {}; std::memcpy(t, e.text, 112);
            const auto when = juce::Time((juce::int64) e.unixTime * 1000);
            s.activity.push_back(when.formatted("%H:%M") + "  " + juce::String::fromUTF8(t));
        }
    }
    juce::String label = s.working ? juce::String::fromUTF8("\u25CF ") + (s.client.isNotEmpty() ? s.client.toUpperCase() : juce::String("AI")) + " IS WORKING"
                       : s.connected ? juce::String::fromUTF8("\u25CF ") + (s.client.isNotEmpty() ? s.client.toUpperCase() : juce::String("AI")) + " CONNECTED"
                                     : juce::String::fromUTF8("\u25CB WORK WITH CLAUDE");
    if (s.connected && !s.activity.empty()) label << "  " << s.activity.front().fromFirstOccurrenceOf("  ", false, false).substring(0, 34);
    if (llmBtn.getButtonText() != label) llmBtn.setButtonText(label);
    llmBtn.getProperties().set("primary", s.connected);
    llmBtn.setColour(juce::TextButton::textColourOffId, s.connected ? ui::Pal::accent : ui::Pal::dim);
    if (help) help->setLlm(s);
}

void ClearingEditor::refreshNow()
{
    refreshLlm();
    auto* h = proc.getHub().get();
    std::vector<TrackView> now;
    if (h) {
        const int grp = proc.groupId() + 1;
        for (int i = 0; i < kSlots; ++i) {
            if (!proc.getHub().live(i)) continue;
            auto& s = h->slots[i];
            TrackView t; char nm[24] {};
            int g = 1;
            const bool ok = seqRead(s.measSeq, [&] {
                std::memcpy(nm, s.name, sizeof nm); t.role = s.role; t.priority = s.priority; t.locked = s.locked != 0; g = s.group;
                t.layer = s.layer; t.amount = s.amount; t.movement = s.movement; t.section = s.section;
                std::copy(s.live, s.live + kBands, t.live.begin()); std::copy(s.cap, s.cap + kBands, t.cap.begin());
                std::copy(s.capPeak, s.capPeak + kBands, t.capPeak.begin());
                t.mode = s.mode; t.trimParam = s.trimParamDb;
                t.activeSec = s.activeSec; t.rmsDb = s.rmsDb; t.capRmsDb = s.capRmsDb;
            });
            seqRead(s.planSeq, [&] { std::copy(s.plan, s.plan + kEqBands, t.plan.begin()); t.trim = s.trim; t.harm = s.harm; });
            if (t.mode == 1) seqRead(s.hSeq, [&] { t.lufsI = s.hLufsI; t.tpMax = s.hTpMax; });
            if (!ok || g != grp) continue;
            nm[sizeof nm - 1] = 0;
            t.name = nm; t.slot = i; t.uid = s.owner.load(); t.mine = i == proc.getSlot();
            if (t.name.isEmpty()) t.name = "Track " + juce::String(i + 1);
            now.push_back(t);
        }
        compareBtn.setToggleState(h->bypassAll.load() != 0, juce::dontSendNotification);
        if (listening && h->listening.load() == 0) listening = false;

        // Section names and leads, the current section (this instance's playhead) and its intended feel.
        sectionName = {}; sectionFeel = {};
        {
            std::vector<juce::String> names; std::vector<uint64_t> leads;
            if (seqRead(h->briefSeq, [&] {
                    names.clear(); leads.clear();
                    for (int k = 0; k < juce::jlimit(0, kSections, (int) h->numSections); ++k) {
                        char a[24]; std::memcpy(a, h->sections[k].name, sizeof a); a[23] = 0;
                        names.push_back(juce::String::fromUTF8(a)); leads.push_back(h->sections[k].leadUid);
                    }
                })) { sectionNames = names; sectionLeads = leads; }
        }
        const int cur = proc.currentSection();
        if (cur >= 0) seqRead(h->briefSeq, [&] {
            if (cur < h->numSections) {
                char a[24], b[96];
                std::memcpy(a, h->sections[cur].name, sizeof a); std::memcpy(b, h->sections[cur].feel, sizeof b);
                a[23] = 0; b[95] = 0;
                sectionName = juce::String::fromUTF8(a); sectionFeel = juce::String::fromUTF8(b);
            }
        });
    }
    tracks = std::move(now);
    if (selectedSlot >= 0 && !find(selectedSlot)) selectedSlot = -1;
    suggestions.erase(std::remove_if(suggestions.begin(), suggestions.end(), [&](auto& s) { return !find(s.slot); }), suggestions.end());

    if (!amountSlider.isMouseButtonDown()) amountSlider.setValue(proc.apvts.getRawParameterValue("amount")->load(), juce::dontSendNotification);
    if (!movementSlider.isMouseButtonDown()) movementSlider.setValue(proc.apvts.getRawParameterValue("movement")->load(), juce::dontSendNotification);

    if (listening) {
        float maxAct = 0; for (auto& t : tracks) maxAct = std::max(maxAct, t.activeSec);
        if (maxAct >= 60.0f) stopListening();
    }
    updateButtons();
    strips->update();
    spectrum->repaint(); suggest->repaint(); repaint();
}

void ClearingEditor::updateButtons()
{
    listenBtn.setButtonText(listening ? "STOP" : "LISTEN");
    listenBtn.getProperties().set("recording", listening);
    listenBtn.getProperties().set("primary", !listening);
    const bool any = std::any_of(suggestions.begin(), suggestions.end(), [](auto& s) { return !s.skip; });
    applyBtn.setEnabled(any && !listening);
    undoBtn.setEnabled(!undoStack.empty());
    listenBtn.repaint(); applyBtn.repaint(); undoBtn.repaint();
}

void ClearingEditor::startListening()
{
    auto* h = proc.getHub().get();
    if (!h) return;
    suggestions.clear(); notes.clear(); suggest->resetScroll();
    h->listenEpoch.fetch_add(1);
    h->listening.store(1);
    listening = true;
    updateButtons(); repaint();
}

void ClearingEditor::stopListening()
{
    if (auto* h = proc.getHub().get()) h->listening.store(0);
    listening = false;
    refreshNow();
    runPlan();
    updateButtons(); repaint();
}

void ClearingEditor::runPlan()
{
    std::vector<TrackMeas> in;
    for (auto& t : tracks) {
        if (t.activeSec <= 0.0f || t.mode == 1) continue; // the master only measures
        TrackMeas m; m.id = t.slot; m.name = t.name.toStdString(); m.role = t.role; m.priority = t.priority;
        m.locked = t.locked; m.layer = t.layer; m.db = t.cap; m.peakDb = t.capPeak; m.levelDb = t.capRmsDb; m.activeSec = t.activeSec;
        in.push_back(m);
    }
    PlannerSettings st;
    for (size_t k = 0; k < sectionNames.size(); ++k) {
        st.sectionNames.push_back(sectionNames[k].toStdString());
        const TrackView* L = sectionLeads[k] ? findUid(sectionLeads[k]) : nullptr;
        st.sectionLeads.push_back(L ? L->slot : -1);
    }
    st.strength = boldBtn.getToggleState() ? 1.0f : 0.6f;
    st.style = proc.styleId();
    suggestions.clear(); notes.clear();
    auto res = planFull(in, st);
    for (auto& c : res.changes) { Suggestion s; s.change = c; s.slot = c.track; suggestions.push_back(s); }
    for (auto& n : res.notes) notes.push_back(juce::String(n));
    suggest->resetScroll();
}

void ClearingEditor::applySuggestions()
{
    auto* h = proc.getHub().get();
    if (!h) return;
    // Remember what every track in the group has now, so UNDO is one step for the whole mix.
    std::vector<Snapshot> snap;
    for (auto& t : tracks) {
        Snapshot s; s.slot = t.slot;
        auto& sl = h->slots[t.slot];
        seqRead(sl.planSeq, [&] { std::copy(sl.plan, sl.plan + kEqBands, s.bands); });
        snap.push_back(s);
    }
    undoStack.push_back(std::move(snap));

    for (auto& t : tracks) {
        if (t.mode == 1) continue;
        EqBandData b[kEqBands];
        int n = 0;
        for (auto& cur : t.plan) if (cur.enabled && cur.ruleId > 0 && n < kEqBands) b[n++] = cur; // hand-placed dips stay
        for (auto& s : suggestions)
            if (s.slot == t.slot && !s.skip && n < kEqBands) {
                b[n].freq = s.change.freq; b[n].gainDb = s.change.gainDb; b[n].q = s.change.q;
                b[n].type = (int) s.change.type; b[n].enabled = 1;
                if (s.change.dynamic) {
                    b[n].dynamic = 1; b[n].threshDb = s.change.threshDb; b[n].sectionMask = s.change.sectionMask;
                    b[n].self = s.change.self ? 1 : 0;
                    b[n].leaderUid = s.change.leader >= 0 ? h->slots[s.change.leader].owner.load() : 0;
                }
                ++n;
            }
        auto& sl = h->slots[t.slot];
        seqWrite(sl.planSeq, [&] { std::copy(b, b + kEqBands, sl.plan); });
    }
    updateButtons(); repaint();
}

void ClearingEditor::undoApply()
{
    auto* h = proc.getHub().get();
    if (!h || undoStack.empty()) return;
    for (auto& s : undoStack.back()) {
        auto& sl = h->slots[s.slot];
        seqWrite(sl.planSeq, [&] { std::copy(s.bands, s.bands + kEqBands, sl.plan); });
    }
    undoStack.pop_back();
    updateButtons(); repaint();
}
