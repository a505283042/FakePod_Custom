"""Local checkpoint journal and one-handle, bounded-cache audio reads (stdlib only)."""
import errno
import hashlib
import io
import json
import os
from pathlib import Path
from collections import OrderedDict
import time


class ScanReadError(RuntimeError):
    """Must not be swallowed by tolerant tag parsers catching OSError/ValueError."""


def retry_io(action, report=lambda text: None):
    for attempt in range(3):
        try:
            return action()
        except OSError as error:
            transient = error.errno in {errno.EIO, errno.ESTALE, errno.ETIMEDOUT,
                                       errno.ECONNRESET, errno.ECONNABORTED, errno.ENOTCONN,
                                       errno.ENETUNREACH, errno.EHOSTUNREACH}
            transient |= getattr(error, 'winerror', None) in {53, 59, 64, 121, 1231, 1232}
            if not transient or attempt == 2:
                raise ScanReadError('读取失败，检查点已保留：' + str(error)) from error
            report('连接异常，重试 %d/2' % (attempt + 1))
            time.sleep(.5 * (attempt + 1))


class AudioSession:
    """Path-compatible lazy session: independent parser cursors share one real handle."""
    def __init__(self, path, stat, report=lambda text: None):
        self.path, self.info, self.report = path, stat, report
        self.handle = None
        self.blocks = OrderedDict()
    def __getattr__(self, name): return getattr(self.path, name)
    def __str__(self): return str(self.path)
    def __fspath__(self): return os.fspath(self.path)
    def __enter__(self): return self
    def __exit__(self, *args): self.close()
    def close(self):
        if self.handle is not None:
            handle, self.handle = self.handle, None
            handle.close()
    def stat(self): return self.info
    def open(self, mode='rb'):
        if mode != 'rb': raise ValueError('Audio session is read-only')
        return AudioCursor(self)
    def block(self, offset):
        if offset in self.blocks:
            self.blocks.move_to_end(offset)
            return self.blocks[offset]
        def read():
            try:
                if self.handle is None:
                    self.handle = self.path.open('rb')
                    current = os.fstat(self.handle.fileno())
                    if (current.st_size, current.st_mtime_ns) != (self.info.st_size, self.info.st_mtime_ns):
                        raise ScanReadError('文件已变化，请重新扫描：' + str(self.path))
                self.handle.seek(offset)
                wanted = min(65536, max(0, self.info.st_size - offset))
                data = self.handle.read(wanted)
                if len(data) != wanted:
                    raise ScanReadError('文件提前结束，请重新扫描：' + str(self.path))
                return data
            except OSError:
                try: self.close()
                except OSError: pass
                raise
        data = retry_io(read, self.report)
        self.blocks[offset] = data
        while len(self.blocks) > 32: self.blocks.popitem(last=False)
        return data


class AudioCursor(io.RawIOBase):
    def __init__(self, session):
        super().__init__()
        self.session, self.pos = session, 0
    def readable(self): return True
    def seekable(self): return True
    def tell(self): return self.pos
    def seek(self, offset, whence=0):
        self._checkClosed()
        if whence not in (0, 1, 2): raise ValueError('invalid whence')
        self.pos = offset + (self.pos if whence == 1 else self.session.info.st_size if whence == 2 else 0)
        if self.pos < 0: raise ValueError('negative seek')
        return self.pos
    def read(self, size=-1):
        self._checkClosed()
        remaining = max(0, self.session.info.st_size - self.pos)
        size = remaining if size < 0 else min(size, remaining)
        out = bytearray()
        while len(out) < size:
            start = self.pos // 65536 * 65536
            data = self.session.block(start)
            piece = data[self.pos - start:self.pos - start + size - len(out)]
            if not piece: raise ScanReadError('音频读取没有进展：' + str(self.session.path))
            out.extend(piece); self.pos += len(piece)
        return bytes(out)
    def readinto(self, buffer):
        data = self.read(len(buffer)); buffer[:len(data)] = data; return len(data)


def checkpoint_home():
    return Path(os.environ.get('LOCALAPPDATA') or (Path.home() / '.cache')) / 'FakePod' / 'nas-scan-checkpoints'


class ScanJournal:
    def __init__(self, directory, identity, reset=False):
        directory = Path(directory); directory.mkdir(parents=True, exist_ok=True)
        root = identity['root']
        stem = hashlib.sha256(root.encode()).hexdigest()
        self.path = directory / (stem + '.jsonl')
        self.lock = (directory / (stem + '.lock')).open('a+b')
        self.lock.seek(0); self.lock.write(b'0'); self.lock.flush(); self.lock.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(self.lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            self.lock.close()
            raise RuntimeError('此曲库已有扫描任务，请先结束该任务') from error
        try:
            self.load(identity, reset)
        except BaseException:
            self.close()
            raise

    def close(self):
        if not self.lock.closed:
            if os.name == 'nt':
                import msvcrt
                self.lock.seek(0)
                msvcrt.locking(self.lock.fileno(), msvcrt.LK_UNLCK, 1)
            self.lock.close()

    def load(self, identity, reset):
        self.entries = {}
        self.key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
        if not reset and self.path.exists():
            if self.path.stat().st_size > 128 * 1024 * 1024:
                raise ValueError('本地检查点超过上限，请备份后重建')
            with self.path.open(encoding='utf-8') as stream:
                try: header = json.loads(next(stream))
                except (ValueError, StopIteration): header = {}
                if header.get('key') == self.key:
                    for line in stream:
                        try:
                            item = json.loads(line)
                            if isinstance(item.get('path'), str) and isinstance(item.get('entry'), dict):
                                self.entries[item['path']] = item['entry']
                        except (ValueError, KeyError, TypeError, AttributeError): pass
        # Atomic compaction bounds repeated resumes and drops a truncated last record.
        temp = self.path.with_suffix('.tmp')
        with temp.open('w', encoding='utf-8') as stream:
            stream.write(json.dumps(dict(key=self.key)) + '\n')
            for path, entry in self.entries.items():
                stream.write(json.dumps(dict(path=path, entry=entry), ensure_ascii=False) + '\n')
            stream.flush(); os.fsync(stream.fileno())
        os.replace(temp, self.path)
    def save(self, path, entry):
        with self.path.open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(dict(path=path, entry=entry), ensure_ascii=False) + '\n')
            stream.flush(); os.fsync(stream.fileno())


class ScanProgress:
    def __init__(self): self.last = 0
    def emit(self, stage, done, total, path='', force=False):
        now = time.monotonic()
        if not force and now - self.last < 1: return
        self.last = now
        print('FAKEPOD_PROGRESS META %d %d' % (done, total), flush=True)
        print('FAKEPOD_STATUS ' + json.dumps(dict(stage=stage, done=done, total=total, path=path), ensure_ascii=False), flush=True)
