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
}
