#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <time.h>
#define FASTLED_ALLOW_INTERRUPTS 0
#include <FastLED.h>

// ========================== НАСТРАИВАЕМЫЕ КОНСТАНТЫ ==========================
#if __has_include("secrets.h")
#include "secrets.h"
#else
const char* WIFI_SSID = "Wi - Fi";
const char* WIFI_PASS = "PASSWORD";
#endif

#define LED_PIN           D4
#define BUTTON_PIN        D2
#define MATRIX_WIDTH      16
#define MATRIX_HEIGHT     16
#define NUM_LEDS          (MATRIX_WIDTH * MATRIX_HEIGHT)
#define LED_TYPE          WS2812B
#define COLOR_ORDER       GRB

const uint8_t BUTTON_ACTIVE_LEVEL = HIGH;
const uint8_t BUTTON_PIN_MODE = INPUT;

const uint8_t LED_SUPPLY_VOLTS = 5;
const uint16_t LED_MAX_MILLIAMPS = 1300;

const bool FONT_MIRROR_X = false;
const bool FONT_MIRROR_Y = false;

const bool MATRIX_SERPENTINE = true;
const uint8_t MATRIX_START_CORNER = 3;     // 0=TL, 1=TR, 2=BR, 3=BL
const bool MATRIX_ROWS_DIRECTION = true;   // true=rows, false=columns

const int32_t UTC_OFFSET_MIN_SEC = -12L * 3600L;
const int32_t UTC_OFFSET_MAX_SEC = 14L * 3600L;
const int32_t UTC_OFFSET_SEC_DEFAULT = 3L * 3600L;
const long DST_OFFSET_SEC = 0;
const uint32_t NTP_RESYNC_MS = 10UL * 60UL * 1000UL;

const uint16_t FRAME_MIN_MS = 16;
const uint16_t FRAME_MAX_MS = 90;

// ============================== ГЛОБАЛЬНЫЕ ДАННЫЕ =============================
ESP8266WebServer server(80);
CRGB leds[NUM_LEDS];

enum DisplayMode : uint8_t {
  MODE_FIRE = 0,
  MODE_FIREPLACE,
  MODE_RAINBOW,
  MODE_CONFETTI,
  MODE_SNOW,
  MODE_MATRIX_RAIN,
  MODE_STARS,
  MODE_COLOR_WAVE,
  MODE_RUNNING_LIGHTS,
  MODE_STATIC_COLOR,
  MODE_NIGHT_LIGHT,
  MODE_CLOCK,
  MODE_DATE,
  MODE_SCROLL_TEXT,
  MODE_COLOR_SPIRAL,
  MODE_PLASMA,
  MODE_COUNT
};

const char* MODE_NAMES_RU[MODE_COUNT] = {
  "Огонь",
  "Камин",
  "Радуга",
  "Конфетти",
  "Снег",
  "Матричный дождь",
  "Звездное небо",
  "Перелив цвета",
  "Бегущие огни",
  "Статичный цвет",
  "Ночная подсветка",
  "Часы",
  "Дата",
  "Бегущая строка",
  "Цветная спираль",
  "Плазма"
};

struct Settings {
  uint32_t magic;
  uint8_t mode;
  uint8_t brightness;
  uint8_t speed;
  uint32_t color;
  uint8_t power;
  int32_t utcOffset;
  char scrollText[96];
  uint16_t crc;
};

Settings cfg;
const uint32_t SETTINGS_MAGIC = 0xBEEFA826;

bool wifiConnected = false;
bool ntpSynced = false;
uint32_t lastNtpSyncMs = 0;
uint32_t lastFrameMs = 0;
uint32_t lastClockBlinkMs = 0;
bool clockDots = true;
uint32_t lastWiFiCheckMs = 0;

bool btnRawState = HIGH;
bool btnStableState = HIGH;
uint32_t btnLastChangeMs = 0;
uint32_t btnPressStartMs = 0;
uint32_t btnLastReleaseMs = 0;
uint8_t clickCount = 0;
bool holdBrightnessActive = false;
int8_t brightnessHoldDir = 1;
uint32_t lastBrightnessStepMs = 0;
bool brightnessHoldPaused = false;
uint32_t brightnessHoldPauseStartMs = 0;

const uint16_t BUTTON_DEBOUNCE_MS = 45;
const uint16_t BUTTON_CLICK_MAX_MS = 500;
const uint16_t BUTTON_MULTI_CLICK_MS = 520;
const uint16_t BUTTON_LONG_PRESS_MS = 700;
const uint16_t BRIGHTNESS_HOLD_STEP_MS = 35;
const uint16_t BRIGHTNESS_HOLD_PAUSE_MS = 1000;
const uint8_t BRIGHTNESS_HOLD_STEP = 5;

uint8_t hueBase = 0;
uint8_t heat[MATRIX_WIDTH][MATRIX_HEIGHT];
uint8_t rainDrops[MATRIX_WIDTH][MATRIX_HEIGHT];
uint8_t snowMap[MATRIX_WIDTH][MATRIX_HEIGHT];
uint8_t starsMap[MATRIX_WIDTH][MATRIX_HEIGHT];
int16_t scrollOffset = MATRIX_WIDTH;
uint32_t lastScrollStepMs = 0;

// ============================== УТИЛИТЫ ======================================
uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  while (len--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; i++) {
      if (crc & 1) crc = (crc >> 1) ^ 0xA001;
      else crc >>= 1;
    }
  }
  return crc;
}

uint8_t clampU8(int v, int lo, int hi) {
  if (v < lo) return (uint8_t)lo;
  if (v > hi) return (uint8_t)hi;
  return (uint8_t)v;
}

int32_t clampI32(int32_t v, int32_t lo, int32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

String ipToString(IPAddress ip) {
  char buf[20];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return String(buf);
}

void sanitizeStoredText() {
  cfg.scrollText[sizeof(cfg.scrollText) - 1] = '\0';
  for (uint8_t i = 0; cfg.scrollText[i] != '\0'; i++) {
    char ch = cfg.scrollText[i];
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch < 32 || ch > 90) ch = ' ';
    cfg.scrollText[i] = ch;
  }
}

void sanitizeSettings() {
  cfg.mode = clampU8(cfg.mode, 0, MODE_COUNT - 1);
  cfg.brightness = clampU8(cfg.brightness, 1, 255);
  cfg.speed = clampU8(cfg.speed, 1, 255);
  cfg.color &= 0xFFFFFFUL;
  cfg.power = cfg.power ? 1 : 0;
  cfg.utcOffset = clampI32(cfg.utcOffset, UTC_OFFSET_MIN_SEC, UTC_OFFSET_MAX_SEC);
  sanitizeStoredText();
  if (cfg.scrollText[0] == '\0') {
    strncpy(cfg.scrollText, "HELLO ESP8266", sizeof(cfg.scrollText) - 1);
    cfg.scrollText[sizeof(cfg.scrollText) - 1] = '\0';
  }
}

uint16_t XY(uint8_t x, uint8_t y) {
  if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return 0;

  uint8_t xx = x;
  uint8_t yy = y;

  switch (MATRIX_START_CORNER) {
    case 1: xx = MATRIX_WIDTH - 1 - x; yy = y; break;
    case 2: xx = MATRIX_WIDTH - 1 - x; yy = MATRIX_HEIGHT - 1 - y; break;
    case 3: xx = x; yy = MATRIX_HEIGHT - 1 - y; break;
    default: break;
  }

  if (MATRIX_ROWS_DIRECTION) {
    if (MATRIX_SERPENTINE && (yy & 1)) return yy * MATRIX_WIDTH + (MATRIX_WIDTH - 1 - xx);
    return yy * MATRIX_WIDTH + xx;
  }

  if (MATRIX_SERPENTINE && (xx & 1)) return xx * MATRIX_HEIGHT + (MATRIX_HEIGHT - 1 - yy);
  return xx * MATRIX_HEIGHT + yy;
}

void setPixelXY(int x, int y, const CRGB& c) {
  if (x < 0 || y < 0 || x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return;
  leds[XY((uint8_t)x, (uint8_t)y)] = c;
}

void fadeAll(uint8_t amount) {
  for (uint16_t i = 0; i < NUM_LEDS; i++) leds[i].fadeToBlackBy(amount);
}

void resetEffectState(uint8_t mode) {
  if (mode == MODE_FIRE || mode == MODE_FIREPLACE) memset(heat, 0, sizeof(heat));
  if (mode == MODE_SNOW) memset(snowMap, 0, sizeof(snowMap));
  if (mode == MODE_MATRIX_RAIN) memset(rainDrops, 0, sizeof(rainDrops));
  if (mode == MODE_STARS) memset(starsMap, 0, sizeof(starsMap));
  if (mode == MODE_SCROLL_TEXT) {
    scrollOffset = MATRIX_WIDTH;
    lastScrollStepMs = 0;
  }
  fill_solid(leds, NUM_LEDS, CRGB::Black);
}

void setMode(uint8_t mode) {
  uint8_t nextMode = clampU8(mode, 0, MODE_COUNT - 1);
  if (cfg.mode == nextMode) return;
  cfg.mode = nextMode;
  resetEffectState(cfg.mode);
}

String jsonEscape(const char* s) {
  String out;
  for (size_t i = 0; s[i] != '\0'; i++) {
    char ch = s[i];
    if (ch == '"' || ch == '\\') out += '\\';
    if ((uint8_t)ch < 0x20) out += ' ';
    else out += ch;
  }
  return out;
}

// ============================== ШРИФТЫ =======================================
const uint8_t DIGITS_3x5[10][5] = {
  {0b111, 0b101, 0b101, 0b101, 0b111},
  {0b010, 0b110, 0b010, 0b010, 0b111},
  {0b111, 0b001, 0b111, 0b100, 0b111},
  {0b111, 0b001, 0b111, 0b001, 0b111},
  {0b101, 0b101, 0b111, 0b001, 0b001},
  {0b111, 0b100, 0b111, 0b001, 0b111},
  {0b111, 0b100, 0b111, 0b101, 0b111},
  {0b111, 0b001, 0b001, 0b001, 0b001},
  {0b111, 0b101, 0b111, 0b101, 0b111},
  {0b111, 0b101, 0b111, 0b001, 0b111}
};

const uint8_t FONT5x7[][5] PROGMEM = {
  {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00},
  {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
  {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
  {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
  {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00},
  {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
  {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08},
  {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
  {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
  {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
  {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
  {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
  {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
  {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
  {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
  {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E},
  {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
  {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41},
  {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
  {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
  {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
  {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F},
  {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
  {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E},
  {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
  {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
  {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
  {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07},
  {0x61,0x51,0x49,0x45,0x43}
};

void drawDigit3x5(uint8_t digit, int x, int y, CRGB color) {
  if (digit > 9) return;
  for (uint8_t row = 0; row < 5; row++) {
    uint8_t bits = DIGITS_3x5[digit][row];
    for (uint8_t col = 0; col < 3; col++) {
      if (bits & (1 << (2 - col))) {
        uint8_t px = FONT_MIRROR_X ? (2 - col) : col;
        uint8_t py = FONT_MIRROR_Y ? (4 - row) : row;
        setPixelXY(x + px, y + py, color);
      }
    }
  }
}

uint8_t safeCharIndex(char c) {
  if (c >= 'a' && c <= 'z') c -= 32;
  if (c < 32 || c > 90) return 0;
  return (uint8_t)(c - 32);
}

void drawChar5x7(char c, int x, int y, const CRGB& color) {
  uint8_t idx = safeCharIndex(c);
  for (uint8_t col = 0; col < 5; col++) {
    uint8_t line = pgm_read_byte(&(FONT5x7[idx][col]));
    for (uint8_t row = 0; row < 7; row++) {
      if (line & (1 << row)) {
        uint8_t px = FONT_MIRROR_X ? (4 - col) : col;
        uint8_t py = FONT_MIRROR_Y ? (6 - row) : row;
        setPixelXY(x + px, y + py, color);
      }
    }
  }
}

// ============================== ХРАНЕНИЕ =====================================
void loadSettings() {
  EEPROM.begin(sizeof(Settings));
  EEPROM.get(0, cfg);

  bool valid = (cfg.magic == SETTINGS_MAGIC);
  if (valid) {
    uint16_t saved = cfg.crc;
    cfg.crc = 0;
    uint16_t calc = crc16((uint8_t*)&cfg, sizeof(Settings));
    cfg.crc = saved;
    valid = (saved == calc);
  }

  if (!valid) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = SETTINGS_MAGIC;
    cfg.mode = MODE_FIRE;
    cfg.brightness = 120;
    cfg.speed = 120;
    cfg.color = 0xFF5500;
    cfg.power = 1;
    cfg.utcOffset = UTC_OFFSET_SEC_DEFAULT;
    strncpy(cfg.scrollText, "HELLO ESP8266", sizeof(cfg.scrollText) - 1);
  }

  sanitizeSettings();
}

void saveSettings() {
  sanitizeSettings();
  cfg.magic = SETTINGS_MAGIC;
  cfg.crc = 0;
  cfg.crc = crc16((uint8_t*)&cfg, sizeof(Settings));
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ============================== WIFI / TIME / OTA ============================
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(120);
    yield();
  }

  wifiConnected = (WiFi.status() == WL_CONNECTED);
}

void maintainWiFi() {
  uint32_t now = millis();
  if (now - lastWiFiCheckMs < 3000) return;
  lastWiFiCheckMs = now;

  wl_status_t st = WiFi.status();
  wifiConnected = (st == WL_CONNECTED);
  if (!wifiConnected) WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void syncTimeNow() {
  if (!wifiConnected) return;
  configTime(cfg.utcOffset, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov", "time.google.com");

  time_t now = time(nullptr);
  uint32_t start = millis();
  while (now < 100000 && millis() - start < 4000) {
    delay(50);
    yield();
    now = time(nullptr);
  }
  ntpSynced = now > 100000;
  lastNtpSyncMs = millis();
}

void setupOTA() {
  ArduinoOTA.setHostname("ESP8266-Lamp");
  ArduinoOTA.onStart([]() { FastLED.clear(true); });
  ArduinoOTA.onEnd([]() { FastLED.clear(true); });
  ArduinoOTA.onError([](ota_error_t error) { (void)error; });
  ArduinoOTA.begin();
}

// ============================== ВЕБ ==========================================
String buildJsonState() {
  char cbuf[10];
  snprintf(cbuf, sizeof(cbuf), "#%06lX", (unsigned long)(cfg.color & 0xFFFFFF));

  String json = "{";
  json += "\"power\":" + String(cfg.power ? 1 : 0);
  json += ",\"mode\":" + String(cfg.mode);
  json += ",\"modeName\":\"" + String(MODE_NAMES_RU[cfg.mode]) + "\"";
  json += ",\"brightness\":" + String(cfg.brightness);
  json += ",\"speed\":" + String(cfg.speed);
  json += ",\"color\":\"" + String(cbuf) + "\"";
  json += ",\"text\":\"" + jsonEscape(cfg.scrollText) + "\"";
  json += ",\"utcOffset\":" + String(cfg.utcOffset);
  json += ",\"ip\":\"" + (wifiConnected ? ipToString(WiFi.localIP()) : String("нет")) + "\"";
  json += ",\"wifi\":" + String(wifiConnected ? 1 : 0);
  json += "}";
  return json;
}

const char WEB_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang='ru'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Wi-Fi лампа ESP8266</title>
<style>
body{font-family:Arial,sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:16px}
.card{max-width:760px;margin:auto;background:#1e293b;border-radius:16px;padding:16px;box-shadow:0 8px 24px rgba(0,0,0,.35)}
h1{font-size:20px;margin:0 0 12px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media(max-width:720px){.grid{grid-template-columns:1fr}}
label{display:block;font-size:13px;color:#94a3b8;margin:8px 0 4px}
input,select,button{width:100%;padding:10px;border-radius:10px;border:none;box-sizing:border-box}
button{background:#2563eb;color:#fff;font-weight:600;cursor:pointer}
button.secondary{background:#334155}
.row{display:flex;gap:8px}
small{color:#94a3b8}
.status{padding:10px;background:#0b1220;border-radius:10px;margin:8px 0}
</style></head><body><div class='card'>
<h1>Wi-Fi лампа (ESP8266)</h1>
<div class='status' id='status'>Загрузка...</div>
<div class='row'>
<button id='pwr'>ВКЛ/ВЫКЛ</button>
<button class='secondary' id='save'>Сохранить настройки</button>
</div>
<div class='grid'>
<div>
<label>Эффект</label><select id='mode'></select>
<label>Яркость</label><input id='br' type='range' min='1' max='255'>
<label>Скорость</label><input id='sp' type='range' min='1' max='255'>
</div><div>
<label>Цвет (для одноцветных режимов)</label><input id='col' type='color' value='#ff5500'>
<label>Текст бегущей строки (ASCII)</label><input id='txt' maxlength='90' placeholder='HELLO ESP8266'>
<label>Часовой пояс (сек)</label><input id='tz' type='number' min='-43200' max='50400' step='3600'>
</div></div>
<p><small>OTA: после первого USB-залива выберите в Arduino IDE сетевой порт вида "ESP8266-Lamp at x.x.x.x".</small></p>
<p><small>IP: <span id='ip'>-</span></small></p>
</div>
<script>
const modeNames = ["Огонь","Камин","Радуга","Конфетти","Снег","Матричный дождь","Звездное небо","Перелив цвета","Бегущие огни","Статичный цвет","Ночная подсветка","Часы","Дата","Бегущая строка","Цветная спираль","Плазма"];
const modeSel=document.getElementById('mode'),br=document.getElementById('br'),sp=document.getElementById('sp'),col=document.getElementById('col'),txt=document.getElementById('txt'),tz=document.getElementById('tz'),ip=document.getElementById('ip');
for(let i=0;i<modeNames.length;i++){const o=document.createElement('option');o.value=i;o.textContent=modeNames[i];modeSel.appendChild(o)}
async function getState(){
  const r=await fetch('/api/state'); const s=await r.json();
  document.getElementById('status').textContent=`Статус: ${s.power?"ВКЛ":"ВЫКЛ"} | Режим: ${s.modeName} | Wi-Fi: ${s.wifi?"OK":"нет"}`;
  modeSel.value=s.mode; br.value=s.brightness; sp.value=s.speed; col.value=s.color; txt.value=s.text||''; tz.value=s.utcOffset; ip.textContent=s.ip;
}
async function apply(part){
  const p=new URLSearchParams(part);
  await fetch('/api/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()});
  getState();
}
modeSel.onchange=()=>apply({mode:modeSel.value});
br.oninput=()=>apply({brightness:br.value});
sp.oninput=()=>apply({speed:sp.value});
col.onchange=()=>apply({color:col.value});
txt.onchange=()=>apply({text:txt.value});
tz.onchange=()=>apply({tz:tz.value});
document.getElementById('pwr').onclick=()=>apply({power:'toggle'});
document.getElementById('save').onclick=()=>fetch('/api/save',{method:'POST'});
getState(); setInterval(getState,2000);
</script></body></html>
)rawliteral";

void handleRoot() {
  server.send_P(200, PSTR("text/html; charset=utf-8"), WEB_PAGE);
}

void handleState() {
  server.send(200, "application/json; charset=utf-8", buildJsonState());
}

uint32_t parseColorHex(const String& s) {
  String t = s;
  if (t.length() == 7 && t[0] == '#') t = t.substring(1);
  return strtoul(t.c_str(), nullptr, 16) & 0xFFFFFFUL;
}

void handleSet() {
  if (server.hasArg("mode")) setMode(server.arg("mode").toInt());
  if (server.hasArg("brightness")) cfg.brightness = clampU8(server.arg("brightness").toInt(), 1, 255);
  if (server.hasArg("speed")) cfg.speed = clampU8(server.arg("speed").toInt(), 1, 255);
  if (server.hasArg("color")) cfg.color = parseColorHex(server.arg("color"));
  if (server.hasArg("text")) {
    String t = server.arg("text");
    t.trim();
    if (t.length() == 0) t = "HELLO ESP8266";
    for (uint16_t i = 0; i < t.length(); i++) {
      char ch = t[i];
      if (ch >= 'a' && ch <= 'z') ch -= 32;
      if (ch < 32 || ch > 90) ch = ' ';
      t.setCharAt(i, ch);
    }
    t.toCharArray(cfg.scrollText, sizeof(cfg.scrollText));
    resetEffectState(MODE_SCROLL_TEXT);
  }
  if (server.hasArg("tz")) {
    cfg.utcOffset = clampI32(server.arg("tz").toInt(), UTC_OFFSET_MIN_SEC, UTC_OFFSET_MAX_SEC);
    syncTimeNow();
  }
  if (server.hasArg("power")) {
    String p = server.arg("power");
    if (p == "0") cfg.power = 0;
    else if (p == "1") cfg.power = 1;
    else cfg.power = !cfg.power;
  }

  sanitizeSettings();
  FastLED.setBrightness(cfg.power ? cfg.brightness : 0);
  server.send(200, "application/json", "{\"ok\":1}");
}

void handleSave() {
  saveSettings();
  server.send(200, "application/json", "{\"saved\":1}");
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/set", HTTP_POST, handleSet);
  server.on("/api/save", HTTP_POST, handleSave);
  server.onNotFound([]() { server.send(404, "text/plain", "404"); });
  server.begin();
}

// ============================== КНОПКА =======================================
void nextEffect() {
  setMode((cfg.mode + 1) % MODE_COUNT);
}

void previousEffect() {
  if (cfg.mode == 0) setMode(MODE_COUNT - 1);
  else setMode(cfg.mode - 1);
}

void executeButtonClicks(uint8_t clicks) {
  if (clicks == 1) {
    cfg.power = !cfg.power;
    FastLED.setBrightness(cfg.power ? cfg.brightness : 0);
  } else if (clicks == 2) {
    nextEffect();
  } else if (clicks >= 3) {
    previousEffect();
  }
}

void startBrightnessHold(uint32_t now) {
  holdBrightnessActive = true;
  clickCount = 0;
  brightnessHoldDir = (cfg.brightness >= 255) ? -1 : 1;
  brightnessHoldPaused = (cfg.brightness >= 255);
  brightnessHoldPauseStartMs = now;
  lastBrightnessStepMs = 0;
  cfg.power = 1;
  FastLED.setBrightness(cfg.brightness);
}

void stopBrightnessHold() {
  holdBrightnessActive = false;
  brightnessHoldPaused = false;
}

void updateButtonBrightness(uint32_t now) {
  if (brightnessHoldPaused) {
    if (now - brightnessHoldPauseStartMs < BRIGHTNESS_HOLD_PAUSE_MS) return;
    brightnessHoldPaused = false;
    lastBrightnessStepMs = now;
  }

  if (now - lastBrightnessStepMs < BRIGHTNESS_HOLD_STEP_MS) return;
  lastBrightnessStepMs = now;

  int nextBrightness = cfg.brightness + brightnessHoldDir * BRIGHTNESS_HOLD_STEP;
  if (nextBrightness >= 255) {
    cfg.brightness = 255;
    brightnessHoldDir = -1;
    brightnessHoldPaused = true;
    brightnessHoldPauseStartMs = now;
  } else if (nextBrightness <= 1) {
    cfg.brightness = 1;
    brightnessHoldDir = 1;
    brightnessHoldPaused = true;
    brightnessHoldPauseStartMs = now;
  } else {
    cfg.brightness = (uint8_t)nextBrightness;
  }

  cfg.power = 1;
  FastLED.setBrightness(cfg.brightness);
}

void handleButton() {
  uint32_t now = millis();
  bool raw = digitalRead(BUTTON_PIN);

  if (raw != btnRawState) {
    btnRawState = raw;
    btnLastChangeMs = now;
  }

  if (now - btnLastChangeMs >= BUTTON_DEBOUNCE_MS && btnStableState != btnRawState) {
    btnStableState = btnRawState;
    bool buttonPressed = (btnStableState == BUTTON_ACTIVE_LEVEL);
    if (buttonPressed) {
      btnPressStartMs = now;
    } else {
      uint32_t pressDur = now - btnPressStartMs;
      if (holdBrightnessActive) {
        stopBrightnessHold();
      } else if (pressDur <= BUTTON_CLICK_MAX_MS) {
        if (clickCount < 3) clickCount++;
        btnLastReleaseMs = now;
        if (clickCount >= 3) {
          executeButtonClicks(clickCount);
          clickCount = 0;
        }
      }
    }
  }

  bool buttonPressedNow = (btnStableState == BUTTON_ACTIVE_LEVEL);
  if (buttonPressedNow && !holdBrightnessActive && (now - btnPressStartMs >= BUTTON_LONG_PRESS_MS)) {
    startBrightnessHold(now);
  }

  if (buttonPressedNow && holdBrightnessActive) {
    updateButtonBrightness(now);
  }

  if (!buttonPressedNow && clickCount > 0 && (now - btnLastReleaseMs >= BUTTON_MULTI_CLICK_MS)) {
    executeButtonClicks(clickCount);
    clickCount = 0;
  }
}

// ============================== РЕЖИМЫ =======================================
uint16_t speedToMs(uint8_t speed) {
  return (uint16_t)map(speed, 1, 255, FRAME_MAX_MS, FRAME_MIN_MS);
}

void drawFire(bool fireplace) {
  uint8_t cooling = fireplace ? 35 : 55;
  uint8_t sparking = fireplace ? 45 : 80;

  for (uint8_t x = 0; x < MATRIX_WIDTH; x++) {
    for (uint8_t y = 1; y < MATRIX_HEIGHT; y++) {
      uint8_t decay = random8(0, ((cooling * 10) / MATRIX_HEIGHT) + 2);
      heat[x][y] = qsub8(heat[x][y], decay);
    }
    for (int y = MATRIX_HEIGHT - 1; y >= 2; y--) {
      heat[x][y] = (heat[x][y - 1] + heat[x][y - 2] + heat[x][y - 2]) / 3;
    }
    if (random8() < sparking) {
      uint8_t y = random8(2);
      heat[x][y] = qadd8(heat[x][y], random8(160, 255));
    }
    for (uint8_t y = 0; y < MATRIX_HEIGHT; y++) {
      uint8_t h = heat[x][y];
      if (y == 0 && h < (fireplace ? 130 : 170)) h = (fireplace ? 130 : 170) + random8(20);
      if (y == 1 && h < (fireplace ? 95 : 135)) h = (fireplace ? 95 : 135) + random8(20);
      CRGB c = HeatColor(h);
      if (fireplace) c.nscale8_video(180);
      setPixelXY(x, MATRIX_HEIGHT - 1 - y, c);
    }
  }
}

void drawRainbow() {
  for (uint8_t y = 0; y < MATRIX_HEIGHT; y++) {
    for (uint8_t x = 0; x < MATRIX_WIDTH; x++) {
      setPixelXY(x, y, CHSV(hueBase + x * 7 + y * 4, 255, 255));
    }
  }
  hueBase++;
}

void drawConfetti() {
  fadeAll(12);
  leds[random16(NUM_LEDS)] += CHSV(hueBase + random8(64), 200, 255);
  hueBase += 2;
}

void drawSnow() {
  fadeAll(32);
  for (int y = MATRIX_HEIGHT - 1; y > 0; y--) {
    for (int x = 0; x < MATRIX_WIDTH; x++) snowMap[x][y] = snowMap[x][y - 1];
  }
  for (int x = 0; x < MATRIX_WIDTH; x++) {
    snowMap[x][0] = random8() < map(cfg.speed, 1, 255, 8, 45) ? 255 : 0;
  }
  for (int y = 0; y < MATRIX_HEIGHT; y++) {
    for (int x = 0; x < MATRIX_WIDTH; x++) {
      if (snowMap[x][y]) setPixelXY(x, y, CRGB(180, 180, 255));
    }
  }
}

void drawMatrixRain() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  for (int x = 0; x < MATRIX_WIDTH; x++) {
    for (int y = MATRIX_HEIGHT - 1; y > 0; y--) {
      rainDrops[x][y] = qsub8(rainDrops[x][y - 1], random8(5, 18));
    }
    if (random8() < map(cfg.speed, 1, 255, 10, 70)) rainDrops[x][0] = 255;
    else rainDrops[x][0] = qsub8(rainDrops[x][0], 28);
  }
  for (int y = 0; y < MATRIX_HEIGHT; y++) {
    for (int x = 0; x < MATRIX_WIDTH; x++) {
      if (rainDrops[x][y]) {
        uint8_t v = rainDrops[x][y];
        setPixelXY(x, y, CRGB(0, v, 0));
      }
    }
  }
}

void drawStars() {
  fadeAll(8);
  for (int i = 0; i < 3; i++) {
    if (random8() < map(cfg.speed, 1, 255, 3, 20)) starsMap[random8(MATRIX_WIDTH)][random8(MATRIX_HEIGHT)] = random8(180, 255);
  }
  for (int y = 0; y < MATRIX_HEIGHT; y++) {
    for (int x = 0; x < MATRIX_WIDTH; x++) {
      if (starsMap[x][y] > 0) {
        starsMap[x][y] = qsub8(starsMap[x][y], random8(5, 20));
        setPixelXY(x, y, CHSV(160, 20, starsMap[x][y]));
      }
    }
  }
}

void drawColorWave() {
  CRGB base = CRGB((cfg.color >> 16) & 0xFF, (cfg.color >> 8) & 0xFF, cfg.color & 0xFF);
  uint8_t t = beatsin8(map(cfg.speed, 1, 255, 4, 30), 40, 255);
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    leds[i] = base;
    leds[i].nscale8_video(t);
  }
}

void drawColorSpiral() {
  int8_t cx = (MATRIX_WIDTH - 1) / 2;
  int8_t cy = (MATRIX_HEIGHT - 1) / 2;
  for (int y = 0; y < MATRIX_HEIGHT; y++) {
    for (int x = 0; x < MATRIX_WIDTH; x++) {
      uint8_t dist = abs(x - cx) * 18 + abs(y - cy) * 18;
      uint8_t wave = sin8(x * 18 + y * 12 + hueBase);
      setPixelXY(x, y, CHSV(hueBase + dist + wave / 3, 240, 255));
    }
  }
  hueBase += map(cfg.speed, 1, 255, 1, 5);
}

void drawPlasma() {
  for (int y = 0; y < MATRIX_HEIGHT; y++) {
    for (int x = 0; x < MATRIX_WIDTH; x++) {
      uint16_t a = sin8(x * 16 + hueBase);
      uint16_t b = sin8(y * 16 + hueBase / 2);
      uint16_t c = sin8((x + y) * 10 + hueBase * 2);
      uint8_t mix = (a + b + c) / 3;
      setPixelXY(x, y, CHSV(mix + hueBase, 230, 180 + (mix / 4)));
    }
  }
  hueBase += map(cfg.speed, 1, 255, 1, 4);
}

void drawRunningLights() {
  CRGB base = CRGB((cfg.color >> 16) & 0xFF, (cfg.color >> 8) & 0xFF, cfg.color & 0xFF);
  uint8_t beat = beatsin8(map(cfg.speed, 1, 255, 4, 24), 0, 255);
  for (int y = 0; y < MATRIX_HEIGHT; y++) {
    for (int x = 0; x < MATRIX_WIDTH; x++) {
      CRGB c = base;
      c.nscale8_video(sin8(x * 16 + y * 8 + beat));
      setPixelXY(x, y, c);
    }
  }
}

void drawStaticColor() {
  fill_solid(leds, NUM_LEDS, CRGB((cfg.color >> 16) & 0xFF, (cfg.color >> 8) & 0xFF, cfg.color & 0xFF));
}

void drawNightLight() {
  fill_solid(leds, NUM_LEDS, CRGB(255, 80, 8));
  for (uint16_t i = 0; i < NUM_LEDS; i++) leds[i].nscale8_video(20);
}

void drawClock() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);

  CRGB col = CHSV(hueBase, 180, 255);
  drawDigit3x5(t.tm_hour / 10, 0, 5, col);
  drawDigit3x5(t.tm_hour % 10, 4, 5, col);
  drawDigit3x5(t.tm_min / 10, 9, 5, col);
  drawDigit3x5(t.tm_min % 10, 13, 5, col);

  if (millis() - lastClockBlinkMs > 500) {
    lastClockBlinkMs = millis();
    clockDots = !clockDots;
  }
  if (clockDots) {
    setPixelXY(8, 6, CRGB::White);
    setPixelXY(8, 8, CRGB::White);
  }
  hueBase++;
}

void drawDate() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);

  CRGB col = CRGB(80, 180, 255);
  drawDigit3x5(t.tm_mday / 10, 0, 5, col);
  drawDigit3x5(t.tm_mday % 10, 4, 5, col);
  setPixelXY(8, 7, CRGB::White);
  drawDigit3x5((t.tm_mon + 1) / 10, 9, 5, col);
  drawDigit3x5((t.tm_mon + 1) % 10, 13, 5, col);
}

int textPixelWidth(const char* s) {
  return (int)strlen(s) * 6;
}

void drawScrollingText() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  uint32_t now = millis();
  uint16_t stepMs = (uint16_t)map(cfg.speed, 1, 255, 170, 35);
  if (now - lastScrollStepMs >= stepMs) {
    lastScrollStepMs = now;
    scrollOffset--;
    int w = textPixelWidth(cfg.scrollText);
    if (scrollOffset < -w) scrollOffset = MATRIX_WIDTH;
  }

  CRGB col((cfg.color >> 16) & 0xFF, (cfg.color >> 8) & 0xFF, cfg.color & 0xFF);
  int x = scrollOffset;
  for (size_t i = 0; i < strlen(cfg.scrollText); i++) {
    drawChar5x7(cfg.scrollText[i], x, 4, col);
    x += 6;
  }
}

void updateEffects() {
  if (!cfg.power) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    return;
  }

  switch (cfg.mode) {
    case MODE_FIRE: drawFire(false); break;
    case MODE_FIREPLACE: drawFire(true); break;
    case MODE_RAINBOW: drawRainbow(); break;
    case MODE_CONFETTI: drawConfetti(); break;
    case MODE_SNOW: drawSnow(); break;
    case MODE_MATRIX_RAIN: drawMatrixRain(); break;
    case MODE_STARS: drawStars(); break;
    case MODE_COLOR_WAVE: drawColorWave(); break;
    case MODE_RUNNING_LIGHTS: drawRunningLights(); break;
    case MODE_STATIC_COLOR: drawStaticColor(); break;
    case MODE_NIGHT_LIGHT: drawNightLight(); break;
    case MODE_CLOCK: drawClock(); break;
    case MODE_DATE: drawDate(); break;
    case MODE_SCROLL_TEXT: drawScrollingText(); break;
    case MODE_COLOR_SPIRAL: drawColorSpiral(); break;
    case MODE_PLASMA: drawPlasma(); break;
    default: drawRainbow(); break;
  }
}

// ============================== SETUP / LOOP =================================
void setup() {
  pinMode(BUTTON_PIN, BUTTON_PIN_MODE);
  btnRawState = digitalRead(BUTTON_PIN);
  btnStableState = btnRawState;
  btnLastChangeMs = millis();
  Serial.begin(115200);
  delay(50);

  loadSettings();

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(LED_SUPPLY_VOLTS, LED_MAX_MILLIAMPS);
  FastLED.setBrightness(cfg.power ? cfg.brightness : 0);
  FastLED.setDither(0);
  FastLED.clear(true);

  random16_set_seed(analogRead(A0));

  setupWiFi();
  setupWebServer();
  setupOTA();
  syncTimeNow();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  handleButton();
  maintainWiFi();

  if (wifiConnected && (!ntpSynced || millis() - lastNtpSyncMs > NTP_RESYNC_MS)) syncTimeNow();

  uint16_t frameMs = speedToMs(cfg.speed);
  if (millis() - lastFrameMs >= frameMs) {
    lastFrameMs = millis();
    FastLED.setBrightness(cfg.power ? cfg.brightness : 0);
    updateEffects();
    FastLED.show();
  }

  yield();
}
