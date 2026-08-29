// g2/parallel.h — Minimaler paralleler for-Loop.
//
// Bewusst mit std::thread statt std::execution::par: die Parallel-Algorithmen
// der Standardbibliothek verlangen auf GCC und Clang zwingend Intel TBB und
// funktionieren nur unter MSVC ohne Zusatzbibliothek. Fuer ein Werkzeug, das
// mit nichts als einem C++20-Compiler bauen soll, ist das ein zu hoher Preis
// fuer ein paar Zeilen Ersparnis.

#pragma once

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace g2 {

inline unsigned defaultThreadCount() {
    const unsigned n = std::thread::hardware_concurrency();
    return n ? n : 4u;
}

// Ruft body(i, worker) fuer i in [0, count) auf, verteilt auf mehrere Threads.
//
// "worker" ist eine Nummer von 0 bis threads-1 und bleibt fuer einen Thread
// gleich. Damit laesst sich je Thread ein eigener Zaehler fuehren, ohne
// Sperre und ohne Datenrennen.
//
// WARUM ALS PARAMETER und nicht ueber thread_local:
//
// Ein thread_local ueberlebt den Aufruf. Laeuft parallelFor seriell — bei
// einem Kern oder threads=1 —, ist der ausfuehrende Thread der Hauptthread,
// und dessen Wert steht beim naechsten Aufruf noch da. Wird dann mit weniger
// Threads gearbeitet, zeigt der gespeicherte Index hinter das Ende des
// Zaehlerfeldes. Das faellt in keinem Test auf, weil es nur bei wechselnder
// Threadzahl zuschlaegt.
//
// Ausnahmen aus body werden aufgefangen und nach dem Join erneut geworfen,
// damit ein Fehler in einer Datei nicht das ganze Programm mitreisst.
template <typename F>
void parallelForWorker(std::size_t count, F&& body, unsigned threads = 0) {
    if (count == 0) return;
    if (threads == 0) threads = defaultThreadCount();
    threads = std::min<unsigned>(threads, static_cast<unsigned>(count));

    if (threads <= 1) {
        for (std::size_t i = 0; i < count; ++i) body(i, 0u);
        return;
    }

    // Arbeit blockweise vergeben statt einzeln.
    //
    // Ein fetch_add je Element ist bei kurzen Aufgaben der Engpass: bei
    // 30384 Frames zu je wenigen Mikrosekunden schlagen sich acht Threads
    // um dieselbe Cachezeile. Bloecke von etwa 64 Elementen verteilen die
    // Last immer noch fein genug — die Aufgaben sind ungleich lang, eine
    // feste Aufteilung waere schlechter —, kosten aber ein Vierundsechzigstel
    // der Synchronisation.
    const std::size_t chunk = std::max<std::size_t>(1, std::min<std::size_t>(64, count / (threads * 8) + 1));

    std::atomic<std::size_t> next{0};
    std::mutex               errMutex;
    std::exception_ptr       firstError;

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            for (;;) {
                const std::size_t begin = next.fetch_add(chunk, std::memory_order_relaxed);
                if (begin >= count) return;
                const std::size_t end = std::min(begin + chunk, count);
                try {
                    for (std::size_t i = begin; i < end; ++i) body(i, t);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(errMutex);
                    if (!firstError) firstError = std::current_exception();
                    return;
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    if (firstError) std::rethrow_exception(firstError);
}

// Ohne Threadnummer, fuer Schleifen, die keine brauchen.
template <typename F>
void parallelFor(std::size_t count, F&& body, unsigned threads = 0) {
    parallelForWorker(
        count, [&](std::size_t i, unsigned) { body(i); }, threads);
}

}  // namespace g2
