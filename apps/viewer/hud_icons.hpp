#pragma once
// Little vector icons drawn with raylib shapes, so the HUD and menus need no icon font or
// textures. Each takes its centre (or top-left where noted), a size in pixels and a colour.
#include "raylib.h"

namespace icon {

void fuelPump(Vector2 c, float s, Color col);
// A battery on its side, top-left at `p`, filled to `frac` with `fill`.
void battery(Vector2 p, float w, float h, float frac, Color fill, Color outline);
void bolt(Vector2 c, float s, Color col);
void stopwatch(Vector2 c, float s, Color col);
// A steering wheel turned by `angle` degrees (+ = clockwise on screen).
void steeringWheel(Vector2 c, float r, float angle, Color col, Color mark);
void wrench(Vector2 c, float s, Color col);
void chequered(Vector2 p, float w, float h);          // a chequered flag panel, top-left at p
void triangle(Vector2 c, float s, bool up, Color col);
void warning(Vector2 c, float s, Color col);            // a warning triangle with a !
// A slanted bar (parallelogram leaning right by `lean` px), top-left at `p`, filled from the
// bottom to `frac`.
void slantBar(Vector2 p, float w, float h, float lean, float frac, Color fill, Color back);
// A ring segment: an arc from `a0` to `a1` degrees (0 = right, clockwise on screen).
void arc(Vector2 c, float r0, float r1, float a0, float a1, Color col);

}  // namespace icon
