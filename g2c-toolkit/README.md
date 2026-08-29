# g2c — Ghoul2-Toolkit, Neuimplementierung

Ein Neuaufbau des Kerns von Ravens `carcass.exe` (v2.2, 2003) in modernem C++.
Entstanden aus einer statischen Analyse des Originalbinaries; das Dateiformat
selbst stammt aus Ravens 2013 unter GPLv2 freigegebenem `mdx_format.h` und
`matcomp.cpp`.

Vollstaendig ausgabekompatibel mit der unveraenderten Jedi-Academy-Engine.
Kein Byte des Formats wurde geaendert.

## Bauen

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/g2_tests
```

Braucht nur einen C++20-Compiler. Keine externen Abhaengigkeiten.

## Was drin ist

| Komponente | Status |
|---|---|
| Quantisierung (`compress.h`) | vollstaendig, mit Fixes fuer B2/B3 |
| GLA schreiben (`mdxa.h`) | vollstaendig, inkl. Pool-Deduplizierung |
| GLA lesen | vollstaendig, mit Strukturpruefung |
| GLM schreiben (`mdxm.h`) | vollstaendig, inkl. Vertex-Weight-Packing |
| Zwischenrepraesentation (`model.h`) | vollstaendig |
| CLI (`g2c info` / `g2c check`) | vollstaendig |
| dotXSI-Parser (`xsi.h`) | vollstaendig, 109 MB/s |
| dotXSI-Import (`xsi_import.h`) | Skelett, Mesh, Animation — gegen echte Assets geprueft |
| `.car`-Skriptparser (`carscript.h`) | vollstaendig, inkl. `$include` |
| Stripifier | nicht enthalten (`-nostrips` ist ohnehin der bessere Weg) |

Der dotXSI-Parser liefert den vollstaendigen Templatebaum jeder Datei. Was noch
fehlt, ist die semantische Schicht: welches Template welchen Bone, welches Mesh
und welche Gewichtung bedeutet. Das laesst sich ohne echte Beispieldateien
nicht seriös festlegen, weil Ravens Max-Exporter eigene Konventionen benutzt.

`g2c xsi <datei>` gibt genau die Information aus, die dafuer noch fehlt.

## Gemessene Ergebnisse

Auf 200.000 zufaelligen Rotationen, Fehler in Grad gegenueber der exakten
Zielrotation:

| Modus | mittl. Fehler | max. Fehler | Zeit |
|---|---|---|---|
| Carcass (Truncation) | 0,00426° | 0,02156° | 32 ms |
| Nearest (nur Rundung) | 0,00213° | 0,01045° | 33 ms |
| Nearest + Optimierung | 0,00202° | 0,00609° | 355 ms |

Die reine Rundungskorrektur ist **gratis** und halbiert den Fehler exakt. Die
Kandidatensuche kostet Faktor 10 in der Kompression und verbessert vor allem
den Worst Case — und genau der ist bei Jitter relevant, nicht der Mittelwert.
Bei einer `_humanoid.gla` mit 72 Bones und 21.000 Frames sind das rund 2,7
Sekunden fuer die Kompression; abschaltbar ueber `CompressOptions::optimizeQuat`.

Alle drei Modi erzeugen gueltige, von der Stock-Engine ladbare Dateien.

## dotXSI-Parsertempo

Auf einer synthetischen 1,56-MB-Datei mit 217 Templates, ein Kern:

```
Parsezeit : 15.3 ms  (102.4 MB/s)
```

Handgeschriebener Zeichenscanner ueber einen zusammenhaengenden Puffer, kein
regex, kein istream, Zahlenkonvertierung ueber `std::from_chars` (auch
Locale-fest — kein Dezimalkomma-Problem). Der Parser im Original liegt bei
`0x448f90`, hat 3.385 Instruktionen und Schleifenverschachtelung 6.

## Benutzung als Bibliothek

```cpp
#include "g2/mdxa.h"

g2::Skeleton skel;
skel.name = "models/players/_humanoid/_humanoid";
skel.scale = 1.0f;
skel.bones.push_back({"model_root", -1, g2::Mat3x4::identity(), 0});
// ... weitere Bones

g2::AnimationFrames frames;
frames.resize(/*frames*/ 100, /*bones*/ (int)skel.bones.size());
frames.at(0, 0) = someMatrix;

const auto result = g2::writeMdxa(skel, frames);
// result.data      -> Bytes der GLA
// result.stats     -> Klemmungen, nicht normierte Quaternionen etc.
// result.poolEntries, result.dedupeRatio()
```

Fehler werfen Exceptions mit konkreter Meldung, statt eine Warnung auf die
Konsole zu schreiben und mit kaputten Daten weiterzulaufen — das ist der
Hauptunterschied im Verhalten gegenueber dem Original.

## CLI

```sh
g2c info  _humanoid.gla    # Header, Skeletthierarchie, Poolstatistik
g2c check _humanoid.gla    # Struktur pruefen, Requantisierungsfehler messen
```

## Lizenz und Herkunft

Das Binaerformat ist Ravens Werk. `mdx_format.h` und `matcomp.cpp` stehen unter
GPLv2 (Copyright Raven Software / Activision / OpenJK-Beitragende). Dieser Code
implementiert das Format neu und uebernimmt keinen Raven-Quellcode; die
Feldreihenfolgen in `include/g2/format.h` sind zwangslaeufig identisch, weil sie
das On-Disk-Layout beschreiben.

Wer daraus etwas veroeffentlicht, sollte die GPLv2-Frage vorher klaeren.
