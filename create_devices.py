import os
import json
import boto3
import tkinter as tk
from tkinter import simpledialog, messagebox

# --- CONFIGURACIÓN ---
AWS_REGION = "us-east-2"
SEARCH_NAME = "DeviceFactoryLambda"
LAMBDA_NAME = None

# --- Búsqueda de la Función Lambda ---
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

if not LAMBDA_NAME:
    # Si LAMBDA_NAME sigue siendo None, lanza una excepción si no hay conexión
    # Pero si el error es de conexión, el messagebox anterior ya lo manejó.
    # Aquí asumimos que si el cliente falló, la ejecución ya se detuvo o se mostró el error.
    pass


# --- Ventana de diálogo para entrada de datos ---
class GatewayDialog(simpledialog.Dialog):
    def body(self, master):
        # Establece el tamaño de la ventana
        master.master.geometry("270x110") 
        
        # Etiqueta para el nombre del dispositivo
        tk.Label(master, text="Nombre (ej: sensores_h):").grid(row=0, column=0, sticky="e", padx=5, pady=5)
        # Etiqueta para el ID de usuario
        tk.Label(master, text="User ID (ej: usuario_A):").grid(row=1, column=0, sticky="e", padx=5, pady=5)

        # Campos de entrada
        self.thing_entry = tk.Entry(master)
        self.user_entry = tk.Entry(master)

        self.thing_entry.grid(row=0, column=1, padx=5, pady=5)
        self.user_entry.grid(row=1, column=1, padx=5, pady=5)

        return self.thing_entry  # Enfocarse en el primer campo

    def apply(self):
        # Guarda los valores de entrada
        self.thing_name = self.thing_entry.get().strip()
        self.user_id = self.user_entry.get().strip()


# ---- Función para invocar la Lambda ----
def create_device(lambda_client, thing_name, user_id):
    """Invoca la función Lambda 'Fábrica de Dispositivos'."""
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


# ---- Guardar archivos ----
def save_device_files(base_dir, device_name, data):
    """Guarda las credenciales y metadatos en un subdirectorio."""
    device_path = os.path.join(base_dir, device_name)
    os.makedirs(device_path, exist_ok=True)

    # Escribir certificado
    with open(os.path.join(device_path, "certificate.pem"), "w") as f:
        f.write(data["certificatePem"])

    # Escribir llave privada
    with open(os.path.join(device_path, "private.key"), "w") as f:
        f.write(data["privateKey"])

    # Escribir llave pública
    with open(os.path.join(device_path, "public.key"), "w") as f:
        f.write(data["publicKey"])

    # Escribir metadatos (incluye el tópico que debe usar el Gateway)
    with open(os.path.join(device_path, "metadata.json"), "w") as f:
        json.dump(data, f, indent=4)

    #escribir el thingname en un archivo separado
    with open(os.path.join(device_path, "thing_name.txt"), "w") as f:
        f.write(device_name)

    print(f"Archivos creados en: {device_path}")


# ---- Main ----
def main():
    root = tk.Tk()
    root.withdraw() # Oculta la ventana principal

    # Si la Lambda no se encontró al inicio, no tiene sentido continuar
    if not LAMBDA_NAME:
        messagebox.showerror("Error", "No se pudo encontrar la función Lambda 'DeviceFactoryLambda'. Verifique la región y el despliegue de CDK.")
        return

    dialog = GatewayDialog(root, title="Crear Gateway IoT")

    thing_name = dialog.thing_name
    user_id = dialog.user_id 

    if not thing_name or not user_id:
        # Esto sucede si el usuario cancela o deja campos vacíos
        if dialog.thing_name is not None: # Si no canceló explícitamente
             messagebox.showerror("Error", "Tanto el Nombre del Gateway como el User ID son obligatorios.")
        return

    # Carpeta fija dentro del proyecto
    output_dir = os.path.join(os.getcwd(), "gateways")
    os.makedirs(output_dir, exist_ok=True)

    lambda_client = boto3.client("lambda", region_name=AWS_REGION)

    print(f"\n➡ Creando Gateway: {thing_name} para User ID: {user_id}")

    result = create_device(lambda_client, thing_name, user_id)

    if result.get("status") == "ok":
        save_device_files(output_dir, thing_name, result)
        messagebox.showinfo(
            "Terminado",
            f"Gateway '{thing_name}' creado exitosamente.\n\nArchivos guardados en la carpeta '{output_dir}/{thing_name}'."
        )
    else:
        messagebox.showerror(
            "Error",
            f"Error creando Gateway {thing_name}: {result.get('message')}"
        )
        print(f"Error creando Gateway {thing_name}: {result.get('message')}")

    
if __name__ == "__main__":
    main()