// Checks the board's model code (bloop_room/voice.h) against the Python model.
// Each line of out/voice_test.txt: prompt ids | expected greedy output ids.
// Built and run on GitHub: gcc -O2 -I bloop_room training/test_voice.c -lm
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voice.h"

static float never(void) { return 0.0f; }

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "training/out/voice_test.txt";
  FILE *f = fopen(path, "r");
  if (!f) { printf("can't open %s\n", path); return 2; }
  static BloopVoice m;
  char line[4096];
  int total = 0, same = 0;
  while (fgets(line, sizeof line, f)) {
    int prompt[64], want[64], np = 0, nw = 0, side = 0;
    for (char *tok = strtok(line, " \n"); tok; tok = strtok(NULL, " \n")) {
      if (!strcmp(tok, "|")) { side = 1; continue; }
      if (side) want[nw++] = atoi(tok); else prompt[np++] = atoi(tok);
    }
    int got[64];
    int ng = bv_generate(&m, prompt, np, got, 64, 0.0f, 1, never);
    int ok = (ng == nw) && !memcmp(got, want, sizeof(int) * ng);
    char text[256];
    bv_text(got, ng, text, sizeof text);
    printf("%s  %s\n", ok ? "same" : "DIFF", text);
    total++;
    same += ok;
  }
  fclose(f);
  printf("%d/%d prompts give exactly the same words as the Python model\n", same, total);
  // Float rounding can flip a near-tie now and then; anything more means a bug.
  return (total > 0 && same * 10 >= total * 9) ? 0 : 1;
}
