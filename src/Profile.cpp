// [TraitExt] Profile=yes - measure where the per-frame cost actually goes.
//
// Written because "does it feel slow" is not evidence. The two things that
// could plausibly cost real time here are (a) proximity gates, which scan a
// radius for every gated unit, and (b) type conversion, which re-registers the
// unit with its house. Both are easy to reason about wrongly: a conversion
// LOOKS expensive and happens at most a few times per unit per minute, while a
// proximity query looks cheap and runs for every gated unit forever.
//
// So count both, time the whole evaluator, and print one line a minute. The
// numbers are what Rex reports back - not an impression.

#include "TraitEngine.h"

#include <Utilities/Debug.h>
#include <Unsorted.h>

#include <windows.h>

namespace TraitExt
{
    namespace Profile
    {
        namespace
        {
            bool g_On = false;

            long long g_Freq = 0;       // QPC ticks per second
            long long g_Ticks = 0;      // ticks inside the evaluator this window

            int g_Evals = 0;            // gate evaluations (unit x pass)
            int g_Queries = 0;          // proximity radius scans
            int g_Converts = 0;         // type conversions applied
            int g_Restores = 0;         // conversions undone
            int g_Opens = 0;            // gate transitions to open
            int g_Closes = 0;           // gate transitions to closed
            int g_Held = 0;             // closes SUPPRESSED by the dwell floor

            int g_WindowStart = 0;
            const int kWindow = 900;    // ~60s at 15fps logic

            long long Now()
            {
                LARGE_INTEGER t {};
                QueryPerformanceCounter(&t);
                return t.QuadPart;
            }
        }

        void SetEnabled(bool on)
        {
            g_On = on;
            if (!on)
                return;

            LARGE_INTEGER f {};
            QueryPerformanceFrequency(&f);
            g_Freq = f.QuadPart;

            g_Ticks = 0;
            g_Evals = g_Queries = g_Converts = g_Restores = 0;
            g_Opens = g_Closes = g_Held = 0;
            g_WindowStart = 0;

            Debug::Log("[TraitExt] PROFILE on - one line every %d frames. "
                "QPC frequency %lld/s\n", kWindow, g_Freq);
        }

        bool Enabled() { return g_On; }

        void CountQuery()   { if (g_On) ++g_Queries; }
        void CountConvert() { if (g_On) ++g_Converts; }
        void CountRestore() { if (g_On) ++g_Restores; }
        void CountOpen()    { if (g_On) ++g_Opens; }
        void CountClose()   { if (g_On) ++g_Closes; }
        void CountHeld()    { if (g_On) ++g_Held; }

        long long Begin()
        {
            if (!g_On)
                return 0;
            ++g_Evals;
            return Now();
        }

        void End(long long started)
        {
            if (!g_On || !started)
                return;
            g_Ticks += Now() - started;
        }

        // Called from the logic tick. Prints at most one line per window, and
        // reports the evaluator's cost as a share of real time so the number
        // means something without knowing the machine.
        void Tick()
        {
            if (!g_On)
                return;

            const int now = Unsorted::CurrentFrame;
            if (g_WindowStart == 0)
            {
                g_WindowStart = now;
                return;
            }
            const int elapsed = now - g_WindowStart;
            if (elapsed < kWindow)
                return;

            const double ms = g_Freq
                ? (static_cast<double>(g_Ticks) * 1000.0 / static_cast<double>(g_Freq))
                : 0.0;
            // A logic frame is nominally 1/15s = 66.7ms, so this is the share
            // of the frame budget TraitExt spent. Over a few percent and the
            // feature is worth switching off; under one and the cost is noise.
            const double budget = static_cast<double>(elapsed) * (1000.0 / 15.0);
            const double pct = budget > 0.0 ? (ms * 100.0 / budget) : 0.0;

            Debug::Log("[TraitExt] PROFILE %d frames: %d evals, %d proximity scans, "
                "%d converts, %d restores, %d opens, %d closes, %d holds | "
                "%.1f ms total = %.2f%% of the logic budget | %.1f us per eval\n",
                elapsed, g_Evals, g_Queries, g_Converts, g_Restores,
                g_Opens, g_Closes, g_Held, ms, pct,
                g_Evals ? (ms * 1000.0 / g_Evals) : 0.0);

            g_Ticks = 0;
            g_Evals = g_Queries = g_Converts = g_Restores = 0;
            g_Opens = g_Closes = g_Held = 0;
            g_WindowStart = now;
        }
    }
}
