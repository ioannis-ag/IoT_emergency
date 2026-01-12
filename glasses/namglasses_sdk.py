#!/usr/bin/env python3
import json
import threading
import time
import uuid
import base64
from typing import Any, Dict, Optional

import paho.mqtt.client as mqtt

class NamGlassesSDK:
    def __init__(
        self,
        broker: str = "labserver.sense-campus.gr",
        port: int = 1883,
        username: Optional[str] = None,
        password: Optional[str] = None,
        client_id: Optional[str] = None,
        keepalive: int = 60,
        clean_session: bool = True,
        downlink_topic: str = "namglasses_downlink",
        uplink_topic: str = "namglasses_uplink",
        qos: int = 1,
    ):
        self.downlink = downlink_topic
        self.uplink = uplink_topic
        self.qos = qos

        self.client = mqtt.Client(client_id=client_id or f"sdk-{uuid.uuid4()}", clean_session=clean_session)
        if username:
            self.client.username_pw_set(username, password)
        self.client.on_message = self._on_message
        self.client.connect(broker, port, keepalive)
        self.client.subscribe(self.uplink, qos=self.qos)
        self.client.loop_start()

        self._waiters: Dict[str, Dict[str, Any]] = {}
        self._lock = threading.Lock()

    def close(self):
        try:
            self.client.loop_stop()
        finally:
            self.client.disconnect()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _on_message(self, client, userdata, msg):
        try:
            data = json.loads(msg.payload.decode("utf-8"))
        except Exception:
            return
        req_id = data.get("request_id")
        if not req_id:
            return
        with self._lock:
            if req_id in self._waiters:
                self._waiters[req_id]["payload"] = data
                self._waiters[req_id]["event"].set()

    def _send(self, cmd: str, args: Optional[Dict[str, Any]] = None, timeout: float = 10.0) -> Dict[str, Any]:
        req_id = str(uuid.uuid4())
        payload = {"cmd": cmd, "args": args or {}, "request_id": req_id, "response_topic": self.uplink}
        evt = threading.Event()
        with self._lock:
            self._waiters[req_id] = {"event": evt, "payload": None}

        self.client.publish(self.downlink, json.dumps(payload, separators=(",", ":")), qos=self.qos)
        ok = evt.wait(timeout)
        with self._lock:
            resp = self._waiters.pop(req_id, {"payload": None})["payload"]
        if not ok or resp is None:
            raise TimeoutError(f"Timed out waiting for response to '{cmd}'")
        if resp.get("type") == "error":
            raise RuntimeError(resp.get("error", "error"))
        return resp

    def display_text(self, text: str, duration_sec: float = 2.0, x: int = 0, y: int = 0, clear: bool = True):
        return self._send("display_text", {"text": text, "duration_sec": duration_sec, "x": x, "y": y, "clear": clear})

    def take_photo(self, timeout: float = 20.0) -> bytes:
        req_id = str(uuid.uuid4())
        payload = {"cmd": "take_photo", "args": {}, "request_id": req_id, "response_topic": self.uplink}
        evt = threading.Event()
        with self._lock:
            self._waiters[req_id] = {"event": evt, "payload": None}
        self.client.publish(self.downlink, json.dumps(payload, separators=(",", ":")), qos=self.qos)
        if not evt.wait(timeout):
            with self._lock:
                self._waiters.pop(req_id, None)
            raise TimeoutError("Timed out waiting for photo")
        with self._lock:
            resp = self._waiters.pop(req_id)["payload"]
        if not resp or resp.get("type") != "photo":
            raise RuntimeError("Unexpected response")
        if resp.get("encoding") == "base64":
            return base64.b64decode(resp["data"])
        raise RuntimeError("Unsupported encoding")

    def get_status(self, timeout: float = 10.0) -> Dict[str, Any]:
        resp = self._send("status", timeout=timeout)
        if resp.get("type") == "status":
            return resp
        req_id = resp.get("request_id")
        if not req_id:
            return resp
        evt = threading.Event()
        with self._lock:
            self._waiters[req_id] = {"event": evt, "payload": None}
        if not evt.wait(timeout):
            with self._lock:
                self._waiters.pop(req_id, None)
            return resp
        with self._lock:
            status_payload = self._waiters.pop(req_id)["payload"]
        return status_payload or resp

    def set_config(self, patch: Dict[str, Any], timeout: float = 10.0):
        return self._send("set_config", {"patch": patch}, timeout=timeout)

    def reload(self, timeout: float = 10.0):
        return self._send("reload", {}, timeout=timeout)

    def ping(self, timeout: float = 5.0) -> Dict[str, Any]:
        return self._send("ping", {}, timeout=timeout)
