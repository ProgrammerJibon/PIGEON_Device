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

const char* ssid = "P1";
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

unsigned long lastBeaconTime = 0;
bool sdAvailable = false;
int prevClientsCount = -1;
String oledStatus = "System Boot...";
float currentAppVersion = 0.0;
bool isDownloadingApp = false;
int expectedAppChunks = 0;
int currentAppChunk = 0;

String base64Encode(String input) {
    try {
        unsigned char out[32];
        size_t out_len;
        mbedtls_base64_encode(out, sizeof(out), &out_len, (const unsigned char*)input.c_str(), input.length());
        return String((char*)out);
    } catch (...) {
        return "";
    }
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
        display.println(ssid);
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
        DynamicJsonDocument doc(4096);
        doc["type"] = type;
        doc["req_id"] = reqId;
        doc["data"] = applySalt(rawData);
        
        String payload;
        serializeJson(doc, payload);
        
        LoRa.beginPacket();
        LoRa.print(payload);
        LoRa.endPacket();
        
        oledStatus = "TX: " + type;
        updateDisplay();
    } catch (...) {}
}

String readFS(String path) {
    try {
        if (!SPIFFS.exists(path)) return "{}";
        File file = SPIFFS.open(path, "r");
        if (!file) return "{}";
        String data = file.readString();
        file.close();
        return data;
    } catch (...) {
        return "{}";
    }
}

void writeFS(String path, String data) {
    try {
        File file = SPIFFS.open(path, "w");
        if (file) {
            file.print(data);
            file.close();
        }
    } catch (...) {}
}

bool loadUser(String username, String &storedPassword, String &storedToken) {
    try {
        String data = readFS("/users.json");
        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, data);
        if (error) return false;
        
        JsonArray arr = doc.as<JsonArray>();
        for (JsonObject u : arr) {
            if (u["username"] == username) {
                storedPassword = u["password"].as<String>();
                storedToken = u["token"].as<String>();
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool saveUser(String username, String password, String token) {
    try {
        String data = readFS("/users.json");
        DynamicJsonDocument doc(4096);
        deserializeJson(doc, data);
        
        JsonArray arr = doc.as<JsonArray>();
        if (arr.isNull()) arr = doc.to<JsonArray>();
        
        bool found = false;
        for (JsonObject u : arr) {
            if (u["username"] == username) {
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
        return true;
    } catch (...) {
        return false;
    }
}

bool validateStoredToken(String token, String &username) {
    try {
        String data = readFS("/users.json");
        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, data);
        if (error) return false;
        
        JsonArray arr = doc.as<JsonArray>();
        for (JsonObject u : arr) {
            if (u["token"] == token) {
                username = u["username"].as<String>();
                return true;
            }
        }
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
        updateDisplay();
    } catch (...) {}
}

void unregisterActiveClient(uint8_t num) {
    try {
        for (auto it = activeClients.begin(); it != activeClients.end(); ++it) {
            if (it->num == num) {
                activeClients.erase(it);
                break;
            }
        }
    } catch (...) {}
}

bool isUserActive(String username, uint8_t &outNum) {
    try {
        for (const auto& c : activeClients) {
            if (c.username == username) {
                outNum = c.num;
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

void sendInfoEvent(uint8_t num) {
    try {
        DynamicJsonDocument doc(256);
        doc["event"] = "info";
        JsonObject data = doc.createNestedObject("data");
        data["id"] = 1;
        data["name"] = ssid;
        String output;
        serializeJson(doc, output);
        webSocket.sendTXT(num, output);
    } catch (...) {}
}

void requestAppFromMesh() {
    isDownloadingApp = true;
    oledStatus = "Req Mesh App DL";
    updateDisplay();
    DynamicJsonDocument doc(256);
    doc["node"] = ssid;
    String raw;
    serializeJson(doc, raw);
    sendLoRa("app_transfer_req", String(millis()), raw);
}

void handleAppVersionCheck() {
    if (sdAvailable && SD.exists("/app/app-version.txt")) {
        File file = SD.open("/app/app-version.txt", "r");
        if (file) {
            String verStr = file.readString();
            currentAppVersion = verStr.toFloat();
            file.close();
        }
    } else {
        requestAppFromMesh();
    }
    DynamicJsonDocument doc(256);
    doc["version"] = currentAppVersion;
    String raw;
    serializeJson(doc, raw);
    sendLoRa("app_version_req", String(millis()), raw);
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    try {
        switch (type) {
            case WStype_DISCONNECTED:
                unregisterActiveClient(num);
                updateDisplay();
                break;
            case WStype_CONNECTED:
                sendInfoEvent(num);
                updateDisplay();
                break;
            case WStype_TEXT: {
                DynamicJsonDocument doc(4096);
                DeserializationError err = deserializeJson(doc, payload, length);
                if (err) return;
                
                String event = doc["event"] | "";
                
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
                } 
                else if (event == "login") {
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
                            
                            DynamicJsonDocument syncDoc(256);
                            syncDoc["username"] = username;
                            syncDoc["password"] = password;
                            syncDoc["token"] = finalToken;
                            String syncStr;
                            serializeJson(syncDoc, syncStr);
                            sendLoRa("sync_user", String(millis()), syncStr);
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
                }
                else if (event == "token_validate") {
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
                }
                else if (event == "connect_user") {
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
                        for (JsonObject c : connArr) {
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
                            
                            sendLoRa("sync_conn", String(millis()), outStr);
                        }
                        
                        DynamicJsonDocument resp(256);
                        resp["event"] = "connect_user_response";
                        JsonObject data = resp.createNestedObject("data");
                        data["success"] = true;
                        data["username"] = targetPeer;
                        String output;
                        serializeJson(resp, output);
                        webSocket.sendTXT(num, output);
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
                }
                else if (event == "get_connections") {
                    String activeUser = "";
                    for (const auto& c : activeClients) {
                        if (c.num == num) {
                            activeUser = c.username;
                            break;
                        }
                    }
                    DynamicJsonDocument listResp(4096);
                    listResp["event"] = "connections_list";
                    JsonArray dataArr = listResp.createNestedArray("data");
                    
                    String connData = readFS("/connections.json");
                    DynamicJsonDocument connDoc(4096);
                    deserializeJson(connDoc, connData);
                    JsonArray connArr = connDoc.as<JsonArray>();
                    
                    for (JsonObject c : connArr) {
                        String u1 = c["user1"]["username"] | "";
                        String u2 = c["user2"]["username"] | "";
                        if (u1 == activeUser || u2 == activeUser) {
                            String peer = (u1 == activeUser) ? u2 : u1;
                            JsonObject row = dataArr.createNestedObject();
                            row["username"] = peer;
                            uint8_t dummy;
                            row["active"] = isUserActive(peer, dummy);
                        }
                    }
                    String output;
                    serializeJson(listResp, output);
                    webSocket.sendTXT(num, output);
                }
                else if (event == "group_create") {
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
                    
                    DynamicJsonDocument syncDoc(1024);
                    syncDoc["id"] = groupId;
                    syncDoc["name"] = groupName;
                    syncDoc["creator"] = creator;
                    String sStr;
                    serializeJson(syncDoc, sStr);
                    sendLoRa("sync_group", String(millis()), sStr);
                    
                    DynamicJsonDocument resp(256);
                    resp["event"] = "group_create_response";
                    JsonObject data = resp.createNestedObject("data");
                    data["success"] = true;
                    data["groupId"] = groupId;
                    data["groupName"] = groupName;
                    String output;
                    serializeJson(resp, output);
                    webSocket.sendTXT(num, output);
                }
                else if (event == "group_join") {
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
                    
                    for (JsonObject g : groupsArr) {
                        if (g["id"] == groupId) {
                            groupFound = true;
                            foundGroupName = g["name"].as<String>();
                            JsonArray users = g["users"].as<JsonArray>();
                            bool alreadyIn = false;
                            for (String u : users) {
                                if (u == joinUser) { alreadyIn = true; break; }
                            }
                            if (!alreadyIn) users.add(joinUser);
                            break;
                        }
                    }
                    
                    if (groupFound) {
                        String outStr;
                        serializeJson(groupsDoc, outStr);
                        writeFS("/groups.json", outStr);
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
                }
                else if (event == "get_groups") {
                    String activeUser = "";
                    for (const auto& c : activeClients) {
                        if (c.num == num) {
                            activeUser = c.username;
                            break;
                        }
                    }
                    DynamicJsonDocument listResp(4096);
                    listResp["event"] = "groups_list";
                    JsonArray dataArr = listResp.createNestedArray("data");
                    
                    String gData = readFS("/groups.json");
                    DynamicJsonDocument groupsDoc(4096);
                    deserializeJson(groupsDoc, gData);
                    JsonArray groupsArr = groupsDoc.as<JsonArray>();
                    
                    for (JsonObject g : groupsArr) {
                        JsonArray users = g["users"].as<JsonArray>();
                        bool isMember = false;
                        for (String u : users) {
                            if (u == activeUser) { isMember = true; break; }
                        }
                        if (isMember) {
                            JsonObject row = dataArr.createNestedObject();
                            row["id"] = g["id"];
                            row["name"] = g["name"];
                            int activeCount = 0;
                            for (String u : users) {
                                uint8_t d;
                                if (isUserActive(u, d)) activeCount++;
                            }
                            row["activeCount"] = activeCount;
                        }
                    }
                    String output;
                    serializeJson(listResp, output);
                    webSocket.sendTXT(num, output);
                }
                else if (event == "message" || event == "image" || event == "location") {
                    String sender = doc["data"]["sender"] | "";
                    String receiver = doc["data"]["receiver"] | "";
                    uint8_t targetNum;
                    bool isActive = isUserActive(receiver, targetNum);
                    
                    if (receiver == sender) {
                        String echoOutput;
                        serializeJson(doc, echoOutput);
                        webSocket.sendTXT(num, echoOutput);
                    } else if (isActive) {
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
                        sendLoRa("message", String(millis()), rawStr);
                    }
                }
                else if (event == "group_message") {
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
                    
                    bool routedLocal = false;
                    for (JsonObject g : groupsArr) {
                        if (g["id"] == groupId) {
                            JsonArray users = g["users"].as<JsonArray>();
                            for (String u : users) {
                                if (u != sender) {
                                    uint8_t targetNum;
                                    if (isUserActive(u, targetNum)) {
                                        String msgOutput;
                                        serializeJson(doc, msgOutput);
                                        webSocket.sendTXT(targetNum, msgOutput);
                                        routedLocal = true;
                                    }
                                }
                            }
                            break;
                        }
                    }
                    
                    String rawStr;
                    serializeJson(doc["data"], rawStr);
                    sendLoRa("group_message", String(millis()), rawStr);
                }
            }
            break;
        }
    } catch (...) {}
}

void checkNeighbors() {
    try {
        unsigned long now = millis();
        auto it = neighbors.begin();
        bool changed = false;
        while (it != neighbors.end()) {
            if (now - it->lastSeen > 30000) {
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
        DynamicJsonDocument doc(256);
        doc["type"] = "ping";
        doc["node"] = ssid;
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
        while (LoRa.available()) {
            packet += (char)LoRa.read();
        }
        
        DynamicJsonDocument doc(4096);
        DeserializationError err = deserializeJson(doc, packet);
        if (err) return;
        
        String type = doc["type"] | "";
        
        if (type == "ping") {
            String fromNode = doc["node"] | "";
            int currentRssi = LoRa.packetRssi();
            bool found = false;
            for (auto &n : neighbors) {
                if (n.nodeName == fromNode) {
                    n.rssi = currentRssi;
                    n.lastSeen = millis();
                    found = true;
                    break;
                }
            }
            if (!found) {
                Neighbor newN;
                newN.nodeName = fromNode;
                newN.rssi = currentRssi;
                newN.lastSeen = millis();
                neighbors.push_back(newN);
            }
            updateDisplay();
        } 
        else {
            String reqId = doc["req_id"] | "";
            String saltedData = doc["data"] | "";
            String rawData = removeSalt(saltedData);
            
            if (rawData.length() > 0) {
                DynamicJsonDocument innerDoc(4096);
                deserializeJson(innerDoc, rawData);
                
                if (type == "sync_user") {
                    saveUser(innerDoc["username"].as<String>(), innerDoc["password"].as<String>(), innerDoc["token"].as<String>());
                }
                else if (type == "sync_conn") {
                    writeFS("/connections.json", rawData);
                }
                else if (type == "sync_group") {
                    String groupId = innerDoc["id"].as<String>();
                    String groupName = innerDoc["name"].as<String>();
                    String creator = innerDoc["creator"].as<String>();
                    
                    String gData = readFS("/groups.json");
                    DynamicJsonDocument groupsDoc(4096);
                    deserializeJson(groupsDoc, gData);
                    JsonArray groupsArr = groupsDoc.as<JsonArray>();
                    if (groupsArr.isNull()) groupsArr = groupsDoc.to<JsonArray>();
                    
                    bool exists = false;
                    for (JsonObject g : groupsArr) {
                        if (g["id"] == groupId) { exists = true; break; }
                    }
                    if (!exists) {
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
                    }
                }
                else if (type == "message") {
                    String receiver = innerDoc["receiver"] | "";
                    uint8_t targetNum;
                    if (isUserActive(receiver, targetNum)) {
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
                }
                else if (type == "msg_delivered") {
                    String sender = innerDoc["receiver"] | ""; 
                    uint8_t targetNum;
                    if (isUserActive(sender, targetNum)) {
                        DynamicJsonDocument fwdDoc(256);
                        fwdDoc["event"] = "msg_delivered";
                        fwdDoc["data"] = innerDoc;
                        String fwdStr;
                        serializeJson(fwdDoc, fwdStr);
                        webSocket.sendTXT(targetNum, fwdStr);
                    }
                }
                else if (type == "group_message") {
                    String groupId = innerDoc["groupId"] | "";
                    String sender = innerDoc["sender"] | "";
                    
                    String gData = readFS("/groups.json");
                    DynamicJsonDocument groupsDoc(4096);
                    deserializeJson(groupsDoc, gData);
                    JsonArray groupsArr = groupsDoc.as<JsonArray>();
                    
                    for (JsonObject g : groupsArr) {
                        if (g["id"] == groupId) {
                            JsonArray users = g["users"].as<JsonArray>();
                            for (String u : users) {
                                if (u != sender) {
                                    uint8_t targetNum;
                                    if (isUserActive(u, targetNum)) {
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
                }
                else if (type == "app_version_req") {
                    if (sdAvailable && currentAppVersion > 0) {
                        DynamicJsonDocument resDoc(256);
                        resDoc["version"] = currentAppVersion;
                        String raw;
                        serializeJson(resDoc, raw);
                        sendLoRa("app_version_res", String(millis()), raw);
                    }
                }
                else if (type == "app_version_res") {
                    float remoteVer = innerDoc["version"].as<float>();
                    if (remoteVer > currentAppVersion) {
                        oledStatus = "New App Ver: " + String(remoteVer);
                        updateDisplay();
                    }
                }
                else if (type == "app_transfer_req") {
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
                                sendLoRa("app_transfer_res", reqId, outStr);
                            }
                        }
                    }
                }
                else if (type == "app_transfer_res") {
                    if (isDownloadingApp) {
                        int chunk = innerDoc["chunk"] | 0;
                        oledStatus = "APK DL Chunk: " + String(chunk);
                        updateDisplay();
                    }
                }
            }
        }
    } catch (...) {}
}

void setup() {
    try {
        SPIFFS.begin(true);
        Wire.begin(SDA_PIN, SCL_PIN);
        
        if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
            display.clearDisplay();
            display.display();
        }

        nodePassword = generatePassword(ssid);
        WiFi.softAP(ssid, nodePassword.c_str());

        sdSPI.begin(18, 19, 23, 5);
        if (SD.begin(5, sdSPI)) {
            sdAvailable = true;
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
        } else {
            oledStatus = "LORA INIT FAIL";
            updateDisplay();
            while (1);
        }

        webSocket.begin();
        webSocket.onEvent(webSocketEvent);

        if (sdAvailable) {
            webServer.on("/download", []() {
                File file = SD.open("/app/app-release.apk");
                if (file) {
                    webServer.streamFile(file, "application/vnd.android.package-archive");
                    file.close();
                } else {
                    webServer.send(404, "text/plain", "APK File Not Found on SD Card");
                }
            });
            webServer.begin();
        }
        
        oledStatus = "System Ready";
        updateDisplay();
        
        handleAppVersionCheck();
        
    } catch (...) {}
}

void loop() {
    try {
        webSocket.loop();
        if (sdAvailable) {
            webServer.handleClient();
        }
        
        int packetSize = LoRa.parsePacket();
        if (packetSize) {
            handleIncomingLoRaPacket(packetSize);
        }
        
        if (millis() - lastBeaconTime > 10000) {
            broadcastPing();
            lastBeaconTime = millis();
        }
        
        checkNeighbors();
        
        int currentClients = WiFi.softAPgetStationNum();
        if (currentClients != prevClientsCount) {
            prevClientsCount = currentClients;
            updateDisplay();
        }
    } catch (...) {}
}