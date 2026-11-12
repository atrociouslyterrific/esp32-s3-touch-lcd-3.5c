// voice.h: Bloop's voice. Runs a tiny transformer language model on the board.
//
// The model (trained on GitHub by training/train_voice.py) reads tokens that
// describe how Bloop feels and what just happened, then writes a short line one
// word at a time. Weights are 8-bit numbers with one scale per row, stored in
// flash (voice_weights.h). This is plain C so the same file is tested on GitHub
// against the Python model (training/test_voice.c).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "voice_weights.h"

// All working memory lives here (not on the stack): ~150 KB for the 256-wide model,
// so the board keeps it in PSRAM.
typedef struct {
  float k[BV_L][BV_T][BV_D];
  float v[BV_L][BV_T][BV_D];
  float logits[BV_V];
  float x[BV_D], h[BV_D], qkv[3 * BV_D], att[BV_D], tmp[BV_D], ff[BV_FF], sc[BV_T];
  int pos;
} BloopVoice;

static void bv_layernorm(float *out, const float *x, const float *g, const float *b) {
  float m = 0, var = 0;
  for (int i = 0; i < BV_D; i++) m += x[i];
  m /= BV_D;
  for (int i = 0; i < BV_D; i++) var += (x[i] - m) * (x[i] - m);
  var /= BV_D;
  float inv = 1.0f / sqrtf(var + 1e-5f);
  for (int i = 0; i < BV_D; i++) out[i] = (x[i] - m) * inv * g[i] + b[i];
}

// out[r] = scale[r] * (row r of q . x) + bias[r]
static void bv_linear(float *out, const int8_t *q, const float *s, const float *b, const float *x, int rows, int cols) {
  for (int r = 0; r < rows; r++) {
    const int8_t *w = q + (size_t)r * cols;
    float acc = 0;
    for (int c = 0; c < cols; c++) acc += (float)w[c] * x[c];
    out[r] = acc * s[r] + b[r];
  }
}

static float bv_gelu(float x) {
  return 0.5f * x * (1.0f + tanhf(0.7978845608f * (x + 0.044715f * x * x * x)));
}

static void bv_reset(BloopVoice *m) { m->pos = 0; }

// Feed one token; leaves the next-word scores in m->logits.
static void bv_step(BloopVoice *m, int tok) {
  float *x = m->x, *h = m->h, *qkv = m->qkv, *att = m->att, *tmp = m->tmp, *ff = m->ff, *sc = m->sc;
  const int hd = BV_D / BV_H;
  const float inv = 1.0f / sqrtf((float)hd);
  int p = m->pos;
  for (int i = 0; i < BV_D; i++) x[i] = (float)BV_TOK_Q[tok * BV_D + i] * BV_TOK_S[tok] + BV_POS[p * BV_D + i];

  for (int l = 0; l < BV_L; l++) {
    bv_layernorm(h, x, BV_LN1_G + l * BV_D, BV_LN1_B + l * BV_D);
    bv_linear(qkv, BV_QKV_Q + (size_t)l * 3 * BV_D * BV_D, BV_QKV_S + l * 3 * BV_D, BV_QKV_B + l * 3 * BV_D, h, 3 * BV_D, BV_D);
    memcpy(m->k[l][p], qkv + BV_D, sizeof(float) * BV_D);
    memcpy(m->v[l][p], qkv + 2 * BV_D, sizeof(float) * BV_D);
    for (int hh = 0; hh < BV_H; hh++) {
      int o = hh * hd;
      float mx = -1e30f, sum = 0;
      for (int t = 0; t <= p; t++) {
        float d = 0;
        for (int i = 0; i < hd; i++) d += m->k[l][t][o + i] * qkv[o + i];
        sc[t] = d * inv;
        if (sc[t] > mx) mx = sc[t];
      }
      for (int t = 0; t <= p; t++) { sc[t] = expf(sc[t] - mx); sum += sc[t]; }
      for (int i = 0; i < hd; i++) {
        float a = 0;
        for (int t = 0; t <= p; t++) a += sc[t] * m->v[l][t][o + i];
        att[o + i] = a / sum;
      }
    }
    bv_linear(tmp, BV_O_Q + (size_t)l * BV_D * BV_D, BV_O_S + l * BV_D, BV_O_B + l * BV_D, att, BV_D, BV_D);
    for (int i = 0; i < BV_D; i++) x[i] += tmp[i];
    bv_layernorm(h, x, BV_LN2_G + l * BV_D, BV_LN2_B + l * BV_D);
    bv_linear(ff, BV_F1_Q + (size_t)l * BV_FF * BV_D, BV_F1_S + l * BV_FF, BV_F1_B + l * BV_FF, h, BV_FF, BV_D);
    for (int i = 0; i < BV_FF; i++) ff[i] = bv_gelu(ff[i]);
    bv_linear(tmp, BV_F2_Q + (size_t)l * BV_D * BV_FF, BV_F2_S + l * BV_D, BV_F2_B + l * BV_D, ff, BV_D, BV_FF);
    for (int i = 0; i < BV_D; i++) x[i] += tmp[i];
  }
  bv_layernorm(h, x, BV_LNF_G, BV_LNF_B);
  for (int v = 0; v < BV_V; v++) {
    const int8_t *w = BV_TOK_Q + v * BV_D;
    float acc = 0;
    for (int i = 0; i < BV_D; i++) acc += (float)w[i] * h[i];
    m->logits[v] = acc * BV_TOK_S[v];
  }
  m->pos++;
}

// Pick the next word. temp <= 0: always the most likely word (used by the test).
// Otherwise sample from the top `topk` words; rnd01() returns a number in [0, 1).
static int bv_pick(const BloopVoice *m, float temp, int topk, float (*rnd01)(void)) {
  int best = 0;
  for (int v = 1; v < BV_V; v++) if (m->logits[v] > m->logits[best]) best = v;
  if (temp <= 0) return best;
  if (topk > 16) topk = 16;
  int ids[16];
  int n = 0;
  for (int k = 0; k < topk; k++) {        // simple top-k selection
    int b = -1;
    for (int v = 0; v < BV_V; v++) {
      int used = 0;
      for (int j = 0; j < n; j++) if (ids[j] == v) { used = 1; break; }
      if (!used && (b < 0 || m->logits[v] > m->logits[b])) b = v;
    }
    ids[n++] = b;
  }
  float pr[16], sum = 0, mx = m->logits[ids[0]];
  for (int k = 0; k < n; k++) { pr[k] = expf((m->logits[ids[k]] - mx) / temp); sum += pr[k]; }
  float r = rnd01() * sum;
  for (int k = 0; k < n; k++) { r -= pr[k]; if (r <= 0) return ids[k]; }
  return ids[n - 1];
}

// Run the prompt, then write up to maxWords words. Returns how many it wrote.
static int bv_generate(BloopVoice *m, const int *prompt, int n, int *out, int maxWords,
                       float temp, int topk, float (*rnd01)(void)) {
  bv_reset(m);
  for (int i = 0; i < n && m->pos < BV_T; i++) bv_step(m, prompt[i]);
  int count = 0;
  while (count < maxWords && m->pos < BV_T) {
    int t = bv_pick(m, temp, topk, rnd01);
    if (t == BV_TOK_END) break;
    out[count++] = t;
    bv_step(m, t);
  }
  return count;
}

// Words -> text, with punctuation stuck to the word before it.
static void bv_text(const int *ids, int n, char *buf, int len) {
  int at = 0;
  buf[0] = 0;
  for (int i = 0; i < n; i++) {
    const char *w = BV_WORDS[ids[i]];
    int punct = !strcmp(w, ".") || !strcmp(w, "!") || !strcmp(w, "?") || !strcmp(w, ",") || !strcmp(w, "...");
    int need = (int)strlen(w) + ((at && !punct) ? 1 : 0);
    if (at + need >= len) break;
    if (at && !punct) buf[at++] = ' ';
    strcpy(buf + at, w);
    at += (int)strlen(w);
  }
}
