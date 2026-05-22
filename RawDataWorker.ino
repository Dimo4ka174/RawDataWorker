#include <SPI.h>
#include <Ethernet.h>
#include "EmonLib.h"

// ===== НАСТРОЙКИ СЕТИ =====
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 10, 177);
IPAddress gateway(192, 168, 10, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);
IPAddress serverIP(192, 168, 10, 200);
const int serverPort = 5050;

// ===== КАЛИБРОВКА =====
const float CALIBRATION = 4; // Выше больше сила тока, ниже меньше сила тока
const float CURRENT_THRESHOLD = 0.05;  // Мертвая зона по силе тока (А)
const float VOLTAGE = 230.0;

EthernetClient client;
EnergyMonitor emon1;

String machineName = "Danobat";

// ===== СТАБИЛИЗАЦИЯ ДАННЫХ =====
const int NEED_STABLE_READINGS = 3;   // Нужно 3 одинаковых показания
int stableCount = 0;                  // Счетчик стабильных показаний
bool lastStableStatus = false;        // Последний стабильный статус
bool lastSentStatus = false;          // Последний отправленный статус
float lastStableCurrent = 0;          // Последний стабильный ток
float lastStablePower = 0;            // Последняя стабильная мощность

// Переменные для опроса
unsigned long lastMeasureTime = 0;
const unsigned long MEASURE_INTERVAL = 1000; // Опрос каждую секунду

// Переменные для отправки
bool isWaitingForResponse = false;
unsigned long responseTimeout = 0;
const unsigned long RESPONSE_WAIT = 2000; // Ждем ответ 2 секунды

// Функция для преобразования IP в строку
String ipToString(IPAddress ip) {
  String result = "";
  result += String(ip[0]);
  result += ".";
  result += String(ip[1]);
  result += ".";
  result += String(ip[2]);
  result += ".";
  result += String(ip[3]);
  return result;
}

// Отправка данных на сервер
bool sendData(bool condition, float current, float power) {
  if (client.connect(serverIP, serverPort)) {
    String json = "{\"machineName\":\"" + machineName + 
                  "\",\"Condition\":" + (condition ? "true" : "false") +
                  ",\"current\":" + String(current, 2) +
                  ",\"power\":" + String(power, 0) + "}";
    
    client.println("POST /api/Wincc/updateMachineStatus HTTP/1.1");
    client.print("Host: ");
    client.println(ipToString(serverIP));
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.println(json.length());
    client.println("Connection: close");
    client.println();
    client.println(json);
    
    unsigned long timeout = millis() + RESPONSE_WAIT;
    bool success = false;
    
    while (millis() < timeout) {
      if (client.available()) {
        String response = client.readString();
        if (response.indexOf("\"changed\":true") >= 0) {
          success = true;
          Serial.print(" [ЗАПИСАНО]");
        } else if (response.indexOf("\"changed\":false") >= 0) {
          success = true;
          Serial.print(" [НЕТ ИЗМЕНЕНИЙ]");
        } else if (response.indexOf("200 OK") >= 0) {
          success = true;
        }
        break;
      }
    }
    
    client.stop();
    return success;
  }
  return false;
}

void setup() {
  Serial.begin(9600);
  delay(1000);
  
  Serial.println("========================================");
  Serial.println("===     АВТОМАТИЧЕСКИЙ МОНИТОРИНГ    ===");
  Serial.println("========================================");
  
  // Инициализация датчика
  Serial.print("Датчик тока... ");
  emon1.current(0, CALIBRATION);
  Serial.println("OK");
  
  // Инициализация Ethernet
  Serial.print("Ethernet... ");
  Ethernet.begin(mac, ip, dns, gateway, subnet);
  delay(1500);
  
  if (Ethernet.localIP() == IPAddress(0,0,0,0)) {
    Serial.println("ОШИБКА!");
    while(true) { delay(1000); }
  } else {
    Serial.println("OK");
  }
  
  Serial.print("Arduino IP: ");
  Serial.println(Ethernet.localIP());
  Serial.print("Сервер: ");
  Serial.print(serverIP);
  Serial.print(":");
  Serial.println(serverPort);
  Serial.println("========================================");
  Serial.println("");
  
  lastMeasureTime = millis();
}

void loop() {
  unsigned long now = millis();
  
  // Если ждем ответ от сервера
  if (isWaitingForResponse) {
    if (now >= responseTimeout) {
      isWaitingForResponse = false;
      Serial.println(F(" ТАЙМАУТ..."));
    }
    return;
  }
  
  // Опрос датчика каждую секунду
  if (now - lastMeasureTime >= MEASURE_INTERVAL) {
    lastMeasureTime = now;
    
    // Измеряем ток
    float current = emon1.calcIrms(1480);
    float power = current * VOLTAGE;
    bool currentStatus = (current > CURRENT_THRESHOLD);
    
    // Выводим сырые данные
    Serial.print(current, 2);
    Serial.print(F(" A | ["));
    Serial.print(currentStatus ? F("Work") : F("Stop"));
    Serial.print(F("]"));
    
    // Стабилизация
    if (currentStatus == lastStableStatus) {
      stableCount++;
      
      // Выводим счетчик стабилизации (но не больше NEED_STABLE_READINGS)
      int displayCount = stableCount;
      if (displayCount > NEED_STABLE_READINGS) {
        displayCount = NEED_STABLE_READINGS;
      }
      Serial.print(F(" "));
      Serial.print(displayCount);
      Serial.print(F("/"));
      Serial.print(NEED_STABLE_READINGS);
      
      // Когда набрали 3 одинаковых показания
      if (stableCount == NEED_STABLE_READINGS) {
        if (currentStatus != lastSentStatus) {
          Serial.print(F(" → Статус изменился! Отправка... "));
          if (sendData(currentStatus, current, power)) {
            Serial.print(F(" OK"));
            lastSentStatus = currentStatus;
          } else {
            Serial.print(F(" ОШИБКА!"));
          }
          isWaitingForResponse = true;
          responseTimeout = millis() + RESPONSE_WAIT;
        } else {
          Serial.print(F(" → Статус не изменился, отправка не требуется"));
        }
      } else if (stableCount > NEED_STABLE_READINGS) {
        // Уже после отправки, просто показываем что статус стабилен
        if (currentStatus == lastSentStatus) {
          Serial.print(F(" → Статус стабилен"));
        }
      }
    } else {
      // Сброс счетчика при несовпадении
      stableCount = 0;
      lastStableStatus = currentStatus;
      Serial.print(F(" Стабилизация сброшена"));
    }
    Serial.println();
  }
  
  delay(10);
}
