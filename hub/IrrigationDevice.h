#pragma once
#include "Device.h"

// Устройство полива (TYPE_IRRIGATION) - клапаны + опциональный датчик
// потока. Соответствует IrrigationSpec/IrrigationTelemetry/IrrigationCommand
// в GardenProtocol.h. Раньше эта логика жила в свободной функции
// handleIrrigationPayload() в hub.ino и в отдельном поле lastActiveValve
// прямо в KnownDevice (см. историю в DeviceManager.h) - теперь то и
// другое инкапсулировано здесь.
class IrrigationDevice : public Device {
public:
    // БИТОВАЯ МАСКА последней телеметрии - бит (v-1) = клапан v открыт (см.
    // IrrigationTelemetry.active_valves в GardenProtocol.h). До введения
    // режимов работы это был единственный номер открытого клапана
    // (lastActiveValve) - теперь маска, чтобы одинаково поддержать и
    // эксклюзивный режим (не более одного бита), и независимый (несколько
    // бит сразу).
    uint8_t lastActiveValvesMask = 0;

    // Кэш последнего полученного MSG_CONFIG (IrrigationSpec) - текущая
    // ДЕЙСТВУЮЩАЯ конфигурация узла, А НЕ то, что Хаб ей когда-либо задавал
    // (см. handlePayload() ниже) - узел мог перезагрузиться/перепрошиться
    // с другим значением по умолчанию/отклонить последний MSG_SET_CONFIG (см.
    // onSetConfig() в flow_node.ino) - эти поля всегда отражают то, что узел
    // ПОДТВЕРДИЛ в ответном эхо, а не то, что Хаб отправил. 0 - ещё ни разу
    // не получено (валидный диапазон valve_count - 1..5, mode - 1..2).
    uint8_t valveCount = 0;
    uint8_t mode = 0;
    uint8_t hasFlowSensor = 0;   // теперь конфигурируется с Хаба (см. GardenProtocol.h::IrrigationConfigSet), а не
                                 // аппаратный факт узла - отображает, что в последнем подтверждённом узлом MSG_CONFIG.
    uint16_t flowPulsesPerLiter = 0; // то же самое для разрешения датчика - 0 ещё ни разу не получено.

    IrrigationDevice(const uint8_t *macAddr) : Device(macAddr, TYPE_IRRIGATION) {}

    void handlePayload(int idx, const UniversalPacket &pkt) override;
    void appendJsonFields(String &json) const override;
};
