/******************************************************************************

  BonoGPS: connect a GPS to mobile Apps to track lap times
  More info at https://github.com/renatobo/bonogps
  Renato Bonomini https://github.com/renatobo

  Version with esp32_https_server:
  Flash: [========= ]  91.5% (used 1798653 bytes from 1966080 bytes)
  compare to builtin WebServer.h
  Flash: [========  ]  83.1% (used 1632974 bytes from 1966080 bytes)
  [D][bonogps.cpp:2281] setup(): Free heap: 133224

******************************************************************************/

// For PlatformIO or VS Code IDE
#include <Arduino.h>

// Enable or disable compiling features
#define UPTIME         // add library to display for how long the device has been running
#define BTSPPENABLED   // add BT-SPP stack, remove if unnecessary as it uses quite a bit of flash space
#define BLEENABLED     // add BLE stack, remove if unnecessary as it uses quite a bit of flash space
//#define ENABLE_OTA     // add code for OTA - decent memory save
#define MDNS_ENABLE    // Enable or disable mDNS - currently not working in all conditions. Small memory save to be worth removing
#define TASK_SCHEDULER // enable adv task scheduler. Small memory save to be worth removing
#define BUTTON         // Enable changing WIFI_STA and WIFI_AP via Boot button. Small memory save to be worth removing - Which button is defined below
#define SHORT_API      // URLs for commands such as enable/disable/change rate respond with a short json answer instead of a full web page

// Configure names and PIN's used for interfacing with external world
#define DEVICE_MANUFACTURER "https://github.com/renatobo"
#define DEVICE_NAME "Bono GPS"
// BONOGPS_FIRMWARE_VER is used in BLE "Firmware" data and in the /status page of web configuration
#define BONOGPS_BUILD_DATE __TIMESTAMP__
#ifdef GIT_REV
#define BONOGPS_FIRMWARE_VER GIT_REV
#else
// the following define is needed to display version when building this with the Arduino IDE
#define BONOGPS_FIRMWARE_VER "v0.3"
#endif
// Bonjour DNS name, access the GPS configuration page by appending .local as DNS
#define BONOGPS_MDNS "bonogps"
// Prefix for the AP name, followed by 4 digits device id
#define BONOGPS_AP "BonoGPS"
#define BONOGPS_PWD "bono5678"
// the AP name is generated on the fly, this is its maximum size
#define MAX_AP_NAME_SIZE 20

// How large should the RX buffer for the GPS port - more than 512 creates issues
#define UART_BUFFER_SIZE_RX 256
// Size of the intermediate buffer where we are storing the current sentence
#define MAX_UART_BUFFER_SIZE 256

// TCP Port for NMEA sentences repeater, used for Harry's LapTimer mostly, but also for proxying with uBlox
#define NMEA_TCP_PORT 1818
// Disabling GPS over NBP as it does not override phone data within TrackAddict right now, so kind of useless mostly
// #define NUMERICAL_BROADCAST_PROTOCOL
#ifdef NUMERICAL_BROADCAST_PROTOCOL
#define NEED_NEOGPS
#define NBP_TCP_PORT 35000
#endif

// Which pin controls the button to switch to STA mode
#define WIFI_MODE_BUTTON 0

// Define configuration of Bluetooth Low Energy
#define BLE_DEVICE_ID "BonoGPS"
#define BLE_SERVICE_UUID (uint16_t)0x1819        // SERVICE_LOCATION_AND_NAVIGATION_UUID
#define BLE_CHARACTERISTIC_UUID (uint16_t)0x2A67 // CHARACTERISTIC_LOCATION_AND_SPEED_CHARACTERISTIC_UUID
#define BLE_MTU 185

// GPS port on UART2
#define gpsPort Serial2
#define GPS_STANDARD_BAUD_RATE 115200
// Min size of a NMEA message
#define MIN_NMEA_MESSAGE_SIZE 6

// Define the serial monitor port
#define SerialMonitor Serial
#define LOG_BAUD_RATE 115200
// LED_BUILTIN is 2 on ESP32
#define LED_BUILTIN 2

/********************************
 * 
 * nothing you should need to configure from here below
 * 
********************************/

#if !(defined(ESP32))
#error This code is designed to run only on the ESP32 board
#endif

// Library to store desidered preferences in NVM
#include <Preferences.h>
Preferences prefs;
// data to be stored
#include "esp_wifi.h"
#include <WiFi.h>

#define WIFI_KEY_MAXLEN (int)64
#define WIFI_SSID_MAXLEN (int)32

typedef struct
{
  unsigned long gps_baud_rate = GPS_STANDARD_BAUD_RATE; // initial or current baud rate
  uint8_t gps_rate = 5;
  WiFiMode_t wifi_mode = WIFI_AP;
  bool nmeaGSA = false;
  bool nmeaGSV = false;
  bool nmeaGBS = true;
  bool ble_active = true;
  bool btspp_active = false;
  bool trackaddict = false;
  uint8_t nmeaGSAGSVpolling = 0;
  char wifi_ssid[WIFI_SSID_MAXLEN];
  char wifi_key[WIFI_KEY_MAXLEN];
} stored_preference_t;

stored_preference_t stored_preferences;

// esp32 and GPS runtime variables
uint16_t chip; // hold the device id to be used in broadcasting unit identifier strings
// WiFi runtime variables
char ap_ssid[MAX_AP_NAME_SIZE];
const char ap_password[] = BONOGPS_PWD;
int max_buffer = 0;

// for status
#ifdef UPTIME
#include "uptime_formatter.h"
#endif
bool gps_powersave = false;

// The next section is used to parse the content of messages locally instead of proxying them
// This can be used for
// - NBP: unused right now
// - Creating a custom binary packed format
#ifdef NEED_NEOGPS
// add libraries to parse GPS responses
//#define NMEAGPS_KEEP_NEWEST_FIXES true
//#define NMEAGPS_PARSING_SCRATCHPAD
//#define NMEAGPS_TIMESTAMP_FROM_INTERVAL
//#define NMEAGPS_DERIVED_TYPES
//#define NMEAGPS_PARSE_PROPRIETARY
//#define NMEAGPS_PARSE_MFR_ID
#define TIME_EPOCH_MODIFIABLE
#undef NEOGPS_PACKED_DATA
#include <GPSfix_cfg.h>
#include <NMEAGPS.h>
#if !defined(GPS_FIX_TIME)
#error You must define GPS_FIX_TIME in GPSfix_cfg.h!
#endif
#if !defined(GPS_FIX_LOCATION)
#error You must uncomment GPS_FIX_LOCATION in GPSfix_cfg.h!
#endif
#if !defined(GPS_FIX_SPEED)
#error You must uncomment GPS_FIX_SPEED in GPSfix_cfg.h!
#endif
#if !defined(GPS_FIX_ALTITUDE)
#error You must uncomment GPS_FIX_ALTITUDE in GPSfix_cfg.h!
#endif
NMEAGPS gps;
gps_fix my_fix;
#endif

#ifdef MDNS_ENABLE
#include <ESPmDNS.h>
#endif

// OTA
#ifdef ENABLE_OTA
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#endif

//for LED status and OTA updates
#ifdef TASK_SCHEDULER
#include <TaskScheduler.h>
// Scheduler for periodic tasks
Scheduler ts;
#endif

// BLueTooth classic stack (Android)
#ifdef BTSPPENABLED
#include "BluetoothSerial.h"
BluetoothSerial SerialBT;
bool bt_deviceConnected = false;
void bt_spp_stop();
void bt_spp_start();
void bt_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);
#endif

#ifdef BLEENABLED
// BLE stack
#include <NimBLEDevice.h>
// ble runtime
String ble_client = "";
bool ble_deviceConnected = false;
void ble_start();
void ble_stop();
#endif

#if defined(BLEENABLED) || defined(BTSPPENABLED)
char ble_device_id[MAX_AP_NAME_SIZE];
#endif

// Respond to button
#ifdef BUTTON
#define EASYBUTTON_DO_NOT_USE_SEQUENCES
#include <EasyButton.h>
// Instance of the button to switch wifi mode
EasyButton button(WIFI_MODE_BUTTON);
#endif

/********************************
 * 
 * Declarations of prototypes required by VS Code or PlatformIO
 * 
********************************/

void WebConfig_start();
void gps_initialize_settings();
void restart_after_sleep();
void wifi_AP();
#ifdef ENABLE_OTA
void handle_OTA();
void OTA_start();
#endif

/********************************

   Utilities

 * ******************************/
#ifdef BUILD_ENV_NAME
#define BONO_GPS_VERSION BONOGPS_FIRMWARE_VER " [" BUILD_ENV_NAME "]"
#else
#define BONO_GPS_VERSION BONOGPS_FIRMWARE_VER " [Arduino]"
#endif
/********************************

   GPS Settings
   These sentences are used to configure a uBlox series 8 device

 * ******************************/
// set rate of GPS to 5hz
const char UBLOX_INIT_5HZ[] PROGMEM = {0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, 0xC8, 0x00, 0x01, 0x00, 0x01, 0x00, 0xDE, 0x6A};
// set rate of GPS to 10Hz
const char UBLOX_INIT_10HZ[] PROGMEM = {0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, 0x64, 0x00, 0x01, 0x00, 0x01, 0x00, 0x7A, 0x12};
// set rate of GPS to 1Hz
const char UBLOX_INIT_1HZ[] PROGMEM = {0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, 0xE8, 0x03, 0x01, 0x00, 0x01, 0x00, 0x01, 0x39};
// set rate of GPS to 1Hz
const char UBLOX_INIT_16HZ[] PROGMEM = {0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, 0xE8, 0x03, 0x01, 0x00, 0x01, 0x00, 0x01, 0x39};

// Enable GxGSA (GNSS DOP and Active Satellites)
const char UBLOX_GxGSA_ON[] PROGMEM = {0xB5, 0x62, 0x06, 0x01, 0x08, 0x00, 0xF0, 0x02, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00, 0x05, 0x41};
// Disable GxGSA
const char UBLOX_GxGSA_OFF[] PROGMEM = {0xB5, 0x62, 0x06, 0x01, 0x08, 0x00, 0xF0, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x31};
// Enable GxGSV (Sat View)
const char UBLOX_GxGSV_ON[] PROGMEM = {0xB5, 0x62, 0x06, 0x01, 0x08, 0x00, 0xF0, 0x03, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00, 0x06, 0x48};
// Disable GxGSV
const char UBLOX_GxGSV_OFF[] PROGMEM = {0xB5, 0x62, 0x06, 0x01, 0x08, 0x00, 0xF0, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x38};
// set Max SVs per Talker id to 8
const char UBLOX_INIT_CHANNEL_8[] PROGMEM = {0xB5, 0x62, 0x06, 0x17, 0x14, 0x00, 0x00, 0x41, 0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7D, 0xE7};
// standard max SVs and extended digits for unsupported SVs
const char UBLOX_INIT_CHANNEL_ALL[] PROGMEM = {0xB5, 0x62, 0x06, 0x17, 0x14, 0x00, 0x00, 0x41, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0x63};
// use GP as Main talker and GSV Talker ID - this is needed for some apps like TrackAddict
const char UBLOX_INIT_MAINTALKER_GP[] PROGMEM = {0xB5, 0x62, 0x06, 0x17, 0x14, 0x00, 0x00, 0x41, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x78};
// set gps port BAUD rate
const char UBLOX_BAUD_57600[] PROGMEM = {0xB5, 0x62, 0x06, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0xD0, 0x08, 0x00, 0x00, 0x00, 0xE1, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0xDA, 0xA9};
const char UBLOX_BAUD_38400[] PROGMEM = {0xB5, 0x62, 0x06, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0xD0, 0x08, 0x00, 0x00, 0x00, 0x96, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8F, 0x70};
const char UBLOX_BAUD_115200[] PROGMEM = {0xB5, 0x62, 0x06, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0xD0, 0x08, 0x00, 0x00, 0x00, 0xC2, 0x01, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0xBC, 0x5E};
// power saving
// enable power saving mode for 1800 or 3600 seconds
// UBX - RXM - PMREQ request
const char UBLOX_PWR_SAVE_30MIN[] PROGMEM = {0xB5, 0x62, 0x02, 0x41, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x77, 0x1B, 0x00, 0x02, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x0F, 0xF6};
const char UBLOX_PWR_SAVE_1HR[] PROGMEM = {0xB5, 0x62, 0x02, 0x41, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xEE, 0x36, 0x00, 0x02, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0xE1, 0x21};
//, 0xrestart UBX-CFG-RST with controlled GNSS only software, hotstart (<4 hrs) so that ephemeris still valid
const char UBLOX_WARMSTART[] PROGMEM = {0xB5, 0x62, 0x06, 0x04, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x10, 0x68};
// Display precision in some apps
const char UBLOX_GxGBS_ON[] PROGMEM = {0xB5, 0x62, 0x06, 0x01, 0x08, 0x00, 0xF0, 0x09, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00, 0x0C, 0x72};
const char UBLOX_GxGBS_OFF[] PROGMEM = {0xB5, 0x62, 0x06, 0x01, 0x08, 0x00, 0xF0, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x62};
// Poll GSA
const char UBLOX_GNGSA_POLL[] PROGMEM = "$EIGNQ,GSA*2D\r\n";
// Poll GSV
const char UBLOX_GNGSV_POLL[] PROGMEM = "$EIGNQ,GSV*3A\r\n";
bool gsaorgsv_turn = true;

void push_gps_message(const char message[], int message_size = 0)
{
  if (gps_powersave)
  {
    log_d("Disable powermode");
    gpsPort.end();
    delay(1000);
    // assumes stored_preferences global
    gps_powersave = false;
    gps_initialize_settings();
  }
  for (int i = 0; i < message_size; i++)
  {
    gpsPort.write(message[i]);
  }
}

#ifdef TASK_SCHEDULER
void poll_GSA_GSV_info()
{
  if (gsaorgsv_turn)
  {
    gpsPort.write(UBLOX_GNGSA_POLL);
  }
  else
  {
    gpsPort.write(UBLOX_GNGSV_POLL);
  }
  gsaorgsv_turn = !gsaorgsv_turn;
}

Task tpoll_GSA_GSV_info(0, TASK_FOREVER, poll_GSA_GSV_info, &ts, false);
void control_poll_GSA_GSV(int frequency)
{
  if (frequency > 0)
  {
    push_gps_message(UBLOX_GxGSV_OFF, sizeof(UBLOX_GxGSV_OFF));
    push_gps_message(UBLOX_GxGSA_OFF, sizeof(UBLOX_GxGSA_OFF));
    tpoll_GSA_GSV_info.setInterval(frequency * 500);
    tpoll_GSA_GSV_info.enable();
  }
  else
  {
    tpoll_GSA_GSV_info.disable();
  }
}

// handle restart when config is lost
Task trestart_after_sleep(0, 1, restart_after_sleep, &ts, false);
void restart_after_sleep()
{
  gps_powersave = false;
  push_gps_message(UBLOX_WARMSTART, sizeof(UBLOX_WARMSTART));
  delay(1000);
  gpsPort.end();
  delay(1000);
  // assumes 'stored_preferences' is global
  gps_initialize_settings();
  trestart_after_sleep.disable();
}
#endif

void switch_baudrate(uint32_t newbaudrate)
{
  // stored_preferences assumed global
  log_d("Send UBX-CFG for rate %d", newbaudrate);
  switch (newbaudrate)
  {
  case 38400:
    push_gps_message(UBLOX_BAUD_38400, sizeof(UBLOX_BAUD_38400));
    break;
  case 57600:
    push_gps_message(UBLOX_BAUD_57600, sizeof(UBLOX_BAUD_57600));
    break;
  case 115200:
    push_gps_message(UBLOX_BAUD_115200, sizeof(UBLOX_BAUD_115200));
    break;
  default:
    push_gps_message(UBLOX_BAUD_38400, sizeof(UBLOX_BAUD_38400));
    break;
  }
  log_d("Flush UART");
  gpsPort.flush();
  delay(500);
  log_d("end connection UART");
  gpsPort.end();
  delay(100);
  log_d("Start new connection UART");
  gpsPort.begin(newbaudrate);
  stored_preferences.gps_baud_rate = newbaudrate;
}

// read from NVM via preferences library
void ReadNVMPreferences()
{
  // prefs assumed global
  // stored_preferences assumed global
  String string_wifi_mode;

  switch (stored_preferences.wifi_mode)
  {
  case WIFI_AP:
    string_wifi_mode = "WIFI_AP";
    break;
  case WIFI_STA:
    string_wifi_mode = "WIFI_STA";
    break;
  default:
    string_wifi_mode = "WIFI_STA";
    break;
  }

  stored_preferences.gps_baud_rate = prefs.getULong("gpsbaudrate", stored_preferences.gps_baud_rate);
  stored_preferences.gps_rate = prefs.getUChar("gpsrate", stored_preferences.gps_rate);
  stored_preferences.nmeaGSAGSVpolling = prefs.getUChar("gsagsvpoll", stored_preferences.nmeaGSAGSVpolling);
  // wifi mode : remember https://www.esp32.com/viewtopic.php?t=12085
  string_wifi_mode = prefs.getString("wifi");
  if (string_wifi_mode == "WIFI_STA")
  {
    stored_preferences.wifi_mode = WIFI_STA;
    log_d("Preference read as WIFI_STA");
  }
  else
  {
    stored_preferences.wifi_mode = WIFI_AP;
    log_d("Preference read as WIFI_AP");
  }

  // this being a char*, we pass size
  prefs.getString("wifikey", stored_preferences.wifi_key, WIFI_KEY_MAXLEN);
  prefs.getString("wifissid", stored_preferences.wifi_ssid, WIFI_SSID_MAXLEN);
  stored_preferences.nmeaGSA = prefs.getBool("nmeagsa", stored_preferences.nmeaGSA);
  stored_preferences.nmeaGSV = prefs.getBool("nmeagsv", stored_preferences.nmeaGSV);
  stored_preferences.nmeaGBS = prefs.getBool("nmeagbs", stored_preferences.nmeaGBS);
  stored_preferences.ble_active = prefs.getBool("ble", stored_preferences.ble_active);
  stored_preferences.btspp_active = prefs.getBool("btspp", stored_preferences.btspp_active);
  stored_preferences.trackaddict = prefs.getBool("trackaddict", stored_preferences.trackaddict);

  if (stored_preferences.ble_active && stored_preferences.btspp_active)
  {
    // prevents both settings to be ON
#ifdef BLEENABLED
    stored_preferences.btspp_active = false;
#else
    stored_preferences.ble_active = false;
#endif
  }
}

// write to preferences
void StoreNVMPreferences(bool savewifi = false)
{
  // prefs assumed global
  // stored_preferences assumed global
  String string_wifi_mode;
  size_t size_t_written;

  if (savewifi)
  {
    switch (stored_preferences.wifi_mode)
    {
    case WIFI_AP:
      string_wifi_mode = "WIFI_AP";
      break;
    case WIFI_STA:
      string_wifi_mode = "WIFI_STA";
      break;
    default:
      string_wifi_mode = "WIFI_STA";
      break;
    }
    size_t_written = prefs.putString("wifi", string_wifi_mode);
  }
  size_t_written = prefs.putString("wifikey", stored_preferences.wifi_key);
  size_t_written = prefs.putString("wifissid", stored_preferences.wifi_ssid);

  size_t_written = prefs.putULong("gpsbaudrate", stored_preferences.gps_baud_rate);
  size_t_written = prefs.putUChar("gpsrate", stored_preferences.gps_rate);
  size_t_written = prefs.putUChar("gsagsvpoll", stored_preferences.nmeaGSAGSVpolling);
  size_t_written = prefs.putBool("nmeagsa", stored_preferences.nmeaGSA);
  size_t_written = prefs.putBool("nmeagsv", stored_preferences.nmeaGSV);
  size_t_written = prefs.putBool("nmeagbs", stored_preferences.nmeaGBS);
  size_t_written = prefs.putBool("ble", stored_preferences.ble_active);
  size_t_written = prefs.putBool("btspp", stored_preferences.btspp_active);
  size_t_written = prefs.putBool("trackaddict", stored_preferences.trackaddict);
  if (size_t_written > 0)
  {
    log_i("Preferences written");
  }
  else
  {
    log_e("Preferences NOT written");
  }
}

void StoreNVMPreferencesWiFi(String string_wifi_mode)
{
  // prefs assumed global
  size_t size_t_written;
  size_t_written = prefs.putString("wifi", string_wifi_mode);
  if (size_t_written > 0)
  {
    log_i("Preferences %s written", string_wifi_mode);
  }
  else
  {
    log_e("Preferences %s NOT written", string_wifi_mode);
  }
}

void StoreNVMPreferencesWiFiCreds()
{
  // prefs assumed global
  size_t size_t_written;
  size_t_written = prefs.putString("wifikey", stored_preferences.wifi_key);
  if (size_t_written > 0)
  {
    log_i("WiFi Key written (size %d)", size_t_written);
  }
  else
  {
    log_e("WiFi Key NOT written");
  }
  size_t_written = prefs.putString("wifissid", stored_preferences.wifi_ssid);
  if (size_t_written > 0)
  {
    log_i("WiFi SSID written (size %d)", size_t_written);
  }
  else
  {
    log_e("WiFi SSID NOT written");
  }
}

void gps_enable_trackaddict()
{
  log_d("Setting GPS to specific Track Addict needs");
  push_gps_message(UBLOX_GxGSA_OFF, sizeof(UBLOX_GxGSA_OFF));
  stored_preferences.nmeaGSA = false;
  push_gps_message(UBLOX_GxGSV_OFF, sizeof(UBLOX_GxGSA_OFF));
  stored_preferences.nmeaGSV = false;
  push_gps_message(UBLOX_GxGBS_OFF, sizeof(UBLOX_GxGBS_OFF));
  stored_preferences.nmeaGBS = false;
  push_gps_message(UBLOX_INIT_MAINTALKER_GP, sizeof(UBLOX_INIT_MAINTALKER_GP));
  stored_preferences.trackaddict = true;
  push_gps_message(UBLOX_INIT_10HZ, sizeof(UBLOX_INIT_10HZ));
  stored_preferences.gps_rate = 10;
#ifdef TASK_SCHEDULER
  control_poll_GSA_GSV(0);
#endif
  stored_preferences.nmeaGSAGSVpolling = 0;
}

#ifdef NUMERICAL_BROADCAST_PROTOCOL
const uint NBPServerPort = NBP_TCP_PORT;
WiFiServer NBPServer(NBPServerPort);
WiFiClient NBPRemoteClient;
#endif

const uint NMEAServerPort = NMEA_TCP_PORT;
WiFiServer NMEAServer(NMEAServerPort);
WiFiClient NMEARemoteClient;

bool ledon;
void led_blink()
{
  //toggle state
  ledon = !ledon;
  digitalWrite(LED_BUILTIN, (ledon ? HIGH : LOW)); // set pin to the opposite state
}

#ifdef TASK_SCHEDULER
Task tLedBlink(0, TASK_FOREVER, &led_blink, &ts, false);
#endif

/********************************

    WiFi connection

 * ******************************/
const char content_type[] PROGMEM = "Content-Type";
const char html_text[] PROGMEM = "text/html";
const char text_json[] PROGMEM = "application/json";
const char json_ok[] PROGMEM = "{'status':'ok'}";
const char json_error[] PROGMEM = "{'status':'error'}";
bool wifi_connected = false;
IPAddress MyIP;

void NMEACheckForConnections()
{
  if (NMEAServer.hasClient())
  {
    // If we are already connected to another computer, then reject the new connection. Otherwise accept the connection.
    if (NMEARemoteClient.connected())
    {
      log_w("NMEA TCP Connection rejected");
      NMEAServer.available().stop();
    }
    else
    {
      NMEARemoteClient = NMEAServer.available();
      // TODO not printing strings correctly
      log_d("NMEA TCP Connection accepted from client: %s", NMEARemoteClient.remoteIP().toString());
    }
  }
}
void start_NMEA_server()
{
  log_i("Start GNSS TCP/IP Service");
  NMEAServer.begin();
  NMEAServer.setNoDelay(true);
}

#ifdef NUMERICAL_BROADCAST_PROTOCOL
unsigned long nbptimer = 0;
void NBPCheckForConnections()
{
  if (NBPServer.hasClient())
  {
    // If we are already connected to another computer, then reject the new connection. Otherwise accept the connection.
    if (NBPRemoteClient.connected())
    {
      log_w("NBP TCP Connection rejected");
      NBPServer.available().stop();
    }
    else
    {
      NBPRemoteClient = NBPServer.available();
      log_i("NBP TCP Connection accepted from client: %s", NBPRemoteClient.remoteIP().toString());
    }
  }
}
void start_NBP_server()
{
  log_i("Start NBP TCP/IP Service");
  NBPServer.begin();
  NBPServer.setNoDelay(true);
}
#endif

// Start STATION mode to connect to a well-known Access Point
void wifi_STA()
{
  // WiFi Access
  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
  // start ticker_wifi with 50 ms because we start in STA mode and try to connect
#ifdef TASK_SCHEDULER
  log_d("Start rapid blinking");
  tLedBlink.setInterval(50);
  tLedBlink.enable();
#endif

  // TODO : add connection to STA mode with stored passwords here
  WiFi.begin(stored_preferences.wifi_ssid, stored_preferences.wifi_key);
  wifi_connected = false;

  int times = 0;
  while (WiFi.status() != WL_CONNECTED && times < 50)
  {
    delay(100);
    log_i("Connecting to WiFi %s , trial %d", stored_preferences.wifi_ssid, times++);
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    log_i("Connected to SSID %s", stored_preferences.wifi_ssid);
    tLedBlink.setInterval(250);
    wifi_connected = true;
#ifdef ENABLE_OTA
    log_i("Start OTA service");
    OTA_start();
#endif
    log_i("Start Web Portal");
    WebConfig_start();
    // WLAN Server for GNSS data
    start_NMEA_server();
#ifdef NUMERICAL_BROADCAST_PROTOCOL
    // WLAN Server for GNSS data
    start_NBP_server();
#endif

    MyIP = WiFi.localIP();

#ifdef BUTTON
    button.onPressed(wifi_AP);
#endif
    stored_preferences.wifi_mode = WIFI_STA;
  }
  else
  {
    log_e("Could not connect to SSID %s, reverting to AP mode", stored_preferences.wifi_ssid);
    wifi_AP();
  }
}

void wifi_AP()
{
  // WiFi Access
  WiFi.mode(WIFI_AP); // explicitly set mode, esp defaults to STA+AP
  // Set WiFi options
  wifi_country_t country = {
      .cc = "US",
      .schan = 1,
      .nchan = 13,
      .max_tx_power = 20,
      .policy = WIFI_COUNTRY_POLICY_AUTO,
  };
  esp_wifi_set_country(&country);
  WiFi.softAP(ap_ssid, ap_password, 1, 0, 2);
  log_i("Wait 100 ms for AP_START...");
  delay(100);
  log_d("Power: %d", WiFi.getTxPower());

  log_i("Set softAPConfig");
  IPAddress Ip(10, 0, 0, 1);
  IPAddress NMask(255, 255, 255, 0);
  WiFi.softAPConfig(Ip, Ip, NMask);

  MyIP = WiFi.softAPIP();
  log_i("AP IP address: %s", MyIP.toString());

  wifi_connected = true;

#ifdef MDNS_ENABLE
  if (!MDNS.begin(BONOGPS_MDNS))
  {
    log_e("Error setting up MDNS responder!");
  }
  else
  {
    log_i("mDNS responder started");
  }
#endif

  log_i("Start Web Portal");
  WebConfig_start();

#ifdef TASK_SCHEDULER
  log_d("Start blinking");
  tLedBlink.setInterval(1000);
  tLedBlink.enable();
#endif

  // WLAN Server for GNSS data
  start_NMEA_server();
#ifdef NUMERICAL_BROADCAST_PROTOCOL
  // WLAN Server for GNSS data
  start_NBP_server();
#endif

#ifdef BUTTON
  button.onPressed(wifi_STA);
#endif
  stored_preferences.wifi_mode = WIFI_AP;
}

/********************************

   Web Configuration portal

 * ******************************/
#if CONFIG_FREERTOS_UNICORE
#define ARDUINO_RUNNING_CORE 0
#else
#define ARDUINO_RUNNING_CORE 1
#endif

#include <HTTPServer.hpp>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>

using namespace httpsserver;

HTTPServer webserver = HTTPServer();

// Helper to populate a page with style sheet
String generate_html_header(bool add_menu = true) 
{
  String htmlbody((char *)0);
  htmlbody.reserve(500);
  htmlbody += F("<!DOCTYPE html><html lang='en'><head><title>");
  htmlbody += ap_ssid;
  htmlbody += F("</title><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><link rel='stylesheet' href='/css'></head><body><script>function Select(e){fetch('/'+e).then(e=>e.text()).then(t=>console.log(e))}</script><header>");
  htmlbody += (add_menu ? "Back to <a href='/'>Main menu</a>" : ap_ssid);
  htmlbody += (gps_powersave ? "<br><em>Powersave mode</em>" : "");
  htmlbody += F("</header>\n");
  return htmlbody;
}
String generate_html_footer() 
{
  String htmlbody((char *)0);
  htmlbody.reserve(300);
#ifdef GIT_REPO
  htmlbody += F("<footer>Version: <a style='font-size: small;background: none;text-decoration: underline;' target='_blank' href='");
  htmlbody += GIT_REPO;
  htmlbody += "'>";
  htmlbody += BONO_GPS_VERSION;
  htmlbody += F("</a></footer></body></html>");
#else
  htmlbody += F("<footer>Version: " BONO_GPS_VERSION "</footer></body></html>");
#endif
  return htmlbody;
}
String generate_html_body(String input, bool add_menu = true)
{
  // assumption: ap_ssid is populated
  String htmlbody((char *)0);
  htmlbody.reserve(5000);
  htmlbody += generate_html_header(add_menu);
  htmlbody += input;
  htmlbody += generate_html_footer();
  return htmlbody;
}

String input_onoff(String divlabel, String parameter, bool parameter_status)
{
  String message((char *)0);
  message.reserve(500);
  message += F("\n<article class='bg'>");
  message += divlabel;
  message += F("\n\t<input type='radio' id='");
  message += parameter;
  message += F("/on' onchange='Select(this.id)' name='");
  message += parameter;
  if (parameter_status)
  {
    message += "' checked>";
  }
  else
  {
    message += "'>";
  }
  message += F("\n\t<label class='button' for='");
  message += parameter;
  message += F("/on'> On </label>\n\t<input type='radio' id='");
  message += parameter;
  message += F("/off' onchange='Select(this.id)' name='");
  message += parameter;
  if (parameter_status)
  {
    message += "'>";
  }
  else
  {
    message += "' checked>";
  }
  message += F("\n\t<label class='button' for='");
  message += parameter;
  message += F("/off'> Off </label>\n\t</article>");
  return message;
}

String html_select(String divlabel, String parameters[], String parameters_names[], bool parameters_status[], String groupname, int numberparams)
{
  String message((char *)0);
  message.reserve(500);
  message += F("\n<article class='bg'>");
  message += divlabel;
  for (int i = 0; i < numberparams; i++)
  {
    message += F("\n\t<input type='radio' id='");
    message += groupname;
    message += "/";
    message += parameters[i];
    message += F("' onchange='Select(this.id)' name='");
    message += groupname;
    if (parameters_status[i])
    {
      message += "' checked>";
    }
    else
    {
      message += "'>";
    }
    message += F("\n\t<label class='button' for='");
    message += groupname;
    message += "/";
    message += parameters[i];
    message += "'> ";
    message += parameters_names[i];
    message += F(" </label>");
  }
  message += "\n</article>";
  return message;
}

void handle_css(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Handle CSS response");
  res->setHeader("Cache-Control", "max-age=3600");
  res->setHeader("Content-Type", "text/css");
  res->println(F("*{box-sizing:border-box;text-align:center;width:100%;font-weight:300}body{font-family:Roboto,system-ui,Arial,Helvetica,sans-serif;font-size:5vw;margin:0}header{background-color:#666;padding:.5vw;text-align:center;color:#fff}article{float:left;padding:10px;width:100%;height:auto}details{display:table;clear:both}summary{font-size:larger;font-weight:400;padding:10px;background-color:#f1f1f1}footer{background-color:#777;padding:.2vw;text-align:center;color:#fff;clear:both;position:fixed;bottom:0;font-size:small}@media (min-width:800px){article{width:50%}*{font-size:2.5vw}}a,input{color:#fff;border-radius:8pt;background:red;text-decoration:none;padding:5pt}.bg input{display:none}label{border:solid;border-radius:8pt;padding:5px;margin:2px;border-color:#bdbdbd;border-width:2px;color:#9e9e9e}.bg input:checked+label,.bg input:checked+label:active{background:red;color:#fff;border-color:red}"));
}
void handle_menu(HTTPRequest *req, HTTPResponse *res)
{
  // variables used to build html buttons
  String sv_values[2] = {"8", "all"};
  String sv_names[2] = {"8", "All"};
  bool sv_status[2] = {false, true};

  String baud_values[3] = {"38400", "57600", "115200"};
  String baud_names[3] = {"38k4", "57k6", "115k2"};
  bool baud_status[3] = {(stored_preferences.gps_baud_rate == 38400), (stored_preferences.gps_baud_rate == 57600), (stored_preferences.gps_baud_rate == 115200)};

  String wifi_values[2] = {"sta", "ap"};
  String wifi_names[2] = {"Client", "AP"};
  bool wifi_status[2] = {(stored_preferences.wifi_mode == WIFI_STA), (stored_preferences.wifi_mode == WIFI_AP)};

#ifdef TASK_SCHEDULER
  String pollgsagsv_values[3] = {"0", "1", "5"};
  String pollgsagsv_names[3] = {"Off", "1 sec", "5 sec"};
  bool pollgsagsv_status[3] = {(stored_preferences.nmeaGSAGSVpolling == 0), (stored_preferences.nmeaGSAGSVpolling == 1), (stored_preferences.nmeaGSAGSVpolling == 5)};
#endif

  log_i("Handle webserver root response");
  res->setHeader(content_type, html_text);
  res->println(generate_html_header(false));
  res->println(F("\n<details open><summary>GPS runtime settings</summary>\n<article class='bg'>Update\n<input type='radio' id='rate/1hz' onchange='Select(this.id)' name='rate'"));
  res->println((stored_preferences.gps_rate == 1) ? "checked" : " ");
  res->println(F("><label class='button' for='rate/1hz'>1 Hz</label>\n<input type='radio' id='rate/5hz' onchange='Select(this.id)' name='rate' "));
  res->println((stored_preferences.gps_rate == 5) ? "checked" : " ");
  res->println(F("><label class='button' for='rate/5hz'>5 Hz</label>\n<input type='radio' id='rate/10hz' onchange='Select(this.id)' name='rate' "));
  res->println((stored_preferences.gps_rate == 10) ? "checked" : " ");
  res->println(F("><label class='button' for='rate/10hz'>10 Hz</label></article>"));
  res->println(input_onoff("Stream GxGBS", "gbs", stored_preferences.nmeaGBS));
  res->println(input_onoff("Stream GxGSA", "gsa", stored_preferences.nmeaGSA));
  res->println(input_onoff("Stream GxGSV", "gsv", stored_preferences.nmeaGSV));
#ifdef TASK_SCHEDULER
  res->println(html_select("Poll GSA + GSV ", pollgsagsv_values, pollgsagsv_names, pollgsagsv_status, String("poll/gsagsv"), 3));
#endif
  res->println(html_select("SVs per Talker Id", sv_values, sv_names, sv_status, String("sv"), 2));
  res->println(html_select("Port BAUD", baud_values, baud_names, baud_status, String("baud"), 3));
  res->println(F("</details>\n<details>\n<summary>Connections</summary><article>List connected <a href='/clients'>clients</a></article>"));
  res->println(html_select("WiFi mode ", wifi_values, wifi_names, wifi_status, String("wifi"), 2));
#ifdef BLEENABLED
  res->println(input_onoff("Bluetooth LE", "ble", stored_preferences.ble_active));
#endif
#ifdef BTSPPENABLED
  res->println(input_onoff("Bluetooth SPP", "btspp", stored_preferences.btspp_active));
#endif
  res->println(F("\n</details>\n<details><summary>Device</summary>\n<article>Suspend GPS for <a href='/powersave/1800'>30'</a> <a href='/powersave/3600'>1 hr</a></article>\n<article><a href='/preset'>Load Preset</a></article>\n<article><a href='/status'>Information</a></article>\n<article><a href='/restart'>Restart</a></article>\n<article><a href='/savecfg'>Save config</a></article>\n<article><a href='/savecfg/wifi/creds'>Save WiFi credentials</a></article></details>"));
  res->println(generate_html_footer());
  log_i("sending webserver root response");
}

void handle_preset(HTTPRequest *req, HTTPResponse *res)
{
  String mainpage((char *)0);
  mainpage.reserve(2000);
#if defined(BTSPPENABLED) || defined(BLEENABLED)
  // hlt main page
#if defined(BTSPPENABLED) && defined(BLEENABLED)
  mainpage += F("<details open><summary>Harry Lap Timer <a target='_blank' href='https://www.gps-laptimer.de/'>?</a></summary><article>Recommended options:<br><ul><li>GBS streaming</li><li>GSA GSV polling</li><li>10 Hz updates</li><li>Android: BT-SPP Connection</li><li>iOS: BT-LE Connection</li></ul></article><article>Load options for:<p><a href='/hlt/tcpip'>WiFi</a></p><a href='/hlt/ios'>iOS</a></p><p><a href='/hlt/android'>Android</a></p><p>A save config and restart are recommended after enabling/disabling BT-SPP</p></article></details>");
#else
#ifdef BLEENABLED
  mainpage += F("<details open><summary>Harry Lap Timer <a target='_blank' href='https://www.gps-laptimer.de/'>?</a></summary><article>Recommended options:<br><ul><li>GBS streaming</li><li>GSA GSV polling</li><li>10 Hz updates</li><li>iOS: BT-LE Connection</li></ul></article><article>Load  options forn:<p><a href='/hlt/ios'>iOS</a></article></details>");
#endif
#ifdef BTSPPENABLED
  mainpage += F("<details open><summary>Harry Lap Timer <a href='https://www.gps-laptimer.de/'>?</a></summary><article>Recommended options:<br><ul><li>GBS streaming</li><li>GSA GSV polling</li><li>10 Hz updates</li><li>Android: BT-SPP Connection</li></ul></article><article><Load options for:<p><a href='/hlt/android'>Android</a></p><p>A save config and restart are recommended after enabling/disabling BT-SPP</article></details>");
#endif
#endif // defined(BTSPPENABLED) && defined(BLEENABLED)

#ifdef BTSPPENABLED
  // racechrono main page
  mainpage += F("<details open><summary>RaceChrono <a target='_blank' href='https://racechrono.com/'>?</a></summary><article>Recommended options:<br><ul><li>Talker id GPS for all systems</li><li>no GSA GSV GBS streaming</li><li>no GSA GSV polling</li><li>10 Hz updates</li><li>BT-SPP Connection only</li></ul></article><article><p>Load options for:<p><a href='/racechrono/android'>Android</a></p></article></details>");

  // trackaddict main page
  mainpage += F("<details open><summary>TrackAddict <a target='_blank' href='https://www.hptuners.com/product/trackaddict-app/'>?</a></summary><article>Required options:<br><ul><li>Talker id GPS for all systems</li><li>no GSA GSV GBS streaming</li><li>no GSA GSV polling</li><li>10 Hz updates</li><li>BT-SPP Connection only</li></ul></article>");
  mainpage += input_onoff("Android", "trackaddict", stored_preferences.trackaddict);
  mainpage += F("<article><p>A save config and restart are recommended after enabling/disabling BT-SPP</p></article></details>");

#endif

#else
  String mainpage = "No settings available with this firmware options";
#endif
  log_i("Handle load preset");
  res->setHeader(content_type, html_text);
  res->println(generate_html_header(true));
  res->println(mainpage);
  res->println(generate_html_footer());
}

#ifdef BLEENABLED
void handle_ble_off(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Turning off BLE");
  stored_preferences.ble_active = false;
  ble_stop();
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  res->setHeader(content_type, html_text);res->println(generate_html_body(F("Turn <b>off</b> BLE<br><a href='/savecfg/nowifi'>Save settings</a>")));
#endif
}
void handle_ble_on(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Turning on BLE");
  stored_preferences.ble_active = true;
#ifdef BTSPPENABLED
  stored_preferences.btspp_active = false;
  bt_spp_stop();
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Turned <b>ON</b> BLE, and turned <b>OFF</b> BT-SPP<p>This causes a restart of the device: to store permanently, disable BT-SPP, save config, enable BLE")));
#endif
#else
#ifdef SHORT_API
  res->setHeader(content_type, text_json);res->println(json_ok);
#else
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Turned <b>ON</b> BLE<br><a href='/savecfg/nowifi'>Save settings</a>")));
#endif
#endif
  ble_start();
}
#endif
#ifdef BTSPPENABLED
void handle_btspp_off(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Turning off BT-SPP");
  stored_preferences.btspp_active = false;
  bt_spp_stop();
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Turn <b>off</b> BLE")));
#endif
}
void handle_btspp_on(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Turning on BT-SPP");
  stored_preferences.btspp_active = true;
#ifdef BLEENABLED
  stored_preferences.ble_active = false;
  ble_stop();
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Turned <b>ON</b> BLE, and turned <b>OFF</b> BT-SPP<br><a href='/savecfg/nowifi'>Save settings</a>")));
#endif
#else
#ifdef SHORT_API
  res->setHeader(content_type, text_json);res->println(json_ok);
#else
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Turned <b>ON</b> BT-SPP")));
#endif
#endif
  bt_spp_start();
}
#endif
void handle_powersave_1hr(HTTPRequest *req, HTTPResponse *res)
{
  log_i("enable power saving mode for 3600 seconds");
  // disable polling of GSA and GSV
#ifdef TASKSCHEDULER
  control_poll_GSA_GSV(0);
  trestart_after_sleep.setInterval(3600000);
  trestart_after_sleep.enableDelayed();
#endif
  push_gps_message(UBLOX_PWR_SAVE_1HR, sizeof(UBLOX_PWR_SAVE_1HR));
  gps_powersave = true;
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Enabled power saving mode for 3600 seconds")));
#endif
}
void handle_powersave_30min(HTTPRequest *req, HTTPResponse *res)
{
  log_i("enable power saving mode for 1800 seconds");
  // disable polling of GSA and GSV
#ifdef TASKSCHEDULER
  control_poll_GSA_GSV(0);
  trestart_after_sleep.setInterval(1800000);
  trestart_after_sleep.enableDelayed();
#endif
  push_gps_message(UBLOX_PWR_SAVE_30MIN, sizeof(UBLOX_PWR_SAVE_30MIN));
  gps_powersave = true;
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Enabled power saving mode for 1800 seconds")));
#endif
}
static const char savecfg[] PROGMEM = "<br><a href='/savecfg/nowifi'>Save settings</a>";
void handle_rate_1hz(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Set GPS Rate to 1 Hz");
  push_gps_message(UBLOX_INIT_1HZ, sizeof(UBLOX_INIT_1HZ));
  stored_preferences.gps_rate = 1;
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Rate set to <b>1 Hz</b>");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}
void handle_rate_5hz(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Set GPS Rate to 5 Hz");
  push_gps_message(UBLOX_INIT_5HZ, sizeof(UBLOX_INIT_5HZ));
  stored_preferences.gps_rate = 5;
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Rate set to <b>5 Hz</b>");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}
void handle_rate_10hz(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Set GPS Rate to 10 Hz");
  push_gps_message(UBLOX_INIT_10HZ, sizeof(UBLOX_INIT_10HZ));
  stored_preferences.gps_rate = 10;
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Rate set to <b>10 Hz</b>");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}
void handle_baudrate_38400(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Set BAUD Rate to 38400");
  switch_baudrate(38400);
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Baud set to <b>38k4</b>");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}
void handle_baudrate_57600(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Set BAUD Rate to 57600");
  switch_baudrate(57600);
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Baud set to <b>57k6</b>");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}
void handle_baudrate_115200(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Set BAUD Rate to 115200");
  switch_baudrate(115200);
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(71);
  message += F("Baud set to <b>115k2</b>");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}
void handle_gsv_on(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Enable GxGSV messages");
#ifdef TASKSCHEDULER
  control_poll_GSA_GSV(0);
#endif
  push_gps_message(UBLOX_GxGSV_ON, sizeof(UBLOX_GxGSV_ON));
  stored_preferences.nmeaGSV = true;
  stored_preferences.nmeaGSAGSVpolling = 0;
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Enabled GxGSV messages");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}
void handle_gsv_off(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Disable GxGSV messages");
  push_gps_message(UBLOX_GxGSV_OFF, sizeof(UBLOX_GxGSV_OFF));
  stored_preferences.nmeaGSV = false;
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(71);
  message += F("Disabled GxGSV messages");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}
void handle_gsa_on(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Enable GxGSA messages");
#ifdef TASKSCHEDULER
  control_poll_GSA_GSV(0);
#endif
  push_gps_message(UBLOX_GxGSA_ON, sizeof(UBLOX_GxGSA_ON));
  stored_preferences.nmeaGSA = true;
  stored_preferences.nmeaGSAGSVpolling = 0;
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Enabled GxGSA messages");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}
void handle_gsa_off(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Disable GxGSA messages");
  push_gps_message(UBLOX_GxGSA_OFF, sizeof(UBLOX_GxGSA_OFF));
  stored_preferences.nmeaGSA = false;
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Disable GxGSA messages");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}
#ifdef TASK_SCHEDULER
void handle_pollgsagsv_on_1(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Enable polling of GSA and GSV messages every 1 sec");
  control_poll_GSA_GSV(1);
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Disable GxGSA messages");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif

  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Enabled polling of alternate GSA and GSV messages every 1 second")));
}
void handle_pollgsagsv_on_5(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Enable polling of GSA and GSV messages every 5 sec");
  control_poll_GSA_GSV(5);
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Disable GxGSA messages");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif

  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Enabled polling of alternate GSA and GSV messages every 5 seconds")));
}
void handle_pollgsagsv_off(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Disable polling of GSA and GSV messages every");
  control_poll_GSA_GSV(0);
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Disable GxGSA messages");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif

  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Disabled polling of alternate GSA and GSV messages")));
}
#endif

void handle_gbs_on(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Enable GxGBS messages");
  push_gps_message(UBLOX_GxGBS_ON, sizeof(UBLOX_GxGBS_ON));
  stored_preferences.nmeaGBS = true;
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Disable GxGSA messages");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif

  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Enabled GxGBS messages<br><a href='/savecfg/nowifi'>Save settings</a>")));
}
void handle_gbs_off(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Disable GxGBS messages");
  push_gps_message(UBLOX_GxGBS_OFF, sizeof(UBLOX_GxGBS_OFF));
  stored_preferences.nmeaGBS = false;
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Disable GxGSA messages");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif

  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Disabled GxGBS messages<br><a href='/savecfg/nowifi'>Save settings</a>")));
}
void handle_svchannel_8(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Restric to 8 SVs per Talker Id");
  push_gps_message(UBLOX_INIT_CHANNEL_8, sizeof(UBLOX_INIT_CHANNEL_8));
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("NMEA  8 SVs per Talker");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}
void handle_svchannel_all(HTTPRequest *req, HTTPResponse *res)
{
  log_i("All SVs per Talker Id");
  push_gps_message(UBLOX_INIT_CHANNEL_ALL, sizeof(UBLOX_INIT_CHANNEL_ALL));
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("All SVs ");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}

#ifdef BTSPPENABLED
void handle_trackaddict_on(HTTPRequest *req, HTTPResponse *res)
{
  // /trackaddict/on
  log_i("Set optimal configuration for Track Addict");
  gps_enable_trackaddict();
#ifdef BLEENABLED
  stored_preferences.ble_active = false;
  ble_stop();
#endif
  bt_spp_start();
  stored_preferences.btspp_active = true;
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("Set TrackAddict ");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}
void handle_trackaddict_off(HTTPRequest *req, HTTPResponse *res)
{
  // /trackaddict/off
  log_i("Unset optimal configuration for Track Addict");
  stored_preferences.trackaddict = false;
  if (stored_preferences.nmeaGSA)
    push_gps_message(UBLOX_GxGSA_ON, sizeof(UBLOX_GxGSA_ON));
  if (stored_preferences.nmeaGSV)
    push_gps_message(UBLOX_GxGSV_ON, sizeof(UBLOX_GxGSV_ON));
  if (stored_preferences.nmeaGBS)
    push_gps_message(UBLOX_GxGBS_ON, sizeof(UBLOX_GxGBS_ON));
#ifdef TASK_SCHEDULER
  control_poll_GSA_GSV(stored_preferences.nmeaGSAGSVpolling);
#endif
  push_gps_message(UBLOX_INIT_CHANNEL_ALL, sizeof(UBLOX_INIT_CHANNEL_ALL));
#ifdef SHORT_API
  res->setHeader(content_type, text_json);
  res->println(json_ok);
#else
  String message((char *)0);
  message.reserve(70);
  message += F("UnSet TrackAddict ");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
#endif
}
#endif

#ifdef BTSPPENABLED
void handle_racechrono_android(HTTPRequest *req, HTTPResponse *res)
{
  // /hlt/android
  log_i("Setting optimal configuration for RaceChrono on Android: 10Hz, GSA+GSV Off, GBS On, BT-SPP");
  push_gps_message(UBLOX_INIT_10HZ, sizeof(UBLOX_INIT_10HZ));
  stored_preferences.gps_rate = 10;
  stored_preferences.nmeaGSA = false;
  stored_preferences.nmeaGSV = false;
#ifdef TASK_SCHEDULER
  stored_preferences.nmeaGSAGSVpolling = 0;
  control_poll_GSA_GSV(0);
#else
  // control_poll will disable GSA and GSV on its own
  push_gps_message(UBLOX_GxGSA_OFF, sizeof(UBLOX_GxGSA_OFF));
  push_gps_message(UBLOX_GxGSV_OFF, sizeof(UBLOX_GxGSV_OFF));
#endif
  push_gps_message(UBLOX_GxGBS_ON, sizeof(UBLOX_GxGBS_ON));
  stored_preferences.nmeaGBS = true;
  push_gps_message(UBLOX_INIT_CHANNEL_ALL, sizeof(UBLOX_INIT_CHANNEL_ALL));
  log_i("Set optimal configuration for RaceChrono on Android");
  stored_preferences.trackaddict = false;
#ifdef BLEENABLED
  stored_preferences.ble_active = false;
  ble_stop();
#endif
  bt_spp_start();
  stored_preferences.btspp_active = true;
  String message((char *)0);
  message.reserve(70);
  message += F("Set optimal configuration for RaceChrono on Android:");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_header(true));
  res->println(message);
  res->println(generate_html_footer());
}
void handle_hlt_android(HTTPRequest *req, HTTPResponse *res)
{
  // /hlt/android
  log_i("Setting optimal configuration for Harry Lap Timer on Android: 10Hz, GSA+GSV Off, GSA+GSV polled on a 5 sec cycle, GBS On, BT-SPP");
  push_gps_message(UBLOX_INIT_10HZ, sizeof(UBLOX_INIT_10HZ));
  stored_preferences.gps_rate = 10;
  stored_preferences.nmeaGSA = false;
  stored_preferences.nmeaGSV = false;
#ifdef TASK_SCHEDULER
  stored_preferences.nmeaGSAGSVpolling = 5;
  control_poll_GSA_GSV(5);
#else
  // control_poll will disable GSA and GSV on its own
  push_gps_message(UBLOX_GxGSA_OFF, sizeof(UBLOX_GxGSA_OFF));
  push_gps_message(UBLOX_GxGSV_OFF, sizeof(UBLOX_GxGSV_OFF));
#endif
  push_gps_message(UBLOX_GxGBS_ON, sizeof(UBLOX_GxGBS_ON));
  stored_preferences.nmeaGBS = true;
  push_gps_message(UBLOX_INIT_CHANNEL_ALL, sizeof(UBLOX_INIT_CHANNEL_ALL));
  log_i("Set optimal configuration for Harry Lap Timer on Android");
  stored_preferences.trackaddict = false;
#ifdef BLEENABLED
  stored_preferences.ble_active = false;
  ble_stop();
#endif
  bt_spp_start();
  stored_preferences.btspp_active = true;
  String message((char *)0);
  message.reserve(70);
  message += F("Set optimal configuration for Harry Lap Timer on Android:");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_header(true));
  res->println(message);
  res->println(generate_html_footer());
}
#endif
void handle_hlt_tcpip(HTTPRequest *req, HTTPResponse *res)
{
  // /hlt/tcpip
  log_i("Setting optimal configuration for Harry Lap Timer on TCP-IP: 10Hz, GSA+GSV Polling, GBS On, BLE Off, BT-SPP Off");
  push_gps_message(UBLOX_INIT_10HZ, sizeof(UBLOX_INIT_10HZ));
  stored_preferences.gps_rate = 10;
  stored_preferences.nmeaGSA = false;
  stored_preferences.nmeaGSV = false;
#ifdef TASK_SCHEDULER
  stored_preferences.nmeaGSAGSVpolling = 5;
  control_poll_GSA_GSV(5);
#else
  // control_poll will disable GSA and GSV on its own
  push_gps_message(UBLOX_GxGSA_OFF, sizeof(UBLOX_GxGSA_OFF));
  push_gps_message(UBLOX_GxGSV_OFF, sizeof(UBLOX_GxGSV_OFF));
#endif
  push_gps_message(UBLOX_GxGBS_ON, sizeof(UBLOX_GxGBS_ON));
  stored_preferences.nmeaGBS = true;
  push_gps_message(UBLOX_INIT_CHANNEL_ALL, sizeof(UBLOX_INIT_CHANNEL_ALL));
  stored_preferences.trackaddict = false;
  log_i("Set optimal configuration for Harry Lap Timer on tcpip");
#ifdef BLEENABLED
  if (stored_preferences.ble_active)
  {
    ble_stop();
    stored_preferences.ble_active = false;
  }
#endif
#ifdef BTSPPENABLED
  if (stored_preferences.btspp_active)
  {
    // Hack: since disabling spp and enabling BLE crashes the stack and restart ESP32, we store the current configuration for a restart
    // stored_preferences.btspp_active = false;
    // StoreNVMPreferences(true);
    // res->setHeader(content_type, html_text);res->println(generate_html_body(F("<b>RESTARTING</b> to set optimal configuration for Harry Lap Timer on iOS devices:</p><p><ul><li>All SV's</li><li>GBS On</li><li>GSA+GSV stream off</li><li>GSA+GSV polled on a 5 sec cycle</li><li>Updates at 10 Hz</li><li>BLE connection</li></ul></p><p>Settings have been already saved</p>")));
    // delay(1000);
    // ESP.restart();
    // This is how it should be:
    stored_preferences.btspp_active = false;
    bt_spp_stop();
  }
#endif
  String message((char *)0);
  message.reserve(70);
  message += F("Set optimal configuration for Harry Lap Timer on WiFi:");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_header(true));
  res->println(message);
  res->println(generate_html_footer());
}
#ifdef BLEENABLED
void handle_hlt_ios(HTTPRequest *req, HTTPResponse *res)
{
  // /hlt/ios
  log_i("Setting optimal configuration for Harry Lap Timer on iOS devices: 10Hz, GSA+GSV Off, GBS On, BLE");
  push_gps_message(UBLOX_INIT_10HZ, sizeof(UBLOX_INIT_10HZ));
  stored_preferences.gps_rate = 10;
  stored_preferences.nmeaGSA = false;
  stored_preferences.nmeaGSV = false;
#ifdef TASK_SCHEDULER
  stored_preferences.nmeaGSAGSVpolling = 5;
  control_poll_GSA_GSV(5);
#else
  // control_poll will disable GSA and GSV on its own
  push_gps_message(UBLOX_GxGSA_OFF, sizeof(UBLOX_GxGSA_OFF));
  push_gps_message(UBLOX_GxGSV_OFF, sizeof(UBLOX_GxGSV_OFF));
#endif
  push_gps_message(UBLOX_GxGBS_ON, sizeof(UBLOX_GxGBS_ON));
  stored_preferences.nmeaGBS = true;
  push_gps_message(UBLOX_INIT_CHANNEL_ALL, sizeof(UBLOX_INIT_CHANNEL_ALL));
  stored_preferences.trackaddict = false;
  log_i("Set optimal configuration for Harry Lap Timer on iOS");
#ifdef BTSPPENABLED
  if (stored_preferences.btspp_active)
  {
    // Hack: since disabling spp and enabling BLE crashes the stack and restart ESP32, we store the current configuration for a restart
    stored_preferences.btspp_active = false;
    stored_preferences.ble_active = true;
    StoreNVMPreferences(true);
    res->setHeader(content_type, html_text);
    res->println(generate_html_body(F("<b>RESTARTING</b> to set optimal configuration for Harry Lap Timer on iOS devices:</p><p>Settings have been already saved</p>")));
    delay(1000);
    ESP.restart();
    // This is how it should be:
    // stored_preferences.btspp_active = false;
    // bt_spp_stop();
  }
#endif
  if (stored_preferences.ble_active == false)
  {
    ble_start();
    stored_preferences.ble_active = true;
  }
  String message((char *)0);
  message.reserve(70);
  message += F("Set optimal configuration for Harry Lap Timer on iOS:");
  message += FPSTR(savecfg);
  res->setHeader(content_type, html_text);
  res->println(generate_html_header(true));
  res->println(message);
  res->println(generate_html_footer());
}
#endif
void handle_wifi_sta(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Start WiFi in STA mode");
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("WiFi STATION mode started, trying to connect to a know Access Point. <br>Reconnect in a few seconds")));
  stored_preferences.wifi_mode = WIFI_STA;
  wifi_STA();
}
void handle_wifi_ap(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Start WiFi in AP mode");
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(String("WiFi Access Point mode started. <br>Reconnect in a few seconds to AP:" + String(ap_ssid))));
  stored_preferences.wifi_mode = WIFI_AP;
  wifi_AP();
}
void handle_restart(HTTPRequest *req, HTTPResponse *res)
{
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Please confirm <form action='/restart' method='post'><input type='submit' value='Restart'></form>")));
}
void handle_restart_execute(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Restarting");
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Restarting device")));
  delay(1000);
  ESP.restart();
  delay(1000);
}
void handle_status(HTTPRequest *req, HTTPResponse *res)
{
  log_d("Logging status to web");
  String message((char *)0);
  message.reserve(1400);
  message += F("<h1>Device status</h1><p>WiFi Mode: ");

  if (stored_preferences.wifi_mode == WIFI_AP)
  {
    message += F("AP<br>Access point name: ");
    message += ap_ssid;
  }
  else
  {
    message += F("STA<br>Connected to AP: ");
    message += WiFi.SSID();
  }

  message += F("<br>IP: ");
  message += MyIP.toString();
  message += F("<br>TCP Port for NMEA repeater: ");
  message += String(NMEAServerPort);
  message += F("<br>u-center URL: tcp://");
  message += MyIP.toString();
  message += String(":");
  message += String(NMEAServerPort);
#ifdef NUMERICAL_BROADCAST_PROTOCOL
  message += F("<br>Port for NBP: ") + String(NBPServerPort);
#else
  message += F("<p>NBP n/a in this firmware ");
#endif
#ifdef BLEENABLED
  message += (stored_preferences.ble_active ? F("<p>BLE Enabled") : F("<p>BLE Not Enabled"));
  message += F("<br>BLE Name: ");
  message += ble_device_id;
  message += F("<br>BLE Service: ");
  message += String(BLE_SERVICE_UUID, HEX);
  message += F("<br>BLE Characteristic: ");
  message += String(BLE_CHARACTERISTIC_UUID, HEX);
#else
  message += F("<p>BLE n/a in this firmware ");
#endif
#ifdef BTSPPENABLED
  message += (stored_preferences.btspp_active ? F("<p>BT-SPP Enabled") : F("<p>BT-SPP Not Enabled"));
  message += F("<br>BT Name: ");
  message += ble_device_id;
#else
  message += F("<p>BT-SPP n/a in this firmware ");
#endif
  message += F("<p>UART Baud: ");
  message += stored_preferences.gps_baud_rate;
  message += F("<br>Refresh rate: ");
  message += stored_preferences.gps_rate + String("Hz");
  message += F("<br>Main Talker ID: ");
  message += (stored_preferences.trackaddict ? F("GPS (TrackAddict)") : F("System Specific (default)"));
  message += F("<br>NMEA GxGSV: ");
  message += (stored_preferences.nmeaGSV ? String("ON") : String("OFF"));
  message += F("<br>NMEA GxGSA: ");
  message += (stored_preferences.nmeaGSA ? String("ON") : String("OFF"));
  message += F("<br>NMEA GxGBS: ");
  message += (stored_preferences.nmeaGBS ? String("ON") : String("OFF"));
  message += F("<br>GPS PowerSave: ");
  message += (gps_powersave ? String(" ON") : String("OFF"));
#ifdef UPTIME
  message += F("<br><br>Uptime: ");
  message += String(uptime_formatter::getUptime());
#endif
  message += F("<br>Version: ");
  message += String(BONO_GPS_VERSION);
  message += F("<br>Build date: ");
  message += String(BONOGPS_BUILD_DATE);
#ifdef ENABLE_OTA
  message += F("<br>OTA Built-in");
#else
  message += F("<br>OTA n/a in this firmware ");
#endif
  message += F("<p>Total heap: ");
  message += String(ESP.getHeapSize());
  message += F("<br>Free heap: ");
  message += String(ESP.getFreeHeap());
  message += F("<br>Total PSRAM: ");
  message += String(ESP.getPsramSize());
  message += F("<br>Free PSRAM: ");
  message += String(ESP.getFreePsram());

  res->setHeader(content_type, html_text);
  res->println(generate_html_header(true));
  res->println(message);
  res->println(generate_html_footer());
}
void handle_clients(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Logging clients status to web");
  String message((char *)0);
  message.reserve(1000);
  message += F("NMEA TCP/IP client");
  if (!NMEARemoteClient.connected())
  {
    message += " not";
  }

#ifdef NUMERICAL_BROADCAST_PROTOCOL
  message += F(" connected<br>NBP TCP/IP client");
  if (NBPRemoteClient.connected())
  {
    message += " not";
  }
#endif
#ifdef BLEENABLED
  message += F(" connected<br>BLE device");
  if (!ble_deviceConnected)
  {
    message += " not";
  }
#endif
#ifdef BTSPPENABLED
  message += F(" connected<br>BT-SPP device");
  if (!bt_deviceConnected)
  {
    message += " not";
  }
#endif
  message += F(" connected");
  res->setHeader(content_type, html_text);
  res->println(generate_html_header(true));
  res->println(message);
  res->println(generate_html_footer());
}

void handle_saveconfig_wifi_creds(HTTPRequest *req, HTTPResponse *res)
{
  res->setHeader(content_type, html_text);
  // due to the inability of finding out how to read POST variables in a ResourceParameters form esp32_https_server, form is GET
  res->println(generate_html_body(F("<article><form action='/savecfg/wifi/creds/post' >SSID<br><input style='background: none;color: black;' name='ssid' maxlength='32'><p>WPA Key<br><input style='background: none;color: black;' type='password' name='key' maxlength='64'><p><input type='submit' value='Save'></form></article>")));
}

void handle_saveconfig_wifi_creds_post(HTTPRequest *req, HTTPResponse *res)
{
  auto params = req->getParams();
  std::string wifissid;
  std::string wifikey;
  bool hasssid = params->getQueryParameter("ssid", wifissid);
  bool haskey = params->getQueryParameter("key", wifikey);
  res->setHeader(content_type, html_text);
  if (hasssid && haskey) {
    log_i("WiFi SSID %s", wifissid);
    log_d("WiFi Key %s", wifikey);
    strcpy(stored_preferences.wifi_ssid, wifissid.c_str());
    strcpy(stored_preferences.wifi_key, wifikey.c_str());
    StoreNVMPreferencesWiFiCreds();
    // TODO check is storing went fine
    res->println(generate_html_body(F("<article>New WiFi SSID and Key stored <p><a href='/wifi/sta'>Try them</a></article>")));
  } 
  else 
  {
    res->println(generate_html_body(F("<article>Either SSID or Key empty - not stored</article>")));
  }  
}

void handle_saveconfig(HTTPRequest *req, HTTPResponse *res)
{
  log_i("Storing preferences intermediate menu");
  String message((char *)0);
  message.reserve(1000);
  message += F("<h1>All configuration values</h1><p>Save <a href='/savecfg/wifi'>all config <b>including</b> WiFi </a></p><p>Save <a href='/savecfg/nowifi'>all but WiFi-");
  if (stored_preferences.wifi_mode == WIFI_AP)
  {
    message += "AP";
  }
  else
  {
    message += "STA";
  }
  message += F(" mode</a></p>\n<h1>Save only a specific WiFi mode</h1><p>Save <a href='/savecfg/wifi/sta'>WiFi client mode WiFi-STA </a></p><p>Save <a href='/savecfg/wifi/ap'>WiFi Access Point mode WiFi-AP</a></p>");
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(message));
}
void handle_saveconfig_withwifi(HTTPRequest *req, HTTPResponse *res)
{
  StoreNVMPreferences(true);
  log_i("Storing preferences including WiFi");
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Preferences including WiFi stored")));
}
void handle_saveconfig_withoutwifi(HTTPRequest *req, HTTPResponse *res)
{
  StoreNVMPreferences(false);
  log_i("Storing preferences excluding WiFi");
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Preferences excluding WiFi stored")));
}
void handle_saveconfig_wifi_sta(HTTPRequest *req, HTTPResponse *res)
{
  StoreNVMPreferencesWiFi("WIFI_STA");
  log_i("Storing preference WIFI_STA");
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Stored preference WIFI_STA as default: this will be used at next restart")));
}
void handle_saveconfig_wifi_ap(HTTPRequest *req, HTTPResponse *res)
{
  StoreNVMPreferencesWiFi("WIFI_AP");
  log_i("Storing preference WIFI_AP");
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("Stored preference WIFI_AP as default: this will be used at next restart")));
}
void handle_NotFound(HTTPRequest *req, HTTPResponse *res)
{
  req->discardRequestBody();
  res->setStatusCode(404);
  res->setStatusText("Not Found");
  res->setHeader(content_type, html_text);
  res->println(generate_html_body(F("<h1>404 Not Found</h1><p>The requested resource was not found on this server.</p>")));
}
void WebConfig_start()
{
  // For every resource available on the server, we need to create a ResourceNode
  // The ResourceNode links URL and HTTP method to a handler function
  ResourceNode *nodeRoot = new ResourceNode("/", "GET", &handle_menu);

  webserver.registerNode(nodeRoot);
  ResourceNode *node404 = new ResourceNode("", "GET", &handle_NotFound);
  webserver.setDefaultNode(node404);

  ResourceNode *nodecss = new ResourceNode("/css", "GET", handle_css);
  webserver.registerNode(nodecss);
  ResourceNode *noderate_1hz = new ResourceNode("/rate/1hz", "GET", handle_rate_1hz);
  webserver.registerNode(noderate_1hz);
  ResourceNode *noderate_5hz = new ResourceNode("/rate/5hz", "GET", handle_rate_5hz);
  webserver.registerNode(noderate_5hz);
  ResourceNode *noderate_10hz = new ResourceNode("/rate/10hz", "GET", handle_rate_10hz);
  webserver.registerNode(noderate_10hz);
  ResourceNode *nodegsa_on = new ResourceNode("/gsa/on", "GET", handle_gsa_on);
  webserver.registerNode(nodegsa_on);
  ResourceNode *nodegsa_off = new ResourceNode("/gsa/off", "GET", handle_gsa_off);
  webserver.registerNode(nodegsa_off);
  ResourceNode *nodegsv_on = new ResourceNode("/gsv/on", "GET", handle_gsv_on);
  webserver.registerNode(nodegsv_on);
  ResourceNode *nodegsv_off = new ResourceNode("/gsv/off", "GET", handle_gsv_off);
  webserver.registerNode(nodegsv_off);
  ResourceNode *nodegbs_on = new ResourceNode("/gbs/on", "GET", handle_gbs_on);
  webserver.registerNode(nodegbs_on);
  ResourceNode *nodegbs_off = new ResourceNode("/gbs/off", "GET", handle_gbs_off);
  webserver.registerNode(nodegbs_off);
  ResourceNode *nodesv8 = new ResourceNode("/sv/8", "GET", handle_svchannel_8);
  webserver.registerNode(nodesv8);
  ResourceNode *nodesv8all = new ResourceNode("/sv/all", "GET", handle_svchannel_all);
  webserver.registerNode(nodesv8all);
#ifdef BTSPPENABLED
  ResourceNode *nodehlt_android = new ResourceNode("/hlt/android", "GET", handle_hlt_android);
  webserver.registerNode(nodehlt_android);
  ResourceNode *nodeta_on = new ResourceNode("/trackaddict/on", "GET", handle_trackaddict_on);
  webserver.registerNode(nodeta_on);
  ResourceNode *nodeta_off = new ResourceNode("/trackaddict/off", "GET", handle_trackaddict_off);
  webserver.registerNode(nodeta_off);
  ResourceNode *noderc_android = new ResourceNode("/racechrono/android", "GET", handle_racechrono_android);
  webserver.registerNode(noderc_android);
#endif
#ifdef BLEENABLED
  ResourceNode *nodehlt_ios = new ResourceNode("/hlt/ios", "GET", handle_hlt_ios);
  webserver.registerNode(nodehlt_ios);
#endif
  ResourceNode *nodehlt_tcpip = new ResourceNode("/hlt/tcpip", "GET", handle_hlt_tcpip);
  webserver.registerNode(nodehlt_tcpip);
  ResourceNode *nodewifi_sta = new ResourceNode("/wifi/sta", "GET", handle_wifi_sta);
  webserver.registerNode(nodewifi_sta);
  ResourceNode *nodewifi_ap = new ResourceNode("/wifi/ap", "GET", handle_wifi_ap);
  webserver.registerNode(nodewifi_ap);
  ResourceNode *noderestart = new ResourceNode("/restart", "GET", handle_restart);
  webserver.registerNode(noderestart);
  ResourceNode *noderestart_post = new ResourceNode("/restart", "POST", handle_restart_execute);
  webserver.registerNode(noderestart_post);
  ResourceNode *nodesavecfg = new ResourceNode("/savecfg", "GET", handle_saveconfig);
  webserver.registerNode(nodesavecfg);
  ResourceNode *nodesavecfg_wifi = new ResourceNode("/savecfg/wifi", "GET", handle_saveconfig_withwifi);
  webserver.registerNode(nodesavecfg_wifi);
  ResourceNode *nodesavecfg_nowifi = new ResourceNode("/savecfg/nowifi", "GET", handle_saveconfig_withoutwifi);
  webserver.registerNode(nodesavecfg_nowifi);
  ResourceNode *nodesavecfg_wifi_sta = new ResourceNode("/savecfg/wifi/sta", "GET", handle_saveconfig_wifi_sta);
  webserver.registerNode(nodesavecfg_wifi_sta);
  ResourceNode *nodesavecfg_wifi_ap = new ResourceNode("/savecfg/wifi/ap", "GET", handle_saveconfig_wifi_ap);
  webserver.registerNode(nodesavecfg_wifi_ap);
  ResourceNode *nodesavecfg_wifi_creds = new ResourceNode("/savecfg/wifi/creds", "GET", handle_saveconfig_wifi_creds);
  webserver.registerNode(nodesavecfg_wifi_creds);
  ResourceNode *nodesavecfg_wifi_creds_post = new ResourceNode("/savecfg/wifi/creds/post", "GET", handle_saveconfig_wifi_creds_post);
  webserver.registerNode(nodesavecfg_wifi_creds_post);
  ResourceNode *nodepreset = new ResourceNode("/preset", "GET", handle_preset);
  webserver.registerNode(nodepreset);
  ResourceNode *nodepowersave_3600 = new ResourceNode("/powersave/3600", "GET", handle_powersave_1hr);
  webserver.registerNode(nodepowersave_3600);
  ResourceNode *nodepowersave_1800 = new ResourceNode("/powersave/1800", "GET", handle_powersave_30min);
  webserver.registerNode(nodepowersave_1800);
  ResourceNode *nodestatus = new ResourceNode("/status", "GET", handle_status);
  webserver.registerNode(nodestatus);
  ResourceNode *nodeclients = new ResourceNode("/clients", "GET", handle_clients);
  webserver.registerNode(nodeclients);
  ResourceNode *nodebaud_38400 = new ResourceNode("/baud/38400", "GET", handle_baudrate_38400);
  webserver.registerNode(nodebaud_38400);
  ResourceNode *nodebaud_57600 = new ResourceNode("/baud/57600", "GET", handle_baudrate_57600);
  webserver.registerNode(nodebaud_57600);
  ResourceNode *nodebaud_115200 = new ResourceNode("/baud/115200", "GET", handle_baudrate_115200);
  webserver.registerNode(nodebaud_115200);
#ifdef BLEENABLED
  ResourceNode *nodeble_on = new ResourceNode("/ble/on", "GET", handle_ble_on);
  webserver.registerNode(nodeble_on);
  ResourceNode *nodeble_off = new ResourceNode("/ble/off", "GET", handle_ble_off);
  webserver.registerNode(nodeble_off);
#endif
#ifdef BTSPPENABLED
  ResourceNode *nodebtspp_on = new ResourceNode("/btspp/on", "GET", handle_btspp_on);
  webserver.registerNode(nodebtspp_on);
  ResourceNode *nodebtspp_off = new ResourceNode("/btspp/off", "GET", handle_btspp_off);
  webserver.registerNode(nodebtspp_off);
#endif
#ifdef TASK_SCHEDULER
  ResourceNode *nodepollgsavgsv_off = new ResourceNode("/poll/gsagsv/0", "GET", handle_pollgsagsv_off);
  webserver.registerNode(nodepollgsavgsv_off);
  ResourceNode *nodepollgsavgsv_on_1 = new ResourceNode("/poll/gsagsv/1", "GET", handle_pollgsagsv_on_1);
  webserver.registerNode(nodepollgsavgsv_on_1);
  ResourceNode *nodepollgsavgsv_on_5 = new ResourceNode("/poll/gsagsv/5", "GET", handle_pollgsagsv_on_5);
  webserver.registerNode(nodepollgsavgsv_on_5);
#endif
  webserver.start();
#ifdef MDNS_ENABLE
  MDNS.addService("http", "tcp", 80);
#endif
}

/********************************

  OTA

* ******************************/
#ifdef ENABLE_OTA
#ifndef OTA_AVAILABLE
#define OTA_AVAILABLE 300 // 300 seconds of OTA running
#endif

#ifdef TASK_SCHEDULER
Task tOTA(1000, OTA_AVAILABLE, &handle_OTA, &ts, false);
#endif

void handle_OTA()
{
  ArduinoOTA.handle();
}
void OTA_start()
{
  ArduinoOTA.setHostname(BONOGPS_MDNS);
  ArduinoOTA
      .onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
          type = "sketch";
        else // U_SPIFFS
          type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        log_i("Start updating %s", type);
      })
      .onEnd([]() {
        log_i("\nEnd");
      })
      .onProgress([](unsigned int progress, unsigned int total) {
        SerialMonitor.printf("Progress: %u%%\r", (progress / (total / 100)));
      })
      .onError([](ota_error_t error) {
        log_e("Error[ %u]: ", error);
        if (error == OTA_AUTH_ERROR)
          Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR)
          Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR)
          Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR)
          Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR)
          Serial.println("End Failed");
      });
  ArduinoOTA.begin();
  tOTA.enable();
}
#endif

/********************************

  BLE

* ******************************/

#ifdef BLEENABLED

#define DEVINFO_UUID (uint16_t)0x180a
#define DEVINFO_MANUFACTURER_UUID (uint16_t)0x2a29 //0x2A29 MfgName utf8s
#define DEVINFO_NAME_UUID (uint16_t)0x2a24         //0x2A24 ModelNum utf8s
#define DEVINFO_SERIAL_UUID (uint16_t)0x2a25       //0x2A25 SerialNum utf8s
#define DEVINFO_FWREV_UUID (uint16_t)0x2a26        //0x2A26 FirmwareRev utf8s

#define SERVICE_BATTERY_SERVICE_UUID (uint16_t)0x180F            // not used
#define CHARACTERISTIC_BATTERY_LEVEL_UUID (uint16_t)0x2A19       // not used
#define CHARACTERISTIC_BATTERY_LEVEL_STATE_UUID (uint16_t)0x2A1B // not used
#define CHARACTERISTIC_BATTERY_POWER_STATE_UUID (uint16_t)0x2A1A // not used
//#define HardwareRev "0001"    //0x2A27 utf8s
//#define SystemID "000001"     //0x2A23 uint40

NimBLEServer *pServer = NULL;
NimBLECharacteristic *pCharacteristic = NULL;
uint16_t ble_mtu;

// Build BTLE messages here
uint8_t gps_currentmessage[MAX_UART_BUFFER_SIZE]; // hold current buffer
uint8_t gps_currentchar = '$';                    // hold current char
int gps_message_pointer = 0;                      //pointer

class MyServerCallbacks : public BLEServerCallbacks
{
  void onConnect(NimBLEServer *pServer, ble_gap_conn_desc *desc)
  {
    ble_deviceConnected = true;
    ble_client = String(NimBLEAddress(desc->peer_ota_addr).toString().c_str());
    log_i("BLE Client address: %s", ble_client);
  };
  void onDisconnect(NimBLEServer *pServer)
  {
    ble_deviceConnected = false;
  }
};

void ble_start()
{
#ifdef BTSPPENABLED
  bt_spp_stop();
#endif
  /// Create the BLE Device
  NimBLEDevice::init(ble_device_id);
  /** Optional: set the transmit power, default is 3db */
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);                             /** +9db */
  NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_ADV);       /** +9db */
  NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_CONN_HDL0); /** +9db */
  NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_CONN_HDL1); /** +9db */
  NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_CONN_HDL2); /** +9db */
  // Set the MTU to a larger value than standard 23
  NimBLEDevice::setMTU(BLE_MTU);
  ble_mtu = NimBLEDevice::getMTU();
  log_d("BLE MTU set to: %d", ble_mtu);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityAuth(/*BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM |*/ BLE_SM_PAIR_AUTHREQ_SC);
  // Create the BLE Server
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  // Create the BLE Service
  NimBLEService *pService = pServer->createService(BLE_SERVICE_UUID);
  // Create a BLE Characteristic
  pCharacteristic = pService->createCharacteristic(BLE_CHARACTERISTIC_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  // Start the service
  pService->start();
  // other service with info
  pService = pServer->createService(DEVINFO_UUID);
  BLECharacteristic *pChar = pService->createCharacteristic(DEVINFO_MANUFACTURER_UUID, NIMBLE_PROPERTY::READ);
  pChar->setValue(DEVICE_MANUFACTURER);
  pChar = pService->createCharacteristic(DEVINFO_NAME_UUID, NIMBLE_PROPERTY::READ);
  pChar->setValue(DEVICE_NAME);
  pChar = pService->createCharacteristic(DEVINFO_SERIAL_UUID, NIMBLE_PROPERTY::READ);
  pChar->setValue(chip);
  pChar = pService->createCharacteristic(DEVINFO_FWREV_UUID, NIMBLE_PROPERTY::READ);
  pChar->setValue(BONO_GPS_VERSION);
  pService->start();
  // Start advertising
  NimBLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(pService->getUUID());
  // define appearance, from https://www.bluetooth.com/wp-content/uploads/Sitecore-Media-Library/Gatt/Xml/Characteristics/org.bluetooth.characteristic.gap.appearance.xml
  pAdvertising->setAppearance(5186);
  pAdvertising->setScanResponse(false);
  // pAdvertising->setMinPreferred(0x06); // functions that help with iPhone connections issue -> not for NimBLE
  /**This method is removed as it was no longer useful and consumed advertising space
    pAdvertising->setMinPreferred(0x0);  // set value to 0x00 to not advertise this parameter
  */
  // NimBLEDevice::startAdvertising();
  pAdvertising->start();
  log_i("Waiting a client connection to notify...");
}

void ble_stop()
{
  if (NimBLEDevice::getInitialized())
  {
    NimBLEDevice::stopAdvertising();
    NimBLEDevice::deinit(true);
  }
}
#endif

#ifdef BTSPPENABLED
/*********************************************************************

  Bluetooth Classic

*******************************************************************/
void bt_spp_start()
{
#ifdef BLEENABLED
  // disable BTLE
  ble_stop();
#endif
  SerialBT.begin(ble_device_id); //Bluetooth device name
  SerialBT.register_callback(bt_callback);
  log_i("Started BT-SPP port");
}
void bt_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
  if (event == ESP_SPP_SRV_OPEN_EVT)
  {
    log_d("BT-SPP Client Connected");
#ifdef BLEENABLED
    ble_deviceConnected = false;
#endif
    bt_deviceConnected = true;
  }
  if (event == ESP_SPP_CLOSE_EVT)
  {
    log_d("BT-SPP Client closed connection");
    bt_deviceConnected = false;
  }
}
void bt_spp_stop()
{
  SerialBT.end();
}
#endif

/*********************************************************************

  SETUP

*******************************************************************/
void gps_initialize_settings()
{
  // GPS Connection
  // if preferences have a value <> than the one stored in the GPS, connect+switch
  if (GPS_STANDARD_BAUD_RATE == stored_preferences.gps_baud_rate)
  {
    log_i("Connecting to GPS at standard %d", stored_preferences.gps_baud_rate);
    gpsPort.begin(stored_preferences.gps_baud_rate);
  }
  else
  {
    log_i("Connecting to GPS at standard %d", GPS_STANDARD_BAUD_RATE);
    gpsPort.begin(GPS_STANDARD_BAUD_RATE);
    log_i("Re-Connecting to GPS at updated %d", stored_preferences.gps_baud_rate);
    switch_baudrate(stored_preferences.gps_baud_rate);
  }

  gpsPort.setRxBufferSize(UART_BUFFER_SIZE_RX);
  delay(50);

  if (stored_preferences.nmeaGSV)
  {
    push_gps_message(UBLOX_GxGSV_ON, sizeof(UBLOX_GxGSV_ON));
  }
  else
  {
    push_gps_message(UBLOX_GxGSV_OFF, sizeof(UBLOX_GxGSV_OFF));
  }

  if (stored_preferences.nmeaGSA)
  {
    push_gps_message(UBLOX_GxGSA_ON, sizeof(UBLOX_GxGSA_ON));
  }
  else
  {
    push_gps_message(UBLOX_GxGSA_OFF, sizeof(UBLOX_GxGSA_OFF));
  }

  if (stored_preferences.nmeaGBS)
  {
    push_gps_message(UBLOX_GxGBS_ON, sizeof(UBLOX_GxGBS_ON));
  }
  else
  {
    push_gps_message(UBLOX_GxGBS_OFF, sizeof(UBLOX_GxGBS_OFF));
  }

#ifdef BTSPPENABLED
  // apply settings required by TrackAddict
  if (stored_preferences.trackaddict)
  {
    gps_enable_trackaddict();
  }
  else
  {
    push_gps_message(UBLOX_INIT_CHANNEL_ALL, sizeof(UBLOX_INIT_CHANNEL_ALL));
  }
#endif

  switch (stored_preferences.gps_rate)
  {
  case 1:
    push_gps_message(UBLOX_INIT_1HZ, sizeof(UBLOX_INIT_1HZ));
    break;
  case 5:
    push_gps_message(UBLOX_INIT_5HZ, sizeof(UBLOX_INIT_5HZ));
    break;
  case 10:
    push_gps_message(UBLOX_INIT_10HZ, sizeof(UBLOX_INIT_10HZ));
    break;
  default:
    push_gps_message(UBLOX_INIT_5HZ, sizeof(UBLOX_INIT_5HZ));
    break;
  }
#ifdef TASK_SCHEDULER
  control_poll_GSA_GSV(stored_preferences.nmeaGSAGSVpolling);
#endif
}

void setup()
{
  SerialMonitor.begin(LOG_BAUD_RATE); // Initialize the serial monitor
  delay(200);
  log_d("ESP32 SDK: %s", ESP.getSdkVersion());
  log_d("Arduino sketch: %s", __FILE__);
  log_d("Compiled on: %s", __DATE__);

  //set led pin as output
  pinMode(LED_BUILTIN, OUTPUT);
  tLedBlink.setInterval(100);
  tLedBlink.enable();

  // Generate device name
  chip = (uint16_t)((uint64_t)ESP.getEfuseMac() >> 32);
  sprintf(ap_ssid, "%s-%04X", BONOGPS_AP, chip);
#if defined(BLEENABLED) || defined(BTSPPENABLED)
  sprintf(ble_device_id, "%s-%04X", BLE_DEVICE_ID, chip);
#endif

  // Read desired status from NVM
  prefs.begin(BONOGPS_MDNS);
  ReadNVMPreferences();

  if (stored_preferences.ble_active)
  {
    // start BTLE
#ifdef BLEENABLED
    ble_start();
#endif
#ifdef BTSPPENABLED
  }
  else if (stored_preferences.btspp_active)
  {
    // else prevents BT-SPP and BLE to start at the same time which crashes the Bluetooth stacks
    // start BTSPP
    bt_spp_start();
    delay(1000);
#endif
  }

#ifdef BUTTON
  button.begin();
#endif

  // Start WiFi
  WiFi.setHostname(BONOGPS_MDNS);
  WiFi.setSleep(false);

  switch (stored_preferences.wifi_mode)
  {
  case WIFI_AP:
    wifi_AP();
    break;
  case WIFI_STA:
    wifi_STA();
    break;
  default:
    wifi_STA();
    break;
  }

  gps_initialize_settings();

  log_d("Total heap: %d", ESP.getHeapSize());
  log_d("Free heap: %d", ESP.getFreeHeap());
  log_d("Total PSRAM: %d", ESP.getPsramSize());
  log_d("Free PSRAM: %d", ESP.getFreePsram());
}

void loop()
{
  // check UART for data
  if (gpsPort.available())
  {
    size_t len = gpsPort.available();
    uint8_t sbuf[len];
    gpsPort.readBytes(sbuf, len);
    // log max buffer size
    if (len > max_buffer)
    {
      max_buffer = len;
      log_w("New max buffer: %d", max_buffer);
    }
    if (wifi_connected && NMEARemoteClient && NMEARemoteClient.connected())
    {
      //push UART data to the TCP client
      NMEARemoteClient.write(sbuf, len);
    }

#ifdef NUMERICAL_BROADCAST_PROTOCOL
    if (wifi_connected && NBPRemoteClient && NBPRemoteClient.connected())
    {
      // send GPS data to NeoGPS processor for parsing
      for (int i = 0; i < len; i++)
      {
        if (gps.decode((char)sbuf[i]) == NMEAGPS::DECODE_COMPLETED)
        {
          log_d("Fix available, size: %d", sizeof(gps.fix()));
          my_fix = gps.fix();

          NBPRemoteClient.print("*NBP1,ALLUPDATE,");
          NBPRemoteClient.print(my_fix.dateTime);
          NBPRemoteClient.print(".");
          NBPRemoteClient.println(my_fix.dateTime_ms());
          if (my_fix.valid.location)
          {
            NBPRemoteClient.print("\"Latitude\",\"deg\":");
            NBPRemoteClient.println(my_fix.latitude(), 12);
            NBPRemoteClient.print("\"Longitude\",\"deg\":");
            NBPRemoteClient.println(my_fix.longitude(), 12);
            NBPRemoteClient.print("\"Altitude\",\"m\":");
            NBPRemoteClient.println(my_fix.altitude());
          }
          if (my_fix.valid.heading)
          {
            NBPRemoteClient.print("\"Heading\",\"deg\":");
            NBPRemoteClient.println(my_fix.heading());
          }
          if (my_fix.valid.speed)
          {
            NBPRemoteClient.print("\"Speed\",\"kmh\":");
            NBPRemoteClient.println(my_fix.speed_kph());
          }
          // NBPRemoteClient.print("\"Accuracy\",\"m\":");
          NBPRemoteClient.println("#");
          NBPRemoteClient.print("@NAME:");
          NBPRemoteClient.println(ap_ssid);
        }
      }
    }
    else
    {
      NBPCheckForConnections();
    }
#endif

#ifdef BTSPPENABLED
    if (bt_deviceConnected && stored_preferences.btspp_active)
    {
      // we have BT-SPP active
      SerialBT.write(sbuf, len);
    }
#endif

#ifdef BLEENABLED
    if (ble_deviceConnected && stored_preferences.ble_active)
    {
      // we have BLE active
      for (int i = 0; i < len; i++)
      {
        if (gps_message_pointer > MAX_UART_BUFFER_SIZE - 3)
        {
          log_e("BLE Buffer saturated, resetting ");
          gps_currentmessage[0] = '$';
          gps_currentmessage[1] = '\0';
          gps_message_pointer = 1;
        }
        if (sbuf[i] == '$')
        {
          gps_currentmessage[gps_message_pointer++] = '\r';
          gps_currentmessage[gps_message_pointer++] = '\n';
          gps_currentmessage[gps_message_pointer] = '\0';
          // There is a new message -> Notify BLE of the changed value if connected and size says it is likely to be valid
          if (gps_message_pointer > MIN_NMEA_MESSAGE_SIZE)
          {
            pCharacteristic->setValue(gps_currentmessage);
            pCharacteristic->notify();
            delay(1);
          }
          gps_currentmessage[0] = '$';
          gps_currentmessage[1] = '\0';
          gps_message_pointer = 1;
        }
        else
        {
          // Accumulate more of the GPS message
          gps_currentmessage[gps_message_pointer++] = sbuf[i];
        }
      }
    }
#endif
  }

  if (wifi_connected)
  {
    // handle sending instructions to GPS via uBlox center connected with TCP/IP
    if (NMEARemoteClient.connected() && NMEARemoteClient.available())
    {
      gpsPort.write(NMEARemoteClient.read());
    }
    else
    {
      NMEACheckForConnections();
    }

    // TODO - impove handling of webserver so that it has a lower priority or spin a separate task
    webserver.loop();
  }

#ifdef BUTTON
  // Continuously read the status of the button.
  button.read();
#endif

#ifdef TASK_SCHEDULER
  // run all periodic tasks
  ts.execute();
#endif
}
