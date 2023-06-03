
/*
 *  Poly Synth for Arduino and Mozzi *
 *  @jidagraphy
 */

#include <MozziGuts.h>
#include <Oscil.h>
#include <mozzi_midi.h>
#include <ADSR.h>
#include <tables/saw512_int8.h>
#include <tables/triangle512_int8.h>
#include <tables/sin512_int8.h>
#include <tables/square_no_alias512_int8.h>
#include "Esp32SynchronizationContext.h"
#include <WiFi.h>

#define CONTROL_RATE 256

//set maximum number of polyphony
#define MAX_POLY 6
#define OSC_NUM_CELLS 512

//Envelope controllers
unsigned int attack = 22;
unsigned int decay = 500;
unsigned int sustain = 8000; // max sustain *length*, not sustain level
unsigned int release = 300;
byte attackLevel = 96; // aka vMax, etc
byte decayLevel = 64; // this is what would normally be called sustain

//Voices
struct Voice{
  Oscil<OSC_NUM_CELLS, AUDIO_RATE> osc;  // audio oscillator
  ADSR<CONTROL_RATE, AUDIO_RATE> env;  // envelope generator
  byte note = 0;
  byte velocity = 0;
};

Voice voices[MAX_POLY];

#define MODE_SWITCH 5

//optional midi monitor
#define LED LED_BUILTIN
#define NUM_KEYS 15 // two are dead and not used, Touch1 and Touch2 (index 1 and 2)
#define NUM_REAL_KEYS 13
#define NUM_BUILTIN_TOUCH 10
#define NOTE_VELOCITY 127

bool onSecondKey = false; // for two-key config sequences
int firstKey = 0;

#define BUILTIN_TOUCH_THRESHOLD 40
#define ADDL_TOUCH_THRESHOLD 20
const byte builtinTouchPins[NUM_BUILTIN_TOUCH] = {4, 0, 2, 15, 13, 12, 14, 27, 33, 32};
bool keyStates[NUM_KEYS] = {0};

#define NUM_ADDL_TOUCH 5
const byte addlTouchPins[NUM_ADDL_TOUCH][2] = {{23, 36}, {22, 39}, {21, 34}, {19, 35}, {18, 26}};
unsigned long addlTouchBaselines[NUM_ADDL_TOUCH] = {0}; // TODO: update w/ empirical values

unsigned long readCapacitiveExternal(byte sendPin, byte recievePin){
  // this is for non-builtin pins, so no touchRead here
  // instead we have a resistor between sendPin and recievePin
  // and we set sendPin and time how long it takes recievePin to go high

  digitalWrite(sendPin, HIGH);
  unsigned long start = micros();
  while(digitalRead(recievePin) == LOW && micros() - start < 1000);
  unsigned long duration = micros() - start;
  // now clean up
  digitalWrite(sendPin, LOW);
  return duration;
}

const byte CHROMATIC_SCALE_C1[NUM_KEYS] = {24, 0, 0, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36};
const byte CHROMATIC_SCALE_C2[NUM_KEYS] = {36, 0, 0, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48};
const byte CHROMATIC_SCALE_C3[NUM_KEYS] = {48, 0, 0, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60};
const byte CHROMATIC_SCALE_C4[NUM_KEYS] = {60, 0, 0, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72};
const byte CHROMATIC_SCALE_C5[NUM_KEYS] = {72, 0, 0, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84};
const byte CHROMATIC_SCALE_C6[NUM_KEYS] = {84, 0, 0, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96};
const byte CHROMATIC_SCALE_C7[NUM_KEYS] = {96, 0, 0, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108};

byte currentOctave = 4;

byte keyNotes[NUM_KEYS];

void setOctave(byte octave) {
  // if it's ugly, but it works, it's not ugly
  switch(octave){
    case 1:
      for (int i = 0; i < NUM_KEYS; i++) {
        keyNotes[i] = CHROMATIC_SCALE_C1[i];
      }
      break;
    case 2:
      for (int i = 0; i < NUM_KEYS; i++) {
        keyNotes[i] = CHROMATIC_SCALE_C2[i];
      }
      break;
    case 3:
      for (int i = 0; i < NUM_KEYS; i++) {
        keyNotes[i] = CHROMATIC_SCALE_C3[i];
      }
      break;
    case 4:
      for (int i = 0; i < NUM_KEYS; i++) {
        keyNotes[i] = CHROMATIC_SCALE_C4[i];
      }
      break;
    case 5:
      for (int i = 0; i < NUM_KEYS; i++) {
        keyNotes[i] = CHROMATIC_SCALE_C5[i];
      }
      break;
    case 6:
      for (int i = 0; i < NUM_KEYS; i++) {
        keyNotes[i] = CHROMATIC_SCALE_C6[i];
      }
      break;
    case 7:
      for (int i = 0; i < NUM_KEYS; i++) {
        keyNotes[i] = CHROMATIC_SCALE_C7[i];
      }
      break;
    default:
      for (int i = 0; i < NUM_KEYS; i++) {
        keyNotes[i] = CHROMATIC_SCALE_C4[i];
      }
      break;
  }
}

// use this to synchronize calls by executing functors on the target thread
Esp32SynchronizationContext g_mainSync;
bool needUpdateStates = false; // how this works: when this is false: no update, main thread not allowed to touch newKeyStates; other thread allowed to touch
// true: happens once other thread has set newKeyStates, main thread is allowed to touch newKeyStates, but hasn't yet; other thread not allowed to touch newKeyStates
// CRITICAL: this MUST ALWAYS BE SET FROM MAIN THREAD TO AVOID RACE CONDITIONS (use g_mainSync.Send() if on other thread)
bool newKeyStates[NUM_KEYS] = {0};
bool lastKeyStates[NUM_KEYS] = {0};
int rawTouchValues[NUM_KEYS] = {0}; // thread will write into this array on change

void HandleNoteOn(byte channel, byte note, byte velocity) {
  if (velocity > 0) {

    int activeVoice = 0;
    int voiceToSteal = 0;
    int lowestVelocity = 128;

    for (unsigned int i = 0 ; i < MAX_POLY; i++) {
      if(!voices[i].env.playing()){
        voices[i].env.noteOff();
        voices[i].osc.setFreq(mtof(float(note)));
        voices[i].env.noteOn();
        voices[i].note = note;
        voices[i].velocity = velocity;
        break;
      }else{
        activeVoice++;
        if(lowestVelocity >= voices[i].velocity){
          lowestVelocity = voices[i].velocity;
          voiceToSteal = i;
        }
      }
    }

    if(activeVoice == MAX_POLY){
        voices[voiceToSteal].env.noteOff();
        voices[voiceToSteal].osc.setFreq(mtof(float(note)));
        voices[voiceToSteal].env.noteOn();
        voices[voiceToSteal].note = note;
        voices[voiceToSteal].velocity = velocity;
    }
    digitalWrite(LED, HIGH);

  } else {
  }
}


void HandleNoteOff(byte channel, byte note, byte velocity) {
  byte handsOffChecker = 0;
  for (unsigned int i = 0; i < MAX_POLY; i++) {
    if (note == voices[i].note) {
      voices[i].env.noteOff();
      voices[i].note = 0;
      voices[i].velocity = 0;
    }
    handsOffChecker += voices[i].note;
  }

  if (handsOffChecker == 0) {
    digitalWrite(LED, LOW);
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_OFF); // disable wifi to leave power for audio and compute for touchUpdate

  pinMode(LED, OUTPUT);
  pinMode(MODE_SWITCH, INPUT_PULLUP);

  // set up the addl touch pins
  for(int i = 0; i < NUM_ADDL_TOUCH; i++){
    pinMode(addlTouchPins[i][0], OUTPUT);
    pinMode(addlTouchPins[i][1], INPUT);
  }

  calibrateTouches();

  setOctave(currentOctave);

  // send the scale to serial
  Serial.print("Scale: ");
  for(int i = 0; i < NUM_KEYS; i++){
    Serial.print(keyNotes[i]);
    Serial.print(" ");
  }
  Serial.println();

  for(unsigned int i = 0; i < MAX_POLY; i++){
    voices[i].env.setADLevels(attackLevel,decayLevel);
    voices[i].env.setTimes(attack,decay,sustain,release);
    voices[i].osc.setTable(SAW512_DATA);
  }
  if(!g_mainSync.begin()) {
    Serial.println("Error initializing synchronization context");
    while(true); // halt
  }
  // create a task on the first core to asynchronously update touch states
  xTaskCreatePinnedToCore(
    updateTouches,    // Function that should be called
    "Touch updater",   // Name of the task (for debugging)
    1000,            // Stack size (bytes)
    NULL,            // Parameter to pass
    1,               // Task priority
    NULL,             // Task handle
    0                // core
  );
  //aSin0.setFreq(440); // default frequency
  startMozzi(CONTROL_RATE);
}

void updateTouches(void* state){
  // RUNS ON OTHER CORE
  // *NEVER* touch any of the other thread's global variables here, because thread safety
  // To change the value of a global, use g_mainSync.post()
  while(true){
    if(needUpdateStates){
      delay(1); // feed watchdog
      continue; // don't do anything if the main thread is still processing the last update
    }
    bool needPost = false;
    // do builtin captouch
    for(int i = 0; i < NUM_BUILTIN_TOUCH; i++){
      if(i == 1 || i == 2){
        continue; // skip the second one, it's not broken out
      }
      rawTouchValues[i] = touchRead(builtinTouchPins[i]);
      //bool noteState = rawTouchValues[i] < BUILTIN_TOUCH_THRESHOLD;
      //if(noteState != newKeyStates[i]){
        //newKeyStates[i] = noteState;
        //needPost = true;
      //}
    }
    // addl captouch
    for(int i = 0; i < NUM_ADDL_TOUCH; i++){
      rawTouchValues[i + NUM_BUILTIN_TOUCH] = readCapacitiveExternal(addlTouchPins[i][0], addlTouchPins[i][1]);
    }
    if(true){
      // send the new states to the main thread
      // use send here to ensure we don't try to change needUpdateStates while the main thread is doing stuff with it
      g_mainSync.send([](void*state){
        // RUNS ON MAIN CORE
        needUpdateStates = true;
      });
    }
    delay(10); // feed watchdog
  }
}

#define NUM_RECAL_SENSES 5

void calibrateTouches(){
  // RUNS ON MAIN CORE
  // recalibrate the touch sensors
  needUpdateStates = true; // make sure the other thread doesn't try to update while we're recalibrating
  // flash the LED to indicate we're recalibrating
  for(int i = 0; i < 5; i++){
    digitalWrite(LED, HIGH);
    delay(50);
    digitalWrite(LED, LOW);
    delay(50);
  }
  // only do this to the addl ones
  for(int i = 0; i < NUM_ADDL_TOUCH; i++){
    // for each pin, sense a few times, then take the lowest value as new baseline
    unsigned long readings[NUM_RECAL_SENSES] = {0};
    for(int j = 0; j < NUM_RECAL_SENSES; j++){
      int val = readCapacitiveExternal(addlTouchPins[i][0], addlTouchPins[i][1]);
      readings[j] = val;
    }
    unsigned long lowest = readings[0];
    for(int j = 1; j < NUM_RECAL_SENSES; j++){
      if(readings[j] < lowest){
        lowest = readings[j];
      }
    }
    addlTouchBaselines[i] = lowest;
  }
  // hold the LED for a second to indicate we're done
  digitalWrite(LED, HIGH);
  delay(1000);
  digitalWrite(LED, LOW);
  needUpdateStates = false; // allow the other thread to update again
}

void updateControl(){
  if(!g_mainSync.update()) {
    Serial.println("Could not update synchronization context");
  }
  if(needUpdateStates){
    // the other thread detected a touch change
    // end old notes, start new ones, handle control inputs, etc
    // TODO: add a switch to change between keys acting as keys and keys acting as controls
    // for now assume we're in play mode
    // go through the new states and handle note on/off
    // see if any builtin touch values are below the threshold
    for(int i = 0; i < NUM_BUILTIN_TOUCH; i++){
      if(i == 1 || i == 2){
        continue; // skip the second one, it's not broken out
      }
      bool noteState = rawTouchValues[i] < BUILTIN_TOUCH_THRESHOLD;
      if(noteState != newKeyStates[i]){
        newKeyStates[i] = noteState;
      }
    }
    // now do the additional touch thresholds
    for(int i = 0; i < NUM_ADDL_TOUCH; i++){
      bool noteState = rawTouchValues[i + NUM_BUILTIN_TOUCH] > ADDL_TOUCH_THRESHOLD + addlTouchBaselines[i];
      if(noteState != newKeyStates[i + NUM_BUILTIN_TOUCH]){
        newKeyStates[i + NUM_BUILTIN_TOUCH] = noteState;
      }
    }
    for(int i = 0; i < NUM_KEYS; i++){
      if(newKeyStates[i] != keyStates[i] && lastKeyStates[i] == newKeyStates[i]){
        if(newKeyStates[i]){
          if(digitalRead(MODE_SWITCH)){
            onSecondKey = false; // don't contiue a config across mode switches
            HandleNoteOn(0, keyNotes[i], NOTE_VELOCITY);
          } else {
            handleConfig(i);
          }
        }else{
          HandleNoteOff(0, keyNotes[i], NOTE_VELOCITY);
        }
        keyStates[i] = newKeyStates[i];
      }
    }
    // write current states to last states
    for(int i = 0; i < NUM_KEYS; i++){
      lastKeyStates[i] = newKeyStates[i];
    }
    // Serial.print("Key states: ");
    // for(int i = 0; i < NUM_KEYS; i++){
    //   Serial.print(keyStates[i]);
    //   Serial.print(" ");
    // }
    // Serial.println();
    // Serial.print("Raw touch values: ");
    // for(int i = 0; i < NUM_KEYS; i++){
    //   Serial.print(rawTouchValues[i]);
    //   Serial.print(" ");
    // }
    // Serial.println();
    needUpdateStates = false;
  }
  // synth stuff
  for(unsigned int i = 0; i < MAX_POLY; i++){
    voices[i].env.update();
  }
}

void setOscWaveforms(const int8_t *newForm){
  // RUNS ON MAIN CORE
  // set the waveforms for the envelopes
  for(unsigned int i = 0; i < MAX_POLY; i++){
    voices[i].osc.setTable(newForm);
  }
}

void setADSR(){
  // use values from the global variables to set the ADSR
  for(unsigned int i = 0; i < MAX_POLY; i++){
    voices[i].env.setTimes(attack, decay, sustain, release);
    voices[i].env.setADLevels(attackLevel, decayLevel);
  }
}

const unsigned int timeMap[NUM_REAL_KEYS] = {0, 22, 100, 200, 300, 500, 750, 1000, 1500, 2000, 2500, 3000, 3500};
const byte levelMap[NUM_REAL_KEYS] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96};

void handleConfig(int key){
  // RUNS ON MAIN CORE
  // handle a key being pressed in config mode
  // first, correct the inKey number to skip the first and second key
  key = key < 1 ? key : key -2;
  // log it
  Serial.print("Key ");
  Serial.print(key);
  Serial.println(" pressed in config mode");
  if(onSecondKey){
    // we're in a two-key sequence
    Serial.println("Second key pressed");
    switch(firstKey){
      case 0:
        // octave change
        currentOctave = key;
        // clamp to 1-7
        if (currentOctave < 1){
          currentOctave = 1;
        } else if (currentOctave > 7){
          currentOctave = 7;
        }
        setOctave(currentOctave);
        Serial.print("Octave set to ");
        Serial.println(currentOctave);
      case 5:
        /*
        sine: 0
        square: 1
        saw: 2
        triangle: 3
        */
       switch(key){
        case 0:
          setOscWaveforms(SIN512_DATA);
          Serial.println("Waveform set to sine");
          break;
        case 1:
          setOscWaveforms(SQUARE_NO_ALIAS512_DATA);
          Serial.println("Waveform set to square");
          break;
        case 2:
          setOscWaveforms(SAW512_DATA);
          Serial.println("Waveform set to saw");
          break;
        case 3:
          setOscWaveforms(TRIANGLE512_DATA);
          Serial.println("Waveform set to triangle");
          break;
       }
       break;
      case 7:
        // attack
        attack = timeMap[key];
        Serial.print("Attack set to ");
        Serial.println(attack);
        setADSR();
        break;
      case 9:
        // decay
        decay = timeMap[key];
        Serial.print("Decay set to ");
        Serial.println(decay);
        setADSR();
        break;
      case 11:
        // sustain
        decayLevel = levelMap[key];
        Serial.print("Sustain set to ");
        Serial.println(decayLevel);
        setADSR();
        break;
      case 12:
        // release
        release = timeMap[key];
        Serial.print("Release set to ");
        Serial.println(release);
        setADSR();
        break;
      default:
        Serial.println("On second key, got unknown initial config key");
        break; // we really should never get here
      }
    onSecondKey = false;
  } else {
    switch(key){
      case 0:
        // octave change (2 key)
        firstKey = key;
        onSecondKey = true;
        Serial.println("Waiting for second key (octave)");
        break;
      case 1:
        // recalibrate sensing (it's C#, get it?)
        calibrateTouches();
        Serial.println("Recalibrated touch sensors");
        break;
      case 4:
        // Octave up
        currentOctave++;
        if(currentOctave > 7){
          currentOctave = 7;
        }
        setOctave(currentOctave);
        Serial.print("Octave up to ");
        Serial.println(currentOctave);
        break;
      case 2:
        // Octave down
        currentOctave--;
        if(currentOctave < 1){
          currentOctave = 1;
        }
        setOctave(currentOctave);
        Serial.print("Octave down to ");
        Serial.println(currentOctave);
        break;
      case 5:
        // Waveform change (2 key)
        firstKey = key;
        onSecondKey = true;
        Serial.println("Waiting for second key (waveform)");
        break;
      case 7:
        // Attack change (2 key)
        firstKey = key;
        onSecondKey = true;
        Serial.println("Waiting for second key (attack)");
        break;
      case 9:
        // Decay change (2 key)
        firstKey = key;
        onSecondKey = true;
        Serial.println("Waiting for second key (decay)");
        break;
      case 11:
        // Sustain change (2 key)
        firstKey = key;
        onSecondKey = true;
        Serial.println("Waiting for second key (sustain)");
        break;
      case 12:
        // Release change (2 key)
        firstKey = key;
        onSecondKey = true;
        Serial.println("Waiting for second key (release)");
        break;
      default:
        Serial.print("Key ");
        Serial.print(key);
        Serial.println(" pressed in config mode, but no action defined");
        break;
    }
  }
}

int updateAudio(){
  int currentSample = 0;

  for(unsigned int i = 0; i < MAX_POLY; i++){
    currentSample += voices[i].env.next() * voices[i].osc.next();
  }
  return (int) (currentSample)>>8;
}


void loop() {
  audioHook();
}
