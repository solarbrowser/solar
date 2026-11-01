"""What the handlers of the web-platform-tests take from wptserve.utils."""


def isomorphic_decode(value):
    return value.decode("iso-8859-1") if isinstance(value, bytes) else value


def isomorphic_encode(value):
    return value.encode("iso-8859-1") if isinstance(value, str) else value


def invert_dict(d):
    return {v: k for k, v in d.items()}
