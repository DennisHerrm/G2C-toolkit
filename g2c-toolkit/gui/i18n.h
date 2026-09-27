// gui/i18n.h - UI strings in four languages.
//
// An enum instead of free-form keys: a typo is then caught at compile time
// and not only as an empty button in the running program. The table has one
// field per entry, so that when adding a language it is visible where
// something is missing.
//
// All strings are UTF-8. Under MSVC this requires /utf-8, otherwise the
// compiler interprets the source file as code page 1252 and turns every
// umlaut into two characters.

#pragma once

#include <cstdio>
#include <string>

namespace g2::gui {

enum class Lang { De = 0, En, Zh, Ja, Count };

const char* langName(Lang l);

// Order doesn't matter, completeness does: every entry needs four texts.
enum class S {
    // Menu
    MenuFile, MenuBuild, MenuView,
    OpenScript, OpenFolder, Save, SaveAll, CloseTab, CloseAll,
    BuildCurrent, BuildAll, ValidateAll,
    Dark, Light, Settings, Language,

    // Toolbar
    BtnSave, BtnSaveAll, BtnBuild, BtnBuildAll, BtnValidate,
    BtnOpenFolder, BtnAddXsi, BtnAddXsiFolder, BtnAddXsiFolderAll, BtnAddXsiAll,
    BtnCancel,

    // Settings
    SecPaths, SecOutput, SecProcessing,
    AssetRoot, ReferenceGla, EnumTable,
    OutputPerScript, AssignDefaultOutputs, AssignTooltip,
    WriteFrames, WriteMesh, WriteSkin,
    UseCache, CacheTooltip, CarcassMode, CarcassTooltip,
    ReadFrameCounts, ReadFrameCountsTooltip, CoresInUse,
    ReferenceGlaTooltip,

    // Table
    ColSequence, ColFrames, ColLoop, ColSpeed, ColExtra, ColSource, ColEnum,
    FilterHint, GrabCount, ReadingFrames,
    OutputTo, NotSet, ChooseFolder, DefaultFolder, ClearFolder,
    EnumOk, EnumMissing, EnumNoTable,
    EnumTooltipHeader, EnumTooltipMissing, EnumTooltipNoTable,

    // Messages
    TabIssues, TabLog, NotValidated, NoIssues,
    ColLevel, ColMessage,
    LevelError, LevelWarning, LevelInfo,

    // Dialog
    DlgSource, DlgMaster, DlgExtra, DlgExtraHint,
    DlgChoose, DlgClear, DlgAddExtra, DlgClose,
    DlgLoopFrame, DlgFrameSpeed, DlgEnum,
    DlgInAnims, DlgNotInAnims, DlgFrameCount,
    ChooserFilter, ChooserNoTable, ChooserNarrow,

    // Status bar and hints
    NoScriptOpen, HintOpenFolder1, HintOpenFolder2,
    ScriptsOpen, Reference,

    // Addendum: everything that was initially only in German.
    TipDefaultFolder, TipEnumColumn, ColLine, ChooserCount,
    DlgTitleCar, DlgTitleCarFolder, DlgTitleXsi, DlgTitleXsiFolder,
    DlgTitleOutput, DlgTitleGla, DlgTitleEnums,

    // Reordering, deleting, creating
    New, NewCar, MoveUp, MoveDown, MoveTop, MoveBottom, DeleteSeq, DeleteSelected,
    CtxEdit, FilterBlocksMove, DragHint, Deleted, Moved, NewCarCreated, DlgTitleNewCar,
    ConfirmDelete, Yes, No,
    InsertHere, InsertHereCount, CutSelection, SelectionHint,

    // Second mode: GLA back to dotXSI
    ModeBuild, ModeExtract, ModeBuildHint, ModeExtractHint,
    OpenGla, OpenCfg, GlaInfo, NoGlaOpen, NoGlaHint,
    ColStart, ColCount, ColFps, ColLoopFrame,
    ExportSelected, ExportAll, ExportTarget, Exported, ExportFailed,
    SeqCount, NoCfgWarning, NoFramesHint,

    // Error messages with substance
    NotAFile, NotAnXsi, FileGone,
    MissingHead, MissingSearched, MissingList, MissingHintBase, MissingHintSkip,
    MissingMore, SeqLabel,
    ExportAllWithCar, ExportAllWithCarTip, Grouped, CarWritten, PartialWarn,

    // Comparing two humanoids
    Compare, CompareTip, CompareLoaded, CompareCol, OnlyMissing, SelectMissing,
    CompareNone, CompareCount, MissingHere, PresentHere,

    // Log messages
    LogReady, LogRestored, LogAlreadyOpen, LogAssetRootFound, LogRefGlaFound,
    LogRefGlaSet, LogFound, LogNoCfgNearby, LogEnumsLoaded, LogXsiInFolder,
    LogNoXsiIn, LogAddedTo, LogOutputsSet, LogSaved, LogExists, LogCannotRead,
    LogCannotWrite, LogGlaUnreadable, LogNoSeqIn, LogOpenGlaFirst, LogNotFound,
    LogHowToOpen, LogNothingToBuild, LogNothingToSave, LogNoOutputDir,
    LogNoRefGla, LogMeshSourceMissing, LogScriptNotWritable, LogBuilt,
    LogFrameCounts, LogFramesNotFound, LogValidation, LogAllCores,
    LogOpened, LogGlaOpened, LogFramesLoaded, LogDeleted, LogMoved, LogSeqsRead,
    LogNoOutputAt, LogUnreadable, LogAddedFiles,

    // Copy and paste
    Copy, Cut, PasteBefore, PasteAfter, PasteEnd, ClipEmpty, Copied, Pasted, ClipHint,

    // Preview
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

    // Confirmations, backups, formerly hard-coded German texts
    UnsavedHead, Discard, OverwriteHead, Overwrite, LogBackedUp, DragOtherTab,
    NErrors, NWarnings, Validated, DlgSeqTitle, LogCarsUnder, DupInCfg, DupIssue,
    LogWriteFailed, LogSeqsSkipped,

    // Updates
    SecUpdates, UpdAuto, UpdAutoTip, UpdStable, UpdSnapshot, UpdChannelTip, AboutVersion,
    UpdCheckNow, UpdChecking, UpdUpToDate, UpdAvailable, UpdInstall, UpdNotes, UpdLater,
    UpdSkip, UpdDownloading, UpdInstalled, UpdRestart, UpdDone, UpdOpenPage, UpdCancelled,
    UpdErrNetwork, UpdErrNoRelease, UpdErrBadAnswer, UpdErrNoAsset, UpdErrUntrusted,
    UpdErrChecksum, UpdErrWrite,

    Count
};

const char* tr(S id);

// Formats a translated text.
//
// Saves the same snprintf with its own buffer in thirty places. The buffer
// is static and used in rotation: the result goes straight into the log, it
// doesn't need to live any longer.
template <typename... Args>
std::string trf(S id, Args... args) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf), tr(id), args...);
    return buf;
}
void        setLanguage(Lang l);
Lang        language();

}  // namespace g2::gui
