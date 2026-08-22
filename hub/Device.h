#pragma once
#include <Arduino.h>
#include <GardenProtocol.h>

// Базовый класс любого устройства в таблице DeviceManager'а - хранит и
// умеет то, что ОДИНАКОВО для любого типа устройства (полив, освещение,
// погодная станция, ...): MAC, тип, состояние связи (когда последний раз
// видели, счётчик packet_id для dedup), пользовательское название и
// признак "установлено" (installed).
//
// Специфика конкретного типа устройства (какие поля телеметрии у него
// есть, как их печатать в Serial/отдавать в JSON веб-API, как разбирать
// его CONFIG/TELEMETRY payload) - в наследниках (см. IrrigationDevice.h).
// DeviceManager хранит устройства ЧЕРЕЗ УКАЗАТЕЛЬ на Device (полиморфно),
// НЕ через массив объектов по значению - при копировании объекта-наследника
// в массив базового типа виртуальные методы "срезались" бы до базового
// класса (object slicing). См. большой комментарий в DeviceManager.h про
// то, как и где эти объекты физически живут в памяти.
//
// installed/name НАМЕРЕННО тоже базовые поля, не специфика наследников -
// это общий workflow подключения устройства к Хабу (см. PROTOCOL.md §11),
// одинаковый для любого типа устройства.
#define DEVICE_NAME_MAX_LEN 24 // включая завершающий '\0' - т.е. до 23 символов имени

class Device {
public:
    bool installed = false;   // true - подтверждено пользователем, защищено от LRU, живёт в NVS
    uint8_t mac[6];
    uint8_t deviceType;       // DeviceType из GardenProtocol.h
    uint16_t lastSeenPacketId = 0;
    unsigned long lastSeenTime = 0;
    char name[DEVICE_NAME_MAX_LEN] = ""; // задаётся пользователем, см. DeviceManager::setName(); "" - не задано

    Device(const uint8_t *macAddr, uint8_t type) : deviceType(type) {
        memcpy(mac, macAddr, 6);
        lastSeenTime = millis();
    }

    // Виртуальный деструктор ОБЯЗАТЕЛЕН: DeviceManager уничтожает объекты
    // через указатель на Device (см. DeviceManager::destroyAt()) - без
    // virtual здесь вызвался бы только ~Device(), а не деструктор
    // фактического наследника (для полей текущих наследников это было бы
    // не критично, т.к. все поля - примитивы без собственных ресурсов, но
    // как только появится наследник с не-POD полем, отсутствие virtual
    // стало бы источником утечки/undefined behavior).
    virtual ~Device() = default;

    // Разобрать входящий CONFIG/TELEMETRY-пакет, специфичный для
    // device_type этого устройства (payload.irrigation/.weather/...) -
    // заменяет собой switch(device_type) в hub.ino, который раньше
    // диспетчеризовал в handleIrrigationPayload()/будущие аналоги. ACK и
    // сам заголовок пакета - общие для всех типов устройств, поэтому
    // разбираются ДО вызова этого метода, не здесь (см.
    // processIncomingPacket() в hub.ino). idx передаётся только для
    // Serial-логов ("от #%d") - сам Device своего индекса в таблице не
    // знает и знать не должен.
    virtual void handlePayload(int idx, const UniversalPacket &pkt) = 0;

    // Дописать в JSON-ответ /api/devices поля, специфичные для этого типа
    // устройства (например, "activeValve" у полива) - вызывается ПОСЛЕ
    // общих полей (mac/type/name/...), поэтому реализация сама должна
    // дописывать ведущую запятую перед своим полем (см.
    // IrrigationDevice::appendJsonFields()). Пустая реализация по
    // умолчанию - для типов без собственных полей (см. GenericDevice).
    virtual void appendJsonFields(String &json) const {}

    // Дописать в Serial-листинг (DeviceManager::printList()) сведения,
    // специфичные для этого типа устройства - аналог appendJsonFields(),
    // но для человекочитаемого вывода. Пустая реализация по умолчанию.
    virtual void printExtra() const {}
};

// Заглушка для device_type, для которого ещё нет собственного класса
// (например TYPE_WEATHER, пока не реализован). Устройство всё равно
// регистрируется в таблице (видно в списке, MAC/тип известны, участвует
// в LRU/install как любое другое) - просто его payload не разбирается,
// как и раньше в hub.ino (см. прежнюю default-ветку switch(device_type)
// в processIncomingPacket()).
class GenericDevice : public Device {
public:
    GenericDevice(const uint8_t *macAddr, uint8_t type) : Device(macAddr, type) {}

    void handlePayload(int idx, const UniversalPacket &pkt) override {
        Serial.printf("Устройство #%d: device_type=%u ещё не поддержан Хабом, payload (msg_type=%u) проигнорирован\n",
                      idx, deviceType, pkt.msg_type);
    }
};
