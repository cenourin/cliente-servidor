// Compilar: gcc -pthread cliente.c -o cliente
// Executar: ./cliente
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TAM_MSG 512

// ─── Protótipos ──────────────────────────────────────────────────────────────

char *montar_msg(const char *tipo, const char *conteudo, char *out, int out_len);
void  parse_conteudo(const char *raw, char *conteudo);
void *thread_auxiliar(void *arg);

// ─── Montar string no protocolo bom|tipo|conteudo|eom ────────────────────────

char *montar_msg(const char *tipo, const char *conteudo, char *out, int out_len) {
    snprintf(out, out_len, "bom|%s|%s|eom", tipo, conteudo);
    return out;
}

// ─── Extrair apenas o conteudo de uma mensagem do protocolo ──────────────────

void parse_conteudo(const char *raw, char *conteudo) {
    conteudo[0] = '\0';

    char copia[TAM_MSG];
    strncpy(copia, raw, TAM_MSG - 1);

    char *token = strtok(copia, "|");   // "bom"
    if (!token) return;
    token = strtok(NULL, "|");          // tipo
    if (!token) return;
    token = strtok(NULL, "|");          // conteudo
    if (!token) return;
    strncpy(conteudo, token, TAM_MSG - 1);
}

// ─── Thread auxiliar: recebe mensagens do servidor e imprime na tela ─────────

void *thread_auxiliar(void *arg) {
    int sockfd = *((int *)arg);
    char raw[TAM_MSG];
    char conteudo[TAM_MSG];
    int n;

    while ((n = recv(sockfd, raw, TAM_MSG - 1, 0)) > 0) {
        raw[n] = '\0';
        parse_conteudo(raw, conteudo);
        printf("\n[Servidor]: %s\n> ", conteudo);
        fflush(stdout);
    }

    printf("\nConexão encerrada pelo servidor.\n");
    pthread_exit(NULL);
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(void) {
    char ip[64];
    int porta;
    char apelido[64];
    char entrada[TAM_MSG];
    char msg[TAM_MSG];

    // 1. Lê IP e porta do usuário
    printf("Endereço IP do servidor: ");
    scanf("%63s", ip);
    printf("Porta: ");
    scanf("%d", &porta);
    getchar(); // consome o '\n' deixado pelo scanf

    // 2. Cria socket e conecta
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) { perror("socket"); return EXIT_FAILURE; }

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family      = AF_INET;
    server.sin_port        = htons(porta);
    server.sin_addr.s_addr = inet_addr(ip);

    if (connect(sockfd, (struct sockaddr *)&server, sizeof(server)) == -1) {
        perror("connect");
        return EXIT_FAILURE;
    }

    // 3. Cria thread auxiliar (já pronta para receber boas-vindas e demais msgs)
    pthread_t tid;
    pthread_create(&tid, NULL, thread_auxiliar, &sockfd);

    // 4. Lê apelido e envia usuario_entra
    printf("Seu apelido: ");
    fgets(apelido, sizeof(apelido), stdin);
    apelido[strcspn(apelido, "\n")] = '\0'; // remove '\n'

    montar_msg("usuario_entra", apelido, msg, TAM_MSG);
    send(sockfd, msg, strlen(msg), 0);

    // 5. Loop principal: lê do teclado e envia mensagens
    while (1) {
        printf("> ");
        fflush(stdout);

        if (!fgets(entrada, sizeof(entrada), stdin)) break;
        entrada[strcspn(entrada, "\n")] = '\0';

        if (strcmp(entrada, "tchau") == 0) {
            montar_msg("usuario_sai", apelido, msg, TAM_MSG);
            send(sockfd, msg, strlen(msg), 0);
            break;
        }

        montar_msg("msg_cliente", entrada, msg, TAM_MSG);
        send(sockfd, msg, strlen(msg), 0);
    }

    // 6. Encerra conexão e aguarda thread auxiliar
    close(sockfd);
    pthread_join(tid, NULL);

    printf("Até logo!\n");
    return EXIT_SUCCESS;
}
