#!/usr/bin/env python3
"""FakePod NAS WebDAV folder picker + V2 catalogue builder (Python stdlib).

WebDAV only discovers folders. NAS audio metadata still requires SMB filesystem
access from Windows; hard links are only created on NAS via SSH + ln.
"""
from __future__ import annotations

import datetime as dt
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk
from urllib.parse import quote, unquote, urlparse
from typing import Callable
from urllib.error import HTTPError, URLError
from urllib.request import (HTTPBasicAuthHandler, HTTPDigestAuthHandler,
                            HTTPPasswordMgrWithDefaultRealm, Request, build_opener)
from xml.etree import ElementTree

SCRIPT_DIR = Path(__file__).resolve().parent
BUILDER = SCRIPT_DIR / "build_nas_catalog_v2.py"
META_FILE = "fakepod_catalog.meta"
EXTRA_FILES = ("fakepod_scan_cache_v1.json", "fakepod_change_report_v1.json", "fakepod_incremental_update.sh")
# Write the meta *last* as the visible commit marker for ESP32 index sync.
ARTIFACTS = ("music_index_v2.bin", "music_manifest_v2.bin", "fakepod_make_static_track_links.sh") + EXTRA_FILES + (META_FILE,)
DAV = "{DAV:}"


def shell_quote(value: str) -> str:
    return "'" + value.replace("'", "'\"'\"'") + "'"


def valid_host(host: str) -> bool:
    return bool(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.\-]{0,252}", host)) and ".." not in host


def parse_meta(path: Path) -> dict[str, str]:
    text = path.read_text(encoding="utf-8")
    if not text.startswith("FAKEPOD_NAS_V1\n"):
        raise ValueError("V2 索引校验信息格式不正确")
    return dict(line.split("=", 1) for line in text.splitlines()[1:] if "=" in line)


def validated_target(root: Path) -> Path:
    if not root.is_dir():
        raise ValueError("Windows 无法读取音乐目录（需要开启 SMB 共享并取得访问权限）")
    if "_FakePod" in root.parts:
        raise ValueError("请选择 Music 原始音乐根目录，不要选择 _FakePod")
    return root / "_FakePod" / "music-index"


def verify_source_snapshot(root: Path, staged_cache: Path,
                           progress: Callable[[int, int], None] | None = None) -> None:
    """Fail closed if NAS songs changed after scanning, before we publish.

    A background worker may report counts through `progress`; it must never call
    Tk widgets directly. Every file still receives the original size/mtime check.
    """
    snapshot = json.loads(staged_cache.read_text(encoding="utf-8"))["entries"]
    total = len(snapshot)
    observed = set()
    if progress:
        progress(0, total)

    def walk_error(error: OSError) -> None:
        raise error

    for dirname, folders, names in os.walk(root, onerror=walk_error):
        if Path(dirname) == root:
            folders[:] = [n for n in folders if n != "_FakePod"]
        for name in names:
            item = Path(dirname) / name
            if item.suffix.lower() not in (".mp3", ".flac", ".wav", ".opus") or not item.is_file():
                continue
            relative = "/" + item.relative_to(root).as_posix()
            st = item.stat()
            previous = snapshot.get(relative)
            if previous is None or (previous["size"], previous["mtime_ns"]) != (st.st_size, st.st_mtime_ns):
                raise RuntimeError(f"扫描后音乐文件发生变化，请重新扫描：{relative}")
            observed.add(relative)
            if progress and (len(observed) % 100 == 0 or len(observed) == total):
                progress(len(observed), total)
    if len(observed) != total:
        raise RuntimeError("扫描后音乐文件数量发生变化，请重新扫描")


def publish_artifacts(stage: Path, target: Path) -> Path:
    for name in ARTIFACTS:
        if not (stage / name).is_file() or (stage / name).stat().st_size == 0:
            raise ValueError(f"生成文件缺失：{name}")
    parent = target.parent
    parent.mkdir(parents=True, exist_ok=True)
    target.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    backup = parent / f"backup-index-{stamp}"
    suffix = 1
    while backup.exists():
        backup = parent / f"backup-index-{stamp}-{suffix}"
        suffix += 1
    backup.mkdir()
    incoming = []
    try:
        for name in ARTIFACTS:
            old = target / name
            if old.exists():
                shutil.copy2(old, backup / name)
        for name in ARTIFACTS:
            dest = target / (".incoming_" + name)
            shutil.copy2(stage / name, dest)
            incoming.append(dest)
        # Meta is last, preserving the existing catalogue commit-marker protocol.
        for name in ARTIFACTS:
            os.replace(target / (".incoming_" + name), target / name)
    except BaseException:
        for dest in incoming:
            dest.unlink(missing_ok=True)
        for name in ARTIFACTS:
            old = backup / name
            dest = target / name
            if old.exists():
                shutil.copy2(old, dest)
            elif dest.exists():
                dest.unlink()
        raise
    return backup


def canonical_dav_path(path: str) -> str:
    """Encode each decoded segment once, rejecting ambiguous slash/traversal."""
    if not path.startswith("/"):
        raise ValueError("WebDAV 路径必须以 / 开头")
    parts = []
    for raw in path.split("/"):
        if not raw:
            continue
        decoded = unquote(raw)
        if decoded in (".", "..") or "/" in decoded or "\\" in decoded or "\x00" in decoded:
            raise ValueError("WebDAV 路径包含不允许的字符")
        parts.append(quote(decoded, safe="-_.~"))
    return "/" + "/".join(parts) + "/" if parts else "/"


def make_locations(host: str, selected: str, volume: str) -> tuple[str, str, str]:
    """Map standard Synology WebDAV /SHARE/Music to SMB and NAS Linux paths."""
    encoded = canonical_dav_path(selected)
    parts = [unquote(seg) for seg in encoded.strip("/").split("/") if seg]
    if len(parts) < 2 or parts[-1] == "_FakePod" or any(seg == "_FakePod" for seg in parts):
        raise ValueError("请选择共享文件夹下的 Music 根目录，不能选 NAS 根目录或 _FakePod")
    if not re.fullmatch(r"/volume[0-9]+", volume):
        raise ValueError("NAS 卷位置应类似 /volume1；其他设备请在高级选项修改")
    share, relative = parts[0], parts[1:]
    if any("\x00" in seg or "/" in seg or "\\" in seg for seg in parts):
        raise ValueError("音乐路径不安全")
    smb = "\\\\" + host + "\\" + "\\".join(parts)
    linux = volume + "/" + "/".join(parts)
    return encoded, smb, linux


def list_webdav_folders(host: str, port: int, username: str, password: str, path: str) -> list[tuple[str, str]]:
    origin = f"http://{host}:{port}"
    path = canonical_dav_path(path)
    manager = HTTPPasswordMgrWithDefaultRealm()
    manager.add_password(None, origin, username, password)
    opener = build_opener(HTTPBasicAuthHandler(manager), HTTPDigestAuthHandler(manager))
    req = Request(origin + path, data=b'<?xml version="1.0"?><d:propfind xmlns:d="DAV:"><d:prop><d:resourcetype/></d:prop></d:propfind>',
                  method="PROPFIND", headers={"Depth": "1", "Content-Type": "application/xml; charset=utf-8"})
    with opener.open(req, timeout=15) as response:
        if response.status != 207:
            raise RuntimeError(f"NAS 未返回 WebDAV 目录清单：HTTP {response.status}")
        payload = response.read(2_000_001)
    if len(payload) > 2_000_000:
        raise RuntimeError("目录响应超出 2 MB 限制，请选择子目录")
    root = ElementTree.fromstring(payload)
    found: dict[str, tuple[str, str]] = {}
    for entry in root.findall(f"{DAV}response"):
        href = entry.findtext(f"{DAV}href", "").strip()
        if not href:
            continue
        parsed = urlparse(href)
        if parsed.netloc and (parsed.hostname != host or parsed.port not in (None, port)):
            continue
        try:
            folder_path = canonical_dav_path(parsed.path)
        except ValueError:
            continue
        # Use successful propstat's resourcetype only.
        is_folder = any((ps.findtext(f"{DAV}status", "").find(" 200 ") >= 0) and
                        ps.find(f"{DAV}prop/{DAV}resourcetype/{DAV}collection") is not None
                        for ps in entry.findall(f"{DAV}propstat"))
        if not is_folder or folder_path == path or not folder_path.startswith(path):
            continue
        tail = folder_path[len(path):].strip("/")
        if not tail or "/" in tail:
            continue
        title = unquote(tail)
        found[folder_path] = (title, folder_path)
    return sorted(found.values(), key=lambda item: item[0].casefold())


class App:
    def __init__(self, window: tk.Tk):
        self.window = window
        window.title("FakePod · NAS 曲库管理")
        window.geometry("990x835")
        window.minsize(820, 705)
        self.host = tk.StringVar(value="192.168.1.105")
        self.webdav_port = tk.StringVar(value="5005")
        self.webdav_user = tk.StringVar(value="admin")
        self.webdav_password = tk.StringVar()
        self.ssh_port = tk.StringVar(value="33")
        self.ssh_user = tk.StringVar(value="root")
        self.volume = tk.StringVar(value="/volume1")
        self.browse_path = "/"
        self.selected_path = None
        self.selected_smb = None
        self.selected_linux = None
        self.generated_smb = None
        self.staging = None
        self.report = None
        self.published = False
        self.busy = False
        self.publish_writing = False
        self.events = queue.Queue()
        self.scan_process = None
        self.scan_cancel = threading.Event()
        self.display_lock = threading.Lock()
        self.pending_display = {}
        self.scan_detail = None
        self.last_display = 0
        self.build()
        self.window.protocol("WM_DELETE_WINDOW", self.on_close)
        self.window.after(100, self.flush_events)

    def build(self):
        main = ttk.Frame(self.window, padding=12)
        main.pack(fill="both", expand=True)
        ttk.Label(main, text="FakePod NAS V2 · WebDAV 选目录 / 索引 / 短 ID", font=("Microsoft YaHei UI", 15, "bold")).pack(anchor="w")
        ttk.Label(main, text="只选一次 Music 根目录；自动确定索引和硬链接目录。WebDAV 浏览，SMB 扫描/发布，SSH 建硬链接。", foreground="#555").pack(anchor="w", pady=(4, 8))
        server = ttk.LabelFrame(main, text="1. NAS WebDAV 连接（只需选择一次音乐根目录）", padding=10)
        server.pack(fill="x", pady=4)
        conn = ttk.Frame(server)
        conn.pack(fill="x")
        for title, var, width, index in [("NAS 地址", self.host, 21, 0), ("WebDAV 端口", self.webdav_port, 7, 2), ("用户名", self.webdav_user, 15, 4)]:
            ttk.Label(conn, text=title).grid(row=0, column=index, padx=(0, 5))
            ttk.Entry(conn, textvariable=var, width=width).grid(row=0, column=index+1, padx=(0, 14), sticky="we")
        ttk.Label(conn, text="密码").grid(row=0, column=6, padx=(0, 5))
        ttk.Entry(conn, textvariable=self.webdav_password, show="●", width=16).grid(row=0, column=7, sticky="we")
        folders = ttk.Frame(server)
        folders.pack(fill="x", pady=(8, 4))
        self.connect_button = ttk.Button(folders, text="连接并浏览文件夹", command=self.connect)
        self.connect_button.pack(side="left")
        self.select_button = ttk.Button(folders, text="使用当前目录作为 Music", command=self.select_current, state="disabled")
        self.select_button.pack(side="left", padx=8)
        self.back_button = ttk.Button(folders, text="返回上一级", command=self.go_parent, state="disabled")
        self.back_button.pack(side="left")
        self.path_label = ttk.Label(server, text="浏览位置：未连接")
        self.path_label.pack(anchor="w", pady=3)
        self.folder_tree = ttk.Treeview(server, columns=("path",), show="tree", height=5, selectmode="browse")
        self.folder_tree.pack(fill="x", pady=(2, 4))
        self.folder_tree.bind("<Double-1>", lambda _: self.open_selected_folder())
        self.folder_tree.bind("<Return>", lambda _: self.open_selected_folder())
        ttk.Label(server, text="双击文件夹进入，停在 Music 根目录后点击『使用当前目录』。目录列表仅读取文件夹，不复制文件。", foreground="#555").pack(anchor="w")
        self.auto_label = ttk.Label(server, text="尚未选择音乐目录", foreground="#24587a", wraplength=900)
        self.auto_label.pack(anchor="w", pady=(7, 0))
        actions = ttk.LabelFrame(main, text="2. 增量扫描 / 预览变更", padding=10)
        actions.pack(fill="x", pady=5)
        line = ttk.Frame(actions); line.pack(fill="x")
        self.generate_button = ttk.Button(line, text="扫描并预览增量更新", command=self.generate, state="disabled")
        self.generate_button.pack(side="left")
        self.cancel_scan_button = ttk.Button(line, text="取消扫描", command=self.cancel_scan, state="disabled")
        self.cancel_scan_button.pack(side="left", padx=6)
        self.force_full = tk.BooleanVar(value=False)
        ttk.Checkbutton(line, text="重建全部元数据缓存", variable=self.force_full).pack(side="left", padx=10)
        self.change_label = ttk.Label(actions, text="尚未生成增量报告", foreground="#265f76", wraplength=920)
        self.change_label.pack(fill="x", pady=(8, 0))
        self.progress = ttk.Progressbar(actions, mode="determinate", maximum=100)
        self.progress.pack(fill="x", pady=(10, 4))
        self.progress_label = ttk.Label(actions, text="等待扫描", wraplength=900)
        self.progress_label.pack(anchor="w")
        remote = ttk.LabelFrame(main, text="3. 安全应用：准备链接 → 发布索引 → 隔离删除", padding=10)
        remote.pack(fill="x", pady=5)
        row = ttk.Frame(remote); row.pack(fill="x")
        for title, var, width in (("SSH 端口", self.ssh_port, 7), ("SSH 用户", self.ssh_user, 14), ("NAS 数据卷（高级）", self.volume, 11)):
            ttk.Label(row, text=title).pack(side="left", padx=(0, 5))
            ttk.Entry(row, textvariable=var, width=width).pack(side="left", padx=(0, 18))
        phases = ttk.Frame(remote); phases.pack(fill="x", pady=(8, 3))
        self.alias_button = ttk.Button(phases, text="① SSH 准备音频/LRC短 ID", command=lambda: self.launch_ssh("prepare"), state="disabled")
        self.alias_button.pack(side="left")
        self.publish_button = ttk.Button(phases, text="② 验证后发布 V2 索引", command=self.publish, state="disabled")
        self.publish_button.pack(side="left", padx=9)
        self.cleanup_button = ttk.Button(phases, text="③ SSH 隔离删除的旧 ID（可选）", command=lambda: self.launch_ssh("cleanup"), state="disabled")
        self.cleanup_button.pack(side="left")
        ttk.Label(remote, text="SSH 执行结果必须在 NAS 终端确认；准备完成的标记会由 GUI 在 SMB 上再次核验。旧兼容别名不自动删除。", foreground="#555").pack(anchor="w")
        logframe = ttk.LabelFrame(main, text="中文操作日志（实时更新）", padding=8)
        logframe.pack(fill="both", expand=True, pady=(6, 0))
        self.logbox = tk.Text(logframe, wrap="word", state="disabled", font=("Microsoft YaHei UI", 10), height=9)
        self.logbox.pack(fill="both", expand=True)
        self.log("就绪。首次建立缓存需要全量扫描；以后只重新解析新增/修改的歌曲。HTTP 5005 请仅在可信局域网使用。")

    def on_close(self):
        if self.publish_writing:
            messagebox.showwarning("NAS 索引正在发布", "正在写入索引文件，请等待完成后再关闭工具。")
            return
        self.scan_cancel.set()
        process = self.scan_process
        if process is not None and process.poll() is None: process.terminate()
        self.window.destroy()

    def cancel_scan(self):
        self.scan_cancel.set()
        self.cancel_scan_button.config(state="disabled")
        self.progress_label.config(text="正在取消扫描，已完成歌曲可在下次复用…")
        process = self.scan_process
        if process is not None and process.poll() is None: process.terminate()

    def on_scan_cancelled(self):
        self.progress.stop()
        self.cancel_scan_button.config(state="disabled")
        self.set_busy(False)
        self.progress_label.config(text="扫描已取消，检查点已保留；未发布索引")

    def log(self, msg):
        self.logbox.configure(state="normal")
        self.logbox.insert("end", str(msg).strip() + "\n")
        self.logbox.see("end")
        self.logbox.configure(state="disabled")

    def emit(self, event, *args):
        if event in ("progress", "scan_status"):
            with self.display_lock: self.pending_display[event] = args
        else: self.events.put((event, args))

    def flush_scan_display(self, force=False):
        now = time.monotonic()
        if not force and now - self.last_display < 1: return
        self.last_display = now
        with self.display_lock:
            pending, self.pending_display = self.pending_display, {}
        if "progress" in pending: self.update_progress(*pending["progress"])
        if "scan_status" in pending: self.scan_detail = (pending["scan_status"][0], now)
        if self.scan_detail is not None and not self.scan_cancel.is_set():
            detail, since = self.scan_detail
            done, total = detail['done'], detail['total']
            self.progress_label.config(text=f"{done}/{total} 首 · {done * 100 // max(1,total)}% · {detail['stage']} · 已等待 {int(now-since)} 秒\n{detail['path']}")

    def flush_events(self):
        self.flush_scan_display()
        try:
            for _ in range(120):
                event, args = self.events.get_nowait()
                if event in ("generated", "error", "cancelled", "publish_verified", "published"):
                    self.flush_scan_display(force=True)
                    self.scan_detail = None
                if event == "log": self.log(args[0])
                elif event == "folders": self.show_folders(*args)
                elif event == "progress": self.update_progress(*args)
                elif event == "generated": self.on_generated(*args)
                elif event == "publish_verified": self.on_publish_verified(*args)
                elif event == "published": self.on_published(*args)
                elif event == "error": self.on_error(*args)
                elif event == "cancelled": self.on_scan_cancelled()
        except queue.Empty:
            pass
        if self.window.winfo_exists():
            self.window.after(100, self.flush_events)

    def connection(self):
        host = self.host.get().strip()
        username = self.webdav_user.get().strip()
        pwd = self.webdav_password.get()
        try: port = int(self.webdav_port.get().strip())
        except ValueError: raise ValueError("WebDAV 端口不是数字")
        if not valid_host(host) or port not in range(1, 65536):
            raise ValueError("NAS IP/域名或 WebDAV 端口不正确")
        if not username or not pwd: raise ValueError("请填写 NAS WebDAV 用户名和密码")
        return host, port, username, pwd

    def set_busy(self, value):
        self.busy = value
        self.connect_button.config(state="disabled" if value else "normal")
        self.select_button.config(state="disabled" if value or not self.browse_path else "normal")
        self.back_button.config(state="disabled" if value or self.browse_path == "/" else "normal")
        self.generate_button.config(state="disabled" if value or not self.selected_smb else "normal")
        self.publish_button.config(state="disabled" if value or not self.staging or self.published else "normal")
        self.alias_button.config(state="disabled" if value or not self.staging or self.published or not self.report or
                                 not (self.report["prepare_paths"] or self.report["prepare_lyrics"] or self.report["remove_aliases"]) else "normal")
        self.cleanup_button.config(state="disabled" if value or not self.published or not self.report or
                                   not self.report["remove_aliases"] else "normal")

    def connect(self):
        # Switching hosts invalidates every previously generated or selected path.
        self.browse_path = "/"
        self.selected_path = self.selected_smb = self.selected_linux = None
        self.staging = None
        self.report = None
        self.published = False
        self.change_label.config(text="尚未生成增量报告")
        self.auto_label.config(text="尚未选择音乐目录")
        self.set_busy(False)
        self.load_folders("/")

    def load_folders(self, path):
        if self.busy: return
        try: conn = self.connection()
        except ValueError as e: messagebox.showerror("连接参数错误", str(e)); return
        self.browse_path = canonical_dav_path(path)
        self.set_busy(True)
        self.path_label.config(text="正在读取 WebDAV 目录：" + unquote(self.browse_path))
        self.log("连接 NAS 并读取文件夹：" + unquote(self.browse_path))
        def work():
            try:
                folders = list_webdav_folders(*conn, self.browse_path)
                self.emit("folders", self.browse_path, folders)
            except HTTPError as exc:
                hint = "（请检查用户名和密码）" if exc.code in (401, 403) else ""
                self.emit("error", f"WebDAV 目录请求失败，HTTP 状态码 {exc.code}{hint}")
            except URLError as exc: self.emit("error", "无法连接 NAS WebDAV，请检查 IP、端口和网络：" + str(exc.reason))
            except Exception as exc: self.emit("error", "读取 WebDAV 目录失败：" + str(exc))
        threading.Thread(target=work, daemon=True).start()

    def show_folders(self, path, folders):
        self.set_busy(False)
        for item in self.folder_tree.get_children(): self.folder_tree.delete(item)
        for title, child_path in folders:
            self.folder_tree.insert("", "end", iid=child_path, text="📁  " + title)
        self.path_label.config(text="当前 WebDAV 目录：" + unquote(path))
        self.log(f"读取成功：找到 {len(folders)} 个子文件夹")
        if not folders: self.log("当前目录没有子文件夹，可以返回上一级。")

    def open_selected_folder(self):
        selection = self.folder_tree.selection()
        if selection: self.load_folders(selection[0])

    def go_parent(self):
        parts = self.browse_path.strip("/").split("/")
        self.load_folders("/" + "/".join(parts[:-1]) + "/" if len(parts) > 1 else "/")

    def select_current(self):
        try:
            host, _, _, _ = self.connection()
            encoded, smb, linux = make_locations(host, self.browse_path, self.volume.get().strip())
            self.selected_path, self.selected_smb, self.selected_linux = encoded, smb, linux
            self.staging = None
            self.report = None
            self.published = False
            self.change_label.config(text="尚未生成增量报告")
            self.progress["value"] = 0
            self.progress_label.config(text="已选择 Music，尚未扫描")
            self.auto_label.config(text=f"选定：{unquote(encoded)}\n索引：{unquote(encoded)}_FakePod/music-index\n短 ID：{unquote(encoded)}_FakePod/track\nWindows SMB：{smb}\nNAS 内部：{linux}")
            self.log("已选 Music：" + unquote(encoded))
            self.log("扫描和发布自动使用 SMB：" + smb)
            self.log("SSH 建链自动使用 NAS 目录：" + linux)
            self.set_busy(False)
        except Exception as exc: messagebox.showerror("目录不正确", str(exc))

    def update_progress(self, kind, done, total):
        if kind == "VERIFY":
            self.progress.stop()
            self.progress.configure(mode="determinate", maximum=max(1, total), value=done)
            self.progress_label.config(text=f"发布前文件校验：{done}/{total} 首（{done * 100 // max(1, total)}%）")
            if done and (done == total or done % 500 == 0):
                self.log(f"发布前文件校验：{done}/{total} 首")
        elif kind == "CATALOG":
            self.progress.configure(mode="indeterminate")
            self.progress.start(12)
            self.progress_label.config(text="音频信息解析完成，正在构建 V2 索引与短 ID 脚本…")
            self.log("正在构建索引、清单和短 ID 建链脚本")
        elif kind == "DISCOVER":
            self.progress.configure(mode="indeterminate")
            self.progress_label.config(text=f"发现歌曲中：已检查 {done} 个目录，找到 {total} 首（继续扫描）")
        elif kind == "FOUND":
            self.progress.stop()
            self.progress.configure(mode="determinate", maximum=max(1, total), value=0)
            self.progress_label.config(text=f"发现歌曲完成，共 {total} 首；开始检查文件并匹配缓存")
            self.log(f"目录扫描完成：发现 {total} 首音频文件")
        elif kind == "META":
            self.progress.configure(mode="determinate", maximum=max(1, total), value=done)
            self.progress_label.config(text=f"文件检查/缓存复用：{done}/{total} 首（{done * 100 // max(1, total)}%）")
            if done == total or done % 250 == 0:
                self.log(f"文件检查/缓存复用：{done}/{total} 首")

    def generate(self):
        if self.busy or not self.selected_smb: return
        try:
            # Don't scan or publish a different NAS after WebDAV selection.
            host = self.connection()[0]
            selected = make_locations(host, self.selected_path, self.volume.get().strip())
            if selected[1] != self.selected_smb or selected[2] != self.selected_linux: raise ValueError("NAS 地址或数据卷已改变，请重新选择 Music")
            root = Path(self.selected_smb)
            validated_target(root)
            if not BUILDER.is_file(): raise ValueError("缺少 V2 索引生成器")
        except Exception as exc: messagebox.showerror("无法扫描", str(exc)); return
        self.staging = None
        self.report = None
        self.published = False
        self.set_busy(True)
        self.progress.configure(mode="indeterminate")
        self.progress.start(12)
        self.progress_label.config(text="正在扫描歌曲目录，统计文件数量…")
        full_scan = self.force_full.get()  # Tk variables must only be read on the GUI thread.
        if full_scan and (validated_target(root) / "fakepod_scan_cache_v1.json").exists():
            if not messagebox.askyesno("确认重新建立完整缓存",
                    "强制完整扫描会舍弃本轮增量变更对比。\n这次不会自动识别应隔离的旧短 ID。\n确定继续？"):
                self.set_busy(False)
                return
        self.scan_cancel.clear()
        self.scan_detail = None
        with self.display_lock: self.pending_display.clear()
        self.cancel_scan_button.config(state="normal")
        self.log("开始扫描并生成 V2 索引；断点检查点保存在电脑本地，不覆盖 NAS。")
        def work():
            temp = Path(tempfile.mkdtemp(prefix="fakepod_v2_stage_"))
            try:
                cache_path = validated_target(root) / "fakepod_scan_cache_v1.json"
                command = [sys.executable, "-u", str(BUILDER), str(root), str(temp)]
                if full_scan:
                    command.append("--force-full")
                else:
                    command.extend(["--cache-from", str(cache_path)])
                child_env = dict(os.environ, PYTHONIOENCODING="utf-8")
                process = subprocess.Popen(command,
                                           env=child_env,
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                           text=True, encoding="utf-8", errors="replace", bufsize=1)
                self.scan_process = process
                if self.scan_cancel.is_set(): process.terminate()
                assert process.stdout is not None
                messages = []
                for line in process.stdout:
                    line = line.strip()
                    if not line: continue
                    if line.startswith("FAKEPOD_PROGRESS "):
                        parts = line.split()
                        if len(parts) == 4:
                            try: self.emit("progress", parts[1], int(parts[2]), int(parts[3]))
                            except ValueError: pass
                    elif line.startswith("FAKEPOD_STATUS "):
                        self.emit("scan_status", json.loads(line[len("FAKEPOD_STATUS "):]))
                    elif line.startswith("FAKEPOD_INCREMENTAL "):
                        self.emit("log", "增量扫描报告：" + line.removeprefix("FAKEPOD_INCREMENTAL "))
                    else:
                        messages.append(line)
                code = process.wait()
                if self.scan_cancel.is_set(): raise RuntimeError("扫描已取消")
                if code: raise RuntimeError("生成器退出失败：" + "；".join(messages[-12:]))
                meta = parse_meta(temp / "fakepod_catalog.meta")
                if int(meta.get("tracks", "0")) < 1:
                    raise RuntimeError("没有找到音乐文件，已阻止发布空索引")
                report = json.loads((temp / "fakepod_change_report_v1.json").read_text(encoding="utf-8"))
                self.emit("generated", root, temp, meta, report)
            except Exception as exc:
                shutil.rmtree(temp, ignore_errors=True)
                if self.scan_cancel.is_set(): self.emit("cancelled")
                else: self.emit("error", "生成失败：" + str(exc))
            finally:
                self.scan_process = None
        threading.Thread(target=work, daemon=True).start()

    def on_error(self, message):
        self.cancel_scan_button.config(state="disabled")
        self.publish_writing = False
        self.progress.stop()
        self.set_busy(False)
        self.log("错误：" + message)
        messagebox.showerror("操作失败", message)

    def on_generated(self, root, temp, meta, report):
        self.cancel_scan_button.config(state="disabled")
        self.progress.stop()
        self.staging, self.generated_smb, self.generated_meta, self.report = temp, str(root), meta, report
        self.set_busy(False)
        baseline = report["mode"] == "baseline"
        summary = (f"首次建立缓存：共 {report['initial']} 首，下次启用增量"
                   if baseline else
                   f"总计 {report['total']} 首｜新增 {len(report['added'])}｜修改 {len(report['modified'])}｜删除 {len(report['removed'])}｜复用 {report['unchanged']}")
        summary += f"｜断点复用 {report.get('resumed', 0)}"
        self.change_label.config(text=summary)
        self.progress_label.config(text="扫描完毕；按顺序先准备短 ID，再发布索引")
        self.log(summary)
        for title, key in (("新增", "added"), ("修改", "modified"), ("删除", "removed")):
            if report[key]:
                self.log(title + "样例：" + "、".join(report[key][:8]) + ("……" if len(report[key]) > 8 else ""))
        self.log(f"V2 索引生成完成：歌曲 {meta.get('tracks','?')} 首，歌手 {meta.get('artists','?')}，专辑 {meta.get('albums','?')}。")
        self.log("输出保存在 Windows 临时目录；尚未修改 NAS：" + str(temp))
        self.set_busy(False)
        required = bool(report["prepare_paths"] or report["prepare_lyrics"] or report["remove_aliases"])
        self.log(f"LRC短ID待准备：{len(report['prepare_lyrics'])}；只创建硬链接，不复制歌词")
        next_action = "先点①准备音频/LRC短ID，再点②发布索引" if required else "无短 ID 变化，可以直接点②发布索引"
        messagebox.showinfo("增量扫描完成", summary + "\n" + next_action)


    def publish(self):
        if self.busy or not self.staging:
            return
        try:
            host = self.connection()[0]
            current = make_locations(host, self.selected_path, self.volume.get().strip())
            if current[1] != self.generated_smb or current[2] != self.selected_linux:
                raise ValueError("NAS 地址或音乐目录已改变，请重新选择并生成")
            target = validated_target(Path(self.selected_smb))
            report = self.report
            if not report:
                raise ValueError("增量报告丢失，请重新扫描")
            stage = self.staging
            root = Path(self.selected_smb)
        except Exception as exc:
            self.on_error("发布前检查失败：" + str(exc))
            return

        # Full SMB re-scan and marker reads may take seconds. They must not
        # run on Tk's event thread; the buttons stay disabled while it runs.
        self.set_busy(True)
        self.progress.stop()
        self.progress.configure(mode="determinate", maximum=1, value=0)
        self.progress_label.config(text="发布前检查：正在核验 NAS 文件和短 ID 准备标记…")
        self.log("发布前检查已开始：后台核验音乐文件，完成后弹出确认窗口。")

        def verify_work():
            try:
                if report["prepare_paths"] or report["prepare_lyrics"] or report["remove_aliases"]:
                    marker = target / report["prepare_marker"]
                    if not marker.is_file() or marker.read_text(encoding="utf-8").strip() != report["id"]:
                        raise ValueError("SSH 增量准备尚未成功。请先点①，在 SSH 终端完成准备，再重试发布")
                    script = target / "fakepod_incremental_update.sh"
                    if not script.is_file() or script.read_bytes() != (stage / script.name).read_bytes():
                        raise ValueError("NAS 端增量建链脚本不匹配，请重新执行①准备")
                verify_source_snapshot(root, stage / "fakepod_scan_cache_v1.json",
                                       lambda done, total: self.emit("progress", "VERIFY", done, total))
                self.emit("publish_verified", stage, target)
            except Exception as exc:
                self.emit("error", "发布前校验失败：" + str(exc))

        threading.Thread(target=verify_work, daemon=True).start()

    def on_publish_verified(self, stage: Path, target: Path):
        # This callback runs on Tk's main thread, after the background check.
        if not self.busy or stage != self.staging:
            self.on_error("发布状态发生变化，请重新扫描")
            return
        self.log("发布前安全校验通过；等待用户确认是否发布。")
        if not messagebox.askyesno(
                "确认发布 V2",
                "已完成 NAS 音乐文件校验。将备份旧索引后，发布新的 V2、Manifest、Meta 和硬链接脚本。\n"
                "不会操作现有 track 目录。\n\n请确保当前没有 NAS 音乐播放。是否继续？"):
            self.set_busy(False)
            self.progress_label.config(text="用户取消发布；索引未更新")
            self.log("已取消发布，NAS 索引未修改。")
            return
        self.publish_writing = True
        self.progress.configure(mode="indeterminate")
        self.progress.start(12)
        self.progress_label.config(text="正在备份旧索引并写入新版 V2，请勿重复操作…")
        self.log("用户已确认；正在后台备份并发布 V2 索引。")

        def publish_work():
            try:
                backup = publish_artifacts(stage, target)
                self.emit("published", target, backup)
            except Exception as exc:
                self.emit("error", "发布失败：" + str(exc))

        # Keep the process alive until the NAS commit/rollback finishes.
        threading.Thread(target=publish_work, daemon=False).start()

    def on_published(self, target: Path, backup: Path):
        self.publish_writing = False
        self.progress.stop()
        self.published = True
        self.set_busy(False)
        self.progress_label.config(text="索引和增量缓存发布成功")
        self.log("发布成功：" + str(target))
        self.log("旧索引备份：" + str(backup))
        self.log("缓存和变更报告已随 V2 索引一同发布")
        detail = ("如果有删除歌曲，最后可点③隔离旧 ID；不会删除音频。"
                  if self.report["remove_aliases"] else "本次没有需要隔离的旧 ID。")
        messagebox.showinfo("发布完成", "V2 索引和扫描缓存已发布，旧索引已备份。\n" + detail)

    def launch_ssh(self, mode="prepare"):
        if self.busy or not self.staging or not self.report:
            return
        if mode == "cleanup" and not self.published:
            messagebox.showerror("操作顺序错误", "必须先发布新索引，才能隔离旧短 ID")
            return
        if mode == "prepare" and self.published:
            return
        host = self.host.get().strip()
        port = self.ssh_port.get().strip()
        username = self.ssh_user.get().strip()
        try:
            if (not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.-]*", username)
                    or not port.isdecimal() or not 1 <= int(port) <= 65535):
                raise ValueError("SSH 用户名或端口不合法")
            current = make_locations(host, self.selected_path, self.volume.get().strip())
            if current[1] != self.generated_smb or current[2] != self.selected_linux:
                raise ValueError("NAS 地址或音乐目录已改变，请重新选择并生成")
            linux = current[2]
            target = validated_target(Path(self.selected_smb))
            script = target / "fakepod_incremental_update.sh"
            if mode == "prepare":
                if not (self.report["prepare_paths"] or self.report["prepare_lyrics"] or self.report["remove_aliases"]):
                    raise ValueError("本次没有短 ID 变化，直接发布 V2 即可")
                # Publish only the PLAN SCRIPT first; do not update index/meta/cache.
                stage_script = self.staging / script.name
                target.mkdir(parents=True, exist_ok=True)
                temp_script = target / (".incoming_" + script.name)
                shutil.copy2(stage_script, temp_script)
                os.replace(temp_script, script)
                self.log("已发布增量准备脚本；尚未修改 V2 索引或硬链接")
            elif not script.is_file() or script.read_bytes() != (self.staging / script.name).read_bytes():
                raise ValueError("NAS 端增量脚本不匹配，不能执行清理")
            if not valid_host(host):
                raise ValueError("SSH 目标主机不合法")
            remote_script = linux + "/_FakePod/music-index/fakepod_incremental_update.sh"
            remote_track = linux + "/_FakePod/track"
            cmd = "sh " + " ".join(shell_quote(x) for x in (remote_script, mode, linux, remote_track))
            title = "准备新短 ID" if mode == "prepare" else "隔离旧短 ID"
            help_text = ("这会在 NAS 内部创建/修复音频与同名LRC的短ID硬链接，不复制文件，完成后写入成功标记。"
                         if mode == "prepare" else
                         "只移动缓存明确识别为已删除歌曲的旧标准 ID 到 _FakePod/quarantine-*；不删除音频、不碰未知兼容别名。")
            if not messagebox.askyesno("确认 " + title,
                                       f"NAS：{host}，SSH {port}\nMusic：{linux}\n执行阶段：{mode}\n\n{help_text}\n\n请确保未在 NAS 上播放歌曲。继续？"):
                return
            if os.name != "nt":
                raise RuntimeError("SSH 弹出终端仅支持 Windows")
            subprocess.Popen(["cmd.exe", "/k", "ssh", "-p", port, f"{username}@{host}", cmd],
                             creationflags=subprocess.CREATE_NEW_CONSOLE)
            self.log(f"已启动 SSH {title}；需要在终端中输入密码并等待 OK。")
            if mode == "prepare":
                self.log("成功标记会由 NAS 写到 music-index；然后再点击②发布，GUI 会核验标记。")
            else:
                self.log("隔离后可在 NAS _FakePod/quarantine-* 找回旧链接。请检查 SSH 输出。")
        except Exception as exc:
            self.on_error("SSH 操作未启动：" + str(exc))


def main():
    window = tk.Tk()
    App(window)
    window.mainloop()


if __name__ == "__main__":
    main()
