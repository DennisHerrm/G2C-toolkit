# dotXSI-Semantik, abgeleitet aus `root.xsi` (Raven, dotXSI 3.5)

Verifiziert an der echten `_humanoid`-Quelle: 1,73 MB, 3444 Templates,
dotXSI 3.5 (`0350txt`), erzeugt mit SOFTIMAGE|XSI 2.0.2001.1116.

## Bones

Jeder Bone ist ein `SI_Model`, verschachtelt entsprechend der Hierarchie:

```
SI_Model MDL-<praefix>.<bonename> {
    SI_Transform BASEPOSE-<praefix>.<bonename> {
        sx, sy, sz,      // Skalierung
        rx, ry, rz,      // Rotation in Grad
        tx, ty, tz       // Translation
    }
    SI_FCurve { ... }    // je eine pro animiertem Kanal
    SI_Model MDL-...     // Kinder
}
```

Der Bonename ist der Teil nach dem letzten Punkt. Das Praefix ist der
Rig-Name, hier `Davinci_Male01_TrueBones`.

### Basispose — empirisch bestimmt und verifiziert

`BASEPOSE-<bone>` ist **absolut**, nicht relativ zum Elternbone. Das war die
entscheidende Erkenntnis; mit der naheliegenden Annahme "lokal" passt gar
nichts.

Die Umrechnung nach GLA lautet:

```
Rotation    R = Rz(rz) * Ry(ry) * Rx(rx)      // XYZ extrinsisch, Grad
GLA-Matrix  M = scale * (C * R * C^T)
GLA-Trans   t = scale * (C * (tx,ty,tz))

              | 1  0  0 |
          C = | 0  0 -1 |      (Y-hoch nach Z-hoch)
              | 0  1  0 |
```

`scale` ist der `$scale`-Wert aus der .car, hier 0.64.

Bestimmt wurde das durch Durchprobieren aller sechs Euler-Reihenfolgen mal
intrinsisch/extrinsisch mal 48 vorzeichenbehafteter Permutationsmatrizen,
verglichen gegen die `basePoseMat` aller 53 Bones der echten
`_humanoid.gla`:

| Reihenfolge | max. Abweichung |
|---|---|
| **XYZ extrinsisch (= ZYX intrinsisch)** | **0.0557** |
| XZY extrinsisch | 0.7710 |

Faktor 14 zum naechstbesten Kandidaten. Pro Bone betrachtet stimmen **44 von
52 auf 6·10⁻⁷ genau**. Die acht Abweichler sind ausnahmslos Gesichtsbones
(`jaw`, `ceyebrow`, `leye`, `reye`, `lblip2`, `rblip2`, `ltlip2`, `rtlip2`) —
also genau der Bereich, in dem sich die beiden vorliegenden Dateistaende
nachweislich unterscheiden (`face` gegen `face_always_`). Deren Translation
stimmt weiterhin auf 2·10⁻⁶.

`SI_CoordinateSystem { 1, 0, 1, 0, 2, 5 }` muss damit nicht mehr dekodiert
werden — das Ergebnis steht empirisch fest.

### basePoseMatInv

Die Engine erwartet die **echte affine Inverse**, nicht die Transponierte. In
der originalen `_humanoid.gla` steht bei `model_root` eine 1.5625 = 1/0.64.
Da Carcass den `$scale` in die Basisposen einbackt, ist der Rotationsteil
nicht orthonormal und die Transponierte liegt um 0.92 daneben. Nachgeprueft:
M · MInv ergibt in der Originaldatei die Einheitsmatrix auf 5·10⁻⁷.

## Animation

```
SI_FCurve {
    "<praefix>.<bonename>",
    "SCALING-X" | "ROTATION-Y" | "TRANSLATION-Z" | ...,
    "LINEAR" | "CUBIC",
    <a>, <b>, <keyCount>,
    frame, wert,
    ...
}
```

Neun Kanaele pro Bone (Skalierung, Rotation, Translation je XYZ).

## Vertexgewichtung

Auf oberster Ebene, nicht im Modellbaum:

```
SI_EnvelopeList <szene> {
    <anzahl>,
    SI_Envelope <name> {
        "MDL-<mesh>",     // das verformte Mesh
        "MDL-<bone>",     // der verformende Bone
        <vertexAnzahl>,
        <vertexIndex>, <gewichtProzent>,
        ...
    }
}
```

Ein Eintrag pro (Mesh, Bone)-Paar, Gewichte in **Prozent** (0–100), nicht 0–1.
Vertices mit Gewicht 0 werden ausdruecklich mitgeschrieben.

## Mesh

```
SI_Mesh MSH-<name> {
    SI_Shape SHP-<name>-ORG {
        <anzahlArrays>,        // 2 oder 3
        "ORDERED",
        <n>, "POSITION",      x,y,z ...
        <n>, "NORMAL",        x,y,z ...
        <n>, "TEX_COORD_UV",  u,v ...
    }
    SI_TriangleList <name> {
        <dreiecksAnzahl>,
        "NORMAL" | "NORMAL|TEX_COORD_UV",   // welche Indexarrays folgen
        "<materialName>",
        <positionsIndizes je Dreieck>,
        <normalenIndizes je Dreieck>,
        <uvIndizes je Dreieck>
    }
}
```

Wichtig: getrennte Indexarrays pro Attribut. Positionen, Normalen und UVs
haben eigene Indizes — im Beispiel 132 Positionen, aber 609 Normalen. Beim
Export nach GLM muss aufgetrennt und ueber eindeutige Tripel
`(posIdx, normIdx, uvIdx)` dedupliziert werden, weil GLM nur einen Index pro
Vertex kennt.

### Vertexposition — verifiziert

Die Positionen in `SI_Shape` sind objektlokal. Die Umrechnung lautet:

```
world = M_mesh.rot * p + M_mesh.trans      // BASEPOSE des Mesh-SI_Model
glm   = scale * (C * world)                // gleiches C und scale wie bei den Bones
```

An `hips` geprueft: 132 XSI-Positionen, 132 GLM-Vertices, und **jede
GLM-Position hat eine exakt passende XSI-Position** (Abstand zum naechsten
Nachbarn: Median und Maximum beide 0,0000).

Carcass aendert allerdings die **Reihenfolge** der Vertices. Ein Vergleich
muss deshalb ueber naechste Nachbarn laufen, nicht ueber den Index.

## Shaderzuordnung

```
XSI_CustomPSet <praefix>.<mesh>.Game {
    "NODE",
    1,
    "Shader","Text","models/players/luke/legs.tga",
}
```

Daher kommen die Shadernamen in der GLM.

## Offene Punkte

- Kodierung von `SI_CoordinateSystem`
- Genaue Bedeutung der Zahlen vor der Keyliste in `SI_FCurve`
- Wie Tags (`tag_*`) im XSI markiert sind — vermutlich ueber Meshnamen
- Behandlung von `SI_Cluster` (333 Stueck) und `SI_IK_Joint` / `SI_IK_Effector`

---

## Animation — verifiziert

Anders als `BASEPOSE` sind die `SI_FCurve`-Werte **lokal**, also relativ zum
Elternmodell in der XSI-Hierarchie. Das ist der entscheidende Unterschied; mit
der Annahme "absolut" passt nichts.

Ausserdem: **die XSI-Hierarchie ist nicht die GLA-Hierarchie.** In
`BOTH_attack10.xsi` ist `ltibia` ein Kind von `lfemurX`, in der GLA dagegen von
`lfemurYZ`. Carcass strukturiert die Bone-Kette um. Fuer die Weltposen gilt die
XSI-Hierarchie, fuer die Relativierung die GLA-Hierarchie.

### Ablauf

```
1. Lokale Matrix je Bone und Frame aus den FCurves:
       L(b,f) = T(trans) * Rz(rz) * Ry(ry) * Rx(rx) * S(scale)

2. Weltpose ueber die XSI-Hierarchie akkumulieren:
       Wx(b) = Wx(parent_xsi) * L(b)

3. In den GLA-Raum umrechnen (gleiches C und scale wie bei den Bones):
       X(b) = scale * (C * Wx(b) * C^-1)

4. Gegen die Basispose und den GLA-Elternbone relativieren:
       A(b) = B(parent_gla) * X(parent_gla)^-1 * X(b) * B(b)^-1

   B ist die basePoseMat aus der GLA. A(b) ist die Matrix, die
   komprimiert in den Bone-Pool geht.
```

Schritt 4 folgt direkt aus der Engine: `tr_ghoul2.cpp` wertet
`W(bone) = W(parent) * A(bone)` aus, und die Skinning-Matrix ist `X * B^-1`.
Umgestellt ergibt sich obige Formel.

### Messung

`BOTH_attack10.xsi` (20 Frames) gegen `_humanoid.gla` ab Zielframe 1541 laut
`animation.cfg`:

| | Wert |
|---|---|
| verglichene (Bone, Frame)-Paare | 840 |
| max. Abweichung Rotation | 1,77·10⁻⁴ |
| Quantisierungsschrittweite Rotation | 6,10·10⁻⁵ |
| Bones mit Translation unter einer Stufe (0,0156) | 40 von 42 |

Die Abweichung liegt damit im Rauschen des 16-Bit-Formats. Die beiden
Ausreisser bei der Translation sind `ltail` und `rtail` — genau die Bones, deren
Elternbeziehung Carcass umbaut.

### Nicht reproduziert

`Torso_handsignal2.xsi` und `face_talk2.xsi` passen mit denselben Zielframes
nicht. Beides sind Teilanimationen (nur Torso bzw. nur Gesicht). Moegliche
Gruende: die Dateien sind neuer als die vorliegende GLA, oder Teilanimationen
werden anders eingesetzt als vollstaendige. Ungeklaert.


---

## Exportvarianten: Raven gegen 3ds Max

Nicht alle dotXSI sind gleich. Zwei Quellen im selben Projekt:

| | Raven | 3ds Max 2025 |
|---|---|---|
| Version | `0350txt` | `0300txt` |
| Modellnamen | `MDL-<rig>.<bone>` | `MDL-<bone>` |
| Kanaele pro Bone | 6 (ohne SCALING) | 9 |
| **Framenummern in SI_FCurve** | **`1`** | **`1.000000`** |

Der letzte Punkt ist eine Falle. Wer die Framenummer als Ganzzahl liest
(`from_chars` auf `long`, `atoi`, `stoi`), bekommt bei Max-Exporten **keinen
einzigen Keyframe** — die Konvertierung scheitert an der Nachkommastelle, und
je nach Fehlerbehandlung wird der Key still verworfen.

Das Ergebnis ist besonders heimtueckisch: die Kanaele existieren, sind aber
leer. Es gibt keine Fehlermeldung, keine fehlende Datei, keinen Absturz. Die
Animation kompiliert durch und jeder Bone steht regungslos in der Ruhepose.

An `BOTH_PISTOLRELOAD.XSI` gemessen, 35 Frames gegen die Originaldaten:

| | vorher | nachher |
|---|---|---|
| max. Abweichung | 89,0 | 0,157 |
| Bones innerhalb einer Quantisierungsstufe | 2 von 53 | 51 von 53 |

Die beiden verbleibenden sind `leye` und `reye` — der bekannte
`face`/`face_always_`-Unterschied.

**Regel: Zahlen in dotXSI immer als Gleitkomma lesen und erst danach runden.**
Das Format unterscheidet nicht zwischen Ganzzahl und Dezimalzahl; welche
Schreibweise kommt, entscheidet der Exporter.


---

## SI_Transform SRT: der Ruhewert

Jedes `SI_Model` kann bis zu zwei Transformationsbloecke tragen:

```
SI_Transform BASEPOSE-<name> { ... }   absolute Bindpose
SI_Transform SRT-<name>      { ... }   statische LOKALE Transformation
```

Beide im selben Aufbau: Skalierung XYZ, Rotation XYZ in Grad, Translation XYZ.

`SRT` ist der Ruhewert des Bones. FCurves ueberschreiben ihn **kanalweise** —
wo keine FCurve existiert, gilt weiter der SRT-Wert. Wer ihn ignoriert und
stattdessen neutral (0 / 1) auffuellt, laesst jeden nicht animierten Bone in
die Identitaet fallen.

In Ravens Animationsdateien faellt das nicht auf: dort hat jeder Bone FCurves
fuer alle sechs benutzten Kanaele. In `root.xsi` dagegen haben nur **126 von
276 Modellen** FCurves — die uebrigen 150 haengen allein am SRT-Block.

Messung an der ROOT-Sequenz (2 Frames), gegen die Originaldaten:

| | ohne SRT | mit SRT |
|---|---|---|
| Bones innerhalb einer Quantisierungsstufe | 14 von 53 | **53 von 53** |
| max. Rotationsabweichung | — | 1,25·10⁻⁴ |

`BASEPOSE` wird hier ausdruecklich **nicht** verwendet: es ist die absolute
Bindpose und gehoert in den Skelettaufbau, nicht in die Frameauswertung.


---

## Wurzelbewegung

Bewegt sich die Figur waehrend einer Sequenz durch den Raum — sterben,
umfallen, aufstehen —, legt Carcass eine **lineare Rampe** auf den
Wurzelbone `model_root`:

```
ramp(f) = -scale * C * (W_motion(letzter) - W_motion(erster)) * f/(n-1)
```

`W_motion` ist die Weltpose des Bones `Motion`. Drei Punkte sind wichtig:

1. **Es ist eine Rampe, nicht die Kurve.** Der `Motion`-Bone schwankt teils
   heftig; in `BOTH_DEATH17` geht seine Z-Koordinate von 0 ueber −1,7 und −5,3
   auf −5,0 zurueck. Der Wurzelbone laeuft trotzdem schnurgerade von 0 nach
   −3,44. Wer die tatsaechliche Kurve uebertraegt, liegt bei Frame 15 um
   Faktor zwei daneben.
2. **Das Vorzeichen ist umgekehrt** zur normalen Positionsumrechnung. Es ist
   eine Gegenbewegung: die Figur laeuft vorwaerts, der Wurzelbone schiebt
   zurueck, und die Engine addiert die Verschiebung selbst wieder drauf.
3. **Nur die Translation.** Der Rotationsteil von `model_root` bleibt in
   beiden geprueften Sequenzen die Einheitsmatrix.

Gegenprobe an zwei Sequenzen, jeweils gegen die Originaldaten:

| Sequenz | Frames | Original bei Ende | berechnet |
|---|---|---|---|
| `BOTH_DEATH17` | 125 | −2,766 | −2,766 |
| `BOTH_SIT2TOSTAND5` | 56 | 4,609 | 4,625 |

Ueberall innerhalb einer Quantisierungsstufe (0,015625).

### Quelle ist ausschliesslich der Motion-Bone

`BOTH_wall_flip_right.xsi` zeigt das deutlich: die Figur springt ueber den
Becken-Bone 10,8 Einheiten zur Seite, der `Motion`-Bone bleibt dabei aber
exakt stehen (Delta 0,0,0). Und die Original-GLA hat fuer diese Sequenz
entsprechend **keine** Wurzelbewegung — nur eine einzelne
Quantisierungsstufe Rauschen.

Carcass wertet also nicht die tatsaechliche Verschiebung der Figur aus,
sondern allein den `Motion`-Bone. Hat der Animator ihn nicht mitbewegt, gibt
es keine Wurzelbewegung, egal wie weit sich die Figur bewegt.

### Bestaetigung der Gegenbewegung

Rechnet man aus der fertigen GLA die Weltpose von `Motion` zurueck, steht sie
ueber eine ganze Sequenz praktisch still — bei `BOTH_RUNSTRAFE_LEFT1` aendert
sie sich um 0,015 Einheiten, waehrend der Wurzelbone um 95,98 laeuft. Genau
das erwartet man, wenn die Rampe die Bewegung des `Motion`-Bones aufhebt.

### Die X-Achse ist belegt

29 Sequenzen der Original-GLA haben eine X-Wurzelbewegung. An
`both_strafe_left1.xsi` nachgeprueft:

| | |
|---|---|
| `Motion` Delta X in der `.xsi` | **+66,000** |
| Formel `-scale * dx` | −0,64 · 66,0 = **−42,24** |
| Original-GLA | **−42,250** |

Differenz 0,01, also unter einer Quantisierungsstufe. Dieselbe
Vektoroperation gilt damit auf allen drei Achsen; das Vorzeichen ist keine
Annahme mehr.

Der ganze Durchlauf, 13 Frames gegen die Originaldaten:

```
Frame  Original model_root      berechnet
0      [  0.000  0.000 -24.0]   [  0.000  0.000 -24.0]
3      [-10.562  0.000 -24.0]   [-10.562  0.000 -24.0]
6      [-21.125  0.000 -24.0]   [-21.125  0.000 -24.0]
9      [-31.688  0.000 -24.0]   [-31.688  0.000 -24.0]
12     [-42.250  0.000 -24.0]   [-42.234  0.000 -24.0]
```

Alle 53 Bones innerhalb einer Quantisierungsstufe, max. Rotationsabweichung
1,97·10⁻⁴.

---

## Mesh: Vertexzusammenfassung

dotXSI hat je Attribut ein eigenes Indexarray, GLM nur einen Index pro Vertex.
Die Zusammenfassung ist **keine Hashtabelle ueber einen Schluessel**, sondern
eine gierige lineare Suche mit Toleranz:

```
gleicher Positionsindex
UND UV innerhalb der UV-Toleranz
UND jede Normalkomponente innerhalb der Normaltoleranz
-> erster Treffer gewinnt
```

Dieselbe Regel benutzt mrwonkos Blender-Exporter. Der Unterschied ist
wesentlich: mit einem exakten Schluessel entstehen zu viele Vertices, und
**kein Rundungsgitter kann das ausgleichen** — die Zusammenfassung ist
reihenfolgeabhaengig, nicht wertdiskret. Ich habe drei Sweeps ueber
Rundungsstufen gebraucht, um das zu begreifen.

### Die Falle im Dateiformat

`TEX_COORD_UV0` traegt einen zusaetzlichen Kopfeintrag:

```
609,
"TEX_COORD_UV0",
"Texture_Projection",     <-- zusaetzlich, POSITION und NORMAL haben ihn nicht
0.127449,0.841402,
```

Wer ihn nicht ueberliest, liest ab da um eine Stelle versetzt. Jede Ecke
bekommt eine falsche UV, und weil die Zusammenfassung UV-Gleichheit verlangt,
faellt gar nichts mehr zusammen — statt 132 Vertices bei `hips` entstehen 583.

Der Fehler tarnt sich weit hinten als Toleranzproblem: keine noch so grosse
Normaltoleranz aendert etwas, weil die UV-Bedingung davor schon alles
blockiert. Genau daran laesst er sich aber auch erkennen — **reagiert ein
Parameter gar nicht, greift der Mechanismus nicht**, und dann hilft kein
weiterer Sweep.

Deshalb: nach Anzahl und Bezeichner alles ueberspringen, was keine Zahl ist.

### Objekttransformation: BASEPOSE, nicht SRT

Fuer die Mesh-Positionen gilt dieselbe Unterscheidung wie bei den Bones:
`BASEPOSE-<name>` ist die **absolute** Pose, `SRT-<name>` die lokale relativ
zum Elternmodell. Die Mesh-Modelle sind ineinander verschachtelt (`torso`
unter `hips` unter `mesh_root`), sodass der lokale Wert ohne Akkumulation
ueber die ganze Kette falsch ist.

Mit SRT lagen 83 von 84 Surfaces um **exakt denselben Betrag** daneben
(49,61 Einheiten). Ein konstanter Versatz ueber fast alle Surfaces ist nie
Rundung — er zeigt auf eine fehlende Transformationsstufe. Mit BASEPOSE
liegen 82 von 84 unter 0,01, maximal 0,0117.

### UV-Spiegelung

`v_glm = 1 - v_xsi`. Gegengeprueft an `torso` und `head_face`: die UV-Mengen
stimmen so **exakt** mit Ravens Datei ueberein, ungespiegelt weichen sie um
bis zu 0,23 ab.

### Stand

Mit Normaltoleranz 0,05 und UV-Toleranz 0,002, gegen Ravens `_humanoid.glm`:

| | |
|---|---|
| Surfaces | 84 von 84 |
| Dreiecke | 2846, exakt |
| Tags, `_off`-Flags, Hierarchie, Shader | exakt |
| Surfaces mit exakter Vertexzahl | **81 von 84** |
| Vertices gesamt | 2673 gegen 2647 |
| Positionen unter 0,01 Abweichung | **82 von 84** |
| Bone-Referenzmengen | **84 von 84** identisch |
| Gewichtssummen | **0** Abweichungen |
| UV-Werte | exakt |

Offen bleiben `hips` (+4), `l_hand` und `r_hand` (je +11).

**Nicht auf die Summe optimiert.** Mit Normaltoleranz 0,15 trifft die
Gesamtzahl exakt 2647 — aber nur, weil `hips` dann +4 und die Haende je −2
liegen und sich die Fehler aufheben. Die Zahl waere getroffen, die Regel
verfehlt, und an einem anderen Modell fiele es auseinander.


---

## Shaderzuweisung

Nur 37 der 84 Mesh-Modelle tragen ein `XSI_CustomPSet` mit Shadernamen. Die
uebrigen erben ihn vom **unmittelbar vorangehenden Geschwister** — gleicher
Elternteil, vorherige Position in der Surfacereihenfolge. Gibt es keines,
steht `[nomaterial]`.

**Nicht vom Elternteil selbst.** Der Unterschied ist gut sichtbar:

| Surface | haengt unter | Shader |
|---|---|---|
| `*l_hand` | `l_hand` (hand.tga) | **torso.tga** von Vorgaenger `l_hand_sleeve` |
| `*l_arm_cap_l_hand` | `l_arm` (torso.tga) | **hand.tga** von Vorgaenger `l_hand` |

Mit Elternvererbung kommt genau das Vertauschte heraus. An allen 46 Tags
geprueft: 44 folgen der Regel, die beiden uebrigen (`*l_leg_calf`,
`*r_leg_calf`) haben kein vorangehendes Geschwister und tragen
`[nomaterial]`.

### Pfadtrenner

Manche `XSI_CustomPSet`-Eintraege schreiben Windows-Pfade mit Backslashes
(`models\players\luke\head.tga`). In der GLM stehen durchgehend
Schraegstriche. Ohne Normalisierung findet die Engine die Textur nicht.

### Korrektur: leere Shader sind erlaubt

Zwischenzeitlich stand hier, ein leerer Shadername sei der Grund gewesen,
warum sich eine erzeugte GLM nicht oeffnen liess. **Das ist widerlegt.**
Ravens `model.glm` (Luke, 4 LODs, 80 Surfaces) hat **alle 80 Shadernamen
leer** und laedt einwandfrei — bei Spielermodellen kommt die Texturzuordnung
ohnehin aus der `.skin`-Datei.

Empirisch half die Aenderung damals trotzdem; die eigentliche Ursache ist
damit unbekannt. Moeglich, dass ModView eine als Modell geoeffnete Datei
anders behandelt als eine, die nur als Animationsquelle dient. Die jetzige
Fassung entspricht Ravens `_humanoid.glm` in allen 84 Shadernamen exakt,
was unabhaengig davon der sichere Zustand ist.

Mit den beiden Regeln oben stimmen **alle 84 Shadernamen exakt** mit Ravens
Datei ueberein, und der Hierarchieblock ist byteweise identisch.

---

## Wickelrichtung der Dreiecke

dotXSI und GLM zaehlen die Ecken **gegenlaeufig**. Beim Uebertragen muessen
zwei Indizes getauscht werden.

An Ravens `_humanoid.glm` gemessen: dort zeigt die aus den Positionen
berechnete Flaechennormale bei **allen 2846 Dreiecken** entgegengesetzt zur
gemittelten Vertexnormale. Ohne Umkehrung kommt bei allen 2846 das Gegenteil
heraus — 100 % in die eine, 100 % in die andere Richtung, kein Graubereich.

Sichtbar wird das als scheinbar **umgedrehte Normalen**: die Flaechen werden
von innen gerendert, das Modell sieht ausgestuelpt aus. Die Normalen selbst
sind dabei voellig in Ordnung — im Vergleich mit der Originaldatei stimmen
89,6 % exakt ueberein und **kein einziger** ist invertiert.

Das ist der Grund, warum sich der Fehler schlecht am Vektor festmachen
laesst: wer die Normalen prueft, findet nichts. Der aussagekraeftige Test ist
das Vorzeichen von `dot(Flaechennormale, Vertexnormale)` — es ist bei einer
korrekten Datei einheitlich, und sein Umschlagen zeigt die Wickelrichtung.


---

## LODs

Verifiziert an Ravens `model.glm` (Luke): 4 LODs, 80 Surfaces, 2208 / 1556 /
943 / 943 Vertices.

```
Header
Offsettabelle der Surface-Hierarchie   (einmal, fuer alle LODs gemeinsam)
Surface-Hierarchie                     (Namen, Flags, Shader, Eltern/Kinder)
LOD 0:  mdxmLOD { ofsEnd }
        int offsets[numSurfaces]       relativ zum Anfang DIESER Tabelle
        Surfaces
LOD 1:  ...
```

Wichtig:

- **Die Surface-Hierarchie steht nur einmal.** Namen, Flags, Shader und
  Eltern/Kind-Beziehungen gelten fuer alle LODs; nur Geometrie und
  Gewichtung unterscheiden sich.
- `mdxmLOD::ofsEnd` ist **relativ zum LOD-Anfang**. Der naechste LOD beginnt
  bei `lod + ofsEnd`, der letzte endet am Dateiende.
- `offsets[0]` ist immer `numSurfaces * 4` — die erste Surface liegt direkt
  hinter der Offsettabelle, und die Werte zaehlen ab deren Anfang, nicht ab
  dem LOD-Anfang.
- Jede Surface traegt in allen LODs denselben `thisSurfaceIndex`.

Der Writer beherrscht das; ein Test baut vier LODs mit abnehmender Dichte und
prueft Offsets, Rueckverweise und den Kettenschluss.

**Offen bleibt die Quellseite:** woran Carcass in der dotXSI erkennt, welches
Mesh zu welchem LOD gehoert. Im Buildlog des Originals steht
`Processing meshes, LOD 0` / `Processing meshes, LOD 1` /
`(Mesh empty, so total LODs = 1)`, es gibt also einen Durchlauf je LOD. Ohne
eine `root.xsi` mit mehreren LOD-Saetzen laesst sich die Namenskonvention
nicht bestimmen.
