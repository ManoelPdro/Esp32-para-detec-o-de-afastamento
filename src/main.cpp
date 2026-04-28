#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2901.h>
#include <BLE2902.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "BMI160Gen.h"

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
#define COUNTDOWN_MOVIMENTO 5000 // Para verificar após 5 segundos se a criança parou de se mexer

// CONFIGURAÇÃO DA HISTERESE (VARIAÇÃO MÍNIMA)
#define TEMP_ESPERADA 33 // Espera que a temperatura da criança está por volta de 33 graus (ajuste esse valor e a variação limite caso necessário)
#define TEMP_VARIACAO_LIMITE 2 // Caso a temperatura varie 2 graus ou mais pode ser que a criança tenha removido e o esp envia o sinal para o app

// Endereços e constantes BMI160
#define BMI160_ADDR 0x69
#define REG_CMD 0x7E
#define CMD_GYR_SUSPEND 0x14
#define LIMIAR 0.2
#define GRAVIDADE 1.0

// --- OBJETOS GLOBAIS ---
BLECharacteristic *pCharacteristic_BUZZER;
BLECharacteristic *pCharacteristic_TEMP;
BLECharacteristic *pCharacteristic_BMI160;
BLEAdvertising *pAdvertising;

OneWire oneWire(TEMP_PIN);
DallasTemperature sensor_temp(&oneWire);

// Variáveis de Dados
bool crianca_movimento = false; // Define se a criança está se mexendo
bool pulseira_removida = false; // Indica se a criança ainda está com a pulseira

// Flags de Estado
bool accel_funcionando = false;
bool deviceConnected = false; 

// Timers
unsigned long timer_accel = 0;
unsigned long timer_temp = 0;
unsigned long timer_ultimo_movimento = 0;

// Variáveis para o Buzzer não-bloqueante
unsigned long timer_inicio_buzzer = 0;
bool buzzer_esta_tocando = false; 
bool comando_buzzer_recebido = false; 


// --- CALLBACKS BLE ---
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    deviceConnected = true;
    digitalWrite(LED_PIN, HIGH);
  }

  void onDisconnect(BLEServer *pServer) {
    deviceConnected = false;
    digitalWrite(LED_PIN, LOW);
    pAdvertising->start(); 
  }
};

class CharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string rxValue = std::string(pCharacteristic->getValue().c_str());
    if (rxValue.length() > 0 && rxValue.substr(0, 1) == "1") {
       comando_buzzer_recebido = true;
    } 
  }
};

// --- FUNÇÃO DO BMI160 ---

void readAccel(){
  int axRaw, ayRaw, azRaw;
  float accelX, accelY, accelZ;
  float magnitude_accel;

  BMI160.readAccelerometer(axRaw, ayRaw, azRaw);

  accelX = axRaw/16384.0;
  accelY = ayRaw/16384.0;
  accelZ = azRaw/16384.0;

  magnitude_accel = sqrt(sq(accelX) + sq(accelY) + sq(accelZ));

  if(abs(magnitude_accel - GRAVIDADE) > LIMIAR){
    timer_ultimo_movimento = millis(); // Pega o último momento que a criança se mexeu
    if(!crianca_movimento){
      crianca_movimento = true;
      if(deviceConnected){
        pCharacteristic_BMI160->setValue((uint8_t*)&crianca_movimento, 1); 
        pCharacteristic_BMI160->notify();
      }
    }
  }
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
  if(!BMI160.begin(BMI160GenClass::I2C_MODE, BMI160_ADDR)){
    //Informar ao app que o acelerômetro não iniciou
    while(1);
  }
  else{
    accel_funcionando = true;
  }

  // Define o giroscópio como desativado (economia de energia)
  BMI160.setAccelerometerRange(2);
  Wire.beginTransmission(BMI160_ADDR);
  Wire.write(REG_CMD);
  Wire.write(CMD_GYR_SUSPEND);
  Wire.endTransmission();

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
  pDescritor_TEMP->setDescription("Indicador da pulseira no braço");
  pDescritor_BMI160->setDescription("Indicador de movimento");

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


  if ((tempo_atual - timer_temp) > INTERVALO_TEMP) {
    timer_temp = tempo_atual;    
    sensor_temp.requestTemperatures(); 
    float t_lida = sensor_temp.getTempCByIndex(0);    
    if (abs(t_lida - TEMP_ESPERADA) >= TEMP_VARIACAO_LIMITE) { 
      if(!pulseira_removida){
        pulseira_removida = true;
        if(deviceConnected){
          pCharacteristic_TEMP->setValue((uint8_t*)&pulseira_removida, 1);
          pCharacteristic_TEMP->notify();
        }
      }
    }
    else if(pulseira_removida){
      pulseira_removida = false;
      if(deviceConnected){
        pCharacteristic_TEMP->setValue((uint8_t*)&pulseira_removida, 1);
        pCharacteristic_TEMP->notify();
      }
    }
  }


  // --- Verificação se a criança continua em movimento ---
  if(crianca_movimento){
    if((tempo_atual - timer_ultimo_movimento) >= COUNTDOWN_MOVIMENTO){
        crianca_movimento = false;
        if(deviceConnected){
          pCharacteristic_BMI160->setValue((uint8_t*)&crianca_movimento, 1);
          pCharacteristic_BMI160->notify();
        }
    }
  }

  // --- Leitura Accel ---
  if ((tempo_atual - timer_accel) >= INTERVALO_ACCEL) {
    timer_accel = tempo_atual;
    if(accel_funcionando) {
      readAccel();
    }
  }
}