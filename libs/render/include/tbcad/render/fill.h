#pragma once

#include <vector>

#include "tbcad/core/math.h"

namespace tbcad::render {

// Triangulates an even-odd filled area (the rings of a DisplayFill). Rings nested at an even
// depth are outlines, rings at an odd depth are holes of the smallest ring around them. Returns
// triangle corners, three per triangle, in world coordinates. Rings with fewer than three
// vertices or non-finite coordinates are ignored.
std::vector<Vec2> triangulateEvenOdd(const std::vector<std::vector<Vec2>>& rings);

// Sum of the areas of the returned triangles (helper for tests and for sanity checks).
double triangleArea(const std::vector<Vec2>& triangles);

}  // namespace tbcad::render
