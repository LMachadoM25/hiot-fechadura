/*
 * Fechadura IoT — ESP32 Wroom + MFRC522 + Servo + Sensor Magnético + MQTT
 * =========================================================================
 *
 * COMO FUNCIONA:
 *  1. Aproxima um cartão NFC no leitor MFRC522
 *  2. O ESP32 lê o UID do cartão e envia para o servidor via MQTT
 *  3. O servidor (Node-RED) decide se o acesso é permitido
 *  4. Se permitido, o servidor envia "abrir" para o ESP32
 *  5. O servo abre a fechadura
 *  6. O sensor magnético detecta se a porta está aberta ou fechada
 *
 * PINAGEM:
 *  MFRC522:
 *   3.3V  → 3.3V
 *   GND   → GND
 *   SDA   → GPIO5
 *   SCK   → GPIO18
 *   MOSI  → GPIO23
 *   MISO  → GPIO19
 *   RST   → GPIO4
 *
 *  Servo:
 *   Sinal → GPIO2
 *   VCC   → Fonte 5V externa (NÃO usar o 5V do ESP32)
 *   GND   → GND do ESP32 e da fonte externa
 *
 *  Sensor magnético:
 *   Um fio → GPIO13
 *   Outro  → GND
 * =========================================================================
 */

#include <Arduino.h>
#include <ESP32Servo.h>
#include <MFRC522.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <WiFi.h>

// =========================================================================
//  CONFIGURAÇÕES — altere aqui antes de gravar
// =========================================================================

// Rede WiFi
#define WIFI_SSID "NPITI-IoT"
#define WIFI_PASS "NPITI-IoT"

// Servidor MQTT (endereço do Node-RED)
#define MQTT_HOST "10.7.40.79"
#define MQTT_PORT 1883
#define MQTT_ID   "fechadura-esp32"
#define MQTT_USER "NPITI-IoT"
#define MQTT_PASS "NPITI-IoT"

// Tópicos MQTT
// O ESP32 PUBLICA nesses tópicos (envia dados para o servidor):
#define TOPIC_UID   "fechadura/uid"    // UID do cartão lido
#define TOPIC_PORTA "fechadura/porta"  // estado da porta (aberta/fechada)
// O ESP32 ASSINA esse tópico (recebe comandos do servidor):
#define TOPIC_CMD   "fechadura/cmd"    // "abrir" ou "fechar"

// Pinos
#define PIN_SS    5   // MFRC522 SDA/SS
#define PIN_RST   4   // MFRC522 RST
#define PIN_SERVO 2   // Servo
#define PIN_DOOR  13  // Sensor magnético

// Posições do servo em graus
#define SERVO_FECHADO  0
#define SERVO_ABERTO   90

// Tempo máximo que a fechadura fica aberta (ms)
// Após esse tempo, fecha automaticamente caso o servidor não envie "fechar"
#define TEMPO_ABERTO_MS 5000

// =========================================================================
//  VARIÁVEIS GLOBAIS
// =========================================================================

MFRC522      rfid(PIN_SS, PIN_RST);  // leitor NFC
Servo        servo;                   // servo da fechadura
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);        // cliente MQTT

bool     fechaduraAberta = false;    // estado atual da fechadura
uint32_t abertaDesdeMs   = 0;        // quando a fechadura foi aberta
bool     portaAberta     = false;    // estado atual da porta

// Variáveis do sensor magnético (usadas na interrupção)
volatile bool     mudouPorta   = false;
volatile uint32_t mudouPortaMs = 0;

// =========================================================================
//  SENSOR MAGNÉTICO — interrupção
// =========================================================================

// Essa função é chamada automaticamente quando o sensor muda de estado.
// Ela só registra que houve mudança — o processamento acontece no loop().
// IMPORTANTE: nunca coloque código complexo dentro de uma interrupção.
void IRAM_ATTR onMudancaPorta() {
    mudouPorta   = true;
    mudouPortaMs = millis();
}

// =========================================================================
//  SERVO
// =========================================================================

// Abre ou fecha a fechadura movendo o servo
void moverFechadura(bool abrir) {
    fechaduraAberta = abrir;
    servo.write(abrir ? SERVO_ABERTO : SERVO_FECHADO);
    Serial.printf("[SERVO] Fechadura %s\n", abrir ? "ABERTA" : "FECHADA");
}

// Fecha automaticamente após TEMPO_ABERTO_MS
// Chamada no loop() para não usar delay()
void verificarAutoFechamento() {
    if (fechaduraAberta && millis() - abertaDesdeMs >= TEMPO_ABERTO_MS) {
        moverFechadura(false);
        Serial.println("[SERVO] Fechamento automático.");
    }
}

// =========================================================================
//  MQTT
// =========================================================================

// Publica uma mensagem em um tópico MQTT
void publicar(const char* topico, const char* mensagem, bool reter = false) {
    if (!mqtt.connected()) return;
    mqtt.publish(topico, mensagem, reter);
    Serial.printf("[MQTT] Publicado em %s: %s\n", topico, mensagem);
}

// Recebe mensagens do servidor (Node-RED)
// Chamada automaticamente pelo PubSubClient quando chega uma mensagem
void onMensagemRecebida(char* topico, byte* payload, unsigned int tamanho) {
    // Converte o payload para String
    String mensagem;
    for (unsigned int i = 0; i < tamanho; i++) mensagem += (char)payload[i];
    mensagem.trim();
    mensagem.toLowerCase();

    Serial.printf("[MQTT] Recebido em %s: %s\n", topico, mensagem.c_str());

    // Processa o comando recebido
    if (mensagem == "abrir") {
        moverFechadura(true);
        abertaDesdeMs = millis();  // inicia contagem para auto-fechamento
    } else if (mensagem == "fechar") {
        moverFechadura(false);
    }
}

// Conecta ao broker MQTT e assina o tópico de comandos
bool conectarMQTT() {
    Serial.printf("[MQTT] Conectando a %s...", MQTT_HOST);
    if (mqtt.connect(MQTT_ID, MQTT_USER, MQTT_PASS)) {
        Serial.println(" OK");
        mqtt.subscribe(TOPIC_CMD);  // assina o tópico de comandos
    } else {
        Serial.printf(" FALHOU (rc=%d)\n", mqtt.state());
    }
    return mqtt.connected();
}

// =========================================================================
//  WIFI
// =========================================================================

bool conectarWiFi() {
    Serial.printf("[WiFi] Conectando a '%s'", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    uint32_t inicio = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - inicio > 15000) {
            Serial.println("\n[WiFi] Timeout.");
            return false;
        }
        delay(500);
        Serial.print('.');
    }
    Serial.printf("\n[WiFi] Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

// =========================================================================
//  SETUP — executado uma vez ao ligar
// =========================================================================

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== Fechadura IoT - ESP32 Wroom ===");

    // -- Servo --
    // Aloca o timer antes do WiFi para evitar conflito de hardware
    ESP32PWM::allocateTimer(0);
    servo.setPeriodHertz(50);
    servo.attach(PIN_SERVO, 500, 2400);
    moverFechadura(false);  // garante que inicia fechada
    Serial.println("[SERVO] OK");

    // -- Sensor magnético --
    pinMode(PIN_DOOR, INPUT_PULLUP);
    // CHANGE = dispara quando o pino muda de HIGH para LOW ou vice-versa
    attachInterrupt(digitalPinToInterrupt(PIN_DOOR), onMudancaPorta, CHANGE);
    portaAberta = digitalRead(PIN_DOOR) == HIGH;
    Serial.printf("[DOOR] Porta %s\n", portaAberta ? "aberta" : "fechada");

    // -- MFRC522 --
    SPI.begin();
    rfid.PCD_Init();
    delay(50);
    byte versao = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    Serial.printf("[RC522] Versao: 0x%02X %s\n",
                  versao,
                  (versao == 0x92 || versao == 0x91) ? "OK" : "ERRO - verifique fiacao!");

    // -- WiFi + MQTT --
    if (conectarWiFi()) {
        mqtt.setServer(MQTT_HOST, MQTT_PORT);
        mqtt.setCallback(onMensagemRecebida);
        conectarMQTT();
    }

    Serial.println("[READY] Sistema pronto. Aproxime um cartão NFC.");
}

// =========================================================================
//  LOOP — executado continuamente
// =========================================================================

void loop() {

    // -- Mantém a conexão MQTT ativa --
    if (WiFi.status() == WL_CONNECTED) {
        if (!mqtt.connected()) {
            // Tenta reconectar a cada 5 segundos
            static uint32_t ultimaTentativa = 0;
            if (millis() - ultimaTentativa > 5000) {
                ultimaTentativa = millis();
                conectarMQTT();
            }
        } else {
            mqtt.loop();  // processa mensagens recebidas
        }
    }

    // -- Sensor magnético --
    // Verifica se houve mudança (registrada pela interrupção)
    if (mudouPorta) {
        noInterrupts();               // pausa interrupções para leitura segura
        uint32_t quando = mudouPortaMs;
        mudouPorta = false;
        interrupts();                 // reativa interrupções

        // Debounce: aguarda 50ms para confirmar que não foi ruído elétrico
        if (millis() - quando >= 50) {
            bool aberta = digitalRead(PIN_DOOR) == HIGH;
            if (aberta != portaAberta) {
                portaAberta = aberta;
                Serial.printf("[DOOR] Porta %s\n", aberta ? "aberta" : "fechada");
                publicar(TOPIC_PORTA, aberta ? "aberta" : "fechada", true);
            }
        }
    }

    // -- Auto-fechamento do servo --
    verificarAutoFechamento();

    // -- Leitor NFC --
    // Verifica se há um cartão presente
    if (!rfid.PICC_IsNewCardPresent()) return;
    if (!rfid.PICC_ReadCardSerial())   return;

    // Lê e converte o UID para string hexadecimal (ex: "A1B2C3D4")
    String uid;
    for (uint8_t i = 0; i < rfid.uid.size; i++) {
        if (rfid.uid.uidByte[i] < 0x10) uid += '0';
        uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    Serial.printf("[NFC] Cartão lido: %s\n", uid.c_str());
    if (uid == "9D11544C") {
        moverFechadura(true);
        abertaDesdeMs = millis();
    } else {
        moverFechadura(false);
    }

    // Envia o UID/ para o servidor — ele decide se abre ou não
    // if (mqtt.connected()) {
    //     publicar(TOPIC_UID, uid.c_str());
    // } else {
    //     Serial.println("[NFC] Sem conexão MQTT — UID não enviado.");
    // }

    // Encerra comunicação com o cartão antes de ler o próximo
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    delay(500);  // aguarda 500ms antes de aceitar novo cartão
}