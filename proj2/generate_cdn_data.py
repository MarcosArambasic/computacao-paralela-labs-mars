#!/usr/bin/env python3

import random
import datetime
import time
import zipfile
import os
import sys
from collections import defaultdict

# --- Parâmetros de Configuração ---

# Quantidade de URLs únicas a serem geradas no manifesto
NUM_UNIQUE_URLS = 100_000

# Quantidade de linhas a serem geradas em CADA arquivo de log
NUM_LOG_LINES = 10_000_000

# Para o log concorrente (hotspot)
HOTSPOT_PERCENTAGE = 0.90  # 90% do tráfego irá para os "hotspots"
HOTSPOT_SIZE = 10           # Número de URLs que são "hotspots"

# Nomes dos arquivos de saída
MANIFEST_FILE = "manifest.txt"
DISTRIBUTED_LOG_FILE = "log_distribuido.txt"
CONCURRENT_LOG_FILE = "log_concorrente.txt"

# --- NOVOS Arquivos de Gabarito ---
GABARITO_DISTRIBUIDO_FILE = "gabarito_distribuido.csv"
GABARITO_CONCORRENTE_FILE = "gabarito_concorrente.csv"

ZIP_FILE = "cdn_data_logs.zip"

# --- Dados para Geração Aleatória ---
RESOURCES = [
    "/index.html", "/images/logo.png", "/css/style.css", "/js/main.js",
    "/about.html", "/contact.html", "/products/item1.html", "/login",
    "/admin/", "/assets/font.woff2", "/api/data", "/images/banner.jpg",
    "/video/promo.mp4", "/audio/podcast.mp3", "/docs/api.pdf",
    "/favicon.ico", "/robots.txt", "/sitemap.xml"
]

# --- Funções ---

def generate_random_url():
    """Gera uma URL aleatória com um ID único."""
    resource = random.choice(RESOURCES)
    unique_id = ''.join(random.choices("abcdef0123456789", k=12))
    
    if resource.startswith('/'):
        resource = resource[1:]
        
    return f"/{resource.split('.')[0]}-{unique_id}.{resource.split('.')[-1]}"

def generate_manifest(filename, num_urls):
    """Gera o arquivo de manifesto com URLs únicas."""
    print(f"[1/5] Gerando manifest com {num_urls:,} URLs únicas...")
    urls = set()
    while len(urls) < num_urls:
        urls.add(generate_random_url())
    
    with open(filename, "w") as f:
        for url in urls:
            f.write(f"{url}\n")
            
    print(f"-> Arquivo '{filename}' gerado.")
    return list(urls)

def write_log_batch(f, urls, num_lines):
    """Escreve um lote de linhas de log no arquivo."""
    current_time = datetime.datetime(2025, 11, 1, 10, 0, 0)
    ip = "127.0.0.1"
    status = 200
    bytes_sent = 1500
    
    batch = []
    for i in range(num_lines):
        resource = urls[i] # Usa a URL pré-escolhida
        time_delta = datetime.timedelta(seconds=random.randint(0, 1))
        current_time += time_delta
        timestamp = current_time.strftime('[%d/%b/%Y:%H:%M:%S %z-0300]')
        log_line = f'{ip} - - {timestamp} "GET {resource} HTTP/1.1" {status} {bytes_sent}\n'
        batch.append(log_line)
        
    f.writelines(batch)

def generate_log_file(filename, num_lines, url_list, hotspot_urls=None):
    """Gera um arquivo de log E RETORNA A CONTAGEM DE HITS."""
    
    # NOVO: Dicionário para contar hits
    hit_counts = defaultdict(int)
    
    BATCH_SIZE = 1_000_000 
    num_batches = (num_lines + BATCH_SIZE - 1) // BATCH_SIZE
    
    with open(filename, "w") as f:
        for i in range(num_batches):
            lines_in_batch = min(BATCH_SIZE, num_lines - (i * BATCH_SIZE))
            
            urls_to_write = []
            if hotspot_urls:
                # Modo Concorrente (Hotspot)
                for _ in range(lines_in_batch):
                    url = ""
                    if random.random() < HOTSPOT_PERCENTAGE:
                        url = random.choice(hotspot_urls)
                    else:
                        url = random.choice(url_list)
                    urls_to_write.append(url)
                    hit_counts[url] += 1 # Conta o hit
            else:
                # Modo Distribuído
                urls_to_write = random.choices(url_list, k=lines_in_batch)
                for url in urls_to_write:
                    hit_counts[url] += 1 # Conta o hit
            
            write_log_batch(f, urls_to_write, lines_in_batch)
            print(f"\r -> Progresso: {((i + 1) / num_batches) * 100:.0f}%", end="")
    print(f"\n-> Arquivo '{filename}' gerado.")
    return hit_counts

def save_gabarito(filename, all_urls, hit_counts):
    """NOVO: Salva o arquivo de gabarito (CSV) em ordem alfabética."""
    print(f" -> Gerando gabarito '{filename}'...")
    
    # Garante que todas as URLs do manifesto estejam no gabarito,
    # mesmo que tenham 0 hits (pouco provável, mas garante consistência)
    final_counts = {url: 0 for url in all_urls}
    final_counts.update(hit_counts)

    with open(filename, "w") as f:
        # Salva em ordem alfabética de URL
        for url in sorted(final_counts.keys()):
            count = final_counts[url]
            f.write(f"{url},{count}\n")
    print(f" -> Gabarito '{filename}' gerado.")


def zip_files(zip_name, files_to_zip):
    """Compacta os arquivos de log e o manifesto."""
    print(f"[5/5] Compactando arquivos em '{zip_name}'...")
    with zipfile.ZipFile(zip_name, 'w', compression=zipfile.ZIP_DEFLATED) as zf:
        for file in files_to_zip:
            if os.path.exists(file):
                zf.write(file)
                os.remove(file)
            else:
                print(f"Aviso: Arquivo '{file}' não encontrado.")
    print(f"-> Arquivos compactados e originais removidos.")

# --- Execução Principal ---
def main():
    start_time = time.time()
    print("--- Gerador de Dados para Cache de CDN (LAB2) v2 ---")
    
    try:
        # 1. Gerar Manifesto
        all_urls = generate_manifest(MANIFEST_FILE, NUM_UNIQUE_URLS)
        
        # 2. Gerar Log Distribuído
        print(f"[2/5] Gerando log distribuído com {NUM_LOG_LINES:,} linhas...")
        dist_hits = generate_log_file(DISTRIBUTED_LOG_FILE, NUM_LOG_LINES, all_urls)
        
        # 3. Gerar Log Concorrente (Hotspot)
        print(f"[3/5] Gerando log concorrente com {NUM_LOG_LINES:,} linhas...")
        if len(all_urls) < HOTSPOT_SIZE:
            print(f"Erro: O número de URLs únicas é menor que o HOTSPOT_SIZE.")
            sys.exit(1)
            
        hotspot_list = random.sample(all_urls, HOTSPOT_SIZE)
        conc_hits = generate_log_file(CONCURRENT_LOG_FILE, NUM_LOG_LINES, all_urls, hotspot_list)

        # 4. Gerar Gabaritos
        print("[4/5] Gerando arquivos de gabarito (CSV)...")
        save_gabarito(GABARITO_DISTRIBUIDO_FILE, all_urls, dist_hits)
        save_gabarito(GABARITO_CONCORRENTE_FILE, all_urls, conc_hits)

        # 5. Compactar tudo
        files_to_zip = [
            MANIFEST_FILE, DISTRIBUTED_LOG_FILE, CONCURRENT_LOG_FILE,
            GABARITO_DISTRIBUIDO_FILE, GABARITO_CONCORRENTE_FILE
        ]
        zip_files(ZIP_FILE, files_to_zip)

        end_time = time.time()
        print("\n--- Concluído ---")
        print(f"Arquivo '{ZIP_FILE}' (v2) gerado com sucesso em {end_time - start_time:.2f} segundos.")
        print("Este arquivo contém o manifesto, os dois logs e os dois gabaritos.")
        
    except KeyboardInterrupt:
        print("\nOperação interrompida.")
        sys.exit(1)
    except Exception as e:
        print(f"\nOcorreu um erro: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()