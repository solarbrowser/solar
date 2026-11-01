#!/usr/bin/env python3
"""Runs one handler of the web-platform-tests (a .py file with main(request, response)) for the server of tests/WptTest.cpp.

usage: tools/wptpy.py HANDLER REQUEST   (REQUEST: the request as the server wrote it: method, url, headers, a blank line, the body)
The answer goes to standard output: the status line ("200 OK"), a line for each header ("name: value"), a blank line, then the body.
It is the part of wptserve that the handlers need: request.GET and POST, headers, cookies, the stash (kept in a file between requests),
response.status, headers, content and the writer.
"""
import base64
import http.cookies
import importlib.util
import json
import os
import pickle
import sys
import tempfile
import traceback
import urllib.parse

try:
    import fcntl
except ImportError:  # (no locking where there is none)
    fcntl = None


def to_bytes(value):
    if isinstance(value, bytes):
        return value
    if isinstance(value, str):
        return value.encode("utf-8")
    return str(value).encode("utf-8")


class MultiDict(dict):
    """The parameters of a query or a form: a name has a list of values (bytes)."""

    def __init__(self, pairs=()):
        super().__init__()
        for key, value in pairs:
            self.setdefault(to_bytes(key), []).append(to_bytes(value))

    def _key(self, key):
        return to_bytes(key)

    def __getitem__(self, key):
        return super().__getitem__(self._key(key))[0]

    def __contains__(self, key):
        return super().__contains__(self._key(key))

    def first(self, key, default=None):
        values = super().get(self._key(key))
        if values:
            return values[0]
        if default is not None:
            return default
        raise KeyError(key)

    def last(self, key, default=None):
        values = super().get(self._key(key))
        if values:
            return values[-1]
        if default is not None:
            return default
        raise KeyError(key)

    def get(self, key, default=None):
        values = super().get(self._key(key))
        return values[0] if values else default

    def get_list(self, key):
        return list(super().get(self._key(key), []))

    def getlist(self, key):
        return self.get_list(key)

    def items(self):
        return [(key, values[0]) for key, values in super().items()]

    def keys(self):
        return list(super().keys())

    def values(self):
        return [values[0] for values in super().values()]


class RequestHeaders(object):
    def __init__(self, pairs):
        self._pairs = [(to_bytes(k), to_bytes(v)) for k, v in pairs]

    def _find(self, key):
        key = to_bytes(key).lower()
        return [v for k, v in self._pairs if k.lower() == key]

    def __getitem__(self, key):
        values = self._find(key)
        if not values:
            raise KeyError(key)
        return b", ".join(values)

    def __contains__(self, key):
        return bool(self._find(key))

    def get(self, key, default=None):
        values = self._find(key)
        return b", ".join(values) if values else default

    def get_list(self, key, default=None):
        values = self._find(key)
        return values if values else (default if default is not None else [])

    def keys(self):
        return list(dict.fromkeys(k.lower() for k, _ in self._pairs))

    def items(self):
        return [(k, self[k]) for k in self.keys()]

    def __iter__(self):
        return iter(self.keys())

    def __len__(self):
        return len(self.keys())


class ResponseHeaders(object):
    def __init__(self):
        self._pairs = []

    def set(self, key, value):
        key = to_bytes(key)
        self._pairs = [(k, v) for k, v in self._pairs if k.lower() != key.lower()]
        self._pairs.append((key, to_bytes(value)))

    def append(self, key, value):
        self._pairs.append((to_bytes(key), to_bytes(value)))

    def get(self, key, default=None):
        key = to_bytes(key).lower()
        values = [v for k, v in self._pairs if k.lower() == key]
        return values if values else (default if default is not None else [])

    def delete(self, key):
        key = to_bytes(key).lower()
        self._pairs = [(k, v) for k, v in self._pairs if k.lower() != key]

    def update(self, pairs):
        for key, value in pairs:
            self.set(key, value)

    def __setitem__(self, key, value):
        self.set(key, value)

    def __contains__(self, key):
        return bool(self.get(key))

    def __iter__(self):
        return iter(self._pairs)

    def items(self):
        return list(self._pairs)


class Stash(object):
    """What handlers keep between requests: in a file of the temporary directory, shared by the processes of the run."""

    def __init__(self, directory):
        self._path = os.path.join(directory, "stash.pickle")

    def _with(self, change):
        lock = open(self._path + ".lock", "a+")
        try:
            if fcntl:
                fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                with open(self._path, "rb") as f:
                    data = pickle.load(f)
            except Exception:
                data = {}
            result = change(data)
            with open(self._path, "wb") as f:
                pickle.dump(data, f)
            return result
        finally:
            lock.close()

    def put(self, key, value, path=None):
        def change(data):
            data[(path, key)] = value
        self._with(change)

    def take(self, key, path=None):
        def change(data):
            return data.pop((path, key), None)
        return self._with(change)

    def take_all(self, key, path=None):
        return self.take(key, path)


class Server(object):
    def __init__(self, stash, config):
        self.stash = stash
        self.config = config


class Request(object):
    def __init__(self, method, url, headers, body, stash, config):
        self.method = method
        self.url = url
        self.url_parts = urllib.parse.urlsplit(url)
        self.raw_headers = RequestHeaders(headers)
        self.headers = self.raw_headers
        self.body = body
        self.raw_input = None
        self.route_params = {}
        self.server = Server(stash, config)
        self.request_path = self.url_parts.path
        self.GET = MultiDict(urllib.parse.parse_qsl(self.url_parts.query, keep_blank_values=True, encoding="latin-1"))
        self._post = None
        self._cookies = None
        self.auth = type("Auth", (), {"username": None, "password": None})()
        authorization = self.headers.get(b"authorization")
        if authorization and authorization.lower().startswith(b"basic "):
            try:
                user, _, password = base64.b64decode(authorization[6:]).partition(b":")
                self.auth.username, self.auth.password = user, password
            except Exception:
                pass

    @property
    def POST(self):
        if self._post is None:
            type_ = self.headers.get(b"content-type", b"")
            if type_.startswith(b"application/x-www-form-urlencoded"):
                self._post = MultiDict(urllib.parse.parse_qsl(self.body.decode("latin-1"), keep_blank_values=True, encoding="latin-1"))
            else:
                self._post = MultiDict()
        return self._post

    @property
    def cookies(self):
        if self._cookies is None:
            self._cookies = {}
            raw = self.headers.get(b"cookie", b"").decode("latin-1")
            for part in raw.split(";"):
                name, _, value = part.strip().partition("=")
                if name:
                    self._cookies[to_bytes(name)] = type("Cookie", (), {"value": to_bytes(value), "__str__": lambda s: s.value.decode("latin-1")})()
        return self._cookies


class Writer(object):
    def __init__(self, response):
        self._response = response
        self._chunks = []
        self.wrote_headers = False

    def write_status(self, code, message=None):
        self._response.status = (code, message or "")

    def write_header(self, name, value):
        self._response.headers.append(name, value)

    def end_headers(self):
        self.wrote_headers = True

    def write_content(self, data):
        self._chunks.append(to_bytes(data))

    def write(self, data):
        self.write_content(data)

    def flush(self):
        pass


class Response(object):
    def __init__(self):
        self.status = 200
        self.headers = ResponseHeaders()
        self.content = b""
        self.writer = Writer(self)
        self.explicit_flush = False
        self.close_connection = False

    def set_cookie(self, name, value, path="/", domain=None, max_age=None, expires=None, secure=False, httponly=False, samesite=None):
        parts = ["%s=%s" % (to_bytes(name).decode("latin-1"), to_bytes(value).decode("latin-1"))]
        if path:
            parts.append("Path=%s" % path)
        if domain:
            parts.append("Domain=%s" % domain)
        if max_age is not None:
            parts.append("Max-Age=%s" % max_age)
        if secure:
            parts.append("Secure")
        if httponly:
            parts.append("HttpOnly")
        if samesite:
            parts.append("SameSite=%s" % samesite)
        self.headers.append(b"Set-Cookie", "; ".join(parts))

    def delete_cookie(self, name, path="/", domain=None):
        self.set_cookie(name, "", path=path, domain=domain, max_age=0)


REASONS = {200: "OK", 201: "Created", 202: "Accepted", 204: "No Content", 206: "Partial Content", 301: "Moved Permanently", 302: "Found", 303: "See Other", 304: "Not Modified",
           307: "Temporary Redirect", 308: "Permanent Redirect", 400: "Bad Request", 401: "Unauthorized", 403: "Forbidden", 404: "Not Found", 405: "Method Not Allowed",
           500: "Internal Server Error"}


def content_bytes(content):
    if content is None:
        return b""
    if isinstance(content, (bytes, str)):
        return to_bytes(content)
    try:
        return b"".join(to_bytes(chunk) for chunk in content)
    except TypeError:
        return to_bytes(content)


def main():
    handler_path, request_path = sys.argv[1], sys.argv[2]
    with open(request_path, "rb") as f:
        raw = f.read()
    head, _, body = raw.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    method = lines[0].decode("latin-1")
    url = lines[1].decode("latin-1")
    headers = []
    for line in lines[2:]:
        name, _, value = line.partition(b":")
        if name:
            headers.append((name.strip(), value.strip()))
    host = urllib.parse.urlsplit(url)
    config = type("Config", (), {"domains": {"": "web-platform.test"}, "ports": {"http": [host.port or 80]}, "server_host": host.hostname, "browser_host": host.hostname,
                                 "browser_host_name": host.hostname})()
    stash = Stash(os.environ.get("WPT_STASH_DIR", tempfile.gettempdir()))
    request = Request(method, url, headers, body, stash, config)
    response = Response()
    sys.path.insert(0, os.path.dirname(os.path.abspath(handler_path)))
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "wptpy"))
    spec = importlib.util.spec_from_file_location("handler", handler_path)
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
        entry = getattr(module, "main", None) or getattr(module, "handler", None)
        if entry is None:
            raise RuntimeError("no main in " + handler_path)
        result = entry(request, response)
        if result is not None:
            if isinstance(result, tuple):
                if len(result) == 3:
                    response.status, extra, response.content = result
                elif len(result) == 2:
                    extra, response.content = result
                else:
                    extra = result[0]
                if isinstance(extra, dict):
                    extra = list(extra.items())
                for key, value in extra:
                    response.headers.set(key, value)
            else:
                response.content = result
    except Exception:
        traceback.print_exc(file=sys.stderr)
        response.status = 500
        response.headers = ResponseHeaders()
        response.headers.set(b"Content-Type", b"text/plain")
        response.content = traceback.format_exc()
    status = response.status
    reason = ""
    if isinstance(status, (tuple, list)):
        status, reason = status[0], to_bytes(status[1]).decode("latin-1") if len(status) > 1 and status[1] else ""
    status = int(status)
    reason = reason or REASONS.get(status, "")
    body_bytes = content_bytes(response.content) + b"".join(response.writer._chunks)
    pairs = response.headers.items()
    if not any(k.lower() == b"content-type" for k, _ in pairs):
        pairs.append((b"Content-Type", b"text/html"))
    head = ("%d %s\n" % (status, reason)).encode("latin-1") + b"".join(k + b": " + v + b"\n" for k, v in pairs) + b"\n"
    sys.stdout.buffer.write(head + body_bytes)

if __name__ == "__main__":
    main()
