import sys
import asyncio
import threading
import queue
import numpy as np
from PyQt5 import QtWidgets, QtCore
import pyqtgraph as pg
from bleak import BleakClient, BleakScanner 

DEVICE_NAME = "ESP32-S3-EMG"
CHAR_UUID = "12345678-1234-1234-5678-123412345677"

CHANNELS_COUNT = 4
SAMPLES_PER_PACKET = 15
PACKET_SIZE = 4 + (SAMPLES_PER_PACKET * CHANNELS_COUNT * 4)  # 244 байта

data_queue = queue.Queue()

class BLEWorker:
    def __init__(self, target_name, char_uuid):
        self.target_name = target_name
        self.char_uuid = char_uuid
        self.client = None
        self.is_running = False

    async def connect_and_listen(self):
        self.is_running = True
        try:
            print(f"Поиск устройства с именем '{self.target_name}'...")
            
            device = await BleakScanner.find_device_by_filter(
                lambda d, ad: d.name == self.target_name,
                timeout=10.0
            )
            
            if device is None:
                print(f"Ошибка: Устройство с именем '{self.target_name}' не найдено в эфире.")
                self.is_running = False
                return

            print(f"Устройство найдено! Подключение к {device.address}...")
            
            async with BleakClient(device) as client:
                self.client = client
                print("Подключено! Ожидание активации данных (Notify)...")
                await client.start_notify(self.char_uuid, self.notification_handler)
                
                while self.is_running and client.is_connected:
                    await asyncio.sleep(0.1)
                    
                await client.stop_notify(self.char_uuid)
                print("Отключено.")
        except Exception as e:
            print(f"Ошибка в потоке BLE: {e}")
        finally:
            self.is_running = False

    def notification_handler(self, sender, data: bytearray):
        if len(data) == PACKET_SIZE:
            samples = np.frombuffer(data[4:], dtype=np.int32).reshape((SAMPLES_PER_PACKET, CHANNELS_COUNT))
            data_queue.put(samples)

    def stop(self):
        self.is_running = False


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Real-Time EMG Monitor")
        self.resize(1000, 600)

        self.buffer_size = 5000  
        self.data_buffer = np.zeros((self.buffer_size, CHANNELS_COUNT))
        self.x_data = np.arange(self.buffer_size)
        self.current_channel = 0 

        central_widget = QtWidgets.QWidget()
        self.setCentralWidget(central_widget)
        layout = QtWidgets.QVBoxLayout(central_widget)

        control_layout = QtWidgets.QHBoxLayout()
        
        self.btn_connect = QtWidgets.QPushButton("Connect")
        self.btn_connect.clicked.connect(self.start_ble)
        
        self.btn_disconnect = QtWidgets.QPushButton("Disconnect")
        self.btn_disconnect.clicked.connect(self.stop_ble)
        self.btn_disconnect.setEnabled(False)

        self.combo_channel = QtWidgets.QComboBox()
        for i in range(CHANNELS_COUNT):
            self.combo_channel.addItem(f"Channel {i}")
        self.combo_channel.currentIndexChanged.connect(self.change_channel)

        control_layout.addWidget(self.btn_connect)
        control_layout.addWidget(self.btn_disconnect)
        control_layout.addWidget(QtWidgets.QLabel("Select Channel:"))
        control_layout.addWidget(self.combo_channel)
        control_layout.addStretch()

        layout.addLayout(control_layout)

        self.plot_widget = pg.PlotWidget(background='w')
        self.plot_widget.setTitle("EMG Raw Data", color="k")
        self.plot_widget.setLabel('left', 'Amplitude')
        self.plot_widget.setLabel('bottom', 'Samples')
        self.plot_widget.showGrid(x=True, y=True)
        self.plot_curve = self.plot_widget.plot(pen=pg.mkPen('b', width=2))
        
        layout.addWidget(self.plot_widget)

        self.timer = QtCore.QTimer()
        self.timer.timeout.connect(self.update_plot)
        self.timer.start(20) 

        self.ble_worker = None
        self.ble_thread = None

    def change_channel(self, index):
        self.current_channel = index

    def start_ble(self):
        self.btn_connect.setEnabled(False)
        self.btn_disconnect.setEnabled(True)
        
        self.ble_worker = BLEWorker(DEVICE_NAME, CHAR_UUID)
        self.ble_thread = threading.Thread(
            target=lambda: asyncio.run(self.ble_worker.connect_and_listen()), 
            daemon=True
        )
        self.ble_thread.start()

    def stop_ble(self):
        if self.ble_worker:
            self.ble_worker.stop()
        self.btn_connect.setEnabled(True)
        self.btn_disconnect.setEnabled(False)

    def update_plot(self):
        new_data_list = []
        while not data_queue.empty():
            try:
                batch = data_queue.get_nowait()
                new_data_list.append(batch)
            except queue.Empty:
                break

        if new_data_list:
            new_data = np.vstack(new_data_list)
            num_new = new_data.shape[0]

            if num_new < self.buffer_size:
                self.data_buffer[:-num_new] = self.data_buffer[num_new:]
                self.data_buffer[-num_new:] = new_data
            else:
                self.data_buffer[:] = new_data[-self.buffer_size:]

            self.plot_curve.setData(self.x_data, self.data_buffer[:, self.current_channel])

    def closeEvent(self, event):
        self.stop_ble()
        event.accept()

if __name__ == "__main__":
    app = QtWidgets.QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec_())