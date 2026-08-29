# Anleitung: eigenen Carcass-Ersatz bauen

Stand nach Verifikation gegen echte Raven-Assets (`_humanoid.gla`,
`_humanoid.glm`, `root.xsi`, `_humanoid.car`).

---

## 1. Bauen

### Schnellstart

```bat
build.bat            :: Windows, Release, mit Tests
build.bat debug      :: Debug-Build
build.bat clean      :: vorher aufraeumen
build.bat notest     :: ohne Testlauf
```

```sh
./build.sh           # Linux / macOS, gleiche Argumente
```

Die Skripte finden Visual Studio 2022 oder 2026 automatisch und fallen auf
MinGW zurueck, falls kein VS installiert ist. Ein Max SDK wird nicht
gebraucht. Ergebnis landet in `output\g2c.exe`.

### Voraussetzungen

- CMake ab 3.16
- Ein C++20-Compiler: GCC 11+, Clang 14+ oder MSVC 2022
- Sonst nichts. Keine externen Bibliotheken.

### Linux / macOS

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/g2_tests
```

### Wichtig: Ordnerstruktur

Das Projekt braucht seine Unterordner. Einzeln heruntergeladene Dateien
landen flach nebeneinander und lassen sich so nicht bauen — dann meldet
CMake, es finde keine `CMakeLists.txt`. Also `g2c-toolkit.zip` entpacken.

Der Zielordner darf frei gewaehlt werden, sollte aber **keine runden
Klammern** enthalten. Ein Pfad wie `Downloads\files(1)` bricht Batch-Skripte,
weil die Klammer Blöcke vorzeitig schliesst. `C:\dev\g2c-toolkit` ist
unproblematisch.

Bei Problemen: siehe `MANUELL.md`.

### Windows, Visual Studio 2022

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\g2_tests.exe
```

### Windows, MinGW

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\g2_tests.exe
```

Erwartete Ausgabe am Ende: `268/268 Pruefungen bestanden`.

Es entstehen `libg2` (statische Bibliothek), `g2c` (Kommandozeilenwerkzeug)
und `g2_tests`.

---

## 2. Variante 2: Animationen gegen ein vorhandenes Skelett

Der Weg, der hier umgesetzt ist. Das Skelett kommt vollstaendig aus einer
Referenz-GLA — Bonenamen, Reihenfolge, Hierarchie und Basisposen bleiben
unveraendert. Nur die Frames werden neu gebaut.

```sh
g2c anim base/_humanoid.gla neu/_humanoid.gla \
    -origin 0 0 24 \
    anims/BOTH_attack10.xsi anims/BOTH_attack11.xsi
```

Das ist fuer den Anwendungsfall "Animationen hinzufuegen" nicht nur bequemer,
sondern richtiger: jede Aenderung am Skelett wuerde alle vorhandenen
GLM-Modelle brechen.

**Gegenprobe an echten Daten.** `BOTH_attack10.xsi` gegen `_humanoid.gla`
gebaut und mit den Originalframes ab 1541 verglichen:

| | |
|---|---|
| max. Rotationsabweichung | 2,3·10⁻⁴ |
| Quantisierungsschrittweite | 6,10·10⁻⁵ |
| Bones innerhalb einer Stufe | 43 von 53 |

Die verbleibenden Abweichungen betreffen ausschliesslich die Gesichtsknochen,
und zwar nur deren Translation — die Rotation stimmt dort auf 0,00000. Ursache
ist der bekannte Versionsunterschied `face` gegen `face_always_` in der
Basispose, nicht die Formel.

### Wichtige Details

**Nicht animierte Bones bekommen die Einheitsmatrix**, nicht ihre Weltruhepose.
Der Unterschied ist entscheidend: mit der Weltruhepose bliebe ein Bone im Raum
stehen, statt seinem Elternbone zu folgen. In der echten `_humanoid.gla` steht
beim nicht animierten Bone `face` exakt die Einheitsmatrix.

**`-origin`** muss mit dem Wert aus der urspruenglichen `.car` uebereinstimmen.
Bei Ravens `_humanoid` ist das `0 0 24`; in der GLA landet er als `(0,0,-24)`
auf `model_root`.

**`-alias gla=xsi`** fuer Umbenennungen zwischen Builds, etwa
`-alias face=face_always_`.

Der `$scale` wird automatisch aus der Referenz-GLA uebernommen.

## 3. Ein ganzes .car-Skript abarbeiten

```sh
g2c build _humanoid.car -ref base/_humanoid.gla -basedir C:\pfad\zu\assets
```

`-basedir` zeigt auf das Verzeichnis, unter dem `models/` liegt — dorthin
zeigen die Pfade aus `$aseanimgrab`. `-ref` liefert das Skelett.

Abgearbeitet wird, was Carcass auch tut: alle `$aseanimgrab` der Reihe nach
laden, aneinanderhaengen, komprimieren, dann **GLA, `animation.cfg`,
`.frames`, GLM und `.skin`** schreiben — fuenf Dateien in einem Lauf.

Die Mesh-Quelle steht im Skript: `$aseanimconvertmdx <root>` ohne Endung. Die
GLM traegt den Namen des Verzeichnisses, in dem diese Datei liegt — wie bei
Carcass. Mit `-nomesh` bleibt es bei der GLA.
Dabei werden `-loop`, `-framespeed`, `-enum` und `-additional` ausgewertet und
`-origin` aus `$aseanimconvertmdx` uebernommen.

Ohne `-o` richtet sich der Ausgabename nach `-makeskel` im Skript.

### Fehlende Dateien

Standardmaessig ist eine fehlende `.xsi` ein harter Abbruch, mit Auflistung.
Das ist Absicht: wuerde man sie stillschweigend ueberspringen, verschoeben sich
**alle nachfolgenden Zielframes**, und die `animation.cfg` passte nicht mehr zur
GLA — ein Fehler, der erst im Spiel auffaellt und dann schwer zu finden ist.

Mit `-skipmissing` laesst sich das erzwingen, aber dann gilt genau das oben.

### Weitere Optionen

| Option | Wirkung |
|---|---|
| `-o <datei.gla>` | Ausgabename |
| `-threads N` | Threadzahl, Vorgabe: alle Kerne |
| `-carcass` | wie Carcass quantisieren (kleiner, ungenauer) |
| `-origin x y z` | ueberschreibt das Skript |
| `-framespeed N` | Vorgabe fuer Zeilen ohne `-framespeed` |
| `-skipmissing` | fehlende `.xsi` ueberspringen |
| `-nomesh` | nur GLA, keine GLM |

### Gegenprobe

Mit einer Test-`.car` aus drei echten Animationsdateien gebaut und die Sequenz
`BOTH_ATTACK10` gegen die Originalframes ab 1541 verglichen: max.
Rotationsabweichung 2,3·10⁻⁴ bei einer Quantisierungsstufe von 6,10·10⁻⁵.
Die erzeugte `animation.cfg`:

```
BOTH_ATTACK10       	0	20	-1	10
TORSO_HANDSIGNAL2   	20	73	-1	20
TORSO_EXTRA_A       	25	10	-1	20
FACE_TALK2          	93	2	-1	5
```

Zielframes fortlaufend, `-additional` korrekt auf den Sequenzanfang addiert
(20 + 5 = 25), Framespeeds aus den Flags.

### Regionseinstellungen

Das alte Carcass lief nur mit amerikanischen Regionseinstellungen. Ursache ist
fast immer `strtod`/`atof`/`std::stof`: die haengen am C-Locale `LC_NUMERIC`,
und auf einem deutschen System wird aus `"0.64"` dann eine 0 — lautlos, ohne
Fehlermeldung.

`g2c` ist davon unabhaengig:

- Alle Zahlen aus `.xsi` und `.car` gehen ueber `std::from_chars`. Das ist per
  Norm locale-unabhaengig und akzeptiert ausschliesslich den Punkt.
- Auch die Kommandozeilenargumente (`-origin`, `-threads`, `-framespeed`)
  werden so gelesen, nicht mit `std::stof`.
- `main` setzt zusaetzlich `std::locale::global(std::locale::classic())`,
  damit die Ausgabe nie Zahlen mit Dezimalkomma schreibt.

Abgesichert durch einen Test, der ein Locale mit Dezimalkomma installiert und
danach `.xsi`- und `.car`-Werte einliest. Der braucht kein Systemlocale und
laeuft deshalb ueberall.

### Zwischenspeicher

Beim ersten Bau wird jede `.xsi` gelesen und geparst — das sind 96 % der
Laufzeit. Das Ergebnis landet in `g2c_cache\` neben der `.car` und wird
beim naechsten Mal wiederverwendet.

Carcass macht dasselbe: im Log des Originals steht
`( Reading 249.63MB CARPET file )`. Die zehn Sekunden, in denen es angeblich
1289 Dateien verarbeitet, sind in Wahrheit das Einlesen dieses Caches.

Gemessen an 50 Dateien, 44 MB Quellen, ein Kern:

| | |
|---|---|
| erster Lauf | 981 ms |
| jeder weitere | 345 ms |
| Cache auf Platte | 11,2 MB |

Die Ausgabe ist mit und ohne Cache **bitgleich**.

| Option | Wirkung |
|---|---|
| `-cache <ordner>` | anderer Ort |
| `-nocache` | ohne Zwischenspeicher |
| `-clearcache` | vorher leeren |

Ein Eintrag verfaellt, sobald sich **Groesse oder Aenderungszeit** der Quelle
aendern — anfassen genuegt, manuelles Leeren ist nicht noetig. Zusaetzlich
steht eine Versionsnummer im Kopf jedes Eintrags: aendert sich das
Einleseverhalten, werden alte Eintraege verworfen. Ohne das lieferte der
Cache Daten nach altem Verstaendnis, und ein Fix saehe aus, als wirke er
nicht — ein Fehler, der sehr schwer zu finden ist.

Anders als CARPET liegt **jede Quelle in einer eigenen Datei**. Ein
beschaedigter Eintrag kostet damit eine Datei statt den ganzen Cache, und
parallele Laeufe kommen sich nicht in die Quere. Geschrieben wird ueber eine
Nebendatei mit anschliessendem Umbenennen, damit ein Abbruch keinen halben
Eintrag hinterlaesst.

### Laufzeit und Threads

Ein Profil ueber Lesen, Parsen, FCurves und Auswertung:

| Phase | Anteil |
|---|---|
| **Datei lesen** | **80 %** |
| dotXSI parsen | 12 % |
| FCurves einsammeln | 4 % |
| Frames auswerten | 4 % |

Das Lesen dominiert deutlich, und genau das laesst sich gut verteilen: jede
`.xsi` wird unabhaengig gelesen, geparst und ausgewertet. Zusammengehaengt wird
danach wieder in Skriptreihenfolge, damit die Zielframes stimmen.

Parallelisiert sind zwei Stellen:

1. **Laden und Auswerten** der Animationsdateien
2. **Kompression** der Bone-Matrizen — bei 1,6 Millionen Instanzen mit
   Kandidatensuche rund 3,2 Sekunden, ohne 0,6

Der **Poolaufbau bleibt seriell**. Er ist mit rund einer halben Sekunde nicht
der Engpass, und die Reihenfolge der Pool-Eintraege bestimmt die Datei — eine
parallele Variante waere nicht mehr reproduzierbar.

Die Ausgabe ist deshalb **bitgleich, unabhaengig von der Threadzahl**. Mit
1, 2, 4 und 8 Threads gebaut ergibt dieselbe Pruefsumme.

Verwendet wird `std::thread`. Die Parallel-Algorithmen der Standardbibliothek
(`std::execution::par`) kamen nicht in Frage: sie verlangen auf GCC und Clang
zwingend Intel TBB und laufen nur unter MSVC ohne Zusatzbibliothek. Fuer ein
Werkzeug, das mit nichts als einem C++20-Compiler bauen soll, ist das ein zu
hoher Preis.

**64 Bit** ist bereits der Normalfall: `build.bat` uebergibt `-A x64` an CMake.

## 4. Zwei GLA vergleichen

```sh
g2c diff alt.gla neu.gla -cfg animation.cfg
```

Geht ueber **jede** Bone-Instanz, nicht ueber eine Stichprobe, und ordnet die
Abweichungen Bones und Sequenzen zu. Bei 1,6 Millionen Instanzen dauert das
wenige Sekunden.

Warum vollstaendig: eine Abweichung, die genau eine Sequenz betrifft, geht in
800 zufaelligen Frames unter. Genau solche Faelle waren in diesem Projekt die
interessanten — die fehlende Wurzelbewegung betraf 7 % der Frames und war in
Stichproben kaum von Rauschen zu unterscheiden.

### Toleranzen

| | Vorgabe |
|---|---|
| Rotation | 0,1 Grad |
| Translation | 2 Quantisierungsstufen (0,03125) |

Die Rotation wird als **echter Winkel** gemessen, nicht als Differenz
einzelner Matrixelemente. Letzteres ist nicht zu deuten: ein halber
Quantisierungsschritt je Quaternionkomponente pflanzt sich ueber die Produkte
in der Matrix zu einem Vielfachen fort. Mit Elementdifferenzen erschienen 73 %
aller Bones auffaellig, mit Winkeln sind es 1,1 % — und das sind die echten.

`-tol M` skaliert beide Toleranzen, `-all` zeigt alle Treffer statt der
ersten zwoelf, `-offset N` verschiebt die zweite Datei (nuetzlich, um eine
einzelne Sequenz gegen eine vollstaendige GLA zu pruefen).

Rueckgabewert 0 bei sauberem Vergleich, 2 bei Abweichungen — damit laesst es
sich in ein Buildskript haengen.

## 5. GLM bauen

```sh
g2c mesh root.xsi -ref base/_humanoid.gla -o model.glm
g2c mesh root.xsi -ref base/_humanoid.gla -compare original.glm   # gegenpruefen
```

Skelett, Bonenamen und `$scale` kommen aus der Referenz-GLA.

Vollstaendig korrekt: **Surfaces, Dreiecke, Tag-Erkennung, Flags, Hierarchie
und Shaderzuordnung**. Die Namensregeln stammen aus Ravens Datei:

| dotXSI | GLM | Flag |
|---|---|---|
| `Stupidtriangle_off` | `stupidtriangle_off` | 2 (OFF) |
| `bolt_back` | `*back` | 1 (Tag) |
| `hips` | `hips` | 0 |

Bei der **Vertexzahl** stimmen 81 von 84 Surfaces exakt; insgesamt 2673 gegen
2647. Offen sind `hips` (+4) und die beiden Haende (je +11). Praktisch
bedeutet das ein Prozent zusaetzliche Vertices an denselben Positionen mit
leicht abweichenden Normalen — sichtbar ist das nicht, es kostet etwas
Speicher.

`-normaltol` und `-uvtol` stellen die Toleranzen. Die Vorgaben 0,05 und 0,002
sind an allen 84 Surfaces gemessen die besten. **Nicht** auf die Gesamtsumme
optimiert: mit 0,15 traefe sie exakt, aber nur weil sich Fehler aufheben.

## 6. Was das Werkzeug jetzt schon kann

```sh
g2c info   <datei.gla>   Header, Skeletthierarchie, Poolstatistik
g2c check  <datei.gla>   Struktur pruefen, Quantisierungsfehler messen
g2c xsi    <datei.xsi>   dotXSI parsen, Templatetypen und Tempo
g2c car    <datei.car>   Carcass-Skript aufloesen  (-v = alle Anweisungen)
g2c import <root.xsi> [referenz.gla] [-o name]
                         dotXSI importieren, optional GLA/GLM schreiben
```

Der Import an der echten Quelle:

```
$ g2c import root.xsi _humanoid.gla -o test
dotXSI 3.50, 3444 Templates
Referenz: _humanoid.gla (53 Bones, Scale 0.64)

Skelett : 53 Bones
Mesh    : 84 Surfaces, 2425 Vertices, 2846 Dreiecke

Basisposen gegen Referenz: max. Abweichung 0.0000001 bei r_d4_j1

Geschrieben: test.gla (9810 Bytes), test.glm (148444 Bytes)
```

Die Referenz-GLA ist optional, aber empfohlen: aus ihr werden `$scale`,
Bonemenge, Reihenfolge und Hierarchie uebernommen. Ohne sie waehlt der
Importer die Bones selbst (siehe unten) und kommt bei `_humanoid` auf 65
statt 53.

Am echten `_humanoid` erprobt:

```
$ g2c info _humanoid.gla
GLA-Name : models/players/_humanoid/_humanoid
Scale    : 0.64
Frames   : 21554
Bones    : 53
Pool     : 497440 Eintraege
Dedupe   : 644922 von 1142362 Bone-Instanzen eingespart (56.5%)
```

---

## 7. Dateiübersicht

| Datei | Inhalt |
|---|---|
| `include/g2/format.h` | On-Disk-Strukturen GLA/GLM, alle Konstanten |
| `include/g2/compress.h`, `src/compress.cpp` | Quantisierung, Matrix↔Quaternion |
| `include/g2/model.h` | Zwischenrepräsentation (Skeleton, Mesh, LOD) |
| `include/g2/mdxa.h`, `src/mdxa.cpp` | GLA schreiben und lesen |
| `include/g2/mdxm.h`, `src/mdxm.cpp` | GLM schreiben |
| `include/g2/xsi.h`, `src/xsi.cpp` | dotXSI-Parser (109 MB/s) |
| `include/g2/carscript.h`, `src/carscript.cpp` | `.car`-Parser, animation.cfg |
| `include/g2/xsi_anim.h`, `src/xsi_anim.cpp` | Animationen gegen Referenzskelett |
| `include/g2/carbuild.h`, `src/carbuild.cpp` | `.car`-Skript abarbeiten |
| `include/g2/gladiff.h`, `src/gladiff.cpp` | zwei GLA vollständig vergleichen |
| `include/g2/animcache.h`, `src/animcache.cpp` | Zwischenspeicher für geparste `.xsi` |
| `include/g2/xsi_mesh.h`, `src/xsi_mesh.cpp` | Mesh aus dotXSI lesen |
| `include/g2/sidefiles.h`, `src/sidefiles.cpp` | `.skin` und `.frames` schreiben |
| `include/g2/animenums.h`, `src/animenums.cpp` | `anims.h` einlesen |
| `include/g2/carvalidate.h`, `src/carvalidate.cpp` | Prüfen, `.car` schreiben, Verzeichnis-Scan |
| `include/g2/xsi_import.h`, `src/xsi_import.cpp` | dotXSI → Skelett, Mesh, Animation |
| `include/g2/bytebuf.h` | Little-Endian-Schreibpuffer |
| `include/g2/readfile.h` | Datei am Stück lesen |
| `include/g2/parallel.h` | Paralleler for-Loop ohne Fremdbibliothek |
| `tools/g2c.cpp` | Kommandozeilenwerkzeug |
| `tests/tests.cpp` | 268 Prüfungen |
| `docs/BUGS.md` | Carcass-Fehler, was behebbar ist |
| `docs/DOTXSI_MAPPING.md` | dotXSI-Semantik, empirisch verifiziert |
| `docs/CAR_FORMAT.md` | `.car`-Grammatik, animation.cfg |
| `examples/README.md` | Beispielaufrufe |
| `build.bat`, `build.sh` | Buildskripte |
| `MANUELL.md` | Bauen ohne Skript, Fehlerbehebung |

---

## 4. Stand des Imports

Die semantische Schicht ist implementiert (`xsi_import.h`). Was gemessen
gegen die echten Assets herauskommt:

| | Ergebnis |
|---|---|
| Basisposen, 53 Bones | max. Abweichung **1·10⁻⁷** |
| Bonenamen und Hierarchie mit Referenz | **53/53** |
| Surfaces | **84** (exakt) |
| Dreiecke | **2846** (exakt) |
| Vertices | 2425 statt 2647 |
| Animation, Bones mit Kurven | **43 von 51** unter 0,001 |

Die acht Animations-Ausreisser sind die Gesichtsbones — dieselben, die schon
bei der Basispose abweichen, weil `root.xsi` `face_always_` heisst und die
vorliegende GLA `face`. Das ist Versionsversatz im Beispielmaterial, kein
Rechenfehler.

Zwei Punkte sind offen:

**Bonenauswahl ohne Referenz.** Automatisch werden alle von `SI_Envelope`
referenzierten Bones genommen, ohne IK-Effektoren (`*_eff`), plus `Motion`
bei `$keepmotion` plus `model_root`. Das ergibt 65 statt 53. Carcass laesst
zusaetzlich zwoelf Blattbones weg (`l_d1_j3`, `ltarsal`, `ltlip1` und
Verwandte); nach welcher Regel, ist nicht ermittelt.

**Bonehierarchie.** Carcass baut sie um: in `root.xsi` haengt `ltibia` unter
`lfemurX`, in der GLA unter `lfemurYZ`. Der naechste-ausgewaehlter-Vorfahre-
Ansatz trifft 31 von 53. Deshalb `ImportOptions::boneParents`.

**Vertexzusammenfassung.** Aufgeteilt wird nach Position und Normalen-*Wert*
(nicht Index — die sind pro Dreiecksecke eindeutig). Messwerte an der echten
GLM: nur Position 1967, Position+Normale 2425, indexbasiert 8538, echte Datei
2647. Die fehlenden 222 erklaert das Modell noch nicht. Ein Modell mit 2425
Vertices ist gueltig und laedt normal, nur nicht byte-identisch.

## 4b. Die verifizierten Formeln

Falls du eigenen Code schreibst statt `xsi_import` zu benutzen:

### Schritt 1 — Skelett aus dotXSI (die Formeln stehen fest)

Alle `SI_Model` einsammeln, deren Name nach dem letzten Punkt einem
Bonenamen entspricht. Für jeden die `SI_Transform BASEPOSE-*` auslesen —
neun Werte: Skalierung XYZ, Rotation XYZ in Grad, Translation XYZ.

```cpp
// BASEPOSE ist ABSOLUT, nicht relativ zum Parent.
Mat3x4 R = Rz(rz) * Ry(ry) * Rx(rx);        // XYZ extrinsisch
Mat3x4 M;
M.rot   = scale * (C * R * transpose(C));
M.trans = scale * (C * vec3(tx, ty, tz));

// C = [[1,0,0],[0,0,-1],[0,1,0]]   Y-hoch nach Z-hoch
// scale = $scale aus der .car
```

Verifiziert: 44 von 52 Bones auf 6·10⁻⁷ genau gegen die echte GLA.

`basePoseMatInv` liefert `affineInverse()` aus `mdxa.h` — **nicht** die
Transponierte, da der `$scale` im Rotationsteil steckt.

Die Elternbeziehung ergibt sich aus der Verschachtelung der `SI_Model`.

### Schritt 2 — Mesh aus dotXSI (ebenfalls verifiziert)

Positionen aus `SI_Shape SHP-<name>-ORG`, Attributblöcke `POSITION`,
`NORMAL`, `TEX_COORD_UV`.

```cpp
// p ist eine Position aus SI_Shape, M die BASEPOSE des Mesh-SI_Model
vec3 world = M.rot * p + M.trans;
vec3 glm   = scale * (C * world);
```

Verifiziert an `hips`: alle 132 Positionen stimmen exakt. **Achtung:**
Carcass ändert die Reihenfolge der Vertices; für einen Vergleich muss man
über nächste Nachbarn abgleichen, nicht über den Index.

`SI_TriangleList` hat getrennte Indexarrays je Attribut — im Beispiel 132
Positionen aber 609 Normalen. GLM kennt nur einen Index pro Vertex, also
müssen eindeutige Tripel `(posIdx, normIdx, uvIdx)` gebildet und
dedupliziert werden. Das ist die eigentliche Arbeit in diesem Schritt.

Shader: `XSI_CustomPSet <mesh>.Game` → `"Shader","Text","<pfad>"`.

### Schritt 3 — Gewichte

`SI_EnvelopeList` auf oberster Ebene, ein `SI_Envelope` pro
(Mesh, Bone)-Paar. Gewichte in **Prozent**, also durch 100 teilen.
Vertices mit Gewicht 0 stehen ausdrücklich drin und gehören verworfen.

Danach pro Vertex auf höchstens vier Gewichte reduzieren und neu
normieren — das erledigt `writeMdxm()` bereits.

### Schritt 4 — Animation (verifiziert)

`SI_FCurve` pro Bone und Kanal, je Keyframe ein Paar aus Framenummer und
Wert. **Anders als BASEPOSE sind diese Werte lokal**, also relativ zum
Elternmodell. Und Achtung: die XSI-Hierarchie ist nicht die GLA-Hierarchie,
Carcass baut sie um.

```cpp
// 1. lokale Matrix je Bone und Frame
L(b,f) = T(trans) * Rz(rz) * Ry(ry) * Rx(rx) * S(scale);

// 2. Weltpose ueber die XSI-Hierarchie
Wx(b) = Wx(parent_xsi) * L(b);

// 3. in den GLA-Raum
X(b) = scale * (C * Wx(b) * inverse(C));

// 4. gegen Basispose und GLA-Elternbone relativieren
A(b) = B(parent_gla) * inverse(X(parent_gla)) * X(b) * inverse(B(b));
```

`B` ist die `basePoseMat`. `A(b)` geht komprimiert in den Bone-Pool.
Schritt 4 folgt aus der Engine: sie wertet `W(bone) = W(parent) * A(bone)`
aus, Skinning-Matrix ist `X * B⁻¹`.

Gemessen an `BOTH_attack10.xsi` gegen die echte GLA ab Zielframe 1541:
840 Vergleiche, max. Rotationsabweichung 1,77·10⁻⁴ bei einer
Quantisierungsschrittweite von 6,10·10⁻⁵. Translation bei 40 von 42 Bones
unter einer Stufe.

### Schritt 5 — `.car` abarbeiten

`car::parseFile()` liefert bereits alles Strukturierte. Die Reihenfolge
folgt den Anweisungen: `$aseanimgrabinit`, dann alle `$aseanimgrab`,
dann `$aseanimgrabfinalize`, dann `$aseanimconvertmdx_noask`.

`Script::buildSequences()` erzeugt die animation.cfg-Einträge; ihm muss
eine Funktion übergeben werden, die zu jeder `.xsi` die Framezahl liefert.

---

## 9. Verifikationsstrategie

Nach jedem Schritt gegen die Originaldateien prüfen, nicht erst am Ende.

1. **Offsets** — `ofsSkel`, `ofsFrames`, `ofsCompBonePool`, `ofsEnd`
   arithmetisch nachrechnen. Alle vier stimmen beim Original exakt.
2. **Basisposen** — eigene gegen die aus der Original-GLA gelesenen
   `basePoseMat` stellen. Abweichung muss unter 10⁻⁵ liegen.
3. **Vertexpositionen** — über nächste Nachbarn abgleichen, nicht über
   den Index.
4. **Ganze Datei** — erst wenn alles andere stimmt, byteweise vergleichen.

Für Schritt 4 wird ein in sich stimmiger Satz gebraucht: `.car`, alle
darin referenzierten `.xsi` und die daraus in **einem** Carcass-Lauf
erzeugten `.gla`/`.glm`.

---

## 10. Was gegenüber Carcass besser ist

| | Carcass v2.2 | hier |
|---|---|---|
| Rotationsfehler (echte Daten) | 0,0180° | 0,0061° |
| Quantisierung | Truncation, einseitiger Bias | Rundung + Kandidatensuche |
| Out-of-Range | wird zu −2,0 bzw. −512 | wird geklemmt |
| Warnungen | einmal pro Programmlauf | jeder Vorfall gezählt |
| Speicher | 104 MB statisch, feste Slots | dynamisch |
| Limitverletzung | Meldung, läuft weiter | Exception |
| dotXSI-Parser | Schleifentiefe 6 | 109 MB/s |
| Bone-Pool-Grenze | ungeprüft | geprüft (24-Bit-Index) |
| Zyklen im Skelett | ungeprüft | erkannt |

Alle Ausgaben bleiben mit der unveränderten Jedi-Academy-Engine
kompatibel. Am Dateiformat wurde nichts geändert.

---

## 11. Rechtliches

Das Binärformat ist Ravens Werk. `mdx_format.h` und `matcomp.cpp` stehen
unter GPLv2 (Raven Software / Activision / OpenJK-Beitragende). Dieser
Code implementiert das Format neu und übernimmt keinen Raven-Quellcode;
die Feldreihenfolgen in `format.h` sind zwangsläufig identisch, weil sie
das On-Disk-Layout beschreiben.

Vor einer Veröffentlichung sollte die GPLv2-Frage geklärt werden. Ich bin
kein Jurist.


---

## 12. Die Begleitdateien

### `.skin`

Zeilenweise `surfacename,texturpfad`, Zeilenende CRLF. Eingetragen werden alle
Surfaces, die **keine Tags** sind und einen echten Shader tragen.

An Ravens `model_red.skin` abgelesen: 34 Eintraege bei 80 Surfaces, von denen
46 Tags sind — genau die uebrigen 34, abzueglich `stupidtriangle_off` mit
seinem `[nomaterial]`.

### `.frames`

Ein Block je eingesammelter Quelldatei:

```
<leerzeile>
c:/pfad/zur/quelle.xsi
{
	"startframe"	"0"
	"duration"	"2"
	"fps"	"60"
	"averagevec"	"0.000 0.000 0.000"
}
```

Zwei Feinheiten:

**`fps` ist nicht der framespeed der animation.cfg.** Es ist die Rate aus
`SI_Scene` der Quelldatei. Bei `face_alert.xsi` steht dort 60, waehrend die
`.car` per `-framespeed 1` etwas anderes vorgibt. Bei `root.xsi` steht 29 —
die abgeschnittenen 29,97, was die Truncation-Regel ein weiteres Mal
bestaetigt.

**`averagevec` ist die Wurzelbewegung pro Frame**, mit umgekehrtem Vorzeichen
zur Rampe auf dem Wurzelbone. Das ist eine unabhaengige Bestaetigung des
Gegenbewegungsmodells:

| Sequenz | Ravens averagevec | berechnete Rampe pro Frame |
|---|---|---|
| `both_strafe_left1` | 3,520 | −42,25 / 12 = −3,521 |
| `both_death17` | 0,028 | −3,444 / 124 = −0,0278 |
| `both_sit2tostand5` | −0,084 | +4,62 / 55 = +0,084 |
| `both_wall_flip_right` | 0,000 | 0 |

Jeweils exakt der negierte Wert.

## 13. LODs: Struktur ja, Quellseite nein

Das **Schreiben** mehrerer LODs ist verifiziert (siehe `docs/DOTXSI_MAPPING.md`)
und durch einen Test abgesichert.

Was fehlt, ist die Quellseite: woran in der dotXSI zu erkennen ist, welches
Mesh zu welchem LOD gehoert. mrwonkos Blender-Exporter benutzt
`model_root_[LOD]` ab 0, wobei alle Ebenen dieselbe Objekthierarchie und
dieselben Namen haben muessen und nur LOD 0 fuer Hierarchie, Tags und
Off-Flags ausgewertet wird. Ravens `root.xsi` hat nur `mesh_root` ohne Suffix,
weshalb im Buildlog `(Mesh empty, so total LODs = 1)` steht.

Ob Carcass `mesh_root_1` erwartet oder etwas anderes, laesst sich ohne eine
Quelldatei mit mehreren LOD-Saetzen nicht entscheiden. Geraten wird hier
nicht.


---

## 14. Pruefen wie Assimilate

```sh
g2c validate _humanoid.car -enums anims.h -basedir C:\assets
g2c scan C:\assets -enums anims.h -validate
```

`scan` durchsucht einen Verzeichnisbaum nach `.car`-Dateien und listet Grabzahl
und Zielmodell; mit `-validate` werden alle gefundenen gleich mitgeprueft.

### Warum die Suche einen eigenen Stapel benutzt

Nicht `std::filesystem::recursive_directory_iterator`. Dessen `increment(ec)`
rueckt bei einem Fehler **nicht zuverlaessig vor** — die Norm laesst das
offen, und die Microsoft-Implementierung bleibt in dem Fall stehen. Eine
Schleife darueber laeuft dann endlos: ohne Fehlermeldung, ohne Absturz, sie
haengt einfach. Genau das ist unter Windows passiert, waehrend dieselbe
Fassung unter Linux monatelang klaglos lief.

Dazu kommen Verzeichnisverknuepfungen auf einen Vorfahren — unter Windows in
Benutzerprofilen ueblich —, denen der Iterator standardmaessig folgt.

Mit eigenem Stapel ist beides ausgeschlossen: jedes Verzeichnis wird einmal
geoeffnet, die Tiefe ist begrenzt, bereits besuchte Pfade werden an ihrer
kanonischen Form erkannt, Verknuepfungen werden nicht verfolgt, und eine
Obergrenze von zwei Millionen Eintraegen bricht notfalls ab. Ein Test legt
eigens eine Verknuepfungsschleife an und prueft, dass die Suche sie
uebersteht.

### Die Pruefungen

| | Stufe |
|---|---|
| Quelldatei nicht gefunden | Fehler |
| Sequenzname mehrfach vergeben | Fehler |
| Loopframe ausserhalb der Sequenz | Fehler |
| `-additional`-Bereich laenger als die Quelldatei (`-frames`) | Fehler |
| `-makeskel` zusammen mit einer GLA-Sequenz | Fehler |
| **Sequenzname ohne Enum in der Tabelle** | **Warnung** |
| **Enum in der Tabelle ohne Sequenz** | **Hinweis** |

### Zwei bewusste Abweichungen von Assimilate

**Unbekannte Enums sind eine Warnung, kein Fehler.** Assimilate lehnt sie ab;
das ergibt fuer Ravens abgeschlossenen Datenbestand Sinn, blockiert aber die
Arbeit an einem Mod. Bei Movie Duels betrifft das 165 Sequenzen — durchweg
eigene Animationen (`BOTH_MD_CIN_*`, `BOTH_BOLT_*`), die noch nicht in der
Header-Datei stehen.

**Die Gegenrichtung gibt es bei Assimilate gar nicht:** Enums, fuer die keine
Sequenz gebaut wird. Bei Movie Duels sind das 185. Im Spiel aeussert sich so
ein Fall als Figur, die eine Animation einfach nicht abspielt — ohne
Fehlermeldung, ohne Absturz, und entsprechend schwer zu finden.

### Fallstrick beim Einlesen von anims.h

Ravens `anims.h` ist **kein gueltiges C**:

```
	BOTH_STAND1		//# Standing idle, no weapon, hands down
	BOTH_STAND1IDLE1	//# Random standing idle
```

Zwischen den Eintraegen fehlt das Komma. Die Datei wird offenbar nur von den
Werkzeugen gelesen, nie uebersetzt. Wer beim Parsen darauf besteht, uebersieht
in der echten Datei **102 von 1705 Eintraegen** und meldet anschliessend
voellig gueltige Sequenzen als unbekannt.

Genau das ist mir zuerst passiert, samt einer Python-Gegenpruefung, die
dieselbe Annahme machte und deshalb dasselbe falsche Ergebnis lieferte. Die
Zahl der angeblich fehlenden Sequenzen sank nach der Korrektur von 266 auf
165.


---

## 15. Die Oberflaeche

```
build\Release\g2c-gui.exe
```

Dear ImGui liegt unter `third_party/imgui/` **im Projekt** — kein
Paketmanager, kein Installer, `build.bat` bleibt unveraendert. Die
Fensteranbindung nutzt Win32 und DirectX 11, beides Bestandteil von Windows.

### Was drin ist

| | |
|---|---|
| Tabs | ein Skript je Tab, umsortierbar, Ueberlaufmenue |
| Ordner oeffnen | durchsucht den Baum rekursiv und legt jede `.car` als Tab an |
| XSI hinzufuegen | Mehrfachauswahl, wahlweise zum aktiven Tab oder **zu allen** |
| Ziehen und Ablegen | `.car` oeffnet, `.xsi` haengt an, Ordner wird durchsucht |
| Stapelbau | alle Tabs nacheinander, im Hintergrund, mit Fortschritt und Abbruch |
| Ausgabe waehlbar | `.frames`, GLM und `.skin` je einzeln abschaltbar |
| Pruefung | Fehler und Warnungen je Tab, Enum-Spalte je Sequenz |
| Dark Mode | Vorgabe; hell umschaltbar unter Ansicht |
| Tabtitel | bei gleichnamigen Skripten entscheidet der Ordner |
| Bildschaerfe | DPI-bewusst, zeichnet in der Aufloesung des Bildschirms |

Jede `.car` erzeugt ihre eigene GLA und `animation.cfg` in
`g2c_out\` neben dem Skript, oder in einem gemeinsamen Ausgabeordner.

### Aufbau

`gui/app.cpp` enthaelt die Oberflaeche und kennt nur `imgui.h` und die
g2-Bibliothek — kein Win32, kein DirectX. Alles Betriebssystemabhaengige
kommt ueber eine `Platform`-Struktur mit drei Funktionszeigern herein.

Das ist keine Portabilitaet um ihrer selbst willen, sondern **Pruefbarkeit**:
`g2_gui_tests` oeffnet Ordner, haengt Dateien an, prueft, schliesst Tabs und
kontrolliert die Zustaende — ohne je ein Fenster zu oeffnen. Der Test laeuft
auf jedem System.

### Was NICHT geprueft ist

`gui/main_win32.cpp` — Fenster, DirectX, Dateidialoge, Ziehen und Ablegen.
Diese Datei entstand ohne Windows-SDK und wurde **nie uebersetzt**. Dort sind
Fehler zu erwarten; melde sie, dann raeume ich sie aus.

Die Oberflaechenlogik dahinter ist geprueft, die Anbindung nicht.


### Zwei Feinheiten

**Schaerfe.** Ohne Anmeldung als DPI-fähig haelt Windows eine Anwendung fuer
eine alte mit fester Aufloesung von 96 dpi: sie zeichnet klein, und Windows
skaliert das fertige Bild hoch. Das Ergebnis ist durchgehend unscharf,
Schrift wie Linien. `ImGui_ImplWin32_EnableDpiAwareness()` steht deshalb vor
dem Erzeugen des Fensters, die Schrift wird in der tatsaechlichen
Bildpunktgroesse geladen (nicht hochskaliert), und `ScaleAllSizes` zieht
Abstaende und Rundungen mit.

**Tabtitel.** In einem Modellbaum heissen alle Skripte `_humanoid.car` —
zwanzig Tabs mit demselben Text sind wertlos. Unterscheidbar sind sie am
Ordner, und genau der wird dann zum Titel (`_humanoid_ani`). Nur wo der
Dateiname schon eindeutig ist, bleibt er stehen. Der vollstaendige Pfad steht
ueber der Tabelle und als Kurzhinweis am Tab.


---

## 16. Was wohin gehoert

Die haeufigste Verwechslung, deshalb ausdruecklich:

| Feld | Richtung |
|---|---|
| **Assetwurzel** | Eingabe — der Ordner, unter dem `models\` liegt |
| **Referenz-GLA** | **Eingabe** — liefert Skelett und Scale, wird nur gelesen |
| **Enumtabelle** | Eingabe — `anims.h` fuer die Namenspruefung |
| **Zeile ueber der Tabelle** | Ausgabe — **je Skript einzeln, zwingend** |

Die Referenz-GLA ist nicht der Ort, an dem die neue GLA landet. Von dort
kommen die 53 Bones und der Scale, gegen die alle Animationen gerechnet
werden; die Datei wird nie ueberschrieben.

**Wo die neue GLA landet, steht je Skript ueber der Tabelle — und muss
gesetzt sein.** Ohne Ziel wird nicht gebaut; der Stapelbau prueft das vorab
und nennt die betroffenen Skripte, statt nach zehn Minuten abzubrechen.

Einen gemeinsamen Ordner fuer alle gibt es **bewusst nicht**. In einem
Modellbaum heissen saemtliche Ausgaben `_humanoid.gla`; ein gemeinsames Ziel
liesse zwanzig Skripte dieselbe Datei ueberschreiben — ohne Fehlermeldung,
mit dem Ergebnis, dass nur das letzte uebrig bleibt. Eine Einstellung, die in
der Mehrzahl der Faelle Daten vernichtet, gehoert nicht ins Programm.

Fuer den bequemen Fall gibt es **„Alle auf g2c_out neben der jeweiligen
.car"**: das setzt fuer jedes Skript ohne Ziel einen eigenen Ordner neben
seiner eigenen `.car`. Zwanzig verschiedene Pfade, kein gemeinsames Ziel.
Die Zuordnung wird gemerkt und steht beim naechsten Start wieder da.

### Framezahlen

Werden **immer** angezeigt, ohne Zutun. Beim ersten Mal muss jede `.xsi`
gelesen werden — bei 1289 Dateien dauert das, also laeuft es im Hintergrund
und die Spalte fuellt sich nach und nach. Beim zweiten Mal kommt alles aus
dem Zwischenspeicher.

| Anzeige | Bedeutung |
|---|---|
| `...` | wird gerade gelesen |
| Zahl | Framezahl der Quelldatei |
| `fehlt` | Datei nicht auffindbar — Assetwurzel pruefen |


---

## 17. Sprachen

Deutsch, English, 中文, 日本語 — umschaltbar unter **Ansicht → Sprache**, wird
gemerkt.

### Was daran nicht trivial ist

**Der Zeichensatz muss die Zeichen enthalten.** Dear ImGui baut beim Start
eine Textur mit genau den Glyphen, die man vorher angibt. Ohne
`GetGlyphRangesChineseSimplifiedCommon` bzw. `GetGlyphRangesJapanese` steht in
der Oberflaeche nur eine Reihe leerer Kaesten — die Zeichen sind schlicht
nicht gebacken.

**Zwei Schriften, nicht eine.** Segoe UI deckt Latein samt Umlauten ab, kann
aber kein CJK. Statt alles auf eine CJK-Schrift umzustellen, wird per
`MergeMode` eine zweite dazugelegt: `msyh.ttc` fuer Chinesisch, `YuGothM.ttc`
fuer Japanisch. Die lateinischen Zeichen kommen weiterhin aus Segoe UI und
bleiben scharf.

**Der Zeichensatz wird beim Wechsel neu gebaut.** Die Texturen des Renderers
haengen daran, also erst freigeben, dann neu bauen, dann neu anlegen.

**`/utf-8` fuer MSVC.** Ohne diesen Schalter deutet der Compiler die
Quelldateien als Codepage 1252 und macht aus jedem Umlaut zwei Zeichen;
chinesische und japanische Texte werden vollends unlesbar. Steht jetzt in
CMakeLists.txt.

### Aufbau

`gui/i18n.h` fuehrt einen Aufzaehlungstyp je Text, `gui/i18n.cpp` die Tabelle
mit vier Spalten. Ein `static_assert` vergleicht Tabellenlaenge und
Aufzaehlung — geraet beides aus dem Tritt, faellt es beim Uebersetzen auf und
nicht als leerer Knopf im laufenden Programm.

Zwei Tests sichern das ab: kein Eintrag darf leer sein, und kein chinesischer
Text darf nur das deutsche Wort sein (das waere eine vergessene
Uebersetzung). Eigennamen wie `g2c_out` sind ausgenommen.


---

## 18. Wo die Einstellungen liegen

`%APPDATA%\g2c\` — dort landen `g2c_settings.txt` (Pfade, Haekchen, Sprache,
offene Tabs, Ausgabeorte je Skript) und `g2c_gui.ini` (Fensterzustand,
Spaltenbreiten).

**Nicht im Arbeitsverzeichnis**, denn das haengt davon ab, wie das Programm
gestartet wurde: per Doppelklick ist es der Ordner der Exe, aus einer
Verknuepfung deren Arbeitsverzeichnis, aus der Eingabeaufforderung ein
beliebiger anderer. Die Einstellungen waren damit mal da und mal weg.

**Nicht unter „Dokumente".** Der Ordner gehoert dem Nutzer fuer eigene
Dateien. Spiele legen dort Spielstaende ab, weil man die sichern und
weitergeben will — Fensterpositionen und Haekchen will man das nicht, und
Programme, die ihre Konfiguration dort ablegen, muellen den Ordner zu.

### Mitnehmbarer Betrieb

Eine leere Datei **`g2c_portable.txt`** neben die Exe legen: dann liegen
Einstellungen und Fensterzustand daneben statt in `%APPDATA%`. Sinnvoll fuer
Stick, Netzlaufwerk oder mehrere Zweige nebeneinander, die sich nicht in die
Quere kommen sollen.

Eine vorhandene `g2c_settings.txt` aus dem alten Ort wird beim ersten Start
uebernommen.


---

## 19. Symbole und Fensterlage

### Symbole ohne mitgelieferte Datei

Windows bringt seit 10 die Schrift **Segoe MDL2 Assets** mit, seit 11
zusaetzlich **Segoe Fluent Icons** (rundere Ecken). Die Glyphen liegen im
privaten Unicode-Bereich ab E700 und werden per `MergeMode` in denselben
Zeichensatz gelegt wie Schrift und CJK. Das Projekt bleibt damit ohne
zusaetzliche Dateien.

Microsoft nennt den Haken selbst: PUA-Zeichen sind nicht standardisiert,
fehlt die Schrift, fehlen die Symbole. Deshalb prueft die Fensteranbindung,
ob das Laden geklappt hat, und laesst die Symbole sonst weg — Text ohne
Symbol statt Text mit leeren Kaesten.

Ein Test prueft, dass alle verwendeten Zeichen im Bereich der Icon-Schrift
liegen. Ein Tippfehler im Code erschiene sonst als leerer Kasten.

### Fensterlage

Position, Groesse und Zustand landen in `g2c_window.txt` neben den
Einstellungen.

`GetWindowPlacement` statt `GetWindowRect`: die Struktur kennt neben der
Position auch den Zustand (normal, maximiert) **und** die Groesse, die das
Fenster im nicht-maximierten Zustand haette. Mit `GetWindowRect` allein waere
ein maximiertes Fenster beim naechsten Start bildschirmgross, aber nicht
maximiert — und beim Wiederherstellen bliebe es riesig.

Zwei Sicherungen:

- Ein **minimiertes** Fenster wird als normal gespeichert. Niemand will ein
  Programm starten, das sofort in der Taskleiste verschwindet.
- Liegt die gespeicherte Lage auf **keinem** Bildschirm mehr, wird sie
  verworfen. Das passiert nach dem Abziehen eines zweiten Monitors: das
  Fenster waere da, aber unsichtbar und nicht erreichbar.


---

## 20. Welche Windows-Fassungen

| Fassung | Lauffaehig |
|---|---|
| Windows 11 | ja |
| Windows 10 | ja |
| Windows 8.1 | vermutlich, nicht geprueft |
| Windows 7 | nein |
| Windows XP | nein, technisch ausgeschlossen |

**XP ist ausgeschlossen**, nicht nur ungetestet: DirectX 11 gibt es dort
nicht, `IFileDialog` gibt es erst ab Vista, und kein C++20-faehiger
Uebersetzer erzeugt noch XP-Programme — der entsprechende Werkzeugsatz endete
mit Visual Studio 2017.

**Windows 7** scheitert an der Entwicklungsseite: Visual Studio 2022 laesst
sich ab Fassung 17.7 auf Windows 7 nicht einmal mehr installieren.

Was ich beheben konnte, ist behoben: `GetDpiForSystem` gibt es erst ab
Windows 10 1607. Fest eingebunden startet ein Programm auf aelteren Systemen
gar nicht — es faellt schon beim Laden mit "Einsprungpunkt nicht gefunden"
aus, bevor eine Zeile lief. Die Funktion wird jetzt zur Laufzeit
nachgeschlagen; fehlt sie, wird der alte Weg ueber den Geraetekontext
genommen.

Die Icon-Schrift fehlt vor Windows 10 ebenfalls — dann laeuft die Oberflaeche
ohne Symbole weiter, statt leere Kaesten anzuzeigen.

## 21. Warum tools/check_win32.py

`gui/main_win32.cpp` ist die einzige Datei, die sich in meiner Umgebung nicht
uebersetzen laesst — es gibt dort keine Windows-Header. Genau deshalb sind in
ihr mehrfach Fehler durchgerutscht, die in jeder anderen Datei der
Uebersetzer sofort gefunden haette:

- eine Signaturaenderung, die nie ankam, weil die Ersetzung um ein
  Leerzeichen danebenlag und **still** ins Leere lief
- Namen aus `g2::gui`, die im namenlosen Namensraum unqualifiziert standen

Das Skript prueft beim Bauen genau diese drei Klassen: unqualifizierte
Namen, Aufrufe mit falscher Argumentzahl, und fest eingebundene Funktionen,
die es erst ab Windows 10 gibt. Eine Gegenprobe mit den drei tatsaechlichen
Fehlern von heute zeigt, dass es sie findet.


---

## 22. Sequenzen umordnen, loeschen, neu anlegen

**Umordnen:** Zeile ziehen, oder Rechtsklick fuer "Nach oben / Nach unten /
An den Anfang / Ans Ende".

Beides ist **bei aktivem Filter gesperrt**, und das ist Absicht: der Filter
blendet Zeilen aus, zwischen zwei sichtbaren Zeilen koennen also
ausgeblendete liegen. "Hierhin verschieben" waere dann mehrdeutig, und das
Ergebnis eine Reihenfolge, die niemand wollte. Der Grund steht im
Kontextmenue, damit man nicht raetselt, warum die Eintraege grau sind.

Das Umordnen wird **nach** dem Zeichnen der Tabelle ausgefuehrt, nicht
mittendrin. Eine Liste umzustellen, ueber die gerade iteriert wird, ist der
klassische Weg zum Absturz.

**Loeschen:** Rechtsklick, einzeln oder alle ausgewaehlten. Mit
Sicherheitsabfrage — rueckgaengig gibt es nicht, und ein verrutschter
Rechtsklick soll keine Sequenz kosten.

Die Auswahlmarkierungen wandern beim Verschieben mit. Ohne das zeigte die
Auswahl nach dem Umordnen auf eine andere Sequenz, und das naechste Loeschen
traefe die falsche — ein Fehler, der erst beim Speichern auffiele.

**Neues Skript:** Datei -> Neues Skript, oder Strg+N. Angelegt wird ein
leeres, aber **gueltiges** Skript: `$aseanimgrabinit` und
`$aseanimgrabfinalize` umschliessen die Grabs, die Konvertierungsanweisung
nennt das Zielmodell. Ohne diese drei waere die Datei kein brauchbares
Carcass-Skript. Eine vorhandene Datei wird nie ueberschrieben.


---

## 23. Mehrfachauswahl und die ROOT-Regel

**Auswahl** wie in einem Dateimanager:

| | |
|---|---|
| Klick | nur diese Zeile |
| Strg + Klick | einzelne dazu oder weg |
| Umschalt + Klick | Bereich vom Anker bis hierher |

Verschieben und Loeschen wirken auf **alle ausgewaehlten** Zeilen. Ist die
angeklickte nicht darunter, gilt nur sie — sonst loescht ein Rechtsklick
irgendwo eine Auswahl, die man laengst vergessen hat.

Beim Blockverschieben muss die Zielposition um die Zeilen verringert werden,
die **vor** ihr entfernt wurden. Ohne das landet der Block genau um die
Anzahl der verschobenen Zeilen zu weit hinten — ein Fehler, der bei einer
Zeile nicht auffaellt und bei fuenf die Reihenfolge zerlegt.

### ROOT steht immer zuletzt

`root.xsi` liefert die Basispose, und in Ravens Skripten steht der Grab
zuletzt. Deshalb:

- neue Animationen werden **vor** ROOT eingereiht
- verschiebt man ROOT nach vorn, wandert er von selbst wieder ans Ende

Das ist keine Kosmetik: die Datei bleibt so, wie Carcass und Assimilate sie
erwarten.

### Warum die Uebersetzungstabelle jetzt Namen traegt

Die Tabelle war allein ueber die **Position** mit der Aufzaehlung
verknuepft, und der `static_assert` prueft nur die Laenge. Als zwei Bloecke
in der `.cpp` an anderer Stelle standen als in der `.h`, stimmte die Laenge
weiterhin — aber ab dieser Stelle war jeder Text um mehrere Plaetze
verschoben. Im Kontextmenue stand dann "Moved: %s" als Eintrag, und "Zeile
ziehen" trug ein Papierkorb-Symbol.

Jetzt traegt jeder Eintrag seinen Namen (`{S::MoveUp, ...}`), und ein
`consteval`-Vergleich prueft die Reihenfolge beim Uebersetzen. Eine
Vertauschung ist damit ein Compilerfehler statt eines stillen Versatzes.


---

## 24. Programmsymbol

`assets/g2c.svg` ist die Vorlage, `tools/make_icon.py` erzeugt daraus
`assets/g2c.ico`, `assets/g2c.rc` bindet sie an die Exe.

Die `.ico` enthaelt vier Groessen, und die kleinen sind **nicht** einfach
herunterskaliert:

| Groesse | Inhalt |
|---|---|
| 256, 48 | volle Zeichnung mit Gelenkkette |
| 32, 16 | nur die Zeichen, Kette weggelassen |

Bei 16 Pixeln waeren die drei Kettenknoten ununterscheidbar und wuerden die
Zeichen nur verschmieren. Genau dafuer erlaubt das Format ein eigenes Bild je
Groesse; Windows waehlt selbst die passende.

Gezeichnet wird in achtfacher Groesse und danach mit LANCZOS verkleinert —
Pillow kann beim Zeichnen nicht kantenglaetten, ohne diesen Umweg haetten die
Rundungen Treppen.

Im Fenster werden `hIcon` und `hIconSm` **beide** gesetzt. Gibt man nur das
grosse an, skaliert Windows es fuer die Titelleiste herunter und es wird
unscharf.

## 25. Geprueftes Schreiben

Alle Ausgabedateien laufen ueber `g2::writeFileChecked`.

Ein blosses `if (!f)` nach dem Oeffnen genuegt nicht — es faengt nur den
Fall ab, dass die Datei gar nicht angelegt werden kann. Geht beim
**Schreiben** etwas schief (Platte voll, Netzlaufwerk weg, Kontingent
erreicht), meldet der Stream das erst spaeter, und ein Teil der Daten steht
schon auf der Platte. Das Ergebnis waere eine halbe GLA, die aussieht wie
eine ganze und erst im Spiel als kaputtes Modell auffaellt.

Der Helfer schreibt, **schliesst** und prueft danach den Zustand — erst
`close()` leert den Puffer, Schreibfehler treten oft genau dort zutage.

Vorher pruefte von zwoelf Schreibstellen keine einzige den Schreibvorgang
selbst, und drei prueften gar nichts. Beide Luecken sind geschlossen und
durch Tests abgedeckt.

---

## 24. Programmsymbol

`icon/` enthaelt zwei SVG-Vorlagen, ein Skript und die fertige `g2c.ico`.

### Warum zwei Vorlagen

Eine `.ico` enthaelt fuer jede Groesse ein **eigenes** Bild, und Windows
waehlt die passende aus. Das ist kein Umweg, sondern der Zweck des Formats:
was bei 256 Bildpunkten fein und lesbar ist, wird bei 16 zu einem Fleck.

| Vorlage | Groessen | Unterschied |
|---|---|---|
| `g2c-large.svg` | 256, 128, 48 | mit Gelenkkette, duennere Striche |
| `g2c-small.svg` | 32, 16 | ohne Kette, kraeftigere Striche, groessere Zeichen |

Bei 16 Bildpunkten bekommt jedes Zeichen nur rund fuenf Bildpunkte Breite.
`g2c` ist dort gerade noch lesbar — das ist die Grenze des Machbaren, nicht
ein Mangel der Umsetzung.

### Neu erzeugen

    pip install cairosvg pillow
    python3 icon/make_icon.py

Aendert man eine SVG, muss die `.ico` neu gebaut werden. CMake bindet sie
ueber `icon/g2c.rc` ein; fehlt die Datei, wird ohne Symbol gebaut statt den
Bau abzubrechen.

### Kennung 1

Windows nimmt fuer das Symbol im Explorer das Icon mit der **niedrigsten**
Kennung in der Exe. Eine andere Zahl waere zwar in der Datei, wuerde aber
nicht als Programmsymbol erscheinen.

Fuer die Titelleiste holt `LoadImageW` mit `SM_CXSMICON` gezielt die
16-Pixel-Fassung. `LoadIconW` liefert immer die grosse — fuer die
Titelleiste waere das eine heruntergerechnete und damit matschige
Darstellung.

---

## 25. Weitergabe: eine einzige Exe

Fuer die Veroeffentlichung reicht **`g2c.exe`**. Eine Datei, mehr braucht
niemand.

### Beides in einer Datei

Dieselbe Exe ist Oberflaeche und Kommandozeile. Das erste Argument
entscheidet:

| Aufruf | Was passiert |
|---|---|
| Doppelklick | Fenster |
| `g2c build x.car -ref ...` | Kommandozeile |
| `.car` auf die Exe gezogen | Fenster, mit dieser Datei geoeffnet |

Das Programm ist als **Fensteranwendung** gebaut — sonst blitzte bei jedem
Doppelklick ein schwarzes Konsolenfenster auf. Fensteranwendungen haben aber
keine Konsole; ohne `AttachConsole(ATTACH_PARENT_PROCESS)` liefe die
Kommandozeilenfassung stumm. Sie haengt sich deshalb an die
Eingabeaufforderung, aus der sie gestartet wurde, und oeffnet nur dann eine
eigene, wenn es keine gibt.

Ein Befehl als erstes Argument bedeutet Kommandozeile, ein Dateiname
bedeutet "in der Oberflaeche oeffnen". Deshalb oeffnet eine auf die Exe
gezogene `.car` das Fenster und nicht eine Konsole.

### Laufzeitbibliothek fest eingebaut

Ohne das braucht jeder Nutzer das "Visual C++ Redistributable". Fehlt es,
startet das Programm nicht und meldet **"VCRUNTIME140.dll wurde nicht
gefunden"** — die haeufigste Ruecklaufursache bei weitergegebenen
Windows-Programmen, und eine, die der Empfaenger nicht selbst loesen kann.

Fest eingebaut ist die Exe rund 100 kB groesser und laeuft dafuer ueberall
ohne Installation.

### Was ins Paket gehoert

    g2c.exe          das Programm, sonst nichts noetig
    ANLEITUNG.md     optional, aber hilfreich

`build.bat` legt daneben noch `g2c-cli.exe` ab: dasselbe Werkzeug ohne
Oberflaeche, zum Entwickeln. Fuer die Weitergabe wird sie nicht gebraucht.
Beide Ziele duerfen nicht denselben Dateinamen tragen, sonst ueberschreiben
sie sich je nach Baureihenfolge — mal mit, mal ohne Oberflaeche.

Nicht noetig: DLLs, Laufzeitpakete, ImGui, Schriften. Die Icon-Schrift und
Segoe UI gehoeren zu Windows; fehlen sie, laeuft die Oberflaeche ohne
Symbole weiter.

**Voraussetzung beim Nutzer:** Windows 10 oder 11, 64 Bit. Siehe Abschnitt 20.

### WhiteoutTexCLI.exe

Wird nur fuer BLP-Umwandlung gebraucht und ist **nicht** Teil von g2c. Wer
das benutzt, legt sie wie gehabt daneben.

### Warum CommandLineToArgvW und nicht __argv

`__argv` ist bei `wWinMain` **leer**. Die Laufzeitbibliothek fuellt bei einem
Unicode-Einstiegspunkt nur `__wargv`; ein Zugriff auf `__argv[1]` laeuft in
einen Nullzeiger. Das Programm stuerzt dann ab, BEVOR das Fenster erscheint —
von aussen sieht es aus, als passiere gar nichts. Genau so verschwand das
Ziehen einer `.car` auf die Exe.

`CommandLineToArgvW` ist ausserdem der einzige zuverlaessige Weg fuer Pfade
mit Umlauten oder anderen Zeichen ausserhalb der Codepage — und solche Pfade
zieht man auf ein Programm.

`tools/check_win32.py` meldet die Verwendung von `__argv` und `__argc` beim
Bauen.


---

## 26. Auswahl und Umordnen in der Tabelle

| Bedienung | Wirkung |
|---|---|
| Klick | nur diese Zeile |
| Strg + Klick | einzelne dazu oder weg |
| Umschalt + Klick | Bereich vom Anker bis hierher |
| **Maustaste halten und ziehen** | Auswahl aufziehen, wie im Explorer |
| **Von einer ausgewaehlten Zeile ziehen** | Auswahl verschieben |
| Rechtsklick auf eine andere Zeile | "%zu Sequenz(en) hierhin verschieben" |

Der Unterschied zwischen Aufziehen und Verschieben entscheidet sich beim
**Druecken**: auf einer bereits ausgewaehlten Zeile beginnt ein Verschieben,
auf einer anderen eine neue Auswahl. Genau so verhaelt sich der Explorer,
deshalb muss man es niemandem erklaeren — und ohne diese Regel koennte man
eine getroffene Auswahl nicht mehr verschieben, ohne sie vorher zu
verlieren.

Zusammen ergibt das den bequemen Weg, verstreute Sequenzen
zusammenzufuehren: mit Strg einsammeln, an der Zielstelle rechtsklicken.

## 27. Warum tools/check_wiring.py

Mehrfach ist in diesem Projekt eine Ersetzung ins Leere gelaufen und hat
still eine Codestelle entfernt. Beim Umordnen traf es die Ausfuehrung: das
Kontextmenue setzte `pendingBlock_`, aber niemand las das Feld mehr aus.

Der Compiler schweigt dazu — eine Zuweisung an ein vorhandenes Feld ist
voellig legal. Die Tests schwiegen ebenfalls, weil die Datenoperation
`moveGrabs` in Ordnung war; nur die Verdrahtung fehlte. Aus Sicht des
Nutzers hiess das: Klick, und nichts passiert.

Das Skript meldet Felder, in die nur geschrieben wird. Solche Felder sind
fast immer genau dieser Fall.

---

## 28. Zweiter Modus: GLA zurueck nach dotXSI

Links am Rand schaltet man zwischen den beiden Richtungen um. Sie sind
bewusst **getrennt**: sie teilen fast nichts — andere Eingaben, andere
Ausgaben, andere Begriffe. In eine gemeinsame Oberflaeche gepresst muesste
man staendig Felder ausgrauen, die im jeweiligen Modus keinen Sinn ergeben.

| Modus | Wofuer |
|---|---|
| **XSI -> GLA** | Skripte bearbeiten und Animationen bauen |
| **GLA -> XSI** | Animationen aus einer fertigen GLA herausloesen |

### Ablauf

**GLA oeffnen genuegt.** Die Begleitdateien werden im selben Ordner gesucht
und mitgeladen; sie liegen praktisch immer daneben. Was gefunden wurde, steht
gruen ueber der Tabelle.

| Datei | Gesucht als | Noetig? |
|---|---|---|
| `animation.cfg` | `animation.cfg`, `<name>_animation.cfg`, `<name>.cfg`, sonst die einzige `.cfg` im Ordner | **ja** — nur sie nennt die Sequenzgrenzen |
| `.frames` | `<name>.frames`, `animation.frames`, sonst die einzige `.frames` | nein — nur fuer die Wurzelbewegung |

Liegen mehrere Dateien derselben Art im Ordner, wird **nicht geraten**; dann
waehlt man von Hand ueber die Schaltflaechen.

Ohne `animation.cfg` gibt es keine Sequenzgrenzen, und es laesst sich nur
alles am Stueck exportieren. Die `.frames` ist optional: von Ravens 1289
Sequenzen haben nur 178 ueberhaupt eine Wurzelbewegung — alle uebrigen laufen
an Ort und Stelle, das Spiel bewegt die Figur.

Danach Zielordner waehlen, Sequenzen auswaehlen (Strg und Umschalt wie
ueberall), exportieren.

Sequenzen, die ueber das Ende der GLA hinausragen, werden uebersprungen und
nicht angezeigt: sie liessen sich ohnehin nicht exportieren, und in der Liste
wuerde man sie fuer brauchbar halten.

### Die Umkehrung

Aus der Vorwaertsformel

    Wurzel:  A(b) = X(b)·B(b)^-1
    sonst:   A(b) = B(p)·X(p)^-1·X(b)·B(b)^-1

folgt eindeutig

    Wurzel:  X(b) = A(b)·B(b)
    sonst:   X(b) = X(p)·B(p)^-1·A(b)·B(b)

Danach zurueck in den dotXSI-Raum ueber `wx = C^-1·(X/scale)·C`, lokal machen
gegen den Elternbone, und in Translation, Rotation und Skalierung zerlegen.

### Drei Fallen, die dabei gefunden wurden

**Reihenfolge.** In Ravens `_humanoid.gla` haben **acht Bones ihren
Elternbone hinter sich** — `ceyebrow`, `jaw` und weitere haengen an Bone 52
(`face`). Wer in Indexreihenfolge rechnet, arbeitet dort mit
uninitialisierten Werten.

**Skalierung.** Die Gesichtsbones tragen in ihrer lokalen Transformation eine
Skalierung von **1,087**. Verwirft man sie beim Zerlegen, kommt beim
Rueckimport genau der Kehrwert heraus — 0,92 statt 1,0.

**Verschachtelung.** `SI_FCurve` muss **direktes Kind** von `SI_Model` sein.
Der Importeur sucht nur unter den unmittelbaren Kindern; ein umschliessender
`SI_Animation`-Block macht die Kurven unsichtbar. Es kamen null Kanaele an,
jeder Bone blieb auf seiner SRT-Ruhepose stehen — und genau deshalb stimmte
ausgerechnet Frame 0 immer und alles danach nicht.

### Genauigkeit

Gegen Ravens `_humanoid.gla`, 60 Sequenzen aus der echten `animation.cfg`:

| | |
|---|---|
| groesster Translationsfehler | 0,033 (zwei Quantisierungsstufen) |
| groesster Rotationsfehler | 0,011 |
| Ausreisser | keine |

Bitgleich wird es nicht: die Rotationen liegen in der GLA als
16-Bit-Quaternionen vor. Eine Quantisierungsstufe sind 0,015625 — mehr
Genauigkeit ist aus der Datei nicht herauszuholen.

**Wichtig:** immer ganze Sequenzen exportieren. Ein willkuerliches Fenster
ueber eine Sequenzgrenze hinweg enthaelt einen Teil der Wurzelbewegungsrampe,
und die laesst sich dann nicht mehr sauber zuordnen.

### Kommandozeile

    g2c export _humanoid.gla -cfg animation.cfg -o xsi_out
    g2c export _humanoid.gla -cfg animation.cfg -o xsi_out -only BOTH_STRAFE

### Extrahieren, mischen, neu bauen

Der eigentliche Zweck: eine Animation aus einer fremden GLA herausloesen, mit
eigenen `.xsi` mischen und eine neue GLA bauen.

    g2c export fremd.gla -cfg animation.cfg -frames fremd.frames -o out/

Zwei Angaben entscheiden darueber, ob das Ergebnis stimmt:

**`-origin`** — der Versatz, mit dem die Quell-GLA gebaut wurde. Er ist beim
Bauen abgezogen worden; wird er beim Export nicht wieder eingesetzt, zieht
das Neubauen ihn ein ZWEITES Mal ab, und das Modell steht um genau diesen
Betrag daneben. Ohne Angabe wird er geschaetzt (der Versatz ist ueber alle
Frames konstant, die Wurzelbewegung nicht) und ausgegeben, damit man ihn
pruefen kann.

**`-frames`** — die `.frames` der Quell-GLA. Carcass entfernt beim Bauen die
Wurzelbewegung und legt den Betrag als `averagevec` dort ab. In der GLA
selbst ist die Bewegung **weg**; ohne diese Datei laeuft eine extrahierte
Laufanimation nach dem Neubauen auf der Stelle.

Gemessen an Ravens `_humanoid.gla`, Ausschnitt herausgeloest und neu gebaut:

| Sequenz | Rotation | Translation | ausserhalb Toleranz |
|---|---|---|---|
| `BOTH_A1_SPECIAL` | 0,016 Grad | 0,0156 | **0** |
| `BOTH_STRAFE_LEFT1` | 0,005 Grad | 0,0156 | **0** |

0,0156 ist genau eine Quantisierungsstufe. Die neu erzeugte `.frames` enthaelt
`averagevec 3.519` gegen `3.520` im Original — die Wurzelbewegung ueberlebt
den Umweg.

### Andere Skelette, andere Modelle

Der Exporter haengt an keiner festen Bonezahl. Geprueft an JK2s
`_humanoid.gla` mit **72 Bones** statt der 53 aus JKA: gelesen, exportiert,
zurueckgerechnet. Auch Rigs eigener Modelle sind kein Sonderfall — sie
brauchen nur ein Referenzskelett, aus dem die Bindposen kommen.

### Ausschnitte tragen einen Rest Wurzelbewegung

Carcass entfernt die Wurzelbewegung **je Animation**. Eine Sequenz, die nur
ein Ausschnitt einer laengeren ist, traegt deshalb einen Rest davon. In JK2s
`animation.cfg` sind das **153 von 931 Sequenzen (16,4 %)** — es sind die aus
`-additional` erzeugten, etwa `BOTH_D2_TL___` mit vier Frames ab Frame 72.

Beim Neubauen wird dieser Rest wieder entfernt und landet in der `.frames`.
**Die Animation bleibt heil** — sie bewegt sich im Spiel genauso, nur steht
die Bewegung dann in der `.frames` statt im Bone. Bitgleich zur Quelle wird
die neue GLA an diesen Stellen nicht.

Das ist keine Ungenauigkeit der Rechnung, sondern eine Eigenschaft des
Formats: die Werkzeugkette erzeugt immer eine GLA, deren Motion-Bone ueber
die Sequenz keinen Weg zuruecklegt. Wer eine Sequenz exportiert, die diese
Eigenschaft nicht hat, kann sie nicht bitgleich zurueckbekommen.

`g2c export` zaehlt die betroffenen Sequenzen und sagt es.

### JK2-GLA: flache Hierarchie und die Grenze des .xsi-Modells

JK2s `_humanoid.gla` hat **72 Bones statt 53**, und — das ist der
entscheidende Unterschied — eine **flache Hierarchie**: 46 der 72 Bones
haengen direkt am Brustkorb, darunter der komplette Arm samt Hand und allen
Fingern. Nicht falsch gelesen; die Kinderlisten in der Datei bestaetigen es
(0 Widersprueche bei 72 Bones).

Das hat eine Folge, die zuerst wie ein Rechenfehler aussah:

Ravens Bindposen sind exakt rechtwinklig, haben aber **ungleiche
Achsenlaengen** (bis 0,696 statt 0,64). Verkettet man mehrere solcher
Matrizen, entsteht **Scherung** — und die kann eine `.xsi` grundsaetzlich
nicht darstellen: der Importeur baut `m = R·diag(s)`, also Rotation mal
Achsenskalierung, ohne Scherterme.

Bei JKA faellt das kaum auf, weil jeder Bone nahe an seinem Elternbone
sitzt. Bei JK2 liegt zwischen Brustkorb und Fingerspitze ein langer Hebel,
und aus einem winzigen Winkelfehler wird dort ein sichtbarer Versatz.

**Was dagegen hilft:** statt die Spalten nur zu normieren, wird ueber eine
Polarzerlegung die *naechstgelegene* Rotation bestimmt. Die Scherung bleibt
unrepraesentierbar, aber der verbleibende Fehler wird so klein wie moeglich.

| | vorher | nachher |
|---|---|---|
| JKA, Rotationsfehler | 0,0032 | **0,00017** |
| JK2, groesster Fehler | 1,07 | **0,10** |

Auch die JKA-Werte haben davon profitiert — die Polarzerlegung ist in beiden
Faellen die bessere Wahl.

**Was bleibt:** bei JK2 koennen einzelne Fingerbones um bis zu 0,1 Einheiten
abweichen, das sind rund sieben Quantisierungsstufen. Fuer Arme, Beine,
Rumpf und Kopf liegt der Fehler im Bereich der Quantisierung. Wer
JK2-Animationen nach JKA uebernimmt, sollte die Finger im Blick behalten;
alles andere ist unauffaellig.

---

## 29. Ein .car aus einem Ordner voller .xsi erzeugen

Fuer Modelle, zu denen es nie eines gab — viele fremde Modelle liegen nur als
Sammlung von `.xsi` vor.

    g2c makecar <ordner> [-o datei.car] [-basedir <pfad>] [-enums anims.h]
                         [-origin 0 0 24] [-root root.xsi] [-makeskel <pfad>]

**Was abgeleitet wird**

| | woher |
|---|---|
| Sequenzname | aus dem Dateinamen, in Grossbuchstaben — so macht es Raven auch |
| Bildrate | aus `SI_Scene` der jeweiligen Datei |
| Modelldatei | `root.xsi`, sonst mit `-root` angeben |
| Assetwurzel | der Ordner oberhalb von `models\` |
| Skelettpfad | aus der Ordnerlage, sonst mit `-makeskel` |
| Reihenfolge | sortiert, `root` zuletzt |

Kein `-framespeed` wird geschrieben: ohne ihn gilt die Rate aus `SI_Scene`,
und die hat der Autor gesetzt. Ein Vorgabewert waere hier eine Erfindung.

**Was NICHT ableitbar ist** und deshalb offen bleibt — das Programm sagt es
ausdruecklich, damit niemand annimmt, es sei rekonstruiert worden:

- `-loop` — welcher Frame die Schleife beginnt, steht in keiner `.xsi`
- `-additional` — Unterbereiche einer Datei sind eine reine Autorenangabe
- `-origin` — muss man wissen; ueblich ist `0 0 24`

Mit `-enums anims.h` wird zusaetzlich gemeldet, wie viele der abgeleiteten
Namen im Spielcode gar nicht vorkommen.

### Der Rahmen um die Grabs

`writeScript` gibt die Grabs **nur nach einem `$aseanimgrabinit`** aus. Ein
von Hand zusammengesetztes Skript ohne diese Anweisung verliert sie beim
Schreiben stillschweigend — die Datei sieht vollstaendig aus und enthaelt
keine einzige Sequenz.

Genau das ist beim ersten Anlauf passiert, und es betraf auch „Neues Skript"
in der Oberflaeche. `addGrabFrame` legt den Rahmen an; ein Test belegt beide
Seiten: ohne Rahmen geht der Grab verloren, mit Rahmen ueberlebt er das
Schreiben und Wiedereinlesen.

### Eigene Humanoids

Weder Bonezahl noch Scale sind festgeschrieben. Geprueft am SBD-Humanoid:
**43 Bones und Scale 0,6** statt Ravens 53 und 0,64 — erkannt, exportiert,
zurueckgerechnet. Der Versatz wurde ebenfalls richtig als `0 0 24`
geschaetzt.

| Skelett | Bones | Scale | max. Rundlauffehler |
|---|---|---|---|
| JKA `_humanoid` | 53 | 0,64 | 0,033 |
| JK2 `_humanoid` | 72 | 0,64 | 0,10 (flache Hierarchie) |
| SBD (eigener) | 43 | 0,60 | 0,076 |

### Bonenamen: Gross- und Kleinschreibung

Ravens Modelle schreiben den Bewegungsbone **`Motion`**, eigene oft
**`motion`** — beim SBD-Humanoid ist genau das der Fall.

Wurde nur genau verglichen, fand die Rampenberechnung ihren Bone nicht. Die
Folge: die Wurzelbewegung wird beim Bauen **nicht entfernt**, und die Figur
rutscht im Spiel beim Laufen davon. Der Fehler zeigt sich nirgends beim
Bauen — keine Meldung, keine auffaellige Zahl.

`indexOf` vergleicht jetzt erst genau und dann ohne Ruecksicht auf die
Schreibweise. Genau zuerst, damit bei zwei Bones, die sich nur darin
unterscheiden, der exakte gewinnt.

Nach der Korrektur erkennt das Werkzeug beim SBD-Humanoid **34 Sequenzen mit
Wurzelbewegung**, die vorher unsichtbar waren.

---

## 30. Fehlermeldungen mit Substanz

Bei 1393 Grabs sagt "eine Animationsdatei fehlt" nichts darueber, welche
Animation betroffen ist. Gemeldet wird deshalb:

    2 von 1393 Animationsdateien nicht gefunden.
    Gesucht unter: C:\jka_animations\md\base
    Es fehlen:
      models/players/x/weg.xsi
          Sequenz BOTH_ATTACK1, .car Zeile 2
          erwartet: C:\jka_animations\md\base\models\players\x\weg.xsi

Drei Angaben, die vorher fehlten:

- **welche Sequenz** — danach sucht man in der `.car`, nicht nach dem Pfad
- **welche Zeile** der `.car`
- **wo genau** die Datei erwartet wurde, als vollstaendiger Pfad

In der Oberflaeche ist die Meldung uebersetzt: sie wird aus den
strukturierten Daten aufgebaut, nicht aus dem Ausnahmetext der Bibliothek.

**Der Probelauf darf nicht werfen.** Fehlen ALLE Dateien, bricht `build` mit
"keine einzige lesbar" ab — dann kaeme die ausfuehrliche Meldung nie
zustande, und man saehe genau den unbrauchbaren Satz, den sie ersetzen soll.
Die Oberflaeche faengt das ab und baut die Liste selbst.

### Abweisen, wo der Fehler entsteht

Ein Ordner oder eine Datei, die keine `.xsi` ist, liess sich anhaengen und
fiel erst beim Bauen auf — dort stand dann ein Pfad in der Fehlerliste, mit
dem niemand etwas anfangen konnte. Genau so kam der Ordner
`C:\Users\...\test` in die Liste.

Jetzt wird beim Hinzufuegen geprueft und mit Grund abgewiesen:

| Fall | Meldung |
|---|---|
| Ordner statt Datei | Das ist ein Ordner, keine Animationsdatei |
| falsche Endung | Keine .xsi-Datei |
| Datei fehlt | Datei gibt es nicht (mehr) |

---

## 31. Eine GLA vollstaendig zerlegen und wieder bauen

**„Alles + .car"** im Modus GLA -> XSI, oder auf der Kommandozeile:

    g2c export _humanoid.gla -cfg animation.cfg -frames _humanoid.frames \
              -o out/models/players/x -prefix models/players/x/ \
              -car out/_humanoid.car

Das Ergebnis ist ein Ordner voller `.xsi` **und** ein Skript, mit dem sich die
GLA sofort wieder bauen laesst.

### Warum das besser ist als makecar

`makecar` sieht nur einen Ordner und muss Loopframe und Rate offenlassen.
Hier stehen beide in der `animation.cfg` — es wird nichts geraten.

### Der Kniff: Unterbereiche

Viele Sequenzen sind Ausschnitte einer laengeren. In JK2s `animation.cfg`
sind das **268 von 989**. Exportierte man jede als eigene Datei, haette die
neue GLA mehr Frames als die alte und die `animation.cfg` passte nicht mehr.

Deshalb wird nur der jeweils groesste, sich nicht ueberschneidende Bereich
als Datei geschrieben; was darin liegt, wird zu `-additional` — genau die
Struktur, die Raven selbst benutzt:

    $aseanimgrab .../both_a1_bl_tr.xsi -loop -1 -framespeed 30 -enum BOTH_A1_BL_TR
        -additional 0 1 -1 -10 BOTH_B1_BL___
        -additional 5 1 -1 30 BOTH_D1_TR___

Bei JK2 ergibt das **721 Master, die genau die 17278 Frames abdecken** — ohne
Luecke, ohne Ueberlappung.

### Gemessen an Ravens JK2-GLA

Zerlegt, neu gebaut, verglichen:

| | |
|---|---|
| Frames | 17278 = Original |
| Sequenzen | 989 = Original |
| `animation.cfg` | **989 von 989 identisch** (Name, Start, Frames, Loop, Rate) |
| mittlere Abweichung | 0,0046 Grad / 0,0065 Einheiten |

Uebrig bleiben zwei bekannte Effekte: `model_root` in den Sequenzen mit
Wurzelbewegung (mit `-frames` behoben) und die Fingerbones bis 0,27 Grad
(Grenze des .xsi-Modells bei JK2s flacher Hierarchie). Rumpf, Kopf und Beine
tauchen in der Abweichungsliste gar nicht auf.

### Der Versatz muss ins Skript

Der Export rechnet den erkannten `-origin` in die `.xsi` ein. Fehlt er dann
im Skript, zieht das Neubauen ihn nicht wieder ab und das **ganze Modell
steht um diesen Betrag daneben** — bei Ravens Dateien um 24 Einheiten. Das
Skript traegt deshalb immer den wirksamen Wert, nicht nur einen ausdruecklich
angegebenen.

### Ohne .frames geht es auch

Die `.frames`-Datei ist fuer die Wurzelbewegung **nicht noetig**. Carcass
rechnet sie als lineare Rampe auf den Wurzelbone — sie steht damit
weiterhin in der GLA, als Translationsdifferenz zwischen erstem und letztem
Frame der Sequenz, geteilt durch die Zahl der Schritte.

Gegen Ravens eigene `_humanoid.frames` geprueft: bei **allen 178 Sequenzen
mit Bewegung** stimmt der so gewonnene Wert mit `averagevec` ueberein.

Die Reihenfolge ist: erst die `.frames`, wenn es eine gibt — sie ist die
Quelle aus erster Hand. Fehlt sie, wird rekonstruiert. Mit `-nomotion` laesst
sich beides abschalten.

### Ravens JKA-GLA vollstaendig zerlegt und neu gebaut, ohne .frames

| | |
|---|---|
| Frames | 30384 = Original |
| Sequenzen | 1683 = Original |
| `animation.cfg` | **1683 von 1683 identisch** |
| ausserhalb der Toleranz | **169 von 1,6 Millionen (0,010 %)** |
| max. Rotation | 0,012 Grad |
| Mittel | 0,00021 Grad / 0,00012 Einheiten |
| erzeugte `.frames` | 1201 von 1265 `averagevec` identisch |

Bei JK2 bleibt es bei den bekannten 2 % — Ursache ist dort die flache
Hierarchie, nicht die Wurzelbewegung.

---

## 32. Was ein erzeugtes .car enthaelt — und was nicht

Vergleicht man ein erzeugtes Skript mit einem von Raven, faellt auf, dass
oben etwas fehlt. Der Reihe nach:

| Anweisung | im erzeugten Skript? | warum |
|---|---|---|
| `$scale` | **ja** | steht in der GLA |
| `$keepmotion` | nur mit `-keepmotion` | aus der GLA nicht ablesbar |
| `$pcj ...` | **nein** | nicht rekonstruierbar |
| `-qdskipstart/-qdskipstop` | nein | reine Autorenangabe |
| `-loop`, `-framespeed` | ja | stehen in der animation.cfg |
| `-additional` | ja | aus den Unterbereichen abgeleitet |

### $pcj ist nicht rekonstruierbar

`$pcj` nennt die Bones, die die Engine zur Laufzeit selbst drehen darf —
Ragdoll, Blickrichtung. Diese Liste steht **nur in der .car** und
hinterlaesst in der GLA keine Spur: in Ravens `_humanoid.gla` tragen nur
zwei von 53 Bones ueberhaupt ein Flag, und das sind `Motion` und `face`.

**Wer eine bestehende .car ersetzt, muss den `$pcj`-Block von dort
uebernehmen.** Fuer die GLA selbst ist er ohne Belang — das Bauen benutzt
ihn nicht —, aber die Engine braucht ihn.

### $keepmotion wurde bisher nicht beachtet

Gelesen wurde es, verwendet nie: bei einem Skript mit `$keepmotion` hat das
Werkzeug die Wurzelbewegung trotzdem herausgerechnet. Ravens
`_humanoid_yoda.car` benutzt es.

Jetzt gemessen: derselbe Bau ergibt beim Wurzelbone **64 Einheiten Versatz
ohne** `$keepmotion` und **0 mit** — die Bewegung bleibt also in der
Animation, wie es soll.

`$scale` aus dem Skript hat ausserdem Vorrang vor dem Wert der Referenz-GLA.
Normalerweise sind beide gleich; steht im Skript ein anderer, ist das eine
Absicht des Autors.

### Zwei Schoenheitsfehler behoben

- `-origin -0 -0 24` — die Schaetzung liefert negative Null. Sieht nach einem
  Fehler aus, war keiner; wird jetzt als `0 0 24` geschrieben.
- `-makeskel /_humanoid` — ohne `-prefix` entstand ein fuehrender
  Schraegstrich und damit ein absoluter Pfad, der nicht gefunden wird.

Und `-only` zusammen mit `-car` wird jetzt gemeldet: das Skript nennt alle
Sequenzen, exportiert werden aber nur die gefilterten.

### Die letzte Zeile

    $aseanimconvertmdx_noask <root> -makeskel <pfad/name> -origin x y z

Sie sagt zweierlei: wo die Quelldatei mit dem Modell liegt, und **wo die GLA
entsteht und wie sie heisst**.

Der `-makeskel`-Wert muss nicht abgeleitet werden — er steht **exakt im Kopf
der Quell-GLA**. Bei Ravens `_humanoid.gla` ist das
`models/players/_humanoid/_humanoid`, und genau das steht auch in ihrer
`.car`. Das erzeugte Skript uebernimmt ihn von dort.

Nur wenn er fehlt, wird er aus dem Ablageort abgeleitet — ein Notbehelf. Mit
`-makeskel <pfad>` laesst er sich ueberschreiben, etwa wenn die neue GLA
anders heissen soll als die Quelle.

Der `root`-Pfad zeigt dagegen dorthin, wo die exportierten Dateien liegen —
er richtet sich nach `-prefix`, nicht nach der Quelle.

Geprueft: die aus dem erzeugten Skript neu gebaute GLA traegt denselben Namen
wie das Original.

---

## 33. Zwei Humanoids vergleichen

**„Vergleichen mit..."** im Modus GLA -> XSI laedt eine zweite
`animation.cfg` — etwa die von JKA — und beantwortet die Frage, um die es
geht: **welche Sequenzen hat diese GLA, die jene nicht hat?** Genau die will
man uebernehmen.

| Bedienung | Wirkung |
|---|---|
| Spalte „Dort" | zeigt je Sequenz `vorhanden` oder `fehlt` |
| Haekchen „nur fehlende" | blendet alles aus, was drueben schon da ist |
| „Alle fehlenden auswaehlen" | markiert sie in einem Zug |

Danach exportieren wie sonst auch — die Auswahl gilt.

Die Namen werden **ohne Ruecksicht auf Gross- und Kleinschreibung**
verglichen, und Leerzeichen am Zeilenende werden abgeschnitten.
`animation.cfg`-Dateien sind darin nicht einheitlich: JK2 schreibt
`BOTH_STAND1`, andere `both_stand1`. Wuerde exakt verglichen, meldete das
Werkzeug hunderte Sequenzen als fehlend, die laengst da sind.

### An den echten Dateien

JK2 hat 989 Sequenzen, JKA 1683. Davon fehlen **151 in JKA** — darunter der
ganze `both_bartender_*`- und `both_cockpit_*`-Block aus den
Zwischensequenzen. Umgekehrt hat JKA 845, die JK2 nicht kennt.

---

## 34. Sequenzen kopieren und einfuegen

Rechtsklick in der Tabelle: **Kopieren**, **Ausschneiden**, und wenn etwas in
der Ablage liegt, **hier einfuegen (davor)** und **(danach)**.

Die Ablage gilt **tab-uebergreifend** — aus einem Skript kopieren, in einem
anderen einfuegen ist der eigentliche Zweck. Oben in der Kopfzeile steht,
wie viele Sequenzen darin liegen.

Kopiert wird der **vollstaendige Grab**: Dateiname, `-enum`, `-loop`,
`-framespeed` und alle `-additional`. Nur den Namen zu uebernehmen waere
wertlos — die Einstellungen sind die eigentliche Arbeit.

Es werden **Kopien** abgelegt, keine Verweise. Wird die Quelle danach
geloescht oder das Skript geschlossen, bleibt die Ablage gueltig, und
mehrfaches Einfuegen ist moeglich.

Wie beim Umordnen geschieht das Einfuegen **nach** dem Zeichnen der Tabelle:
mitten im Zeichnen die Liste zu veraendern, ueber die gerade iteriert wird,
ist der klassische Weg zum Absturz.

ROOT bleibt auch danach die letzte Sequenz.

## 35. Oberflaeche vollstaendig uebersetzt

Alle Protokollmeldungen liegen jetzt in der Uebersetzungstabelle. Vorher
standen dort 30 feste deutsche Texte — wer auf Englisch, Chinesisch oder
Japanisch arbeitete, bekam bei jedem zweiten Vorgang deutschen Text.

Dafuer gibt es `trf(S::Id, ...)`: uebersetzen und formatieren in einem
Schritt. Das spart an dreissig Stellen dasselbe `snprintf` mit eigenem
Puffer.

---

## 36. Vorschau: Skelett abspielen

Dritter Knopf links, unter „GLA -> XSI". Er nutzt die GLA, die dort geladen
ist — sie ein zweites Mal zu laden waere Verschwendung, die Datei ist 15 MB
gross und liegt bereits im Speicher.

| Bedienung | Wirkung |
|---|---|
| Sequenz waehlen | Auswahlliste mit Filter |
| Abspielen / Anhalten | mit der Rate aus der `animation.cfg` |
| Schieberegler | einzelne Frames |
| Ziehen | Ansicht drehen |
| Mausrad | zoomen |

### Ohne DirectX

Gezeichnet wird mit **ImGuis Zeichenliste**, nicht mit einer eigenen
Grafikschnittstelle. Zwei Gruende:

Der Code liegt damit in `gui/app.cpp` und `gui/preview.cpp` — Dateien, die
sich hier **uebersetzen und testen** lassen. `gui/main_win32.cpp`, die
einzige, die das nicht kann, bleibt unberuehrt. In ihr sind heute schon
mehrere Fehler unbemerkt durchgerutscht.

Und ein Skelett braucht keine Grafikschnittstelle: ein paar hundert Linien
pro Bild zeichnet ImGui ohne Muehe, und wir sparen uns Shader, Puffer und
einen zweiten Renderpfad.

### Was dabei zu beachten war

**Z ist oben**, nicht Y. Das entspricht der Konvention der GLA — im Spiel
steht die Figur in Z. Wer hier Y als oben annimmt, sieht das Modell liegen.

**Punkte hinter der Kamera** duerfen nicht gezeichnet werden, sonst
erscheint die Linie gespiegelt auf der falschen Seite.

**Die Kamera muss sich einpassen.** Die Skalierung der GLA ist nicht
einheitlich (0,6 bis 0,64), und eigene Modelle koennen deutlich groesser
sein. Ohne automatisches Einpassen steht die Kamera entweder im Kopf oder
hundert Einheiten daneben.

**Tiefe als Helligkeit:** was weiter weg ist, wird dunkler. Ohne das ist bei
einem Skelett von vorn nicht zu erkennen, welcher Arm vorn liegt.

**Negative Raten laufen rueckwaerts.** In Ravens `animation.cfg` steht etwa
`BOTH_UNCROUCH1` mit `-20` — das ist `BOTH_CROUCH1` rueckwaerts.

### An Ravens _humanoid.gla geprueft

    53 Bones, Kasten -24,0 bis 28,1 in Z, Radius 26,0
    Kameraabstand: 108,9
    sichtbare Verbindungen: 52 von 52
    liegt im Bild: x 361..539 von 0..900, y 142..539 von 0..700

Alle Bones sichtbar, alles innerhalb der Zeichenflaeche.

### Nicht enthalten

Kein Modell, keine Texturen, keine Shader. Das waere Stufe 2 und 3 — und
Stufe 3 (vollstaendiges Q3-Shadersystem, Glow, Partikel, Klingen) ist ein
eigenes Projekt, das ModView bereits gut macht.

**ModView steht unter GPL-2.0.** Code von dort zu uebernehmen wuerde
bedeuten, dass g2c ebenfalls GPL-2.0 werden muss. Hier ist nichts davon
uebernommen; die Formate kennen wir aus eigener Arbeit.

---

## 37. build.bat: Abhaengigkeitspruefung und Fenster

### Laeuft die Exe bei anderen?

Nach dem Bau wird geprueft, ob `g2c.exe` an der Visual-C++-Laufzeit haengt:

    Pruefe Abhaengigkeiten...
      [OK] Laufzeit fest eingebaut - laeuft ohne Visual C++ Redistributable.

Steht dort stattdessen eine Warnung, meldet das Programm bei jedem ohne
installiertes Redistributable **"VCRUNTIME140.dll wurde nicht gefunden"** und
startet gar nicht. Der Empfaenger kann daran nichts aendern — deshalb faellt
es besser hier auf.

Die Pruefung braucht `dumpbin`, also die Entwicklereingabeaufforderung von
Visual Studio. Fehlt es, wird das gesagt und der Bau laeuft normal weiter.

### Warum sich das Fenster schloss

`pause` stand zwar da, wurde aber je nach Startart uebersprungen. Jetzt wird
geprueft, ob das Fenster ueberhaupt zugehen wuerde:

    echo %cmdcmdline% | find /i "%~nx0" >nul
    if not errorlevel 1 pause

Per Doppelklick startet Windows die Datei mit `cmd /c ...` — dann steht der
Dateiname in der Befehlszeile, und es wird angehalten. Aus einer offenen
Eingabeaufforderung heraus waere das nur laestig, und dann unterbleibt es.

### Sprungmarken statt Klammern

Die Pruefung ist mit `goto`-Marken geschrieben, nicht mit verschachtelten
Klammerbloecken. Batch wertet Bloecke als Ganzes aus, und ein `if errorlevel`
darin liest leicht den falschen Wert. Marken sind laenger, tun aber, was
dasteht.

### tools/check_batch.py

`build.bat` laesst sich hier nicht ausfuehren — dieselbe Lage wie bei
`gui/main_win32.cpp`. Geprueft wird deshalb beim Bauen, was ohne Ausfuehrung
pruefbar ist:

- jedes `goto` hat eine Sprungmarke, jede Marke wird angesprungen
- Klammern sind ausgeglichen (Klammern in `%VAR%`, Zeichenketten und hinter
  `^` zaehlen nicht mit — `%ProgramFiles(x86)%` ist kein Block)
- kein blankes `exit`, das die ganze Eingabeaufforderung beendet und dem
  Nutzer das Fenster vor der Nase zuschlaegt

Gefunden hat es dabei gleich eine tote Sprungmarke, die seit einer frueheren
Aenderung nicht mehr erreichbar war.

---

## 38. Laeuft es auf fremden Rechnern?

Das ist die einzige Frage, die man vor einer Weitergabe wirklich beantworten
muss — und sie soll sich ohne installierte Werkzeuge beantworten lassen.

### Im Programm

**Ansicht -> Ueber g2c**, oder auf der Kommandozeile:

    g2c about

Ausgabe:

    Laufzeit     : fest eingebaut - laeuft auf jedem Windows 10/11
                   ohne Visual C++ Redistributable.

Steht dort stattdessen eine Warnung, startet das Programm auf Rechnern ohne
Redistributable **nicht**, und der Empfaenger kann daran nichts aendern.

Die Auskunft steht zur Uebersetzungszeit fest: MSVC setzt bei `/MD` sowohl
`_MT` als auch `_DLL`, bei `/MT` nur `_MT`.

### Ohne das Programm zu starten

    findstr /m /c:"VCRUNTIME140.dll" g2c.exe

Die Namen der benoetigten DLLs stehen im Klartext in der Exe. Keine Ausgabe
heisst: fest eingebaut. Wird der Dateiname ausgegeben, nicht.

Das geht in **jeder** Eingabeaufforderung — `dumpbin` und die
Entwicklerkonsole von Visual Studio braucht es dafuer nicht.

---

## 39. Wenn sich das Fenster sofort wieder schliesst

Bis hierher gab es dafuer genau eine Ursache und keine Meldung: schlug die
Erzeugung des DirectX-Geraets fehl, beendete sich das Programm
stillschweigend. Fuer den Nutzer ist das von einem kaputten Download nicht
zu unterscheiden.

### Rueckfall auf den Softwarerasterisierer

Versucht werden jetzt nacheinander:

1. die Grafikkarte (`D3D_DRIVER_TYPE_HARDWARE`)
2. **WARP**, der Softwarerasterisierer von Windows

WARP gehoert zu Windows und ist immer vorhanden. Fuer eine Oberflaeche aus
Linien und Text reicht er vollkommen. Damit laeuft g2c auch dort, wo bisher
nichts kam:

- virtuelle Maschinen
- Remotedesktop ohne Grafikweiterleitung
- Rechner mit fehlendem oder veraltetem Grafiktreiber

Zusaetzlich sind die Feature-Level bis 9.1 erweitert, damit auch sehr alte
Grafikkarten bedient werden.

### Und wenn auch das nicht geht

Dann erscheint ein Fenster mit dem Fehlercode und den moeglichen Ursachen —
statt gar nichts. Mit dem Hinweis, dass die Kommandozeile davon unabhaengig
funktioniert.

### Ausnahmen beim Start

Der gesamte Start liegt in einem `try`/`catch`. Eine Ausnahme beim Laden der
Einstellungen oder beim Wiederherstellen der Tabs beendete das Programm
vorher ebenfalls lautlos; jetzt wird sie angezeigt.

### Zum Eingrenzen

    g2c about

Kommt eine Ausgabe, ist die Exe in Ordnung und es liegt an der Grafik.
Kommt nichts, stimmt etwas mit der Datei selbst nicht.

---

## 40. Wenn Virenschutz die Exe blockiert

Das haeufigste Problem beim Weitergeben, und es hat nichts mit dem Programm
zu tun.

Windows Defender arbeitet seit 2015 nicht mehr mit festen Signaturen,
sondern mit einem Vorhersagemodell. Eine unbekannte Datei kann allein wegen
"Verdaechtigkeit" markiert werden, ohne dass sie bekannter Schadsoftware
aehnelt. Unsignierte `.exe`-Dateien werden dabei haeufig direkt geloescht —
und ein Programm, das sich sofort wieder schliesst, ist fuer den Nutzer
davon nicht zu unterscheiden.

### Was dagegen getan ist

**Versionsangaben.** `assets/g2c.rc` enthaelt jetzt einen
`VS_VERSION_INFO`-Block: Produktname, Beschreibung, Version,
Originaldateiname, in Deutsch und Englisch. Eine Exe ohne solche Angaben ist
fuer die Heuristik die unguenstigste Ausgangslage; sie sieht aus wie etwas,
das seine Herkunft verbergen will.

Das ersetzt keine Signatur, kostet aber nichts. Zum Vergleich: Ravens
`carcass.exe` hat ueberhaupt keine Versionsangaben.

`tools/check_win32.py` prueft beim Bauen, dass der Block da und in sich
stimmig ist — fehlende Angaben und unausgeglichene `BEGIN`/`END` faellt sonst
erst `rc.exe` auf.

### Was nur du tun kannst

**Bei Microsoft melden.** Der offizielle und dauerhafte Weg ist die
Einreichung als Falschmeldung ueber das Microsoft-Portal fuer
Sicherheitsanalysen. Das ist kostenlos, und die Freigabe gilt anschliessend
fuer alle Nutzer. Bei jeder neuen Fassung ist eine erneute Einreichung
noetig.

**Signieren.** Ein Zertifikat einer anerkannten Stelle ist die eigentliche
Loesung — es kostet Geld, aber es ist der einzige Weg, der das Problem
grundsaetzlich beseitigt. Fuer ein Werkzeug, das in einem Forum
weitergereicht wird, muss man abwaegen.

**In der Ankuendigung darauf hinweisen.** Ein Satz dazu erspart
Rueckfragen — und Nutzer, die vorgewarnt sind, halten die Meldung nicht fuer
einen Beleg.

### Was der betroffene Nutzer tun kann

- Windows-Sicherheit -> Schutzverlauf: die Datei wiederherstellen
- oder einen Ausschluss fuer genau diesen Ordner eintragen

Beides sind Notloesungen. Ein Ausschluss ist eine Luecke im Schutz und
sollte so eng wie moeglich gefasst und spaeter wieder entfernt werden.

---

## 41. Absturz beim Start: weisses Fenster, dann Ende

Drei Ursachen, eine davon selbst verschuldet.

### 1. Feature-Level unter 10.0 — der eigentliche Fehler

Beim Einbau des WARP-Rueckfallwegs hatte ich die Feature-Level bis 9.1
erweitert, in der Annahme, damit mehr alte Grafikkarten zu bedienen. Das war
falsch.

ImGuis DX11-Anbindung uebersetzt ihre Shader mit **`vs_4_0`**, und das ist
ein Ziel fuer Feature-Level **10.0 aufwaerts**. Auf 9.1 oder 9.3 laesst sich
das Geraet zwar erzeugen — die Shader scheitern aber. Ergebnis: Fenster
erscheint, bleibt weiss, Absturz. Level 9.x braeuchte die Profile
`vs_4_0_level_9_x`, die ImGui nicht benutzt.

Ein Geraet zu erzeugen, das anschliessend nichts zeichnen kann, ist
schlechter als gar keins: bei letzterem erscheint wenigstens die
Fehlermeldung.

Jetzt: 11_0, 10_1, 10_0 — dieselben Level wie in ImGuis eigenem Beispiel,
plus WARP als Rueckfall.

### 2. Treiberneustart

Faellt der Grafiktreiber aus und wird von Windows neu gestartet — bei NVIDIA,
AMD und Intel gleichermassen ueblich, etwa nach einem Treiberupdate im
laufenden Betrieb —, meldet `Present` `DXGI_ERROR_DEVICE_REMOVED` oder
`DEVICE_RESET`. Danach schlaegt **jeder** weitere Aufruf fehl.

Ohne Pruefung: weisses Fenster, dann Absturz. Genau dasselbe Bild wie oben,
aber eine voellig andere Ursache — deshalb wird der Grund jetzt genannt,
samt `GetDeviceRemovedReason`.

### 3. Renderziel ohne Pruefung

`createRenderTarget` gab seinen Fehlschlag nicht zurueck. Blieb `g_rtv`
null, zeichnete `OMSetRenderTargets` ins Leere — weisses Fenster, und der
naechste Zugriff stuerzte ab. Jetzt liefert die Funktion einen Wert, der
ausgewertet wird, und die Schleife zeichnet ohne Renderziel gar nicht erst.

### Was ausdruecklich geprueft wurde und in Ordnung ist

- **Schriftpfade**: ueber `GetWindowsDirectoryW`, nicht `C:\Windows`
  fest verdrahtet. Rueckfall auf die Standardschrift, wenn die Datei fehlt.
- **Keine absoluten Pfade** irgendwo im Quelltext.
- **Landeseinstellungen**: eigener Test (`testLocaleIndependence`), weil
  Zahlen mit Komma statt Punkt sonst alles zerlegen wuerden. Ein tuerkisches
  oder deutsches Windows aendert nichts.
- **Kopieroperationen**: `App` und `BuildJob` sind jetzt ausdruecklich
  unkopierbar und unbeweglich. Ihre Member sind es ohnehin, aber die
  C++-Richtlinien verlangen, die Absicht hinzuschreiben statt sie aus den
  Membertypen ableiten zu lassen.
- **Keine rohen `new`/`delete`**, keine C-Casts.

---

## 42. Warum das Baufenster zuging: Zeilenenden

Der eigentliche Grund, und er ist peinlich.

`build.bat` hatte **382 einzelne LF und kein einziges CRLF** — Unix-
Zeilenenden. Meine Bearbeitungen mit Python hatten die Windows-Zeilenenden
ersetzt, ohne dass es jemandem auffiel.

`cmd.exe` verhaelt sich mit solchen Dateien unzuverlaessig: Sprungmarken
greifen nicht, Befehle laufen zusammen, und ein `pause` am Ende wird
uebersprungen. Das Fenster geht zu, bevor jemand etwas lesen kann.

Und das hatte eine Folge, die schwerer wiegt als das Fenster selbst: **wer
den Ausgang des Baus nicht sieht, weiss nicht, ob er gerade eine neue oder
eine alte Exe testet.**

### Dazu: Bytes ueber 127

In Zeile 344 stand ein Gedankenstrich. Die Codepage einer
Eingabeaufforderung ist nicht vorhersagbar — 850 in Deutschland, 857 in der
Tuerkei, 437 in den USA. Alles ueber 127 erscheint dort als Unsinn oder
schlimmer. `build.bat`, `start_build.bat` und `assets/g2c.rc` sind jetzt
reines ASCII.

### Kuenftig geprueft

`tools/check_batch.py` prueft bei jedem Bau:

- **CRLF ueberall** — sonst Fehler
- **kein Byte ueber 127**
- **kein BOM am Dateianfang**

fuer `build.bat`, `start_build.bat` und `assets/g2c.rc`. Gegenprobe gemacht:
die Pruefung meldet beide Fehler zuverlaessig.

### Lehre

Textdateien fuer Windows mit einem Werkzeug zu bearbeiten, das
standardmaessig LF schreibt, ist eine stille Falle. Sie erzeugt keinen
Uebersetzungsfehler und keine Warnung — nur ein Verhalten, das man nicht
erklaeren kann.

---

## 43. Fenster geht zu, obwohl pause UND cmd /k dastehen

Wenn selbst `cmd /k` das Fenster nicht offen haelt, liegt es **nicht mehr an
der Batchdatei**. Bei Microsoft ist genau dieser Fall dokumentiert: weder
`pause` noch `cmd /k` halfen, waehrend eine einfache Test-Batchdatei normal
lief. Ursache dort war die beschaedigte Dateizuordnung fuer `.bat`.

### Feststellen: 20 Sekunden

Doppelklick auf **`fenster_test.bat`**. Die Datei tut nichts ausser anhalten.

| Ergebnis | Bedeutung |
|---|---|
| Fenster bleibt offen | die Zuordnung ist in Ordnung, das Problem liegt in `build.bat` |
| Fenster geht sofort zu | die Zuordnung ist beschaedigt, `build.bat` ist unschuldig |

### Wenn die Zuordnung beschaedigt ist

`fix_bat_zuordnung.reg` beilegt: Doppelklick, Nachfrage bestaetigen,
abmelden und wieder anmelden. Sie setzt

    HKEY_CLASSES_ROOT\batfile\shell\open\command  =  "%1" %*

auf den Auslieferungszustand und entfernt die benutzerbezogene
Ueberschreibung unter

    HKEY_CURRENT_USER\...\Explorer\FileExts\.bat

Genau die wird von Editoren und Packprogrammen gern gesetzt und sorgt dafuer,
dass `.bat` nicht mehr ausgefuehrt, sondern geoeffnet wird.

Zum Pruefen ohne Registry, in einer Eingabeaufforderung:

    assoc .bat        erwartet: .bat=batfile
    ftype batfile     erwartet: batfile="%1" %*

### Der Weg, der immer funktioniert

Eingabeaufforderung **zuerst** oeffnen, dann die Batchdatei darin aufrufen:

    cd "C:\Pfad\zum\Ordner"
    build.bat

Dann gehoert das Fenster der Eingabeaufforderung und nicht der Batchdatei —
es kann gar nicht zugehen.

---

## 44. Der Mantel um build.bat

Der Fenstertest hat es entschieden: `pause` und `cmd /k` funktionieren auf
dem betroffenen Rechner einwandfrei, die Dateizuordnung ist in Ordnung. Das
Fenster ging trotzdem zu — also brach `build.bat` vorher ab.

**Eine Batchdatei endet sofort und wortlos, wenn ein `goto` auf eine Marke
zeigt, die es nicht gibt.** Kein Fehlercode, keine Meldung, und das `pause`
am Dateiende wird nie erreicht. Zusammen mit einem sich schliessenden
Fenster ist das nicht diagnostizierbar: die Meldung, die den Grund nennt,
verschwindet mit dem Fenster.

Statt weiter zu suchen, ist der Ablauf jetzt so gebaut, dass er nicht
vorzeitig enden **kann**:

    call :main %*
    set "RC=%ERRORLEVEL%"
    ... Ergebnis melden ...
    cmd /k

`call :main` legt einen eigenen Kontext an. Bricht darin etwas ab — ein
fehlendes `goto`, ein `exit /b`, ein beliebiger Fehler —, kehrt die
Ausfuehrung in den Mantel zurueck, statt das Fenster zu schliessen. Nur ein
blankes `exit` kaeme durch, und dass es keines gibt, prueft
`tools/check_batch.py`.

Der Mantel meldet ausserdem den Rueckgabewert, wechselt in den
Ausgabeordner und laesst eine Eingabeaufforderung offen, in der man g2c
sofort ausprobieren kann. `build.bat noshell` beendet sich wie gewohnt.

### Was die Pruefung jetzt sicherstellt

- `build.bat` ruft `:main` ueber `call` auf — sonst Fehler
- ein `cmd /k` ist vorhanden — sonst bliebe das Fenster nicht offen
- beides **zeilenweise** geprueft, nicht im Gesamttext

Der letzte Punkt klingt nach Kleinigkeit, war aber wichtig: die erste
Fassung dieser Pruefung fand `call :main` im **Kommentar** darueber und
meldete deshalb nichts. Eine Pruefung, die auf ihren eigenen Kommentar
hereinfaellt, ist schlimmer als keine.

---

## 45. Absturz auf einem fremden Rechner eingrenzen

Laeuft es auf einem Rechner und stuerzt auf einem anderen ab, hilft Raten
nicht. Der Nutzer sieht nichts: kein Fenster, keine Meldung, und die Hardware
ist unverdaechtig — eine GTX 1060 beherrscht DirectX 11 auf Feature-Level
11_0.

### %APPDATA%\g2c\startup.log

Jeder Schritt wird **vor** seiner Ausfuehrung vermerkt und die Datei sofort
geschlossen. Was zuletzt darin steht, ist der Schritt, der nicht mehr fertig
wurde:

    18:42:01.123  Start
    18:42:01.140  DPI-Bewusstsein setzen
    18:42:01.155  Fensterklasse anmelden
    18:42:01.201  DirectX-Geraet erzeugen
    18:42:01.388  Fenster anzeigen
    18:42:01.390  ImGui-Kontext anlegen
    18:42:01.392  DPI ermitteln
    18:42:01.395  Schriften laden
    18:42:01.410  Einstellungen laden und Skripte wiederherstellen
    18:42:01.520  erstes Bild gezeichnet - ab hier laeuft alles

Steht die letzte Zeile da, war der Start erfolgreich. Fehlt sie, nennt die
vorletzte den Ort.

Die Datei wird bei jedem Start neu angelegt — eine, die endlos waechst,
liest niemand, und nur der letzte Versuch ist interessant.

### g2c -reset

Verwirft `g2c_settings.txt`, `g2c_window.txt` und `imgui.ini` vor dem
Start.

Ohne diesen Ausweg gaebe es keinen, wenn eine beschaedigte
Einstellungsdatei den Start verhindert: das Programm stuerzt bei jedem
Versuch an derselben Stelle ab, und `%APPDATA%\g2c` ist fuer einen Nutzer
nicht auffindbar.

### Nebenbei abgesichert: DPI

`GetDpiScaleForHwnd` liefert normalerweise 1.0 bis 3.0. Ein defekter
Treiber oder eine ungewoehnliche Mehrschirmeinrichtung kann 0 melden — dann
waere die Schriftgroesse 0, ImGui erzeugte keine Glyphen und stuerzte beim
Zeichnen ab, weit weg von der Ursache. Werte ausserhalb 0,1 bis 8,0 werden
jetzt auf 1,0 gesetzt.

### Absturz beim Schriftenladen

Das Startprotokoll eines betroffenen Rechners endete bei „Schriften laden".
Zwischen dieser Marke und der naechsten lagen drei Dinge — `buildFonts`,
`ImGui_ImplWin32_Init` und `ImGui_ImplDX11_Init` —, also war der Ort noch
nicht bestimmt. Jetzt gibt es Marken fuer jeden einzelnen Schritt:

    Schriften laden
      Grundschrift segoeui.ttf
      Symbolschrift
      Zeichensatz aufbauen
    Win32-Anbindung starten
    DirectX-Anbindung starten
    Anbindungen bereit

Ausserdem wird der Rueckgabewert von `io.Fonts->Build()` jetzt ausgewertet.
Schlaegt der Aufbau fehl, wird auf die eingebaute Schrift zurueckgefallen,
statt mit einem leeren Zeichensatz ins Zeichnen zu gehen — dort waere der
Absturz weit weg von seiner Ursache.

**`g2c -nofont`** ueberspringt die Windows-Schriften vollstaendig und
benutzt nur die eingebaute. Sie sind der einzige Teil des Starts, der von
Dateien auf dem fremden Rechner abhaengt; eine beschaedigte oder ersetzte
`segoeui.ttf` ist nicht auszuschliessen.

Windows 10 und 11 unterscheiden sich hier uebrigens: Windows 11 bringt
`SegoeIcons.ttf` mit, Windows 10 nur `segmdl2.ttf`. Beide werden versucht,
und fehlt auch die zweite, werden die Symbole weggelassen statt als leere
Kaesten angezeigt.

### Die Ursache: fehlende Schriftdatei

Das feinere Protokoll endete bei „Symbolschrift". Damit war es gefunden.

**Windows 11 bringt `SegoeIcons.ttf` mit, Windows 10 nicht.** Wir riefen
`AddFontFromFileTTF` trotzdem damit auf und verliessen uns darauf, dass ein
Nullzeiger zurueckkommt. ImGui loest bei einer fehlenden Schriftdatei aber
ein `IM_ASSERT_USER_ERROR` aus — ist die Pruefung aktiv, bricht das Programm
genau dort ab.

Das erklaert das ganze Bild: auf Windows 11 lief es, auf Windows 10 nicht,
und die Hardware hatte damit nie etwas zu tun.

`addFontIfPresent` sieht jetzt mit `GetFileAttributesW` nach, bevor es die
Datei anfasst. Das kostet einen Systemaufruf und nimmt der Sache jede
Abhaengigkeit davon, wie ImGui uebersetzt wurde. Gilt fuer alle Schriften —
Grundschrift, CJK und Symbole.

### Nebenbei: 4608 Zeichen fuer zwanzig Symbole

Angefordert wurde der Bereich `E700`–`F8FF`, also 4608 Zeichen. Benutzt
werden **zwanzig**, alle zwischen `E70F` und `EA39`. Der Zeichensatz legte
fuer den ganzen Bereich eine Nachschlagetabelle an.

Jetzt wird nur der benutzte Ausschnitt angefordert.
`tools/check_win32.py` prueft, dass jedes in `icons.h` definierte Symbol
darin liegt — wer eines ausserhalb hinzufuegt, saehe sonst ein leeres
Kaestchen, und zwar nur auf manchen Rechnern.

---

## 46. Nebenlaeufigkeit: was gemessen wurde und was sich geaendert hat

### Wie parallelisiert wird

Vier Stellen, jeweils **ein** `parallelFor`-Aufruf pro Lauf:

| Stelle | Aufgeteilt nach |
|---|---|
| `carbuild.cpp` | Quelldateien — jede `.xsi` wird gelesen und ausgewertet |
| `carvalidate.cpp` | Quelldateien |
| `gladiff.cpp` | Frames |
| `mdxa.cpp` | Frames beim Komprimieren |

Damit entstehen hoechstens vier Threadsaetze je Programmlauf. Ein Threadpool
waere hier kein Gewinn — er lohnt sich, wenn viele kurze Aufgaben anfallen,
nicht bei vier langen Phasen. Die Ersparnis laege im Bereich von
Millisekunden, der Preis waere ein dauerhaft laufender Satz Threads und
deutlich mehr Zustand.

### Behoben: Threadnummer im thread_local

Beim Komprimieren fuehrt jeder Thread einen eigenen Statistikzaehler. Die
Nummer dafuer stand in einem `thread_local`:

    thread_local int myslot = -1;
    if (myslot < 0) myslot = slot.fetch_add(1) % nThreads;

Ein `thread_local` **ueberlebt den Aufruf**. Laeuft `parallelFor` seriell —
bei einem Kern oder `-threads 1` —, ist der ausfuehrende Thread der
Hauptthread, und dessen Wert steht beim naechsten Bau noch da. Wird dann mit
weniger Threads gearbeitet, zeigt der gespeicherte Index **hinter das Ende
des Zaehlerfeldes**.

In der Oberflaeche ist das erreichbar: mehrere Skripte nacheinander bauen und
zwischendurch die Threadzahl verringern.

Die Nummer kommt jetzt als Parameter von `parallelForWorker`. Ein Test
prueft genau diese Reihenfolge — 8 Threads, dann 4, dann 1, dann 2 — und
schlaegt an, wenn eine Nummer ausserhalb des gueltigen Bereichs auftaucht.

### Geaendert: Arbeit blockweise statt einzeln

Vorher holte sich jeder Thread ein Element je `fetch_add`. Bei 30 384 Frames
zu je wenigen Mikrosekunden schlagen sich acht Threads dabei um dieselbe
Cachezeile.

Jetzt werden Bloecke von bis zu 64 Elementen vergeben — fein genug, dass
ungleich lange Aufgaben sich noch ausgleichen, aber ein Vierundsechzigstel
der Synchronisation. Eine feste Aufteilung waere schlechter: die Dateien sind
sehr unterschiedlich gross.

### Gemessen

**Keine Datenrennen.** ThreadSanitizer ueber die gesamte Testsuite und ueber
einen echten Bau mit acht Threads, mit und ohne Cache: **0 Meldungen**.

**Determinismus haelt.** Derselbe Bau mit 1, 3 und 8 Threads, mit und ohne
Cache: **eine** Pruefsumme.

Echte Beschleunigungszahlen kann ich hier nicht liefern — die
Entwicklungsumgebung hat einen Kern. Wer messen will: `-threads 1` gegen
`-threads N` auf demselben Skript.

### Was NICHT geaendert wurde und warum

**Kein `std::execution::par`.** Die Parallel-Algorithmen der
Standardbibliothek verlangen auf GCC und Clang zwingend Intel TBB. Fuer ein
Werkzeug, das mit nichts als einem C++20-Compiler bauen soll, ist das ein zu
hoher Preis.

**Der Poolaufbau bleibt seriell.** Die Reihenfolge der Eintraege bestimmt die
Datei; eine parallele Variante waere nicht mehr reproduzierbar. Determinismus
ist hier mehr wert als der Zeitgewinn.

**Kein eigener Speicherzuteiler.** Das Lesen der `.xsi` belegt viel Speicher,
und ein skalierender Zuteiler waere messbar — aber er brauchte eine
zusaetzliche Bibliothek, und der Bau soll ohne auskommen.

---

## 47. Zwei Fehler beim Uebernehmen von JK2-Animationen

Gemeldet wurde: die neu gebaute GLA spielt im Spiel falsche Animationen ab —
statt Gehen eine JK2-Sequenz. Carcass meldete ausserdem fuer jeden Bone
"Basepose for bone ... differs". Zwei getrennte Ursachen.

### 1. Die animation.cfg im Spiel ist die alte

Das ist die Ursache der falschen Animationen, und sie liegt nicht im
Werkzeug.

Werden Animationen in eine `.car` eingefuegt, **verschieben sich alle
nachfolgenden Zielframes**. Gemessen an der gemeldeten Datei:

| | Original-JKA | neu gebaut |
|---|---|---|
| Frames gesamt | 30384 | 54190 |
| `BOTH_WALK1` beginnt bei | 30293 | **48289** |
| bei Frame 30293 liegt jetzt | `BOTH_WALK1` | **`BOTH_CIN_55`** |

Die Engine schlaegt in `animation.cfg` nach, welcher Frame zu welchem Namen
gehoert. Liegt im Spielordner noch die alte Datei, zeigt jeder Eintrag ins
Leere — und bei 30293 laeuft eben das, was dort jetzt steht.

**Die neu erzeugte `animation.cfg` muss zusammen mit der `.gla` kopiert
werden.** Sie gehoeren zusammen wie Schluessel und Schloss; eine `.gla` ohne
ihre `.cfg` ist unbrauchbar.

Das gilt auch fuer jedes andere Modell, das auf dieselbe `_humanoid.gla`
zeigt: alle brauchen die neue `animation.cfg`.

### 2. SI_Transform trug die Pose von Frame 0

Das war ein echter Fehler im Exporteur.

Carcass liest den `SI_Transform`-Block einer `.xsi` als **Bindepose**,
verkettet ihn ueber die Hierarchie und vergleicht das Ergebnis mit dem
Zielskelett. Wir schrieben dort die animierte Pose von Frame 0 — daher fuer
jeden Bone eine Meldung, bei `lower_lumbar` etwa um 41,35, was exakt der
Z-Hoehe seiner Bindepose entspricht.

Der eigene Importeur nahm die Werte nie: er wertet die `SI_FCurve`-Kurven
aus, und die waren immer richtig. Deshalb blieb der Fehler unbemerkt, bis
jemand die exportierte Datei durch Ravens Carcass schickte.

Jetzt steht dort die tatsaechliche Bindepose — lokal gegen den Elternbone,
in dotXSI-Koordinaten. Ein Test wertet eine Kopie der Datei ohne FCurves
aus, sodass der SRT-Rueckfall greift, und vergleicht das Ergebnis mit dem
Skelett: Abweichung **0**.

**Nicht durch die Skelettskalierung teilen.** Der erste Anlauf tat das und
schrieb Skalierung 1,5625 statt 1,0 und Translation 14,0625 statt 9,0 — die
Skalierung steckt in jeder Bindepose und kuerzt sich beim Bilden der
lokalen Pose (Eltern^-1 mal Kind) heraus.

Gegengeprueft an Ravens eigener : dort steht fuer
 exakt Skalierung 1,0 und Translation 9,0 — genau das, was die
lokale Bindepose ergibt.

Der Rundlauf ist davon unberuehrt: dieselben 2,010 % wie vorher, weil die
Kurven sich nicht geaendert haben.

### Die animation.cfg hiess falsch

Nachtrag zum vorigen Abschnitt, und der eigentliche Grund, warum der Fehler
ueberhaupt auftreten konnte.

Auf der Kommandozeile hiess die erzeugte Datei **`<name>_animation.cfg`** —
bei `_humanoid.gla` also `_humanoid_animation.cfg`. Sie ueberschrieb damit
nichts und lag harmlos daneben.

Die Engine sucht aber **`animation.cfg`**. Wer den Ausgabeordner ins Spiel
kopierte, hatte weiterhin die alte, und die zeigte auf die alten Zielframes.

Jetzt heisst sie `animation.cfg`, eine vorhandene wird vorher als `.bak`
gesichert, und beim Bauen erscheint:

    WICHTIG: animation.cfg gehoert ZUSAMMEN mit der GLA kopiert.
             Die Zielframes haben sich gegenueber der alten Fassung
             verschoben. Bleibt die alte Datei im Spiel liegen, wird bei
             jedem Namen die Animation abgespielt, die dort zufaellig steht.
             Das betrifft auch jedes andere Modell, das dieselbe GLA nutzt.

Die Oberflaeche schrieb schon immer `animation.cfg` — nur die Kommandozeile
wich ab. Genau solche Unterschiede zwischen zwei Wegen ins selbe Ergebnis
sind es, die man nicht bemerkt.

---

## 48. Bauen speichert jetzt, und Ordnerkollisionen werden gemeldet

### Die .car wird vor dem Bauen gespeichert

Gebaut wurde der Zustand im **Speicher**, waehrend die Datei auf der Platte
die alte blieb. Wer das Programm danach schloss, hatte eine GLA, die zu
keiner `.car` mehr passte — und beim naechsten Oeffnen war die Aenderung
weg.

Die Datei ist das, was gebaut wurde. Beides auseinanderlaufen zu lassen ist
in keinem Fall richtig. Geaenderte Skripte werden jetzt vor dem Bauen
gespeichert, mit Sicherung wie beim Speichern von Hand, und das Protokoll
nennt es.

### Wird fuer jede GLA eine eigene animation.cfg geschrieben?

**Ja.** Der Ausgabeordner gehoert zum Dokument (`d.outputDir`), nicht zum
Programm — genau deshalb gibt es keinen gemeinsamen Ordner und den Knopf
"Alle auf g2c_out neben der .car" statt eines einzelnen Feldes.

Jedes Skript schreibt `<name>.gla`, `animation.cfg`, `<name>.frames`,
`<name>.glm` und `<name>.skin` in **seinen** Ordner.

**Aber:** setzt man zwei Skripte auf denselben Ordner, ueberschreiben sie
sich. Und dabei ist die `animation.cfg` das gefaehrlichere Stueck — sie
gehoert danach zur zweiten GLA, waehrend die erste weg ist. Im Spiel sieht
das aus wie vertauschte Animationen, und die Ursache ist von aussen nicht
erkennbar.

Das wird jetzt vor dem Bauen gemeldet:

    _humanoid.car und _humanoid_jk2.car schreiben in denselben Ordner -
    die zweite GLA ueberschreibt die erste.

Nur eine Warnung, kein Abbruch: es gibt Faelle, in denen genau das gewollt
ist.

### Und nach jedem Bau

    animation.cfg gehoert ZUSAMMEN mit der GLA kopiert - sonst spielt das
    Spiel bei jedem Namen die Animation ab, die dort zufaellig steht.


### Nachtrag: die Wurzel

Nach der Korrektur meldete Carcass zusaetzlich

    Basepose for bone "model_root" differs by 0.230400

und 0,2304 ist genau 0,64 minus 0,64 mal 0,64.

Carcass multipliziert die gelesene Ruhelage selbst mit der
Skelettskalierung. Bei allen Bones ausser der Wurzel kuerzt sie sich beim
Bilden der lokalen Pose heraus — die Wurzel hat keinen Elternbone, dort
blieb sie stehen und wurde ein zweites Mal angewandt.

Ravens eigene Dateien bestaetigen es: fuer  steht dort Skalierung
1,0, nicht 0,64.

Der 3x3-Teil der Wurzel wird jetzt durch die Skalierung geteilt.

**Der zugehoerige Test war zu schwach.** Er verglich nur die Translation —
und die ist an der Wurzel null, weshalb der Fehler durchging. Jetzt werden
Translation und Spaltenlaengen geprueft.

---

## 49. Protokoll kopieren

Ueber dem Protokoll: **Protokoll kopieren**, **Leeren**, und die Zahl der
Zeilen.

Kopiert wird nicht nur der sichtbare Text, sondern ein vollstaendiger
Fehlerbericht:

    g2c - Protokoll
    Gebaut: Aug  6 2026 09:12:44, 64 Bit, 8 Kerne
    Laufzeit: fest eingebaut
    Assetwurzel: C:\jka_animations\md\base
    Referenz-GLA: _humanoid.gla
    Skripte offen: 25
    ----------------------------------------
    [OK]      _humanoid.car geoeffnet (1386 Grabs)
    [WARNUNG] animation.cfg gehoert ZUSAMMEN mit der GLA kopiert
    [FEHLER]  Lesefehler in "both_cin_19.xsi"

Zwei Dinge sind dabei wichtig:

**Der Kopf.** Fassung, Adressbreite, Kerne, Laufzeitbindung, Assetwurzel,
Referenz-GLA. Ein Fehlerbericht ohne diese Angaben kostet immer eine
Rueckfrage.

**Die Art vor jeder Zeile.** Die Farbe geht beim Kopieren verloren, und
gerade sie unterscheidet Hinweis von Fehler.

Ein Speichern-Knopf fehlt bewusst: die Plattformschicht hat keinen
Speicherdialog, und einen zu ergaenzen hiesse, in `gui/main_win32.cpp` zu
arbeiten — der einzigen Datei, die sich hier nicht uebersetzen und testen
laesst. Die Zwischenablage tut dasselbe.

## 50. Templatenamen mit Leerzeichen

Im selben Protokoll fielen fuenf Dateien auf:

    Lesefehler in "both_cin_19.xsi": dotXSI Zeile 10, Spalte 37:
    '{' erwartet nach Template "SI_Scene"

Softimage schreibt Szenennamen mit **Leerzeichen und Punkten**. Der Parser
erwartete genau ein Token als Namen und brach dort ab — die Spaltenangabe
wechselte je nach Laenge des Namens: 29, 37, 52.

Jetzt wird alles bis zur oeffnenden Klammer als Name genommen. Eine Grenze
von 64 Token verhindert, dass eine Datei ohne Klammer alles Nachfolgende
verschluckt; ein Test prueft beide Seiten.

Fuenf von 1400 Dateien fielen so aus, und zwar in den Zwischensequenzen —
also genau dort, wo man es im Spiel zuletzt bemerkt.

---

## 51. Der BASEPOSE-Block

Die eigentliche Ursache der "Basepose ... differs"-Meldungen, und sie lag
tiefer als vermutet.

**Ravens `root.xsi` hat pro Bone ZWEI Transform-Bloecke:**

    SI_Model MDL-Davinci_Male01_TrueBones.lfemurYZ {
        SI_Transform BASEPOSE-Davinci_Male01_TrueBones.lfemurYZ { ... }
        SI_FCurve { ... }
    }

`SRT-` traegt die Pose, **`BASEPOSE-` die Bindepose** — und Carcass liest
den zweiten. Wir schrieben ihn ueberhaupt nicht, weshalb Carcass fuer jeden
Bone eine Abweichung meldete.

Das erklaert auch, warum die Zahlen ueber drei Fassungen hinweg **identisch**
blieben, obwohl ich zweimal am `SRT-`-Block gearbeitet hatte: er war nie das,
was Carcass ansah.

### Der Inhalt

Die **Welt**-Bindepose in dotXSI-Koordinaten, geteilt durch die
Skelettskalierung — nicht die lokale.

Gegengeprueft an Ravens eigener Datei, alle neun Zahlen:

| | Raven | g2c |
|---|---|---|
| Skalierung | 1.0 1.0 1.0 | 1.000000 1.000000 1.000000 |
| Rotation | 90.174767 3.982727 -74.993988 | 90.174767 3.982727 -74.993988 |
| Translation | 5.643987 55.604065 0.322196 | 5.643987 55.604065 0.322196 |

Ein Test prueft, dass je Bone genau ein BASEPOSE und ein SRT geschrieben
wird und dass BASEPOSE zuerst kommt, wie bei Raven.

### Was ich daraus mitnehme

Drei Anlaeufe gingen an der falschen Stelle vorbei, weil ich die Meldung
gedeutet statt die Originaldatei angesehen habe. Die entscheidende
Information stand die ganze Zeit in `root.xsi` — es hat nur niemand
nachgeschaut.

Bei einem Format ohne Spezifikation ist die Originaldatei die Spezifikation.

---

## 52. Ein-Frame-Sequenzen

Carcass weist eine `.xsi` mit nur einem Frame ab:

    XSI file-format doesn't support 1-frames files 100% legally,
    re-export this file please!

Ravens eigene **58 Ein-Frame-Sequenzen** sind denn auch alle
`-additional`-Unterbereiche laengerer Dateien; einzelne Ein-Frame-Dateien
gibt es dort nicht.

Beim Export wird der Frame deshalb **verdoppelt**: zwei identische Frames,
Framebereich 0..1. Die Animation aendert sich dadurch nicht — ein Test
prueft, dass beide Frames bitgleich sind.

Wer das Skript ueber **„Alles + .car"** erzeugt, merkt davon ohnehin nichts:
dort werden Ein-Frame-Sequenzen als `-additional` in ihre laengere
Ursprungsdatei eingebettet, genau wie bei Raven. Der Rundlauf ueber JK2
bleibt entsprechend bei 17278 Frames.

Betroffen ist nur, wer eine solche Sequenz **einzeln** exportiert. Ohne die
Verdopplung liesse sie sich mit Carcass gar nicht bauen.

---

## 53. -basepose: welche Bindepose will Carcass?

Carcass meldet bei unseren Dateien

    Bone "lfemurYZ" had non-uniform scaling: X=0.410 Y=0.411 Z=0.411

und **0,64 mal 0,64 ist 0,4096** — die Skalierung wird zweimal angewandt.

Der BASEPOSE-Block enthaelt die **Welt**-Bindepose. Das entspricht Ravens
`root.xsi`, dort stimmen alle neun Zahlen exakt. Trotzdem sieht es so aus,
als verkette Carcass die Bloecke zusaetzlich ueber die Hierarchie.

Bei Raven faellt das nicht auf, weil dort **Gruppierungsknoten** dazwischen
liegen (`lleg_root`, `rd1root` ...), deren Bindepose neutral ist. Unsere
Dateien haben die nicht, weil die GLA sie nicht kennt.

Welche Variante richtig ist, kann nur Carcass beantworten. Statt zu raten
sind alle drei erzeugbar:

    g2c export <gla> -cfg animation.cfg -o out\ -basepose world   (Vorgabe)
    g2c export <gla> -cfg animation.cfg -o out\ -basepose local
    g2c export <gla> -cfg animation.cfg -o out\ -basepose none

| | |
|---|---|
| `world` | Weltpose, wie in Ravens Dateien |
| `local` | gegen den Elternbone — falls Carcass selbst verkettet |
| `none` | gar kein Block, wie vor der Korrektur |

Ein Test prueft, dass sich die drei **tatsaechlich unterscheiden**. Ein
Schalter, der nichts aendert, waere schlimmer als keiner: man probiert
dreimal dasselbe und haelt das Ergebnis fuer eine Antwort.

### Nebenbei gefunden

`-keepmotion` stand gar nicht in der Argumentschleife — eine frueher
eingefuegte Zeile war ins Leere gelaufen und dort nie angekommen. Beim
Einbau von `-basepose` fiel es auf.

### In der Oberflaeche

Im Modus **GLA -> XSI**, neben den Exportknoepfen: **Bindepose** mit den
Werten *Welt*, *Lokal* und *Keine*.

Sie steht dort und nicht in den Einstellungen, weil sie nur den Export
betrifft — wer sie braucht, sucht sie bei den Exportknoepfen.

Die Wahl bleibt ueber Sitzungen erhalten (`basepose=` in
`g2c_settings.txt`). Wer sie einmal umstellen musste, will das nicht bei
jedem Start wiederholen.

Der Hinweistext sagt ausdruecklich, dass die Einstellung **nur fuer Carcass**
zaehlt: g2c selbst baut mit jeder Variante gleich, weil es die FCurves
auswertet und den BASEPOSE-Block gar nicht ansieht.

### Der Test hat entschieden: Welt ist richtig

Mit `-basepose local` kamen die "Basepose ... differs"-Meldungen zurueck,
mit `world` blieben sie weg. Die Weltpose ist es also, wie Ravens Dateien
auch zeigen. Die Vorgabe bleibt.

### Verbleibend: 0,64 mal 0,64

Was bleibt, ist

    Bone "lfemurYZ" had non-uniform scaling: X=0.410 Y=0.411 Z=0.411

und 0,4096 ist genau 0,64 zum Quadrat. Unsere Werte sind dabei exakt
`1.000000` — sowohl in den SCALING-Kurven als auch im BASEPOSE-Block. Die
Ungleichheit entsteht erst in Carcass' eigener Rechnung.

Der naechste Verdacht: das Skript enthaelt **`$scale 0.64`**, und die
BASEPOSE-Werte sind bereits durch dieselbe Zahl geteilt. Wendet Carcass
beides an, wird zweimal skaliert.

    g2c export <gla> -cfg animation.cfg -o out\ -car out\_humanoid.car -noscale

laesst `$scale` weg. Ob das die Ursache ist, kann nur ein Durchlauf durch
Carcass zeigen — hier laesst es sich nicht ausfuehren.

### Die Wurzel bekommt keinen BASEPOSE-Block

Der Vergleich mit Ravens Dateien hat es entschieden — und zwar durch bloszes
Zaehlen:

| Datei | SRT | BASEPOSE |
|---|---|---|
| `root.xsi` | 277 | 274 |
| `both_A1_special.xsi` | 98 | 95 |
| `BOTH_turn_left1.xsi` | 13 | 10 |
| `both_strafe_left1.xsi` | 186 | 183 |
| **unsere Fassung** | **72** | **72** |

Durchweg genau **drei** SRT mehr als BASEPOSE. Nachgesehen, welche fehlen:
`model_root`, `mesh_root` und `skeleton_root` — die Wurzelknoten.

Wir schrieben fuer `model_root` einen, und Carcass verkettete ihn mit allem
darunter. Daher "non-uniform scaling" mit Werten um 0,4096, also 0,64 zum
Quadrat.

`mesh_root` und `skeleton_root` gibt es bei uns nicht; die GLA kennt sie
nicht. Deshalb ist die Differenz bei uns genau **eins**.

### Und `$scale` war es nicht

Die Vermutung, `$scale 0.64` im Skript wuerde doppelt wirken, war falsch:
Ravens eigenes `_humanoid.car` hat die Zeile ebenfalls. Gut, dass die Datei
vor dem naechsten Umbau kam.

Zum dritten Mal in dieser Sache hat die Originaldatei die Antwort gegeben und
nicht die Fehlermeldung. Bei einem Format ohne Spezifikation ist das
offenbar die einzig verlaessliche Quelle.

---

## 54. Der GLA-Name im Kopf — die eigentliche Ursache

Gefunden hat es der Nutzer mit einem Hex-Editor, nicht ich mit Rechnen.

Jede gebaute GLA trug im Kopf

    2LGA....models/players/_humanoid/_humanoid

**egal wohin sie gehoerte.** Der Name kam aus der Referenz-GLA, weil das
Skelett von dort uebernommen wird — und mit ihm sein Name.

Der Name ist keine Beschriftung. Er sagt der Engine, **welches Skelett das
ist**. Ein eigener Humanoid unter `_humanoid_bdroid` meldete sich damit als
der Standard-Humanoid. Die Engine nahm dessen `animation.cfg` und spielte an
jeder Stelle die Animation ab, die dort zufaellig stand.

Genau danach sah es aus — und genau deshalb war es an den Animationen selbst
nie zu finden. Wochenlang.

Der Name kommt jetzt aus **`-makeskel`**, wie schon beim erzeugten Skript:

    GLA-Name  : models/players/_humanoid_bdroid/_humanoid
                (aus -makeskel; die Referenz heisst "models/players/_humanoid_jk2/_humanoid")

Weicht er von der Referenz ab, wird das gemeldet. Fehlt `-makeskel`, bleibt
es beim Namen der Referenz.

### Was das ueber die Suche sagt

Drei Runden gingen an Carcass-Meldungen, eine an `$scale`, eine an die
Bindepose. Alle fuenf betrafen echte Fehler und waren richtig zu beheben —
aber keiner davon war die Ursache fuer das, was im Spiel passierte.

Die Ursache war ein Name in Byte 8 der Datei. Sichtbar in zehn Sekunden mit
einem Hex-Editor, unsichtbar in jeder Fehlermeldung.

---

## 55. Sicherungsdateien

Die `.bak` der **animation.cfg** ist weg. Sie war eine Fehlannahme: ich
hatte sie eingebaut, als ich das Ueberschreiben fuer die Ursache der
falschen Animationen hielt. Tatsaechlich war es der GLA-Name — und eine
Sicherung einer Datei, die bei jedem Bau vollstaendig neu entsteht, ist
ohnehin wertlos.

Fuer **`.car`-Dateien** bleibt sie, aber abschaltbar:
**Einstellungen -> Sicherung der .car anlegen**.

Sie entsteht nur beim **ersten** Speichern je Datei. Der Stand vor der
ersten Bearbeitung ist das, was man zurueckhaben will — nicht der von
vorhin, der schon die Haelfte der Aenderungen enthaelt.

## 56. ImGui 1.92

Eingebunden ist **1.92.1**. Der Wechsel von 1.91.5 lohnt sich fuer genau
eine Sache: **Mehrsprachigkeit**.

Seit 1.92 laedt der Zeichensatz Glyphen **bei Bedarf** nach, sofern das
Backend  unterstuetzt — das
DX11-Backend tut das seit Juni 2025. Aus der offiziellen Beschreibung:

> Users of icons, Asian and non-English languages do not need to pre-build
> all glyphs ahead of time. Saving on loading time, memory, and also
> reducing issues with missing glyphs.

Das trifft uns direkt: bisher wurden fuer Chinesisch 2500 und fuer Japanisch
1946 Ideogramme im Voraus gerastert, bei jedem Sprachwechsel neu. Genau in
diesem Code ist heute der Absturz beim Schriftenladen entstanden.

### Was der Umbau gekostet hat

Nichts. Geprueft, bevor umgestellt wurde:

- **Unser Fontcode uebersetzt unveraendert** gegen 1.92 — dieselben Aufrufe,
  dieselben Ergebnisse.
- ,  und
   **existieren weiterhin**. Sie sind als veraltet
  markiert, aber funktionsfaehig.
-  benutzen wir gar nicht — der grosse Breaking Change von 1.92
  (zweiter Parameter) trifft uns nicht.
- **403 Pruefungen** laufen gegen 1.92 unveraendert durch.

### Die Bereichsangaben bleiben stehen

Sie sind jetzt nur noch ein Hinweis und keine Bedingung mehr. Sie kosten
nichts, und sollte das Programm je auf einem Rechner ohne die
Backend-Unterstuetzung laufen, sind sie der Unterschied zwischen lesbarem
Text und leeren Kaestchen.

### Vollstaendig auf 1.92

Die `GetGlyphRangesXXX()`-Aufrufe sind entfernt. Sie waren seit 1.92 nur
noch veraltet mitgeschleppt; jetzt laedt der Zeichensatz nach, was
tatsaechlich angezeigt wird.

Fuer Chinesisch wurden bisher 2500 und fuer Japanisch 1946 Ideogramme im
Voraus gerastert — bei **jedem** Sprachwechsel neu. Genau in diesem Code ist
der Absturz beim Schriftenladen entstanden.

## 57. Farbige Symbole

ImGui zeichnet den Text eines Knopfes einfarbig, also auch ein Symbol
darin. Deshalb zeichnet **iconButton** den Knopf zuerst leer und setzt
Symbol und Text anschliessend mit der Zeichenliste darauf — Rahmen und
Hover-Verhalten bleiben die von ImGui, das Symbol bekommt seine eigene
Farbe.

Dasselbe fuer Text ohne Knopf: **iconText**.

### Wie die Farben vergeben sind

Nach **Bedeutung**, nicht nach Geschmack — sonst waere es Dekoration:

| Farbe | wofuer |
|---|---|
| gruen | erzeugt etwas: Bauen, Alles + .car |
| blau | oeffnet oder speichert: Speichern, XSI hinzufuegen |
| violett | betrifft ALLE: Ordner oeffnen, XSI zu allen, Vergleichen |
| gelb | pruefen und warnen: Validieren, fehlt |
| rot | Fehler |
| grau | vorhanden, unauffaellig |

Die drei Modusknoepfe sind gruen, violett und blau. Beim **aktiven** Modus
bleibt das Symbol weiss: auf dem blauen Grund waere eine zweite Farbe
unruhig, und welcher Modus laeuft, sagt schon der Hintergrund.

### Wo bewusst keine Farbe ist

Menueeintraege und Kontextmenues. Dort steht viel untereinander, und
gefaerbte Symbole in einer langen Liste ergeben ein Muster, das vom Text
ablenkt statt zu fuehren.

 prueft, dass die Farbwerte im Bereich 0..1 liegen —
ImGui erwartet keine 0..255, und ein Wert daneben wird nur geklemmt.

---

## 58. Startprotokoll neben der Exe

Es heisst jetzt **`g2c-startup.log`** und liegt **neben `g2c.exe`**. Dort
sucht man es, und dorthin laesst sich jemand verweisen, ohne ihm `%APPDATA%`
erklaeren zu muessen.

**Mit Rueckfall.** Liegt das Programm unter „Programme", in einem
Netzwerkpfad oder auf einem schreibgeschuetzten Medium, schlaegt das
Schreiben fehl — dann geht es nach `%APPDATA%\g2c\startup.log` wie bisher.
Ohne diesen Rueckfall gaebe es gerade auf den Rechnern kein Protokoll, auf
denen etwas schiefgeht.

Der Test ist ein **Schreibversuch**, keine Rechtepruefung: unter Windows sagt
die Rechtelage allein nicht zuverlaessig, ob eine Datei entsteht
(Virtualisierung, Richtlinien, Virenschutz).

Wo es tatsaechlich liegt, sagt das Programm selbst:

- `g2c about` auf der Kommandozeile
- **Ansicht -> Ueber g2c**, dort Klick auf den Pfad kopiert ihn
- in der Fehlermeldung, wenn die Grafik nicht startet

Die Oberflaeche ermittelt den Pfad **nicht selbst**, sondern bekommt ihn von
der Plattformschicht. Sonst koennten Auskunft und tatsaechlich beschriebene
Datei auseinanderlaufen.

### Ein Fehler in der eigenen Pruefung

`tools/check_win32.py` meldete daraufhin einen Aufruf
`startupLogPath()` „mit 1 Argument". Den gab es nicht: bei
`startupLogPath().c_str())` fing der Ausdruck `).c_str(` als Argumentliste
ein — gleich viele Klammern, aber in falscher Reihenfolge.

Geprueft wird jetzt die **Schachtelung**, nicht die Anzahl. Gegenprobe
gemacht: echte Argumentfehler werden weiterhin gefunden.
