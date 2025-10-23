// wish.c — Minimal Unix-like shell (based on Remzi's "Process Shell" assignment)
// Cumple: modo interactivo/batch, built-ins (exit, cd, path),
// redirección ">" (stdout+stderr), comandos paralelos "&", búsqueda con PATH,
// mensaje de error único, uso de execv/access, fork/wait.
//
// Compilación: gcc -Wall -Wextra -O2 -o wish wish.c
// Uso interactivo: ./wish
// Uso batch:      ./wish batch.txt

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>



// funcion para imprimir el error
static void print_error(void) {
   char error_message[30] = "An error has occurred\n";
   write(STDERR_FILENO, error_message, strlen(error_message));
}

// función para eliminar espacios en blanco al inicio y al final
static char *eliminar_espacios(char *comando) {
    //Retorna null si comando es null
    if (comando == NULL) return NULL;

    //Encuentra donde inicia el comando sin espacios
    while (*comando == ' ' || *comando == '\t' || *comando == '\n') comando++;
    if (*comando == '\0') return comando;

    //Encuentra esta el ultimo caracter del comando antes de '\0'y lo remplaza por '\0'
    char *end = comando + strlen(comando) - 1;
    while (end >= comando && (*end == ' ' || *end == '\t' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    return comando;
}

//Funcion para contar apariciones de un caracter en una cadena
static int contar_caracter(const char *cadena, char caracter) {
    int k = 0;
    for (; cadena && *cadena; cadena++) if (*cadena == caracter) k++;
    return k;
}

// Función para separar comandos por '&' cuando vienen en paralelo ejm p5.sh > /tmp/output22 & p4.sh > /tmp/output22
static char **split_ampersands(char *comandos, int *cantidad_comandos) {
    // creación de lista dinamica, se inicia con capacidad 8 comandos, se duplica si hay mas de 8
    int capacidad_lista = 8, n = 0;
    char **list = malloc(sizeof(char*) * capacidad_lista);
    

    char *start = comandos;
    while (start && *start) {
        // Buscar siguiente '&' 
        char *ampersan = strchr(start, '&');
        if (ampersan) *ampersan = '\0';
        char *piece = eliminar_espacios(start);
        if (*piece) {
            if (n == capacidad_lista) { capacidad_lista *= 2; list = realloc(list, sizeof(char*) * capacidad_lista); if (list == NULL){ print_error(); exit(1);} }
            list[n++] = piece;
        }
        if (!ampersan) break;
        start = ampersan + 1;
    }
    //sirve para que la función te entregue el número de 
    //subcomandos encontrados junto con la lista de punteros
    *cantidad_comandos = n;
    return list;
}

// Función para separar una cadena en tokens separados por espacio, tab o salto de línea
static char **split_whitespace(char *comandos) {
    // creación de lista dinamica, se inicia con capacidad 8 comandos, se duplica si hay mas de 8
    int capacidad_lista = 8, n = 0;
    char **list = malloc(sizeof(char*) * capacidad_lista);

    const char *delimitadores = " \t\n";
    for (char *token = strtok(comandos, delimitadores); token; token = strtok(NULL, delimitadores)) {
        if (n == capacidad_lista) { capacidad_lista *= 2; list = realloc(list, sizeof(char*) * capacidad_lista); if (!list){ print_error(); exit(1);} }
        list[n++] = token;
    }

    //Asegura un hueco para el terminador NULL
    if (n == capacidad_lista) { capacidad_lista += 1; list = realloc(list, sizeof(char*) * capacidad_lista); if (!list){ print_error(); exit(1);} }
    //se termina en null para indicar el final de la lista
    list[n] = NULL;
    return list; }


//PATH del shell donde se buscan los ejecutables
typedef struct {
    // arreglo dinámico de punteros a cada directorio)
    char **dirs;
    int size;
} PathList; //declara una variable de tipo struct

//función inicializa la estructura PathList del shell con un PATH por defecto que contiene solo /bin
static void path_init(PathList *p) {
    p->dirs = malloc(sizeof(char*));
    //Guarda en la primera posición del arreglo una copia  de la cadena "/bin"
    p->dirs[0] = strdup("/bin");
    p->size = 1;
}

//función libera por completo la memoria asociada al PATH
static void path_clear(PathList *p) {
    free(p->dirs);
    p->dirs = NULL;
    p->size = 0;
}

static void path_set(PathList *p, char **args, int argc) {
    path_clear(p);
    //Path vacío, no hay directorios,no se puede ejecutar comandos externos
    if (argc == 0) {return;}

    p->dirs = malloc(sizeof(char*) * argc);
   
    for (int i = 0; i < argc; i++) {
        p->dirs[i] = strdup(args[i]);
        if (!p->dirs[i]) { print_error(); exit(1); }
    }
    p->size = argc;
}

// construir ruta ejecutable usando PATH y access(X_OK)
static char *resolve_executable(PathList *p, const char *comando) {
    
    for (int i = 0; i < p->size; i++) {
        size_t longitud = strlen(p->dirs[i]) + 1 + strlen(comando) + 1;
        char *ruta_candidata = malloc(longitud);
        // Aseguramos exactamente un '/' entre directorio y comando 
        if (p->dirs[i][strlen(p->dirs[i]) - 1] == '/')
            snprintf(ruta_candidata, longitud, "%s%s", p->dirs[i], comando);
        else
            snprintf(ruta_candidata, longitud, "%s/%s", p->dirs[i], comando);
        if (access(ruta_candidata, X_OK) == 0) {
            return ruta_candidata; // encontrado
        }
        free(ruta_candidata);
    }
    return NULL; // no encontrado
}



// función que ejecuta las lineas de comando
static void execute_line(PathList *path, char *line) {
    // Ignorar líneas vacías/espaciadas
    char *clean = eliminar_espacios(line);
    
    // se separan los comandos por '&'
    int ncmds = 0;
    char **cmds = split_ampersands(clean, &ncmds);

    int children = 0;
    
    // Procesar cada comando por separado
    for (int i = 0; i < ncmds; i++) {
        char *command = cmds[i];

        int redir_count = contar_caracter(command, '>');
        if (redir_count > 1) { print_error(); continue; }

        char *cmd_part = command;
        char *outfile = NULL;

        if (redir_count == 1) {
            char *gt = strchr(command, '>');
            *gt = '\0';
            char *right = eliminar_espacios(gt + 1);
            if (*right == '\0') { print_error(); continue; }
            // Asegurarse de que solo haya un nombre de archivo después del '>'
            char *tmp = strdup(right);
            if (!tmp) { print_error(); exit(1); }
            char **outv = split_whitespace(tmp);
            // Si no hay archivo o hay más de uno, error
            if (!outv[0] || outv[1]) {
                free(outv); free(tmp);
                print_error(); continue;
            }
            outfile = strdup(outv[0]);
            free(outv); free(tmp);         
        }
        //Validar parte izquierda del comando (antes del '>')
        cmd_part = eliminar_espacios(cmd_part);
        if (*cmd_part == '\0') { if (outfile) free(outfile); print_error(); continue; }

        char *tmp2 = strdup(cmd_part);
        if (!tmp2) { print_error(); exit(1); }
        char **argv = split_whitespace(tmp2);
        if (!argv[0]) { free(argv); free(tmp2); if (outfile) free(outfile); print_error(); continue; }

        // Built-ins para exit
        if (strcmp(argv[0], "exit") == 0) {
            if (argv[1]) { print_error(); }
            else { free(argv); free(tmp2); if (outfile) free(outfile); free(cmds); exit(0); }
            free(argv); free(tmp2); if (outfile) free(outfile);
            continue;
        }
        // Built-in para cd
        if (strcmp(argv[0], "cd") == 0) {
            if (!argv[1] || argv[2]) { print_error(); }
            else { if (chdir(argv[1]) != 0) { print_error(); } }
            free(argv); free(tmp2); if (outfile) free(outfile);
            continue;
        }
        // Built-in para path
        if (strcmp(argv[0], "path") == 0) {
            int count = 0;
            for (int j = 1; argv[j]; j++) count++;
            path_set(path, &argv[1], count);
            free(argv); free(tmp2); if (outfile) free(outfile);
            continue;
        }

        // Comando externo
        char *exe = resolve_executable(path, argv[0]);
        if (!exe) {
            print_error();
            free(argv); free(tmp2); if (outfile) free(outfile);
            continue;
        }
        
        //se crea un proceso hijo para ejecutar el comando
        pid_t pid = fork();
        if (pid < 0) {
            print_error();
            free(exe); free(argv); free(tmp2); if (outfile) free(outfile);
            continue;
        } else if (pid == 0) {
            if (outfile) {
                int fd = open(outfile, O_CREAT | O_TRUNC | O_WRONLY, 0666);
                if (fd < 0) { print_error(); _exit(1); }
                if (dup2(fd, STDOUT_FILENO) < 0) { print_error(); _exit(1); }
                if (dup2(fd, STDERR_FILENO) < 0) { print_error(); _exit(1); }
                close(fd);
            }
            execv(exe, argv);
            print_error();
            _exit(1);
        // en el padre
        } else {
            children++;
        }

        free(exe);
        free(argv);
        free(tmp2);
        if (outfile) free(outfile);
    }

    // Esperar a todos los hijos lanzados en esta línea
    for (int i = 0; i < children; i++) {
        while (wait(NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
    }

    free(cmds);
}

// ciclo para el shell
int main(int argc, char *argv[]) {
    // Validación de invocación
    if (argc > 2) {
        print_error();
        exit(1);
    }
    
    // Abrir archivo de entrada en modo batch o usar stdin en modo interactivo
    FILE *input = stdin;
    bool interactive = true;

    if (argc == 2) {
        interactive = false;
        input = fopen(argv[1], "r");
        if (!input) {
            print_error();
            exit(1);
        }
    }
    
    // Inicializar PATH
    PathList path;
    path_init(&path);

    char *line = NULL;
    size_t cap = 0;

    while (1) {
        if (interactive) {
            fprintf(stdout, "wish> ");
            fflush(stdout);
        }

        ssize_t n = getline(&line, &cap, input);
        if (n == -1) {
            // EOF o error de lectura: salimos con éxito en interactivo; en batch también
            free(line);
            path_clear(&path);
            exit(0);
        }

        // Procesar línea (puede contener varios comandos en paralelo)
        execute_line(&path, line);
    }

    // (No se llega)
    return 0;
}
