#include <SoftwareSerial.h>
// ============================================================
// CENTRAL - ID 0
// ============================================================
// Topologia: ESTRELA
//   NORTE : RX pino 2 | TX pino 3
//   SUL   : RX pino 4 | TX pino 5
//   LESTE : RX pino 6 | TX pino 7
//   OESTE : RX pino 8 | TX pino 9
//
// Funcionamento:
//   - Passa TOKEN para cada roteador na ordem NORTE->SUL->LESTE->OESTE
//   - Recebe pacote (3 flits: ORIG, DEST, PAY) do roteador com token
//   - Repassa o pacote ao roteador destino
//   - Aguarda ACK do destino antes de confirmar para a origem
//   - So avanca para o proximo roteador apos entrega confirmada
//   - Roteador que responde NADA e marcado como finalizado
// ============================================================

// --- Objetos TX (so enviam, RX invalido = 255) ---
SoftwareSerial txNORTE(255, 3);
SoftwareSerial txSUL  (255, 5);
SoftwareSerial txLESTE(255, 7);
SoftwareSerial txOESTE(255, 9);

// --- Objetos RX (so recebem, TX invalido = 255) ---
SoftwareSerial rxNORTE(2, 255);
SoftwareSerial rxSUL  (4, 255);
SoftwareSerial rxLESTE(6, 255);
SoftwareSerial rxOESTE(8, 255);

// Arrays indexados: 0=NORTE, 1=SUL, 2=LESTE, 3=OESTE
SoftwareSerial* txCanais[4] = {&txNORTE, &txSUL, &txLESTE, &txOESTE};
SoftwareSerial* rxCanais[4] = {&rxNORTE, &rxSUL, &rxLESTE, &rxOESTE};
String nomesCanais[4] = {"NORTE", "SUL", "LESTE", "OESTE"};

// --- Configuracoes de timing ---
#define BAUD_RATE          2400   // Velocidade serial (igual em todos os nos)
#define ESPERA_APOS_TOKEN  2000   // Tempo (ms) aguardando resposta apos enviar TOKEN
#define TIMEOUT_FLIT       3000   // Tempo maximo (ms) aguardando cada flit
#define TIMEOUT_ACK_DESTINO 8000  // Tempo maximo (ms) aguardando ACK do destino
#define DELAY_FLIT          800   // Pausa (ms) entre envio de flits consecutivos
// DELAY_FLIT_EXTRA: pausa adicional apos enviar o 3o flit, antes de escutar ACK.
// Necessario porque o destino precisa de tempo para processar os 3 flits
// e enviar o ACK. Sem este delay o Central ainda esta no ultimo delay(DELAY_FLIT)
// quando o ACK chega e perde a mensagem.
#define DELAY_APOS_ULTIMO_FLIT 1500

// --- Controle de estado ---
int canalAtual = 0;               // Indice do canal que esta com o token agora
bool roteadorAtivo[4] = {true, true, true, true}; // true = ainda tem pacotes
int roteadoresAtivos = 4;         // Quantos roteadores ainda nao finalizaram

// --- Estatisticas ---
int pacotesRepassados = 0;        // Total de pacotes entregues com sucesso
int pacotesCentral = 0;           // Total de pacotes destinados ao proprio Central

// ------------------------------------------------------------
// Retorna o indice (0-3) de um canal pelo nome, ou -1 se nao encontrado
// ------------------------------------------------------------
int getIdxCanal(String nome) {
  for (int i = 0; i < 4; i++) {
    if (nomesCanais[i] == nome) return i;
  }
  return -1;
}

// ------------------------------------------------------------
// Descarta todos os bytes pendentes no buffer RX de um canal.
// Deve ser chamado antes de comecar a escutar um canal para
// evitar que bytes residuais de transmissoes anteriores
// corrompam a leitura atual.
// ------------------------------------------------------------
void limparBuffer(int canal) {
  rxCanais[canal]->listen(); // Ativa este canal para recepcao
  delay(50);                 // Aguarda qualquer byte em transito chegar
  while (rxCanais[canal]->available()) {
    rxCanais[canal]->read(); // Descarta cada byte residual
  }
}

// ------------------------------------------------------------
// Le uma linha do canal especificado ate '\n' ou timeout.
// Retorna a string sem espacos/\r nas bordas, ou "" se timeout.
// ------------------------------------------------------------
String lerLinha(int canal, unsigned long timeout) {
  unsigned long ini = millis();
  while (millis() - ini < timeout) {
    if (rxCanais[canal]->available()) {
      String linha = rxCanais[canal]->readStringUntil('\n');
      linha.trim(); // Remove \r e espacos
      if (linha.length() > 0) return linha;
    }
  }
  return ""; // Timeout
}

// ------------------------------------------------------------
// Envia TOKEN para o canal indicado e aguarda os 3 flits
// (ORIG, DEST, PAY) do roteador. Preenche orig, dest, pay.
// Retorna true se pacote completo recebido, false caso contrario.
// Se o roteador responder NADA, marca-o como finalizado.
// ------------------------------------------------------------
bool coletarPacote(int canal, String& orig, String& dest, String& pay) {
  if (!roteadorAtivo[canal]) return false;

  // Limpa residuos antes de escutar
  limparBuffer(canal);
  rxCanais[canal]->listen();

  // Envia o token
  Serial.println("[CENTRAL] TOKEN -> " + nomesCanais[canal]);
  txCanais[canal]->println("TOKEN");

  // Aguarda o roteador processar o TOKEN e comecar a enviar
  delay(ESPERA_APOS_TOKEN);

  // Le flit 1: ORIG
  orig = lerLinha(canal, TIMEOUT_FLIT);
  if (orig == "" || orig == "NADA") {
    if (orig == "NADA") {
      Serial.println("[CENTRAL] " + nomesCanais[canal] + ": terminou!");
      roteadorAtivo[canal] = false;
      roteadoresAtivos--;
    }
    return false;
  }

  // Le flit 2: DEST
  dest = lerLinha(canal, TIMEOUT_FLIT);
  if (dest == "") {
    Serial.println("[CENTRAL] Timeout DEST de " + nomesCanais[canal]);
    return false;
  }

  // Le flit 3: PAY
  pay = lerLinha(canal, TIMEOUT_FLIT);
  if (pay == "") {
    Serial.println("[CENTRAL] Timeout PAY de " + nomesCanais[canal]);
    return false;
  }

  Serial.println("[CENTRAL] Pacote: " + orig + " | " + dest + " | " + pay);
  return true;
}

// ------------------------------------------------------------
// Repassa um pacote da origem para o destino e aguarda ACK
// do destino para confirmar entrega.
// Fluxo:
//   1. Limpa buffer do destino
//   2. Envia os 3 flits ao destino
//   3. Aguarda ACK do destino (TIMEOUT_ACK_DESTINO ms)
//   4. Se ACK recebido: envia ACK para a origem e retorna true
//   5. Se timeout: nao envia ACK para origem (ela reenviara)
// ------------------------------------------------------------
bool repassarEAguardarACKdoDestino(int canalOrigem, int canalDestino,
                                    String orig, String dest, String pay) {
  Serial.println("[CENTRAL] REPASSANDO " + nomesCanais[canalOrigem] +
                 " -> " + nomesCanais[canalDestino]);
  Serial.println("[CENTRAL] Aguardando ACK do destino...");

  // Limpa residuos do canal destino antes de escutar
  limparBuffer(canalDestino);

  // Ativa recepcao no canal destino (para capturar o ACK que vira apos os flits)
  rxCanais[canalDestino]->listen();

  // Envia flit 1: ORIG
  txCanais[canalDestino]->println(orig);
  delay(DELAY_FLIT);

  // Envia flit 2: DEST
  txCanais[canalDestino]->println(dest);
  delay(DELAY_FLIT);

  // Envia flit 3: PAY
  txCanais[canalDestino]->println(pay);

  // Aguarda tempo extra apos o ultimo flit:
  // O destino precisa processar os 3 flits e preparar o ACK.
  // Sem esta pausa o Central comeca a escutar antes do ACK chegar.
  delay(DELAY_APOS_ULTIMO_FLIT);

  // Aguarda ACK do destino
  unsigned long inicioEspera = millis();
  bool ackRecebido = false;

  while (millis() - inicioEspera < TIMEOUT_ACK_DESTINO) {
    if (rxCanais[canalDestino]->available()) {
      String resposta = rxCanais[canalDestino]->readStringUntil('\n');
      resposta.trim();
      if (resposta == "ACK") {
        ackRecebido = true;
        Serial.println(">>> ACK do destino " + nomesCanais[canalDestino] + " recebido!");
        break;
      }
    }
    delay(50);
  }

  if (ackRecebido) {
    // Volta a escutar a origem para enviar o ACK de confirmacao
    rxCanais[canalOrigem]->listen();
    delay(DELAY_FLIT);
    txCanais[canalOrigem]->println("ACK");
    pacotesRepassados++;
    Serial.print(">>> ACK enviado para ");
    Serial.print(nomesCanais[canalOrigem]);
    Serial.println(" (destino confirmou)");
    return true;
  } else {
    // Timeout: destino nao respondeu. Origem nao recebe ACK e
    // reenviara o mesmo pacote no proximo TOKEN.
    Serial.println("[CENTRAL] TIMEOUT! Destino " + nomesCanais[canalDestino] +
                   " nao respondeu.");
    rxCanais[canalOrigem]->listen();
    Serial.print(">>> Origem ");
    Serial.print(nomesCanais[canalOrigem]);
    Serial.println(" nao recebeu ACK (reenviara no proximo TOKEN)");
    return false;
  }
}

// ------------------------------------------------------------
// SETUP: inicializa serial e todos os canais TX/RX
// ------------------------------------------------------------
void setup() {
  Serial.begin(2400);

  // Inicia todos os canais TX
  txNORTE.begin(BAUD_RATE);
  txSUL.begin(BAUD_RATE);
  txLESTE.begin(BAUD_RATE);
  txOESTE.begin(BAUD_RATE);

  // Inicia todos os canais RX
  rxNORTE.begin(BAUD_RATE);
  rxSUL.begin(BAUD_RATE);
  rxLESTE.begin(BAUD_RATE);
  rxOESTE.begin(BAUD_RATE);

  // Marca todos os roteadores como ativos
  for (int i = 0; i < 4; i++) roteadorAtivo[i] = true;
  roteadoresAtivos = 4;

  // Aguarda roteadores inicializarem
  delay(4000);

  Serial.println("============================================");
  Serial.println("CENTRAL");
  Serial.println("So passa token apos destino confirmar recebimento!");
  Serial.println("Ordem: NORTE -> SUL -> LESTE -> OESTE");
  Serial.println("============================================");
}

// ------------------------------------------------------------
// LOOP principal: ciclo de token entre os roteadores ativos
// ------------------------------------------------------------
void loop() {
  // Se todos finalizaram, exibe resumo e para
  if (roteadoresAtivos == 0) {
    Serial.println("============================================");
    Serial.println("TODOS OS ROTEADORES FINALIZARAM!");
    Serial.println("Total de pacotes repassados: " + String(pacotesRepassados));
    Serial.println("Total de pacotes para CENTRAL: " + String(pacotesCentral));
    Serial.println("============================================");
    delay(10000);
    return;
  }

  // Pula canais finalizados para chegar no proximo ativo
  int tentativas = 0;
  while (!roteadorAtivo[canalAtual] && tentativas < 4) {
    canalAtual = (canalAtual + 1) % 4;
    tentativas++;
  }

  // Coleta pacote do roteador atual (se houver)
  String orig = "", dest = "", pay = "";
  bool temPacote = coletarPacote(canalAtual, orig, dest, pay);

  if (temPacote) {
    // Extrai nome do destino removendo o prefixo "DEST:"
    String destNome = dest.substring(5);
    String origemNome = nomesCanais[canalAtual];

    if (destNome == "CENTRAL") {
      // Pacote destinado ao proprio Central: exibe e confirma
      pacotesCentral++;
      Serial.println(">>> MSG PARA CENTRAL");
      Serial.println("    Origem:  " + origemNome);
      Serial.println("    Payload: " + pay.substring(4)); // Remove prefixo "PAY:"
      delay(DELAY_FLIT);
      txCanais[canalAtual]->println("ACK");
      Serial.println(">>> ACK -> " + origemNome);
      canalAtual = (canalAtual + 1) % 4; // Avanca para o proximo canal

    } else {
      // Pacote para outro roteador: repassa e aguarda confirmacao
      int idxDest = getIdxCanal(destNome);

      if (idxDest == -1) {
        // Destino desconhecido: confirma mesmo assim para nao travar
        Serial.println("ERRO: Destino desconhecido: " + destNome);
        delay(DELAY_FLIT);
        txCanais[canalAtual]->println("ACK");
        canalAtual = (canalAtual + 1) % 4;
      } else {
        bool entregue = repassarEAguardarACKdoDestino(
                          canalAtual, idxDest, orig, dest, pay);
        if (entregue) {
          // Entrega confirmada: avanca para o proximo canal
          canalAtual = (canalAtual + 1) % 4;
        } else {
          // Falha na entrega: mantem o mesmo canal para reenvio
          Serial.println(">>> Destino nao respondeu. " + origemNome +
                         " tera nova chance no proximo ciclo.");
        }
      }
    }
  } else {
    // Sem pacote: avanca para o proximo canal mesmo assim
    canalAtual = (canalAtual + 1) % 4;
  }

  // Exibe status a cada 4 ciclos completos
  static int ciclo = 0;
  ciclo++;
  if (ciclo >= 4) {
    Serial.print("Roteadores ativos: ");
    for (int i = 0; i < 4; i++) {
      if (roteadorAtivo[i]) Serial.print(nomesCanais[i] + " ");
    }
    Serial.println(" | Repasses c/ sucesso: " + String(pacotesRepassados));
    ciclo = 0;
  }

  delay(300);
}