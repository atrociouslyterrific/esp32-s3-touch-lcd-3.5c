// brain.h: Bloop's brain.
//
// Bloop learns on the board, live, with tabular Q-learning. It is born knowing
// nothing: every action is worth 0 in every situation. Every couple of seconds
// it looks at its situation (the "state"), picks an action, and later scores
// how that went (the "reward"). The score nudges a table of numbers, Q[state][action],
// that says how good each action is in each situation. Over time it mostly
// picks the best-scoring action, but still tries random things now and then
// ("exploring") so it can discover better habits.
//
// The rewards come from:
//   - its body: eating when hungry, sleeping when tired, playing (good);
//     hard landings, overeating, hunger, exhaustion (bad)
//   - you: petting it and the GOOD button (good), the NO button (bad)
#pragma once
#include <Preferences.h>

enum Action : uint8_t {
  ACT_WANDER, ACT_BALL, ACT_EAT, ACT_SLEEP, ACT_GRIP, ACT_CURL, ACT_COME, ACT_SIT,
  ACT_DANCE, ACT_JUMP, ACT_BEG, ACT_HIDE, ACT_WINDOW, ACT_ROLL,
  NUM_ACTIONS
};
const char *const ACTION_NAMES[NUM_ACTIONS] = {
  "WANDER", "CHASE BALL", "EAT", "SLEEP", "GRIP WALL", "CURL UP", "COME TO YOU", "SIT",
  "DANCE", "JUMP", "BEG FOR FOOD", "HIDE BY BED", "LOOK OUTSIDE", "ROLL AROUND"
};
// Short words for Bloop's thought bubble.
const char *const ACTION_THOUGHTS[NUM_ACTIONS] = {
  "...", "ball!", "food?", "zzz", "hold!", "curl", "you?", "hmm",
  "la la!", "boing!", "pls food", "eep!", "ooh", "wheee"
};

// The situation is built from 6 yes/no-ish features:
//   hungry (2) x tired (2) x room (calm / tilting / floating = 3)
//   x ball near (2) x food out (2) x you're here (2) = 96 situations
enum RoomFeel : uint8_t { ROOM_CALM, ROOM_TILTING, ROOM_FLOATING };
const int NUM_STATES = 2 * 2 * 3 * 2 * 2 * 2;

struct Senses {
  bool hungry, tired;
  RoomFeel room;
  bool ballNear, foodOut, attention;
};

inline int encodeState(const Senses &s) {
  int i = s.hungry;
  i = i * 2 + s.tired;
  i = i * 3 + s.room;
  i = i * 2 + s.ballNear;
  i = i * 2 + s.foodOut;
  i = i * 2 + s.attention;
  return i;
}

inline Senses decodeState(int i) {
  Senses s;
  s.attention = i % 2; i /= 2;
  s.foodOut = i % 2;   i /= 2;
  s.ballNear = i % 2;  i /= 2;
  s.room = (RoomFeel)(i % 3); i /= 3;
  s.tired = i % 2;     i /= 2;
  s.hungry = i % 2;
  return s;
}

class Brain {
public:
  float q[NUM_STATES][NUM_ACTIONS];
  uint32_t decisions = 0;   // how many choices it has learned from
  int lastState = -1;
  Action lastAction = ACT_SIT;
  bool lastWasExplore = false;
  float lastReward = 0;

  // Learning settings
  const float ALPHA = 0.25f;  // how fast new experience overrides old
  const float GAMMA = 0.80f;  // how much it cares about what happens next

  void clear() {
    for (int s = 0; s < NUM_STATES; s++)
      for (int a = 0; a < NUM_ACTIONS; a++) q[s][a] = 0;
    decisions = 0;
    lastState = -1;
  }

  // Exploration: lots of random tries as a baby, fewer as it grows up.
  float epsilon() const {
    float e = 0.40f * expf(-(float)decisions / 500.0f);
    return e < 0.06f ? 0.06f : e;
  }

  Action best(int s) const {
    int b = 0;
    for (int a = 1; a < NUM_ACTIONS; a++)
      if (q[s][a] > q[s][b]) b = a;
    return (Action)b;
  }

  float maxQ(int s) const { return q[s][best(s)]; }

  // Called each time Bloop makes a choice. `reward` is everything that
  // happened since the previous choice. Learns from that, then picks again.
  Action decide(int state, float reward) {
    if (lastState >= 0) {
      float &old = q[lastState][lastAction];
      float target = reward + GAMMA * maxQ(state);
      old += ALPHA * (target - old);
      // Keep values in a sane range so one fluke can't dominate forever.
      if (old > 20) old = 20;
      if (old < -20) old = -20;
      decisions++;
    }
    lastReward = reward;

    Action a;
    if ((float)esp_random() / 4294967295.0f < epsilon()) {
      a = (Action)(esp_random() % NUM_ACTIONS);
      lastWasExplore = true;
    } else {
      // Break ties randomly so a fresh brain doesn't always pick WANDER.
      float m = maxQ(state);
      Action ties[NUM_ACTIONS];
      int n = 0;
      for (int i = 0; i < NUM_ACTIONS; i++)
        if (q[state][i] >= m - 0.001f) ties[n++] = (Action)i;
      a = ties[esp_random() % n];
      lastWasExplore = false;
    }
    lastState = state;
    lastAction = a;
    return a;
  }

  // "What does Bloop usually do when ...": average over every situation that
  // matches `filter`, and return the action it rates best.
  template <typename F>
  Action habit(F filter, float *score = nullptr) const {
    float sum[NUM_ACTIONS] = {0};
    int count = 0;
    for (int s = 0; s < NUM_STATES; s++) {
      if (!filter(decodeState(s))) continue;
      for (int a = 0; a < NUM_ACTIONS; a++) sum[a] += q[s][a];
      count++;
    }
    int b = 0;
    for (int a = 1; a < NUM_ACTIONS; a++)
      if (sum[a] > sum[b]) b = a;
    if (score) *score = count ? sum[b] / count : 0;
    return (Action)b;
  }

  void save(Preferences &p) {
    p.putBytes("q", q, sizeof(q));
    p.putUInt("dec", decisions);
  }

  bool load(Preferences &p) {
    if (p.getBytesLength("q") != sizeof(q)) {
      clear();
      return false;
    }
    p.getBytes("q", q, sizeof(q));
    decisions = p.getUInt("dec", 0);
    return true;
  }
};
