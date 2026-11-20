/*
  Orbit firmware, build 1
  Board: Waveshare ESP32-S3-Touch-AMOLED-1.75 (works on the -G GPS version too)

  What this build does:
    - draws Orbit's eyes with all 12 emotions (ported from the HTML preview)
    - blinks, and drifts into Sleepy when left alone, then wakes up
    - shake the board  -> Dizzy for a couple of seconds (QMI8658 motion sensor)
    - press BOOT button -> next emotion

  Not in this build yet: touch, apps, home screen, wake word, AI, GPS.
  GPIO17 and GPIO18 are left alone because the GPS module uses them.
*/

#include <Arduino.h>
#include <Wire.h>
#include "Arduino_GFX_Library.h"
#include "SensorQMI8658.hpp"
#include "esp_heap_caps.h"
#include "eyes.h"

// ---- pins (from Waveshare's hardware reference) ----
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_CS 12
#define LCD_RESET 39
#define LCD_WIDTH 466
#define LCD_HEIGHT 466
#define IIC_SDA 15
#define IIC_SCL 14
#define BOOT_PIN 0

// ---- tuning ----
#define SHAKE_SPIKE 0.9f        // how hard a jolt must be, as a fraction of gravity
#define SHAKE_COUNT 3           // jolts needed within SHAKE_WINDOW_MS
#define SHAKE_WINDOW_MS 1200
#define DIZZY_MS 2400
#define DOZE_AFTER_MS 60000     // idle time before Orbit falls asleep
#define BRIGHT_AWAKE 200
#define BRIGHT_ASLEEP 30
#define FRAME_MS 33

// Eye canvases: each eye is drawn into its own small buffer, then pushed to the screen.
// All numbers are even because the CO5300 panel needs even coordinates.
#define CW 144
#define CH 232
#define CY 117               // top of both canvases (centre of screen is y=233)
#define CX_L 88              // left canvas x
#define CX_R 234             // right canvas x

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 1 /* rotation */, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

SensorQMI8658 qmi;
bool imuOk = false;

uint16_t *bufL = nullptr, *bufR = nullptr;

// current (smoothed) eye shapes
EyeP curL, curR;
float spiralMix = 0;      // 0..1, fades the spiral in for Dizzy

int baseEmo = EMO_NEUTRAL;
int tmpEmo = -1;
uint32_t tmpUntil = 0;
bool dozing = false;
uint32_t lastActivity = 0;

// blink
float blink = 1.0f;
uint32_t nextBlink = 0, blinkEnd = 0;

// shake detection
float gRest = 0;
float prevX = 0, prevY = 0, prevZ = 0;
bool havePrev = false;
uint32_t spikeTimes[SHAKE_COUNT];
int spikeN = 0;
uint32_t lastSpike = 0;

// button
bool btnPrev = false;
uint32_t btnT = 0;

uint16_t *allocBuf(size_t bytes) {
  void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  return (uint16_t *)p;
}

int targetEmo() {
  if (dozing) return EMO_SLEEPY;
  if (tmpEmo >= 0) return tmpEmo;
  return baseEmo;
}

void poke() {          // any interaction wakes Orbit up
  lastActivity = millis();
  if (dozing) {
    dozing = false;
    gfx->setBrightness(BRIGHT_AWAKE);
  }
}

void setTemp(int e, uint32_t ms) {
  tmpEmo = e;
  tmpUntil = millis() + ms;
}

void checkShake() {
  if (!imuOk || !qmi.getDataReady()) return;
  float x, y, z;
  if (!qmi.getAccelerometer(x, y, z)) return;
  float mag = sqrtf(x * x + y * y + z * z);
  if (gRest <= 0) gRest = mag;
  gRest = gRest * 0.98f + mag * 0.02f;   // learns what "1 g" looks like in this library's units
  if (havePrev) {
    float d = fabsf(x - prevX) + fabsf(y - prevY) + fabsf(z - prevZ);
    uint32_t now = millis();
    if (d > SHAKE_SPIKE * gRest && now - lastSpike > 100) {
      lastSpike = now;
      // drop old spikes, add this one
      int k = 0;
      for (int i = 0; i < spikeN; i++)
        if (now - spikeTimes[i] < SHAKE_WINDOW_MS) spikeTimes[k++] = spikeTimes[i];
      spikeN = k;
      spikeTimes[spikeN++] = now;
      if (spikeN >= SHAKE_COUNT) {
        spikeN = 0;
        poke();
        setTemp(EMO_DIZZY, DIZZY_MS);
        Serial.println("shake!");
      }
    }
  }
  prevX = x; prevY = y; prevZ = z; havePrev = true;
}

void checkButton() {
  bool pressed = (digitalRead(BOOT_PIN) == LOW);
  uint32_t now = millis();
  if (pressed && !btnPrev && now - btnT > 250) {
    btnT = now;
    bool wasDozing = dozing;
    poke();
    if (!wasDozing) {
      baseEmo = (baseEmo + 1) % EMO_COUNT;
      tmpEmo = -1;
      Serial.print("emotion: ");
      Serial.println(EMO_NAMES[baseEmo]);
    }
  }
  btnPrev = pressed;
}

static inline void approach(float &c, float t, float f) { c += (t - c) * f; }
static void approachEye(EyeP &c, const EyeP &t, float f) {
  approach(c.w, t.w, f);   approach(c.h, t.h, f);
  approach(c.lt0, t.lt0, f); approach(c.lt1, t.lt1, f);
  approach(c.lb, t.lb, f); approach(c.rot, t.rot, f);
}

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  Wire.begin(IIC_SDA, IIC_SCL);

  gfx->begin();
  gfx->fillScreen(0x0000);
  gfx->setBrightness(BRIGHT_AWAKE);

  bufL = allocBuf(CW * CH * 2);
  bufR = allocBuf(CW * CH * 2);
  if (!bufL || !bufR) {
    Serial.println("out of memory for eye buffers");
    while (1) delay(1000);
  }

  imuOk = qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (imuOk) {
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_1000Hz,
                            SensorQMI8658::LPF_MODE_0);
    qmi.enableAccelerometer();
  } else {
    Serial.println("QMI8658 not found, shake disabled");
  }

  bool sp;
  emoTarget(EMO_NEUTRAL, curL, curR, sp);
  lastActivity = millis();
  nextBlink = millis() + 3000;
  Serial.println("Orbit ready");
}

uint32_t lastFrame = 0;

void loop() {
  uint32_t now = millis();
  checkButton();
  checkShake();

  if (tmpEmo >= 0 && (int32_t)(now - tmpUntil) > 0) tmpEmo = -1;
  if (!dozing && tmpEmo < 0 && now - lastActivity > DOZE_AFTER_MS) {
    dozing = true;
    gfx->setBrightness(BRIGHT_ASLEEP);
  }

  if (now - lastFrame < FRAME_MS) { delay(1); return; }
  float dt = (now - lastFrame) / 1000.0f;
  lastFrame = now;
  if (dt > 0.25f) dt = 0.25f;

  int e = targetEmo();
  EyeP tL, tR; bool tSpiral;
  emoTarget(e, tL, tR, tSpiral);

  // Falling asleep is slow (about 1.2 s), everything else about 0.6 s, like the preview.
  float tau = (e == EMO_SLEEPY) ? 0.45f : 0.2f;
  float f = 1.0f - expf(-dt / tau);
  approachEye(curL, tL, f);
  approachEye(curR, tR, f);
  approach(spiralMix, tSpiral ? 1.0f : 0.0f, 1.0f - expf(-dt / 0.1f));

  // blinking (not while dizzy or sleepy)
  if (e != EMO_DIZZY && e != EMO_SLEEPY) {
    if (blinkEnd == 0 && (int32_t)(now - nextBlink) > 0) {
      blinkEnd = now + 130;
      nextBlink = now + 2500 + random(0, 3500);
    }
  }
  if (blinkEnd != 0) {
    blink = 0.08f;
    if ((int32_t)(now - blinkEnd) > 0) { blinkEnd = 0; blink = 1.0f; }
  } else {
    blink = 1.0f;
  }

  memset(bufL, 0, CW * CH * 2);
  memset(bufR, 0, CW * CH * 2);
  float cx = CW / 2.0f, cy = CH / 2.0f;
  if (spiralMix < 0.5f) {
    renderEye(bufL, CW, CH, cx, cy, curL, false, blink);
    renderEye(bufR, CW, CH, cx, cy, curR, true, blink);
  } else {
    float phase = (now % 1100) / 1100.0f * 6.2831853f;
    renderSpiral(bufL, CW, CH, cx, cy, curL.w, phase);
    renderSpiral(bufR, CW, CH, cx, cy, curR.w, phase);
  }
  gfx->draw16bitRGBBitmap(CX_L, CY, bufL, CW, CH);
  gfx->draw16bitRGBBitmap(CX_R, CY, bufR, CW, CH);
}
