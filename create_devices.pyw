import os
import json
import boto3
import tkinter as tk
from tkinter import simpledialog, messagebox

AWS_REGION = "us-east-2"
SEARCH_NAME = "DeviceFactoryLambda"
LAMBDA_NAME = None
client = boto3.client("iot", region_name="us-east-2")
response = client.describe_endpoint(endpointType="iot:Data-ATS")
AWS_IOT_ENDPOINT = response["endpointAddress"]

# Búsqueda de la Función Lambda
try:
    lambda_client = boto3.client("lambda", region_name=AWS_REGION)
    paginator = lambda_client.get_paginator('list_functions')

    for page in paginator.paginate():
        for func in page['Functions']:
            if SEARCH_NAME in func['FunctionName']:
                LAMBDA_NAME = func['FunctionName']
                break
        if LAMBDA_NAME:
            break
except Exception as e:
    messagebox.showerror("Error de AWS", f"No se pudo conectar a AWS o listar funciones: {e}")
    LAMBDA_NAME = None

# Asumimos que si el cliente falló, la ejecución ya se detuvo o se mostró el error.
if not LAMBDA_NAME:
    pass


# Ventana de diálogo para entrada de datos
class GatewayDialog(simpledialog.Dialog):
    def body(self, master):
        master.master.geometry("270x180") 
        
        tk.Label(master, text="ID de Usuario:").grid(row=0, column=0, sticky="e", padx=5, pady=5)
        tk.Label(master, text="Localización:").grid(row=1, column=0, sticky="e", padx=5, pady=5)

        tk.Label(master, text="SSID:").grid(row=2, column=0, sticky="e", padx=5, pady=5)
        tk.Label(master, text="WiFi Password:").grid(row=3, column=0, sticky="e", padx=5, pady=5)

        self.thing_entry = tk.Entry(master)
        self.user_entry = tk.Entry(master)
        self.ssid_entry = tk.Entry(master)
        self.wifi_password_entry = tk.Entry(master, show="*")

        self.user_entry.grid(row=0, column=1, padx=5, pady=5)
        self.thing_entry.grid(row=1, column=1, padx=5, pady=5)
        self.ssid_entry.grid(row=2, column=1, padx=5, pady=5)
        self.wifi_password_entry.grid(row=3, column=1, padx=5, pady=5)

        return self.thing_entry 

    def apply(self):
        self.thing_name = self.thing_entry.get().strip()
        self.user_id = self.user_entry.get().strip()
        self.ssid = self.ssid_entry.get().strip()
        self.wifi_password = self.wifi_password_entry.get().strip()

# Función para invocar la Lambda
def create_device(lambda_client, thing_name, user_id):
    if not LAMBDA_NAME:
         return {"status": "error", "message": "Función Lambda no encontrada o error de conexión."}
         
    payload = {"thingName": thing_name, "userId": user_id}
    print(f"Invocando Lambda {LAMBDA_NAME} con payload: {payload}")
    
    response = lambda_client.invoke(
        FunctionName=LAMBDA_NAME,
        InvocationType="RequestResponse",
        Payload=json.dumps(payload)
    )
    
    # Leer el payload de la respuesta
    resp_payload = response["Payload"].read().decode("utf-8")
    return json.loads(resp_payload)


# Guardar archivos
def save_device_files(base_dir, device_name, data, user_id, ssid, wifi_password):
    
    # Crear rutas necesarias
    device_path = os.path.join(base_dir, "gateways", device_name)
    esp32_path = os.path.join(base_dir, "sketches", "sensor_humidity_wifi","data")
    paths = [device_path, esp32_path]
    os.makedirs(device_path, exist_ok=True)
    os.makedirs(esp32_path, exist_ok=True)

    # Crear metadata
    metadata = {
        "thingName": device_name,
        "userId": user_id,
        "gatewayTopic": "gateway/" + user_id + "/data/telemetry",
        "awsIotEndpoint": AWS_IOT_ENDPOINT,
        "SSID": ssid,
        "WiFiPassword": wifi_password   
    }
    files = {
        "certificatePem": "certificate.pem",
        "privateKey": "private.key",
        "publicKey":  "public.key"
    }

    # Crear archivos en ambas rutas
    for path in paths:
        for key, filename in files.items():
            with open(os.path.join(path, filename), "w") as f:
                f.write(data[key])

        with open(os.path.join(path, "metadata.json"), "w") as f:
            json.dump(metadata, f, indent=4)

    print(f"Archivos creados en: {device_path}")


# Main
def main():
    root = tk.Tk()
    root.withdraw() 

    if not LAMBDA_NAME:
        messagebox.showerror("Error", "No se pudo encontrar la función Lambda 'DeviceFactoryLambda'. Verifique la región y el despliegue de CDK.")
        return

    dialog = GatewayDialog(root, title="Crear Gateway IoT")

    thing_name = dialog.thing_name
    user_id = dialog.user_id 
    ssid = dialog.ssid
    wifi_password = dialog.wifi_password

    if not thing_name or not user_id:
        if dialog.thing_name is not None: 
             messagebox.showerror("Error", "Tanto el Nombre del Gateway como el User ID son obligatorios.")
        return

    # Carpeta fija dentro del proyecto
    output_dir = os.path.join(os.getcwd())
    os.makedirs(output_dir, exist_ok=True)

    lambda_client = boto3.client("lambda", region_name=AWS_REGION)

    print(f"\n➡ Creando Gateway: {thing_name} para User ID: {user_id}")

    result = create_device(lambda_client, thing_name, user_id)

    if result.get("status") == "ok":
        save_device_files(output_dir, thing_name, result, user_id, ssid, wifi_password)
        messagebox.showinfo(
            "Terminado",
            f"Gateway '{thing_name}' creado exitosamente.\n\nArchivos guardados en la carpeta 'gateways/{thing_name}'."
        )
    else:
        messagebox.showerror(
            "Error",
            f"Error creando Gateway {thing_name}: {result.get('message')}"
        )
        print(f"Error creando Gateway {thing_name}: {result.get('message')}")

if __name__ == "__main__":
    main()