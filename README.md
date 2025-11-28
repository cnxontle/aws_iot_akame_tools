# 🛠️ AWS IoT Akame Tools

Este repositorio contiene un conjunto de herramientas diseñadas para simplificar el uso del **stack de AWS IoT**.

## Flujo de trabajo general

1. Ejecutar el script Python para crear un nuevo dispositivo IoT.
2. La Lambda en AWS genera:  
   - Certificado  
   - Clave privada  
   - Políticas  
   - Endpoint  
   - Metadata JSON
3. Los archivos generados se guardan localmente.
4. Copiar los certificados a la ESP32 (LittleFS).
5. Cargar el *sketch* de Arduino que se conecta y publica en AWS IoT Core.

---
