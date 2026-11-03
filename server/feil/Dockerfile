# The error report service (feil.py, mounted at /app by compose.yaml):
# Python with Pillow, which reads each screenshot and makes it anew, and
# cryptography, which checks the display's signature.
FROM python:3.13-alpine
RUN pip install --no-cache-dir pillow==11.3.0 cryptography==45.0.7
