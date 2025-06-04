import json
import mysql.connector
from flask import Flask, jsonify, request
from flask_cors import CORS
from paho.mqtt.client import Client
from datetime import datetime

# MQTT cấu hình
MQTT_BROKER = "192.168.6.1"
MQTT_PORT = 1884
MQTT_USER = "toan"
MQTT_PASS = "1"
MQTT_SENSOR_TOPIC = "bbb/sensors"
MQTT_LED_TOPIC = "bbb/led"

# MySQL cấu hình
MYSQL_CONFIG = {
    "host": "localhost",
    "user": "root",
    "password": "123456",
    "database": "sensor_system",
    'port': 3305  # Thay 3306 thành 33060
}

app = Flask(__name__)
CORS(app)

def get_db_connection():
    return mysql.connector.connect(**MYSQL_CONFIG)

@app.route('/api/latest', methods=['GET'])
def get_latest():
    db = get_db_connection()
    cursor = db.cursor(dictionary=True)
    try:
        cursor.execute("""
            SELECT s.temperature, s.humidity, s.lux, s.timestamp,
                   COALESCE(l.led1, 'OFF') as led1,
                   COALESCE(l.led2, 'OFF') as led2
            FROM sensor_data s
            LEFT JOIN led_status l
              ON l.timestamp = (
                  SELECT MAX(timestamp)
                  FROM led_status
                  WHERE timestamp <= s.timestamp
              )
            ORDER BY s.id DESC LIMIT 1
        """)
        row = cursor.fetchone()
        if row:
            return jsonify({
                "temperature": int(round(row['temperature'])),
                "humidity": int(round(row['humidity'])),
                "lux": int(round(row['lux'])),
                "led1": row['led1'],
                "led2": row['led2'],
                "timestamp": row['timestamp'].strftime('%Y-%m-%d %H:%M:%S')
            })
        else:
            return jsonify({"error": "No data found"})
    finally:
        cursor.close()
        db.close()

@app.route('/api/led', methods=['POST'])
def update_led():
    data = request.json or {}
    led1 = data.get("led1")
    led2 = data.get("led2")

    db = get_db_connection()
    cursor = db.cursor()
    try:
        cursor.execute("SELECT led1, led2 FROM led_status ORDER BY timestamp DESC LIMIT 1")
        latest = cursor.fetchone()
        led1_current = latest[0] if latest else "OFF"
        led2_current = latest[1] if latest else "OFF"

        # Nếu không gửi từ Web, giữ nguyên trạng thái cũ
        led1 = led1.upper() if led1 is not None else led1_current
        led2 = led2.upper() if led2 is not None else led2_current

        mqtt_payload = {"led1": led1, "led2": led2}
        mqtt_client.publish(MQTT_LED_TOPIC, json.dumps(mqtt_payload))
        print(f"[MQTT → Device] Gửi trạng thái từ Web: {mqtt_payload}")

        cursor.execute(
            "INSERT INTO led_status (led1, led2) VALUES (%s, %s)",
            (led1, led2)
        )
        db.commit()
    finally:
        cursor.close()
        db.close()

    return jsonify({
        "success": True,
        "led1": led1,
        "led2": led2
    })

@app.route('/api/history_sensors', methods=['GET'])
def get_sensor_history():
    db = get_db_connection()
    cursor = db.cursor(dictionary=True)
    try:
        cursor.execute("""
            SELECT temperature, humidity, lux, timestamp
            FROM sensor_data
            ORDER BY id DESC
            LIMIT 10
        """)
        rows = cursor.fetchall()
        return jsonify([
            {
                "temperature": float(row['temperature']),
                "humidity": float(row['humidity']),
                "lux": float(row['lux']),
                "timestamp": row['timestamp'].strftime('%Y-%m-%d %H:%M:%S')
            } for row in rows
        ])
    finally:
        cursor.close()
        db.close()

@app.route('/api/history_led', methods=['GET'])
def get_led_history():
    db = get_db_connection()
    cursor = db.cursor(dictionary=True)
    try:
        cursor.execute("""
            SELECT led1, led2, timestamp
            FROM led_status
            ORDER BY id DESC
            LIMIT 10
        """)
        rows = cursor.fetchall()
        return jsonify([
            {
                "led1": row['led1'],
                "led2": row['led2'],
                "timestamp": row['timestamp'].strftime('%Y-%m-%d %H:%M:%S')
            } for row in rows
        ])
    finally:
        cursor.close()
        db.close()

def on_message(client, userdata, msg):
    try:
        payload = msg.payload.decode()
        data = json.loads(payload)

        temperature = data.get("temperature", 0)
        humidity = data.get("humidity", 0)
        lux = data.get("lux", 0)

        print(f"[MQTT Received] Temp: {temperature}, Humidity: {humidity}, Lux: {lux}, Time: {datetime.now()}")

        led2_status = "ON" if temperature > 27 else "OFF"

        db = get_db_connection()
        cursor = db.cursor()
        try:
            # Lấy trạng thái hiện tại của led1 từ CSDL
            cursor.execute("SELECT led1 FROM led_status ORDER BY timestamp DESC LIMIT 1")
            result = cursor.fetchone()
            led1_status = result[0] if result else "OFF"

            mqtt_payload = {"led1": led1_status, "led2": led2_status}
            mqtt_client.publish(MQTT_LED_TOPIC, json.dumps(mqtt_payload))
            print(f"[MQTT → Device] Gửi từ cảm biến: {mqtt_payload}")

            # Lưu dữ liệu
            cursor.execute(
                "INSERT INTO sensor_data (temperature, humidity, lux) VALUES (%s, %s, %s)",
                (temperature, humidity, lux)
            )
            cursor.execute(
                "INSERT INTO led_status (led1, led2) VALUES (%s, %s)",
                (led1_status, led2_status)
            )
            db.commit()
            print(f"[DB] Đã lưu trạng thái: LED1 = {led1_status}, LED2 = {led2_status}")
        finally:
            cursor.close()
            db.close()
    except Exception as e:
        import traceback
        print("❌ Lỗi xử lý dữ liệu MQTT:", e)
        traceback.print_exc()

# MQTT setup
mqtt_client = Client()
mqtt_client.username_pw_set(MQTT_USER, MQTT_PASS)
mqtt_client.on_message = on_message
mqtt_client.connect(MQTT_BROKER, MQTT_PORT)
mqtt_client.subscribe(MQTT_SENSOR_TOPIC)
mqtt_client.loop_start()

if __name__ == "__main__":
    app.run(debug=True, port=5000)
