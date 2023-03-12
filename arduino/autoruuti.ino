#include <LiquidCrystal.h>
#include <LCDKeypad.h>
#include <SoftwareSerial.h>
#include <Stepper.h>
#include <Regexp.h>
#include <EEPROM.h>

//RX: D3
//TX: D2
SoftwareSerial scale(3, 2);

LCDKeypad lcd;

int incomingByte = 0; // for incoming serial data

const int INPUT_BUFFER_SIZE = 64;
char inputBuffer[INPUT_BUFFER_SIZE];

// Defines the number of steps per rotation
const int measureTimeout = 15;

MatchState ms;

const int SCALE_ERROR_TIMEOUT = -1001;
const int SCALE_ERROR_REPLY_FORMAT = -1000;

const int LONG_BUTTON_PRESS_MS = 800;
unsigned long lastButtonDownEventMs = 0;  //in millis(). if > 0, a button down event was occured.
uint8_t lastButtonDownType;               //KEYPAD_DOWN, KEYPAD_UP etc
bool lastButtonLongPressEmitted = false;  //This is for tracking if the long press event was already emitted (so we don't do it again)
const int timerValue = 0;

const int SETTINGS_HEADER1_I = 0;
const int SETTINGS_HEADER1_V = 0xF0;
const int SETTINGS_HEADER2_I = 1;
const int SETTINGS_HEADER2_V = 0x01;
const int SETTINGS_VERSION_I = 2;
const int SETTINGS_VERSION_V = 1;
const int SETTINGS_STRUCT_I = 3;
const int STEPS_PER_ROTATION = 2038;

// Pins entered in sequence IN1-IN3-IN2-IN4 for proper step sequence
Stepper stepper(STEPS_PER_ROTATION, A5, A4, A3, A2);

uint8_t mode = 0;   //0 = operation, 1 = settings menu
#define MODE_OPERATION 0
#define MODE_SETTINGS 1

bool paused = true;
bool saveRequired = false;

#define PHASE_WAIT 0
#define PHASE_1 1
#define PHASE_2 2
#define PHASE_3 3
#define PHASE_COMPLETE 4
#define PHASE_OVERWEIGHT 5

uint8_t phase = 0;        //what phase are we in (1-3), 0=waiting startWeight, 4=complete, 5=overweight
double weight = 0.0;  //last measured weight
bool weightError = true;

#define MENUITEM_COUNT 12
uint8_t menuitem = 0;
#define MENUITEM_STARTWEIGHT 0
#define MENUITEM_TARGETWEIGHT 1
#define MENUITEM_PHASE1SPEED 2
#define MENUITEM_PHASE2SPEED 3
#define MENUITEM_PHASE3SPEED 4
#define MENUITEM_STABILIZETIMEMS 5
#define MENUITEM_PHASE1WEIGHT 6
#define MENUITEM_PHASE2WEIGHT 7
#define MENUITEM_OVERWEIGHTWARNING 8
#define MENUITEM_PHASE1ROTATIONS 9
#define MENUITEM_PHASE2ROTATIONS 10
#define MENUITEM_PHASE3ROTATIONS 11
const char *menuitem_names[] = {
  "Aloituspaino g",
  "Tavoitepaino g",
  "Vaihe 1 nopeus ask",
  "Vaihe 2 nopeus ask",
  "Vaihe 3 nopeus ask",
  "Stabilointiaika ms",
  "Vaihe 1 painoero g",
  "Vaihe 2 painoero g",
  "Ylipainovaroitus g",
  "Vaihe 1 kierrokset",
  "Vaihe 2 kierrokset",
  "Vaihe 3 kierrokset"
};
const double menuitem_modifiers_short[] = {
  0.1,
  0.001,
  1.0,
  1.0,
  1.0,
  100.0,
  0.01,
  0.002,
  0.001,
  0.05,
  0.05,
  0.05
};
const double menuitem_modifiers_long[] = {
  1.0,
  0.1,
  5.0,
  5.0,
  5.0,
  500.0,
  0.1,
  0.1,
  0.01,
  0.5,
  0.5,
  0.5
};

unsigned long settingsChangedTimeMs = 0;


// STORED SETTINGS
struct settings_t {
  double startWeight;             //e.g. 2.5    when to start addition
  double targetWeight;            //e.g. 3.3    how much we want the weight to be
  int phase1Speed; //50           //e.g. 50     phase 1 addition speed
  int phase2Speed; //20           //e.g. 20     phase 2 addition speed
  int phase3Speed; //5            //e.g. 5      phase 3 addition speed
  int stabilizeTimeMs;            //e.g. 5000   scale stabilization time in ms
  double phase1Weight;            //e.g. 0.1    when we're missing this much weight, go to phase 2
  double phase2Weight;            //e.g. 0.05   when we're missing this much weight, go to phase 3
  double overWeightWarning;       //e.g. 0.02   if we go this much over target, display a warning
  double phase1Rotations;         //e.g. 0.5
  double phase2Rotations;         //e.g. 0.25
  double phase3Rotations;         //e.g. 0.05   when doing phase 3 measurement, how much to add at once (rotations)
};

struct settings_t settings;

void loadDefaults() {
  Serial.println("load hardcoded default settings");

  //load default settings
  memset(&settings, 0, sizeof(settings_t));
  settings.startWeight = 2.0;
  settings.targetWeight = 3.3;
  settings.phase1Speed = 10;
  settings.phase2Speed = 10;
  settings.phase3Speed = 10;
  settings.stabilizeTimeMs = 5000;
  settings.phase1Weight = 0.1;
  settings.phase2Weight = 0.05;
  settings.overWeightWarning = 0.01;
  settings.phase1Rotations = 0.5;
  settings.phase2Rotations = 0.25;
  settings.phase3Rotations = 0.05;
}

void loadSettings() {
  Serial.println("load settings from EEPROM");

  //load settings from EEPROM, or if all fails, load default settings
  //#0, #1: expect static header, 0xF001
  uint8_t header1 = EEPROM.read(SETTINGS_HEADER1_I);
  uint8_t header2 = EEPROM.read(SETTINGS_HEADER2_I);
  if (header1 != SETTINGS_HEADER1_V || header2 != SETTINGS_HEADER2_V) {
    Serial.println("Header mismatch, load defaults");
    loadDefaults();
    return;
  }

  //Settings version number. Not really used right now but for future.
  uint8_t version = EEPROM.read(SETTINGS_VERSION_I);
  if (version != SETTINGS_VERSION_V) {
    Serial.println("Version mismatch, load defaults");
    loadDefaults();
    return;
  }

  //looks good, load defaults
  Serial.println("Load settings");

  EEPROM.get(SETTINGS_STRUCT_I, settings);
}

void saveSettings() {
  Serial.println("save settings to EEPROM");
  EEPROM.write(SETTINGS_HEADER1_I, SETTINGS_HEADER1_V);
  EEPROM.write(SETTINGS_HEADER2_I, SETTINGS_HEADER2_V);
  EEPROM.write(SETTINGS_VERSION_I, SETTINGS_VERSION_V);
  EEPROM.put(SETTINGS_STRUCT_I, settings);

  settingsChangedTimeMs = 0;
}

void settingsChanged() {
  settingsChangedTimeMs = millis();
}

void print_error(String error1, String error2)
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(error1);
  lcd.setCursor(0, 1);
  lcd.print(error2);
  delay(5000);
}

int readScaleMeasurement()
{
  memset(inputBuffer, 0, INPUT_BUFFER_SIZE);
  int read = scale.readBytesUntil('\n', inputBuffer, INPUT_BUFFER_SIZE - 1);
  //Serial.print("read ");
  //Serial.print(read);
  //Serial.print("\r\n");
  //for (int k=0; k<read; k++) {
  //  Serial.print(int(inputBuffer[k]));
  //}
  //Serial.print(inputBuffer);
  //Serial.print("\r\n");

  if (read == 0) {
    weightError = true;
    return SCALE_ERROR_TIMEOUT;
  }
  for (int k=0; k<read; k++) {
    if (inputBuffer[k] == '\r' || inputBuffer[k] == '\n') {
      inputBuffer[k] = 0;
    }
  }
  ms.Target(inputBuffer);
  char match = ms.Match("%s*(-*%s*%d+.%d+) g", 0);
  if (match != REGEXP_MATCHED) {
    //print_error("ERROR in reply:", inputBuffer);
    weightError = true;
    return SCALE_ERROR_REPLY_FORMAT;
  }

  //Unsafe code. GetCapture does not support max len?
  char parsedValue[50];
  ms.GetCapture(parsedValue, 0);
  String str(parsedValue);
  str.replace(" ", "");
  //Serial.print("str ");
  //Serial.print(str);
  //Serial.print("\r\n");

  double value = atof(str.c_str());
  Serial.print("Read weight: ");
  Serial.print(value, 3);
  Serial.print("\r\n");
  weight = value;
  weightError = false;
  return 0;
}

int measure(bool stable)
{
  for (int k=0; k<measureTimeout; k++) {
    //discard all data
    scale.readBytes(inputBuffer, INPUT_BUFFER_SIZE);
    lcd.setCursor(15, 1);
    lcd.print("P");

    if (stable)  {
      scale.write("S\r\n");
      //Serial.print("write S\r\n");
    } else {
      //scale does not support SI? protocol looks different from the PDF...
      scale.write("SI\r\n");
      //Serial.print("write SI\r\n");
    }
    int ret = readScaleMeasurement();
    if (ret == SCALE_ERROR_TIMEOUT) {
      delay(500);
      continue;
    } else if (ret == SCALE_ERROR_REPLY_FORMAT) {
      delay(500);
      continue;
    }
    lcd.setCursor(15, 1);
    lcd.print(" ");
    return 0;
  }
  lcd.setCursor(15, 1);
  lcd.print("E");
  return SCALE_ERROR_TIMEOUT;
}

void pause() {
  paused = true;
}

void unpause() {
  paused = false;
}

void determinePhase(bool recursive) {
  bool stableRead = true;
  if (phase == PHASE_1) {
    stableRead = false;
  }
  double beforeWeight = weight;
  int ret = measure(stableRead);
  if (ret) {
    return;
  }

  int lastPhase = phase;

  // phase 1
  if (weight > (settings.targetWeight + settings.overWeightWarning)) {
    pause();
    phase = PHASE_OVERWEIGHT;
  } else if (weight >= (settings.targetWeight - 0.001)) {
    phase = PHASE_COMPLETE;
  } else if (weight > settings.targetWeight - settings.phase2Weight) {
    phase = PHASE_3;
  } else if (weight > settings.targetWeight - settings.phase1Weight) {
    phase = PHASE_2;
  } else if (weight > settings.startWeight) {
    phase = PHASE_1;
  } else {
    phase = PHASE_WAIT;
  }

  if (!stableRead && phase != PHASE_1) {
    if (recursive) {
      //weight changed again, give up and retry later
      return;
    }
    //phase was changed from 1, run againt to do a stable reading
    determinePhase(true);
  }

  if (beforeWeight != weight || lastPhase != phase) {
    drawOperationStatus();
  }
}

void runPhase(
  ) {
  //Addition will be done only if:
  // * Measured (stabilized) weight >= minWeight and <maxWeight
  
  //Phase 1
  //- add using phase 1 speed until we measure targetWeight - phase1Weight (fast read)
  //- start phase 2
  //Phase 2
  // - add using phase 2 speed until we measure targetWeight - phase2Weight (fast read)
  // - start phase 3
  //Phase 3
  // - add using phase 3 rotations
  // - stabilize
  // - read stabilized weight
  // - if less than target, do again

  Serial.print("runPhase w=");
  Serial.print(weight, 3);
  Serial.print(" phase=");
  Serial.print(phase);
  Serial.println("");

  if (phase == 1) {
    performAddition(settings.startWeight, settings.phase1Speed, settings.targetWeight - settings.phase1Weight, settings.phase1Rotations, false, 0, settings.targetWeight + settings.overWeightWarning);
  } else if (phase == 2) {
    performAddition(settings.startWeight, settings.phase2Speed, settings.targetWeight - settings.phase2Weight, settings.phase2Rotations, false, 0, settings.targetWeight + settings.overWeightWarning);
  } else if (phase == 3) {
    performAddition(settings.startWeight, settings.phase2Speed, settings.targetWeight, settings.phase3Rotations, true, settings.stabilizeTimeMs, settings.targetWeight + settings.overWeightWarning);
  }
}

void printWeight(double weight, bool stable, double target)
{
  lcd.setCursor(0, 1);
  lcd.print(weight, 3);
  lcd.print(" / ");
  lcd.print(target, 3);
  lcd.print(" g");
  lcd.print("        ");
}

void printPhase(int phase, double phaseTarget, double finalTarget) {
  lcd.setCursor(0, 0);
  lcd.print("Target ");
  lcd.print(finalTarget);
  lcd.print(" Pha");
  lcd.print(phase);
  lcd.print("      ");
}

void performAddition(double startWeight, int speed, double target, double rotations, bool stableMeasure, int stabilizeTimeMs, double overWeightLimit) {
  if (stabilizeTimeMs) {
    delay(stabilizeTimeMs);
    while (measure(false)) {
    }
    draw();
  }
  if (weight < startWeight) {
    return;
  }
  if (weight >= target) {
    return;
  }
  lcd.setCursor(15, 1);
  lcd.print("M");
  stepper.setSpeed(speed);
  stepper.step(rotations * STEPS_PER_ROTATION);
  lcd.setCursor(15, 1);
  lcd.print(" ");
}

void setup() {
  loadSettings();

  lcd.begin(20, 2);
  lcd.clear();
  lcd.backlight();
  lcd.print("Autoruuti v1.0");

  Serial.begin(9600);
  Serial.print("booting\r\n");
  Serial.println("A");
  Serial.println(3.301 > 3.300);
  Serial.println("B");
  Serial.println(3.302 > 3.300);
  Serial.println("C");
  Serial.println(3.308 > 3.300);
  Serial.println("D");
  Serial.println(3.300 >= 3.300);
  Serial.println("E");
  Serial.println(3.301 >= 3.300);
  Serial.println("F");
  Serial.println(3.299 >= 3.300);

  scale.setTimeout(500);
  scale.begin(9600);
  lcd.setCursor(0, 1);
  lcd.print("Moottoritesti...");
  stepper.setSpeed(10);
  stepper.step(STEPS_PER_ROTATION * 0.1);
  delay(500);
  do {
    lcd.setCursor(0, 1);
    lcd.print("Puntaritesti...  ");
    while (measure(true)) {
    }
    delay(1000);
    lcd.clear();
  } while(weight <= -1000);

  //set up a timer for button checking
  noInterrupts();
  TCCR1A = 0;
  TCCR1B = 0;
  TCNT1 = timerValue;         // preload timer
  TCCR1B |= 1<<CS01;          // 8 prescaler
  TIMSK1 |= 1<<TOIE1;         // enable timer overflow interrupt ISR
  interrupts();
  determinePhase(false);
  drawOperationStatus();
}

void enterSettings() {
  mode = MODE_SETTINGS;
  drawMenu();
}

void changeSetting(bool down, bool longPress) {
  double modifier;
  if (longPress) {
    modifier = menuitem_modifiers_long[menuitem];
  } else {
    modifier = menuitem_modifiers_short[menuitem];
  }
  if (down) {
    modifier = -modifier;
  }

  switch (menuitem) {
    case MENUITEM_STARTWEIGHT:
      settings.startWeight += double(modifier);
      break;
    case MENUITEM_TARGETWEIGHT:
      settings.targetWeight += double(modifier);
      break;
    case MENUITEM_PHASE1SPEED:
      settings.phase1Speed += int(modifier);
      break;
    case MENUITEM_PHASE2SPEED:
      settings.phase2Speed += int(modifier);
      break;
    case MENUITEM_PHASE3SPEED:
      settings.phase3Speed += int(modifier);
      break;
    case MENUITEM_STABILIZETIMEMS:
      settings.stabilizeTimeMs += int(modifier);
      break;
    case MENUITEM_PHASE1WEIGHT:
      settings.phase1Weight += double(modifier);
      break;
    case MENUITEM_PHASE2WEIGHT:
      settings.phase2Weight += double(modifier);
      break;
    case MENUITEM_OVERWEIGHTWARNING:
      settings.overWeightWarning += double(modifier);
      break;
    case MENUITEM_PHASE1ROTATIONS:
      settings.phase1Rotations += double(modifier);
      break;
    case MENUITEM_PHASE2ROTATIONS:
      settings.phase2Rotations += double(modifier);
      break;
    case MENUITEM_PHASE3ROTATIONS:
      settings.phase3Rotations += double(modifier);
      break;
  }

  settingsChanged();
}

void handleSettingsButtonEvent(uint8_t button, bool longPress) {
  if (button == KEYPAD_RIGHT) {
    menuitem++;
    if (menuitem >= MENUITEM_COUNT) {
      menuitem = 0;
    }
  } else if (button == KEYPAD_LEFT) {
    if (menuitem == 0) {
      menuitem = MENUITEM_COUNT - 1;
    } else {
      menuitem--;
    }
  } else if (button == KEYPAD_UP) {
    changeSetting(false, longPress);
  } else if (button == KEYPAD_DOWN) {
    changeSetting(true, longPress);
  } else if (button == KEYPAD_SELECT) {
    if (settingsChangedTimeMs) {
      saveSettings();
    }
    mode = MODE_OPERATION;
  }
}

void buttonEvent(uint8_t button, bool longPress)
{
  Serial.print("buttonEvent: ");
  Serial.print(button);
  Serial.print(" long=");
  Serial.print(longPress);
  Serial.print("\r\n");

  if (mode == MODE_SETTINGS) {
    handleSettingsButtonEvent(button, longPress);
    draw();
    return;
  }

  if (button == KEYPAD_SELECT && !longPress) {
    if (paused) {
      unpause();
    } else {
      pause();
    }
  } else if (button == KEYPAD_SELECT && longPress) {
    enterSettings();
    settingsChanged();
  } else if (button == KEYPAD_LEFT) {
    if (longPress) {
      settings.targetWeight -= 0.5;
    } else {
      settings.targetWeight -= 0.001;
    }
    settingsChanged();
  } else if (button == KEYPAD_RIGHT) {
    if (longPress) {
      settings.targetWeight += 0.5;
    } else {
      settings.targetWeight += 0.001;
    }
    settingsChanged();
  } else if (button == KEYPAD_UP) {
    if (longPress) {
      settings.targetWeight += menuitem_modifiers_long[MENUITEM_TARGETWEIGHT];
    } else {
      settings.targetWeight += menuitem_modifiers_short[MENUITEM_TARGETWEIGHT];
    }
    settingsChanged();
  } else if (button == KEYPAD_DOWN) {
    if (longPress) {
      settings.targetWeight -= menuitem_modifiers_long[MENUITEM_TARGETWEIGHT];
    } else {
      settings.targetWeight -= menuitem_modifiers_short[MENUITEM_TARGETWEIGHT];
    }
    settingsChanged();
  }
  draw();
}

void checkButtons() {
  uint8_t button = lcd.buttonBlocking(20, 50);
  if (button == KEYPAD_BLOCKED) {
    //run again later
    return;
  }
  if (lastButtonDownEventMs) {
    //last button down event was set
    if (button == KEYPAD_NONE) {
      if (lastButtonLongPressEmitted) {
        lastButtonLongPressEmitted = false;
        lastButtonDownEventMs = 0;
        return;
      }
      //Serial.print("button was released, emit buttonEvent short press\r\n");
      buttonEvent(lastButtonDownType, false);
      lastButtonDownEventMs = 0;
      return;
    } else if (button != lastButtonDownType) {
      //Serial.print("button was changed to something else, do nothing\r\n");
      lastButtonDownEventMs = 0;
      return;
    } else if (!lastButtonLongPressEmitted) {
      //button is still being held on and no lastButtonLongPressEmitted set
      unsigned long now = millis();
      if (now - lastButtonDownEventMs > LONG_BUTTON_PRESS_MS) {
        //long button press event
        lastButtonLongPressEmitted = true;
        buttonEvent(button, true);
      } else {
        //wait until button is released or LONG_BUTTON_PRESS_MS reached
      }
    } else {
      //button is still being held on but we don't care
    }
  } else {
    if (button == KEYPAD_NONE) {
      //no down event was before and no button pressed, do nothing
    } else {
      if (lastButtonLongPressEmitted) {
        return;
      }
      //Serial.print("button down event occured\r\n");
      lastButtonDownEventMs = millis();
      lastButtonDownType = button;
    }
  }
}

ISR(TIMER1_OVF_vect)
{
  TCNT1 = timerValue;
  checkButtons();
}

void drawMenu() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(menuitem_names[menuitem]);

  lcd.setCursor(0, 1);

  switch (menuitem) {
    case MENUITEM_STARTWEIGHT:
      lcd.print(settings.startWeight, 3);
      break;
    case MENUITEM_TARGETWEIGHT:
      lcd.print(settings.targetWeight, 3);
      break;
    case MENUITEM_PHASE1SPEED:
      lcd.print(settings.phase1Speed);
      break;
    case MENUITEM_PHASE2SPEED:
      lcd.print(settings.phase2Speed);
      break;
    case MENUITEM_PHASE3SPEED:
      lcd.print(settings.phase3Speed);
      break;
    case MENUITEM_STABILIZETIMEMS:
      lcd.print(settings.stabilizeTimeMs);
      break;
    case MENUITEM_PHASE1WEIGHT:
      lcd.print(settings.phase1Weight, 3);
      break;
    case MENUITEM_PHASE2WEIGHT:
      lcd.print(settings.phase2Weight, 3);
      break;
    case MENUITEM_OVERWEIGHTWARNING:
      lcd.print(settings.overWeightWarning, 3);
      break;
    case MENUITEM_PHASE1ROTATIONS:
      lcd.print(settings.phase1Rotations);
      break;
    case MENUITEM_PHASE2ROTATIONS:
      lcd.print(settings.phase2Rotations);
      break;
    case MENUITEM_PHASE3ROTATIONS:
      lcd.print(settings.phase3Rotations);
      break;
  }

}

void drawOperationStatus() {
  lcd.clear();
  lcd.setCursor(0, 0);
  if (phase == PHASE_OVERWEIGHT) {
    lcd.print("Ylipaino!");
  } else if (paused) {
    lcd.print("Tauko");
  } else if (phase == PHASE_WAIT) {
    lcd.print("Odotetaan");
  } else if (phase == 1) {
    lcd.print("Vaihe 1");
  } else if (phase == 2) {
    lcd.print("Vaihe 2");
  } else if (phase == 3) {
    lcd.print("Vaihe 3");
  } else if (phase == PHASE_COMPLETE) {
    lcd.print("Valmis");
  }

  lcd.setCursor(11, 0);
  lcd.print(settings.targetWeight, 3);

  lcd.setCursor(0, 1);
  lcd.print(weight, 3);
  if (phase == 0) {
    lcd.print(" / ");
    lcd.print(settings.startWeight, 3);
  } else if (phase == 1) {
    lcd.print(" / ");
    lcd.print(settings.targetWeight - settings.phase1Weight, 3);
  } else if (phase == 2) {
    lcd.print(" / ");
    lcd.print(settings.targetWeight - settings.phase2Weight, 3);
  } else if (phase == 3) {
    lcd.print(" / ");
    lcd.print(settings.targetWeight, 3);
  }
}

void draw() {
  if (mode == MODE_OPERATION) {
    drawOperationStatus();
  } else {
    drawMenu();
  }
}

#define SETTINGS_AUTOSAVE_DELAY_MS 5000

void loop() {
  if (settingsChangedTimeMs) {
    unsigned long now = millis();
    if (now - settingsChangedTimeMs > SETTINGS_AUTOSAVE_DELAY_MS) {
      saveSettings();
    }
  }
  if (mode == MODE_OPERATION) {
    determinePhase(false);
    if (paused) {
      return;
    }
    if (!weightError) {
      runPhase();
    }
  } else if (mode == MODE_SETTINGS) {
    //drawMenu();
    //delay(100);
  }
}
