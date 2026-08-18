#include <esp_now.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <GardenProtocol.h>
#include "WebPage.h"

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
// ВЕБ-ИНТЕРФЕЙС: Хаб поднимает СОБСТВЕННУЮ точку доступа Wi-Fi (режим
// WIFI_AP_STA), а не подключается к домашнему роутеру. Это осознанный
// выбор ради независимости от внешней сетевой инфраструктуры (в поле
// может не быть роутера вообще, вместо него может использоваться
// GSM-модем) и ради того, чтобы не решать проблему рассинхронизации
// радиоканала с чужим роутером (см. PROTOCOL.md §7/§10): раз Хаб сам
// создаёт точку доступа, канал полностью в его руках и совпадает с
// ESPNOW_CHANNEL по определению.
//
// УСТАНОВКА УСТРОЙСТВ: таблица устройств теперь двухуровневая.
//   - Автоматически обнаруженные (по трафику) - как и раньше, обычные
//     "кандидаты": подлежат LRU-вытеснению, если таблица заполнится, и
//     живут только в RAM (исчезают при перезагрузке Хаба, но быстро
//     переоткрываются заново по следующему пакету от того же MAC).
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
// старта. Часы не привязаны к батарейке (сбрасываются на 1970-01-01 при
// каждой перезагрузке без внешнего RTC-модуля) - поэтому нужна
// синхронизация при каждом запуске. Сейчас единственный источник времени
// - браузер: страница веб-интерфейса при загрузке сама шлёт Хабу текущее
// UTC-время (POST /api/settime). См. PROTOCOL.md §12.
//
// Как только Хаб получил время от браузера, он тут же разносит его дальше
// по ESP-NOW всем периферийным узлам - тем же broadcast-пакетом MSG_CONFIG,
// которым объявляет о своей (пере)загрузке (см. sendHubAnnounce()). Узлы
// подхватывают его в GardenNode::handleIncoming() и выставляют свои собственные
// часы - то же решение, что и у самого Хаба, просто на один шаг дальше по
// цепочке источника времени.
//
// Время - МЕСТНОЕ, не UTC: его уже в таком виде отдаёт браузер (см. ниже и
// WebPage.h) - Хаб просто хранит полученное значение как есть и никак его не
// пересчитывает.

#define MAX_DEVICES 64 // с запасом над плановыми 30-50 реальными устройствами

// Точка доступа для веб-интерфейса. Пароль ОБЯЗАН быть от 8 символов
// (требование WPA2) - поменяйте на свой перед использованием.
const char *AP_SSID = "SmartGarden-Hub";
const char *AP_PASSWORD = "garden123";

// NVS (энергонезависимая память ESP32) для хранения списка установленных
// устройств. Имя пространства ограничено 15 символами платформой.
const char *NVS_NAMESPACE = "smartgarden";
const char *NVS_KEY_DEVICES = "installed";

uint8_t myMac[6];

UniversalPacket txPacket;
UniversalPacket rxPacket;

uint16_t lastPacketId = 0; // счётчик ИСХОДЯЩИХ пакетов Хаба

WebServer server(80);

// true - хотя бы раз получили время от браузера. До этого показания
// time()/currentTimeString() бессмысленны (Хаб думает, что сейчас
// 1970-01-01, если нет внешнего RTC-модуля).
bool timeSynced = false;

// Таблица известных устройств - заполняется по мере прихода MSG_CONFIG/TELEMETRY,
// плюс восстанавливается из NVS при старте (installed-записи).
// Dedup per-MAC + сброс dedup при получении MSG_CONFIG ("устройство
// перезагрузилось") - логика подробно объяснена в GardenProtocol.h.
struct KnownDevice {
    bool used;
    bool installed;   // true - подтверждено пользователем, защищено от LRU, живёт в NVS
    uint8_t mac[6];
    uint8_t deviceType;
    uint16_t lastSeenPacketId;
    unsigned long lastSeenTime;
    uint8_t lastActiveValve; // кэш последней телеметрии - для веб-интерфейса
};

KnownDevice devices[MAX_DEVICES];

// Компактная запись для хранения в NVS - только то, что нужно, чтобы
// узнать устройство при следующей загрузке (MAC + тип). Остальные поля
// KnownDevice (lastSeenTime, lastActiveValve и т.п.) - оперативные,
// восстанавливаются из реального трафика после старта, хранить их не
// нужно.
struct InstalledDeviceRecord {
    uint8_t mac[6];
    uint8_t deviceType;
};

int findDevice(const uint8_t *mac) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].used && memcmp(devices[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

// Сохраняет ТЕКУЩИЙ набор installed-устройств в NVS целиком. Вызывается
// только по явному действию пользователя (install/forget) - события
// редкие, поэтому цельная перезапись каждый раз не создаёт проблем с
// износом флеша (в отличие от записи на каждый пакет).
void saveInstalledDevicesToNVS() {
    InstalledDeviceRecord records[MAX_DEVICES];
    int count = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].used && devices[i].installed) {
            memcpy(records[count].mac, devices[i].mac, 6);
            records[count].deviceType = devices[i].deviceType;
            count++;
        }
    }

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBytes(NVS_KEY_DEVICES, records, count * sizeof(InstalledDeviceRecord));
    prefs.end();
    Serial.printf("Сохранено в NVS: %d установленных устройств\n", count);
}

// Записывает устройство в слот i (новая регистрация ИЛИ вытеснение
// старой записи) и логирует это. installed по умолчанию false - вызов
// из loadInstalledDevicesFromNVS() выставляет его в true отдельно.
void registerDeviceAt(int i, const uint8_t *mac, uint8_t deviceType) {
    devices[i].used = true;
    devices[i].installed = false;
    memcpy(devices[i].mac, mac, 6);
    devices[i].deviceType = deviceType;
    devices[i].lastSeenPacketId = 0;
    devices[i].lastSeenTime = millis();
    devices[i].lastActiveValve = 0;
    Serial.printf("Новое устройство #%d, type=%u, MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  i, deviceType, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Восстанавливает установленные устройства из NVS - вызывается один раз
// в setup(), до начала приёма ESP-NOW трафика. Занимает слоты 0..count-1
// таблицы - до того, как туда сможет попасть что-либо автообнаруженное.
void loadInstalledDevicesFromNVS() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true); // read-only
    size_t len = prefs.getBytesLength(NVS_KEY_DEVICES);
    if (len == 0) {
        prefs.end();
        Serial.println("В NVS нет сохранённых установленных устройств.");
        return;
    }

    int count = len / sizeof(InstalledDeviceRecord);
    if (count > MAX_DEVICES) count = MAX_DEVICES; // защита на случай ручной правки NVS

    InstalledDeviceRecord records[MAX_DEVICES];
    prefs.getBytes(NVS_KEY_DEVICES, records, count * sizeof(InstalledDeviceRecord));
    prefs.end();

    for (int i = 0; i < count; i++) {
        registerDeviceAt(i, records[i].mac, records[i].deviceType);
        devices[i].installed = true;
    }
    Serial.printf("Восстановлено из NVS: %d установленных устройств\n", count);
}

int findOrRegisterDevice(const uint8_t *mac, uint8_t deviceType) {
    int idx = findDevice(mac);
    if (idx >= 0) return idx;

    // Свободный слот есть - используем его
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!devices[i].used) {
            registerDeviceAt(i, mac, deviceType);
            return i;
        }
    }

    // Таблица полна: вытесняем запись, которая дольше всех молчала (LRU) -
    // НО ТОЛЬКО среди НЕ установленных (installed=false). Установленные
    // устройства защищены от вытеснения по определению - убрать их можно
    // только явным forget от пользователя (см. handleApiForget()).
    //
    // ПРИМЕЧАНИЕ: сравнение lastSeenTime через millis() не защищено от
    // переполнения (происходит примерно раз в 49 дней непрерывной
    // работы) - в редком краевом случае это может выбрать не совсем
    // самую старую запись. Для целей LRU-вытеснения (а не dedup, где
    // это было бы критично) такая неточность не страшна.
    int oldestIdx = -1;
    unsigned long oldestTime = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].installed) continue; // защищены
        if (oldestIdx == -1 || devices[i].lastSeenTime < oldestTime) {
            oldestTime = devices[i].lastSeenTime;
            oldestIdx = i;
        }
    }

    if (oldestIdx == -1) {
        // Все MAX_DEVICES слотов заняты УСТАНОВЛЕННЫМИ устройствами -
        // вытеснять нечего, новому (ещё не установленному) кандидату
        // просто не хватило места. Штатная ситуация только если реально
        // установлено MAX_DEVICES устройств - при плановых 30-50 узлах
        // и MAX_DEVICES=64 такое маловероятно, но не невозможно.
        Serial.println("Таблица устройств полностью занята УСТАНОВЛЕННЫМИ "
                        "устройствами - новый кандидат не добавлен");
        return -1;
    }

    Serial.printf("Таблица устройств полна - вытесняю #%d (молчал %lus) под новое устройство\n",
                  oldestIdx, (millis() - oldestTime) / 1000);
    registerDeviceAt(oldestIdx, mac, deviceType);
    return oldestIdx;
}

void printMac(const uint8_t *mac) {
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// --- Часы ---

// Форматирует текущее время Хаба в читаемый вид - для Serial и
// веб-интерфейса. Хранимое значение - МЕСТНОЕ время (его уже таким присылает
// браузер, сдвинув свой часовой пояс перед отправкой, см. WebPage.h), поэтому
// тут оно просто читается `gmtime_r()`-ом как есть, без какого-либо повторного
// сдвига на стороне Хаба - использование именно `gmtime_r()`, а не `localtime_r()`,
// здесь не означает "показываем UTC" - просто не тащим на ESP32 встроенную
// систему перевода часовых поясов/DST, которая тут не нужна - пояс уже
// учтён браузером до отправки (см. PROTOCOL.md §12).
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
    prepareHeader(MSG_CONFIG, BROADCAST_MAC);
    txPacket.payload.hub.epoch = timeSynced ? (uint32_t) time(nullptr) : 0;
    esp_now_send(BROADCAST_MAC, (uint8_t *) &txPacket, sizeof(txPacket));
    Serial.printf("Sent: HUB ANNOUNCE, packet_id=%u, epoch=%lu\n",
                  txPacket.packet_id, (unsigned long) txPacket.payload.hub.epoch);
}

void sendCommand(int deviceIdx, uint8_t targetValve, uint8_t mode, uint16_t durationSec, uint16_t volumeL) {
    if (deviceIdx < 0 || deviceIdx >= MAX_DEVICES || !devices[deviceIdx].used) {
        Serial.println("Нет такого устройства (см. 'list')");
        return;
    }
    prepareHeader(MSG_COMMAND, devices[deviceIdx].mac);
    txPacket.payload.irrigation.command.target_valve = targetValve;
    txPacket.payload.irrigation.command.mode = mode;
    txPacket.payload.irrigation.command.duration_sec = durationSec;
    txPacket.payload.irrigation.command.volume_l = volumeL;

    esp_err_t result = esp_now_send(BROADCAST_MAC, (uint8_t *) &txPacket, sizeof(txPacket));
    if (result != ESP_OK) {
        Serial.printf("esp_now_send failed: %d\n", (int) result);
    }

    Serial.print("Sent COMMAND to #"); Serial.print(deviceIdx);
    Serial.printf(" valve=%u mode=%u duration_sec=%u volume_l=%u (packet_id=%u)\n",
                  targetValve, mode, durationSec, volumeL, txPacket.packet_id);
}

// Разбор payload устройств полива (TYPE_IRRIGATION). Когда появится
// TYPE_LIGHTING/др. - добавляется своя handleXxxPayload() и case ниже,
// эта функция не меняется.
void handleIrrigationPayload(int idx, const UniversalPacket &pkt) {
    if (pkt.msg_type == MSG_CONFIG) {
        IrrigationSpec spec = pkt.payload.irrigation.spec;
        Serial.printf("CONFIG (IRRIGATION) от #%d: valve_count=%u has_flow_sensor=%u\n",
                      idx, spec.valve_count, spec.has_flow_sensor);
    } else if (pkt.msg_type == MSG_TELEMETRY) {
        IrrigationTelemetry t = pkt.payload.irrigation.telemetry;
        devices[idx].lastActiveValve = t.active_valve; // кэш для веб-интерфейса
        Serial.printf("TELEMETRY (IRRIGATION) от #%d: active_valve=%u current_flow=%lu total_water_used=%lu\n",
                      idx, t.active_valve, (unsigned long) t.current_flow, (unsigned long) t.total_water_used);
    } else {
        Serial.printf("IRRIGATION: неожиданный msg_type=%u от #%d\n", pkt.msg_type, idx);
    }
}

// Колбэк приёма - сигнатура актуальная для arduino-esp32 3.x.
// МAC отправителя в протоколе передаётся ВНУТРИ пакета (rxPacket.sender_mac),
// поэтому info->src_addr (MAC на радиоуровне) здесь не используется -
// это совпадает с тем, как уже был спроектирован протокол для
// прохождения через ретрансляторы.
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
    if (len != (int) sizeof(UniversalPacket)) return;
    memcpy(&rxPacket, incomingData, sizeof(UniversalPacket));

    // Хаб принимает только то, что адресовано ему (HUB_MAC)
    if (memcmp(rxPacket.receiver_mac, HUB_MAC, 6) != 0) return;

    int idx = findOrRegisterDevice(rxPacket.sender_mac, rxPacket.device_type);
    if (idx < 0) return;

    // MSG_CONFIG = устройство только что (пере)загрузилось -> сбрасываем
    // dedup для него, иначе после ребута узла его пакеты будут выглядеть
    // "старыми" относительно уже виденных ранее номеров (см. GardenProtocol.h).
    if (rxPacket.msg_type == MSG_CONFIG) {
        devices[idx].lastSeenPacketId = 0;

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

    if (!isNewerPacketId(rxPacket.packet_id, devices[idx].lastSeenPacketId)) {
        Serial.print("Игнорирован дубликат/старый пакет от #"); Serial.print(idx);
        Serial.printf(" packet_id=%u (last=%u)\n", rxPacket.packet_id, devices[idx].lastSeenPacketId);
        return;
    }
    devices[idx].lastSeenPacketId = rxPacket.packet_id;
    devices[idx].lastSeenTime = millis();

    // ACK - формат общий для всех типов устройств, не зависит от device_type.
    if (rxPacket.msg_type == MSG_ACK) {
        AckData ack = rxPacket.payload.ack;
        Serial.printf("ACK от #%d: acked_packet_id=%u status=%u\n",
                      idx, ack.acked_packet_id, ack.status);
        return;
    }

    // CONFIG/TELEMETRY - формат payload зависит от device_type, поэтому
    // диспетчеризуем ПО ТИПУ УСТРОЙСТВА, а не только по msg_type. Без
    // этой проверки Хаб попытался бы читать payload незнакомого типа
    // устройства как IrrigationSpec/Telemetry.
    switch (rxPacket.device_type) {
        case TYPE_IRRIGATION:
            handleIrrigationPayload(idx, rxPacket);
            break;
        // TODO: case TYPE_LIGHTING: handleLightingPayload(idx, rxPacket); break;
        // TODO: case TYPE_WEATHER:  handleWeatherPayload(idx, rxPacket); break;
        default:
            Serial.printf("Устройство #%d: device_type=%u ещё не поддержан Хабом, payload (msg_type=%u) проигнорирован\n",
                          idx, rxPacket.device_type, rxPacket.msg_type);
    }
}

void listDevices() {
    Serial.println("--- Известные устройства ---");
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!devices[i].used) continue;
        Serial.print("#"); Serial.print(i); Serial.print(" MAC=");
        printMac(devices[i].mac);
        Serial.printf(" type=%u lastSeenPacketId=%u %lus назад %s\n",
                      devices[i].deviceType, devices[i].lastSeenPacketId,
                      (millis() - devices[i].lastSeenTime) / 1000,
                      devices[i].installed ? "[УСТАНОВЛЕНО]" : "[кандидат]");
    }
    Serial.println("-----------------------------");
}

// Простой текстовый интерфейс через Serial Monitor - работает параллельно
// с веб-интерфейсом, один не отменяет другой:
//   list                          - показать известные устройства
//   open <idx> <valve> <sec>      - открыть клапан на N секунд (mode=0)
//   volume <idx> <valve> <liters> - открыть клапан на N литров (mode=1)
//   close <idx>                   - закрыть все клапаны устройства
//   install <idx>                 - подтвердить устройство, защитить от вытеснения, сохранить в NVS
//   forget <idx>                  - удалить устройство (установленное или нет) из таблицы и NVS
//   time                          - показать текущее время Хаба и статус синхронизации с браузером
void handleSerialCommand(String line) {
    line.trim();
    if (line.length() == 0) return;

    int sp1 = line.indexOf(' ');
    String cmd = sp1 == -1 ? line : line.substring(0, sp1);

    if (cmd == "list") {
        listDevices();
    } else if (cmd == "open") {
        int idx, valve, sec;
        if (sscanf(line.c_str(), "open %d %d %d", &idx, &valve, &sec) == 3) {
            sendCommand(idx, valve, /*mode=*/0, /*duration_sec=*/sec, /*volume_l=*/0);
        } else {
            Serial.println("Использование: open <idx> <valve> <sec>");
        }
    } else if (cmd == "volume") {
        int idx, valve, liters;
        if (sscanf(line.c_str(), "volume %d %d %d", &idx, &valve, &liters) == 3) {
            sendCommand(idx, valve, /*mode=*/1, /*duration_sec=*/0, /*volume_l=*/liters);
        } else {
            Serial.println("Использование: volume <idx> <valve> <liters>");
        }
    } else if (cmd == "close") {
        int idx;
        if (sscanf(line.c_str(), "close %d", &idx) == 1) {
            sendCommand(idx, /*target_valve=*/0, /*mode=*/0, /*duration_sec=*/0, /*volume_l=*/0);
        } else {
            Serial.println("Использование: close <idx>");
        }
    } else if (cmd == "install") {
        int idx;
        if (sscanf(line.c_str(), "install %d", &idx) == 1) {
            if (idx < 0 || idx >= MAX_DEVICES || !devices[idx].used) {
                Serial.println("Нет такого устройства (см. 'list')");
            } else {
                devices[idx].installed = true;
                saveInstalledDevicesToNVS();
                Serial.printf("Устройство #%d установлено и защищено от вытеснения.\n", idx);
            }
        } else {
            Serial.println("Использование: install <idx>");
        }
    } else if (cmd == "forget") {
        int idx;
        if (sscanf(line.c_str(), "forget %d", &idx) == 1) {
            if (idx < 0 || idx >= MAX_DEVICES || !devices[idx].used) {
                Serial.println("Нет такого устройства (см. 'list')");
            } else {
                devices[idx].used = false;
                devices[idx].installed = false;
                saveInstalledDevicesToNVS();
                Serial.printf("Устройство #%d удалено из таблицы и NVS.\n", idx);
            }
        } else {
            Serial.println("Использование: forget <idx>");
        }
    } else if (cmd == "time") {
        // Смотреть результат синхронизации не только в момент самого
        // POST /api/settime (та строка быстро уходит вверх по Serial
        // Monitor), а по запросу в любой момент - тот же timeSynced и
        // currentTimeString(), что отдаёт и GET /api/status.
        if (timeSynced) {
            Serial.print("Время Хаба синхронизировано с браузером: ");
            Serial.println(currentTimeString());
        } else {
            Serial.println("Время Хаба ещё НЕ синхронизировано - ждём загрузки веб-страницы "
                            "в браузере (см. PROTOCOL.md §12).");
        }
    } else {
        Serial.println("Команды: list | open <idx> <valve> <sec> | volume <idx> <valve> <liters> | "
                        "close <idx> | install <idx> | forget <idx> | time");
    }
}

// --- Веб-интерфейс ---
// HTML страницы (константа INDEX_HTML) вынесена в отдельный файл
// WebPage.h рядом - Arduino IDE покажет его отдельной вкладкой в том же
// окне редактора, не засоряя логику в hub.ino огромным raw-string-литералом.

void handleRoot() {
    server.send(200, "text/html", INDEX_HTML);
}

// GET /api/devices - список известных устройств в JSON. Собирается
// вручную (без ArduinoJson) - таблица маленькая, лишняя зависимость не
// нужна.
void handleApiDevices() {
    String json = "[";
    bool first = true;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!devices[i].used) continue;
        if (!first) json += ",";
        first = false;

        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 devices[i].mac[0], devices[i].mac[1], devices[i].mac[2],
                 devices[i].mac[3], devices[i].mac[4], devices[i].mac[5]);
        unsigned long agoSec = (millis() - devices[i].lastSeenTime) / 1000;

        json += "{\"idx\":" + String(i) +
                ",\"mac\":\"" + String(macStr) + "\"" +
                ",\"type\":" + String(devices[i].deviceType) +
                ",\"agoSec\":" + String(agoSec) +
                ",\"activeValve\":" + String(devices[i].lastActiveValve) +
                ",\"installed\":" + String(devices[i].installed ? "true" : "false") + "}";
    }
    json += "]";
    server.send(200, "application/json", json);
}

// POST /api/command - form-urlencoded: idx, valve, mode, duration, volume.
// Ровно то же самое, что делают Serial-команды open/volume/close - просто
// другой вход в ту же sendCommand(). Работает и для не-установленных
// кандидатов - можно проверить/опознать устройство физически (например,
// подёргать клапан) ДО того, как решить его устанавливать.
void handleApiCommand() {
    if (!server.hasArg("idx")) {
        server.send(400, "text/plain", "missing idx");
        return;
    }
    int idx = server.arg("idx").toInt();
    int valve = server.hasArg("valve") ? server.arg("valve").toInt() : 0;
    int mode = server.hasArg("mode") ? server.arg("mode").toInt() : 0;
    int duration = server.hasArg("duration") ? server.arg("duration").toInt() : 0;
    int volume = server.hasArg("volume") ? server.arg("volume").toInt() : 0;

    sendCommand(idx, (uint8_t) valve, (uint8_t) mode, (uint16_t) duration, (uint16_t) volume);
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
    if (idx < 0 || idx >= MAX_DEVICES || !devices[idx].used) {
        server.send(404, "text/plain", "device not found");
        return;
    }
    devices[idx].installed = true;
    saveInstalledDevicesToNVS();
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
    if (idx < 0 || idx >= MAX_DEVICES || !devices[idx].used) {
        server.send(404, "text/plain", "device not found");
        return;
    }
    devices[idx].used = false;
    devices[idx].installed = false;
    saveInstalledDevicesToNVS();
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

    // Сразу же разносим новое время по ESP-NOW всем периферийным узлам -
    // чтобы уже работающие узлы получили время сразу, не дожидаясь своего
    // собственного очередного MSG_CONFIG (база ~1 час) или перезагрузки Хаба.
    // Срабатывает это же при каждой повторной синхронизации страницы раз в 30 минут
    // (см. WebPage.h) - попутно тоже компенсирует дрейф часов узлов, не только Хаба.
    sendHubAnnounce();

    server.send(200, "text/plain", "ok");
}

// GET /api/status - служебная информация Хаба, сейчас только про часы -
// веб-страница опрашивает это в том же цикле автообновления, что и
// /api/devices, чтобы показать оператору визуальное подтверждение, что
// синхронизация времени сработала.
void handleApiStatus() {
    String json = "{";
    json += "\"timeSynced\":" + String(timeSynced ? "true" : "false");
    json += ",\"epoch\":" + String((unsigned long) time(nullptr));
    json += ",\"timeString\":\"" + currentTimeString() + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void setup() {
    Serial.begin(115200);

    // Восстанавливаем установленные устройства ИЗ NVS ДО начала приёма
    // ESP-NOW трафика - чтобы их слоты 0..count-1 были заняты раньше,
    // чем туда сможет попасть что-либо автообнаруженное.
    loadInstalledDevicesFromNVS();

    // Хаб поднимает СВОЮ точку доступа вместо подключения к домашнему
    // роутеру - канал задаём явно тем же ESPNOW_CHANNEL, что у периферии,
    // поэтому отдельно синхронизировать канал с чем-либо ещё не нужно.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD, ESPNOW_CHANNEL);
    Serial.print("Точка доступа: "); Serial.println(AP_SSID);
    Serial.print("Веб-интерфейс: http://"); Serial.println(WiFi.softAPIP());

    WiFi.macAddress(myMac);
    Serial.print("Hub MAC: ");
    Serial.println(WiFi.macAddress());

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW Init Failed");
        return;
    }

    esp_now_register_recv_cb(onDataRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, BROADCAST_MAC, 6);
    peerInfo.channel = ESPNOW_CHANNEL;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Failed to add broadcast peer");
        return;
    }

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/devices", HTTP_GET, handleApiDevices);
    server.on("/api/command", HTTP_POST, handleApiCommand);
    server.on("/api/install", HTTP_POST, handleApiInstall);
    server.on("/api/forget", HTTP_POST, handleApiForget);
    server.on("/api/settime", HTTP_POST, handleApiSetTime);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.begin();
    Serial.println("Веб-сервер запущен.");

    Serial.println("Serial-команды: list | open <idx> <valve> <sec> | volume <idx> <valve> <liters> | "
                    "close <idx> | install <idx> | forget <idx> | time");

    // Объявляем о своей (пере)загрузке всем узлам сети - см. sendHubAnnounce().
    // Несколько повторов с паузой, а не один пакет, на случай потерь в эфире.
    for (int i = 0; i < 3; i++) {
        sendHubAnnounce();
        delay(1000);
    }
}

void loop() {
    server.handleClient();

    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        handleSerialCommand(line);
    }
}
