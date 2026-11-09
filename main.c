#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"

#define PIN_BANDA      GPIO_NUM_15   // Control relé Banda (RL1 / Q1)
#define PIN_BOMBA      GPIO_NUM_2    // Control relé Bomba (RL3 / Q2)
#define PIN_PISTON1    GPIO_NUM_4    // Control relé Piston 1 (RL4 / Q3)
#define PIN_PISTON2    GPIO_NUM_5    // Control relé Piston 2 (RL5 / Q3)
#define PIN_CALENTADOR GPIO_NUM_18   // Control relé Auxiliar / Calentador (RL2 / Q4)

// Definición de Pines de Entrada (Pulsadores / Sensores, activos en bajo)
#define PIN_START_F    GPIO_NUM_32   // Botón de Inicio Ciclo
#define PIN_STOP_EMG   GPIO_NUM_33   // Paro de Emergencia

// Bits del grupo de eventos del sistema
#define EV_CICLO_ACTIVO BIT0         // Hay un ciclo de sellado en curso
#define EV_EMERGENCIA   BIT1         // Paro de emergencia enclavado
#define EV_START_WEB    BIT2         // Solicitud de inicio de ciclo desde la web

#define PERIODO_SUPERVISION_MS 20
#define REBOTE_MS              50

// --- Salidas indexadas (para la web y el estado espejo) ---
enum { S_BANDA, S_BOMBA, S_PISTON1, S_PISTON2, S_CALENTADOR, N_SALIDAS };
static const gpio_num_t PIN_SALIDA[N_SALIDAS] = {
    PIN_BANDA, PIN_BOMBA, PIN_PISTON1, PIN_PISTON2, PIN_CALENTADOR,
};
static const char *NOMBRE_SALIDA[N_SALIDAS] = {"banda", "bomba", "piston1", "piston2", "calentador"};
static volatile bool estado_salida[N_SALIDAS];  // gpio_get_level no lee pines solo-salida

// --- Etapa del proceso (monitoreo) ---
typedef enum { ET_ESPERA, ET_SUJECION, ET_SELLADO, ET_EVACUACION } etapa_t;
static const char *NOMBRE_ETAPA[] = {"ESPERA", "SUJECION", "SELLADO", "EVACUACION"};
static volatile etapa_t etapa = ET_ESPERA;
static volatile uint32_t ciclos_completos = 0;

// --- Tiempos configurables desde la web (ms), persistidos en NVS ---
typedef struct {
    uint32_t sujecion;   // Calentador + pistón 1
    uint32_t sellado;    // Bomba + pistón 2
    uint32_t evacuacion; // Espera final con banda
    uint32_t banda;      // Tiempo de marcha de la banda
    uint32_t pausa;      // Pausa de la banda entre arranques
} tiempos_t;
static tiempos_t tiempos = {1500, 5000, 3000, 4000, 2000};
#define T_MIN_MS 100
#define T_MAX_MS 60000

#define AP_SSID     CONFIG_FLOWLINE_AP_SSID
#define AP_PASSWORD CONFIG_FLOWLINE_AP_PASSWORD

static EventGroupHandle_t xEventosSistema;

// --- Bitácora circular para la web (además de printf por serie) ---
#define LOG_N   24
#define LOG_LEN 96
static char log_txt[LOG_N][LOG_LEN];
static uint32_t log_ms[LOG_N];
static uint32_t log_seq = 0;  // Total de mensajes escritos
static SemaphoreHandle_t xLogMutex;

static uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void bitacora(const char *fmt, ...) {
    char buf[LOG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    if (xSemaphoreTake(xLogMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint32_t i = log_seq % LOG_N;
        strncpy(log_txt[i], buf, LOG_LEN);
        log_txt[i][LOG_LEN - 1] = '\0';
        log_ms[i] = millis();
        log_seq++;
        xSemaphoreGive(xLogMutex);
    }
}

static void salida(int s, bool on) {
    gpio_set_level(PIN_SALIDA[s], on ? 1 : 0);
    estado_salida[s] = on;
}

static bool emergencia_activa(void) {
    return (xEventGroupGetBits(xEventosSistema) & EV_EMERGENCIA) != 0;
}

// Espera ms en pasos cortos; retorna false si se activa la emergencia durante la espera
static bool esperar_ms(uint32_t ms) {
    const uint32_t paso = 20;
    while (ms > 0) {
        if (emergencia_activa()) {
            return false;
        }
        uint32_t t = (ms < paso) ? ms : paso;
        vTaskDelay(pdMS_TO_TICKS(t));
        ms -= t;
    }
    return !emergencia_activa();
}

static void apagar_todas_las_salidas(void) {
    for (int s = 0; s < N_SALIDAS; s++) {
        salida(s, false);
    }
}

static void activar_emergencia(const char *origen) {
    xEventGroupSetBits(xEventosSistema, EV_EMERGENCIA);
    xEventGroupClearBits(xEventosSistema, EV_START_WEB);
    apagar_todas_las_salidas();
    bitacora("[ALERTA CRITICA] PARO DE EMERGENCIA ACTIVADO (%s)!", origen);
}

// --- TAREA 1: Control de la Banda Transportadora en Paralelo ---
// Solo corre mientras hay un ciclo activo y no hay emergencia
static void vTaskBanda(void *pvParameters) {
    while (true) {
        xEventGroupWaitBits(xEventosSistema, EV_CICLO_ACTIVO, pdFALSE, pdTRUE, portMAX_DELAY);
        if (emergencia_activa()) {
            vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
            continue;
        }

        // Operación de la banda transportadora para evacuación
        bitacora("[BANDA] Arrancando banda transportadora para evacuacion...");
        salida(S_BANDA, true);
        bool ok = esperar_ms(tiempos.banda); // Tiempo que corre la banda

        bitacora("[BANDA] Deteniendo banda...");
        salida(S_BANDA, false);
        if (ok) {
            esperar_ms(tiempos.pausa); // Espera entre ciclos
        }
    }
}

// --- TAREA 2: Secuencia del Proceso (Calentador, Bomba y Pistones) ---
static void vTaskProceso(void *pvParameters) {
    while (true) {
        etapa = ET_ESPERA;
        bitacora("[PROCESO] Esperando inicio de ciclo (START_F o web)...");
        // Espera flanco de bajada en START_F con antirrebote, o solicitud web, sin emergencia
        bool desde_boton = false;
        while (true) {
            if (!emergencia_activa()) {
                if (xEventGroupClearBits(xEventosSistema, EV_START_WEB) & EV_START_WEB) {
                    break;
                }
                if (gpio_get_level(PIN_START_F) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(REBOTE_MS));
                    if (gpio_get_level(PIN_START_F) == 0) {
                        desde_boton = true;
                        break;
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
        }
        // Espera a que se suelte el botón para no encadenar ciclos
        while (desde_boton && gpio_get_level(PIN_START_F) == 0) {
            vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
        }

        apagar_todas_las_salidas(); // Descarta cualquier salida dejada en modo manual
        xEventGroupSetBits(xEventosSistema, EV_CICLO_ACTIVO);

        // Paso 1: Encender Calentador y Activar Pistón 1 (Sujeción / Tirón)
        etapa = ET_SUJECION;
        bitacora("[PROCESO] Encendiendo CALENTADOR y activando PISTON 1...");
        salida(S_CALENTADOR, true); // Calentador ENCENDIDO
        salida(S_PISTON1, true);
        bool ok = esperar_ms(tiempos.sujecion);

        // Paso 2: Activar Bomba y Pistón 2 (Sellado térmico)
        if (ok) {
            etapa = ET_SELLADO;
            bitacora("[PROCESO] Iniciando SELLADO (Bomba y Piston 2 activos por %lu ms)...",
                     (unsigned long)tiempos.sellado);
            salida(S_BOMBA, true);
            salida(S_PISTON2, true);
            ok = esperar_ms(tiempos.sellado);
        }

        // Paso 3: Fin de sellado (o aborto por emergencia). Apagamos actuadores, bomba y calentador.
        bitacora(ok ? "[PROCESO] Fin de sellado. Apagando bomba, pistones y calentador..."
                    : "[PROCESO] Ciclo abortado por emergencia.");
        salida(S_BOMBA, false);
        salida(S_PISTON1, false);
        salida(S_PISTON2, false);
        salida(S_CALENTADOR, false);

        if (ok) {
            etapa = ET_EVACUACION;
            ok = esperar_ms(tiempos.evacuacion); // Deja terminar la evacuación por la banda
            if (ok) {
                ciclos_completos++;
            }
        }
        xEventGroupClearBits(xEventosSistema, EV_CICLO_ACTIVO);
    }
}

// --- TAREA 3: Supervisión de seguridad (máxima prioridad) ---
static void vTaskSeguridad(void *pvParameters) {
    while (true) {
        bool stop_presionado = (gpio_get_level(PIN_STOP_EMG) == 0);

        if (stop_presionado && !emergencia_activa()) {
            activar_emergencia("boton STOP");
        } else if (!stop_presionado && emergencia_activa()) {
            // Rearme: STOP liberado y el operador pulsa START
            if (gpio_get_level(PIN_START_F) == 0) {
                vTaskDelay(pdMS_TO_TICKS(REBOTE_MS));
                while (gpio_get_level(PIN_START_F) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
                }
                xEventGroupClearBits(xEventosSistema, EV_EMERGENCIA);
                bitacora("[SEGURIDAD] Sistema rearmado. Pulse START_F para un nuevo ciclo.");
            }
        }

        if (emergencia_activa()) {
            apagar_todas_las_salidas(); // Refuerza el estado seguro mientras dure la emergencia
        }
        vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
    }
}

// ======================= PERSISTENCIA DE TIEMPOS =======================
static const char *CLAVE_T[] = {"t_suj", "t_sel", "t_evac", "t_banda", "t_pausa"};

static uint32_t *tiempo_por_indice(int i) {
    uint32_t *t[] = {&tiempos.sujecion, &tiempos.sellado, &tiempos.evacuacion,
                     &tiempos.banda, &tiempos.pausa};
    return t[i];
}

static void cargar_tiempos(void) {
    nvs_handle_t h;
    if (nvs_open("flowline", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    for (int i = 0; i < 5; i++) {
        uint32_t v;
        if (nvs_get_u32(h, CLAVE_T[i], &v) == ESP_OK && v >= T_MIN_MS && v <= T_MAX_MS) {
            *tiempo_por_indice(i) = v;
        }
    }
    nvs_close(h);
}

static void guardar_tiempos(void) {
    nvs_handle_t h;
    if (nvs_open("flowline", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    for (int i = 0; i < 5; i++) {
        nvs_set_u32(h, CLAVE_T[i], *tiempo_por_indice(i));
    }
    nvs_commit(h);
    nvs_close(h);
}

// ======================= WEB APP (offline, servida por el ESP32) =======================
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t h_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_estado(httpd_req_t *req) {
    static char json[4096];  // Solo lo usa la tarea del servidor HTTP
    EventBits_t ev = xEventGroupGetBits(xEventosSistema);
    wifi_sta_list_t sta = {0};
    esp_wifi_ap_get_sta_list(&sta);
    int n = snprintf(json, sizeof(json),
        "{\"etapa\":\"%s\",\"ciclo\":%d,\"emergencia\":%d,\"stop\":%d,\"start\":%d,"
        "\"ciclos\":%lu,\"uptime\":%lu,\"clientes\":%d,\"salidas\":{",
        NOMBRE_ETAPA[etapa], (ev & EV_CICLO_ACTIVO) ? 1 : 0, (ev & EV_EMERGENCIA) ? 1 : 0,
        gpio_get_level(PIN_STOP_EMG) == 0, gpio_get_level(PIN_START_F) == 0,
        (unsigned long)ciclos_completos, (unsigned long)millis(), sta.num);
    for (int s = 0; s < N_SALIDAS; s++) {
        n += snprintf(json + n, sizeof(json) - n, "%s\"%s\":%d", s ? "," : "",
                      NOMBRE_SALIDA[s], estado_salida[s] ? 1 : 0);
    }
    n += snprintf(json + n, sizeof(json) - n,
                  "},\"tiempos\":{\"sujecion\":%lu,\"sellado\":%lu,\"evacuacion\":%lu,"
                  "\"banda\":%lu,\"pausa\":%lu},\"logSeq\":%lu,\"log\":[",
                  (unsigned long)tiempos.sujecion, (unsigned long)tiempos.sellado,
                  (unsigned long)tiempos.evacuacion, (unsigned long)tiempos.banda,
                  (unsigned long)tiempos.pausa, (unsigned long)log_seq);
    if (xSemaphoreTake(xLogMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint32_t total = log_seq < LOG_N ? log_seq : LOG_N;
        for (uint32_t k = 0; k < total && n < (int)sizeof(json) - LOG_LEN - 32; k++) {
            uint32_t i = (log_seq - total + k) % LOG_N;
            n += snprintf(json + n, sizeof(json) - n, "%s[%lu,\"", k ? "," : "",
                          (unsigned long)log_ms[i]);
            for (const char *c = log_txt[i]; *c && n < (int)sizeof(json) - 8; c++) {
                if (*c == '"' || *c == '\\') {
                    json[n++] = '\\';
                }
                json[n++] = (*c >= 0x20) ? *c : ' ';
            }
            n += snprintf(json + n, sizeof(json) - n, "\"]");
        }
        xSemaphoreGive(xLogMutex);
    }
    n += snprintf(json + n, sizeof(json) - n, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

static esp_err_t responder(httpd_req_t *req, bool ok, const char *msg) {
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"msg\":\"%s\"}", ok ? "true" : "false", msg);
    httpd_resp_set_type(req, "application/json");
    if (!ok) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    return httpd_resp_sendstr(req, buf);
}

// POST /api/cmd?a=start|paro|rearme|salida&s=<nombre>&v=0|1
static esp_err_t h_cmd(httpd_req_t *req) {
    char q[96] = "", a[16] = "", s[16] = "", v[4] = "";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "a", a, sizeof(a));
    EventBits_t ev = xEventGroupGetBits(xEventosSistema);

    if (strcmp(a, "start") == 0) {
        if (ev & EV_EMERGENCIA) return responder(req, false, "Emergencia activa: rearme primero");
        if (ev & EV_CICLO_ACTIVO) return responder(req, false, "Ya hay un ciclo en curso");
        xEventGroupSetBits(xEventosSistema, EV_START_WEB);
        bitacora("[WEB] Inicio de ciclo solicitado");
        return responder(req, true, "Ciclo solicitado");
    }
    if (strcmp(a, "paro") == 0) {
        if (!(ev & EV_EMERGENCIA)) activar_emergencia("web");
        return responder(req, true, "Paro de emergencia activado");
    }
    if (strcmp(a, "rearme") == 0) {
        if (!(ev & EV_EMERGENCIA)) return responder(req, true, "No hay emergencia");
        if (gpio_get_level(PIN_STOP_EMG) == 0) return responder(req, false, "Libere el boton STOP fisico");
        xEventGroupClearBits(xEventosSistema, EV_EMERGENCIA);
        bitacora("[SEGURIDAD] Sistema rearmado desde la web.");
        return responder(req, true, "Sistema rearmado");
    }
    if (strcmp(a, "salida") == 0) {
        if (ev & EV_EMERGENCIA) return responder(req, false, "Emergencia activa");
        if (ev & EV_CICLO_ACTIVO) return responder(req, false, "Modo manual bloqueado durante el ciclo");
        httpd_query_key_value(q, "s", s, sizeof(s));
        httpd_query_key_value(q, "v", v, sizeof(v));
        for (int i = 0; i < N_SALIDAS; i++) {
            if (strcmp(s, NOMBRE_SALIDA[i]) == 0) {
                salida(i, v[0] == '1');
                bitacora("[MANUAL] %s -> %s", NOMBRE_SALIDA[i], v[0] == '1' ? "ON" : "OFF");
                return responder(req, true, "OK");
            }
        }
        return responder(req, false, "Salida desconocida");
    }
    return responder(req, false, "Accion desconocida");
}

// POST /api/config?sujecion=..&sellado=..&evacuacion=..&banda=..&pausa=..
static esp_err_t h_config(httpd_req_t *req) {
    static const char *NOMBRE_T[] = {"sujecion", "sellado", "evacuacion", "banda", "pausa"};
    if (xEventGroupGetBits(xEventosSistema) & EV_CICLO_ACTIVO) {
        return responder(req, false, "Espere a que termine el ciclo");
    }
    char q[160] = "", v[12];
    httpd_req_get_url_query_str(req, q, sizeof(q));
    uint32_t nuevos[5];
    for (int i = 0; i < 5; i++) {
        nuevos[i] = *tiempo_por_indice(i);
        if (httpd_query_key_value(q, NOMBRE_T[i], v, sizeof(v)) == ESP_OK) {
            long x = strtol(v, NULL, 10);
            if (x < T_MIN_MS || x > T_MAX_MS) {
                return responder(req, false, "Tiempos fuera de rango (100-60000 ms)");
            }
            nuevos[i] = (uint32_t)x;
        }
    }
    for (int i = 0; i < 5; i++) {
        *tiempo_por_indice(i) = nuevos[i];
    }
    guardar_tiempos();
    bitacora("[CONFIG] Tiempos: suj=%lu sel=%lu evac=%lu banda=%lu pausa=%lu",
             (unsigned long)nuevos[0], (unsigned long)nuevos[1], (unsigned long)nuevos[2],
             (unsigned long)nuevos[3], (unsigned long)nuevos[4]);
    return responder(req, true, "Tiempos guardados");
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
            .channel = 1,
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
    bitacora("[WIFI] Red \"%s\" activa. Abra http://192.168.4.1", AP_SSID);
}

static void iniciar_servidor_web(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        bitacora("[WEB] No se pudo iniciar el servidor HTTP");
        return;
    }
    const httpd_uri_t rutas[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/estado", .method = HTTP_GET, .handler = h_estado},
        {.uri = "/api/cmd", .method = HTTP_POST, .handler = h_cmd},
        {.uri = "/api/config", .method = HTTP_POST, .handler = h_config},
    };
    for (size_t i = 0; i < sizeof(rutas) / sizeof(rutas[0]); i++) {
        httpd_register_uri_handler(srv, &rutas[i]);
    }
}

// --- FUNCIÓN PRINCIPAL (APP_MAIN) ---
void app_main(void) {
    printf("=== INICIANDO SISTEMA MULTITAREA FREERTOS - SELLADORA ===\n");

    // Salidas: gpio_config hace reset del IOMUX (GPIO15 arranca como MTDO/JTAG)
    gpio_config_t salidas = {
        .pin_bit_mask = (1ULL << PIN_BANDA) | (1ULL << PIN_BOMBA) | (1ULL << PIN_PISTON1) |
                        (1ULL << PIN_PISTON2) | (1ULL << PIN_CALENTADOR),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&salidas);
    apagar_todas_las_salidas();

    // Configuración de entradas con resistencias Pull-Up internas
    gpio_config_t entradas = {
        .pin_bit_mask = (1ULL << PIN_START_F) | (1ULL << PIN_STOP_EMG),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&entradas);

    xEventosSistema = xEventGroupCreate();
    xLogMutex = xSemaphoreCreateMutex();
    configASSERT(xEventosSistema && xLogMutex);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    cargar_tiempos();

    // Creación de tareas independientes en FreeRTOS (stack en bytes; printf necesita margen)
    xTaskCreate(vTaskSeguridad, "Task_Seguridad", 3072, NULL, 5, NULL);
    xTaskCreate(vTaskProceso, "Task_Proceso", 4096, NULL, 3, NULL);
    xTaskCreate(vTaskBanda, "Task_Banda", 4096, NULL, 2, NULL);

    // Monitoreo y control: red Wi-Fi propia + servidor HTTP (tarea httpd, prioridad 5 por defecto)
    iniciar_wifi_ap();
    iniciar_servidor_web();
}
