
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
#define NUM_KEYS 13
#define NUM_BUILTIN_TOUCH 10
#define NOTE_VELOCITY 127

#define BUILTIN_TOUCH_THRESHOLD 40
#define ADDL_TOUCH_THRESHOLD 40
const byte builtinTouchPins[NUM_BUILTIN_TOUCH] = {4, 0, 2, 15, 13, 12, 14, 27, 33, 32};
bool keyStates[NUM_KEYS] = {0};
byte keyNotes[NUM_KEYS] = {60, 0, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71}; // MIDI note numbers

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
  pinMode(LED, OUTPUT);
  // Serial.begin(115200);
  for(unsigned int i = 0; i < MAX_POLY; i++){
    voices[i].env.setADLevels(ATTACK_LEVEL,DECAY_LEVEL);
    voices[i].env.setTimes(ATTACK,DECAY,SUSTAIN,RELEASE);
    voices[i].osc.setTable(WAVE_DATA);
  }

  //aSin0.setFreq(440); // default frequency
  startMozzi(CONTROL_RATE);
}


void updateControl(){
  // do builtin captouch
  for(int i = 0; i < 10; i++){
    bool noteState = touchRead(builtinTouchPins[i]) < BUILTIN_TOUCH_THRESHOLD;
    if(noteState != keyStates[i]){
      keyStates[i] = noteState;
      if(noteState){
        HandleNoteOn(1, keyNotes[i], NOTE_VELOCITY);
      } else {
        HandleNoteOff(1, keyNotes[i], NOTE_VELOCITY);
      }
    }
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
