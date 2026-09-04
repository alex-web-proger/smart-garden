#include "SerialCommands.h"
#include "DeviceManager.h"
#include <WiFi.h>

// Глобальные объекты/функции живут в hub.ino - это отдельная единица
// трансляции (в отличие от .ino-файлов, .cpp-файлы не склеиваются Arduino
// IDE в один файл автоматически), поэтому доступ к ним - через extern.
extern DeviceManager deviceManager;
extern bool timeSynced;
extern bool apEnabled; // состояние точки доступа - для команды status ниже
extern float fanOnTempC;   // порог включения вентилятора охлаждения (см. hub.ino) - для status ниже
extern float fanOffTempC;  // порог выключения - там же
extern bool fanOn;         // текущее состояние вентилятора - там же
extern String currentTimeString();
extern void sendCommand(int deviceIdx, uint8_t targetValve, uint8_t action, uint8_t mode,
                         uint16_t durationSec, uint16_t volumeL, uint16_t volumeDl);
extern void sendSetConfig(int deviceIdx, uint8_t valveCount, uint8_t mode, uint8_t hasFlowSensor, uint16_t pulsesPerLiter);

// Список доступных команд - одна строка на команду, вынесена в отдельную
// функцию printHelp(), чтобы один и тот же текст использовался и по явной команде
// "help", и при нераспознанной команде (вместо двух расходящихся копий этого же
// текста, как было раньше).
//
//   list                          - показать известные устройства
//   open <idx> <valve> <sec>      - открыть клапан на N секунд (mode=0). В режиме 1 (эксклюзивный)
//                                    автоматически закроет другие клапаны этого устройства, в режиме 2
//                                    (независимый) - нет, см. IrrigationSpec.mode в GardenProtocol.h.
//   volume <idx> <valve> <liters> - открыть клапан на N литров (mode=1), та же оговорка про режимы
//   dose <idx> <valve> <liters>   - ТРЕТИЙ РЕЖИМ: точная дозировка объёма с точностью до 0.1 л (mode=2,
//                                    РЕАЛИЗОВАНА на узле, см. checkDosing() в flow_node.ino) - требует настроенного
//                                    датчика потока (config ... hasFlowSensor=1), всегда ФОРСИРОВАННО открывает
//                                    РОВНО ОДИН клапан, закрывая остальные, <liters> дробное (например "2.5")
//   close <idx> [valve]           - закрыть один конкретный клапан (если указан) или все сразу
//                                    (без valve) - работает одинаково во всех режимах
//   config <idx> <valves> <mode> <hasFlowSensor> <pulsesPerLiter>
//                                 - задать устройству количество каналов (1..5), режим
//                                    работы (1 - эксклюзивный, 2 - независимый, 3 -
//                                    дозирование, требует hasFlowSensor=1), наличие
//                                    датчика потока (0|1) и его разрешение (импульсов на
//                                    литр, 1..20000) - хранится НА САМОМ узле (EEPROM),
//                                    переживает его перезагрузку, в отличие от open/volume/dose/close
//   install <idx>                 - подтвердить устройство, защитить от вытеснения, сохранить в NVS
//   forget <idx>                  - удалить устройство (установленное или нет) из таблицы и NVS
//   rename <idx> <name>           - переименовать установленное устройство, сохранить в NVS
//   time                          - показать текущее время Хаба и статус синхронизации
//   status                        - показать частоту процессора, мощность передатчика, число
//                                    установленных устройств, состояние точки доступа, текущую температуру
//                                    и пороги включения/выключения вентилятора (вместе с текущим состоянием вентилятора)
//   help                          - показать этот список команд
void printHelp() {
    Serial.println("Доступные команды:");
    Serial.println("  list                          - показать известные устройства");
    Serial.println("  open <idx> <valve> <sec>      - открыть клапан на N секунд");
    Serial.println("  volume <idx> <valve> <liters> - открыть клапан на N литров (целых, не реализовано)");
    Serial.println("  dose <idx> <valve> <liters>   - точная дозировка, открывает РОВНО один клапан (требует датчик потока)");
    Serial.println("  close <idx> [valve]           - закрыть клапан (все, если valve не указан)");
    Serial.println("  config <idx> <valves> <mode> <hasFlowSensor> <pulsesPerLiter>");
    Serial.println("                                 - задать конфигурацию устройства (сохраняется на узле)");
    Serial.println("  install <idx>                 - подтвердить устройство, защитить от вытеснения");
    Serial.println("  forget <idx>                  - удалить устройство из таблицы и NVS");
    Serial.println("  rename <idx> <name>           - переименовать установленное устройство");
    Serial.println("  time                          - показать текущее время Хаба и статус синхронизации");
    Serial.println("  status                        - показать частоту CPU, мощность передатчика, число");
    Serial.println("                                   установленных устройств, точку доступа, температуру и пороги вентилятора");
    Serial.println("  help                          - показать этот список команд");
}
void handleSerialCommand(String line) {
    line.trim();
    if (line.length() == 0) return;

    int sp1 = line.indexOf(' ');
    String cmd = sp1 == -1 ? line : line.substring(0, sp1);

    if (cmd == "list") {
        deviceManager.printList();
    } else if (cmd == "open") {
        int idx, valve, sec;
        if (sscanf(line.c_str(), "open %d %d %d", &idx, &valve, &sec) == 3) {
            sendCommand(idx, valve, ACTION_OPEN, /*mode=*/0, /*duration_sec=*/sec, /*volume_l=*/0, /*volume_dl=*/0);
        } else {
            Serial.println("Использование: open <idx> <valve> <sec>");
        }
    } else if (cmd == "volume") {
        int idx, valve, liters;
        if (sscanf(line.c_str(), "volume %d %d %d", &idx, &valve, &liters) == 3) {
            sendCommand(idx, valve, ACTION_OPEN, /*mode=*/1, /*duration_sec=*/0, /*volume_l=*/liters, /*volume_dl=*/0);
        } else {
            Serial.println("Использование: volume <idx> <valve> <liters>");
        }
    } else if (cmd == "dose") {
        // Третий режим (mode=2) - точная дозировка с точностью до десятых литра -
        // <liters> разбирается как float (например "2.5"), тут же округляется до десятых
        // литра - точно так же, как handleApiCommand() в hub.ino делает это для POST /api/command.
        int idx, valve;
        float liters;
        if (sscanf(line.c_str(), "dose %d %d %f", &idx, &valve, &liters) == 3) {
            uint16_t volumeDl = (uint16_t) (liters * 10.0f + 0.5f);
            sendCommand(idx, valve, ACTION_OPEN, /*mode=*/2, /*duration_sec=*/0, /*volume_l=*/0, volumeDl);
        } else {
            Serial.println("Использование: dose <idx> <valve> <liters>");
        }
    } else if (cmd == "close") {
        // Второй аргумент опционален - сначала пробуем разобрать с ним и только
        // если не получилось - без него 0 = закрыть ВСЕ (см. target_valve в
        // GardenProtocol.h).
        int idx, valve;
        if (sscanf(line.c_str(), "close %d %d", &idx, &valve) == 2) {
            sendCommand(idx, valve, ACTION_CLOSE, /*mode=*/0, /*duration_sec=*/0, /*volume_l=*/0, /*volume_dl=*/0);
        } else if (sscanf(line.c_str(), "close %d", &idx) == 1) {
            sendCommand(idx, /*target_valve=*/0, ACTION_CLOSE, /*mode=*/0, /*duration_sec=*/0, /*volume_l=*/0, /*volume_dl=*/0);
        } else {
            Serial.println("Использование: close <idx> [valve]");
        }
    } else if (cmd == "config") {
        int idx, valves, mode, hasFlowSensor, pulsesPerLiter;
        if (sscanf(line.c_str(), "config %d %d %d %d %d", &idx, &valves, &mode, &hasFlowSensor, &pulsesPerLiter) == 5) {
            sendSetConfig(idx, (uint8_t) valves, (uint8_t) mode, (uint8_t) hasFlowSensor, (uint16_t) pulsesPerLiter);
        } else {
            Serial.println("Использование: config <idx> <valves 1..5> <mode 1|2|3> <hasFlowSensor 0|1> <pulsesPerLiter 1..20000>");
        }
    } else if (cmd == "install") {
        int idx;
        if (sscanf(line.c_str(), "install %d", &idx) == 1) {
            if (deviceManager.install(idx)) {
                Serial.printf("Устройство #%d установлено и защищено от вытеснения.\n", idx);
            } else {
                Serial.println("Нет такого устройства (см. 'list')");
            }
        } else {
            Serial.println("Использование: install <idx>");
        }
    } else if (cmd == "forget") {
        int idx;
        if (sscanf(line.c_str(), "forget %d", &idx) == 1) {
            if (deviceManager.forget(idx)) {
                Serial.printf("Устройство #%d удалено из таблицы и NVS.\n", idx);
            } else {
                Serial.println("Нет такого устройства (см. 'list')");
            }
        } else {
            Serial.println("Использование: forget <idx>");
        }
    } else if (cmd == "rename") {
        int idx;
        if (sscanf(line.c_str(), "rename %d", &idx) == 1) {
            // Имя - всё, что после второго пробела ("rename <idx> "), может
            // содержать пробелы - поэтому ищем вручную, а не через sscanf %s
            // (тот бы остановился на первом пробеле внутри имени).
            int firstSpace = line.indexOf(' ');
            int secondSpace = firstSpace == -1 ? -1 : line.indexOf(' ', firstSpace + 1);
            String name = secondSpace == -1 ? "" : line.substring(secondSpace + 1);
            name.trim();
            if (deviceManager.setName(idx, name.c_str())) {
                Serial.printf("Устройство #%d переименовано: \"%s\"\n", idx, name.c_str());
            } else {
                Serial.println("Нет такого устройства, либо оно ещё не установлено (см. 'list')");
            }
        } else {
            Serial.println("Использование: rename <idx> <название>");
        }
    } else if (cmd == "time") {
        // Смотреть результат синхронизации не только в момент самого
        // POST /api/settime (та строка быстро уходит вверх по Serial
        // Monitor), а по запросу в любой момент - тот же timeSynced и
        // currentTimeString(), что отдаёт и GET /api/status.
        if (timeSynced) {
            Serial.print("Время Хаба синхронизировано: ");
            Serial.println(currentTimeString());
        } else {
            Serial.println("Время Хаба ещё НЕ синхронизировано - ждём загрузки веб-страницы "
                            "в браузере или обнаружения часов DS3231 (см. PROTOCOL.md §12).");
        }
    } else if (cmd == "status") {
        // Считаем установленные устройства прямо здесь (а не через отдельный метод
        // DeviceManager) - поле devices[] публичное именно для таких разовых частых
        // чтений отдельных полей (см. большой комментарий в DeviceManager.h), заводить
        // геттер под одно такое обращение избыточно.
        int installedCount = 0;
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (deviceManager.devices[i] != nullptr && deviceManager.devices[i]->installed) installedCount++;
        }
        // WiFi.getTxPower() возвращает wifi_power_t (сырое число в четвертях дБм, см.
        // TX_POWER_OPTIONS/большой комментарий в hub.ino) - здесь просто делим на 4 для
        // читаемого вывода, отдельная функция форматирования под Serial не заводилась.
        Serial.println("--- Статус Хаба ---");
        Serial.printf("Частота процессора: %u МГц\n", (unsigned) getCpuFrequencyMhz());
        Serial.printf("Мощность передатчика: %.2f dBm\n", WiFi.getTxPower() / 4.0);
        Serial.printf("Установлено устройств: %d\n", installedCount);
        Serial.printf("Точка доступа: %s\n", apEnabled ? "ВКЛЮЧЕНА" : "ВЫКЛЮЧЕНА");
        Serial.printf("Текущая температура: %.1f °C\n", temperatureRead());
        Serial.printf("Температура включения вентилятора: %.1f °C\n", fanOnTempC);
        Serial.printf("Температура выключения вентилятора: %.1f °C\n", fanOffTempC);
        Serial.printf("Вентилятор: %s\n", fanOn ? "ВКЛЮЧЁН" : "выключен");
        Serial.println("-------------------");
    } else if (cmd == "help") {
        printHelp();
    } else {
        Serial.println("Неизвестная команда. Введите 'help' для списка доступных команд.");
    }
}
