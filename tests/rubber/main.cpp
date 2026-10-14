// Rubber-on-track checks: the map wraps at the start line, cells fill up and saturate, the grip gain
// stays inside its limit, and cells off the line stay green. Exits non-zero on the first failure.
#include <cmath>
#include <cstdio>

#include "rubber.hpp"

static int fails = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++fails;                                              \
        }                                                         \
    } while (0)

int main() {
    rr::TrackRubber r;
    CHECK(r.empty() && r.at(10, 0) == 0.0f && r.grip(10, 0) == 1.0f);  // no map: green and no gain
    r.reset(1000.0f);
    CHECK(!r.empty() && r.cols() == 250);
    CHECK(r.at(100, 0) == 0.0f && r.grip(100, 0) == 1.0f);

    r.add(100, 0.2f, 1e-4f);  // a little wear on the spot
    CHECK(r.at(100, 0.2f) > 0.0f && r.at(100, 0.2f) < 0.05f);
    CHECK(r.at(101.5f, 0.3f) == r.at(100, 0.2f));              // same 4 m x 0.5 m cell
    CHECK(r.at(104.5f, 0.2f) == 0.0f && r.at(100, 1.2f) == 0.0f);  // the next cells are untouched
    CHECK(r.at(1100.0f, 0.2f) == r.at(100, 0.2f));             // distances wrap at the line
    CHECK(r.at(-900.0f, 0.2f) == r.at(100, 0.2f));

    for (int k = 0; k < 1000; ++k) r.add(100, 0.2f, 1e-3f);
    CHECK(r.at(100, 0.2f) == 1.0f);                                          // saturates
    CHECK(std::fabs(r.grip(100, 0.2f) - (1.0f + rr::TrackRubber::kGain)) < 1e-6f);
    CHECK(r.grip(100, 0.2f) <= 1.031f && r.grip(100, 3.0f) == 1.0f);         // off the line: green

    r.add(100, 40.0f, 1.0f);   // outside the map: ignored
    CHECK(r.at(100, 40.0f) == 0.0f && r.at(100, -40.0f) == 0.0f);
    CHECK(r.meanLine() > 0.0f && r.meanLine() < 0.01f);

    r.reset(1000.0f);          // a new map starts green again
    CHECK(r.at(100, 0.2f) == 0.0f && r.meanLine() == 0.0f);

    if (fails == 0) std::printf("rubber ok\n");
    return fails ? 1 : 0;
}
