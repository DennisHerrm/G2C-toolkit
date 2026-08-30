# g2c — Ghoul2 Toolkit

**A modern replacement for Carcass and Assimilate, in a single executable.**

g2c builds `.gla` animation files and `.glm` meshes from dotXSI sources — and takes finished `.gla` files apart again, back into editable `.xsi`.

One file. No installer, no runtime package, no dependencies.

[![g2c in action](https://img.youtube.com/vi/NWsVS_ckK_o/hqdefault.jpg)](https://www.youtube.com/watch?v=NWsVS_ckK_o)

▶ **[Watch on YouTube](https://www.youtube.com/watch?v=NWsVS_ckK_o)**

---

## Why

Raven's original tools are from 2003. Carcass is a command-line compiler with no error recovery; Assimilate is a GUI over it that has not been updated since. Between them they still do the job, but they were never meant for the way people mod today — mixing animations from several sources, porting between JKA and JK2, or working with custom skeletons.

g2c does what they did, more accurately, and adds the direction they never had: **taking a finished GLA apart**.

---

## What it does

### Build (XSI → GLA)

- Reads all 19 `.car` directives, including `-additional`, `-qdskipstart`, `$pcj`, `$keepmotion`
- Writes `.gla`, `.glm`, `animation.cfg`, `.frames`, `.skin`
- Opens a whole model tree at once — each `.car` becomes a tab
- Sequence editor: enum, loop frame, frame speed, sub-ranges
- Copy and paste sequences **between scripts**
- Validation that reports missing files by **sequence name and line number**
- Batch build across every open script

### Extract (GLA → XSI)

- Open a `.gla`; `animation.cfg` and `.frames` are found automatically
- Export selected sequences, or **everything plus a working `.car`**
- Root motion is restored — the `.frames` file is not required, it can be reconstructed from the GLA itself
- **Compare two humanoids**: see which sequences are missing in the other, select and export exactly those
- Export as **dotXSI 3.0 or 3.5**, whichever your toolchain expects

### Preview

Play back a sequence's skeleton. Orbit, zoom, scrub frames, at the rate from `animation.cfg`.

### Command line

The same executable works from a console:

```
g2c build _humanoid.car -ref _humanoid.gla -basedir C:\base
g2c export _humanoid.gla -cfg animation.cfg -o out\ -car out\_humanoid.car
g2c export _humanoid.gla -cfg animation.cfg -o out\ -xsi 3.5
g2c makecar <folder>     build a .car from a folder of .xsi files
g2c diff a.gla b.gla     compare two files bone by bone
g2c validate / info / check / mesh / anim / xsi / car / scan / about
```

---

## Accuracy

Measured against Raven's own `_humanoid.gla`, not estimated.

| | Carcass | g2c |
|---|---|---|
| Max rotation error | 0.0181° | **0.0061°** |

**Round trip** — Raven's complete JKA humanoid taken apart and rebuilt from the generated `.car`:

| | |
|---|---|
| Frames | 30384 — identical |
| Sequences | 1683 — identical |
| `animation.cfg` | **1683 of 1683 identical** |
| Outside tolerance | **169 of 1.6 million (0.010%)** |
| Mean deviation | 0.0002° / 0.0001 units |

**Deterministic.** The same input always produces a byte-identical `.gla`, regardless of thread count or cache state.

**Mesh output** matches Raven's on names, flags, shaders, hierarchy and bone references — 84 of 84 surfaces.

---

## Carcass bugs that are fixed

Found by disassembling `carcass.exe` and measuring against real files.

**Truncation instead of rounding.** `SquashFloat` at `0x004181D0` tail-jumps into `_ftol`, which truncates toward zero. That doubles the mean error and — worse — makes it *directional*: every component drifts the same way, so errors accumulate along a bone chain instead of cancelling. That is the bone jitter people see on fingers and weapon tags.

**Out-of-range values wrap.** A quaternion component outside ±2 is written as `0`, which decodes to **−2.0** — the opposite end. The bone flips.

**Silent limit violations.** Several hard format limits are checked in a way that lets bad data through and produces a file the engine mis-reads later.

**No candidate search.** Rounding each quaternion component independently does not give the nearest *rotation*. g2c evaluates the neighbours and keeps the best.

---

## dotXSI 3.0 and 3.5

The two versions differ in more than the header number:

| | v3.0 | v3.5 |
|---|---|---|
| Templates | **named** — `SI_FCurve <bone>-SCALING-X { ... }` | unnamed — `SI_FCurve { "<bone>", "SCALING-X", ... }` |
| Raven uses it for | `root.xsi`, both JKA and JK2 | the animation files |

g2c writes **3.0 by default** and can write 3.5 on request — pick it next to the export buttons, or pass `-xsi 3.5`. The content is identical either way: exporting the same sequence in both and reading them back gives a difference of **0.000000000**.

Older tools sometimes expect 3.0, which is why it is the default.

## Requirements

Windows 10 or 11, 64-bit. Nothing else — the runtime is linked statically, so there is no Visual C++ Redistributable to install. If you are unsure about a build of your own, the program tells you: **View → About g2c**.

Interface in **German, English, 中文 and 日本語**, switchable at runtime.

---

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/g2_tests
```

On Windows, `build.bat` does the same and checks that the result is redistributable.

Needs a C++20 compiler. Dear ImGui 1.92.1 is vendored; there are no other dependencies.

---

## Known limitations

Stated openly, because you will run into them eventually.

**JK2 finger bones.** JK2's `_humanoid.gla` has a flat hierarchy — 46 of its 72 bones hang directly off the chest. Combined with base poses that have unequal axis lengths, this produces shear the `.xsi` format cannot represent, amplified about fifteenfold by the long lever arm. Finger bones can be off by up to 0.1 units; torso, head and legs are unaffected. This is a limit of the format, not the tool.

**`$pcj` cannot be recovered.** The list of player-controlled joints exists only in the `.car` and leaves no trace in the `.gla`. If you replace an existing `.car` with a generated one, copy that block across by hand.

**26 extra vertices.** Mesh export produces 2673 vertices against Raven's 2647, in the hips and both hands. The model renders correctly, but it is not bit-identical. Unexplained.

**Unsigned executable.** Windows Defender flags unknown unsigned programs on suspicion. Windows Security → Protection history → restore, or exclude that one folder.

---

## Two things that will save you an evening

**`animation.cfg` belongs with the `.gla`.** Inserting animations shifts every following target frame. If the old config stays in the game folder, every name points at whatever animation now sits at that frame — and it looks exactly like broken animations.

**The GLA name lives in the file header.** It tells the engine which skeleton this is. g2c takes it from `-makeskel`; if that is wrong, a custom humanoid announces itself as the standard one and the engine uses the wrong config. This one cost weeks to find, and it was visible in ten seconds with a hex editor.

---

## Verification

410 automated checks run on every build, including round-trip tests against real files. Address, behaviour and thread sanitizers run clean over the whole suite and over real data. 150 deliberately corrupted `.gla` files produce zero crashes.

Three static checkers run at build time for the parts that cannot be unit-tested: the Win32 layer, GUI wiring, and `build.bat`.

Where something could not be verified, it is documented rather than assumed.

---

## Credits

**g2c by DennisH.**

- **Raven Software** for Carcass, Assimilate and the Ghoul2 formats
- **Dear ImGui** for the interface layer

Format details were worked out against Raven's own asset files. No code from other tools was used.

---

## Feedback

Verified against JKA, JK2 and two custom humanoids — a 43-bone model at scale 0.6, and a droid rig. That covers a lot, but not everything.

If you have a skeleton with unusual bone names, a different scale, or a model that behaves oddly, that is the interesting case. Please open an issue.

If it does not start, `g2c-startup.log` next to the executable records every startup step and names the one that failed. Attaching that file is the fastest way to get it diagnosed.
