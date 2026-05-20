#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hash_table.h"

// Tamanho da tabela hash (primo próximo de 2^17, maior que 100.000 URLs)
#define TABLE_SIZE 131071

// Tamanho máximo de uma linha do log
#define MAX_LINE_LEN 512

/*
 * Lê o manifesto e insere cada URL na tabela hash.
 * Retorna o número de URLs carregadas, ou -1 em caso de erro.
 */
static long load_manifest(HashTable* ht, const char* filename) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        perror("Erro ao abrir manifest.txt");
        return -1;
    }

    char line[MAX_LINE_LEN];
    long count = 0;

    while (fgets(line, sizeof(line), fp)) {
        // Remove o '\n' do final
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        if (line[0] != '\0') {
            ht_put(ht, line);
            count++;
        }
    }

    fclose(fp);
    return count;
}

/*
 * Extrai a URL de uma linha de log no formato Apache/Nginx.
 * Exemplo de linha:
 *   127.0.0.1 - - [01/Nov/2025:10:00:01 -0300] "GET /video/trailer.mp4 HTTP/1.1" 200 1500
 *
 * Estratégia: busca o primeiro '"', depois "GET ", depois copia até o próximo espaço.
 * Retorna ponteiro para 'dest' em caso de sucesso, ou NULL se não encontrar.
 */
static char* extract_url(const char* line, char* dest, size_t dest_size) {
    // Acha a abertura de aspas da requisição
    const char* quote = strchr(line, '"');
    if (!quote) return NULL;

    // Avança a aspas e pula o método HTTP (ex: "GET ")
    const char* method_end = strchr(quote + 1, ' ');
    if (!method_end) return NULL;

    const char* url_start = method_end + 1;

    // A URL termina no próximo espaço
    const char* url_end = strchr(url_start, ' ');
    if (!url_end) return NULL;

    size_t url_len = (size_t)(url_end - url_start);
    if (url_len == 0 || url_len >= dest_size) return NULL;

    memcpy(dest, url_start, url_len);
    dest[url_len] = '\0';

    return dest;
}

/*
 * Processa um arquivo de log: lê cada linha, extrai a URL,
 * localiza na tabela hash e incrementa o hit_count.
 * Retorna o número de hits processados, ou -1 em caso de erro.
 */
static long process_log(HashTable* ht, const char* filename) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        perror("Erro ao abrir arquivo de log");
        return -1;
    }

    char line[MAX_LINE_LEN];
    char url[MAX_LINE_LEN];
    long processed = 0;
    long not_found  = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (extract_url(line, url, sizeof(url)) == NULL) {
            continue;
        }

        CacheNode* node = ht_get(ht, url);
        if (node) {
            node->hit_count++;
            processed++;
        } else {
            // Não deve ocorrer se o manifesto estiver correto
            not_found++;
        }
    }

    fclose(fp);

    if (not_found > 0) {
        fprintf(stderr, "Aviso: %ld URLs do log não foram encontradas na tabela hash.\n", not_found);
    }

    return processed;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <arquivo_de_log>\n", argv[0]);
        fprintf(stderr, "Exemplo: %s log_distribuido.txt\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* log_file    = argv[1];
    const char* manifest    = "manifest.txt";
    const char* output_csv  = "results.csv";

    // ------------------------------------------------------------------
    // Fase 1: Construção da tabela hash a partir do manifesto
    // ------------------------------------------------------------------
    printf("[1/3] Carregando manifesto '%s'...\n", manifest);

    HashTable* ht = ht_create(TABLE_SIZE);
    if (!ht) {
        fprintf(stderr, "Erro ao criar tabela hash.\n");
        return EXIT_FAILURE;
    }

    long url_count = load_manifest(ht, manifest);
    if (url_count < 0) {
        ht_destroy(ht);
        return EXIT_FAILURE;
    }
    printf("    -> %ld URLs carregadas na tabela hash.\n", url_count);

    // ------------------------------------------------------------------
    // Fase 2: Processamento do log
    // ------------------------------------------------------------------
    printf("[2/3] Processando log '%s'...\n", log_file);

    long hits = process_log(ht, log_file);
    if (hits < 0) {
        ht_destroy(ht);
        return EXIT_FAILURE;
    }
    printf("    -> %ld requisições processadas.\n", hits);

    // ------------------------------------------------------------------
    // Fase 3: Salvar resultados
    // ------------------------------------------------------------------
    printf("[3/3] Salvando resultados em '%s'...\n", output_csv);
    ht_save_results(ht, output_csv);
    printf("    -> Concluído.\n");

    // ------------------------------------------------------------------
    // Limpeza
    // ------------------------------------------------------------------
    ht_destroy(ht);

    return EXIT_SUCCESS;
}
