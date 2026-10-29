#include <SoftwareSerial.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <Servo.h>

// ======================================================
// COMUNICAÇÃO COM ESP8266
// ======================================================
// Arduino D11 = RX  <- ESP GPIO1 (TX) (via conversor de nível)
// Arduino D10 = TX  -> ESP GPIO3 (RX) (via conversor de nível)
// GND do Arduino e do ESP em comum
// ======================================================
SoftwareSerial espSerial(11, 10);

// ======================================================
// LCD I2C
// ======================================================
LiquidCrystal_I2C lcd(0x27, 20, 4);

// ======================================================
// DHT11
// ======================================================
#define PIN_DHT 4
#define DHTTYPE DHT11
DHT dht(PIN_DHT, DHTTYPE);

// ======================================================
// SENSORES
// ======================================================
#define PIN_PIR 5
#define PIN_MAG 12          // Sensor magnético da porta (HIGH = aberta)

// ======================================================
// RELÉS (ativos em LOW) E SAÍDAS
// ======================================================
#define PIN_RELE_GARAGEM 2
#define PIN_BUZZER 3
#define PIN_LED_ALERTA 13
#define PIN_RELE_SALA 6
#define PIN_RELE_QUARTO 7
#define PIN_RELE_COZINHA 8
#define PIN_RELE_COOLER A0

// ======================================================
// SERVOS
// ======================================================
#define PIN_SERVO_PORTA 9
#define PIN_SERVO_GARAGEM A3
Servo servoPorta;
Servo servoGaragem;

// ======================================================
// HC-SR04
// ======================================================
#define PIN_TRIG A1
#define PIN_ECHO A2

// ======================================================
// CONSTANTES
// ======================================================
const float DISTANCIA_ALERTA = 50.0;          // cm
const float TEMP_LIGA_COOLER = 28.0;          // liga em >= 28
const float TEMP_DESLIGA_COOLER = 27.0;       // desliga em <= 27 (histerese)
const unsigned long TEMPO_LUZ_SALA = 10000;   // luz fica 10 s após o último movimento
const unsigned long TEMPO_PRESENCA_SIRENE = 5000;  // objeto próximo por 5 s liga o buzzer
const unsigned long TEMPO_AUTORIZ_GARAGEM = 15000; // se a garagem não abrir, cancela
const unsigned long TEMPO_AUTORIZ_PORTA = 30000;
const unsigned long TEMPO_FECHAR_PORTA = 10000;

const unsigned long INTERVALO_DHT = 2000;
const unsigned long INTERVALO_DIST = 100;
const unsigned long INTERVALO_LCD = 500;
const unsigned long INTERVALO_ENVIO = 2000;

// ======================================================
// VARIÁVEIS
// ======================================================
float temperatura = NAN;
float humidade = NAN;
float distanciaGaragem = 999;

bool modoManual = false;
bool alarmeAtivo = true;          // ALARME_OFF silencia o buzzer (o LED continua)
bool coolerLigado = false;

// Garagem
bool garagemAberta = false;               // informado pelo ESP
bool aberturaGaragemAutorizada = false;
unsigned long tempoAutorizacao = 0;

// Porta principal
bool portaAutorizada = false;
bool portaFechando = false;
unsigned long tempoAutorizPorta = 0;
unsigned long tempoFechoPorta = 0;

// Luz da sala (PIR)
unsigned long ultimoMovimento = 0;
bool salaLigadaPorPIR = false;

// Presença perto da garagem
bool objetoPresente = false;
unsigned long inicioPresenca = 0;

// Temporizadores
unsigned long tDHT = 0, tDist = 0, tLCD = 0, tEnvio = 0;

// Buffer de comandos recebidos do ESP
char bufferCmd[40];
uint8_t idxCmd = 0;

// ======================================================
// SETUP
// ======================================================
void setup() {
  Serial.begin(9600);
  espSerial.begin(9600);

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_MAG, INPUT_PULLUP);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);

  // Relés: escreve HIGH ANTES do pinMode para não estalar no boot
  digitalWrite(PIN_RELE_GARAGEM, HIGH);
  digitalWrite(PIN_RELE_SALA, HIGH);
  digitalWrite(PIN_RELE_QUARTO, HIGH);
  digitalWrite(PIN_RELE_COZINHA, HIGH);
  digitalWrite(PIN_RELE_COOLER, HIGH);
  pinMode(PIN_RELE_GARAGEM, OUTPUT);
  pinMode(PIN_RELE_SALA, OUTPUT);
  pinMode(PIN_RELE_QUARTO, OUTPUT);
  pinMode(PIN_RELE_COZINHA, OUTPUT);
  pinMode(PIN_RELE_COOLER, OUTPUT);

  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_LED_ALERTA, LOW);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_ALERTA, OUTPUT);

  servoPorta.attach(PIN_SERVO_PORTA);
  servoPorta.write(0);
  servoGaragem.attach(PIN_SERVO_GARAGEM);
  servoGaragem.write(0);

  dht.begin();

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcdLinha(0, " CASA INTELIGENTE");
  lcdLinha(1, " Arduino + ESP");
  delay(2000);
  lcd.clear();

  espSerial.println("ARDUINO_PRONTO");
}

// ======================================================
// LOOP PRINCIPAL
// ======================================================
void loop() {
  receberESP();
  lerSensores();
  sistemaAutomatico();
  sistemaGaragem();
  sistemaSeguranca();
  atualizarLCD();
  enviarDados();
}

// ======================================================
// RECEBER COMANDOS DO ESP (sem bloquear o loop)
// ======================================================
void receberESP() {
  while (espSerial.available()) {
    char c = (char)espSerial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      bufferCmd[idxCmd] = '\0';
      if (idxCmd > 0) processarComando(bufferCmd);
      idxCmd = 0;
    } else if (idxCmd < sizeof(bufferCmd) - 1) {
      bufferCmd[idxCmd++] = c;
    } else {
      idxCmd = 0;  // linha longa demais: descarta
    }
  }
}

void processarComando(const char* cmd) {
  Serial.print(F("ESP -> "));
  Serial.println(cmd);

  // Modo
  if (!strcmp(cmd, "AUTO")) modoManual = false;
  if (!strcmp(cmd, "MANUAL")) modoManual = true;

  // Alarme sonoro
  if (!strcmp(cmd, "ALARME_ON")) alarmeAtivo = true;
  if (!strcmp(cmd, "ALARME_OFF")) alarmeAtivo = false;

  // Luzes e cooler (só no modo manual)
  if (modoManual) {
    if (!strcmp(cmd, "LIGA_SALA")) digitalWrite(PIN_RELE_SALA, LOW);
    if (!strcmp(cmd, "DESLIGA_SALA")) digitalWrite(PIN_RELE_SALA, HIGH);
    if (!strcmp(cmd, "LIGA_QUARTO")) digitalWrite(PIN_RELE_QUARTO, LOW);
    if (!strcmp(cmd, "DESLIGA_QUARTO")) digitalWrite(PIN_RELE_QUARTO, HIGH);
    if (!strcmp(cmd, "LIGA_COZINHA")) digitalWrite(PIN_RELE_COZINHA, LOW);
    if (!strcmp(cmd, "DESLIGA_COZINHA")) digitalWrite(PIN_RELE_COZINHA, HIGH);
    if (!strcmp(cmd, "LIGA_COOLER")) {
      digitalWrite(PIN_RELE_COOLER, LOW);
      coolerLigado = true;
    }
    if (!strcmp(cmd, "DESLIGA_COOLER")) {
      digitalWrite(PIN_RELE_COOLER, HIGH);
      coolerLigado = false;
    }
  }

  // Porta principal
  if (!strcmp(cmd, "ABRIR_PORTA")) {
    portaAutorizada = true;
    portaFechando = false;
    tempoAutorizPorta = millis();
    servoPorta.write(90);
  }
  if (!strcmp(cmd, "FECHAR_PORTA")) {
    servoPorta.write(0);
    portaFechando = true;           // autorização cai quando a porta fechar
    tempoFechoPorta = millis();
  }

  // Garagem
  if (!strcmp(cmd, "ABRIR_GARAGEM")) {
    aberturaGaragemAutorizada = true;
    tempoAutorizacao = millis();
    servoGaragem.write(90);
  }
  if (!strcmp(cmd, "FECHAR_GARAGEM")) {
    servoGaragem.write(0);
    // A autorização só cai quando o sensor confirmar GARAGEM_FECHADA
  }

  // Sensor magnético da garagem (ligado ao ESP)
  if (!strcmp(cmd, "GARAGEM_ABERTA")) garagemAberta = true;
  if (!strcmp(cmd, "GARAGEM_FECHADA")) {
    // O ESP reenvia o estado a cada 3 s: só cancela a autorização
    // quando a garagem estava aberta e acabou de fechar.
    if (garagemAberta) aberturaGaragemAutorizada = false;
    garagemAberta = false;
  }
}

// ======================================================
// LER SENSORES (cada um no seu ritmo)
// ======================================================
void lerSensores() {
  unsigned long agora = millis();

  if (agora - tDHT >= INTERVALO_DHT) {
    tDHT = agora;
    temperatura = dht.readTemperature();
    humidade = dht.readHumidity();
  }

  if (agora - tDist >= INTERVALO_DIST) {
    tDist = agora;
    distanciaGaragem = medirDistancia();
  }
}

float medirDistancia() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  long duracao = pulseIn(PIN_ECHO, HIGH, 30000);
  if (duracao == 0) return 999;
  return duracao * 0.0343 / 2.0;
}

// ======================================================
// SISTEMA AUTOMÁTICO
// ======================================================
void sistemaAutomatico() {
  if (modoManual) return;

  // Cooler com histerese
  if (!isnan(temperatura)) {
    if (temperatura >= TEMP_LIGA_COOLER) coolerLigado = true;
    else if (temperatura <= TEMP_DESLIGA_COOLER) coolerLigado = false;
    digitalWrite(PIN_RELE_COOLER, coolerLigado ? LOW : HIGH);
  }

  // Luz da sala por PIR, com tempo de espera
  if (digitalRead(PIN_PIR) == HIGH) {
    ultimoMovimento = millis();
    salaLigadaPorPIR = true;
  } else if (salaLigadaPorPIR && millis() - ultimoMovimento > TEMPO_LUZ_SALA) {
    salaLigadaPorPIR = false;
  }
  digitalWrite(PIN_RELE_SALA, salaLigadaPorPIR ? LOW : HIGH);
}

// ======================================================
// SISTEMA DA GARAGEM
// ======================================================
void sistemaGaragem() {
  // Lâmpada acende com a garagem aberta
  digitalWrite(PIN_RELE_GARAGEM, garagemAberta ? LOW : HIGH);

  // Se mandou abrir e a garagem não abriu, cancela a autorização
  if (aberturaGaragemAutorizada && !garagemAberta &&
      millis() - tempoAutorizacao > TEMPO_AUTORIZ_GARAGEM) {
    aberturaGaragemAutorizada = false;
  }
}

// ======================================================
// SISTEMA DE SEGURANÇA
// ======================================================
void sistemaSeguranca() {
  unsigned long agora = millis();
  bool portaAberta = (digitalRead(PIN_MAG) == HIGH);

  // Autorização da porta termina quando ela fecha (ou após o tempo limite)
  if (portaFechando && (!portaAberta || agora - tempoFechoPorta > TEMPO_FECHAR_PORTA)) {
    portaAutorizada = false;
    portaFechando = false;
  }
  if (portaAutorizada && !portaFechando && !portaAberta &&
      agora - tempoAutorizPorta > TEMPO_AUTORIZ_PORTA) {
    portaAutorizada = false;
  }

  bool alertaPorta = portaAberta && !portaAutorizada;
  bool alertaGaragem = garagemAberta && !aberturaGaragemAutorizada;
  bool objetoProximo = (distanciaGaragem <= DISTANCIA_ALERTA);

  // Presença contínua perto da garagem
  if (objetoProximo) {
    if (!objetoPresente) {
      objetoPresente = true;
      inicioPresenca = agora;
    }
  } else {
    objetoPresente = false;
  }
  bool presencia = objetoPresente && (agora - inicioPresenca >= TEMPO_PRESENCA_SIRENE);

  // LED: qualquer alerta. Buzzer: só alertas graves e se o alarme estiver ativo.
  bool led = alertaPorta || alertaGaragem || objetoProximo;
  bool sirene = alertaPorta || alertaGaragem || presencia;

  digitalWrite(PIN_LED_ALERTA, led ? HIGH : LOW);
  digitalWrite(PIN_BUZZER, (sirene && alarmeAtivo) ? HIGH : LOW);
}

// ======================================================
// LCD (linhas sempre preenchidas com 20 colunas)
// ======================================================
void lcdLinha(uint8_t linha, const char* texto) {
  char buf[21];
  snprintf(buf, sizeof(buf), "%-20s", texto);
  lcd.setCursor(0, linha);
  lcd.print(buf);
}

void atualizarLCD() {
  if (millis() - tLCD < INTERVALO_LCD) return;
  tLCD = millis();

  char linha[24];
  char t[8], h[8];

  lcdLinha(0, modoManual ? "MODO: MANUAL" : "MODO: AUTOMATICO");

  if (isnan(temperatura)) strcpy(t, "--");
  else dtostrf(temperatura, 4, 1, t);
  if (isnan(humidade)) strcpy(h, "--");
  else dtostrf(humidade, 2, 0, h);
  snprintf(linha, sizeof(linha), "Temp:%sC Hum:%s%%", t, h);
  lcdLinha(1, linha);

  lcdLinha(2, digitalRead(PIN_PIR) ? "PIR: MOVIMENTO" : "PIR: SEM MOVIMENTO");
  lcdLinha(3, garagemAberta ? "GARAGEM: ABERTA" : "GARAGEM: FECHADA");
}

// ======================================================
// ENVIAR DADOS PARA O ESP
// ======================================================
void enviarDados() {
  if (millis() - tEnvio < INTERVALO_ENVIO) return;
  tEnvio = millis();

  espSerial.print(F("TEMP="));
  if (isnan(temperatura)) espSerial.print(F("NAN"));
  else espSerial.print(temperatura, 1);

  espSerial.print(F(";HUM="));
  if (isnan(humidade)) espSerial.print(F("NAN"));
  else espSerial.print(humidade, 0);

  espSerial.print(F(";PIR="));
  espSerial.print(digitalRead(PIN_PIR));

  espSerial.print(F(";PORTA="));
  espSerial.print(digitalRead(PIN_MAG));

  espSerial.print(F(";GARAGEM="));
  espSerial.print(garagemAberta ? 1 : 0);

  espSerial.print(F(";DIST="));
  espSerial.print(distanciaGaragem, 1);

  espSerial.print(F(";MODO="));
  espSerial.print(modoManual ? F("MANUAL") : F("AUTO"));

  espSerial.print(F(";ALARME="));
  espSerial.println(alarmeAtivo ? 1 : 0);
}
