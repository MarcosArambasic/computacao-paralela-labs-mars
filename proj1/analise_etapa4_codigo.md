# Análise do código otimizado e respostas da Etapa 4

## Leitura técnica

O código implementa uma versão paralela otimizada do analisador de logs IoT com Pthreads. A ideia central é trocar a atualização global protegida por mutex por acumuladores locais por thread, com uma redução final ao término do processamento.

Cada thread recebe um bloco contínuo de linhas do arquivo e atualiza apenas suas estruturas privadas. Com isso, o laço principal fica sem travas e a sincronização acontece só no final, quando os resultados são consolidados.

## Como o código funciona

1. O arquivo inteiro é carregado em memória na struct `Arquivo`, que armazena `char **linhas` e `total_linhas`. fileciteturn2file0L19-L26  
2. O `main` divide as linhas entre as threads por blocos contíguos e balanceados, tratando o resto com uma linha extra para as primeiras threads. fileciteturn2file0L199-L219  
3. Cada thread executa `processar_fatia_otimizada`, faz o parsing com `sscanf`, soma alertas e energia localmente e acumula temperatura em `sensores_locais`. fileciteturn2file0L66-L104  
4. Após o `pthread_join`, a função `reduzir_resultados` combina os acumuladores locais nas estruturas globais. fileciteturn2file0L109-L135  

## Pontos fortes

- reduz overhead de sincronização;
- elimina mutex dentro do laço principal;
- mantém particionamento simples;
- usa soma e soma dos quadrados para cálculo incremental.

## Limitações

- a detecção de anomalias ainda não foi implementada no trecho enviado; fileciteturn2file0L227-L236  
- o programa ainda não imprime média dos 10 primeiros sensores de temperatura nem o sensor mais instável, saídas pedidas no projeto; fileciteturn0file0L44-L51  
- a redução final faz busca linear por sensor;
- a carga do arquivo continua sequencial.

## Texto pronto para a Etapa 4

### 5.1 Estratégia de otimização

A estratégia adotada foi o uso de acumuladores locais por thread, seguido de uma etapa de redução final. Em vez de todas as threads atualizarem estruturas globais compartilhadas durante o processamento do log, cada thread mantém suas próprias estatísticas locais. No código, isso aparece na struct `ArgThread`, que contém um vetor local de sensores, além dos acumuladores `alertas_local` e `energia_local`. fileciteturn2file0L28-L37

Durante a execução da função `processar_fatia_otimizada`, cada thread percorre apenas sua fatia do arquivo e atualiza exclusivamente seus dados privados. Isso evita a necessidade de travamento com mutex no laço principal. Ao final do processamento, a função `reduzir_resultados` combina os resultados parciais em estruturas globais. fileciteturn2file0L66-L104 fileciteturn2file0L109-L135

Essa estratégia tende a apresentar desempenho superior à versão com mutex global porque elimina o custo de lock e unlock para cada leitura relevante do log. Assim, a sincronização deixa de acontecer milhares ou milhões de vezes e passa a ocorrer apenas uma vez por thread, no momento da redução. O principal trade-off foi o aumento do uso de memória, pois cada thread mantém sua própria cópia parcial das estatísticas. Em compensação, houve redução da contenção e melhora potencial da escalabilidade.

### 5.2 Implementação

A implementação foi baseada em três decisões principais. A primeira foi carregar o arquivo inteiro em memória com a struct `Arquivo`, o que facilita a divisão do trabalho entre threads por blocos contíguos. fileciteturn2file0L19-L26 fileciteturn2file0L140-L163

A segunda decisão foi criar estruturas locais por thread, reunidas em `ArgThread`. Cada thread recebe os índices `inicio` e `fim`, processa apenas sua faixa e atualiza localmente suas estatísticas, sem acessar as estruturas globais durante o processamento principal. fileciteturn2file0L28-L37 fileciteturn2file0L199-L219

A terceira decisão foi realizar uma combinação final dos resultados. Depois do `pthread_join`, a função `reduzir_resultados` soma os contadores locais de alertas, energia e estatísticas de temperatura nas estruturas globais. Dessa forma, o custo de sincronização é deslocado para o final da execução. fileciteturn2file0L109-L135

As principais diferenças em relação à versão com mutex global são a presença de acumuladores locais por thread e a etapa de redução final.

### 5.3 Resultados

O código mede separadamente o tempo de carga do arquivo, o tempo de processamento paralelo e o tempo de detecção de anomalias. Para a análise de desempenho da Etapa 4, o valor mais relevante é o tempo de processamento (`t_proc`), pois ele representa diretamente o trecho otimizado do programa. fileciteturn2file0L181-L194 fileciteturn2file0L221-L225

A tabela pedida no relatório pode ser preenchida da seguinte forma:

| Threads | Tempo (s) | Speedup |
|---|---:|---:|
| 1 | T1 | 1,00 |
| 2 | T2 | T1/T2 |
| 4 | T4 | T1/T4 |
| 8 | T8 | T1/T8 |
| 16 | T16 | T1/T16 |

Se o professor comparar com a versão sequencial, também pode ser usado o speedup clássico `S(p) = Tseq / Tpar(p)`, conforme o enunciado do projeto. fileciteturn0file0L106-L109 

## Respostas objetivas às perguntas

**Qual estratégia você utilizou e por quê?**  
Foi utilizada uma estratégia de acumuladores locais por thread com redução final. Essa abordagem foi escolhida para evitar contenção causada por mutex global e melhorar o desempenho em ambientes com várias threads.

**Por que essa estratégia deveria apresentar melhor desempenho que a versão com mutex global?**  
Porque elimina travamentos frequentes durante o processamento das linhas do arquivo. Na prática, cada thread trabalha de forma independente na maior parte do tempo, e a sincronização ocorre apenas ao final.

**Que trade-offs você considerou?**  
Os principais trade-offs foram memória versus velocidade e simplicidade versus desempenho. A versão otimizada usa mais memória por manter estruturas locais por thread, mas reduz significativamente o overhead de sincronização.

**Como é feita a combinação dos resultados parciais ao final?**  
Depois do `pthread_join`, a thread principal percorre o vetor de argumentos das threads e soma os acumuladores locais nas variáveis globais. Para os sensores de temperatura, também soma `contagem`, `soma` e `soma_quadrados`.

**Houve necessidade de novas estruturas de dados? Quais?**  
Sim. A principal estrutura nova é `ArgThread`, que reúne os limites da fatia da thread, o ponteiro para o arquivo e os acumuladores locais.

## Observação importante

O relatório precisa deixar claro que, no estado atual do código, a detecção de anomalias ainda não foi concluída. Também faltam as saídas completas exigidas no projeto, como média dos 10 primeiros sensores de temperatura e identificação do sensor mais instável. fileciteturn2file0L227-L248
