// Person responsible (to be shown in version string)
#define PERSON_RESPONSIBLE "johndoe"

// change this to your AES encryption key (default: abcdefghijklmnop)
// encrypt passwords using
//   echo -n SECRETPASSWD | openssl enc -K 6162636465666768696a6b6c6d6e6f70 -aes-128-ecb -base64
char key[] = {0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70};

// path to config files
#define CONFIG_BASEURL "http://webserver.domain.tld/path/"

// max. animation steps
#define MAX_ANIMATION_STEPS 20
// internal log buffer size (rows)
#define LOG_BUFFER_SIZE 200

// Ethernet config for Olimex ESP32-PoE board
#define ETH_CLK_MODE ETH_CLOCK_GPIO17_OUT
#define ETH_PHY_POWER 12

// default hostname for this device
#define NODE_HOSTNAME "esp-doornode"

// defaults for GPIOs
#define MINIMUM_ACTIVE_TIME 50

// WS2812 LED defaults
#define GPIO_WS2812 33
#define WS2812_BRIGHTNESS 64
#define WS2812_GAMMA 1.0
#define WS2812_NOETH "000000 -300 ff0000 -300 000000 0"

// defaults for integrated web server
#define WEB_PORT 80
#define WEB_ACCESSPASSWD ""
#define WEB_ADMINPASSWD ""

// defaults for syslog
#define SYSLOG_HOST "syslog.domain.tld"
#define SYSLOG_PORT 514
#define SYSLOG_APPNAME "doornode"

// NTP defaults
#define TIME_CLIENT_INTERVAL 1000
