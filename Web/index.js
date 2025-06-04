const tempEl = document.getElementById('temp');
const humEl = document.getElementById('hum');
const lightEl = document.getElementById('light');
const led1El = document.getElementById('led1-status');
const led2El = document.getElementById('led2-status');
const led1ToggleBtn = document.getElementById('led1-toggle-btn');

const chart = new Chart(document.getElementById('sensorChart'), {
  type: 'line',
  data: {
    labels: [],
    datasets: [
      {
        label: 'Temp (°C)',
        borderColor: 'red',
        backgroundColor: 'rgba(255,0,0,0.1)',
        data: [],
        fill: false,
        tension: 0.4,
        borderWidth: 2,
        pointRadius: 3,
        pointHoverRadius: 5,
      },
      {
        label: 'Humidity (%)',
        borderColor: 'blue',
        backgroundColor: 'rgba(0,0,255,0.1)',
        data: [],
        fill: false,
        tension: 0.4,
        borderWidth: 2,
        pointRadius: 3,
        pointHoverRadius: 5,
      },
      {
        label: 'Light (lux)',
        borderColor: 'orange',
        backgroundColor: 'rgba(255,165,0,0.1)',
        data: [],
        fill: false,
        tension: 0.4,
        borderWidth: 2,
        pointRadius: 3,
        pointHoverRadius: 5,
      }
    ]
  },
  options: {
    responsive: true,
    maintainAspectRatio: false,
    animation: false,
    plugins: {
      legend: {
        labels: {
          font: { size: 14, weight: 'bold' },
          color: '#111'
        }
      }
    },
    scales: {
      x: {
        title: {
          display: true,
          text: 'Time',
          color: '#111',
          font: { size: 14, weight: 'bold' }
        },
        ticks: {
          color: '#111',
          font: { size: 12 }
        }
      },
      y: {
        title: {
          display: true,
          text: 'Value',
          color: '#111',
          font: { size: 14, weight: 'bold' }
        },
        ticks: {
          color: '#111',
          font: { size: 12 }
        }
      }
    }
  }
});

let led1UserStatus = null;
let lastLed2Status = null;

async function fetchSensorData() {
  try {
    const res = await fetch('http://localhost:5000/api/latest');
    const data = await res.json();
    if (data.error) return;

    const time = new Date().toLocaleTimeString();
    const temperature = parseInt(data.temperature);
    const humidity = parseInt(data.humidity);
    const lux = parseInt(data.lux);

    tempEl.textContent = isNaN(temperature) ? '-- °C' : `${temperature} °C`;
    humEl.textContent = isNaN(humidity) ? '-- %' : `${humidity} %`;
    lightEl.textContent = isNaN(lux) ? '-- lux' : `${lux} lux`;

    if (led1UserStatus === null) {
      led1El.textContent = data.led1 || "OFF";
    } else {
      led1El.textContent = led1UserStatus;
    }

    const led2Current = data.led2 || "OFF";
        const alertSound = document.getElementById('alert-sound');

        if (led2Current === "ON") {
        led2El.innerHTML = '<span class="blink-scale">⚠️</span>';
        led2El.style.color = "red";

        // Phát âm thanh cảnh báo
        alertSound.play().catch(e => {
            // Bỏ qua lỗi nếu trình duyệt chặn autoplay
            console.warn("Không thể phát âm thanh:", e);
        });
        } else {
        led2El.textContent = "OFF";
        led2El.style.color = "#111";
        alertSound.pause();
        alertSound.currentTime = 0;
        }


    if (led2Current !== lastLed2Status) {
      lastLed2Status = led2Current;
      await fetch('http://localhost:5000/api/log_led2', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ led2: led2Current })
      });
    }

    if (chart.data.labels.length > 10) {
      chart.data.labels.shift();
      chart.data.datasets.forEach(ds => ds.data.shift());
    }

    chart.data.labels.push(time);
    chart.data.datasets[0].data.push(temperature);
    chart.data.datasets[1].data.push(humidity);
    chart.data.datasets[2].data.push(lux);
    chart.update();

  } catch (err) {
    console.error("Fetch failed:", err);
  }
}

async function toggleLed1() {
  try {
    const currentStatus = led1El.textContent.trim();
    const newStatus = currentStatus === "ON" ? "OFF" : "ON";

    const res = await fetch('http://localhost:5000/api/led', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ led1: newStatus })
    });

    const result = await res.json();

    if (result.success) {
      led1El.textContent = newStatus;
      led1UserStatus = newStatus;
    } else {
      alert('Failed to update LED1 status.');
    }
  } catch (err) {
    console.error("Toggle LED1 failed:", err);
  }
}

led1ToggleBtn.addEventListener('click', toggleLed1);
fetchSensorData();
setInterval(fetchSensorData, 1000);
