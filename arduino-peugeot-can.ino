#include <SPI.h>
#include "mcp_can.h"

int reverseCamPin = 3; // LCD AV2 source switch pin
int screenPin = 6; // LCD power switch pin

MCP_CAN CAN(10); // CAN Shield interface

// Enumeration representing the type of frames sent over serial
typedef enum {
    INIT_STATUS_FRAME    = 0x00,
    VOLUME_FRAME         = 0x01,
    TEMPERATURE_FRAME    = 0x02,
    RADIO_SOURCE_FRAME   = 0x03,
    RADIO_NAME_FRAME     = 0x04,
    RADIO_FREQ_FRAME     = 0x05,
    RADIO_FMTYPE_FRAME   = 0x06,
    RADIO_DESC_FRAME     = 0x07,
    INFO_MSG_FRAME       = 0x08,
    RADIO_STATIONS_FRAME = 0x09,
    SEATBELTS_FRAME      = 0x0A,
    AIRBAG_STATUS_FRAME  = 0x0B,
    INFO_TRIP1_FRAME     = 0x0C,
    INFO_TRIP2_FRAME     = 0x0D,
    INFO_INSTANT_FRAME   = 0x0E,
    TRIP_MODE_FRAME      = 0x0F,
    AUDIO_SETTINGS_FRAME = 0x10,
    SECRET_FRAME         = 0x42, // Dark button
} FrameType;

// Screen power state
byte screenOn = 0;
byte wantedScreenState = 0;
unsigned long timeSinceSourceChange = 0;

// Reverse gear state
byte reverseEngaged = 0;

bool shouldStopReverse = false;
unsigned long timeSinceReverseDisengaged = 0;

// Radio volume
int volume = 0;

// Outside temperature
int temperature = 0;

// Radio source (FM, AUX1, AUX2, ...)
int radioSource = 0;

// FM band number (1, 2, AST)
int fmType = 0;

// Radio frequency
int fmFreq = 0;

// Radio station name
char radioName[9];

// Radio text
char radioMsg[100];
char msgRecvCount = 0;

// Saved stations
char stations[100];
char stationsRecvCount = 0;
char tempBuffer[100];

// Seat belts status bitmask
byte seatBeltStatus = 0;

// Passenger airbag state
boolean airbagStatus = 0;

// Information message data (automatic wipers, open door, ...)
byte messageInfo[8];

// Audio settings (bass/treble, equalizer, ...)
byte audioSettings[7];

// Trip computer data (memory 1, memory 2, instant data)
byte infoTrip1[7];
byte infoTrip2[7];
byte infoInstant[7];

// Current displayed trip computer data
byte tripMode = 0;

// Trip mode button state
boolean tripModeButtonPressed = false;

boolean tripDidReset = false;
unsigned long timeSinceTripInfoButtonPressed = 0;

// Secret button state
byte secretButtonPressed = 0;

void setup()
{
    // Configure pin modes
    pinMode(reverseCamPin, OUTPUT);
    digitalWrite(reverseCamPin, LOW);

    pinMode(screenPin, OUTPUT);
    digitalWrite(screenPin, LOW);

    // Initialize serial port and CAN bus shield
    Serial.begin(115200);

    while (CAN.begin(CAN_125KBPS) != CAN_OK) {
        // Retry until successful init
        sendByteWithType(INIT_STATUS_FRAME, 0x01);
        delay(100);
    }

    // Successful init
    sendByteWithType(INIT_STATUS_FRAME, 0x00);
}

void loop()
{
    unsigned char len = 0;
    byte buf[8];
    byte tmp[9];
    int tempValue;

    // -------
    // Process timers
    // -------
    // There are 3 timers in place :
    //  - timeSinceSourceChange: when the screen is on AV2 or switching from AV2 to HDMI
    //    (when we stop reversing), we must not power down the screen or it will stay on AV2
    //    when turning it back on, instead of HDMI. So if we need to turn off the screen, we
    //    set the wantedScreenState to 0, and the timer will make sure we actually turn off
    //    the screen once it's back on HDMI (9 seconds seems to be the minimum value for
    //    reliable results). So if the car is stopped right after disengaging the reverse,
    //    the screen will stay on a little while and turn off.
    //
    //  - timeSinceReverseDisengaged: this timer waits a few seconds after disengaging reverse
    //    before switching back to HDMI, in case we're parallel parking and switching back and
    //    forth between reverse and first gear. If we don't do this, the screen would switch
    //    back to HDMI immediately (which takes some time) and it takes some time to go back
    //    to AV2 again, which is annoying when parking.
    //
    //  - timeSinceTripInfoButtonPressed: this timer sends a reset frame on the CAN bus when
    //    the trip mode button is pressed for more than 2 seconds.

    if (shouldStopReverse && millis() - timeSinceReverseDisengaged > 5000) {
        digitalWrite(reverseCamPin, LOW);
        timeSinceSourceChange = millis();
        reverseEngaged = false;
        shouldStopReverse = false;
        timeSinceReverseDisengaged = 0;
    }

    if (screenOn != wantedScreenState && millis() - timeSinceSourceChange > 9000) {
        screenOn = wantedScreenState;

        digitalWrite(screenPin, HIGH);
        delay(100);
        digitalWrite(screenPin, LOW);
    }

    if (tripModeButtonPressed && tripMode > 0 && millis() - timeSinceTripInfoButtonPressed > 2000) {
        // This is sent multiple times deliberately as long as the button is pressed
        // because sending the frame once isn't enough to trigger the reset
        // FIXME: we should test explicitely the tripModes because it will currently
        // reset the second memory if tripMode is equal to any value besides 1
        byte data[] = {
            tripMode == 1 ? 0x82 : 0x44, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00
        };
        CAN.sendMsgBuf(359, 0, 8, data);
        tripDidReset = true;
    }

    // -------
    // Process data from the CAN bus
    // -------

    if (CAN.checkReceive() == CAN_MSGAVAIL) {
        // Read CAN frame into `buf` and length into `len`
        CAN.readMsgBuf(&len, buf);
        int id = CAN.getCanId();

        if (id == 246 && len == 8) {
            // Reverse gear state
            if (buf[7] & 0b10000000) {
                // Reverse gear engaged
                digitalWrite(reverseCamPin, HIGH);
                reverseEngaged = true;
                shouldStopReverse = false;
                timeSinceReverseDisengaged = 0;
            } else {
                // Reverse gear disengaged
                if (reverseEngaged && !shouldStopReverse) {
                    shouldStopReverse = true;
                    timeSinceReverseDisengaged = millis();
                }
            }

            // Decode temperature value and send it if it changed
            tempValue = ceil((buf[5] & 0xFF) / 2.0) - 40;
            if (temperature != tempValue) {
                temperature = tempValue;
                sendByteWithType(TEMPERATURE_FRAME, temperature);
            }
        } else if (id == 480) {
            // Radio power state, turns the screen on when turning on the ignition
            if (wantedScreenState != buf[0]) {
                wantedScreenState = buf[0];
            }
        } else if (id == 997) {
            // Switch LCD power manually when pressing MENU + OK simultaneously
            // (in case it gets out of sync for any reason)
            if ((buf[2] & 0b01010000) == 0b01010000) {
                digitalWrite(screenPin, HIGH);
                delay(100);
                digitalWrite(screenPin, LOW);
            }

            // Reset all data stored in memory when pressing MENU + ESC
            // Forces a resend of all data on the next loop() iteration
            // (in case we restarted the iOS app)
            if ((buf[2] & 0b00010000) && (buf[0] & 0b01000000)) {
                volume = 0;
                temperature = 0;
                radioSource = 0;
                fmType = 0;
                fmFreq = 0;
                memset(radioName, 0, sizeof radioName);
                memset(radioMsg, 0, sizeof radioMsg);
                memset(stations, 0, sizeof stations);
                seatBeltStatus = 0;
                airbagStatus = 0;
                memset(messageInfo, 0, sizeof messageInfo);
                memset(infoTrip1, 0, sizeof infoTrip1);
                memset(infoTrip2, 0, sizeof infoTrip2);
                memset(infoInstant, 0, sizeof infoInstant);
                memset(audioSettings, 0, sizeof audioSettings);
                secretButtonPressed = 0;
            }

            // Check whether the DARK button is pressed or not
            tempValue = (buf[2] & 0x04) == 0x04;
            if (secretButtonPressed != tempValue) {
                secretButtonPressed = tempValue;

                sendByteWithType(SECRET_FRAME, secretButtonPressed);
            }
        } else if (id == 421) {
            // Volume
            tempValue = buf[0] & 0b00011111;
            if (volume != tempValue) {
                volume = tempValue;

                sendByteWithType(VOLUME_FRAME, volume);
            }
        } else if (id == 357) {
            // Radio source
            tempValue = buf[2] >> 4;
            if (radioSource != tempValue) {
                radioSource = tempValue;

                sendByteWithType(RADIO_SOURCE_FRAME, radioSource);
            }
        } else if (id == 677) {
            // Radio station name
            if (strncmp((char*)buf, radioName, len)) {
                strncpy(radioName, (char*)buf, len);

                sendFrameWithType(RADIO_NAME_FRAME, buf, len);
            }
        } else if (id == 549) {
            // Radio frequency
            tempValue = ((((buf[3] & 0xFF) << 8) + (buf[4] & 0xFF)) / 2 + 500);
            if (fmFreq != tempValue) {
                fmFreq = tempValue;

                byte freqBytes[] = { (fmFreq >> 8) & 0xFF, fmFreq & 0xFF };
                sendFrameWithType(RADIO_FREQ_FRAME, freqBytes, 2);
            }

            // FM type
            tempValue = buf[2] >> 4;
            if (fmType != tempValue) {
                fmType = tempValue;

                sendByteWithType(RADIO_FMTYPE_FRAME, fmType);
            }
        } else if (id == 164) {
            // Radio text frame

            // This frame can have different meanings depending on its first byte
            // When the radio receive a message from RDS, it will send a frame
            // where the first byte is 0x10 and the two last bytes contains the first 2
            // characters of the message.
            // We send need to send CAN frame 159 with [0x30, 0x00, 0x0A] to receive the
            // remaining data
            // The radio will then send numbered frames containing the remaining text
            // (from 0x20 to 0x29). We have to be careful as they are not always received
            // in the right order, so we have to fill the radioMsg array according to the
            // frame "index". When we received 10 frames, the text is complete and we can
            // send it over serial. If the radio stops sending the text for a any reason,
            // it will send a frame with its first byte set to 0x05 to "reset" the counter
            // so we can receive a full frame next time.

            if (buf[0] == 0x05) {
                msgRecvCount = 0;
            }

            if (buf[0] & 0x10) {
                msgRecvCount++;

                radioMsg[0] = buf[6];
                radioMsg[1] = buf[7];

                byte data[] = { 0x30, 0x00, 0x0A };
                CAN.sendMsgBuf(159, 0, 3, data);
            } else if (buf[0] & 0x20) {
                msgRecvCount++;

                int idx = buf[0] & 0x0F;
                for (int i = 1; i < len; i++) {
                    radioMsg[2 + (idx - 1) * 7 + (i - 1)] = buf[i];
                }

                if (buf[0] == 0x29) {
                    radioMsg[2 + (idx - 1) * 7 + (len - 1)] = '\0';
                }
            }

            if (msgRecvCount == 10) {
                sendFrameWithType(RADIO_DESC_FRAME, (byte*)radioMsg, strlen(radioMsg));
                msgRecvCount = 0;
            }
        } else if (id == 293) {
            // Memorized radio station names. Works roughly the same way as frame 164
            // (see above)
            if (buf[0] & 0x10) {
                stationsRecvCount = 0;

                tempBuffer[0] = buf[6];
                tempBuffer[1] = buf[7];
            } else if (buf[0] & 0x20) {
                stationsRecvCount++;

                int idx = buf[0] & 0x0F;
                for (int i = 1; i < len; i++) {
                    tempBuffer[2 + (idx - 1) * 7 + (i - 1)] = buf[i];
                }

                if (buf[0] == 0x29) {
                    tempBuffer[2 + (idx - 1) * 7 + (len - 1)] = '\0';
                }
            }

            if (stationsRecvCount == 8) {
                char* p = tempBuffer;
                while (*p != '\0') {
                    if (*p == '\xA0' || *p == '\xB0' || *p == '\x90' || *p > 127) {
                        // set separator between station names
                        *p = '|';
                    }

                    p++;
                }

                if (strcmp(tempBuffer, stations)) {
                    strcpy(stations, tempBuffer);

                    sendFrameWithType(RADIO_STATIONS_FRAME, (byte*)stations, strlen(stations));
                }
                stationsRecvCount = 0;
            }
        } else if (id == 296) {
            // Seat belts frame
            tempValue = (buf[0] | (buf[5] << 1)) & 0xFF;
            if (seatBeltStatus != tempValue) {
                seatBeltStatus = tempValue;

                sendByteWithType(SEATBELTS_FRAME, seatBeltStatus);
            }
        } else if (id == 24) {
            // Passenger airbag state frame
            tempValue = (buf[0] >> 7) & 1;

            if (airbagStatus != tempValue) {
                airbagStatus = tempValue;

                sendByteWithType(AIRBAG_STATUS_FRAME, airbagStatus);
            }
        } else if (id == 417) {
            // Information message frame
            // We send the raw frame over serial as there is many different data
            // to parse in it, so we do it on the iOS app side
            for (int i = 0; i < len; ++i) {
                tempBuffer[i] = buf[i];
            }

            if (memcmp(tempBuffer, messageInfo, 8)) {
                memcpy(messageInfo, tempBuffer, 8);

                sendFrameWithType(INFO_MSG_FRAME, messageInfo, 8);
            }
        } else if (id == 673 || id == 609 || id == 545) {
            // Trip computer data frames
            // There is 3 different frames (1 for each data set)
            // but they're all structured the same way
            byte* value;
            byte frameType;

            switch (id) {
            case 673:
                value = infoTrip1;
                frameType = INFO_TRIP1_FRAME;
                break;
            case 609:
                value = infoTrip2;
                frameType = INFO_TRIP2_FRAME;
                break;
            case 545:
                value = infoInstant;
                frameType = INFO_INSTANT_FRAME;
                break;
            }

            if (memcmp(buf, value, 7)) {
                memcpy(value, buf, 7);

                sendFrameWithType(frameType, value, 7);
            }

            if (id == 545) {
                // Special treatment for the instant data frame
                // which contains the trip data button state
                if ((buf[0] & 0x0F) == 0x08 && !tripModeButtonPressed) {
                    tripModeButtonPressed = true;
                    timeSinceTripInfoButtonPressed = millis();
                } else if ((buf[0] & 0x0F) == 0x00) {
                    if (tripModeButtonPressed) {
                        if (!tripDidReset) {
                            tripMode++;
                            tripMode %= 3;
                            sendByteWithType(TRIP_MODE_FRAME, tripMode);
                        } else {
                            for (int i = 0; i < 50; i++) {
                                // We need to send this to actually stop the reset
                                // (else the distance/fuel counters never goes up again)
                                // FIXME: we should test explicitely the tripModes because it
                                // will currently reset the second memory if tripMode is equal
                                // to any value besides 1
                                byte data[] = { tripMode == 1 ? 0x02 : 0x04,
                                    0x00,
                                    0xFF,
                                    0xFF,
                                    0x00,
                                    0x00,
                                    0x00,
                                    0x00 };
                                CAN.sendMsgBuf(359, 0, 8, data);
                            }
                        }

                        tripDidReset = false;
                        tripModeButtonPressed = false;
                        timeSinceTripInfoButtonPressed = 0;
                    }
                }
            }
        } else if (id == 485) {
            // Audio settings frame
            // Same as information message frame: we send the raw frame and parse it
            // in the iOS app
            for (int i = 0; i < 7; ++i) {
                tempBuffer[i] = buf[i];
            }

            if (memcmp(tempBuffer, audioSettings, 7)) {
                memcpy(audioSettings, tempBuffer, 7);

                sendFrameWithType(AUDIO_SETTINGS_FRAME, audioSettings, 7);
            }
        }
    }
}

/*
 Serial packet structure:

 0x12   0x04    0xXX  0xXX  0xXX  0xXX  0x13
 start  length  type  `--   data   --´  end

 Escape sequence: 0x7e

 If any byte between start and end is 0x12, 0x13, or 0x7e,
 it is preceded by 0x7e and the byte is XOR'd by 0x20
*/

#define FRAME_START 0x12
#define FRAME_END 0x13
#define FRAME_ESCAPE 0x7E
#define ESCAPE_XOR 0x20

#define isControlChar(x) \
    (x == FRAME_START || x == FRAME_END || x == FRAME_ESCAPE)

byte serialBuffer[100];

void sendFrameWithType(byte frameType, const byte* data, int dataLength)
{
    int pos = 0;

    serialBuffer[pos++] = FRAME_START;

    byte lengthByte = dataLength + 1; // account for frame type
    if (isControlChar(lengthByte)) {
        serialBuffer[pos++] = FRAME_ESCAPE;
        lengthByte ^= ESCAPE_XOR;
    }

    serialBuffer[pos++] = lengthByte;

    if (isControlChar(frameType)) {
        serialBuffer[pos++] = FRAME_ESCAPE;
        frameType ^= ESCAPE_XOR;
    }

    serialBuffer[pos++] = frameType;

    for (int i = 0; i < dataLength; ++i) {
        byte b = data[i];
        if (isControlChar(b)) {
            serialBuffer[pos++] = FRAME_ESCAPE;

            b ^= ESCAPE_XOR;
        }

        serialBuffer[pos++] = b;
    }

    serialBuffer[pos++] = FRAME_END;

    Serial.write(serialBuffer, pos);
}

inline void sendByteWithType(byte frameType, byte byteToSend)
{
    byte arr[] = { byteToSend };
    sendFrameWithType(frameType, arr, 1);
}
