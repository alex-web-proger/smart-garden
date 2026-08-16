# GardenNet

Общий протокол и переиспользуемая логика периферийного узла для проекта
Smart Garden (ESP-NOW, ESP8266/ESP32). Подробное описание самого
протокола (формат пакета, адресация, dedup, ретрансляция) — в
[`../../PROTOCOL.md`](../../PROTOCOL.md); этот файл — про то, как
пользоваться библиотекой при написании нового скетча.

## Что внутри

```
GardenNet/
├── library.properties
└── src/
    ├── GardenProtocol.h   - формат пакета, типы устройств/сообщений,
    │                        дедупликация (isNewerPacketId)
    ├── GardenNode.h        - класс переиспользуемой логики узла
    └── GardenNode.cpp
```

**`GardenProtocol.h`** — то, что нужно ЛЮБОМУ устройству сети, включая
Хаб и ретранслятор: структура `UniversalPacket`, перечисления
`DeviceType`/`MsgType`, структуры payload для полива
(`IrrigationSpec`/`Telemetry`/`Command`), `AckData`, константа
`ESPNOW_CHANNEL`, функция `isNewerPacketId()`.

**`GardenNode`** — класс нужен только периферийным узлам (не Хабу и не
ретранслятору). Берёт на себя: подготовку заголовка пакета, адресацию
(личный MAC + широковещательный `FF..FF`), приём и дедупликацию
входящих пакетов, сброс dedup по announce Хаба о (пере)загрузке,
случайный джиттер интервалов `MSG_CONFIG`/`MSG_TELEMETRY`, watchdog по
таймауту связи, автоматическую отправку `MSG_ACK` на команды.

Класс **не занимается**:
- **Транспортом** (`esp_now_init`/`esp_now_send`/регистрация
  recv-колбэка) — отличается между ESP8266 (`<espnow.h>`) и ESP32
  (`<esp_now.h>`), поэтому остаётся в скетче.
- **Доменом устройства** — что конкретно лежит в payload
  `CONFIG`/`TELEMETRY`, что делать с принятой командой, что делать при
  потере связи — тоже остаётся в скетче, через колбэки.

## Установка

Библиотека должна лежать в глобальной папке библиотек скетчбука Arduino
IDE, а не внутри `smart_garden/`, иначе IDE не найдёт её из других
скетчей:

```
mv ~/Arduino/smart_garden/libraries/GardenNet ~/Arduino/libraries/
```

(На Windows аналогично — `C:\Users\<user>\Documents\Arduino\smart_garden\libraries\GardenNet`
в `C:\Users\<user>\Documents\Arduino\libraries\`.) После переноса
перезапустите Arduino IDE, чтобы она перечитала список библиотек.

## Порядок использования в новом скетче узла

Ниже — последовательность вызовов, которую должен реализовать любой
периферийный узел (полив, освещение, датчик погоды и т.д.). Все шаги,
кроме транспортных, одинаковы независимо от домена устройства.

### 1. Подключить заголовки

```cpp
#include <ESP8266WiFi.h>   // или WiFi.h + esp_now.h для ESP32
#include <espnow.h>         // или esp_now.h для ESP32
#include <GardenProtocol.h>
#include <GardenNode.h>
```

### 2. Завести транспортную функцию отправки

Библиотека не знает, как именно отправлять байты — сигнатура
`esp_now_send` отличается на ESP8266/ESP32. Нужно обернуть её в функцию
вида `bool(const uint8_t*, size_t)`:

```cpp
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

bool sendRaw(const uint8_t *data, size_t len) {
    return esp_now_send(broadcastAddress, (uint8_t *) data, len) == 0;
}
```

### 3. Реализовать колбэки домена

Четыре функции — под конкретное устройство. Любую можно оставить
`nullptr` в `setCallbacks()`, если она не нужна (например, устройство
без входящих команд может не реализовывать `onCommand`).

```cpp
// Заполнить payload MSG_CONFIG ("паспорт" устройства). Заголовок пакета
// уже готов на момент вызова.
void fillConfig(UniversalPacket &pkt) {
    pkt.payload.irrigation.spec.valve_count = VALVE_COUNT;
    pkt.payload.irrigation.spec.has_flow_sensor = HAS_FLOW_SENSOR;
}

// Заполнить payload MSG_TELEMETRY (текущие показания).
void fillTelemetry(UniversalPacket &pkt) {
    pkt.payload.irrigation.telemetry.active_valve = activeValve;
    // ...
}

// Обработать входящую MSG_COMMAND. Пакет уже прошёл проверку адресации
// и дедупликации. Возвращает статус для автоматически отправляемого
// MSG_ACK: 0 - принято, 1 - отклонено.
uint8_t onCommand(const UniversalPacket &pkt) {
    IrrigationCommand cmd = pkt.payload.irrigation.command;
    // ...применить команду...
    return 0;
}

// Вызывается, если watchdog сработал (armWatchdog() был вызван, но
// валидных пакетов от Хаба не было дольше заданного таймаута).
void onWatchdogTimeout() {
    // ...например, закрыть клапан/выключить свет...
}
```

### 4. Транспортный recv-колбэк

Платформенная сигнатура остаётся как есть, но внутри — просто передача
уже скопированного пакета в библиотеку:

```cpp
void onDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
    if (len != sizeof(UniversalPacket)) return;
    UniversalPacket pkt;
    memcpy(&pkt, incomingData, sizeof(pkt));
    node.handleIncoming(pkt);
}
```

### 5. `setup()`: транспорт, затем `GardenNode`

Порядок важен: сначала должен быть готов Wi-Fi/ESP-NOW (нужен MAC и
рабочий `esp_now_send`), потом инициализируется `GardenNode`.

```cpp
GardenNode node;
uint8_t myMac[6];

void setup() {
    Serial.begin(115200);

    // ...своя инициализация железа (пины и т.п.)...

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.macAddress(myMac);

    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO); // только ESP8266
    esp_now_register_recv_cb(onDataRecv);
    esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);

    // Теперь можно инициализировать GardenNode:
    node.begin(TYPE_IRRIGATION, myMac, sendRaw);
    node.setCallbacks(fillConfig, fillTelemetry, onCommand, onWatchdogTimeout);
    node.setTiming(
        /*telemetryIntervalMs=*/10000, /*telemetryJitterMs=*/2000,
        /*configIntervalMs=*/3600000,  /*configJitterMs=*/300000,
        /*watchdogTimeoutMs=*/30000
    );
    node.sendConfig(); // заявить о себе сразу при включении
}
```

### 6. `loop()`: одна строка

```cpp
void loop() {
    node.loop(); // обслуживает таймеры CONFIG/TELEMETRY и watchdog
}
```

### 7. Управление watchdog из домена

Когда устройство переходит в состояние, которое нужно автоматически
сбросить при потере связи (например, открылся клапан) — вызвать
`node.armWatchdog()`. Когда состояние снято вручную (клапан закрыли
командой) — `node.disarmWatchdog()`. Обычно это делается прямо внутри
`onCommand`:

```cpp
uint8_t onCommand(const UniversalPacket &pkt) {
    // ...
    if (activeValve != 0) node.armWatchdog();
    else node.disarmWatchdog();
    return 0;
}
```

## Событийные устройства (например, кнопка)

Шаги 5–6 описывают периодическую модель (телеметрия по таймеру).
Если устройство должно реагировать на дискретное событие немедленно
(кнопка, герконовый датчик и т.п.), есть отдельный метод:

```cpp
void onButtonPressed() {
    pressCount++;           // монотонный счётчик, см. ниже
    node.sendTelemetryNow(); // не ждём следующего тика loop()
}
```

`sendTelemetryNow()` шлёт `MSG_TELEMETRY` немедленно, не трогая обычный
периодический таймер из `loop()` (он всё равно сработает по расписанию).

**Важное ограничение**: у `MSG_TELEMETRY` нет ACK и нет автоповтора. Для
обычной периодической телеметрии это не проблема — потерянный пакет
восстановится следующим тиком. Для дискретного события (одно нажатие кнопки)
потеря одного-единственного пакета невосстановима — поэтому в payload
стоит класть не булево "было нажатие", а монотонно растущий счётчик
нажатий — тот же приём, что и `packet_id` в самом протоколе: даже пропавший
отдельный пакет Хаб увидит в следующем сообщении (внеочередном или очередном),
что счётчик вырос — и поймёт, что событие было, даже не зная точно, сколько
раз и когда именно.

На `MSG_CONFIG` для этого опираться НЕ стоит: `MSG_CONFIG` в протоколе имеет
зарезервированный смысл — это сигнал "устройство (пере)загрузилось",
по которому Хаб сбрасывает dedup (см. `PROTOCOL.md` §4.3). Использовать его как
общий "отправь что-нибудь прямо сейчас" — злоупотребление смысла; для
внеочередной отправки события есть специально `sendTelemetryNow()`.

## Полный пример

Готовый рабочий пример — `modules/flow_node/flow_node.ino` в проекте.
Он и есть эталонная реализация всех шагов выше для узла полива.

## Расширение протокола под новый тип устройства

Сам класс `GardenNode` менять не нужно — он не знает про домены. Чтобы
добавить, например, освещение:

1. В `GardenProtocol.h`: добавить `TYPE_LIGHTING` в `enum DeviceType`,
   завести структуры `LightingSpec`/`LightingTelemetry`/`LightingCommand`,
   добавить их как ещё один `arm` в `union payload` (рядом с
   `irrigation`).
2. Написать `modules/light_node/light_node.ino` по шагам 1–7 выше, с
   `TYPE_LIGHTING` в `node.begin()` и своими реализациями четырёх
   колбэков.
3. На Хабе: добавить `handleLightingPayload()` и `case TYPE_LIGHTING`
   в диспетчер `onDataRecv()` (сейчас там уже есть заготовка-комментарий
   под это).

## Версия и платформы

`library.properties`: `architectures=esp8266,esp32` — библиотека
написана на чистом C++/Arduino API (`Serial`, `millis()`,
`random()`/`randomSeed()`, `memcpy`/`memcmp`), без вызовов, специфичных
для одной платформы, поэтому одинаково собирается под обе.
