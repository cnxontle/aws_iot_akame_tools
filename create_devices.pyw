import os
import json
import boto3
import tkinter as tk
from tkinter import simpledialog, messagebox

AWS_REGION = "us-east-2"

DEVICE_FACTORY_SEARCH = "DeviceFactoryLambda"
LAMBDA_NAME = None


# AWS clients
iot_client = boto3.client("iot", region_name=AWS_REGION)
lambda_client = boto3.client("lambda", region_name=AWS_REGION)

AWS_IOT_ENDPOINT = iot_client.describe_endpoint(
    endpointType="iot:Data-ATS"
)["endpointAddress"]


# Buscar DeviceFactoryLambda
paginator = lambda_client.get_paginator("list_functions")

for page in paginator.paginate():
    for fn in page["Functions"]:
        name = fn["FunctionName"]
        if DEVICE_FACTORY_SEARCH in name:
            LAMBDA_NAME = name
            break
    if LAMBDA_NAME:
        break

if not LAMBDA_NAME:
    messagebox.showerror("Error", "DeviceFactoryLambda no encontrada")
    exit(1)


# UI para ingresar datos
class GatewayDialog(simpledialog.Dialog):
    def body(self, master):
        master.master.geometry("250x150")

        labels = [
            "SSID:",
            "WiFi Password:",
            "Plan (días):",
        ]

        for i, text in enumerate(labels):
            tk.Label(master, text=text).grid(row=i, column=0, sticky="e", padx=5, pady=5)

        self.ssid_entry = tk.Entry(master)
        self.pass_entry = tk.Entry(master, show="*")
        self.plan_entry = tk.Entry(master)

        self.ssid_entry.grid(row=0, column=1, padx=5)
        self.pass_entry.grid(row=1, column=1, padx=5)
        self.plan_entry.grid(row=2, column=1, padx=5)

        return self.ssid_entry

    def apply(self):
        self.ssid = self.ssid_entry.get().strip()
        self.wifi_password = self.pass_entry.get().strip()

        raw = self.plan_entry.get().strip()
        self.plan_days = int(raw) if raw else None


# Lambda call
def create_device():
    payload = {}

    # El plan es opcional (la lambda usa DEFAULT_EXPIRATION_SECONDS)
    if dialog.plan_days:
        payload["planDays"] = dialog.plan_days
    response = lambda_client.invoke(
        FunctionName=LAMBDA_NAME,
        InvocationType="RequestResponse",
        Payload=json.dumps(payload),
    )
    return json.loads(response["Payload"].read())


# Archivos y metadata
def save_files(base_dir, thing_name, data):
    gw_path = os.path.join(base_dir, "gateways", thing_name)
    esp_path = os.path.join(base_dir, "sketches", "sensor_humidity_wifi", "data")

    os.makedirs(gw_path, exist_ok=True)
    os.makedirs(esp_path, exist_ok=True)

    metadata = {
        "thingName": thing_name,
        "gatewayTopic": data["gatewayTopic"],
        "awsIotEndpoint": AWS_IOT_ENDPOINT,
        "SSID": dialog.ssid,
        "WiFiPassword": dialog.wifi_password,
        "activationCode": data["activationCode"],
    }

    for path in (gw_path, esp_path):
        with open(os.path.join(path, "certificate.pem"), "w") as f:
            f.write(data["certificatePem"])

        with open(os.path.join(path, "private.key"), "w") as f:
            f.write(data["privateKey"])

        with open(os.path.join(path, "public.key"), "w") as f:
            f.write(data["publicKey"])

        with open(os.path.join(path, "metadata.json"), "w") as f:
            json.dump(metadata, f, indent=4)

    return gw_path


# Main
root = tk.Tk()
root.withdraw()

dialog = GatewayDialog(root, title="Crear Gateway IoT")

# Crear el Thing en AWS IoT
result = create_device()

if result.get("status") != "ok":
    messagebox.showerror("Error", result.get("message", "Error desconocido"))
    exit(1)

thing_name = result["thingName"]
activation_code = result["activationCode"]

base_dir = os.getcwd()

# Guardar certificados y metadata
gw_path = save_files(base_dir, thing_name, result)


messagebox.showinfo(
    "Gateway creado",
    f"ThingName:\n{thing_name}\n\n"
    f"Activation Code:\n{activation_code}",
)
