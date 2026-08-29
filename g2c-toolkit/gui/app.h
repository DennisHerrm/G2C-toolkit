// gui/app.h — Oberflaeche fuer g2c.
//
// Bewusst getrennt von der Fensteranbindung: diese Datei kennt nur imgui.h
// und die g2-Bibliothek, kein Win32 und kein DirectX. Alles, was das
// Betriebssystem braucht — Dateidialoge, Ordnerauswahl — kommt ueber die
// Platform-Struktur herein.
//
// Der Grund ist nicht Portabilitaet um ihrer selbst willen, sondern
// Pruefbarkeit: so laesst sich der weitaus groessere Teil der Oberflaeche
// uebersetzen und testen, ohne ein Fenster zu oeffnen.

#pragma once

#include "gui/i18n.h"
#include "gui/icons.h"
#include "gui/preview.h"

#include "g2/animenums.h"
#include "g2/xsi_export.h"
#include "g2/carbuild.h"
#include "g2/carscript.h"
#include "g2/carvalidate.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <array>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace g2::gui {

// Was die Oberflaeche vom Betriebssystem braucht.
struct Platform {
    // Mehrfachauswahl moeglich; leere Liste = abgebrochen.
    //
    // startDir sagt, wo der Dialog aufgehen soll. Leer = Windows entscheidet.
    std::function<std::vector<std::string>(const char* title, const char* filter, bool multi,
                                           const std::string& startDir)>
        openFiles;
    std::function<std::string(const char* title, const std::string& startDir)> pickFolder;
    std::function<void(const std::string& path)> revealInExplorer;
};

// Ein geoeffnetes Skript — ein Tab.
struct Document {
    std::string  path;
    std::string  title;      // Dateiname, in der Tableiste
    car::Script  script;
    bool         dirty = false;
    bool         open = true;
    std::string  loadError;

    // Ergebnis der letzten Pruefung, damit die Anzeige nicht bei jedem
    // Bildaufbau neu rechnet.
    car::ValidateResult validation;
    bool                validated = false;

    // Zeilenauswahl fuer Mehrfachaktionen.
    std::vector<char> selected;

    // Ausgabeort dieses Skripts. MUSS gesetzt sein, sonst wird nicht gebaut.
    //
    // Es gibt bewusst keinen gemeinsamen Ordner fuer alle: in einem
    // Modellbaum heissen saemtliche Ausgaben "_humanoid.gla", und ein
    // gemeinsames Ziel liesse zwanzig Skripte dieselbe Datei ueberschreiben —
    // ohne Fehlermeldung, mit dem Ergebnis, dass nur das letzte uebrig
    // bleibt. Eine Einstellung, die in der Mehrzahl der Faelle Daten
    // vernichtet, gehoert nicht ins Programm.
    std::string outputDir;

    void syncSelection() { selected.assign(script.grabs.size(), 0); }
};

// Einstellungen, die fuer alle Tabs gelten.
struct Settings {
    std::string enumPath;      // anims.h
    std::string baseDir;       // Wurzel, unter der models/ liegt
    std::string referenceGla;  // Skelettquelle

    bool writeFrames = true;
    bool writeMesh = true;
    bool writeSkin = true;
    bool useCache = true;
    bool carcassCompat = false;

    // Beim ersten Speichern eine .car.bak anlegen.
    //
    // Nur EINMAL je Datei — der urspruengliche Stand vor der ersten
    // Bearbeitung ist das, was man zurueckhaben will, nicht der von vorhin.
    // Wer das nicht braucht, schaltet es ab.
    bool keepBackup = true;
    bool readFrameCounts = false;

    // Immer alle Kerne. Bewusst keine Einstellung: die Ausgabe ist
    // nachweislich unabhaengig von der Threadzahl bitgleich, und eine
    // Einstellung, die man nur falsch stellen kann, ist keine.
    static constexpr int threads = 0;

    bool darkMode = true;

    // Sprache als Zahl, damit die Einstellungsdatei einfach bleibt.
    int  language = 0;

    // Bildpunkte je logischem Punkt, von der Fensteranbindung gesetzt.
    //
    // Ohne das zeichnet ImGui in 96 dpi und Windows skaliert das fertige Bild
    // hoch — das Ergebnis ist unscharf. Richtig ist, gleich groesser zu
    // zeichnen: groessere Schrift, groessere Abstaende, scharfe Kanten.
    float dpiScale = 1.0f;
};

// Eine Zeile im Protokoll.
struct LogLine {
    enum class Kind { Info, Good, Warn, Bad };
    Kind        kind = Kind::Info;
    std::string text;
};

// Zustand eines laufenden Stapelbaus.
struct BuildJob {
    std::atomic<bool>        running{false};
    std::atomic<bool>        cancel{false};
    std::atomic<std::size_t> done{0};
    std::atomic<std::size_t> total{0};
    std::string              current;
    std::mutex               currentMutex;
    std::thread              worker;

    ~BuildJob();
    void join();

    // Ausdruecklich unbeweglich und unkopierbar.
    //
    // mutex und thread sind es ohnehin, der Uebersetzer erzeugt die
    // Operationen also nicht. Es hinzuschreiben kostet nichts und macht die
    // Absicht sichtbar, statt sie aus den Membertypen ableiten zu lassen.
    BuildJob() = default;
    BuildJob(const BuildJob&) = delete;
    BuildJob& operator=(const BuildJob&) = delete;
    BuildJob(BuildJob&&) = delete;
    BuildJob& operator=(BuildJob&&) = delete;
};

class App {
public:
    explicit App(Platform platform);
    ~App();

    // App haelt einen BuildJob mit laufendem Thread — kopieren oder
    // verschieben waere in jedem Fall falsch.
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    // Ein Bildaufbau. Ruft ImGui auf und kehrt zurueck.
    void draw();

    // --- Auch ohne Fenster benutzbar, damit pruefbar -----------------------

    bool openCar(const std::string& path);
    std::size_t openFolder(const std::string& root);   // liefert die Zahl der Funde

    // Datei oder Ordner, je nachdem was es ist. Fuer Ziehen auf das Fenster
    // und fuer Argumente beim Start.
    bool openPath(const std::string& path);

    // --- Zweiter Modus: GLA zurueck nach dotXSI ---------------------------
    //
    // Bewusst getrennt vom Bauen. Die beiden Richtungen teilen fast nichts:
    // andere Eingaben, andere Ausgaben, andere Begriffe. In eine gemeinsame
    // Oberflaeche gepresst muesste man staendig Felder ausgrauen, die im
    // jeweiligen Modus keinen Sinn ergeben.
    enum class Mode { Build, Extract, Preview };

    struct ExtractSeq {
        std::string name;
        int         start = 0;
        int         count = 0;
        int         loopFrame = -1;
        int         fps = 20;
    };

    struct ExtractState {
        std::string glaPath;
        std::string cfgPath;

        // Pfad zur .frames der Quell-GLA. Ohne sie fehlt die
        // Wurzelbewegung, und eine Laufanimation laeuft nach dem Neubauen
        // auf der Stelle.
        std::string framesPath;

        // Versatz, mit dem die Quell-GLA gebaut wurde. Wird beim Oeffnen
        // geschaetzt und angezeigt; er muss beim Export wieder eingesetzt
        // werden, sonst zieht das Neubauen ihn ein zweites Mal ab.
        std::optional<std::array<float, 3>> origin;

        // Welche Bindepose in den BASEPOSE-Block.
        //
        // Betrifft nur, wer die Dateien durch Ravens Carcass schickt; g2c
        // selbst baut mit jeder Einstellung gleich, weil es die FCurves
        // auswertet und den Block gar nicht ansieht.
        xsiexp::ExportOptions::BasePose basePose = xsiexp::ExportOptions::BasePose::World;

        // --- Vergleich mit einem anderen Humanoid -------------------------
        //
        // Sequenznamen der Gegenseite, klein geschrieben. Damit laesst sich
        // beantworten: welche Animationen hat DIESE GLA, die jene nicht hat?
        // Genau die will man uebernehmen.
        std::string           comparePath;
        std::set<std::string> compareNames;
        bool                  onlyMissing = false;

        std::size_t             withMotion = 0;
        std::string             outDir;
        MdxaFile                gla;
        bool                    loaded = false;
        std::vector<ExtractSeq> seqs;
        std::vector<char>       selected;
        std::string             error;
    };

    Mode                mode() const { return mode_; }
    void                setMode(Mode m) { mode_ = m; }
    ExtractState&       extract() { return extract_; }
    const ExtractState& extract() const { return extract_; }

    bool        loadGlaForExtract(const std::string& path);
    bool        loadAnimationCfg(const std::string& path);
    bool        loadFramesFile(const std::string& path);

    // Nur fuer Tests: aktiven Tab setzen, ohne die Oberflaeche zu zeichnen.
    void setActiveForTest(int i) { active_ = i; }

    // Sucht animation.cfg und .frames neben der GLA.
    //
    // Sie liegen praktisch immer im selben Ordner; sie einzeln auswaehlen zu
    // lassen ist Arbeit, die das Programm selbst erledigen kann. Die
    // Schaltflaechen bleiben fuer die Ausnahme.
    void        findCompanionFiles(const std::string& glaPath);

    // Zweite animation.cfg laden, um Sequenznamen zu vergleichen.
    bool        loadCompareCfg(const std::string& path);

    // Ist die Sequenz auf der Gegenseite vorhanden?
    bool        existsInCompare(const std::string& name) const;

    // Alle auswaehlen, die drueben fehlen. Liefert die Anzahl.
    std::size_t selectMissing();
    std::size_t exportSequences(const std::vector<std::size_t>& rows, const std::string& dir);

    // Alles exportieren UND ein .car dazu schreiben, mit dem sich die GLA
    // sofort wieder bauen laesst. Liefert die Zahl der geschriebenen .xsi.
    std::size_t exportAllWithScript(const std::string& dir, const std::string& carPath,
                                    const std::string& xsiPrefix);
    void closeDocument(std::size_t index);

    // Haengt Dateien als $aseanimgrab an. `toAll` verteilt sie auf alle Tabs.
    std::size_t addXsiFiles(const std::vector<std::string>& files, bool toAll);

    void validateDocument(std::size_t index);
    void validateAll();

    bool loadEnums(const std::string& path);

    // Jedem Skript den Ordner seiner eigenen .car zuweisen. Bequemlichkeit
    // ohne die Gefahr eines gemeinsamen Ziels: die Pfade sind verschieden.
    std::size_t assignDefaultOutputs(bool onlyEmpty = true);

    // --- Sequenzen umordnen und loeschen ----------------------------------
    //
    // Reine Datenoperationen, damit sie ohne Fenster pruefbar sind.

    // Verschiebt EINE Sequenz von from nach to. Die Auswahlmarkierungen
    // wandern mit, sonst zeigt die Auswahl nach dem Verschieben auf die
    // falsche Zeile.
    bool moveGrab(std::size_t docIndex, std::size_t from, std::size_t to);

    // Verschiebt MEHRERE Zeilen als Block vor die Zeile "before".
    // Die Auswahl bleibt auf denselben Sequenzen.
    std::size_t moveGrabs(std::size_t docIndex, std::vector<std::size_t> rows,
                          std::size_t before);

    // ROOT ans Ende.
    //
    // root.xsi liefert die Basispose; in Ravens Skripten steht der Grab
    // immer zuletzt. Neue Animationen davor einzureihen ist deshalb kein
    // Schoenheitsfehler, sondern haelt die Datei so, wie Carcass und
    // Assimilate sie erwarten.
    bool keepRootLast(std::size_t docIndex);

    // Ist das der ROOT-Grab?
    static bool isRootGrab(const car::GrabDirective& g);

    // Loescht die uebergebenen Zeilen. Absteigend sortiert abgearbeitet,
    // damit sich die Indizes nicht unter der Schleife verschieben.
    std::size_t deleteGrabs(std::size_t docIndex, std::vector<std::size_t> rows);

    // Legt ein leeres, gueltiges Skript an und oeffnet es als Tab.
    bool newCar(const std::string& path);

    // --- Kopieren und Einfuegen -------------------------------------------
    //
    // Die Zwischenablage haelt vollstaendige Grabs, samt -additional, Loop
    // und Rate. Sie gilt tab-uebergreifend: aus einem Skript kopieren, in
    // einem anderen einfuegen ist der eigentliche Zweck.
    //
    // Kopiert werden Kopien, keine Verweise. Wird die Quelle danach
    // geloescht, bleibt die Ablage gueltig.
    std::size_t copyGrabs(std::size_t docIndex, const std::vector<std::size_t>& rows);
    std::size_t cutGrabs(std::size_t docIndex, const std::vector<std::size_t>& rows);
    std::size_t pasteGrabs(std::size_t docIndex, std::size_t before);
    std::size_t clipboardSize() const { return clipboard_.size(); }

    void setLogPath(std::string p) { logPath_ = std::move(p); }

    // Das ganze Protokoll als Text.
    //
    // Fuer die Zwischenablage. Mit Zeitstempel und Programmangaben im Kopf:
    // ein Fehlerbericht ohne Fassung und Systemangaben kostet immer eine
    // Rueckfrage.
    std::string logAsText() const;

    // Alle .xsi eines Ordners anhaengen — rekursiv, sortiert.
    std::size_t addXsiFolder(const std::string& folder, bool toAll);

    // Framezahl einer Quelldatei, sofern schon bekannt. -1 = noch nicht
    // gelesen, -2 = nicht auffindbar.
    int frameCountOf(const std::string& relPath) const;

    // Einstellungen ueber Sitzungen hinweg merken.
    // Ordner fuer Einstellungen und Fensterzustand.
    //
    // Statisch, weil die Fensteranbindung ihn braucht, bevor die App
    // ueberhaupt gebaut ist: der Pfad der imgui-Datei muss stehen, bevor
    // ImGui den ersten Bildaufbau macht.
    static std::string configDir();

    // Auswahldialoge mit eigenem Gedaechtnis.
    //
    // Jeder Zweck merkt sich SEINEN Ordner: die anims.h liegt woanders als
    // die .xsi, und die wieder woanders als der Ausgabeordner. Ein
    // gemeinsamer "zuletzt benutzt"-Ordner heisst, sich bei jedem zweiten
    // Dialog neu durch den Baum zu klicken.
    std::vector<std::string> askFiles(const char* purpose, const char* title,
                                      const char* filter, bool multi);
    std::string              askFolder(const char* purpose, const char* title);
    std::string settingsPath() const;
    void        saveSettings() const;
    void        loadSettings();

    // Skript zurueckschreiben. Legt vorher eine Sicherung an.
    bool saveDocument(std::size_t index);
    std::size_t saveAllDocuments();

    void log(LogLine::Kind kind, std::string text);

    Settings&                      settings() { return settings_; }
    const std::vector<Document>&   documents() const { return docs_; }
    std::vector<Document>&         documents() { return docs_; }
    const anim::EnumTable&         enums() const { return enums_; }
    const std::deque<LogLine>&     logLines() const { return log_; }
    int                            activeTab() const { return active_; }

    bool buildRunning() const { return job_.running.load(); }

    // Die Fensteranbindung fragt das ab: bei Chinesisch oder Japanisch muss
    // der Zeichensatz mit anderen Glyphenbereichen neu aufgebaut werden.
    bool fontsDirty() const { return fontsDirty_; }
    void clearFontsDirty() { fontsDirty_ = false; }

private:
    void drawMenuBar();
    void drawToolbar();
    void drawSettingsPanel();
    void drawTabs();
    void drawSequenceTable(Document& d);
    void drawModeBar();
    void drawExtractPanel();
    void drawPreviewPanel();
    void drawIssues();
    void drawSequenceDialog(Document& d);
    // Auswahlliste fuer Enums. Liefert true, wenn etwas gewaehlt wurde.
    bool drawEnumChooser(const char* popupId, std::string& target);
    void drawLog();

    // Farbiges Symbol, dann Text - beides in einer Zeile.
    //
    // ImGui zeichnet Text einfarbig, also auch Symbole innerhalb eines
    // Labels. Getrennt gezeichnet laesst sich das Symbol einfaerben, und
    // das hilft beim Erfassen: gruen heisst fertig, rot heisst Fehler, ohne
    // dass man den Text lesen muss.
    //
    // Ohne Symbolschrift wird nur der Text gezeichnet.
    static void iconText(const char* icon, const IconColor& col, const char* text);

    // Knopf mit FARBIGEM Symbol.
    //
    // ImGui zeichnet den Text eines Knopfes einfarbig, also auch ein Symbol
    // darin. Hier wird der Knopf zuerst leer gezeichnet und Symbol und Text
    // anschliessend mit der Zeichenliste daraufgesetzt — so bekommt das
    // Symbol seine eigene Farbe, waehrend Rahmen und Hover-Verhalten die
    // von ImGui bleiben.
    //
    // Ohne Symbolschrift wird nur der Text gezeichnet, und der Knopf ist
    // entsprechend schmaler.
    static bool iconButton(const char* icon, const IconColor& col, const char* text,
                           bool small = false);
    void drawStatusBar();
    void applyStyle();
    void refreshTabTitles();

    void startBuild(bool allTabs);
    void buildOne(const Document& d);

    Platform             platform_;
    Settings             settings_;
    std::vector<Document> docs_;
    anim::EnumTable      enums_;
    std::deque<LogLine>  log_;
    // mutable, damit logAsText() const sein kann: Sperren aendert nichts am
    // sichtbaren Zustand.
    mutable std::mutex   logMutex_;
    int                  active_ = 0;
    bool                 showSettings_ = true;
    Mode                 mode_ = Mode::Build;

    // --- Vorschau ----------------------------------------------------------
    //
    // Nutzt die GLA, die im Modus GLA -> XSI geladen ist. Sie ein zweites
    // Mal zu laden waere Verschwendung: die Datei ist 15 MB gross und liegt
    // bereits im Speicher.
    PreviewCamera        previewCam_;
    Playback             playback_;
    int                  previewSeq_ = -1;
    double               previewLastTime_ = 0.0;
    ExtractState         extract_;
    char                 extractFilter_[128] = {0};
    int                  extractAnchor_ = -1;

    bool                 showAbout_ = false;

    // Pfad des Startprotokolls, von der Plattformschicht gesetzt.
    //
    // Die Oberflaeche ermittelt ihn nicht selbst: wo die Exe liegt, weiss
    // nur main_win32.cpp, und die Auskunft muss mit der Datei
    // uebereinstimmen, die dort auch wirklich beschrieben wird.
    std::string          logPath_;
    bool                 styleApplied_ = false;
    bool                 fontsDirty_ = false;
    char                 filter_[128] = {0};
    BuildJob             job_;

    // Ausgewaehlte Meldung -> Sprung in die Tabelle.
    int  jumpToRow_ = -1;
    int  issueSelected_ = -1;

    // Offener Bearbeitungsdialog: Index des Grabs, -1 = keiner.
    // Aufgeschobenes Einfuegen: erst nach der Tabelle ausfuehren.
    long pendingPaste_ = -1;

    // Zwischenablage fuer Sequenzen, tab-uebergreifend.
    std::vector<car::GrabDirective> clipboard_;

    // Ankerzeile fuer die Bereichsauswahl.
    int  selAnchor_ = -1;

    // Aufziehen einer Auswahl mit gehaltener Maustaste, wie im Explorer.
    //
    // Der Unterschied zwischen "Auswahl aufziehen" und "Zeilen verschieben"
    // entscheidet sich beim Druecken: auf einer BEREITS ausgewaehlten Zeile
    // beginnt ein Verschieben, auf einer anderen eine neue Auswahl. Genau so
    // verhaelt sich der Explorer, und deshalb muss man es niemandem
    // erklaeren.
    bool rangeSelecting_ = false;

    // Aufgeschobenes Umordnen: erst nach der Tabelle ausfuehren.
    std::pair<std::vector<std::size_t>, std::size_t> pendingBlock_{{}, 0};
    // Zeilen, deren Loeschung noch bestaetigt werden muss.
    std::vector<std::size_t> pendingDelete_;

    int  editRow_ = -1;
    bool editOpen_ = false;
    char enumFilter_[128] = {0};

    // Gemerkte Ausgabeorte je .car-Pfad, aus den Einstellungen geladen.
    std::map<std::string, std::string> savedOutputs_;

    // Zuletzt offene Tabs, beim Start wiederhergestellt.
    // Zuletzt benutzter Ordner je Zweck.
    std::map<std::string, std::string> lastDirs_;

    std::vector<std::string> savedTabs_;
    int                      savedActive_ = 0;

    // --- Framezahlen im Hintergrund ---------------------------------------
    //
    // 1289 Dateien im Vordergrund zu lesen wuerde das Fenster fuer Sekunden
    // einfrieren. Deshalb laeuft das nebenher und die Spalte fuellt sich
    // nach und nach. Ein Knopf dafuer waere unnoetige Arbeit fuer den
    // Nutzer — die Zahl will man immer sehen.
    mutable std::mutex          frameMutex_;
    std::map<std::string, int>  frameCounts_;      // relativer Pfad -> Frames
    std::atomic<bool>           frameWorkerRunning_{false};
    std::atomic<bool>           frameWorkerStop_{false};
    std::thread                 frameWorker_;
    std::string                 frameWorkerBaseDir_;

    void startFrameWorker(const Document& d);
    void stopFrameWorker();
};

}  // namespace g2::gui
