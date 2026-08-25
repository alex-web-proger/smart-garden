#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <GardenProtocol.h>
#include "WebPage.h"
#include "Device.h"
#include "DeviceManager.h"
#include "SerialCommands.h"
#include "Ds3231Rtc.h"
#include "ApButton.h"

// Настоящий Хаб проекта - работает на ESP32.
//
// ВАЖНО: API esp-now здесь (<esp_now.h> из ядра arduino-esp32 3.x)
// заметно отличается от ESP8266 (<espnow.h>):
//   - колбэк приёма: void(const esp_now_recv_info_t*, const uint8_t*, int),
//     а не void(uint8_t*, uint8_t*, uint8_t);
//   - пир регистрируется через esp_now_peer_info_t, а не отдельными
//     аргументами esp_now_add_peer(mac, role, channel, key, key_len);
//   - нет esp_now_set_self_role() - этой функции на ESP32 не существует.
// Сам протокол (GardenProtocol.h, структура UniversalPacket) общий для
// обеих платформ и не меняется - это и есть смысл разделения на
// "протокол" и "транспорт".
//
// СТРУКТУРА ФАЙЛА: логика Хаба разложена по нескольким файлам одного
// скетча (Arduino IDE показывает их отдельными вкладками того же окна):
//   - DeviceManager.h/.cpp  - таблица устройств: подключение/автообнаружение,
//     install/forget, LRU-вытеснение, сохранение в NVS.
//   - SerialCommands.h/.cpp - разбор команд Serial Monitor'а.
//   - Ds3231Rtc.h/.cpp      - опциональные часы реального времени DS3231.
//   - ApButton.h/.cpp       - антидребезг кнопки точки доступа.
//   - WebPage.h             - HTML/JS веб-интерфейса.
// В самом hub.ino остаются: протокол/ESP-NOW, веб-API (HTTP-обвязка над
// DeviceManager), часы Хаба и включение точки доступа по кнопке.
//
// ОДНОПОТОЧНОСТЬ БИЗНЕС-ЛОГИКИ: ядро arduino-esp32 вызывает колбэк
// приёма ESP-NOW (onDataRecv()) из ЗАДАЧИ СТЕКА WI-FI, а не из loop() -
// то есть без дополнительных мер таблица устройств (DeviceManager) и
// исходящий пакет (txPacket) читались и писались бы из двух разных
// задач FreeRTOS параллельно с loop() (Serial-команды, обработчики
// веб-API). Решение здесь - очередь incomingPacketQueue: колбэк только
// копирует пакет и кладёт его в очередь, а вся содержательная обработка
// (processIncomingPacket()) выполняется из loop(), разгребающего эту
// очередь. Таким образом ВСЯ бизнес-логика Хаба (включая обработку
// входящих пакетов) выполняется одной задачей - гонок не возникает без
// единой критической секции. См. подробнее у onDataRecv()/
// processIncomingPacket() ниже.
//
// ВЕБ-ИНТЕРФЕЙС: Хаб поднимает СОБСТВЕННУЮ точку доступа Wi-Fi (режим
// WIFI_AP_STA), а не подключается к домашнему роутеру. Это осознанный
// выбор ради независимости от внешней сетевой инфраструктуры (в поле
// может не быть роутера вообще, вместо него может использоваться
// GSM-модем) и ради того, чтобы не решать проблему рассинхронизации
// радиоканала с чужим роутером (см. PROTOCOL.md §7/§10): раз Хаб сам
// создаёт точку доступа, канал полностью в его руках и совпадает с
// ESPNOW_CHANNEL по определению. При этом сама точка доступа теперь
// НЕ включена постоянно - см. блок "Кнопка/точка доступа" ниже.
//
// ДОМЕННОЕ ИМЯ: веб-интерфейс, помимо http://192.168.4.1, дополнительно
// доступен по http://smartgarden.local через mDNS (см. HUB_HOSTNAME,
// restartMDNS() ниже) - удобно, чтобы не запоминать IP. Работает только
// пока точка доступа включена (запускается вместе с ней в
// setApEnabled()); поддержка на стороне КЛИЕНТА (телефона/ноутбука) не
// гарантирована на 100%: из коробки работает на iOS/macOS, обычно на
// Linux (если установлен avahi) и на большинстве современных Android;
// на Windows зависит от версии/наличия Bonjour Print Services. Если
// .local-адрес не резолвится на конкретном устройстве - достаточно
// зайти по IP (192.168.4.1).
//
// УСТАНОВКА УСТРОЙСТВ: таблица устройств (см. DeviceManager) двухуровневая.
//   - Автоматически обнаруженные (по трафику) - обычные "кандидаты":
//     подлежат LRU-вытеснению, если таблица заполнится, и живут только в
//     RAM (исчезают при перезагрузке Хаба, но быстро переоткрываются
//     заново по следующему пакету от того же MAC).
//   - "Установленные" (installed=true) - пользователь явно подтвердил
//     устройство через веб-интерфейс (или Serial-команду install). Такое
//     устройство НЕ участвует в LRU-вытеснении и переживает перезагрузку
//     Хаба - список установленных устройств сохраняется в NVS (Preferences)
//     и восстанавливается при старте. Убрать его можно только явным
//     "forget" от пользователя.
// См. PROTOCOL.md §11 для подробностей и обоснования.
//
// ЧАСЫ: Хаб ведёт внутренние часы через стандартный POSIX-интерфейс
// ESP32 (settimeofday()/time()), а не вручную считает миллисекунды от
// старта. Без внешнего RTC-модуля эти часы сбрасываются на 1970-01-01 при
// каждой перезагрузке - поэтому нужна синхронизация при каждом запуске.
// Источников времени теперь два:
//   - Браузер: страница веб-интерфейса при загрузке сама шлёт Хабу текущее
//     время (POST /api/settime) - как и раньше.
//   - DS3231 (если обнаружена, см. Ds3231Rtc.h): работает как автономный
//     источник, переживающий перезагрузку/отключение питания Хаба -
//     см. подробности при вызове rtc.begin() в setup() ниже.
// Каждая успешная синхронизация с браузером одновременно записывается и в
// DS3231 (см. handleApiSetTime()), чтобы часы не расходились.
//
// Как только Хаб получил время (от браузера или от DS3231 при старте), он
// тут же разносит его дальше по ESP-NOW всем периферийным узлам - тем же
// broadcast-пакетом MSG_CONFIG, которым объявляет о своей (пере)загрузке
// (см. sendHubAnnounce()). Узлы подхватывают его в GardenNode::handleIncoming()
// и выставляют свои собственные часы - то же решение, что и у самого
// Хаба, просто на один шаг дальше по цепочке источника времени.
//
// Время - МЕСТНОЕ, не UTC: его уже в таком виде отдаёт браузер (см. ниже и
// WebPage.h) - Хаб просто хранит полученное значение как есть и никак его
// не пересчитывает; то же самое местное время хранится и в DS3231.

// Точка доступа для веб-интерфейса. Пароль ОБЯЗАН быть от 8 символов
// (требование WPA2) - поменяйте на свой перед использованием.
const char *AP_SSID = "SmartGarden-Hub";
const char *AP_PASSWORD = "garden123";

// --- Мощность передатчика Wi-Fi/ESP-NOW (оба используют один и тот же
// радиотракт, настройка общая для обоих) - WiFi.setTxPower()/WiFi.getTxPower()
// из ядра arduino-esp32 принимают/возвращают значения типа wifi_power_t -
// фиксированный набор дискретных шагов (не любое целое число дБм), численно
// равный четвертям дБм × 4 (например, 19.5 дБм = 78) - завёрнут аппаратного API,
// мы их не придумываем заново. Список выбираемого на веб-странице (см. WebPage.h) собран
// из всех значений этого enum - валидация входящего в handleApiSetTxPower() ниже сверяется
// С ЭТИМ СПИСКОМ, а не просто с диапазоном чисел - произвольное число в диапазоне
// было бы невалидным значением этого enum и могло бы дать непредсказуемое
// поведение драйвера. УМЕНЬШЕНИЕ мощности сокращает дальность связи и может
// пригодиться для соблюдения регуляторных ограничений или снижения взаимных помех между
// несколькими изделиями в помещении.
//
// Сохраняется в NVS (см. saveTxPowerToNVS()/loadRadioSettingsFromNVS() ниже) -
// в отличие от apEnabled выше, здесь персистентность как раз то, что нужно:
// точку доступа безопаснее каждый раз включать заново явным действием оператора,
// а вот выбранную для конкретного изделия мощность передатчика, наоборот, удобнее
// применять автоматически при каждом старте, не заставляя оператора выставлять её
// заново после любого сброса/перебоя питания. При старте (см. loadRadioSettingsFromNVS(),
// вызывается из setup()) восстановленное значение всё равно сверяется С НАБОРОМ
// TX_POWER_OPTIONS ниже - та же защита, что и в handleApiSetTxPower() - если в NVS
// вдруг окажется значение вне этого набора (повреждение/старая прошивка с другим
// набором опций), оно молча игнорируется и остаётся дефолт ядра.
struct TxPowerOption {
    wifi_power_t value;
    const char *label; // для Serial-лога и как подсказка при отладке - веб-страница сама
                        // форматирует текст из числа (см. WebPage.h), это поле ей не отдаётся.
};
const TxPowerOption TX_POWER_OPTIONS[] = {
    {WIFI_POWER_19_5dBm, "19.5 dBm"},
    {WIFI_POWER_19dBm, "19 dBm"},
    {WIFI_POWER_18_5dBm, "18.5 dBm"},
    {WIFI_POWER_17dBm, "17 dBm"},
    {WIFI_POWER_15dBm, "15 dBm"},
    {WIFI_POWER_13dBm, "13 dBm"},
    {WIFI_POWER_11dBm, "11 dBm"},
    {WIFI_POWER_8_5dBm, "8.5 dBm"},
    {WIFI_POWER_7dBm, "7 dBm"},
    {WIFI_POWER_5dBm, "5 dBm"},
    {WIFI_POWER_2dBm, "2 dBm"},
    {WIFI_POWER_MINUS_1dBm, "-1 dBm"},
};
#define TX_POWER_OPTIONS_COUNT (sizeof(TX_POWER_OPTIONS) / sizeof(TX_POWER_OPTIONS[0]))

// --- Частота процессора ---
// arduino-esp32 позволяет менять частоту CPU на лету через setCpuFrequencyMhz()/
// getCpuFrequencyMhz() (esp32-hal-cpu.h, не требует отдельного #include - часть ядра,
// подключается автоматически вместе с Arduino.h). СНИЖЕНИЕ частоты сокращает
// энергопотребление и тепловыделение, но СТОИТ осторожно - при активном Wi-Fi/ESP-NOW
// (а они активны всегда, как только Хаб работает) надёжно поддержаны только 240/160/80 МГц -
// меньшие значения (40/20/10) требуют отключения PLL и на практике ломают ESP-NOW/Wi-Fi
// (или вовсе не применяются - setCpuFrequencyMhz() вернёт false, см. handleApiSetCpuFreq() ниже) -
// восстановить связь в таком случае может потребовать физического сброса Хаба на месте -
// поэтому в список выбора на веб-странице (см. WebPage.h) сознательно не включаются ниже 80 МГц.
// Как и TX_POWER_OPTIONS выше, теперь сохраняется в NVS и восстанавливается при каждом
// старте (см. saveCpuFreqToNVS()/loadRadioSettingsFromNVS() ниже) - с той же защитой:
// восстановленное значение сверяется С НАБОРОМ CPU_FREQ_OPTIONS ниже, а не применяется
// как сырое число из NVS, поэтому повреждённое/чужое значение там не может заставить
// ядро попытаться выставить что-то за пределами заведомо проверенных 240/160/80 МГц.
const uint32_t CPU_FREQ_OPTIONS[] = {240, 160, 80};
#define CPU_FREQ_OPTIONS_COUNT (sizeof(CPU_FREQ_OPTIONS) / sizeof(CPU_FREQ_OPTIONS[0]))

// NVS-namespace/ключи для мощности передатчика и частоты процессора - тот же
// namespace ("smartgarden"), что и у DeviceManager (см. DeviceManager.cpp,
// NVS_NAMESPACE) - конфликта нет, т.к. ключи разные ("installed" против
// "txpower"/"cpufreq"), а отдельный namespace ради двух дополнительных целых
// чисел не оправдан. См. saveTxPowerToNVS()/saveCpuFreqToNVS()/
// loadRadioSettingsFromNVS() ниже.
const char *RADIO_NVS_NAMESPACE = "smartgarden";
const char *NVS_KEY_TX_POWER = "txpower";
const char *NVS_KEY_CPU_FREQ = "cpufreq";

// Имя хоста для mDNS (протокол "Bonjour"/"zeroconf", в Linux обычно
// реализован через avahi) - см. большой комментарий про домен в начале
// файла.
const char *HUB_HOSTNAME = "smartgarden";

uint8_t myMac[6];

UniversalPacket txPacket;

uint16_t lastPacketId = 0; // счётчик ИСХОДЯЩИХ пакетов Хаба

WebServer server(80);

DeviceManager deviceManager;
Ds3231Rtc rtc;
ApButton apButton;

// true - хотя бы раз получили время (от браузера или от DS3231 при
// старте). До этого показания time()/currentTimeString() бессмысленны.
bool timeSynced = false;

// true, если esp_now_init()+esp_now_add_peer() прошли успешно в setup().
// Без этого Хаб физически не может ни принять, ни отправить ни одного
// пакета периферии - молча продолжать работу в таком состоянии было бы
// хуже, чем явно просигналить об этом (см. updateStatusLed() ниже и
// setup()).
bool radioReady = false;

// Очередь входящих пакетов ESP-NOW - см. пояснение про однопоточность
// бизнес-логики в начале файла. С запасом над практической нагрузкой:
// телеметрия у 30-50 узлов приходит раз в ~10 сек с джиттером, то есть
// в среднем меньше одного пакета в секунду - переполнение возможно
// только в случае, если loop() надолго завис (само по себе тревожный
// признак, не относящийся к размеру очереди).
#define INCOMING_QUEUE_LENGTH 16
QueueHandle_t incomingPacketQueue = nullptr;

// --- Кнопка/точка доступа ---
//
// Физическая кнопка на плате включает/выключает точку доступа
// веб-интерфейса (антидребезг - см. ApButton.h). СОСТОЯНИЕ НЕ ПЕРЕЖИВАЕТ
// перезагрузку Хаба - НИКАК НЕ сохраняется в NVS намеренно: каждый сброс Хаба
// всегда стартует с выключенной точкой доступа (так надёжнее по умолчанию, чем
// восстанавливать прежнее состояние - открытая вечно точка доступа после любого
// сбоя/перезагрузки - лишняя поверхность атаки в поле). Оператор включает её явно
// каждый раз, когда она нужна.
//
// АВТО-ОТКЛЮЧЕНИЕ: если точка доступа включена, но к ней больше AP_AUTO_OFF_MS подряд
// ни одного клиента не подключено (см. updateApAutoOff() ниже, WiFi.softAPgetStationNum()) - она
// выключается сама - чтобы не держать радиоэфир открытым бессрочно, если оператор
// забыл её выключить вручную. Отсчёт начинается заново при каждом отключении последнего
// клиента (или с момента включения AP, если никто так и не подключился) - любое новое
// подключение его сбрасывает.
//
// Пока AP выключена, устройство всё равно продолжает принимать и
// обрабатывать ESP-NOW трафик от узлов - выключена именно точка
// доступа для веб-интерфейса, а не радио целиком.
//
// AP_BUTTON_PIN=0 - стандартная кнопка "BOOT", присутствующая на
// подавляющем большинстве отладочных плат ESP32 с модулем WROOM (сам
// модуль WROOM кнопок не имеет - они на плате-носителе).
// AP_LED_PIN=2 - встроенный светодиод, присутствующий на большинстве
// таких плат. Тот же светодиод по совместительству используется и для
// сигнализации об отказе радио (см. updateStatusLed()) - см. пояснение
// там же, почему это не конфликтует.
// Если распиновка конкретной платы отличается - поменяйте оба define.
#define AP_BUTTON_PIN 0
#define AP_LED_PIN 2
const unsigned long AP_BUTTON_DEBOUNCE_MS = 50;

// 5 минут без ни одного подключённого к точке доступа клиента - автоматически
// выключаем (см. updateApAutoOff() ниже).
#define AP_AUTO_OFF_MS (5UL * 60UL * 1000UL)

bool apEnabled = false; // выключена при старте - и НАВСЕГДА (нет NVS-восстановления прежнего
                        // состояния, см. комментарий выше)

// 0 - таймер не запущен (есть хотя бы одно подключение, или AP и так выключена);
// иначе - millis() момента, с которого ни одного клиента не было - см. updateApAutoOff().
unsigned long apNoClientsSinceMs = 0;

// --- Часы ---

// Форматирует текущее время Хаба в читаемый вид - для Serial и
// веб-интерфейса. Хранимое значение - МЕСТНОЕ время (его уже таким присылает
// браузер, сдвинув свой часовой пояс перед отправкой, см. WebPage.h; или
// таким его хранит DS3231, см. Ds3231Rtc.h), поэтому тут оно просто
// читается `gmtime_r()`-ом как есть, без какого-либо повторного сдвига на
// стороне Хаба - использование именно `gmtime_r()`, а не `localtime_r()`,
// здесь не означает "показываем UTC" - просто не тащим на ESP32 встроенную
// систему перевода часовых поясов/DST, которая тут не нужна - пояс уже
// учтён до записи (см. PROTOCOL.md §12).
String currentTimeString() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    char buf[20];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
              timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
              timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buf);
}

// Заголовок исходящего пакета. receiverMac - конкретный узел (для команды),
// не HUB_MAC (тот зарезервирован под адресацию Хаба). Сама механика
// заполнения полей — общая для всех отправителей (см. fillPacketHeader()
// в GardenProtocol.h); у Хаба своя обёртка остаётся только потому, что ему
// (в отличие от узла, всегда пишущего одному и тому же адресату — Хабу)
// нужен receiverMac как параметр, а не константа.
void prepareHeader(MsgType mType, const uint8_t *receiverMac) {
    fillPacketHeader(txPacket, myMac, receiverMac, TYPE_HUB, mType, lastPacketId);
}

// Объявление о (пере)загрузке Хаба - broadcast MSG_CONFIG на "адрес всем"
// (FF..FF). Узлы, увидев MSG_CONFIG от TYPE_HUB, обязаны сбросить свой
// dedup-счётчик для Хаба - иначе после ребута Хаба его packet_id снова
// стартует с малых значений и все команды будут молча отклоняться как
// "старые" (см. PROTOCOL.md §4.3). Шлём несколько раз с паузой на случай
// потери одного пакета в эфире/через репку - это не бесконечный повтор,
// а короткая пачка сразу при старте.
//
// Тем же пакетом Хаб разносит по сети своё текущее время (поле
// payload.hub.epoch, 0 если сам ещё не синхронизирован) - узлы подхватывают
// его в GardenNode::handleIncoming() и выставляют свои часы (см. PROTOCOL.md §12).
// Вызывается не только при загрузке Хаба, но и повторно из
// handleApiSetTime() при каждой успешной синхронизации от браузера - чтобы
// уже работающие узлы получали время сразу, не дожидаясь своего
// собственного очередного MSG_CONFIG (база ~1 час) или перезагрузки Хаба.
void sendHubAnnounce() {
    if (!radioReady) return; // радио не инициализировано - отправлять нечем (см. setup())
    prepareHeader(MSG_CONFIG, BROADCAST_MAC);
    txPacket.payload.hub.epoch = timeSynced ? (uint32_t) time(nullptr) : 0;
    esp_now_send(BROADCAST_MAC, (uint8_t *) &txPacket, sizeof(txPacket));
    Serial.printf("Sent: HUB ANNOUNCE, packet_id=%u, epoch=%lu\n",
                  txPacket.packet_id, (unsigned long) txPacket.payload.hub.epoch);
}

void sendCommand(int deviceIdx, uint8_t targetValve, uint8_t action, uint8_t mode, uint16_t durationSec, uint16_t volumeL) {
    if (!radioReady) {
        Serial.println("ESP-NOW не инициализирован - команда не отправлена.");
        return;
    }
    if (!deviceManager.isValid(deviceIdx)) {
        Serial.println("Нет такого устройства (см. 'list')");
        return;
    }
    prepareHeader(MSG_COMMAND, deviceManager.devices[deviceIdx]->mac);
    txPacket.payload.irrigation.command.target_valve = targetValve;
    txPacket.payload.irrigation.command.action = action;
    txPacket.payload.irrigation.command.mode = mode;
    txPacket.payload.irrigation.command.duration_sec = durationSec;
    txPacket.payload.irrigation.command.volume_l = volumeL;

    esp_err_t result = esp_now_send(BROADCAST_MAC, (uint8_t *) &txPacket, sizeof(txPacket));
    if (result != ESP_OK) {
        Serial.printf("esp_now_send failed: %d\n", (int) result);
    }

    Serial.print("Sent COMMAND to #"); Serial.print(deviceIdx);
    Serial.printf(" valve=%u action=%u mode=%u duration_sec=%u volume_l=%u (packet_id=%u)\n",
                  targetValve, action, mode, durationSec, volumeL, txPacket.packet_id);
}

// Отправить MSG_SET_CONFIG конкретному устройству - желаемые valve_count/mode/has_flow_sensor/
// flow_pulses_per_liter, которые узел должен ПРИМЕНИТЬ И СОХРАНИТЬ у себя (EEPROM), в отличие от
// sendCommand() выше (разовое действие, не переживает перезагрузку узла).
// Симметрично sendCommand() - тот же принцип адресации/сборки заголовка,
// другой msg_type и payload. См. GardenProtocol.h (MSG_SET_CONFIG/
// IrrigationConfigSet) и onSetConfig() на стороне узла (flow_node.ino).
//
// Диапазоны здесь (valveCount 1..5, hasFlowSensor 0/1, pulsesPerLiter 1..20000) - только
// базовая защита от заведомо бессмысленного ввода (веб-форма и так ограничивает его
// теми же диапазонами, см. WebPage.h) - РЕАЛЬНУЮ валидацию под конкретную плату (её
// фактическую распайку MAX_VALVES, и какой датчик на ней реально стоит) делает сам узел
// и может отклонить значение через MSG_ACK{status=1}, даже если оно прошло эту проверку
// здесь - Хаб не знает физическую емкость/комплектацию конкретного узла.
void sendSetConfig(int deviceIdx, uint8_t valveCount, uint8_t mode, uint8_t hasFlowSensor, uint16_t pulsesPerLiter) {
    if (!radioReady) {
        Serial.println("ESP-NOW не инициализирован - конфигурация не отправлена.");
        return;
    }
    if (!deviceManager.isValid(deviceIdx)) {
        Serial.println("Нет такого устройства (см. 'list')");
        return;
    }
    if (valveCount < 1 || valveCount > 5 || (mode != 1 && mode != 2) ||
        (hasFlowSensor != 0 && hasFlowSensor != 1) ||
        pulsesPerLiter < 1 || pulsesPerLiter > 20000) {
        Serial.println("Некорректная конфигурация: valve_count должен быть 1..5, mode - 1 или 2, "
                        "has_flow_sensor - 0 или 1, pulses_per_liter - 1..20000.");
        return;
    }
    prepareHeader(MSG_SET_CONFIG, deviceManager.devices[deviceIdx]->mac);
    txPacket.payload.irrigation.configSet.valve_count = valveCount;
    txPacket.payload.irrigation.configSet.mode = mode;
    txPacket.payload.irrigation.configSet.has_flow_sensor = hasFlowSensor;
    txPacket.payload.irrigation.configSet.flow_pulses_per_liter = pulsesPerLiter;

    esp_err_t result = esp_now_send(BROADCAST_MAC, (uint8_t *) &txPacket, sizeof(txPacket));
    if (result != ESP_OK) {
        Serial.printf("esp_now_send failed: %d\n", (int) result);
    }

    Serial.print("Sent SET_CONFIG to #"); Serial.print(deviceIdx);
    Serial.printf(" valve_count=%u mode=%u has_flow_sensor=%u pulses_per_liter=%u (packet_id=%u)\n",
                  valveCount, mode, hasFlowSensor, pulsesPerLiter, txPacket.packet_id);
}

// Вся содержательная обработка входящего пакета - раньше жила прямо в
// колбэке onDataRecv(), теперь выполняется ИСКЛЮЧИТЕЛЬНО из loop() (см.
// цикл разбора incomingPacketQueue там же). Благодаря этому обращения к
// deviceManager.devices[] и txPacket (через sendHubAnnounce()) происходят
// из одной и той же задачи (Arduino loop task), что и Serial-команды, и
// обработчики веб-API - никакой другой код их не трогает. Гонка, из-за
// которой могли одновременно сработать, например, install через веб и
// вытеснение того же слота приходящим пакетом, устранена не блокировками,
// а тем, что делить нечего - один владелец состояния.
void processIncomingPacket(const UniversalPacket &rxPacket) {
    // Хаб принимает только то, что адресовано ему (HUB_MAC)
    if (memcmp(rxPacket.receiver_mac, HUB_MAC, 6) != 0) return;

    int idx = deviceManager.findOrRegister(rxPacket.sender_mac, rxPacket.device_type);
    if (idx < 0) return;

    // MSG_CONFIG = устройство только что (пере)загрузилось -> сбрасываем
    // dedup для него, иначе после ребута узла его пакеты будут выглядеть
    // "старыми" относительно уже виденных ранее номеров (см. GardenProtocol.h).
    if (rxPacket.msg_type == MSG_CONFIG) {
        deviceManager.devices[idx]->lastSeenPacketId = 0;

        // Узел только что (пере)загрузился - его часы сброшены на 1970-01-01
        // (у него нет RTC, см. PROTOCOL.md §12.6). Не ждём его собственного очередного
        // MSG_CONFIG (база ~1 час) или своей очередной пересинхронизации страницы с
        // браузером (до 30 минут) - сразу же отвечаем текущим временем тем же
        // broadcast-announce, что и так рассылается при (пере)загрузке/синхронизации
        // самого Хаба (см. sendHubAnnounce()) - отдельного механизма под это не
        // потребовалось. Если сам Хаб ещё не синхронизирован, epoch в announce
        // будет 0 - узел это корректно обработает (часы не трогает, см. §12.6).
        sendHubAnnounce();
    }

    if (!isNewerPacketId(rxPacket.packet_id, deviceManager.devices[idx]->lastSeenPacketId)) {
        Serial.print("Игнорирован дубликат/старый пакет от #"); Serial.print(idx);
        Serial.printf(" packet_id=%u (last=%u)\n", rxPacket.packet_id, deviceManager.devices[idx]->lastSeenPacketId);
        return;
    }
    deviceManager.devices[idx]->lastSeenPacketId = rxPacket.packet_id;
    deviceManager.devices[idx]->lastSeenTime = millis();

    // ACK - формат общий для всех типов устройств, не зависит от device_type.
    if (rxPacket.msg_type == MSG_ACK) {
        AckData ack = rxPacket.payload.ack;
        Serial.printf("ACK от #%d: acked_packet_id=%u status=%u\n",
                      idx, ack.acked_packet_id, ack.status);
        return;
    }

    // CONFIG/TELEMETRY - формат payload зависит от device_type, поэтому
    // диспетчеризация ПО ТИПУ УСТРОЙСТВА теперь полиморфная (см.
    // Device::handlePayload() в Device.h/IrrigationDevice.h) - вместо switch
    // по device_type с отдельной handleXxxPayload() на каждый тип (как было
    // раньше) каждое устройство само знает, как разобрать свой payload -
    // добавление нового типа устройства больше не требует правки этой функции.
    // Устройства неподдержанного типа (ещё нет своего наследника Device) в
    // таблице всё равно числятся как GenericDevice (см. DeviceManager::registerAt()),
    // чей handlePayload() просто логирует игнорирование - то же самое
    // поведение, что и у прежнего default-случая здесь.
    deviceManager.devices[idx]->handlePayload(idx, rxPacket);
}

// Колбэк приёма - сигнатура актуальная для arduino-esp32 3.x. Вызывается
// ядром ИЗ ЗАДАЧИ СТЕКА WI-FI, а не из loop() - здесь намеренно делается
// МИНИМУМ: проверка размера, копирование пакета и постановка в очередь.
// Вся содержательная обработка (включая работу с deviceManager) - в
// processIncomingPacket(), вызываемой из loop() - см. пояснение про
// однопоточность бизнес-логики в начале файла.
//
// МAC отправителя в протоколе передаётся ВНУТРИ пакета (sender_mac),
// поэтому info->src_addr (MAC на радиоуровне) здесь не используется -
// это совпадает с тем, как уже был спроектирован протокол для
// прохождения через ретрансляторы.
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
    if (len != (int) sizeof(UniversalPacket)) return;

    UniversalPacket pkt;
    memcpy(&pkt, incomingData, sizeof(UniversalPacket));

    // timeout=0 - не блокируем задачу стека Wi-Fi, если очередь вдруг
    // переполнена: лучше молча уронить пакет (он придёт снова со
    // следующей телеметрией/announce), чем задержать колбэк радиостека.
    xQueueSend(incomingPacketQueue, &pkt, 0);
}

// --- Кнопка/точка доступа/статус радио ---

// (Пере)запускает mDNS-отклик - вызывается вместе с включением точки
// доступа (см. setApEnabled()), поскольку резолвить имя есть смысл
// только пока есть что резолвить (сама точка доступа поднята). При
// выключении точки доступа mDNS останавливается (MDNS.end()) - это
// безопасно вызвать, даже если сервис не был запущен.
void restartMDNS() {
    MDNS.end();
    if (MDNS.begin(HUB_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.print("mDNS: веб-интерфейс также доступен как http://");
        Serial.print(HUB_HOSTNAME); Serial.println(".local (если клиент поддерживает mDNS)");
    } else {
        Serial.println("mDNS: не удалось запустить - веб-интерфейс всё равно доступен по IP.");
    }
}

// Включает или выключает точку доступа веб-интерфейса - вызывается из loop() по
// нажатию кнопки или автоматически из updateApAutoOff() ниже (отсутствие клиентов
// 5 минут). СОСТОЯНИЕ НИКУДА НЕ СОХРАНЯЕТСЯ (в отличие от более ранней
// версии - см. большой комментарий у apEnabled выше), поэтому у функции больше
// нет параметра persist.
void setApEnabled(bool enabled) {
    apEnabled = enabled;
    apNoClientsSinceMs = 0; // любое изменение состояния сбрасывает таймер авто-отключения
    if (apEnabled) {
        WiFi.softAP(AP_SSID, AP_PASSWORD, ESPNOW_CHANNEL);
        Serial.print("Точка доступа ВКЛЮЧЕНА: "); Serial.println(AP_SSID);
        Serial.print("Веб-интерфейс: http://"); Serial.println(WiFi.softAPIP());
        restartMDNS();
    } else {
        WiFi.softAPdisconnect(true);
        MDNS.end();
        Serial.println("Точка доступа ВЫКЛЮЧЕНА.");
    }
    if (radioReady) {
        // Пока радио не готово, светодиодом безраздельно управляет
        // updateStatusLed() (мигание отказа) - см. там же.
        //digitalWrite(AP_LED_PIN, apEnabled ? HIGH : LOW);
    }
}

// Вызывается каждую итерацию loop() - см. большой комментарий про АВТО-ОТКЛЮЧЕНИЕ
// у apEnabled выше. WiFi.softAPgetStationNum() - число СЕЙЧАС подключённых к
// точке доступа Wi-Fi-станций (телефонов/ноутбуков с открытой веб-страницей или без
// неё) - не требует отдельного учёта со стороны Самого Хаба.
void updateApAutoOff() {
    if (!apEnabled) {
        apNoClientsSinceMs = 0; // не считаем, пока AP и так выключена
        return;
    }
    if (WiFi.softAPgetStationNum() > 0) {
        apNoClientsSinceMs = 0; // есть хотя бы одно подключение - сбрасываем отсчёт
        return;
    }
    if (apNoClientsSinceMs == 0) {
        apNoClientsSinceMs = millis(); // подключений нет - начинаем отсчёт
        return;
    }
    if (millis() - apNoClientsSinceMs >= AP_AUTO_OFF_MS) {
        Serial.println("Точка доступа: нет активных подключений 5 минут подряд - выключаю автоматически.");
        setApEnabled(false);
    }
}

const unsigned long RADIO_FAIL_BLINK_MS = 250; // период мигания светодиода при неисправном радио
unsigned long lastRadioFailBlinkMs = 0;
bool radioFailLedState = false;

// Обновляет светодиод платы. В штатном режиме (radioReady==true) ничего
// не делает - состояние светодиода целиком определяется setApEnabled() и
// меняется только по событию (нажатие кнопки), а не каждый тик. Если же
// инициализация ESP-NOW провалилась в setup() - это критичнее статуса
// точки доступа (без ESP-NOW Хаб не примет и не отправит ни одного
// пакета периферии), поэтому светодиод здесь ПЕРЕОПРЕДЕЛЯЕТ обычную
// индикацию AP и быстро мигает, пока проблема не устранена (обычно
// означает: перепрошить или проверить питание/антенну). Вызывать в
// каждой итерации loop().
void updateStatusLed() {
    if (radioReady) return;

    if (millis() - lastRadioFailBlinkMs > RADIO_FAIL_BLINK_MS) {
        lastRadioFailBlinkMs = millis();
        radioFailLedState = !radioFailLedState;
       // digitalWrite(AP_LED_PIN, radioFailLedState ? HIGH : LOW);
    }
}

// --- Веб-интерфейс ---
// HTML страницы (константа INDEX_HTML) вынесена в отдельный файл
// WebPage.h рядом - Arduino IDE покажет его отдельной вкладкой в том же
// окне редактора, не засоряя логику в hub.ino огромным raw-string-литералом.

void handleRoot() {
    server.send(200, "text/html", INDEX_HTML);
}

// Экранирует строку для безопасной вставки в JSON-значение (кавычки,
// обратный слэш, управляющие символы). Остальные строковые поля в JSON-ответах
// Хаба (MAC, время) безопасны по построению (формируются самим Хабом из
// hex-цифр/чисел), экранирование понадобилось только с появлением произвольного
// пользовательского текста - названия устройства (см. handleApiRename()).
String jsonEscape(const String &input) {
    String out;
    out.reserve(input.length() + 4);
    for (size_t i = 0; i < input.length(); i++) {
        char c = input[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char) c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// GET /api/devices - список известных устройств в JSON. Собирается
// вручную (без ArduinoJson) - таблица маленькая, лишняя зависимость не
// нужна.
void handleApiDevices() {
    String json = "[";
    bool first = true;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (deviceManager.devices[i] == nullptr) continue;
        if (!first) json += ",";
        first = false;

        Device *d = deviceManager.devices[i];
        const uint8_t *mac = d->mac;
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        unsigned long agoSec = (millis() - d->lastSeenTime) / 1000;

        json += "{\"idx\":" + String(i) +
                ",\"mac\":\"" + String(macStr) + "\"" +
                ",\"type\":" + String(d->deviceType) +
                ",\"name\":\"" + jsonEscape(String(d->name)) + "\"" +
                ",\"agoSec\":" + String(agoSec) +
                ",\"installed\":" + String(d->installed ? "true" : "false");
        // Поля, специфичные для типа устройства (например, "activeValve" у
        // полива) - раньше были зашиты здесь напрямую, теперь каждое
        // устройство дописывает их само (см. Device::appendJsonFields()) -
        // добавление нового типа устройства больше не требует правки этой функции.
        d->appendJsonFields(json);
        json += "}";
    }
    json += "]";
    server.send(200, "application/json", json);
}

// POST /api/command - form-urlencoded: idx, valve, action, mode, duration, volume.
// action - ValveAction (0=ACTION_OPEN, 1=ACTION_CLOSE, см. GardenProtocol.h) - по умолчанию
// ACTION_OPEN для обратной совместимости с вызовами без этого поля (веб-страница
// всегда отправляет его явно, см. WebPage.h). Ровно то же самое, что делают Serial-команды
// open/volume/close - просто другой вход в ту же sendCommand(). Работает и для
// не-установленных кандидатов - можно проверить/опознать устройство физически
// (например, подёргать клапан) ДО того, как решить его устанавливать.
void handleApiCommand() {
    if (!server.hasArg("idx")) {
        server.send(400, "text/plain", "missing idx");
        return;
    }
    int idx = server.arg("idx").toInt();
    int valve = server.hasArg("valve") ? server.arg("valve").toInt() : 0;
    int action = server.hasArg("action") ? server.arg("action").toInt() : ACTION_OPEN;
    int mode = server.hasArg("mode") ? server.arg("mode").toInt() : 0;
    int duration = server.hasArg("duration") ? server.arg("duration").toInt() : 0;
    int volume = server.hasArg("volume") ? server.arg("volume").toInt() : 0;

    sendCommand(idx, (uint8_t) valve, (uint8_t) action, (uint8_t) mode, (uint16_t) duration, (uint16_t) volume);
    server.send(200, "text/plain", "ok");
}

// POST /api/install - form-urlencoded: idx.
// Помечает устройство как установленное: защищает от LRU-вытеснения и
// сохраняет весь список установленных устройств в NVS.
void handleApiInstall() {
    if (!server.hasArg("idx")) {
        server.send(400, "text/plain", "missing idx");
        return;
    }
    int idx = server.arg("idx").toInt();
    if (!deviceManager.install(idx)) {
        server.send(404, "text/plain", "device not found");
        return;
    }
    server.send(200, "text/plain", "ok");
}

// POST /api/forget - form-urlencoded: idx.
// Единственный способ убрать УСТАНОВЛЕННОЕ устройство (для кандидатов
// это тоже работает - просто освобождает слот раньше срабатывания LRU).
void handleApiForget() {
    if (!server.hasArg("idx")) {
        server.send(400, "text/plain", "missing idx");
        return;
    }
    int idx = server.arg("idx").toInt();
    if (!deviceManager.forget(idx)) {
        server.send(404, "text/plain", "device not found");
        return;
    }
    server.send(200, "text/plain", "ok");
}

// POST /api/setConfig - form-urlencoded: idx, valveCount, mode, hasFlowSensor, pulsesPerLiter.
// Конфигурация (в отличие от /api/command) хранится НА САМОМ узле (EEPROM) и
// переживает его перезагрузку - см. sendSetConfig() и GardenProtocol.h
// (MSG_SET_CONFIG). Работает и для не-установленных кандидатов - как и
// /api/command (см. его комментарий), можно настроить устройство ДО того, как
// решить его устанавливать.
void handleApiSetConfig() {
    if (!server.hasArg("idx") || !server.hasArg("valveCount") || !server.hasArg("mode") ||
        !server.hasArg("hasFlowSensor") || !server.hasArg("pulsesPerLiter")) {
        server.send(400, "text/plain", "missing idx, valveCount, mode, hasFlowSensor or pulsesPerLiter");
        return;
    }
    int idx = server.arg("idx").toInt();
    int valveCount = server.arg("valveCount").toInt();
    int mode = server.arg("mode").toInt();
    int hasFlowSensor = server.arg("hasFlowSensor").toInt();
    int pulsesPerLiter = server.arg("pulsesPerLiter").toInt();
    if (valveCount < 1 || valveCount > 5 || (mode != 1 && mode != 2) ||
        (hasFlowSensor != 0 && hasFlowSensor != 1) ||
        pulsesPerLiter < 1 || pulsesPerLiter > 20000) {
        server.send(400, "text/plain", "invalid valveCount, mode, hasFlowSensor or pulsesPerLiter");
        return;
    }
    sendSetConfig(idx, (uint8_t) valveCount, (uint8_t) mode, (uint8_t) hasFlowSensor, (uint16_t) pulsesPerLiter);
    server.send(200, "text/plain", "ok");
}

// POST /api/rename - form-urlencoded: idx, name.
// Переименовать можно ТОЛЬКО УСТАНОВЛЕННОЕ устройство (см.
// DeviceManager::setName()) - у кандидата название всё равно не переживёт его
// возможное LRU-вытеснение. server.arg("name") уже URL-декодирован WebServer'ом -
// отправитель (WebPage.h) кодирует его через encodeURIComponent() перед отправкой.
void handleApiRename() {
    if (!server.hasArg("idx") || !server.hasArg("name")) {
        server.send(400, "text/plain", "missing idx or name");
        return;
    }
    int idx = server.arg("idx").toInt();
    String name = server.arg("name");
    if (!deviceManager.setName(idx, name.c_str())) {
        server.send(404, "text/plain", "device not found or not installed");
        return;
    }
    server.send(200, "text/plain", "ok");
}

// POST /api/settime - form-urlencoded: epoch (целое число секунд, МЕСТНОЕ время браузера,
// уже со сдвигом на часовой пояс, не UTC - см. localEpochSeconds() в WebPage.h).
// Вызывается автоматически самим веб-интерфейсом при каждой
// загрузке страницы в браузере (см. WebPage.h, syncTime()).
void handleApiSetTime() {
    if (!server.hasArg("epoch")) {
        server.send(400, "text/plain", "missing epoch");
        return;
    }
    time_t epoch = (time_t) server.arg("epoch").toInt();
    struct timeval tv;
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    timeSynced = true;
    Serial.print("Часы синхронизированы с браузером: "); Serial.println(currentTimeString());

    // Держим DS3231 (если она есть) в актуальном состоянии - именно так
    // она "синхронизирует своё время с браузером" (а не только Хаб): в
    // следующий раз, если Хаб перезагрузится ДО открытия веб-страницы,
    // он подхватит уже это, а не устаревшее время (см. Ds3231Rtc.h).
    if (rtc.isPresent()) {
        rtc.setEpoch((uint32_t) epoch);
    }

    // Сразу же разносим новое время по ESP-NOW всем периферийным узлам -
    // чтобы уже работающие узлы получили время сразу, не дожидаясь своего
    // собственного очередного MSG_CONFIG (база ~1 час) или перезагрузки Хаба.
    // Срабатывает это же при каждой повторной синхронизации страницы раз в 30 минут
    // (см. WebPage.h) - попутно тоже компенсирует дрейф часов узлов, не только Хаба.
    sendHubAnnounce();

    server.send(200, "text/plain", "ok");
}

// GET /api/status - служебная информация Хаба: часы, точка доступа,
// время работы с момента сброса (uptimeSec), температура кристалла, текущая
// мощность передатчика и текущая частота процессора - веб-страница опрашивает это
// в том же цикле автообновления, что и /api/devices, чтобы показать оператору визуальное
// подтверждение, что синхронизация времени сработала, как долго Хаб уже
// работает без перезагрузки, и текущее состояние радио/процессора.
void handleApiStatus() {
    String json = "{";
    json += "\"timeSynced\":" + String(timeSynced ? "true" : "false");
    json += ",\"epoch\":" + String((unsigned long) time(nullptr));
    json += ",\"timeString\":\"" + currentTimeString() + "\"";
    json += ",\"rtcPresent\":" + String(rtc.isPresent() ? "true" : "false");
    json += ",\"apEnabled\":" + String(apEnabled ? "true" : "false");
    json += ",\"radioReady\":" + String(radioReady ? "true" : "false");
    // Время работы с момента последнего сброса (millis()/1000) - В ОТЛИЧИЕ от часов
    // (time()/timeSynced выше), это независимо от того, была ли вообще синхронизация с браузером/DS3231 -
    // всегда доступно с самого включения питания. millis() у ESP32 - uint32_t в миллисекундах,
    // переполняется через ~49.7 дня непрерывной работы - для отображаемого здесь счётчика
    // дней/часов работы хаба это в практике несущественно (перезагрузка раньше всё
    // равно сбросила бы этот счётчик).
    json += ",\"uptimeSec\":" + String((unsigned long) (millis() / 1000));
    // Температура кристалла ESP32 (встроенный датчик, не окружающей среды!) через
    // temperatureRead() из ядра arduino-esp32, в °C - точность этого встроенного датчика
    // умеренная (калибровка заводская, погрешность порядка нескольких градусов возможна)
    // - для грубого контроля перегрева/адекватности теплового режима этого вполне
    // достаточно, для прецизионных измерений - нет. String(float) округляет до 2 знаков после запятой
    // по умолчанию - достаточно для отображения.
    json += ",\"chipTempC\":" + String(temperatureRead(), 1);
    // Текущая мощность передатчика - сырое число в четвертях дБм (см. TxPowerOption выше,
    // wifi_power_t неявно конвертируется в int) - веб-страница сама делит на 4 для отображения.
    json += ",\"txPowerQuarterDbm\":" + String((int) WiFi.getTxPower());
    // Текущая частота процессора - см. CPU_FREQ_OPTIONS выше.
    json += ",\"cpuFreqMhz\":" + String(getCpuFrequencyMhz());
    json += "}";
    server.send(200, "application/json", json);
}

// Сохраняет текущую мощность передатчика в NVS - вызывается из handleApiSetTxPower()
// после успешного применения значения - чтобы оно пережило перезагрузку Хаба
// (см. loadRadioSettingsFromNVS() ниже, вызывается из setup()). Отдельная функция,
// а не putInt() прямо внутри handleApiSetTxPower() - чтобы та же логика была доступна
// и для возможных будущих вызовов (например, из Serial-команд, если они когда-нибудь
// появятся для этих настроек).
void saveTxPowerToNVS(wifi_power_t value) {
    Preferences prefs;
    prefs.begin(RADIO_NVS_NAMESPACE, false);
    prefs.putInt(NVS_KEY_TX_POWER, (int) value);
    prefs.end();
}

// Симметрично saveTxPowerToNVS() выше - для частоты процессора.
void saveCpuFreqToNVS(uint32_t value) {
    Preferences prefs;
    prefs.begin(RADIO_NVS_NAMESPACE, false);
    prefs.putUInt(NVS_KEY_CPU_FREQ, value);
    prefs.end();
}

// Восстанавливает мощность передатчика и частоту процессора из NVS - вызывается один
// раз из setup(), после WiFi.mode(WIFI_AP_STA) (без этого WiFi.setTxPower() не
// имеет смысла - радио ещё не инициализировано), чтобы Хаб сразу работал
// с настройками, которые оператор выбрал в прошлый раз, а не с дефолтами ядра.
// Каждое восстановленное значение сверяется С НАБОРОМ допустимых значений
// (TX_POWER_OPTIONS/CPU_FREQ_OPTIONS) - той же проверкой, что и в
// handleApiSetTxPower()/handleApiSetCpuFreq() - если в NVS вдруг оказалось значение
// вне этого набора (повреждение, старая прошивка с другим набором опций),
// оно молча игнорируется, и остаётся дефолт ядра, а НЕ падаем и НЕ применяем
// недоверенное сырое число.
//
// Частота процессора ниже 240 МГц может временно замедлить радио/веб-интерфейс
// на первых итерациях после сброса, но это ожидаемое поведение самой пониженной
// частоты (см. комментарий у CPU_FREQ_OPTIONS выше) - оператор, сохранивший такое
// значение через веб-интерфейс, уже явно согласился на этот компромисс.
void loadRadioSettingsFromNVS() {
    Preferences prefs;
    prefs.begin(RADIO_NVS_NAMESPACE, true); // read-only
    bool hasTxPower = prefs.isKey(NVS_KEY_TX_POWER);
    int savedTxPower = hasTxPower ? prefs.getInt(NVS_KEY_TX_POWER) : 0;
    bool hasCpuFreq = prefs.isKey(NVS_KEY_CPU_FREQ);
    uint32_t savedCpuFreq = hasCpuFreq ? prefs.getUInt(NVS_KEY_CPU_FREQ) : 0;
    prefs.end();

    if (hasTxPower) {
        bool applied = false;
        for (size_t i = 0; i < TX_POWER_OPTIONS_COUNT; i++) {
            if ((int) TX_POWER_OPTIONS[i].value == savedTxPower) {
                WiFi.setTxPower(TX_POWER_OPTIONS[i].value);
                Serial.printf("Мощность передатчика восстановлена из NVS: %s\n", TX_POWER_OPTIONS[i].label);
                applied = true;
                break;
            }
        }
        if (!applied) {
            Serial.println("Сохранённая в NVS мощность передатчика не входит в допустимый набор - игнорирую, оставляю дефолт.");
        }
    }

    if (hasCpuFreq) {
        bool applied = false;
        for (size_t i = 0; i < CPU_FREQ_OPTIONS_COUNT; i++) {
            if (CPU_FREQ_OPTIONS[i] == savedCpuFreq) {
                bool ok = setCpuFrequencyMhz(CPU_FREQ_OPTIONS[i]);
                if (ok) {
                    Serial.printf("Частота процессора восстановлена из NVS: %u МГц\n", (unsigned) CPU_FREQ_OPTIONS[i]);
                } else {
                    Serial.printf("Не удалось восстановить частоту процессора %u МГц из NVS (отклонено ядром) - оставляю дефолт.\n",
                                  (unsigned) CPU_FREQ_OPTIONS[i]);
                }
                applied = true;
                break;
            }
        }
        if (!applied) {
            Serial.println("Сохранённая в NVS частота процессора не входит в допустимый набор - игнорирую, оставляю дефолт.");
        }
    }
}

// POST /api/setTxPower - form-urlencoded: value (целое число в четвертях дБм, то есть сырое значение
// wifi_power_t - см. TX_POWER_OPTIONS выше). Обязательно сверяется С НАБОРОМ допустимых
// значений, а НЕ с диапазоном чисел - см. комментарий у TX_POWER_OPTIONS выше, почему это
// важно. Влияет СРАЗУ (без перезагрузки радио) и СОХРАНЯЕТСЯ в NVS (см.
// saveTxPowerToNVS() выше), поэтому переживёт следующий сброс Хаба (см.
// loadRadioSettingsFromNVS(), вызывается из setup()).
void handleApiSetTxPower() {
    if (!server.hasArg("value")) {
        server.send(400, "text/plain", "missing value");
        return;
    }
    int requested = server.arg("value").toInt();
    for (size_t i = 0; i < TX_POWER_OPTIONS_COUNT; i++) {
        if ((int) TX_POWER_OPTIONS[i].value == requested) {
            WiFi.setTxPower(TX_POWER_OPTIONS[i].value);
            saveTxPowerToNVS(TX_POWER_OPTIONS[i].value);
            Serial.printf("Мощность передатчика установлена: %s\n", TX_POWER_OPTIONS[i].label);
            server.send(200, "text/plain", "ok");
            return;
        }
    }
    server.send(400, "text/plain", "invalid value - not in the known wifi_power_t set");
}

// POST /api/setCpuFreq - form-urlencoded: value (целое число в МГц, см. CPU_FREQ_OPTIONS выше).
// Сверяется С НАБОРОМ допустимых значений - та же логика, что и у handleApiSetTxPower() выше.
// setCpuFrequencyMhz() возвращает bool - может отказаться (неверный делитель для текущего
// источника тактовой частоты), проверяем его и отвечаем честно, а не всегда "ok". При
// успехе также СОХРАНЯЕТ выбор в NVS (см. saveCpuFreqToNVS() выше), по той же причине,
// что и handleApiSetTxPower() - чтобы выбор пережил следующий сброс Хаба. На отказе ядра
// (ok == false) в NVS НИЧЕГО не пишется - сохранять имеет смысл только то, что реально
// применилось к живому радио, иначе при следующем старте loadRadioSettingsFromNVS()
// попыталась бы вновь выставить то же заведомо отклонённое ядром значение.
void handleApiSetCpuFreq() {
    if (!server.hasArg("value")) {
        server.send(400, "text/plain", "missing value");
        return;
    }
    int requested = server.arg("value").toInt();
    for (size_t i = 0; i < CPU_FREQ_OPTIONS_COUNT; i++) {
        if ((int) CPU_FREQ_OPTIONS[i] == requested) {
            bool ok = setCpuFrequencyMhz(CPU_FREQ_OPTIONS[i]);
            if (ok) {
                saveCpuFreqToNVS(CPU_FREQ_OPTIONS[i]);
                Serial.printf("Частота процессора установлена: %u МГц\n", (unsigned) CPU_FREQ_OPTIONS[i]);
                server.send(200, "text/plain", "ok");
            } else {
                Serial.printf("Не удалось установить частоту процессора %u МГц (отклонено ядром)\n",
                              (unsigned) CPU_FREQ_OPTIONS[i]);
                server.send(500, "text/plain", "core rejected this frequency");
            }
            return;
        }
    }
    server.send(400, "text/plain", "invalid value - not in the allowed set");
}

void setup() {
    Serial.begin(115200);

    pinMode(AP_LED_PIN, OUTPUT);
 //   digitalWrite(AP_LED_PIN, LOW); // безопасный начальный дефолт - реальное состояние выставит setApEnabled(), когда оператор явно включит AP
    apButton.begin(AP_BUTTON_PIN, AP_BUTTON_DEBOUNCE_MS);

    // --- DS3231 ---
    // Обнаруживаем ДО восстановления времени из NVS/сети - если часы
    // найдены и их время достоверно (не сработал OSF, см. Ds3231Rtc.h),
    // используем их немедленно как отправную точку, не дожидаясь
    // открытия веб-страницы в браузере.
    Wire.begin();
    if (rtc.begin()) {
        Serial.println("DS3231: часы реального времени ОБНАРУЖЕНЫ на шине I2C.");
        if (rtc.hasReliableTime()) {
            uint32_t epoch = rtc.getEpoch();
            struct timeval tv;
            tv.tv_sec = (time_t) epoch;
            tv.tv_usec = 0;
            settimeofday(&tv, nullptr);
            timeSynced = true;
            Serial.print("Время восстановлено из DS3231: "); Serial.println(currentTimeString());
        } else {
            Serial.println("DS3231: обнаружена потеря питания часов (OSF) - хранимое время "
                            "не заслуживает доверия, жду синхронизации с браузером.");
        }
    } else {
        Serial.println("DS3231: часы НЕ обнаружены - единственный источник времени - браузер "
                        "(часы Хаба сбрасываются на 1970-01-01 при каждой перезагрузке).");
    }

    // Восстанавливаем установленные устройства ИЗ NVS ДО начала приёма
    // ESP-NOW трафика - чтобы их слоты 0..count-1 были заняты раньше,
    // чем туда сможет попасть что-либо автообнаруженное.
    deviceManager.loadFromNVS();

    // Радиоканал фиксируем явно тем же ESPNOW_CHANNEL, что у периферии, ещё
    // ДО включения точки доступа (которая по умолчанию теперь выключена,
    // см. блок "Кнопка/точка доступа" выше) - иначе ESP-NOW окажется без
    // определённого канала, пока пользователь не нажмёт кнопку.
    WiFi.mode(WIFI_AP_STA);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    // Восстанавливаем мощность передатчика/частоту процессора из NVS СРАЗУ после
    // WiFi.mode() (раньше WiFi.setTxPower() не сработает - радио ещё не инициализировано)
    // и ДО esp_now_init()/точки доступа ниже - чтобы Хаб сразу начинал работу с
    // настройками, выбранными оператором в прошлый раз (см. loadRadioSettingsFromNVS()
    // выше и большой комментарий у TX_POWER_OPTIONS/CPU_FREQ_OPTIONS в начале файла).
    loadRadioSettingsFromNVS();

    WiFi.macAddress(myMac);
    Serial.print("Hub MAC: ");
    Serial.println(WiFi.macAddress());

    // Очередь - ДО инициализации ESP-NOW и регистрации колбэка, иначе
    // первый же пришедший пакет попытается писать в ещё не созданную
    // очередь (см. пояснение про однопоточность в начале файла).
    incomingPacketQueue = xQueueCreate(INCOMING_QUEUE_LENGTH, sizeof(UniversalPacket));
    if (incomingPacketQueue == nullptr) {
        Serial.println("Не удалось создать очередь входящих пакетов - ESP-NOW не будет инициализирован.");
    } else if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW Init Failed.");
    } else {
        esp_now_register_recv_cb(onDataRecv);

        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, BROADCAST_MAC, 6);
        peerInfo.channel = ESPNOW_CHANNEL;
        peerInfo.encrypt = false;
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("Failed to add broadcast peer");
        } else {
            radioReady = true;
        }
    }

    if (!radioReady) {
        // Без ESP-NOW Хаб бесполезен как центр сети (не примет и не
        // отправит ни одного пакета периферии) - не даём этому тихо
        // затеряться в логе: явный флаг + характерное мигание светодиода
        // (см. updateStatusLed(), вызывается из loop()). Веб-интерфейс и
        // Serial по-прежнему доступны - достаточно для диагностики на
        // месте, без необходимости тащить Хаб обратно к столу.
        Serial.println("ВНИМАНИЕ: радио ESP-NOW не готово - светодиод будет быстро мигать, пока "
                        "проблема не устранена (проверьте прошивку/питание/антенну).");
    }

    // Точка доступа при старте всегда выключена (apEnabled уже инициализирована в false выше,
    // и НИКАКОГО NVS-восстановления больше нет - см. большой комментарий у apEnabled выше),
    // поэтому вызывать setApEnabled() здесь не надо - WiFi.softAP() и так не вызывается,
    // пока оператор явно не нажмёт кнопку.

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/devices", HTTP_GET, handleApiDevices);
    server.on("/api/command", HTTP_POST, handleApiCommand);
    server.on("/api/setConfig", HTTP_POST, handleApiSetConfig);
    server.on("/api/install", HTTP_POST, handleApiInstall);
    server.on("/api/forget", HTTP_POST, handleApiForget);
    server.on("/api/rename", HTTP_POST, handleApiRename);
    server.on("/api/settime", HTTP_POST, handleApiSetTime);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/setTxPower", HTTP_POST, handleApiSetTxPower);
    server.on("/api/setCpuFreq", HTTP_POST, handleApiSetCpuFreq);
    server.begin();
    Serial.println("Веб-сервер запущен.");

    Serial.println("Serial-команды: введите 'help' для списка доступных команд (list, open, volume, close, "
                    "config, install, forget, rename, time, status, help).");

    // Объявляем о своей (пере)загрузке всем узлам сети - см. sendHubAnnounce().
    // Несколько повторов с паузой, а не один пакет, на случай потерь в эфире.
    // Работает независимо от состояния точки доступа - это ESP-NOW, не Wi-Fi AP.
    // Пропускаем, если радио не инициализировано - sendHubAnnounce() и так
    // ничего не сделает, но нет смысла тратить на это 3 секунды в setup().
    if (radioReady) {
        for (int i = 0; i < 3; i++) {
            sendHubAnnounce();
            delay(1000);
        }
    }
}

void loop() {
    if (apButton.wasPressed()) {
        setApEnabled(!apEnabled);
    }
    updateApAutoOff();
    updateStatusLed();

    server.handleClient();

    // Разбираем всё, что накопилось в очереди с последнего тика - см.
    // пояснение про однопоточность бизнес-логики в начале файла.
    // incomingPacketQueue может быть nullptr, если её создание в setup()
    // не удалось (тогда радио и так не инициализировано - очередь просто
    // всегда пуста).
    if (incomingPacketQueue != nullptr) {
        UniversalPacket incoming;
        while (xQueueReceive(incomingPacketQueue, &incoming, 0) == pdTRUE) {
            processIncomingPacket(incoming);
        }
    }

    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        handleSerialCommand(line);
    }
}
