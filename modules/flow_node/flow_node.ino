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

// Датчик потока (сам датчик ещё не опрошит, см. TODO у fillTelemetry() ниже) - ОБА параметра
// ТЕПЕРЬ КОНФИГУРИРУЕМЫЕ с Хаба (см. configuredHasFlowSensor/configuredFlowPulsesPerLiter ниже), а НЕ
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

// --- Конфигурация, задаваемая Хабом (MSG_SET_CONFIG) и хранимая на самом
// узле в EEPROM - переживает перезагрузку, в отличие от activeValvesMask выше. До первого
// успешного loadConfig() (или пока в EEPROM ещё ничего не записано, самая первая
// прошивка платы) действуют значения по умолчанию ниже - MAX_VALVES каналов, режим 1.
uint8_t configuredValveCount = MAX_VALVES;
uint8_t configuredMode = 1;
uint8_t configuredHasFlowSensor = DEFAULT_HAS_FLOW_SENSOR;
uint16_t configuredFlowPulsesPerLiter = DEFAULT_FLOW_PULSES_PER_LITER;

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
    pkt.payload.irrigation.telemetry.current_flow = 0;      // TODO: датчик потока
    pkt.payload.irrigation.telemetry.total_water_used = 0;  // TODO: накопление + EEPROM
}

uint8_t onCommand(const UniversalPacket &pkt) {
    IrrigationCommand cmd = pkt.payload.irrigation.command;
    Serial.printf("COMMAND RECEIVED: valve=%u action=%u mode=%u duration_sec=%u volume_l=%u\n",
                  cmd.target_valve, cmd.action, cmd.mode, cmd.duration_sec, cmd.volume_l);

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
        if (cmd.target_valve == 0) {
            activeValvesMask = 0;
        } else {
            activeValvesMask &= ~(uint8_t) (1 << (cmd.target_valve - 1));
        }
    } else { // ACTION_OPEN
        if (cmd.target_valve == 0) {
            Serial.println("COMMAND отклонена: ACTION_OPEN с target_valve=0 не имеет смысла");
            return 1;
        }
        uint8_t bit = (uint8_t) (1 << (cmd.target_valve - 1));
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

    // Реальное управление GPIO. Для mode==1 (полив по объёму, дозировка
    // команды - не путать с configuredMode) расчёт по датчику потока ещё не
    // реализован - клапан просто открывается, как при mode==0 (TODO).
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

    Serial.printf("SET_CONFIG применён и сохранён: valve_count=%u mode=%u has_flow_sensor=%u pulses_per_liter=%u\n",
                  configuredValveCount, configuredMode, configuredHasFlowSensor, configuredFlowPulsesPerLiter);
    return 0;
}

void onWatchdogTimeout() {
    activeValvesMask = 0;
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
}
