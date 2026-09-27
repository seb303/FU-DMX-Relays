#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiUdp.h>
#include <esp_system.h>

// ============================================================
// DEBUG
// ============================================================

#define DEBUG 1
#define DEBUG_ARTNET 1
#define DEBUG_VERBOSE 0

#if DEBUG
  #define DBG_PRINT(...) Serial.print(__VA_ARGS__)
  #define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
  #define DBG_PRINT(...)
  #define DBG_PRINTLN(...)
#endif

#if DEBUG_ARTNET
  #define ARTNET_PRINT(...) Serial.print(__VA_ARGS__)
  #define ARTNET_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
  #define ARTNET_PRINT(...)
  #define ARTNET_PRINTLN(...)
#endif

// DEBUG_VERBOSE is a cross-cutting verbosity dial, not a topic of
// its own. It does not gate anything by itself:
//   - Combined with DEBUG, it controls per-request web server logging.
//   - Combined with DEBUG_ARTNET, it controls the full per-packet
//     Art-Net firehose (every packet, including ArtPoll, and every
//     DMX frame). With DEBUG_ARTNET on and DEBUG_VERBOSE off, only
//     relay state *changes* are logged.

// ============================================================
// HARDWARE
// ============================================================

const int RELAY_OUTER = 16;
const int RELAY_INNER = 17;

// Minimum time a relay must remain in a given state before it can
// switch again, to protect the relay contacts from excessive wear
// if the controlling DMX channel changes rapidly (e.g. a stray
// strobe/chase effect pointed at this channel by mistake).
const unsigned long RELAY_DWELL_MS = 200;

// Maximum number of ArtPollReply sends that can be queued at once,
// each with its own randomised send time. Sized generously for a
// handful of controllers/apps polling around the same time; if the
// queue is ever full, the newest poll is simply not replied to (the
// polling controller will just ask again shortly).
const int MAX_PENDING_ARTPOLL_REPLIES = 8;

// ============================================================
// WIFI
// ============================================================

const int MAX_WIFI_NETWORKS = 3;

const unsigned long CONNECT_TIMEOUT_MS = 10000;
const unsigned long WIFI_CHECK_INTERVAL_MS = 1000;

String wifiSSID[MAX_WIFI_NETWORKS];
String wifiPassword[MAX_WIFI_NETWORKS];

String apSSID = "ESP32-Relay";

String webUser = "admin";
String webPass = "admin";

bool accessPointMode = false;
bool hasConnectedToWiFi = false;

int currentNetwork = -1;
int nextNetworkToTry = 0;

bool wifiRecoveryActive = false;

unsigned long lastWiFiCheck = 0;

// ============================================================
// ART-NET
// ============================================================

const uint16_t ARTNET_PORT = 6454;

// Global Art-Net Port-Address / Universe.
// Valid Art-Net 4 range is 0-32767 (15-bit Port-Address).
// 0 is a valid value even though deprecated.
// Default value is 1
uint16_t artnetUniverse = 1;

// Individual DMX addresses for the two relays.
uint16_t relayOuterDMXAddress = 1;
uint16_t relayInnerDMXAddress = 2;

// Single configured node name, used for both the Art-Net
// ShortName and LongName fields.
String artnetNodeName = "ESP32-Relay";

// Generic/unregistered values.
// These are deliberately not presented as a real manufacturer identity.
const uint16_t ARTNET_OEM_CODE = 0xFFFF;
const uint16_t ARTNET_ESTA_MANUFACTURER = 0x0000;

// Firmware revision reported in ArtPollReply.
const uint16_t ARTNET_FIRMWARE_VERSION = 0x0001;

WiFiUDP artnetUDP;
bool artnetStarted = false;

unsigned long artPollReplyCounter = 0;

// Timestamp (millis()) of the most recently received valid
// Art-Net packet (any type), for status-page display. everReceived
// distinguishes "never" from a genuine 0ms-ago reading.
unsigned long lastArtnetPacketMs = 0;
bool everReceivedArtnetPacket = false;

// ----------------------------------------------------------
// Pending ArtPollReply queue
// ----------------------------------------------------------
//
// ArtPollReply sends are deliberately delayed by a random amount
// (0-1000ms) rather than sent immediately, so that many nodes on
// a network don't all reply to a broadcast ArtPoll at the same
// instant. Queued here and drained from loop() so it never blocks.

struct PendingArtPollReply
{
  bool active;
  IPAddress destination;
  unsigned long sendAtMs;
};

PendingArtPollReply pendingArtPollReplies[MAX_PENDING_ARTPOLL_REPLIES];

// ============================================================
// PREFERENCES
// ============================================================

Preferences preferences;

// Determined once at boot (see determineResetReason()) and reused
// for the status page rather than re-reading/clearing NVS on every
// page load.
String bootResetReasonText;

// ============================================================
// FORWARD DECLARATIONS
// ============================================================

void startArtNet();
void stopArtNet();
void handleArtNet();

void startAccessPoint();
bool tryWiFiNetwork(int index);
void connectToSavedNetworks();
void startWiFiRecovery();
void handleWiFiRecovery();
void handleWiFiMonitor();

void sendArtPollReply(IPAddress destination);
void scheduleArtPollReply(IPAddress destination);
void handlePendingArtPollReplies();

String getMACString();
String getSerialNumber();
bool getStationMAC(uint8_t mac[6]);
String getHardwareResetReasonString();
String determineResetReason();
String formatDuration(unsigned long ms);
String describeRSSI(int rssi);

String htmlEscape(const String &input);
String getFormValue(const char *name);

void loadConfiguration();
void saveConfiguration();

void handleRoot();
void handleSave();
void handleDiscard();
void handlePing();
void logWebRequest();

// ============================================================
// WEB SERVER
// ============================================================

WebServer server(80);

// Logs one line per incoming HTTP request (method, path, client
// IP). Gated on DEBUG_VERBOSE, per spec: request logging is only
// wanted when deliberately running verbose, since a busy control
// surface (frequent status polling, etc.) can generate a lot of
// requests. Never logs headers or body content, so this is safe
// to call even for /save and /discard.
void logWebRequest()
{
#if DEBUG && DEBUG_VERBOSE
  DBG_PRINT("[WEB] ");
  DBG_PRINT(server.method() == HTTP_GET ? "GET " : "POST ");
  DBG_PRINT(server.uri());
  DBG_PRINT(" from ");
  DBG_PRINTLN(server.client().remoteIP());
#endif
}

// ============================================================
// MAC / SERIAL
// ============================================================

// Reads the station interface's MAC address. Now that
// startAccessPoint() uses WIFI_AP_STA (keeping the STA interface
// initialised even while running as an access point),
// WiFi.macAddress() returns the correct value in every mode, so
// there's no need for any lower-level esp-idf MAC API here.
bool getStationMAC(uint8_t mac[6])
{
  return WiFi.macAddress(mac) != nullptr;
}

String getMACString()
{
  return WiFi.macAddress();
}

// The Art-Net serial is simply the node's MAC address with no
// separators and no additional prefix, so it is easy to match
// up against the MAC shown elsewhere in the interface.
String getSerialNumber()
{
  uint8_t mac[6] = { 0, 0, 0, 0, 0, 0 };

  getStationMAC(mac);

  char buf[13];

  snprintf(
    buf, sizeof(buf),
    "%02X%02X%02X%02X%02X%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
  );

  return String(buf);
}

// ============================================================
// RESET REASON
// ============================================================
//
// The hardware-level reset reason (power-on, brownout, watchdog,
// panic, etc.) doesn't distinguish an intentional web-triggered
// reboot from any other software reset. To surface that distinction
// on the status page, handleSave()/handleDiscard() write a short
// description to NVS immediately before calling ESP.restart(); on
// the next boot, determineResetReason() consumes it (clearing it
// so it can't apply to a later, unrelated reboot) and uses it only
// when the hardware reason agrees that this was in fact a software
// reset - otherwise a stale flag is discarded and the real hardware
// reason (e.g. Brownout) is shown instead.

String getHardwareResetReasonString()
{
  switch (esp_reset_reason())
  {
    case ESP_RST_POWERON:
      return "Power-on";
    case ESP_RST_SW:
      return "Software reset";
    case ESP_RST_PANIC:
      return "Panic (exception)";
    case ESP_RST_INT_WDT:
      return "Interrupt watchdog timeout";
    case ESP_RST_TASK_WDT:
      return "Task watchdog timeout";
    case ESP_RST_WDT:
      return "Other watchdog timeout";
    case ESP_RST_BROWNOUT:
      return "Brownout";
    case ESP_RST_DEEPSLEEP:
      return "Deep-sleep wake";
    case ESP_RST_SDIO:
      return "SDIO reset";
    default:
      return "Unknown";
  }
}

String determineResetReason()
{
  String custom = preferences.getString("resetreason", "");

  if (custom.length() > 0)
  {
    // Single-shot: never let this apply to a future reboot.
    preferences.remove("resetreason");
  }

  if (esp_reset_reason() == ESP_RST_SW && custom.length() > 0)
  {
    return custom;
  }

  return getHardwareResetReasonString();
}

// ============================================================
// DURATION FORMATTING
// ============================================================

String formatDuration(unsigned long ms)
{
  unsigned long totalSeconds = ms / 1000;

  unsigned long days = totalSeconds / 86400;
  unsigned long hours = (totalSeconds % 86400) / 3600;
  unsigned long minutes = (totalSeconds % 3600) / 60;
  unsigned long seconds = totalSeconds % 60;

  String result;

  if (days > 0)
  {
    result += String(days) + "d ";
  }

  if (days > 0 || hours > 0)
  {
    result += String(hours) + "h ";
  }

  if (days > 0 || hours > 0 || minutes > 0)
  {
    result += String(minutes) + "m ";
  }

  result += String(seconds) + "s";

  return result;
}

// ============================================================
// WIFI SIGNAL DESCRIPTION
// ============================================================

String describeRSSI(int rssi)
{
  if (rssi >= -50)
  {
    return "Excellent";
  }

  if (rssi >= -60)
  {
    return "Strong";
  }

  if (rssi >= -70)
  {
    return "Good";
  }

  if (rssi >= -80)
  {
    return "Weak";
  }

  return "Poor";
}

// ============================================================
// HTML ESCAPING
// ============================================================

String htmlEscape(const String &input)
{
  String output;
  output.reserve(input.length() + 16);

  for (size_t i = 0; i < input.length(); i++)
  {
    char c = input[i];

    switch (c)
    {
      case '&':
        output += "&amp;";
        break;

      case '<':
        output += "&lt;";
        break;

      case '>':
        output += "&gt;";
        break;

      case '"':
        output += "&quot;";
        break;

      case '\'':
        output += "&#39;";
        break;

      default:
        output += c;
        break;
    }
  }

  return output;
}

// ============================================================
// LOAD CONFIGURATION
// ============================================================

void loadConfiguration()
{
  preferences.begin("relaynode", false);

  apSSID = preferences.getString("ap_ssid", "ESP32-Relay");

  webUser = preferences.getString("webuser", "admin");
  webPass = preferences.getString("webpass", "admin");

  for (int i = 0; i < MAX_WIFI_NETWORKS; i++)
  {
    String ssidKey = "ssid" + String(i);
    String passKey = "pass" + String(i);

    wifiSSID[i] = preferences.getString(ssidKey.c_str(), "");
    wifiPassword[i] = preferences.getString(passKey.c_str(), "");
  }

  artnetNodeName = preferences.getString("nodename", "ESP32-Relay");

  artnetUniverse = preferences.getUShort("universe", 1);

  relayOuterDMXAddress = preferences.getUShort("dmx1", 1);
  relayInnerDMXAddress = preferences.getUShort("dmx2", 2);

  // Safety validation.

  if (artnetUniverse > 32767)
  {
    artnetUniverse = 0;
  }

  if (relayOuterDMXAddress < 1 || relayOuterDMXAddress > 512)
  {
    relayOuterDMXAddress = 1;
  }

  if (relayInnerDMXAddress < 1 || relayInnerDMXAddress > 512)
  {
    relayInnerDMXAddress = 2;
  }

  if (artnetNodeName.length() == 0)
  {
    artnetNodeName = "ESP32-Relay";
  }

  // Node name is limited to 17 characters and is used for
  // both the Art-Net ShortName and LongName fields.
  if (artnetNodeName.length() > 17)
  {
    artnetNodeName = artnetNodeName.substring(0, 17);
  }

#if DEBUG
  DBG_PRINTLN();
  DBG_PRINTLN("[CONFIG] Configuration loaded.");
  DBG_PRINT("[CONFIG] AP SSID: ");
  DBG_PRINTLN(apSSID);

  DBG_PRINT("[CONFIG] Node name: ");
  DBG_PRINTLN(artnetNodeName);

  DBG_PRINT("[CONFIG] Art-Net Universe: ");
  DBG_PRINTLN(artnetUniverse);

  DBG_PRINT("[CONFIG] Relay 1 DMX address: ");
  DBG_PRINTLN(relayOuterDMXAddress);

  DBG_PRINT("[CONFIG] Relay 2 DMX address: ");
  DBG_PRINTLN(relayInnerDMXAddress);

  DBG_PRINT("[CONFIG] MAC: ");
  DBG_PRINTLN(getMACString());

  DBG_PRINT("[CONFIG] Serial: ");
  DBG_PRINTLN(getSerialNumber());

  for (int i = 0; i < MAX_WIFI_NETWORKS; i++)
  {
    if (wifiSSID[i].length() > 0)
    {
      DBG_PRINT("[CONFIG] WiFi ");
      DBG_PRINT(i + 1);
      DBG_PRINT(": ");
      DBG_PRINT(wifiSSID[i]);
      DBG_PRINT(" / password length ");
      DBG_PRINTLN(wifiPassword[i].length());
    }
  }
#endif
}

// ============================================================
// SAVE CONFIGURATION
// ============================================================

void saveConfiguration()
{
  preferences.putString("ap_ssid", apSSID);

  preferences.putString("webuser", webUser);
  preferences.putString("webpass", webPass);

  for (int i = 0; i < MAX_WIFI_NETWORKS; i++)
  {
    String ssidKey = "ssid" + String(i);
    String passKey = "pass" + String(i);

    preferences.putString(ssidKey.c_str(), wifiSSID[i]);
    preferences.putString(passKey.c_str(), wifiPassword[i]);
  }

  preferences.putString("nodename", artnetNodeName);

  preferences.putUShort("universe", artnetUniverse);

  preferences.putUShort("dmx1", relayOuterDMXAddress);
  preferences.putUShort("dmx2", relayInnerDMXAddress);

#if DEBUG
  DBG_PRINTLN("[CONFIG] Configuration saved.");
#endif
}

// ============================================================
// WIFI CONNECTION
// ============================================================

bool tryWiFiNetwork(int index)
{
  if (index < 0 || index >= MAX_WIFI_NETWORKS)
  {
    return false;
  }

  if (wifiSSID[index].length() == 0)
  {
    return false;
  }

#if DEBUG
  DBG_PRINT("[WIFI] Trying network ");
  DBG_PRINT(index + 1);
  DBG_PRINT(": ");
  DBG_PRINTLN(wifiSSID[index]);
#endif

  WiFi.mode(WIFI_STA);

  // Disconnect from the previous network without erasing
  // the saved station configuration.
  WiFi.disconnect(false, false);

  delay(100);

  WiFi.begin(
    wifiSSID[index].c_str(),
    wifiPassword[index].c_str()
  );

  unsigned long startTime = millis();

  while (millis() - startTime < CONNECT_TIMEOUT_MS)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
#if DEBUG
      DBG_PRINT("[WIFI] Connected to: ");
      DBG_PRINTLN(wifiSSID[index]);

      DBG_PRINT("[WIFI] IP address: ");
      DBG_PRINTLN(WiFi.localIP());

      DBG_PRINT("[WIFI] Gateway: ");
      DBG_PRINTLN(WiFi.gatewayIP());

      DBG_PRINT("[WIFI] RSSI: ");
      DBG_PRINTLN(WiFi.RSSI());
#endif

      currentNetwork = index;
      hasConnectedToWiFi = true;
      accessPointMode = false;

      // Keep WiFi power-save disabled once connected, so periodic
      // broadcast traffic (e.g. an Art-Net controller's ArtPoll)
      // isn't at risk of being missed around DTIM beacons.
      WiFi.setSleep(false);

      startArtNet();

      return true;
    }

    delay(100);
  }

#if DEBUG
  DBG_PRINT("[WIFI] Failed to connect to: ");
  DBG_PRINTLN(wifiSSID[index]);
#endif

  WiFi.disconnect(false, false);

  return false;
}

// ============================================================
// CONNECT TO SAVED NETWORKS
// ============================================================

void connectToSavedNetworks()
{
  accessPointMode = false;

  for (int i = 0; i < MAX_WIFI_NETWORKS; i++)
  {
    if (tryWiFiNetwork(i))
    {
      return;
    }
  }

  startAccessPoint();
}

// ============================================================
// ACCESS POINT
// ============================================================

void startAccessPoint()
{
  stopArtNet();

  accessPointMode = true;

  // WIFI_AP_STA rather than plain WIFI_AP: this keeps the station
  // interface initialised (even though it isn't associated to
  // anything) alongside the fallback AP. Without it,
  // WiFi.macAddress() returns all zeros while running as an
  // access point, because its underlying call requires the STA
  // interface to have been started.
  WiFi.mode(WIFI_AP_STA);

  bool result = WiFi.softAP(apSSID.c_str());

#if DEBUG
  DBG_PRINT("[WIFI] Starting access point: ");
  DBG_PRINTLN(apSSID);

  DBG_PRINT("[WIFI] AP start result: ");
  DBG_PRINTLN(result ? "SUCCESS" : "FAILED");

  DBG_PRINT("[WIFI] AP IP: ");
  DBG_PRINTLN(WiFi.softAPIP());
#endif
}

// ============================================================
// WIFI RECOVERY
// ============================================================

void startWiFiRecovery()
{
  if (wifiRecoveryActive)
  {
    return;
  }

#if DEBUG
  DBG_PRINTLN("[WIFI] WiFi connection lost.");
  DBG_PRINTLN("[WIFI] Starting recovery.");
#endif

  stopArtNet();

  wifiRecoveryActive = true;
  nextNetworkToTry = 0;
  currentNetwork = -1;
}

// ============================================================
// WIFI RECOVERY HANDLER
// ============================================================

void handleWiFiRecovery()
{
  if (!wifiRecoveryActive)
  {
    return;
  }

  // Try all saved networks in sequence.
  // Each attempt can take up to CONNECT_TIMEOUT_MS.
  // Continue cycling indefinitely until one works.

  for (int count = 0; count < MAX_WIFI_NETWORKS; count++)
  {
    int index = nextNetworkToTry;

    nextNetworkToTry++;
    if (nextNetworkToTry >= MAX_WIFI_NETWORKS)
    {
      nextNetworkToTry = 0;
    }

    if (wifiSSID[index].length() == 0)
    {
      continue;
    }

    if (tryWiFiNetwork(index))
    {
      wifiRecoveryActive = false;

#if DEBUG
      DBG_PRINTLN("[WIFI] Recovery successful.");
#endif

      return;
    }
  }

#if DEBUG
  DBG_PRINTLN("[WIFI] Recovery cycle failed. Trying again.");
#endif
}

// ============================================================
// WIFI MONITOR
// ============================================================

void handleWiFiMonitor()
{
  if (accessPointMode)
  {
    return;
  }

  if (!hasConnectedToWiFi)
  {
    return;
  }

  if (wifiRecoveryActive)
  {
    return;
  }

  if (millis() - lastWiFiCheck < WIFI_CHECK_INTERVAL_MS)
  {
    return;
  }

  lastWiFiCheck = millis();

  if (WiFi.status() != WL_CONNECTED)
  {
    startWiFiRecovery();
  }
}

// ============================================================
// ART-NET START
// ============================================================

void startArtNet()
{
  if (artnetStarted)
  {
    return;
  }

  if (WiFi.status() != WL_CONNECTED)
  {
#if DEBUG_ARTNET
    ARTNET_PRINTLN("[ARTNET] Cannot start: WiFi is not connected.");
#endif
    return;
  }

  if (artnetUDP.begin(ARTNET_PORT))
  {
    artnetStarted = true;

#if DEBUG_ARTNET
    ARTNET_PRINT("[ARTNET] UDP listening on port ");
    ARTNET_PRINTLN(ARTNET_PORT);

    ARTNET_PRINT("[ARTNET] Universe: ");
    ARTNET_PRINTLN(artnetUniverse);

    ARTNET_PRINT("[ARTNET] Relay 1 DMX address: ");
    ARTNET_PRINTLN(relayOuterDMXAddress);

    ARTNET_PRINT("[ARTNET] Relay 2 DMX address: ");
    ARTNET_PRINTLN(relayInnerDMXAddress);
#endif
  }
  else
  {
#if DEBUG_ARTNET
    ARTNET_PRINTLN("[ARTNET] Failed to start UDP.");
#endif
  }
}

// ============================================================
// ART-NET STOP
// ============================================================

void stopArtNet()
{
  if (!artnetStarted)
  {
    return;
  }

  artnetUDP.stop();
  artnetStarted = false;

#if DEBUG_ARTNET
  ARTNET_PRINTLN("[ARTNET] UDP stopped.");
#endif
}

// ============================================================
// ART-NET HELPERS
// ============================================================

uint16_t readUInt16LE(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

uint16_t readUInt16BE(const uint8_t *data)
{
  return ((uint16_t)data[0] << 8) | (uint16_t)data[1];
}

void writeUInt16LE(uint8_t *data, uint16_t value)
{
  data[0] = value & 0xFF;
  data[1] = (value >> 8) & 0xFF;
}

void copyFixedString(
  uint8_t *destination,
  size_t destinationSize,
  const String &source
)
{
  memset(destination, 0, destinationSize);

  size_t length = source.length();

  if (length >= destinationSize)
  {
    length = destinationSize - 1;
  }

  memcpy(destination, source.c_str(), length);
}

// ============================================================
// ART-POLL REPLY
// ============================================================

void sendArtPollReply(IPAddress destination)
{
  // Current Art-Net 4 ArtPollReply length.
  // Fields through BackgroundQueuePolicy plus 10-byte filler.
  uint8_t packet[239];

  memset(packet, 0, sizeof(packet));

  // ----------------------------------------------------------
  // Header
  // ----------------------------------------------------------

  memcpy(packet + 0, "Art-Net\0", 8);

  // OpPollReply = 0x2100, low byte first.
  packet[8] = 0x00;
  packet[9] = 0x21;

  // Node IP address.
  IPAddress ip = WiFi.localIP();

  packet[10] = ip[0];
  packet[11] = ip[1];
  packet[12] = ip[2];
  packet[13] = ip[3];

  // UDP port 0x1936, low byte first.
  packet[14] = 0x36;
  packet[15] = 0x19;

  // Firmware version.
  packet[16] = (ARTNET_FIRMWARE_VERSION >> 8) & 0xFF;
  packet[17] = ARTNET_FIRMWARE_VERSION & 0xFF;

  // ----------------------------------------------------------
  // Port Address
  // ----------------------------------------------------------

  // Net = bits 14-8.
  packet[18] = (artnetUniverse >> 8) & 0x7F;

  // Sub-Net = bits 7-4.
  packet[19] = (artnetUniverse >> 4) & 0x0F;

  // OEM code.
  packet[20] = (ARTNET_OEM_CODE >> 8) & 0xFF;
  packet[21] = ARTNET_OEM_CODE & 0xFF;

  // UBEA version = 0.
  packet[22] = 0;

  // ----------------------------------------------------------
  // Status1
  // ----------------------------------------------------------

  // Bits 7-6 = normal indicator state (11)
  // Bits 5-4 = web/network programmed (10)
  // Bit 1 = RDM not supported (0)
  //
  // 0b11100000 = normal + programmed by web/network.
  packet[23] = 0xE0;

  // ESTA manufacturer code, generic/unassigned here.
  packet[24] = ARTNET_ESTA_MANUFACTURER & 0xFF;
  packet[25] = (ARTNET_ESTA_MANUFACTURER >> 8) & 0xFF;

  // ----------------------------------------------------------
  // Names
  // ----------------------------------------------------------

  // The single configured node name is used for both the
  // ShortName and LongName fields.
  copyFixedString(packet + 26, 18, artnetNodeName);
  copyFixedString(packet + 44, 64, artnetNodeName);

  // ----------------------------------------------------------
  // Node report
  // ----------------------------------------------------------

  char nodeReport[65];

  snprintf(
    nodeReport,
    sizeof(nodeReport),
    "#0001 [%04lu] OK Serial %s",
    artPollReplyCounter % 10000,
    getSerialNumber().c_str()
  );

  copyFixedString(packet + 108, 64, String(nodeReport));

  // ----------------------------------------------------------
  // Number of ports
  // ----------------------------------------------------------

  packet[172] = 0x00;
  packet[173] = 0x01;

  // ----------------------------------------------------------
  // PortTypes
  // ----------------------------------------------------------
  //
  // Bit 7 = output from Art-Net network.
  // Bits 5-0 = Art-Net protocol (000101).
  //
  // 0x80 | 0x05 = 0x85
  //
  // This is one logical Art-Net output port.
  //

  packet[174] = 0x85;

  // Other three ports remain zero.

  // ----------------------------------------------------------
  // GoodInput
  // ----------------------------------------------------------

  // No physical DMX inputs.
  packet[178] = 0x00;

  // ----------------------------------------------------------
  // GoodOutputA
  // ----------------------------------------------------------

  // Art-Net data is being processed by the logical output.
  packet[182] = 0x80;

  // ----------------------------------------------------------
  // SwIn
  // ----------------------------------------------------------

  // No physical DMX inputs.
  packet[186] = 0x00;

  // ----------------------------------------------------------
  // SwOut
  // ----------------------------------------------------------

  // Low 4 bits of Port-Address.
  packet[190] = artnetUniverse & 0x0F;

  // ----------------------------------------------------------
  // sACN priority
  // ----------------------------------------------------------

  packet[194] = 0;

  // SwMacro, SwRemote, spare fields remain zero.

  // ----------------------------------------------------------
  // Style
  // ----------------------------------------------------------

  packet[200] = 0x00;  // StNode

  // ----------------------------------------------------------
  // MAC address
  // ----------------------------------------------------------

  uint8_t mac[6];

  if (getStationMAC(mac))
  {
    for (int i = 0; i < 6; i++)
    {
      packet[201 + i] = mac[i];
    }
  }

  // ----------------------------------------------------------
  // Bind IP
  // ----------------------------------------------------------

  // This is a standalone/root node.
  packet[207] = 0;
  packet[208] = 0;
  packet[209] = 0;
  packet[210] = 0;

  // Root BindIndex.
  packet[211] = 1;

  // ----------------------------------------------------------
  // Status2
  // ----------------------------------------------------------
  //
  // Bit 3 = 15-bit Port-Address support.
  // Bit 2 = DHCP capable.
  // Bit 1 = IP is DHCP configured when DHCP was used.
  // Bit 0 = web configuration available.
  //

  uint8_t status2 = 0;

  status2 |= 0x08;  // 15-bit Port-Address
  status2 |= 0x04;  // DHCP capable

  if (WiFi.getMode() == WIFI_STA)
  {
    // The ESP32 is normally using DHCP when connected
    // to a saved network.
    status2 |= 0x02;
  }

  status2 |= 0x01;  // Web configuration available

  packet[212] = status2;

  // ----------------------------------------------------------
  // GoodOutputB
  // ----------------------------------------------------------
  //
  // Bit 7 = RDM disabled.
  // Bit 6 = continuous output style.
  //

  packet[213] = 0xC0;

  // ----------------------------------------------------------
  // Status3
  // ----------------------------------------------------------

  // No failsafe/RDMnet/background queue features claimed.
  packet[217] = 0x00;

  // ----------------------------------------------------------
  // Default Responder UID
  // ----------------------------------------------------------

  // RDMnet/LLRP not supported, therefore zero.
  memset(packet + 218, 0, 6);

  // User-specific fields.
  memset(packet + 224, 0, 2);

  // Refresh rate.
  // 0 = maximum DMX512-compatible rate.
  packet[226] = 0;
  packet[227] = 0;

  // Background queue policy.
  packet[228] = 4;  // Collection disabled.

  // Bytes 229-238 remain zero as filler.

  artPollReplyCounter++;

  if (artnetUDP.beginPacket(destination, ARTNET_PORT))
  {
    artnetUDP.write(packet, sizeof(packet));
    artnetUDP.endPacket();

#if DEBUG_ARTNET && DEBUG_VERBOSE
    ARTNET_PRINTLN("[ARTNET] ArtPollReply sent.");

    ARTNET_PRINT("[ARTNET] Destination: ");
    ARTNET_PRINTLN(destination);

    ARTNET_PRINT("[ARTNET] Node name: ");
    ARTNET_PRINTLN(artnetNodeName);

    ARTNET_PRINT("[ARTNET] Serial: ");
    ARTNET_PRINTLN(getSerialNumber());

    ARTNET_PRINT("[ARTNET] Universe: ");
    ARTNET_PRINTLN(artnetUniverse);

    ARTNET_PRINT("[ARTNET] Relay 1 DMX address: ");
    ARTNET_PRINTLN(relayOuterDMXAddress);

    ARTNET_PRINT("[ARTNET] Relay 2 DMX address: ");
    ARTNET_PRINTLN(relayInnerDMXAddress);
#endif
  }
  else
  {
#if DEBUG_ARTNET && DEBUG_VERBOSE
    ARTNET_PRINTLN("[ARTNET] Failed to begin ArtPollReply packet.");
#endif
  }
}

// ============================================================
// ART-NET POLL REPLY SCHEDULING
// ============================================================
//
// Queues an ArtPollReply to be sent after a random 0-1000ms delay,
// instead of sending it immediately. This spreads out replies when
// several nodes on the network all receive the same broadcast
// ArtPoll and would otherwise reply at the same instant.

void scheduleArtPollReply(IPAddress destination)
{
  for (int i = 0; i < MAX_PENDING_ARTPOLL_REPLIES; i++)
  {
    if (!pendingArtPollReplies[i].active)
    {
      pendingArtPollReplies[i].active = true;
      pendingArtPollReplies[i].destination = destination;
      pendingArtPollReplies[i].sendAtMs = millis() + (unsigned long)random(0, 1001);

#if DEBUG_ARTNET && DEBUG_VERBOSE
      ARTNET_PRINT("[ARTNET] ArtPollReply to ");
      ARTNET_PRINT(destination);
      ARTNET_PRINT(" scheduled in ");
      ARTNET_PRINT(pendingArtPollReplies[i].sendAtMs - millis());
      ARTNET_PRINTLN("ms.");
#endif
      return;
    }
  }

#if DEBUG_ARTNET && DEBUG_VERBOSE
  ARTNET_PRINTLN("[ARTNET] ArtPollReply queue full. Dropping this reply.");
#endif
}

// Called from loop() on every iteration. Sends any queued
// ArtPollReply whose scheduled time has arrived. Cheap to call
// often: it's just a small fixed-size array scan with no blocking.
void handlePendingArtPollReplies()
{
  if (!artnetStarted)
  {
    return;
  }

  unsigned long now = millis();

  for (int i = 0; i < MAX_PENDING_ARTPOLL_REPLIES; i++)
  {
    if (pendingArtPollReplies[i].active &&
        (long)(now - pendingArtPollReplies[i].sendAtMs) >= 0)
    {
      sendArtPollReply(pendingArtPollReplies[i].destination);
      pendingArtPollReplies[i].active = false;
    }
  }
}

// ============================================================
// ART-NET RECEIVE
// ============================================================

void handleArtNet()
{
  if (!artnetStarted)
  {
    return;
  }

  int packetSize = artnetUDP.parsePacket();

  if (packetSize <= 0)
  {
    return;
  }

#if DEBUG_ARTNET && DEBUG_VERBOSE
  ARTNET_PRINTLN();
  ARTNET_PRINT("[ARTNET] UDP packet received: ");
  ARTNET_PRINT(packetSize);
  ARTNET_PRINT(" bytes from ");
  ARTNET_PRINT(artnetUDP.remoteIP());
  ARTNET_PRINT(":");
  ARTNET_PRINTLN(artnetUDP.remotePort());
#endif

  uint8_t packet[530];

  int bytesToRead = packetSize;

  if (bytesToRead > (int)sizeof(packet))
  {
    bytesToRead = sizeof(packet);
  }

  int bytesRead = artnetUDP.read(packet, bytesToRead);

  if (bytesRead < 10)
  {
#if DEBUG_ARTNET && DEBUG_VERBOSE
    ARTNET_PRINTLN("[ARTNET] Packet too short. Ignoring.");
#endif
    return;
  }

  if (memcmp(packet, "Art-Net\0", 8) != 0)
  {
#if DEBUG_ARTNET && DEBUG_VERBOSE
    ARTNET_PRINTLN("[ARTNET] Invalid Art-Net header. Ignoring.");
#endif
    return;
  }

  // Recognised as a genuine Art-Net packet - record it for the
  // status page's "time since last Art-Net packet" display,
  // regardless of packet type or whether it's for our universe.
  lastArtnetPacketMs = millis();
  everReceivedArtnetPacket = true;

  uint16_t opcode = readUInt16LE(packet + 8);

#if DEBUG_ARTNET && DEBUG_VERBOSE
  ARTNET_PRINT("[ARTNET] Opcode: 0x");
  if (opcode < 0x1000)
  {
    ARTNET_PRINT("0");
  }
  ARTNET_PRINT(opcode, HEX);
  ARTNET_PRINTLN();
#endif

  // ----------------------------------------------------------
  // ArtPoll
  // ----------------------------------------------------------

  if (opcode == 0x2000)
  {
#if DEBUG_ARTNET && DEBUG_VERBOSE
    ARTNET_PRINTLN("[ARTNET] ArtPoll received.");
#endif

    // Art-Net 4 requires the ArtPollReply to be unicast to the
    // controller that sent the ArtPoll. It is queued with a random
    // delay rather than sent immediately - see scheduleArtPollReply().
    scheduleArtPollReply(artnetUDP.remoteIP());

    return;
  }

  // ----------------------------------------------------------
  // Only ArtDMX is handled below.
  // ----------------------------------------------------------

  if (opcode != 0x5000)
  {
#if DEBUG_ARTNET && DEBUG_VERBOSE
    ARTNET_PRINTLN("[ARTNET] Packet is not ArtDMX. Ignoring.");
#endif
    return;
  }

  // ArtDMX minimum packet length is 18 bytes.

  if (packetSize < 18 || bytesRead < 18)
  {
#if DEBUG_ARTNET && DEBUG_VERBOSE
    ARTNET_PRINTLN("[ARTNET] ArtDMX packet too short. Ignoring.");
#endif
    return;
  }

  uint16_t protocolVersion = readUInt16BE(packet + 10);

  uint8_t sequence = packet[12];

  uint16_t universe = readUInt16LE(packet + 14);

  uint16_t dmxLength = readUInt16BE(packet + 16);

#if DEBUG_ARTNET && DEBUG_VERBOSE
  ARTNET_PRINT("[ARTNET] Protocol version: ");
  ARTNET_PRINTLN(protocolVersion);

  ARTNET_PRINT("[ARTNET] Sequence: ");
  ARTNET_PRINTLN(sequence);

  ARTNET_PRINT("[ARTNET] Universe: ");
  ARTNET_PRINTLN(universe);

  ARTNET_PRINT("[ARTNET] DMX length: ");
  ARTNET_PRINTLN(dmxLength);
#endif

  if (dmxLength < 2 || dmxLength > 512 || (dmxLength & 1))
  {
#if DEBUG_ARTNET && DEBUG_VERBOSE
    ARTNET_PRINTLN("[ARTNET] Invalid DMX length. Ignoring.");
#endif
    return;
  }

  if (packetSize < (18 + dmxLength))
  {
#if DEBUG_ARTNET && DEBUG_VERBOSE
    ARTNET_PRINTLN("[ARTNET] Packet does not contain complete DMX payload.");
#endif
    return;
  }

  if (bytesRead < (18 + dmxLength))
  {
#if DEBUG_ARTNET && DEBUG_VERBOSE
    ARTNET_PRINTLN("[ARTNET] Buffer does not contain complete DMX payload.");
#endif
    return;
  }

  // Ignore universes that are not configured for this node.
  if (universe != artnetUniverse)
  {
#if DEBUG_ARTNET && DEBUG_VERBOSE
    ARTNET_PRINT("[ARTNET] Universe ");
    ARTNET_PRINT(universe);
    ARTNET_PRINT(" does not match configured universe ");
    ARTNET_PRINT(artnetUniverse);
    ARTNET_PRINTLN(". Ignoring.");
#endif
    return;
  }

  // ----------------------------------------------------------
  // Relay 1
  // ----------------------------------------------------------

  if (relayOuterDMXAddress >= 1 &&
      relayOuterDMXAddress <= 512 &&
      relayOuterDMXAddress <= dmxLength)
  {
    static unsigned long relayOuterLastChangeMs = 0;

    uint8_t dmxValue = packet[18 + relayOuterDMXAddress - 1];
    bool newState = dmxValue > 0;
    bool currentState = digitalRead(RELAY_OUTER) == HIGH;

    if (newState != currentState)
    {
      if (millis() - relayOuterLastChangeMs >= RELAY_DWELL_MS)
      {
        digitalWrite(
          RELAY_OUTER,
          newState ? HIGH : LOW
        );

        relayOuterLastChangeMs = millis();

#if DEBUG_ARTNET
        ARTNET_PRINT("[ARTNET] Relay 1 state changed: ");
        ARTNET_PRINT(newState ? "OFF -> ON" : "ON -> OFF");
        ARTNET_PRINT(" (DMX ");
        ARTNET_PRINT(relayOuterDMXAddress);
        ARTNET_PRINT(" = ");
        ARTNET_PRINT(dmxValue);
        ARTNET_PRINTLN(")");
#endif
      }
#if DEBUG_ARTNET && DEBUG_VERBOSE
      else
      {
        ARTNET_PRINTLN("[ARTNET] Relay 1 change suppressed (dwell time).");
      }
#endif
    }
  }

  // ----------------------------------------------------------
  // Relay 2
  // ----------------------------------------------------------

  if (relayInnerDMXAddress >= 1 &&
      relayInnerDMXAddress <= 512 &&
      relayInnerDMXAddress <= dmxLength)
  {
    static unsigned long relayInnerLastChangeMs = 0;

    uint8_t dmxValue = packet[18 + relayInnerDMXAddress - 1];
    bool newState = dmxValue > 0;
    bool currentState = digitalRead(RELAY_INNER) == HIGH;

    if (newState != currentState)
    {
      if (millis() - relayInnerLastChangeMs >= RELAY_DWELL_MS)
      {
        digitalWrite(
          RELAY_INNER,
          newState ? HIGH : LOW
        );

        relayInnerLastChangeMs = millis();

#if DEBUG_ARTNET
        ARTNET_PRINT("[ARTNET] Relay 2 state changed: ");
        ARTNET_PRINT(newState ? "OFF -> ON" : "ON -> OFF");
        ARTNET_PRINT(" (DMX ");
        ARTNET_PRINT(relayInnerDMXAddress);
        ARTNET_PRINT(" = ");
        ARTNET_PRINT(dmxValue);
        ARTNET_PRINTLN(")");
#endif
      }
#if DEBUG_ARTNET && DEBUG_VERBOSE
      else
      {
        ARTNET_PRINTLN("[ARTNET] Relay 2 change suppressed (dwell time).");
      }
#endif
    }
  }
}

// ============================================================
// WEB ROOT
// ============================================================

// Shared dark-theme stylesheet for the configuration page and
// the post-save reboot page. Kept as a single constant so both
// pages stay visually consistent without duplicating the rules.
const char PAGE_STYLE[] PROGMEM = R"CSS(
:root {
    --bg: #0f1216;
    --panel-bg: #171b21;
    --border: #2a2f37;
    --text: #e6e9ee;
    --text-muted: #a7e6ff;
    --accent: #8cb7da;
    --accent-text: #0b1117;
    --radius: 8px;
}

* {
    box-sizing: border-box;
}

body {
    background: var(--bg);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    line-height: 1.5;
    margin: 0;
    padding: 24px 16px 64px;
}

.page {
    max-width: 640px;
    margin: 0 auto;
}

.page__title {
    font-size: 1.6rem;
    font-weight: 600;
    margin: 0 0 20px;
}

.status {
    background: var(--panel-bg);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 16px;
    margin: 0 0 24px;
}

.status__row {
    display: flex;
    justify-content: space-between;
    gap: 12px;
    padding: 4px 0;
    font-size: 1rem;
}

.status__label {
    color: var(--text-muted);
}

.status__value {
    font-weight: 500;
    text-align: right;
    word-break: break-all;
}

.panel {
    background: var(--panel-bg);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 16px 16px 4px;
    margin: 0 0 20px;
}

.panel__title {
    font-size: .9rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: var(--text-muted);
    padding: 0 0 12px;
    margin: 0 0 12px;
    border-bottom: 1px solid var(--border);
}

.field {
    margin: 0 0 16px;
}

.field__label {
    display: block;
    font-size: 0.85rem;
    color: var(--text-muted);
    margin: 0 0 6px;
}

.field__input {
    width: 100%;
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 6px;
    color: var(--text);
    padding: 10px 12px;
    font-size: 0.95rem;
}

.field__input:focus {
    outline: none;
    border-color: var(--accent);
}

.password-field {
    display: flex;
    align-items: center;
    gap: 10px;
}

.password-field__input {
    flex: 1;
}

.password-field__toggle {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 0.82rem;
    color: var(--text-muted);
    white-space: nowrap;
}

.btn {
    display: inline-block;
    background: var(--accent);
    color: var(--accent-text);
    border: none;
    border-radius: 6px;
    padding: 12px 20px;
    font-size: 0.95rem;
    font-weight: 600;
    cursor: pointer;
    margin: 4px 8px 24px 0;
}

.btn:hover {
    filter: brightness(1.08);
}

.btn--secondary {
    background: transparent;
    color: var(--text-muted);
    border: 1px solid var(--border);
}

.btn--secondary:hover {
    filter: none;
    border-color: var(--text-muted);
}
)CSS";

void handleRoot()
{
  logWebRequest();

  if (!server.authenticate(webUser.c_str(), webPass.c_str()))
  {
    return server.requestAuthentication();
  }

  String html;
  html.reserve(8192);

  html += "<!DOCTYPE html>";
  html += "<html>";
  html += "<head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta name='color-scheme' content='dark'>";
  html += "<title>ESP32 Relay Node</title>";
  html += "<style>";
  html += FPSTR(PAGE_STYLE);
  html += "</style>";
  html += "</head>";
  html += "<body>";

  html += "<div class='page'>";
  html += "<h1 class='page__title'>ESP32 Relay Art-Net Node</h1>";

  // ----------------------------------------------------------
  // Status
  // ----------------------------------------------------------

  html += "<div class='status'>";

  html += "<div class='status__row'><span class='status__label'>MAC</span><span class='status__value'>";
  html += htmlEscape(getMACString());
  html += "</span></div>";

  html += "<div class='status__row'><span class='status__label'>Serial</span><span class='status__value'>";
  html += htmlEscape(getSerialNumber());
  html += "</span></div>";

  html += "<div class='status__row'><span class='status__label'>WiFi mode</span><span class='status__value'>";
  html += accessPointMode ? "Access Point" : "Station";
  html += "</span></div>";

  if (WiFi.status() == WL_CONNECTED)
  {
    html += "<div class='status__row'><span class='status__label'>IP</span><span class='status__value'>";
    html += WiFi.localIP().toString();
    html += "</span></div>";

    html += "<div class='status__row'><span class='status__label'>Gateway</span><span class='status__value'>";
    html += WiFi.gatewayIP().toString();
    html += "</span></div>";

    html += "<div class='status__row'><span class='status__label'>Connected network</span><span class='status__value'>";
    html += htmlEscape(WiFi.SSID());
    html += "</span></div>";

    html += "<div class='status__row'><span class='status__label'>Signal strength</span><span class='status__value'>";
    html += String(WiFi.RSSI());
    html += " dBm (" + describeRSSI(WiFi.RSSI()) + ")</span></div>";
  }
  else if (accessPointMode)
  {
    html += "<div class='status__row'><span class='status__label'>AP IP</span><span class='status__value'>";
    html += WiFi.softAPIP().toString();
    html += "</span></div>";
  }

  html += "<div class='status__row'><span class='status__label'>Uptime</span><span class='status__value'>";
  html += formatDuration(millis());
  html += "</span></div>";

  html += "<div class='status__row'><span class='status__label'>Last reset reason</span><span class='status__value'>";
  html += htmlEscape(bootResetReasonText);
  html += "</span></div>";

  html += "<div class='status__row'><span class='status__label'>Last Art-Net packet</span><span class='status__value'>";
  if (everReceivedArtnetPacket)
  {
    html += formatDuration(millis() - lastArtnetPacketMs) + " ago";
  }
  else
  {
    html += "Never";
  }
  html += "</span></div>";

  html += "<div class='status__row'><span class='status__label'>Relay 1</span><span class='status__value'>";
  html += (digitalRead(RELAY_OUTER) == HIGH) ? "ON" : "OFF";
  html += "</span></div>";

  html += "<div class='status__row'><span class='status__label'>Relay 2</span><span class='status__value'>";
  html += (digitalRead(RELAY_INNER) == HIGH) ? "ON" : "OFF";
  html += "</span></div>";

  html += "</div>";

  html += "<form method='POST' action='/save'>";

  // ----------------------------------------------------------
  // Art-Net
  // ----------------------------------------------------------

  html += "<div class='panel'>";
  html += "<div class='panel__title'>Art-Net</div>";

  html += "<div class='field'>";
  html += "<label class='field__label' for='nodename'>Node Name (maximum 17 characters)</label>";
  html += "<input class='field__input' type='text' id='nodename' name='nodename' required minlength='1' maxlength='17' value='";
  html += htmlEscape(artnetNodeName);
  html += "'>";
  html += "</div>";

  html += "<div class='field'>";
  html += "<label class='field__label' for='universe'>Art-Net Universe / Port-Address (0-32767)</label>";
  html += "<input class='field__input' type='number' id='universe' name='universe' required min='0' max='32767' value='";
  html += String(artnetUniverse);
  html += "'>";
  html += "</div>";

  html += "<div class='field'>";
  html += "<label class='field__label' for='dmx1'>Relay 1 DMX Address (1-512)</label>";
  html += "<input class='field__input' type='number' id='dmx1' name='dmx1' required min='1' max='512' value='";
  html += String(relayOuterDMXAddress);
  html += "'>";
  html += "</div>";

  html += "<div class='field'>";
  html += "<label class='field__label' for='dmx2'>Relay 2 DMX Address (1-512)</label>";
  html += "<input class='field__input' type='number' id='dmx2' name='dmx2' required min='1' max='512' value='";
  html += String(relayInnerDMXAddress);
  html += "'>";
  html += "</div>";

  html += "</div>";

  // ----------------------------------------------------------
  // WiFi
  // ----------------------------------------------------------

  html += "<div class='panel'>";
  html += "<div class='panel__title'>WiFi Networks</div>";

  for (int i = 0; i < MAX_WIFI_NETWORKS; i++)
  {
    String ssidId = "ssid" + String(i);
    String passId = "pass" + String(i);

    html += "<div class='field'>";
    html += "<label class='field__label' for='" + ssidId + "'>Network " + String(i + 1) + " SSID</label>";
    html += "<input class='field__input' type='text' id='" + ssidId + "' name='" + ssidId + "' maxlength='32' value='";
    html += htmlEscape(wifiSSID[i]);
    html += "'>";
    html += "</div>";

    html += "<div class='field'>";
    html += "<label class='field__label' for='" + passId + "'>Password</label>";
    html += "<div class='password-field'>";
    html += "<input class='field__input password-field__input' type='password' id='" + passId + "' name='" + passId + "' maxlength='32' value='";
    html += htmlEscape(wifiPassword[i]);
    html += "'>";
    html += "<label class='password-field__toggle'><input type='checkbox' onchange=\"togglePassword('" + passId + "', this.checked)\"> Show</label>";
    html += "</div>";
    html += "</div>";
  }

  html += "<div class='field'>";
  html += "<label class='field__label' for='apssid'>Access Point SSID</label>";
  html += "<input class='field__input' type='text' id='apssid' name='apssid' required minlength='1' maxlength='32' value='";
  html += htmlEscape(apSSID);
  html += "'>";
  html += "</div>";

  html += "</div>";

  // ----------------------------------------------------------
  // Web authentication
  // ----------------------------------------------------------

  html += "<div class='panel'>";
  html += "<div class='panel__title'>Web Authentication</div>";

  html += "<div class='field'>";
  html += "<label class='field__label' for='webuser'>Username</label>";
  html += "<input class='field__input' type='text' id='webuser' name='webuser' required minlength='1' maxlength='32' value='";
  html += htmlEscape(webUser);
  html += "'>";
  html += "</div>";

  html += "<div class='field'>";
  html += "<label class='field__label' for='webpass'>Password</label>";
  html += "<div class='password-field'>";
  html += "<input class='field__input password-field__input' type='password' id='webpass' name='webpass' required minlength='1' maxlength='32' value='";
  html += htmlEscape(webPass);
  html += "'>";
  html += "<label class='password-field__toggle'><input type='checkbox' onchange=\"togglePassword('webpass', this.checked)\"> Show</label>";
  html += "</div>";
  html += "</div>";

  html += "</div>";

  html += "<button class='btn' type='submit' onclick=\"return confirm('Save the new configuration and reboot the node now?');\">Save &amp; Reboot</button>";
  html += "<button class='btn btn--secondary' type='submit' formaction='/discard' formnovalidate onclick=\"return confirm('Discard any changes and reboot the node now? Unsaved changes will be lost.');\">Discard &amp; Reboot</button>";

  html += "</form>";
  html += "</div>";

  html += "<script>";
  html += "function togglePassword(id, show) {";
  html += "    var el = document.getElementById(id);";
  html += "    el.type = show ? 'text' : 'password';";
  html += "}";
  html += "</script>";

  html += "</body>";
  html += "</html>";

  server.send(200, "text/html", html);
}

// ============================================================
// WEB SAVE
// ============================================================

// ============================================================
// REBOOT PAGE
// ============================================================
//
// Shared "rebooting now" page used by both Save & Reboot and
// Discard & Reboot, so the two stay visually identical and the
// poll-and-redirect behaviour only needs to be written once.

String buildRebootPage(const char *heading, const char *message)
{
  String html;

  html += "<!DOCTYPE html>";
  html += "<html>";
  html += "<head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta name='color-scheme' content='dark'>";
  html += "<title>Rebooting...</title>";
  html += "<style>";
  html += FPSTR(PAGE_STYLE);
  html += "</style>";
  html += "</head>";
  html += "<body>";
  html += "<div class='page'>";
  html += "<h1 class='page__title'>";
  html += heading;
  html += "</h1>";
  html += "<div class='status'>";
  html += "<p>";
  html += message;
  html += "</p>";
  html += "<p id='pollStatus'>Waiting for the node to come back online&hellip;</p>";
  html += "</div>";
  html += "</div>";

  // Poll a lightweight, unauthenticated endpoint until the node
  // responds again, then move on to the main page. This avoids
  // the user having to manually refresh (which would otherwise
  // resubmit this POST) or landing on a stale page.
  html += "<script>";
  html += "function poll() {";
  html += "    fetch('/ping', { cache: 'no-store' })";
  html += "        .then(function (response) {";
  html += "            if (response.ok) {";
  html += "                window.location.href = '/';";
  html += "            } else {";
  html += "                setTimeout(poll, 1000);";
  html += "            }";
  html += "        })";
  html += "        .catch(function () {";
  html += "            setTimeout(poll, 1000);";
  html += "        });";
  html += "}";
  html += "setTimeout(poll, 2000);";
  html += "</script>";

  html += "</body>";
  html += "</html>";

  return html;
}

void handleSave()
{
  logWebRequest();

  if (!server.authenticate(webUser.c_str(), webPass.c_str()))
  {
    return server.requestAuthentication();
  }

  // WiFi networks. Empty SSID/password is valid (0 bytes) - it
  // marks the slot unused/open. Anything over the 32-byte limit is
  // rejected outright, leaving the previously stored value in place.

  for (int i = 0; i < MAX_WIFI_NETWORKS; i++)
  {
    String ssidName = "ssid" + String(i);
    String passName = "pass" + String(i);

    if (server.hasArg(ssidName))
    {
      String newSSID = server.arg(ssidName);

      if (newSSID.length() <= 32)
      {
        wifiSSID[i] = newSSID;
      }
    }

    if (server.hasArg(passName))
    {
      String newPassword = server.arg(passName);

      if (newPassword.length() <= 32)
      {
        wifiPassword[i] = newPassword;
      }
    }
  }

  // AP SSID: required, 1-32 bytes.

  if (server.hasArg("apssid"))
  {
    String newAPSSID = server.arg("apssid");

    if (newAPSSID.length() >= 1 && newAPSSID.length() <= 32)
    {
      apSSID = newAPSSID;
    }
  }

  // Web credentials: required, 1-32 bytes each.

  if (server.hasArg("webuser"))
  {
    String newWebUser = server.arg("webuser");

    if (newWebUser.length() >= 1 && newWebUser.length() <= 32)
    {
      webUser = newWebUser;
    }
  }

  if (server.hasArg("webpass"))
  {
    String newWebPass = server.arg("webpass");

    if (newWebPass.length() >= 1 && newWebPass.length() <= 32)
    {
      webPass = newWebPass;
    }
  }

  // Node name: required, 1-17 bytes. Rejected outright rather than
  // truncated, so an oversized submission never silently mutates
  // into something the user didn't type.

  if (server.hasArg("nodename"))
  {
    String newNodeName = server.arg("nodename");

    if (newNodeName.length() >= 1 && newNodeName.length() <= 17)
    {
      artnetNodeName = newNodeName;
    }
  }

  // Art-Net Universe.

  if (server.hasArg("universe"))
  {
    long value = server.arg("universe").toInt();

    if (value >= 0 && value <= 32767)
    {
      artnetUniverse = (uint16_t)value;
    }
  }

  // Relay 1 DMX address.

  if (server.hasArg("dmx1"))
  {
    long value = server.arg("dmx1").toInt();

    if (value >= 1 && value <= 512)
    {
      relayOuterDMXAddress = (uint16_t)value;
    }
  }

  // Relay 2 DMX address.

  if (server.hasArg("dmx2"))
  {
    long value = server.arg("dmx2").toInt();

    if (value >= 1 && value <= 512)
    {
      relayInnerDMXAddress = (uint16_t)value;
    }
  }

  saveConfiguration();

  preferences.putString("resetreason", "Configuration change (web interface)");

#if DEBUG
  DBG_PRINTLN("[CONFIG] Web configuration updated.");
  DBG_PRINTLN("[CONFIG] Rebooting to apply new configuration.");
#endif

  String html = buildRebootPage(
    "Configuration Saved",
    "The new settings have been saved. The node is rebooting to apply them."
  );

  server.send(200, "text/html", html);

  delay(500);

  ESP.restart();
}

// ============================================================
// WEB DISCARD
// ============================================================
//
// Discards whatever was submitted in the form (it is never read
// or applied here) and reboots, restoring the last saved
// configuration on next boot.

void handleDiscard()
{
  logWebRequest();

  if (!server.authenticate(webUser.c_str(), webPass.c_str()))
  {
    return server.requestAuthentication();
  }

  preferences.putString("resetreason", "Manual reboot (discarded changes)");

#if DEBUG
  DBG_PRINTLN("[CONFIG] Changes discarded. Rebooting without saving.");
#endif

  String html = buildRebootPage(
    "Changes Discarded",
    "No changes were saved. The node is rebooting with its existing configuration."
  );

  server.send(200, "text/html", html);

  delay(500);

  ESP.restart();
}

// ============================================================
// WEB PING
// ============================================================
//
// Lightweight, unauthenticated endpoint used by the Save & Reboot
// page to detect when the node has come back up after a reboot.
// It deliberately requires no authentication so the browser does
// not show a login prompt while polling in the background.

void handlePing()
{
  logWebRequest();

  server.send(200, "text/plain", "OK");
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);

  delay(500);

#if DEBUG
  DBG_PRINTLN();
  DBG_PRINTLN("========================================");
  DBG_PRINTLN("ESP32 Relay Art-Net Node");
  DBG_PRINTLN("========================================");
#endif

  // ----------------------------------------------------------
  // Relay outputs
  // ----------------------------------------------------------

  pinMode(RELAY_OUTER, OUTPUT);
  pinMode(RELAY_INNER, OUTPUT);

  // Relays are active HIGH.
  // Always start with both OFF.

  digitalWrite(RELAY_OUTER, LOW);
  digitalWrite(RELAY_INNER, LOW);

#if DEBUG
  DBG_PRINTLN("[GPIO] Relay 1 GPIO16 = OFF");
  DBG_PRINTLN("[GPIO] Relay 2 GPIO17 = OFF");
#endif

  // ----------------------------------------------------------
  // WiFi
  // ----------------------------------------------------------

  WiFi.mode(WIFI_STA);

  // Disable WiFi modem sleep. With power-save enabled, the ESP32
  // radio can miss periodic broadcast/UDP traffic (such as an
  // Art-Net controller's ArtPoll discovery broadcasts) because it
  // is only awake around each DTIM beacon. Continuous unicast
  // ArtDMX traffic is largely unaffected, which is why direct
  // control can work while broadcast-based discovery does not.
  WiFi.setSleep(false);

  loadConfiguration();

  // Preferences are open at this point (loadConfiguration() calls
  // preferences.begin()), so this can safely read/clear the
  // one-shot reset-reason flag written by handleSave()/handleDiscard().
  bootResetReasonText = determineResetReason();

#if DEBUG
  DBG_PRINT("[STATUS] Last reset reason: ");
  DBG_PRINTLN(bootResetReasonText);

  DBG_PRINTLN("[WIFI] Attempting saved networks...");
#endif

  connectToSavedNetworks();

  // ----------------------------------------------------------
  // Web server
  // ----------------------------------------------------------

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/discard", HTTP_POST, handleDiscard);
  server.on("/ping", HTTP_GET, handlePing);

  server.begin();

#if DEBUG
  DBG_PRINTLN("[WEB] HTTP server started.");
#endif

  // ----------------------------------------------------------
  // Final status
  // ----------------------------------------------------------

#if DEBUG
  DBG_PRINTLN();
  DBG_PRINTLN("[STATUS] Startup complete.");

  DBG_PRINT("[STATUS] MAC: ");
  DBG_PRINTLN(getMACString());

  DBG_PRINT("[STATUS] Serial: ");
  DBG_PRINTLN(getSerialNumber());

  if (accessPointMode)
  {
    DBG_PRINT("[STATUS] AP IP: ");
    DBG_PRINTLN(WiFi.softAPIP());
  }
  else if (WiFi.status() == WL_CONNECTED)
  {
    DBG_PRINT("[STATUS] Station IP: ");
    DBG_PRINTLN(WiFi.localIP());
  }

  DBG_PRINTLN();
#endif
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
  server.handleClient();

  // Art-Net remains active while connected.
  if (!wifiRecoveryActive)
  {
    handleArtNet();
    handlePendingArtPollReplies();
  }

  // Monitor the established WiFi connection.
  handleWiFiMonitor();

  // If the connection has been lost, cycle through the saved
  // networks until one succeeds.
  if (wifiRecoveryActive)
  {
    handleWiFiRecovery();
  }
}