// g2/xsi_mesh.h — Mesh aus dotXSI lesen.
//
// Gegenstueck zu xsi_anim.h: dort die Animation, hier die Geometrie.
// Verifiziert gegen Ravens _humanoid.glm (84 Surfaces, 2647 Verts, 2846 Tris).

#pragma once

#include "g2/model.h"
#include "g2/xsi.h"

#include <map>
#include <string>
#include <vector>

namespace g2::xsi {

struct MeshImportOptions {
    // $scale aus der .car. Muss mit der Referenz-GLA uebereinstimmen.
    float scale = 1.0f;

    // Name der GLM, wie er in den Header geschrieben wird.
    std::string modelName;

    // GLA-Referenz ohne Endung, z.B. "models/players/_humanoid/_humanoid".
    std::string animName;

    // Bonenamen des Referenzskeletts. Die Envelope-Eintraege nennen Bones
    // beim Namen; ohne diese Liste liesse sich kein Index bilden.
    std::vector<std::string> boneNames;

    // Umbenennungen Referenz -> dotXSI, wie bei der Animation.
    std::map<std::string, std::string> aliases;

    // Toleranzen fuer die Vertexzusammenfassung.
    //
    // dotXSI hat je Attribut ein eigenes Indexarray, GLM nur einen Index pro
    // Vertex. Zusammengefasst wird nach Positionsindex, UV und Normale — aber
    // NICHT ueber einen Hashschluessel, sondern durch gierige Suche mit
    // Toleranz: der erste passende Kandidat gewinnt.
    //
    // Der Unterschied ist wesentlich. Mit exakter Gleichheit entstehen zu
    // viele Vertices, und kein Rundungsgitter kann das ausgleichen — die
    // Zusammenfassung ist reihenfolgeabhaengig, nicht wertdiskret. Dieselbe
    // Regel benutzt auch mrwonkos Blender-Exporter.
    float normalTolerance = 0.05f;
    // 0,002 statt exakter Gleichheit: an allen 84 Surfaces gemessen der
    // beste Wert (81 exakt gegen 77 bei exakter Gleichheit). Groessere Werte
    // treffen zwar die Gesamtsumme, aber durch sich aufhebende Fehler.
    float uvTolerance = 0.002f;
};

struct MeshImportStats {
    std::size_t   surfaces = 0;
    std::size_t   vertices = 0;
    std::size_t   triangles = 0;
    std::size_t   tags = 0;          // Surfaces mit *-Praefix
    std::size_t   offSurfaces = 0;   // Surfaces mit _off
    std::uint64_t splitVertices = 0; // durch Attributaufloesung entstandene
    std::vector<std::string> warnings;
};

struct MeshImportResult {
    Mesh            mesh;
    MeshImportStats stats;
};

// Wandelt einen Surfacenamen aus der dotXSI in den GLM-Namen um.
//
// Zwei Regeln, an Ravens _humanoid.glm abgelesen:
//   - alles klein: "Stupidtriangle_off" -> "stupidtriangle_off"
//   - Praefix "bolt_" wird zu "*":  "bolt_back" -> "*back"
//
// Die zweite Regel betrifft 46 der 84 Surfaces, und genau 46 nennt Ravens
// Info-Datei "tags only".
std::string surfaceNameToGlm(const std::string& xsiName);

// Flags aus dem Namen ableiten:
//   *-Praefix   -> kSurfFlagIsBolt (1)
//   Endung _off -> kSurfFlagOff (2)
// In der Originaldatei tragen 46 Surfaces Flag 1 und 17 Flag 2 — beide Zahlen
// stehen so in Ravens Info-Datei.
std::uint32_t surfaceFlagsFromName(const std::string& glmName);

MeshImportResult importMesh(const Document& doc, const MeshImportOptions& opt);
MeshImportResult importMeshFile(const std::string& path, const MeshImportOptions& opt);

}  // namespace g2::xsi
