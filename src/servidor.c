// Compilar: gcc -pthread servidor.c -o servidor
// Executar: ./servidor <porta>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>

// Configurações

#define MAX_CLIENTES   20
#define TAM_BUFFER     20       // slots do buffer circular
#define TAM_MSG        512      // tamanho máximo de uma mensagem

// Buffer compartilhado (produtor/consumidor)

char buffer[TAM_BUFFER][TAM_MSG];
int head = 0;
int tail = 0;

sem_t semVazio;        // conta slots livres (inicia em TAM_BUFFER)
sem_t semCheio;        // conta slots com dados (inicia em 0)
pthread_mutex_t mutexBuffer = PTHREAD_MUTEX_INITIALIZER;

// Lista de clientes conectados

typedef struct {
    int fd;
    char apelido[64];
} Cliente;

Cliente clientes[MAX_CLIENTES];
int numClientes = 0;
pthread_mutex_t mutexLista = PTHREAD_MUTEX_INITIALIZER;

// Protótipos

void  bufferProduzir(const char *msg);
void  bufferConsumir(char *msgOut);
void  listaAdicionar(int fd, const char *apelido);
void  listaRemover(int fd);
void  broadcast(const char *msg);
char *montarMsg(const char *tipo, const char *conteudo, char *out, int outLen);
void  parseMsg(const char *raw, char *tipo, char *conteudo);
void *threadConsumidora(void *arg);
void *threadProdutora(void *arg);

// Buffer: produzir

void bufferProduzir(const char *msg) {
    sem_wait(&semVazio);
    pthread_mutex_lock(&mutexBuffer);

    strncpy(buffer[head], msg, TAM_MSG - 1);
    head = (head + 1) % TAM_BUFFER;

    pthread_mutex_unlock(&mutexBuffer);
    sem_post(&semCheio);
}

// Buffer: consumir

void bufferConsumir(char *msgOut) {
    sem_wait(&semCheio);
    pthread_mutex_lock(&mutexBuffer);

    strncpy(msgOut, buffer[tail], TAM_MSG - 1);
    tail = (tail + 1) % TAM_BUFFER;

    pthread_mutex_unlock(&mutexBuffer);
    sem_post(&semVazio);
}

// Lista: adicionar cliente

void listaAdicionar(int fd, const char *apelido) {
    pthread_mutex_lock(&mutexLista);
    if (numClientes < MAX_CLIENTES) {
        clientes[numClientes].fd = fd;
        strncpy(clientes[numClientes].apelido, apelido, 63);
        numClientes++;
    }
    pthread_mutex_unlock(&mutexLista);
}

// Lista: remover cliente

void listaRemover(int fd) {
    pthread_mutex_lock(&mutexLista);
    for (int i = 0; i < numClientes; i++) {
        if (clientes[i].fd == fd) {
            clientes[i] = clientes[numClientes - 1];
            numClientes--;
            break;
        }
    }
    pthread_mutex_unlock(&mutexLista);
}

// Enviar mensagem para todos os clientes

void broadcast(const char *msg) {
    pthread_mutex_lock(&mutexLista);
    for (int i = 0; i < numClientes; i++) {
        send(clientes[i].fd, msg, strlen(msg), 0);
    }
    pthread_mutex_unlock(&mutexLista);
}

// Montar string no protocolo bom|tipo|conteudo|eom

char *montarMsg(const char *tipo, const char *conteudo, char *out, int outLen) {
    snprintf(out, outLen, "bom|%s|%s|eom", tipo, conteudo);
    return out;
}

// Extrair tipo e conteudo de uma string do protocolo

void parseMsg(const char *raw, char *tipo, char *conteudo) {
    // formato esperado: bom|tipo|conteudo|eom
    tipo[0] = '\0';
    conteudo[0] = '\0';

    char copia[TAM_MSG];
    strncpy(copia, raw, TAM_MSG - 1);

    char *token = strtok(copia, "|");   // "bom"
    if (!token) return;
    token = strtok(NULL, "|");          // tipo
    if (!token) return;
    strncpy(tipo, token, 63);
    token = strtok(NULL, "|");          // conteudo
    if (!token) return;
    strncpy(conteudo, token, TAM_MSG - 1);
}

// Thread consumidora (única, global)

void *threadConsumidora(void *arg) {
    char msgBuffer[TAM_MSG];
    char msgSaida[TAM_MSG];

    while (1) {
        bufferConsumir(msgBuffer);
        montarMsg("msg_servidor", msgBuffer, msgSaida, TAM_MSG);
        broadcast(msgSaida);
    }
    return NULL;
}

// Thread produtora (uma por cliente)

void *threadProdutora(void *arg) {
    int fd = *((int *)arg);
    free(arg);

    char raw[TAM_MSG];
    char tipo[64];
    char conteudo[TAM_MSG];
    char apelido[64] = "";
    char evento[TAM_MSG * 2];
    char boasVindas[TAM_MSG];

    // 1. Envia boas-vindas
    montarMsg("msg_servidor", "Olá! Seja bem-vindo!", boasVindas, TAM_MSG);
    send(fd, boasVindas, strlen(boasVindas), 0);

    // 2. Aguarda usuario_entra
    int n = recv(fd, raw, TAM_MSG - 1, 0);
    if (n <= 0) goto encerrar;
    raw[n] = '\0';
    parseMsg(raw, tipo, conteudo);

    if (strcmp(tipo, "usuario_entra") != 0) goto encerrar;
    strncpy(apelido, conteudo, 63);
    listaAdicionar(fd, apelido);

    snprintf(evento, TAM_MSG, "%s entrou na sala de conversa.", apelido);
    bufferProduzir(evento);

    // 3. Loop principal: recebe mensagens do cliente
    while (1) {
        memset(raw, 0, TAM_MSG);
        n = recv(fd, raw, TAM_MSG - 1, 0);
        if (n <= 0) {
            // desconexão abrupta
            snprintf(evento, TAM_MSG, "%s saiu da sala de conversa.", apelido);
            bufferProduzir(evento);
            goto encerrar;
        }
        raw[n] = '\0';
        parseMsg(raw, tipo, conteudo);

        if (strcmp(tipo, "msg_cliente") == 0) {
            snprintf(evento, TAM_MSG * 2, "%s enviou: %s", apelido, conteudo);
            bufferProduzir(evento);
        } else if (strcmp(tipo, "usuario_sai") == 0) {
            snprintf(evento, TAM_MSG, "%s saiu da sala de conversa.", apelido);
            bufferProduzir(evento);
            goto encerrar;
        }
    }

encerrar:
    if (apelido[0] != '\0') listaRemover(fd);
    close(fd);
    pthread_exit(NULL);
}

// Main

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <porta>\n", argv[0]);
        return EXIT_FAILURE;
    }
    int porta = atoi(argv[1]);

    // Inicializa semáforos
    sem_init(&semVazio, 0, TAM_BUFFER);
    sem_init(&semCheio, 0, 0);

    // Cria socket
    int serverfd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverfd == -1) { perror("socket"); return EXIT_FAILURE; }

    int yes = 1;
    setsockopt(serverfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family      = AF_INET;
    server.sin_port        = htons(porta);
    server.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverfd, (struct sockaddr *)&server, sizeof(server)) == -1) {
        perror("bind"); return EXIT_FAILURE;
    }
    if (listen(serverfd, MAX_CLIENTES) == -1) {
        perror("listen"); return EXIT_FAILURE;
    }

    printf("Servidor escutando na porta %d...\n", porta);

    // Cria thread consumidora (única, global)
    pthread_t tidConsumidor;
    pthread_create(&tidConsumidor, NULL, threadConsumidora, NULL);
    pthread_detach(tidConsumidor);

    // Loop de accept: cria uma thread produtora por cliente
    while (1) {
        struct sockaddr_in client;
        socklen_t clientLen = sizeof(client);
        int clientfd = accept(serverfd, (struct sockaddr *)&client, &clientLen);
        if (clientfd == -1) { perror("accept"); continue; }

        int *fdPtr = malloc(sizeof(int));
        *fdPtr = clientfd;

        pthread_t tid;
        pthread_create(&tid, NULL, threadProdutora, fdPtr);
        pthread_detach(tid);
    }

    close(serverfd);
    sem_destroy(&semVazio);
    sem_destroy(&semCheio);
    return EXIT_SUCCESS;
}
