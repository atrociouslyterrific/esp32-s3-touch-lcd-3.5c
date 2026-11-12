// visitor.h: Pip and Bloop visit each other.
//
// Pip is the e-paper pet on the other board (PipAI in the esp32-epaper-projects
// repo). Pip calls out over the radio (ESP-NOW) a few times a second. When this
// board hears Pip loud and clear, the two boards are side by side, so Pip hops
// into Bloop's room. Move the boards apart and Pip goes home again.
//
// While Pip is here, Bloop tells Pip so (and how Bloop is doing), and Bloop
// shows up in Pip's room on Pip's screen too.
//
// Pip also trains Bloop while it is here: see visitorCoach().
//
// Included from bloop_room.ino after its helpers (it uses pet, ball, bed, cv...).
#pragma once
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---- The radio message. Must match the copy in PipAI.ino. ----
struct __attribute__((packed)) PalMsg {
  uint8_t magic[4];   // "PAL1"
  uint8_t who;        // PAL_PIP or PAL_BLOOP
  uint8_t action;     // what the sender is doing (its own action number)
  uint8_t flags;      // PALF_* bits about the sender
  uint8_t hosting;    // Bloop: 1 while Pip is in Bloop's room
};
enum : uint8_t { PAL_PIP = 1, PAL_BLOOP = 2 };
enum : uint8_t { PALF_SAD = 1, PALF_ASLEEP = 2 };
// Pip's actions, in the order of PipAction in pip_world.h
enum : uint8_t { PIP_IDLE, PIP_NAP, PIP_EAT, PIP_EXPLORE, PIP_PLAY, PIP_SING, PIP_HIDE,
                 PIP_WARM_UP, PIP_COOL_DOWN, PIP_CALL, PIP_DANCE, PIP_WINDOW };

// ---- How close is "together"? ----
// Signal strength in dBm: closer to 0 = nearer. Boards touching read about -25
// to -45; a metre apart about -50 to -60. Pip comes in above NEAR and leaves
// below FAR (the gap stops it flickering in and out at the edge).
const int PIP_NEAR_DBM = -52;
const int PIP_FAR_DBM = -64;

struct Visitor {
  Body body;
  bool here;
  uint8_t action, flags;
  float hopCool, heartCool, bubbleT;
  const char *bubble;
  uint32_t seenDecisions;   // Bloop's choice count when Pip last looked
  float coachT;             // counts down to Pip judging Bloop's newest choice
  float coachCool;          // Pip doesn't comment on everything
  uint32_t lessons;         // how many times Pip has praised or scolded this visit
};
Visitor pip;

// Written by the radio (in the Wi-Fi task), read by the game loop.
volatile uint32_t palHeardMs = 0;
volatile int8_t palLastRssi = -127;
volatile uint8_t palAction = 0, palFlags = 0;
volatile bool palFresh = false;

bool palOk = false;
float pipRssi = -127;          // smoothed signal strength
float pipNearT = 0, pipFarT = 0, palHopT = 0, palSendT = 0;
uint8_t palChan = 1;
const uint8_t PAL_EVERYONE[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void palOnRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(PalMsg) || memcmp(data, "PAL1", 4) != 0 || data[4] != PAL_PIP) return;
  palAction = data[5];
  palFlags = data[6];
  palLastRssi = info->rx_ctrl ? info->rx_ctrl->rssi : -127;
  palHeardMs = millis();
  palFresh = true;
}

void palSend(bool hosting) {
  uint8_t flags = (pet.asleep ? PALF_ASLEEP : 0) | ((pet.joy < 25 || pet.sadT > 0 || pet.hurtT > 0) ? PALF_SAD : 0);
  PalMsg m = {{'P', 'A', 'L', '1'}, PAL_BLOOP, (uint8_t)pet.action, flags, (uint8_t)(hosting ? 1 : 0)};
  esp_now_send(PAL_EVERYONE, (const uint8_t *)&m, sizeof(m));
}

// Switch the radio on (it never joins a network, it only listens for Pip).
void visitorBegin() {
  pip.here = false;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (esp_now_init() != ESP_OK) return;
  esp_now_register_recv_cb(palOnRecv);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, PAL_EVERYONE, 6);
  peer.channel = 0;   // whatever channel the radio is on
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  palOk = esp_now_add_peer(&peer) == ESP_OK;
}

void visitorPuff() {
  for (int k = 0; k < 8; k++) {
    spawnParticle(PT_STAR, pip.body.x + frand(-18, 18), pip.body.y + frand(-18, 18),
                  frand(-70, 70), frand(-70, 70), 0.7f, RGB565_YELLOW);
    if (k < 4) spawnParticle(PT_DUST, pip.body.x, pip.body.y, frand(-90, 90), frand(-90, 90), 0.5f, rgb(230, 228, 235));
  }
}

void visitorArrive() {
  Vec2 p = ceilingSpot();
  bodyInit(pip.body, BODY_BALL, p.x, p.y, 17, 0.35f, 6);
  pip.body.stick = 0.2f;
  pip.here = true;
  pip.hopCool = 1.5f;
  pip.heartCool = 2.0f;
  pip.bubble = "Hi Bloop!";
  pip.bubbleT = 3.0f;
  pip.seenDecisions = brain.decisions;
  pip.coachT = -1;
  pip.coachCool = 5.0f;   // say hello first, teach after
  pip.lessons = 0;
  visitorPuff();
  pet.joy = clampf(pet.joy + 15, 0, 100);
  if (!pet.asleep) { snprintf(sayText, sizeof(sayText), "Pip! You came!"); sayT = 3.5f; }
  sendEvent("pip_in", pipRssi);
}

void visitorLeave() {
  visitorPuff();
  pip.here = false;
  for (int i = 0; i < 3; i++) palSend(false);
  if (!pet.asleep) { snprintf(sayText, sizeof(sayText), "Bye, Pip!"); sayT = 3.0f; }
  sendEvent("pip_out", pipRssi);
}

// Listen for Pip and decide whether it is close enough to be in the room.
void visitorRadio(float dt) {
  if (!palOk) return;
  uint32_t heard = palHeardMs;
  uint32_t now = millis();
  uint32_t quiet = heard ? now - heard : 0xFFFFFFFFu;

  if (palFresh) {
    palFresh = false;
    float r = palLastRssi;
    pipRssi = (pipRssi < -120) ? r : pipRssi + (r - pipRssi) * 0.25f;
    pip.action = palAction;
    pip.flags = palFlags;
  }

  // Pip's radio sits on whatever channel its home Wi-Fi uses, and we don't know
  // which. Until we hear Pip, listen on each channel in turn.
  bool heardLately = quiet < 2500;
  if (!heardLately) {
    pipRssi = -127;
    palHopT += dt;
    if (palHopT > 0.6f) {
      palHopT = 0;
      palChan = palChan % 13 + 1;
      esp_wifi_set_channel(palChan, WIFI_SECOND_CHAN_NONE);
    }
  }

  pipNearT = (heardLately && pipRssi >= PIP_NEAR_DBM) ? pipNearT + dt : 0;
  pipFarT = (heardLately && pipRssi <= PIP_FAR_DBM) ? pipFarT + dt : 0;
  if (!pip.here && pipNearT > 1.5f) visitorArrive();
  else if (pip.here && (quiet > 6000 || pipFarT > 4.0f)) visitorLeave();

  // Tell Pip it is in our room. (Pip's radio naps to save battery, so say it often.)
  if (pip.here) {
    palSendT += dt;
    if (palSendT > 0.1f) { palSendT = 0; palSend(true); }
  }
}

// ---- Pip trains Bloop ----
// Pip is the older, wiser pet (its brain was trained for days on GitHub), so
// while it visits it watches what Bloop chooses to do and tells it "Good Bloop!"
// or "No, Bloop!", just like your GOOD and NO buttons but a little gentler.
// Bloop's brain learns from Pip's opinion the same way it learns from yours, so
// time spent with Pip teaches a young Bloop sensible habits much faster.
//
// What Pip thinks is sensible: hold on, curl up or hide when the room tips; eat
// when hungry (and beg if there's no food); sleep when tired; play with the toy
// when all is well. Silly: dancing or napping while the room tips, ignoring food
// when hungry, eating or begging when full. Anything else, Pip keeps quiet.
// Returns +1 (good), -1 (no) or 0 (no opinion).
int pipOpinion(const Senses &s, Action a) {
  if (s.room == ROOM_TILTING) {
    if (a == ACT_GRIP || a == ACT_CURL || a == ACT_HIDE) return 1;
    if (a == ACT_DANCE || a == ACT_JUMP || a == ACT_SLEEP || a == ACT_WINDOW) return -1;
    return 0;
  }
  if (s.room == ROOM_FLOATING) return 0;   // zero-g: Pip is as confused as Bloop
  if (s.hungry) {
    if (s.foodOut) return a == ACT_EAT ? 1 : -1;
    return a == ACT_BEG ? 1 : 0;
  }
  if (a == ACT_EAT || a == ACT_BEG) return -1;   // not hungry: greedy!
  if (s.tired) return a == ACT_SLEEP ? 1 : ((a == ACT_DANCE || a == ACT_JUMP) ? -1 : 0);
  if (s.ballNear && a == ACT_BALL) return 1;
  return 0;
}

const float PIP_PRAISE = 1.5f;   // your GOOD / NO buttons are worth 2.5

void visitorCoach(float dt) {
  pip.coachCool -= dt;
  if (brain.decisions != pip.seenDecisions) {   // Bloop just chose something new
    pip.seenDecisions = brain.decisions;
    pip.coachT = 0.7f;                          // watch for a moment, then judge
  }
  if (pip.coachT < 0) return;
  pip.coachT -= dt;
  if (pip.coachT >= 0) return;
  // (coachT is now below zero, so each choice is judged once.)
  if (pip.coachCool > 0 || (pip.flags & PALF_ASLEEP) || pet.asleep || pet.held) return;
  int verdict = pipOpinion(sense(), pet.action);
  if (verdict == 0) return;

  pip.coachCool = frand(3.0f, 6.0f);
  pip.lessons++;
  rewardAcc += verdict * PIP_PRAISE;   // credited to what Bloop is doing right now
  if (verdict > 0) {
    pip.bubble = "Good Bloop!";
    pet.joy = clampf(pet.joy + 4, 0, 100);
    for (int k = 0; k < 3; k++)
      spawnParticle(PT_HEART, pet.body.x, pet.body.y - 10, frand(-50, 50), frand(-90, -30), 1.0f, rgb(255, 90, 140));
    pip.hopCool = 0;                   // a happy little hop
  } else {
    pip.bubble = "No, Bloop!";
    pet.sadT = 0.8f;
  }
  pip.bubbleT = 2.0f;
  sendEvent(verdict > 0 ? "pip_good" : "pip_no", verdict * PIP_PRAISE);
}

// Once a frame: the radio, and what Pip gets up to in the room.
void visitorUpdate(float dt) {
  visitorRadio(dt);
  if (!pip.here) return;
  Body &b = pip.body;
  pip.hopCool -= dt;
  pip.heartCool -= dt;
  pip.bubbleT -= dt;
  visitorCoach(dt);

  bool asleep = pip.flags & PALF_ASLEEP;
  bool lively = pip.action == PIP_PLAY || pip.action == PIP_DANCE || pip.action == PIP_SING;
  float dx = pet.body.x - b.x, dy = pet.body.y - b.y;
  float dist = sqrtf(dx * dx + dy * dy);

  // Pip gets about by hopping: over to Bloop, then on the spot beside it.
  uint8_t fw = floorWall(gScreen);
  if (!floating && !asleep && (b.touching & fw) && pip.hopCool <= 0) {
    Vec2 n = wallNormal(fw), t = floorTangent(fw);
    bool farOff = dist > 80;
    float dir = farOff ? ((dx * t.x + dy * t.y) > 0 ? 1.0f : -1.0f) : (frand(0, 1) < 0.5f ? -1.0f : 1.0f);
    float up = lively ? 300 : 210, along = farOff ? 120 : 45;
    b.vx += n.x * up + t.x * dir * along;
    b.vy += n.y * up + t.y * dir * along;
    pip.hopCool = lively ? frand(0.5f, 1.0f) : frand(1.4f, 3.2f);
  }

  // Bloop likes the company.
  pet.joy = clampf(pet.joy + 0.25f * dt, 0, 100);
  if (dist < 75 && !asleep && !pet.asleep && pip.heartCool <= 0) {
    pip.heartCool = frand(2.5f, 5.0f);
    pet.joy = clampf(pet.joy + 2, 0, 100);
    spawnParticle(PT_HEART, (b.x + pet.body.x) / 2, (b.y + pet.body.y) / 2 - 22, frand(-15, 15), -55, 1.0f, rgb(255, 90, 140));
  }
  if (asleep && frand(0, 1) < dt * 0.7f)
    spawnParticle(PT_ZZZ, b.x + 12, b.y - 22, 8, -18, 1.6f, rgb(200, 210, 255));
  if ((pip.action == PIP_SING || pip.action == PIP_DANCE) && !asleep && frand(0, 1) < dt * 1.2f)
    spawnParticle(PT_NOTE, b.x + frand(-20, 20), b.y - 26, frand(-15, 15), -40, 1.1f, rgb(250, 250, 255));
}

// One physics step for Pip: fall, then bump into everything else.
void visitorStep(Vec2 g, float h) {
  if (!pip.here) return;
  Body &b = pip.body;
  stepBody(b, g, h, floating);
  if (!pet.held && !pet.gripping) collide(pet.body, b, 3.0f, 2.0f);
  if (ball.active) collide(b, ball, 2.0f, 1.0f);
  collideBox(b, bed, 2.0f, 4.0f);
  for (int i = 0; i < MAX_FOOD; i++)
    if (food[i].active) collide(b, food[i], 2.0f, 0.3f);
}

void visitorShake() {
  if (!pip.here) return;
  pip.body.vx += frand(-420, 420);
  pip.body.vy += frand(-420, 420);
  pip.bubble = "Whoa!";
  pip.bubbleT = 1.5f;
}

// Pip looks like it does on its own e-paper screen: a white blob with a thick
// black outline and a little tip on top. Its feet point at the floor, like Bloop's.
void drawVisitor() {
  if (!pip.here) return;
  Body &b = pip.body;
  Vec2 d = downDir();
  Vec2 r = v2(d.y, -d.x);
  auto P = [&](float lx, float ly) { return v2(b.x + lx * r.x + ly * d.x, b.y + lx * r.y + ly * d.y); };
  auto line = [&](float x0, float y0, float x1, float y1, uint16_t c) {
    Vec2 a = P(x0, y0), e = P(x1, y1), a2 = P(x0, y0 + 1), e2 = P(x1, y1 + 1);
    cv->drawLine(a.x, a.y, e.x, e.y, c);
    cv->drawLine(a2.x, a2.y, e2.x, e2.y, c);
  };
  const uint16_t ink = rgb(30, 28, 40), paper = rgb(250, 248, 240);
  bool asleep = pip.flags & PALF_ASLEEP, sad = pip.flags & PALF_SAD;

  Vec2 t0 = P(-5, -b.r + 4), t1 = P(5, -b.r + 4), t2 = P(1, -b.r - 6);
  cv->fillTriangle(t0.x, t0.y, t1.x, t1.y, t2.x, t2.y, ink);
  cv->fillCircle(b.x, b.y, b.r, ink);
  cv->fillCircle(b.x, b.y, b.r - 3, paper);
  line(-10, -6, -7, -10, rgb(205, 200, 195));   // shine

  // It looks toward Bloop.
  float look = ((pet.body.x - b.x) * r.x + (pet.body.y - b.y) * r.y) > 0 ? 2 : -2;
  for (int s = -1; s <= 1; s += 2) {
    float ex = s * 6 + look;
    if (asleep) { line(ex - 3, -2, ex + 3, -2, ink); continue; }
    Vec2 e = P(ex, -2), hl = P(ex + 1, -3);
    cv->fillCircle(e.x, e.y, 3, ink);
    cv->drawPixel(hl.x, hl.y, paper);
  }
  if (asleep) {
    Vec2 m = P(look, 7);
    cv->fillCircle(m.x, m.y, 1, ink);
  } else if (pip.action == PIP_SING || pip.action == PIP_DANCE || pip.action == PIP_CALL) {
    Vec2 m = P(look, 7);
    cv->fillCircle(m.x, m.y, 3, ink);
  } else if (sad) {
    line(look - 5, 9, look - 2, 7, ink); line(look - 2, 7, look + 2, 7, ink); line(look + 2, 7, look + 5, 9, ink);
  } else {
    line(look - 5, 5, look - 2, 7, ink); line(look - 2, 7, look + 2, 7, ink); line(look + 2, 7, look + 5, 5, ink);
  }

  if (pip.bubbleT > 0 && pip.bubble) {
    int w = (int)strlen(pip.bubble) * 12 + 12, h = 24;
    int bx = (int)clampf(b.x - w / 2, ROOM_L + 2, ROOM_R - w - 2);
    int by = (int)(b.y - b.r - 14 - h);
    if (by < ROOM_T + 2) by = (int)clampf(b.y + b.r + 10, ROOM_T + 2, ROOM_B - h - 2);
    cv->fillRoundRect(bx, by, w, h, 8, RGB565_WHITE);
    cv->drawRoundRect(bx, by, w, h, 8, ink);
    cv->setFont((const GFXfont *)NULL);
    cv->setTextSize(2);
    cv->setTextColor(ink);
    cv->setCursor(bx + 6, by + 5);
    cv->print(pip.bubble);
    cv->setTextSize(1);
  }
}
