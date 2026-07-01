# Sistema embebido para etiquetado inteligente con interfaz de tinta electrónica

Este repositorio contiene el código fuente desarrollado para el Trabajo de Fin de Grado **"Sistema embebido para etiquetado inteligente con interfaz de tinta electrónica"**.

El proyecto consiste en un sistema embebido basado en un microcontrolador ESP32-S3 y una pantalla de tinta electrónica, con alimentación mediante batería recargable e interfaz web local para configurar el contenido mostrado en la pantalla.

## Estructura del repositorio.

- `Página web/`
  - `index.html`: interfaz web utilizada para configurar la información mostrada en la pantalla de tinta electrónica.
  - `servidor.py`: servidor local desarrollado en Python, encargado de recibir los datos de la interfaz web y proporcionarlos al ESP32-S3.
  - `datos.json`: archivo utilizado para almacenar la configuración de la etiqueta.
  - `sensores.json`: archivo utilizado para almacenar la información de monitorización recibida desde el dispositivo.
  - `sensores_historial.json`: archivo utilizado para guardar el histórico de medidas recibidas.

- `firmware_esp32/`
  - Contiene el firmware cargado en el microcontrolador ESP32-S3.

## Funcionamiento general

El usuario modifica el contenido de la etiqueta desde la interfaz web local. El servidor Python recibe esta configuración y la almacena en formato JSON. Posteriormente, el ESP32-S3 consulta al servidor, obtiene los datos disponibles y actualiza la pantalla de tinta electrónica.

El firmware del ESP32-S3 se encarga de establecer la conexión Wi-Fi, comunicarse con el servidor local, interpretar los datos recibidos en formato JSON, controlar la pantalla de tinta electrónica, gestionar la monitorización del sistema y activar el modo de bajo consumo cuando corresponde.

## Autor

Juan David Aranda Andrade


