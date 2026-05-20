#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>

#include "hash_table.h"

/*
 * analyzer_par_lock.c
 *
 * Versao paralela com OpenMP usando Bucket Lock.
 *
 * Ideia principal:
 * Em vez de proteger todos os incrementos com uma unica regiao critica
 * (#pragma omp critical), criamos um lock para cada bucket da tabela hash.
 *
 * Assim, duas threads so precisam esperar uma pela outra quando acessam
 * URLs que caem no mesmo bucket. Se elas acessarem buckets diferentes,
 * podem executar ao mesmo tempo.
 *
 * Essa estrategia tem uma granularidade intermediaria:
 * - mais paralela que critical global;
 * - mais controlada que deixar as threads atualizarem sem sincronizacao;
 * - simples de entender e aplicar em uma tabela hash com buckets.
 */

/*
 * Tamanho da tabela hash.
 * Foi usado o mesmo valor sugerido no enunciado: 131071,
 * um numero primo proximo de 2^17.
 *
 * Um tamanho maior reduz a chance de muitas URLs cairem no mesmo bucket,
 * diminuindo colisoes e tambem diminuindo a disputa pelos locks.
 */
#define HT_SIZE 131071

/* Arquivos padrao usados pelo projeto. */
#define MANIFEST_FILE "manifest.txt"
#define RESULTS_FILE  "results.csv"

/*
 * Tamanho maximo de uma linha lida do log.
 * O log gerado pelo projeto e simples, mas foi usado 2048 para ter margem.
 */
#define MAX_LINE 2048

/*
 * Capacidade inicial do vetor de linhas do log.
 * Como o arquivo pode ser grande, o vetor cresce dinamicamente com realloc.
 */
#define INITIAL_CAPACITY 1024

/*
 * Calcula o indice do bucket de uma URL.
 *
 * Observacao importante:
 * A hash_table.c ja possui uma funcao de hash interna, mas ela esta como
 * static. Isso significa que ela so pode ser usada dentro do proprio arquivo
 * hash_table.c.
 *
 * Como nesta versao precisamos descobrir qual bucket sera bloqueado antes
 * de chamar ht_get(), foi necessario repetir aqui a mesma logica de hash.
 *
 * Se a funcao de hash daqui fosse diferente da funcao usada em hash_table.c,
 * poderiamos bloquear um bucket errado. Isso quebraria a ideia de proteger
 * corretamente o contador hit_count daquele bucket.
 */
static size_t bucket_index_for_url(const char* str, size_t size) {
    unsigned long hash = 5381;
    int c;

    /*
     * Algoritmo djb2:
     * hash = hash * 33 + caractere
     *
     * A operacao (hash << 5) + hash equivale a hash * 32 + hash,
     * ou seja, hash * 33.
     */
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }

    return hash % size;
}

/*
 * Carrega o arquivo manifest.txt na tabela hash.
 *
 * O manifesto contem uma URL por linha. Cada URL e inserida na tabela hash
 * antes da execucao paralela.
 *
 * Essa etapa e propositalmente sequencial, porque o enunciado define que
 * a tabela deve ser construida antes da fase paralela. Depois disso, nenhuma
 * thread insere ou remove elementos: apenas atualiza hit_count.
 */
static long load_manifest(HashTable* ht, const char* manifest_path) {
    FILE* fp = fopen(manifest_path, "r");
    if (!fp) {
        perror("Erro ao abrir manifest.txt");
        exit(EXIT_FAILURE);
    }

    char buffer[MAX_LINE];
    long count = 0;

    while (fgets(buffer, sizeof(buffer), fp)) {
        /* Remove \n e \r para deixar a URL limpa. */
        buffer[strcspn(buffer, "\r\n")] = '\0';

        /* Ignora linhas vazias, caso existam. */
        if (buffer[0] == '\0') {
            continue;
        }

        /* Insere a URL com contador inicial igual a zero. */
        ht_put(ht, buffer);
        count++;
    }

    fclose(fp);
    return count;
}

/*
 * Carrega todas as linhas do arquivo de log em memoria.
 *
 * Essa estrategia facilita a paralelizacao com #pragma omp for, porque depois
 * cada thread pode pegar um conjunto de posicoes do vetor e processar as linhas
 * de forma independente.
 *
 * Desvantagem: usa mais memoria, pois o log inteiro fica carregado.
 * Vantagem: deixa a divisao do trabalho entre threads mais simples.
 */
static char** load_log_lines(const char* log_path, long* out_n_lines) {
    FILE* fp = fopen(log_path, "r");
    if (!fp) {
        perror("Erro ao abrir arquivo de log");
        exit(EXIT_FAILURE);
    }

    long capacity = INITIAL_CAPACITY;
    long n = 0;

    char** lines = (char**)malloc(sizeof(char*) * capacity);
    if (!lines) {
        perror("Erro ao alocar vetor de linhas");
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    char buffer[MAX_LINE];

    while (fgets(buffer, sizeof(buffer), fp)) {
        /* Se o vetor estiver cheio, dobra a capacidade. */
        if (n == capacity) {
            capacity *= 2;

            char** tmp = (char**)realloc(lines, sizeof(char*) * capacity);
            if (!tmp) {
                perror("Erro ao realocar vetor de linhas");
                fclose(fp);

                /* Libera o que ja tinha sido alocado antes de encerrar. */
                for (long i = 0; i < n; i++) {
                    free(lines[i]);
                }
                free(lines);

                exit(EXIT_FAILURE);
            }

            lines = tmp;
        }

        /* Copia a linha para uma area propria de memoria. */
        lines[n] = (char*)malloc(strlen(buffer) + 1);
        if (!lines[n]) {
            perror("Erro ao alocar linha");
            fclose(fp);

            for (long i = 0; i < n; i++) {
                free(lines[i]);
            }
            free(lines);

            exit(EXIT_FAILURE);
        }

        strcpy(lines[n], buffer);
        n++;
    }

    fclose(fp);

    *out_n_lines = n;
    return lines;
}

/*
 * Extrai a URL de uma linha de log.
 *
 * Formato esperado da linha:
 * 127.0.0.1 - - [data] "GET /url HTTP/1.1" 200 1500
 *
 * A funcao procura:
 * 1. a primeira aspas;
 * 2. o espaco depois do metodo GET;
 * 3. o espaco depois da URL.
 *
 * Retorna 1 se conseguiu extrair a URL.
 * Retorna 0 se a linha estiver fora do formato esperado.
 */
static int extract_url(const char* line, char* url_out, size_t url_out_size) {
    const char* quote = strchr(line, '"');
    if (!quote) {
        return 0;
    }

    /* Encontra o espaco depois do metodo HTTP, por exemplo depois de GET. */
    const char* method_end = strchr(quote + 1, ' ');
    if (!method_end) {
        return 0;
    }

    /* A URL comeca logo depois desse espaco. */
    const char* url_start = method_end + 1;

    /* A URL termina no proximo espaco, antes de HTTP/1.1. */
    const char* url_end = strchr(url_start, ' ');
    if (!url_end) {
        return 0;
    }

    size_t url_len = (size_t)(url_end - url_start);

    /* Evita copiar URL vazia ou maior que o buffer de saida. */
    if (url_len == 0 || url_len >= url_out_size) {
        return 0;
    }

    memcpy(url_out, url_start, url_len);
    url_out[url_len] = '\0';

    return 1;
}

/*
 * Cria e inicializa um lock para cada bucket da tabela hash.
 *
 * Esta e a parte central da versao Bucket Lock:
 * - se a tabela tem N buckets, criamos N locks;
 * - o lock da posicao i protege o bucket i;
 * - assim, buckets diferentes podem ser atualizados ao mesmo tempo.
 */
static omp_lock_t* create_bucket_locks(size_t table_size) {
    omp_lock_t* locks = (omp_lock_t*)malloc(sizeof(omp_lock_t) * table_size);
    if (!locks) {
        perror("Erro ao alocar locks por bucket");
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < table_size; i++) {
        omp_init_lock(&locks[i]);
    }

    return locks;
}

/*
 * Destroi todos os locks criados e libera a memoria do vetor.
 *
 * Todo omp_init_lock deve ter um omp_destroy_lock correspondente.
 * Isso evita vazamento de recursos internos usados pelo OpenMP.
 */
static void destroy_bucket_locks(omp_lock_t* locks, size_t table_size) {
    if (!locks) {
        return;
    }

    for (size_t i = 0; i < table_size; i++) {
        omp_destroy_lock(&locks[i]);
    }

    free(locks);
}

int main(int argc, char* argv[]) {
    /*
     * O programa espera receber o arquivo de log pela linha de comando.
     * Exemplo:
     * ./analyzer_par_lock log_distribuido.txt
     */
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <arquivo_log>\n", argv[0]);
        fprintf(stderr, "Exemplo: %s log_distribuido.txt\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* log_path = argv[1];

    /* Mede o tempo total do programa. */
    double t_start = omp_get_wtime();

    printf("[FASE 1] Carregando manifest e construindo hash table...\n");
    double t0 = omp_get_wtime();

    /* Cria a tabela hash que armazenara as URLs e seus contadores. */
    HashTable* ht = ht_create(HT_SIZE);
    if (!ht) {
        fprintf(stderr, "Falha ao criar hash table.\n");
        return EXIT_FAILURE;
    }

    /* Carrega as URLs conhecidas do manifest.txt. */
    long n_urls = load_manifest(ht, MANIFEST_FILE);

    double t1 = omp_get_wtime();
    printf("  -> %ld URLs carregadas em %.3f s\n", n_urls, t1 - t0);

    printf("[FASE 2a] Carregando log em memoria: %s\n", log_path);
    t0 = omp_get_wtime();

    long n_lines = 0;
    char** log_lines = load_log_lines(log_path, &n_lines);

    t1 = omp_get_wtime();
    printf("  -> %ld linhas carregadas em %.3f s\n", n_lines, t1 - t0);

    printf("[FASE 2b] Inicializando locks por bucket...\n");
    t0 = omp_get_wtime();

    /*
     * Cria exatamente um lock para cada bucket da tabela hash.
     * Essa e a principal diferenca em relacao ao critical global.
     */
    omp_lock_t* bucket_locks = create_bucket_locks(ht->size);

    t1 = omp_get_wtime();
    printf("  -> %zu locks inicializados em %.3f s\n", ht->size, t1 - t0);

    int n_threads_used = 0;
    long misses = 0;

    printf("[FASE 2c] Processando log em paralelo com Bucket Lock...\n");
    t0 = omp_get_wtime();

    /*
     * Regiao paralela principal.
     *
     * Cada thread processa uma parte das linhas do log. A leitura das linhas
     * ja foi feita antes, entao agora o trabalho principal e:
     * - extrair URL;
     * - calcular bucket;
     * - bloquear somente aquele bucket;
     * - buscar a URL na tabela;
     * - incrementar hit_count;
     * - liberar o bucket.
     */
    #pragma omp parallel
    {
        /* Guarda quantas threads foram usadas na execucao. */
        #pragma omp single
        n_threads_used = omp_get_num_threads();

        char url[MAX_LINE];

        #pragma omp for schedule(static) reduction(+:misses)
        for (long i = 0; i < n_lines; i++) {
            if (!extract_url(log_lines[i], url, sizeof(url))) {
                misses++;
                continue;
            }

            size_t bucket = bucket_index_for_url(url, ht->size);

            omp_set_lock(&bucket_locks[bucket]);

            CacheNode* node = ht_get(ht, url);
            if (node) {
                node->hit_count++;
            } else {
   
                misses++;
            }

            /* Libera o lock do bucket para outras threads. */
            omp_unset_lock(&bucket_locks[bucket]);
        }
    }

    t1 = omp_get_wtime();
    double t_parallel = t1 - t0;

    printf("  -> Processadas %ld linhas em %.3f s usando %d thread(s)\n",
           n_lines, t_parallel, n_threads_used);

    if (misses > 0) {
        printf("  -> AVISO: %ld linhas nao foram contabilizadas.\n", misses);
    }

    printf("[FASE 3] Salvando resultados em '%s'...\n", RESULTS_FILE);
    t0 = omp_get_wtime();

    ht_save_results(ht, RESULTS_FILE);

    t1 = omp_get_wtime();
    printf("  -> Salvo em %.3f s\n", t1 - t0);

    printf("[LIMPEZA] Liberando memoria e destruindo locks...\n");

    /* Destroi os locks criados para os buckets. */
    destroy_bucket_locks(bucket_locks, ht->size);

    /* Libera as linhas do log que foram carregadas em memoria. */
    for (long i = 0; i < n_lines; i++) {
        free(log_lines[i]);
    }
    free(log_lines);

    /* Libera a tabela hash. */
    ht_destroy(ht);

    double t_total = omp_get_wtime() - t_start;

    printf("\n[OK] Tempo total: %.3f s | Tempo paralelo (Fase 2c): %.3f s\n",
           t_total, t_parallel);

    return EXIT_SUCCESS;
}
