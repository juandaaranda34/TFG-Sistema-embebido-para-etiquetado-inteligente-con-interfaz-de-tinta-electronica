import json
from datetime import datetime
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

# ======================================================
# Servidor local para la etiqueta e-Paper
# ======================================================
# - Sirve la página web index.html.
# - Guarda la configuración de la etiqueta en datos.json.
# - Recibe medidas del ESP32 en sensores.json.
# - Mantiene un histórico ambiental en sensores_historial.json.
# - Permite activar/desactivar el modo bajo consumo sin cambiar la versión
#   de la etiqueta, evitando refrescos innecesarios de la pantalla.

PORT = 8000
MAX_MUESTRAS = 120

DATOS_FILE = "datos.json"
SENSORES_FILE = "sensores.json"
HISTORIAL_FILE = "sensores_historial.json"


def datos_por_defecto():
    """Configuración inicial si datos.json no existe todavía."""
    return {
        "modo": "sensores",
        "etiqueta": {},
        "version": 0,
        "bajo_consumo": {
            "activo": False,
            "intervalo_s": 600
        }
    }


def sensores_por_defecto():
    """Valores mostrados por la web hasta recibir la primera medida del ESP32."""
    return {
        "temperatura": "-- °C",
        "humedad": "-- %",
        "presion": "-- hPa",
        "bateria": "-- %",
        "vbat": "-- V",
        "actualizacion": "--:--:--"
    }


def leer_json(path, default):
    """Lee un archivo JSON. Si no existe o hay error, devuelve default."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return default


def guardar_json(path, data):
    """Guarda data en formato JSON legible."""
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)


class MiHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        # Página principal.
        if self.path == "/":
            try:
                with open("index.html", "rb") as f:
                    contenido = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(contenido)
            except FileNotFoundError:
                self.send_error(404, "No se encontró index.html")

        # Configuración de la etiqueta y del modo bajo consumo.
        elif self.path == "/datos":
            data = leer_json(DATOS_FILE, datos_por_defecto())
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.end_headers()
            self.wfile.write(json.dumps(data, ensure_ascii=False).encode("utf-8"))

        # Última lectura recibida desde el ESP32.
        elif self.path == "/sensores":
            data = leer_json(SENSORES_FILE, sensores_por_defecto())
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.end_headers()
            self.wfile.write(json.dumps(data, ensure_ascii=False).encode("utf-8"))

        # Histórico de medidas ambientales.
        # La batería no se incluye aquí porque se muestra como estado actual.
        elif self.path == "/sensores_historial":
            data = leer_json(HISTORIAL_FILE, [])
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.end_headers()
            self.wfile.write(json.dumps(data, ensure_ascii=False).encode("utf-8"))

        else:
            self.send_error(404, "Ruta no encontrada")

    def do_POST(self):
        longitud = int(self.headers.get("Content-Length", 0))
        cuerpo = self.rfile.read(longitud)

        # Guarda la configuración principal de la etiqueta.
        # Esta ruta sí actualiza datos.json completo y normalmente incrementa version
        # desde la propia página web cuando cambia el contenido mostrado.
        if self.path == "/guardar":
            try:
                datos = json.loads(cuerpo.decode("utf-8"))

                guardar_json(DATOS_FILE, datos)

                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.end_headers()
                self.wfile.write(b"Configuracion guardada correctamente")
            except Exception:
                self.send_error(400, "JSON invalido")

        # Guarda únicamente la configuración de bajo consumo.
        # No cambia la version de la etiqueta, para que el ESP32 no refresque
        # la pantalla solo por activar o desactivar deep sleep.
        elif self.path == "/bajo_consumo":
            try:
                datos_bc = json.loads(cuerpo.decode("utf-8"))
                datos = leer_json(DATOS_FILE, datos_por_defecto())

                try:
                    intervalo_s = int(datos_bc.get("intervalo_s", 600))
                except Exception:
                    intervalo_s = 600

                intervalo_s = max(30, min(intervalo_s, 24 * 60 * 60))

                datos["bajo_consumo"] = {
                    "activo": bool(datos_bc.get("activo", False)),
                    "intervalo_s": intervalo_s
                }

                guardar_json(DATOS_FILE, datos)

                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.end_headers()
                self.wfile.write(b"Modo bajo consumo actualizado")
            except Exception:
                self.send_error(400, "JSON invalido")

        # Recibe sensores desde el ESP32.
        elif self.path == "/sensores":
            try:
                datos = json.loads(cuerpo.decode("utf-8"))

                # Si el ESP32 no manda hora, la añade el servidor.
                if "actualizacion" not in datos or not datos["actualizacion"]:
                    datos["actualizacion"] = datetime.now().strftime("%H:%M:%S")

                # Última muestra completa para la web actual.
                guardar_json(SENSORES_FILE, datos)

                # Histórico solo ambiental. La batería se excluye del histórico.
                muestra_historial = {
                    "temperatura": datos.get("temperatura", "-- °C"),
                    "humedad": datos.get("humedad", "-- %"),
                    "presion": datos.get("presion", "-- hPa"),
                    "actualizacion": datos.get("actualizacion", "--:--:--")
                }

                historial = leer_json(HISTORIAL_FILE, [])
                historial.append(muestra_historial)
                historial = historial[-MAX_MUESTRAS:]
                guardar_json(HISTORIAL_FILE, historial)

                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.end_headers()
                self.wfile.write(b"Datos de sensores guardados correctamente")
            except Exception:
                self.send_error(400, "JSON invalido")

        else:
            self.send_error(404, "Ruta no encontrada")


if __name__ == "__main__":
    servidor = ThreadingHTTPServer(("0.0.0.0", PORT), MiHandler)
    print(f"Servidor local en http://localhost:{PORT}")
    servidor.serve_forever()
