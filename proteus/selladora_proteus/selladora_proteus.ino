// Port del SO de la selladora (main/main.c) a ATmega328P para simulación en Proteus 8.
// La pieza "ESP32 Module (TEP)" de Proteus es un ATmega328P @16MHz por dentro,
// así que este .hex se carga en ella o en un Arduino Uno.
// Compilar con la librería parcheada ../libraries/FreeRTOS_Proteus (tick por Timer1,
// porque Proteus no simula bien el tick por Watchdog de Arduino_FreeRTOS).
#include <Arduino_FreeRTOS.h>
#include <event_groups.h>
#include <semphr.h>

#define PIN_BANDA      8    // Control relé Banda (RL1 / Q1)
#define PIN_BOMBA      9    // Control relé Bomba (RL3 / Q2)
#define PIN_PISTON1    10   // Control relé Piston 1 (RL4 / Q3)
#define PIN_PISTON2    11   // Control relé Piston 2 (RL5 / Q3)
#define PIN_CALENTADOR 12   // Control relé Auxiliar / Calentador (RL2 / Q4)

// Definición de Pines de Entrada (Pulsadores / Sensores, activos en bajo)
#define PIN_START_F    2    // Botón de Inicio Ciclo
#define PIN_STOP_EMG   3    // Paro de Emergencia

// Bits del grupo de eventos del sistema
#define EV_CICLO_ACTIVO (1 << 0)    // Hay un ciclo de sellado en curso
#define EV_EMERGENCIA   (1 << 1)    // Paro de emergencia enclavado

#define PERIODO_SUPERVISION_MS 30
#define REBOTE_MS              50

static EventGroupHandle_t xEventosSistema;
static SemaphoreHandle_t xMutexSerial;

static void log_msg(const __FlashStringHelper *msg) {
    if (xSemaphoreTake(xMutexSerial, portMAX_DELAY) == pdTRUE) {
        Serial.println(msg);
        xSemaphoreGive(xMutexSerial);
    }
}

static bool emergencia_activa(void) {
    return (xEventGroupGetBits(xEventosSistema) & EV_EMERGENCIA) != 0;
}

// Espera ms en pasos cortos; retorna false si se activa la emergencia durante la espera
static bool esperar_ms(uint32_t ms) {
    const uint32_t paso = 30;
    while (ms > 0) {
        if (emergencia_activa()) {
            return false;
        }
        uint32_t t = (ms < paso) ? ms : paso;
        vTaskDelay(pdMS_TO_TICKS(t) > 0 ? pdMS_TO_TICKS(t) : 1);
        ms -= t;
    }
    return !emergencia_activa();
}

static void apagar_todas_las_salidas(void) {
    digitalWrite(PIN_BANDA, LOW);
    digitalWrite(PIN_BOMBA, LOW);
    digitalWrite(PIN_PISTON1, LOW);
    digitalWrite(PIN_PISTON2, LOW);
    digitalWrite(PIN_CALENTADOR, LOW);
}

// --- TAREA 1: Control de la Banda Transportadora en Paralelo ---
static void vTaskBanda(void *pvParameters) {
    while (true) {
        xEventGroupWaitBits(xEventosSistema, EV_CICLO_ACTIVO, pdFALSE, pdTRUE, portMAX_DELAY);
        if (emergencia_activa()) {
            vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
            continue;
        }

        log_msg(F("[BANDA] Arrancando banda transportadora para evacuacion..."));
        digitalWrite(PIN_BANDA, HIGH);
        bool ok = esperar_ms(4000); // Tiempo que corre la banda

        log_msg(F("[BANDA] Deteniendo banda..."));
        digitalWrite(PIN_BANDA, LOW);
        if (ok) {
            esperar_ms(2000); // Espera entre ciclos
        }
    }
}

// --- TAREA 2: Secuencia del Proceso (Calentador, Bomba y Pistones) ---
static void vTaskProceso(void *pvParameters) {
    while (true) {
        log_msg(F("[PROCESO] Esperando inicio de ciclo (START_F)..."));
        while (true) {
            if (!emergencia_activa() && digitalRead(PIN_START_F) == LOW) {
                vTaskDelay(pdMS_TO_TICKS(REBOTE_MS));
                if (digitalRead(PIN_START_F) == LOW) {
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
        }
        while (digitalRead(PIN_START_F) == LOW) {
            vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
        }

        xEventGroupSetBits(xEventosSistema, EV_CICLO_ACTIVO);

        // Paso 1: Encender Calentador y Activar Pistón 1 (Sujeción / Tirón)
        log_msg(F("[PROCESO] Encendiendo CALENTADOR y activando PISTON 1..."));
        digitalWrite(PIN_CALENTADOR, HIGH);
        digitalWrite(PIN_PISTON1, HIGH);
        bool ok = esperar_ms(1500);

        // Paso 2: Activar Bomba y Pistón 2 (Sellado térmico de 5 segundos)
        if (ok) {
            log_msg(F("[PROCESO] Iniciando SELLADO (Bomba y Piston 2 activos por 5s)..."));
            digitalWrite(PIN_BOMBA, HIGH);
            digitalWrite(PIN_PISTON2, HIGH);
            ok = esperar_ms(5000);
        }

        // Paso 3: Fin de sellado (o aborto por emergencia)
        if (ok) {
            log_msg(F("[PROCESO] Fin de sellado. Apagando bomba, pistones y calentador..."));
        } else {
            log_msg(F("[PROCESO] Ciclo abortado por emergencia."));
        }
        digitalWrite(PIN_BOMBA, LOW);
        digitalWrite(PIN_PISTON1, LOW);
        digitalWrite(PIN_PISTON2, LOW);
        digitalWrite(PIN_CALENTADOR, LOW);

        if (ok) {
            esperar_ms(3000); // Deja terminar la evacuación por la banda
        }
        xEventGroupClearBits(xEventosSistema, EV_CICLO_ACTIVO);
    }
}

// --- TAREA 3: Supervisión de seguridad (máxima prioridad) ---
static void vTaskSeguridad(void *pvParameters) {
    while (true) {
        bool stop_presionado = (digitalRead(PIN_STOP_EMG) == LOW);

        if (stop_presionado && !emergencia_activa()) {
            xEventGroupSetBits(xEventosSistema, EV_EMERGENCIA);
            apagar_todas_las_salidas();
            log_msg(F("[ALERTA CRITICA] PARO DE EMERGENCIA ACTIVADO!"));
        } else if (!stop_presionado && emergencia_activa()) {
            // Rearme: STOP liberado y el operador pulsa START
            if (digitalRead(PIN_START_F) == LOW) {
                vTaskDelay(pdMS_TO_TICKS(REBOTE_MS));
                while (digitalRead(PIN_START_F) == LOW) {
                    vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
                }
                xEventGroupClearBits(xEventosSistema, EV_EMERGENCIA);
                log_msg(F("[SEGURIDAD] Sistema rearmado. Pulse START_F para un nuevo ciclo."));
            }
        }

        if (emergencia_activa()) {
            apagar_todas_las_salidas();
        }
        vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISION_MS));
    }
}

void setup() {
    Serial.begin(9600);

    pinMode(PIN_BANDA, OUTPUT);
    pinMode(PIN_BOMBA, OUTPUT);
    pinMode(PIN_PISTON1, OUTPUT);
    pinMode(PIN_PISTON2, OUTPUT);
    pinMode(PIN_CALENTADOR, OUTPUT);
    apagar_todas_las_salidas();

    // Entradas con resistencias Pull-Up internas
    pinMode(PIN_START_F, INPUT_PULLUP);
    pinMode(PIN_STOP_EMG, INPUT_PULLUP);

    Serial.println(F("=== INICIANDO SISTEMA MULTITAREA FREERTOS - SELLADORA ==="));

    xEventosSistema = xEventGroupCreate();
    xMutexSerial = xSemaphoreCreateMutex();

    // Stacks en bytes (StackType_t = uint8_t en AVR); el ATmega328P solo tiene 2 KB de RAM
    xTaskCreate(vTaskSeguridad, "Seg", 224, NULL, 3, NULL);
    xTaskCreate(vTaskProceso, "Proc", 256, NULL, 2, NULL);
    xTaskCreate(vTaskBanda, "Banda", 224, NULL, 1, NULL);
    // Arduino_FreeRTOS arranca el scheduler automáticamente al salir de setup()
}

void loop() {
    // Tarea idle: sin uso
}
