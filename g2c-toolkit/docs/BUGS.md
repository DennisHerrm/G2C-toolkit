# Die Fehler in Carcass v2.2 und was davon behebbar ist

Adressen beziehen sich auf das analysierte Binary (Imagebase `0x400000`).

## Wichtige Korrektur zu B1

In der ersten Analyse hatte ich B1 — den verschenkten Wertebereich — als den
wahrscheinlichsten Jitter-Verursacher und als behebbar eingestuft. **Nach
Einsicht in `matcomp.cpp` stimmt der zweite Teil nicht.**

`MC_UnCompressQuat` in der Engine dekodiert fest mit:

```c
w = *pwIn++;  w /= 16383.0f;  w -= 2.0f;
```

Der Bereich −2…+2 steckt also auf **beiden** Seiten. Ein Compiler, der ihn auf
−1…+1 aendert, produziert Dateien, die die Stock-Engine falsch dekodiert. Das
ginge nur mit einer neuen GLA-Version und einer Engine-Aenderung, also nur im
OpenJK-Umfeld, nicht fuer die Originalspiele.

Der Befund selbst bleibt richtig: das Format verschenkt ein volles Bit, weil
Einheits-Quaternionen nie ueber ±1 hinausgehen. Es ist aber ein Fehler *im
Format*, kein Fehler im Compiler. `g2c` behaelt den Bereich bei.

## B2 — Truncation statt Rundung (behebbar, gratis)

`SquashFloat` bei `0x4181d0`, `SquashXlatFloat` bei `0x4182b0`, beide enden in
`_ftol` bei `0x450fb0`. `_ftol` schneidet Richtung Null ab.

Da vorher konstant `+2.0` bzw. `+512.0` addiert wird, ist der Operand immer
positiv. Der Rundungsfehler zeigt damit **konsistent in eine Richtung** statt
symmetrisch zu streuen: gerichtete Drift statt Rauschen. Genau so sieht Jitter
in Animationen aus.

Messung ueber 200.000 Werte:

```
mittl. |Fehler| Legacy  : 3.045e-05  (0.50 Stufen)
mittl. |Fehler| Nearest : 1.527e-05  (0.25 Stufen)
mittl. Bias Legacy      : -3.045e-05   <-- volle halbe Stufe, einseitig
mittl. Bias Nearest     : -4.784e-09   <-- praktisch null
```

Der Fix ist eine Instruktion (`+0.5f` bzw. `lrintf`), aendert das Format nicht
und kostet keine messbare Zeit. Auf Rotationsebene: mittlerer Fehler von
0,00426° auf 0,00213°, Maximum von 0,02156° auf 0,01045°.

## B3 — Out-of-Range wird zu Extremwerten (behebbar)

Der Fehlerpfad beider Squash-Funktionen (`0x418217`, `0x4182f7`):

```asm
cmp byte ptr [0x4b2100], 0    ; globales "schon gewarnt"-Flag
jne  skip                     ; Warnung nur EINMAL pro Programmlauf
push "SquashFloat(): Fatal Error..."
call printf
mov byte ptr [0x4b2100], 1
skip:
xor ax, ax                    ; Rueckgabe 0
ret
```

Zwei Probleme:

1. **Rueckgabe 0** dekodiert zu `0/16383 − 2 = −2.0` beim Quaternion und
   `0/64 − 512 = −512` bei der Translation. Ein einzelner ungueltiger Wert
   setzt den Bone also auf das absolute Extrem, statt ihn zu klemmen.
2. **Das Flag ist global und wird nie zurueckgesetzt** — und beide Funktionen
   teilen sich dasselbe Byte `0x4b2100`. Eine Warnung aus `SquashFloat`
   unterdrueckt also auch alle spaeteren Meldungen aus `SquashXlatFloat`.
   Nach dem ersten Vorfall laeuft der Rest des Kompiliervorgangs stumm.

`g2c` klemmt stattdessen auf den naechstgelegenen gueltigen Wert, faengt NaN ab
und zaehlt jeden Vorfall einzeln in `CompressStats`.

## B4 — Limitverletzungen brechen nicht ab (behebbar)

In `0x41b000`:

```asm
cmp  eax, 0x3e8              ; 1000 Vertices
jle  weiter
push "too many vertices\n"
call printf                  ; ... und dann?
cmp  dword [esi-8], 0x7d0    ; 2000 Faces
jle  weiter
push "too many faces\n"
call printf
<rechnet ungebremst weiter>
```

Kein `return`, kein `exit`. Die Offsetberechnung laeuft mit den zu grossen
Werten weiter. In `g2c` sind Limitverletzungen Exceptions.

## B5 — Nicht normalisiertes Quaternion beim Dekodieren (ausnutzbar)

Kein Fehler im engeren Sinn, aber folgenreich: `MC_UnCompressQuat` baut die
Matrix direkt aus den dekodierten Komponenten, **ohne zu normalisieren**. Ein
Quantisierungsfehler veraendert damit nicht nur die Rotation, sondern macht die
Bone-Matrix leicht nicht-orthonormal — sie wird minimal geschert und skaliert.

Daraus folgt, dass komponentenweises Runden gar nicht optimal ist. Da die
Dekodierseite feststeht, kann der Encoder die 81 Kandidaten im Umkreis von ±1
Stufe je Komponente durchprobieren und den waehlen, dessen dekodierte Matrix am
wenigsten von der Zielrotation abweicht.

Das druckt den maximalen Rotationsfehler von 0,01045° auf 0,00609°, also
3,5× besser als Carcass, bei unveraendertem Dateiformat. Kostet Faktor 10 in
der Kompressionszeit (`CompressOptions::optimizeQuat`, abschaltbar).

## Messfallstrick am Rande

Beim Verifizieren zunaechst selbst hineingelaufen: den Winkel zwischen zwei
Rotationen ueber `2*acos(dot(qa,qb))` zu bestimmen ist bei kleinen Winkeln
unbrauchbar. `acos` hat bei Argumenten nahe 1 eine unendliche Ableitung, sodass
allein die float-Rundung eine Messuntergrenze von rund 0,03° erzeugt — mehr als
der Fehler, den man messen will. Alle drei Quantisierungsmodi sahen dadurch
zunaechst exakt gleich gut aus.

Stabil ist `2*atan2(|qa−qb|, |qa+qb|)` in doppelter Genauigkeit. Siehe
`angleBetweenDeg()` in `compress.h`.

## Nicht behebbar ohne Formataenderung

| Grenze | Wert | Ursache |
|---|---|---|
| Bone-Referenzen pro Surface | 32 | 5 Bit pro Referenz im Vertex |
| Gewichte pro Vertex | 4 | Vertexgroesse auf 32 Byte festgelegt |
| Gewichtsaufloesung | 10 Bit | 8 Bit im Byte-Array + 2 Bit gepackt |
| Bone-Pool-Eintraege | 16.777.215 | 3-Byte-Frameindex |
| Quaternion-Wertebereich | ±2 | `MC_UnCompressQuat` |
| Translationsbereich | ±511 | `MC_UnCompressQuat` |

Die Poolgrenze prueft Carcass gar nicht. `g2c` wirft dort eine Exception.

---

# Pruefung September 2026: Fehler in g2c selbst

Gefunden mit den Unit-Tests, einem neuen Oberflaechentreiber
(`tests/gui_driver.cpp`), einem Rundlauf ueber 53 echte .car-Dateien und
einem Vergleich gegen Carcass v2.2 und Ravens ausgelieferte `_humanoid.gla`.
Alle behoben; die Tests dazu stehen in `tests/`.

## Ergebnis der Animationen

- 1400 gemeinsame Sequenzen gegen Ravens `_humanoid.gla`: kein Koerper-Bone
  weicht in irgendeinem Frame mehr als 0,13 Grad / 0,12 Einheiten ab
  (Quantisierungsrauschen).
- Voller Movie-Duels-Humanoid (1854 Dateien, 47422 Frames): bitgleich
  reproduzierbar, unabhaengig von Cache und Threadzahl (1 bis 32).
- Carcass v2.2 kann diesen Humanoid nicht mehr bauen: Abbruch an einer
  Basepose-Abweichung (leye/reye in both_flamethrower.xsi), mit
  `-ignorebasedeviations` dann an einer Translation von -520,8 (erlaubt
  +-511).

## Behoben

| Bereich | Fehler | Folge |
|---|---|---|
| Bauen | `$keepmotion` schaltete die Wurzelrampe ab | Figur rutscht bis 96 Einheiten aus der Mitte |
| Bauen | vorhandene, aber unlesbare .xsi still uebersprungen | alle folgenden Zielframes verschoben |
| Bauen (GUI) | "vor dem Bauen speichern" ueber `&d - docs_.data()` auf eine Kopie | undefiniert; .car praktisch nie gespeichert |
| Bauen (GUI) | Ausgaben ueber ungepruefte ofstreams, direkt ins Ziel | gesperrte/volle Platte: "gebaut" gemeldet, alte GLA auf null gekuerzt |
| Alle Ausgaben | Schreiben ohne Nebendatei | Abbruch = halbe Datei. Jetzt: Nebendatei + Umbenennen |
| .car speichern | Kopfkommentare, Zeilenendkommentare, `-makeskin`, unbekannte Flags, Anfuehrungszeichen, qdskip-Klammer, Zeilenreihenfolge | jede Zeile umgeschrieben, Teile verloren. Jetzt 53/53 Dateien zeilengenau |
| .car speichern | Grabs ohne `$aseanimgrabinit` | Datei ohne Sequenzen |
| .car speichern | `$include`-Grabs ins Hauptskript kopiert | doppelte Sequenzen |
| GUI | Chinesisch/Japanisch: `%s` vor `%zu` | Absturz beim XSI-Ordner und beim GLA-Oeffnen. Jetzt Pruefung zur Uebersetzungszeit |
| GUI | Umlaute in Pfaden | Datei nicht gefunden. Jetzt UTF-8-Manifest |
| GUI | Schliessen (Kreuz, Strg+W, Alle, Fenster) | ungespeicherte Aenderungen ohne Rueckfrage weg |
| GUI | "Neue .car" mit Oeffnen-Dialog | liess sich nie anlegen |
| GUI | Loeschen einer Sequenz | Trennlinien darueber mitgeloescht |
| GUI | Ziehen in der Tabelle | ganze Zielzeile eingerahmt, immer davor eingefuegt. Jetzt Linie ueber/unter der Zeile je nach Mausposition, Einfuegen genau dort; unter eine Ueberschrift gezogen bleibt sie darueber |
| GUI | Umschalt-Auswahl mit Filter | ausgeblendete Zeilen mit ausgewaehlt und geloescht |
| GUI | Ziehen auf anderen Tab | verschob Zeilen im falschen Skript |
| GUI | Sequenzdialog/Loeschrueckfrage beim Tabwechsel | bearbeitete/loeschte im anderen Skript |
| GUI | Hell/Dunkel bei >100 % DPI | Abstaende wuchsen je Wechsel, Knoepfe am Fensterrand unklickbar |
| GUI | Zeilenkommentar per Doppelklick | oeffnete den Sequenzdialog; getippter Text ging verloren |
| GUI | Export in vorhandenen Ordner | .xsi und .car ohne Rueckfrage ueberschrieben |
| GUI | Vorschau mit fremder animation.cfg | Zugriff ausserhalb der Liste |
| GUI | Strg+O, Strg+Umschalt+O, Strg+W, F5, Umschalt+F5, F7 | standen im Menue, taten nichts |
| GUI | .gla/anims.h aufs Fenster gezogen | ignoriert |
| GUI | Framezahlen nach Korrektur der Assetwurzel | blieben "fehlt" bis zum Neustart |
| GUI | Fensterlage | wurde nie gespeichert; `-reset` loeschte die falsche Datei |
| GUI | Tab-Auswahl von aussen (Doppelklick auf eine .car, zuletzt aktiver Tab beim Start) | ImGui zeigte trotzdem den ersten Tab und ueberschrieb die Auswahl. Jetzt ImGuiTabItemFlags_SetSelected |
| GUI | Doppelklick auf eine .car bei offenem g2c | zweites Fenster mit allen Tabs. Jetzt an das laufende Fenster weitergereicht (WM_COPYDATA), das auf den Tab springt |
| Tests | `g2_gui_tests` benutzte die echten Einstellungen | konnte eine echte .car ueberschreiben |
| Tests | offene Datei bei `remove_all` | 77 Pruefungen still uebersprungen |
| CLI | `anim ref.gla ref.gla ...`, `build` ohne -o im Modellordner | Referenz und animation.cfg ohne Sicherung ersetzt. Jetzt .bak |
| CLI | Ziehen einer .car: GLA-Name der Referenz statt -makeskel | Engine nimmt die falsche animation.cfg |
| CLI | `export`/`makecar`/`scan`: unbekannte Optionen ignoriert, `stof` | "-scale 0,64" = 0, "-orgin" = geschaetzt |
| CLI | `mesh -compare` ohne Grenzpruefung | Lesen ausserhalb des Puffers |
| CLI | GLM-Fehler, Ausnahme bei gezogener Datei | Rueckgabe 0 bzw. Programmende ohne Meldung |
| CLI | `-cache X -clearcache`, `-o` ohne Endung, `-o ordner/` | falscher Cache geleert, "C:\jka.frames", "players//x.xsi" |
| Lesen | negative Offsets, ungueltige Parent-Indizes in GLA und Cache | Lesen/Schreiben ausserhalb des Speichers |
| Lesen | GLM-Surfaces nach uebersprungenem Mesh | Elternbezuege um eins verrutscht |
| GUI | Minimiertes Fenster | Hauptschleife lief leer weiter: 88 % eines Kerns, solange g2c in der Taskleiste lag. Jetzt Pause, gemessen 1 % |
| GUI | Schliessen ueber die Taskleiste bei minimiertem Fenster mit ungespeicherten Aenderungen | Rueckfrage unsichtbar. Jetzt wird das Fenster dafuer wiederhergestellt |
| GUI | Framespeed ohne `-framespeed` als 0 bzw. "auto" angezeigt | man sah nicht, wie schnell eine Animation laeuft; "+" im Dialog machte aus der echten 25 eine 1. Jetzt die echte Zahl (SI_Scene, sonst 30), Regressionstest im GUI-Treiber |
| GUI | Loop ohne `-loop` im Dialog als -1 | gebaut wird 0 (wie Carcass, geprueft an Ravens animation.cfg); "-" machte -2 daraus |
| GUI | Split-Teile (`-additional`) nur als Anzahl | Namen jetzt in der Tabelle, Details (Ziel, Frames, Loop, Speed) per Ansicht-Menue, wie Assimilate |
| Bauen (CMake) | `-DG2C_VERSION=v1.2.0` ungequotet aus PowerShell | PowerShell teilt am Punkt, die Exe hiess "1". Jetzt Abbruch mit Hinweis; Workflow quotet |

## Code-Review Oktober 2026

Fuenf Pruefer haben den Code nach Bereichen durchsucht (Skript und Bau,
Binaerformate, dotXSI, Oberflaeche, Updater und Kommandozeile). Jeder Fund
wurde am Code und, wo moeglich, an den echten Daten nachgeprueft. Fuer jeden
behobenen gibt es einen Regressionstest (`REGRESSION` in der Ausgabe); gegen
den alten Code laufen gelassen schlagen sie fehl - 30 in den Unit-Tests (der
Block mit kaputten GLA-Koepfen haengt dort sogar), 6 von 6 an der
Kommandozeile.

Der _humanoid baut nach allen Aenderungen weiter bitgleich, alle 45 .car-Dateien
kommen beim Speichern byte-gleich zurueck.

| Bereich | Fehler | Folge |
|---|---|---|
| Skript | alles hinter `$exit` | beim Speichern geloescht |
| Skript | Leerzeilen ohne Kommentar, eingerueckte Kommentare, reine LF-Dateien | verloren bzw. jede Zeile auf CRLF umgeschrieben |
| Skript | Kommentar ueber `$aseanimgrabfinalize` hinter `$scale`/`$pcj` | sprang hinter die letzte Animation |
| Skript | zwei `$aseanimconvertmdx`-Zeilen | die erste ging verloren |
| Skript | Zeile zwischen zwei Grabs | wanderte hinter den letzten Grab |
| Skript | `//` mitten in einem Pfad | als Kommentar genommen, landete in animation.cfg und wuchs bei jeder Bearbeitung |
| Skript | `-loop nan`, `1e10`, `inf` | INT_MIN in animation.cfg |
| Modell-Dialog | `$pcj`/`$scale`/`$keepmotion` aus `$include` | ins Hauptskript kopiert; Kommentar ueber geloeschter Zeile verschwand |
| Pruefen | doppelter Name | zweimal gemeldet, Fehlerzahl doppelt |
| Bauen | `-additional` ausserhalb der Datei | ohne Warnung in die animation.cfg |
| GLA | skalierte oder gespiegelte Bone-Matrix | falscher Winkel (0.64 * 90 Grad -> 76), Nullskalierung -> 180 Grad |
| GLA | Translation zwischen -512 und -511 | um bis zu 1 verschoben |
| GLA lesen | riesige Bone-Anzahl, Parent-Kreis | 200 GB Anforderung bzw. still Nullmatrizen |
| Diff | negativer Startframe in der cfg | Lesen vor der Frametabelle |
| Cache | Datei waehrend des Lesens geaendert | halbe Animation dauerhaft im Cache |
| dotXSI | Kurven mit mehreren Werten pro Key (CUBIC) | Tangenten als Keys gelesen |
| dotXSI | `CONSTANT` mit Luecken | linear statt gestuft (in keiner der 2729 Dateien hier) |
| dotXSI | abgeschnittene Kurve, NaN-Wert | Bone still in Ruhepose |
| dotXSI | Keys kuerzer als SI_Scene | eingefrorene Frames ohne Warnung |
| dotXSI | SI_Scene mit Milliarden Frames | Terabyte-Anforderung |
| dotXSI | BOM am Dateianfang | abgelehnt |
| Mesh | mehrere TriangleLists, COLOR-Block, kurzer Indexblock | Dreiecke fehlten, falsche UVs, Lesen hinter dem Ende |
| Mesh | Gewichte auf Bones ausserhalb des Skeletts, ungueltiger Index | still verworfen bzw. Vertex im Ursprung - jetzt Warnung |
| Mesh | mehr als 1000 Vertices pro Surface | Spiel laedt das Modell nicht - jetzt Warnung |
| GUI | Datei von zweitem g2c waehrend offenem Dialog | Schreiben in freigegebenen Speicher |
| GUI | Kommentar-Bearbeitung beim Tabwechsel | landete im anderen Skript |
| GUI | zwei Dialoge gleichzeitig (z. B. Fenster schliessen bei offenem Framespeed-Dialog) | beide unsichtbar, nichts klickbar |
| GUI | Ausgabeordner geschlossener Skripte, Pfade mit `=` | vergessen |
| GUI | aktiver Tab beim Start, wenn eine fruehere Datei fehlt | falscher Tab |
| GUI | Doppelnamen vom Bauen | beim zweiten Bauen doppelt, bei gleichem Titel im falschen Tab |
| GUI | Framezahlen | ohne Assetwurzel nie gelesen; nach Neu-Export veraltet; Tabwechsel las alles neu |
| GUI | Umschalt-Auswahl | Anker aus anderem Tab; Filter auf Teilnamen uebersprungen |
| GUI | Kommentar mit `##` | nur bis zum `##` angezeigt |
| GUI | Monitor mit anderer Skalierung | Schrift und Abstaende blieben |
| Updater | Ruecknahme beim Austausch scheitert auch | geprueftes neues Exe geloescht, keines mehr da |
| Updater | nach "Spaeter" erneut gesucht | dasselbe Update noch einmal geladen, dann Fehler |
| Updater | Ordner nicht beschreibbar | als "keine Verbindung" gemeldet |
| Updater | Test-Umleitung per Umgebungsvariable | auch auf fremde Server - jetzt nur 127.0.0.1/localhost |
| CLI | neue GLA neben der Referenz | deren animation.cfg ohne Sicherung ersetzt |
| CLI | `export` | ueberschrieb .car und .xsi ohne Rueckfrage - jetzt nur mit `-force` |
| CLI | `-threads -1` | "bad allocation" erst nach dem ganzen Bau |
| CLI | `export -only BOTH_RUN1` | exportierte auch BOTH_RUN1_* (11 statt 1) |

Bewusst nicht geaendert:

- **Translation der Wurzel im exportierten Bind-Pose-Block bei Skalierung**: ohne
  eigenen Carcass-Versuch mit verschobener Wurzel nicht sicher zu entscheiden;
  `model_root` liegt bei JKA im Ursprung.
- **Bone-Namen mit Punkt/Leerzeichen, gespiegelte Bind-Posen beim Export**:
  kommen bei JKA-Skeletten nicht vor; Umbenennen wuerde die Zuordnung brechen.
- **Stromausfall direkt nach dem Ersetzen einer Datei**: das Schreiben ueber
  Nebendatei und Umbenennen schuetzt vor Abbruch und Absturz, nicht vor
  Stromausfall (dafuer fehlt ein Flush auf die Platte).
- **GLA und animation.cfg an der Kommandozeile** sind zwei getrennte Schreibvorgaenge.
- **g2c-cli.exe**, deren Austausch beim Update scheitert, bleibt bis zum naechsten
  Release alt; das Update meldet es.


## Oktober 2026: Carcass v2.2 vollstaendig

Alles, was Carcass kann, ist in g2c (ANLEITUNG Abschnitt 72). Dabei wurden
diese Carcass-Fehler gefunden. g2c macht es anders - mit `-carcass` nur dort
wie Carcass, wo es um bytegleiche Dateien geht (Rundung, Glaetten).

| Bereich | Carcass | g2c |
|---|---|---|
| Kommandozeile | `-makeskin`, `-makeskel`, `-origin` stehen in der Hilfe, sind aber "Unknown option" | gehen |
| Kommandozeile | Optionen mit Wert als letztes Argument: Absturz; Optionen nach der .car werden als .car gelesen | Fehlermeldung; Optionen ueberall, gross/klein egal |
| Kommandozeile | `-smooth`/`-losedupverts` wirkungslos (jede Convert-Zeile setzt sie zurueck) | wirken |
| Aktualitaet | vergleicht nur mit der GLM, liest auskommentierte Zeilen, baut bei gleichem Zeitstempel | prueft GLA und GLM, Inhalt statt Text |
| `-recursive` | gross/klein beim Ueberspringen von `backup`/`ignore_`; ein Fehler beendet den ganzen Lauf | ohne Unterschied; jede .car fuer sich |
| `-filelist` | Zeilen kleingeschrieben, an Leerzeichen abgeschnitten, eingerueckte verworfen | Pfade wie geschrieben, Kommentarzeilen erlaubt |
| `-framestep` | Speed nicht geteilt (doppelt so schnell); `_skip` erst ab 3 | Speed/Loop/Teile mitgeteilt; `_skip` ab 2 |
| `.frames` | eine Datei zweimal gegrabbt: ein Block, der erste Startframe verloren | ein Block je Grab |
| `$pcj`/`-flatten` | vor den Grabs: Abbruch mit falscher `_always_`-Meldung (nennt die GLA statt den Bone) | Warnung mit Bone-Namen |
| `$aseanimref_gla` | mit einer GLA, die Carcass selbst gebaut hat, immer Abbruch; Gesichts-Bones still unter cranium | Abgleich ueber den GLA-Namen, Reihenfolge der Referenz |
| Skelett | Bone-Namen gross/klein verschieden: zweiter Bone, Finger still umgehaengt | ein Bone, Hinweis |
| Skelett | Bone ohne BASEPOSE: lokale statt absolute Pose | aus der SRT-Kette zusammengesetzt |
| Skelett | abweichende Bindepose: Abbruch (MD-Humanoid unbaubar) | Warnung, letzte Datei gilt |
| `$bonehiercap` | eine alte `.bonecap` ohne Kappung still geloescht | bleibt |
| Glaetten | entgegengesetzte Normalen addiert (doppelseitig = schwarz), Tags verbiegen die Nahtnormalen | nur gleichseitige, ohne Tags |
| Gewichte | groesstes Gewicht rundet auf 0 -> "vier Gewichte, alle null" | ein Gewicht |
| Tags | zwei gleich lange Kanten: Richtung des vorigen Tags, Vertex doppelt moeglich | eindeutig |
| Tags | Name `*back\0ack` (Rest von `bolt_back` hinter der Null) | sauber |
| `-makeskin` | erstes Byte jedes Shader-Namens auf 0 | Shader leer |
| UV | ohne UV-Block: UVs ueber den Normalenindex; COLOR-Block als UV gelesen | nach dem Attribut-String |
| Normalen | Mesh ohne NORMAL: Null-Normalen | Flaechennormalen |
| Vertex ohne Gewicht | Abbruch | an Bone 0, Warnung |
| `_info.txt` | "Index bytes for pooling" aus der Mesh-Statistik | echte 3 Byte je Bone und Frame |
| ASE | Namen ohne Anfuehrungszeichen verlieren das erste Zeichen, mit Leerzeichen werden abgeschnitten; `.ase` geht nicht | ganz gelesen; `.ase` geht |
| ASE | "Bip"-Objekte sollten wegfallen, fallen nie weg (nach dem Kleinschreiben gesucht) | fallen weg |
| ASE | Objekt ohne `*MESH_WEIGHTS`: alle Punkte im Ursprung, NaN-Normalen | Fehler |
| ASE | Bone-Index hinter der Bone-Liste: still Bone 0 | Fehler |
| MDR | komprimierte Frames im falschen Format (vom Spiel nicht lesbar); immer `test.mdr` | unkomprimiert; nach der .ask benannt |
| MDR | `$aseanimgrab_gla`: alle Matrizen Einheitsmatrizen | die echten Frames der GLA |
| MDR | Namen mit `_x` am Ende verlieren zwei Zeichen (`u_x` -> `u`) | nur die LOD-Endung faellt weg |
| ASE-GLM | `*NODE_PARENT` ignoriert: nur Ein-Objekt-Modelle baubar | Hierarchie aus `*NODE_PARENT`, sonst unter dem ersten Objekt |
| CARPET | Zwischenspeicher in `C:\ravenlocal\CARPET`, waechst unbegrenzt (bei dir 11 GB) | Cache neben der .car |

Nicht nachgebaut, weil Carcass es selbst nicht kann: MD3-Ausgabe,
`-playerparms` (head/upper/lower.mdr), `-weapon`.
