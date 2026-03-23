#include "wled.h"

/*
 * Physical IO
 */

#define WLED_DEBOUNCE_THRESHOLD      50 // only consider button input of at least 50ms as valid (debouncing)
#define WLED_LONG_PRESS             600 // long press if button is released after held for at least 600ms
#define WLED_DOUBLE_PRESS           350 // double press if another press within 350ms after a short press
#define WLED_LONG_REPEATED_ACTION   400 // how often a repeated action (e.g. dimming) is fired on long press on button IDs >0
#define WLED_LONG_FACTORY_RESET   15000 // how long button 0 needs to be held to trigger a factory reset
#define WLED_LONG_DOWNLOAD_MODE   30000 // how long button 0 needs to be held to trigger download mode
#define WLED_TRIPLE_PRESS_WINDOW    500 // ms window to detect a triple press on button 0
#define WLED_LONG_BRI_STEPS          16 // how much to increase/decrease the brightness with each long press repetition

static const char _mqtt_topic_button[] PROGMEM = "%s/button/%d";  // optimize flash usage
static bool buttonBriDirection = false; // true: increase brightness, false: decrease brightness

// Flash all LEDs with a solid colour (r,g,b) a given number of times, then restore the previous state.
static void flashLEDs(byte r, byte g, byte b_, byte count) {
  byte savedCol[4] = {colPri[0], colPri[1], colPri[2], colPri[3]};
  byte savedBri = bri;
  for (byte i = 0; i < count; i++) {
    colPri[0] = r; colPri[1] = g; colPri[2] = b_; colPri[3] = 0;
    bri = 200;
    colorUpdated(CALL_MODE_NO_NOTIFY);
    strip.service();
    delay(250);
    bri = 0;
    stateUpdated(CALL_MODE_NO_NOTIFY);
    strip.service();
    delay(200);
  }
  colPri[0] = savedCol[0]; colPri[1] = savedCol[1];
  colPri[2] = savedCol[2]; colPri[3] = savedCol[3];
  bri = savedBri;
  colorUpdated(CALL_MODE_NO_NOTIFY);
}

void shortPressAction(uint8_t b)
{
  if (!buttons[b].macroButton) {
    switch (b) {
      case 0: setRandomColor(colPri); colorUpdated(CALL_MODE_BUTTON); break;
      case 1: ++effectCurrent %= strip.getModeCount(); stateChanged = true; colorUpdated(CALL_MODE_BUTTON); break;
    }
  } else {
    applyPreset(buttons[b].macroButton, CALL_MODE_BUTTON_PRESET);
  }

#ifndef WLED_DISABLE_MQTT
  if (buttonPublishMqtt && WLED_MQTT_CONNECTED) {
    char subuf[MQTT_MAX_TOPIC_LEN + 32];
    sprintf_P(subuf, _mqtt_topic_button, mqttDeviceTopic, (int)b);
    mqtt->publish(subuf, 0, false, "short");
  }
#endif
}

void longPressAction(uint8_t b)
{
  if (!buttons[b].macroLongPress) {
    switch (b) {
      case 0: toggleOnOff(); stateUpdated(CALL_MODE_BUTTON); break;
      case 1:
        if (buttonBriDirection) {
          if (bri == 255) break;
          if (bri >= 255 - WLED_LONG_BRI_STEPS) bri = 255;
          else bri += WLED_LONG_BRI_STEPS;
        } else {
          if (bri == 1) break;
          if (bri <= WLED_LONG_BRI_STEPS) bri = 1;
          else bri -= WLED_LONG_BRI_STEPS;
        }
        stateUpdated(CALL_MODE_BUTTON);
        buttons[b].pressedTime = millis();
        break; // repeatable action
    }
  } else {
    applyPreset(buttons[b].macroLongPress, CALL_MODE_BUTTON_PRESET);
  }

#ifndef WLED_DISABLE_MQTT
  if (buttonPublishMqtt && WLED_MQTT_CONNECTED) {
    char subuf[MQTT_MAX_TOPIC_LEN + 32];
    sprintf_P(subuf, _mqtt_topic_button, mqttDeviceTopic, (int)b);
    mqtt->publish(subuf, 0, false, "long");
  }
#endif
}

void doublePressAction(uint8_t b)
{
  if (!buttons[b].macroDoublePress) {
    switch (b) {
      //case 0: toggleOnOff(); colorUpdated(CALL_MODE_BUTTON); break;
      case 1: ++effectPalette %= getPaletteCount(); colorUpdated(CALL_MODE_BUTTON); break;
    }
  } else {
    applyPreset(buttons[b].macroDoublePress, CALL_MODE_BUTTON_PRESET);
  }

#ifndef WLED_DISABLE_MQTT
  if (buttonPublishMqtt && WLED_MQTT_CONNECTED) {
    char subuf[MQTT_MAX_TOPIC_LEN + 32];
    sprintf_P(subuf, _mqtt_topic_button, mqttDeviceTopic, (int)b);
    mqtt->publish(subuf, 0, false, "double");
  }
#endif
}

bool isButtonPressed(uint8_t b)
{
  if (buttons[b].pin < 0) return false;
  unsigned pin = buttons[b].pin;

  switch (buttons[b].type) {
    case BTN_TYPE_NONE:
    case BTN_TYPE_RESERVED:
      break;
    case BTN_TYPE_PUSH:
    case BTN_TYPE_SWITCH:
      if (digitalRead(pin) == LOW) return true;
      break;
    case BTN_TYPE_PUSH_ACT_HIGH:
    case BTN_TYPE_PIR_SENSOR:
      if (digitalRead(pin) == HIGH) return true;
      break;
    case BTN_TYPE_TOUCH:
    case BTN_TYPE_TOUCH_SWITCH:
      #if defined(ARDUINO_ARCH_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32C3)
        #ifdef SOC_TOUCH_VERSION_2 //ESP32 S2 and S3 provide a function to check touch state (state is updated in interrupt)
        if (touchInterruptGetLastStatus(pin)) return true;
        #else
        if (digitalPinToTouchChannel(pin) >= 0 && touchRead(pin) <= touchThreshold) return true;
        #endif
      #endif
     break;
  }
  return false;
}

void handleSwitch(uint8_t b)
{
  // isButtonPressed() handles inverted/noninverted logic
  if (buttons[b].pressedBefore != isButtonPressed(b)) {
    DEBUG_PRINTF_P(PSTR("Switch: State changed %u\n"), b);
    buttons[b].pressedTime = millis();
    buttons[b].pressedBefore = !buttons[b].pressedBefore; // toggle pressed state
  }

  if (buttons[b].longPressed == buttons[b].pressedBefore) return;

  if (millis() - buttons[b].pressedTime > WLED_DEBOUNCE_THRESHOLD) { //fire edge event only after 50ms without change (debounce)
    DEBUG_PRINTF_P(PSTR("Switch: Activating  %u\n"), b);
    if (!buttons[b].pressedBefore) { // on -> off
      DEBUG_PRINTF_P(PSTR("Switch: On -> Off (%u)\n"), b);
      if (buttons[b].macroButton) applyPreset(buttons[b].macroButton, CALL_MODE_BUTTON_PRESET);
      else { //turn on
        if (!bri) {toggleOnOff(); stateUpdated(CALL_MODE_BUTTON);}
      }
    } else {  // off -> on
      DEBUG_PRINTF_P(PSTR("Switch: Off -> On (%u)\n"), b);
      if (buttons[b].macroLongPress) applyPreset(buttons[b].macroLongPress, CALL_MODE_BUTTON_PRESET);
      else { //turn off
        if (bri) {toggleOnOff(); stateUpdated(CALL_MODE_BUTTON);}
      }
    }

#ifndef WLED_DISABLE_MQTT
    // publish MQTT message
    if (buttonPublishMqtt && WLED_MQTT_CONNECTED) {
      char subuf[MQTT_MAX_TOPIC_LEN + 32];
      if (buttons[b].type == BTN_TYPE_PIR_SENSOR) sprintf_P(subuf, PSTR("%s/motion/%d"), mqttDeviceTopic, (int)b);
      else sprintf_P(subuf, _mqtt_topic_button, mqttDeviceTopic, (int)b);
      mqtt->publish(subuf, 0, false, !buttons[b].pressedBefore ? "off" : "on");
    }
#endif

    buttons[b].longPressed = buttons[b].pressedBefore; //save the last "long term" switch state
  }
}

#define ANALOG_BTN_READ_CYCLE 250   // min time between two analog reading cycles
#define STRIP_WAIT_TIME 6           // max wait time in case of strip.isUpdating()
#define POT_SMOOTHING 0.25f         // smoothing factor for raw potentiometer readings
#define POT_SENSITIVITY 4           // changes below this amount are noise (POT scratching, or ADC noise)

void handleAnalog(uint8_t b)
{
  static uint8_t oldRead[WLED_MAX_BUTTONS] = {0};
  static float filteredReading[WLED_MAX_BUTTONS] = {0.0f};
  unsigned rawReading;    // raw value from analogRead, scaled to 12bit

  DEBUG_PRINTF_P(PSTR("Analog: Reading button %u\n"), b);

  #ifdef ESP8266
  rawReading = analogRead(A0) << 2;   // convert 10bit read to 12bit
  #else
  if ((buttons[b].pin < 0) /*|| (digitalPinToAnalogChannel(buttons[b].pin) < 0)*/) return; // pin must support analog ADC - newer esp32 frameworks throw lots of warnings otherwise
  rawReading = analogRead(buttons[b].pin); // collect at full 12bit resolution
  #endif
  yield();                            // keep WiFi task running - analog read may take several millis on ESP8266

  filteredReading[b] += POT_SMOOTHING * ((float(rawReading) / 16.0f) - filteredReading[b]); // filter raw input, and scale to [0..255]
  unsigned aRead = max(min(int(filteredReading[b]), 255), 0);                               // squash into 8bit
  if (aRead <= POT_SENSITIVITY) aRead = 0;                                                   // make sure that 0 and 255 are used
  if (aRead >= 255-POT_SENSITIVITY) aRead = 255;

  if (buttons[b].type == BTN_TYPE_ANALOG_INVERTED) aRead = 255 - aRead;

  // remove noise & reduce frequency of UI updates
  if (abs(int(aRead) - int(oldRead[b])) <= POT_SENSITIVITY) return;  // no significant change in reading

  DEBUG_PRINTF_P(PSTR("Analog: Raw = %u\n"), rawReading);
  DEBUG_PRINTF_P(PSTR(" Filtered = %u\n"), aRead);

  oldRead[b] = aRead;

  // if no macro for "short press" and "long press" is defined use brightness control
  if (!buttons[b].macroButton && !buttons[b].macroLongPress) {
    DEBUG_PRINTF_P(PSTR("Analog: Action = %u\n"), buttons[b].macroDoublePress);
    // if "double press" macro defines which option to change
    if (buttons[b].macroDoublePress >= 250) {
      // global brightness
      if (aRead == 0) {
        briLast = bri;
        bri = 0;
      } else {
        if (bri == 0) strip.restartRuntime();
        bri = aRead;
      }
    } else if (buttons[b].macroDoublePress == 249) {
      // effect speed
      effectSpeed = aRead;
    } else if (buttons[b].macroDoublePress == 248) {
      // effect intensity
      effectIntensity = aRead;
    } else if (buttons[b].macroDoublePress == 247) {
      // selected palette
      effectPalette = map(aRead, 0, 252, 0, getPaletteCount()-1);
      effectPalette = constrain(effectPalette, 0, getPaletteCount()-1);  // map is allowed to "overshoot", so we need to contrain the result
    } else if (buttons[b].macroDoublePress == 200) {
      // primary color, hue, full saturation
      colorHStoRGB(aRead*256, 255, colPri);
    } else {
      // otherwise use "double press" for segment selection
      Segment& seg = strip.getSegment(buttons[b].macroDoublePress);
      if (aRead == 0) {
        seg.on = false; // do not use transition
      } else {
        seg.opacity = aRead; // set brightness (opacity) of segment
        seg.on = true;
      }
      // this will notify clients of update (websockets,mqtt,etc)
      updateInterfaces(CALL_MODE_BUTTON);
    }
  } else {
    DEBUG_PRINTLN(F("Analog: No action"));
  }
  colorUpdated(CALL_MODE_BUTTON);
}

void handleButton()
{
  static unsigned long lastAnalogRead = 0UL;
  static unsigned long lastRun        = 0UL;
  // button 0 triple-press tracking
  static uint8_t  b0PressCount  = 0;
  static uint32_t b0LastRelease = 0;
  // button 0 hold-feedback flags (reset on release)
  static bool b0Feedback15s = false;
  static bool b0Feedback30s = false;
  unsigned long now = millis();

  if (strip.isUpdating() && (now - lastRun < ANALOG_BTN_READ_CYCLE+1)) return; // don't interfere with strip update (unless strip is updating continuously, e.g. very long strips)
  lastRun = now;

  for (unsigned b = 0; b < buttons.size(); b++) {
    #ifdef ESP8266
    if ((buttons[b].pin < 0 && !(buttons[b].type == BTN_TYPE_ANALOG || buttons[b].type == BTN_TYPE_ANALOG_INVERTED)) || buttons[b].type == BTN_TYPE_NONE) continue;
    #else
    if (buttons[b].pin < 0 || buttons[b].type == BTN_TYPE_NONE) continue;
    #endif

    if (UsermodManager::handleButton(b)) continue; // did usermod handle buttons

    if (buttons[b].type == BTN_TYPE_ANALOG || buttons[b].type == BTN_TYPE_ANALOG_INVERTED) { // button is not a button but a potentiometer
      if (now - lastAnalogRead > ANALOG_BTN_READ_CYCLE) handleAnalog(b);
      continue;
    }

    // button is not momentary, but switch. This is only suitable on pins whose on-boot state does not matter (NOT gpio0)
    if (buttons[b].type == BTN_TYPE_SWITCH || buttons[b].type == BTN_TYPE_TOUCH_SWITCH || buttons[b].type == BTN_TYPE_PIR_SENSOR) {
      handleSwitch(b);
      continue;
    }

    // momentary button logic
    if (isButtonPressed(b)) { // pressed

      // if all macros are the same, fire action immediately on rising edge
      if (buttons[b].macroButton && buttons[b].macroButton == buttons[b].macroLongPress && buttons[b].macroButton == buttons[b].macroDoublePress) {
        if (!buttons[b].pressedBefore) shortPressAction(b);
        buttons[b].pressedBefore = true;
        buttons[b].pressedTime = now; // continually update (for debouncing to work in release handler)
        continue;
      }

      if (!buttons[b].pressedBefore) buttons[b].pressedTime = now;
      buttons[b].pressedBefore = true;

      if (b == 0) {
        // button 0: power toggles immediately at 600ms; LED feedback given at reset thresholds
        unsigned long held = now - buttons[0].pressedTime;
        if (held >= WLED_LONG_DOWNLOAD_MODE && !b0Feedback30s) {
          flashLEDs(255, 0, 0, 2);  // red x2 — download mode coming on release
          b0Feedback30s = true;
        } else if (held >= WLED_LONG_FACTORY_RESET && !b0Feedback15s) {
          flashLEDs(0, 0, 255, 2);  // blue x2 — factory reset coming on release
          b0Feedback15s = true;
        }
        if (held >= WLED_LONG_PRESS && !buttons[b].longPressed) {
          toggleOnOff();
          stateUpdated(CALL_MODE_BUTTON);
          b0PressCount = 0;
          buttons[b].longPressed = true;
        }
      } else {
        // standard long press handling for buttons 1+
        if (now - buttons[b].pressedTime > WLED_LONG_PRESS) {
          if (!buttons[b].longPressed) {
            buttonBriDirection = !buttonBriDirection; //toggle brightness direction on long press
            longPressAction(b);
          } else if (b) { //repeatable action (~5 times per s) on button > 0
            longPressAction(b);
            buttons[b].pressedTime = now - WLED_LONG_REPEATED_ACTION; //200ms
          }
          buttons[b].longPressed = true;
        }
      }

    } else if (buttons[b].pressedBefore) { // released
      long dur = now - buttons[b].pressedTime;

      // released after rising-edge short press action
      if (buttons[b].macroButton && buttons[b].macroButton == buttons[b].macroLongPress && buttons[b].macroButton == buttons[b].macroDoublePress) {
        if (dur > WLED_DEBOUNCE_THRESHOLD) buttons[b].pressedBefore = false; // debounce, blocks button for 50 ms once it has been released
        continue;
      }

      if (dur < WLED_DEBOUNCE_THRESHOLD) { buttons[b].pressedBefore = false; continue; } // too short "press", debounce

      if (b == 0) {
        // button 0: determine action by hold duration
        b0Feedback15s = false;
        b0Feedback30s = false;
        if (dur >= WLED_LONG_DOWNLOAD_MODE) {
          // 30 s hold → download mode; keep GPIO0 held through the reboot for the bootloader to catch it
          doReboot = true;
        } else if (dur >= WLED_LONG_FACTORY_RESET) {
          // 15 s hold → factory reset, then restore default preset so boot colour survives
          WLED_FS.format();
          {
            File f = WLED_FS.open("/presets.json", "w");
            if (f) { f.print("{\"1\":{\"n\":\"Boot Color\",\"on\":true,\"bri\":128,\"seg\":[{\"col\":[[8,255,0]]}]}}"); f.close(); }
            f = WLED_FS.open("/cfg.json", "w");
            if (f) { f.print("{\"def\":{\"ps\":1,\"on\":true,\"bri\":128}}"); f.close(); }
          }
          doReboot = true;
        } else if (!buttons[b].longPressed) {
          // short press → accumulate for triple-press detection
          b0PressCount++;
          b0LastRelease = now;
        }
      } else {
        bool doublePress = buttons[b].waitTime; //did we have a short press before?
        buttons[b].waitTime = 0;
        if (!buttons[b].longPressed) { //short press
          //NOTE: this interferes with double click handling in usermods so usermod needs to implement full button handling
          if (b != 1 && !buttons[b].macroDoublePress) { //don't wait for double press on buttons without a default action if no double press macro set
            shortPressAction(b);
          } else { //double press if less than 350 ms between current press and previous short press release (buttonWaitTime!=0)
            if (doublePress) doublePressAction(b);
            else buttons[b].waitTime = now;
          }
        }
      }

      buttons[b].pressedBefore = false;
      buttons[b].longPressed   = false;
    }

    // double-press timeout for buttons other than 0
    if (b != 0 && buttons[b].waitTime && now - buttons[b].waitTime > WLED_DOUBLE_PRESS && !buttons[b].pressedBefore) {
      buttons[b].waitTime = 0;
      shortPressAction(b);
    }
  }

  // button 0 triple-press window: fire action once WLED_TRIPLE_PRESS_WINDOW ms has passed with no new press
  if (b0PressCount > 0 && buttons.size() > 0 && !buttons[0].pressedBefore && now - b0LastRelease > WLED_TRIPLE_PRESS_WINDOW) {
    if (b0PressCount >= 3) {
      flashLEDs(0, 255, 0, 2);          // green x2 — AP mode
      WLED::instance().initAP(true);
    } else if (b0PressCount == 1) {
      setRandomColor(colPri);            // single press — change colour
      colorUpdated(CALL_MODE_BUTTON);
    }
    // 2 presses: intentionally no action
    b0PressCount = 0;
  }

  if (now - lastAnalogRead > ANALOG_BTN_READ_CYCLE) lastAnalogRead = now;
}

// handleIO() happens *after* handleTransitions() (see wled.cpp) which may change bri/briT but *before* strip.service()
// where actual LED painting occurrs
// this is important for relay control and in the event of turning off on-board LED
void handleIO()
{
  handleButton();

  // if we want to control on-board LED (ESP8266) or relay we have to do it here as the final show() may not happen until
  // next loop() cycle
  handleOnOff();
}

void handleOnOff(bool forceOff)
{
  if (strip.getBrightness() && !forceOff) {
    lastOnTime = millis();
    if (offMode) {
      BusManager::on();
      if (rlyPin>=0) {
        // note: pinMode is set in first call to handleOnOff(true) in beginStrip()
        digitalWrite(rlyPin, rlyMde); // set to on state
        delay(RELAY_DELAY); // let power stabilize before sending LED data (#346 #812 #3581 #3955)
      }
      offMode = false;
    }
  } else if ((millis() - lastOnTime > 600 && !strip.needsUpdate()) || forceOff) {
    // for turning LED or relay off we need to wait until strip no longer needs updates (strip.trigger())
    if (!offMode) {
      BusManager::off();
      if (rlyPin>=0) {
        digitalWrite(rlyPin, !rlyMde); // set output before disabling high-z state to avoid output glitches
        pinMode(rlyPin, rlyOpenDrain ? OUTPUT_OPEN_DRAIN : OUTPUT);
      }
      offMode = true;
    }
  }
}

void IRAM_ATTR touchButtonISR()
{
  // used for ESP32 S2 and S3: nothing to do, ISR is just used to update registers of HAL driver
}
