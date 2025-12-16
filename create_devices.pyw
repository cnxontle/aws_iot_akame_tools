import os
import json
import boto3
import tkinter as tk
from tkinter import simpledialog, messagebox

AWS_REGION = "us-east-2"
SEARCH_NAME = "DeviceFactoryLambda"
LAMBDA_NAME = None

# IoT endpoint
iot_client = boto3.client("iot", region_name=AWS_REGION)
AWS_IOT_ENDPOINT = iot_client.describe_endpoint(
    endpointType="iot:Data-ATS"
)["endpointAddress"]

# Buscar Lambda
lambda_client = boto3.client("lambda", region_name=AWS_REGION)
paginator = lambda_client.get_paginator("list_functions")

for page in paginator.paginate():
    for fn in page["Functions"]:
        if SEARCH_NAME in fn["FunctionName"]:
            LAMBDA_NAME = fn["FunctionName"]
            break

if not LAMBDA_NAME:
    messagebox.showerror("Error", "DeviceFactoryLambda no encontrada")
    exit(1)


# ======================
# UI
# ======================
class GatewayDialog(simpledialog.Dialog):
    def body(self, master):
        master.master.geometry("270x170")

        tk.Label(master, text="User ID:").grid(row=0, column=0, sticky="e")
        tk.Label(master, text="Display Name:").grid(row=1, column=0, sticky="e")
        tk.Label(master, text="SSID:").grid(row=2, column=0, sticky="e")
        tk.Label(master, text="WiFi Password:").grid(row=3, column=0, sticky="e")
        tk.Label(master, text="Plan (días):").grid(row=4, column=0, sticky="e")

        self.user_entry = tk.Entry(master)
        self.display_entry = tk.Entry(master)
        self.ssid_entry = tk.Entry(master)
        self.pass_entry = tk.Entry(master, show="*")
        self.plan_entry = tk.Entry(master)

        self.user_entry.grid(row=0, column=1)
        self.display_entry.grid(row=1, column=1)
        self.ssid_entry.grid(row=2, column=1)
        self.pass_entry.grid(row=3, column=1)
        self.plan_entry.grid(row=4, column=1)

        return self.user_entry

    def apply(self):
        self.user_id = self.user_entry.get().strip()
        self.display_name = self.display_entry.get().strip()
        self.ssid = self.ssid_entry.get().strip()
        self.wifi_password = self.pass_entry.get().strip()
        raw = self.plan_entry.get().strip()
        self.plan_days = int(raw) if raw else None


# ======================
# Lambda call
# ======================
def create_device():
    payload = {
        "userId": dialog.user_id,
        "displayName": dialog.display_name,
        "planDays": dialog.plan_days,
    }

    response = lambda_client.invoke(
        FunctionName=LAMBDA_NAME,
        InvocationType="RequestResponse",
        Payload=json.dumps(payload),
    )

    return json.loads(response["Payload"].read())


# ======================
# Save files
# ======================
def save_files(base_dir, thing_name, data):
    gw_path = os.path.join(base_dir, "gateways", thing_name)
    esp_path = os.path.join(base_dir, "sketches", "sensor_humidity_wifi", "data")

    os.makedirs(gw_path, exist_ok=True)
    os.makedirs(esp_path, exist_ok=True)

    metadata = {
        "thingName": thing_name,
        "displayName": data["displayName"],
        "userId": dialog.user_id,
        "gatewayTopic": data["gatewayTopic"],
        "awsIotEndpoint": AWS_IOT_ENDPOINT,
        "SSID": dialog.ssid,
        "WiFiPassword": dialog.wifi_password,
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


# ======================
# Main
# ======================
root = tk.Tk()
root.withdraw()

dialog = GatewayDialog(root, title="Crear Gateway IoT")

if not dialog.user_id:
    messagebox.showerror("Error", "userId es obligatorio")
    exit(1)

result = create_device()

if result.get("status") == "ok":
    thing_name = result["thingName"]
    save_files(os.getcwd(), thing_name, result)

    messagebox.showinfo(
        "Gateway creado",
        f"Display Name: {result['displayName']}\nThingName: {thing_name}"
    )
else:
    messagebox.showerror("Error", result.get("message"))
