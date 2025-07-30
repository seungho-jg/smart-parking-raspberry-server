#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include <stdlib.h>
#include <time.h>
#include <mysql/mysql.h>

// MySQL DB를 초기화 하고 사용할 수 있도록 연결하는 함수
int initDB(MYSQL *mysql, const char *host, const char *id, const char *pw, const char *db)
{
    printf("(i) initDB called, host=%s, id=%s, pw=%s, db=%s\n", host, id, pw, db);
    mysql_init(mysql); // DB 초기화
    if(mysql_real_connect(mysql, host, id, pw, db, 0, NULL, 0)) // DB 접속
    {
        printf("(i) mysql_real_connect success\n");
        return 0; // 성공
    }
    printf("(!) mysql_real_connect failed\n");
    return -1; // 실패
}


// 모든 주차공간 삭제 (초기화용)
int remove_parking_spaces(MYSQL *mysql)
{
    char strQuery[255]="";
    // remove 쿼리 작성
    sprintf(strQuery, "DELETE FROM parking_spaces");
    int res = mysql_query(mysql, strQuery);

    if(!res) {
        printf("(i) removed %lu parking spaces.\n", (unsigned long)mysql_affected_rows(mysql));
        return 0;
    } else {
        fprintf(stderr, "(!) remove error %d : %s\n", mysql_errno(mysql), mysql_error(mysql));
        return -1;
    }
}

// 주차공간 추가
int insert_parking_spaces(MYSQL *mysql, char *name, char *status)
{
    char strQuery[512] = "";

    sprintf(strQuery, "INSERT INTO parking_spaces(name, status) VALUES('%s', '%s')", name, status);
    int res = mysql_query(mysql, strQuery);
    
    if(!res) {
        printf("(i) inserted parking space: %s, status: %s\n", name, status);
        return (int)mysql_insert_id(mysql); // 생성된 ID 반환
    } else {
        fprintf(stderr, "(!) insert error %d : %s\n", mysql_errno(mysql), mysql_error(mysql));
        return -1;
    }
}
// 주차공간 상태 업데이트
int update_parking_space_status(MYSQL *mysql, int space_id, char *status)
{
    char strQuery[512] = "";
    sprintf(strQuery, "UPDATE parking_spaces SET status='%s' WHERE id=%d", status, space_id);
    int res = mysql_query(mysql, strQuery);

    if(!res) {
        printf("(i) updated parking space %d to status: %s\n", space_id, status);
        return 0;
    } else {
        fprintf(stderr, "(!) update error %d : %s\n", mysql_errno(mysql), mysql_error(mysql));
        return -1;
    }
}

// 비회원 주차 기록 (입차)
int insert_guest_parking_record(MYSQL *mysql, int space_id)
{
    char strQuery[512] = "";
    sprintf(strQuery, "INSERT INTO parking_records(space_id, entry_time) VALUES(%d, NOW())", space_id);
    int res = mysql_query(mysql, strQuery);
    
    if(!res) {
        int record_id = (int)mysql_insert_id(mysql);
        printf("(i) guest parking record created: record_id=%d, space_id=%d\n", record_id, space_id);
        return record_id;
    } else {
        fprintf(stderr, "(!) insert parking record error %d : %s\n", mysql_errno(mysql), mysql_error(mysql));
        return -1;
    }
}

// 회원 주차 기록 (입차)
int insert_member_parking_record(MYSQL *mysql, int space_id, int user_id)
{
    char strQuery[512] = "";
    sprintf(strQuery, "INSERT INTO parking_records(space_id, user_id, entry_time) VALUES(%d, %d, NOW())", space_id, user_id);
    int res = mysql_query(mysql, strQuery);
    
    if(!res) {
        int record_id = (int)mysql_insert_id(mysql);
        printf("(i) member parking record created: record_id=%d, space_id=%d, user_id=%d\n", record_id, space_id, user_id);
        return record_id;
    } else {
        fprintf(stderr, "(!) insert parking record error %d : %s\n", mysql_errno(mysql), mysql_error(mysql));
        return -1;
    }
}

// 출차 기록 (요금 계산 포함)
int update_parking_exit(MYSQL *mysql, int space_id, int parking_fee)
{
    char strQuery[512] = "";
    // 해당 주차공간의 출차 시간이 없는(아직 주차중인) 최신 기록을 업데이트
    sprintf(strQuery, "UPDATE parking_records SET exit_time=NOW(), parking_fee=%d WHERE space_id=%d AND exit_time IS NULL ORDER BY entry_time DESC LIMIT 1", parking_fee, space_id);
    int res = mysql_query(mysql, strQuery);
    
    if(!res) {
        if(mysql_affected_rows(mysql) > 0) {
            printf("(i) parking exit updated: space_id=%d, fee=%d\n", space_id, parking_fee);
            return 0;
        } else {
            printf("(!) no active parking record found for space_id=%d\n", space_id);
            return -1;
        }
    } else {
        fprintf(stderr, "(!) update exit error %d : %s\n", mysql_errno(mysql), mysql_error(mysql));
        return -1;
    }
}

// 특정 주차공간의 기록 조회
int read_parking_records(MYSQL *mysql, char *buf, int space_id)
{
    char strQuery[512] = "";
    buf[0] = 0; // 버퍼 초기화
    
    // 최근 10개 기록 조회
    sprintf(strQuery, "SELECT pr.id, pr.space_id, u.name, pr.entry_time, pr.exit_time, pr.parking_fee FROM parking_records pr LEFT JOIN users u ON pr.user_id = u.id WHERE pr.space_id=%d ORDER BY pr.entry_time DESC LIMIT 10", space_id);
    
    int res = mysql_query(mysql, strQuery);
    if(res != 0) {
        fprintf(stderr, "(!) query error %d : %s\n", mysql_errno(mysql), mysql_error(mysql));
        return -1;
    }
    
    MYSQL_RES *res_ptr = mysql_use_result(mysql);
    MYSQL_ROW sqlrow;
    
    strcat(buf, "주차기록|ID|공간ID|사용자|입차시간|출차시간|요금\n");
    
    while((sqlrow = mysql_fetch_row(res_ptr))) {
        char buf_line[512] = "";
        sprintf(buf_line, "|%s|%s|%s|%s|%s|%s\n",
                sqlrow[0] ? sqlrow[0] : "0",           // record_id
                sqlrow[1] ? sqlrow[1] : "0",           // space_id
                sqlrow[2] ? sqlrow[2] : "비회원",       // user_name
                sqlrow[3] ? sqlrow[3] : "없음",         // entry_time
                sqlrow[4] ? sqlrow[4] : "주차중",       // exit_time
                sqlrow[5] ? sqlrow[5] : "0");          // parking_fee
        strcat(buf, buf_line);
    }
    
    mysql_free_result(res_ptr);
    return 0;
}

// 주차공간 상태 조회
int read_parking_space(MYSQL *mysql, char *buf, int space_id)
{
    char strQuery[512] = "";
    buf[0] = 0; // 버퍼 초기화
    
    if(space_id == 0) {
        // 모든 주차공간 조회
        sprintf(strQuery, "SELECT id, name, status, updated_at FROM parking_spaces ORDER BY id");
    } else {
        // 특정 주차공간 조회
        sprintf(strQuery, "SELECT id, name, status, updated_at FROM parking_spaces WHERE id=%d", space_id);
    }
    
    int res = mysql_query(mysql, strQuery);
    if(res != 0) {
        fprintf(stderr, "(!) query error %d : %s\n", mysql_errno(mysql), mysql_error(mysql));
        return -1;
    }
    
    MYSQL_RES *res_ptr = mysql_use_result(mysql);
    MYSQL_ROW sqlrow;
    
    while((sqlrow = mysql_fetch_row(res_ptr))) {
        char buf_field[256] = "";
        sprintf(buf_field, "|%s|%s|%s|%s",
                sqlrow[0] ? sqlrow[0] : "0",      // id
                sqlrow[1] ? sqlrow[1] : "없음",    // name
                sqlrow[2] ? sqlrow[2] : "0",      // status
                sqlrow[3] ? sqlrow[3] : "없음");   // updated_at
        strcat(buf, buf_field);
    }
    
    if(mysql_errno(mysql)) {
        fprintf(stderr, "(!) error: %s\n", mysql_error(mysql));
        mysql_free_result(res_ptr);
        return -1;
    }
    
    mysql_free_result(res_ptr);
    return 0;
}

// 주차공간을 항상 1~24 ID로 유지
int init_parking_spaces(MYSQL *mysql)
{
    // 기존 데이터 삭제 및 AUTO_INCREMENT 초기화
    remove_parking_spaces(mysql);
    
    // 1~24번 ID로 고정 생성
    for(int i = 1; i <= 24; i++) {
        char strQuery[512] = "";
        char name[20];
        sprintf(name, "Space-%d", i);
        
        // ID를 명시적으로 지정
        sprintf(strQuery, "INSERT INTO parking_spaces(id, name, status) VALUES(%d, '%s', 'AVAILABLE')", i, name);
        int res = mysql_query(mysql, strQuery);
        
        if(res) {
            fprintf(stderr, "(!) failed to create parking space %d: %s\n", i, mysql_error(mysql));
            return -1;
        }
    }
    
    // AUTO_INCREMENT를 25로 설정 (다음 자동 생성 ID)
    char strQuery[255] = "";
    sprintf(strQuery, "ALTER TABLE parking_spaces AUTO_INCREMENT = 25");
    mysql_query(mysql, strQuery);
    
    printf("(i) initialized 24 parking spaces with fixed IDs (1-24)\n");
    return 0;
}

// 현재 주차중인 차량 수 조회
int get_occupied_count(MYSQL *mysql)
{
    char strQuery[512] = "";
    sprintf(strQuery, "SELECT COUNT(*) FROM parking_spaces WHERE status='2'");
    int res = mysql_query(mysql, strQuery);
    
    if(res != 0) {
        fprintf(stderr, "(!) query error %d : %s\n", mysql_errno(mysql), mysql_error(mysql));
        return -1;
    }
    
    MYSQL_RES *res_ptr = mysql_use_result(mysql);
    MYSQL_ROW sqlrow = mysql_fetch_row(res_ptr);
    
    int count = 0;
    if(sqlrow && sqlrow[0]) {
        count = atoi(sqlrow[0]);
    }
    
    mysql_free_result(res_ptr);
    return count;
}

// 차량 진입시 entry_time 업데이트 (Java 예약 → 센서 감지)
int update_parking_entry_time(MYSQL *mysql, int space_id)
{
    char strQuery[512] = "";
    // entry_time이 NULL인 예약 기록을 찾아서 현재 시간으로 업데이트
    sprintf(strQuery, "UPDATE parking_records SET entry_time=NOW() WHERE space_id=%d AND entry_time IS NULL AND exit_time IS NULL ORDER BY id DESC LIMIT 1", space_id);
    int res = mysql_query(mysql, strQuery);

    if(!res) {
        if(mysql_affected_rows(mysql) > 0) {
            printf("(i) entry time updated: space_id=%d\n", space_id);
            return 0;
        } else {
            printf("(!) no reservation found for space_id=%d\n", space_id);
            return -1;
        }
    } else {
        fprintf(stderr, "(!) update entry time error %d : %s\n", mysql_errno(mysql), mysql_error(mysql));
        return -1;
    }
}

// 요금 계산 함수 (시간당 1000원, 최소 1시간)
int calculate_parking_fee(MYSQL *mysql, int space_id)
{
    char strQuery[512] = "";
    sprintf(strQuery, "SELECT TIMESTAMPDIFF(HOUR, entry_time, NOW()) as hours FROM parking_records WHERE space_id=%d AND entry_time IS NOT NULL AND exit_time IS NULL ORDER BY entry_time DESC LIMIT 1", space_id);
    
    int res = mysql_query(mysql, strQuery);
    if(res != 0) {
        fprintf(stderr, "(!) query error %d : %s\n", mysql_errno(mysql), mysql_error(mysql));
        return 1000; // 기본 1시간 요금
    }

    MYSQL_RES *res_ptr = mysql_use_result(mysql);
    MYSQL_ROW sqlrow = mysql_fetch_row(res_ptr);

    int hours = 1; // 기본 1시간
    if(sqlrow && sqlrow[0]) {
        hours = atoi(sqlrow[0]);
        if(hours == 0) hours = 1; // 최소 1시간
    }

    mysql_free_result(res_ptr);
    int fee = hours * 1000; // 시간당 1000원
    printf("(i) calculated parking fee: %d hours = %d won\n", hours, fee);
    return fee;
}

// DB 접속 종료하기
int closeDB(MYSQL *mysql)
{
    mysql_close(mysql); // db 연결 해제
    return 1;
}