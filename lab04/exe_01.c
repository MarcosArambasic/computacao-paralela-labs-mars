#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>

#define NUM_THREADS 4
#define INCREMENTS_PER_THREAD 1000000
#define TAM_VETOR 100000000

long long global_sum = 0;
long long counter = 0;

int vetor[TAM_VETOR];

typedef struct {
    int inicio;
    int fim;
} thread_limites;


void* somar_func(void* arg) {
    thread_limites* limites = (thread_limites*)arg;

    for(int i = limites->inicio; i < limites->fim; i++){
        global_sum += vetor[i];
    }

    free(limites);
    return NULL;
}


int main() {
    pthread_t threads[NUM_THREADS];
    long long soma_esperada = 0;

    for(int i = 0; i < NUM_THREADS; i++){
        vetor[i] = 1;
        soma_esperada += 1;
    }
    
    int fatia = TAM_VETOR / NUM_THREADS;

    for(int i = 0; i < NUM_THREADS; i++) {
        thread_limites* limites = malloc(sizeof(thread_limites));
        limites->inicio = i * fatia;
        limites->fim = (i == NUM_THREADS - 1) ? TAM_VETOR : (i + 1) * fatia;

        if(pthread_create(&threads[i], NULL, somar_func, (void*)limites) != 0) {
            perror("Erro ao criar thread");
            return 1;
        }
    }

    // Esperar todas as threads terminarem
    for (int i = 0; i < NUM_THREADS; ++i) {
        pthread_join(threads[i], NULL);
    }

     // 4. Exibir resultados
    printf("--- Resultado da Soma de Vetor ---\n");
    printf("Soma calculada (global_sum): %lld\n", global_sum);
    printf("Soma esperada:               %lld\n", soma_esperada);

    if (global_sum != soma_esperada) {
        printf("Diferenca: %lld. CONDICAO DE CORRIDA DETECTADA!\n", soma_esperada - global_sum);
    } else {
        printf("Resultado correto! (Raro em 100M de elementos sem proteção)\n");
    }

    return 0;

}
