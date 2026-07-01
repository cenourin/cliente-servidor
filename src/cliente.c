// Compilar: gcc -pthread cliente.c -o cliente
// Executar: ./cliente
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TAM_MSG 512

// Input atual do usuário (compartilhado com thread auxiliar)

char inputAtual[TAM_MSG] = {0};
pthread_mutex_t mutexInput = PTHREAD_MUTEX_INITIALIZER;
sem_t semBoasVindas; // TA sinaliza após exibir boas-vindas


// Modo raw do terminal

static struct termios termoOriginal;

static void entrarModoRaw(void) {
    tcgetattr(STDIN_FILENO, &termoOriginal);
    struct termios raw = termoOriginal;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void restaurarTerminal(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &termoOriginal);
}

// Protótipos

char *montarMsg(const char *tipo, const char *conteudo, char *out, int outLen);
void  parseConteudo(const char *raw, char *conteudo);
void *threadAuxiliar(void *arg);

// Montar string no protocolo bom|tipo|conteudo|eom

char *montarMsg(const char *tipo, const char *conteudo, char *out, int outLen) {
    snprintf(out, outLen, "bom|%s|%s|eom", tipo, conteudo);
    return out;
}

// Extrair apenas o conteudo de uma mensagem do protocolo

void parseConteudo(const char *raw, char *conteudo) {
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

// Thread auxiliar: recebe mensagens do servidor e imprime na tela

void *threadAuxiliar(void *arg) {
    int sockfd = *((int *)arg);
    char raw[TAM_MSG];
    char conteudo[TAM_MSG];
    int n;

    // Primeira mensagem: boas-vindas
    n = recv(sockfd, raw, TAM_MSG - 1, 0);
    if (n > 0) {
        raw[n] = '\0';
        parseConteudo(raw, conteudo);
        printf("[Servidor]: %s\n", conteudo);
        fflush(stdout);
    }
    sem_post(&semBoasVindas); // libera TP para pedir apelido

    // Mensagens seguintes: reimprime o que o usuário estava digitando
    while ((n = recv(sockfd, raw, TAM_MSG - 1, 0)) > 0) {
        raw[n] = '\0';
        parseConteudo(raw, conteudo);

        pthread_mutex_lock(&mutexInput);
        printf("\r\033[K[Servidor]: %s\n> %s", conteudo, inputAtual);
        fflush(stdout);
        pthread_mutex_unlock(&mutexInput);
    }

    printf("\r\033[KConexão encerrada pelo servidor.\n");
    pthread_exit(NULL);
}

// Lê uma linha do teclado em modo raw, mantendo inputAtual atualizado

static int lerLinha(char *buf, int maxlen) {
    int len = 0;
    buf[0] = '\0';

    pthread_mutex_lock(&mutexInput);
    inputAtual[0] = '\0';
    pthread_mutex_unlock(&mutexInput);

    int c;
    while ((c = getchar()) != EOF) {
        if (c == '\n' || c == '\r') {
            printf("\n");
            fflush(stdout);
            break;
        } else if (c == 127 || c == '\b') {  // backspace
            if (len > 0) {
                len--;
                buf[len] = '\0';
                pthread_mutex_lock(&mutexInput);
                inputAtual[len] = '\0';
                pthread_mutex_unlock(&mutexInput);
                printf("\b \b");
                fflush(stdout);
            }
        } else if (len < maxlen - 1) {
            buf[len++] = (char)c;
            buf[len]   = '\0';
            pthread_mutex_lock(&mutexInput);
            inputAtual[len - 1] = (char)c;
            inputAtual[len]     = '\0';
            pthread_mutex_unlock(&mutexInput);
            printf("%c", c);
            fflush(stdout);
        }
    }

    return len;
}

// Main

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

    // 3. Cria TA imediatamente após conectar; ela exibe boas-vindas e sinaliza
    sem_init(&semBoasVindas, 0, 0);
    pthread_t tid;
    pthread_create(&tid, NULL, threadAuxiliar, &sockfd);

    // 4. TP espera boas-vindas ser exibida, depois pede apelido
    sem_wait(&semBoasVindas);
    sem_destroy(&semBoasVindas);

    printf("Seu apelido: ");
    fgets(apelido, sizeof(apelido), stdin);
    apelido[strcspn(apelido, "\n")] = '\0';

    montarMsg("usuario_entra", apelido, msg, TAM_MSG);
    send(sockfd, msg, strlen(msg), 0);

    // 5. Entra em modo raw para o loop de chat
    entrarModoRaw();

    // 6. Loop principal: lê do teclado e envia mensagens
    while (1) {
        printf("> ");
        fflush(stdout);

        lerLinha(entrada, TAM_MSG);

        if (strcmp(entrada, "tchau") == 0) {
            montarMsg("usuario_sai", apelido, msg, TAM_MSG);
            send(sockfd, msg, strlen(msg), 0);
            break;
        }

        if (entrada[0] == '\0') continue;

        montarMsg("msg_cliente", entrada, msg, TAM_MSG);
        send(sockfd, msg, strlen(msg), 0);
    }

    // 7. Encerra conexão e aguarda thread auxiliar
    restaurarTerminal();
    close(sockfd);
    pthread_join(tid, NULL);

    printf("Até logo!\n");
    return EXIT_SUCCESS;
}
