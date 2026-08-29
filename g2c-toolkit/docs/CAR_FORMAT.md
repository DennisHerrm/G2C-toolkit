# `.car`-Skriptformat, verifiziert an `_humanoid.car`

1311 Zeilen, 1289 `$aseanimgrab`. Alle Befehle werden vom Parser erkannt.

## Tatsaechlich verwendete Befehle

| Befehl | Haeufigkeit | Bedeutung |
|---|---|---|
| `$aseanimgrab` | 1289 | Animationsdatei einsammeln |
| `$pcj` | 17 | Player Controlled Joint |
| `$scale` | 1 | Buildscale (hier 0.64) |
| `$keepmotion` | 1 | "Motion"-Bone erhalten |
| `$aseanimgrabinit` | 1 | Sammelvorgang beginnen |
| `$aseanimgrabfinalize` | 1 | Sammelvorgang abschliessen |
| `$aseanimconvertmdx_noask` | 1 | GLA/GLM erzeugen |

`$pcj` fehlte in der ersten Fassung des Parsers — der Befehl steht zwar im
Binary, ich hatte ihn im Strings-Dump faelschlich fuer Rauschen gehalten.

## Syntax

```
$pcj $flatten                  // Schalter, kein Bonename
$pcj upper_lumbar              // 16 weitere: Bonenamen

$aseanimgrab <datei.xsi> [-loop N] [-framespeed N] [-enum NAME]
             [-qdskipstart] [-additional t c l s NAME]... [-qdskipstop]

$aseanimconvertmdx_noask <root> -makeskel <glaPfad> -origin x y z
```

`-additional t c l s NAME` erzeugt einen zusaetzlichen animation.cfg-Eintrag,
der in dieselben gesammelten Frames zeigt: Zielframe-Versatz, Frameanzahl,
Loopframe, Framespeed, Name.

Ohne `-enum` ergibt sich der Sequenzname aus dem Dateinamen ohne Pfad und
Endung in Grossbuchstaben. **Gegen die echte animation.cfg geprueft: 1417 von
1425 Namen stimmen.**

## animation.cfg

Das Format dokumentiert sich selbst im Dateikopf:

```
// Format:  enum, targetFrame, frameCount, loopFrame, frameSpeed
FACE_ALERT          	0	2	-1	1
```

Name auf 20 Zeichen mit Leerzeichen aufgefuellt, dann Tabulatoren,
Zeilenende CRLF.

## Vorgabewerte ohne Flags — an Ravens animation.cfg abgelesen

Fehlt einer Grab-Zeile ein Flag, ist der Wert **nicht** frei waehlbar:

| fehlendes Flag | Carcass schreibt |
|---|---|
| `-loop` | `0` (nicht -1) |
| `-framespeed` | die Framerate aus `SI_Scene` der jeweiligen `.xsi` |

Beides durch Vergleich von 1477 gemeinsamen Sequenzen bestimmt: 45 Zeilen ohne
`-loop` stehen im Original mit Loopframe 0, und 144 Zeilen ohne `-framespeed`
mit Werten von 5 bis 60 — also keinem festen Vorgabewert.

`SI_Scene` sieht so aus:

```
SI_Scene js_strooper_hand_signal_02 {
    "FRAMES",
    0.000000,
    72.000000,
    20.000000,
}
```

Also Startframe, Endframe, Framerate. Bei `torso_handsignal2.xsi` ergibt das
73 Frames bei Rate 20 — genau die Werte in Ravens animation.cfg. `SI_Scene`
ist damit auch die maszgebliche Quelle fuer den Framebereich, nicht die
Keyframes der FCurves.

## `-qdskipstart` / `-qdskipstop`

Noch nicht sicher gedeutet. Carcass hat gleichnamige Kommandozeilenoptionen,
es handelt sich also um Regionen, die unter einer globalen Bedingung
uebersprungen werden ("quick and dirty"). In `_humanoid.car` liegen 394 der
1683 Sequenzen in solchen Regionen.

Auffaellig: von den 75 Sequenznamen, die sich aus der .car ableiten lassen
aber nicht in der animation.cfg stehen, liegen 74 in einer `-qdskip`-Region.
Das ist ein starkes Indiz, aber kein Beweis — die mitgelieferten Dateien
stammen aus verschiedenen Builds (siehe unten).

## Verifizierte GLM-Details

Am echten `_humanoid.glm` (84 Surfaces, 2647 Verts, 2846 Tris) geprueft:

- `ofsSurfHierarchy` = `164 + numSurfaces*4` = 500 — Offsettabelle liegt direkt
  hinter dem Header, Werte relativ zum Headerende.
- **Die LOD-Surface-Offsets sind relativ zur Offsettabelle** (`lod + 4`), NICHT
  zu `lod`. Beim Original steht fuer Surface 0 der Wert 336 = 84*4. Mit `lod`
  als Basis waere alles um 4 Byte verschoben. Genau diesen Fehler hatte die
  erste Fassung des Writers.
- `ofsHeader` jeder Surface ist der negierte absolute Dateioffset dieser
  Surface. Bei allen 84 konsistent.
- `mdxmSurface.ident` ist 0.
- `mdxmLOD.ofsEnd` ist relativ zum LOD-Anfang und zeigt aufs Dateiende.
- Vertexgewichte: 10-Bit-Rohwert, `raw/1023` ergibt das Gewicht. Bei `hips`
  Vertex 0: ein Gewicht, lokaler Bone 0 → globaler Bone 1 (`pelvis`), raw 1023
  = 1,0 — passend zum `SI_Envelope` "hips" → "pelvis" mit 100 %.

## Achtung: Versionsversatz im Beispielmaterial

| Datei | Frames | Hinweis |
|---|---|---|
| `_humanoid.gla` | 21554 | Bone heisst `face` |
| `animation.cfg` | 21554, 1425 Sequenzen | passt zur GLA |
| `_humanoid_info.txt` | 30384 | Bone heisst `face_always_`, 15,0 MB |
| `_humanoid.car` | 1683 Sequenzen | wieder ein anderer Stand |

GLA und animation.cfg passen zueinander. Die info.txt und die .car beschreiben
jeweils andere Builds. Fuer eine byte-exakte Verifikation braucht es einen in
sich stimmigen Satz aus einem einzigen Carcass-Lauf.
