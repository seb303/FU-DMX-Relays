#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <Update.h>

// ============================================================
// DEBUG
// ============================================================

#define DEBUG 0
#define DEBUG_ARTNET 0
#define DEBUG_VERBOSE 0

// The in-memory debug log, and the web interface panel that shows it,
// exist only when DEBUG or DEBUG_ARTNET is set. With both of them off,
// none of it is compiled in, so no memory is used for it.
#if DEBUG || DEBUG_ARTNET
    #define DEBUG_LOG_ENABLED 1
#else
    #define DEBUG_LOG_ENABLED 0
#endif

// All debug output goes through debugLog (defined below), which sends it
// to the serial port as it always did and also keeps it for the web
// interface.
#if DEBUG
    #define DBG_PRINT(...) debugLog.print(__VA_ARGS__)
    #define DBG_PRINTLN(...) debugLog.println(__VA_ARGS__)
#else
    #define DBG_PRINT(...)
    #define DBG_PRINTLN(...)
#endif

#if DEBUG_ARTNET
    #define ARTNET_PRINT(...) debugLog.print(__VA_ARGS__)
    #define ARTNET_PRINTLN(...) debugLog.println(__VA_ARGS__)
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

#if DEBUG_LOG_ENABLED

// How much of the most recent debug output is kept in memory for the
// web interface, in bytes. It is a fixed block of RAM that exists only
// in debug builds. 16 KB is roughly 200 lines of typical output; when
// it is full the oldest output is discarded. (With DEBUG_ARTNET and
// DEBUG_VERBOSE together, which log every Art-Net packet, it holds only
// the last few seconds.)
const size_t DEBUG_LOG_BYTES = 16384;

// A Print that sends everything to the serial port (unchanged) and also
// keeps it in a ring buffer, so that DBG_PRINT() and friends can keep
// using the usual print()/println() forms.
//
// In the buffer, each line starts with the time since boot in seconds,
// and line endings are a single "\n". The serial output has neither.
//
// Every byte stored has a position that counts up from 0 for as long as
// the controller runs and is never reused, so the web interface can ask
// for "everything from position N" and only be sent what is new.
//
// There is no locking: this is only ever used from the main loop task,
// so nothing running in another task (such as onWiFiEvent()) may log.
class DebugLog : public Print
{
public:
    using Print::write;

    size_t write(uint8_t c) override
    {
        Serial.write(c);
        store(c);

        return 1;
    }

    size_t write(const uint8_t *data, size_t size) override
    {
        Serial.write(data, size);

        for (size_t i = 0; i < size; i++)
        {
            store(data[i]);
        }

        return size;
    }

    // Returns the part of the log from position "from" onwards, as text
    // (position 0 is the first byte ever stored, so from = 0 asks for
    // everything still held).
    //
    // next is set to the position just after the last byte returned,
    // which is what to ask for next time. gap is set if some of what was
    // asked for is no longer held, in which case the text starts at the
    // oldest complete line that is.
    String readFrom(uint64_t from, uint64_t &next, bool &gap) const
    {
        uint64_t oldest = total - used;

        gap = false;
        next = total;

        // Not a position this log has ever had (e.g. a stale one).
        if (from > total)
        {
            from = 0;
            gap = true;
        }

        size_t skip = 0;

        if (from < oldest)
        {
            gap = true;
            from = oldest;

            // The oldest line is probably cut off part-way, so start at
            // the first complete line.
            while (skip < used && byteAt(oldest + skip) != '\n')
            {
                skip++;
            }

            // Step over the newline itself too.
            skip++;

            if (skip > used)
            {
                skip = used;
            }
        }

        String result;
        result.reserve((size_t)(total - from) - skip + 1);

        for (uint64_t position = from + skip; position < total; position++)
        {
            result += byteAt(position);
        }

        return result;
    }

    // Identifies this run of the controller. It changes on every restart,
    // so a browser holding positions from an earlier run can tell they no
    // longer mean anything. (Chosen the first time it is needed, rather
    // than when the program starts.)
    uint32_t sessionId()
    {
        if (session == 0)
        {
            session = (uint32_t)random(1, 2147483647);
        }

        return session;
    }

private:
    char ring[DEBUG_LOG_BYTES];
    size_t head = 0;            // where the next byte goes
    size_t used = 0;            // bytes held, up to DEBUG_LOG_BYTES
    uint64_t total = 0;         // bytes ever stored: the position after the newest
    uint32_t session = 0;
    bool atLineStart = true;

    // The byte at an absolute position, which must be one still held.
    char byteAt(uint64_t position) const
    {
        return ring[(size_t)(position % DEBUG_LOG_BYTES)];
    }

    // Adds one byte, overwriting the oldest once the buffer is full.
    void append(char c)
    {
        ring[head] = c;
        head = (head + 1) % DEBUG_LOG_BYTES;
        total++;

        if (used < DEBUG_LOG_BYTES)
        {
            used++;
        }
    }

    void store(uint8_t c)
    {
        if (c == '\r')
        {
            return;
        }

        // Stamp the start of each line (but not empty lines).
        if (atLineStart && c != '\n')
        {
            uint64_t ms = (uint64_t)(esp_timer_get_time() / 1000);
            char stamp[24];

            int length = snprintf(
                stamp,
                sizeof(stamp),
                "[%lu.%03u] ",
                (unsigned long)(ms / 1000),
                (unsigned int)(ms % 1000)
            );

            for (int i = 0; i < length; i++)
            {
                append(stamp[i]);
            }
        }

        append((char)c);
        atLineStart = (c == '\n');
    }
};

DebugLog debugLog;

#endif

// ============================================================
// FIRMWARE
// ============================================================

// Firmware version: "major.minor" plus the date and time of the
// last change to this source, as "YYYY-MM-DD HH:MM". It is shown
// on the web interface status panel and printed at startup.
const char FIRMWARE_VERSION[] = "1.0 2026-10-15 22:22";

// Number of leading bytes of an uploaded firmware file that are
// checked to confirm it is an ESP32 application image before
// anything is written to flash (see isValidFirmwareHeader()).
const size_t FIRMWARE_HEADER_BYTES = 36;

// ============================================================
// HARDWARE
// ============================================================

// See SPEC.md for physical relay positions on board.
const int RELAY_1 = 17;
const int RELAY_2 = 16;

// On-board status LED. LED_ON_LEVEL is the GPIO level that lights the
// LED; change only this if the LED turns out to be wired active-low.
const int STATUS_LED = 23;
const int LED_ON_LEVEL = HIGH;
const int LED_OFF_LEVEL = (LED_ON_LEVEL == HIGH) ? LOW : HIGH;

// Minimum time a relay must remain in a given state before it can
// switch again, to protect the relay contacts from excessive wear
// if the controlling DMX channel changes rapidly (e.g. a stray
// strobe/chase effect pointed at this channel by mistake). A change
// requested sooner than this is held back and applied once the time
// has elapsed (see applyRelays()), not discarded.
const unsigned long RELAY_DWELL_MS = 500;

// Status LED patterns. Each pattern is derived from millis() modulo
// its period, so it needs no state and never blocks.
//   Fast flash   - trying to connect to WiFi
//   Solid        - ArtDMX received for the configured universe
//                  within LED_DMX_ACTIVE_MS
//   Occasional   - fallback access point mode, no recent DMX
//   Slow flash   - otherwise (powered on, no recent DMX)
const unsigned long LED_FAST_PERIOD_MS = 200;
const unsigned long LED_FAST_ON_MS = 100;
const unsigned long LED_SLOW_PERIOD_MS = 1000;
const unsigned long LED_SLOW_ON_MS = 500;
const unsigned long LED_AP_PERIOD_MS = 2000;
const unsigned long LED_AP_ON_MS = 100;
// Longer than the 2 second refresh interval of the QLC+ controller, which
// only resends unchanged DMX data every 2 seconds, so the LED stays solid
// between refreshes.
const unsigned long LED_DMX_ACTIVE_MS = 3000;

// Maximum number of ArtPollReply sends that can be queued at once,
// each with its own randomised send time. Sized generously for a
// handful of controllers/apps polling around the same time; if the
// queue is ever full, the newest poll is simply not replied to (the
// polling controller will just ask again shortly).
const int MAX_PENDING_ARTPOLL_REPLIES = 8;

// Fixed IP configuration for the fallback access point. Forced
// explicitly rather than relying on the WiFi library's own default
// (which happens to already be 192.168.4.1/24, but that's an
// implementation detail we shouldn't depend on going forward).
const IPAddress AP_LOCAL_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY_IP(192, 168, 4, 1);
const IPAddress AP_SUBNET_MASK(255, 255, 255, 0);

// ============================================================
// WIFI
// ============================================================

const int MAX_WIFI_NETWORKS = 3;

// WPA/WPA2 passphrase limits, counted in bytes rather than characters
// (a non-ASCII UTF-8 character takes 2-4 bytes). An empty password
// means an open network.
const size_t WIFI_PASSWORD_MIN_BYTES = 8;
const size_t WIFI_PASSWORD_MAX_BYTES = 63;

// Shown in a password field in place of a stored password, so the real
// one is never sent to the browser (see handleRoot() and handleSave()).
const char PASSWORD_PLACEHOLDER[] = "**********";

// The longest a single attempt to join one WiFi network may take before
// giving up. It applies to every attempt: at startup, when reconnecting
// after the connection is lost, and when leaving fallback access point
// mode. The main loop is blocked while an attempt is in progress, so
// Art-Net and the web interface are unavailable meanwhile. An attempt
// ends sooner if it succeeds, or if the network refuses the credentials
// (see onWiFiEvent()).
const unsigned long CONNECT_TIMEOUT_MS = 10000;

// How often an established WiFi connection is checked (see
// handleWiFiMonitor()). A lost connection is noticed within this long,
// at which point Art-Net stops and reconnection begins.
const unsigned long WIFI_CHECK_INTERVAL_MS = 1000;

// During reconnection, the least time between the start of one cycle
// through the saved networks and the start of the next (see
// handleWiFiRecovery()). It adds no delay to a cycle that is slow
// anyway (a network that is not there takes up to CONNECT_TIMEOUT_MS to
// fail). It is for attempts that fail within a fraction of a second,
// such as a router that now refuses the stored password, which would
// otherwise be retried continuously.
const unsigned long WIFI_RECOVERY_CYCLE_INTERVAL_MS = 5000;

// While running as the fallback access point, how often to scan for
// a configured network having become available (e.g. the router
// rebooting after a power cut, coming up after this controller did).
const unsigned long AP_SCAN_INTERVAL_MS = 30000;

// A normal scan completes in a few seconds. If WiFi.scanComplete()
// ever reports WIFI_SCAN_RUNNING for much longer than that - e.g. a
// dropped WIFI_EVENT_SCAN_DONE, which can happen with concurrent
// AP+STA scanning - treat it as stuck rather than waiting forever.
const unsigned long AP_SCAN_MAX_AGE_MS = 15000;

// If a network refuses our credentials (e.g. a wrong password), wait
// this long before the next scan instead of AP_SCAN_INTERVAL_MS (see
// shouldHoldOffAfter() for the exact rule). Each join attempt takes the
// fallback access point down, so this gives someone connected to it
// time to correct the credentials in the web interface. It is only a
// delay, not a lock-out: a router that merely refused the password
// temporarily while starting up is still picked up afterwards. It
// applies from the moment the fallback access point starts, whether
// that follows the attempts at startup or a recovery attempt.
const unsigned long AP_AUTH_FAIL_HOLD_OFF_MS = 180000;

String wifiSSID[MAX_WIFI_NETWORKS];
String wifiPassword[MAX_WIFI_NETWORKS];

// Default for both the fallback access point SSID and the Art-Net node
// name, used until they are configured and whenever a stored value is
// missing or invalid.
const char DEFAULT_NAME[] = "ESP32-Relay";

String apSSID = DEFAULT_NAME;

String webUser = "admin";
String webPass = "admin";

bool accessPointMode = false;
bool hasConnectedToWiFi = false;

int currentNetwork = -1;
int nextNetworkToTry = 0;

bool wifiRecoveryActive = false;

// When handleWiFiRecovery() may start its next cycle (millis()).
unsigned long nextRecoveryCycleMs = 0;

// True only while tryWiFiNetwork() is waiting on a connection attempt.
// Used by the status LED.
bool wifiConnecting = false;

// AP-mode network scan state (see handleAccessPointRecovery()).
bool apScanInProgress = false;
unsigned long lastApScanMs = 0;
unsigned long apScanStartMs = 0;

// Set by tryWiFiNetwork() to say how the most recent attempt failed
// (both false if it succeeded, or if no attempt was made):
//   lastJoinAuthFailed - the network refused our credentials (e.g. a
//                        wrong password), so it is there.
//   lastJoinNotFound   - no network with that SSID was found at all.
// If neither is set after a failure, it failed some other way, usually
// by timing out.
bool lastJoinAuthFailed = false;
bool lastJoinNotFound = false;

// How the failed attempts of one series (all the networks tried at
// startup, or in one recovery attempt) failed; see shouldHoldOffAfter().
//
// This is defined here, ahead of the first function, because the Arduino
// IDE generates a prototype for every function in the sketch before the
// first one, so a type used in a function's parameters has to be
// declared by then.
struct JoinFailures
{
    int refused;    // the network refused our credentials
    int other;      // any other way, usually a timeout
                    // (networks that were not found are not counted)
};

// Written by onWiFiEvent(), which runs in the WiFi library's event task
// rather than in loop(), hence volatile. wifiCredentialsRefused becomes
// true when the station is disconnected for a reason that means the
// network refused our credentials; wifiLastDisconnectReason is the
// reason code of the most recent disconnect (0 = none since the current
// attempt began), kept for the debug output. wifiNetworkNotFound becomes
// true when the station reports that no network with the wanted SSID
// could be found.
volatile bool wifiCredentialsRefused = false;
volatile bool wifiNetworkNotFound = false;
volatile uint8_t wifiLastDisconnectReason = 0;

// How long handleAccessPointRecovery() waits between scans. Normally
// AP_SCAN_INTERVAL_MS; AP_AUTH_FAIL_HOLD_OFF_MS when a network has
// refused our credentials (see shouldHoldOffAfter()).
unsigned long apScanDelayMs = AP_SCAN_INTERVAL_MS;

unsigned long lastWiFiCheck = 0;

// ============================================================
// ART-NET
// ============================================================

const uint16_t ARTNET_PORT = 6454;

// Global Art-Net Port-Address / Universe.
// Valid Art-Net 4 range is 0-32767 (15-bit Port-Address).
// 0 is a valid value even though deprecated.
//
// Used until a universe is configured, and whenever the stored value is
// missing or out of range.
const uint16_t DEFAULT_UNIVERSE = 1;
uint16_t artnetUniverse = DEFAULT_UNIVERSE;

// Individual DMX addresses for the two relays.
uint16_t relay1DMXAddress = 1;
uint16_t relay2DMXAddress = 2;

// Single configured node name, used for both the Art-Net
// ShortName and LongName fields.
String artnetNodeName = DEFAULT_NAME;

// Generic/unregistered values.
// These are deliberately not presented as a real manufacturer identity.
const uint16_t ARTNET_OEM_CODE = 0xFFFF;
const uint16_t ARTNET_ESTA_MANUFACTURER = 0x0000;

// Firmware revision reported in ArtPollReply.
const uint16_t ARTNET_FIRMWARE_VERSION = 0x0001;

WiFiUDP artnetUDP;
bool artnetStarted = false;

unsigned long artPollReplyCounter = 0;

// Timestamps (uptimeMs()) of the most recently received ArtPoll and
// ArtDMX packets, tracked separately so the status page can show
// whether the node is actually receiving DMX data or only being
// polled. everReceived* distinguishes "never" from a genuine
// 0ms-ago reading. They are 64-bit, like the uptime, so that they
// do not wrap after 49.7 days the way millis() does.
uint64_t lastArtPollMs = 0;
bool everReceivedArtPoll = false;

uint64_t lastArtDmxMs = 0;
bool everReceivedArtDmx = false;

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
void applyRelays();

void startAccessPoint();
void handleAccessPointRecovery();
void updateStatusLed();
bool tryWiFiNetwork(int index);
void connectToSavedNetworks();
void startWiFiRecovery();
void handleWiFiRecovery();
void handleWiFiMonitor();

void sendArtPollReply(IPAddress destination);
void scheduleArtPollReply(IPAddress destination);
void handlePendingArtPollReplies();
void clearPendingArtPollReplies();

String getMACString();
String getSerialNumber();
bool getStationMAC(uint8_t mac[6]);
String getHardwareResetReasonString();
String determineResetReason();
uint64_t uptimeMs();
String formatDuration(uint64_t ms);
String describeRSSI(int rssi);

String htmlEscape(const String &input);
bool isPrintableAscii(const String &input);
bool isValidNodeName(const String &name);
bool isValidWiFiPassword(const String &password);
bool parseDigits(const String &text, long &value);
String getFormValue(const char *name);

void loadConfiguration();
void saveConfiguration();

void handleRoot();
void handleSave();
void handleDiscard();
void handleFirmwareUpdate();
void handleFirmwareUpload();
bool isFirmwareUpdateAllowed();
void handlePing();
void handleNotFound();
void logWebRequest();

#if DEBUG_LOG_ENABLED
void handleLog();
#endif

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
// WiFi.macAddress() returns the correct value in every mode - but only
// once WiFi has been started, by the first WiFi.mode() call in setup().
// Before that it returns all zeros.
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

// Milliseconds since boot as a 64-bit value. millis() is only 32 bits
// and wraps after about 49.7 days, which would make the uptime on the
// status page start again from zero on a node left running that long.
uint64_t uptimeMs()
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

String formatDuration(uint64_t ms)
{
    uint64_t totalSeconds = ms / 1000;

    unsigned long days = (unsigned long)(totalSeconds / 86400);
    unsigned long hours = (unsigned long)((totalSeconds % 86400) / 3600);
    unsigned long minutes = (unsigned long)((totalSeconds % 3600) / 60);
    unsigned long seconds = (unsigned long)(totalSeconds % 60);

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

// Mirrors the client-side pattern='[ -~]*' constraint on the SSID and
// web credential fields. A byte length check alone isn't sufficient:
// the browser's maxlength counts characters, while our server-side
// checks count bytes, and those only agree if every character is a
// single-byte one - i.e. printable ASCII. (WiFi passwords are the
// exception: they may be UTF-8, and both the browser script and
// isValidWiFiPassword() count them in bytes.)
bool isPrintableAscii(const String &input)
{
    for (size_t i = 0; i < input.length(); i++)
    {
        uint8_t c = (uint8_t)input[i];

        if (c < 0x20 || c > 0x7E)
        {
            return false;
        }
    }

    return true;
}

// Node name rules: 1 to 17 bytes, letters, digits and hyphens only,
// with no hyphen at the start or end. The name doubles as the DHCP
// hostname, so it has to be a valid hostname label.
bool isValidNodeName(const String &name)
{
    size_t length = name.length();

    if (length < 1 || length > 17)
    {
        return false;
    }

    for (size_t i = 0; i < length; i++)
    {
        char c = name[i];

        bool isLetterOrDigit =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9');

        if (!isLetterOrDigit && c != '-')
        {
            return false;
        }

        if (c == '-' && (i == 0 || i == length - 1))
        {
            return false;
        }
    }

    return true;
}

// WiFi password rules: empty (an open network), or 8 to 63 bytes. The
// limits are in bytes, not characters, because that is how WPA/WPA2 and
// the ESP32 count them. Any UTF-8 text is allowed (a non-ASCII character
// takes 2-4 bytes); control characters are not.
bool isValidWiFiPassword(const String &password)
{
    size_t length = password.length();

    if (length == 0)
    {
        return true;
    }

    if (length < WIFI_PASSWORD_MIN_BYTES || length > WIFI_PASSWORD_MAX_BYTES)
    {
        return false;
    }

    for (size_t i = 0; i < length; i++)
    {
        uint8_t c = (uint8_t)password[i];

        if (c < 0x20 || c == 0x7F)
        {
            return false;
        }
    }

    return true;
}

// Parses a whole-number form value. String::toInt() is too forgiving:
// it returns 0 for text and stops at the first non-digit, and browsers
// let a type='number' field submit things like "5e1", so "abc" would
// quietly become 0 and "5e1" would become 5. This accepts only one or
// more digits (up to five, enough for the largest valid value, 32767).
bool parseDigits(const String &text, long &value)
{
    size_t length = text.length();

    if (length < 1 || length > 5)
    {
        return false;
    }

    for (size_t i = 0; i < length; i++)
    {
        if (text[i] < '0' || text[i] > '9')
        {
            return false;
        }
    }

    value = text.toInt();

    return true;
}

// ============================================================
// LOAD CONFIGURATION
// ============================================================

void loadConfiguration()
{
    preferences.begin("relaynode", false);

    apSSID = preferences.getString("ap_ssid", DEFAULT_NAME);

    webUser = preferences.getString("webuser", "admin");
    webPass = preferences.getString("webpass", "admin");

    for (int i = 0; i < MAX_WIFI_NETWORKS; i++)
    {
        String ssidKey = "ssid" + String(i);
        String passKey = "pass" + String(i);

        wifiSSID[i] = preferences.getString(ssidKey.c_str(), "");
        wifiPassword[i] = preferences.getString(passKey.c_str(), "");
    }

    artnetNodeName = preferences.getString("nodename", DEFAULT_NAME);

    artnetUniverse = preferences.getUShort("universe", DEFAULT_UNIVERSE);

    relay1DMXAddress = preferences.getUShort("dmx1", 1);
    relay2DMXAddress = preferences.getUShort("dmx2", 2);

    // Safety validation.

    if (artnetUniverse > 32767)
    {
        artnetUniverse = DEFAULT_UNIVERSE;
    }

    if (relay1DMXAddress < 1 || relay1DMXAddress > 512)
    {
        relay1DMXAddress = 1;
    }

    if (relay2DMXAddress < 1 || relay2DMXAddress > 512)
    {
        relay2DMXAddress = 2;
    }

    // The node name is used for both the Art-Net ShortName and
    // LongName fields and as the DHCP hostname, so it must be valid
    // (see isValidNodeName()). Fall back to the default if the stored
    // value is not.
    if (!isValidNodeName(artnetNodeName))
    {
        artnetNodeName = DEFAULT_NAME;
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
    DBG_PRINTLN(relay1DMXAddress);

    DBG_PRINT("[CONFIG] Relay 2 DMX address: ");
    DBG_PRINTLN(relay2DMXAddress);

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

    preferences.putUShort("dmx1", relay1DMXAddress);
    preferences.putUShort("dmx2", relay2DMXAddress);

#if DEBUG
    DBG_PRINTLN("[CONFIG] Configuration saved.");
#endif
}

// ============================================================
// STATUS LED
// ============================================================

// Highest priority first:
//   1. Connecting to WiFi              -> fast flash
//   2. DMX in configured universe      -> solid
//   3. Fallback access point mode      -> occasional flash
//   4. Otherwise                       -> slow flash
void updateStatusLed()
{
    unsigned long now = millis();
    bool on;

    if (wifiConnecting || wifiRecoveryActive)
    {
        on = (now % LED_FAST_PERIOD_MS) < LED_FAST_ON_MS;
    }
    else if (everReceivedArtDmx && (uptimeMs() - lastArtDmxMs) < LED_DMX_ACTIVE_MS)
    {
        on = true;
    }
    else if (accessPointMode)
    {
        on = (now % LED_AP_PERIOD_MS) < LED_AP_ON_MS;
    }
    else
    {
        on = (now % LED_SLOW_PERIOD_MS) < LED_SLOW_ON_MS;
    }

    digitalWrite(STATUS_LED, on ? LED_ON_LEVEL : LED_OFF_LEVEL);
}

// ============================================================
// WIFI CONNECTION
// ============================================================

// Registered with WiFi.onEvent() in setup(). Records why the station
// disconnected, so tryWiFiNetwork() can tell a network that refused our
// credentials from one that is absent or simply slow.
//
// The status that WiFi.status() reports is not enough for this: the
// library only maps some failures to WL_CONNECT_FAILED, and a wrong
// WPA/WPA2 password often shows up as a 4-way handshake timeout, which
// it reports as an ordinary disconnect while it keeps retrying until
// our timeout expires. The disconnect reason is reliable.
//
// Runs in the WiFi event task, so it only sets flags: nothing here may
// block or touch other state.
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info)
{
    if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    {
        return;
    }

    uint8_t reason = info.wifi_sta_disconnected.reason;

    wifiLastDisconnectReason = reason;

    switch (reason)
    {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            wifiCredentialsRefused = true;
            break;

        case WIFI_REASON_NO_AP_FOUND:
            wifiNetworkNotFound = true;
            break;

        default:
            break;
    }
}

bool tryWiFiNetwork(int index)
{
    // Reset first, so that a call that returns early without trying
    // anything can't leave the previous attempt's outcome behind.
    lastJoinAuthFailed = false;
    lastJoinNotFound = false;

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

    wifiConnecting = true;
    updateStatusLed();

    WiFi.mode(WIFI_STA);

    // Disconnect from the previous network without erasing
    // the saved station configuration.
    WiFi.disconnect(false, false);

    delay(100);

    // Cleared here, after the disconnect above has had time to deliver
    // its own event, so only this attempt's outcome is seen.
    wifiCredentialsRefused = false;
    wifiNetworkNotFound = false;
    wifiLastDisconnectReason = 0;

    bool sawNotFound = false;

    WiFi.begin(
        wifiSSID[index].c_str(),
        wifiPassword[index].c_str()
    );

    unsigned long startTime = millis();

    while (millis() - startTime < CONNECT_TIMEOUT_MS)
    {
        wl_status_t status = WiFi.status();

        if (status == WL_CONNECTED)
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
            wifiConnecting = false;

            // Keep WiFi power-save disabled once connected, so periodic
            // broadcast traffic (e.g. an Art-Net controller's ArtPoll)
            // isn't at risk of being missed around DTIM beacons. This
            // setting isn't reliably persistent across a WiFi.mode()/
            // WiFi.begin() cycle, so it's re-applied here on every
            // successful connect (boot, WiFi recovery, and AP-mode
            // recovery) rather than once in setup().
            WiFi.setSleep(false);

            // Leaving the fallback access point does not stop Art-Net
            // (the UDP socket stays bound), so replies queued for
            // controllers on the access point are dropped here.
            clearPendingArtPollReplies();

            startArtNet();

            return true;
        }

        // The network refused our credentials (e.g. a wrong password),
        // either as reported by onWiFiEvent() or as WL_CONNECT_FAILED.
        // Retrying with the same password cannot succeed, so give up on
        // this network now instead of waiting out the timeout. (The
        // library would otherwise keep retrying in the background.)
        if (wifiCredentialsRefused || status == WL_CONNECT_FAILED)
        {
            lastJoinAuthFailed = true;
            break;
        }

        // No network with that SSID answered. This does not end the
        // attempt: it may only be a router that is still starting up, so
        // the library keeps looking until the timeout.
        if (wifiNetworkNotFound || status == WL_NO_SSID_AVAIL)
        {
            sawNotFound = true;
        }

        // Short poll interval so the fast LED flash stays accurate.
        updateStatusLed();
        delay(20);
    }

    wifiConnecting = false;

    // A refusal means the network is there, so it outranks "not found"
    // from an earlier retry in the same attempt.
    lastJoinNotFound = sawNotFound && !lastJoinAuthFailed;

#if DEBUG
    DBG_PRINT("[WIFI] Failed to connect to: ");
    DBG_PRINT(wifiSSID[index]);
    DBG_PRINT(" (after ");
    DBG_PRINT(millis() - startTime);
    DBG_PRINTLN(" ms)");

    DBG_PRINT("[WIFI] Last disconnect reason code: ");
    DBG_PRINTLN(wifiLastDisconnectReason);

    if (lastJoinAuthFailed)
    {
        DBG_PRINTLN("[WIFI] The network refused the credentials (wrong password?).");
    }
    else if (lastJoinNotFound)
    {
        DBG_PRINTLN("[WIFI] The network was not found.");
    }
#endif

    WiFi.disconnect(false, false);

    return false;
}

// ============================================================
// HOLD-OFF AFTER REFUSED CREDENTIALS
// ============================================================
//
// A network that refuses our credentials is there, and will keep
// refusing them until they are corrected. Trying it again after the
// usual 30 seconds would only take the fallback access point down again,
// possibly while someone is connected to it to correct them. So when a
// series of attempts has failed, the caller asks shouldHoldOffAfter()
// whether to wait AP_AUTH_FAIL_HOLD_OFF_MS instead.

// Adds the attempt that has just failed, as described by
// lastJoinAuthFailed and lastJoinNotFound, to the tally.
void recordJoinFailure(JoinFailures &failures)
{
    if (lastJoinAuthFailed)
    {
        failures.refused++;
    }
    else if (!lastJoinNotFound)
    {
        failures.other++;
    }
}

// Hold off if at least one network refused our credentials, and every
// other attempt failed only because that network was not there at all.
// A network that is absent says nothing either way - it cannot refuse
// anything, and it is no reason to keep retrying one that did. A network
// that failed in some other way (e.g. a timeout) might be in range but
// unreachable, and might recover on its own, so then the normal
// interval applies.
bool shouldHoldOffAfter(const JoinFailures &failures)
{
    return failures.refused > 0 && failures.other == 0;
}

// ============================================================
// CONNECT TO SAVED NETWORKS
// ============================================================

void connectToSavedNetworks()
{
    accessPointMode = false;

    JoinFailures failures = { 0, 0 };

    for (int i = 0; i < MAX_WIFI_NETWORKS; i++)
    {
        if (wifiSSID[i].length() == 0)
        {
            continue;
        }

        if (tryWiFiNetwork(i))
        {
            return;
        }

        recordJoinFailure(failures);
    }

    startAccessPoint();

    // startAccessPoint() schedules the first recovery scan for
    // AP_SCAN_INTERVAL_MS from now. If a network refused our
    // credentials, hold that off straight away, rather than only after
    // the first recovery attempt has dropped the access point.
    if (shouldHoldOffAfter(failures))
    {
        apScanDelayMs = AP_AUTH_FAIL_HOLD_OFF_MS;

#if DEBUG
        DBG_PRINTLN("[WIFI] A network refused the credentials. Holding off scanning.");
#endif
    }
}

// ============================================================
// ACCESS POINT
// ============================================================

void startAccessPoint()
{
    stopArtNet();

    accessPointMode = true;

    // Cancel/discard any scan left over from a previous AP session,
    // and reset the recovery timer so the next scan waits a full
    // interval rather than firing immediately.
    if (apScanInProgress)
    {
        WiFi.scanDelete();
        apScanInProgress = false;
    }
    lastApScanMs = millis();
    apScanDelayMs = AP_SCAN_INTERVAL_MS;

    // WIFI_AP_STA rather than plain WIFI_AP: this keeps the station
    // interface initialised (even though it isn't associated to
    // anything) alongside the fallback AP. Without it,
    // WiFi.macAddress() returns all zeros while running as an
    // access point, because its underlying call requires the STA
    // interface to have been started.
    WiFi.mode(WIFI_AP_STA);

    // Force the AP's IP configuration explicitly rather than relying
    // on the library default, so it's guaranteed and documented
    // rather than incidental.
    bool configResult = WiFi.softAPConfig(AP_LOCAL_IP, AP_GATEWAY_IP, AP_SUBNET_MASK);

    bool result = WiFi.softAP(apSSID.c_str());

#if DEBUG
    DBG_PRINT("[WIFI] Starting access point: ");
    DBG_PRINTLN(apSSID);

    DBG_PRINT("[WIFI] AP config result: ");
    DBG_PRINTLN(configResult ? "SUCCESS" : "FAILED");

    DBG_PRINT("[WIFI] AP start result: ");
    DBG_PRINTLN(result ? "SUCCESS" : "FAILED");

    DBG_PRINT("[WIFI] AP IP: ");
    DBG_PRINTLN(WiFi.softAPIP());
#endif

    if (result)
    {
        startArtNet();
    }
}

// ============================================================
// ACCESS POINT RECOVERY (SCAN FOR A CONFIGURED NETWORK)
// ============================================================

// While in fallback access point mode, periodically scans for
// configured networks coming back into range - e.g. after a brief
// power cut where this controller boots faster than the router - and
// switches out of access point mode if one of them can be joined.
//
// startAccessPoint() runs WIFI_AP_STA, so the station interface
// remains available for scanning while the fallback AP continues
// running. The scan runs in the background and never blocks the main
// loop, but it shares the ESP32's WiFi radio with the AP, so there may
// be brief interruptions to AP traffic while it is in progress.
//
// Joining a network is different: tryWiFiNetwork() switches to
// WIFI_STA, which drops the AP, and blocks (for up to
// CONNECT_TIMEOUT_MS) until the attempt succeeds or fails. Every
// configured network found by the scan is tried in turn, and if none
// can be joined the AP is restarted.
//
// If a network refused our credentials (wrong password), the next scan
// is held off for AP_AUTH_FAIL_HOLD_OFF_MS instead of
// AP_SCAN_INTERVAL_MS (see shouldHoldOffAfter()). Each attempt takes the
// AP down, so without this, someone connected to the AP to correct the
// credentials would be knocked off on every scan interval. It is only a
// delay: recovery carries on afterwards. The same hold-off applies from
// the start if the attempts at startup are what led to the AP.
void handleAccessPointRecovery()
{
    if (!accessPointMode)
    {
        return;
    }

    if (apScanInProgress)
    {
        int16_t result = WiFi.scanComplete();

        if (result == WIFI_SCAN_RUNNING)
        {
            if (millis() - apScanStartMs < AP_SCAN_MAX_AGE_MS)
            {
                return;
            }

#if DEBUG
            DBG_PRINTLN("[WIFI] AP-mode scan stuck (no result after max wait). Resetting.");
#endif

            WiFi.scanDelete();
            apScanInProgress = false;
            lastApScanMs = millis();

            return;
        }

        apScanInProgress = false;

        // Which configured networks does the scan show to be in range?
        bool inRange[MAX_WIFI_NETWORKS] = { false };

        if (result > 0)
        {
#if DEBUG
            DBG_PRINT("[WIFI] AP-mode scan found ");
            DBG_PRINT(result);
            DBG_PRINTLN(" network(s).");
#endif

            for (int i = 0; i < result; i++)
            {
                String foundSSID = WiFi.SSID(i);

                for (int n = 0; n < MAX_WIFI_NETWORKS; n++)
                {
                    if (wifiSSID[n].length() > 0 && wifiSSID[n] == foundSSID)
                    {
                        inRange[n] = true;
                    }
                }
            }
        }
#if DEBUG
        else if (result == 0)
        {
            DBG_PRINTLN("[WIFI] AP-mode scan found no networks.");
        }
        else
        {
            DBG_PRINTLN("[WIFI] AP-mode scan failed.");
        }
#endif

        WiFi.scanDelete();

        // Try every configured network that is in range, in order.
        int attempted = 0;
        JoinFailures failures = { 0, 0 };

        for (int n = 0; n < MAX_WIFI_NETWORKS; n++)
        {
            if (!inRange[n])
            {
                continue;
            }

#if DEBUG
            DBG_PRINT("[WIFI] Configured network back in range: ");
            DBG_PRINTLN(wifiSSID[n]);
            DBG_PRINTLN("[WIFI] Attempting to leave access point mode.");
#endif

            attempted++;

            if (tryWiFiNetwork(n))
            {
#if DEBUG
                DBG_PRINTLN("[WIFI] Switched from access point to station mode.");
#endif
                return;
            }

            recordJoinFailure(failures);
        }

        if (attempted > 0)
        {
#if DEBUG
            DBG_PRINTLN("[WIFI] Connection attempt(s) failed. Resuming access point.");
#endif

            // tryWiFiNetwork() switches to WIFI_STA on its way in,
            // which drops the fallback AP - restart it since none of
            // the connection attempts succeeded after all.
            startAccessPoint();

            if (shouldHoldOffAfter(failures))
            {
                // A network refused our credentials. Leave the access
                // point alone for a while so they can be fixed.
                apScanDelayMs = AP_AUTH_FAIL_HOLD_OFF_MS;

#if DEBUG
                DBG_PRINTLN("[WIFI] A network refused the credentials. Holding off scanning.");
#endif
            }

            return;
        }

        lastApScanMs = millis();

        return;
    }

    if (millis() - lastApScanMs < apScanDelayMs)
    {
        return;
    }

    bool anyConfigured = false;

    for (int i = 0; i < MAX_WIFI_NETWORKS; i++)
    {
        if (wifiSSID[i].length() > 0)
        {
            anyConfigured = true;
            break;
        }
    }

    if (!anyConfigured)
    {
        lastApScanMs = millis();

        return;
    }

    // From here on the normal interval applies again (it may have been
    // lengthened by a hold-off after refused credentials).
    apScanDelayMs = AP_SCAN_INTERVAL_MS;

#if DEBUG
    DBG_PRINTLN("[WIFI] Scanning for configured networks (access point mode)...");
#endif

    int16_t scanStartResult = WiFi.scanNetworks(true, false);

    if (scanStartResult == WIFI_SCAN_RUNNING)
    {
        apScanInProgress = true;
        apScanStartMs = millis();
    }
    else
    {
#if DEBUG
        DBG_PRINTLN("[WIFI] AP-mode scan failed to start.");
#endif
        // Leave apScanInProgress false and just retry after the next
        // interval, rather than polling scanComplete() on a scan that
        // never began.
        lastApScanMs = millis();
    }
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
    nextRecoveryCycleMs = millis();
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

    // Don't start a cycle before the interval since the previous one
    // started has passed (see WIFI_RECOVERY_CYCLE_INTERVAL_MS). The rest
    // of loop() carries on meanwhile.
    if ((long)(millis() - nextRecoveryCycleMs) < 0)
    {
        return;
    }

    // Set before the attempts, so that the interval runs from the start
    // of this cycle and a cycle that took longer than that is followed
    // straight away by the next.
    nextRecoveryCycleMs = millis() + WIFI_RECOVERY_CYCLE_INTERVAL_MS;

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

    // Art-Net runs either on a joined station connection or on the
    // fallback access point. In AP mode the station interface is never
    // associated, so WiFi.status() cannot be used as the test there.
    if (!accessPointMode && WiFi.status() != WL_CONNECTED)
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
        ARTNET_PRINTLN(relay1DMXAddress);

        ARTNET_PRINT("[ARTNET] Relay 2 DMX address: ");
        ARTNET_PRINTLN(relay2DMXAddress);
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

    clearPendingArtPollReplies();

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
    IPAddress ip = accessPointMode ? WiFi.softAPIP() : WiFi.localIP();

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
    // Bits 5-0 = DMX512 (000000).
    //
    // This is one logical DMX output port (NumPorts above = 1).
    //

    packet[174] = 0x80;

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
        ARTNET_PRINTLN(relay1DMXAddress);

        ARTNET_PRINT("[ARTNET] Relay 2 DMX address: ");
        ARTNET_PRINTLN(relay2DMXAddress);
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

// Discards every queued ArtPollReply. Called when Art-Net stops, and
// when a network has just been joined, because a reply is addressed to
// a controller on the network it was queued on, and would otherwise
// still be sent once Art-Net runs again (to a controller that may not
// be there any more).
void clearPendingArtPollReplies()
{
    for (int i = 0; i < MAX_PENDING_ARTPOLL_REPLIES; i++)
    {
        pendingArtPollReplies[i].active = false;
    }
}

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
// RELAY OUTPUTS
// ============================================================
//
// DMX data does not switch the relays directly. handleArtNet() records
// the state each relay has most recently been asked to be in, and
// applyRelays() (called on every pass of loop()) switches the output
// once RELAY_DWELL_MS has passed since that relay last changed. A
// change requested inside the dwell time is therefore held back, not
// lost: the relay ends up in the last requested state without having
// to wait for the controller to send the frame again.

struct RelayState
{
    int pin;
    bool wanted;
    uint8_t lastDmxValue;
    unsigned long lastChangeMs;
};

RelayState relays[2] = {
    { RELAY_1, false, 0, 0 },
    { RELAY_2, false, 0, 0 }
};

// Records the state the DMX data wants for relay "index" (0 or 1).
void requestRelayState(int index, uint8_t dmxValue)
{
    relays[index].wanted = dmxValue > 0;
    relays[index].lastDmxValue = dmxValue;

#if DEBUG_ARTNET && DEBUG_VERBOSE
    if (relays[index].wanted != (digitalRead(relays[index].pin) == HIGH) &&
            millis() - relays[index].lastChangeMs < RELAY_DWELL_MS)
    {
        ARTNET_PRINT("[ARTNET] Relay ");
        ARTNET_PRINT(index + 1);
        ARTNET_PRINTLN(" change deferred (dwell time).");
    }
#endif
}

// Switches each relay to the state last requested by DMX data, provided
// the dwell time since its previous change has elapsed.
void applyRelays()
{
    for (int i = 0; i < 2; i++)
    {
        bool isOn = digitalRead(relays[i].pin) == HIGH;

        if (relays[i].wanted == isOn)
        {
            continue;
        }

        if (millis() - relays[i].lastChangeMs < RELAY_DWELL_MS)
        {
            continue;
        }

        digitalWrite(relays[i].pin, relays[i].wanted ? HIGH : LOW);
        relays[i].lastChangeMs = millis();

#if DEBUG_ARTNET
        ARTNET_PRINT("[ARTNET] Relay ");
        ARTNET_PRINT(i + 1);
        ARTNET_PRINT(" state changed: ");
        ARTNET_PRINT(relays[i].wanted ? "OFF -> ON" : "ON -> OFF");
        ARTNET_PRINT(" (DMX ");
        ARTNET_PRINT(i == 0 ? relay1DMXAddress : relay2DMXAddress);
        ARTNET_PRINT(" = ");
        ARTNET_PRINT(relays[i].lastDmxValue);
        ARTNET_PRINTLN(")");
#endif
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
        // A structurally valid ArtPoll is at least 14 bytes:
        // ID[8] + OpCode[2] + ProtVerHi/Lo[2] + TalkToMe[1] + Priority[1].
        // Anything shorter is malformed - drop it, and don't count it
        // as a received ArtPoll for the status page.
        if (packetSize < 14 || bytesRead < 14)
        {
#if DEBUG_ARTNET && DEBUG_VERBOSE
            ARTNET_PRINTLN("[ARTNET] ArtPoll too short. Ignoring.");
#endif
            return;
        }

        uint16_t protocolVersion = readUInt16BE(packet + 10);
        uint8_t flags = packet[12];

        // TalkToMe bit 5 (value 0x20) is Targeted Mode: the poll only
        // wants a reply from nodes whose Port-Address falls within
        // [TargetPortAddressBottom, TargetPortAddressTop]. Those two
        // extra 16-bit fields only exist when this bit is set.
        bool targetedMode = (flags & 0x20) != 0;

        if (targetedMode)
        {
            // Targeted Mode is an Art-Net 4 feature and requires the
            // packet to actually carry the extra range fields (18 bytes
            // total). A poll claiming Targeted Mode from an older
            // protocol version, or one too short to hold the range, is
            // internally inconsistent - treat it as malformed.
            if (protocolVersion < 14 || packetSize < 18 || bytesRead < 18)
            {
#if DEBUG_ARTNET && DEBUG_VERBOSE
                ARTNET_PRINTLN("[ARTNET] Malformed targeted ArtPoll. Ignoring.");
#endif
                return;
            }

            uint16_t targetTop = readUInt16BE(packet + 14);
            uint16_t targetBottom = readUInt16BE(packet + 16);

            uint16_t rangeLow = targetTop < targetBottom ? targetTop : targetBottom;
            uint16_t rangeHigh = targetTop < targetBottom ? targetBottom : targetTop;

            if (artnetUniverse < rangeLow || artnetUniverse > rangeHigh)
            {
#if DEBUG_ARTNET && DEBUG_VERBOSE
                ARTNET_PRINT("[ARTNET] Targeted ArtPoll range ");
                ARTNET_PRINT(rangeLow);
                ARTNET_PRINT("-");
                ARTNET_PRINT(rangeHigh);
                ARTNET_PRINTLN(" does not include our universe. Not replying.");
#endif
                // Not addressed to us - a valid poll, just not one we
                // should count or reply to.
                return;
            }
        }

        // Either untargeted (addressed to every node) or targeted at a
        // range that includes our universe - this is the point where
        // it counts as "an ArtPoll for us" for the status page.
        lastArtPollMs = uptimeMs();
        everReceivedArtPoll = true;

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

    // Recognised as carrying DMX data - the "Last DMX data" tracking
    // (below, after we've confirmed the universe matches) only counts
    // it once we know it's actually addressed to us.

    // ArtDMX minimum packet length is 18 bytes.

    if (packetSize < 18 || bytesRead < 18)
    {
#if DEBUG_ARTNET && DEBUG_VERBOSE
        ARTNET_PRINTLN("[ARTNET] ArtDMX packet too short. Ignoring.");
#endif
        return;
    }

    uint16_t protocolVersion = readUInt16BE(packet + 10);

    // Sequence is used by controllers/nodes that need to detect and
    // discard out-of-order or duplicate packets (e.g. when merging
    // ArtDMX from multiple sources). We don't merge, interpolate, or
    // otherwise rely on packet ordering: each received frame is
    // treated independently and the two relay channels simply take
    // whichever value arrived most recently, so there's nothing here
    // that "out of order" could corrupt. It's read (for the verbose
    // debug log below) but intentionally never checked.
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

    if (dmxLength < 1 || dmxLength > 512)
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

    // Addressed to our universe - this is "DMX data for us" for the
    // status page's "Last DMX data" reading.
    lastArtDmxMs = uptimeMs();
    everReceivedArtDmx = true;

    // ----------------------------------------------------------
    // Relays
    // ----------------------------------------------------------
    //
    // Only record what the DMX data is asking for. applyRelays() does
    // the switching, so that RELAY_DWELL_MS is respected without
    // losing a change that arrives too soon after the previous one.
    // A relay whose channel is not in this packet is left alone.

    if (relay1DMXAddress >= 1 &&
            relay1DMXAddress <= 512 &&
            relay1DMXAddress <= dmxLength)
    {
        requestRelayState(0, packet[18 + relay1DMXAddress - 1]);
    }

    if (relay2DMXAddress >= 1 &&
            relay2DMXAddress <= 512 &&
            relay2DMXAddress <= dmxLength)
    {
        requestRelayState(1, packet[18 + relay2DMXAddress - 1]);
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
    text-wrap: balance;
}

.status__value {
    flex-shrink: 0;
    max-width: 65%;
    font-weight: 500;
    text-align: right;
    overflow-wrap: anywhere;
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

.btn--in-panel {
    margin-bottom: 12px;
}

.panel__note {
    font-size: 0.85rem;
    color: var(--text-muted);
    margin: 0 0 12px;
}

.progress {
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 6px;
    height: 12px;
    margin: 0 0 12px;
    overflow: hidden;
}

.progress__bar {
    background: var(--accent);
    height: 100%;
    width: 0;
}
)CSS";

// Browser-side half of the firmware update (see handleFirmwareUpdate()).
// Only included in the page when updates are available, after
// REBOOT_SCRIPT (it uses waitForReboot()). Written for older browsers:
// no arrow functions, let/const, promises or fetch.
const char UPDATE_SCRIPT[] PROGMEM = R"JS(
function setUpdateStatus(message) {
    document.getElementById('updateStatus').textContent = message;
}

function failUpdate(message) {
    document.getElementById('updateButton').disabled = false;
    document.getElementById('updateProgress').hidden = true;
    setUpdateStatus(message);
}

function sendFirmware(file) {
    var button = document.getElementById('updateButton');
    var progress = document.getElementById('updateProgress');
    var bar = document.getElementById('updateBar');
    var formData = new FormData();
    var xhr = new XMLHttpRequest();

    formData.append('firmware', file, file.name);

    button.disabled = true;
    bar.style.width = '0%';
    progress.hidden = false;
    setUpdateStatus('Uploading firmware. Do not close this page or power off the node.');

    xhr.upload.onprogress = function (event) {
        if (event.lengthComputable) {
            bar.style.width = Math.round(event.loaded * 100 / event.total) + '%';
        }
    };

    xhr.onload = function () {
        if (xhr.status === 200) {
            bar.style.width = '100%';
            setUpdateStatus('Firmware verified. The node is rebooting; waiting for it to come back online...');
            window.setTimeout(waitForReboot, 3000);
        } else {
            failUpdate((xhr.responseText || 'The update failed.') + ' The current firmware has not been changed.');
        }
    };

    xhr.onerror = function () {
        failUpdate('The connection was lost during the upload. Reload this page to check the firmware version.');
    };

    xhr.ontimeout = xhr.onerror;
    xhr.timeout = 180000;

    xhr.open('POST', '/update');
    xhr.send(formData);
}

function startUpdate() {
    var file = document.getElementById('firmwarefile').files[0];
    var reader = new FileReader();

    if (!file) {
        setUpdateStatus('Choose a firmware (.bin) file first.');
        return;
    }

    if (!/\.bin$/i.test(file.name)) {
        setUpdateStatus('The firmware file must be a .bin file.');
        return;
    }

    // Check the start of the file before uploading it: an ESP32
    // application image begins with 0xE9 and has the app descriptor
    // magic word (0xABCD5432) at offset 32. The node repeats this check.
    reader.onload = function () {
        var bytes = new Uint8Array(reader.result);

        if (bytes.length < 36 || bytes[0] !== 0xE9 ||
                bytes[32] !== 0x32 || bytes[33] !== 0x54 ||
                bytes[34] !== 0xCD || bytes[35] !== 0xAB) {
            setUpdateStatus('This is not an ESP32 application firmware image. Use the plain .bin file, not the one that includes the bootloader.');
            return;
        }

        if (confirm('Upload this firmware and reboot the node? Both relays will switch off while it restarts.')) {
            sendFirmware(file);
        }
    };

    reader.readAsArrayBuffer(file.slice(0, 36));
}
)JS";

// Polls /ping until the node answers again after a reboot, then loads
// the status page. Shared by the firmware update (UPDATE_SCRIPT) and the
// Save & Reboot / Discard & Reboot page. Same older-browser rules as
// UPDATE_SCRIPT.
const char REBOOT_SCRIPT[] PROGMEM = R"JS(
function waitForReboot() {
    var xhr = new XMLHttpRequest();

    xhr.open('GET', '/ping?' + new Date().getTime());
    xhr.timeout = 2000;
    xhr.onload = function () {
        if (xhr.status === 200) {
            window.location.href = '/';
        } else {
            window.setTimeout(waitForReboot, 1000);
        }
    };
    xhr.onerror = function () {
        window.setTimeout(waitForReboot, 1000);
    };
    xhr.ontimeout = xhr.onerror;
    xhr.send();
}
)JS";

// Browser-side helpers for the configuration form. Same older-browser
// rules as UPDATE_SCRIPT.
//
// Stored passwords are never sent to the browser: their fields hold a
// placeholder, and a hidden "<id>_edited" field (0 or 1) tells the
// server whether the user has typed a new password. The show/hide
// checkbox stays disabled while the placeholder is showing, since there
// is nothing real to reveal.
const char PAGE_SCRIPT[] PROGMEM = R"JS(
function togglePassword(id, show) {
    var element = document.getElementById(id);

    element.type = show ? 'text' : 'password';
}

// Asks the user to confirm saving, but only once everything in the form
// is valid. The click handler runs before the browser checks the form,
// so asking unconditionally would show the confirmation every time the
// user clicks Save, even while there are still entries to correct. If
// anything is invalid, nothing is asked: returning true lets the click
// carry on, and the browser then reports the invalid entry instead.
function confirmSave(form) {
    if (!form.checkValidity()) {
        return true;
    }

    return confirm('Save the new configuration and reboot the node now?');
}

// Length of a string once encoded as UTF-8, in bytes. WiFi passwords
// are limited in bytes rather than characters, and a non-ASCII
// character takes 2 to 4 bytes.
function utf8Length(value) {
    var length = 0;
    var code;
    var i;

    for (i = 0; i < value.length; i += 1) {
        code = value.charCodeAt(i);

        if (code < 0x80) {
            length += 1;
        } else if (code < 0x800) {
            length += 2;
        } else if (code >= 0xD800 && code <= 0xDBFF) {
            // First half of a surrogate pair: one 4-byte character.
            length += 4;
            i += 1;
        } else {
            length += 3;
        }
    }

    return length;
}

// While the placeholder is showing, select it all so that typing or
// deleting replaces it instead of being added to it.
function passwordFocus(input) {
    var edited = document.getElementById(input.id + '_edited');

    if (edited.value === '0' && input.value !== '') {
        input.setSelectionRange(0, input.value.length);
    }
}

function passwordInput(input, minBytes, maxBytes, allowEmpty) {
    var length = utf8Length(input.value);
    var message = '';

    document.getElementById(input.id + '_edited').value = '1';
    document.getElementById(input.id + '_show').disabled = false;

    if (length === 0 && !allowEmpty) {
        message = 'A password is required.';
    } else if (length > 0 && length < minBytes) {
        message = 'The password must be at least ' + minBytes + ' bytes.';
    } else if (length > maxBytes) {
        message = 'The password must be no more than ' + maxBytes + ' bytes.';
    }

    input.setCustomValidity(message);
}

// Browsers let a number field hold things like "5e1". Only plain digits
// are accepted.
function checkInteger(input) {
    var valid = /^[0-9]+$/.test(input.value);

    input.setCustomValidity(valid ? '' : 'Enter a whole number using digits only.');
}
)JS";


#if DEBUG_LOG_ENABLED

// Style for the Debug Log panel. Debug builds only.
const char DEBUG_LOG_STYLE[] PROGMEM = R"CSS(
.log {
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 6px;
    color: var(--text);
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 0.75rem;
    height: 320px;
    margin: 0 0 12px;
    overflow: auto;
    padding: 10px 12px;
    white-space: pre-wrap;
    word-break: break-all;
}

.log__auto {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: 0.82rem;
    color: var(--text-muted);
}
)CSS";

// Browser-side half of the Debug Log panel (see handleLog()). Each
// refresh asks the controller only for the entries added since the last
// one, and appends them to those already in the panel, so earlier
// entries stay for as long as the page is open. Optionally refreshes
// every 2 seconds. Debug builds only. Same older-browser rules as
// UPDATE_SCRIPT.
const char DEBUG_LOG_SCRIPT[] PROGMEM = R"JS(
var debugLogTimer = null;
var debugLogLoading = false;
var debugLogNext = 0;
var debugLogSession = '';
var debugLogAtLineStart = true;

function loadDebugLog() {
    var box = document.getElementById('debugLog');
    var status = document.getElementById('debugLogStatus');
    var url = '/log?from=' + debugLogNext;
    var xhr;

    // Don't pile up requests if the node is slow to answer.
    if (debugLogLoading) {
        return;
    }

    debugLogLoading = true;

    // Nothing to say about the session until the first reply has told us.
    if (debugLogSession !== '') {
        url += '&session=' + debugLogSession;
    }

    xhr = new XMLHttpRequest();
    xhr.open('GET', url + '&t=' + new Date().getTime());
    xhr.timeout = 5000;
    xhr.onload = function () {
        // Only follow the newest output if the view was already at the
        // bottom, so scrolling back to read something isn't undone.
        var atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 20;
        var next = xhr.getResponseHeader('X-Log-Next');
        var session = xhr.getResponseHeader('X-Log-Session');
        var gap = xhr.getResponseHeader('X-Log-Gap') === '1';
        var atLineStart = debugLogAtLineStart;
        var addition = '';

        debugLogLoading = false;

        if (xhr.status !== 200 || next === null || session === null) {
            status.textContent = 'Could not load the log (HTTP ' + xhr.status + ').';
            return;
        }

        status.textContent = '';

        // The first reply replaces the "Loading" placeholder.
        if (debugLogSession === '') {
            box.textContent = '';
        }

        // A different session means the controller has restarted since
        // the last refresh: what follows is from the new run. Mark it,
        // with a blank line before.
        if (debugLogSession !== '' && session !== debugLogSession) {
            if (!atLineStart) {
                addition += '\n';
            }

            addition += '\n*****  Rebooted *****\n';
            atLineStart = true;
        }

        // If what came back does not continue straight on from what is
        // already shown, say so on a line of its own between the two.
        // (This is also how a log that had already been cut short is
        // shown when the page is first loaded.)
        if (gap) {
            if (!atLineStart) {
                addition += '\n';
            }

            addition += '[older entries discarded]\n';
        }

        addition += xhr.responseText;

        if (addition !== '') {
            box.appendChild(document.createTextNode(addition));
            debugLogAtLineStart = addition.charAt(addition.length - 1) === '\n';
        }

        debugLogNext = next;
        debugLogSession = session;

        if (atBottom) {
            box.scrollTop = box.scrollHeight;
        }
    };
    xhr.onerror = function () {
        debugLogLoading = false;
        status.textContent = 'Could not load the log.';
    };
    xhr.ontimeout = xhr.onerror;
    xhr.send();
}

function toggleDebugLogAuto(on) {
    if (debugLogTimer !== null) {
        window.clearInterval(debugLogTimer);
        debugLogTimer = null;
    }

    if (on) {
        loadDebugLog();
        debugLogTimer = window.setInterval(loadDebugLog, 2000);
    }
}

// After a reload the browser may restore the Auto-refresh tick (it
// remembers form state) without anything starting the timer, so make the
// timer match the checkbox. Done again once the page has finished
// loading, because some browsers only restore the state at that point.
function syncDebugLogAuto() {
    toggleDebugLogAuto(document.getElementById('debugLogAuto').checked);
}

loadDebugLog();
syncDebugLogAuto();
window.addEventListener('load', syncDebugLogAuto);
)JS";

#endif

// Memory reserved up front for the generated status page. This is
// only a pre-allocation hint (the String grows if the page turns out
// larger), but staying within it avoids repeated reallocation. With
// DEBUG and DEBUG_VERBOSE enabled, the actual page size and free heap
// are logged on each load so the headroom can be checked.
const size_t STATUS_PAGE_RESERVE_BYTES = 24576;

void handleRoot()
{
    logWebRequest();

    if (!server.authenticate(webUser.c_str(), webPass.c_str()))
    {
        return server.requestAuthentication();
    }

    String html;
    html.reserve(STATUS_PAGE_RESERVE_BYTES);

    html += "<!DOCTYPE html>";
    html += "<html>";
    html += "<head>";
    html += "<meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<meta name='color-scheme' content='dark'>";
    html += "<title>";
    html += htmlEscape(artnetNodeName);
    html += "</title>";
    html += "<style>";
    html += FPSTR(PAGE_STYLE);
#if DEBUG_LOG_ENABLED
    html += FPSTR(DEBUG_LOG_STYLE);
#endif
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

    html += "<div class='status__row'><span class='status__label'>Firmware version</span><span class='status__value'>";
    html += FIRMWARE_VERSION;
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
    html += formatDuration(uptimeMs());
    html += "</span></div>";

    html += "<div class='status__row'><span class='status__label'>Last reset reason</span><span class='status__value'>";
    html += htmlEscape(bootResetReasonText);
    html += "</span></div>";

    html += "<div class='status__row'><span class='status__label'>Last DMX data (configured universe)</span><span class='status__value'>";
    if (everReceivedArtDmx)
    {
        html += formatDuration(uptimeMs() - lastArtDmxMs) + " ago";
    }
    else
    {
        html += "Never";
    }
    html += "</span></div>";

    html += "<div class='status__row'><span class='status__label'>Last ArtPoll (targeting node)</span><span class='status__value'>";
    if (everReceivedArtPoll)
    {
        html += formatDuration(uptimeMs() - lastArtPollMs) + " ago";
    }
    else
    {
        html += "Never";
    }
    html += "</span></div>";

    html += "<div class='status__row'><span class='status__label'>Relay 1</span><span class='status__value'>";
    html += (digitalRead(RELAY_1) == HIGH) ? "ON" : "OFF";
    html += "</span></div>";

    html += "<div class='status__row'><span class='status__label'>Relay 2</span><span class='status__value'>";
    html += (digitalRead(RELAY_2) == HIGH) ? "ON" : "OFF";
    html += "</span></div>";

    html += "</div>";

    html += "<form method='POST' action='/save'>";

    // ----------------------------------------------------------
    // Art-Net
    // ----------------------------------------------------------

    html += "<div class='panel'>";
    html += "<div class='panel__title'>Art-Net</div>";

    html += "<div class='field'>";
    html += "<label class='field__label' for='nodename'>Node Name (1-17 letters, digits or hyphens)</label>";
    html += "<input class='field__input' type='text' id='nodename' name='nodename' required minlength='1' maxlength='17' pattern='[A-Za-z0-9]([A-Za-z0-9\\-]*[A-Za-z0-9])?' title='Letters, digits and hyphens only; must not start or end with a hyphen' value='";
    html += htmlEscape(artnetNodeName);
    html += "'>";
    html += "</div>";

    html += "<div class='field'>";
    html += "<label class='field__label' for='universe'>Art-Net Universe / Port-Address (0-32767)</label>";
    html += "<input class='field__input' type='number' id='universe' name='universe' required min='0' max='32767' oninput='checkInteger(this)' value='";
    html += String(artnetUniverse);
    html += "'>";
    html += "</div>";

    html += "<div class='field'>";
    html += "<label class='field__label' for='dmx1'>Relay 1 DMX Address (1-512)</label>";
    html += "<input class='field__input' type='number' id='dmx1' name='dmx1' required min='1' max='512' oninput='checkInteger(this)' value='";
    html += String(relay1DMXAddress);
    html += "'>";
    html += "</div>";

    html += "<div class='field'>";
    html += "<label class='field__label' for='dmx2'>Relay 2 DMX Address (1-512)</label>";
    html += "<input class='field__input' type='number' id='dmx2' name='dmx2' required min='1' max='512' oninput='checkInteger(this)' value='";
    html += String(relay2DMXAddress);
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
        html += "<input class='field__input' type='text' id='" + ssidId + "' name='" + ssidId + "' maxlength='32' pattern='[ -~]*' title='ASCII characters only' value='";
        html += htmlEscape(wifiSSID[i]);
        html += "'>";
        html += "</div>";

        // The stored password is never sent to the browser. If there is
        // one, the field shows a placeholder and the show/hide control
        // is disabled until the user types a new password; an open
        // network (empty password) is shown as an empty field.
        bool hasPassword = wifiPassword[i].length() > 0;

        html += "<div class='field'>";
        html += "<label class='field__label' for='" + passId + "'>Password (8-63 bytes, or empty for an open network)</label>";
        html += "<div class='password-field'>";
        html += "<input class='field__input password-field__input' type='password' id='" + passId + "' name='" + passId + "' maxlength='63' autocomplete='new-password' value='";
        html += hasPassword ? PASSWORD_PLACEHOLDER : "";
        html += "' onfocus='passwordFocus(this)' onclick='passwordFocus(this)' oninput='passwordInput(this, 8, 63, true)'>";
        html += "<input type='hidden' id='" + passId + "_edited' name='" + passId + "_edited' value='0'>";
        html += "<label class='password-field__toggle'><input type='checkbox' id='" + passId + "_show'" + (hasPassword ? " disabled" : "") + " onchange=\"togglePassword('" + passId + "', this.checked)\"> Show</label>";
        html += "</div>";
        html += "</div>";
    }

    html += "<div class='field'>";
    html += "<label class='field__label' for='apssid'>Access Point SSID</label>";
    html += "<input class='field__input' type='text' id='apssid' name='apssid' required minlength='1' maxlength='32' pattern='[ -~]*' title='ASCII characters only' value='";
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
    html += "<input class='field__input' type='text' id='webuser' name='webuser' required minlength='1' maxlength='32' pattern='[ -~]*' title='ASCII characters only' value='";
    html += htmlEscape(webUser);
    html += "'>";
    html += "</div>";

    html += "<div class='field'>";
    html += "<label class='field__label' for='webpass'>Password</label>";
    html += "<div class='password-field'>";
    html += "<input class='field__input password-field__input' type='password' id='webpass' name='webpass' required minlength='1' maxlength='32' pattern='[ -~]*' title='ASCII characters only' autocomplete='new-password' value='";
    html += PASSWORD_PLACEHOLDER;
    html += "' onfocus='passwordFocus(this)' onclick='passwordFocus(this)' oninput='passwordInput(this, 1, 32, false)'>";
    html += "<input type='hidden' id='webpass_edited' name='webpass_edited' value='0'>";
    html += "<label class='password-field__toggle'><input type='checkbox' id='webpass_show' disabled onchange=\"togglePassword('webpass', this.checked)\"> Show</label>";
    html += "</div>";
    html += "</div>";

    html += "</div>";

    html += "<button class='btn' type='submit' onclick=\"return confirmSave(this.form);\">Save &amp; Reboot</button>";
    html += "<button class='btn btn--secondary' type='submit' formaction='/discard' formnovalidate onclick=\"return confirm('Discard any changes and reboot the node now? Unsaved changes will be lost.');\">Discard &amp; Reboot</button>";

    html += "</form>";

    // ----------------------------------------------------------
    // Firmware update
    // ----------------------------------------------------------

    bool firmwareUpdateAllowed = isFirmwareUpdateAllowed();

    html += "<div class='panel'>";
    html += "<div class='panel__title'>Firmware Update</div>";

    if (firmwareUpdateAllowed)
    {
        html += "<div class='field'>";
        html += "<label class='field__label' for='firmwarefile'>Firmware file (.bin)</label>";
        html += "<input class='field__input' type='file' id='firmwarefile' accept='.bin'>";
        html += "</div>";

        html += "<div class='progress' id='updateProgress' hidden><div class='progress__bar' id='updateBar'></div></div>";

        html += "<p class='panel__note' id='updateStatus' role='status'>Use the plain application .bin (Arduino IDE: Sketch &gt; Export Compiled Binary), not the &quot;with bootloader&quot; file. The node reboots after a successful update, and both relays switch off until DMX data is received again.</p>";

        html += "<button class='btn btn--in-panel' type='button' id='updateButton' onclick='startUpdate()'>Upload &amp; Reboot</button>";
    }
    else
    {
        html += "<p class='panel__note'>Firmware updates are only available when the node is connected to a password-protected WiFi network. They are disabled in fallback access point mode and on open networks.</p>";
    }

    html += "</div>";

#if DEBUG_LOG_ENABLED
    // ----------------------------------------------------------
    // Debug log (debug builds only)
    // ----------------------------------------------------------

    html += "<div class='panel'>";
    html += "<div class='panel__title'>Debug Log</div>";
    html += "<p class='panel__note'>The debug output, newest at the bottom, with the time since boot in seconds. Refreshing adds the new entries to those already shown. The controller only keeps its most recent output, so entries may be missing where the log says so.</p>";
    html += "<pre class='log' id='debugLog'>Loading&hellip;</pre>";
    html += "<p class='panel__note' id='debugLogStatus' role='status'></p>";
    html += "<button class='btn btn--in-panel' type='button' onclick='loadDebugLog()'>Refresh</button>";
    html += "<label class='log__auto'><input type='checkbox' id='debugLogAuto' onchange='toggleDebugLogAuto(this.checked)'> Auto-refresh</label>";
    html += "</div>";
#endif

    html += "</div>";

    html += "<script>";
    html += FPSTR(PAGE_SCRIPT);
    html += "</script>";

#if DEBUG_LOG_ENABLED
    html += "<script>";
    html += FPSTR(DEBUG_LOG_SCRIPT);
    html += "</script>";
#endif

    if (firmwareUpdateAllowed)
    {
        html += "<script>";
        html += FPSTR(REBOOT_SCRIPT);
        html += FPSTR(UPDATE_SCRIPT);
        html += "</script>";
    }

    html += "</body>";
    html += "</html>";

#if DEBUG && DEBUG_VERBOSE
    DBG_PRINT("[WEB] Status page: ");
    DBG_PRINT(html.length());
    DBG_PRINT(" bytes (reserved ");
    DBG_PRINT(STATUS_PAGE_RESERVE_BYTES);
    DBG_PRINT("), free heap ");
    DBG_PRINT(ESP.getFreeHeap());
    DBG_PRINT(" bytes, largest free block ");
    DBG_PRINT(ESP.getMaxAllocHeap());
    DBG_PRINTLN(" bytes");
#endif

    // Tell the browser to close this connection rather than keep it
    // open for reuse. The WebServer library only tracks one client
    // connection at a time, so a browser holding a connection open
    // (or opening several, e.g. alongside a /favicon.ico request)
    // can otherwise make an unrelated subsequent request queue up
    // behind it until the library's own close-wait period elapses.
    server.sendHeader("Connection", "close");
    server.send(200, "text/html; charset=utf-8", html);
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
    html += "<meta charset='utf-8'>";
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
    // resubmit this POST) or landing on a stale page. It is the same
    // waitForReboot() that the firmware update uses.
    html += "<script>";
    html += FPSTR(REBOOT_SCRIPT);
    html += "window.setTimeout(waitForReboot, 2000);";
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

    // WiFi networks. An empty SSID marks the slot unused; an SSID is up
    // to 32 bytes of printable ASCII. A password is only changed if the
    // page reports that the user edited it (passN_edited): until then
    // the page only ever showed a placeholder, so the stored password
    // is left as it is. A new password must be empty (an open network)
    // or 8-63 bytes, and may be any UTF-8 text without control
    // characters. Anything that doesn't comply is rejected outright,
    // leaving the previously stored value in place. A slot left with an
    // empty SSID has no use for a password, so its password is cleared,
    // whether it was an old one or one submitted along with the empty
    // SSID.

    for (int i = 0; i < MAX_WIFI_NETWORKS; i++)
    {
        String ssidName = "ssid" + String(i);
        String passName = "pass" + String(i);

        if (server.hasArg(ssidName))
        {
            String newSSID = server.arg(ssidName);

            if (newSSID.length() <= 32 && isPrintableAscii(newSSID))
            {
                wifiSSID[i] = newSSID;
            }
        }

        if (server.arg(passName + "_edited") == "1" && server.hasArg(passName))
        {
            String newPassword = server.arg(passName);

            if (isValidWiFiPassword(newPassword))
            {
                wifiPassword[i] = newPassword;
            }
        }

        if (wifiSSID[i].length() == 0)
        {
            wifiPassword[i] = "";
        }
    }

    // AP SSID: required, 1-32 bytes, printable ASCII.

    if (server.hasArg("apssid"))
    {
        String newAPSSID = server.arg("apssid");

        if (newAPSSID.length() >= 1 && newAPSSID.length() <= 32 &&
                isPrintableAscii(newAPSSID))
        {
            apSSID = newAPSSID;
        }
    }

    // Web credentials: required, 1-32 bytes each, printable ASCII (the
    // encoding of non-ASCII characters in HTTP Basic authentication is
    // not consistent between browsers). The password is only changed if
    // the page reports that the user edited it (webpass_edited).

    if (server.hasArg("webuser"))
    {
        String newWebUser = server.arg("webuser");

        if (newWebUser.length() >= 1 && newWebUser.length() <= 32 &&
                isPrintableAscii(newWebUser))
        {
            webUser = newWebUser;
        }
    }

    if (server.arg("webpass_edited") == "1" && server.hasArg("webpass"))
    {
        String newWebPass = server.arg("webpass");

        if (newWebPass.length() >= 1 && newWebPass.length() <= 32 &&
                isPrintableAscii(newWebPass))
        {
            webPass = newWebPass;
        }
    }

    // Node name: see isValidNodeName(). Rejected outright rather than
    // corrected, so a bad submission never silently mutates into
    // something the user didn't type.

    if (server.hasArg("nodename"))
    {
        String newNodeName = server.arg("nodename");

        if (isValidNodeName(newNodeName))
        {
            artnetNodeName = newNodeName;
        }
    }

    // Art-Net Universe. Digits only (see parseDigits()).

    if (server.hasArg("universe"))
    {
        long value = 0;

        if (parseDigits(server.arg("universe"), value) && value <= 32767)
        {
            artnetUniverse = (uint16_t)value;
        }
    }

    // Relay 1 DMX address.

    if (server.hasArg("dmx1"))
    {
        long value = 0;

        if (parseDigits(server.arg("dmx1"), value) && value >= 1 && value <= 512)
        {
            relay1DMXAddress = (uint16_t)value;
        }
    }

    // Relay 2 DMX address.

    if (server.hasArg("dmx2"))
    {
        long value = 0;

        if (parseDigits(server.arg("dmx2"), value) && value >= 1 && value <= 512)
        {
            relay2DMXAddress = (uint16_t)value;
        }
    }

    saveConfiguration();

    preferences.putString("resetreason", "Configuration change");

#if DEBUG
    DBG_PRINTLN("[CONFIG] Web configuration updated.");
    DBG_PRINTLN("[CONFIG] Rebooting to apply new configuration.");
#endif

    String html = buildRebootPage(
        "Configuration Saved",
        "The new settings have been saved. The node is rebooting to apply them."
    );

    server.sendHeader("Connection", "close");
    server.send(200, "text/html; charset=utf-8", html);

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

    server.sendHeader("Connection", "close");
    server.send(200, "text/html; charset=utf-8", html);

    delay(500);

    ESP.restart();
}

// ============================================================
// WEB FIRMWARE UPDATE
// ============================================================
//
// Over-the-air firmware update from the web interface. The browser
// POSTs the compiled application binary (multipart/form-data) to
// /update. handleFirmwareUpload() is called repeatedly by the
// WebServer library as chunks arrive and streams them straight into
// the inactive OTA flash partition, so the running firmware is never
// touched while receiving. handleFirmwareUpdate() is called once the
// upload has finished and reports the result.
//
// Safeguards against a wrong, truncated or corrupt upload, none of
// which need any extra step from the user:
//   - The browser checks the file's header before uploading it, and
//     the node checks it again as the first bytes arrive (see
//     isValidFirmwareHeader()). This rejects files that are not an
//     ESP32 application image, such as the "with bootloader" image.
//   - Nothing is activated until Update.end(true) succeeds. That call
//     verifies the whole image (its checksum and hash) before it
//     switches the boot partition, so a truncated or damaged upload
//     is rejected and the current firmware stays in place.
//   - Any error, or the connection dropping mid-upload, aborts the
//     update and the node carries on running as before.
//
// Updates are only accepted in station mode on a password-protected
// WiFi network (see isFirmwareUpdateAllowed()).

// Progress of the firmware upload currently being received. Shared
// between handleFirmwareUpload() (called as each chunk arrives) and
// handleFirmwareUpdate() (called once at the end of the request).
bool firmwareUpdateStarted = false;
bool firmwareUpdateComplete = false;
String firmwareUpdateError = "";
uint8_t firmwareHeader[FIRMWARE_HEADER_BYTES];
size_t firmwareHeaderLength = 0;

// Firmware updates are only offered when the node is running as a
// station on a WiFi network that has a password configured. The web
// interface may still be using its default credentials, so the
// protection of a secured network is what keeps strangers from
// flashing the node. They are therefore never available in fallback
// access point mode (an open network), or when connected to an open
// network.
bool isFirmwareUpdateAllowed()
{
    return !accessPointMode &&
        !wifiRecoveryActive &&
        WiFi.status() == WL_CONNECTED &&
        currentNetwork >= 0 &&
        currentNetwork < MAX_WIFI_NETWORKS &&
        wifiPassword[currentNetwork].length() > 0;
}

// An ESP32 application image starts with the magic byte 0xE9, and
// has the app descriptor's magic word (0xABCD5432, little-endian) at
// offset 32, straight after the image and first segment headers.
// Bootloader and "merged" images also start with 0xE9, but have no
// app descriptor at that offset, so they are rejected here.
bool isValidFirmwareHeader(const uint8_t *header)
{
    return header[0] == 0xE9 &&
        header[32] == 0x32 &&
        header[33] == 0x54 &&
        header[34] == 0xCD &&
        header[35] == 0xAB;
}

// Records why the update failed and discards whatever has been
// written so far. The running firmware is unaffected.
void failFirmwareUpdate(const String &reason)
{
    firmwareUpdateError = reason;

    if (firmwareUpdateStarted)
    {
        Update.abort();
        firmwareUpdateStarted = false;
    }

#if DEBUG
    DBG_PRINT("[UPDATE] Failed: ");
    DBG_PRINTLN(reason);
#endif
}

void handleFirmwareUpload()
{
    HTTPUpload &upload = server.upload();

    if (upload.status == UPLOAD_FILE_START)
    {
        firmwareUpdateStarted = false;
        firmwareUpdateComplete = false;
        firmwareUpdateError = "";
        firmwareHeaderLength = 0;

        // Rejected uploads are still read to the end by the WebServer
        // library, but nothing is written. handleFirmwareUpdate()
        // sends the actual 401/403 response.
        if (!server.authenticate(webUser.c_str(), webPass.c_str()))
        {
            firmwareUpdateError = "Authentication required.";
            return;
        }

        if (!isFirmwareUpdateAllowed())
        {
            firmwareUpdateError = "Firmware updates are only available when connected to a password-protected WiFi network.";
            return;
        }

#if DEBUG
        DBG_PRINT("[UPDATE] Receiving firmware file: ");
        DBG_PRINTLN(upload.filename);
#endif

        // The size is not known up front (the request length also
        // includes the multipart framing), so the image is sized by
        // what actually arrives. Update.end(true) below is what
        // confirms it is complete.
        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
        {
            failFirmwareUpdate(String("Could not start the update: ") + Update.errorString());
            return;
        }

        firmwareUpdateStarted = true;
    }
    else if (upload.status == UPLOAD_FILE_WRITE)
    {
        if (!firmwareUpdateStarted)
        {
            return;
        }

        // The first chunk could in theory be shorter than the header
        // check needs, so collect the leading bytes across chunks.
        if (firmwareHeaderLength < FIRMWARE_HEADER_BYTES)
        {
            size_t needed = FIRMWARE_HEADER_BYTES - firmwareHeaderLength;
            size_t count = (upload.currentSize < needed) ? upload.currentSize : needed;

            memcpy(firmwareHeader + firmwareHeaderLength, upload.buf, count);
            firmwareHeaderLength += count;

            if (firmwareHeaderLength == FIRMWARE_HEADER_BYTES &&
                    !isValidFirmwareHeader(firmwareHeader))
            {
                failFirmwareUpdate("The file is not an ESP32 application firmware image. Use the plain .bin file, not the one that includes the bootloader.");
                return;
            }
        }

        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
        {
            failFirmwareUpdate(String("Writing to flash failed: ") + Update.errorString());
            return;
        }

        // The WebServer library is busy receiving the upload, so keep
        // the rest of the node running in the meantime: the status LED
        // and Art-Net (so the relays keep following DMX data) are
        // serviced between chunks.
        updateStatusLed();
        handleArtNet();
        handlePendingArtPollReplies();
        applyRelays();
    }
    else if (upload.status == UPLOAD_FILE_END)
    {
        if (!firmwareUpdateStarted)
        {
            return;
        }

        if (firmwareHeaderLength < FIRMWARE_HEADER_BYTES)
        {
            failFirmwareUpdate("The file is too small to be a firmware image.");
            return;
        }

        // Verifies the complete image and, only if it is good,
        // makes it the partition to boot from.
        if (!Update.end(true))
        {
            failFirmwareUpdate(String("The firmware image failed verification: ") + Update.errorString());
            return;
        }

        firmwareUpdateStarted = false;
        firmwareUpdateComplete = true;

#if DEBUG
        DBG_PRINT("[UPDATE] Firmware received and verified (");
        DBG_PRINT(upload.totalSize);
        DBG_PRINTLN(" bytes).");
#endif
    }
    else if (upload.status == UPLOAD_FILE_ABORTED)
    {
        failFirmwareUpdate("The upload was interrupted.");
    }
}

// Called once the upload request has been fully received. The browser
// page expects "OK" with status 200 on success, or a plain-text
// explanation with an error status.
void handleFirmwareUpdate()
{
    logWebRequest();

    if (!server.authenticate(webUser.c_str(), webPass.c_str()))
    {
        return server.requestAuthentication();
    }

    bool complete = firmwareUpdateComplete;
    String error = firmwareUpdateError;

    firmwareUpdateComplete = false;
    firmwareUpdateError = "";

    if (!complete)
    {
        int statusCode = 400;

        if (!isFirmwareUpdateAllowed())
        {
            statusCode = 403;
            error = "Firmware updates are only available when connected to a password-protected WiFi network.";
        }
        else if (error.length() == 0)
        {
            error = "No firmware file was received.";
        }

        server.sendHeader("Connection", "close");
        server.send(statusCode, "text/plain", error);
        return;
    }

    preferences.putString("resetreason", "Firmware update");

#if DEBUG
    DBG_PRINTLN("[UPDATE] Firmware update complete. Rebooting.");
#endif

    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", "OK");

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

    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", "OK");
}

#if DEBUG_LOG_ENABLED

// ============================================================
// DEBUG LOG
// ============================================================
//
// Serves the in-memory debug log as plain text, for the Debug Log panel
// on the status page. Unlike the other pages it does not call
// logWebRequest(): the panel can refresh itself every couple of
// seconds, and logging its own requests would fill the log with them.
//
// The panel asks for only what it has not seen yet:
//
//   /log?from=N&session=S
//
// where N and S are the X-Log-Next and X-Log-Session values it was given
// by the previous reply. (The first request has no session yet, so it
// sends only from=0.) The reply is the log text from position N, with
// these headers:
//
//   X-Log-Next     the position to ask for next time
//   X-Log-Session  identifies this run of the controller
//   X-Log-Gap      1 if the text does not continue straight on from N
//                  because those entries have been discarded since.
//                  Otherwise 0.
//
// If S is not this run's session, the controller has restarted since
// the panel last asked, so N means nothing: the reply is this run's log
// from the beginning, and the panel sees that X-Log-Session has changed.
//
// Opening /log directly (no "from") returns the whole log, starting with
// a note if entries have been discarded.

void handleLog()
{
    if (!server.authenticate(webUser.c_str(), webPass.c_str()))
    {
        return server.requestAuthentication();
    }

    bool fromPanel = server.hasArg("from");
    uint64_t from = 0;

    // A position only means something to the run it came from. With a
    // different session (or none yet), start from the beginning.
    if (server.hasArg("session") &&
            (uint32_t)strtoul(server.arg("session").c_str(), nullptr, 10) == debugLog.sessionId())
    {
        from = (uint64_t)strtoull(server.arg("from").c_str(), nullptr, 10);
    }

    uint64_t next = 0;
    bool gap = false;
    String body = debugLog.readFrom(from, next, gap);

    if (!fromPanel && gap)
    {
        body = String("[older entries discarded]\n") + body;
    }

    char number[24];

    snprintf(number, sizeof(number), "%llu", (unsigned long long)next);
    server.sendHeader("X-Log-Next", number);

    snprintf(number, sizeof(number), "%lu", (unsigned long)debugLog.sessionId());
    server.sendHeader("X-Log-Session", number);

    server.sendHeader("X-Log-Gap", gap ? "1" : "0");
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain; charset=utf-8", body);
}

#endif

// ============================================================
// NOT FOUND
// ============================================================
//
// Replaces the WebServer library's own default not-found handler,
// so unmatched requests (most commonly a browser's automatic
// /favicon.ico request) also close the connection immediately
// rather than potentially lingering.

void handleNotFound()
{
    logWebRequest();

    server.sendHeader("Connection", "close");
    server.send(404, "text/plain", "Not found");
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(115200);

    // ----------------------------------------------------------
    // Relay outputs
    // ----------------------------------------------------------
    //
    // These come first, before anything that waits, so that the relays
    // are driven off as early as possible after a reset.

    pinMode(RELAY_1, OUTPUT);
    pinMode(RELAY_2, OUTPUT);

    // Relays are active HIGH.
    // Always start with both OFF.

    digitalWrite(RELAY_1, LOW);
    digitalWrite(RELAY_2, LOW);

    // Status LED starts off; updateStatusLed() drives it from here on.
    pinMode(STATUS_LED, OUTPUT);
    digitalWrite(STATUS_LED, LED_OFF_LEVEL);

#if DEBUG
    // Give the serial monitor a moment to connect, so that the startup
    // messages are not lost. Debug builds only, and after the outputs
    // above have been set up, so it never delays them.
    delay(500);

    DBG_PRINTLN();
    DBG_PRINTLN("========================================");
    DBG_PRINTLN("ESP32 Relay Art-Net Node");
    DBG_PRINT("Firmware version: ");
    DBG_PRINTLN(FIRMWARE_VERSION);
    DBG_PRINTLN("========================================");

    DBG_PRINT("[GPIO] Relay 1 GPIO");
    DBG_PRINT(RELAY_1);
    DBG_PRINTLN(" = OFF");

    DBG_PRINT("[GPIO] Relay 2 GPIO");
    DBG_PRINT(RELAY_2);
    DBG_PRINTLN(" = OFF");

    DBG_PRINT("[GPIO] Status LED GPIO");
    DBG_PRINT(STATUS_LED);
    DBG_PRINTLN(" = OFF");
#endif

    // ----------------------------------------------------------
    // Configuration
    // ----------------------------------------------------------

    loadConfiguration();

    // Preferences are open at this point (loadConfiguration() calls
    // preferences.begin()), so this can safely read/clear the
    // one-shot reset-reason flag written by handleSave()/handleDiscard().
    bootResetReasonText = determineResetReason();

#if DEBUG
    DBG_PRINT("[STATUS] Last reset reason: ");
    DBG_PRINTLN(bootResetReasonText);
#endif

    // ----------------------------------------------------------
    // WiFi
    // ----------------------------------------------------------

    // Lets tryWiFiNetwork() see why a connection attempt failed.
    WiFi.onEvent(onWiFiEvent);

    // The DHCP hostname must be set before WiFi is started.
    WiFi.setHostname(artnetNodeName.c_str());
    WiFi.softAPsetHostname(artnetNodeName.c_str());

#if DEBUG
    DBG_PRINT("[WIFI] Hostname: ");
    DBG_PRINTLN(WiFi.getHostname());
#endif

    WiFi.mode(WIFI_STA);

#if DEBUG
    DBG_PRINTLN("[WIFI] Attempting saved networks...");
#endif

    connectToSavedNetworks();

    // ----------------------------------------------------------
    // Web server
    // ----------------------------------------------------------

    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/discard", HTTP_POST, handleDiscard);
    server.on("/update", HTTP_POST, handleFirmwareUpdate, handleFirmwareUpload);
    server.on("/ping", HTTP_GET, handlePing);

#if DEBUG_LOG_ENABLED
    server.on("/log", HTTP_GET, handleLog);
#endif

    server.onNotFound(handleNotFound);

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
    updateStatusLed();

    server.handleClient();

    // Art-Net remains active while connected.
    if (!wifiRecoveryActive)
    {
        handleArtNet();
        handlePendingArtPollReplies();
    }

    // Switch the relays to the state most recently requested by DMX
    // data, as soon as the dwell time allows.
    applyRelays();

    // Monitor the established WiFi connection.
    handleWiFiMonitor();

    // While in fallback access point mode, periodically scan for a
    // configured network having come back into range.
    handleAccessPointRecovery();

    // If the connection has been lost, cycle through the saved
    // networks until one succeeds.
    if (wifiRecoveryActive)
    {
        handleWiFiRecovery();
    }
}