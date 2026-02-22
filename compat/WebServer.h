/**
 * When USE_HTTPS is defined, replace Arduino WebServer with ESPWebServerSecure
 * from esp32_https_server_compat so AutoConnect runs natively over HTTPS.
 * Include path -Icompat must come before the framework path.
 */
#if defined(USE_HTTPS) && defined(ARDUINO_ARCH_ESP32)
#ifndef COMPAT_WEBSERVER_H_
#define COMPAT_WEBSERVER_H_
#include <ESPWebServer.hpp>
#include <ESPWebServerSecure.hpp>
typedef ESPWebServerSecure WebServer;
#endif /* COMPAT_WEBSERVER_H_ */
#else
#include_next <WebServer.h>
#endif
