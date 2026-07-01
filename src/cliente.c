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

// ─── Input atual do usuário (compartilhado com thread auxiliar) ───────────────

char input_atual[TAM_MSG] = {0};
pthread_mutex_t mutex_input = PTHREAD_MUTEX_INITIALIZER;
sem_t sem_boas_vindas; // TA sinaliza após exibir boas-vindas


// ─── Modo raw do terminal ─────────────────────────────────────────────────────

static struct termios termo_original;

static void entrar_modo_raw(void) {
    tcgetattr(STDIN_FILENO, &termo_original);
    struct termios raw = termo_original;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void restaurar_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &termo_original);
}

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

    // Primeira mensagem: boas-vindas
    n = recv(sockfd, raw, TAM_MSG - 1, 0);
    if (n > 0) {
        raw[n] = '\0';
        parse_conteudo(raw, conteudo);
        printf("[Servidor]: %s\n", conteudo);
        fflush(stdout);
    }
    sem_post(&sem_boas_vindas); // libera TP para pedir apelido

    // Mensagens seguintes: reimprime o que o usuário estava digitando
    while ((n = recv(sockfd, raw, TAM_MSG - 1, 0)) > 0) {
        raw[n] = '\0';
        parse_conteudo(raw, conteudo);

        pthread_mutex_lock(&mutex_input);
        printf("\r\033[K[Servidor]: %s\n> %s", conteudo, input_atual);
        fflush(stdout);
        pthread_mutex_unlock(&mutex_input);
    }

    printf("\r\033[KConexão encerrada pelo servidor.\n");
    pthread_exit(NULL);
}

// ─── Lê uma linha do teclado em modo raw, mantendo input_atual atualizado ────

static int ler_linha(char *buf, int maxlen) {
    int len = 0;
    buf[0] = '\0';

    pthread_mutex_lock(&mutex_input);
    input_atual[0] = '\0';
    pthread_mutex_unlock(&mutex_input);

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
                pthread_mutex_lock(&mutex_input);
                input_atual[len] = '\0';
                pthread_mutex_unlock(&mutex_input);
                printf("\b \b");
                fflush(stdout);
            }
        } else if (len < maxlen - 1) {
            buf[len++] = (char)c;
            buf[len]   = '\0';
            pthread_mutex_lock(&mutex_input);
            input_atual[len - 1] = (char)c;
            input_atual[len]     = '\0';
            pthread_mutex_unlock(&mutex_input);
            printf("%c", c);
            fflush(stdout);
        }
    }

    return len;
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

    // 3. Cria TA imediatamente após conectar; ela exibe boas-vindas e sinaliza
    sem_init(&sem_boas_vindas, 0, 0);
    pthread_t tid;
    pthread_create(&tid, NULL, thread_auxiliar, &sockfd);

    // 4. TP espera boas-vindas ser exibida, depois pede apelido
    sem_wait(&sem_boas_vindas);
    sem_destroy(&sem_boas_vindas);

    printf("Seu apelido: ");
    fgets(apelido, sizeof(apelido), stdin);
    apelido[strcspn(apelido, "\n")] = '\0';

    montar_msg("usuario_entra", apelido, msg, TAM_MSG);
    send(sockfd, msg, strlen(msg), 0);

    // 5. Entra em modo raw para o loop de chat
    entrar_modo_raw();

    // 6. Loop principal: lê do teclado e envia mensagens
    while (1) {
        printf("> ");
        fflush(stdout);

        ler_linha(entrada, TAM_MSG);

        if (strcmp(entrada, "tchau") == 0) {
            montar_msg("usuario_sai", apelido, msg, TAM_MSG);
            send(sockfd, msg, strlen(msg), 0);
            break;
        }

        if (entrada[0] == '\0') continue;

        montar_msg("msg_cliente", entrada, msg, TAM_MSG);
        send(sockfd, msg, strlen(msg), 0);
    }

    // 7. Encerra conexão e aguarda thread auxiliar
    restaurar_terminal();
    close(sockfd);
    pthread_join(tid, NULL);

    printf("Até logo!\n");
    return EXIT_SUCCESS;
}
