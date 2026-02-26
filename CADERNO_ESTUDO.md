# CADERNO DE ESTUDO - COMPUTAÇÃO PARALELA

__________

## LAB 01.1

- gcc: O nome do compilador (GNU C Compiler).
- -o hello: Especifica que o arquivo executável de saída (output) deve se chamar hello.
- hello-world.c: O arquivo de código-fonte de entrada.
- -Wall: Uma flag muito importante que habilita todos os avisos (warnings) do compilador. **(IMPORTANTE SEMPRE USAR)**

- Instrucoes do codigo
#include <unistd.h>: Inclui as definicoes para fork(), getpid() e getppid().
#include <sys/wait.h>: Inclui a definicao para wait().
pid t pid = fork();: A linha crucial. Aqui, o processo se duplica. A variável pid terá valores diferentes no pai e no filho.
if (pid == 0): Este bloco so sera executado pelo processo filho.
else: Este bloco so sera executado pelo processo pai.
getpid(): Retorna o ID do processo atual.
getppid(): Retorna o ID do processo pai do processo atual.
wait(NULL): Faz o pai pausar ate que o filho termine sua execucao.
