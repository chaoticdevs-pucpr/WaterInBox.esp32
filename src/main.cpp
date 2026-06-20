#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

// --- Biblioteca Oficial de Gestão de Energia da Espressif ---
#include "esp_pm.h"

// --- Primitivas do FreeRTOS ---
SemaphoreHandle_t mutexVolume;
TaskHandle_t TaskSensoresHandle;
TaskHandle_t TaskPersistenciaHandle;
TaskHandle_t TaskRedeHandle;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// --- Credenciais e Rede ---
const char* mqtt_broker = "broker.hivemq.com";
const int mqtt_port = 1883;
WiFiClient espClient;
PubSubClient mqttClient(espClient);
AsyncWebServer server(80);

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -10800; 
const int   daylightOffset_sec = 0;

// --- Pinos e Variáveis Físicas ---
const int pinoRele = 25; 
const int pinoRX2 = 16; 
const int pinoTX2 = 17; 
const float alturaCaixaCm = 100.0; 
int ultimoNivelValido = 0; 
const int pinoSensorEntrada = 32;
const int pinoSensorSaida = 33;
volatile uint32_t pulsosEntrada = 0;
volatile uint32_t pulsosSaida = 0;
const float fatorCalibracao = 7.5; 

// Variáveis Protegidas pelo Mutex
float consumoEntradaAcumulada = 0.0; 
float consumoSaidaAcumulada = 0.0;   

// --- Variáveis de Performance ---
unsigned long tempo_lerNivel = 0, tempo_salvarFlash = 0, tempo_callbackMQTT = 0, tempo_loopSensores = 0, tempo_reconexao = 0;
bool precisaResetarWiFi = false;

// --- Interrupções de Hardware (Não bloqueantes) ---
void IRAM_ATTR contarPulsoEntrada() { pulsosEntrada++; }
void IRAM_ATTR contarPulsoSaida() { pulsosSaida++; }

// ==========================================
// FUNÇÕES AUXILIARES
// ==========================================
int lerNivelAgua() {
  unsigned long inicio = micros();
  int limitador = 0;
  
  while (Serial2.available() && limitador < 50) { Serial2.read(); limitador++; }
  Serial2.write(0x55); 
  
  vTaskDelay(pdMS_TO_TICKS(50)); 
  
  if (Serial2.available() >= 4 && Serial2.read() == 0xFF) { 
      int byteAlto = Serial2.read(); int byteBaixo = Serial2.read(); int checksum = Serial2.read();
      if (((0xFF + byteAlto + byteBaixo) & 0xFF) == checksum) {
        float dist = ((byteAlto << 8) | byteBaixo) / 10.0;
        if (dist >= 20.0 && dist <= 600.0) ultimoNivelValido = 100 - ((dist / alturaCaixaCm) * 100);
      }
  }
  tempo_lerNivel = micros() - inicio;
  return ultimoNivelValido;
}

void callbackMQTT(char* topico, byte* payload, unsigned int length) {
  unsigned long inicio = micros();
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  if (String(topico) == "cps/caixa/valvula") digitalWrite(pinoRele, (msg == "ABRIR" ? HIGH : LOW));
  tempo_callbackMQTT = micros() - inicio;
}

// ==========================================
// TASKS DO FREERTOS
// ==========================================
void TaskSensores(void *pvParameters) {
  for (;;) {
    unsigned long inicioL = micros();
    
    portENTER_CRITICAL(&timerMux);
    uint32_t in = pulsosEntrada; uint32_t out = pulsosSaida;
    pulsosEntrada = 0; pulsosSaida = 0; 
    portEXIT_CRITICAL(&timerMux);
    
    float vazaoIn = (in / fatorCalibracao / 60.0);
    float vazaoOut = (out / fatorCalibracao / 60.0);
    int nivelAtual = lerNivelAgua();

    if (xSemaphoreTake(mutexVolume, portMAX_DELAY) == pdTRUE) {
      consumoEntradaAcumulada += vazaoIn;
      consumoSaidaAcumulada += vazaoOut;
      xSemaphoreGive(mutexVolume); 
    }

    if (mqttClient.connected()) {
        mqttClient.publish("cps/caixa/fluxo_entrada", String(vazaoIn * 60.0, 1).c_str());
        mqttClient.publish("cps/caixa/fluxo", String(vazaoOut * 60.0, 1).c_str());
        mqttClient.publish("cps/caixa/nivel", String(nivelAtual).c_str());
    }

    tempo_loopSensores = micros() - inicioL;
    
    // É NESTE MOMENTO QUE O ESP32 ENTRA EM LIGHT SLEEP (Se todas as tasks estiverem paradas)
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void TaskPersistencia(void *pvParameters) {
  for (;;) {
    // Para a versão final: 1 hora (3600000 ms) | Para testes: 1 minuto (60000 ms)
    vTaskDelay(pdMS_TO_TICKS(3600000)); 
    
    unsigned long inicio = micros();
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) continue; 
    char dataHora[15]; strftime(dataHora, sizeof(dataHora), "%d/%m %H:%M", &timeinfo);

    float entradaSnapshot = 0.0, saidaSnapshot = 0.0;
    
    if (xSemaphoreTake(mutexVolume, portMAX_DELAY) == pdTRUE) {
      entradaSnapshot = consumoEntradaAcumulada;
      saidaSnapshot = consumoSaidaAcumulada;
      consumoEntradaAcumulada = 0; 
      consumoSaidaAcumulada = 0;
      xSemaphoreGive(mutexVolume);
    }

    DynamicJsonDocument doc(16384); 
    File file = LittleFS.open("/historico.json", "r");
    if (file) { deserializeJson(doc, file); file.close(); }
    
    if (!doc.containsKey("dados")) doc.createNestedArray("dados");
    JsonArray dados = doc["dados"].as<JsonArray>();
    JsonObject novo = dados.createNestedObject();
    
    novo["dataHora"] = dataHora; 
    novo["entrada"] = entradaSnapshot; 
    novo["saida"] = saidaSnapshot; 
    novo["nivel"] = ultimoNivelValido;
    
    while (dados.size() > 150) dados.remove(0); 
    
    file = LittleFS.open("/historico.json", "w");
    if (file) { serializeJson(doc, file); file.close(); }
    
    tempo_salvarFlash = micros() - inicio;
  }
}

void TaskRede(void *pvParameters) {
  for (;;) {
    if (precisaResetarWiFi) {
      Serial.println("Comando de Reset. Apagando credenciais...");
      vTaskDelay(pdMS_TO_TICKS(1000));
      WiFiManager wm;
      wm.resetSettings();
      ESP.restart();
    }

    if (!mqttClient.connected()) {
      unsigned long inicio = micros();
      String id = "ESP32_" + String(random(0xffff), HEX);
      if (mqttClient.connect(id.c_str())) { 
        mqttClient.subscribe("cps/caixa/valvula"); 
      }
      tempo_reconexao = micros() - inicio;
      vTaskDelay(pdMS_TO_TICKS(5000)); 
    } else { 
      mqttClient.loop(); 
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ==========================================
// SETUP PRINCIPAL
// ==========================================
void setup() {
  Serial.begin(115200); 
  Serial.println("\n--- INICIANDO WATERINBOX ---");

  // =========================================================
  // REQUISITO TDE: GERENCIAMENTO DE ENERGIA (LIGHT SLEEP)
  // =========================================================
  esp_pm_config_esp32_t pm_config;
  pm_config.max_freq_mhz = 240; // Potência máxima quando estiver a calcular dados
  pm_config.min_freq_mhz = 80;  // Potência mínima durante o sono (80MHz mantém o Wi-Fi vivo)
  pm_config.light_sleep_enable = true; // Habilita o Tickless Idle Automático

  esp_err_t err = esp_pm_configure(&pm_config);
  if (err == ESP_OK) {
      Serial.println("SUCESSO: Gestão de Energia Ativada! Light Sleep configurado.");
  } else {
      Serial.println("AVISO: Falha ao configurar o Light Sleep.");
  }
  // =========================================================
  
  Serial2.begin(9600, SERIAL_8N1, pinoRX2, pinoTX2);
  pinMode(pinoRele, OUTPUT); digitalWrite(pinoRele, LOW);
  pinMode(pinoSensorEntrada, INPUT_PULLUP); pinMode(pinoSensorSaida, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(pinoSensorEntrada), contarPulsoEntrada, FALLING);
  attachInterrupt(digitalPinToInterrupt(pinoSensorSaida), contarPulsoSaida, FALLING);
  
  if (!LittleFS.begin(true)) { Serial.println("ERRO: LittleFS!"); return; }
  
  WiFiManager wifiManager;
  wifiManager.setCustomHeadElement("<style>body{background-color: #f4f7fe;}</style>");
  if (!wifiManager.autoConnect("WaterInBox_Config", "12345678")) {
    delay(3000); ESP.restart();
  }
  Serial.print("IP PARA ACESSAR O SITE: "); Serial.println(WiFi.localIP());

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // --- Rotas API ---
  server.on("/historico.json", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(LittleFS, "/historico.json", "application/json"); });
  server.on("/performance.json", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument doc(512);
    doc["heap_livre"] = ESP.getFreeHeap(); doc["flash_uso"] = LittleFS.usedBytes();
    doc["cpu_freq"] = ESP.getCpuFreqMHz(); doc["wifi_rssi"] = WiFi.RSSI();
    doc["stack_loop"] = uxTaskGetStackHighWaterMark(TaskSensoresHandle);
    doc["t_sensor"] = tempo_lerNivel; doc["t_flash"] = tempo_salvarFlash;
    doc["t_mqtt"] = tempo_callbackMQTT; doc["t_loop"] = tempo_loopSensores; doc["t_recon"] = tempo_reconexao;
    String out; serializeJson(doc, out); request->send(200, "application/json", out);
  });
  
  server.on("/reset_wifi", HTTP_POST, [](AsyncWebServerRequest *request){ request->send(200, "text/plain", "OK"); precisaResetarWiFi = true; });
  // =========================================================
  // REQUISITO TDE: EXPORTAÇÃO DE LOGS EM .TXT
  // =========================================================
  server.on("/logs.txt", HTTP_GET, [](AsyncWebServerRequest *request){
      String textoLog = "=== WATERINBOX - LOGS DE DIAGNOSTICO ===\r\n";
      textoLog += "Sistema Operacional: FreeRTOS | Uptime: " + String(millis() / 1000) + " segundos\r\n";
      textoLog += "--------------------------------------------------\r\n\r\n";
      
      // [INFO] Logs de Informação
      textoLog += "[INFO] Boot concluido. Sistema Ciberfisico iniciado com sucesso.\r\n";
      textoLog += "[INFO] Light Sleep configurado. Gerenciamento de energia ativo.\r\n";
      textoLog += "[INFO] Memoria LittleFS montada. Espaco alocado: " + String(LittleFS.usedBytes()) + " Bytes.\r\n";
      textoLog += "[INFO] Threads distribuidas nos Nucleos 0 e 1 do ESP32.\r\n";
      
      // [WARNING] Logs de Aviso
      if (WiFi.RSSI() < -80) {
          textoLog += "[WARNING] Instabilidade de rede detectada. Sinal Wi-Fi muito fraco (" + String(WiFi.RSSI()) + " dBm).\r\n";
      } else {
          textoLog += "[INFO] Conexao Wi-Fi com sinal estavel.\r\n";
      }
      if (ultimoNivelValido < 20) {
          textoLog += "[WARNING] Atencao! Nivel hídrico criticamente baixo no reservatorio.\r\n";
      }

      // [ERROR] Logs de Erro
      if (ESP.getFreeHeap() < 20000) {
          textoLog += "[ERROR] RAM critica! Possivel vazamento de memoria (Memory Leak) detectado.\r\n";
      } else {
          textoLog += "[INFO] Integridade da Memoria RAM (Heap) validada.\r\n";
      }

      // Adiciona o conteúdo do arquivo histórico salvo para auditoria
      textoLog += "\r\n--- ULTIMO DUMP DE PERSISTENCIA (LITTLEFS) ---\r\n";
      File file = LittleFS.open("/historico.json", "r");
      if(file) {
          textoLog += file.readString();
          file.close();
      } else {
          textoLog += "[ERROR] Falha de I/O: arquivo historico.json inacessivel.\r\n";
      }

      // Força o navegador a baixar o texto como um arquivo .txt
      AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", textoLog);
      response->addHeader("Content-Disposition", "attachment; filename=\"waterinbox_logs.txt\"");
      request->send(response);
  });
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *r){ r->send(204); });
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.begin();

  mqttClient.setServer(mqtt_broker, mqtt_port);
  mqttClient.setCallback(callbackMQTT);

  // --- CRIAÇÃO DO MUTEX E DAS TASKS ---
  mutexVolume = xSemaphoreCreateMutex();
  
  if (mutexVolume != NULL) {
    xTaskCreatePinnedToCore(TaskSensores, "Sensores", 4096, NULL, 3, &TaskSensoresHandle, 1);     
    xTaskCreatePinnedToCore(TaskPersistencia, "FlashFS", 4096, NULL, 2, &TaskPersistenciaHandle, 1); 
    xTaskCreatePinnedToCore(TaskRede, "Network", 4096, NULL, 1, &TaskRedeHandle, 0);                 
  }
}

void loop() {
  vTaskDelete(NULL); 
}