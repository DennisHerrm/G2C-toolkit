// Test the UI logic without a window.
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
#include <map>
#include <tuple>
#include <iterator>
#include <functional>
namespace fs=std::filesystem;

// Create a small but valid .xsi.
//
// The UI rejects files that don't exist - made-up names are therefore no
// longer enough. That is intentional: a folder or a missing file should be
// noticed when it is added, not only at build time.
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

// --- Updater ----------------------------------------------------------------
//
// Everything except WinHTTP itself: versions, JSON, SHA-256, trust rules and
// the complete swap of the exe against a fake server - including every way
// it can go wrong. A failed update must never leave a broken exe behind.
namespace upd = g2::gui::update;

struct FakeServer {
  std::map<std::string, std::pair<int, std::string>> pages;   // url -> status, body
  std::map<std::string, std::string> files;                   // url -> content
  std::function<void()> duringDownload;                       // e.g. cancel
  int gets = 0, downloads = 0;

  upd::Network network() {
    upd::Network n;
    n.get = [this](const std::string& url, std::size_t maxBytes) {
      ++gets;
      upd::HttpResult r;
      const auto it = pages.find(url);
      if (it == pages.end()) { r.error = "no route to host"; return r; }
      r.status = it->second.first;
      r.body = it->second.second.substr(0, maxBytes);
      return r;
    };
    n.download = [this](const std::string& url, const fs::path& dest,
                        const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
      ++downloads;
      const auto it = files.find(url);
      if (it == files.end()) return std::string("HTTP 404");
      std::ofstream f(dest, std::ios::binary);
      f << it->second;
      f.close();
      if (duringDownload) duringDownload();
      if (!progress(it->second.size(), it->second.size())) return std::string("cancelled");
      return std::string();
    };
    return n;
  }
};

static std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

static void spit(const fs::path& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary);
  f << s;
}

static const std::string kDl = "https://github.com/DennisHerrm/G2C-toolkit/releases/download/v1.1.0/";

// Release JSON as GitHub sends it, with the noise around the fields we read.
static std::string releaseJson(const std::string& tag, const std::string& commit,
                               const std::vector<std::tuple<std::string, std::string, std::size_t>>& assets,
                               bool prerelease = false) {
  std::string j = "{\n  \"url\": \"https://api.github.com/repos/x/y/releases/1\",\n"
                  "  \"html_url\": \"https://github.com/DennisHerrm/G2C-toolkit/releases/tag/" + tag + "\",\n"
                  "  \"id\": 123456789, \"author\": {\"login\": \"bot\", \"id\": 41898282, \"site_admin\": false},\n"
                  "  \"tag_name\": \"" + tag + "\", \"target_commitish\": \"" + commit + "\",\n"
                  "  \"name\": \"g2c " + tag + "\", \"draft\": false, \"prerelease\": " +
                  (prerelease ? "true" : "false") + ",\n  \"created_at\": \"2026-09-27T10:00:00Z\",\n"
                  "  \"assets\": [";
  for (std::size_t i = 0; i < assets.size(); ++i) {
    const auto& [name, url, size] = assets[i];
    j += std::string(i ? "," : "") + "\n    {\"id\": " + std::to_string(1000 + i) + ", \"name\": \"" + name +
         "\", \"label\": null, \"uploader\": {\"login\": \"github-actions[bot]\"},"
         " \"content_type\": \"application/octet-stream\", \"size\": " + std::to_string(size) +
         ", \"download_count\": 0, \"browser_download_url\": \"" + url + "\"}";
  }
  j += "\n  ],\n  \"body\": \"## What's changed\\n* Fix \\\"quotes\\\" \\u00e4 \\ud83d\\ude00\\r\\n\"\n}\n";
  return j;
}


// Regression tests for the GUI findings of the code review (2026-10-03).
static void testReviewGui() {
  const fs::path root = fs::temp_directory_path() / "g2c_review_gui";
  std::error_code ec;
  fs::remove_all(root, ec);
  const fs::path eq = root / "a=b";   // a folder name with '=' in it
  fs::create_directories(eq);
  fs::create_directories(root / "c");
  const auto writeCar = [](const fs::path& f) {
    std::ofstream o(f);
    o << "$aseanimgrabinit\n$aseanimgrab anims/x.xsi -enum BOTH_STAND1\n$aseanimgrabfinalize\n";
  };
  const fs::path A = eq / "a.car", B = root / "c" / "b.car", C = root / "c" / "c.car";
  writeCar(A); writeCar(B); writeCar(C);

  {
    g2::gui::App app{g2::gui::Platform{}};
    while (!app.documents().empty()) app.closeDocument(0);
    app.openCar(A.string());
    app.openCar(B.string());
    app.openCar(C.string());
    app.documents()[0].outputDir = (root / "outA").string();
    app.documents()[1].outputDir = (root / "outB").string();
    app.activate(1);                    // B active
    app.closeDocument(0);               // A closed - its folder must stay known
    app.activate(0);                    // B again (index shifted)
  }
  fs::remove(C, ec);                    // C is gone on the next start
  {
    g2::gui::App app{g2::gui::Platform{}};
    ck(app.documents().size() == 1, "B wiederhergestellt, C fehlt auf der Platte");
    ck(!app.documents().empty() && fs::path(app.documents()[app.activeTab()].path).filename() == "b.car",
       "REGRESSION: der aktive Tab kommt nach Pfad zurueck");
    app.openCar(A.string());
    ck(app.documents().back().outputDir == (root / "outA").string(),
       "REGRESSION: Ausgabeordner eines geschlossenen Skripts im Ordner 'a=b' bleibt erhalten");
    while (!app.documents().empty()) app.closeDocument(0);
  }

  // Active tab by path when an earlier tab's file is gone.
  writeCar(C);
  {
    g2::gui::App app{g2::gui::Platform{}};
    while (!app.documents().empty()) app.closeDocument(0);
    app.openCar(A.string());
    app.openCar(B.string());
    app.openCar(C.string());
    app.activate(1);   // B
  }
  fs::remove(A, ec);   // the FIRST tab disappears
  {
    g2::gui::App app{g2::gui::Platform{}};
    ck(app.documents().size() == 2 && fs::path(app.documents()[app.activeTab()].path).filename() == "b.car",
       "REGRESSION: erster Tab fehlt -> trotzdem B aktiv (vorher C)");
    while (!app.documents().empty()) app.closeDocument(0);
  }
  fs::remove_all(root, ec);
}

static void testUpdater() {
  // Versions: the example ordering from the semver 2.0 specification.
  {
    const char* order[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                           "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1",
                           "1.1.0", "1.10.0", "2.0.0"};
    bool sorted = true;
    for (std::size_t i = 0; i + 1 < std::size(order); ++i) {
      const auto a = upd::parseVersion(order[i]), b = upd::parseVersion(order[i + 1]);
      if (!a || !b || upd::compare(*a, *b) >= 0 || upd::compare(*b, *a) <= 0) sorted = false;
    }
    ck(sorted, "Semver-Reihenfolge wie in der Spezifikation");
    const auto v = upd::parseVersion("v2.3.4+build.7");
    ck(v && v->major == 2 && v->minor == 3 && v->patch == 4 && v->pre.empty(), "v-Praefix und Build-Anhang");
    ck(upd::compare(*upd::parseVersion("1.2.3"), *upd::parseVersion("V1.2.3")) == 0, "v1.2.3 == 1.2.3");
    bool rejected = true;
    for (const char* bad : {"", "1", "1.2", "1.2.3.4", "1..3", "a.b.c", "1.2.x", "1.2.3-",
                            "1234567890.0.0", "dev", "snapshot-abc1234", "-1.0.0", " 1.0.0"})
      if (upd::parseVersion(bad)) rejected = false;
    ck(rejected, "ungueltige Versionen abgelehnt");
    ck(upd::kindOf({"1.0.0", ""}) == upd::BuildKind::Release, "Release erkannt");
    ck(upd::kindOf({"snapshot-abc1234", "abc1234ff"}) == upd::BuildKind::Snapshot, "Snapshot erkannt");
    ck(upd::kindOf({"dev", ""}) == upd::BuildKind::Dev, "eigener Bau erkannt");
    ck(upd::defaultChannel({"snapshot-abc1234", ""}) == upd::Channel::Snapshot, "Snapshot bleibt Snapshot");
    ck(upd::defaultChannel({"1.0.0", ""}) == upd::Channel::Stable, "Release bleibt stabil");
  }

  // SHA-256: the FIPS 180-2 test vectors.
  {
    ck(upd::sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 leer");
    ck(upd::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 abc");
    ck(upd::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256 448 Bit");
    const std::string million(1000000, 'a');
    ck(upd::sha256Hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
       "SHA-256 eine Million a");
    // Block boundaries: 55, 56, 63, 64 and 65 bytes take different padding paths.
    bool same = true;
    const fs::path f = fs::temp_directory_path() / "g2c_sha_test.bin";
    for (std::size_t n : {55u, 56u, 63u, 64u, 65u, 70000u}) {
      std::string s(n, '\0');
      for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>(i * 7 + 3);
      spit(f, s);
      if (upd::sha256File(f) != upd::sha256Hex(s)) same = false;
    }
    ck(same, "Datei und Speicher ergeben dieselbe Pruefsumme");
    fs::remove(f);
    ck(!upd::sha256File(fs::temp_directory_path() / "gibt_es_nicht.bin"), "fehlende Datei: keine Summe");

    const std::string h1(64, 'a'), h2 = "ABCDEF" + std::string(58, '0');
    const std::string sums = h1 + "  g2c.exe\r\n" + h2 + " *g2c-cli.exe\n\nmuell\n" + std::string(63, 'b') + "  kurz.exe\n";
    ck(upd::checksumFor(sums, "g2c.exe") == h1, "sha256sum-Zeile gefunden");
    ck(upd::checksumFor(sums, "G2C-CLI.EXE") == "abcdef" + std::string(58, '0'), "Binaermarkierung, Grossschreibung");
    ck(!upd::checksumFor(sums, "kurz.exe"), "zu kurzer Hash abgelehnt");
    ck(!upd::checksumFor(sums, "fehlt.exe"), "fehlender Eintrag");
    ck(!upd::checksumFor(sums, "g2c"), "nur ganze Namen");
  }

  // JSON: a realistic answer, then everything that isn't one.
  {
    const auto r = upd::parseRelease(releaseJson("v1.1.0", "0123456789abcdef", {
        {"g2c.exe", kDl + "g2c.exe", 4242}, {"SHA256SUMS.txt", kDl + "SHA256SUMS.txt", 200}}));
    ck(r.has_value(), "Release gelesen");
    if (r) {
      ck(r->tag == "v1.1.0" && r->commit == "0123456789abcdef" && !r->prerelease, "Felder");
      ck(r->assets.size() == 2 && r->find("G2C.EXE") && r->find("g2c.exe")->size == 4242, "Dateien");
      ck(r->notes.find("\"quotes\"") != std::string::npos, "Escape \\\"");
      ck(r->notes.find("\xC3\xA4") != std::string::npos, "\\u00e4 -> UTF-8");
      ck(r->notes.find("\xF0\x9F\x98\x80") != std::string::npos, "Surrogatpaar -> UTF-8");
      ck(r->htmlUrl.find("/releases/tag/v1.1.0") != std::string::npos, "Seite der Version");
    }
    bool none = true;
    const std::string deep = std::string(100000, '[');
    for (const std::string& bad : {std::string(), std::string("[]"), std::string("{"), std::string("null"),
                                   std::string("{\"tag_name\": 1}"), std::string("{\"tag_name\": \"v1\""),
                                   std::string("{\"tag_name\": \"v1\"} x"), std::string("{\"a\" 1}"),
                                   std::string("{\"tag_name\": \"\\ud800\"}"), std::string("{\"tag_name\": \"a\nb\"}"),
                                   std::string("{\"tag_name\": \"\\x\"}"), std::string("{\"tag_name\": tru}"),
                                   std::string("{\"message\": \"Not Found\"}"), deep})
      if (upd::parseRelease(bad)) none = false;
    ck(none, "kaputtes JSON abgelehnt, auch 100000 Ebenen tief");
    const auto odd = upd::parseRelease("{\"tag_name\":\"v1.0.0\",\"assets\":[{\"name\":\"x\"},{\"browser_download_url\":\"u\"},"
                                       "{\"name\":\"g2c.exe\",\"browser_download_url\":\"u\",\"size\":-5}],\"prerelease\":\"ja\"}");
    ck(odd && odd->assets.size() == 1 && odd->assets[0].size == 0 && !odd->prerelease, "unvollstaendige Eintraege uebergangen");
  }

  // Which release is worth offering?
  {
    upd::Release stable;
    stable.tag = "v1.2.0";
    upd::Release snap;
    snap.tag = "snapshot";
    snap.commit = "abcdef0123456789";
    ck(upd::isNewer({"1.1.9", ""}, stable, upd::Channel::Stable), "1.2.0 > 1.1.9");
    ck(!upd::isNewer({"1.2.0", ""}, stable, upd::Channel::Stable), "gleiche Version");
    ck(!upd::isNewer({"1.3.0", ""}, stable, upd::Channel::Stable), "aeltere wird nicht angeboten");
    ck(upd::isNewer({"1.2.0-rc.1", ""}, stable, upd::Channel::Stable), "Final nach Release-Kandidat");
    ck(upd::isNewer({"snapshot-1234567", "1234567"}, stable, upd::Channel::Stable), "Snapshot -> stabil");
    upd::Release pre = stable;
    pre.prerelease = true;
    ck(!upd::isNewer({"1.0.0", ""}, pre, upd::Channel::Stable), "Vorabversion nicht auf stabil");
    upd::Release junk = stable;
    junk.tag = "latest";
    ck(!upd::isNewer({"1.0.0", ""}, junk, upd::Channel::Stable), "Tag ohne Nummer nicht angeboten");
    ck(upd::isNewer({"snapshot-1234567", "1234567aaa"}, snap, upd::Channel::Snapshot), "neuer Commit");
    ck(!upd::isNewer({"snapshot-abcdef0", "abcdef0"}, snap, upd::Channel::Snapshot), "gekuerzter gleicher Commit");
    ck(!upd::isNewer({"snapshot-abcdef0", "abcdef0123456789"}, snap, upd::Channel::Snapshot), "gleicher Commit");
    ck(upd::isNewer({"1.2.0", ""}, snap, upd::Channel::Snapshot), "Release -> Snapshot");
    upd::Release noCommit = snap;
    noCommit.commit.clear();
    ck(!upd::isNewer({"1.2.0", ""}, noCommit, upd::Channel::Snapshot), "Snapshot ohne Commit nie");
    ck(upd::releaseId(stable, upd::Channel::Stable) == "v1.2.0", "Kennung stabil = Tag");
    ck(upd::releaseId(snap, upd::Channel::Snapshot) == snap.commit, "Kennung Snapshot = Commit");
    ck(upd::displayName(stable, upd::Channel::Stable) == "1.2.0", "Anzeige ohne v");
    ck(upd::displayName(snap, upd::Channel::Snapshot) == "snapshot abcdef0", "Anzeige Snapshot");
    ck(upd::apiUrl(upd::Channel::Stable) == "https://api.github.com/repos/DennisHerrm/G2C-toolkit/releases/latest", "API stabil");
    ck(upd::apiUrl(upd::Channel::Snapshot) == "https://api.github.com/repos/DennisHerrm/G2C-toolkit/releases/tags/snapshot", "API Snapshot");
  }

  // Only downloads from this repository's releases.
  {
    ck(upd::trustedDownloadUrl(kDl + "g2c.exe"), "eigene Release-Datei");
    ck(upd::trustedDownloadUrl("https://github.com/dennisherrm/g2c-toolkit/releases/download/snapshot/g2c.exe"), "Gross/klein egal");
    bool refused = true;
    for (const char* bad : {"http://github.com/DennisHerrm/G2C-toolkit/releases/download/v1/g2c.exe",
                            "https://github.com/DennisHerrm/G2C-toolkit-evil/releases/download/v1/g2c.exe",
                            "https://github.com/Other/G2C-toolkit/releases/download/v1/g2c.exe",
                            "https://github.com.evil.example/DennisHerrm/G2C-toolkit/releases/download/v1/g2c.exe",
                            "https://github.com/DennisHerrm/G2C-toolkit/releases/download/../../../x/g2c.exe",
                            "https://github.com/DennisHerrm/G2C-toolkit/releases/download/",
                            "https://github.com/DennisHerrm/G2C-toolkit/archive/main.zip", ""})
      if (upd::trustedDownloadUrl(bad)) refused = false;
    ck(refused, "fremde Adressen abgelehnt");
  }

  // Swapping a file: rename away, rename in, undo on failure.
  {
    const fs::path d = fs::temp_directory_path() / "g2c_replace_test";
    fs::remove_all(d);
    fs::create_directories(d);
    spit(d / "g2c.exe", "MZalt");
    spit(d / "neu", "MZneu");
    ck(upd::replaceFile(d / "g2c.exe", d / "neu").empty(), "ersetzt");
    ck(slurp(d / "g2c.exe") == "MZneu" && slurp(d / "g2c.exe.old") == "MZalt", "neu an Ort und Stelle, alt als .old");
    spit(d / "neu2", "MZneu2");
    ck(upd::replaceFile(d / "g2c.exe", d / "neu2").empty(), "zweites Mal trotz altem .old");
    ck(slurp(d / "g2c.exe") == "MZneu2" && slurp(d / "g2c.exe.old") == "MZneu", "richtige Reihenfolge");
    ck(!upd::replaceFile(d / "g2c.exe", d / "fehlt").empty(), "fehlende Quelle gemeldet");
    ck(slurp(d / "g2c.exe") == "MZneu2", "Ziel nach Fehlschlag unveraendert");
    // A failed swap already cleared the old .old - put one back, as a real
    // update leaves it.
    spit(d / "g2c.exe.old", "MZrest");
    ck(upd::cleanupAfterUpdate(d / "g2c.exe"), "Reste erkannt");
    ck(!fs::exists(d / "g2c.exe.old"), ".old entfernt");
    ck(!upd::cleanupAfterUpdate(d / "g2c.exe"), "danach nichts mehr");
    fs::remove_all(d);
  }

  // The whole flow against a fake server.
  const fs::path dir = fs::temp_directory_path() / "g2c_update_test";
  const std::string newExe = "MZ" + std::string(300000, 'N');
  const std::string newCli = "MZ" + std::string(1000, 'C');
  const auto setup = [&](FakeServer& srv, const std::string& sums, const std::string& exeContent) {
    fs::remove_all(dir);
    fs::create_directories(dir);
    spit(dir / "g2c.exe", "MZalt");
    spit(dir / "g2c-cli.exe", "MZcli-alt");
    srv.files[kDl + "g2c.exe"] = exeContent;
    srv.files[kDl + "g2c-cli.exe"] = newCli;
    srv.files[kDl + "SHA256SUMS.txt"] = sums;
    srv.pages[kDl + "SHA256SUMS.txt"] = {200, sums};
    srv.pages[upd::apiUrl(upd::Channel::Stable)] = {200, releaseJson("v1.1.0", "feedbeef", {
        {"g2c-v1.1.0-win64.zip", kDl + "g2c-v1.1.0-win64.zip", 1},
        {"g2c.exe", kDl + "g2c.exe", exeContent.size()},
        {"g2c-cli.exe", kDl + "g2c-cli.exe", newCli.size()},
        {"SHA256SUMS.txt", kDl + "SHA256SUMS.txt", sums.size()}})};
  };
  const std::string goodSums = upd::sha256Hex(newExe) + "  g2c.exe\n" + upd::sha256Hex(newCli) + "  g2c-cli.exe\n";
  const auto leftovers = [&] {
    return fs::exists(dir / "g2c.exe.download") || fs::exists(dir / "g2c-cli.exe.download");
  };

  {
    FakeServer srv;
    setup(srv, goodSums, newExe);
    upd::Updater u(srv.network(), {"1.0.0", ""}, dir / "g2c.exe");
    ck(!u.justUpdated(), "kein Update vorher");
    u.check(upd::Channel::Stable, true);
    u.wait();
    auto st = u.status();
    ck(st.phase == upd::Phase::Available && st.release.tag == "v1.1.0", "neue Version gefunden");
    ck(fs::file_size(dir / "g2c.exe") == 5, "Pruefen allein aendert nichts");
    u.install();
    u.wait();
    st = u.status();
    ck(st.phase == upd::Phase::Installed && st.detail.empty(), "installiert");
    ck(slurp(dir / "g2c.exe") == newExe, "g2c.exe ersetzt");
    ck(slurp(dir / "g2c-cli.exe") == newCli, "g2c-cli.exe daneben mit ersetzt");
    ck(slurp(dir / "g2c.exe.old") == "MZalt", "alte Exe als .old");
    ck(st.done == st.total && st.total == newExe.size() + newCli.size(), "Fortschritt vollstaendig");
    ck(!leftovers(), "keine .download-Reste");
    u.install();
    u.wait();
    ck(u.status().phase == upd::Phase::Installed, "zweites install() ohne Wirkung");

    upd::Updater next(srv.network(), {"1.1.0", ""}, dir / "g2c.exe");
    ck(next.justUpdated(), "Neustart erkennt das Update");
    next.startup(false, upd::Channel::Stable);
    next.wait();
    ck(!fs::exists(dir / "g2c.exe.old") && !fs::exists(dir / "g2c-cli.exe.old"), ".old beim Start aufgeraeumt");
    ck(next.status().phase == upd::Phase::Idle, "ohne Auftrag keine Pruefung");
    const int before = srv.gets;
    next.startup(true, upd::Channel::Stable);
    next.wait();
    ck(srv.gets == before + 1 && next.status().phase == upd::Phase::UpToDate && !next.status().manual,
       "automatische Pruefung: aktuell, still");
  }

  // Every failure: reported, nothing replaced, nothing left behind.
  const auto failCase = [&](const char* what, upd::Error expected, auto&& tweak) {
    FakeServer srv;
    setup(srv, goodSums, newExe);
    upd::Updater u(srv.network(), {"1.0.0", ""}, dir / "g2c.exe");
    tweak(srv, u);
    u.check(upd::Channel::Stable, false);
    u.wait();
    if (u.status().phase == upd::Phase::Available) {
      u.install();
      u.wait();
    }
    const auto st = u.status();
    const bool ok = st.phase == upd::Phase::Failed && st.error == expected &&
                    slurp(dir / "g2c.exe") == "MZalt" && slurp(dir / "g2c-cli.exe") == "MZcli-alt" && !leftovers();
    if (!ok) printf("    %s: Phase %d, Fehler %d, Detail '%s'\n", what, static_cast<int>(st.phase),
                    static_cast<int>(st.error), st.detail.c_str());
    ck(ok, what);
  };
  failCase("falsche Pruefsumme", upd::Error::Checksum, [&](FakeServer& s, upd::Updater&) {
    s.files[kDl + "g2c.exe"] = "MZ" + std::string(300000, 'X');
  });
  failCase("HTML-Seite statt Exe", upd::Error::Checksum, [&](FakeServer& s, upd::Updater&) {
    const std::string html = "<html>" + std::string(300002 - 13, ' ') + "</html>";
    s.files[kDl + "g2c.exe"] = html;
    s.pages[kDl + "SHA256SUMS.txt"].second = upd::sha256Hex(html) + "  g2c.exe\n" + upd::sha256Hex(newCli) + "  g2c-cli.exe\n";
  });
  failCase("abgeschnittener Download", upd::Error::Checksum, [&](FakeServer& s, upd::Updater&) {
    s.files[kDl + "g2c.exe"] = newExe.substr(0, 1000);
  });
  failCase("CLI kaputt: auch g2c.exe bleibt", upd::Error::Checksum, [&](FakeServer& s, upd::Updater&) {
    s.files[kDl + "g2c-cli.exe"] = "MZ" + std::string(1000, 'Y');
  });
  failCase("Pruefsummendatei ohne Eintrag", upd::Error::NoAsset, [&](FakeServer& s, upd::Updater&) {
    s.pages[kDl + "SHA256SUMS.txt"].second = upd::sha256Hex(newCli) + "  g2c-cli.exe\n";
  });
  failCase("fremde Download-Adresse", upd::Error::Untrusted, [&](FakeServer& s, upd::Updater&) {
    auto& body = s.pages[upd::apiUrl(upd::Channel::Stable)].second;
    const std::string from = kDl + "g2c.exe\"", to = "https://evil.example/g2c.exe\"";
    body.replace(body.find(from), from.size(), to);
  });
  failCase("Version ohne Pruefsummen", upd::Error::NoAsset, [&](FakeServer& s, upd::Updater&) {
    auto& body = s.pages[upd::apiUrl(upd::Channel::Stable)].second;
    body.replace(body.find("\"SHA256SUMS.txt\""), 16, "\"README.txt\"");
  });
  failCase("Download bricht ab", upd::Error::Network, [&](FakeServer& s, upd::Updater&) {
    s.files.erase(kDl + "g2c.exe");
  });
  failCase("Abbrechen waehrend des Downloads", upd::Error::Cancelled, [&](FakeServer& s, upd::Updater& u) {
    s.duringDownload = [&u] { u.cancel(); };
  });
  failCase("keine Verbindung", upd::Error::Network, [&](FakeServer& s, upd::Updater&) { s.pages.clear(); });
  failCase("noch nichts veroeffentlicht", upd::Error::NoRelease, [&](FakeServer& s, upd::Updater&) {
    s.pages[upd::apiUrl(upd::Channel::Stable)] = {404, "{\"message\": \"Not Found\"}"};
  });
  failCase("Ratenlimit", upd::Error::Network, [&](FakeServer& s, upd::Updater&) {
    s.pages[upd::apiUrl(upd::Channel::Stable)] = {403, "{\"message\": \"API rate limit exceeded\"}"};
  });
  // REGRESSION: a folder without write access is not a network problem.
  {
    FakeServer srv;
    setup(srv, goodSums, newExe);
    auto net = srv.network();
    net.download = [](const std::string&, const fs::path& dest,
                      const std::function<bool(std::uint64_t, std::uint64_t)>&) {
      return std::string("write:") + dest.parent_path().string();
    };
    upd::Updater u(net, {"1.0.0", ""}, dir / "g2c.exe");
    u.check(upd::Channel::Stable, false);
    u.wait();
    u.install();
    u.wait();
    ck(u.status().phase == upd::Phase::Failed && u.status().error == upd::Error::Write,
       "REGRESSION: Ordner nicht beschreibbar = Schreibfehler, nicht 'keine Verbindung'");
  }
  // REGRESSION: installed, "Later", check again -> not offered a second time.
  {
    FakeServer srv;
    setup(srv, goodSums, newExe);
    upd::Updater u(srv.network(), {"1.0.0", ""}, dir / "g2c.exe");
    u.check(upd::Channel::Stable, true);
    u.wait();
    u.install();
    u.wait();
    u.dismiss();
    const int downloads = srv.downloads;
    u.check(upd::Channel::Stable, true);
    u.wait();
    ck(u.status().phase == upd::Phase::Installed && srv.downloads == downloads,
       "REGRESSION: schon installiert -> Neustart anbieten statt erneut laden");
  }
  failCase("Antwort kein JSON", upd::Error::BadAnswer, [&](FakeServer& s, upd::Updater&) {
    s.pages[upd::apiUrl(upd::Channel::Stable)] = {200, "<html>captive portal</html>"};
  });

  // Exe in a folder where the rename fails: the .old slot is taken by a
  // directory, so moving the running exe away is impossible.
  {
    FakeServer srv;
    setup(srv, goodSums, newExe);
    fs::create_directories(dir / "g2c.exe.old" / "blockiert");
    upd::Updater u(srv.network(), {"1.0.0", ""}, dir / "g2c.exe");
    u.check(upd::Channel::Stable, false);
    u.wait();
    u.install();
    u.wait();
    ck(u.status().phase == upd::Phase::Failed && u.status().error == upd::Error::Write, "nicht ersetzbar: gemeldet");
    ck(slurp(dir / "g2c.exe") == "MZalt" && !leftovers(), "nicht ersetzbar: alte Exe unversehrt");
  }

  // Snapshot channel compares commits.
  {
    FakeServer srv;
    setup(srv, goodSums, newExe);
    srv.pages[upd::apiUrl(upd::Channel::Snapshot)] = {200, releaseJson("snapshot", "feedbeef00112233", {}, true)};
    upd::Updater same(srv.network(), {"snapshot-feedbee", "feedbeef00112233"}, dir / "g2c.exe");
    same.check(upd::Channel::Snapshot, true);
    same.wait();
    ck(same.status().phase == upd::Phase::UpToDate && same.status().manual, "Snapshot aktuell");
    upd::Updater older(srv.network(), {"snapshot-1234567", "1234567"}, dir / "g2c.exe");
    older.check(upd::Channel::Snapshot, false);
    older.wait();
    ck(older.status().phase == upd::Phase::Available, "neuerer Snapshot");
    older.dismiss();
    ck(older.status().phase == upd::Phase::Idle, "Spaeter -> Ruhe");
  }

  // The App: checks by itself as a release, never as a self-built version,
  // and keeps the update settings.
  {
    FakeServer srv;
    setup(srv, goodSums, newExe);
    {
      g2::gui::Platform p;
      p.network = srv.network();
      p.exePath = dir / "g2c.exe";
      p.build = {"dev", ""};
      g2::gui::App a(std::move(p));
      ck(a.updater() != nullptr, "Updater vorhanden");
      a.updater()->wait();
      ck(srv.gets == 0 && a.updater()->status().phase == upd::Phase::Idle, "eigener Bau fragt nicht selbst");
    }
    {
      g2::gui::Platform p;
      p.network = srv.network();
      p.exePath = dir / "g2c.exe";
      p.build = {"1.0.0", ""};
      g2::gui::App a(std::move(p));
      a.updater()->wait();
      ck(srv.gets == 1 && a.updater()->status().phase == upd::Phase::Available, "Release fragt beim Start");
      ck(a.updateChannel() == upd::Channel::Stable, "Kanal aus dem Bau");
      a.settings().updateChannel = 1;
      a.settings().skippedUpdate = "v1.1.0";
      a.settings().checkUpdates = false;
      ck(!a.restartRequested(), "kein Neustart ohne Klick");
    }
    {
      g2::gui::Platform p;
      p.network = srv.network();
      p.exePath = dir / "g2c.exe";
      p.build = {"1.0.0", ""};
      g2::gui::App a(std::move(p));
      a.updater()->wait();
      ck(a.settings().updateChannel == 1 && a.settings().skippedUpdate == "v1.1.0" && !a.settings().checkUpdates,
         "Update-Einstellungen gespeichert");
      ck(srv.gets == 1, "abgeschaltet: keine Pruefung beim Start");
      ck(a.updateChannel() == upd::Channel::Snapshot, "gewaehlter Kanal gilt");
    }
    {
      g2::gui::Platform p;   // no network: no updater, no crash
      g2::gui::App a(std::move(p));
      ck(a.updater() == nullptr, "ohne Netz kein Updater");
      a.checkForUpdates(true);
    }
  }
  fs::remove_all(dir);
}

int main(){
  // Don't touch the user's settings.
  //
  // Every App restores the last open scripts from %APPDATA%\g2c on startup
  // and writes the settings back on exit. Without the redirect this test
  // opened the user's real .car files, possibly saved one of them via
  // saveDocument(0), and finally overwrote their tabs and output folders
  // with test paths.
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
  g2::gui::Platform plat;  // deliberately empty: the logic has to cope with that
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

  // Tab titles must be distinguishable. In a model tree all scripts have the
  // same name; then the folder decides.
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

    // If only one is left, the file name may be used again.
    a2.closeDocument(0); a2.closeDocument(0);
    ck(a2.documents().size()==1,"eines uebrig");
    ck(a2.documents()[0].title=="_humanoid.car","eindeutiger Dateiname wird wieder benutzt");
    fs::remove_all(r2);
  }

  // Append a folder full of .xsi files.
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
    // Sorted, so the order doesn't depend on the file system.
    const auto& gs = a3.documents()[0].script.grabs;
    bool sorted = true;
    for (size_t i=1;i<gs.size();++i) if (gs[i-1].file > gs[i].file) sorted = false;
    ck(sorted,"Reihenfolge ist sortiert");

    fs::remove_all(xd); fs::remove_all(r3);
  }

  // "Only missing": pick the whole folder again, only new .xsi go in.
  {
    const fs::path xd = fs::temp_directory_path()/"g2c_gui_onlynew";
    fs::remove_all(xd); fs::create_directories(xd/"unter");
    for (auto n : {"a.xsi","b.xsi","unter/c.xsi"}) makeXsi(xd/n);
    const fs::path rc = fs::temp_directory_path()/"g2c_gui_onlynew_car";
    fs::remove_all(rc); fs::create_directories(rc);
    // Already grabbed: a.xsi with a different case, c without its extension
    // (Carcass appends .XSI) - both must count as present.
    { std::ofstream f(rc/"m.car"); f<<"$aseanimgrabinit\n$aseanimgrab A.XSI\n$aseanimgrab unter/c\n"
                                       "$aseanimgrabfinalize\n"; }
    { std::ofstream f(rc/"n.car"); f<<"$aseanimgrabinit\n$aseanimgrabfinalize\n"; }

    g2::gui::Platform pN;
    g2::gui::App aN(std::move(pN));
    aN.settings().baseDir = xd.string();
    ck(g2::gui::Settings{}.onlyNewXsi,"Nur neue XSI ist von Haus aus an");
    aN.settings().onlyNewXsi = true;
    aN.openCar((rc/"m.car").string());
    const auto gm = [&]() -> const g2::gui::Document& {
      for (auto& d : aN.documents()) if (fs::path(d.path).filename() == "m.car") return d;
      return aN.documents()[0];
    };
    ck(aN.addXsiFolder(xd.string(), false)==1,"Nur neue XSI: Skript wurde geaendert");
    ck(gm().script.grabs.size()==3,"Nur neue XSI: nur b.xsi kam dazu");
    ck(gm().script.grabs.back().file=="b.xsi","Nur neue XSI: neue Zeile relativ zur Assetwurzel");
    ck(aN.addXsiFolder(xd.string(), false)==0,"Nur neue XSI: zweiter Durchlauf aendert nichts");
    ck(gm().script.grabs.size()==3,"Nur neue XSI: keine doppelten Zeilen");

    makeXsi(xd/"d.xsi");
    aN.addXsiFiles({(xd/"d.xsi").string(),(xd/"d.xsi").string()}, false);
    ck(gm().script.grabs.size()==4,"Nur neue XSI: dieselbe Datei zweimal auf einmal - einmal eingefuegt");

    // To all: each script gets what IT is missing.
    aN.openCar((rc/"n.car").string());
    ck(aN.addXsiFiles({(xd/"a.xsi").string(),(xd/"d.xsi").string()}, true)==1,
       "Nur fehlende, zu allen: nur das Skript ohne die Dateien wurde geaendert");
    for (auto& d : aN.documents())
      ck(d.script.grabs.size()==(fs::path(d.path).filename()=="n.car" ? 2u : 4u),
         "Nur fehlende, zu allen: jedes Skript hat jede Datei genau einmal");

    // Off: the old behaviour, everything is appended.
    aN.settings().onlyNewXsi = false;
    aN.addXsiFiles({(xd/"a.xsi").string()}, false);
    ck(aN.documents()[aN.activeTab()].script.grabs.size()==3,"ohne Haken wird wie bisher alles angehaengt");
    fs::remove_all(xd); fs::remove_all(rc);
  }

  // Separate output location per script.
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

  // Saving: create a backup, keep the content, reset dirty.
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

    // Read it back: the content must be complete.
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

  // Output location: must be set per script, there is no shared one.
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

    // Crucial: the targets must be DIFFERENT. A shared folder would let the
    // three identically named GLAs overwrite each other.
    std::set<std::string> outs;
    for (auto&d : a7.documents()) outs.insert(d.outputDir);
    ck(outs.size()==3,"drei verschiedene Ausgabeorte");

    // Targets that are already set are not overwritten.
    a7.documents()[0].outputDir = "C:/eigen";
    const std::size_t n8 = a7.assignDefaultOutputs(true);
    ck(n8==0,"nichts zu setzen, wenn alle ein Ziel haben");
    ck(a7.documents()[0].outputDir=="C:/eigen","eigenes Ziel bleibt unangetastet");

    fs::remove_all(r7);
  }

  // Translations: every entry must be filled in for all four languages.
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
      // Chinese must not simply be the German word - that would be a
      // forgotten translation.
      //
      // Exempt are texts WITHOUT translatable content: pure format strings
      // like "%zu/%zu  %s" and proper names like "g2c_out". The rule checks
      // this on the text itself instead of maintaining an exception list -
      // a list goes stale as soon as someone adds an entry.
      const std::string de = txt[0], zh = txt[2];
      std::string rest;
      for (size_t k=0;k<de.size();++k) {
        if (de[k]=='%') { while (k+1<de.size() && !isupper((unsigned char)de[k+1])
                                 && !islower((unsigned char)de[k+1])) k++;
                          k++; continue; }
        rest += de[k];
      }
      // Only text that contains words at all is translatable.
      //
      // Not translatable and therefore exempt:
      //   - pure format strings ("%zu/%zu  %s")
      //   - abbreviations and format names without lowercase letters ("XSI -> GLA")
      //   - file names ("animation.cfg")
      // The rule checks this on the text itself instead of maintaining an
      // exception list - a list goes stale as soon as someone adds something.
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

    // German and English texts may only contain Latin-1.
    //
    // For these languages the font atlas is baked with GetGlyphRangesDefault,
    // i.e. 0x20 to 0xFF. An em dash or a typographic quotation mark lies
    // above that and shows up as a question mark - which is exactly how the
    // menu ended up showing "Assetwurzel ? enthaelt models".
    int ausserhalb=0;
    for (int l=0;l<2;l++) {
      setLanguage((Lang)l);
      for (int i=0;i<(int)S::Count;i++)
        for (const unsigned char* q=(const unsigned char*)tr((S)i); *q; ++q)
          if (*q >= 0x80) { ausserhalb++; break; }
    }
    ck(ausserhalb==0,"de/en enthalten nur darstellbare Zeichen");

    // Language names must be readable in the active language.
    setLanguage(Lang::De);
    ck(std::string(langName(Lang::Zh))=="Chinesisch","Sprachname auf Deutsch lesbar");
    setLanguage(Lang::En);
    ck(std::string(langName(Lang::Ja))=="Japanese","Sprachname auf Englisch lesbar");
    ck(gleich==0,"kein chinesischer Text ist nur das deutsche Wort");

    // Umlauts and CJK must arrive as UTF-8, not as question marks.
    setLanguage(Lang::Zh);
    const std::string zhFile = tr(S::MenuFile);
    ck(zhFile.size()>3,"chinesischer Text ist mehrbyte-kodiert");
    setLanguage(Lang::Ja);
    const std::string jaFile = tr(S::MenuFile);
    ck(jaFile.size()>3,"japanischer Text ist mehrbyte-kodiert");
    ck(zhFile!=jaFile,"Chinesisch und Japanisch sind verschieden");
    setLanguage(before);
  }

  // Settings folder: not the working directory, except in portable
  // mode.
  {
    const std::string cfg = g2::gui::App::configDir();
    ck(!cfg.empty(),"Einstellungsordner ermittelt");
    ck(fs::exists(cfg),"Einstellungsordner existiert");

    const bool portable = fs::exists(fs::current_path()/"g2c_portable.txt");
    if (!portable)
      ck(fs::path(cfg) != fs::current_path(),
         "ohne Markierung nicht im Arbeitsverzeichnis");

    // With the marker file it must end up in the working directory.
    { std::ofstream f(fs::current_path()/"g2c_portable.txt"); f<<"\n"; }
    ck(fs::path(g2::gui::App::configDir()) == fs::current_path(),
       "mit Markierung mitnehmbar neben der Exe");
    fs::remove(fs::current_path()/"g2c_portable.txt");
    ck(fs::path(g2::gui::App::configDir()) != fs::current_path() || portable,
       "ohne Markierung wieder der uebliche Ort");
  }

  // Icons: only show them if the font could be loaded.
  {
    using namespace g2::gui;
    setIconsAvailable(false);
    ck(std::string(withIcon(ICON_SAVE,"Speichern"))=="Speichern",
       "ohne Schrift nur Text, keine leeren Kaesten");

    setIconsAvailable(true);
    const std::string mit = withIcon(ICON_SAVE,"Speichern");
    ck(mit.size() > std::strlen("Speichern"), "mit Schrift kommt ein Symbol dazu");
    ck(mit.find("Speichern")!=std::string::npos, "der Text bleibt erhalten");

    // All icons must lie in the private-use range the font provides. A
    // character outside it would be a typo in the code and would show up
    // as an empty box.
    const char* alle[] = {ICON_FOLDER, ICON_FOLDER_OPEN, ICON_OPEN_FILE, ICON_SAVE,
                          ICON_SAVE_ALL, ICON_BUILD, ICON_CHECK, ICON_VALIDATE, ICON_ADD,
                          ICON_CANCEL, ICON_SETTINGS, ICON_GLOBE, ICON_DELETE, ICON_EDIT};
    int ausserhalb=0;
    for (const char* ic : alle) {
      const unsigned char* q = (const unsigned char*)ic;
      // Decode three-byte UTF-8.
      if ((q[0]&0xF0)!=0xE0) { ausserhalb++; continue; }
      const unsigned cp = ((q[0]&0x0F)<<12) | ((q[1]&0x3F)<<6) | (q[2]&0x3F);
      if (cp < kIconRangeMin || cp > kIconRangeMax) ausserhalb++;
    }
    ck(ausserhalb==0,"alle Symbole liegen im Bereich der Icon-Schrift");
    setIconsAvailable(false);
  }

  // Reordering, deleting, creating new.
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

    // The selection must move along, otherwise a later delete hits the
    // wrong sequence.
    auto&d9 = a9.documents()[0];
    std::fill(d9.selected.begin(), d9.selected.end(), 0);
    d9.selected[0]=1;                       // "d"
    a9.moveGrab(0,0,3);                     // d to the end
    ck(names()=="bcad","d ist ans Ende gewandert");
    ck(d9.selected[3]==1 && d9.selected[0]==0,"die Auswahl ist mitgewandert");

    // Delete several at once; the indices must not shift.
    ck(a9.deleteGrabs(0,{0,2})==2,"zwei geloescht");
    ck(names()=="cd","die richtigen beiden sind weg: b und a");
    ck(a9.documents()[0].selected.size()==2,"Auswahlfeld bleibt gleich lang");
    ck(a9.documents()[0].dirty,"als geaendert markiert");

    // New script: must be valid and readable again.
    const std::string neu = (r9/"neu.car").string();
    ck(a9.newCar(neu),"neues Skript angelegt");
    ck(fs::exists(neu),"Datei liegt auf der Platte");
    ck(a9.documents().size()==2,"als Tab geoeffnet");
    ck(a9.documents()[1].script.convert.has_value(),
       "enthaelt die Konvertierungsanweisung");

    // A new script must be able to ACCEPT grabs. Without the init/finalize
    // frame writeScript doesn't write them, and the file would be silently empty.
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

  // Block moves and the ROOT rule.
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

    // Two non-adjacent rows as a block to the second slot.
    ck(aA.moveGrabs(0,{0,4},1)==2,"zwei Zeilen als Block verschoben");
    // a (row 0) and e (row 4) before row 1: both move to the front and
    // end up next to each other.
    ck(seq()=="aebcdr","Block sitzt zusammen und in Ausgangsreihenfolge");

    // Crucial: root stays at the end, no matter what gets moved.
    ck(aA.documents()[0].script.grabs.back().file=="root.xsi","root ist weiterhin letzter");
    ck(aA.moveGrabs(0,{5},0)>0,"root nach vorn geschoben");
    ck(aA.documents()[0].script.grabs.back().file=="root.xsi",
       "root wandert von selbst wieder ans Ende");

    // Newly added animations land BEFORE root.
    makeXsi(rA/"neu.xsi");
    aA.addXsiFiles({(rA/"neu.xsi").string()}, false);
    ck(aA.documents()[0].script.grabs.back().file=="root.xsi",
       "auch nach dem Hinzufuegen steht root zuletzt");
    const auto& gs = aA.documents()[0].script.grabs;
    // Without an asset root set, the path is stored as absolute.
    ck(fs::path(gs[gs.size()-2].file).filename()=="neu.xsi",
       "die neue Animation steht direkt davor");

    fs::remove_all(rA);
  }

  // openPath works out from the content what to do. It is the same path for
  // dropping onto the window, onto the exe, and for startup arguments.
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

  // Second mode: GLA -> XSI.
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
      // The uploaded GLA sits next to animation.cfg, so the automatic lookup
      // already kicks in here - exactly as intended.
      ck(aC.extract().seqs.size()>1,"Begleitdatei sofort mitgeladen");
      ck(aC.extract().origin.has_value(),"Versatz aus der GLA geschaetzt");

      // Companion files are looked up next to the GLA itself.
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

        // Without companion files nothing may be guessed.
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
        // Don't expect a fixed number: which animation.cfg sits next to it
        // depends on the user. JKA has 1683 sequences, JK2 only 931.
        // No fixed number: the user determines which animation.cfg sits next
        // to it. JKA has 1683, JK2 989, a cutscene humanoid 26.
        ck(aC.extract().seqs.size()>5,"Sequenzen gelesen");
        ck(aC.extract().selected.size()==aC.extract().seqs.size(),
           "Auswahlfeld passt zur Sequenzzahl");

        // All sequences must lie INSIDE the GLA. A line from the cfg that
        // extends past it could not be exported - showing it in the list
        // would mean treating it as usable.
        bool inRange = true;
        for (const auto& q : aC.extract().seqs)
          if (q.start < 0 || q.start + q.count > aC.extract().gla.numFrames) inRange = false;
        ck(inRange,"alle gezeigten Sequenzen liegen in der GLA");

        // Really export and convert back.
        const fs::path od = fs::temp_directory_path()/"g2c_extract";
        fs::remove_all(od);
        // Only take full sequences. A slice of a longer animation carries
        // some leftover root motion that moves into the .frames on rebuild -
        // that is correct behavior and would wrongly count as an error here.
        // Walk through every sequence, not every 97th.
        //
        // A fixed stride fits JKA's 1683 sequences but, for a cutscene
        // humanoid with 26, skips everything except the first - the test
        // then found nothing and reported an error that didn't exist.
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

        // Only count over the folder if it was actually created.
        //
        // Without the check the iterator throws, and a throwing test takes
        // the whole suite down with it - instead of reporting one line.
        std::error_code lec;
        size_t files=0;
        if (fs::is_directory(od, lec))
          for (const auto& e : fs::directory_iterator(od, lec)) { (void)e; files++; }
        ck(files==rows.size(),"je eine Datei je Sequenz");

        // The actual proof: reading it back must give the same result.
        double worst = 0;
        for (size_t k=0;k<rows.size();++k) {
          const auto& q = aC.extract().seqs[rows[k]];
          std::string low = q.name;
          std::transform(low.begin(), low.end(), low.begin(),
                         [](unsigned char ch){ return (char)std::tolower(ch); });
          const auto doc = g2::xsi::parseFile((od/(low+".xsi")).string());
          const auto an = g2::xsi::loadAnimation(doc, "x");
          // Evaluate WITH the same offset that was used for export.
          // The export puts it back in so the file looks like one from the
          // artist; when building, the .car subtracts it again.
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
        // One quantization step is 0.015625. On top of that comes what the
        // SRT model of an .xsi cannot represent: shear from bind poses with
        // unequal axis lengths. With flat skeletons and a long lever some of
        // it remains - 0.1 is the threshold above which I would consider it
        // an error.
        ck(worst < 0.1, "Rundlauf GLA -> XSI -> GLA im erwarteten Rahmen");
        fs::remove_all(od);
      }
    }
  }

  // Compare two humanoids: which sequences are missing on the other side?
  {
    const fs::path cr = fs::temp_directory_path()/"g2c_compare";
    fs::remove_all(cr); fs::create_directories(cr);

    // Build a small GLA with three sequences.
    g2::Skeleton cs; cs.name="c"; cs.scale=0.64f;
    { g2::Bone b; b.name="model_root"; b.basePose=g2::Mat3x4::identity(); cs.bones.push_back(b); }
    g2::AnimationFrames cf; cf.numBones=1; cf.matrices.assign(30, g2::Mat3x4::identity());
    { std::ofstream o(cr/"_humanoid.gla", std::ios::binary);
      const auto w=g2::writeMdxa(cs,cf);
      o.write((const char*)w.data.data(), (std::streamsize)w.data.size()); }
    { std::ofstream o(cr/"animation.cfg");
      o<<"BOTH_STAND1 0 10 -1 20\nBOTH_WALK1 10 10 0 20\nBOTH_SPEZIAL 20 10 -1 20\n"; }
    // The other side knows only two of them, one with different spelling.
    { std::ofstream o(cr/"andere.cfg");
      o<<"both_stand1 0 5 -1 20\nBOTH_WALK1 5 5 0 20\nBOTH_NURDORT 10 5 -1 20\n"; }

    g2::gui::Platform pF;
    g2::gui::App aF(std::move(pF));
    ck(aF.loadGlaForExtract((cr/"_humanoid.gla").string()),"GLA geladen");
    ck(aF.extract().seqs.size()==3,"drei Sequenzen");

    ck(aF.loadCompareCfg((cr/"andere.cfg").string()),"Vergleich geladen");
    ck(aF.extract().compareNames.size()==3,"drei Namen drueben");

    // Case must not matter: "both_stand1" over there is the same sequence
    // as "BOTH_STAND1" here.
    ck(aF.existsInCompare("BOTH_STAND1"),"Schreibweise wird ignoriert");
    ck(aF.existsInCompare("BOTH_WALK1"),"exakter Treffer");
    ck(!aF.existsInCompare("BOTH_SPEZIAL"),"fehlt drueben");

    const std::size_t n = aF.selectMissing();
    ck(n==1,"genau eine fehlt");
    std::size_t sel=0; for (char c : aF.extract().selected) if (c) sel++;
    ck(sel==1,"genau eine ausgewaehlt");
    // And the right one at that.
    for (size_t i=0;i<aF.extract().seqs.size();++i)
      if (aF.extract().selected[i])
        ck(aF.extract().seqs[i].name=="BOTH_SPEZIAL","die richtige ausgewaehlt");

    // An unreadable file must not clear the comparison.
    ck(!aF.loadCompareCfg((cr/"gibtsnicht.cfg").string()),"fehlende Datei abgewiesen");
    ck(aF.extract().compareNames.size()==3,"vorheriger Vergleich bleibt erhalten");

    fs::remove_all(cr);
  }

  // Copy and paste, also between two scripts.
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

    // Paste into the OTHER script - that is the actual purpose.
    ck(aP.pasteGrabs(B,0)==2,"in das andere Skript eingefuegt");
    ck(names(B)=="xzq","davor eingefuegt, Reihenfolge erhalten");
    ck(aP.documents()[B].dirty,"als geaendert markiert");

    // The grab's settings must come along, not just the name.
    const auto& g0 = aP.documents()[B].script.grabs[0];
    ck(g0.loop && *g0.loop==3,"Loopframe mitkopiert");
    ck(g0.frameSpeed && *g0.frameSpeed==40,"Framespeed mitkopiert");

    // The source stays untouched.
    ck(names(A)=="xyz","Kopieren aendert die Quelle nicht");

    // Cutting removes them there.
    ck(aP.cutGrabs(A,{1})==1,"eine ausgeschnitten");
    ck(names(A)=="xz","aus der Quelle entfernt");
    ck(aP.pasteGrabs(B,3)==1,"ans Ende eingefuegt");
    ck(names(B)=="xzqy","am Ende gelandet");

    // The clipboard stays valid after pasting - pasting multiple times
    // must work.
    ck(aP.pasteGrabs(B,0)==1,"nochmals eingefuegt");
    ck(names(B)=="yxzqy","zweimal eingefuegt");

    // Nonsensical targets must not crash.
    ck(aP.pasteGrabs(B,9999)==1,"Position hinter dem Ende wird geklemmt");
    ck(aP.pasteGrabs(99,0)==0,"unbekanntes Skript liefert nichts");
    ck(aP.copyGrabs(A,{})==0,"leere Auswahl kopiert nichts");

    fs::remove_all(rP);
  }

  // Preview: camera, projection, playback.
  {
    using namespace g2::gui;

    // A skeleton with known positions.
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

    // The look-at point must land in the center of the image.
    float x=0,y=0,d=0;
    ck(projectPoint(cam.target, cam, 800.0f, 600.0f, x, y, d),"Blickpunkt sichtbar");
    ck(std::fabs(x-400.0f)<0.5f,"waagerecht mittig");
    ck(std::fabs(y-300.0f)<0.5f,"senkrecht mittig");
    ck(std::fabs(d-cam.distance)<0.5f,"Tiefe entspricht dem Abstand");

    // A point BEHIND the camera must not be drawn - otherwise the line
    // appears mirrored on the wrong side.
    std::array<float,3> hinten = cam.target;
    hinten[0] += cam.distance * 4.0f;
    hinten[1] += cam.distance * 4.0f;
    const bool sichtbar = projectPoint(hinten, cam, 800.0f, 600.0f, x, y, d);
    ck(!sichtbar || d > 0.0f, "hinter der Kamera wird abgewiesen oder hat positive Tiefe");

    // A point above the look-at point must land HIGHER UP, i.e. with a
    // smaller y. Screen coordinates run downward.
    PreviewCamera side; side.yawDeg=0; side.pitchDeg=0; side.distance=100;
    side.target = {{0,0,0}};
    float xm=0,ym=0,dm=0, xo=0,yo=0,dо=0;
    projectPoint({{0,0,0}}, side, 800, 600, xm, ym, dm);
    projectPoint({{0,0,20}}, side, 800, 600, xo, yo, dо);
    ck(yo < ym, "hoeher liegender Punkt erscheint weiter oben");

    // Play back at the sequence's rate.
    Playback pb; pb.playing = true;
    advancePlayback(pb, 0.5f, 20, 30);      // 0.5 s at 20 frames/s = 10
    ck(pb.frame==10,"zehn Frames weiter");
    advancePlayback(pb, 1.5f, 20, 30);      // another 30 -> wraps around
    ck(pb.frame==10,"laeuft um");

    // A negative rate means backwards - that is how Raven's cfg has it.
    Playback rb; rb.playing = true; rb.frame = 20;
    advancePlayback(rb, 0.5f, -20, 30);
    ck(rb.frame==10,"negative Rate laeuft rueckwaerts");

    // When paused, nothing moves.
    Playback sb; sb.playing = false; sb.frame = 5;
    advancePlayback(sb, 10.0f, 20, 30);
    ck(sb.frame==5,"angehalten bleibt stehen");

    // A sequence with a single frame must not run into an endless loop.
    Playback one; one.playing = true;
    advancePlayback(one, 10.0f, 20, 1);
    ck(one.frame==0,"ein Frame bleibt bei null");
  }

  // Save before building.
  //
  // Previously the in-memory state was built while the file on disk stayed
  // the old one. Anyone who closed the program afterwards had a GLA that no
  // longer matched any .car.
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

    // The disk still holds the old state.
    {
      const auto vorher = g2::car::parseFile((sd/"_humanoid.car").string());
      ck(vorher.grabs.size()==1,"Datei traegt noch den alten Stand");
    }

    // Saving must store the new state.
    ck(aS.saveDocument(0),"gespeichert");
    {
      const auto nachher = g2::car::parseFile((sd/"_humanoid.car").string());
      ck(nachher.grabs.size()==2,"Datei traegt jetzt den neuen Stand");
    }
    ck(!aS.documents()[0].dirty,"nicht mehr als geaendert markiert");

    fs::remove_all(sd);
  }

  // Log as text: what ends up in the bug report.
  {
    g2::gui::Platform pL;
    g2::gui::App aL(std::move(pL));
    const std::string t = aL.logAsText();

    // Header with the things people would otherwise have to ask about.
    ck(t.find("g2c - Protokoll")!=std::string::npos,"Kopfzeile vorhanden");
    ck(t.find("Gebaut:")!=std::string::npos,"Baudatum vorhanden");
    ck(t.find("Laufzeit:")!=std::string::npos,"Laufzeitangabe vorhanden");

    // The kind must be in the text: the color is lost when copying, and it
    // is precisely what distinguishes a notice from an error.
    aL.openCar("/gibt/es/nicht.car");
    const std::string t2 = aL.logAsText();
    ck(t2.find("[")!=std::string::npos,"Art wird vorangestellt");

    // And more lines than before.
    ck(t2.size()>t.size(),"Meldung ist enthalten");
  }

  // Bind pose choice: takes effect and is retained.
  {
    g2::gui::Platform pB;
    g2::gui::App aB(std::move(pB));
    ck(aB.extract().basePose==g2::xsiexp::ExportOptions::BasePose::World,
       "Vorgabe ist die Weltpose");
  }

  // Add several folders at once.
  {
    const fs::path mr = fs::temp_directory_path()/"g2c_multifolder";
    fs::remove_all(mr); fs::create_directories(mr);
    { std::ofstream f(mr/"_humanoid.car");
      f<<"$aseanimgrabinit\n$aseanimgrab a.xsi -enum BOTH_STAND1\n$aseanimgrabfinalize\n"; }

    // Three source folders with two animations each.
    for (const char* d : {"quelle1","quelle2","quelle3"}) {
      fs::create_directories(mr/d);
      makeXsi(mr/d/"eins.xsi");
      makeXsi(mr/d/"zwei.xsi");
    }

    g2::gui::Platform pM;
    // The platform reports three folders at once.
    pM.pickFolders = [&](const char*, const std::string&) {
      return std::vector<std::string>{(mr/"quelle1").string(), (mr/"quelle2").string(),
                                      (mr/"quelle3").string()};
    };
    g2::gui::App aM(std::move(pM));
    aM.openCar((mr/"_humanoid.car").string());
    ck(aM.documents()[0].script.grabs.size()==1,"ein Grab am Anfang");

    const auto dirs = aM.askFolders("test","");
    ck(dirs.size()==3,"drei Ordner geliefert");
    // addXsiFolder returns the number of SCRIPTS touched, not of files -
    // so with one open script, 1 per folder.
    for (const auto& d : dirs) aM.addXsiFolder(d, false);
    ck(aM.documents()[0].script.grabs.size()==7,
       "sechs Dateien aus drei Ordnern angehaengt");

    // Fallback: if the platform can only do one, one comes back - never nothing.
    g2::gui::Platform pS;
    pS.pickFolder = [&](const char*, const std::string&) {
      return (mr/"quelle1").string();
    };
    g2::gui::App aS(std::move(pS));
    const auto einer = aS.askFolders("test","");
    ck(einer.size()==1,"Rueckfall auf Einzelauswahl");

    // And without any selection nothing may happen.
    g2::gui::Platform pN;
    g2::gui::App aN(std::move(pN));
    ck(aN.askFolders("test","").empty(),"ohne Dialog leere Liste");

    fs::remove_all(mr);
  }

  // Separator lines and comments: the whole way through.
  //
  // Set in the UI -> written to the .car -> read back in -> in the
  // animation.cfg. Checking each stage on its own is not enough: the value
  // has to make it through all four.
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

    // The way the button does it.
    aT.documents()[0].script.grabs[1].commentsBefore.push_back(
        "//////////////////////////////////////////");
    aT.documents()[0].script.grabs[1].commentsBefore.push_back("//  LAUFANIMATIONEN");
    ck(aT.saveDocument(0),"gespeichert");

    // Is it really in the file?
    {
      std::ifstream in(kd/"_humanoid.car");
      std::string all((std::istreambuf_iterator<char>(in)), {});
      ck(all.find("LAUFANIMATIONEN")!=std::string::npos,"Kommentar in der .car");
      ck(all.find("LAUFANIMATIONEN")<all.find("b.xsi"),"steht VOR seiner Sequenz");
    }

    // And when reading it back in?
    const auto wieder = g2::car::parseFile((kd/"_humanoid.car").string());
    ck(wieder.grabs.size()==2,"wieder zwei Grabs");
    if (wieder.grabs.size()==2) {
      ck(wieder.grabs[0].commentsBefore.empty(),"erster ohne Kommentar");
      ck(wieder.grabs[1].commentsBefore.size()==2,"zweiter mit zwei Zeilen");
    }

    // All the way into the animation.cfg.
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

    // Empty comment lines must not go into the cfg.
    //
    // Creating one via the context menu first produces an empty line. If it
    // stays empty it is removed again during editing - but if one does slip
    // through, it must not end up in the file as a bare "//".
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
      // The empty one becomes a blank line, not "//"
      ck(cfg2.find("//\r\n//\r\n// unten")==std::string::npos,
         "leere Zeile wird nicht zu //");
    }

    fs::remove_all(kd);
  }

  // Moving a separator: it belongs to no sequence.
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

    // Upward: attaches itself to the previous grab.
    ck(aV.moveComment(0,1,0,true),"nach oben verschoben");
    ck(sc.grabs[1].commentsBefore.empty(),"bei B weg");
    ck(sc.grabs[0].commentsBefore.size()==1,"bei A angekommen");

    // And back again.
    ck(aV.moveComment(0,0,0,false),"nach unten verschoben");
    ck(sc.grabs[1].commentsBefore.size()==1,"wieder bei B");

    // All the way down - past the last animation.
    ck(aV.moveComment(0,1,0,false),"weiter nach unten");
    ck(sc.grabs[2].commentsBefore.size()==1,"bei C");
    ck(aV.moveComment(0,2,0,false),"ans Ende");
    ck(sc.trailingComments.size()==1,"hinter der letzten Animation");
    ck(sc.grabs[2].commentsBefore.empty(),"bei C weg");

    // Going past the edge is not possible.
    ck(!aV.moveComment(0,2,0,false),"am Ende kein weiteres Verschieben");

    // And the whole thing has to survive the .car.
    ck(aV.saveDocument(0),"gespeichert");
    const auto neu = g2::car::parseFile((vd/"_humanoid.car").string());
    ck(neu.trailingComments.size()==1,"Schlusskommentar wieder eingelesen");

    fs::remove_all(vd);
  }

  // Line comment: .car -> display -> .car -> animation.cfg
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

    // The comment must not interfere with parsing.
    ck(sc.grabs[0].enumName && *sc.grabs[0].enumName=="BOTH_WALK1_ANI",
       "enum trotz Kommentar erkannt");

    // Save and read back.
    sc.grabs[1].trailingComment = "// Basispose";
    ck(aZ.saveDocument(0),"gespeichert");
    const auto neu = g2::car::parseFile((td/"_humanoid.car").string());
    ck(neu.grabs.size()==2,"wieder zwei Grabs");
    if (neu.grabs.size()==2) {
      ck(neu.grabs[0].trailingComment.find("Anakins")!=std::string::npos,"erster erhalten");
      ck(neu.grabs[1].trailingComment.find("Basispose")!=std::string::npos,"zweiter erhalten");
    }

    // And in the animation.cfg after the numbers.
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
    // There must be no line break between the two.
    ck(cfg.find('\n', z) > k, "in derselben Zeile");

    fs::remove_all(td);
  }

  // Move a separator to an arbitrary position (what dragging triggers).
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

    // From A straight to C - dragging skips intermediate stops.
    ck(aD.moveComment(0,0,0,false),"eine Stufe");
    ck(sc.grabs[1].commentsBefore.size()==1,"bei B");
    ck(aD.moveComment(0,1,0,false),"noch eine");
    ck(sc.grabs[2].commentsBefore.size()==1,"bei C");

    // Past the last animation and back.
    ck(aD.moveComment(0,2,0,false),"ans Ende");
    ck(sc.trailingComments.size()==1,"hinter der letzten");
    ck(sc.grabs[2].commentsBefore.empty(),"bei C weg");

    // The text must stay unchanged along the way.
    ck(sc.trailingComments[0]=="// TRENNER","Text unveraendert");

    fs::remove_all(dd);
  }

  testUpdater();
  testReviewGui();

  ck(!app.buildRunning(),"kein Bau aktiv");
  ck(app.logLines().size()>0,"Protokoll gefuellt");

  fs::remove_all(root);
  printf(fails? "  %d Fehler\n":"  Oberflaechenlogik in Ordnung\n", fails);
  return fails?1:0;
}
