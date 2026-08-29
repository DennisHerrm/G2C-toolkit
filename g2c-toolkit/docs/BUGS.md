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
