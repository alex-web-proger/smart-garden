#include <ESP8266WiFi.h>
#include <espnow.h>
#include <GardenProtocol.h>
#include <GardenNode.h>

// Настройки модуля
#define MY_DEVICE_TYPE TYPE_IRRIGATION
#define VALVE_COUNT 4
#define HAS_FLOW_SENSOR 1

// Пины, на которые подключены светодиоды (через резистор) - на месте
// реального МОСФЕТ-ключа, управляющего клапаном. Подробности выбора
// именно этих GPIO - см. PROTOCOL.md §6.1.
const uint8_t valvePins[VALVE_COUNT] = {5, 4, 14, 12};

#define WATCHDOG_TIMEOUT_MS 30000UL
#define TELEMETRY_INTERVAL_MS 10000UL
#define TELEMETRY_JITTER_MS 2000UL
#define CONFIG_INTERVAL_MS 3600000UL
#define CONFIG_JITTER_MS 300000UL

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t myMac[6];

GardenNode node; // вся протокольная логика (dedup/джиттер/watchdog/ACK) - в библиотеке

uint8_t activeValve = 0; // 0 - всё закрыто, 1..VALVE_COUNT - открыт клапан N

void applyValveState(uint8_t valve) {
    for (uint8_t i = 0; i < VALVE_COUNT; i++) {
        digitalWrite(valvePins[i], (valve == (i + 1)) ? HIGH : LOW);
    }
}

// Транспорт - единственное, что зависит от платформы (ESP8266 vs ESP32),
// поэтому живёт в скетче, а не в библиотеке.
bool sendRaw(const uint8_t *data, size_t len) {
    return esp_now_send(broadcastAddress, (uint8_t *) data, len) == 0;
}

// --- Колбэки под конкретный домен устройства (полив) ---

void fillConfig(UniversalPacket &pkt) {
    pkt.payload.irrigation.spec.valve_count = VALVE_COUNT;
    pkt.payload.irrigation.spec.has_flow_sensor = HAS_FLOW_SENSOR;
}

void fillTelemetry(UniversalPacket &pkt) {
    pkt.payload.irrigation.telemetry.active_valve = activeValve;
    pkt.payload.irrigation.telemetry.current_flow = 0;      // TODO: датчик потока
    pkt.payload.irrigation.telemetry.total_water_used = 0;  // TODO: накопление + EEPROM
}

uint8_t onCommand(const UniversalPacket &pkt) {
    IrrigationCommand cmd = pkt.payload.irrigation.command;
    Serial.printf("COMMAND RECEIVED: valve=%u mode=%u duration_sec=%u volume_l=%u\n",
                  cmd.target_valve, cmd.mode, cmd.duration_sec, cmd.volume_l);

    // Реальное управление GPIO. Для mode==1 (полив по объёму) расчёт по
    // датчику потока ещё не реализован - клапан просто открывается,
    // как при mode==0 (TODO).
    activeValve = cmd.target_valve;
    applyValveState(activeValve);

    if (activeValve != 0) node.armWatchdog();
    else node.disarmWatchdog();

    return 0; // 0 = команда принята (узел пока ничего не отклоняет)
}

void onWatchdogTimeout() {
    activeValve = 0;
    applyValveState(0);
}

// --- Транспортный recv-колбэк (ESP8266-специфичная сигнатура) ---
void onDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
    if (len != sizeof(UniversalPacket)) return;
    UniversalPacket pkt;
    memcpy(&pkt, incomingData, sizeof(pkt));
    node.handleIncoming(pkt);
}

void setup() {
    Serial.begin(115200);

    for (uint8_t i = 0; i < VALVE_COUNT; i++) pinMode(valvePins[i], OUTPUT);
    applyValveState(0);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    WiFi.macAddress(myMac);
    Serial.print("My MAC: ");
    for (int i = 0; i < 6; i++) Serial.printf("%02X%c", myMac[i], i < 5 ? ':' : '\n');

    if (esp_now_init() != 0) {
        Serial.println("ESP-NOW Init Failed");
        return;
    }
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(onDataRecv);
    esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);

    node.begin(MY_DEVICE_TYPE, myMac, sendRaw);
    node.setCallbacks(fillConfig, fillTelemetry, onCommand, onWatchdogTimeout);
    node.setTiming(TELEMETRY_INTERVAL_MS, TELEMETRY_JITTER_MS,
                   CONFIG_INTERVAL_MS, CONFIG_JITTER_MS, WATCHDOG_TIMEOUT_MS);
    node.sendConfig(); // заявляем о себе сразу при включении
}

void loop() {
    node.loop();
}
