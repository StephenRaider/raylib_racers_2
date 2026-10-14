#pragma once
// Track and air effects, drawn in the 3D pass: the rubber on the racing line (the sim's own map, which
// also gives grip), skid marks, tyre tracks and dust off the grass and gravel, and sparks from contact.
// Viewer only: nothing here touches the simulation.
#include <vector>

#include "raylib.h"
#include "race.hpp"

class Effects {
public:
    void setLevel(int level) { level_ = level; }  // 0 off, 1 marks only, 2 everything
    // True for tracks with hills (RR2's renderer): marks sit on the road's height instead of the flat ground.
    void setUseHeights(bool on) { heights_ = on; }
    void update(const rr::Race& race, float dt);
    void draw(const rr::Race& race, Vector3 camPos) const;

    struct Mark { Vector3 a, b; float width; unsigned char alpha; Color col; };
    struct Particle { Vector3 p, v; float age, life, size; Color col; int kind; };
    // What there is to draw, for renderers that do their own (RR2's PBR renderer).
    int level() const { return level_; }
    const std::vector<Mark>& skidMarks() const { return skids_; }    // on the tarmac
    const std::vector<Mark>& tyreTracks() const { return tracks_; }  // on grass, gravel and dirt
    const std::vector<Particle>& particles() const { return parts_; }
    unsigned marksVersion() const { return marksVersion_; }  // changes when a mark is added

private:
    void reset(const rr::Race& race);
    void addMark(std::vector<Mark>& ring, size_t& head, size_t cap, Vector3 a, Vector3 b, float w, unsigned char alpha, Color col);
    void emit(int kind, Vector3 p, Vector3 v, int count, Color col);

    int level_ = 2;
    const rr::Race* race_ = nullptr;
    double lastTime_ = 0;
    bool heights_ = false;
    unsigned marksVersion_ = 0;
    std::vector<Mark> skids_;      // marks on the tarmac: black
    size_t skidHead_ = 0;
    std::vector<Mark> tracks_;     // tyre tracks on grass, gravel and dirt: dark earth
    size_t trackHead_ = 0;
    std::vector<Particle> parts_;
    struct Wheels { Vector3 p[4]; bool valid = false; bool loose[4] = {false, false, false, false}; int collisions = 0; };
    std::vector<Wheels> last_;
    unsigned rng_ = 12345;
};
