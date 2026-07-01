/*
  Proyecto: Etiqueta e-Paper con ESP32-S3

  Descripción general:
  - El ESP32 se conecta a una red Wi-Fi local.
  - Consulta un servidor HTTP local para leer la configuración de la etiqueta.
  - Actualiza una pantalla de tinta electrónica únicamente cuando cambia la versión
    recibida desde el servidor, evitando refrescos innecesarios.
  - Envía al servidor medidas básicas: temperatura interna del ESP32 y nivel
    estimado de batería.
  - Permite activar un modo de bajo consumo mediante deep sleep desde la página web,
    de forma que el dispositivo despierte periódicamente, consulte el servidor
    y vuelva a dormir.
*/

// =========================
// LIBRERÍAS PRINCIPALES
// =========================

// Wi-Fi y peticiones HTTP al servidor local.
#include <WiFi.h>
#include <HTTPClient.h>

// Librería para parsear los JSON recibidos desde el servidor.
#include <ArduinoJson.h>

// Funciones de deep sleep del ESP32.
#include "esp_sleep.h"

// Comunicación SPI y librería de control de la pantalla e-Paper.
#include <SPI.h>
#include <GxEPD2_BW.h>

// Fuentes utilizadas para dibujar el contenido en la pantalla.
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeSerif9pt7b.h>
#include <Fonts/TomThumb.h>

// Sensor interno de temperatura del ESP32-S3.
#include "driver/temperature_sensor.h"


// =========================
// DEFINICIÓN DE PINES
// =========================

// LED azul integrado en la PCB. Se usa como indicador visual de estado.
#define LED_BLUE    14

// Señal que habilita la alimentación de la pantalla e-Paper.
// HIGH: pantalla alimentada.
// LOW: pantalla apagada.
#define EPD_PWR_EN  4

// Pines SPI y señales de control de la pantalla e-Paper.
#define EPD_MOSI    5
#define EPD_SCK     6
#define EPD_CS      7
#define EPD_DC      8
#define EPD_RST     10
#define EPD_BUSY    11

// Entrada analógica conectada al divisor resistivo de batería.
#define VBAT_ADC_PIN 1


// ======================================================
// CONFIGURACIÓN DE RED Y RUTAS DEL SERVIDOR LOCAL
// ======================================================

// Nombre y contraseña de la red Wi-Fi.
// Recomendación: para una versión pública del código, sustituir por placeholders
// o mover las credenciales a un archivo separado no versionado.
const char* ssid = "XXXXX";
const char* password = "XXXXX";

// Endpoints del servidor Python.
// /datos: configuración actual de la etiqueta.a
// /sensores: recepción de datos enviados por el ESP32.
const char* urlDatos = "http://XXX.XXX.X.XXX:8000/datos";
const char* urlSensores = "http://XXX.XXX.X.XXX:8000/sensores";


// =========================
// CONFIGURACIÓN DE BAJO CONSUMO
// =========================

// El modo de bajo consumo se configura desde el servidor/web.
//
// Si usarDeepSleep = false, el ESP32 permanece funcionando en modo normal:
// se queda en loop(), enviando sensores y consultando cambios cada cierto tiempo.
//
// Si usarDeepSleep = true, el ESP32 ejecuta un único ciclo:
// despertar -> conectar Wi-Fi -> consultar servidor -> actualizar si procede
// -> enviar sensores -> entrar en deep sleep.
//
// Estos valores son solo valores por defecto. Después de leer /datos, se sustituyen
// por los campos bajo_consumo.activo y bajo_consumo.intervalo_s enviados por el servidor.
bool usarDeepSleep = false;
uint32_t intervaloWakeupS = 600;   // 600 s = 10 min

// Límites de seguridad para evitar intervalos absurdos recibidos del servidor.
const uint32_t INTERVALO_WAKEUP_MIN_S = 30;
const uint32_t INTERVALO_WAKEUP_MAX_S = 24UL * 60UL * 60UL;


// =========================
// CONFIGURACIÓN DE MEDIDA DE BATERÍA
// =========================

// Divisor resistivo utilizado para medir VBAT:
// VBAT -- 470 kΩ -- GPIO1 -- 150 kΩ -- GND
//
// El ADC del ESP32 mide la tensión del nodo intermedio, no VBAT directamente.
// Por tanto, se multiplica por el factor del divisor para reconstruir VBAT.
const float R_VBAT_SUP = 470000.0f;
const float R_VBAT_INF = 150000.0f;
const float FACTOR_DIVISOR_VBAT = (R_VBAT_SUP + R_VBAT_INF) / R_VBAT_INF;

// Calibración empírica de la lectura de batería.
// En la placa se observó que, cuando el cargador indicaba fin de carga,
// la lectura era de aproximadamente 4,09 V en lugar de 4,20 V.
// Este factor corrige de forma aproximada la medida.
//
// Nota: esta corrección es aproximada porque no se dispone de calibración
// completa del ADC con polímetro en varios puntos.
const float FACTOR_CAL_VBAT = 4.20f / 4.09f;

// Rango usado para estimar el porcentaje de batería.
// Se usa 3,40 V como 0 % funcional porque el sistema genera 3,3 V mediante
// un buck, por lo que no interesa apurar la celda hasta tensiones demasiado bajas.
const float VBAT_MIN = 3.40f;  // 0 % funcional
const float VBAT_MAX = 4.20f;  // 100 %


// =========================
// OBJETO DE PANTALLA E-PAPER
// =========================

// Pantalla de 2,13" blanco/negro controlada mediante GxEPD2.
// El tipo concreto debe coincidir con el panel utilizado.
GxEPD2_BW<GxEPD2_213_BN, GxEPD2_213_BN::HEIGHT> display(
  GxEPD2_213_BN(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);


// =========================
// VARIABLES GLOBALES
// =========================

// Manejador del sensor interno de temperatura.
temperature_sensor_handle_t temp_handle = NULL;

// Variables que almacenan la configuración recibida desde el servidor.
String modo = "";
String nombre = "";
String precio = "";
String codigo = "";
String estado = "";
String extra = "";
String descripcion = "";

// Versión actual recibida desde el servidor.
int version = 0;

// Última versión que ya fue mostrada en pantalla.
// RTC_DATA_ATTR permite conservar este valor durante deep sleep.
// Así, al despertar, el ESP32 puede saber si la versión del servidor cambió.
RTC_DATA_ATTR int ultimaVersionMostrada = -1;

// Temporización usada solo en modo normal, cuando no se emplea deep sleep.
unsigned long ultimoEnvioSensores = 0;
const unsigned long intervaloSensores = 5000;


// =========================
// FUNCIONES DEL LED
// =========================

// Parpadeo rápido: usado durante la conexión Wi-Fi.
void ledParpadeoRapido() {
  digitalWrite(LED_BLUE, HIGH);
  delay(100);
  digitalWrite(LED_BLUE, LOW);
  delay(100);
}

// Parpadeo lento: usado como indicación de error.
void ledParpadeoLento() {
  digitalWrite(LED_BLUE, HIGH);
  delay(500);
  digitalWrite(LED_BLUE, LOW);
  delay(500);
}

// LED encendido fijo: indica funcionamiento normal o Wi-Fi conectado.
void ledFijo() {
  digitalWrite(LED_BLUE, HIGH);
}

// LED apagado: se usa antes de entrar en deep sleep para ahorrar consumo.
void ledApagado() {
  digitalWrite(LED_BLUE, LOW);
}

// Destello breve: usado justo antes de actualizar la pantalla.
void ledDestello() {
  digitalWrite(LED_BLUE, LOW);
  delay(120);
  digitalWrite(LED_BLUE, HIGH);
}


// =========================
// SENSOR INTERNO DE TEMPERATURA
// =========================

// Inicializa el sensor interno de temperatura del ESP32-S3.
// El rango configurado es de 10 °C a 80 °C.
void iniciarSensorInterno() {
  temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
  temperature_sensor_install(&temp_sensor_config, &temp_handle);
  temperature_sensor_enable(temp_handle);
}

// Lee la temperatura interna del ESP32-S3.
// Nota: no es una medida exacta de temperatura ambiente, sino del propio chip.
float leerTemperaturaInterna() {
  float temp = 0.0;

  if (temp_handle != NULL) {
    temperature_sensor_get_celsius(temp_handle, &temp);
  }

  return temp;
}


// =========================
// MEDIDA DE BATERÍA
// =========================

// Configura el ADC para leer la tensión del divisor de batería.
// - Resolución de 12 bits.
// - Atenuación de 11 dB para permitir leer tensiones más altas en el pin ADC.
void iniciarMedidaBateria() {
  analogReadResolution(12);
  analogSetPinAttenuation(VBAT_ADC_PIN, ADC_11db);
}

// Lee la tensión de batería a partir del divisor resistivo.
//
// Proceso:
// 1. Realiza varias muestras para reducir ruido.
// 2. Calcula la tensión media medida en el pin ADC.
// 3. Reconstruye VBAT usando el factor del divisor resistivo.
// 4. Aplica una corrección empírica aproximada.
float leerTensionBateria() {
  const int muestras = 20;
  uint32_t suma_mV = 0;

  for (int i = 0; i < muestras; i++) {
    suma_mV += analogReadMilliVolts(VBAT_ADC_PIN);
    delay(2);
  }

  float v_adc = (suma_mV / (float)muestras) / 1000.0f;
  float v_bat_medida = v_adc * FACTOR_DIVISOR_VBAT;

  // Corrección empírica aproximada de la lectura del ADC.
  float v_bat_corregida = v_bat_medida * FACTOR_CAL_VBAT;

  return v_bat_corregida;
}

// Convierte la tensión de batería corregida en un porcentaje aproximado.
//
// Importante:
// La relación tensión-porcentaje de una LiPo no es perfectamente lineal.
// Por tanto, este porcentaje es orientativo, no un estado de carga exacto.
int calcularPorcentajeBateria(float v_bat) {
  float porcentaje = (v_bat - VBAT_MIN) * 100.0f / (VBAT_MAX - VBAT_MIN);

  if (porcentaje < 0.0f) porcentaje = 0.0f;
  if (porcentaje > 100.0f) porcentaje = 100.0f;

  return (int)(porcentaje + 0.5f);
}


// =========================
// CONEXIÓN WI-FI
// =========================

// Conecta el ESP32 a la red Wi-Fi configurada.
//
// Parámetro:
// - timeout_ms: tiempo máximo de espera para conectar.
//
// Devuelve:
// - true si se conecta correctamente.
// - false si se agota el tiempo de espera.
bool conectarWiFi(uint32_t timeout_ms = 15000) {
  Serial.println("Conectando al WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long t_inicio = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - t_inicio < timeout_ms) {
    Serial.print(".");
    ledParpadeoRapido();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println();
    Serial.println("No se pudo conectar al WiFi dentro del tiempo límite");
    ledParpadeoLento();
    return false;
  }

  Serial.println();
  Serial.println("WiFi conectado");
  Serial.print("IP del ESP32: ");
  Serial.println(WiFi.localIP());

  ledFijo();
  return true;
}


// =========================
// COMUNICACIÓN HTTP / JSON
// =========================

// Envía al servidor las medidas disponibles:
// - temperatura interna del ESP32;
// - humedad y presión como campos vacíos/provisionales;
// - porcentaje estimado de batería;
// - tensión de batería.
//
// El servidor guarda esta información y la muestra en la página web.
void enviarSensores() {
  float temp = leerTemperaturaInterna();
  float vbat = leerTensionBateria();
  int bateria = calcularPorcentajeBateria(vbat);

  HTTPClient http;
  http.begin(urlSensores);
  http.addHeader("Content-Type", "application/json");

  // Construcción manual del JSON enviado al servidor.
  String payload = "{";
  payload += "\"temperatura\":\"" + String(temp, 1) + " °C\",";
  payload += "\"humedad\":\"-- %\",";
  payload += "\"presion\":\"-- hPa\",";
  payload += "\"bateria\":\"" + String(bateria) + " %\",";
  payload += "\"vbat\":\"" + String(vbat, 2) + " V\"";
  payload += "}";

  int httpCode = http.POST(payload);

  Serial.print("POST /sensores -> ");
  Serial.println(httpCode);
  Serial.println(payload);

  if (httpCode <= 0) {
    Serial.println("Error enviando sensores");
    ledParpadeoLento();
    ledFijo();
  }

  http.end();
}

// Lee la configuración actual del servidor.
//
// El servidor devuelve un JSON con:
// - modo de funcionamiento;
// - versión;
// - campos de la etiqueta.
//
// La versión se usa para saber si hace falta refrescar la pantalla.
bool leerConfiguracion() {
  HTTPClient http;
  http.begin(urlDatos);

  int httpCode = http.GET();

  if (httpCode <= 0) {
    Serial.print("Error GET /datos: ");
    Serial.println(http.errorToString(httpCode));
    http.end();

    ledParpadeoLento();
    ledFijo();

    return false;
  }

  String payload = http.getString();
  http.end();

  Serial.println("JSON recibido:");
  Serial.println(payload);

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.print("Error JSON: ");
    Serial.println(error.c_str());

    ledParpadeoLento();
    ledFijo();

    return false;
  }

  // Lectura segura de los campos del JSON.
  // Si algún campo no existe, se deja como cadena vacía o cero.
  modo = doc["modo"].is<const char*>() ? doc["modo"].as<const char*>() : "";
  version = doc["version"] | 0;

  nombre = doc["etiqueta"]["nombre"].is<const char*>() ? doc["etiqueta"]["nombre"].as<const char*>() : "";
  precio = doc["etiqueta"]["precio"].is<const char*>() ? doc["etiqueta"]["precio"].as<const char*>() : "";
  codigo = doc["etiqueta"]["codigo"].is<const char*>() ? doc["etiqueta"]["codigo"].as<const char*>() : "";
  estado = doc["etiqueta"]["estado"].is<const char*>() ? doc["etiqueta"]["estado"].as<const char*>() : "";
  extra = doc["etiqueta"]["extra"].is<const char*>() ? doc["etiqueta"]["extra"].as<const char*>() : "";
  descripcion = doc["etiqueta"]["descripcion"].is<const char*>() ? doc["etiqueta"]["descripcion"].as<const char*>() : "";

  // Lectura de la configuración de bajo consumo.
  // El servidor envía un objeto bajo_consumo con dos campos:
  // - activo: true/false, indica si se debe usar deep sleep.
  // - intervalo_s: segundos que permanecerá dormido entre ciclos.
  // Si el objeto no existe, se mantienen valores seguros por defecto.
  if (doc["bajo_consumo"].is<JsonObject>()) {
    JsonObject bajoConsumo = doc["bajo_consumo"];
    usarDeepSleep = bajoConsumo["activo"] | false;
    intervaloWakeupS = limitarIntervaloWakeup(bajoConsumo["intervalo_s"] | 600);
  } else {
    usarDeepSleep = false;
    intervaloWakeupS = 600;
  }

  Serial.print("Modo: ");
  Serial.println(modo);

  Serial.print("Version: ");
  Serial.println(version);

  Serial.print("Bajo consumo: ");
  Serial.println(usarDeepSleep ? "Activado" : "Desactivado");

  Serial.print("Intervalo wakeup: ");
  Serial.print(intervaloWakeupS);
  Serial.println(" s");

  return true;
}


// =========================
// UTILIDADES
// =========================

// Recorta texto para que no se salga de las zonas dibujadas en la pantalla.
// Si el texto supera maxChars, se añaden dos puntos al final.
String recortarTexto(String texto, int maxChars) {
  if (texto.length() <= maxChars) return texto;
  return texto.substring(0, maxChars - 2) + "..";
}

// Limita un intervalo recibido del servidor a un rango seguro.
// Esto evita que un error en la web deje el dispositivo durmiendo demasiado poco
// o durante un tiempo excesivamente largo.
uint32_t limitarIntervaloWakeup(uint32_t intervalo_s) {
  if (intervalo_s < INTERVALO_WAKEUP_MIN_S) return INTERVALO_WAKEUP_MIN_S;
  if (intervalo_s > INTERVALO_WAKEUP_MAX_S) return INTERVALO_WAKEUP_MAX_S;
  return intervalo_s;
}


// =========================
// CONTROL DE LA PANTALLA E-PAPER
// =========================

// Alimenta e inicializa la pantalla e-Paper.
//
// La pantalla se alimenta solo cuando es necesario actualizarla.
// Esto reduce consumo durante los periodos de reposo.
void encenderPantalla() {
  digitalWrite(EPD_PWR_EN, HIGH);
  delay(500);

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);

  display.init(115200, true, 2, false);
  display.setRotation(1);
  display.setFullWindow();
}

// Envía la pantalla a hibernación y corta su alimentación.
// EPD_PWR_EN en LOW desactiva el circuito de alimentación del panel.
void apagarPantalla() {
  display.hibernate();
  delay(100);
  digitalWrite(EPD_PWR_EN, LOW);
}

// Función auxiliar para imprimir texto en una posición concreta.
void imprimirTexto(int x, int y, String texto) {
  display.setCursor(x, y);
  display.print(texto);
}

// Dibuja la plantilla de etiqueta comercial.
// Incluye nombre, código, precio, estado, información extra y descripción.
void dibujarEtiqueta() {
  ledDestello();

  display.setFullWindow();

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    // Marco exterior y divisiones principales.
    display.drawRect(0, 0, 250, 122, GxEPD_BLACK);

    display.drawLine(0, 30, 250, 30, GxEPD_BLACK);
    display.drawLine(0, 92, 250, 92, GxEPD_BLACK);
    display.drawLine(175, 30, 175, 92, GxEPD_BLACK);
    display.drawLine(175, 61, 250, 61, GxEPD_BLACK);

    // Nombre del producto.
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(6, 21);
    display.print(recortarTexto(nombre, 20));

    // Código o referencia.
    display.setFont(&FreeSans9pt7b);
    display.setCursor(200, 18);
    display.print(recortarTexto(codigo, 10));

    // Precio principal.
    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(15, 75);
    display.print(precio);

    // Campo de estado, por ejemplo "Oferta" o "Nuevo".
    display.setFont(&FreeSans9pt7b);
    display.setCursor(182, 52);
    display.print(recortarTexto(estado, 8));

    // Información extra, por ejemplo "IVA incl.".
    display.setFont(&FreeSans9pt7b);
    display.setCursor(182, 82);
    display.print(recortarTexto(extra, 8));

    // Descripción inferior.
    display.setFont(&FreeSans9pt7b);
    display.setCursor(6, 112);
    display.print(recortarTexto(descripcion, 38));

  } while (display.nextPage());

  ledFijo();
}

// Dibuja la plantilla de monitorización ambiental.
// En esta versión se muestra la temperatura interna del ESP32 y campos
// reservados para humedad y presión.
void dibujarSensores() {
  ledDestello();

  float temp = leerTemperaturaInterna();

  display.setFullWindow();

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    display.drawRect(0, 0, 250, 122, GxEPD_BLACK);
    display.drawLine(0, 28, 250, 28, GxEPD_BLACK);

    display.setFont(&FreeSans9pt7b);
    display.setCursor(8, 20);
    display.print("Monitor ambiental");

    // Cajas de los cuatro campos.
    display.drawRect(8, 38, 116, 34, GxEPD_BLACK);
    display.drawRect(126, 38, 116, 34, GxEPD_BLACK);
    display.drawRect(8, 78, 116, 34, GxEPD_BLACK);
    display.drawRect(126, 78, 116, 34, GxEPD_BLACK);

    // Títulos.
    display.setFont(&FreeSans9pt7b);

    display.setCursor(28, 50);
    display.print("Temp");

    display.setCursor(151, 50);
    display.print("Humedad");

    display.setCursor(40, 90);
    display.print("Presion");

    display.setCursor(150, 90);
    display.print("Actualiz.");

    // Valores mostrados.
    display.setFont(&FreeSansBold9pt7b);

    display.setCursor(25, 68);
    display.print(temp, 1);
    display.print(" C");

    display.setCursor(153, 68);
    display.print("-- %");

    display.setCursor(28, 108);
    display.print("-- hPa");

    display.setCursor(151, 108);
    display.print("--:--");

  } while (display.nextPage());

  ledFijo();
}

// Selecciona qué plantilla dibujar en función del modo recibido del servidor.
// Al terminar, apaga la pantalla para reducir el consumo.
void actualizarPantallaSegunModo() {
  encenderPantalla();

  if (modo == "etiqueta") {
    Serial.println("Actualizando pantalla: etiqueta");
    dibujarEtiqueta();
  }
  else if (modo == "sensores") {
    Serial.println("Actualizando pantalla: sensores");
    dibujarSensores();
  }
  else {
    Serial.println("Modo no reconocido");
    ledParpadeoLento();
    ledFijo();
  }

  apagarPantalla();
}


// =========================
// DEEP SLEEP
// =========================

// Prepara el sistema para bajo consumo y entra en deep sleep.
//
// Además, calcula el tiempo activo del ciclo, útil para estimar autonomía:
// desde el inicio de setup() hasta justo antes de dormir.
void entrarDeepSleep(unsigned long t_inicio_ciclo) {
  unsigned long t_fin_ciclo = millis();
  float t_activo_s = (t_fin_ciclo - t_inicio_ciclo) / 1000.0f;

  Serial.print("Tiempo activo total: ");
  Serial.print(t_activo_s, 3);
  Serial.println(" s");

  Serial.print("Entrando en deep sleep durante ");
  Serial.print(intervaloWakeupS);
  Serial.println(" s");

  // Apagar elementos controlables antes de dormir.
  ledApagado();
  digitalWrite(EPD_PWR_EN, LOW);

  // Desconectar Wi-Fi para reducir consumo antes de entrar en deep sleep.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  Serial.flush();
  delay(100);

  // Configura el temporizador RTC como fuente de despertar.
  esp_sleep_enable_timer_wakeup((uint64_t)intervaloWakeupS * 1000000ULL);

  // Entrada efectiva en deep sleep. Al despertar, el programa empieza de nuevo
  // desde setup(), pero las variables RTC_DATA_ATTR se conservan.
  esp_deep_sleep_start();
}


// =========================
// SETUP / LOOP
// =========================

// setup() se ejecuta al encender el ESP32 y también tras cada despertar
// desde deep sleep. Por eso toda la lógica del ciclo de bajo consumo se
// concentra aquí.
void setup() {
  // Marca temporal para medir cuánto tiempo permanece activo el sistema
  // antes de volver a dormir.
  unsigned long t_inicio_ciclo = millis();

  Serial.begin(115200);
  delay(1000);

  // Inicialización de salidas básicas.
  pinMode(LED_BLUE, OUTPUT);
  ledApagado();

  // La pantalla se deja apagada por defecto. Solo se alimenta al actualizar.
  pinMode(EPD_PWR_EN, OUTPUT);
  digitalWrite(EPD_PWR_EN, LOW);

  // Inicialización de sensores y medida de batería.
  iniciarSensorInterno();
  iniciarMedidaBateria();

  // Conexión Wi-Fi. Si falla, no se actualiza ni se envían datos.
  bool wifiOK = conectarWiFi();

  if (wifiOK) {
    // Lee configuración desde el servidor.
    if (leerConfiguracion()) {
      // Condiciones para refrescar la pantalla:
      // - primera ejecución;
      // - versión distinta a la última mostrada;
      // - modo sensores, que se actualiza siempre para mostrar datos nuevos.
      bool primeraVez = (ultimaVersionMostrada < 0);
      bool versionNueva = (version != ultimaVersionMostrada);
      bool modoSensores = (modo == "sensores");

      if (primeraVez || versionNueva || modoSensores) {
        actualizarPantallaSegunModo();
        ultimaVersionMostrada = version;
      } else {
        Serial.println("La versión no ha cambiado. No se actualiza la pantalla.");
      }
    }

    // Envía temperatura y batería a la web.
    enviarSensores();
  }

  // Si el modo deep sleep está activo, se duerme al terminar el ciclo.
  if (usarDeepSleep) {
    entrarDeepSleep(t_inicio_ciclo);
  }
}

// loop() se usa en modo normal.
// Si desde la web se activa el bajo consumo, el ESP32 lo detectará al leer /datos
// y entrará en deep sleep al final de esa iteración.
void loop() {
  // Reconexión Wi-Fi en caso de pérdida de conexión.
  if (WiFi.status() != WL_CONNECTED) {
    conectarWiFi();
  }

  unsigned long ahora = millis();

  // En modo normal, envía sensores y consulta el servidor cada intervaloSensores.
  if (ahora - ultimoEnvioSensores >= intervaloSensores) {
    ultimoEnvioSensores = ahora;

    enviarSensores();

    if (leerConfiguracion()) {
      // Solo refresca la pantalla si el servidor tiene una versión nueva.
      if (version != ultimaVersionMostrada) {
        actualizarPantallaSegunModo();
        ultimaVersionMostrada = version;
      }

      // Si el usuario ha activado el bajo consumo desde la web, se entra
      // en deep sleep al final de esta iteración.
      if (usarDeepSleep) {
        entrarDeepSleep(ahora);
      }
    }
  }
}
