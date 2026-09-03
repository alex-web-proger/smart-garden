#include <GardenNode.h>
#include <GardenProtocol.h>

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <EEPROM.h>
#include <GardenProtocol.h>
#include <GardenNode.h>

// Настройки модуля
#define MY_DEVICE_TYPE TYPE_IRRIGATION
#define MAX_VALVES 4        // сколько клапанов ФИЗИЧЕСКИ распаяно на этой плате (см. valvePins ниже) -
                             // верхняя граница для configuredValveCount, А НЕ текущее рабочее значение -
                             // то теперь конфигурируется с Хаба и хранится в EEPROM, см. ниже.

// Датчик потока - САМ опрос (через прерывание на FLOW_SENSOR_PIN, см. ниже) уже реализован,
// но реального датчика пока нет в руках - вместо него временно работает программная
// эмуляция (FLOW_SENSOR_EMULATE, см. ниже). ОБА параметра ниже ТЕПЕРЬ КОНФИГУРИРУЕМЫЕ
// с Хаба (см. configuredHasFlowSensor/configuredFlowPulsesPerLiter ниже), а НЕ
// компиляционные константы - одна и та же прошивка может стоять на плате и с датчиком, и без него.
// Значения ниже - ТОЛЬКО стартовые дефолты до первого MSG_SET_CONFIG от Хаба (или если в EEPROM ещё
// ничего не сохранено - самая первая прошивка платы, см. loadConfig() ниже): датчик по умолчанию считается
// НЕ установленным (безопаснее, чем ошибочно считать его присутствующим и принимать мусорные показания
// за реальный расход), а разрешение - типичное для распространённых герконовых датчиков вроде YF-S201 -
// в любом случае только стартовое значение, реальное должно быть выставлено оператором через Хаб под
// конкретный установленный датчик.
#define DEFAULT_HAS_FLOW_SENSOR 0
#define DEFAULT_FLOW_PULSES_PER_LITER 450

// Границы валидации flow_pulses_per_liter при приёме MSG_SET_CONFIG (см. onSetConfig() ниже) -
// с большим запасом над любым реальным датчиком - просто отсекают заведомо мусорный ввод (например, 0).
#define MIN_FLOW_PULSES_PER_LITER 1
#define MAX_FLOW_PULSES_PER_LITER 20000

// Пины, на которые подключены светодиоды (через резистор) - на месте
// реального МОСФЕТ-ключа, управляющего клапаном. Подробности выбора
// именно этих GPIO - см. PROTOCOL.md §6.1.
const uint8_t valvePins[MAX_VALVES] = {5, 4, 14, 12};

// Вход датчика потока (импульсный, типа геркона датчика Холла) - ВЫБРАН 13-й GPIO,
// потому что он поддерживает аппаратное прерывание на ESP8266 (в отличие, например,
// от GPIO16) и не пересекается с valvePins выше/STATUS_LED ниже и загрузочными пинами
// ESP8266 (GPIO0/2/15/16). На NodeMCU/Wemos D1 mini обозначается как D7.
#define FLOW_SENSOR_PIN 13

// FLOW_SENSOR_EMULATE=1 - РЕАЛЬНОГО датчика пока физически нет: вместо чтения
// FLOW_SENSOR_PIN узел САМ программно генерирует импульсы, как будто вода текла через
// открытый клапан с заданным для него расходом (см. emulatedFlowMlPerMin[] и
// updateFlowEmulation() ниже) - удобно проверить весь путь "импульсы -> л/мин ->
// накопленный объём -> телеметрия -> Хаб -> веб-интерфейс" ещё до того, как датчик
// появится в руках. ВАЖНО: вся дальнейшая обработка импульсов (processFlowPulses()
// ниже) ОДИНАКОВА в обоих режимах - она не знает и не должна знать, откуда взялся
// очередной импульс в общем счётчике flowPulseCount ниже - поэтому переключение на
// реальный датчик позже сводится к переключению этого одного флага в 0.
#define FLOW_SENSOR_EMULATE 1

#define WATCHDOG_TIMEOUT_MS 300000UL
#define TELEMETRY_INTERVAL_MS 10000UL
#define TELEMETRY_JITTER_MS 2000UL
#define CONFIG_INTERVAL_MS 3600000UL
#define CONFIG_JITTER_MS 300000UL

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t myMac[6];

GardenNode node; // вся протокольная логика (dedup/джиттер/watchdog/ACK) - в библиотеке

uint8_t activeValvesMask = 0; // БИТОВАЯ МАСКА: бит (i) = клапан (i+1) открыт. 0 - всё
                               // закрыто. В mode=1 (эксклюзивный) установлен не более одного бита
                               // (onCommand() ниже сам это гарантирует), в mode=2 (независимый)
                               // может быть установлено несколько битов сразу. См. IrrigationTelemetry.active_valves
                               // в GardenProtocol.h.

// --- Дозирование по объёму (IrrigationCommand.mode==2, см. GardenProtocol.h) - третий режим
// открытия клапана, в дополнение к mode=0 (по времени) и легаси-mode=1 (по целым литрам,
// так и не реализован). В отличие от них, здесь узел САМ закрывает клапан, как только
// накопленный объём достигает цели - см. checkDosing() ниже, вызывается из loop().
// Переменные объявлены здесь же, а не рядом с checkDosing() - тот же принцип, что и у
// configuredValveCount/activeValvesMask выше: читаются/пишутся из нескольких мест файла
// (onCommand()/checkDosing()/onWatchdogTimeout()/onSetConfig() ниже).
bool doseActive = false;    // true - прямо сейчас идёт дозирование одного клапана (doseValve ниже)
uint8_t doseValve = 0;      // номер дозируемого клапана (1..N), бессмыслен при doseActive==false
uint32_t doseTargetMl = 0;  // целевой объём в мл (1 дл = 100 мл)
uint32_t doseStartMl = 0;   // снимок totalWaterUsedMl на момент старта дозирования - так как
                             // totalWaterUsedMl накапливается С МОМЕНТА ВКЛЮЧЕНИЯ узла (а не с начала
                             // текущего дозирования), checkDosing() сравнивает С РАЗНИЦЕЙ
                             // (totalWaterUsedMl - doseStartMl) с doseTargetMl, а не totalWaterUsedMl напрямую.

// --- Конфигурация, задаваемая Хабом (MSG_SET_CONFIG) - объявлена ЗДЕСЬ, выше блока
// датчика потока ниже (вместо своего обычного места рядом с saveConfig()/loadConfig() ниже),
// потому что updateFlowEmulation()/processFlowPulses() читают configuredHasFlowSensor/
// configuredFlowPulsesPerLiter, а в C++ глобальная переменная должна быть объявлена ВЫШЕ
// места использования (в отличие от функций, для которых Arduino IDE сама генерирует
// прототипы). До первого успешного loadConfig() (или пока в EEPROM ещё ничего не
// записано, самая первая прошивка платы) действуют значения по умолчанию ниже.
uint8_t configuredValveCount = MAX_VALVES;
uint8_t configuredMode = 1;
uint8_t configuredHasFlowSensor = DEFAULT_HAS_FLOW_SENSOR;
uint16_t configuredFlowPulsesPerLiter = DEFAULT_FLOW_PULSES_PER_LITER;

// --- Датчик потока: общий счётчик импульсов и его источники ---

// Сырой накапливающийся счётчик импульсов с момента включения - НИКОГДА не обнуляется,
// processFlowPulses() ниже сам следит за разницей с предыдущим чтением. Пишется из ISR
// (FLOW_SENSOR_EMULATE=0) или из loop() через updateFlowEmulation() (FLOW_SENSOR_EMULATE=1) -
// в обоих случаях чтение из processFlowPulses() должно быть атомарным (см. там noInterrupts()/
// interrupts()) - отсюда volatile.
volatile uint32_t flowPulseCount = 0;

#if !FLOW_SENSOR_EMULATE
// ISR реального датчика - ОБЯЗАНА быть IRAM_ATTR на ESP8266 (иначе крэш при отключённом
// от flash-кэша контексте прерывания) и предельно короткой - никакого Serial, делений
// и т.п. внутри, только инкремент - вся остальная обработка (перевод в литры/расход) - в
// processFlowPulses() ниже, вне ISR.
IRAM_ATTR void onFlowPulseISR() {
    flowPulseCount++;
}
#endif

#if FLOW_SENSOR_EMULATE
// Целевой расход КАЖДОГО канала при эмуляции (мл/мин) - клапан 1 → 5 л/мин, клапан 2 →
// 6 л/мин и т.д. (по требованию - у каждого канала свой разный расход). Индекс i
// соответствует клапану (i+1), как и во всех остальных битовых масках проекта
// (activeValvesMask и т.п.). Если MAX_VALVES выше когда-нибудь изменится - дополнить этот
// массив до того же размера. Используется ТОЛЬКО при FLOW_SENSOR_EMULATE=1.
const uint32_t emulatedFlowMlPerMin[MAX_VALVES] = {5000, 6000, 7000, 8000};

uint32_t lastEmulationMs = 0;   // millis() последнего вызова updateFlowEmulation()
double emulatedMlAccum = 0.0;   // накопленные, но ещё не конвертированные в целый импульс "мл" -
                                 // без этого остатка при частых вызовах loop() дробная часть мл каждый раз
                                 // отбрасывалась бы округлением, и реальный воспроизводимый расход выше
                                 // был бы занижен, чем задано.

// Вызывается каждую итерацию loop() (НЕ раз в TELEMETRY_INTERVAL_MS - иначе при открытии
// клапана прямо перед плановой телеметрией мы бы "проспали" почти весь интервал накопления
// импульсов и получили бы резкий скачок вместо плавного счёта). Считает суммарный целевой
// расход по всем открытым СЕЙЧАС клапанам (emulatedFlowMlPerMin[]) и генерирует ровно
// столько импульсов, сколько реальный датчик с разрешением configuredFlowPulsesPerLiter
// сгенерировал бы за прошедшее время при таком расходе - т.е. эмуляция "знает" истинный
// физический расход в мл/мин, а не подделывает частоту импульсов напрямую - поэтому смена
// configuredFlowPulsesPerLiter (даже через Хаб на лету) не меняет воспроизводимый расход, только
// частоту импульсов.
void updateFlowEmulation() {
    uint32_t now = millis();
    uint32_t elapsedMs = now - lastEmulationMs; // корректно и при переполнении millis()
    lastEmulationMs = now;
    if (elapsedMs == 0) return;

    if (!configuredHasFlowSensor || activeValvesMask == 0) {
        // Датчика "нет" по конфигурации или всё закрыто - реальных импульсов быть не
        // должно - сбрасываем накопленную дробную часть, чтобы при следующем открытии клапана не
        // начать с неё же (из-за простоя в закрытом состоянии она всё равно равна 0).
        emulatedMlAccum = 0.0;
        return;
    }

    uint32_t totalMlPerMin = 0;
    for (uint8_t i = 0; i < MAX_VALVES; i++) {
        if (activeValvesMask & (1 << i)) totalMlPerMin += emulatedFlowMlPerMin[i];
    }

    emulatedMlAccum += (double) totalMlPerMin * elapsedMs / 60000.0;
    // Сколько ЦЕЛЫХ импульсов соответствует накопленным мл при ТЕКУЩЕМ разрешении - остаток
    // (дробные мл) остаётся в emulatedMlAccum до следующего вызова.
    uint32_t pulses = (uint32_t) (emulatedMlAccum * configuredFlowPulsesPerLiter / 1000.0);
    if (pulses > 0) {
        flowPulseCount += pulses;
        emulatedMlAccum -= (double) pulses * 1000.0 / configuredFlowPulsesPerLiter;
    }
}
#endif

// Частота опроса счётчика импульсов (processFlowPulses() ниже) - НЕ привязана к
// TELEMETRY_INTERVAL_MS, чтобы current_flow оставался свежим даже между отправками телеметрии
// (а не только в момент её сборки) - и чтобы окно усреднения было достаточно широким для
// точности при небольших configuredFlowPulsesPerLiter.
#define FLOW_PROCESS_INTERVAL_MS 1000UL

uint32_t lastFlowProcessMs = 0;        // millis() последней обработки импульсов
uint32_t lastProcessedPulseCount = 0;  // значение flowPulseCount на момент предыдущей обработки
uint32_t totalWaterUsedMl = 0;         // накоплено с момента включения, в мл (точнее, чем хранить сразу
                                        // в целых литрах - в литры для IrrigationTelemetry.total_water_used
                                        // конвертируется только в fillTelemetry())
uint32_t currentFlowMlPerMin = 0;      // последний вычисленный мгновенный расход - читается из
                                        // fillTelemetry(), пишется только здесь, в processFlowPulses()

// Общая обработка накопленных импульсов - ОДИНАКОВА для реального датчика и эмуляции:
// оба источника пишут в один и тот же flowPulseCount выше, а эта функция лишь читает его -
// переключение на реальный датчик (FLOW_SENSOR_EMULATE=0) её не затрагивает.
void processFlowPulses() {
    uint32_t now = millis();
    if (now - lastFlowProcessMs < FLOW_PROCESS_INTERVAL_MS) return;
    uint32_t elapsedMs = now - lastFlowProcessMs;
    lastFlowProcessMs = now;

    // Атомарное чтение - flowPulseCount могут менять ISR (FLOW_SENSOR_EMULATE=0) в любой
    // момент - без отключения прерываний чтение 32-битного значения на 8-битной AVR-подобной
    // архитектуре было бы неатомарным (на ESP8266/32-бит это на самом деле и так атомарно, но
    // явное noInterrupts()/interrupts() делает это не зависящим от платформы).
    noInterrupts();
    uint32_t pulsesNow = flowPulseCount;
    interrupts();

    uint32_t deltaPulses = pulsesNow - lastProcessedPulseCount; // корректно и при переполнении uint32_t
    lastProcessedPulseCount = pulsesNow;

    if (!configuredHasFlowSensor) {
        // Датчика по конфигурации "нет" - даже если на входе случайные наведённые импульсы
        // (плавающий неподключённый вход при FLOW_SENSOR_EMULATE=0), не трактуем их как расход -
        // текущий расход считаем нулевым, накопленный объём не трогаем.
        currentFlowMlPerMin = 0;
        return;
    }

    if (deltaPulses > 0 && configuredFlowPulsesPerLiter > 0) {
        uint32_t deltaMl = (uint32_t) ((uint64_t) deltaPulses * 1000UL / configuredFlowPulsesPerLiter);
        totalWaterUsedMl += deltaMl;
        currentFlowMlPerMin = (uint32_t) ((uint64_t) deltaMl * 60000UL / elapsedMs);
    } else {
        currentFlowMlPerMin = 0; // импульсов за этот интервал не было - расход сейчас нулевой
    }
}

// Третий режим открытия клапана (IrrigationCommand.mode==2) - точная дозировка объёма.
// Вызывается из loop() ПОСЛЕ processFlowPulses() (только он обновляет totalWaterUsedMl,
// поэтому порядок вызова в loop() важен - иначе сравнение ниже видело бы устаревшееся
// значение). Как только накопленный С МОМЕНТА СТАРТА ДОЗИРОВАНИЯ объём достигает
// цели - закрывает клапан сам, не дожидаясь отдельной команды close от Хаба.
//
// ТОЧНОСТЬ: привязана к гранулярности processFlowPulses() (FLOW_PROCESS_INTERVAL_MS=1000Мс) -
// цель может быть превышена на объём, прошедший за последнюю секунду перед тем, как измерение
// догонит цель (при типичных расходах это доли литра - для требуемой точности 0.1 л это
// приемлемый компромисс - для большей точности пришлось бы уменьшать FLOW_PROCESS_INTERVAL_MS).
void checkDosing() {
    if (!doseActive) return;

    uint32_t dispensedMl = totalWaterUsedMl - doseStartMl; // корректно и при переполнении uint32_t
    if (dispensedMl < doseTargetMl) return;

    uint8_t bit = (uint8_t) (1 << (doseValve - 1));
    activeValvesMask &= ~bit;
    applyValveState(activeValvesMask);
    doseActive = false;

    Serial.printf("Дозирование клапана %u завершено: вылито %lu.%01lu л (цель была %lu.%01lu л)\n",
                  doseValve,
                  (unsigned long) dispensedMl / 1000UL, (unsigned long) (dispensedMl % 1000UL) / 100UL,
                  (unsigned long) doseTargetMl / 1000UL, (unsigned long) (doseTargetMl % 1000UL) / 100UL);

    if (activeValvesMask == 0) node.disarmWatchdog();
    node.sendTelemetryNow();
}

// --- Конфигурация, задаваемая Хабом (MSG_SET_CONFIG) и хранимая на самом
// узле в EEPROM - переживает перезагрузку, в отличие от activeValvesMask выше.
// Сами переменные configuredValveCount/configuredMode/configuredHasFlowSensor/
// configuredFlowPulsesPerLiter объявлены ВЫШЕ (рядом с activeValvesMask, до блока датчика
// потока) - см. комментарий там почему.

// Метка "конфигурация в EEPROM записана осмысленно" - отличает "узел уже
// настраивали" от "EEPROM только что стёрт/чип новый" (обычно там 0xFF или мусор) -
// без неё узел после первой же прошивки принял бы случайный мусор из непрограммированной
// флеш-памяти за валидную конфигурацию.
// v2 (был 0x53): добавлены hasFlowSensor/pulsesPerLiter - магия поднята специально (а не
// оставлена прежней), чтобы старый EEPROM с уже работавших в поле плат не был ошибочно принят
// за уже содержащий новые поля (байты за прежним размером структуры никто туда не писал - при
// чтении большей структуры там был бы мусор). Смена magic заставляет loadConfig() отклонить
// старую запись как "нет валидной конфигурации" и один раз откатиться к дефолтам выше.
#define EEPROM_MAGIC 0x54
struct PersistedConfig {
    uint8_t magic;
    uint8_t valveCount;
    uint8_t mode;
    uint8_t hasFlowSensor;
    uint16_t pulsesPerLiter;
};

void saveConfig() {
    PersistedConfig cfg;
    cfg.magic = EEPROM_MAGIC;
    cfg.valveCount = configuredValveCount;
    cfg.mode = configuredMode;
    cfg.hasFlowSensor = configuredHasFlowSensor;
    cfg.pulsesPerLiter = configuredFlowPulsesPerLiter;
    EEPROM.put(0, cfg);
    EEPROM.commit();
}

// Вызывается ОДИН РАЗ в setup(), до esp_now_init()/node.begin() - читает то, что
// Хаб мог задать ещё ДО этой перезагрузки (см. onSetConfig() ниже), в оперативные
// configuredValveCount/configuredMode/configuredHasFlowSensor/configuredFlowPulsesPerLiter выше - до того, как
// они кому-либо понадобятся (fillConfig()/applyValveState() и т.п.).
void loadConfig() {
    EEPROM.begin(sizeof(PersistedConfig));
    PersistedConfig cfg;
    EEPROM.get(0, cfg);
    if (cfg.magic == EEPROM_MAGIC && cfg.valveCount >= 1 && cfg.valveCount <= MAX_VALVES &&
        (cfg.mode == 1 || cfg.mode == 2) && (cfg.hasFlowSensor == 0 || cfg.hasFlowSensor == 1) &&
        cfg.pulsesPerLiter >= MIN_FLOW_PULSES_PER_LITER && cfg.pulsesPerLiter <= MAX_FLOW_PULSES_PER_LITER) {
        configuredValveCount = cfg.valveCount;
        configuredMode = cfg.mode;
        configuredHasFlowSensor = cfg.hasFlowSensor;
        configuredFlowPulsesPerLiter = cfg.pulsesPerLiter;
        Serial.printf("Конфигурация восстановлена из EEPROM: valve_count=%u mode=%u has_flow_sensor=%u pulses_per_liter=%u\n",
                      configuredValveCount, configuredMode, configuredHasFlowSensor, configuredFlowPulsesPerLiter);
    } else {
        Serial.println("В EEPROM нет валидной конфигурации - используются значения по умолчанию.");
        saveConfig(); // сразу же записываем дефолт - чтобы magic был выставлен для следующей загрузки
    }
}

void applyValveState(uint8_t mask) {
    for (uint8_t i = 0; i < MAX_VALVES; i++) {
        digitalWrite(valvePins[i], (mask & (1 << i)) ? HIGH : LOW);
    }
}

// Встроенный светодиод на плате (GPIO2, active LOW). Не используется для
// клапанов — см. README — но удобен как индикатор передачи в эфир.
#define STATUS_LED LED_BUILTIN

// Транспорт - единственное, что зависит от платформы (ESP8266 vs ESP32),
// поэтому живёт в скетче, а не в библиотеке.
bool sendRaw(const uint8_t *data, size_t len) {
    digitalWrite(STATUS_LED, LOW);
    bool ok = esp_now_send(broadcastAddress, (uint8_t *) data, len) == 0;
    delay(50);
    digitalWrite(STATUS_LED, HIGH);
    return ok;
}

// --- Колбэки под конкретный домен устройства (полив) ---

void fillConfig(UniversalPacket &pkt) {
    // Текущие ДЕЙСТВУЮЩИЕ значения (после возможного более раннего
    // MSG_SET_CONFIG от Хаба, см. onSetConfig() ниже) - НЕ компиляционные
    // константы, см. комментарий у IrrigationSpec в GardenProtocol.h.
    pkt.payload.irrigation.spec.valve_count = configuredValveCount;
    pkt.payload.irrigation.spec.has_flow_sensor = configuredHasFlowSensor;
    pkt.payload.irrigation.spec.mode = configuredMode;
    pkt.payload.irrigation.spec.flow_pulses_per_liter = configuredFlowPulsesPerLiter;
}

void fillTelemetry(UniversalPacket &pkt) {
    pkt.payload.irrigation.telemetry.active_valves = activeValvesMask;
    // Оба поля - из processFlowPulses() выше (вызывается из loop() независимо от телеметрии,
    // см. там) - уже учитывают configuredHasFlowSensor сами (оба занулены, если датчика "нет").
    pkt.payload.irrigation.telemetry.current_flow = currentFlowMlPerMin;
    pkt.payload.irrigation.telemetry.total_water_used = totalWaterUsedMl / 1000UL;
}

uint8_t onCommand(const UniversalPacket &pkt) {
    IrrigationCommand cmd = pkt.payload.irrigation.command;
    Serial.printf("COMMAND RECEIVED: valve=%u action=%u mode=%u duration_sec=%u volume_l=%u volume_dl=%u\n",
                  cmd.target_valve, cmd.action, cmd.mode, cmd.duration_sec, cmd.volume_l, cmd.volume_dl);

    // Клапан за пределами ТЕКУЩЕЙ сконфигурированной ёмкости (см.
    // configuredValveCount/onSetConfig() ниже) - отклоняем, а не молча
    // открываем несуществующий/отключённый по конфигурации канал.
    // target_valve==0 разрешён всегда (имеет смысл только с action=ACTION_CLOSE, см. ниже),
    // вне зависимости от configuredValveCount.
    if (cmd.target_valve > configuredValveCount) {
        Serial.printf("COMMAND отклонена: valve=%u выходит за пределы текущего valve_count=%u\n",
                      cmd.target_valve, configuredValveCount);
        return 1;
    }

    if (cmd.action == ACTION_CLOSE) {
        // Закрытие работает ОДИНАКОВО в обоих режимах - затрагивает
        // ТОЛЬКО свой бит (или все сразу при target_valve==0) - никакой
        // зависимости от configuredMode здесь нет (в отличие от ACTION_OPEN ниже).
        // Если закрываемый клапан (или все сразу) сейчас дозируется (doseActive) -
        // отменяем дозирование, иначе checkDosing() ниже позже попытался бы снова
        // закрыть уже закрытый клапан/перепутать битовую маску какого-то другого клапана.
        if (cmd.target_valve == 0) {
            activeValvesMask = 0;
            doseActive = false;
        } else {
            activeValvesMask &= ~(uint8_t) (1 << (cmd.target_valve - 1));
            if (doseActive && doseValve == cmd.target_valve) doseActive = false;
        }
    } else { // ACTION_OPEN
        if (cmd.target_valve == 0) {
            Serial.println("COMMAND отклонена: ACTION_OPEN с target_valve=0 не имеет смысла");
            return 1;
        }
        uint8_t bit = (uint8_t) (1 << (cmd.target_valve - 1));

        if (cmd.mode == 2) {
            // Третий режим - точная дозировка объёма с точностью до десятых литра
            // (volume_dl, см. GardenProtocol.h). Требует датчик потока - без него узлу
            // нечем измерить вылитый объём, поэтому отклоняем команду через MSG_ACK{status=1},
            // а не молча открываем клапан без ограничения.
            if (!configuredHasFlowSensor) {
                Serial.println("COMMAND отклонена: дозирование (mode=2) требует датчик потока (has_flow_sensor=0)");
                return 1;
            }
            if (cmd.volume_dl == 0) {
                Serial.println("COMMAND отклонена: дозирование (mode=2) с volume_dl=0 не имеет смысла");
                return 1;
            }
            // ФОРСИРОВАННО эксклюзивно - всегда ровно один клапан, вне зависимости от
            // configuredMode - смешанный поток через несколько клапанов сделал бы измерение
            // бессмысленным (общий счётчик импульсов датчика не различает, через какой
            // клапан прошёл конкретный импульс).
            activeValvesMask = bit;
            doseActive = true;
            doseValve = cmd.target_valve;
            doseTargetMl = (uint32_t) cmd.volume_dl * 100UL; // десятые литра -> мл
            doseStartMl = totalWaterUsedMl;
            Serial.printf("Дозирование запущено: клапан=%u целевой объём=%u.%u л\n",
                          cmd.target_valve, cmd.volume_dl / 10, cmd.volume_dl % 10);
        } else {
            doseActive = false; // любое не-дозирующее открытие отменяет текущее дозирование, если оно шло
            if (configuredMode == 1) {
                // Режим 1 (эксклюзивный) - открытие ЛЮБОГО клапана автоматически
                // закрывает все остальные - исходное поведение проекта.
                activeValvesMask = bit;
            } else {
                // Режим 2 (независимый) - открытие этого клапана НЕ трогает
                // состояние остальных.
                activeValvesMask |= bit;
            }
        }
    }

    // Реальное управление GPIO. Для легаси-mode==1 (полив по объёму в целых
    // литрах, дозировка команды - не путать с configuredMode) расчёт по датчику потока
    // так и не реализован - клапан просто открывается и остаётся открытым, как при mode==0 без
    // таймера (TODO). Для mode==2 (точная дозировка) автоматическое закрытие по
    // достижению цели выполняет checkDosing() ниже (вызывается из loop()), а не эта функция.
    applyValveState(activeValvesMask);

    if (activeValvesMask != 0) node.armWatchdog();
    else node.disarmWatchdog();

    // Обычная телеметрия шлётся по таймеру раз в ~10 сек (см.
    // TELEMETRY_INTERVAL_MS/JITTER выше) - для физического открытия/
    // закрытия клапана (которое уже произошло СТРОКОЙ ВЫШЕ) это заметная
    // задержка на стороне Хаба/веб-интерфейса: клапан щёлкнул сразу, а
    // индикация в браузере ждёт следующего планового тика. sendTelemetryNow()
    // - штатный метод GardenNode именно под такие событийные изменения
    // состояния (см. его описание в GardenNode.h) - отправляет
    // внеочередной MSG_TELEMETRY немедленно, не трогая и не сбрасывая сам
    // плановый таймер. Если этот конкретный пакет потеряется в эфире -
    // не страшно (в отличие от дискретных однократных событий вроде
    // нажатия кнопки, предостережение в GardenNode.h касается именно их):
    // activeValvesMask - это уровень состояния, а не разовое событие, и
    // очередная плановая телеметрия его в любом случае подтвердит.
    node.sendTelemetryNow();

    return 0; // 0 = команда принята (узел отклоняет только выход target_valve за диапазон или ACTION_OPEN с target_valve=0)
}

// Новая конфигурация от Хаба (MSG_SET_CONFIG) - валидирует, применяет и СОХРАНЯЕТ на
// себе (EEPROM), в отличие от onCommand() выше (разовое действие, ничего
// не сохраняет). Возвращаемое значение - статус для авто-ACK библиотеки (0 -
// принято и применено, 1 - отклонено), и заодно - признак для
// GardenNode::handleIncoming() - слать ли немедленное MSG_CONFIG-эхо (см. GardenNode.h/.cpp).
uint8_t onSetConfig(const UniversalPacket &pkt) {
    IrrigationConfigSet cfg = pkt.payload.irrigation.configSet;

    // Валидация диапазона - ИМЕННО здесь, а не на Хабе: только узел
    // знает свою реальную аппаратную ёмкость (MAX_VALVES этой конкретной
    // платы, см. GardenProtocol.h у IrrigationConfigSet).
    if (cfg.valve_count < 1 || cfg.valve_count > MAX_VALVES) {
        Serial.printf("SET_CONFIG отклонён: valve_count=%u вне диапазона 1..%u\n",
                      cfg.valve_count, MAX_VALVES);
        return 1;
    }
    if (cfg.mode != 1 && cfg.mode != 2) {
        Serial.printf("SET_CONFIG отклонён: mode=%u не поддержан (допустимо 1 или 2)\n", cfg.mode);
        return 1;
    }
    // has_flow_sensor - чисто операторский выбор (см. комментарий у
    // IrrigationConfigSet.has_flow_sensor в GardenProtocol.h) - узел не валидирует его против
    // какого-либо аппаратного факта, только проверяет, что это валидный bool (0/1),
    // а не мусор.
    if (cfg.has_flow_sensor != 0 && cfg.has_flow_sensor != 1) {
        Serial.printf("SET_CONFIG отклонён: has_flow_sensor=%u вне диапазона 0..1\n", cfg.has_flow_sensor);
        return 1;
    }
    if (cfg.flow_pulses_per_liter < MIN_FLOW_PULSES_PER_LITER || cfg.flow_pulses_per_liter > MAX_FLOW_PULSES_PER_LITER) {
        Serial.printf("SET_CONFIG отклонён: flow_pulses_per_liter=%u вне диапазона %u..%u\n",
                      cfg.flow_pulses_per_liter, MIN_FLOW_PULSES_PER_LITER, MAX_FLOW_PULSES_PER_LITER);
        return 1;
    }

    configuredValveCount = cfg.valve_count;
    configuredMode = cfg.mode;
    configuredHasFlowSensor = cfg.has_flow_sensor;
    configuredFlowPulsesPerLiter = cfg.flow_pulses_per_liter;
    saveConfig();

    // Если valve_count уменьшился и какие-то биты оказались за пределами
    // нового диапазона - сбрасываем их, чтобы не остался "залипший" открытый
    // канал, которым по новой конфигурации якобы уже нельзя управлять.
    uint8_t validMask = configuredValveCount >= 8 ? 0xFF : (uint8_t) ((1 << configuredValveCount) - 1);
    if (activeValvesMask & ~validMask) {
        activeValvesMask &= validMask;
        applyValveState(activeValvesMask);
        node.sendTelemetryNow();
    }

    // Если переключились в эксклюзивный режим (mode=1), пока открыто больше
    // одного клапана (наследие независимого режима) - непонятно, какой
    // из них теперь "главный", поэтому безопаснее закрыть всё и ждать явную
    // новую команду, чем произвольно выбрать один из открытых.
    if (configuredMode == 1 && (activeValvesMask & (activeValvesMask - 1)) != 0) {
        activeValvesMask = 0;
        applyValveState(0);
        node.sendTelemetryNow();
    }

    // Любое из двух принудительных закрытий выше могло затронуть бит дозируемого клапана -
    // если так, дозирование нельзя продолжать - иначе checkDosing() позже попытался бы
    // снять бит уже закрытого клапана или вовсе чужого, если номера клапанов переиспользовались.
    if (doseActive && !(activeValvesMask & (uint8_t) (1 << (doseValve - 1)))) {
        doseActive = false;
    }

    Serial.printf("SET_CONFIG применён и сохранён: valve_count=%u mode=%u has_flow_sensor=%u pulses_per_liter=%u\n",
                  configuredValveCount, configuredMode, configuredHasFlowSensor, configuredFlowPulsesPerLiter);
    return 0;
}

void onWatchdogTimeout() {
    activeValvesMask = 0;
    doseActive = false; // потеря связи с Хабом обрывает и любое текущее дозирование, не только обычные клапаны
    applyValveState(0);
    // Аналогично onCommand() выше - автоматическое закрытие по watchdog
    // тоже реальное изменение состояния клапана, о котором Хаб должен
    // узнать как можно быстрее, а не только на следующем плановом тике
    // телеметрии.
    node.sendTelemetryNow();
}

// --- Транспортный recv-колбэк (ESP8266-специфичная сигнатура) ---
void onDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
    if (len != sizeof(UniversalPacket)) return;
    UniversalPacket pkt;
    memcpy(&pkt, incomingData, sizeof(pkt));
    node.handleIncoming(pkt);
}

void setup() {
    Serial.begin(115200);

    // ДО pinMode/esp_now - в оперативные configuredValveCount/configuredMode
    // читается то, что Хаб мог задать ещё до этой перезагрузки (см.
    // loadConfig() выше).
    loadConfig();

    for (uint8_t i = 0; i < MAX_VALVES; i++) pinMode(valvePins[i], OUTPUT);
    applyValveState(0);

#if !FLOW_SENSOR_EMULATE
    // INPUT_PULLUP - геркон/открытый коллектор типичного импульсного датчика Холла (например,
    // YF-S201) в паузах между импульсами оставляет линию "в воздухе" - без подтяжки она была бы
    // плавать и давать ложные срабатывания FALLING. Компилируется ТОЛЬКО при выключенной
    // эмуляции (см. FLOW_SENSOR_EMULATE в начале файла) - чтобы не держать вход в
    // неопределённом состоянии, пока реальный датчик не подключён.
    pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), onFlowPulseISR, FALLING);
#endif

    pinMode(STATUS_LED, OUTPUT);
    digitalWrite(STATUS_LED, HIGH);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    WiFi.macAddress(myMac);
    Serial.print("My MAC: ");
    for (int i = 0; i < 6; i++) Serial.printf("%02X%c", myMac[i], i < 5 ? ':' : '\n');

    if (esp_now_init() != 0) {
        Serial.println("ESP-NOW Init Failed");
        return;
    }
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(onDataRecv);
    esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);

    node.begin(MY_DEVICE_TYPE, myMac, sendRaw);
    node.setCallbacks(fillConfig, fillTelemetry, onCommand, onWatchdogTimeout);
    node.setConfigHandler(onSetConfig);
    node.setTiming(TELEMETRY_INTERVAL_MS, TELEMETRY_JITTER_MS,
                   CONFIG_INTERVAL_MS, CONFIG_JITTER_MS, WATCHDOG_TIMEOUT_MS);
    node.sendConfig(); // заявляем о себе сразу при включении
}

void loop() {
    node.loop();
#if FLOW_SENSOR_EMULATE
    updateFlowEmulation(); // каждую итерацию - пишет в flowPulseCount, см. комментарий там
#endif
    processFlowPulses(); // сам себя троттлит до FLOW_PROCESS_INTERVAL_MS, можно звать каждую итерацию
    checkDosing(); // ОБЯЗАТЕЛЬНО ПОСЛЕ processFlowPulses() - см. комментарий у неё
}
