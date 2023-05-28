
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
#define WAVE_DATA SAW512_DATA 

//Envelope controllers
#define ATTACK 22
#define DECAY 3000
#define SUSTAIN 8000
#define RELEASE 300
#define ATTACK_LEVEL 64
#define DECAY_LEVEL 0

//Voices
struct Voice{
  Oscil<OSC_NUM_CELLS, AUDIO_RATE> osc;  // audio oscillator
  ADSR<CONTROL_RATE, AUDIO_RATE> env;  // envelope generator
  byte note = 0;
  byte velocity = 0;
};

Voice voices[MAX_POLY];

//optional midi monitor
#define LED 35
#define NUM_KEYS 15 // two are dead and not used, Touch1 and Touch2 (index 1 and 2)
#define NUM_BUILTIN_TOUCH 10
#define NOTE_VELOCITY 127

#define BUILTIN_TOUCH_THRESHOLD 40
#define ADDL_TOUCH_THRESHOLD 40
const byte builtinTouchPins[NUM_BUILTIN_TOUCH] = {4, 0, 2, 15, 13, 12, 14, 27, 33, 32};
bool keyStates[NUM_KEYS] = {0};

#define CHROMATIC_SCALE_C1 {24, 0, 0, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36}
#define CHROMATIC_SCALE_C2 {36, 0, 0, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48}
#define CHROMATIC_SCALE_C3 {48, 0, 0, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60}
#define CHROMATIC_SCALE_C4 {60, 0, 0, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72}
#define CHROMATIC_SCALE_C5 {72, 0, 0, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84}
#define CHROMATIC_SCALE_C6 {84, 0, 0, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96}
#define CHROMATIC_SCALE_C7 {96, 0, 0, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108}

byte currentOctave = 4;

byte keyNotes[NUM_KEYS] = CHROMATIC_SCALE_C4;

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

void HandleControlChange(byte channel, byte control, byte value) {

}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_OFF); // disable wifi to leave power for audio and compute for touchUpdate

  // send the scale to serial
  Serial.print("Scale: ");
  for(int i = 0; i < NUM_KEYS; i++){
    Serial.print(keyNotes[i]);
    Serial.print(" ");
  }
  Serial.println();

  for(unsigned int i = 0; i < MAX_POLY; i++){
    voices[i].env.setADLevels(ATTACK_LEVEL,DECAY_LEVEL);
    voices[i].env.setTimes(ATTACK,DECAY,SUSTAIN,RELEASE);
    voices[i].osc.setTable(WAVE_DATA);
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
  // *NEVER* touch any mutable global variables here, because thread safety
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
    // see if any touch values are below the threshold
    for(int i = 0; i < NUM_BUILTIN_TOUCH; i++){
      if(i == 1 || i == 2){
        continue; // skip the second one, it's not broken out
      }
      bool noteState = rawTouchValues[i] < BUILTIN_TOUCH_THRESHOLD;
      if(noteState != newKeyStates[i]){
        newKeyStates[i] = noteState;
      }
    }
    for(int i = 0; i < NUM_KEYS; i++){
      if(newKeyStates[i] != keyStates[i] && lastKeyStates[i] == newKeyStates[i]){
        if(newKeyStates[i]){
          HandleNoteOn(0, keyNotes[i], NOTE_VELOCITY);
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
