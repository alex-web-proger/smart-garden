#include "Ds3231Rtc.h"
#include <Wire.h>

namespace {
constexpr uint8_t DS3231_I2C_ADDR = 0x68;
constexpr uint8_t REG_SECONDS = 0x00; // .. 0x06 = year, BCD, см. datasheet DS3231
constexpr uint8_t REG_STATUS = 0x0F;  // бит 7 = OSF (Oscillator Stop Flag)
constexpr uint8_t OSF_BIT = 0x80;

uint8_t readRegister(uint8_t reg) {
    Wire.beginTransmission(DS3231_I2C_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false); // repeated start - без отпускания шины
    Wire.requestFrom((int) DS3231_I2C_ADDR, 1);
    if (Wire.available()) return Wire.read();
    return 0;
}

void writeRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(DS3231_I2C_ADDR);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}
} // namespace

uint8_t Ds3231Rtc::bcdToDec(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

uint8_t Ds3231Rtc::decToBcd(uint8_t dec) {
    return ((dec / 10) << 4) | (dec % 10);
}

bool Ds3231Rtc::begin() {
    // Простейшая проверка присутствия: если по адресу 0x68 никто не
    // ответил (NACK), считаем, что микросхема не установлена - это
    // штатный, а не ошибочный сценарий (проект должен работать и без неё,
    // на одной синхронизации с браузером).
    Wire.beginTransmission(DS3231_I2C_ADDR);
    uint8_t error = Wire.endTransmission();
    present = (error == 0);
    return present;
}

bool Ds3231Rtc::hasReliableTime() {
    if (!present) return false;
    uint8_t status = readRegister(REG_STATUS);
    return (status & OSF_BIT) == 0;
}

uint32_t Ds3231Rtc::civilToEpoch(int year, int month, int day, int hour, int minute, int second) {
    // Алгоритм Хауарда Хиннанта (days_from_civil) - количество дней от
    // 1970-01-01 для произвольной календарной даты, без обращения к
    // системным timezone-таблицам.
    int y = year - (month <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned) (y - era * 400);               // [0, 399]
    unsigned mp = (unsigned) (month + (month > 2 ? -3 : 9));  // [0, 11]
    unsigned doy = (153 * mp + 2) / 5 + (unsigned) day - 1;   // [0, 365]
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;     // [0, 146096]
    long days = era * 146097L + (long) doe - 719468L;         // дней от 1970-01-01

    return (uint32_t) (days * 86400L + hour * 3600L + minute * 60L + second);
}

void Ds3231Rtc::epochToCivil(uint32_t epoch, int &year, int &month, int &day,
                              int &hour, int &minute, int &second) {
    long days = (long) (epoch / 86400UL);
    long rem = (long) (epoch % 86400UL);
    hour = (int) (rem / 3600);
    minute = (int) ((rem % 3600) / 60);
    second = (int) (rem % 60);

    // Обратное преобразование (civil_from_days) - тот же алгоритм
    // Хауарда Хиннанта.
    days += 719468;
    long era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned) (days - era * 146097);                       // [0, 146096]
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    long y = (long) yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                // [0, 365]
    unsigned mp = (5 * doy + 2) / 153;                                     // [0, 11]
    day = (int) (doy - (153 * mp + 2) / 5 + 1);                            // [1, 31]
    month = (int) (mp < 10 ? mp + 3 : mp - 9);                             // [1, 12]
    year = (int) (month <= 2 ? y + 1 : y);
}

uint32_t Ds3231Rtc::getEpoch() {
    if (!present) return 0;

    Wire.beginTransmission(DS3231_I2C_ADDR);
    Wire.write(REG_SECONDS);
    Wire.endTransmission(false);
    Wire.requestFrom((int) DS3231_I2C_ADDR, 7);
    if (Wire.available() < 7) return 0;

    uint8_t rawSec = Wire.read();
    uint8_t rawMin = Wire.read();
    uint8_t rawHour = Wire.read();
    Wire.read();                    // день недели (0x03) - для эпохи не нужен
    uint8_t rawDate = Wire.read();
    uint8_t rawMonth = Wire.read();
    uint8_t rawYear = Wire.read();

    int second = bcdToDec(rawSec & 0x7F);
    int minute = bcdToDec(rawMin & 0x7F);
    // Работаем строго в 24-часовом режиме (бит 6 = 0) - setEpoch() всегда
    // пишет именно в этом режиме, так что при чтении обратно бит 6 будет
    // сброшен, если время писал этот же класс.
    int hour = bcdToDec(rawHour & 0x3F);
    int day = bcdToDec(rawDate & 0x3F);
    int month = bcdToDec(rawMonth & 0x1F); // бит 7 (флаг века) не используем
    int year = 2000 + bcdToDec(rawYear);

    return civilToEpoch(year, month, day, hour, minute, second);
}

void Ds3231Rtc::setEpoch(uint32_t epoch) {
    if (!present) return;

    int year, month, day, hour, minute, second;
    epochToCivil(epoch, year, month, day, hour, minute, second);

    // День недели DS3231 не используется нигде в проекте (Хаб читает
    // только дату/время) - пишем что-то валидное (1-7) просто чтобы
    // регистр не остался в произвольном состоянии. 1970-01-01 - четверг,
    // поэтому со сдвигом +4 воскресенье (0) даёт 4 - остаётся привести к
    // диапазону 1-7.
    long days = (long) (epoch / 86400UL);
    uint8_t dow = (uint8_t) (((days + 4) % 7 + 7) % 7) + 1;

    Wire.beginTransmission(DS3231_I2C_ADDR);
    Wire.write(REG_SECONDS);
    Wire.write(decToBcd((uint8_t) second));
    Wire.write(decToBcd((uint8_t) minute));
    Wire.write(decToBcd((uint8_t) hour)); // бит 6 = 0 -> 24-часовой режим
    Wire.write(dow);
    Wire.write(decToBcd((uint8_t) day));
    Wire.write(decToBcd((uint8_t) month)); // бит 7 (век) оставляем 0
    Wire.write(decToBcd((uint8_t) (year - 2000)));
    Wire.endTransmission();

    // Сбрасываем OSF - время только что задано заново, поэтому оно снова
    // достоверно с этого момента.
    uint8_t status = readRegister(REG_STATUS);
    writeRegister(REG_STATUS, status & ~OSF_BIT);
}
