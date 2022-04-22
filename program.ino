#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <Arduino_JSON.h>
#include <ModbusMaster.h>
#include <SoftwareSerial.h>

#define METER_VALUE_COUNT      6
#define PLUG_COUNT             5
#define PLUG_POWER_THRESHOLD   10
#define PLUG_MEASUREMENT_COUNT 4

#define MAX485_TX_ENABLE_PIN   D2
#define MAX485_RX_PIN          D5
#define MAX485_TX_PIN          D6
#define MODBUS_ADDRESS         1
#define MODBUS_READ_DELAY      500

#define LED_PIN                D1

#define WIFI_SLEEP_SECONDS     6
#define NO_WIFI_SLEEP_MINUTES  15
#define COUNTER_MAX            10 // 6 * 10 = 60 sec.

const char* ssid = "";
const char* password = "";

const char* plug_base_ip = "http://192.168.178.10";
const char* plug_status_path = "/cm?cmnd=status%200";
const char* plug_power_path = "/cm?cmnd=Power%20";

const char* cloud_url = "http://x.x.x.x:8086/write?db=meter";
const char* cloud_authorization = "Basic CODE=";

const char* meter_source = "max485";
const char* plug_source = "tasmota";

struct measurement {
  String name = "";
  const char* source = "";
  int value = 0;
  bool error = false;
};

measurement measurements[METER_VALUE_COUNT + PLUG_COUNT * PLUG_MEASUREMENT_COUNT + 1]; // + 1 for plug_power_sum
measurement old_measurements[METER_VALUE_COUNT + PLUG_COUNT * PLUG_MEASUREMENT_COUNT + 1];

// measurement counter
byte m_c = 0;
// old measurement counter
byte o_m_c = 0;

byte counter = 0;

ModbusMaster dts238;
SoftwareSerial softSerial;

void preTransmission() {
  digitalWrite(MAX485_TX_ENABLE_PIN, HIGH);
  digitalWrite(LED_PIN, HIGH);
}

void postTransmission() {
  digitalWrite(MAX485_TX_ENABLE_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Hello");
  softSerial.begin(2400, SWSERIAL_8N1, MAX485_RX_PIN, MAX485_TX_PIN, false);

  if (!softSerial) {
    Serial.println("Invalid SoftwareSerial pin configuration, check config"); 
    while (1) {
      delay (1000);
    }
  } 

  Serial.println("set pins");
  pinMode(MAX485_TX_ENABLE_PIN, OUTPUT);
  digitalWrite(MAX485_TX_ENABLE_PIN, LOW);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println("start modbus serial");
  dts238.begin(MODBUS_ADDRESS, softSerial);
  dts238.preTransmission(preTransmission);
  dts238.postTransmission(postTransmission);

  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED) {
    Serial.println("wait for wifi");
    delay(500);
  }
  Serial.println("wifi found");

  delay(1000);
}

void loop() {
  if(WiFi.status() != WL_CONNECTED) {
    //delay(NO_WIFI_SLEEP_MINUTES * 60000);
    ESP.deepSleep(NO_WIFI_SLEEP_MINUTES * 6e7);

    return;
  }

  for (byte i = 0; i < m_c; i++) {
    measurements[i] = {};
  }
  
  m_c = 0;
  counter++;

  readMeterPower();
  readPlugs();
  bool switchedPlugs = switchPlugs();

  if (switchedPlugs || counter >= COUNTER_MAX) {
    counter = 0;

    readMeterWork();
    sendMeasurements();
  }

  delay(WIFI_SLEEP_SECONDS * 1000);
}

void readMeterPower() {
  delay(MODBUS_READ_DELAY);
  byte power_result = dts238.readInputRegisters(0x0088, 3);
  
  if (power_result != 0) {
    addMeasurement("meter_power_error", meter_source, power_result, true);
    
    return;
  }

  int power_sum = 0;
  
  for (byte i = 0; i < 3; i++) {
    int value = dts238.getResponseBuffer(i);

    if (value > 20000) {
      value = value - 65535;
    }

    addMeasurement(String("meter_power_")+ (i + 1), meter_source, value);      
    power_sum += value;
  }

  addMeasurement("meter_power_sum", meter_source, power_sum);
}

void readPlugs() {
  WiFiClient client;
  HTTPClient http;

  int plugs_sum = 0;

  for (byte i = 0; i < PLUG_COUNT; i++) {
    char plug_status_url[42] = "";
    sprintf(plug_status_url, "%s%i%s", plug_base_ip, i, plug_status_path);
      
    http.begin(client, plug_status_url);
    int http_response_code = http.GET();
    
    if (http_response_code == 200) {
      String payload = http.getString();
      JSONVar plug_json = JSON.parse(payload);
  
      if (JSON.typeof(plug_json) != "undefined") {
        int power_value = plug_json["StatusSNS"]["ENERGY"]["Power"];

        if (i > 0) {
          plugs_sum += power_value;
        }
        
        addMeasurement(String("plug_") + i + "_status", plug_source, plug_json["Status"]["Power"]); 
        addMeasurement(String("plug_") + i + "_led_state", plug_source, plug_json["Status"]["LedState"]); 
        addMeasurement(String("plug_") + i + "_power", plug_source, power_value); 
        addMeasurement(String("plug_") + i + "_work", plug_source, plug_json["StatusSNS"]["ENERGY"]["Total"]); 
      }    
    } else { 
      addMeasurement(String("plug_") + i + "_read_status_error", plug_source, http_response_code, true);
    }
  
    http.end();
  }

  addMeasurement("plugs_power_sum", plug_source, plugs_sum); 
}

bool switchPlugs() {
  WiFiClient client;
  HTTPClient http;

  int power_sum = getMeasurementValue("meter_power_sum");
  int i = -1;
  int power_status = -1;

  if (power_sum == 0 || power_sum == -1) {
    return false;
  } else if (power_sum < 0) { // einschalten
    for (byte p = 1; p < PLUG_COUNT; p++) {
      if (getMeasurementValue(String("plug_") + p + "_status") == 0) {
        i = p;
        power_status = 1;

        break;
      }
    }
  } else { // ausschalten
    bool further_plug_status = false;
    for (byte p = PLUG_COUNT - 1; p > 0; p--) {
      int plug_status = getMeasurementValue(String("plug_") + p + "_status");
      int plug_power = getMeasurementValue(String("plug_") + p + "_power");
      // if plug is offline, plug is off, plug power < threshold or led state is 1
      if (plug_status == -1 || plug_status == 0 || plug_power < PLUG_POWER_THRESHOLD ||
          getMeasurementValue(String("plug_") + p + "_led_state") == 1) {
        continue;
      }

      if (!further_plug_status && power_sum - plug_power > PLUG_POWER_THRESHOLD) {
        i = p;
        power_status = 0;

        break;
      }

      further_plug_status = true;
    }
  }

  if (i == -1 || power_status == -1) {
    return false;
  }

  char plug_power_url[41] = "";
  sprintf(plug_power_url, "%s%i%s%i", plug_base_ip, i, plug_power_path, power_status);
  
  http.begin(client, plug_power_url);
  int http_response_code = http.GET();
  http.end();

  if (http_response_code != 200) {
    addMeasurement(String("plug_") + i + "_switch_error", plug_source, http_response_code, true);

    return false;
  }

  return true;
}

void readMeterWork() {
  delay(MODBUS_READ_DELAY);
  byte work_result = dts238.readInputRegisters(0x0008, 4);

  if (work_result == 0) {
    addMeasurement(
      "meter_work_in",
      meter_source,
      calculateWork(dts238.getResponseBuffer(2), dts238.getResponseBuffer(3))
    );
    addMeasurement(
      "meter_work_out",
      meter_source,
      calculateWork(dts238.getResponseBuffer(0), dts238.getResponseBuffer(1))
    );
  } else {
    addMeasurement("meter_work_error", meter_source, work_result, true);
  }
}

int calculateWork(byte value1, int value2) {
  return ((value1 * 65535) + value2) / 100;
}

void addMeasurement(String name, const char* source, int value) {
  addMeasurement(name, source, value, false);
}

void addMeasurement(String name, const char* source, int value, bool error) {
  measurements[m_c].name = name;
  measurements[m_c].source = source;
  measurements[m_c].value = value;
  measurements[m_c].error = error;
  m_c++;
}

int getMeasurementValue(String name) {
  for (byte i = 0; i < m_c; i++) {
    if (measurements[i].name == name) {
      return measurements[i].value;
    }
  }

  return -1;
}

int getOldMeasurementValue(String name) {
  for (byte i = 0; i < o_m_c; i++) {
    if (old_measurements[i].name == name) {
      return old_measurements[i].value;
    }
  }

  return -1;
}

void sendMeasurements() {
  WiFiClient client;
  HTTPClient http;

  http.begin(client, cloud_url);
  http.addHeader("Authorization", cloud_authorization);

  String data = "";

  for (byte i = 0; i < m_c; i++) {

    // don't send not changed values, but not errors
    if (!measurements[i].error) {
      int old_value = getOldMeasurementValue(measurements[i].name);
  
      if (measurements[i].value == old_value) {
        continue;
      }
    }

    data += "\n";
    data += measurements[i].name;
    data += ",source=";
    data += measurements[i].source;
    data += " value=";
    data += measurements[i].value;

    old_measurements[i] = measurements[i];
  }

  o_m_c = m_c;

  Serial.println(data);
  
  http.POST(data);
  http.end();
}
