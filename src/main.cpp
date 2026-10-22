#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <U8g2lib.h> // chỉ để lấy font tiếng Việt
#include "lgfx_config.hpp"

// =====================================================
// Phần cứng & hằng số
// =====================================================
#define SPI_SCK 18
#define SPI_MISO 19
#define SPI_MOSI 23
#define RC522_SS 21
#define RC522_RST 22

#define BUZZER_PIN 25
#define BUZZER_PASSIVE true // buzzer của bạn là loại passive
#define BUZZER_CH 2         // kênh LEDC (đèn nền màn dùng kênh 7)

#define RESET_BTN_PIN 0 // nút BOOT trên board

const char *FW_VERSION = "1.1.0";
const char *DEFAULT_API_URL = "https://theantam.aiturbox.com/api/reader/scan";

const unsigned long SAME_CARD_COOLDOWN_MS = 3000;
const unsigned long RESULT_SHOW_MS = 3000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
const unsigned long WIFI_DOWN_RESTART_MS = 5UL * 60 * 1000;
const unsigned long AP_KEEP_AFTER_OK_MS = 60000;
const unsigned long RFID_CHECK_INTERVAL_MS = 10000;
const unsigned long RESET_HOLD_MS = 5000;
const uint32_t WDT_TIMEOUT_S = 30;

// =====================================================
// Cấu hình lưu trong flash (NVS)
// =====================================================
struct Config
{
  String ssid, pass, key, api;
};

Config cfg;
Preferences prefs;

void loadConfig()
{
  prefs.begin("cfg", true);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.pass = prefs.getString("pass", "");
  cfg.key = prefs.getString("key", "");
  cfg.api = prefs.getString("api", DEFAULT_API_URL);
  prefs.end();
  if (!cfg.api.length())
    cfg.api = DEFAULT_API_URL;
}

void saveConfig(const Config &c)
{
  prefs.begin("cfg", false);
  prefs.putString("ssid", c.ssid);
  prefs.putString("pass", c.pass);
  prefs.putString("key", c.key);
  prefs.putString("api", c.api);
  prefs.end();
}

void clearConfig()
{
  prefs.begin("cfg", false);
  prefs.clear();
  prefs.end();
}

bool isConfigured()
{
  return cfg.ssid.length() && cfg.key.length();
}

// =====================================================
// Trạng thái chung
// =====================================================
WebServer server(80);
DNSServer dns;

bool apActive = false;
String apSsid;
bool rfidOk = false;
String lastClass;

bool lastWifiState = false;
unsigned long apCloseAt = 0;
unsigned long wifiLostSince = 0;
unsigned long lastWifiRetry = 0;
unsigned long restartAt = 0;

bool pendingConnect = false;
Config pendingCfg;
String connectStatus = "idle"; // idle | connecting | ok | fail

bool showingTemp = false; // đang hiện màn tạm (kết quả quét, thông báo)
unsigned long tempUntil = 0;

// =====================================================
// Màn hình
// =====================================================
LGFX tft;
static const lgfx::U8g2font fontVN(u8g2_font_unifont_t_vietnamese2);

constexpr int W = 320, H = 170;
constexpr int HEADER_H = 22;
constexpr int FOOTER_Y = 148;

constexpr uint16_t C_BG = TFT_BLACK;
constexpr uint16_t C_BAR = 0x2104;
constexpr uint16_t C_TEXT = TFT_WHITE;
constexpr uint16_t C_DIM = 0x8410;
constexpr uint16_t C_INFO = TFT_CYAN;
constexpr uint16_t C_OK = TFT_GREEN;
constexpr uint16_t C_LATE = TFT_YELLOW;
constexpr uint16_t C_BAD = TFT_RED;

String cleanText(String s)
{
  s.replace("–", "-");
  s.replace("—", "-");
  return s;
}

void drawFit(const String &text, int y, uint16_t color, float maxSize)
{
  String s = cleanText(text);
  float size = maxSize;
  tft.setTextSize(size);
  while (size > 1.0f && tft.textWidth(s) > W - 12)
  {
    size -= 0.5f;
    tft.setTextSize(size);
  }
  tft.setTextColor(color);
  tft.setTextDatum(middle_center);
  tft.drawString(s, W / 2, y);
}

void drawHeader()
{
  tft.fillRect(0, 0, W, HEADER_H, C_BAR);
  tft.setTextSize(1);

  String left, right;
  uint16_t lc;
  if (WiFi.status() == WL_CONNECTED)
  {
    left = WiFi.SSID();
    right = WiFi.localIP().toString();
    lc = C_OK;
  }
  else if (apActive)
  {
    left = apSsid;
    right = WiFi.softAPIP().toString();
    lc = C_LATE;
  }
  else
  {
    left = "Mất WiFi";
    right = "";
    lc = C_BAD;
  }
  if (left.length() > 20)
    left = left.substring(0, 18) + "..";

  tft.setTextDatum(middle_left);
  tft.setTextColor(lc);
  tft.drawString(left, 6, HEADER_H / 2);
  tft.setTextDatum(middle_right);
  tft.setTextColor(C_TEXT);
  tft.drawString(right, W - 6, HEADER_H / 2);
}

void showLines(const String &l1, uint16_t c1,
               const String &l2 = "", uint16_t c2 = C_TEXT,
               const String &l3 = "", uint16_t c3 = C_TEXT)
{
  tft.fillRect(0, HEADER_H, W, FOOTER_Y - HEADER_H, C_BG);
  if (l3.length())
  {
    drawFit(l1, 52, c1, 2.0f);
    drawFit(l2, 90, c2, 1.0f);
    drawFit(l3, 122, c3, 1.5f);
  }
  else if (l2.length())
  {
    drawFit(l1, 68, c1, 2.0f);
    drawFit(l2, 115, c2, 1.5f);
  }
  else
  {
    drawFit(l1, 85, c1, 2.0f);
  }
}

void showFooter(const String &text, uint16_t color = C_DIM)
{
  tft.fillRect(0, FOOTER_Y, W, H - FOOTER_Y, C_BG);
  tft.drawFastHLine(0, FOOTER_Y, W, C_BAR);
  tft.setTextSize(1);
  tft.setTextDatum(middle_center);
  tft.setTextColor(color);
  tft.drawString(cleanText(text), W / 2, (FOOTER_Y + H) / 2 + 1);
}

void showTemp(unsigned long ms)
{
  showingTemp = true;
  tempUntil = millis() + ms;
}

uint16_t colorForKind(const String &kind)
{
  if (kind == "ok")
    return C_OK;
  if (kind == "late")
    return C_LATE;
  return C_BAD;
}

// Màn chờ, tùy theo trạng thái thiết bị
void refreshIdle()
{
  bool connected = WiFi.status() == WL_CONNECTED;
  String apIp = WiFi.softAPIP().toString();

  if (!isConfigured())
  {
    showLines("Cài đặt thiết bị", C_LATE,
              "Kết nối WiFi: " + apSsid, C_TEXT,
              "Mở " + apIp, C_INFO);
    showFooter("Chỉ hỗ trợ WiFi 2.4GHz", C_LATE);
    return;
  }

  if (!connected && apActive)
  {
    showLines("Không vào được WiFi", C_BAD,
              "Cài lại: WiFi " + apSsid, C_TEXT,
              "Mở " + apIp, C_INFO);
    showFooter("Vẫn đang thử lại: " + cfg.ssid, C_LATE);
    return;
  }

  if (!connected)
  {
    showLines("Mất kết nối WiFi", C_BAD, "Đang thử kết nối lại...", C_DIM);
    showFooter(cfg.ssid);
    return;
  }

  if (lastClass.length())
    showLines("Mời quẹt thẻ", C_TEXT, "Lớp: " + lastClass, C_INFO,
              "Đưa thẻ lại gần đầu đọc", C_DIM);
  else
    showLines("Mời quẹt thẻ", C_TEXT, "Đưa thẻ lại gần đầu đọc", C_DIM);

  if (!rfidOk)
    showFooter("Lỗi đầu đọc RC522 - kiểm tra dây", C_BAD);
  else
    showFooter("Thẻ An Tâm");
}

// =====================================================
// Buzzer
// =====================================================
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#define BUZZ_TONE(f) ledcWriteTone(BUZZER_PIN, f)
#else
#define BUZZ_TONE(f) ledcWriteTone(BUZZER_CH, f)
#endif

struct Note
{
  uint16_t freq;
  uint16_t ms;
}; // freq = 0 là nghỉ

const Note SND_OK[] = {{2700, 150}};
const Note SND_LATE[] = {{2200, 150}, {0, 100}, {2200, 150}};
const Note SND_BAD[] = {{1000, 600}};
const Note SND_ERR[] = {{1500, 80}, {0, 80}, {1500, 80}, {0, 80}, {1500, 80}};

const Note *sndSeq = nullptr;
uint8_t sndLen = 0, sndIdx = 0;
unsigned long sndStepEnd = 0;

void buzzerOff()
{
  if (BUZZER_PASSIVE)
    BUZZ_TONE(0);
  else
    digitalWrite(BUZZER_PIN, LOW);
}

void buzzerOn(uint16_t freq)
{
  if (freq == 0)
  {
    buzzerOff();
    return;
  }
  if (BUZZER_PASSIVE)
    BUZZ_TONE(freq);
  else
    digitalWrite(BUZZER_PIN, HIGH);
}

void buzzerInit()
{
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  if (BUZZER_PASSIVE)
  {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttachChannel(BUZZER_PIN, 2000, 8, BUZZER_CH);
#else
    ledcSetup(BUZZER_CH, 2000, 8);
    ledcAttachPin(BUZZER_PIN, BUZZER_CH);
#endif
    BUZZ_TONE(0);
  }
}

template <size_t N>
void playSound(const Note (&seq)[N])
{
  sndSeq = seq;
  sndLen = N;
  sndIdx = 0;
  buzzerOn(seq[0].freq);
  sndStepEnd = millis() + seq[0].ms;
}

void buzzerUpdate()
{
  if (!sndSeq || millis() < sndStepEnd)
    return;
  if (++sndIdx >= sndLen)
  {
    buzzerOff();
    sndSeq = nullptr;
    return;
  }
  buzzerOn(sndSeq[sndIdx].freq);
  sndStepEnd = millis() + sndSeq[sndIdx].ms;
}

void beepTap()
{
  buzzerOn(3000);
  delay(50);
  buzzerOff();
}

void playForKind(const String &kind)
{
  if (kind == "ok")
    playSound(SND_OK);
  else if (kind == "late")
    playSound(SND_LATE);
  else
    playSound(SND_BAD);
}

// =====================================================
// NFC
// =====================================================
MFRC522 rfid(RC522_SS, RC522_RST);

String lastUid;
unsigned long lastScanMs = 0;

void rfidInit()
{
  rfid.PCD_Init();
  delay(10);
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
}

bool rfidCheck()
{
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  return !(v == 0x00 || v == 0xFF);
}

String uidToString(const MFRC522::Uid &uid)
{
  String s;
  for (byte i = 0; i < uid.size; i++)
  {
    if (i)
      s += ':';
    if (uid.uidByte[i] < 0x10)
      s += '0';
    s += String(uid.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// Tự kiểm tra RC522, khởi tạo lại nếu bị treo
void rfidHealthTick()
{
  static unsigned long last = 0;
  if (millis() - last < RFID_CHECK_INTERVAL_MS)
    return;
  last = millis();

  bool ok = rfidCheck();
  if (!ok)
  {
    Serial.println("RC522 khong phan hoi, khoi tao lai...");
    rfidInit();
    ok = rfidCheck();
  }
  if (ok != rfidOk)
  {
    rfidOk = ok;
    if (!showingTemp)
      refreshIdle();
  }
}

// =====================================================
// Watchdog
// =====================================================
void wdtInit()
{
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t c = {};
  c.timeout_ms = WDT_TIMEOUT_S * 1000;
  c.idle_core_mask = 0;
  c.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&c) != ESP_OK)
    esp_task_wdt_init(&c);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
}

// =====================================================
// WiFi
// =====================================================
// Các việc nền phải chạy cả khi đang chờ kết nối
void serviceBackground()
{
  esp_task_wdt_reset();
  server.handleClient();
  if (apActive)
    dns.processNextRequest();
  buzzerUpdate();
}

bool connectSTA(const String &ssid, const String &pass, unsigned long timeoutMs)
{
  Serial.printf("Ket noi WiFi \"%s\"", ssid.c_str());
  WiFi.disconnect(false);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : nullptr);

  unsigned long start = millis(), dot = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs)
  {
    serviceBackground();
    if (millis() - dot > 500)
    {
      dot = millis();
      Serial.print('.');
    }
    delay(20);
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void startAP()
{
  if (apActive)
    return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid.c_str());
  delay(100);
  dns.start(53, "*", WiFi.softAPIP());
  apActive = true;
  Serial.printf("Bat WiFi cai dat \"%s\" - %s\n",
                apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

void stopAP()
{
  if (!apActive)
    return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  Serial.println("Tat WiFi cai dat");
}

void wifiTick()
{
  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected != lastWifiState)
  {
    lastWifiState = connected;
    if (connected)
    {
      Serial.printf("WiFi OK, IP: %s\n", WiFi.localIP().toString().c_str());
      wifiLostSince = 0;
      if (apActive && !apCloseAt)
        apCloseAt = millis() + AP_KEEP_AFTER_OK_MS;
    }
    else
    {
      Serial.println("Mat WiFi");
      wifiLostSince = millis();
      lastWifiRetry = millis();
    }
    drawHeader();
    if (!showingTemp)
      refreshIdle();
  }

  if (!connected && isConfigured())
  {
    if (millis() - lastWifiRetry > WIFI_RETRY_INTERVAL_MS)
    {
      lastWifiRetry = millis();
      Serial.println("Thu ket noi lai WiFi...");
      WiFi.disconnect(false);
      WiFi.begin(cfg.ssid.c_str(), cfg.pass.length() ? cfg.pass.c_str() : nullptr);
    }
    if (!apActive && wifiLostSince && millis() - wifiLostSince > WIFI_DOWN_RESTART_MS)
    {
      Serial.println("Mat WiFi qua lau, khoi dong lai");
      ESP.restart();
    }
  }

  // Đã vào WiFi ổn định thì tắt WiFi cài đặt
  if (apActive && apCloseAt && connected && millis() > apCloseAt)
  {
    apCloseAt = 0;
    stopAP();
    drawHeader();
  }
}

// Thử kết nối WiFi mới nhập từ trang cài đặt
void doPendingConnect()
{
  pendingConnect = false;
  showLines("Đang kết nối WiFi", C_LATE, pendingCfg.ssid, C_TEXT);
  showFooter("Chỉ hỗ trợ WiFi 2.4GHz", C_LATE);

  bool ok = connectSTA(pendingCfg.ssid, pendingCfg.pass, WIFI_CONNECT_TIMEOUT_MS);

  if (ok)
  {
    cfg = pendingCfg;
    saveConfig(cfg);
    connectStatus = "ok";
    wifiLostSince = 0;
    lastWifiState = true;
    apCloseAt = millis() + AP_KEEP_AFTER_OK_MS;
    Serial.printf("WiFi OK, IP: %s - da luu cau hinh\n", WiFi.localIP().toString().c_str());

    drawHeader();
    showLines("Kết nối thành công", C_OK,
              "Trang quản trị:", C_DIM,
              "http://" + WiFi.localIP().toString(), C_INFO);
    showFooter("WiFi cài đặt sẽ tắt sau 1 phút");
    playSound(SND_OK);
    showTemp(10000);
  }
  else
  {
    connectStatus = "fail";
    Serial.println("Ket noi that bai");
    WiFi.disconnect(false);
    if (isConfigured()) // quay lại thử WiFi cũ
      WiFi.begin(cfg.ssid.c_str(), cfg.pass.length() ? cfg.pass.c_str() : nullptr);
    lastWifiState = false;

    drawHeader();
    showLines("Kết nối thất bại", C_BAD,
              "Sai mật khẩu hoặc không phải 2.4GHz", C_TEXT,
              "Thử lại tại " + WiFi.softAPIP().toString(), C_INFO);
    showFooter("WiFi cài đặt: " + apSsid, C_LATE);
    playSound(SND_ERR);
    showTemp(8000);
  }
}

// =====================================================
// API
// =====================================================
struct ScanResult
{
  bool netOk;
  String kind, name, message, className;
};

// Lấy tên lớp từ JSON, thử nhiều tên trường
String pickClass(JsonDocument &res)
{
  const char *keys[] = {"class_name", "className", "class", "classroom", "lop"};
  for (const char *k : keys)
  {
    JsonVariant v = res[k];
    if (v.is<const char *>())
      return v.as<const char *>();
    if (v.is<JsonObject>() && v["name"].is<const char *>())
      return v["name"].as<const char *>();
  }
  return "";
}

ScanResult sendScan(const String &uid)
{
  if (WiFi.status() != WL_CONNECTED)
    return {false, "", "", "Mất WiFi"};

  bool https = cfg.api.startsWith("https://");
  WiFiClientSecure secure;
  WiFiClient plain;
  if (https)
  {
    secure.setInsecure(); // MVP: bỏ qua kiểm tra chứng chỉ
    secure.setHandshakeTimeout(10);
  }
  WiFiClient &client = https ? (WiFiClient &)secure : plain;

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(client, cfg.api))
    return {false, "", "", "API URL không hợp lệ"};
  http.addHeader("Content-Type", "application/json");

  JsonDocument req;
  req["key"] = cfg.key;
  req["uid"] = uid;
  String body;
  serializeJson(req, body);

  unsigned long t0 = millis();
  int code = http.POST(body);
  if (code <= 0)
  {
    String err = http.errorToString(code);
    http.end();
    Serial.printf("=> Loi ket noi: %s\n", err.c_str());
    return {false, "", "", "Không gọi được máy chủ"};
  }

  String resp = http.getString();
  http.end();
  Serial.printf("HTTP %d (%lu ms): %s\n", code, millis() - t0, resp.c_str());

  JsonDocument res;
  if (deserializeJson(res, resp))
    return {false, "", "", "Phản hồi lỗi (HTTP " + String(code) + ")"};

  ScanResult r;
  r.netOk = true;
  r.kind = res["kind"] | "bad";
  r.name = res["name"] | "";
  r.message = res["message"] | "Lỗi không xác định";
  r.className = pickClass(res);
  return r;
}

// =====================================================
// Web: trang cài đặt / quản trị
// =====================================================
const char PAGE_HEAD[] = R"HTML(<!DOCTYPE html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Máy điểm danh</title><style>
body{font-family:system-ui,-apple-system,sans-serif;max-width:480px;margin:0 auto;padding:16px;background:#f4f5f7;color:#222}
h2{margin:8px 0 16px}.card{background:#fff;border-radius:10px;padding:16px;margin-bottom:12px;box-shadow:0 1px 3px rgba(0,0,0,.08)}
label{display:block;font-weight:600;margin:14px 0 4px}
input{width:100%;padding:10px;border:1px solid #ccc;border-radius:6px;box-sizing:border-box;font-size:16px}
button{width:100%;padding:12px;margin-top:16px;border:0;border-radius:6px;background:#1565c0;color:#fff;font-size:16px}
.warn{background:#fff3cd;border:1px solid #ffe08a;padding:10px;border-radius:6px;font-size:14px}
.muted{color:#666;font-size:13px}.row{display:flex;justify-content:space-between;gap:12px;padding:4px 0;font-size:14px}
a{color:#1565c0}</style></head><body><h2>Máy điểm danh</h2>)HTML";

String esc(const String &s)
{
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++)
  {
    char c = s[i];
    switch (c)
    {
    case '&': o += "&amp;"; break;
    case '<': o += "&lt;"; break;
    case '>': o += "&gt;"; break;
    case '"': o += "&quot;"; break;
    case '\'': o += "&#39;"; break;
    default: o += c;
    }
  }
  return o;
}

String row(const String &k, const String &v)
{
  return "<div class='row'><span class='muted'>" + k + "</span><span>" + v + "</span></div>";
}

void sendPage(const String &body, int code = 200)
{
  String html = PAGE_HEAD;
  html += body;
  html += "</body></html>";
  server.send(code, "text/html; charset=utf-8", html);
}

void handleRoot()
{
  bool connected = WiFi.status() == WL_CONNECTED;

  String b = "<div class='card'>";
  b += row("Chế độ", apActive ? String("Cài đặt (WiFi ") + esc(apSsid) + ")" : String("Hoạt động"));
  b += row("WiFi", connected ? esc(WiFi.SSID()) + " (" + String(WiFi.RSSI()) + " dBm)" : String("Chưa kết nối"));
  b += row("IP trong LAN", connected ? WiFi.localIP().toString() : String("-"));
  b += row("Đầu đọc thẻ", rfidOk ? String("Hoạt động") : String("Lỗi"));
  if (lastClass.length())
    b += row("Lớp", esc(lastClass));
  b += "</div>";

  b += "<form class='card' method='POST' action='/save'>";
  b += "<div class='warn'>Chỉ hỗ trợ WiFi <b>2.4GHz</b>. Mạng 5GHz sẽ không kết nối được.</div>";
  b += "<label>Tên WiFi</label><input name='ssid' list='nets' required value='" + esc(cfg.ssid) + "'>";
  b += "<datalist id='nets'></datalist>";
  b += "<label>Mật khẩu WiFi</label><input name='pass' type='password' placeholder='" +
       String(cfg.ssid.length() ? "Để trống nếu giữ nguyên" : "Mật khẩu WiFi") + "'>";
  b += "<label>Mã máy đọc (key)</label><input name='key' required value='" + esc(cfg.key) + "'>";
  b += "<label>API URL</label><input name='api' value='" + esc(cfg.api) + "'>";
  b += "<div class='muted'>Giữ mặc định nếu không có yêu cầu thay đổi.</div>";
  b += "<button>Lưu và kết nối</button></form>";

  b += "<form class='card' method='POST' action='/reset' "
       "onsubmit=\"return confirm('Xoá toàn bộ cấu hình và khởi động lại?')\">"
       "<button style='background:#c62828;margin-top:0'>Xoá cấu hình</button></form>";

  b += "<p class='muted'>Firmware " + String(FW_VERSION) + " · " + esc(apSsid) + "</p>";
  b += "<script>fetch('/scan').then(r=>r.json()).then(a=>{const d=document.getElementById('nets');"
       "a.forEach(n=>{const o=document.createElement('option');o.value=n.s;o.label=n.s+' ('+n.r+' dBm)';"
       "d.appendChild(o)})}).catch(()=>{})</script>";

  sendPage(b);
}

void handleSave()
{
  Config n;
  n.ssid = server.arg("ssid");
  n.ssid.trim();
  n.pass = server.arg("pass");
  n.key = server.arg("key");
  n.key.trim();
  n.api = server.arg("api");
  n.api.trim();
  if (!n.api.length())
    n.api = DEFAULT_API_URL;

  if (!n.ssid.length() || !n.key.length())
  {
    sendPage("<div class='card'>Thiếu tên WiFi hoặc mã máy đọc. <a href='/'>Quay lại</a></div>", 400);
    return;
  }

  // Để trống mật khẩu + cùng WiFi = giữ mật khẩu cũ
  if (!n.pass.length() && n.ssid == cfg.ssid)
    n.pass = cfg.pass;

  bool wifiChanged = (n.ssid != cfg.ssid) || (n.pass != cfg.pass);
  bool connected = WiFi.status() == WL_CONNECTED;

  // Chỉ đổi key / API: áp dụng ngay
  if (!wifiChanged && connected)
  {
    cfg = n;
    saveConfig(cfg);
    sendPage("<div class='card'>Đã lưu cấu hình. <a href='/'>Quay lại</a></div>");
    if (!showingTemp)
      refreshIdle();
    return;
  }

  // Đổi WiFi khi đang vào qua mạng LAN: lưu rồi khởi động lại
  if (connected && !apActive)
  {
    saveConfig(n);
    sendPage("<div class='card'>Đã lưu. Thiết bị sẽ khởi động lại và vào WiFi <b>" + esc(n.ssid) +
             "</b>.<br><br>Xem IP mới trên màn hình thiết bị. Nếu sai mật khẩu, thiết bị sẽ tự bật lại "
             "WiFi cài đặt <b>" + esc(apSsid) + "</b>.</div>");
    restartAt = millis() + 1500;
    return;
  }

  // Đang ở chế độ cài đặt: thử kết nối trước, thành công mới lưu
  pendingCfg = n;
  pendingConnect = true;
  connectStatus = "connecting";

  String b = "<div class='card' id='s'>Đang kết nối tới WiFi <b>" + esc(n.ssid) +
             "</b>...<br><span class='muted'>Có thể xem trạng thái trên màn hình thiết bị.</span></div>";
  b += "<script>let n=0;const t=setInterval(()=>{fetch('/status').then(r=>r.json()).then(j=>{"
       "const s=document.getElementById('s');"
       "if(j.state==='ok'){clearInterval(t);s.innerHTML='<b>Kết nối thành công!</b><br><br>"
       "Trang quản trị trong mạng LAN: <b>http://'+j.ip+'</b><br>Chuyển điện thoại sang WiFi <b>'+j.ssid+"
       "'</b> rồi mở địa chỉ trên. WiFi cài đặt sẽ tự tắt sau 1 phút.';}"
       "else if(j.state==='fail'){clearInterval(t);s.innerHTML='<b>Không kết nối được.</b><br><br>"
       "Kiểm tra tên, mật khẩu và chắc chắn đó là WiFi 2.4GHz. <a href=\"/\">Thử lại</a>';}"
       "}).catch(()=>{});if(++n>60)clearInterval(t);},1500);</script>";
  sendPage(b);
}

void handleStatus()
{
  JsonDocument d;
  d["state"] = connectStatus;
  d["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  d["ssid"] = cfg.ssid;
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleScan()
{
  int n = WiFi.scanNetworks();
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  for (int i = 0; i < n; i++)
  {
    String s = WiFi.SSID(i);
    if (!s.length())
      continue;
    bool dup = false;
    for (int k = 0; k < i; k++)
      if (WiFi.SSID(k) == s)
      {
        dup = true;
        break;
      }
    if (dup)
      continue;
    JsonObject o = arr.add<JsonObject>();
    o["s"] = s;
    o["r"] = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleReset()
{
  clearConfig();
  sendPage("<div class='card'>Đã xoá cấu hình. Thiết bị đang khởi động lại...</div>");
  restartAt = millis() + 1500;
}

// Captive portal: mọi đường dẫn lạ đều chuyển về trang cài đặt
void handleNotFound()
{
  if (apActive)
  {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "Not found");
}

void setupWebServer()
{
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/reset", HTTP_POST, handleReset);
  server.onNotFound(handleNotFound);
}

// =====================================================
// Nút BOOT: giữ 5 giây để xoá cấu hình
// =====================================================
void resetButtonTick()
{
  static unsigned long pressedAt = 0;
  if (digitalRead(RESET_BTN_PIN) == LOW)
  {
    if (!pressedAt)
      pressedAt = millis();
    else if (millis() - pressedAt > RESET_HOLD_MS)
    {
      showLines("Đã xoá cấu hình", C_LATE, "Đang khởi động lại...", C_DIM);
      clearConfig();
      delay(1000);
      ESP.restart();
    }
  }
  else
  {
    pressedAt = 0;
  }
}

// =====================================================
// Quét thẻ
// =====================================================
void handleCard()
{
  if (!rfid.PICC_IsNewCardPresent())
    return;
  if (!rfid.PICC_ReadCardSerial())
    return;

  String uid = uidToString(rfid.uid);
  unsigned long now = millis();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (uid == lastUid && now - lastScanMs < SAME_CARD_COOLDOWN_MS)
  {
    lastScanMs = now;
    return;
  }
  lastUid = uid;
  lastScanMs = now;

  if (!isConfigured())
  {
    playSound(SND_BAD);
    return;
  }

  Serial.printf("\nUID: %s\n", uid.c_str());
  beepTap();
  showLines("Đang xử lý...", C_INFO, uid, C_DIM);
  showFooter("UID: " + uid);

  ScanResult r = sendScan(uid);
  if (r.className.length())
    lastClass = r.className;

  if (!r.netOk)
  {
    showLines("Lỗi kết nối", C_BAD, r.message, C_TEXT);
    playSound(SND_ERR);
  }
  else
  {
    uint16_t c = colorForKind(r.kind);
    if (r.name.length() && r.className.length())
      showLines(r.name, C_TEXT, "Lớp: " + r.className, C_INFO, r.message, c);
    else if (r.name.length())
      showLines(r.name, C_TEXT, r.message, c);
    else
      showLines(r.message, c);
    playForKind(r.kind);
  }

  showTemp(RESULT_SHOW_MS);
}

// =====================================================
// Main
// =====================================================
void setup()
{
  buzzerInit();
  Serial.begin(115200);
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  delay(300);

  tft.init();
  tft.setRotation(1);
  tft.setBrightness(200);
  tft.setFont(&fontVN);
  tft.fillScreen(C_BG);
  showLines("Đang khởi động...", C_INFO);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, RC522_SS);
  rfidInit();
  rfidOk = rfidCheck();
  Serial.printf("RC522: %s\n", rfidOk ? "OK" : "LOI");

  wdtInit();
  loadConfig();

  uint64_t mac = ESP.getEfuseMac();
  char buf[24];
  snprintf(buf, sizeof(buf), "DiemDanh-%04X", (uint16_t)(mac >> 32));
  apSsid = buf;

  WiFi.persistent(false); // cấu hình do mình tự lưu trong NVS
  WiFi.setHostname(apSsid.c_str());
  WiFi.setAutoReconnect(true);
  setupWebServer();

  if (isConfigured())
  {
    WiFi.mode(WIFI_STA);
    server.begin();
    drawHeader();
    showLines("Đang kết nối WiFi", C_LATE, cfg.ssid, C_TEXT);
    showFooter("Chỉ hỗ trợ WiFi 2.4GHz");

    if (!connectSTA(cfg.ssid, cfg.pass, WIFI_CONNECT_TIMEOUT_MS))
    {
      Serial.println("Khong vao duoc WiFi, bat che do cai dat");
      startAP();
      playSound(SND_ERR);
    }
  }
  else
  {
    startAP();
    server.begin();
  }

  lastWifiState = WiFi.status() == WL_CONNECTED;
  if (lastWifiState)
    Serial.printf("WiFi OK, IP: %s\n", WiFi.localIP().toString().c_str());

  drawHeader();
  refreshIdle();
}

void loop()
{
  serviceBackground();
  resetButtonTick();

  if (restartAt && millis() > restartAt)
    ESP.restart();

  if (pendingConnect)
    doPendingConnect();

  wifiTick();
  rfidHealthTick();

  if (showingTemp && millis() > tempUntil)
  {
    showingTemp = false;
    refreshIdle();
  }

  handleCard();
}