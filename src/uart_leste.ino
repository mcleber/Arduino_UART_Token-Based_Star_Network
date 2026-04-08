#include <SoftwareSerial.h>
// ============================================================
// LESTE - ID 3
// ============================================================
// Conexao com o CENTRAL:
//   RX pino 2 <- TX pino 7 do CENTRAL
//   TX pino 3 -> RX pino 6 do CENTRAL
//   GND ligado ao GND do CENTRAL
//
// Funcionamento:
//   - Aguarda TOKEN do CENTRAL
//   - Ao receber TOKEN: envia proximo pacote da fila (ou "NADA")
//   - Aguarda ACK do CENTRAL antes de marcar pacote como entregue
//   - Tambem RECEBE pacotes de outros roteadores encaminhados pelo CENTRAL
//   - Ao receber pacote: exibe e envia ACK para o CENTRAL
// ============================================================

// Pinos RX=2, TX=3 (ligados ao CENTRAL)
SoftwareSerial meuSerial(2, 3);

// --- Configuracoes de timing (devem ser compativeis com o CENTRAL) ---
#define BAUD_RATE   2400   // Velocidade serial
#define DELAY_FLIT   600   // Pausa (ms) entre flits enviados
#define TIMEOUT_ACK 12000  // Timeout (ms) aguardando ACK do CENTRAL

// ============================================================
// AREA DE CONFIGURACAO - EDITE AQUI
// ============================================================
const char MEU_NOME[]    = "LESTE";
const char MEU_DESTINO[] = "OESTE"; // Destinos validos: NORTE, SUL, OESTE, CENTRAL

// Mensagens a enviar (uma por TOKEN recebido)
const char* payloads[] = {
  "LESTE 1/2",
  "LESTE 2/2"
};
int totalPacotes = 2; // Deve ser igual ao numero de entradas em payloads[]
// ============================================================

// --- Variaveis para recepcao de pacotes de outros roteadores ---
String flitsRecebidos[3]; // Armazena os 3 flits do pacote recebido
int flitCount = 0;        // Quantos flits do pacote atual ja chegaram
int pacotesRecebidos = 0; // Total de pacotes recebidos de outros roteadores

// --- Buffer circular de recepcao ---
// Armazena mensagens recebidas antes de processar.
// Evita perda de dados enquanto o loop esta ocupado.
#define BUF_RX_TAM 8
String bufRX[BUF_RX_TAM]; // Array circular de mensagens
int bufRX_head  = 0;       // Indice de leitura
int bufRX_tail  = 0;       // Indice de escrita
int bufRX_count = 0;       // Quantidade de mensagens no buffer

// ------------------------------------------------------------
// pushRX: insere uma mensagem no buffer circular.
// Retorna false se o buffer estiver cheio (mensagem descartada).
// ------------------------------------------------------------
bool pushRX(String msg) {
  if (bufRX_count >= BUF_RX_TAM) {
    // Buffer lotado: avisa e descarta
    Serial.print("[");
    Serial.print(MEU_NOME);
    Serial.println("] BUFFER RX CHEIO! Mensagem descartada.");
    return false;
  }
  bufRX[bufRX_tail] = msg;                        // Armazena no slot atual
  bufRX_tail = (bufRX_tail + 1) % BUF_RX_TAM;    // Avanca tail circularmente
  bufRX_count++;
  return true;
}

// ------------------------------------------------------------
// popRX: remove e retorna a proxima mensagem do buffer.
// Retorna "" se o buffer estiver vazio.
// ------------------------------------------------------------
String popRX() {
  if (bufRX_count == 0) return "";                // Buffer vazio
  String msg = bufRX[bufRX_head];                 // Le do slot atual
  bufRX_head = (bufRX_head + 1) % BUF_RX_TAM;    // Avanca head circularmente
  bufRX_count--;
  return msg;
}

// --- Controle de envio ---
int  pacotesEnviados  = 0;     // Quantos pacotes ja foram confirmados com ACK
bool aguardandoACK    = false; // true = enviou pacote, aguardando confirmacao
unsigned long tempoEnvio = 0;  // Momento do ultimo envio (para calculo de timeout)

// ------------------------------------------------------------
// enviarPacote: envia os 3 flits de um pacote para o CENTRAL.
// Formato dos flits:
//   Flit 1: ORIG:<MEU_NOME>
//   Flit 2: DEST:<MEU_DESTINO>
//   Flit 3: PAY:<payload>
// Aguarda DELAY_FLIT ms entre cada flit para o CENTRAL processar.
// ------------------------------------------------------------
void enviarPacote(const char* payload) {
  // Monta os 3 flits em buffers locais
  char flit1[32], flit2[32], flit3[64];
  snprintf(flit1, sizeof(flit1), "ORIG:%s", MEU_NOME);
  snprintf(flit2, sizeof(flit2), "DEST:%s", MEU_DESTINO);
  snprintf(flit3, sizeof(flit3), "PAY:%s",  payload);

  // Exibe no monitor serial sem concatenacao de String
  Serial.print("[");
  Serial.print(MEU_NOME);
  Serial.print("] Enviando pacote ");
  Serial.print(pacotesEnviados + 1);
  Serial.print("/");
  Serial.println(totalPacotes);
  Serial.println("---------------------");
  Serial.print("Flit 1 (origem):  "); Serial.println(flit1);
  Serial.print("Flit 2 (destino): "); Serial.println(flit2);
  Serial.print("Flit 3 (payload): "); Serial.println(flit3);

  // Envia cada flit com pausa entre eles
  meuSerial.println(flit1); delay(DELAY_FLIT);
  meuSerial.println(flit2); delay(DELAY_FLIT);
  meuSerial.println(flit3); delay(DELAY_FLIT);

  Serial.println(">> Pacote enviado! Aguardando ACK...");
  Serial.println("---------------------");

  tempoEnvio    = millis(); // Registra momento do envio para timeout
  aguardandoACK = true;     // Marca que estamos aguardando confirmacao
}

// ------------------------------------------------------------
// processarMensagemRecebida: processa pacote completo recebido
// de outro roteador (via CENTRAL).
// Extrai ORIG, DEST e PAY dos flitsRecebidos[] e exibe cada campo
// separadamente sem concatenacao de String para evitar corrupcao
// de heap no Arduino.
// Ao final envia ACK ao CENTRAL confirmando o recebimento.
// ------------------------------------------------------------
void processarMensagemRecebida() {
  pacotesRecebidos++; // Incrementa contador de pacotes recebidos

  // Variaveis para armazenar os campos extraidos dos flits
  String origem  = "";
  String destino = "";
  String payload = "";

  // Percorre os 3 flits e extrai cada campo pelo prefixo
  for (int i = 0; i < 3; i++) {
    if (flitsRecebidos[i].startsWith("ORIG:"))
      origem  = flitsRecebidos[i].substring(5); // Remove prefixo "ORIG:"
    else if (flitsRecebidos[i].startsWith("DEST:"))
      destino = flitsRecebidos[i].substring(5); // Remove prefixo "DEST:"
    else if (flitsRecebidos[i].startsWith("PAY:"))
      payload = flitsRecebidos[i].substring(4); // Remove prefixo "PAY:"
  }

  // Exibe resultado sem concatenacao de String (evita corrupcao de heap)
  Serial.println("==========================================");
  Serial.print("[");
  Serial.print(MEU_NOME);
  Serial.print("] MENSAGEM RECEBIDA #");
  Serial.println(pacotesRecebidos);
  Serial.print("  De:     "); Serial.println(origem);
  Serial.print("  Para:   "); Serial.println(destino);
  Serial.print("  Msg:    "); Serial.println(payload);
  Serial.println("==========================================");

  // Limpa o array de flits para o proximo pacote
  for (int i = 0; i < 3; i++) flitsRecebidos[i] = "";

  // Envia ACK ao CENTRAL confirmando que recebemos o pacote
  delay(DELAY_FLIT);
  meuSerial.println("ACK");
  Serial.print("[");
  Serial.print(MEU_NOME);
  Serial.println("] ACK enviado para CENTRAL");
}

// ------------------------------------------------------------
// setup: inicializa as portas seriais e aguarda estabilizacao.
// ------------------------------------------------------------
void setup() {
  Serial.begin(2400);         // Serial de debug (monitor serial)
  meuSerial.begin(BAUD_RATE); // Serial de comunicacao com o CENTRAL
  delay(2000);                // Aguarda estabilizacao do hardware

  // Exibe informacoes de inicializacao sem concatenacao
  Serial.println("============================================");
  Serial.print(MEU_NOME);
  Serial.print(" pronto! Destino: ");
  Serial.println(MEU_DESTINO);
  Serial.print("Pacotes a enviar: ");
  Serial.println(totalPacotes);
  Serial.print("[");
  Serial.print(MEU_NOME);
  Serial.print("] Buffer RX: ");
  Serial.print(BUF_RX_TAM);
  Serial.println(" slots");
  Serial.println("Aguardando TOKEN do CENTRAL...");
  Serial.println("============================================");
}

// ------------------------------------------------------------
// loop: ciclo principal de operacao.
//   Passo 1 - Leitura: drena tudo disponivel na serial para o buffer.
//   Passo 2 - Processamento: consome uma mensagem do buffer por iteracao.
//     - Flits (ORIG/DEST/PAY): acumula ate completar 3, entao processa
//     - TOKEN: envia proximo pacote ou NADA
//     - ACK: confirma entrega do ultimo pacote enviado
// ------------------------------------------------------------
void loop() {
  // --- Passo 1: drena a serial para o buffer ---
  while (meuSerial.available() > 0) {
    String msg = meuSerial.readStringUntil('\n'); // Le ate fim de linha
    msg.trim();                                   // Remove \r e espacos
    if (msg.length() > 0) pushRX(msg);           // Empurra no buffer se nao vazio
  }

  // --- Passo 2: consome uma mensagem do buffer ---
  String msg = popRX();
  if (msg == "") {
    // Buffer vazio: verifica se houve timeout esperando ACK
    if (aguardandoACK && millis() - tempoEnvio > TIMEOUT_ACK) {
      Serial.print("[");
      Serial.print(MEU_NOME);
      Serial.println("] Timeout! Pacote sera reenviado no proximo TOKEN.");
      aguardandoACK = false; // Libera para reenvio no proximo TOKEN
    }
    return;
  }

  // --- Trata flits de pacotes recebidos (ORIG:, DEST:, PAY:) ---
  if (msg.startsWith("ORIG:") || msg.startsWith("DEST:") || msg.startsWith("PAY:")) {

    // Se chegou ORIG com flitCount != 0 houve desalinhamento: reseta tudo
    if (msg.startsWith("ORIG:") && flitCount != 0) {
      Serial.print("[");
      Serial.print(MEU_NOME);
      Serial.println("] DESALINHAMENTO! Resetando flitCount.");
      flitCount = 0;
      for (int i = 0; i < 3; i++) flitsRecebidos[i] = "";
    }

    flitsRecebidos[flitCount] = msg; // Armazena flit na posicao correta
    flitCount++;

    // Exibe flit recebido sem concatenacao
    Serial.print("[");
    Serial.print(MEU_NOME);
    Serial.print("] Flit recebido ");
    Serial.print(flitCount);
    Serial.print(": ");
    Serial.println(flitsRecebidos[flitCount - 1]);

    // Quando os 3 flits chegarem processa o pacote completo
    if (flitCount == 3) {
      processarMensagemRecebida();
      flitCount = 0; // Reseta contador para o proximo pacote
      // Descarta bytes residuais que possam ter chegado durante o processamento
      while (meuSerial.available() > 0) meuSerial.read();
    }
    return;
  }

  // --- Trata TOKEN: envia proximo pacote ou sinaliza fim ---
  if (msg == "TOKEN") {
    Serial.print("[");
    Serial.print(MEU_NOME);
    Serial.println("] TOKEN recebido!");

    if (pacotesEnviados >= totalPacotes) {
      // Todos os pacotes ja foram enviados e confirmados
      Serial.print("[");
      Serial.print(MEU_NOME);
      Serial.println("] Nada a enviar. Respondendo NADA.");
      meuSerial.println("NADA"); // Sinaliza ao CENTRAL que terminou
    } else {
      // Ainda ha pacotes na fila: envia o proximo
      enviarPacote(payloads[pacotesEnviados]);
    }
    return;
  }

  // --- Trata ACK: confirma entrega do pacote que enviamos ---
  if (msg == "ACK") {
    if (aguardandoACK) {
      // ACK recebido dentro do tempo: pacote entregue com sucesso
      Serial.print("[");
      Serial.print(MEU_NOME);
      Serial.print("] ACK recebido! Pacote ");
      Serial.print(pacotesEnviados + 1);
      Serial.println(" entregue.");
      pacotesEnviados++;     // Avanca para o proximo pacote da fila
      aguardandoACK = false; // Libera flag de espera

      if (pacotesEnviados >= totalPacotes) {
        // Todos os pacotes foram confirmados
        Serial.print("[");
        Serial.print(MEU_NOME);
        Serial.print("] Todos os ");
        Serial.print(totalPacotes);
        Serial.println(" pacotes enviados com sucesso!");
      }
    }
    return;
  }

  // --- Mensagem nao reconhecida ---
  Serial.print("[");
  Serial.print(MEU_NOME);
  Serial.print("] Mensagem desconhecida: ");
  Serial.println(msg);
}