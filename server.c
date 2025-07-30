#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <wiringPi.h>
#include <wiringSerial.h>
// 소켓
#include <sys/socket.h>
#include <arpa/inet.h>
// db
#include <mysql/mysql.h>
#include <time.h>

// 전역 변수
int serial_fd; // 아두이노와 통신할 fd
int java_fd; // java와 통신할 fd
MYSQL mysql; // mysql 커넥션
char device[] = "/dev/ttyACM0";
unsigned long baud = 9600;
char parking_status[25] = "000000000000000000000000"; // 24개 주차공간 0: 사용가능(AVAILABLE) 1: 예약됨(RESERVED) 2: 점유됨(OCCUPIED)

//ControlDB.c
extern int initDB(MYSQL*, const char *host, const char *id, const char *pw, const char *db);
extern int remove_parking_spaces(MYSQL *mysql); // 모든 주차공간 삭제 (초기화용)
extern int insert_parking_spaces(MYSQL *mysql, char *name, char *status); // 주차공간 추가
extern int update_parking_space_status(MYSQL *mysql, int space_id, char *status); // 주차공간 상태 업데이트
extern int insert_guest_parking_record(MYSQL *mysql, int space_id); // 비회원 주차 기록 (입차)
extern int insert_member_parking_record(MYSQL *mysql, int space_id, int user_id); // 회원 주차 기록 (입차)
extern int update_parking_exit(MYSQL *mysql, int space_id, int parking_fee); //출차 기록 (요금 계산 포함)
extern int read_parking_records(MYSQL *mysql, char *buf, int space_id);  // 특정 주차공간의 기록 조회
extern int read_parking_space(MYSQL *mysql, char *buf, int space_id); // 주차공간 상태 조회
extern int init_parking_spaces(MYSQL *mysql); // 주차공간 초기화
extern int get_occupied_count(MYSQL *mysql); // 현재 주차중인 차량 수 조회
extern int update_parking_entry_time(MYSQL *mysql, int space_id); // 차량 진입시 entry_time 업데이트
extern int calculate_parking_fee(MYSQL *mysql, int space_id); // 요금 계산
extern int closeDB(MYSQL *mysql);


// 함수 호이스팅
void error_handling(char *msg);
void *arduino_read_thread(void *arg);
void *tcp_server_thread(void *arg);
void *java_tcp_thread(void *arg);
void send_to_arduino(int space_id, char status);
void update_parking_status(int space_id, char new_status);


int main() {
    printf("%s \n", "Raspberry Startup!");
    fflush(stdout);

    // db init
    if(initDB(&mysql, "localhost", "seunho", "12345", "parkingsys") < 0)
    {
        printf("(!) initDB faild\n"); // 실패
        return -1;
    }
    else printf("(i) intDB successd!\n"); // 성공

    // 주차공간 초기화
    init_parking_spaces(&mysql);

    // 시리얼 포트 열기
    if ((serial_fd = serialOpen(device, baud)) < 0) {
        fprintf(stderr, "Unalbe to open serial device: %s\n", strerror(errno));
        return 1;
    }

    // wiringPi 초기화
    if (wiringPiSetup() == -1) {
        fprintf(stdout, "Unable to start wiringPi: %s\n", strerror(errno));
        return 1;
    }

    pthread_t arduino_tid, tcp_tid;

    // TCP 서버 스레드 생성
    if (pthread_create(&tcp_tid, NULL, tcp_server_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create TCP server thread\n");
        return 1;
    }

    // 읽기 스레드 생성
    if (pthread_create(&arduino_tid, NULL, arduino_read_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create read thread\n");
        return 1;
    }

    // 스레드가 종료될 때까지 메인 스레드는 대기
    pthread_join(tcp_tid, NULL);
    pthread_join(arduino_tid, NULL);

    serialClose(serial_fd);
    closeDB(&mysql);
    return 0;
}

void error_handling(char *message)
{
	fputs(message, stderr);
	fputc('\n', stderr);
	exit(1);
}

// 아두이노에서 센서 데이터 수신
void *arduino_read_thread(void *arg) {
    char buffer[256];
    int buffer_pos = 0;

    while(1) {
        if (serialDataAvail(serial_fd)) {
            char newChar = serialGetchar(serial_fd);

            if (newChar == '\n' || newChar == '\r') {
                if (buffer_pos > 0) {
                    buffer[buffer_pos] = '\0';
                    printf("아두이노에서 받음: %s\n", buffer);

                    // "1:true" 또는 "1:false" 형식 파싱
                    int space_id;
                    char sensor_value[10];
                    if (sscanf(buffer, "%d:%s", &space_id, sensor_value) == 2) {
                        if (space_id >= 1 && space_id <= 24) {
                            char new_status;
                            if (strcmp(sensor_value, "true") == 0) {
                                new_status = '2'; // 점유됨
                            } else if (strcmp(sensor_value, "false") == 0) {
                                new_status = '0'; // 사용가능 (예약 상태는 유지x)
                            } else {
                                continue; // 잘못된 값
                            }

                            update_parking_status(space_id - 1, new_status); // 배열은 0부터 시작
                        }
                    }

                    buffer_pos = 0;
                }
            } else {
                if (buffer_pos < sizeof(buffer) - 1) {
                    buffer[buffer_pos++] = newChar;
                }
            }
        }
        usleep(10000);
    }
    return NULL;
}

// 주차 상태 업데이트
void update_parking_status(int space_id, char new_status) {
    char old_status = parking_status[space_id];
    char upload_status[255] = "";

    // 상태 변화가 있을 때만 처리
    if (old_status != new_status) {
        parking_status[space_id] = new_status;
        printf("주차공간 %d 상태 변경: %c -> %c\n", space_id + 1, old_status, new_status);

        // 상태에 따른 DB용 문자열 설정
        if (new_status == '0') {
            strcpy(upload_status, "AVAILABLE");
        } else if (new_status == '1') {
            strcpy(upload_status, "RESERVED");
        } else if (new_status == '2') {
            strcpy(upload_status, "OCCUPIED");
        } else {
            strcpy(upload_status, "UNKNOWN");
        }

        printf("DB 업데이트: space_id=%d, status=%s\n", space_id + 1, upload_status);

        // 상태별 주차기록 처리
        if (old_status == '1' && new_status == '2') {
            // 예약됨 → 점유됨: 차량 진입 감지
            printf("차량 진입 감지: 주차기록 entry_time 업데이트\n");
            update_parking_entry_time(&mysql, space_id + 1);
        } else if (old_status == '2' && new_status == '0') {
            // 점유됨 → 사용가능: 차량 이탈 감지
            printf("차량 이탈 감지: 주차기록 exit_time 업데이트\n");
            int parking_fee = calculate_parking_fee(&mysql, space_id + 1);
            update_parking_exit(&mysql, space_id + 1, parking_fee);
        } else if (old_status == '1' && new_status == '0') {
            // 예약됨 → 사용가능: 예약 취소 (Java에서 처리됨)
            printf("예약 취소 또는 예약 시간 초과\n");
        } else if (old_status == '0' && new_status == '2') {
            insert_guest_parking_record(&mysql, space_id + 1);
            printf("비회원 주차기록 업데이트 완료\n");
        }
            

        // 아두이노 LED 제어
        send_to_arduino(space_id + 1, new_status);
        update_parking_space_status(&mysql, space_id + 1, upload_status);
    }
}

// 아두이노에 LED 제어 명령 전송
void send_to_arduino(int space_id, char status) {
    char command[50];
    if (status == '0') {
        sprintf(command, "%d:AVAILABLE", space_id);
    } else if (status == '1') {
        sprintf(command, "%d:RESERVED", space_id);
    } else if (status == '2') {
        sprintf(command, "%d:OCCUPIED", space_id);
    }

    serialPuts(serial_fd, command);
    printf("아두이노에 전송: %s\n", command);
}


// TCP 서버 스레드
void *tcp_server_thread(void *arg) {
    int server_fd, client_fd; // 소켓 파일 디스크립터
    struct sockaddr_in server_adr, client_adr;
    socklen_t client_adr_size;

    server_fd = socket(PF_INET, SOCK_STREAM, 0);
    if(server_fd == -1) error_handling("socket() error");

    // 소켓 재사용 옵션
    int option = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    // 소켓 생성 및 바인딩  (포트 8888)
    memset(&server_adr, 0, sizeof(server_adr));
    server_adr.sin_family=AF_INET;
    server_adr.sin_addr.s_addr=htonl(INADDR_ANY);
    server_adr.sin_port=htons(8888);

    if(bind(server_fd, (struct sockaddr *) &server_adr, sizeof(server_adr))==-1)
        error_handling("bind() error");

    if(listen(server_fd, 5)==-1)
        error_handling("listen() error");

    while(1)
    {
        client_adr_size = sizeof(client_adr);
        client_fd = accept(server_fd, (struct sockaddr*) &client_adr, &client_adr_size);

        if(client_fd == -1) continue;

        printf("java WAS 연결됨!\n");
        java_fd = client_fd;

        // Java와 통신하는 스레드 시작
        pthread_t java_thread;
        pthread_create(&java_thread, NULL, java_tcp_thread, &client_fd);

        // 스레드 종료까지 대기
        pthread_join(java_thread, NULL);

        close(client_fd);
        java_fd = -1;
        printf("Java WAS 연결 끊어짐\n");
    }

}

// Java에서 오는 메시지 계속 읽기
void *java_tcp_thread(void *arg) {
    int client_fd = *(int*)arg;
    char buffer[256];
    int bytes;

    while(1) {
        // Java에서 명령 받기
        bytes = recv(client_fd, buffer, sizeof(buffer)-1, 0);
        if(bytes <= 0) break; // 연결 끊어짐

        buffer[bytes] = '\0';
        printf("Java에서 받음: %s\n", buffer);

        // 명령 처리
        if (strncmp(buffer, "GET_ALL", 7) == 0) {
            // 전체 상태 전송: "24:000111222000111222000111"
            char response[50];
            sprintf(response, "24:%s\n", parking_status);
            send(client_fd, response, strlen(response), 0);
            printf("Java에게 전송: %s", response);

        } else if (strncmp(buffer, "RESERVE:", 8) == 0) {
            // 예약 명령: "RESERVE:3"
            int space_id;
            if (sscanf(buffer, "RESERVE:%d", &space_id) == 1) {
                if (space_id >= 1 && space_id <= 24) {
                    // 사용 가능한 상태일 때만 예약 가능
                    if (parking_status[space_id - 1] == '0') {
                        update_parking_status(space_id - 1, '1');
                        send(client_fd, "OK\n", 3, 0);
                        printf("예약 성공: %d번 자리\n", space_id);
                    } else {
                        send(client_fd, "FAIL\n", 5, 0);
                        printf("예약 실패: %d번 자리 (이미 사용중)\n", space_id);
                    }
                }
            }

        } else if (strncmp(buffer, "CANCEL:", 7) == 0) {
            // 예약 취소 명령: "CANCEL:3"
            int space_id;
            if (sscanf(buffer, "CANCEL:%d", &space_id) == 1) {
                if (space_id >= 1 && space_id <= 24) {
                    // 예약 상태일 때만 취소 가능
                    if (parking_status[space_id - 1] == '1') {
                        update_parking_status(space_id - 1, '0');
                        send(client_fd, "OK\n", 3, 0);
                        printf("예약 취소: %d번 자리\n", space_id);
                    } else {
                        send(client_fd, "FAIL\n", 5, 0);
                        printf("취소 실패: %d번 자리 (예약 상태 아님)\n", space_id);
                    }
                }
            }
        }
    }

    return NULL;
}