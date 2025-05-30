/*
 * CoShell: 터미널 기반 협업 툴 - 통합 구현(coshell.c)
 *   - ToDo 리스트 관리
 *   - 실시간 채팅 서버/클라이언트
 *   - 파일 전송용 QR 코드 생성 & 화면 출력
 *   - ncurses UI: 분할 창, 버튼
 *
 * 이제 서버 모드를 실행하면 내부적으로 Serveo.net에 SSH 원격 포워딩을 요청하여
 * 할당된 원격 포트(Allocated port)를 자동으로 받아오고, 이를 사용자에게 출력한 뒤
 * 로컬 채팅 서버(LOCAL_PORT)로 바인딩하여 실행합니다.
 *
 * 사용법:
 *   # 인자 없이 실행 시 자동으로 메뉴가 뜹니다.
 *   $ coshell        <-- PATH에 설치되어 있다면 이렇게만 입력해도 됩니다.
 *
 *   # 서버 모드(Serveo 터널 자동 설정)
 *   $ coshell server
 *
 *   # 클라이언트 모드
 *   $ coshell client <host> <port>
 *
 *   # UI 모드 (ToDo + Chat)
 *   $ coshell ui
 *
 *   # CLI 모드: ToDo/QR
 *   $ coshell add <item>
 *   $ coshell list
 *   $ coshell del <index>
 *   $ coshell qr <filepath>
 */

#define _POSIX_C_SOURCE 200809L  // strdup, popen, getaddrinfo 등을 명시적으로 활성화

#include <locale.h>       // (한글 UI 재사용 시 필요) setlocale()
#include <ncurses.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>      // 일부 환경에서 strdup이 없을 때 대비
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>        // getaddrinfo
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#define MAX_CLIENTS 5
#define BUF_SIZE    1024
#define TODO_FILE   "tasks_personal.txt"
#define MAX_TODO    100

// 로컬 채팅 서버가 바인딩할 포트 (Serveo와 연결 시, 로컬 포트)
#define LOCAL_PORT 12345

/* 전역 변수 */
WINDOW *win_chat, *win_todo, *win_input;
pthread_mutex_t todo_lock    = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
int client_socks[MAX_CLIENTS];
int client_count = 0;

/* ToDo 리스트 데이터 */
char *todos[MAX_TODO];
int todo_count = 0;

/* 함수 선언 */
void show_main_menu();
void cli_main(int argc, char *argv[]);
void ui_main();
void load_todo();
void draw_todo();
void add_todo(const char *item);
void show_qr(const char *filename);
void chat_server(int port);
void *client_handler(void *arg);
void chat_client(const char *host, int port);
void *recv_handler(void *arg);

/* =============== Serveo 터널 설정 ============== */
/*
 * Serveo.net에 SSH 원격 포워딩을 요청하고,
 * 할당된 원격 포트(Allocated port)를 파싱해서 반환.
 * 반환값: 원격 포트 번호 (성공 시), -1 (실패 시)
 * 
 * 내부적으로 fork() + pipe()를 사용하여 자식 프로세스로 SSH 실행,
 * 부모는 자식의 출력을 읽으며 "Allocated port <n>" 라인을 찾아냄.
 */
int setup_serveo_tunnel(int local_port) {
    int pipe_fd[2];
    if (pipe(pipe_fd) < 0) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return -1;
    }
    else if (pid == 0) {
        // 자식 프로세스: stdout, stderr를 모두 pipe_fd[1]로 리다이렉트
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[1]);

        // "-R" 인자 문자열을 미리 만들어 줘야 ssh가 올바르게 인식함
        char forward_arg[64];
        snprintf(forward_arg, sizeof(forward_arg), "0:localhost:%d", local_port);

        execlp(
            "ssh", "ssh",
            "-o", "StrictHostKeyChecking=no",
            "-o", "ServerAliveInterval=60",
            "-N",
            "-R", forward_arg,
            "serveo.net",
            (char*)NULL
        );
        // execlp가 실패하면
        perror("execlp");
        _exit(1);
    }
    else {
        // 부모 프로세스: pipe_fd[0]에서 읽기, pipe_fd[1] 닫기
        close(pipe_fd[1]);

        FILE *fp = fdopen(pipe_fd[0], "r");
        if (!fp) {
            perror("fdopen");
            close(pipe_fd[0]);
            return -1;
        }

        char line[512];
        int remote_port = -1;
        // "Allocated port <n> for remote forward to localhost:<local_port>" 패턴을 찾음
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "Allocated port %d", &remote_port) == 1) {
                // 원격 포트 번호 파싱 성공
                printf(">> Serveo 터널 할당 완료: serveo.net:%d\n", remote_port);
                fflush(stdout);
                break;
            }
        }

        // 이 시점부터 SSH 프로세스(pid)는 백그라운드에서 계속 실행되며,
        // Serveo 터널을 유지함.
        fclose(fp);
        return remote_port;
    }
}
/* ================================================ */

int main(int argc, char *argv[]) {
    // (한글 UI를 다시 사용하려면 아래 주석을 해제하세요)
    // setlocale(LC_ALL, "");
    // initscr(); endwin();

    if (argc == 1) {
        /* 인자 없이 실행된 경우 → 메뉴 모드 */
        show_main_menu();
    }
    else if (
        /* CLI 모드(직접 실행) */
        strcmp(argv[1], "add") == 0 ||
        strcmp(argv[1], "list") == 0 ||
        strcmp(argv[1], "del") == 0 ||
        strcmp(argv[1], "qr") == 0
    ) {
        cli_main(argc - 1, &argv[1]);
    }
    else if (strcmp(argv[1], "ui") == 0) {
        /* UI 모드(직접 실행) */
        ui_main();
    }
    else if (strcmp(argv[1], "server") == 0) {
        /* 서버 모드(Serveo 터널 자동 설정) */
        printf(">> Serveo.net을 통해 원격 포트를 요청하는 중...\n");

        int remote_port = setup_serveo_tunnel(LOCAL_PORT);
        if (remote_port < 0) {
            fprintf(stderr, "Serveo 터널 설정 실패. 로컬 서버를 바로 실행합니다.\n");
            chat_server(LOCAL_PORT);
        } else {
            printf(">> Serveo 원격 주소: serveo.net:%d\n", remote_port);
            printf(">> 로컬 채팅 서버를 %d번 포트에 바인딩하여 실행합니다.\n", LOCAL_PORT);
            chat_server(LOCAL_PORT);
        }
    }
    else if (strcmp(argv[1], "client") == 0 && argc == 4) {
        /* 클라이언트 모드(직접 실행) */
        chat_client(argv[2], atoi(argv[3]));
    }
    else {
        fprintf(stderr, "Invalid mode or missing arguments.\n");
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  coshell [add|list|del|qr] ...  # CLI 모드\n");
        fprintf(stderr, "  coshell              # 메뉴 모드\n");
        fprintf(stderr, "  coshell ui           # UI 모드 (ToDo + Chat)\n");
        fprintf(stderr, "  coshell server       # Serveo 터널 자동 설정 + 서버 실행\n");
        fprintf(stderr, "  coshell client <host> <port>   # 채팅 클라이언트\n");
        return 1;
    }

    return 0;
}

/*==============================*/
/*   메인 메뉴 출력 함수       */
/*==============================*/
void show_main_menu() {
    int choice = 0;
    while (1) {
        // ANSI escape로 화면 지우기
        printf("\033[H\033[J");

        printf("\n===== CoShell Main Menu =====\n");
        printf("1. Run Chat Server (Serveo 터널 자동 설정)\n");
        printf("2. Run Client (ToDo + Chat UI)\n");
        printf("3. Exit\n");
        printf("Select (1-3): ");

        if (scanf("%d", &choice) != 1) {
            fprintf(stderr, "Input error. Exiting.\n");
            return;
        }
        getchar();  // 개행 문자 제거

        if (choice == 1) {
            printf("\033[H\033[J");
            printf("Chat server (Serveo 터널 자동 설정) 실행 중...\n");
            // Serveo 터널 자동 설정 + 서버 실행
            int remote_port = setup_serveo_tunnel(LOCAL_PORT);
            if (remote_port < 0) {
                fprintf(stderr, "Serveo 터널 설정 실패. 로컬 서버를 바로 실행합니다.\n");
                chat_server(LOCAL_PORT);
            } else {
                printf(">> Serveo 원격 주소: serveo.net:%d\n", remote_port);
                printf(">> 로컬 채팅 서버를 %d번 포트에 바인딩하여 실행합니다.\n", LOCAL_PORT);
                chat_server(LOCAL_PORT);
            }
            break;
        }
        else if (choice == 2) {
            ui_main();
            break;
        }
        else if (choice == 3) {
            printf("Exiting program.\n");
            break;
        }
        else {
            printf("Invalid selection. Try again.\n");
            sleep(1);
        }
    }
}

// Modify by Geonwo
/*==============================*/
/*           CLI 모드           */
/*==============================*/
void cli_main(int argc, char *argv[]) {
    if (argc == 0) {
        printf("Usage:\n");
        printf("  coshell list\n");
        printf("  coshell add <item>\n");
        printf("  coshell del <index>\n");
        printf("  coshell qr <filepath>\n");
        return;
    }

    // load todo list
    load_todo();

    if (strcmp(argv[0], "list") == 0) {
        for (int i = 0; i < todo_count; i++) {
            printf("%d. %s\n", i + 1, todos[i]);
        }
    }
    else if (strcmp(argv[0], "add") == 0 && argc >= 2) {
        // 공백 포함 전체 메시지 합치기
        char buf[512] = {0};
        for (int i = 1; i < argc; i++) {
            strcat(buf, argv[i]);
            if (i < argc - 1) strcat(buf, " ");
        }

        pthread_mutex_lock(&todo_lock);
        add_todo(buf);
        pthread_mutex_unlock(&todo_lock);

        printf("Added: %s\n", buf);
    }
    else if (strcmp(argv[0], "del") == 0 && argc == 2) {
        int idx = atoi(argv[1]) - 1;
        if (idx < 0 || idx >= todo_count) {
            printf("Invalid index.\n");
            return;
        }

        pthread_mutex_lock(&todo_lock);
        // 삭제
        free(todos[idx]);
        for (int i = idx; i < todo_count - 1; i++) {
            todos[i] = todos[i + 1];
        }
        todo_count--;

        // 파일 다시 저장
        FILE *fp = fopen(TODO_FILE, "w");
        if (fp) {
            for (int i = 0; i < todo_count; i++) {
                fprintf(fp, "%s\n", todos[i]);
            }
            fclose(fp);
        }
        pthread_mutex_unlock(&todo_lock);

        printf("Deleted todo #%d\n", idx + 1);
    }
    else if (strcmp(argv[0], "qr") == 0 && argc == 2) {
        show_qr(argv[1]);  // QR은 chat 창 없이 터미널에 직접 출력됨
    }
    else {
        fprintf(stderr, "Unknown or invalid CLI command.\n");
    }
}


/*==============================*/
/*   UI 모드 (ToDo + 채팅)      */
/*==============================*/
void ui_main() {
    initscr();            // ncurses 시작
    cbreak();             // 라인 버퍼 없이 키 입력받기
    noecho();             // 키 입력 시 화면에 표시 안 함
    keypad(stdscr, TRUE); // 화살표·기능키 사용 허용

    int h, w;
    getmaxyx(stdscr, h, w);

    // 채팅 창: 왼쪽 (높이 h-3, 너비 w/2)
    win_chat = newwin(h - 3, w / 2, 0, 0);
    // ToDo 창: 오른쪽 (높이 h-3, 너비 w - w/2)
    win_todo = newwin(h - 3, w - w / 2, 0, w / 2);
    // 입력 창: 아래 (높이 3, 너비 w)
    win_input = newwin(3, w, h - 3, 0);

    scrollok(win_chat, TRUE); // 채팅 윈도우 스크롤 허용

    load_todo();
    draw_todo();

    while (1) {
        // 입력 프롬프트
        werase(win_input);
        mvwprintw(win_input, 1, 1, "Command: (a)Add (q)QR (c)Chat (x)Exit > ");
        wrefresh(win_input);

        int ch = wgetch(win_input);
        if (ch == 'x') {
            break;
        }
        else if (ch == 'a') {
            // [a] ToDo 항목 추가
            werase(win_input);
            mvwprintw(win_input, 1, 1, "Add ToDo: ");
            wrefresh(win_input);

            echo();
            char buf[256];
            mvwgetnstr(win_input, 1, 12, buf, 200);
            noecho();

            pthread_mutex_lock(&todo_lock);
            add_todo(buf);
            draw_todo();
            pthread_mutex_unlock(&todo_lock);
        }
        else if (ch == 'q') {
            // [q] QR 코드 생성
            werase(win_input);
            mvwprintw(win_input, 1, 1, "Enter file path for QR: ");
            wrefresh(win_input);

            echo();
            char buf[256];
            mvwgetnstr(win_input, 1, 23, buf, 200);
            noecho();

            show_qr(buf);
        }
        else if (ch == 'c') {
            // [c] 채팅 클라이언트 실행
            werase(win_input);

            const char *prompt = "Chat server host (e.g., localhost or 127.0.0.1): ";
            mvwprintw(win_input, 1, 1, "%s", prompt);
            wrefresh(win_input);

            echo();
            char buf[128];
            mvwgetnstr(win_input, 1, strlen(prompt) + 1, buf, 100);
            noecho();

            char host[128];
            strncpy(host, buf, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';

            werase(win_input);

            const char *prompt2 = "Port: ";
            mvwprintw(win_input, 1, 1, "%s", prompt2);
            wrefresh(win_input);

            echo();
            mvwgetnstr(win_input, 1, strlen(prompt2) + 1, buf, 10);
            noecho();

            int port = atoi(buf);

            // ncurses 종료 후, 채팅 클라이언트 모드 진입
            endwin();
            chat_client(host, port);
            return;
        }
        // 그 외 입력은 무시하고 다시 루프
    }

    endwin();  // ncurses 종료
}

/*==============================*/
/*   ToDo 리스트 로드/표시       */
/*==============================*/
void load_todo() {
    FILE *fp = fopen(TODO_FILE, "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp) && todo_count < MAX_TODO) {
        line[strcspn(line, "\n")] = '\0';
        todos[todo_count++] = strdup(line);
    }
    fclose(fp);
}

void draw_todo() {
    werase(win_todo);
    box(win_todo, 0, 0);
    mvwprintw(win_todo, 0, 2, " ToDo List ");
    for (int i = 0; i < todo_count; i++) {
        mvwprintw(win_todo, i + 1, 2, "%d. %s", i + 1, todos[i]);
    }
    wrefresh(win_todo);
}

void add_todo(const char *item) {
    if (todo_count >= MAX_TODO) return;
    FILE *fp = fopen(TODO_FILE, "a");
    if (!fp) return;

    fprintf(fp, "%s\n", item);
    fclose(fp);
    todos[todo_count++] = strdup(item);
}

/*==============================*/
/*   QR 코드 생성 및 출력       */
/*==============================*/
void show_qr(const char *filename) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "qrencode -t ASCII -o - '%s'", filename);

    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    werase(win_chat);
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        wprintw(win_chat, "%s", line);
    }
    pclose(fp);
    wrefresh(win_chat);
}

/*==============================*/
/*       채팅 서버 구현         */
/*==============================*/
void chat_server(int port) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return;
    }

    struct sockaddr_in addr = { 0 };
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_sock);
        return;
    }
    if (listen(server_sock, MAX_CLIENTS) < 0) {
        perror("listen");
        close(server_sock);
        return;
    }

    printf("Chat server listening on port %d...\n", port);

    while (1) {
        int client = accept(server_sock, NULL, NULL);
        if (client < 0) {
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&clients_lock);
        if (client_count < MAX_CLIENTS) {
            client_socks[client_count++] = client;

            // 소켓 디스크립터를 스레드에 안전하게 전달하기 위해 malloc 사용
            int *client_ptr = malloc(sizeof(int));
            if (!client_ptr) {
                perror("malloc");
                close(client);
                pthread_mutex_unlock(&clients_lock);
                continue;
            }
            *client_ptr = client;

            pthread_t tid;
            if (pthread_create(&tid, NULL, client_handler, client_ptr) != 0) {
                perror("pthread_create");
                free(client_ptr);
                close(client);
                pthread_mutex_unlock(&clients_lock);
                continue;
            }
            pthread_detach(tid);
        }
        else {
            close(client);
        }
        pthread_mutex_unlock(&clients_lock);
    }
}

/*==============================*/
/*     클라이언트 핸들러        */
/*==============================*/
void *client_handler(void *arg) {
    int sock = *(int*)arg;
    free(arg);

    char buf[BUF_SIZE];
    while (1) {
        int len = recv(sock, buf, sizeof(buf) - 1, 0);
        if (len <= 0) break;
        buf[len] = '\0';

        pthread_mutex_lock(&clients_lock);
        for (int i = 0; i < client_count; i++) {
            if (client_socks[i] != sock) {
                send(client_socks[i], buf, len, 0);
            }
        }
        pthread_mutex_unlock(&clients_lock);
    }

    close(sock);
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < client_count; i++) {
        if (client_socks[i] == sock) {
            client_socks[i] = client_socks[--client_count];
            break;
        }
    }
    pthread_mutex_unlock(&clients_lock);
    return NULL;
}

/*==============================*/
/*       채팅 클라이언트        */
/*==============================*/
void chat_client(const char *host, int port) {
    // DNS 해석을 위해 getaddrinfo 사용
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        fprintf(stderr, "Failed to resolve host: %s\n", host);
        return;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        perror("socket");
        freeaddrinfo(res);
        return;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect");
        close(sock);
        freeaddrinfo(res);
        return;
    }

    freeaddrinfo(res);

    // 서버 메시지 수신 스레드
    int *sock_ptr = malloc(sizeof(int));
    if (!sock_ptr) {
        perror("malloc");
        close(sock);
        return;
    }
    *sock_ptr = sock;

    pthread_t recv_tid;
    if (pthread_create(&recv_tid, NULL, recv_handler, sock_ptr) != 0) {
        perror("pthread_create");
        free(sock_ptr);
        close(sock);
        return;
    }

    // stdin → 서버로 보냄
    char msg[BUF_SIZE];
    while (fgets(msg, sizeof(msg), stdin)) {
        if (send(sock, msg, strlen(msg), 0) <= 0) break;
    }

    close(sock);
    printf("Chat client exiting.\n");
}

/*==============================*/
/*    서버로부터 수신 핸들러     */
/*==============================*/
void *recv_handler(void *arg) {
    int sock = *(int*)arg;
    free(arg);

    char buf[BUF_SIZE];
    while (1) {
        int len = recv(sock, buf, sizeof(buf) - 1, 0);
        if (len <= 0) {
            printf("\nConnection closed by server.\n");
            break;
        }
        buf[len] = '\0';
        printf("%s", buf);
        fflush(stdout);
    }
    return NULL;
}
