#include "ApButton.h"

void ApButton::begin(uint8_t buttonPin, unsigned long debounce) {
    pin = buttonPin;
    debounceMs = debounce;
    pinMode(pin, INPUT_PULLUP);
    // Инициализируем текущим реальным состоянием, а не жёстким HIGH -
    // если кнопка почему-то зажата уже на старте, не хотим считать это
    // "нажатием" в первую же итерацию loop().
    lastReading = digitalRead(pin);
    lastChangeMs = millis();
}

bool ApButton::wasPressed() {
    int reading = digitalRead(pin);

    if (reading != lastReading) {
        lastChangeMs = millis();
        lastReading = reading;
    }

    if (millis() - lastChangeMs > debounceMs) {
        bool pressedNow = (reading == LOW);
        bool edge = pressedNow && !isPressed; // фронт HIGH -> LOW
        isPressed = pressedNow;
        return edge;
    }

    return false;
}
