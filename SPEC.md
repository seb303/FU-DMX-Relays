# ESP32 Art-Net Relay Controller

## 1. Overview

Firmware for an ESP32-based two-relay controller.

The software receives Art-Net 4 / DMX data over WiFi and uses two DMX channels to control two onboard relays.

The controller is intended for simple, reliable control of two on/off lighting outputs. The implementation should remain lightweight and avoid unnecessary functionality.

The specific control application used will be [Radig DMX Control](https://www.ulrichradig.de/home/index.php/software/radig-dmx-control), but the firmware should also work with any Art-Net application.

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

There should be a minimum relay dwell time / anti-chatter guard of 200ms to prevent excessive wear to the relays from a rapidly changing control signal.

---

## 3. Art-Net

The controller shall receive Art-Net 4 / DMX packets over WiFi.

The Art-Net serial should be derived from the hardware MAC address and remain constant between reboots.

### Art-Net configuration

The following Art-Net parameters are configurable via the web interface:

- Node name (maximum 17 characters, used for both short and long name)
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

The hostname used when obtaining an IP address via DHCP shall match the configured Art-Net node name.

### Boot-time connection behaviour

On boot:

1. Attempt to connect to configured WiFi network 1.
2. If unsuccessful, attempt network 2.
3. Continue through all configured networks until a successful connection.
4. If none are available, start the fallback open access point.

The connection process should have sensible connection timeouts so that an unavailable network does not prevent the controller from progressing to the next configured network.

### Subsequent connection failure

If the controller successfully connects to a configured WiFi network and the connection subsequently fails:

1. Do not start the fallback access point.
2. Continually attempt to reconnect.
3. Cycle through the configured WiFi networks in turn.
4. Continue attempting until a configured network becomes available.

The fallback access point is a boot-time fallback only.

Art-Net processing (ArtPoll replies and ArtDMX reception) is active in fallback access point mode in the same way as in station mode, so a controller connected directly to the access point can control the relays.

### Fallback access point

The fallback access point is has no password and provides DHCP. The device will have a fixed IP of 192.168.4.1 on subnet 192.168.4.0/24.

#### Recovery while in fallback access point mode

A brief power cut can cause both the WiFi router and this controller to reboot together, with the controller starting up faster and finding no configured network available, so it starts the fallback access point. Without a recovery mechanism it would then stay in access point mode indefinitely, even once the router comes back.

While running as the fallback access point, the controller shall periodically scan for WiFi networks:

1. If a configured network is found, attempt to connect to it.
2. If the connection succeeds, stop the fallback access point and resume normal station operation.
3. If the connection fails, or no configured network is found, resume the fallback access point and continue scanning periodically.

This scanning must not stop the access point from working while it is in progress, and must not block Art-Net processing or the web interface.

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
- WiFi mode
- IP address
- Gateway IP address
- Connected network SSID or indicate fallback access point mode
- WiFi signal strength (RSSI)
- Uptime (time since last reset/boot)
- Reason for last reset/boot (power-on, configuration change, watchdog timeout, brownout, panic, etc.)
- Time since last DMX data received for the configured universe (snapshot when web page loaded)
- Time since last ArtPoll targetting the node (snapshot when web page loaded)
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

### Actions

The configuration interface shall provide a **Save & Reboot** button and a **Discard & Reboot** button.

The **Save & Reboot** button shall:

1. Confirm the Save & Reboot action
2. Validate the submitted configuration.
3. Store the configuration in non-volatile memory.
4. Reboot the ESP32.
5. Apply the new configuration during startup.

Configuration should not be changed permanently until the user selects Save & Reboot.

The **Discard & Reboot** button shall:

1. Confirm the Discard & Reboot action
2. Discard any changes to the form, keeping the current configuration.
3. Reboot the ESP32.

### Validation

Configured values should be validated on the client-side, and prevent submission if invalid. They should also be validated on the server-side and prevent store/update of the individual value if invalid.

#### Art-Net

- Node name => string, 1 to 17 bytes
- Art-Net Universe => integer, 0 to 32767
- Relay 1 DMX channel => integer, 1 to 512
- Relay 2 DMX channel => integer, 1 to 512

#### WiFi

- Fallback access point SSID => string, 1 to 32 bytes
- WiFi network SSID => string, 0 to 32 bytes
- WiFi network password => string, 0 to 32 bytes

Each WiFi password field shall have a control to show or hide the password characters.

#### Web interface authentication

- Username => string, 1 to 32 bytes
- Password => string, 1 to 32 bytes

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
- Web requests
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

- Log requests to the web server
- Log every received Art-Net packet, including ArtPoll broadcasts, and each frame of DMX data

These flags should be independent.

For example:

```text
#define DEBUG 1
#define DEBUG_ARTNET 1
#define DEBUG_VERBOSE 0
```

---

## 7. Startup Sequence

The intended startup sequence is:

1. Initialise serial debugging.
2. Initialise GPIOs (relays and status LED).
3. Set both relays to initial state Off.
4. Load configuration from non-volatile storage.
5. Display configuration if `DEBUG` is enabled.
6. Initialise WiFi.
7. Attempt configured WiFi networks in order.
8. If a network connects:
    - Start normal WiFi/Station operation.
    - Start Art-Net processing.
    - Start the web interface.
9. If no configured network connects:
    - Start the fallback access point.
    - Start DHCP.
    - Start Art-Net processing.
    - Start the web interface.
10. Continue monitoring the WiFi connection.
11. If an established connection is lost:
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
| 2 | ArtDMX data received for the configured universe within the last 1 second | Solid on |
| 3 | Fallback access point mode, and no such ArtDMX data within the last 1 second | Occasional flash (mostly off) |
| 4 | Otherwise (powered on, no such ArtDMX data within the last 1 second) | Slow flash |

Only ArtDMX packets for the configured universe count as received DMX data. ArtPoll packets and packets for other universes do not affect the LED.

Flash timings:

- Fast flash: 100ms on, 100ms off
- Slow flash: 500ms on, 500ms off
- Occasional flash: 100ms on once every 2 seconds

The LED pattern must be generated without blocking the main loop.

---

## 9. Security

The controller must support WPA/WPA2-secured WiFi networks as supported by the ESP32 WiFi stack.

The web interface must require authentication.

Default web credentials on first boot:

    Username: admin
    Password: admin

Passwords should not be printed in serial debug output.

---

## 10. Configuration Validation

The web interface should validate configuration before saving.

Examples:

### Node name

- Maximum 17 characters

### Universe

- Must be within the valid Art-Net universe range used by the implementation
- 0 is also permitted, even though deprecated by the current standard

### DMX channels

- Must be valid DMX channel numbers
- Relay 1 and Relay 2 may use the same or different channels

### WiFi

- Empty SSID entries should be ignored
- Passwords should be stored exactly as entered
- Invalid or incomplete entries should not prevent the remaining configured networks from being attempted

---

## 11. Hardware Constants

Hardware-specific values should be defined in one place in the source code.

Example:

    RELAY_1 = 17
    RELAY_2 = 16
    STATUS_LED = 23

The relay and status LED GPIO assignments, and the LED on-level (polarity), should not be scattered throughout the firmware.

---

## 12. Non-Goals

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
- Remote firmware management
- Mobile application
- MQTT
- Database
- External server dependency

Additional functionality should only be added if there is a specific requirement for it.