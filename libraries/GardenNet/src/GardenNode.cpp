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

void GardenNode::setConfigHandler(OnSetConfigFn onSetConfig) {
    onSetConfigFn = onSetConfig;
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

    // 2. Hub перезагрузился и объявляет об этом broadcast-ом MSG_CONFIG. В отличие от
    //    старой версии, ЗДЕСЬ lastReceivedCommandId НЕ трогается (см. п.3 ниже: hub-announce
    //    теперь ПОЛНОСТЬЮ обходит проверку свежести и синхронизирует счётчик НАПРЯМУЮ
    //    с packet_id ЭТОГО пакета). Сброс в 0 был ошибкой: если СОБСТВЕННЫЙ исходящий
    //    счётчик Хаба (lastPacketId в hub.ino) к этому моменту уже перевалил за половину
    //    16-битного диапазона (32768 - вполне реально при достаточном аптайме,
    //    ведь Хаб шлёт по announce на КАЖДОЕ периодическое CONFIG любого узла, см.
    //    processIncomingPacket() в hub.ino), то САМО это announce отклонялось бы шагом ниже
    //    как "старое" относительно свежеобнулённого 0 (см. isNewerPacketId()) - узел оставался бы
    //    отклонённым, пока packet_id всех последующих пакетов Хаба не сделает полный
    //    оборот (дни). См. PROTOCOL.md §4.5.
    bool isHubAnnounce = (packet.device_type == TYPE_HUB && packet.msg_type == MSG_CONFIG);

    if (isHubAnnounce) {
        Serial.println("Hub объявил о (пере)загрузке");

        // Хаб мог либо ДЕЙСТВИТЕЛЬНО перезагрузиться, либо просто РЕТРАНСЛИРОВАТЬ это
        // announce в ответ на ЧЬЕ-ЛИБО чужое периодическое MSG_CONFIG (Hub в hub.ino делает
        // это для ЛЮБОГО пришедшего MSG_CONFIG, чтобы быстро синхронизировать время
        // свежезагруженного УЗЛА, а не только ПРИ СВОЕЙ собственной перезагрузке -
        // см. processIncomingPacket() в hub.ino). Отличаем эти два случая по packet_id
        // САМОГО ХабА: при реальной перезагрузке Хаба его СОБСТВЕННЫЙ счётчик
        // исходящих пакетов (lastPacketId в hub.ino) сбрасывается в 0 и растёт заново -
        // то есть packet_id этого announce будет МЕНЬШЕ последнего виденного нами (в
        // циклическом смысле, см. isNewerPacketId()) - а не просто следующим по порядку,
        // как при обычной ретрансляции. Именно в этом случае (Хаб потерял нашу
        // конфигурацию вместе с RAM) имеет смысл немедленно переслать её заново, не
        // ждущи планового MSG_CONFIG (база ~1 час, см. PROTOCOL.md) - если бы узел
        // реагировал на ЛЮБОЙ announce безусловно - при 30-50 узлах это была бы
        // лавина: очередной узел присылает свой CONFIG -> Хаб шлёт очередной announce всем
        // (таково его поведение, см. выше) -> все другие узлы тоже перешлют свой
        // CONFIG -> новые announce'ы всем ... и так бесконечно - поэтому проверка
        // ниже КРИТИЧНА, а не просто "для надёжности".
        if (hubAnnounceSeen && !isNewerPacketId(packet.packet_id, lastHubAnnouncePacketId)) {
            Serial.println("Похоже, Hub ПЕРЕЗАГРУЗИЛСЯ (packet_id сброшен) - пересылаю свою "
                            "конфигурацию заново");
            sendConfig();
        }
        hubAnnounceSeen = true;
        lastHubAnnouncePacketId = packet.packet_id;

        // Тем же пакетом Хаб мог разнести своё текущее МЕСТНОЕ время (таким его
        // прислал браузер, см. PROTOCOL.md §12). epoch==0 - валидное значение "Хаб сам
        // ещё не синхронизирован", а не ошибка - в этом случае часы узла не
        // трогаем, чтобы не затереть уже возможно валидное время, полученное
        // ранее (например, если Хаб перезагрузился и ещё не успел 
        // пересинхронизироваться с браузером заново).
        uint32_t epoch = packet.payload.hub.epoch;
        if (epoch != 0) {
            struct timeval tv;
            tv.tv_sec = (time_t) epoch;
            tv.tv_usec = 0;
            settimeofday(&tv, nullptr);
            timeSynced = true;
            Serial.print("Часы узла синхронизированы с Хабом: "); Serial.println(currentTimeString());
        }
    }

    // 3. Дедупликация с учётом переполнения uint16_t. Hub-announce (isHubAnnounce) принимается
    // БЕЗУСЛОВНО, МИНУЯ эту проверку - он и есть сам сигнал "начни отсчёт заново",
    // сравнивать его со СТАРЫМ значением было бы бессмысленно (и опасно - см. п.2 выше).
    // lastReceivedCommandId синхронизируется НАПРЯМУЮ с packet_id ЭТОГО пакета (а не с 0) -
    // это и есть новая точка отсчёта.
    if (!isHubAnnounce && !isNewerPacketId(packet.packet_id, lastReceivedCommandId)) {
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

    // 5. Новая конфигурация от Хаба - то же самое авто-ACK, что и у
    // команды выше, но дополнительно - если колбэк принял и применил новые
    // значения (status==0), немедленно шлём очередной MSG_CONFIG - тот же
    // механизм, что и у sendConfig() в setup() узла, просто триггернутый сейчас,
    // а не при загрузке - чтобы Хаб увидел новые valve_count/mode быстро, а не ждал
    // планового MSG_CONFIG (база ~1 час, см. PROTOCOL.md). При отказе (status!=0,
    // например невалидное valve_count) ничего дополнительно не шлём - сам ACK{status=1}
    // уже сообщил Хабу, что ничего не изменилось, повторное эхо с теми же старыми
    // значениями ничего бы не добавило.
    if (packet.msg_type == MSG_SET_CONFIG) {
        uint8_t status = 0;
        if (onSetConfigFn) status = onSetConfigFn(packet);
        sendAck(packet.packet_id, status);
        if (status == 0) sendConfig();
    }
}

void GardenNode::armWatchdog() {
    watchdogArmed = true;
}

void GardenNode::disarmWatchdog() {
    watchdogArmed = false;
}

// Симметрично Hub::currentTimeString() в hub.ino - тот же формат вывода, чтобы
// логи Хаба и узла читались одинаково. Хранимое значение - МЕСТНОЕ
// время (таким его уже прислал Хаб в announce, сам ПОЛУЧИВШИЙ его от браузера,
// см. PROTOCOL.md §12) - поэтому тут оно просто читается `gmtime_r()`-ом как есть, без
// какого-либо повторного сдвига здесь - часовой пояс уже учтён браузером до
// отправки, а Хаб просто ретранслирует полученное значение дальше узлам как есть.
String GardenNode::currentTimeString() const {
    time_t now = time(nullptr);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    char buf[20];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
              timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
              timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buf);
}
