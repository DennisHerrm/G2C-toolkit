# Beispiele

## Ganzes .car-Skript abarbeiten

```sh
g2c build _humanoid.car -ref base/_humanoid.gla -basedir C:\assets
```

`-basedir` zeigt auf das Verzeichnis, unter dem `models/` liegt. Ergebnis sind
eine GLA und eine dazu passende `animation.cfg` mit den echten `-loop`- und
`-framespeed`-Werten aus dem Skript.

Fehlt eine referenzierte `.xsi`, bricht der Lauf ab und nennt sie. Das ist
Absicht — beim Ueberspringen wuerden sich alle nachfolgenden Zielframes
verschieben.


## Animationen zu einem vorhandenen _humanoid hinzufuegen

```sh
g2c anim base/_humanoid.gla neu/_humanoid.gla \
    -origin 0 0 24 \
    anims/BOTH_attack10.xsi \
    anims/BOTH_attack11.xsi
```

Das Skelett kommt vollstaendig aus der Referenz-GLA — Bonenamen, Reihenfolge,
Hierarchie und Basisposen bleiben bitweise erhalten. Nur die Frames werden neu
gebaut. Das ist entscheidend, weil jede Aenderung am Skelett alle vorhandenen
GLM-Modelle brechen wuerde.

Der `-origin`-Wert muss mit dem aus der urspruenglichen `.car` uebereinstimmen.
Bei Ravens `_humanoid` ist das `0 0 24`.

Nebenbei entsteht eine passende `_animation.cfg` mit den Zielframes. Framespeed
und Loopframe stehen dort auf Vorgabewerten; die echten Werte kommen aus den
`-framespeed`- und `-loop`-Flags der `.car`.

## Bone-Umbenennungen

Raven hat zwischen Builds umbenannt. Heisst ein Bone in der Referenz-GLA anders
als in den Animationsdateien:

```sh
g2c anim base/_humanoid.gla neu/_humanoid.gla \
    -alias face=face_always_ \
    anims/face_talk2.xsi
```

Ohne die Zuordnung bleiben der Bone und alles darunter in der Ruhepose.

## Vorher pruefen

```sh
g2c info  base/_humanoid.gla     # Bonenamen, Scale, Hierarchie
g2c xsi   anims/BOTH_attack10.xsi # welche Templates stecken drin
g2c car   _humanoid.car           # welche Sequenzen und Flags
```

`g2c info` zeigt den Scale der Referenz. Der wird automatisch uebernommen; ein
abweichender Wert wuerde die Basisposen unbrauchbar machen.

## Ergebnis pruefen

```sh
g2c check neu/_humanoid.gla
```

Meldet Skelettprobleme und misst den Quantisierungsfehler.
