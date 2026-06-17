import tkinter as tk
from tkinter import scrolledtext, messagebox, ttk, filedialog
import socket
import threading
import struct
import os
import json
from datetime import datetime
import numpy as np
from scipy import signal
import matplotlib
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk

matplotlib.use('TkAgg')
import matplotlib.pyplot as plt

plt.rcParams['font.sans-serif'] = [
    'Microsoft YaHei',
    'SimHei',
    'Arial Unicode MS',
    'DejaVu Sans',
]
plt.rcParams['axes.unicode_minus'] = False

# ==========================================
# 1. 协议配置
# ==========================================
FRAME_SYNC = b"GLinkOtdr-3800M\0"

CMD_HOST_START_MEASURE = 0x10000000
CMD_HOST_STOP_MEASURE = 0x10000001
CMD_HOST_SET_IP = 0x10000002
CMD_HOST_NETWORK_IDLE = 0x10000004

CMD_UPDATE_START = 0x20000001
CMD_UPDATE_DATA = 0x20000002
CMD_UPDATE_FINISH = 0x20000003

CMD_DSP_UPLOAD_ALL_DATA = 0x90000000
CMD_DSP_UPLOAD_REF_DATA = 0x90000001
CMD_RESPONSE_STATE = 0xA0000000

FRAME_HEADER_SIZE = struct.calcsize('<16sIIIIIIIII')

# ==========================================
# 2. UI 类定义
# ==========================================
class OTDR_UI:
    def __init__(self, root):
        self.root = root
        self.root.title("千兆网 OTDR 测试平台 (单通道满血全速版)")
        self.root.geometry("1650x950")

        self.target_ip = tk.StringVar(value="192.168.1.10")
        self.target_port = tk.StringVar(value="5000")
        self.capture_points = tk.StringVar(value="196605")
        self.adc_delay_var = tk.StringVar(value="0")
        self.refresh_interval = tk.StringVar(value="10")

        self.hw_acc_times = tk.StringVar(value="1024")
        self.sw_acc_times_var = tk.StringVar(value="1")

        self.lambda_nm = tk.StringVar(value="1550")
        self.pulse_width_ns = tk.StringVar(value="80")
        self.measure_time_ms = tk.StringVar(value="15000")
        self.refractive_index = tk.StringVar(value="1.466")
        self.end_threshold = tk.StringVar(value="5.0")
        self.non_reflect_threshold = tk.StringVar(value="0.0")
        self.export_golay_raw_csv = tk.BooleanVar(value=False)
        self.display_lpf_enable = tk.BooleanVar(value=True)
        self.rcos_filter_enable = tk.BooleanVar(value=True)
        self.x_axis_unit = tk.StringVar(value="公里(km)")
        self.upload_format = tk.StringVar(value="实验(float32)")

        self.sock = None
        self.connected = False
        self.is_measuring = False
        self.plot_queue = None
        self.latest_measurement = None
        self.ota_ack_event = threading.Event()

        self.build_ui()
        self.root.after(200, self.periodic_plot_update)

    def build_ui(self):
        self.root.option_add("*Font", ("Microsoft YaHei UI", 10))
        ctrl_frame = tk.Frame(self.root, pady=6)
        ctrl_frame.pack(fill=tk.X, padx=10)

        def label(parent, text, pad=(8, 2)):
            tk.Label(parent, text=text).pack(side=tk.LEFT, padx=pad)

        def entry(parent, variable, width):
            widget = tk.Entry(parent, textvariable=variable, width=width)
            widget.pack(side=tk.LEFT, padx=(0, 8), ipady=2)
            return widget

        def combo(parent, variable, values, width, readonly=True):
            widget = ttk.Combobox(
                parent,
                textvariable=variable,
                values=values,
                width=width,
                state="readonly" if readonly else "normal",
            )
            widget.pack(side=tk.LEFT, padx=(0, 8), ipady=1)
            return widget

        def action_button(parent, text, command, bg, width, state=tk.NORMAL):
            widget = tk.Button(
                parent,
                text=text,
                command=command,
                bg=bg,
                fg="white",
                width=width,
                height=1,
                relief=tk.RAISED,
                bd=1,
                padx=6,
                pady=4,
                state=state,
            )
            widget.pack(side=tk.LEFT, padx=4, pady=2)
            return widget

        row_actions = tk.Frame(ctrl_frame)
        row_actions.pack(fill=tk.X, pady=2)
        row_acquire = tk.Frame(ctrl_frame)
        row_acquire.pack(fill=tk.X, pady=2)
        row_options = tk.Frame(ctrl_frame)
        row_options.pack(fill=tk.X, pady=2)

        label(row_actions, "IP:", (0, 2))
        entry(row_actions, self.target_ip, 13)
        self.btn_conn = action_button(
            row_actions,
            "连接(UDP)",
            self.toggle_conn,
            "#4CAF50",
            10,
        )
        self.btn_set_ip = action_button(
            row_actions,
            "改下位机IP",
            self.set_device_ip_dialog,
            "#607D8B",
            10,
            tk.DISABLED,
        )

        label(row_actions, "通道:")
        self.cb_ch_sel = combo(row_actions, None, ["0", "1", "2", "3"], 3)
        self.cb_ch_sel.current(0)

        label(row_actions, "工作模式:")
        self.cb_work_mode = combo(
            row_actions,
            None,
            ["高精度(Golay)", "快速(单脉冲)"],
            13,
        )
        self.cb_work_mode.current(0)

        label(row_actions, "触发:")
        self.cb_mode = combo(row_actions, None, ["单次", "连续"], 5)
        self.cb_mode.current(1)

        self.btn_start = action_button(
            row_actions,
            "启动测试",
            self.start_measure,
            "#2196F3",
            10,
            tk.DISABLED,
        )
        self.btn_stop = action_button(
            row_actions,
            "停止",
            self.stop_measure,
            "#f44336",
            8,
            tk.DISABLED,
        )
        self.btn_save_measurement = action_button(
            row_actions,
            "保存当前测量",
            self.save_current_measurement,
            "#455A64",
            12,
            tk.DISABLED,
        )
        self.btn_ota = action_button(
            row_actions,
            "固件升级(OTA)",
            self.start_ota_dialog,
            "#FF9800",
            12,
            tk.DISABLED,
        )

        label(row_acquire, "采样率:", (0, 2))
        self.cb_sample_rate = combo(
            row_acquire,
            None,
            ["250MHz(全速)", "125MHz(降采样)"],
            14,
        )
        self.cb_sample_rate.current(0)

        label(row_acquire, "点数:")
        entry(row_acquire, self.capture_points, 9)
        label(row_acquire, "延时:")
        entry(row_acquire, self.adc_delay_var, 6)
        label(row_acquire, "硬件叠加:")
        self.cb_hw_acc = combo(
            row_acquire,
            self.hw_acc_times,
            ["1", "2", "4", "8", "16", "32", "64", "128", "256", "512", "1024"],
            6,
            readonly=False,
        )
        label(row_acquire, "软件叠加:")
        entry(row_acquire, self.sw_acc_times_var, 6)
        label(row_acquire, "波长(nm):")
        self.cb_lambda = combo(
            row_acquire,
            self.lambda_nm,
            ["1310", "1550", "1625", "850", "1300"],
            7,
            readonly=False,
        )
        label(row_acquire, "脉宽(ns):")
        self.cb_pw = combo(
            row_acquire,
            self.pulse_width_ns,
            [
                "8", "12", "16", "20", "24", "28", "32", "36",
                "40", "80", "100", "500", "1000", "5000", "10000", "20000",
            ],
            7,
            readonly=False,
        )
        label(row_acquire, "测量时间(ms):")
        entry(row_acquire, self.measure_time_ms, 9)
        label(row_acquire, "折射率(n):")
        entry(row_acquire, self.refractive_index, 8)

        tk.Checkbutton(
            row_options,
            text="Golay实验数据",
            variable=self.export_golay_raw_csv,
        ).pack(side=tk.LEFT, padx=(0, 10))
        tk.Checkbutton(
            row_options,
            text="显示低通",
            variable=self.display_lpf_enable,
        ).pack(side=tk.LEFT, padx=10)
        tk.Checkbutton(
            row_options,
            text="相关升余弦",
            variable=self.rcos_filter_enable,
        ).pack(side=tk.LEFT, padx=10)

        label(row_options, "上传格式:")
        self.cb_upload_format = combo(
            row_options,
            self.upload_format,
            ["实验(float32)", "交付(标准协议)"],
            14,
        )
        self.cb_upload_format.current(0)
        label(row_options, "横坐标:")
        self.cb_x_axis_unit = combo(
            row_options,
            self.x_axis_unit,
            ["点数", "米(m)", "公里(km)"],
            9,
        )
        self.cb_x_axis_unit.current(2)
        label(row_options, "结束门限(dB):")
        entry(row_options, self.end_threshold, 6)
        label(row_options, "非反射门限:")
        entry(row_options, self.non_reflect_threshold, 6)

        main_pane = tk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        main_pane.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)

        left_frame = tk.Frame(main_pane)
        self.fig, self.ax = plt.subplots(1, 1, figsize=(9, 8))
        self.fig.subplots_adjust(left=0.08, right=0.98, top=0.92, bottom=0.08)
        self.canvas = FigureCanvasTkAgg(self.fig, master=left_frame)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        self.ax.grid(True)

        self.toolbar = NavigationToolbar2Tk(self.canvas, left_frame)
        self.toolbar.update()
        self.toolbar.pack(side=tk.BOTTOM, fill=tk.X)
        main_pane.add(left_frame, stretch="always")

        right_frame = tk.Frame(main_pane, width=400)
        tk.Label(right_frame, text="📍 链路事件分析表", font=("Arial", 11, "bold")).pack(pady=5)

        columns = ("No.", "Location", "Type", "Ins.Loss(dB)", "Ref.Loss(dB)", "Atten.")
        self.tree = ttk.Treeview(right_frame, columns=columns, show="headings", height=20)
        for col, title, width in zip(columns, ["序号", "位置", "事件类型", "插损(dB)", "反射(dB)", "衰减系数"],
                                     [40, 80, 100, 70, 70, 70]):
            self.tree.heading(col, text=title)
            self.tree.column(col, width=width, anchor=tk.CENTER)
        self.tree.pack(fill=tk.BOTH, expand=True)
        main_pane.add(right_frame)

        self.log_txt = scrolledtext.ScrolledText(self.root, height=5)
        self.log_txt.pack(fill=tk.X, padx=10, pady=5)
        self.log("系统启动。单通道高性能硬件叠加版已就绪！")

    def log(self, msg):
        self.log_txt.insert(tk.END, msg + "\n")
        self.log_txt.see(tk.END)

    def km_per_sample(self):
        try:
            n = float(self.refractive_index.get())
            if n <= 0:
                n = 1.4685
        except ValueError:
            n = 1.4685
        sample_period_s = 8e-9 if self.cb_sample_rate.current() == 1 else 4e-9
        return 299792458.0 * sample_period_s / (2.0 * n) / 1000.0

    def sample_to_km(self, sample, include_delay=True):
        try:
            delay = int(self.adc_delay_var.get()) if include_delay else 0
        except ValueError:
            delay = 0
        return (np.asarray(sample, dtype=np.float64) + delay) * self.km_per_sample()

    def sample_to_m(self, sample, include_delay=True):
        return self.sample_to_km(sample, include_delay=include_delay) * 1000.0

    def x_axis_values(self, sample, include_delay=True):
        unit = self.x_axis_unit.get()
        sample_arr = np.asarray(sample, dtype=np.float64)
        if unit.startswith("米"):
            return self.sample_to_m(sample_arr, include_delay=include_delay)
        if unit.startswith("公里"):
            return self.sample_to_km(sample_arr, include_delay=include_delay)
        return sample_arr

    def x_axis_label(self):
        unit = self.x_axis_unit.get()
        if unit.startswith("米"):
            return "Distance (m)"
        if unit.startswith("公里"):
            return "Distance (km)"
        return "Sample index"

    def format_x_value(self, sample):
        unit = self.x_axis_unit.get()
        if unit.startswith("米"):
            return f"{float(self.sample_to_m(sample)):.3f} m"
        if unit.startswith("公里"):
            return f"{float(self.sample_to_km(sample)):.3f} km"
        return str(int(sample))

    def sample_period_label(self):
        return "8ns/sample" if self.cb_sample_rate.current() == 1 else "4ns/sample"

    def to_db(self, y, floor_val=1e-6):
        y_abs = np.abs(np.asarray(y, dtype=np.float64))
        return 10.0 * np.log10(np.maximum(y_abs, floor_val))

    def to_normalized_db(self, y, floor_val=1e-6, ignore_tail=True):
        y_abs = np.abs(np.asarray(y, dtype=np.float64))
        valid = y_abs
        if ignore_tail and y_abs.size > 100:
            valid_len = int(y_abs.size * 0.97)
            valid_len = max(1, min(valid_len, y_abs.size))
            valid = y_abs[:valid_len]
        max_abs = np.max(valid) if valid.size else 0.0
        if max_abs <= floor_val:
            max_abs = 1.0
        return 10.0 * np.log10(np.maximum(y_abs / max_abs, floor_val))

    def single_pulse_power_like(self, y):
        y = np.asarray(y, dtype=np.float64)
        if y.size == 0:
            return y

        tail_len = max(32, min(y.size // 20, 4096))
        baseline = float(np.median(y[-tail_len:]))

        probe_len = max(32, min(y.size // 20, 4096))
        head = y[:probe_len]
        head_hi = float(np.percentile(head, 95))
        head_lo = float(np.percentile(head, 5))

        # APD polarity is not guaranteed. Pick the direction whose early trace
        # moves farther away from the tail baseline, then clip non-positive power.
        if abs(head_hi - baseline) >= abs(baseline - head_lo):
            power = y - baseline
        else:
            power = baseline - y

        return np.maximum(power, 0.0)

    def display_lpf(self, y):
        y = np.asarray(y, dtype=np.float64)
        if not self.display_lpf_enable.get() or y.size < 3:
            return y
        out = y.copy()
        out[1:-1] = (y[:-2] + 2.0 * y[1:-1] + y[2:]) * 0.25
        return out

    def save_current_measurement(self):
        data = self.latest_measurement
        if not data:
            messagebox.showinfo("保存测量", "当前还没有可保存的测量数据。")
            return

        kind = data.get("data_kind", "unknown")
        curve = np.asarray(data.get("curve", []), dtype=np.float64)
        pts = int(data.get("points_per_trace", 0))
        if pts <= 0 or curve.size < pts:
            messagebox.showerror("保存失败", "当前测量数据长度不完整。")
            return

        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        mode_name = {
            "single": "single_pulse",
            "golay_experiment": "golay_ap_an_bp_bn_corr",
            "golay_result": "golay_ap_corr",
        }.get(kind, "otdr_measurement")
        out_path = filedialog.asksaveasfilename(
            title="保存当前测量 CSV",
            initialdir=os.path.dirname(__file__),
            initialfile=f"{mode_name}_{ts}.csv",
            defaultextension=".csv",
            filetypes=[("CSV Files", "*.csv"), ("All Files", "*.*")],
        )
        if not out_path:
            return

        sample = np.arange(pts, dtype=np.int64)
        distance_m = self.sample_to_m(sample)

        if kind == "single":
            columns = np.column_stack((sample, distance_m, curve[:pts]))
            header = "sample,distance_m,single_raw"
        elif kind == "golay_experiment" and curve.size >= pts * 5:
            columns = np.column_stack((
                sample,
                distance_m,
                curve[0 * pts:1 * pts],
                curve[1 * pts:2 * pts],
                curve[2 * pts:3 * pts],
                curve[3 * pts:4 * pts],
                curve[4 * pts:5 * pts],
            ))
            header = "sample,distance_m,ap,an,bp,bn,final_corr"
        elif kind == "golay_result" and curve.size >= pts * 2:
            columns = np.column_stack((
                sample,
                distance_m,
                curve[:pts],
                curve[pts:2 * pts],
            ))
            header = "sample,distance_m,ap,final_corr"
        else:
            messagebox.showerror("保存失败", f"暂不支持当前数据布局: {kind}")
            return

        np.savetxt(out_path, columns, delimiter=",", fmt="%.9g", header=header, comments="")

        metadata = dict(data.get("metadata", {}))
        metadata.update({
            "data_kind": kind,
            "csv_file": os.path.basename(out_path),
            "points_per_trace": pts,
            "columns": header.split(","),
            "saved_at": datetime.now().isoformat(timespec="seconds"),
            "hardware_accumulation": int(self.hw_acc_times.get()),
            "software_accumulation": int(self.sw_acc_times_var.get()),
            "adc_delay_samples": int(self.adc_delay_var.get()),
            "rcos_reference_enabled": bool(self.rcos_filter_enable.get()),
            "upload_format": self.upload_format.get(),
        })
        json_path = os.path.splitext(out_path)[0] + ".json"
        with open(json_path, "w", encoding="utf-8") as f:
            json.dump(metadata, f, ensure_ascii=False, indent=2)

        self.log(f"已保存当前测量 CSV: {out_path}")
        self.log(f"已保存测量参数 JSON: {json_path}")





    def make_header(self, cmd_code, data_len):
        return struct.pack('<16sIIIIIIIII', FRAME_SYNC, 52 + data_len, 1, 0, 0, 0, 0, 0xffffeeee, cmd_code, data_len)

    def toggle_conn(self):
        if self.connected:
            self.stop_measure()
            self.connected = False
            if self.sock: self.sock.close(); self.sock = None
            self.btn_conn.config(text="连接(UDP)", bg="#4CAF50")
            self.btn_start.config(state=tk.DISABLED)
            self.btn_stop.config(state=tk.DISABLED)
            self.btn_ota.config(state=tk.DISABLED)
            self.btn_set_ip.config(state=tk.DISABLED)
        else:
            try:
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                self.sock.settimeout(1.0)
                self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 * 1024 * 1024)
                self.sock.connect((self.target_ip.get(), int(self.target_port.get())))

                self.connected = True
                self.btn_conn.config(text="断开", bg="#9E9E9E")
                self.btn_start.config(state=tk.NORMAL)
                self.btn_ota.config(state=tk.NORMAL)
                self.btn_set_ip.config(state=tk.NORMAL)

                self.sock.sendall(self.make_header(CMD_HOST_NETWORK_IDLE, 0))
                threading.Thread(target=self.udp_recv_thread, daemon=True).start()
                self.log(f"UDP 通道已绑定 -> {self.target_ip.get()}:{self.target_port.get()}")
            except Exception as e:
                messagebox.showerror("连接失败", str(e))

    def set_device_ip_dialog(self):
        if not self.connected or self.sock is None: return
        from tkinter import simpledialog
        new_ip = simpledialog.askstring("修改设备IP", "请输入下位机的新 IP 地址:", initialvalue="192.168.1.10")
        if new_ip:
            try:
                mask = "255.255.255.0"
                gw = "192.168.1.1"
                payload = struct.pack('16s 16s 16s',
                                      new_ip.encode('ascii'),
                                      mask.encode('ascii'),
                                      gw.encode('ascii'))
                self.sock.sendall(self.make_header(CMD_HOST_SET_IP, len(payload)) + payload)
                self.log(f"网络: 已下发修改设备 IP 指令 -> {new_ip}")
            except Exception as e:
                self.log(f"IP 修改指令发送失败: {e}")

    def start_ota_dialog(self):
        if not self.connected or self.sock is None: return
        filepath = filedialog.askopenfilename(title="选择 OTA 固件包或 BOOT.bin",
                                              filetypes=[("OTA Package", "*.pkg"),
                                                         ("BIN Files", "*.bin"),
                                                         ("All Files", "*.*")])
        if not filepath: return

        with open(filepath, 'rb') as f:
            bin_data = f.read()

        total_size = len(bin_data)
        self.log(f"准备升级固件: {filepath} ({total_size / 1024:.2f} KB)")
        self.btn_start.config(state=tk.DISABLED)
        self.btn_ota.config(state=tk.DISABLED)

        self.ota_win = tk.Toplevel(self.root)
        self.ota_win.title("OTA 升级中...")
        self.ota_win.geometry("400x120")
        self.ota_win.transient(self.root)
        self.ota_win.grab_set()

        tk.Label(self.ota_win, text="正在下发固件包，请绝对不要断电！", fg="red").pack(pady=10)
        self.ota_progress = ttk.Progressbar(self.ota_win, orient=tk.HORIZONTAL, length=320, mode='determinate')
        self.ota_progress.pack(pady=5)
        threading.Thread(target=self.ota_task, args=(bin_data,), daemon=True).start()

    def ota_task(self, bin_data):
        total_size = len(bin_data)
        self.ota_ack_event.clear()
        payload = struct.pack('<I', total_size)
        try:
            self.sock.sendall(self.make_header(CMD_UPDATE_START, len(payload)) + payload)
        except Exception as e:
            self.root.after(0, self.ota_finish, f"发送失败: {e}");
            return

        if not self.ota_ack_event.wait(3.0): self.root.after(0, self.ota_finish, "START命令超时"); return

        chunk_size = 1024
        offset = 0
        while offset < total_size:
            chunk = bin_data[offset:offset + chunk_size]
            payload = struct.pack('<I', offset) + chunk

            retry = 0
            while retry < 3:
                self.ota_ack_event.clear()
                try:
                    self.sock.sendall(self.make_header(CMD_UPDATE_DATA, len(payload)) + payload)
                except:
                    pass
                if self.ota_ack_event.wait(0.5): break
                retry += 1

            if retry == 3: self.root.after(0, self.ota_finish, f"数据包 {offset} 连续三次无响应，中止！"); return
            offset += len(chunk)
            self.root.after(0, self.update_ota_progress, int((offset / total_size) * 100))

        self.ota_ack_event.clear()
        try:
            self.sock.sendall(self.make_header(CMD_UPDATE_FINISH, 0))
        except:
            pass

        if self.ota_ack_event.wait(3.0):
            self.root.after(0, self.ota_finish, "✅ 下发成功！等待板卡重启。")
        else:
            self.root.after(0, self.ota_finish, "⚠️ 等待 FINISH ACK 超时。请观察串口输出。")

    def update_ota_progress(self, pct):
        if hasattr(self, 'ota_progress'):
            self.ota_progress['value'] = pct
            self.ota_win.update_idletasks()

    def ota_finish(self, msg):
        self.log(f"[OTA] {msg}")
        messagebox.showinfo("OTA 结果", msg)
        if hasattr(self, 'ota_win'): self.ota_win.destroy()
        if self.connected:
            self.btn_start.config(state=tk.NORMAL)
            self.btn_ota.config(state=tk.NORMAL)

    def start_measure(self):
        if not self.connected or self.sock is None: return
        self.is_measuring = True
        self.btn_start.config(state=tk.DISABLED)
        self.btn_stop.config(state=tk.NORMAL)
        self.btn_ota.config(state=tk.DISABLED)

        for item in self.tree.get_children(): self.tree.delete(item)

        try:
            com_sel_idx = int(self.cb_ch_sel.get())
            hw_acc_times = int(self.hw_acc_times.get())
            sw_acc_times = int(self.sw_acc_times_var.get())
            pts = int(self.capture_points.get())
            adc_dly = int(self.adc_delay_var.get())

            opt_mode = 0 if self.cb_work_mode.current() == 0 else 1
            # 【新增】提取降采样状态
            downsample_en = 1 if self.cb_sample_rate.current() == 1 else 0
            enable_refresh = 2 if (opt_mode == 0 and self.export_golay_raw_csv.get()) else 1
            if enable_refresh == 2:
                self.cb_mode.current(0)

            mode_idx = 1 if self.cb_mode.current() == 0 else 2
            lam_nm = int(self.lambda_nm.get())
            pw_ns = int(self.pulse_width_ns.get())
            meas_time = int(self.measure_time_ms.get())
            n_val = float(self.refractive_index.get())
            end_th = float(self.end_threshold.get())
            nr_th = float(self.non_reflect_threshold.get())

            standard_range_m = int(pts * 0.3)
            payload_len = 76
            rcos_enable = 1 if self.rcos_filter_enable.get() else 0
            upload_mode = 1 if self.upload_format.get().startswith("交付") else 0

            # 【修改】借用 Ctrl 结构体中的第三个字段(原先是0, 即RSVD)来传递 downsample_en
            payload = struct.pack('<II IIIII IIIIfff IIIIIII',
                                  CMD_HOST_START_MEASURE, payload_len,
                                  # --- Ctrl ---
                                  mode_idx, opt_mode, downsample_en, enable_refresh, 1000,
                                  # --- State ---
                                  lam_nm, standard_range_m, pw_ns, meas_time, n_val, end_th, nr_th,
                                  # --- 专属硬件扩展区 ---
                                  hw_acc_times, sw_acc_times, com_sel_idx, adc_dly, pts,
                                  rcos_enable, upload_mode)

            self.sock.sendall(self.make_header(CMD_HOST_START_MEASURE, len(payload)) + payload)

            mode_str = "高精度(Golay)" if opt_mode == 0 else "快速(单脉冲)"
            ds_str = "降采样" if downsample_en == 1 else "全速"
            rcos_str = "升余弦相关" if rcos_enable else "矩形相关"
            upload_str = "标准协议" if upload_mode else "float32实验"
            raw_export_str = ", Golay五组浮点实验数据" if enable_refresh == 2 else ""
            self.log(
                f"▶ 启动 {mode_str} 测试! CH:{com_sel_idx}, 模式:{ds_str}, "
                f"参考:{rcos_str}, 上传:{upload_str}, 采集点数:{pts}, 硬件叠加:{hw_acc_times}, "
                f"软件叠加:{sw_acc_times}{raw_export_str}")
        except Exception as e:
            self.log(f"参数错误或发送失败: {e}")

    def stop_measure(self):
        if not self.connected or self.sock is None: return
        self.is_measuring = False
        self.btn_start.config(state=tk.NORMAL)
        self.btn_stop.config(state=tk.DISABLED)
        self.btn_ota.config(state=tk.NORMAL)

        stop_type = 2
        payload = struct.pack('<II', stop_type, 0)
        self.sock.sendall(self.make_header(CMD_HOST_STOP_MEASURE, len(payload)) + payload)
        self.log("⏹ 已发送终止命令 (强制下位机提前结算当前数据)")

    def send_ack(self):
        if self.connected and self.sock:
            try:
                self.sock.sendall(self.make_header(CMD_HOST_NETWORK_IDLE, 0))
            except:
                pass

    def udp_recv_thread(self):
        recv_buf = bytearray()
        while self.connected:
            try:
                data = self.sock.recv(65536)
                if data: recv_buf.extend(data)
            except socket.timeout:
                if self.is_measuring: self.send_ack()
                continue
            except:
                break

            while len(recv_buf) >= FRAME_HEADER_SIZE:
                sync_pos = recv_buf.find(FRAME_SYNC)
                if sync_pos == -1:
                    recv_buf.clear();
                    break
                if sync_pos > 0: del recv_buf[:sync_pos]
                if len(recv_buf) < FRAME_HEADER_SIZE: break

                sync, total_len, rev, ftype, src, dst, pkt_id, rsvd1, cmd, dlen = \
                    struct.unpack('<16sIIIIIIIII', recv_buf[:FRAME_HEADER_SIZE])

                if total_len < FRAME_HEADER_SIZE or total_len > 64 * 1024 * 1024:
                    del recv_buf[:FRAME_HEADER_SIZE];
                    continue
                if len(recv_buf) < total_len: break

                frame_data = bytes(recv_buf[:total_len])
                del recv_buf[:total_len]
                payload = frame_data[FRAME_HEADER_SIZE:FRAME_HEADER_SIZE + dlen]

                if cmd == CMD_RESPONSE_STATE:
                    if len(payload) >= 4:
                        state_code = struct.unpack('<I', payload[:4])[0]
                        if state_code == 0: self.ota_ack_event.set()
                    continue

                elif cmd == CMD_DSP_UPLOAD_ALL_DATA:
                    measure_param = struct.unpack('<IIIIIffffffII', payload[:52])
                    otdr_mode = int(measure_param[11])
                    is_single_raw_int16 = False
                    data_num = struct.unpack('<I', payload[52:56])[0]
                    is_float32 = (data_num & 0x80000000) != 0
                    is_int32 = (data_num & 0x40000000) != 0
                    point_count = data_num & 0x3fffffff
                    try:
                        expected_pts = int(self.capture_points.get())
                    except ValueError:
                        expected_pts = 0
                    is_single_raw_float = is_float32 and expected_pts > 0 and point_count == expected_pts
                    is_golay_experiment_float = (
                        is_float32
                        and expected_pts > 0
                        and point_count == expected_pts * 5
                        and self.cb_work_mode.current() == 0
                    )
                    is_single_pulse_corr_float = (
                        is_float32
                        and expected_pts > 0
                        and point_count == expected_pts * 2
                        and self.cb_work_mode.current() == 1
                    )

                    if is_float32:
                        adc_offset = 56 + point_count * 4
                        raw_adc_bytes = payload[56:adc_offset]
                        curve_data = np.frombuffer(raw_adc_bytes, dtype='<f4').astype(np.float64)
                        is_official_db = False
                    elif is_int32:
                        adc_offset = 56 + point_count * 4
                        raw_adc_bytes = payload[56:adc_offset]
                        curve_data = np.frombuffer(raw_adc_bytes, dtype='<i4').astype(np.float64)
                        is_official_db = False
                    elif otdr_mode == 1:
                        is_single_raw_int16 = True
                        adc_offset = 56 + point_count * 2
                        raw_adc_bytes = payload[56:adc_offset]
                        curve_data = np.frombuffer(raw_adc_bytes, dtype='<i2').astype(np.float64)
                        is_official_db = False
                    else:
                        adc_offset = 56 + point_count * 2
                        raw_adc_bytes = payload[56:adc_offset]
                        curve_data = np.frombuffer(raw_adc_bytes, dtype='<u2').astype(np.float64) / 1000.0 - 5.0
                        is_official_db = True

                    event_num = struct.unpack('<I', payload[adc_offset:adc_offset + 4])[0]
                    event_offset = adc_offset + 4

                    parsed_events = []
                    for e in range(event_num):
                        evt_data = struct.unpack('<IIffff', payload[event_offset: event_offset + 24])
                        event_type = int(evt_data[1])
                        reflect_loss = evt_data[2] if evt_data[2] != 8192.0 else "-"
                        insert_loss = evt_data[3] if evt_data[3] != 8192.0 else "-"
                        attenuation = evt_data[4] if evt_data[4] != 8192.0 else "-"
                        if event_type == 1:
                            event_name = "反射(接头)"
                        elif event_type == 2:
                            event_name = "非反射(插损)"
                        elif event_type == 3:
                            event_name = (
                                "结束(反射)"
                                if reflect_loss != "-" and reflect_loss > 0.0
                                else "结束(断点)"
                            )
                        else:
                            event_name = f"未知事件({event_type})"

                        evt_dict = {
                            'xlabel': evt_data[0],
                            'type': event_name,
                            'ref_loss': reflect_loss,
                            'ins_loss': insert_loss,
                            'atten': attenuation,
                        }
                        parsed_events.append(evt_dict)
                        event_offset += 24

                    official_export = None
                    if is_single_raw_float:
                        data_kind = "single"
                        points_per_trace = expected_pts
                    elif is_golay_experiment_float:
                        data_kind = "golay_experiment"
                        points_per_trace = expected_pts
                    elif is_float32 and expected_pts > 0 and point_count == expected_pts * 2:
                        data_kind = "golay_result"
                        points_per_trace = expected_pts
                    else:
                        data_kind = "unknown"
                        points_per_trace = expected_pts if expected_pts > 0 else point_count

                    metadata = {
                        "sample_rate_hz": int(measure_param[0]),
                        "measure_length_m": int(measure_param[1]),
                        "pulse_width_ns": int(measure_param[2]),
                        "wavelength_nm": int(measure_param[3]),
                        "measure_time_ms": int(measure_param[4]),
                        "refractive_index": float(measure_param[5]),
                        "otdr_mode": otdr_mode,
                        "measure_mode": int(measure_param[12]),
                    }
                    self.plot_queue = {
                        'curve': curve_data,
                        'events': parsed_events,
                        'raw_bytes': raw_adc_bytes,
                        'official_export': official_export,
                        'is_official_db': is_official_db,
                        'is_single_raw_int16': is_single_raw_int16,
                        'is_single_raw_float': is_single_raw_float,
                        'is_single_pulse_corr_float': is_single_pulse_corr_float,
                        'is_golay_experiment_float': is_golay_experiment_float,
                        'is_int32': is_int32,
                        'otdr_mode': otdr_mode,
                        'data_kind': data_kind,
                        'points_per_trace': points_per_trace,
                        'metadata': metadata,
                    }

    def curve_stats_text(self, name, y):
        if len(y) == 0:
            return f"{name}: empty"
        min_idx = int(np.argmin(y))
        max_idx = int(np.argmax(y))
        abs_idx = int(np.argmax(np.abs(y)))
        return (
            f"{name}: min={y[min_idx]:.0f}@{min_idx}, "
            f"max={y[max_idx]:.0f}@{max_idx}, "
            f"max_abs={y[abs_idx]:.0f}@{abs_idx}, mean={np.mean(y):.1f}"
        )

    @staticmethod
    def format_event_metric(value, decimals=2):
        if isinstance(value, str):
            return value
        return f"{float(value):.{decimals}f}"

    def draw_event_markers(self, ax, events, y_top):
        if not events:
            return
        for evt in events:
            evt_x = float(self.x_axis_values(evt['xlabel']))
            ax.axvline(x=evt_x, color='r', linestyle='--', alpha=0.7)
            ax.text(
                evt_x,
                y_top,
                f" {evt['type']} {self.format_x_value(evt['xlabel'])}",
                color='r',
                verticalalignment='top'
            )

    def try_plot_and_save_raw_golay_int32(self, raw_bytes):
        try:
            pts = int(self.capture_points.get())
        except ValueError:
            return False

        expected_bytes = pts * 4 * 4
        if pts <= 0 or len(raw_bytes) != expected_bytes:
            return False

        raw_i32 = np.frombuffer(raw_bytes, dtype='<i4')
        ap = raw_i32[0:pts].astype(np.int32)
        an = raw_i32[pts:pts * 2].astype(np.int32)
        bp = raw_i32[pts * 2:pts * 3].astype(np.int32)
        bn = raw_i32[pts * 3:pts * 4].astype(np.int32)
        sample = np.arange(pts, dtype=np.int32)
        x = self.x_axis_values(sample)

        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        default_name = f"golay_raw_ap_an_bp_bn_{ts}.csv"
        out_path = filedialog.asksaveasfilename(
            title="保存 Golay 四路原始数据 CSV",
            initialdir=os.path.dirname(__file__),
            initialfile=default_name,
            defaultextension=".csv",
            filetypes=[("CSV Files", "*.csv"), ("All Files", "*.*")]
        )
        if out_path:
            out = np.column_stack((sample, ap, an, bp, bn))
            np.savetxt(out_path, out, delimiter=',', fmt='%d',
                       header='sample,ap,an,bp,bn', comments='')

        self.fig.clear()
        ax_ap, ax_an, ax_bp, ax_bn = self.fig.subplots(4, 1, sharex=True)
        self.fig.subplots_adjust(left=0.08, right=0.98, top=0.92, bottom=0.08, hspace=0.08)
        for ax in (ax_ap, ax_an, ax_bp, ax_bn):
            ax.grid(True)
        ax_ap.plot(x, ap, color='tab:blue', linewidth=1.0)
        ax_an.plot(x, an, color='tab:orange', linewidth=1.0)
        ax_bp.plot(x, bp, color='tab:green', linewidth=1.0)
        ax_bn.plot(x, bn, color='tab:red', linewidth=1.0)
        ax_ap.set_ylabel("AP int32")
        ax_an.set_ylabel("AN int32")
        ax_bp.set_ylabel("BP int32")
        ax_bn.set_ylabel("BN int32")
        ax_bn.set_xlabel(self.x_axis_label())
        ax_ap.set_title(f"Raw Golay AP/AN/BP/BN frames for MATLAB, points={pts}")
        self.ax = ax_bn
        self.canvas.draw()

        if out_path:
            self.log(f"已保存Golay原始AP/AN/BP/BN CSV: {out_path}")
        else:
            self.log("Golay原始AP/AN/BP/BN CSV保存已取消")
        self.log(self.curve_stats_text("AP raw", ap))
        self.log(self.curve_stats_text("AN raw", an))
        self.log(self.curve_stats_text("BP raw", bp))
        self.log(self.curve_stats_text("BN raw", bn))
        return True

    def plot_ap_and_corr_debug(self, curve, pts, events=None):
        ap = curve[:pts]
        corr = curve[pts:pts * 2]
        x = self.x_axis_values(np.arange(pts))
        ap_plot = self.display_lpf(ap)
        corr_plot = self.display_lpf(corr)
        corr_db = self.to_normalized_db(corr_plot)

        self.fig.clear()
        ax_raw, ax_corr = self.fig.subplots(2, 1, sharex=True)
        self.fig.subplots_adjust(left=0.08, right=0.98, top=0.92, bottom=0.08, hspace=0.12)
        ax_raw.grid(True)
        ax_corr.grid(True)

        ax_raw.plot(x, ap_plot, color='tab:blue', linewidth=1.0)
        ax_corr.plot(x, corr_db, color='tab:red', linewidth=1.0)
        self.draw_event_markers(ax_corr, events, float(np.max(corr_db)) if corr_db.size else 0.0)

        # [新增] 限定第一个图的纵坐标范围
        ax_raw.set_ylim(-10000, 10000)

        ax_raw.set_ylabel("AP raw")
        ax_corr.set_ylabel("CorrA+CorrB norm (dB)")
        ax_corr.set_xlabel(self.x_axis_label())
        lpf_text = ", display LPF on" if self.display_lpf_enable.get() else ""
        ax_raw.set_title(f"AP raw + final Golay correlation, points={pts}, {self.sample_period_label()}{lpf_text}")
        self.ax = ax_corr
        self.canvas.draw()

        self.log(self.curve_stats_text("AP raw", ap))
        self.log(self.curve_stats_text("CorrA+CorrB raw float", corr))

    def plot_golay_experiment_debug(self, curve, pts):
        ap = np.asarray(curve[0 * pts:1 * pts], dtype=np.float64)
        corr = np.asarray(curve[4 * pts:5 * pts], dtype=np.float64)
        x = self.x_axis_values(np.arange(pts))
        ap_plot = self.display_lpf(ap)
        corr_plot = self.display_lpf(corr)
        corr_db = self.to_normalized_db(corr_plot)

        self.fig.clear()
        ax_raw, ax_corr = self.fig.subplots(2, 1, sharex=True)
        self.fig.subplots_adjust(left=0.08, right=0.98, top=0.92, bottom=0.08, hspace=0.12)
        ax_raw.grid(True)
        ax_corr.grid(True)
        ax_raw.plot(x, ap_plot, color="tab:blue", linewidth=1.0)
        ax_corr.plot(x, corr_db, color="tab:red", linewidth=1.0)
        ax_raw.set_ylabel("AP raw")
        ax_corr.set_ylabel("Final corr norm (dB)")
        ax_corr.set_xlabel(self.x_axis_label())
        ax_raw.set_title(
            f"Golay experiment AP + final correlation, points={pts}, "
            f"{self.sample_period_label()}"
        )
        self.ax = ax_corr
        self.canvas.draw()

        self.log(self.curve_stats_text("AP raw float", ap))
        self.log(self.curve_stats_text("Final correlation float", corr))

    def plot_single_raw_norm_debug(self, curve, pts, events=None):
        raw = np.asarray(curve[:pts], dtype=np.float64)
        x = self.x_axis_values(np.arange(pts))
        raw_plot = self.display_lpf(raw)
        power_plot = self.display_lpf(self.single_pulse_power_like(raw))
        raw_db = self.to_normalized_db(power_plot, ignore_tail=True)

        self.fig.clear()
        ax_raw, ax_db = self.fig.subplots(2, 1, sharex=True)
        self.fig.subplots_adjust(left=0.08, right=0.98, top=0.92, bottom=0.08, hspace=0.12)
        ax_raw.grid(True)
        ax_db.grid(True)

        ax_raw.plot(x, raw_plot, color='tab:blue', linewidth=1.0)
        ax_db.plot(x, raw_db, color='tab:red', linewidth=1.0)
        self.draw_event_markers(ax_db, events, float(np.max(raw_db)) if raw_db.size else 0.0)

        # [新增] 限定第一个图的纵坐标范围
        ax_raw.set_ylim(-10000, 10000)

        ax_raw.set_ylabel("Single raw")
        ax_db.set_ylabel("Single power norm (dB)")
        ax_db.set_xlabel(self.x_axis_label())
        lpf_text = ", display LPF on" if self.display_lpf_enable.get() else ""
        ax_raw.set_title(f"Single pulse raw + normalized dB, points={pts}, {self.sample_period_label()}{lpf_text}")

        self.ax = ax_db
        self.canvas.draw()

        self.log(self.curve_stats_text("Single raw", raw))

    def plot_single_pulse_and_corr_debug(self, curve, pts):
        raw = np.asarray(curve[:pts], dtype=np.float64)
        x = self.x_axis_values(np.arange(pts))
        raw_plot = self.display_lpf(raw)

        sps = self.pulse_samples_per_symbol()
        ref = np.ones(sps, dtype=np.float64)
        corr = np.convolve(raw, ref[::-1], mode='same') / float(sps)
        corr_plot = self.display_lpf(corr)
        corr_db = self.to_normalized_db(corr_plot)

        self.fig.clear()
        ax_raw, ax_corr = self.fig.subplots(2, 1, sharex=True)
        self.fig.subplots_adjust(left=0.08, right=0.98, top=0.92, bottom=0.08, hspace=0.12)
        ax_raw.grid(True)
        ax_corr.grid(True)

        ax_raw.plot(x, raw_plot, color='tab:blue', linewidth=1.0)
        ax_corr.plot(x, corr_db, color='tab:red', linewidth=1.0)

        # [新增] 限定第一个图的纵坐标范围
        ax_raw.set_ylim(-10000, 10000)

        ax_raw.set_ylabel("Single raw")
        ax_corr.set_ylabel("Pulse corr norm (dB)")
        ax_corr.set_xlabel(self.x_axis_label())
        lpf_text = ", display LPF on" if self.display_lpf_enable.get() else ""
        ax_raw.set_title(
            f"Single pulse raw + rectangular matched correlation, points={pts}, "
            f"sps={sps}, {self.sample_period_label()}{lpf_text}"
        )

        self.ax = ax_corr
        self.canvas.draw()

        self.log(self.curve_stats_text("Single raw", raw))
        self.log(self.curve_stats_text("Single pulse corr raw float", corr))

    def plot_single_pulse_uploaded_corr_debug(self, curve, pts):
        raw = np.asarray(curve[:pts], dtype=np.float64)
        corr = np.asarray(curve[pts:pts * 2], dtype=np.float64)
        x = self.x_axis_values(np.arange(pts))
        raw_plot = self.display_lpf(raw)
        corr_plot = self.display_lpf(corr)
        corr_db = self.to_normalized_db(corr_plot)

        self.fig.clear()
        ax_raw, ax_corr = self.fig.subplots(2, 1, sharex=True)
        self.fig.subplots_adjust(left=0.08, right=0.98, top=0.92, bottom=0.08, hspace=0.12)
        ax_raw.grid(True)
        ax_corr.grid(True)

        ax_raw.plot(x, raw_plot, color='tab:blue', linewidth=1.0)
        ax_corr.plot(x, corr_db, color='tab:red', linewidth=1.0)

        # [新增] 限定第一个图的纵坐标范围 
        ax_raw.set_ylim(-10000, 10000)

        ax_raw.set_ylabel("Single raw")
        ax_corr.set_ylabel("Pulse corr norm (dB)")
        ax_corr.set_xlabel(self.x_axis_label())
        lpf_text = ", display LPF on" if self.display_lpf_enable.get() else ""
        ax_raw.set_title(
            f"Single pulse raw + Vitis rectangular correlation, points={pts}, "
            f"{self.sample_period_label()}{lpf_text}"
        )

        self.ax = ax_corr
        self.canvas.draw()

        self.log(self.curve_stats_text("Single raw", raw))
        self.log(self.curve_stats_text("Single pulse Vitis corr raw float", corr))

    def periodic_plot_update(self):
        if self.plot_queue is not None:
            data = self.plot_queue
            self.plot_queue = None

            for item in self.tree.get_children(): self.tree.delete(item)
            for idx, evt in enumerate(data['events']):
                self.tree.insert(
                    "",
                    tk.END,
                    values=(
                        idx + 1,
                        self.format_x_value(evt['xlabel']),
                        evt['type'],
                        self.format_event_metric(evt['ins_loss']),
                        self.format_event_metric(evt['ref_loss']),
                        self.format_event_metric(evt['atten'], 4),
                    ),
                )

           

            curve = data['curve']
            try:
                expected_pts = int(self.capture_points.get())
            except ValueError:
                expected_pts = 0

            if data.get('data_kind') in ("single", "golay_experiment", "golay_result"):
                self.latest_measurement = data
                self.btn_save_measurement.config(state=tk.NORMAL)

            is_single_raw = data.get('is_single_raw_int16', False) or data.get('is_int32', False)

            if data.get('is_golay_experiment_float', False) and expected_pts > 0 and len(curve) == expected_pts * 5:
                self.plot_golay_experiment_debug(curve, expected_pts)
            elif data.get('is_single_pulse_corr_float', False) and expected_pts > 0 and len(curve) == expected_pts * 2:
                self.plot_single_pulse_uploaded_corr_debug(curve, expected_pts)
            elif data.get('is_single_raw_float', False) and expected_pts > 0 and len(curve) == expected_pts:
                self.plot_single_raw_norm_debug(curve, expected_pts, data['events'])
            elif expected_pts > 0 and len(curve) == expected_pts * 2:
                self.plot_ap_and_corr_debug(curve, expected_pts, data['events'])
            elif self.export_golay_raw_csv.get() and self.try_plot_and_save_raw_golay_int32(data.get('raw_bytes', b'')):
                pass
            else:
                self.fig.clear()
                self.ax = self.fig.add_subplot(111)
                self.fig.subplots_adjust(left=0.08, right=0.98, top=0.92, bottom=0.08)
                self.ax.clear()
                self.ax.grid(True)

                selected_ch = self.cb_ch_sel.get()
                self.fig.suptitle(f"Single Channel OTDR (CH {selected_ch}) - HW/SW Acc", fontsize=14, color='green')
                self.ax.set_ylabel("ADC raw" if is_single_raw else "Amplitude (dB)")

               # ch_data_abs = np.abs(data['curve'])
                ch_data_abs = curve
                is_official_db = data.get('is_official_db', False)
                if len(ch_data_abs) > 0:
                    mean_val = np.mean(ch_data_abs)
                    std_val = np.std(ch_data_abs)
                    dynamic_threshold = mean_val + 5 * std_val

                    peaks, _ = signal.find_peaks(ch_data_abs, height=dynamic_threshold, distance=100)

                    step = 1 if len(ch_data_abs) < 100000 else len(ch_data_abs) // 50000
                    sample_axis = np.arange(0, len(ch_data_abs), step)
                    x_axis = self.x_axis_values(sample_axis)
                    y_plot = ch_data_abs if (is_official_db or is_single_raw) else self.to_db(ch_data_abs)
                    self.ax.plot(x_axis, y_plot[::step], color='b', linewidth=1.0)
                    self.ax.set_xlabel(self.x_axis_label())
                    for p in peaks:
                        self.ax.plot(float(self.x_axis_values(p)), y_plot[p], 'r^', markersize=8)

                    y_max = max(y_plot)
                    for evt in data['events']:
                        evt_x = float(self.x_axis_values(evt['xlabel']))
                        self.ax.axvline(x=evt_x, color='r', linestyle='--', alpha=0.7)
                        self.ax.text(evt_x, y_max, f" {evt['type']} {self.format_x_value(evt['xlabel'])}", color='r', verticalalignment='top')

                self.canvas.draw()

            if self.is_measuring:
                if self.cb_mode.current() == 1:
                    interval = int(self.refresh_interval.get()) if self.refresh_interval.get().isdigit() else 500
                    self.root.after(interval, self.send_ack)
                else:
                    self.send_ack()

        self.root.after(200, self.periodic_plot_update)


if __name__ == "__main__":
    root = tk.Tk()
    app = OTDR_UI(root)
    root.mainloop()
