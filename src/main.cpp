  #include <Arduino.h>
  #include <math.h>   // fabs() usado no filtro de histerese de lerNivelAgua()
  #include <WiFi.h>
  #include <WiFiManager.h>
  #include <PubSubClient.h>
  #include <ESPAsyncWebServer.h>
  #include <LittleFS.h>
  #include <ArduinoJson.h>
  #include <time.h>
  #include <Wire.h>
  #include <LiquidCrystal_I2C.h>

  // --- Biblioteca Oficial de Gestão de Energia da Espressif ---
  #include "esp_pm.h"

  // --- Configuração do Display I2C ---
  LiquidCrystal_I2C lcd(0x27, 16, 2);

  // --- Primitivas do FreeRTOS ---
  SemaphoreHandle_t mutexVolume;
  TaskHandle_t TaskSensoresHandle;
  TaskHandle_t TaskPersistenciaHandle;
  TaskHandle_t TaskRedeHandle;
  TaskHandle_t TaskDisplayHandle;
  portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

  // --- Flags de Controle ---
  volatile bool forcarSalvamento = false;
  bool valvulaAberta = true; // Mantém o estado lógico da válvula
  int contadorVazamento = 0; // Tempo em segundos de fluxo contínuo

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
  const int pinoTrig = 5;
  const int pinoEcho = 18;

  // NOVAS VARIÁVEIS DE CALIBRAÇÃO (Ajustadas para a garrafa plástica)
  // !! ATENÇÃO - LIMITE FÍSICO DO SENSOR !!
  // O span (distVazia - distCheia) abaixo é de apenas 0.15 cm (1.5 mm).
  // A repetibilidade real do HC-SR04 (ring-down do transdutor, temperatura,
  // threshold do comparador) é tipicamente 2-3 mm — MAIOR que todo o seu
  // intervalo útil de medição. Isso significa que parte do "ruído" que você
  // vê é matemática, não bug: 1 mm de variação / 1.5 mm de span = ~67% de salto.
  // RECALIBRE usando o MESMO processo robusto de 15 amostras (não uma leitura manual
  // única) e, se possível, force fisicamente o span a ser o maior possível
  // (ex.: ao definir "cheio", deixe alguns mm de ar livre é normal, mas evite
  // calibrar com a garrafa transbordando até o gargalo).
  const float distVazia = 18.27; // Nível 0% (Caixa Vazia)
  const float distCheia = 18.12; // Nível 100% -> RECALIBRE COM O MÉTODO ROBUSTO (ver lerNivelAgua)

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
  unsigned long tempo_lerNivel = 0, tempo_salvarFlash = 0, tempo_callbackMQTT = 0, tempo_loopSensores = 0, tempo_reconexao = 0, tempo_lcd = 0;
  bool precisaResetarWiFi = false;

  // --- Interrupções de Hardware (Não bloqueantes) ---
  void IRAM_ATTR contarPulsoEntrada() { pulsosEntrada++; }
  void IRAM_ATTR contarPulsoSaida() { pulsosSaida++; }

  // ==========================================
  // FUNÇÕES AUXILIARES
  // ==========================================

  // --- Parâmetros de Filtragem DSP (ajustados ao desafio físico da garrafa) ---
  #define DEBUG_NIVEL 1            // 1 = imprime RESUMO do ciclo no Serial, 0 = desliga em produção
  #define DEBUG_RAW   1            // 1 = imprime TODAS as 15 leituras brutas (use para diagnosticar, depois desligue)

  const int NUM_LEITURAS         = 15;   // Amostras brutas por ciclo (subiu de 11 -> 15: mais poder de média)
  const int MIN_LEITURAS_VALIDAS = 5;    // Quórum mínimo: abaixo disso, descarta o ciclo inteiro (mantém último valor bom)
  const float GAP_CLUSTER_CM     = 0.35; // Separação (cm) para considerar duas leituras como "ecos diferentes"
  const float MAX_PASSO_CICLO    = 6.0;  // Variação máxima permitida (%) por ciclo de leitura, ANTES da EMA
  const float ALPHA_EMA          = 0.15; // Suavização final
  const float HISTERESE_PCT      = 1.0;  // Só atualiza o valor publicado se mudar >= 1% real (evita flicker)


  const unsigned long TIMEOUT_ECO_US = 20000UL;

  int lerNivelAgua() {
  #if DEBUG_NIVEL
    Serial.println("[DSP] >>> entrou em lerNivelAgua()");
  #endif
    unsigned long inicio = micros();
    float leituras[NUM_LEITURAS];
    int leiturasValidas = 0;
    int nTimeouts = 0;     // pulseIn não detectou eco dentro do TIMEOUT_ECO_US
    int nForaDeFaixa = 0;  // eco detectado, mas distância fora de [15.0, 22.0] cm

    // 1. Coleta de amostras com espaçamento acústico alargado
    for (int i = 0; i < NUM_LEITURAS; i++) {
        digitalWrite(pinoTrig, LOW);
        delayMicroseconds(2);

        digitalWrite(pinoTrig, HIGH);
        delayMicroseconds(10);
        digitalWrite(pinoTrig, LOW);

        long duracao = pulseIn(pinoEcho, HIGH, TIMEOUT_ECO_US);

        if (duracao > 0) {
            float dist = duracao * 0.034 / 2.0;

            // Filtro de banda focado no comportamento da garrafa
            if (dist >= 15.0 && dist <= 22.0) {
                leituras[leiturasValidas] = dist;
                leiturasValidas++;
  #if DEBUG_RAW
                Serial.printf("   ping %2d: duracao=%5ldus dist=%.3fcm [OK]\n", i, duracao, dist);
  #endif
            } else {
                nForaDeFaixa++;
  #if DEBUG_RAW
                Serial.printf("   ping %2d: duracao=%5ldus dist=%.3fcm [FORA DE FAIXA]\n", i, duracao, dist);
  #endif
            }
        } else {
            nTimeouts++;
  #if DEBUG_RAW
            Serial.printf("   ping %2d: TIMEOUT (sem eco em %luus)\n", i, TIMEOUT_ECO_US);
  #endif
        }

        // Janela de 30ms: Crucial para o eco interno morrer nas paredes da garrafa.
        // Se a reverberação persistir no seu protótipo real, é seguro subir até ~50ms.
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    // 2. Quórum mínimo: poucos ecos válidos => sinal degradado demais neste ciclo.
    // Em vez de calcular com 1-2 amostras (ruído alto), descarta o ciclo e mantém
    // o último valor estável publicado.
    if (leiturasValidas < MIN_LEITURAS_VALIDAS) {
  #if DEBUG_NIVEL
        Serial.printf("[DSP] Ciclo descartado: %d/%d validas | timeouts=%d foraDeFaixa=%d\n",
                      leiturasValidas, NUM_LEITURAS, nTimeouts, nForaDeFaixa);
  #endif
        tempo_lerNivel = micros() - inicio;
        return ultimoNivelValido;
    }

    // 3. Ordenação crescente (Bubble Sort - ok para N pequeno)
    for (int i = 0; i < leiturasValidas - 1; i++) {
        for (int j = 0; j < leiturasValidas - i - 1; j++) {
            if (leituras[j] > leituras[j + 1]) {
                float temp = leituras[j];
                leituras[j] = leituras[j + 1];
                leituras[j + 1] = temp;
            }
        }
    }

    // 4. CLUSTERIZAÇÃO POR MODA (robusto a eco multipercurso / multipath)
    // Em uma garrafa com gargalo estreito, é comum ter DUAS populações de eco:
    // a reflexão real na superfície da água e uma reflexão parasita (ex. nas paredes
    // do gargalo). Uma média/mediana ingênua sobre TODAS as leituras "na faixa" mistura
    // essas duas populações e produz uma distância que não corresponde a NENHUM estado
    // físico real — e que varia conforme a proporção aleatória capturada em cada ciclo.
    // A abordagem correta é agrupar leituras próximas entre si (clusters) e escolher o
    // cluster com MAIS membros (a "moda" dos ecos) — ou seja, o eco mais repetido e
    // consistente dentro do próprio ciclo, não uma mistura matemática de tudo.
    struct Cluster { float soma; int n; float minV; float maxV; };
    Cluster clusters[NUM_LEITURAS];
    int nClusters = 0;

    for (int i = 0; i < leiturasValidas; i++) {
        if (nClusters == 0 || (leituras[i] - clusters[nClusters - 1].maxV) > GAP_CLUSTER_CM) {
            clusters[nClusters].soma = leituras[i];
            clusters[nClusters].n = 1;
            clusters[nClusters].minV = leituras[i];
            clusters[nClusters].maxV = leituras[i];
            nClusters++;
        } else {
            clusters[nClusters - 1].soma += leituras[i];
            clusters[nClusters - 1].n++;
            clusters[nClusters - 1].maxV = leituras[i];
        }
    }

    // Escolhe o cluster com mais membros. Em empate, prefere o de MENOR distância
    // (mais perto do sensor) — geralmente o caminho acústico mais curto/direto.
    int idxMelhor = 0;
    for (int i = 1; i < nClusters; i++) {
        if (clusters[i].n > clusters[idxMelhor].n ||
            (clusters[i].n == clusters[idxMelhor].n && clusters[i].minV < clusters[idxMelhor].minV)) {
            idxMelhor = i;
        }
    }
    float distFiltrada = clusters[idxMelhor].soma / clusters[idxMelhor].n;

  #if DEBUG_NIVEL
    Serial.printf("[DSP] %d cluster(s):\n", nClusters);
    for (int i = 0; i < nClusters; i++) {
        Serial.printf("   cluster %d: n=%d faixa=%.2f-%.2fcm media=%.3fcm%s\n",
                      i, clusters[i].n, clusters[i].minV, clusters[i].maxV,
                      clusters[i].soma / clusters[i].n,
                      (i == idxMelhor) ? "  <-- ESCOLHIDO (moda)" : "");
    }
  #endif

    // 5. Cálculo do percentual bruto
    float nivelCalculado;
    if (distFiltrada >= distVazia) {
        nivelCalculado = 0.0;
    } else if (distFiltrada <= distCheia) {
        nivelCalculado = 100.0;
    } else {
        nivelCalculado = ((distVazia - distFiltrada) / (distVazia - distCheia)) * 100.0;
    }

    // 6. LIMITADOR DE TAXA (Slew-Rate Limiter) — aplicado ANTES da EMA
    // Mesmo após os filtros acima, o span útil (distVazia - distCheia) é de apenas
    // alguns milímetros: qualquer ruído residual de ~1mm ainda é capaz de gerar saltos
    // de várias dezenas de % na fórmula acima (isso é matemática do span pequeno, não bug).
    // Este limitador trava a variação MÁXIMA fisicamente plausível por ciclo para uma
    // garrafa de 1,7L (ela não esvazia/enche 50% em ~1.5s). Ajuste MAX_PASSO_CICLO
    // para a vazão real do seu sistema.
    static float nivelSuavizado = -1.0;

    if (nivelSuavizado < 0) {
        nivelSuavizado = nivelCalculado; // Inicialização no primeiro ciclo de execução
    } else {
        float minPermitido = nivelSuavizado - MAX_PASSO_CICLO;
        float maxPermitido = nivelSuavizado + MAX_PASSO_CICLO;
        if (nivelCalculado < minPermitido) nivelCalculado = minPermitido;
        if (nivelCalculado > maxPermitido) nivelCalculado = maxPermitido;

        // 7. FILTRO PASSA-BAIXAS (Média Móvel Exponencial - EMA)
        // alpha = 0.15 significa que o sistema absorve 15% da nova leitura (já limitada
        // pelo slew-rate acima) e mantém 85% do histórico.
        nivelSuavizado = (ALPHA_EMA * nivelCalculado) + ((1.0 - ALPHA_EMA) * nivelSuavizado);
    }

    if (nivelSuavizado < 0) nivelSuavizado = 0;
    if (nivelSuavizado > 100) nivelSuavizado = 100;

    // 8. HISTERESE DE EXIBIÇÃO (Deadband)
    // Só atualiza o valor publicado/exibido se a mudança real for >= HISTERESE_PCT.
    // Isso elimina o "tremor" visual de ficar oscilando entre dois inteiros vizinhos
    // (ex: 86% <-> 87%) quando o valor suavizado está bem na fronteira de arredondamento.
    static float ultimoPublicado = -1.0;
    if (ultimoPublicado < 0 || fabs(nivelSuavizado - ultimoPublicado) >= HISTERESE_PCT) {
        ultimoPublicado = nivelSuavizado;
    }

    int nivelFinal = (int)(ultimoPublicado + 0.5);
    if (nivelFinal < 0) nivelFinal = 0;
    if (nivelFinal > 100) nivelFinal = 100;

    ultimoNivelValido = nivelFinal;

  #if DEBUG_NIVEL
    Serial.printf("[DSP] validas=%d/%d (timeouts=%d foraDeFaixa=%d) distFiltrada=%.3fcm bruto=%.1f%% suavizado=%.1f%% final=%d%%\n",
                  leiturasValidas, NUM_LEITURAS, nTimeouts, nForaDeFaixa, distFiltrada, nivelCalculado, nivelSuavizado, nivelFinal);
  #endif

    tempo_lerNivel = micros() - inicio;
    return ultimoNivelValido;
  }

  void callbackMQTT(char* topico, byte* payload, unsigned int length) {
    unsigned long inicio = micros();
    String msg = "";
    for (int i = 0; i < length; i++) msg += (char)payload[i];
    
    if (String(topico) == "cps/caixa/valvula") {
        if (msg == "ABRIR") {
            digitalWrite(pinoRele, HIGH);
            valvulaAberta = true;
        } else if (msg == "FECHAR") {
            digitalWrite(pinoRele, LOW);
            valvulaAberta = false;
        }
    }
    tempo_callbackMQTT = micros() - inicio;
  }

  // ==========================================
  // TASKS DO FREERTOS
  // ==========================================
  void TaskSensores(void *pvParameters) {
    for (;;) {
      unsigned long inicioL = micros();

  #if DEBUG_NIVEL
      Serial.println("[TASK] TaskSensores: novo ciclo iniciado");
  #endif

      portENTER_CRITICAL(&timerMux);
      uint32_t in = pulsosEntrada; uint32_t out = pulsosSaida;
      pulsosEntrada = 0; pulsosSaida = 0; 
      portEXIT_CRITICAL(&timerMux);
      
      float vazaoIn = (in / fatorCalibracao / 60.0);
      float vazaoOut = (out / fatorCalibracao / 60.0);

  #if DEBUG_NIVEL
      Serial.println("[TASK] Chamando lerNivelAgua()...");
  #endif
      int nivelAtual = lerNivelAgua();
  #if DEBUG_NIVEL
      Serial.printf("[TASK] lerNivelAgua() retornou: %d%%\n", nivelAtual);
  #endif

      // =======================================================
      // LÓGICA CIBERFÍSICA DE SEGURANÇA (VAZAMENTO)
      // =======================================================
      if (vazaoOut > 0.5) { 
          contadorVazamento++;
          if (contadorVazamento >= 10 && valvulaAberta) {
              valvulaAberta = false;
              digitalWrite(pinoRele, LOW); 
              
              if (mqttClient.connected()) {
                  mqttClient.publish("cps/caixa/valvula", "FECHAR"); 
              }
              Serial.println("[ALERTA] VAZAMENTO DETECTADO! Válvula de segurança acionada.");
          }
      } else {
          contadorVazamento = 0; 
      }
      // =======================================================

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
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  void TaskDisplay(void *pvParameters) {
    for (;;) {
      unsigned long inicio = micros();
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("WaterInBox-Nivel");
      lcd.setCursor(0, 1);
      
      lcd.print("Agua: ");
      lcd.print(ultimoNivelValido);
      lcd.print("%");

      tempo_lcd = micros() - inicio;
      
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }

  void TaskPersistencia(void *pvParameters) {
    unsigned long ultimoSalvamento = millis();
    const unsigned long INTERVALO_AUTO = 3600000; 

    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000)); 
      
      if (forcarSalvamento || (millis() - ultimoSalvamento >= INTERVALO_AUTO)) {
        forcarSalvamento = false;
        ultimoSalvamento = millis();

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
        Serial.println("[INFO] Dados gravados na Flash com sucesso.");
      }
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

    // --- Inicialização do Display ---
    Wire.begin(26,27);
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("Iniciando...");

    esp_pm_config_esp32_t pm_config;
    pm_config.max_freq_mhz = 240; 
    pm_config.min_freq_mhz = 80;  
    pm_config.light_sleep_enable = true; 

    esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_OK) {
        Serial.println("SUCESSO: Gestão de Energia Ativada! Light Sleep configurado.");
    }

    // --- Configuração dos pinos físicos ---
    pinMode(pinoTrig, OUTPUT); 
    pinMode(pinoEcho, INPUT);
    
    pinMode(pinoRele, OUTPUT); 
    digitalWrite(pinoRele, HIGH); 
    
    pinMode(pinoSensorEntrada, INPUT_PULLUP); pinMode(pinoSensorSaida, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(pinoSensorEntrada), contarPulsoEntrada, FALLING);
    attachInterrupt(digitalPinToInterrupt(pinoSensorSaida), contarPulsoSaida, FALLING);
    
    if (!LittleFS.begin(true)) { Serial.println("ERRO: LittleFS!"); return; }
    
    if (!LittleFS.exists("/historico.json")) {
        File f = LittleFS.open("/historico.json", "w");
        if (f) { f.print("{\"dados\":[]}"); f.close(); }
    }

    lcd.setCursor(0, 1);
    lcd.print("Conectando WiFi");

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
      doc["t_lcd"] = tempo_lcd; 
      String out; serializeJson(doc, out); request->send(200, "application/json", out);
    });
    
    server.on("/reset_wifi", HTTP_POST, [](AsyncWebServerRequest *request){ request->send(200, "text/plain", "OK"); precisaResetarWiFi = true; });
    
    server.on("/salvar_agora", HTTP_POST, [](AsyncWebServerRequest *request){ 
      forcarSalvamento = true;
      request->send(200, "text/plain", "OK"); 
    });

    server.on("/logs.txt", HTTP_GET, [](AsyncWebServerRequest *request){
        String textoLog = "=== WATERINBOX - LOGS DE DIAGNOSTICO ===\r\n";
        textoLog += "Sistema Operacional: FreeRTOS | Uptime: " + String(millis() / 1000) + " segundos\r\n";
        textoLog += "--------------------------------------------------\r\n\r\n";
        
        textoLog += "[INFO] Boot concluido. Sistema Ciberfisico iniciado com sucesso.\r\n";
        textoLog += "[INFO] Light Sleep configurado. Gerenciamento de energia ativo.\r\n";
        textoLog += "[INFO] Memoria LittleFS montada. Espaco alocado: " + String(LittleFS.usedBytes()) + " Bytes.\r\n";
        textoLog += "[INFO] Threads distribuidas nos Nucleos 0 e 1 do ESP32.\r\n";
        
        if (WiFi.RSSI() < -80) { textoLog += "[WARNING] Sinal Wi-Fi fraco (" + String(WiFi.RSSI()) + " dBm).\r\n"; } 
        else { textoLog += "[INFO] Conexao Wi-Fi com sinal estavel.\r\n"; }
        
        if (ultimoNivelValido < 20) { textoLog += "[WARNING] Nivel hídrico criticamente baixo.\r\n"; }

        if (ESP.getFreeHeap() < 20000) { textoLog += "[ERROR] RAM critica! Possivel vazamento de memoria.\r\n"; } 
        else { textoLog += "[INFO] Integridade da Memoria RAM validada.\r\n"; }

        textoLog += "\r\n--- ULTIMO DUMP DE PERSISTENCIA (LITTLEFS) ---\r\n";
        if (LittleFS.exists("/historico.json")) {
            File file = LittleFS.open("/historico.json", "r");
            textoLog += file.readString(); file.close();
        } else {
            textoLog += "[INFO] Aguardando o primeiro ciclo da Task de Persistencia.\r\n";
        }

        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", textoLog);
        response->addHeader("Content-Disposition", "attachment; filename=\"waterinbox_logs.txt\"");
        request->send(response);
    });
    
    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *r){ r->send(204); });
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    server.begin();

    mqttClient.setServer(mqtt_broker, mqtt_port);
    mqttClient.setCallback(callbackMQTT);

    mutexVolume = xSemaphoreCreateMutex();

    if (mutexVolume != NULL) {
      Serial.println("[SETUP] Mutex criado. Criando tasks...");
      xTaskCreatePinnedToCore(TaskSensores, "Sensores", 4096, NULL, 3, &TaskSensoresHandle, 1);     
      xTaskCreatePinnedToCore(TaskPersistencia, "FlashFS", 4096, NULL, 2, &TaskPersistenciaHandle, 1); 
      xTaskCreatePinnedToCore(TaskRede, "Network", 4096, NULL, 1, &TaskRedeHandle, 0);   
      xTaskCreatePinnedToCore(TaskDisplay, "LCD", 2048, NULL, 1, &TaskDisplayHandle, 0); 
      Serial.printf("[SETUP] TaskSensores handle: %p\n", TaskSensoresHandle);
    } else {
      Serial.println("[SETUP] ERRO CRÍTICO: mutexVolume é NULL! Tasks NÃO foram criadas.");
    }
  }

  void loop() {
    vTaskDelete(NULL); 
  }