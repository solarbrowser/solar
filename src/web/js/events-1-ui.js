// The event interfaces of the specifications that sit on the DOM's Event: UI Events, HTML's and the others
// that a page makes with `new`, and Document.createEvent, which makes them from the old names. Each is a class
// whose members live in a slot of its own, where only this program reaches, so that a getter can tell the
// object it is called on from a lookalike.
(function (global) {
'use strict';

const { Event, EventTarget, TypeError, WeakMap, Object, Reflect, Number, String, Array, DOMException, Symbol } = global;
const { defineProperty, defineProperties } = Object;
const WeakMapGet = WeakMap.prototype.get;
const WeakMapSet = WeakMap.prototype.set;
const eventInit = Event.prototype.initEvent;
const eventPhaseGetter = Object.getOwnPropertyDescriptor(Event.prototype, 'eventPhase').get;
const makeUninitialized = global.__solarEventUninitialized;
const setRelatedTarget = global.__solarEventSetRelatedTarget;
const getRelatedTarget = global.__solarEventRelatedTarget;
delete global.__solarEventUninitialized;
delete global.__solarEventSetRelatedTarget;
delete global.__solarEventRelatedTarget;

// Web IDL's conversions, for the members of the dictionaries.
const toBoolean = (v) => !!v;
const toLong = (v) => v | 0;
const toUnsignedLong = (v) => v >>> 0;
const toUnsignedShort = (v) => (v >>> 0) & 0xFFFF;
const toShort = (v) => (v << 16) >> 16;
const toDouble = (v) => {
  const n = Number(v);
  if (!Number.isFinite(n)) throw new TypeError('The provided double value is non-finite');
  return n;
};
const toUnrestrictedDouble = (v) => Number(v);
const toDomString = (v) => String(v);
const toAny = (v) => v;
const toWindow = (v) => {
  if (v === null || v === undefined) return null;
  if (v !== global) throw new TypeError("Failed to construct: member view is not of type 'Window'.");
  return v;
};
const toEventTarget = (v) => {
  if (v === null || v === undefined) return null;
  if (!(v instanceof EventTarget)) throw new TypeError('The provided value is not of type EventTarget.');
  return v;
};
const toSequence = (v) => {
  if (v === undefined) return [];
  if (v === null || typeof v !== 'object' || typeof v[Symbol.iterator] !== 'function') throw new TypeError('The provided value cannot be converted to a sequence.');
  return Array.from(v);
};

// A member: its name, what it is when there is none, and how it is converted.
const member = (name, fallback, convert) => ({ name, fallback, convert });

const interfaces = {};

// Makes the class `name`, which has `parent` and the `members` of its dictionary, in alphabetical order as
// Web IDL has the dictionary read. `legacyInit` is the old init<Name>() method, as [name, parameters].
const defineInterface = (name, parent, members, legacyInit, extras) => {
  const slots = new WeakMap();
  const sorted = [...members].sort((a, b) => (a.name < b.name ? -1 : 1));
  const C = ({
    [name]: class extends parent {
      constructor(type, eventInitDict) {
        if (arguments.length < 1) throw new TypeError("Failed to construct '" + name + "': 1 argument required, but only 0 present.");
        super(type, eventInitDict);
        const init = eventInitDict === undefined || eventInitDict === null ? {} : eventInitDict;
        const values = {};
        for (const m of sorted) {
          const v = init[m.name];
          values[m.name] = v === undefined ? m.fallback() : m.convert(v);
        }
        WeakMapSet.call(slots, this, values);
        if (Object.prototype.hasOwnProperty.call(values, 'relatedTarget')) setRelatedTarget(this, values.relatedTarget);
      }
    },
  })[name];
  const check = (self) => {
    const values = WeakMapGet.call(slots, self);
    if (values === undefined) throw new TypeError('Illegal invocation');
    return values;
  };
  for (const m of members) {
    const getter = m.name === 'relatedTarget' ? ({ get relatedTarget() { check(this); return getRelatedTarget(this); } }) : ({ get [m.name]() { return check(this)[m.name]; } });
    defineProperty(C.prototype, m.name, { get: Object.getOwnPropertyDescriptor(getter, m.name).get, enumerable: true, configurable: true });
  }
  if (legacyInit) {
    const [initName, parameters] = legacyInit;
    const method = ({
      [initName](...args) {
        const values = check(this);
        if (args.length < 1) throw new TypeError("Failed to execute '" + initName + "' on '" + name + "': 1 argument required, but only 0 present.");
        // While it is being dispatched an event is not changed.
        if (Reflect.apply(eventPhaseGetter, this, []) !== 0) return;
        Reflect.apply(eventInit, this, [args[0], toBoolean(args[1]), toBoolean(args[2])]);
        for (let i = 3; i < parameters.length; i++) {
          const m = parameters[i];
          if (m !== null && i < args.length) {
            values[m.name] = m.convert(args[i]);
            if (m.name === 'relatedTarget') setRelatedTarget(this, values[m.name]);
          }
        }
      },
    })[initName];
    defineProperty(method, 'length', { value: 1, configurable: true });
    defineProperty(C.prototype, initName, { value: method, writable: true, enumerable: true, configurable: true });
  }
  if (extras) extras(C, check);
  defineProperty(global, name, { value: C, writable: true, enumerable: false, configurable: true });
  interfaces[name] = C;
  return C;
};

const nullTarget = () => null;
const falseValue = () => false;
const zero = () => 0;
const emptyString = () => '';

// ---- UI Events ----

const UIEvent = defineInterface('UIEvent', Event, [member('detail', zero, toLong), member('view', nullTarget, toWindow)],
  ['initUIEvent', [null, null, null, member('view', nullTarget, toWindow), member('detail', zero, toLong)]]);

const modifierMembers = [
  member('altKey', falseValue, toBoolean), member('ctrlKey', falseValue, toBoolean), member('metaKey', falseValue, toBoolean), member('shiftKey', falseValue, toBoolean),
  member('modifierAltGraph', falseValue, toBoolean), member('modifierCapsLock', falseValue, toBoolean), member('modifierFn', falseValue, toBoolean),
  member('modifierFnLock', falseValue, toBoolean), member('modifierHyper', falseValue, toBoolean), member('modifierNumLock', falseValue, toBoolean),
  member('modifierScrollLock', falseValue, toBoolean), member('modifierSuper', falseValue, toBoolean), member('modifierSymbol', falseValue, toBoolean),
  member('modifierSymbolLock', falseValue, toBoolean),
];
// Of those, the modifier ones are the dictionary's and not attributes: only the four are.
const publicModifiers = ['altKey', 'ctrlKey', 'metaKey', 'shiftKey'];

const modifierState = (check) => ({
  getModifierState(keyArg) {
    const values = check(this);
    if (arguments.length < 1) throw new TypeError("Failed to execute 'getModifierState': 1 argument required, but only 0 present.");
    const key = String(keyArg);
    switch (key) {
      case 'Alt': return values.altKey;
      case 'Control': return values.ctrlKey;
      case 'Meta': return values.metaKey;
      case 'Shift': return values.shiftKey;
      case 'AltGraph': return values.modifierAltGraph;
      case 'CapsLock': return values.modifierCapsLock;
      case 'Fn': return values.modifierFn;
      case 'FnLock': return values.modifierFnLock;
      case 'Hyper': return values.modifierHyper;
      case 'NumLock': return values.modifierNumLock;
      case 'ScrollLock': return values.modifierScrollLock;
      case 'Super': return values.modifierSuper;
      case 'Symbol': return values.modifierSymbol;
      case 'SymbolLock': return values.modifierSymbolLock;
      default: return false;
    }
  },
}).getModifierState;

const withModifierState = (C, check) => {
  defineProperty(C.prototype, 'getModifierState', { value: modifierState(check), writable: true, enumerable: true, configurable: true });
};

const MouseEvent = (() => {
  const own = [
    member('button', zero, toShort), member('buttons', zero, toUnsignedShort), member('clientX', zero, toDouble), member('clientY', zero, toDouble),
    member('movementX', zero, toDouble), member('movementY', zero, toDouble), member('relatedTarget', nullTarget, toEventTarget),
    member('screenX', zero, toDouble), member('screenY', zero, toDouble),
  ];
  const all = [...own, ...modifierMembers];
  const C = defineInterface('MouseEvent', UIEvent, all,
    ['initMouseEvent', [null, null, null, member('view', nullTarget, toWindow), member('detail', zero, toLong), member('screenX', zero, toDouble), member('screenY', zero, toDouble),
      member('clientX', zero, toDouble), member('clientY', zero, toDouble), member('ctrlKey', falseValue, toBoolean), member('altKey', falseValue, toBoolean),
      member('shiftKey', falseValue, toBoolean), member('metaKey', falseValue, toBoolean), member('button', zero, toShort), member('relatedTarget', nullTarget, toEventTarget)]],
    withModifierState);
  // The modifier members besides the four are not attributes.
  for (const m of modifierMembers) {
    if (!publicModifiers.includes(m.name)) delete C.prototype[m.name];
  }
  // x and y, the page's and the offsets: where the layout has the pointer, which is where it says it is for now.
  const get = (name) => Object.getOwnPropertyDescriptor(C.prototype, name).get;
  const clientX = get('clientX'), clientY = get('clientY');
  const alias = (name, source) => defineProperty(C.prototype, name, {
    get: Object.getOwnPropertyDescriptor({ get [name]() { return Reflect.apply(source, this, []); } }, name).get,
    enumerable: true,
    configurable: true,
  });
  for (const [name, source] of [['x', clientX], ['y', clientY], ['pageX', clientX], ['pageY', clientY], ['offsetX', clientX], ['offsetY', clientY]]) alias(name, source);
  return C;
})();

defineInterface('FocusEvent', UIEvent, [member('relatedTarget', nullTarget, toEventTarget)]);
defineInterface('CompositionEvent', UIEvent, [member('data', emptyString, toDomString)],
  ['initCompositionEvent', [null, null, null, member('view', nullTarget, toWindow), member('data', emptyString, toDomString)]]);
defineInterface('InputEvent', UIEvent, [member('data', nullTarget, (v) => (v === null ? null : String(v))), member('inputType', emptyString, toDomString), member('isComposing', falseValue, toBoolean)]);
defineInterface('WheelEvent', MouseEvent, [member('deltaMode', zero, toUnsignedLong), member('deltaX', zero, toUnrestrictedDouble), member('deltaY', zero, toUnrestrictedDouble), member('deltaZ', zero, toUnrestrictedDouble)]);

const KeyboardEvent = defineInterface('KeyboardEvent', UIEvent, [
  member('charCode', zero, toUnsignedLong), member('code', emptyString, toDomString), member('isComposing', falseValue, toBoolean), member('key', emptyString, toDomString),
  member('keyCode', zero, toUnsignedLong), member('location', zero, toUnsignedLong), member('repeat', falseValue, toBoolean), ...modifierMembers,
], ['initKeyboardEvent', [null, null, null, member('view', nullTarget, toWindow), member('key', emptyString, toDomString), member('location', zero, toUnsignedLong),
  member('ctrlKey', falseValue, toBoolean), member('altKey', falseValue, toBoolean), member('shiftKey', falseValue, toBoolean), member('metaKey', falseValue, toBoolean)]],
withModifierState);
for (const m of modifierMembers) {
  if (!publicModifiers.includes(m.name)) delete KeyboardEvent.prototype[m.name];
}
for (const [name, value] of Object.entries({ DOM_KEY_LOCATION_STANDARD: 0, DOM_KEY_LOCATION_LEFT: 1, DOM_KEY_LOCATION_RIGHT: 2, DOM_KEY_LOCATION_NUMPAD: 3 })) {
  defineProperty(KeyboardEvent, name, { value, enumerable: true });
  defineProperty(KeyboardEvent.prototype, name, { value, enumerable: true });
}
defineProperty(global.WheelEvent, 'DOM_DELTA_PIXEL', { value: 0, enumerable: true });
defineProperty(global.WheelEvent, 'DOM_DELTA_LINE', { value: 1, enumerable: true });
defineProperty(global.WheelEvent, 'DOM_DELTA_PAGE', { value: 2, enumerable: true });
defineProperty(global.WheelEvent.prototype, 'DOM_DELTA_PIXEL', { value: 0, enumerable: true });
defineProperty(global.WheelEvent.prototype, 'DOM_DELTA_LINE', { value: 1, enumerable: true });
defineProperty(global.WheelEvent.prototype, 'DOM_DELTA_PAGE', { value: 2, enumerable: true });

// ---- HTML and the others ----

defineInterface('HashChangeEvent', Event, [member('newURL', emptyString, toDomString), member('oldURL', emptyString, toDomString)],
  ['initHashChangeEvent', [null, null, null, member('oldURL', emptyString, toDomString), member('newURL', emptyString, toDomString)]]);
defineInterface('MessageEvent', Event, [
  member('data', nullTarget, toAny), member('lastEventId', emptyString, toDomString), member('origin', emptyString, toDomString), member('ports', () => Object.freeze([]), (v) => Object.freeze(toSequence(v))),
  member('source', nullTarget, (v) => (v === undefined ? null : v)),
], ['initMessageEvent', [null, null, null, member('data', nullTarget, toAny), member('origin', emptyString, toDomString), member('lastEventId', emptyString, toDomString),
  member('source', nullTarget, (v) => (v === undefined ? null : v)), member('ports', () => [], toSequence)]]);
defineInterface('ErrorEvent', Event, [member('colno', zero, toUnsignedLong), member('error', () => undefined, toAny), member('filename', emptyString, toDomString),
  member('lineno', zero, toUnsignedLong), member('message', emptyString, toDomString)]);
defineInterface('ProgressEvent', Event, [member('lengthComputable', falseValue, toBoolean), member('loaded', zero, toUnrestrictedDouble), member('total', zero, toUnrestrictedDouble)]);
defineInterface('CloseEvent', Event, [member('code', zero, toUnsignedShort), member('reason', emptyString, toDomString), member('wasClean', falseValue, toBoolean)]);
defineInterface('PageTransitionEvent', Event, [member('persisted', falseValue, toBoolean)]);
defineInterface('PromiseRejectionEvent', Event, [member('promise', () => undefined, toAny), member('reason', () => undefined, toAny)]);
defineInterface('StorageEvent', Event, [member('key', nullTarget, (v) => (v === null ? null : String(v))), member('newValue', nullTarget, (v) => (v === null ? null : String(v))),
  member('oldValue', nullTarget, (v) => (v === null ? null : String(v))), member('storageArea', nullTarget, toAny), member('url', emptyString, toDomString)]);
defineInterface('BeforeUnloadEvent', Event, []);
defineInterface('DeviceMotionEvent', Event, []);
defineInterface('DeviceOrientationEvent', Event, []);
defineInterface('DragEvent', MouseEvent, [member('dataTransfer', nullTarget, toAny)]);
defineInterface('TouchEvent', UIEvent, []);
defineInterface('TextEvent', UIEvent, [member('data', emptyString, toDomString)], ['initTextEvent', [null, null, null, member('view', nullTarget, toWindow), member('data', emptyString, toDomString)]]);

// ---- Document.createEvent ----

const byName = {
  beforeunloadevent: 'BeforeUnloadEvent', compositionevent: 'CompositionEvent', customevent: 'CustomEvent', devicemotionevent: 'DeviceMotionEvent',
  deviceorientationevent: 'DeviceOrientationEvent', dragevent: 'DragEvent', event: 'Event', events: 'Event', focusevent: 'FocusEvent', hashchangeevent: 'HashChangeEvent',
  htmlevents: 'Event', keyboardevent: 'KeyboardEvent', messageevent: 'MessageEvent', mouseevent: 'MouseEvent', mouseevents: 'MouseEvent', storageevent: 'StorageEvent',
  svgevents: 'Event', textevent: 'TextEvent', touchevent: 'TouchEvent', uievent: 'UIEvent', uievents: 'UIEvent',
};

const createEvent = ({
  createEvent(interfaceName) {
    if (arguments.length < 1) throw new TypeError("Failed to execute 'createEvent' on 'Document': 1 argument required, but only 0 present.");
    const key = String(interfaceName).toLowerCase();
    const name = Object.prototype.hasOwnProperty.call(byName, key) ? byName[key] : undefined;
    if (name === undefined) throw new DOMException('The provided event type (\'' + String(interfaceName) + '\') is invalid.', 'NotSupportedError');
    const event = Reflect.construct(name === 'Event' ? Event : name === 'CustomEvent' ? global.CustomEvent : interfaces[name], ['']);
    makeUninitialized(event);
    return event;
  },
}).createEvent;
defineProperty(Document.prototype, 'createEvent', { value: createEvent, writable: true, enumerable: true, configurable: true });
})(globalThis);
