#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "AudioEngine.h"
#include "DrumKit.h"
#include "SerialSource.h"

using namespace bateria;

//==============================================================================
// Nome amigável (General MIDI percussion) para uma nota; fallback "Nota N".
static juce::String gmDrumName (int note)
{
    switch (note)
    {
        case 35: case 36: return "Bumbo";
        case 37: return "Aro";
        case 38: case 40: return "Caixa";
        case 39: return "Palma";
        case 41: case 43: return "Tom Grave";
        case 45: case 47: return "Tom Medio";
        case 48: case 50: return "Tom Agudo";
        case 42: return "Chimbal";
        case 44: return "Chimbal Pedal";
        case 46: return "Chimbal Aberto";
        case 49: case 57: return "Crash";
        case 51: case 59: return "Ride";
        case 52: return "China";
        case 53: return "Ride Bell";
        case 54: return "Pandeiro";
        case 55: return "Splash";
        case 56: return "Cowbell";
        default:  return "Nota " + juce::String (note);
    }
}

//==============================================================================
// Layout do kit: cada peça desenhada, com posição normalizada [0,1] na área.
struct KitPieceDef
{
    const char* name;
    enum Type { Drum, Cymbal, Kick } type;
    float cx, cy, w, h;   // centro e tamanho relativos à área do kit
    int   defaultNote;    // nota MIDI que o hardware manda para essa peça (GM)
    const char* category; // pasta da biblioteca de samples (kick/snare/tom/...)
};

// Ordem = trás-para-frente (pratos atrás, bumbo na frente).
inline constexpr KitPieceDef kLayout[] = {
    { "Crash E",    KitPieceDef::Cymbal, 0.19f, 0.27f, 0.21f, 0.075f, 49, "crash" },
    { "Crash D",    KitPieceDef::Cymbal, 0.66f, 0.18f, 0.23f, 0.080f, 57, "crash" },
    { "Ride",       KitPieceDef::Cymbal, 0.85f, 0.40f, 0.27f, 0.090f, 51, "ride" },
    { "Chimbal",    KitPieceDef::Cymbal, 0.10f, 0.55f, 0.18f, 0.065f, 42, "hihat" },
    { "Tom 1",      KitPieceDef::Drum,   0.40f, 0.39f, 0.155f, 0.165f, 48, "tom" },
    { "Tom 2",      KitPieceDef::Drum,   0.575f, 0.39f, 0.155f, 0.165f, 47, "tom" },
    { "Surdo",      KitPieceDef::Drum,   0.80f, 0.70f, 0.200f, 0.200f, 43, "tom" },
    { "Caixa",      KitPieceDef::Drum,   0.27f, 0.69f, 0.170f, 0.150f, 38, "snare" },
    { "Bumbo",      KitPieceDef::Kick,   0.49f, 0.77f, 0.310f, 0.310f, 36, "kick" },
};
inline constexpr int kNumPieces = (int) (sizeof (kLayout) / sizeof (kLayout[0]));

//==============================================================================
/** Tema escuro estilo estúdio / Superior Drummer. */
class DarkLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    DarkLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, juce::Colour (0xff17181c));
        setColour (juce::Slider::thumbColourId,               juce::Colour (0xfff0a830));
        setColour (juce::Slider::trackColourId,               juce::Colour (0xff3a3c46));
        setColour (juce::Slider::backgroundColourId,          juce::Colour (0xff23242b));
        setColour (juce::ComboBox::backgroundColourId,        juce::Colour (0xff23242b));
        setColour (juce::ComboBox::textColourId,              juce::Colours::white);
        setColour (juce::ComboBox::outlineColourId,           juce::Colour (0xff3a3c46));
        setColour (juce::TextButton::buttonColourId,          juce::Colour (0xff2a2c34));
        setColour (juce::TextButton::textColourOffId,         juce::Colours::white);
        setColour (juce::Label::textColourId,                 juce::Colour (0xffe6e6ea));
        setColour (juce::PopupMenu::backgroundColourId,       juce::Colour (0xff1d1e24));
    }
};

//==============================================================================
/** Console de log MIDI em tempo real (estilo Hairless). */
class MidiMonitor final : public juce::Component
{
public:
    MidiMonitor()
    {
        // TextEditor read-only multilinha: scroll (barra + roda do mouse) e
        // seleção/cópia (Cmd+C, menu de contexto) nativos, de graça.
        editor.setMultiLine (true, false);
        editor.setReadOnly (true);
        editor.setCaretVisible (false);
        editor.setScrollbarsShown (true);
        editor.setPopupMenuEnabled (true);          // menu: Copiar / Selecionar tudo
        editor.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
        editor.setColour (juce::TextEditor::backgroundColourId,      juce::Colour (0xff0b0b0e));
        editor.setColour (juce::TextEditor::outlineColourId,         juce::Colour (0xff2a2c34));
        editor.setColour (juce::TextEditor::focusedOutlineColourId,  juce::Colour (0xff3a3c46));
        editor.setColour (juce::TextEditor::shadowColourId,          juce::Colours::transparentBlack);
        editor.setTextToShowWhenEmpty ("Aguardando MIDI da serial... bata num pad.",
                                       juce::Colour (0xff55555f));
        addAndMakeVisible (editor);
    }

    void addLine (juce::String text, juce::Colour colour)
    {
        lines.push_back ({ std::move (text), colour });
        // Poda em lote (raro): evita reconstruir o editor a cada linha.
        if ((int) lines.size() > maxLines + trimBatch) { rebuild(); return; }
        appendLine (lines.back());
    }

    void clear() { lines.clear(); editor.clear(); }

    /** Todo o texto do monitor (pro botão Copiar). */
    juce::String getAllText() const { return editor.getText(); }

    void resized() override { editor.setBounds (getLocalBounds()); }

private:
    struct Line { juce::String text; juce::Colour colour; };

    void appendLine (const Line& l)
    {
        editor.moveCaretToEnd();
        editor.setColour (juce::TextEditor::textColourId, l.colour);
        editor.insertTextAtCaret (l.text + juce::newLine);   // cor por linha preservada
        editor.moveCaretToEnd();                             // autoscroll p/ o fim
    }

    void rebuild()
    {
        while ((int) lines.size() > maxLines) lines.pop_front();
        editor.clear();
        for (auto& l : lines) appendLine (l);
    }

    juce::TextEditor editor;
    std::deque<Line> lines;
    static constexpr int maxLines  = 500;
    static constexpr int trimBatch = 100;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiMonitor)
};

//==============================================================================
// Categoria -> formato desenhado e defaults.
enum class Shape { Drum, Cymbal, Kick };
static Shape shapeForCategory (const juce::String& c)
{
    if (c == "kick") return Shape::Kick;
    if (c == "hihat" || c == "crash" || c == "ride") return Shape::Cymbal;
    return Shape::Drum; // snare, tom
}
static int defaultNoteForCategory (const juce::String& c)
{
    if (c == "kick")    return 36;  if (c == "snare")   return 38;
    if (c == "tom")     return 47;  if (c == "hihat")   return 42;
    if (c == "crash")   return 49;  if (c == "ride")    return 51;
    if (c == "clap")    return 39;  if (c == "cowbell") return 56;
    if (c == "perc")    return 60;
    return 38;
}
static juce::String displayNameForCategory (const juce::String& c)
{
    if (c == "kick")    return "Bumbo";   if (c == "snare")   return "Caixa";
    if (c == "tom")     return "Tom";     if (c == "hihat")   return "Chimbal";
    if (c == "crash")   return "Crash";   if (c == "ride")    return "Ride";
    if (c == "clap")    return "Palma";   if (c == "cowbell") return "Cowbell";
    if (c == "perc")    return "Perc";
    return c;
}
static void defaultSizeForCategory (const juce::String& c, float& w, float& h)
{
    switch (shapeForCategory (c))
    {
        case Shape::Kick:   w = 0.30f; h = 0.30f; break;
        case Shape::Cymbal: w = 0.22f; h = 0.075f; break;
        default:            w = 0.16f; h = 0.16f; break;
    }
}
// Posição inicial (centro normalizado) sugerida por categoria — evita que pads
// novos nasçam todos no centro empilhados.
static void defaultPositionForCategory (const juce::String& c, float& cx, float& cy)
{
    if      (c == "kick")    { cx = 0.50f; cy = 0.78f; }
    else if (c == "snare")   { cx = 0.30f; cy = 0.68f; }
    else if (c == "tom")     { cx = 0.50f; cy = 0.40f; }
    else if (c == "hihat")   { cx = 0.12f; cy = 0.55f; }
    else if (c == "crash")   { cx = 0.25f; cy = 0.25f; }
    else if (c == "ride")    { cx = 0.85f; cy = 0.42f; }
    else if (c == "clap")    { cx = 0.42f; cy = 0.60f; }
    else if (c == "cowbell") { cx = 0.62f; cy = 0.55f; }
    else                     { cx = 0.68f; cy = 0.62f; } // perc / outros
}
inline const char* kCategories[] = { "kick", "snare", "tom", "hihat", "crash",
                                     "ride", "clap", "cowbell", "perc" };
inline constexpr int kNumCategories = 9;

//==============================================================================
/** Visão do kit de bateria desenhada em vetor, com pads DINÂMICOS:
    adicionar/remover, arrastar para posicionar, e cada um editável. */
class DrumKitView final : public juce::Component
{
public:
    std::function<void()> onSelectionChanged;
    std::function<void()> onChanged; // mudança que deve persistir

    explicit DrumKitView (AudioEngine& eng) : engine (eng) { selected = -1; } // começa VAZIO

    //== Hits / animação =======================================================
    void hit (int note, float velocity)
    {
        for (auto& p : pieces)
            if (p.note == note && p.enabled)
                p.flash = juce::jmax (p.flash, juce::jlimit (0.3f, 1.0f, velocity * 2.2f));
        repaint();
    }

    void decay()
    {
        bool any = false;
        for (auto& p : pieces)
            if (p.flash > 0.0f) { p.flash *= 0.82f; if (p.flash < 0.01f) p.flash = 0.0f; any = true; }
        if (any) repaint();
    }

    //== Seleção / leitura =====================================================
    int  getSelected() const noexcept { return selected; }
    int  getNumPieces() const noexcept { return (int) pieces.size(); }

    juce::String pieceName (int i) const { return pieces[(size_t) i].name; }
    juce::String pieceCategory (int i) const { return pieces[(size_t) i].category; }
    int   pieceNote (int i)  const { return pieces[(size_t) i].note; }
    int   pieceSound (int i) const { return pieces[(size_t) i].sound; }
    float pieceVol (int i)   const { return pieces[(size_t) i].volume; }
    float pieceSensitivity (int i) const { return pieces[(size_t) i].sensitivity; }
    bool  pieceEnabled (int i) const { return pieces[(size_t) i].enabled; }
    bool  pieceMute (int i)  const { return pieces[(size_t) i].mute; }
    juce::File pieceSampleFile (int i) const { return pieces[(size_t) i].sampleFile; }
    float pieceCX (int i) const { return pieces[(size_t) i].cx; }
    float pieceCY (int i) const { return pieces[(size_t) i].cy; }

    //== Edição (inspector) ====================================================
    void setPieceConfig (int i, int note, int sound, float vol, float sensitivity,
                         bool enabled, bool mute, bool notify = true)
    {
        auto& p = pieces[(size_t) i];
        const int oldNote = p.note;
        p.note = note; p.sound = sound; p.volume = vol; p.sensitivity = sensitivity;
        p.enabled = enabled; p.mute = mute;
        applyPiece (p);
        if (oldNote != note) engine.setNoteSample (note, p.sampleFile);
        repaint();
        if (notify && onChanged) onChanged();
    }

    void setPieceName (int i, const juce::String& name, bool notify = true)
    {
        pieces[(size_t) i].name = name;
        repaint();
        if (notify && onChanged) onChanged();
    }

    /** Troca a categoria (muda formato/defaults). O sample fica a cargo da UI. */
    void setPieceCategory (int i, const juce::String& cat, bool notify = true)
    {
        auto& p = pieces[(size_t) i];
        p.category = cat;
        defaultSizeForCategory (cat, p.w, p.h);
        p.sampleFile = juce::File(); // categoria nova: limpa sample antigo
        repaint();
        if (notify && onChanged) onChanged();
    }

    void setPieceSample (int i, const juce::File& f, bool notify = true)
    {
        pieces[(size_t) i].sampleFile = f;
        engine.setNoteSample (pieces[(size_t) i].note, f);
        if (notify && onChanged) onChanged();
    }

    void setPieceSampleOnly (int i, const juce::File& f) { pieces[(size_t) i].sampleFile = f; }

    //== Adicionar / remover pads =============================================
    /** Cria um pad novo de uma categoria, no centro, com nota livre. Seleciona. */
    int addPiece (const juce::String& cat)
    {
        Piece p;
        p.category = cat;
        p.name = uniqueName (cat);
        p.note = nextFreeNote (defaultNoteForCategory (cat));
        p.sound = p.note;
        pieces.push_back (p);
        selected = (int) pieces.size() - 1;
        applyPiece (pieces.back());
        repaint();
        if (onSelectionChanged) onSelectionChanged();
        if (onChanged) onChanged();
        return selected;
    }

    /** Remove o pad selecionado. */
    void removeSelected()
    {
        if (selected < 0 || selected >= (int) pieces.size()) return;
        engine.setNoteSample (pieces[(size_t) selected].note, juce::File()); // limpa sample
        pieces.erase (pieces.begin() + selected);
        selected = juce::jlimit (0, (int) pieces.size() - 1, selected);
        repaint();
        if (onSelectionChanged) onSelectionChanged();
        if (onChanged) onChanged();
    }

    /** Esvazia o kit inteiro (todos os pads). */
    void clearAll()
    {
        pieces.clear();
        selected = -1;
        repaint();
        if (onSelectionChanged) onSelectionChanged();
        if (onChanged) onChanged();
    }

    //== Carga em lote (config) ===============================================
    void clearPieces() { pieces.clear(); selected = -1; }

    /** Adiciona um pad com estado completo, SEM aplicar (config). */
    void addPieceFull (const juce::String& name, const juce::String& cat,
                       float cx, float cy, int note, float vol, float sensitivity,
                       bool enabled, bool mute, const juce::File& sample)
    {
        Piece p;
        p.name = name; p.category = cat; p.cx = cx; p.cy = cy;
        defaultSizeForCategory (cat, p.w, p.h);
        p.note = note; p.sound = note; p.volume = vol; p.sensitivity = sensitivity;
        p.enabled = enabled; p.mute = mute;
        p.sampleFile = sample;
        pieces.push_back (p);
    }

    /** Espalha pads que ficaram sobrepostos (ex.: config antiga empilhada). */
    void deOverlap()
    {
        const float minD = 0.11f;
        for (size_t i = 0; i < pieces.size(); ++i)
            for (int attempt = 0; attempt < 60; ++attempt)
            {
                bool clash = false;
                for (size_t j = 0; j < pieces.size(); ++j)
                {
                    if (j == i) continue;
                    const float dx = pieces[j].cx - pieces[i].cx, dy = pieces[j].cy - pieces[i].cy;
                    if (dx * dx + dy * dy < minD * minD) { clash = true; break; }
                }
                if (! clash) break;
                pieces[i].cx += 0.09f;
                if (pieces[i].cx > 0.9f) { pieces[i].cx = 0.12f; pieces[i].cy += 0.13f; }
                if (pieces[i].cy > 0.9f) pieces[i].cy = 0.12f;
                pieces[i].cx = juce::jlimit (0.08f, 0.92f, pieces[i].cx);
                pieces[i].cy = juce::jlimit (0.08f, 0.92f, pieces[i].cy);
            }
        repaint();
    }

    void buildDefaultKit()
    {
        pieces.clear();
        for (int i = 0; i < kNumPieces; ++i)
        {
            Piece p;
            p.name = kLayout[i].name;
            p.category = kLayout[i].category;
            p.cx = kLayout[i].cx; p.cy = kLayout[i].cy; p.w = kLayout[i].w; p.h = kLayout[i].h;
            p.note = kLayout[i].defaultNote; p.sound = kLayout[i].defaultNote;
            pieces.push_back (p);
        }
        selected = 0;
    }

    /** Aplica todos os pieces (atomics) e seus samples em LOTE: 1 só rebuild. */
    void applyAllToEngine()
    {
        for (auto& p : pieces)
        {
            applyPiece (p);
            engine.assignSample (p.note, p.sampleFile);
        }
        engine.rebuildActiveBank();
    }

    //== Mouse: clica num quadrado = seleciona + toca =========================
    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto area = getLocalBounds().toFloat();
        for (int i = 0; i < (int) pieces.size(); ++i)
        {
            auto r = tileBounds (i, area);
            if (r.contains (e.position))
            {
                if (selected != i) { selected = i; if (onSelectionChanged) onSelectionChanged(); }
                const float v = juce::jlimit (0.4f, 1.0f, (e.position.y - r.getY()) / juce::jmax (1.0f, r.getHeight()));
                triggerPiece (i, v);
                return;
            }
        }
    }

    /** Toca a peça selecionada (botão de play do inspector). */
    void triggerSelected() { if (selected >= 0 && selected < (int) pieces.size()) triggerPiece (selected, 0.9f); }

    // Expostos para o preview do pad no inspector reutilizarem o mesmo ícone/cor.
    static juce::Colour colourForCategory (const juce::String& c) { return categoryColour (c); }
    static void paintIcon (juce::Graphics& g, juce::Rectangle<float> b,
                           const juce::String& cat, juce::Colour col) { drawCategoryIcon (g, b, cat, col); }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();

        // Fundo escuro com leve gradiente.
        juce::ColourGradient bg (juce::Colour (0xff202128), area.getCentreX(), area.getY(),
                                 juce::Colour (0xff14151a), area.getCentreX(), area.getBottom(), false);
        g.setGradientFill (bg);
        g.fillRect (area);

        if (pieces.empty())
        {
            auto a = area;
            g.setColour (juce::Colour (0xff6a6a76));
            g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
            g.drawText ("Kit vazio", a.removeFromTop (a.getHeight() * 0.45f),
                        juce::Justification::centredBottom, false);
            g.setColour (juce::Colour (0xff8a8a96));
            g.setFont (juce::FontOptions (14.0f));
            g.drawText ("Clique em  + Pad  para adicionar um pad",
                        a, juce::Justification::centredTop, false);
            return;
        }

        for (int i = 0; i < (int) pieces.size(); ++i)
            drawTile (g, i, area);
    }

private:
    struct Piece
    {
        juce::String name, category { "tom" };
        float cx = 0.5f, cy = 0.5f, w = 0.16f, h = 0.16f;
        int   note = 38, sound = 38;
        float volume = 0.8f;
        float sensitivity = 1.5f;   // expoente da curva de velocity (>1 = mais dinâmica)
        bool  enabled = true, mute = false;
        float flash = 0.0f;
        juce::File sampleFile;
    };

    //== Grade de quadrados ====================================================
    static constexpr float tileSize = 132.0f;
    static constexpr float tileGap  = 16.0f;
    static constexpr float gridMargin = 18.0f;

    int columns (float w) const
    {
        return juce::jmax (1, (int) ((w - 2 * gridMargin + tileGap) / (tileSize + tileGap)));
    }

    juce::Rectangle<float> tileBounds (int i, juce::Rectangle<float> area) const
    {
        const int cols = columns (area.getWidth());
        const int row = i / cols, col = i % cols;
        return { area.getX() + gridMargin + (float) col * (tileSize + tileGap),
                 area.getY() + gridMargin + (float) row * (tileSize + tileGap),
                 tileSize, tileSize };
    }

    static juce::Colour categoryColour (const juce::String& c)
    {
        if (c == "kick")    return juce::Colour (0xffe8503a);
        if (c == "snare")   return juce::Colour (0xfff2b134);
        if (c == "hihat")   return juce::Colour (0xff35c4d4);
        if (c == "tom")     return juce::Colour (0xff58c463);
        if (c == "crash" || c == "ride") return juce::Colour (0xff9b6cf2);
        if (c == "clap")    return juce::Colour (0xffe86fb0);
        if (c == "cowbell") return juce::Colour (0xffc9a14a);
        return juce::Colour (0xff7a8aa0); // perc / outros
    }

    /** Desenha um pad como quadrado (card) com o desenho da peça + nome. */
    void drawTile (juce::Graphics& g, int i, juce::Rectangle<float> area)
    {
        auto& p = pieces[(size_t) i];
        const auto r = tileBounds (i, area);
        const bool on = ! p.mute;   // mutado = "desligado" (mesma coisa)
        const bool sel = (i == selected);
        const auto cat = categoryColour (p.category);

        // Card.
        g.setColour (juce::Colour (on ? 0xff262732 : 0xff202026));
        g.fillRoundedRectangle (r, 12.0f);

        // Faixa de cor da categoria no topo.
        auto strip = r.withHeight (6.0f).reduced (10.0f, 0.0f).withY (r.getY() + 7.0f);
        g.setColour (cat.withAlpha (on ? 0.95f : 0.35f));
        g.fillRoundedRectangle (strip, 3.0f);

        // Ícone flat da categoria (recortado dentro do card).
        auto art = r.reduced (18.0f);
        art.removeFromTop (6.0f);
        art.removeFromBottom (24.0f); // espaço pro nome
        const auto iconCol = on ? cat.brighter (0.35f) : juce::Colour (0xff55555f);
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (r.toNearestInt());
            drawCategoryIcon (g, art, p.category, iconCol);

            // Triângulo de play transparente por cima (dica de "clique pra tocar").
            const float ps = art.getWidth() * 0.30f;
            const auto c = art.getCentre();
            juce::Path play;
            play.addTriangle (c.x - ps * 0.45f, c.y - ps * 0.6f,
                              c.x - ps * 0.45f, c.y + ps * 0.6f,
                              c.x + ps * 0.65f, c.y);
            g.setColour (juce::Colours::white.withAlpha (0.22f));
            g.fillPath (play);
        }

        // Flash colore o card inteiro.
        if (p.flash > 0.0f)
        {
            g.setColour (cat.withAlpha (0.35f * p.flash));
            g.fillRoundedRectangle (r, 12.0f);
        }

        // Nome.
        g.setColour (juce::Colours::white.withAlpha (on ? 0.92f : 0.4f));
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (p.name, r.withTop (r.getBottom() - 30.0f).withHeight (16.0f).toNearestInt(),
                    juce::Justification::centred, true);
        // Nota + mute.
        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.setFont (juce::FontOptions (10.5f));
        g.drawText ("nota " + juce::String (p.note) + (p.mute ? "  [MUTE]" : ""),
                    r.withTop (r.getBottom() - 15.0f).withHeight (13.0f).toNearestInt(),
                    juce::Justification::centred, false);

        // Borda + alças de seleção.
        if (sel)
        {
            g.setColour (juce::Colour (0xfff0a830));
            g.drawRoundedRectangle (r.reduced (1.0f), 12.0f, 2.5f);
            const float hs = 7.0f;
            for (auto corner : { r.getTopLeft(), r.getTopRight(), r.getBottomLeft(), r.getBottomRight() })
            {
                g.setColour (juce::Colour (0xff7da7ff));
                g.fillRect (juce::Rectangle<float> (hs, hs).withCentre (corner));
            }
        }
        else
        {
            g.setColour (juce::Colour (0xff34363f));
            g.drawRoundedRectangle (r.reduced (0.5f), 12.0f, 1.0f);
        }
    }

    bool noteInUse (int note) const
    {
        for (auto& p : pieces) if (p.note == note) return true;
        return false;
    }
    int nextFreeNote (int start) const
    {
        for (int n = start; n < 128; ++n) if (! noteInUse (n)) return n;
        for (int n = start - 1; n >= 0; --n) if (! noteInUse (n)) return n;
        return start;
    }
    /** Empurra (cx,cy) até não ficar em cima de nenhum pad existente. */
    void placeFree (float& cx, float& cy) const
    {
        const float minD = 0.11f;
        for (int attempt = 0; attempt < 60; ++attempt)
        {
            bool clash = false;
            for (auto& p : pieces)
            {
                const float dx = p.cx - cx, dy = p.cy - cy;
                if (dx * dx + dy * dy < minD * minD) { clash = true; break; }
            }
            if (! clash) return;
            cx += 0.09f;
            if (cx > 0.9f) { cx = 0.12f; cy += 0.13f; }
            if (cy > 0.9f) cy = 0.12f;
            cx = juce::jlimit (0.08f, 0.92f, cx);
            cy = juce::jlimit (0.08f, 0.92f, cy);
        }
    }

    juce::String uniqueName (const juce::String& cat) const
    {
        const auto base = displayNameForCategory (cat);
        int count = 0;
        for (auto& p : pieces) if (p.name.startsWith (base)) ++count;
        return count == 0 ? base : base + " " + juce::String (count + 1);
    }

    void triggerPiece (int i, float velocity)
    {
        auto& p = pieces[(size_t) i];
        engine.triggerNote (p.note, velocity);
        p.flash = juce::jmax (p.flash, 1.0f);
        repaint();
    }

    void applyPiece (const Piece& p)
    {
        engine.setPieceEnabled     (p.note, p.enabled);
        engine.setPieceSound       (p.note, p.sound);
        engine.setPieceGain        (p.note, p.volume);
        engine.setPieceMute        (p.note, p.mute);
        engine.setPieceSensitivity (p.note, p.sensitivity);
    }

    //== Ícones flat por categoria ============================================
    static void drawCategoryIcon (juce::Graphics& g, juce::Rectangle<float> b,
                                  const juce::String& cat, juce::Colour col)
    {
        const float cx = b.getCentreX(), cy = b.getCentreY();
        const float u  = juce::jmin (b.getWidth(), b.getHeight());
        const float t  = juce::jmax (2.0f, u * 0.055f);
        g.setColour (col);

        auto ring = [&] (float ecx, float ecy, float ew, float eh, float th)
        { g.drawEllipse (ecx - ew * 0.5f, ecy - eh * 0.5f, ew, eh, th); };

        auto disc = [&] (float ecx, float ecy, float d)
        { g.fillEllipse (ecx - d * 0.5f, ecy - d * 0.5f, d, d); };

        // Cilindro de tambor (topo elíptico + corpo) — usado por snare/tom/perc.
        auto drumBody = [&] (float w, float h, float lift)
        {
            const float ry = w * 0.16f;
            const float top = cy - h * 0.5f + lift, bot = cy + h * 0.5f + lift;
            ring (cx, top, w, ry * 2.0f, t);                       // pele
            g.drawLine (cx - w * 0.5f, top, cx - w * 0.5f, bot, t); // laterais
            g.drawLine (cx + w * 0.5f, top, cx + w * 0.5f, bot, t);
            juce::Path arc;                                         // fundo
            arc.addArc (cx - w * 0.5f, bot - ry, w, ry * 2.0f, 0.0f, juce::MathConstants<float>::pi, true);
            g.strokePath (arc, juce::PathStrokeType (t));
        };

        // Prato (elipse fina) com inclinação opcional + haste.
        auto cymbal = [&] (float w, float angleDeg, bool bell)
        {
            juce::Path p;
            p.addEllipse (cx - w * 0.5f, cy - w * 0.11f, w, w * 0.22f);
            p.applyTransform (juce::AffineTransform::rotation (
                juce::degreesToRadians (angleDeg), cx, cy));
            g.strokePath (p, juce::PathStrokeType (t));
            g.drawLine (cx, cy, cx, cy + u * 0.42f, t * 0.8f);     // haste
            if (bell) disc (cx, cy, u * 0.12f);
        };

        if (cat == "kick")
        {
            const float R = u * 0.40f;
            ring (cx, cy, 2 * R, 2 * R, t);
            ring (cx, cy, u * 0.30f, u * 0.30f, t * 0.8f);
            g.drawLine (cx - R * 0.6f, cy + R * 0.85f, cx - R * 1.05f, cy + R * 1.15f, t);
            g.drawLine (cx + R * 0.6f, cy + R * 0.85f, cx + R * 1.05f, cy + R * 1.15f, t);
        }
        else if (cat == "snare") { drumBody (u * 0.74f, u * 0.34f, 0.0f); }
        else if (cat == "tom")   { drumBody (u * 0.62f, u * 0.50f, 0.0f); }
        else if (cat == "perc")  { disc (cx - u * 0.18f, cy, u * 0.34f); ring (cx + u * 0.20f, cy, u * 0.30f, u * 0.30f, t); }
        else if (cat == "hihat")
        {
            ring (cx, cy - u * 0.06f, u * 0.66f, u * 0.16f, t);    // prato de cima
            ring (cx, cy + u * 0.06f, u * 0.66f, u * 0.16f, t);    // prato de baixo
            g.drawLine (cx, cy + u * 0.13f, cx, cy + u * 0.45f, t * 0.8f);
        }
        else if (cat == "crash") { cymbal (u * 0.74f, -22.0f, false); }
        else if (cat == "ride")  { cymbal (u * 0.74f, 0.0f, true); }
        else if (cat == "cowbell")
        {
            juce::Path p;                                          // trapézio
            p.startNewSubPath (cx - u * 0.10f, cy - u * 0.28f);
            p.lineTo (cx + u * 0.10f, cy - u * 0.28f);
            p.lineTo (cx + u * 0.22f, cy + u * 0.30f);
            p.lineTo (cx - u * 0.22f, cy + u * 0.30f);
            p.closeSubPath();
            g.strokePath (p, juce::PathStrokeType (t));
        }
        else if (cat == "clap")
        {
            juce::Path p;                                          // triângulo (palma/impacto)
            p.addTriangle (cx, cy - u * 0.30f, cx - u * 0.30f, cy + u * 0.26f, cx + u * 0.30f, cy + u * 0.26f);
            g.strokePath (p, juce::PathStrokeType (t));
            for (float a : { -0.6f, 0.0f, 0.6f })                  // faíscas
                g.drawLine (cx + a * u * 0.4f, cy - u * 0.42f, cx + a * u * 0.4f, cy - u * 0.34f, t * 0.6f);
        }
        else { ring (cx, cy, u * 0.5f, u * 0.5f, t); }             // genérico
    }

    //== Desenho das peças (legado, mantido para referência) ==================
    static void drawDrum (juce::Graphics& g, juce::Rectangle<float> r, float flash, bool on, bool /*sel*/)
    {
        const float depth = r.getHeight() * 0.38f;
        const juce::Colour shell = on ? juce::Colour (0xff7a3326) : juce::Colour (0xff343036);

        // Corpo (casca) com profundidade.
        juce::Path body;
        body.addRoundedRectangle (r.getX(), r.getCentreY(), r.getWidth(), depth, 4.0f);
        g.setColour (shell.darker (0.6f));
        g.fillPath (body);
        g.setColour (shell.darker (0.9f));
        g.fillEllipse (r.translated (0.0f, depth));

        // Pele (head).
        juce::ColourGradient head (juce::Colour (0xfff3ede0), r.getCentreX(), r.getY(),
                                   juce::Colour (0xffb9b2a4), r.getCentreX(), r.getBottom(), false);
        g.setGradientFill (head);
        g.fillEllipse (r);
        if (! on) { g.setColour (juce::Colour (0x99202026)); g.fillEllipse (r); }

        // Aro metálico.
        g.setColour (juce::Colour (0xffc8c8d0));
        g.drawEllipse (r, 2.2f);

        // Brilho do hit.
        if (flash > 0.0f)
        {
            g.setColour (juce::Colour (0xfff5d27a).withAlpha (flash));
            g.drawEllipse (r.expanded (2.0f + 5.0f * flash), 2.0f + 4.0f * flash);
            g.setColour (juce::Colours::white.withAlpha (0.25f * flash));
            g.fillEllipse (r);
        }
    }

    static void drawKick (juce::Graphics& g, juce::Rectangle<float> r, float flash, bool on, bool /*sel*/)
    {
        const juce::Colour wood = on ? juce::Colour (0xff7a3326) : juce::Colour (0xff343036);
        // Aro de madeira.
        g.setColour (wood.darker (0.2f));
        g.fillEllipse (r);
        // Pele frontal.
        auto inner = r.reduced (r.getWidth() * 0.09f);
        juce::ColourGradient head (juce::Colour (0xfff6f0e4), inner.getCentreX(), inner.getY(),
                                   juce::Colour (0xffc1baac), inner.getCentreX(), inner.getBottom(), false);
        g.setGradientFill (head);
        g.fillEllipse (inner);
        if (! on) { g.setColour (juce::Colour (0x99202026)); g.fillEllipse (inner); }
        g.setColour (juce::Colour (0xffd8d2c4));
        g.drawEllipse (inner, 2.0f);

        if (flash > 0.0f)
        {
            g.setColour (juce::Colour (0xfff5d27a).withAlpha (flash));
            g.drawEllipse (r.expanded (2.0f + 6.0f * flash), 2.5f + 5.0f * flash);
            g.setColour (juce::Colours::white.withAlpha (0.22f * flash));
            g.fillEllipse (inner);
        }
    }

    static void drawCymbal (juce::Graphics& g, juce::Rectangle<float> r, float flash, bool on)
    {
        const float cx = r.getCentreX(), cy = r.getCentreY();
        // Haste.
        g.setColour (juce::Colour (0xff5a5c64));
        g.fillRect (cx - 1.5f, cy, 3.0f, r.getHeight() * 1.4f);

        // Prato com gradiente radial dourado.
        const juce::Colour gold1 = on ? juce::Colour (0xffe9c069) : juce::Colour (0xff5b5650);
        const juce::Colour gold2 = on ? juce::Colour (0xff8a6a2e) : juce::Colour (0xff3a3630);
        juce::ColourGradient grad (gold1, cx, cy, gold2, r.getX(), cy, true);
        g.setGradientFill (grad);
        g.fillEllipse (r);

        // Sulcos (lathe).
        g.setColour (gold2.withAlpha (0.5f));
        for (float f = 0.3f; f < 1.0f; f += 0.22f)
            g.drawEllipse (r.withSizeKeepingCentre (r.getWidth() * f, r.getHeight() * f), 1.0f);

        // Bell central.
        auto bell = r.withSizeKeepingCentre (r.getWidth() * 0.18f, r.getHeight() * 0.5f);
        g.setColour (gold1.brighter (0.2f));
        g.fillEllipse (bell);

        if (flash > 0.0f)
        {
            g.setColour (juce::Colour (0xfffff0b0).withAlpha (flash));
            g.drawEllipse (r.expanded (2.0f + 5.0f * flash), 2.0f + 3.0f * flash);
            g.setColour (juce::Colours::white.withAlpha (0.35f * flash));
            g.fillEllipse (r);
        }
    }

    AudioEngine& engine;
    std::vector<Piece> pieces;
    int selected = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumKitView)
};

//==============================================================================
/** Quadrado de preview do pad selecionado (ícone da categoria), no topo do
    inspector — a "caixinha" daquele pad. */
class PadPreview final : public juce::Component
{
public:
    PadPreview() = default;

    std::function<void()> onToggleMute;   // clicar no ícone muta/desmuta

    void set (const juce::String& cat, bool muted) { category = cat; mute = muted; repaint(); }
    void clear() { category = {}; repaint(); }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (category.isNotEmpty() && onToggleMute) onToggleMute();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float s = juce::jmin (r.getWidth(), r.getHeight());
        auto card = juce::Rectangle<float> (s, s).withCentre (r.getCentre());

        g.setColour (juce::Colour (0xff262732));
        g.fillRoundedRectangle (card, 12.0f);
        g.setColour (mute ? juce::Colour (0xffe8503a) : juce::Colour (0xff3a3c46));
        g.drawRoundedRectangle (card, 12.0f, mute ? 2.0f : 1.0f);

        if (category.isEmpty()) return;
        auto art = card.reduced (card.getWidth() * 0.20f);
        const auto col = mute ? juce::Colour (0xff55555f)
                              : DrumKitView::colourForCategory (category).brighter (0.35f);
        DrumKitView::paintIcon (g, art, category, col);

        // Indicador de mute: barra diagonal vermelha sobre o ícone.
        if (mute)
        {
            g.setColour (juce::Colour (0xffe8503a).withAlpha (0.9f));
            g.drawLine (art.getX(), art.getY(), art.getRight(), art.getBottom(), 3.0f);
        }

        // Dica.
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.setFont (juce::FontOptions (10.0f));
        g.drawText (mute ? "MUTADO (clique p/ ativar)" : "clique p/ mutar",
                    getLocalBounds().removeFromBottom (14), juce::Justification::centred, false);
    }

private:
    juce::String category;
    bool mute = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadPreview)
};

//==============================================================================
/** Painel de configuração do HARDWARE (os 6 pads físicos do Arduino A0-A5).
    Edita sensibilidade/threshold/scantime/masktime/nota/curva/ativo e ENVIA
    pro Arduino pela serial (que aplica e salva na EEPROM). */
class HardwareConfigPanel final : public juce::Component
{
public:
    static constexpr int NP = 6;
    using HwArray = int[NP][7];

    HardwareConfigPanel (HwArray& cfg, AudioEngine& eng, std::function<void()> onChanged)
        : hw (cfg), engine (eng), changed (std::move (onChanged))
    {
        static const char* padNames[NP] =
            { "Caixa (A0)", "Bumbo (A1)", "Chimbal (A2)", "Tom 1 (A3)", "Tom 2 (A4)", "Crash (A5)" };
        static const char* heads[7] = { "Sens", "Thresh", "Scan", "Mask", "Nota", "Curva", "Ativo" };

        title.setText ("Config do Hardware (Arduino)  -  enviado pela serial, salvo na EEPROM",
                       juce::dontSendNotification);
        title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        addAndMakeVisible (title);

        for (int k = 0; k < 7; ++k)
        {
            head[k].setText (heads[k], juce::dontSendNotification);
            head[k].setFont (juce::FontOptions (11.0f, juce::Font::bold));
            head[k].setJustificationType (juce::Justification::centred);
            addAndMakeVisible (head[k]);
        }

        for (int p = 0; p < NP; ++p)
        {
            name[p].setText (padNames[p], juce::dontSendNotification);
            name[p].setFont (juce::FontOptions (12.0f, juce::Font::bold));
            addAndMakeVisible (name[p]);

            setupSlider (sens[p],   0, 255, hw[p][0], p);
            setupSlider (thresh[p], 0, 255, hw[p][1], p);
            setupSlider (scan[p],   0, 100, hw[p][2], p);
            setupSlider (mask[p],   0, 100, hw[p][3], p);
            setupSlider (note[p],   0, 127, hw[p][4], p);
            setupSlider (curve[p],  0,   4, hw[p][5], p);

            enable[p].setToggleState (hw[p][6] != 0, juce::dontSendNotification);
            enable[p].onClick = [this, p] { onPadChanged (p); };
            addAndMakeVisible (enable[p]);
        }

        setSize (640, 320);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        title.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);
        const int nameW = 110;
        const int colW = (r.getWidth() - nameW) / 7;

        auto hdr = r.removeFromTop (18);
        hdr.removeFromLeft (nameW);
        for (int k = 0; k < 7; ++k) head[k].setBounds (hdr.removeFromLeft (colW));

        for (int p = 0; p < NP; ++p)
        {
            auto row = r.removeFromTop (38);
            name[p].setBounds (row.removeFromLeft (nameW));
            sens[p]  .setBounds (row.removeFromLeft (colW).reduced (2));
            thresh[p].setBounds (row.removeFromLeft (colW).reduced (2));
            scan[p]  .setBounds (row.removeFromLeft (colW).reduced (2));
            mask[p]  .setBounds (row.removeFromLeft (colW).reduced (2));
            note[p]  .setBounds (row.removeFromLeft (colW).reduced (2));
            curve[p] .setBounds (row.removeFromLeft (colW).reduced (2));
            enable[p].setBounds (row.removeFromLeft (colW).withSizeKeepingCentre (24, 24));
        }
    }

private:
    void setupSlider (juce::Slider& s, int lo, int hi, int val, int pad)
    {
        s.setSliderStyle (juce::Slider::IncDecButtons);
        s.setRange (lo, hi, 1);
        s.setValue (val, juce::dontSendNotification);
        s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 38, 20);
        s.onValueChange = [this, pad] { onPadChanged (pad); };
        addAndMakeVisible (s);
    }

    void onPadChanged (int p)
    {
        hw[p][0] = (int) sens[p].getValue();
        hw[p][1] = (int) thresh[p].getValue();
        hw[p][2] = (int) scan[p].getValue();
        hw[p][3] = (int) mask[p].getValue();
        hw[p][4] = (int) note[p].getValue();
        hw[p][5] = (int) curve[p].getValue();
        hw[p][6] = enable[p].getToggleState() ? 1 : 0;
        engine.sendPadConfig (p, hw[p][0], hw[p][1], hw[p][2], hw[p][3], hw[p][4], hw[p][5], hw[p][6]);
        if (changed) changed();
    }

    HwArray& hw;
    AudioEngine& engine;
    std::function<void()> changed;

    juce::Label title, head[7], name[NP];
    juce::Slider sens[NP], thresh[NP], scan[NP], mask[NP], note[NP], curve[NP];
    juce::ToggleButton enable[NP];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardwareConfigPanel)
};

//==============================================================================
class MainComponent final : public juce::Component,
                            private juce::Timer
{
public:
    MainComponent() : kitView (engine)
    {
        setLookAndFeel (&lnf);

        // --- Inspector global ---
        for (int i = 0; i < kNumKits; ++i) kitBox.addItem (kKits[i].name, i + 1);
        kitBox.setSelectedId (1, juce::dontSendNotification);
        kitBox.onChange = [this] { engine.setKit (kitBox.getSelectedId() - 1); configDirty = true; };
        addAndMakeVisible (kitBox);

        audioBtn.setButtonText ("Audio / Latencia");
        audioBtn.onClick = [this] { openAudioSettings(); };
        addAndMakeVisible (audioBtn);

        hwBtn.setButtonText ("Config Hardware");
        hwBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a4a6a));
        hwBtn.onClick = [this] { openHardwareConfig(); };
        addAndMakeVisible (hwBtn);

        setupSlider (master, 0.0, 1.0, 0.001, 0.8);
        master.onValueChange = [this] { engine.setMasterGain ((float) master.getValue()); configDirty = true; };
        // 'sens' agora é POR-PEÇA (sensibilidade/resposta da velocity do pad selecionado).
        // Expoente da curva de velocity: <1 comprime (tudo soa igual), >1 expande
        // (forte MUITO mais alto que fraco). 1.5 = dinâmica clara; 3.0 = bem dramático.
        setupSlider (sens, 0.3, 3.0, 0.01, 1.5);
        sens.onValueChange = [this] { onPieceEdit(); };

        makeLabel (kitLbl, "Kit");
        makeLabel (masterLbl, "Vol");
        makeLabel (sensLbl, "Dinamica (fraco <-> forte)");
        makeLabel (pieceHdr, "PECA SELECIONADA");
        pieceHdr.setColour (juce::Label::textColourId, juce::Colour (0xfff0a830));

        // --- Adicionar / remover pads ---
        addBtn.setButtonText ("+ Pad");
        addBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a5a3a));
        addBtn.onClick = [this] { showAddMenu(); };
        addAndMakeVisible (addBtn);

        removeBtn.setButtonText ("- Pad");
        removeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff5a3a3a));
        removeBtn.onClick = [this] { removePad(); };
        addAndMakeVisible (removeBtn);

        clearKitBtn.setButtonText ("Limpar kit (esvaziar)");
        clearKitBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff4a3030));
        clearKitBtn.onClick = [this] { kitView.clearAll(); refreshInspector(); configDirty = true; };
        addAndMakeVisible (clearKitBtn);

        // --- Inspector da peça selecionada ---
        padPreview.onToggleMute = [this] { toggleSelectedMute(); };

        makeLabel (nameLbl, "Nome");
        nameEdit.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        nameEdit.onReturnKey = [this] { commitName(); };
        nameEdit.onFocusLost = [this] { commitName(); };
        addAndMakeVisible (nameEdit);

        makeLabel (catLbl, "Categoria (tipo da peca)");
        for (int i = 0; i < kNumCategories; ++i)
            catBox.addItem (displayNameForCategory (kCategories[i]) + " (" + kCategories[i] + ")", i + 1);
        catBox.onChange = [this] { onCategoryChanged(); };
        addAndMakeVisible (catBox);

        makeLabel (noteLbl, "Nota MIDI (do pad)");
        noteSlider.setSliderStyle (juce::Slider::IncDecButtons);
        noteSlider.setRange (0, 127, 1);
        noteSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 56, 22);
        noteSlider.onValueChange = [this] { onPieceEdit(); };
        addAndMakeVisible (noteSlider);

        makeLabel (soundLbl, "Sample (som da peca)");
        soundBox.onChange = [this] { onSampleChanged(); };
        addAndMakeVisible (soundBox);

        makeLabel (volLbl, "Volume da peca");
        setupSlider (pieceVol, 0.0, 1.0, 0.01, 0.8);
        pieceVol.onValueChange = [this] { onPieceEdit(); };

        samplesBtn.setButtonText ("Carregar samples (pasta WAV)...");
        samplesBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a5a3a));
        samplesBtn.onClick = [this] { chooseSamplesFolder(); };
        addAndMakeVisible (samplesBtn);

        // --- Monitor MIDI ---
        makeLabel (monitorLbl, "MONITOR MIDI");
        clearBtn.setButtonText ("Limpar");
        clearBtn.onClick = [this] { monitor.clear(); };
        addAndMakeVisible (clearBtn);

        copyBtn.setButtonText ("Copiar");
        copyBtn.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (monitor.getAllText()); };
        addAndMakeVisible (copyBtn);
        addAndMakeVisible (monitor);

        // --- Kit view ---
        kitView.onSelectionChanged = [this] { refreshInspector(); };
        kitView.onChanged = [this] { configDirty = true; };
        addAndMakeVisible (kitView);

        // --- Sobe o motor (porta serial real FTDI 115200, MIDI) ---
        auto src = std::make_unique<PosixSerialSource> ("/dev/cu.usbserial-A5069RR4", 115200);
        engineReady = engine.initialise (std::move (src));
        engine.setMasterGain ((float) master.getValue());

        // Biblioteca de samples reais: ./samples/library/<categoria>/*.wav
        if (auto dir = findSamplesDir(); dir.isDirectory())
        {
            scanLibrary (dir);
            // Default: primeiro sample de cada categoria por peça.
            for (int i = 0; i < kitView.getNumPieces(); ++i)
            {
                const auto cat = kitView.pieceCategory (i);
                if (auto it = library.find (cat); it != library.end() && ! it->second.isEmpty())
                    kitView.setPieceSampleOnly (i, it->second.getFirst());
            }
        }
        kitView.applyAllToEngine();   // aplica atomics + samples (1 rebuild)

        loadConfig();                 // sobrescreve com a config salva
        // NÃO sincroniza hardware no startup: abrir a porta RESETA o Arduino (DTR),
        // e disparar comandos 'C' durante o boot corrompe a EEPROM (config bagunçada,
        // pads re-habilitados, ruído). A EEPROM da placa é a fonte da verdade; ela
        // persiste sozinha. A config só é enviada quando o usuário mexe no painel
        // Config Hardware (sendPadConfig ao vivo, fora da janela de reset).

        // Inspector ROLÁVEL: move os controles para um content dentro de um Viewport,
        // pra nenhuma linha (ex.: Sensibilidade) ser cortada se a janela for baixa.
        // Só os controles POR-PAD vão pro inspector rolável; os globais ficam na barra de topo.
        juce::Component* inspCtrls[] = {
            &pieceHdr, &padPreview, &addBtn, &removeBtn, &clearKitBtn, &nameLbl, &nameEdit,
            &catLbl, &catBox, &noteLbl, &noteSlider, &soundLbl, &soundBox,
            &volLbl, &pieceVol, &sensLbl, &sens
        };
        for (juce::Component* c : inspCtrls) inspectorContent.addAndMakeVisible (c);
        inspectorView.setViewedComponent (&inspectorContent, false);
        inspectorView.setScrollBarsShown (true, false);
        addAndMakeVisible (inspectorView);

        refreshInspector();

        std::fill (std::begin (lastCount), std::end (lastCount), 0u);
        appStartMs = juce::Time::getMillisecondCounter();

        setSize (1120, 720);
        startTimerHz (60);
    }

    ~MainComponent() override
    {
        if (configDirty) saveConfig();
        engine.shutdown();
        setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff17181c));
        // Barra de topo (configs globais).
        g.setColour (juce::Colour (0xff1f2026));
        g.fillRect (getLocalBounds().removeFromTop (topBarHeight));
        g.setColour (juce::Colour (0xff34363f));
        g.drawHorizontalLine (topBarHeight, 0.0f, (float) getWidth());
        // Painel inspector (à direita).
        auto insp = getLocalBounds();
        insp.removeFromTop (topBarHeight);
        insp.removeFromBottom (monitorHeight);
        g.setColour (juce::Colour (0xff1b1c22));
        g.fillRect (insp.removeFromRight (inspectorWidth));
        g.setColour (juce::Colour (0xff34363f));
        g.drawVerticalLine (getWidth() - inspectorWidth, (float) topBarHeight, (float) (getHeight() - monitorHeight));
    }

    void resized() override
    {
        auto area = getLocalBounds();

        // --- Barra de topo: configs globais (Kit, Volume Master, Audio, Samples) ---
        auto bar = area.removeFromTop (topBarHeight).reduced (10, 7);
        kitLbl.setBounds (bar.removeFromLeft (28));
        kitBox.setBounds (bar.removeFromLeft (150)); bar.removeFromLeft (14);
        audioBtn.setBounds (bar.removeFromLeft (130)); bar.removeFromLeft (8);
        hwBtn.setBounds (bar.removeFromLeft (130)); bar.removeFromLeft (8);
        samplesBtn.setBounds (bar.removeFromLeft (190)); bar.removeFromLeft (14);
        masterLbl.setBounds (bar.removeFromLeft (44));
        master.setBounds (bar.removeFromLeft (juce::jmin (260, bar.getWidth())));

        // Monitor embaixo.
        auto mon = area.removeFromBottom (monitorHeight).reduced (16, 8);
        auto monHeader = mon.removeFromTop (20);
        monitorLbl.setBounds (monHeader.removeFromLeft (160));
        clearBtn.setBounds (monHeader.removeFromRight (78).withHeight (20));
        monHeader.removeFromRight (6);
        copyBtn.setBounds (monHeader.removeFromRight (78).withHeight (20));
        monitor.setBounds (mon);

        // Inspector à direita (rola SÓ se precisar; altura do conteúdo = a real).
        auto insp = area.removeFromRight (inspectorWidth);
        inspectorView.setBounds (insp);
        const int margin = 12, sbW = 10;                 // reserva fixa p/ a barra de rolagem
        const int contentW = insp.getWidth() - sbW;
        const int innerW = contentW - margin * 2;
        const int used = layoutInspector (juce::Rectangle<int> (margin, margin, innerW, 4000));
        const int contentH = juce::jmax (used + margin * 2, insp.getHeight());
        inspectorContent.setSize (contentW, contentH);   // == viewH quando cabe -> sem scroll

        // Kit no centro.
        kitView.setBounds (area.reduced (10));
    }

private:
    static constexpr int topBarHeight = 44;
    static constexpr int inspectorWidth = 270;
    static constexpr int monitorHeight = 150;

    void makeLabel (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        l.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa6));
        addAndMakeVisible (l);
    }

    void setupSlider (juce::Slider& s, double lo, double hi, double step, double val)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setRange (lo, hi, step);
        s.setValue (val, juce::dontSendNotification);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
        addAndMakeVisible (s);
    }

    /** Posiciona os controles do inspector. @returns altura total usada (em px). */
    int layoutInspector (juce::Rectangle<int> r)
    {
        const int top = r.getY();
        const int rowH = 24, gap = 6;
        auto row = [&] (int h = rowH) { auto x = r.removeFromTop (h); r.removeFromTop (gap); return x; };

        pieceHdr.setBounds (row (18));
        padPreview.setBounds (row (118));
        auto pad = row (26);
        addBtn.setBounds (pad.removeFromLeft (pad.getWidth() / 2 - 4));
        removeBtn.setBounds (pad.removeFromRight (pad.getWidth() - 4));
        clearKitBtn.setBounds (row (22));
        nameLbl.setBounds (row (16));
        nameEdit.setBounds (row());
        catLbl.setBounds (row (16));
        catBox.setBounds (row());
        noteLbl.setBounds (row (16));
        noteSlider.setBounds (row());
        soundLbl.setBounds (row (16));
        soundBox.setBounds (row());
        volLbl.setBounds (row (16));
        pieceVol.setBounds (row());
        sensLbl.setBounds (row (16));
        sens.setBounds (row());
        return r.getY() - top; // altura consumida desde o topo
    }

    void setPieceControlsEnabled (bool on)
    {
        juce::Component* ctrls[] = { &nameEdit, &catBox, &noteSlider, &soundBox, &pieceVol,
                                     &sens, &removeBtn };
        for (juce::Component* c : ctrls) c->setEnabled (on);
    }

    void refreshInspector()
    {
        const int n = kitView.getNumPieces();
        const int i = kitView.getSelected();
        setPieceControlsEnabled (n > 0);
        if (n == 0 || i < 0 || i >= n)
        {
            nameEdit.setText ("", juce::dontSendNotification);
            soundBox.clear (juce::dontSendNotification);
            soundLbl.setText ("Sample", juce::dontSendNotification);
            pieceHdr.setText ("ADICIONE UM PAD (+)", juce::dontSendNotification);
            padPreview.clear();
            return;
        }
        pieceHdr.setText ("PECA SELECIONADA", juce::dontSendNotification);
        padPreview.set (kitView.pieceCategory (i), kitView.pieceMute (i));

        nameEdit.setText (kitView.pieceName (i), juce::dontSendNotification);
        // Categoria.
        for (int k = 0; k < kNumCategories; ++k)
            if (kitView.pieceCategory (i) == kCategories[k]) { catBox.setSelectedId (k + 1, juce::dontSendNotification); break; }
        noteSlider.setValue (kitView.pieceNote (i), juce::dontSendNotification);
        pieceVol.setValue (kitView.pieceVol (i), juce::dontSendNotification);
        sens.setValue (kitView.pieceSensitivity (i), juce::dontSendNotification);

        // Dropdown de Sample: SÓ os arquivos da categoria desta peça + "Sintetizado".
        soundBox.clear (juce::dontSendNotification);
        soundBox.addItem ("Sintetizado (sem sample)", 1);
        const auto cat = kitView.pieceCategory (i);
        const auto cur = kitView.pieceSampleFile (i);
        int selId = 1, count = 0;
        if (auto it = library.find (cat); it != library.end())
        {
            count = it->second.size();
            for (int k = 0; k < it->second.size(); ++k)
            {
                soundBox.addItem (prettySampleName (it->second[k]), k + 2);
                if (it->second[k] == cur) selId = k + 2;
            }
        }
        soundBox.setSelectedId (selId, juce::dontSendNotification);
        // Rótulo deixa explícito que a lista é só daquela categoria.
        soundLbl.setText ("Sample - " + displayNameForCategory (cat) + " (" + juce::String (count) + ")",
                          juce::dontSendNotification);
    }

    /** Nome de sample legível: tira underscores e capitaliza. */
    static juce::String prettySampleName (const juce::File& f)
    {
        auto s = f.getFileNameWithoutExtension().replaceCharacter ('_', ' ');
        return s.isEmpty() ? s : s.substring (0, 1).toUpperCase() + s.substring (1);
    }

    /** Slider de nota / volume / enable / mute mudou. */
    void onPieceEdit()
    {
        const int i = kitView.getSelected();
        if (i < 0 || i >= kitView.getNumPieces()) return;
        const int note = (int) noteSlider.getValue();
        // 'enabled' sempre true; mute preservado (toggle fica no ícone do pad).
        kitView.setPieceConfig (i, note, note, (float) pieceVol.getValue(),
                                (float) sens.getValue(),
                                true, kitView.pieceMute (i));
        configDirty = true;
    }

    /** Alterna o mute do pad selecionado (clique no ícone do preview). */
    void toggleSelectedMute()
    {
        const int i = kitView.getSelected();
        if (i < 0 || i >= kitView.getNumPieces()) return;
        kitView.setPieceConfig (i, kitView.pieceNote (i), kitView.pieceNote (i),
                                kitView.pieceVol (i), kitView.pieceSensitivity (i),
                                true, ! kitView.pieceMute (i));
        refreshInspector();
        configDirty = true;
    }

    void commitName()
    {
        const int i = kitView.getSelected();
        if (i >= 0 && i < kitView.getNumPieces() && nameEdit.getText().isNotEmpty())
        {
            kitView.setPieceName (i, nameEdit.getText());
            configDirty = true;
        }
    }

    void onCategoryChanged()
    {
        const int i = kitView.getSelected();
        if (i < 0 || i >= kitView.getNumPieces()) return;
        const juce::String cat = kCategories[catBox.getSelectedId() - 1];
        kitView.setPieceCategory (i, cat);
        kitView.setPieceSample (i, defaultSampleFor (cat)); // sample default da nova categoria
        refreshInspector();
        configDirty = true;
    }

    void showAddMenu()
    {
        juce::PopupMenu m;
        for (int k = 0; k < kNumCategories; ++k)
            m.addItem (k + 1, displayNameForCategory (kCategories[k]) + " (" + kCategories[k] + ")");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addBtn),
            [this] (int r)
            {
                if (r <= 0) return;
                const juce::String cat = kCategories[r - 1];
                const int idx = kitView.addPiece (cat);
                kitView.setPieceSample (idx, defaultSampleFor (cat));
                refreshInspector();
                configDirty = true;
            });
    }

    void removePad()
    {
        kitView.removeSelected();
        refreshInspector();
        configDirty = true;
    }

    juce::File defaultSampleFor (const juce::String& cat) const
    {
        if (auto it = library.find (cat); it != library.end() && ! it->second.isEmpty())
            return it->second.getFirst();
        return {};
    }

    /** Usuário escolheu um sample (ou "Sintetizado") no dropdown. */
    void onSampleChanged()
    {
        const int i = kitView.getSelected();
        if (i < 0 || i >= kitView.getNumPieces()) return;
        const int id = soundBox.getSelectedId();
        if (id <= 1)
        {
            kitView.setPieceSample (i, juce::File()); // synth
        }
        else if (auto it = library.find (kitView.pieceCategory (i)); it != library.end())
        {
            const int idx = id - 2;
            if (idx >= 0 && idx < it->second.size())
                kitView.setPieceSample (i, it->second[idx]);
        }
        configDirty = true;
    }

    //== Biblioteca de samples =================================================
    void scanLibrary (const juce::File& samplesDir)
    {
        library.clear();
        libraryDir = samplesDir.getChildFile ("library");
        if (! libraryDir.isDirectory()) return;

        juce::Array<juce::File> cats;
        libraryDir.findChildFiles (cats, juce::File::findDirectories, false);
        for (auto& cat : cats)
        {
            juce::Array<juce::File> files;
            cat.findChildFiles (files, juce::File::findFiles, false, "*.wav");
            std::sort (files.begin(), files.end(),
                       [] (const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });
            if (! files.isEmpty())
                library[cat.getFileName()] = files;
        }
    }

    /** Qual pad físico (A0-A5) emitiu esta nota. A nota é só o ID de roteamento
        pad->software; o que importa pro usuário é o pad. -1 = nota sem pad mapeado. */
    int padForNote (int note) const
    {
        for (int p = 0; p < 6; ++p) if (hw[p][4] == note && hw[p][6]) return p;
        for (int p = 0; p < 6; ++p) if (hw[p][4] == note)             return p;
        return -1;
    }

    static const char* padName (int p)
    {
        static const char* n[6] = { "caixa", "bumbo", "chimbal", "tom1", "tom2", "crash" };
        return juce::isPositiveAndBelow (p, 6) ? n[p] : "?";
    }

    void openAudioSettings()
    {
        auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
            engine.getDeviceManager(), 0, 0, 1, 2, false, false, true, false);
        selector->setSize (520, 380);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (selector.release());
        o.dialogTitle = "Configuracoes de Audio";
        o.dialogBackgroundColour = juce::Colour (0xff1a1a1f);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }

    void openHardwareConfig()
    {
        auto panel = std::make_unique<HardwareConfigPanel> (hw, engine, [this] { configDirty = true; });
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (panel.release());
        o.dialogTitle = "Config Hardware (Arduino)";
        o.dialogBackgroundColour = juce::Colour (0xff1a1a1f);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }

    /** Envia toda a config de hardware salva pro Arduino (sync no startup). */
    void syncHardwareToArduino()
    {
        for (int p = 0; p < 6; ++p)
            engine.sendPadConfig (p, hw[p][0], hw[p][1], hw[p][2], hw[p][3], hw[p][4], hw[p][5], hw[p][6]);
    }

    //== Samples reais (WAV) ===================================================
    /** Procura uma pasta "samples" no projeto/bundle (subindo a partir do .exe). */
    static juce::File findSamplesDir()
    {
        auto d = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        for (int i = 0; i < 12 && d.exists(); ++i)
        {
            auto cand = d.getChildFile ("samples");
            if (cand.isDirectory()) return cand;
            auto parent = d.getParentDirectory();
            if (parent == d) break;
            d = parent;
        }
        // Fallback: pasta do usuário.
        auto user = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                        .getChildFile ("SomBateria").getChildFile ("samples");
        return user.isDirectory() ? user : juce::File();
    }

    void chooseSamplesFolder()
    {
        chooser = std::make_unique<juce::FileChooser> (
            "Escolha a pasta 'samples' (que contem a pasta library/)",
            juce::File::getSpecialLocation (juce::File::userHomeDirectory));
        chooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                auto dir = fc.getResult();
                if (! dir.isDirectory()) return;
                scanLibrary (dir);
                // Reaplica os defaults onde a peça nao tem sample valido.
                for (int i = 0; i < kitView.getNumPieces(); ++i)
                {
                    if (kitView.pieceSampleFile (i).existsAsFile()) continue;
                    const auto cat = kitView.pieceCategory (i);
                    if (auto it = library.find (cat); it != library.end() && ! it->second.isEmpty())
                        kitView.setPieceSampleOnly (i, it->second.getFirst());
                }
                kitView.applyAllToEngine();
                refreshInspector();
                samplesBtn.setButtonText ("Biblioteca recarregada");
            });
    }

    //== Persistência ==========================================================
    juce::File configFile() const
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("SomBateria").getChildFile ("config.json");
    }

    void saveConfig()
    {
        auto* root = new juce::DynamicObject();
        root->setProperty ("kit", kitBox.getSelectedId() - 1);
        root->setProperty ("masterGain", master.getValue());

        juce::Array<juce::var> arr;
        for (int i = 0; i < kitView.getNumPieces(); ++i)
        {
            auto* p = new juce::DynamicObject();
            p->setProperty ("name",     kitView.pieceName (i));
            p->setProperty ("category", kitView.pieceCategory (i));
            p->setProperty ("cx",       kitView.pieceCX (i));
            p->setProperty ("cy",       kitView.pieceCY (i));
            p->setProperty ("note",     kitView.pieceNote (i));
            p->setProperty ("volume",   kitView.pieceVol (i));
            p->setProperty ("sens",     kitView.pieceSensitivity (i));
            p->setProperty ("enabled",  kitView.pieceEnabled (i));
            p->setProperty ("mute",     kitView.pieceMute (i));
            // Sample como caminho relativo à biblioteca ("crash/bright.wav"); "" = synth.
            const auto sf = kitView.pieceSampleFile (i);
            p->setProperty ("sample", sf.existsAsFile() && libraryDir.isDirectory()
                                          ? sf.getRelativePathFrom (libraryDir) : juce::String());
            arr.add (juce::var (p));
        }
        root->setProperty ("pieces", arr);

        // Config de hardware (6 pads x 7 params).
        juce::Array<juce::var> hwArr;
        for (int p = 0; p < 6; ++p)
        {
            juce::Array<juce::var> row;
            for (int k = 0; k < 7; ++k) row.add (hw[p][k]);
            hwArr.add (row);
        }
        root->setProperty ("hardware", hwArr);

        auto f = configFile();
        f.getParentDirectory().createDirectory();
        f.replaceWithText (juce::JSON::toString (juce::var (root), true));
        configDirty = false;
    }

    void loadConfig()
    {
        auto f = configFile();
        if (! f.existsAsFile()) return;
        const juce::var doc = juce::JSON::parse (f.loadFileAsString());
        if (! doc.isObject()) return;

        if (doc.hasProperty ("kit")) kitBox.setSelectedId ((int) doc["kit"] + 1, juce::dontSendNotification);
        engine.setKit (kitBox.getSelectedId() - 1);
        if (doc.hasProperty ("masterGain")) master.setValue ((double) doc["masterGain"], juce::dontSendNotification);
        engine.setMasterGain ((float) master.getValue());

        // Reconstrói o kit DINÂMICO a partir da config (lista completa de pads).
        if (auto* arr = doc["pieces"].getArray(); arr != nullptr && ! arr->isEmpty())
        {
            kitView.clearPieces();
            for (const juce::var& p : *arr)
            {
                if (! p.isObject()) continue;
                const juce::String name = p.getProperty ("name", "Pad").toString();
                const juce::String cat  = p.getProperty ("category", "tom").toString();
                const float cx = (float) (double) p.getProperty ("cx", 0.5);
                const float cy = (float) (double) p.getProperty ("cy", 0.5);
                const int   note = (int) p.getProperty ("note", 38);

                juce::File sample;
                const juce::String rel = p.getProperty ("sample", "").toString();
                if (rel.isNotEmpty() && libraryDir.isDirectory())
                {
                    auto cand = libraryDir.getChildFile (rel);
                    if (cand.existsAsFile()) sample = cand;
                }
                kitView.addPieceFull (name, cat, cx, cy, note,
                    (float) (double) p.getProperty ("volume", 0.8),
                    (float) (double) p.getProperty ("sens", 0.6),
                    (bool)  p.getProperty ("enabled", true),
                    (bool)  p.getProperty ("mute", false),
                    sample);
            }
            kitView.applyAllToEngine(); // aplica tudo de uma vez (1 rebuild)
        }

        // Config de hardware.
        if (auto* hwArr = doc["hardware"].getArray())
        {
            for (int p = 0; p < juce::jmin (6, hwArr->size()); ++p)
                if (auto* row = (*hwArr)[p].getArray())
                    for (int k = 0; k < juce::jmin (7, row->size()); ++k)
                        hw[p][k] = (int) (*row)[k];
        }
        configDirty = false;
    }

    void timerCallback() override
    {
        auto& tel = engine.getTelemetry();
        for (int note = 0; note < HitTelemetry::numNotes; ++note)
        {
            const auto count = tel.getCount (note);
            if (count != lastCount[note])
            {
                lastCount[note] = count;
                kitView.hit (note, tel.getLastVelocity (note));
            }
        }

        if (auto* lq = engine.getLogQueue())
        {
            MidiLogEvent ev;
            while (lq->pop (ev))
            {
                if (! ev.noteOn) continue;   // pad é one-shot: só a batida interessa
                const double secs = (double) (ev.timeMs - appStartMs) / 1000.0;
                const int pad = padForNote (ev.note);
                const juce::String who = pad >= 0
                    ? "Pad " + juce::String (pad) + "  " + padName (pad)
                    : "nota " + juce::String (ev.note);   // fallback: pad desconhecido
                juce::String line;
                line << juce::String (secs, 3).paddedLeft (' ', 9) << "s   "
                     << who.paddedRight (' ', 16)
                     << "  forca " << juce::String (ev.velocity).paddedLeft (' ', 3);
                monitor.addLine (line, juce::Colour (0xff5fd75f));
            }
        }

        kitView.decay();

        if (configDirty && ++saveThrottle >= 30) { saveThrottle = 0; saveConfig(); }
    }

    DarkLookAndFeel lnf;
    AudioEngine engine;
    DrumKitView kitView;
    bool engineReady = false, configDirty = false;
    int saveThrottle = 0;

    std::map<juce::String, juce::Array<juce::File>> library; // categoria -> WAVs
    juce::File libraryDir;

    juce::Viewport inspectorView;       // inspector rolável
    juce::Component inspectorContent;   // segura todos os controles do inspector
    PadPreview padPreview;              // quadrado com o ícone do pad selecionado

    juce::Label kitLbl, masterLbl, sensLbl, pieceHdr,
                nameLbl, catLbl, noteLbl, soundLbl, volLbl, monitorLbl;
    juce::ComboBox kitBox, soundBox, catBox;
    juce::TextEditor nameEdit;
    juce::TextButton audioBtn, hwBtn, clearBtn, copyBtn, samplesBtn, addBtn, removeBtn, clearKitBtn;

    // Config dos 6 pads físicos do Arduino: sens, thresh, scan, mask, nota, curva, ativo.
    // 'sens' é o TETO do piezo que vira velocity 127 (HelloDrum: teto = sens*10, ADC 0-1023).
    // Mais alto = mais dinâmica (batida forte vs fraca espalham mais); baixo demais satura tudo
    // em 127. 110 (teto ~1100) usa a faixa inteira do ADC. Ajuste fino no painel Config Hardware.
    // ATIVO (última coluna) = 1 só nos pads com piezo conectado. Pino flutuante
    // (sem piezo) capta crosstalk/ruído e dispara sozinho — ex.: bumbo fazia o
    // A2 disparar nota 42. Ligue mais pads no painel Config Hardware ao montá-los.
    // Valores INICIAIS do painel Config Hardware (não são mais enviados no boot;
    // a EEPROM da placa manda). sens=25 -> teto 250 (calibrado, sem ruído, velocity
    // ~16-59); thr=5 -> piso 50; mask=30; curva 0 = linear. Ajuste fino no painel.
    int hw[6][7] = {
        { 25, 5, 20, 30, 38, 0, 1 },  // caixa (A0)  -> piezo mais forte: teto 250
        { 14, 5, 20, 30, 36, 0, 1 },  // bumbo (A1)  -> piezo mais fraco (espuma): teto 140
        { 25, 5, 20, 30, 42, 0, 0 },  // chimbal (A2) -> sem piezo: DESLIGADO
        { 25, 5, 20, 30, 48, 0, 0 },  // tom1 (A3)
        { 25, 5, 20, 30, 45, 0, 0 },  // tom2 (A4)
        { 25, 5, 20, 30, 49, 0, 0 },  // crash (A5)
    };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Slider master, sens, noteSlider, pieceVol;
    MidiMonitor monitor;

    std::uint32_t lastCount[HitTelemetry::numNotes] {};
    std::uint32_t appStartMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

//==============================================================================
class BateriaApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "SomBateria"; }
    const juce::String getApplicationVersion() override { return "3.0.0"; }

    void initialise (const juce::String&) override
    {
        mainWindow = std::make_unique<MainWindow> (juce::String());
    }
    void shutdown() override { mainWindow = nullptr; }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, juce::Colour (0xff17181c), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (920, 600, 4000, 3000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (BateriaApplication)
