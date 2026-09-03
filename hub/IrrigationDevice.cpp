#include <GardenNode.h>
#include <GardenProtocol.h>

#include <GardenNode.h>
#include <GardenProtocol.h>

#include <GardenNode.h>
#include <GardenProtocol.h>

#include "IrrigationDevice.h"

void IrrigationDevice::handlePayload(int idx, const UniversalPacket &pkt) {
    if (pkt.msg_type == MSG_CONFIG) {
        IrrigationSpec spec = pkt.payload.irrigation.spec;
        valveCount = spec.valve_count;
        hasFlowSensor = spec.has_flow_sensor;
        mode = spec.mode;
        flowPulsesPerLiter = spec.flow_pulses_per_liter;
        Serial.printf("CONFIG (IRRIGATION) от #%d: valve_count=%u has_flow_sensor=%u mode=%u pulses_per_liter=%u\n",
                      idx, spec.valve_count, spec.has_flow_sensor, spec.mode, spec.flow_pulses_per_liter);
    } else if (pkt.msg_type == MSG_TELEMETRY) {
        IrrigationTelemetry t = pkt.payload.irrigation.telemetry;
        lastActiveValvesMask = t.active_valves; // кэш для веб-интерфейса
        lastTotalWaterUsed = t.total_water_used; // то же для накопленного расхода (см. IrrigationDevice.h)
        Serial.printf("TELEMETRY (IRRIGATION) от #%d: active_valves=0x%02X current_flow=%lu total_water_used=%lu\n",
                      idx, t.active_valves, (unsigned long) t.current_flow, (unsigned long) t.total_water_used);
    } else {
        Serial.printf("IRRIGATION: неожиданный msg_type=%u от #%d\n", pkt.msg_type, idx);
    }
}

void IrrigationDevice::appendJsonFields(String &json) const {
    // activeValvesMask - БИТОВАЯ маска (см. комментарий у lastActiveValvesMask в
    // IrrigationDevice.h) - веб-страница сама разбирает биты под valveCount,
    // не полагаясь на "ровно один установленный бит" (см. WebPage.h).
    json += ",\"activeValvesMask\":" + String(lastActiveValvesMask);
    json += ",\"valveCount\":" + String(valveCount);
    json += ",\"mode\":" + String(mode);
    json += ",\"hasFlowSensor\":" + String(hasFlowSensor);
    json += ",\"flowPulsesPerLiter\":" + String(flowPulsesPerLiter);
    json += ",\"totalWaterUsed\":" + String(lastTotalWaterUsed);

    // Настройки автополива по каждому клапану (см. комментарий у valveSchedules в IrrigationDevice.h) -
    // всегда все MAX_IRRIGATION_VALVES элементов массива, даже если реально на узле меньше
    // клапанов (valveCount) - веб-страница сама отбрасывает лишние за пределами valveCount
    // (тот же принцип, что и с строками клапанов в секции "Управление", см. WebPage.h).
    // volumeL пересчитывается из хранимых десятых литра (volumeDl) обратно в литры с
    // одним знаком после запятой - именно эта граница с веб-интерфейсом/HTTP - единственное место,
    // где целочисленное хранение превращается в дробное число для отображения/редактирования.
    json += ",\"valveSchedules\":[";
    for (int v = 0; v < MAX_IRRIGATION_VALVES; v++) {
        if (v > 0) json += ",";
        json += "{\"intervalDays\":" + String(valveSchedules[v].intervalDays) +
                ",\"volumeL\":" + String(valveSchedules[v].volumeDl / 10.0, 1) +
                ",\"autoEnabled\":" + String(valveSchedules[v].autoEnabled ? "true" : "false") + "}";
    }
    json += "]";
}
