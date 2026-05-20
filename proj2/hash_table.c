#include "hash_table.h"

// --- Constantes Internas ---
#define FNV_OFFSET_BASIS 2166136261UL
#define FNV_PRIME 16777619UL

// --- Funções Privadas ---

/*
 * Função de Hash (djb2)
 * Converte uma string (URL) em um índice para a tabela.
 * Fonte: http://www.cse.yorku.ca/~oz/hash.html
 */
static size_t hash_djb2(const char* str, size_t size) {
    unsigned long hash = 5381;
    int c;

    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }

    return hash % size;
}

/*
 * Cria um novo nó de cache (função auxiliar).
 */
static CacheNode* create_node(const char* url) {
    CacheNode* node = (CacheNode*)malloc(sizeof(CacheNode));
    if (!node) {
        perror("Erro ao alocar CacheNode");
        exit(EXIT_FAILURE);
    }
    
    // Aloca e copia a URL
    node->url = (char*)malloc(strlen(url) + 1);
    if (!node->url) {
        perror("Erro ao alocar string da URL");
        free(node);
        exit(EXIT_FAILURE);
    }
    strcpy(node->url, url);
    
    node->hit_count = 0; // Inicializa o contador
    node->next = NULL;
    return node;
}

// --- Implementação da API Pública ---

/*
 * Cria uma nova Tabela Hash
 */
HashTable* ht_create(size_t size) {
    if (size < 1) {
        fprintf(stderr, "Tamanho da tabela deve ser ao menos 1\n");
        return NULL;
    }
    
    // Aloca a estrutura principal
    HashTable* ht = (HashTable*)malloc(sizeof(HashTable));
    if (!ht) {
        perror("Erro ao alocar HashTable");
        return NULL;
    }
    
    // Aloca os "buckets" (array de ponteiros)
    // Usamos calloc para inicializar todos os ponteiros (buckets) como NULL
    ht->table = (CacheNode**)calloc(size, sizeof(CacheNode*));
    if (!ht->table) {
        perror("Erro ao alocar buckets da tabela");
        free(ht);
        return NULL;
    }
    
    ht->size = size;
    return ht;
}

/*
 * Libera toda a memória
 */
void ht_destroy(HashTable* ht) {
    if (!ht) return;
    
    for (size_t i = 0; i < ht->size; i++) {
        CacheNode* current = ht->table[i];
        while (current) {
            CacheNode* next = current->next;
            free(current->url); // Libera a string copiada
            free(current);      // Libera o nó
            current = next;
        }
    }
    
    free(ht->table); // Libera o array de buckets
    free(ht);        // Libera a estrutura principal
}

/*
 * Insere um novo elemento
 */
void ht_put(HashTable* ht, const char* url) {
    if (!ht || !url) return;
    
    // 1. Calcula o índice (bucket)
    size_t index = hash_djb2(url, ht->size);
    
    // 2. Verifica se a chave já existe (não faremos nada se existir)
    CacheNode* current = ht->table[index];
    while (current) {
        if (strcmp(current->url, url) == 0) {
            // Chave já existe. No nosso caso (manifesto), isso não deve
            // acontecer se o manifesto for de URLs únicas.
            return; 
        }
        current = current->next;
    }
    
    // 3. Cria o novo nó
    CacheNode* new_node = create_node(url);
    
    // 4. Insere no início da lista encadeada (bucket)
    new_node->next = ht->table[index];
    ht->table[index] = new_node;
}

/*
 * Busca um elemento
 */
CacheNode* ht_get(HashTable* ht, const char* url) {
    if (!ht || !url) return NULL;
    
    // 1. Calcula o índice (bucket)
    size_t index = hash_djb2(url, ht->size);
    
    // 2. Percorre a lista encadeada daquele bucket
    CacheNode* current = ht->table[index];
    while (current) {
        if (strcmp(current->url, url) == 0) {
            return current; // Encontrou!
        }
        current = current->next;
    }
    
    return NULL; // Não encontrou
}

/*
 * (NOVA FUNÇÃO) Salva os resultados em um arquivo CSV.
 */
void ht_save_results(HashTable* ht, const char* filename) {
    if (!ht || !filename) {
        fprintf(stderr, "Erro: Tabela ou nome de arquivo nulo ao salvar resultados.\n");
        return;
    }

    FILE* fp = fopen(filename, "w");
    if (!fp) {
        perror("Erro ao abrir arquivo de resultados");
        return;
    }

    // Itera por todos os buckets
    for (size_t i = 0; i < ht->size; i++) {
        CacheNode* current = ht->table[i];
        // Itera por toda a lista encadeada (colisões)
        while (current) {
            // Formato: url,hit_count
            fprintf(fp, "%s,%ld\n", current->url, current->hit_count);
            current = current->next;
        }
    }

    fclose(fp);
}


/*
 * Função de depuração
 */
void ht_print(HashTable* ht) {
    if (!ht) return;
    printf("--- Estado da Tabela Hash (Size: %zu) ---\n", ht->size);
    for (size_t i = 0; i < ht->size; i++) {
        printf("Bucket[%zu]: ", i);
        CacheNode* current = ht->table[i];
        if (!current) {
            printf("~ VAZIO ~\n");
            continue;
        }
        
        while (current) {
            printf("[\"%s\" (%ld)] -> ", current->url, current->hit_count);
            current = current->next;
        }
        printf("NULL\n");
    }
    printf("-----------------------------------------\n");
}