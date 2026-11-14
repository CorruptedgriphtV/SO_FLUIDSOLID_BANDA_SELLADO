# Diagramas del sistema

Diagramas de bloques, conexiones y software de los dos proyectos del repositorio. GitHub los dibuja directamente a partir del código Mermaid.

- [Selladora Flow Line](#selladora-flow-line)
  - [Bloques de hardware](#bloques-de-hardware)
  - [Conexiones del ESP32](#conexiones-del-esp32)
  - [Tareas FreeRTOS](#tareas-freertos)
  - [Ciclo de una bolsa](#ciclo-de-una-bolsa)
  - [Compresor protegido](#compresor-protegido)
- [Brazo robótico 5 DOF](#brazo-robótico-5-dof)
  - [Bloques y conexiones](#bloques-y-conexiones)
  - [De la web al servo](#de-la-web-al-servo)
  - [Estados de una articulación](#estados-de-una-articulación)

---

## Selladora Flow Line

![Render de la selladora](img/selladora.svg)

### Bloques de hardware

```mermaid
flowchart LR
    subgraph PC["Celular o PC"]
        NAV["Navegador<br/>http://192.168.4.1"]
    end
    subgraph CTRL["Control"]
        ESP["ESP32<br/>FreeRTOS + servidor web"]
    end
    subgraph R4["Módulo 4 relés · 5 V · activo en LOW"]
        P1["Pistón 1 · D4"]
        P2["Pistón 2 · D5"]
        CAL["Calentador · D23"]
        COMP["Compresor · D26"]
    end
    subgraph R2["Módulo 2 relés SLA 30 A · jumper H"]
        BANDA["Banda · D15"]
        BOMBA["Bomba · D2"]
    end
    subgraph ENT["Entradas"]
        START["START · D19"]
        STOP["STOP · TX2/GPIO17"]
        S1["Sensor Bolsa S1 · RX2/GPIO16"]
        S2["Sensor Fin S2 · D18"]
    end
    NAV <-- "Wi-Fi AP FlowLine-Selladora" --> ESP
    ESP -- "drenador abierto" --> R4
    ESP -- "push-pull 3.3 V" --> R2
    START --> ESP
    STOP --> ESP
    S1 -- "diodo" --> ESP
    S2 -- "diodo" --> ESP
```

### Conexiones del ESP32

```mermaid
flowchart TB
    ESP(("ESP32"))
    ESP --- D15["D15 → IN1 módulo 2 relés → Banda"]
    ESP --- D2["D2 → IN2 módulo 2 relés → Bomba"]
    ESP --- D4["D4 → IN módulo 4 relés → Pistón 1"]
    ESP --- D5["D5 → IN módulo 4 relés → Pistón 2"]
    ESP --- D23["D23 → IN módulo 4 relés → Calentador"]
    ESP --- D26["D26 → IN módulo 4 relés → Compresor"]
    ESP --- D19["D19 ← botón START a GND"]
    ESP --- G17["GPIO17 ← botón STOP a GND"]
    ESP --- G16["GPIO16 ← diodo ← E18-D80NK Bolsa (5 V)"]
    ESP --- D18["D18 ← diodo ← E18-D80NK Fin de banda (5 V)"]
    ESP --- GND["GND común con relés, sensores y fuentes"]
```

### Tareas FreeRTOS

```mermaid
flowchart LR
    subgraph T["Tareas"]
        SEG["Task_Seguridad<br/>prioridad 5 · cada 20 ms"]
        HTTP["httpd<br/>prioridad 5"]
        PROC["Task_Proceso<br/>prioridad 3"]
        CMP["Task_Compresor<br/>prioridad 2 · cada 100 ms"]
    end
    EV[["Event group<br/>CICLO_ACTIVO · EMERGENCIA<br/>START_WEB · AUTO"]]
    LM[("xLineaMutex<br/>línea de tiempo")]
    LG[("xLogMutex<br/>bitácora")]
    NVS[("NVS<br/>linea2 · c_marcha · c_reposo")]
    SEG <--> EV
    HTTP <--> EV
    PROC <--> EV
    CMP --> EV
    HTTP <--> LM
    PROC <--> LM
    SEG --> LG
    PROC --> LG
    CMP --> LG
    HTTP --> LG
    HTTP <--> NVS
```

### Ciclo de una bolsa

```mermaid
flowchart TD
    A([Inicio: START, web o modo automático]) --> B{"¿Bolsa (S1) detectada<br/>300 ms seguidos?"}
    B -- No --> B
    B -- Sí --> C["Paso 1: Bomba ON 3 s → OFF"]
    C --> D["Paso 2: Pistón 1 + Pistón 2 ON 4 s → OFF"]
    D --> E["Paso 3: Banda ON"]
    E --> F{"¿Fin de banda (S2)<br/>detecta una bolsa nueva?"}
    F -- Sí --> G["Banda OFF · ciclo completo"]
    F -- "No en 15 s" --> H["[ALERTA] Banda OFF<br/>ciclo detenido, automático apagado"]
    G --> I{"¿Modo automático?"}
    I -- Sí --> B
    I -- No --> A
    X(["STOP o PARO web"]) -.-> Z["Emergencia: todo OFF<br/>rearme con START"]
```

El orden, los tiempos y los sensores de cada paso se cambian desde la web sin reprogramar.

### Compresor protegido

```mermaid
stateDiagram-v2
    [*] --> APAGADO
    APAGADO --> MARCHA: habilitado y ya enfriado
    APAGADO --> REPOSO: habilitado pero aún caliente
    MARCHA --> REPOSO: llegó al tiempo máximo de marcha
    MARCHA --> APAGADO: apagado desde la web o emergencia
    REPOSO --> MARCHA: terminó el reposo
    REPOSO --> APAGADO: apagado desde la web o emergencia
    note right of REPOSO
        reposo = t_reposo × (tiempo marchado / t_marcha)
        apagar y encender no se salta el enfriamiento
    end note
```

---

## Brazo robótico 5 DOF

![Render del brazo](img/brazo.svg)

### Bloques y conexiones

```mermaid
flowchart LR
    NAV["Navegador<br/>http://192.168.4.1"] <-- "Wi-Fi AP Robot-Pinza" --> ESP(("ESP32"))
    ESP -- "GPIO 23 · LEDC 0" --> J1["J1 Base"]
    ESP -- "GPIO 22 · LEDC 1" --> J2["J2 Hombro"]
    ESP -- "GPIO 19 · LEDC 2" --> J3["J3 Codo"]
    ESP -- "GPIO 18 · LEDC 3" --> J4["J4 Muñeca"]
    ESP -- "GPIO 15 · LEDC 4" --> J5["J5 Pinza"]
    F["Fuente 5–6 V ≥ 5 A"] --> J1 & J2 & J3 & J4 & J5
    F -. "GND común" .- ESP
```

### De la web al servo

```mermaid
sequenceDiagram
    participant W as Web (slider)
    participant H as Servidor HTTP
    participant Q as Cola de órdenes
    participant T as Task_Servos (cada 20 ms)
    participant S as Servo (PWM 50 Hz)
    W->>H: POST /api/servo?s=3&g=120 (máx. 1 cada 60 ms)
    H->>Q: orden {J3, 120°}
    H-->>W: JSON pos / obj
    loop cada 20 ms
        T->>Q: lee órdenes nuevas
        T->>S: avanza 1.8° hacia el objetivo
    end
    W->>H: GET /api/pos (sondeo)
    H-->>W: posición real
```

### Estados de una articulación

```mermaid
stateDiagram-v2
    [*] --> SinSenal: arranque (pin en LOW, sin PWM)
    SinSenal --> Quieta: primera orden (va directo al ángulo)
    Quieta --> Moviendo: nueva orden
    Moviendo --> Moviendo: paso de 1.8° cada 20 ms
    Moviendo --> Quieta: llegó al objetivo
```
