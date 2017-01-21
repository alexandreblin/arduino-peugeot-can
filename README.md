# Arduino Peugeot 207 CAN bus sketch

This sketch reads data from my Peugeot 207 "confort" CAN bus and sends it over serial. It is meant to replace the stock screen by a custom color LCD running an [iOS app](https://github.com/alexandreblin/ios-car-dashboard), and add a backup camera (see this [blog post](https://medium.com/@alexandreblin/can-bus-reverse-engineering-with-arduino-and-ios-5627f2b1709a) for more information).

It works with a [CAN bus shield](http://wiki.seeed.cc/CAN-BUS_Shield_V1.2/) (or any MCP2515 based boards) and the [CAN bus library](https://github.com/Seeed-Studio/CAN_BUS_Shield) from Seeedstudio. I only tested it on a Peugeot 207 but it should work on all RD4-equipped Peugeot/Citroen cars.

## Serial packets
The packets sent over serial are formatted like this:

| 0x12  | 0xXX   | 0xXX | 0xXX | … | 0xXX | 0x13 |
|-------|--------|------|------|---|------|------|
| Start | Length | Type | Data | … | Data | End  |

A frame starts with `0x12`, and ends with `0x13`. The length is the number of bytes between start and end bytes. The type is a byte containing which data will be sent (volume is `0x01`, outside temperature is `0x02`, etc...).

If any byte inside the frame is equal to `0x12`, `0x13`, or `0x7e` (including length and type bytes), it's preceded by `0x7e` and XOR'd by `0x20` (e.g `0x13` becomes `0x7E 0x33`).

## Wiring
I wired the CAN shield to pins 2 and 7 (part C), behind the car's RD4 radio unit.
![RD4 wiring diagram](http://i.imgur.com/oGioW6B.jpg)
![RD4 wiring photo](http://i.imgur.com/EMkS5Xyl.jpg)
