/*
 * CoShell: 터미널 기반 협업 툴 - 통합 구현(coshell.c)
 * - ToDo 리스트 관리
 * - 실시간 채팅 서버/클라이언트
 * - 파일 전송용 QR 코드 생성 & 화면 출력
 * - ncurses UI: 분할 창, 버튼
 *
 * 사용법:
 *   # 인자 없이 실행 시 자동으로 메뉴가 뜹니다.
 *   ./coshell
 *
 *   # 기존 인자 방식 유지
 *   gcc -Wall -O2 -std=c11 -lncurses -lpthread -o coshell coshell.c
 *   # UI 모드 (ToDo + 채팅 UI)
 *   ./coshell ui
 *   # 채팅 서버 실행 (인자 방식)
 *   ./coshell server <port>
 *   # 채팅 클라이언트 실행 (인자 방식)
 *   ./coshell client <host> <port>
 */

#include <ncurses.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>

#define MAX_CLIENTS 5
#define BUF_SIZE 1024
#define TODO_FILE "tasks_personal.txt"

/* 전역 변수 */
WINDOW *win_chat, *win_todo, *win_input;
pthread_mutex_t todo_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
int client_socks[MAX_CLIENTS];
int client_count = 0;

/* ToDo 리스트 데이터 */
#define MAX_TODO 100
char *todos[MAX_TODO];
int todo_count = 0;

/* 함수 선언 */
void show_main_menu();
void ui_main();
void load_todo();
void draw_todo();
void add_todo(const char *item);
void show_qr(const char *filename);
void chat_server(int port);
void *client_handler(void *arg);
void chat_client(const char *host, int port);
void *recv_handler(void *arg);

int main(int argc, char *argv[]) {
    if (argc == 1) {
        /* 인자 없이 실행된 경우, 메뉴 모드로 진입 */
        show_main_menu();
    }
    else if (strcmp(argv[1], "ui") == 0) {
        /* 기존 UI 모드 직접 실행 */
        ui_main();
    }
    else if (strcmp(argv[1], "server") == 0 && argc == 3) {
        /* 기존 인자 방식으로 서버 실행 */
        chat_server(atoi(argv[2]));
    }
    else if (strcmp(argv[1], "client") == 0 && argc == 4) {
        /* 기존 인자 방식으로 클라이언트 실행 */
        chat_client(argv[2], atoi(argv[3]));
    }
    else {
        fprintf(stderr, "Invalid mode or missing arguments.\n");
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  ./coshell            # 메뉴 모드\n");
        fprintf(stderr, "  ./coshell ui         # UI 모드 (ToDo + 채팅)\n");
        fprintf(stderr, "  ./coshell server <port>\n");
        fprintf(stderr, "  ./coshell client <host> <port>\n");
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
        /* 화면 클리어 */
        if (system("clear") < 0) { /* Windows 환경이면 "cls"로 변경 필요 */ }
        printf("\n===== CoShell Main Menu =====\n");
        printf("1. 서버 실행 (Chat Server)\n");
        printf("2. 클라이언트(UI) 실행 (ToDo + 채팅)\n");
        printf("3. 종료\n");
        printf("선택(번호) 입력: ");

        if (scanf("%d", &choice) != 1) {
            /* 잘못된 입력 처리 */
            fprintf(stderr, "입력 오류. 종료합니다.\n");
            return;
        }
        getchar(); /* 버퍼 비우기 */

        if (choice == 1) {
            int port;
            printf("채팅 서버 포트 입력: ");
            if (scanf("%d", &port) != 1) {
                fprintf(stderr, "포트 입력 오류. 메뉴로 돌아갑니다.\n");
                getchar();
                continue;
            }
            getchar(); /* 버퍼 비우기 */
            /* 화면 클리어 후 서버 실행 */
            if (system("clear") < 0) { }
            printf("Chat server listening on port %d...\n", port);
            chat_server(port);
            break; /* 서버는 무한 루프 대신 종료시키려면 ctrl+C로 끄시면 됩니다. */
        }
        else if (choice == 2) {
            /* 기존 UI 모드를 호출(ToDo + 채팅) */
            ui_main();
            break;
        }
        else if (choice == 3) {
            printf("프로그램을 종료합니다.\n");
            break;
        }
        else {
            printf("잘못된 선택입니다. 다시 입력해주세요.\n");
            sleep(1);
        }
    }
}

/*==============================*/
/*   UI 모드 (ToDo + 채팅)      */
/*==============================*/
void ui_main() {
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE);
    int h, w; getmaxyx(stdscr, h, w);
    win_chat = newwin(h-3, w/2, 0, 0);
    win_todo = newwin(h-3, w - w/2, 0, w/2);
    win_input = newwin(3, w, h-3, 0);
    scrollok(win_chat, TRUE);

    load_todo(); draw_todo();

    mvwprintw(win_input, 1, 1, "명령: (a)추가 (q)QR (c)채팅 (x)종료 > ");
    wrefresh(win_chat); wrefresh(win_todo); wrefresh(win_input);

    int ch;
    char buf[256];
    while ((ch = wgetch(win_input)) != 'x') {
        werase(win_input);
        mvwprintw(win_input, 1, 1, "명령: (a)추가 (q)QR (c)채팅 (x)종료 > ");
        wrefresh(win_input);

        if (ch == 'a') {
            /* ToDo 항목 추가 */
            echo();
            mvwgetnstr(win_input, 1, 30, buf, 200);
            noecho();
            pthread_mutex_lock(&todo_lock);
            add_todo(buf);
            draw_todo();
            pthread_mutex_unlock(&todo_lock);
        }
        else if (ch == 'q') {
            /* QR 코드 생성 및 출력 */
            werase(win_input);
            mvwprintw(win_input, 1, 1, "QR 만들 파일 경로: ");
            echo();
            mvwgetnstr(win_input, 1, 20, buf, 200);
            noecho();
            show_qr(buf);
        }
        else if (ch == 'c') {
            /* 채팅 클라이언트 실행 */
            werase(win_input);
            mvwprintw(win_input, 1, 1, "채팅 서버 호스트: ");
            echo();
            mvwgetnstr(win_input, 1, 20, buf, 100);
            noecho();
            char host[128];
            strcpy(host, buf);

            werase(win_input);
            mvwprintw(win_input, 1, 1, "포트: ");
            echo();
            mvwgetnstr(win_input, 1, 10, buf, 10);
            noecho();
            int port = atoi(buf);

            /* UI 종료 후 채팅 모드로 진입 */
            endwin();
            chat_client(host, port);
            return;
        }
        /* 그 외 키는 무시 */
    }
    endwin();
}

/*==============================*/
/*   ToDo 리스트 로드/표시       */
/*==============================*/
void load_todo() {
    FILE *fp = fopen(TODO_FILE, "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp) && todo_count < MAX_TODO) {
        line[strcspn(line, "\n")] = 0;
        todos[todo_count++] = strdup(line);
    }
    fclose(fp);
}

void draw_todo() {
    werase(win_todo);
    box(win_todo, 0, 0);
    mvwprintw(win_todo, 0, 2, " ToDo List ");
    for (int i = 0; i < todo_count; i++) {
        mvwprintw(win_todo, i+1, 2, "%d. %s", i+1, todos[i]);
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
    snprintf(cmd, sizeof(cmd), "qrencode -t ANSIUTF8 -o - '%s'", filename);
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

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

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
            pthread_t tid;
            pthread_create(&tid, NULL, client_handler, &client);
            pthread_detach(tid);
        } else {
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
    char buf[BUF_SIZE];

    while (1) {
        int len = recv(sock, buf, sizeof(buf)-1, 0);
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
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "호스트 IP 변환 실패: %s\n", host);
        close(sock);
        return;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return;
    }

    /* 별도의 스레드에서 서버로부터 오는 메시지를 출력 */
    pthread_t recv_tid;
    pthread_create(&recv_tid, NULL, recv_handler, &sock);

    char msg[BUF_SIZE];
    while (fgets(msg, sizeof(msg), stdin)) {
        send(sock, msg, strlen(msg), 0);
    }
    close(sock);
}

/*==============================*/
/*    서버로부터 수신 핸들러     */
/*==============================*/
void *recv_handler(void *arg) {
    int sock = *(int*)arg;
    char buf[BUF_SIZE];
    while (1) {
        int len = recv(sock, buf, sizeof(buf)-1, 0);
        if (len <= 0) break;
        buf[len] = '\0';
        printf("%s", buf);
        fflush(stdout);
    }
    return NULL;
}
