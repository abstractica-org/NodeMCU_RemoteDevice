/*
  Author: Tobias Grundtvig
*/

#ifndef DeviceUDPClient_h
#define DeviceUDPClient_h

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <BasicUDP.h>

class DeviceUDPClient : BasicUDP
{
public:
    DeviceUDPClient(uint64_t deviceId, const char* deviceType, uint16_t deviceVersion);
    void begin(uint16_t localPort, uint16_t serverPort);
    void update(unsigned long curTime);
    void stop();
    bool readyToSendPacketToServer();

    //Sending overrides:
    uint16_t sendPacketToServer(uint16_t command,
                                uint16_t arg1,
                                uint16_t arg2,
                                uint8_t* pData,
                                uint16_t size,
                                bool blocking,
                                bool forceSend      );

    uint16_t sendPacketToServer(uint16_t command,
                                uint16_t arg1,
                                uint16_t arg2,
                                const char* str,
                                bool blocking,
                                bool forceSend      );

    uint16_t sendPacketToServer(uint16_t command,
                                uint16_t arg1,
                                uint16_t arg2,
                                bool blocking,
                                bool forceSend      );

    uint16_t sendPacketToServer(uint16_t command,
                                uint16_t arg1,
                                uint16_t arg2,
                                uint8_t* pData,
                                uint16_t size       );

    uint16_t sendPacketToServer(uint16_t command,
                                uint16_t arg1,
                                uint16_t arg2       );

    uint16_t sendPacketToServer(uint16_t command,
                                uint16_t arg1,
                                uint16_t arg2,
                                const char *str);

    
    virtual void onPacketDelivered(uint16_t msgId, uint16_t response)
    {
        Serial.print("Packet delivered: (msgId: ");
        Serial.print(msgId);
        Serial.print(", response: ");
        Serial.print(response);
        Serial.println(")");
    }

    virtual void onPacketCancelled(uint16_t msgId)
    {
        Serial.print("Packet cancelled: ");
        Serial.println(msgId);
    }

    virtual uint16_t onPacketReceived(uint16_t command, uint16_t arg1, uint16_t arg2, uint8_t* pData, uint16_t size)
    {
        Serial.print("Packet received from server: (command: ");
        Serial.print(command);
        Serial.print(", argument 1: ");
        Serial.print(arg1);
        Serial.print(", argument 2: ");
        Serial.print(arg2);
        Serial.print(", packetsize: ");
        Serial.print(size);
        Serial.println(")");
        if(size > 0)
        {
            Serial.print("Load: {");
            for(int i = 0; i < size; ++i)
            {
                if(i > 0) Serial.print(",");
                Serial.print(pData[i]);
            }
            Serial.println("}");
        }
        Serial.println();
        return command;
    }

    virtual void onWiFiConnected(long curTime)
    {
        Serial.println("WiFi connected!");
    }

    virtual void onWiFiDisconnected(long curTime)
    {
        Serial.println("WiFi disconnected!");
    }

    virtual void onServerConnected(long curTime)
    {
        Serial.println("Server connected!");
    }

    virtual void onServerDisconnected(long curTime)
    {
        Serial.println("Server disconnected!");
    }
    
protected:
    void _writeIntegerToBuffer(uint8_t *buffer, uint64_t data, uint16_t index, uint8_t size);
    uint64_t _readIntegerFromBuffer(uint8_t *buffer, uint16_t index, uint8_t size);

private:
    void onPacketReceived(unsigned long curTime, IPAddress srcAddress, uint16_t srcPort, uint8_t* pData, uint16_t size);
    uint16_t _sendPingToServer();
    uint16_t _sendPacketToServer(uint16_t command, uint16_t arg1, uint16_t arg2, uint8_t* pData, uint16_t size, bool blocking, bool forceSend);
    void _sendReplyPacket(uint16_t msgId, uint16_t command, uint16_t arg1, uint16_t arg2, uint8_t* pData, uint16_t size);
    IPAddress _serverAddress;
    uint16_t _serverPort;
    uint64_t _deviceId;
    const char* _deviceType;
    uint16_t _deviceVersion;
    uint8_t _sendBuffer[_MAX_PACKET_SIZE];
    uint8_t _replyPacket[_MAX_PACKET_SIZE];
    uint16_t _sendBufferSize;
    uint16_t _curMsgId;
    uint16_t _lastReceivedMsgId;
    uint16_t _lastResponse;
    unsigned long _lastReceiveTime;
    unsigned long _lastSentTime;
    uint8_t _sentCount;
    bool _isSending;
    bool _isBlocking;                                                                                                                                                                             
    bool _serverConnected;
    bool _wifiConnected;
};

#endif