// world.h: the room, its physics, and the things in it.
//
// The room is a box. Gravity comes from the board's motion sensor, so whichever
// side of the board points at the ground is the room's floor. Everything in the
// room (Bloop, its ball, food and bed) falls, slides, bounces and lands.
#pragma once
#include <math.h>

// ---- Screen layout (portrait 320 x 480) ----
const int SCREEN_W = 320, SCREEN_H = 480;
const int HUD_H = 46;                 // status bar at the top
const int TOOLBAR_Y = 426;            // buttons at the bottom
const float ROOM_L = 8, ROOM_R = SCREEN_W - 8;
const float ROOM_T = HUD_H + 8, ROOM_B = TOOLBAR_Y - 8;

// ---- Physics tuning ----
const float PX_PER_G = 950.0f;        // 1 g of tilt = this many px/s^2
const float FLOAT_GRAVITY = 0.12f;    // gravity scale when the board lies flat
const float AIR_DRAG = 0.15f;         // per second
const float FLOAT_DRAG = 0.6f;        // thicker "air" in zero-g so it drifts
const float HURT_SPEED = 430.0f;      // landing faster than this hurts
const float STEEP = 0.55f;            // slope (sideways share of gravity) Bloop can't walk up

struct Vec2 {
  float x, y;
};
inline float clampf_w(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline Vec2 v2(float x, float y) { Vec2 v = {x, y}; return v; }
inline float vlen(Vec2 v) { return sqrtf(v.x * v.x + v.y * v.y); }

enum BodyKind : uint8_t { BODY_PET, BODY_BALL, BODY_FOOD, BODY_BED };

// Bits for which walls a body is touching.
enum : uint8_t { WALL_L = 1, WALL_R = 2, WALL_T = 4, WALL_B = 8 };

struct Body {
  BodyKind kind;
  bool active;
  float x, y, vx, vy, r;
  float bounce;      // 0 = thud, 1 = superball
  float friction;    // how fast it stops sliding along a wall
  uint8_t touching;  // WALL_* bits this frame
  float impact;      // hardest wall hit this frame (px/s)
  float airTime;     // seconds without touching anything
  float stick;       // static friction: resists sliding on gentle slopes
  bool isBox;        // the bed is a box, everything else is round
  float hw, hh;      // box half-width / half-height
};

inline void bodyInit(Body &b, BodyKind k, float x, float y, float r, float bounce, float friction) {
  b.kind = k; b.active = true;
  b.x = x; b.y = y; b.vx = 0; b.vy = 0; b.r = r;
  b.bounce = bounce; b.friction = friction;
  b.touching = 0; b.impact = 0; b.airTime = 0;
  b.stick = 0.12f; b.isBox = false; b.hw = r; b.hh = r;
}

// Inward-pointing normal of each wall.
inline Vec2 wallNormal(uint8_t wall) {
  switch (wall) {
    case WALL_L: return v2(1, 0);
    case WALL_R: return v2(-1, 0);
    case WALL_T: return v2(0, 1);
    default:     return v2(0, -1);
  }
}

// Which wall is "the floor" right now: the one gravity pushes hardest into.
inline uint8_t floorWall(Vec2 g) {
  if (fabsf(g.x) > fabsf(g.y)) return g.x > 0 ? WALL_R : WALL_L;
  return g.y > 0 ? WALL_B : WALL_T;
}

// Physics runs in small fixed steps so it behaves the same at any frame rate.
const float PHYS_DT = 1.0f / 240.0f;

// Move one body for dt seconds under gravity g (px/s^2) and bounce it off walls.
inline void stepBody(Body &b, Vec2 g, float dt, bool floating) {
  float ex = b.isBox ? b.hw : b.r, ey = b.isBox ? b.hh : b.r;

  // Static friction: something resting on a wall stays put on a gentle slope
  // (and ignores tiny sensor wobble) instead of creeping and jittering.
  Vec2 ga = g;
  if (!floating && b.touching) {
    if (b.touching & (WALL_B | WALL_T)) {
      float gn = (b.touching & WALL_B) ? g.y : -g.y;
      if (gn > 0 && fabsf(g.x) < b.stick * gn && fabsf(b.vx) < 20) { ga.x = 0; b.vx *= 0.6f; }
    }
    if (b.touching & (WALL_L | WALL_R)) {
      float gn = (b.touching & WALL_R) ? g.x : -g.x;
      if (gn > 0 && fabsf(g.y) < b.stick * gn && fabsf(b.vy) < 20) { ga.y = 0; b.vy *= 0.6f; }
    }
  }

  b.vx += ga.x * dt;
  b.vy += ga.y * dt;
  float drag = 1.0f - (floating ? FLOAT_DRAG : AIR_DRAG) * dt;
  b.vx *= drag;
  b.vy *= drag;
  b.x += b.vx * dt;
  b.y += b.vy * dt;

  b.touching = 0;
  float slack = 1.0f;  // treat "resting against" as touching
  if (b.x - ex <= ROOM_L + slack) {
    if (b.x - ex < ROOM_L) b.x = ROOM_L + ex;
    if (b.vx < 0) { b.impact = fmaxf(b.impact, -b.vx); b.vx = -b.vx * b.bounce; }
    b.touching |= WALL_L;
  }
  if (b.x + ex >= ROOM_R - slack) {
    if (b.x + ex > ROOM_R) b.x = ROOM_R - ex;
    if (b.vx > 0) { b.impact = fmaxf(b.impact, b.vx); b.vx = -b.vx * b.bounce; }
    b.touching |= WALL_R;
  }
  if (b.y - ey <= ROOM_T + slack) {
    if (b.y - ey < ROOM_T) b.y = ROOM_T + ey;
    if (b.vy < 0) { b.impact = fmaxf(b.impact, -b.vy); b.vy = -b.vy * b.bounce; }
    b.touching |= WALL_T;
  }
  if (b.y + ey >= ROOM_B - slack) {
    if (b.y + ey > ROOM_B) b.y = ROOM_B - ey;
    if (b.vy > 0) { b.impact = fmaxf(b.impact, b.vy); b.vy = -b.vy * b.bounce; }
    b.touching |= WALL_B;
  }

  // Sliding friction along whatever it's resting on.
  float f = 1.0f - b.friction * dt;
  if (f < 0) f = 0;
  if (b.touching & (WALL_T | WALL_B)) b.vx *= f;
  if (b.touching & (WALL_L | WALL_R)) b.vy *= f;

  // Tiny bounces just jitter; settle them.
  if ((b.touching & (WALL_T | WALL_B)) && fabsf(b.vy) < 30) b.vy = 0;
  if ((b.touching & (WALL_L | WALL_R)) && fabsf(b.vx) < 30) b.vx = 0;

  b.airTime = b.touching ? 0 : b.airTime + dt;
}

// Push two overlapping round bodies apart; returns the closing speed if they hit.
inline float collide(Body &a, Body &b, float massA, float massB) {
  float dx = b.x - a.x, dy = b.y - a.y;
  float d = sqrtf(dx * dx + dy * dy);
  float minD = a.r + b.r;
  if (d >= minD || d < 0.001f) return 0;
  float nx = dx / d, ny = dy / d;
  float overlap = minD - d;
  float total = massA + massB;
  a.x -= nx * overlap * (massB / total);
  a.y -= ny * overlap * (massB / total);
  b.x += nx * overlap * (massA / total);
  b.y += ny * overlap * (massA / total);
  float rel = (b.vx - a.vx) * nx + (b.vy - a.vy) * ny;
  if (rel >= 0) return 0;  // already separating
  float e = fminf(a.bounce, b.bounce) + 0.2f;
  float j = -(1 + e) * rel / (1 / massA + 1 / massB);
  a.vx -= j * nx / massA; a.vy -= j * ny / massA;
  b.vx += j * nx / massB; b.vy += j * ny / massB;
  return -rel;
}

// Push a round body out of a box (the bed). Returns the closing speed if they hit.
inline float collideBox(Body &c, Body &box, float massC, float massB) {
  float px = clampf_w(c.x, box.x - box.hw, box.x + box.hw);
  float py = clampf_w(c.y, box.y - box.hh, box.y + box.hh);
  float dx = c.x - px, dy = c.y - py;
  float d2 = dx * dx + dy * dy;
  float nx, ny, overlap;
  if (d2 > 0.0001f) {
    float d = sqrtf(d2);
    if (d >= c.r) return 0;
    nx = dx / d; ny = dy / d;
    overlap = c.r - d;
  } else {
    // Centre is inside the box: push out the shortest way.
    float ox = box.hw + c.r - fabsf(c.x - box.x), oy = box.hh + c.r - fabsf(c.y - box.y);
    if (ox < oy) { nx = c.x > box.x ? 1 : -1; ny = 0; overlap = ox; }
    else { nx = 0; ny = c.y > box.y ? 1 : -1; overlap = oy; }
  }
  float total = massC + massB;
  c.x += nx * overlap * (massB / total);
  c.y += ny * overlap * (massB / total);
  box.x -= nx * overlap * (massC / total);
  box.y -= ny * overlap * (massC / total);
  float rel = (c.vx - box.vx) * nx + (c.vy - box.vy) * ny;
  if (rel >= 0) return 0;
  float e = fminf(c.bounce, box.bounce) + 0.1f;
  float j = -(1 + e) * rel / (1 / massC + 1 / massB);
  c.vx += j * nx / massC; c.vy += j * ny / massC;
  box.vx -= j * nx / massB; box.vy -= j * ny / massB;
  return -rel;
}

// ---- Particles (hearts, stars, crumbs, dust) ----
enum ParticleKind : uint8_t { PT_HEART, PT_STAR, PT_CRUMB, PT_DUST, PT_ZZZ, PT_NOTE };

struct Particle {
  bool active;
  ParticleKind kind;
  float x, y, vx, vy, life;
  uint16_t color;
};

const int MAX_PARTICLES = 40;
const int MAX_FOOD = 6;
