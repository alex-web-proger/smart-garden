#pragma once
#include <Arduino.h>

// Радиоканал Wi-Fi, на котором работает вся ESP-NOW сеть проекта.
// ESP-NOW видит только устройства на ОДНОМ И ТОМ ЖЕ канале, поэтому
// это значение должно быть одинаковым на Хабе и на всех периферийных
// узлах.
// TODO: пока канал фиксирован. Когда Хаб будет подключаться к домашнему
// Wi-Fi (канал которого не контролируем), нужна будет рассылка реального
// канала периферии, а не константа.
#define ESPNOW_CHANNEL 1

// Все структуры пакета передаются "как есть" по радиоканалу между разными
// платформами (ESP8266 / ESP32), поэтому фиксируем выравнивание в 1 байт:
// без этого компилятор может добавить padding внутри структуры, и layout
// в памяти на разных платформах/версиях компилятора может не совпасть.
#pragma pack(push, 1)

// Типы устройств
enum DeviceType : uint8_t {
    TYPE_HUB = 0,
    TYPE_IRRIGATION = 1,
    TYPE_WEATHER = 2
};

// Типы сообщений
enum MsgType : uint8_t {
    MSG_CONFIG = 1,     // "Паспорт" устройства. Отправляется редко + сразу при загрузке.
                        // Приём MSG_CONFIG от устройства = сигнал "это устройство только
                        // что перезагрузилось", получатель должен сбросить dedup-счётчик
                        // для этого MAC (см. isNewerPacketId ниже).
    MSG_TELEMETRY = 2,  // Текущие показания, часто
    MSG_COMMAND = 3,    // Команда от Хаба к устройству
    MSG_ACK = 4         // Подтверждение приёма команды
};

// Спецификация полива (Паспорт устройства)
struct IrrigationSpec {
    uint8_t valve_count;
    uint8_t has_flow_sensor;
};

// Телеметрия полива
struct IrrigationTelemetry {
    uint8_t active_valve;      // 0 - всё закрыто, 1..N - номер открытого клапана
    uint32_t current_flow;     // мл/мин
    uint32_t total_water_used; // литры (накопительно)
};

// Команда управления поливом.
// Вынесена отдельно от телеметрии: "что просим сделать" и "что происходит
// сейчас" - разные по смыслу вещи, раньше делили одно поле active_valve.
struct IrrigationCommand {
    uint8_t target_valve;  // 0 - закрыть всё, 1..N - какой клапан открыть
    uint8_t mode;          // 0 - по времени (duration_sec), 1 - по объёму (volume_l)
    uint16_t duration_sec; // сколько секунд держать клапан открытым (mode=0)
    uint16_t volume_l;     // сколько литров вылить (mode=1, требует датчика потока)
};

// Подтверждение приёма пакета (сейчас используется для MSG_COMMAND)
struct AckData {
    uint16_t acked_packet_id; // packet_id пакета, который подтверждаем
    uint8_t status;           // 0 - принято, 1 - отклонено/ошибка
};

// ГЛАВНАЯ СТРУКТУРА ПАКЕТА
struct UniversalPacket {
    uint8_t sender_mac[6];   // КТО отправил
    uint8_t receiver_mac[6]; // КОМУ (00..00 - Хабу, FF..FF - широковещательно
                              // всем устройствам, иначе - конкретный узел)
    uint16_t packet_id;      // Номер пакета (для dedup, см. isNewerPacketId)
    uint8_t hop_count;       // Прыжки (для ретрансляторов)
    uint8_t device_type;     // DeviceType
    uint8_t msg_type;        // MsgType

    union {
        // TYPE_IRRIGATION: какое из полей валидно, зависит от msg_type
        union {
            IrrigationSpec spec;           // MSG_CONFIG
            IrrigationTelemetry telemetry; // MSG_TELEMETRY
            IrrigationCommand command;     // MSG_COMMAND
        } irrigation;

        AckData ack; // MSG_ACK, общий для всех типов устройств
        // Здесь будут другие устройства (TYPE_WEATHER и т.д.)
    } payload;
};

#pragma pack(pop)

// ESP-NOW ограничивает полезную нагрузку 250 байтами - проверяем на этапе
// компиляции, чтобы будущие добавления в payload не вышли за лимит незаметно.
static_assert(sizeof(UniversalPacket) <= 250,
              "UniversalPacket превышает лимит полезной нагрузки ESP-NOW (250 байт)");

// MAC-адреса специального назначения, общие для всего протокола.
// static (а не extern) - каждая единица трансляции, подключившая этот
// заголовок, получает свою копию; для двух 6-байтных массивов это
// дешевле, чем заводить отдельный .cpp только ради extern-определения.
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // "всем" / радио-broadcast
static const uint8_t HUB_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};       // "адресовано Хабу"

// Заполняет заголовок пакета - общая механика для ЛЮБОГО отправителя
// (Хаба или периферийного узла): кто/кому, номер по счётчику, hop_count,
// тип устройства и сообщения. packetIdCounter передаётся по ссылке и
// инкрементируется - это отдельный счётчик ИСХОДЯЩИХ пакетов у каждого
// устройства (не общий на весь проект).
//
// Раньше эта механика была продублирована почти дословно в hub.ino и в
// GardenNode.cpp - у Хаба и узла объективно разные требования к тому,
// ЧТО подставлять в receiver_mac/device_type (Хабу нужен произвольный
// MAC конкретного узла на каждый вызов, узлу - всегда фиксированный
// HUB_MAC), но сам процесс заполнения полей одинаков у обоих. Разная
// вызывающая логика (hub.ino::prepareHeader и GardenNode::prepareHeader)
// сохраняется - меняется только то, что они больше не дублируют тело.
inline void fillPacketHeader(UniversalPacket &pkt, const uint8_t senderMac[6],
                              const uint8_t receiverMac[6], DeviceType deviceType,
                              MsgType msgType, uint16_t &packetIdCounter) {
    memcpy(pkt.sender_mac, senderMac, 6);
    memcpy(pkt.receiver_mac, receiverMac, 6);
    pkt.packet_id = ++packetIdCounter;
    pkt.hop_count = 0;
    pkt.device_type = deviceType;
    pkt.msg_type = msgType;
}

// Циклическое сравнение packet_id (аналог сравнения TCP sequence numbers).
// Корректно обрабатывает переполнение uint16_t (65535 -> 0): считает пакет
// "новее", если разница по модулю 65536 меньше половины диапазона, и
// строго отклоняет точное совпадение (diff==0) как дубликат.
//
// Правило "MSG_CONFIG сбрасывает dedup" реализовано в обе стороны на
// уровне приложения (GardenNode - для узлов, hub.ino - для Хаба), не
// здесь: получив MSG_CONFIG от устройства, нужно сбросить сохранённый
// lastSeen packet_id для этого отправителя в 0 ДО вызова этой функции.
inline bool isNewerPacketId(uint16_t incoming, uint16_t lastSeen) {
    uint16_t diff = incoming - lastSeen;
    return diff != 0 && diff < 0x8000;
}
