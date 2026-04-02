#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define MAX_SENSORES  1000
#define MAX_ANOMALIAS 100000

// Estrutura para os dados brutos (O "Vector" do professor)
typedef struct {
    int    sensor_idx;
    double valor;
    char   tipo[16];
    char   status[16];
    char   timestamp[21];   // "AAAA-MM-DD HH:MM:SS" = 19 chars + '\0'
} LogEntry;

// Estrutura para os resultados (Requisito 2)
typedef struct {
    char id[16];
    char tipo[16];
    int ativo;
    long long contagem;
    double soma;
    double soma_quadrados;
} SensorStats;

// Estrutura para anomalias detectadas
typedef struct {
    char sensor_id[16];
    char timestamp[21];
    double valor;
} Anomalia;

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Uso: %s <arquivo_de_log>\n", argv[0]);
        return 1;
    }

    // --- FASE 1: CARGA DE DADOS ---

    // Timer inicia ANTES da abertura do arquivo para medir o tempo total real
    struct timespec t_inicio, t_fim;
    clock_gettime(CLOCK_MONOTONIC, &t_inicio);

    FILE *fp = fopen(argv[1], "r");
    if (!fp) { perror("Erro ao abrir arquivo"); return 1; }

    long long total_linhas = 0;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), fp)) total_linhas++;
    rewind(fp);

    // Alocação dinâmica com verificação de segurança
    LogEntry *log_vector = (LogEntry *) malloc(total_linhas * sizeof(LogEntry));
    if (!log_vector) {
        printf("Erro: Memoria insuficiente para carregar o arquivo.\n");
        fclose(fp);
        return 1;
    }
    
    long long i = 0;
    char s_id[16], data[12], hora[9], tipo[16], p_status[10], status[10];
    double valor;

    while (fgets(buffer, sizeof(buffer), fp) && i < total_linhas) {
        if (sscanf(buffer, "%15s %11s %9s %15s %lf %9s %9s", 
                   s_id, data, hora, tipo, &valor, p_status, status) == 7) {
            
            int idx;
            if (sscanf(s_id, "sensor_%d", &idx) == 1 && idx >= 0 && idx < MAX_SENSORES) {
                log_vector[i].sensor_idx = idx;
                log_vector[i].valor = valor;
                strncpy(log_vector[i].tipo, tipo, 15);
                strncpy(log_vector[i].status, status, 15);
                snprintf(log_vector[i].timestamp, sizeof(log_vector[i].timestamp),
                         "%s %s", data, hora);
                i++;
            }
        }
    }
    fclose(fp);
    long long registros_validos = i;

    // --- FASE 2: PROCESSAMENTO SEQUENCIAL ---

    SensorStats *stats = (SensorStats *) calloc(MAX_SENSORES, sizeof(SensorStats));
    long long total_alertas = 0;
    double consumo_energia = 0;

    for (long long j = 0; j < registros_validos; j++) {
        int idx = log_vector[j].sensor_idx;

        if (!stats[idx].ativo) {
            sprintf(stats[idx].id, "sensor_%03d", idx);
            strncpy(stats[idx].tipo, log_vector[j].tipo, 15);
            stats[idx].ativo = 1;
        }

        stats[idx].contagem++;
        stats[idx].soma += log_vector[j].valor;
        stats[idx].soma_quadrados += (log_vector[j].valor * log_vector[j].valor);

        if (strcmp(log_vector[j].status, "ALERTA") == 0 || strcmp(log_vector[j].status, "CRITICO") == 0)
            total_alertas++;
        
        if (strcmp(log_vector[j].tipo, "energia") == 0)
            consumo_energia += log_vector[j].valor;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_fim);

    // --- FASE 3: DETECÇÃO DE ANOMALIAS (3 desvios padrão) ---
    // Aproveita o log_vector já em memória — sem segundo scan no arquivo

    // Calcula média e desvio padrão de cada sensor de temperatura
    double medias[MAX_SENSORES]  = {0};
    double desvios[MAX_SENSORES] = {0};

    for (int k = 0; k < MAX_SENSORES; k++) {
        if (!stats[k].ativo || stats[k].contagem == 0 ||
            strcmp(stats[k].tipo, "temperatura") != 0) continue;

        double n = (double)stats[k].contagem;
        double m = stats[k].soma / n;
        double v = (stats[k].soma_quadrados / n) - (m * m);
        medias[k]  = m;
        desvios[k] = (v > 0.0) ? sqrt(v) : 0.0;
    }

    // Aloca vetor de anomalias e varre o log_vector
    Anomalia *anomalias = (Anomalia *) malloc(MAX_ANOMALIAS * sizeof(Anomalia));
    int num_anomalias = 0;

    if (anomalias) {
        for (long long j = 0; j < registros_validos && num_anomalias < MAX_ANOMALIAS; j++) {
            if (strcmp(log_vector[j].tipo, "temperatura") != 0) continue;

            int k = log_vector[j].sensor_idx;
            if (desvios[k] == 0.0) continue;

            if (fabs(log_vector[j].valor - medias[k]) > 3.0 * desvios[k]) {
                anomalias[num_anomalias].valor = log_vector[j].valor;
                strncpy(anomalias[num_anomalias].sensor_id,
                        stats[k].id, sizeof(anomalias[num_anomalias].sensor_id) - 1);
                strncpy(anomalias[num_anomalias].timestamp,
                        log_vector[j].timestamp,
                        sizeof(anomalias[num_anomalias].timestamp) - 1);
                num_anomalias++;
            }
        }
    }

    // --- FASE 4: RESULTADOS ---
    double tempo = (t_fim.tv_sec - t_inicio.tv_sec) + (t_fim.tv_nsec - t_inicio.tv_nsec) / 1e9;

    printf("\n[ MÉDIA DE TEMPERATURA (10 PRIMEIROS) ]\n");
    int exibidos = 0;
    for (int k = 0; k < MAX_SENSORES && exibidos < 10; k++) {
        if (stats[k].ativo && strcmp(stats[k].tipo, "temperatura") == 0) {
            printf("  %s: %.2f C (%lld leituras)\n", stats[k].id, stats[k].soma / stats[k].contagem, stats[k].contagem);
            exibidos++;
        }
    }

    int idx_instavel = -1;
    double maior_dp = -1;
    for (int k = 0; k < MAX_SENSORES; k++) {
        if (stats[k].ativo && stats[k].contagem > 0 && strcmp(stats[k].tipo, "temperatura") == 0) {
            double media = stats[k].soma / stats[k].contagem;
            double variancia = (stats[k].soma_quadrados / stats[k].contagem) - (media * media);
            double dp = sqrt(variancia < 0 ? 0 : variancia);
            if (dp > maior_dp) { maior_dp = dp; idx_instavel = k; }
        }
    }

    printf("\n[ SENSOR MAIS INSTÁVEL ]\n");
    if (idx_instavel != -1) 
        printf("  %s: Desvio Padrão = %.4f C\n", stats[idx_instavel].id, maior_dp);

    printf("\n[ TOTAIS GLOBAIS ]\n");
    printf("  Registros processados: %lld\n", registros_validos);
    printf("  Alertas:               %lld\n", total_alertas);
    printf("  Consumo Energia:       %.2f W\n", consumo_energia);
    printf("  Tempo de Execução:     %.4f s  (carga + processamento)\n", tempo);

    printf("\n[ ANOMALIAS DETECTADAS (3 desvios padrão) ]\n");
    if (!anomalias) {
        printf("  (erro de alocacao)\n");
    } else {
        printf("  Total: %d\n", num_anomalias);
        int max_exibir = (num_anomalias < 5) ? num_anomalias : 5;
        for (int k = 0; k < max_exibir; k++) {
            printf("  [%d] %s  %s  valor=%.2f\n", k + 1,
                   anomalias[k].sensor_id,
                   anomalias[k].timestamp,
                   anomalias[k].valor);
        }
    }

    free(anomalias);
    free(log_vector);
    free(stats);

    return 0;
}