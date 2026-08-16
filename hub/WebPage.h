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
<table>
  <thead><tr><th>#</th><th>MAC</th><th>Тип</th><th>Связь</th><th>Статус</th><th>Клапан</th><th>Управление</th></tr></thead>
  <tbody id="devices-body"><tr><td colspan="7" class="empty">Загрузка...</td></tr></tbody>
</table>

<script>
async function refresh() {
  try {
    const res = await fetch('/api/devices');
    const devices = await res.json();
    const body = document.getElementById('devices-body');
    if (devices.length === 0) {
      body.innerHTML = '<tr><td colspan="7" class="empty">Устройств пока не видно</td></tr>';
      return;
    }
    body.innerHTML = '';
    devices.forEach(d => {
      const tr = document.createElement('tr');
      const isOpen = d.activeValve !== 0;
      const valveText = isOpen ? ('открыт #' + d.activeValve) : 'закрыт';
      const typeText = d.type === 1 ? 'полив' : ('тип ' + d.type);
      const statusText = d.installed ? 'установлено' : 'кандидат';
      const statusClass = d.installed ? 'status-installed' : 'status-candidate';
      const installBtn = d.installed
        ? ''
        : '<button class="install-btn" onclick="installDevice(' + d.idx + ')">Установить</button>';
      tr.innerHTML =
        '<td>' + d.idx + '</td>' +
        '<td>' + d.mac + '</td>' +
        '<td>' + typeText + '</td>' +
        '<td>' + d.agoSec + 'с назад</td>' +
        '<td class="' + statusClass + '">' + statusText + '</td>' +
        '<td class="' + (isOpen ? 'valve-open' : 'valve-closed') + '">' + valveText + '</td>' +
        '<td>' +
          '<input type="number" min="1" max="4" value="1" id="valve-' + d.idx + '"> клапан ' +
          '<input type="number" min="1" value="10" id="sec-' + d.idx + '"> сек ' +
          '<button onclick="openValve(' + d.idx + ')">Открыть</button>' +
          '<button class="close-btn" onclick="closeValve(' + d.idx + ')">Закрыть</button>' +
          installBtn +
          '<button class="forget-btn" onclick="forgetDevice(' + d.idx + ', ' + d.installed + ')">Забыть</button>' +
        '</td>';
      body.appendChild(tr);
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

refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
)rawliteral";
