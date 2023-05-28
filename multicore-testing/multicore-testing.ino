#include <Arduino.h>
#include "Esp32SynchronizationContext.h"

// use this to synchronize calls by executing functors on the target thread
Esp32SynchronizationContext g_mainSync;
// just something we can increment
unsigned long long g_count;
void thread1(void * state){
  // This task just posts Hello World 1! and a count
  // to the target thread over and over again, delaying
  // by 3/4 of a second each time

  // infinite loop or stop if error
  // use post() to execute code on the target thread
  // - does not block here
  while(g_mainSync.post([](void*state){
    // BEGIN EXECUTE ON TARGET THREAD
    Serial.printf("Hello world 1! - Count: %llu\r\n",g_count);
    // normally we couldn't access g_count
    // from a different task/thread safely
    // but this always runs on the target 
    // thread so we're okay since g_count
    // never gets touched by any other
    // thread
    ++g_count;
    // END EXECUTE ON TARGET THREAD
    })) {
      // EXECUTES ON THIS THREAD:
      delay(750);
    }
    
  // never executes unless error, but if 
  // we get here, delete the task
  vTaskDelete( NULL );
}
void thread2(void * state){
  // This task just sends Hello World 2! and a count
  // to the target thread over and over again, delaying
  // by 1 second each time

  // infinite loop or stop if error
  // use send() to execute code on the target thread
  // - blocks here until method returns
  while(g_mainSync.send([](void*state){
    // BEGIN EXECUTE ON TARGET THREAD
    Serial.printf("Hello world 2! - Count: %llu\r\n",g_count);
    // normally we couldn't access g_count
    // from a different task/thread safely
    ++g_count;
    // END EXECUTE ON TARGET THREAD
    })) {
      // EXECUTES ON THIS THREAD:
      delay(1000);
    }
    
  // never executes unless error, but if 
  // we get here, delete the task
  vTaskDelete( NULL );
}

void setup()
{
  g_count = 0;
  Serial.begin(115200);
  // initialize our synchronization context
  if(!g_mainSync.begin()) {
    Serial.println("Error initializing synchronization context");
    while(true); // halt
  }
  // create a task on the first core (the one that FreeRTOS runs on)
  xTaskCreatePinnedToCore(
    thread1,    // Function that should be called
    "Message feeder 1",   // Name of the task (for debugging)
    1000,            // Stack size (bytes)
    NULL,            // Parameter to pass
    1,               // Task priority
    NULL,             // Task handle
    0                // core
  );
  // create a task on the second core (the one setup()/loop() run on, and the one the Arduino framework runs on)
  xTaskCreatePinnedToCore(
    thread2,    // Function that should be called
    "Message feeder 2",   // Name of the task (for debugging)
    1000,            // Stack size (bytes)
    NULL,            // Parameter to pass
    1,               // Task priority
    NULL,             // Task handle
    1               // core
  );
}

void loop()
{
  // this simply dispatches calls made by send() or post()
  // by executing them here. Note that long running methods
  // or a backlogged queue can cause this to block for a 
  // significant amount of time. Try to avoid putting long
  // running calls into the synchronization context themselves
  // that's what Tasks are for anyway.
  if(!g_mainSync.update()) {
    Serial.println("Could not update synchronization context");
  }
}