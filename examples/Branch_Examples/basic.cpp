#include "EspMQTTClient.h"

// !!!IMPORTANT!!! => CUSTOM WIFI MANAGER PACKAGE REQUIRED!!!

EspMQTTClient client("TestClient"); //location name

bool received = false;
unsigned long last_call = 0;

void setup()
{
  Serial.begin(115200);
  client.enableDebuggingMessages(); // Enable debugging messages sent to serial output
//   client.enableHTTPWebUpdater(); // Enable the web updater. User and password default to values of MQTTUsername and MQTTPassword. These can be overridded with enableHTTPWebUpdater("user", "password").
  client.enableOTA(); // Enable OTA (Over The Air) updates. Password defaults to MQTTPassword. Port is the default OTA port. Can be overridden with enableOTA("password", port).
  client.enable_publish_Wifi_RSSI();
}

void print_mqtt(const String &topicStr, byte* payload, unsigned int length){
  Serial.println(topicStr);
}

void print_strom(const String &topicStr, byte* payload, unsigned int length){
  Serial.println(client.payload_to_float(payload, length));
  client.go_to_sleep(1);
}

// This function is called once everything is connected (Wifi and MQTT)
// WARNING : YOU MUST IMPLEMENT IT IF YOU USE EspMQTTClient
void onConnectionEstablished()
{
  // client.publish("depp",1,false);
  client.subscribe("measurement1",1,print_mqtt);
  client.subscribe("measurement2",1,print_strom);
}

void loop()
{
  client.loop(); //loop am Ende, dann kann der readout auch im loop sein (connection wird erst danach gemacht)
}