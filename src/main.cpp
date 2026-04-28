#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2901.h>
#include <BLE2902.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "esp_bt.h"

// --- DEFINES ---
#define DEVICE_NAME "ESP32 SENSOR"
#define SERVICE_UUID "44806210-84bc-4a2a-a9ff-92ff65f30c8c"
#define CHARACTERISTIC_BUZZER_UUID "6064f23e-7f34-49c0-a969-070cd0ebcce0"
#define CHARACTERISTIC_TEMP_UUID "bd072c9e-1f6b-4a84-b8aa-7b55ddc0d986"
#define CHARACTERISTIC_BMI160_UUID "e2770502-84a8-49fd-b716-1f9ade3e4ea9"

#define BUZZER_PIN 18
#define LED_PIN 2
#define TEMP_PIN 4

#define INTERVALO_TEMP 3000    
#define INTERVALO_ACCEL 200    
#define TEMPO_DURACAO_BUZZER 1500 

// CONFIGURAÇÃO DA HISTERESE (VARIAÇÃO MÍNIMA)
#define TEMP_VARIACAO_LIMITE 0.5 // Só atualiza se mudar 0.5 graus ou mais

// Endereços BMI160
#define BMI160_ADDR 0x69 
#define REG_ACCEL_DATA 0x12 
#define REG_CMD        0x7E 
#define CMD_ACCEL_NORMAL 0x11 
#define CMD_SOFT_RESET   0xB6

// --- OBJETOS GLOBAIS ---
BLECharacteristic *pCharacteristic_BUZZER;
BLECharacteristic *pCharacteristic_TEMP;
BLECharacteristic *pCharacteristic_BMI160;
BLEAdvertising *pAdvertising;

OneWire oneWire(TEMP_PIN);
DallasTemperature sensor_temp(&oneWire);

// Variáveis de Dados
int16_t accel_data[3]; 
float temperatura_atual = -999.0; // Valor inicial impossível para forçar a 1ª atualização

// Flags de Estado
bool accel_funcionando = false;
bool deviceConnected = false; 

// Timers
unsigned long timer_accel = 0;
unsigned long timer_temp = 0;

// Variáveis para o Buzzer não-bloqueante
unsigned long timer_inicio_buzzer = 0;
bool buzzer_esta_tocando = false; 
bool comando_buzzer_recebido = false; 

// --- FUNÇÕES I2C BMI160 ---
void writeRegister(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(BMI160_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void readAccelRaw() {
  Wire.beginTransmission(BMI160_ADDR);
  Wire.write(REG_ACCEL_DATA);
  Wire.endTransmission();
  Wire.requestFrom(BMI160_ADDR, 6);
  
  if (Wire.available() == 6) {
    uint8_t x_lsb = Wire.read(); uint8_t x_msb = Wire.read();
    uint8_t y_lsb = Wire.read(); uint8_t y_msb = Wire.read();
    uint8_t z_lsb = Wire.read(); uint8_t z_msb = Wire.read();
    accel_data[0] = (x_msb << 8) | x_lsb;
    accel_data[1] = (y_msb << 8) | y_lsb;
    accel_data[2] = (z_msb << 8) | z_lsb;
  }
}

// --- CALLBACKS BLE ---
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    deviceConnected = true;
    digitalWrite(LED_PIN, HIGH);
    Serial.println("Dispositivo Conectado!");
  }

  void onDisconnect(BLEServer *pServer) {
    deviceConnected = false;
    digitalWrite(LED_PIN, LOW);
    Serial.println("Dispositivo Desconectado.");
    pAdvertising->start(); 
  }
};

class CharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string rxValue = std::string(pCharacteristic->getValue().c_str());
    if (rxValue.length() > 0 && rxValue.substr(0, 1) == "1") {
       comando_buzzer_recebido = true;
       Serial.println("Comando Buzzer Recebido!");
    } 
  }
};

// --- FUNÇÃO DE BROADCAST ---
void updateBroadcastData() {
  if (deviceConnected) return; 

  BLEAdvertisementData oAdvertisementData = BLEAdvertisementData();
  oAdvertisementData.setFlags(0x04);
  oAdvertisementData.setName(DEVICE_NAME);

  uint8_t payload[10]; 

  payload[0] = 0xFF; 
  payload[1] = 0xFF;

  // Usa a temperatura_atual (que agora só muda se a variação for grande)
  int16_t tempInt = (int16_t)(temperatura_atual * 100);

  payload[2] = tempInt & 0xFF;
  payload[3] = (tempInt >> 8) & 0xFF;

  payload[4] = accel_data[0] & 0xFF;
  payload[5] = (accel_data[0] >> 8) & 0xFF;

  payload[6] = accel_data[1] & 0xFF;
  payload[7] = (accel_data[1] >> 8) & 0xFF;

  payload[8] = accel_data[2] & 0xFF;
  payload[9] = (accel_data[2] >> 8) & 0xFF;

  String strData = "";
  for (int i = 0; i < 10; i++) {
    strData += (char)payload[i];
  }

  oAdvertisementData.setManufacturerData(strData);
  
  pAdvertising->stop();
  pAdvertising->setAdvertisementData(oAdvertisementData);
  pAdvertising->start();
}

void setup() {
  Serial.begin(9600);
  Wire.begin();  
  
  sensor_temp.begin();
  sensor_temp.setWaitForConversion(false); 
  
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH); 

  // Configuração BMI160
  writeRegister(REG_CMD, CMD_SOFT_RESET);
  delay(100); 
  Wire.beginTransmission(BMI160_ADDR);
  if(Wire.endTransmission() == 0) {
      Serial.println("BMI160 OK!");
      accel_funcionando = true;
      writeRegister(REG_CMD, CMD_ACCEL_NORMAL);
      delay(100); 
  } else {
      Serial.println("ERRO BMI160");
  }

  // Configuração BLE
  BLEDevice::init(DEVICE_NAME);
  
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);

  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  //Caracteristicas
  pCharacteristic_BUZZER = pService->createCharacteristic(
    CHARACTERISTIC_BUZZER_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pCharacteristic_TEMP = pService->createCharacteristic(
    CHARACTERISTIC_TEMP_UUID, 
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic_BMI160 = pService->createCharacteristic(
    CHARACTERISTIC_BMI160_UUID, 
    BLECharacteristic::PROPERTY_NOTIFY
  );

  pCharacteristic_BUZZER->setCallbacks(new CharacteristicCallbacks());

  // descritores
  BLE2901 *pDescritor_BUZZER = new BLE2901();
  BLE2901 *pDescritor_TEMP = new BLE2901();
  BLE2901 *pDescritor_BMI160 = new BLE2901();

  BLE2902 *pDescritor_TEMP_subscribe = new BLE2902();
  BLE2902 *pDescritor_BMI160_subscribe = new BLE2902();

  pDescritor_BUZZER->setDescription("Buzzer");
  pDescritor_TEMP->setDescription("Temperatura");
  pDescritor_BMI160->setDescription("Acelerômetro");

  pCharacteristic_BUZZER->addDescriptor(pDescritor_BUZZER);
  pCharacteristic_TEMP->addDescriptor(pDescritor_TEMP);
  pCharacteristic_BMI160->addDescriptor(pDescritor_BMI160);
  pCharacteristic_TEMP->addDescriptor(pDescritor_TEMP_subscribe);
  pCharacteristic_BMI160->addDescriptor(pDescritor_BMI160_subscribe);

  pService->start();

  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinInterval(0x20); 
  pAdvertising->setMaxInterval(0x20); 

  pAdvertising->start();
  Serial.println("BLE Iniciado...");
}

void loop() {
  unsigned long tempo_atual = millis();

  // --- Lógica Buzzer ---
  if (comando_buzzer_recebido) {
    digitalWrite(BUZZER_PIN, LOW); 
    timer_inicio_buzzer = tempo_atual;
    buzzer_esta_tocando = true;
    comando_buzzer_recebido = false; 
  }

  if (buzzer_esta_tocando) {
    if (tempo_atual - timer_inicio_buzzer >= TEMPO_DURACAO_BUZZER) {
      digitalWrite(BUZZER_PIN, HIGH); 
      buzzer_esta_tocando = false;
      Serial.println("Buzzer finalizado.");
    }
  }


  if ((tempo_atual - timer_temp) >= INTERVALO_TEMP) {
    timer_temp = tempo_atual;    
    sensor_temp.requestTemperatures(); 
    float t_lida = sensor_temp.getTempCByIndex(0);    
    if(t_lida > -100) {
      if (abs(t_lida - temperatura_atual) >= TEMP_VARIACAO_LIMITE) {
          temperatura_atual = t_lida;       
          pCharacteristic_TEMP->setValue((uint8_t*)&temperatura_atual, sizeof(float));
          pCharacteristic_TEMP->notify();
      }
    }
  }

  // --- Leitura Accel e Envio do Broadcast ---
  if ((tempo_atual - timer_accel) >= INTERVALO_ACCEL) {
    timer_accel = tempo_atual;
    if(accel_funcionando) {
      readAccelRaw();
    }
    updateBroadcastData();
  }
}