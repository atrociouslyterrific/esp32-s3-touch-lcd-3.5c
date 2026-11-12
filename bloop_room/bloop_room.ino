// BLOOP'S ROOM: a pet that lives in a room you can tip over.
// For the Waveshare ESP32-S3-Touch-LCD-3.5-C.
//
// - The room follows real gravity from the board's motion sensor. Tip the board
//   and the floor changes: Bloop, its ball, food and bed slide and fall. Lay the
//   board flat and everything floats. Shake it and things fly.
// - Bloop learns by itself, on the board (see brain.h). It starts as a clueless
//   baby and slowly works out what to do when it's hungry, tired, bored, or when
//   the room starts tipping (grip the wall? curl up into a ball?).
// - You help train it: pet it (hold a finger on it) or tap GOOD when it does
//   something you like, tap NO when it doesn't. Drag Bloop or the ball to throw them.
// - Its brain and stats are saved, so it remembers after being unplugged.

#include <Arduino_GFX_Library.h>
#include "board.h"
#include <TouchDrvFT6X36.hpp>
#include <SensorQMI8658.hpp>
#include <Preferences.h>
#include "world.h"
#include "brain.h"
#include "FreeSans9pt7b.h"
#include "FreeSansBold9pt7b.h"
#include "FreeSansBold12pt7b.h"

// Bloop's voice: a tiny language model trained on GitHub (training/train_voice.py).
// GitHub generates voice_weights.h before compiling. Without it Bloop just
// shows short thoughts instead of talking.
#if __has_include("voice_weights.h")
#include "voice.h"
#define BLOOP_HAS_VOICE 1
#else
#define BLOOP_HAS_VOICE 0
#endif

// Board pins are in board.h
#define LCD_SPI_HZ 80000000     // Waveshare's own firmware runs the screen at 80 MHz

// ---- Screens ----
enum Screen : uint8_t { SCR_ROOM, SCR_BRAIN, SCR_CAL, SCR_MENU };

// ---- Bloop ----
struct Pet {
  Body body;
  Action action;
  float actionTime;      // seconds since this action started
  float hunger, energy, joy, health, stamina;   // 0..100
  float hurtT, dizzyT, eatT, sadT, kickCool, hopCool, blinkT, bubbleT;
  bool asleep, held, curled, gripping, slipping, hiding;
  float danceT;          // dance animation clock
  float angle;           // drawing angle of "down" (radians)
  float walkPhase;
  Vec2 target, look;
  uint32_t falls, treats, kicks;
  float age;             // seconds alive
};

// ---- Hardware objects ----
TCA9554 expander(0x20);
XPowersPMU pmu;
BoardStatus boardSt;
bool screenOk = false, backlightOn = false;
TouchDrvFT6X36 touch;
SensorQMI8658 imu;
Preferences prefs;
Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, -1, SPI_SCLK, SPI_MOSI, SPI_MISO);
Arduino_ST7796 *lcd = new Arduino_ST7796(bus, -1, 0, true, SCREEN_W, SCREEN_H);
Arduino_Canvas *cv = new Arduino_Canvas(SCREEN_W, SCREEN_H, lcd, 0, 0, 0);
bool imuOk = false, touchOk = false;

// ---- World state ----
Pet pet;
Body ball, bed;
Body food[MAX_FOOD];
Particle parts[MAX_PARTICLES];
Brain brain;
Screen screen = SCR_ROOM;

// Options (MORE menu)
bool lightsOff = false;
enum Toy : uint8_t { TOY_BALL, TOY_BLOCK, TOY_BEACH, NUM_TOYS };
const char *const TOY_NAMES[NUM_TOYS] = {"BALL", "BLOCK", "BEACH BALL"};
uint8_t toy = TOY_BALL;
float toySpin = 0;      // for drawing the block turning
float bedAngle = PI / 2;
bool toyOut = true;     // false when the toy is put away

// What Bloop is saying (its language model writes here; see voice.h)
char sayText[64] = "";
float sayT = 0;
uint32_t sayCount = 0;      // goes up each time Bloop says something (the radio watches it)
uint8_t sayEventNow = 0;    // what the latest line was about
uint8_t pipTopic = 6;       // what Pip last talked about (PipTopic), for Bloop's reply
int toolFlash = -1;     // toolbar button to highlight after a tap
uint32_t toolFlashMs = 0;

// Gravity
float rawA[3] = {0, 0, -1};        // filtered chip readings (g)
uint8_t axisY = 1;                  // which chip axis points along screen up/down
int8_t signX = -1, signY = -1;
Vec2 gScreen = {0, 1};              // gravity in screen directions (g)
bool floating = false;
float tiltRate = 0;                 // how fast gravity is changing (g/s)
float shakeT = 0;                   // seconds since last shake
float lastShakeMag = 0;

// Learning bookkeeping
float rewardAcc = 0;                // reward collected since the last choice
float sinceDecide = 0;
bool decideNow = true;
int curState = 0;

// Touch
bool fingerDown = false;
float fx = 0, fy = 0, fStartX = 0, fStartY = 0, fvx = 0, fvy = 0;
uint32_t fStartMs = 0, lastTouchMs = 0;
enum Grab : uint8_t { GRAB_NONE, GRAB_PET, GRAB_BALL, GRAB_BED, GRAB_FOOD };
Grab grabCand = GRAB_NONE;
int grabFood = -1;          // which snack, when grabCand == GRAB_FOOD
bool grabbing = false;
bool petting = false;
Vec2 callPoint = {SCREEN_W / 2.0f, (ROOM_T + ROOM_B) / 2};

// Calibration / brain screen
int calStep = 0;
const char *calMsg = "";
bool confirmReset = false;
float saveTimer = 0;
float physAcc = 0;
float brainSendTimer = 0;
char cmdBuf[24];
int cmdLen = 0;
uint32_t lastFrameMs = 0;

// ============================================================ helpers

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline float frand(float a, float b) {
  return a + (b - a) * ((float)esp_random() / 4294967295.0f);
}
static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void spawnParticle(ParticleKind k, float x, float y, float vx, float vy, float life, uint16_t color) {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (parts[i].active) continue;
    parts[i].active = true;
    parts[i].kind = k;
    parts[i].x = x; parts[i].y = y;
    parts[i].vx = vx; parts[i].vy = vy;
    parts[i].life = life;
    parts[i].color = color;
    return;
  }
}

Vec2 downDir() {
  return v2(cosf(pet.angle), sinf(pet.angle));
}

// Tangent along the current floor (for walking).
Vec2 floorTangent(uint8_t fw) {
  return (fw == WALL_L || fw == WALL_R) ? v2(0, 1) : v2(1, 0);
}

int activeFood() {
  int n = 0;
  for (int i = 0; i < MAX_FOOD; i++) n += food[i].active;
  return n;
}

int nearestFood() {
  int best = -1;
  float bd = 1e9;
  for (int i = 0; i < MAX_FOOD; i++) {
    if (!food[i].active) continue;
    float d = vlen(v2(food[i].x - pet.body.x, food[i].y - pet.body.y));
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

// A spot on the "ceiling" (the wall opposite the floor), for dropping things in.
Vec2 ceilingSpot() {
  uint8_t fw = floorWall(gScreen);
  switch (fw) {
    case WALL_B: return v2(frand(ROOM_L + 30, ROOM_R - 30), ROOM_T + 20);
    case WALL_T: return v2(frand(ROOM_L + 30, ROOM_R - 30), ROOM_B - 20);
    case WALL_L: return v2(ROOM_R - 20, frand(ROOM_T + 30, ROOM_B - 30));
    default:     return v2(ROOM_L + 20, frand(ROOM_T + 30, ROOM_B - 30));
  }
}

// ============================================================ saving

void saveAll() {
  brain.save(prefs);
  prefs.putFloat("hun", pet.hunger);
  prefs.putFloat("ene", pet.energy);
  prefs.putFloat("joy", pet.joy);
  prefs.putFloat("hp", pet.health);
  prefs.putFloat("age", pet.age);
  prefs.putUInt("falls", pet.falls);
  prefs.putUInt("treats", pet.treats);
  prefs.putUInt("kicks", pet.kicks);
  prefs.putBool("dark", lightsOff);
  prefs.putUChar("toy", toy);
}

void loadAll() {
  brain.load(prefs);
  pet.hunger = prefs.getFloat("hun", 30);
  pet.energy = prefs.getFloat("ene", 80);
  pet.joy = prefs.getFloat("joy", 60);
  pet.health = prefs.getFloat("hp", 100);
  pet.age = prefs.getFloat("age", 0);
  pet.falls = prefs.getUInt("falls", 0);
  pet.treats = prefs.getUInt("treats", 0);
  pet.kicks = prefs.getUInt("kicks", 0);
  lightsOff = prefs.getBool("dark", false);
  toy = prefs.getUChar("toy", TOY_BALL) % NUM_TOYS;
  toyOut = prefs.getBool("toyOut", true);
  axisY = prefs.getUChar("axY", 1);
  signX = prefs.getChar("sgX", -1);
  signY = prefs.getChar("sgY", -1);
}

// ============================================================ sensors

void readImu(float dt) {
  if (!imuOk) {
    gScreen = v2(0, 1);
    floating = false;
    return;
  }
  float ax, ay, az;
  if (!imu.getAccelerometer(ax, ay, az)) return;

  // Shake: total force far from 1 g.
  float mag = sqrtf(ax * ax + ay * ay + az * az);
  lastShakeMag = fabsf(mag - 1.0f);
  shakeT += dt;
  if (lastShakeMag > 0.9f && shakeT > 0.6f) {
    shakeT = 0;
    onShake();
  }

  float k = clampf(dt * 12.0f, 0, 1);  // smoothing
  rawA[0] += (ax - rawA[0]) * k;
  rawA[1] += (ay - rawA[1]) * k;
  rawA[2] += (az - rawA[2]) * k;

  uint8_t axisX = 1 - axisY;
  Vec2 g = v2(signX * rawA[axisX], signY * rawA[axisY]);
  float change = vlen(v2(g.x - gScreen.x, g.y - gScreen.y)) / fmaxf(dt, 0.001f);
  tiltRate += (change - tiltRate) * clampf(dt * 4, 0, 1);
  gScreen = g;

  float inPlane = vlen(g);
  if (floating && inPlane > 0.42f) floating = false;
  else if (!floating && inPlane < 0.30f) floating = true;
}

// ============================================================ Bloop's senses & rewards

Senses sense() {
  Senses s;
  s.hungry = pet.hunger > 55;
  s.tired = pet.energy < 35;
  if (floating) s.room = ROOM_FLOATING;
  else {
    Vec2 g = gScreen;
    float m = fmaxf(vlen(g), 0.01f);
    float side = fminf(fabsf(g.x), fabsf(g.y)) / m;   // how far off straight down
    bool unsteady = tiltRate > 0.7f || shakeT < 1.5f || pet.body.airTime > 0.2f || side > 0.42f;
    s.room = unsteady ? ROOM_TILTING : ROOM_CALM;
  }
  s.ballNear = ball.active && vlen(v2(ball.x - pet.body.x, ball.y - pet.body.y)) < 110;
  s.foodOut = activeFood() > 0;
  s.attention = millis() - lastTouchMs < 6000;
  return s;
}

void startAction(Action a) {
  if (a != pet.action) {
    pet.actionTime = 0;
    if (!BLOOP_HAS_VOICE) pet.bubbleT = 2.2f;
    const int8_t talk[NUM_ACTIONS] = {-1, SAY_TOY, -1, SAY_SLEEP, SAY_GRIP, -1, -1, -1,
                                      SAY_DANCE, SAY_JUMP, SAY_BEG, SAY_HIDE, SAY_WINDOW, SAY_ROLL};
    if (talk[a] >= 0 && (a == ACT_GRIP || frand(0, 1) < 0.6f)) {
      pet.action = a;   // so say() sees the new action
      say(talk[a], a == ACT_GRIP);
    }
  }
  pet.action = a;
  if (a != ACT_SLEEP) pet.asleep = false;
  if (a == ACT_WANDER)
    pet.target = v2(frand(ROOM_L + 30, ROOM_R - 30), frand(ROOM_T + 30, ROOM_B - 30));
}

// ---- Training data over USB, for the dashboard website ----
// One JSON object per line. "d" = a decision, "q" = the whole brain table,
// "ev" = something happened. The website reads these with Web Serial.

void sendEvent(const char *name, float value) {
  if (!Serial) return;
  Serial.printf("{\"t\":\"ev\",\"e\":\"%s\",\"v\":%.2f,\"ms\":%lu}\n", name, value, (unsigned long)millis());
}

void sendDecision(float reward) {
  if (!Serial) return;
  Senses s = decodeState(curState);
  Serial.printf("{\"t\":\"d\",\"n\":%lu,\"s\":%d,\"a\":%d,\"r\":%.3f,\"x\":%d,\"eps\":%.3f,"
                "\"hun\":%.1f,\"ene\":%.1f,\"joy\":%.1f,\"hp\":%.1f,\"room\":%d,"
                "\"falls\":%lu,\"snacks\":%lu,\"kicks\":%lu,\"age\":%.0f,\"ms\":%lu}\n",
                (unsigned long)brain.decisions, curState, (int)pet.action, reward, brain.lastWasExplore ? 1 : 0,
                brain.epsilon(), pet.hunger, pet.energy, pet.joy, pet.health, (int)s.room,
                (unsigned long)pet.falls, (unsigned long)pet.treats, (unsigned long)pet.kicks, pet.age,
                (unsigned long)millis());
}

void sendBrain() {
  if (!Serial) return;
  Serial.print("{\"t\":\"q\",\"q\":[");
  for (int s = 0; s < NUM_STATES; s++)
    for (int a = 0; a < NUM_ACTIONS; a++) {
      if (s || a) Serial.print(',');
      Serial.print(brain.q[s][a], 2);
    }
  Serial.println("]}");
}

// ---- Talking ----
uint32_t lastSayMs = 0;

// who: "bloop" or "pip". Pip's lines come over the radio, so they're cleaned of quotes first.
void sendSay(const char *who, const char *ev, const char *text, unsigned long us) {
  if (!Serial) return;
  char clean[64];
  int j = 0;
  for (int i = 0; text[i] && j < 63; i++)
    if (text[i] != '"' && text[i] != '\\' && (uint8_t)text[i] >= 32) clean[j++] = text[i];
  clean[j] = 0;
  Serial.printf("{\"t\":\"say\",\"who\":\"%s\",\"ev\":\"%s\",\"text\":\"%s\",\"us\":%lu}\n", who, ev, clean, us);
}

#if BLOOP_HAS_VOICE
BloopVoice voiceModel;   // the model's working memory (~33 KB)
const char *const SAY_NAMES[NUM_SAY] = {
  "idle", "fall", "shake", "eat", "overeat", "kick", "pet", "good", "no", "wake", "sleep",
  "dance", "jump", "hide", "window", "beg", "roll", "grip", "float", "toy",
  "visit", "pip", "chat", "bye"
};
const int SAY_TOKENS[NUM_SAY] = {
  BV_TOK_EVENT_IDLE, BV_TOK_EVENT_FALL, BV_TOK_EVENT_SHAKE, BV_TOK_EVENT_EAT, BV_TOK_EVENT_OVEREAT,
  BV_TOK_EVENT_KICK, BV_TOK_EVENT_PET, BV_TOK_EVENT_GOOD, BV_TOK_EVENT_NO, BV_TOK_EVENT_WAKE,
  BV_TOK_EVENT_SLEEP, BV_TOK_EVENT_DANCE, BV_TOK_EVENT_JUMP, BV_TOK_EVENT_HIDE, BV_TOK_EVENT_WINDOW,
  BV_TOK_EVENT_BEG, BV_TOK_EVENT_ROLL, BV_TOK_EVENT_GRIP, BV_TOK_EVENT_FLOAT, BV_TOK_EVENT_TOY,
  BV_TOK_EVENT_VISIT, BV_TOK_EVENT_PIP, BV_TOK_EVENT_CHAT, BV_TOK_EVENT_BYE
};
const int PIP_TOPIC_TOKENS[NUM_PIP_TOPICS] = {
  BV_TOK_PIP_FOOD, BV_TOK_PIP_SLEEP, BV_TOK_PIP_PLAY, BV_TOK_PIP_WEATHER, BV_TOK_PIP_LOVE,
  BV_TOK_PIP_QUESTION, BV_TOK_PIP_ANY
};
float voiceRand() { return (float)(esp_random() >> 8) / 16777216.0f; }
#endif

// Bloop says something about `ev`. Important things interrupt; chatter waits its turn.
void say(uint8_t ev, bool important) {
#if BLOOP_HAS_VOICE
  if (pet.asleep && ev != SAY_SLEEP) return;
  uint32_t now = millis();
  if (now - lastSayMs < (important ? 1200u : 7000u)) return;
  lastSayMs = now;
  Senses s = sense();
  int prompt[12], n = 0;
  prompt[n++] = s.hungry ? BV_TOK_HUNGER_HUNGRY : BV_TOK_HUNGER_FULL;
  prompt[n++] = s.tired ? BV_TOK_ENERGY_TIRED : BV_TOK_ENERGY_AWAKE;
  prompt[n++] = pet.joy > 65 ? BV_TOK_MOOD_HAPPY : (pet.joy < 30 ? BV_TOK_MOOD_SAD : BV_TOK_MOOD_OK);
  prompt[n++] = s.room == ROOM_CALM ? BV_TOK_ROOM_CALM : (s.room == ROOM_TILTING ? BV_TOK_ROOM_TILT : BV_TOK_ROOM_FLOAT);
  if (lightsOff) prompt[n++] = BV_TOK_DARK;
  if (s.attention) prompt[n++] = BV_TOK_YOU;
  if (ev == SAY_PIP) prompt[n++] = PIP_TOPIC_TOKENS[pipTopic % NUM_PIP_TOPICS];
  prompt[n++] = SAY_TOKENS[ev];
  prompt[n++] = BV_TOK_SAY;
  int words[20];
  uint32_t t0 = micros();
  int nw = bv_generate(&voiceModel, prompt, n, words, 16, 0.8f, 12, voiceRand);
  unsigned long us = micros() - t0;
  bv_text(words, nw, sayText, sizeof(sayText));
  sayT = 3.5f;
  sayCount++;
  sayEventNow = ev;
  sendSay("bloop", SAY_NAMES[ev], sayText, us);
#else
  (void)ev; (void)important;
#endif
}

void think() {
  curState = encodeState(sense());
  float r = rewardAcc;
  Action a = brain.decide(curState, r);
  rewardAcc = 0;
  sinceDecide = 0;
  decideNow = false;
  startAction(a);
  sendDecision(r);
}

void hurt(float dmg) {
  if (pet.curled) dmg *= 0.2f;   // curling up protects it
  if (dmg <= 0.3f) return;
  pet.health = clampf(pet.health - dmg, 0, 100);
  rewardAcc -= dmg * 0.3f;
  pet.joy = clampf(pet.joy - dmg, 0, 100);
  pet.asleep = false;
  if (dmg > 2) pet.hurtT = 1.2f;
  if (dmg > 6) pet.dizzyT = 3.0f;
  pet.falls++;
  decideNow = true;
  sendEvent("fall", dmg);
  say(SAY_FALL, true);
}

void visitorShake();   // in visitor.h (included below)

void onShake() {
  Body *all[3] = {&ball, &bed, &pet.body};
  for (int i = 0; i < 3; i++) {
    if (i == 2 && (pet.held || (pet.gripping && pet.stamina > 0))) continue;
    all[i]->vx += frand(-420, 420);
    all[i]->vy += frand(-420, 420);
  }
  for (int i = 0; i < MAX_FOOD; i++)
    if (food[i].active) { food[i].vx += frand(-300, 300); food[i].vy += frand(-300, 300); }
  if (pet.hiding) {
    rewardAcc += 0.8f;   // phew, safe by the bed
  } else if (!pet.curled && !(pet.gripping && pet.stamina > 0)) {
    pet.dizzyT = 2.5f;
    rewardAcc -= 1.0f;
    pet.asleep = false;
  }
  decideNow = true;
  sendEvent("shake", lastShakeMag);
  say(SAY_SHAKE, true);
  visitorShake();
}

#include "visitor.h"   // Pip, the pet from the other board, comes over when the boards are together

// ============================================================ the world

void dropFood() {
  for (int i = 0; i < MAX_FOOD; i++) {
    if (food[i].active) continue;
    Vec2 p = ceilingSpot();
    bodyInit(food[i], BODY_FOOD, p.x, p.y, 7, 0.3f, 5);
    food[i].stick = 0.25f;
    if (pet.action == ACT_BEG) {   // begging worked!
      rewardAcc += pet.hunger > 40 ? 1.2f : 0.3f;
      pet.joy = clampf(pet.joy + 5, 0, 100);
    }
    decideNow = true;
    return;
  }
}

void initToy(float x, float y) {
  switch (toy) {
    case TOY_BLOCK: bodyInit(ball, BODY_BALL, x, y, 12, 0.15f, 5.0f); ball.stick = 0.4f; break;
    case TOY_BEACH: bodyInit(ball, BODY_BALL, x, y, 20, 0.65f, 0.4f); ball.stick = 0.03f; break;
    default:        bodyInit(ball, BODY_BALL, x, y, 13, 0.8f, 0.8f); ball.stick = 0.05f; break;
  }
}

void tossBall() {
  Vec2 p = ceilingSpot();
  toyOut = true;
  prefs.putBool("toyOut", true);
  initToy(p.x, p.y);
  ball.vx = frand(-200, 200);
  ball.vy = frand(-200, 200);
}

// The window on the back wall (drawn in drawRoom).
const int WIN_X = 105, WIN_Y = 120, WIN_W = 110, WIN_H = 86;

bool nearBed(float extra) {
  return vlen(v2(bed.x - pet.body.x, bed.y - pet.body.y)) < pet.body.r + bed.r + extra;
}

void petBehave(float dt) {
  Body &b = pet.body;
  uint8_t fw = floorWall(gScreen);
  bool onFloor = (b.touching & fw) && !floating;
  Vec2 t = floorTangent(fw);
  float gm = fmaxf(vlen(gScreen), 0.01f);
  float slope = (gScreen.x * t.x + gScreen.y * t.y) / gm;   // sideways pull along the floor
  Vec2 up = v2(-gScreen.x / gm, -gScreen.y / gm);

  pet.curled = (pet.action == ACT_CURL || pet.action == ACT_ROLL) && !pet.asleep;
  pet.hiding = (pet.action == ACT_HIDE) && nearBed(14);
  bool calm = onFloor && !pet.slipping && pet.dizzyT <= 0 && pet.hurtT <= 0;
  bool watched = millis() - lastTouchMs < 6000;
  pet.gripping = (pet.action == ACT_GRIP) && b.touching && pet.stamina > 0;
  pet.slipping = onFloor && fabsf(slope) > STEEP && !pet.gripping;

  b.r = pet.curled ? 17 : (pet.hiding ? 18 : 22);
  b.bounce = pet.curled ? 0.55f : 0.22f;
  b.friction = pet.curled ? 1.0f : 7.0f;

  // Where does it want to go?
  bool wantsMove = false;
  float speed = 85;
  switch (pet.action) {
    case ACT_WANDER:
      wantsMove = true;
      if (vlen(v2(pet.target.x - b.x, pet.target.y - b.y)) < 25 || pet.actionTime > 5)
        pet.target = v2(frand(ROOM_L + 30, ROOM_R - 30), frand(ROOM_T + 30, ROOM_B - 30));
      break;
    case ACT_BALL:
      if (ball.active) { pet.target = v2(ball.x, ball.y); wantsMove = true; speed = 140; }
      break;
    case ACT_EAT: {
      int f = nearestFood();
      if (f >= 0) { pet.target = v2(food[f].x, food[f].y); wantsMove = true; speed = 110; }
      break;
    }
    case ACT_SLEEP:
      pet.target = v2(bed.x, bed.y);
      wantsMove = !pet.asleep;
      break;
    case ACT_COME:
      pet.target = callPoint;
      wantsMove = true;
      speed = 120;
      break;
    case ACT_BEG:
      pet.target = callPoint;
      wantsMove = vlen(v2(callPoint.x - b.x, callPoint.y - b.y)) > 40;
      break;
    case ACT_HIDE:
      pet.target = v2(bed.x, bed.y);
      wantsMove = !pet.hiding;
      speed = 130;
      break;
    case ACT_WINDOW:
      pet.target = v2(WIN_X + WIN_W / 2, WIN_Y + WIN_H / 2);
      wantsMove = true;
      break;
    default:
      break;
  }
  if (pet.dizzyT > 0 || pet.hurtT > 0 || pet.energy < 2) wantsMove = false;

  Vec2 toT = v2(pet.target.x - b.x, pet.target.y - b.y);
  float dist = vlen(toT);
  if (dist > 1) pet.look = v2(toT.x / dist, toT.y / dist);

  if (wantsMove && floating) {
    // Paddle through zero-g.
    b.vx += pet.look.x * 90 * dt;
    b.vy += pet.look.y * 90 * dt;
    pet.walkPhase += dt * 10;
    pet.energy -= 0.3f * dt;
  } else if (wantsMove && onFloor && !pet.slipping && !pet.curled) {
    float along = toT.x * t.x + toT.y * t.y;
    float dir = fabsf(along) > 6 ? (along > 0 ? 1 : -1) : 0;
    float vt = b.vx * t.x + b.vy * t.y;
    float want = dir * speed;
    float nv = vt + (want - vt) * clampf(dt * 8, 0, 1);
    b.vx += (nv - vt) * t.x;
    b.vy += (nv - vt) * t.y;
    if (dir != 0) {
      pet.walkPhase += dt * speed * 0.15f;
      pet.energy -= 0.25f * dt;
    }
    // Hop if the target is up off the floor and close by.
    float upDist = toT.x * up.x + toT.y * up.y;
    if (upDist > 45 && fabsf(along) < 60 && pet.hopCool <= 0 && pet.energy > 10) {
      b.vx += up.x * 420;
      b.vy += up.y * 420;
      pet.hopCool = 1.4f;
      pet.energy -= 1.5f;
    }
  }

  // ---- The fun stuff ----
  switch (pet.action) {
    case ACT_DANCE:
      if (calm) {
        pet.danceT += dt;
        float sway = sinf(pet.danceT * 9) * 60;      // shuffle side to side
        b.vx += t.x * (sway - (b.vx * t.x + b.vy * t.y)) * clampf(dt * 6, 0, 1);
        b.vy += t.y * (sway - (b.vx * t.x + b.vy * t.y)) * clampf(dt * 6, 0, 1);
        pet.walkPhase += dt * 14;
        pet.joy = clampf(pet.joy + 4 * dt, 0, 100);
        pet.energy -= 0.4f * dt;
        rewardAcc += (watched ? 0.3f : 0.05f) * dt;   // more fun with an audience
        if (frand(0, 1) < dt * 3)
          spawnParticle(PT_NOTE, b.x + frand(-20, 20), b.y - b.r - 6, frand(-20, 20), -50, 1.0f, rgb(255, 220, 120));
      }
      break;
    case ACT_JUMP:
      if (calm && pet.hopCool <= 0 && pet.energy > 5) {
        b.vx += up.x * 470;
        b.vy += up.y * 470;
        pet.hopCool = 0.9f;
        pet.energy -= 2;
        pet.joy = clampf(pet.joy + 3, 0, 100);
        rewardAcc += pet.energy > 30 ? 0.25f : -0.2f;
      }
      break;
    case ACT_BEG:
      if (activeFood() == 0) rewardAcc -= 0.02f * dt;   // waiting is boring
      break;
    case ACT_HIDE:
      if (pet.hiding) {
        Senses s = sense();
        rewardAcc += (s.room == ROOM_TILTING ? 0.12f : -0.03f) * dt;
      }
      break;
    case ACT_WINDOW: {
      Vec2 w = v2(WIN_X + WIN_W / 2 - b.x, WIN_Y + WIN_H / 2 - b.y);
      float d = vlen(w);
      if (d > 1) pet.look = v2(w.x / d, w.y / d);
      bool below = fabsf((WIN_X + WIN_W / 2 - b.x) * t.x + (WIN_Y + WIN_H / 2 - b.y) * t.y) < 30;
      if (below && calm) {
        pet.joy = clampf(pet.joy + 2 * dt, 0, 100);
        rewardAcc += (!pet.asleep && pet.hunger < 55 && pet.energy > 35 ? 0.12f : 0.0f) * dt;
      }
      break;
    }
    case ACT_ROLL:
      if (b.touching && !floating) {
        float roll = (fmodf(pet.actionTime, 2.0f) < 1.0f ? 1 : -1) * 150;
        float vt = b.vx * t.x + b.vy * t.y;
        b.vx += t.x * (roll - vt) * clampf(dt * 3, 0, 1);
        b.vy += t.y * (roll - vt) * clampf(dt * 3, 0, 1);
        pet.energy -= 0.5f * dt;
        pet.joy = clampf(pet.joy + 3 * dt, 0, 100);
        Senses s = sense();
        rewardAcc += (pet.energy > 30 && s.room == ROOM_CALM ? 0.2f : -0.1f) * dt;
      }
      break;
    default:
      break;
  }

  // Sleeping: on the bed (or anywhere if it can't get there).
  if (pet.action == ACT_SLEEP && !pet.asleep) {
    bool onBed = vlen(v2(bed.x - b.x, bed.y - b.y)) < b.r + bed.r + 8;
    if (onBed || pet.actionTime > 6) pet.asleep = true;
  }
  if (pet.asleep) {
    bool onBed = vlen(v2(bed.x - b.x, bed.y - b.y)) < b.r + bed.r + 10;
    pet.energy += (onBed ? 4.0f : 2.0f) * dt;
    float cozy = lightsOff ? 1.5f : 1.0f;   // sleeping is nicer in the dark
    if (pet.energy < 40) rewardAcc += 0.6f * cozy * dt;
    else if (pet.energy > 85) rewardAcc -= 0.3f * dt;
    else rewardAcc += 0.1f * dt;
    if (frand(0, 1) < dt * 0.8f)
      spawnParticle(PT_ZZZ, b.x + 8, b.y - b.r, 8, -25, 1.6f, RGB565_WHITE);
  }

  if (pet.gripping) {
    pet.stamina -= 22 * dt;
    if (pet.stamina <= 0) decideNow = true;
  } else {
    pet.stamina = fminf(100, pet.stamina + 12 * dt);
  }
}

void eatAndPlay() {
  Body &b = pet.body;
  // Eating
  if (pet.action == ACT_EAT && pet.eatT <= 0 && !pet.held) {
    for (int i = 0; i < MAX_FOOD; i++) {
      if (!food[i].active) continue;
      if (vlen(v2(food[i].x - b.x, food[i].y - b.y)) > b.r + food[i].r + 3) continue;
      food[i].active = false;
      float before = pet.hunger;
      pet.hunger = clampf(pet.hunger - 28, 0, 100);
      pet.eatT = 0.8f;
      pet.treats++;
      sendEvent("eat", before);
      say(before < 20 ? SAY_OVEREAT : SAY_EAT, true);
      if (before > 55) rewardAcc += 2.0f * before / 100.0f;
      else if (before < 20) { rewardAcc -= 0.8f; pet.sadT = 1.0f; }   // tummy ache
      else rewardAcc += 0.4f;
      for (int k = 0; k < 6; k++)
        spawnParticle(PT_CRUMB, food[i].x, food[i].y, frand(-60, 60), frand(-90, 10), 0.6f, rgb(230, 170, 60));
      break;
    }
  }
  // Kicking the toy
  if (ball.active && !pet.held && pet.action == ACT_BALL && pet.kickCool <= 0 &&
      vlen(v2(ball.x - b.x, ball.y - b.y)) < b.r + ball.r + 3) {
    Vec2 g = gScreen;
    float gm = fmaxf(vlen(g), 0.01f);
    float dx = ball.x - b.x, dy = ball.y - b.y;
    float d = fmaxf(vlen(v2(dx, dy)), 1);
    ball.vx += dx / d * 260 - g.x / gm * 220;
    ball.vy += dy / d * 260 - g.y / gm * 220;
    pet.kickCool = 0.6f;
    pet.kicks++;
    sendEvent("kick", pet.energy);
    say(SAY_KICK, false);
    pet.joy = clampf(pet.joy + 6, 0, 100);
    pet.energy -= 1.5f;
    rewardAcc += pet.energy > 15 ? 0.6f : -0.2f;
  }
}

// Keep things from overlapping. Runs after every physics step.
void resolveContacts() {
  Body &b = pet.body;
  bool petSolid = !pet.held && !pet.gripping;
  for (int it = 0; it < 2; it++) {
    if (ball.active && petSolid) collide(b, ball, 3.0f, 1.0f);
    if (!pet.held && !pet.asleep) collideBox(b, bed, 3.0f, 4.0f);
    if (ball.active) collideBox(ball, bed, 1.0f, 4.0f);
    for (int i = 0; i < MAX_FOOD; i++) {
      if (!food[i].active) continue;
      if (petSolid) collide(b, food[i], 3.0f, 0.3f);
      collideBox(food[i], bed, 0.3f, 4.0f);
      if (ball.active) collide(ball, food[i], 1.0f, 0.3f);
      for (int j = i + 1; j < MAX_FOOD; j++)
        if (food[j].active) collide(food[i], food[j], 0.3f, 0.3f);
    }
  }
}

void updateNeeds(float dt) {
  pet.age += dt;
  pet.hunger = clampf(pet.hunger + 0.18f * dt, 0, 100);
  if (!pet.asleep) pet.energy -= (lightsOff ? 0.25f : 0.10f) * dt;
  pet.energy = clampf(pet.energy, 0, 100);
  pet.joy = clampf(pet.joy - 0.10f * dt, 0, 100);
  if (pet.hunger < 80) pet.health = clampf(pet.health + 0.15f * dt, 0, 100);
  if (pet.hunger > 92) pet.health = clampf(pet.health - 0.1f * dt, 0, 100);
  if (pet.hunger > 80) rewardAcc -= 0.15f * dt;
  if (pet.energy < 15) rewardAcc -= 0.10f * dt;

  pet.hurtT -= dt; pet.dizzyT -= dt; pet.eatT -= dt; pet.sadT -= dt;
  pet.kickCool -= dt; pet.hopCool -= dt; pet.bubbleT -= dt; pet.blinkT -= dt;
  sayT -= dt;
  if (pet.blinkT < -0.15f) pet.blinkT = frand(2, 5);
  pet.actionTime += dt;
}

// Put a body under your finger (kept inside the room) and give it your finger's speed.
void holdAtFinger(Body &b) {
  float ex = b.isBox ? b.hw : b.r, ey = b.isBox ? b.hh : b.r;
  b.x = clampf(fx, ROOM_L + ex, ROOM_R - ex);
  b.y = clampf(fy, ROOM_T + ey, ROOM_B - ey);
  b.vx = fvx;
  b.vy = fvy;
  b.airTime = 0;
  b.touching = 0;
}

void updateWorld(float dt) {
  // A snack you're holding can get eaten (hand-feeding!) or cleaned up.
  if (grabCand == GRAB_FOOD && (grabFood < 0 || !food[grabFood].active)) {
    grabCand = GRAB_NONE;
    grabbing = false;
  }

  Vec2 g = v2(gScreen.x * PX_PER_G, gScreen.y * PX_PER_G);
  if (floating) { g.x *= FLOAT_GRAVITY; g.y *= FLOAT_GRAVITY; }

  updateNeeds(dt);
  petBehave(dt);
  visitorUpdate(dt);

  // Things worth mentioning
  static bool wasAsleep = false, wasFloating = false;
  if (pet.asleep != wasAsleep) { wasAsleep = pet.asleep; say(pet.asleep ? SAY_SLEEP : SAY_WAKE, true); }
  if (floating != wasFloating) { wasFloating = floating; if (floating) say(SAY_FLOAT, true); }
  if (millis() - lastSayMs > 25000 && !pet.asleep) say(SAY_IDLE, false);

  // Bed shape follows the floor: wide along it, short away from it.
  uint8_t fw = floorWall(gScreen);
  bool sideways = !floating && (fw == WALL_L || fw == WALL_R);
  bed.hw = sideways ? 25 : 38;
  bed.hh = sideways ? 38 : 25;

  // Physics in fixed 1/240 s steps: smooth at any frame rate.
  physAcc += dt;
  int steps = 0;
  pet.body.impact = 0;
  ball.impact = 0;
  while (physAcc >= PHYS_DT && steps < 16) {
    physAcc -= PHYS_DT;
    steps++;
    float h = PHYS_DT;
    if (pet.held) {
      pet.body.x = clampf(fx, ROOM_L + pet.body.r, ROOM_R - pet.body.r);
      pet.body.y = clampf(fy, ROOM_T + pet.body.r, ROOM_B - pet.body.r);
      pet.body.vx = fvx; pet.body.vy = fvy;
      pet.body.airTime = 0;
    } else if (pet.gripping) {
      pet.body.vx = 0; pet.body.vy = 0;  // holding on tight
    } else {
      stepBody(pet.body, g, h, floating);
    }
    // Whatever your finger is holding follows it; everything else falls.
    if (ball.active) {
      if (grabbing && grabCand == GRAB_BALL) holdAtFinger(ball);
      else stepBody(ball, g, h, floating);
    }
    if (grabbing && grabCand == GRAB_BED) holdAtFinger(bed);
    else stepBody(bed, g, h, floating);
    for (int i = 0; i < MAX_FOOD; i++) {
      if (!food[i].active) continue;
      if (grabbing && grabCand == GRAB_FOOD && i == grabFood) holdAtFinger(food[i]);
      else stepBody(food[i], g, h, floating);
    }
    resolveContacts();
    visitorStep(g, h);
  }
  if (physAcc > PHYS_DT * 4) physAcc = 0;   // don't spiral after a long pause
  eatAndPlay();

  // Asleep in bed: settle onto the mattress.
  if (pet.asleep && !pet.held && nearBed(12)) {
    Vec2 d = v2(cosf(bedAngle), sinf(bedAngle));
    float tx = bed.x - d.x * 8, ty = bed.y - d.y * 8;
    float k = clampf(dt * 4, 0, 1);
    pet.body.x += (tx - pet.body.x) * k;
    pet.body.y += (ty - pet.body.y) * k;
    pet.body.vx = bed.vx; pet.body.vy = bed.vy;
  }

  // Hard landing?
  if (!pet.held && pet.body.impact > 250) {
    int n = (int)clampf((pet.body.impact - 200) / 80, 1, 8);
    for (int k = 0; k < n; k++)
      spawnParticle(PT_DUST, pet.body.x, pet.body.y, frand(-80, 80), frand(-80, 80), 0.5f, rgb(200, 190, 210));
  }
  if (!pet.held && pet.body.impact > HURT_SPEED) hurt((pet.body.impact - HURT_SPEED) / 35.0f);

  // Petting
  if (petting) {
    pet.joy = clampf(pet.joy + 12 * dt, 0, 100);
    rewardAcc += 0.7f * dt;
    if (frand(0, 1) < dt * 5)
      spawnParticle(PT_HEART, pet.body.x + frand(-15, 15), pet.body.y - 20, frand(-20, 20), -60, 1.0f, rgb(255, 90, 140));
  }
  if (pet.dizzyT > 0 && frand(0, 1) < dt * 3)
    spawnParticle(PT_STAR, pet.body.x + frand(-20, 20), pet.body.y - 25, frand(-20, 20), -20, 0.6f, RGB565_YELLOW);

  // Particles
  for (int i = 0; i < MAX_PARTICLES; i++) {
    Particle &p = parts[i];
    if (!p.active) continue;
    p.life -= dt;
    if (p.life <= 0) { p.active = false; continue; }
    if (p.kind == PT_CRUMB || p.kind == PT_DUST) {
      p.vx += gScreen.x * 300 * dt;
      p.vy += gScreen.y * 300 * dt;
    }
    p.x += p.vx * dt;
    p.y += p.vy * dt;
  }

  // Time to choose what to do next?
  sinceDecide += dt;
  if (sinceDecide > 2.2f || (decideNow && sinceDecide > 0.4f)) think();

  saveTimer += dt;
  if (saveTimer > 90) { saveTimer = 0; saveAll(); }

  brainSendTimer += dt;
  if (brainSendTimer > 8) { brainSendTimer = 0; sendBrain(); }
}

// ============================================================ drawing

// Point on Bloop's body: lx to its right, ly toward its feet.
Vec2 bp(float lx, float ly) {
  Vec2 d = downDir();
  Vec2 r = v2(d.y, -d.x);
  return v2(pet.body.x + lx * r.x + ly * d.x, pet.body.y + lx * r.y + ly * d.y);
}

uint16_t bodyColor() {
  if (pet.hurtT > 0) return rgb(235, 100, 100);
  if (pet.health < 30) return rgb(150, 150, 120);
  float j = pet.joy / 100.0f;
  return rgb(90 + (uint8_t)(10 * j), 120 + (uint8_t)(90 * j), 190 - (uint8_t)(20 * j));
}

void drawEye(Vec2 c, float side) {
  if (pet.asleep || pet.curled) {
    cv->drawLine(c.x - 5, c.y, c.x + 5, c.y, RGB565_BLACK);
    cv->drawLine(c.x - 5, c.y + 1, c.x + 5, c.y + 1, RGB565_BLACK);
    return;
  }
  if (pet.hurtT > 0) {
    cv->drawLine(c.x - 4, c.y - 4, c.x + 4, c.y + 4, RGB565_BLACK);
    cv->drawLine(c.x - 4, c.y + 4, c.x + 4, c.y - 4, RGB565_BLACK);
    return;
  }
  cv->fillCircle(c.x, c.y, 7, RGB565_WHITE);
  if (pet.dizzyT > 0) {
    float a = millis() / 120.0f + side;
    cv->drawCircle(c.x, c.y, 4, RGB565_BLACK);
    cv->fillCircle(c.x + cosf(a) * 3, c.y + sinf(a) * 3, 2, RGB565_BLACK);
    return;
  }
  if (pet.blinkT < 0) {
    cv->fillCircle(c.x, c.y, 7, bodyColor());
    cv->drawLine(c.x - 6, c.y, c.x + 6, c.y, RGB565_BLACK);
    return;
  }
  cv->fillCircle(c.x + pet.look.x * 3, c.y + pet.look.y * 3, 3, RGB565_BLACK);
  cv->fillCircle(c.x + pet.look.x * 3 - 1, c.y + pet.look.y * 3 - 1, 1, RGB565_WHITE);
}

void drawBloop() {
  float r = pet.body.r;
  uint16_t col = bodyColor();
  uint16_t dark = rgb(50, 70, 110);
  Vec2 c = v2(pet.body.x, pet.body.y);
  Vec2 d = downDir();
  float downDeg = atan2f(d.y, d.x) * 180.0f / PI;

  if (pet.curled) {
    cv->fillCircle(c.x, c.y, r, col);
    cv->drawCircle(c.x, c.y, r - 4, dark);
    cv->drawCircle(c.x, c.y, r - 9, dark);
    Vec2 e1 = bp(-5, -4), e2 = bp(5, -4);
    cv->drawLine(e1.x - 3, e1.y, e1.x + 3, e1.y, RGB565_BLACK);
    cv->drawLine(e2.x - 3, e2.y, e2.x + 3, e2.y, RGB565_BLACK);
    return;
  }

  // Feet
  float step = sinf(pet.walkPhase) * 5;
  Vec2 f1 = bp(-r * 0.45f + step, r * 0.85f), f2 = bp(r * 0.45f - step, r * 0.85f);
  cv->fillCircle(f1.x, f1.y, 7, dark);
  cv->fillCircle(f2.x, f2.y, 7, dark);

  // Ears
  Vec2 a1 = bp(-r * 0.75f, -r * 0.45f), a2 = bp(-r * 0.2f, -r * 0.9f), a3 = bp(-r * 0.75f, -r * 1.35f);
  Vec2 b1 = bp(r * 0.75f, -r * 0.45f), b2 = bp(r * 0.2f, -r * 0.9f), b3 = bp(r * 0.75f, -r * 1.35f);
  if (pet.sadT > 0 || pet.health < 30) {  // droopy ears
    a3 = bp(-r * 1.25f, -r * 0.6f);
    b3 = bp(r * 1.25f, -r * 0.6f);
  }
  cv->fillTriangle(a1.x, a1.y, a2.x, a2.y, a3.x, a3.y, col);
  cv->fillTriangle(b1.x, b1.y, b2.x, b2.y, b3.x, b3.y, col);

  // Body and belly
  cv->fillCircle(c.x, c.y, r, col);
  Vec2 belly = bp(0, r * 0.35f);
  cv->fillCircle(belly.x, belly.y, r * 0.5f, rgb(220, 235, 245));

  // Gripping: little arms reaching to the wall
  if (pet.gripping) {
    Vec2 h1 = bp(-r * 1.1f, 0), h2 = bp(r * 1.1f, 0);
    cv->fillCircle(h1.x, h1.y, 5, dark);
    cv->fillCircle(h2.x, h2.y, 5, dark);
  }

  // Face
  drawEye(bp(-r * 0.38f, -r * 0.2f), 0);
  drawEye(bp(r * 0.38f, -r * 0.2f), 3);
  Vec2 m = bp(0, r * 0.12f);
  if (pet.eatT > 0) {
    cv->fillCircle(m.x, m.y, 4 + (int)(sinf(millis() / 60.0f) * 2), rgb(120, 30, 40));
  } else if (pet.hurtT > 0 || pet.sadT > 0 || pet.joy < 25) {
    Vec2 m2 = bp(0, r * 0.32f);
    cv->fillArc(m2.x, m2.y, 7, 5, downDeg + 180 - 50, downDeg + 180 + 50, RGB565_BLACK);
  } else if (!pet.asleep) {
    cv->fillArc(m.x, m.y, 7, 5, downDeg - 55, downDeg + 55, RGB565_BLACK);
  }
  if (pet.joy > 70 && !pet.asleep) {  // blush
    Vec2 k1 = bp(-r * 0.62f, r * 0.08f), k2 = bp(r * 0.62f, r * 0.08f);
    cv->fillCircle(k1.x, k1.y, 3, rgb(255, 140, 170));
    cv->fillCircle(k2.x, k2.y, 3, rgb(255, 140, 170));
  }
}

// ---- Text helpers (proportional fonts) ----
const GFXfont *const F_SMALL = &FreeSans9pt7b;
const GFXfont *const F_BOLD = &FreeSansBold9pt7b;
const GFXfont *const F_TITLE = &FreeSansBold12pt7b;

void useFont(const GFXfont *f) {
  cv->setFont(f);
  cv->setTextSize(1);
}
void plainFont() { cv->setFont((const GFXfont *)NULL); }

int textWidth(const GFXfont *f, const char *str) {
  int16_t x1, y1;
  uint16_t w, h;
  useFont(f);
  cv->getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  plainFont();
  return w;
}
// y is the text baseline.
void textAt(const GFXfont *f, int x, int y, const char *str, uint16_t color) {
  useFont(f);
  cv->setTextColor(color);
  cv->setCursor(x, y);
  cv->print(str);
  plainFont();
}
void textCentered(const GFXfont *f, int cx, int y, const char *str, uint16_t color) {
  textAt(f, cx - textWidth(f, str) / 2, y, str, color);
}

// Four-cornered shape (two triangles), for things that turn with gravity.
void fillQuad(Vec2 a, Vec2 b, Vec2 c, Vec2 d, uint16_t col) {
  cv->fillTriangle(a.x, a.y, b.x, b.y, c.x, c.y, col);
  cv->fillTriangle(a.x, a.y, c.x, c.y, d.x, d.y, col);
}

// Point on the bed: lx along it, ly toward the floor. Its legs stand on the floor.
Vec2 bedPt(float lx, float ly) {
  Vec2 d = v2(cosf(bedAngle), sinf(bedAngle));
  Vec2 r = v2(d.y, -d.x);
  return v2(bed.x + lx * r.x + ly * d.x, bed.y + lx * r.y + ly * d.y);
}
void bedRect(float x0, float y0, float x1, float y1, uint16_t col) {
  fillQuad(bedPt(x0, y0), bedPt(x1, y0), bedPt(x1, y1), bedPt(x0, y1), col);
}

const uint16_t BLANKET = 0x4C3B;   // soft blue

void drawBed() {
  uint16_t wood = rgb(150, 100, 60), woodDark = rgb(110, 70, 40);
  bedRect(-34, 17, -29, 26, woodDark);          // legs
  bedRect(29, 17, 34, 26, woodDark);
  bedRect(-36, 8, 36, 18, wood);                // frame
  bedRect(-38, -24, -31, 18, wood);             // headboard
  bedRect(31, -8, 38, 18, wood);                // footboard
  bedRect(-31, -3, 31, 9, rgb(240, 236, 228));  // mattress
  Vec2 p = bedPt(-21, -6);                      // pillow
  cv->fillCircle(p.x, p.y, 7, RGB565_WHITE);
  p = bedPt(-15, -6);
  cv->fillCircle(p.x, p.y, 7, RGB565_WHITE);
  if (!(pet.asleep && nearBed(12))) {
    bedRect(-6, -4, 31, 9, BLANKET);            // blanket, folded back
    bedRect(-6, -4, -2, 9, rgb(140, 180, 240));
  }
}

// When Bloop sleeps in bed, tuck it in.
void drawBlanketOverBloop() {
  float r = pet.body.r;
  fillQuad(bp(-r * 1.25f, -r * 0.05f), bp(r * 1.25f, -r * 0.05f), bp(r * 1.25f, r * 1.0f), bp(-r * 1.25f, r * 1.0f), BLANKET);
  fillQuad(bp(-r * 1.25f, -r * 0.05f), bp(r * 1.25f, -r * 0.05f), bp(r * 1.25f, r * 0.2f), bp(-r * 1.25f, r * 0.2f), rgb(140, 180, 240));
}

void drawToy() {
  if (!ball.active) return;
  if (toy == TOY_BLOCK) {
    float a = toySpin, h = ball.r;
    Vec2 c = v2(ball.x, ball.y);
    Vec2 u = v2(cosf(a) * h, sinf(a) * h), v = v2(-sinf(a) * h, cosf(a) * h);
    fillQuad(v2(c.x - u.x - v.x, c.y - u.y - v.y), v2(c.x + u.x - v.x, c.y + u.y - v.y),
             v2(c.x + u.x + v.x, c.y + u.y + v.y), v2(c.x - u.x + v.x, c.y - u.y + v.y), rgb(250, 200, 60));
    cv->setTextSize(1);
    cv->setTextColor(rgb(150, 90, 20));
    cv->setCursor(ball.x - 2, ball.y - 3);
    cv->print("B");
  } else if (toy == TOY_BEACH) {
    cv->fillCircle(ball.x, ball.y, ball.r, RGB565_WHITE);
    const uint16_t cols[3] = {rgb(240, 70, 80), rgb(60, 140, 240), rgb(250, 200, 50)};
    for (int k = 0; k < 3; k++) {
      float a0 = toySpin + k * 2.094f;
      Vec2 c = v2(ball.x, ball.y);
      cv->fillTriangle(c.x, c.y, c.x + cosf(a0) * ball.r, c.y + sinf(a0) * ball.r,
                       c.x + cosf(a0 + 1.0f) * ball.r, c.y + sinf(a0 + 1.0f) * ball.r, cols[k]);
    }
    cv->fillCircle(ball.x, ball.y, 3, RGB565_WHITE);
  } else {
    cv->fillCircle(ball.x, ball.y, ball.r, rgb(240, 70, 80));
    float a = toySpin;
    cv->drawLine(ball.x - cosf(a) * ball.r, ball.y - sinf(a) * ball.r, ball.x + cosf(a) * ball.r, ball.y + sinf(a) * ball.r, RGB565_WHITE);
    cv->fillCircle(ball.x - 4, ball.y - 4, 3, rgb(255, 170, 170));
  }
}

// Word-wrapped bubble with a little tail pointing at Bloop.
void drawBubble(const char *txt) { drawBubbleAt(pet.body.x, pet.body.y, pet.body.r, txt, RGB565_WHITE); }

// Same, pointing at whoever is at (px, py) with radius pr (Pip uses this too).
void drawBubbleAt(float px, float py, float pr, const char *txt, uint16_t fill) {
  const int maxW = 180, lineH = 18, padX = 10, padY = 7;
  char lines[3][40];
  int n = 0, widest = 0;
  char word[40];
  lines[0][0] = 0;
  const char *p = txt;
  while (*p && n < 3) {
    int wl = 0;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && wl < 38) word[wl++] = *p++;
    word[wl] = 0;
    if (!wl) break;
    char trial[80];
    snprintf(trial, sizeof(trial), "%s%s%s", lines[n], lines[n][0] ? " " : "", word);
    if (textWidth(F_SMALL, trial) > maxW && lines[n][0]) {
      if (++n >= 3) break;
      snprintf(lines[n], sizeof(lines[n]), "%s", word);
    } else {
      snprintf(lines[n], sizeof(lines[n]), "%s", trial);
    }
  }
  int count = (n < 3) ? n + 1 : 3;
  for (int i = 0; i < count; i++) widest = max(widest, textWidth(F_SMALL, lines[i]));
  int w = widest + padX * 2, h = count * lineH + padY * 2 - 4;

  // Above the speaker if there's room, otherwise below.
  float top = py - pr - 30;
  bool above = top - h > ROOM_T + 4;
  int bx = (int)clampf(px - w / 2, ROOM_L + 4, ROOM_R - w - 4);
  int by = above ? (int)(top - h) : (int)clampf(py + pr + 16, ROOM_T + 4, ROOM_B - h - 4);
  uint16_t edge = rgb(40, 36, 60);
  int tx = (int)clampf(px, bx + 12, bx + w - 12);
  if (above) cv->fillTriangle(tx - 7, by + h - 2, tx + 7, by + h - 2, (int)px, by + h + 10, fill);
  else cv->fillTriangle(tx - 7, by + 2, tx + 7, by + 2, (int)px, by - 10, fill);
  cv->fillRoundRect(bx, by, w, h, 10, fill);
  cv->drawRoundRect(bx, by, w, h, 10, edge);
  for (int i = 0; i < count; i++) textAt(F_SMALL, bx + padX, by + padY + 12 + i * lineH, lines[i], rgb(30, 28, 40));
}

// Soft shadow on the floor under Bloop.
void drawShadow() {
  if (floating || pet.held) return;
  uint8_t fw = floorWall(gScreen);
  float x = pet.body.x, y = pet.body.y, gap;
  switch (fw) {
    case WALL_B: gap = ROOM_B - (y + pet.body.r); y = ROOM_B - 3; break;
    case WALL_T: gap = (y - pet.body.r) - ROOM_T; y = ROOM_T + 3; break;
    case WALL_L: gap = (x - pet.body.r) - ROOM_L; x = ROOM_L + 3; break;
    default:     gap = ROOM_R - (x + pet.body.r); x = ROOM_R - 3; break;
  }
  if (gap > 140) return;
  int len = (int)(pet.body.r * (1.0f - gap / 200.0f)), thick = 3;
  uint16_t c = lightsOff ? rgb(14, 12, 26) : rgb(36, 30, 60);
  if (fw == WALL_B || fw == WALL_T) cv->fillEllipse(x, y, len, thick, c);
  else cv->fillEllipse(x, y, thick, len, c);
}

void drawRoom() {
  // Wallpaper (dimmer with the lights off)
  uint16_t wall = lightsOff ? rgb(26, 22, 44) : rgb(58, 50, 92);
  uint16_t stripe = lightsOff ? rgb(30, 26, 50) : rgb(66, 58, 104);
  cv->fillRect(0, HUD_H, SCREEN_W, TOOLBAR_Y - HUD_H, wall);
  for (int x = 12; x < SCREEN_W; x += 28) cv->fillRect(x, HUD_H, 8, TOOLBAR_Y - HUD_H, stripe);

  // Window on the back wall: day sky, or night when Bloop sleeps
  int wx = 105, wy = 120, ww = 110, wh = 86;
  bool night = pet.asleep || lightsOff;
  cv->fillRect(wx - 5, wy - 5, ww + 10, wh + 10, rgb(140, 100, 70));
  cv->fillRect(wx, wy, ww, wh, night ? rgb(20, 24, 60) : rgb(120, 190, 240));
  if (night) {
    cv->fillCircle(wx + 80, wy + 25, 12, rgb(250, 240, 200));
    cv->fillCircle(wx + 86, wy + 21, 11, rgb(20, 24, 60));
    cv->fillCircle(wx + 20, wy + 15, 1, RGB565_WHITE);
    cv->fillCircle(wx + 45, wy + 55, 1, RGB565_WHITE);
    cv->fillCircle(wx + 30, wy + 40, 1, RGB565_WHITE);
  } else {
    cv->fillCircle(wx + 85, wy + 22, 11, rgb(255, 220, 90));
    cv->fillCircle(wx + 30, wy + 50, 12, RGB565_WHITE);
    cv->fillCircle(wx + 45, wy + 46, 14, RGB565_WHITE);
    cv->fillCircle(wx + 60, wy + 52, 10, RGB565_WHITE);
  }
  cv->fillRect(wx + ww / 2 - 2, wy, 4, wh, rgb(140, 100, 70));
  cv->fillRect(wx, wy + wh / 2 - 2, ww, 4, rgb(140, 100, 70));

  // Picture frame
  cv->fillRect(36, 270, 54, 42, rgb(190, 150, 70));
  cv->fillRect(41, 275, 44, 32, rgb(240, 230, 210));
  cv->fillCircle(63, 293, 9, rgb(110, 200, 170));
  cv->fillCircle(60, 291, 2, RGB565_BLACK);
  cv->fillCircle(66, 291, 2, RGB565_BLACK);

  // Walls: the current floor gets carpet
  uint16_t wood = rgb(110, 75, 50), carpet = rgb(170, 80, 70);
  uint8_t fw = floating ? 0 : floorWall(gScreen);
  int T = (int)ROOM_T, B = (int)ROOM_B, L = (int)ROOM_L, R = (int)ROOM_R;
  cv->fillRect(0, HUD_H, SCREEN_W, T - HUD_H, fw == WALL_T ? carpet : wood);
  cv->fillRect(0, B, SCREEN_W, TOOLBAR_Y - B, fw == WALL_B ? carpet : wood);
  cv->fillRect(0, HUD_H, L, TOOLBAR_Y - HUD_H, fw == WALL_L ? carpet : wood);
  cv->fillRect(R, HUD_H, SCREEN_W - R, TOOLBAR_Y - HUD_H, fw == WALL_R ? carpet : wood);

  drawBed();

  // Food
  for (int i = 0; i < MAX_FOOD; i++) {
    if (!food[i].active) continue;
    cv->fillCircle(food[i].x, food[i].y, food[i].r, rgb(230, 150, 50));
    cv->fillCircle(food[i].x - 2, food[i].y - 2, 2, rgb(255, 220, 140));
  }

  drawToy();

  drawShadow();
  drawBloop();
  if (pet.asleep && nearBed(12)) drawBlanketOverBloop();
  drawVisitor();

  // Particles
  for (int i = 0; i < MAX_PARTICLES; i++) {
    Particle &p = parts[i];
    if (!p.active) continue;
    switch (p.kind) {
      case PT_HEART:
        cv->fillCircle(p.x - 3, p.y, 3, p.color);
        cv->fillCircle(p.x + 3, p.y, 3, p.color);
        cv->fillTriangle(p.x - 6, p.y + 1, p.x + 6, p.y + 1, p.x, p.y + 8, p.color);
        break;
      case PT_STAR:
        cv->drawLine(p.x - 4, p.y, p.x + 4, p.y, p.color);
        cv->drawLine(p.x, p.y - 4, p.x, p.y + 4, p.color);
        break;
      case PT_CRUMB:
        cv->fillRect(p.x, p.y, 3, 3, p.color);
        break;
      case PT_DUST:
        cv->fillCircle(p.x, p.y, (int)(p.life * 8) + 1, p.color);
        break;
      case PT_NOTE:
        cv->fillCircle(p.x, p.y + 6, 3, p.color);
        cv->drawFastVLine(p.x + 2, p.y - 2, 8, p.color);
        cv->drawLine(p.x + 2, p.y - 2, p.x + 6, p.y, p.color);
        break;
      case PT_ZZZ:
        cv->setTextSize(p.life > 0.8f ? 2 : 1);
        cv->setTextColor(p.color);
        cv->setCursor(p.x, p.y);
        cv->print("z");
        break;
    }
  }

  // Speech / thought bubble
  if (!pet.asleep && (sayT > 0 || pet.bubbleT > 0)) {
    char txt[64];
    if (sayT > 0) snprintf(txt, sizeof(txt), "%s", sayText);
    else snprintf(txt, sizeof(txt), "%s%s", ACTION_THOUGHTS[pet.action], brain.lastWasExplore ? "?" : "");
    drawBubble(txt);
  }

  if (floating) textCentered(F_BOLD, SCREEN_W / 2, ROOM_T + 24, "ZERO-G", rgb(180, 220, 255));
}

// Little icons for the status bars.
void iconFood(int x, int y) { cv->fillCircle(x, y, 4, rgb(230, 150, 50)); cv->fillCircle(x - 1, y - 1, 1, rgb(255, 220, 140)); }
void iconEnergy(int x, int y) {
  uint16_t c = rgb(250, 210, 70);
  cv->fillTriangle(x + 1, y - 5, x - 3, y + 1, x + 1, y + 1, c);
  cv->fillTriangle(x - 1, y + 5, x + 3, y - 1, x - 1, y - 1, c);
}
void iconJoy(int x, int y) {
  uint16_t c = rgb(255, 110, 160);
  cv->fillCircle(x - 2, y - 1, 2, c); cv->fillCircle(x + 2, y - 1, 2, c);
  cv->fillTriangle(x - 4, y, x + 4, y, x, y + 4, c);
}
void iconHealth(int x, int y) { uint16_t c = rgb(110, 220, 120); cv->fillRect(x - 1, y - 4, 3, 9, c); cv->fillRect(x - 4, y - 1, 9, 3, c); }

void drawMeter(int x, int y, int which, float v) {
  switch (which) {
    case 0: iconFood(x + 4, y + 3); break;
    case 1: iconEnergy(x + 4, y + 3); break;
    case 2: iconJoy(x + 4, y + 3); break;
    default: iconHealth(x + 4, y + 3); break;
  }
  const uint16_t cols[4] = {rgb(230, 150, 50), rgb(250, 210, 70), rgb(255, 110, 160), rgb(110, 220, 120)};
  v = clampf(v, 0, 100);
  cv->fillRoundRect(x + 12, y, 44, 7, 3, rgb(48, 46, 62));
  int w = (int)(44 * v / 100);
  if (w > 2) cv->fillRoundRect(x + 12, y, w, 7, 3, v < 20 ? rgb(235, 80, 80) : cols[which]);
}

void drawHud() {
  cv->fillRect(0, 0, SCREEN_W, HUD_H, rgb(22, 20, 34));
  cv->drawFastHLine(0, HUD_H - 1, SCREEN_W, rgb(48, 44, 70));
  // Bloop's face as an avatar
  cv->fillCircle(22, 23, 15, bodyColor());
  cv->fillCircle(17, 20, 3, RGB565_WHITE); cv->fillCircle(27, 20, 3, RGB565_WHITE);
  cv->fillCircle(17, 20, 1, RGB565_BLACK); cv->fillCircle(27, 20, 1, RGB565_BLACK);
  cv->fillArc(22, 25, 5, 4, 20, 160, RGB565_BLACK);
  textAt(F_BOLD, 44, 20, "Bloop", rgb(140, 230, 200));
  char sub[40];
  snprintf(sub, sizeof(sub), "Lv %lu  %s", (unsigned long)(brain.decisions / 40 + 1), pet.asleep ? "asleep" : ACTION_NAMES[pet.action]);
  cv->setTextSize(1);
  cv->setTextColor(rgb(170, 168, 190));
  cv->setCursor(44, 30);
  cv->print(sub);

  drawMeter(152, 10, 0, 100 - pet.hunger);
  drawMeter(152, 28, 1, pet.energy);
  drawMeter(214, 10, 2, pet.joy);
  drawMeter(214, 28, 3, pet.health);

  // Which way is down
  int cx = 297, cy = 23;
  cv->fillCircle(cx, cy, 15, rgb(34, 32, 50));
  cv->drawCircle(cx, cy, 15, rgb(70, 66, 96));
  if (floating) {
    cv->setTextColor(rgb(180, 220, 255));
    cv->setCursor(cx - 5, cy - 3);
    cv->print("0g");
  } else {
    float m = fmaxf(vlen(gScreen), 0.01f);
    float ux = gScreen.x / m, uy = gScreen.y / m;
    int tx = cx + ux * 12, ty = cy + uy * 12;
    cv->drawLine(cx - ux * 9, cy - uy * 9, tx, ty, RGB565_WHITE);
    cv->fillTriangle(tx, ty, tx - ux * 6 - uy * 4, ty - uy * 6 + ux * 4, tx - ux * 6 + uy * 4, ty - uy * 6 - ux * 4, RGB565_WHITE);
  }
}

const char *const TOOL_LABELS[5] = {"Feed", "Toy", "Good", "No", "More"};

void drawToolIcon(int i, int cx, int cy, uint16_t c) {
  switch (i) {
    case 0:  // bowl with food
      cv->fillCircle(cx - 4, cy - 2, 3, c); cv->fillCircle(cx + 2, cy - 3, 3, c); cv->fillCircle(cx + 6, cy - 1, 2, c);
      cv->fillArc(cx, cy, 10, 0, 0, 180, c);
      break;
    case 1:  // ball
      cv->drawCircle(cx, cy, 9, c); cv->drawCircle(cx, cy, 8, c);
      cv->drawLine(cx - 8, cy, cx + 8, cy, c);
      cv->drawFastVLine(cx, cy - 8, 17, c);
      break;
    case 2:  // heart
      cv->fillCircle(cx - 4, cy - 2, 5, c); cv->fillCircle(cx + 4, cy - 2, 5, c);
      cv->fillTriangle(cx - 9, cy, cx + 9, cy, cx, cy + 9, c);
      break;
    case 3:  // X
      for (int d = -1; d <= 1; d++) {
        cv->drawLine(cx - 7 + d, cy - 7, cx + 7 + d, cy + 7, c);
        cv->drawLine(cx - 7 + d, cy + 7, cx + 7 + d, cy - 7, c);
      }
      break;
    default:  // three dots
      cv->fillCircle(cx - 8, cy, 3, c); cv->fillCircle(cx, cy, 3, c); cv->fillCircle(cx + 8, cy, 3, c);
      break;
  }
}

void drawToolbar() {
  cv->fillRect(0, TOOLBAR_Y, SCREEN_W, SCREEN_H - TOOLBAR_Y, rgb(22, 20, 34));
  cv->drawFastHLine(0, TOOLBAR_Y, SCREEN_W, rgb(48, 44, 70));
  const uint16_t cols[5] = {rgb(214, 128, 52), rgb(206, 72, 84), rgb(58, 160, 98), rgb(160, 64, 70), rgb(92, 92, 176)};
  bool flash = toolFlash >= 0 && millis() - toolFlashMs < 140;
  for (int i = 0; i < 5; i++) {
    int x = i * 64 + 4;
    bool on = (i == 4 && screen != SCR_ROOM) || (flash && i == toolFlash);
    uint16_t bg = on ? RGB565_WHITE : cols[i], fg = on ? cols[i] : RGB565_WHITE;
    cv->fillRoundRect(x, TOOLBAR_Y + 6, 56, 46, 12, bg);
    drawToolIcon(i, x + 28, TOOLBAR_Y + 21, fg);
    textCentered(F_SMALL, x + 28, TOOLBAR_Y + 46, TOOL_LABELS[i], fg);
  }
}

// ---- Brain screen ----
const int BTN_Y = 382;

void describeState(const Senses &s, char *out, size_t n) {
  const char *room = s.room == ROOM_CALM ? "calm" : (s.room == ROOM_TILTING ? "TILTING" : "floating");
  snprintf(out, n, "%s, %s, room %s%s%s%s",
           s.hungry ? "hungry" : "full", s.tired ? "tired" : "awake", room,
           s.ballNear ? ", ball near" : "", s.foodOut ? ", food out" : "",
           s.attention ? ", you're here" : "");
}

bool habitTilt(const Senses &s) { return s.room == ROOM_TILTING; }
bool habitHungryFood(const Senses &s) { return s.hungry && s.foodOut && s.room == ROOM_CALM; }
bool habitTired(const Senses &s) { return s.tired && !s.hungry && s.room == ROOM_CALM; }
bool habitFloat(const Senses &s) { return s.room == ROOM_FLOATING; }

void drawHabit(int y, const char *when, Action a, float score) {
  cv->setTextColor(RGB565_LIGHTGREY);
  cv->setCursor(10, y);
  cv->print(when);
  cv->setTextColor(fabsf(score) < 0.05f ? RGB565_DARKGREY : rgb(120, 220, 190));
  cv->setCursor(170, y);
  cv->print(fabsf(score) < 0.05f ? "(not learned)" : ACTION_NAMES[a]);
}

void drawBrain() {
  cv->fillRect(0, HUD_H, SCREEN_W, TOOLBAR_Y - HUD_H, rgb(16, 14, 26));
  textAt(F_TITLE, 10, HUD_H + 26, "Bloop's brain", rgb(170, 176, 255));

  cv->setTextSize(1);
  cv->setTextColor(RGB565_LIGHTGREY);
  cv->setCursor(10, HUD_H + 34);
  cv->printf("Choices learned from: %lu   Exploring: %d%%", (unsigned long)brain.decisions, (int)(brain.epsilon() * 100));
  cv->setCursor(10, HUD_H + 46);
  cv->printf("Age %dm  Falls %lu  Snacks %lu  Kicks %lu", (int)(pet.age / 60), (unsigned long)pet.falls,
             (unsigned long)pet.treats, (unsigned long)pet.kicks);

  char buf[96];
  describeState(decodeState(curState), buf, sizeof(buf));
  cv->setTextColor(RGB565_WHITE);
  cv->setCursor(10, HUD_H + 62);
  cv->print("Right now: ");
  cv->setTextColor(rgb(255, 210, 120));
  cv->setCursor(10, HUD_H + 74);
  cv->print(buf);

  // How much Bloop expects each action to pay off right now
  int y0 = HUD_H + 86;
  float lo = -3, hi = 3;
  for (int a = 0; a < NUM_ACTIONS; a++) {
    float v = brain.q[curState][a];
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  float span = fmaxf(fabsf(lo), fabsf(hi));
  int mid = 200, half = 105;
  for (int a = 0; a < NUM_ACTIONS; a++) {
    int y = y0 + a * 13;
    bool chosen = (a == pet.action);
    cv->setTextColor(chosen ? RGB565_WHITE : RGB565_LIGHTGREY);
    cv->setCursor(10, y + 2);
    cv->print(chosen ? ">" : " ");
    cv->print(ACTION_NAMES[a]);
    float v = brain.q[curState][a];
    int w = (int)(fabsf(v) / span * half);
    cv->drawFastVLine(mid, y, 12, RGB565_DARKGREY);
    if (v >= 0) cv->fillRect(mid, y + 2, w, 8, rgb(80, 200, 140));
    else cv->fillRect(mid - w, y + 2, w, 8, rgb(220, 80, 80));
  }

  int hy = y0 + NUM_ACTIONS * 13 + 6;
  cv->setTextColor(RGB565_WHITE);
  cv->setCursor(10, hy);
  cv->print("Habits so far:");
  float sc;
  Action h;
  h = brain.habit(habitTilt, &sc);       drawHabit(hy + 12, "When the room tilts:", h, sc);
  h = brain.habit(habitHungryFood, &sc); drawHabit(hy + 24, "Hungry, food out:", h, sc);
  h = brain.habit(habitTired, &sc);      drawHabit(hy + 36, "When tired:", h, sc);
  h = brain.habit(habitFloat, &sc);      drawHabit(hy + 48, "In zero-g:", h, sc);

  // Buttons
  cv->fillRoundRect(8, BTN_Y, 146, 38, 8, confirmReset ? rgb(220, 60, 60) : rgb(110, 40, 40));
  cv->setTextSize(2);
  cv->setTextColor(RGB565_WHITE);
  cv->setCursor(confirmReset ? 22 : 30, BTN_Y + 12);
  cv->print(confirmReset ? "TAP AGAIN" : "NEW BRAIN");
  cv->fillRoundRect(166, BTN_Y, 146, 38, 8, rgb(50, 70, 120));
  cv->setCursor(184, BTN_Y + 12);
  cv->print("TILT SETUP");
}

// ---- Tilt calibration ----
void drawCal() {
  cv->fillScreen(rgb(16, 14, 26));
  textAt(F_TITLE, 10, 36, "Tilt setup", rgb(170, 176, 255));
  cv->setTextColor(RGB565_WHITE);
  cv->setCursor(10, 60);
  if (!imuOk) {
    cv->print("Motion sensor not\nfound. Tap SKIP.");
  } else if (calStep == 0) {
    cv->println("Step 1 of 2");
    cv->setTextSize(2);
    cv->setCursor(10, 90);
    cv->print("Hold the board\nupright, like a\npicture frame,\nwith these words\nthe right way up.\n\nThen tap OK.");
  } else {
    cv->println("Step 2 of 2");
    cv->setCursor(10, 90);
    cv->print("Now tip it so the\nLEFT edge points\nat the ground.\n\nThen tap OK.");
  }
  cv->setTextSize(1);
  cv->setTextColor(RGB565_LIGHTGREY);
  cv->setCursor(10, 300);
  cv->printf("sensor x %.2f  y %.2f  z %.2f", rawA[0], rawA[1], rawA[2]);
  cv->setTextColor(rgb(255, 150, 120));
  cv->setCursor(10, 320);
  cv->print(calMsg);

  cv->fillRoundRect(20, 400, 130, 50, 10, rgb(50, 150, 80));
  cv->fillRoundRect(170, 400, 130, 50, 10, rgb(90, 90, 110));
  cv->setTextSize(2);
  cv->setTextColor(RGB565_WHITE);
  cv->setCursor(73, 418);
  cv->print("OK");
  cv->setCursor(211, 418);
  cv->print("SKIP");
}

void calTap(int x, int y) {
  if (y < 400 || y > 450) return;
  if (x >= 170 || !imuOk) {   // SKIP
    screen = SCR_ROOM;
    return;
  }
  if (calStep == 0) {
    int ax = fabsf(rawA[1]) > fabsf(rawA[0]) ? 1 : 0;
    if (fabsf(rawA[ax]) < 0.6f) { calMsg = "Didn't feel it. Hold it upright and try again."; return; }
    axisY = ax;
    signY = rawA[ax] > 0 ? 1 : -1;
    calStep = 1;
    calMsg = "";
  } else {
    int other = 1 - axisY;
    if (fabsf(rawA[other]) < 0.6f) { calMsg = "Didn't feel it. Tip the LEFT edge down more."; return; }
    signX = rawA[other] > 0 ? -1 : 1;   // left edge down = gravity points screen-left
    prefs.putUChar("axY", axisY);
    prefs.putChar("sgX", signX);
    prefs.putChar("sgY", signY);
    prefs.putBool("cal", true);
    calMsg = "";
    screen = SCR_ROOM;
  }
}

// ============================================================ touch

// ---- MORE menu ----
const int MENU_X[2] = {10, 164}, MENU_Y0 = HUD_H + 50, MENU_BW = 146, MENU_BH = 74, MENU_GAP = 8;
const int MENU_ROWS = 4;

void menuButton(int col, int row, const char *title, const char *value, uint16_t color) {
  int x = MENU_X[col], y = MENU_Y0 + row * (MENU_BH + MENU_GAP);
  cv->fillRoundRect(x, y, MENU_BW, MENU_BH, 12, color);
  textAt(F_BOLD, x + 12, y + 28, title, RGB565_WHITE);
  cv->setTextSize(1);
  cv->setTextColor(rgb(235, 235, 245));
  cv->setCursor(x + 12, y + 50);
  cv->print(value);
}

void drawMenu() {
  cv->fillRect(0, HUD_H, SCREEN_W, TOOLBAR_Y - HUD_H, rgb(16, 14, 26));
  textAt(F_TITLE, 12, HUD_H + 32, "More", rgb(170, 176, 255));
  char buf[32];
  menuButton(0, 0, "Brain", "what Bloop has learned", rgb(84, 84, 168));
  menuButton(1, 0, "Lights", lightsOff ? "off: Bloop gets sleepy" : "on", lightsOff ? rgb(44, 44, 76) : rgb(196, 152, 44));
  snprintf(buf, sizeof(buf), "now: %s", TOY_NAMES[toy]);
  menuButton(0, 1, "Toy", buf, rgb(206, 72, 84));
  menuButton(1, 1, toyOut ? "Put toy away" : "Bring toy", toyOut ? "take it out of the room" : "back into the room", rgb(150, 80, 120));
  menuButton(0, 2, "Clean up", "remove all snacks", rgb(64, 128, 92));
  menuButton(1, 2, "Bedtime", pet.asleep ? "Bloop is asleep" : "send Bloop to bed", rgb(96, 72, 150));
  menuButton(0, 3, "Tilt setup", "which way is down", rgb(52, 76, 128));
  menuButton(1, 3, "Back", "to the room", rgb(70, 68, 90));
}

void menuTap(int x, int y) {
  int col = x < 160 ? 0 : 1;
  if (y < MENU_Y0) return;
  int row = (y - MENU_Y0) / (MENU_BH + MENU_GAP);
  if (row >= MENU_ROWS || (y - MENU_Y0) % (MENU_BH + MENU_GAP) > MENU_BH) return;
  switch (row * 2 + col) {
    case 0: screen = SCR_BRAIN; break;
    case 1: lightsOff = !lightsOff; prefs.putBool("dark", lightsOff); sendEvent(lightsOff ? "lights_off" : "lights_on", 0); break;
    case 2: {
      toy = (toy + 1) % NUM_TOYS;
      prefs.putUChar("toy", toy);
      float bx = ball.active ? ball.x : SCREEN_W / 2, by = ball.active ? ball.y : (ROOM_T + ROOM_B) / 2;
      initToy(bx, by);
      ball.active = toyOut;
      break;
    }
    case 3:
      if (toyOut) {
        toyOut = false;
        ball.active = false;
        if (grabCand == GRAB_BALL) grabCand = GRAB_NONE;
        prefs.putBool("toyOut", false);
      } else {
        tossBall();
      }
      break;
    case 4:
      for (int i = 0; i < MAX_FOOD; i++) food[i].active = false;
      break;
    case 5:
      startAction(ACT_SLEEP);
      sinceDecide = 0;
      screen = SCR_ROOM;
      break;
    case 6: calStep = 0; calMsg = ""; screen = SCR_CAL; break;
    case 7: screen = SCR_ROOM; break;
  }
}

void toolbarTap(int i) {
  confirmReset = false;
  toolFlash = i;
  toolFlashMs = millis();
  switch (i) {
    case 0: dropFood(); break;
    case 1: tossBall(); break;
    case 2:   // GOOD
      rewardAcc += 2.5f;
      pet.joy = clampf(pet.joy + 10, 0, 100);
      for (int k = 0; k < 6; k++)
        spawnParticle(PT_HEART, pet.body.x, pet.body.y - 10, frand(-70, 70), frand(-110, -30), 1.0f, rgb(255, 90, 140));
      decideNow = true;
      sinceDecide = 1;   // credit it to what Bloop is doing right now
      sendEvent("good", 2.5f);
      say(SAY_GOOD, true);
      break;
    case 3:   // NO
      rewardAcc -= 2.5f;
      pet.sadT = 1.2f;
      decideNow = true;
      sinceDecide = 1;
      sendEvent("no", -2.5f);
      say(SAY_NO, true);
      break;
    case 4:
      screen = (screen == SCR_ROOM) ? SCR_MENU : SCR_ROOM;
      break;
  }
}

void brainTap(int x, int y) {
  if (y < BTN_Y || y > BTN_Y + 38) return;
  if (x < 160) {
    if (!confirmReset) { confirmReset = true; return; }
    brain.clear();
    pet.falls = pet.treats = pet.kicks = 0;
    pet.age = 0;
    saveAll();
    confirmReset = false;
  } else {
    calStep = 0;
    calMsg = "";
    screen = SCR_CAL;
  }
}

void readTouch(float dt) {
  if (!touchOk) return;
  int16_t xs[1], ys[1];
  bool down = touch.getPoint(xs, ys, 1) > 0;
  float x = clampf(xs[0], 0, SCREEN_W - 1), y = clampf(ys[0], 0, SCREEN_H - 1);

  if (down && !fingerDown) {   // finger just landed
    fingerDown = true;
    fx = fStartX = x; fy = fStartY = y;
    fvx = fvy = 0;
    fStartMs = millis();
    grabCand = GRAB_NONE;
    grabbing = false;

    if (screen == SCR_CAL) { calTap(x, y); return; }
    if (y >= TOOLBAR_Y) { toolbarTap((int)(x / 64)); return; }
    if (screen == SCR_BRAIN) { brainTap(x, y); return; }
    if (screen == SCR_MENU) { menuTap(x, y); return; }

    lastTouchMs = millis();
    int f = -1;
    float fd = 1e9;
    for (int i = 0; i < MAX_FOOD; i++) {
      if (!food[i].active) continue;
      float d = vlen(v2(x - food[i].x, y - food[i].y));
      if (d < food[i].r + 16 && d < fd) { fd = d; f = i; }
    }
    bool onBed = fabsf(x - bed.x) < bed.hw + 8 && fabsf(y - bed.y) < bed.hh + 8;
    if (vlen(v2(x - pet.body.x, y - pet.body.y)) < pet.body.r + 14) grabCand = GRAB_PET;
    else if (ball.active && vlen(v2(x - ball.x, y - ball.y)) < ball.r + 14) grabCand = GRAB_BALL;
    else if (f >= 0) { grabCand = GRAB_FOOD; grabFood = f; }
    else if (onBed) grabCand = GRAB_BED;
    else {
      callPoint = v2(x, y);
      spawnParticle(PT_DUST, x, y, 0, 0, 0.4f, rgb(255, 255, 200));
    }
    return;
  }

  if (down && fingerDown) {    // finger moving
    float k = clampf(dt * 15, 0, 1);
    fvx += ((x - fx) / fmaxf(dt, 0.001f) - fvx) * k;
    fvy += ((y - fy) / fmaxf(dt, 0.001f) - fvy) * k;
    fx = x; fy = y;
    if (screen != SCR_ROOM) return;
    lastTouchMs = millis();
    float moved = vlen(v2(x - fStartX, y - fStartY));
    if (grabCand != GRAB_NONE && moved > 10) {
      grabbing = true;
      petting = false;
      if (grabCand == GRAB_PET) { pet.held = true; pet.asleep = false; }
    }
    if (grabCand == GRAB_PET && !grabbing && millis() - fStartMs > 150) {
      if (!petting) say(SAY_PET, true);
      petting = true;
    }
    return;
  }

  if (!down && fingerDown) {   // finger lifted: throw whatever was held
    fingerDown = false;
    float sp = vlen(v2(fvx, fvy));
    if (sp > 1500) { fvx *= 1500 / sp; fvy *= 1500 / sp; }
    if (grabbing && grabCand == GRAB_PET) {
      pet.held = false;
      pet.body.vx = fvx; pet.body.vy = fvy;
      decideNow = true;
    }
    if (grabbing && grabCand == GRAB_BALL) { ball.vx = fvx; ball.vy = fvy; }
    if (grabbing && grabCand == GRAB_BED) { bed.vx = fvx * 0.6f; bed.vy = fvy * 0.6f; }   // it's heavy
    if (grabbing && grabCand == GRAB_FOOD && grabFood >= 0 && food[grabFood].active) {
      food[grabFood].vx = fvx;
      food[grabFood].vy = fvy;
    }
    grabbing = false;
    petting = false;
    grabCand = GRAB_NONE;
  }
}

// Commands from the website: feed, ball, good, no, brain (send the table now).
void readCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      cmdBuf[cmdLen] = 0;
      if (!strcmp(cmdBuf, "feed")) toolbarTap(0);
      else if (!strcmp(cmdBuf, "ball") || !strcmp(cmdBuf, "toy")) toolbarTap(1);
      else if (!strcmp(cmdBuf, "lights")) { lightsOff = !lightsOff; prefs.putBool("dark", lightsOff); }
      else if (!strcmp(cmdBuf, "good")) toolbarTap(2);
      else if (!strcmp(cmdBuf, "no")) toolbarTap(3);
      else if (!strcmp(cmdBuf, "brain")) sendBrain();
      cmdLen = 0;
    } else if (cmdLen < (int)sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

// ============================================================ setup & loop

// Startup report, for the website's "Board log" and the Serial Monitor.
void sendBootReport() {
  Serial.printf("[boot] Bloop's Room | reset: %s%s | PSRAM %s (%u KB free) | heap %u KB free\n",
                resetReasonName(boardSt.reason), boardSt.coldRestart ? " (after cold-start restart)" : "",
                boardSt.psram ? "found" : "NOT FOUND", (unsigned)(ESP.getFreePsram() / 1024),
                (unsigned)(ESP.getFreeHeap() / 1024));
  Serial.printf("[boot] IO expander %s | power chip %s | screen %s | touch %s | motion sensor %s\n",
                boardSt.expander ? "ok" : "MISSING", boardSt.pmu ? "ok" : "MISSING",
                screenOk ? "ok" : "FAILED", touchOk ? "ok" : "MISSING", imuOk ? "ok" : "MISSING");
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never stall the game when no computer is listening
  boardSt = boardPowerUp(expander, pmu);

  // SPI mode 0, like Waveshare's firmware. (GFX Library 1.6+ defaults the ST7796
  // to mode 3, which this board's screen ignores: the screen stays black.)
  bool lcdOk = lcd->begin(LCD_SPI_HZ, SPI_MODE0);
  screenOk = lcdOk && cv->begin(GFX_SKIP_OUTPUT_BEGIN);
  if (lcdOk && !screenOk) {
    // No framebuffer (usually PSRAM missing): say so on the screen directly.
    lcd->fillScreen(RGB565_BLACK);
    lcd->setTextColor(RGB565_RED);
    lcd->setTextSize(2);
    lcd->setCursor(10, 40);
    lcd->println("Bloop can't start:");
    lcd->println("no memory for the");
    lcd->println("screen buffer.");
    lcd->println();
    lcd->println(boardSt.psram ? "PSRAM found but full" : "PSRAM not found.");
    lcd->println("Check Tools > PSRAM");
    lcd->println("= OPI PSRAM.");
    boardBacklight(true);
  }

  touchOk = touch.begin(Wire, FT6X36_SLAVE_ADDRESS);
  imuOk = imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS);
  if (imuOk) {
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_250Hz, SensorQMI8658::LPF_MODE_0);
    imu.enableAccelerometer();
  }
  sendBootReport();

  prefs.begin("bloop", false);
  loadAll();
  if (!prefs.getBool("cal", false)) screen = SCR_CAL;

  bodyInit(pet.body, BODY_PET, SCREEN_W / 2, ROOM_B - 40, 22, 0.22f, 7);
  bodyInit(bed, BODY_BED, ROOM_R - 50, ROOM_B - 30, 26, 0.05f, 8);
  bed.isBox = true; bed.hw = 38; bed.hh = 25; bed.stick = 0.35f;
  initToy(ROOM_L + 60, ROOM_B - 30);
  ball.active = toyOut;
  for (int i = 0; i < MAX_FOOD; i++) food[i].active = false;
  for (int i = 0; i < MAX_PARTICLES; i++) parts[i].active = false;
  pet.action = ACT_SIT;
  pet.stamina = 100;
  pet.angle = PI / 2;
  pet.look = v2(1, 0);
  pet.blinkT = 3;

  visitorBegin();
  lastFrameMs = millis();
}

uint32_t bootReportAt = 0;

void loop() {
  uint32_t now = millis();
  // Repeat the startup report now and then, so a computer that connects late still sees it.
  if (now - bootReportAt > (now < 30000 ? 3000u : 60000u)) { bootReportAt = now; sendBootReport(); }
  if (!screenOk) { delay(100); return; }
  float dt = clampf((now - lastFrameMs) / 1000.0f, 0.001f, 0.05f);
  lastFrameMs = now;

  readImu(dt);
  readTouch(dt);
  readCommands();

  if (screen == SCR_CAL) {
    drawCal();
    cv->flush();
    if (!backlightOn) { boardBacklight(true); backlightOn = true; }
    return;
  }

  updateWorld(dt);

  // Turn Bloop smoothly so its feet point at the floor.
  if (!floating) {
    float want = atan2f(gScreen.y, gScreen.x);
    float diff = want - pet.angle;
    while (diff > PI) diff -= 2 * PI;
    while (diff < -PI) diff += 2 * PI;
    pet.angle += diff * clampf(dt * 6, 0, 1);
  } else {
    pet.angle += dt * 0.6f;   // slowly tumbles in zero-g
  }
  if (pet.held) pet.angle += sinf(now / 90.0f) * 0.05f;  // wiggles when held
  if (pet.action == ACT_DANCE && !pet.asleep) pet.angle += sinf(pet.danceT * 9) * 0.04f;

  // The bed turns to stand on the floor too.
  if (!floating) {
    float want = atan2f(gScreen.y, gScreen.x);
    float diff = want - bedAngle;
    while (diff > PI) diff -= 2 * PI;
    while (diff < -PI) diff += 2 * PI;
    bedAngle += diff * clampf(dt * 5, 0, 1);
  } else {
    bedAngle += dt * 0.3f;
  }
  if (ball.active) toySpin += (ball.vx + ball.vy) * dt / (ball.r * 2.0f);

  if (screen == SCR_BRAIN) drawBrain();
  else if (screen == SCR_MENU) drawMenu();
  else drawRoom();
  drawHud();
  drawToolbar();
  cv->flush();
  if (!backlightOn) { boardBacklight(true); backlightOn = true; }
}
