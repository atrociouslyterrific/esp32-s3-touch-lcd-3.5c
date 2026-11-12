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
// And they talk. Every line Bloop says (from its language model, voice.h) goes to
// Pip in a "PALS" message; Pip answers each one with a line from its own voice
// model. Bloop sorts Pip's answer into a topic (food, sleep, play, weather, love,
// a question, anything else) and replies about it, at most twice per chat so the
// two don't talk forever. Every 25-40 s Bloop starts a new chat.
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
  uint8_t hosting;    // Bloop: 0 = apart, 1 = both pets are in Bloop's room, 2 = both are in Pip's room
};
enum : uint8_t { PAL_PIP = 1, PAL_BLOOP = 2 };

// A spoken line. Must match the copy in PipAI.ino.
struct __attribute__((packed)) PalSay {
  uint8_t magic[4];   // "PALS"
  uint8_t who;        // PAL_PIP or PAL_BLOOP
  uint8_t seq;        // the sender's line number (goes up by 1 each line, wraps at 255)
  uint8_t replyTo;    // the other pet's line this answers (0 = starts a new topic)
  char text[58];      // what was said, ending in a 0 byte
};
enum : uint8_t { PALF_SAD = 1, PALF_ASLEEP = 2, PALF_SCARED = 4, PALF_GRUMPY = 8, PALF_HAPPY = 16 };

// Something that happens to Pip in Bloop's room ("PALE"). Bloop's board tells Pip's
// board, and Pip's real feelings change (how much depends on its personality), so
// its mood here, on its own screen and in what it says all follow.
// Must match the copy in PipAI.ino.
enum : uint8_t { ROOMEV_SHAKE = 1, ROOMEV_BUMP, ROOMEV_TILT, ROOMEV_FLOAT, ROOMEV_DARK, ROOMEV_LIGHT,
                 ROOMEV_PLAY, ROOMEV_CUDDLE, NUM_ROOMEV };
struct __attribute__((packed)) PalEvent {
  uint8_t magic[4];   // "PALE"
  uint8_t who;        // PAL_BLOOP
  uint8_t seq;        // a new number for each event
  uint8_t kind;       // ROOMEV_*
  uint8_t strength;   // 0..100
};
// The face Pip pulls right away, before its own board has caught up.
enum : uint8_t { FEEL_NONE, FEEL_SCARED, FEEL_OUCH, FEEL_WHEE, FEEL_SLEEPY };
// Pip's actions, in the order of PipAction in pip_world.h
enum : uint8_t { PIP_IDLE, PIP_NAP, PIP_EAT, PIP_EXPLORE, PIP_PLAY, PIP_SING, PIP_HIDE,
                 PIP_WARM_UP, PIP_COOL_DOWN, PIP_CALL, PIP_DANCE, PIP_WINDOW };

// ---- How close is "together"? ----
// Signal strength in dBm: closer to 0 = nearer. Boards touching read about -25
// to -45; a metre apart about -50 to -60. Pip comes in above NEAR and leaves
// below FAR (the gap stops it flickering in and out at the edge).
const int PIP_NEAR_DBM = -52;
const int PIP_FAR_DBM = -62;

struct Visitor {
  Body body;
  bool here;
  uint8_t action, flags;
  float hopCool, heartCool, bubbleT;
  const char *bubble;
  uint8_t feel;             // FEEL_*: its reaction to what just happened in the room
  float enterT, exitT;      // seconds left of hopping in / out through the door (0 = not)
  float walkSecs;           // how long this walk takes in all
  Vec2 pathFrom, pathTo;    // where the walk starts and ends
  float feelT;              // seconds that reaction lasts
  uint32_t seenDecisions;   // Bloop's choice count when Pip last looked
  float coachT;             // counts down to Pip judging Bloop's newest choice
  float coachCool;          // Pip doesn't comment on everything
  uint32_t lessons;         // how many times Pip has praised or scolded this visit
};
Visitor pip;

uint32_t pipLessonsTotal = 0;   // every "Good Bloop!" / "No, Bloop!" Pip has ever given (saved on the board)
uint32_t pipLessonsSaved = 0;   // the count that is in the board's memory right now
float pipLessonsSaveT = 0;

// Save the lesson count if it has changed. (Not after every single lesson: the
// board's memory wears out if it is written every few seconds.)
void visitorSaveLessons() {
  if (pipLessonsTotal == pipLessonsSaved) return;
  prefs.putUInt("pipLessons", pipLessonsTotal);
  pipLessonsSaved = pipLessonsTotal;
}

// While the boards are together the two pets share ONE room at a time: a pet that
// leaves its own room turns up in the other's. They start in Bloop's room, then
// take turns: Bloop goes over to Pip's (and this screen shows its room empty),
// later they both come back here.
enum : uint8_t { PLACE_BLOOPS = 1, PLACE_PIPS = 2 };
uint8_t palPlace = PLACE_BLOOPS;
float placeT = 0;   // seconds until they move to the other room
bool visitorBloopAway() { return pip.here && palPlace == PLACE_PIPS; }

// Written by the radio (in the Wi-Fi task), read by the game loop.
volatile uint32_t palHeardMs = 0;
volatile int8_t palLastRssi = -127;
volatile uint8_t palAction = 0, palFlags = 0;
volatile bool palFresh = false;

// Pip's latest line, written by the radio task
volatile bool palSayFresh = false;
volatile uint8_t palSayReplyTo = 0;
char palSayBuf[58];

bool palOk = false;
float pipRssi = -127;          // smoothed signal strength
float pipNearT = 0, pipFarT = 0, palHopT = 0, palSendT = 0;
uint8_t palChan = 1;
const uint8_t PAL_EVERYONE[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Pip's verdict on something Bloop did (see "Pip trains Bloop"), written by the radio task
volatile bool palVerdictFresh = false;
volatile uint8_t palVerdictSeq = 0, palVerdictStrength = 0;
volatile int8_t palVerdict = 0;
char palVerdictText[24];

void palOnRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len >= 8 && memcmp(data, "PALV", 4) == 0 && data[4] == PAL_PIP) {
    if (palVerdictFresh) return;   // the game loop hasn't read the last one yet (Pip sends each 3 times)
    palVerdictSeq = data[5];
    palVerdict = (int8_t)data[6];
    palVerdictStrength = data[7];
    int n = len - 8;
    if (n > 23) n = 23;
    if (n < 0) n = 0;
    memcpy(palVerdictText, data + 8, n);
    palVerdictText[n] = 0;
    palVerdictFresh = true;
    return;
  }
  if (len >= 8 && memcmp(data, "PALS", 4) == 0 && data[4] == PAL_PIP) {
    int n = len - 7;
    if (n > 57) n = 57;
    memcpy(palSayBuf, data + 7, n);
    palSayBuf[n] = 0;
    palSayReplyTo = data[6];
    palSayFresh = true;
    return;
  }
  if (len < (int)sizeof(PalMsg) || memcmp(data, "PAL1", 4) != 0 || data[4] != PAL_PIP) return;
  palAction = data[5];
  palFlags = data[6];
  palLastRssi = info->rx_ctrl ? info->rx_ctrl->rssi : -127;
  palHeardMs = millis();
  palFresh = true;
}

void palSend(bool hosting) {
  uint8_t flags = (pet.asleep ? PALF_ASLEEP : 0) | ((pet.joy < 25 || pet.sadT > 0 || pet.hurtT > 0) ? PALF_SAD : 0);
  PalMsg m = {{'P', 'A', 'L', '1'}, PAL_BLOOP, (uint8_t)pet.action, flags, (uint8_t)(hosting ? palPlace : 0)};
  esp_now_send(PAL_EVERYONE, (const uint8_t *)&m, sizeof(m));
}

// ---- Talking ----
uint8_t palSaySeq = 0;        // our latest line number
uint32_t palSaidCount = 0;    // sayCount when we last sent
int chatReplies = 0;          // Bloop's replies in the current chat
float chatT = 0;              // until Bloop starts a new chat
float replyT = -1;            // until Bloop answers Pip
char pipLine[58] = "";

void palSendSay(const char *text, uint8_t replyTo) {
  PalSay m = {};
  memcpy(m.magic, "PALS", 4);
  m.who = PAL_BLOOP;
  m.seq = ++palSaySeq;
  if (m.seq == 0) m.seq = ++palSaySeq;   // 0 means "no line"
  m.replyTo = replyTo;
  strncpy(m.text, text, sizeof(m.text) - 1);
  for (int i = 0; i < 2; i++) esp_now_send(PAL_EVERYONE, (const uint8_t *)&m, 7 + strlen(m.text) + 1);
}

// Sort what Pip said into a topic Bloop's voice knows how to answer.
uint8_t pipTopicOf(const char *line) {
  char low[58];
  int i = 0;
  for (; line[i] && i < 57; i++) low[i] = tolower((unsigned char)line[i]);
  low[i] = 0;
  // Does a word start with w? ("eat" matches "eat" and "eating", not "great".)
  auto has = [&](const char *w) {
    for (const char *p = strstr(low, w); p; p = strstr(p + 1, w))
      if (p == low || !isalpha((unsigned char)p[-1])) return true;
    return false;
  };
  // Words from Pip's own voice count too (its "tummy", "pats", "chilly", "boing"...).
  if (has("food") || has("snack") || has("eat") || has("hungry") || has("yum") || has("crumb") ||
      has("tummy") || has("treat") || has("cookie") || has("berr") || has("munch")) return PT_FOOD_TALK;
  if (has("sleep") || has("nap") || has("tired") || has("yawn") || has("zzz") || has("bed") || has("dream") ||
      has("snore") || has("pooped") || has("comfy") || has("five more") || has("night")) return PT_SLEEP_TALK;
  if (has("play") || has("dance") || has("sing") || has("fun") || has("game") || has("jump") || has("la la") ||
      has("boing") || has("tag") || has("catch") || has("wiggle") || has("bored") || has("groovy") ||
      has("adventure") || has("explore")) return PT_PLAY_TALK;
  if (has("cold") || has("warm") || has("hot") || has("sun") || has("rain") || has("weather") || has("chilly") ||
      has("brr") || has("freez") || has("toasty") || has("melt") || has("heater") || has("blanket") ||
      has("sky") || has("cloud") || has("moon") || has("star") || has("breeze")) return PT_WEATHER_TALK;
  if (has("love") || has("friend") || has("like you") || has("hug") || has("<3") || has("favourite") ||
      has("pats") || has("miss you") || has("tickles") || has("best")) return PT_LOVE_TALK;
  if (has("?")) return PT_QUESTION;
  return PT_ANY_TALK;
}

void visitorTalk(float dt) {
  // Anything Bloop just said goes to Pip.
  if (sayCount != palSaidCount) {
    palSaidCount = sayCount;
    if (sayEventNow != SAY_PIP) chatReplies = 0;   // Bloop changed the subject
    palSendSay(sayText, 0);
  }
  // Pip said something.
  if (palSayFresh) {
    palSayFresh = false;
    strncpy(pipLine, palSayBuf, sizeof(pipLine) - 1);
    pipLine[sizeof(pipLine) - 1] = 0;
    pip.bubble = pipLine;
    pip.bubbleT = 4.5f;
    sendSay("pip", "pip", pipLine, 0);
    pipTopic = pipTopicOf(pipLine);
    bool answersUs = palSayReplyTo == palSaySeq;
    if (answersUs && chatReplies < 2 && !pet.asleep) replyT = frand(1.6f, 2.6f);
  }
  if (replyT >= 0) {
    replyT -= dt;
    if (replyT < 0 && !pet.asleep) {
      chatReplies++;
      say(SAY_PIP, true);
    }
  }
  // Now and then, Bloop starts a new chat.
  chatT -= dt;
  if (chatT <= 0) {
    chatT = frand(25, 40);
    if (!pet.asleep && !(pip.flags & PALF_ASLEEP)) say(SAY_CHAT, true);
  }
}

// Switch the radio on (it never joins a network, it only listens for Pip).
void visitorBegin() {
  pip.here = false;
  pip.enterT = pip.exitT = 0;
  pip.walkSecs = 1;
  pipLessonsTotal = pipLessonsSaved = prefs.getUInt("pipLessons", 0);
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

// ---- The door ----
// Pip doesn't just appear: a door opens in the wall at floor level and Pip hops in
// through it, and hops back out the same way when it leaves.
uint8_t doorWall = WALL_B;   // which wall was the floor when the door last opened
float doorShowT = 0;         // seconds the door stays drawn

// A spot on the floor, `inward` pixels into the room from the door (negative = outside).
Vec2 doorSpot(float inward) {
  const float r = 17;   // Pip's size
  switch (doorWall) {
    case WALL_B: return v2(ROOM_L + inward, ROOM_B - r);
    case WALL_T: return v2(ROOM_L + inward, ROOM_T + r);
    case WALL_L: return v2(ROOM_L + r, ROOM_T + inward);
    default:     return v2(ROOM_R - r, ROOM_T + inward);
  }
}

void visitorWalkIn() {
  doorWall = floorWall(gScreen);
  pip.pathFrom = doorSpot(-26);
  pip.pathTo = doorSpot(52);
  bodyInit(pip.body, BODY_BALL, pip.pathFrom.x, pip.pathFrom.y, 17, 0.35f, 6);
  pip.body.stick = 0.2f;
  pip.walkSecs = pip.enterT = 1.6f;
  pip.exitT = 0;
  doorShowT = pip.walkSecs + 0.5f;
}

void visitorWalkOut() {
  doorWall = floorWall(gScreen);
  pip.pathFrom = v2(pip.body.x, pip.body.y);
  pip.pathTo = doorSpot(-26);
  float far = vlen(v2(pip.pathTo.x - pip.pathFrom.x, pip.pathTo.y - pip.pathFrom.y));
  pip.walkSecs = pip.exitT = clampf(far / 150.0f, 0.8f, 2.4f);
  pip.enterT = 0;
  doorShowT = pip.walkSecs + 0.4f;
}

// Once a frame: move Pip along its way in or out. True while it is on the way
// (it isn't pushed about by the room until it's properly inside).
bool visitorWalking(float dt) {
  doorShowT -= dt;
  bool in = pip.enterT > 0;
  if (!in && pip.exitT <= 0) return false;
  float &t = in ? pip.enterT : pip.exitT;
  t -= dt;
  float u = clampf(1.0f - t / pip.walkSecs, 0, 1);
  Vec2 n = wallNormal(doorWall);
  float hops = fmaxf(2, roundf(pip.walkSecs * 2.5f));
  float hop = fabsf(sinf(u * PI * hops)) * 12;   // little hops along the floor
  Body &b = pip.body;
  b.x = pip.pathFrom.x + (pip.pathTo.x - pip.pathFrom.x) * u + n.x * hop;
  b.y = pip.pathFrom.y + (pip.pathTo.y - pip.pathFrom.y) * u + n.y * hop;
  b.vx = b.vy = 0;
  b.touching = 0;
  b.impact = 0;
  if (t <= 0) t = 0;
  return t > 0;
}

void drawDoor() {
  if (doorShowT <= 0) return;
  const uint16_t dark = rgb(34, 26, 40), frame = rgb(150, 110, 70);
  int x, y, w, h;
  switch (doorWall) {
    case WALL_B: x = 0; y = (int)ROOM_B - 48; w = (int)ROOM_L + 6; h = 48; break;
    case WALL_T: x = 0; y = (int)ROOM_T; w = (int)ROOM_L + 6; h = 48; break;
    case WALL_L: x = (int)ROOM_L; y = HUD_H; w = 48; h = (int)ROOM_T - HUD_H + 6; break;
    default:     x = (int)ROOM_R - 48; y = HUD_H; w = 48; h = (int)ROOM_T - HUD_H + 6; break;
  }
  cv->fillRect(x, y, w, h, dark);
  cv->drawRect(x, y, w, h, frame);
}

void visitorArrive() {
  visitorWalkIn();
  pip.here = true;
  palPlace = PLACE_BLOOPS;
  placeT = frand(50, 75);
  pip.hopCool = 1.5f;
  pip.feel = FEEL_NONE;
  pip.feelT = 0;
  pip.heartCool = 2.0f;
  pip.bubble = "Hi Bloop!";
  pip.bubbleT = 3.0f;
  pip.seenDecisions = brain.decisions;
  pip.coachT = -1;
  pip.coachCool = 5.0f;   // say hello first, teach after
  pip.lessons = 0;
  pet.joy = clampf(pet.joy + 15, 0, 100);
  chatT = frand(20, 30);
  replyT = -1;
  chatReplies = 0;
  palSaidCount = sayCount;
  lastSayMs = 0;   // greet Pip right away
  say(SAY_VISIT, true);
#if !BLOOP_HAS_VOICE
  if (!pet.asleep) { snprintf(sayText, sizeof(sayText), "Pip! You came!"); sayT = 3.5f; }
#endif
  sendEvent("pip_in", pipRssi);
}

void visitorLeave() {
  if (palPlace == PLACE_BLOOPS) visitorWalkOut();   // it hops out through the door
  visitorSaveLessons();
  pip.here = false;
  palPlace = PLACE_BLOOPS;   // Bloop is back in its own room
  for (int i = 0; i < 3; i++) palSend(false);
  lastSayMs = 0;
  say(SAY_BYE, true);
#if BLOOP_HAS_VOICE
  palSaidCount = sayCount;   // Pip has gone; don't send the goodbye later
#else
  if (!pet.asleep) { snprintf(sayText, sizeof(sayText), "Bye, Pip!"); sayT = 3.0f; }
#endif
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
  else if (pip.here && (quiet > 4000 || pipFarT > 3.0f)) visitorLeave();   // same limits as Pip's own check, so both screens agree

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
// (Pip's own board decides when it can: see visitorCoach(). These rules are the
// stand-in for when Pip doesn't answer.)
// What Pip thinks is sensible: hold on, curl up or hide when the room tips; eat
// when hungry (and beg if there's no food); sleep when tired; play, dance or
// come over when all is well (not nap or hide for no reason); play with the toy
// when all is well. Silly: dancing or napping while the room tips, ignoring food
// when hungry, eating or begging when full. Anything else, Pip keeps quiet.
// Returns +1 (good), -1 (no) or 0 (no opinion).
int pipOpinion(const Senses &s, Action a) {
  if (s.room == ROOM_TILTING) {
    if (a == ACT_GRIP || a == ACT_CURL || a == ACT_HIDE) return 1;
    if (a == ACT_DANCE || a == ACT_JUMP || a == ACT_SLEEP || a == ACT_WINDOW) return -1;
    return 0;
  }
  // The rest holds whether the room is steady or the board is lying flat (zero-g),
  // which is how it usually sits on a desk.
  if (s.hungry) {
    if (s.foodOut) return a == ACT_EAT ? 1 : -1;
    return a == ACT_BEG ? 1 : 0;
  }
  if (a == ACT_EAT || a == ACT_BEG) return -1;   // not hungry: greedy!
  if (s.tired) {
    if (a == ACT_SLEEP) return 1;
    if (a == ACT_DANCE || a == ACT_JUMP || a == ACT_ROLL || a == ACT_BALL) return -1;   // rest, silly!
    return 0;
  }
  // Fed and rested: time to play.
  if (a == ACT_BALL && s.ballNear) return 1;
  if (a == ACT_DANCE) return 1;
  if (a == ACT_COME && s.attention) return 1;
  if (a == ACT_SLEEP || a == ACT_HIDE) return -1;   // nothing to sleep off or hide from
  return 0;
}

const float PIP_PRAISE = 1.5f;   // your GOOD / NO buttons are worth 2.5

// Bloop asks Pip's own board what it thinks ("PALQ"), and Pip answers ("PALV") with
// good / no / no opinion, how strongly it feels, and what it says. Pip judges with
// its own personality: a playful Pip cheers dancing, a lazy one approves of naps, a
// grumpy one is hard to please. If no answer comes (an older Pip, or a lost message),
// the rules in pipOpinion() above are used instead.
struct __attribute__((packed)) PalAsk {
  uint8_t magic[4];   // "PALQ"
  uint8_t who;        // PAL_BLOOP
  uint8_t seq;        // a new number for each question
  uint8_t action;     // Bloop's Action
  uint8_t bits;       // 1 hungry, 2 tired, 4 ball near, 8 food out, 16 you're here
  uint8_t room;       // RoomFeel: 0 calm, 1 tilting, 2 floating
};
PalAsk coachAsk;
Senses coachSenses;
float coachWait = -1;         // seconds left to wait for Pip's answer (-1 = not waiting)
int coachResend = 0;
char coachLine[24] = "";

// Pip's verdict lands: reward or scold what Bloop is doing right now.
void visitorVerdict(int verdict, float strength, const char *text) {
  if (verdict == 0) return;
  pip.coachCool = frand(2.0f, 4.0f);
  pip.lessons++;
  pipLessonsTotal++;
  float amount = verdict * PIP_PRAISE * strength;
  rewardAcc += amount;   // credited to what Bloop is doing right now
  if (text && text[0]) {
    snprintf(coachLine, sizeof(coachLine), "%s", text);
    pip.bubble = coachLine;
  } else {
    pip.bubble = verdict > 0 ? "Good Bloop!" : "No, Bloop!";
  }
  if (verdict > 0) {
    pet.joy = clampf(pet.joy + 4, 0, 100);
    for (int k = 0; k < 3; k++)
      spawnParticle(PT_HEART, pet.body.x, pet.body.y - 10, frand(-50, 50), frand(-90, -30), 1.0f, rgb(255, 90, 140));
    pip.hopCool = 0;                   // a happy little hop
  } else {
    pet.sadT = 0.8f;
  }
  pip.bubbleT = 2.5f;
  sendEvent(verdict > 0 ? "pip_good" : "pip_no", amount);
}

void visitorCoach(float dt) {
  pip.coachCool -= dt;
  if (brain.decisions != pip.seenDecisions) {   // Bloop just chose something new
    pip.seenDecisions = brain.decisions;
    pip.coachT = 0.4f;                          // watch for a moment, then judge
    coachWait = -1;                             // too late for the previous choice
  }

  // Waiting for Pip's answer about the current choice.
  if (coachWait >= 0) {
    if (coachResend > 0) { coachResend--; esp_now_send(PAL_EVERYONE, (const uint8_t *)&coachAsk, sizeof(coachAsk)); }
    if (palVerdictFresh) {
      bool ours = palVerdictSeq == coachAsk.seq;
      int v = palVerdict;
      float strength = clampf(palVerdictStrength / 100.0f, 0, 1);
      char text[24];
      snprintf(text, sizeof(text), "%s", palVerdictText);
      palVerdictFresh = false;
      if (ours) {
        coachWait = -1;
        if (v != 0) visitorVerdict(v > 0 ? 1 : -1, strength, text);
        // Pip itself has no strong feelings about this one: it still teaches the
        // basics (eat when hungry, sleep when tired...), just more gently.
        else if (!(pip.flags & PALF_ASLEEP)) visitorVerdict(pipOpinion(coachSenses, pet.action), 0.6f, nullptr);
        return;
      }
    }
    coachWait -= dt;
    if (coachWait < 0) visitorVerdict(pipOpinion(coachSenses, pet.action), 1.0f, nullptr);   // no answer: use the rules
    return;
  }
  palVerdictFresh = false;   // a late or repeated answer: ignore it

  if (pip.coachT < 0) return;
  pip.coachT -= dt;
  if (pip.coachT >= 0) return;
  // (coachT is now below zero, so each choice is judged once.)
  if (pip.coachCool > 0 || (pip.flags & PALF_ASLEEP) || pet.asleep || pet.held) return;

  // Ask Pip.
  coachSenses = sense();
  memcpy(coachAsk.magic, "PALQ", 4);
  coachAsk.who = PAL_BLOOP;
  coachAsk.seq++;
  coachAsk.action = (uint8_t)pet.action;
  coachAsk.bits = (coachSenses.hungry ? 1 : 0) | (coachSenses.tired ? 2 : 0) | (coachSenses.ballNear ? 4 : 0) |
                  (coachSenses.foodOut ? 8 : 0) | (coachSenses.attention ? 16 : 0);
  coachAsk.room = (uint8_t)coachSenses.room;
  esp_now_send(PAL_EVERYONE, (const uint8_t *)&coachAsk, sizeof(coachAsk));
  coachResend = 2;      // and twice more on the next frames, in case one is lost
  coachWait = 1.0f;
}

// ---- Pip feels what happens in the room ----
float roomEvCool[NUM_ROOMEV] = {0};
uint8_t roomEvSeq = 0;
float pipBallHit = 0;   // hardest knock from the toy since the last frame

// Something happened to Pip: pull a face, maybe say something, and tell Pip's board.
// `cooldown` stops the same thing being reported over and over.
void roomEvent(uint8_t kind, float strength, uint8_t feel, float feelSecs, const char *bubble, float cooldown) {
  if (roomEvCool[kind] > 0) return;
  roomEvCool[kind] = cooldown;
  if (feel != FEEL_NONE) { pip.feel = feel; pip.feelT = feelSecs; }
  if (bubble && !(pip.flags & PALF_ASLEEP)) { pip.bubble = bubble; pip.bubbleT = 1.6f; }
  PalEvent m = {{'P', 'A', 'L', 'E'}, PAL_BLOOP, ++roomEvSeq, kind, (uint8_t)clampf(strength, 0, 100)};
  for (int i = 0; i < 2; i++) esp_now_send(PAL_EVERYONE, (const uint8_t *)&m, sizeof(m));
  sendEvent("pip_feels", kind);
}

// Once a frame while Pip is in Bloop's room: notice what the room is doing to it.
void visitorSenses(float dt) {
  static bool wasFloating = false, wasDark = false;
  Body &b = pip.body;
  for (int i = 0; i < NUM_ROOMEV; i++) roomEvCool[i] -= dt;
  pip.feelT -= dt;
  if (pip.feelT <= 0) pip.feel = FEEL_NONE;

  if (b.impact > 380) roomEvent(ROOMEV_BUMP, (b.impact - 300) / 5, FEEL_OUCH, 2.0f, "Ouch!", 3);
  b.impact = 0;
  if (!floating && tiltRate > 0.9f) roomEvent(ROOMEV_TILT, 60, FEEL_SCARED, 1.5f, "Hold on!", 6);
  if (floating && !wasFloating) roomEvent(ROOMEV_FLOAT, 100, FEEL_WHEE, 3.0f, "I'm floating!", 10);
  wasFloating = floating;
  if (lightsOff != wasDark) {
    if (lightsOff) roomEvent(ROOMEV_DARK, 100, FEEL_SLEEPY, 4.0f, "So dark...", 2);
    else roomEvent(ROOMEV_LIGHT, 100, FEEL_NONE, 0, "Oh! Light!", 2);
    wasDark = lightsOff;
  }
  if (pipBallHit > 120) roomEvent(ROOMEV_PLAY, pipBallHit / 4, FEEL_WHEE, 1.5f, "Boing!", 5);
  pipBallHit = 0;
}

void bloopPuff() {
  for (int k = 0; k < 8; k++)
    spawnParticle(PT_STAR, pet.body.x + frand(-22, 22), pet.body.y + frand(-22, 22),
                  frand(-70, 70), frand(-70, 70), 0.7f, RGB565_YELLOW);
}

// Time to move to the other room?
void visitorTakeTurns(float dt) {
  placeT -= dt;
  if (placeT > 0) return;
  if (palPlace == PLACE_BLOOPS) {
    // Off to Pip's. (Not while Bloop is asleep or in your hand.)
    if (pet.asleep || pet.held) { placeT = 10; return; }
    visitorWalkOut();
    bloopPuff();
    palPlace = PLACE_PIPS;
    placeT = frand(40, 60);
    sendEvent("to_pips_room", 0);
  } else {
    // Back to Bloop's: Bloop pops up where it was, Pip hops in through the door again.
    palPlace = PLACE_BLOOPS;
    placeT = frand(50, 75);
    visitorWalkIn();
    pip.hopCool = 1.5f;
    pip.seenDecisions = brain.decisions;
    pip.coachT = -1;
    coachWait = -1;
    pip.coachCool = 3.0f;
    bloopPuff();
    sendEvent("to_bloops_room", 0);
  }
  palSend(true);
}

// Once a frame: the radio, and what Pip gets up to in the room.
void visitorUpdate(float dt) {
  visitorRadio(dt);
  pipLessonsSaveT += dt;
  if (pipLessonsSaveT > 30) { pipLessonsSaveT = 0; visitorSaveLessons(); }   // at most every 30 s
  visitorWalking(dt);
  if (!pip.here) return;
  visitorTakeTurns(dt);
  if (palPlace == PLACE_PIPS) {   // both are over at Pip's: only the chatting carries on here
    visitorTalk(dt);
    return;
  }
  Body &b = pip.body;
  pip.hopCool -= dt;
  pip.heartCool -= dt;
  pip.bubbleT -= dt;
  visitorCoach(dt);
  visitorTalk(dt);
  visitorSenses(dt);

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
    roomEvent(ROOMEV_CUDDLE, 100, FEEL_NONE, 0, nullptr, 15);
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
  if (!pip.here || palPlace == PLACE_PIPS) return;
  if (pip.enterT > 0 || pip.exitT > 0) return;   // still coming through the door
  Body &b = pip.body;
  stepBody(b, g, h, floating);
  if (!pet.held && !pet.gripping) collide(pet.body, b, 3.0f, 2.0f);
  if (ball.active) pipBallHit = fmaxf(pipBallHit, collide(b, ball, 2.0f, 1.0f));
  collideBox(b, bed, 2.0f, 4.0f);
  for (int i = 0; i < MAX_FOOD; i++)
    if (food[i].active) collide(b, food[i], 2.0f, 0.3f);
}

void visitorShake() {
  if (!pip.here || palPlace == PLACE_PIPS) return;
  pip.body.vx += frand(-420, 420);
  pip.body.vy += frand(-420, 420);
  roomEvent(ROOMEV_SHAKE, 100, FEEL_SCARED, 2.5f, "Whoa!", 2);
}

// Pip looks like it does on its own e-paper screen: a white blob with a thick
// black outline and a little tip on top. Its feet point at the floor, like Bloop's.
void drawVisitor() {
  bool leaving = pip.exitT > 0;   // still hopping out through the door
  drawDoor();
  if (!pip.here && !leaving) return;
  if (palPlace == PLACE_PIPS && !leaving) {   // nobody home: a note on the wall
    const uint16_t ink = rgb(30, 28, 40);
    int w = 236, h = 62, x = (SCREEN_W - w) / 2, y = (int)(ROOM_T + ROOM_B) / 2 - h / 2;
    cv->fillRoundRect(x, y, w, h, 10, RGB565_WHITE);
    cv->drawRoundRect(x, y, w, h, 10, ink);
    cv->setFont((const GFXfont *)NULL);
    cv->setTextSize(2);
    cv->setTextColor(ink);
    cv->setCursor(x + 16, y + 12);
    cv->print("Bloop is over at");
    cv->setCursor(x + 40, y + 36);
    cv->print("Pip's room...");
    cv->setTextSize(1);
    return;
  }
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
  // Its mood: what just happened to it here, or else how Pip's own board says it feels.
  uint8_t feel = pip.feel;
  if (feel == FEEL_NONE) {
    if (pip.flags & PALF_SCARED) feel = FEEL_SCARED;
    else if (pip.flags & PALF_GRUMPY) feel = FEEL_OUCH;
    else if (pip.flags & PALF_HAPPY) feel = FEEL_WHEE;
  }
  if (asleep && pip.feel == FEEL_NONE) feel = FEEL_NONE;   // a sleeping face, unless something just woke it
  for (int s = -1; s <= 1; s += 2) {
    float ex = s * 6 + look;
    Vec2 e = P(ex, -2), hl = P(ex + 1, -3);
    if (feel == FEEL_SCARED) {            // wide eyes
      cv->fillCircle(e.x, e.y, 4, ink);
      cv->fillCircle(e.x, e.y, 3, paper);
      cv->fillCircle(e.x, e.y, 1, ink);
    } else if (feel == FEEL_OUCH) {       // cross brows over small eyes
      cv->fillCircle(e.x, e.y, 2, ink);
      line(ex - s * 4, -7, ex + s * 3, -5, ink);
    } else if (feel == FEEL_WHEE) {       // ^ ^
      line(ex - 3, -1, ex, -4, ink); line(ex, -4, ex + 3, -1, ink);
    } else if (feel == FEEL_SLEEPY) {     // half shut
      line(ex - 3, -3, ex + 3, -3, ink);
      Vec2 lid = P(ex, -1);
      cv->fillCircle(lid.x, lid.y, 1, ink);
    } else if (asleep) {
      line(ex - 3, -2, ex + 3, -2, ink);
    } else {
      cv->fillCircle(e.x, e.y, 3, ink);
      cv->drawPixel(hl.x, hl.y, paper);
    }
  }
  if (feel == FEEL_SCARED) {              // a small "o"
    Vec2 m = P(look, 8);
    cv->fillCircle(m.x, m.y, 3, ink);
    cv->fillCircle(m.x, m.y, 1, paper);
    Vec2 d0 = P(b.r - 5, -b.r + 6), d1 = P(b.r - 8, -b.r + 12), d2 = P(b.r - 2, -b.r + 12);   // sweat drop
    cv->fillTriangle(d0.x, d0.y, d1.x, d1.y, d2.x, d2.y, rgb(120, 190, 255));
  } else if (feel == FEEL_OUCH) {         // flat, unimpressed
    line(look - 4, 8, look + 4, 8, ink);
  } else if (feel == FEEL_WHEE) {         // big open smile
    Vec2 m = P(look, 7);
    cv->fillCircle(m.x, m.y, 4, ink);
    Vec2 t = P(look, 5);
    cv->fillCircle(t.x, t.y, 3, paper);
    line(look - 5, 5, look - 3, 8, ink); line(look + 3, 8, look + 5, 5, ink);
  } else if (feel == FEEL_SLEEPY) {       // a yawn
    Vec2 m = P(look, 8);
    cv->fillCircle(m.x, m.y, 2, ink);
  } else if (asleep) {
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

  // So you can see the teaching happen: a line along the bottom of the room.
  {
    char note[40];
    if (asleep) snprintf(note, sizeof(note), "Pip is napping: no lessons");
    else snprintf(note, sizeof(note), "Pip is teaching Bloop: %lu", (unsigned long)pipLessonsTotal);
    cv->setFont((const GFXfont *)NULL);
    cv->setTextSize(1);
    int nx = SCREEN_W / 2 - (int)strlen(note) * 3, ny = (int)ROOM_B - 10;
    cv->setTextColor(rgb(20, 18, 30));
    cv->setCursor(nx + 1, ny + 1);
    cv->print(note);
    cv->setTextColor(RGB565_WHITE);
    cv->setCursor(nx, ny);
    cv->print(note);
  }

  if (pip.bubbleT > 0 && pip.bubble) drawBubbleAt(b.x, b.y, b.r, pip.bubble, rgb(250, 248, 240));
}
