#define _POSIX_C_SOURCE 200809L

/*
 * =============================================================
 * ANALISADOR PARALELO DE LOGS IoT - VERSÃO OTIMIZADA E COMENTADA
 * =============================================================
 *
 * Objetivo deste programa:
 * 1) Ler um arquivo de log de sensores IoT.
 * 2) Processar as linhas em paralelo com Pthreads.
 * 3) Calcular:
 *    - média de temperatura por sensor
 *    - sensor mais instável (maior desvio padrão)
 *    - total de alertas
 *    - consumo total de energia
 *    - detecção de anomalias em temperatura
 *
 * Ideia principal da otimização:
 * Em vez de todas as threads atualizarem estruturas globais o tempo todo
 * usando mutex, cada thread acumula seus próprios resultados localmente.
 * Depois, ao final, o programa realiza uma etapa de redução (merge),
 * combinando os resultados parciais em estruturas globais.
 *
 * Vantagem:
 * - reduz contenção
 * - reduz overhead de sincronização
 * - melhora escalabilidade
 */

#include <stdio.h>      // printf, fprintf, perror, fgets, fopen, fclose
#include <stdlib.h>     // malloc, free, atoi, EXIT_FAILURE, EXIT_SUCCESS
#include <string.h>     // strcmp, strncpy, strlen, memcpy
#include <math.h>       // sqrt, fabs
#include <pthread.h>    // pthread_t, pthread_create, pthread_join
#include <time.h>       // clock_gettime, CLOCK_MONOTONIC

/* ===================== CONSTANTES DO PROGRAMA ===================== */

/*
 * Quantidade máxima de sensores suportada.
 * O enunciado fala de sensores do tipo sensor_001 até sensor_999,
 * então 1000 é suficiente para manter uma margem segura.
 */
#define MAX_SENSORES      1000

/*
 * Tamanho máximo para armazenar o nome do sensor.
 * Exemplo: "sensor_001"
 */
#define MAX_NOME_SENSOR   16

/*
 * Tamanho máximo esperado para uma linha do arquivo de log.
 * Isso precisa ser suficiente para conter todos os campos da linha.
 */
#define MAX_LINHA         256

/*
 * Quantidade máxima de anomalias que serão armazenadas.
 * Esse limite evita crescimento ilimitado de memória.
 */
#define MAX_ANOMALIAS     100000

/* ===================== ESTRUTURAS DE DADOS ===================== */

/*
 * Estrutura que guarda estatísticas acumuladas de um sensor.
 *
 * Campos:
 * - id: identificador do sensor, por exemplo "sensor_034"
 * - contagem: quantas leituras desse sensor foram processadas
 * - soma: soma total dos valores
 * - soma_quadrados: soma dos quadrados dos valores
 *
 * Com soma e soma dos quadrados, conseguimos calcular:
 * - média
 * - variância
 * - desvio padrão
 *
 * sem precisar guardar todas as leituras individualmente.
 */
typedef struct {
    char   id[MAX_NOME_SENSOR];
    long   contagem;
    double soma;
    double soma_quadrados;
} EstatSensor;

/*
 * Estrutura usada para registrar uma leitura considerada anômala.
 *
 * Campos:
 * - sensor_id: qual sensor gerou a anomalia
 * - timestamp: data e hora concatenadas
 * - valor: valor da leitura anômala
 */
typedef struct {
    char   sensor_id[MAX_NOME_SENSOR];
    char   timestamp[20];
    double valor;
} Anomalia;

/*
 * Estrutura que representa o arquivo carregado em memória.
 *
 * Estratégia adotada:
 * - cada linha do arquivo é armazenada em um vetor de strings
 * - isso facilita a divisão do trabalho entre threads, pois cada
 *   thread recebe um intervalo [inicio, fim) de linhas
 *
 * Campos:
 * - linhas: vetor de ponteiros para strings
 * - total_linhas: quantidade total de linhas carregadas
 */
typedef struct {
    char **linhas;
    long   total_linhas;
} Arquivo;

/*
 * Estrutura de argumentos passada para cada thread.
 *
 * Campos de controle:
 * - thread_id: identificador da thread
 * - inicio: índice inicial da fatia que a thread irá processar
 * - fim: índice final (não inclusivo) da fatia
 * - arq: ponteiro para o arquivo carregado em memória
 *
 * Campos de resultados locais:
 * - sensores_locais: estatísticas locais por sensor de temperatura
 * - num_sensores_locais: quantos sensores locais foram registrados
 * - alertas_local: total de ALERTA/CRITICO encontrados pela thread
 * - energia_local: soma local das leituras de energia
 *
 * Essa separação é o coração da otimização:
 * cada thread trabalha em suas próprias estruturas, sem mutex
 * durante o processamento principal.
 */
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

/* ===================== VARIÁVEIS GLOBAIS ===================== */

/*
 * Estruturas globais finais.
 * Elas serão preenchidas apenas após a execução das threads,
 * durante a fase de redução dos resultados.
 */
static EstatSensor g_sensores[MAX_SENSORES];
static int         g_num_sensores = 0;

static long   g_total_alertas = 0;
static double g_total_energia = 0.0;

/*
 * Vetor global de anomalias detectadas.
 */
static Anomalia g_anomalias[MAX_ANOMALIAS];
static int      g_num_anomalias = 0;

/* ===================== FUNÇÃO AUXILIAR LOCAL ===================== */

/*
 * encontrar_ou_criar_local
 * ------------------------
 * Procura um sensor no vetor local da thread.
 *
 * Se já existir:
 *   retorna o índice correspondente.
 *
 * Se não existir:
 *   cria uma nova entrada no vetor local, inicializa os campos
 *   estatísticos e retorna o índice criado.
 *
 * Se o vetor local atingir o limite:
 *   retorna -1.
 *
 * Essa função é usada apenas dentro da thread, ou seja,
 * não há concorrência entre threads aqui.
 */
static int encontrar_ou_criar_local(
        EstatSensor *vet, int *n, const char *id)
{
    /* Primeiro tenta localizar um sensor já existente no vetor local */
    for (int i = 0; i < *n; i++)
        if (strcmp(vet[i].id, id) == 0)
            return i;

    /* Se chegou ao limite máximo, não há como inserir novo sensor */
    if (*n >= MAX_SENSORES) return -1;

    /* Cria uma nova posição no vetor local */
    int idx = (*n)++;

    /* Copia o id do sensor com segurança */
    strncpy(vet[idx].id, id, MAX_NOME_SENSOR - 1);
    vet[idx].id[MAX_NOME_SENSOR - 1] = '\0';

    /* Inicializa estatísticas */
    vet[idx].contagem = 0;
    vet[idx].soma = 0.0;
    vet[idx].soma_quadrados = 0.0;

    return idx;
}

/* ===================== FUNÇÃO WORKER DAS THREADS ===================== */

/*
 * processar_fatia_otimizada
 * -------------------------
 * Essa é a função executada por cada thread.
 *
 * O que ela faz:
 * 1) percorre apenas a sua faixa de linhas do arquivo
 * 2) faz o parsing de cada linha
 * 3) atualiza contadores locais
 * 4) acumula estatísticas locais por sensor
 *
 * Importante:
 * - não usa mutex nesse laço principal
 * - cada thread escreve somente em sua própria estrutura ArgThread
 *
 * Isso reduz fortemente a contenção em relação à versão com mutex global.
 */
static void *processar_fatia_otimizada(void *arg)
{
    /* Faz o cast do argumento genérico para o tipo correto */
    ArgThread *a = arg;

    /* Guarda um atalho para o arquivo em memória */
    const Arquivo *arq = a->arq;

    /* Reinicializa os acumuladores locais da thread */
    a->num_sensores_locais = 0;
    a->alertas_local = 0;
    a->energia_local = 0.0;

    /*
     * Percorre somente o intervalo de linhas atribuído a essa thread.
     * Convenção adotada: [inicio, fim)
     * Ou seja, processa inicio, inicio+1, ..., fim-1
     */
    for (long i = a->inicio; i < a->fim; i++) {
        /*
         * Variáveis temporárias para armazenar os campos extraídos
         * de cada linha do log.
         */
        char sensor_id[MAX_NOME_SENSOR];
        char data[12], hora[9], tipo[16], kw_status[8], status[10];
        double valor;

        /*
         * Faz o parsing da linha.
         *
         * Exemplo de linha:
         * sensor_001 2026-03-19 08:00:00 temperatura 23.5 status OK
         *
         * Campos lidos:
         * 1) sensor_id
         * 2) data
         * 3) hora
         * 4) tipo
         * 5) valor
         * 6) palavra "status" (guardada em kw_status)
         * 7) status propriamente dito
         *
         * Se não conseguir ler os 7 campos, a linha é ignorada.
         */
        if (sscanf(arq->linhas[i],
                   "%15s %11s %8s %15s %lf %7s %9s",
                   sensor_id, data, hora,
                   tipo, &valor, kw_status, status) < 7)
            continue;

        /*
         * Conta alertas locais.
         * Consideramos como alerta tanto ALERTA quanto CRITICO.
         */
        if (!strcmp(status,"ALERTA") || !strcmp(status,"CRITICO"))
            a->alertas_local++;

        /*
         * Se a linha for de energia, acumula no somatório local
         * e segue para a próxima linha.
         *
         * Não precisamos registrar energia por sensor para esta tarefa,
         * apenas o total acumulado.
         */
        if (!strcmp(tipo,"energia")) {
            a->energia_local += valor;
            continue;
        }

        /*
         * Se a linha for de temperatura, atualizamos as estatísticas
         * locais do sensor.
         */
        if (!strcmp(tipo,"temperatura")) {
            int idx = encontrar_ou_criar_local(
                a->sensores_locais,
                &a->num_sensores_locais,
                sensor_id
            );

            /*
             * Só atualiza se houve espaço e o sensor foi encontrado/criado.
             */
            if (idx >= 0) {
                a->sensores_locais[idx].contagem++;
                a->sensores_locais[idx].soma += valor;
                a->sensores_locais[idx].soma_quadrados += valor * valor;
            }
        }
    }

    /* Thread termina sem retornar valor útil */
    return NULL;
}

/* ===================== REDUÇÃO FINAL ===================== */

/*
 * reduzir_resultados
 * ------------------
 * Após todas as threads terminarem, esta função combina os resultados
 * locais de cada thread nas estruturas globais.
 *
 * Etapas:
 * 1) soma alertas locais no total global
 * 2) soma energia local no total global
 * 3) combina as estatísticas de cada sensor
 *
 * Essa redução é sequencial, mas o custo dela costuma ser pequeno
 * comparado ao ganho de eliminar mutex no processamento principal.
 */
static void reduzir_resultados(ArgThread *args, int nthreads)
{
    /* Percorre os resultados produzidos por cada thread */
    for (int t = 0; t < nthreads; t++) {
        /* Soma os acumuladores globais simples */
        g_total_alertas += args[t].alertas_local;
        g_total_energia += args[t].energia_local;

        /*
         * Agora combina as estatísticas de sensores da thread t
         * com a estrutura global.
         */
        for (int i = 0; i < args[t].num_sensores_locais; i++) {
            EstatSensor *src = &args[t].sensores_locais[i];

            /*
             * Procura o sensor no vetor global.
             * Se encontrar, soma os acumuladores.
             * Se não encontrar, cria uma nova entrada global.
             */
            int idx = -1;
            for (int j = 0; j < g_num_sensores; j++)
                if (!strcmp(g_sensores[j].id, src->id)) {
                    idx = j;
                    break;
                }

            /* Sensor ainda não existe no vetor global */
            if (idx < 0) {
                idx = g_num_sensores++;
                g_sensores[idx] = *src;
            }
            /* Sensor já existe: somar estatísticas */
            else {
                g_sensores[idx].contagem += src->contagem;
                g_sensores[idx].soma += src->soma;
                g_sensores[idx].soma_quadrados += src->soma_quadrados;
            }
        }
    }
}

/* ===================== DETECÇÃO DE ANOMALIAS ===================== */

/*
 * detectar_anomalias
 * ------------------
 * 
 *
 * Estratégia:
 * 1) calcula média e desvio padrão de cada sensor global
 * 2) percorre novamente o arquivo
 * 3) verifica se cada leitura de temperatura foge mais de 3 desvios
 * 4) registra as anomalias encontradas
 */
static void detectar_anomalias(const Arquivo *arq)
{
    /*
     * Vetores auxiliares para guardar média e desvio por sensor.
     * O índice usado aqui corresponde ao vetor global g_sensores.
     */
    double medias[MAX_SENSORES];
    double desvios[MAX_SENSORES];

    /*
     * Primeira fase: calcular média e desvio padrão de cada sensor.
     */
    for (int i = 0; i < g_num_sensores; i++) {
        /* Se não houve leituras, média e desvio são zero */
        if (g_sensores[i].contagem == 0) {
            medias[i] = desvios[i] = 0.0;
            continue;
        }

        /*
         * Cálculo incremental com soma e soma dos quadrados:
         * média = soma / n
         * variância = (soma_quadrados / n) - média²
         * desvio = sqrt(variância)
         */
        double n  = (double)g_sensores[i].contagem;
        double m  = g_sensores[i].soma / n;
        double v  = (g_sensores[i].soma_quadrados / n) - (m * m);

        medias[i]  = m;
        desvios[i] = (v > 0.0) ? sqrt(v) : 0.0;
    }

    /*
     * Segunda fase: percorrer o arquivo novamente e verificar
     * se cada leitura de temperatura é anômala.
     */
    for (long i = 0; i < arq->total_linhas && g_num_anomalias < MAX_ANOMALIAS; i++) {
        const char *linha = arq->linhas[i];

        char   sensor_id[MAX_NOME_SENSOR];
        char   data[12], hora[9];
        char   tipo[16];
        double valor;
        char   kw_status[8];
        char   status[10];

        /*
         * Faz o parsing da linha novamente.
         * Se falhar ou se não for temperatura, ignora.
         */
        int campos = sscanf(linha, "%15s %11s %8s %15s %lf %7s %9s",
                            sensor_id, data, hora, tipo, &valor, kw_status, status);
        if (campos < 7 || strcmp(tipo, "temperatura") != 0) continue;

        /*
         * Localiza o sensor correspondente no vetor global.
         */
        int idx = -1;
        for (int j = 0; j < g_num_sensores; j++) {
            if (strcmp(g_sensores[j].id, sensor_id) == 0) {
                idx = j;
                break;
            }
        }

        /*
         * Se não encontrou o sensor ou o desvio padrão é zero,
         * não faz sentido testar anomalia.
         */
        if (idx < 0 || desvios[idx] == 0.0) continue;

        /*
         * Verifica o critério dos 3 desvios padrão.
         */
        if (fabs(valor - medias[idx]) > 3.0 * desvios[idx]) {
            Anomalia *an = &g_anomalias[g_num_anomalias++];

            strncpy(an->sensor_id, sensor_id, MAX_NOME_SENSOR - 1);
            an->sensor_id[MAX_NOME_SENSOR - 1] = '\0';

            /* Junta data e hora em uma única string */
            snprintf(an->timestamp, sizeof(an->timestamp), "%s %s", data, hora);

            an->valor = valor;
        }
    }
}

/* ===================== CARGA DO ARQUIVO EM MEMÓRIA ===================== */

/*
 * carregar_arquivo
 * ----------------
 * Lê o arquivo de log e coloca todas as linhas em memória.
 *
 * Estratégia em 2 passos:
 * 1) conta quantas linhas existem
 * 2) aloca espaço e lê linha por linha armazenando em arq->linhas
 *
 * Retorna:
 * 0  -> sucesso
 * -1 -> erro
 */
static int carregar_arquivo(const char *caminho, Arquivo *arq)
{
    FILE *fp = fopen(caminho, "r");
    if (!fp) {
        perror("Erro ao abrir arquivo");
        return -1;
    }

    /* Primeira passagem: contar total de linhas */
    long total = 0;
    char buf[MAX_LINHA];
    while (fgets(buf, sizeof(buf), fp)) total++;

    /* Volta ao início do arquivo para fazer a leitura real */
    rewind(fp);

    arq->total_linhas = total;
    arq->linhas = (char **)malloc(total * sizeof(char *));
    if (!arq->linhas) {
        fclose(fp);
        return -1;
    }

    /*
     * Segunda passagem: copiar cada linha para memória dinâmica.
     */
    for (long i = 0; i < total; i++) {
        if (!fgets(buf, sizeof(buf), fp)) break;

        size_t len = strlen(buf);

        /* Remove o '\n' do final, se existir */
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';

        /* Aloca espaço exato para a linha */
        arq->linhas[i] = (char *)malloc((len + 1) * sizeof(char));
        if (!arq->linhas[i]) {
            fclose(fp);
            return -1;
        }

        /* Copia o conteúdo da linha para o vetor */
        memcpy(arq->linhas[i], buf, len + 1);
    }

    fclose(fp);
    return 0;
}

/*
 * liberar_arquivo
 * ---------------
 * Libera toda a memória alocada para armazenar o arquivo.
 */
static void liberar_arquivo(Arquivo *arq)
{
    for (long i = 0; i < arq->total_linhas; i++)
        free(arq->linhas[i]);

    free(arq->linhas);
}

/* ===================== FUNÇÃO PRINCIPAL ===================== */

/*
 * main
 * ----
 * Fluxo geral do programa:
 *
 * 1) valida argumentos
 * 2) carrega o arquivo em memória
 * 3) cria as threads e divide as linhas entre elas
 * 4) espera todas terminarem
 * 5) reduz os resultados locais para globais
 * 6) detecta anomalias
 * 7) calcula métricas finais
 * 8) imprime resultados
 * 9) libera memória
 */
int main(int argc, char *argv[])
{
    /* Verifica se os argumentos mínimos foram informados */
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <num_threads> <arquivo.log>\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Lê a quantidade de threads passada na linha de comando */
    int num_threads = atoi(argv[1]);

    /*
     * Garante pelo menos 1 thread.
     * Se o usuário passar 0 ou valor negativo, usa 1.
     */
    if (num_threads < 1) num_threads = 1;

    /* Caminho do arquivo de log */
    const char *caminho = argv[2];

    printf("=============================================================\n");
    printf(" ANALISADOR PARALELO DE LOGS IoT  (threads = %d)\n", num_threads);
    printf("=============================================================\n");

    Arquivo arq;
    struct timespec t0, t1;

    /* --------------------- ETAPA 1: CARGA --------------------- */

    printf("[1/4] Carregando arquivo '%s'...\n", caminho);

    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (carregar_arquivo(caminho, &arq) != 0)
        return EXIT_FAILURE;

    clock_gettime(CLOCK_MONOTONIC, &t1);

    /* Tempo de carga do arquivo */
    double t_carga = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("    Total de linhas : %ld\n", arq.total_linhas);
    printf("    Tempo de carga  : %.3f s\n", t_carga);

    /* ------------------ ETAPA 2: PROCESSAMENTO ------------------ */

    printf("[2/4] Processando com %d thread(s)...\n", num_threads);

    clock_gettime(CLOCK_MONOTONIC, &t0);

    /*
     * Aloca:
     * - vetor de identificadores das threads
     * - vetor de argumentos para cada thread
     */
    pthread_t *threads = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
    ArgThread *args    = (ArgThread *)malloc(num_threads * sizeof(ArgThread));

    /*
     * Divide o total de linhas em blocos contíguos.
     *
     * bloco = parte base para cada thread
     * resto = linhas que sobraram e serão distribuídas uma a uma
     *         para as primeiras threads
     */
    long bloco = arq.total_linhas / num_threads;
    long resto  = arq.total_linhas % num_threads;
    long cursor = 0;

    /*
     * Criação das threads:
     * cada thread recebe um intervalo [inicio, fim)
     */
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

    /*
     * Espera todas as threads concluírem.
     * Só depois disso é seguro fazer a redução final.
     */
    for (int t = 0; t < num_threads; t++)
        pthread_join(threads[t], NULL);

    /*
     * Redução final:
     * substitui a necessidade de mutex global durante o processamento.
     */
    reduzir_resultados(args, num_threads);

    clock_gettime(CLOCK_MONOTONIC, &t1);

    /* Tempo gasto só na parte paralela de processamento */
    double t_proc = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("    Tempo de processamento: %.3f s\n", t_proc);

    /* ---------------- ETAPA 3: ANOMALIAS ---------------- */

    printf("[3/4] Detectando anomalias...\n");

    clock_gettime(CLOCK_MONOTONIC, &t0);
    detectar_anomalias(&arq);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double t_anom = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("    Tempo detecção: %.3f s\n", t_anom);

    /* Tempo total do programa */
    double t_total = t_carga + t_proc + t_anom;

    /* ---------------- ETAPA 4: RESULTADOS ---------------- */

    printf("[4/4] Resultados\n");
    printf("=============================================================\n");

    /*
     * Exibe a média dos 10 primeiros sensores encontrados.
     * Aqui estamos assumindo que g_sensores guarda sensores de temperatura.
     */
    printf("\n--- Média de temperatura por sensor (10 primeiros) ---\n");
    int exibidos = 0;

    for (int i = 0; i < g_num_sensores && exibidos < 10; i++) {
        if (g_sensores[i].contagem == 0) continue;

        double media = g_sensores[i].soma / (double)g_sensores[i].contagem;

        printf("  %-14s : %.2f C  (%ld leituras)\n",
               g_sensores[i].id, media, g_sensores[i].contagem);

        exibidos++;
    }

    /*
     * Descobre o sensor mais instável:
     * aquele com maior desvio padrão.
     */
    int    idx_instavel = -1;
    double maior_desvio = -1.0;

    for (int i = 0; i < g_num_sensores; i++) {
        if (g_sensores[i].contagem < 2) continue;

        double n = (double)g_sensores[i].contagem;
        double m = g_sensores[i].soma / n;
        double v = (g_sensores[i].soma_quadrados / n) - (m * m);
        double d = (v > 0.0) ? sqrt(v) : 0.0;

        if (d > maior_desvio) {
            maior_desvio = d;
            idx_instavel = i;
        }
    }

    printf("\n--- Sensor mais instável ---\n");
    if (idx_instavel >= 0) {
        printf("  Sensor : %s\n", g_sensores[idx_instavel].id);
        printf("  Desvio : %.4f C\n", maior_desvio);
    } else {
        printf("  (nenhum sensor de temperatura encontrado)\n");
    }

    /*
     * Totais globais principais.
     */
    printf("\n--- Totais ---\n");
    printf("  Total de alertas (ALERTA + CRITICO) : %ld\n", g_total_alertas);
    printf("  Consumo total de energia            : %.2f W\n", g_total_energia);

    /*
     * Exibe quantidade de anomalias e as 5 primeiras encontradas.
     */
    printf("\n--- Anomalias detectadas (3 desvios padrao) ---\n");
    printf("  Total: %d\n", g_num_anomalias);

    int max_exibir = (g_num_anomalias < 5) ? g_num_anomalias : 5;
    for (int i = 0; i < max_exibir; i++) {
        printf("  [%d] %s  %s  valor=%.2f\n", i + 1,
               g_anomalias[i].sensor_id, g_anomalias[i].timestamp, g_anomalias[i].valor);
    }

    /*
     * Exibe métricas de desempenho.
     */
    printf("\n--- Desempenho ---\n");
    printf("  Threads     : %d\n", num_threads);
    printf("  Tempo total : %.3f s  (carga=%.3f  proc=%.3f  anomalias=%.3f)\n",
           t_total, t_carga, t_proc, t_anom);
    printf("=============================================================\n");

    /* ---------------- LIBERAÇÃO DE MEMÓRIA ---------------- */

    free(threads);
    free(args);
    liberar_arquivo(&arq);

    return EXIT_SUCCESS;
}
