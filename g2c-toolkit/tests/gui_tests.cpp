// Oberflaechenlogik ohne Fenster pruefen.
#include "gui/app.h"
#include "gui/i18n.h"
#include "gui/icons.h"
#include "gui/preview.h"
#include "g2/xsi.h"
#include "g2/xsi_anim.h"
#include "g2/mdxa.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <set>
namespace fs=std::filesystem;

// Eine kleine, aber gueltige .xsi anlegen.
//
// Die Oberflaeche weist Dateien ab, die es nicht gibt — erfundene Namen
// reichen deshalb nicht mehr. Das ist gewollt: ein Ordner oder eine
// fehlende Datei soll beim Hinzufuegen auffallen, nicht erst beim Bauen.
static void makeXsi(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << "xsi 0350txt 0032\n\nSI_Model MDL-model_root {\n"
       "  SI_Transform SRT-model_root {\n"
       "    1.0,\n    1.0,\n    1.0,\n    0.0,\n    0.0,\n    0.0,\n"
       "    0.0,\n    0.0,\n    0.0,\n  }\n}\n";
}
int fails=0;
void ck(bool c,const char*m){ if(!c){printf("  FEHLER: %s\n",m);fails++;} }
int main(){
  // Einstellungen des Nutzers nicht anfassen.
  //
  // Jede App stellt beim Start die zuletzt offenen Skripte aus %APPDATA%\g2c
  // wieder her und schreibt die Einstellungen beim Beenden zurueck. Ohne
  // Umlenkung oeffnete dieser Test die echten .car-Dateien des Nutzers,
  // speicherte mit saveDocument(0) womoeglich eine davon und ueberschrieb
  // am Ende seine Tabs und Ausgabeordner mit Testpfaden.
  {
    const fs::path sandbox = fs::temp_directory_path()/"g2c_gui_test_appdata";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
#ifdef _WIN32
    _putenv_s("APPDATA", sandbox.string().c_str());
#else
    setenv("APPDATA", sandbox.string().c_str(), 1);
    setenv("HOME", sandbox.string().c_str(), 1);
#endif
  }
  const fs::path root = fs::temp_directory_path()/"g2c_gui_test";
  fs::remove_all(root); fs::create_directories(root/"sub"/"tief");
  for (auto p : {root/"a.car", root/"sub"/"b.car", root/"sub"/"tief"/"c.car"}) {
    std::ofstream f(p);
    f<<"$aseanimgrabinit\n$aseanimgrab anims/x.xsi -enum BOTH_STAND1\n"
       "$aseanimgrabfinalize\n$aseanimconvertmdx_noask r -makeskel models/x/_humanoid\n";
  }
  g2::gui::Platform plat;  // absichtlich leer: die Logik darf das aushalten
  g2::gui::App app(std::move(plat));

  ck(app.documents().empty(),"startet ohne Dokumente");
  const std::size_t n = app.openFolder(root.string());
  ck(n==3,"drei .car aus Unterordnern geoeffnet");
  ck(app.documents().size()==3,"drei Tabs");

  app.openCar((root/"a.car").string());
  ck(app.documents().size()==3,"dieselbe Datei wird nicht doppelt geoeffnet");

  const fs::path xd0 = fs::temp_directory_path()/"g2c_xsi_real";
  fs::remove_all(xd0);
  makeXsi(xd0/"n1.xsi"); makeXsi(xd0/"n2.xsi"); makeXsi(xd0/"n3.xsi");
  const std::size_t touched = app.addXsiFiles({(xd0/"n1.xsi").string(),(xd0/"n2.xsi").string()}, true);
  ck(touched==3,"XSI an alle Tabs angehaengt");
  for (auto&d:app.documents()) ck(d.script.grabs.size()==3,"jeder Tab hat jetzt drei Grabs");
  for (auto&d:app.documents()) ck(d.dirty,"als geaendert markiert");

  app.addXsiFiles({(xd0/"n3.xsi").string()}, false);
  ck(app.documents()[app.activeTab()].script.grabs.size()==4,"nur der aktive Tab bekam die vierte");

  app.validateAll();
  ck(app.documents()[0].validated,"Pruefung gelaufen");
  ck(app.documents()[0].validation.errors>0,"fehlende Dateien werden bemerkt");

  app.closeDocument(0);
  ck(app.documents().size()==2,"Tab geschlossen");
  ck(app.activeTab()>=0 && app.activeTab()<2,"aktiver Tab bleibt gueltig");

  app.closeDocument(0); app.closeDocument(0);
  ck(app.documents().empty(),"alle geschlossen");
  ck(app.activeTab()==0,"Index zurueckgesetzt");

  // Tabtitel muessen unterscheidbar sein. In einem Modellbaum heissen alle
  // Skripte gleich; dann entscheidet der Ordner.
  {
    const fs::path r2 = fs::temp_directory_path()/"g2c_gui_titles";
    fs::remove_all(r2);
    for (auto d : {"anim_a","anim_b","anim_c"}) {
      fs::create_directories(r2/d);
      std::ofstream f(r2/d/"_humanoid.car");
      f<<"$aseanimgrabinit\n$aseanimgrab x.xsi -enum BOTH_STAND1\n$aseanimgrabfinalize\n";
    }
    g2::gui::Platform p2;
    g2::gui::App a2(std::move(p2));
    a2.openFolder(r2.string());
    ck(a2.documents().size()==3,"drei gleichnamige Skripte geoeffnet");
    std::set<std::string> titles;
    for (auto&d:a2.documents()) titles.insert(d.title);
    ck(titles.size()==3,"Tabtitel sind unterscheidbar");
    ck(titles.count("anim_a")==1,"Ordnername wird als Titel benutzt");
    ck(titles.count("_humanoid.car")==0,"nicht dreimal derselbe Dateiname");

    // Bleibt nur eines uebrig, darf wieder der Dateiname stehen.
    a2.closeDocument(0); a2.closeDocument(0);
    ck(a2.documents().size()==1,"eines uebrig");
    ck(a2.documents()[0].title=="_humanoid.car","eindeutiger Dateiname wird wieder benutzt");
    fs::remove_all(r2);
  }

  // Ordner voller .xsi anhaengen.
  {
    const fs::path xd = fs::temp_directory_path()/"g2c_gui_xsi";
    fs::remove_all(xd); fs::create_directories(xd/"unter");
    for (auto n : {"b.xsi","a.xsi"}) { std::ofstream f(xd/n); f<<"xsi 0350txt 0032\n"; }
    { std::ofstream f(xd/"unter"/"c.xsi"); f<<"xsi 0350txt 0032\n"; }
    { std::ofstream f(xd/"egal.txt"); f<<"x\n"; }

    const fs::path r3 = fs::temp_directory_path()/"g2c_gui_folder";
    fs::remove_all(r3); fs::create_directories(r3);
    { std::ofstream f(r3/"m.car"); f<<"$aseanimgrabinit\n$aseanimgrabfinalize\n"; }

    g2::gui::Platform p3;
    g2::gui::App a3(std::move(p3));
    a3.openCar((r3/"m.car").string());
    const std::size_t n3 = a3.addXsiFolder(xd.string(), false);
    ck(n3==1,"Ordner an ein Skript angehaengt");
    ck(a3.documents()[0].script.grabs.size()==3,"drei .xsi gefunden, .txt ignoriert");
    // Sortiert, damit die Reihenfolge nicht vom Dateisystem abhaengt.
    const auto& gs = a3.documents()[0].script.grabs;
    bool sorted = true;
    for (size_t i=1;i<gs.size();++i) if (gs[i-1].file > gs[i].file) sorted = false;
    ck(sorted,"Reihenfolge ist sortiert");

    fs::remove_all(xd); fs::remove_all(r3);
  }

  // Ausgabeort je Skript getrennt.
  {
    const fs::path r4 = fs::temp_directory_path()/"g2c_gui_out";
    fs::remove_all(r4); fs::create_directories(r4/"x"); fs::create_directories(r4/"y");
    for (auto d : {"x","y"}) { std::ofstream f(r4/d/"_humanoid.car");
      f<<"$aseanimgrabinit\n$aseanimgrabfinalize\n"; }
    g2::gui::Platform p4;
    g2::gui::App a4(std::move(p4));
    a4.openFolder(r4.string());
    ck(a4.documents().size()==2,"zwei Skripte");
    a4.documents()[0].outputDir = "C:/ziel_a";
    a4.documents()[1].outputDir = "C:/ziel_b";
    ck(a4.documents()[0].outputDir != a4.documents()[1].outputDir,
       "jedes Skript hat seinen eigenen Ausgabeort");
    fs::remove_all(r4);
  }

  // Speichern: Sicherung anlegen, Inhalt erhalten, dirty zuruecksetzen.
  {
    const fs::path r5 = fs::temp_directory_path()/"g2c_gui_save";
    fs::remove_all(r5); fs::create_directories(r5);
    {
      std::ofstream f(r5/"s.car");
      f<<"$aseanimgrabinit\n"
         "$aseanimgrab a.xsi -enum BOTH_STAND1 -loop -1 -framespeed 20\n"
         "$aseanimgrab b.xsi -enum BOTH_WALK1 -additional 0 5 -1 20 BOTH_RUN1\n"
         "$aseanimgrabfinalize\n"
         "$aseanimconvertmdx_noask r -makeskel models/x/_humanoid\n";
    }
    g2::gui::Platform p5;
    g2::gui::App a5(std::move(p5));
    a5.openCar((r5/"s.car").string());
    ck(a5.documents().size()==1,"Skript geoeffnet");
    const size_t before = a5.documents()[0].script.grabs.size();

    makeXsi(r5/"c.xsi");
    a5.addXsiFiles({(r5/"c.xsi").string()}, false);
    ck(a5.documents()[0].dirty,"nach Aenderung als geaendert markiert");

    ck(a5.saveDocument(0),"gespeichert");
    ck(!a5.documents()[0].dirty,"danach nicht mehr geaendert");
    ck(fs::exists(r5/"s.car.bak"),"Sicherung wurde angelegt");

    // Wieder einlesen: der Inhalt muss vollstaendig sein.
    g2::gui::Platform p6;
    g2::gui::App a6(std::move(p6));
    a6.openCar((r5/"s.car").string());
    ck(a6.documents()[0].script.grabs.size()==before+1,"neue Sequenz ist in der Datei");
    ck(a6.documents()[0].script.convert.has_value(),"Konvertierungsanweisung erhalten");
    bool haveAdd=false;
    for (auto&g : a6.documents()[0].script.grabs) if (!g.additional.empty()) haveAdd=true;
    ck(haveAdd,"-additional erhalten");

    fs::remove_all(r5);
  }

  // Ausgabeort: muss je Skript gesetzt sein, es gibt keinen gemeinsamen.
  {
    const fs::path r7 = fs::temp_directory_path()/"g2c_gui_out2";
    fs::remove_all(r7);
    for (auto d : {"a","b","c"}) {
      fs::create_directories(r7/d);
      std::ofstream f(r7/d/"_humanoid.car"); f<<"$aseanimgrabinit\n$aseanimgrabfinalize\n";
    }
    g2::gui::Platform p7;
    g2::gui::App a7(std::move(p7));
    a7.openFolder(r7.string());
    ck(a7.documents().size()==3,"drei Skripte");
    for (auto&d : a7.documents()) ck(d.outputDir.empty(),"anfangs kein Ziel gesetzt");

    const std::size_t n7 = a7.assignDefaultOutputs(true);
    ck(n7==3,"Sammelzuweisung setzt drei Ziele");

    // Entscheidend: die Ziele muessen VERSCHIEDEN sein. Ein gemeinsamer
    // Ordner liesse die drei gleichnamigen GLA einander ueberschreiben.
    std::set<std::string> outs;
    for (auto&d : a7.documents()) outs.insert(d.outputDir);
    ck(outs.size()==3,"drei verschiedene Ausgabeorte");

    // Bereits gesetzte werden nicht ueberschrieben.
    a7.documents()[0].outputDir = "C:/eigen";
    const std::size_t n8 = a7.assignDefaultOutputs(true);
    ck(n8==0,"nichts zu setzen, wenn alle ein Ziel haben");
    ck(a7.documents()[0].outputDir=="C:/eigen","eigenes Ziel bleibt unangetastet");

    fs::remove_all(r7);
  }

  // Uebersetzungen: jeder Eintrag muss in allen vier Sprachen belegt sein.
  {
    using namespace g2::gui;
    const Lang before = language();
    int leer=0, gleich=0;
    for (int i=0;i<(int)S::Count;i++) {
      const char* txt[4];
      for (int l=0;l<(int)Lang::Count;l++) {
        setLanguage((Lang)l);
        txt[l] = tr((S)i);
        if (!txt[l] || !*txt[l]) leer++;
      }
      // Chinesisch darf nicht einfach das deutsche Wort sein - das waere
      // eine vergessene Uebersetzung.
      //
      // Ausgenommen sind Texte OHNE uebersetzbaren Inhalt: reine
      // Formatangaben wie "%zu/%zu  %s" und Eigennamen wie "g2c_out". Die
      // Regel prueft das am Text selbst, statt eine Ausnahmeliste zu pflegen
      // - eine Liste veraltet, sobald jemand einen Eintrag hinzufuegt.
      const std::string de = txt[0], zh = txt[2];
      std::string rest;
      for (size_t k=0;k<de.size();++k) {
        if (de[k]=='%') { while (k+1<de.size() && !isupper((unsigned char)de[k+1])
                                 && !islower((unsigned char)de[k+1])) k++;
                          k++; continue; }
        rest += de[k];
      }
      // Uebersetzbar ist nur, was ueberhaupt Woerter enthaelt.
      //
      // Nicht uebersetzbar und deshalb ausgenommen:
      //   - reine Formatangaben ("%zu/%zu  %s")
      //   - Abkuerzungen und Formatnamen ohne Kleinbuchstaben ("XSI -> GLA")
      //   - Dateinamen ("animation.cfg")
      // Die Regel prueft das am Text selbst, statt eine Ausnahmeliste zu
      // pflegen - eine Liste veraltet, sobald jemand etwas hinzufuegt.
      size_t letters=0, lower=0;
      for (char c : rest) {
        if (isalpha((unsigned char)c)) letters++;
        if (islower((unsigned char)c)) lower++;
      }
      const bool istDateiname = rest.find('.')!=std::string::npos
                                && rest.find(' ')==std::string::npos;
      const bool hasWord = letters>3 && lower>0 && !istDateiname;
      if (de==zh && hasWord && de.find("g2c")==std::string::npos)
        gleich++;
    }
    setLanguage(before);
    ck(leer==0,"kein leerer Uebersetzungseintrag");

    // Deutsche und englische Texte duerfen nur Latin-1 enthalten.
    //
    // Der Zeichensatz wird fuer diese Sprachen mit GetGlyphRangesDefault
    // gebacken, also 0x20 bis 0xFF. Ein Geviertstrich oder ein typografisches
    // Anfuehrungszeichen liegt darueber und erscheint als Fragezeichen -
    // genau so stand im Menue "Assetwurzel ? enthaelt models".
    int ausserhalb=0;
    for (int l=0;l<2;l++) {
      setLanguage((Lang)l);
      for (int i=0;i<(int)S::Count;i++)
        for (const unsigned char* q=(const unsigned char*)tr((S)i); *q; ++q)
          if (*q >= 0x80) { ausserhalb++; break; }
    }
    ck(ausserhalb==0,"de/en enthalten nur darstellbare Zeichen");

    // Sprachnamen muessen in der aktiven Sprache lesbar sein.
    setLanguage(Lang::De);
    ck(std::string(langName(Lang::Zh))=="Chinesisch","Sprachname auf Deutsch lesbar");
    setLanguage(Lang::En);
    ck(std::string(langName(Lang::Ja))=="Japanese","Sprachname auf Englisch lesbar");
    ck(gleich==0,"kein chinesischer Text ist nur das deutsche Wort");

    // Umlaute und CJK muessen als UTF-8 ankommen, nicht als Fragezeichen.
    setLanguage(Lang::Zh);
    const std::string zhFile = tr(S::MenuFile);
    ck(zhFile.size()>3,"chinesischer Text ist mehrbyte-kodiert");
    setLanguage(Lang::Ja);
    const std::string jaFile = tr(S::MenuFile);
    ck(jaFile.size()>3,"japanischer Text ist mehrbyte-kodiert");
    ck(zhFile!=jaFile,"Chinesisch und Japanisch sind verschieden");
    setLanguage(before);
  }

  // Einstellungsordner: nicht das Arbeitsverzeichnis, ausser bei
  // mitnehmbarem Betrieb.
  {
    const std::string cfg = g2::gui::App::configDir();
    ck(!cfg.empty(),"Einstellungsordner ermittelt");
    ck(fs::exists(cfg),"Einstellungsordner existiert");

    const bool portable = fs::exists(fs::current_path()/"g2c_portable.txt");
    if (!portable)
      ck(fs::path(cfg) != fs::current_path(),
         "ohne Markierung nicht im Arbeitsverzeichnis");

    // Mit Markierungsdatei muss er im Arbeitsverzeichnis landen.
    { std::ofstream f(fs::current_path()/"g2c_portable.txt"); f<<"\n"; }
    ck(fs::path(g2::gui::App::configDir()) == fs::current_path(),
       "mit Markierung mitnehmbar neben der Exe");
    fs::remove(fs::current_path()/"g2c_portable.txt");
    ck(fs::path(g2::gui::App::configDir()) != fs::current_path() || portable,
       "ohne Markierung wieder der uebliche Ort");
  }

  // Symbole: nur anzeigen, wenn die Schrift geladen werden konnte.
  {
    using namespace g2::gui;
    setIconsAvailable(false);
    ck(std::string(withIcon(ICON_SAVE,"Speichern"))=="Speichern",
       "ohne Schrift nur Text, keine leeren Kaesten");

    setIconsAvailable(true);
    const std::string mit = withIcon(ICON_SAVE,"Speichern");
    ck(mit.size() > std::strlen("Speichern"), "mit Schrift kommt ein Symbol dazu");
    ck(mit.find("Speichern")!=std::string::npos, "der Text bleibt erhalten");

    // Alle Symbole muessen im privaten Bereich liegen, den die Schrift
    // beisteuert. Ein Zeichen ausserhalb waere ein Tippfehler im Code und
    // erschiene als leerer Kasten.
    const char* alle[] = {ICON_FOLDER, ICON_FOLDER_OPEN, ICON_OPEN_FILE, ICON_SAVE,
                          ICON_SAVE_ALL, ICON_BUILD, ICON_CHECK, ICON_VALIDATE, ICON_ADD,
                          ICON_CANCEL, ICON_SETTINGS, ICON_GLOBE, ICON_DELETE, ICON_EDIT};
    int ausserhalb=0;
    for (const char* ic : alle) {
      const unsigned char* q = (const unsigned char*)ic;
      // Drei-Byte-UTF-8 zurueckrechnen.
      if ((q[0]&0xF0)!=0xE0) { ausserhalb++; continue; }
      const unsigned cp = ((q[0]&0x0F)<<12) | ((q[1]&0x3F)<<6) | (q[2]&0x3F);
      if (cp < kIconRangeMin || cp > kIconRangeMax) ausserhalb++;
    }
    ck(ausserhalb==0,"alle Symbole liegen im Bereich der Icon-Schrift");
    setIconsAvailable(false);
  }

  // Umordnen, Loeschen, Neuanlage.
  {
    const fs::path r9 = fs::temp_directory_path()/"g2c_gui_edit";
    fs::remove_all(r9); fs::create_directories(r9);
    {
      std::ofstream f(r9/"e.car");
      f<<"$aseanimgrabinit\n";
      for (auto n : {"a","b","c","d"})
        f<<"$aseanimgrab "<<n<<".xsi -enum BOTH_"<<n<<"\n";
      f<<"$aseanimgrabfinalize\n$aseanimconvertmdx_noask r -makeskel models/x/y\n";
    }
    g2::gui::Platform p9;
    g2::gui::App a9(std::move(p9));
    a9.openCar((r9/"e.car").string());
    auto names = [&]{
      std::string o;
      for (auto& g : a9.documents()[0].script.grabs) o += g.file.substr(0,1);
      return o;
    };
    ck(names()=="abcd","Ausgangsreihenfolge");

    ck(a9.moveGrab(0,0,2),"verschoben");
    ck(names()=="bcad","a steht jetzt an dritter Stelle");
    ck(a9.moveGrab(0,3,0),"ans andere Ende verschoben");
    ck(names()=="dbca","d steht vorn");
    ck(!a9.moveGrab(0,0,0),"gleiche Position aendert nichts");
    ck(!a9.moveGrab(0,9,0),"unmoegliche Position wird abgewiesen");

    // Die Auswahl muss mitwandern, sonst trifft ein spaeteres Loeschen die
    // falsche Sequenz.
    auto&d9 = a9.documents()[0];
    std::fill(d9.selected.begin(), d9.selected.end(), 0);
    d9.selected[0]=1;                       // "d"
    a9.moveGrab(0,0,3);                     // d ans Ende
    ck(names()=="bcad","d ist ans Ende gewandert");
    ck(d9.selected[3]==1 && d9.selected[0]==0,"die Auswahl ist mitgewandert");

    // Mehrere auf einmal loeschen, Indizes duerfen nicht verrutschen.
    ck(a9.deleteGrabs(0,{0,2})==2,"zwei geloescht");
    ck(names()=="cd","die richtigen beiden sind weg: b und a");
    ck(a9.documents()[0].selected.size()==2,"Auswahlfeld bleibt gleich lang");
    ck(a9.documents()[0].dirty,"als geaendert markiert");

    // Neues Skript: muss gueltig sein und sich wieder einlesen lassen.
    const std::string neu = (r9/"neu.car").string();
    ck(a9.newCar(neu),"neues Skript angelegt");
    ck(fs::exists(neu),"Datei liegt auf der Platte");
    ck(a9.documents().size()==2,"als Tab geoeffnet");
    ck(a9.documents()[1].script.convert.has_value(),
       "enthaelt die Konvertierungsanweisung");

    // Ein neues Skript muss Grabs AUFNEHMEN koennen. Ohne den Rahmen
    // schreibt writeScript sie nicht, und die Datei waere still leer.
    a9.setActiveForTest(1);
    makeXsi(r9/"neu.xsi");
    a9.addXsiFiles({(r9/"neu.xsi").string()}, false);
    ck(a9.saveDocument(1),"neues Skript gespeichert");
    g2::gui::Platform pZ;
    g2::gui::App aZ(std::move(pZ));
    aZ.openCar(neu);
    ck(aZ.documents()[0].script.grabs.size()==1,
       "der Grab steht wirklich in der Datei");
    ck(!a9.newCar(neu),"vorhandene Datei wird nicht ueberschrieben");

    fs::remove_all(r9);
  }

  // Blockverschieben und die ROOT-Regel.
  {
    const fs::path rA = fs::temp_directory_path()/"g2c_gui_root";
    fs::remove_all(rA); fs::create_directories(rA);
    {
      std::ofstream f(rA/"r.car");
      f<<"$aseanimgrabinit\n";
      for (auto n : {"a","b","c","d","e"})
        f<<"$aseanimgrab "<<n<<".xsi -enum BOTH_"<<n<<"\n";
      f<<"$aseanimgrab root.xsi -enum ROOT\n";
      f<<"$aseanimgrabfinalize\n";
    }
    g2::gui::Platform pA;
    g2::gui::App aA(std::move(pA));
    aA.openCar((rA/"r.car").string());
    auto seq = [&]{
      std::string o;
      for (auto& g : aA.documents()[0].script.grabs) o += g.file.substr(0,1);
      return o;
    };
    ck(seq()=="abcder","Ausgangslage mit root am Ende");

    // Zwei nicht benachbarte Zeilen als Block ans zweite Feld.
    ck(aA.moveGrabs(0,{0,4},1)==2,"zwei Zeilen als Block verschoben");
    // a (Zeile 0) und e (Zeile 4) vor Zeile 1: beide wandern nach vorn und
    // stehen danach beieinander.
    ck(seq()=="aebcdr","Block sitzt zusammen und in Ausgangsreihenfolge");

    // Entscheidend: root bleibt hinten, egal was verschoben wird.
    ck(aA.documents()[0].script.grabs.back().file=="root.xsi","root ist weiterhin letzter");
    ck(aA.moveGrabs(0,{5},0)>0,"root nach vorn geschoben");
    ck(aA.documents()[0].script.grabs.back().file=="root.xsi",
       "root wandert von selbst wieder ans Ende");

    // Neu hinzugefuegte Animationen landen VOR root.
    makeXsi(rA/"neu.xsi");
    aA.addXsiFiles({(rA/"neu.xsi").string()}, false);
    ck(aA.documents()[0].script.grabs.back().file=="root.xsi",
       "auch nach dem Hinzufuegen steht root zuletzt");
    const auto& gs = aA.documents()[0].script.grabs;
    // Der Pfad wird ohne gesetzte Assetwurzel absolut gespeichert.
    ck(fs::path(gs[gs.size()-2].file).filename()=="neu.xsi",
       "die neue Animation steht direkt davor");

    fs::remove_all(rA);
  }

  // openPath erkennt am Inhalt, was zu tun ist. Das ist derselbe Weg fuer
  // Ziehen auf das Fenster, auf die Exe und Argumente beim Start.
  {
    const fs::path rB = fs::temp_directory_path()/"g2c_gui_open";
    fs::remove_all(rB); fs::create_directories(rB/"unter");
    { std::ofstream f(rB/"x.car"); f<<"$aseanimgrabinit\n$aseanimgrabfinalize\n"; }
    { std::ofstream f(rB/"unter"/"y.car"); f<<"$aseanimgrabinit\n$aseanimgrabfinalize\n"; }
    { std::ofstream f(rB/"a.xsi"); f<<"xsi 0350txt 0032\n"; }
    { std::ofstream f(rB/"s.gla"); f<<"dummy\n"; }

    g2::gui::Platform pB;
    g2::gui::App aB(std::move(pB));
    ck(aB.openPath((rB/"x.car").string()),".car wird als Skript geoeffnet");
    ck(aB.documents().size()==1,"ein Tab");
    ck(aB.openPath((rB/"s.gla").string()),".gla setzt die Referenz");
    ck(aB.settings().referenceGla==(rB/"s.gla").string(),"Referenz wurde gesetzt");
    ck(aB.openPath((rB/"a.xsi").string()),".xsi wird angehaengt");
    ck(aB.documents()[0].script.grabs.size()==1,"Sequenz kam dazu");
    ck(aB.openPath(rB.string()),"Ordner wird durchsucht");
    ck(!aB.openPath((rB/"gibtsnicht.car").string()),"fehlende Datei sauber abgewiesen");
    fs::remove_all(rB);
  }

  // Zweiter Modus: GLA -> XSI.
  {
    g2::gui::Platform pC;
    g2::gui::App aC(std::move(pC));
    ck(aC.mode()==g2::gui::App::Mode::Build,"startet im Baumodus");
    aC.setMode(g2::gui::App::Mode::Extract);
    ck(aC.mode()==g2::gui::App::Mode::Extract,"umgeschaltet");

    const std::string gla = "/mnt/user-data/uploads/_humanoid_-_Kopie.gla";
    if (fs::exists(gla)) {
      ck(aC.loadGlaForExtract(gla),"GLA geladen");
      ck(aC.extract().loaded,"als geladen markiert");
      ck(aC.extract().gla.numFrames>1000,"Frames gelesen");
      // Die hochgeladene GLA liegt neben animation.cfg, also greift die
      // automatische Suche schon hier - genau so soll es sein.
      ck(aC.extract().seqs.size()>1,"Begleitdatei sofort mitgeladen");
      ck(aC.extract().origin.has_value(),"Versatz aus der GLA geschaetzt");

      // Begleitdateien werden neben der GLA selbst gesucht.
      {
        const fs::path nd = fs::temp_directory_path()/"g2c_companion";
        fs::remove_all(nd); fs::create_directories(nd);
        fs::copy_file(gla, nd/"_humanoid.gla");
        if (fs::exists("/mnt/user-data/uploads/animation.cfg"))
          fs::copy_file("/mnt/user-data/uploads/animation.cfg", nd/"animation.cfg");
        if (fs::exists("/mnt/user-data/uploads/_humanoid.frames"))
          fs::copy_file("/mnt/user-data/uploads/_humanoid.frames", nd/"_humanoid.frames");

        g2::gui::Platform pD;
        g2::gui::App aD(std::move(pD));
        ck(aD.loadGlaForExtract((nd/"_humanoid.gla").string()),"GLA aus eigenem Ordner");
        ck(!aD.extract().cfgPath.empty(),"animation.cfg von selbst gefunden");
        ck(aD.extract().seqs.size()>5,"Sequenzen daraus gelesen");
        ck(!aD.extract().framesPath.empty(),".frames von selbst gefunden");

        // Ohne Begleitdateien darf nichts geraten werden.
        const fs::path bare = fs::temp_directory_path()/"g2c_bare";
        fs::remove_all(bare); fs::create_directories(bare);
        fs::copy_file(gla, bare/"_humanoid.gla");
        g2::gui::Platform pE;
        g2::gui::App aE(std::move(pE));
        aE.loadGlaForExtract((bare/"_humanoid.gla").string());
        ck(aE.extract().cfgPath.empty(),"ohne cfg wird nichts erfunden");
        ck(aE.extract().seqs.size()==1,"dann nur ein Block");

        fs::remove_all(nd); fs::remove_all(bare);
      }
      if (aC.extract().origin)
        ck(std::fabs((*aC.extract().origin)[2] - 24.0f) < 0.5f,
           "geschaetzter Versatz ist die uebliche 24 in Z");

      const std::string cfg = "/mnt/user-data/uploads/animation.cfg";
      if (fs::exists(cfg)) {
        ck(aC.loadAnimationCfg(cfg),"animation.cfg auch von Hand ladbar");
        // Keine feste Zahl erwarten: welche animation.cfg danebenliegt,
        // haengt vom Nutzer ab. JKA hat 1683 Sequenzen, JK2 nur 931.
        // Keine feste Zahl: welche animation.cfg danebenliegt, bestimmt der
        // Nutzer. JKA hat 1683, JK2 989, ein Cutscene-Humanoid 26.
        ck(aC.extract().seqs.size()>5,"Sequenzen gelesen");
        ck(aC.extract().selected.size()==aC.extract().seqs.size(),
           "Auswahlfeld passt zur Sequenzzahl");

        // Alle Sequenzen muessen INNERHALB der GLA liegen. Eine Zeile aus
        // der cfg, die darueber hinausragt, waere nicht exportierbar - sie
        // in der Liste zu zeigen hiesse, sie fuer brauchbar zu halten.
        bool inRange = true;
        for (const auto& q : aC.extract().seqs)
          if (q.start < 0 || q.start + q.count > aC.extract().gla.numFrames) inRange = false;
        ck(inRange,"alle gezeigten Sequenzen liegen in der GLA");

        // Wirklich exportieren und zurueckrechnen.
        const fs::path od = fs::temp_directory_path()/"g2c_extract";
        fs::remove_all(od);
        // Nur Vollsequenzen nehmen. Ein Ausschnitt einer laengeren
        // Animation traegt einen Rest Wurzelbewegung, der beim Neubauen in
        // die .frames wandert — das ist richtiges Verhalten und wuerde hier
        // faelschlich als Fehler zaehlen.
        // Jede Sequenz durchgehen, nicht jede 97ste.
        //
        // Eine feste Schrittweite passt zu JKAs 1683 Sequenzen und
        // ueberspringt bei einem Cutscene-Humanoid mit 26 alles ausser der
        // ersten — der Test fand dann nichts und meldete einen Fehler, den
        // es nicht gab.
        std::vector<std::size_t> rows;
        for (size_t i=0;i<aC.extract().seqs.size() && rows.size()<3;++i) {
          const auto& q = aC.extract().seqs[i];
          if (q.count < 3) continue;
          if (g2::xsiexp::residualMotion(aC.extract().gla,
                                         {q.name, q.start, q.count}) > 0.05f) continue;
          rows.push_back(i);
        }
        ck(!rows.empty(),"brauchbare Sequenzen gefunden");
        ck(aC.exportSequences(rows, od.string())==rows.size(),"Sequenzen geschrieben");

        // Ueber den Ordner nur zaehlen, wenn er auch entstanden ist.
        //
        // Ohne die Pruefung wirft der Iterator, und ein geworfener Test
        // reisst die ganze Suite mit — statt eine Zeile zu melden.
        std::error_code lec;
        size_t files=0;
        if (fs::is_directory(od, lec))
          for (const auto& e : fs::directory_iterator(od, lec)) { (void)e; files++; }
        ck(files==rows.size(),"je eine Datei je Sequenz");

        // Der eigentliche Beweis: zurueckgelesen muss dasselbe herauskommen.
        double worst = 0;
        for (size_t k=0;k<rows.size();++k) {
          const auto& q = aC.extract().seqs[rows[k]];
          std::string low = q.name;
          std::transform(low.begin(), low.end(), low.begin(),
                         [](unsigned char ch){ return (char)std::tolower(ch); });
          const auto doc = g2::xsi::parseFile((od/(low+".xsi")).string());
          const auto an = g2::xsi::loadAnimation(doc, "x");
          // MIT demselben Versatz auswerten, mit dem exportiert wurde.
          // Der Export setzt ihn wieder ein, damit die Datei aussieht wie
          // eine vom Kuenstler; beim Bauen zieht ihn die .car wieder ab.
          g2::xsi::EvalOptions eo; eo.scale = aC.extract().gla.skeleton.scale;
          eo.origin = aC.extract().origin;
          const auto rr = g2::xsi::evaluate(aC.extract().gla.skeleton, an, eo);
          for (int fr=0; fr<q.count; ++fr)
            for (int b=0;b<(int)aC.extract().gla.skeleton.bones.size();++b) {
              const auto A = aC.extract().gla.boneMatrix(q.start+fr, b);
              const auto B = rr.frames.at(fr, b);
              for (int r=0;r<3;r++) for (int c=0;c<4;c++)
                worst = std::max(worst, (double)std::fabs(A.m[r][c]-B.m[r][c]));
            }
        }
        // Eine Quantisierungsstufe sind 0.015625. Dazu kommt, was das
        // SRT-Modell einer .xsi nicht darstellen kann: Scherung aus
        // Bindposen mit ungleichen Achsenlaengen. Bei flachen Skeletten mit
        // langem Hebel bleibt davon etwas uebrig — 0.1 ist die Grenze, ab
        // der ich es als Fehler ansehen wuerde.
        ck(worst < 0.1, "Rundlauf GLA -> XSI -> GLA im erwarteten Rahmen");
        fs::remove_all(od);
      }
    }
  }

  // Zwei Humanoids vergleichen: welche Sequenzen fehlen drueben?
  {
    const fs::path cr = fs::temp_directory_path()/"g2c_compare";
    fs::remove_all(cr); fs::create_directories(cr);

    // Eine kleine GLA mit drei Sequenzen bauen.
    g2::Skeleton cs; cs.name="c"; cs.scale=0.64f;
    { g2::Bone b; b.name="model_root"; b.basePose=g2::Mat3x4::identity(); cs.bones.push_back(b); }
    g2::AnimationFrames cf; cf.numBones=1; cf.matrices.assign(30, g2::Mat3x4::identity());
    { std::ofstream o(cr/"_humanoid.gla", std::ios::binary);
      const auto w=g2::writeMdxa(cs,cf);
      o.write((const char*)w.data.data(), (std::streamsize)w.data.size()); }
    { std::ofstream o(cr/"animation.cfg");
      o<<"BOTH_STAND1 0 10 -1 20\nBOTH_WALK1 10 10 0 20\nBOTH_SPEZIAL 20 10 -1 20\n"; }
    // Die Gegenseite kennt nur zwei davon, eine mit anderer Schreibweise.
    { std::ofstream o(cr/"andere.cfg");
      o<<"both_stand1 0 5 -1 20\nBOTH_WALK1 5 5 0 20\nBOTH_NURDORT 10 5 -1 20\n"; }

    g2::gui::Platform pF;
    g2::gui::App aF(std::move(pF));
    ck(aF.loadGlaForExtract((cr/"_humanoid.gla").string()),"GLA geladen");
    ck(aF.extract().seqs.size()==3,"drei Sequenzen");

    ck(aF.loadCompareCfg((cr/"andere.cfg").string()),"Vergleich geladen");
    ck(aF.extract().compareNames.size()==3,"drei Namen drueben");

    // Gross- und Kleinschreibung darf keine Rolle spielen: "both_stand1"
    // drueben ist dieselbe Sequenz wie "BOTH_STAND1" hier.
    ck(aF.existsInCompare("BOTH_STAND1"),"Schreibweise wird ignoriert");
    ck(aF.existsInCompare("BOTH_WALK1"),"exakter Treffer");
    ck(!aF.existsInCompare("BOTH_SPEZIAL"),"fehlt drueben");

    const std::size_t n = aF.selectMissing();
    ck(n==1,"genau eine fehlt");
    std::size_t sel=0; for (char c : aF.extract().selected) if (c) sel++;
    ck(sel==1,"genau eine ausgewaehlt");
    // Und zwar die richtige.
    for (size_t i=0;i<aF.extract().seqs.size();++i)
      if (aF.extract().selected[i])
        ck(aF.extract().seqs[i].name=="BOTH_SPEZIAL","die richtige ausgewaehlt");

    // Eine unlesbare Datei darf den Vergleich nicht loeschen.
    ck(!aF.loadCompareCfg((cr/"gibtsnicht.cfg").string()),"fehlende Datei abgewiesen");
    ck(aF.extract().compareNames.size()==3,"vorheriger Vergleich bleibt erhalten");

    fs::remove_all(cr);
  }

  // Kopieren und Einfuegen, auch zwischen zwei Skripten.
  {
    const fs::path rP = fs::temp_directory_path()/"g2c_clip";
    fs::remove_all(rP); fs::create_directories(rP/"a"); fs::create_directories(rP/"b");
    { std::ofstream f(rP/"a"/"_humanoid.car");
      f<<"$aseanimgrabinit\n";
      for (auto n : {"x","y","z"}) f<<"$aseanimgrab "<<n<<".xsi -enum BOTH_"<<n
                                    <<" -loop 3 -framespeed 40\n";
      f<<"$aseanimgrabfinalize\n"; }
    { std::ofstream f(rP/"b"/"_humanoid.car");
      f<<"$aseanimgrabinit\n$aseanimgrab q.xsi -enum BOTH_q\n$aseanimgrabfinalize\n"; }

    g2::gui::Platform pP;
    g2::gui::App aP(std::move(pP));
    aP.openFolder(rP.string());
    ck(aP.documents().size()==2,"zwei Skripte");

    auto names=[&](size_t d){ std::string o;
      for (auto&g : aP.documents()[d].script.grabs) o += g.file.substr(0,1); return o; };
    const size_t A = names(0)=="xyz" ? 0 : 1, B = 1-A;
    ck(names(A)=="xyz","Ausgangslage a");
    ck(names(B)=="q","Ausgangslage b");

    ck(aP.copyGrabs(A,{0,2})==2,"zwei kopiert");
    ck(aP.clipboardSize()==2,"zwei in der Ablage");

    // In das ANDERE Skript einfuegen — das ist der eigentliche Zweck.
    ck(aP.pasteGrabs(B,0)==2,"in das andere Skript eingefuegt");
    ck(names(B)=="xzq","davor eingefuegt, Reihenfolge erhalten");
    ck(aP.documents()[B].dirty,"als geaendert markiert");

    // Die Einstellungen des Grabs muessen mitkommen, nicht nur der Name.
    const auto& g0 = aP.documents()[B].script.grabs[0];
    ck(g0.loop && *g0.loop==3,"Loopframe mitkopiert");
    ck(g0.frameSpeed && *g0.frameSpeed==40,"Framespeed mitkopiert");

    // Die Quelle bleibt unangetastet.
    ck(names(A)=="xyz","Kopieren aendert die Quelle nicht");

    // Ausschneiden entfernt sie dort.
    ck(aP.cutGrabs(A,{1})==1,"eine ausgeschnitten");
    ck(names(A)=="xz","aus der Quelle entfernt");
    ck(aP.pasteGrabs(B,3)==1,"ans Ende eingefuegt");
    ck(names(B)=="xzqy","am Ende gelandet");

    // Die Ablage bleibt nach dem Einfuegen gueltig — mehrfach einfuegen
    // muss gehen.
    ck(aP.pasteGrabs(B,0)==1,"nochmals eingefuegt");
    ck(names(B)=="yxzqy","zweimal eingefuegt");

    // Unsinnige Ziele duerfen nicht abstuerzen.
    ck(aP.pasteGrabs(B,9999)==1,"Position hinter dem Ende wird geklemmt");
    ck(aP.pasteGrabs(99,0)==0,"unbekanntes Skript liefert nichts");
    ck(aP.copyGrabs(A,{})==0,"leere Auswahl kopiert nichts");

    fs::remove_all(rP);
  }

  // Vorschau: Kamera, Projektion, Abspielen.
  {
    using namespace g2::gui;

    // Ein Skelett mit bekannten Positionen.
    std::vector<g2::Mat3x4> w(3, g2::Mat3x4::identity());
    w[0].m[0][3]=0;  w[0].m[1][3]=0;  w[0].m[2][3]=0;
    w[1].m[0][3]=10; w[1].m[1][3]=0;  w[1].m[2][3]=0;
    w[2].m[0][3]=0;  w[2].m[1][3]=0;  w[2].m[2][3]=20;

    const auto b = computeBounds(w);
    ck(b.valid,"Kasten berechnet");
    ck(std::fabs(b.center()[0]-5.0f)<1e-4f,"Mitte in X");
    ck(std::fabs(b.center()[2]-10.0f)<1e-4f,"Mitte in Z");
    ck(std::fabs(b.radius()-10.0f)<1e-4f,"Radius aus der groessten Ausdehnung");

    PreviewCamera cam;
    frameAll(cam, b);
    ck(cam.distance > b.radius(), "Kamera steht ausserhalb des Modells");
    ck(std::fabs(cam.target[2]-10.0f)<1e-4f,"Blickpunkt in der Mitte");

    // Der Blickpunkt muss in der Bildmitte landen.
    float x=0,y=0,d=0;
    ck(projectPoint(cam.target, cam, 800.0f, 600.0f, x, y, d),"Blickpunkt sichtbar");
    ck(std::fabs(x-400.0f)<0.5f,"waagerecht mittig");
    ck(std::fabs(y-300.0f)<0.5f,"senkrecht mittig");
    ck(std::fabs(d-cam.distance)<0.5f,"Tiefe entspricht dem Abstand");

    // Ein Punkt HINTER der Kamera darf nicht gezeichnet werden — sonst
    // erscheint die Linie gespiegelt auf der falschen Seite.
    std::array<float,3> hinten = cam.target;
    hinten[0] += cam.distance * 4.0f;
    hinten[1] += cam.distance * 4.0f;
    const bool sichtbar = projectPoint(hinten, cam, 800.0f, 600.0f, x, y, d);
    ck(!sichtbar || d > 0.0f, "hinter der Kamera wird abgewiesen oder hat positive Tiefe");

    // Ein Punkt ueber dem Blickpunkt muss WEITER OBEN landen, also
    // kleineres y. Bildschirmkoordinaten laufen nach unten.
    PreviewCamera side; side.yawDeg=0; side.pitchDeg=0; side.distance=100;
    side.target = {{0,0,0}};
    float xm=0,ym=0,dm=0, xo=0,yo=0,dо=0;
    projectPoint({{0,0,0}}, side, 800, 600, xm, ym, dm);
    projectPoint({{0,0,20}}, side, 800, 600, xo, yo, dо);
    ck(yo < ym, "hoeher liegender Punkt erscheint weiter oben");

    // Abspielen mit der Rate der Sequenz.
    Playback pb; pb.playing = true;
    advancePlayback(pb, 0.5f, 20, 30);      // 0,5 s bei 20 Bildern/s = 10
    ck(pb.frame==10,"zehn Frames weiter");
    advancePlayback(pb, 1.5f, 20, 30);      // weitere 30 -> Umlauf
    ck(pb.frame==10,"laeuft um");

    // Negative Rate bedeutet rueckwaerts - steht so in Ravens cfg.
    Playback rb; rb.playing = true; rb.frame = 20;
    advancePlayback(rb, 0.5f, -20, 30);
    ck(rb.frame==10,"negative Rate laeuft rueckwaerts");

    // Angehalten bewegt sich nichts.
    Playback sb; sb.playing = false; sb.frame = 5;
    advancePlayback(sb, 10.0f, 20, 30);
    ck(sb.frame==5,"angehalten bleibt stehen");

    // Eine Sequenz mit einem Frame darf nicht in eine Endlosschleife laufen.
    Playback one; one.playing = true;
    advancePlayback(one, 10.0f, 20, 1);
    ck(one.frame==0,"ein Frame bleibt bei null");
  }

  // Vor dem Bauen speichern.
  //
  // Gebaut wurde bisher der Zustand im Speicher, waehrend die Datei auf der
  // Platte die alte blieb. Wer das Programm danach schloss, hatte eine GLA,
  // die zu keiner .car mehr passte.
  {
    const fs::path sd = fs::temp_directory_path()/"g2c_saveonbuild";
    fs::remove_all(sd); fs::create_directories(sd);
    { std::ofstream f(sd/"_humanoid.car");
      f<<"$aseanimgrabinit\n$aseanimgrab a.xsi -enum BOTH_STAND1\n$aseanimgrabfinalize\n"; }

    g2::gui::Platform pS;
    g2::gui::App aS(std::move(pS));
    aS.openCar((sd/"_humanoid.car").string());
    ck(aS.documents().size()==1,"Skript geoeffnet");
    ck(aS.documents()[0].script.grabs.size()==1,"ein Grab");

    makeXsi(sd/"neu.xsi");
    aS.addXsiFiles({(sd/"neu.xsi").string()}, false);
    ck(aS.documents()[0].dirty,"als geaendert markiert");
    ck(aS.documents()[0].script.grabs.size()==2,"zwei Grabs im Speicher");

    // Auf der Platte steht noch der alte Stand.
    {
      const auto vorher = g2::car::parseFile((sd/"_humanoid.car").string());
      ck(vorher.grabs.size()==1,"Datei traegt noch den alten Stand");
    }

    // Speichern muss den neuen Stand hinterlegen.
    ck(aS.saveDocument(0),"gespeichert");
    {
      const auto nachher = g2::car::parseFile((sd/"_humanoid.car").string());
      ck(nachher.grabs.size()==2,"Datei traegt jetzt den neuen Stand");
    }
    ck(!aS.documents()[0].dirty,"nicht mehr als geaendert markiert");

    fs::remove_all(sd);
  }

  // Protokoll als Text: was im Fehlerbericht landet.
  {
    g2::gui::Platform pL;
    g2::gui::App aL(std::move(pL));
    const std::string t = aL.logAsText();

    // Kopf mit dem, wonach sonst zurueckgefragt wird.
    ck(t.find("g2c - Protokoll")!=std::string::npos,"Kopfzeile vorhanden");
    ck(t.find("Gebaut:")!=std::string::npos,"Baudatum vorhanden");
    ck(t.find("Laufzeit:")!=std::string::npos,"Laufzeitangabe vorhanden");

    // Die Art muss im Text stehen: die Farbe geht beim Kopieren verloren,
    // und gerade sie unterscheidet Hinweis von Fehler.
    aL.openCar("/gibt/es/nicht.car");
    const std::string t2 = aL.logAsText();
    ck(t2.find("[")!=std::string::npos,"Art wird vorangestellt");

    // Und mehr Zeilen als vorher.
    ck(t2.size()>t.size(),"Meldung ist enthalten");
  }

  // Bindepose-Wahl: wirkt und bleibt erhalten.
  {
    g2::gui::Platform pB;
    g2::gui::App aB(std::move(pB));
    ck(aB.extract().basePose==g2::xsiexp::ExportOptions::BasePose::World,
       "Vorgabe ist die Weltpose");
  }

  // Mehrere Ordner auf einmal hinzufuegen.
  {
    const fs::path mr = fs::temp_directory_path()/"g2c_multifolder";
    fs::remove_all(mr); fs::create_directories(mr);
    { std::ofstream f(mr/"_humanoid.car");
      f<<"$aseanimgrabinit\n$aseanimgrab a.xsi -enum BOTH_STAND1\n$aseanimgrabfinalize\n"; }

    // Drei Quellordner mit je zwei Animationen.
    for (const char* d : {"quelle1","quelle2","quelle3"}) {
      fs::create_directories(mr/d);
      makeXsi(mr/d/"eins.xsi");
      makeXsi(mr/d/"zwei.xsi");
    }

    g2::gui::Platform pM;
    // Die Plattform meldet drei Ordner auf einmal.
    pM.pickFolders = [&](const char*, const std::string&) {
      return std::vector<std::string>{(mr/"quelle1").string(), (mr/"quelle2").string(),
                                      (mr/"quelle3").string()};
    };
    g2::gui::App aM(std::move(pM));
    aM.openCar((mr/"_humanoid.car").string());
    ck(aM.documents()[0].script.grabs.size()==1,"ein Grab am Anfang");

    const auto dirs = aM.askFolders("test","");
    ck(dirs.size()==3,"drei Ordner geliefert");
    // addXsiFolder liefert die Zahl der beruehrten SKRIPTE, nicht der
    // Dateien — bei einem offenen Skript also 1 je Ordner.
    for (const auto& d : dirs) aM.addXsiFolder(d, false);
    ck(aM.documents()[0].script.grabs.size()==7,
       "sechs Dateien aus drei Ordnern angehaengt");

    // Rueckfall: kann die Plattform nur einen, kommt einer - nie nichts.
    g2::gui::Platform pS;
    pS.pickFolder = [&](const char*, const std::string&) {
      return (mr/"quelle1").string();
    };
    g2::gui::App aS(std::move(pS));
    const auto einer = aS.askFolders("test","");
    ck(einer.size()==1,"Rueckfall auf Einzelauswahl");

    // Und ohne jede Auswahl darf nichts passieren.
    g2::gui::Platform pN;
    g2::gui::App aN(std::move(pN));
    ck(aN.askFolders("test","").empty(),"ohne Dialog leere Liste");

    fs::remove_all(mr);
  }

  // Trennlinien und Kommentare: der ganze Weg.
  //
  // In der Oberflaeche gesetzt -> in die .car geschrieben -> wieder
  // eingelesen -> in der animation.cfg. Jede Stufe einzeln zu pruefen
  // reicht nicht: der Wert muss durch alle vier.
  {
    const fs::path kd = fs::temp_directory_path()/"g2c_trenner";
    fs::remove_all(kd); fs::create_directories(kd);
    { std::ofstream f(kd/"_humanoid.car");
      f<<"$aseanimgrabinit\n"
       <<"$aseanimgrab a.xsi -enum BOTH_STAND1\n"
       <<"$aseanimgrab b.xsi -enum BOTH_WALK1\n"
       <<"$aseanimgrabfinalize\n"; }

    g2::gui::Platform pT;
    g2::gui::App aT(std::move(pT));
    aT.openCar((kd/"_humanoid.car").string());
    ck(aT.documents()[0].script.grabs.size()==2,"zwei Grabs");

    // Wie der Knopf es tut.
    aT.documents()[0].script.grabs[1].commentsBefore.push_back(
        "//////////////////////////////////////////");
    aT.documents()[0].script.grabs[1].commentsBefore.push_back("//  LAUFANIMATIONEN");
    ck(aT.saveDocument(0),"gespeichert");

    // Steht es wirklich in der Datei?
    {
      std::ifstream in(kd/"_humanoid.car");
      std::string all((std::istreambuf_iterator<char>(in)), {});
      ck(all.find("LAUFANIMATIONEN")!=std::string::npos,"Kommentar in der .car");
      ck(all.find("LAUFANIMATIONEN")<all.find("b.xsi"),"steht VOR seiner Sequenz");
    }

    // Und beim Wiedereinlesen?
    const auto wieder = g2::car::parseFile((kd/"_humanoid.car").string());
    ck(wieder.grabs.size()==2,"wieder zwei Grabs");
    if (wieder.grabs.size()==2) {
      ck(wieder.grabs[0].commentsBefore.empty(),"erster ohne Kommentar");
      ck(wieder.grabs[1].commentsBefore.size()==2,"zweiter mit zwei Zeilen");
    }

    // Bis in die animation.cfg.
    std::vector<g2::car::Sequence> sq;
    for (const auto& g : wieder.grabs) {
      g2::car::Sequence q;
      q.name = g.enumName ? *g.enumName : g.derivedName();
      q.frameCount = 2;
      q.commentsBefore = g.commentsBefore;
      sq.push_back(std::move(q));
    }
    const std::string cfg = g2::car::writeAnimationCfg(sq, "test");
    ck(cfg.find("LAUFANIMATIONEN")!=std::string::npos,"Kommentar in der cfg");
    ck(cfg.find("LAUFANIMATIONEN")<cfg.find("BOTH_WALK1"),"in der cfg vor der Sequenz");
    ck(cfg.find("LAUFANIMATIONEN")>cfg.find("BOTH_STAND1"),"und nach der vorigen");

    // Leere Kommentarzeilen duerfen nicht in die cfg.
    //
    // Beim Anlegen ueber das Kontextmenue entsteht zunaechst eine leere
    // Zeile. Bleibt sie leer, wird sie beim Bearbeiten wieder entfernt —
    // aber wenn doch eine durchrutscht, darf sie in der Datei nicht als
    // nacktes "//" landen.
    {
      std::vector<g2::car::Sequence> sq2;
      g2::car::Sequence q;
      q.name = "BOTH_STAND1";
      q.frameCount = 2;
      q.commentsBefore = {"// oben", "", "// unten"};
      sq2.push_back(std::move(q));
      const std::string cfg2 = g2::car::writeAnimationCfg(sq2, "t");
      ck(cfg2.find("// oben")!=std::string::npos,"erste Zeile da");
      ck(cfg2.find("// unten")!=std::string::npos,"dritte Zeile da");
      // Die leere wird zur Leerzeile, nicht zu "//"
      ck(cfg2.find("//\r\n//\r\n// unten")==std::string::npos,
         "leere Zeile wird nicht zu //");
    }

    fs::remove_all(kd);
  }

  // Trenner verschieben: er gehoert keiner Sequenz.
  {
    const fs::path vd = fs::temp_directory_path()/"g2c_trennerverschieben";
    fs::remove_all(vd); fs::create_directories(vd);
    { std::ofstream f(vd/"_humanoid.car");
      f<<"$aseanimgrabinit\n$aseanimgrab a.xsi -enum A\n$aseanimgrab b.xsi -enum B\n"
         "$aseanimgrab c.xsi -enum C\n$aseanimgrabfinalize\n"; }

    g2::gui::Platform pV;
    g2::gui::App aV(std::move(pV));
    aV.openCar((vd/"_humanoid.car").string());
    auto& sc = aV.documents()[0].script;
    ck(sc.grabs.size()==3,"drei Grabs");

    sc.grabs[1].commentsBefore.push_back("// TRENNER");

    // Nach oben: haengt sich an den vorigen Grab.
    ck(aV.moveComment(0,1,0,true),"nach oben verschoben");
    ck(sc.grabs[1].commentsBefore.empty(),"bei B weg");
    ck(sc.grabs[0].commentsBefore.size()==1,"bei A angekommen");

    // Und wieder zurueck.
    ck(aV.moveComment(0,0,0,false),"nach unten verschoben");
    ck(sc.grabs[1].commentsBefore.size()==1,"wieder bei B");

    // Bis ganz nach unten - hinter die letzte Animation.
    ck(aV.moveComment(0,1,0,false),"weiter nach unten");
    ck(sc.grabs[2].commentsBefore.size()==1,"bei C");
    ck(aV.moveComment(0,2,0,false),"ans Ende");
    ck(sc.trailingComments.size()==1,"hinter der letzten Animation");
    ck(sc.grabs[2].commentsBefore.empty(),"bei C weg");

    // Ueber den Rand hinaus geht nicht.
    ck(!aV.moveComment(0,2,0,false),"am Ende kein weiteres Verschieben");

    // Und das Ganze muss die .car ueberleben.
    ck(aV.saveDocument(0),"gespeichert");
    const auto neu = g2::car::parseFile((vd/"_humanoid.car").string());
    ck(neu.trailingComments.size()==1,"Schlusskommentar wieder eingelesen");

    fs::remove_all(vd);
  }

  // Zeilenkommentar: .car -> Anzeige -> .car -> animation.cfg
  {
    const fs::path td = fs::temp_directory_path()/"g2c_zeilenkommentar";
    fs::remove_all(td); fs::create_directories(td);
    { std::ofstream f(td/"_humanoid.car");
      f<<"$aseanimgrabinit\n"
         "$aseanimgrab a.xsi -enum BOTH_WALK1_ANI  // nur mit Anakins Schwert\n"
         "$aseanimgrab b.xsi -enum ROOT\n"
         "$aseanimgrabfinalize\n"; }

    g2::gui::Platform pZ;
    g2::gui::App aZ(std::move(pZ));
    aZ.openCar((td/"_humanoid.car").string());
    auto& sc = aZ.documents()[0].script;
    ck(sc.grabs.size()==2,"zwei Grabs");
    ck(sc.grabs[0].trailingComment.find("Anakins")!=std::string::npos,
       "Zeilenkommentar eingelesen");
    ck(sc.grabs[1].trailingComment.empty(),"zweiter ohne");

    // Der Kommentar darf die Auswertung nicht stoeren.
    ck(sc.grabs[0].enumName && *sc.grabs[0].enumName=="BOTH_WALK1_ANI",
       "enum trotz Kommentar erkannt");

    // Speichern und wieder lesen.
    sc.grabs[1].trailingComment = "// Basispose";
    ck(aZ.saveDocument(0),"gespeichert");
    const auto neu = g2::car::parseFile((td/"_humanoid.car").string());
    ck(neu.grabs.size()==2,"wieder zwei Grabs");
    if (neu.grabs.size()==2) {
      ck(neu.grabs[0].trailingComment.find("Anakins")!=std::string::npos,"erster erhalten");
      ck(neu.grabs[1].trailingComment.find("Basispose")!=std::string::npos,"zweiter erhalten");
    }

    // Und in der animation.cfg hinter den Zahlen.
    std::vector<g2::car::Sequence> sq;
    for (const auto& g : neu.grabs) {
      g2::car::Sequence q;
      q.name = g.enumName ? *g.enumName : g.derivedName();
      q.frameCount = 2;
      q.trailingComment = g.trailingComment;
      sq.push_back(std::move(q));
    }
    const std::string cfg = g2::car::writeAnimationCfg(sq,"t");
    const std::size_t z = cfg.find("BOTH_WALK1_ANI");
    const std::size_t k = cfg.find("Anakins");
    ck(z!=std::string::npos && k!=std::string::npos && k>z,
       "Kommentar steht HINTER dem Namen");
    // Zwischen beiden darf kein Zeilenumbruch liegen.
    ck(cfg.find('\n', z) > k, "in derselben Zeile");

    fs::remove_all(td);
  }

  // Trenner an beliebige Stelle umhaengen (was das Ziehen ausloest).
  {
    const fs::path dd = fs::temp_directory_path()/"g2c_trennerziehen";
    fs::remove_all(dd); fs::create_directories(dd);
    { std::ofstream f(dd/"_humanoid.car");
      f<<"$aseanimgrabinit\n$aseanimgrab a.xsi -enum A\n$aseanimgrab b.xsi -enum B\n"
         "$aseanimgrab c.xsi -enum C\n$aseanimgrabfinalize\n"; }

    g2::gui::Platform pD;
    g2::gui::App aD(std::move(pD));
    aD.openCar((dd/"_humanoid.car").string());
    auto& sc = aD.documents()[0].script;
    sc.grabs[0].commentsBefore.push_back("// TRENNER");

    // Von A direkt zu C - das Ziehen ueberspringt Zwischenstationen.
    ck(aD.moveComment(0,0,0,false),"eine Stufe");
    ck(sc.grabs[1].commentsBefore.size()==1,"bei B");
    ck(aD.moveComment(0,1,0,false),"noch eine");
    ck(sc.grabs[2].commentsBefore.size()==1,"bei C");

    // Bis hinter die letzte Animation und zurueck.
    ck(aD.moveComment(0,2,0,false),"ans Ende");
    ck(sc.trailingComments.size()==1,"hinter der letzten");
    ck(sc.grabs[2].commentsBefore.empty(),"bei C weg");

    // Der Text muss unterwegs unveraendert bleiben.
    ck(sc.trailingComments[0]=="// TRENNER","Text unveraendert");

    fs::remove_all(dd);
  }

  ck(!app.buildRunning(),"kein Bau aktiv");
  ck(app.logLines().size()>0,"Protokoll gefuellt");

  fs::remove_all(root);
  printf(fails? "  %d Fehler\n":"  Oberflaechenlogik in Ordnung\n", fails);
  return fails?1:0;
}
