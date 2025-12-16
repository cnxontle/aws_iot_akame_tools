import json
import boto3
import requests
from requests_aws4auth import AWS4Auth

# ========= CONFIG =========
STACK_NAME = "RenewalStack"
OUTPUT_KEY = "DeviceAdminApiUrl"
USER_ID = "Juan"
SERVICE = "execute-api"
TIMEOUT = 10
# ==========================


def get_api_url():
    """Obtiene el API Gateway URL desde CloudFormation Outputs"""
    cf = boto3.client("cloudformation")

    resp = cf.describe_stacks(StackName=STACK_NAME)
    outputs = resp["Stacks"][0].get("Outputs", [])

    for o in outputs:
        if o["OutputKey"] == OUTPUT_KEY:
            return o["OutputValue"]

    raise RuntimeError(f"No se encontró el output {OUTPUT_KEY} en {STACK_NAME}")


def main():
    # --- Sesión AWS ---
    session = boto3.Session()
    region = session.region_name or "us-east-1"

    credentials = session.get_credentials().get_frozen_credentials()

    auth = AWS4Auth(
        credentials.access_key,
        credentials.secret_key,
        region,
        SERVICE,
        session_token=credentials.token,
    )

    # --- API URL ---
    api_url = get_api_url()
    url = f"{api_url}user/status"

    payload = {
        "userId": USER_ID
    }

    headers = {
        "Content-Type": "application/json"
    }

    print(f"→ POST {url}")
    print(f"→ userId = {USER_ID}")

    response = requests.post(
        url,
        auth=auth,
        headers=headers,
        data=json.dumps(payload),
        timeout=TIMEOUT,
    )

    print("\nStatus code:", response.status_code)

    try:
        print("Response:")
        print(json.dumps(response.json(), indent=2))
    except Exception:
        print("Raw response:")
        print(response.text)


if __name__ == "__main__":
    main()
