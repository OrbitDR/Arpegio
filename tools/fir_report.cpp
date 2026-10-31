#include <cmath>
#include <cstdio>
#include "dsd/dsd_fir.h"
using namespace arpegio;
static double db(double m) { return 20 * std::log10(m); }
int main() {
    struct C { uint32_t rate; uint32_t pcm; } cases[] = {{2822400, 176400}, {2822400, 88200}, {5644800, 352800}, {5644800, 176400},
        {11289600, 352800}, {22579200, 352800}, {22579200, 176400}, {3072000, 192000}, {24576000, 384000}};
    for (auto c : cases) {
        uint32_t r = c.pcm;
        auto plan = plan_dsd_to_pcm(c.rate, std::span<const uint32_t>(&r, 1));
        auto taps = design_dsd_fir(c.rate, plan->filter);
        // Time-domain figure: span holding all but 1e-6 of the impulse energy.
        double e = 0; for (double t : taps) e += t * t;
        double acc = 0; size_t lo = 0; while (acc < e * 0.5e-6) acc += taps[lo] * taps[lo], ++lo;
        printf("%8u->%6u div=%3u taps=%5zu (%.0f us, 99.9999%% energy %.0f us) f3=%.0f stop=%.0f |", c.rate, c.pcm, plan->divider, taps.size(),
               taps.size() * 1e6 / c.rate, (taps.size() - 2 * lo) * 1e6 / c.rate, plan->filter.f3_hz, plan->filter.stop_hz);
        for (double f : {20000., 24000., 30000., 40000., 50000., 70000., 90000.}) printf(" %.0fk=%.4f", f / 1000, db(fir_magnitude(taps, c.rate, f)));
        printf(" | stop %.1f, 1.02stop %.1f, 1.05stop %.1f\n", db(fir_magnitude(taps, c.rate, plan->filter.stop_hz)), db(fir_magnitude(taps, c.rate, plan->filter.stop_hz * 1.02)), db(fir_magnitude(taps, c.rate, plan->filter.stop_hz * 1.05)));
    }
}
