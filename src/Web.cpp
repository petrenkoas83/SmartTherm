/* Web.cpp - portal/system glue only; pages are in src/Web/ */

#if defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
using WiFiWebServer = ESP8266WebServer;
#define FORMAT_ON_FAIL
#elif defined(ARDUINO_ARCH_ESP32)
#include <WiFi.h>
#include <WebServer.h>
#include <esp_task_wdt.h>
using WiFiWebServer = WebServer;
#define FORMAT_ON_FAIL  true
#if defined(USE_HTTPS)
#include <HTTPSServer.hpp>
#include <SSLCert.hpp>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
using namespace httpsserver;
#endif
#endif

#include <time.h>
#include <AutoConnect.h>
#include <AutoConnectFS.h>
#include "OpenTherm.h"
#include "SmartDevice.hpp"
#include "SD_OpenTherm.hpp"
#include "Web/Shared.hpp"

AutoConnectFS::FS& FlashFS = AUTOCONNECT_APPLIED_FILESYSTEM;

extern SD_Termo SmOT;
int WiFiDebugInfo[10] ={0,0,0,0,0, 0,0,0,0,0};
unsigned int OTDebugInfo[12] ={0,0,0,0,0, 0,0,0,0,0, 0,0};
extern OpenThermID OT_ids[N_OT_NIDS];
unsigned int OTcount = 0;

AutoConnectConfig config;
AutoConnect portal;
unsigned long authRealmCounter = 0; // Счетчик для изменения realm при disconnect

#if defined(ARDUINO_ARCH_ESP32) && defined(USE_HTTPS)
static SSLCert* g_httpsCert = nullptr;
static HTTPSServer* g_httpsServer = nullptr;
static WiFiServer* g_httpRedirectServer = nullptr;  // port 80 -> redirect to https
static const uint16_t HTTP_BACKEND_PORT = 8080;     // AutoConnect on 8080 when USE_HTTPS

static void handleHttpsProxy(HTTPRequest* req, HTTPResponse* res) {
  Serial_db.printf("[HTTPS proxy] free heap %u\n", (unsigned)ESP.getFreeHeap());
  WiFiClient backend;
  IPAddress backendAddr = (WiFi.getMode() & WIFI_STA) && (WiFi.status() == WL_CONNECTED) ? WiFi.localIP() : WiFi.softAPIP();
  Serial_db.printf("[HTTPS proxy] connecting to %s:%u\n", backendAddr.toString().c_str(), (unsigned)HTTP_BACKEND_PORT);
  if (!backend.connect(backendAddr, HTTP_BACKEND_PORT, 5000)) {
    Serial_db.printf("[HTTPS proxy] backend connect failed\n");
    res->setStatusCode(502);
    res->setStatusText("Bad Gateway");
    res->setHeader("Content-Type", "text/plain");
    res->println("Backend unreachable");
    req->discardRequestBody();
    return;
  }
  Serial_db.printf("[HTTPS proxy] connected, sending request\n");
  String reqLine = String(req->getMethod().c_str()) + " " + String(req->getRequestString().c_str()) + " HTTP/1.1\r\n";
  backend.print(reqLine);
  backend.print("Host: ");
  backend.print(backendAddr);
  backend.print(":");
  backend.print(HTTP_BACKEND_PORT);
  backend.print("\r\n");
  std::string auth = req->getHeader("Authorization");
  if (!auth.empty()) {
    backend.print("Authorization: ");
    backend.println(auth.c_str());
  }
  std::string ct = req->getHeader("Content-Type");
  if (!ct.empty()) {
    backend.print("Content-Type: ");
    backend.println(ct.c_str());
  }
  size_t cl = req->getContentLength();
  if (cl > 0 && cl < 65536) {
    backend.print("Content-Length: ");
    backend.print(cl);
    backend.print("\r\n");
  }
  backend.print("Connection: close\r\n\r\n");
  if (cl > 0 && cl < 65536) {
    byte buf[256];
    while (cl > 0) {
      size_t n = req->readBytes(buf, cl < sizeof(buf) ? cl : sizeof(buf));
      if (n == 0) break;
      backend.write(buf, n);
      cl -= n;
    }
  } else {
    req->discardRequestBody();
  }
  backend.flush();
  Serial_db.printf("[HTTPS proxy] request sent, waiting for response\n");
  unsigned long t0 = millis();
  while (!backend.available() && backend.connected() && millis() - t0 < 10000) {
    portal.handleClient();  // даём HTTP-серверу обработать соединение на 8080
    esp_task_wdt_reset();
    yield();
    delay(1);
  }
  if (!backend.available()) {
    Serial_db.printf("[HTTPS proxy] backend timeout (no data)\n");
    res->setStatusCode(502);
    res->setStatusText("Bad Gateway");
    res->setHeader("Content-Type", "text/plain");
    res->println("Backend timeout");
    backend.stop();
    return;
  }
  Serial_db.printf("[HTTPS proxy] first bytes received, reading status\n");
  String statusLine = backend.readStringUntil('\n');
  statusLine.trim();
  int code = 200;
  if (statusLine.indexOf("HTTP/") == 0) {
    int space = statusLine.indexOf(' ', 5);
    if (space > 0) code = statusLine.substring(5, space).toInt();
  }
  res->setStatusCode(code);
  int space2 = statusLine.indexOf(' ', statusLine.indexOf(' ') + 1);
  if (space2 > 0) res->setStatusText(statusLine.substring(space2 + 1).c_str());
  String headerLine;
  long contentLength = -1;
  String contentType;
  for (;;) {
    headerLine = backend.readStringUntil('\n');
    if (headerLine == "\r" || headerLine.length() == 0) break;
    headerLine.trim();
    if (headerLine.startsWith("Content-Type:")) {
      contentType = headerLine.substring(13);
      contentType.trim();
    } else if (headerLine.startsWith("Content-Length:")) {
      contentLength = headerLine.substring(15).toInt();
    }
  }
  Serial_db.printf("[HTTPS proxy] headers done, body len=%ld\n", (long)contentLength);
  if (contentType.length() > 0) res->setHeader("Content-Type", contentType.c_str());
  if (contentLength >= 0) {
    byte buf[512];
    unsigned long bodyStart = millis();
    while (contentLength > 0) {
      esp_task_wdt_reset();
      if (!backend.connected()) break;  // бэкенд закрыл соединение
      size_t toRead = (size_t)(contentLength < (long)sizeof(buf) ? contentLength : sizeof(buf));
      size_t n = backend.readBytes(buf, toRead);
      if (n == 0) {
        if (millis() - bodyStart > 15000) break;
        portal.handleClient();
        yield();
        delay(1);
        continue;
      }
      bodyStart = millis();
      res->write(buf, n);
      contentLength -= (long)n;
      yield();
    }
  } else {
    // Нет Content-Length: читаем до закрытия соединения, но не дольше таймаута
    const unsigned long BODY_NO_CLEN_TIMEOUT_MS = 20000;
    unsigned long lastDataAt = millis();
    while (backend.connected() || backend.available()) {
      esp_task_wdt_reset();
      if (backend.available()) {
        res->write(backend.read());
        lastDataAt = millis();
      } else {
        if (millis() - lastDataAt > BODY_NO_CLEN_TIMEOUT_MS) break;
        portal.handleClient();
        yield();
        delay(1);
      }
    }
  }
  Serial_db.printf("[HTTPS proxy] body done, heap %u, calling finalize\n", (unsigned)ESP.getFreeHeap());
  esp_task_wdt_reset();
  res->finalize();
  Serial_db.printf("[HTTPS proxy] done\n");
  backend.stop();
}
#endif

// Forward
void onRoot(void);
void onConnect(IPAddress& ipaddr);
int setup_web_common_onconnect(void);
void check_fs(void);
void loop_web(void);
int OutUTCtime(time_t now);
extern void onOTAstart(void);
extern void exitOTAError(uint8_t err);
extern void OTloop_callback(void);
extern int ST_setCpuFrequencyMhz(int code);
extern unsigned short int bootCount, bootReason, bootSts, bootSts1, bootSts2;
#if MQTT_USE
extern void mqtt_loop(void);
extern void mqtt_start(void);
#endif

String utc_time_jc;

void setup_web_common(void) {
  // Настройка аутентификации ПЕРЕД регистрацией страниц
  config.ota = AC_OTA_BUILTIN;
  config.portalTimeout = 1;
  config.retainPortal = true;
  config.autoRise = true;
  config.autoReconnect = true;
  config.reconnectInterval = 1;
  config.menuItems = config.menuItems | AC_MENUITEM_DELETESSID;
  
  // Настройка аутентификации согласно документации AutoConnect
  // AC_AUTHSCOPE_AUX - защищает все кастомные страницы (AUX)
  // AC_AUTHSCOPE_PORTAL - защищает все страницы (AutoConnect + AUX)
  // AC_AUTHSCOPE_WITHCP - позволяет аутентификацию в режиме captive portal
  config.auth = AC_AUTH_BASIC;  // Используем BASIC аутентификацию
  config.authScope = AC_AUTHSCOPE_AUX | AC_AUTHSCOPE_WITHCP;  // Защищаем все кастомные страницы (AUX)
  
  // Используем сохраненные учетные данные или значения по умолчанию
  // Данные уже должны быть загружены из файловой системы в setup_read_config()
  Serial_db.printf("[setup_web_common] SmOT.web_auth_username='%s', SmOT.web_auth_password='%s'\n",
                    SmOT.web_auth_username, SmOT.web_auth_password);
  if (SmOT.web_auth_username[0] != 0) {
    config.username = SmOT.web_auth_username;
  } else {
    config.username = "admin";  // Имя пользователя по умолчанию
  }
  
  if (SmOT.web_auth_password[0] != 0) {
    config.password = SmOT.web_auth_password;
  } else {
    config.password = "admin";  // Пароль по умолчанию
  }
  
  Serial_db.printf("WiFi AP SSID %s psk=%s\n", config.apid.c_str(), config.psk.c_str());
  Serial_db.printf("Web authentication: username=%s, password=%s, authScope=0x%04X\n", 
                    config.username.c_str(), config.password.c_str(), config.authScope);
  
  // Настраиваем портал перед регистрацией страниц
  portal.config(config);
  portal.onOTAStart(onOTAstart);
  portal.onOTAError(exitOTAError);
  portal.onConnect(onConnect);
  
  // Регистрируем страницы после настройки конфигурации
  RegisterWebPages(portal);
  
  /* When using AutoConnect with max_time_use support: portal.max_time_use = 200; portal.callback_at_maxtime = OTloop_callback; */

  portal.begin();

#if defined(ARDUINO_ARCH_ESP32) && defined(USE_HTTPS)
  // HTTPS-сервер на порту 443: редирект на HTTP (AutoConnect работает только по HTTP)
#if defined(USE_HTTPS_PRECOMPILED_CERT)
  #include "cert_embed.h"
  g_httpsCert = new SSLCert(
    (unsigned char*)https_cert_der, (uint16_t)https_cert_der_len,
    (unsigned char*)https_key_der, (uint16_t)https_key_der_len);
#else
  Serial_db.printf("HTTPS: creating self-signed certificate (may take up to 1 min)...\n");
  g_httpsCert = new SSLCert();
  int cr = createSelfSignedCert(*g_httpsCert, KEYSIZE_1024, "CN=SmartTherm.local,O=SmartTherm,C=RU", "20200101000000", "20301231235959");
  if (cr != 0) {
    Serial_db.printf("HTTPS: certificate creation failed, code=0x%02X\n", cr);
    delete g_httpsCert;
    g_httpsCert = nullptr;
  }
#endif
  if (g_httpsCert != nullptr) {
    g_httpsServer = new HTTPSServer(g_httpsCert);
    ResourceNode* nodeProxy = new ResourceNode("", "GET", (HTTPSCallbackFunction*)&handleHttpsProxy);
    g_httpsServer->setDefaultNode(nodeProxy);
    g_httpRedirectServer = new WiFiServer(80);
    g_httpRedirectServer->begin();
    g_httpsServer->start();
    if (g_httpsServer->isRunning()) {
      Serial_db.printf("HTTPS: 443 proxy -> localhost:%u, port 80 redirect -> https\n", (unsigned)HTTP_BACKEND_PORT);
    } else {
      Serial_db.printf("HTTPS: server start failed\n");
      delete g_httpsServer;
      g_httpsServer = nullptr;
    }
  }
#endif

  WiFiWebServer&  webServer = portal.host();
  
  // Добавляем логирование всех необработанных запросов для отладки
  webServer.onNotFound([]() {
    WiFiWebServer& ws = portal.host();
    Serial_db.printf("[onNotFound] Request: method=%d, URI=%s\n", ws.method(), ws.uri().c_str());
    Serial_db.printf("[onNotFound] Args count: %d\n", ws.args());
    for (int i = 0; i < ws.args(); i++) {
      Serial_db.printf("[onNotFound] Arg[%d]: %s=%s\n", i, ws.argName(i).c_str(), ws.arg(i).c_str());
    }
    // Проверяем специально для set_par
    if (ws.uri() == SET_PAR_URI || ws.uri() == SET_ADD_URI) {
      Serial_db.printf("[onNotFound] ВАЖНО: Запрос на %s попал в onNotFound! Это означает, что страница не зарегистрирована!\n", ws.uri().c_str());
    }
  });
  
  webServer.on("/", onRoot);
  
  // Обработчик для страницы disconnect - принудительно "отключает" пользователя
  // Используем агрессивный подход для очистки кэша браузера
  webServer.on("/_ac/disc", HTTP_GET, []() {
    WiFiWebServer& ws = portal.host();
    
    // Увеличиваем счетчик realm для изменения realm
    authRealmCounter++;
    
    // Отправляем HTML страницу с JavaScript, которая заставит браузер забыть кэш
    // и запросить аутентификацию заново
    String realm = "AutoConnect_" + String(authRealmCounter);
    String html = "<!DOCTYPE html><html><head><title>Disconnected</title>";
    html += "<script>";
    html += "// Очищаем кэш браузера для этого домена";
    html += "if ('caches' in window) { caches.keys().then(function(names) {";
    html += "  for (let name of names) caches.delete(name);";
    html += "}); }";
    html += "// Используем XMLHttpRequest с неправильными учетными данными для очистки кэша";
    html += "var xhr = new XMLHttpRequest();";
    html += "xhr.open('GET', '/', false);";
    html += "xhr.setRequestHeader('Authorization', 'Basic ' + btoa('invalid:invalid'));";
    html += "try { xhr.send(); } catch(e) {}";
    html += "// Перенаправляем на корневую страницу с новым realm";
    html += "setTimeout(function() {";
    html += "  window.location.href = '/?logout=' + Date.now();";
    html += "}, 100);";
    html += "</script>";
    html += "<body><h1>Disconnected</h1><p>Please wait...</p></body></html>";
    
    ws.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, private, max-age=0");
    ws.sendHeader("Pragma", "no-cache");
    ws.sendHeader("Expires", "Thu, 01 Jan 1970 00:00:00 GMT");
    ws.sendHeader("WWW-Authenticate", "Basic realm=\"" + realm + "\"");
    ws.send(200, "text/html", html);
  });

  if (WiFi.status() != WL_CONNECTED)  {
    Serial_db.printf("WiFi Not connected\n");
    WiFi.setAutoReconnect(true);
  }

#if defined(ARDUINO_ARCH_ESP8266)
  if(WiFi.getMode() == WIFI_OFF) {
    wifi_get_macaddr(STATION_IF, SmOT.Mac);
  } else {
    wifi_get_macaddr(STATION_IF, SmOT.Mac);
  }
#elif defined(ARDUINO_ARCH_ESP32)
  if(WiFi.getMode() == WIFI_MODE_NULL){
    esp_read_mac(SmOT.Mac, ESP_MAC_WIFI_STA);
  }
  else{
    esp_wifi_get_mac(WIFI_IF_STA, SmOT.Mac);
  }
#endif
}

#include "esp_sntp.h"

void time_sync_notification_cb(struct timeval *tv) {
  Serial_db.printf("Time updated, Unix time: %ld\n", tv->tv_sec);
}

int setup_web_common_onconnect(void) {
  static int init = 0;
  int rc;
  Serial_db.printf("WiFi connected, SSID: %s IP address: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  sprintf(SmOT.LocalUrl,"http://%s", WiFi.localIP().toString().c_str());
  Serial_db.printf("WiFi mode = %d\n", WiFi.getMode());
  if(init)
    return 1;

  WiFi.setAutoReconnect(true);

  const char*  const _ntp1 = "europe.pool.ntp.org";
  const char*  const _ntp2 = "pool.ntp.org";
  configTzTime("UTC0", _ntp1 ,_ntp2);
#if defined(ARDUINO_ARCH_ESP32)
  Serial_db.printf("Sync time in ms: %d\n", sntp_get_sync_interval());
#endif
  esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);

#if MQTT_USE
  // Read_mqtt_fs() уже был вызван в setup_read_config(), но вызываем снова для обновления MQTT настроек
  Serial_db.printf("Read_mqtt_fs (onconnect):\n");
  rc = SmOT.Read_mqtt_fs();
  SmOT.stsMQTTcfg = rc;
  Serial_db.printf("SmOT.Read_mqtt_fs() rc = %d\n", rc);
#endif

  // Обновляем конфигурацию AutoConnect с загруженными учетными данными (на случай, если они изменились)
  // ВАЖНО: Это должно происходить всегда, независимо от MQTT_USE
  // Перезагружаем веб-аутентификацию из файла на случай, если файл был обновлен
  SmOT.Read_web_auth_fs();
  
  if (SmOT.web_auth_username[0] != 0 && config.username != SmOT.web_auth_username) {
    config.username = SmOT.web_auth_username;
    Serial_db.printf("[setup_web_common_onconnect] Updated config.username from filesystem: %s\n", config.username.c_str());
    portal.config(config);
  }
  if (SmOT.web_auth_password[0] != 0 && config.password != SmOT.web_auth_password) {
    config.password = SmOT.web_auth_password;
    Serial_db.printf("[setup_web_common_onconnect] Updated config.password from filesystem: %s\n", config.password.c_str());
    portal.config(config);
  }

  init = 1;
  return 0;
}

void onConnect(IPAddress& ipaddr) {
  Serial_db.printf("onConnect %s portalStatus = %d\n", ipaddr.toString().c_str(), portal.portalStatus());
  int rc = setup_web_common_onconnect();
  if(rc) {
#if SERIAL_DEBUG
    Serial.print(F("onConnect:WiFi connected with "));
    Serial.print(WiFi.SSID());
    Serial.print(F(", IP:"));
    Serial.println(ipaddr.toString());
#endif
  }
}

// Redirect from root to INFO_URI
void onRoot() {
  WiFiWebServer&  webServer = portal.host();
  
  // Проверяем параметр logout в URL (добавляется JavaScript после disconnect)
  String uri = webServer.uri();
  bool forceLogout = uri.indexOf("logout=") >= 0;
  
  // Проверяем аутентификацию
  if (config.auth != AC_AUTH_NONE && config.username.length() > 0) {
    // Всегда используем уникальный realm на основе счетчика
    // После disconnect счетчик увеличивается, realm меняется, браузер забывает кэш
    String realm = "AutoConnect_" + String(authRealmCounter);
    
    // Если был запрос logout, всегда отправляем 401 с новым realm
    if (forceLogout) {
      webServer.sendHeader("WWW-Authenticate", "Basic realm=\"" + realm + "\"");
      webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, private, max-age=0");
      webServer.sendHeader("Pragma", "no-cache");
      webServer.sendHeader("Expires", "Thu, 01 Jan 1970 00:00:00 GMT");
      webServer.sendHeader("Clear-Site-Data", "\"cache\", \"cookies\", \"storage\"");
      webServer.requestAuthentication();
      return;
    }
    
    // Проверяем аутентификацию с текущими учетными данными
    if (!webServer.authenticate(config.username.c_str(), config.password.c_str())) {
      // Если аутентификация не прошла, отправляем 401 с новым realm
      webServer.sendHeader("WWW-Authenticate", "Basic realm=\"" + realm + "\"");
      webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, private, max-age=0");
      webServer.sendHeader("Pragma", "no-cache");
      webServer.sendHeader("Expires", "Thu, 01 Jan 1970 00:00:00 GMT");
      webServer.requestAuthentication();
      return;
    }
  }
#if defined(WEB_PAGES_MINIMAL)
  const char* redirectUri = SETUP_URI;  /* только Setup: логин, пароль, диапазоны, MQTT */
#else
  const char* redirectUri = INFO_URI;
#endif
  webServer.sendHeader("Location", String("http://") + webServer.client().localIP().toString() + String(redirectUri));
  webServer.send(302, "text/plain", "");
  webServer.client().flush();
  webServer.client().stop();
}

float mRSSi = 0.f;
int WiFists = -1;
extern int LedSts;

void loop_web() {
  bootSts1 = 1;
  int rc = WiFi.status();
  {
    static int oldstatus=-1, oldmode=-1, needStopAP=0;
    static long t0 = 0;
    int mode = WiFi.getMode();
    int ch = WiFi.channel();

    if(rc == WL_CONNECTION_LOST && (rc != oldstatus) && (SmOT.stsOT == 2)) {
      portal._ac_wifi_scan_sc = -100;
      Serial_db.printf("crasy state test\n");
    }
    bootSts1 = 2;
    if(rc == WL_CONNECTION_LOST || rc == WL_IDLE_STATUS) {
      if(portal._ac_wifi_scan_sc != -100 && portal._ac_wifi_scan_sc == 0 && SmOT.stsOT == 2) {
        Serial_db.printf("crasy state detected\n");
        ST_setCpuFrequencyMhz(SmOT.useCPU_freq);
        portal._ac_wifi_scan_sc = -200;
      }
    }

    if((rc != oldstatus) || mode != oldmode) {
      Serial_db.printf("WiFi: status=%d (%d) mode = %d chanel=%d  (%d)\n", rc, oldstatus, mode, ch, millis());
      if(rc == WL_CONNECTED &&  (oldstatus == WL_IDLE_STATUS || oldstatus == WL_DISCONNECTED ||  oldstatus == WL_NO_SSID_AVAIL)) {
        needStopAP = 1; t0 = millis();
      }
      oldmode = mode; oldstatus = rc;
    } else if(needStopAP) {
      if(millis()-t0 > 20000) {
        needStopAP = 0;
        if(mode == WIFI_MODE_APSTA) { WiFi.softAPdisconnect(true); WiFi.enableAP(false); }
      }
    }
  }

  portal.handleClient();

#if defined(ARDUINO_ARCH_ESP32) && defined(USE_HTTPS)
  if (g_httpsServer && g_httpsServer->isRunning()) {
    g_httpsServer->loop();
  }
  if (g_httpRedirectServer) {
    if (WiFiClient client = g_httpRedirectServer->available()) {
      String path = "/";
      if (client.connected() && client.available()) {
        String line = client.readStringUntil('\n');
        int s = line.indexOf(' ');
        int s2 = line.indexOf(' ', s + 1);
        if (s > 0 && s2 > s) path = line.substring(s + 1, s2);
      }
      IPAddress ip = (WiFi.getMode() & WIFI_STA) && (WiFi.status() == WL_CONNECTED) ? WiFi.localIP() : WiFi.softAPIP();
      client.print(F("HTTP/1.1 301 Moved Permanently\r\nLocation: https://"));
      client.print(ip);
      client.print(path);
      client.print(F("\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"));
      client.stop();
    }
  }
#endif

  if(rc != WiFists) {
#if SERIAL_DEBUG
    Serial_db.printf("WiFi.status=%i\n", rc);
#endif
    if(rc == WL_CONNECTED) {
      LedSts = 0;
#if SERIAL_DEBUG
      Serial_db.printf((PGM_P)F("RSSI: %d dBm (%i%%)\n"), WiFi.RSSI(),_toWiFiQuality(WiFi.RSSI()));
      Serial.print(F("IP address: "));
      Serial.println(WiFi.localIP());
#endif
    } else {
      Serial_db.printf("WiFi disconnected (sts=%d t %d stsOT %d %d %d)\n", rc, millis(), SmOT.stsOT, SmOT.ns_OT, SmOT.nr_OT);
      LedSts = 1;
    }
    if( rc >=0 && rc <=7) WiFiDebugInfo[rc]++;
    WiFists = rc;
  }

  if(rc ==  WL_CONNECTED) {
    static int sRSSI = 0, razRSSI = 0; static unsigned long t0 = 0; int dt = millis() - t0;
    if(dt > 10000) { t0 = millis(); razRSSI++; sRSSI += WiFi.RSSI(); if(razRSSI > 60) { mRSSi =  float(sRSSI)/float(razRSSI); razRSSI = 0; sRSSI = 0; } }
  }

#if MQTT_USE
  if(rc ==  WL_CONNECTED && (SmOT.useMQTT== 0x03)) mqtt_loop();
#endif
}

unsigned int _toWiFiQuality(int32_t rssi) {
  unsigned int  qu;
  if (rssi == 31) qu = 0; else if (rssi <= -100) qu = 0; else if (rssi >= -50) qu = 100; else qu = 2 * (rssi + 100);
  return qu;
}

int OutUTCtime(time_t now) {
  char str[312]; char buffer[26]; struct tm* tm_info;
  const char *s0 = "<em id=\"utcl\"></em><time id=\"upd_at\" dt=\"";
  const char *s1 = "\"></time><script>";
  const char *s2 =
"const src_el=document.getElementById('upd_at');const d=new Date(src_el.getAttribute('dt')).toLocaleString();document.getElementById(\"utcl\").innerHTML=d;</script>";
  tm_info = localtime(&now);
  strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info); buffer[25] = 0;
  sprintf(str,"%s%sZ%s%s", s0,buffer,s1, s2);
  utc_time_jc = str;
  return 0;
}

void setup_read_config(void) {
  bool b = FlashFS.begin(AUTOCONNECT_FS_INITIALIZATION);
  if(b == false) { Serial.println(F("FlashFS.begin failed")); }
  SmOT.Read_ot_fs();
#if MQTT_USE
  // Загружаем данные MQTT ДО настройки веб-сервера
  SmOT.Read_mqtt_fs();
#endif
  // Загружаем веб-аутентификацию из отдельного файла (независимо от MQTT)
  SmOT.Read_web_auth_fs();
  SmOT.init(1);
}

void check_fs(void) {
#if defined(ARDUINO_ARCH_ESP32)
  File root = FlashFS.open("/"); File file = root.openNextFile();
  while(file){
#if SERIAL_DEBUG
    Serial.print("FILE: "); Serial_db.printf( "%s %d\n", file.name(), file.size());
#endif
    if(file.size() > 1000000) { char str[80]; sprintf(str,"/%s",file.name()); file.close(); FlashFS.remove(str); break; }
    file = root.openNextFile();
  }
#endif
#if SERIAL_DEBUG
  { int tBytes, uBytes;
#if defined(ARDUINO_ARCH_ESP8266)
    FSInfo info; FlashFS.info(info); tBytes = info.totalBytes; uBytes = info.usedBytes;
#else
    tBytes  = FlashFS.totalBytes(); uBytes = FlashFS.usedBytes();
#endif
    Serial_db.printf("FlashFS tBytes = %d used = %d\n", tBytes, uBytes);
  }
#endif
}
