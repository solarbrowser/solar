"""The part of tools.wpt.utils the handlers of the web-platform-tests import."""
import importlib.util


def load_source(modname, filename):
    spec = importlib.util.spec_from_file_location(modname, filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
