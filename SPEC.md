# ESP32 Art-Net Relay Controller

## 1. Overview

Firmware for an ESP32-based two-relay controller.

The software receives Art-Net 4 / DMX data over WiFi and uses two DMX channels to control two onboard relays.

The controller is intended for simple, reliable control of two on/off lighting outputs. The implementation should remain lightweight and avoid unnecessary functionality.

### Hardware

#### Board

- ESP32-based 2-relay board
- eBay listing: https://www.ebay.co.uk/itm/277016777105

#### Features

- ESP32-32E module on board, 4M Byte flash
- ESP32 module I/O port and UART program download port all lead out, convenient for secondary development
- On-board AC-DC switching power supply module, supporting AC 90-250V
- On-board RST reset button and IO0 programmable button
- 2 on-board 5V relay, the output switch signal, suitable for controlling the control voltage of AC 250V/DC30V loads
- 1 programmable LED and 2 relay indicators on board

#### Relay outputs

- Relay 1: GPIO17 (inner-side relay)
- Relay 2: GPIO16 (outer-side relay)

#### Status LED

- On-board programmable LED: GPIO23

### Communications

- Art-Net 4 over WiFi
- DMX data carried within Art-Net
- Web browser configuration interface
- Firmware update via the web interface
- Serial debugging via UART

---

## 2. Relay Control

The two relays are controlled directly from ESP32 GPIOs.

| Relay | ESP32 GPIO | DMX channel |
|---|---:|---:|
| Relay 1 | GPIO17 | Configurable |
| Relay 2 | GPIO16 | Configurable |

The DMX channel assigned to each relay is configurable through the web interface.

### DMX-to-relay behaviour

Each relay is controlled by the value received on its configured DMX channel.

The exact threshold behaviour should be defined as:

- DMX value `0` = GPIO LOW = relay OFF
- DMX value `1-255` = GPIO HIGH = relay ON

This provides straightforward compatibility with lighting-control software and avoids requiring a specific dimmer level.

There should be a minimum relay dwell time / anti-chatter guard of 500ms to prevent excessive wear to the relays from a rapidly changing control signal: after a relay changes state, it shall not change state again until at least 500ms have passed.

A change requested during the dwell time is held back, not discarded. The controller keeps the state most recently requested by DMX data for each relay and applies it as soon as the dwell time has elapsed, without waiting for the sender to transmit the value again. If the requested state changes back before the dwell time has elapsed, the relay is not switched at all.

---

## 3. Art-Net

The controller shall receive Art-Net 4 / DMX packets over WiFi.

The Art-Net serial should be derived from the hardware MAC address and remain constant between reboots.

### Art-Net configuration

The following Art-Net parameters are configurable via the web interface:

- Node name (1 to 17 characters: letters, digits and hyphens only; used for both short and long name, the DHCP hostname, and the web interface title tag)
- Universe
- Relay 1 DMX channel
- Relay 2 DMX channel

### Packet handling

The firmware should:

1. Respond to ArtPoll broadcasts.
2. Listen for Art-Net DMX packets.
3. Identify packets belonging to the configured universe.
4. Extract the configured DMX channels.
5. Update the corresponding relay outputs.
6. Ignore packets for other universes.

ArtPollReply messages should have a random response delay (0 to 1000ms) to avoid flooding the network with a large amount of replies simultaneously. This should not block the main loop.

Art-Net packet processing should not block network, web interface, or WiFi-management functions.

---

## 4. WiFi

The controller must support connection to secure WiFi networks, and provide an open access point as a fallback.

Up to three WiFi network configurations can be stored.

Each configuration consists of:

- SSID
- Password

Open networks can be used by configuring an empty password.

Note that a hidden SSID will work at boot, but will not be found in a recovery scan.

The hostname used when obtaining an IP address via DHCP shall match the configured Art-Net node name.

### Boot-time connection behaviour

On boot:

1. Attempt to connect to configured WiFi network 1.
2. If unsuccessful, attempt network 2.
3. Continue through all configured networks until a successful connection.
4. If none are available, start the fallback open access point.

The connection process should have sensible connection timeouts so that an unavailable network does not prevent the controller from progressing to the next configured network. An attempt shall also end as soon as the network refuses the stored credentials (for example, a wrong password), rather than waiting for the timeout, because retrying with the same credentials cannot succeed.

### Subsequent connection failure

If the controller successfully connects to a configured WiFi network and the connection subsequently fails:

1. Do not start the fallback access point.
2. Continually attempt to reconnect.
3. Cycle through the configured WiFi networks in turn.
4. Continue attempting until a configured network becomes available. Each connection cycle shall occur no more frequently than every 5 seconds (to avoid hammering the router if connection attempts are failing quickly).

The fallback access point is a boot-time fallback only.

Art-Net processing (ArtPoll replies and ArtDMX reception) is active in fallback access point mode in the same way as in station mode, so a controller connected directly to the access point can control the relays.

### Fallback access point

The fallback access point has no password and provides DHCP. The device will have a fixed IP of 192.168.4.1 on subnet 192.168.4.0/24.

#### Recovery while in fallback access point mode

A brief power cut can cause both the WiFi router and this controller to reboot together, with the controller starting up faster and finding no configured network available, so it starts the fallback access point. Without a recovery mechanism it would then stay in access point mode indefinitely, even once the router comes back.

While running as the fallback access point, the controller shall periodically (every 30 seconds) scan for WiFi networks:

1. If one or more configured networks are found, attempt to connect to each of them in turn, in the order they are configured, until one succeeds.
2. If a connection succeeds, stop the fallback access point and resume normal station operation.
3. If every connection attempt fails, or no configured network is found, resume the fallback access point and continue scanning periodically.

The scan runs in the background and must not block Art-Net processing or the web interface. Because the WiFi radio is shared with the access point, it may briefly interrupt traffic on the access point while the scan is in progress.

Connecting to a network found by the scan is different: it stops the access point, and blocks Art-Net processing and the web interface for up to the connection timeout for each network attempted. The access point is only restarted once the attempts have finished and none succeeded.

**Hold-off after refused credentials.** If a network refused the stored credentials (an authentication failure, such as a wrong password), the controller shall wait 3 minutes before the next scan instead of 30 seconds. This applies after a recovery attempt, and also from the moment the fallback access point is started after the connection attempts at startup. Each connection attempt takes the access point down, so without this, anyone connected to the access point to correct the WiFi credentials in the web interface would be disconnected over and over again; the 3 minutes gives them time to do so.

Networks that were not found at all are ignored when deciding this. If any network that was tried failed in some other way (for example, by not answering within the timeout), the hold-off does not apply, because that network may be in range but unreachable and might recover on its own.

This is a delay, not a lock-out. After 3 minutes recovery resumes as normal, so a router that only refused the password temporarily while it was starting up is still picked up.

### Web interface

The web interface should report the current WiFi mode, IP address and SSID or fallback access point mode.

---

## 5. Web Interface

The controller shall provide a browser-based configuration and status interface.

The interface should be usable from a desktop computer, tablet, or phone.

### Status information

The main interface should display:

- MAC address
- Art-Net serial
- Firmware version
- WiFi mode
- IP address
- Gateway IP address
- Connected network SSID or indicate fallback access point mode
- WiFi signal strength (RSSI)
- Uptime (time since last reset/boot)
- Reason for last reset/boot (power-on, configuration change, firmware update, watchdog timeout, brownout, panic, etc.)
- Time since last DMX data received for the configured universe (snapshot when web page loaded)
- Time since last ArtPoll targeting the node (snapshot when web page loaded)
- State of relay outputs (snapshot when web page loaded)

### Configuration

The following settings shall be editable and stored persistently across power cycles and ESP32 resets.

#### Art-Net

- Node name (used for both short and long name) [default="ESP32-Relay"]
- Art-Net Universe [default=1]
- Relay 1 DMX channel [default=1]
- Relay 2 DMX channel [default=2]

#### WiFi

- Fallback access point SSID [default="ESP32-Relay"]
- WiFi network 1
    - SSID
    - Password
- WiFi network 2
    - SSID
    - Password
- WiFi network 3
    - SSID
    - Password

Each WiFi password field shall have a control to show or hide the password characters.

#### Web interface authentication

- Username [default="admin"]
- Password [default="admin"]

#### Stored passwords

The web interface never sends a stored password to the browser, neither the WiFi network passwords nor the web interface password. A password field that has a stored password shows ten placeholder characters instead, and its show/hide control is disabled because there is nothing to reveal.

Saving the configuration without editing a password field leaves that stored password unchanged. As soon as the user types in the field, the placeholder is replaced by what they type, the show/hide control is enabled, and the new password is stored when the configuration is saved. A WiFi network that has no password (an open network) is shown with an empty field; to remove a stored WiFi password, clear its field.

### Actions

The configuration interface shall provide a **Save & Reboot** button and a **Discard & Reboot** button.

The **Save & Reboot** button shall:

1. Validate the entered configuration in the browser. If anything is invalid, report it and stop.
2. Confirm the Save & Reboot action.
3. Validate the submitted configuration on the server.
4. Store the configuration in non-volatile memory.
5. Reboot the ESP32.
6. Apply the new configuration during startup.

Configuration should not be changed permanently until the user selects Save & Reboot.

The **Discard & Reboot** button shall:

1. Confirm the Discard & Reboot action
2. Discard any changes to the form, keeping the current configuration.
3. Reboot the ESP32.

### Validation

Configured values should be validated on the client-side, and prevent submission if invalid. They should also be validated on the server-side and prevent store/update of the individual value if invalid.

#### Art-Net

- Node name => string, 1 to 17 bytes: letters, digits and hyphens only, not starting or ending with a hyphen (it is also used as the DHCP hostname and the web interface title tag)
- Art-Net Universe => integer, 0 to 32767
- Relay 1 DMX channel => integer, 1 to 512
- Relay 2 DMX channel => integer, 1 to 512

Numeric values must consist of digits only. Anything else (including signs, decimal points and exponents such as `5e1`) is invalid and must not be interpreted as a number.

#### WiFi

- Fallback access point SSID => string, 1 to 32 bytes, printable ASCII
- WiFi network SSID => string, 0 to 32 bytes, printable ASCII
- WiFi network password => empty (an open network), or 8 to 63 bytes. Any UTF-8 text is permitted, apart from control characters.

The password length is counted in bytes, not characters, because WPA/WPA2 and the ESP32 limit it in bytes: a character outside ASCII takes 2 to 4 bytes, so a password containing such characters has fewer characters available.

#### Web interface authentication

- Username => string, 1 to 32 bytes, printable ASCII
- Password => string, 1 to 32 bytes, printable ASCII

### Firmware version

The firmware has a version string, defined in one place in the source code and displayed in the status information.

The version string is the date and time of the last change to the source code, in the format `YYYY-MM-DD HH:MM`. It is updated by hand whenever the source code is changed.

The version is also printed at startup when `DEBUG` is enabled.

### Firmware update

The firmware can be updated over the air from the web interface, as an alternative to a UART connection. The update is a compiled ESP32 application binary (`.bin`; the application image only, not an image that includes the bootloader).

#### Availability

Firmware update is only available while the controller is running in station mode and connected to a WiFi network that has a password configured.

It is not available in fallback access point mode, or when connected to network without a configured password. In those cases the web interface does not offer the upload control, explains why, and the server rejects any update request.

Authentication is still required, but the default web credentials (`admin` / `admin`) are acceptable: the secured WiFi network is what protects the controller from unauthorised updates.

#### Update process

1. The user selects a firmware file in the web interface.
2. The browser checks the file's name and header (see below), and asks the user to confirm the update.
3. The file is uploaded, with a progress indication.
4. The controller verifies the received firmware.
5. If the firmware is good, the controller reboots into it, and the web interface waits for the controller to return and then reloads the status page. The reason for last reset/boot is reported as a firmware update.
6. If anything fails, an error is shown in the web interface and the controller carries on running the current firmware.

The relays follow the normal reboot behaviour: they are switched off at startup, and are controlled again once DMX data is received.

#### Protection against bad uploads

Corrupt, truncated or wrong files must not be able to replace the working firmware. This must not require any extra step from the user, such as entering a checksum.

- The new firmware is written to the inactive OTA partition, so the running firmware is not modified during the upload.
- The browser, and again the controller, check the start of the file: it must begin with the ESP32 image magic byte `0xE9` and contain the application descriptor magic word `0xABCD5432` at offset 32. This rejects files that are not an application image, such as an image that includes the bootloader.
- The new firmware is only activated if the ESP32 update library reports the whole image as complete and valid (`Update.end(true)` succeeds, which verifies the image's checksum and hash before switching the boot partition).
- A write error, a failed verification, or the connection being lost during the upload aborts the update without changing which firmware boots.
- Known limitation: there is no automatic rollback. An image that passes these checks but does not work (for example one that crashes at startup or cannot connect to WiFi) is not detected, and the controller does not revert to the previous firmware. Recovery in that case is by serial (UART) flashing.

#### Runtime behaviour during an update

- The status LED, Art-Net processing and ArtPoll replies continue to be serviced while the upload is received, so the relays keep following DMX data until the controller reboots.
- The flash must have a partition scheme with two OTA application slots (the Arduino default 4MB scheme has two slots of about 1.25MB). The compiled binary must fit in one slot.

---

## 6. Serial Debugging

Serial debugging shall operate at:

`115200 baud`

Debug output is controlled by three compile-time flags.

### DEBUG

Controls general debugging information, including:

- Startup information
- Loaded configuration
- Configuration changes
- Reboots
- WiFi connection attempts
- WiFi connection results
- IP address
- WiFi mode
- Web server startup
- Web requests (verbose only)
- Firmware version and firmware update progress and results
- Reconnection attempts
- Errors and other general diagnostic information

### DEBUG_ARTNET

Controls Art-Net-specific debugging information, including:

- Received Art-Net packets
- Packet type
- Universe
- Sequence information where relevant
- DMX data information
- Relay state changes

### DEBUG_VERBOSE

Determines debugging verbosity.

When disabled:

- Do not log requests to the web server
- Log only Art-Net packets that result in a relay state change

When enabled:

- Log requests to the web server, and the size of the generated status page together with the free heap memory
- Log every received Art-Net packet, including ArtPoll broadcasts, and each frame of DMX data

These flags should be independent.

For example:

```text
#define DEBUG 1
#define DEBUG_ARTNET 1
#define DEBUG_VERBOSE 0
```

### In-memory log and web interface panel

When `DEBUG` or `DEBUG_ARTNET` is set, everything written to the serial debug output shall also be kept in a memory buffer, and shown in a **Debug Log** panel at the bottom of the web interface.

- The buffer holds a fixed amount (16 KB) of the most recent output. When it is full, the oldest entries are discarded to make room for new ones.
- Each line in the web log starts with the time since boot, in seconds. The serial output is unchanged.
- The newest entry is at the bottom of the panel. The panel has a button to refresh it and an option to refresh it automatically every 2 seconds. Automatic refresh follows the newest output, unless the user has scrolled up to read earlier output.
- A refresh fetches only the entries added since the previous one, and appends them to the entries already in the panel, so earlier entries remain in the page for as long as it stays loaded.
- Where entries are missing, the panel shows "[older entries discarded]" on a line of its own. This is at the beginning, if entries had already been discarded from the buffer when the page was loaded; and between the previous entries and the new ones, if a refresh finds that entries have been discarded since the previous refresh.
- If the controller has restarted since the previous refresh, the panel shows "*****  Rebooted *****" on a line of its own, with a blank line before it, between the previous entries and the entries from the new run.
- The panel loads the log from a separate page, which requires the same authentication as the rest of the web interface. Requests for that page are not themselves logged, so that refreshing the panel does not fill the log.

When both `DEBUG` and `DEBUG_ARTNET` are off, none of this exists: no memory is reserved for the buffer, the panel is not shown, and the log page is not served.

Logging every received Art-Net packet (DEBUG_ARTNET together with DEBUG_VERBOSE) produces a lot of output, so the buffer may then hold only the last few seconds.

---

## 7. Startup Sequence

The intended startup sequence is:

1. Initialise serial debugging.
2. Initialise GPIOs (relays and status LED).
3. Set relays and status LED to initial state Off.
4. Load configuration from non-volatile storage.
5. Initialise WiFi.
6. Attempt configured WiFi networks in order.
7. If a network connects:
    - Start normal WiFi/Station operation.
    - Start Art-Net processing.
    - Start the web interface.
8. If no configured network connects:
    - Start the fallback access point.
    - Start DHCP.
    - Start Art-Net processing.
    - Start the web interface.
9. Continue monitoring the WiFi connection.
10. If an established connection is lost:
    - Attempt configured networks repeatedly.
    - Do not start the fallback AP.

---

## 8. Runtime Behaviour

The main firmware loop must remain responsive to:

- Art-Net packets
- Relay state changes
- Web requests
- WiFi reconnection
- Configuration requests

Long blocking delays should be avoided.

While attempting to connect to WiFi networks, blocking is acceptable since the web interface and Art-Net will be unavailable until connected.

The state of the relays must remain unchanged in case of partial packet received (packet not containing relay channel), network failure, and while reconnection is being attempted.

No DMX timeout shall be applied. If Art-Net communication stops, each relay shall retain its last known state indefinitely until a valid ArtDMX packet changes it.

### Status LED

The on-board LED on GPIO23 indicates the operating state of the controller. The first matching condition in the following table applies:

| Priority | Condition | LED pattern |
|:-:|---|---|
| 1 | Attempting to connect to WiFi networks (at boot or during reconnection) | Fast flash |
| 2 | ArtDMX data received for the configured universe within the last 3 seconds | Solid on |
| 3 | Fallback access point mode, and no such ArtDMX data within the last 3 seconds | Occasional flash (mostly off) |
| 4 | Otherwise (powered on, no such ArtDMX data within the last 3 seconds) | Slow flash |

Only ArtDMX packets for the configured universe count as received DMX data. ArtPoll packets and packets for other universes do not affect the LED.

The 3 second window is deliberately longer than the 2 second refresh interval of the QLC+ controller that only resends unchanged DMX data every 2 seconds, so that the LED stays solid between refreshes.

Flash timings:

- Fast flash: 100ms on, 100ms off
- Slow flash: 500ms on, 500ms off
- Occasional flash: 100ms on once every 2 seconds

The LED pattern must be generated without blocking the main loop.

---

## 9. Security

The controller must support WPA/WPA2-secured WiFi networks as supported by the ESP32 WiFi stack.

The web interface must require authentication, except /ping which returns a static "OK".

Default web credentials on first boot:

    Username: admin
    Password: admin

Passwords should not be printed in serial debug output.

Firmware updates through the web interface are only accepted when connected to a password-protected WiFi network, and never in fallback access point mode, as described in section 5.

---

## 10. Hardware Constants

Hardware-specific values should be defined in one place in the source code.

Example:

    RELAY_1 = 17
    RELAY_2 = 16
    STATUS_LED = 23

The relay and status LED GPIO assignments, and the LED on-level (polarity), should not be scattered throughout the firmware.

---

## 11. Non-Goals

The firmware is intentionally intended to remain simple.

The following are not currently required:

- RDM
- DMX input/output over physical RS-485
- Multiple Art-Net universes
- Art-Net output
- sACN
- Complex lighting effects
- Relay dimming
- Cloud connectivity
- Mobile application
- MQTT
- Database
- External server dependency

Additional functionality should only be added if there is a specific requirement for it.