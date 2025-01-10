/*
  Author: Tobias Grundtvig
*/

#include <Arduino.h>
#include "DeviceUDPClient.h"

#define MAX_IDLE_TIME 5000
#define CONNECTED_SEND_INTERVAL 1000
#define DISCONNECTED_SEND_INTERVAL 10000
#define CONNECTED_SEND_COUNT 5

#define INIT 65535
#define INITACK 65534
#define MSGACK 65533
#define PING 65532


bool test = true;

DeviceUDPClient::DeviceUDPClient(uint64_t deviceId, const char* deviceType, uint16_t deviceVersion )
{
  _deviceId = deviceId;
  _deviceType = deviceType;
  _deviceVersion = deviceVersion;
}

void DeviceUDPClient::begin(uint16_t port, uint16_t serverPort)
{
  _serverAddress = IPAddress(255, 255, 255, 255);
  _serverPort = serverPort;
  _serverConnected = false;
  _wifiConnected = false;
  _isSending = false;
  _lastReceiveTime = 0;
  _lastSentTime = 0;
  _curMsgId = 0;
  _writeIntegerToBuffer(_sendBuffer, _deviceId, 0, 8);

  _writeIntegerToBuffer(_replyPacket, _deviceId, 0, 8);
  BasicUDP::begin(port);
  Serial.println("Sending inital package to server!");
  sendPacketToServer(INIT, _deviceVersion, 0, _deviceType);
}

void DeviceUDPClient::update(unsigned long curTime)
{
  if(WiFi.isConnected())
  {
    if(!_wifiConnected)
    {
      _wifiConnected = true;
      onWiFiConnected(curTime);
    }
    BasicUDP::update(curTime);
    if(_isSending)
    {
      unsigned long timeSinceLastSend = curTime - _lastSentTime;
      unsigned long interval = _serverConnected ? CONNECTED_SEND_INTERVAL : DISCONNECTED_SEND_INTERVAL;
      if(timeSinceLastSend > interval)
      {
        if(_serverConnected && _sentCount >= CONNECTED_SEND_COUNT)
        {
          _serverConnected = false;
          _serverAddress = IPAddress(255, 255, 255, 255);
          Serial.print("timeSinceLastSend: ");
          Serial.print(timeSinceLastSend);
          Serial.print(", _sentCount: ");
          Serial.println(_sentCount);
          
          onServerDisconnected(curTime);
          return;
        }
        ++_sentCount;
        _lastSentTime = curTime;
        Serial.println("Resending packet!");
        sendPacket(_serverAddress, _serverPort, _sendBuffer, _sendBufferSize);
      }
    }
    else
    {
      unsigned long timeSinceLastReceive = curTime - _lastReceiveTime;
      if(timeSinceLastReceive >= MAX_IDLE_TIME)
      {
        //Send PING packet
        _sendPacketToServer(PING, 0, 0, 0, 0, false, false);
      }
    }
  }
  else
  {
    if(_wifiConnected)
    {
      _wifiConnected = false;
      if(_serverConnected)
      {
        _serverConnected = false;
        onServerDisconnected(curTime);
      }
      onWiFiDisconnected(curTime);
    }
  }
}

void DeviceUDPClient::stop()
{
  BasicUDP::stop();
}

void DeviceUDPClient::onPacketReceived(unsigned long curTime, IPAddress srcAddress, uint16_t srcPort, uint8_t* pData, uint16_t size)
{
  //Since we have received a package, we assume that wifi is connected...
  if(!_wifiConnected)
  {
    _wifiConnected = true;
    onWiFiConnected(curTime);
  }
  Serial.print("Packet received: ");
  if(size < 16)
  {
    Serial.println("Packet too small");
    return;
  }
  uint64_t deviceId = _readIntegerFromBuffer(pData, 0, 8);
  if(_deviceId != deviceId)
  {
    Serial.println("Packet not for us!");
    return;
  }
  //We assume that the packet comes from the server since it has the correct Device ID in the header
  if(!_serverConnected)
  {
    _sentCount = 0; //Server just reconnected, so we reset send count for current message
    _serverConnected = true;
    _serverAddress = srcAddress;
    onServerConnected(curTime);
  }

  //Update last receive time
  _lastReceiveTime = curTime;

  //Extract message ID and Command from message
  uint16_t msgId = _readIntegerFromBuffer(pData, 8, 2);
  uint16_t command = _readIntegerFromBuffer(pData, 10, 2);
  
  //Handle INIT and INITACK
  if(command == INIT || command == INITACK)
  {
    _lastReceivedMsgId = 0;
    _sentCount = 0;
    if(command == INIT)
    {
      Serial.println("INIT received!");
      _sendReplyPacket(msgId, INITACK, _deviceVersion, 0, (uint8_t*) _deviceType, strlen(_deviceType));
    }
    else
    {
      Serial.println("INITACK received!");
      _isSending = false;
    }
    return;
  }  

  //Handle PING from server
  if(command == PING)
  {
    _sendReplyPacket(msgId, MSGACK, 0, 0, 0, 0);
    return;
  }

  //Handle MSGACK
  if(command == MSGACK)
  {
    if(_isSending && msgId == _curMsgId)
    {
      //This is a MSGACK for the current message
      _isSending = false;
      _sentCount = 0;
      //report delivered if the current sending message is not a PING.
      if(_readIntegerFromBuffer(_sendBuffer,10,2) != PING)
      {
        //Read the response from the incoming message
        uint16_t response = _readIntegerFromBuffer(pData, 12, 2);
        onPacketDelivered(_curMsgId, response);
      }
    }
    return;
  }
  
  //This is a regular message from the server
  if( msgId > _lastReceivedMsgId || (_lastReceivedMsgId - msgId) > 30000 )
  {
    //We have a new message
    _lastReceivedMsgId = msgId;
    uint16_t arg1 = _readIntegerFromBuffer(pData, 12, 2);
    uint16_t arg2 = _readIntegerFromBuffer(pData, 14, 2);
    _lastResponse = onPacketReceived(command, arg1, arg2, pData+16, size-16);
    _sendReplyPacket(msgId, MSGACK, _lastResponse, 0, 0, 0);
  }
  else
  {
    //We have an old message
    if(msgId == _lastReceivedMsgId)
    {
      //It is the latest message that we have already proccessed, so we just resend the result.
      Serial.print("Resending message acknowledgement (msgId: ");
      Serial.print(msgId);
      Serial.println(").");
      _sendReplyPacket(msgId, MSGACK, _lastResponse, 0, 0, 0);
    }
    else
    {
      //This is an older "ghost" message, so we just report it and ignore it.
      Serial.print("Discarded ghost message: (msgId: ");
      Serial.print(msgId);
      Serial.print(", lastRecievedMsgId: ");
      Serial.print(_lastReceivedMsgId);
      Serial.println(")");
    }
  }
  
}

uint16_t DeviceUDPClient::sendPacketToServer( uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2,
                                              uint8_t* pData,
                                              uint16_t size      )
{
  return _sendPacketToServer(command, arg1, arg2, pData, size, true, false);
}


uint16_t DeviceUDPClient::sendPacketToServer( uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2       )
{
  return _sendPacketToServer(command, arg1, arg2, 0, 0, true, false);
}

uint16_t DeviceUDPClient::sendPacketToServer( uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2,
                                              const char* str       )
{
  return _sendPacketToServer(command, arg1, arg2, (uint8_t*) str, strlen(str), true, false);
}

uint16_t DeviceUDPClient::sendPacketToServer( uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2,
                                              uint8_t* pData,
                                              uint16_t size,
                                              bool blocking,
                                              bool forceSend)
{ 
  return _sendPacketToServer(command, arg1, arg2, pData, size, blocking, forceSend);
}

uint16_t DeviceUDPClient::sendPacketToServer( uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2,
                                              bool blocking,
                                              bool forceSend)
{
  return _sendPacketToServer(command, arg1, arg2, 0, 0, true, false);
}

uint16_t DeviceUDPClient::sendPacketToServer( uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2,
                                              const char* str,
                                              bool blocking,
                                              bool forceSend)
{ 
  return _sendPacketToServer(command, arg1, arg2, (uint8_t*) str, strlen(str), blocking, forceSend);
}

uint16_t DeviceUDPClient::_sendPacketToServer(uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2,
                                              uint8_t* pData,
                                              uint16_t size,
                                              bool blocking,
                                              bool forceSend)
{
  if(_isSending)
  { 
    if(_isBlocking && !forceSend)
    {
      //Current message has priority
      return 0;
    }
    if(_readIntegerFromBuffer(_sendBuffer,10,2) != PING)
    {
      //The current packet is not a PING, so it needs to be reported as cancelled!
      onPacketCancelled(_curMsgId);
    }
  }
  //Update current message id. 0 is reserved for not sending
  _curMsgId = _curMsgId == 65535 ? 1 : _curMsgId + 1;

  //Write packet to _sendBuffer
  _writeIntegerToBuffer(_sendBuffer, _curMsgId, 8, 2);
  _writeIntegerToBuffer(_sendBuffer, command, 10, 2);
  _writeIntegerToBuffer(_sendBuffer, arg1, 12, 2);
  _writeIntegerToBuffer(_sendBuffer, arg2, 14, 2);
  for(int i = 0; i < size; ++i)
  {
    _sendBuffer[16+i] = pData[i];
  }

  //Setting the packet meta info:
  _isSending = true;
  _isBlocking = blocking;
  _sentCount = 1;
  _sendBufferSize = 16 + size;
  _lastSentTime = millis();

  //First send attempt:
  sendPacket(_serverAddress, _serverPort, _sendBuffer, _sendBufferSize);
  return _curMsgId;
}

void DeviceUDPClient::_sendReplyPacket(uint16_t msgId, uint16_t command, uint16_t arg1, uint16_t arg2, uint8_t* pData, uint16_t size)
{
  _writeIntegerToBuffer(_replyPacket, msgId, 8, 2);
  _writeIntegerToBuffer(_replyPacket, command, 10, 2);
  _writeIntegerToBuffer(_replyPacket, arg1, 12, 2);
  _writeIntegerToBuffer(_replyPacket, arg2, 14, 2);
  for(int i = 0; i < size; ++i)
  {
    _replyPacket[16+i] = pData[i];
  }
  sendPacket(_serverAddress, _serverPort, _replyPacket, 16 + size);
}

void DeviceUDPClient::_writeIntegerToBuffer(uint8_t *buffer, uint64_t data, uint16_t index, uint8_t size)
{
  for (uint8_t i = 0; i < size; ++i)
  {
    buffer[i + index] = (uint8_t)(data >> (i * 8));
  }
}

uint64_t DeviceUDPClient::_readIntegerFromBuffer(uint8_t *buffer, uint16_t index, uint8_t size)
{
  uint64_t res = 0;
  for (int8_t i = size - 1; i >= 0; --i)
  {
    res <<= 8;
    res += buffer[i + index];
  }
  return res;
}
