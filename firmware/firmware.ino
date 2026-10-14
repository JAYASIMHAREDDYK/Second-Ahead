// SECONDS AHEAD - Glyph H2 (ESP32-H2) version, fixed
// Arduino IDE: board "ESP32H2 Dev Module", USB CDC On Boot = Enabled
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------- Pins (GPIO numbers) ----------
const int TRIG_A = 10, ECHO_A = 11;
const int TRIG_B = 14, ECHO_B = 1;    // only used if USE_B = true
const int PIN_IR = 12;
const bool IR_ACTIVE_LOW = true;
const int PIN_BTN = 24;               // optional mute button to GND
const int LED_RED_0 = 0, LED_RED_13 = 13; // Dual-output: both GPIO 0 & GPIO 13 drive RED LED
const int LED_BLUE = 4;
const int PIN_BUZZER = 5;
const bool BUZZER_PASSIVE = false;    // true if steady HIGH gives no sound
const int I2C_SDA = 22, I2C_SCL = 23;

// ---------- Tuning ----------
const float MIN_CM = 2.0f, MAX_CM = 100.0f;   // detection range on the bench
const float MAX_DISAGREE_CM = 15.0f;
const float MIN_APPROACH_CMS = 4.0f;
const float TTC_ENTER_HIGH = 3.0f;
const float TTC_EXIT_HIGH = 3.5f;
const float DANGER_CM = 50.0f;        // approaching AND closer than this = HIGH RISK
const int MEDIAN_WINDOW = 3;
const int HISTORY = 5;
const int MIN_HIST = 4;               // samples needed before speed is trusted (fast path)
const int MIN_STREAK = 2;
const int MAX_BAD = 5;                // bad reads before state is re-evaluated
const int HOLD_BAD = 25;              // HIGH_RISK is held through dropouts up to this many bad reads
const int LOST_BAD = 40;              // bad reads before target is considered gone
const unsigned long GAP_MS = 500;     // history older than this is flushed

enum State { NO_TARGET, NO_APPROACH, UNCERTAIN, MONITOR, HIGH_RISK };
const char* stateText[] = {"WAIT", "NO APPROACH", "UNCERTAIN", "MONITOR", "HIGH RISK"};

struct Tracker {
  float raw[MEDIAN_WINDOW];
  int rawCount = 0;
  float histD[HISTORY];
  unsigned long histT[HISTORY];
  int histCount = 0;
  int badStreak = 0;
  int approachStreak = 0;
  float dist = 0;
  float speed = 0;
  float ttc = -1;
  State state = NO_TARGET;
};

Tracker tracker;
LiquidCrystal_I2C* lcd = nullptr;
bool muted = false;

const bool USE_B = true; // Enabled for Perpendicular / Lateral Side Sensor
float sideDist = 0.0f;

float readDistanceCm(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(3);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  unsigned long us = pulseIn(echo, HIGH, 25000UL); // 25ms timeout (~400cm max, reliably catches full 100cm range)
  if (us == 0) return -1;
  return us * 0.0343f / 2.0f;
}

// Front Primary Sensor (Sensor A)
float readFused() {
  float a = readDistanceCm(TRIG_A, ECHO_A);
  if (a >= MIN_CM && a <= MAX_CM) return a;
  return -1.0f;
}

// Perpendicular Lateral Side Sensor (Sensor B)
float readSideSensor() {
  if (!USE_B) return -1.0f;
  float b = readDistanceCm(TRIG_B, ECHO_B);
  if (b >= MIN_CM && b <= MAX_CM) return b;
  return -1.0f;
}

float median(const float* v, int n) {
  float s[MEDIAN_WINDOW];
  for (int i = 0; i < n; i++) s[i] = v[i];
  for (int i = 1; i < n; i++) {
    float k = s[i];
    int j = i - 1;
    while (j >= 0 && s[j] > k) {
      s[j + 1] = s[j];
      j--;
    }
    s[j + 1] = k;
  }
  return s[n / 2];
}

void pushHistory(Tracker& t, float d, unsigned long now) {
  if (t.histCount == HISTORY) {
    for (int i = 1; i < HISTORY; i++) {
      t.histD[i - 1] = t.histD[i];
      t.histT[i - 1] = t.histT[i];
    }
    t.histCount--;
  }
  t.histD[t.histCount] = d;
  t.histT[t.histCount] = now;
  t.histCount++;
}

void resetTrack(Tracker& t) {
  t.rawCount = 0;
  t.histCount = 0;
  t.approachStreak = 0;
  t.speed = 0;
  t.ttc = -1;
}

void classify(Tracker& t) {
  if (t.histCount == 0) {
    t.state = NO_TARGET;
    t.ttc = -1;
    return;
  }
  if (t.badStreak >= MAX_BAD) {
    // short dropout while in HIGH_RISK (e.g. angled fast car): keep warning
    if (t.state == HIGH_RISK && t.badStreak < HOLD_BAD) return;
    t.state = UNCERTAIN;
    t.ttc = -1;
    return;
  }
  if (t.histCount < MIN_HIST) {
    t.state = UNCERTAIN;
    t.ttc = -1;
    return;
  }
  if (t.speed < MIN_APPROACH_CMS) {
    t.state = NO_APPROACH;
    t.ttc = -1;
    t.approachStreak = 0;
    return;
  }
  t.approachStreak++;
  if (t.approachStreak < MIN_STREAK) {
    t.state = UNCERTAIN;
    t.ttc = -1;
    return;
  }
  t.ttc = t.dist / t.speed;
  if (t.state == HIGH_RISK) {
    t.state = (t.ttc > TTC_EXIT_HIGH && t.dist > DANGER_CM + 10) ? MONITOR : HIGH_RISK;
  } else {
    t.state = (t.ttc < TTC_ENTER_HIGH || t.dist < DANGER_CM) ? HIGH_RISK : MONITOR;
  }
}

void updateTracker(Tracker& t) {
  unsigned long now = millis();
  float d = readFused();

  if (d < 0) {
    t.badStreak++;
    if (t.badStreak >= LOST_BAD) {
      resetTrack(t);
      t.dist = 0;
      t.state = NO_TARGET;
      return;
    }
    // only re-evaluate once the dropout is long enough; never on stale data
    if (t.badStreak >= MAX_BAD) classify(t);
    return;
  }
  t.badStreak = 0;

  // flush stale samples so speed isn't computed across a gap
  if (t.histCount > 0 && now - t.histT[t.histCount - 1] > GAP_MS) resetTrack(t);

  if (t.rawCount < MEDIAN_WINDOW) {
    t.raw[t.rawCount++] = d;
  } else {
    for (int i = 1; i < MEDIAN_WINDOW; i++) t.raw[i - 1] = t.raw[i];
    t.raw[MEDIAN_WINDOW - 1] = d;
  }
  if (t.rawCount < MEDIAN_WINDOW) return;

  t.dist = median(t.raw, MEDIAN_WINDOW);
  pushHistory(t, t.dist, now);

  // speed from whatever history exists (>= MIN_HIST) instead of waiting for a full buffer
  if (t.histCount >= MIN_HIST) {
    int last = t.histCount - 1;
    float dt = (t.histT[last] - t.histT[0]) / 1000.0f;
    if (dt > 0) t.speed = (t.histD[0] - t.histD[last]) / dt;
  }
  classify(t);
}

void drawLcd(const Tracker& t) {
  if (!lcd) return;
  char l1[24], l2[24], pad1[24];
  if (t.state == NO_TARGET) {
    snprintf(l1, sizeof(l1), "NO TARGET");
  } else if (t.ttc >= 0) {
    snprintf(l1, sizeof(l1), "D:%03dcm TTC:%.1f", (int)t.dist, t.ttc);
  } else {
    snprintf(l1, sizeof(l1), "D:%03dcm TTC:--", (int)t.dist);
  }
  snprintf(pad1, sizeof(pad1), "%-16s", l1);
  snprintf(l2, sizeof(l2), "%-16s", stateText[t.state]);
  lcd->setCursor(0, 0);
  lcd->print(pad1);
  lcd->setCursor(0, 1);
  lcd->print(l2);
}

void setBuzzer(bool on) {
  if (BUZZER_PASSIVE) {
    if (on) tone(PIN_BUZZER, 2000);
    else noTone(PIN_BUZZER);
  } else {
    digitalWrite(PIN_BUZZER, on);
  }
}

void driveOutputs(State s) {
  digitalWrite(LED_RED_0, s == HIGH_RISK);
  digitalWrite(LED_RED_13, s == HIGH_RISK);
  digitalWrite(LED_BLUE, s == MONITOR);

  static unsigned long lastToggle = 0;
  static bool buzzOn = false;
  if (s == HIGH_RISK && !muted) {
    if (millis() - lastToggle > 150) {
      buzzOn = !buzzOn;
      lastToggle = millis();
    }
  } else {
    buzzOn = false;
  }
  setBuzzer(buzzOn);
}

void handleButton() {
  static bool last = HIGH;
  static unsigned long lastChange = 0;
  bool now = digitalRead(PIN_BTN);
  if (now != last && millis() - lastChange > 40) {
    lastChange = millis();
    last = now;
    if (now == LOW) muted = !muted;
  }
}

void trackLeadTime(State s) {
  static State prev = NO_TARGET;
  static unsigned long warnAt = 0;
  static bool irWasHit = false;

  if (s == HIGH_RISK && prev != HIGH_RISK) warnAt = millis();
  if (s == NO_APPROACH || s == NO_TARGET) warnAt = 0;
  prev = s;

  bool irHit = digitalRead(PIN_IR) == (IR_ACTIVE_LOW ? LOW : HIGH);
  if (irHit && !irWasHit) {
    if (warnAt) {
      Serial.printf("IR_HIT,lead_ms=%ld\n", (long)(millis() - warnAt));
    } else {
      Serial.println("IR_HIT,no_warning_before_contact");
    }
    warnAt = 0;
  }
  irWasHit = irHit;
}

uint8_t findLcdAddress() {
  const uint8_t candidates[] = {0x27, 0x3F};
  for (uint8_t a : candidates) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) return a;
  }
  return 0;
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_A, OUTPUT);
  pinMode(ECHO_A, INPUT);
  if (USE_B) {
    pinMode(TRIG_B, OUTPUT);
    pinMode(ECHO_B, INPUT);
  }
  pinMode(PIN_IR, INPUT_PULLUP); // pullup prevents floating 0 false triggers
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(LED_RED_0, OUTPUT);
  pinMode(LED_RED_13, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  // Power-on self-test: flash both RED and BLUE LEDs for 1.2s to confirm wiring
  digitalWrite(LED_RED_0, HIGH);
  digitalWrite(LED_RED_13, HIGH);
  digitalWrite(LED_BLUE, HIGH);
  delay(1200);
  digitalWrite(LED_RED_0, LOW);
  digitalWrite(LED_RED_13, LOW);
  digitalWrite(LED_BLUE, LOW);

  Wire.setTimeOut(30);
  Wire.begin(I2C_SDA, I2C_SCL);
  uint8_t addr = findLcdAddress();
  if (addr) {
    lcd = new LiquidCrystal_I2C(addr, 16, 2);
    lcd->init();
    lcd->backlight();
    lcd->setCursor(0, 0);
    lcd->print("SECONDS AHEAD");
  } else {
    Serial.println("LCD not found on 0x27 or 0x3F");
  }
  Serial.println("ms,dist,speed,ttc,state,ir,muted,side_dist");
  delay(500);
}

void loop() {
  static unsigned long lastLcd = 0;

  updateTracker(tracker);
  handleButton();
  driveOutputs(tracker.state);
  trackLeadTime(tracker.state);

  // Sample Perpendicular Lateral Side Sensor B
  delay(12); // Pacing between sensor pings
  float side = readSideSensor();
  if (side > 0) sideDist = side;
  else sideDist = 0.0f;

  if (millis() - lastLcd > 200) {
    lastLcd = millis();
    drawLcd(tracker);
  }

  Serial.printf("%lu,%.1f,%.1f,%.2f,%s,%d,%d,%.1f\n",
                millis(), tracker.dist, tracker.speed, tracker.ttc,
                stateText[tracker.state], digitalRead(PIN_IR), muted, sideDist);

  delay(20); // Steady ~20-25 Hz telemetry rate
}
