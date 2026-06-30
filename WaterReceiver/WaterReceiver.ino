// =============================================================
//  RECEIVER — Heltec WiFi LoRa 32 V3.2
//  - Syncs time via NTP on boot (WiFi)
//  - Timestamps packets on arrival
//  - Logs to LittleFS as CSV
//  - Hosts a web dashboard on local network
//  Serial commands: DUMP | CLEAR | STATUS
// =============================================================

#include "LoRaWan_APP.h"
#include "Arduino.h"
#include "lora_receiver_constants.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include "time.h"
#include "webpage.h"

// ----- Firmware version — increment this each time you export a new .bin -----
#define FW_VERSION "1.0.1"

// ----- OTA -----
// 1. Compile: Sketch → Export Compiled Binary → find receiver.ino.bin in sketch folder
// 2. Upload receiver.ino.bin to your GitHub repo
// 3. Also upload a version.txt file containing just the version string e.g. "1.0.1"
// 4. Paste the raw GitHub URLs below
#define OTA_VERSION_URL "https://raw.githubusercontent.com/NisargMehta7/Water-Flow-Receiver-OTA/refs/heads/main/version.txt"
#define OTA_BIN_URL     "https://raw.githubusercontent.com/NisargMehta7/Water-Flow-Receiver-OTA/refs/heads/main/WaterReceiver/build/Heltec-esp32.esp32.heltec_wifi_lora_32_V3/WaterReceiver.ino.bin"

// ----- WiFi credentials — change these -----
const char* WIFI_SSID     = "NM Phone";
const char* WIFI_PASSWORD = "nisarg123";

// ----- NTP / Timezone -----
#define UTC_OFFSET_SEC      -25200
#define DAYLIGHT_OFFSET_SEC  3600
const char* NTP_SERVER = "time.google.com";

// ----- Log file -----
#define LOG_FILE   "/datalog.csv"
#define CONFIG_FILE "/config.txt"
#define CSV_HEADER "timestamp,sender,flowrate_Lmin,volume_L,rssi_dBm,snr\n"

// ----- No-flow tracking -----
#define DEFAULT_NO_FLOW_THRESHOLD   6
#define DEFAULT_NO_PACKET_THRESHOLD 3
int noFlowThreshold   = DEFAULT_NO_FLOW_THRESHOLD;
int noPacketThreshold = DEFAULT_NO_PACKET_THRESHOLD;
int noFlowCount[3]    = {0, 0, 0};
int noPacketCount[3]  = {0, 0, 0};
bool packetReceivedThisInterval[3] = {false, false, false};

// Watchdog fires once per hour to check if senders have gone silent
#define WATCHDOG_INTERVAL_MS 3600000UL
uint32_t lastWatchdogCheck = 0;

void loadConfig(void) {
  if (LittleFS.exists(CONFIG_FILE)) {
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (f) {
      String line1 = f.readStringUntil('\n'); line1.trim();
      String line2 = f.readStringUntil('\n'); line2.trim();
      int v1 = line1.toInt(); if (v1 > 0) noFlowThreshold   = v1;
      int v2 = line2.toInt(); if (v2 > 0) noPacketThreshold = v2;
      f.close();
    }
  }
  Serial.printf("Config: no-flow=%dh, no-packet=%d intervals\r\n",
                noFlowThreshold, noPacketThreshold);
}

void saveConfig(void) {
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (f) {
    f.println(noFlowThreshold);
    f.println(noPacketThreshold);
    f.close();
  }
}

// ----- Sender name lookup -----
String getSenderName(uint8_t id) {
  switch(id) {
    case 1:  return "Tap 1";
    case 2:  return "Tap 2";
    default: return "Unknown";
  }
}

// ----- Web server -----
WebServer server(80);

// ----- LoRa -----
char rxpacket[BUFFER_SIZE];
bool lora_idle = true;
static RadioEvents_t RadioEvents;

// ----- Forward declarations -----
void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr);
void OnRxTimeout(void);
void OnRxError(void);
void dumpLog(void);
void clearLog(void);
void printStatus(void);
bool syncNTP(void);
String getTimestamp(void);
void setupWebServer(void);
void handleRoot(void);
void handleCSV(void);
void handleClear(void);
void handleSetThreshold(void);
void checkPacketWatchdog(void);
void checkOTA(void);

// =============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  // --- LittleFS ---
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed — halting.");
    while (true) delay(1000);
  }
  if (!LittleFS.exists(LOG_FILE)) {
    File f = LittleFS.open(LOG_FILE, "w");
    if (f) { f.print(CSV_HEADER); f.close(); }
    Serial.println("New log file created.");
  }

  // --- Load saved config ---
  loadConfig();

  // --- NTP time sync ---
  if (!syncNTP()) {
    Serial.println("WARNING: NTP sync failed. Timestamps will be incorrect.");
  }

  // --- OTA check ---
  checkOTA();

  // --- Web server ---
  setupWebServer();

  // --- LoRa ---
  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);

  RadioEvents.RxDone    = OnRxDone;
  RadioEvents.RxTimeout = OnRxTimeout;
  RadioEvents.RxError   = OnRxError;

  Radio.Init(&RadioEvents);
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetRxConfig(MODEM_LORA, LORA_BANDWIDTH, LORA_SPREADING_FACTOR,
                    LORA_CODINGRATE, 0, LORA_PREAMBLE_LENGTH,
                    LORA_SYMBOL_TIMEOUT, LORA_FIX_LENGTH_PAYLOAD_ON,
                    0, true, 0, 0, LORA_IQ_INVERSION_ON, true);

  Serial.println("Receiver ready. Commands: DUMP | CLEAR | STATUS");
  printStatus();
  lastWatchdogCheck = millis();
}

// =============================================================
void loop() {
  server.handleClient();

  if (millis() - lastWatchdogCheck >= WATCHDOG_INTERVAL_MS) {
    checkPacketWatchdog();
    lastWatchdogCheck = millis();
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toUpperCase();
    if      (cmd == "DUMP")   dumpLog();
    else if (cmd == "CLEAR")  clearLog();
    else if (cmd == "STATUS") printStatus();
    else Serial.println("Unknown command. Use: DUMP | CLEAR | STATUS");
  }

  if (lora_idle) {
    lora_idle = false;
    Radio.Rx(0);
  }

  Radio.IrqProcess();
}

// =============================================================
void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr) {
  uint16_t safeSize = min(size, (uint16_t)(BUFFER_SIZE - 1));
  memcpy(rxpacket, payload, safeSize);
  rxpacket[safeSize] = '\0';
  Radio.Sleep();

  String timestamp = getTimestamp();

  uint8_t senderID = (uint8_t)rxpacket[0];
  char *data = rxpacket + 1;

  int pulses;
  float flowRate, volume;
  int parsed = sscanf(data, "%f,%f,%d", &flowRate, &volume, &pulses);

  if (parsed < 2) {
    Serial.printf("RX: bad parse (%d/2 fields) — discarded. Raw: %s\r\n", parsed, rxpacket);
    lora_idle = true;
    return;
  }

  String senderName = getSenderName(senderID);

  // Mark packet received for watchdog
  if (senderID >= 1 && senderID <= 2) {
    packetReceivedThisInterval[senderID] = true;
    noPacketCount[senderID] = 0;
  }

  // --- No-flow tracking ---
  if (senderID >= 1 && senderID <= 2) {
    if (flowRate < 0.1) {
      noFlowCount[senderID]++;
      if (noFlowCount[senderID] >= noFlowThreshold) {
        Serial.printf("WARNING: %s has had no flow for %d hours — check connections.\r\n",
                      senderName.c_str(), noFlowCount[senderID]);
      }
    } else {
      noFlowCount[senderID] = 0;
    }
  }

  Serial.println("------ Packet received ------");
  Serial.printf("  Sender           : %s (ID %d)\r\n", senderName.c_str(), senderID);
  Serial.printf("  Timestamp        : %s\r\n", timestamp.c_str());
  Serial.printf("  Flow Rate (L/min): %.2f\r\n", flowRate);
  Serial.printf("  Volume this hour : %.2f L\r\n", volume);
  Serial.printf("  Pulses : %d L\r\n", pulses);
  Serial.printf("  RSSI             : %d dBm\r\n", rssi);
  Serial.printf("  SNR              : %d\r\n", snr);

  File f = LittleFS.open(LOG_FILE, "a");
  if (f) {
    char row[120];
    snprintf(row, sizeof(row),
             "%s,%s,%.2f,%.2f,%d,%d\n",
             timestamp.c_str(), senderName.c_str(),
             flowRate, volume, rssi, snr);
    f.print(row);
    f.close();
    Serial.println("  Logged to flash.");
  } else {
    Serial.println("  WARNING: could not open log file for writing.");
  }

  lora_idle = true;
}

void OnRxTimeout(void) { Radio.Sleep(); lora_idle = true; }
void OnRxError(void)   { Radio.Sleep(); Serial.println("RX error."); lora_idle = true; }

// =============================================================
//  Packet watchdog — called once per hour
// =============================================================
void checkPacketWatchdog(void) {
  for (int id = 1; id <= 2; id++) {
    if (!packetReceivedThisInterval[id]) {
      noPacketCount[id]++;
      Serial.printf("WARNING: No packet from %s for %d hour(s).\r\n",
                    getSenderName(id).c_str(), noPacketCount[id]);
    }
    packetReceivedThisInterval[id] = false;  // reset for next interval
  }
}

// =============================================================
//  OTA update
// =============================================================
void checkOTA(void) {
  if (strlen(OTA_VERSION_URL) == 0 || strlen(OTA_BIN_URL) == 0) {
    Serial.println("OTA: no URLs configured, skipping.");
    return;
  }

  // First fetch the remote version string and compare to running firmware
  HTTPClient http;
  WiFiClientSecure versionClient;
  versionClient.setInsecure();
  http.begin(versionClient, OTA_VERSION_URL);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("OTA: version check failed (HTTP %d), skipping.\r\n", code);
    http.end();
    return;
  }
  String remoteVersion = http.getString();
  remoteVersion.trim();
  http.end();

  Serial.printf("OTA: running v%s, remote v%s\r\n", FW_VERSION, remoteVersion.c_str());

  if (remoteVersion == FW_VERSION) {
    Serial.println("OTA: firmware is up to date.");
    return;
  }

  // New version available — download and flash
  Serial.printf("OTA: update available (%s → %s), flashing...\r\n", FW_VERSION, remoteVersion.c_str());
  WiFiClientSecure client;
  client.setInsecure();   // skip certificate verification — fine for OTA over trusted network
  httpUpdate.setLedPin(LED_BUILTIN, LOW);
  t_httpUpdate_return ret = httpUpdate.update(client, OTA_BIN_URL);
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("OTA: failed (%d): %s\r\n", httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("OTA: no update available.");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("OTA: update successful — rebooting.");
      // Board reboots automatically after this
      break;
  }
}
void setupWebServer() {
  server.on("/",            handleRoot);
  server.on("/csv",          handleCSV);
  server.on("/clear",        handleClear);
  server.on("/setthreshold", handleSetThreshold);
  server.begin();
  Serial.printf("Dashboard: http://%s\r\n", WiFi.localIP().toString().c_str());
}

void handleRoot() {
  String tableRows    = "";
  String chartLabels1 = "", chartVolume1 = "";
  String chartLabels2 = "", chartVolume2 = "";
  String northName    = getSenderName(1);
  String southName    = getSenderName(2);

  File f = LittleFS.open(LOG_FILE, "r");
  if (f) {
    bool firstLine = true;
    const int MAX_ROWS = 200;
    String* lines = new String[MAX_ROWS];
    int total = 0;
    while (f.available() && total < MAX_ROWS) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;
      if (firstLine) { firstLine = false; continue; }
      lines[total++] = line;
    }
    f.close();

    // Table rows newest first
    for (int i = total - 1; i >= 0; i--) {
      String cols[6];
      int c = 0, start = 0;
      for (int j = 0; j <= lines[i].length() && c < 6; j++) {
        if (j == lines[i].length() || lines[i][j] == ',') {
          cols[c++] = lines[i].substring(start, j);
          start = j + 1;
        }
      }
      tableRows += "<tr data-sender=\"" + cols[1] + "\">";
      for (int k = 0; k < 6; k++) tableRows += "<td>" + cols[k] + "</td>";
      tableRows += "</tr>\n";
    }

    // Chart data oldest first
    for (int i = 0; i < total; i++) {
      String cols[6];
      int c = 0, start = 0;
      for (int j = 0; j <= lines[i].length() && c < 6; j++) {
        if (j == lines[i].length() || lines[i][j] == ',') {
          cols[c++] = lines[i].substring(start, j);
          start = j + 1;
        }
      }
      if (cols[1] == northName) {
        String comma = chartLabels1.length() > 0 ? "," : "";
        chartLabels1 += comma + "\"" + cols[0] + "\"";
        chartVolume1 += comma + cols[3];
      } else if (cols[1] == southName) {
        String comma = chartLabels2.length() > 0 ? "," : "";
        chartLabels2 += comma + "\"" + cols[0] + "\"";
        chartVolume2 += comma + cols[3];
      }
    }

    delete[] lines;
  }

  // Warning boxes
  String warningBoxes = "";
  for (int id = 1; id <= 2; id++) {
    if (noFlowCount[id] >= noFlowThreshold) {
      warningBoxes += "<div class='warning-box'>&#9888; <strong>" + getSenderName(id) +
                       "</strong>: No flow detected for " + String(noFlowCount[id]) +
                       " hours. Check sensor connection.</div>\n";
    }
    if (noPacketCount[id] >= noPacketThreshold) {
      warningBoxes += "<div class='warning-box' style='border-color:#922b21;background:#f9d0cc'>&#9888; <strong>" +
                       getSenderName(id) + "</strong>: No data received for " +
                       String(noPacketCount[id]) + " hours. Device may be offline.</div>\n";
    }
  }

  // Sender filter buttons
  String senderButtons = "";
  for (int id = 1; id <= 2; id++) {
    String name = getSenderName(id);
    senderButtons += "<button class=\"btn filter-btn\" data-filter=\"" + name +
                      "\" onclick=\"filterTable('" + name + "',this)\">" + name + "</button>\n";
  }

  String page = String(INDEX_HTML);
  page.replace("TABLE_ROWS_PLACEHOLDER",    tableRows);
  page.replace("SENDER_BUTTONS_PLACEHOLDER", senderButtons);
  page.replace("WARNING_BOXES_PLACEHOLDER",  warningBoxes);
  page.replace("THRESHOLD_PLACEHOLDER", String(noFlowThreshold));
  page.replace("NOPKT_PLACEHOLDER",     String(noPacketThreshold));
  page.replace("FW_VER", String(FW_VERSION));
  page.replace("CHART_LABELS_1_PLACEHOLDER", chartLabels1);
  page.replace("CHART_VOLUME_1_PLACEHOLDER", chartVolume1);
  page.replace("CHART_LABELS_2_PLACEHOLDER", chartLabels2);
  page.replace("CHART_VOLUME_2_PLACEHOLDER", chartVolume2);
  server.send(200, "text/html", page);
}

void handleCSV() {
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) { server.send(404, "text/plain", "No log file found."); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=feedlot_data.csv");
  server.streamFile(f, "text/csv");
  f.close();
}

void handleClear() {
  clearLog();
  for (int i = 0; i < 3; i++) {
    noFlowCount[i]   = 0;
    noPacketCount[i] = 0;
    packetReceivedThisInterval[i] = false;
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSetThreshold() {
  bool changed = false;
  if (server.hasArg("hours")) {
    int v = server.arg("hours").toInt();
    if (v > 0 && v <= 1000) { noFlowThreshold = v; changed = true; }
  }
  if (server.hasArg("nopacket")) {
    int v = server.arg("nopacket").toInt();
    if (v > 0 && v <= 1000) { noPacketThreshold = v; changed = true; }
  }
  if (changed) {
    saveConfig();
    Serial.printf("Config updated — no-flow: %dh, no-packet: %d intervals\r\n",
                  noFlowThreshold, noPacketThreshold);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

// =============================================================
//  NTP + timestamp
// =============================================================
bool syncNTP(void) {
  Serial.printf("Connecting to WiFi: %s\r\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 15000) { Serial.println("WiFi timed out."); return false; }
    delay(500); Serial.print(".");
  }
  Serial.printf("\r\nConnected. IP: %s\r\n", WiFi.localIP().toString().c_str());
  delay(2000);
  configTime(UTC_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  struct tm ti;
  start = millis();
  while (!getLocalTime(&ti)) {
    if (millis() - start > 30000) { Serial.println("NTP timed out."); return false; }
    delay(500); Serial.print(".");
  }
  Serial.printf("\r\nTime synced: %04d-%02d-%02d %02d:%02d:%02d\r\n",
                ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                ti.tm_hour, ti.tm_min, ti.tm_sec);
  return true;
}

String getTimestamp(void) {
  struct tm ti;
  if (!getLocalTime(&ti)) return "0000-00-00 00:00:00";
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
           ti.tm_hour, ti.tm_min, ti.tm_sec);
  return String(buf);
}

// =============================================================
//  Serial command handlers
// =============================================================
void dumpLog(void) {
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) { Serial.println("No log file found."); return; }
  Serial.println("===== BEGIN CSV DUMP =====");
  while (f.available()) Serial.write(f.read());
  f.close();
  Serial.println("===== END CSV DUMP =====");
}

void clearLog(void) {
  LittleFS.remove(LOG_FILE);
  File f = LittleFS.open(LOG_FILE, "w");
  if (f) { f.print(CSV_HEADER); f.close(); }
  Serial.println("Log cleared.");
}

void printStatus(void) {
  if (!LittleFS.exists(LOG_FILE)) { Serial.println("STATUS: no log file."); return; }
  File f = LittleFS.open(LOG_FILE, "r");
  size_t fileSize = f.size();
  int lines = 0;
  while (f.available()) { if (f.read() == '\n') lines++; }
  f.close();
  Serial.printf("STATUS: %d record(s), %u bytes used.\r\n", max(0, lines - 1), fileSize);
}
