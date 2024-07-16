/*
 * msx-joyduke v1, an original Xbox controller to MSX joystick adapter
 * Copyright (C) 2024 Albert Herranz
 *
 * This code is released under GPL V2.0
 *
 * Valid-License-Identifier: GPL-2.0-only
 * SPDX-URL: https://spdx.org/licenses/GPL-2.0-only.html
 *
 * Based on USB Host Shield 2.0 library
 * - https://github.com/felis/USB_Host_Shield_2.0
 *
 * Other references:
 * - https://chris-donnelly.github.io/xboxpad.html
 * - https://github.com/felis/USB_Host_Shield_2.0
 * - https://www.pjrc.com/teensy/td_libs_USBHostShield.html
 * - https://chrz.de/2023/12/05/fido2-hardware-part-2-usb-host-circuit-boards/
 * - https://www.msx.org/wiki/General_Purpose_port
 * - https://github.com/MCUdude/MiniCore
 * - https://www.arduino.cc/en/software
 */

/*
 Example sketch for the original Xbox library - developed by Kristian Lauszus
 For more information visit my blog: http://blog.tkjelectronics.dk/ or
 send me an e-mail:  kristianl@tkjelectronics.com
 */

/*
 SS           YELLOW atmega328p -> usbhs                 3.3V  ss (slave select)  10 yellow
 MOSI         ORANGE atmega328p -> usbhs                 3.3V  mosi               11 orange
 MISO         BROWN  usbhs -> atmega328p  open-collector 3.3V  miso               12 brown
 CLK          BLUE   atmega328p -> usbhs                 3.3V  sck                13 blue

 INT          WHITE                                                                9 white

 UHC_EN                                                                           A3
 VUSB_EN                                                                          A2
 green led                                                                        A0

*/
#define DEBUG
#include <XBOXOLD.h>
#include <usbhub.h>

// Satisfy the IDE, which needs to see the include statment in the ino too.
#ifdef dobogusinclude
#include <spi4teensy3.h>
#endif
#include <SPI.h>

USB Usb;
USBHub Hub1(&Usb);  // The controller has a built in hub, so this instance is needed
XBOXOLD Xbox(&Usb);

const byte PIN_HAVECONTROLLER = A0;
const byte PIN_UHC_EN = A3;
const byte PIN_VUSB_EN = A2;

/*
 * Atmega328p pins for MSX signals.
 * Use PORTD for all MSX joystick arrow and buttons signals.
 * This allows to set them all at the same time.
 */
#define PORT_MSX_JOYSTICK PORTD
#define DDR_MSX_JOYSTICK DDRD

/* MSX general purpose signals as mapped to Atmega328p PORTD bits */
const int PORT_MSX_JOYSTICK_UP = 7;       /* PD7, MSX joystick pin1 */
const int PORT_MSX_JOYSTICK_DOWN = 6;     /* PD6, MSX joystick pin2 */
const int PORT_MSX_JOYSTICK_LEFT = 5;     /* PD5, MSX joystick pin3 */
const int PORT_MSX_JOYSTICK_RIGHT = 4;    /* PD4, MSX joystick pin4 */
const int PORT_MSX_JOYSTICK_TRIGGER1 = 3; /* PD3, MSX joystick pin6 */
const int PORT_MSX_JOYSTICK_TRIGGER2 = 2; /* PD2, MSX joystick pin7 */

volatile uint8_t curr_msx_joystick_signals = 0x00;

/* Microsoft original Xbox controller pad axis valid range */
#define XBOXOLD_AXIS_MIN -32768
#define XBOXOLD_AXIS_MAX  32767

/*
 * Discrete thresholds for Microsoft Xbox joystick analog pad axis:
 * - Values below the min threshold on the horizontal/vertical axis indicate
 *   left/down active, respectively.
 * - Values above the max threshold on the horizontal/vertical axis indicate
 *   right/up active, respectively.
 */
int16_t axis_threshold_min, axis_threshold_max;

/* serial output width control */
uint8_t output_width_count = 0;

#ifdef DEBUG
void print_hex8(uint8_t data)
{
  char tmp[2 + 1];
  byte nibble;

  nibble = (data >> 4) | 48;
  tmp[0] = (nibble > 57) ? nibble + (byte)39 : nibble;
  nibble = (data & 0x0f) | 48;
  tmp[1] = (nibble > 57) ? nibble + (byte)39 : nibble;
  tmp[2] = 0;

  Serial.print(tmp);
}

void print_hex16(uint16_t data)
{
  print_hex8((uint8_t)(data >> 8));
  print_hex8((uint8_t)(data & 0xff));
}
#endif

void print_rolling_sequence(void)
{
  static char rolling_chars[] = { '-', '\\', '|', '/' };
  static uint8_t rolling_index = 0;

  //Serial.write(8);
  Serial.print(rolling_chars[rolling_index++]);
  if (rolling_index >= sizeof(rolling_chars))
    rolling_index = 0;
}

inline void __update_msx_signals(uint8_t signals)
{
  /* write all signal states at once to MSX side */
  PORT_MSX_JOYSTICK = (signals);
  DDR_MSX_JOYSTICK = (~0);
}

void setup()
{
  int16_t axis_threshold;

  pinMode(PIN_HAVECONTROLLER, OUTPUT);
  pinMode(PIN_UHC_EN, OUTPUT);
  pinMode(PIN_VUSB_EN, OUTPUT);

  __update_msx_signals(curr_msx_joystick_signals);

  Serial.begin(115200);
  Serial.println(F("msx-joyduke-v1 20240630_1"));

  /*
     * Calculate XBOX controller analog pad thresholds.
     * Analog pad needs to be pushed from its origin in one direction at least
     * 1/6 of the complete run to get active
     */
  axis_threshold = (XBOXOLD_AXIS_MAX - XBOXOLD_AXIS_MIN) / 3;
  axis_threshold_min = XBOXOLD_AXIS_MIN + axis_threshold;
  axis_threshold_max = XBOXOLD_AXIS_MAX - axis_threshold;

  Serial.print("axis_threshold_max: ");
  Serial.println(axis_threshold_max);
  Serial.print("axis_threshold_min: ");
  Serial.println(axis_threshold_min);

  digitalWrite(PIN_VUSB_EN, HIGH);
  delay(150);
  digitalWrite(PIN_UHC_EN, HIGH);

  if (Usb.Init() == -1) {
    Serial.println(F("OSC did not start, halting!"));
    while (1)
      ;  // halt
  }
  Serial.println(F("XBOX Library Started"));
}

void (*soft_reset)(void) = 0;

void loop()
{
  static unsigned long last_health_check;
  static int16_t slx, sly, srx, sry;
  int16_t lx, ly, rx, ry;

  Usb.Task();

  if (millis() - last_health_check > 1000) {
    last_health_check = millis();

    uint8_t HIRQ;
    HIRQ = Usb.regRd(rHIRQ);  //determine interrupt source
    if (HIRQ == 0xff) {
      // something went wrong with the MAX3421, restart the whole thing
      Serial.println(F("MCU RESTART!"));
      delay(5);
      soft_reset();
    }
  }

  if (Xbox.XboxConnected) {
    digitalWrite(PIN_HAVECONTROLLER, HIGH);

    uint8_t msx_joystick_signals = 0x00;

    lx = Xbox.getAnalogHat(LeftHatX);
    ly = Xbox.getAnalogHat(LeftHatY);
    rx = Xbox.getAnalogHat(RightHatX);
    ry = Xbox.getAnalogHat(RightHatY);

#ifdef DEBUG    
    bool need_lf;

    if (Xbox.getButtonPress(BLACK) || Xbox.getButtonPress(WHITE)) {
      Serial.print("BLACK: ");
      Serial.print(Xbox.getButtonPress(BLACK));
      Serial.print("\tWHITE: ");
      Serial.println(Xbox.getButtonPress(WHITE));
      //Xbox.setRumbleOn(Xbox.getButtonPress(BLACK), Xbox.getButtonPress(WHITE));
    } else
      /*Xbox.setRumbleOn(0, 0)*/;

    if (Xbox.getButtonClick(START))
      Serial.println(F("Start"));
    if (Xbox.getButtonClick(BACK))
      Serial.println(F("Back"));
    if (Xbox.getButtonClick(L3))
      Serial.println(F("L3"));
    if (Xbox.getButtonClick(R3))
      Serial.println(F("R3"));

    if (Xbox.getButtonPress(A)) {
      Serial.print(F("A: "));
      Serial.println(Xbox.getButtonPress(A));
    }
    if (Xbox.getButtonPress(B)) {
      Serial.print(F("B: "));
      Serial.println(Xbox.getButtonPress(B));
    }
    if (Xbox.getButtonPress(X)) {
      Serial.print(F("X: "));
      Serial.println(Xbox.getButtonPress(X));
    }
    if (Xbox.getButtonPress(Y)) {
      Serial.print(F("Y: "));
      Serial.println(Xbox.getButtonPress(Y));
    }
    if (Xbox.getButtonPress(LT)) {
      Serial.print(F("LT: "));
      Serial.println(Xbox.getButtonPress(LT));
    }
    if (Xbox.getButtonPress(RT)) {
      Serial.print(F("RT: "));
      Serial.println(Xbox.getButtonPress(RT));
    }

    need_lf = false;
    if (lx > 7500 || lx < -7500) {
        if (lx != slx) {
            Serial.print(F("LeftHatX: "));
            Serial.print(lx);
            Serial.print("\t");
            need_lf = true;
        }
    }
    if (ly > 7500 || ly < -7500) {
        if (ly != sly) {
            Serial.print(F("LeftHatY: "));
            Serial.print(ly);
            Serial.print("\t");
            need_lf = true;
        }
    }
    if (need_lf)
        Serial.println();

    slx = lx;
    sly = ly;

    need_lf = false;
    if (rx > 7500 || rx < -7500) {
        if (rx != srx) {
            Serial.print(F("RightHatX: "));
            Serial.print(rx);
            Serial.print("\t");
            need_lf = true;
        }
    }
    if (ry > 7500 || ry < -7500) {
        if (ry != sry) {
            Serial.print(F("RightHatY: "));
            Serial.print(ry);
            Serial.print("\t");
            need_lf = true;
        }
    }
    if (need_lf)
        Serial.println();

    srx = rx;
    sry = ry;
#endif

    if (Xbox.getButtonPress(UP))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_UP);
    if (Xbox.getButtonPress(DOWN))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_DOWN);
    if (Xbox.getButtonPress(LEFT))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_LEFT);
    if (Xbox.getButtonPress(RIGHT))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_RIGHT);
    if (Xbox.getButtonPress(A))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_TRIGGER1);
    if (Xbox.getButtonPress(B))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_TRIGGER2);

    /* left analog hat */

    /* before activating DOWN check that UP is not activated */
    if ((ly < axis_threshold_min) && !(msx_joystick_signals & (1 << PORT_MSX_JOYSTICK_UP)))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_DOWN);

    /* before activating UP check that DOWN is not activated */
    if ((ly > axis_threshold_max) && !(msx_joystick_signals & (1 << PORT_MSX_JOYSTICK_DOWN)))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_UP);

    /* before activating LEFT check that RIGHT is not activated */
    if ((lx < axis_threshold_min) && !(msx_joystick_signals & (1 << PORT_MSX_JOYSTICK_RIGHT)))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_LEFT);

    /* before activating RIGHT check that LEFT is not activated */
    if ((lx > axis_threshold_max) && !(msx_joystick_signals & (1 << PORT_MSX_JOYSTICK_LEFT)))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_RIGHT);

    /* right analog hat */

    if ((ry < axis_threshold_min) && !(msx_joystick_signals & (1 << PORT_MSX_JOYSTICK_UP)))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_DOWN);
    if ((ry > axis_threshold_max) && !(msx_joystick_signals & (1 << PORT_MSX_JOYSTICK_DOWN)))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_UP);
    if ((rx < axis_threshold_min) && !(msx_joystick_signals & (1 << PORT_MSX_JOYSTICK_RIGHT)))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_LEFT);
    if ((rx > axis_threshold_max) && !(msx_joystick_signals & (1 << PORT_MSX_JOYSTICK_LEFT)))
      msx_joystick_signals |= (1 << PORT_MSX_JOYSTICK_RIGHT);

    curr_msx_joystick_signals = msx_joystick_signals;
    __update_msx_signals(curr_msx_joystick_signals);
  } else {
    digitalWrite(PIN_HAVECONTROLLER, LOW);

    curr_msx_joystick_signals = 0x00;
    __update_msx_signals(curr_msx_joystick_signals);

    /* write rolling sequence while not connected */
    print_rolling_sequence();
    /* wrap output at 80 columns ... */
    if (output_width_count++ > 79) {
      output_width_count = 0;
      Serial.println("");
    }
  }
  delay(1);
}
