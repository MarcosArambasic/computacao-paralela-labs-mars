#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>

#define MAX_SENSORES      1000
#define MAX_NOME_SENSOR   16
#define MAX_LINHA         256
#define MAX_ANOMALIAS     100000

typedef struct {
    char   id[MAX_NOME_SENSOR];
    long   contagem;
    double soma;
    double soma_quadrados;
} EstatSensor;

typedef struct {
    char   sensor_id[MAX_NOME_SENSOR];
    char   timestamp[20];
    double valor;
} Anomalia;

typedef struct {
    char **linhas;
    long   total_linhas;
} Arquivo;

typedef struct {
    int   thread_id;
    long  inicio;
    long  fim;
    const Arquivo *arq;
} ArgThread;

static EstatSensor g_sensores[MAX_SENSORES];
static int         g_num_sensores = 0;

static long   g_total_alertas = 0;
static double g_total_energia = 0.0;

static Anomalia g_anomalias[MAX_ANOMALIAS];
static int      g_num_anomalias = 0;

static pthread_mutex_t mutex_sensores  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutex_totais    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutex_anomalias = PTHREAD_MUTEX_INITIALIZER;

static int encontrar_ou_criar_sensor(const char *id)
{
    for (int i = 0; i < g_num_sensores; i++) {
        if (strcmp(g_sensores[i].id, id) == 0)
            return i;
    }
    if (g_num_sensores >= MAX_SENSORES) return -1;

    int idx = g_num_sensores++;
    strncpy(g_sensores[idx].id, id, MAX_NOME_SENSOR - 1);
    g_sensores[idx].id[MAX_NOME_SENSOR - 1] = '\0';
    g_sensores[idx].contagem       = 0;
    g_sensores[idx].soma           = 0.0;
    g_sensores[idx].soma_quadrados = 0.0;
    return idx;
}

static void *processar_fatia(void *arg)
{
    ArgThread     *a   = (ArgThread *)arg;
    const Arquivo *arq = a->arq;

    long   alertas_local = 0;
    double energia_local = 0.0;

    for (long i = a->inicio; i < a->fim; i++) {
        const char *linha = arq->linhas[i];

        char   sensor_id[MAX_NOME_SENSOR];
        char   data[12], hora[9];
        char   tipo[16];
        double valor;
        char   kw_status[8];
        char   status[10];

        int campos = sscanf(linha, "%15s %11s %8s %15s %lf %7s %9s",
                            sensor_id, data, hora, tipo, &valor, kw_status, status);
        if (campos < 7) continue;

        char timestamp[20];
        snprintf(timestamp, sizeof(timestamp), "%s %s", data, hora);

        if (strcmp(status, "ALERTA") == 0 || strcmp(status, "CRITICO") == 0)
            alertas_local++;

        if (strcmp(tipo, "energia") == 0) {
            energia_local += valor;
            continue;
        }

        if (strcmp(tipo, "temperatura") == 0) {
            pthread_mutex_lock(&mutex_sensores);
            int idx = encontrar_ou_criar_sensor(sensor_id);
            if (idx >= 0) {
                g_sensores[idx].contagem++;
                g_sensores[idx].soma           += valor;
                g_sensores[idx].soma_quadrados += valor * valor;
            }
            pthread_mutex_unlock(&mutex_sensores);
        }
    }

    pthread_mutex_lock(&mutex_totais);
    g_total_alertas += alertas_local;
    g_total_energia += energia_local;
    pthread_mutex_unlock(&mutex_totais);

    return NULL;
}

static void detectar_anomalias(const Arquivo *arq)
{
    double medias[MAX_SENSORES];
    double desvios[MAX_SENSORES];

    for (int i = 0; i < g_num_sensores; i++) {
        if (g_sensores[i].contagem == 0) {
            medias[i] = desvios[i] = 0.0;
            continue;
        }
        double n  = (double)g_sensores[i].contagem;
        double m  = g_sensores[i].soma / n;
        double v  = (g_sensores[i].soma_quadrados / n) - (m * m);
        medias[i]  = m;
        desvios[i] = (v > 0.0) ? sqrt(v) : 0.0;
    }

    for (long i = 0; i < arq->total_linhas && g_num_anomalias < MAX_ANOMALIAS; i++) {
        const char *linha = arq->linhas[i];

        char   sensor_id[MAX_NOME_SENSOR];
        char   data[12], hora[9];
        char   tipo[16];
        double valor;
        char   kw_status[8];
        char   status[10];

        int campos = sscanf(linha, "%15s %11s %8s %15s %lf %7s %9s",
                            sensor_id, data, hora, tipo, &valor, kw_status, status);
        if (campos < 7 || strcmp(tipo, "temperatura") != 0) continue;

        int idx = -1;
        for (int j = 0; j < g_num_sensores; j++) {
            if (strcmp(g_sensores[j].id, sensor_id) == 0) { idx = j; break; }
        }
        if (idx < 0 || desvios[idx] == 0.0) continue;

        if (fabs(valor - medias[idx]) > 3.0 * desvios[idx]) {
            Anomalia *an = &g_anomalias[g_num_anomalias++];
            strncpy(an->sensor_id, sensor_id, MAX_NOME_SENSOR - 1);
            snprintf(an->timestamp, sizeof(an->timestamp), "%s %s", data, hora);
            an->valor = valor;
        }
    }
}

static int carregar_arquivo(const char *caminho, Arquivo *arq)
{
    FILE *fp = fopen(caminho, "r");
    if (!fp) { perror("Erro ao abrir arquivo"); return -1; }

    long total = 0;
    char buf[MAX_LINHA];
    while (fgets(buf, sizeof(buf), fp)) total++;
    rewind(fp);

    arq->total_linhas = total;
    arq->linhas = (char **)malloc(total * sizeof(char *));
    if (!arq->linhas) { fclose(fp); return -1; }

    for (long i = 0; i < total; i++) {
        if (!fgets(buf, sizeof(buf), fp)) break;
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
        arq->linhas[i] = (char *)malloc((len + 1) * sizeof(char));
        if (!arq->linhas[i]) { fclose(fp); return -1; }
        memcpy(arq->linhas[i], buf, len + 1);
    }

    fclose(fp);
    return 0;
}

static void liberar_arquivo(Arquivo *arq)
{
    for (long i = 0; i < arq->total_linhas; i++)
        free(arq->linhas[i]);
    free(arq->linhas);
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <num_threads> <arquivo.log>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int num_threads = atoi(argv[1]);
    if (num_threads < 1) num_threads = 1;
    const char *caminho = argv[2];

    printf("=============================================================\n");
    printf(" ANALISADOR PARALELO DE LOGS IoT  (threads = %d)\n", num_threads);
    printf("=============================================================\n");

    Arquivo arq;
    struct timespec t0, t1;

    printf("[1/4] Carregando arquivo '%s'...\n", caminho);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (carregar_arquivo(caminho, &arq) != 0) return EXIT_FAILURE;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double t_carga = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("    Total de linhas : %ld\n", arq.total_linhas);
    printf("    Tempo de carga  : %.3f s\n", t_carga);

    printf("[2/4] Processando com %d thread(s)...\n", num_threads);
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_t *threads = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
    ArgThread *args    = (ArgThread *)malloc(num_threads * sizeof(ArgThread));

    long bloco = arq.total_linhas / num_threads;
    long resto  = arq.total_linhas % num_threads;
    long cursor = 0;

    for (int t = 0; t < num_threads; t++) {
        args[t].thread_id = t;
        args[t].arq       = &arq;
        args[t].inicio    = cursor;
        args[t].fim       = cursor + bloco + (t < resto ? 1 : 0);
        cursor            = args[t].fim;
        if (pthread_create(&threads[t], NULL, processar_fatia, &args[t]) != 0) {
            perror("pthread_create");
            return EXIT_FAILURE;
        }
    }

    for (int t = 0; t < num_threads; t++)
        pthread_join(threads[t], NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double t_proc = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("    Tempo de processamento: %.3f s\n", t_proc);

    printf("[3/4] Detectando anomalias...\n");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    detectar_anomalias(&arq);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double t_anom = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("    Tempo detecção: %.3f s\n", t_anom);

    double t_total = t_carga + t_proc + t_anom;

    printf("[4/4] Resultados\n");
    printf("=============================================================\n");

    printf("\n--- Média de temperatura por sensor (10 primeiros) ---\n");
    int exibidos = 0;
    for (int i = 0; i < g_num_sensores && exibidos < 10; i++) {
        if (g_sensores[i].contagem == 0) continue;
        double media = g_sensores[i].soma / (double)g_sensores[i].contagem;
        printf("  %-14s : %.2f C  (%ld leituras)\n",
               g_sensores[i].id, media, g_sensores[i].contagem);
        exibidos++;
    }

    int    idx_instavel = -1;
    double maior_desvio = -1.0;
    for (int i = 0; i < g_num_sensores; i++) {
        if (g_sensores[i].contagem < 2) continue;
        double n = (double)g_sensores[i].contagem;
        double m = g_sensores[i].soma / n;
        double v = (g_sensores[i].soma_quadrados / n) - (m * m);
        double d = (v > 0.0) ? sqrt(v) : 0.0;
        if (d > maior_desvio) { maior_desvio = d; idx_instavel = i; }
    }

    printf("\n--- Sensor mais instável ---\n");
    if (idx_instavel >= 0) {
        printf("  Sensor : %s\n", g_sensores[idx_instavel].id);
        printf("  Desvio : %.4f C\n", maior_desvio);
    } else {
        printf("  (nenhum sensor de temperatura encontrado)\n");
    }

    printf("\n--- Totais ---\n");
    printf("  Total de alertas (ALERTA + CRITICO) : %ld\n", g_total_alertas);
    printf("  Consumo total de energia            : %.2f W\n", g_total_energia);

    printf("\n--- Anomalias detectadas (3 desvios padrao) ---\n");
    printf("  Total: %d\n", g_num_anomalias);
    int max_exibir = (g_num_anomalias < 5) ? g_num_anomalias : 5;
    for (int i = 0; i < max_exibir; i++) {
        printf("  [%d] %s  %s  valor=%.2f\n", i + 1,
               g_anomalias[i].sensor_id, g_anomalias[i].timestamp, g_anomalias[i].valor);
    }

    printf("\n--- Desempenho ---\n");
    printf("  Threads     : %d\n", num_threads);
    printf("  Tempo total : %.3f s  (carga=%.3f  proc=%.3f  anomalias=%.3f)\n",
           t_total, t_carga, t_proc, t_anom);
    printf("=============================================================\n");

    free(threads);
    free(args);
    liberar_arquivo(&arq);
    pthread_mutex_destroy(&mutex_sensores);
    pthread_mutex_destroy(&mutex_totais);
    pthread_mutex_destroy(&mutex_anomalias);

    return EXIT_SUCCESS;
}
