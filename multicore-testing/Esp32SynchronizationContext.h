#ifndef HTCW_ESP32SYNCHRONIZATIONCONTEXT_H
#define HTCW_ESP32SYNCHRONIZATIONCONTEXT_H
#include <Arduino.h>
#include <functional>
#include "freertos/ringbuf.h"
// provides an easy way to synchronize access between threads
class Esp32SynchronizationContext
{
    RingbufHandle_t m_messageRingBufferHandle;
    struct Message
    {
        std::function<void(void *)> callback;
        void *state;
        TaskHandle_t finishedNotifyHandle;
    };

public:
    Esp32SynchronizationContext() : m_messageRingBufferHandle(nullptr) {}
    Esp32SynchronizationContext(const Esp32SynchronizationContext &rhs) = delete;
    Esp32SynchronizationContext(Esp32SynchronizationContext &&rhs) = delete;
    Esp32SynchronizationContext &operator=(const Esp32SynchronizationContext &rhs) = delete;
    Esp32SynchronizationContext &operator=(const Esp32SynchronizationContext &&rhs) = delete;
    virtual ~Esp32SynchronizationContext()
    {
        end();
    }
    // begins the synchronization context. the queueSize parameter indicates the number of methods in the queue before it blocks
    bool begin(size_t queueSize = 10)
    {
        if (nullptr != m_messageRingBufferHandle)
            return false;
        m_messageRingBufferHandle = xRingbufferCreate(sizeof(Message) * queueSize + (sizeof(Message) - 1), RINGBUF_TYPE_NOSPLIT);
        if (nullptr == m_messageRingBufferHandle)
        {
            return false;
        }
        return true;
    }
    // ends the synchronization context
    void end()
    {
        if (nullptr != m_messageRingBufferHandle)
        {
            vRingbufferDelete(m_messageRingBufferHandle);
            m_messageRingBufferHandle = nullptr;
        }
    }
    // posts a message to the thread update() is called from. this method does not block
    bool post(std::function<void(void *)> fn, void *state = nullptr, uint32_t timeoutMS = 10000)
    {
        Message msg;
        msg.callback = fn;
        msg.state = state;
        msg.finishedNotifyHandle = nullptr;
        UBaseType_t res = xRingbufferSend(m_messageRingBufferHandle, &msg, sizeof(msg), pdMS_TO_TICKS(timeoutMS));
        return (res == pdTRUE);
    }
    // sends a message to the thread update() is called from. this method blocks until the update thread executes the method and it returns.
    bool send(std::function<void(void *)> fn, void *state = nullptr, uint32_t timeoutMS = 10000)
    {
        Message msg;
        msg.callback = fn;
        msg.state = state;
        msg.finishedNotifyHandle = xTaskGetCurrentTaskHandle();
        uint32_t mss = millis();
        UBaseType_t res = xRingbufferSend(m_messageRingBufferHandle, &msg, sizeof(msg), pdMS_TO_TICKS(timeoutMS));
        mss = millis() - mss;
        if (timeoutMS >= mss)
            timeoutMS -= mss;
        else
            timeoutMS = 0;
        if (res == pdTRUE)
        {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeoutMS));
            return true;
        }
        return false;
    }
    // processes pending messages in the message queue. This should be called in a loop on the target thread.
    bool update()
    {
        //Receive an item from no-split ring buffer
        size_t size = sizeof(Message);
        Message *pmsg = (Message *)xRingbufferReceive(m_messageRingBufferHandle, &size, 0);
        if (nullptr == pmsg)
            return true;
        if (size != sizeof(Message))
            return false;
        Message msg = *pmsg;
        vRingbufferReturnItem(m_messageRingBufferHandle, pmsg);
        msg.callback(msg.state);
        if (nullptr != msg.finishedNotifyHandle)
        {
            xTaskNotifyGive(msg.finishedNotifyHandle);
        }
        return true;
    }
};
#endif