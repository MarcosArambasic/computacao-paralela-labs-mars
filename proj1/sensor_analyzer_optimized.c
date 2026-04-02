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

/* ===================== ESTRUTURAS ===================== */

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

    EstatSensor sensores_locais[MAX_SENSORES];
    int num_sensores_locais;

    long   alertas_local;
    double energia_local;
} ArgThread;

/* ===================== GLOBAIS (IGUAIS AO ORIGINAL) ===================== */

static EstatSensor g_sensores[MAX_SENSORES];
static int         g_num_sensores = 0;

static long   g_total_alertas = 0;
static double g_total_energia = 0.0;

static Anomalia g_anomalias[MAX_ANOMALIAS];
static int      g_num_anomalias = 0;

/* ===================== WORKER OTIMIZADO ===================== */

static int encontrar_ou_criar_local(
        EstatSensor *vet, int *n, const char *id)
{
    for (int i = 0; i < *n; i++)
        if (strcmp(vet[i].id, id) == 0)
            return i;

    if (*n >= MAX_SENSORES) return -1;

    int idx = (*n)++;
    strncpy(vet[idx].id, id, MAX_NOME_SENSOR - 1);
    vet[idx].id[MAX_NOME_SENSOR - 1] = '\0';
    vet[idx].contagem = 0;
    vet[idx].soma = 0.0;
    vet[idx].soma_quadrados = 0.0;
    return idx;
}

static void *processar_fatia_otimizada(void *arg)
{
    ArgThread *a = arg;
    const Arquivo *arq = a->arq;

    a->num_sensores_locais = 0;
    a->alertas_local = 0;
    a->energia_local = 0.0;

    for (long i = a->inicio; i < a->fim; i++) {
        char sensor_id[MAX_NOME_SENSOR];
        char data[12], hora[9], tipo[16], kw_status[8], status[10];
        double valor;

        if (sscanf(arq->linhas[i],
                   "%15s %11s %8s %15s %lf %7s %9s",
                   sensor_id, data, hora,
                   tipo, &valor, kw_status, status) < 7)
            continue;

        if (!strcmp(status,"ALERTA") || !strcmp(status,"CRITICO"))
            a->alertas_local++;

        if (!strcmp(tipo,"energia")) {
            a->energia_local += valor;
            continue;
        }

        if (!strcmp(tipo,"temperatura")) {
            int idx = encontrar_ou_criar_local(
                a->sensores_locais,
                &a->num_sensores_locais,
                sensor_id
            );
            if (idx >= 0) {
                a->sensores_locais[idx].contagem++;
                a->sensores_locais[idx].soma += valor;
                a->sensores_locais[idx].soma_quadrados += valor * valor;
            }
        }
    }
    return NULL;
}

/* ===================== REDUÇÃO FINAL ===================== */

static void reduzir_resultados(ArgThread *args, int nthreads)
{
    for (int t = 0; t < nthreads; t++) {
        g_total_alertas += args[t].alertas_local;
        g_total_energia += args[t].energia_local;

        for (int i = 0; i < args[t].num_sensores_locais; i++) {
            EstatSensor *src = &args[t].sensores_locais[i];

            int idx = -1;
            for (int j = 0; j < g_num_sensores; j++)
                if (!strcmp(g_sensores[j].id, src->id)) {
                    idx = j; break;
                }

            if (idx < 0) {
                idx = g_num_sensores++;
                g_sensores[idx] = *src;
            } else {
                g_sensores[idx].contagem += src->contagem;
                g_sensores[idx].soma += src->soma;
                g_sensores[idx].soma_quadrados += src->soma_quadrados;
            }
        }
    }
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
        if (pthread_create(&threads[t], NULL, processar_fatia_otimizada, &args[t]) != 0) {
            perror("pthread_create");
            return EXIT_FAILURE;
        }
    }

    for (int t = 0; t < num_threads; t++)
        pthread_join(threads[t], NULL);

    //Redução final, substituindo mutex globais
    reduzir_resultados(args, num_threads);

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

    return EXIT_SUCCESS;
}