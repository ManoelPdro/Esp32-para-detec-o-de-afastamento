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

#define BUZZER_PIN 4
#define LED_PIN 8
#define TEMP_PIN 2
#define SDA_PIN 5
#define SCL_PIN 6

#define INTERVALO_TEMP 3000 // Lê a temperatura a cada 3 segundos (talvez seja melhor diminuir esse tempo, em 3 segundos a criança já tirou e correu 3 quarteirões e morreu atropelada por 3 motoboys)    
#define INTERVALO_ACCEL 200    
#define TEMPO_DURACAO_BUZZER 1500 // Buzzer toca por 1,5 segundos
#define COUNTDOWN_MOVIMENTO 5000 // Para verificar após 5 segundos se a criança parou de se mexer (pode diminuir o tempo se achar pertinente)

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

// --- VARIÁVEIS GLOBAIS ---
// Mux para proteger as variáveis compartilhadas
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Variáveis de Dados compartilhadas pelas threads
volatile bool crianca_movimento = false; // Define se a criança está se mexendo
volatile bool pulseira_removida = false; // Indica se a criança ainda está com a pulseira
volatile bool deviceConnected = false; 
volatile bool comando_buzzer_recebido = false; 

// Flags de notificação, para a thread dos sensores avisar à thread ble para enviar os dados
volatile bool flag_notify_temp = false;
volatile bool flag_notify_bmi = false;

// Flags de Estado
bool accel_funcionando = false;

// Handles
TaskHandle_t TaskSensoresHandle;
TaskHandle_t TaskBLEHandle;

// --- CALLBACKS BLE ---
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    portENTER_CRITICAL(&mux);
    deviceConnected = true;
    portEXIT_CRITICAL(&mux);
    digitalWrite(LED_PIN, HIGH); // Pode remover essa linha para economizar bateria ou caso o professor remova o processador do esp da placa, remova também o define do LED_PIN
    vTaskResume(TaskSensoresHandle);
  }

  void onDisconnect(BLEServer *pServer) {
    portENTER_CRITICAL(&mux);
    deviceConnected = false;
    portEXIT_CRITICAL(&mux);
    digitalWrite(LED_PIN, LOW); // Pode remover essa linha também pelo mesmo motivo da anotação anterior
    vTaskSuspend(TaskSensoresHandle);
    pAdvertising->start(); 
  }
};

class CharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string rxValue = std::string(pCharacteristic->getValue().c_str());
    if (rxValue.length() > 0 && rxValue.substr(0, 1) == "1") {
      portENTER_CRITICAL(&mux); 
      comando_buzzer_recebido = true;
      portEXIT_CRITICAL(&mux);
    } 
  }
};

// --- FUNÇÕES AUXILIARES ---
void setupBLE(){
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
  pAdvertising->setMinInterval(0x100); 
  pAdvertising->setMaxInterval(0x200); // tempo aumentado para economizar bateria, diminuam caso queiram testar se afeta o RSSI

  pAdvertising->start();
}

// ==============================================
// THREAD 1: LEITURA DOS SENSORES (CORE 1)
// ==============================================
void TaskSensores(void *pvParameters){
  unsigned long timer_accel = 0;
  unsigned long timer_temp = 0;
  unsigned long timer_ultimo_movimento = 0;
  
  //loop infinito da thread
  for(;;){
    unsigned long tempo_atual = millis();
    
    portENTER_CRITICAL(&mux);
    bool local_pulseira = pulseira_removida;
    bool local_movimento = crianca_movimento;
    portEXIT_CRITICAL(&mux);

    //leitura da temperatura
    if ((tempo_atual - timer_temp) > INTERVALO_TEMP) {
      timer_temp = tempo_atual;    
      sensor_temp.requestTemperatures(); 
      float t_lida = sensor_temp.getTempCByIndex(0); 
      
      if (abs(t_lida - TEMP_ESPERADA) >= TEMP_VARIACAO_LIMITE) { 
        if(!local_pulseira){
          portENTER_CRITICAL(&mux);
          pulseira_removida = true;
          flag_notify_temp = true; // Avisa a thread BLE para atualizar o status da criança estar ou não com a pulseira
          portEXIT_CRITICAL(&mux);
        }
      }
      else if(local_pulseira){
        portENTER_CRITICAL(&mux);
        pulseira_removida = false;
        flag_notify_temp = true;
        portEXIT_CRITICAL(&mux);
      }
    }

    // leitura do acelerômetro
    if((tempo_atual - timer_accel) >= INTERVALO_ACCEL){
      timer_accel = tempo_atual;
      if(accel_funcionando){
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
          if(!local_movimento){
            portENTER_CRITICAL(&mux);
            crianca_movimento = true;
            flag_notify_bmi = true; // Avisa a thread BLE para indicar que a criança se mexeu
            portEXIT_CRITICAL(&mux);
          }
        }
      } 
    }
    
    if(local_movimento){
      if((millis() - timer_ultimo_movimento) >= COUNTDOWN_MOVIMENTO){
        portENTER_CRITICAL(&mux);
        crianca_movimento = false;
        flag_notify_bmi = true;
        portEXIT_CRITICAL(&mux);
      }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}
  
// ==============================================
// THREAD 2: BLE E ATUADORES (CORE 0)
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

    // Lógica do buzzer
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

    portENTER_CRITICAL(&mux);
    bool local_connected = deviceConnected;
    portEXIT_CRITICAL(&mux);

    // Envio das notificações via BLE
    if(local_connected){
      portENTER_CRITICAL(&mux);
      bool local_flag_temp = flag_notify_temp;
      bool local_pulseira = pulseira_removida;
      flag_notify_temp = false;
      portEXIT_CRITICAL(&mux);

      if(local_flag_temp){
        uint8_t confirmacao_remocao = local_pulseira ? 1 : 0;
        pCharacteristic_TEMP->setValue(&confirmacao_remocao, 1);
        pCharacteristic_TEMP->notify();
      }
      portENTER_CRITICAL(&mux);
      bool local_flag_bmi = flag_notify_bmi;
      bool local_movimento = crianca_movimento;
      flag_notify_bmi = false;
      portEXIT_CRITICAL(&mux);

      if(local_flag_bmi){
        uint8_t confirmacao_movimento = local_movimento ? 1 : 0; 
        pCharacteristic_BMI160->setValue(&confirmacao_movimento, 1);
        pCharacteristic_BMI160->notify();
      }
    }
    else{
      // Limpa as flags caso disconecte para não ficar enviando sem necessidade
      portENTER_CRITICAL(&mux);
      flag_notify_temp = false;
      flag_notify_bmi = false;
      portEXIT_CRITICAL(&mux);
    }

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}


void setup() {
  Serial.begin(9600);
  Wire.begin(SDA_PIN, SCL_PIN);  
  
  sensor_temp.begin();
  sensor_temp.setWaitForConversion(false); 
  
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH); 

  setupBLE();

  // Configuração BMI160
  if(!BMI160.begin(BMI160GenClass::I2C_MODE, BMI160_ADDR)){
    //Informar ao app que o acelerômetro não iniciou
  }
  else{
    accel_funcionando = true;
    
    // Define o giroscópio como desativado (economia de energia)
    BMI160.setAccelerometerRange(2);
    Wire.beginTransmission(BMI160_ADDR);
    Wire.write(REG_CMD);
    Wire.write(CMD_GYR_SUSPEND);
    Wire.endTransmission();
  }

  // Criação das threads de sensores e BLE 
  xTaskCreate(
    TaskSensores,    // Função da task
    "TaskSensores",  // Nome da task
    8192,          // Tamanho da pilha (stack)
    NULL,            // Parâmetros passados (no caso, nenhum)
    1,               // Prioridade (baixa)
    &TaskSensoresHandle
  );

  xTaskCreate(
    TaskBLE,
    "TaskBLE",
    6144,
    NULL,
    1,
    &TaskBLEHandle
  );
}

void loop() {
  vTaskDelete(NULL); // Deleta a thread loop pois consumiria memória a toa
}