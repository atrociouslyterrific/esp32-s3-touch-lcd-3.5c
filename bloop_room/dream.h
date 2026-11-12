// dream.h: Bloop learns in its dreams.
//
// Learning only from real life is slow: Bloop makes a choice every couple of
// seconds, so teaching it by hand takes ages. So while it sleeps, the board runs
// a tiny pretend version of Bloop's life in its head: it gets hungry and tired,
// food turns up, the room tips, you come and go. Bloop tries things there and
// learns from what would happen, about a thousand times a second, using the same
// brain table it uses when awake. One nap teaches it sensible habits (eat when
// hungry, grip or curl up when the room tips, sleep when tired...). Real life,
// Pip, and your GOOD / NO buttons keep fine-tuning it afterwards.
//
// The rewards here are rough copies of the real ones in bloop_room.ino, so a
// dream lesson and a real lesson mean the same thing to the brain.
#pragma once
#include "brain.h"

struct DreamWorld {
  Senses s;
  uint32_t seed;

  float rnd() {   // quick pseudo-random number in [0, 1)
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) / 16777216.0f;
  }
  bool chance(float p) { return rnd() < p; }

  void start(uint32_t seedIn) {
    seed = seedIn | 1;
    s = decodeState(seed % NUM_STATES);
  }

  // Bloop does `a` in the dream. Returns the reward; the world moves on.
  float act(Action a) {
    float r = 0;
    bool calm = s.room == ROOM_CALM;

    switch (a) {
      case ACT_EAT:
        if (s.foodOut) {
          r += s.hungry ? 1.6f : -0.8f;          // yum / tummy ache
          s.hungry = false;
          if (chance(0.6f)) s.foodOut = false;    // that was the last snack
        } else {
          r -= 0.1f;                              // nothing to eat
        }
        break;
      case ACT_BEG:
        if (s.hungry && !s.foodOut) {
          if (chance(0.5f)) { s.foodOut = true; r += 1.2f; }   // you fed it!
        } else {
          r -= 0.2f;
        }
        break;
      case ACT_SLEEP:
        if (s.tired) { r += 1.0f; if (chance(0.5f)) s.tired = false; }
        else r -= 0.3f;
        break;
      case ACT_BALL:
        if (s.ballNear && !s.tired) r += 0.6f;
        else if (s.tired) r -= 0.2f;
        break;
      case ACT_DANCE: r += s.tired ? -0.2f : (s.attention ? 0.5f : 0.1f); break;
      case ACT_JUMP:  r += s.tired ? -0.2f : 0.25f; break;
      case ACT_ROLL:  r += s.tired ? -0.1f : 0.2f; break;
      case ACT_WINDOW: r += (!s.hungry && !s.tired) ? 0.25f : 0.0f; break;
      case ACT_COME:  r += s.attention ? 0.6f : -0.05f; break;   // pats!
      case ACT_SIT:   r += s.tired ? 0.1f : -0.05f; break;
      case ACT_WANDER: r += 0.05f; break;
      default: break;   // grip / curl / hide: see below
    }

    // The room tipping: holding on, curling up or hiding keeps Bloop safe.
    bool safe = a == ACT_GRIP || a == ACT_CURL || a == ACT_HIDE;
    if (s.room == ROOM_TILTING) {
      if (safe) r += 0.4f;
      else {
        bool silly = a == ACT_DANCE || a == ACT_JUMP || a == ACT_WINDOW || a == ACT_SLEEP;
        if (chance(silly ? 0.9f : 0.6f)) r -= 2.5f;   // a hard landing
      }
    } else if (safe && calm) {
      r -= 0.05f;   // hiding from nothing is a bit dull
    }

    // Needs nag a little every turn.
    if (s.hungry) r -= 0.3f;
    if (s.tired) r -= 0.2f;

    // The world moves on.
    if (!s.hungry && chance(0.15f)) s.hungry = true;
    if (!s.tired && chance(0.10f)) s.tired = true;
    if (s.room == ROOM_CALM) {
      if (chance(0.12f)) s.room = ROOM_TILTING;
      else if (chance(0.04f)) s.room = ROOM_FLOATING;
    } else if (s.room == ROOM_TILTING) {
      if (chance(0.4f)) s.room = ROOM_CALM;
    } else if (chance(0.3f)) {
      s.room = ROOM_CALM;
    }
    if (!s.foodOut && chance(0.08f)) s.foodOut = true;
    if (chance(0.2f)) s.attention = !s.attention;
    if (chance(0.25f)) s.ballNear = !s.ballNear;
    return r;
  }
};

DreamWorld dreamWorld;
bool dreamStarted = false;

// Dream `steps` lessons. Returns how many it did.
uint32_t dream(Brain &brain, uint32_t steps) {
  if (!dreamStarted) { dreamWorld.start(esp_random()); dreamStarted = true; }
  for (uint32_t i = 0; i < steps; i++) {
    int st = encodeState(dreamWorld.s);
    Action a;
    if (dreamWorld.chance(0.3f)) a = (Action)(dreamWorld.seed % NUM_ACTIONS);   // try things
    else a = brain.best(st);
    float r = dreamWorld.act(a);
    brain.learn(st, a, r, encodeState(dreamWorld.s), 0.1f);
  }
  brain.dreams += steps;
  return steps;
}
