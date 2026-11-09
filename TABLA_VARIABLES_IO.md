# Flow Line · Selladora — Tabla de variables y E/S digitales

Firmware: `main/main.c` (ESP-IDF 6.1, FreeRTOS, ESP32-WROOM).

## 1. Salidas digitales

| Variable / macro | GPIO | Índice / bit de máscara | Nombre web | Relé / transistor | Carga | Nivel activo | Paso por defecto |
|---|---|---|---|---|---|---|---|
| `PIN_BANDA` | 21 (D21) | `S_BANDA` = 0 → valor 1 | `banda` | Módulo de relés | Banda transportadora | **LOW** | Paso 3 |
| `PIN_BOMBA` | 22 (D22) | `S_BOMBA` = 1 → valor 2 | `bomba` | Módulo de relés | Bomba | **LOW** | Paso 1 |
| `PIN_PISTON1` | 4 | `S_PISTON1` = 2 → valor 4 | `piston1` | Módulo de relés | Electroválvula pistón 1 | **LOW** | Paso 2 |
| `PIN_PISTON2` | 5 | `S_PISTON2` = 3 → valor 8 | `piston2` | Módulo de relés | Electroválvula pistón 2 | **LOW** | Paso 2 |
| `PIN_CALENTADOR` | 23 (D23) | `S_CALENTADOR` = 4 → valor 16 | `calentador` | Módulo de relés | Calentador / selladora | **LOW** | No se usa (pendiente) |

Todas se apagan en un paro de emergencia, al iniciar cada ciclo y al terminar cada paso.

**Lógica inversa en todas las salidas** (`ACTIVA_EN_BAJO[] = {true, true, true, true, true}`), porque el módulo de relés optoacoplado se activa con LOW en IN1–IN4. Las salidas están en **drenador abierto** (`GPIO_MODE_OUTPUT_OD`): relé activado → pin en LOW; relé apagado → pin flotante, igual que `pinMode(pin, INPUT)` en el código Arduino original. Con push-pull a 3.3 V el optoacoplador del módulo de 5 V no se apaga del todo. En la web y en la línea de tiempo "encendido" sigue significando relé activado.

Conexión recomendada del módulo: VCC y JD-VCC a 5 V (con el jumper puesto), GND común con el ESP32, IN1–IN4 a los pines de la tabla.

Durante el reset y los primeros milisegundos del arranque, antes de que corra el firmware, D4 tiene pull-down interno (queda en LOW = relé activado). Para que no dé un pulso al encender, pon una resistencia de 10 kΩ de D4 a 3V3.

## 2. Entradas digitales

| Variable / macro | GPIO | Dispositivo | Configuración | Nivel activo | Función |
|---|---|---|---|---|---|
| `PIN_START_F` | 19 (D19) | Pulsador START | Entrada con pull-up interno | LOW | Inicia un ciclo; con STOP liberado, rearma tras emergencia |
| `PIN_STOP_EMG` | 17 (TX2) | Pulsador STOP | Entrada con pull-up interno | LOW | Paro de emergencia enclavado |
| `PIN_SENSOR_1` | 15 (D15) | S1: sensor de **bolsa** (estación de llenado), NPN | Entrada con pull-up interno | LOW (detecta) | La bomba empieza cuando hay bolsa |
| `PIN_SENSOR_2` | 18 (D18) | S2: sensor de **fin de banda**, NPN | Entrada con pull-up interno | LOW (detecta) | La banda se detiene cuando la bolsa llega al final |

Los pulsadores deben cerrar a **GND** (no a 3V3).

**Sensores NPN (tipo E18-D80NK):** café → 5 V, azul → GND (común con el GND del ESP32), negro (señal) → D15 (S1) o D18 (S2). Antes de conectar la señal, mide con el multímetro el cable negro sin objeto: si marca cerca de 5 V, el sensor trae pull-up interno a 5 V y hay que bajar la señal con un divisor (10 kΩ en serie y 20 kΩ a GND) para no dañar el ESP32.

### Mapa de la hilera superior de la placa (de derecha a izquierda, con el USB a la derecha)

| Serigrafía | 3V3 | GND | D15 | D2 | D4 | RX2 | TX2 | D5 | D18 | D19 | D21 | RXD | TXD | D22 | D23 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Uso | — | GND de botones y sensores | Bolsa (S1) | sin usar | Pistón 1 | libre | STOP | Pistón 2 | Fin de banda (S2) | START | Banda | no usar (USB) | no usar (USB) | Bomba | Calentador |

Todas las entradas y salidas usadas están en la hilera superior. D2 queda sin usar: tiene el LED azul de la placa (con resistencia a GND) y es pin de arranque, así que no sirve para el módulo de relés ni como entrada con pull-up.

## 3. Línea de tiempo del ciclo (por defecto)

| Paso | Empieza | Salidas encendidas | Máscara | Tiempo | Termina |
|---|---|---|---|---|---|
| 1 | Cuando **Bolsa (S1)** detecta 300 ms seguidos | Bomba | 2 | 3000 ms | Por tiempo; se apaga la bomba |
| 2 | De inmediato | Pistón 1 + Pistón 2 | 12 | 4000 ms | Por tiempo; se apagan los dos pistones |
| 3 | De inmediato | Banda | 1 | 15000 ms **máx.** | Cuando **Fin de banda (S2)** detecta una bolsa nueva (50 ms); se apaga la banda |

Condiciones (`paso.condicion` para empezar, `paso.fin` para terminar): 0 = ninguna, 1 = Bolsa (S1), 2 = Fin de banda (S2), 3 = ambos, 4 = cualquiera.

- La espera para empezar solo se cancela con un paro de emergencia.
- Para terminar, solo cuenta una detección **nueva**: si la bolsa anterior sigue frente a S2 cuando arranca la banda, primero tiene que salir.
- Si se cumple el tiempo máximo sin detectar (atasco o bolsa caída), se apaga la banda, el ciclo se detiene, el modo automático se apaga y la bitácora muestra `[ALERTA]`.

El orden, las salidas de cada paso y los tiempos se cambian desde la web (tarjeta **Editar orden y tiempos**) y se guardan en NVS. No hace falta reprogramar. Un paso sin salidas funciona como pausa.

## 4. Bits del grupo de eventos (`xEventosSistema`)

| Bit | Macro | Significado | Lo activa | Lo borra |
|---|---|---|---|---|
| BIT0 | `EV_CICLO_ACTIVO` | Hay un ciclo en curso | Tarea Proceso al iniciar ciclo | Tarea Proceso al terminar |
| BIT1 | `EV_EMERGENCIA` | Paro enclavado | STOP físico o botón PARO web | Rearme (START físico o web, con STOP liberado) |
| BIT2 | `EV_START_WEB` | Inicio pedido desde la web | Botón "Iniciar ciclo" | Tarea Proceso al atenderlo, o un paro |
| BIT3 | `EV_AUTO` | Modo automático: ciclos continuos | Interruptor "Modo automático" | Interruptor apagado, o un paro |

## 5. Variables del programa

| Variable | Tipo | Valor inicial | Unidad | Descripción |
|---|---|---|---|---|
| `linea.n` | `uint8_t` | 3 | pasos | Número de pasos (1 a 8) |
| `linea.paso[i].mascara` | `uint8_t` | 2, 12, 1 | bits | Salidas encendidas en el paso i (suma de valores de la tabla 1) |
| `linea.paso[i].ms` | `uint32_t` | 3000, 4000, 15000 | ms | Duración del paso i, o tiempo máximo si tiene `fin` (100 a 60000) |
| `linea.paso[i].condicion` | `uint8_t` | 1, 0, 0 | — | Sensores que debe esperar el paso i para empezar (ver tabla 3) |
| `linea.paso[i].fin` | `uint8_t` | 0, 0, 2 | — | Sensores que terminan el paso i antes de su tiempo (ver tabla 3) |
| `paso_actual` | `int` | -1 | — | Paso en ejecución (-1 = en espera) |
| `esperando_sensores` | `bool` | `false` | — | El paso actual aún espera a los sensores |
| `paso_t0` | `uint32_t` | 0 | ms | Momento en que empezó el paso actual |
| `ciclos_completos` | `uint32_t` | 0 | ciclos | Ciclos terminados sin emergencia (se reinicia al apagar) |
| `estado_salida[5]` | `bool` | `false` | — | Copia del estado de cada salida (para la web) |
| `log_txt`, `log_ms`, `log_seq` | arreglos / `uint32_t` | vacío / 0 | — | Bitácora circular de 24 mensajes |

La línea se guarda en NVS, espacio `flowline`, clave `linea2` (blob).

## 6. Constantes

| Constante | Valor | Uso |
|---|---|---|
| `MAX_PASOS` | 8 | Pasos máximos en la línea de tiempo |
| `T_MIN_MS` / `T_MAX_MS` | 100 / 60000 ms | Rango válido de cada paso |
| `PERIODO_SUPERVISION_MS` | 20 ms | Periodo de sondeo de entradas |
| `REBOTE_MS` | 50 ms | Antirrebote de pulsadores |
| `T_DETECCION_MS` | 300 ms | Detección estable para empezar un paso |
| `T_DETECCION_FIN_MS` | 50 ms | Detección estable para terminar un paso (detener la banda) |
| `AP_SSID` | `FlowLine-Selladora` | Red Wi-Fi del ESP32 |
| `AP_PASSWORD` | `flowline123` | Contraseña WPA2 |

## 7. Tareas FreeRTOS y sincronización

| Tarea / objeto | Prioridad | Pila | Función |
|---|---|---|---|
| `Task_Seguridad` | 5 | 3072 B | Vigila STOP cada 20 ms, enclava el paro y gestiona el rearme |
| `httpd` (servidor web) | 5 | 6144 B | Web app y API en `http://192.168.4.1` |
| `Task_Proceso` | 3 | 4096 B | Ejecuta la línea de tiempo paso por paso |
| `xLineaMutex` | — | — | Protege `linea` entre la web y la tarea Proceso |
| `xLogMutex` | — | — | Protege la bitácora |

## 8. Modo automático

- Se enciende con el interruptor **Modo automático** en la web.
- Mientras está encendido, al terminar un ciclo empieza otro sin pulsar START.
- Al apagarlo, termina el ciclo en curso y se queda en espera.
- Un paro de emergencia (STOP físico o PARO web) lo **desactiva**. Después del rearme hay que volver a encenderlo.
- No se guarda en memoria: al reiniciar el ESP32 arranca en modo manual, para que la máquina no se mueva sola tras un corte de luz.
- Con el modo automático encendido se bloquean las salidas manuales y la edición de la línea de tiempo.
