#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"

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

#define PERIODO_SUPERVISION_MS 20
#define REBOTE_MS              50

static EventGroupHandle_t xEventosSistema;

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
    gpio_set_level(PIN_BANDA, 0);
    gpio_set_level(PIN_BOMBA, 0);
    gpio_set_level(PIN_PISTON1, 0);
    gpio_set_level(PIN_PISTON2, 0);
    gpio_set_level(PIN_CALENTADOR, 0);
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
        printf("[BANDA] Arrancando banda transportadora para evacuacion...\n");
        gpio_set_level(PIN_BANDA, 1);
        bool ok = esperar_ms(4000); // Tiempo que corre la banda

        printf("[BANDA] Deteniendo banda...\n");
        gpio_set_level(PIN_BANDA, 0);
        if (ok) {
            esperar_ms(2000); // Espera entre ciclos
        }
    }
}

// --- TAREA 2: Secuencia del Proceso (Calentador, Bomba y Pistones) ---
static void vTaskProceso(void *pvParameters) {
    while (true) {
        printf("[PROCESO] Esperando inicio de ciclo (START_F)...\n");
        // Espera flanco de bajada en START_F con antirrebote, sin emergencia
        while (true) {
            if (!emergencia_activa() && gpio_get_level(PIN_START_F) == 0) {
                vTaskDelay(pdMS_TO_TICKS(REBOTE_MS));
                if (gpio_get_level(PIN_START_F) == 0) {
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
        }
        // Espera a que se suelte el botón para no encadenar ciclos
        while (gpio_get_level(PIN_START_F) == 0) {
            vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
        }

        xEventGroupSetBits(xEventosSistema, EV_CICLO_ACTIVO);

        // Paso 1: Encender Calentador y Activar Pistón 1 (Sujeción / Tirón)
        printf("[PROCESO] Encendiendo CALENTADOR y activando PISTON 1...\n");
        gpio_set_level(PIN_CALENTADOR, 1); // Calentador ENCENDIDO
        gpio_set_level(PIN_PISTON1, 1);
        bool ok = esperar_ms(1500);

        // Paso 2: Activar Bomba y Pistón 2 (Sellado térmico exacto de 5 segundos)
        if (ok) {
            printf("[PROCESO] Iniciando SELLADO (Bomba y Piston 2 activos por 5s)...\n");
            gpio_set_level(PIN_BOMBA, 1);
            gpio_set_level(PIN_PISTON2, 1);
            ok = esperar_ms(5000); // 5 segundos exactos de sellado
        }

        // Paso 3: Fin de sellado (o aborto por emergencia). Apagamos actuadores, bomba y calentador.
        printf(ok ? "[PROCESO] Fin de sellado. Apagando bomba, pistones y calentador...\n"
                  : "[PROCESO] Ciclo abortado por emergencia.\n");
        gpio_set_level(PIN_BOMBA, 0);
        gpio_set_level(PIN_PISTON1, 0);
        gpio_set_level(PIN_PISTON2, 0);
        gpio_set_level(PIN_CALENTADOR, 0);

        if (ok) {
            esperar_ms(3000); // Deja terminar la evacuación por la banda
        }
        xEventGroupClearBits(xEventosSistema, EV_CICLO_ACTIVO);
    }
}

// --- TAREA 3: Supervisión de seguridad (máxima prioridad) ---
static void vTaskSeguridad(void *pvParameters) {
    while (true) {
        bool stop_presionado = (gpio_get_level(PIN_STOP_EMG) == 0);

        if (stop_presionado && !emergencia_activa()) {
            xEventGroupSetBits(xEventosSistema, EV_EMERGENCIA);
            apagar_todas_las_salidas();
            printf("[ALERTA CRITICA] PARO DE EMERGENCIA ACTIVADO!\n");
        } else if (!stop_presionado && emergencia_activa()) {
            // Rearme: STOP liberado y el operador pulsa START
            if (gpio_get_level(PIN_START_F) == 0) {
                vTaskDelay(pdMS_TO_TICKS(REBOTE_MS));
                while (gpio_get_level(PIN_START_F) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
                }
                xEventGroupClearBits(xEventosSistema, EV_EMERGENCIA);
                printf("[SEGURIDAD] Sistema rearmado. Pulse START_F para un nuevo ciclo.\n");
            }
        }

        if (emergencia_activa()) {
            apagar_todas_las_salidas(); // Refuerza el estado seguro mientras dure la emergencia
        }
        vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
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
    configASSERT(xEventosSistema);

    // Creación de tareas independientes en FreeRTOS (stack en bytes; printf necesita margen)
    xTaskCreate(vTaskSeguridad, "Task_Seguridad", 3072, NULL, 5, NULL);
    xTaskCreate(vTaskProceso, "Task_Proceso", 4096, NULL, 3, NULL);
    xTaskCreate(vTaskBanda, "Task_Banda", 4096, NULL, 2, NULL);
}
