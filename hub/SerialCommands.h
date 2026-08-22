#pragma once
#include <Arduino.h>

// Простой текстовый интерфейс через Serial Monitor - работает параллельно
// с веб-интерфейсом, один не отменяет другой. Полный список команд
// см. в hub.ino (Serial.println в setup()) и в README.md модуля.
//
// Вынесен из hub.ino в отдельный файл, чтобы не смешивать разбор
// пользовательского ввода с логикой ESP-NOW/веб-сервера. Обращается к
// глобальным объектам, определённым в hub.ino (deviceManager, sendCommand,
// timeSynced, currentTimeString) через extern - см. SerialCommands.cpp.
void handleSerialCommand(String line);
