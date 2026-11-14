#!/usr/bin/env python3
"""Windows test lab: tiny hand-over server for the home network.

The Linux build machine serves packages and test jobs; the agent on the Windows machine (agent.ps1) polls for
jobs, runs them in the logged-in desktop session and uploads the results. Every path starts with a random token.

  python3 server.py <root> <token> [port]
  GET  /<token>/next            oldest job from <root>/jobs/*.json (moved to jobs/sent/), 204 when there is none
  GET  /<token>/agent.ps1       the agent
  GET  /<token>/files/<name>    a file from <root>/files/
  PUT  /<token>/results/<name>  stored in <root>/results/
"""
import http.server, os, shutil, sys, time
ROOT, TOKEN = sys.argv[1], sys.argv[2]
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8765
HERE = os.path.dirname(os.path.abspath(__file__))

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, fmt, *a):
        with open(os.path.join(ROOT, "server.log"), "a") as f:
            f.write(time.strftime("%H:%M:%S ") + (fmt % a) + "\n")
    def _send(self, code, body=b"", ctype="text/plain"):
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def _path(self):
        p = self.path.split("?")[0].strip("/").split("/")
        return p[1:] if p and p[0] == TOKEN else None
    def do_GET(self):
        p = self._path()
        if p is None: return self._send(404)
        if p == ["next"]:
            with open(os.path.join(ROOT, "agent_seen"), "w") as f: f.write(time.strftime("%Y-%m-%d %H:%M:%S"))
            d = os.path.join(ROOT, "jobs")
            js = sorted(x for x in os.listdir(d) if x.endswith(".json"))
            if not js: return self._send(204)
            src = os.path.join(d, js[0]); body = open(src, "rb").read()
            shutil.move(src, os.path.join(d, "sent", js[0]))
            return self._send(200, body, "application/json")
        if p == ["agent.ps1"]:
            return self._send(200, open(os.path.join(HERE, "agent.ps1"), "rb").read())
        if len(p) == 2 and p[0] == "files" and "/" not in p[1] and ".." not in p[1]:
            f = os.path.join(ROOT, "files", p[1])
            if not os.path.isfile(f): return self._send(404)
            self.send_response(200); self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(os.path.getsize(f))); self.end_headers()
            with open(f, "rb") as src: shutil.copyfileobj(src, self.wfile, 1 << 20)
            return
        self._send(404)
    def do_PUT(self):
        p = self._path()
        if p is None or len(p) != 2 or p[0] != "results" or "/" in p[1] or ".." in p[1]: return self._send(404)
        n = int(self.headers.get("Content-Length", "0")); tmp = os.path.join(ROOT, "results", p[1] + ".part")
        with open(tmp, "wb") as f:
            while n > 0:
                b = self.rfile.read(min(n, 1 << 20))
                if not b: break
                f.write(b); n -= len(b)
        os.replace(tmp, os.path.join(ROOT, "results", p[1])); self._send(200, b"ok")

http.server.ThreadingHTTPServer(("0.0.0.0", PORT), H).serve_forever()
