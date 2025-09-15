# Processamento de Imagens PGM com IPC e Threads

Este projeto implementa um sistema de **processamento de imagens PGM** em C utilizando:

- **Comunicação entre processos (IPC)** via **FIFO (named pipe)**.  
- **Paralelismo com threads**, sincronizadas com **mutex** e **semáforos**.  

São suportados dois modos de processamento:  
- **Negativo** → inverte os pixels da imagem.  
- **Slice (Fatiamento)** → mantém apenas pixels dentro de um intervalo `[t1, t2]`, zerando os demais.  

---

## Estrutura do Projeto

- **sender.c** → Lê a imagem PGM, prepara o cabeçalho e envia os dados via FIFO.  
- **worker.c** → Recebe os dados, distribui tarefas entre threads e aplica o processamento.  

---

## Compilação

Para compilar os módulos, use o `gcc`:

```bash
gcc sender.c -o sender
gcc worker.c -o worker -lpthread
```

---

## Execução

A execução ocorre em **dois processos** distintos:  

### 1. Iniciar o `worker` (consumidor)
```bash
./worker <fifo_path> <saida.pgm> <modo> [parametros...]
```

- `<fifo_path>` → caminho do FIFO (ex.: `fifo1`).  
- `<saida.pgm>` → nome da imagem de saída processada.  
- `<modo>` → `negativo` ou `fatiamento`.  

Parâmetros adicionais:
- **Modo negativo:**  
  ```bash
  ./worker fifo1 saida_neg.pgm negativo [num_threads]
  ```
  (se `num_threads` não for passado, usa 4 threads por padrão).  

- **Modo fatiamento:**  
  ```bash
  ./worker fifo1 saida_slice.pgm fatiamento <t1> <t2> [num_threads]
  ```
  Exemplo:  
  ```bash
  ./worker fifo1 saida_slice.pgm fatiamento 100 200 4
  ```

---

### 2. Executar o `sender` (produtor)
```bash
./sender <fifo_path> <entrada.pgm>
```

- `<fifo_path>` → mesmo caminho FIFO passado ao worker.  
- `<entrada.pgm>` → imagem de entrada no formato **PGM binário (P5)**.  

Exemplo:
```bash
./sender fifo1 entrada.pgm
```

---

## Fluxo de Execução

1. O **worker** cria o FIFO e fica aguardando dados.  
2. O **sender** abre o mesmo FIFO, envia cabeçalho + pixels da imagem.  
3. O **worker** lê os dados, distribui o trabalho entre as threads e aplica o modo escolhido.  
4. A imagem processada é salva no arquivo de saída especificado.  

---

## Exemplo Completo

```bash
# Terminal 1 - iniciar worker (modo negativo com 4 threads)
./worker fifo1 saida_neg.pgm negativo 4

# Terminal 2 - enviar imagem
./sender fifo1 entrada.pgm
```

```bash
# Terminal 1 - iniciar worker (modo slice com t1=100, t2=200, 8 threads)
./worker fifo1 saida_slice.pgm fatiamento 100 200 8

# Terminal 2 - enviar imagem
./sender fifo1 entrada.pgm
```

---

## Requisitos

- Sistema Unix-like (Linux ou WSL recomendado).  
- Compilador `gcc`.  
- Biblioteca `pthread`.  
- Imagens no formato **PGM (P5, binário)**.  

---

## Resultados Esperados

- Redução significativa no tempo de processamento com aumento do número de threads.  
- Ganho de desempenho especialmente perceptível em imagens grandes (>100MB).  
