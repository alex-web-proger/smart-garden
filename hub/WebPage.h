#pragma once

// HTML-страница веб-интерфейса Хаба. Вынесена из hub.ino в отдельный
// файл, чтобы не засорять логику огромным raw-string-литералом - Arduino
// IDE покажет этот файл отдельной вкладкой в том же окне редактора при
// открытии hub.ino (стандартный механизм многофайловых скетчей, файл
// лежит в той же папке).
//
// Отдаётся как есть, никакого шаблонизатора на стороне Хаба - данные
// устройств страница получает сама через /api/devices (fetch), см.
// hub.ino и PROTOCOL.md §10/§11.
//
// ПРЕДСТАВЛЕНИЕ УСТРОЙСТВ: карточки в адаптивной сетке (а не таблица) -
// каждая карточка залита цветом по статусу (установлено/кандидат),
// показывает индикатор связи, тип и условное имя устройства. Подробности
// (MAC, точный статус, управление клапанами, install/forget) вынесены в
// модальное окно по кнопке ⚙ на карточке - на самой карточке места под
// это нет, да и не нужно для беглого взгляда на состояние сети.
//
// КОНФИГУРАЦИЯ МОДУЛЯ: /api/devices отдаёт реальные valveCount/mode/hasFlowSensor/
// flowPulsesPerLiter/totalWaterUsed узла (см. IrrigationDevice в hub/IrrigationDevice.h) - раздел
// "Управление" в модальном окне (сворачиваемый, по умолчанию закрыт, такой же
// <details>/<summary>, как и у "Конфигурации модуля" ниже) рисует РОВНО столько клапанов, сколько сейчас
// сконфигурировано на самом узле (0, пока первый MSG_CONFIG от него ещё не пришёл -
// тогда секция клапанов временно пуста), а при hasFlowSensor==true ещё и накопленный
// объём воды с момента последнего сброса узла. Открытие клапана поддерживает ТРИ
// режима, но ВЫБИРАЮТСЯ ОНИ В "КОНФИГУРАЦИИ МОДУЛЯ" НИЖЕ (<select id="modal-cfg-mode">,
// персистентный режим устройства, сохраняется на самом узле) - А НЕ отдельным
// переключателем в самом "Управлении": 1 — эксклюзивный (открытие любого клапана
// закрывает остальные), 2 — независимый (каждый клапан отдельно), 3 — ТРЕТИЙ: точная
// дозировка с точностью до 0.1 л (РЕАЛИЗОВАНА на узле, см. checkDosing() в flow_node.ino),
// требует hasFlowSensor==1 в ТОМ ЖЕ запросе конфигурации (иначе узел отклонит, см.
// onSetConfig() в flow_node.ino) и ВСЕГДА открывает РОВНО один клапан, форсированно
// закрывая остальные (см. onCommand() в flow_node.ino). Когда устройство сконфигурировано на
// mode=3, "Управление" САМО показывает поле "Объём, л" вместо длительности и отправляет
// команду с IrrigationCommand.mode=2 (см. buildIrrigationTypeSpecific()/valveButtonClick() ниже) -
// выбора в самом "Управлении" НЕТ. Легаси-режим по целым литрам (IrrigationCommand.mode=1) не
// реализован на узле и из веб-интерфейса не вызывается. Перед ним, выше - также
// сворачиваемая (по умолчанию закрыта)
// секция "Настройка" (id="modal-settings-section") - по одному блоку на каждый клапан (в
// пределах текущего valveCount) с тремя полями: периодичность полива (выпадающий список
// "Раз в N дней", 1..7), объём за один полив в литрах (до десятых) и чекбокс "Автополив
// включён". В ОТЛИЧИЕ от "Конфигурации модуля" ниже, эти настройки ПОЛНОСТЬЮ локальны для
// Хаба и НЕ отправляются узлу по ESP-NOW вообще - только сохраняются в NVS САМОГО ХАБА
// (POST /api/setValveSchedule -> DeviceManager::setValveSchedule(), см. hub.ino/DeviceManager.h/.cpp
// и большой комментарий у ValveSchedule в hub/IrrigationDevice.h) - сам автополив по расписанию
// (то есть фактическая периодическая отправка MSG_COMMAND) пока НЕ РЕАЛИЗОВАН, эти поля пока
// только хранятся и готовы к будущему планировщику. У каждого клапана своя кнопка "Сохранить" -
// изменения по разным клапанам применяются независимо. Тут же, ниже самого
// "Управления" - сектор "Конфигурация
// модуля" (также сворачиваемый, по умолчанию закрыт, <details>/<summary>) с количеством
// каналов (1-5), режимом работы (1/2), наличием датчика потока (чекбокс) и его
// разрешением (импульсов на литр) - изменения уходят на
// Хаб через POST /api/setConfig, который отправляет узлу MSG_SET_CONFIG; узел сам
// валидирует, применяет и СОХРАНЯЕТ конфигурацию у себя (EEPROM, переживает его
// перезагрузку) - см. GardenProtocol.h/PROTOCOL.md §3.2 и onSetConfig() в
// flow_node.ino.
//
// САМ ХАБ (не узел): температура кристалла ESP32, мощность
// передатчика Wi-Fi/ESP-NOW и частота процессора, все три read+write через /api/status
// (GET) и /api/setTxPower, /api/setCpuFreq (POST) — см. handleApiStatus()/
// handleApiSetTxPower()/handleApiSetCpuFreq() в hub.ino. Мощность передатчика и частота
// процессора сохраняются в NVS и применяются автоматически при каждом сбросе Хаба (см.
// loadRadioSettingsFromNVS() в hub.ino) — температура, разумеется, не настраивается и
// нигде не сохраняется, это просто текущее показание датчика.
// Температура показывается СРАЗУ в двух местах - короткой строкой на главной странице
// (под "Время после сброса") и ещё раз в модальном окне "Настройки Хаба" - у каждой свой
// DOM-id (temp-status/modal-temp-value), обновляются они одним refreshStatus() в скрипте.
// Сама модалка открывается шестерёнкой рядом с заголовком страницы (см. openHubModal() в
// скрипте) - мощность/частота настраиваются там же, простыми строками без
// схлопывающихся секций и без поясняющих подписей - это редкие настройки, но, раз уж
// оператор открыл модалку, лучше видеть их сразу целиком, без лишнего клика по каждой.
// Там же, ниже - два порога УПРАВЛЕНИЯ ВЕНТИЛЯТОРОМ (температура его
// включения и температура выключения, см. большой комментарий у
// fanOnTempC/fanOffTempC в hub.ino) - в отличие от мощности/частоты это <input>, а не
// <select> (значения произвольные, не из фиксированного набора), и заполняются они из
// /api/status только ОДИН РАЗ за всю жизнь страницы (см. fanInputsInitialized в
// скрипте), а не на каждый тик - иначе набираемое оператором значение стиралось бы
// прямо во время ввода. Обе настройки сохраняются одной кнопкой (см.
// saveFanThresholds() в скрипте, POST /api/setFanThresholds) и в NVS на Хабе.
// Если вентилятор охлаждения сейчас включён - об этом также сказано прямо в строке
// температуры на главной странице (fanOn из /api/status, см. refreshStatus()).
//
// НАЗВАНИЕ УСТРОЙСТВА: карточка показывает его вместо "Узел #idx", если
// оно задано - редактируется в модальном окне (поле "Название" + кнопка
// 💾), доступно ТОЛЬКО для установленных устройств (ограничение
// проверяется и на стороне Хаба, см. DeviceManager::setName()) и
// сохраняется в NVS - переживает перезагрузку Хаба. Индекс (#idx) при
// этом всё равно показывается рядом на карточке - для сверки с выводом
// Serial-команды list, где своих названий нет.

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Garden — Хаб</title>
<style>
  * { box-sizing: border-box; }
  /* Глобальное увеличение шрифта в 1.15 раза - все остальные размеры в этом файле
     заданы в em/% (не px), то есть каскадно отсчитываются от размера шрифта
     родителя, а body font-size сам не задан явно и наследуется от html - поэтому
     достаточно одной этой строки, чтобы пропорционально увеличить ВСЕ шрифты
     страницы (и элементы, зависящие от em, вроде padding у кнопок) разом, вместо
     правки каждого правила ниже по отдельности. */
  html { font-size: 115%; }
  body { font-family: -apple-system, sans-serif; margin: 16px; background:#f6f6f2; color:#222; }
  h1 { font-size: 1.2em; margin-bottom: 4px; }
  /* Шестеренка открытия модалки "Настройки Хаба" прямо в заголовке (inline внутри <h1>,
     не position:absolute, как у .gear-btn на карточке устройства - там нужен именно отступ
     от края карточки, здесь же просто иконка рядом с текстом). */
  .header-gear-btn { display:inline-flex; align-items:center; justify-content:center; vertical-align:middle;
                      margin-left:8px; border:none; background:none; cursor:pointer; padding:0; box-shadow:none; }
  .header-gear-btn svg { width:22px; height:22px; fill:#555; }
  .header-gear-btn:active svg { fill:#222; }
  #uptime-status { font-size: 0.85em; margin-bottom: 4px; color:#666; }
  #temp-status { font-size: 0.85em; margin-bottom: 16px; color:#666; }
  .temp-hot { color:#c0392b; font-weight:600; }

  .grid { display:grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 12px; }
  .empty { padding: 30px; text-align:center; color:#999; background:#fff; border-radius:10px; }

  .device-card { position:relative; background:#fff; border-radius:10px; padding:14px 50px 14px 14px;
                 border:2px solid transparent; box-shadow:0 1px 3px rgba(0,0,0,0.07); cursor:default; }
  .device-card.status-installed { background:#e9f6e4; border-color:#bfe0b2; }
  .device-card.status-candidate { background:#fff3e0; border-color:#f0c98a; }

  .gear-btn { position:absolute; top:8px; right:8px; width:auto; height:auto; border-radius:0;
              border:none; background:none; cursor:pointer; padding:0;
              display:flex; align-items:center; justify-content:center;
              box-shadow:none; }
  .gear-btn svg { width:35px; height:35px; fill:#555; }
  .gear-btn:active svg { fill:#222; }

  .conn-row { display:flex; align-items:center; gap:6px; margin-bottom:8px; }
  .conn-dot { width:9px; height:9px; border-radius:50%; flex:0 0 auto; }
  .conn-dot.conn-ok { background:#2a7d2a; }
  .conn-dot.conn-warn { background:#d8a400; }
  .conn-dot.conn-bad { background:#c0392b; }
  .device-type { font-size:0.72em; text-transform:uppercase; letter-spacing:0.04em; color:#666; }

  .device-name { font-weight:600; font-size:1.05em; margin-bottom:2px; }
  .device-sub { font-size:0.8em; color:#777; margin-bottom:10px; }

  .status-pill { display:inline-block; font-size:0.75em; padding:3px 10px; border-radius:20px; font-weight:600; color:#fff; }
  .status-pill.installed { background:#3a8f2e; }
  .status-pill.candidate { background:#d8860a; }

  /* --- Модальное окно --- */
  .modal-backdrop { position:fixed; inset:0; background:rgba(0,0,0,0.45); display:none;
                     align-items:center; justify-content:center; padding:16px; z-index:50; }
  .modal-backdrop.open { display:flex; }
  .modal { background:#fff; border-radius:12px; padding:20px; width:100%; max-width:420px;
           max-height:90vh; overflow-y:auto; }
  .modal-header { display:flex; justify-content:space-between; align-items:flex-start; margin-bottom:4px; gap:10px; }
  .modal-header h2 { margin:0; font-size:1.1em; }
  .modal-close { border:none; background:none; font-size:1.375em; line-height:1; cursor:pointer; color:#888; padding:0 4px; }

  .modal-field { font-size:0.85em; margin-bottom:6px; color:#444; display:flex; justify-content:space-between; gap:10px; align-items:center; }
  .modal-field b { color:#222; font-weight:600; }
  .modal-field select { padding:4px 6px; border:1px solid #ccc; border-radius:4px; font-size:0.85em; }
  .modal-field input[type=number] { width:70px; padding:4px 6px; border:1px solid #ccc; border-radius:4px; font-size:0.85em; }
  .name-edit { display:flex; gap:6px; align-items:center; }
  .name-edit input { width:130px; padding:4px 6px; border:1px solid #ccc; border-radius:4px; font-size:0.85em; }
  .name-edit button { padding:4px 9px; font-size:0.9em; }

  .modal-actions { margin:14px 0 4px; display:flex; gap:8px; flex-wrap:wrap; }

  .valve-section { margin-top:14px; border-top:1px solid #eee; padding-top:12px; }
  .valve-section summary { font-size:0.9em; margin:0 0 8px; cursor:pointer; font-weight:600; }
  .valve-section[open] summary { margin-bottom:8px; }

  .settings-section { margin-top:14px; border-top:1px solid #eee; padding-top:12px; }
  .settings-section summary { font-size:0.9em; margin:0 0 8px; cursor:pointer; font-weight:600; }
  .settings-section[open] summary { margin-bottom:8px; }
  /* Один блок настроек автополива на клапан внутри секции "Настройка" - тот же принцип
     разделительных полосок между блоками, что и у .valve-row (см. ниже), но здесь блок
     МНОГОСТРОЧНЫЙ (три поля + кнопка "Сохранить"), а не одна строка. */
  .schedule-row { padding:8px 0 4px; border-bottom:1px solid #f2f2f2; }
  .schedule-row:last-child { border-bottom:none; }
  .schedule-row-title { font-weight:600; font-size:0.85em; margin-bottom:6px; }
  .schedule-row .config-row { margin-bottom:6px; }
  .schedule-row button.apply-btn { margin-top:2px; }
  .duration-row { font-size:0.85em; margin-bottom:10px; display:flex; align-items:center; gap:8px; }
  .duration-row input { width:60px; padding:4px; border:1px solid #ccc; border-radius:4px; }
  .valve-row { display:flex; align-items:center; justify-content:space-between; gap:8px;
               padding:7px 0; border-bottom:1px solid #f2f2f2; }
  .valve-row:last-child { border-bottom:none; }
  .valve-state { font-size:0.85em; flex:1; text-align:right; padding-right:8px; }
  .valve-state.open { color:#2a7d2a; font-weight:600; }
  .valve-state.closed { color:#999; }

  .config-section { margin-top:14px; border-top:1px solid #eee; padding-top:12px; }
  .config-section summary { font-size:0.9em; margin:0 0 8px; cursor:pointer; font-weight:600; }
  .config-section[open] summary { margin-bottom:8px; }
  .config-row { font-size:0.85em; margin-bottom:10px; display:flex; align-items:center; justify-content:space-between; gap:8px; }
  .config-row input, .config-row select { padding:4px 6px; border:1px solid #ccc; border-radius:4px; font-size:0.85em; }
  .config-row input[type=number] { width:55px; }
  .config-hint { font-size:0.75em; color:#999; margin:-4px 0 10px; }
  button.apply-btn { background:#2a6fa3; }

  /* Кратковременная обратная связь на кнопках сохранения (имя, конфигурация модуля) -
     см. flashButton() в скрипте: меняем цвет/текст кнопки на пару секунд после
     ответа сервера, чтобы было явно видно, применились изменения или нет. */
  button.btn-flash-ok { background:#2a7d2a !important; }
  button.btn-flash-err { background:#c0392b !important; }

  button { padding: 8px 12px; border:none; border-radius:6px; background:#4a7c3f; color:white; font-size:0.85em; cursor:pointer; }
  button.close-btn { background:#a33; }
  button.install-btn { background:#2a6fa3; }
  button.forget-btn { background:#888; }
</style>
</head>
<body>
<h1>Smart Garden — Хаб
  <button class="header-gear-btn" onclick="openHubModal()" title="Настройки Хаба">
    <svg viewBox="0 0 24 24"><path d="M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58c.18-.14.23-.41.12-.61l-1.92-3.32c-.12-.22-.37-.29-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54c-.04-.24-.24-.41-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.63c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58c-.18.14-.23.41-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z"/></svg>
  </button>
</h1>
<div id="uptime-status">Время после сброса: —</div>
<div id="temp-status">Температура Хаба: —</div>

<div class="grid" id="devices-grid"><div class="empty">Загрузка...</div></div>

<!-- Модальное окно с подробностями устройства - одно на страницу,
     переиспользуется для любого устройства (см. openModal() в скрипте) -->
<div class="modal-backdrop" id="modal-backdrop">
  <div class="modal">
    <div class="modal-header">
      <h2 id="modal-title">Устройство</h2>
      <button class="modal-close" onclick="closeModal()">×</button>
    </div>
    <div class="modal-field"><span>MAC-адрес</span><b id="modal-mac">—</b></div>
    <div class="modal-field"><span>Связь</span><b id="modal-conn">—</b></div>
    <div class="modal-field"><span>Статус</span><span id="modal-status" class="status-pill">—</span></div>

    <!-- Действие "Установить" - показывается ТОЛЬКО для кандидата
         (installed=false), см. updateModal() в скрипте. До установки больше
         ничего показывать нечего - переименование/управление имеют смысл только
         после того, как оператор подтвердил устройство. -->
    <div class="modal-actions" id="modal-install-action"></div>

    <!-- Всё, что ниже, имеет смысл ТОЛЬКО для УЖЕ УСТАНОВЛЕННОГО устройства
         (переименование, удаление, управление клапанами и т.п. - всё, что специфично
         для модуля или требует уже подтверждённого устройства) - кандидат сначала
         должен быть установлен. Целиком скрыто/показано через display в
         updateModal() - один тоггл, а не отдельный на каждый блок, как было раньше. -->
    <div id="modal-installed-section" style="display:none;">
      <!-- Сам 'input' создан ОДИН РАЗ навсегда (не пересоздаётся каждый раз) - его
           значение выставляется ТОЛЬКО при открытии модалки (openModal()), а не на
           каждый фоновый refresh() - иначе набранное пользователем значение
           стиралось бы каждые 2 сек (та же причина, что и у #modal-duration ниже). -->
      <div class="modal-field">
        <span>Название</span>
        <span class="name-edit">
          <input type="text" id="modal-name-input" maxlength="23" placeholder="без названия">
          <button onclick="saveDeviceName(this)" title="Сохранить название">💾</button>
        </span>
      </div>

      <div class="modal-actions" id="modal-forget-action"></div>

      <div id="modal-type-specific"></div>
    </div>
  </div>
</div>

<!-- Модальное окно с настройками САМОГО ХАБА (не узла!) - температура кристалла,
     мощность передатчика Wi-Fi/ESP-NOW и частота процессора - отдельная от
     модалки устройства выше (свой backdrop, свой openHubModal()/closeHubModal() в
     скрипте) - открывается шестеренкой в заголовке страницы. Температура/частота/
     мощность - простые строки (.modal-field, тот же паттерн, что и у MAC/Связь/Статус в
     модалке устройства) - без схлопывающихся секций и без поясняющих подписей - настройки
     редкие, но всегда видны целиком сразу, без лишнего клика. Списки в <select> строятся из
     TX_POWER_OPTIONS/CPU_FREQ_OPTIONS в скрипте - тот же набор значений, что и в hub.ino
     (TX_POWER_OPTIONS/CPU_FREQ_OPTIONS там) - дублируется вручную, потому что это JS, а не C++,
     общего enum'а между ними нет (та же ситуация, что с ACTION_OPEN/ACTION_CLOSE выше в
     valveButtonClick()). Температура здесь - второй, отдельный от главной страницы, DOM-элемент
     (id="modal-temp-value") - у главной страницы свой (id="temp-status"), оба обновляются
     одним refreshStatus() независимо друг от друга (id не могут повторяться на странице). Ниже
     этих трёх строк - ещё два поля, УЖЕ С <input>, а не <select> - пороги ВКЛЮЧЕНИЯ/
     ВЫКЛЮЧЕНИЯ ВЕНТИЛЯТОРА, см. их отдельный комментарий ниже у самих полей. -->
<div class="modal-backdrop" id="hub-modal-backdrop">
  <div class="modal">
    <div class="modal-header">
      <h2>Настройки Хаба</h2>
      <button class="modal-close" onclick="closeHubModal()">×</button>
    </div>
    <div class="modal-field"><span>Температура Хаба</span><b id="modal-temp-value">—</b></div>
    <div class="modal-field">
      <span>Частота процессора</span>
      <select id="cpu-freq-select" onchange="setCpuFreq(this)"></select>
    </div>
    <div class="modal-field">
      <span>Мощность передатчика</span>
      <select id="tx-power-select" onchange="setTxPower(this)"></select>
    </div>

    <!-- Управление вентилятором охлаждения (см. большой комментарий у fanOnTempC/fanOffTempC в hub.ino) - два порога,
         каждый своим <input type=number> + общая кнопка "Сохранить" ниже (оба значения валидны
         ТОЛЬКО ВМЕСТЕ, отдельной кнопки на каждое поле нет - см. saveFanThresholds() в
         скрипте). Значения в эти поля проставляются ТОЛЬКО ОДИН РАЗ (см.
         fanInputsInitialized в refreshStatus()) - в отличие от <select> частоты/мощности
         выше, это <input> - перезаписывать его каждые 2 сек значило бы стирать набираемое
         оператором значение прямо во время ввода (та же причина, что и у #modal-duration в
         модалке устройства). -->
    <div class="modal-field">
      <span>Температура включения вентилятора</span>
      <input type="number" id="fan-on-input" step="0.5" min="40" max="120">
    </div>
    <div class="modal-field">
      <span>Температура выключения вентилятора</span>
      <input type="number" id="fan-off-input" step="0.5" min="40" max="120">
    </div>
    <div style="text-align:right; margin-top:8px;">
      <button class="apply-btn" onclick="saveFanThresholds(this)">Сохранить</button>
    </div>
  </div>
</div>

<script>
// idx -> { el: <DOM-карточка>, installed: bool|null } - хранится между
// вызовами refresh(), чтобы не пересоздавать DOM-узлы карточек на
// каждый тик автообновления (та же идея, что раньше была для строк
// таблицы).
let cardsByIdx = {};

// idx -> последние полученные данные устройства - нужно, чтобы
// открыть/обновить модальное окно без отдельного похода в сеть.
let devicesByIdx = {};

// idx открытого сейчас модального окна, или null - пока оно открыто,
// его данные обновляются вместе с обычным refresh() (кроме поля ввода
// длительности - его руками никто не трогает, см. updateModal()).
let openModalIdx = null;

// Для какого idx/valveCount построена секция клапанов+конфигурации - см.
// комментарий у нужной пересборки ниже (updateModal()).
let typeSpecificBuiltFor = null;

// Аналог openModalIdx/closeModal() выше, но для модалки "Настройки Хаба" - там
// нет своего idx (она одна на всю страницу, про сам Хаб, а не про конкретное
// устройство), поэтому достаточно простого bool. Содержимое обновляется тем
// же refreshStatus(), что и раньше, независимо от того, открыта модалка или нет (элементы
// просто скрыты через display:none у затемнённого фона, это не мешает им обновляться в
// фоне).
let hubModalOpen = false;

// Поля порогов вентилятора (#fan-on-input/#fan-off-input) заполняются из
// /api/status только ОДИН РАЗ - см. использование ниже в refreshStatus() и
// комментарий у самих полей в HTML выше.
let fanInputsInitialized = false;

function openHubModal() {
  hubModalOpen = true;
  document.getElementById('hub-modal-backdrop').classList.add('open');
}

function closeHubModal() {
  hubModalOpen = false;
  document.getElementById('hub-modal-backdrop').classList.remove('open');
}

function connInfo(agoSec) {
  // Пороги ориентируются на типичный интервал телеметрии (см.
  // PROTOCOL.md) - раз в ~10 сек с некоторым джиттером.
  if (agoSec < 30) return { cls: 'conn-ok' };
  if (agoSec < 120) return { cls: 'conn-warn' };
  return { cls: 'conn-bad' };
}

function typeText(type) {
  return type === 1 ? 'Полив' : ('Тип ' + type);
}

function createCard(d) {
  const el = document.createElement('div');
  el.className = 'device-card';
  el.innerHTML =
    '<button class="gear-btn" title="Подробнее">' +
      '<svg viewBox="0 0 24 24"><path d="M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58c.18-.14.23-.41.12-.61l-1.92-3.32c-.12-.22-.37-.29-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54c-.04-.24-.24-.41-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.63c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58c-.18.14-.23.41-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z"/></svg>' +
    '</button>' +
    '<div class="conn-row"><span class="conn-dot"></span><span class="device-type"></span></div>' +
    '<div class="device-name"></div>' +
    '<div class="device-sub"></div>' +
    '<div class="status-pill"></div>';
  el.querySelector('.gear-btn').addEventListener('click', () => openModal(d.idx));
  return el;
}

function updateCard(el, d) {
  el.className = 'device-card ' + (d.installed ? 'status-installed' : 'status-candidate');

  const conn = connInfo(d.agoSec);
  el.querySelector('.conn-dot').className = 'conn-dot ' + conn.cls;
  el.querySelector('.device-type').textContent = typeText(d.type);
  // Название, если оператор его задал (см. saveDeviceName()) - иначе
  // стандартный вид по индексу, как и раньше.
  el.querySelector('.device-name').textContent = (d.name && d.name.length > 0) ? d.name : ('Узел #' + d.idx);
  // Индекс показываем всегда, даже когда есть своё название - помогает
  // сопоставить с выводом Serial-команды 'list' (там имён нет, только индексы).
  el.querySelector('.device-sub').textContent = d.agoSec + ' с назад · #' + d.idx;

  const pill = el.querySelector('.status-pill');
  pill.textContent = d.installed ? 'Установлено' : 'Кандидат';
  pill.className = 'status-pill ' + (d.installed ? 'installed' : 'candidate');
}

async function refresh() {
  try {
    const res = await fetch('/api/devices');
    const devices = await res.json();
    const grid = document.getElementById('devices-grid');

    if (devices.length === 0) {
      grid.innerHTML = '<div class="empty">Устройств пока не видно</div>';
      cardsByIdx = {};
      devicesByIdx = {};
      if (openModalIdx !== null) closeModal();
      return;
    }

    // Перед первым реальным наполнением убираем плейсхолдер
    // ("Загрузка..."/"Устройств пока не видно").
    if (Object.keys(cardsByIdx).length === 0) {
      grid.innerHTML = '';
    }

    devicesByIdx = {};
    const seenIdx = new Set();

    devices.forEach(d => {
      devicesByIdx[d.idx] = d;
      seenIdx.add(d.idx);

      let card = cardsByIdx[d.idx];
      if (!card) {
        card = createCard(d);
        grid.appendChild(card);
        cardsByIdx[d.idx] = card;
      }
      updateCard(card, d);
    });

    // Устройство пропало из ответа (например, кто-то его забыл через
    // другой клиент) - убираем его карточку.
    Object.keys(cardsByIdx).forEach(function (idxStr) {
      const idx = Number(idxStr);
      if (!seenIdx.has(idx)) {
        cardsByIdx[idx].remove();
        delete cardsByIdx[idx];
      }
    });

    // Модальное окно открыто на устройстве, которое либо пропало, либо
    // обновилось - в первом случае закрываем, во втором - освежаем
    // содержимое (кроме поля ввода длительности, см. updateModal()).
    if (openModalIdx !== null) {
      if (devicesByIdx[openModalIdx]) {
        updateModal(devicesByIdx[openModalIdx]);
      } else {
        closeModal();
      }
    }
  } catch (e) {
    console.error('Не удалось обновить список устройств', e);
  }
}

function openModal(idx) {
  const d = devicesByIdx[idx];
  if (!d) return;
  openModalIdx = idx;
  document.getElementById('modal-title').textContent = typeText(d.type) + ' — Узел #' + idx;
  // Значение поля имени выставляется ТОЛЬКО здесь, при самом открытии - см.
  // комментарий у #modal-name-input в HTML выше.
  document.getElementById('modal-name-input').value = d.name || '';
  updateModal(d);
  document.getElementById('modal-backdrop').classList.add('open');
}

function closeModal() {
  openModalIdx = null;
  typeSpecificBuiltFor = null;
  document.getElementById('modal-backdrop').classList.remove('open');
}

// Обновляет ВСЁ содержимое модального окна, КРОМЕ поля ввода
// длительности (#modal-duration) - оно создаётся один раз внутри
// секции клапанов ниже, только если самого элемента ещё нет в DOM
// (то есть при первом построении этой секции для текущего открытия
// окна), а не на каждый фоновый refresh() - иначе набранное
// пользователем значение стиралось бы каждые 2 сек.
function updateModal(d) {
  document.getElementById('modal-mac').textContent = d.mac;
  document.getElementById('modal-conn').textContent = d.agoSec + ' с назад';

  const statusEl = document.getElementById('modal-status');
  statusEl.textContent = d.installed ? 'Установлено' : 'Кандидат (не установлено)';
  statusEl.className = 'status-pill ' + (d.installed ? 'installed' : 'candidate');

  // Переименование/управление/удаление - только для уже установленных (совпадает
  // с ограничением на стороне Хаба для переименования, см. DeviceManager::setName()) - один
  // тоггл для всего блока целиком (имя + действия + тип-специфичная часть), а не
  // отдельный на каждое поле внутри. Значение #modal-name-input здесь НЕ трогается
  // (оно ставится только в openModal()) - иначе набранное пользователем значение
  // стиралось бы каждые 2 сек при фоновом refresh().
  document.getElementById('modal-installed-section').style.display = d.installed ? '' : 'none';

  if (d.installed) {
    document.getElementById('modal-install-action').innerHTML = '';
    document.getElementById('modal-forget-action').innerHTML =
      '<button class="forget-btn" onclick="forgetDevice(' + d.idx + ', true)">Удалить устройство</button>';
  } else {
    document.getElementById('modal-install-action').innerHTML =
      '<button class="install-btn" onclick="installDevice(' + d.idx + ')">Установить</button>';
    document.getElementById('modal-forget-action').innerHTML = '';
  }

  // Специфичная для типа устройства часть (только для установленных, см. выше) - сейчас
  // есть только для полива (TYPE_IRRIGATION=1). Для будущих типов (освещение и т.п.)
  // сюда добавится свой блок по тому же принципу, что и
  // handleIrrigationPayload()/TODO в hub.ino.
  if (!d.installed) return; // тип-специфичная часть всё равно скрыта внутри modal-installed-section
  const typeSpecific = document.getElementById('modal-type-specific');
  if (d.type === 1) {
    // ПОЛНАЯ пересборка HTML (включая <select id="modal-cfg-mode">) происходит
    // ТОЛЬКО когда число клапанов/датчик потока/режим РЕАЛЬНО изменились (структурное
    // изменение - другое число строк или другое поле в "Управлении" - длительность вместо
    // объёма, см. buildIrrigationTypeSpecific() выше) или при открытии модалки на другом
    // устройстве - А НЕ на каждый фоновый refresh() (каждые 2 сек, пока модалка открыта) -
    // без этого ограничения открытый нативный <select> закрывался бы САМ СОБОЙ при каждой
    // перестройке DOM (браузер так реагирует на пересоздание узла, даже если значение
    // потом корректно восстанавливается) - из-за этого выпадающий список режима
    // самопроизвольно закрывался прямо во время выбора. На каждый обычный тик
    // вместо этого точечно обновляется только состояние клапанов/подсказка режима -
    // см. updateIrrigationLiveState() ниже. mode включён в ключ пересборки (в отличие от
    // valveCount/hasFlowSensor он меняется реже, но тоже структурно меняет разметку "Управления"
    // - длительность становится объёмом при переходе в режим 3 и обратно - без этого
    // поле осталось бы старым до следующего закрытия/открытия модалки.
    const needsRebuild = !typeSpecificBuiltFor ||
                          typeSpecificBuiltFor.idx !== d.idx ||
                          typeSpecificBuiltFor.valveCount !== d.valveCount ||
                          typeSpecificBuiltFor.hasFlowSensor !== d.hasFlowSensor ||
                          typeSpecificBuiltFor.mode !== d.mode;
    if (needsRebuild) {
      buildIrrigationTypeSpecific(d);
      typeSpecificBuiltFor = { idx: d.idx, valveCount: d.valveCount, hasFlowSensor: d.hasFlowSensor, mode: d.mode };
    }
    updateIrrigationLiveState(d);
  } else {
    typeSpecific.innerHTML =
      '<div class="valve-section"><span style="color:#999;font-size:0.85em;">' +
      'Специфичная информация для этого типа устройства пока не поддержана.</span></div>';
    typeSpecificBuiltFor = null;
  }
}

// Полная пересборка СТРУКТУРЫ секции клапанов+конфигурации - вызывается ТОЛЬКО из
// updateModal() при needsRebuild==true выше (смена устройства/valveCount), НЕ на каждый
// фоновый refresh(). Каждому ряду клапана и подсказке режима даются id - чтобы
// updateIrrigationLiveState() могла обновить их точечно, не трогая остальной DOM.
function buildIrrigationTypeSpecific(d) {
  const typeSpecific = document.getElementById('modal-type-specific');

  let rows = '';
  for (let v = 1; v <= d.valveCount; v++) {
    // Начальное состояние при построении - дальше его обновляет
    // updateIrrigationLiveState(), а не повторный вызов этой функции.
    const isOpen = ((d.activeValvesMask >> (v - 1)) & 1) === 1;
    rows +=
      '<div class="valve-row" id="valve-row-' + v + '">' +
        '<span>Клапан ' + v + '</span>' +
        '<span class="valve-state ' + (isOpen ? 'open' : 'closed') + '">' + (isOpen ? 'открыт' : 'закрыт') + '</span>' +
        '<button class="' + (isOpen ? 'close-btn' : '') + '">' +
          (isOpen ? 'Закрыть' : 'Открыть') +
        '</button>' +
      '</div>';
  }

  const existingDuration = document.getElementById('modal-duration');
  const durationValue = existingDuration ? existingDuration.value : 10;
  // Третий режим открытия (точная дозировка) - какое поле показать (длительность или
  // объём) решает НЕ отдельный переключатель тут, а текущий применённый режим
  // устройства d.mode (он задаётся в "Конфигурации модуля" ниже - см. valveButtonClick()
  // и блок вывода ниже). Здесь только сохраняем текущее значение поля объёма при
  // пересборке, тот же принцип, что и у durationValue выше.
  const existingDoseVolume = document.getElementById('modal-dose-volume');
  const doseVolumeValue = existingDoseVolume ? existingDoseVolume.value : '1.0';

  // Строки настроек автополива по каждому клапану - d.valveSchedules всегда массив из
  // MAX_IRRIGATION_VALVES элементов от Хаба (см. IrrigationDevice::appendJsonFields() в hub/IrrigationDevice.cpp),
  // здесь берётся только первые d.valveCount из них - тот же принцип, что и у строк клапанов в
  // секции "Управление" выше. При пересборке (needsRebuild в updateModal()) текущие
  // значения в полях (если они уже есть в DOM) сохраняются - тот же принцип, что и у
  // modal-cfg-* ниже, чтобы случайная пересборка (например, из-за смены hasFlowSensor) не стерла
  // несохранённый ввод оператора.
  let scheduleRows = '';
  for (let v = 1; v <= d.valveCount; v++) {
    const sched = (d.valveSchedules && d.valveSchedules[v - 1]) || { intervalDays: 1, volumeL: 0, autoEnabled: false };

    const existingInterval = document.getElementById('schedule-interval-' + v);
    const intervalValue = existingInterval ? existingInterval.value : sched.intervalDays;
    const existingVolume = document.getElementById('schedule-volume-' + v);
    const volumeValue = existingVolume ? existingVolume.value : sched.volumeL.toFixed(1);
    const existingAuto = document.getElementById('schedule-auto-' + v);
    const autoChecked = existingAuto ? existingAuto.checked : !!sched.autoEnabled;

    scheduleRows +=
      '<div class="schedule-row" id="schedule-row-' + v + '">' +
        '<div class="schedule-row-title">Клапан ' + v + '</div>' +
        '<div class="config-row">' +
          '<label for="schedule-interval-' + v + '">Периодичность полива</label>' +
          '<select id="schedule-interval-' + v + '">' + intervalDaysOptions(intervalValue) + '</select>' +
        '</div>' +
        '<div class="config-row">' +
          '<label for="schedule-volume-' + v + '">Объём, л</label>' +
          '<input type="number" min="0" step="0.1" id="schedule-volume-' + v + '" value="' + volumeValue + '">' +
        '</div>' +
        '<div class="config-row">' +
          '<label for="schedule-auto-' + v + '">Автополив включён</label>' +
          '<input type="checkbox" id="schedule-auto-' + v + '"' + (autoChecked ? ' checked' : '') + '>' +
        '</div>' +
        '<button class="apply-btn" onclick="saveValveSchedule(this, ' + d.idx + ', ' + v + ')">Сохранить</button>' +
      '</div>';
  }

  const existingCfgValves = document.getElementById('modal-cfg-valves');
  const cfgValvesValue = existingCfgValves ? existingCfgValves.value : (d.valveCount || 4);
  const existingCfgMode = document.getElementById('modal-cfg-mode');
  const cfgModeValue = existingCfgMode ? existingCfgMode.value : (d.mode || 1);
  const existingCfgFlowSensor = document.getElementById('modal-cfg-flow-sensor');
  const cfgFlowSensorChecked = existingCfgFlowSensor ? existingCfgFlowSensor.checked : !!d.hasFlowSensor;
  const existingCfgPulses = document.getElementById('modal-cfg-pulses');
  const cfgPulsesValue = existingCfgPulses ? existingCfgPulses.value : (d.flowPulsesPerLiter || 450);

  typeSpecific.innerHTML =
    // Настройки автополива по каждому клапану (периодичность/объём/вкл-выкл) - хранятся в NVS
    // Самого Хаба (см. DeviceManager::setValveSchedule()) и НЕ отправляются узлу - сам
    // автополив по расписанию пока не реализован (см. большой комментарий у ValveSchedule в hub/IrrigationDevice.h).
    // Сворачиваемая (<details>/<summary>, тот же паттерн, что и у "Управления"/"Конфигурации модуля"
    // ниже) - по умолчанию СВЁРНУТА (атрибут open не проставлен): настройки автополива
    // задаются редко, и по умолчанию свёрнутое состояние меньше загромождает окно при открытии
    // модалки.
    '<details class="settings-section" id="modal-settings-section">' +
      '<summary>Настройка</summary>' +
      (d.valveCount > 0
        ? scheduleRows
        : '<span style="color:#999;font-size:0.85em;">Ожидание конфигурации от устройства...</span>') +
    '</details>' +
    // Сворачиваемая (<details>/<summary>, тот же паттерн, что и у "Конфигурации модуля"
    // ниже) - по умолчанию свёрнута (атрибут open не проставлен): это самая часто
    // используемая часть модалки, но открытые клапаны и так видны точечным
    // обновлением на карточке/в других местах, а при частом открытии модалки на разных
    // устройствах свёрнутое по умолчанию состояние меньше загромождает окно.
    '<details class="valve-section">' +
      '<summary>Управление</summary>' +
      (d.hasFlowSensor
        ? '<div class="modal-field"><span>Израсходовано воды</span><b id="modal-water-used">—</b></div>'
        : '') +
      (d.valveCount > 0
        ? ((d.mode === 3
            // Режим 3 (дозирование, см. "Конфигурация модуля" ниже) - вместо длительности
            // показывается поле объёма - без отдельного переключателя, выбор режима
            // уже сделан в конфигурации. valveButtonClick() ниже сам читает d.mode и
            // отправляет команду с mode=2 (dose) и этим объёмом.
            ? ('<div class="duration-row">' +
                 '<label for="modal-dose-volume">Объём, л:</label>' +
                 '<input type="number" min="0.1" step="0.1" id="modal-dose-volume" value="' + doseVolumeValue + '">' +
               '</div>')
            : ('<div class="duration-row">' +
                 '<label for="modal-duration">Длительность открытия, сек:</label>' +
                 '<input type="number" min="1" id="modal-duration" value="' + durationValue + '">' +
               '</div>')) +
           '<div id="modal-mode-hint" style="font-size:0.75em;color:#999;margin:-6px 0 8px;"></div>' +
           rows)
        : '<span style="color:#999;font-size:0.85em;">Ожидание конфигурации от устройства...</span>') +
    '</details>' +
    '<details class="config-section">' +
      '<summary>Конфигурация модуля</summary>' +
      '<div class="config-row">' +
        '<label for="modal-cfg-valves">Количество каналов</label>' +
        '<input type="number" min="1" max="5" id="modal-cfg-valves" value="' + cfgValvesValue + '">' +
      '</div>' +
      '<div class="config-row">' +
        '<label for="modal-cfg-mode">Режим работы</label>' +
        '<select id="modal-cfg-mode">' +
          '<option value="1"' + (cfgModeValue == 1 ? ' selected' : '') + '>1 — Эксклюзивный (один клапан)</option>' +
          '<option value="2"' + (cfgModeValue == 2 ? ' selected' : '') + '>2 — Независимый (несколько сразу)</option>' +
          '<option value="3"' + (cfgModeValue == 3 ? ' selected' : '') + '>3 — Дозирование (по литрам)</option>' +
        '</select>' +
      '</div>' +
      '<div class="config-row">' +
        '<label for="modal-cfg-flow-sensor">Датчик расхода воды</label>' +
        '<input type="checkbox" id="modal-cfg-flow-sensor"' + (cfgFlowSensorChecked ? ' checked' : '') + '>' +
      '</div>' +
      '<div class="config-row">' +
        '<label for="modal-cfg-pulses">Импульсов на литр</label>' +
        '<input type="number" min="1" max="20000" id="modal-cfg-pulses" value="' + cfgPulsesValue + '">' +
      '</div>' +
      '<div class="config-hint">Наличие датчика и его разрешение нужно указать вручную по модели вашего датчика (например, YF-S201 — 450 имп/л) — узел не умеет определить это сам.</div>' +
      '<div class="config-hint">Хранится на самом устройстве и переживает его перезагрузку.</div>' +
      '<button class="apply-btn" onclick="saveModuleConfig(this, ' + d.idx + ')">Применить</button>' +
    '</details>';
}

// Точечное обновление состояния клапанов/подсказки режима - вызывается НА
// КАЖДЫЙ refresh(), НО НЕ трогает форму конфигурации (#modal-cfg-valves/#modal-cfg-mode) и
// #modal-duration вообще - именно это и решает проблему самопроизвольного закрытия
// выпадающего списка режима (см. buildIrrigationTypeSpecific() выше). Если d.valveCount<=0
// (ещё нет конфигурации от узла) - обновлять нечего, показывается заглушка "ожидание".
function updateIrrigationLiveState(d) {
  // Объём воды с момента последнего сброса узла (totalWaterUsed из /api/devices, см.
  // IrrigationDevice::lastTotalWaterUsed) - элемент существует в DOM ТОЛЬКО при d.hasFlowSensor==true (см.
  // buildIrrigationTypeSpecific()) - без датчика строки просто нет, и этот блок молча пропускает.
  const waterUsedEl = document.getElementById('modal-water-used');
  if (waterUsedEl) waterUsedEl.textContent = d.totalWaterUsed + ' л';

  if (d.valveCount <= 0) return;
  for (let v = 1; v <= d.valveCount; v++) {
    const row = document.getElementById('valve-row-' + v);
    if (!row) continue; // на всякий случай, если структура ещё не построена в этом тике
    const isOpen = ((d.activeValvesMask >> (v - 1)) & 1) === 1;
    const stateEl = row.querySelector('.valve-state');
    const btnEl = row.querySelector('button');
    stateEl.className = 'valve-state ' + (isOpen ? 'open' : 'closed');
    stateEl.textContent = isOpen ? 'открыт' : 'закрыт';
    btnEl.className = isOpen ? 'close-btn' : '';
    btnEl.textContent = isOpen ? 'Закрыть' : 'Открыть';
    btnEl.onclick = () => valveButtonClick(d.idx, v, isOpen);
  }

  const hint = document.getElementById('modal-mode-hint');
  if (hint) {
    hint.textContent = d.mode === 3
      ? 'Режим 3: открытие всегда точно по объёму и закрывает остальные клапаны.'
      : (d.mode === 2
        ? 'Режим 2: клапаны управляются независимо друг от друга.'
        : 'Режим 1: открытие клапана закрывает остальные.');
  }
}

// ACTION_OPEN=0, ACTION_CLOSE=1 (см. ValveAction в GardenProtocol.h) - дублирую
// числами здесь вместо именованных констант - это JS, не C++, общего enum'а с
// протоколом нет, значения дублируются вручную.
function valveButtonClick(idx, valve, wasOpen) {
  if (wasOpen) {
    // Закрываем ТОЛЬКО этот конкретный клапан (не все сразу) - корректно
    // работает в обоих режимах: в режиме 1 это и так был единственный открытый
    // клапан, в режиме 2 - останутся открытыми, как и должно быть при независимом
    // управлении. Также отменяет текущее дозирование этого клапана, если оно было
    // запущено (см. onCommand() в flow_node.ino).
    sendCmd(idx, valve, 1, 0, 0, 0);
    return;
  }

  // Третий режим открытия (точная дозировка) включается через "Конфигурацию модуля"
  // (персистентный d.mode==3 устройства, а не отдельный переключатель тут) - читаем
  // его из devicesByIdx (там же данные, что и в buildIrrigationTypeSpecific() выше, обновляются
  // в refresh()). При отсутствии записи (крайний случай, не должно случаться, так как
  // модалка уже открыта по этому idx) падаем на mode=0 (по времени).
  const dev = devicesByIdx[idx];

  if (dev && dev.mode === 3) {
    // volume теперь дробное (например "2.5") - бэкенд (handleApiCommand() в hub.ino) сам
    // округлит его до десятых литра (volume_dl) - см. комментарий там.
    const doseInput = document.getElementById('modal-dose-volume');
    const liters = doseInput ? (doseInput.value || '1.0') : '1.0';
    sendCmd(idx, valve, 0, 2, 0, liters);
  } else {
    const durationInput = document.getElementById('modal-duration');
    const sec = durationInput ? (durationInput.value || 10) : 10;
    sendCmd(idx, valve, 0, 0, sec, 0);
  }
}

async function sendCmd(idx, valve, action, mode, duration, volume) {
  await fetch('/api/command', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: 'idx=' + idx + '&valve=' + valve + '&action=' + action + '&mode=' + mode + '&duration=' + duration + '&volume=' + volume
  });
  setTimeout(refresh, 300);
}

// Сохраняет конфигурацию модуля (количество каналов/режим) из формы в
// модалке - отправляет их НА УЗЛ (POST /api/setConfig -> MSG_SET_CONFIG, см.
// sendSetConfig() в hub.ino) - узел сам валидирует и применяет, поэтому здесь
// нет оптимистичного обновления интерфейса - очередной фоновый refresh() подтянет
// уже действительное значение с узла (d.valveCount/d.mode), как только тот пришлёт (смотри
// комментарий у config-section в updateModal()).
async function saveModuleConfig(btn, idx) {
  const valves = document.getElementById('modal-cfg-valves').value;
  const mode = document.getElementById('modal-cfg-mode').value;
  const hasFlowSensor = document.getElementById('modal-cfg-flow-sensor').checked ? 1 : 0;
  const pulsesPerLiter = document.getElementById('modal-cfg-pulses').value;
  // Режим 3 (дозирование) без датчика потока всё равно будет отклонён и хабом
  // (см. handleApiSetConfig() в hub.ino), и узлом (onSetConfig() в flow_node.ino) - лучше
  // сказать об этом сразу, не тратя цикл запрос/повторов впустую.
  if (mode === '3' && !hasFlowSensor) {
    flashButton(btn, '✗ Включите датчик потока', false);
    return;
  }
  try {
    const res = await fetch('/api/setConfig', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'idx=' + idx + '&valveCount=' + valves + '&mode=' + mode +
            '&hasFlowSensor=' + hasFlowSensor + '&pulsesPerLiter=' + pulsesPerLiter
    });
    // Здесь "успешно" означает только "Хаб принял запрос и отправил его узлу" - самузел
    // валидирует и применяет асинхронно (см. комментарий выше у config-section) - достоверное
    // подтверждение применённого значения всё равно придёт позже с очередным refresh(), когда
    // узел пришлёт свои реальные d.valveCount/d.mode.
    flashButton(btn, res.ok ? '✓ Отправлено' : '✗ Ошибка', res.ok);
  } catch (e) {
    flashButton(btn, '✗ Ошибка', false);
  }
  setTimeout(refresh, 300);
}

// Склоняемое русского существительного после числа ("N день/дня/дней") - используется только
// в intervalDaysOptions() ниже, для диапазона 1..7 возможных значений периодичности полива
// (см. IntervalDays в ValveSchedule, hub/IrrigationDevice.h).
function intervalDaysWord(n) {
  if (n === 1) return 'день';
  if (n >= 2 && n <= 4) return 'дня';
  return 'дней';
}

// Строит набор <option> для выпадающего списка периодичности полива (1..7 дней, см.
// секцию "Настройка" в buildIrrigationTypeSpecific()) - selected может прийти как числом,
// так и строкой (от уже существующего <select>.value) - нестрогое сравнение (==) ниже
// специально ради этого.
function intervalDaysOptions(selected) {
  let opts = '';
  for (let n = 1; n <= 7; n++) {
    opts += '<option value="' + n + '"' + (n == selected ? ' selected' : '') + '>' +
            'Раз в ' + n + ' ' + intervalDaysWord(n) + '</option>';
  }
  return opts;
}

// Сохраняет настройки автополива одного клапана из секции "Настройка" (POST /api/setValveSchedule -
// чисто хабовая настройка, см. handleApiSetValveSchedule() в hub.ino - в ОТЛИЧИЕ от saveModuleConfig()
// ниже НИЧЕГО не отправляется узлу, поэтому результат применяется/подтверждается
// СРАЗУ, без ожидания асинхронного ответа от узла). Отдельная кнопка на каждый клапан (а не
// одна общая на всю секцию) - оператор может править и сохранять каждую линию независимо.
async function saveValveSchedule(btn, idx, valve) {
  const interval = document.getElementById('schedule-interval-' + valve).value;
  const volume = document.getElementById('schedule-volume-' + valve).value;
  const autoEnabled = document.getElementById('schedule-auto-' + valve).checked ? 1 : 0;
  try {
    const res = await fetch('/api/setValveSchedule', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'idx=' + idx + '&valve=' + valve + '&intervalDays=' + interval +
            '&volumeL=' + volume + '&autoEnabled=' + autoEnabled
    });
    flashButton(btn, res.ok ? '✓' : '✗', res.ok);
  } catch (e) {
    flashButton(btn, '✗', false);
  }
  setTimeout(refresh, 300);
}

function closeValve(idx) {
  // Закрыть ВСЕ клапаны устройства разом (target_valve=0 + ACTION_CLOSE, см.
  // sendCommand() в hub.ino) - сейчас ничем в интерфейсе не вызывается напрямую
  // (кнопка у каждого клапана теперь закрывает ТОЛЬКО его, см. valveButtonClick()) -
  // оставлена как готовая точка расширения (например, кнопка "Закрыть всё"
  // для режима 2, если понадобится).
  sendCmd(idx, 0, 1, 0, 0, 0);
}

// Кратковременно показывает на кнопке btn, применились ли изменения - меняет её
// текст/цвет (через btn-flash-ok/btn-flash-err, см. стили выше) на
// FLASH_DURATION_MS, потом возвращает исходный вид. Оригинальный текст кнопки
// запоминается в dataset при первом вызове (на случай, если кнопку нажмут
// повторно до того, как предыдущий flash закончился - таймер перезапускается, старый
// через clearTimeout не успеет вернуть чужой текст поверх нового).
const FLASH_DURATION_MS = 1600;
function flashButton(btn, tempText, ok) {
  if (!btn.dataset.origText) btn.dataset.origText = btn.textContent;
  if (btn._flashTimer) clearTimeout(btn._flashTimer);
  btn.textContent = tempText;
  btn.classList.remove('btn-flash-ok', 'btn-flash-err');
  btn.classList.add(ok ? 'btn-flash-ok' : 'btn-flash-err');
  btn._flashTimer = setTimeout(() => {
    btn.textContent = btn.dataset.origText;
    btn.classList.remove('btn-flash-ok', 'btn-flash-err');
    btn._flashTimer = null;
  }, FLASH_DURATION_MS);
}

// Сохраняет название текущего открытого в модалке устройства (openModalIdx) -
// кнопка 💾 теперь передаёт себя (this) в аргументе - чтобы показать на ней
// кратковременную обратную связь после ответа сервера (см. flashButton()) - раньше
// оператор не видел никакого подтверждения, что нажатие вообще что-то сделало.
async function saveDeviceName(btn) {
  if (openModalIdx === null) return;
  const input = document.getElementById('modal-name-input');
  try {
    const res = await fetch('/api/rename', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'idx=' + openModalIdx + '&name=' + encodeURIComponent(input.value)
    });
    flashButton(btn, res.ok ? '✓' : '✗', res.ok);
  } catch (e) {
    flashButton(btn, '✗', false);
  }
  setTimeout(refresh, 300);
}

async function installDevice(idx) {
  await fetch('/api/install', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: 'idx=' + idx
  });
  setTimeout(refresh, 300);
}

async function forgetDevice(idx, installed) {
  const msg = installed
    ? ('Устройство #' + idx + ' установлено и сохранено в памяти. Удалить насовсем?')
    : ('Забыть устройство #' + idx + '?');
  if (!confirm(msg)) return;
  await fetch('/api/forget', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: 'idx=' + idx
  });
  closeModal();
  setTimeout(refresh, 300);
}

// Закрытие модального окна по клику на затемнённый фон (не на саму
// карточку модального окна) и по Escape - обычные ожидаемые способы - для обеих
// модалок (устройства и "Настройки Хаба").
document.getElementById('modal-backdrop').addEventListener('click', (e) => {
  if (e.target.id === 'modal-backdrop') closeModal();
});
document.getElementById('hub-modal-backdrop').addEventListener('click', (e) => {
  if (e.target.id === 'hub-modal-backdrop') closeHubModal();
});
document.addEventListener('keydown', (e) => {
  if (e.key !== 'Escape') return;
  if (openModalIdx !== null) closeModal();
  else if (hubModalOpen) closeHubModal();
});

// Хаб не имеет RTC-модуля с батарейкой - его часы (time()/settimeofday())
// сбрасываются на 1970-01-01 при каждой перезагрузке. Нужно МЕСТНОЕ
// время (читаемые метки в логах/статусе), а не UTC - поэтому страница
// сдвигает Date.now() на местный часовой пояс браузера (getTimezoneOffset())
// перед отправкой и шлёт уже готовое "местное" значение. Хаб просто
// хранит его как есть (вся арифметика с поясом/DST - на стороне браузера, не
// встраиваемой системы - тот же принцип, что и раньше). См. PROTOCOL.md §12.
function localEpochSeconds() {
  const now = new Date();
  const utcSeconds = Math.floor(now.getTime() / 1000);
  if (typeof now.getTimezoneOffset === 'function') {
    // getTimezoneOffset() - разница UTC-местное в минутах (положительная для
    // поясов западнее UTC, отрицательная для восточнее) - именно
    // поэтому здесь вычитание, а не сложение.
    return utcSeconds - now.getTimezoneOffset() * 60;
  }
  // Фоллбэк на случай, если браузер вдруг не поддержал стандартный метод
  // Date.getTimezoneOffset() (в любом современном браузере он есть, это чисто защита
  // на самый крайний случай) - считаем часовой пояс Киева: летом (как сейчас)
  // UTC+3 (EEST), зимой UTC+2 (EET) - без доступа к getTimezoneOffset() DST-переход
  // отследить нечем, поэтому берём летнее значение как более вероятное большую
  // часть сезона выращивания.
  const KYIV_SUMMER_OFFSET_SECONDS = 3 * 3600;
  return utcSeconds + KYIV_SUMMER_OFFSET_SECONDS;
}

async function syncTime() {
  try {
    await fetch('/api/settime', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'epoch=' + localEpochSeconds()
    });
  } catch (e) {
    console.error('Не удалось синхронизировать время', e);
  }
  // Статус обновляем сразу после попытки синхронизации, не дожидаясь
  // следующего тика refreshStatus() - иначе индикатор до 2 сек показывал
  // бы старое (не)синхронизированное состояние.
  refreshStatus();
}

// Форматирует время работы в секундах (uptimeSec из /api/status, считается на самом Хабе
// от millis(), см. handleApiStatus() в hub.ino) в читаемый вид "N д. HH:MM:SS" (дни опускаются,
// если их 0) - это время СОБСТВЕННО ХАБА с момента его последнего сброса/включения питания
// (в отличие от часов выше - не зависит от синхронизации с браузером/DS3231, доступно всегда).
function formatUptime(totalSec) {
  const days = Math.floor(totalSec / 86400);
  const hours = Math.floor((totalSec % 86400) / 3600);
  const mins = Math.floor((totalSec % 3600) / 60);
  const secs = totalSec % 60;
  const pad = (n) => String(n).padStart(2, '0');
  const hms = pad(hours) + ':' + pad(mins) + ':' + pad(secs);
  return days > 0 ? (days + ' д. ' + hms) : hms;
}

async function refreshStatus() {
  try {
    const res = await fetch('/api/status');
    const status = await res.json();

    const uptimeEl = document.getElementById('uptime-status');
    if (typeof status.uptimeSec === 'number') {
      uptimeEl.textContent = 'Время после сброса: ' + formatUptime(status.uptimeSec);
    }

    // Температура кристалла Самого Хаба (chipTempC из /api/status, см. temperatureRead() в
    // handleApiStatus() в hub.ino) - обновляется СРАЗУ ДВА элемента - одна строка на
    // главной странице (#temp-status) и одна в модалке "Настройки Хаба"
    // (#modal-temp-value) - id не могут повторяться на странице, поэтому два разных элемента.
    // Красный цвет при заметно высокой температуре (порог чисто для визуального
    // внимания, не аварийный порог - встроенный датчик ESP32 всё равно не
    // точен, см. комментарий в hub.ino) - оба элемента подсвечиваются одинаково, через
    // общий CSS-класс .temp-hot (не привязан к конкретному id, см. стили выше).
    if (typeof status.chipTempC === 'number') {
      const tempText = status.chipTempC.toFixed(1) + ' °C';
      const isHot = status.chipTempC >= 75;

      const tempStatusEl = document.getElementById('temp-status');
      tempStatusEl.textContent = 'Температура Хаба: ' + tempText;
      tempStatusEl.classList.toggle('temp-hot', isHot);

      const modalTempEl = document.getElementById('modal-temp-value');
      modalTempEl.textContent = tempText;
      modalTempEl.classList.toggle('temp-hot', isHot);
    }

    // Мощность передатчика (txPowerQuarterDbm из /api/status, сырое число в четвертях дБм,
    // точно то же, что возвращает WiFi.getTxPower() на Хабе) - список опций строится ОДИН
    // РАЗ (populateTxPowerOptions() ниже), а дальше каждый тик просто ставится текущее значение.
    // В отличие от текстовых полей/секции конфигурации узла выше, ЗДЕСЬ НАРОЧНО НЕ
    // проверяется document.activeElement: сама по себе установка .value у <select> НЕ закрывает
    // открытый список и НЕ сбивает набор текста (как было бы с <input>) - а без этой синхронизации
    // выбранное значение не подтверждалось/не подсвечивалось в списке, пока select оставался в фокусе
    // (то есть практически всё время после выбора, если оператор не кликнул куда-то ещё) - именно
    // это было багом.
    const txSelect = document.getElementById('tx-power-select');
    if (txSelect.options.length === 0) populateTxPowerOptions(txSelect);
    if (typeof status.txPowerQuarterDbm === 'number') {
      setSelectValueEnsuringOption(txSelect, status.txPowerQuarterDbm, (v) => (v / 4) + ' дБм (нестандартно)');
    }

    // Частота процессора (cpuFreqMhz из /api/status, см. getCpuFrequencyMhz() в hub.ino) - то же
    // самое обращение, что и с мощностью передатчика выше - список один раз, значение каждый тик.
    const cpuSelect = document.getElementById('cpu-freq-select');
    if (cpuSelect.options.length === 0) populateCpuFreqOptions(cpuSelect);
    if (typeof status.cpuFreqMhz === 'number') {
      setSelectValueEnsuringOption(cpuSelect, status.cpuFreqMhz, (v) => v + ' МГц (нестандартно)');
    }

    // Пороги включения/выключения вентилятора (fanOnTempC/fanOffTempC из /api/status, см. большой
    // комментарий у fanOnTempC/fanOffTempC в hub.ino) - заполняются ТОЛЬКО ОДИН РАЗ за всю
    // жизнь страницы (fanInputsInitialized) - это <input>, не <select>, повторная
    // установка .value каждые 2 сек стирала бы набираемое оператором значение
    // прямо во время ввода (см. комментарий у самих полей в HTML).
    if (!fanInputsInitialized && typeof status.fanOnTempC === 'number' && typeof status.fanOffTempC === 'number') {
      document.getElementById('fan-on-input').value = status.fanOnTempC;
      document.getElementById('fan-off-input').value = status.fanOffTempC;
      fanInputsInitialized = true;
    }

    // Если вентилятор сейчас включён (fanOn из /api/status) - добавляем это к тексту
    // строки температуры на главной странице - оператор должен видеть это, даже не
    // открывая модалку.
    if (status.fanOn) {
      const tempStatusEl = document.getElementById('temp-status');
      tempStatusEl.textContent += ' — вентилятор включён';
    }
  } catch (e) {
    console.error('Не удалось получить статус Хаба', e);
  }
}

// Ставит в select текущее значение с Хаба; если такого <option> ещё нет среди штатных
// (например, если драйвер/регуляторные ограничения округлили/отклонили запрошенное
// значение, и реально применённое число не совпало ни с одним из наших вариантов) -
// добавляет недостающий <option> на лету, чтобы список ВСЕГДА показывал реальное текущее
// значение, а не оставался пустым/невыбранным - именно это был симптом описанного
// бага (выбранное значение не подсвечивалось в списке).
function setSelectValueEnsuringOption(selectEl, value, formatFallbackLabel) {
  const strValue = String(value);
  let hasOption = false;
  for (let i = 0; i < selectEl.options.length; i++) {
    if (selectEl.options[i].value === strValue) { hasOption = true; break; }
  }
  if (!hasOption) {
    const opt = document.createElement('option');
    opt.value = strValue;
    opt.textContent = formatFallbackLabel(value);
    selectEl.appendChild(opt);
  }
  selectEl.value = strValue;
}

// Список допустимых значений мощности передатчика - должен точно совпадать с
// TX_POWER_OPTIONS в hub.ino (те же значения wifi_power_t, численно равные четвертям
// дБм) - если там список когда-нибудь изменится - править и здесь.
const TX_POWER_OPTIONS = [
  [78, '19.5 дБм'], [76, '19 дБм'], [74, '18.5 дБм'], [68, '17 дБм'],
  [60, '15 дБм'], [52, '13 дБм'], [44, '11 дБм'], [34, '8.5 дБм'],
  [28, '7 дБм'], [20, '5 дБм'], [8, '2 дБм'], [-4, '-1 дБм'],
];

function populateTxPowerOptions(selectEl) {
  TX_POWER_OPTIONS.forEach(([value, label]) => {
    const opt = document.createElement('option');
    opt.value = value;
    opt.textContent = label;
    selectEl.appendChild(opt);
  });
}

// Вызывается при выборе значения в <select> (onchange, см. разметку выше) - меняет
// мощность передатчика САМОГО ХАБА (не узла!) через POST /api/setTxPower - см.
// handleApiSetTxPower() в hub.ino. Никакого отдельного подтверждения на самом select не нужно - изменение
// мощности применяется СРАЗУ на стороне Хаба, без перезагрузки радио.
async function setTxPower(selectEl) {
  try {
    await fetch('/api/setTxPower', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'value=' + selectEl.value
    });
  } catch (e) {
    console.error('Не удалось изменить мощность передатчика', e);
  }
  refreshStatus();
}

// Список допустимых частот процессора (МГц) - должен точно совпадать с
// CPU_FREQ_OPTIONS в hub.ino (там же пояснение, почему только 240/160/80 - меньшие частоты
// ломают ESP-NOW/Wi-Fi при активном радио).
const CPU_FREQ_OPTIONS = [240, 160, 80];

function populateCpuFreqOptions(selectEl) {
  CPU_FREQ_OPTIONS.forEach((mhz) => {
    const opt = document.createElement('option');
    opt.value = mhz;
    opt.textContent = mhz + ' МГц';
    selectEl.appendChild(opt);
  });
}

// Вызывается при выборе значения в <select> частоты - аналогично setTxPower() выше, POST
// /api/setCpuFreq (см. handleApiSetCpuFreq() в hub.ino). Сама смена частоты происходит НА
// САМОМ ХАБЕ мгновенно (без перезагрузки), но сервер может ответить ошибкой (500), если
// ядро отказалось от переключения - refreshStatus() в любом случае подтянет ФАКТИЧЕСКУЮ
// текущую частоту от Хаба и вернёт select к прежнему значению.
async function setCpuFreq(selectEl) {
  try {
    await fetch('/api/setCpuFreq', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'value=' + selectEl.value
    });
  } catch (e) {
    console.error('Не удалось изменить частоту процессора', e);
  }
  refreshStatus();
}

// Сохраняет оба порога включения/выключения вентилятора одним запросом (POST
// /api/setFanThresholds, см. handleApiSetFanThresholds() в hub.ino) - одна кнопка на оба
// поля, потому что валидны они только вместе (температура включения должна быть выше
// температуры выключения - сама проверка делается на стороне Хаба, здесь ничего не
// дублируется). Обратная связь на кнопке - через flashButton(), тот же паттерн, что и у
// saveModuleConfig()/saveDeviceName() выше.
async function saveFanThresholds(btn) {
  const on = document.getElementById('fan-on-input').value;
  const off = document.getElementById('fan-off-input').value;
  try {
    const res = await fetch('/api/setFanThresholds', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'on=' + on + '&off=' + off
    });
    flashButton(btn, res.ok ? '✓ Сохранено' : '✗ Ошибка', res.ok);
  } catch (e) {
    flashButton(btn, '✗ Ошибка', false);
  }
}

syncTime();
refresh();
setInterval(refresh, 2000);
setInterval(refreshStatus, 2000);
// Периодическая пересинхронизация: часы ESP32 без внешнего RTC понемногу
// уходят (обычный дрейф кварцевого генератора). Раз в 30 минут, пока
// страница открыта, поправляем их заново - недорого и не требует участия
// оператора.
setInterval(syncTime, 1800000);
</script>
</body>
</html>
)rawliteral";
