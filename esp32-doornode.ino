// mainframe.io hackspace access control system door node for ESP32-PoE

// TODO: Doku :)
// TODO: Source überarbeiten
// TODO: Build-Environment neu aufsetzen und testen obs immer noch funktioniert :)

// postponed: Config per dhcp holen
// low prio TODO: Keypad
// low prio TODO: Display

// Features may be disabled for debugging purposes
#define WS2812
#define MQTT
#define LLDP
#define NTP
#define CONFIG
#define SYSLOG
#define WEBSERVER
#define LOGBUFFER
#define WEBUPDATE
#define WATCHDOG

#include <Arduino.h>

#ifdef WATCHDOG
#include "esp_system.h"
#endif

// Config structure as represented by JSON config file
struct conf_s
{
  struct node             // node specific data
  {
    char *hostname;       // this node's hostname
  } node;
  struct mqtt             // MQTT settings
  {
    struct server         // MQTT server settings
    {
      char *host;         // hostname of MQTT server
      uint16_t port;      // TCP port to use
      char *user;         // username
      char *passwd;       // passwd
      char *fingerprint;  // certificate fingerprint
    } server;
    struct topic          // MQTT topics
    {
      char *base;         // base topic for auto-created topics
      char *closed;       // topic for status "door closed"
      char *locked;       // topic for status "door locked"
      char *doorbell;     // topic for "doorbell pressed"
      char *buzzer;       // topic for door buzzer
      char *led;          // topic for WS2812 LED animations
    } topic;
    struct will           // MQTT last will settings
    {
      char *topic;        // last will topic
      int qos;            // last will QoS
      bool retain;        // last will retain flag
      char *message;      // last will message
    } will;
  } mqtt;
  struct debug            // debugging settings
  {
    boolean force;        // send every log message to syslog (or only most important)
    unsigned long uptimeInterval;  // interval (in ms) after which the current uptime and free heap is sent to syslog
  } debug;
  struct gpio             // GPIO port numbers
  {
    int buzzer;           // GPIO port for door buzzer
    int ws2812;           // GPIO port for WS2812 LED(s)
    int doorbell;         // GPIO port for doorbell button (active low)
    int door;             // GPIO port for door closed detection (active low)
    int lock;             // GPIO port for door locked detection (active low)
    bool doorbellUseInterrupt;  // flag: use interrupt for doorbell button press detection
    unsigned long minimumActiveTime; // minimum low interval to trigger GPIO
  } gpio;
  struct ws2812           // WS2812 config
  {
    char *type;           // type: "RGB" or "GRB"
    uint16_t num;         // number of LEDs
    int brightness;       // brightness (0..255)
    float gamma;          // gamma correction
    char *noEth;          // animation string to be shown if ethernet is not connected
    char *noMqtt;         // animation string to be shown if MQTT is not connected
    unsigned long animationInterval;  // interval (in ms) after which LED animation is updated
  } ws2812;
  struct web              // internal web server config
  {
    uint16_t port;        // TCP port (usually 80)
    char *accessPasswd;   // access password
    char *adminPasswd;    // admin password (for rebooting)
  } web;
  struct syslog           // syslog settings
  {
    char *host;           // syslog server hostname
    uint16_t port;        // syslog UDP port
    char *appName;        // app name
  } syslog;
  struct ntp              // NTP settings
  {
    char *host;           // NTP server hostname
  } ntp;
  struct buzzer           // Door buzzer settings
  {
    unsigned long maxTime;  // maximum time (in ms) the door buzzer may be enabled
  } buzzer;
  struct lldp             // LLDP settings
  {
    unsigned long interval;  // LLDP packet send interval in ms (usually 30000 for 30s)
  } lldp;
};

// active config
conf_s conf;
// default config
conf_s configDefault;

#ifdef WS2812
// WS2812 animation structure
struct animation
{
  byte r;     // red color value
  byte g;     // green color value
  byte b;     // blue color value
  bool fade;  // true - fade to next step, false - keep color until next step
  unsigned long interval;  // length (in ms) of current step
  bool end;   // true - stop animation and keep this color if this step is reached
};
#endif

#include "settings.h"

#include <ETH.h>

#ifdef MQTT
#include <PubSubClient.h>
#endif

#ifdef NTP
#include <NTPClient.h>
#endif

#if defined SYSLOG || defined NTP
#include <WiFiUdp.h>
#endif

#include <Time.h>
#include <TimeLib.h>

#ifdef MQTT
#include <WiFiClientSecure.h>
#endif

#ifdef WEBSERVER
#include <WebServer.h>
#endif

#ifdef SYSLOG
#include <Syslog.h>
#endif

#ifdef CONFIG
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#endif

#ifdef WS2812
#include <NeoPixelBus.h>
#endif

#ifdef CONFIG
#include <ArduinoJson.h>
#include <HTTPClient.h>
#endif

#ifdef WEBUPDATE
#include <Update.h>
#endif

String     = "";

// Some magic to create an ISO formatted compile timestamp
#define COMPILE_HOUR          (((__TIME__[0]-'0')*10) + (__TIME__[1]-'0'))
#define COMPILE_MINUTE        (((__TIME__[3]-'0')*10) + (__TIME__[4]-'0'))
#define COMPILE_SECOND        (((__TIME__[6]-'0')*10) + (__TIME__[7]-'0'))
#define COMPILE_YEAR          ((((__DATE__ [7]-'0')*10+(__DATE__[8]-'0'))*10+(__DATE__ [9]-'0'))*10+(__DATE__ [10]-'0'))
#define COMPILE_MONTH         ((  __DATE__ [2] == 'n' ? (__DATE__ [1] == 'a' ? 0 : 5)   \
                                  : __DATE__ [2] == 'b' ? 1                               \
                                  : __DATE__ [2] == 'r' ? (__DATE__ [0] == 'M' ?  2 : 3)  \
                                  : __DATE__ [2] == 'y' ? 4                               \
                                  : __DATE__ [2] == 'l' ? 6                               \
                                  : __DATE__ [2] == 'g' ? 7                               \
                                  : __DATE__ [2] == 'p' ? 8                               \
                                  : __DATE__ [2] == 't' ? 9                               \
                                  : __DATE__ [2] == 'v' ? 10 : 11) +1)
#define COMPILE_DAY           ((__DATE__ [4]==' ' ? 0 : __DATE__  [4]-'0')*10+(__DATE__[5]-'0'))


#ifdef WEBSERVER
WebServer server;
#endif

// MQTT
#ifdef MQTT
WiFiClientSecure wifiClientSecure;
PubSubClient * mqttClient;
#endif

// NTP
#ifdef NTP
WiFiUDP ntpUDP;
NTPClient* timeClient;
#endif

// WS2812
#ifdef WS2812
NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod>* stripGRB = NULL;
NeoPixelBus<NeoRgbFeature, Neo800KbpsMethod>* stripRGB = NULL;
#endif

#ifdef SYSLOG
WiFiUDP syslogUDP;
Syslog* syslog;
#endif

bool eth_connected = false;
bool doorClosed;
bool doorLocked;
bool doorBell;
bool doorClosedOld = false;
bool doorLockedOld = false;
bool doorBellOld = false;
bool doorBellInterrupt = false;
bool mqttConnected;
bool mqttCertificateError = false;
bool mqttTryConnect = true;
bool forceStatusUpdate = false;
bool doorBuzzer = false;
bool gotConfig = false;
bool ws2812GRB = false;   // true - GRB, false - RGB

unsigned long retryTimestampMqtt = millis();
unsigned long retryTimestampMqttCert = millis();
unsigned long uptimeTimestamp = millis();
unsigned long mqttConnectTimestamp;
unsigned long doorBuzzerTimestamp;
unsigned long animationTimestamp = millis();
unsigned long ethConnectTimestamp;
unsigned long timeClientTimestamp = millis();
unsigned long retryGetConfig = millis();

unsigned long doorBuzzerInterval = 0;

unsigned long animationBegin = millis();
unsigned long animationLength = 0;
bool animationRunning = true;

byte fixedR = 0, fixedG = 0, fixedB = 0;

const unsigned long retryIntervalMqtt = 10000;
const unsigned long retryIntervalMqttCert = 60000;
const unsigned long doorBuzzerIntervalMax = 10000;
const unsigned long getConfigInterval = 30000;

unsigned long lldpTimestamp = millis();

#ifdef WS2812
animation animationSteps[MAX_ANIMATION_STEPS];
#endif

#ifdef LOGBUFFER
typedef char logLine[120];
int logBufferPos = 0;
logLine logBuffer[LOG_BUFFER_SIZE];
#endif

String configUrl;

#ifdef WATCHDOG
hw_timer_t *timer = NULL;
const unsigned long watchdogTimeout = 60*1000;
#endif


#ifdef MQTT
void mqttCallback(char* topic, byte* payload, unsigned int length)
{
  String sTopic = String((char *)topic);
  String sPayload = String((char *)payload).substring(0, length);
  debugText("Received: " + sTopic + " => " + sPayload, true);

  // Ignore anything but the requested topics
  String sTopicBuzzer = String((char *)conf.mqtt.topic.buzzer);
  String sTopicLed = String((char *)conf.mqtt.topic.led);
  if (sTopic == sTopicBuzzer)
  {
    debugText("Buzzer topic matches");

    doorBuzzerInterval = sPayload.toInt();
    if (doorBuzzerInterval == -1)
    {
      debugText("That was probably my own buzzer turn off confirmation", true);
    }
    else if (doorBuzzerInterval < 0 || doorBuzzerInterval > doorBuzzerIntervalMax)
    {
      debugText("Invalid door buzzer interval", true);
    }
    else
    {
      doorBuzzer = true;
      doorBuzzerTimestamp = millis();
    }
  }
  else if (sTopic == sTopicLed)
  {
    debugText("LED topic matches");
    parseAnimation(sPayload);
  }
  else
  {
    debugText("Unknown topic");
  }
}
#endif

void initObjects()
{
#ifdef WEBSERVER
  if (conf.web.port > 0)
  {
    debugText("Init web server");
    server.begin(conf.web.port);
    server.on("/", webPageRoot);
    server.on("/gpio", webPageGpio);
    server.on("/log", webPageLog);
    server.on("/config", webPageConfig);
    server.on("/led", webPageLed);
    server.on("/reboot", webPageReboot);
#ifdef WEBUPDATE
    server.on("/update", webPageUpdate);
    server.on("/updateDo", HTTP_POST, webPageUpdateDo, webPageUpdateDoProcess);
#endif
    server.onNotFound(webPage404);
    server.begin();
  }
#endif

#ifdef NTP
  if (strlen(conf.ntp.host) > 0)
  {
    debugText("Init NTP client");
    if (timeClient != NULL)
      delete timeClient;
    timeClient = new NTPClient(ntpUDP, conf.ntp.host);
    timeClient->begin();
  }
#endif

#ifdef WS2812
  if (conf.gpio.ws2812 >= 0)
  {
    debugText("Init WS2812");
    if (strcmp(conf.ws2812.type, "GRB") == 0)
    {
      ws2812GRB = true;
      if (stripGRB != NULL)
        delete stripGRB;
      stripGRB = new NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod>(conf.ws2812.num, conf.gpio.ws2812);
      stripGRB->Begin();
    }
    else
    {
      ws2812GRB = false;
      if (stripRGB != NULL)
        delete stripRGB;
      stripRGB = new NeoPixelBus<NeoRgbFeature, Neo800KbpsMethod>(conf.ws2812.num, conf.gpio.ws2812);
      stripRGB->Begin();
    }
  }
#endif

#ifdef SYSLOG
  if (syslog != NULL)
    delete syslog;
  if (strlen(conf.syslog.host) > 0)
  {
    debugText("Init syslog " + String(conf.syslog.host));
    syslog = new Syslog(syslogUDP, conf.syslog.host, conf.syslog.port, conf.node.hostname, conf.syslog.appName, LOG_KERN);
    debugText(getEthInfo(), true);
  }
#endif

  // Init GPIO
  debugText("Init GPIO");
  if (conf.gpio.buzzer >= 0)
  {
    pinMode(conf.gpio.buzzer, OUTPUT);
    digitalWrite(conf.gpio.buzzer, LOW);
  }
  if (conf.gpio.doorbell >= 0)
    pinMode(conf.gpio.doorbell, INPUT);
  if (conf.gpio.door >= 0)
    pinMode(conf.gpio.door, INPUT);
  if (conf.gpio.lock >= 0)
    pinMode(conf.gpio.lock, INPUT);

  // Init interrupt if required
  if (conf.gpio.doorbell >= 0 && conf.gpio.doorbellUseInterrupt)
    startInterrupt();

}

String formatIP(IPAddress ip)
{
  // return IP address in human readable form (e.g. 127.0.0.1)
  String out;
  out += String(ip[0]) + ".";
  out += String(ip[1]) + ".";
  out += String(ip[2]) + ".";
  out += String(ip[3]);
  return out;
}

String getEthInfo()
{
  // get ethernet info string
  String out = "ETH MAC: " + ETH.macAddress() + ", IPv4: " + formatIP(ETH.localIP()) + ", " + ETH.linkSpeed() + "Mbps";
  if (ETH.fullDuplex())
    out += ", Full duplex";
  return out;
}

void WiFiEvent(WiFiEvent_t event)
{
  // wi-fi event handler
  switch (event)
  {
    case SYSTEM_EVENT_ETH_START:
      {
        debugText("Eth started");
        //set eth hostname here
        if (strlen(conf.node.hostname) > 0)
        {
          ETH.setHostname(conf.node.hostname);
          debugText("Hostname is now " + String(conf.node.hostname));
        }
      }
      break;
    case SYSTEM_EVENT_ETH_CONNECTED:
      {
        debugText("Eth connected");
      }
      break;
    case SYSTEM_EVENT_ETH_GOT_IP:
      {
        String ethInfo = getEthInfo();
        debugText(ethInfo);
        eth_connected = true;
        ethConnectTimestamp = millis();
        // force get config
        retryGetConfig = millis() - getConfigInterval;
      }
      break;
    case SYSTEM_EVENT_ETH_DISCONNECTED:
      {
        debugText("Eth disconnected");
        eth_connected = false;
        parseAnimation(String(conf.ws2812.noEth));
      }
      break;
    case SYSTEM_EVENT_ETH_STOP:
      {
        debugText("Eth stopped");
        eth_connected = false;
        parseAnimation(String(conf.ws2812.noEth));
      }
      break;
    default:
      {
        // nothing
      }
      break;
  }

}

void getMqttStatus()
{
  // get MQTT connection status and re-connect if required

#ifdef MQTT
  if (mqttClient == NULL || !mqttClient->connected())
  {

    mqttConnected = false;
    if (mqttTryConnect)
    {
      mqttTryConnect = false;
      debugText("Connecting to MQTT server...", true);

      if (mqttClient != NULL)
      {
        debugText("delete mqttClient");
        delete mqttClient;
      }

      mqttClient = new PubSubClient(conf.mqtt.server.host, conf.mqtt.server.port, wifiClientSecure);
      mqttClient->setServer(conf.mqtt.server.host, conf.mqtt.server.port);
      mqttClient->setCallback(mqttCallback);

      bool connectSuccess;

      // will topic available?
      if (strlen(conf.mqtt.will.topic) == 0) // no will topic set
        connectSuccess = mqttClient->connect(conf.node.hostname, conf.mqtt.server.user, conf.mqtt.server.passwd);
      else // will topic is set
        connectSuccess = mqttClient->connect(conf.node.hostname, conf.mqtt.server.user, conf.mqtt.server.passwd, conf.mqtt.will.topic, conf.mqtt.will.qos, conf.mqtt.will.retain, conf.mqtt.will.message);

      if (connectSuccess)
      {
        debugText("Connected to MQTT server, checking cert...");

        if (wifiClientSecure.verify(conf.mqtt.server.fingerprint, conf.mqtt.server.host))
        {
          debugText("Certificate matches!", true);

          mqttConnectTimestamp = millis();
          forceStatusUpdate = true;
        }
        else
        {
          debugText("Certificate doesn't match!", true);
          mqttCertificateError = true;
          retryTimestampMqttCert = millis();
        }

        if (strlen(conf.mqtt.topic.buzzer) > 0) // buzzer topic available
          mqttClient->subscribe(conf.mqtt.topic.buzzer);

        if (strlen(conf.mqtt.topic.led) > 0) // led topic available
          mqttClient->subscribe(conf.mqtt.topic.led);

      }
      else
      {
        retryTimestampMqtt = millis();
        debugText("Could not connect to MQTT server.", true);
        parseAnimation(String(conf.ws2812.noMqtt));
      }
    }
  }

  if (!mqttConnected && mqttClient->connected())
  {
    mqttConnected = true;
  }
  if (mqttConnected)
  {
    mqttClient->loop();
  }
#endif
}

void getDoorStatus()
{
  // get status of door (open/closed, locked?) and send MQTT messages

  if (conf.gpio.door >= 0)
    doorClosed = !digitalRead(conf.gpio.door);
  if (conf.gpio.lock >= 0)
    doorLocked = !digitalRead(conf.gpio.lock);

  if (conf.gpio.doorbell >= 0)
  {
    if (conf.gpio.doorbellUseInterrupt)
    {
      doorBell = doorBellInterrupt;
    }
    else
    {
      doorBell = !digitalRead(conf.gpio.doorbell);
    }
  }

  if (doorClosed || doorLocked || doorBell)
  {
    delay(conf.gpio.minimumActiveTime);
    if (doorClosed)
      doorClosed = !digitalRead(conf.gpio.door);
    if (doorLocked)
      doorLocked = !digitalRead(conf.gpio.lock);
    if (!conf.gpio.doorbellUseInterrupt)
    {
      if (doorBell)
        doorBell = !digitalRead(conf.gpio.doorbell);
    }
  }



  if (doorClosed != doorClosedOld || forceStatusUpdate)
  {
    //// poor man's debouncing: just wait 5ms
    //delay(5);
    //debugText("door changed");

    if (doorClosed)
    {
      debugText("Door closed", true);
#ifdef MQTT
      mqttClient->publish(conf.mqtt.topic.closed, "1", true);
#endif
    }
    else
    {
      debugText("Door opened", true);
#ifdef MQTT
      mqttClient->publish(conf.mqtt.topic.closed, "0", true);
#endif
    }
  }

  if (doorLocked != doorLockedOld || forceStatusUpdate)
  {
    //// poor man's debouncing: just wait 5ms
    //delay(5);
    //debugText("lock changed");

    if (doorLocked)
    {
      debugText("Door locked", true);
#ifdef MQTT
      mqttClient->publish(conf.mqtt.topic.locked, "1", true);
#endif
    }
    else
    {
      debugText("Door unlocked", true);
#ifdef MQTT
      mqttClient->publish(conf.mqtt.topic.locked, "0", true);
#endif
    }
  }

  if (doorBell && !doorBellOld)
  {
    //// poor man's debouncing: just wait 5ms
    //delay(5);
    debugText("Bell pressed", true);
#ifdef MQTT
    mqttClient->publish(conf.mqtt.topic.doorbell, "1", false);
#endif
    if (conf.gpio.doorbell >= 0 && conf.gpio.doorbellUseInterrupt)
      startInterrupt();
  }
  else if (!doorBell && doorBellOld)
  {
    //// poor man's debouncing: just wait 5ms
    //delay(5);
  }

  // keep track of state of previous run
  doorClosedOld = doorClosed;
  doorLockedOld = doorLocked;
  doorBellOld = doorBell;

  forceStatusUpdate = false;

}


void debugText(String text)
{
  // send a debug message with low importance
  debugText(text, false);
}

void debugText(String text, boolean overrideSpam)
{
  // get current date/time
  String formattedTime = "00:00:00";
  unsigned long NTPstamp = 0;
  String debugText = "";
#ifdef NTP
  if (timeClient != NULL)
  {
    formattedTime = timeClient->getFormattedTime();
    NTPstamp = timeClient->getEpochTime();
  }

  debugText += String(year(NTPstamp));
  debugText += "-";
  debugText += padZero(String(month(NTPstamp)));
  debugText += "-";
  debugText += padZero(String(day(NTPstamp)));
  debugText += " ";
  debugText += formattedTime;
  debugText += " ";
#endif

  debugText += text;

  // log to serial console
  Serial.println(debugText);

  // add log line to log buffer if enabled
#ifdef LOGBUFFER
  debugText.toCharArray(logBuffer[logBufferPos], 200);
  //  logBuffer[logBufferPos] = debugText;
  logBufferPos++;
  logBufferPos = logBufferPos % LOG_BUFFER_SIZE;
#endif

  // send log message via syslog if configured
#ifdef SYSLOG
  if (conf.debug.force || overrideSpam)
  {
    char text_c[200];
    debugText.toCharArray(text_c, 200);
    if (syslog != NULL)
      syslog->logf(LOG_INFO, text_c);
  }
#endif
}

String padZero(String val)
{
  // pad a value with leading zero to create two-digit output
  if (val.length() < 2)
  {
    val = "0" + val;
  }
  return val;
}

String millis2String(unsigned long m)
{
  // convert milliseconds to days/hours/minutes/seconds string (e.g. 3d 12:34:56)
  long s = m / 1000;
  int ups = s % 60;
  int upm = s % 3600 / 60;
  int uph = s % 86400 / 3600;
  int upd = s / 86400;

  String out = "";
  out += String(upd);
  out += "d ";
  out += padZero(String(uph));
  out += ":";
  out += padZero(String(upm));
  out += ":";
  out += padZero(String(ups));

  return out;
}

String uptimeString()
{
  // create string which contains uptimes of system, ethernet, mqtt connection and current free heap
  long m = millis();

  String uptime = "System up: ";
  uptime += millis2String(m);

  if (eth_connected)
  {
    uptime += " Eth: ";
    uptime += millis2String(m - ethConnectTimestamp);
  }
  else
  {
    uptime += " Eth down";
  }

#ifdef MQTT
  if (mqttConnected)
  {
    uptime += " MQTT: ";
    uptime += millis2String(m - mqttConnectTimestamp);
  }
  else
  {
    uptime += " MQTT down";
  }
#endif

  uptime += " Heap: ";
  uptime += String(ESP.getFreeHeap());

  return uptime;
}

void debugUptime()
{
  // send uptime string to debug outputs
  String uptime = uptimeString();
  debugText(uptime, true);
}


void showAnimation()
{
  // calculate next animatin step and send to WS2812 LEDs

#ifdef WS2812
  if (!animationRunning)
  {
    setLedColor(fixedR, fixedG, fixedB);
    return;
  }

  unsigned long pos = millis() - animationBegin;
  pos = pos % animationLength;

  byte r = 0, g = 0, b = 0;

  unsigned long posa = 0;
  unsigned long posb = 0;

  for (int i = 0; i < 19; i++)
  {

    animation current = animationSteps[i];
    animation next = animationSteps[i + 1];

    posa = posb;
    posb += current.interval;

    if ((pos >= posa && (current.end || pos <= posb))) // || current.end)
    {
      if (current.end) // && pos >= posa)
      {
        animationRunning = false;
        fixedR = current.r;
        fixedG = current.g;
        fixedB = current.b;
        return;
      }
      else if (current.fade)
      {
        r = map(pos, posa, posb, current.r, next.r);
        g = map(pos, posa, posb, current.g, next.g);
        b = map(pos, posa, posb, current.b, next.b);
      }
      else
      {
        r = current.r;
        g = current.g;
        b = current.b;
      }
    }
  }
  setLedColor(r, g, b);
#endif
}

void setLedColor(byte r, byte g, byte b)
{
  // Set LED color, calculate brightness and gamma, send to LED stripe

#ifdef WS2812
  RgbColor col(calcGamma(r * conf.ws2812.brightness >> 8),
               calcGamma(g * conf.ws2812.brightness >> 8),
               calcGamma(b * conf.ws2812.brightness >> 8));
  if (ws2812GRB)
  {
    if (stripGRB != NULL)
    {
      for (int i = 0; i < conf.ws2812.num; i++)
        stripGRB->SetPixelColor(i, col);
      stripGRB->Show();
    }
  }
  else
  {
    if (stripRGB != NULL)
    {
      for (int i = 0; i < conf.ws2812.num; i++)
        stripRGB->SetPixelColor(i, col);
      stripRGB->Show();
    }
  }
#endif
}

void getDefaultConfig()
{
  // fill config structure with defaults

  configDefault.node.hostname             = NODE_HOSTNAME;
  configDefault.ntp.host                  = "";
  configDefault.mqtt.server.host          = "";
  configDefault.mqtt.server.port          = 0;
  configDefault.mqtt.server.user          = "";
  configDefault.mqtt.server.passwd        = "";
  configDefault.mqtt.server.fingerprint   = "";
  configDefault.mqtt.topic.base           = "";
  configDefault.mqtt.topic.closed         = "";
  configDefault.mqtt.topic.locked         = "";
  configDefault.mqtt.topic.doorbell       = "";
  configDefault.mqtt.topic.buzzer         = "";
  configDefault.mqtt.topic.led            = "";
  configDefault.mqtt.will.topic           = "";
  configDefault.mqtt.will.qos             = 0;
  configDefault.mqtt.will.retain          = false;
  configDefault.mqtt.will.message         = "";
  configDefault.debug.force               = false;
  configDefault.debug.uptimeInterval      = 300000;
  configDefault.gpio.buzzer               = -1;
  configDefault.gpio.ws2812               = GPIO_WS2812;
  configDefault.gpio.doorbell             = -1;
  configDefault.gpio.door                 = -1;
  configDefault.gpio.lock                 = -1;
  configDefault.gpio.doorbellUseInterrupt = false;
  configDefault.gpio.minimumActiveTime    = MINIMUM_ACTIVE_TIME;
  configDefault.ws2812.type               = "RGB";
  configDefault.ws2812.num                = 8;
  configDefault.ws2812.brightness         = WS2812_BRIGHTNESS;
  configDefault.ws2812.gamma              = WS2812_GAMMA;
  configDefault.ws2812.noEth              = WS2812_NOETH;
  configDefault.ws2812.noMqtt             = "";
  configDefault.ws2812.animationInterval  = 10;
  configDefault.web.port                  = WEB_PORT;
  configDefault.web.accessPasswd          = WEB_ACCESSPASSWD;
  configDefault.web.adminPasswd           = WEB_ADMINPASSWD;
  configDefault.syslog.host               = SYSLOG_HOST;
  configDefault.syslog.port               = SYSLOG_PORT;
  configDefault.syslog.appName            = SYSLOG_APPNAME;
  configDefault.ntp.host                  = "";
  configDefault.buzzer.maxTime            = 10000;
  configDefault.lldp.interval             = 30000;
}


void startInterrupt()
{
  // start interrupt for doorbell button

  doorBellInterrupt = false;
  attachInterrupt(digitalPinToInterrupt(conf.gpio.doorbell), handleDoorBell, FALLING);
}

void handleDoorBell()
{
  // handle doorbell interrupt

  detachInterrupt(digitalPinToInterrupt(conf.gpio.doorbell));
  delay(conf.gpio.minimumActiveTime);
  if (!digitalRead(conf.gpio.doorbell))
    doorBellInterrupt = true;
}

void parseAnimation(String in)
{
  // parse animation string to internal structure

#ifdef WS2812
  int p;
  String t;
  bool found = true;
  bool color = true;
  bool foundAnimation = false;
  int stepCounter = 0;
  animation animationStep;

  in.concat(" ");

  clearAnimation();

  while (found && stepCounter < MAX_ANIMATION_STEPS)
  {
    p = in.indexOf(" ");
    if (p < 0)
    {
      t = in;
      found = false;
    }
    else
    {
      t = in.substring(0, p);
      in = in.substring(p + 1);
    }

    if (color)
    {
      // current value is a color
      unsigned long color = strtol(&t[0], NULL, 16);
      animationStep.r = color >> 16;
      animationStep.g = color >> 8 & 0xff;
      animationStep.b = color & 0xff;
    }
    else
    {
      // current value is an interval
      foundAnimation = true;
      if (t == "z" || t == "Z")
      {
        animationStep.end = true;
        animationStep.interval = 1000;
      }
      else
      {
        animationStep.end = false;
        long interval = t.toInt();
        if (interval < 0)
        {
          animationStep.fade = true;
          interval *= -1;
        }
        else
          animationStep.fade = false;

        animationStep.interval = interval;
      }
      animationSteps[stepCounter] = animationStep;
      stepCounter++;

    }
    color = !color;

  }

  if (!foundAnimation)
  {
    animationRunning = false;
    fixedR = animationStep.r;
    fixedG = animationStep.g;
    fixedB = animationStep.b;
  }

  countAnimation();

#endif
}

void clearAnimation()
{
  // clear animation buffer

#ifdef WS2812
  // clear animation buffer
  animation step;
  step.r = 0;
  step.g = 0;
  step.b = 0;
  step.fade = false;
  step.interval = 0;
  step.end = false;
  for (int i = 0; i < 20; i++)
    animationSteps[i] = step;

  fixedR = 0;
  fixedG = 0;
  fixedB = 0;
#endif
}

void countAnimation()
{
  // count number of animation steps

#ifdef WS2812
  // count total length of animation and reset animation start time
  animationLength = 0;
  for (int i = 0; i < 20; i++)
    animationLength += animationSteps[i].interval;
  if (animationLength == 0)
  {
    animationLength++;
    animationRunning = false;
    fixedR = animationSteps[0].r;
    fixedG = animationSteps[0].g;
    fixedB = animationSteps[0].b;
  }
  else
  {
    animationRunning = true;
    animationBegin = millis();
  }
#endif
}

#ifdef WEBSERVER
void webPageGpio()
{
  // handle webpage for GPIO state

  debugText("Web access from " + server.client().remoteIP().toString() + " path: " + server.uri());
  String passwd = getPasswdParameter();
  char passwd_c[16];
  String response = webPageHeader();
  passwd.toCharArray(passwd_c, 16);

  if (strcmp(conf.web.accessPasswd, passwd_c) == 0 || strcmp(conf.web.adminPasswd, passwd_c) == 0 )
  {

    response += "I/O state: ";
    response += "0b";
    for (int i = 39; i >= 0; i--)
      response += (digitalRead(i) ? "1" : "0");
    response += "<br><br>\n";

    for (int i = 0; i < 40; i++)
    {
      response += "GPIO " + String(i) + ": " + (digitalRead(i) ? "HIGH" : "low ");
      if (i == conf.gpio.ws2812) response += " - WS2812";
      if (i == conf.gpio.buzzer) response += " - buzzer";
      if (i == conf.gpio.doorbell) response += " - doorbell";
      if (i == conf.gpio.door) response += " - door";
      if (i == conf.gpio.lock) response += " - lock";
      if (i == ETH_PHY_POWER) response += " - PHY power";
      response += "<br>\n";
    }
    response += "<br>\n";

  }
  else
    response += "access denied<br>\n";

  response += webPageFooter();
  server.send(200, "text/html", response);
}

void webPageLog()
{
  // webpage: log buffer output

  debugText("Web access from " + server.client().remoteIP().toString() + " path: " + server.uri());
  String passwd = getPasswdParameter();
  char passwd_c[16];
  String response = webPageHeader();
  passwd.toCharArray(passwd_c, 16);

  if (strcmp(conf.web.accessPasswd, passwd_c) == 0 || strcmp(conf.web.adminPasswd, passwd_c) == 0 )
  {

    response += "Log:<br><br><span style=\"font-family: Courier, Lucida Console, monospace;\">\n";
#ifdef LOGBUFFER
    for (int i = 0; i < LOG_BUFFER_SIZE; i++)
    {
      int logBufferRow;
      logBufferRow = (logBufferPos + i) % LOG_BUFFER_SIZE;
      if (strlen(logBuffer[logBufferRow]) > 0)
        response += String(logBuffer[logBufferRow]) + "<br>\n";
    }
#endif
    response += "</span>\n";
  }
  else
    response += "access denied<br>\n";

  response += webPageFooter();
  server.send(200, "text/html", response);
}

void webPageLed()
{
  // webpage: LED status

  debugText("Web access from " + server.client().remoteIP().toString() + " path: " + server.uri());
  String passwd = getPasswdParameter();
  char passwd_c[16];
  String response = webPageHeader();
  passwd.toCharArray(passwd_c, 16);

  if (strcmp(conf.web.accessPasswd, passwd_c) == 0 || strcmp(conf.web.adminPasswd, passwd_c) == 0 )
  {

    response += "LED:<br><br>\n";

    response += "Brightness: " + String(conf.ws2812.brightness) + "<br>\n";
    response += "Gamma: " + String(conf.ws2812.gamma) + "<br><br>\n";

    response += "Animation ";
    if (!animationRunning)
      response += "not ";
    response += "running<br><br>\n";

    response += "Fixed color: ";
    response += "<table cellspacing=\"2\" cellpadding=\"2\" border=\"0\">\n";
    response += "<tr>";
    response += "<td>";
    response += "#" + hexEncode(fixedR) + hexEncode(fixedG) + hexEncode(fixedB);
    response += "<td>";
    response += "<td bgcolor=\"#" + hexEncode(fixedR) + hexEncode(fixedG) + hexEncode(fixedB) + "\">";
    response += "&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;";
    response += "</td>";
    response += "</tr>";
    response += "</table>\n";
    response += "<br><br>\n";

    response += "Animation steps:<br>\n";
    response += "<table cellspacing=\"2\" cellpadding=\"2\" border=\"0\">\n";
    response += "<tr><td>Step</td><td>Color</td><td></td><td>Fade</td><td>Interval</td><td>End</td></tr>\n";
#ifdef WS2812
    for (int i = 0; i < 20; i++)
    {
      response += "<tr>";
      // Step
      response += "<td align=\"right\">";
      response += String(i);
      response += "</td>";
      // Color
      response += "<td>";
      response += "#" + hexEncode(animationSteps[i].r) + hexEncode(animationSteps[i].g) + hexEncode(animationSteps[i].b);
      response += "</td>";
      response += "<td bgcolor=\"#" + hexEncode(animationSteps[i].r) + hexEncode(animationSteps[i].g) + hexEncode(animationSteps[i].b) + "\">";
      response += "&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;";
      response += "</td>";
      // Fade
      response += "<td>";
      response += (animationSteps[i].fade ? "yes" : "no");
      response += "</td>";
      // Interval
      response += "<td align=\"right\">";
      response += String(animationSteps[i].interval);
      response += "</td>";
      // End
      response += "<td>";
      response += (animationSteps[i].end ? "yes" : "no");
      response += "</td>";
      response += "</tr>\n";
    }
#endif
    response += "</table>\n";
  }
  else
    response += "access denied<br>\n";

  response += webPageFooter();
  server.send(200, "text/html", response);

}

#ifdef WEBUPDATE
void webPageUpdate()
{
  // webpage: Update (requires admin access)

  debugText("Web access from " + server.client().remoteIP().toString() + " path: " + server.uri());

  String passwd = getPasswdParameter();
  char passwd_c[16];
  String response = webPageHeader();
  passwd.toCharArray(passwd_c, 16);


  //  for (int i = 0; i < server.args(); i++)
  //  {
  //    if (server.argName(i)=="passwd")
  //      return server.arg(i);
  //  }



  if (strcmp(conf.web.adminPasswd, passwd_c) == 0 )
  {

    response += "<form method='POST' action='/updateDo?passwd=" + getPasswdParameter() + "' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form><br><br>\n";

  }
  else
    response += "access denied<br>\n";

  response += webPageFooter();
  server.send(200, "text/html", response);

}

void webPageUpdateDo()
{
  // webpage: Update (requires admin access)

  debugText("Web access from " + server.client().remoteIP().toString() + " path: " + server.uri());

  String passwd = getPasswdParameter();
  char passwd_c[16];
  String response = webPageHeader();
  passwd.toCharArray(passwd_c, 16);


  if (strcmp(conf.web.adminPasswd, passwd_c) == 0 )
  {

    String response = webPageHeader();

    response += "\n<br>";
    response += (Update.hasError()) ? "NOK" : "OK";
    response += "\n<br>\n";
    response += webPageFooter();


    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", response);
    delay(1000);
    ESP.restart();


  }
  else
    response += "access denied<br>\n";


}

void webPageUpdateDoProcess()
{
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.setDebugOutput(true);
    Serial.printf("Update: %s\n", upload.filename.c_str());
    uint32_t maxSketchSpace = (1048576 - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSketchSpace)) { //start with max available size
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) { //true to set the size to the current progress
      Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
    Serial.setDebugOutput(false);
  }
  yield();
}

#endif


void webPageReboot()
{
  // webpage: Reboot (requires admin access)

  debugText("Web access from " + server.client().remoteIP().toString() + " path: " + server.uri());

  String passwd = getPasswdParameter();
  char passwd_c[16];
  String response = webPageHeader();
  passwd.toCharArray(passwd_c, 16);

  if (strcmp(conf.web.adminPasswd, passwd_c) == 0 )
  {

    String response = webPageHeader();
    response += "if you can read this, the system is now rebooting<br>\n";

  }
  else
    response += "access denied<br>\n";

  response += webPageFooter();
  server.send(200, "text/html", response);

  if (strcmp(conf.web.adminPasswd, passwd_c) == 0 )
    ESP.restart();

}

void webPageConfig()
{
  // webpage: show current config

  debugText("Web access from " + server.client().remoteIP().toString() + " path: " + server.uri());
  String passwd = getPasswdParameter();
  char passwd_c[16];
  String response = webPageHeader();
  passwd.toCharArray(passwd_c, 16);

  if (strcmp(conf.web.accessPasswd, passwd_c) == 0 || strcmp(conf.web.adminPasswd, passwd_c) == 0 )
  {
    response += "Config:<br><br><span style=\"font-family: Courier, Lucida Console, monospace;\">\n";
    response += "MAC=" + ETH.macAddress() + "<br>\n";
    response += "<br>\n";
    response += "Source URL: " + configUrl + "<br>\n";
    response += "<br>\n";
    response += "node.hostname=" + String(conf.node.hostname) + "<br>\n";
    response += "<br>\n";
    response += "mqtt.server.host=" + String(conf.mqtt.server.host) + "<br>\n";
    response += "mqtt.server.port=" + String(conf.mqtt.server.port) + "<br>\n";
    response += "mqtt.server.user=" + String(conf.mqtt.server.user) + "<br>\n";
    response += "mqtt.server.passwd=" + hidePasswd(conf.mqtt.server.passwd) + "<br>\n";
    response += "mqtt.server.fingerprint=" + String(conf.mqtt.server.fingerprint) + "<br>\n";
    response += "<br>\n";
    response += "mqtt.topic.base=" + String(conf.mqtt.topic.base) + "<br>\n";
    response += "mqtt.topic.closed=" + String(conf.mqtt.topic.closed) + "<br>\n";
    response += "mqtt.topic.locked=" + String(conf.mqtt.topic.locked) + "<br>\n";
    response += "mqtt.topic.doorbell=" + String(conf.mqtt.topic.doorbell) + "<br>\n";
    response += "mqtt.topic.buzzer=" + String(conf.mqtt.topic.buzzer) + "<br>\n";
    response += "mqtt.topic.led=" + String(conf.mqtt.topic.led) + "<br>\n";
    response += "<br>\n";
    response += "mqtt.will.topic=" + String(conf.mqtt.will.topic) + "<br>\n";
    response += "mqtt.will.qos=" + String(conf.mqtt.will.qos) + "<br>\n";
    response += "mqtt.will.retain=" + String(conf.mqtt.will.retain) + "<br>\n";
    response += "mqtt.will.message=" + String(conf.mqtt.will.message) + "<br>\n";
    response += "<br>\n";
    response += "debug.force=" + String(conf.debug.force) + "<br>\n";
    response += "debug.uptimeInterval=" + String(conf.debug.uptimeInterval) + "<br>\n";
    response += "<br>\n";
    response += "gpio.buzzer=" + String(conf.gpio.buzzer) + "<br>\n";
    response += "gpio.ws2812=" + String(conf.gpio.ws2812) + "<br>\n";
    response += "gpio.doorbell=" + String(conf.gpio.doorbell) + "<br>\n";
    response += "gpio.door=" + String(conf.gpio.door) + "<br>\n";
    response += "gpio.lock=" + String(conf.gpio.lock) + "<br>\n";
    response += "gpio.doorbellUseInterrupt=" + String(conf.gpio.doorbellUseInterrupt) + "<br>\n";
    response += "gpio.minimumActiveTime=" + String(conf.gpio.minimumActiveTime) + "<br>\n";
    response += "<br>\n";
    response += "ws2812.type=" + String(conf.ws2812.type) + "<br>\n";
    response += "ws2812.num=" + String(conf.ws2812.num) + "<br>\n";
    response += "ws2812.brightness=" + String(conf.ws2812.brightness) + "<br>\n";
    response += "ws2812.gamma=" + String(conf.ws2812.gamma) + "<br>\n";
    response += "ws2812.noEth=" + String(conf.ws2812.noEth) + "<br>\n";
    response += "ws2812.noMqtt=" + String(conf.ws2812.noMqtt) + "<br>\n";
    response += "ws2812.animationInterval=" + String(conf.ws2812.animationInterval) + "<br>\n";
    response += "<br>\n";
    response += "web.port=" + String(conf.web.port) + "<br>\n";
    response += "web.accessPasswd=" + hidePasswd(conf.web.accessPasswd) + "<br>\n";
    response += "web.adminPasswd=" + hidePasswd(conf.web.adminPasswd) + "<br>\n";
    response += "<br>\n";
    response += "syslog.host=" + String(conf.syslog.host) + "<br>\n";
    response += "syslog.port=" + String(conf.syslog.port) + "<br>\n";
    response += "syslog.appName=" + String(conf.syslog.appName) + "<br>\n";
    response += "<br>\n";
    response += "ntp.host=" + String(conf.ntp.host) + "<br>\n";
    response += "<br>\n";
    response += "buzzer.maxTime=" + String(conf.buzzer.maxTime) + "<br>\n";
    response += "<br>\n";
    response += "lldp.interval=" + String(conf.lldp.interval) + "<br>\n";
    response += "<br>\n";
    response += "</span>\n";
  }
  else
    response += "access denied<br>\n";

  response += webPageFooter();
  server.send(200, "text/html", response);
}

void webPageRoot()
{
  // webpage: root/menu

  debugText("Web access from " + server.client().remoteIP().toString() + " path: " + server.uri());

  String response = webPageHeader();

  response += "<pre>" + uptimeString() + "</pre><br>\n";

  response += webPageFooter();

  server.send(200, "text/html", response);
}

void webPage404()
{
  // webpage: 404

  String message = "404 File Not Found\n\n";
  message += "path: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

String webPageHeader()
{
  // header for each webpage
  String passwd = getPasswdParameter();

  String response = "<html>\n";
  response += "<head>\n";
  response += "<title>mainframe door node</title>\n";
  response += "<style>\n";
  response += "body {font-family: Arial, Helvetica, Sans-Serif;}\n";
  response += "</style>\n";
  response += "</head>\n";
  response += "<html>\n";
  response += "<strong>" + versionString + "</strong><br><br>\n";
  response += "<a href=\"/?passwd=" + passwd + "\">start</a> - \n";
  response += "<a href=\"/config?passwd=" + passwd + "\">config</a> - \n";
  response += "<a href=\"/led?passwd=" + passwd + "\">LED state</a> - \n";
  response += "<a href=\"/gpio?passwd=" + passwd + "\">GPIO state</a> - \n";
  response += "<a href=\"/log?passwd=" + passwd + "\">log</a> - \n";
  response += "<a href=\"/reboot?passwd=" + passwd + "\">reboot</a>\n";
  #ifdef WEBUPDATE
  response += " - <a href=\"/update?passwd=" + passwd + "\">update</a>\n";
  #endif
  response += "&nbsp;&nbsp;&nbsp;Passwd: <form style=\"display: inline\" method=\"GET\"><input type=\"text\" name=\"passwd\" size=\"10\" value=\"" + passwd + "\">&nbsp;<input type=\"submit\" value=\"send\"></form><br><br>\n";
  return response;

}

String webPageFooter()
{
  // footer for each webpage

  String response = "<br>\n";
  response += "</body>\n";
  response += "</head>\n";
  return response;
}

String hidePasswd(char* passwd)
{
  // Hide password, show only first and last character

  String s = String(passwd);
  if (s.length() == 0)
  {
    s = "";
  }
  else if (s.length() < 3)
  {
    s = "***";
  }
  else
  {
    for (int i = 1; i < s.length() - 1; i++)
      s.setCharAt(i, '*');
  }
  return s;
}

String getPasswdParameter()
{
  // get password from GET parameter

  for (int i = 0; i < server.args(); i++)
  {
    if (server.argName(i) == "passwd")
      return server.arg(i);
  }
  return "";
}

#endif

String hexEncode(byte in)
{
  // convert byte to hex digits
  String out = "";
  if (in < 16)
    out += "0";
  out += String(in, HEX);
  return out;
}


int calcGamma(uint16_t in)
{
  // calculate gamma correction
  return (int)(pow((float)in / (float)255, conf.ws2812.gamma) * 255 + 0.5);
}

#ifdef LLDP
void sendLLDP()
{
  // build and send LLDP packet

  uint8_t packet[1000];

  int pos = 0;

  // Create LLDP packet
  // LLDP multicast (destination) address
  packet[0] = 0x01;
  packet[1] = 0x80;
  packet[2] = 0xc2;
  packet[3] = 0x00;
  packet[4] = 0x00;
  packet[5] = 0x0e;

  // Source MAC address
  // First do some very special Arduino foo
  int macI[6];
  uint8_t mac[6];
  String macAddr = ETH.macAddress();
  char macAddrC[18];
  macAddr.toCharArray(macAddrC, sizeof(macAddrC));
  sscanf(macAddrC, "%x:%x:%x:%x:%x:%x%*c", &macI[0], &macI[1], &macI[2], &macI[3], &macI[4], &macI[5]);
  for (int i = 0; i < 6; i++)
    mac[i] = macI[i];
  packet[6]  = mac[0];
  packet[7]  = mac[1];
  packet[8]  = mac[2];
  packet[9]  = mac[3];
  packet[10] = mac[4];
  packet[11] = mac[5];
  // Ethertype
  packet[12]  = 0x88;
  packet[13]  = 0xcc;

  pos = 14;

  //uint16_t tlv;
  byte value[200];

  // TLV type 1: Chassis ID
  // tlv = lldp_tlv_create(1, 7);
  // Subtype 4, MAC addr
  value[0] = 0x04;
  for (int i = 0; i < 6; i++)
    value[i + 1] = mac[i];
  lldp_tlv_move(packet, 1, 7, pos, value);
  pos += 2 + 7;

  // TLV type 2: Port ID
  // Subtype 7, locally assigned
  value[0] = 0x07;
  value[1] = 'e';
  value[2] = 't';
  value[3] = 'h';
  lldp_tlv_move(packet, 2, 4, pos, value);
  pos += 2 + 4;

  // TLV type 3: Time to live (hardcoded 120s)
  value[0] = 0x00;
  value[1] = 0x78;
  lldp_tlv_move(packet, 3, 2, pos, value);
  pos += 2 + 2;

  // TLV type 5: system name
  String hostname(conf.node.hostname);
  String name = hostname;
  name.toCharArray((char*)value, sizeof(value));
  lldp_tlv_move(packet, 5, name.length(), pos, value);
  pos += 2 + name.length();

  // TLV type 6: system description
  name = versionString + " - " + uptimeString();
  name.toCharArray((char*)value, sizeof(value));
  lldp_tlv_move(packet, 6, name.length(), pos, value);
  pos += 2 + name.length();

  // TLV type 7: system capabilities
  // system capabilities
  value[0] = 0x00;
  value[1] = 0x80; // station only
  // enabled capabilities
  value[2] = 0x00;
  value[3] = 0x80; // station only
  lldp_tlv_move(packet, 7, 4, pos, value);
  pos += 2 + 4;

  // TLV type 8: Management address
  value[0] = 0x05;
  value[1] = 0x01; // subtype IPv4
  IPAddress ip = ETH.localIP();
  value[2] = ip[0];
  value[3] = ip[1];
  value[4] = ip[2];
  value[5] = ip[3];
  value[6] = 0x03; // subtype system port number
  value[7] = 0x00;
  value[8] = 0x00;
  value[9] = 0x00;
  value[10] = 0x00;
  value[11] = 0x00; // OID string length
  lldp_tlv_move(packet, 8, 12, pos, value);
  pos += 2 + 12;

  // TLV type 0: End of LLDPDU
  lldp_tlv_move(packet, 0, 0, pos, value);
  pos += 2;

  esp_err_t fehler;
  fehler =  esp_eth_tx(packet, pos);
  //  debugText(String(esp_err_to_name(fehler)));

}

void lldp_tlv_move(uint8_t packet[], byte type, uint16_t len, int start, uint8_t value[])
{
  // append TLV (type length value) to LLDP packet

  uint16_t tlv;
  tlv = lldp_tlv_create(type, len);
  packet[start] = highByte(tlv);
  packet[start + 1] = lowByte(tlv);
  for (int i = 0; i < len; i++)
    packet[start + 2 + i] = value[i];
}

uint16_t lldp_tlv_create(byte type, uint16_t len)
{
  // create TLV header
  uint16_t tlv;
  tlv = type;
  tlv = tlv << 9;
  tlv = tlv | len;
  return tlv;
}
#endif


void concatenate(char* a, char* b, char* &t)
{
  // concatenate char arrays

  t = (char *)malloc(strlen(a) + strlen(b));
  strcpy(t, a);
  strcat(t, b);
}


void getMqttTopics(conf_s &c)
{
  // derive MQTT topics from base topic

#ifdef MQTT
  if (strlen(c.mqtt.topic.base) > 0)
  {
    if (strlen(c.mqtt.topic.closed) == 0)
      concatenate(c.mqtt.topic.base, "reed-switch", c.mqtt.topic.closed);
    if (strlen(c.mqtt.topic.locked) == 0)
      concatenate(c.mqtt.topic.base, "bolt-contact", c.mqtt.topic.locked);
    if (strlen(c.mqtt.topic.doorbell) == 0)
      concatenate(c.mqtt.topic.base, "bell-button", c.mqtt.topic.doorbell);
    if (strlen(c.mqtt.topic.buzzer) == 0)
      concatenate(c.mqtt.topic.base, "buzzer", c.mqtt.topic.buzzer);
    if (strlen(c.mqtt.topic.led) == 0)
      concatenate(c.mqtt.topic.base, "RGBLED", c.mqtt.topic.led);
  }
#endif
}

#ifdef CONFIG
bool getConfig()
{
  // get and parse config JSON from http server

  configUrl = CONFIG_BASEURL;
  String mac;
  mac = ETH.macAddress();
  mac.setCharAt(2, '-');
  mac.setCharAt(5, '-');
  mac.setCharAt(8, '-');
  mac.setCharAt(11, '-');
  mac.setCharAt(14, '-');
  mac.toLowerCase();
  configUrl.concat(mac);
  configUrl.concat(".json");
  debugText("Config URL: " + configUrl);

  HTTPClient http;

  http.begin(configUrl);
  int httpCode = http.GET();
  debugText("Config GET request status code: " + String(httpCode));

  if (httpCode != 200)
  {
    debugText("Config GET failed");
    return false;
  }

  DynamicJsonDocument doc(16384);
  deserializeJson(doc, http.getString());
  JsonObject root = doc.as<JsonObject>();
  http.end();

  parseConfig(root);

  parseAnimation(String(conf.ws2812.noEth));

  // initialize/instantiate objects
  initObjects();

  return true;

  // return false if not succeeded
}

void decryptPasswd(const char* ciphertextb64, char*& plaintext)
{
  // decrypt base64 encoded and aes encrypted password

  size_t outputLength;
  size_t out_len = 0;
  mbedtls_base64_decode(NULL, 0, &out_len, (const unsigned char*)ciphertextb64, strlen(ciphertextb64));
  unsigned char decoded[out_len];
  mbedtls_base64_decode(decoded, out_len, &out_len, (const unsigned char*)ciphertextb64, strlen(ciphertextb64));

  unsigned char outputBuffer[out_len];

  mbedtls_aes_context aes;

  mbedtls_aes_init( &aes );
  mbedtls_aes_setkey_dec( &aes, (const unsigned char*) key, strlen(key) * 8 );
  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, (const unsigned char*)decoded, outputBuffer);
  mbedtls_aes_free( &aes );

  out_len = 50;
  plaintext = (char *)malloc(out_len);

  int pwlen = 0;
  for (int i = 0; i < 16; i++)
  {
    if (outputBuffer[i] < 0x20)
    {
      if (pwlen != 0)
        pwlen = i;
      outputBuffer[i] = 0x00;
      break;
    }
  }

  strcpy(plaintext, (char*)outputBuffer);

}

void parseConfig(JsonObject& json)
{
  // parse JSON object to config structure

  getDefaultConfig();

  moveJson(json["node"]["hostname"], conf.node.hostname);

  moveJson(json["mqtt"]["server"]["host"], conf.mqtt.server.host);
  moveJson(json["mqtt"]["server"]["port"], conf.mqtt.server.port);
  moveJson(json["mqtt"]["server"]["user"], conf.mqtt.server.user);

  decryptPasswd(json["mqtt"]["server"]["passwd"].as<char *>(), conf.mqtt.server.passwd);

  moveJson(json["mqtt"]["server"]["fingerprint"], conf.mqtt.server.fingerprint);

  moveJson(json["mqtt"]["topic"]["base"], conf.mqtt.topic.base);
#ifdef MQTT
  getMqttTopics(conf);
#endif

  moveJson(json["mqtt"]["topic"]["closed"], conf.mqtt.topic.closed);
  moveJson(json["mqtt"]["topic"]["locked"], conf.mqtt.topic.locked);
  moveJson(json["mqtt"]["topic"]["doorbell"], conf.mqtt.topic.doorbell);
  moveJson(json["mqtt"]["topic"]["buzzer"], conf.mqtt.topic.buzzer);
  moveJson(json["mqtt"]["topic"]["led"], conf.mqtt.topic.led);

  moveJson(json["mqtt"]["will"]["topic"], conf.mqtt.will.topic);
  moveJson(json["mqtt"]["will"]["qos"], conf.mqtt.will.qos);
  moveJson(json["mqtt"]["will"]["retain"], conf.mqtt.will.retain);
  moveJson(json["mqtt"]["will"]["message"], conf.mqtt.will.message);

  moveJson(json["debug"]["force"], conf.debug.force);
  moveJson(json["debug"]["uptimeInterval"], conf.debug.uptimeInterval);

  moveJson(json["gpio"]["buzzer"], conf.gpio.buzzer);
  moveJson(json["gpio"]["ws2812"], conf.gpio.ws2812);
  moveJson(json["gpio"]["doorbell"], conf.gpio.doorbell);
  moveJson(json["gpio"]["door"], conf.gpio.door);
  moveJson(json["gpio"]["lock"], conf.gpio.lock);
  moveJson(json["gpio"]["doorbellUseInterrupt"], conf.gpio.doorbellUseInterrupt);
  moveJson(json["gpio"]["minimumActiveTime"], conf.gpio.minimumActiveTime);

  moveJson(json["ws2812"]["type"], conf.ws2812.type);
  moveJson(json["ws2812"]["num"], conf.ws2812.num);
  moveJson(json["ws2812"]["brightness"], conf.ws2812.brightness);
  moveJson(json["ws2812"]["gamma"], conf.ws2812.gamma);
  moveJson(json["ws2812"]["noEth"], conf.ws2812.noEth);
  moveJson(json["ws2812"]["noMqtt"], conf.ws2812.noMqtt);
  moveJson(json["ws2812"]["animationInterval"], conf.ws2812.animationInterval);

  moveJson(json["web"]["port"], conf.web.port);
  decryptPasswd(json["web"]["accessPasswd"].as<char *>(), conf.web.accessPasswd);
  decryptPasswd(json["web"]["adminPasswd"].as<char *>(), conf.web.adminPasswd);

  moveJson(json["syslog"]["host"], conf.syslog.host);
  moveJson(json["syslog"]["port"], conf.syslog.port);
  moveJson(json["syslog"]["appName"], conf.syslog.appName);

  moveJson(json["ntp"]["host"], conf.ntp.host);

  moveJson(json["buzzer"]["maxTime"], conf.buzzer.maxTime);

  moveJson(json["lldp"]["interval"], conf.lldp.interval);

}


void moveJson(JsonVariant s, char* &t)
{
  // move JSON value to char array
  if (!s.isNull())
  {
    t = (char *)malloc(strlen(s));
    strcpy(t, s.as<char*>());
  }
}

void moveJson(JsonVariant s, uint16_t &t)
{
  // move JSON value to uint16
  if (!s.isNull())
    t = s;
}

void moveJson(JsonVariant s, int &t)
{
  // move JSON value to int
  if (!s.isNull())
    t = s;
}

void moveJson(JsonVariant s, bool &t)
{
  // move JSON value to bool
  if (!s.isNull())
    t = s;
}

void moveJson(JsonVariant s, unsigned long &t)
{
  // move JSON value to unsigned long
  if (!s.isNull())
    t = s;
}

void moveJson(JsonVariant s, float &t)
{
  // move JSON value to float
  if (!s.isNull())
    t = s;
}

#endif

void setup()
{

  // init serial debug output
  Serial.begin(115200);
  delay(100);
  Serial.println();
  
  // get default config
  getDefaultConfig();
  // build MQTT topics in default config
#ifdef MQTT
  getMqttTopics(configDefault);
#endif
  conf = configDefault;

  // build VersionString with ISO date
  versionString = "mainframe.io doorNode ";
  versionString.concat(COMPILE_YEAR);
  versionString.concat("-");
  versionString.concat(padZero(String(COMPILE_MONTH)));
  versionString.concat("-");
  versionString.concat(padZero(String(COMPILE_DAY)));
  versionString.concat(" ");
  versionString.concat(padZero(String(COMPILE_HOUR)));
  versionString.concat(":");
  versionString.concat(padZero(String(COMPILE_MINUTE)));
  versionString.concat(":");
  versionString.concat(padZero(String(COMPILE_SECOND)));
  versionString.concat(" ");
  versionString.concat(PERSON_RESPONSIBLE);

  debugText(versionString);

  // clear log buffer
#ifdef LOGBUFFER
  for (int i = 0; i < LOG_BUFFER_SIZE; i++)
    logBuffer[i][0] = 0x00;
#endif

  debugText("Init Eth");
  WiFi.mode(WIFI_MODE_NULL);
  WiFi.onEvent(WiFiEvent);
  ETH.begin();


  // initialize/instantiate objects
  initObjects();

  // init watchdog
#ifdef WATCHDOG
  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &resetModule, true);
  timerAlarmWrite(timer, watchdogTimeout*1000, false);
  timerAlarmEnable(timer);
#endif  

  debugText("Init finished");

  parseAnimation(String(configDefault.ws2812.noEth));

}

void loop()
{
  // Reset watchdog counter
#ifdef WATCHDOG
  timerWrite(timer, 0);
#endif

  if (eth_connected)
  {
    // do some things that only make sense if ethernet is connected

    // get config if not yet available
#ifdef CONFIG
    if (!gotConfig  && (millis() - retryGetConfig > getConfigInterval))
    {
      gotConfig = getConfig();
      retryGetConfig = millis();
    }
#endif

    // check NTP client
#ifdef NTP
    if (millis() - timeClientTimestamp > TIME_CLIENT_INTERVAL)
    {
      if (timeClient != NULL)
      {
        timeClient->update();
      }
      timeClientTimestamp = millis();
    }
#endif

    // handle web server requests
#ifdef WEBSERVER
    server.handleClient();
#endif

  }

  if (millis() - uptimeTimestamp > conf.debug.uptimeInterval)
  {
    uptimeTimestamp = millis();
    debugUptime();
  }

  if (conf.gpio.buzzer >= 0 && doorBuzzer)
  {
    if (millis() - doorBuzzerTimestamp > doorBuzzerInterval)
    {
      // time out - turn off door buzzer
      debugText("Turn off buzzer");
      digitalWrite(conf.gpio.buzzer, LOW);
      doorBuzzer = false;
      doorBuzzerInterval = 0;
      // Send -1 to buzzer topic to confirm buzzer was turned off
#ifdef MQTT
      mqttClient->publish(conf.mqtt.topic.buzzer, "-1");
#endif
    }
    else
    {
      // turn on door buzzer
      if (!digitalRead(conf.gpio.buzzer))
        debugText("Turn on buzzer");
      digitalWrite(conf.gpio.buzzer, HIGH);
    }
  }

#ifdef MQTT
  if (!mqttTryConnect && !mqttConnected)
  {
    if (mqttCertificateError)
    {
      if (millis() - retryTimestampMqttCert > retryIntervalMqttCert)
      {
        debugText("Re-enable MQTT after certificate check fail", true);
        mqttTryConnect = true;
      }
    }
    else
    {
      if (millis() - retryTimestampMqtt > retryIntervalMqtt)
      {
        debugText("Re-enable MQTT after connection fail", true);
        mqttTryConnect = true;
      }
    }
  }
#endif

#ifdef MQTT
  getMqttStatus();
#endif
  getDoorStatus();

#ifdef WS2812
  if (millis() - animationTimestamp > conf.ws2812.animationInterval)
  {
    showAnimation();
    animationTimestamp = millis();
  }
#endif

#ifdef LLDP
  if (millis() - lldpTimestamp > conf.lldp.interval)
  {
    sendLLDP();
    lldpTimestamp = millis();
  }
#endif
}

#ifdef WATCHDOG
void resetModule()
{
  esp_restart();
}
#endif
