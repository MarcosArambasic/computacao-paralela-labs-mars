#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Gerador de Logs de Sensores IoT para Data Center
Uso: python generate_sensor_log.py --sensores 100 --dias 7 --output sensores.log
"""

import random
import datetime
import time
import argparse
import math

# --- Configurações padrão ---
DEFAULT_SENSORES = 50
DEFAULT_DIAS = 1
DEFAULT_OUTPUT = "sensores.log"

# --- Tipos de sensores e suas faixas típicas ---
TIPOS_SENSORES = [
    {
        'nome': 'temperatura',
        'unidade': '°C',
        'min': 18.0,
        'max': 35.0,
        'alerta_min': 28.0,
        'alerta_max': 32.0,
        'critico_min': 32.0,
        'critico_max': 40.0
    },
    {
        'nome': 'umidade',
        'unidade': '%',
        'min': 30.0,
        'max': 70.0,
        'alerta_min': 60.0,
        'alerta_max': 80.0,
        'critico_min': 80.0,
        'critico_max': 100.0
    },
    {
        'nome': 'energia',
        'unidade': 'W',
        'min': 100.0,
        'max': 800.0,
        'alerta_min': 700.0,
        'alerta_max': 900.0,
        'critico_min': 900.0,
        'critico_max': 1200.0
    },
    {
        'nome': 'corrente',
        'unidade': 'A',
        'min': 0.5,
        'max': 5.0,
        'alerta_min': 4.0,
        'alerta_max': 5.5,
        'critico_min': 5.5,
        'critico_max': 8.0
    },
    {
        'nome': 'pressao',
        'unidade': 'Pa',
        'min': 95000.0,
        'max': 105000.0,
        'alerta_min': 102000.0,
        'alerta_max': 106000.0,
        'critico_min': 106000.0,
        'critico_max': 110000.0
    }
]

# Mapeamento de tipos para IDs de sensores
SENSORES_POR_TIPO = {
    'temperatura': [],  # será preenchido dinamicamente
    'umidade': [],
    'energia': [],
    'corrente': [],
    'pressao': []
}

# --- Status possíveis ---
STATUS = ['OK', 'ALERTA', 'CRITICO']

# --- Função para determinar status baseado no valor e tipo ---
def determinar_status(valor, tipo_sensor):
    """Retorna OK, ALERTA ou CRITICO baseado nos limiares do sensor"""
    if valor < tipo_sensor['alerta_min']:
        return 'OK'
    elif valor < tipo_sensor['critico_min']:
        return 'ALERTA'
    else:
        return 'CRITICO'

# --- Função para gerar valor com variação temporal (sazonalidade) ---
def gerar_valor_com_tendencia(tipo_sensor, hora_do_dia, tendencia_anomalia=0):
    """
    Gera valores com padrões realistas:
    - Temperatura sobe durante o dia, cai à noite
    - Consumo de energia acompanha carga de trabalho
    - Possibilidade de anomalias (tendencia_anomalia != 0)
    """
    base = (tipo_sensor['min'] + tipo_sensor['max']) / 2
    amplitude = (tipo_sensor['max'] - tipo_sensor['min']) / 2
    
    # Sazonalidade diária (senoide)
    if tipo_sensor['nome'] == 'temperatura':
        # Mais quente às 14h (14 = pico), mais frio às 4h
        variacao = math.sin((hora_do_dia - 4) * math.pi / 12) * amplitude * 0.7
    elif tipo_sensor['nome'] == 'energia':
        # Pico de consumo durante o dia (horário comercial)
        variacao = math.sin((hora_do_dia - 12) * math.pi / 12) * amplitude * 0.5
        if 9 <= hora_do_dia <= 18:
            variacao += amplitude * 0.3  # aumento no horário comercial
    else:
        # Variação aleatória para outros sensores
        variacao = random.gauss(0, amplitude * 0.2)
    
    valor = base + variacao + random.gauss(0, amplitude * 0.1)
    
    # Adicionar anomalia se necessário
    if tendencia_anomalia != 0:
        valor += tendencia_anomalia * amplitude * 2
    
    # Garantir que fique dentro dos limites extremos
    return max(tipo_sensor['min'] * 0.8, min(tipo_sensor['max'] * 1.2, valor))

# --- Função principal de geração ---
def gerar_log(num_sensores, num_dias, output_file, prob_anomalia=0.01):
    """
    Gera arquivo de log com leituras de sensores
    - num_sensores: quantidade de sensores (serão divididos entre os tipos)
    - num_dias: período em dias (1 dia = 86400 leituras por sensor a 1Hz)
    - prob_anomalia: probabilidade de uma leitura ser anômala (desvio intencional)
    """
    
    print(f"Gerando log com {num_sensores} sensores por {num_dias} dia(s)...")
    print(f"Total estimado de linhas: {num_sensores * num_dias * 86400:,}")
    
    # Distribuir sensores entre os tipos
    sensores_por_tipo = {}
    tipos_disponiveis = TIPOS_SENSORES.copy()
    
    for i in range(1, num_sensores + 1):
        # Escolhe um tipo aleatoriamente (com distribuição realista)
        # Mais sensores de temperatura/umidade, menos de pressão
        pesos = [0.4, 0.3, 0.15, 0.1, 0.05]  # temp, umid, energia, corrente, pressão
        tipo = random.choices(TIPOS_SENSORES, weights=pesos, k=1)[0]
        tipo_nome = tipo['nome']
        
        sensor_id = f"sensor_{i:03d}"
        if tipo_nome not in sensores_por_tipo:
            sensores_por_tipo[tipo_nome] = []
        sensores_por_tipo[tipo_nome].append({
            'id': sensor_id,
            'tipo': tipo
        })
    
    # Estatísticas para exibição
    total_linhas = 0
    inicio_global = datetime.datetime(2026, 3, 1, 0, 0, 0)
    
    print("\nDistribuição dos sensores:")
    for tipo, sensores in sensores_por_tipo.items():
        print(f"  {tipo}: {len(sensores)} sensores")
    
    print(f"\nGerando arquivo: {output_file}")
    start_time = time.time()
    
    with open(output_file, 'w') as f:
        # Para cada dia
        for dia in range(num_dias):
            data_base = inicio_global + datetime.timedelta(days=dia)
            print(f"\nDia {dia+1}/{num_dias}...")
            
            # Para cada segundo do dia (24h * 3600s = 86400)
            # Vamos gerar em blocos para mostrar progresso
            segundos_por_dia = 24 * 3600
            blocos = 24  # mostrar progresso a cada hora
            
            for bloco in range(blocos):
                inicio_bloco = bloco * (segundos_por_dia // blocos)
                fim_bloco = (bloco + 1) * (segundos_por_dia // blocos)
                
                for segundo in range(inicio_bloco, fim_bloco):
                    timestamp = data_base + datetime.timedelta(seconds=segundo)
                    hora = timestamp.hour
                    
                    # Para cada sensor, gerar uma leitura
                    for tipo_nome, sensores in sensores_por_tipo.items():
                        tipo_info = next(t for t in TIPOS_SENSORES if t['nome'] == tipo_nome)
                        
                        for sensor in sensores:
                            # Decidir se esta leitura será anômala
                            anomalia = 0
                            if random.random() < prob_anomalia:
                                anomalia = random.choice([-1, 1])  # desvio negativo ou positivo
                            
                            # Gerar valor com tendência baseada na hora
                            valor = gerar_valor_com_tendencia(tipo_info, hora, anomalia)
                            
                            # Determinar status
                            status = determinar_status(valor, tipo_info)
                            
                            # Escrever linha no formato especificado
                            linha = (f"{sensor['id']} {timestamp.strftime('%Y-%m-%d %H:%M:%S')} "
                                    f"{tipo_nome} {valor:.1f} status {status}\n")
                            f.write(linha)
                            total_linhas += 1
                
                # Mostrar progresso
                progresso = (bloco + 1) / blocos * 100
                print(f"\r  Hora {bloco+1:2d}/24: {progresso:.0f}%", end="")
            
            print()  # nova linha após cada dia
    
    end_time = time.time()
    tamanho_mb = os.path.getsize(output_file) / (1024 * 1024)
    
    print(f"\n\nArquivo gerado com sucesso!")
    print(f"Total de linhas: {total_linhas:,}")
    print(f"Tamanho do arquivo: {tamanho_mb:.2f} MB")
    print(f"Tempo de geração: {end_time - start_time:.2f} segundos")

# --- Ponto de entrada ---
if __name__ == "__main__":
    import os  # para getsize
    
    parser = argparse.ArgumentParser(description='Gerador de Logs de Sensores IoT')
    parser.add_argument('--sensores', type=int, default=DEFAULT_SENSORES,
                       help=f'Número de sensores (padrão: {DEFAULT_SENSORES})')
    parser.add_argument('--dias', type=int, default=DEFAULT_DIAS,
                       help=f'Número de dias (padrão: {DEFAULT_DIAS})')
    parser.add_argument('--output', type=str, default=DEFAULT_OUTPUT,
                       help=f'Arquivo de saída (padrão: {DEFAULT_OUTPUT})')
    parser.add_argument('--anomalia', type=float, default=0.01,
                       help='Probabilidade de anomalia (0.0 a 1.0, padrão: 0.01)')
    
    args = parser.parse_args()
    
    print("=" * 60)
    print("GERADOR DE LOGS DE SENSORES IoT PARA DATA CENTER")
    print("=" * 60)
    
    gerar_log(args.sensores, args.dias, args.output, args.anomalia)
