#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

// --- Configurações de Rede e MQTT ---
const char* ssid = "TGT";
const char* password = "21012007";
const char* mqtt_broker = "broker.hivemq.com";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- Mapeamento de Pinos (Hardware) ---
// 1. Relé (Válvula Solenoide)
const int pinoRele = 25; 

// 2. Ultrassom JSN-SR04T (Modo Serial UART)
const int pinoRX2 = 16; // Ligue no TX do Sensor (Use divisor de tensão aqui!)
const int pinoTX2 = 17; // Ligue no RX do Sensor
const float alturaCaixaCm = 100.0; // Altura máxima do seu reservatório
int ultimoNivelValido = 0; 

// 3. Sensores de Fluxo (Vazão)
const int pinoSensorEntrada = 32;
const int pinoSensorSaida = 33;

volatile uint32_t pulsosEntrada = 0;
volatile uint32_t pulsosSaida = 0;
const float fatorCalibracao = 7.5; 

unsigned long tempoAnterior = 0;

// --- Interrupções de Hardware ---
void IRAM_ATTR contarPulsoEntrada() { pulsosEntrada++; }
void IRAM_ATTR contarPulsoSaida() { pulsosSaida++; }

// --- Função de Leitura do Ultrassom (Serial UART) ---
int lerNivelAgua() {
  // Limpa o buffer de leitura
  while (Serial2.available()) {
    Serial2.read();
  }

  // Dispara o comando de leitura (0x55)
  Serial2.write(0x55);
  delay(50); // Aguarda o processamento do sensor

  if (Serial2.available() >= 4) {
    if (Serial2.read() == 0xFF) { // Byte inicial de confirmação
      int byteAlto = Serial2.read();
      int byteBaixo = Serial2.read();
      int checksum = Serial2.read();

      // Verifica integridade do pacote
      if (((0xFF + byteAlto + byteBaixo) & 0xFF) == checksum) {
        float distanciaMm = (byteAlto << 8) | byteBaixo;
        float distanciaCm = distanciaMm / 10.0;
        
        int porcentagem = 100 - ((distanciaCm / alturaCaixaCm) * 100);
        
        if (porcentagem > 100) porcentagem = 100;
        if (porcentagem < 0) porcentagem = 0;
        
        ultimoNivelValido = porcentagem;
        return porcentagem;
      }
    }
  }
  return ultimoNivelValido;
}

// --- Callback MQTT: Processa Comandos do Site ---
void callbackMQTT(char* topico, byte* payload, unsigned int length) {
  String mensagem = "";
  for (int i = 0; i < length; i++) {
    mensagem += (char)payload[i];
  }

  if (String(topico) == "cps/caixa/valvula") {
    if (mensagem == "ABRIR" || mensagem == "ABERTA") {
      digitalWrite(pinoRele, HIGH);
      Serial.println("Status: Válvula ABERTA via MQTT");
    } 
    else if (mensagem == "FECHAR" || mensagem == "FECHADA") {
      digitalWrite(pinoRele, LOW);
      Serial.println("Status: Válvula FECHADA via MQTT");
    }
  }
}

// --- Conexão ao Broker HiveMQ ---
void reconectarMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Conectando ao broker HiveMQ...");
    String clientId = "ESP32_Backend_" + String(random(0xffff), HEX);
    
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("Conectado!");
      mqttClient.subscribe("cps/caixa/valvula");
    } else {
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  // Inicia a comunicação com o sensor ultrassônico
  Serial2.begin(9600, SERIAL_8N1, pinoRX2, pinoTX2);

  pinMode(pinoRele, OUTPUT);
  digitalWrite(pinoRele, LOW); // Inicia fechado
  
  pinMode(pinoSensorEntrada, INPUT_PULLUP);
  pinMode(pinoSensorSaida, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(pinoSensorEntrada), contarPulsoEntrada, FALLING);
  attachInterrupt(digitalPinToInterrupt(pinoSensorSaida), contarPulsoSaida, FALLING);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }
  Serial.println("\nWi-Fi Conectado!");

  mqttClient.setServer(mqtt_broker, mqtt_port);
  mqttClient.setCallback(callbackMQTT);
}

void loop() {
  if (!mqttClient.connected()) reconectarMQTT();
  mqttClient.loop();

  unsigned long tempoAtual = millis();

  // Envia a telemetria a cada 1 segundo
  if (tempoAtual - tempoAnterior >= 1000) {
    
    noInterrupts();
    uint32_t tokensIn = pulsosEntrada;
    uint32_t tokensOut = pulsosSaida;
    pulsosEntrada = 0;
    pulsosSaida = 0;
    interrupts();

    float vazaoEntrada = tokensIn / fatorCalibracao;
    float vazaoSaida = tokensOut / fatorCalibracao;
    int nivelReservatorio = lerNivelAgua();

    mqttClient.publish("cps/caixa/fluxo_entrada", String(vazaoEntrada, 1).c_str());
    mqttClient.publish("cps/caixa/fluxo", String(vazaoSaida, 1).c_str());
    mqttClient.publish("cps/caixa/nivel", String(nivelReservatorio).c_str());

    Serial.printf("In: %.1f L/min | Out: %.1f L/min | Nivel: %d%%\n", vazaoEntrada, vazaoSaida, nivelReservatorio);

    tempoAnterior = tempoAtual;
  }
}