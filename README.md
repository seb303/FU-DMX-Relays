# ESP32 Art-Net Relay Controller

Two-relay ESP32 controller for simple on/off lighting control via **Art-Net 4 / DMX over WiFi**.

![ESP32 Art-Net Relay Controller PCB](photos/board-00.jpg)

The image below shows the board fitted inside a trailing mains extension lead. Note that the plug fuse has been changed to 3A, since the standard 13A fuse would exceed the rating of the relays.

![ESP32 Art-Net Relay Controller PCB](photos/installed-01.jpg)

## Features

* Art-Net* 4 / DMX over WiFi
* Two onboard relay outputs
* Mains powered
* Configurable DMX channels
* Configurable Art-Net universe
* Web-based configuration and status interface
* Up to three stored WiFi networks
* Fallback open access point for initial configuration
* Persistent configuration across reboots
* Serial debugging
* Lightweight implementation with no unnecessary functionality

*Art-Net™ Designed by and Copyright Artistic Licence Engineering Ltd

## Hardware

ESP32-based two-relay board.

| Relay   |   GPIO |
| ------- | -----: |
| Relay 1 | GPIO17 |
| Relay 2 | GPIO16 |

eBay listing: https://www.ebay.co.uk/itm/277016777105

## Documentation

See **[SPEC.md](SPEC.md)** for the complete firmware specification.