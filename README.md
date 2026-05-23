# lights_server

ESP-IDF firmware for an ESP32-C3 that exposes a small Wi-Fi light-control API.

The board starts a Wi-Fi SoftAP named `lights`, serves HTTP requests, reads a
button on GPIO10, and drives an LED on GPIO8.

## HTTP API

Connect to the `lights` Wi-Fi network, then use:

```sh
curl http://192.168.4.1/lights
curl -X POST "http://192.168.4.1/lights?state=1"
curl -X POST "http://192.168.4.1/lights?state=0"
```

`GET /lights` returns `0` or `1`. `POST /lights?state=...` updates the light
state.

## Build and Flash

```sh
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with your ESP32-C3 serial port, for example `/dev/ttyACM0`.
