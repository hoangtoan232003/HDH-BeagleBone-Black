USE sensor_system;
/*CREATE TABLE sensor_data (
    id INT AUTO_INCREMENT PRIMARY KEY,
    temperature FLOAT NOT NULL,
    humidity FLOAT NOT NULL,
    lux FLOAT NOT NULL,
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
);*/
/*CREATE TABLE led_status (
    id INT AUTO_INCREMENT PRIMARY KEY,
    led1 VARCHAR(3) NOT NULL,  -- Giá trị: 'ON' hoặc 'OFF'
    led2 VARCHAR(3) NOT NULL,  -- Giá trị: 'ON' hoặc 'OFF'
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
);*/
/*-- Xóa và reset AUTO_INCREMENT
TRUNCATE TABLE sensor_data;
TRUNCATE TABLE led_status;*/
SELECT * FROM led_status;
SELECT * FROM sensor_data;
