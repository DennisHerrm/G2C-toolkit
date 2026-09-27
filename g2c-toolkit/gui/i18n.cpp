#include "gui/i18n.h"

#include <cstddef>

namespace g2::gui {
namespace {

struct Entry {
    // The entry carries its own name.
    //
    // Previously the table was linked to the enum by position alone, and the
    // static_assert only checks the LENGTH. When I inserted two blocks in the
    // .cpp at a different place than in the .h, the length still matched -
    // but from that point on every text was shifted by several slots. The
    // menu then showed "Moved: %s" as an entry, and "Zeile ziehen" carried a
    // trash-can icon.
    //
    // With the name in the entry, this becomes a compile error.
    S           id;
    const char* de;
    const char* en;
    const char* zh;
    const char* ja;
};

// The order MUST match the enum in i18n.h.
constexpr Entry kTable[] = {
    {S::MenuFile, "Datei", "File", "文件", "ファイル"},
    {S::MenuBuild, "Bauen", "Build", "构建", "ビルド"},
    {S::MenuView, "Ansicht", "View", "视图", "表示"},
    {S::OpenScript, "Skript oeffnen...", "Open script...", "打开脚本...", "スクリプトを開く..."},
    {S::OpenFolder, "Ordner oeffnen...", "Open folder...", "打开文件夹...", "フォルダーを開く..."},
    {S::Save, "Speichern", "Save", "保存", "保存"},
    {S::SaveAll, "Alle speichern", "Save all", "全部保存", "すべて保存"},
    {S::CloseTab, "Tab schliessen", "Close tab", "关闭标签页", "タブを閉じる"},
    {S::CloseAll, "Alle schliessen", "Close all", "全部关闭", "すべて閉じる"},
    {S::BuildCurrent, "Aktuelles Skript", "Current script", "当前脚本", "現在のスクリプト"},
    {S::BuildAll, "Alle Skripte", "All scripts", "所有脚本", "すべてのスクリプト"},
    {S::ValidateAll, "Alle pruefen", "Validate all", "全部检查", "すべて検証"},
    {S::Dark, "Dunkel", "Dark", "深色", "ダーク"},
    {S::Light, "Hell", "Light", "浅色", "ライト"},
    {S::Settings, "Einstellungen", "Settings", "设置", "設定"},
    {S::Language, "Sprache", "Language", "语言", "言語"},
    {S::BtnSave, "Speichern", "Save", "保存", "保存"},
    {S::BtnSaveAll, "Alle speichern", "Save all", "全部保存", "すべて保存"},
    {S::BtnBuild, "Bauen", "Build", "构建", "ビルド"},
    {S::BtnBuildAll, "Alle bauen", "Build all", "全部构建", "すべてビルド"},
    {S::BtnValidate, "Pruefen", "Validate", "检查", "検証"},
    {S::BtnOpenFolder, "Ordner oeffnen", "Open folder", "打开文件夹", "フォルダーを開く"},
    {S::BtnAddXsi, "XSI hinzufuegen", "Add XSI", "添加 XSI", "XSI を追加"},
    {S::BtnAddXsiFolder, "XSI-Ordner", "XSI folder", "XSI 文件夹", "XSI フォルダー"},
    {S::BtnAddXsiFolderAll, "XSI-Ordner zu allen", "XSI folder to all", "XSI 文件夹到全部", "XSI フォルダーを全体へ"},
    {S::BtnAddXsiAll, "XSI zu allen", "XSI to all", "XSI 到全部", "XSI を全体へ"},
    {S::BtnCancel, "Abbrechen", "Cancel", "取消", "中止"},
    {S::SecPaths, "Pfade", "Paths", "路径", "パス"},
    {S::SecOutput, "Ausgabe", "Output", "输出", "出力"},
    {S::SecProcessing, "Verarbeitung", "Processing", "处理", "処理"},
    {S::AssetRoot, "Assetwurzel - enthaelt models\\", "Asset root - contains models\\", "资源根目录 - 包含 models\\",
     "アセットルート - models\\ を含む"},
    {S::ReferenceGla, "Referenz-GLA - Skelettquelle, wird automatisch erkannt",
     "Reference GLA - skeleton source, detected automatically",
     "参考 GLA - 骨架来源，自动识别", "参照 GLA - スケルトン供給元、自動検出"},
    {S::EnumTable, "Enumtabelle (anims.h)", "Enum table (anims.h)", "枚举表 (anims.h)", "列挙表 (anims.h)"},
    {S::OutputPerScript, "Ausgabe wird je Skript ueber der Tabelle gesetzt.",
     "Output is set per script above the table.", "输出在表格上方按脚本单独设置。",
     "出力は表の上でスクリプトごとに設定します。"},
    {S::AssignDefaultOutputs, "Alle auf g2c_out neben der jeweiligen .car",
     "Set all to g2c_out next to each .car", "全部设为各自 .car 旁的 g2c_out",
     "すべてを各 .car の隣の g2c_out に設定"},
    {S::AssignTooltip, "Setzt fuer jedes Skript OHNE Ziel den Ordner g2c_out neben seiner eigenen .car.\n"
     "Verschiedene Pfade, kein gemeinsames Ziel.",
     "Sets g2c_out next to each script's own .car, for every script WITHOUT a target.\n"
     "Distinct paths, no shared target.",
     "为每个尚未设置目标的脚本，在其自身 .car 旁设置 g2c_out。\n路径各不相同，没有共享目标。",
     "目的地が未設定の各スクリプトに、その .car の隣の g2c_out を設定します。\n"
     "パスはそれぞれ異なり、共有先はありません。"},
    {S::WriteFrames, ".frames schreiben", "Write .frames", "写入 .frames", ".frames を書き出す"},
    {S::WriteMesh, "GLM erzeugen", "Generate GLM", "生成 GLM", "GLM を生成"},
    {S::WriteSkin, ".skin schreiben", "Write .skin", "写入 .skin", ".skin を書き出す"},
    {S::UseCache, "Zwischenspeicher benutzen", "Use cache", "使用缓存", "キャッシュを使う"},
    {S::CacheTooltip, "Geparste .xsi merken. Der zweite Bau ist deutlich schneller.",
     "Remember parsed .xsi files. The second build is much faster.",
     "记住已解析的 .xsi。第二次构建会快得多。",
     "解析済みの .xsi を保持します。二度目のビルドは大幅に速くなります。"},
    {S::CarcassMode, "Wie Carcass quantisieren", "Quantise like Carcass", "按 Carcass 方式量化",
     "Carcass と同じ量子化"},
    {S::CarcassTooltip, "Kleinere Datei, aber rund dreifacher Rotationsfehler.",
     "Smaller file, but roughly three times the rotation error.",
     "文件更小，但旋转误差约为三倍。", "ファイルは小さくなりますが、回転誤差は約 3 倍になります。"},
    {S::ReadFrameCounts, "Framezahlen beim Pruefen lesen", "Read frame counts when validating", "检查时读取帧数",
     "検証時にフレーム数を読む"},
    {S::ReadFrameCountsTooltip, "Genauer, muss dafuer jede .xsi lesen.", "More accurate, but reads every .xsi.",
     "更准确，但需要读取每个 .xsi。", "より正確ですが、すべての .xsi を読み込みます。"},
    {S::CoresInUse, "Alle %u Kerne werden genutzt.", "Using all %u cores.", "使用全部 %u 个核心。",
     "%u 個のコアをすべて使用します。"},
    {S::ReferenceGlaTooltip, "Zwingend, aber normalerweise nichts zu tun: der Pfad wird aus -makeskel abgeleitet.\n\n"
     "Von hier kommen Skelett, Hierarchie, Basisposen und Scale. Die Datei wird nur gelesen.\n"
     "Wo die NEUE GLA landet, steht ueber der Tabelle.",
     "Required, but usually nothing to do: the path is derived from -makeskel.\n\n"
     "Skeleton, hierarchy, base poses and scale come from here. The file is only read.\n"
     "Where the NEW GLA goes is set above the table.",
     "必需，但通常无需操作：路径由 -makeskel 推导。\n\n骨架、层级、基础姿势和缩放都来自这里。"
     "该文件只会被读取。\n新 GLA 的输出位置在表格上方设置。",
     "必須ですが通常は操作不要です。パスは -makeskel から導出されます。\n\n"
     "スケルトン、階層、基本ポーズ、スケールはここから読み込まれます。書き込みは行いません。\n"
     "新しい GLA の出力先は表の上で設定します。"},
    {S::ColSequence, "Sequenz", "Sequence", "序列", "シーケンス"},
    {S::ColFrames, "Frames", "Frames", "帧数", "フレーム"},
    {S::ColLoop, "Loop", "Loop", "循环", "ループ"},
    {S::ColSpeed, "Speed", "Speed", "速度", "速度"},
    {S::ColExtra, "Zusatz", "Extra", "附加", "追加"},
    {S::ColSource, "Quelldatei", "Source file", "源文件", "ソースファイル"},
    {S::ColEnum, "Enum", "Enum", "枚举", "列挙"},
    {S::FilterHint, "Filtern...", "Filter...", "筛选...", "絞り込み..."},
    {S::GrabCount, "%zu Grabs", "%zu grabs", "%zu 个抓取", "%zu 件の取り込み"},
    {S::ReadingFrames, "Framezahlen werden gelesen...", "Reading frame counts...", "正在读取帧数...",
     "フレーム数を読み込み中..."},
    {S::OutputTo, "GLA + animation.cfg nach:", "GLA + animation.cfg to:", "GLA + animation.cfg 输出到：",
     "GLA + animation.cfg の出力先:"},
    {S::NotSet, "nicht gesetzt - so wird nicht gebaut", "not set - build will not run",
     "未设置 - 无法构建", "未設定 - ビルドできません"},
    {S::ChooseFolder, "Waehlen", "Choose", "选择", "選択"},
    {S::DefaultFolder, "g2c_out", "g2c_out", "g2c_out", "g2c_out"},
    {S::ClearFolder, "Leeren", "Clear", "清除", "クリア"},
    {S::EnumOk, "ok", "ok", "有", "あり"},
    {S::EnumMissing, "fehlt", "missing", "缺失", "なし"},
    {S::EnumNoTable, "-", "-", "-", "-"},
    {S::EnumTooltipHeader, "Steht der Sequenzname in der anims.h?", "Is the sequence name present in anims.h?",
     "该序列名是否存在于 anims.h 中？", "シーケンス名が anims.h にあるか？"},
    {S::EnumTooltipMissing, "\"%s\" steht nicht in der anims.h.\n"
     "Die Animation landet in der GLA, das Spiel kann sie aber nicht ansprechen.",
     "\"%s\" is not in anims.h.\n"
     "The animation goes into the GLA, but the game cannot reference it.",
     "\"%s\" 不在 anims.h 中。\n该动画会写入 GLA，但游戏无法引用它。",
     "\"%s\" は anims.h にありません。\nGLA には含まれますが、ゲームから参照できません。"},
    {S::EnumTooltipNoTable, "Keine anims.h geladen - links unter \"Enumtabelle\" waehlen.",
     "No anims.h loaded - choose one under \"Enum table\" on the left.",
     "未加载 anims.h - 请在左侧「枚举表」中选择。",
     "anims.h が未読み込みです - 左の「列挙表」で選択してください。"},
    {S::TabIssues, "Meldungen", "Issues", "问题", "メッセージ"},
    {S::TabLog, "Protokoll", "Log", "日志", "ログ"},
    {S::NotValidated, "Noch nicht geprueft.", "Not validated yet.", "尚未检查。", "まだ検証していません。"},
    {S::NoIssues, "Keine Beanstandungen.", "Nothing to report.", "没有问题。", "問題はありません。"},
    {S::ColLevel, "Stufe", "Level", "级别", "レベル"},
    {S::ColMessage, "Meldung", "Message", "消息", "メッセージ"},
    {S::LevelError, "Fehler", "Error", "错误", "エラー"},
    {S::LevelWarning, "Warnung", "Warning", "警告", "警告"},
    {S::LevelInfo, "Hinweis", "Note", "提示", "情報"},
    {S::DlgSource, "Quelldatei", "Source file", "源文件", "ソースファイル"},
    {S::DlgMaster, "Master", "Master", "主序列", "マスター"},
    {S::DlgExtra, "Zusatzsequenzen", "Additional sequences", "附加序列", "追加シーケンス"},
    {S::DlgExtraHint, "Aus -additional: Bereiche derselben Datei, die als eigene Sequenz zaehlen.",
     "From -additional: ranges of the same file counted as their own sequence.",
     "来自 -additional：同一文件中被视为独立序列的片段。",
     "-additional による、同じファイル内の別シーケンス扱いの区間です。"},
    {S::DlgChoose, "Waehlen", "Choose", "选择", "選択"},
    {S::DlgClear, "Loeschen", "Clear", "清除", "クリア"},
    {S::DlgAddExtra, "Zusatzsequenz anlegen", "Add additional sequence", "添加附加序列", "追加シーケンスを作成"},
    {S::DlgClose, "Schliessen", "Close", "关闭", "閉じる"},
    {S::DlgLoopFrame, "Loopframe (-1 = keiner)", "Loop frame (-1 = none)", "循环帧 (-1 = 无)",
     "ループフレーム (-1 = なし)"},
    {S::DlgFrameSpeed, "Framespeed (0 = aus SI_Scene)", "Frame speed (0 = from SI_Scene)", "帧速 (0 = 取自 SI_Scene)",
     "フレーム速度 (0 = SI_Scene から)"},
    {S::DlgEnum, "Enum", "Enum", "枚举", "列挙"},
    {S::DlgInAnims, "in anims.h vorhanden", "present in anims.h", "存在于 anims.h", "anims.h にあります"},
    {S::DlgNotInAnims, "nicht in anims.h", "not in anims.h", "不在 anims.h 中", "anims.h にありません"},
    {S::DlgFrameCount, "Die Datei hat %d Frames", "The file has %d frames", "该文件有 %d 帧",
     "このファイルは %d フレームです"},
    {S::ChooserFilter, "Filtern...", "Filter...", "筛选...", "絞り込み..."},
    {S::ChooserNoTable, "Keine Enumtabelle geladen.", "No enum table loaded.", "未加载枚举表。",
     "列挙表が読み込まれていません。"},
    {S::ChooserNarrow, "... weiter eingrenzen", "... narrow down further", "... 请继续缩小范围",
     "... さらに絞り込んでください"},
    {S::NoScriptOpen, "Kein Skript geoeffnet.", "No script open.", "没有打开的脚本。", "スクリプトが開かれていません。"},
    {S::HintOpenFolder1, "Ordner oeffnen durchsucht einen Baum nach .car-Dateien",
     "Open folder scans a tree for .car files", "「打开文件夹」会在目录树中查找 .car 文件",
     "「フォルダーを開く」はツリーから .car を探します"},
    {S::HintOpenFolder2, "und legt jede als eigenen Tab an.", "and opens each as its own tab.",
     "并为每个文件建立独立标签页。", "それぞれを個別のタブとして開きます。"},
    {S::ScriptsOpen, "%zu Skript(e) geoeffnet", "%zu script(s) open", "已打开 %zu 个脚本",
     "%zu 件のスクリプトを開いています"},
    {S::Reference, "Referenz", "Reference", "参考", "参照"},
    {S::TipDefaultFolder, "Ordner g2c_out neben dieser .car", "Folder g2c_out next to this .car",
     "此 .car 旁的 g2c_out 文件夹", "この .car の隣の g2c_out フォルダー"},
    {S::TipEnumColumn, "Steht der Sequenzname in der anims.h?\n"
     "\"ok\"    = das Spiel kennt ihn und kann die Animation abspielen\n"
     "\"fehlt\" = im Code gibt es kein Enum dafuer, die Animation ist zwar in\n"
     "         der GLA, aber nicht ansprechbar",
     "Is the sequence name present in anims.h?\n"
     "\"ok\"      = the game knows it and can play the animation\n"
     "\"missing\" = no enum in the code; the animation is in the GLA\n"
     "           but cannot be referenced",
     "该序列名是否存在于 anims.h 中？\n"
     "\"有\"   = 游戏认识它，可以播放该动画\n"
     "\"缺失\" = 代码中没有对应枚举；动画虽在 GLA 中，但无法被引用",
     "シーケンス名が anims.h にあるか？\n"
     "「あり」= ゲームが認識し、再生できます\n"
     "「なし」= コードに列挙がなく、GLA には含まれても参照できません"},
    {S::ColLine, "(Zeile %zu)", "(line %zu)", "(第 %zu 行)", "(%zu 行目)"},
    {S::ChooserCount, "%zu Enums", "%zu enums", "%zu 个枚举", "%zu 件の列挙"},
    {S::DlgTitleCar, "Carcass-Skript", "Carcass script", "Carcass 脚本", "Carcass スクリプト"},
    {S::DlgTitleCarFolder, "Ordner mit .car-Dateien", "Folder with .car files", "包含 .car 文件的文件夹",
     ".car ファイルのあるフォルダー"},
    {S::DlgTitleXsi, "Animationsdateien", "Animation files", "动画文件", "アニメーションファイル"},
    {S::DlgTitleXsiFolder, "Ordner mit .xsi-Dateien", "Folder with .xsi files", "包含 .xsi 文件的文件夹",
     ".xsi ファイルのあるフォルダー"},
    {S::DlgTitleOutput, "Ausgabeordner fuer dieses Skript", "Output folder for this script", "此脚本的输出文件夹",
     "このスクリプトの出力フォルダー"},
    {S::DlgTitleGla, "Referenz-GLA", "Reference GLA", "参考 GLA", "参照 GLA"},
    {S::DlgTitleEnums, "Enumtabelle (anims.h)", "Enum table (anims.h)", "枚举表 (anims.h)", "列挙表 (anims.h)"},
    {S::New, "Neu", "New", "新建", "新規"},
    {S::NewCar, "Neues Skript...", "New script...", "新建脚本...", "新しいスクリプト..."},
    {S::MoveUp, "Nach oben", "Move up", "上移", "上へ"},
    {S::MoveDown, "Nach unten", "Move down", "下移", "下へ"},
    {S::MoveTop, "An den Anfang", "Move to top", "移到开头", "先頭へ"},
    {S::MoveBottom, "Ans Ende", "Move to end", "移到末尾", "末尾へ"},
    {S::DeleteSeq, "Sequenz loeschen", "Delete sequence", "删除序列", "シーケンスを削除"},
    {S::DeleteSelected, "%zu ausgewaehlte loeschen", "Delete %zu selected", "删除选中的 %zu 项",
     "選択した %zu 件を削除"},
    {S::CtxEdit, "Bearbeiten...", "Edit...", "编辑...", "編集..."},
    {S::FilterBlocksMove, "Umordnen geht nur ohne Filter - der Filter blendet Zeilen aus.",
     "Reordering requires no filter - the filter hides rows.",
     "排序需要先清除筛选 - 筛选会隐藏行。",
     "並べ替えには絞り込みの解除が必要です - 行が隠れているためです。"},
    {S::DragHint, "Zeile ziehen zum Umordnen", "Drag a row to reorder", "拖动行以排序",
     "行をドラッグして並べ替え"},
    {S::Deleted, "%zu Sequenz(en) geloescht", "%zu sequence(s) deleted", "已删除 %zu 个序列",
     "%zu 件のシーケンスを削除しました"},
    {S::Moved, "Verschoben: %s", "Moved: %s", "已移动：%s", "移動しました: %s"},
    {S::NewCarCreated, "Neues Skript angelegt: %s", "New script created: %s", "已新建脚本：%s",
     "新しいスクリプトを作成しました: %s"},
    {S::DlgTitleNewCar, "Neues Carcass-Skript", "New Carcass script", "新建 Carcass 脚本",
     "新しい Carcass スクリプト"},
    {S::ConfirmDelete, "%zu Sequenz(en) wirklich loeschen?", "Really delete %zu sequence(s)?",
     "确定删除 %zu 个序列吗？", "%zu 件のシーケンスを削除しますか？"},
    {S::Yes, "Loeschen", "Delete", "删除", "削除"},
    {S::No, "Abbrechen", "Cancel", "取消", "中止"},
    {S::InsertHere, "Auswahl hierhin verschieben", "Move selection here",
     "将所选移到此处", "選択をここへ移動"},
    {S::InsertHereCount, "%zu Sequenz(en) hierhin verschieben", "Move %zu sequence(s) here",
     "将 %zu 个序列移到此处", "%zu 件をここへ移動"},
    {S::CutSelection, "Auswahl aufheben", "Clear selection", "取消选择", "選択を解除"},
    {S::SelectionHint, "%zu ausgewaehlt", "%zu selected", "已选 %zu 项", "%zu 件を選択中"},
    {S::ModeBuild, "XSI -> GLA", "XSI -> GLA", "XSI -> GLA", "XSI -> GLA"},
    {S::ModeExtract, "GLA -> XSI", "GLA -> XSI", "GLA -> XSI", "GLA -> XSI"},
    {S::ModeBuildHint, "Skripte bearbeiten und Animationen bauen",
     "Edit scripts and build animations", "编辑脚本并构建动画",
     "スクリプトを編集してアニメーションをビルド"},
    {S::ModeExtractHint, "Animationen aus einer fertigen GLA herausloesen",
     "Extract animations from a finished GLA", "从现成的 GLA 中提取动画",
     "完成済みの GLA からアニメーションを取り出す"},
    {S::OpenGla, "GLA oeffnen", "Open GLA", "打开 GLA", "GLA を開く"},
    {S::OpenCfg, "animation.cfg", "animation.cfg", "animation.cfg", "animation.cfg"},
    {S::GlaInfo, "%d Frames, %zu Bones", "%d frames, %zu bones", "%d 帧，%zu 个骨骼",
     "%d フレーム、%zu ボーン"},
    {S::NoGlaOpen, "Keine GLA geoeffnet.", "No GLA open.", "未打开 GLA。",
     "GLA が開かれていません。"},
    {S::NoGlaHint, "GLA oeffnen, dann die animation.cfg dazu - sie nennt die Sequenzen.",
     "Open a GLA, then its animation.cfg - it names the sequences.",
     "先打开 GLA，再打开其 animation.cfg - 它给出各个序列。",
     "GLA を開き、その animation.cfg も開いてください - シーケンス名が入っています。"},
    {S::ColStart, "Start", "Start", "起始", "開始"},
    {S::ColCount, "Frames", "Frames", "帧数", "フレーム"},
    {S::ColFps, "Rate", "Rate", "速率", "レート"},
    {S::ColLoopFrame, "Loop", "Loop", "循环", "ループ"},
    {S::ExportSelected, "%zu ausgewaehlte exportieren", "Export %zu selected",
     "导出选中的 %zu 项", "選択した %zu 件を書き出す"},
    {S::ExportAll, "Alle exportieren", "Export all", "全部导出", "すべて書き出す"},
    {S::ExportTarget, "Zielordner", "Target folder", "目标文件夹", "出力フォルダー"},
    {S::Exported, "%zu .xsi geschrieben", "%zu .xsi written", "已写入 %zu 个 .xsi",
     "%zu 件の .xsi を書き出しました"},
    {S::ExportFailed, "%zu fehlgeschlagen", "%zu failed", "%zu 项失败", "%zu 件が失敗"},
    {S::SeqCount, "%zu Sequenzen", "%zu sequences", "%zu 个序列", "%zu 件のシーケンス"},
    {S::NoCfgWarning, "Ohne animation.cfg laesst sich nur alles am Stueck exportieren.",
     "Without animation.cfg only the whole thing can be exported.",
     "没有 animation.cfg 时只能整体导出。",
     "animation.cfg がないと全体を一括でしか書き出せません。"},
    {S::NoFramesHint, "keine .frames (nur fuer Wurzelbewegung noetig)",
     "no .frames (only needed for root motion)", "无 .frames（仅根运动需要）",
     ".frames なし（ルートモーションにのみ必要）"},
    {S::NotAFile, "Das ist ein Ordner, keine Animationsdatei:",
     "That is a folder, not an animation file:", "这是文件夹，不是动画文件：",
     "これはフォルダーであり、アニメーションファイルではありません:"},
    {S::NotAnXsi, "Keine .xsi-Datei:", "Not a .xsi file:", "不是 .xsi 文件：",
     ".xsi ファイルではありません:"},
    {S::FileGone, "Datei gibt es nicht (mehr):", "File does not exist:", "文件不存在：",
     "ファイルが存在しません:"},
    {S::MissingHead, "%zu von %zu Animationsdateien nicht gefunden.",
     "%zu of %zu animation files not found.", "%zu / %zu 个动画文件未找到。",
     "%zu / %zu 件のアニメーションファイルが見つかりません。"},
    {S::MissingSearched, "Gesucht unter:", "Searched under:", "查找位置：", "検索場所:"},
    {S::MissingList, "Es fehlen:", "Missing:", "缺少：", "不足しているもの:"},
    {S::MissingHintBase,
     "Assetwurzel pruefen: der Ordner, unter dem \"models\\\" liegt.",
     "Check the asset root: the folder that contains \"models\\\".",
     "请检查资源根目录：包含 \"models\\\" 的文件夹。",
     "アセットルートを確認してください: \"models\\\" を含むフォルダーです。"},
    {S::MissingHintSkip,
     "Trotzdem bauen verschiebt alle nachfolgenden Zielframes - die animation.cfg "
     "passt dann nicht mehr zur GLA.",
     "Building anyway shifts every following target frame - animation.cfg will no longer "
     "match the GLA.",
     "强行构建会使后续所有目标帧位移 - animation.cfg 将与 GLA 不再匹配。",
     "それでもビルドすると以降の対象フレームがすべてずれ、animation.cfg が GLA と一致しなくなります。"},
    {S::MissingMore, "... und %zu weitere", "... and %zu more", "... 还有 %zu 个",
     "... 他 %zu 件"},
    {S::SeqLabel, "Sequenz", "Sequence", "序列", "シーケンス"},
    {S::ExportAllWithCar, "Alles + .car", "Everything + .car", "全部 + .car",
     "すべて + .car"},
    {S::ExportAllWithCarTip,
     "Exportiert alle Sequenzen und schreibt ein .car dazu, mit dem sich die GLA "
     "sofort wieder bauen laesst.\nLoopframe und Rate kommen aus der animation.cfg, "
     "muessen also nicht geraten werden.",
     "Exports every sequence and writes a .car that rebuilds the GLA right away.\n"
     "Loop frame and rate come from animation.cfg, so nothing has to be guessed.",
     "导出所有序列并生成可立即重新构建 GLA 的 .car。\n循环帧和速率取自 animation.cfg，无需猜测。",
     "すべてのシーケンスを書き出し、GLA をすぐ再構築できる .car も作成します。\n"
     "ループフレームと速度は animation.cfg から取得するため、推測は不要です。"},
    {S::Grouped, "%zu Master, %zu Unterbereiche als -additional",
     "%zu masters, %zu sub-ranges as -additional", "%zu 个主序列，%zu 个子区间作为 -additional",
     "マスター %zu 件、サブ範囲 %zu 件を -additional として"},
    {S::CarWritten, "Skript geschrieben: %s", "Script written: %s", "已写入脚本：%s",
     "スクリプトを書き出しました: %s"},
    {S::PartialWarn, "%zu Sequenzen ueberlappen nur teilweise und fehlen im Skript.",
     "%zu sequences overlap only partially and are missing from the script.",
     "%zu 个序列仅部分重叠，未写入脚本。",
     "%zu 件のシーケンスは部分的にしか重ならないため、スクリプトに含まれません。"},
    {S::Compare, "Vergleichen mit...", "Compare with...", "与...比较", "比較対象を選択..."},
    {S::CompareTip,
     "Eine zweite animation.cfg laden - etwa die von JKA - und sehen, welche Sequenzen\n"
     "dort FEHLEN. Genau die sind es, die man uebernehmen will.",
     "Load a second animation.cfg - JKA's for instance - and see which sequences are\n"
     "MISSING there. Those are the ones worth taking over.",
     "加载第二个 animation.cfg（例如 JKA 的），查看其中缺少哪些序列。\n那些正是值得移植的。",
     "2 つ目の animation.cfg（例えば JKA のもの）を読み込み、そこに無いシーケンスを表示します。\n"
     "移植したいのはまさにそれらです。"},
    {S::CompareLoaded, "Verglichen mit %s: %zu Sequenzen dort",
     "Compared with %s: %zu sequences there", "与 %s 比较：其中有 %zu 个序列",
     "%s と比較: あちらには %zu 件のシーケンス"},
    {S::CompareCol, "Dort", "There", "对方", "相手"},
    {S::OnlyMissing, "nur fehlende", "only missing", "仅显示缺少的", "不足分のみ"},
    {S::SelectMissing, "Alle fehlenden auswaehlen", "Select all missing", "选择所有缺少的",
     "不足分をすべて選択"},
    {S::CompareNone, "Kein Vergleich geladen", "No comparison loaded", "未加载比较",
     "比較対象が未読み込みです"},
    {S::CompareCount, "%zu fehlen dort", "%zu missing there", "对方缺少 %zu 个",
     "あちらに %zu 件不足"},
    {S::MissingHere, "fehlt", "missing", "缺少", "なし"},
    {S::PresentHere, "vorhanden", "present", "有", "あり"},
    {S::LogReady, "g2c bereit. Ordner oeffnen oder .car hineinziehen.",
     "g2c ready. Open a folder or drop a .car here.", "g2c 就绪。打开文件夹或拖入 .car。",
     "g2c の準備ができました。フォルダーを開くか .car をドロップしてください。"},
    {S::LogRestored, "%zu Skript(e) wiederhergestellt", "%zu script(s) restored",
     "已恢复 %zu 个脚本", "%zu 件のスクリプトを復元しました"},
    {S::LogAlreadyOpen, "Bereits geoeffnet: %s", "Already open: %s", "已打开：%s",
     "すでに開いています: %s"},
    {S::LogAssetRootFound, "Assetwurzel erkannt: %s", "Asset root detected: %s",
     "识别到资源根目录：%s", "アセットルートを検出: %s"},
    {S::LogRefGlaFound, "Referenz-GLA erkannt: %s", "Reference GLA detected: %s",
     "识别到参考 GLA：%s", "参照 GLA を検出: %s"},
    {S::LogRefGlaSet, "Referenz-GLA gesetzt: %s", "Reference GLA set: %s", "已设置参考 GLA：%s",
     "参照 GLA を設定: %s"},
    {S::LogFound, "Gefunden: %s", "Found: %s", "已找到：%s", "見つかりました: %s"},
    {S::LogNoCfgNearby,
     "Keine animation.cfg neben der GLA - ohne sie gibt es keine Sequenzgrenzen.",
     "No animation.cfg next to the GLA - without it there are no sequence boundaries.",
     "GLA 旁没有 animation.cfg - 没有它就没有序列边界。",
     "GLA の隣に animation.cfg がありません - これがないとシーケンス境界が分かりません。"},
    {S::LogEnumsLoaded, "anims.h: %zu Eintraege", "anims.h: %zu entries", "anims.h：%zu 个条目",
     "anims.h: %zu 件"},
    // Placeholders in EVERY language in the same order as the arguments
    // (%zu, then %s). snprintf has no positional specifiers; in the old
    // Chinese and Japanese versions %s came first, and the number was read
    // as a pointer - a crash when adding a folder.
    {S::LogXsiInFolder, "%zu .xsi in %s", "%zu .xsi in %s", "%zu 个 .xsi（位于 %s）",
     "%zu 件の .xsi（%s）"},
    {S::LogNoXsiIn, "Keine .xsi in %s", "No .xsi in %s", "%s 中没有 .xsi",
     "%s に .xsi がありません"},
    {S::LogAddedTo, "%zu Datei(en) zu %zu Skript(en) hinzugefuegt",
     "%zu file(s) added to %zu script(s)", "已将 %zu 个文件添加到 %zu 个脚本",
     "%zu 件のファイルを %zu 件のスクリプトに追加しました"},
    {S::LogOutputsSet, "%zu Ausgabeordner gesetzt (g2c_out je Skript)",
     "%zu output folders set (g2c_out per script)", "已设置 %zu 个输出文件夹（每个脚本一个 g2c_out）",
     "%zu 件の出力フォルダーを設定しました（スクリプトごとの g2c_out）"},
    {S::LogSaved, "%s gespeichert (Sicherung: %s)", "%s saved (backup: %s)",
     "已保存 %s（备份：%s）", "%s を保存しました（バックアップ: %s）"},
    {S::LogExists, "%s gibt es schon", "%s already exists", "%s 已存在", "%s は既に存在します"},
    {S::LogCannotRead, "Kann %s nicht lesen", "Cannot read %s", "无法读取 %s",
     "%s を読み込めません"},
    {S::LogCannotWrite, "Kann %s nicht schreiben", "Cannot write %s", "无法写入 %s",
     "%s を書き込めません"},
    {S::LogGlaUnreadable, "GLA nicht lesbar: %s", "GLA not readable: %s", "GLA 无法读取：%s",
     "GLA を読み込めません: %s"},
    {S::LogNoSeqIn, "Keine Sequenzen in %s", "No sequences in %s", "%s 中没有序列",
     "%s にシーケンスがありません"},
    {S::LogOpenGlaFirst, "Erst eine GLA oeffnen", "Open a GLA first", "请先打开 GLA",
     "先に GLA を開いてください"},
    {S::LogNotFound, "Nicht gefunden: %s", "Not found: %s", "未找到：%s", "見つかりません: %s"},
    {S::LogHowToOpen, "Womit soll ich %s oeffnen?", "How should %s be opened?",
     "该如何打开 %s？", "%s はどう開けばよいですか？"},
    {S::LogNothingToBuild, "Nichts zu bauen", "Nothing to build", "没有可构建的内容",
     "ビルドするものがありません"},
    {S::LogNothingToSave, "Nichts zu speichern", "Nothing to save", "没有可保存的内容",
     "保存するものがありません"},
    {S::LogNoOutputDir, "Kein Ausgabeordner gesetzt", "No output folder set", "未设置输出文件夹",
     "出力フォルダーが未設定です"},
    {S::LogNoRefGla, "Keine Referenz-GLA gesetzt", "No reference GLA set", "未设置参考 GLA",
     "参照 GLA が未設定です"},
    {S::LogMeshSourceMissing, "%s: Mesh-Quelle nicht gefunden", "%s: mesh source not found",
     "%s：未找到网格源", "%s: メッシュのソースが見つかりません"},
    {S::LogScriptNotWritable, "Skript nicht schreibbar: %s", "Script not writable: %s",
     "脚本无法写入：%s", "スクリプトを書き込めません: %s"},
    {S::LogBuilt, "%s -> %s", "%s -> %s", "%s -> %s", "%s -> %s"},
    {S::LogFrameCounts, "Framezahlen: %zu gelesen, %zu nicht lesbar",
     "Frame counts: %zu read, %zu unreadable", "帧数：已读取 %zu，无法读取 %zu",
     "フレーム数: %zu 件を読み込み、%zu 件は読み込めません"},
    {S::LogFramesNotFound,
     "Keine der %zu Quelldateien gefunden - stimmt die Assetwurzel?",
     "None of the %zu source files found - is the asset root correct?",
     "%zu 个源文件均未找到 - 资源根目录是否正确？",
     "%zu 件のソースファイルがいずれも見つかりません - アセットルートは正しいですか？"},
    {S::LogValidation, "Pruefung: %zu Fehler, %zu Warnungen",
     "Validation: %zu errors, %zu warnings", "检查：%zu 个错误，%zu 个警告",
     "検証: エラー %zu 件、警告 %zu 件"},
    {S::LogAllCores, "alle Kerne", "all cores", "全部核心", "全コア"},
    {S::LogOpened, "%s geoeffnet (%zu Grabs)", "%s opened (%zu grabs)", "已打开 %s（%zu 个抓取）",
     "%s を開きました（%zu 件の取り込み）"},
    {S::LogGlaOpened, "%s: %d Frames, %zu Bones", "%s: %d frames, %zu bones",
     "%s：%d 帧，%zu 个骨骼", "%s: %d フレーム、%zu ボーン"},
    {S::LogFramesLoaded, "%zu Sequenzen mit Wurzelbewegung aus %s",
     "%zu sequences with root motion from %s", "%zu 个带根运动的序列（来自 %s）",
     "%zu 件のルートモーション付きシーケンス（%s から）"},
    {S::LogDeleted, "%zu Sequenz(en) geloescht", "%zu sequence(s) deleted", "已删除 %zu 个序列",
     "%zu 件のシーケンスを削除しました"},
    {S::LogMoved, "%zu verschoben", "%zu moved", "已移动 %zu 个", "%zu 件を移動しました"},
    {S::LogSeqsRead, "%zu Sequenzen gelesen%s", "%zu sequences read%s", "已读取 %zu 个序列%s",
     "%zu 件のシーケンスを読み込みました%s"},
    {S::LogNoOutputAt,
     "Kein Ausgabeordner bei: %s - ueber der Tabelle setzen oder \"Alle auf g2c_out\" benutzen",
     "No output folder for: %s - set it above the table or use \"Set all to g2c_out\"",
     "以下脚本未设置输出文件夹：%s - 请在表格上方设置，或使用「全部设为 g2c_out」",
     "出力フォルダー未設定: %s - 表の上で設定するか「すべてを g2c_out に」を使ってください"},
    {S::LogUnreadable, "Konnte nicht gelesen werden:", "Could not be read:", "无法读取：",
     "読み込めませんでした:"},
    {S::LogAddedFiles, "%zu Datei(en) zu %zu Skript(en) hinzugefuegt",
     "%zu file(s) added to %zu script(s)", "已将 %zu 个文件添加到 %zu 个脚本",
     "%zu 件のファイルを %zu 件のスクリプトに追加しました"},
    {S::Copy, "Kopieren", "Copy", "复制", "コピー"},
    {S::Cut, "Ausschneiden", "Cut", "剪切", "切り取り"},
    {S::PasteBefore, "%zu hier einfuegen (davor)", "Paste %zu here (before)", "在此前粘贴 %zu 项",
     "%zu 件をこの前に貼り付け"},
    {S::PasteAfter, "%zu hier einfuegen (danach)", "Paste %zu here (after)", "在此后粘贴 %zu 项",
     "%zu 件をこの後に貼り付け"},
    {S::PasteEnd, "%zu ans Ende einfuegen", "Paste %zu at the end", "在末尾粘贴 %zu 项",
     "%zu 件を末尾に貼り付け"},
    {S::ClipEmpty, "Zwischenablage leer", "Clipboard empty", "剪贴板为空",
     "クリップボードは空です"},
    {S::Copied, "%zu Sequenz(en) kopiert", "%zu sequence(s) copied", "已复制 %zu 个序列",
     "%zu 件のシーケンスをコピーしました"},
    {S::Pasted, "%zu Sequenz(en) eingefuegt", "%zu sequence(s) pasted", "已粘贴 %zu 个序列",
     "%zu 件のシーケンスを貼り付けました"},
    {S::ClipHint, "%zu in der Zwischenablage", "%zu on the clipboard", "剪贴板中有 %zu 项",
     "クリップボードに %zu 件"},
    {S::About, "Ueber g2c", "About g2c", "关于 g2c", "g2c について"},
    {S::AboutBuilt, "Gebaut: %s %s", "Built: %s %s", "构建于：%s %s", "ビルド: %s %s"},
    {S::AboutBits, "%zu Bit, %u Kerne", "%zu bit, %u cores", "%zu 位，%u 个核心",
     "%zu ビット、%u コア"},
    {S::AboutRuntimeOk,
     "Laufzeit fest eingebaut - laeuft auf jedem Windows 10/11 ohne Visual C++ Redistributable.",
     "Runtime linked statically - runs on any Windows 10/11 without the Visual C++ Redistributable.",
     "运行库已静态链接 - 无需 Visual C++ Redistributable 即可在任何 Windows 10/11 上运行。",
     "ランタイムを静的リンク済み - Visual C++ 再頒布可能パッケージなしで Windows 10/11 で動作します。"},
    {S::AboutRuntimeBad,
     "ACHTUNG: Laufzeit dynamisch gebunden. Auf Rechnern ohne Visual C++ Redistributable "
     "startet das Programm NICHT.",
     "WARNING: runtime linked dynamically. On machines without the Visual C++ Redistributable "
     "this program will NOT start.",
     "警告：运行库为动态链接。在没有 Visual C++ Redistributable 的机器上本程序无法启动。",
     "警告: ランタイムが動的リンクです。Visual C++ 再頒布可能パッケージが無い環境では起動しません。"},
    {S::SavedBeforeBuild, "%s vor dem Bauen gespeichert",
     "%s saved before building", "构建前已保存 %s", "ビルド前に %s を保存しました"},
    {S::SameOutDir,
     "%s und %s schreiben in denselben Ordner - die zweite GLA ueberschreibt die erste.",
     "%s and %s write to the same folder - the second GLA overwrites the first.",
     "%s 与 %s 输出到同一文件夹 - 第二个 GLA 会覆盖第一个。",
     "%s と %s の出力先が同じです - 2 つ目の GLA が 1 つ目を上書きします。"},
    {S::CfgBelongsWithGla,
     "animation.cfg gehoert ZUSAMMEN mit der GLA kopiert - sonst spielt das Spiel "
     "bei jedem Namen die Animation ab, die dort zufaellig steht.",
     "animation.cfg must be copied TOGETHER with the GLA - otherwise the game plays "
     "whatever animation happens to sit at each frame.",
     "animation.cfg 必须与 GLA 一起复制 - 否则游戏会播放该帧位置上碰巧存在的动画。",
     "animation.cfg は GLA と一緒にコピーしてください - さもないとそのフレーム位置に"
     "たまたまあるアニメーションが再生されます。"},
    {S::CopyLog, "Protokoll kopieren", "Copy log", "复制日志", "ログをコピー"},
    {S::CopyLogTip,
     "Das ganze Protokoll in die Zwischenablage - zum Einfuegen in einen Fehlerbericht.",
     "The whole log to the clipboard - for pasting into a bug report.",
     "将整个日志复制到剪贴板 - 便于粘贴到错误报告中。",
     "ログ全体をクリップボードへ - 不具合報告への貼り付け用です。"},
    {S::LogCopied, "%zu Zeilen kopiert", "%zu lines copied", "已复制 %zu 行",
     "%zu 行をコピーしました"},
    {S::ClearLog, "Leeren", "Clear", "清空", "消去"},
    {S::BasePoseLabel, "Bindepose", "Base pose", "绑定姿势", "バインドポーズ"},
    {S::BasePoseTip,
     "Welche Bindepose in den BASEPOSE-Block der .xsi geschrieben wird.\n\n"
     "Nur wichtig, wenn die Dateien durch Ravens Carcass sollen - g2c selbst\n"
     "baut mit jeder Einstellung gleich.\n\n"
     "Welt: wie in Ravens Dateien. Lokal: gegen den Elternbone, falls Carcass\n"
     "selbst verkettet. Keine: gar kein Block.",
     "Which base pose goes into the .xsi BASEPOSE block.\n\n"
     "Only matters if the files are meant for Raven's Carcass - g2c itself\n"
     "builds the same either way.\n\n"
     "World: as in Raven's files. Local: against the parent bone, in case\n"
     "Carcass chains them itself. None: no block at all.",
     "写入 .xsi 的 BASEPOSE 块使用哪种绑定姿势。\n\n"
     "仅在文件需要经过 Raven 的 Carcass 时才重要 - g2c 自身构建结果相同。\n\n"
     "世界：与 Raven 的文件一致。局部：相对父骨骼。无：不写入该块。",
     ".xsi の BASEPOSE ブロックにどのバインドポーズを書くか。\n\n"
     "Raven の Carcass に通す場合のみ重要です - g2c 自身のビルド結果は変わりません。\n\n"
     "ワールド: Raven のファイルと同じ。ローカル: 親ボーン基準。なし: ブロックを書きません。"},
    {S::BasePoseWorld, "Welt", "World", "世界", "ワールド"},
    {S::BasePoseLocal, "Lokal", "Local", "局部", "ローカル"},
    {S::BasePoseNone, "Keine", "None", "无", "なし"},
    {S::GlaNameFromMakeSkel, "GLA-Name aus -makeskel: %s",
     "GLA name from -makeskel: %s", "GLA 名称取自 -makeskel：%s",
     "GLA 名は -makeskel より: %s"},
    {S::KeepBackup, "Sicherung der .car anlegen", "Keep a .car backup",
     "保留 .car 备份", ".car のバックアップを作成"},
    {S::KeepBackupTip,
     "Legt beim ERSTEN Speichern einmalig eine .car.bak an - den Stand vor der\n"
     "ersten Bearbeitung. Danach nicht mehr.\n\n"
     "Die animation.cfg wird nie gesichert: sie entsteht bei jedem Bau neu.",
     "Creates a .car.bak once, on the FIRST save - the state before your first\n"
     "edit. Not afterwards.\n\n"
     "animation.cfg is never backed up: it is regenerated on every build.",
     "首次保存时创建一次 .car.bak - 即首次编辑前的状态，之后不再创建。\n\n"
     "animation.cfg 从不备份：每次构建都会重新生成。",
     "最初の保存時に一度だけ .car.bak を作成します - 最初の編集前の状態です。\n\n"
     "animation.cfg はバックアップしません: ビルドのたびに再生成されます。"},
    {S::AboutLogPath, "Startprotokoll: %s", "Startup log: %s", "启动日志：%s",
     "起動ログ: %s"},
    {S::XsiVersionTip,
     "Fassung der erzeugten .xsi.\n\n"
     "3.0 benennt die Templates wie Ravens root.xsi.\n"
     "3.5 laesst sie namenlos wie Ravens Animationsdateien.\n\n"
     "Der Inhalt ist derselbe. Aeltere Werkzeuge erwarten teils 3.0.",
     "Version of the generated .xsi.\n\n"
     "3.0 names its templates, like Raven's root.xsi.\n"
     "3.5 leaves them unnamed, like Raven's animation files.\n\n"
     "The content is the same. Some older tools expect 3.0.",
     "生成的 .xsi 版本。\n\n3.0 为模板命名，与 Raven 的 root.xsi 一致。\n"
     "3.5 不命名，与 Raven 的动画文件一致。\n\n内容相同。部分旧工具需要 3.0。",
     "生成する .xsi のバージョン。\n\n3.0 は Raven の root.xsi と同様にテンプレートに名前を付けます。\n"
     "3.5 は名前を付けません。\n\n内容は同じです。古いツールでは 3.0 が必要な場合があります。"},
    {S::FoldersAdded, "%zu Ordner durchsucht, %zu Datei(en) hinzugefuegt",
     "%zu folders scanned, %zu file(s) added", "已扫描 %zu 个文件夹，添加 %zu 个文件",
     "%zu 個のフォルダーを検索し、%zu 件のファイルを追加しました"},
    {S::OpenOutputDir, "Ausgabeordner oeffnen", "Open output folder", "打开输出文件夹",
     "出力フォルダーを開く"},
    {S::RefIsTarget,
     "%s: Die Referenz-GLA liegt im Ausgabeordner und wuerde ueberschrieben. "
     "Sie wird gelesen und gleichzeitig beschrieben - Windows sperrt die Datei. "
     "Anderen Ausgabeordner waehlen oder die Referenz woanders hinlegen.",
     "%s: The reference GLA sits in the output folder and would be overwritten. "
     "It is read and written at the same time - Windows locks the file. "
     "Choose a different output folder, or move the reference elsewhere.",
     "%s：参考 GLA 位于输出文件夹中并会被覆盖。读写同时进行会导致 Windows 锁定该文件。"
     "请选择其他输出文件夹，或将参考文件移至别处。",
     "%s: 参照 GLA が出力フォルダーにあり、上書きされます。読み書きが同時に行われ "
     "Windows がファイルをロックします。別の出力フォルダーを選ぶか、参照を移動してください。"},
    {S::RefMissing,
     "Die Referenz-GLA \"%s\" gibt es nicht. Sie liefert das SKELETT - Bonenamen, "
     "Hierarchie, Bindeposen, Skalierung. Die .xsi-Dateien enthalten nur "
     "Animationsdaten; ohne Skelett laesst sich keine GLA schreiben. Eine "
     "vorhandene GLA mit demselben Skelett auswaehlen.",
     "The reference GLA \"%s\" does not exist. It provides the SKELETON - bone "
     "names, hierarchy, base poses, scale. The .xsi files hold animation data "
     "only; without a skeleton no GLA can be written. Pick any existing GLA with "
     "the same skeleton.",
     "\u53c2\u8003 GLA \"%s\" \u4e0d\u5b58\u5728\u3002\u5b83\u63d0\u4f9b\u9aa8\u67b6\u3002"
     "\u8bf7\u9009\u62e9\u5177\u6709\u76f8\u540c\u9aa8\u67b6\u7684\u73b0\u6709 GLA\u3002",
     "\u53c2\u7167 GLA \"%s\" \u304c\u5b58\u5728\u3057\u307e\u305b\u3093\u3002"
     "\u540c\u3058\u30b9\u30b1\u30eb\u30c8\u30f3\u306e\u65e2\u5b58 GLA \u3092\u9078\u3093\u3067\u304f\u3060\u3055\u3044\u3002"},
    {S::BuildStoppedDup,
     "Bau abgebrochen: %zu Sequenzname(n) kommen mehrfach vor. Unter \"Issues\" "
     "anklicken - der Sprung geht der Reihe nach zu jedem Vorkommen.",
     "Build stopped: %zu sequence name(s) appear more than once. Click them under "
     "\"Issues\" - each click jumps to the next occurrence.",
     "\u6784\u5efa\u5df2\u505c\u6b62\uff1a%zu \u4e2a\u5e8f\u5217\u540d\u91cd\u590d\u3002",
     "\u30d3\u30eb\u30c9\u4e2d\u6b62: %zu \u4ef6\u306e\u30b7\u30fc\u30b1\u30f3\u30b9\u540d\u304c\u91cd\u8907\u3057\u3066\u3044\u307e\u3059\u3002"},
    {S::DlgComment, "Kommentar davor", "Comment above", "上方注释", "上のコメント"},
    {S::DlgCommentTip,
     "Steht im Skript VOR dieser Sequenz und landet so auch in der\n"
     "erzeugten animation.cfg.\n\n"
     "Eine Zeile je Zeile. \"//\" wird vorangestellt, falls es fehlt.\n"
     "Damit laesst sich eine lange Liste gliedern, statt eine Wand aus\n"
     "Zahlen zu hinterlassen.",
     "Goes into the script ABOVE this sequence, and from there into the\n"
     "generated animation.cfg.\n\n"
     "One line per line. \"//\" is prepended if missing.\n"
     "Use it to break up a long list instead of leaving a wall of numbers.",
     "写在脚本中该序列的上方，并会出现在生成的 animation.cfg 中。\n\n"
     "每行一条。若缺少 \"//\" 会自动添加。",
     "スクリプト内でこのシーケンスの上に置かれ、生成される animation.cfg にも反映されます。\n\n"
     "1 行につき 1 件。\"//\" が無ければ自動で付きます。"},
    {S::AddDivider, "Trennlinie davor", "Divider above", "在上方添加分隔线",
     "上に区切り線"},
    {S::AddComment, "Kommentar davor...", "Comment above...", "在上方添加注释...",
     "上にコメント..."},
    {S::ColComment, "Kommentar", "Comment", "注释", "コメント"},
    {S::CommentEditHint,
     "Doppelklick zum Bearbeiten, Rechtsklick zum Loeschen.\nLeer lassen loescht die Zeile.",
     "Double-click to edit, right-click to delete.\nLeaving it empty removes the line.",
     "双击编辑，右键删除。\n留空则删除该行。",
     "ダブルクリックで編集、右クリックで削除。\n空にすると行が消えます。"},
    {S::TrailCommentTip,
     "Hinweis zu dieser Animation. Steht in der .car hinter der Zeile und\n"
     "landet in der animation.cfg an derselben Stelle.\n\nDoppelklick zum Bearbeiten.",
     "A note about this animation. Sits after the line in the .car and ends up\n"
     "in the same place in animation.cfg.\n\nDouble-click to edit.",
     "关于此动画的说明。位于 .car 行尾，并出现在 animation.cfg 的相同位置。\n\n双击编辑。",
     "このアニメーションへの注記。.car の行末に置かれ、animation.cfg の同じ位置に出力されます。\n\nダブルクリックで編集。"},
    {S::ModePreview, "Vorschau", "Preview", "预览", "プレビュー"},
    {S::ModePreviewHint, "Skelett einer Sequenz abspielen",
     "Play back a sequence's skeleton", "播放某个序列的骨架",
     "シーケンスのスケルトンを再生"},
    {S::PreviewNoGla, "Erst im Modus GLA -> XSI eine GLA oeffnen.",
     "Open a GLA in the GLA -> XSI mode first.", "请先在 GLA -> XSI 模式中打开 GLA。",
     "先に GLA -> XSI モードで GLA を開いてください。"},
    {S::PreviewPlay, "Abspielen", "Play", "播放", "再生"},
    {S::PreviewPause, "Anhalten", "Pause", "暂停", "一時停止"},
    {S::PreviewFrame, "Frame %d/%d", "Frame %d/%d", "第 %d/%d 帧", "フレーム %d/%d"},
    {S::PreviewReset, "Ansicht zuruecksetzen", "Reset view", "重置视角", "視点をリセット"},
    {S::PreviewHint, "Ziehen dreht, Mausrad zoomt", "Drag to orbit, wheel to zoom",
     "拖动旋转，滚轮缩放", "ドラッグで回転、ホイールでズーム"},
    {S::PreviewBones, "%zu Bones", "%zu bones", "%zu 个骨骼", "%zu ボーン"},
    {S::PreviewSeq, "Sequenz waehlen", "Choose sequence", "选择序列", "シーケンスを選択"},
    {S::OpenFrames, ".frames", ".frames", ".frames", ".frames"},
    {S::OriginLabel, "Origin", "Origin", "原点", "原点"},
    {S::OriginDetected, "erkannt: %.0f %.0f %.0f", "detected: %.0f %.0f %.0f",
     "已识别：%.0f %.0f %.0f", "検出: %.0f %.0f %.0f"},
    {S::OriginNone, "keiner", "none", "无", "なし"},
    {S::FramesMissing,
     "Ohne .frames fehlt die Wurzelbewegung - Laufanimationen laufen auf der Stelle.",
     "Without .frames the root motion is missing - walk cycles run in place.",
     "没有 .frames 就缺少根位移 - 行走动画会原地踏步。",
     ".frames がないとルートモーションが欠け、歩行アニメがその場歩きになります。"},
    {S::FramesLoaded, "%zu mit Wurzelbewegung", "%zu with root motion", "%zu 项含根位移",
     "%zu 件にルートモーション"},
    {S::NotValidatedHint, "Noch nicht geprueft - Schaltflaeche \"Pruefen\".",
     "Not validated yet - use the \"Validate\" button.",
     "尚未检查 - 请使用「检查」按钮。", "未検証です - 「検証」ボタンを使ってください。"},
    {S::EnumsAvailable, "Links unter \"Enumtabelle (anims.h)\" eine Datei waehlen.",
     "Choose a file under \"Enum table (anims.h)\" on the left.",
     "请在左侧「枚举表 (anims.h)」中选择文件。",
     "左の「列挙表 (anims.h)」でファイルを選んでください。"},
    {S::DlgStart, "Start", "Start", "起始", "開始"},
    {S::SpeedAuto, "auto", "auto", "自动", "自動"},
    {S::BuildProgress, "%zu/%zu  %s", "%zu/%zu  %s", "%zu/%zu  %s", "%zu/%zu  %s"},

    {S::UnsavedHead, "%zu Skript(e) mit ungespeicherten Aenderungen:",
     "%zu script(s) with unsaved changes:", "%zu 个脚本有未保存的更改：",
     "%zu 件のスクリプトに未保存の変更があります:"},
    {S::Discard, "Verwerfen", "Discard", "放弃", "破棄"},
    {S::OverwriteHead, "%zu Datei(en) gibt es im Zielordner schon; sie wuerden ueberschrieben:",
     "%zu file(s) already exist in the target folder and would be overwritten:",
     "目标文件夹中已有 %zu 个文件，将被覆盖：", "出力先に %zu 件のファイルが既にあり、上書きされます:"},
    {S::Overwrite, "Ueberschreiben", "Overwrite", "覆盖", "上書き"},
    {S::LogBackedUp, "Vorhandene Datei gesichert: %s", "Existing file backed up: %s",
     "已备份现有文件：%s", "既存ファイルをバックアップしました: %s"},
    {S::DragOtherTab,
     "Zeilen lassen sich nicht in ein anderes Skript ziehen - dafuer Kopieren und Einfuegen "
     "benutzen",
     "Rows cannot be dragged into another script - use Copy and Paste instead",
     "不能把行拖到另一个脚本中——请使用复制和粘贴",
     "行を別のスクリプトへドラッグすることはできません。コピーと貼り付けを使ってください"},
    {S::NErrors, "%zu Fehler", "%zu errors", "%zu 个错误", "エラー %zu 件"},
    {S::NWarnings, "%zu Warnungen", "%zu warnings", "%zu 个警告", "警告 %zu 件"},
    {S::Validated, "geprueft", "validated", "已检查", "検証済み"},
    {S::DlgSeqTitle, "Sequenz: %s", "Sequence: %s", "序列：%s", "シーケンス: %s"},
    {S::LogCarsUnder, "%zu .car-Dateien unter %s", "%zu .car files under %s",
     "%zu 个 .car 文件（位于 %s）", "%zu 件の .car（%s）"},
    {S::DupInCfg,
     "%s: %s steht %dx in der animation.cfg - die Engine nimmt den letzten, die uebrigen sind "
     "unerreichbar.",
     "%s: %s appears %dx in animation.cfg - the engine uses the last one, the others are "
     "unreachable.",
     "%s：%s 在 animation.cfg 中出现 %d 次——引擎只使用最后一个，其余无法访问。",
     "%s: %s は animation.cfg に %d 回あります。エンジンは最後のものだけを使い、残りは使われません。"},
    {S::DupIssue,
     "%s kommt mehrfach vor - umbenennen oder eine Fassung loeschen. Jeder Klick springt zum "
     "naechsten Vorkommen.",
     "%s occurs more than once - rename or delete one. Each click jumps to the next occurrence.",
     "%s 出现多次——请重命名或删除其中一个。每次点击跳到下一处。",
     "%s が複数あります。名前を変えるか一方を削除してください。クリックするたびに次の箇所へ移動します。"},
    {S::LogWriteFailed, "%s: Schreiben fehlgeschlagen - %s", "%s: writing failed - %s",
     "%s：写入失败——%s", "%s: 書き込みに失敗しました - %s"},
    {S::LogSeqsSkipped, ", %zu ausserhalb der GLA uebersprungen", ", %zu outside the GLA skipped",
     "，跳过 %zu 个超出 GLA 范围的序列", "、GLA の範囲外 %zu 件をスキップ"},

    // Updates
    {S::SecUpdates, "Updates", "Updates", "更新", "アップデート"},
    {S::UpdAuto, "Beim Start nach Updates suchen", "Check for updates at startup",
     "启动时检查更新", "起動時にアップデートを確認"},
    {S::UpdAutoTip,
     "Fragt beim Start bei GitHub nach, ob es eine neuere Version gibt. Installiert wird nur "
     "nach einem Klick auf \"Jetzt aktualisieren\".",
     "Asks GitHub at startup whether a newer version exists. Nothing is installed until you "
     "click \"Update now\".",
     "启动时向 GitHub 查询是否有新版本。只有点击“立即更新”后才会安装。",
     "起動時に GitHub で新しいバージョンがあるか確認します。「今すぐ更新」を押すまでは何もインストールされません。"},
    {S::UpdStable, "Stabile Versionen", "Stable releases", "稳定版", "安定版"},
    {S::UpdSnapshot, "Snapshot (neuester Stand)", "Snapshot (latest changes)", "快照版（最新改动）",
     "スナップショット（最新の変更）"},
    {S::UpdChannelTip,
     "Stabil: nur freigegebene Versionen.\nSnapshot: jeder neue Stand von main, sobald alle "
     "Tests bestanden sind.",
     "Stable: released versions only.\nSnapshot: every new state of main once all tests pass.",
     "稳定版：仅正式发布的版本。\n快照版：main 分支的每次更新，全部测试通过后即发布。",
     "安定版: 正式リリースのみ。\nスナップショット: すべてのテストに合格した main の最新状態。"},
    {S::AboutVersion, "Version: %s", "Version: %s", "版本：%s", "バージョン: %s"},
    {S::UpdCheckNow, "Nach Updates suchen", "Check for updates", "检查更新",
     "アップデートを確認"},
    {S::UpdChecking, "Suche nach Updates ...", "Checking for updates ...", "正在检查更新……",
     "アップデートを確認しています..."},
    {S::UpdUpToDate, "g2c ist aktuell (%s).", "g2c is up to date (%s).", "g2c 已是最新版本（%s）。",
     "g2c は最新です (%s)。"},
    {S::UpdAvailable, "Neue Version verfuegbar: %s (installiert: %s)",
     "New version available: %s (installed: %s)", "有新版本：%s（当前：%s）",
     "新しいバージョンがあります: %s (現在: %s)"},
    {S::UpdInstall, "Jetzt aktualisieren", "Update now", "立即更新", "今すぐ更新"},
    {S::UpdNotes, "Was ist neu?", "What's new?", "更新内容", "変更点"},
    {S::UpdLater, "Spaeter", "Later", "稍后", "後で"},
    {S::UpdSkip, "Diese Version ueberspringen", "Skip this version", "跳过此版本",
     "このバージョンをスキップ"},
    {S::UpdDownloading, "Lade %s herunter", "Downloading %s", "正在下载 %s", "%s をダウンロード中"},
    {S::UpdInstalled, "Update auf %s installiert - wirksam nach einem Neustart.",
     "Update to %s installed - takes effect after a restart.", "已安装 %s 更新——重启后生效。",
     "%s へのアップデートをインストールしました。再起動すると有効になります。"},
    {S::UpdRestart, "Jetzt neu starten", "Restart now", "立即重启", "今すぐ再起動"},
    {S::UpdDone, "g2c wurde auf %s aktualisiert.", "g2c was updated to %s.", "g2c 已更新到 %s。",
     "g2c を %s に更新しました。"},
    {S::UpdOpenPage, "Download-Seite oeffnen", "Open download page", "打开下载页面",
     "ダウンロードページを開く"},
    {S::UpdCancelled, "Update abgebrochen.", "Update cancelled.", "更新已取消。",
     "アップデートを中止しました。"},
    {S::UpdErrNetwork, "Keine Verbindung zu GitHub: %s", "Could not reach GitHub: %s",
     "无法连接 GitHub：%s", "GitHub に接続できません: %s"},
    {S::UpdErrNoRelease, "Auf diesem Kanal ist noch keine Version veroeffentlicht.",
     "No version has been published on this channel yet.", "此渠道尚未发布任何版本。",
     "このチャンネルではまだバージョンが公開されていません。"},
    {S::UpdErrBadAnswer, "Die Antwort von GitHub war nicht lesbar.",
     "The answer from GitHub could not be read.", "无法解析 GitHub 的响应。",
     "GitHub からの応答を読み取れませんでした。"},
    {S::UpdErrNoAsset, "Die Version enthaelt %s nicht.", "The release does not contain %s.",
     "该版本不包含 %s。", "このリリースには %s が含まれていません。"},
    {S::UpdErrUntrusted, "Download-Adresse ausserhalb des g2c-Repositorys - abgelehnt.",
     "Download address outside the g2c repository - refused.",
     "下载地址不在 g2c 仓库内——已拒绝。", "ダウンロード先が g2c リポジトリの外です。拒否しました。"},
    {S::UpdErrChecksum, "Download beschaedigt (%s) - verworfen, nichts wurde ersetzt.",
     "Download damaged (%s) - discarded, nothing was replaced.",
     "下载的文件已损坏（%s）——已丢弃，未替换任何文件。",
     "ダウンロードが破損しています (%s)。破棄しました。何も置き換えていません。"},
    {S::UpdErrWrite,
     "Die Exe liess sich nicht ersetzen (%s). Liegt g2c in einem geschuetzten Ordner, die neue "
     "Version bitte von Hand herunterladen.",
     "Could not replace the exe (%s). If g2c sits in a protected folder, please download the "
     "new version by hand.",
     "无法替换程序文件（%s）。如果 g2c 位于受保护的文件夹中，请手动下载新版本。",
     "exe を置き換えられませんでした (%s)。g2c が保護されたフォルダーにある場合は、新しいバージョンを手動でダウンロードしてください。"},
};

static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<std::size_t>(S::Count),
              "Tabelle und Aufzaehlung sind unterschiedlich lang");

// And now the ORDER too, not just the length.
consteval bool tableInOrder() {
    for (std::size_t i = 0; i < static_cast<std::size_t>(S::Count); ++i)
        if (kTable[i].id != static_cast<S>(i)) return false;
    return true;
}
static_assert(tableInOrder(), "Ein Eintrag steht an der falschen Stelle in kTable");

// Placeholders must be the same in all languages, in the same order.
//
// snprintf matches arguments by position alone. If a translation had %s
// before %zu, the number was read as a pointer - that is how the program
// crashed in the Chinese and Japanese UI when adding an XSI folder and when
// opening a GLA. Here, that becomes a compile error.
struct FormatSpec {
    char len1 = 0;
    char len2 = 0;
    char conv = 0;
};

consteval FormatSpec nextFormatSpec(const char*& p) {
    while (*p) {
        if (*p != '%') {
            ++p;
            continue;
        }
        ++p;
        if (*p == '%') {
            ++p;
            continue;
        }
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') ++p;
        while ((*p >= '0' && *p <= '9') || *p == '*') ++p;
        if (*p == '.') {
            ++p;
            while ((*p >= '0' && *p <= '9') || *p == '*') ++p;
        }
        FormatSpec s;
        if (*p == 'h' || *p == 'l' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L') {
            s.len1 = *p++;
            if (*p == 'h' || *p == 'l') s.len2 = *p++;
        }
        s.conv = *p;
        if (*p) ++p;
        return s;
    }
    return {};
}

consteval bool sameFormat(const char* a, const char* b) {
    for (;;) {
        const FormatSpec x = nextFormatSpec(a);
        const FormatSpec y = nextFormatSpec(b);
        if (x.len1 != y.len1 || x.len2 != y.len2 || x.conv != y.conv) return false;
        if (x.conv == 0) return true;
    }
}

consteval bool formatsMatch() {
    for (const Entry& e : kTable)
        if (!sameFormat(e.de, e.en) || !sameFormat(e.de, e.zh) || !sameFormat(e.de, e.ja))
            return false;
    return true;
}
static_assert(formatsMatch(), "Platzhalter (%...) stimmen zwischen den Sprachen nicht ueberein");

Lang g_lang = Lang::De;

}  // namespace

// Language names in the CURRENTLY active language.
//
// This used to be hard-coded as "中文" and "日本語". But as long as German
// is selected, these characters aren't baked into the font atlas at all -
// the menu showed two rows of question marks. Anyone looking for Chinese
// has to be able to read it BEFORE switching it on.
const char* langName(Lang l) {
    struct Names {
        const char* de;
        const char* en;
        const char* zh;
        const char* ja;
    };
    static const Names kNames[] = {
        {"Deutsch", "German", "德语", "ドイツ語"},
        {"Englisch", "English", "英语", "英語"},
        {"Chinesisch", "Chinese", "中文", "中国語"},
        {"Japanisch", "Japanese", "日语", "日本語"},
    };
    const auto i = static_cast<std::size_t>(l);
    if (i >= 4) return "?";
    const Names& e = kNames[i];
    switch (g_lang) {
        case Lang::En: return e.en;
        case Lang::Zh: return e.zh;
        case Lang::Ja: return e.ja;
        default: return e.de;
    }
}

void setLanguage(Lang l) {
    if (l >= Lang::De && l < Lang::Count) g_lang = l;
}

Lang language() { return g_lang; }

const char* tr(S id) {
    const auto i = static_cast<std::size_t>(id);
    if (i >= static_cast<std::size_t>(S::Count)) return "?";
    const Entry& e = kTable[i];
    switch (g_lang) {
        case Lang::En: return e.en;
        case Lang::Zh: return e.zh;
        case Lang::Ja: return e.ja;
        default: return e.de;
    }
}

}  // namespace g2::gui
