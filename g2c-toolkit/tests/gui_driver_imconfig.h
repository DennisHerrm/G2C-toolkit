// Nur fuer tests/gui_driver.cpp: ImGui-Zusicherungen abfangen statt abbrechen.
//
// Der Treiber klickt sich durch jedes Fenster. Eine fehlgeschlagene
// Zusicherung soll dort als Fehler im Protokoll landen, mit Datei und Zeile,
// statt den Lauf ohne Hinweis zu beenden.
#pragma once

void g2DriverAssert(const char* expr, const char* file, int line);

#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : g2DriverAssert(#_EXPR, __FILE__, __LINE__))
