#include "solar/web/DomBindings.h"

#include "solar/web/DomBindingsInternal.h"

namespace solar::web {

namespace {

template <typename Host>
void Install(Host& host) {
  Quanta::Context& ctx = host.GetContext();
  DefineDomExceptionClass(ctx);
  DefineEventClasses(ctx);
  DefineAbortClasses(ctx);
  DefineMessagingClasses(ctx);

  // What Web IDL has and the embedding surface cannot say: the constants, which are on the
  // interface object and its prototype, and the static side of AbortSignal inheriting EventTarget's.
  host.Evaluate(R"JS(
    (function () {
      const define = (target, constants) => {
        for (const [name, value] of Object.entries(constants)) {
          Object.defineProperty(target, name, { value, writable: false, enumerable: true, configurable: false });
        }
      };
      const codes = {
        INDEX_SIZE_ERR: 1, DOMSTRING_SIZE_ERR: 2, HIERARCHY_REQUEST_ERR: 3, WRONG_DOCUMENT_ERR: 4,
        INVALID_CHARACTER_ERR: 5, NO_DATA_ALLOWED_ERR: 6, NO_MODIFICATION_ALLOWED_ERR: 7, NOT_FOUND_ERR: 8,
        NOT_SUPPORTED_ERR: 9, INUSE_ATTRIBUTE_ERR: 10, INVALID_STATE_ERR: 11, SYNTAX_ERR: 12,
        INVALID_MODIFICATION_ERR: 13, NAMESPACE_ERR: 14, INVALID_ACCESS_ERR: 15, VALIDATION_ERR: 16,
        TYPE_MISMATCH_ERR: 17, SECURITY_ERR: 18, NETWORK_ERR: 19, ABORT_ERR: 20, URL_MISMATCH_ERR: 21,
        QUOTA_EXCEEDED_ERR: 22, TIMEOUT_ERR: 23, INVALID_NODE_TYPE_ERR: 24, DATA_CLONE_ERR: 25,
      };
      define(DOMException, codes);
      define(DOMException.prototype, codes);
      const phases = { NONE: 0, CAPTURING_PHASE: 1, AT_TARGET: 2, BUBBLING_PHASE: 3 };
      define(Event, phases);
      define(Event.prototype, phases);
      Object.setPrototypeOf(AbortSignal, EventTarget);

      // AbortSignal.timeout: a signal that aborts with a TimeoutError after a time. It is made of
      // what a script could write, holding on to the intrinsics it uses so that a page that replaces
      // setTimeout or AbortController afterwards does not change what it does.
      const setTimeoutIntrinsic = setTimeout;
      const Controller = AbortController;
      const Exception = DOMException;
      const timeout = function timeout(milliseconds) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'timeout' on 'AbortSignal': 1 argument required, but only 0 present.");
        const delay = Number(milliseconds);
        if (!(delay >= 0) || delay > 18446744073709551615 || delay !== delay) {
          throw new TypeError("Failed to execute 'timeout' on 'AbortSignal': Value is outside the 'unsigned long long' value range.");
        }
        const controller = new Controller();
        setTimeoutIntrinsic(() => controller.abort(new Exception("signal timed out", "TimeoutError")), Math.floor(delay));
        return controller.signal;
      };
      Object.defineProperty(AbortSignal, "timeout", { value: timeout, writable: true, enumerable: true, configurable: true });
    })();
  )JS");
}

}  // namespace

void InstallDomApis(Quanta::Embed::Realm& realm) { Install(realm); }
void InstallDomApis(Quanta::Embed::Runtime& runtime) { Install(runtime); }

}  // namespace solar::web
