#include "DeviceManager.h"
#include <Preferences.h>

const char *DeviceManager::NVS_NAMESPACE = "smartgarden";
const char *DeviceManager::NVS_KEY_DEVICES = "installed";

// Подстраховка на этапе компиляции: если когда-нибудь появится наследник
// Device крупнее выделенного слота (deviceStorageSize() в
// DeviceManager.h не обновили при его добавлении), этот static_assert
// сработает при сборке - вместо того, чтобы объект тихо повредил память
// соседнего слота в рантайме. Добавляйте сюда по одной строке на каждый
// новый наследник.
static_assert(sizeof(IrrigationDevice) <= deviceStorageSize(),
              "IrrigationDevice не помещается в зарезервированный слот DeviceManager - "
              "см. deviceStorageSize() в DeviceManager.h");

DeviceManager::~DeviceManager() {
    for (int i = 0; i < MAX_DEVICES; i++) destroyAt(i);
}

void DeviceManager::destroyAt(int i) {
    if (devices[i] != nullptr) {
        devices[i]->~Device(); // явный вызов деструктора - объект создан через placement new, не через обычный new
        devices[i] = nullptr;
    }
}

void DeviceManager::registerAt(int i, const uint8_t *mac, uint8_t deviceType) {
    destroyAt(i); // если слот переиспользуется (LRU-вытеснение) - сначала разрушить прежнего жильца

    void *mem = storage[i];
    Device *d;
    switch (deviceType) {
        case TYPE_IRRIGATION:
            d = new (mem) IrrigationDevice(mac);
            break;
        // TODO: case TYPE_LIGHTING: d = new (mem) LightingDevice(mac); break;
        default:
            d = new (mem) GenericDevice(mac, deviceType);
    }
    devices[i] = d;

    Serial.printf("Новое устройство #%d, type=%u, MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  i, deviceType, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void DeviceManager::saveToNVS() {
    // static, НЕ локальный массив на стеке - с добавлением valveSchedules в InstalledDeviceRecord каждая
    // запись заметно выросла, а records[MAX_DEVICES] на стеке задачи loop() (8КБ по умолчанию на
    // ESP32, общий со всем остальным, что делает loop()) рисковал бы переполнить его при install/
    // rename/setValveSchedule (все вызывают saveToNVS()) - static кладёт массив в .bss вместо стека,
    // цена - функция больше не реентрабельна (некритично — вызывается только из loop()-задачи,
    // см. комментарий про "Однопоточность бизнес-логики" в hub.ino).
    static InstalledDeviceRecord records[MAX_DEVICES];
    int count = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i] != nullptr && devices[i]->installed) {
            memcpy(records[count].mac, devices[i]->mac, 6);
            records[count].deviceType = devices[i]->deviceType;
            memcpy(records[count].name, devices[i]->name, sizeof(records[count].name));
            // Настройки автополива есть ТОЛЬКО у IrrigationDevice (см. комментарий у valveSchedules в
            // InstalledDeviceRecord выше) - для остальных типов устройств просто зануляем блок - даункастинг
            // на IrrigationDevice* безопасен, т.к. делается ТОЛЬКО после проверки deviceType (та же
            // схема, что и в registerAt() выше - там DeviceManager тоже уже "знает" про IrrigationDevice конкретно).
            if (devices[i]->deviceType == TYPE_IRRIGATION) {
                IrrigationDevice *irr = static_cast<IrrigationDevice *>(devices[i]);
                memcpy(records[count].valveSchedules, irr->valveSchedules, sizeof(records[count].valveSchedules));
            } else {
                memset(records[count].valveSchedules, 0, sizeof(records[count].valveSchedules));
            }
            count++;
        }
    }
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBytes(NVS_KEY_DEVICES, records, count * sizeof(InstalledDeviceRecord));
    prefs.end();
    Serial.printf("Сохранено в NVS: %d установленных устройств\n", count);
}

void DeviceManager::loadFromNVS() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true); // read-only
    size_t len = prefs.getBytesLength(NVS_KEY_DEVICES);
    if (len == 0) {
        prefs.end();
        Serial.println("В NVS нет сохранённых установленных устройств.");
        return;
    }

    int count = len / sizeof(InstalledDeviceRecord);
    if (count > MAX_DEVICES) count = MAX_DEVICES; // защита от повреждённых/чужих данных в NVS

    // static - та же причина, что и у records в saveToNVS() выше (см. там комментарий) - здесь это
    // важнее ещё и потому, что loadFromNVS() вызывается из setup() до того, как стек Wi-Fi/ESP-NOW
    // успел нарастить своё потребление стека от других локальных переменных.
    static InstalledDeviceRecord records[MAX_DEVICES];
    prefs.getBytes(NVS_KEY_DEVICES, records, count * sizeof(InstalledDeviceRecord));
    prefs.end();

    for (int i = 0; i < count; i++) {
        registerAt(i, records[i].mac, records[i].deviceType);
        devices[i]->installed = true;
        memcpy(devices[i]->name, records[i].name, sizeof(devices[i]->name));
        // Симметрично saveToNVS() выше - настройки автополива восстанавливаются ТОЛЬКО для
        // только что созданного registerAt() IrrigationDevice - для прочих типов в записи всё равно
        // только занулённые байты (см. saveToNVS()).
        if (records[i].deviceType == TYPE_IRRIGATION) {
            IrrigationDevice *irr = static_cast<IrrigationDevice *>(devices[i]);
            memcpy(irr->valveSchedules, records[i].valveSchedules, sizeof(irr->valveSchedules));
        }
    }
    Serial.printf("Восстановлено из NVS: %d установленных устройств\n", count);
}

int DeviceManager::find(const uint8_t *mac) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i] != nullptr && memcmp(devices[i]->mac, mac, 6) == 0) return i;
    }
    return -1;
}

int DeviceManager::findOrRegister(const uint8_t *mac, uint8_t deviceType) {
    int idx = find(mac);
    if (idx >= 0) return idx;

    // Свободный слот?
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i] == nullptr) {
            registerAt(i, mac, deviceType);
            return i;
        }
    }

    // Таблица полна - ищем самого "молчаливого" НЕ установленного
    // кандидата на вытеснение (LRU). Установленные устройства в этом
    // поиске не участвуют вообще.
    int oldestIdx = -1;
    unsigned long oldestTime = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i]->installed) continue;
        if (oldestIdx == -1 || devices[i]->lastSeenTime < oldestTime) {
            oldestTime = devices[i]->lastSeenTime;
            oldestIdx = i;
        }
    }

    if (oldestIdx == -1) {
        Serial.println("Таблица устройств полностью занята УСТАНОВЛЕННЫМИ "
                        "устройствами - новый кандидат не добавлен");
        return -1;
    }

    Serial.printf("Таблица устройств полна - вытесняю #%d (молчал %lus) под новое устройство\n",
                  oldestIdx, (millis() - oldestTime) / 1000);
    registerAt(oldestIdx, mac, deviceType);
    return oldestIdx;
}

bool DeviceManager::isValid(int idx) const {
    return idx >= 0 && idx < MAX_DEVICES && devices[idx] != nullptr;
}

bool DeviceManager::install(int idx) {
    if (!isValid(idx)) return false;
    devices[idx]->installed = true;
    saveToNVS();
    return true;
}

bool DeviceManager::forget(int idx) {
    if (!isValid(idx)) return false;
    destroyAt(idx);
    saveToNVS();
    return true;
}

bool DeviceManager::setName(int idx, const char *newName) {
    if (!isValid(idx) || !devices[idx]->installed) return false;
    if (newName == nullptr) newName = "";
    strncpy(devices[idx]->name, newName, DEVICE_NAME_MAX_LEN - 1);
    devices[idx]->name[DEVICE_NAME_MAX_LEN - 1] = '\0';
    saveToNVS();
    return true;
}

bool DeviceManager::setValveSchedule(int idx, int valve, uint8_t intervalDays, uint16_t volumeDl, bool autoEnabled) {
    if (!isValid(idx) || !devices[idx]->installed) return false;
    if (devices[idx]->deviceType != TYPE_IRRIGATION) return false;
    if (valve < 1 || valve > MAX_IRRIGATION_VALVES) return false;
    if (intervalDays < 1 || intervalDays > 7) return false;

    IrrigationDevice *irr = static_cast<IrrigationDevice *>(devices[idx]);
    ValveSchedule &sched = irr->valveSchedules[valve - 1];
    sched.intervalDays = intervalDays;
    sched.volumeDl = volumeDl;
    sched.autoEnabled = autoEnabled ? 1 : 0;
    saveToNVS();
    return true;
}

void DeviceManager::printList() const {
    Serial.println("--- Известные устройства ---");
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i] == nullptr) continue;
        const uint8_t *mac = devices[i]->mac;
        Serial.printf("#%d MAC=%02X:%02X:%02X:%02X:%02X:%02X type=%u lastSeenPacketId=%u %lus назад %s",
                      i, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                      devices[i]->deviceType, devices[i]->lastSeenPacketId,
                      (millis() - devices[i]->lastSeenTime) / 1000,
                      devices[i]->installed ? "[УСТАНОВЛЕНО]" : "[кандидат]");
        if (devices[i]->name[0] != '\0') {
            Serial.printf(" name=\"%s\"", devices[i]->name);
        }
        devices[i]->printExtra();
        Serial.println();
    }
    Serial.println("-----------------------------");
}
