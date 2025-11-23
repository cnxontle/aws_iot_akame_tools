import json
import time
import os
import random
import sys
import paho.mqtt.client as mqtt

# --- CONFIGURACIÓN DE RUTAS ---
BASE_DIR = os.path.dirname(os.path.abspath(__file__)) if '__file__' in locals() else os.getcwd()
AWS_IOT_ENDPOINT = "afusoll07pjc2-ats.iot.us-east-2.amazonaws.com"
THING_NAME = "sensores_h" 
CREDENTIALS_DIR = os.path.join(os.getcwd(), "gateways", THING_NAME)
METADATA_PATH = os.path.join(CREDENTIALS_DIR, "metadata.json")

# Definición de ruta absoluta para el CA Root
CA_ROOT_PATH = os.path.join(BASE_DIR, "AmazonRootCA1.pem")

# --- Variables cargadas de la metadata ---
try:
    with open(METADATA_PATH, 'r') as f:
        metadata = json.load(f)
        PUBLISH_TOPIC = metadata["gatewayTopic"]
        USER_ID = PUBLISH_TOPIC.split("/")[1]  
         
    print(f" Configuración cargada: {THING_NAME}")
    print(f" Tópico PUBLISH: {PUBLISH_TOPIC}")

except FileNotFoundError:
    print(f"Error: No se encontraron metadatos en {METADATA_PATH}")
    print("Asegúrate de que la carpeta 'gateways/sensor-1' exista y contenga 'metadata.json'.")
    sys.exit(1)

# Generar datos de humedad simulados
def generate_simple_humidity_data(sensor_ids=["sensor-10", "sensor-11", "sensor-12", "sensor-13"]):
    readings = []
    for sensor_id in sensor_ids:
        readings.append({
            "id": sensor_id,
            "humidity": round(random.uniform(40.0, 80.0), 2) # Humedad simulada (40% a 80%)
        })

    # Estructura del payload consolidado
    data = {
        "gatewayId": THING_NAME,
        "userId": USER_ID,
        "timestamp": int(time.time()),
        "readings": readings
    }
    return json.dumps(data)


# callbacks de MQTT
def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print(f"Conexión Exitosa a AWS IoT Core!")
    else:
        print(f"Conexión Fallida. Código de retorno: {reason_code}")

def on_publish(client, userdata, mid, reason_code, properties):
    print(f" Publicación confirmada (MID: {mid})")

# --- BUCLE PRINCIPAL ---
def run_gateway():
    client = mqtt.Client(client_id=THING_NAME, userdata=None, callback_api_version=mqtt.CallbackAPIVersion.VERSION2) 
    
    # 2. Configuración de TLS
    client.tls_set(
        ca_certs=CA_ROOT_PATH, 
        certfile=os.path.join(CREDENTIALS_DIR, "certificate.pem"), 
        keyfile=os.path.join(CREDENTIALS_DIR, "private.key") 
    )

    # 3. Asignar Callbacks
    client.on_connect = on_connect
    client.on_publish = on_publish # Usamos la función on_publish simple
    
    # 4. Intentar conexión
    try:
        client.connect(AWS_IOT_ENDPOINT, 8883, keepalive=60)
    except Exception as e:
        print(f"Error de conexión: {e}")
        return

    client.loop_start()

    # 5. Bucle de publicación
    try:
        while True:
            # 1. Generar los datos
            payload = generate_simple_humidity_data()
            
            # 2. Publicar en el tópico de grupo
            client.publish(PUBLISH_TOPIC, payload, qos=1)
            
            time.sleep(3) # Publica cada 3 segundos
            
    except KeyboardInterrupt:
        print("\nDesconexión por el usuario...")
    finally:
        client.loop_stop()
        client.disconnect()
        print("Desconexión completada.")

if __name__ == "__main__":
    # La verificación de la ruta ahora usa la ruta absoluta
    if not os.path.exists(CA_ROOT_PATH):
        print(f"ALERTA: No se encontró el archivo AmazonRootCA1.pem en la ruta: {CA_ROOT_PATH}")
        print("Asegúrate de descargarlo de https://www.amazontrust.com/repository/AmazonRootCA1.pem y colocarlo en el mismo directorio del script.")
        sys.exit(1)
        
    run_gateway()