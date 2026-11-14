// backup.h: copy everything Bloop has stored to the computer, and put it back.
//
// Bloop keeps its brain, stats, settings (and Pip's lessons) in the board's
// settings storage (NVS). The website's "Back up Bloop" button sends "backup";
// the board answers with every stored item, one JSON line each:
//   {"t":"bk","begin":1}
//   {"t":"bk","ns":"bloop","k":"q","ty":"blob","v":"<hex>"}
//   {"t":"bk","end":<count>}
// It copies every item generically, so anything added later is included too.
//
// Restoring: the website sends these lines and waits for {"t":"bk","ok":1} after each:
//   rs <ns> <key> <type> <value>     a number or text
//   rb <ns> <key> <length>           start a blob
//   rd <hex>                         more of the blob
//   re                               write the blob
//   rdone                            finished: the board restarts with the restored data
#pragma once
#include <nvs.h>
#include <nvs_flash.h>

bool restoring = false;   // stops Bloop's own autosave from overwriting a restore

static const char *bkTypeName(nvs_type_t t) {
  switch (t) {
    case NVS_TYPE_U8: return "u8";   case NVS_TYPE_I8: return "i8";
    case NVS_TYPE_U16: return "u16"; case NVS_TYPE_I16: return "i16";
    case NVS_TYPE_U32: return "u32"; case NVS_TYPE_I32: return "i32";
    case NVS_TYPE_U64: return "u64"; case NVS_TYPE_I64: return "i64";
    case NVS_TYPE_STR: return "str"; case NVS_TYPE_BLOB: return "blob";
    default: return nullptr;
  }
}

// Wi-Fi and radio calibration live in the same storage; leave those alone.
static bool bkSkipNamespace(const char *ns) {
  return !strncmp(ns, "nvs.", 4) || !strcmp(ns, "phy") || !strcmp(ns, "misc") || !strncmp(ns, "net80211", 8);
}

void backupDump() {
  if (!Serial) return;
  Serial.setTxTimeoutMs(500);   // this must arrive complete: wait for the computer
  Serial.println("{\"t\":\"bk\",\"begin\":1}");
  int count = 0;
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find("nvs", nullptr, NVS_TYPE_ANY, &it);
  while (err == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    const char *ty = bkTypeName(info.type);
    nvs_handle_t h;
    if (ty && !bkSkipNamespace(info.namespace_name) && nvs_open(info.namespace_name, NVS_READONLY, &h) == ESP_OK) {
      Serial.printf("{\"t\":\"bk\",\"ns\":\"%s\",\"k\":\"%s\",\"ty\":\"%s\",\"v\":", info.namespace_name, info.key, ty);
      switch (info.type) {
        case NVS_TYPE_U8:  { uint8_t v = 0;  nvs_get_u8(h, info.key, &v);  Serial.printf("%u", v); break; }
        case NVS_TYPE_I8:  { int8_t v = 0;   nvs_get_i8(h, info.key, &v);  Serial.printf("%d", v); break; }
        case NVS_TYPE_U16: { uint16_t v = 0; nvs_get_u16(h, info.key, &v); Serial.printf("%u", v); break; }
        case NVS_TYPE_I16: { int16_t v = 0;  nvs_get_i16(h, info.key, &v); Serial.printf("%d", v); break; }
        case NVS_TYPE_U32: { uint32_t v = 0; nvs_get_u32(h, info.key, &v); Serial.printf("%lu", (unsigned long)v); break; }
        case NVS_TYPE_I32: { int32_t v = 0;  nvs_get_i32(h, info.key, &v); Serial.printf("%ld", (long)v); break; }
        case NVS_TYPE_U64: { uint64_t v = 0; nvs_get_u64(h, info.key, &v); Serial.printf("\"%llu\"", (unsigned long long)v); break; }
        case NVS_TYPE_I64: { int64_t v = 0;  nvs_get_i64(h, info.key, &v); Serial.printf("\"%lld\"", (long long)v); break; }
        case NVS_TYPE_STR: {
          size_t len = 0;
          nvs_get_str(h, info.key, nullptr, &len);
          char *s = (char *)malloc(len + 1);
          Serial.print('"');
          if (s && nvs_get_str(h, info.key, s, &len) == ESP_OK)
            for (size_t i = 0; s[i]; i++) {   // hex-escape anything that would break the line
              uint8_t c = s[i];
              if (c < 32 || c == '"' || c == '\\') Serial.printf("\\u%04x", c);
              else Serial.print((char)c);
            }
          Serial.print('"');
          free(s);
          break;
        }
        case NVS_TYPE_BLOB: {
          size_t len = 0;
          nvs_get_blob(h, info.key, nullptr, &len);
          uint8_t *b = (uint8_t *)malloc(len ? len : 1);
          Serial.print('"');
          if (b && nvs_get_blob(h, info.key, b, &len) == ESP_OK)
            for (size_t i = 0; i < len; i++) Serial.printf("%02x", b[i]);
          Serial.print('"');
          free(b);
          break;
        }
        default: Serial.print("null"); break;
      }
      Serial.println("}");
      count++;
      nvs_close(h);
    }
    err = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  Serial.printf("{\"t\":\"bk\",\"end\":%d}\n", count);
  Serial.flush();
  Serial.setTxTimeoutMs(0);
}

// ---- Restore ----
static char bkNs[16], bkKey[16];
static uint8_t *bkBlob = nullptr;
static size_t bkBlobLen = 0, bkBlobAt = 0;
static int bkRestored = 0;

static void bkAck(bool ok, const char *why) {
  Serial.setTxTimeoutMs(500);
  if (ok) Serial.println("{\"t\":\"bk\",\"ok\":1}");
  else Serial.printf("{\"t\":\"bk\",\"ok\":0,\"why\":\"%s\"}\n", why);
  Serial.setTxTimeoutMs(0);
}

static int bkHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Returns true if the line was a restore command.
bool restoreCommand(char *line) {
  if (strncmp(line, "rs ", 3) && strncmp(line, "rb ", 3) && strncmp(line, "rd ", 3) &&
      strcmp(line, "re") && strcmp(line, "rdone")) return false;
  restoring = true;

  if (!strcmp(line, "rdone")) {
    Serial.setTxTimeoutMs(500);
    Serial.printf("{\"t\":\"bk\",\"ok\":1,\"restored\":%d}\n", bkRestored);
    Serial.flush();
    delay(300);
    ESP.restart();   // start again with the restored brain and stats
  }

  if (!strncmp(line, "rs ", 3)) {
    char ns[16], key[16], ty[8];
    int used = 0;
    if (sscanf(line + 3, "%15s %15s %7s %n", ns, key, ty, &used) < 3) { bkAck(false, "bad line"); return true; }
    const char *val = line + 3 + used;
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) { bkAck(false, "open"); return true; }
    esp_err_t e = ESP_FAIL;
    if (!strcmp(ty, "u8")) e = nvs_set_u8(h, key, (uint8_t)strtoul(val, nullptr, 10));
    else if (!strcmp(ty, "i8")) e = nvs_set_i8(h, key, (int8_t)strtol(val, nullptr, 10));
    else if (!strcmp(ty, "u16")) e = nvs_set_u16(h, key, (uint16_t)strtoul(val, nullptr, 10));
    else if (!strcmp(ty, "i16")) e = nvs_set_i16(h, key, (int16_t)strtol(val, nullptr, 10));
    else if (!strcmp(ty, "u32")) e = nvs_set_u32(h, key, (uint32_t)strtoul(val, nullptr, 10));
    else if (!strcmp(ty, "i32")) e = nvs_set_i32(h, key, (int32_t)strtol(val, nullptr, 10));
    else if (!strcmp(ty, "u64")) e = nvs_set_u64(h, key, strtoull(val, nullptr, 10));
    else if (!strcmp(ty, "i64")) e = nvs_set_i64(h, key, strtoll(val, nullptr, 10));
    else if (!strcmp(ty, "str")) e = nvs_set_str(h, key, val);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) bkRestored++;
    bkAck(e == ESP_OK, "write");
    return true;
  }

  if (!strncmp(line, "rb ", 3)) {
    unsigned long len = 0;
    if (sscanf(line + 3, "%15s %15s %lu", bkNs, bkKey, &len) < 3 || len > 64 * 1024) { bkAck(false, "bad blob"); return true; }
    free(bkBlob);
    bkBlob = (uint8_t *)malloc(len ? len : 1);
    bkBlobLen = len;
    bkBlobAt = 0;
    bkAck(bkBlob != nullptr, "memory");
    return true;
  }

  if (!strncmp(line, "rd ", 3)) {
    for (const char *p = line + 3; p[0] && p[1] && bkBlob && bkBlobAt < bkBlobLen; p += 2) {
      int hi = bkHex(p[0]), lo = bkHex(p[1]);
      if (hi < 0 || lo < 0) break;
      bkBlob[bkBlobAt++] = (uint8_t)(hi * 16 + lo);
    }
    bkAck(bkBlob != nullptr, "no blob");
    return true;
  }

  // "re": write the blob
  nvs_handle_t h;
  esp_err_t e = ESP_FAIL;
  if (bkBlob && bkBlobAt == bkBlobLen && nvs_open(bkNs, NVS_READWRITE, &h) == ESP_OK) {
    e = nvs_set_blob(h, bkKey, bkBlob, bkBlobLen);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
  }
  free(bkBlob);
  bkBlob = nullptr;
  if (e == ESP_OK) bkRestored++;
  bkAck(e == ESP_OK, "blob write");
  return true;
}
