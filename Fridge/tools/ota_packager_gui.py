"""Windows GUI for one-click RA6M5 MCUboot packaging and server upload."""

from __future__ import annotations

import json
import os
import threading
import traceback
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from ota_packager_core import (
    FirmwareServerClient,
    PackageResult,
    PackagerError,
    build_packages,
    find_node,
    find_objcopy,
    read_source_version,
)


APP_TITLE = "RA6M5 OTA 一键打包上传工具"


class PackagerApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title(APP_TITLE)
        self.root.geometry("980x760")
        self.root.minsize(820, 660)
        self.busy = False
        self.last_package: PackageResult | None = None

        fridge_dir = Path(__file__).resolve().parent.parent
        project_dir = fridge_dir / "project_firdge"
        debug_dir = project_dir / "Debug"
        default_key = Path.home() / "Documents" / "RA6M5_OTA_KEYS" / "fridge_ota_ecdsa_p256.pem"

        self.settings_path = Path(os.environ.get("APPDATA", Path.home())) / "FridgeOtaPackager" / "settings.json"
        saved = self._load_settings()
        self.vars = {
            "elf": tk.StringVar(value=saved.get("elf", str(debug_dir / "project_firdge.elf"))),
            "config": tk.StringVar(value=saved.get("config", str(project_dir / "src" / "ota_update_config.h"))),
            "version": tk.StringVar(value=saved.get("version", "")),
            "key": tk.StringVar(value=saved.get("key", str(default_key))),
            "template": tk.StringVar(value=saved.get("template", str(debug_dir / "project_firdge.bin.signed"))),
            "output": tk.StringVar(value=saved.get("output", str(debug_dir / "ota_packages"))),
            "server": tk.StringVar(value=saved.get("server", "http://101.245.87.7:10087")),
            "password": tk.StringVar(),
            "title": tk.StringVar(value=saved.get("title", "RA6M5 冰箱主控固件")),
            "changelog": tk.StringVar(value=saved.get("changelog", "由OTA打包工具生成并上传")),
            "node": tk.StringVar(value=saved.get("node", "")),
            "objcopy": tk.StringVar(value=saved.get("objcopy", "")),
        }

        self._build_ui()
        self._read_version(silent=True)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _load_settings(self) -> dict[str, str]:
        try:
            data = json.loads(self.settings_path.read_text(encoding="utf-8"))
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def _save_settings(self) -> None:
        data = {
            name: variable.get().strip()
            for name, variable in self.vars.items()
            if name != "password"
        }
        try:
            self.settings_path.parent.mkdir(parents=True, exist_ok=True)
            self.settings_path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
        except OSError:
            pass

    def _build_ui(self) -> None:
        outer = ttk.Frame(self.root, padding=14)
        outer.pack(fill="both", expand=True)
        ttk.Label(outer, text=APP_TITLE, font=("Microsoft YaHei UI", 17, "bold")).pack(anchor="w")
        ttk.Label(
            outer,
            text="生成网站 OTA pending.bin 和本地 USB sparse.hex；上传成功后仍需在网站手动点击“部署”。",
        ).pack(anchor="w", pady=(3, 12))

        form = ttk.Frame(outer)
        form.pack(fill="x")
        form.columnconfigure(1, weight=1)
        row = 0
        row = self._path_row(form, row, "e² studio ELF", "elf", [("ELF", "*.elf"), ("所有文件", "*.*")])
        row = self._path_row(form, row, "版本配置文件", "config", [("头文件", "*.h"), ("所有文件", "*.*")])

        ttk.Label(form, text="固件版本").grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        ttk.Entry(form, textvariable=self.vars["version"]).grid(row=row, column=1, sticky="ew", pady=4)
        ttk.Button(form, text="从源码读取", command=self._read_version).grid(row=row, column=2, padx=(8, 0), pady=4)
        row += 1

        row = self._path_row(form, row, "ECDSA-P256私钥", "key", [("PEM私钥", "*.pem"), ("所有文件", "*.*")])
        row = self._path_row(form, row, "MCUboot模板BIN", "template", [("BIN", "*.bin"), ("所有文件", "*.*")])
        row = self._directory_row(form, row, "输出目录", "output")

        ttk.Separator(form).grid(row=row, column=0, columnspan=3, sticky="ew", pady=10)
        row += 1
        ttk.Label(form, text="服务器地址").grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        ttk.Entry(form, textvariable=self.vars["server"]).grid(row=row, column=1, columnspan=2, sticky="ew", pady=4)
        row += 1
        ttk.Label(form, text="管理员密码").grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        ttk.Entry(form, textvariable=self.vars["password"], show="●").grid(row=row, column=1, columnspan=2, sticky="ew", pady=4)
        row += 1
        ttk.Label(form, text="版本标题").grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        ttk.Entry(form, textvariable=self.vars["title"]).grid(row=row, column=1, columnspan=2, sticky="ew", pady=4)
        row += 1
        ttk.Label(form, text="更新说明").grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        ttk.Entry(form, textvariable=self.vars["changelog"]).grid(row=row, column=1, columnspan=2, sticky="ew", pady=4)

        advanced = ttk.LabelFrame(outer, text="高级工具路径（通常留空自动检测）", padding=8)
        advanced.pack(fill="x", pady=(10, 8))
        advanced.columnconfigure(1, weight=1)
        self._tool_row(advanced, 0, "Node.js", "node")
        self._tool_row(advanced, 1, "arm-none-eabi-objcopy", "objcopy")

        actions = ttk.Frame(outer)
        actions.pack(fill="x", pady=(2, 8))
        self.check_button = ttk.Button(actions, text="检查环境", command=self._check_environment)
        self.package_button = ttk.Button(actions, text="一键打包", command=lambda: self._start(False))
        self.upload_button = ttk.Button(actions, text="一键打包并上传", command=lambda: self._start(True))
        self.open_button = ttk.Button(actions, text="打开输出目录", command=self._open_output)
        for button in (self.check_button, self.package_button, self.upload_button, self.open_button):
            button.pack(side="left", padx=(0, 8))

        self.status = tk.StringVar(value="就绪")
        ttk.Label(outer, textvariable=self.status).pack(anchor="w")
        self.log_text = tk.Text(outer, height=14, wrap="word", font=("Consolas", 9))
        self.log_text.pack(fill="both", expand=True, pady=(5, 0))
        self.log_text.configure(state="disabled")

    def _path_row(self, parent: ttk.Frame, row: int, label: str, name: str, filetypes: list[tuple[str, str]]) -> int:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        ttk.Entry(parent, textvariable=self.vars[name]).grid(row=row, column=1, sticky="ew", pady=4)
        ttk.Button(
            parent,
            text="浏览…",
            command=lambda: self._browse_file(name, filetypes),
        ).grid(row=row, column=2, padx=(8, 0), pady=4)
        return row + 1

    def _directory_row(self, parent: ttk.Frame, row: int, label: str, name: str) -> int:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        ttk.Entry(parent, textvariable=self.vars[name]).grid(row=row, column=1, sticky="ew", pady=4)
        ttk.Button(parent, text="浏览…", command=lambda: self._browse_directory(name)).grid(
            row=row, column=2, padx=(8, 0), pady=4
        )
        return row + 1

    def _tool_row(self, parent: ttk.LabelFrame, row: int, label: str, name: str) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=3)
        ttk.Entry(parent, textvariable=self.vars[name]).grid(row=row, column=1, sticky="ew", pady=3)
        ttk.Button(
            parent,
            text="浏览…",
            command=lambda: self._browse_file(name, [("可执行文件", "*.exe"), ("所有文件", "*.*")]),
        ).grid(row=row, column=2, padx=(8, 0), pady=3)

    def _browse_file(self, name: str, filetypes: list[tuple[str, str]]) -> None:
        current = Path(self.vars[name].get()).expanduser()
        selected = filedialog.askopenfilename(
            initialdir=str(current.parent if current.parent.exists() else Path.cwd()),
            filetypes=filetypes,
        )
        if selected:
            self.vars[name].set(selected)
            if name == "config":
                self._read_version(silent=True)

    def _browse_directory(self, name: str) -> None:
        selected = filedialog.askdirectory(initialdir=self.vars[name].get() or str(Path.cwd()))
        if selected:
            self.vars[name].set(selected)

    def _read_version(self, silent: bool = False) -> None:
        try:
            version = read_source_version(Path(self.vars["config"].get()).expanduser())
            self.vars["version"].set(version)
            if not silent:
                self._log(f"已从源码读取版本：{version}")
        except PackagerError as exc:
            if not silent:
                messagebox.showerror(APP_TITLE, str(exc))

    def _log(self, message: str) -> None:
        def append() -> None:
            self.log_text.configure(state="normal")
            self.log_text.insert("end", message.rstrip() + "\n")
            self.log_text.see("end")
            self.log_text.configure(state="disabled")
        self.root.after(0, append)

    def _set_busy(self, busy: bool, status: str) -> None:
        self.busy = busy
        state = "disabled" if busy else "normal"
        for button in (self.check_button, self.package_button, self.upload_button):
            button.configure(state=state)
        self.status.set(status)

    def _check_environment(self) -> None:
        try:
            node = find_node(self.vars["node"].get().strip())
            objcopy = find_objcopy(self.vars["objcopy"].get().strip())
            version = read_source_version(Path(self.vars["config"].get()).expanduser())
            self._log(f"Node.js: {node}")
            self._log(f"objcopy: {objcopy}")
            self._log(f"源码版本: {version}")
            messagebox.showinfo(APP_TITLE, "环境检查通过。")
        except PackagerError as exc:
            messagebox.showerror(APP_TITLE, str(exc))

    def _values(self) -> dict[str, str]:
        return {name: variable.get().strip() for name, variable in self.vars.items()}

    def _start(self, upload: bool) -> None:
        if self.busy:
            return
        values = self._values()
        if upload and not values["password"]:
            messagebox.showerror(APP_TITLE, "打包并上传时必须输入网站管理员密码。")
            return
        self._save_settings()
        self._set_busy(True, "正在打包并上传…" if upload else "正在打包…")
        self._log("=" * 64)
        worker = threading.Thread(target=self._worker, args=(values, upload), daemon=True)
        worker.start()

    def _worker(self, values: dict[str, str], upload: bool) -> None:
        try:
            config_path = Path(values["config"]).expanduser().resolve()
            project_dir = config_path.parent.parent
            result = build_packages(
                project_dir=project_dir,
                elf_path=Path(values["elf"]).expanduser().resolve(),
                config_path=config_path,
                private_key=Path(values["key"]).expanduser().resolve(),
                template_signed_bin=Path(values["template"]).expanduser().resolve(),
                output_dir=Path(values["output"]).expanduser().resolve(),
                version=values["version"],
                node_path=values["node"],
                objcopy_path=values["objcopy"],
                log=self._log,
            )
            self.last_package = result
            self._log(f"网站上传文件：{result.ota_pending_bin}")
            self._log(f"本地USB文件：{result.local_sparse_hex}")
            self._log(f"OTA SHA-256：{result.sha256}")

            if upload:
                self._log("正在登录固件服务器……")
                client = FirmwareServerClient(values["server"])
                client.login(values["password"])
                self._log("管理员登录成功，正在创建或复用固件版本……")
                release = client.ensure_release(
                    result.version,
                    title=values["title"],
                    changelog=values["changelog"],
                )
                self._log(f"固件版本ID：{release['id']}，正在上传 {result.ota_pending_bin.name}……")
                artifact = client.upload_ota(int(release["id"]), result.ota_pending_bin)
                self._log(
                    f"服务器验签通过：artifact={artifact.get('id')} "
                    f"version={artifact.get('image_version')} image_valid={artifact.get('image_valid')}"
                )
                self._finish_success(
                    "打包和上传成功。\n\n服务器已经确认 ECDSA-P256 签名有效。\n"
                    "固件尚未部署，请到网站固件中心选择设备并点击“部署到设备”。"
                )
            else:
                self._finish_success(
                    "打包成功。\n\n网站上传文件：\n"
                    f"{result.ota_pending_bin}\n\n本地USB烧录文件：\n{result.local_sparse_hex}"
                )
        except Exception as exc:
            if not isinstance(exc, PackagerError):
                self._log(traceback.format_exc())
            self._finish_error(str(exc))

    def _finish_success(self, message: str) -> None:
        def finish() -> None:
            self._set_busy(False, "完成")
            messagebox.showinfo(APP_TITLE, message)
        self.root.after(0, finish)

    def _finish_error(self, message: str) -> None:
        self._log(f"失败：{message}")
        def finish() -> None:
            self._set_busy(False, "失败")
            messagebox.showerror(APP_TITLE, message)
        self.root.after(0, finish)

    def _open_output(self) -> None:
        output = Path(self.vars["output"].get()).expanduser()
        output.mkdir(parents=True, exist_ok=True)
        os.startfile(output)  # type: ignore[attr-defined]

    def _on_close(self) -> None:
        if self.busy and not messagebox.askyesno(APP_TITLE, "任务仍在运行，确定关闭吗？"):
            return
        self._save_settings()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    try:
        ttk.Style(root).theme_use("vista")
    except tk.TclError:
        pass
    PackagerApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
