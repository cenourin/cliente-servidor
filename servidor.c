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

// ─── Configurações ───────────────────────────────────────────────────────────

#define MAX_CLIENTES   10
#define TAM_BUFFER     20       // slots do buffer circular
#define TAM_MSG        512      // tamanho máximo de uma mensagem

// ─── Buffer compartilhado (produtor/consumidor) ───────────────────────────────

char buffer[TAM_BUFFER][TAM_MSG];
int head = 0;
int tail = 0;

sem_t sem_vazio;        // conta slots livres (inicia em TAM_BUFFER)
sem_t sem_cheio;        // conta slots com dados (inicia em 0)
pthread_mutex_t mutex_buffer = PTHREAD_MUTEX_INITIALIZER;

// ─── Lista de clientes conectados ────────────────────────────────────────────

typedef struct {
    int fd;
    char apelido[64];
} Cliente;

Cliente clientes[MAX_CLIENTES];
int num_clientes = 0;
pthread_mutex_t mutex_lista = PTHREAD_MUTEX_INITIALIZER;

// ─── Protótipos ──────────────────────────────────────────────────────────────

void  buffer_produzir(const char *msg);
void  buffer_consumir(char *msg_out);
void  lista_adicionar(int fd, const char *apelido);
void  lista_remover(int fd);
void  broadcast(const char *msg);
char *montar_msg(const char *tipo, const char *conteudo, char *out, int out_len);
void  parse_msg(const char *raw, char *tipo, char *conteudo);
void *thread_consumidora(void *arg);
void *thread_produtora(void *arg);

// ─── Buffer: produzir ────────────────────────────────────────────────────────

void buffer_produzir(const char *msg) {
    sem_wait(&sem_vazio);
    pthread_mutex_lock(&mutex_buffer);

    strncpy(buffer[head], msg, TAM_MSG - 1);
    head = (head + 1) % TAM_BUFFER;

    pthread_mutex_unlock(&mutex_buffer);
    sem_post(&sem_cheio);
}

// ─── Buffer: consumir ────────────────────────────────────────────────────────

void buffer_consumir(char *msg_out) {
    sem_wait(&sem_cheio);
    pthread_mutex_lock(&mutex_buffer);

    strncpy(msg_out, buffer[tail], TAM_MSG - 1);
    tail = (tail + 1) % TAM_BUFFER;

    pthread_mutex_unlock(&mutex_buffer);
    sem_post(&sem_vazio);
}

// ─── Lista: adicionar cliente ─────────────────────────────────────────────────

void lista_adicionar(int fd, const char *apelido) {
    pthread_mutex_lock(&mutex_lista);
    if (num_clientes < MAX_CLIENTES) {
        clientes[num_clientes].fd = fd;
        strncpy(clientes[num_clientes].apelido, apelido, 63);
        num_clientes++;
    }
    pthread_mutex_unlock(&mutex_lista);
}

// ─── Lista: remover cliente ───────────────────────────────────────────────────

void lista_remover(int fd) {
    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < num_clientes; i++) {
        if (clientes[i].fd == fd) {
            clientes[i] = clientes[num_clientes - 1];
            num_clientes--;
            break;
        }
    }
    pthread_mutex_unlock(&mutex_lista);
}

// ─── Enviar mensagem para todos os clientes ───────────────────────────────────

void broadcast(const char *msg) {
    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < num_clientes; i++) {
        send(clientes[i].fd, msg, strlen(msg), 0);
    }
    pthread_mutex_unlock(&mutex_lista);
}

// ─── Montar string no protocolo bom|tipo|conteudo|eom ────────────────────────

char *montar_msg(const char *tipo, const char *conteudo, char *out, int out_len) {
    snprintf(out, out_len, "bom|%s|%s|eom", tipo, conteudo);
    return out;
}

// ─── Extrair tipo e conteudo de uma string do protocolo ──────────────────────

void parse_msg(const char *raw, char *tipo, char *conteudo) {
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

// ─── Thread consumidora (única, global) ──────────────────────────────────────

void *thread_consumidora(void *arg) {
    char msg_buffer[TAM_MSG];
    char msg_saida[TAM_MSG];

    while (1) {
        buffer_consumir(msg_buffer);
        montar_msg("msg_servidor", msg_buffer, msg_saida, TAM_MSG);
        broadcast(msg_saida);
    }
    return NULL;
}

// ─── Thread produtora (uma por cliente) ──────────────────────────────────────

void *thread_produtora(void *arg) {
    int fd = *((int *)arg);
    free(arg);

    char raw[TAM_MSG];
    char tipo[64];
    char conteudo[TAM_MSG];
    char apelido[64] = "";
    char evento[TAM_MSG * 2];
    char boas_vindas[TAM_MSG];

    // 1. Envia boas-vindas
    montar_msg("msg_servidor", "Olá! Seja bem-vindo!", boas_vindas, TAM_MSG);
    send(fd, boas_vindas, strlen(boas_vindas), 0);

    // 2. Aguarda usuario_entra
    int n = recv(fd, raw, TAM_MSG - 1, 0);
    if (n <= 0) goto encerrar;
    raw[n] = '\0';
    parse_msg(raw, tipo, conteudo);

    if (strcmp(tipo, "usuario_entra") != 0) goto encerrar;
    strncpy(apelido, conteudo, 63);
    lista_adicionar(fd, apelido);

    snprintf(evento, TAM_MSG, "%s entrou na sala de conversa.", apelido);
    buffer_produzir(evento);

    // 3. Loop principal: recebe mensagens do cliente
    while (1) {
        memset(raw, 0, TAM_MSG);
        n = recv(fd, raw, TAM_MSG - 1, 0);
        if (n <= 0) {
            // desconexão abrupta
            snprintf(evento, TAM_MSG, "%s saiu da sala de conversa.", apelido);
            buffer_produzir(evento);
            goto encerrar;
        }
        raw[n] = '\0';
        parse_msg(raw, tipo, conteudo);

        if (strcmp(tipo, "msg_cliente") == 0) {
            snprintf(evento, TAM_MSG * 2, "%s enviou: %s", apelido, conteudo);
            buffer_produzir(evento);
        } else if (strcmp(tipo, "usuario_sai") == 0) {
            snprintf(evento, TAM_MSG, "%s saiu da sala de conversa.", apelido);
            buffer_produzir(evento);
            goto encerrar;
        }
    }

encerrar:
    if (apelido[0] != '\0') lista_remover(fd);
    close(fd);
    pthread_exit(NULL);
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <porta>\n", argv[0]);
        return EXIT_FAILURE;
    }
    int porta = atoi(argv[1]);

    // Inicializa semáforos
    sem_init(&sem_vazio, 0, TAM_BUFFER);
    sem_init(&sem_cheio, 0, 0);

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
    pthread_t tid_consumidor;
    pthread_create(&tid_consumidor, NULL, thread_consumidora, NULL);
    pthread_detach(tid_consumidor);

    // Loop de accept: cria uma thread produtora por cliente
    while (1) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int clientfd = accept(serverfd, (struct sockaddr *)&client, &client_len);
        if (clientfd == -1) { perror("accept"); continue; }

        int *fd_ptr = malloc(sizeof(int));
        *fd_ptr = clientfd;

        pthread_t tid;
        pthread_create(&tid, NULL, thread_produtora, fd_ptr);
        pthread_detach(tid);
    }

    close(serverfd);
    sem_destroy(&sem_vazio);
    sem_destroy(&sem_cheio);
    return EXIT_SUCCESS;
}
