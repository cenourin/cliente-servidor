# Trabalho SO — Chat Multicliente em C

## Informações gerais

- **Prazo:** 30/06/2026 às 23:59 via Moodle
- **Equipe:** dupla, com apresentação em sala de aula
- **Compilador:** gcc — o código deve compilar sem alterações
- **Instituição:** IFG Campus Anápolis — Disciplina de Sistemas Operacionais

## Entregáveis obrigatórios

1. Código-fonte de `servidor.c` e `cliente.c` (compiláveis com gcc sem modificações)
2. Executáveis do servidor e do cliente
3. Arquivo-texto com: nome dos alunos, comandos de compilação e de uso

## Protocolo de comunicação

Todas as mensagens seguem o formato:

```
bom|tipo_do_comando|conteudo|eom
```

| Tipo | Direção | Significado |
|---|---|---|
| `msg_servidor` | servidor → clientes | Boas-vindas ou broadcast de evento |
| `usuario_entra` | cliente → servidor | Informa o apelido do novo usuário |
| `msg_cliente` | cliente → servidor | Mensagem comum de chat |
| `usuario_sai` | cliente → servidor | Notifica desconexão voluntária |

TCP é um protocolo de stream — `recv()` pode retornar mensagens parciais. Os delimitadores `bom` e `eom` permitem reconstituir mensagens fragmentadas.

### Strings geradas para o buffer (broadcast)

Cada evento recebido pelo servidor gera uma string humanizada colocada no buffer:

| Evento recebido | String colocada no buffer |
|---|---|
| `usuario_entra` com apelido "Hugo" | `Hugo entrou na sala de conversa.` |
| `usuario_sai` com apelido "Hugo" | `Hugo saiu da sala de conversa.` |
| `msg_cliente` de "Hugo" com texto "Oi" | `Hugo enviou: Oi` |

O apelido de cada usuário **deve estar armazenado no servidor** para montar essas strings.

O broadcast é enviado a todos os clientes no formato:
```
bom|msg_servidor|string_contida_no_buffer|eom
```

---

## Servidor

### Arquitetura de threads (conforme diagrama de sequência do enunciado)

O servidor possui **três tipos de threads**:

| Thread | Quantidade | Papel |
|---|---|---|
| Thread principal | 1 | Prepara socket, cria thread consumidora, loop de `accept()` |
| Thread produtora | 1 por cliente | Espera mensagens do cliente, coloca no buffer |
| Thread consumidora | 1 (global) | Consome o buffer e envia `msg_servidor` para todos os clientes |

A thread consumidora é criada **uma única vez** na inicialização do servidor, antes do loop de `accept()`.

### Sequência de inicialização

```
socket() → bind() → listen()
→ pthread_create(thread_consumidora)
→ loop: accept() → pthread_create(thread_produtora_N)
```

### Ao aceitar nova conexão (obrigatório pelo enunciado)

1. Servidor envia imediatamente: `bom|msg_servidor|Olá! Seja bem-vindo!|eom`
2. Aguarda receber `bom|usuario_entra|apelido|eom`
3. Coloca `"apelido entrou na sala de conversa."` no buffer
4. Entra em loop aguardando `msg_cliente` ou `usuario_sai`

### Thread produtora (uma por cliente)

1. Recebe `bom|usuario_entra|apelido|eom` → registra apelido, coloca evento no buffer
2. Loop: `recv()` → analisa tipo
   - `msg_cliente` → formata string e produz no buffer
   - `usuario_sai` ou `recv() == 0` → produz evento de saída no buffer, encerra

### Thread consumidora (única, global)

Loop contínuo:
1. `sem_wait(sem_cheio)` — bloqueia se buffer vazio
2. Retira string do buffer (com mutex)
3. `sem_post(sem_vazio)`
4. Percorre lista de clientes e envia `bom|msg_servidor|string|eom` para cada um (com `mutex_lista`)

### Lista global de clientes

Array de `ClienteInfo*` protegido por `mutex_lista`. Toda thread que lê ou modifica a lista trava esse mutex.

---

## Buffer Compartilhado — Produtor/Consumidor

### Buffer circular (ring buffer)

- `N` posições fixas
- `head`: próxima posição para escrever (produtor avança)
- `tail`: próxima posição para ler (consumidor avança)
- Vazio: `head == tail` | Cheio: `(head + 1) % N == tail`

### Três primitivas de sincronização

| Primitiva | Tipo | Valor inicial | Papel |
|---|---|---|---|
| `mutex_buffer` | Mutex | destrancado | Acesso exclusivo às variáveis head/tail/buffer |
| `sem_cheio` | Semáforo | 0 | Conta posições com dados para consumir |
| `sem_vazio` | Semáforo | N | Conta posições livres para escrever |

**Produtor (thread produtora do cliente):**
```
sem_wait(sem_vazio)        // bloqueia se buffer cheio
pthread_mutex_lock(mutex_buffer)
  buffer[head] = mensagem
  head = (head + 1) % N
pthread_mutex_unlock(mutex_buffer)
sem_post(sem_cheio)        // acorda consumidor
```

**Consumidor (thread consumidora):**
```
sem_wait(sem_cheio)        // bloqueia se buffer vazio
pthread_mutex_lock(mutex_buffer)
  mensagem = buffer[tail]
  tail = (tail + 1) % N
pthread_mutex_unlock(mutex_buffer)
sem_post(sem_vazio)        // libera vaga para produtor
```

O semáforo controla *quando* acessar; o mutex controla *quem* acessa simultaneamente.

---

## Cliente

### Sequência obrigatória (conforme enunciado)

1. Solicita ao usuário: endereço IP do servidor e porta
2. `connect()` ao servidor via TCP
3. Cria thread auxiliar (que aguarda mensagens do servidor)
4. Recebe e exibe mensagem de boas-vindas (via thread auxiliar)
5. Pergunta ao usuário o apelido desejado
6. Envia `bom|usuario_entra|apelido|eom`
7. Loop: lê mensagem do teclado → se "tchau": envia `usuario_sai` e encerra; senão: envia `msg_cliente`

### Duas threads

**Thread principal:**
- Gerencia conexão inicial e leitura do teclado (passo 1–7 acima)

**Thread auxiliar (receptora):**
- Criada logo após o `connect()`
- Loop: `recv()` → imprime na tela as mensagens do servidor
- Encerra quando `recv()` retorna 0

### Desconexão ("tchau")

1. TP envia `bom|usuario_sai|apelido|eom`
2. `close(socket)`
3. TA percebe `recv() == 0` e encerra
4. TP faz `pthread_join(thread_auxiliar)` e sai

---

## Desconexão e Limpeza no Servidor

### Desconexão voluntária

Thread produtora recebe `usuario_sai`:
1. Coloca `"apelido saiu da sala de conversa."` no buffer
2. Remove cliente da lista global (com `mutex_lista`)
3. `close(socket_cliente)`
4. `free(ClienteInfo)`
5. `pthread_exit()`

### Desconexão abrupta

`recv()` retorna `0` ou `-1` — tratar igual ao `usuario_sai`.

### Detach vs Join

- Threads produtoras no servidor: `pthread_detach()` — número desconhecido em tempo de compilação
- Thread auxiliar do cliente: `pthread_join()` — main aguarda antes de encerrar

---

## Arquitetura geral (diagrama de sequência simplificado)

```
Cliente 1                  Servidor                      Cliente 2
─────────────────────────────────────────────────────────────────
TP: connect()     →   Thread principal: accept()
                      cria Thread produtora 1
                  ←   envia boas-vindas (msg_servidor)
TA: exibe boas-vindas
TP: digita apelido
TP: usuario_entra →   Thread produtora 1: coloca no buffer
                      Thread consumidora: retira do buffer
                  ←   msg_servidor: "user1 entrou"     →  TA: exibe
                                              TP: connect()
                                              Thread principal: accept()
                                              cria Thread produtora 2
                                          ←  envia boas-vindas
                                          TA: exibe boas-vindas
                                          TP: usuario_entra → buffer
                  ←   msg_servidor: "user2 entrou"     ←  buffer
TA: exibe                                              TA: exibe
TP: msg_cliente   →   buffer → msg_servidor para todos
TA: exibe         ←                                    ← TA: exibe
                                          TP: "tchau" → usuario_sai
                                                         buffer → msg_servidor
TA: exibe saída   ←                       Thread produtora 2 encerra
```

---

## Arquivos do projeto

- `servidor.c` — socket, threads produtoras, thread consumidora, buffer, lista de clientes
- `cliente.c` — conexão TCP, thread auxiliar, envio/recebimento
- Arquivo-texto — nomes, comandos de compilação (`gcc`) e de uso

## Análise dos arquivos de exemplo fornecidos

### `prodCons_thread_semaforo.cpp`

Demonstra:
- `sem_init`, `sem_wait`, `sem_post`, `sem_destroy`
- `pthread_create` e `pthread_join`
- Lógica básica de produtor/consumidor com loop de espera

**O que falta / diferenças para o trabalho:**
- Não tem buffer real (só um contador `slotsUtilizados`) — no trabalho será um array de strings
- Não usa `pthread_mutex_t` — a variável `slotsUtilizados` é acessada sem mutex (race condition latente no exemplo)
- No trabalho haverá M produtores simultâneos, não apenas 1
- Compilar com: `g++ -pthread prodCons_thread_semaforo.cpp -o prodCons_thread_semaforo`

### `servidor.cpp`

Demonstra:
- `socket()`, `bind()`, `listen()`, `accept()`, `send()`, `recv()`, `close()`
- `setsockopt(SO_REUSEADDR)` — evita erro "port already in use" ao reiniciar
- `struct sockaddr_in` para configurar endereço/porta
- `htons()` para converter porta para network byte order

**O que falta / diferenças para o trabalho:**
- Porta hardcoded (`#define PORT 4242`) — no trabalho deve ser lida do usuário
- `accept()` é chamado **fora** do loop — só aceita um cliente; no trabalho fica dentro de um `while`
- Nenhuma thread — no trabalho cada `accept()` dispara um `pthread_create`
- Protocolo simples ("bye") — no trabalho será `bom|...|eom`
- Compilar com: `g++ -pthread servidor.cpp -o servidor`

### `cliente.cpp`

Demonstra:
- `socket()`, `connect()`, `send()`, `recv()`, `close()`
- `inet_addr()` para converter IP string para binário
- `struct sockaddr_in` com `sin_family`, `sin_port`, `sin_addr`
- Loop de envio/recebimento sequencial

**O que falta / diferenças para o trabalho:**
- IP e porta hardcoded — no trabalho devem ser lidos do usuário via `scanf`/`fgets`
- Single-threaded — no trabalho haverá thread auxiliar para `recv()` simultâneo ao `fgets()`
- Sem protocolo `bom|...|eom`
- Sem lógica de "tchau" → `usuario_sai`
- Compilar com: `g++ cliente.cpp -o cliente` (sem `-pthread`, mas o trabalho precisará)
