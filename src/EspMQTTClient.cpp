#include "EspMQTTClient.h"

EspMQTTClient::EspMQTTClient(const char* location, const char* mqttUsername, const char* mqttPassword) : 
_mqttClientName(location),
_mqttUsername(mqttUsername),
_mqttPassword(mqttPassword)
{
  // _mqttClient(mqttServerIp, mqttServerPort, _wifiClient)
  _mqttClient.setClient(_wifiClient);
  // WiFi connection
  _handleWiFi = true;
  _wifiConnected = false;
  _connectingToWifi = false;
  _nextWifiConnectionAttemptMillis = 500;
  _lastWifiConnectionAttemptMillis = 0;
  _wifiReconnectionAttemptDelay = 60 * 1000;
  _failedWifiConnectionAttemptCount = 0;
  _max_uptime_server_minutes = 3; //nur wenn aktiv!

  // MQTT client
  _mqttConnected = false;
  _nextMqttConnectionAttemptMillis = 0;
  _mqttReconnectionAttemptDelay = 15 * 1000; // 15 seconds of waiting between each mqtt reconnection attempts by default
  _mqttLastWillTopic = 0;
  _mqttLastWillMessage = 0;
  _mqttLastWillRetain = false;
  _mqttCleanSession = true;
  _mqttClient.setCallback([this](char* topic, byte* payload, unsigned int length) {this->mqttMessageReceivedCallback(topic, payload, length);});
  _failedMQTTConnectionAttemptCount = 0;
  _sleep = false;
  _loopcount = 0;
  _looptime_millis = 0;
  _publish_Wifi_RSSI = false;

  // HTTP/OTA update related
  _updateServerAddress = NULL;
  _httpServer = NULL;
  _httpUpdater = NULL;
  _enableOTA = false;
  _OTA_via_MQTT = false;

  // other
  _enableDebugMessages = false;
  _drasticResetOnConnectionFailures = false;
  _connectionEstablishedCallback = onConnectionEstablished;
  _connectionEstablishedCount = 0;
}

EspMQTTClient::~EspMQTTClient()
{
  if (_httpServer != NULL)
    delete _httpServer;
  if (_httpUpdater != NULL)
    delete _httpUpdater;
}


// =============== Configuration functions, most of them must be called before the first loop() call ==============

void EspMQTTClient::enableDebuggingMessages(const bool enabled)
{
  _enableDebugMessages = enabled;
}

void EspMQTTClient::enableHTTPWebUpdater(const char* username, const char* password, const char* address)
{
  if (_httpServer == NULL)
  {
    _httpServer = new WebServer(80);
    _httpUpdater = new ESPHTTPUpdateServer(_enableDebugMessages);
    _updateServerUsername = (char*)username;
    _updateServerPassword = (char*)password;
    _updateServerAddress = (char*)address;
  }
  else if (_enableDebugMessages)
    Serial.print("SYS! You can't call enableHTTPWebUpdater() more than once !\n");
}

void EspMQTTClient::enableHTTPWebUpdater(const char* address)
{
  if(_mqttUsername == NULL || _mqttPassword == NULL)
    enableHTTPWebUpdater("", "", address);
  else
    enableHTTPWebUpdater(_mqttUsername, _mqttPassword, address);
}

void EspMQTTClient::enableOTA(const char *password, const uint16_t port)
{
  _enableOTA = true;

  if (_mqttClientName != NULL)
    ArduinoOTA.setHostname(_mqttClientName);

  if (password != NULL)
    ArduinoOTA.setPassword(password);
  else if (_mqttPassword != NULL)
    ArduinoOTA.setPassword(_mqttPassword);

  if (port)
    ArduinoOTA.setPort(port);
}

void EspMQTTClient::enableMQTTPersistence()
{
  _mqttCleanSession = false;
}

void EspMQTTClient::enableLastWillMessage(const char* topic, const char* message, const bool retain)
{
  _mqttLastWillTopic = (char*)topic;
  _mqttLastWillMessage = (char*)message;
  _mqttLastWillRetain = retain;
}


// =============== Main loop / connection state handling =================

void EspMQTTClient::go_to_sleep(unsigned int deepsleeptime_minutes){
  if(!_sleep){
    _deepsleeptime_minutes = deepsleeptime_minutes;
    _loopcount = 0;
    _looptime_millis = millis(); 
    _sleep = true;
    if(_enableDebugMessages){
      Serial.println("Sleep activated");
    }
  }
}

void EspMQTTClient::loop()
{
  _wifiManager.process();
  bool wifiStateChanged = handleWiFi();
  // If there is a change in the wifi connection state, don't handle the mqtt connection state right away.
  // We will wait at least one lopp() call. This prevent the library from doing too much thing in the same loop() call.
  if(wifiStateChanged)
    return;

  // MQTT Handling
  bool mqttStateChanged = handleMQTT();
  if(mqttStateChanged)
    return;

  if (!_OTA_via_MQTT && ((_sleep && getConnectionEstablishedCount() > 0) || (_sleep && _failedWifiConnectionAttemptCount > 1)) && !_wifiManager.WifiAP_active(_max_uptime_server_minutes)){
    //only go to sleep if connected or at least one reconnection attempt was made to prevent too early sleep)
    _loopcount++;
    if(_loopcount > 10 && (millis() - _looptime_millis) > 5000){//loop at least 10 times and at least for 5s
      if(_enableDebugMessages){
        Serial.println("Going to sleep");
      }
      ESP.deepSleep(_deepsleeptime_minutes*60000000);
      delay(100);
    }
  }
  processDelayedExecutionRequests();
}

bool EspMQTTClient::handleWiFi()
{
  // When it's the first call, reset the wifi radio and schedule the wifi connection
  static bool firstLoopCall = true;
  if(_handleWiFi && firstLoopCall)
  {
    WiFi.disconnect(true);
    _nextWifiConnectionAttemptMillis = millis() + 500;
    firstLoopCall = false;
    return true;
  }

  // Get the current connection status
  bool isWifiConnected = (WiFi.status() == WL_CONNECTED);


  /***** Detect ans handle the current WiFi handling state *****/

  // Connection established
  if (isWifiConnected && !_wifiConnected)
  {
    onWiFiConnectionEstablished();
    _connectingToWifi = false;

    // At least 500 miliseconds of waiting before an mqtt connection attempt.
    // Some people have reported instabilities when trying to connect to
    // the mqtt broker right after being connected to wifi.
    // This delay prevent these instabilities.
    _nextMqttConnectionAttemptMillis = millis() + 500;
  }

  // Connection in progress
  else if(_connectingToWifi)
  {
      if((WiFi.status() == WL_CONNECT_FAILED || millis() - _lastWifiConnectionAttemptMillis >= _wifiReconnectionAttemptDelay) && 
          !_wifiManager.WifiAP_active(_max_uptime_server_minutes))
      {
        if(_enableDebugMessages)
          Serial.printf("WiFi! Connection attempt failed, delay expired. (%fs). \n", millis()/1000.0);

        WiFi.disconnect(true);
        MDNS.end();

        _nextWifiConnectionAttemptMillis = millis() + 500;
        _connectingToWifi = false;
      }
  }

  // Connection lost
  else if (!isWifiConnected && _wifiConnected)
  {
    onWiFiConnectionLost();

    if(_handleWiFi)
      _nextWifiConnectionAttemptMillis = millis() + 500;
  }

  // Connected since at least one loop() call
  else if (isWifiConnected && _wifiConnected)
  {
    // Web updater handling
    if (_httpServer != NULL)
    {
      _httpServer->handleClient();
      #ifdef ESP8266
        MDNS.update(); // We need to do this only for ESP8266
      #endif
    }

    if (_enableOTA)
      ArduinoOTA.handle();
  }

  // Disconnected since at least one loop() call
  // Then, if we handle the wifi reconnection process and the waiting delay has expired, we connect to wifi
  else if(_handleWiFi && _nextWifiConnectionAttemptMillis > 0 && millis() >= _nextWifiConnectionAttemptMillis)
  {
    connectToWifi();
    _nextWifiConnectionAttemptMillis = 0;
    _connectingToWifi = true;
    _lastWifiConnectionAttemptMillis = millis();
    _failedWifiConnectionAttemptCount++;
  }

  /**** Detect and return if there was a change in the WiFi state ****/

  if (isWifiConnected != _wifiConnected)
  {
    _wifiConnected = isWifiConnected;
    return true;
  }
  else
    return false;
}

bool EspMQTTClient::handleMQTT()
{
  // PubSubClient main loop() call
  _mqttClient.loop();

  // Get the current connextion status
  bool isMqttConnected = (isWifiConnected() && _mqttClient.connected());

  /***** Detect and handle the current MQTT handling state *****/

  // Connection established
  if (isMqttConnected && !_mqttConnected)
  {
    _mqttConnected = true;
    onMQTTConnectionEstablished();
  }

  // Connection lost
  else if (!isMqttConnected && _mqttConnected)
  {
    onMQTTConnectionLost();
    _nextMqttConnectionAttemptMillis = millis() + _mqttReconnectionAttemptDelay;
  }

  // It's time to connect to the MQTT broker
  else if (isWifiConnected() && _nextMqttConnectionAttemptMillis > 0 && millis() >= _nextMqttConnectionAttemptMillis)
  {
    // Connect to MQTT broker
    if(connectToMqttBroker())
    {
      _failedMQTTConnectionAttemptCount = 0;
      _nextMqttConnectionAttemptMillis = 0;
    }
    else
    {
      // Connection failed, plan another connection attempt
      _nextMqttConnectionAttemptMillis = millis() + _mqttReconnectionAttemptDelay;
      _mqttClient.disconnect();
      _failedMQTTConnectionAttemptCount++;

      if (_enableDebugMessages)
        Serial.printf("MQTT!: Failed MQTT connection count: %i \n", _failedMQTTConnectionAttemptCount);

      // When there is too many failed attempt, sometimes it help to reset the WiFi connection or to restart the board.
      if(_handleWiFi && _failedMQTTConnectionAttemptCount == 8)
      {
        if (_enableDebugMessages)
          Serial.println("MQTT!: Can't connect to broker after too many attempt, resetting WiFi ...");

        WiFi.disconnect(true);
        MDNS.end();
        _nextWifiConnectionAttemptMillis = millis() + 500;

        if(!_drasticResetOnConnectionFailures)
          _failedMQTTConnectionAttemptCount = 0;
      }
      else if(_drasticResetOnConnectionFailures && _failedMQTTConnectionAttemptCount == 12) // Will reset after 12 failed attempt (3 minutes of retry)
      {
        if (_enableDebugMessages)
          Serial.println("MQTT!: Can't connect to broker after too many attempt, resetting board ...");

        #ifdef ESP8266
          ESP.reset();
        #else
          ESP.restart();
        #endif
      }
    }
  }


  /**** Detect and return if there was a change in the MQTT state ****/

  if(_mqttConnected != isMqttConnected)
  {
    _mqttConnected = isMqttConnected;
    return true;
  }
  else
    return false;
}

void EspMQTTClient::onWiFiConnectionEstablished()
{
    _failedWifiConnectionAttemptCount = 0;
    if (_enableDebugMessages)
      Serial.printf("WiFi: Connected (%fs), ip : %s \n", millis()/1000.0, WiFi.localIP().toString().c_str());

    // Config of web updater
    if (_httpServer != NULL)
    {
      MDNS.begin(_mqttClientName);
      _httpUpdater->setup(_httpServer, _updateServerAddress, _updateServerUsername, _updateServerPassword);
      _httpServer->begin();
      MDNS.addService("http", "tcp", 80);

      if (_enableDebugMessages)
        Serial.printf("WEB: Updater ready, open http://%s.local in your browser and login with username '%s' and password '%s'.\n", _mqttClientName, _updateServerUsername, _updateServerPassword);
    }

    if (_enableOTA)
      ArduinoOTA.begin();
}

void EspMQTTClient::onWiFiConnectionLost()
{
  if (_enableDebugMessages)
    Serial.printf("WiFi! Lost connection (%fs). \n", millis()/1000.0);

  // If we handle wifi, we force disconnection to clear the last connection
  if (_handleWiFi)
  {
    WiFi.disconnect(true);
    MDNS.end();
  }
}

void EspMQTTClient::onMQTTConnectionEstablished()
{
  _connectionEstablishedCount++;
  if (_publish_Wifi_RSSI){
    publish("WIFI_RSSI",WiFi.RSSI(),false);
  }
  subscribe("OTA_Update",1, [this](const String &topicStr, byte* payload, unsigned int length) {this->OTA_via_MQTT_callback(topicStr, payload, length);});
  _connectionEstablishedCallback();
}

void EspMQTTClient::onMQTTConnectionLost()
{
  if (_enableDebugMessages)
  {
    Serial.printf("MQTT! Lost connection (%fs). \n", millis()/1000.0);
    Serial.printf("MQTT: Retrying to connect in %i seconds. \n", _mqttReconnectionAttemptDelay / 1000);
  }
}

// =============== Public functions for interaction with thus lib =================

bool EspMQTTClient::setMaxPacketSize(const uint16_t size)
{

  bool success = _mqttClient.setBufferSize(size);

  if(!success && _enableDebugMessages)
    Serial.println("MQTT! failed to set the max packet size.");

  return success;
}

bool EspMQTTClient::publish(const char* measurement, float data, bool persist, const char* custom_location)
{
  // Do not try to publish if MQTT is not connected.
  if(!isConnected())
  {
    if (_enableDebugMessages)
      Serial.println("MQTT! Trying to publish when disconnected, skipping.");

    return false;
  }

  const char* location = _mqttClientName;

  if(custom_location != NULL){
      location = custom_location;
  }

  String dummy = String(data);
  char to_send[dummy.length() + 1];
  dummy.toCharArray(to_send, dummy.length() + 1);
  char topic[strlen(location)+strlen(measurement)+6+1];
  strcpy(topic, "home/");
  strcat(topic, location);
  strcat(topic, "/");
  strcat(topic, measurement);
  const char* topic_mqtt = topic;

  bool success = _mqttClient.publish(topic_mqtt, to_send, persist);

  if (_enableDebugMessages)
  {
    if(success)
      Serial.printf("MQTT << [%s] %s\n", topic_mqtt, to_send);
    else
      Serial.println("MQTT! publish failed, is the message too long ? (see setMaxPacketSize())"); // This can occurs if the message is too long according to the maximum defined in PubsubClient.h
      Serial.println(topic_mqtt);
      Serial.println(to_send);
  }
  return success;
}

bool EspMQTTClient::subscribe(const char* measurement,  int qos, MessageReceivedCallbackWithTopic messageReceivedCallbackWithTopic, const char* custom_location)
{
  // Do not try to subscribe if MQTT is not connected.
  if(!isConnected())
  {
    if (_enableDebugMessages)
      Serial.println("MQTT! Trying to subscribe when disconnected, skipping.");

    return false;
  }

  const char* location = _mqttClientName;

  if(custom_location != NULL){
      location = custom_location;
  }

  char topic[strlen(location)+strlen(measurement)+6+1];
  strcpy(topic, "home/");
  strcat(topic, location);
  strcat(topic, "/");
  strcat(topic, measurement);

  bool success = _mqttClient.subscribe(topic, qos);

  if(success)
  {
    // Add the record to the subscription list only if it does not exists.
    bool found = false;
    for (std::size_t i = 0; i < _topicSubscriptionList.size() && !found; i++)
      found = _topicSubscriptionList[i].topic.equals(topic);

    if(!found)
      _topicSubscriptionList.push_back({ topic, NULL, messageReceivedCallbackWithTopic });
  }

  if (_enableDebugMessages)
  {
    if(success)
      Serial.printf("MQTT: Subscribed to [%s]\n", topic);
    else
      Serial.println("MQTT! subscribe failed");
  }

  return success;
}

bool EspMQTTClient::unsubscribe(const String &topic)
{
  // Do not try to unsubscribe if MQTT is not connected.
  if(!isConnected())
  {
    if (_enableDebugMessages)
      Serial.println("MQTT! Trying to unsubscribe when disconnected, skipping.");

    return false;
  }

  for (std::size_t i = 0; i < _topicSubscriptionList.size(); i++)
  {
    if (_topicSubscriptionList[i].topic.equals(topic))
    {
      if(_mqttClient.unsubscribe(topic.c_str()))
      {
        _topicSubscriptionList.erase(_topicSubscriptionList.begin() + i);
        i--;

        if(_enableDebugMessages)
          Serial.printf("MQTT: Unsubscribed from %s\n", topic.c_str());
      }
      else
      {
        if(_enableDebugMessages)
          Serial.println("MQTT! unsubscribe failed");

        return false;
      }
    }
  }

  return true;
}

void EspMQTTClient::setKeepAlive(uint16_t keepAliveSeconds)
{
  _mqttClient.setKeepAlive(keepAliveSeconds);
}

void EspMQTTClient::executeDelayed(const unsigned long delay, DelayedExecutionCallback callback)
{
  DelayedExecutionRecord delayedExecutionRecord;
  delayedExecutionRecord.targetMillis = millis() + delay;
  delayedExecutionRecord.callback = callback;

  _delayedExecutionList.push_back(delayedExecutionRecord);
}

float EspMQTTClient::payload_to_float(byte* payload, unsigned int length){
    float result = 0;
    int dezimal = 0;
    for(unsigned int i=0;i<length;i++){
        char c = payload[i];
        // Serial.println(c);
        if (c >= '0' && c <= '9'){
            if(dezimal == 0){
                result = result*10 + (c - '0');
            }
            else{
                result = result + (c - '0')*pow(0.1,dezimal);
                dezimal++;
            }
        }
        else if ( c == '.' || c ==','){
            dezimal = 1;
        }
        else {
            Serial.print ((float)c);
            Serial.println(" war so nicht erwartet");
            return(-1);
        }
    }
    return(result);
}

void EspMQTTClient::OTA_via_MQTT_callback(const String &topicStr, byte* payload, unsigned int length){
  if (payload_to_float(payload,length) == 1){
    _OTA_via_MQTT = true;
    if(_enableDebugMessages){
      Serial.println("OTA via MQTT enabled");
    }
  }
  else{
    _OTA_via_MQTT = false; //important! otherwise, it will be in OTA mode forever even if deactivated via MQTT 
    if(_enableDebugMessages){
      Serial.println("OTA via MQTT disabled");
    }
  }
}

// ================== Private functions ====================-

// Initiate a Wifi connection (non-blocking)
void EspMQTTClient::connectToWifi()
{
  if(!_wifiManager.WifiAP_active(_max_uptime_server_minutes)){
    WiFi.mode(WIFI_STA);
    _wifiManager.setConfigPortalBlocking(false);
    _wifiManager.setDebugOutput(true);
    // _wifiManager.setConfigPortalTimeout(60); //timeout in sekunden, danach geht code einfach weiter (ohne internet)
    _wifiManager.autoConnect(_mqttClientName,"MeinWifiManagerPasswort");
    if (_enableDebugMessages)
      Serial.printf("\nWiFi: Connecting to WIFI... (%fs) \n", millis()/1000.0);
  }
}

// Try to connect to the MQTT broker and return True if the connection is successfull (blocking)
bool EspMQTTClient::connectToMqttBroker()
{
  bool success = false;

  if (_mqttServerIp != nullptr && strlen(_mqttServerIp) > 0)
  {
    if (_enableDebugMessages)
    {
      if (_mqttUsername)
        Serial.printf("MQTT: Connecting to broker \"%s\" with client name \"%s\" and username \"%s\" ... (%fs)", _mqttServerIp, _mqttClientName, _mqttUsername, millis()/1000.0);
      else
        Serial.printf("MQTT: Connecting to broker \"%s\" with client name \"%s\" ... (%fs)", _mqttServerIp, _mqttClientName, millis()/1000.0);
    }

    // explicitly set the server/port here in case they were not provided in the constructor
    _mqttClient.setServer(_mqttServerIp, _mqttServerPort);
    success = _mqttClient.connect(_mqttClientName, _mqttUsername, _mqttPassword, _mqttLastWillTopic, 0, _mqttLastWillRetain, _mqttLastWillMessage, _mqttCleanSession);
  }
  else
  {
    if (_enableDebugMessages)
      Serial.printf("MQTT: Broker server ip is not set, not connecting (%fs)\n", millis()/1000.0);
    success = false;
  }

  if (_enableDebugMessages)
  {
    if (success)
      Serial.printf(" - ok. (%fs) \n", millis()/1000.0);
    else
    {
      Serial.printf("unable to connect (%fs), reason: ", millis()/1000.0);

      switch (_mqttClient.state())
      {
        case -4:
          Serial.println("MQTT_CONNECTION_TIMEOUT");
          break;
        case -3:
          Serial.println("MQTT_CONNECTION_LOST");
          break;
        case -2:
          Serial.println("MQTT_CONNECT_FAILED");
          break;
        case -1:
          Serial.println("MQTT_DISCONNECTED");
          break;
        case 1:
          Serial.println("MQTT_CONNECT_BAD_PROTOCOL");
          break;
        case 2:
          Serial.println("MQTT_CONNECT_BAD_CLIENT_ID");
          break;
        case 3:
          Serial.println("MQTT_CONNECT_UNAVAILABLE");
          break;
        case 4:
          Serial.println("MQTT_CONNECT_BAD_CREDENTIALS");
          break;
        case 5:
          Serial.println("MQTT_CONNECT_UNAUTHORIZED");
          break;
      }

      Serial.printf("MQTT: Retrying to connect in %i seconds.\n", _mqttReconnectionAttemptDelay / 1000);
    }
  }

  return success;
}

// Delayed execution handling.
// Check if there is delayed execution requests to process and execute them if needed.
void EspMQTTClient::processDelayedExecutionRequests()
{
  if (_delayedExecutionList.size() > 0)
  {
    unsigned long currentMillis = millis();

    for (std::size_t i = 0 ; i < _delayedExecutionList.size() ; i++)
    {
      if (_delayedExecutionList[i].targetMillis <= currentMillis)
      {
        _delayedExecutionList[i].callback();
        _delayedExecutionList.erase(_delayedExecutionList.begin() + i);
        i--;
      }
    }
  }
}

/**
 * Matching MQTT topics, handling the eventual presence of wildcards character
 * It doesn't validate the correctness of the topic pattern.
 *
 * @param topic1 may contain wildcards (+, #)
 * @param topic2 must not contain wildcards
 * @return true on MQTT topic match, false otherwise
 */
bool EspMQTTClient::mqttTopicMatch(const String &topic1, const String &topic2)
{
  const char *topic1_p = topic1.begin();
  const char *topic1_end = topic1.end();
  const char *topic2_p = topic2.begin();
  const char *topic2_end = topic2.end();

  while (topic1_p < topic1_end && topic2_p < topic2_end)
  {
    if (*topic1_p == '#')
    {
      // we assume '#' can be present only at the end of the topic pattern
      return true;
    }

    if (*topic1_p == '+')
    {
      // move to the end of the matched section (till next '/' if any, otherwise the end of text)
      const char *temp = strchr(topic2_p, '/');
      if (temp)
        topic2_p = temp;
      else
        topic2_p = topic2_end;

      ++topic1_p;
      continue;
    }

    // find the end of current section, it is either before next wildcard or at the end of text
    const char* temp = strchr(topic1_p, '+');
    int len = temp == NULL ? topic1_end - topic1_p : temp - topic1_p;
    if (topic1_p[len - 1] == '#')
      --len;

    if (topic2_end - topic2_p < len)
      return false;

    if (strncmp(topic1_p, topic2_p, len))
      return false;

    topic1_p += len;
    topic2_p += len;
  }

  // Check if there is any remaining characters not matched
  return !(topic1_p < topic1_end || topic2_p < topic2_end);
}

void EspMQTTClient::mqttMessageReceivedCallback(char* topic, byte* payload, unsigned int length)
{
  // Convert the payload into a String
  // First, We ensure that we dont bypass the maximum size of the PubSubClient library buffer that originated the payload
  // This buffer has a maximum length of _mqttClient.getBufferSize() and the payload begin at "headerSize + topicLength + 1"
  unsigned int strTerminationPos;
  if (strlen(topic) + length + 9 >= _mqttClient.getBufferSize())
  {
    strTerminationPos = length - 1;

    if (_enableDebugMessages)
      Serial.print("MQTT! Your message may be truncated, please set setMaxPacketSize() to a higher value.\n");
  }
  else
    strTerminationPos = length;

  // Second, we add the string termination code at the end of the payload and we convert it to a String object
  payload[strTerminationPos] = '\0';
  String payloadStr((char*)payload);
  String topicStr(topic);

  // Logging
  if (_enableDebugMessages)
    Serial.printf("MQTT >> [%s] %s\n", topic, payloadStr.c_str());

  // Send the message to subscribers
  for (std::size_t i = 0 ; i < _topicSubscriptionList.size() ; i++)
  {
    if (mqttTopicMatch(_topicSubscriptionList[i].topic, String(topic)))
    {
      if(_topicSubscriptionList[i].callback != NULL)
        _topicSubscriptionList[i].callback(payloadStr); // Call the callback
      if(_topicSubscriptionList[i].callbackWithTopic != NULL)
        _topicSubscriptionList[i].callbackWithTopic(topicStr, payload, length); // Call the callback
    }
  }
}
