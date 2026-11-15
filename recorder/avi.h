// avi.h: write MJPEG video as a standard .avi file (plays in VLC, QuickTime, etc.).
//
// Layout:  RIFF 'AVI '
//            LIST 'hdrl'  ('avih' main header, LIST 'strl' with 'strh' + 'strf')
//            LIST 'movi'  one '00dc' chunk per JPEG frame
//            'idx1'       where each frame is, so players can seek
// The header is written first with blank counts and fixed at the end, when the
// number of frames and the real frame rate are known.
#pragma once
#include <FS.h>

struct AviIndexEntry {
  uint32_t offset;   // from the start of the 'movi' data
  uint32_t size;
};

class AviWriter {
public:
  File f;
  uint32_t frames = 0;
  uint32_t moviStart = 0;      // file position of the 'movi' fourcc
  uint32_t maxFrame = 0;
  AviIndexEntry *index = nullptr;
  uint32_t indexCap = 0;
  uint16_t width = 0, height = 0;

  bool begin(File file, uint16_t w, uint16_t h, uint32_t maxFrames) {
    f = file;
    width = w;
    height = h;
    frames = 0;
    maxFrame = 0;
    indexCap = maxFrames;
    free(index);
    index = (AviIndexEntry *)ps_malloc(sizeof(AviIndexEntry) * maxFrames);
    if (!index) return false;
    writeHeader(0, 50000);
    return true;
  }

  // `chunk` must start with 8 spare bytes for the '00dc' + size header, followed
  // by `jpegLen` bytes of JPEG, and have room for 1 more byte of padding.
  bool addFrame(uint8_t *chunk, uint32_t jpegLen) {
    if (frames >= indexCap) return false;
    uint32_t padded = (jpegLen + 1) & ~1u;   // chunks are 2-byte aligned
    memcpy(chunk, "00dc", 4);
    put32(chunk + 4, jpegLen);
    if (padded != jpegLen) chunk[8 + jpegLen] = 0;
    uint32_t pos = f.position();
    if (f.write(chunk, 8 + padded) != 8 + padded) return false;
    index[frames].offset = pos - moviStart;
    index[frames].size = jpegLen;
    if (jpegLen > maxFrame) maxFrame = jpegLen;
    frames++;
    return true;
  }

  // Finish the file: write the index, then go back and fill in the header.
  void end(uint32_t elapsedUs) {
    uint32_t moviEnd = f.position();
    // idx1
    write4("idx1");
    write32(frames * 16);
    for (uint32_t i = 0; i < frames; i++) {
      uint8_t e[16];
      memcpy(e, "00dc", 4);
      put32(e + 4, 0x10);              // keyframe (every MJPEG frame is)
      put32(e + 8, index[i].offset);
      put32(e + 12, index[i].size);
      f.write(e, 16);
    }
    uint32_t fileEnd = f.position();
    uint32_t usPerFrame = frames ? elapsedUs / frames : 50000;
    f.seek(0);
    writeHeader(frames, usPerFrame ? usPerFrame : 50000);
    // RIFF size and 'movi' list size
    f.seek(4);
    write32(fileEnd - 8);
    f.seek(moviStart - 4);
    write32(moviEnd - moviStart);
    f.close();
    free(index);
    index = nullptr;
  }

private:
  static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
  void write4(const char *s) { f.write((const uint8_t *)s, 4); }
  void write32(uint32_t v) { uint8_t b[4]; put32(b, v); f.write(b, 4); }
  void write16(uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; f.write(b, 2); }

  void writeHeader(uint32_t nFrames, uint32_t usPerFrame) {
    write4("RIFF"); write32(0); write4("AVI ");
    write4("LIST"); write32(4 + 8 + 56 + 8 + 4 + 8 + 56 + 8 + 40); write4("hdrl");
    // avih
    write4("avih"); write32(56);
    write32(usPerFrame);
    write32(maxFrame * (1000000 / (usPerFrame ? usPerFrame : 50000)));   // max bytes per second
    write32(0);
    write32(0x10);                      // has an index
    write32(nFrames);
    write32(0);
    write32(1);                         // one stream
    write32(maxFrame);
    write32(width); write32(height);
    write32(0); write32(0); write32(0); write32(0);
    // strl
    write4("LIST"); write32(4 + 8 + 56 + 8 + 40); write4("strl");
    write4("strh"); write32(56);
    write4("vids"); write4("MJPG");
    write32(0); write16(0); write16(0); write32(0);
    write32(usPerFrame);                // scale / rate = seconds per frame
    write32(1000000);
    write32(0);
    write32(nFrames);
    write32(maxFrame);
    write32(0xFFFFFFFF);
    write32(0);
    write16(0); write16(0); write16(width); write16(height);
    write4("strf"); write32(40);
    write32(40); write32(width); write32(height);
    write16(1); write16(24);
    write4("MJPG");
    write32((uint32_t)width * height * 3);
    write32(0); write32(0); write32(0); write32(0);
    // movi
    write4("LIST"); write32(0); write4("movi");
    moviStart = f.position() - 4;       // offsets in idx1 count from the 'movi' fourcc
  }
};
