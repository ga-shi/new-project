#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <dlfcn.h>

//#include "common/spi.h"

#define NOPRINT 1

//#include "tracer.c"

#define MAX_EVENTS 64
#define MAX_WORKERS 4
#define MAX_ACCEPT 2
#define MAX_SESSIONS 256
#define BUFFER_SIZE 4096
#define MAX_FD 1024
#define MAX_REMOTE_PORT 4
#define MAX_HOST 2
#define MAX_HISTORY 1000

typedef struct {
    int client_fd;
    int server_fd;
    int start_time_sec;
    long start_time_nsec;
    int server_port;
    unsigned int server_host;
} connection_t;

typedef struct {
    int epoll_fd;
    int worker_id;
    ssize_t (*modify_data)(char *buf, ssize_t len);
    pthread_mutex_t modify_data_mutex;
    connection_t *fd_connection_map[MAX_FD];
    int w_flag;
} worker_t;

typedef struct {
    connection_t *fd_connection_map[MAX_FD];
    pthread_mutex_t data_mutex;
    int lis_fd;
    int lis_port;
} accept_t;

accept_t accs[MAX_ACCEPT];
worker_t workersA[MAX_WORKERS];
worker_t workersB[MAX_WORKERS];
pthread_t worker_threadsA[MAX_WORKERS];
pthread_t worker_threadsB[MAX_WORKERS];
pthread_t acc_threads[MAX_ACCEPT];
pthread_t print_threads;

double all_durationA1[MAX_REMOTE_PORT];
double all_durationA2[MAX_REMOTE_PORT];
double all_durationB1[MAX_REMOTE_PORT];
double all_durationB2[MAX_REMOTE_PORT];
double ave_durationA1[MAX_REMOTE_PORT];
double ave_durationA2[MAX_REMOTE_PORT];
double ave_durationB1[MAX_REMOTE_PORT];
double ave_durationB2[MAX_REMOTE_PORT];
int division_countA1[MAX_REMOTE_PORT];
int division_countA2[MAX_REMOTE_PORT];
int division_countB1[MAX_REMOTE_PORT];
int division_countB2[MAX_REMOTE_PORT];
double division_listA1_1[MAX_HISTORY];
double division_listA1_2[MAX_HISTORY];
double division_listA1_3[MAX_HISTORY];
double division_listA1_4[MAX_HISTORY];
double division_listA2_1[MAX_HISTORY];
double division_listA2_2[MAX_HISTORY];
double division_listA2_3[MAX_HISTORY];
double division_listA2_4[MAX_HISTORY];
double division_listB1_1[MAX_HISTORY];
double division_listB1_2[MAX_HISTORY];
double division_listB1_3[MAX_HISTORY];
double division_listB1_4[MAX_HISTORY];
double division_listB2_1[MAX_HISTORY];
double division_listB2_2[MAX_HISTORY];
double division_listB2_3[MAX_HISTORY];
double division_listB2_4[MAX_HISTORY];


#define TOTAL_SESSION (1000)
static unsigned short trace_session[TOTAL_SESSION] = {0},live_trace_session[TOTAL_SESSION] = {0};

static uint64_t cnt = 0;

int is_tracer_session(unsigned short sid) {
  return live_trace_session[sid];
}
void close_tracer_session(unsigned short sid) {
  live_trace_session[sid] = 0;
}
uint32_t get_usec(uint64_t *time){
  *time = cnt;
  cnt++;
  return 0;
}

int CID_count = 0;

//ヘッダーにCIDを追加
ssize_t add_CID(char *buf, ssize_t len) {
    char ad[BUFFER_SIZE];
    //末尾に足すものを生成
    sprintf(ad, "CID:%d\r\n\r\n", CID_count);

    CID_count += 1;

    //bufの末尾にadを追加
    strcat(buf, ad);

    //連結したbufの文字列の長さを取得
    ssize_t length = (ssize_t)strlen(buf);
    return length;
}

ssize_t default_modify_data(char *buf, ssize_t len) {
        // デフォルトではデータをそのまま返す
    /*
    int ret;
    struct toe_512_header header;
    header_t p_header = & header;
    struct trace_info trace_info;
    trace_info.request_trace = 0;
    trace_info.tracer_id = 1;
*/
   /* printf("============= New req. size %d ================\n", (int)len);
    for(int i=0;i<len;i++){
      printf("%c",buf[i]);
    }
    printf("<---------------------\n");
*/
  /*  p_header->appNotification_length = len;

    ret = spi_handle(p_header, buf, &trace_info);
    len = (ssize_t) p_header->appNotification_length;

    printf("eeeeeeeeeeeeeeee resp. size %d eeeeeeeeeeeeeeee\n", (int)len);
    for(int i=0;i<len;i++){
      printf("%c",buf[i]);
    }
    printf("<---------------------\n");
*/
    return len;
}

int create_listen_socket(int port) {
    int listen_fd;
    struct sockaddr_in addr;
    int opt = 1;

    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // すべてのインターフェースでリッスン
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

int create_server_socket(char *host, int port) {
    int server_fd;
    struct sockaddr_in addr;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }

    // TCP_NODELAYを設定
    int flag = 1;
    if (setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flag, sizeof(int)) < 0) {
        perror("setsockopt TCP_NODELAY on server_fd");
    }

    // ノンブロッキングに設定
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(server_fd);
        return -1;
    }

    if (connect(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
       // printf("aaa");
       // printf("%d\n", errno);
        if (errno != EINPROGRESS) {
            perror("connect");
            close(server_fd);
            return -1;
        }
    }

    return server_fd;
}

//bytesReadは実際に読み込まれたバイト数
int contains_test1_in_uri(char *buffer, ssize_t bytesRead) {
    char requestLine[BUFFER_SIZE];
    int i = 0;

    //printf("buffer = %s\n", buffer);
    //最初の行(リクエストライン)を探す
    while(i < bytesRead && i < BUFFER_SIZE - 1) {
        if (buffer[i] == '\r' && buffer[i+1] == '\n') {
            //改行が見つかったら、その前までをリクエストラインとする
            requestLine[i] = '\0'; //NULL終端を追加
            break;
        }
        requestLine[i] = buffer[i];
        i++;
    }

    // リクエストラインの中に URI が含まれているか調べる
    // リクエストラインは形式「<メソッド> <URI> <HTTPバージョン」
    // メソッドとHTTPバージョンをスキップして URI を抽出する
    char *uri_start = strchr(requestLine, ' ');
    //printf("request = %s\n", requestLine);
    // //printf("start1 =%s\n", uri_start);
    if (uri_start != NULL) {
        uri_start++;  // メソッドの次（最初の空白）の位置からURIが始まる
        char *uri_end = strchr(uri_start, ' ');

        //printf("end1 = %s\n", uri_end);
        if (uri_end != NULL) {
            // URIの終わりを見つけたら、そこまでがURI
            *uri_end = '\0';  // NULL終端を追加
        }
    //     //printf("end2 = %s\n", uri_end);
    //     //printf("start2 =%s\n", uri_start);

        // URI部分に"test1"が含まれているかチェック
        if (strstr(requestLine, "test1") != NULL) {
            return 1;  // "aaa"が含まれている
        }
    }

    return 0; //aaaが含まれていない
}

int contains_test2_in_uri(char *buffer, ssize_t bytesRead) {
    char requestLine[BUFFER_SIZE];
    int i = 0;

    //最初の行(リクエストライン)を探す
    while(i < bytesRead && i < BUFFER_SIZE - 1) {
        if (buffer[i] == '\r' && buffer[i+1] == '\n') {
            //改行が見つかったら、その前までをリクエストラインとする
            requestLine[i] = '\0'; //NULL終端を追加
            break;
        }
        requestLine[i] = buffer[i];
        i++;
    }

    // リクエストラインの中に URI が含まれているか調べる
    // リクエストラインは形式「<メソッド> <URI> <HTTPバージョン」
    // メソッドとHTTPバージョンをスキップして URI を抽出する
    char *uri_start = strchr(requestLine, ' ');
    if (uri_start != NULL) {
        uri_start++;  // メソッドの次（最初の空白）の位置からURIが始まる
        char *uri_end = strchr(uri_start, ' ');

        if (uri_end != NULL) {
            // URIの終わりを見つけたら、そこまでがURI
            *uri_end = '\0';  // NULL終端を追加
        }

        // URI部分に"test2"が含まれているかチェック
        if (strstr(requestLine, "test2") != NULL) {
            return 1;  // "aaa"が含まれている
        }
    }

    return 0; //aaaが含まれていない
}


void close_connection(worker_t *worker, connection_t *conn) {
    if (conn == NULL)
        return;
    epoll_ctl(worker->epoll_fd, EPOLL_CTL_DEL, conn->client_fd, NULL);
    epoll_ctl(worker->epoll_fd, EPOLL_CTL_DEL, conn->server_fd, NULL);
    close(conn->client_fd);
    close(conn->server_fd);
    if (conn->client_fd < MAX_FD)
        worker->fd_connection_map[conn->client_fd] = NULL;
    if (conn->server_fd < MAX_FD)
        worker->fd_connection_map[conn->server_fd] = NULL;
    free(conn);
}

int find_min_index(double arr[], int size) {
    if (size <= 0) {
        //配列が空でないことを確認
        return -1;
    }
    int min_index = 0;
    for (int i = 1; i < size; i++) {
        if (arr[i] < arr[min_index]) {
            min_index = i;
        }
    }
    return min_index;

}

ssize_t read_all(int fd, char *buf, ssize_t size) {
    ssize_t total_read = 0;
    ssize_t bytes_read;

    do {
        //printf("read_roop\n");
        bytes_read = read(fd, buf + total_read, size - total_read);
        if (bytes_read != -1) {
        total_read += bytes_read;
        }
        //printf("%ld\n", bytes_read);
    } while(bytes_read > 0);
        //printf("A");
        if (bytes_read == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
            //printf("%ld\n", bytes_read);
            perror("read");
            return -1;
        }
        else if (bytes_read == 0 && total_read == 0) {
            //printf("%ld\n", bytes_read);
            //printf("close");
            return 0;
        }
        else{
            //printf("%ld\n", bytes_read);
            return total_read;
        }

}

ssize_t write_all(int fd, char *buf, ssize_t size) {
    ssize_t total_written = 0;
    ssize_t bytes_written;

    while(total_written != size) {
        bytes_written = write(fd, buf + total_written, size - total_written);
        total_written += bytes_written;
        //printf("write_loop\n");
    }
        if (bytes_written == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("write");
            return -1;
        }
        else if (bytes_written == 0 && total_written == 0) {
            return 0;
        }
        else {
            //正常にデータが書き込まれた場合
            return total_written;
        }
}

void *ave_print(void *arg) {
    int i, j, k, l, m;
    while(1) {
        // printf("division_listA1_3 = ");
        // for (m = 0; m < 10; m++) {
        // printf(" %f,", division_listA1_4[m]);
        // }
        for (i = 0; i < MAX_REMOTE_PORT; i++) {
            printf("ave_durationA1[%d] = %fus, count = %d\n", i, ave_durationA1[i], division_countA1[i]);
        }
        for (j = 0; j < MAX_REMOTE_PORT; j++) {
            printf("ave_durationA2[%d] = %fus, count = %d\n", j, ave_durationA2[j], division_countA2[j]);
        }
        for (k = 0; k < MAX_REMOTE_PORT; k++) {
            printf("ave_durationB1[%d] = %fus, count = %d\n", k, ave_durationB1[k], division_countB1[k]);
        }
        for (l = 0; l < MAX_REMOTE_PORT; l++) {
            printf("ave_durationB2[%d] = %fus, count = %d\n", l, ave_durationB2[l], division_countB2[l]);
        }

        sleep(10);
    }
    return NULL;
}

void *worker_func(void *arg) {
    worker_t *worker = (worker_t *)arg;
    struct epoll_event events[MAX_EVENTS];
    int nfds, i,j;
    unsigned int host1 = inet_addr("192.168.62.61");
    unsigned int host2 = inet_addr("192.168.62.90");


    while (1) {
        //printf("wating\n");
        nfds = epoll_wait(worker->epoll_fd, events, MAX_EVENTS, -1);
        //printf("changed\n");
        if (nfds == -1) {
            perror("epoll_wait");
            continue;
        }
        //時間を保存しておいて、後ほど、connがserver_fdだった時に活用。
        struct timespec end_time;
        if (worker->w_flag > 0){
            if(clock_gettime(CLOCK_REALTIME, &end_time) != 0) {
                perror("clock_gettime");
                continue;
            }
        }
        //printf("endtime_flag =%d\n", worker->w_flag);

        for (i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            connection_t *conn = NULL;
            if (fd < MAX_FD)
                conn = worker->fd_connection_map[fd];
            if (conn == NULL) {
                continue;
            }

            if (events[i].events & EPOLLIN) {
                if (fd == conn->client_fd) {
                    char buffer[BUFFER_SIZE];
                    char *p_buffer = buffer;
                    struct timespec ts;
                    
                    //printf("reading_cli\n");
                    ssize_t len = read_all(fd, p_buffer, sizeof(buffer));
                    //ssize_t len = read(fd, buffer, sizeof(buffer));
                    //printf("con_worker%d_buf = %ld\n", worker->worker_id, len);
                    //printf("%ld\n", len);

                    if (len == 0 || len == -1) {
                        close_connection(worker, conn);
                        //printf("close\n");
                        continue;
                    }
                    //printf("%s\n", buffer);
                    //pthread_mutex_lock(&worker->modify_data_mutex);
                    ssize_t modified_len = worker->modify_data(buffer, len);
                    //pthread_mutex_unlock(&worker->modify_data_mutex);
                    ssize_t len_w = write_all(conn->server_fd, p_buffer, modified_len);

                    if (worker->w_flag > 0) {
                        if(clock_gettime(CLOCK_REALTIME, &ts) != 0) {
                            perror("clock_gettime");
                            continue;
                        }
                        //workerの中のconnection_tの中のstart_timeを更新
                        worker->fd_connection_map[fd]->start_time_sec = ts.tv_sec;
                        worker->fd_connection_map[fd]->start_time_nsec = ts.tv_nsec;
                    }
                    worker->w_flag += 1;
                    //printf("written_cli\n");
                    //printf("%ld\n", len_w);
                    //printf("cli_after = %d\n", worker->w_flag);

                } 
                else if (fd == conn->server_fd) {
                    if(worker->w_flag > 1) {
                        int x_sec;
                        long x_nsec;
                        x_sec = end_time.tv_sec - conn->start_time_sec;
                        x_nsec = (x_sec * 1000000000 + end_time.tv_nsec - conn->start_time_nsec) / 1000;
                        //printf("%ldnsec\n", x_nsec);
                        //printf("%d\n", conn->server_port);
                        //port毎にかかった時間の和をとりその平均を出す
                        //printf("ser_before =%d\n", worker->w_flag);
                        //printf("%d\n", conn->server_port);
                        //printf("%s\n", conn->server_host);
                        for (j = 0; j < MAX_REMOTE_PORT; j++) {
                            //portそれぞれfunctionAとBが最大3つずつあるから、ave_durationA1とA2、B1とB2を作る必要がある
                            //8000番台ポートに繋がっているものの平均
                            if ((conn->server_port == 8000 + j) && (conn->server_host == host1) && x_nsec > 0) {
                                pthread_mutex_lock(&worker->modify_data_mutex);
                                if (conn->server_port == 8000 && division_countA1[0] < 10) {
                                    division_listA1_1[division_countA1[0]] = x_nsec;
                                    all_durationA1[0] = all_durationA1[0] + x_nsec;
                                }
                                else if(conn->server_port == 8000 && division_countA1[0] >= 10) {
                                    division_listA1_1[division_countA1[0]] = x_nsec;
                                    all_durationA1[0] = all_durationA1[0] + x_nsec - division_listA1_1[division_countA1[0] - 10];
                                }
                                else if (conn->server_port == 8001 && division_countA1[1] < 10) {
                                    division_listA1_2[division_countA1[1]] = x_nsec;
                                    all_durationA1[1] = all_durationA1[1] + x_nsec;
                                }
                                else if (conn->server_port == 8001 && division_countA1[1] >= 10) {
                                    division_listA1_2[division_countA1[1]] = x_nsec;
                                    all_durationA1[1] = all_durationA1[1] + x_nsec - division_listA1_2[division_countA1[1] - 10];
                                }
                                else if (conn->server_port == 8002 && division_countA1[2] < 10) {
                                    division_listA1_3[division_countA1[2]] = x_nsec;
                                    all_durationA1[2] = all_durationA1[2] + x_nsec;
                                }
                                else if (conn->server_port == 8002 && division_countA1[2] >= 10) {
                                    division_listA1_3[division_countA1[2]] = x_nsec;
                                    all_durationA1[2] = all_durationA1[2] + x_nsec - division_listA1_3[division_countA1[2] - 10];
                                }
                                else if (conn->server_port == 8003 && division_countA1[3] < 10){
                                    division_listA1_4[division_countA1[3]] = x_nsec;
                                    all_durationA1[3] = all_durationA1[3] + x_nsec;
                                }
                                else {
                                    division_listA1_4[division_countA1[3]] = x_nsec;
                                    all_durationA1[3] = all_durationA1[3] + x_nsec - division_listA1_4[division_countA1[3] - 10];
                                }
                                division_countA1[j] += 1;
                                if (division_countA1[j] < 10){
                                    ave_durationA1[j] = all_durationA1[j] / division_countA1[j];
                                }
                                else {
                                    ave_durationA1[j] = all_durationA1[j] / 10;
                                }
                                pthread_mutex_unlock(&worker->modify_data_mutex);
                                
                            }
                            if ((conn->server_port == 8000 + j) && (conn->server_host == host2) && x_nsec > 0) {
                                pthread_mutex_lock(&worker->modify_data_mutex);
                                if (conn->server_port == 8000) {
                                    division_listA2_1[division_countA2[0]] = x_nsec;
                                }
                                else if (conn->server_port == 8001) {
                                    division_listA2_2[division_countA2[1]] = x_nsec;
                                }
                                else if (conn->server_port == 8002) {
                                    division_listA2_3[division_countA2[2]] = x_nsec;
                                }
                                else {
                                    division_listA2_4[division_countA2[3]] = x_nsec;
                                }         
                                all_durationA2[j] = all_durationA2[j] + x_nsec;
                                division_countA2[j] += 1;
                                ave_durationA2[j] = all_durationA2[j] / division_countA2[j];
                                pthread_mutex_unlock(&worker->modify_data_mutex);
                            }
                            //9000番台ポートに繋がっているものの平均を出す
                            if ((conn->server_port == 9000 + j) && (conn->server_host == host1) && x_nsec > 0) {
                                pthread_mutex_lock(&worker->modify_data_mutex);
                                if (conn->server_port == 9000) {
                                    division_listB1_1[division_countB1[0]] = x_nsec;
                                }
                                else if (conn->server_port == 9001) {
                                    division_listB1_2[division_countB1[1]] = x_nsec;
                                }
                                else if (conn->server_port == 9002) {
                                    division_listB1_3[division_countB1[2]] = x_nsec;
                                }
                                else {
                                    division_listB1_4[division_countB1[3]] = x_nsec;
                                }         
                                all_durationB1[j] = all_durationB1[j] + x_nsec;
                                division_countB1[j] += 1;
                                ave_durationB1[j] = all_durationB1[j] / division_countB1[j];
                                pthread_mutex_unlock(&worker->modify_data_mutex);
                            }
                            if ((conn->server_port == 9000 + j) && (conn->server_host == host2) && x_nsec > 0) {
                                pthread_mutex_lock(&worker->modify_data_mutex);
                                if (conn->server_port == 9000) {
                                    division_listB2_1[division_countB2[0]] = x_nsec;
                                }
                                else if (conn->server_port == 9001) {
                                    division_listB2_2[division_countB2[1]] = x_nsec;
                                }
                                else if (conn->server_port == 9002) {
                                    division_listB2_3[division_countB2[2]] = x_nsec;
                                }
                                else {
                                    division_listB2_4[division_countB2[3]] = x_nsec;
                                }         
                                all_durationB2[j] = all_durationB2[j] + x_nsec;
                                division_countB2[j] += 1;
                                ave_durationB2[j] = all_durationB2[j] / division_countB2[j];
                                pthread_mutex_unlock(&worker->modify_data_mutex);
                            }
                        }
                    }
                    worker->w_flag += 1;
                    //printf("ser_after = %d\n", worker->w_flag);

                    char buffer[BUFFER_SIZE];
                    char *p_buffer = buffer;
                    //printf("reading_ser\n");
                    ssize_t len = read_all(fd, p_buffer, sizeof(buffer));
                    //ssize_t len = read(fd, buffer, sizeof(buffer));
                    //printf("%s\n", buffer);
                    //printf("%ld\n", len);
                    if (len == 0 || len == -1) {
                        //printf("close\n");
                        close_connection(worker, conn);
                        continue;
                    }
                    //pthread_mutex_lock(&worker->modify_data_mutex);
                    ssize_t modified_len = worker->modify_data(buffer, len);
                    //pthread_mutex_unlock(&worker->modify_data_mutex);
                    //write(conn->client_fd, buffer, modified_len);
                    write_all(conn->client_fd, p_buffer, modified_len);
                    //printf("written_ser\n");
                }
            }
        }
    }
    return NULL;
}

void *accept_func(void *arg){
    accept_t *acc = (accept_t *)arg;
    char *remote_host_list[MAX_HOST];
    int remote_portA[MAX_REMOTE_PORT];
    int remote_portB[MAX_REMOTE_PORT];
    int worker_index, i= 0;
    int port_indexA[MAX_HOST];
    int port_indexB[MAX_HOST];
    int host_index[MAX_HOST];
    int remote_port;
    char *remote_host;

    //int fd_count = 0;

    //port, hostのindexを初期化
    memset(port_indexA, 0, sizeof(port_indexA));
    memset(port_indexB, 0, sizeof(port_indexB));
    memset(host_index, 0, sizeof(host_index));

    remote_host_list[0] = "192.168.62.61";
    remote_host_list[1] = "192.168.62.90";

    for(i = 0; i < MAX_REMOTE_PORT; i++){
        remote_portA[i] = 8000 + i;
        remote_portB[i] = 9000 + i;
    }

    // 接続受け入れループ
    while (1) {
        int client_fd;
        //int server_fd;
        char buf[BUFFER_SIZE];
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        client_fd = accept(acc->lis_fd, (struct sockaddr *)&client_addr, &client_len);
        // printf("%d\n", acc->lis_fd);
        // printf("%d\n", client_fd);
        // printf("accepted");

        if(inet_ntop(AF_INET, &client_addr.sin_addr, buf, sizeof(buf)) == NULL){
            perror("inet_ntop");
            continue;
        }

        // printf("accepted\n");
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        // TCP_NODELAYを設定
        int flag = 1;
        if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flag, sizeof(int)) < 0) {
            perror("setsockopt TCP_NODELAY on client_fd");
        }

        // ノンブロッキングに設定
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        //各functionの中で、一番平均時間が短いところに投げる
/*        if (acc->lis_port == 18000) {
            int min_index_A1 = find_min_index(ave_durationA1, MAX_REMOTE_PORT);
            int min_index_A2 = find_min_index(ave_durationA2, MAX_REMOTE_PORT);
            printf("%d\n", min_index_A1);
            printf("%d\n", min_index_A2);

            //ave_durationA1の中に、最も平均時間が短いポートがあるとき
            if (ave_durationA1[min_index_A1] <= ave_durationA2[min_index_A2]) {
                remote_host = remote_host_list[0];
                remote_port = remote_portA[min_index_A1];
            }
            //ave_durationA2の中に、最も平均時間が短いポートがあるとき
            else {
                remote_host = remote_host_list[1];
                remote_port = remote_portA[min_index_A2];
            }
        }
        if (acc->lis_port == 19000) {
            int min_index_B1 = find_min_index(ave_durationB1, MAX_REMOTE_PORT);
            int min_index_B2 = find_min_index(ave_durationB2, MAX_REMOTE_PORT);

            //ave_durationB1の中に、最も平均時間が短いポートがあるとき
            if (ave_durationB1[min_index_B1] <= ave_durationB2[min_index_B2]) {
                remote_host = remote_host_list[0];
                remote_port = remote_portB[min_index_B1];
            }
            else {
                remote_host = remote_host_list[1];
                remote_port = remote_portB[min_index_B2];
            }
        }
*/

        //ただ一つのサーバーにのみ繋げる
        //remote_host ="192.168.62.61";
        //remote_port = 8000;
        //ラウンドロビン(port18000 -> muc0(functionAのみ), port19000 -> muc1(functionBのみ))
/*        if (acc->lis_port == 18000) {
            remote_host = remote_host_list[0];
            remote_port = remote_portA[port_indexA[0]];
            port_indexA[0] = (port_indexA[0] + 1) % MAX_REMOTE_PORT;
        }
        else if (acc->lis_port == 19000) {
            remote_host = remote_host_list[1];
            remote_port = remote_portB[port_indexB[0]];

            port_indexB[0] = (port_indexB[0] + 1) % MAX_REMOTE_PORT;
        }
*/ 

        //ラウンドロビン(port18000 -> functionA(muc0とmuc1交互), port19000 -> functionB(muc0とmuc1交互))
        //functionAのportに繋げる
/*        if (acc->lis_port == 18000) {
            remote_host = remote_host_list[host_index[0]];
            host_index[0] = (host_index[0] + 1) % MAX_HOST;
            if (remote_host == remote_host_list[0]) {
                remote_port = remote_portA[port_indexA[0]];
                port_indexA[0] = (port_indexA[0] + 1) % MAX_REMOTE_PORT;
            }
            else {
                remote_port = remote_portA[port_indexA[1]];
                port_indexA[1] = (port_indexA[1] + 1) % MAX_REMOTE_PORT;
            }
        }
        //functionBのportに繋げる
        else if (acc->lis_port == 19000) {
            remote_host = remote_host_list[host_index[1]];
            host_index[1] = (host_index[1] + 1) % MAX_HOST;
            if (remote_host == remote_host_list[0]) {
                remote_port = remote_portB[port_indexB[0]];
                port_indexB[0] = (port_indexB[0] + 1) % MAX_REMOTE_PORT;
            }
            else {
                remote_port = remote_portB[port_indexB[1]];
                port_indexB[1] = (port_indexB[1] + 1) % MAX_REMOTE_PORT;
            }
        }
        else {
            perror("acc->lis_port");
        }
*/

            //readして中身を見て、uriを確認してそこで、どのポートに投げるか判断
        char buffer[BUFFER_SIZE];
        char *p_buf = buffer;
        //readしてデータをp_bufに保存
        ssize_t len = read_all(client_fd, p_buf, sizeof(buffer));
        //printf("%s\n", p_buf);
        //printf("%ld\n", len);
        //ここにlen = 0でもcloseすると、まだ到達していないだけのものが、接続が切れてしまう。
        if (len == -1) {
            close(client_fd);
            printf("close\n");
            continue;
        }
        //printf("%ld\n", len);
        //printf("%ld\n", sizeof(buffer));
        //remote_host = remote_host_list[0];
        //remote_port = remote_portA[2];
        //int test1_include = 0;
        //int test2_include = 0;
        //printf("port = %d\n", acc->lis_port);

        if (acc->lis_port == 18000) {
            int test1_include = contains_test1_in_uri(p_buf, len);
            //printf("%d\n", test1_include);
            if (test1_include == 1) {
                //ファンクションA1の平均時間を集めて比較する。書く物理マシンのポート8000,8001をファンクションA1とする。
                double A1_ave_duration[MAX_REMOTE_PORT];
                A1_ave_duration[0] = ave_durationA1[0];
                A1_ave_duration[1] = ave_durationA1[1];
                A1_ave_duration[2] = ave_durationA2[0];
                A1_ave_duration[3] = ave_durationA2[1];
                int min_index_A1 = find_min_index(A1_ave_duration, MAX_REMOTE_PORT);
                //printf("min_index_A1 = %d\n", min_index_A1);
                //min_index_A1に対応したホスト、ポートに投げる
                if (min_index_A1 == 0) {
                    remote_host = remote_host_list[0];
                    remote_port = remote_portA[0];
                }
                else if (min_index_A1 == 1) {
                    remote_host = remote_host_list[0];
                    remote_port = remote_portA[1];
                }
                else if (min_index_A1 == 2) {
                    remote_host = remote_host_list[1];
                    remote_port = remote_portA[0];
                }
                else {
                    remote_host = remote_host_list[1];
                    remote_port = remote_portA[1];
                }
            }
            //特定のuriが含まれていない場合。
            else {
                //printf("else = %d\n", test1_include);
                //ファンクションA2の平均時間を集めて比較する。書く物理マシンのポート8002,8003をファンクションA2とする。
                double A2_ave_duration[MAX_REMOTE_PORT];
                A2_ave_duration[0] = ave_durationA1[2];
                A2_ave_duration[1] = ave_durationA1[3];
                A2_ave_duration[2] = ave_durationA2[2];
                A2_ave_duration[3] = ave_durationA2[3];
                int min_index_A2 = find_min_index(A2_ave_duration, MAX_REMOTE_PORT);
                //printf("min_index_A2 = %d\n", min_index_A2);
                //min_index_A2に対応したホスト、ポートに投げる
                if (min_index_A2 == 0) {
                    remote_host = remote_host_list[0];
                    remote_port = remote_portA[2];
                }
                else if (min_index_A2 == 1) {
                    remote_host = remote_host_list[0];
                    remote_port = remote_portA[3];
                }
                else if (min_index_A2 == 2) {
                    remote_host = remote_host_list[1];
                    remote_port = remote_portA[2];
                }
                else {
                    remote_host = remote_host_list[1];
                    remote_port = remote_portA[3];
                }
            }
        }
        else if (acc->lis_port == 19000) {
            int test2_include = contains_test2_in_uri(p_buf, len);
            //printf("%d\n", test2_include);
            if (test2_include == 1) {
                //ファンクションB1の平均時間を集めて比較する。書く物理マシンのポート9000,9001をファンクションB1とする。
                double B1_ave_duration[MAX_REMOTE_PORT];
                B1_ave_duration[0] = ave_durationB1[0];
                B1_ave_duration[1] = ave_durationB1[1];
                B1_ave_duration[2] = ave_durationB2[0];
                B1_ave_duration[3] = ave_durationB2[1];
                int min_index_B1 = find_min_index(B1_ave_duration, MAX_REMOTE_PORT);
                //printf("min_index_B1 =%d\n", min_index_B1);
                //min_index_B1に対応したホスト、ポートに投げる
                if (min_index_B1 == 0) {
                    remote_host = remote_host_list[0];
                    remote_port = remote_portB[0];
                }
                else if (min_index_B1 == 1) {
                    remote_host = remote_host_list[0];
                    remote_port = remote_portB[1];
                }
                else if (min_index_B1 == 2) {
                    remote_host = remote_host_list[1];
                    remote_port = remote_portB[0];
                }
                else {
                    remote_host = remote_host_list[1];
                    remote_port = remote_portB[1];
                }
            }
            else {
                //ファンクションB2の平均時間を集めて比較する。書く物理マシンのポート9002,9003をファンクションB1とする。
                double B2_ave_duration[MAX_REMOTE_PORT];
                B2_ave_duration[0] = ave_durationB1[2];
                B2_ave_duration[1] = ave_durationB1[3];
                B2_ave_duration[2] = ave_durationB2[2];
                B2_ave_duration[3] = ave_durationB2[3];
                int min_index_B2 = find_min_index(B2_ave_duration, MAX_REMOTE_PORT);
                //printf("min_index_B2 =%d\n", min_index_B2);
                //min_index_B2に対応したホスト、ポートに投げる
                if (min_index_B2 == 0) {
                    remote_host = remote_host_list[0];
                    remote_port = remote_portB[2];
                }
                else if (min_index_B2 == 1) {
                    remote_host = remote_host_list[0];
                    remote_port = remote_portB[3];
                }
                else if (min_index_B2 == 2) {
                    remote_host = remote_host_list[1];
                    remote_port = remote_portB[2];
                }
                else {
                    remote_host = remote_host_list[1];
                    remote_port = remote_portB[3];
                }
            }
        }




        // リモートサーバーへの接続
        int server_fd = create_server_socket(remote_host, remote_port);
            //printf("try connected :%d", server_fd);
        if (server_fd < 0) {
            close(client_fd);
            continue;
        }
        
        connection_t *conn = malloc(sizeof(connection_t));
        worker_t *worker;
        // ワーカーへの割り当て
        if (acc->lis_port == 18000){
            worker = &workersA[worker_index];
        }
        else if (acc->lis_port == 19000){
            worker = &workersB[worker_index];
        }
        else {
            perror("acc->lis_port");
            continue;
        }
        worker_index = (worker_index + 1) % MAX_WORKERS;

        // コネクションの作成
        conn->client_fd = client_fd;
        conn->server_fd = server_fd;
        conn->server_port = remote_port;
        conn->server_host = inet_addr(remote_host);

        //readして中身を見て、uriを確認する場合のみ、ここでwriteする
        //write_all(server_fd, p_buf, len);

        // //時間を保存
        // struct timespec ts;
        // if(clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        //     perror("clock_gettime");
        //     continue;
        // }
        // //workerの中のconnection_tの中のstart_timeを更新
        // conn->start_time_sec = ts.tv_sec;
        // conn->start_time_nsec = ts.tv_nsec;

        // epollへのファイルディスクリプタの追加
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = client_fd;
        if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
            perror("epoll_ctl: client_fd");
            close(client_fd);
            close(server_fd);
            free(conn);
            continue;
        }
        ev.data.fd = server_fd;
        if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
            perror("epoll_ctl: server_fd");
            close(client_fd);
            close(server_fd);
            free(conn);
            continue;
        }

        //アクセプトのfdマップにコネクションを保存
        if(client_fd < MAX_FD)
            acc->fd_connection_map[client_fd] = conn;
            //fd_count += 1;
        if(server_fd < MAX_FD)
            acc->fd_connection_map[server_fd] = conn;

        //printf("%d\n", fd_count);
        // ワーカーのfdマップにコネクションを保存
        if (client_fd < MAX_FD)
            worker->fd_connection_map[client_fd] = conn;
        if (server_fd < MAX_FD)
            worker->fd_connection_map[server_fd] = conn;
    }
    return NULL;

}

int main(int argc, char *argv[]) {
    int listen_portA = 18000;
    int listen_portB = 19000;
    int listenA_fd;
    int listenB_fd;
    int i = 0;

    //all_durationの初期化
    memset(all_durationA1, 0, sizeof(all_durationA1));
    memset(all_durationA2, 0, sizeof(all_durationA2));
    memset(all_durationB1, 0, sizeof(all_durationB1));
    memset(all_durationB2, 0, sizeof(all_durationB2));

    //ave_durationの初期化
    memset(ave_durationA1, 0, sizeof(ave_durationA1));
    memset(ave_durationA2, 0, sizeof(ave_durationA2));
    memset(ave_durationB1, 0, sizeof(ave_durationB1));
    memset(ave_durationB2, 0, sizeof(ave_durationB2));

    //division_countの初期化
    memset(division_countA1, 0, sizeof(division_countA1));
    memset(division_countA2, 0, sizeof(division_countA1));
    memset(division_countB1, 0, sizeof(division_countB1));
    memset(division_countB2, 0, sizeof(division_countB2));

    //division_listの初期化
    memset(division_listA1_1, 0, sizeof(division_listA1_1));
    memset(division_listA1_2, 0, sizeof(division_listA1_2));
    memset(division_listA1_3, 0, sizeof(division_listA1_3));
    memset(division_listA1_4, 0, sizeof(division_listA1_4));
    memset(division_listA2_1, 0, sizeof(division_listA2_1));
    memset(division_listA2_2, 0, sizeof(division_listA2_2));
    memset(division_listA2_3, 0, sizeof(division_listA2_3));
    memset(division_listA2_4, 0, sizeof(division_listA2_4));
    memset(division_listB1_1, 0, sizeof(division_listB1_1));
    memset(division_listB1_2, 0, sizeof(division_listB1_2));
    memset(division_listB1_3, 0, sizeof(division_listB1_3));
    memset(division_listB1_4, 0, sizeof(division_listB1_4));
    memset(division_listB2_1, 0, sizeof(division_listB2_1));
    memset(division_listB2_2, 0, sizeof(division_listB2_2));
    memset(division_listB2_3, 0, sizeof(division_listB2_3));
    memset(division_listB2_4, 0, sizeof(division_listB2_4));

    // リスニングソケットの作成
    listenA_fd = create_listen_socket(listen_portA);
    listenB_fd = create_listen_socket(listen_portB);
    if (listenA_fd < 0) {
        exit(EXIT_FAILURE);
    }
    if (listenB_fd < 0) {
        exit(EXIT_FAILURE);
    }

    // printf("%d\n", listenA_fd);
    // printf("%d\n", listenB_fd);

    //listen_portをacceptに書き込む
    accs[0].lis_port = listen_portA;
    accs[1].lis_port = listen_portB;

    //listen_fdをacceptに書き込む
    accs[0].lis_fd = listenA_fd;
    accs[1].lis_fd = listenB_fd;



    // ワーカーの初期化
    for (i = 0; i < MAX_WORKERS; i++) {
        workersA[i].epoll_fd = epoll_create1(0);
        workersB[i].epoll_fd = epoll_create1(0);
        if (workersA[i].epoll_fd == -1) {
            perror("epoll_create1");
            exit(EXIT_FAILURE);
        }
        if (workersB[i].epoll_fd == -1) {
            perror("epoll_create1");
            exit(EXIT_FAILURE);
        }
        workersA[i].worker_id = i;
        workersB[i].worker_id = i;
        workersA[i].modify_data = default_modify_data;
        workersB[i].modify_data = default_modify_data;
        pthread_mutex_init(&workersA[i].modify_data_mutex, NULL);
        pthread_mutex_init(&workersB[i].modify_data_mutex, NULL);
        memset(workersA[i].fd_connection_map, 0, sizeof(workersA[i].fd_connection_map));
        memset(workersB[i].fd_connection_map, 0, sizeof(workersB[i].fd_connection_map));

        if (pthread_create(&worker_threadsA[i], NULL, worker_func, &workersA[i]) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
        if (pthread_create(&worker_threadsB[i], NULL, worker_func, &workersB[i]) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }
    //アクセプトの初期化
    for (i=0; i < MAX_ACCEPT; i++){
        memset(accs[i].fd_connection_map, 0, sizeof(accs[i].fd_connection_map));
        if (pthread_create(&acc_threads[i], NULL, accept_func, &accs[i]) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }

    //print_threads
    pthread_create(&print_threads, NULL, ave_print, NULL); 

    //while(1);

    // クリーンアップ
    
    for (i = 0; i < MAX_WORKERS; i++) {
        //printf("aa\n");
        pthread_join(worker_threadsA[i], NULL);
        pthread_join(worker_threadsB[i], NULL);
        close(workersA[i].epoll_fd);
        close(workersB[i].epoll_fd);
        //printf("worker closed");
    }
    return 0;

     // クリーンアップ
    for (i = 0; i < MAX_ACCEPT; i++) {
        pthread_join(acc_threads[i], NULL);
    }
    return 0;

    pthread_join(print_threads, NULL);

    
    close(listenA_fd);
    close(listenB_fd);
    //printf("all closed");
}

