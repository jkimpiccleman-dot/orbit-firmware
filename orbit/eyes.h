// Orbit eyes: pure drawing math (no hardware). Ported from the HTML preview.

rotation: 90
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

struct EyeP { float w, h, lt0, lt1, lb, rot; };

enum { EMO_NEUTRAL, EMO_HAPPY, EMO_SAD, EMO_SURPRISED, EMO_CONFUSED, EMO_TIRED,
       EMO_SLEEPY, EMO_EXCITED, EMO_ANGRY, EMO_CURIOUS, EMO_MINDBLOWN, EMO_DIZZY, EMO_COUNT };

static const char *EMO_NAMES[EMO_COUNT] = {
  "neutral", "happy", "sad", "surprised", "confused", "tired",
  "sleepy", "excited", "angry", "curious", "mindblown", "dizzy" };

static const float EYE_SCALE = 1.9f;  // same scale the preview uses on a 466 px screen

// Target shape for each emotion (values copied from the preview's table E).
static inline void emoTarget(int e, EyeP &L, EyeP &R, bool &spiral) {
  EyeP b = {40, 82, 0, 0, 0, 0};
  spiral = false;
  L = b; R = b;
  switch (e) {
    case EMO_HAPPY:     L.h = R.h = 76; L.lb = R.lb = 50; break;
    case EMO_SAD:       L.lt0 = R.lt0 = 38; L.lt1 = R.lt1 = -18; break;
    case EMO_SURPRISED: L.w = R.w = 46; L.h = R.h = 104; break;
    case EMO_CONFUSED:  L.h = 60; L.rot = -8; R.h = 88; break;
    case EMO_TIRED:     L.lt0 = R.lt0 = 42; break;
    case EMO_SLEEPY:    L.lt0 = R.lt0 = 72; break;
    case EMO_EXCITED:   L.w = R.w = 44; L.h = R.h = 92; L.lb = R.lb = 20; break;
    case EMO_ANGRY:     L.lt0 = R.lt0 = 42; L.lt1 = R.lt1 = 22; break;
    case EMO_CURIOUS:   R.w = 50; R.h = 104; break;
    case EMO_MINDBLOWN: L.w = R.w = 50; L.h = R.h = 90; break;
    case EMO_DIZZY:     L.w = R.w = 70; L.h = R.h = 70; spiral = true; break;
    default: break;
  }
  L.w *= EYE_SCALE; L.h *= EYE_SCALE; R.w *= EYE_SCALE; R.h *= EYE_SCALE;
}

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline uint16_t gray565(float cov) {
  int v = (int)(255.0f * clamp01(cov) + 0.5f);
  return (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
}
static inline void putMax(uint16_t *buf, int W, int H, int x, int y, float cov) {
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  uint16_t c = gray565(cov);
  if (c > buf[y * W + x]) buf[y * W + x] = c;
}

// One eye: white rounded pill with optional top lid, bottom curve, rotation.
// 'mirror' is true for the right eye (angles flip, like the preview).
// 'blink' scales the height (1 = open, 0.08 = closed).
static inline void renderEye(uint16_t *buf, int W, int H, float cx, float cy,
                             const EyeP &e, bool mirror, float blink) {
  const float DEG = 0.01745329252f;
  float hw = e.w * 0.5f, hh = e.h * 0.5f;
  float r = hw < hh ? hw : hh;
  float th = e.rot * (mirror ? -1.f : 1.f) * DEG;
  float cs = cosf(th), sn = sinf(th);
  float lang = e.lt1 * (mirror ? -1.f : 1.f) * DEG;
  float lcos = cosf(lang), ltan = tanf(lang);
  float lidTop = -hh + e.h * e.lt0 / 100.f;
  float lbTop = -hh + e.h * (100.f - e.lb) / 100.f;
  float la = 0.8f * e.w, lbh = e.h * 0.5f, lyc = lbTop + lbh;
  float lmin = la < lbh ? la : lbh;
  float R = (hw > hh ? hw : hh) * 1.15f + 4.f;
  int x0 = (int)(cx - R), x1 = (int)(cx + R);
  int y0 = (int)(cy - R * blink - 4), y1 = (int)(cy + R * blink + 4);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > W - 1) x1 = W - 1;
  if (y1 > H - 1) y1 = H - 1;
  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      float dx = x + 0.5f - cx, dy = (y + 0.5f - cy) / blink;
      float u = dx * cs + dy * sn, v = -dx * sn + dy * cs;
      float qx = fabsf(u) - (hw - r), qy = fabsf(v) - (hh - r);
      float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
      float sdf = sqrtf(mx * mx + my * my) + fminf(fmaxf(qx, qy), 0.f) - r;
      float cov = clamp01(0.5f - sdf);
      if (cov <= 0) continue;
      if (e.lt0 > 0) {
        float edge = lidTop + ltan * u;
        cov *= 1.f - clamp01(0.5f + (edge - v) * lcos);
      }
      if (e.lb > 0) {
        float f = sqrtf((u / la) * (u / la) + ((v - lyc) / lbh) * ((v - lyc) / lbh));
        cov *= 1.f - clamp01(0.5f - (f - 1.f) * lmin);
      }
      if (cov > 0) buf[y * W + x] = gray565(cov);
    }
  }
}

// Dizzy eye: a rotating spiral (same curve as the preview).
static inline void renderSpiral(uint16_t *buf, int W, int H, float cx, float cy,
                                float size, float phase) {
  float s = size / 100.f, rad = 3.5f * s;
  float px = 0, py = 0; bool have = false;
  for (float a = 0; a <= 12.5f; a += 0.1f) {
    float rr = a * 3.1f * s;
    float x = cx + rr * cosf(a + phase), y = cy + rr * sinf(a + phase);
    if (have) {
      float len = sqrtf((x - px) * (x - px) + (y - py) * (y - py));
      int n = (int)(len) + 1;
      for (int i = 0; i <= n; i++) {
        float t = (float)i / n, sx = px + (x - px) * t, sy = py + (y - py) * t;
        for (int yy = (int)(sy - rad - 1); yy <= (int)(sy + rad + 1); yy++)
          for (int xx = (int)(sx - rad - 1); xx <= (int)(sx + rad + 1); xx++) {
            float d = sqrtf((xx + 0.5f - sx) * (xx + 0.5f - sx) + (yy + 0.5f - sy) * (yy + 0.5f - sy));
            putMax(buf, W, H, xx, yy, 0.5f + rad - d);
          }
      }
    }
    px = x; py = y; have = true;
  }
}
