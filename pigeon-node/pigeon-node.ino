#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <Wire.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <LoRa.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SD.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <vector>
#include <mbedtls/base64.h>
#include <esp_netif.h>
#include <arpa/inet.h>

using namespace std;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SDA_PIN 2
#define SCL_PIN 15

#define LORA_SCK 13
#define LORA_MISO 12
#define LORA_MOSI 14
#define LORA_CS 27
#define LORA_RST 26
#define LORA_DIO0 25

String nodeName = "P1";
String repeaterSSID = "xx2";
String repeaterPass = "12345678";
String nodePassword = "";

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WebSocketsServer webSocket = WebSocketsServer(81);
WebServer webServer(80);
DNSServer dnsServer;

SPIClass loraSPI(HSPI);
SPIClass sdSPI(VSPI);

struct Neighbor {
  String nodeName;
  int rssi;
  unsigned long lastSeen;
};
vector<Neighbor> neighbors;

struct ActiveClient {
  uint8_t num;
  String username;
};
vector<ActiveClient> activeClients;

struct RemoteUser {
  String username;
  unsigned long lastSeen;
};
vector<RemoteUser> remoteActiveUsers;

struct SyncTask {
  String fileKey;
  String targetNode;
};
vector<SyncTask> syncQueue;
bool isSyncing = false;
unsigned long lastSyncRequestTime = 0;
String currentSyncFile = "";
String syncBuffer = "";
int expectedChunkIndex = 0;

String msgBuffer = "";
String currentMsgReqId = "";
int expectedMsgChunkIndex = 0;
unsigned long lastChunkRxTime = 0;

struct TxTask {
  String type;
  String fileKey;
  int version;
  String msgType;
  String fullBase64Data;
  int totalChunks;
};
vector<TxTask> txQueue;
int currentChunkIdx = 0;
unsigned long lastTxTime = 0;

unsigned long lastBeaconTime = 0;
unsigned long lastVersionCheckTime = 0;
unsigned long lastPresenceTime = 0;
bool sdAvailable = false;
int prevClientsCount = -1;
String oledStatus = "System Boot...";
float currentAppVersion = 0.0;
bool isDownloadingApp = false;

void requestNextSyncFile();
void sendLocalVersions();
void broadcastVersions();
void broadcastPresence();
void notifyAllActiveClients();

bool isReceivingStream() {
  return ((expectedChunkIndex > 0 || expectedMsgChunkIndex > 0) && (millis() - lastChunkRxTime < 8000));
}

bool isLocalActive(String username, uint8_t& outNum) {
  for (const auto& c : activeClients) {
    if (c.username == username) {
      outNum = c.num;
      return true;
    }
  }
  return false;
}

bool isRemoteActive(String username) {
  for (const auto& r : remoteActiveUsers) {
    if (r.username == username && (millis() - r.lastSeen < 45000)) {
      return true;
    }
  }
  return false;
}

String readFS(String path) {
  try {
    Serial.println("[FS] Reading file: " + path);
    if (!SPIFFS.exists(path)) {
      Serial.println("[FS] File not found, returning empty JSON.");
      return "{}";
    }
    File file = SPIFFS.open(path, "r");
    if (!file) {
      Serial.println("[FS] Failed to open file.");
      return "{}";
    }
    String data = file.readString();
    file.close();
    Serial.println("[FS] Read successful. Length: " + String(data.length()));
    return data;
  } catch (...) {
    Serial.println("[FS] Exception caught during read.");
    return "{}";
  }
}

void writeFS(String path, String data) {
  try {
    Serial.println("[FS] Writing file: " + path + " | Length: " + String(data.length()));
    File file = SPIFFS.open(path, "w");
    if (file) {
      file.print(data);
      file.close();
      Serial.println("[FS] Write successful.");
    } else {
      Serial.println("[FS] Failed to open file for writing.");
    }
  } catch (...) {
    Serial.println("[FS] Exception caught during write.");
  }
}

int getFileVersion(String fileKey) {
  try {
    String vData = readFS("/data_versions.json");
    DynamicJsonDocument vDoc(512);
    deserializeJson(vDoc, vData);
    int version = vDoc[fileKey] | 0;
    return version;
  } catch (...) { return 0; }
}

int incrementFileVersion(String fileKey) {
  try {
    Serial.println("[VERSION] Incrementing version for: " + fileKey);
    String vData = readFS("/data_versions.json");
    DynamicJsonDocument vDoc(512);
    deserializeJson(vDoc, vData);
    int v = (vDoc[fileKey] | 0) + 1;
    vDoc[fileKey] = v;
    String out;
    serializeJson(vDoc, out);
    writeFS("/data_versions.json", out);
    return v;
  } catch (...) { return 1; }
}

void setFileVersion(String fileKey, int version) {
  try {
    String vData = readFS("/data_versions.json");
    DynamicJsonDocument vDoc(512);
    deserializeJson(vDoc, vData);
    vDoc[fileKey] = version;
    String out;
    serializeJson(vDoc, out);
    writeFS("/data_versions.json", out);
  } catch (...) {}
}

String applySalt(String data) {
  return "PIGEON_SALT_" + data + "_ENDSALT";
}

String removeSalt(String saltedData) {
  try {
    if (saltedData.startsWith("PIGEON_SALT_") && saltedData.endsWith("_ENDSALT")) {
      return saltedData.substring(12, saltedData.length() - 8);
    }
    return "";
  } catch (...) {
    return "";
  }
}

void updateDisplay() {
  try {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("NODE: ");
    display.println(nodeName);
    display.setCursor(0, 10);
    display.print("IP: ");
    display.println(WiFi.softAPIP().toString());
    display.setCursor(0, 20);
    display.print("CLIENTS: ");
    display.println(WiFi.softAPgetStationNum());

    if (neighbors.empty()) {
      display.setCursor(0, 30);
      display.println("LORA: SINGLE NODE");
    } else {
      display.setCursor(0, 30);
      display.print("LORA: MESH [");
      display.print(neighbors.size());
      display.println("]");
    }

    display.setCursor(0, 40);
    display.print("SD: ");
    display.println(sdAvailable ? "MOUNTED" : "OFFLINE");

    display.setCursor(0, 50);
    display.println(oledStatus);

    display.display();
  } catch (...) {}
}

void sendLoRa(String type, String reqId, String rawData) {
  try {
    Serial.println("[LORA_TX] Building packet -> Type: " + type + ", ReqID: " + reqId);
    DynamicJsonDocument doc(4096);
    doc["type"] = type;
    doc["req_id"] = reqId;
    doc["data"] = applySalt(rawData);

    String payload;
    serializeJson(doc, payload);

    Serial.println("[LORA_TX] Payload Size: " + String(payload.length()) + " bytes");
    LoRa.beginPacket();
    LoRa.print(payload);
    LoRa.endPacket();

    oledStatus = "TX: " + type;
    updateDisplay();
    Serial.println("[LORA_TX] Packet transmitted over RF.");
  } catch (...) {
    Serial.println("[LORA_TX] Exception caught during RF transmission.");
  }
}

String base64Encode(String input) {
  try {
    size_t outputLength = 4 * ((input.length() + 2) / 3) + 1;
    unsigned char* out = (unsigned char*)malloc(outputLength);
    if (!out) return "";
    size_t actual_len;
    int res = mbedtls_base64_encode(out, outputLength, &actual_len, (const unsigned char*)input.c_str(), input.length());
    if (res != 0) { free(out); return ""; }
    out[actual_len] = '\0';
    String encoded = String((char*)out);
    free(out);
    return encoded;
  } catch (...) {
    return "";
  }
}

String base64Decode(String input) {
  try {
    size_t outputLength = input.length() / 4 * 3 + 1;
    unsigned char* out = (unsigned char*)malloc(outputLength);
    if (!out) return "";
    size_t actual_len;
    int res = mbedtls_base64_decode(out, outputLength, &actual_len, (const unsigned char*)input.c_str(), input.length());
    if (res != 0) { free(out); return ""; }
    out[actual_len] = '\0';
    String decoded = String((char*)out);
    free(out);
    return decoded;
  } catch (...) {
    return "";
  }
}

void queueFileChunks(String fileKey, int v, String content) {
    txQueue.clear(); 
    currentChunkIdx = 0;
    
    TxTask t;
    t.type = "sync";
    t.fileKey = fileKey;
    t.version = v;
    t.fullBase64Data = base64Encode(content);
    t.totalChunks = (t.fullBase64Data.length() + 48 - 1) / 48;
    
    Serial.println("[SYNC_TX] Encoded " + fileKey + " to Base64. Slicing into " + String(t.totalChunks) + " chunks for safe transmission.");
    txQueue.push_back(t);
}

void queueMessageChunks(String msgType, String reqId, String content) {
    TxTask t;
    t.type = "msg";
    t.fileKey = reqId;
    t.msgType = msgType;
    t.version = 0;
    t.fullBase64Data = base64Encode(content);
    t.totalChunks = (t.fullBase64Data.length() + 48 - 1) / 48;
    
    Serial.println("[MSG_TX] Encoded message payload to Base64. Slicing into " + String(t.totalChunks) + " chunks for safe transmission.");
    txQueue.push_back(t);
}

void broadcastFileUpdateWithoutIncrement(String fileKey) {
  try {
    Serial.println("[SYNC_TX] Enqueueing specific file for requested sync: " + fileKey);
    int v = getFileVersion(fileKey);
    String content = readFS("/" + fileKey + ".json");
    queueFileChunks(fileKey, v, content);
  } catch (...) {}
}

void broadcastFileUpdate(String fileKey) {
  try {
    Serial.println("[SYNC_TX] Local file modified. Enqueueing mesh broadcast chunks: " + fileKey);
    int v = incrementFileVersion(fileKey);
    String content = readFS("/" + fileKey + ".json");
    queueFileChunks(fileKey, v, content);
  } catch (...) {}
}

void broadcastVersions() {
  try {
    Serial.println("[SYNC_TX] Broadcasting local version table.");
    String vData = readFS("/data_versions.json");
    DynamicJsonDocument doc(1024);
    doc["node"] = nodeName;
    doc["versions"] = vData;
    String out;
    serializeJson(doc, out);
    sendLoRa("version_check", String(millis()), out);
  } catch (...) {}
}

void broadcastPresence() {
  try {
    DynamicJsonDocument doc(512);
    JsonArray arr = doc.createNestedArray("users");
    for (const auto& c : activeClients) {
        arr.add(c.username);
    }
    String out;
    serializeJson(doc, out);
    sendLoRa("presence", String(millis()), out);
  } catch (...) {}
}

void requestVersionsFromClosestNeighbor() {
  if (neighbors.empty()) return;
  
  String bestNode = "";
  int bestRssi = -9999;
  for (const auto& n : neighbors) {
    if (n.rssi > bestRssi) {
      bestRssi = n.rssi;
      bestNode = n.nodeName;
    }
  }
  if (bestNode != "") {
    Serial.println("[SYNC] Requesting version ledger exclusively from closest node: " + bestNode + " (RSSI: " + String(bestRssi) + ")");
    DynamicJsonDocument reqDoc(256);
    reqDoc["targetNode"] = bestNode;
    String reqOut;
    serializeJson(reqDoc, reqOut);
    sendLoRa("req_version", String(millis()), reqOut);
  }
}

void sendLocalVersions() {
  try {
    Serial.println("[SYNC_TX] Transmitting local version ledger directly.");
    String vData = readFS("/data_versions.json");
    DynamicJsonDocument doc(1024);
    doc["node"] = nodeName;
    doc["versions"] = vData;
    String out;
    serializeJson(doc, out);
    sendLoRa("version_check", String(millis()), out);
  } catch (...) {}
}

void loadWifiConfig() {
  try {
    Serial.println("[INIT] Loading WiFi config from storage.");
    if (SPIFFS.exists("/wifi_data.json")) {
      File file = SPIFFS.open("/wifi_data.json", "r");
      if (file) {
        DynamicJsonDocument doc(512);
        DeserializationError err = deserializeJson(doc, file);
        if (!err) {
          if (doc.containsKey("nodeName")) nodeName = doc["nodeName"].as<String>();
          if (doc.containsKey("repeaterSSID")) repeaterSSID = doc["repeaterSSID"].as<String>();
          if (doc.containsKey("repeaterPass")) repeaterPass = doc["repeaterPass"].as<String>();
          Serial.println("[INIT] WiFi config parsed successfully.");
        }
        file.close();
      }
    }
  } catch (...) {
    Serial.println("[INIT] Exception parsing WiFi config.");
  }
}

void saveWifiConfig() {
  try {
    Serial.println("[INIT] Saving new WiFi config.");
    DynamicJsonDocument doc(512);
    doc["nodeName"] = nodeName;
    doc["repeaterSSID"] = repeaterSSID;
    doc["repeaterPass"] = repeaterPass;
    File file = SPIFFS.open("/wifi_data.json", "w");
    if (file) {
      serializeJson(doc, file);
      file.close();
    }
  } catch (...) {}
}

String generatePassword(String deviceName) {
  try {
    String b64 = base64Encode(deviceName);
    if (b64.length() > 8) {
      b64 = b64.substring(0, 8);
    }
    while (b64.length() < 8) {
      b64 += "7";
    }
    return b64;
  } catch (...) {
    return "12345678";
  }
}

bool loadUser(String username, String& storedPassword, String& storedToken) {
  try {
    Serial.println("[AUTH] Checking credentials for user: " + username);
    String data = readFS("/users.json");
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, data);
    if (error) return false;

    JsonArray arr = doc.as<JsonArray>();
    for (int i = 0; i < arr.size(); i++) {
      JsonObject u = arr[i];
      if (u["username"].as<String>() == username) {
        storedPassword = u["password"].as<String>();
        storedToken = u["token"].as<String>();
        Serial.println("[AUTH] User authentication block found.");
        return true;
      }
    }
    Serial.println("[AUTH] User not found in local registry.");
    return false;
  } catch (...) {
    return false;
  }
}

bool saveUser(String username, String password, String token) {
  try {
    Serial.println("[AUTH] Saving new user credentials: " + username);
    String data = readFS("/users.json");
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, data);

    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull()) arr = doc.to<JsonArray>();

    bool found = false;
    for (int i = 0; i < arr.size(); i++) {
      JsonObject u = arr[i];
      if (u["username"].as<String>() == username) {
        u["password"] = password;
        u["token"] = token;
        found = true;
        break;
      }
    }

    if (!found) {
      JsonObject newUser = arr.createNestedObject();
      newUser["username"] = username;
      newUser["password"] = password;
      newUser["token"] = token;
    }

    String out;
    serializeJson(doc, out);
    writeFS("/users.json", out);
    Serial.println("[AUTH] User credentials committed to storage.");
    return true;
  } catch (...) {
    return false;
  }
}

bool validateStoredToken(String token, String& username) {
  try {
    Serial.println("[AUTH] Validating session token.");
    String data = readFS("/users.json");
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, data);
    if (error) return false;

    JsonArray arr = doc.as<JsonArray>();
    for (int i = 0; i < arr.size(); i++) {
      JsonObject u = arr[i];
      if (u["token"].as<String>() == token) {
        username = u["username"].as<String>();
        Serial.println("[AUTH] Token valid for user: " + username);
        return true;
      }
    }
    Serial.println("[AUTH] Token rejection. Invalid or expired.");
    return false;
  } catch (...) {
    return false;
  }
}

String generateToken(String username) {
  long randVal = random(100000, 999999);
  return "PIGEON_TOKEN_" + username + "_" + String(randVal);
}

void registerActiveClient(uint8_t num, String username) {
  try {
    Serial.println("[WSS] Registering active socket client: " + username);
    for (auto it = activeClients.begin(); it != activeClients.end(); ++it) {
      if (it->num == num) {
        activeClients.erase(it);
        break;
      }
    }
    ActiveClient c;
    c.num = num;
    c.username = username;
    activeClients.push_back(c);
    
    broadcastPresence();

    String pData = readFS("/pending_deletes.json");
    DynamicJsonDocument pDoc(4096);
    deserializeJson(pDoc, pData);
    JsonArray pArr = pDoc.as<JsonArray>();
    
    bool changed = false;
    DynamicJsonDocument newPDoc(4096);
    JsonArray newPArr = newPDoc.to<JsonArray>();
    
    for (int i = 0; i < pArr.size(); i++) {
      JsonObject p = pArr[i];
      if (p["target"].as<String>() == username) {
        Serial.println("[QUEUE] Executing offline pending delete action for: " + username);
        DynamicJsonDocument fwdDoc(256);
        fwdDoc["event"] = p["event"];
        JsonObject dData = fwdDoc.createNestedObject("data");
        dData["peer"] = p["peer"];
        dData["sender"] = p["peer"];
        if (p.containsKey("timestamp")) dData["timestamp"] = p["timestamp"];
        if (p.containsKey("text")) dData["text"] = p["text"];
        String fwdStr;
        serializeJson(fwdDoc, fwdStr);
        webSocket.sendTXT(num, fwdStr);
        changed = true;
      } else {
        newPArr.add(p);
      }
    }
    
    if (changed) {
      String outStr;
      serializeJson(newPDoc, outStr);
      writeFS("/pending_deletes.json", outStr);
    }
    
    updateDisplay();
  } catch (...) {}
}

void unregisterActiveClient(uint8_t num) {
  try {
    Serial.println("[WSS] Dropping active socket client: " + String(num));
    for (auto it = activeClients.begin(); it != activeClients.end(); ++it) {
      if (it->num == num) {
        activeClients.erase(it);
        break;
      }
    }
    broadcastPresence();
  } catch (...) {}
}

void sendInfoEvent(uint8_t num) {
  try {
    Serial.println("[WSS] Pushing info event to socket: " + String(num));
    DynamicJsonDocument doc(256);
    doc["event"] = "info";
    JsonObject data = doc.createNestedObject("data");
    data["id"] = 1;
    data["name"] = nodeName;
    String output;
    serializeJson(doc, output);
    webSocket.sendTXT(num, output);
  } catch (...) {}
}

void requestAppFromMesh() {
  Serial.println("[APP] Initiating App transfer request from Mesh.");
  isDownloadingApp = true;
  oledStatus = "Req Mesh App DL";
  updateDisplay();
  DynamicJsonDocument doc(256);
  doc["node"] = nodeName;
  String raw;
  serializeJson(doc, raw);
  sendLoRa("app_transfer_req", String(millis()), raw);
}

void handleAppVersionCheck() {
  Serial.println("[APP] Validating local App manifest.");
  if (sdAvailable && SD.exists("/app/app-version.txt")) {
    File file = SD.open("/app/app-version.txt", "r");
    if (file) {
      String verStr = file.readString();
      currentAppVersion = verStr.toFloat();
      file.close();
      Serial.println("[APP] Local APK Version identified: " + String(currentAppVersion));
    }
  } else {
    Serial.println("[APP] No local APK available.");
    requestAppFromMesh();
  }
  DynamicJsonDocument doc(256);
  doc["version"] = currentAppVersion;
  String raw;
  serializeJson(doc, raw);
  sendLoRa("app_version_req", String(millis()), raw);
}

bool isBlocked(String u1, String u2, bool &u1BlockedU2, bool &u2BlockedU1) {
  u1BlockedU2 = false;
  u2BlockedU1 = false;
  try {
    String data = readFS("/block_list.json");
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, data);
    JsonArray arr = doc.as<JsonArray>();
    for (int i = 0; i < arr.size(); i++) {
      JsonObject b = arr[i];
      String blocker = b["blocker"].as<String>();
      String blocked = b["blocked"].as<String>();
      if (blocker == u1 && blocked == u2) u1BlockedU2 = true;
      if (blocker == u2 && blocked == u1) u2BlockedU1 = true;
    }
  } catch (...) {}
  return u1BlockedU2 || u2BlockedU1;
}

void handleBlockStatus(String blocker, String blocked, bool isBlock) {
  try {
    Serial.println("[POLICY] Processing block rule change. Target: " + blocked);
    String data = readFS("/block_list.json");
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, data);
    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull()) arr = doc.to<JsonArray>();
    
    bool changed = false;
    DynamicJsonDocument newDoc(4096);
    JsonArray newArr = newDoc.to<JsonArray>();
    
    for (int i = 0; i < arr.size(); i++) {
      JsonObject b = arr[i];
      if (b["blocker"].as<String>() == blocker && b["blocked"].as<String>() == blocked) {
        if (!isBlock) changed = true;
      } else {
        newArr.add(b);
      }
    }
    
    if (isBlock) {
      bool found = false;
      for (int i = 0; i < newArr.size(); i++) {
        JsonObject b = newArr[i];
        if (b["blocker"].as<String>() == blocker && b["blocked"].as<String>() == blocked) found = true;
      }
      if (!found) {
        JsonObject nb = newArr.createNestedObject();
        nb["blocker"] = blocker;
        nb["blocked"] = blocked;
        changed = true;
      }
    }
    
    if (changed) {
      Serial.println("[POLICY] Block list modified and saving.");
      String out;
      serializeJson(newDoc, out);
      writeFS("/block_list.json", out);
      broadcastFileUpdate("block_list");
    }
  } catch (...) {}
}

void sendInitialData(uint8_t num, String username) {
  try {
    Serial.println("[WSS] Pushing initial sync data to client ID: " + String(num));
    DynamicJsonDocument connResp(4096);
    connResp["event"] = "connections_list";
    JsonArray connArrData = connResp.createNestedArray("data");
    String connData = readFS("/connections.json");
    DynamicJsonDocument connDoc(4096);
    deserializeJson(connDoc, connData);
    JsonArray connArr = connDoc.as<JsonArray>();
    for (int i = 0; i < connArr.size(); i++) {
      JsonObject c = connArr[i];
      String u1 = c["user1"]["username"] | "";
      String u2 = c["user2"]["username"] | "";
      if (u1 == username || u2 == username) {
        String peer = (u1 == username) ? u2 : u1;
        JsonObject row = connArrData.createNestedObject();
        row["username"] = peer;
        uint8_t dummy;
        row["active"] = isLocalActive(peer, dummy) || isRemoteActive(peer);
        bool bMe, bPeer;
        isBlocked(username, peer, bMe, bPeer);
        row["blockedByMe"] = bMe;
        row["blockedByPeer"] = bPeer;
      }
    }
    String connOut;
    serializeJson(connResp, connOut);
    webSocket.sendTXT(num, connOut);

    DynamicJsonDocument grpResp(4096);
    grpResp["event"] = "groups_list";
    JsonArray grpArrData = grpResp.createNestedArray("data");
    String gData = readFS("/groups.json");
    DynamicJsonDocument groupsDoc(4096);
    deserializeJson(groupsDoc, gData);
    JsonArray groupsArr = groupsDoc.as<JsonArray>();
    for (int i = 0; i < groupsArr.size(); i++) {
      JsonObject g = groupsArr[i];
      JsonArray users = g["users"].as<JsonArray>();
      bool isMember = false;
      for (int j = 0; j < users.size(); j++) {
        if (users[j].as<String>() == username) { isMember = true; break; }
      }
      if (isMember) {
        JsonObject row = grpArrData.createNestedObject();
        row["id"] = g["id"];
        row["name"] = g["name"];
        int activeCount = 0;
        for (int j = 0; j < users.size(); j++) {
          uint8_t d;
          if (isLocalActive(users[j].as<String>(), d) || isRemoteActive(users[j].as<String>())) activeCount++;
        }
        row["activeCount"] = activeCount;
      }
    }
    String grpOut;
    serializeJson(grpResp, grpOut);
    webSocket.sendTXT(num, grpOut);

    DynamicJsonDocument blkResp(4096);
    blkResp["event"] = "block_list";
    JsonArray blkArrData = blkResp.createNestedArray("data");
    String bData = readFS("/block_list.json");
    DynamicJsonDocument blkDoc(4096);
    deserializeJson(blkDoc, bData);
    JsonArray blkArr = blkDoc.as<JsonArray>();
    for (int i = 0; i < blkArr.size(); i++) {
      JsonObject b = blkArr[i];
      if (b["blocker"].as<String>() == username) {
        JsonObject row = blkArrData.createNestedObject();
        row["username"] = b["blocked"];
      }
    }
    String blkOut;
    serializeJson(blkResp, blkOut);
    webSocket.sendTXT(num, blkOut);
  } catch (...) {}
}

void notifyAllActiveClients() {
  try {
    Serial.println("[WSS] Notifying all active clients of recent database/presence changes.");
    for (const auto& c : activeClients) {
      sendInitialData(c.num, c.username);
    }
  } catch (...) {}
}

void savePendingAction(String target, String eventName, String peer, String timestamp = "", String text = "") {
  try {
    Serial.println("[QUEUE] Storing offline pending event: " + eventName + " for target: " + target);
    String data = readFS("/pending_deletes.json");
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, data);
    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull()) arr = doc.to<JsonArray>();
    
    JsonObject obj = arr.createNestedObject();
    obj["target"] = target;
    obj["event"] = eventName;
    obj["peer"] = peer;
    if (timestamp != "") obj["timestamp"] = timestamp;
    if (text != "") obj["text"] = text;
    
    String out;
    serializeJson(doc, out);
    writeFS("/pending_deletes.json", out);
  } catch (...) {}
}

void sendGroupSystemMessage(String groupId, String msgText) {
  try {
    Serial.println("[GROUP] Dispatching automated system message: " + msgText);
    String gData = readFS("/groups.json");
    DynamicJsonDocument groupsDoc(4096);
    deserializeJson(groupsDoc, gData);
    JsonArray groupsArr = groupsDoc.as<JsonArray>();

    for (int i = 0; i < groupsArr.size(); i++) {
      JsonObject g = groupsArr[i];
      if (g["id"].as<String>() == groupId) {
        JsonArray users = g["users"].as<JsonArray>();
        
        DynamicJsonDocument fwdDoc(4096);
        fwdDoc["event"] = "group_message";
        JsonObject data = fwdDoc.createNestedObject("data");
        data["groupId"] = groupId;
        data["sender"] = "System";
        data["text"] = msgText;
        data["type"] = "text";
        data["timestamp"] = "Now";
        
        String rawStr;
        serializeJson(data, rawStr);
        if (rawStr.length() <= 120) {
            sendLoRa("group_message", String(millis()), rawStr);
        } else {
            queueMessageChunks("group_message", String(millis()), rawStr);
        }
        
        String fwdStr;
        serializeJson(fwdDoc, fwdStr);
        for (int j = 0; j < users.size(); j++) {
          uint8_t targetNum;
          if (isLocalActive(users[j].as<String>(), targetNum)) {
            webSocket.sendTXT(targetNum, fwdStr);
          }
        }
        break;
      }
    }
  } catch (...) {}
}

void requestNextSyncFile() {
  if (syncQueue.empty()) {
    isSyncing = false;
    currentSyncFile = "";
    syncBuffer = "";
    expectedChunkIndex = 0;
    Serial.println("[SYNC_RX] All targeted files synchronized seamlessly. Resuming normal operations.");
    oledStatus = "Mesh Sync Complete";
    updateDisplay();
    return;
  }
  
  isSyncing = true;
  currentSyncFile = syncQueue[0].fileKey;
  String targetNode = syncQueue[0].targetNode;
  lastSyncRequestTime = millis();
  syncBuffer = "";
  expectedChunkIndex = 0;
  
  DynamicJsonDocument reqDoc(256);
  reqDoc["fileKey"] = currentSyncFile;
  reqDoc["targetNode"] = targetNode;
  String reqOut;
  serializeJson(reqDoc, reqOut);
  
  Serial.println("[SYNC_RX] Requesting priority file update: " + currentSyncFile + " directly from node: " + targetNode);
  oledStatus = "Syncing " + currentSyncFile + "...";
  updateDisplay();
  
  sendLoRa("req_file", String(millis()), reqOut);
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  try {
    switch (type) {
      case WStype_DISCONNECTED:
        Serial.println("[WSS] Client disconnected from socket.");
        unregisterActiveClient(num);
        updateDisplay();
        break;
      case WStype_CONNECTED:
        Serial.println("[WSS] Client connected to socket ID: " + String(num));
        sendInfoEvent(num);
        updateDisplay();
        break;
      case WStype_TEXT:
        {
          DynamicJsonDocument doc(4096);
          DeserializationError err = deserializeJson(doc, payload, length);
          if (err) {
             Serial.println("[WSS] Payload JSON parse failure.");
             return;
          }

          String event = doc["event"] | "";
          Serial.println("[WSS] Internal App Event Triggered: " + event);

          if (event == "test") {
            DynamicJsonDocument resp(256);
            resp["event"] = "test_response";
            JsonObject data = resp.createNestedObject("data");
            data["success"] = true;
            data["userId"] = doc["data"]["userId"] | 0;
            data["msg"] = doc["data"]["msg"] | "";
            String output;
            serializeJson(resp, output);
            webSocket.sendTXT(num, output);
          } else if (event == "login") {
            String username = doc["data"]["username"] | "";
            String password = doc["data"]["password"] | "";

            String storedPassword = "";
            String storedToken = "";
            bool userExists = loadUser(username, storedPassword, storedToken);
            bool loginSuccess = false;
            String finalToken = "";
            String errMsg = "";

            if (userExists) {
              if (storedPassword == password) {
                loginSuccess = true;
                finalToken = storedToken;
                registerActiveClient(num, username);
              } else {
                errMsg = "Authentication failed: invalid PIN code.";
              }
            } else {
              finalToken = generateToken(username);
              if (saveUser(username, password, finalToken)) {
                loginSuccess = true;
                registerActiveClient(num, username);
                broadcastFileUpdate("users");
              } else {
                errMsg = "Internal system storage failure.";
              }
            }

            DynamicJsonDocument resp(256);
            resp["event"] = "login_response";
            JsonObject data = resp.createNestedObject("data");
            data["success"] = loginSuccess;
            if (loginSuccess) {
              data["token"] = finalToken;
              data["username"] = username;
            } else {
              data["message"] = errMsg;
            }
            String output;
            serializeJson(resp, output);
            webSocket.sendTXT(num, output);
            
            if (loginSuccess) {
                sendInitialData(num, username);
            }
          } else if (event == "token_validate") {
            String token = doc["data"]["token"] | "";
            String username = "";
            bool isValid = validateStoredToken(token, username);

            DynamicJsonDocument resp(256);
            resp["event"] = "token_validated";
            JsonObject data = resp.createNestedObject("data");
            data["success"] = isValid;
            data["token"] = token;
            if (isValid) {
              data["username"] = username;
              registerActiveClient(num, username);
            }
            String output;
            serializeJson(resp, output);
            webSocket.sendTXT(num, output);
            
            if (isValid) {
                sendInitialData(num, username);
            }
          } else if (event == "connect_user") {
            String targetPeer = doc["data"]["username"] | "";
            bool userExists = false;
            String dummyP, dummyT;
            userExists = loadUser(targetPeer, dummyP, dummyT);

            String initiator = "";
            for (const auto& c : activeClients) {
              if (c.num == num) {
                initiator = c.username;
                break;
              }
            }
            if (targetPeer == initiator) userExists = true;

            if (userExists) {
              String connData = readFS("/connections.json");
              DynamicJsonDocument connDoc(4096);
              deserializeJson(connDoc, connData);
              JsonArray connArr = connDoc.as<JsonArray>();
              if (connArr.isNull()) connArr = connDoc.to<JsonArray>();

              bool connExists = false;
              for (int i = 0; i < connArr.size(); i++) {
                JsonObject c = connArr[i];
                String u1 = c["user1"]["username"] | "";
                String u2 = c["user2"]["username"] | "";
                if ((u1 == initiator && u2 == targetPeer) || (u1 == targetPeer && u2 == initiator)) {
                  connExists = true;
                  break;
                }
              }
              if (!connExists) {
                JsonObject newConn = connArr.createNestedObject();
                JsonObject u1Obj = newConn.createNestedObject("user1");
                u1Obj["username"] = initiator;
                u1Obj["nickname"] = "";
                JsonObject u2Obj = newConn.createNestedObject("user2");
                u2Obj["username"] = targetPeer;
                u2Obj["nickname"] = "";
                String outStr;
                serializeJson(connDoc, outStr);
                writeFS("/connections.json", outStr);

                broadcastFileUpdate("connections");
              }

              DynamicJsonDocument resp(256);
              resp["event"] = "connect_user_response";
              JsonObject data = resp.createNestedObject("data");
              data["success"] = true;
              data["username"] = targetPeer;
              String output;
              serializeJson(resp, output);
              webSocket.sendTXT(num, output);
              sendInitialData(num, initiator);
            } else {
              DynamicJsonDocument resp(256);
              resp["event"] = "connect_user_response";
              JsonObject data = resp.createNestedObject("data");
              data["success"] = false;
              data["username"] = targetPeer;
              data["message"] = "Target username does not exist on this node.";
              String output;
              serializeJson(resp, output);
              webSocket.sendTXT(num, output);
            }
          } else if (event == "get_connections") {
            String activeUser = "";
            for (const auto& c : activeClients) {
              if (c.num == num) {
                activeUser = c.username;
                break;
              }
            }
            sendInitialData(num, activeUser);
          } else if (event == "get_block_list") {
            String activeUser = "";
            for (const auto& c : activeClients) {
              if (c.num == num) {
                activeUser = c.username;
                break;
              }
            }
            DynamicJsonDocument blkResp(4096);
            blkResp["event"] = "block_list";
            JsonArray blkArrData = blkResp.createNestedArray("data");
            String bData = readFS("/block_list.json");
            DynamicJsonDocument blkDoc(4096);
            deserializeJson(blkDoc, bData);
            JsonArray blkArr = blkDoc.as<JsonArray>();
            for (int i = 0; i < blkArr.size(); i++) {
              JsonObject b = blkArr[i];
              if (b["blocker"].as<String>() == activeUser) {
                JsonObject row = blkArrData.createNestedObject();
                row["username"] = b["blocked"];
              }
            }
            String blkOut;
            serializeJson(blkResp, blkOut);
            webSocket.sendTXT(num, blkOut);
          } else if (event == "group_create") {
            String groupName = doc["data"]["groupName"] | "";
            String creator = "";
            for (const auto& c : activeClients) {
              if (c.num == num) {
                creator = c.username;
                break;
              }
            }
            String groupId = "";
            for (int i = 0; i < 8; i++) {
              groupId += String(random(0, 10));
            }

            String gData = readFS("/groups.json");
            DynamicJsonDocument groupsDoc(4096);
            deserializeJson(groupsDoc, gData);
            JsonArray groupsArr = groupsDoc.as<JsonArray>();
            if (groupsArr.isNull()) groupsArr = groupsDoc.to<JsonArray>();

            JsonObject newGroup = groupsArr.createNestedObject();
            newGroup["id"] = groupId;
            newGroup["name"] = groupName;
            JsonArray adminsArr = newGroup.createNestedArray("admins");
            adminsArr.add(creator);
            JsonArray usersArr = newGroup.createNestedArray("users");
            usersArr.add(creator);

            String outStr;
            serializeJson(groupsDoc, outStr);
            writeFS("/groups.json", outStr);

            broadcastFileUpdate("groups");

            DynamicJsonDocument resp(256);
            resp["event"] = "group_create_response";
            JsonObject data = resp.createNestedObject("data");
            data["success"] = true;
            data["groupId"] = groupId;
            data["groupName"] = groupName;
            String output;
            serializeJson(resp, output);
            webSocket.sendTXT(num, output);
            sendInitialData(num, creator);
          } else if (event == "group_join") {
            String groupId = doc["data"]["groupId"] | "";
            String joinUser = "";
            for (const auto& c : activeClients) {
              if (c.num == num) {
                joinUser = c.username;
                break;
              }
            }
            bool groupFound = false;
            String foundGroupName = "";

            String gData = readFS("/groups.json");
            DynamicJsonDocument groupsDoc(4096);
            deserializeJson(groupsDoc, gData);
            JsonArray groupsArr = groupsDoc.as<JsonArray>();

            for (int i = 0; i < groupsArr.size(); i++) {
              JsonObject g = groupsArr[i];
              if (g["id"].as<String>() == groupId) {
                groupFound = true;
                foundGroupName = g["name"].as<String>();
                JsonArray users = g["users"].as<JsonArray>();
                bool alreadyIn = false;
                for (int j = 0; j < users.size(); j++) {
                  if (users[j].as<String>() == joinUser) {
                    alreadyIn = true;
                    break;
                  }
                }
                if (!alreadyIn) {
                    users.add(joinUser);
                }
                break;
              }
            }

            if (groupFound) {
              String outStr;
              serializeJson(groupsDoc, outStr);
              writeFS("/groups.json", outStr);
              broadcastFileUpdate("groups");
              sendGroupSystemMessage(groupId, joinUser + " joined the group.");
            }

            DynamicJsonDocument resp(256);
            resp["event"] = "group_join_response";
            JsonObject data = resp.createNestedObject("data");
            data["success"] = groupFound;
            data["groupId"] = groupId;
            data["groupName"] = foundGroupName;
            String output;
            serializeJson(resp, output);
            webSocket.sendTXT(num, output);
            if (groupFound) sendInitialData(num, joinUser);
          } else if (event == "get_groups") {
            String activeUser = "";
            for (const auto& c : activeClients) {
              if (c.num == num) {
                activeUser = c.username;
                break;
              }
            }
            sendInitialData(num, activeUser);
          } else if (event == "get_group_info") {
            String groupId = doc["data"]["groupId"] | "";
            String gData = readFS("/groups.json");
            DynamicJsonDocument groupsDoc(4096);
            deserializeJson(groupsDoc, gData);
            JsonArray groupsArr = groupsDoc.as<JsonArray>();

            for (int i = 0; i < groupsArr.size(); i++) {
              JsonObject g = groupsArr[i];
              if (g["id"].as<String>() == groupId) {
                DynamicJsonDocument resp(1024);
                resp["event"] = "group_info_res";
                JsonObject data = resp.createNestedObject("data");
                data["groupId"] = groupId;
                data["users"] = g["users"];
                data["admins"] = g["admins"];
                String output;
                serializeJson(resp, output);
                webSocket.sendTXT(num, output);
                break;
              }
            }
          } else if (event == "message" || event == "location") {
            String sender = doc["data"]["sender"] | "";
            String receiver = doc["data"]["receiver"] | "";
            
            bool bMe, bPeer;
            if (isBlocked(sender, receiver, bMe, bPeer)) {
                Serial.println("[WSS] Message rejected due to active blockade policy.");
                DynamicJsonDocument errorResp(256);
                errorResp["event"] = "send_error";
                JsonObject errData = errorResp.createNestedObject("data");
                errData["message"] = "Transmission blocked by network policy.";
                String outError;
                serializeJson(errorResp, outError);
                webSocket.sendTXT(num, outError);
                return;
            }

            uint8_t targetNum;
            if (receiver == sender) {
              String echoOutput;
              serializeJson(doc, echoOutput);
              webSocket.sendTXT(num, echoOutput);
            } else if (isLocalActive(receiver, targetNum)) {
              String msgOutput;
              serializeJson(doc, msgOutput);
              webSocket.sendTXT(targetNum, msgOutput);

              DynamicJsonDocument delResp(256);
              delResp["event"] = "msg_delivered";
              JsonObject dData = delResp.createNestedObject("data");
              dData["receiver"] = receiver;
              String delStr;
              serializeJson(delResp, delStr);
              webSocket.sendTXT(num, delStr);
            } else {
              String rawStr;
              serializeJson(doc["data"], rawStr);
              String msgType = doc["data"]["type"] | "text";
              
              if (msgType.startsWith("img_")) {
                  Serial.println("[MSG_TX] App-level image protocol (" + String(rawStr.length()) + " bytes). Instant LoRa TX.");
                  sendLoRa("message", String(millis()), rawStr);
              } else if (rawStr.length() <= 120) {
                  Serial.println("[MSG_TX] Short payload (" + String(rawStr.length()) + " bytes). Instant LoRa TX.");
                  sendLoRa("message", String(millis()), rawStr);
              } else {
                  Serial.println("[MSG_TX] Large payload (" + String(rawStr.length()) + " bytes). Passing to msg_chunk framework.");
                  queueMessageChunks("message", String(millis()), rawStr);
              }
            }
          } else if (event == "group_message") {
            String groupId = doc["data"]["groupId"] | "";
            String sender = "";
            for (const auto& c : activeClients) {
              if (c.num == num) {
                sender = c.username;
                break;
              }
            }
            doc["data"]["sender"] = sender;

            String gData = readFS("/groups.json");
            DynamicJsonDocument groupsDoc(4096);
            deserializeJson(groupsDoc, gData);
            JsonArray groupsArr = groupsDoc.as<JsonArray>();

            for (int i = 0; i < groupsArr.size(); i++) {
              JsonObject g = groupsArr[i];
              if (g["id"].as<String>() == groupId) {
                JsonArray users = g["users"].as<JsonArray>();
                for (int j = 0; j < users.size(); j++) {
                  if (users[j].as<String>() != sender) {
                    uint8_t targetNum;
                    if (isLocalActive(users[j].as<String>(), targetNum)) {
                      Serial.println("[ROUTING] Mapping group multicast logic toward active client socket.");
                      DynamicJsonDocument fwdDoc(4096);
                      fwdDoc["event"] = "group_message";
                      fwdDoc["data"] = doc["data"];
                      String fwdStr;
                      serializeJson(fwdDoc, fwdStr);
                      webSocket.sendTXT(targetNum, fwdStr);
                    }
                  }
                }
                break;
              }
            }

            String rawStr;
            serializeJson(doc["data"], rawStr);
            String msgType = doc["data"]["type"] | "text";
            
            if (msgType.startsWith("img_")) {
                Serial.println("[MSG_TX] App-level group image protocol (" + String(rawStr.length()) + " bytes). Instant LoRa TX.");
                sendLoRa("group_message", String(millis()), rawStr);
            } else if (rawStr.length() <= 120) {
                Serial.println("[MSG_TX] Short group message (" + String(rawStr.length()) + " bytes). Instant LoRa TX.");
                sendLoRa("group_message", String(millis()), rawStr);
            } else {
                Serial.println("[MSG_TX] Large group message (" + String(rawStr.length()) + " bytes). Passing to msg_chunk framework.");
                queueMessageChunks("group_message", String(millis()), rawStr);
            }
          } else if (event == "block_user" || event == "unblock_user") {
            String target = doc["data"]["target"] | "";
            String sender = doc["data"]["sender"] | "";
            handleBlockStatus(sender, target, event == "block_user");
            sendInitialData(num, sender);
          } else if (event == "delete_chat_both") {
            String target = doc["data"]["target"] | "";
            String sender = doc["data"]["sender"] | "";
            
            uint8_t targetNum;
            if (isLocalActive(target, targetNum)) {
              DynamicJsonDocument fwdDoc(256);
              fwdDoc["event"] = "delete_chat";
              JsonObject dData = fwdDoc.createNestedObject("data");
              dData["peer"] = sender;
              String fwdStr;
              serializeJson(fwdDoc, fwdStr);
              webSocket.sendTXT(targetNum, fwdStr);
            } else {
              savePendingAction(target, "delete_chat", sender);
            }
            
            String rawStr;
            serializeJson(doc["data"], rawStr);
            sendLoRa("delete_chat_both", String(millis()), rawStr);
          } else if (event == "delete_message_both") {
            String target = doc["data"]["target"] | "";
            String sender = doc["data"]["sender"] | "";
            String timestamp = doc["data"]["timestamp"] | "";
            String text = doc["data"]["text"] | "";
            uint8_t targetNum;
            if (isLocalActive(target, targetNum)) {
              DynamicJsonDocument fwdDoc(512);
              fwdDoc["event"] = "delete_message_both";
              fwdDoc["data"] = doc["data"];
              String fwdStr;
              serializeJson(fwdDoc, fwdStr);
              webSocket.sendTXT(targetNum, fwdStr);
            } else {
              savePendingAction(target, "delete_message_both", sender, timestamp, text);
            }
            
            String rawStr;
            serializeJson(doc["data"], rawStr);
            sendLoRa("delete_message_both", String(millis()), rawStr);
          } else if (event == "delete_group_message_both") {
            String groupId = doc["data"]["groupId"] | "";
            String sender = doc["data"]["sender"] | "";

            String gData = readFS("/groups.json");
            DynamicJsonDocument groupsDoc(4096);
            deserializeJson(groupsDoc, gData);
            JsonArray groupsArr = groupsDoc.as<JsonArray>();

            for (int i = 0; i < groupsArr.size(); i++) {
              JsonObject g = groupsArr[i];
              if (g["id"].as<String>() == groupId) {
                JsonArray users = g["users"].as<JsonArray>();
                for (int j = 0; j < users.size(); j++) {
                  if (users[j].as<String>() != sender) {
                    uint8_t targetNum;
                    if (isLocalActive(users[j].as<String>(), targetNum)) {
                      DynamicJsonDocument fwdDoc(512);
                      fwdDoc["event"] = "delete_group_message_both";
                      fwdDoc["data"] = doc["data"];
                      String fwdStr;
                      serializeJson(fwdDoc, fwdStr);
                      webSocket.sendTXT(targetNum, fwdStr);
                    }
                  }
                }
                break;
              }
            }
            
            String rawStr;
            serializeJson(doc["data"], rawStr);
            sendLoRa("delete_group_message_both", String(millis()), rawStr);
          } else if (event == "app_version_req") {
            if (sdAvailable && currentAppVersion > 0) {
              DynamicJsonDocument resDoc(256);
              resDoc["version"] = currentAppVersion;
              String raw;
              serializeJson(resDoc, raw);
              sendLoRa("app_version_res", String(millis()), raw);
            }
          } else if (event == "app_version_res") {
            float remoteVer = doc["data"]["version"].as<float>();
            if (remoteVer > currentAppVersion) {
              oledStatus = "New App Ver: " + String(remoteVer);
              updateDisplay();
            }
          } else if (event == "app_transfer_req") {
            if (sdAvailable && SD.exists("/app/app-release.apk")) {
              File file = SD.open("/app/app-release.apk", "r");
              if (file) {
                uint8_t buf[64];
                int bytesRead = file.read(buf, sizeof(buf));
                file.close();
                if (bytesRead > 0) {
                  unsigned char out[128];
                  size_t out_len;
                  mbedtls_base64_encode(out, sizeof(out), &out_len, buf, bytesRead);
                  DynamicJsonDocument resDoc(1024);
                  resDoc["chunk"] = 0;
                  resDoc["data"] = String((char*)out);
                  String outStr;
                  serializeJson(resDoc, outStr);
                  sendLoRa("app_transfer_res", String(millis()), outStr);
                }
              }
            }
          } else if (event == "app_transfer_res") {
            if (isDownloadingApp) {
              int chunk = doc["data"]["chunk"] | 0;
              oledStatus = "APK DL Chunk: " + String(chunk);
              updateDisplay();
            }
          }
        }
        break;
    }
  } catch (...) {
      Serial.println("[WSS] Handled unexpected exception processing socket message.");
  }
}

void checkRemoteUsers() {
  unsigned long now = millis();
  auto it = remoteActiveUsers.begin();
  bool changed = false;
  while (it != remoteActiveUsers.end()) {
    if (now - it->lastSeen > 45000) {
      it = remoteActiveUsers.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed) notifyAllActiveClients();
}

void checkNeighbors() {
  try {
    unsigned long now = millis();
    auto it = neighbors.begin();
    bool changed = false;
    while (it != neighbors.end()) {
      if (now - it->lastSeen > 30000) {
        Serial.println("[LORA_NET] Device went offline. Purging neighbor: " + it->nodeName);
        it = neighbors.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
    if (changed) updateDisplay();
  } catch (...) {}
}

void broadcastPing() {
  try {
    Serial.println("[LORA_TX] Broadcasting network ping.");
    DynamicJsonDocument doc(256);
    doc["type"] = "ping";
    doc["node"] = nodeName;
    String output;
    serializeJson(doc, output);
    LoRa.beginPacket();
    LoRa.print(output);
    LoRa.endPacket();
  } catch (...) {}
}

void handleIncomingLoRaPacket(int packetSize) {
  try {
    String packet = "";
    packet.reserve(packetSize + 1);
    while (LoRa.available()) {
      packet += (char)LoRa.read();
    }

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, packet);
    if (err) {
      Serial.println("[LORA_RX] Failed to parse JSON envelope. Discarding block.");
      return;
    }

    String type = doc["type"] | "";
    
    if (type != "sync_chunk" && type != "msg_chunk") {
         Serial.println("[LORA_RX] Envelope identified as: " + type);
    }

    if (type == "ping") {
      String fromNode = doc["node"] | "";
      int currentRssi = LoRa.packetRssi();
      bool found = false;
      for (auto& n : neighbors) {
        if (n.nodeName == fromNode) {
          n.rssi = currentRssi;
          n.lastSeen = millis();
          found = true;
          break;
        }
      }
      if (!found) {
        Serial.println("[LORA_NET] Detected new node link: " + fromNode);
        Neighbor newN;
        newN.nodeName = fromNode;
        newN.rssi = currentRssi;
        newN.lastSeen = millis();
        neighbors.push_back(newN);
        if (!isReceivingStream() && txQueue.empty()) {
            requestVersionsFromClosestNeighbor();
        }
      }
      updateDisplay();
    } else if (type == "presence") {
      String saltedData = doc["data"] | "";
      String rawData = removeSalt(saltedData);
      DynamicJsonDocument pDoc(512);
      DeserializationError pErr = deserializeJson(pDoc, rawData);
      if (!pErr) {
        JsonArray arr = pDoc["users"].as<JsonArray>();
        bool changed = false;
        for (int i = 0; i < arr.size(); i++) {
            String u = arr[i].as<String>();
            bool found = false;
            for (auto& r : remoteActiveUsers) {
                if (r.username == u) {
                    if (millis() - r.lastSeen > 40000) changed = true;
                    r.lastSeen = millis();
                    found = true;
                    break;
                }
            }
            if (!found) {
                RemoteUser ru;
                ru.username = u;
                ru.lastSeen = millis();
                remoteActiveUsers.push_back(ru);
                changed = true;
            }
        }
        if (changed) notifyAllActiveClients();
      }
    } else if (type == "req_version") {
      String saltedData = doc["data"] | "";
      String rawData = removeSalt(saltedData);
      DynamicJsonDocument reqDoc(256);
      deserializeJson(reqDoc, rawData);
      String target = reqDoc["targetNode"].as<String>();
      if (target == nodeName) {
         Serial.println("[SYNC_RX] Targeted version request received. Replying with ledger.");
         sendLocalVersions();
      }
    } else if (type == "version_check") {
      String saltedData = doc["data"] | "";
      String rawData = removeSalt(saltedData);
      DynamicJsonDocument wrapDoc(1024);
      deserializeJson(wrapDoc, rawData);
      
      String remoteNode = wrapDoc["node"].as<String>();
      String vJson = wrapDoc["versions"].as<String>();
      DynamicJsonDocument remoteVersions(512);
      deserializeJson(remoteVersions, vJson);
      
      String localVData = readFS("/data_versions.json");
      DynamicJsonDocument localVersions(512);
      deserializeJson(localVersions, localVData);

      Serial.println("[SYNC] Processing version ledger from node: " + remoteNode);
      String keys[] = {"users", "connections", "groups", "block_list"};
      for (String k : keys) {
        int locV = localVersions[k] | 0;
        int remV = remoteVersions[k] | 0;
        Serial.println("[SYNC] Record: " + k + " | Local: " + String(locV) + " | Remote: " + String(remV));
        
        if (locV < remV) {
          Serial.println("[SYNC] Outdated record identified. Queueing strict request for: " + k + " from node: " + remoteNode);
          bool inQueue = false;
          for (auto& task : syncQueue) {
              if (task.fileKey == k) { inQueue = true; break; }
          }
          if (!inQueue) {
             SyncTask st;
             st.fileKey = k;
             st.targetNode = remoteNode;
             syncQueue.push_back(st);
          }
        }
      }
      
      if (!syncQueue.empty() && !isSyncing) {
        Serial.println("[SYNC] Commencing synchronized file requests sequence.");
        requestNextSyncFile();
      }
    } else if (type == "req_file") {
      String saltedData = doc["data"] | "";
      String rawData = removeSalt(saltedData);
      DynamicJsonDocument reqDoc(256);
      deserializeJson(reqDoc, rawData);
      String k = reqDoc["fileKey"].as<String>();
      String tNode = reqDoc["targetNode"].as<String>();
      
      if (tNode == nodeName) {
         Serial.println("[SYNC_TX] Direct file request received and approved for sequence: " + k);
         broadcastFileUpdateWithoutIncrement(k);
      }
    } else if (type == "sync_chunk") {
      lastChunkRxTime = millis();
      String saltedData = doc["data"] | "";
      String rawData = removeSalt(saltedData);
      DynamicJsonDocument fileDoc(1024);
      DeserializationError fErr = deserializeJson(fileDoc, rawData);
      
      if (fErr) {
         Serial.println("[SYNC_RX] Severe parsing disruption in sync chunk stream.");
      } else {
          String fileKey = fileDoc["fileKey"].as<String>();
          int remoteV = fileDoc["version"].as<int>();
          int chunk = fileDoc["chunk"].as<int>();
          int total = fileDoc["total"].as<int>();
          String chunkData = fileDoc["data"].as<String>();

          Serial.println("[SYNC_RX] Received Base64 chunk " + String(chunk+1) + "/" + String(total) + " for " + fileKey);

          if (chunk == 0) {
              syncBuffer = chunkData;
              syncBuffer.reserve(total * 48);
              expectedChunkIndex = 1;
          } else if (chunk == expectedChunkIndex) {
              syncBuffer += chunkData;
              expectedChunkIndex++;
          } else {
              Serial.println("[SYNC_RX] Chunk sequence mismatch! Expected: " + String(expectedChunkIndex) + ", Got: " + String(chunk));
              return;
          }

          if (chunk == total - 1) {
              int localV = getFileVersion(fileKey);
              if (remoteV > localV) {
                String decodedContent = base64Decode(syncBuffer);
                if (decodedContent.length() > 0) {
                    Serial.println("[SYNC_RX] Base64 reassembled and decoded successfully: " + fileKey + " to v" + String(remoteV));
                    writeFS("/" + fileKey + ".json", decodedContent);
                    setFileVersion(fileKey, remoteV);
                    oledStatus = "Sync " + fileKey + " v" + String(remoteV);
                    updateDisplay();
                    notifyAllActiveClients();
                } else {
                    Serial.println("[SYNC_RX] Base64 decoding process failed on the reassembled buffer.");
                }
              }
              
              if (isSyncing && currentSyncFile == fileKey) {
                  Serial.println("[SYNC_RX] Queue sequence validated. Transitioning to next request marker.");
                  if (!syncQueue.empty()) syncQueue.erase(syncQueue.begin());
                  requestNextSyncFile();
              }
          }
      }
    } else if (type == "msg_chunk") {
      lastChunkRxTime = millis();
      String saltedData = doc["data"] | "";
      String rawData = removeSalt(saltedData);
      DynamicJsonDocument msgDoc(1024);
      DeserializationError fErr = deserializeJson(msgDoc, rawData);
      
      if (fErr) {
         Serial.println("[MSG_RX] Severe parsing disruption in msg chunk stream.");
      } else {
          String reqId = msgDoc["reqId"].as<String>();
          String msgType = msgDoc["msgType"].as<String>();
          int chunk = msgDoc["chunk"].as<int>();
          int total = msgDoc["total"].as<int>();
          String chunkData = msgDoc["data"].as<String>();

          Serial.println("[MSG_RX] Received Base64 slice " + String(chunk+1) + "/" + String(total) + " for message ID: " + reqId);

          if (chunk == 0) {
              msgBuffer = chunkData;
              msgBuffer.reserve(total * 48);
              expectedMsgChunkIndex = 1;
              currentMsgReqId = reqId;
          } else if (chunk == expectedMsgChunkIndex && reqId == currentMsgReqId) {
              msgBuffer += chunkData;
              expectedMsgChunkIndex++;
          } else {
              Serial.println("[MSG_RX] Slice sequence mismatch! Expected: " + String(expectedMsgChunkIndex) + ", Got: " + String(chunk));
              return;
          }

          if (chunk == total - 1) {
              String decodedContent = base64Decode(msgBuffer);
              msgBuffer = "";
              if (decodedContent.length() > 0) {
                  Serial.println("[MSG_RX] Message reassembled and decoded successfully. Payload size: " + String(decodedContent.length()));
                  
                  DynamicJsonDocument innerDoc(8192);
                  DeserializationError jsonErr = deserializeJson(innerDoc, decodedContent);

                  if (!jsonErr) {
                      if (msgType == "message") {
                          String receiver = innerDoc["receiver"] | "";
                          String sender = innerDoc["sender"] | "";
                          
                          bool bMe, bPeer;
                          if (!isBlocked(sender, receiver, bMe, bPeer)) {
                            uint8_t targetNum;
                            if (isLocalActive(receiver, targetNum)) {
                              Serial.println("[ROUTING] Direct internal delivery executed to client socket.");
                              DynamicJsonDocument fwdDoc(8192);
                              fwdDoc["event"] = "message";
                              fwdDoc["data"] = innerDoc;
                              String fwdStr;
                              serializeJson(fwdDoc, fwdStr);
                              webSocket.sendTXT(targetNum, fwdStr);

                              DynamicJsonDocument delDoc(256);
                              delDoc["receiver"] = receiver;
                              String delRaw;
                              serializeJson(delDoc, delRaw);
                              sendLoRa("msg_delivered", reqId, delRaw);
                            }
                          } else {
                             Serial.println("[ROUTING] Access denied. Packet discarded due to block rule.");
                          }
                      } else if (msgType == "group_message") {
                          String groupId = innerDoc["groupId"] | "";
                          String sender = innerDoc["sender"] | "";

                          String gData = readFS("/groups.json");
                          DynamicJsonDocument groupsDoc(4096);
                          deserializeJson(groupsDoc, gData);
                          JsonArray groupsArr = groupsDoc.as<JsonArray>();

                          for (int i = 0; i < groupsArr.size(); i++) {
                            JsonObject g = groupsArr[i];
                            if (g["id"].as<String>() == groupId) {
                              JsonArray users = g["users"].as<JsonArray>();
                              for (int j = 0; j < users.size(); j++) {
                                if (users[j].as<String>() != sender) {
                                  uint8_t targetNum;
                                  if (isLocalActive(users[j].as<String>(), targetNum)) {
                                    Serial.println("[ROUTING] Mapping group multicast logic toward active client socket.");
                                    DynamicJsonDocument fwdDoc(8192);
                                    fwdDoc["event"] = "group_message";
                                    fwdDoc["data"] = innerDoc;
                                    String fwdStr;
                                    serializeJson(fwdDoc, fwdStr);
                                    webSocket.sendTXT(targetNum, fwdStr);
                                  }
                                }
                              }
                              break;
                            }
                          }
                      }
                  } else {
                      Serial.println("[MSG_RX] Reassembled message JSON parsing failed!");
                  }
              } else {
                  Serial.println("[MSG_RX] Base64 decoding process failed on the reassembled message buffer.");
              }
          }
      }
    } else {
      String reqId = doc["req_id"] | "";
      String saltedData = doc["data"] | "";
      String rawData = removeSalt(saltedData);

      if (rawData.length() > 0) {
        DynamicJsonDocument innerDoc(4096);
        deserializeJson(innerDoc, rawData);

        if (type == "message") {
          String receiver = innerDoc["receiver"] | "";
          String sender = innerDoc["sender"] | "";
          
          bool bMe, bPeer;
          if (!isBlocked(sender, receiver, bMe, bPeer)) {
            uint8_t targetNum;
            if (isLocalActive(receiver, targetNum)) {
              Serial.println("[ROUTING] Direct internal delivery executed to client socket.");
              DynamicJsonDocument fwdDoc(4096);
              fwdDoc["event"] = "message";
              fwdDoc["data"] = innerDoc;
              String fwdStr;
              serializeJson(fwdDoc, fwdStr);
              webSocket.sendTXT(targetNum, fwdStr);

              DynamicJsonDocument delDoc(256);
              delDoc["receiver"] = receiver;
              String delRaw;
              serializeJson(delDoc, delRaw);
              sendLoRa("msg_delivered", reqId, delRaw);
            }
          } else {
             Serial.println("[ROUTING] Access denied. Packet discarded due to block rule.");
          }
        } else if (type == "msg_delivered") {
          String sender = innerDoc["receiver"] | "";
          uint8_t targetNum;
          if (isLocalActive(sender, targetNum)) {
            DynamicJsonDocument fwdDoc(256);
            fwdDoc["event"] = "msg_delivered";
            fwdDoc["data"] = innerDoc;
            String fwdStr;
            serializeJson(fwdDoc, fwdStr);
            webSocket.sendTXT(targetNum, fwdStr);
          }
        } else if (type == "group_message") {
          String groupId = innerDoc["groupId"] | "";
          String sender = innerDoc["sender"] | "";

          String gData = readFS("/groups.json");
          DynamicJsonDocument groupsDoc(4096);
          deserializeJson(groupsDoc, gData);
          JsonArray groupsArr = groupsDoc.as<JsonArray>();

          for (int i = 0; i < groupsArr.size(); i++) {
            JsonObject g = groupsArr[i];
            if (g["id"].as<String>() == groupId) {
              JsonArray users = g["users"].as<JsonArray>();
              for (int j = 0; j < users.size(); j++) {
                if (users[j].as<String>() != sender) {
                  uint8_t targetNum;
                  if (isLocalActive(users[j].as<String>(), targetNum)) {
                    Serial.println("[ROUTING] Mapping group multicast logic toward active client socket.");
                    DynamicJsonDocument fwdDoc(4096);
                    fwdDoc["event"] = "group_message";
                    fwdDoc["data"] = innerDoc;
                    String fwdStr;
                    serializeJson(fwdDoc, fwdStr);
                    webSocket.sendTXT(targetNum, fwdStr);
                  }
                }
              }
              break;
            }
          }
        } else if (type == "delete_chat_both") {
          String target = innerDoc["target"] | "";
          String sender = innerDoc["sender"] | "";
          uint8_t targetNum;
          if (isLocalActive(target, targetNum)) {
            DynamicJsonDocument fwdDoc(256);
            fwdDoc["event"] = "delete_chat";
            JsonObject dData = fwdDoc.createNestedObject("data");
            dData["peer"] = sender;
            String fwdStr;
            serializeJson(fwdDoc, fwdStr);
            webSocket.sendTXT(targetNum, fwdStr);
          } else {
            savePendingAction(target, "delete_chat", sender);
          }
        } else if (type == "delete_message_both") {
          String target = innerDoc["target"] | "";
          String sender = innerDoc["sender"] | "";
          String timestamp = innerDoc["timestamp"] | "";
          String text = innerDoc["text"] | "";
          uint8_t targetNum;
          if (isLocalActive(target, targetNum)) {
            DynamicJsonDocument fwdDoc(512);
            fwdDoc["event"] = "delete_message_both";
            fwdDoc["data"] = doc["data"];
            String fwdStr;
            serializeJson(fwdDoc, fwdStr);
            webSocket.sendTXT(targetNum, fwdStr);
          } else {
            savePendingAction(target, "delete_message_both", sender, timestamp, text);
          }
        } else if (type == "delete_group_message_both") {
          String groupId = innerDoc["groupId"] | "";
          String sender = innerDoc["sender"] | "";

          String gData = readFS("/groups.json");
          DynamicJsonDocument groupsDoc(4096);
          deserializeJson(groupsDoc, gData);
          JsonArray groupsArr = groupsDoc.as<JsonArray>();

          for (int i = 0; i < groupsArr.size(); i++) {
            JsonObject g = groupsArr[i];
            if (g["id"].as<String>() == groupId) {
              JsonArray users = g["users"].as<JsonArray>();
              for (int j = 0; j < users.size(); j++) {
                if (users[j].as<String>() != sender) {
                  uint8_t targetNum;
                  if (isLocalActive(users[j].as<String>(), targetNum)) {
                    DynamicJsonDocument fwdDoc(512);
                    fwdDoc["event"] = "delete_group_message_both";
                    fwdDoc["data"] = doc["data"];
                    String fwdStr;
                    serializeJson(fwdDoc, fwdStr);
                    webSocket.sendTXT(targetNum, fwdStr);
                  }
                }
              }
              break;
            }
          }
        } else if (type == "app_version_req") {
          if (sdAvailable && currentAppVersion > 0) {
            DynamicJsonDocument resDoc(256);
            resDoc["version"] = currentAppVersion;
            String raw;
            serializeJson(resDoc, raw);
            sendLoRa("app_version_res", String(millis()), raw);
          }
        } else if (type == "app_version_res") {
          float remoteVer = innerDoc["version"].as<float>();
          if (remoteVer > currentAppVersion) {
            oledStatus = "New App Ver: " + String(remoteVer);
            updateDisplay();
          }
        } else if (type == "app_transfer_req") {
          if (sdAvailable && SD.exists("/app/app-release.apk")) {
            File file = SD.open("/app/app-release.apk", "r");
            if (file) {
              uint8_t buf[64];
              int bytesRead = file.read(buf, sizeof(buf));
              file.close();
              if (bytesRead > 0) {
                unsigned char out[128];
                size_t out_len;
                mbedtls_base64_encode(out, sizeof(out), &out_len, buf, bytesRead);
                DynamicJsonDocument resDoc(1024);
                resDoc["chunk"] = 0;
                resDoc["data"] = String((char*)out);
                String outStr;
                serializeJson(resDoc, outStr);
                sendLoRa("app_transfer_res", String(millis()), outStr);
              }
            }
          }
        } else if (type == "app_transfer_res") {
          if (isDownloadingApp) {
            int chunk = doc["data"]["chunk"] | 0;
            oledStatus = "APK DL Chunk: " + String(chunk);
            updateDisplay();
          }
        }
      }
    }
  } catch (...) {
      Serial.println("[LORA_RX] General exception during packet sequence extraction.");
  }
}

void startWebServer(){
    Serial.println("[WIFI_WEB] Initializing Web Server HTTP Endpoints.");
    webServer.on("/", HTTP_GET, []() {
      String html = "<html><body style='font-family:sans-serif; padding:20px;'>";
      html += "<h1>PIGEON Node Configuration</h1>";
      if (sdAvailable) {
        html += "<p><a href='/download'>Download App (APK)</a></p><hr>";
      } else {
        html += "<p>SD Card Offline - APK Download Unavailable</p><hr>";
      }
      html += "<h2>Repeater WiFi Settings</h2>";
      html += "<form action='/update_wifi' method='POST'>";
      html += "SSID: <input type='text' name='ssid' value='" + repeaterSSID + "'><br><br>";
      html += "Pass: <input type='text' name='pass' value='" + repeaterPass + "'><br><br>";
      html += "<input type='submit' value='Update WiFi'>";
      html += "</form><hr>";
      html += "<h2>Node Name Settings</h2>";
      html += "<form action='/update_node' method='POST'>";
      html += "Node (P1-P99): <input type='text' name='node' value='" + nodeName + "'><br><br>";
      html += "<input type='submit' value='Update Node'>";
      html += "</form>";
      html += "</body></html>";
      webServer.send(200, "text/html", html);
    });

    webServer.on("/update_wifi", HTTP_POST, []() {
      if (webServer.hasArg("ssid") && webServer.hasArg("pass")) {
        repeaterSSID = webServer.arg("ssid");
        repeaterPass = webServer.arg("pass");
        saveWifiConfig();
        webServer.send(200, "text/html", "<h1>WiFi Updated! Rebooting...</h1>");
        Serial.println("[WIFI_WEB] Repeater configuration updated. Restarting node sequence.");
        delay(1000);
        ESP.restart();
      } else {
        webServer.send(400, "text/plain", "Missing parameters");
      }
    });

    webServer.on("/update_node", HTTP_POST, []() {
      if (webServer.hasArg("node")) {
        nodeName = webServer.arg("node");
        saveWifiConfig();
        webServer.send(200, "text/html", "<h1>Node Name Updated! Rebooting...</h1>");
        Serial.println("[WIFI_WEB] Node Name updated. Restarting node sequence.");
        delay(1000);
        ESP.restart();
      } else {
        webServer.send(400, "text/plain", "Missing parameters");
      }
    });

    webServer.on("/download", []() {
      if (sdAvailable) {
        File file = SD.open("/app/app-release.apk");
        if (file) {
          webServer.streamFile(file, "application/vnd.android.package-archive");
          file.close();
        } else {
          webServer.send(404, "text/plain", "APK File Not Found on SD Card");
        }
      } else {
        webServer.send(404, "text/plain", "Not Found the SD Card");
      }
    });

    webServer.begin();
}

void setup() {
  try {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== PIGEON NODE BOOTING SEQUENCE ===");

    SPIFFS.begin(true);
    Serial.println("[SYS] SPIFFS local storage mounted.");
    loadWifiConfig();
    
    String vData = readFS("/data_versions.json");
    if (vData == "{}" || vData == "") {
      Serial.println("[SYS] Bootstrapping primary data_versions.json file parameters.");
      DynamicJsonDocument vDoc(512);
      vDoc["users"] = 0;
      vDoc["connections"] = 0;
      vDoc["groups"] = 0;
      vDoc["block_list"] = 0;
      String out;
      serializeJson(vDoc, out);
      writeFS("/data_versions.json", out);
      vData = out;
    }

    Serial.println("[SYS] --- FIRMWARE SYNC VERSION LEDGER ---");
    Serial.println(vData);
    Serial.println("[SYS] ------------------------------------");
    
    Wire.begin(SDA_PIN, SCL_PIN);
    if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
      display.clearDisplay();
      display.display();
      Serial.println("[SYS] OLED Diagnostic Display configured.");
    }

    WiFi.mode(WIFI_AP_STA);
    nodePassword = generatePassword(nodeName);
    Serial.println("[WIFI] Switching core state to AP_STA Mode.");
    Serial.println("[WIFI] Node SoftAP Creds >> ID: " + nodeName + " PASS: " + nodePassword);
    WiFi.softAP(nodeName.c_str(), nodePassword.c_str());
    
    Serial.println("[WIFI] Requesting upstream link configuration: " + repeaterSSID);
    WiFi.begin(repeaterSSID.c_str(), repeaterPass.c_str());

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif) {
        Serial.println("[WIFI] Applying NAPT packet modifications for DNS transparency.");
        esp_netif_dhcps_stop(netif);
        esp_netif_napt_enable(netif);
        esp_netif_dns_info_t dns;
        dns.ip.u_addr.ip4.addr = inet_addr("8.8.8.8");
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        uint8_t opt_val = 1; 
        esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &opt_val, sizeof(opt_val));
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
        esp_netif_dhcps_start(netif);
    }

    sdSPI.begin(19, 5, 18, 21);
    if (SD.begin(21, sdSPI)) {
      sdAvailable = true;
      Serial.println("[SYS] SD Card external memory Mounted.");
    } else {
      Serial.println("[SYS] Warning: No valid SD Card signature found.");
    }

    loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    LoRa.setSPI(loraSPI);
    LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);

    if (LoRa.begin(433E6)) {
      LoRa.setTxPower(17);
      LoRa.setSpreadingFactor(7);
      LoRa.setSignalBandwidth(125E3);
      LoRa.setCodingRate4(5);
      LoRa.enableCrc();
      Serial.println("[LORA] Core Transceiver Radio link activated.");
    } else {
      Serial.println("[LORA] Radio chip hardware failure recorded.");
      oledStatus = "LORA INIT FAIL";
      updateDisplay();
      while (1);
    }

    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    Serial.println("[WSS] WebSockets binding on local port 81.");

    startWebServer();

    oledStatus = "System Ready";
    updateDisplay();
    handleAppVersionCheck();

  } catch (...) {
      Serial.println("[SYS] Fatal boot exception logged to crash dump.");
  }
}

void loop() {
  try {
    webSocket.loop();
    webServer.handleClient();

    int packetSize = LoRa.parsePacket();
    if (packetSize) {
      handleIncomingLoRaPacket(packetSize);
    }

    if (!txQueue.empty()) {
        if (millis() - lastTxTime > 300) {
          TxTask& t = txQueue[0];
          String chunkData = t.fullBase64Data.substring(currentChunkIdx * 48, min((currentChunkIdx + 1) * 48, (int)t.fullBase64Data.length()));
          
          DynamicJsonDocument doc(1024);
          if (t.type == "sync") {
              doc["fileKey"] = t.fileKey;
              doc["version"] = t.version;
              doc["chunk"] = currentChunkIdx;
              doc["total"] = t.totalChunks;
              doc["data"] = chunkData;
              String out;
              serializeJson(doc, out);
              sendLoRa("sync_chunk", String(millis()), out);
          } else if (t.type == "msg") {
              doc["reqId"] = t.fileKey;
              doc["msgType"] = t.msgType;
              doc["chunk"] = currentChunkIdx;
              doc["total"] = t.totalChunks;
              doc["data"] = chunkData;
              String out;
              serializeJson(doc, out);
              sendLoRa("msg_chunk", String(millis()), out);
          }
          
          currentChunkIdx++;
          if (currentChunkIdx >= t.totalChunks) {
              txQueue.erase(txQueue.begin());
              currentChunkIdx = 0;
          }
          lastTxTime = millis();
        }
    } else if (isSyncing) {
        if (millis() - lastSyncRequestTime > 15000) {
             Serial.println("[SYNC_TIMEOUT] Time boundary exceeded while retrieving " + currentSyncFile + ". Retrying...");
             requestNextSyncFile();
        }
    } else if (!isReceivingStream()) {
        if (millis() - lastBeaconTime > 10000) {
          broadcastPing();
          lastBeaconTime = millis();
        }

        if (millis() - lastPresenceTime > 25000) {
          if (!activeClients.empty()) {
            broadcastPresence();
          }
          lastPresenceTime = millis();
        }

        if (millis() - lastVersionCheckTime > 60000) {
          requestVersionsFromClosestNeighbor();
          lastVersionCheckTime = millis();
        }
    }

    checkNeighbors();
    checkRemoteUsers();

    int currentClients = WiFi.softAPgetStationNum();
    if (currentClients != prevClientsCount) {
      Serial.println("[WIFI] Active gateway endpoints verified: " + String(currentClients));
      prevClientsCount = currentClients;
      updateDisplay();
    }
  } catch (...) {
      Serial.println("[SYS] Unknown logic bypass caught in main operating envelope loop.");
  }
}