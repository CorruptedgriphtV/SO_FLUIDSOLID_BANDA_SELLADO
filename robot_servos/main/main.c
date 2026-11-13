#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

// --- Brazo de 5 grados de libertad (señal de 50 Hz, pulso de 0.5 a 2.4 ms para 0 a 180°) ---
enum { J_BASE, J_HOMBRO, J_CODO, J_MUNECA, J_PINZA, N_SERVOS };
static const gpio_num_t PIN_SERVO[N_SERVOS] = {
    GPIO_NUM_23,  // J1 base   (D23)
    GPIO_NUM_22,  // J2 hombro (D22)
    GPIO_NUM_19,  // J3 codo   (D19)
    GPIO_NUM_18,  // J4 muñeca (D18)
    GPIO_NUM_15,  // J5 pinza  (D15)
};
static const char *NOMBRE_SERVO[N_SERVOS] = {"base", "hombro", "codo", "muneca", "pinza"};
static const ledc_channel_t CANAL_SERVO[N_SERVOS] = {
    LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3, LEDC_CHANNEL_4,
};
#define SERVO_FREQ_HZ  50
#define SERVO_RES      LEDC_TIMER_14_BIT
#define SERVO_DUTY_MAX ((1u << 14) - 1)
#define SERVO_PERIODO_US 20000
#define SERVO_MIN_US   500           // Pulso para 0°
#define SERVO_MAX_US   2400          // Pulso para 180°

// Posición de reposo (home): se aplica solo con el botón Home de la web.
// Al arrancar no sale ningún pulso PWM (pin en LOW): cada servo se queda donde está hasta su primera orden.
static const uint8_t HOME[N_SERVOS] = {90, 90, 90, 90, 90};
#define SIN_ORDEN -1                 // Posición reportada mientras el servo no ha recibido ninguna orden

// Movimiento suave: cada articulación avanza hacia su objetivo a velocidad limitada.
// Evita golpes en la estructura y picos de corriente que reinician el ESP32.
#define PERIODO_MOV_MS   20          // Un paso por cada pulso del servo
#define VEL_GRADOS_S     90          // Velocidad máxima de cada articulación
#define PASO_DECIMAS     (VEL_GRADOS_S * PERIODO_MOV_MS / 100)  // Décimas de grado por paso

// --- Red Wi-Fi propia del ESP32 (sin internet) ---
#define AP_SSID     "Robot-Pinza"
#define AP_PASSWORD "robot1234"      // 8+ caracteres; vacía = red abierta

typedef struct {
    uint8_t servo;   // 0..N_SERVOS-1
    uint8_t grados;  // 0..180
} orden_t;

// Posiciones en décimas de grado (0..1800). actual = lo que recibe el servo; objetivo = lo pedido.
static volatile int actual[N_SERVOS] = {SIN_ORDEN, SIN_ORDEN, SIN_ORDEN, SIN_ORDEN, SIN_ORDEN};
static volatile int objetivo[N_SERVOS] = {SIN_ORDEN, SIN_ORDEN, SIN_ORDEN, SIN_ORDEN, SIN_ORDEN};
static QueueHandle_t xOrdenes;

static uint32_t duty_de_decimas(int decimas) {
    uint32_t us = SERVO_MIN_US + (uint32_t)(SERVO_MAX_US - SERVO_MIN_US) * decimas / 1800;
    return us * SERVO_DUTY_MAX / SERVO_PERIODO_US;
}

static void aplicar(int s, int decimas) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, CANAL_SERVO[s], duty_de_decimas(decimas));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, CANAL_SERVO[s]);
    actual[s] = decimas;
}

// Primera orden: conecta el PWM al pin directamente en el ángulo pedido. No se sabe dónde está el
// servo, así que no hay desde dónde suavizar; las órdenes siguientes ya se mueven a velocidad limitada.
static void activar(int s, int decimas) {
    ledc_channel_config_t canal = {
        .gpio_num = PIN_SERVO[s],
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = CANAL_SERVO[s],
        .timer_sel = LEDC_TIMER_0,
        .duty = duty_de_decimas(decimas),
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&canal));
    actual[s] = decimas;
}

// --- TAREA: recibe órdenes de la web y mueve cada articulación paso a paso hacia su objetivo ---
static void vTaskServos(void *pvParameters) {
    TickType_t ultimo = xTaskGetTickCount();
    orden_t o;
    while (true) {
        while (xQueueReceive(xOrdenes, &o, 0) == pdTRUE) {
            objetivo[o.servo] = o.grados * 10;
            if (actual[o.servo] == SIN_ORDEN) {
                activar(o.servo, objetivo[o.servo]);
            }
            printf("[SERVO] J%d %s -> %d grados\n", o.servo + 1, NOMBRE_SERVO[o.servo], o.grados);
        }
        for (int s = 0; s < N_SERVOS; s++) {
            int a = actual[s], obj = objetivo[s];
            if (a == SIN_ORDEN || a == obj) {
                continue;
            }
            int d = obj - a;
            if (d > PASO_DECIMAS) d = PASO_DECIMAS;
            if (d < -PASO_DECIMAS) d = -PASO_DECIMAS;
            aplicar(s, a + d);
        }
        vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(PERIODO_MOV_MS));
    }
}

static bool enviar_orden(int s, int grados) {
    orden_t o = {.servo = (uint8_t)s, .grados = (uint8_t)grados};
    return xQueueSend(xOrdenes, &o, pdMS_TO_TICKS(50)) == pdTRUE;
}

// ======================= WEB APP (servida por el ESP32) =======================
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t h_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

// Grados enteros redondeados, o -1 si el servo no ha recibido orden
static int a_grados(int decimas) {
    return decimas == SIN_ORDEN ? SIN_ORDEN : (decimas + 5) / 10;
}

// {"ok":..,"msg":"..","pos":[actual..],"obj":[objetivo..],"home":[..]}
static esp_err_t responder_pos(httpd_req_t *req, bool ok, const char *msg) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":%s,\"msg\":\"%s\",\"pos\":[", ok ? "true" : "false", msg);
    for (int s = 0; s < N_SERVOS; s++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s%d", s ? "," : "", a_grados(actual[s]));
    }
    n += snprintf(buf + n, sizeof(buf) - n, "],\"obj\":[");
    for (int s = 0; s < N_SERVOS; s++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s%d", s ? "," : "", a_grados(objetivo[s]));
    }
    n += snprintf(buf + n, sizeof(buf) - n, "],\"home\":[");
    for (int s = 0; s < N_SERVOS; s++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s%d", s ? "," : "", HOME[s]);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    return httpd_resp_send(req, buf, n);
}

// GET /api/pos
static esp_err_t h_pos(httpd_req_t *req) {
    return responder_pos(req, true, "");
}

// POST /api/servo?s=1..5&g=0..180
static esp_err_t h_servo(httpd_req_t *req) {
    char q[48] = "", s[4] = "", g[8] = "";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "s", s, sizeof(s));
    httpd_query_key_value(q, "g", g, sizeof(g));
    int servo = atoi(s) - 1;
    char *fin;
    long grados = strtol(g, &fin, 10);
    if (servo < 0 || servo >= N_SERVOS) return responder_pos(req, false, "Servo invalido (1 a 5)");
    if (fin == g || *fin || grados < 0 || grados > 180) return responder_pos(req, false, "Grados 0 a 180");
    if (!enviar_orden(servo, (int)grados)) return responder_pos(req, false, "Cola llena");
    return responder_pos(req, true, "OK");
}

// POST /api/home
static esp_err_t h_home(httpd_req_t *req) {
    bool ok = true;
    for (int s = 0; s < N_SERVOS; s++) {
        ok &= enviar_orden(s, HOME[s]);
    }
    return responder_pos(req, ok, ok ? "Home" : "Cola llena: repita Home");
}

static void iniciar_wifi_ap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t ap = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .password = AP_PASSWORD,
            .channel = 6,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(AP_PASSWORD) < 8) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    printf("[WIFI] Red \"%s\" activa. Abra http://192.168.4.1\n", AP_SSID);
}

static void iniciar_servidor_web(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        printf("[WEB] No se pudo iniciar el servidor HTTP\n");
        return;
    }
    const httpd_uri_t rutas[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/pos", .method = HTTP_GET, .handler = h_pos},
        {.uri = "/api/servo", .method = HTTP_POST, .handler = h_servo},
        {.uri = "/api/home", .method = HTTP_POST, .handler = h_home},
    };
    for (size_t i = 0; i < sizeof(rutas) / sizeof(rutas[0]); i++) {
        httpd_register_uri_handler(srv, &rutas[i]);
    }
}

void app_main(void) {
    // Pines de señal en LOW fijo: sin pulsos los servos no se mueven (quedan sueltos donde están)
    uint64_t mascara = 0;
    for (int s = 0; s < N_SERVOS; s++) {
        mascara |= 1ULL << PIN_SERVO[s];
    }
    gpio_config_t pines = {
        .pin_bit_mask = mascara,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pines);
    for (int s = 0; s < N_SERVOS; s++) {
        gpio_set_level(PIN_SERVO[s], 0);
    }

    // El temporizador PWM queda listo; cada canal se conecta a su pin con la primera orden
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    xOrdenes = xQueueCreate(16, sizeof(orden_t));
    configASSERT(xOrdenes);
    xTaskCreate(vTaskServos, "Task_Servos", 3072, NULL, 4, NULL);

    esp_err_t err = nvs_flash_init();  // El Wi-Fi necesita NVS
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    iniciar_wifi_ap();
    iniciar_servidor_web();
    printf("LISTO brazo 5 DOF: base D23, hombro D22, codo D19, muneca D18, pinza D15. Sin senal hasta la primera orden\n");
}
