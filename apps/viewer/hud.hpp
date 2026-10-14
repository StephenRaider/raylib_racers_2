#pragma once
#include <string>
#include <vector>

#include "raylib.h"
#include "anim.hpp"
#include "menu.hpp"
#include "race.hpp"
#include "renderer.hpp"
#include "testlog.hpp"

std::string lapTime(double t);         // "1:05.432", "-:--.---" for 0
Color compoundColor(int compound);     // red soft, yellow medium, white hard
const char* compoundName(int compound);

// One line of the qualifying classification.
struct QualiLine {
    std::string name;
    Color color;
    float time = 0;      // best lap, 0 = no time yet
    bool running = false;  // on track now
};

struct HudState {
    bool paused = false;
    float lights = -2;            // start lights: >0 counting down (5 red lights, one a second), <=0 lights out
    float timeScale = 1.0f;
    int focus = 0;
    bool followLeader = true;  // keep the camera on whoever is P1
    CamMode camera = CAM_CHASE;
    std::string directorCaption;  // what the director cam is showing
    bool showHud = true;
    bool showHelp = false;
    bool muted = false;
    // weekend mode
    bool qualifying = false;      // a practice or qualifying run is on track
    std::string sessionTitle = "QUALIFYING";  // which of the two
    int qualiRun = 0, qualiRuns = 0;
    std::vector<QualiLine> quali; // sorted: timed cars by time, then the rest
    ViewOptions view;
    std::string logPath;          // where the finished race's log was saved
    std::string notice;           // a line shown over the race-end screen (championship rounds)
    // Testing replays: what the car panel shows instead of the live clock (< 0 = live).
    float lapClock = -1, lastLap = -1, bestLap = -1;
    // Race-end screen: which window, and whether it is shown at all (G).
    int resultsWindow = 0;
    bool showResults = true;
};

// The testing screen: what is shown and where the cursor is.
struct TestView {
    const rr::TestRecorder* rec = nullptr;
    std::string title;        // algorithm and car
    std::string setup;        // tyres, fuel, stats
    int laps = 0;             // laps planned
    bool live = true;         // the cursor follows the car
    bool replay = false;      // a saved run: no car on track
    int runId = 0;            // the saved run (replay, or once saved)
    bool runOver = false;
    double cursor = 0;        // race time shown when not live
    bool playing = false;     // playing back from the cursor
    int window = 0;           // 0 dashboard only, 1 driving, 2 session, 3 track and events
    bool dashboard = true;
    int compareLap = 0;       // lap to compare with, 0 = the best lap
    int mapColour = 0;        // track map: 0 speed, 1 pedals, 2 grip use
    int eventTop = 0;         // first event shown in the list
    bool fastForward = false;
    std::string message;      // shown in the session panel (where the run was saved)
};

// Clickable areas of the testing screen.
struct TestHit {
    enum Kind { Timeline = 1, Tile, Lap, Event, Dist, Close };
    Rectangle r;
    int kind = 0;
    int value = 0;         // Tile: window; Lap / Dist: lap number
    float a = 0, b = 0;    // Timeline: time range; Dist: distance range; Event: its time
};

// 2D overlay: timing tower, minimap, focused-car telemetry, help and results.
class Hud {
public:
    void init(const rr::Track& track, const std::string& assetsDir);
    void shutdown();
    void draw(const rr::Race& race, const HudState& st);
    // The testing screen: session panel, car panel, telemetry dashboard or a
    // graph window, and the timeline. Fills `hits` with its clickable areas.
    void drawTest(const rr::Race& race, const HudState& st, const TestView& tv, std::vector<TestHit>& hits);
    // Car index of the timing-tower row under `p` (screen pixels), -1 if none.
    int towerCarAt(const rr::Race& race, const HudState& st, Vector2 p) const;
    // Race-end window tab under `p`, -1 if none.
    int resultsTabAt(Vector2 p) const;
    // The car whose row in the race-end window is at p, or -1; inside sets whether p is on the window.
    int resultsCarAt(Vector2 p, bool* inside) const;
    // The race setup screen; fills `hits` with its clickable areas.
    void drawMenu(const MenuState& m, std::vector<MenuHit>& hits);
    // Screenshots: show every animation at its end state (lights lit, rows in place).
    bool settle = false;

private:
    void text(const char* s, float x, float y, float size, Color c, bool bold = false, bool mono = false);
    float width(const char* s, float size, bool bold = false, bool mono = false);
    void textRight(const char* s, float right, float y, float size, Color c, bool bold = false, bool mono = false);
    void panel(Rectangle r, float alpha = 0.62f);
    // text with a soft drop shadow, for text drawn straight over the 3D view
    void textS(const char* s, float x, float y, float size, Color c, bool bold = false, bool mono = false);
    // the race HUD's building blocks: a slanted band, a tyre compound badge
    void band(Rectangle r, Color c, float slant = 8);
    void compoundBadge(Vector2 c, float r, int compound, float alpha = 1);
    void towerHeader(float x, float w, const char* label, int now, int total, const char* line2, const char* clock,
                     const char* corner);
    // readies 2D drawing and advances the HUD's animations; resets them when a new race starts
    void animate(const rr::Race& race);

    void drawTower(const rr::Race& race, const HudState& st);
    void drawMinimap(const rr::Race& race, const HudState& st);
    void drawCarPanel(const rr::Race& race, const HudState& st, float px = -1, float py = -1);
    // testing screen parts (hud_test.cpp)
    struct Plot {
        Rectangle r;
        float x0, x1, y0, y1;
        Vector2 at(float x, float y) const {
            return {r.x + (x - x0) / (x1 - x0) * r.width, r.y + r.height - (y - y0) / (y1 - y0) * r.height};
        }
    };
    Plot plotFrame(Rectangle box, const char* title, float x0, float x1, float y0, float y1, const char* yFmt, int yTicks,
                   bool xLaps);
    void plotSeries(const Plot& p, const std::vector<Vector2>& pts, Color c, float thick);
    void drawTestSession(const rr::Race& race, const TestView& tv, std::vector<TestHit>& hits);
    void drawTimeline(const TestView& tv, std::vector<TestHit>& hits);
    void drawDashboard(const TestView& tv, Rectangle area, std::vector<TestHit>& hits);
    void drawDrivingWindow(const TestView& tv, Rectangle area, std::vector<TestHit>& hits);
    void drawSessionWindow(const TestView& tv, Rectangle area, std::vector<TestHit>& hits);
    void drawTrackWindow(const TestView& tv, Rectangle area, std::vector<TestHit>& hits);
    void drawTrackMap(const TestView& tv, Rectangle box, int lap, bool big);
    // distance graphs of one lap against the comparison lap
    void distGraph(const TestView& tv, Rectangle box, int which, int lap, int ref, bool big, std::vector<TestHit>& hits);
    // session graphs over all laps
    void sessionGraph(const TestView& tv, Rectangle box, int which, bool big);
    void drawHelp();
    // the setup menu (hud_menu.cpp): widgets, then the pages
    void card(Rectangle r, const char* title, const char* sub = nullptr);
    void button(Rectangle r, const char* label, int style, bool focus, int row, std::vector<MenuHit>& hits,
                int value = -1);  // style 0 primary, 1 secondary, 2 quiet
    void stepper(Rectangle r, const char* value, bool focus, int row, std::vector<MenuHit>& hits);
    void segmented(Rectangle r, const std::vector<const char*>& options, int sel, bool focus, int row,
                   std::vector<MenuHit>& hits);
    void dropdown(Rectangle r, const char* value, bool focus, int row, std::vector<MenuHit>& hits, Color chip = BLANK);
    void field(float x, float y, float w, const char* label, const char* note, bool focus);
    void trackShape(const std::vector<rr::Vec2>& pts, Rectangle box, Color c, float thick);
    void drawTopBar(const MenuState& m, std::vector<MenuHit>& hits, float& top);
    void drawRaceSetup(const MenuState& m, std::vector<MenuHit>& hits, Rectangle area);
    void drawChampSetup(const MenuState& m, std::vector<MenuHit>& hits, Rectangle area);
    void drawTestSetup(const MenuState& m, std::vector<MenuHit>& hits, Rectangle area);
    void drawTrackCard(const MenuState& m, std::vector<MenuHit>& hits, Rectangle r);
    void drawGridCard(const MenuState& m, std::vector<MenuHit>& hits, Rectangle r, bool teamList);
    void drawSeasonPage(const MenuState& m, std::vector<MenuHit>& hits);
    void drawPopup(const MenuState& m, std::vector<MenuHit>& hits);
    void drawColourPicker(const MenuState& m, std::vector<MenuHit>& hits);
    void drawNameBox(const MenuState& m, std::vector<MenuHit>& hits);
    void drawLineupList(const MenuState& m, std::vector<MenuHit>& hits);
    void drawTeamsPage(const MenuState& m, std::vector<MenuHit>& hits);
    void drawGridPage(const MenuState& m, std::vector<MenuHit>& hits);
    void drawTestStatsPage(const MenuState& m, std::vector<MenuHit>& hits);
    void drawRunsPage(const MenuState& m, std::vector<MenuHit>& hits);
    void drawQualiTower(const rr::Race& race, const HudState& st);
public:
    // Qualifying classification between the sessions, with the race start prompt.
    void drawQualiResults(const HudState& st);
private:
    void drawResults(const rr::Race& race, const HudState& st);
    std::vector<Rectangle> resultTabs_;  // the race-end window tabs, as last drawn
    struct RowHit { Rectangle r; int car; };
    std::vector<RowHit> resultRows_;     // the race-end window's driver rows, as last drawn
    Rectangle resultsBox_{};             // the race-end window, as last drawn (empty when hidden)

    // animation state of the race HUD (anim.hpp)
    struct RowAnim {
        anim::Tween y;          // row the car is drawn at, easing to its position
        int lastRow = -1;
        float changed = 99;     // seconds since the car gained or lost a place
        int delta = 0;          // + gained, - lost
        anim::Light focus, hover;
        float pitStart = -1, pitTime = 0, pitShown = 99;  // the stop in the box: start (race time), length, s since
    };
    anim::Keyed<int, RowAnim> towerRows_;
    anim::Keyed<std::string, RowAnim> qualiRows_;
    struct PanelAnim {
        float drs = 0, zone = 0, shift = 0;          // DRS and zone lettering, the rev limit glow
        anim::Light blue, yellow, penalty, pit;      // banners above the panel
        std::string blueMsg, yellowMsg, penaltyMsg, pitMsg;  // kept while a banner fades out
        anim::Crossfade<int> pos, gear, car;
        float rpm = 0;
        bool started = false;
    } panelAnim_;
    int fastestCar_ = -1;                // holder of the race's fastest lap, its time, s since it was set
    float fastestTime_ = 0, fastestAge_ = 99;
    float dt_ = 0, clock_ = 0;
    const rr::Race* animRace_ = nullptr;
    double animTime_ = 0;

    Font regular_{}, bold_{}, mono_{};
    bool ownFonts_ = false;
    std::vector<rr::Vec2> outline_;  // centreline samples for the minimap
    float minX_ = 0, minY_ = 0, maxX_ = 1, maxY_ = 1, trackWidth_ = 14;
};
