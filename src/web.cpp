#include "web.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ETH.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Update.h>
#include <WebServer.h>
#include "WiFiClientSecure.h"
#include "FS.h"
#include "WiFi.h"
#include "config.h"
#include "etc.h"
#include "log.h"
#include "webh/PAGE_ABOUT.html.gz.h"
#include "webh/PAGE_ETHERNET.html.gz.h"
#include "webh/PAGE_GENERAL.html.gz.h"
#include "webh/PAGE_LOADER.html.gz.h"
#include "webh/PAGE_ROOT.html.gz.h"
#include "webh/PAGE_SECURITY.html.gz.h"
#include "webh/PAGE_SERIAL.html.gz.h"
#include "webh/PAGE_SYSTOOLS.html.gz.h"
#include "webh/PAGE_WIFI.html.gz.h"
#include "webh/bootstrap.min.js.gz.h"
#include "webh/favicon.ico.gz.h"
#include "webh/functions.js.gz.h"
#include "webh/jquery-min.js.gz.h"
#include "webh/masonry.js.gz.h"
#include "webh/required.css.gz.h"
#include "webh/PAGE_LOGOUT.html.gz.h"
#include "webh/icons.svg.gz.h"
#include "Ticker.h"

extern struct ConfigSettingsStruct ConfigSettings;
extern struct zbVerStruct zbVer;

bool wifiWebSetupInProgress = false;
extern const char *coordMode;
extern const char* configFileSystem;
extern const char* configFileWifi;
extern const char* configFileEther;
extern const char* configFileGeneral;
extern const char* configFileSecurity;
extern const char* configFileSerial;
extern const char* deviceModel;
const char *contTypeTextHtml = "text/html";
const char *contTypeTextJs = "text/javascript";
const char *contTypeTextCss = "text/css";
const char *checked = "true";
const char *respHeaderName = "respValuesArr";
const char *contTypeJson = "application/json";
const char *contTypeText = "text/plain";

enum API_PAGE_t : uint8_t { API_PAGE_ROOT,
                                API_PAGE_GENERAL,
                                API_PAGE_ETHERNET,
                                API_PAGE_WIFI,
                                API_PAGE_ZHA_Z2M,
                                API_PAGE_SECURITY,
                                API_PAGE_SYSTOOLS,
                                API_PAGE_ABOUT };

WebServer serverWeb(80);

HTTPClient clientWeb;
WiFiClient eventsClient;

void webServerHandleClient() {
    serverWeb.handleClient();
}

void initWebServer() {
    serverWeb.on("/js/bootstrap.min.js", []() { sendGzip(contTypeTextJs, bootstrap_min_js_gz, bootstrap_min_js_gz_len); });
    serverWeb.on("/js/masonry.min.js", []() { sendGzip(contTypeTextJs, masonry_js_gz, masonry_js_gz_len); });
    serverWeb.on("/js/functions.js", []() { sendGzip(contTypeTextJs, functions_js_gz, functions_js_gz_len); });
    serverWeb.on("/js/jquery-min.js", []() { sendGzip(contTypeTextJs, jquery_min_js_gz, jquery_min_js_gz_len); });
    serverWeb.on("/css/required.css", []() { sendGzip(contTypeTextJs, required_css_gz, required_css_gz_len); });
    serverWeb.on("/favicon.ico", []() { sendGzip("img/png", favicon_ico_gz, favicon_ico_gz_len); });
    serverWeb.on("/icons.svg", []() { sendGzip("image/svg+xml", icons_svg_gz, icons_svg_gz_len); });

    serverWeb.onNotFound([]() { serverWeb.send(HTTP_CODE_OK, contTypeText, F("URL NOT FOUND")); });  // handleNotFound);
    serverWeb.on("/", []() { sendGzip(contTypeTextHtml, PAGE_LOADER_html_gz, PAGE_LOADER_html_gz_len); });
    serverWeb.on("/general", []() { sendGzip(contTypeTextHtml, PAGE_LOADER_html_gz, PAGE_LOADER_html_gz_len); });
    serverWeb.on("/security", []() { sendGzip(contTypeTextHtml, PAGE_LOADER_html_gz, PAGE_LOADER_html_gz_len); });
    serverWeb.on("/wifi", []() { sendGzip(contTypeTextHtml, PAGE_LOADER_html_gz, PAGE_LOADER_html_gz_len); });
    serverWeb.on("/ethernet", []() { sendGzip(contTypeTextHtml, PAGE_LOADER_html_gz, PAGE_LOADER_html_gz_len); });
    serverWeb.on("/zha-z2m", []() { sendGzip(contTypeTextHtml, PAGE_LOADER_html_gz, PAGE_LOADER_html_gz_len); });
    serverWeb.on("/about", []() { sendGzip(contTypeTextHtml, PAGE_LOADER_html_gz, PAGE_LOADER_html_gz_len); });
    serverWeb.on("/sys-tools", []() { sendGzip(contTypeTextHtml, PAGE_LOADER_html_gz, PAGE_LOADER_html_gz_len); });
    serverWeb.on("/saveParams", HTTP_POST, handleSaveParams);
    serverWeb.on("/cmdZigRST", handleZigbeeRestart);
    serverWeb.on("/cmdZigBSL", handleZigbeeBSL);
    serverWeb.on("/saveFile", handleSavefile);
    serverWeb.on("/switch/firmware_update/toggle", handleZigbeeBSL);  // for cc-2538.py ESPHome edition back compatibility | will be disabled on 1.0.0
    serverWeb.on("/api", handleApi);
    serverWeb.on("/events", handleEvents);
    serverWeb.on("/metrics", handleMetrics);
    serverWeb.on("/logout", []() { 
        serverWeb.sendHeader(F("Content-Encoding"), F("gzip"));
        serverWeb.send_P(HTTP_CODE_UNAUTHORIZED, contTypeTextHtml, (const char *)PAGE_LOGOUT_html_gz, PAGE_LOGOUT_html_gz_len);
    });

    /*handling uploading firmware file */
    serverWeb.on(
        "/update", HTTP_POST, []() {
        serverWeb.sendHeader("Connection", "close");
        serverWeb.send(HTTP_CODE_OK, contTypeText, (Update.hasError()) ? "FAIL" : "OK");
        ESP.restart(); },
        []() {
                HTTPUpload &upload = serverWeb.upload();
                if (upload.status == UPLOAD_FILE_START) {
                    if(!checkAuth()) return;
                    Serial.printf("Update: %s\n", upload.filename.c_str());
                    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {  // start with max available size
                        Update.printError(Serial);
                    }
                } else if (upload.status == UPLOAD_FILE_WRITE) {
                    /* flashing firmware to ESP*/
                    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                        Update.printError(Serial);
                    }
                } else if (upload.status == UPLOAD_FILE_END) {
                    if (Update.end(true)) {  // true to set the size to the current progress
                        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
                        serverWeb.send(HTTP_CODE_OK, contTypeText, (Update.hasError()) ? "FAIL" : "OK");
                    } else {
                        Update.printError(Serial);
                    }
                }

        });
    serverWeb.begin();
    DEBUG_PRINTLN(F("webserver setup done"));
}

void handleMetrics(){
    const char* rn = "\n";
    const float CPUtemp = getCPUtemp();
    const char* prefix = "smlight_";
    char resp[400];
    sprintf(resp, "%ssocket_clients{} %d%s%ssocket_uptime{} %d%s%sdevice_uptime{} %d%s%sdevice_temp{} %s%s%sfree_heap{} %d", prefix, ConfigSettings.connectedClients, rn, prefix, ConfigSettings.socketTime, rn, prefix, millis(), rn, prefix, String(CPUtemp), rn, prefix, ESP.getFreeHeap() / 1024);//socketConnected=1\r\nsocketUptime=1111111111\r\ndeviceUptime=1111111111\r\ndeviceTemp=28.44\r\nfreeHeap=111
    serverWeb.send(HTTP_CODE_OK, contTypeText, resp);
}

void clearS2Buffer(){
  while (Serial2.available()){//clear buffer
    Serial2.read();
  }
}

void getZbVer(){
    zbVer.zbRev = 0;
    const byte cmdFrameStart = 0xFE;
    const byte zero = 0x00;
    const byte cmd1 = 0x21;
    const byte cmd2 = 0x02;
    const byte cmdSysVersion[] = {cmdFrameStart, zero, cmd1, cmd2, 0x23};
    for (uint8_t i = 0; i < 6; i++){
      if (Serial2.read() != cmdFrameStart || Serial2.read() != 0x0a || Serial2.read() != 0x61 || Serial2.read() != cmd2){//check for packet start
        clearS2Buffer();//skip
        Serial2.write(cmdSysVersion, sizeof(cmdSysVersion));
        Serial2.flush();
        delay(100);
      }else{
        const uint8_t zbVerLen = 11;
        byte zbVerBuf[zbVerLen];
        for (uint8_t i = 0; i < zbVerLen; i++){
          zbVerBuf[i] = Serial2.read();
        }
        zbVer.zbRev =  zbVerBuf[5] | (zbVerBuf[6] << 8) | (zbVerBuf[7] << 16) | (zbVerBuf[8] << 24);
        zbVer.maintrel = zbVerBuf[4];
        zbVer.minorrel = zbVerBuf[3];
        zbVer.majorrel = zbVerBuf[2];
        zbVer.product = zbVerBuf[1];
        zbVer.transportrev = zbVerBuf[0];
        printLogMsg(String("[ZBVER]") + " Rev: " + zbVer.zbRev + " Maintrel: " + zbVer.maintrel + " Minorrel: " + zbVer.minorrel + " Majorrel: " + zbVer.majorrel + " Transportrev: " + zbVer.transportrev + " Product: " + zbVer.product);
        clearS2Buffer();
        break;
      }
    }
}

void handleEvents(){
    eventsClient = serverWeb.client();
    if(eventsClient){// send events header
        eventsClient.println("HTTP/1.1 200 OK");
        eventsClient.println("Content-Type: text/event-stream;");
        eventsClient.println("Connection: close");
        eventsClient.println("Access-Control-Allow-Origin: *");
        eventsClient.println("Cache-Control: no-cache");
        eventsClient.println();
        eventsClient.flush();
    }
}

void sendEvent(const char* event, const uint8_t evsz, const String data){
    if(eventsClient){
        char evnmArr[10 + evsz];
        sprintf(evnmArr, "event: %s\n", event);
        eventsClient.print(evnmArr);
        eventsClient.print(String("data: ") + data + "\n\n");
        //eventsClient.println();
        eventsClient.flush();
    }
}

void sendGzip(const char *contentType, const uint8_t content[], uint16_t contentLen) {
    serverWeb.sendHeader(F("Content-Encoding"), F("gzip"));
    serverWeb.send_P(HTTP_CODE_OK, contentType, (const char *)content, contentLen);
}

void hex2bin(uint8_t *out, const char *in){
    //uint8_t sz = 0;
    while (*in) {
        while (*in == ' ') in++;  // skip spaces
        if (!*in) break;
        uint8_t c = *in>='a' ? *in-'a'+10 : *in>='A' ? *in-'A'+10 : *in-'0';
        in++;
        c <<= 4;
        if (!*in) break;
        c |= *in>='a' ? *in-'a'+10 : *in>='A' ? *in-'A'+10 : *in-'0';
        in++;
        *out++ = c;
        //sz++;
    }
}

bool sendCmdWait(const byte cmd[], const uint8_t cmdlen, const uint16_t maxdelay){
    unsigned long start = millis();
    Serial2.write(cmd, cmdlen);
    Serial2.flush();
    while (true){
       if (millis() - start >= maxdelay){
        return 0;
       }else if(Serial2.read() == 0x00 && Serial2.read() == 0xcc){
        return 1;
       }
       delay(20);
    }
}

void handleApi() {  // http://192.168.0.116/api?action=0&page=0
    enum API_ACTION_t : uint8_t { API_GET_PAGE,
                                  API_GET_PARAM,
                                  API_STARTWIFISCAN,
                                  API_WIFISCANSTATUS,
                                  API_GET_FILELIST,
                                  API_GET_FILE,
                                  API_SEND_HEX,
                                  API_WIFICONNECTSTAT,
                                  API_CMD,
                                  API_GET_LOG,
                                  API_FLASH_ZB };
    const char *action = "action";
    const char *page = "page";
    const char *Authentication = "Authentication";
    const char* param = "param";
    const char* wrongArgs = "wrong args";
    const char* ok = "ok";

    if(ConfigSettings.webAuth){
        if(!checkAuth()){//Authentication
            serverWeb.sendHeader(Authentication, "fail");
            serverWeb.send(HTTP_CODE_UNAUTHORIZED, contTypeText, F("wrong login or password"));
            return;
        }else{
            serverWeb.sendHeader(Authentication, ok);
        }
    }
    if (serverWeb.argName(0) != action) {
        DEBUG_PRINTLN(F("[handleApi] wrong arg 'action'"));
        DEBUG_PRINTLN(serverWeb.argName(0));
        serverWeb.send(HTTP_CODE_INTERNAL_SERVER_ERROR, contTypeText, wrongArgs);
    } else {
        const uint8_t action = serverWeb.arg(action).toInt();
        DEBUG_PRINTLN(F("[handleApi] arg 0 is:"));
        DEBUG_PRINTLN(action);
        switch (action) {
            case API_FLASH_ZB:{
                ConfigSettings.zbFlashing = 1;
                const char* fwurlArg = "fwurl";
                const uint8_t eventLen = 11;
                const char* tagZB_FW_info = "ZB_FW_info";
                const char* tagZB_FW_err = "ZB_FW_err";
                const char* tagZB_FW_progress = "ZB_FW_prgs";
                if(serverWeb.hasArg(fwurlArg)){
                    String fwUrl = serverWeb.arg(fwurlArg);
                    serverWeb.send(HTTP_CODE_OK, contTypeText, ok);
                    uint8_t evWaitCount = 0;
                    while (!eventsClient.connected() && evWaitCount < 200){//wait for events
                        webServerHandleClient();
                        delay(25);
                        evWaitCount++;
                    }
                    //sendEvent(tag, eventLen, String("FW Url: ") + fwUrl);
                    HTTPClient https;
                    WiFiClientSecure client;
                    client.setInsecure();
                    https.begin(client, fwUrl);//https://raw.githubusercontent.com/Tarik2142/devHost/main/coordinator_20211217.bin
                    https.addHeader("Content-Type", "application/octet-stream");
                    const int16_t httpsCode = https.GET();
                    //sendEvent(tag, eventLen, String("REQ result: ") + httpsCode);
                    if (httpsCode == HTTP_CODE_OK){
                        const uint32_t fwSize = https.getSize();
                        sendEvent(tagZB_FW_info, eventLen, "[start]");
                        sendEvent(tagZB_FW_info, eventLen, "Downloading firmware...");
                        const char* tempFile = "/fw.bin";
                        LittleFS.remove(tempFile);
                        File fwFile = LittleFS.open(tempFile, "w", 1);
                        uint8_t buff[4];
                        uint32_t downloaded = 0;
                        while(client.readBytes(buff, sizeof(buff))){
                            downloaded += fwFile.write(buff, sizeof(buff));
                            if(!(downloaded % 8192)){
                                const uint8_t d = ((float)downloaded / fwSize) * 100;
                                sendEvent(tagZB_FW_progress, eventLen, String(d));
                            }
                        }
                        fwFile.close();
                        //in development
                        
                    }else{
                        serverWeb.send(HTTP_CODE_BAD_REQUEST, contTypeText, String(httpsCode));
                        sendEvent(tagZB_FW_err, eventLen, "REQ error: http_code " + String(httpsCode));
                    }
                }else{
                    serverWeb.send(HTTP_CODE_BAD_REQUEST, contTypeText, "missing arg 1");
                }
                ConfigSettings.zbFlashing = 0;
            }
            break;
            case API_GET_LOG:{// todo: move to server events
                String result;
                result = logPrint();
                serverWeb.send(HTTP_CODE_OK, contTypeText, result);
            }
            break;
            case API_CMD:{
                enum CMD_t : uint8_t {//cmd list for buttons starts from 0
                    CMD_ZB_ROUTER_RECON,
                    CMD_ZB_RST,
                    CMD_ZB_BSL,
                    CMD_ESP_RES,
                    CMD_ADAP_LAN,
                    CMD_ADAP_USB,
                    CMD_LEDY_TOG,
                    CMD_LEDB_TOG,
                    CMD_CLEAR_LOG
                };
                String result = wrongArgs;
                const char* argCmd = "cmd";
                if(serverWeb.hasArg(argCmd)){
                    result = "ok";
                    switch (serverWeb.arg(argCmd).toInt()){
                        case CMD_CLEAR_LOG:
                            logClear();
                        break;
                        case CMD_ZB_ROUTER_RECON:
                            digitalWrite(CC2652P_FLSH, 0);
                            delay(250);
                            digitalWrite(CC2652P_FLSH, 1);
                            printLogMsg("Router reconnect");              
                        break;
                        case CMD_ZB_RST:
                            zigbeeRestart();
                        break;
                        case CMD_ZB_BSL:
                            zigbeeEnableBSL();
                        break;
                        case CMD_ESP_RES:
                            serverWeb.send(HTTP_CODE_OK, contTypeText, result);
                            ESP.restart();
                        break;
                        case CMD_ADAP_LAN:
                            adapterModeLAN();
                        break;
                        case CMD_ADAP_USB:
                            adapterModeUSB();
                        break;
                        case CMD_LEDY_TOG:
                            ledYellowToggle();
                        break;
                        case CMD_LEDB_TOG:
                            ledBlueToggle();
                        break;
                    
                    default:
                        break;
                    }
                    serverWeb.send(HTTP_CODE_OK, contTypeText, result);
                }
            }
            break;
            case API_WIFICONNECTSTAT:{
                String result;
                StaticJsonDocument<70> doc;
                const char* connected = "connected";
                if(WiFi.status() == WL_CONNECTED){
                    doc[connected] = true;
                    doc["ip"] = WiFi.localIP().toString();
                }else{
                    doc[connected] = false;
                }
                serializeJson(doc, result);
                serverWeb.send(HTTP_CODE_OK, contTypeJson, result);
            }
            break;
            case API_SEND_HEX:{
                String result = wrongArgs;
                const char* argSize = "size";
                const char* argHex = "hex";
                if(serverWeb.hasArg(argHex) && serverWeb.hasArg(argSize)){
                    result = ok;
                    uint8_t size = serverWeb.arg(argSize).toInt();
                    byte resp[size];
                    hex2bin(resp, serverWeb.arg(argHex).c_str());
                    Serial2.write(resp, size);
                }
                serverWeb.send(HTTP_CODE_OK, contTypeText, result);
            }
            break;
            case API_GET_FILE:{
                String result = wrongArgs;
                const char* argFilename = "filename";
                if(serverWeb.hasArg(argFilename)){
                    String filename = "/config/" + serverWeb.arg(argFilename);
                    File file = LittleFS.open(filename, "r");
                    if (!file) return;
                    result = "";              
                    while (file.available()) {
                        result += (char)file.read();
                    }
                    file.close();    
                }
                serverWeb.send(HTTP_CODE_OK, contTypeText, result);
            }
            break;
            case API_GET_PARAM:{
                    String resp = wrongArgs;
                    if(serverWeb.hasArg(param)){
                        if (serverWeb.arg(param) == "refreshLogs"){
                            resp = (String)ConfigSettings.refreshLogs;
                        }else if(serverWeb.arg(param) == "coordMode"){
                            if(wifiWebSetupInProgress){
                                resp = "1";
                            }else{
                                resp = (String)ConfigSettings.coordinator_mode;
                            }
                        }else if(serverWeb.arg(param) == "zbRev"){
                            resp = zbVer.zbRev > 0 ? (String)zbVer.zbRev : "Unknown";
                        }
                    }
                    serverWeb.send(HTTP_CODE_OK, contTypeText, resp);
                }
                break;
            case API_STARTWIFISCAN:
                    if (WiFi.getMode() == WIFI_OFF) {  // enable wifi for scan
                        WiFi.mode(WIFI_STA);
                    }
                    // } else if (WiFi.getMode() == WIFI_AP) {  // enable sta for scan
                    //     WiFi.mode(WIFI_AP_STA);
                    // }
                    WiFi.scanNetworks(true);
                    serverWeb.send(HTTP_CODE_OK, contTypeTextHtml, ok);
                break;
            case API_WIFISCANSTATUS: {
                static uint8_t timeout = 0;
                DynamicJsonDocument doc(1024);
                String result = "";
                int16_t scanRes = WiFi.scanComplete();
                const char* scanDone = "scanDone";
                doc[scanDone] = false;
                if (scanRes == -2) {
                    WiFi.scanNetworks(true);
                } else if (scanRes > 0) {
                    doc[scanDone] = true;
                    JsonArray wifi = doc.createNestedArray("wifi");
                    for (int i = 0; i < scanRes; ++i) {
                        JsonObject wifi_0 = wifi.createNestedObject();
                        wifi_0["ssid"] = WiFi.SSID(i);
                        wifi_0["rssi"] = WiFi.RSSI(i);
                        wifi_0["channel"] = WiFi.channel(i);
                        wifi_0["secure"] = WiFi.encryptionType(i);
                    }
                    WiFi.scanDelete();
                    //if (ConfigSettings.coordinator_mode == COORDINATOR_MODE_LAN) WiFi.mode(WIFI_OFF); no more wifi scan in lan mode
                }
                if (timeout < 10) {
                    timeout++;
                } else {
                    doc[scanDone] = true;
                    WiFi.scanDelete();
                    timeout = 0;
                    //if (ConfigSettings.coordinator_mode == COORDINATOR_MODE_LAN) WiFi.mode(WIFI_OFF);
                }

                serializeJson(doc, result);
                serverWeb.send(HTTP_CODE_OK, contTypeJson, result);
                break;
            }
            case API_GET_PAGE:
                if (!serverWeb.arg(page).length() > 0) {
                    DEBUG_PRINTLN(F("[handleApi] wrong arg 'page'"));
                    DEBUG_PRINTLN(serverWeb.argName(1));
                    serverWeb.send(HTTP_CODE_INTERNAL_SERVER_ERROR, contTypeText, wrongArgs);
                    return;
                }
                switch (serverWeb.arg(page).toInt()) {
                    case API_PAGE_ROOT:
                        handleRoot();
                        sendGzip(contTypeTextHtml, PAGE_ROOT_html_gz, PAGE_ROOT_html_gz_len);
                        break;
                    case API_PAGE_GENERAL:
                        handleGeneral();
                        sendGzip(contTypeTextHtml, PAGE_GENERAL_html_gz, PAGE_GENERAL_html_gz_len);
                        break;
                    case API_PAGE_ETHERNET:
                        handleEther();
                        sendGzip(contTypeTextHtml, PAGE_ETHERNET_html_gz, PAGE_ETHERNET_html_gz_len);
                        break;
                    case API_PAGE_WIFI:
                        handleWifi();
                        sendGzip(contTypeTextHtml, PAGE_WIFI_html_gz, PAGE_WIFI_html_gz_len);
                        break;
                    case API_PAGE_ZHA_Z2M:
                        handleSerial();
                        sendGzip(contTypeTextHtml, PAGE_SERIAL_html_gz, PAGE_SERIAL_html_gz_len);
                        break;
                    case API_PAGE_SECURITY:
                        handleSecurity();
                        sendGzip(contTypeTextHtml, PAGE_SECURITY_html_gz, PAGE_SECURITY_html_gz_len);
                        break;
                    case API_PAGE_SYSTOOLS:
                        handleSysTools();
                        sendGzip(contTypeTextHtml, PAGE_SYSTOOLS_html_gz, PAGE_SYSTOOLS_html_gz_len);
                        break;
                    case API_PAGE_ABOUT:
                        //handleAbout();
                        sendGzip(contTypeTextHtml, PAGE_ABOUT_html_gz, PAGE_ABOUT_html_gz_len);
                        break;

                    default:
                        break;
                }
                break;
            case API_GET_FILELIST:{
                String fileList = "";
                DynamicJsonDocument doc(512);
                JsonArray files = doc.createNestedArray("files");
                File root = LittleFS.open("/config");
                File file = root.openNextFile();
                while (file) {
                    JsonObject jsonfile = files.createNestedObject();
                    jsonfile["filename"] = String(file.name());
                    jsonfile["size"] = file.size();
                    file = root.openNextFile();
                }
                serializeJson(doc, fileList);
                serverWeb.send(HTTP_CODE_OK, contTypeJson, fileList);
                break;
            }

            default:
                DEBUG_PRINTLN(F("[handleApi] switch (action) err"));
                break;
        }
    }
}

void handleSaveParams(){
    String result;
    DynamicJsonDocument doc(512);
    const char* pageId = "pageId";
    const char* on = "on";
    File configFile;
    const uint8_t one = 1;
    const uint8_t zero = 0;
    if (serverWeb.hasArg(pageId)){
       switch (serverWeb.arg(pageId).toInt()){
            case API_PAGE_GENERAL:{
                configFile = LittleFS.open(configFileGeneral, FILE_READ);
                deserializeJson(doc, configFile);
                configFile.close();

                if (serverWeb.hasArg(coordMode)) {
                    const uint8_t mode = serverWeb.arg(coordMode).toInt();
                    if(mode <= 2 && mode >= zero){
                        //ConfigSettings.coordinator_mode = static_cast<COORDINATOR_MODE_t>(mode);
                        if(mode == 1) wifiWebSetupInProgress = true;
                        doc[coordMode] = static_cast<COORDINATOR_MODE_t>(mode);
                    }           
                }
                const char* keepWeb = "keepWeb";
                if (serverWeb.arg(keepWeb) == on) {
                    doc[keepWeb] = one;
                } else {
                    doc[keepWeb] = zero;
                }
                const char* disableLedYellow = "disableLedYellow";
                if (serverWeb.arg(disableLedYellow) == on) {
                    doc[disableLedYellow] = one;
                } else {
                    doc[disableLedYellow] = zero;
                }
                const char* disableLedBlue = "disableLedBlue";
                if (serverWeb.arg(disableLedBlue) == on) {
                    doc[disableLedBlue] = one;
                } else {
                    doc[disableLedBlue] = zero;
                }
                configFile = LittleFS.open(configFileGeneral, FILE_WRITE);
                serializeJson(doc, configFile);
                configFile.close();
            }
            break;
            case API_PAGE_ETHERNET:{
                configFile = LittleFS.open(configFileEther, FILE_READ);
                deserializeJson(doc, configFile);
                configFile.close();
                doc["ip"] = serverWeb.arg("ipAddress");
                doc["mask"] = serverWeb.arg("ipMask");
                doc["gw"] = serverWeb.arg("ipGW");
                const char* dhcp = "dhcp";
                if (serverWeb.arg(dhcp) == "on") {
                    doc[dhcp] = one;
                } else {
                    doc[dhcp] = zero;
                }
                configFile = LittleFS.open(configFileEther, FILE_WRITE);
                serializeJson(doc, configFile);
                configFile.close();
            }
            break;
            case API_PAGE_WIFI:{
                configFile = LittleFS.open(configFileWifi, FILE_READ);
                deserializeJson(doc, configFile);
                configFile.close();
                doc["ssid"] = serverWeb.arg("WIFISSID");
                doc["pass"] = serverWeb.arg("WIFIpassword");
                const char* dhcpWiFi = "dhcpWiFi";
                if (serverWeb.arg(dhcpWiFi) == on) {
                    doc[dhcpWiFi] = 1;
                } else {
                    doc[dhcpWiFi] = 0;
                }
                doc["ip"] = serverWeb.arg("ipAddress");
                doc["mask"] = serverWeb.arg("ipMask");
                doc["gw"] = serverWeb.arg("ipGW");
                configFile = LittleFS.open(configFileWifi, FILE_WRITE);
                serializeJson(doc, configFile);
                configFile.close();
                WiFi.persistent(false);
                if (ConfigSettings.apStarted){
                    WiFi.mode(WIFI_AP_STA);
                }else{
                    WiFi.mode(WIFI_STA);
                }
                WiFi.begin(serverWeb.arg("WIFISSID").c_str(), serverWeb.arg("WIFIpassword").c_str());
            }
            break;
            case API_PAGE_ZHA_Z2M:{
                configFile = LittleFS.open(configFileSerial, FILE_READ);
                deserializeJson(doc, configFile);
                configFile.close();
                const char* baud = "baud";
                if (serverWeb.hasArg(baud)){
                    doc[baud] = serverWeb.arg(baud);
                }else{
                    doc[baud] = 115200;
                }
                const char* port = "port";
                if (serverWeb.hasArg(baud)){
                    doc[port] = serverWeb.arg(port);
                }else{
                    doc[port] = 6638;
                }
                configFile = LittleFS.open(configFileSerial, FILE_WRITE);
                serializeJson(doc, configFile);
                configFile.close();
            }
            break;
            case API_PAGE_SECURITY:{
                configFile = LittleFS.open(configFileSecurity, FILE_READ);
                deserializeJson(doc, configFile);
                configFile.close();
                const char* disableWeb = "disableWeb";
                if (serverWeb.arg(disableWeb) == on) {
                    doc[disableWeb] = 1;
                } else {
                    doc[disableWeb] = 0;
                }
                const char* webAuth = "webAuth";
                if (serverWeb.arg(webAuth) == on) {
                    doc[webAuth] = 1;
                } else {
                    doc[webAuth] = 0;
                }
                const char* webUser = "webUser";
                if (serverWeb.arg(webUser) != "") {
                    doc[webUser] = serverWeb.arg(webUser);
                } else {
                    doc[webUser] = "admin";
                }
                const char* fwEnabled = "fwEnabled";
                if (serverWeb.arg(fwEnabled) == on) {
                    doc[fwEnabled] = 1;
                } else {
                    doc[fwEnabled] = 0;
                }
                const char* fwIp = "fwIp";
                doc[fwIp] = serverWeb.arg(fwIp);
                doc["webPass"] = serverWeb.arg("webPass");

                configFile = LittleFS.open(configFileSecurity, FILE_WRITE);
                serializeJson(doc, configFile);
                configFile.close();
            }
            break;
            case API_PAGE_SYSTOOLS:{
                const char *refreshLogs = "refreshLogs";
                const char *hostname = "hostname";
                configFile = LittleFS.open(configFileGeneral, FILE_READ);
                deserializeJson(doc, configFile);
                configFile.close();
                if (serverWeb.hasArg(refreshLogs)){
                    ConfigSettings.refreshLogs = serverWeb.arg(refreshLogs).toInt();
                    doc[refreshLogs] = ConfigSettings.refreshLogs;
                }
                if (serverWeb.hasArg(hostname)){
                    doc[hostname] = serverWeb.arg(hostname);
                    strlcpy(ConfigSettings.hostname, serverWeb.arg(hostname).c_str(), sizeof(ConfigSettings.hostname));
                }
                configFile = LittleFS.open(configFileGeneral, FILE_WRITE);
                serializeJson(doc, configFile);
                configFile.close();
            }
            break;
       
            default:
            break;
       }
       serverWeb.send(HTTP_CODE_OK, contTypeText, "ok");
    }else{
        serverWeb.send(HTTP_CODE_INTERNAL_SERVER_ERROR, contTypeText, "bad args");
    }
}

bool checkAuth() {
    if(!ConfigSettings.webAuth) return true;
    const char *www_realm = "Login Required";
    if (!serverWeb.authenticate(ConfigSettings.webUser, ConfigSettings.webPass)) {
        serverWeb.requestAuthentication(DIGEST_AUTH, www_realm, "Authentication failed");
        return false;
    } else {
        return true;
    }
}

void handleGeneral() {
        DynamicJsonDocument doc(1024);
        String result;

        //doc["pageName"] = "General";
        // DEBUG_PRINTLN(ConfigSettings.usbMode);
        switch (ConfigSettings.coordinator_mode) {
            case COORDINATOR_MODE_USB:
                doc["checkedUsbMode"] = checked;
                break;
            case COORDINATOR_MODE_WIFI:
                doc["checkedWifiMode"] = checked;
                break;
            case COORDINATOR_MODE_LAN:
                doc["checkedLanMode"] = checked;
                break;

            default:
                break;
        }
        // DEBUG_PRINTLN(ConfigSettings.disableLedYellow);
        if (ConfigSettings.keepWeb) {
            doc["keepWeb"] = checked;
        }
        if (ConfigSettings.disableLedYellow) {
            doc["checkedDisableLedYellow"] = checked;
        }
        // DEBUG_PRINTLN(ConfigSettings.disableLedBlue);
        if (ConfigSettings.disableLedBlue) {
            doc["checkedDisableLedBlue"] = checked;
        }
        serializeJson(doc, result);
        serverWeb.sendHeader(respHeaderName, result);
}
void handleSecurity() {
        String result;
        DynamicJsonDocument doc(1024);

        if (ConfigSettings.disableWeb) {
            doc["disableWeb"] = checked;
        }

        if (ConfigSettings.webAuth) {
            doc["webAuth"] = checked;
        }
        doc["webUser"] = (String)ConfigSettings.webUser;
        doc["webPass"] = (String)ConfigSettings.webPass;
        if (ConfigSettings.fwEnabled) {
            doc["fwEnabled"] = checked;
        }
        doc["fwIp"] = ConfigSettings.fwIp.toString();

        serializeJson(doc, result);
        serverWeb.sendHeader(respHeaderName, result);
}

void handleWifi() {
        String result;
        DynamicJsonDocument doc(1024);

        //doc["pageName"] = "Config WiFi";
        doc["ssid"] = String(ConfigSettings.ssid);
        doc["passWifi"] = String(ConfigSettings.password);
        if (ConfigSettings.dhcpWiFi) {
            doc["dchp"] = checked;
        }
        doc["ip"] = ConfigSettings.ipAddressWiFi;
        doc["mask"] = ConfigSettings.ipMaskWiFi;
        doc["gw"] = ConfigSettings.ipGWWiFi;

        serializeJson(doc, result);
        serverWeb.sendHeader(respHeaderName, result);
}

void handleSerial() {
        String result;
        DynamicJsonDocument doc(1024);

        if (ConfigSettings.serialSpeed == 9600) {
            doc["9600"] = checked;
        } else if (ConfigSettings.serialSpeed == 19200) {
            doc["19200"] = checked;
        } else if (ConfigSettings.serialSpeed == 38400) {
            doc["8400"] = checked;
        } else if (ConfigSettings.serialSpeed == 57600) {
            doc["57600"] = checked;
        } else if (ConfigSettings.serialSpeed == 115200) {
            doc["115200"] = checked;
        } else {
            doc["115200"] = checked;
        }
        doc["socketPort"] = String(ConfigSettings.socketPort);

        serializeJson(doc, result);
        serverWeb.sendHeader(respHeaderName, result);
}

void handleEther() {
        String result;
        DynamicJsonDocument doc(1024);

        //doc["pageName"] = "Config Ethernet";

        if (ConfigSettings.dhcp) {
            doc["modeEther"] = checked;
        }
        doc["ipEther"] = ConfigSettings.ipAddress;
        doc["maskEther"] = ConfigSettings.ipMask;
        doc["GWEther"] = ConfigSettings.ipGW;

        // if (ConfigSettings.disablePingCtrl) {
        //     doc["disablePingCtrl"] = checked;
        // }

        serializeJson(doc, result);
        serverWeb.sendHeader(respHeaderName, result);
}

void handleRoot() {
        String result;
        DynamicJsonDocument doc(1024);

        char verArr[25];
        sprintf(verArr, "%s (%s)", VERSION, BUILD_TIMESTAMP);
        doc["VERSION"] = verArr;

        String readableTime;
        getReadableTime(readableTime, ConfigSettings.socketTime);
        const char* connectedSocketStatus = "connectedSocketStatus";
        const char* connectedSocket = "connectedSocket";
        const char* notConnected = "Not connected";
        const char* yes = "Yes";
        const char* no = "No";
        const char* on = "On";
        const char* off = "Off";

        if (ConfigSettings.connectedClients > 0) {
            char str[32];
            const uint8_t n = ConfigSettings.connectedClients;
            snprintf(str, sizeof(str), "%s, %d connection%s", yes, n, (n > 1 ? "s" : ""));
            doc[connectedSocketStatus] = str;
            doc[connectedSocket] = readableTime;
        } else {
            doc[connectedSocketStatus] = no;
            doc[connectedSocket] = notConnected;
        }

        const char* operationalMode = "operationalMode";
        switch (ConfigSettings.coordinator_mode) {
            case COORDINATOR_MODE_USB:
                doc[operationalMode] = "Zigbee-to-USB";
                break;
            case COORDINATOR_MODE_WIFI:
                doc[operationalMode] = "Zigbee-to-WiFi";
                break;
            case COORDINATOR_MODE_LAN:
                doc[operationalMode] = "Zigbee-to-Ethernet";
                break;

            default:
                break;
        }

        // ETHERNET TAB
        const char* ethDhcp = "ethDhcp";
        if (ConfigSettings.dhcp) {
            doc[ethDhcp] = on;
        } else {
            doc[ethDhcp] = off;
        }

        const char* connectedEther = "connectedEther";
        const char* ethConnection = "ethConnection";
        const char* ethMac = "ethMac";
        const char* ethSpd = "ethSpd";
        const char* ethIp = "ethIp";
        const char* etchMask = "etchMask";
        const char* ethGate = "ethGate";
        if (ConfigSettings.connectedEther) {
            doc[connectedEther] = yes;
            doc[ethConnection] = "Connected";
            doc[ethMac] = ETH.macAddress();
            doc[ethSpd] = String(ETH.linkSpeed()) + String(" Mbps");
            doc[ethIp] = ETH.localIP().toString();
            doc[etchMask] = ETH.subnetMask().toString();
            doc[ethGate] = ETH.gatewayIP().toString();
        } else {
            doc[connectedEther] = no;
            doc[ethConnection] = notConnected;
            doc[ethMac] = notConnected;
            doc[ethSpd] = notConnected;
            doc[ethIp] = notConnected;
            doc[etchMask] = notConnected;
            doc[ethGate] = notConnected;
        }

        getReadableTime(readableTime, 0);
        doc["uptime"] = readableTime;

        const float CPUtemp = getCPUtemp();
        doc["deviceTemp"] = String(CPUtemp);
        doc["hwRev"] = deviceModel;
        doc["espModel"] = String(ESP.getChipModel());
        doc["espCores"] = String(ESP.getChipCores());
        doc["espFreq"] = String(ESP.getCpuFreqMHz());
        doc["espHeapFree"] = String(ESP.getFreeHeap() / 1024);
        doc["espHeapSize"] = String(ESP.getHeapSize() / 1024);

        esp_chip_info_t chip_info;
        esp_chip_info(&chip_info);

        const char* espFlashType = "espFlashType";
        if (chip_info.features & CHIP_FEATURE_EMB_FLASH) {
            doc[espFlashType] = "embedded";
        } else {
            doc[espFlashType] = "external";
        }

        doc["espFlashSize"] = String(ESP.getFlashChipSize() / (1024 * 1024));
        //wifi
        const char* wifiSsid = "wifiSsid";
        const char* wifiRssi = "wifiRssi";
        const char* wifiIp = "wifiIp";
        const char* wifiConnected = "wifiConnected";
        const char* wifiSubnet = "wifiSubnet";
        const char* wifiGate = "wifiGate";
        const char* wifiEnabled = "wifiEnabled";
        const char* wifiMode = "wifiMode";
        const char* wifiModeAPStatus = "wifiModeAPStatus";
        const char* wifiModeAP = "wifiModeAP";
        const char* wifiDhcp = "wifiDhcp";
        doc["wifiMac"] = String(WiFi.macAddress().c_str());
        if (ConfigSettings.coordinator_mode == COORDINATOR_MODE_WIFI) {
            doc[wifiEnabled] = yes;
            doc[wifiMode] = "Client";
            if (WiFi.status() == WL_CONNECTED) {  // STA connected
                String rssiWifi = String(WiFi.RSSI()) + String(" dBm");
                doc[wifiSsid] = WiFi.SSID();
                doc[wifiRssi] = rssiWifi;
                doc[wifiConnected] = "Connected to " + WiFi.SSID();
                doc[wifiIp] = WiFi.localIP().toString();
                doc[wifiSubnet] = WiFi.subnetMask().toString();
                doc[wifiGate] = WiFi.gatewayIP().toString();
            } else {
                const char *connecting = "Connecting...";
                doc["wifiSsid"] = ConfigSettings.ssid;      
                doc[wifiRssi] = connecting;
                doc[wifiConnected] = connecting;
                doc[wifiIp] = ConfigSettings.dhcpWiFi ? WiFi.localIP().toString() : connecting;
                doc[wifiSubnet] = ConfigSettings.dhcpWiFi ? WiFi.subnetMask().toString() : connecting;
                doc[wifiGate] = ConfigSettings.dhcpWiFi ? WiFi.gatewayIP().toString() : connecting;
            }
            if (ConfigSettings.dhcpWiFi) {
                doc[wifiDhcp] = on;
            } else {
                doc[wifiDhcp] = off;
            }
        } else {
            doc[wifiEnabled] = no;
            doc[wifiConnected] = notConnected;
            doc[wifiMode] = off;
            doc[wifiSsid] = off;
            doc[wifiIp] = off;
            doc[wifiSubnet] = off;
            doc[wifiGate] = off;
            doc[wifiRssi] = off;
            doc[wifiDhcp] = off;
        }
        if (ConfigSettings.apStarted) {  // AP active
            String AP_NameString;
            char apSsid[18];
            getDeviceID(apSsid);
            char wifiSsidBuf[35];
            sprintf(wifiSsidBuf, "%s (no password)", apSsid);
            doc[wifiSsid] = wifiSsidBuf;
            doc[wifiIp] = "192.168.1.1 (SLZB-06 web interface)";
            doc[wifiSubnet] = "255.255.255.0 (Access point)";
            doc[wifiGate] = "192.168.1.1 (this device)";
            doc[wifiDhcp] = "On (Access point)";
            doc[wifiModeAPStatus] = "AP started";
            doc[wifiMode] = "AP";
            doc[wifiModeAP] = yes;
            doc[wifiRssi] = "N/A";
        } else {
            doc[wifiModeAP] = no;
            doc[wifiModeAPStatus] = "Not started";
            //doc[wifiMode] = "Client";
        }

        if (zbVer.zbRev > 0) doc["zbRev"] = zbVer.zbRev;

        serializeJson(doc, result);
        serverWeb.sendHeader(respHeaderName, result);
}

void handleSysTools() {
        String result;
        DynamicJsonDocument doc(512);
        //doc["pageName"] = "System and Tools";
        doc["hostname"] = ConfigSettings.hostname;
        doc["refreshLogs"] = ConfigSettings.refreshLogs;
        serializeJson(doc, result);
        serverWeb.sendHeader(respHeaderName, result);
}

void handleSavefile() {
    if (checkAuth()) {
        if (serverWeb.method() != HTTP_POST) {
            serverWeb.send(HTTP_CODE_METHOD_NOT_ALLOWED, contTypeText, F("Method Not Allowed"));
        } else {
            String filename = "/config/" + serverWeb.arg(0);
            String content = serverWeb.arg(1);
            File file = LittleFS.open(filename, "w");
            if (!file) {
                DEBUG_PRINT(F("Failed to open file for reading\r\n"));
                return;
            }

            int bytesWritten = file.print(content);

            if (bytesWritten > 0) {
                DEBUG_PRINTLN(F("File was written"));
                DEBUG_PRINTLN(bytesWritten);
            } else {
                DEBUG_PRINTLN(F("File write failed"));
            }

            file.close();
            serverWeb.sendHeader(F("Location"), F("/sys-tools"));
            serverWeb.send(HTTP_CODE_SEE_OTHER);
        }
    }
}

void handleZigbeeBSL() {//todo move to api
    if (checkAuth()) {
        zigbeeEnableBSL();
        serverWeb.send(HTTP_CODE_OK, contTypeText, "");
    }
}

void handleZigbeeRestart(){
    if (checkAuth()) {
        zigbeeRestart();
        serverWeb.send(HTTP_CODE_OK, contTypeText, "");
    }
}

void printLogTime() {
    String tmpTime;
    unsigned long timeLog = millis();
    tmpTime = String(timeLog, DEC);
    logPush('[');
    for (int j = 0; j < tmpTime.length(); j++) {
        logPush(tmpTime[j]);
    }
    logPush(']');
}

void printLogMsg(String msg) {
    printLogTime();
    logPush(' ');
    logPush('|');
    logPush(' ');
    for (int j = 0; j < msg.length(); j++) {
        logPush(msg[j]);
    }
    logPush('\n');
    webServerHandleClient();
}

// int totalLength;        // total size of firmware
// int currentLength = 0;  // current size of written firmware

// void progressFunc(unsigned int progress, unsigned int total) {
//     Serial.printf("Progress: %u of %u\r", progress, total);
// };

// void checkUpdateFirmware() {//todo del
//     clientWeb.begin(UPD_FILE);
//     clientWeb.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
//     // Get file, just to check if each reachable
//     int resp = clientWeb.GET();
//     Serial.print("Response: ");
//     Serial.println(resp);
//     // If file is reachable, start downloading
//     if (resp == HTTP_CODE_OK) {
//         // get length of document (is -1 when Server sends no Content-Length header)
//         totalLength = clientWeb.getSize();
//         // transfer to local variable
//         int len = totalLength;
//         // this is required to start firmware update process
//         Update.begin(UPDATE_SIZE_UNKNOWN);
//         Update.onProgress(progressFunc);
//         DEBUG_PRINT("FW Size: ");

//         DEBUG_PRINTLN(totalLength);
//         // create buffer for read
//         uint8_t buff[128] = {0};
//         // get tcp stream
//         WiFiClient *stream = clientWeb.getStreamPtr();
//         // read all data from server
//         DEBUG_PRINTLN("Updating firmware...");
//         while (clientWeb.connected() && (len > 0 || len == -1)) {
//             // get available data size
//             size_t size = stream->available();
//             if (size) {
//                 // read up to 128 byte
//                 int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
//                 // pass to function
//                 runUpdateFirmware(buff, c);
//                 if (len > 0) {
//                     len -= c;
//                 }
//             }
//             // DEBUG_PRINT("Bytes left to flash ");
//             // DEBUG_PRINTLN(len);
//             // delay(1);
//         }
//     } else {
//         Serial.println("Cannot download firmware file. Only HTTP response 200: OK is supported. Double check firmware location #defined in UPD_FILE.");
//     }
//     clientWeb.end();
// }

// void runUpdateFirmware(uint8_t *data, size_t len) {//todo del
//     Update.write(data, len);
//     currentLength += len;
//     // Print dots while waiting for update to finish
//     Serial.print('.');
//     // if current length of written firmware is not equal to total firmware size, repeat
//     if (currentLength != totalLength) return;
//     Update.end(true);
//     Serial.printf("\nUpdate Success, Total Size: %u\nRebooting...\n", currentLength);
//     // Restart ESP32 to see changes
//     ESP.restart();
// }
