# lb_kernel.py — LlamaBoss persistent Python session kernel (v2).
# Written and managed by LlamaBoss; do not edit or run by hand.
import sys, os, io, json, ast, textwrap, time, traceback

KERNEL_VERSION = 2
CAP = 4 * 1024 * 1024  # per-stream capture cap folded into each response

# Serialized-size budget per response field, in bytes of the final
# ensure_ascii JSON.  Escaping can grow text up to 6x (\u00XX for control
# characters, 12 bytes for a non-BMP character), so the 4 MiB capture
# cap alone does not bound the frame.  The budgets total 20 MiB, well
# under the C++ reader's 32 MiB ceiling -- a frame over it is treated as
# desync and kills the session, which would erase its variables.
FIELD_BUDGET = {
    "stdout": 8 * 1024 * 1024,
    "stderr": 8 * 1024 * 1024,
    "traceback": 2 * 1024 * 1024,
    "exc_message": 1 * 1024 * 1024,
}
TRUNC_NOTE = "\n[... truncated to fit the response size limit]"


def _fatal(crash_out, msg):
    try:
        os.write(crash_out, ("lb_kernel fatal: %s\n" % msg).encode(
            "utf-8", "replace"))
    except OSError:
        pass
    os._exit(3)


def read_frame(rf):
    header = rf.readline(64)
    if not header:
        return None  # parent closed stdin -> orderly shutdown
    if not header.startswith(b"LBPY1 ") or not header.endswith(b"\n"):
        raise ValueError("bad frame header: %r" % header[:32])
    n = int(header[6:-1])
    if n < 0 or n > 64 * 1024 * 1024:
        raise ValueError("bad frame length: %d" % n)
    buf = b""
    while len(buf) < n:
        chunk = rf.read(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return json.loads(buf.decode("utf-8"))


def _fit_json(s, budget):
    # Longest prefix of s whose JSON encoding fits in budget bytes.
    # Encoding is linear in length, so scale the cut by the measured
    # ratio; this converges in a few passes even for all-escape text.
    if len(json.dumps(s, ensure_ascii=True)) <= budget:
        return s, False
    budget -= len(json.dumps(TRUNC_NOTE, ensure_ascii=True))
    n = len(s)
    while n > 0:
        enc = len(json.dumps(s[:n], ensure_ascii=True))
        if enc <= budget:
            break
        n = min(n - 1, int(n * budget / enc * 0.98))
    return s[:max(n, 0)] + TRUNC_NOTE, True


def _fit_response(obj):
    for key, budget in FIELD_BUDGET.items():
        val = obj.get(key)
        if not isinstance(val, str) or not val:
            continue
        val, cut = _fit_json(val, budget)
        if cut:
            obj[key] = val
            if key in ("stdout", "stderr"):
                obj[key + "_truncated"] = True
    return obj


def write_frame(wf, obj):
    payload = json.dumps(_fit_response(obj), ensure_ascii=True).encode("ascii")
    wf.write(b"LBPY1 %d\n" % len(payload))
    wf.write(payload)
    wf.flush()


def read_capture(path):
    try:
        with open(path, "rb") as f:
            data = f.read(CAP + 1)
    except OSError:
        return "", False
    trunc = len(data) > CAP
    if trunc:
        data = data[:CAP]
    return data.decode("utf-8", "replace"), trunc


def run_exec(rid, code, ns, out_path, err_path):
    # Reset both capture files.  fds 1/2 are O_APPEND, so truncating
    # to zero is safe regardless of any writer's current offset.
    sys.stdout.flush()
    sys.stderr.flush()
    try:
        os.ftruncate(1, 0)
        os.ftruncate(2, 0)
    except OSError:
        pass

    ok, etype, emsg, etb = True, "", "", ""
    t0 = time.perf_counter()
    try:
        # Models sometimes emit uniformly indented code blocks; a
        # common-prefix dedent makes those parse instead of dying on
        # IndentationError, and is a no-op for normal code.
        code = textwrap.dedent(code)
        tree = ast.parse(code, "<py>", "exec")
        if tree.body and isinstance(tree.body[-1], ast.Expr):
            # REPL semantics: run everything but a trailing bare
            # expression, then eval the expression; a non-None value
            # is repr()-printed and bound to `_`, Jupyter-style.
            last = ast.Expression(tree.body[-1].value)
            ast.copy_location(last, tree.body[-1])
            tree.body = tree.body[:-1]
            exec(compile(tree, "<py>", "exec"), ns)
            val = eval(compile(last, "<py>", "eval"), ns)
            if val is not None:
                ns["_"] = val
                print(repr(val))
        else:
            exec(compile(tree, "<py>", "exec"), ns)
    except SystemExit as e:
        # A bare exit()/sys.exit() in user code must not take the
        # kernel down — report it and keep the namespace alive.
        ok = False
        etype = "SystemExit"
        emsg = "" if e.code is None else str(e.code)
        etb = ("SystemExit was raised by the code; the session is "
               "still running and its variables are preserved.\n")
    except BaseException as e:
        ok = False
        etype = type(e).__name__
        emsg = str(e)
        tb = e.__traceback__
        if tb is not None:
            tb = tb.tb_next  # hide the kernel's own exec() frame
        try:
            etb = "".join(traceback.format_exception(type(e), e, tb))
        except Exception:
            etb = "%s: %s\n" % (etype, emsg)
    dur = int((time.perf_counter() - t0) * 1000)

    sys.stdout.flush()
    sys.stderr.flush()
    so, so_t = read_capture(out_path)
    se, se_t = read_capture(err_path)
    return {
        "id": rid, "ok": ok, "stdout": so, "stderr": se,
        "exc_type": etype, "exc_message": emsg, "traceback": etb,
        "duration_ms": dur,
        "stdout_truncated": so_t, "stderr_truncated": se_t,
    }

def _install_helpers(ns):
    # Convenience helpers preloaded into the persistent namespace so
    # models can work with workspace files and large objects without
    # boilerplate.  Deliberately shadowable: user code that rebinds
    # these names just wins; the kernel never calls them itself.
    import glob as _glob

    def load(path, encoding="utf-8", errors="replace", binary=False):
        """load(path) -> str. Read a file; relative paths resolve
        against the conversation workspace. binary=True returns bytes."""
        if binary:
            with open(path, "rb") as f:
                return f.read()
        with open(path, "r", encoding=encoding, errors=errors) as f:
            return f.read()

    def ws(pattern="*"):
        """ws() -> sorted list of workspace paths matching a glob.
        Use ws('**/*') for a recursive walk, ws('Vars/*') for spools."""
        return sorted(_glob.glob(pattern, recursive=True))

    def _clip(s, limit):
        s = str(s)
        return s if len(s) <= limit else s[:limit] + "..."

    def peek(obj, n=10):
        """peek(obj) -> None. Print a compact preview (size/shape plus
        a small sample) instead of flooding output with a full repr."""
        try:
            if isinstance(obj, (str, bytes)):
                nl = "\n" if isinstance(obj, str) else b"\n"
                print("%s: %d chars, %d lines"
                      % (type(obj).__name__, len(obj), obj.count(nl) + 1))
                print(_clip(obj[:600], 600))
                if len(obj) > 600:
                    print("... [tail] ...")
                    print(_clip(obj[-240:], 240))
            elif hasattr(obj, "shape") and hasattr(obj, "head"):
                # pandas-shaped objects, duck-typed so pandas is never
                # imported by the kernel itself.
                print("%s shape=%s" % (type(obj).__name__, obj.shape))
                print(obj.head(n))
            elif isinstance(obj, dict):
                print("dict: %d keys" % len(obj))
                for i, (k, v) in enumerate(obj.items()):
                    if i >= n:
                        print("... (%d more)" % (len(obj) - n))
                        break
                    print("  %s: %s" % (_clip(k, 60), _clip(repr(v), 120)))
            elif isinstance(obj, (list, tuple, set, frozenset)):
                seq = list(obj)
                print("%s: %d items" % (type(obj).__name__, len(seq)))
                for i, item in enumerate(seq[:n]):
                    print("  [%d] %s" % (i, _clip(repr(item), 120)))
                if len(seq) > n:
                    print("... (%d more)" % (len(seq) - n))
            else:
                print(_clip(repr(obj), 600))
        except Exception as e:
            print("peek failed: %r" % (e,))

    ns["load"] = load
    ns["ws"] = ws
    ns["peek"] = peek


def main():
    if len(sys.argv) < 2:
        os._exit(2)
    capdir = sys.argv[1]

    # Private handles to the ORIGINAL stdio pipes, taken before any
    # repointing.  These are the frame channel (in/out) and the crash
    # channel (the original stderr pipe, which the C++ drain thread
    # keeps reading for the life of the session).
    proto_in = os.dup(0)
    proto_out = os.dup(1)
    crash_out = os.dup(2)

    try:
        devnull = os.open(os.devnull, os.O_RDONLY)
        os.dup2(devnull, 0)
        os.close(devnull)

        out_path = os.path.join(capdir, "out.log")
        err_path = os.path.join(capdir, "err.log")
        flags = os.O_CREAT | os.O_WRONLY | os.O_APPEND | os.O_TRUNC
        ofd = os.open(out_path, flags)
        efd = os.open(err_path, flags)
        os.dup2(ofd, 1)
        os.dup2(efd, 2)
        os.close(ofd)
        os.close(efd)

        # Rebind the Python-level streams onto the (now file-backed)
        # fds 1/2 with UTF-8 text wrappers so print() from user code
        # is captured with sane encoding regardless of console
        # codepage history.
        sys.stdout = io.TextIOWrapper(io.FileIO(1, "w", closefd=False),
                                      encoding="utf-8", errors="replace",
                                      line_buffering=True)
        sys.stderr = io.TextIOWrapper(io.FileIO(2, "w", closefd=False),
                                      encoding="utf-8", errors="replace",
                                      line_buffering=True)

        rf = io.open(proto_in, "rb")
        wf = io.open(proto_out, "wb", buffering=0)
    except Exception as e:
        _fatal(crash_out, "init failed: %r" % (e,))
        return

    ns = {"__name__": "__main__", "__builtins__": __builtins__}
    _install_helpers(ns)

    while True:
        try:
            req = read_frame(rf)
        except ValueError as e:
            _fatal(crash_out, str(e))
            return
        except OSError:
            os._exit(0)
        if req is None:
            os._exit(0)  # parent closed the pipe: orderly shutdown

        rid = req.get("id", 0)
        op = req.get("op", "")
        if op == "ping":
            resp = {"id": rid, "ok": True, "op": "ping",
                    "python": sys.version.split()[0],
                    "pid": os.getpid(), "kernel": KERNEL_VERSION,
                    "cwd": os.getcwd()}
        elif op == "exec":
            resp = run_exec(rid, req.get("code", ""), ns,
                            out_path, err_path)
        else:
            resp = {"id": rid, "ok": False,
                    "exc_type": "ProtocolError",
                    "exc_message": "unknown op: %r" % (op,),
                    "stdout": "", "stderr": "", "traceback": "",
                    "duration_ms": 0,
                    "stdout_truncated": False,
                    "stderr_truncated": False}
        try:
            write_frame(wf, resp)
        except OSError:
            os._exit(0)  # parent gone mid-write


if __name__ == "__main__":
    main()
