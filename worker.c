#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>

// funcao para calcular tempo de processamento
// estatistica usada no relatorio
double now() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

#define QMAX 128
#define MODE_NEG 0
#define MODE_SLICE 1

typedef struct {
    int w, h, maxv;
    unsigned char* data;
} PGM;

typedef struct {
    int w, h, maxv; // metadados da imagem
    int mode;       // 0 = negativo e 1 slice  
    int t1, t2;     // parametros do slice, t1 e t2
} Header;

typedef struct {
    int row_start; //linha inicial (inclusiva)
    int row_end; // linha final (exclusiva)
} Task;

Task queue_buf[QMAX]; // vetor da fila de tarefas, evita realocação de memoria
int q_head = 0, q_tail = 0, q_count = 0;

pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
sem_t sem_items; // quantas tarefas disponiveis
sem_t sem_space; //espaco livre na fila

pthread_mutex_t done_lock = PTHREAD_MUTEX_INITIALIZER;
sem_t sem_done; // sinaliza quando todas as tarefas finalizam
int remaining_tasks = 0;

// dados compartilhados para o processamento
PGM g_in, g_out;
int g_mode;
int g_t1, g_t2;
int g_nthreads;

void apply_negative_block(int rs, int re) {
    for (int y = rs; y < re; y++) {
        for (int x = 0; x < g_in.w; x++) {
            int idx = y * g_in.w + x; // posição no vetor data
            g_out.data[idx] = 255 - g_in.data[idx]; // 255 - pixel
        }
    }
}

void apply_slice_block(int rs, int re, int t1, int t2) {
    for (int y = rs; y < re; y++) {
        for (int x = 0; x < g_in.w; x++) {
            int idx = y * g_in.w + x;
            unsigned char pixel = g_in.data[idx];
            
            if (pixel <= t1 || pixel >= t2) {
                g_out.data[idx] = 0; // fora do intervalo = preto
            } else {
                g_out.data[idx] = pixel; // dentro do intervalo = mantidos
            }
        }
    }
}

void* worker_thread(void* arg) { // cada thread de trabalho:
    while (1) {
        sem_wait(&sem_items); // espera por tarefa disponivel
        pthread_mutex_lock(&q_lock); // acessa ou modifica fila, acessa com segurança mutex
// uma thread por vez pode modificar q_head q_tail e q_count
        
        if (q_count == 0) {
            pthread_mutex_unlock(&q_lock);
            break;
        }
        
        Task task = queue_buf[q_head]; // fila
        q_head = (q_head + 1) % QMAX;
        q_count--;
        
        pthread_mutex_unlock(&q_lock);
        sem_post(&sem_space); // libera espaço na fila
        
        if (g_mode == MODE_NEG) {
            apply_negative_block(task.row_start, task.row_end);
        } else {
            apply_slice_block(task.row_start, task.row_end, g_t1, g_t2);
        }
        
        pthread_mutex_lock(&done_lock);
        remaining_tasks--; // contador compartilhado, cada thread decrementa até terminar sua tarefa, com mutex apenas 1
        // thread acessa
        if (remaining_tasks == 0) {
            sem_post(&sem_done); // tudo terminou, thread worker avisa para fechar as demais threads
        }
        pthread_mutex_unlock(&done_lock); // desbloqueia o contador e protege o contador de tarefas restantes
    }
    return NULL;
}

int write_pgm(const char* path, const PGM* img) {
    FILE* file = fopen(path, "wb");
    if (!file) return -1;
    
    fprintf(file, "P5\n%d %d\n%d\n", img->w, img->h, img->maxv);
    fwrite(img->data, 1, img->w * img->h, file);
    fclose(file);
    return 0;
}

int main(int argc, char** argv) {
    const char* fifo_path = argv[1];
    const char* output_path = argv[2];
    const char* mode_str = argv[3];
    double t0 = now();
    if (strcmp(mode_str, "negativo") == 0) {
        g_mode = MODE_NEG;
        g_nthreads = (argc >= 5) ? atoi(argv[4]) : 4;
    } else if (strcmp(mode_str, "fatiamento") == 0) {
        if (argc < 6) {
            fprintf(stderr, "modo fatiamento requer t1 e t2\n");
            exit(1);
        }
        g_mode = MODE_SLICE;
        g_t1 = atoi(argv[4]);
        g_t2 = atoi(argv[5]);
        g_nthreads = (argc >= 7) ? atoi(argv[6]) : 4;
    } else {
        fprintf(stderr, "Modo inválido\n");
        exit(1);
    }
    // 777 porque 666 nao estava deixando criar no WSL
    mkfifo(fifo_path, 0777);
    
    int fd = open(fifo_path, O_RDONLY);
    if (fd == -1) {
        perror("Não abriu a FIFO para leitura");
        exit(1);
    }
    
    // LEITURA CORRETA DO CABEÇALHO
    Header header;
    ssize_t bytes_read = read(fd, &header, sizeof(Header));
    if (bytes_read != sizeof(Header)) {
        perror("Erro ao ler cabeçalho completo");
        close(fd);
        exit(1);
    }
    // extrai os valores recebidos do Header do sender
    int w = header.w;
    int h = header.h;
    int maxv = header.maxv;
    // imagem de entrada e saída com os mesmos parâmetros e tamanhos
    g_in.w = g_out.w = w;
    g_in.h = g_out.h = h;
    g_in.maxv = g_out.maxv = maxv;
    
    g_in.data = malloc(w * h); // alocação da imagem recebida
    g_out.data = malloc(w * h); // alocação da imagem exportada
    
    if (!g_in.data || !g_out.data) {
        fprintf(stderr, "ERRO: Falha na alocação de memória\n");
        exit(1);
    }
    
    size_t total_bytes_to_read = w * h;
    size_t total_bytes_read = 0;
    char* buffer = (char*)g_in.data;

    while (total_bytes_read < total_bytes_to_read) {
        ssize_t bytes_read = read(fd, buffer + total_bytes_read, total_bytes_to_read - total_bytes_read);
        
        if (bytes_read == -1) {
            perror("Erro na leitura");
            close(fd);
            exit(1);
        }
        if (bytes_read == 0) {
            fprintf(stderr, "Conexão fechada antes de terminar\n");
            close(fd);
            exit(1);
        }
        
        total_bytes_read += bytes_read;
    }

    close(fd);
    
    sem_init(&sem_items, 0, 0);
    sem_init(&sem_space, 0, QMAX);
    sem_init(&sem_done, 0, 0);
    
    // cria pool de threads e fila de tarefas
    pthread_t threads[g_nthreads];
    for (int i = 0; i < g_nthreads; i++) {
        pthread_create(&threads[i], NULL, worker_thread, NULL);
    }
    
    int rows_per_task = (h + g_nthreads - 1) / g_nthreads; // a imagem é dividida em blocos de linha, cada bloco vira uma 
    //tarefa que sera processada por uma thread
    remaining_tasks = 0;
    
    for (int i = 0; i < g_nthreads; i++) {
        Task task = {
            .row_start = i * rows_per_task,
            .row_end = (i + 1) * rows_per_task
        };
        
        if (task.row_end > h) {
            task.row_end = h;
        }
        
        sem_wait(&sem_space); // espera espaço livre
        pthread_mutex_lock(&q_lock); // protege o acesso a fila
        
        queue_buf[q_tail] = task;
        q_tail = (q_tail + 1) % QMAX;
        q_count++;
        remaining_tasks++;
        
        pthread_mutex_unlock(&q_lock);
        sem_post(&sem_items); // depois de adcionar uma tarefa a aplicação avisa que há uma nova tarefa
    }
    
    sem_wait(&sem_done); // espera todas as tarefas terminarem
    
    for (int i = 0; i < g_nthreads; i++) {
        sem_post(&sem_items);
    }
    
    for (int i = 0; i < g_nthreads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    if (write_pgm(output_path, &g_out) == 0) {
        printf("Imagem salva: %s\n", output_path);
    } else {
        fprintf(stderr, "ERRO ao salvar imagem\n");
    }
    double t1 = now();
    printf("Tempo de processamento: %.3f segundos\n", t1 - t0);
    
    free(g_in.data);
    free(g_out.data);
    sem_destroy(&sem_items);
    sem_destroy(&sem_space);
    sem_destroy(&sem_done);
    
    return 0;
}