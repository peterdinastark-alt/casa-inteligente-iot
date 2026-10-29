#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>   // base de dados (histórico)

// ======================================================
// WI-FI (modo ponto de acesso) E LOGIN DA PÁGINA
// ======================================================
// TROQUE ESTAS SENHAS ANTES DE USAR!
const char* ssid = "Casa_Controle";
const char* password = "troque_esta_senha";   // mínimo 8 caracteres
const char* USUARIO = "admin";
const char* SENHA_WEB = "troque_esta_senha_web";

ESP8266WebServer server(80);

// ======================================================
// LIGAÇÕES
// ======================================================
// Serial (GPIO1 TX / GPIO3 RX) <-> Arduino, SEMPRE via conversor de nível.
// ATENÇÃO: ao gravar o ESP por USB, desconecte os fios TX/RX do Arduino,
// pois a gravação usa essa mesma serial.
#define LED_STATUS 4
#define SENSOR_MAG_GARAGEM 5   // HIGH = garagem aberta (ímã afastado)

// ======================================================
// VARIÁVEIS
// ======================================================
// Sensor da garagem (com debounce)
bool garagemAberta = false;
bool leituraAnterior = false;
unsigned long tMudanca = 0;
unsigned long tReenvio = 0;

// Últimos dados recebidos do Arduino
String dTemp = "--", dHum = "--", dPir = "0", dPorta = "0";
String dGaragem = "0", dDist = "--", dModo = "--", dAlarme = "1";
unsigned long ultimoDado = 0;

// Buffer da linha recebida do Arduino
char linha[100];
uint8_t idxLinha = 0;

// ======================================================
// BASE DE DADOS (HISTÓRICO DE EVENTOS) - LittleFS
// ======================================================
// Guarda os eventos num arquivo na memória flash do ESP.
// Cada linha do arquivo: boot;tempo;tipo;detalhe
//   boot  = número de vezes que o ESP foi ligado
//   tempo = hh:mm:ss desde que o ESP ligou (não há relógio nem internet)
//
// NA IDE: Ferramentas > Flash Size > escolha uma opção com FS,
// por exemplo "4MB (FS:2MB OTA:~1019KB)".
// Se não houver FS, o resto do sistema funciona igual, sem histórico.
//
// Páginas (pedem o mesmo usuário e senha):
//   /historico      -> tabela com os eventos
//   /historico.csv  -> baixar o histórico
//   /historico/limpar -> apagar o histórico
// ======================================================
bool autenticado();

bool fsOk = false;
unsigned long numeroBoot = 0;
const size_t TAMANHO_MAX_LOG = 16000;               // bytes por arquivo
const unsigned long INTERVALO_TEMP_LOG = 600000UL;  // temperatura a cada 10 min

// Últimos valores já registrados (para detectar mudanças)
String uPorta = "", uPir = "", uModo = "", uAlarme = "";
bool uOnline = false;
bool uPerto = false;
unsigned long tUltimoPir = 0;
unsigned long tUltimaTemp = 0;

const char HIST_TOPO[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="pt">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Histórico</title>
<style>
body{font-family:Arial,sans-serif;background:#eee;margin:0;padding:12px}
h1{font-size:22px;text-align:center}
table{border-collapse:collapse;width:100%;max-width:520px;margin:10px auto;background:#fff;font-size:14px}
th,td{border:1px solid #ccc;padding:5px 7px;text-align:left}
th{background:#ddd}
p,small{display:block;text-align:center}
small{color:#555}
a{margin:0 8px}
</style>
</head>
<body>
<h1>Histórico de eventos</h1>
<p><a href="/">Voltar</a><a href="/historico.csv">Baixar</a><a href="#" onclick="if(confirm('Apagar todo o histórico?'))location='/historico/limpar';return false">Limpar</a></p>
<small>Boot = nº da ligação do ESP. Tempo = hh:mm:ss desde que ele ligou.</small>
<table>
<tr><th>Boot</th><th>Tempo</th><th>Tipo</th><th>Detalhe</th></tr>
)=====";

String tempoDesdeLigar() {
  unsigned long s = millis() / 1000;
  char b[16];
  snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  return String(b);
}

// Acrescenta uma linha ao histórico
void registrarEvento(const char* tipo, const String& detalhe) {
  if (!fsOk) return;

  String d = detalhe;
  d.replace(";", ",");
  d.replace("<", "");
  d.replace(">", "");

  File f = LittleFS.open("/log.txt", "a");
  if (!f) return;

  // Arquivo cheio: o atual vira "antigo" e começa um novo
  if (f.size() > TAMANHO_MAX_LOG) {
    f.close();
    LittleFS.remove("/log_old.txt");
    LittleFS.rename("/log.txt", "/log_old.txt");
    f = LittleFS.open("/log.txt", "a");
    if (!f) return;
  }

  f.print(numeroBoot);
  f.print(";");
  f.print(tempoDesdeLigar());
  f.print(";");
  f.print(tipo);
  f.print(";");
  f.println(d);
  f.close();
}

// Envia as linhas de um arquivo como linhas da tabela
void enviarLinhasTabela(const char* caminho) {
  File f = LittleFS.open(caminho, "r");
  if (!f) return;

  while (f.available()) {
    String l = f.readStringUntil('\n');
    l.trim();
    if (l.length() == 0) continue;

    String linhaHtml = "<tr>";
    int ini = 0;
    for (int c = 0; c < 4; c++) {
      int p = (c < 3) ? l.indexOf(';', ini) : -1;
      String campo = (p >= 0) ? l.substring(ini, p) : l.substring(ini);
      linhaHtml += "<td>" + campo + "</td>";
      if (p < 0) break;
      ini = p + 1;
    }
    linhaHtml += "</tr>";
    server.sendContent(linhaHtml);
    yield();
  }
  f.close();
}

// Envia as linhas de um arquivo sem formatar (para baixar)
void enviarArquivoBruto(const char* caminho) {
  File f = LittleFS.open(caminho, "r");
  if (!f) return;

  while (f.available()) {
    String l = f.readStringUntil('\n');
    server.sendContent(l + "\n");
    yield();
  }
  f.close();
}

void paginaHistorico() {
  if (!autenticado()) return;

  if (!fsOk) {
    server.send(200, "text/plain",
                "Historico indisponivel: em Ferramentas > Flash Size, escolha uma opcao com FS.");
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent_P(HIST_TOPO);
  enviarLinhasTabela("/log_old.txt");
  enviarLinhasTabela("/log.txt");
  server.sendContent("</table><script>window.scrollTo(0,document.body.scrollHeight)</script></body></html>");
  server.sendContent("");
}

void baixarHistorico() {
  if (!autenticado()) return;

  if (!fsOk) {
    server.send(200, "text/plain", "Historico indisponivel.");
    return;
  }

  server.sendHeader("Content-Disposition", "attachment; filename=historico.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");
  server.sendContent("boot;tempo;tipo;detalhe\n");
  enviarArquivoBruto("/log_old.txt");
  enviarArquivoBruto("/log.txt");
  server.sendContent("");
}

void limparHistorico() {
  if (!autenticado()) return;

  if (fsOk) {
    LittleFS.remove("/log.txt");
    LittleFS.remove("/log_old.txt");
    registrarEvento("SISTEMA", "HISTORICO LIMPO");
  }
  server.sendHeader("Location", "/historico");
  server.send(303, "text/plain", "");
}

// Chamada uma vez no setup(): abre a memória e cria as páginas
void iniciarHistorico() {
  fsOk = LittleFS.begin();

  if (fsOk) {
    File f = LittleFS.open("/boot.txt", "r");
    if (f) {
      numeroBoot = f.parseInt();
      f.close();
    }
    numeroBoot++;
    f = LittleFS.open("/boot.txt", "w");
    if (f) {
      f.print(numeroBoot);
      f.close();
    }
    registrarEvento("SISTEMA", "LIGADO");
  }

  server.on("/historico", paginaHistorico);
  server.on("/historico.csv", baixarHistorico);
  server.on("/historico/limpar", limparHistorico);
}

// Chamada a cada volta do loop(): registra o que mudou
void registrarMudancas() {
  if (!fsOk) return;

  // Arduino respondendo ou sem sinal
  bool online = (ultimoDado != 0) && (millis() - ultimoDado < 6000);
  if (online != uOnline) {
    uOnline = online;
    registrarEvento("ARDUINO", online ? "CONECTADO" : "SEM SINAL");
  }
  if (!online) return;

  // Porta principal
  if (dPorta != uPorta) {
    uPorta = dPorta;
    registrarEvento("PORTA", dPorta == "1" ? "ABERTA" : "FECHADA");
  }

  // Modo automático / manual
  if (dModo != uModo) {
    uModo = dModo;
    registrarEvento("MODO", dModo);
  }

  // Alarme sonoro
  if (dAlarme != uAlarme) {
    uAlarme = dAlarme;
    registrarEvento("ALARME", dAlarme == "1" ? "ATIVO" : "SILENCIADO");
  }

  // Movimento (no máximo 1 registro por minuto)
  if (dPir != uPir) {
    uPir = dPir;
    if (dPir == "1" && (tUltimoPir == 0 || millis() - tUltimoPir > 60000UL)) {
      tUltimoPir = millis();
      registrarEvento("MOVIMENTO", "DETECTADO");
    }
  }

  // Objeto a menos de 50 cm da garagem
  float dist = dDist.toFloat();
  bool perto = (dist > 0 && dist <= 50.0);
  if (perto != uPerto) {
    uPerto = perto;
    registrarEvento("OBJETO", perto ? "PROXIMO DA GARAGEM" : "AFASTOU-SE");
  }

  // Temperatura e umidade a cada 10 minutos
  if ((tUltimaTemp == 0 || millis() - tUltimaTemp >= INTERVALO_TEMP_LOG) &&
      dTemp != "NAN" && dTemp != "--") {
    tUltimaTemp = millis();
    registrarEvento("TEMPERATURA", dTemp + " C / " + dHum + " %");
  }
}

// ======================================================
// PÁGINA WEB (guardada na flash)
// ======================================================
const char PAGINA[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="pt">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Casa Inteligente</title>
<style>
body{font-family:Arial,sans-serif;text-align:center;background:#eee;margin:0;padding:12px}
h1{color:#222;font-size:24px}
h2{margin:18px 0 6px;font-size:18px}
button{width:44%;max-width:180px;height:52px;margin:5px;font-size:16px;border-radius:10px;border:1px solid #888;background:#fff}
button:active{background:#cde}
.card{background:#fff;border-radius:12px;padding:10px;margin:10px auto;max-width:420px;text-align:left}
.card div{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #eee}
#msg{min-height:22px;color:#06c;margin-top:10px}
</style>
</head>
<body>
<h1>🏠 CASA INTELIGENTE</h1>

<div class="card">
<div><span>Arduino</span><b id="online">--</b></div>
<div><span>Temperatura</span><b id="temp">--</b></div>
<div><span>Umidade</span><b id="hum">--</b></div>
<div><span>Movimento</span><b id="pir">--</b></div>
<div><span>Porta</span><b id="porta">--</b></div>
<div><span>Garagem</span><b id="garagem">--</b></div>
<div><span>Distância</span><b id="dist">--</b></div>
<div><span>Modo</span><b id="modo">--</b></div>
<div><span>Alarme sonoro</span><b id="alarme">--</b></div>
</div>
<div id="msg"></div>

<h2>⚙️ Modo</h2>
<button onclick="cmd('/auto')">AUTOMÁTICO</button>
<button onclick="cmd('/manual')">MANUAL</button>

<h2>💡 Lâmpada da sala</h2>
<button onclick="cmd('/lampada1/on')">LIGAR</button>
<button onclick="cmd('/lampada1/off')">DESLIGAR</button>

<h2>💡 Lâmpada do quarto</h2>
<button onclick="cmd('/lampada2/on')">LIGAR</button>
<button onclick="cmd('/lampada2/off')">DESLIGAR</button>

<h2>💡 Lâmpada da cozinha</h2>
<button onclick="cmd('/lampada3/on')">LIGAR</button>
<button onclick="cmd('/lampada3/off')">DESLIGAR</button>

<h2>🌀 Cooler</h2>
<button onclick="cmd('/cooler/on')">LIGAR</button>
<button onclick="cmd('/cooler/off')">DESLIGAR</button>

<h2>🚪 Porta principal</h2>
<button onclick="cmd('/servo/abrir')">ABRIR</button>
<button onclick="cmd('/servo/fechar')">FECHAR</button>

<h2>🚗 Garagem</h2>
<button onclick="cmd('/garagem/abrir')">ABRIR</button>
<button onclick="cmd('/garagem/fechar')">FECHAR</button>

<h2>🔔 Alarme sonoro</h2>
<button onclick="cmd('/alarme/on')">ATIVAR</button>
<button onclick="cmd('/alarme/off')">SILENCIAR</button>

<script>
function $(i){return document.getElementById(i)}
function cmd(p){
  fetch(p).then(function(r){return r.text()})
    .then(function(t){$('msg').textContent=t;setTimeout(atualizar,600)})
    .catch(function(){$('msg').textContent='Sem conexão com a casa'});
}
function atualizar(){
  fetch('/dados').then(function(r){return r.json()}).then(function(d){
    $('online').textContent=d.online?'Conectado':'Sem sinal';
    $('temp').textContent=d.temp+' °C';
    $('hum').textContent=d.hum+' %';
    $('pir').textContent=d.pir=='1'?'Movimento':'Sem movimento';
    $('porta').textContent=d.porta=='1'?'Aberta':'Fechada';
    $('garagem').textContent=d.garagem=='1'?'Aberta':'Fechada';
    $('dist').textContent=d.dist+' cm';
    $('modo').textContent=d.modo;
    $('alarme').textContent=d.alarme=='1'?'Ativo':'Silenciado';
  }).catch(function(){$('online').textContent='Sem conexão'});
}
setInterval(atualizar,2000);
atualizar();
</script>
</body>
</html>
)=====";

// ======================================================
// FUNÇÕES AUXILIARES
// ======================================================
void enviarComando(const char* comando) {
  Serial.println(comando);
}

void enviarEstadoGaragem() {
  enviarComando(garagemAberta ? "GARAGEM_ABERTA" : "GARAGEM_FECHADA");
}

bool autenticado() {
  if (server.authenticate(USUARIO, SENHA_WEB)) return true;
  server.requestAuthentication();
  return false;
}

// Cria uma rota que envia um comando ao Arduino
void rota(const char* caminho, const char* comando, const char* msg) {
  server.on(caminho, [=]() {
    if (!autenticado()) return;
    enviarComando(comando);
    registrarEvento("COMANDO", comando);  // base de dados
    server.send(200, "text/plain", msg);
  });
}

void enviarDadosJson() {
  if (!autenticado()) return;

  bool online = (ultimoDado != 0) && (millis() - ultimoDado < 6000);

  String j = "{\"online\":";
  j += online ? "true" : "false";
  j += ",\"temp\":\"" + dTemp + "\"";
  j += ",\"hum\":\"" + dHum + "\"";
  j += ",\"pir\":\"" + dPir + "\"";
  j += ",\"porta\":\"" + dPorta + "\"";
  j += ",\"garagem\":\"" + dGaragem + "\"";
  j += ",\"dist\":\"" + dDist + "\"";
  j += ",\"modo\":\"" + dModo + "\"";
  j += ",\"alarme\":\"" + dAlarme + "\"}";

  server.send(200, "application/json", j);
}

// ======================================================
// RECEBER DADOS DO ARDUINO
// Formato: TEMP=27.5;HUM=60;PIR=0;PORTA=0;GARAGEM=0;DIST=120.0;MODO=AUTO;ALARME=1
// ======================================================
void processarLinha(char* s) {
  // Arduino acabou de ligar: reenviar o estado da garagem
  if (!strcmp(s, "ARDUINO_PRONTO")) {
    enviarEstadoGaragem();
    return;
  }
  if (!strchr(s, '=')) return;  // ignora lixo

  ultimoDado = millis();

  char* par = strtok(s, ";");
  while (par) {
    char* eq = strchr(par, '=');
    if (eq) {
      *eq = '\0';
      const char* k = par;
      const char* v = eq + 1;
      if (!strcmp(k, "TEMP")) dTemp = v;
      else if (!strcmp(k, "HUM")) dHum = v;
      else if (!strcmp(k, "PIR")) dPir = v;
      else if (!strcmp(k, "PORTA")) dPorta = v;
      else if (!strcmp(k, "GARAGEM")) dGaragem = v;
      else if (!strcmp(k, "DIST")) dDist = v;
      else if (!strcmp(k, "MODO")) dModo = v;
      else if (!strcmp(k, "ALARME")) dAlarme = v;
    }
    par = strtok(NULL, ";");
  }
}

void lerArduino() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      linha[idxLinha] = '\0';
      if (idxLinha > 0) processarLinha(linha);
      idxLinha = 0;
    } else if (idxLinha < sizeof(linha) - 1) {
      linha[idxLinha++] = c;
    } else {
      idxLinha = 0;  // linha longa demais: descarta
    }
  }
}

// ======================================================
// SENSOR MAGNÉTICO DA GARAGEM (debounce de 50 ms)
// Envia ao mudar e reenvia a cada 3 s (o Arduino recupera o estado
// se algum dos dois reiniciar).
// ======================================================
void lerGaragem() {
  bool leitura = (digitalRead(SENSOR_MAG_GARAGEM) == HIGH);

  if (leitura != leituraAnterior) {
    leituraAnterior = leitura;
    tMudanca = millis();
  }

  if (millis() - tMudanca > 50 && leitura != garagemAberta) {
    garagemAberta = leitura;
    registrarEvento("GARAGEM", leitura ? "ABERTA" : "FECHADA");  // base de dados
    enviarEstadoGaragem();
    tReenvio = millis();
  }

  if (millis() - tReenvio > 3000) {
    tReenvio = millis();
    enviarEstadoGaragem();
  }
}

// LED fixo = comunicando com o Arduino; piscando = sem sinal
void atualizarLed() {
  bool online = (ultimoDado != 0) && (millis() - ultimoDado < 6000);
  if (online) digitalWrite(LED_STATUS, HIGH);
  else digitalWrite(LED_STATUS, (millis() / 500) % 2);
}

// ======================================================
// SETUP
// ======================================================
void setup() {
  Serial.begin(9600);  // ESP <-> Arduino

  pinMode(LED_STATUS, OUTPUT);
  digitalWrite(LED_STATUS, LOW);
  pinMode(SENSOR_MAG_GARAGEM, INPUT_PULLUP);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);

  server.on("/", []() {
    if (!autenticado()) return;
    server.send_P(200, "text/html", PAGINA);
  });

  server.on("/dados", enviarDadosJson);
  server.on("/status", enviarDadosJson);

  rota("/lampada1/on", "LIGA_SALA", "Lampada da sala ligada");
  rota("/lampada1/off", "DESLIGA_SALA", "Lampada da sala desligada");
  rota("/lampada2/on", "LIGA_QUARTO", "Lampada do quarto ligada");
  rota("/lampada2/off", "DESLIGA_QUARTO", "Lampada do quarto desligada");
  rota("/lampada3/on", "LIGA_COZINHA", "Lampada da cozinha ligada");
  rota("/lampada3/off", "DESLIGA_COZINHA", "Lampada da cozinha desligada");
  rota("/cooler/on", "LIGA_COOLER", "Cooler ligado");
  rota("/cooler/off", "DESLIGA_COOLER", "Cooler desligado");
  rota("/servo/abrir", "ABRIR_PORTA", "Porta principal aberta");
  rota("/servo/fechar", "FECHAR_PORTA", "Porta principal fechada");
  rota("/garagem/abrir", "ABRIR_GARAGEM", "Comando enviado: abrir garagem");
  rota("/garagem/fechar", "FECHAR_GARAGEM", "Comando enviado: fechar garagem");
  rota("/auto", "AUTO", "Modo automatico ativado");
  rota("/manual", "MANUAL", "Modo manual ativado");
  rota("/alarme/on", "ALARME_ON", "Alarme sonoro ativado");
  rota("/alarme/off", "ALARME_OFF", "Alarme sonoro silenciado");

  iniciarHistorico();  // base de dados (LittleFS)

  server.begin();
}

// ======================================================
// LOOP
// ======================================================
void loop() {
  server.handleClient();
  lerArduino();
  lerGaragem();
  atualizarLed();
  registrarMudancas();  // base de dados
}
