#pragma once
#include "Device.h"

// Максимальное число клапанов на одном узле полива - то же ограничение, что и у valve_count в
// sendSetConfig()/handleApiSetConfig() в hub.ino (там же 1..5) - вынесено сюда отдельной
// константой, чтобы размер массива valveSchedules ниже и размер соответствующей
// записи в DeviceManager::InstalledDeviceRecord (см. DeviceManager.h) были выражены одним числом,
// а не дублированы каждый раз литералом "5".
#define MAX_IRRIGATION_VALVES 5

// Настройки АВТОПОЛИВА одного клапана, задаваемые оператором в секции "Настройка"
// модального окна (см. WebPage.h) - В ОТЛИЧИЕ от valveCount/mode/hasFlowSensor/flowPulsesPerLiter выше,
// это НЕ зеркало узла и НЕ отправляется ему по ESP-NOW вообще - это чисто хабовая
// настройка, хранимая только в NVS САМОГО ХАБА (см. DeviceManager::InstalledDeviceRecord
// и DeviceManager::setValveSchedule() в DeviceManager.h/.cpp) - сам автополив по расписанию
// (то есть фактическая отправка MSG_COMMAND по таймеру) пока НЕ РЕАЛИЗОВАН - эти поля только
// хранятся и отдаются в веб-интерфейс готовыми к будущему планировщику. Дефолтные
// значения полей (до первого явного сохранения оператором) - автополив выключен, интервал
// 1 день, объём 0 - то есть заведомо безопасные значения, а не что-то случайное.
struct ValveSchedule {
    uint8_t intervalDays = 1;  // периодичность полива - раз в N дней, 1..7 (см. валидацию в
                                // DeviceManager::setValveSchedule()/handleApiSetValveSchedule() в hub.ino).
    uint16_t volumeDl = 0;     // объём за один полив, в ДЕСЯТЫХ ЛИТРА (то есть значение в литрах × 10) -
                                // целочисленный тип вместо float даёт точное хранение/сравнение без
                                // проблем округления float, точности до одного знака после запятой
                                // (как требует оператор) вполне достаточно - перевод в литры/обратно делается
                                // на границе с веб-интерфейсом/HTTP-API (см. appendJsonFields() ниже и
                                // handleApiSetValveSchedule() в hub.ino).
    uint8_t autoEnabled = 0;   // 0/1 - "активировать автополив данной линии"
};

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

    // Кэш последней полученной телеметрии по расходу (IrrigationTelemetry.total_water_used
    // в GardenProtocol.h) - литры, НАКОПИТЕЛЬНО с момента последнего сброса узла (узел ещё не
    // реализует сам датчик - всегда 0, см. TODO у fillTelemetry() в flow_node.ino). Имеет смысл
    // только при hasFlowSensor==1 (без датчика узел всегда шлёт 0) - веб-интерфейс сам решает,
    // показывать ли это поле (см. WebPage.h).
    uint32_t lastTotalWaterUsed = 0;

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

    // Настройки автополива по каждому клапану (индекс v-1 = клапан v, тот же сдвиг,
    // что и у битовой маски lastActiveValvesMask выше) - см. большой комментарий у ValveSchedule/
    // MAX_IRRIGATION_VALVES выше в этом файле - в отличие от полей выше это НЕ зеркало узла,
    // а собственная настройка Хаба, хранимая им же в NVS (см. DeviceManager::saveToNVS()/
    // loadFromNVS()/setValveSchedule() в DeviceManager.h/.cpp) - заполняется дефолтами самой
    // структуры (автополив выключен) для всех MAX_IRRIGATION_VALVES слотов сразу при
    // создании объекта - даже если на узле сейчас меньше клапанов (valveCount) - веб-интерфейс
    // всё равно показывает/редактирует только строки в пределах valveCount (см. WebPage.h) - остальные
    // просто не используются, но место под них заранее зарезервировано - если оператор
    // потом увеличит valveCount через "Конфигурацию модуля", ранее сохранённые настройки
    // новых клапанов найдутся уже готовыми (дефолтными).
    ValveSchedule valveSchedules[MAX_IRRIGATION_VALVES];

    IrrigationDevice(const uint8_t *macAddr) : Device(macAddr, TYPE_IRRIGATION) {}

    void handlePayload(int idx, const UniversalPacket &pkt) override;
    void appendJsonFields(String &json) const override;
};
