/* FastRobox Level 1: ESP32-S3-CAM dual core + sensor health + watchdog
 * Hardware layout uses the same pins as the user's WORKING RC522-only test.
 * IMPORTANT: Verify this exact pin layout fits your ESP32-S3-CAM variant.
 * Built-in one-pixel RGB: GPIO48, not three external LED channels.
 * Uses ESP32 Arduino core 3.x (ESP-IDF 5.x Task WDT API).
 */
#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <DHT.h>
#include <Adafruit_NeoPixel.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_err.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

#define DHT_PIN 1
#define BOOT_PIN 0
#define TEAM_ID "T13"  // change this to your team ID
#define FIRMWARE_VERSION "2.0.0"
#define FLAME_PIN 21
#define TOUCH_PIN 2
#define BUZZER_PIN 14
#define RGB_PIN 48
#define RFID_SS 42
#define RFID_RST 41
#define RFID_SCK 38
#define RFID_MISO 39
#define RFID_MOSI 40
#define DHT_TYPE DHT11

static const uint32_t DHT_PERIOD_MS = 5000;
static const uint32_t FLAME_PERIOD_MS = 100;
static const uint32_t RFID_PERIOD_MS = 200;
static const uint32_t LED_STEP_MS = 250; // 2 Hz heartbeat: 250 ms ON / 250 ms OFF
static const uint32_t PANIC_PERIOD_MS = 10000;
static const uint32_t DOOR_PERIOD_MS = 5000;
static const uint32_t FIRE_BUZZ_MS = 5000;
static const uint32_t RFID_BEEP_MS = 150;
static const uint32_t WDT_TIMEOUT_MS = 8000;

DHT dht(DHT_PIN, DHT_TYPE);
MFRC522 rfid(RFID_SS, RFID_RST);
Adafruit_NeoPixel rgb(1, RGB_PIN, NEO_GRB + NEO_KHZ800);

volatile bool fireDetected = false, dhtOK = false, rfidOK = false;
volatile bool panicActive = false, doorUnlocked = false;
volatile float tempC = NAN, humidity = NAN;
volatile uint32_t fireStart = 0, panicStart = 0, doorStart = 0;
volatile uint32_t touchValue = 0;
uint32_t touchThreshold = 0;
TaskHandle_t sensorTaskHandle = nullptr;
TaskHandle_t rfidTaskHandle = nullptr;
TaskHandle_t outputTaskHandle = nullptr;
String commandLine;
WebServer web(80);
Preferences pref;
String wifiSsid, wifiPassword, roomId, deviceToken, endpoint, caCert, apPassword, wifiError;
bool apActive=false, wasConnected=false, cloudOK=false, mdnsActive=false;
uint32_t offlineSince=0, nextRetry=0, retryGap=1000, wifiAttemptStarted=0;
uint32_t nextTelemetry=0, lastBootPress=0;
uint64_t currentSequence=0, reservedSequence=0;
char lastUID[32]="-";
struct PendingEvent { char type[24]; char detail[100]; };
QueueHandle_t eventQueue=nullptr;
PendingEvent activeEvent={};
bool eventPending=false;

// Includes Local setup page and live status page. Written to LittleFS once at startup.
const char LOCAL_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><meta charset="utf-8"><title>SmartRoom setup</title><style>body{font:16px system-ui;color:#e5f2ff;background:#081423;max-width:750px;margin:20px auto;padding:16px}h1{color:#9bdcf7}section{border:1px solid #36536e;background:#102439;padding:18px;margin:14px 0;border-radius:12px}input,textarea,button{display:block;width:100%;box-sizing:border-box;padding:11px;margin:8px 0;background:#172c44;color:white;border:1px solid #4a6c88;border-radius:6px}textarea{min-height:110px}pre{white-space:pre-wrap;overflow-wrap:anywhere}button{background:#207aa1;cursor:pointer}</style></head><body><h1>SmartRoom — <span id="team">Device</span></h1><section><h2>Live status</h2><pre id="readings">Loading...</pre></section><section><h2>Wi-Fi & secure cloud setup</h2><p>Connect to the ESP32 setup hotspot to save settings.</p><p id="issue"></p><button onclick="scan()">Scan nearby Wi-Fi</button><pre id="networks"></pre><form method="post" action="/save"><label>Wi-Fi Name (SSID)<input name="ssid" required maxlength="32"></label><label>Wi-Fi Password<input name="password" type="password" maxlength="63"></label><label>Room ID<input name="room" required pattern="[A-Za-z0-9_-]{1,32}"></label><label>HTTPS ingestion URL<input name="endpoint" required placeholder="https://your-service.onrender.com/api/ingest"></label><label>Device token<input name="token" type="password" required></label><label>Root CA Certificate (PEM; not private key)<textarea name="ca" required placeholder="-----BEGIN CERTIFICATE-----"></textarea></label><button>Save to ESP32 flash & restart</button></form></section><script>async function update(){try{let r=await fetch('/api/status');let j=await r.json();document.getElementById('readings').textContent=JSON.stringify(j,null,2);document.getElementById('team').textContent=j.deviceId;document.getElementById('issue').textContent=j.error||'';}catch(e){}}async function scan(){let r=await fetch('/scan');document.getElementById('networks').textContent=JSON.stringify(await r.json(),null,2)}update();setInterval(update,2000)</script></body></html>)HTML";

void queueEvent(const char* type, const char* detail) {
  if (!eventQueue) return;
  PendingEvent e={};
  snprintf(e.type,sizeof(e.type),"%s",type);
  snprintf(e.detail,sizeof(e.detail),"%s",detail);
  if (xQueueSend(eventQueue,&e,0)!=pdTRUE) Serial.println("[ERROR] event queue full");
}


void setLed(uint8_t r, uint8_t g, uint8_t b) {
  static uint32_t lastColor = 0xFFFFFFFF;
  uint32_t newColor = rgb.Color(r, g, b);
  if (newColor == lastColor) return;
  rgb.setPixelColor(0, newColor);
  rgb.show();
  lastColor = newColor;
}

const char* statusName() {
  if (fireDetected) return "FIRE";
  if (panicActive) return "PANIC";
  if (!dhtOK || !rfidOK) return "SENSOR_ERROR";
  if (doorUnlocked) return "ACCESS_GRANTED";
  return "SAFE";
}

const char* resetReason(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWER_ON";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_PANIC: return "CRASH";
    default: return "OTHER";
  }
}

void printStatus() {
  Serial.printf("[STATUS] up=%lus T=", millis()/1000UL);
  if (dhtOK) Serial.printf("%.1fC RH=%.0f%%", (double)tempC, (double)humidity);
  else Serial.print("- RH=-");
  Serial.printf(" flame=%u door=%s state=%s touch=%lu rfid=%s heap=%luk psram=%luk\n",
      fireDetected ? 1 : 0, doorUnlocked ? "UNLOCKED" : "LOCKED", statusName(),
      (unsigned long)touchValue, rfidOK ? "OK" : "ERROR",
      (unsigned long)(ESP.getFreeHeap()/1024),
      (unsigned long)(ESP.getFreePsram()/1024));
}

void sensorTask(void*) {
  esp_task_wdt_add(NULL);
  uint32_t lastDHT = millis() - DHT_PERIOD_MS;
  uint32_t lastFlame = millis() - FLAME_PERIOD_MS;
  uint32_t lastTouch = millis();
  bool prevFlame = false;
  bool prevDhtOK = false;
  bool touchHeld = false;
  for (;;) {
    uint32_t now = millis();
    if (now - lastFlame >= FLAME_PERIOD_MS) {
      lastFlame = now;
      bool f = digitalRead(FLAME_PIN) == LOW; // common 3-pin module: active LOW
      if (f != prevFlame) {
        Serial.printf("[EVENT] %s\n", f ? "FIRE" : "FIRE_CLEARED");
        if (f) fireStart = now;
        queueEvent(f ? "FIRE" : "FIRE_CLEARED", f ? "detected" : "cleared");
        prevFlame = f;
      }
      fireDetected = f;
    }
    if (now - lastDHT >= DHT_PERIOD_MS) {
      lastDHT = now;
      float t = dht.readTemperature();
      float h = dht.readHumidity();
      bool ok = !isnan(t) && !isnan(h) && h >= 0 && h <= 100;
      tempC = ok ? t : NAN;
      humidity = ok ? h : NAN;
      dhtOK = ok;
      if (ok != prevDhtOK) queueEvent(ok ? "SENSOR_RECOVERED" : "SENSOR_ERROR", "DHT11");
      if (ok != prevDhtOK) Serial.printf("[%s] %s\n", ok ? "EVENT" : "ERROR", ok ? "DHT11 RECOVERED" : "SENSOR ERROR: DHT11");
      prevDhtOK = ok;
    }
    if (now - lastTouch >= 50) {
      lastTouch = now;
      uint32_t sample = touchRead(TOUCH_PIN);
      touchValue = sample;
      bool touched = sample > touchThreshold;
      if (touched && !touchHeld && !panicActive) {
        panicStart = now;
        panicActive = true;
        Serial.println("[EVENT] PANIC");
        queueEvent("PANIC", "touch wire activated");
      }
      touchHeld = touched;
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void rfidTask(void*) {
  esp_task_wdt_add(NULL);
  uint32_t lastPoll = millis() - RFID_PERIOD_MS;
  uint32_t lastHealth = millis() - 2000;
  String lastCard = "";
  uint32_t lastTap = 0;
  bool wasOK = false;
  for (;;) {
    uint32_t now = millis();
    if (now - lastHealth >= 2000) {
      lastHealth = now;
      byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
      bool ok = version != 0 && version != 0xFF;
      rfidOK = ok;
      if (wasOK != ok) queueEvent(ok ? "SENSOR_RECOVERED" : "SENSOR_ERROR", "RFID");
      if (wasOK != ok) Serial.printf("[%s] %s\n", ok ? "EVENT" : "ERROR", ok ? "RFID RECOVERED" : "SENSOR ERROR: RFID");
      wasOK = ok;
    }
    if (now - lastPoll >= RFID_PERIOD_MS) {
      lastPoll = now;
      if (rfidOK && rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        String uid;
        for (byte i=0; i<rfid.uid.size; ++i) {
          if (rfid.uid.uidByte[i] < 0x10) uid += '0';
          uid += String(rfid.uid.uidByte[i], HEX);
        }
        uid.toUpperCase();
        if (uid != lastCard || now - lastTap > 2500) {
          lastCard = uid;
          lastTap = now;
          Serial.printf("[CARD] uid=%s\n", uid.c_str());
          snprintf(lastUID,sizeof(lastUID),"%s", uid.c_str());
          queueEvent("CARD_TAP", uid.c_str());
          // DEMO ONLY: a real installation must verify authorized UIDs.
          if (!fireDetected && !panicActive) {
            doorStart = now;
            doorUnlocked = true;
            Serial.println("[EVENT] ACCESS GRANTED; DOOR UNLOCKED (SIMULATED)");
          }
        }
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
      }
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void outputTask(void*) {
  esp_task_wdt_add(NULL);
  for (;;) {
    uint32_t now = millis();
    if (panicActive && now - panicStart >= PANIC_PERIOD_MS) {
      panicActive = false;
      Serial.println("[EVENT] PANIC ENDED");
    }
    if (doorUnlocked && now - doorStart >= DOOR_PERIOD_MS) {
      doorUnlocked = false;
      Serial.println("[EVENT] DOOR LOCKED");
    }
    bool buzzerOn = false;
    if (fireDetected) {
      uint32_t e = now - fireStart;
      if ((e / 100) % 2 == 0) setLed(255, 0, 0); else setLed(0, 0, 0);
      buzzerOn = e < FIRE_BUZZ_MS && (e / 200) % 2 == 0;
    } else if (panicActive) {
      uint32_t e = now - panicStart;
      if ((e / 100) % 2 == 0) setLed(255, 0, 0); else setLed(0, 0, 255);
      buzzerOn = (e / 100) % 2 == 0;
    } else if (doorUnlocked) {
      setLed(0, 255, 0);
      buzzerOn = now - doorStart < RFID_BEEP_MS;
    } else if (!dhtOK || !rfidOK) {
      if ((now / LED_STEP_MS) % 2 == 0) setLed(255, 0, 255); else setLed(0, 0, 0);
    } else if (WiFi.status()!=WL_CONNECTED) {
      if ((now / LED_STEP_MS) % 2 == 0) setLed(0, 0, 255); else setLed(0, 0, 0);
    } else {
      if ((now / LED_STEP_MS) % 2 == 0) setLed(0, 255, 0); else setLed(0, 0, 0);
    }
    digitalWrite(BUZZER_PIN, buzzerOn ? HIGH : LOW);
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


// =============== LEVEL 2: NVS / NETWORK / HTTPS =================
String escapeJson(const String& input) {
  String out; out.reserve(input.length()+8);
  for (size_t i=0; i<input.length(); ++i) {
    char c=input[i];
    if(c=='"'||c=='\\') {out+='\\';out+=c;}
    else if(c=='\n'||c=='\r'||c=='\t') out+=' ';
    else if((uint8_t)c >= 32) out+=c;
  }
  return out;
}
String jsonString(String v){return "\""+escapeJson(v)+"\"";}
String numOrNull(float n, int dec=1){return isfinite(n)?String(n,dec):"null";}
void startAP(){
  if(apActive) return;
  WiFi.mode(WIFI_AP_STA);
  IPAddress ip(192,168,10,1), mask(255,255,255,0);
  WiFi.softAPConfig(ip,ip,mask);
  String name="SmartRoom-"+String(TEAM_ID);
  apPassword=String((uint32_t)esp_random(),HEX)+String((uint32_t)esp_random(),HEX);
  apActive=WiFi.softAP(name.c_str(),apPassword.c_str());
  Serial.printf("[NET] setup AP=%s password=%s address=http://192.168.10.1\n",name.c_str(),apPassword.c_str());
  if(!apActive) Serial.println("[ERROR] could not start setup hotspot");
}
void stopAP(){if(apActive){WiFi.softAPdisconnect(true);apActive=false;Serial.println("[NET] setup AP stopped");}}
void clearSettings(){
  pref.clear();
  Serial.println("[NET] Wi-Fi and API credentials reset");
  delay(150); ESP.restart();
}
String statusJson(){
  time_t now=time(nullptr);
  struct tm t={}; localtime_r(&now,&t); char local[48];
  strftime(local,sizeof(local),"%Y-%m-%d %H:%M:%S",&t);
  String s="{\"deviceId\":"+jsonString(String(TEAM_ID)+"-D1")+",\"roomId\":"+jsonString(roomId);
  s+=",\"localTime\":"+jsonString(now>1700000000 ? String(local) : "NTP pending");
  s+=",\"tempC\":"+(dhtOK?numOrNull(tempC):"null");
  s+=",\"rh\":"+(dhtOK?numOrNull(humidity):"null");
  s+=",\"flame\":"+String(fireDetected?"true":"false");
  s+=",\"door\":"+jsonString(doorUnlocked?"UNLOCKED":"LOCKED");
  s+=",\"status\":"+jsonString(statusName());
  s+=",\"lastCardUID\":"+jsonString(String(lastUID));
  s+=",\"rssi\":"+(WiFi.status()==WL_CONNECTED?String(WiFi.RSSI()):"null");
  s+=",\"chipTempC\":"+numOrNull(temperatureRead());
  s+=",\"fw\":"+jsonString(FIRMWARE_VERSION);
  s+=",\"connection\":"+jsonString(WiFi.status()==WL_CONNECTED?(cloudOK?"LIVE":"WIFI_ONLY"):"OFFLINE");
  s+=",\"error\":"+jsonString(wifiError)+"}";
  return s;
}
void setupWeb(){
  if(LittleFS.begin(true)){
    if(!LittleFS.exists("/index.html")){
      File f=LittleFS.open("/index.html","w");
      if(f){f.print(FPSTR(LOCAL_HTML));f.close();}
    }
  }else Serial.println("[ERROR] LittleFS failed");
  web.on("/",HTTP_GET,[](){
    File f=LittleFS.open("/index.html","r");
    if(!f){web.send(500,"text/plain","LittleFS page missing");return;}
    web.streamFile(f,"text/html");f.close();
  });
  web.on("/api/status",HTTP_GET,[](){web.sendHeader("Cache-Control","no-store");web.send(200,"application/json",statusJson());});
  web.on("/scan",HTTP_GET,[](){
    if(!apActive){web.send(403,"application/json","{\"error\":\"setup mode only\"}");return;}
    int found=WiFi.scanNetworks(false,true);
    String result="[";
    for(int i=0;i<found;i++){
      if(i)result+=",";
      result+="{\"ssid\":"+jsonString(WiFi.SSID(i))+",\"rssi\":"+String(WiFi.RSSI(i))+"}";
    }
    result+="]";WiFi.scanDelete();web.send(200,"application/json",result);
  });
  web.on("/save",HTTP_POST,[](){
    // Restrict credential setup to clients using the device's AP interface.
    if(!apActive || web.client().localIP()!=WiFi.softAPIP()){
      web.send(403,"text/plain","Connect to SmartRoom setup hotspot");return;
    }
    String ssid=web.arg("ssid"), pass=web.arg("password"), room=web.arg("room");
    String url=web.arg("endpoint"), token=web.arg("token"), ca=web.arg("ca");
    bool validRoom=room.length()>0 && room.length()<=32;
    for(char ch:room) if(!isalnum((unsigned char)ch)&&ch!='_'&&ch!='-')validRoom=false;
    if(ssid.isEmpty() || !validRoom || !url.startsWith("https://") || url.length()>240 ||
       token.length()<16 || token.length()>256 || !ca.startsWith("-----BEGIN CERTIFICATE-----") || ca.indexOf("-----END CERTIFICATE-----")<0 || ca.length()>6000){
      web.send(400,"text/plain","Invalid SSID / Room ID / HTTPS URL / token / root CA");return;
    }
    pref.putString("ssid",ssid);pref.putString("pass",pass);pref.putString("room",room);
    pref.putString("url",url);pref.putString("token",token);pref.putString("ca",ca);
    web.send(200,"text/plain","Saved. Rebooting. Reconnect to the classroom Wi-Fi.");
    delay(200);ESP.restart();
  });
  web.begin();
}
void loadSettings(){
  wifiSsid=pref.getString("ssid","");wifiPassword=pref.getString("pass","");
  roomId=pref.getString("room","UNSET");endpoint=pref.getString("url","");
  deviceToken=pref.getString("token","");caCert=pref.getString("ca","");
}
void beginWiFi(){
  if(wifiSsid.isEmpty()){startAP();return;}
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(wifiSsid.c_str(),wifiPassword.c_str());
  wifiAttemptStarted=millis();offlineSince=millis();retryGap=7000;nextRetry=millis()+7000;
  Serial.printf("[NET] connecting to %s\n",wifiSsid.c_str());
}
void networkTick(uint32_t now){
  bool connected=WiFi.status()==WL_CONNECTED;
  if(connected){
    if(!wasConnected){
      Serial.printf("[NET] connected IP=%s\n",WiFi.localIP().toString().c_str());
      wasConnected=true;retryGap=1000;offlineSince=0;wifiError="";cloudOK=false;
      if(!mdnsActive){String host=String("classroom-")+TEAM_ID;host.toLowerCase();
        mdnsActive=MDNS.begin(host.c_str());
        if(mdnsActive) Serial.printf("[NET] http://%s.local\n",host.c_str());}
      configTzTime("BDT-6","pool.ntp.org","time.google.com");
      stopAP();
    }
  }else{
    cloudOK=false;
    if(wasConnected){Serial.println("[NET] Wi-Fi disconnected");wasConnected=false;wifiError="Wi-Fi disconnected";}
    if(!offlineSince) offlineSince=now;
    if(wifiSsid.isEmpty()) {startAP();return;}
    if((int32_t)(now-nextRetry)>=0){
      Serial.printf("[NET] reconnect attempt, retry gap=%lums\n",(unsigned long)retryGap);
      WiFi.disconnect(false,false);
      WiFi.begin(wifiSsid.c_str(),wifiPassword.c_str());
      nextRetry=now+retryGap;
      retryGap=min(retryGap*2UL,30000UL);
    }
    if(now-offlineSince>=15000 && !apActive){
      wifiError="Wi-Fi unavailable or password incorrect. Check settings.";
      startAP();
    }
  }
}
void reserveSeq(){
  if(currentSequence>=reservedSequence){
    reservedSequence=currentSequence+512;
    pref.putULong64("seqHigh",reservedSequence); // persist NEXT ALLOCATABLE BLOCK BEFORE use
  }
}
uint64_t nextSeq(){reserveSeq();return ++currentSequence;}
String uint64String(uint64_t value){char buf[24];snprintf(buf,sizeof(buf),"%llu",(unsigned long long)value);return String(buf);}
String eventPayload(const PendingEvent& e){
  String s="{\"kind\":\"EVENT\",\"deviceId\":"+jsonString(String(TEAM_ID)+"-D1");
  s+=",\"roomId\":"+jsonString(roomId)+",\"seq\":"+uint64String(nextSeq());
  s+=",\"ts\":"+String((unsigned long)time(nullptr));
  s+=",\"type\":"+jsonString(e.type)+",\"detail\":"+jsonString(e.detail)+"}";
  return s;
}
String telemetryPayload(){
  String s="{\"kind\":\"TELEMETRY\",\"deviceId\":"+jsonString(String(TEAM_ID)+"-D1");
  s+=",\"roomId\":"+jsonString(roomId)+",\"seq\":"+uint64String(nextSeq());
  s+=",\"ts\":"+String((unsigned long)time(nullptr));
  s+=",\"uptimeS\":"+String(millis()/1000UL);
  s+=",\"tempC\":"+(dhtOK?numOrNull(tempC):"null");
  s+=",\"rh\":"+(dhtOK?numOrNull(humidity):"null");
  float hi=dhtOK?dht.computeHeatIndex((float)tempC,(float)humidity,false):NAN;
  s+=",\"heatIndexC\":"+numOrNull(hi);
  s+=",\"flame\":"+String(fireDetected?"true":"false");
  s+=",\"door\":"+jsonString(doorUnlocked?"UNLOCKED":"LOCKED");
  s+=",\"status\":"+jsonString(statusName());
  s+=",\"heatTrend\":\"UNKNOWN\"";
  s+=",\"rssi\":"+String(WiFi.RSSI());
  s+=",\"heapKb\":"+String(ESP.getFreeHeap()/1024);
  s+=",\"chipTempC\":"+numOrNull(temperatureRead());
  s+=",\"fw\":"+jsonString(FIRMWARE_VERSION);
  s+=",\"quality\":"+jsonString((dhtOK&&rfidOK)?"good":"bad")+"}";
  return s;
}
// TLS CA is provisioned on the local setup page (never call setInsecure).
int securePost(const String& payload){
  if(!endpoint.startsWith("https://")||caCert.length()<100||deviceToken.length()<16)return -99;
  WiFiClientSecure client;
  client.setCACert(caCert.c_str());client.setTimeout(3500);
  HTTPClient http;
  if(!http.begin(client,endpoint))return -98;
  http.setTimeout(3500);
  http.addHeader("Content-Type","application/json");
  http.addHeader("X-Device-Token",deviceToken);
  int status=http.POST(payload);
  http.end();
  cloudOK=(status==201||status==409);
  return status;
}
void cloudTick(uint32_t now){
  if(WiFi.status()!=WL_CONNECTED||time(nullptr)<1700000000)return;
  static uint32_t nextEventTry=0;
  if(!eventPending && xQueueReceive(eventQueue,&activeEvent,0)==pdTRUE) nextEventTry=0;
  if((eventPending || activeEvent.type[0]) && (int32_t)(now-nextEventTry)>=0){
    // Do not allocate a new sequence number on retries.
    static String pendingPayload="";
    if(!eventPending){eventPending=true;pendingPayload=eventPayload(activeEvent);}
    int status=securePost(pendingPayload);
    if(status==201||status==409){
      Serial.printf("[NET] event %s stored/replayed (%d)\n",activeEvent.type,status);
      eventPending=false;pendingPayload="";activeEvent.type[0]=0;
    }else { Serial.printf("[NET] event upload failed HTTP=%d\n",status); nextEventTry=millis()+4000; }
  }
  if((int32_t)(now-nextTelemetry)>=0){
    nextTelemetry=millis()+5000;
    int status=securePost(telemetryPayload());
    Serial.printf("[NET] telemetry HTTP=%d, WiFi RSSI=%d\n",status,WiFi.RSSI());
  }
}
void checkBootHold(uint32_t now){
  if(digitalRead(BOOT_PIN)==LOW){
    if(lastBootPress==0)lastBootPress=now;
    if(now-lastBootPress>5000)clearSettings();
  }else lastBootPress=0;
}

void printHealth() {
  Serial.printf("[CMD] chipTemp=%.1fC heap=%lu psram=%lu flash=%lu MHz=%lu uptime=%lus reset=%s\n",
    (double)temperatureRead(), (unsigned long)ESP.getFreeHeap(),
    (unsigned long)ESP.getFreePsram(), (unsigned long)ESP.getFlashChipSize(),
    (unsigned long)ESP.getCpuFreqMHz(), millis()/1000UL,
    resetReason(esp_reset_reason()));
}
void printTasks() {
  Serial.printf("[CMD] sensors core=0 stackFree=%lu\n", (unsigned long)uxTaskGetStackHighWaterMark(sensorTaskHandle));
  Serial.printf("[CMD] rfid core=1 stackFree=%lu\n", (unsigned long)uxTaskGetStackHighWaterMark(rfidTaskHandle));
  Serial.printf("[CMD] led+buzzer core=1 stackFree=%lu\n", (unsigned long)uxTaskGetStackHighWaterMark(outputTaskHandle));
}
void handleCommand(String cmd) {
  cmd.trim(); cmd.toLowerCase();
  if (cmd == "help") Serial.println("[CMD] help status health tasks ver hang reset");
  else if (cmd == "status") printStatus();
  else if (cmd == "health") printHealth();
  else if (cmd == "tasks") printTasks();
  else if (cmd == "reset") clearSettings();
  else if (cmd == "ver") Serial.printf("[CMD] firmware=%s build=%s %s\n", FIRMWARE_VERSION, __DATE__, __TIME__);
  else if (cmd == "hang") {
    Serial.println("[CMD] Simulated loop-task freeze; watchdog should reset device (~8 seconds)");
    Serial.flush();
    for (;;) vTaskDelay(pdMS_TO_TICKS(100)); // deliberately STOP feeding loop task WDT
  }
  else if (cmd.length()) Serial.println("[CMD] unknown; type help");
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.printf("[EVENT] BOOT reason=%s\n", resetReason(esp_reset_reason()));
  pinMode(BOOT_PIN, INPUT_PULLUP);
  pinMode(FLAME_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  rgb.begin(); rgb.setBrightness(60); rgb.clear(); rgb.show();
  dht.begin();
  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  rfid.PCD_Init();
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  rfidOK = version != 0 && version != 0xFF;
  Serial.printf("[NET] RFID version=0x%02X (%s)\n", version, rfidOK ? "responding" : "NOT responding");
  pref.begin("fastroom",false);
  currentSequence=pref.getULong64("seqHigh",0);reservedSequence=currentSequence;
  loadSettings();
  eventQueue=xQueueCreate(16,sizeof(PendingEvent));
  setupWeb();
  beginWiFi();
  queueEvent("BOOT",resetReason(esp_reset_reason()));
  nextTelemetry=millis()+5000;
  // Calibrate before creating sensor task. Do not touch GPIO2 here.
  uint64_t total=0;
  Serial.println("[EVENT] Calibrating touch; keep wire untouched");
  for (int i=0;i<30;++i) { total += touchRead(TOUCH_PIN); delay(20); }
  uint32_t baseline = total/30;
  touchThreshold = baseline + max((uint32_t)1000, baseline/4);
  Serial.printf("[EVENT] Touch baseline=%lu threshold=%lu\n", (unsigned long)baseline, (unsigned long)touchThreshold);
  fireDetected = digitalRead(FLAME_PIN) == LOW;
  if (fireDetected) fireStart = millis();

  esp_task_wdt_config_t wdtCfg = {};
  wdtCfg.timeout_ms = WDT_TIMEOUT_MS;
  wdtCfg.idle_core_mask = 0; // only explicit tasks below; avoid idle task conflicts
  wdtCfg.trigger_panic = true; // reboot on watchdog timeout
  esp_err_t result = esp_task_wdt_init(&wdtCfg);
  if (result == ESP_ERR_INVALID_STATE) result = esp_task_wdt_reconfigure(&wdtCfg);
  Serial.printf("[EVENT] Watchdog configure: %s\n", esp_err_to_name(result));
  esp_err_t addResult = esp_task_wdt_add(NULL); // Arduino loop task
  Serial.printf("[EVENT] Watchdog subscribe loop: %s\n", esp_err_to_name(addResult));
  BaseType_t a=xTaskCreatePinnedToCore(sensorTask, "sensors", 4096, NULL, 2, &sensorTaskHandle, 0);
  BaseType_t b=xTaskCreatePinnedToCore(rfidTask, "rfid", 4096, NULL, 1, &rfidTaskHandle, 1);
  BaseType_t c=xTaskCreatePinnedToCore(outputTask, "led_buzzer", 4096, NULL, 1, &outputTaskHandle, 1);
  Serial.printf("[EVENT] FreeRTOS tasks: %s\n", (a==pdPASS && b==pdPASS && c==pdPASS) ? "CREATED" : "FAILED");
  Serial.println("[EVENT] SYSTEM READY; commands: help, status, health, tasks, ver, hang, reset");
}

void loop() {
  static uint32_t lastStatus=0;
  static uint32_t lastNetwork=0;
  static uint32_t nextUpload=0;
  web.handleClient();
  if(millis()-lastNetwork>=250){lastNetwork=millis();networkTick(lastNetwork);checkBootHold(lastNetwork);}
  if((int32_t)(millis()-nextUpload)>=0){ nextUpload=millis()+250;cloudTick(millis()); }
  uint32_t now=millis();
  if (now-lastStatus>=1000) { lastStatus=now; printStatus(); }
  while (Serial.available()) {
    char ch=(char)Serial.read();
    if (ch=='\n') { handleCommand(commandLine); commandLine=""; }
    else if (ch!='\r' && commandLine.length()<64) commandLine+=ch;
  }
  esp_task_wdt_reset();
  delay(10);
}
