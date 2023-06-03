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

#define THRESHOLD 30
#define BASELINE 10

void setup(){
    Serial.begin(115200);
    pinMode(23, OUTPUT);
    pinMode(36, INPUT);
    pinMode(39, INPUT);
    pinMode(22, OUTPUT);
    Serial.println("setup");
}

void loop(){
    unsigned long touch1 = readCapacitiveExternal(23, 36);
    unsigned long touch2 = readCapacitiveExternal(22, 39);
    bool touch1State = touch1 > THRESHOLD + BASELINE;
    bool touch2State = touch2 > THRESHOLD + BASELINE;
    Serial.print(touch1State);
    Serial.print(" ");
    Serial.print(touch2State);
    Serial.print(" ");
    Serial.print(touch1);
    Serial.print(" ");
    Serial.println(touch2);
    delay(50);
}