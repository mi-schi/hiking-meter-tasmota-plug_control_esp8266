# hiking-meter-tasmota-plug_control_esp8266

## needed components

- DTS238 hiking power meter
- ESP8266 (like Wemos)
- MAX485
- tasmota plugs (like Gosound SP1)

## needed cloud components

- influx
- grafana

## How to install it?

1. set up grafana and influx (for example in the google cloud with the free E1 instance)
5. activate an external IP and open the needed port 8086, secure your cloud interfaces
2. use the arduino IDE and add ESP support
3. adapt the first lines in the program code for Wifi, cloud ip, cloud authorization
4. flash your ESP

## what does the program do?

This program reads the power values for all three phases via RS485 modbus from the hiking DTS238 power meter. Then it reads the plug data like work, power and status values via REST. After that it switches the plugs on or off via REST too.

At least every minute the program read the work values from the meter and send all the information to the influx database.

## result

![Bildschirmfoto von 2022-04-22 15-12-07](https://user-images.githubusercontent.com/8821732/164721061-cfbb0895-d3ee-4c06-bdce-d930c8778cbe.png)
![Bildschirmfoto von 2022-04-22 15-10-41](https://user-images.githubusercontent.com/8821732/164720895-a6a57864-42ae-44e9-b58a-d7a8d94b8c65.png)
