Place your MQTT broker's public CA certificate here as `mqtt_ca.crt` before
building. The broker hostname supplied during provisioning must match its TLS
certificate. This folder must not contain a CA private key or client credentials.
Local certificates are excluded by `.gitignore`.
