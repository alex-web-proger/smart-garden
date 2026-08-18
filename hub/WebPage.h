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

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Garden — Хаб</title>
<style>
  body { font-family: -apple-system, sans-serif; margin: 16px; background:#f6f6f2; color:#222; }
  h1 { font-size: 1.2em; margin-bottom: 4px; }
  .sub { color:#777; font-size: 0.85em; margin-bottom: 16px; }
  #time-status { font-size: 0.85em; margin-bottom: 12px; }
  .time-synced { color:#2a7d2a; }
  .time-unsynced { color:#b8860b; }
  table { width:100%; border-collapse: collapse; background:#fff; border-radius:8px; overflow:hidden; }
  th, td { padding: 10px 8px; border-bottom: 1px solid #eee; text-align:left; font-size: 0.85em; vertical-align: middle; }
  th { background:#eef4ea; }
  button { padding: 8px 12px; margin: 2px; border:none; border-radius:6px; background:#4a7c3f; color:white; font-size:0.85em; }
  button.close-btn { background:#a33; }
  button.install-btn { background:#2a6fa3; }
  button.forget-btn { background:#888; }
  input[type=number] { width: 48px; padding:4px; border:1px solid #ccc; border-radius:4px; }
  .valve-open { color:#2a7d2a; font-weight:bold; }
  .valve-closed { color:#999; }
  .status-installed { color:#2a7d2a; font-weight:bold; }
  .status-candidate { color:#b8860b; }
  .empty { padding: 20px; text-align:center; color:#999; }
</style>
</head>
<body>
<h1>Smart Garden — Хаб</h1>
<div class="sub">Автообновление каждые 2 сек. "Кандидат" — обнаружен по трафику, может быть вытеснен. "Установлено" — подтверждено, сохранено, защищено от вытеснения.</div>
<div id="time-status" class="time-unsynced">Синхронизация времени...</div>
<table>
  <thead><tr><th>#</th><th>MAC</th><th>Тип</th><th>Связь</th><th>Статус</th><th>Клапан</th><th>Управление</th></tr></thead>
  <tbody id="devices-body"><tr><td colspan="7" class="empty">Загрузка...</td></tr></tbody>
</table>

<script>
// idx -> { tr: <DOM-строка>, installed: bool } - хранится между вызовами
// refresh(), чтобы не пересоздавать DOM-узлы (в т.ч. поля ввода)
// на каждый тик автообновления.
let rowsByIdx = {};

async function refresh() {
  try {
    const res = await fetch('/api/devices');
    const devices = await res.json();
    const body = document.getElementById('devices-body');

    if (devices.length === 0) {
      body.innerHTML = '<tr><td colspan="7" class="empty">Устройств пока не видно</td></tr>';
      rowsByIdx = {};
      return;
    }

    // Перед первым реальным наполнением убираем плейсхолдер
    // ("Загрузка..."/"Устройств пока не видно").
    if (Object.keys(rowsByIdx).length === 0) {
      body.innerHTML = '';
    }

    const seenIdx = new Set();

    devices.forEach(d => {
      seenIdx.add(d.idx);
      const isOpen = d.activeValve !== 0;
      const valveText = isOpen ? ('открыт #' + d.activeValve) : 'закрыт';
      const typeText = d.type === 1 ? 'полив' : ('тип ' + d.type);
      const statusText = d.installed ? 'установлено' : 'кандидат';
      const statusClass = d.installed ? 'status-installed' : 'status-candidate';

      let row = rowsByIdx[d.idx];
      if (!row) {
        // Строка создаётся ОДИН раз на устройство. В частности, поля
        // ввода (valve-N/sec-N) создаются здесь и больше никогда не
        // трогаются - именно это чинит "стирание при вводе": раньше
        // вся строка (вместе с input-ами) пересоздавалась заново на каждый
        // тик автообновления.
        const tr = document.createElement('tr');
        tr.innerHTML =
          '<td>' + d.idx + '</td>' +
          '<td>' + d.mac + '</td>' +
          '<td>' + typeText + '</td>' +
          '<td class="conn"></td>' +
          '<td class="status"></td>' +
          '<td class="valve"></td>' +
          '<td>' +
            '<input type="number" min="1" max="4" value="1" id="valve-' + d.idx + '"> клапан ' +
            '<input type="number" min="1" value="10" id="sec-' + d.idx + '"> сек ' +
            '<button onclick="openValve(' + d.idx + ')">Открыть</button>' +
            '<button class="close-btn" onclick="closeValve(' + d.idx + ')">Закрыть</button>' +
            '<span class="install-slot"></span>' +
            '<button class="forget-btn">Забыть</button>' +
          '</td>';
        body.appendChild(tr);
        row = { tr: tr, installed: null };
        rowsByIdx[d.idx] = row;
      }

      // Дальше - обновление ТОЛЬКО тех ячеек, что реально меняются
      // (время связи, статус, состояние клапана). Поля ввода и кнопки
      // не пересоздаются, поэтому не теряют введённое значение/фокус.
      row.tr.querySelector('.conn').textContent = d.agoSec + 'с назад';

      const statusCell = row.tr.querySelector('.status');
      statusCell.textContent = statusText;
      statusCell.className = 'status ' + statusClass;

      const valveCell = row.tr.querySelector('.valve');
      valveCell.textContent = valveText;
      valveCell.className = 'valve ' + (isOpen ? 'valve-open' : 'valve-closed');

      // Кнопку "Установить"/обработчик "Забыть" трогаем, только если
      // статус installed реально изменился (редкое, инициируется
      // пользователем) событие - не на каждый тик.
      if (row.installed !== d.installed) {
        row.installed = d.installed;
        const installSlot = row.tr.querySelector('.install-slot');
        installSlot.innerHTML = d.installed
          ? ''
          : '<button class="install-btn" onclick="installDevice(' + d.idx + ')">Установить</button>';
        row.tr.querySelector('.forget-btn')
          .setAttribute('onclick', 'forgetDevice(' + d.idx + ', ' + d.installed + ')');
      }
    });

    // Устройство пропало из ответа (например, кто-то его забыл через
    // другой клиент) - убираем его строку.
    Object.keys(rowsByIdx).forEach(function (idxStr) {
      const idx = Number(idxStr);
      if (!seenIdx.has(idx)) {
        rowsByIdx[idx].tr.remove();
        delete rowsByIdx[idx];
      }
    });
  } catch (e) {
    console.error('Не удалось обновить список устройств', e);
  }
}

async function sendCmd(idx, valve, mode, duration, volume) {
  await fetch('/api/command', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: 'idx=' + idx + '&valve=' + valve + '&mode=' + mode + '&duration=' + duration + '&volume=' + volume
  });
  setTimeout(refresh, 300);
}

function openValve(idx) {
  const valve = document.getElementById('valve-' + idx).value;
  const sec = document.getElementById('sec-' + idx).value;
  sendCmd(idx, valve, 0, sec, 0);
}

function closeValve(idx) {
  sendCmd(idx, 0, 0, 0, 0);
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
  setTimeout(refresh, 300);
}

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

async function refreshStatus() {
  try {
    const res = await fetch('/api/status');
    const status = await res.json();
    const el = document.getElementById('time-status');
    if (status.timeSynced) {
      el.textContent = 'Время Хаба: ' + status.timeString;
      el.className = 'time-synced';
    } else {
      el.textContent = 'Время Хаба не синхронизировано (ожидание браузера)';
      el.className = 'time-unsynced';
    }
  } catch (e) {
    console.error('Не удалось получить статус Хаба', e);
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
