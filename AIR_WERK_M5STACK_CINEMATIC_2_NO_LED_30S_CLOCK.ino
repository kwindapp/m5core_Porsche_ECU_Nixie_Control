#include <Arduino.h>
#include <M5Stack.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include <string.h>

// AIR WERK graphical controller for ORIGINAL M5Stack Core (non-touch)
// ESP32 GPIO17 TX -> Mega RX1 pin 19; GND -> GND.
// Mega TX1 must NOT connect directly to ESP32 RX (5V vs 3.3V).
HardwareSerial NixieSerial(2);
const int NIXIE_TX = 17, NIXIE_RX = 16;
const uint32_t NIXIE_BAUD = 9600;

const char* items[] = {"NIXIE CINEMATIC", "RTC CLOCK", "SHOW 911930", "LED ON / OFF", "LED COLOR", "LED BRIGHTNESS", "LED FADE DEMO", "NIXIE BRIGHTNESS", "STARTUP REPLAY", "BUZZER ON / OFF", "BUZZER TEST", "MEGA STATUS", "STOPWATCH MODE", "STOPWATCH VIEW", "STOPWATCH START", "STOPWATCH PAUSE", "STOPWATCH RESET", "BUZZER TONE", "ECU LIVE DATA", "NIXIE RPM", "NIXIE BATTERY", "NIXIE ENGINE TEMP", "NIXIE THROTTLE", "NIXIE PEDAL", "ECU DISPLAY OFF"};
const uint8_t ITEM_COUNT = sizeof(items) / sizeof(items[0]);
const char* colors[] = {"AMBER", "RED", "GREEN", "BLUE", "WHITE", "PURPLE"};
uint8_t selected = 0, color = 0, brightness = 25, nixieBrightness = 100;
bool buzzerOn = true;
String megaReply = "NO RX FEEDBACK";
bool ledOn = false, randomActive = false, fadeActive = false, nixieEdit = false;
bool stopwatchMode = false, stopwatchRunning = false, stopwatchView = false;
uint32_t stopwatchStarted = 0, stopwatchElapsed = 0, stopwatchLastDraw = 0, stopwatchLastTx = 0;
uint32_t randomStart = 0, lastFrame = 0, lastFade = 0, txCount = 0;
uint8_t fadeValue = 0;
int8_t fadeDirection = 1;
String nixie = "------";
const uint32_t RANDOM_TIME = 4800, FRAME_TIME = 85;
const uint32_t CINEMATIC_CLOCK_RETURN_MS = 30000UL; // From animation start

// Exact ESP-NOW packet from Waveshare Trijekt sender: 19 bytes, packed.
struct __attribute__((packed)) DashPacket {
  uint16_t rpm;
  float batt, motor, dk, gp;
  uint8_t funk;
};
static_assert(sizeof(DashPacket) == 19, "Unexpected Trijekt ESP-NOW packet size");
static constexpr uint8_t ECU_CHANNEL = 1;
static constexpr uint32_t ECU_TIMEOUT_MS = 2000;
portMUX_TYPE ecuMux = portMUX_INITIALIZER_UNLOCKED;
DashPacket receivedECU{};
uint32_t receivedAt = 0;
uint32_t receivedCount = 0;
bool receivedOnce = false;
bool ecuView = false;
uint8_t ecuViewPage = 0;
uint8_t ecuDisplay = 0; // 0 none, 1 RPM, 2 battery, 3 engine temp, 4 throttle, 5 pedal
uint32_t lastECUDisplayMs = 0;
uint32_t lastECUDrawMs = 0;
bool espNowReady = false;

void acceptECUPacket(const uint8_t* data, int len) {
  if (len != sizeof(DashPacket)) return;
  DashPacket incoming;
  memcpy(&incoming, data, sizeof(incoming));
  portENTER_CRITICAL(&ecuMux);
  receivedECU = incoming;
  receivedAt = millis();
  receivedCount++;
  receivedOnce = true;
  portEXIT_CRITICAL(&ecuMux);
}
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void ecuReceiveCallback(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  (void)info;
  acceptECUPacket(data, len);
}
#else
void ecuReceiveCallback(const uint8_t* mac, const uint8_t* data, int len) {
  (void)mac;
  acceptECUPacket(data, len);
}
#endif
bool readECU(DashPacket& out, uint32_t& age, uint32_t& packets) {
  uint32_t stamp;
  bool seen;
  portENTER_CRITICAL(&ecuMux);
  out = receivedECU;
  stamp = receivedAt;
  packets = receivedCount;
  seen = receivedOnce;
  portEXIT_CRITICAL(&ecuMux);
  age = millis() - stamp;
  return seen && age <= ECU_TIMEOUT_MS;
}
void initECUNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_err_t channelResult = esp_wifi_set_channel(ECU_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (channelResult != ESP_OK) {
    Serial.printf("ESP-NOW channel error %d\n", (int)channelResult);
    return;
  }
  esp_err_t result = esp_now_init();
  if (result != ESP_OK) {
    Serial.printf("ESP-NOW init error %d\n", (int)result);
    return;
  }
  result = esp_now_register_recv_cb(ecuReceiveCallback);
  espNowReady = result == ESP_OK;
  Serial.printf("ESP-NOW receive %s, channel %u\n", espNowReady ? "READY" : "FAILED", ECU_CHANNEL);
}
void drawECUView() {
  DashPacket d; uint32_t age, packets;
  bool live = readECU(d, age, packets);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(ORANGE, BLACK);
  M5.Lcd.setTextSize(3); M5.Lcd.setCursor(12, 8); M5.Lcd.print("TRIJEKT ECU");
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(live ? GREEN : RED, BLACK);
  M5.Lcd.setCursor(12, 44);
  M5.Lcd.print(live ? "LIVE ESP-NOW" : "NO LIVE DATA");
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setTextSize(2);
  if (live) {
    if (ecuViewPage == 0) {
      M5.Lcd.setCursor(12, 79); M5.Lcd.printf("RPM:       %u", d.rpm);
      M5.Lcd.setCursor(12, 108); M5.Lcd.printf("Battery:   %.2f V", d.batt);
      M5.Lcd.setCursor(12, 137); M5.Lcd.printf("Engine:    %.1f C", d.motor);
    } else {
      M5.Lcd.setCursor(12, 79); M5.Lcd.printf("Throttle:  %.1f %%", d.dk);
      M5.Lcd.setCursor(12, 108); M5.Lcd.printf("Pedal:     %.1f %%", d.gp);
      M5.Lcd.setCursor(12, 137); M5.Lcd.printf("Funk:      %u", d.funk);
    }
  } else {
    M5.Lcd.setCursor(12, 93); M5.Lcd.print("Check sender + CH 1");
  }
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(12, 185);
  if (packets) M5.Lcd.printf("Packets: %lu  Age: %lu ms", (unsigned long)packets, (unsigned long)age);
  else M5.Lcd.print("Waiting for 19-byte broadcast...");
  M5.Lcd.setCursor(12, 218); M5.Lcd.print("A/C: PAGE    B: BACK");
}
// Forward declarations for PlatformIO C++ compilation.
void sendCmd(const String& cmd);
void sendDisplay(const String& digits);

void updateECUNixie() {
  if (!ecuDisplay || randomActive) return;
  if (millis() - lastECUDisplayMs < 250) return;
  lastECUDisplayMs = millis();
  DashPacket d; uint32_t age, packets;
  if (!readECU(d, age, packets)) {
    // Never display a stale engine measurement as live data.
    if (nixie != "------") { sendCmd("MODE:CLOCK"); nixie = "------"; }
    return;
  }
  float v = 0;
  switch (ecuDisplay) {
    case 1: v = d.rpm; break;
    case 2: v = d.batt * 100.0f; break;    // 12.34V => 001234
    case 3: v = d.motor * 10.0f; break;   // 85.6C => 000856
    case 4: v = d.dk * 10.0f; break;      // 98.1% => 000981
    case 5: v = d.gp * 10.0f; break;
  }
  if (!isfinite(v) || v < 0 || v > 999999) return;
  char digits[7];
  snprintf(digits, sizeof(digits), "%06lu", (unsigned long)(v + 0.5f));
  if (nixie != digits) sendDisplay(String(digits));
}

void sendCmd(const String& cmd) {
  NixieSerial.println(cmd);
  Serial.print("UART TX: "); Serial.println(cmd);
  ++txCount;
}
void sendDisplay(const String& digits) {
  nixie = digits;
  sendCmd("DISPLAY:" + digits);
}
// Stopwatch uses MMSSCC (minutes, seconds, centiseconds), maximum 99:59.99.
uint32_t stopwatchTimeMs() {
  return stopwatchElapsed + (stopwatchRunning ? millis() - stopwatchStarted : 0);
}
void updateStopwatch() {
  if (!stopwatchMode || randomActive || ecuDisplay) return;
  uint32_t now = millis();
  if (now - stopwatchLastTx < 40) return; // 25 updates/second, no UART flooding
  stopwatchLastTx = now;
  uint32_t cs = (stopwatchTimeMs() / 10) % 600000UL;
  uint32_t mm = cs / 6000UL, ss = (cs / 100UL) % 60UL, cc = cs % 100UL;
  char digits[7];
  snprintf(digits, sizeof(digits), "%02lu%02lu%02lu", (unsigned long)mm, (unsigned long)ss, (unsigned long)cc);
  if (nixie != digits) sendDisplay(String(digits));
}
void drawStopwatch() {
  uint32_t cs = (stopwatchTimeMs() / 10) % 600000UL;
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(ORANGE, BLACK);
  M5.Lcd.setTextSize(3); M5.Lcd.setCursor(15, 12); M5.Lcd.print("AIR WERK");
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setTextSize(2); M5.Lcd.setCursor(15, 62); M5.Lcd.print("STOPWATCH");
  M5.Lcd.setTextSize(4); M5.Lcd.setCursor(13, 110);
  M5.Lcd.printf("%02lu:%02lu.%02lu", (unsigned long)(cs/6000), (unsigned long)((cs/100)%60), (unsigned long)(cs%100));
  M5.Lcd.setTextSize(1); M5.Lcd.setCursor(15, 186);
  M5.Lcd.print(stopwatchRunning ? "RUNNING" : "PAUSED");
  M5.Lcd.setCursor(12, 220); M5.Lcd.print("A:START/PAUSE B:RESET C:BACK");
}
void stopStopwatch() {
  if (stopwatchRunning) stopwatchElapsed += millis() - stopwatchStarted;
  stopwatchRunning = false;
}
void activateStopwatch() {
  randomActive = false; ecuDisplay = 0;
  stopwatchMode = true;
  sendDisplay("000000");
}

void drawMenu() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextSize(3);
  M5.Lcd.setTextColor(ORANGE, BLACK);
  M5.Lcd.setCursor(16, 9); M5.Lcd.print("AIR WERK");
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setCursor(15, 47); M5.Lcd.print("NIXIE "); M5.Lcd.print(nixie);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(15, 74);
  M5.Lcd.printf("LED:%s %s %u%%  TUBE:%u%%", ledOn ? "ON" : "OFF", colors[color], brightness, nixieBrightness);
  M5.Lcd.setCursor(15, 89);
  M5.Lcd.print(randomActive ? "ANIMATION RUNNING" : fadeActive ? "LED FADE RUNNING" : ecuDisplay ? "ECU -> NIXIE (LIVE ONLY)" : megaReply.substring(0, 43));
  // Four visible entries, selected entry always in view.
  uint8_t first = (selected / 4) * 4;
  for (uint8_t i = first; i < ITEM_COUNT && i < first + 4; ++i) {
    int y = 111 + (i - first) * 26;
    if (i == selected) {
      M5.Lcd.fillRoundRect(8, y - 3, 304, 24, 5, ORANGE);
      M5.Lcd.setTextColor(BLACK, ORANGE);
    } else M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.setTextSize(2); M5.Lcd.setCursor(14, y); M5.Lcd.print(items[i]);
  }
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setTextSize(1); M5.Lcd.setCursor(13, 224);
  M5.Lcd.print("A: UP    B: SELECT    C: DOWN");
}
void drawNixieBrightness() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(ORANGE, BLACK);
  M5.Lcd.setTextSize(3);
  M5.Lcd.setCursor(18, 14); M5.Lcd.print("AIR WERK");
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(18, 65); M5.Lcd.print("NIXIE BRIGHTNESS");
  M5.Lcd.setTextSize(4);
  M5.Lcd.setCursor(90, 105); M5.Lcd.printf("%3u%%", nixieBrightness);
  M5.Lcd.drawRect(18, 161, 284, 21, WHITE);
  M5.Lcd.fillRect(21, 164, (278 * nixieBrightness) / 100, 15, ORANGE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(12, 211); M5.Lcd.print("A: -   B: +   C: BACK");
}
// Persistent settings in ESP32 NVS (written only on user changes).
Preferences airWerkPrefs;
void loadAirWerkSettings() {
  airWerkPrefs.begin("airwerk", false);
  ledOn = airWerkPrefs.getBool("led", false);
  color = airWerkPrefs.getUChar("color", 0);
  if (color >= 6) color = 0;
  brightness = airWerkPrefs.getUChar("ledbright", 25);
  if (brightness > 100) brightness = 25;
  nixieBrightness = airWerkPrefs.getUChar("tube", 100);
  if (nixieBrightness > 100) nixieBrightness = 100;
  buzzerOn = airWerkPrefs.getBool("buzzer", true);
}
void saveAirWerkSettings() {
  airWerkPrefs.putBool("led", ledOn);
  airWerkPrefs.putUChar("color", color);
  airWerkPrefs.putUChar("ledbright", brightness);
  airWerkPrefs.putUChar("tube", nixieBrightness);
  airWerkPrefs.putBool("buzzer", buzzerOn);
}
void restoreAirWerkOutputs() {
  // Send settings first; switch LED on only after color/brightness restored.
  sendCmd("LED:OFF");
  sendCmd(String("LED:COLOR:") + color);
  sendCmd(String("LED:BRIGHT:") + brightness);
  sendCmd(String("NIXIE:BRIGHT:") + nixieBrightness);
  sendCmd(buzzerOn ? "BUZZ:ON" : "BUZZ:OFF");
  if (ledOn) sendCmd("LED:ON");
  else sendCmd("LED:OFF");
}

void changeNixieBrightness(int delta) {
  int value = (int)nixieBrightness + delta;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  nixieBrightness = (uint8_t)value;
  saveAirWerkSettings();
  sendCmd(String("NIXIE:BRIGHT:") + nixieBrightness);
  drawNixieBrightness();
}
// Cinematic 2: Nixie tubes only. Never switch RGB LEDs on.
void endAnimationLights() {
  sendCmd("LED:OFF");
  ledOn = false;
}

void startRandom() {
  fadeActive = false;
  ledOn = false;
  randomActive = true;
  randomStart = millis();
  sendCmd("LED:OFF");
  // No LED effects during cinematic animation.
  lastFrame = millis() - FRAME_TIME;
  nixie = "ANIM";
  sendDisplay("000000");
  drawMenu();
}
void selectItem() {
  switch (selected) {
    case 0: stopwatchMode=false; stopStopwatch(); ecuDisplay=0; startRandom(); return;
    case 1: stopwatchMode=false; stopStopwatch(); ecuDisplay=0; randomActive = false; endAnimationLights(); nixie = "CLOCK"; sendCmd("MODE:CLOCK"); break;
    case 2: stopwatchMode=false; stopStopwatch(); ecuDisplay=0; randomActive = false; endAnimationLights(); sendDisplay("911930"); break;
    case 3: fadeActive = false; ledOn = !ledOn; sendCmd(ledOn ? "LED:ON" : "LED:OFF"); saveAirWerkSettings(); break;
    case 4: fadeActive = false; color = (color + 1) % 6; ledOn = true; sendCmd(String("LED:COLOR:") + color); saveAirWerkSettings(); break;
    case 5: fadeActive = false; brightness = brightness >= 100 ? 0 : brightness + 10;
            ledOn = true; sendCmd("LED:ON");
            sendCmd(String("LED:BRIGHT:") + brightness); saveAirWerkSettings(); break;
    case 6:
      fadeActive = !fadeActive;
      if (fadeActive) {
        // Fade must explicitly wake the Mega LEDs after LED:OFF.
        ledOn = true;
        sendCmd("LED:ON");
        // Start at zero for a clearly visible fade-in.
        fadeValue = 0;
        fadeDirection = 1;
        brightness = 0;
        sendCmd("LED:BRIGHT:0");
      } else {
        brightness = fadeValue;
        saveAirWerkSettings(); // Save only when user stops fade, not each frame.
      }
      lastFade = millis();
      break;
    case 7:
      // Requires matching Mega firmware support for NIXIE:BRIGHT.
      // Requires AIR_WERK_MEGA_NIXIE_BRIGHTNESS_UART.ino on Mega.
      nixieEdit = true;
      drawNixieBrightness();
      return;
    case 8: stopwatchMode=false; stopStopwatch(); ecuDisplay=0; randomActive = false; endAnimationLights(); nixie = "START"; sendCmd("STARTUP:PLAY"); break;
    case 9: buzzerOn = !buzzerOn; sendCmd(buzzerOn ? "BUZZ:ON" : "BUZZ:OFF"); saveAirWerkSettings(); break;
    case 10: sendCmd("BUZZ:BEEP"); break;
    case 11: sendCmd("PING"); sendCmd("NIXIE:STATUS"); sendCmd("BUZZ:STATUS"); sendCmd("LED:STATUS"); break;
    case 12: activateStopwatch(); stopwatchView=true; drawStopwatch(); return;
    case 13: activateStopwatch(); stopwatchView=true; drawStopwatch(); return;
    case 14: if (!stopwatchMode) activateStopwatch();
             if (!stopwatchRunning) { stopwatchStarted=millis(); stopwatchRunning=true; } break;
    case 15: stopStopwatch(); break;
    case 16: stopStopwatch(); stopwatchElapsed=0;
             if (!stopwatchMode) activateStopwatch();
             sendDisplay("000000"); break;
    case 17: sendCmd("BUZZ:TONE:1500,250"); break;
    case 18: ecuView = true; ecuViewPage = 0; drawECUView(); return;
    case 19: case 20: case 21: case 22: case 23:
      stopwatchMode=false; stopStopwatch(); randomActive = false;
      ecuDisplay = selected - 18;
      nixie = "------"; // force first valid update
      break;
    case 24: stopwatchMode=false; stopStopwatch(); ecuDisplay = 0; nixie = "CLOCK"; sendCmd("MODE:CLOCK"); break;
  }
  drawMenu();
}
void setup() {
  M5.begin();
  Serial.begin(115200);
  NixieSerial.begin(NIXIE_BAUD, SERIAL_8N1, NIXIE_RX, NIXIE_TX);
  randomSeed(esp_random());
  loadAirWerkSettings();
  // RGB LEDs are always OFF outside explicit user actions/animation.
  ledOn = false;
  saveAirWerkSettings();
  initECUNow();
  delay(250);
  // Restore last saved user settings (first boot defaults to LED OFF).
  restoreAirWerkOutputs();
  sendCmd("LED:STATUS");
  sendCmd("NIXIE:STATUS");
  // A delayed OFF handles a Mega that boots after the ESP32.
  sendCmd("LED:OFF");
  drawMenu();
}
void loop() {
  // Repeat LED:OFF briefly at boot: Mega may reset or run startup LEDs later.
  static uint32_t lastBootLedOff = 0;
  if (millis() < 5000 && !ledOn && !randomActive && millis() - lastBootLedOff >= 500) {
    lastBootLedOff = millis();
    sendCmd("LED:OFF");
  }
  // UART feedback requires Mega TX1 -> a 5V-to-3.3V level shifter -> ESP32 GPIO16.
  static char rxLine[80]; static uint8_t rxLen=0;
  while (NixieSerial.available()) {
    char c=(char)NixieSerial.read();
    if (c=='\r') continue;
    if (c=='\n') {
      rxLine[rxLen]=0;
      if(rxLen) { megaReply=rxLine; Serial.print("MEGA RX: "); Serial.println(megaReply); }
      rxLen=0;
    } else if(rxLen<sizeof(rxLine)-1) rxLine[rxLen++]=c;
    else rxLen=0;
  }
  static unsigned long lastStatusDraw=0;
  if (millis()-lastStatusDraw>700 && !nixieEdit && !fadeActive && !randomActive) {
    lastStatusDraw=millis();
    M5.Lcd.fillRect(12,87,300,17,BLACK);
    M5.Lcd.setTextSize(1); M5.Lcd.setTextColor(WHITE,BLACK);
    M5.Lcd.setCursor(15,89); M5.Lcd.print(ecuDisplay ? "ECU -> NIXIE (LIVE ONLY)" : megaReply.substring(0,43));
  }
  M5.update();
  if (stopwatchView) {
    if (M5.BtnA.wasPressed()) {
      if (stopwatchRunning) stopStopwatch();
      else { stopwatchStarted=millis(); stopwatchRunning=true; }
      drawStopwatch();
    }
    if (M5.BtnB.wasPressed()) {
      stopStopwatch(); stopwatchElapsed=0; sendDisplay("000000"); drawStopwatch();
    }
    if (M5.BtnC.wasPressed()) { stopwatchView=false; drawMenu(); }
    updateStopwatch();
    if (stopwatchView && millis()-stopwatchLastDraw >= 100) {
      stopwatchLastDraw=millis(); drawStopwatch();
    }
    delay(5); return;
  }
  if (ecuView) {
    if (M5.BtnA.wasPressed()) { ecuViewPage ^= 1; drawECUView(); }
    if (M5.BtnC.wasPressed()) { ecuViewPage ^= 1; drawECUView(); }
    if (M5.BtnB.wasPressed()) { ecuView = false; drawMenu(); }
    if (ecuView && millis() - lastECUDrawMs >= 500) {
      lastECUDrawMs = millis(); drawECUView();
    }
    updateECUNixie();
    delay(5); return;
  }
  if (nixieEdit) {
    if (M5.BtnA.wasPressed()) changeNixieBrightness(-10);
    if (M5.BtnB.wasPressed()) changeNixieBrightness(+10);
    if (M5.BtnC.wasPressed()) { nixieEdit = false; drawMenu(); }
    delay(5);
    return;
  }
  if (M5.BtnA.wasPressed()) { selected = (selected + ITEM_COUNT - 1) % ITEM_COUNT; drawMenu(); }
  if (M5.BtnC.wasPressed()) { selected = (selected + 1) % ITEM_COUNT; drawMenu(); }
  if (M5.BtnB.wasPressed()) selectItem();

  uint32_t now = millis();
  if (randomActive) {
    const uint32_t elapsed = now - randomStart;
    if (elapsed >= CINEMATIC_CLOCK_RETURN_MS) {
      randomActive = false;
      endAnimationLights();
      nixie = "CLOCK";
      sendCmd("MODE:CLOCK");
      drawMenu();
    } else if (elapsed >= RANDOM_TIME) {
      // Hold 911930 steadily until 30 seconds have elapsed.
      if (nixie != "911930") { sendDisplay("911930"); drawMenu(); }
    } else if (now - lastFrame >= FRAME_TIME) {
      lastFrame = now;
      // LEDs stay OFF; Nixie digits animate independently.
      // Cinematic reveal: outer tubes -> middle pair -> center pair.
      // Numeric-only frames are compatible with the Mega DISPLAY:dddddd protocol.
      // The final two seconds progressively lock each digit of 911930.
      const char target[7] = "911930";
      char digits[7] = "000000";
      if (elapsed < 800) {
        // Phase 1: outermost tubes spin in opposite directions.
        uint8_t step = elapsed / 85;
        digits[0] = '0' + (step % 10);
        digits[5] = '0' + ((10 - step % 10) % 10);
      } else if (elapsed < 1550) {
        // Phase 2: second pair joins the rolling wave.
        uint8_t step = (elapsed - 800) / 85;
        digits[0] = '0' + ((step + 3) % 10);
        digits[5] = '0' + ((step + 7) % 10);
        digits[1] = '0' + ((step + 5) % 10);
        digits[4] = '0' + ((step + 2) % 10);
      } else if (elapsed < 2450) {
        // Phase 3: all six tubes spin, accelerating toward the center.
        uint8_t step = (elapsed - 1550) / 65;
        for (uint8_t i = 0; i < 6; ++i)
          digits[i] = '0' + ((step * (i % 2 ? 3 : 7) + i * 3) % 10);
      } else if (elapsed < 3900) {
        // Phase 4: lock from the outside inward: 9____0, 91__30, 911930.
        uint8_t step = (elapsed - 2450) / 85;
        for (uint8_t i = 0; i < 6; ++i)
          digits[i] = '0' + ((step * (i % 2 ? 3 : 7) + i * 3) % 10);
        digits[0] = target[0]; digits[5] = target[5];
        if (elapsed >= 2900) { digits[1] = target[1]; digits[4] = target[4]; }
        if (elapsed >= 3450) { digits[2] = target[2]; digits[3] = target[3]; }
      } else {
        // Phase 5: solid final signature, no flicker.
        memcpy(digits, target, sizeof(digits));
      }
      if (nixie != digits) sendDisplay(String(digits));
    }
  }
  if (fadeActive && now - lastFade >= 35) {
    lastFade = now;
    int next = (int)fadeValue + (int)fadeDirection * 2;
    if (next >= 100) { next = 100; fadeDirection = -1; }
    if (next <= 0) { next = 0; fadeDirection = 1; }
    fadeValue = (uint8_t)next;
    brightness = fadeValue;
    sendCmd(String("LED:BRIGHT:") + fadeValue);
    // Update the brightness value on screen without clearing/flickering.
    M5.Lcd.fillRect(12, 72, 300, 31, BLACK);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.setCursor(15, 74);
    M5.Lcd.printf("LED:ON %s %u%%  TUBE:%u%%", colors[color], brightness, nixieBrightness);
    M5.Lcd.setCursor(15, 89);
    M5.Lcd.print("LED FADE RUNNING");
  }
  updateECUNixie();
  updateStopwatch();
  delay(5);
}
