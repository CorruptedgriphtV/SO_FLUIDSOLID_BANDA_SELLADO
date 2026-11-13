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

#define PIN_BANDA      GPIO_NUM_15   // Control relé Banda (RL1 / Q1) (D15)
#define PIN_BOMBA      GPIO_NUM_2    // Control relé Bomba (RL3 / Q2) (D2)
#define PIN_PISTON1    GPIO_NUM_4    // Control relé Piston 1 (RL4 / Q3)
#define PIN_PISTON2    GPIO_NUM_5    // Control relé Piston 2 (RL5 / Q3)
#define PIN_CALENTADOR GPIO_NUM_23   // Control relé Auxiliar / Calentador (RL2 / Q4) (D23)

// Definición de Pines de Entrada (Pulsadores / Sensores, activos en bajo)
// Salidas, botones y sensores van en la hilera de 3V3/GND (D15 D2 D4 RX2 TX2 D5 D18 D19 D23);
// el relé del compresor (D26) está en la hilera opuesta
#define PIN_START_F    GPIO_NUM_19   // Botón de Inicio Ciclo (D19)
#define PIN_STOP_EMG   GPIO_NUM_17   // Paro de Emergencia (TX2 = GPIO17)

// Sensores ópticos NPN (tipo E18-D80NK): la salida baja a LOW cuando detectan un objeto
#define PIN_SENSOR_1   GPIO_NUM_16   // S1: sensor de bolsa en la estación de llenado (RX2 = GPIO16)
#define PIN_SENSOR_2   GPIO_NUM_18   // S2: sensor de bolsa al final de la banda (D18)

// --- Compresor: relé del módulo de 4 canales (activo en LOW, drenador abierto) ---
// Sin sensor de presión, el compresor se queda en marcha aunque ya no suba la presión y se calienta.
// Por eso la marcha está limitada a comp_marcha_s y después descansa comp_reposo_s. El reposo es
// proporcional a lo que marchó, así que apagarlo y volverlo a encender no se salta el enfriamiento.
#define PIN_COMPRESOR      GPIO_NUM_26  // Relé del compresor (D26)
#define COMP_MARCHA_DEF_S  60           // Tiempo máximo de marcha continua
#define COMP_REPOSO_DEF_S  120          // Descanso tras una marcha completa
#define COMP_MARCHA_MIN_S  5
#define COMP_MARCHA_MAX_S  600
#define COMP_REPOSO_MIN_S  10
#define COMP_REPOSO_MAX_S  3600
#define COMP_PERIODO_MS    100
#define T_DETECCION_MS 300           // Para iniciar un paso, la detección debe mantenerse estable este tiempo
#define T_DETECCION_FIN_MS 50        // Para terminar un paso (p. ej. detener la banda) basta este tiempo

// Bits del grupo de eventos del sistema
#define EV_CICLO_ACTIVO BIT0         // Hay un ciclo de sellado en curso
#define EV_EMERGENCIA   BIT1         // Paro de emergencia enclavado
#define EV_START_WEB    BIT2         // Solicitud de inicio de ciclo desde la web
#define EV_AUTO         BIT3         // Modo automático: encadena ciclos hasta apagarlo o un paro

#define PERIODO_SUPERVISION_MS 20
#define REBOTE_MS              50

// --- Salidas indexadas (para la web y el estado espejo) ---
enum { S_BANDA, S_BOMBA, S_PISTON1, S_PISTON2, S_CALENTADOR, N_SALIDAS };
static const gpio_num_t PIN_SALIDA[N_SALIDAS] = {
    PIN_BANDA, PIN_BOMBA, PIN_PISTON1, PIN_PISTON2, PIN_CALENTADOR,
};
static const char *NOMBRE_SALIDA[N_SALIDAS] = {"banda", "bomba", "piston1", "piston2", "calentador"};
// Lógica inversa: true = la salida se activa con LOW.
// - Pistones y calentador: módulo de 4 relés optoacoplado (5 V), activo en LOW, en drenador abierto.
// - Banda y bomba: otro módulo de relés con entrada por transistor, activo en HIGH, en push-pull (3.3 V).
static const bool ACTIVA_EN_BAJO[N_SALIDAS] = {false, false, true, true, true};
static volatile bool estado_salida[N_SALIDAS];  // Estado lógico (true = activada), no el nivel del pin

// --- Línea de tiempo del ciclo: lista de pasos editable desde la web, persistida en NVS ---
// Cada paso enciende las salidas de su máscara (bit i = salida i) durante ms y apaga las demás.
// Al terminar el último paso se apaga todo. Cambiar el orden no requiere reprogramar.
// Un paso puede esperar a los sensores ópticos antes de encender sus salidas (condicion), y puede
// terminar antes de su tiempo cuando un sensor detecta (fin); en ese caso ms es el tiempo máximo.
#define MAX_PASOS 8
#define T_MIN_MS  100
#define T_MAX_MS  60000
enum { C_NINGUNA, C_S1, C_S2, C_AMBOS, C_CUALQUIERA, N_CONDICIONES };
static const char *NOMBRE_CONDICION[N_CONDICIONES] = {"", "BOLSA", "FIN DE BANDA", "BOLSA y FIN", "BOLSA o FIN"};
typedef struct {
    uint8_t mascara;
    uint32_t ms;
    uint8_t condicion;  // C_*: qué sensores deben detectar antes de iniciar el paso
    uint8_t fin;        // C_*: qué sensores terminan el paso antes de ms (C_NINGUNA = solo por tiempo)
} paso_t;
typedef struct {
    uint8_t n;
    paso_t paso[MAX_PASOS];
} linea_t;
#define M(s) (1u << (s))
static linea_t linea = {
    .n = 3,
    .paso = {
        {M(S_BOMBA), 3000, C_S1, C_NINGUNA},                    // 1. Espera bolsa, bomba (dosificación), luego se apaga
        {M(S_PISTON1) | M(S_PISTON2), 4000, C_NINGUNA, C_NINGUNA}, // 2. Ambos pistones (sellado), luego se apagan
        {M(S_BANDA), 15000, C_NINGUNA, C_S2},                   // 3. Banda hasta que la bolsa llega al final (máx 15 s)
    },
};
static SemaphoreHandle_t xLineaMutex;

// --- Monitoreo del ciclo ---
static volatile int paso_actual = -1;     // -1 = en espera
static volatile uint32_t paso_t0 = 0;     // millis() al iniciar el paso actual
static volatile bool esperando_sensores = false;  // El paso actual aún espera su condición
static volatile uint32_t ciclos_completos = 0;

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
    gpio_set_level(PIN_SALIDA[s], (on != ACTIVA_EN_BAJO[s]) ? 1 : 0);
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

static bool sensor_1(void) {
    return gpio_get_level(PIN_SENSOR_1) == 0;  // NPN: LOW = objeto detectado
}

static bool sensor_2(void) {
    return gpio_get_level(PIN_SENSOR_2) == 0;
}

static bool condicion_cumplida(uint8_t c) {
    switch (c) {
        case C_S1: return sensor_1();
        case C_S2: return sensor_2();
        case C_AMBOS: return sensor_1() && sensor_2();
        case C_CUALQUIERA: return sensor_1() || sensor_2();
        default: return true;
    }
}

// Espera a que la condición se mantenga T_DETECCION_MS seguidos; retorna false si hay emergencia
static bool esperar_condicion(uint8_t c) {
    if (c == C_NINGUNA) {
        return true;
    }
    uint32_t estable = 0;
    while (estable < T_DETECCION_MS) {
        if (emergencia_activa()) {
            return false;
        }
        estable = condicion_cumplida(c) ? estable + PERIODO_SUPERVISION_MS : 0;
        vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
    }
    return !emergencia_activa();
}

// Resultado de esperar_fin
enum { FIN_EMERGENCIA = -1, FIN_TIEMPO = 0, FIN_SENSOR = 1 };

// Espera hasta max_ms o hasta que el sensor de fin detecte. Solo cuenta una detección "nueva":
// si el sensor ya detectaba al empezar (p. ej. la bolsa anterior sigue al final), primero debe liberarse.
static int esperar_fin(uint32_t max_ms, uint8_t c) {
    uint32_t t = 0, estable = 0;
    bool armado = !condicion_cumplida(c);
    while (t < max_ms) {
        if (emergencia_activa()) {
            return FIN_EMERGENCIA;
        }
        bool det = condicion_cumplida(c);
        if (!det) {
            armado = true;
        }
        estable = (armado && det) ? estable + PERIODO_SUPERVISION_MS : 0;
        if (estable >= T_DETECCION_FIN_MS) {
            return FIN_SENSOR;
        }
        vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
        t += PERIODO_SUPERVISION_MS;
    }
    return emergencia_activa() ? FIN_EMERGENCIA : FIN_TIEMPO;
}

static void apagar_todas_las_salidas(void) {
    for (int s = 0; s < N_SALIDAS; s++) {
        salida(s, false);
    }
}

// Deja encendidas solo las salidas de la máscara
static void aplicar_mascara(uint8_t mascara) {
    for (int s = 0; s < N_SALIDAS; s++) {
        salida(s, (mascara & M(s)) != 0);
    }
}

// Texto "bomba+piston1" para la bitácora
static void nombres_mascara(uint8_t mascara, char *buf, size_t len) {
    buf[0] = '\0';
    for (int s = 0; s < N_SALIDAS; s++) {
        if (mascara & M(s)) {
            if (buf[0]) strncat(buf, "+", len - strlen(buf) - 1);
            strncat(buf, NOMBRE_SALIDA[s], len - strlen(buf) - 1);
        }
    }
    if (!buf[0]) strncpy(buf, "pausa", len);
}

// --- Estado del compresor ---
typedef enum { COMP_APAGADO, COMP_MARCHA, COMP_REPOSO } fase_comp_t;
static const char *NOMBRE_FASE_COMP[] = {"APAGADO", "MARCHA", "REPOSO"};
static volatile bool comp_habilitado = false;     // No se guarda: al arrancar el compresor queda apagado
static volatile fase_comp_t comp_fase = COMP_APAGADO;
static volatile uint32_t comp_t0 = 0;             // millis() al arrancar la marcha actual
static volatile uint32_t comp_reposo_hasta = 0;   // millis() hasta el que debe seguir enfriándose
static volatile uint32_t comp_marcha_s = COMP_MARCHA_DEF_S;
static volatile uint32_t comp_reposo_s = COMP_REPOSO_DEF_S;

static void rele_compresor(bool on) {
    gpio_set_level(PIN_COMPRESOR, on ? 0 : 1);  // LOW = relé activado; 1 = pin flotante (apagado)
}

static void activar_emergencia(const char *origen) {
    xEventGroupSetBits(xEventosSistema, EV_EMERGENCIA);
    xEventGroupClearBits(xEventosSistema, EV_START_WEB | EV_AUTO);  // El paro también cancela el modo automático
    apagar_todas_las_salidas();
    rele_compresor(false);  // Task_Compresor registra el reposo en su siguiente paso
    comp_habilitado = false;
    bitacora("[ALERTA CRITICA] PARO DE EMERGENCIA ACTIVADO (%s)!", origen);
}

// --- TAREA 1: Secuencia del Proceso según la línea de tiempo ---
static void vTaskProceso(void *pvParameters) {
    while (true) {
        paso_actual = -1;
        if (!(xEventGroupGetBits(xEventosSistema) & EV_AUTO)) {
            bitacora("[PROCESO] Esperando inicio de ciclo (START_F, web o modo automatico)...");
        }
        // Espera flanco de bajada en START_F con antirrebote, solicitud web o modo automático, sin emergencia
        bool desde_boton = false;
        while (true) {
            if (!emergencia_activa()) {
                if (xEventGroupClearBits(xEventosSistema, EV_START_WEB) & EV_START_WEB) {
                    break;
                }
                if (xEventGroupGetBits(xEventosSistema) & EV_AUTO) {
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

        // Copia local: la web no puede cambiar la línea a mitad de un ciclo
        linea_t l;
        xSemaphoreTake(xLineaMutex, portMAX_DELAY);
        l = linea;
        xSemaphoreGive(xLineaMutex);

        apagar_todas_las_salidas(); // Descarta cualquier salida dejada en modo manual
        xEventGroupSetBits(xEventosSistema, EV_CICLO_ACTIVO);

        bool ok = true;
        const char *motivo = "emergencia";
        for (int i = 0; i < l.n && ok; i++) {
            char nombres[64];
            nombres_mascara(l.paso[i].mascara, nombres, sizeof(nombres));
            paso_t0 = millis();
            paso_actual = i;
            if (l.paso[i].condicion != C_NINGUNA) {
                esperando_sensores = true;
                bitacora("[PROCESO] Paso %d/%d: esperando sensores %s...", i + 1, l.n,
                         NOMBRE_CONDICION[l.paso[i].condicion]);
                ok = esperar_condicion(l.paso[i].condicion);
                esperando_sensores = false;
                if (!ok) {
                    break;
                }
                paso_t0 = millis();
            }
            aplicar_mascara(l.paso[i].mascara);
            if (l.paso[i].fin == C_NINGUNA) {
                bitacora("[PROCESO] Paso %d/%d: %s ON por %lu ms", i + 1, l.n, nombres,
                         (unsigned long)l.paso[i].ms);
                ok = esperar_ms(l.paso[i].ms);
            } else {
                bitacora("[PROCESO] Paso %d/%d: %s ON hasta %s (max %lu ms)", i + 1, l.n, nombres,
                         NOMBRE_CONDICION[l.paso[i].fin], (unsigned long)l.paso[i].ms);
                int r = esperar_fin(l.paso[i].ms, l.paso[i].fin);
                if (r == FIN_TIEMPO) {
                    // La bolsa no llegó a tiempo (atasco o caída): se detiene y se apaga el automático
                    motivo = "tiempo maximo sin deteccion";
                    xEventGroupClearBits(xEventosSistema, EV_AUTO);
                    bitacora("[ALERTA] Paso %d: %s no detecto en %lu ms. Automatico apagado.", i + 1,
                             NOMBRE_CONDICION[l.paso[i].fin], (unsigned long)l.paso[i].ms);
                }
                ok = (r == FIN_SENSOR);
            }
            apagar_todas_las_salidas(); // Cada paso termina apagando lo que encendió
        }

        if (ok) {
            ciclos_completos++;
            bitacora("[PROCESO] Ciclo %lu completo.", (unsigned long)ciclos_completos);
        } else {
            bitacora("[PROCESO] Ciclo abortado por %s.", motivo);
        }
        xEventGroupClearBits(xEventosSistema, EV_CICLO_ACTIVO);
    }
}

// --- TAREA 2: Supervisión de seguridad (máxima prioridad) ---
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

// Detiene la marcha y calcula el enfriamiento proporcional a lo que estuvo encendido
static void detener_compresor(uint32_t ahora, const char *motivo) {
    rele_compresor(false);
    uint32_t marcho_ms = ahora - comp_t0;
    uint64_t reposo_ms = (uint64_t)comp_reposo_s * 1000 * marcho_ms / ((uint64_t)comp_marcha_s * 1000);
    comp_reposo_hasta = ahora + (uint32_t)reposo_ms;
    bitacora("[COMPRESOR] OFF (%s) tras %lu s. Enfriando %lu s", motivo, (unsigned long)(marcho_ms / 1000),
             (unsigned long)(reposo_ms / 1000));
}

// --- TAREA 3: Compresor con límite de marcha y descanso obligatorio ---
static void vTaskCompresor(void *pvParameters) {
    while (true) {
        uint32_t ahora = millis();
        bool puede = comp_habilitado && !emergencia_activa();
        if (!puede) {
            if (comp_fase == COMP_MARCHA) {
                detener_compresor(ahora, emergencia_activa() ? "emergencia" : "apagado desde la web");
            }
            rele_compresor(false);
            comp_fase = COMP_APAGADO;
        } else if (comp_fase == COMP_MARCHA) {
            if (ahora - comp_t0 >= comp_marcha_s * 1000) {
                detener_compresor(ahora, "tiempo maximo de marcha");
                comp_fase = COMP_REPOSO;
            }
        } else if ((int32_t)(ahora - comp_reposo_hasta) >= 0) {
            comp_t0 = ahora;
            comp_fase = COMP_MARCHA;
            rele_compresor(true);
            bitacora("[COMPRESOR] ON (maximo %lu s)", (unsigned long)comp_marcha_s);
        } else {
            comp_fase = COMP_REPOSO;  // Habilitado, pero aún enfriándose
        }
        vTaskDelay(pdMS_TO_TICKS(COMP_PERIODO_MS));
    }
}

// Segundos que faltan para que termine la fase actual (marcha o reposo)
static uint32_t comp_restante_s(void) {
    uint32_t ahora = millis();
    if (comp_fase == COMP_MARCHA) {
        uint32_t t = ahora - comp_t0;
        return t >= comp_marcha_s * 1000 ? 0 : (comp_marcha_s * 1000 - t + 999) / 1000;
    }
    int32_t falta = (int32_t)(comp_reposo_hasta - ahora);
    return falta > 0 ? ((uint32_t)falta + 999) / 1000 : 0;
}

// ======================= PERSISTENCIA DEL COMPRESOR =======================
static void cargar_tiempos_compresor(void) {
    nvs_handle_t h;
    if (nvs_open("flowline", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint32_t v;
    if (nvs_get_u32(h, "c_marcha", &v) == ESP_OK && v >= COMP_MARCHA_MIN_S && v <= COMP_MARCHA_MAX_S) {
        comp_marcha_s = v;
    }
    if (nvs_get_u32(h, "c_reposo", &v) == ESP_OK && v >= COMP_REPOSO_MIN_S && v <= COMP_REPOSO_MAX_S) {
        comp_reposo_s = v;
    }
    nvs_close(h);
}

static void guardar_tiempos_compresor(void) {
    nvs_handle_t h;
    if (nvs_open("flowline", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u32(h, "c_marcha", comp_marcha_s);
    nvs_set_u32(h, "c_reposo", comp_reposo_s);
    nvs_commit(h);
    nvs_close(h);
}

// ======================= PERSISTENCIA DE LA LÍNEA DE TIEMPO =======================
static bool linea_valida(const linea_t *l) {
    if (l->n < 1 || l->n > MAX_PASOS) {
        return false;
    }
    for (int i = 0; i < l->n; i++) {
        if (l->paso[i].mascara >= M(N_SALIDAS) || l->paso[i].ms < T_MIN_MS || l->paso[i].ms > T_MAX_MS ||
            l->paso[i].condicion >= N_CONDICIONES || l->paso[i].fin >= N_CONDICIONES) {
            return false;
        }
    }
    return true;
}

static void cargar_linea(void) {
    nvs_handle_t h;
    if (nvs_open("flowline", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    linea_t l;
    size_t len = sizeof(l);
    // "linea2": formato con condición de fin (el blob "linea" anterior tiene otro formato y se ignora)
    if (nvs_get_blob(h, "linea2", &l, &len) == ESP_OK && len == sizeof(l) && linea_valida(&l)) {
        linea = l;
    }
    nvs_close(h);
}

static void guardar_linea(const linea_t *l) {
    nvs_handle_t h;
    if (nvs_open("flowline", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "linea2", l, sizeof(*l));
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
    int p = paso_actual;
    int n = snprintf(json, sizeof(json),
        "{\"paso\":%d,\"pasoMs\":%lu,\"esperando\":%d,\"ciclo\":%d,\"emergencia\":%d,\"auto\":%d,"
        "\"stop\":%d,\"start\":%d,\"s1\":%d,\"s2\":%d,"
        "\"ciclos\":%lu,\"uptime\":%lu,\"clientes\":%d,\"salidas\":{",
        p, (unsigned long)(p >= 0 && !esperando_sensores ? millis() - paso_t0 : 0),
        (p >= 0 && esperando_sensores) ? 1 : 0,
        (ev & EV_CICLO_ACTIVO) ? 1 : 0, (ev & EV_EMERGENCIA) ? 1 : 0, (ev & EV_AUTO) ? 1 : 0,
        gpio_get_level(PIN_STOP_EMG) == 0, gpio_get_level(PIN_START_F) == 0, sensor_1(), sensor_2(),
        (unsigned long)ciclos_completos, (unsigned long)millis(), sta.num);
    for (int s = 0; s < N_SALIDAS; s++) {
        n += snprintf(json + n, sizeof(json) - n, "%s\"%s\":%d", s ? "," : "",
                      NOMBRE_SALIDA[s], estado_salida[s] ? 1 : 0);
    }
    // Diagnóstico: nivel real leído en cada pin de salida (modo INPUT_OUTPUT) y si es activa en bajo
    n += snprintf(json + n, sizeof(json) - n, "},\"pinOut\":[");
    for (int s = 0; s < N_SALIDAS; s++) {
        n += snprintf(json + n, sizeof(json) - n, "%s[%d,%d]", s ? "," : "",
                      gpio_get_level(PIN_SALIDA[s]), ACTIVA_EN_BAJO[s] ? 1 : 0);
    }
    n += snprintf(json + n, sizeof(json) - n,
                  "],\"pinIn\":{\"start\":%d,\"stop\":%d,\"s1\":%d,\"s2\":%d",
                  gpio_get_level(PIN_START_F), gpio_get_level(PIN_STOP_EMG),
                  gpio_get_level(PIN_SENSOR_1), gpio_get_level(PIN_SENSOR_2));
    n += snprintf(json + n, sizeof(json) - n,
                  "},\"comp\":{\"hab\":%d,\"fase\":\"%s\",\"restante\":%lu,\"marcha\":%lu,\"reposo\":%lu,"
                  "\"nivel\":%d",
                  comp_habilitado ? 1 : 0, NOMBRE_FASE_COMP[comp_fase], (unsigned long)comp_restante_s(),
                  (unsigned long)comp_marcha_s, (unsigned long)comp_reposo_s, gpio_get_level(PIN_COMPRESOR));
    n += snprintf(json + n, sizeof(json) - n, "},\"linea\":[");
    xSemaphoreTake(xLineaMutex, portMAX_DELAY);
    for (int i = 0; i < linea.n; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s[%u,%lu,%u,%u]", i ? "," : "",
                      linea.paso[i].mascara, (unsigned long)linea.paso[i].ms, linea.paso[i].condicion,
                      linea.paso[i].fin);
    }
    xSemaphoreGive(xLineaMutex);
    n += snprintf(json + n, sizeof(json) - n, "],\"logSeq\":%lu,\"log\":[", (unsigned long)log_seq);
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

// POST /api/cmd?a=start|paro|rearme|auto|salida&s=<nombre>&v=0|1
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
    if (strcmp(a, "auto") == 0) {
        httpd_query_key_value(q, "v", v, sizeof(v));
        if (v[0] != '1') {
            xEventGroupClearBits(xEventosSistema, EV_AUTO);
            bitacora("[WEB] Modo automatico DESACTIVADO (termina el ciclo en curso)");
            return responder(req, true, "Automatico desactivado");
        }
        if (ev & EV_EMERGENCIA) return responder(req, false, "Emergencia activa: rearme primero");
        xEventGroupSetBits(xEventosSistema, EV_AUTO);
        bitacora("[WEB] Modo automatico ACTIVADO: ciclos continuos");
        return responder(req, true, "Automatico activado");
    }
    if (strcmp(a, "salida") == 0) {
        if (ev & EV_EMERGENCIA) return responder(req, false, "Emergencia activa");
        if (ev & EV_AUTO) return responder(req, false, "Desactive el modo automatico");
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

// POST /api/linea?p=<mascara>.<ms>.<condicion>.<fin>_<mascara>.<ms>.<condicion>.<fin>_...   (en orden)
static esp_err_t h_linea(httpd_req_t *req) {
    EventBits_t ev = xEventGroupGetBits(xEventosSistema);
    if (ev & (EV_CICLO_ACTIVO | EV_AUTO)) {
        return responder(req, false, "Apague el modo automatico y espere a que termine el ciclo");
    }
    char q[200] = "", p[180] = "";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    if (httpd_query_key_value(q, "p", p, sizeof(p)) != ESP_OK) {
        return responder(req, false, "Falta la lista de pasos");
    }
    linea_t l = {0};
    char *c = p;
    while (*c) {
        if (l.n >= MAX_PASOS) return responder(req, false, "Maximo 8 pasos");
        char *fin;
        long m = strtol(c, &fin, 10);
        if (*fin != '.') return responder(req, false, "Formato de pasos invalido");
        long ms = strtol(fin + 1, &fin, 10);
        if (*fin != '.') return responder(req, false, "Formato de pasos invalido");
        long cond = strtol(fin + 1, &fin, 10);
        if (*fin != '.') return responder(req, false, "Formato de pasos invalido");
        long cfin = strtol(fin + 1, &fin, 10);
        if (*fin != '_' && *fin != '\0') return responder(req, false, "Formato de pasos invalido");
        l.paso[l.n].mascara = (m >= 0 && m < (long)M(N_SALIDAS)) ? (uint8_t)m : 0xFF;
        l.paso[l.n].ms = (ms > 0) ? (uint32_t)ms : 0;
        l.paso[l.n].condicion = (cond >= 0 && cond < N_CONDICIONES) ? (uint8_t)cond : 0xFF;
        l.paso[l.n].fin = (cfin >= 0 && cfin < N_CONDICIONES) ? (uint8_t)cfin : 0xFF;
        l.n++;
        c = (*fin == '_') ? fin + 1 : fin;
    }
    if (!linea_valida(&l)) {
        return responder(req, false, "Pasos invalidos (1-8 pasos, 100-60000 ms)");
    }
    xSemaphoreTake(xLineaMutex, portMAX_DELAY);
    linea = l;
    xSemaphoreGive(xLineaMutex);
    guardar_linea(&l);
    bitacora("[CONFIG] Linea de tiempo guardada: %d pasos", l.n);
    return responder(req, true, "Linea de tiempo guardada");
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

// POST /api/comp?hab=0|1   y/o   ?marcha=<s>&reposo=<s>
static esp_err_t h_comp(httpd_req_t *req) {
    char q[64] = "", v[12] = "";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    bool cambia_t = false;
    uint32_t marcha = comp_marcha_s, reposo = comp_reposo_s;
    if (httpd_query_key_value(q, "marcha", v, sizeof(v)) == ESP_OK) {
        long x = strtol(v, NULL, 10);
        if (x < COMP_MARCHA_MIN_S || x > COMP_MARCHA_MAX_S) return responder(req, false, "Marcha: 5 a 600 s");
        marcha = (uint32_t)x;
        cambia_t = true;
    }
    if (httpd_query_key_value(q, "reposo", v, sizeof(v)) == ESP_OK) {
        long x = strtol(v, NULL, 10);
        if (x < COMP_REPOSO_MIN_S || x > COMP_REPOSO_MAX_S) return responder(req, false, "Reposo: 10 a 3600 s");
        reposo = (uint32_t)x;
        cambia_t = true;
    }
    if (cambia_t) {
        comp_marcha_s = marcha;
        comp_reposo_s = reposo;
        guardar_tiempos_compresor();
        bitacora("[COMPRESOR] Tiempos: marcha %lu s, reposo %lu s", (unsigned long)marcha, (unsigned long)reposo);
    }
    if (httpd_query_key_value(q, "hab", v, sizeof(v)) == ESP_OK) {
        if (v[0] == '1') {
            if (emergencia_activa()) return responder(req, false, "Emergencia activa: rearme primero");
            comp_habilitado = true;
            bitacora("[COMPRESOR] Habilitado desde la web");
            return responder(req, true, "Compresor habilitado");
        }
        comp_habilitado = false;
        return responder(req, true, "Compresor apagado");
    }
    return responder(req, true, cambia_t ? "Tiempos guardados" : "Sin cambios");
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
        {.uri = "/api/linea", .method = HTTP_POST, .handler = h_linea},
        {.uri = "/api/comp", .method = HTTP_POST, .handler = h_comp},
    };
    for (size_t i = 0; i < sizeof(rutas) / sizeof(rutas[0]); i++) {
        httpd_register_uri_handler(srv, &rutas[i]);
    }
}

// Solo para las salidas activas en LOW (drenador abierto): apagadas quedan flotantes y el IN del módulo
// conectado jala el pin hacia VCC a través del LED del optoacoplador: se lee 1. Sin cable normalmente se lee 0.
// Las salidas activas en HIGH son push-pull: el ESP32 fija el nivel y no se puede detectar el cable.
static void diagnostico_salidas(void) {
    static const char *PIN_TXT[N_SALIDAS] = {"D15", "D2", "D4", "D5", "D23"};
    char buf[LOG_LEN] = "[DIAG] IN modulo 4 reles (1=conectado):";
    vTaskDelay(pdMS_TO_TICKS(20));
    for (int s = 0; s < N_SALIDAS; s++) {
        if (!ACTIVA_EN_BAJO[s]) {
            continue;
        }
        size_t n = strlen(buf);
        snprintf(buf + n, sizeof(buf) - n, " %s=%d", PIN_TXT[s], gpio_get_level(PIN_SALIDA[s]));
    }
    bitacora("%s", buf);
}

// --- FUNCIÓN PRINCIPAL (APP_MAIN) ---
void app_main(void) {
    printf("=== INICIANDO SISTEMA MULTITAREA FREERTOS - SELLADORA ===\n");

    // Nivel de reposo antes de habilitar las salidas, para que ningún relé pulse al arrancar
    apagar_todas_las_salidas();
    // Salidas activas en LOW (módulo de 4 relés): gpio_config hace reset del IOMUX de cada pin
    gpio_config_t salidas_bajo = {
        .pin_bit_mask = (1ULL << PIN_PISTON1) | (1ULL << PIN_PISTON2) | (1ULL << PIN_CALENTADOR),
        // Drenador abierto: nivel 0 = LOW (relé activado), nivel 1 = pin flotante (relé apagado).
        // Con 3.3 V en push-pull el optoacoplador del módulo de 5 V no se apaga del todo.
        // INPUT_OUTPUT_OD permite leer el pin para el diagnóstico de cableado.
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&salidas_bajo);
    // Salidas activas en HIGH (módulo de banda y bomba): push-pull, 3.3 V = relé activado
    gpio_config_t salidas_alto = {
        .pin_bit_mask = (1ULL << PIN_BANDA) | (1ULL << PIN_BOMBA),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&salidas_alto);
    apagar_todas_las_salidas();

    // Compresor: mismo módulo de 4 relés (activo en LOW, drenador abierto), apagado al arrancar
    rele_compresor(false);
    gpio_config_t compresor = {
        .pin_bit_mask = 1ULL << PIN_COMPRESOR,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&compresor);
    rele_compresor(false);

    // Configuración de entradas con resistencias Pull-Up internas
    gpio_config_t entradas = {
        .pin_bit_mask = (1ULL << PIN_START_F) | (1ULL << PIN_STOP_EMG) |
                        (1ULL << PIN_SENSOR_1) | (1ULL << PIN_SENSOR_2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&entradas);

    xEventosSistema = xEventGroupCreate();
    xLogMutex = xSemaphoreCreateMutex();
    xLineaMutex = xSemaphoreCreateMutex();
    configASSERT(xEventosSistema && xLogMutex && xLineaMutex);
    diagnostico_salidas();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    cargar_linea();
    cargar_tiempos_compresor();

    // Creación de tareas independientes en FreeRTOS (stack en bytes; printf necesita margen)
    xTaskCreate(vTaskSeguridad, "Task_Seguridad", 3072, NULL, 5, NULL);
    xTaskCreate(vTaskProceso, "Task_Proceso", 4096, NULL, 3, NULL);
    xTaskCreate(vTaskCompresor, "Task_Compresor", 3072, NULL, 2, NULL);

    // Monitoreo y control: red Wi-Fi propia + servidor HTTP (tarea httpd, prioridad 5 por defecto)
    iniciar_wifi_ap();
    iniciar_servidor_web();
}
