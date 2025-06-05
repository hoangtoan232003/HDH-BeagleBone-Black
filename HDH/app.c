#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <cjson/cJSON.h>
#include <mosquitto.h>
#include <linux/watchdog.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/time.h>
#include <pthread.h>

/* Khung định nghĩa */
#define DHT11_DEVICE_PATH "/dev/dht11"
#define BH1750_DEVICE_PATH "/dev/bh1750"
#define LED_DEVICE_PATH "/dev/led"   /* Đọc trạng thái từ /dev/led */
#define LOG_FILE "/var/log/system.log"
#define WATCHDOG_DEVICE "/dev/watchdog"

#define BUFFER_SIZE 128
#define MAX_RETRIES 3

/* MQTT configuration */
#define MQTT_BROKER "192.168.6.1"
#define MQTT_PORT 1884
#define MQTT_CLIENT_ID "bbb_sensor_app"
#define MQTT_QOS 1
#define MQTT_USER "toan"
#define MQTT_PASS "1"

/* Topic: dữ liệu cảm biến gửi lên và lệnh LED nhận xuống */
#define MQTT_SENSOR_TOPIC "bbb/sensors"  /* Gửi dữ liệu: temperature, humidity, lux */
#define MQTT_LED_TOPIC "bbb/led"           /* Nhận lệnh điều khiển LED (JSON có "led1" và "led2") */

#define BLINK_INTERVAL 200000     /* 0.2 giây */
#define RECONNECT_INTERVAL 5      /* Giây */
#define WATCHDOG_TIMEOUT 120      /* Timeout watchdog */
#define MAX_LOG_SIZE (1024 * 1024)  /* 1MB */
#define LOG_STATUS_INTERVAL 300   /* 5 phút */

/* Các macro bổ sung */
#define IO_TIMEOUT_SECONDS 2      /* I/O timeout */
#define DHT11_MAX_FAILS 5         /* Ngưỡng lỗi DHT11 */
#define DHT11_THREAD_TIMEOUT 10   /* Timeout thread DHT11 */

/* Nếu muốn sử dụng watchdog, đặt =1 */
static int use_watchdog = 1;
static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t dht11_enabled = 1;
static int dht11_fail_count = 0;
static volatile time_t last_loop_time = 0;
static volatile time_t last_dht11_time = 0;
static float last_temp = 0.0, last_humid = 0.0;
static pthread_t dht11_thread, monitor_thread;
static pthread_mutex_t dht11_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Biến LED2: Nếu led2_blinking = 1 thì LED2 đang ở chế độ nháy liên tục */
static volatile int led2_blinking = 0;


void log_data(const char *message) {
    struct stat st;
    if(stat(LOG_FILE, &st)==0 && st.st_size > MAX_LOG_SIZE)
        rename(LOG_FILE, LOG_FILE ".bak");
    FILE *fp = fopen(LOG_FILE, "a");
    if(fp) {
        time_t now = time(NULL);
        char *time_str = ctime(&now);
        if(time_str)
            time_str[strlen(time_str)-1] = '\0';
        fprintf(fp, "[%s] %s\n", time_str, message);
        fclose(fp);
    }
}

void log_system_status() {
    struct rusage usage;
    char buffer[BUFFER_SIZE];
    if(getrusage(RUSAGE_SELF, &usage)==0) {
        snprintf(buffer, sizeof(buffer),
          "System status: Memory usage: %ld KB, User time: %ld.%06ld s",
          usage.ru_maxrss, usage.ru_utime.tv_sec, usage.ru_utime.tv_usec);
        log_data(buffer);
    }
    FILE *fp = popen("lsof -p $(pidof app) | wc -l", "r");
    if(fp) {
        int fd_count;
        if(fscanf(fp, "%d", &fd_count)==1) {
            snprintf(buffer, sizeof(buffer),
              "System status: Open file descriptors: %d", fd_count);
            log_data(buffer);
        }
        pclose(fp);
    }
}

static int check_device(const char *path) {
    struct stat st;
    if(stat(path, &st)<0) {
        char buffer[BUFFER_SIZE];
        snprintf(buffer, sizeof(buffer), "Device %s không tồn tại", path);
        log_data(buffer);
        return -1;
    }
    return 0;
}

/* --------------------- BH1750 --------------------- */
int read_bh1750(unsigned int *lux) {
    int fd = open(BH1750_DEVICE_PATH, O_RDONLY);  /* Blocking read */
    if(fd < 0) {
        log_data("BH1750: Failed to open device");
        return -1;
    }
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));
    ssize_t ret = read(fd, buffer, sizeof(buffer)-1);
    if(ret <= 0) {
        log_data("BH1750: Failed to read device or no data returned");
        close(fd);
        return -1;
    }
    buffer[ret] = '\0';
    {
        char raw_data[BUFFER_SIZE];
        snprintf(raw_data, sizeof(raw_data), "BH1750: Raw data: '%s'", buffer);
        log_data(raw_data);
    }
    if(sscanf(buffer, "%u", lux)!=1) {
        log_data("BH1750: Failed to parse lux value");
        close(fd);
        return -1;
    }
    close(fd);
    log_data("BH1750: Read successful");
    printf("BH1750: Light value = %u lux\n", *lux);
    fflush(stdout);
    return 0;
}

/* --------------------- LED STATUS --------------------- */
int read_led_status(char *status, size_t max_len) {
    int fd = open(LED_DEVICE_PATH, O_RDONLY);
    if(fd < 0) {
        log_data("LED: Failed to open device for reading status");
        return -1;
    }
    ssize_t ret = read(fd, status, max_len-1);
    if(ret < 0) {
        log_data("LED: Failed to read device status");
        close(fd);
        return -1;
    }
    status[ret] = '\0';
    size_t len = strlen(status);
    while(len > 0 && (status[len-1]=='\n' || status[len-1]=='\r' || status[len-1] < 32)) {
        status[len-1] = '\0';
        len--;
    }
    close(fd);
    return 0;
}

/* --------------------- DHT11 --------------------- */
void *dht11_thread_func(void *arg) {
    float temp, humid;
    int fd = -1;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    int retries;
    
    while(running && dht11_enabled) {
        retries = 0;
        log_data("DHT11: Starting communication");
        if(check_device(DHT11_DEVICE_PATH)!=0) {
            log_data("DHT11: Device check failed");
            pthread_mutex_lock(&dht11_mutex);
            dht11_fail_count++;
            pthread_mutex_unlock(&dht11_mutex);
            sleep(1);
            continue;
        }
        while(retries < MAX_RETRIES && running && dht11_enabled) {
            fd = open(DHT11_DEVICE_PATH, O_RDONLY | O_NONBLOCK);
            if(fd < 0) {
                fprintf(stderr, "DHT11: Failed to open device: %s\n", strerror(errno));
                log_data("DHT11: Failed to open device");
                pthread_mutex_lock(&dht11_mutex);
                dht11_fail_count++;
                pthread_mutex_unlock(&dht11_mutex);
                sleep(1);
                break;
            }
            struct timeval timeout;
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(fd, &read_fds);
            timeout.tv_sec = IO_TIMEOUT_SECONDS;
            timeout.tv_usec = 0;
            int ret_sel = select(fd+1, &read_fds, NULL, NULL, &timeout);
            if(ret_sel <= 0) {
                fprintf(stderr, "DHT11: Select timeout or error: %s\n", ret_sel==0 ? "Timeout" : strerror(errno));
                log_data("DHT11: Select timeout or error");
                close(fd);
                retries++;
                pthread_mutex_lock(&dht11_mutex);
                dht11_fail_count++;
                pthread_mutex_unlock(&dht11_mutex);
                usleep(100000);
                continue;
            }
            memset(buffer, 0, sizeof(buffer));
            bytes_read = read(fd, buffer, sizeof(buffer)-1);
            if(bytes_read < 0) {
                fprintf(stderr, "DHT11: Failed to read device: %s\n", strerror(errno));
                log_data("DHT11: Failed to read device");
                close(fd);
                retries++;
                pthread_mutex_lock(&dht11_mutex);
                dht11_fail_count++;
                pthread_mutex_unlock(&dht11_mutex);
                usleep(100000);
                continue;
            }
            buffer[bytes_read] = '\0';
            {
                char raw_data[BUFFER_SIZE];
                snprintf(raw_data, sizeof(raw_data), "DHT11: Raw data: '%s'", buffer);
                log_data(raw_data);
            }
            int temp_int, humid_int;
            if(sscanf(buffer, "Temp: %dC, Hum: %d%%", &temp_int, &humid_int) != 2) {
                fprintf(stderr, "DHT11: Failed to parse data: '%s' (bytes read: %zd)\n", buffer, bytes_read);
                snprintf(buffer, sizeof(buffer), "DHT11: Failed to parse data: '%s'", buffer);
                log_data(buffer);
                close(fd);
                retries++;
                pthread_mutex_lock(&dht11_mutex);
                dht11_fail_count++;
                pthread_mutex_unlock(&dht11_mutex);
                usleep(100000);
                continue;
            }
            temp = (float)temp_int;
            humid = (float)humid_int;
            close(fd);
            log_data("DHT11: Read successful");
            pthread_mutex_lock(&dht11_mutex);
            last_temp = temp;
            last_humid = humid;
            dht11_fail_count = 0;
            last_dht11_time = time(NULL);
            pthread_mutex_unlock(&dht11_mutex);
            break;
        }
        if(retries >= MAX_RETRIES) {
            log_data("DHT11: Failed to read after retries");
            pthread_mutex_lock(&dht11_mutex);
            dht11_fail_count++;
            pthread_mutex_unlock(&dht11_mutex);
        }
        if(dht11_fail_count >= DHT11_MAX_FAILS) {
            log_data("DHT11: Disabled due to excessive failures");
            dht11_enabled = 0;
        }
        sleep(3);
    }
    if(fd >= 0) close(fd);
    return NULL;
}

void *monitor_thread_func(void *arg) {
    while(running) {
        time_t now = time(NULL);
        if(now - last_loop_time > 5)
            log_data("Monitor: Main loop appears to be stuck");
        if(dht11_enabled && now - last_dht11_time > DHT11_THREAD_TIMEOUT) {
            log_data("Monitor: DHT11 thread appears to be stuck, restarting");
            pthread_cancel(dht11_thread);
            pthread_join(dht11_thread, NULL);
            if(pthread_create(&dht11_thread, NULL, dht11_thread_func, NULL) != 0)
                log_data("Monitor: Failed to restart DHT11 thread");
        }
        sleep(2);
    }
    return NULL;
}

/* --------------------- WATCHDOG --------------------- */
int init_watchdog(int *watchdog_fd) {
    if(!use_watchdog) {
        log_data("Watchdog disabled");
        return 0;
    }
    *watchdog_fd = open(WATCHDOG_DEVICE, O_RDWR);
    if(*watchdog_fd < 0) {
        fprintf(stderr, "Failed to open watchdog device: %s\n", strerror(errno));
        log_data("Failed to open watchdog device");
        return -1;
    }
    int timeout = WATCHDOG_TIMEOUT;
    if(ioctl(*watchdog_fd, WDIOC_SETTIMEOUT, &timeout) < 0) {
        fprintf(stderr, "Failed to set watchdog timeout: %s\n", strerror(errno));
        log_data("Failed to set watchdog timeout");
        close(*watchdog_fd);
        return -1;
    }
    if(ioctl(*watchdog_fd, WDIOC_KEEPALIVE, 0) < 0) {
        fprintf(stderr, "Failed to send initial watchdog keepalive: %s\n", strerror(errno));
        log_data("Failed to send initial watchdog keepalive");
        close(*watchdog_fd);
        return -1;
    }
    printf("Watchdog initialized with timeout %d seconds\n", timeout);
    log_data("Watchdog initialized");
    return 0;
}

int ping_watchdog(int watchdog_fd) {
    if(!use_watchdog || watchdog_fd < 0)
        return 0;
    if(ioctl(watchdog_fd, WDIOC_KEEPALIVE, 0) < 0) {
        fprintf(stderr, "Failed to ping watchdog: %s\n", strerror(errno));
        log_data("Failed to ping watchdog");
        return -1;
    }
    return 0;
}

void disable_watchdog(int watchdog_fd) {
    if(!use_watchdog || watchdog_fd < 0)
        return;
    if(write(watchdog_fd, "V", 1) < 0) {
        fprintf(stderr, "Failed to disable watchdog: %s\n", strerror(errno));
        log_data("Failed to disable watchdog");
    }
    close(watchdog_fd);
    log_data("Watchdog disabled");
}

/* --------------------- LED CONTROL --------------------- */
int set_led_state(int led_num, int state) {
    int fd = -1;
    char buffer[16];
    log_data("LED: Attempting to set state");
    if(check_device(LED_DEVICE_PATH) != 0) {
        log_data("LED: Device check failed");
        return -1;
    }
    fd = open(LED_DEVICE_PATH, O_WRONLY);
    if(fd < 0) {
        fprintf(stderr, "LED: Failed to open device: %s\n", strerror(errno));
        log_data("LED: Failed to open device");
        return -1;
    }
    sprintf(buffer, "%d:%d", led_num, state);
    if(write(fd, buffer, strlen(buffer)) < 0) {
        fprintf(stderr, "LED: Failed to control LED %d: %s\n", led_num, strerror(errno));
        snprintf(buffer, sizeof(buffer), "LED: Failed to control LED %d", led_num);
        log_data(buffer);
        close(fd);
        return -1;
    }
    close(fd);
    log_data("LED: Set state successful");
    return 0;
}

/* Hàm blink_led dành cho LED2: 
   Khi led2_blinking = 1, LED2 sẽ nháy liên tục cho đến khi nhận được lệnh OFF.
*/
int blink_led(struct mosquitto *mosq, int led_num) {
    char buffer[BUFFER_SIZE];
    if(led_num == 2 && led2_blinking) {
        /* Trong vòng lặp main, nếu led2_blinking đang bật, ta gọi hàm blink_led mỗi vòng. */
        if(set_led_state(led_num, 1) < 0)
            return -1;
        usleep(BLINK_INTERVAL);
        if(set_led_state(led_num, 0) < 0)
            return -1;
        usleep(BLINK_INTERVAL);
        return 0;
    } else {
        /* Với LED1 hoặc LED2 khi không nháy, thực hiện nháy 1 chu kỳ */
        if(set_led_state(led_num, 1) < 0)
            return -1;
        usleep(BLINK_INTERVAL);
        if(set_led_state(led_num, 0) < 0)
            return -1;
        usleep(BLINK_INTERVAL);
        return 0;
    }
}

int publish_led_status(struct mosquitto *mosq, int led_num, const char *state) {
    char buffer[BUFFER_SIZE];
    log_data("MQTT: Attempting to publish LED status");
    cJSON *jobj = cJSON_CreateObject();
    if(!jobj) {
        log_data("MQTT: Failed to create cJSON object for LED status");
        return -1;
    }
    /* Sử dụng chuỗi "ON" hoặc "OFF" cho LED */
    if(strcmp(state, "1") == 0)
        cJSON_AddStringToObject(jobj, "state", "ON");
    else
        cJSON_AddStringToObject(jobj, "state", "OFF");
    char *payload = cJSON_PrintUnformatted(jobj);
    if(!payload) {
        log_data("MQTT: Failed to print cJSON object for LED status");
        cJSON_Delete(jobj);
        return -1;
    }
    char topic[32];
    snprintf(topic, sizeof(topic), "status/led/%d", led_num);
    int ret = mosquitto_publish(mosq, NULL, topic, strlen(payload), payload, MQTT_QOS, false);
    if(ret != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTT: Failed to publish LED %d status to %s: %s\n", led_num, topic, mosquitto_strerror(ret));
        snprintf(buffer, sizeof(buffer), "MQTT: Failed to publish LED %d status to %s: %s", led_num, topic, mosquitto_strerror(ret));
        log_data(buffer);
        free(payload);
        cJSON_Delete(jobj);
        return -1;
    }
    printf("Published LED %d status to %s: %s\n", led_num, topic, payload);
    snprintf(buffer, sizeof(buffer), "MQTT: Published LED %d status to %s: %s", led_num, topic, payload);
    log_data(buffer);
    free(payload);
    cJSON_Delete(jobj);
    return 0;
}

/* --------------------- MQTT CALLBACK & DATA PUBLISHING --------------------- */
void mosquitto_log_callback(struct mosquitto *mosq, void *userdata, int level, const char *str) {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "MQTT: Mosquitto log: [Level %d] %s", level, str);
    log_data(buffer);
}

void mosquitto_message_callback(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *message) {
    char buffer[BUFFER_SIZE];
    log_data("MQTT: Received message");
    cJSON *json = cJSON_Parse(message->payload);
    if(!json) {
        fprintf(stderr, "MQTT: Failed to parse message on %s: %s\n",
                message->topic, cJSON_GetErrorPtr());
        snprintf(buffer, sizeof(buffer), "MQTT: Failed to parse message on %s", message->topic);
        log_data(buffer);
        return;
    }
    /* Xử lý LED1 */
    cJSON *led1_obj = cJSON_GetObjectItem(json, "led1");
    if(led1_obj && cJSON_IsString(led1_obj)) {
        if(strcmp(led1_obj->valuestring, "ON") == 0) {
            set_led_state(1, 1);
            publish_led_status(mosq, 1, "1");
        }
        else if(strcmp(led1_obj->valuestring, "OFF") == 0) {
            set_led_state(1, 0);
            publish_led_status(mosq, 1, "0");
        }
    }
    /* Xử lý LED2: nếu nhận "ON" thì bật nháy liên tục cho đến khi nhận "OFF" */
    cJSON *led2_obj = cJSON_GetObjectItem(json, "led2");
    if(led2_obj && cJSON_IsString(led2_obj)) {
        if(strcmp(led2_obj->valuestring, "ON") == 0) {
            led2_blinking = 1;
        }
        else if(strcmp(led2_obj->valuestring, "OFF") == 0) {
            led2_blinking = 0;
            set_led_state(2, 0);
            publish_led_status(mosq, 2, "0");
        }
    }
    cJSON_Delete(json);
}

int publish_mqtt(struct mosquitto *mosq, const char *topic, const char *payload) {
    log_data("MQTT: Attempting to publish data");
    int ret = mosquitto_publish(mosq, NULL, topic, strlen(payload), payload, MQTT_QOS, false);
    if(ret != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTT: Failed to publish to %s: %s (error code: %d)\n",
                topic, mosquitto_strerror(ret), ret);
        char buffer[BUFFER_SIZE];
        snprintf(buffer, sizeof(buffer),
                 "MQTT: Failed to publish to %s: %s (error code: %d)",
                 topic, mosquitto_strerror(ret), ret);
        log_data(buffer);
        return -1;
    }
    printf("Published to %s: %s\n", topic, payload);
    {
        char buffer[BUFFER_SIZE];
        snprintf(buffer, sizeof(buffer), "MQTT: Published to %s: %s", topic, payload);
        log_data(buffer);
    }
    return 0;
}

/* --------------------- KHỞI TẠO & KẾT NỐI MQTT --------------------- */
int mqtt_init_connect(struct mosquitto **mosq) {
    char buffer[BUFFER_SIZE];
    log_data("MQTT: Initializing");
    mosquitto_lib_init();
    *mosq = mosquitto_new(MQTT_CLIENT_ID, true, NULL);
    if(!*mosq) {
        fprintf(stderr, "MQTT: Failed to create client\n");
        log_data("MQTT: Failed to create client");
        return -1;
    }
    if(mosquitto_username_pw_set(*mosq, MQTT_USER, MQTT_PASS) != MOSQ_ERR_SUCCESS)
        log_data("MQTT: Failed to set username/password");
    mosquitto_log_callback_set(*mosq, mosquitto_log_callback);
    mosquitto_message_callback_set(*mosq, mosquitto_message_callback);
    int ret = mosquitto_connect(*mosq, MQTT_BROKER, MQTT_PORT, 120);
    if(ret != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTT: Failed to connect to broker %s:%d: %s\n",
                MQTT_BROKER, MQTT_PORT, mosquitto_strerror(ret));
        snprintf(buffer, sizeof(buffer), "MQTT: Failed to connect to broker %s:%d: %s",
                 MQTT_BROKER, MQTT_PORT, mosquitto_strerror(ret));
        log_data(buffer);
        mosquitto_destroy(*mosq);
        *mosq = NULL;
        return -1;
    }
    ret = mosquitto_subscribe(*mosq, NULL, MQTT_LED_TOPIC, MQTT_QOS);
    if(ret != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTT: Failed to subscribe to %s: %s\n", MQTT_LED_TOPIC, mosquitto_strerror(ret));
        snprintf(buffer, sizeof(buffer), "MQTT: Failed to subscribe to %s: %s", MQTT_LED_TOPIC, mosquitto_strerror(ret));
        log_data(buffer);
    }
    ret = mosquitto_loop_start(*mosq);
    if(ret != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTT: Failed to start loop: %s\n", mosquitto_strerror(ret));
        snprintf(buffer, sizeof(buffer), "MQTT: Failed to start loop: %s", mosquitto_strerror(ret));
        log_data(buffer);
        mosquitto_destroy(*mosq);
        *mosq = NULL;
        return -1;
    }
    printf("MQTT: Connected to broker\n");
    log_data("MQTT: Connected to broker");
    return 0;
}

int mqtt_reconnect(struct mosquitto **mosq) {
    char buffer[BUFFER_SIZE];
    log_data("MQTT: Attempting to reconnect");
    if(*mosq) {
        mosquitto_loop_stop(*mosq, true);
        mosquitto_disconnect(*mosq);
        mosquitto_destroy(*mosq);
        *mosq = NULL;
    }
    int ret = mqtt_init_connect(mosq);
    if(ret != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTT: Failed to reconnect\n");
        log_data("MQTT: Failed to reconnect");
        return -1;
    }
    return 0;
}

/* --------------------- XỬ LÝ TÍN HIỆU --------------------- */
void signal_handler(int sig, siginfo_t *info, void *context) {
    printf("Received signal %d, shutting down...\n", sig);
    log_data("Received signal, shutting down");
    running = 0;
}

/* --------------------- MAIN --------------------- */
int main(int argc, char *argv[]) {
    char log_buffer[BUFFER_SIZE];
    int led1_state = 0, led2_state = 0; /* LED1 điều khiển từ frontend; LED2- chế độ nháy liên tục khi nhận ON */
    int mqtt_connected = 0;
    time_t last_status_log = 0;
    float temp, humid;
    unsigned int lux = 0;
    char led_status[BUFFER_SIZE];  /* trạng thái LED từ /dev/led */
    int watchdog_fd_local = -1;
    struct mosquitto *mosq_local = NULL;
    
    /* Kiểm tra tham số dòng lệnh, ví dụ: --watchdog */
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--watchdog") == 0) {
            use_watchdog = 1;
            printf("Watchdog enabled via command line\n");
            log_data("Watchdog enabled via command line");
        }
    }
    
    /* Cài đặt xử lý tín hiệu SIGINT, SIGTERM */
    struct sigaction sa;
    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    
    /* Khởi tạo thread giám sát */
    if(pthread_create(&monitor_thread, NULL, monitor_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to create monitor thread: %s\n", strerror(errno));
        log_data("Failed to create monitor thread");
        return -1;
    }
    
    /* Khởi tạo thread DHT11 */
    if(pthread_create(&dht11_thread, NULL, dht11_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to create DHT11 thread: %s\n", strerror(errno));
        log_data("Failed to create DHT11 thread");
        return -1;
    }
    
    /* Khởi tạo watchdog nếu kích hoạt */
    if(init_watchdog(&watchdog_fd_local) != 0) {
        fprintf(stderr, "Failed to initialize watchdog, continuing without watchdog\n");
        log_data("Failed to initialize watchdog, continuing without watchdog");
        use_watchdog = 0;
    }
    
    /* Khởi tạo và kết nối MQTT */
    if(mqtt_init_connect(&mosq_local) == 0)
        mqtt_connected = 1;
    
    printf("Starting sensor system...\n");
    log_data("Starting sensor system");
    
    while(running) {
        last_loop_time = time(NULL);
        ping_watchdog(watchdog_fd_local);
        
        /* Ghi trạng thái hệ thống định kỳ */
        time_t now = time(NULL);
        if(now - last_status_log >= LOG_STATUS_INTERVAL) {
            log_system_status();
            last_status_log = now;
        }
        
        /* Lấy dữ liệu từ DHT11 */
        pthread_mutex_lock(&dht11_mutex);
        temp = last_temp;
        humid = last_humid;
        pthread_mutex_unlock(&dht11_mutex);
        
        /* Đọc dữ liệu từ BH1750 */
        if(read_bh1750(&lux) == 0) {
            snprintf(log_buffer, sizeof(log_buffer), "BH1750: Light: %u lux", lux);
            log_data(log_buffer);
            printf("BH1750: Light: %u lux\n", lux);
            fflush(stdout);
        } else {
            snprintf(log_buffer, sizeof(log_buffer), "BH1750: Failed to read lux");
            log_data(log_buffer);
            printf("BH1750: Failed to read lux\n");
            fflush(stdout);
        }
        
        /* Đọc trạng thái LED từ /dev/led (dùng cho log) */
        if(read_led_status(led_status, sizeof(led_status)) == 0) {
            printf("LED status (from /dev/led): %s\n", led_status);
            fflush(stdout);
            log_data(led_status);
        } else {
            log_data("Failed to read LED status");
        }
        
        /* Gửi dữ liệu cảm biến lên MQTT topic sensors (chỉ gồm temperature, humidity, lux) */
        if(mqtt_connected) {
            cJSON *jobj = cJSON_CreateObject();
            if(jobj) {
                cJSON_AddNumberToObject(jobj, "temperature", temp);
                cJSON_AddNumberToObject(jobj, "humidity", humid);
                cJSON_AddNumberToObject(jobj, "lux", (double)lux);
                char *payload = cJSON_PrintUnformatted(jobj);
                if(payload) {
                    publish_mqtt(mosq_local, MQTT_SENSOR_TOPIC, payload);
                    free(payload);
                }
                cJSON_Delete(jobj);
            }
        }
        
        /* Quản lý LED2: Nếu led2_blinking = 1 thì LED2 nháy liên tục */
        if(led2_blinking) {
            blink_led(mosq_local, 2);
        }
        
        if(mosq_local) {
            log_data("MQTT: Running mosquitto loop");
            int ret = mosquitto_loop(mosq_local, 0, 1);
            if(ret != MOSQ_ERR_SUCCESS) {
                fprintf(stderr, "MQTT: Loop error: %s\n", mosquitto_strerror(ret));
                snprintf(log_buffer, sizeof(log_buffer), "MQTT: Loop error: %s", mosquitto_strerror(ret));
                log_data(log_buffer);
                mqtt_connected = 0;
            }
        }
        sleep(5);
    }
    
    log_data("Cleaning up before exit");
    running = 0;
    pthread_cancel(dht11_thread);
    pthread_cancel(monitor_thread);
    pthread_join(dht11_thread, NULL);
    pthread_join(monitor_thread, NULL);
    pthread_mutex_destroy(&dht11_mutex);
    disable_watchdog(watchdog_fd_local);
    if(mosq_local) {
        mosquitto_loop_stop(mosq_local, true);
        mosquitto_disconnect(mosq_local);
        mosquitto_destroy(mosq_local);
        mosquitto_lib_cleanup();
    }
    log_data("Application terminated gracefully");
    return 0;
}
