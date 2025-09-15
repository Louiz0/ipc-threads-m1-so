#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

typedef struct {
    int w, h, maxv; // maxv é 255
    unsigned char* data; // w * h bytes -- grayscale
} PGM;

typedef struct {
    int w, h, maxv; // metadados da imagem
    int mode; // 0 = negativo e 1 slice
    int t1, t2; // parametros do slice, t1 e t2
} Header;

int read_pgm(const char* path, PGM* img) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        perror("Erro ao abrir arquivo, verificar PATH e nome do arquivo");
        return -1;
    }
    
    char typePGM[3];
    if (fscanf(file, "%2s", typePGM) != 1) {
        fclose(file);
        return -1;
    }
    
    printf("typePGM: %s\n", typePGM);
    
    if (typePGM[0] != 'P' || typePGM[1] != '5') {
        fprintf(stderr, "Formato inválido. Esperado P5, encontrado: %s\n", typePGM);
        fclose(file);
        return -1;
    }
    
    // pular comentarios e espaco em branco
    int c;
    while ((c = fgetc(file)) != EOF) {
        if (c == '#') {
            // pula comentario do gimp
            while ((c = fgetc(file)) != EOF && c != '\n');
        } else if (c == ' ' || c == '\t' || c == '\n') {
            // continua pulando espaco em branco
            continue;
        } else {
            // encontrou um caractere que não é comentário/whitespace
            ungetc(c, file);
            break;
        }
    }
    
    if (fscanf(file, "%d %d", &img->w, &img->h) != 2) {
        fprintf(stderr, "Erro ao ler dimensões\n");
        fclose(file);
        return -1;
    }
    
    printf("Dimensões: %dx%d\n", img->w, img->h);
    
    // pula espaco em branco até o maxgv
    while ((c = fgetc(file)) != EOF && (c == ' ' || c == '\t' || c == '\n'));
    ungetc(c, file);
    
    if (fscanf(file, "%d", &img->maxv) != 1) {
        fprintf(stderr, "Erro ao ler maxv\n");
        fclose(file);
        return -1;
    }
    
    printf("Maxv: %d\n", img->maxv);
    
    // pular newline antes do binario
    while ((c = fgetc(file)) != EOF && (c == ' ' || c == '\t' || c == '\n'));
    ungetc(c, file);
    
    img->data = malloc(img->w * img->h);
    if (!img->data) {
        fprintf(stderr, "Erro ao alocar memória\n");
        fclose(file);
        return -1;
    }
    
    size_t bytes_read = fread(img->data, 1, img->w * img->h, file);
    fclose(file);
    
    if (bytes_read != img->w * img->h) {
        fprintf(stderr, "Erro: leu %zd bytes, esperava %d\n", bytes_read, img->w * img->h);
        free(img->data);
        return -1;
    }
    
    printf("Imagem lida com sucesso: %dx%d, maxv=%d, bytes=%zd\n", img->w, img->h, img->maxv, bytes_read);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <fifo_path> <entrada.pgm>\n", argv[0]);
        exit(1);
    }
    
    const char* fifo_path = argv[1];
    const char* input_path = argv[2];
    
    printf("Sender iniciado. FIFO: %s, Imagem: %s\n", fifo_path, input_path);
    
    PGM img;
    if (read_pgm(input_path, &img) != 0) {
        fprintf(stderr, "Erro ao ler imagem\n");
        exit(1);
    }
    
    if (img.w == 0 || img.h == 0) {
        fprintf(stderr, "Erro dimensões inválidas da imagem!\n");
        exit(1);
    }
    
    printf("Imagem carregada: %dx%d, maxv=%d\n", img.w, img.h, img.maxv);
    
    // preparar cabeçalho
    Header header = {
        .w = img.w,
        .h = img.h,
        .maxv = img.maxv,
        .mode = 0,
        .t1 = 0,
        .t2 = 0
    };
    
    // abrir FIFO para ESCRITA
    int fd = open(fifo_path, O_WRONLY);
    if (fd == -1) {
        perror("Não conseguiu abrir o FIFO para escrita");
        exit(1);
    }
    printf("FIFO aberto para escrita. Enviando cabeçalho...\n");
    
    // enviar cabeçalho
    ssize_t bytes_written = write(fd, &header, sizeof(Header));
    printf("Bytes escritos do cabeçalho: %zd\n", bytes_written);
    
    // enviar dados da imagem aos poucos, imagens grandes não envia sem tempo de espera
    size_t total_bytes_to_write = img.w * img.h;
    size_t total_bytes_written = 0;
    const char* buffer = (const char*)img.data;

    printf("Enviando imagem grande: %zd bytes...\n", total_bytes_to_write);

    while (total_bytes_written < total_bytes_to_write) {
        bytes_written = write(fd, buffer + total_bytes_written, total_bytes_to_write - total_bytes_written);
        
        if (bytes_written == -1) {
            perror("Erro na escrita");
            close(fd);
            exit(1);
        }
        
        total_bytes_written += bytes_written;
        printf("Progresso: %zd/%zd bytes (%.1f%%)\r",
               total_bytes_written, total_bytes_to_write,
               (total_bytes_written * 100.0) / total_bytes_to_write);
        fflush(stdout);
    }

    printf("\nImagem enviada completamente: %zd bytes\n", total_bytes_written);
    
    close(fd);
    free(img.data);
    printf("Sender finalizado.\n");
    return 0;
}