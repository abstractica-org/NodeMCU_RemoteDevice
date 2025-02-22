/*
  Author: Tobias Grundtvig
*/

#include <Arduino.h>
#include "RemoteDevice.h"

#define MAX_IDLE_TIME 5000
#define CONNECTED_SEND_INTERVAL 1000
#define DISCONNECTED_SEND_INTERVAL 10000
#define CONNECTED_SEND_COUNT 5

#define INIT 65535
#define INITACK 65534
#define MSGACK 65533
#define PING 65532


bool test = true;

RemoteDevice::RemoteDevice(uint64_t deviceId, const char* deviceType, uint16_t deviceVersion )
{
  _deviceId = deviceId;
  _deviceType = deviceType;
  _deviceVersion = deviceVersion;
}

void RemoteDevice::begin(uint16_t port, uint16_t serverPort)
{
  begin(port, serverPort, 0);
}

void RemoteDevice::begin(uint16_t port, uint16_t serverPort, uint16_t serverId)
{
  _serverAddress = IPAddress(255, 255, 255, 255);
  _serverPort = serverPort;
  _serverId = serverId;
  _serverConnected = false;
  _wifiConnected = false;
  _isSending = false;
  _lastReceiveTime = 0;
  _lastSentTime = 0;
  _curMsgId = 0;
  _writeIntegerToBuffer(_sendBuffer, _deviceId, 0, 8);

  _writeIntegerToBuffer(_replyPacket, _deviceId, 0, 8);
  BasicUDP::begin(port);
  #ifdef REMOTE_DEVICE_DEBUG
  Serial.println("Sending inital package to server!");
  #endif
  sendPacketToServer(INIT, _deviceVersion, _serverId, 0, 0, _deviceType);
}

void RemoteDevice::update(unsigned long curTime)
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
          #ifdef REMOTE_DEVICE_DEBUG
          Serial.print("timeSinceLastSend: ");
          Serial.print(timeSinceLastSend);
          Serial.print(", _sentCount: ");
          Serial.println(_sentCount);
          #endif
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
        _sendPacketToServer(PING, 0, 0, 0, 0, 0, 0, false, false);
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

void RemoteDevice::stop()
{
  BasicUDP::stop();
}

void RemoteDevice::onPacketReceived(unsigned long curTime, IPAddress srcAddress, uint16_t srcPort, uint8_t* pData, uint16_t size)
{
  //Since we have received a package, we assume that wifi is connected...
  if(!_wifiConnected)
  {
    _wifiConnected = true;
    onWiFiConnected(curTime);
  }
  #ifdef REMOTE_DEVICE_DEBUG
  Serial.print("Packet received: ");
  #endif
  if(size < 20)
  {
    #ifdef REMOTE_DEVICE_DEBUG
    Serial.println("Packet too small");
    #endif
    return;
  }
  uint64_t deviceId = _readIntegerFromBuffer(pData, 0, 8);
  if(_deviceId != deviceId)
  {
    #ifdef REMOTE_DEVICE_DEBUG
    Serial.println("Packet not for us!");
    #endif
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
      #ifdef REMOTE_DEVICE_DEBUG
      Serial.println("INIT received!");
      #endif
      _sendReplyPacket(msgId, INITACK, _deviceVersion, _serverId, 0, 0, (uint8_t*) _deviceType, strlen(_deviceType));
    }
    else
    {
      #ifdef REMOTE_DEVICE_DEBUG
      Serial.println("INITACK received!");
      #endif
      _isSending = false;
    }
    return;
  }  

  //Handle PING from server
  if(command == PING)
  {
    #ifdef REMOTE_DEVICE_DEBUG
    Serial.println("PING!");
    #endif
    _sendReplyPacket(msgId, MSGACK, 0, 0, 0, 0, 0, 0);
    return;
  }

  //Handle MSGACK
  if(command == MSGACK)
  {
    #ifdef REMOTE_DEVICE_DEBUG
    Serial.println("MSGACK!");
    #endif
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
    uint16_t arg3 = _readIntegerFromBuffer(pData, 16, 2);
    uint16_t arg4 = _readIntegerFromBuffer(pData, 18, 2);
    #ifdef REMOTE_DEVICE_DEBUG
    Serial.print("CMD: ");
    Serial.print(command);
    Serial.print("Arg1: ");
    Serial.print(arg1);
    Serial.print("Arg2: ");
    Serial.println(arg2);
    Serial.print("Arg3: ");
    Serial.print(arg3);
    Serial.print("Arg4: ");
    Serial.println(arg4);
    #endif
    _lastResponse = onPacketReceived(command, arg1, arg2, arg3, arg4, pData+20, size-20);
    _sendReplyPacket(msgId, MSGACK, _lastResponse, 0, 0, 0, 0, 0);
  }
  else
  {
    //We have an old message
    if(msgId == _lastReceivedMsgId)
    {
      //It is the latest message that we have already proccessed, so we just resend the result.
      #ifdef REMOTE_DEVICE_DEBUG
      Serial.print("Resending message acknowledgement (msgId: ");
      Serial.print(msgId);
      Serial.println(").");
      #endif
      _sendReplyPacket(msgId, MSGACK, _lastResponse, 0, 0, 0, 0, 0);
    }
    #ifdef REMOTE_DEVICE_DEBUG
    else
    {
      //This is an older "ghost" message, so we just report it and ignore it.
      Serial.print("Discarded ghost message: (msgId: ");
      Serial.print(msgId);
      Serial.print(", lastRecievedMsgId: ");
      Serial.print(_lastReceivedMsgId);
      Serial.println(")");
    }
    #endif
  }
  
}

uint16_t RemoteDevice::sendPacketToServer( uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2,
                                              uint16_t arg3,
                                              uint16_t arg4,
                                              uint8_t* pData,
                                              uint16_t size      )
{
  return _sendPacketToServer(command, arg1, arg2, arg3, arg4, pData, size, true, false);
}


uint16_t RemoteDevice::sendPacketToServer( uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2,
                                              uint16_t arg3,
                                              uint16_t arg4       )
{
  return _sendPacketToServer(command, arg1, arg2, arg3, arg4, 0, 0, true, false);
}

uint16_t RemoteDevice::sendPacketToServer( uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2,
                                              uint16_t arg3,
                                              uint16_t arg4, 
                                              const char* str       )
{
  return _sendPacketToServer(command, arg1, arg2, arg3, arg4, (uint8_t*) str, strlen(str), true, false);
}

uint16_t RemoteDevice::sendPacketToServer( uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2,
                                              uint16_t arg3,
                                              uint16_t arg4, 
                                              uint8_t* pData,
                                              uint16_t size,
                                              bool blocking,
                                              bool forceSend)
{ 
  return _sendPacketToServer(command, arg1, arg2, arg3, arg4, pData, size, blocking, forceSend);
}

uint16_t RemoteDevice::sendPacketToServer( uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2,
                                              uint16_t arg3,
                                              uint16_t arg4,
                                              bool blocking,
                                              bool forceSend)
{
  return _sendPacketToServer(command, arg1, arg2, arg3, arg4, 0, 0, true, false);
}

uint16_t RemoteDevice::sendPacketToServer( uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2,
                                              uint16_t arg3,
                                              uint16_t arg4,
                                              const char* str,
                                              bool blocking,
                                              bool forceSend)
{ 
  return _sendPacketToServer(command, arg1, arg2, arg3, arg4, (uint8_t*) str, strlen(str), blocking, forceSend);
}

uint16_t RemoteDevice::_sendPacketToServer(uint16_t command,
                                              uint16_t arg1,
                                              uint16_t arg2,
                                              uint16_t arg3,
                                              uint16_t arg4,
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
  _writeIntegerToBuffer(_sendBuffer, arg3, 16, 2);
  _writeIntegerToBuffer(_sendBuffer, arg4, 18, 2);
  for(int i = 0; i < size; ++i)
  {
    _sendBuffer[20+i] = pData[i];
  }

  //Setting the packet meta info:
  _isSending = true;
  _isBlocking = blocking;
  _sentCount = 1;
  _sendBufferSize = 20 + size;
  _lastSentTime = millis();

  //First send attempt:
  sendPacket(_serverAddress, _serverPort, _sendBuffer, _sendBufferSize);
  return _curMsgId;
}

void RemoteDevice::_sendReplyPacket(uint16_t msgId, uint16_t command, uint16_t arg1, uint16_t arg2, uint16_t arg3, uint16_t arg4, uint8_t* pData, uint16_t size)
{
  _writeIntegerToBuffer(_replyPacket, msgId, 8, 2);
  _writeIntegerToBuffer(_replyPacket, command, 10, 2);
  _writeIntegerToBuffer(_replyPacket, arg1, 12, 2);
  _writeIntegerToBuffer(_replyPacket, arg2, 14, 2);
  _writeIntegerToBuffer(_replyPacket, arg3, 16, 2);
  _writeIntegerToBuffer(_replyPacket, arg4, 18, 2);
  for(int i = 0; i < size; ++i)
  {
    _replyPacket[20+i] = pData[i];
  }
  sendPacket(_serverAddress, _serverPort, _replyPacket, 20 + size);
}

void RemoteDevice::_writeIntegerToBuffer(uint8_t *buffer, uint64_t data, uint16_t index, uint8_t size)
{
  for (uint8_t i = 0; i < size; ++i)
  {
    buffer[i + index] = (uint8_t)(data >> (i * 8));
  }
}

uint64_t RemoteDevice::_readIntegerFromBuffer(uint8_t *buffer, uint16_t index, uint8_t size)
{
  uint64_t res = 0;
  for (int8_t i = size - 1; i >= 0; --i)
  {
    res <<= 8;
    res += buffer[i + index];
  }
  return res;
}
