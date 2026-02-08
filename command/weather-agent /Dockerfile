FROM python:3.11-slim

WORKDIR /app
COPY weather_agent.py /app/weather_agent.py

RUN pip install --no-cache-dir requests

ENV PYTHONUNBUFFERED=1

CMD ["python", "/app/weather_agent.py"]
