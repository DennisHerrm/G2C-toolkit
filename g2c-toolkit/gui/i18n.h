// gui/i18n.h — Zeichenketten der Oberflaeche in vier Sprachen.
//
// Ein Aufzaehlungstyp statt freier Schluessel: ein Tippfehler faellt dann
// beim Uebersetzen auf und nicht erst als leerer Knopf im laufenden
// Programm. Die Tabelle liegt als ein Feld je Eintrag vor, damit beim
// Nachtragen einer Sprache sichtbar wird, wo etwas fehlt.
//
// Alle Zeichenketten sind UTF-8. Unter MSVC ist dafuer /utf-8 noetig, sonst
// deutet der Compiler die Quelldatei als Codepage 1252 und macht aus jedem
// Umlaut zwei Zeichen.

#pragma once

#include <cstdio>
#include <string>

namespace g2::gui {

enum class Lang { De = 0, En, Zh, Ja, Count };

const char* langName(Lang l);

// Reihenfolge egal, Vollstaendigkeit nicht: jeder Eintrag braucht vier Texte.
enum class S {
    // Menue
    MenuFile, MenuBuild, MenuView,
    OpenScript, OpenFolder, Save, SaveAll, CloseTab, CloseAll,
    BuildCurrent, BuildAll, ValidateAll,
    Dark, Light, Settings, Language,

    // Werkzeugleiste
    BtnSave, BtnSaveAll, BtnBuild, BtnBuildAll, BtnValidate,
    BtnOpenFolder, BtnAddXsi, BtnAddXsiFolder, BtnAddXsiFolderAll, BtnAddXsiAll,
    BtnCancel,

    // Einstellungen
    SecPaths, SecOutput, SecProcessing,
    AssetRoot, ReferenceGla, EnumTable,
    OutputPerScript, AssignDefaultOutputs, AssignTooltip,
    WriteFrames, WriteMesh, WriteSkin,
    UseCache, CacheTooltip, CarcassMode, CarcassTooltip,
    ReadFrameCounts, ReadFrameCountsTooltip, CoresInUse,
    ReferenceGlaTooltip,

    // Tabelle
    ColSequence, ColFrames, ColLoop, ColSpeed, ColExtra, ColSource, ColEnum,
    FilterHint, GrabCount, ReadingFrames,
    OutputTo, NotSet, ChooseFolder, DefaultFolder, ClearFolder,
    EnumOk, EnumMissing, EnumNoTable,
    EnumTooltipHeader, EnumTooltipMissing, EnumTooltipNoTable,

    // Meldungen
    TabIssues, TabLog, NotValidated, NoIssues,
    ColLevel, ColMessage,
    LevelError, LevelWarning, LevelInfo,

    // Dialog
    DlgSource, DlgMaster, DlgExtra, DlgExtraHint,
    DlgChoose, DlgClear, DlgAddExtra, DlgClose,
    DlgLoopFrame, DlgFrameSpeed, DlgEnum,
    DlgInAnims, DlgNotInAnims, DlgFrameCount,
    ChooserFilter, ChooserNoTable, ChooserNarrow,

    // Statuszeile und Hinweise
    NoScriptOpen, HintOpenFolder1, HintOpenFolder2,
    ScriptsOpen, Reference,

    // Nachtrag: alles, was zuerst nur auf Deutsch dastand.
    TipDefaultFolder, TipEnumColumn, ColLine, ChooserCount,
    DlgTitleCar, DlgTitleCarFolder, DlgTitleXsi, DlgTitleXsiFolder,
    DlgTitleOutput, DlgTitleGla, DlgTitleEnums,

    // Umordnen, Loeschen, Neuanlage
    New, NewCar, MoveUp, MoveDown, MoveTop, MoveBottom, DeleteSeq, DeleteSelected,
    CtxEdit, FilterBlocksMove, DragHint, Deleted, Moved, NewCarCreated, DlgTitleNewCar,
    ConfirmDelete, Yes, No,
    InsertHere, InsertHereCount, CutSelection, SelectionHint,

    // Zweiter Modus: GLA zurueck nach dotXSI
    ModeBuild, ModeExtract, ModeBuildHint, ModeExtractHint,
    OpenGla, OpenCfg, GlaInfo, NoGlaOpen, NoGlaHint,
    ColStart, ColCount, ColFps, ColLoopFrame,
    ExportSelected, ExportAll, ExportTarget, Exported, ExportFailed,
    SeqCount, NoCfgWarning, NoFramesHint,

    // Fehlermeldungen mit Substanz
    NotAFile, NotAnXsi, FileGone,
    MissingHead, MissingSearched, MissingList, MissingHintBase, MissingHintSkip,
    MissingMore, SeqLabel,
    ExportAllWithCar, ExportAllWithCarTip, Grouped, CarWritten, PartialWarn,

    // Zwei Humanoids vergleichen
    Compare, CompareTip, CompareLoaded, CompareCol, OnlyMissing, SelectMissing,
    CompareNone, CompareCount, MissingHere, PresentHere,

    // Protokollmeldungen
    LogReady, LogRestored, LogAlreadyOpen, LogAssetRootFound, LogRefGlaFound,
    LogRefGlaSet, LogFound, LogNoCfgNearby, LogEnumsLoaded, LogXsiInFolder,
    LogNoXsiIn, LogAddedTo, LogOutputsSet, LogSaved, LogExists, LogCannotRead,
    LogCannotWrite, LogGlaUnreadable, LogNoSeqIn, LogOpenGlaFirst, LogNotFound,
    LogHowToOpen, LogNothingToBuild, LogNothingToSave, LogNoOutputDir,
    LogNoRefGla, LogMeshSourceMissing, LogScriptNotWritable, LogBuilt,
    LogFrameCounts, LogFramesNotFound, LogValidation, LogAllCores,
    LogOpened, LogGlaOpened, LogFramesLoaded, LogDeleted, LogMoved, LogSeqsRead,
    LogNoOutputAt, LogUnreadable, LogAddedFiles,

    // Kopieren und Einfuegen
    Copy, Cut, PasteBefore, PasteAfter, PasteEnd, ClipEmpty, Copied, Pasted, ClipHint,

    // Vorschau
    About, AboutBuilt, AboutBits, AboutRuntimeOk, AboutRuntimeBad,
    SavedBeforeBuild, SameOutDir, CfgBelongsWithGla,
    CopyLog, CopyLogTip, LogCopied, ClearLog,
    BasePoseLabel, BasePoseTip, BasePoseWorld, BasePoseLocal, BasePoseNone,
    GlaNameFromMakeSkel, KeepBackup, KeepBackupTip, AboutLogPath, XsiVersionTip,
    FoldersAdded, OpenOutputDir, RefIsTarget, RefMissing, BuildStoppedDup,
    DlgComment, DlgCommentTip, AddDivider, AddComment, ColComment, CommentEditHint, TrailCommentTip,
    ModePreview, ModePreviewHint, PreviewNoGla, PreviewPlay, PreviewPause,
    PreviewFrame, PreviewReset, PreviewHint, PreviewBones, PreviewSeq,
    OpenFrames, OriginLabel, OriginDetected, OriginNone, FramesMissing, FramesLoaded,
    NotValidatedHint, EnumsAvailable, DlgStart, SpeedAuto, BuildProgress,

    // Rueckfragen, Sicherungen, bisher fest deutsche Texte
    UnsavedHead, Discard, OverwriteHead, Overwrite, LogBackedUp, DragOtherTab,
    NErrors, NWarnings, Validated, DlgSeqTitle, LogCarsUnder, DupInCfg, DupIssue,
    LogWriteFailed, LogSeqsSkipped,

    Count
};

const char* tr(S id);

// Uebersetzten Text formatieren.
//
// Spart an dreissig Stellen dasselbe snprintf mit eigenem Puffer. Der Puffer
// ist statisch und wird reihum benutzt: das Ergebnis wandert sofort ins
// Protokoll, ein laengeres Leben braucht es nicht.
template <typename... Args>
std::string trf(S id, Args... args) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf), tr(id), args...);
    return buf;
}
void        setLanguage(Lang l);
Lang        language();

}  // namespace g2::gui
