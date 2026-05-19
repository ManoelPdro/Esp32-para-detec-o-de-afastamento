#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// --- DEFINES ---
#define DEVICE_NAME "ESP32 SENSOR"
#define MANUFACTURER_ID 0xFFFF // ID genérico usado para o empacotamento de dados

#define SERVICE_UUID "44806210-84bc-4a2a-a9ff-92ff65f30c8c"
#define CHARACTERISTIC_BUZZER_UUID "6064f23e-7f34-49c0-a969-070cd0ebcce0"

#define BUZZER_PIN 3
#define TEMP_PIN 7
#define SDA_PIN 8
#define SCL_PIN 9

#define INTERVALO_TEMP 3000
#define INTERVALO_ACCEL 200
#define TEMPO_DURACAO_BUZZER 1500 // Buzzer toca por 1,5 segundos

// Constantes físicas e de registradores do BMI160
#define BMI160_ADDR     0x69
#define REG_CHIP_ID     0x00  // Deve retornar 0xD1
#define REG_DATA_X      0x12  // Início dos dados do acelerômetro (X_LSB)
#define REG_CMD         0x7E  // Registrador de comando

// Comandos de gerenciamento de energia internos do chip
#define CMD_ACC_NORMAL  0x11  // Liga o Acelerômetro em Modo Normal
#define CMD_GYR_SUSPEND 0x14  // Coloca o Giroscópio em Suspend (Economia)

#define LIMIAR 0.2
#define GRAVIDADE 1.0

BLECharacteristic *pCharacteristic_BUZZER = nullptr;
BLEAdvertising *pAdvertising = nullptr;

OneWire oneWire(TEMP_PIN);
DallasTemperature sensor_temp(&oneWire);
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

volatile bool deviceConnected = false; 
volatile bool comando_buzzer_recebido = false; 

bool accel_funcionando = false;

// Handles das Tasks
TaskHandle_t TaskSensoresHandle = NULL;
TaskHandle_t TaskBLEHandle = NULL;

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    portENTER_CRITICAL(&mux);
    deviceConnected = true;
    portEXIT_CRITICAL(&mux);
    Serial.println("[BLE] App Conectou! Pronto para receber comando do Buzzer.");
  }

  void onDisconnect(BLEServer *pServer) {
    portENTER_CRITICAL(&mux);
    deviceConnected = false;
    portEXIT_CRITICAL(&mux);
    Serial.println("[BLE] App Desconectou. Reiniciando Broadcast de dados...");
    
    pAdvertising->start(); 
  }
};

class CharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string rxValue = pCharacteristic->getValue();
    
    // Se o app enviar "1", ativa o sinal do buzzer
    if (rxValue.length() > 0 && rxValue[0] == '1') {
      portENTER_CRITICAL(&mux); 
      comando_buzzer_recebido = true;
      portEXIT_CRITICAL(&mux);
    } 
  }
};

void setupBLE(){
  BLEDevice::init(DEVICE_NAME);
  
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);

  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic_BUZZER = pService->createCharacteristic(
    CHARACTERISTIC_BUZZER_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pCharacteristic_BUZZER->setCallbacks(new CharacteristicCallbacks());
  pService->start();

  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  
  pAdvertising->setMinInterval(0x20); 
  pAdvertising->setMaxInterval(0x40); 
  
  pAdvertising->start();
}

// --- ATUALIZAÇÃO DOS DADOS DE BROADCAST ---
void atualizarBroadcast(float temperatura, int16_t accX, int16_t accY, int16_t accZ) {
    if (deviceConnected) return;

    BLEAdvertisementData oAdvertisementData;
    uint8_t strServiceData[10];
    
    strServiceData[0] = MANUFACTURER_ID & 0xFF;
    strServiceData[1] = (MANUFACTURER_ID >> 8) & 0xFF;
    
    int16_t tempInt = (int16_t)(temperatura * 100);
    
    strServiceData[2] = tempInt & 0xFF;
    strServiceData[3] = (tempInt >> 8) & 0xFF;
    strServiceData[4] = accX & 0xFF;
    strServiceData[5] = (accX >> 8) & 0xFF;
    strServiceData[6] = accY & 0xFF;
    strServiceData[7] = (accY >> 8) & 0xFF;
    strServiceData[8] = accZ & 0xFF;
    strServiceData[9] = (accZ >> 8) & 0xFF;

    std::string dataPayload((char*)strServiceData, 10);
    oAdvertisementData.setManufacturerData(dataPayload);
    oAdvertisementData.setName(DEVICE_NAME);
    
    pAdvertising->stop(); // Previne vazamento de memória RAM nas atualizações recorrentes
    pAdvertising->setAdvertisementData(oAdvertisementData);
    pAdvertising->start(); 
}

// Função de segurança para limpar barramento travado
void destravarI2C() {
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, OUTPUT);
  for (byte i = 0; i < 9; i++) {
    digitalWrite(SCL_PIN, HIGH); delayMicroseconds(5);
    digitalWrite(SCL_PIN, LOW); delayMicroseconds(5);
  }
  pinMode(SDA_PIN, OUTPUT);
  digitalWrite(SDA_PIN, LOW); delayMicroseconds(5);
  digitalWrite(SCL_PIN, HIGH); delayMicroseconds(5);
  digitalWrite(SDA_PIN, HIGH); delayMicroseconds(5);
}

// THREAD 1: LEITURA DOS SENSORES (CORE 1)
void TaskSensores(void *pvParameters){
  unsigned long timer_accel = 0;
  unsigned long timer_temp = 0;
  
  float ultima_temp = 0.0;
  int16_t axRaw = 0, ayRaw = 0, azRaw = 0;
  
  for(;;){
    unsigned long tempo_atual = millis();
    
    // 1. Leitura do Sensor de Temperatura Dallas
    if ((tempo_atual - timer_temp) > INTERVALO_TEMP) {
      timer_temp = tempo_atual;
      sensor_temp.requestTemperatures();
      ultima_temp = sensor_temp.getTempCByIndex(0);
      Serial.print("[TEMP] Atualizada: "); Serial.println(ultima_temp);
    }

    // 2. Leitura do Acelerômetro Manual Direta via Wire (A cada 200ms)
    if((tempo_atual - timer_accel) >= INTERVALO_ACCEL){
      timer_accel = tempo_atual;
      
      if(accel_funcionando){
        // Solicita os dados a partir do registrador base X_LSB (0x12)
        Wire.beginTransmission(BMI160_ADDR);
        Wire.write(REG_DATA_X);
        
        if (Wire.endTransmission(false) == 0) {
          int bytesRecebidos = Wire.requestFrom(BMI160_ADDR, 6);
          
          if (bytesRecebidos == 6) {
            uint8_t x_lsb = Wire.read();
            uint8_t x_msb = Wire.read();
            uint8_t y_lsb = Wire.read();
            uint8_t y_msb = Wire.read();
            uint8_t z_lsb = Wire.read();
            uint8_t z_msb = Wire.read();
            
            // Junção binária correta e segura (MSB deslocado e mascarado com LSB)
            axRaw = (int16_t)((x_msb << 8) | x_lsb);
            ayRaw = (int16_t)((y_msb << 8) | y_lsb);
            azRaw = (int16_t)((z_msb << 8) | z_lsb);

            // Processamento matemático original mantido intacto
            float accelX = axRaw / 16384.0;
            float accelY = ayRaw / 16384.0;
            float accelZ = azRaw / 16384.0;

            float magnitude_accel = sqrt(sq(accelX) + sq(accelY) + sq(accelZ));

            // Print idêntico ao solicitado para monitoramento dos limites
            Serial.print("mov x,y,z "); Serial.print(accelX); Serial.print(" "); Serial.print(accelY); Serial.print(" "); Serial.println(accelZ);
            
            if(abs(magnitude_accel - GRAVIDADE) > LIMIAR){
              Serial.println("[ACCEL] Movimento detectado acima do limiar!");
            }
          } else {
            Serial.print("[ERRO I2C] Bytes incompletos recebidos: "); Serial.println(bytesRecebidos);
          }
        } else {
          Serial.println("[ERRO I2C] Barramento inacessível. Tentando restaurar...");
          destravarI2C();
        }
      } 
      // Transmite as variáveis brutas (raw) convertidas para int16_t via payload estruturado
      atualizarBroadcast(ultima_temp, axRaw, ayRaw, azRaw);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
  
// ==============================================
// THREAD 2: COMANDOS DO BUZZER (CORE 0)
// ==============================================
void TaskBLE(void *pvParameters){
  unsigned long timer_inicio_buzzer = 0;
  bool buzzer_esta_tocando = false;
  
  for(;;){
    unsigned long tempo_atual = millis();

    portENTER_CRITICAL(&mux);
    bool local_buzzer = comando_buzzer_recebido;
    comando_buzzer_recebido = false;
    portEXIT_CRITICAL(&mux);

    if (local_buzzer) {
      digitalWrite(BUZZER_PIN, LOW); 
      timer_inicio_buzzer = tempo_atual;
      buzzer_esta_tocando = true;
    }

    if (buzzer_esta_tocando) {
      if (tempo_atual - timer_inicio_buzzer >= TEMPO_DURACAO_BUZZER) {
        digitalWrite(BUZZER_PIN, HIGH); 
        buzzer_esta_tocando = false;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// --- SETUP PRINCIPAL ---
void setup() {
  Serial.begin(115200);
  delay(1000); // Janela para estabilizar a Serial
  
  destravarI2C(); // Limpa pinos antes de associar ao Wire

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000); // Frequência inicial robusta de 100kHz
  delay(100);
  
  sensor_temp.begin();
  sensor_temp.setWaitForConversion(false); 
  
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH); 

  setupBLE();

  // Verificação de Conexão Física Direta com o Chip (Sem depender de Libs)
  Wire.beginTransmission(BMI160_ADDR);
  Wire.write(REG_CHIP_ID);
  
  if(Wire.endTransmission(false) == 0) {
    Wire.requestFrom(BMI160_ADDR, 1);
    if(Wire.available() == 1 && Wire.read() == 0xD1) {
      
      // CONFIGURAÇÃO SEGUIDA DE ENERGIA DO CHIP:
      // 1. Liga o Acelerômetro (Modo Normal)
      Wire.beginTransmission(BMI160_ADDR);
      Wire.write(REG_CMD);
      Wire.write(CMD_ACC_NORMAL);
      Wire.endTransmission();
      delay(50); // Delay obrigatório exigido pelo Datasheet (3.8ms a 50ms)

      // 2. Desliga/Suspende o Giroscópio conforme sua lógica original
      Wire.beginTransmission(BMI160_ADDR);
      Wire.write(REG_CMD);
      Wire.write(CMD_GYR_SUSPEND);
      Wire.endTransmission();
      delay(50);

      accel_funcionando = true;
      Serial.println("[SUCESSO] BMI160 acordado e configurado via Wire puro.");
    } else {
      Serial.println("[ERRO CRÍTICO] Chip ID inválido retornado pelo BMI160.");
    }
  } else {
    Serial.println("[AVISO] Acelerômetro BMI160 não respondeu no endereço 0x69.");
  }

  // Criação das Tasks em seus Cores nativos (Mantido sem alterações)
  xTaskCreatePinnedToCore(
    TaskBLE,
    "TaskBLE",
    4096,
    NULL,
    2,
    &TaskBLEHandle,
    0
  );

  xTaskCreatePinnedToCore(
    TaskSensores,
    "TaskSensores",
    4096,
    NULL,
    1,
    &TaskSensoresHandle,
    1
  );
}

void loop() {
  vTaskDelete(NULL); 
}