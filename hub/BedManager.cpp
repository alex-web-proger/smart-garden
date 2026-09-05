#include "BedManager.h"
#include <Preferences.h>

// Тот же NVS-namespace, что и у DeviceManager/hub.ino ("smartgarden") -
// конфликта нет, ключ свой ("beds"), отдельный namespace ради ещё одного
// маленького блоба не оправдан (тот же аргумент, что и у RADIO_NVS_NAMESPACE
// в hub.ino).
static const char *BEDS_NVS_NAMESPACE = "smartgarden";
static const char *BEDS_NVS_KEY = "beds";

// Компактная запись для хранения в NVS - только то, что задаёт грядку, без
// служебного поля used (при чтении подряд восстановленные грядки просто
// занимают слоты 0..count-1 - тот же приём, что и у
// DeviceManager::InstalledDeviceRecord в DeviceManager.h/.cpp).
struct BedRecord {
    char name[BED_NAME_MAX_LEN];
    uint8_t cropId;
    uint8_t mac[6]; // MAC модуля полива - см. большой комментарий в BedManager.h про ссылку по MAC
    uint8_t valve;
};

void BedManager::loadFromNVS() {
    Preferences prefs;
    prefs.begin(BEDS_NVS_NAMESPACE, true); // read-only
    size_t len = prefs.getBytesLength(BEDS_NVS_KEY);
    if (len == 0 || len % sizeof(BedRecord) != 0) {
        // Ничего не сохранено (первый запуск) либо длина не кратна размеру
        // записи (повреждение/чужие данные под этим ключом) - в обоих
        // случаях просто ничего не восстанавливаем, а не падаем и не
        // пытаемся распарсить частичную запись.
        prefs.end();
        return;
    }
    size_t count = len / sizeof(BedRecord);
    if (count > MAX_BEDS) count = MAX_BEDS; // защита на случай уменьшения MAX_BEDS в будущей прошивке

    BedRecord records[MAX_BEDS]; // на стеке, без heap-аллокации - см. большой комментарий
                                  // в DeviceManager.h, почему на этой платформе стоит избегать
                                  // частых new/delete; здесь это разовый вызов из setup(), но
                                  // фиксированный стековый буфер проще и ничем не хуже.
    prefs.getBytes(BEDS_NVS_KEY, records, count * sizeof(BedRecord));
    prefs.end();

    for (size_t i = 0; i < count; i++) {
        beds[i].used = true;
        strncpy(beds[i].name, records[i].name, BED_NAME_MAX_LEN - 1);
        beds[i].name[BED_NAME_MAX_LEN - 1] = '\0';
        beds[i].cropId = records[i].cropId;
        memcpy(beds[i].mac, records[i].mac, 6);
        beds[i].valve = records[i].valve;
    }
    Serial.printf("Грядки восстановлены из NVS: %u шт.\n", (unsigned) count);
}

void BedManager::saveToNVS() {
    BedRecord records[MAX_BEDS];
    size_t count = 0;
    for (int i = 0; i < MAX_BEDS; i++) {
        if (!beds[i].used) continue;
        strncpy(records[count].name, beds[i].name, BED_NAME_MAX_LEN - 1);
        records[count].name[BED_NAME_MAX_LEN - 1] = '\0';
        records[count].cropId = beds[i].cropId;
        memcpy(records[count].mac, beds[i].mac, 6);
        records[count].valve = beds[i].valve;
        count++;
    }

    Preferences prefs;
    prefs.begin(BEDS_NVS_NAMESPACE, false);
    prefs.putBytes(BEDS_NVS_KEY, records, count * sizeof(BedRecord));
    prefs.end();
}

int BedManager::addBed(const char *name, uint8_t cropId, const uint8_t *mac, uint8_t valve) {
    for (int i = 0; i < MAX_BEDS; i++) {
        if (beds[i].used) continue;
        beds[i].used = true;
        strncpy(beds[i].name, name, BED_NAME_MAX_LEN - 1);
        beds[i].name[BED_NAME_MAX_LEN - 1] = '\0';
        beds[i].cropId = cropId;
        memcpy(beds[i].mac, mac, 6);
        beds[i].valve = valve;
        saveToNVS();
        return i;
    }
    return -1; // таблица заполнена (MAX_BEDS=32) - маловероятно, но вызывающий код (hub.ino)
               // должен ответить оператору внятной ошибкой, а не тихо промолчать
}

bool BedManager::updateBed(int id, const char *name, uint8_t cropId, const uint8_t *mac, uint8_t valve) {
    if (!isValid(id)) return false;
    strncpy(beds[id].name, name, BED_NAME_MAX_LEN - 1);
    beds[id].name[BED_NAME_MAX_LEN - 1] = '\0';
    beds[id].cropId = cropId;
    memcpy(beds[id].mac, mac, 6);
    beds[id].valve = valve;
    saveToNVS();
    return true;
}

bool BedManager::deleteBed(int id) {
    if (!isValid(id)) return false;
    beds[id] = GardenBed(); // сброс к дефолтным значениям конструктора - used становится false
    saveToNVS();
    return true;
}

bool BedManager::isValveTakenByOtherBed(const uint8_t *mac, uint8_t valve, int excludeId) const {
    for (int i = 0; i < MAX_BEDS; i++) {
        if (!beds[i].used) continue;
        if (i == excludeId) continue;
        if (beds[i].valve != valve) continue;
        if (memcmp(beds[i].mac, mac, 6) == 0) return true;
    }
    return false;
}

bool BedManager::isValid(int id) const {
    return id >= 0 && id < MAX_BEDS && beds[id].used;
}
