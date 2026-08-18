#pragma once
#include <Arduino.h>
#include <time.h>
#include <sys/time.h>
#include "GardenProtocol.h"

// Переиспользуемая логика ПЕРИФЕРИЙНОГО узла (не Хаба): подготовка
// заголовка, периодическая отправка MSG_CONFIG/MSG_TELEMETRY с
// джиттером, приём и дедупликация входящих пакетов (включая сброс
// dedup по announce-объявлению Хаба о перезагрузке), автоматическая
// отправка MSG_ACK на команды, watchdog по таймауту связи.
//
// Транспорт (esp_now_send/esp_now_init/регистрация recv-колбэка)
// СОЗНАТЕЛЬНО остаётся в скетче, а не здесь - API esp-now отличается
// между ESP8266 (<espnow.h>) и ESP32 (<esp_now.h>), и эта разница не
// должна протекать в общую логику. Скетч передаёт сюда только готовую
// функцию "отправить эти байты" (SendFn).
//
// Домен устройства (что конкретно в payload CONFIG/TELEMETRY, что
// делать с командой, что делать при потере связи) тоже остаётся в
// скетче - через колбэки. Так один и тот же класс обслуживает и
// текущий узел полива, и будущие узлы освещения/др. без изменений
// внутри самого класса.
class GardenNode {
public:
    // Отправка уже готового набора байт (весь UniversalPacket). Разная
    // реализация на ESP8266/ESP32 - см. пример в flow_node.ino.
    // Возвращаемое значение используется только для лога.
    typedef bool (*SendFn)(const uint8_t *data, size_t len);

    // Заполнение payload перед отправкой MSG_CONFIG/MSG_TELEMETRY.
    // Заголовок (sender_mac/receiver_mac/packet_id/device_type/msg_type)
    // уже заполнен к моменту вызова - трогать не нужно.
    typedef void (*FillPayloadFn)(UniversalPacket &packet);

    // Обработка входящей команды (MSG_COMMAND). К моменту вызова пакет
    // уже прошёл проверку адресации и дедупликации. Возвращает статус
    // для автоматически отправляемого MSG_ACK: 0 - принято, 1 - отклонено.
    typedef uint8_t (*OnCommandFn)(const UniversalPacket &packet);

    // Вызывается, когда watchdog сработал (armWatchdog() был вызван, но
    // валидных пакетов от Хаба не было дольше watchdogTimeoutMs).
    typedef void (*OnWatchdogFn)();

    // Инициализация. Вызвать в setup() ПОСЛЕ того, как известен MAC
    // устройства (WiFi.macAddress), но можно ДО esp_now_init - джиттер
    // сидируется от MAC сразу здесь.
    void begin(DeviceType deviceType, const uint8_t mac[6], SendFn sendFn);

    // Колбэки под конкретный домен устройства. Любой можно оставить
    // nullptr, если не нужен (например, если устройство не принимает
    // команд).
    void setCallbacks(FillPayloadFn fillConfig, FillPayloadFn fillTelemetry,
                       OnCommandFn onCommand, OnWatchdogFn onWatchdog);

    // Периоды отправки (базовое значение) и границы случайного джиттера
    // вокруг них - подробнее см. PROTOCOL.md §6.2 проекта Smart Garden.
    // watchdogTimeoutMs - сколько ждать связи с Хабом после armWatchdog(),
    // прежде чем сработает onWatchdog().
    void setTiming(unsigned long telemetryIntervalMs, unsigned long telemetryJitterMs,
                   unsigned long configIntervalMs, unsigned long configJitterMs,
                   unsigned long watchdogTimeoutMs);

    // Отправить MSG_CONFIG прямо сейчас. Вызвать один раз в setup()
    // после esp_now_add_peer - это же "объявление о своей загрузке" для
    // Хаба (см. GardenProtocol.h про MSG_CONFIG).
    void sendConfig();

    // Вызывать в каждой итерации loop(). Обслуживает таймеры
    // CONFIG/TELEMETRY и watchdog.
    void loop();

    // Вызывать из платформенного recv-колбэка, передав уже
    // скопированный и проверенный по размеру UniversalPacket. Сама
    // решает: адресован ли пакет этому узлу, не дубликат ли он, и что
    // делать с командой (включая автоматическую отправку MSG_ACK).
    void handleIncoming(const UniversalPacket &packet);

    // "Вооружить"/снять watchdog - например, после открытия клапана
    // нужно armWatchdog(), после закрытия (в т.ч. вручную) - disarmWatchdog().
    void armWatchdog();
    void disarmWatchdog();

    // Часы узла синхронизируются автоматически, без участия скетча:
    // узел не ведёт собственный часовой источник - время приходит попутно
    // с каждым broadcast-объявлением Хаба о (пере)загрузке (device_type==TYPE_HUB,
    // msg_type==MSG_CONFIG) - тот же broadcast-пакет, что уже используется для
    // сброса dedup (см. handleIncoming()) - никакого отдельного MsgType под это
    // заводить не потребовалось. См. PROTOCOL.md §12.
    //
    // Если Хаб сам ещё не синхронизирован с браузером, его announce несёт
    // epoch=0 - в этом случае часы узла не трогаются и остаются не
    // синхронизированными (isTimeSynced() вернёт false).
    bool isTimeSynced() const { return timeSynced; }

    // Текущее время узла (UTC) в читаемом виде - для Serial-лога. Если
    // isTimeSynced()==false, отражает реальные (бессмысленные) показания часов
    // платформы (обычно около 1970-01-01) - вызывающий код должен проверять
    // флаг отдельно, если это важно.
    String currentTimeString() const;

    // Отправить MSG_TELEMETRY ПРЯМО СЕЙЧАС, не дожидаясь периодического
    // таймера из loop(). Нужен для событийных устройств (например,
    // кнопки) - вызвать сразу в момент события, а не ждать следующего
    // тика. Не сбрасывает и не трогает обычный периодический таймер
    // телеметрии - следующий плановый тик по-прежнему случится по
    // расписанию, это дополнительная внеочередная отправка.
    //
    // ВАЖНО для по-настоящему дискретных событий (не периодического
    // состояния): у telemetry нет ACK и нет автоповтора - если этот
    // конкретный пакет потеряется в эфире, восстанавливать нечего.
    // Для события вроде "нажатие кнопки" разумнее класть в payload не
    // булево "было/не было", а монотонно растущий счётчик - тогда даже
    // потеря одного внеочередного пакета не теряет сам факт события:
    // следующая обычная телеметрия (или следующее нажатие) принесёт
    // актуальное значение счётчика, и Хаб увидит разницу.
    void sendTelemetryNow();

private:
    DeviceType myType = TYPE_IRRIGATION;
    uint8_t myMac[6] = {0, 0, 0, 0, 0, 0};
    SendFn sendFn = nullptr;

    FillPayloadFn fillConfigFn = nullptr;
    FillPayloadFn fillTelemetryFn = nullptr;
    OnCommandFn onCommandFn = nullptr;
    OnWatchdogFn onWatchdogFn = nullptr;

    unsigned long telemetryIntervalMs = 10000;
    unsigned long telemetryJitterMs = 2000;
    unsigned long configIntervalMs = 3600000;
    unsigned long configJitterMs = 300000;
    unsigned long watchdogTimeoutMs = 30000;

    uint16_t lastPacketId = 0;          // счётчик ИСХОДЯЩИХ пакетов
    uint16_t lastReceivedCommandId = 0; // dedup ВХОДЯЩИХ пакетов от Хаба

    unsigned long lastTelemetryTime = 0;
    unsigned long lastConfigTime = 0;
    unsigned long nextTelemetryInterval = 0;
    unsigned long nextConfigInterval = 0;

    unsigned long lastHubContactTime = 0;
    bool watchdogArmed = false;
    bool timeSynced = false; // true - хотя бы один раз получили epoch!=0 в announce от Хаба

    UniversalPacket txPacket;

    void prepareHeader(MsgType mType);
    unsigned long jitteredInterval(unsigned long base, unsigned long jitter);
    void sendAck(uint16_t ackedPacketId, uint8_t status);
};
