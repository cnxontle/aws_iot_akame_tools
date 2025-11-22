import os
import json
import boto3
import tkinter as tk
from tkinter import simpledialog, filedialog, messagebox

LAMBDA_NAME = "DeviceFactoryLambda"   
AWS_REGION = "us-east-2"              

#Invoca la Lambda y regresa el JSON decodificado.
def create_device(lambda_client, thing_name, user_id):
    payload = {"thingName": thing_name, "userId": user_id}
    response = lambda_client.invoke(
        FunctionName=LAMBDA_NAME,
        InvocationType="RequestResponse",
        Payload=json.dumps(payload)
    )
    resp_payload = response["Payload"].read().decode("utf-8")
    return json.loads(resp_payload)

#Guarda certificate.pem, private.key, public.key y metadata.json
def save_device_files(base_dir, device_name, data):
    device_path = os.path.join(base_dir, device_name)
    os.makedirs(device_path, exist_ok=True)

    with open(os.path.join(device_path, "certificate.pem"), "w") as f:
        f.write(data["certificatePem"])

    with open(os.path.join(device_path, "private.key"), "w") as f:
        f.write(data["privateKey"])

    with open(os.path.join(device_path, "public.key"), "w") as f:
        f.write(data["publicKey"])

    with open(os.path.join(device_path, "metadata.json"), "w") as f:
        json.dump(data, f, indent=4)

    print(f"Archivos creados en: {device_path}")


def main():
    root = tk.Tk()
    root.withdraw()

    prefix = simpledialog.askstring(
        "Prefijo",
        "Ingresa prefijo para los dispositivos (ej: sensor, device, node):"
    )
    if not prefix:
        messagebox.showerror("Error", "Debes ingresar un prefijo.")
        return

    start = simpledialog.askinteger(
        "Inicio",
        "Número inicial del rango (ej: 20):"
    )
    if start is None:
        return

    count = simpledialog.askinteger(
        "Cantidad",
        "Cantidad de dispositivos a crear:"
    )
    if count is None:
        return

    user = simpledialog.askstring(
        "Usuario",
        "UserID (ej: juan):"
    )
    if not user:
        user = "juan"

    # Seleccionar carpeta
    messagebox.showinfo("Carpeta", "Selecciona la carpeta donde se guardarán los dispositivos.")
    output_dir = filedialog.askdirectory()

    if not output_dir:
        messagebox.showerror("Error", "No seleccionaste carpeta. Cancelado.")
        return

    # Proceso de creación
    lambda_client = boto3.client("lambda", region_name=AWS_REGION)
    end = start + count - 1

    for i in range(start, end + 1):
        thing_name = f"{prefix}-{i}"
        print(f"\n➡ Creando dispositivo: {thing_name}")

        result = create_device(lambda_client, thing_name, user)

        if result.get("status") == "ok":
            save_device_files(output_dir, thing_name, result)
        else:
            print(f"Error creando {thing_name}: {result.get('message')}")

    messagebox.showinfo(
        "Terminado",
        f"{count} dispositivos creados con prefijo '{prefix}'."
    )


if __name__ == "__main__":
    main()