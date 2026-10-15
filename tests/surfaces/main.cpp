// Surface checks: the table, the track file's "surface" / "offtrack" / "gravel" lines, kerbs and
// the automatic gravel traps. Exits non-zero on the first failure.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "track.hpp"

static int fails = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++fails;                                              \
        }                                                         \
    } while (0)

int main() {
    using rr::surfaceProps;
    // the table: kerbs hold nearly all the grip and add no drag; the loose stuff is slower
    CHECK(surfaceProps(RR_SURF_TARMAC).mu == 1.0f);
    CHECK(surfaceProps(RR_SURF_KERB).mu > 0.95f && surfaceProps(RR_SURF_KERB).drag == 0.0f);
    CHECK(surfaceProps(RR_SURF_GRASS).mu < surfaceProps(RR_SURF_KERB).mu);
    CHECK(surfaceProps(RR_SURF_GRAVEL).mu < surfaceProps(RR_SURF_GRASS).mu);
    CHECK(surfaceProps(RR_SURF_GRAVEL).drag > surfaceProps(RR_SURF_GRASS).drag);
    CHECK(rr::surfaceFromName("gravel") == RR_SURF_GRAVEL && rr::surfaceFromName("nonsense") < 0);

    // an oval (pit area on the left) with a dirt strip and run-off tarmac on the right, and no automatic gravel
    const std::string src = std::string(RR_SOURCE_DIR) + "/tracks/oval.trk";
    std::ifstream in(src);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    CHECK(!text.empty());
    const std::string tmp = "surfaces_test.trk";
    {
        std::ofstream out(tmp);
        out << text << "\ngravel none\nsurface dirt right 100 200\nsurface runoff right 300 400 1.2 4\n";
    }
    rr::Track t;
    std::string err;
    CHECK(t.load(tmp, &err));
    const float hw = t.at(t.indexAt(150)).halfWidth;
    CHECK(t.surfaceAt(150, 0, hw) == RR_SURF_TARMAC);
    CHECK(t.surfaceAt(150, -(hw + 3), hw) == RR_SURF_DIRT);          // inside the zone, right
    CHECK(t.surfaceAt(150, hw + 3, hw) == RR_SURF_PIT);              // the other side is the pit area
    CHECK(t.surfaceAt(250, -(hw + 3), hw) == RR_SURF_GRASS);         // outside the zone
    CHECK(t.surfaceAt(350, -(hw + 3), hw) == RR_SURF_RUNOFF && t.paved(350, -(hw + 3), hw));
    CHECK(t.surfaceAt(350, -(hw + 5), hw) == RR_SURF_GRASS);         // past "to"
    CHECK(!t.paved(150, -(hw + 3), hw));
    for (int i = 0; i < t.size(); ++i) CHECK(t.gravelSide(i) == 0.0f);

    // the default: gravel traps on the outside of the corners, kerbs along them
    rr::Track a;
    CHECK(a.load(std::string(RR_SOURCE_DIR) + "/tracks/highmoor.trk", &err));
    bool trap = false, kerb = false;
    for (int i = 0; i < a.size() && !(trap && kerb); ++i) {
        const float h = a.at(i).halfWidth, s = a.at(i).s;
        const float g = a.gravelSide(i);
        if (std::fabs(g) > 0.9f) {
            trap = true;
            CHECK(a.surfaceAt(s, (g > 0 ? 1 : -1) * (h + 4), h) == RR_SURF_GRAVEL);
            CHECK(a.surfaceAt(s, -(g > 0 ? 1 : -1) * (h + 4), h) != RR_SURF_GRAVEL);
        }
        if (a.kerbAt(i)) {
            kerb = true;
            CHECK(a.surfaceAt(s, h + 0.6f, h) == RR_SURF_KERB && a.paved(s, h + 0.6f, h));
        }
    }
    CHECK(trap && kerb);

    // "barrier" zones set the wall's distance (easing in and out), the narrowest is runoff(); "feature" lines are kept
    {
        std::ofstream out(tmp);
        out << text << "\nbarrier right 300 500 25\nbarrier both 700 800 4\nfeature tyrewall left 100 160 2\nfeature bridge both 400 400\n";
    }
    rr::Track b;
    CHECK(b.load(tmp, &err));
    const float bh = b.at(b.indexAt(400)).halfWidth;
    CHECK(std::fabs(b.barrierOffset(400, -1, bh) - (bh + 25.0f)) < 0.5f);     // inside the zone, right
    CHECK(std::fabs(b.barrierOffset(400, 1, bh) - (bh + b.runoff())) > 1.0f);  // the left stays the default (or the narrowest)
    CHECK(std::fabs(b.barrierOffset(750, 1, bh) - (bh + 4.0f)) < 0.5f && std::fabs(b.barrierOffset(750, -1, bh) - (bh + 4.0f)) < 0.5f);
    CHECK(b.runoff() <= 4.0f + 1e-3f);
    CHECK(std::fabs(b.barrierOffset(299, -1, bh) - b.barrierOffset(301, -1, bh)) < 2.5f);  // eased: no step across the zone start
    CHECK(b.features().size() == 2 && b.features()[0].kind == "tyrewall" && b.features()[0].side == 1 &&
          b.features()[0].args.size() == 1 && b.features()[1].kind == "bridge" && b.features()[1].side == 0);
    {
        std::ofstream out(tmp);
        out << text << "\nbarrier up 1 2 3\n";
    }
    rr::Track bad;
    CHECK(!bad.load(tmp, &err));

    std::remove(tmp.c_str());
    std::printf(fails ? "surfaces: %d failure(s)\n" : "surfaces ok\n", fails);
    return fails ? 1 : 0;
}
