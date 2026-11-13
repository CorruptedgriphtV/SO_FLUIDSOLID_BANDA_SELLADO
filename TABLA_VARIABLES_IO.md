# Flow Line · Selladora — Tabla de variables y E/S digitales

Firmware: `main/main.c` (ESP-IDF 6.1, FreeRTOS, ESP32-WROOM). Documentación completa: [docs/selladora.md](docs/selladora.md).

## 1. Salidas digitales

| Variable / macro | GPIO | Índice / valor en máscara | Nombre web | Módulo | Carga | Nivel activo | Modo GPIO | Paso por defecto |
|---|---|---|---|---|---|---|---|---|
| `PIN_BANDA` | 15 (D15) | `S_BANDA` = 0 → 1 | `banda` | 2 relés SLA-12VDC 30 A (jumper **H**) | Banda transportadora | **HIGH** | `GPIO_MODE_INPUT_OUTPUT` (push-pull) | Paso 3 |
| `PIN_BOMBA` | 2 (D2) | `S_BOMBA` = 1 → 2 | `bomba` | 2 relés SLA-12VDC 30 A (jumper **H**) | Bomba (dosificación) | **HIGH** | `GPIO_MODE_INPUT_OUTPUT` (push-pull) | Paso 1 |
| `PIN_PISTON1` | 4 (D4) | `S_PISTON1` = 2 → 4 | `piston1` | 4 relés optoacoplado 5 V | Electroválvula pistón 1 | **LOW** | `GPIO_MODE_INPUT_OUTPUT_OD` (drenador abierto) | Paso 2 |
| `PIN_PISTON2` | 5 (D5) | `S_PISTON2` = 3 → 8 | `piston2` | 4 relés optoacoplado 5 V | Electroválvula pistón 2 | **LOW** | `GPIO_MODE_INPUT_OUTPUT_OD` (drenador abierto) | Paso 2 |
| `PIN_CALENTADOR` | 23 (D23) | `S_CALENTADOR` = 4 → 16 | `calentador` | 4 relés optoacoplado 5 V | Calentador / selladora | **LOW** | `GPIO_MODE_INPUT_OUTPUT_OD` (drenador abierto) | No se usa |
| `PIN_COMPRESOR` | 26 (D26) | — (no está en la máscara) | `comp` (API propia) | 4 relés optoacoplado 5 V | Compresor | **LOW** | `GPIO_MODE_INPUT_OUTPUT_OD` (drenador abierto) | Tarea propia |

- `ACTIVA_EN_BAJO[] = {false, false, true, true, true}` (banda, bomba, piston1, piston2, calentador). En la web y en la línea de tiempo "encendido" siempre significa **relé activado**, sin importar el nivel del pin.
- **Drenador abierto**: relé activado → pin en LOW; relé apagado → pin flotante. Con 3.3 V en push-pull el optoacoplador del módulo de 5 V no se apaga del todo. Módulo de 4 relés: VCC y JD-VCC a 5 V, GND común.
- **Módulo SLA**: el jumper H/L va en **H**. En L funciona al revés, a medias, y puede meter corriente de 12 V al pin.
- Las salidas de proceso se apagan en un paro de emergencia, al iniciar cada ciclo y al terminar cada paso. El compresor se apaga en un paro y queda deshabilitado.
- El modo `INPUT_OUTPUT` permite leer el nivel real de cada pin de salida para el diagnóstico.

## 2. Entradas digitales

| Variable / macro | GPIO | Dispositivo | Configuración | Nivel activo | Función |
|---|---|---|---|---|---|
| `PIN_START_F` | 19 (D19) | Pulsador START a GND | Entrada con pull-up interno | LOW | Inicia un ciclo; con STOP liberado, rearma tras una emergencia |
| `PIN_STOP_EMG` | 17 (TX2) | Pulsador STOP a GND | Entrada con pull-up interno | LOW | Paro de emergencia enclavado |
| `PIN_SENSOR_1` | 16 (RX2) | S1: sensor de **bolsa** (estación de llenado), E18-D80NK NPN | Entrada con pull-up interno | LOW (detecta) | Condición de inicio del paso 1 (bomba) |
| `PIN_SENSOR_2` | 18 (D18) | S2: sensor de **fin de banda**, E18-D80NK NPN | Entrada con pull-up interno | LOW (detecta) | Condición de fin del paso 3 (detiene la banda) |

**Sensores E18-D80NK:** café → **5 V** (a 3.3 V detectan siempre), azul → GND común, negro (señal) → GPIO a través de un **diodo con la raya hacia el sensor** (Schottky ideal), porque la salida sube a 4.8 V sin objeto. El alcance se ajusta con el tornillo trasero.

## 3. Mapa de pines de la placa (ESP32 DevKit de 30 pines)

### Hilera de E/S principal (de derecha a izquierda, con el USB a la derecha)

| Serigrafía | 3V3 | GND | D15 | D2 | D4 | RX2 | TX2 | D5 | D18 | D19 | D21 | RXD | TXD | D22 | D23 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| GPIO | — | — | 15 | 2 | 4 | 16 | 17 | 5 | 18 | 19 | 21 | 3 | 1 | 22 | 23 |
| Uso | — | GND de botones y sensores | Banda | Bomba | Pistón 1 | Bolsa (S1) | STOP | Pistón 2 | Fin de banda (S2) | START | libre | no usar (USB) | no usar (USB) | libre | Calentador |

### Hilera opuesta

| Serigrafía | VIN | GND | D13 | D12 | D14 | D27 | D26 | D25 | D33 | D32 | D35 | D34 | VN | VP | EN |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Uso | — | GND | libre | libre | libre | libre | **Compresor** | libre | libre | libre | libre | libre | libre | libre | reset |

El orden de la serigrafía puede variar entre fabricantes; verifícalo en tu placa. No usar TX/RX (puerto USB) ni D0, D1, CLK, SD, CMD (GPIO 6–11, memoria flash). **D2 es pin de arranque**: si la placa no entra a modo de grabación, desconecta la bomba de D2.

## 4. Línea de tiempo del ciclo (por defecto)

| Paso | Empieza (`condicion`) | Salidas encendidas | Máscara | Tiempo (`ms`) | Termina (`fin`) |
|---|---|---|---|---|---|
| 1 | `C_S1`: **Bolsa (S1)** estable 300 ms | Bomba | 2 | 3000 ms | `C_NINGUNA`: por tiempo |
| 2 | `C_NINGUNA`: de inmediato | Pistón 1 + Pistón 2 | 12 | 4000 ms | `C_NINGUNA`: por tiempo |
| 3 | `C_NINGUNA`: de inmediato | Banda | 1 | 15000 ms **máx.** | `C_S2`: **Fin de banda (S2)**, detección nueva de 50 ms |

| Valor | Macro | Texto en bitácora | Significado |
|---|---|---|---|
| 0 | `C_NINGUNA` | — | Ninguna (de inmediato / por tiempo) |
| 1 | `C_S1` | `BOLSA` | Bolsa (S1) |
| 2 | `C_S2` | `FIN DE BANDA` | Fin de banda (S2) |
| 3 | `C_AMBOS` | `BOLSA y FIN` | S1 y S2 |
| 4 | `C_CUALQUIERA` | `BOLSA o FIN` | S1 o S2 |

- La espera para empezar solo se cancela con un paro de emergencia.
- Para terminar solo cuenta una detección **nueva**: si el sensor ya detectaba al empezar el paso, primero debe liberarse.
- Si se cumple el tiempo máximo sin detectar, el ciclo se aborta, el modo automático se apaga y la bitácora muestra `[ALERTA]`.
- La línea se edita desde la web (tarjeta **Editar orden y tiempos**) o con `POST /api/linea`, y se guarda en NVS (espacio `flowline`, clave `linea2`). Un paso sin salidas es una pausa.

## 5. Bits del grupo de eventos (`xEventosSistema`)

| Bit | Macro | Significado | Lo activa | Lo borra |
|---|---|---|---|---|
| BIT0 | `EV_CICLO_ACTIVO` | Hay un ciclo en curso | `Task_Proceso` al iniciar el ciclo | `Task_Proceso` al terminar o abortar |
| BIT1 | `EV_EMERGENCIA` | Paro enclavado | STOP físico o `a=paro` (botón PARO web) | Rearme: START físico o `a=rearme`, con STOP liberado |
| BIT2 | `EV_START_WEB` | Inicio pedido desde la web | `a=start` (botón "Iniciar ciclo") | `Task_Proceso` al atenderlo, o un paro |
| BIT3 | `EV_AUTO` | Modo automático: ciclos continuos | `a=auto&v=1` | `a=auto&v=0`, un paro, o un paso que agota su tiempo máximo sin detección |

## 6. Variables del programa

| Variable | Tipo | Valor inicial | Unidad | Descripción |
|---|---|---|---|---|
| `linea.n` | `uint8_t` | 3 | pasos | Número de pasos (1 a 8) |
| `linea.paso[i].mascara` | `uint8_t` | 2, 12, 1 | bits | Salidas encendidas en el paso i (suma de valores de la tabla 1) |
| `linea.paso[i].ms` | `uint32_t` | 3000, 4000, 15000 | ms | Duración del paso, o tiempo máximo si tiene `fin` (100 a 60000) |
| `linea.paso[i].condicion` | `uint8_t` | 1, 0, 0 | — | Sensores que debe esperar el paso para empezar |
| `linea.paso[i].fin` | `uint8_t` | 0, 0, 2 | — | Sensores que terminan el paso antes de su tiempo |
| `paso_actual` | `volatile int` | -1 | — | Paso en ejecución (-1 = en espera) |
| `paso_t0` | `volatile uint32_t` | 0 | ms | Momento en que empezó el paso actual (o terminó su espera de sensores) |
| `esperando_sensores` | `volatile bool` | `false` | — | El paso actual aún espera su condición de inicio |
| `ciclos_completos` | `volatile uint32_t` | 0 | ciclos | Ciclos terminados sin abortar (se reinicia al apagar) |
| `estado_salida[5]` | `volatile bool` | `false` | — | Estado lógico de cada salida (true = activada), no el nivel del pin |
| `comp_habilitado` | `volatile bool` | `false` | — | Compresor habilitado desde la web (no se guarda) |
| `comp_fase` | `fase_comp_t` | `COMP_APAGADO` | — | `COMP_APAGADO`, `COMP_MARCHA` o `COMP_REPOSO` |
| `comp_t0` | `volatile uint32_t` | 0 | ms | Inicio de la marcha actual |
| `comp_reposo_hasta` | `volatile uint32_t` | 0 | ms | Instante hasta el que debe seguir enfriándose |
| `comp_marcha_s` | `volatile uint32_t` | 60 | s | Marcha máxima continua (NVS `c_marcha`) |
| `comp_reposo_s` | `volatile uint32_t` | 120 | s | Reposo tras una marcha completa (NVS `c_reposo`) |
| `log_txt`, `log_ms`, `log_seq` | `char[24][96]`, `uint32_t[24]`, `uint32_t` | vacío / 0 | — | Bitácora circular de 24 mensajes |

### Persistencia en NVS (espacio `flowline`)

| Clave | Tipo | Contenido |
|---|---|---|
| `linea2` | blob (`linea_t`) | Línea de tiempo |
| `c_marcha` | u32 | Marcha máxima del compresor (s) |
| `c_reposo` | u32 | Reposo del compresor (s) |

## 7. Constantes

| Constante | Valor | Uso |
|---|---|---|
| `MAX_PASOS` | 8 | Pasos máximos en la línea de tiempo |
| `T_MIN_MS` / `T_MAX_MS` | 100 / 60000 ms | Rango válido de cada paso |
| `PERIODO_SUPERVISION_MS` | 20 ms | Periodo de sondeo de entradas y esperas |
| `REBOTE_MS` | 50 ms | Antirrebote de pulsadores |
| `T_DETECCION_MS` | 300 ms | Detección estable para empezar un paso |
| `T_DETECCION_FIN_MS` | 50 ms | Detección estable para terminar un paso |
| `COMP_MARCHA_DEF_S` | 60 s | Marcha máxima por defecto del compresor |
| `COMP_REPOSO_DEF_S` | 120 s | Reposo por defecto del compresor |
| `COMP_MARCHA_MIN_S` / `COMP_MARCHA_MAX_S` | 5 / 600 s | Rango válido de la marcha |
| `COMP_REPOSO_MIN_S` / `COMP_REPOSO_MAX_S` | 10 / 3600 s | Rango válido del reposo |
| `COMP_PERIODO_MS` | 100 ms | Periodo de `Task_Compresor` |
| `LOG_N` / `LOG_LEN` | 24 / 96 | Mensajes y caracteres de la bitácora |
| `AP_SSID` | `CONFIG_FLOWLINE_AP_SSID` = `FlowLine-Selladora` | Red Wi-Fi del ESP32 (`main/Kconfig.projbuild`) |
| `AP_PASSWORD` | `CONFIG_FLOWLINE_AP_PASSWORD` = `flowline123` | Contraseña WPA2 (abierta si tiene menos de 8 caracteres) |

## 8. Tareas FreeRTOS y sincronización

| Tarea / objeto | Prioridad | Pila | Periodo | Función |
|---|---|---|---|---|
| `Task_Seguridad` | 5 | 3072 B | 20 ms | Vigila STOP, enclava el paro, gestiona el rearme por START y mantiene las salidas apagadas durante la emergencia |
| `httpd` (servidor web) | 5 (por defecto) | 6144 B | — | Web app y API en `http://192.168.4.1` |
| `Task_Proceso` | 3 | 4096 B | 20 ms | Ejecuta la línea de tiempo paso por paso |
| `Task_Compresor` | 2 | 3072 B | 100 ms | Marcha limitada y reposo proporcional del compresor |
| `xEventosSistema` | — | — | — | Event group con los bits de la tabla 5 |
| `xLineaMutex` | — | — | — | Protege `linea` entre la web y `Task_Proceso` |
| `xLogMutex` | — | — | — | Protege la bitácora |

## 9. Modo automático

- Se enciende con el interruptor **Modo automático** en la web (`a=auto&v=1`).
- Mientras está encendido, al terminar un ciclo empieza otro sin pulsar START.
- Al apagarlo, termina el ciclo en curso y se queda en espera.
- Un paro de emergencia lo **desactiva**, igual que un paso que agota su tiempo máximo sin detectar. Después hay que volver a encenderlo.
- No se guarda en memoria: al reiniciar el ESP32 arranca apagado, para que la máquina no se mueva sola tras un corte de luz.
- Con el automático encendido se bloquean las salidas manuales, el botón **Iniciar ciclo** y el guardado de la línea de tiempo.
