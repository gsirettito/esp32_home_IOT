#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"
#include <ArduinoJson.h>
#include <Regexp.h>
#include <DNSServer.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ESP32Ping.h>

const char *remote_host = "www.google.com";

// Telegram BOT Token (Get from Botfather)
String telegram_bot_token; //"XXXXXXXXX:XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"

const unsigned long BOT_MTBS = 1000; // mean time between scan messages

WiFiClientSecure secured_client;
UniversalTelegramBot bot(telegram_bot_token, secured_client);
unsigned long bot_lasttime; // last time messages' scan has been done

// Search for parameter in HTTP POST request
const char *PARAM_INPUT_1 = "ssid";
const char *PARAM_INPUT_2 = "pass";
const char *PARAM_INPUT_3 = "ip";
const char *PARAM_INPUT_4 = "gateway";
const char *PARAM_INPUT_5 = "netmask";
const char *PARAM_INPUT_6 = "bot_token";

// Variables to save values from HTML form
String wificonfig;
String ssid;
String pass;
String ip;
String netmask;
String gw;

// File paths to save input values permanently
const char *wificonfigPath = "/wificonfig.json";
const char *telegramBotTokenPath = "/token";

const char *softAP_ssid = "ESP32-WIFI-MANAGER";

WiFiEventId_t eventID;

IPAddress localIP;
// IPAddress localIP(192, 168, 1, 200); // hardcoded

// Set your Gateway IP address
IPAddress localGateway;
// IPAddress localGateway(192, 168, 1, 1); //hardcoded
IPAddress subnet(255, 255, 255, 0);

// The access points IP address and net mask
// It uses the default Google DNS IP address 8.8.8.8 to capture all
// Android dns requests
IPAddress apIP(8, 8, 8, 8);
IPAddress netMsk(255, 255, 255, 0);

// DNS server
const byte DNS_PORT = 53;
DNSServer dnsServer;

// Timer variables
unsigned long previousMillis = 0;
const long interval = 10000; // interval to wait for Wi-Fi connection (milliseconds)

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);

// Create an Event Source on /events
AsyncEventSource events("/events");

// Json Variable to manage wifi config
DynamicJsonDocument json(200);

// Timer variables
unsigned long lastTime = 0;
unsigned long timerDelay = 10000;

// Set LED GPIO
const int ledPin = 2;
// Stores LED state

String ledState;

// Initialize SPIFFS
void initSPIFFS()
{
    if (!SPIFFS.begin(true))
    {
        Serial.println("An error has occurred while mounting SPIFFS");
    }
    else
        Serial.println("SPIFFS mounted successfully");
}

// Read File from SPIFFS
String readFile(fs::FS &fs, const char *path)
{
    Serial.printf("Reading file: %s\r\n", path);

    File file = fs.open(path);
    if (!file || file.isDirectory())
    {
        Serial.println("- failed to open file for reading");
        return String();
    }

    String fileContent;
    while (file.available())
    {
        fileContent = file.readStringUntil('\n');
        break;
    }
    return fileContent;
}

// Write file to SPIFFS
void writeFile(fs::FS &fs, const char *path, const char *message)
{
    Serial.printf("Writing file: %s\r\n", path);

    File file = fs.open(path, FILE_WRITE);
    if (!file)
    {
        Serial.println("- failed to open file for writing");
        return;
    }
    if (file.print(message))
    {
        Serial.println("- file written");
    }
    else
    {
        Serial.println("- frite failed");
    }
}

// Initialize WiFi
bool initWiFi()
{
    if (ssid == "" || ip == "")
    {
        Serial.println("Undefined SSID or IP address.");
        return false;
    }

    WiFi.mode(WIFI_STA);
    localIP.fromString(ip.c_str());
    localGateway.fromString(gw.c_str());
    subnet.fromString(netmask.c_str());
    if (!WiFi.config(localIP, localGateway, subnet))
    {
        Serial.println("STA Failed to configure");
        return false;
    }

    WiFi.onEvent(WiFiStationConnected, ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
                 {
        Serial.println("WiFi connected");
        Serial.println("IP address: ");
        Serial.println(IPAddress(info.got_ip.ip_info.ip.addr)); },
                 WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    eventID = WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
                           {
        Serial.print("WiFi lost connection. Reason: ");
        Serial.println(info.wifi_sta_disconnected.reason); },
                           WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.print("Connecting to WiFi...");

    unsigned long currentMillis = millis();
    previousMillis = currentMillis;

    while (WiFi.status() != WL_CONNECTED)
    {
        currentMillis = millis();
        if (currentMillis - previousMillis >= interval)
        {
            Serial.println("Failed to connect.");
            return false;
        }
    }
    Serial.println(WiFi.localIP());
    return true;
}

// check if this string is an IP address
boolean isIp(String str)
{
    for (size_t i = 0; i < str.length(); i++)
    {
        int c = str.charAt(i);
        if (c != '.' && (c < '0' || c > '9'))
        {
            return false;
        }
    }
    return true;
}

String toStringIp(IPAddress ip)
{
    String res = "";
    for (int i = 0; i < 3; i++)
    {
        res += String((ip >> (8 * i)) & 0xFF) + ".";
    }
    res += String(((ip >> 8 * 3)) & 0xFF);
    return res;
}

// checks if the request is for the controllers IP, if not we redirect automatically to the
// captive portal
boolean captivePortal(AsyncWebServerRequest *request)
{
    if (!isIp(request->host()))
    {
        Serial.println("Request redirected to captive portal");
        AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "");
        response->addHeader("Location", String("http://") + toStringIp(request->client()->localIP()));
        request->send(response);
        request->client()->stop();
        return true;
    }
    return false;
}

void handleRoot(AsyncWebServerRequest *request)
{
    if (captivePortal(request))
    {
        return;
    }
    AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/wifimanager.html", "text/html");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "-1");
    request->send(response);
}

void handleNotFound(AsyncWebServerRequest *request)
{
    if (captivePortal(request))
    {
        return;
    }
    String message = F("File Not Found\n\n");
    message += F("URI: ");
    message += request->url();
    message += F("\nMethod: ");
    message += (request->method() == HTTP_GET) ? "GET" : "POST";
    message += F("\nArguments: ");
    message += request->args();
    message += F("\n");

    for (uint8_t i = 0; i < request->args(); i++)
    {
        message += String(F(" ")) + request->argName(i) + F(": ") + request->arg(i) + F("\n");
    }

    AsyncWebServerResponse *response = request->beginResponse(404, "text/plain", message);
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "-1");
    request->send(response);
}

// wifi OnConnected event
void WiFiStationConnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    Serial.println("Connected to AP!");

    Serial.print("SSID Length: ");
    Serial.println(info.wifi_sta_connected.ssid_len);

    Serial.print("SSID: ");
    for (int i = 0; i < info.wifi_sta_connected.ssid_len; i++)
    {
        Serial.print((char)info.wifi_sta_connected.ssid[i]);
    }

    Serial.print("\nBSSID: ");
    for (int i = 0; i < 6; i++)
    {
        Serial.printf("%02X", info.wifi_sta_connected.bssid[i]);

        if (i < 5)
        {
            Serial.print(":");
        }
    }

    Serial.print("\nChannel: ");
    Serial.println(info.wifi_sta_connected.channel);

    Serial.print("Auth mode: ");
    Serial.println(info.wifi_sta_connected.authmode);
}

// Replaces placeholder with LED state value
String processor(const String &var)
{
    if (var == "STATE")
    {
        if (digitalRead(ledPin))
        {
            ledState = "ON";
        }
        else
        {
            ledState = "OFF";
        }
        return ledState;
    }
    return String();
}

// Telegram bot handle messages
void handleNewMessages(int numNewMessages)
{
    Serial.print("handleNewMessages ");
    Serial.println(numNewMessages);

    String answer;
    for (int i = 0; i < numNewMessages; i++)
    {
        telegramMessage &msg = bot.messages[i];
        Serial.println("Received " + msg.text);
        if (msg.text == "/help")
            answer = "So you need _help_, uh? me too! use /start or /status";
        else if (msg.text == "/start")
            answer = "Welcome my new friend! You are the first *" + msg.from_name + "* I've ever met";
        else if (msg.text == "/status")
            answer = "All is good here, thanks for asking!";
        else
            answer = "Say what?";

        bot.sendMessage(msg.chat_id, answer, "Markdown");
    }
}

void bot_setup()
{
    bot.updateToken(telegram_bot_token);
    const String commands = F("["
                              "{\"command\":\"help\",  \"description\":\"Get bot usage help\"},"
                              "{\"command\":\"start\", \"description\":\"Message sent when you open a chat with a bot\"},"
                              "{\"command\":\"status\",\"description\":\"Answer device current status\"}" // no comma on last command
                              "]");
    bot.setMyCommands(commands);
    // bot.sendMessage("25235518", "Hola amigo!", "Markdown");
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Welcome to home automation IOT");
    initSPIFFS();

    // Set GPIO 2 as an OUTPUT
    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, LOW);

    secured_client.setCACert(TELEGRAM_CERTIFICATE_ROOT); // Add root certificate for api.telegram.org

    // Load values saved in SPIFFS
    wificonfig = readFile(SPIFFS, wificonfigPath);
    telegram_bot_token = readFile(SPIFFS, telegramBotTokenPath);
    deserializeJson(json, wificonfig);
    ssid = (const char *)json["ssid"];
    pass = (const char *)json["pass"];
    ip = (const char *)json["ip"];
    netmask = (const char *)json["netmask"];
    gw = (const char *)json["gw"];
    Serial.println(ssid);
    Serial.println(pass);
    Serial.println(ip);
    Serial.println(netmask);
    Serial.println(gw);

    if (initWiFi())
    {
        // Web Server Root URL
        server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
                  { request->send(SPIFFS, "/index.html", "text/html"); });

        server.serveStatic("/", SPIFFS, "/");

        // Route to set GPIO state to HIGH
        server.on("/on", HTTP_GET, [](AsyncWebServerRequest *request)
                  {
      digitalWrite(ledPin, HIGH);
      request->send(SPIFFS, "/index.html", "text/html", false, processor); });

        // Route to set GPIO state to LOW
        server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request)
                  {
      digitalWrite(ledPin, LOW);
      request->send(SPIFFS, "/index.html", "text/html", false, processor); });
        server.begin();

        Serial.print("Retrieving time: ");
        configTime(0, 0,
                   "0.north-america.pool.ntp.org",
                   "1.north-america.pool.ntp.org",
                   "2.north-america.pool.ntp.org"); // get UTC time via NTP
        // time_t now = time(nullptr);
        // while (now < 24 * 3600)
        // {
        //     Serial.println(now);
        //     delay(100);
        //     now = time(nullptr);
        // }
        // Serial.println(now);

        bot_setup();

        Serial.print("Pinging host ");
        Serial.println(remote_host);

        if (Ping.ping(remote_host))
        {
            Serial.println("Success!!");
        }
        else
        {
            Serial.println("Error :(");
        }
    }
    else
    {
        WiFi.removeEvent(eventID);
        // Connect to Wi-Fi network with SSID and password
        Serial.println("Setting AP (Access Point)");
        // NULL sets an open Access Point
        WiFi.softAPConfig(apIP, apIP, netMsk);
        // its an open WLAN access point without a password parameter
        WiFi.softAP(softAP_ssid);
        delay(1000);

        IPAddress IP = WiFi.softAPIP();
        Serial.print("AP IP address: ");
        Serial.println(IP);

        /* Setup the DNS server redirecting all the domains to the apIP */
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        dnsServer.start(DNS_PORT, "*", apIP);

        // Web Server Root URL
        /* Setup the web server */
        server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
                  { handleRoot(request); });

        server.serveStatic("/", SPIFFS, "/");
        server.on("/", HTTP_POST, [](AsyncWebServerRequest *request)
                  {
      int params = request->params();
      for(int i = 0;i < params;i++){
        AsyncWebParameter* p = request->getParam(i);
        if(p -> isPost()){
          // HTTP POST ssid value
          if (p->name() == PARAM_INPUT_1) {
            ssid = p->value().c_str();
            Serial.print("SSID set to: ");
            Serial.println(ssid);
            // Write file to save value
            json["ssid"] = ssid.c_str();
            //writeFile(SPIFFS, ssidPath, ssid.c_str());
          }
          // HTTP POST pass value
          if (p->name() == PARAM_INPUT_2) {
            pass = p->value().c_str();
            Serial.print("Password set to: ");
            Serial.println(pass);
            // Write file to save value
            json["pass"] = pass.c_str();
            //writeFile(SPIFFS, passPath, pass.c_str());
          }
          // HTTP POST ip value
          if (p->name() == PARAM_INPUT_3) {
            ip = p->value().c_str();
            Serial.print("IP Address set to: ");
            Serial.println(ip);
            // Write file to save value
            json["ip"] = ip.c_str();
            //writeFile(SPIFFS, ipPath, ip.c_str());
          }
          // HTTP POST gateway value
          if (p->name() == PARAM_INPUT_4) {
            gw = p->value().c_str();
            Serial.print("Gateway set to: ");
            Serial.println(gw);
            // Write file to save value
            json["gw"] = gw.c_str();
            //writeFile(SPIFFS, gatewayPath, gateway.c_str());
          }
          // HTTP POST netmask value
          if (p->name() == PARAM_INPUT_5) {
            netmask = p->value().c_str();
            Serial.print("Netmask set to: ");
            Serial.println(netmask);
            // Write file to save value
            json["netmask"] = netmask.c_str();
            //writeFile(SPIFFS, gatewayPath, gateway.c_str());
          }
          // HTTP POST token value
          if (p->name() == PARAM_INPUT_6) {
            telegram_bot_token = p->value().c_str();
            Serial.print("Telegram bot Token set to: ");
            Serial.println(telegram_bot_token);
            // Write file to save value
            writeFile(SPIFFS, telegramBotTokenPath, telegram_bot_token.c_str());
          }
          // save json to flash file
          String out;
          serializeJson(json, out);
          writeFile(SPIFFS, wificonfigPath, out.c_str());
          //Serial.printf("POST[%s]: %s\n", p->name().c_str(), p->value().c_str());
        }
      }
      request->send(200, "text/plain", "Done. ESP will restart, connect to your router and go to IP address: " + ip);
      delay(3000);
      ESP.restart(); });

        server.on("/generate_204", handleRoot);
        server.onNotFound(handleNotFound);
        server.begin();
    }
}

void loop()
{
    if (Serial.available())
    {
        String read = Serial.readString();
        read.trim();
        MatchState ms;
        ms.Target((char *)read.c_str());
        if (ms.Match("set ssid="))
        {
            ssid = read.substring(ms.MatchLength);
            Serial.print("SSID set to: ");
            Serial.println(ssid);
            json["ssid"] = ssid.c_str();
        }
        else if (ms.Match("set password="))
        {
            pass = read.substring(ms.MatchLength);
            Serial.print("Password set to: ");
            Serial.println(pass);
            json["pass"] = pass.c_str();
        }
        else if (ms.Match("set ip="))
        {
            ip = read.substring(ms.MatchLength);
            Serial.print("IP Address set to: ");
            Serial.println(ip);
            json["ip"] = ip.c_str();
        }
        else if (ms.Match("set gw="))
        {
            gw = read.substring(ms.MatchLength);
            Serial.print("Gateway set to: ");
            Serial.println(gw);
            json["gw"] = gw.c_str();
        }
        else if (ms.Match("set netmask="))
        {
            netmask = read.substring(ms.MatchLength);
            Serial.print("Netmask set to: ");
            Serial.println(netmask);
            json["netmask"] = netmask.c_str();
        }
        else if (ms.Match("set bot_token="))
        {
            telegram_bot_token = read.substring(ms.MatchLength);
            Serial.print("Telegram bot Token set to: ");
            Serial.println(telegram_bot_token);
            writeFile(SPIFFS, telegramBotTokenPath, telegram_bot_token.c_str());
        }
        else if (ms.Match("save"))
        {
            String out;
            serializeJson(json, out);
            writeFile(SPIFFS, wificonfigPath, out.c_str());
            Serial.println("Config saved!");
        }
        else if (ms.Match("reload"))
        {
            Serial.println("Reloading device....");
            ESP.restart();
        }
    }
    // DNS
    dnsServer.processNextRequest();
    // HTTP
    // server.handleClient();

    if (millis() - bot_lasttime > BOT_MTBS)
    {
        int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

        while (numNewMessages)
        {
            Serial.println("got response");
            handleNewMessages(numNewMessages);
            numNewMessages = bot.getUpdates(bot.last_message_received + 1);
        }

        bot_lasttime = millis();
    }
}