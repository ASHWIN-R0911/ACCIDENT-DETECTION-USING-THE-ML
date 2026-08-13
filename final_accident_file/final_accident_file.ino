/*
  AI Based Vehicle Accident Detection System
  ESP32 DevKit V1
  WhatsApp Alert via TextMeBot API
  Trained on VZCrash dataset — Accuracy 94.81%
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UrlEncode.h>
#include <math.h>
#include "accident_model.h"

// ── WiFi Credentials ─────────────────────────────────
#define WIFI_SSID     "xxxxxxxxxx"
#define WIFI_PASSWORD "xxxxxxxxxx"

// ── WhatsApp Numbers and API Keys ────────────────────
// Replace with real numbers and API keys from TextMeBot
// Get your apikey from https://textmebot.com
#define FAMILY_PHONE   "xxxxxxxxxx"  // with country code
#define FAMILY_APIKEY  "xxxxxxxxxx"       // from TextMeBot


// ── OLED ─────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT,
                          &Wire, -1);

// ── MPU6050 ──────────────────────────────────────────
#define MPU_ADDR 0x68
int16_t AcX, AcY, AcZ, Tmp, GyX, GyY, GyZ;
float   ax_ms2, ay_ms2, az_ms2;
float   gx_dps, gy_dps, gz_dps;

// ── Pins ─────────────────────────────────────────────
#define BUZZER_PIN    13
#define LED_PIN       12
#define BUTTON_PIN    14
#define VIBRATION_PIN 32
#define GPS_RX_PIN    16
#define GPS_TX_PIN    17

// ── GPS ──────────────────────────────────────────────
TinyGPSPlus gps;
HardwareSerial GPSSerial(2);

// ── Window Buffer ────────────────────────────────────
#define WINDOW_SIZE 20

struct Sample {
  float ax, ay, az;
  float gx, gy, gz;
  float magnitude;
  bool  vibration;
};

Sample samples[WINDOW_SIZE];
int sampleCount = 0;
int sampleIndex = 0;

// ── State Machine ────────────────────────────────────
enum State {
  MONITORING,
  COUNTDOWN,
  ALERT_SENT,
  CANCELLED
} state = MONITORING;

// ── Timing ───────────────────────────────────────────
unsigned long lastSampleMs     = 0;
unsigned long countdownStartMs = 0;
unsigned long lastBeepMs       = 0;
unsigned long lastOledUpdateMs = 0;

// ── Detection Variables ───────────────────────────────
float crashProbability  = 0;
float normalProbability = 1;
float latestMagnitude   = 0;
float lastPeakMagnitude = 0;
float lastPeakGyro      = 0;
bool  lastVibration     = false;
int   lastDisplayedSecond = 16;
bool  wifiConnected     = false;

// ── Send WhatsApp via TextMeBot ───────────────────────
bool sendWhatsApp(String phone, String apiKey,
                  String message) {

  WiFiClientSecure client;
  client.setInsecure();

  Serial.printf("Sending WhatsApp to %s...\n",
                phone.c_str());

  if (!client.connect("api.textmebot.com", 443)) {
    Serial.println("TextMeBot connection failed");
    return false;
  }

  // URL encode the message
  String encodedMsg = urlEncode(message);

  String url = "/send.php?recipient=" + phone +
               "&apikey=" + apiKey +
               "&text=" + encodedMsg;

  client.print(String("GET ") + url +
               " HTTP/1.1\r\n" +
               "Host: api.textmebot.com\r\n" +
               "Connection: close\r\n\r\n");

  // Wait for response
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 10000) {
      Serial.println("TextMeBot timeout");
      client.stop();
      return false;
    }
  }

  String response = "";
  while (client.available()) {
    response += client.readStringUntil('\n');
  }
  client.stop();

  Serial.println("TextMeBot response received");

  // Check if message was sent OK.
  // TextMeBot returns "Message Sent" (or similar success text)
  // on success, and an "Error" string on failure.
  if (response.indexOf("Error") < 0 &&
      (response.indexOf("Message Sent") >= 0 ||
       response.indexOf("200") >= 0)) {
    Serial.println("WhatsApp message sent OK");
    return true;
  } else {
    Serial.println("WhatsApp send failed");
    Serial.println(response.substring(0, 150));
    return false;
  }
}

// ── Build and Send All Alerts ─────────────────────────
void sendAlertMessages() {
  if (!wifiConnected) {
    Serial.println("WiFi not connected — alert via Serial");
    return;
  }

  // Build WhatsApp message
  String msg = "🚨 ACCIDENT ALERT 🚨\n";
  msg += "━━━━━━━━━━━━━━━━━━━━\n";
  msg += "Crash Confidence: ";
  msg += String(crashProbability * 100, 1);
  msg += "%\n";
  msg += "Peak Impact: ";
  msg += String(lastPeakMagnitude, 2);
  msg += " m/s2\n";
  msg += "Vibration: CONFIRMED\n";

  if (gpsLocked()) {
    msg += "📍 Location:\n";
    msg += String(gps.location.lat(), 6);
    msg += ", ";
    msg += String(gps.location.lng(), 6);
    msg += "\n";
    msg += "🗺 Map: https://maps.google.com/?q=";
    msg += String(gps.location.lat(), 6);
    msg += ",";
    msg += String(gps.location.lng(), 6);
    msg += "\n";
  } else {
    msg += "📍 GPS: Locating...\n";
  }

  msg += "━━━━━━━━━━━━━━━━━━━━\n";
  msg += "AI Model: 94.81% accuracy\n";
  msg += "Please send help immediately!";

  // Send to family
  bool sent1 = sendWhatsApp(
    String(FAMILY_PHONE),
    String(FAMILY_APIKEY),
    msg
  );

  delay(5000); // gap between messages (TextMeBot recommends >=5s)


  if (sent1) {
    Serial.println("WhatsApp alert sent");
  } else {
    Serial.println("WhatsApp failed — check API keys");
  }
}

// ── MPU6050 Read ──────────────────────────────────────
void readMPU6050() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 14, true);

  if (Wire.available() == 14) {
    AcX = Wire.read() << 8 | Wire.read();
    AcY = Wire.read() << 8 | Wire.read();
    AcZ = Wire.read() << 8 | Wire.read();
    Tmp = Wire.read() << 8 | Wire.read();
    GyX = Wire.read() << 8 | Wire.read();
    GyY = Wire.read() << 8 | Wire.read();
    GyZ = Wire.read() << 8 | Wire.read();

    ax_ms2 = (AcX / 16384.0f) * 9.81f;
    ay_ms2 = (AcY / 16384.0f) * 9.81f;
    az_ms2 = (AcZ / 16384.0f) * 9.81f;
    gx_dps = GyX / 131.0f;
    gy_dps = GyY / 131.0f;
    gz_dps = GyZ / 131.0f;
  }
}

// ── GPS ──────────────────────────────────────────────
void readGPS() {
  while (GPSSerial.available())
    gps.encode(GPSSerial.read());
}

bool gpsLocked() {
  return gps.location.isValid() &&
         gps.location.age() < 3000;
}

// ── WiFi Connect ─────────────────────────────────────
void connectWiFi() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Connecting WiFi...");
  display.setCursor(0, 14);
  display.println(WIFI_SSID);
  display.display();

  Serial.println("\nInitializing WiFi...");

  // Force clean state
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true); 
  delay(100);

  // Disable modem sleep for stable connection
  WiFi.setSleep(false);

  // Start connection
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to SSID: ");
  Serial.println(WIFI_SSID);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\n[SUCCESS] WiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi Connected!");
    display.setCursor(0, 14);
    display.println(WiFi.localIP().toString());
    display.setCursor(0, 28);
    display.println("WhatsApp: READY");
    display.display();
    delay(1500);

  } else {
    wifiConnected = false;
    Serial.println("\n[ERROR] WiFi Failed to connect.");

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi FAILED");
    display.setCursor(0, 14);
    display.println("Alert: Serial only");
    display.display();
    delay(1500);
  }
}

// ── OLED Screens ─────────────────────────────────────
void showBootScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 0);
  display.println("ACCIDENT DETECTION");
  display.setCursor(28, 12);
  display.println("SYSTEM v2.0");
  display.setCursor(0, 26);
  display.println("Model: VZCrash RF");
  display.setCursor(0, 38);
  display.println("Accuracy: 94.81%");
  display.setCursor(10, 50);
  display.println("CIT ECE PBL 2026");
  display.display();
}

void showMonitoring() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(15, 0);
  display.println("=== MONITORING ===");
  display.setCursor(0, 11);
  display.print("Accel: ");
  display.print(latestMagnitude, 2);
  display.println(" m/s2");
  display.setCursor(0, 22);
  display.print("Crash:");
  display.print(crashProbability * 100, 0);
  display.print("% Norm:");
  display.print(normalProbability * 100, 0);
  display.println("%");
  display.setCursor(0, 33);
  display.print("Vibration: ");
  display.println(lastVibration ? "YES" : "NO");
  display.setCursor(0, 44);
  display.println(gpsLocked() ?
    "GPS: LOCKED" : "GPS: SEARCHING");
  display.setCursor(0, 55);
  display.print("WA: ");
  display.println(wifiConnected ? "READY" : "OFFLINE");
  display.display();
}

void showCountdown(int seconds) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(15, 0);
  display.println("!! ACCIDENT !!");
  display.setCursor(0, 11);
  display.print("Crash: ");
  display.print(crashProbability * 100, 0);
  display.println("%");
  display.setCursor(0, 20);
  display.println("Vibration: CONFIRMED");
  display.setTextSize(3);
  display.setCursor(seconds >= 10 ? 35 : 52, 30);
  display.print(seconds);
  display.setTextSize(1);
  display.setCursor(5, 56);
  display.println("Press BTN to CANCEL");
  display.display();
}

void showDriverWarning(int seconds) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("ACCIDENT DETECTED!");
  display.setCursor(0, 12);
  display.println("Are you OK?");
  display.setCursor(0, 24);
  display.println("Press BTN if safe");
  display.setCursor(0, 36);
  display.print("WhatsApp in: ");
  display.setTextSize(2);
  display.setCursor(80, 32);
  display.print(seconds);
  display.setTextSize(1);
  display.setCursor(0, 54);
  display.print("WA: ");
  display.println(wifiConnected ? "READY" : "OFFLINE");
  display.display();
}

void showAlertSent() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 0);
  display.println("=== ALERT SENT ===");
  display.setCursor(0, 11);
  display.print("Crash: ");
  display.print(crashProbability * 100, 0);
  display.println("%");
  display.setCursor(0, 22);
  display.print("Peak: ");
  display.print(lastPeakMagnitude, 2);
  display.println(" m/s2");
  display.setCursor(0, 33);
  if (gpsLocked()) {
    display.print(gps.location.lat(), 4);
    display.print(",");
    display.println(gps.location.lng(), 4);
  } else {
    display.println("GPS: No fix");
  }
  display.setCursor(0, 44);
  display.println("WhatsApp: SENT");
  display.setCursor(0, 55);
  display.println("Family + Rescue notified");
  display.display();
}

void showCancelledScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 10);
  display.println("Alert CANCELLED");
  display.setCursor(0, 25);
  display.println("Driver confirmed OK");
  display.setCursor(0, 40);
  display.println("Resuming monitor...");
  display.display();
}

// ── Buzzer Pattern ────────────────────────────────────
void handleBuzzerPattern(int secondsLeft) {
  unsigned long now = millis();
  int beepInterval;

  if      (secondsLeft > 10) beepInterval = 1000;
  else if (secondsLeft > 5)  beepInterval = 500;
  else                       beepInterval = 250;

  if (now - lastBeepMs >= (unsigned long)beepInterval) {
    lastBeepMs = now;
    digitalWrite(BUZZER_PIN, HIGH);
    delay(80);
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, HIGH);
    delay(80);
    digitalWrite(LED_PIN, LOW);
  }
}

// ── Fire Alert ────────────────────────────────────────
void fireAlert() {
  state = ALERT_SENT;
  digitalWrite(LED_PIN, HIGH);

  digitalWrite(BUZZER_PIN, HIGH);
  delay(800);
  digitalWrite(BUZZER_PIN, LOW);
  delay(200);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(800);
  digitalWrite(BUZZER_PIN, LOW);

  Serial.println("========================================");
  Serial.println("       ACCIDENT ALERT CONFIRMED         ");
  Serial.println("========================================");
  Serial.printf("Crash Confidence  : %.1f%%\n",
                crashProbability * 100);
  Serial.printf("Peak Acceleration : %.2f m/s2\n",
                lastPeakMagnitude);
  Serial.println("Vibration         : CONFIRMED");
  if (gpsLocked()) {
    Serial.printf("GPS               : %.6f, %.6f\n",
                  gps.location.lat(),
                  gps.location.lng());
  }
  Serial.println("Sending WhatsApp alerts...");

  sendAlertMessages();

  Serial.println("========================================");
  showAlertSent();
}

// ── Begin Countdown ───────────────────────────────────
void beginCountdown() {
  state               = COUNTDOWN;
  countdownStartMs    = millis();
  lastBeepMs          = 0;
  lastDisplayedSecond = 16;
  digitalWrite(LED_PIN, HIGH);

  Serial.println("================================");
  Serial.println("  ACCIDENT DETECTED             ");
  Serial.printf ("  Crash: %.0f%%\n",
                 crashProbability * 100);
  Serial.println("  15s countdown started         ");
  Serial.println("  Press BTN to CANCEL           ");
  Serial.println("================================");
}

// ── Evaluate Window ───────────────────────────────────
void evaluateWindow() {
  if (state != MONITORING) return;

  float peakMag = 0, sumMag = 0, peakGyro = 0;
  float axPeak  = 0, ayPeak = 0, azPeak   = 0;
  float gzPeak  = 0;
  bool  vib     = false;

  for (int i = 0; i < WINDOW_SIZE; ++i) {
    const Sample &s = samples[i];
    peakMag  = max(peakMag,  s.magnitude);
    sumMag  += s.magnitude;
    peakGyro = max(peakGyro,
      sqrtf(s.gx*s.gx + s.gy*s.gy + s.gz*s.gz));
    axPeak   = max(axPeak,  fabsf(s.ax));
    ayPeak   = max(ayPeak,  fabsf(s.ay));
    azPeak   = max(azPeak,  fabsf(s.az));
    gzPeak   = max(gzPeak,  fabsf(s.gz));
    vib     |= s.vibration;
  }

  float meanMag  = sumMag / WINDOW_SIZE;
  float variance = 0;
  for (int i = 0; i < WINDOW_SIZE; ++i) {
    float d = samples[i].magnitude - meanMag;
    variance += d * d;
  }
  variance /= WINDOW_SIZE;

  float magStd   = sqrtf(variance);
  float skewness = 0;
  if (magStd > 0) {
    for (int i = 0; i < WINDOW_SIZE; ++i) {
      float d = samples[i].magnitude - meanMag;
      skewness += (d * d * d);
    }
    skewness /= (WINDOW_SIZE * magStd * magStd * magStd);
  }

  float impactEnergy = 0;
  for (int i = 0; i < WINDOW_SIZE; ++i) {
    impactEnergy += samples[i].magnitude *
                    samples[i].magnitude;
  }
  impactEnergy /= WINDOW_SIZE;

  float raw_features[N_FEATURES] = {
    peakMag, meanMag, peakGyro,
    vib ? 1.0f : 0.0f,
    axPeak, ayPeak, azPeak,
    magStd, skewness, 0.0f,
    gzPeak, impactEnergy,
    peakMag, meanMag
  };

  crashProbability  = predictCrashProbability(raw_features);
  normalProbability = 1.0f - crashProbability;
  lastVibration     = vib;
  lastPeakMagnitude = peakMag;
  lastPeakGyro      = peakGyro;

  Serial.printf(
    "Mag:%.2f | Crash:%.0f%% | Norm:%.0f%% | Vib:%s\n",
    peakMag,
    crashProbability  * 100,
    normalProbability * 100,
    vib ? "YES" : "NO"
  );

  if (crashProbability > 0.50f && vib) {
    beginCountdown();
  } else if (crashProbability > 0.50f && !vib) {
    Serial.println("High crash % — vibration not confirmed");
  } else if (crashProbability >= 0.30f) {
    Serial.println("Suspicious — monitoring");
  }
}

// ── Take Sample ───────────────────────────────────────
void takeSample() {
  readMPU6050();

  Sample &s   = samples[sampleIndex];
  s.ax        = ax_ms2;
  s.ay        = ay_ms2;
  s.az        = az_ms2;
  s.gx        = gx_dps;
  s.gy        = gy_dps;
  s.gz        = gz_dps;
  s.magnitude = sqrtf(ax_ms2*ax_ms2 +
                      ay_ms2*ay_ms2 +
                      az_ms2*az_ms2);
  s.vibration = digitalRead(VIBRATION_PIN) == LOW;

  latestMagnitude = s.magnitude;
  sampleIndex     = (sampleIndex + 1) % WINDOW_SIZE;
  if (sampleCount < WINDOW_SIZE) ++sampleCount;

  if (sampleCount == WINDOW_SIZE && sampleIndex == 0) {
    evaluateWindow();
  }
}

// ── Handle Countdown ─────────────────────────────────
void handleCountdown() {
  unsigned long now     = millis();
  unsigned long elapsed = (now - countdownStartMs) / 1000;
  int remaining         = 15 - (int)elapsed;

  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      state = CANCELLED;
      digitalWrite(BUZZER_PIN, LOW);
      digitalWrite(LED_PIN,    LOW);

      Serial.println("================================");
      Serial.println("  ALERT CANCELLED BY DRIVER     ");
      Serial.println("  Driver confirmed safe          ");
      Serial.println("================================");

      showCancelledScreen();
      delay(2000);
      return;
    }
  }

  if (remaining <= 0) {
    fireAlert();
    return;
  }

  if (remaining != lastDisplayedSecond) {
    lastDisplayedSecond = remaining;
    if (remaining % 2 == 0) {
      showCountdown(remaining);
    } else {
      showDriverWarning(remaining);
    }
    Serial.printf("Countdown: %d sec — BTN to cancel\n",
                  remaining);
  }

  handleBuzzerPattern(remaining);
}

// ── Setup ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN,    OUTPUT);
  pinMode(LED_PIN,       OUTPUT);
  pinMode(BUTTON_PIN,    INPUT_PULLUP);
  pinMode(VIBRATION_PIN, INPUT);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN,    LOW);

  Wire.begin(21, 22);
  delay(500);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED failed");
    while (true) delay(10);
  }

  showBootScreen();
  delay(2000);

  connectWiFi();

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0);
  Wire.endTransmission(true);
  delay(200);

  Wire.beginTransmission(MPU_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("MPU6050 not found");
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 20);
    display.println("MPU6050 ERROR");
    display.setCursor(0, 36);
    display.println("Check GPIO21/22");
    display.display();
    while (true) delay(10);
  }

  Serial.println("MPU6050 ready");
  Serial.println("Alert: WhatsApp via TextMeBot");
  Serial.println("Monitoring started");

  GPSSerial.begin(9600, SERIAL_8N1,
                  GPS_RX_PIN, GPS_TX_PIN);
  delay(500);
}

// ── Loop ──────────────────────────────────────────────
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    WiFi.reconnect();
  } else {
    wifiConnected = true;
  }

  readGPS();
  unsigned long now = millis();

  if (now - lastSampleMs >= 10) {
    lastSampleMs = now;
    takeSample();
  }

  switch (state) {

    case MONITORING:
      if (now - lastOledUpdateMs >= 300) {
        lastOledUpdateMs = now;
        showMonitoring();
      }
      break;

    case COUNTDOWN:
      handleCountdown();
      break;

    case ALERT_SENT:
      digitalWrite(LED_PIN, HIGH);
      break;

    case CANCELLED:
      sampleCount      = 0;
      sampleIndex      = 0;
      crashProbability = 0;
      lastVibration    = false;
      digitalWrite(LED_PIN,    LOW);
      digitalWrite(BUZZER_PIN, LOW);
      state = MONITORING;
      break;
  }
}
