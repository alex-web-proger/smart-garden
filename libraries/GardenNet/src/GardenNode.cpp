#include "GardenNode.h"

void GardenNode::begin(DeviceType deviceType, const uint8_t mac[6], SendFn sf) {
    myType = deviceType;
    memcpy(myMac, mac, 6);
    sendFn = sf;

    // Сидируем джиттер от MAC (простой FNV-1a хэш) - детерминировано на
    // конкретную плату (удобно при отладке по логам), но разное между
    // устройствами - это и требуется, чтобы узлы не отправляли
    // телеметрию синхронно после одновременного включения (см.
    // PROTOCOL.md §6.2).
    uint32_t seed = 2166136261UL;
    for (uint8_t i = 0; i < 6; i++) {
        seed ^= myMac[i];
        seed *= 16777619UL;
    }
    randomSeed(seed);
}

void GardenNode::setCallbacks(FillPayloadFn fillConfig, FillPayloadFn fillTelemetry,
                               OnCommandFn onCommand, OnWatchdogFn onWatchdog) {
    fillConfigFn = fillConfig;
    fillTelemetryFn = fillTelemetry;
    onCommandFn = onCommand;
    onWatchdogFn = onWatchdog;
}

void GardenNode::setTiming(unsigned long tIntervalMs, unsigned long tJitterMs,
                            unsigned long cIntervalMs, unsigned long cJitterMs,
                            unsigned long wdTimeoutMs) {
    telemetryIntervalMs = tIntervalMs;
    telemetryJitterMs = tJitterMs;
    configIntervalMs = cIntervalMs;
    configJitterMs = cJitterMs;
    watchdogTimeoutMs = wdTimeoutMs;

    nextTelemetryInterval = jitteredInterval(telemetryIntervalMs, telemetryJitterMs);
    nextConfigInterval = jitteredInterval(configIntervalMs, configJitterMs);
}

unsigned long GardenNode::jitteredInterval(unsigned long base, unsigned long jitter) {
    if (jitter == 0) return base;
    long offset = random(-(long) jitter, (long) jitter + 1);
    return (unsigned long) ((long) base + offset);
}

void GardenNode::prepareHeader(MsgType mType) {
    // Узел всегда шлёт Хабу (HUB_MAC) от своего типа устройства (myType) -
    // общая механика заполнения полей теперь в GardenProtocol.h, здесь
    // остаётся только то, что специфично именно узлу (фиксированный
    // адресат). См. комментарий у fillPacketHeader().
    fillPacketHeader(txPacket, myMac, HUB_MAC, myType, mType, lastPacketId);
}

void GardenNode::sendConfig() {
    prepareHeader(MSG_CONFIG);
    if (fillConfigFn) fillConfigFn(txPacket);
    if (sendFn) sendFn((const uint8_t *) &txPacket, sizeof(txPacket));
    Serial.println("Sent: CONFIG");
}

void GardenNode::sendTelemetryNow() {
    prepareHeader(MSG_TELEMETRY);
    if (fillTelemetryFn) fillTelemetryFn(txPacket);
    if (sendFn) sendFn((const uint8_t *) &txPacket, sizeof(txPacket));
    Serial.println("Sent: TELEMETRY");
}

void GardenNode::sendAck(uint16_t ackedPacketId, uint8_t status) {
    prepareHeader(MSG_ACK);
    txPacket.payload.ack.acked_packet_id = ackedPacketId;
    txPacket.payload.ack.status = status;
    if (sendFn) sendFn((const uint8_t *) &txPacket, sizeof(txPacket));
    Serial.printf("Sent: ACK for packet_id=%u status=%u\n", ackedPacketId, status);
}

void GardenNode::loop() {
    unsigned long now = millis();

    if (now - lastTelemetryTime > nextTelemetryInterval) {
        lastTelemetryTime = now;
        nextTelemetryInterval = jitteredInterval(telemetryIntervalMs, telemetryJitterMs);
        sendTelemetryNow();
    }

    if (now - lastConfigTime > nextConfigInterval) {
        lastConfigTime = now;
        nextConfigInterval = jitteredInterval(configIntervalMs, configJitterMs);
        sendConfig();
    }

    if (watchdogArmed && (millis() - lastHubContactTime > watchdogTimeoutMs)) {
        watchdogArmed = false;
        Serial.printf("WATCHDOG: no contact with hub for >%lums\n", watchdogTimeoutMs);
        if (onWatchdogFn) onWatchdogFn();
    }
}

void GardenNode::handleIncoming(const UniversalPacket &packet) {
    // 1. Проверяем, нам ли пакет: либо адресован лично нам, либо это
    //    широковещательное объявление "всем" (FF..FF) - например,
    //    announce Хаба о своей (пере)загрузке, см. п.2 ниже.
    bool addressedToMe = memcmp(packet.receiver_mac, myMac, 6) == 0;
    bool addressedToAll = memcmp(packet.receiver_mac, BROADCAST_MAC, 6) == 0;
    if (!addressedToMe && !addressedToAll) return;

    // 2. Хаб перезагрузился и объявляет об этом broadcast-ом MSG_CONFIG.
    //    Сбрасываем dedup ДО проверки isNewerPacketId ниже - иначе само
    //    announce-сообщение могло бы быть отклонено как "старое"
    //    (см. PROTOCOL.md §4.3).
    if (packet.device_type == TYPE_HUB && packet.msg_type == MSG_CONFIG) {
        lastReceivedCommandId = 0;
        Serial.println("Hub объявил о (пере)загрузке - dedup сброшен");
    }

    // 3. Дедупликация с учётом переполнения uint16_t.
    if (!isNewerPacketId(packet.packet_id, lastReceivedCommandId)) {
        Serial.printf("Ignored duplicate/old packet_id=%u (last=%u)\n",
                       packet.packet_id, lastReceivedCommandId);
        return;
    }
    lastReceivedCommandId = packet.packet_id;

    // Любой валидный пакет от Хаба считаем признаком того, что связь жива.
    lastHubContactTime = millis();

    // 4. Команда - передаём в колбэк домена и автоматически шлём ACK.
    if (packet.msg_type == MSG_COMMAND) {
        uint8_t status = 0;
        if (onCommandFn) status = onCommandFn(packet);
        sendAck(packet.packet_id, status);
    }
}

void GardenNode::armWatchdog() {
    watchdogArmed = true;
}

void GardenNode::disarmWatchdog() {
    watchdogArmed = false;
}
