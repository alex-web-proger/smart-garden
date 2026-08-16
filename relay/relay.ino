#include <ESP8266WiFi.h>
#include <espnow.h>
#include <GardenProtocol.h>

// Ретранслятор ("Репка"). Намеренно максимально "тупое" устройство:
// не разбирает payload, не знает про клапаны/освещение/что угодно ещё,
// не ведёт таблицу устройств. Единственная задача - продлить радиус
// действия сети: услышал оригинал (hop_count==0) - переслал его дальше
// с hop_count=1. Уже ретранслированные пакеты (hop_count!=0) повторно
// не пересылает - это и есть защита от петель/усиления эфира при
// нескольких репках в зоне слышимости друг друга (см. common.h).
//
// Дедупликация ЗДЕСЬ НАМЕРЕННО НЕ РЕАЛИЗОВАНА. Ошибочный dedup на
// транзитном узле, который один раз пропустит "вроде бы уже виденный"
// пакет, может стоить узлу вне прямой досягаемости Хаба ЕДИНСТВЕННОГО
// пути доставки команды - и она не дойдёт вообще. А лишняя пересылка
// безвредна: конечный узел/Хаб сам корректно отбросит дубликат через
// isNewerPacketId() (см. common.h). Поэтому репка пересылает КАЖДЫЙ
// оригинал безусловно, не пытаясь угадывать "было или не было".

UniversalPacket relayPacket;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

unsigned long forwardedCount = 0;
unsigned long skippedCount = 0;
unsigned long lastHeartbeat = 0;

void onDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
    if (len != sizeof(UniversalPacket)) return;
    memcpy(&relayPacket, incomingData, sizeof(UniversalPacket));

    // Уже ретранслированный (hop_count >= 1) пакет не трогаем. Это и есть
    // анти-петля: даже если несколько репок слышат друг друга, дальше
    // одного "прыжка" пакет не улетит и не начнёт множиться.
    if (relayPacket.hop_count != 0) {
        skippedCount++;
        return;
    }

    relayPacket.hop_count = 1;
    esp_now_send(broadcastAddress, (uint8_t *) &relayPacket, sizeof(relayPacket));
    forwardedCount++;

    Serial.printf("Retransmitted packet_id=%u device_type=%u msg_type=%u (forwarded=%lu, skipped=%lu)\n",
                  relayPacket.packet_id, relayPacket.device_type, relayPacket.msg_type,
                  forwardedCount, skippedCount);
}

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != 0) {
        Serial.println("ESP-NOW Init Failed");
        return;
    }

    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(onDataRecv);
    esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);

    Serial.println("Relay ready.");
}

void loop() {
    // Вся логика - в onDataRecv(). Тут только периодический "живой" отчёт,
    // чтобы на столе было видно, что репка не зависла, даже без трафика.
    if (millis() - lastHeartbeat > 30000) {
        lastHeartbeat = millis();
        Serial.printf("Relay alive. forwarded=%lu skipped=%lu\n", forwardedCount, skippedCount);
    }
}
