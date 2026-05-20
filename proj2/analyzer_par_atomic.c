#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>

#include "hash_table.h"

#define HT_SIZE 131071
#define MANIFEST_FILE "manifest.txt"
#define RESULTS_FILE  "results.csv"
#define MAX_LINE 2048
#define INITIAL_CAPACITY 1024


static long load_manifest(HashTable* ht, const char* manifest_path) {
    FILE* fp = fopen(manifest_path, "r");
    if (!fp) {
        perror("Erro ao abrir manifest.txt");
        exit(EXIT_FAILURE);
    }

    char buffer[MAX_LINE];
    long count = 0;

    while (fgets(buffer, sizeof(buffer), fp)) {
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
        }
        if (buffer[0] == '\0') continue;

        ht_put(ht, buffer);
        count++;
    }

    fclose(fp);
    return count;
}


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
        if (n == capacity) {
            capacity *= 2;
            char** tmp = (char**)realloc(lines, sizeof(char*) * capacity);
            if (!tmp) {
                perror("Erro ao realocar vetor de linhas");
                fclose(fp);
                exit(EXIT_FAILURE);
            }
            lines = tmp;
        }

        lines[n] = (char*)malloc(strlen(buffer) + 1);
        if (!lines[n]) {
            perror("Erro ao alocar linha");
            fclose(fp);
            exit(EXIT_FAILURE);
        }
        strcpy(lines[n], buffer);
        n++;
    }

    fclose(fp);
    *out_n_lines = n;
    return lines;
}


static int extract_url(const char* line, char* url_out) {
    char ip[64], user[8], auth[8], timestamp[64], tz[16];
    char method[8], protocol[16];
    int status, bytes;

    int matched = sscanf(line,
        "%63s %7s %7s %63s %15s \"%7s %1023s %15[^\"]\" %d %d",
        ip, user, auth, timestamp, tz,
        method, url_out, protocol, &status, &bytes);

    return (matched >= 7) ? 1 : 0;
}


int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <arquivo_log>\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char* log_path = argv[1];

    double t_start = omp_get_wtime();

    printf("[FASE 1] Carregando manifest e construindo hash table...\n");
    double t0 = omp_get_wtime();

    HashTable* ht = ht_create(HT_SIZE);
    if (!ht) {
        fprintf(stderr, "Falha ao criar hash table.\n");
        return EXIT_FAILURE;
    }

    long n_urls = load_manifest(ht, MANIFEST_FILE);
    double t1 = omp_get_wtime();
    printf("  -> %ld URLs carregadas em %.3f s\n", n_urls, t1 - t0);

    printf("[FASE 2a] Carregando log em memória: %s\n", log_path);
    t0 = omp_get_wtime();

    long n_lines = 0;
    char** log_lines = load_log_lines(log_path, &n_lines);

    t1 = omp_get_wtime();
    printf("  -> %ld linhas carregadas em %.3f s\n", n_lines, t1 - t0);

    int n_threads_used = 0;
    long misses = 0;

    printf("[FASE 2b] Processando log em paralelo (atomic update)...\n");
    t0 = omp_get_wtime();

    #pragma omp parallel
    {
        #pragma omp single
        n_threads_used = omp_get_num_threads();

        char url[MAX_LINE];

        #pragma omp for schedule(static) reduction(+:misses)
        for (long i = 0; i < n_lines; i++) {
            if (!extract_url(log_lines[i], url)) {
                misses++;
                continue;
            }

            CacheNode* node = ht_get(ht, url);
            if (!node) {
                misses++;
                continue;
            }

            #pragma omp atomic update
            node->hit_count++;
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

    for (long i = 0; i < n_lines; i++) {
        free(log_lines[i]);
    }
    free(log_lines);
    ht_destroy(ht);

    double t_total = omp_get_wtime() - t_start;
    printf("\n[OK] Tempo total: %.3f s | Tempo paralelo (Fase 2b): %.3f s\n",
           t_total, t_parallel);

    return EXIT_SUCCESS;
}
