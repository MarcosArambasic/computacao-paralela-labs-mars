#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "hash_table.h"

#define max_line 1024
#define hash_size 131071 
#define max_log_lines 10000000 

int main(int argc, char *argv[]) {
    // verifica se o usuario passou o nome do arquivo de log ao rodar
    if (argc < 2) {
        printf("uso: %s <arquivo_de_log>\n", argv[0]);
        return 1;
    }

    // cria a estrutura da tabela que vai guardar os contadores
    HashTable *ht = ht_create(hash_size);

    // fase 1 le o arquivo manifest que tem a lista de sites conhecidos
    FILE *mf = fopen("manifest.txt", "r");
    if (!mf) {
        perror("erro ao abrir manifest.txt");
        return 1;
    }

    char line[max_line];
    while (fgets(line, max_line, mf)) {
        // limpa restos de quebra de linha para o nome ficar limpo
        line[strcspn(line, "\r\n")] = 0; 
        if (strlen(line) > 0) {
            // coloca o nome na tabela com contador valendo zero
            ht_put(ht, line);
        }
    }
    fclose(mf);

    // abre o arquivo de log para comecar a leitura dos dados
    FILE *lf = fopen(argv[1], "r");
    if (!lf) {
        perror("erro ao abrir arquivo de log");
        return 1;
    }

    // reserva um espaco gigante na memoria para guardar as linhas do log
    char **logs = malloc(max_log_lines * sizeof(char*));
    int count = 0;
    // le as linhas do arquivo e guarda na memoria para as threads trabalharem
    while (count < max_log_lines && fgets(line, max_line, lf)) {
        logs[count++] = strdup(line);
    }
    fclose(lf);

    // fase 2 aqui o trabalho e dividido entre varios nucleos do computador
    #pragma omp parallel for
    for (int i = 0; i < count; i++) {
        // procura o pedaco do texto que indica o inicio do endereco
        char *start = strstr(logs[i], "GET");
        if (start) {
            start += 4; // pula o texto get e o espaco
            char *end = strchr(start, ' '); // procura o espaco que vem depois do nome
            if (end) {
                int url_len = end - start;
                char url_buffer[max_line];
                // copia apenas o nome do endereco para um pote temporario
                strncpy(url_buffer, start, url_len);
                url_buffer[url_len] = '\0';

                // busca esse endereco na tabela que criamos antes
                CacheNode *node = ht_get(ht, url_buffer);
                if (node) {
                    // so uma thread por vez pode entrar aqui para nao quebrar a conta
                    #pragma omp critical
                    {
                        node->hit_count++;
                    }
                }
            }
        }
    }

    // depois de contar tudo grava o resultado final no arquivo csv
    ht_save_results(ht, "results.csv");

    // limpa a memoria que usamos para nao deixar o computador lento
    for(int i = 0; i < count; i++) free(logs[i]);
    free(logs);
    ht_destroy(ht);
    return 0;
}
