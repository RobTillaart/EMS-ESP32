/*
 * MyESP - my ESP helper class to handle WiFi, MQTT and Telnet
 * 
 * Paul Derbyshire - first revision: December 2018
 *
 * Ideas borrowed from Espurna https://github.com/xoseperez/espurna
 */

#include "MyESP.h"

#ifdef CRASH
EEPROM_Rotate EEPROMr;
#endif

union system_rtcmem_t {
    struct {
        uint8_t stability_counter;
        uint8_t reset_reason;
        uint8_t boot_status;
        uint8_t _reserved_;
    } parts;
    uint32_t value;
};

uint8_t RtcmemSize = (sizeof(RtcmemData) / 4u);
auto    Rtcmem     = reinterpret_cast<volatile RtcmemData *>(RTCMEM_ADDR);

// constructor
MyESP::MyESP() {
    _app_hostname = strdup("MyESP");
    _app_name     = strdup("MyESP");
    _app_version  = strdup(MYESP_VERSION);

    _boottime     = NULL;
    _load_average = 100; // calculated load average

    _telnetcommand_callback = NULL;
    _telnet_callback        = NULL;

    _command[0] = '\0';

    _fs_callback          = NULL;
    _fs_settings_callback = NULL;

    _web_callback = NULL;

    _serial = false;

    _heartbeat                 = false;
    _mqtt_host                 = NULL;
    _mqtt_password             = NULL;
    _mqtt_username             = NULL;
    _mqtt_retain               = false;
    _mqtt_keepalive            = 300;
    _mqtt_will_topic           = NULL;
    _mqtt_will_online_payload  = NULL;
    _mqtt_will_offline_payload = NULL;
    _mqtt_base                 = NULL;
    _mqtt_topic                = NULL;
    _mqtt_qos                  = 0;
    _mqtt_reconnect_delay      = MQTT_RECONNECT_DELAY_MIN;
    _mqtt_last_connection      = 0;
    _mqtt_connecting           = false;

    _firstInstall = false;

    _wifi_password  = NULL;
    _wifi_ssid      = NULL;
    _wifi_callback  = NULL;
    _wifi_connected = false;

    _ota_pre_callback  = NULL;
    _ota_post_callback = NULL;
    _ota_doing_update  = false;

    _suspendOutput = false;

    _rtcmem_status = false;
    _systemStable  = true;
}

MyESP::~MyESP() {
    end();
}

// end
void MyESP::end() {
    SerialAndTelnet.end();
    jw.disconnect();
}

// general debug to the telnet or serial channels
void MyESP::myDebug(const char * format, ...) {
    if (_suspendOutput)
        return;

    va_list args;
    va_start(args, format);
    char test[1];

    int len = ets_vsnprintf(test, 1, format, args) + 1;

    char * buffer = new char[len];
    ets_vsnprintf(buffer, len, format, args);
    va_end(args);

    SerialAndTelnet.println(buffer);

    delete[] buffer;
}

// for flashmemory. Must use PSTR()
void MyESP::myDebug_P(PGM_P format_P, ...) {
    if (_suspendOutput)
        return;

    char format[strlen_P(format_P) + 1];
    memcpy_P(format, format_P, sizeof(format));

    va_list args;
    va_start(args, format_P);
    char test[1];
    int  len = ets_vsnprintf(test, 1, format, args) + 1;

    char * buffer = new char[len];
    ets_vsnprintf(buffer, len, format, args);

    va_end(args);

#ifdef MYESP_TIMESTAMP
    // capture & print timestamp
    char timestamp[10] = {0};
    snprintf_P(timestamp, sizeof(timestamp), PSTR("[%06lu] "), millis() % 1000000);
    SerialAndTelnet.print(timestamp);
#endif

    SerialAndTelnet.println(buffer);

    delete[] buffer;
}

// use Serial?
bool MyESP::getUseSerial() {
    return (_serial);
}

// heartbeat
bool MyESP::getHeartbeat() {
    return (_heartbeat);
}

// init heap ram
uint32_t MyESP::_getInitialFreeHeap() {
    static uint32_t _heap = 0;

    if (0 == _heap) {
        _heap = ESP.getFreeHeap();
    }

    return _heap;
}

// used heap mem
// note calls to getFreeHeap sometimes causes some ESPs to crash
uint32_t MyESP::_getUsedHeap() {
    return _getInitialFreeHeap() - ESP.getFreeHeap();
}

// called when WiFi is connected, and used to start OTA, MQTT
void MyESP::_wifiCallback(justwifi_messages_t code, char * parameter) {
    if ((code == MESSAGE_CONNECTED)) {
#if defined(ARDUINO_ARCH_ESP32)
        String hostname = String(WiFi.getHostname());
#else
        String hostname = WiFi.hostname();
#endif

        myDebug_P(PSTR("[WIFI] SSID  %s"), WiFi.SSID().c_str());
        myDebug_P(PSTR("[WIFI] CH    %d"), WiFi.channel());
        myDebug_P(PSTR("[WIFI] RSSI  %d"), WiFi.RSSI());
        myDebug_P(PSTR("[WIFI] IP    %s"), WiFi.localIP().toString().c_str());
        myDebug_P(PSTR("[WIFI] MAC   %s"), WiFi.macAddress().c_str());
        myDebug_P(PSTR("[WIFI] GW    %s"), WiFi.gatewayIP().toString().c_str());
        myDebug_P(PSTR("[WIFI] MASK  %s"), WiFi.subnetMask().toString().c_str());
        myDebug_P(PSTR("[WIFI] DNS   %s"), WiFi.dnsIP().toString().c_str());
        myDebug_P(PSTR("[WIFI] HOST  %s"), hostname.c_str());

        // start OTA
        ArduinoOTA.begin(); // moved to support esp32
        myDebug_P(PSTR("[OTA] listening to %s.local:%u"), ArduinoOTA.getHostname().c_str(), OTA_PORT);

        // MQTT Setup
        _mqtt_setup();

        _wifi_connected = true;

        // finally if we don't want Serial anymore, turn it off
        if (!_serial) {
            myDebug_P(PSTR("[SYSTEM] Disabling serial port communication."));
            SerialAndTelnet.flush(); // flush so all buffer is printed to serial
            SerialAndTelnet.setSerial(NULL);
        }

        // call any final custom settings
        if (_wifi_callback) {
            _wifi_callback();
        }

        jw.enableAPFallback(false); // Disable AP mode after initial connect was successful
    }

    if (code == MESSAGE_ACCESSPOINT_CREATED) {
        _wifi_connected = true;

        myDebug_P(PSTR("[WIFI] MODE AP"));
        myDebug_P(PSTR("[WIFI] SSID  %s"), jw.getAPSSID().c_str());
        myDebug_P(PSTR("[WIFI] IP    %s"), WiFi.softAPIP().toString().c_str());
        myDebug_P(PSTR("[WIFI] MAC   %s"), WiFi.softAPmacAddress().c_str());

        // finally if we don't want Serial anymore, turn it off
        if (!_serial) {
            myDebug_P(PSTR("[SYSTEM] Disabling serial port communication."));
            SerialAndTelnet.flush(); // flush so all buffer is printed to serial
            SerialAndTelnet.setSerial(NULL);
        }

        // call any final custom settings
        if (_wifi_callback) {
            _wifi_callback();
        }
    }

    if (code == MESSAGE_CONNECTING) {
        myDebug_P(PSTR("[WIFI] Connecting to %s"), parameter);
        _wifi_connected = false;
    }

    if (code == MESSAGE_CONNECT_FAILED) {
        myDebug_P(PSTR("[WIFI] Could not connect to %s"), parameter);
        _wifi_connected = false;
    }

    if (code == MESSAGE_DISCONNECTED) {
        myDebug_P(PSTR("[WIFI] Disconnected"));
        _wifi_connected = false;
    }

    if (code == MESSAGE_SCANNING) {
        myDebug_P(PSTR("[WIFI] Scanning"));
    }

    if (code == MESSAGE_SCAN_FAILED) {
        myDebug_P(PSTR("[WIFI] Scan failed"));
    }

    if (code == MESSAGE_NO_NETWORKS) {
        myDebug_P(PSTR("[WIFI] No networks found"));
    }

    if (code == MESSAGE_NO_KNOWN_NETWORKS) {
        myDebug_P(PSTR("[WIFI] No known networks found"));
    }

    if (code == MESSAGE_FOUND_NETWORK) {
        myDebug_P(PSTR("[WIFI] %s"), parameter);
    }

    if (code == MESSAGE_CONNECT_WAITING) {
        // too much noise
    }

    if (code == MESSAGE_ACCESSPOINT_CREATING) {
        myDebug_P(PSTR("[WIFI] Creating access point"));
    }

    if (code == MESSAGE_ACCESSPOINT_FAILED) {
        myDebug_P(PSTR("[WIFI] Could not create access point"));
    }
}

// return true if in WiFi AP mode
// does not work after wifi reset on ESP32 yet. See https://github.com/espressif/arduino-esp32/issues/1306
bool MyESP::isAPmode() {
    return (WiFi.getMode() & WIFI_AP);
}

// received MQTT message
// we send this to the call back function. Important to parse are the event strings such as MQTT_MESSAGE_EVENT and MQTT_CONNECT_EVENT
void MyESP::_mqttOnMessage(char * topic, char * payload, size_t len) {
    if (len == 0)
        return;

    char message[len + 1];
    strlcpy(message, (char *)payload, len + 1);

    // myDebug_P(PSTR("[MQTT] Received %s => %s"), topic, message); // enable for debugging

    // topics are in format MQTT_BASE/HOSTNAME/TOPIC
    char * topic_magnitude = strrchr(topic, '/'); // strip out everything until last /
    if (topic_magnitude != nullptr) {
        topic = topic_magnitude + 1;
    }

    // check for standard messages
    // Restart the device
    if (strcmp(topic, MQTT_TOPIC_RESTART) == 0) {
        myDebug_P(PSTR("[MQTT] Received restart command"), message);
        resetESP();
        return;
    }

    // handle response from a start message
    // for example with HA it sends the system time from the server
    if (strcmp(topic, MQTT_TOPIC_START) == 0) {
        myDebug_P(PSTR("[MQTT] Received boottime: %s"), message);
        setBoottime(message);
        return;
    }

    // Send message event to custom service
    (_mqtt_callback)(MQTT_MESSAGE_EVENT, topic, message);
}

// MQTT subscribe
// to MQTT_BASE/app_hostname/topic
void MyESP::mqttSubscribe(const char * topic) {
    if (mqttClient.connected() && (strlen(topic) > 0)) {
        unsigned int packetId = mqttClient.subscribe(_mqttTopic(topic), _mqtt_qos);
        myDebug_P(PSTR("[MQTT] Subscribing to %s (PID %d)"), _mqttTopic(topic), packetId);
    }
}

// MQTT unsubscribe
// to MQTT_BASE/app_hostname/topic
void MyESP::mqttUnsubscribe(const char * topic) {
    if (mqttClient.connected() && (strlen(topic) > 0)) {
        unsigned int packetId = mqttClient.unsubscribe(_mqttTopic(topic));
        myDebug_P(PSTR("[MQTT] Unsubscribing to %s (PID %d)"), _mqttTopic(topic), packetId);
    }
}

// MQTT Publish
void MyESP::mqttPublish(const char * topic, const char * payload) {
    // myDebug_P(PSTR("[MQTT] Sending pubish to %s with payload %s"), _mqttTopic(topic), payload);
    mqttClient.publish(_mqttTopic(topic), _mqtt_qos, _mqtt_retain, payload);
}

// MQTT onConnect - when a connect is established
void MyESP::_mqttOnConnect() {
    myDebug_P(PSTR("[MQTT] Connected"));
    _mqtt_reconnect_delay = MQTT_RECONNECT_DELAY_MIN;

    _mqtt_last_connection = millis();

    // say we're alive to the Last Will topic
    mqttClient.publish(_mqttTopic(_mqtt_will_topic), 1, true, _mqtt_will_online_payload);

    // subscribe to general subs
    mqttSubscribe(MQTT_TOPIC_RESTART);

    // subscribe to a start message and send the first publish
    myESP.mqttSubscribe(MQTT_TOPIC_START);
    myESP.mqttPublish(MQTT_TOPIC_START, MQTT_TOPIC_START_PAYLOAD);

    // call custom function to handle mqtt receives
    (_mqtt_callback)(MQTT_CONNECT_EVENT, NULL, NULL);
}

// MQTT setup
void MyESP::_mqtt_setup() {
    if (!_mqtt_host) {
        myDebug_P(PSTR("[MQTT] is disabled"));
    }

    mqttClient.onConnect([this](bool sessionPresent) { _mqttOnConnect(); });

    mqttClient.onDisconnect([this](AsyncMqttClientDisconnectReason reason) {
        if (reason == AsyncMqttClientDisconnectReason::TCP_DISCONNECTED) {
            myDebug_P(PSTR("[MQTT] TCP Disconnected"));
            (_mqtt_callback)(MQTT_DISCONNECT_EVENT, NULL, NULL); // call callback with disconnect
        }
        if (reason == AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED) {
            myDebug_P(PSTR("[MQTT] Identifier Rejected"));
        }
        if (reason == AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE) {
            myDebug_P(PSTR("[MQTT] Server unavailable"));
        }
        if (reason == AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS) {
            myDebug_P(PSTR("[MQTT] Malformed credentials"));
        }
        if (reason == AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED) {
            myDebug_P(PSTR("[MQTT] Not authorized"));
        }

        // Reset reconnection delay
        _mqtt_last_connection = millis();
        _mqtt_connecting      = false;
    });

    //mqttClient.onSubscribe([this](uint16_t packetId, uint8_t qos) { myDebug_P(PSTR("[MQTT] Subscribe ACK for PID %d"), packetId); });

    //mqttClient.onPublish([this](uint16_t packetId) { myDebug_P(PSTR("[MQTT] Publish ACK for PID %d"), packetId); });

    mqttClient.onMessage([this](char * topic, char * payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
        _mqttOnMessage(topic, payload, len);
    });
}

// WiFI setup
void MyESP::_wifi_setup() {
    jw.setHostname(_app_hostname); // Set WIFI hostname
    jw.subscribe([this](justwifi_messages_t code, char * parameter) { _wifiCallback(code, parameter); });
    jw.enableAP(false);
    jw.setConnectTimeout(WIFI_CONNECT_TIMEOUT);
    jw.setReconnectTimeout(WIFI_RECONNECT_INTERVAL);
    jw.enableAPFallback(true);                 // AP mode only as fallback
    jw.enableSTA(true);                        // Enable STA mode (connecting to a router)
    jw.enableScan(false);                      // Configure it to scan available networks and connect in order of dBm
    jw.cleanNetworks();                        // Clean existing network configuration
    jw.addNetwork(_wifi_ssid, _wifi_password); // Add a network

#if defined(ESP8266)
    WiFi.setSleepMode(WIFI_NONE_SLEEP); // added to possibly fix wifi dropouts in arduino core 2.5.0
#endif
}

// set the callback function for the OTA onstart
void MyESP::setOTA(ota_callback_f OTACallback_pre, ota_callback_f OTACallback_post) {
    _ota_pre_callback  = OTACallback_pre;
    _ota_post_callback = OTACallback_post;
}

// OTA callback when the upload process starts
void MyESP::_OTACallback() {
    myDebug_P(PSTR("[OTA] Start"));

#ifdef CRASH
    // If we are not specifically reserving the sectors we are using as
    // EEPROM in the memory layout then any OTA upgrade will overwrite
    // all but the last one.
    // Calling rotate(false) disables rotation so all writes will be done
    // to the last sector. It also sets the dirty flag to true so the next commit()
    // will actually persist current configuration to that last sector.
    // Calling rotate(false) will also prevent any other EEPROM write
    // to overwrite the OTA image.
    // In case the OTA process fails, reenable rotation.
    // See onError callback below.
    EEPROMr.rotate(false);
    EEPROMr.commit();
#endif

    // stop the web server
    webServer.close();

    _ota_doing_update = true;

    if (_ota_pre_callback) {
        (_ota_pre_callback)(); // call custom function
    }
}

// OTA Setup
void MyESP::_ota_setup() {
    if (!_wifi_ssid) {
        return;
    }

    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setHostname(_app_hostname);

    ArduinoOTA.onStart([this]() { _OTACallback(); });
    ArduinoOTA.onEnd([this]() {
        myDebug_P(PSTR("[OTA] Done, restarting..."));
        _ota_doing_update = false;
        _deferredReset(500, CUSTOM_RESET_OTA);
    });

    ArduinoOTA.onProgress([this](unsigned int progress, unsigned int total) {
        static unsigned int _progOld;
        unsigned int        _prog = (progress / (total / 100));
        if (_prog != _progOld) {
            myDebug_P(PSTR("[OTA] Progress: %u%%\r"), _prog);
            _progOld = _prog;
        }
    });

    ArduinoOTA.onError([this](ota_error_t error) {
        if (error == OTA_AUTH_ERROR)
            myDebug_P(PSTR("[OTA] Auth Failed"));
        else if (error == OTA_BEGIN_ERROR)
            myDebug_P(PSTR("[OTA] Begin Failed"));
        else if (error == OTA_CONNECT_ERROR)
            myDebug_P(PSTR("[OTA] Connect Failed"));
        else if (error == OTA_RECEIVE_ERROR)
            myDebug_P(PSTR("[OTA] Receive Failed"));
        else if (error == OTA_END_ERROR)
            myDebug_P(PSTR("[OTA] End Failed"));

#ifdef CRASH
        // There's been an error, reenable eeprom rotation
        EEPROMr.rotate(true);
#endif
    });
}

// sets boottime
void MyESP::setBoottime(const char * boottime) {
    if (_boottime) {
        free(_boottime);
    }
    _boottime = strdup(boottime);
}

// eeprom
void MyESP::_eeprom_setup() {
#ifdef CRASH
    EEPROMr.size(4);
    EEPROMr.begin(SPI_FLASH_SEC_SIZE);
#endif
}

// Set callback of sketch function to process project messages
void MyESP::setTelnet(telnetcommand_callback_f callback_cmd, telnet_callback_f callback) {
    _telnetcommand_callback = callback_cmd; // external function to handle commands
    _telnet_callback        = callback;
}

void MyESP::_telnetConnected() {
    myDebug_P(PSTR("[TELNET] Telnet connection established"));
    _consoleShowHelp(); // Show the initial message

#ifdef CRASH
    // show crash dump if just restarted after a fatal crash
    uint32_t crash_time;
    EEPROMr.get(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_CRASH_TIME, crash_time);
    if ((crash_time != 0) && (crash_time != 0xFFFFFFFF)) {
        myDebug_P(PSTR("[SYSTEM] There is stack data available from the last system crash. Use 'crash dump' to view and 'crash clear' to reset"));
    }
#endif

    // call callback
    if (_telnet_callback) {
        (_telnet_callback)(TELNET_EVENT_CONNECT);
    }
}

void MyESP::_telnetDisconnected() {
    myDebug_P(PSTR("[TELNET] Telnet connection closed"));
    if (_telnet_callback) {
        (_telnet_callback)(TELNET_EVENT_DISCONNECT); // call callback
    }
}

// Initialize the telnet server
void MyESP::_telnet_setup() {
    SerialAndTelnet.setWelcomeMsg("");
    SerialAndTelnet.setCallbackOnConnect([this]() { _telnetConnected(); });
    SerialAndTelnet.setCallbackOnDisconnect([this]() { _telnetDisconnected(); });
    SerialAndTelnet.setDebugOutput(false);
    SerialAndTelnet.begin(TELNET_SERIAL_BAUD); // default baud is 115200

    // init command buffer for console commands
    memset(_command, 0, TELNET_MAX_COMMAND_LENGTH);
}

// Show help of commands
void MyESP::_consoleShowHelp() {
    myDebug_P(PSTR(""));
    myDebug_P(PSTR("* Connected to: %s version %s"), _app_name, _app_version);

    if (isAPmode()) {
        myDebug_P(PSTR("* Device is in AP mode with SSID %s"), jw.getAPSSID().c_str());
    } else {
        myDebug_P(PSTR("* Hostname: %s (%s)"), _getESPhostname().c_str(), WiFi.localIP().toString().c_str());
        myDebug_P(PSTR("* WiFi SSID: %s (signal %d%%)"), WiFi.SSID().c_str(), getWifiQuality());
        if (isMQTTConnected()) {
            myDebug_P(PSTR("* MQTT connected (heartbeat %s)"), getHeartbeat() ? "enabled" : "disabled");
        } else {
            myDebug_P(PSTR("* MQTT disconnected"));
        }
    }

    myDebug_P(PSTR("*"));
    myDebug_P(PSTR("* Commands:"));
    myDebug_P(PSTR("*  ?=help, CTRL-D/quit=exit telnet session"));
    myDebug_P(PSTR("*  set, system, reboot"));
#ifdef CRASH
    myDebug_P(PSTR("*  crash <dump | clear | test [n]>"));
#endif

    // call callback function
    if (_telnet_callback) {
        (_telnet_callback)(TELNET_EVENT_SHOWCMD);
    }

    myDebug_P(PSTR("")); // newline
}

// print all set commands and current values
void MyESP::_printSetCommands() {
    myDebug_P(PSTR("")); // newline
    myDebug_P(PSTR("The following set commands are available:"));
    myDebug_P(PSTR("")); // newline
    myDebug_P(PSTR("  set erase"));
    myDebug_P(PSTR("  set <wifi_ssid | wifi_password> [value]"));
    myDebug_P(PSTR("  set <mqtt_host | mqtt_username | mqtt_password> [value]"));
    myDebug_P(PSTR("  set serial <on | off>"));

    // call callback function
    if (_telnet_callback) {
        (_telnet_callback)(TELNET_EVENT_SHOWSET);
    }

    myDebug_P(PSTR("")); // newline
    myDebug_P(PSTR("Stored settings:"));
    myDebug_P(PSTR("")); // newline
    myDebug_P(PSTR("  wifi_ssid=%s "), (!_wifi_ssid) ? "<not set>" : _wifi_ssid);
    SerialAndTelnet.print(FPSTR("  wifi_password="));
    if (!_wifi_password) {
        SerialAndTelnet.print(FPSTR("<not set>"));
    } else {
        for (uint8_t i = 0; i < strlen(_wifi_password); i++) {
            SerialAndTelnet.print(FPSTR("*"));
        }
    }
    myDebug_P(PSTR("")); // newline
    myDebug_P(PSTR("  mqtt_host=%s"), (!_mqtt_host) ? "<not set>" : _mqtt_host);
    myDebug_P(PSTR("  mqtt_username=%s"), (!_mqtt_username) ? "<not set>" : _mqtt_username);
    SerialAndTelnet.print(FPSTR("  mqtt_password="));
    if (!_mqtt_password) {
        SerialAndTelnet.print(FPSTR("<not set>"));
    } else {
        for (uint8_t i = 0; i < strlen(_mqtt_password); i++) {
            SerialAndTelnet.print(FPSTR("*"));
        }
    }

    myDebug_P(PSTR("")); // newline
    myDebug_P(PSTR("  serial=%s"), (_serial) ? "on" : "off");
    myDebug_P(PSTR("  heartbeat=%s"), (_heartbeat) ? "on" : "off");

    // print any custom settings
    (_fs_settings_callback)(MYESP_FSACTION_LIST, 0, NULL, NULL);

    myDebug_P(PSTR("")); // newline
}

// reset / restart
void MyESP::resetESP() {
    myDebug_P(PSTR("* Reboot ESP..."));
    _deferredReset(500, CUSTOM_RESET_TERMINAL);
    end();
#if defined(ARDUINO_ARCH_ESP32)
    ESP.restart();
#else
    ESP.restart();
#endif
}

// read next word from string buffer
// if parameter true then a word is only terminated by a newline
char * MyESP::_telnet_readWord(bool allow_all_chars) {
    if (allow_all_chars) {
        return (strtok(NULL, "\n")); // allow only newline
    } else {
        return (strtok(NULL, ", \n")); // allow space and comma
    }
}

// change settings - always as strings
// messy code but effective since we don't have too many settings
// wc is word count, number of parameters after the 'set' command
bool MyESP::_changeSetting(uint8_t wc, const char * setting, const char * value) {
    bool ok = false;

    // check for our internal commands first
    if (strcmp(setting, "erase") == 0) {
        _fs_eraseConfig();
        return true;

    } else if (strcmp(setting, "wifi_ssid") == 0) {
        if (_wifi_ssid)
            free(_wifi_ssid);
        _wifi_ssid = NULL; // just to be sure
        if (value) {
            _wifi_ssid = strdup(value);
        }
        ok = true;
        jw.enableSTA(false);
        myDebug_P(PSTR("Note: please 'reboot' ESP to apply new WiFi settings"));
    } else if (strcmp(setting, "wifi_password") == 0) {
        if (_wifi_password)
            free(_wifi_password);
        _wifi_password = NULL; // just to be sure
        if (value) {
            _wifi_password = strdup(value);
        }
        ok = true;
        jw.enableSTA(false);
        myDebug_P(PSTR("Note: please 'reboot' ESP to apply new WiFi settings"));

    } else if (strcmp(setting, "mqtt_host") == 0) {
        if (_mqtt_host)
            free(_mqtt_host);
        _mqtt_host = NULL; // just to be sure
        if (value) {
            _mqtt_host = strdup(value);
        }
        ok = true;
    } else if (strcmp(setting, "mqtt_username") == 0) {
        if (_mqtt_username)
            free(_mqtt_username);
        _mqtt_username = NULL; // just to be sure
        if (value) {
            _mqtt_username = strdup(value);
        }
        ok = true;
    } else if (strcmp(setting, "mqtt_password") == 0) {
        if (_mqtt_password)
            free(_mqtt_password);
        _mqtt_password = NULL; // just to be sure
        if (value) {
            _mqtt_password = strdup(value);
        }
        ok = true;

    } else if (strcmp(setting, "serial") == 0) {
        ok      = true;
        _serial = false;
        if (value) {
            if (strcmp(value, "on") == 0) {
                _serial = true;
                ok      = true;
                myDebug_P(PSTR("Reboot ESP to activate Serial mode."));
            } else if (strcmp(value, "off") == 0) {
                _serial = false;
                ok      = true;
                myDebug_P(PSTR("Reboot ESP to deactivate Serial mode."));
            } else {
                ok = false;
            }
        }

    } else if (strcmp(setting, "heartbeat") == 0) {
        ok         = true;
        _heartbeat = false;
        if (value) {
            if (strcmp(value, "on") == 0) {
                _heartbeat = true;
                ok         = true;
                myDebug_P(PSTR("Heartbeat on"));
            } else if (strcmp(value, "off") == 0) {
                _heartbeat = false;
                ok         = true;
                myDebug_P(PSTR("Heartbeat off"));
            } else {
                ok = false;
            }
        }
    } else {
        // finally check for any custom commands
        ok = (_fs_settings_callback)(MYESP_FSACTION_SET, wc, setting, value);
    }

    // if we were able to recognize the set command, continue
    if (ok) {
        // check for 2 params
        if (value == nullptr) {
            myDebug_P(PSTR("%s setting reset to its default value."), setting);
        } else {
            // must be 3 params
            myDebug_P(PSTR("%s changed."), setting);
        }

        myDebug_P(PSTR("")); // newline

        (void)fs_saveConfig(); // always save the values
    }

    return ok;
}

// force the serial on/off
void MyESP::setUseSerial(bool b) {
    _serial = b;
    SerialAndTelnet.setSerial(b ? &Serial : NULL);
}

void MyESP::_telnetCommand(char * commandLine) {
    char * str   = commandLine;
    bool   state = false;

    if (strlen(commandLine) == 0)
        return;

    // count the number of arguments
    unsigned wc = 0;
    while (*str) {
        if (*str == ' ' || *str == '\n' || *str == '\t') {
            state = false;
        } else if (state == false) {
            state = true;
            ++wc;
        }
        ++str;
    }

    // check first for reserved commands
    char * temp             = strdup(commandLine);         // because strotok kills original string buffer
    char * ptrToCommandName = strtok((char *)temp, " \n"); // space and newline

    // set command
    if (strcmp(ptrToCommandName, "set") == 0) {
        bool ok = false;
        if (wc == 1) {
            _printSetCommands();
            ok = true;
        } else if (wc == 2) { // set <xxx>
            char * setting = _telnet_readWord(false);
            ok             = _changeSetting(wc - 1, setting, NULL);
        } else { // set <xxx> <yyy>
            char * setting = _telnet_readWord(false);
            char * value   = _telnet_readWord(true); // allow strange characters
            ok             = _changeSetting(wc - 1, setting, value);
        }

        if (!ok) {
            myDebug_P(PSTR("\nInvalid parameter for set command."));
        }

        return;
    }

    // reboot command
    if ((strcmp(ptrToCommandName, "reboot") == 0) && (wc == 1)) {
        resetESP();
    }

    // show system stats
    if ((strcmp(ptrToCommandName, "system") == 0) && (wc == 1)) {
        showSystemStats();
        return;
    }

    // show system stats
    if ((strcmp(ptrToCommandName, "quit") == 0) && (wc == 1)) {
        myDebug_P(PSTR("[TELNET] exiting telnet session"));
        SerialAndTelnet.disconnectClient();
        return;
    }

#ifdef CRASH
    // crash command
    if ((strcmp(ptrToCommandName, "crash") == 0) && (wc >= 2)) {
        char * cmd = _telnet_readWord(false);
        if (strcmp(cmd, "dump") == 0) {
            crashDump();
        } else if (strcmp(cmd, "clear") == 0) {
            crashClear();
        } else if ((strcmp(cmd, "test") == 0) && (wc == 3)) {
            char * value = _telnet_readWord(false);
            crashTest(atoi(value));
        } else {
            myDebug_P(PSTR("Error. Usage: crash <dump | clear | test [n]>"));
        }
        return; // don't call custom command line callback
    }
#endif

    // call callback function
    if (_telnetcommand_callback) {
        (_telnetcommand_callback)(wc, commandLine);
    }
}

// returns WiFi hostname as a String object
String MyESP::_getESPhostname() {
    String hostname;

#if defined(ARDUINO_ARCH_ESP32)
    hostname = String(WiFi.getHostname());
#else
    hostname = WiFi.hostname();
#endif

    return (hostname);
}

// returns build time as a String - copied for espurna. see (c)
// takes the time from the gcc during compilation
String MyESP::_buildTime() {
    const char   time_now[] = __TIME__; // hh:mm:ss
    unsigned int hour       = atoi(&time_now[0]);
    unsigned int minute     = atoi(&time_now[3]);
    unsigned int second     = atoi(&time_now[6]);

    const char   date_now[] = __DATE__; // Mmm dd yyyy
    const char * months[]   = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    unsigned int month      = 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(date_now, months[i], 3) == 0) {
            month = i + 1;
            break;
        }
    }
    unsigned int day  = atoi(&date_now[3]);
    unsigned int year = atoi(&date_now[7]);

    char buffer[20];
    snprintf_P(buffer, sizeof(buffer), PSTR("%04d-%02d-%02d %02d:%02d:%02d"), year, month, day, hour, minute, second);

    return String(buffer);
}

// returns system uptime in seconds - copied for espurna. see (c)
unsigned long MyESP::_getUptime() {
    static uint32_t last_uptime      = 0;
    static uint8_t  uptime_overflows = 0;

    if (millis() < last_uptime) {
        ++uptime_overflows;
    }
    last_uptime             = millis();
    uint32_t uptime_seconds = uptime_overflows * (UPTIME_OVERFLOW / 1000) + (last_uptime / 1000);

    return uptime_seconds;
}

// init RTC mem
void MyESP::_rtcmemInit() {
    memset((uint32_t *)RTCMEM_ADDR, 0, sizeof(uint32_t) * RTCMEM_BLOCKS);
    Rtcmem->magic = RTCMEM_MAGIC;
}

uint8_t MyESP::getSystemBootStatus() {
    system_rtcmem_t data;
    data.value = Rtcmem->sys;
    return data.parts.boot_status;
}

void MyESP::_setSystemBootStatus(uint8_t status) {
    system_rtcmem_t data;
    data.value             = Rtcmem->sys;
    data.parts.boot_status = status;
    Rtcmem->sys            = data.value;
    // myDebug("*** setting boot status to %d", data.parts.boot_status);
}

uint8_t MyESP::_getSystemStabilityCounter() {
    system_rtcmem_t data;
    data.value = Rtcmem->sys;
    return data.parts.stability_counter;
}

void MyESP::_setSystemStabilityCounter(uint8_t counter) {
    system_rtcmem_t data;
    data.value                   = Rtcmem->sys;
    data.parts.stability_counter = counter;
    Rtcmem->sys                  = data.value;
}

uint8_t MyESP::_getSystemResetReason() {
    system_rtcmem_t data;
    data.value = Rtcmem->sys;
    return data.parts.reset_reason;
}

void MyESP::_setSystemResetReason(uint8_t reason) {
    system_rtcmem_t data;
    data.value              = Rtcmem->sys;
    data.parts.reset_reason = reason;
    Rtcmem->sys             = data.value;
}

// system_get_rst_info() result is cached by the Core init for internal use
uint32_t MyESP::getSystemResetReason() {
    return resetInfo.reason;
}

void MyESP::_rtcmemSetup() {
    _rtcmem_status = _rtcmemStatus();
    if (!_rtcmem_status) {
        _rtcmemInit();
    }
}

void MyESP::_setCustomResetReason(uint8_t reason) {
    _setSystemResetReason(reason);
}

// returns false if not set and needs to be intialized, causing all rtcmem data to be wiped
bool MyESP::_rtcmemStatus() {
    bool readable;

    uint32_t reason = getSystemResetReason();

    // the last reset could have been caused by manually pressing the reset button
    // so before wiping, capture the boot sequence
    if (reason == REASON_EXT_SYS_RST) { // external system reset
        if (getSystemBootStatus() == MYESP_BOOTSTATUS_BOOTING) {
            _setSystemBootStatus(MYESP_BOOTSTATUS_RESETNEEDED);
        } else {
            _setSystemBootStatus(MYESP_BOOTSTATUS_POWERON);
        }
    }

    switch (reason) {
    //case REASON_EXT_SYS_RST: // external system reset
    case REASON_WDT_RST:     // hardware watch dog reset
    case REASON_DEFAULT_RST: // normal startup by power on
        readable = false;
        break;
    default:
        readable = true;
    }

    readable = readable and (RTCMEM_MAGIC == Rtcmem->magic);

    return readable;
}

bool MyESP::_getRtcmemStatus() {
    return _rtcmem_status;
}

uint8_t MyESP::_getCustomResetReason() {
    static uint8_t status = 255;
    if (status == 255) {
        if (_rtcmemStatus())
            status = _getSystemResetReason();
        if (status > 0)
            _setCustomResetReason(0);
        if (status > CUSTOM_RESET_MAX)
            status = 0;
    }
    return status;
}

void MyESP::_deferredReset(unsigned long delaytime, uint8_t reason) {
    _setSystemBootStatus(MYESP_BOOTSTATUS_POWERON);
    _setCustomResetReason(reason);
    delay(delaytime);
}

// Call this method on boot with stable=true to reset the crash counter
// Each call increments the counter
// If the counter reaches SYSTEM_CHECK_MAX then the system is flagged as unstable
void MyESP::_setSystemCheck(bool stable) {
    uint8_t value = 0;

    if (stable) {
        value = 0; // system is ok
    } else {
        if (!_getRtcmemStatus()) {
            _setSystemStabilityCounter(1);
            return;
        }

        value = _getSystemStabilityCounter();

        if (++value > SYSTEM_CHECK_MAX) {
            _systemStable = false;
            value         = 0; // system is unstable
            myDebug_P(PSTR("[SYSTEM] Warning, system UNSTABLE."));

            /*
            // enable Serial again
            if (!_serial) {
                SerialAndTelnet.setSerial(&Serial);
                _serial = true;
            }
            */
        }
    }

    _setSystemStabilityCounter(value);
}

// return if system is stable (false=bad)
bool MyESP::_getSystemCheck() {
    return _systemStable;
}

// periodically check if system is stable
void MyESP::_systemCheckLoop() {
    static bool checked = false;
    if (!checked && (millis() > SYSTEM_CHECK_TIME)) {
        _setSystemCheck(true); // Flag system as stable
        checked = true;
    }
}


// print out ESP system stats
// for battery power is ESP.getVcc()
void MyESP::showSystemStats() {
#if defined(ESP8266)
    myDebug_P(PSTR("%sESP8266 System stats:%s"), COLOR_BOLD_ON, COLOR_BOLD_OFF);
#else
    myDebug_P(PSTR("ESP32 System stats:"));
#endif
    myDebug_P(PSTR(""));

    myDebug_P(PSTR(" [APP] %s version: %s"), _app_name, _app_version);
    myDebug_P(PSTR(" [APP] MyESP version: %s"), MYESP_VERSION);
    myDebug_P(PSTR(" [APP] Build timestamp: %s"), _buildTime().c_str());
    if (_boottime != NULL) {
        myDebug_P(PSTR(" [APP] Boot time: %s"), _boottime);
    }

    // uptime
    uint32_t t = _getUptime(); // seconds

    uint32_t d   = t / 86400L;
    uint32_t h   = ((t % 86400L) / 3600L) % 60;
    uint32_t rem = t % 3600L;
    uint8_t  m   = rem / 60;
    uint8_t  s   = rem % 60;
    myDebug_P(PSTR(" [APP] Uptime: %d days %d hours %d minutes %d seconds"), d, h, m, s);

    myDebug_P(PSTR(" [APP] System Load: %d%%"), getSystemLoadAverage());

    if (!_getSystemCheck()) {
        myDebug_P(PSTR(" [SYSTEM] Device is in SAFE MODE"));
    }

    if (isAPmode()) {
        myDebug_P(PSTR(" [WIFI] Device is in AP mode with SSID %s"), jw.getAPSSID().c_str());
    } else {
        myDebug_P(PSTR(" [WIFI] WiFi Hostname: %s"), _getESPhostname().c_str());
        myDebug_P(PSTR(" [WIFI] WiFi IP: %s"), WiFi.localIP().toString().c_str());
        myDebug_P(PSTR(" [WIFI] WiFi signal strength: %d%%"), getWifiQuality());
    }

    myDebug_P(PSTR(" [WIFI] WiFi MAC: %s"), WiFi.macAddress().c_str());

    if (isMQTTConnected()) {
        myDebug_P(PSTR(" [MQTT] is connected (with heartbeat %s)"), getHeartbeat() ? "enabled" : "disabled");
    } else {
        myDebug_P(PSTR(" [MQTT] is disconnected"));
    }

#ifdef CRASH
    char output_str[80] = {0};
    char buffer[16]     = {0};
    myDebug_P(PSTR(" [EEPROM] EEPROM size: %u"), EEPROMr.reserved() * SPI_FLASH_SEC_SIZE);
    strlcpy(output_str, " [EEPROM] EEPROM Sector pool size is ", sizeof(output_str));
    strlcat(output_str, itoa(EEPROMr.size(), buffer, 10), sizeof(output_str));
    strlcat(output_str, ", and in use are: ", sizeof(output_str));
    for (uint32_t i = 0; i < EEPROMr.size(); i++) {
        strlcat(output_str, itoa(EEPROMr.base() - i, buffer, 10), sizeof(output_str));
        strlcat(output_str, " ", sizeof(output_str));
    }
    myDebug(output_str);
#endif

#ifdef ARDUINO_BOARD
    myDebug_P(PSTR(" [SYSTEM] Board: %s"), ARDUINO_BOARD);
#endif

    myDebug_P(PSTR(" [SYSTEM] CPU frequency: %u MHz"), ESP.getCpuFreqMHz());
    myDebug_P(PSTR(" [SYSTEM] SDK version: %s"), ESP.getSdkVersion());

#if defined(ESP8266)
    myDebug_P(PSTR(" [SYSTEM] CPU chip ID: 0x%06X"), ESP.getChipId());
    myDebug_P(PSTR(" [SYSTEM] Core version: %s"), ESP.getCoreVersion().c_str());
    myDebug_P(PSTR(" [SYSTEM] Boot version: %d"), ESP.getBootVersion());
    myDebug_P(PSTR(" [SYSTEM] Boot mode: %d"), ESP.getBootMode());
    unsigned char reason = _getCustomResetReason();
    if (reason > 0) {
        char buffer[32];
        strcpy_P(buffer, custom_reset_string[reason - 1]);
        myDebug_P(PSTR(" [SYSTEM] Last reset reason: %s"), buffer);
    } else {
        myDebug_P(PSTR(" [SYSTEM] Last reset reason: %s"), (char *)ESP.getResetReason().c_str());
        myDebug_P(PSTR(" [SYSTEM] Last reset info: %s"), (char *)ESP.getResetInfo().c_str());
    }
    myDebug_P(PSTR(" [SYSTEM] Restart count: %d"), _getSystemStabilityCounter());

    myDebug_P(PSTR(" [SYSTEM] rtcmem status: blocks:%u addr:0x%p"), RtcmemSize, Rtcmem);
    for (uint8_t block = 0; block < RtcmemSize; ++block) {
        myDebug_P(PSTR(" [SYSTEM] rtcmem %02u: %u"), block, reinterpret_cast<volatile uint32_t *>(RTCMEM_ADDR)[block]);
    }
#endif

    FlashMode_t mode = ESP.getFlashChipMode();
#if defined(ESP8266)
    myDebug_P(PSTR(" [FLASH] Flash chip ID: 0x%06X"), ESP.getFlashChipId());
#endif
    myDebug_P(PSTR(" [FLASH] Flash speed: %u Hz"), ESP.getFlashChipSpeed());
    myDebug_P(PSTR(" [FLASH] Flash mode: %s"), mode == FM_QIO ? "QIO" : mode == FM_QOUT ? "QOUT" : mode == FM_DIO ? "DIO" : mode == FM_DOUT ? "DOUT" : "UNKNOWN");
#if defined(ESP8266)
    myDebug_P(PSTR(" [FLASH] Flash size (CHIP): %d"), ESP.getFlashChipRealSize());
#endif
    myDebug_P(PSTR(" [FLASH] Flash size (SDK): %d"), ESP.getFlashChipSize());
    myDebug_P(PSTR(" [FLASH] Flash Reserved: %d"), 1 * SPI_FLASH_SEC_SIZE);
    myDebug_P(PSTR(" [MEM] Firmware size: %d"), ESP.getSketchSize());
    myDebug_P(PSTR(" [MEM] Max OTA size: %d"), (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
    myDebug_P(PSTR(" [MEM] OTA Reserved: %d"), 4 * SPI_FLASH_SEC_SIZE);

    uint32_t total_memory = _getInitialFreeHeap();
    uint32_t free_memory  = ESP.getFreeHeap();

    myDebug(" [MEM] Free Heap: %d bytes initially | %d bytes used (%2u%%) | %d bytes free (%2u%%)",
            total_memory,
            total_memory - free_memory,
            100 * (total_memory - free_memory) / total_memory,
            free_memory,
            100 * free_memory / total_memory);

    myDebug_P(PSTR(""));
}

/*
 * Send heartbeat via MQTT with all system data
 */
void MyESP::_heartbeatCheck(bool force = false) {
    static uint32_t last_heartbeat = 0;

    if ((millis() - last_heartbeat > HEARTBEAT_INTERVAL) || force) {
        last_heartbeat = millis();

        if (!isMQTTConnected() || !(_heartbeat)) {
            return;
        }

        uint32_t total_memory  = _getInitialFreeHeap();
        uint32_t free_memory   = ESP.getFreeHeap();
        uint8_t  mem_available = 100 * free_memory / total_memory; // as a %

        char payload[300] = {0};
        char s[10];
        strlcpy(payload, "version=", sizeof(payload));
        strlcat(payload, _app_version, sizeof(payload)); // version
        strlcat(payload, ", IP=", sizeof(payload));
        strlcat(payload, WiFi.localIP().toString().c_str(), sizeof(payload)); // IP address
        strlcat(payload, ", rssid=", sizeof(payload));
        strlcat(payload, itoa(getWifiQuality(), s, 10), sizeof(payload)); // rssi %
        strlcat(payload, "%, load=", sizeof(payload));
        strlcat(payload, ltoa(getSystemLoadAverage(), s, 10), sizeof(payload)); // load
        strlcat(payload, "%, uptime=", sizeof(payload));
        strlcat(payload, ltoa(_getUptime(), s, 10), sizeof(payload)); // uptime in secs
        strlcat(payload, "secs, freemem=", sizeof(payload));
        strlcat(payload, itoa(mem_available, s, 10), sizeof(payload)); // free mem as a %
        strlcat(payload, "%", sizeof(payload));

        // send to MQTT
        myESP.mqttPublish(MQTT_TOPIC_HEARTBEAT, payload);
    }
}

// handler for Telnet
void MyESP::_telnetHandle() {
    SerialAndTelnet.handle();

    static uint8_t charsRead = 0;
    // read asynchronously until full command input
    while (SerialAndTelnet.available()) {
        char c = SerialAndTelnet.read();

        if (c == 0)
            return;

        SerialAndTelnet.serialPrint(c); // echo to Serial (if connected)

        switch (c) {
        case '\r': // likely have full command in buffer now, commands are terminated by CR and/or LF
        case '\n':
            _command[charsRead] = '\0'; // null terminate our command char array

            if (charsRead > 0) {
                charsRead      = 0; // is static, so have to reset
                _suspendOutput = false;
                if (_serial) {
                    SerialAndTelnet.serialPrint('\n'); // force newline if in Serial
                }
                _telnetCommand(_command);
            }
            break;

        case '\b': // (^H)
        case 0x7F: // (^?)
            if (charsRead > 0) {
                _command[--charsRead] = '\0';

                SerialAndTelnet.write(' ');
                SerialAndTelnet.write('\b');
            }

            break;

        case '?':
            if (!_suspendOutput) {
                _consoleShowHelp();
            } else {
                _command[charsRead++] = c; // add it to buffer as its part of the string entered
            }
            break;
        case 0x04: // EOT, CTRL-D
            myDebug_P(PSTR("[TELNET] exiting telnet session"));
            SerialAndTelnet.disconnectClient();
            break;
        default:
            _suspendOutput = true;
            if (charsRead < TELNET_MAX_COMMAND_LENGTH) {
                _command[charsRead++] = c;
            }
            _command[charsRead] = '\0'; // just in case
            break;
        }
    }
}

// make sure we have a connection to MQTT broker
void MyESP::_mqttConnect() {
    if (!_mqtt_host)
        return; // MQTT not enabled

    // Do not connect if already connected or still trying to connect
    if (mqttClient.connected() || _mqtt_connecting || (WiFi.status() != WL_CONNECTED)) {
        return;
    }

    // Check reconnect interval
    if (millis() - _mqtt_last_connection < _mqtt_reconnect_delay) {
        return;
    }

    _mqtt_connecting = true; // we're doing a connection

    // Increase the reconnect delay
    _mqtt_reconnect_delay += MQTT_RECONNECT_DELAY_STEP;
    if (_mqtt_reconnect_delay > MQTT_RECONNECT_DELAY_MAX) {
        _mqtt_reconnect_delay = MQTT_RECONNECT_DELAY_MAX;
    }

    mqttClient.setServer(_mqtt_host, MQTT_PORT);
    mqttClient.setClientId(_app_hostname);
    mqttClient.setKeepAlive(_mqtt_keepalive);
    mqttClient.setCleanSession(false);

    // last will
    if (_mqtt_will_topic) {
        //myDebug_P(PSTR("[MQTT] Setting last will topic %s"), _mqttTopic(_mqtt_will_topic));
        mqttClient.setWill(_mqttTopic(_mqtt_will_topic), 1, true,
                           _mqtt_will_offline_payload); // retain always true
    }

    if (_mqtt_username && _mqtt_password) {
        myDebug_P(PSTR("[MQTT] Connecting to MQTT using user %s..."), _mqtt_username);
        mqttClient.setCredentials(_mqtt_username, _mqtt_password);
    } else {
        myDebug_P(PSTR("[MQTT] Connecting to MQTT..."));
    }

    // Connect to the MQTT broker
    mqttClient.connect();
}

// Setup everything we need
void MyESP::setWIFI(const char * wifi_ssid, const char * wifi_password, wifi_callback_f callback) {
    // Check SSID too long or missing
    if (!wifi_ssid || *wifi_ssid == 0x00 || strlen(wifi_ssid) > MAX_SSID_LEN) {
        _wifi_ssid = NULL;
    } else {
        _wifi_ssid = strdup(wifi_ssid);
    }

    // Check PASS too long
    if (!wifi_password || *wifi_ssid == 0x00 || strlen(wifi_password) > MAX_PWD_LEN) {
        _wifi_password = NULL;
    } else {
        _wifi_password = strdup(wifi_password);
    }

    // callback
    _wifi_callback = callback;
}

// init MQTT settings
void MyESP::setMQTT(const char *    mqtt_host,
                    const char *    mqtt_username,
                    const char *    mqtt_password,
                    const char *    mqtt_base,
                    unsigned long   mqtt_keepalive,
                    unsigned char   mqtt_qos,
                    bool            mqtt_retain,
                    const char *    mqtt_will_topic,
                    const char *    mqtt_will_online_payload,
                    const char *    mqtt_will_offline_payload,
                    mqtt_callback_f callback) {
    // can be empty
    if (!mqtt_host || *mqtt_host == 0x00) {
        _mqtt_host = NULL;
    } else {
        _mqtt_host = strdup(mqtt_host);
    }

    // mqtt username and password can be empty
    if (!mqtt_username || *mqtt_username == 0x00) {
        _mqtt_username = NULL;
    } else {
        _mqtt_username = strdup(mqtt_username);
    }

    // can be empty
    if (!mqtt_password || *mqtt_password == 0x00) {
        _mqtt_password = NULL;
    } else {
        _mqtt_password = strdup(mqtt_password);
    }

    // base
    if (_mqtt_base) {
        free(_mqtt_base);
    }
    _mqtt_base = strdup(mqtt_base);

    // callback
    _mqtt_callback = callback;

    // various mqtt settings
    _mqtt_keepalive = mqtt_keepalive;
    _mqtt_qos       = mqtt_qos;
    _mqtt_retain    = mqtt_retain;

    // last will
    if (!mqtt_will_topic || *mqtt_will_topic == 0x00) {
        _mqtt_will_topic = NULL;
    } else {
        _mqtt_will_topic = strdup(mqtt_will_topic);
    }

    if (!mqtt_will_online_payload || *mqtt_will_online_payload == 0x00) {
        _mqtt_will_online_payload = NULL;
    } else {
        _mqtt_will_online_payload = strdup(mqtt_will_online_payload);
    }

    if (!mqtt_will_offline_payload || *mqtt_will_offline_payload == 0x00) {
        _mqtt_will_offline_payload = NULL;
    } else {
        _mqtt_will_offline_payload = strdup(mqtt_will_offline_payload);
    }
}

// builds up a topic by prefixing the base and hostname
char * MyESP::_mqttTopic(const char * topic) {
    char buffer[MQTT_MAX_TOPIC_SIZE] = {0};

    strlcpy(buffer, _mqtt_base, sizeof(buffer));
    strlcat(buffer, "/", sizeof(buffer));
    strlcat(buffer, _app_hostname, sizeof(buffer));
    strlcat(buffer, "/", sizeof(buffer));
    strlcat(buffer, topic, sizeof(buffer));

    if (_mqtt_topic) {
        free(_mqtt_topic);
    }
    _mqtt_topic = strdup(buffer);

    return _mqtt_topic;
}

// print contents of file
// assumes Serial is open
void MyESP::_fs_printConfig() {
    myDebug_P(PSTR("[FS] Contents:"));

    File configFile = SPIFFS.open(MYEMS_CONFIG_FILE, "r");
    if (!configFile) {
        myDebug_P(PSTR("[FS] Failed to read file for printing"));
        return;
    }

    while (configFile.available()) {
        SerialAndTelnet.print((char)configFile.read());
    }
    myDebug_P(PSTR("")); // newline

    configFile.close();
}

// format File System
void MyESP::_fs_eraseConfig() {
    myDebug_P(PSTR("[FS] Erasing all settings, please wait a few seconds. ESP will "
                   "automatically restart when finished."));

    if (SPIFFS.remove(MYEMS_CONFIG_FILE)) {
        delay(1000); // wait 1 second
        SerialAndTelnet.flush();
        resetESP(); // hard reset
    }
}

// custom callback for web info
void MyESP::setWeb(web_callback_f callback_web) {
    _web_callback = callback_web;
}

void MyESP::setSettings(fs_callback_f callback_fs, fs_settings_callback_f callback_settings_fs) {
    _fs_callback          = callback_fs;
    _fs_settings_callback = callback_settings_fs;
}

// load from spiffs
bool MyESP::_fs_loadConfig() {
    File configFile = SPIFFS.open(MYEMS_CONFIG_FILE, "r");

    size_t size = configFile.size();
    if (size > 1024) {
        myDebug_P(PSTR("[FS] Config file size is too large"));
        return false;
    } else if (size == 0) {
        return false;
    }

    StaticJsonDocument<SPIFFS_MAXSIZE> doc;
    JsonObject                         json = doc.to<JsonObject>();

    // Deserialize the JSON document
    DeserializationError error = deserializeJson(doc, configFile);
    if (error) {
        myDebug_P(PSTR("[FS] Failed to read config file. Error %s"), error.c_str());
        return false;
    }

    const char * value;

    // fetch the standard system parameters
    value      = json["wifi_ssid"];
    _wifi_ssid = (value) ? strdup(value) : NULL;

    value          = json["wifi_password"];
    _wifi_password = (value) ? strdup(value) : NULL;

    value      = json["mqtt_host"];
    _mqtt_host = (value) ? strdup(value) : NULL;

    value          = json["mqtt_username"];
    _mqtt_username = (value) ? strdup(value) : NULL;

    value          = json["mqtt_password"];
    _mqtt_password = (value) ? strdup(value) : NULL;

    _heartbeat = (bool)json["heartbeat"]; // defaults to off

// serial is only on when booting
#ifdef FORCE_SERIAL
    _serial = true;
#else
    _serial = json["serial"];
#endif

    // callback for loading custom settings
    // ok is false if there's a problem loading a custom setting (e.g. does not exist)
    bool ok = (_fs_callback)(MYESP_FSACTION_LOAD, json);

    configFile.close();

    return ok;
}

// save settings to spiffs
bool MyESP::fs_saveConfig() {
    bool ok = true;

    // call any custom functions before handling SPIFFS
    if (_ota_pre_callback) {
        (_ota_pre_callback)();
    }

    StaticJsonDocument<SPIFFS_MAXSIZE> doc;
    JsonObject                         json = doc.to<JsonObject>();

    json["app_version"]   = _app_version;
    json["wifi_ssid"]     = _wifi_ssid;
    json["wifi_password"] = _wifi_password;
    json["mqtt_host"]     = _mqtt_host;
    json["mqtt_username"] = _mqtt_username;
    json["mqtt_password"] = _mqtt_password;
    json["serial"]        = _serial;
    json["heartbeat"]     = _heartbeat;

    // callback for saving custom settings
    (void)(_fs_callback)(MYESP_FSACTION_SAVE, json);

    // if file exists, remove it just to be safe
    if (SPIFFS.exists(MYEMS_CONFIG_FILE)) {
        SPIFFS.remove(MYEMS_CONFIG_FILE);
    }

    // open for writing
    File configFile = SPIFFS.open(MYEMS_CONFIG_FILE, "w");
    if (!configFile) {
        myDebug_P(PSTR("[FS] Failed to open config file for writing"));
        return false;
    }

    // Serialize JSON to file
    if (serializeJson(json, configFile) == 0) {
        myDebug_P(PSTR("[FS] Failed to write config file"));
        ok = false;
    }

    configFile.close();

    // call any custom functions before handling SPIFFS
    if (_ota_post_callback) {
        (_ota_post_callback)();
    }

    return ok; // it worked
}

// init the SPIFF file system and load the config
// if it doesn't exist try and create it
void MyESP::_fs_setup() {
    if (!SPIFFS.begin()) {
        myDebug_P(PSTR("[FS] Failed to mount the file system. Erasing..."));
        _fs_eraseConfig(); // fix for ESP32
        return;
    }

    // if its flagged as a first install, re-create the initial config file and quit function
    if (_firstInstall) {
        myDebug_P(PSTR("[FS] Re-creating config file for initial install"));
        fs_saveConfig();
        return;
    }

    // load the config file. if it doesn't exist (function returns false) create it
    if (!_fs_loadConfig()) {
        myDebug_P(PSTR("[FS] Re-creating config file"));
        fs_saveConfig();
        _firstInstall = true; // flag as a first install
    }

    // assume if the wifi ssid is empty, its a fresh install too
    if ((_wifi_ssid == NULL)) {
        _firstInstall = true; // flag as a first install
    }

    myDebug_P(PSTR("[FS] Settings loaded from SPIFFS"));

    // _fs_printConfig(); // enable for debugging
}

uint32_t MyESP::getSystemLoadAverage() {
    return _load_average;
}

// calculate load average
void MyESP::_calculateLoad() {
    static uint32_t last_loadcheck    = 0;
    static uint32_t load_counter_temp = 0;
    load_counter_temp++;

    if (millis() - last_loadcheck > LOADAVG_INTERVAL) {
        static uint32_t load_counter     = 0;
        static uint32_t load_counter_max = 1;

        load_counter      = load_counter_temp;
        load_counter_temp = 0;
        if (load_counter > load_counter_max) {
            load_counter_max = load_counter;
        }
        _load_average  = 100 - (100 * load_counter / load_counter_max);
        last_loadcheck = millis();
    }
}

// returns true is MQTT is alive
bool MyESP::isMQTTConnected() {
    return mqttClient.connected();
}

// return true if wifi is connected
bool MyESP::isWifiConnected() {
    return (_wifi_connected);
}

/*
   Return the quality (Received Signal Strength Indicator)
   of the WiFi network.
   Returns a number between 0 and 100 if WiFi is connected.
   Returns -1 if WiFi is disconnected.

   High quality: 90% ~= -55dBm
   Medium quality: 50% ~= -75dBm
   Low quality: 30% ~= -85dBm
   Unusable quality: 8% ~= -96dBm
*/
int MyESP::getWifiQuality() {
    if (WiFi.status() != WL_CONNECTED)
        return -1;
    int dBm = WiFi.RSSI();
    if (dBm <= -100)
        return 0;
    if (dBm >= -50)
        return 100;
    return 2 * (dBm + 100);
}

#ifdef CRASH
/**
 * Save crash information in EEPROM
 * This function is called automatically if ESP8266 suffers an exception
 * It should be kept quick / consise to be able to execute before hardware wdt may kick in
 */
extern "C" void custom_crash_callback(struct rst_info * rst_info, uint32_t stack_start, uint32_t stack_end) {
    // write crash time to EEPROM
    uint32_t crash_time = millis();
    EEPROMr.put(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_CRASH_TIME, crash_time);

    // write reset info to EEPROM
    EEPROMr.write(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_RESTART_REASON, rst_info->reason);
    EEPROMr.write(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_EXCEPTION_CAUSE, rst_info->exccause);

    // write epc1, epc2, epc3, excvaddr and depc to EEPROM
    EEPROMr.put(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_EPC1, rst_info->epc1);
    EEPROMr.put(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_EPC2, rst_info->epc2);
    EEPROMr.put(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_EPC3, rst_info->epc3);
    EEPROMr.put(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_EXCVADDR, rst_info->excvaddr);
    EEPROMr.put(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_DEPC, rst_info->depc);

    // write stack start and end address to EEPROM
    EEPROMr.put(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_STACK_START, stack_start);
    EEPROMr.put(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_STACK_END, stack_end);

    // write stack trace to EEPROM and avoid overwriting settings
    int16_t current_address = SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_STACK_TRACE;
    for (uint32_t i = stack_start; i < stack_end; i++) {
        byte * byteValue = (byte *)i;
        EEPROMr.write(current_address++, *byteValue);
    }

    EEPROMr.commit();
}

/**
 * Clears crash info
 */
void MyESP::crashClear() {
    myDebug_P(PSTR("[CRASH] Clearing crash dump"));
    uint32_t crash_time = 0xFFFFFFFF;
    EEPROMr.put(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_CRASH_TIME, crash_time);
    EEPROMr.commit();
}

/**
 * Print out crash information that has been previously saved in EEPROM
 * Copied from https://github.com/krzychb/EspSaveCrash
 */
void MyESP::crashDump() {
    uint32_t crash_time;
    EEPROMr.get(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_CRASH_TIME, crash_time);
    if ((crash_time == 0) || (crash_time == 0xFFFFFFFF)) {
        myDebug_P(PSTR("[CRASH] No crash data captured."));
        return;
    }

    uint32_t t   = crash_time / 1000; // convert to seconds
    uint32_t d   = t / 86400L;
    uint32_t h   = (t / 3600L) % 60;
    uint32_t rem = t % 3600L;
    uint8_t  m   = rem / 60;
    uint8_t  s   = rem % 60;
    myDebug_P(PSTR("[CRASH] Last crash was %d days %d hours %d minutes %d seconds since boot time"), d, h, m, s);

    // get reason and exception
    // https://www.espressif.com/sites/default/files/documentation/esp8266_reset_causes_and_common_fatal_exception_causes_en.pdf
    char buffer[80] = {0};
    char ss[16]     = {0};
    strlcpy(buffer, "[CRASH] Reason of restart: ", sizeof(buffer));

    uint8_t reason = EEPROMr.read(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_RESTART_REASON);
    switch (reason) {
    case REASON_WDT_RST:
        strlcat(buffer, "1 - Hardware WDT reset", sizeof(buffer));
        break;
    case REASON_EXCEPTION_RST:
        strlcat(buffer, "2 - Fatal exception", sizeof(buffer));
        break;
    case REASON_SOFT_WDT_RST:
        strlcat(buffer, "3 - Software watchdog reset", sizeof(buffer));
        break;
    case REASON_EXT_SYS_RST:
        strlcat(buffer, "6 - Hardware reset", sizeof(buffer));
        break;
    case REASON_SOFT_RESTART:
        strlcat(buffer, "4 - Software reset", sizeof(buffer));
        break;
    default:
        strlcat(buffer, itoa(reason, ss, 10), sizeof(buffer));
    }
    myDebug(buffer);

    // check for exception
    // see https://github.com/esp8266/Arduino/blob/master/doc/exception_causes.rst
    if (reason == REASON_EXCEPTION_RST) {
        // get exception cause
        uint8_t cause = EEPROMr.read(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_EXCEPTION_CAUSE);
        strlcpy(buffer, "[CRASH] Exception cause: ", sizeof(buffer));
        if (cause == 0) {
            strlcat(buffer, "0 - IllegalInstructionCause", sizeof(buffer));
        } else if (cause == 3) {
            strlcat(buffer, "3 - LoadStoreErrorCause", sizeof(buffer));
        } else if (cause == 6) {
            strlcat(buffer, "6 - IntegerDivideByZeroCause", sizeof(buffer));
        } else if (cause == 9) {
            strlcat(buffer, "9 - LoadStoreAlignmentCause", sizeof(buffer));
        } else {
            strlcat(buffer, itoa(cause, ss, 10), sizeof(buffer));
        }
    }
    myDebug(buffer);

    uint32_t epc1, epc2, epc3, excvaddr, depc;
    EEPROMr.get(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_EPC1, epc1);
    EEPROMr.get(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_EPC2, epc2);
    EEPROMr.get(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_EPC3, epc3);
    EEPROMr.get(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_EXCVADDR, excvaddr);
    EEPROMr.get(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_DEPC, depc);

    myDebug_P(PSTR("[CRASH] epc1=0x%08x epc2=0x%08x epc3=0x%08x"), epc1, epc2, epc3);
    myDebug_P(PSTR("[CRASH] excvaddr=0x%08x depc=0x%08x"), excvaddr, depc);

    uint32_t stack_start, stack_end;
    EEPROMr.get(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_STACK_START, stack_start);
    EEPROMr.get(SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_STACK_END, stack_end);

    myDebug_P(PSTR("[CRASH] sp=0x%08x end=0x%08x"), stack_start, stack_end);

    int16_t current_address = SAVE_CRASH_EEPROM_OFFSET + SAVE_CRASH_STACK_TRACE;
    int16_t stack_len       = stack_end - stack_start;

    uint32_t stack_trace;

    myDebug_P(PSTR(">>>stack>>>"));

    for (int16_t i = 0; i < stack_len; i += 0x10) {
        SerialAndTelnet.printf("%08x: ", stack_start + i);
        for (byte j = 0; j < 4; j++) {
            EEPROMr.get(current_address, stack_trace);
            SerialAndTelnet.printf("%08x ", stack_trace);
            current_address += 4;
        }
        SerialAndTelnet.println();
    }
    myDebug_P(PSTR("<<<stack<<<"));
    myDebug_P(PSTR("\nTo clean this dump use the command: %scrash clear%s\n"), COLOR_BOLD_ON, COLOR_BOLD_OFF);
}

/*
 * Force some crashes to test if stack collection and debugging works
 */
void MyESP::crashTest(uint8_t t) {
    if (t == 1) {
        myDebug_P(PSTR("[CRASH] Attempting to divide by zero ..."));
        int result, zero;
        zero   = 0;
        result = 1 / zero;
        Serial.printf("Result = %d", result);
    }

    if (t == 2) {
        myDebug_P(PSTR("[CRASH] Attempting to read through a pointer to no object ..."));
        int * nullPointer;
        nullPointer = NULL;
        // null pointer dereference - read
        // attempt to read a value through a null pointer
        Serial.println(*nullPointer);
    }

    if (t == 3) {
        myDebug_P(PSTR("[CRASH] Crashing with hardware WDT (%ld ms) ...\n"), millis());
        ESP.wdtDisable();
        while (true) {
            // stay in an infinite loop doing nothing
            // this way other process can not be executed
            //
            // Note:
            // Hardware wdt kicks in if software wdt is unable to perfrom
            // Nothing will be saved in EEPROM for the hardware wdt
        }
    }

    if (t == 4) {
        myDebug_P(PSTR("[CRASH] Crashing with software WDT (%ld ms) ...\n"), millis());
        while (true) {
            // stay in an infinite loop doing nothing
            // this way other process can not be executed
        }
    }
}

#else
void MyESP::crashTest(uint8_t t) {
}
void MyESP::crashClear() {
}
void MyESP::crashDump() {
}
void MyESP::crashInfo() {
}
#endif

// default home web page
void MyESP::_webRootPage() {
    char s[1000] = {0};

    strlcpy(s, webCommonPage_start, sizeof(s));
    // strlcat(s, webCommonPage_start_refresh, sizeof(s));
    strlcat(s, webCommonPage_start_body, sizeof(s));

    strlcat(s, "<h1>", sizeof(s));
    strlcat(s, _app_name, sizeof(s));
    strlcat(s, " version ", sizeof(s));
    strlcat(s, _app_version, sizeof(s));
    strlcat(s, "</h1>", sizeof(s));

    strlcat(s, "<p><b>System stats:</b><br>", sizeof(s));

    if (isAPmode()) {
        strlcat(s, " Device is in Wifi Access Point mode with SSID <b>", sizeof(s));
        strlcat(s, jw.getAPSSID().c_str(), sizeof(s));
        strlcat(s, "</b>", sizeof(s));
    } else {
        char buf[4];
        strlcat(s, " Connected to wireless network <b>", sizeof(s));
        strlcat(s, _getESPhostname().c_str(), sizeof(s));
        strlcat(s, "</b> with signal strength <b>", sizeof(s));
        strlcat(s, itoa(getWifiQuality(), buf, 10), sizeof(s));
        strlcat(s, "%</b>", sizeof(s));
    }

    strlcat(s, isMQTTConnected() ? "<br> MQTT is connected\n" : " MQTT is disconnected\n", sizeof(s));
    strlcat(s, "</br>", sizeof(s));

    // uptime
    char     buffer[200];
    uint32_t t   = _getUptime(); // seconds
    uint32_t d   = t / 86400L;
    uint32_t h   = ((t % 86400L) / 3600L) % 60;
    uint32_t rem = t % 3600L;
    uint8_t  m   = rem / 60;
    uint8_t  sec = rem % 60;
    sprintf(buffer, " System uptime: %d days %d hours %d minutes %d seconds", d, h, m, sec);
    strlcat(s, buffer, sizeof(s));

    // memory
    //uint32_t total_memory = _getInitialFreeHeap();
    //uint32_t free_memory  = ESP.getFreeHeap();
    //sprintf(buffer, " Memory: %d bytes free (%2u%%)<br>", free_memory, 100 * free_memory / total_memory);
    //strlcat(s, buffer, sizeof(s));

    strlcat(s, "<p>", sizeof(s));
    if (_web_callback) {
        char custom[MYESP_MAXCHARBUFFER];
        (_web_callback)(custom);
        strlcat(s, custom, sizeof(s));
    }
    strlcat(s, "</p><br>", sizeof(s));

    // check why we're here
    if ((_firstInstall) || (_wifi_ssid == NULL)) {
        strlcat(s, "<p>Looks like a first install! Go <a href=/reset>here</a> to connect the System to your network.</p>", sizeof(s));
    } else {
        strlcat(s, "<p>Go <a href=/reset>here</a> to connect the System to your wireless network.</p>", sizeof(s));
    }

    strlcat(s, webCommonPage_end, sizeof(s));
    webServer.sendHeader("Content-Length", String(strlen(s)));
    webServer.send(200, "text/html", s);
}

// Creates a webpage that allows the user to change the SSID and Password from the browser
void MyESP::_webResetPage() {
    char s[1000] = {0};

    strlcpy(s, webCommonPage_start, sizeof(s));
    strlcat(s, webCommonPage_start_body, sizeof(s));

    strlcat(s, "<h1>", sizeof(s));
    strlcat(s, _app_name, sizeof(s));
    strlcat(s, " version ", sizeof(s));
    strlcat(s, _app_version, sizeof(s));
    strlcat(s, "</h1>", sizeof(s));

    // Check to see if we've been sent any arguments and instantly return if not
    if (webServer.args() == 0) {
        strlcat(s, "<p>", sizeof(s));

        if (_wifi_ssid != NULL) {
            strlcat(s, "Current wifi SSID is ", sizeof(s));
            strlcat(s, _wifi_ssid, sizeof(s));
            strlcat(s, ".<br>", sizeof(s));
        }

        strlcat(s, "<br>Please enter your new wifi credentials below.</p>", sizeof(s));

        strlcat(s, webResetPage_form, sizeof(s));
        strlcat(s, webCommonPage_end, sizeof(s));
        webServer.sendHeader("Content-Length", String(strlen(s)));
        webServer.send(200, "text/html", s);

    } else {
        // Create a string containing all the arguments
        // Check to see if there are new values (also doubles to check the length of the new value is long enough)
        if (webServer.arg("newssid").length() <= MAX_SSID_LEN) {
            if (webServer.arg("newssid").length() == 0) {
                _wifi_ssid = NULL;
            } else {
                _wifi_ssid = strdup(webServer.arg("newssid").c_str());
            }
        }

        if (webServer.arg("newpassword").length() <= MAX_PWD_LEN) {
            if (webServer.arg("newpassword").length() == 0) {
                _wifi_password = NULL;
            } else {
                _wifi_password = strdup(webServer.arg("newpassword").c_str());
            }
        }

        // Store the new settings
        fs_saveConfig();

        // Reply with a web page to indicate success or failure
        strlcat(s, webResetPage_post, sizeof(s));
        strlcat(s, webCommonPage_end, sizeof(s));
        webServer.sendHeader("Content-Length", String(strlen(s)));
        webServer.send(200, "text/html", s);

        delay(500);
        resetESP();
    }
}

// reset all settings
void MyESP::_webResetAllPage() {
    char s[1000] = {0};

    strlcpy(s, webCommonPage_start, sizeof(s));
    strlcat(s, webCommonPage_start_body, sizeof(s));

    strlcat(s, "<h1>", sizeof(s));
    strlcat(s, _app_name, sizeof(s));
    strlcat(s, " version ", sizeof(s));
    strlcat(s, _app_version, sizeof(s));
    strlcat(s, "</h1>", sizeof(s));

    // Check to see if we've been sent any arguments and instantly return if not
    if (webServer.args() == 0) {
        strlcat(s,
                "<p>Are you absolutely sure you want to erase all settings?<br>Typing 'yes' will restart the System and you'll need to reconnect to the wifi "
                "Access Point called ems-esp.</p>",
                sizeof(s));

        strlcat(s, webResetAllPage_form, sizeof(s));
        strlcat(s, webCommonPage_end, sizeof(s));
        webServer.sendHeader("Content-Length", String(strlen(s)));
        webServer.send(200, "text/html", s);
    } else {
        // delete all settings
        if (webServer.arg("confirm") == "yes") {
            _fs_eraseConfig();
            delay(1000); // wait 1 sec
            resetESP();
        }
    }
}

// set up web server
void MyESP::_webserver_setup() {
    webServer.on("/", [this]() { _webRootPage(); });
    webServer.on("/reset", [this]() { _webResetPage(); });
    webServer.on("/resetall", [this]() { _webResetAllPage(); });

    webServer.begin();

    myDebug_P(PSTR("[WEB] Web server started"));
}

// bootup sequence
// quickly flash LED until we get a Wifi connection, or AP established
// fast way is to use WRITE_PERI_REG(PERIPHS_GPIO_BASEADDR + (state ? 4 : 8), (1 << EMSESP_Status.led_gpio)); // 4 is on, 8 is off
void MyESP::_bootupSequence() {
    uint8_t boot_status = getSystemBootStatus();

    if ((boot_status == MYESP_BOOTSTATUS_BOOTED) || (millis() <= MYESP_BOOTUP_DELAY)) {
        return; // already booted, or still starting up
    }

    // only kick in after a few seconds
    if (boot_status == MYESP_BOOTSTATUS_POWERON) {
        _setSystemBootStatus(MYESP_BOOTSTATUS_BOOTING);
    }

    static uint32_t last_bootupflash = 0;

    // flash LED quickly
    if ((millis() - last_bootupflash > MYESP_BOOTUP_FLASHDELAY)) {
        last_bootupflash = millis();
        int state        = digitalRead(LED_BUILTIN);
        digitalWrite(LED_BUILTIN, !state);
    }

    if (isWifiConnected()) {
        _setSystemBootStatus(MYESP_BOOTSTATUS_BOOTED); // completed, reset flag
        digitalWrite(LED_BUILTIN, LOW);                // turn off LED
    }
}

// setup MyESP
void MyESP::begin(const char * app_hostname, const char * app_name, const char * app_version) {
    _app_hostname = strdup(app_hostname);
    _app_name     = strdup(app_name);
    _app_version  = strdup(app_version);

    _telnet_setup(); // Telnet setup, called first to set Serial

    // print a welcome message
    myDebug_P(PSTR("\n\n* %s version %s"), _app_name, _app_version);

    // set up onboard LED
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

    _getInitialFreeHeap(); // get initial free mem
    _rtcmemSetup();        // rtc internal mem setup

    if (getSystemBootStatus() == MYESP_BOOTSTATUS_RESETNEEDED) {
        myDebug_P(PSTR("** resetting all settings"));
        _firstInstall = true; // flag as an initial install so the config file will be recreated
    }

    _eeprom_setup();    // set up EEPROM for storing crash data, if compiled with -DCRASH
    _fs_setup();        // SPIFFS setup, do this first to get values
    _wifi_setup();      // WIFI setup
    _ota_setup();       // init OTA
    _webserver_setup(); // init web server

    _setSystemCheck(false); // reset system check
    _heartbeatCheck(true);  // force heartbeat check (not the MQTT one)

    SerialAndTelnet.flush();
}

/*
 * Loop. This is called as often as possible and it handles wifi, telnet, mqtt etc
 */
void MyESP::loop() {
    jw.loop();           // WiFi
    ArduinoOTA.handle(); // OTA

    if (_ota_doing_update) {
        return; // quit if in the middle of an OTA update
    }

    _calculateLoad();
    _systemCheckLoop();
    _heartbeatCheck();
    _bootupSequence();
    webServer.handleClient();
    _telnetHandle();
    _mqttConnect();

    yield(); // ...and breath
}

MyESP myESP; // create instance