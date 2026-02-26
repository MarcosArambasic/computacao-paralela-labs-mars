#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

int main() {
    pid_t pid = fork();
    
    if(pid < 0){ // Erro
        fprintf(stderr , " Fork falhou !\n") ;
        return 1;
    }else if (pid == 0) { // Processo Filho
        printf("Eu sou o pai! Meu PID e %d, meu filho e %d.\n", getppid(),getpid());
    }else{ // Processo Pai
        printf("Eu sou o filho! Meu PID e %d, fui criado pelo pai %d.\n", getpid(), pid);
        wait(NULL); // Pai espera o filho terminar
        printf("Meu pai terminou, agora eu posso terminar.\n") ;
    }
    return 0;
}