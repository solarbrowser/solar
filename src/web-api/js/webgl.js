// WebGL 1 and 2 (https://registry.khronos.org/webgl/specs/latest/): WebGLRenderingContext, WebGL2RenderingContext, the objects, the extensions. The drawing is the OpenGL ES 3 context
// of src/paint/WebGl.cpp (the machine's, or ANGLE's); this file is the interface of the standard on it: the checks of arguments, the pixel storage of WebGL's own, the sources of
// textures, the values getParameter gives back, and the extensions (those that the ES 3 context has already are wrappers).
(() => {
  "use strict";
  const N = {};
  for (const n of ["GlOps", "GlCreate", "GlInfo", "GlResize", "GlDestroy", "Gl", "GlPresent"]) { N[n] = globalThis["__solar" + n]; delete globalThis["__solar" + n]; }
  const C = globalThis.__solarGlConstants;
  delete globalThis.__solarGlConstants;
  const internals = globalThis.__solarCanvasInternals;
  delete globalThis.__solarCanvasInternals;
  if (typeof N.GlCreate !== "function" || !internals || !C) return;
  const define = (target, name, value) => Object.defineProperty(target, name, { value, writable: true, configurable: true, enumerable: false });
  const OP = {};
  N.GlOps().forEach((name, i) => { OP[name] = i; });
  const call = N.Gl;

  // ---- The objects ----
  const token = Symbol("webgl");
  class WebGLObject { constructor(t, context, name) { if (t !== token) throw new TypeError("Illegal constructor"); this._ctx = context; this._name = name; this._deleted = false; this._label = null; } }
  const objectClass = (name) => {
    const cls = { [name]: class extends WebGLObject {} }[name];
    Object.defineProperty(cls.prototype, Symbol.toStringTag, { value: name, configurable: true });
    define(globalThis, name, cls);
    return cls;
  };
  const Buffer = objectClass("WebGLBuffer"), Texture = objectClass("WebGLTexture"), Program = objectClass("WebGLProgram"), Shader = objectClass("WebGLShader"),
    Framebuffer = objectClass("WebGLFramebuffer"), Renderbuffer = objectClass("WebGLRenderbuffer"), Query = objectClass("WebGLQuery"), Sampler = objectClass("WebGLSampler"),
    TransformFeedback = objectClass("WebGLTransformFeedback"), VertexArray = objectClass("WebGLVertexArrayObject"), Sync = objectClass("WebGLSync");
  class WebGLUniformLocation { constructor(t, program, location, info) { if (t !== token) throw new TypeError("Illegal constructor"); this._program = program; this._loc = location; this._info = info; } }
  class WebGLActiveInfo { constructor(name, size, type) { Object.defineProperties(this, { name: { value: name, enumerable: true }, size: { value: size, enumerable: true }, type: { value: type, enumerable: true } }); } }
  class WebGLShaderPrecisionFormat { constructor(a, b, c) { Object.defineProperties(this, { rangeMin: { value: a, enumerable: true }, rangeMax: { value: b, enumerable: true }, precision: { value: c, enumerable: true } }); } }
  class WebGLContextEvent extends Event {
    #message;
    constructor(type, init = {}) { super(type, init); this.#message = init.statusMessage === undefined ? "" : String(init.statusMessage); }
    get statusMessage() { return this.#message; }
  }
  for (const [name, cls] of [["WebGLUniformLocation", WebGLUniformLocation], ["WebGLActiveInfo", WebGLActiveInfo], ["WebGLShaderPrecisionFormat", WebGLShaderPrecisionFormat], ["WebGLContextEvent", WebGLContextEvent]]) {
    Object.defineProperty(cls.prototype, Symbol.toStringTag, { value: name, configurable: true });
    define(globalThis, name, cls);
  }

  // ---- Types of uniforms ----
  // type -> [kind (0 float, 1 int, 2 unsigned, 3 bool), components, matrix]
  const UNIFORM_TYPES = {
    [C.FLOAT]: [0, 1], [C.FLOAT_VEC2]: [0, 2], [C.FLOAT_VEC3]: [0, 3], [C.FLOAT_VEC4]: [0, 4], [C.INT]: [1, 1], [C.INT_VEC2]: [1, 2], [C.INT_VEC3]: [1, 3], [C.INT_VEC4]: [1, 4],
    [C.UNSIGNED_INT]: [2, 1], [C.UNSIGNED_INT_VEC2]: [2, 2], [C.UNSIGNED_INT_VEC3]: [2, 3], [C.UNSIGNED_INT_VEC4]: [2, 4], [C.BOOL]: [3, 1], [C.BOOL_VEC2]: [3, 2], [C.BOOL_VEC3]: [3, 3],
    [C.BOOL_VEC4]: [3, 4], [C.FLOAT_MAT2]: [0, 4], [C.FLOAT_MAT3]: [0, 9], [C.FLOAT_MAT4]: [0, 16], [C.FLOAT_MAT2x3]: [0, 6], [C.FLOAT_MAT2x4]: [0, 8], [C.FLOAT_MAT3x2]: [0, 6],
    [C.FLOAT_MAT3x4]: [0, 12], [C.FLOAT_MAT4x2]: [0, 8], [C.FLOAT_MAT4x3]: [0, 12],
  };
  for (const t of ["SAMPLER_2D", "SAMPLER_CUBE", "SAMPLER_3D", "SAMPLER_2D_SHADOW", "SAMPLER_2D_ARRAY", "SAMPLER_2D_ARRAY_SHADOW", "SAMPLER_CUBE_SHADOW", "INT_SAMPLER_2D", "INT_SAMPLER_3D",
    "INT_SAMPLER_CUBE", "INT_SAMPLER_2D_ARRAY", "UNSIGNED_INT_SAMPLER_2D", "UNSIGNED_INT_SAMPLER_3D", "UNSIGNED_INT_SAMPLER_CUBE", "UNSIGNED_INT_SAMPLER_2D_ARRAY"]) UNIFORM_TYPES[C[t]] = [1, 1];

  // ---- Pixel formats ----
  const COMPONENTS = { [C.RGBA]: 4, [C.RGB]: 3, [C.LUMINANCE_ALPHA]: 2, [C.LUMINANCE]: 1, [C.ALPHA]: 1, [C.RED]: 1, [C.RG]: 2, [C.RED_INTEGER]: 1, [C.RG_INTEGER]: 2, [C.RGB_INTEGER]: 3,
    [C.RGBA_INTEGER]: 4, [C.DEPTH_COMPONENT]: 1, [C.DEPTH_STENCIL]: 1 };
  const TYPE_BYTES = { [C.UNSIGNED_BYTE]: 1, [C.BYTE]: 1, [C.UNSIGNED_SHORT]: 2, [C.SHORT]: 2, [C.HALF_FLOAT]: 2, [C.UNSIGNED_INT]: 4, [C.INT]: 4, [C.FLOAT]: 4 };
  const PACKED = { [C.UNSIGNED_SHORT_5_6_5]: 2, [C.UNSIGNED_SHORT_4_4_4_4]: 2, [C.UNSIGNED_SHORT_5_5_5_1]: 2, [C.UNSIGNED_INT_2_10_10_10_REV]: 4, [C.UNSIGNED_INT_10F_11F_11F_REV]: 4,
    [C.UNSIGNED_INT_5_9_9_9_REV]: 4, [C.UNSIGNED_INT_24_8]: 4, [C.FLOAT_32_UNSIGNED_INT_24_8_REV]: 8 };
  const bytesPerPixel = (format, type) => PACKED[type] || (COMPONENTS[format] || 0) * (TYPE_BYTES[type] || 0);

  const ARRAYS = { 0x1400: Int8Array, 0x1401: Uint8Array, 0x1402: Int16Array, 0x1403: Uint16Array, 0x1404: Int32Array, 0x1405: Uint32Array, 0x1406: Float32Array };
  const isView = (v) => ArrayBuffer.isView(v);
  const bytesOf = (v) => (v instanceof ArrayBuffer ? new Uint8Array(v) : v);
  const numbers = (list) => Array.from(list, Number);

  // The values that are the standard's and not OpenGL's: they stay here.
  const PNAMES_JS = new Set([C.UNPACK_FLIP_Y_WEBGL, C.UNPACK_PREMULTIPLY_ALPHA_WEBGL, C.UNPACK_COLORSPACE_CONVERSION_WEBGL]);

  const SUPPORTED_ATTRIBUTES = ["alpha", "depth", "stencil", "antialias", "premultipliedAlpha", "preserveDrawingBuffer", "powerPreference", "failIfMajorPerformanceCaveat", "desynchronized"];

  // ---- The contexts ----
  class WebGLRenderingContext {
    constructor(t, canvas, kind, attributes, width, height) {
      if (t !== token) throw new TypeError("Illegal constructor");
      this._canvas = canvas; this._kind = kind;
      const a = attributes || {};
      this._attrs = {
        alpha: a.alpha === undefined ? true : !!a.alpha, depth: a.depth === undefined ? true : !!a.depth, stencil: !!a.stencil, antialias: a.antialias === undefined ? true : !!a.antialias,
        premultipliedAlpha: a.premultipliedAlpha === undefined ? true : !!a.premultipliedAlpha, preserveDrawingBuffer: !!a.preserveDrawingBuffer,
        powerPreference: a.powerPreference === undefined ? "default" : String(a.powerPreference), failIfMajorPerformanceCaveat: !!a.failIfMajorPerformanceCaveat, desynchronized: !!a.desynchronized,
      };
      this._error = 0; this._lost = false; this._extensions = new Map();
      this._unpack = { flipY: false, premultiply: false, colorspace: C.BROWSER_DEFAULT_WEBGL, alignment: 4, rowLength: 0, imageHeight: 0, skipPixels: 0, skipRows: 0, skipImages: 0 };
      this._bound = new Map();  // target -> object
      this._units = []; this._activeUnit = 0;
      this._vao = null; this._program = null; this._locations = new Map();
      this._width = width; this._height = height;
      this._create();
    }
    _create() {
      const made = N.GlCreate(this._kind, JSON.stringify(this._attrs), Math.max(1, this._width), Math.max(1, this._height));
      if (!made) throw new Error("no context");
      [this._id, this._surface] = made;
      this._info = JSON.parse(N.GlInfo(this._id));
      this._fbo = this._info.defaultFramebuffer;
      internals.surfaces.set(this._canvas, this._surface);
      if (typeof HTMLCanvasElement === "function" && this._canvas instanceof HTMLCanvasElement) internals.attach(this._surface, this._canvas);
      this._native = new Set(this._info.extensions);
    }
    _resize(w, h) {
      this._width = w; this._height = h;
      if (!this._lost) N.GlResize(this._id, Math.max(1, w), Math.max(1, h));
    }
    _present() { if (!this._lost) N.GlPresent(this._id); }
    _op(name, ...args) { return call(this._id, OP[name], ...args); }
    _setError(e) { if (!this._error) this._error = e; }
    // The name of an object the script gave, or 0 for none; -1 when it is not usable (the error is set).
    _use(obj, cls, allowDeleted) {
      if (obj === null || obj === undefined) return 0;
      if (!(obj instanceof cls)) throw new TypeError("parameter is not of type '" + cls.prototype[Symbol.toStringTag] + "'.");
      if (obj._ctx !== this || (obj._deleted && !allowDeleted)) { this._setError(C.INVALID_OPERATION); return -1; }
      return obj._name;
    }
    get canvas() { return this._canvas; }
    get drawingBufferWidth() { return Math.max(1, this._width); }
    get drawingBufferHeight() { return Math.max(1, this._height); }
    get drawingBufferColorSpace() { return "srgb"; }
    get unpackColorSpace() { return "srgb"; }
    getContextAttributes() { return this._lost ? null : { ...this._attrs }; }
    isContextLost() { return this._lost; }
    makeXRCompatible() { return Promise.resolve(); }
    getError() {
      if (this._lost) return 0;
      if (this._error) { const e = this._error; this._error = 0; return e; }
      return this._op("getError");
    }
  }
  const proto = WebGLRenderingContext.prototype;
  const method = (name, fn) => define(proto, name, fn);

  // Calls that are the ES function with the same numbers.
  const PASS = {
    activeTexture: "activeTexture", blendColor: "blendColor", blendEquation: "blendEquation", blendEquationSeparate: "blendEquationSeparate", blendFunc: "blendFunc",
    blendFuncSeparate: "blendFuncSeparate", clearColor: "clearColor", clearDepth: "clearDepthf", clearStencil: "clearStencil", colorMask: "colorMask", cullFace: "cullFace",
    depthFunc: "depthFunc", depthMask: "depthMask", depthRange: "depthRangef", disable: "disable", disableVertexAttribArray: "disableVertexAttribArray", enable: "enable",
    enableVertexAttribArray: "enableVertexAttribArray", finish: "finish", flush: "flush", frontFace: "frontFace", hint: "hint", lineWidth: "lineWidth", polygonOffset: "polygonOffset",
    sampleCoverage: "sampleCoverage", scissor: "scissor", stencilFunc: "stencilFunc", stencilFuncSeparate: "stencilFuncSeparate", stencilMask: "stencilMask",
    stencilMaskSeparate: "stencilMaskSeparate", stencilOp: "stencilOp", stencilOpSeparate: "stencilOpSeparate", vertexAttrib1f: "vertexAttrib1f", vertexAttrib2f: "vertexAttrib2f",
    vertexAttrib3f: "vertexAttrib3f", vertexAttrib4f: "vertexAttrib4f", copyTexImage2D: "copyTexImage2D", copyTexSubImage2D: "copyTexSubImage2D", renderbufferStorage: "renderbufferStorage",
    texParameterf: "texParameterf", texParameteri: "texParameteri", validateProgram: null, viewport: "viewport", drawArrays: "drawArrays", generateMipmap: "generateMipmap",
  };
  const ARGC = { activeTexture: 1, blendColor: 4, blendEquation: 1, blendEquationSeparate: 2, blendFunc: 2, blendFuncSeparate: 4, clearColor: 4, clearDepth: 1, clearStencil: 1, colorMask: 4,
    cullFace: 1, depthFunc: 1, depthMask: 1, depthRange: 2, disable: 1, disableVertexAttribArray: 1, enable: 1, enableVertexAttribArray: 1, frontFace: 1, hint: 2, lineWidth: 1,
    polygonOffset: 2, sampleCoverage: 2, scissor: 4, stencilFunc: 3, stencilFuncSeparate: 4, stencilMask: 1, stencilMaskSeparate: 2, stencilOp: 3, stencilOpSeparate: 4, vertexAttrib1f: 2,
    vertexAttrib2f: 3, vertexAttrib3f: 4, vertexAttrib4f: 5, copyTexImage2D: 8, copyTexSubImage2D: 8, renderbufferStorage: 4, texParameterf: 3, texParameteri: 3, viewport: 4, drawArrays: 3,
    generateMipmap: 1 };
  for (const [name, op] of Object.entries(PASS)) {
    if (!op) continue;
    const argc = ARGC[name] || 0;
    method(name, function (...a) {
      if (a.length < argc) throw new TypeError(`Failed to execute '${name}' on 'WebGLRenderingContext': ${argc} arguments required, but only ${a.length} present.`);
      if (this._lost) return;
      if (name === "activeTexture") this._activeUnit = Number(a[0]) - C.TEXTURE0;
      return this._op(op, ...a.slice(0, argc).map(Number));
    });
  }
  method("clear", function clear(mask) { if (!this._lost) this._op("clear", Number(mask)); });
  method("getSupportedExtensions", function getSupportedExtensions() { return this._lost ? null : Object.keys(EXTENSIONS).filter((n) => EXTENSIONS[n].has(this)); });

  // ---- Creating and deleting ----
  const makers = [["createBuffer", Buffer, "createBuffer"], ["createTexture", Texture, "createTexture"], ["createFramebuffer", Framebuffer, "createFramebuffer"],
    ["createRenderbuffer", Renderbuffer, "createRenderbuffer"]];
  for (const [name, cls, op] of makers) method(name, function () { return this._lost ? null : new cls(token, this, this._op(op)); });
  method("createProgram", function createProgram() { return this._lost ? null : new Program(token, this, this._op("createProgram")); });
  method("createShader", function createShader(type) {
    if (this._lost) return null;
    type = Number(type);
    if (type !== C.VERTEX_SHADER && type !== C.FRAGMENT_SHADER) { this._setError(C.INVALID_ENUM); return null; }
    const s = new Shader(token, this, this._op("createShader", type));
    s._type = type;
    return s;
  });
  const deleters = [["deleteBuffer", Buffer, "deleteBuffer"], ["deleteTexture", Texture, "deleteTexture"], ["deleteFramebuffer", Framebuffer, "deleteFramebuffer"],
    ["deleteRenderbuffer", Renderbuffer, "deleteRenderbuffer"], ["deleteProgram", Program, "deleteProgram"], ["deleteShader", Shader, "deleteShader"]];
  for (const [name, cls, op] of deleters) {
    method(name, function (obj) {
      if (this._lost) return;
      const n = this._use(obj, cls, true);
      if (n <= 0 || obj._deleted) return;
      obj._deleted = true;
      this._op(op, n);
      for (const [target, bound] of this._bound) if (bound === obj) this._bound.delete(target);
      if (obj === this._program) this._program = null;
    });
  }
  const isers = [["isBuffer", Buffer, "isBuffer"], ["isTexture", Texture, "isTexture"], ["isFramebuffer", Framebuffer, "isFramebuffer"], ["isRenderbuffer", Renderbuffer, "isRenderbuffer"],
    ["isProgram", Program, "isProgram"], ["isShader", Shader, "isShader"]];
  for (const [name, cls, op] of isers) method(name, function (obj) { return !this._lost && obj instanceof cls && obj._ctx === this && !obj._deleted && !!this._op(op, obj._name); });

  // ---- Binding ----
  method("bindBuffer", function bindBuffer(target, buffer) {
    if (this._lost) return;
    const n = this._use(buffer, Buffer); if (n < 0) return;
    target = Number(target);
    this._op("bindBuffer", target, n);
    if (target === C.ELEMENT_ARRAY_BUFFER && this._vao) this._vao._element = buffer; else this._bound.set(target, buffer);
  });
  method("bindTexture", function bindTexture(target, texture) {
    if (this._lost) return;
    const n = this._use(texture, Texture); if (n < 0) return;
    target = Number(target);
    this._op("bindTexture", target, n);
    this._bound.set("t" + this._activeUnit + ":" + target, texture);
  });
  method("bindRenderbuffer", function bindRenderbuffer(target, rb) {
    if (this._lost) return;
    const n = this._use(rb, Renderbuffer); if (n < 0) return;
    this._op("bindRenderbuffer", Number(target), n);
    this._bound.set(C.RENDERBUFFER, rb);
  });
  method("bindFramebuffer", function bindFramebuffer(target, fb) {
    if (this._lost) return;
    const n = this._use(fb, Framebuffer); if (n < 0) return;
    target = Number(target);
    this._op("bindFramebuffer", target, n);
    if (target === C.FRAMEBUFFER || target === C.DRAW_FRAMEBUFFER) this._bound.set(C.DRAW_FRAMEBUFFER, fb);
    if (target === C.FRAMEBUFFER || target === C.READ_FRAMEBUFFER) this._bound.set(C.READ_FRAMEBUFFER, fb);
    if (target === C.FRAMEBUFFER) this._bound.set(C.FRAMEBUFFER, fb);
  });
  method("framebufferTexture2D", function framebufferTexture2D(target, attachment, textarget, texture, level) {
    if (this._lost) return;
    const n = this._use(texture, Texture); if (n < 0) return;
    this._op("framebufferTexture2D", Number(target), Number(attachment), Number(textarget), n, Number(level));
  });
  method("framebufferRenderbuffer", function framebufferRenderbuffer(target, attachment, rbTarget, rb) {
    if (this._lost) return;
    const n = this._use(rb, Renderbuffer); if (n < 0) return;
    this._op("framebufferRenderbuffer", Number(target), Number(attachment), Number(rbTarget), n);
  });
  method("checkFramebufferStatus", function checkFramebufferStatus(target) { return this._lost ? 0 : this._op("checkFramebufferStatus", Number(target)); });

  // ---- Buffers and vertex arrays ----
  method("bufferData", function bufferData(target, data, usage, srcOffset, length) {
    if (this._lost) return;
    if (arguments.length < 3) throw new TypeError("Failed to execute 'bufferData' on 'WebGLRenderingContext': 3 arguments required, but only " + arguments.length + " present.");
    if (data === null) { this._setError(C.INVALID_VALUE); return; }
    if (typeof data === "number" || (!isView(data) && !(data instanceof ArrayBuffer))) {
      const size = Number(data);
      if (!(size >= 0)) { this._setError(C.INVALID_VALUE); return; }
      this._op("bufferData", Number(target), size, Number(usage));
      return;
    }
    let view = bytesOf(data);
    if (srcOffset !== undefined) {
      const element = view.BYTES_PER_ELEMENT || 1;
      const start = Number(srcOffset) || 0;
      const count = length ? Number(length) : view.length - start;
      view = new Uint8Array(view.buffer, view.byteOffset + start * element, count * element);
    }
    this._op("bufferData", Number(target), view, Number(usage));
  });
  method("bufferSubData", function bufferSubData(target, offset, data, srcOffset, length) {
    if (this._lost) return;
    if (data === null) { this._setError(C.INVALID_VALUE); return; }
    let view = bytesOf(data);
    if (srcOffset !== undefined) {
      const element = view.BYTES_PER_ELEMENT || 1;
      const start = Number(srcOffset) || 0;
      const count = length ? Number(length) : view.length - start;
      view = new Uint8Array(view.buffer, view.byteOffset + start * element, count * element);
    }
    this._op("bufferSubData", Number(target), Number(offset), view);
  });
  method("getBufferParameter", function getBufferParameter(target, pname) { return this._lost ? null : this._op("getBufferParameter", Number(target), Number(pname)); });
  method("vertexAttribPointer", function vertexAttribPointer(index, size, type, normalized, stride, offset) {
    if (this._lost) return;
    this._op("vertexAttribPointer", Number(index), Number(size), Number(type), normalized ? 1 : 0, Number(stride), Number(offset));
  });
  method("drawElements", function drawElements(mode, count, type, offset) { if (!this._lost) this._op("drawElements", Number(mode), Number(count), Number(type), Number(offset)); });
  method("getVertexAttrib", function getVertexAttrib(index, pname) {
    if (this._lost) return null;
    pname = Number(pname);
    const v = this._op("getVertexAttrib", Number(index), pname);
    if (pname === C.CURRENT_VERTEX_ATTRIB) return new Float32Array(v);
    if (pname === C.VERTEX_ATTRIB_ARRAY_BUFFER_BINDING) return this._bufferNamed(v);
    if (pname === C.VERTEX_ATTRIB_ARRAY_ENABLED || pname === C.VERTEX_ATTRIB_ARRAY_NORMALIZED || pname === C.VERTEX_ATTRIB_ARRAY_INTEGER) return !!v;
    return v;
  });
  method("getVertexAttribOffset", function getVertexAttribOffset(index, pname) { return this._lost ? 0 : this._op("getVertexAttribOffset", Number(index)); });
  const buffersByName = new WeakMap();  // context -> Map(name -> WebGLBuffer) of those still alive in the script's hands (weak)
  method("_bufferNamed", function (name) {
    if (!name) return null;
    for (const b of this._bound.values()) if (b instanceof Buffer && b._name === name) return b;
    return null;
  });

  // ---- Textures ----
  const flipRows = (data, width, height, rowBytes) => {
    const out = new Uint8Array(data.length);
    for (let y = 0; y < height; y++) out.set(data.subarray((height - 1 - y) * rowBytes, (height - y) * rowBytes), y * rowBytes);
    return out;
  };
  // The bytes of a source (pixels from an element, ImageData, canvas, bitmap), as the format and type asked for.
  const fromSource = (gl, source, format, type, method) => {
    const picture = internals.pixelsOf(source, method);
    if (!picture) return null;
    let { width, height, data } = picture;
    const u = gl._unpack;
    if (u.flipY) data = flipRows(data, width, height, width * 4);
    if (u.premultiply) {
      const out = new Uint8Array(data);
      for (let i = 0; i < out.length; i += 4) { const a = out[i + 3]; if (a !== 255) { out[i] = Math.round(out[i] * a / 255); out[i + 1] = Math.round(out[i + 1] * a / 255); out[i + 2] = Math.round(out[i + 2] * a / 255); } }
      data = out;
    }
    let out = data;
    if (type === C.UNSIGNED_BYTE || type === undefined) {
      if (format === C.RGB) { out = new Uint8Array(width * height * 3); for (let i = 0, j = 0; i < data.length; i += 4) { out[j++] = data[i]; out[j++] = data[i + 1]; out[j++] = data[i + 2]; } }
      else if (format === C.LUMINANCE) { out = new Uint8Array(width * height); for (let i = 0; i < data.length; i += 4) out[i / 4] = data[i]; }
      else if (format === C.ALPHA) { out = new Uint8Array(width * height); for (let i = 0; i < data.length; i += 4) out[i / 4] = data[i + 3]; }
      else if (format === C.LUMINANCE_ALPHA) { out = new Uint8Array(width * height * 2); for (let i = 0, j = 0; i < data.length; i += 4) { out[j++] = data[i]; out[j++] = data[i + 3]; } }
      else if (format === C.RED) { out = new Uint8Array(width * height); for (let i = 0; i < data.length; i += 4) out[i / 4] = data[i]; }
      else if (format === C.RG) { out = new Uint8Array(width * height * 2); for (let i = 0, j = 0; i < data.length; i += 4) { out[j++] = data[i]; out[j++] = data[i + 1]; } }
    } else if (type === C.UNSIGNED_SHORT_4_4_4_4 && format === C.RGBA) {
      const o = new Uint16Array(width * height);
      for (let i = 0; i < o.length; i++) o[i] = ((data[i * 4] >> 4) << 12) | ((data[i * 4 + 1] >> 4) << 8) | ((data[i * 4 + 2] >> 4) << 4) | (data[i * 4 + 3] >> 4);
      out = new Uint8Array(o.buffer);
    } else if (type === C.UNSIGNED_SHORT_5_5_5_1 && format === C.RGBA) {
      const o = new Uint16Array(width * height);
      for (let i = 0; i < o.length; i++) o[i] = ((data[i * 4] >> 3) << 11) | ((data[i * 4 + 1] >> 3) << 6) | ((data[i * 4 + 2] >> 3) << 1) | (data[i * 4 + 3] >> 7);
      out = new Uint8Array(o.buffer);
    } else if (type === C.UNSIGNED_SHORT_5_6_5 && format === C.RGB) {
      const o = new Uint16Array(width * height);
      for (let i = 0; i < o.length; i++) o[i] = ((data[i * 4] >> 3) << 11) | ((data[i * 4 + 1] >> 2) << 5) | (data[i * 4 + 2] >> 3);
      out = new Uint8Array(o.buffer);
    } else if (type === C.FLOAT) {
      const n = COMPONENTS[format] || 4;
      const o = new Float32Array(width * height * n);
      for (let i = 0; i < width * height; i++) for (let c = 0; c < n; c++) o[i * n + c] = data[i * 4 + (format === C.ALPHA ? 3 : c)] / 255;
      out = new Uint8Array(o.buffer);
    }
    return { width, height, data: out };
  };
  // The unpack state for the bytes of a source (tight, nothing skipped), and back.
  const withTightUnpack = (gl, fn) => {
    gl._op("pixelStorei", C.UNPACK_ALIGNMENT, 1);
    if (gl._kind === 2) { gl._op("pixelStorei", C.UNPACK_ROW_LENGTH, 0); gl._op("pixelStorei", C.UNPACK_SKIP_PIXELS, 0); gl._op("pixelStorei", C.UNPACK_SKIP_ROWS, 0); gl._op("pixelStorei", C.UNPACK_IMAGE_HEIGHT, 0); gl._op("pixelStorei", C.UNPACK_SKIP_IMAGES, 0); }
    try { return fn(); } finally {
      const u = gl._unpack;
      gl._op("pixelStorei", C.UNPACK_ALIGNMENT, u.alignment);
      if (gl._kind === 2) { gl._op("pixelStorei", C.UNPACK_ROW_LENGTH, u.rowLength); gl._op("pixelStorei", C.UNPACK_SKIP_PIXELS, u.skipPixels); gl._op("pixelStorei", C.UNPACK_SKIP_ROWS, u.skipRows); gl._op("pixelStorei", C.UNPACK_IMAGE_HEIGHT, u.imageHeight); gl._op("pixelStorei", C.UNPACK_SKIP_IMAGES, u.skipImages); }
    }
  };
  const isSource = (v) => v !== null && typeof v === "object" && !isView(v) && !(v instanceof ArrayBuffer);
  // Bytes from an ArrayBufferView, with the flip and the premultiplication of the pixel storage applied where the layout is known.
  const prepareView = (gl, view, width, height, format, type, depth = 1) => {
    if (view === null || view === undefined) return null;
    const u = gl._unpack;
    if (!u.flipY && !u.premultiply) return bytesOf(view);
    const bytes = new Uint8Array(bytesOf(view).buffer, bytesOf(view).byteOffset, bytesOf(view).byteLength);
    const bpp = bytesPerPixel(format, type);
    if (!bpp || !width || !height) return bytesOf(view);
    const rowLength = (u.rowLength || width) * bpp;
    const rowBytes = Math.ceil(rowLength / u.alignment) * u.alignment;
    if (bytes.length < rowBytes * (height - 1) + width * bpp) return bytesOf(view);
    let out = new Uint8Array(bytes);
    if (u.flipY && depth === 1) {
      for (let y = 0; y < height; y++) out.set(bytes.subarray((height - 1 - y) * rowBytes, (height - 1 - y) * rowBytes + width * bpp), y * rowBytes);
    }
    if (u.premultiply && type === C.UNSIGNED_BYTE && format === C.RGBA) {
      for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
        const i = y * rowBytes + x * 4, a = out[i + 3];
        if (a !== 255) { out[i] = Math.round(out[i] * a / 255); out[i + 1] = Math.round(out[i + 1] * a / 255); out[i + 2] = Math.round(out[i + 2] * a / 255); }
      }
    }
    return out;
  };
  const sliceView = (view, srcOffset) => {
    if (srcOffset === undefined || !isView(view)) return view;
    const e = view.BYTES_PER_ELEMENT || 1;
    return new Uint8Array(view.buffer, view.byteOffset + Number(srcOffset) * e, view.byteLength - Number(srcOffset) * e);
  };
  method("texImage2D", function texImage2D(target, level, internalformat, ...rest) {
    if (this._lost) return;
    target = Number(target); level = Number(level); internalformat = Number(internalformat);
    if (rest.length === 3 || (rest.length === 6 && isSource(rest[5]) && this._kind === 2 && typeof rest[0] === "number" && rest.length === 6 && false)) {
      // (target, level, internalformat, format, type, source)
      const [format, type, source] = rest;
      if (!isSource(source)) { if (source === null) { this._setError(C.INVALID_VALUE); return; } throw new TypeError("Failed to execute 'texImage2D' on 'WebGLRenderingContext': The provided value is not of type 'TexImageSource'."); }
      const made = fromSource(this, source, Number(format), Number(type), "texImage2D");
      if (!made) return;
      withTightUnpack(this, () => this._op("texImage2D", target, level, internalformat, made.width, made.height, 0, Number(format), Number(type), made.data));
      return;
    }
    const [width, height, border, format, type, pixels, srcOffset] = rest;
    if (rest.length < 6) throw new TypeError("Failed to execute 'texImage2D' on 'WebGLRenderingContext': 6 arguments required, but only " + (rest.length + 3) + " present.");
    if (isSource(pixels)) {
      // (the sized overload of WebGL 2 with a source)
      const made = fromSource(this, pixels, Number(format), Number(type), "texImage2D");
      if (!made) return;
      withTightUnpack(this, () => this._op("texImage2D", target, level, internalformat, Number(width), Number(height), Number(border), Number(format), Number(type), made.data));
      return;
    }
    if (typeof pixels === "number") { this._op("texImage2D", target, level, internalformat, Number(width), Number(height), Number(border), Number(format), Number(type), pixels); return; }
    const view = pixels === undefined ? null : prepareView(this, sliceView(pixels, srcOffset), Number(width), Number(height), Number(format), Number(type));
    this._op("texImage2D", target, level, internalformat, Number(width), Number(height), Number(border), Number(format), Number(type), view);
  });
  method("texSubImage2D", function texSubImage2D(target, level, xoffset, yoffset, ...rest) {
    if (this._lost) return;
    target = Number(target); level = Number(level); xoffset = Number(xoffset); yoffset = Number(yoffset);
    if (rest.length === 3) {
      const [format, type, source] = rest;
      if (!isSource(source)) throw new TypeError("Failed to execute 'texSubImage2D' on 'WebGLRenderingContext': The provided value is not of type 'TexImageSource'.");
      const made = fromSource(this, source, Number(format), Number(type), "texSubImage2D");
      if (!made) return;
      withTightUnpack(this, () => this._op("texSubImage2D", target, level, xoffset, yoffset, made.width, made.height, Number(format), Number(type), made.data));
      return;
    }
    const [width, height, format, type, pixels, srcOffset] = rest;
    if (isSource(pixels)) {
      const made = fromSource(this, pixels, Number(format), Number(type), "texSubImage2D");
      if (!made) return;
      withTightUnpack(this, () => this._op("texSubImage2D", target, level, xoffset, yoffset, Number(width), Number(height), Number(format), Number(type), made.data));
      return;
    }
    if (typeof pixels === "number") { this._op("texSubImage2D", target, level, xoffset, yoffset, Number(width), Number(height), Number(format), Number(type), pixels); return; }
    if (pixels === null || pixels === undefined) { this._setError(C.INVALID_VALUE); return; }
    this._op("texSubImage2D", target, level, xoffset, yoffset, Number(width), Number(height), Number(format), Number(type), prepareView(this, sliceView(pixels, srcOffset), Number(width), Number(height), Number(format), Number(type)));
  });
  method("compressedTexImage2D", function compressedTexImage2D(target, level, internalformat, width, height, border, data, srcOffset, srcLength) {
    if (this._lost) return;
    if (typeof data === "number") { this._op("compressedTexImage2D", Number(target), Number(level), Number(internalformat), Number(width), Number(height), Number(border), data); return; }
    this._op("compressedTexImage2D", Number(target), Number(level), Number(internalformat), Number(width), Number(height), Number(border), sliceView(data, srcOffset));
  });
  method("compressedTexSubImage2D", function compressedTexSubImage2D(target, level, x, y, width, height, format, data, srcOffset) {
    if (this._lost) return;
    this._op("compressedTexSubImage2D", Number(target), Number(level), Number(x), Number(y), Number(width), Number(height), Number(format), sliceView(data, srcOffset));
  });
  method("pixelStorei", function pixelStorei(pname, param) {
    if (this._lost) return;
    pname = Number(pname);
    const u = this._unpack;
    if (pname === C.UNPACK_FLIP_Y_WEBGL) { u.flipY = !!param; return; }
    if (pname === C.UNPACK_PREMULTIPLY_ALPHA_WEBGL) { u.premultiply = !!param; return; }
    if (pname === C.UNPACK_COLORSPACE_CONVERSION_WEBGL) { u.colorspace = Number(param); return; }
    param = Number(param);
    if (pname === C.UNPACK_ALIGNMENT) u.alignment = param;
    else if (this._kind === 2) {
      if (pname === C.UNPACK_ROW_LENGTH) u.rowLength = param; else if (pname === C.UNPACK_IMAGE_HEIGHT) u.imageHeight = param;
      else if (pname === C.UNPACK_SKIP_PIXELS) u.skipPixels = param; else if (pname === C.UNPACK_SKIP_ROWS) u.skipRows = param; else if (pname === C.UNPACK_SKIP_IMAGES) u.skipImages = param;
    }
    this._op("pixelStorei", pname, param);
  });
  method("getTexParameter", function getTexParameter(target, pname) { return this._lost ? null : this._op("getTexParameter", Number(target), Number(pname)); });
  method("readPixels", function readPixels(x, y, width, height, format, type, pixels, dstOffset) {
    if (this._lost) return;
    if (typeof pixels === "number") { this._op("readPixels", Number(x), Number(y), Number(width), Number(height), Number(format), Number(type), pixels); return; }
    if (pixels === null || pixels === undefined) { this._setError(C.INVALID_VALUE); return; }
    this._op("readPixels", Number(x), Number(y), Number(width), Number(height), Number(format), Number(type), sliceView(pixels, dstOffset));
  });

  // ---- Shaders and programs ----
  method("shaderSource", function shaderSource(shader, source) {
    if (this._lost) return;
    const n = this._use(shader, Shader); if (n <= 0) { if (n === 0) throw new TypeError("parameter 1 is not of type 'WebGLShader'."); return; }
    shader._source = String(source);
    this._op("shaderSource", n, shader._source);
  });
  method("compileShader", function compileShader(shader) { if (this._lost) return; const n = this._use(shader, Shader); if (n > 0) this._op("compileShader", n); });
  method("getShaderSource", function getShaderSource(shader) { if (this._lost) return null; const n = this._use(shader, Shader); return n > 0 ? (shader._source || "") : null; });
  method("getShaderParameter", function getShaderParameter(shader, pname) {
    if (this._lost) return null;
    const n = this._use(shader, Shader); if (n <= 0) return null;
    pname = Number(pname);
    if (pname === C.DELETE_STATUS || pname === C.COMPILE_STATUS) return !!this._op("getShaderiv", n, pname);
    if (pname === C.SHADER_TYPE) return this._op("getShaderiv", n, pname);
    if (pname === 0x91B1) return true;  // COMPLETION_STATUS_KHR
    this._setError(C.INVALID_ENUM); return null;
  });
  method("getShaderInfoLog", function getShaderInfoLog(shader) { if (this._lost) return null; const n = this._use(shader, Shader); return n > 0 ? this._op("getShaderInfoLog", n) : null; });
  method("getShaderPrecisionFormat", function getShaderPrecisionFormat(shadertype, precisiontype) {
    if (this._lost) return null;
    const v = this._op("getShaderPrecisionFormat", Number(shadertype), Number(precisiontype));
    return new WebGLShaderPrecisionFormat(v[0], v[1], v[2]);
  });
  method("attachShader", function attachShader(program, shader) {
    if (this._lost) return;
    const p = this._use(program, Program), s = this._use(shader, Shader); if (p <= 0 || s <= 0) return;
    this._op("attachShader", p, s);
    (program._shaders || (program._shaders = [])).push(shader);
  });
  method("detachShader", function detachShader(program, shader) {
    if (this._lost) return;
    const p = this._use(program, Program), s = this._use(shader, Shader); if (p <= 0 || s <= 0) return;
    this._op("detachShader", p, s);
    if (program._shaders) program._shaders = program._shaders.filter((x) => x !== shader);
  });
  method("getAttachedShaders", function getAttachedShaders(program) { const p = this._use(program, Program); return p > 0 ? (program._shaders || []).slice() : null; });
  method("linkProgram", function linkProgram(program) { if (this._lost) return; const p = this._use(program, Program); if (p > 0) { this._op("linkProgram", p); this._locations.delete(program); } });
  method("useProgram", function useProgram(program) {
    if (this._lost) return;
    const p = this._use(program, Program); if (p < 0) return;
    this._op("useProgram", p);
    this._program = p ? program : null;
  });
  method("validateProgram", function validateProgram(program) { if (this._lost) return; const p = this._use(program, Program); if (p > 0) this._op("validateProgram", p); });
  method("getProgramParameter", function getProgramParameter(program, pname) {
    if (this._lost) return null;
    const p = this._use(program, Program); if (p <= 0) return null;
    pname = Number(pname);
    if (pname === 0x91B1) return true;
    const v = this._op("getProgramiv", p, pname);
    if (pname === C.DELETE_STATUS || pname === C.LINK_STATUS || pname === C.VALIDATE_STATUS) return !!v;
    return v;
  });
  method("getProgramInfoLog", function getProgramInfoLog(program) { if (this._lost) return null; const p = this._use(program, Program); return p > 0 ? this._op("getProgramInfoLog", p) : null; });
  method("bindAttribLocation", function bindAttribLocation(program, index, name) { if (this._lost) return; const p = this._use(program, Program); if (p > 0) this._op("bindAttribLocation", p, Number(index), String(name)); });
  method("getAttribLocation", function getAttribLocation(program, name) { if (this._lost) return -1; const p = this._use(program, Program); return p > 0 ? this._op("getAttribLocation", p, String(name)) : -1; });
  const activeInfo = (gl, program, index, op) => {
    if (gl._lost) return null;
    const p = gl._use(program, Program); if (p <= 0) return null;
    const text = gl._op(op, p, Number(index));
    const info = JSON.parse(text);
    return info.name === "" && info.size === 0 ? null : new WebGLActiveInfo(info.name, info.size, info.type);
  };
  method("getActiveAttrib", function getActiveAttrib(program, index) { return activeInfo(this, program, index, "getActiveAttrib"); });
  method("getActiveUniform", function getActiveUniform(program, index) { return activeInfo(this, program, index, "getActiveUniform"); });
  method("getUniformLocation", function getUniformLocation(program, name) {
    if (this._lost) return null;
    const p = this._use(program, Program); if (p <= 0) return null;
    name = String(name);
    const loc = this._op("getUniformLocation", p, name);
    if (loc < 0) return null;
    // The type of what it is: found among the active uniforms by name.
    let info = null;
    const count = this._op("getProgramiv", p, C.ACTIVE_UNIFORMS);
    const base = name.replace(/\[\d+\]$/, "");
    for (let i = 0; i < count && !info; i++) {
      const a = JSON.parse(this._op("getActiveUniform", p, i));
      if (a.name === name || a.name === base || a.name === base + "[0]") info = a;
    }
    return new WebGLUniformLocation(token, program, loc, info);
  });
  method("getUniform", function getUniform(program, location) {
    if (this._lost) return null;
    const p = this._use(program, Program); if (p <= 0) return null;
    if (!(location instanceof WebGLUniformLocation) || location._program !== program) { this._setError(C.INVALID_OPERATION); return null; }
    const t = location._info && UNIFORM_TYPES[location._info.type];
    const [kind, count] = t || [0, 1];
    const v = this._op("getUniform", p, location._loc, kind === 3 ? 1 : kind, count);
    if (kind === 3) return count === 1 ? !!v[0] : v.map(Boolean);
    if (count === 1) return v[0];
    return kind === 0 ? new Float32Array(v) : kind === 1 ? new Int32Array(v) : new Uint32Array(v);
  });

  // ---- Uniforms ----
  const locOf = (gl, location) => {
    if (location === null || location === undefined) return null;
    if (!(location instanceof WebGLUniformLocation)) throw new TypeError("parameter 1 is not of type 'WebGLUniformLocation'.");
    if (!gl._program || location._program !== gl._program) { gl._setError(C.INVALID_OPERATION); return null; }
    return location._loc;
  };
  for (const n of [1, 2, 3, 4]) {
    for (const [suffix, kindOp] of [["f", "f"], ["i", "i"]]) {
      const name = "uniform" + n + suffix, op = name;
      method(name, function (location, ...v) {
        if (this._lost) return;
        const l = locOf(this, location); if (l === null) return;
        this._op(op, l, ...v.slice(0, n).map(Number));
      });
    }
    method("uniform" + n + "fv", function (location, v, srcOffset, srcLength) {
      if (this._lost) return;
      const l = locOf(this, location); if (l === null) return;
      let a = v instanceof Float32Array ? v : Float32Array.from(v);
      if (srcOffset !== undefined) a = a.subarray(Number(srcOffset), srcLength ? Number(srcOffset) + Number(srcLength) : undefined);
      if (a.length % n || a.length === 0) { this._setError(C.INVALID_VALUE); return; }
      this._op("uniform" + n + "fv", l, a);
    });
    method("uniform" + n + "iv", function (location, v, srcOffset, srcLength) {
      if (this._lost) return;
      const l = locOf(this, location); if (l === null) return;
      let a = v instanceof Int32Array ? v : Int32Array.from(v);
      if (srcOffset !== undefined) a = a.subarray(Number(srcOffset), srcLength ? Number(srcOffset) + Number(srcLength) : undefined);
      if (a.length % n || a.length === 0) { this._setError(C.INVALID_VALUE); return; }
      this._op("uniform" + n + "iv", l, a);
    });
  }
  for (const n of [2, 3, 4]) {
    method("uniformMatrix" + n + "fv", function (location, transpose, v, srcOffset, srcLength) {
      if (this._lost) return;
      const l = locOf(this, location); if (l === null) return;
      if (transpose && this._kind === 1) { this._setError(C.INVALID_VALUE); return; }
      let a = v instanceof Float32Array ? v : Float32Array.from(v);
      if (srcOffset !== undefined) a = a.subarray(Number(srcOffset), srcLength ? Number(srcOffset) + Number(srcLength) : undefined);
      if (a.length % (n * n) || a.length === 0) { this._setError(C.INVALID_VALUE); return; }
      this._op("uniformMatrix" + n + "fv", l, transpose ? 1 : 0, a);
    });
  }
  for (const n of [1, 2, 3, 4]) {
    method("vertexAttrib" + n + "fv", function (index, v) {
      if (this._lost) return;
      const a = v instanceof Float32Array ? v : Float32Array.from(v);
      if (a.length < n) { this._setError(C.INVALID_VALUE); return; }
      this._op("vertexAttrib" + n + "fv", Number(index), a);
    });
  }

  // ---- State ----
  const FLOAT1 = new Set([C.DEPTH_CLEAR_VALUE, C.LINE_WIDTH, C.POLYGON_OFFSET_FACTOR, C.POLYGON_OFFSET_UNITS, C.SAMPLE_COVERAGE_VALUE, C.MAX_TEXTURE_LOD_BIAS]);
  const BOOL1 = new Set([C.BLEND, C.CULL_FACE, C.DEPTH_TEST, C.DEPTH_WRITEMASK, C.DITHER, C.POLYGON_OFFSET_FILL, C.SAMPLE_COVERAGE_INVERT, C.SCISSOR_TEST, C.STENCIL_TEST,
    C.SAMPLE_ALPHA_TO_COVERAGE, C.SAMPLE_COVERAGE, C.RASTERIZER_DISCARD, C.TRANSFORM_FEEDBACK_ACTIVE, C.TRANSFORM_FEEDBACK_PAUSED]);
  const INT64 = new Set([C.MAX_SERVER_WAIT_TIMEOUT, C.MAX_UNIFORM_BLOCK_SIZE, C.MAX_ELEMENT_INDEX, C.MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS, C.MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS]);
  const OBJECT_BINDINGS = {
    [C.ARRAY_BUFFER_BINDING]: C.ARRAY_BUFFER, [C.RENDERBUFFER_BINDING]: C.RENDERBUFFER, [C.FRAMEBUFFER_BINDING]: C.DRAW_FRAMEBUFFER, [C.DRAW_FRAMEBUFFER_BINDING]: C.DRAW_FRAMEBUFFER,
    [C.READ_FRAMEBUFFER_BINDING]: C.READ_FRAMEBUFFER, [C.COPY_READ_BUFFER_BINDING]: C.COPY_READ_BUFFER, [C.COPY_WRITE_BUFFER_BINDING]: C.COPY_WRITE_BUFFER,
    [C.PIXEL_PACK_BUFFER_BINDING]: C.PIXEL_PACK_BUFFER, [C.PIXEL_UNPACK_BUFFER_BINDING]: C.PIXEL_UNPACK_BUFFER, [C.TRANSFORM_FEEDBACK_BUFFER_BINDING]: C.TRANSFORM_FEEDBACK_BUFFER,
    [C.UNIFORM_BUFFER_BINDING]: C.UNIFORM_BUFFER,
  };
  method("getParameter", function getParameter(pname) {
    if (this._lost) return null;
    pname = Number(pname);
    const u = this._unpack;
    switch (pname) {
      case C.UNPACK_FLIP_Y_WEBGL: return u.flipY;
      case C.UNPACK_PREMULTIPLY_ALPHA_WEBGL: return u.premultiply;
      case C.UNPACK_COLORSPACE_CONVERSION_WEBGL: return u.colorspace;
      case C.VENDOR: return "WebKit";
      case C.RENDERER: return "WebKit WebGL";
      case C.VERSION: return this._kind === 2 ? "WebGL 2.0 (OpenGL ES 3.0 Solar)" : "WebGL 1.0 (OpenGL ES 2.0 Solar)";
      case C.SHADING_LANGUAGE_VERSION: return this._kind === 2 ? "WebGL GLSL ES 3.00 (OpenGL ES GLSL ES 3.0 Solar)" : "WebGL GLSL ES 1.0 (OpenGL ES GLSL ES 1.0 Solar)";
      case 0x9245: return this._extensions.has("WEBGL_debug_renderer_info") ? this._info.vendor : (this._setError(C.INVALID_ENUM), null);
      case 0x9246: return this._extensions.has("WEBGL_debug_renderer_info") ? this._info.renderer : (this._setError(C.INVALID_ENUM), null);
      case C.ACTIVE_TEXTURE: return C.TEXTURE0 + this._activeUnit;
      case C.CURRENT_PROGRAM: return this._program;
      case C.VERTEX_ARRAY_BINDING: return this._kind === 2 || this._extensions.has("OES_vertex_array_object") ? this._vao : (this._setError(C.INVALID_ENUM), null);
      case C.ELEMENT_ARRAY_BUFFER_BINDING: return this._vao ? this._vao._element || null : this._bound.get(C.ELEMENT_ARRAY_BUFFER) || null;
      case C.TEXTURE_BINDING_2D: return this._bound.get("t" + this._activeUnit + ":" + C.TEXTURE_2D) || null;
      case C.TEXTURE_BINDING_CUBE_MAP: return this._bound.get("t" + this._activeUnit + ":" + C.TEXTURE_CUBE_MAP) || null;
      case C.TEXTURE_BINDING_3D: return this._bound.get("t" + this._activeUnit + ":" + C.TEXTURE_3D) || null;
      case C.TEXTURE_BINDING_2D_ARRAY: return this._bound.get("t" + this._activeUnit + ":" + C.TEXTURE_2D_ARRAY) || null;
      case C.COMPRESSED_TEXTURE_FORMATS: return new Uint32Array(this._compressedFormats());
      case C.SAMPLES: return this._op("getIntegerv", C.SAMPLES, 1)[0];
    }
    if (pname in OBJECT_BINDINGS) return this._bound.get(OBJECT_BINDINGS[pname]) || null;
    if (pname === C.VIEWPORT || pname === C.SCISSOR_BOX) return new Int32Array(this._op("getIntegerv", pname, 4));
    if (pname === C.MAX_VIEWPORT_DIMS) return new Int32Array(this._op("getIntegerv", pname, 2));
    if (pname === C.ALIASED_LINE_WIDTH_RANGE || pname === C.ALIASED_POINT_SIZE_RANGE || pname === C.DEPTH_RANGE) return new Float32Array(this._op("getFloatv", pname, 2));
    if (pname === C.COLOR_CLEAR_VALUE || pname === C.BLEND_COLOR) return new Float32Array(this._op("getFloatv", pname, 4));
    if (pname === C.COLOR_WRITEMASK) return this._op("getBooleanv", pname, 4).map(Boolean);
    if (BOOL1.has(pname)) return !!this._op("getBooleanv", pname, 1)[0];
    if (FLOAT1.has(pname)) return this._op("getFloatv", pname, 1)[0];
    if (pname === 0x84FF) return this._extensions.has("EXT_texture_filter_anisotropic") ? this._op("getFloatv", 0x84FF, 1)[0] : (this._setError(C.INVALID_ENUM), null);
    if (INT64.has(pname)) return this._op("getInteger64v", pname);
    const v = this._op("getIntegerv", pname, 1)[0];
    const e = this._op("getError");
    if (e) { this._setError(e); return null; }
    return v;
  });
  method("_compressedFormats", function () {
    const formats = [];
    const has = (n) => this._native.has(n);
    if (this._extensions.has("WEBGL_compressed_texture_s3tc") && has("GL_EXT_texture_compression_s3tc")) formats.push(0x83F0, 0x83F1, 0x83F2, 0x83F3);
    if (this._extensions.has("WEBGL_compressed_texture_etc")) formats.push(0x9270, 0x9271, 0x9272, 0x9273, 0x9274, 0x9275, 0x9276, 0x9277, 0x9278, 0x9279);
    return formats;
  });
  method("isEnabled", function isEnabled(cap) { return this._lost ? false : !!this._op("isEnabled", Number(cap)); });
  method("getFramebufferAttachmentParameter", function getFramebufferAttachmentParameter(target, attachment, pname) {
    if (this._lost) return null;
    const v = this._op("getFramebufferAttachmentParameter", Number(target), Number(attachment), Number(pname));
    if (Number(pname) === C.FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) {
      const type = this._op("getFramebufferAttachmentParameter", Number(target), Number(attachment), C.FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE);
      if (type === C.NONE) return null;
      for (const b of this._bound.values()) if ((b instanceof Texture || b instanceof Renderbuffer) && b._name === v) return b;
      return null;
    }
    return v;
  });
  method("getRenderbufferParameter", function getRenderbufferParameter(target, pname) { return this._lost ? null : this._op("getRenderbufferParameter", Number(target), Number(pname)); });
  method("isContextLost", function isContextLost() { return this._lost; });
  method("getExtension", function getExtension(name) {
    if (this._lost) return null;
    name = String(name);
    const key = Object.keys(EXTENSIONS).find((k) => k.toLowerCase() === name.toLowerCase());
    if (!key || !EXTENSIONS[key].has(this)) return null;
    let ext = this._extensions.get(key);
    if (!ext) { ext = EXTENSIONS[key].make(this); this._extensions.set(key, ext); }
    return ext;
  });

  // ---- WebGL 2 ----
  class WebGL2RenderingContext extends WebGLRenderingContext {}
  const proto2 = WebGL2RenderingContext.prototype;
  const method2 = (name, fn) => define(proto2, name, fn);
  const V2 = ["bindBufferBase", "bindBufferRange"];
  method2("bindBufferBase", function bindBufferBase(target, index, buffer) { if (this._lost) return; const n = this._use(buffer, Buffer); if (n < 0) return; this._op("bindBufferBase", Number(target), Number(index), n); this._bound.set(Number(target), buffer); });
  method2("bindBufferRange", function bindBufferRange(target, index, buffer, offset, size) { if (this._lost) return; const n = this._use(buffer, Buffer); if (n < 0) return; this._op("bindBufferRange", Number(target), Number(index), n, Number(offset), Number(size)); this._bound.set(Number(target), buffer); });
  method2("copyBufferSubData", function copyBufferSubData(a, b, c, d, e) { if (!this._lost) this._op("copyBufferSubData", Number(a), Number(b), Number(c), Number(d), Number(e)); });
  method2("getBufferSubData", function getBufferSubData(target, srcByteOffset, dst, dstOffset, length) {
    if (this._lost) return;
    let view = bytesOf(dst);
    if (dstOffset !== undefined) {
      const e = view.BYTES_PER_ELEMENT || 1;
      const start = Number(dstOffset) || 0;
      view = new Uint8Array(view.buffer, view.byteOffset + start * e, (length ? Number(length) : view.length - start) * e);
    }
    this._op("getBufferSubData", Number(target), Number(srcByteOffset), view);
  });
  method2("blitFramebuffer", function blitFramebuffer(...a) { if (!this._lost) this._op("blitFramebuffer", ...a.slice(0, 10).map(Number)); });
  method2("framebufferTextureLayer", function framebufferTextureLayer(target, attachment, texture, level, layer) { if (this._lost) return; const n = this._use(texture, Texture); if (n < 0) return; this._op("framebufferTextureLayer", Number(target), Number(attachment), n, Number(level), Number(layer)); });
  method2("renderbufferStorageMultisample", function renderbufferStorageMultisample(t, s, f, w, h) { if (!this._lost) this._op("renderbufferStorageMultisample", Number(t), Number(s), Number(f), Number(w), Number(h)); });
  method2("readBuffer", function readBuffer(src) { if (!this._lost) this._op("readBuffer", Number(src)); });
  method2("invalidateFramebuffer", function invalidateFramebuffer(target, attachments) { if (!this._lost) this._op("invalidateFramebuffer", Number(target), Uint32Array.from(attachments)); });
  method2("invalidateSubFramebuffer", function invalidateSubFramebuffer(target, attachments, x, y, w, h) { if (!this._lost) this._op("invalidateSubFramebuffer", Number(target), Uint32Array.from(attachments), Number(x), Number(y), Number(w), Number(h)); });
  method2("getInternalformatParameter", function getInternalformatParameter(target, internalformat, pname) { return this._lost ? null : new Int32Array(this._op("getInternalformatParameter", Number(target), Number(internalformat))); });
  method2("texStorage2D", function (t, l, f, w, h) { if (!this._lost) this._op("texStorage2D", Number(t), Number(l), Number(f), Number(w), Number(h)); });
  method2("texStorage3D", function (t, l, f, w, h, d) { if (!this._lost) this._op("texStorage3D", Number(t), Number(l), Number(f), Number(w), Number(h), Number(d)); });
  method2("texImage3D", function texImage3D(target, level, internalformat, width, height, depth, border, format, type, pixels, srcOffset) {
    if (this._lost) return;
    if (isSource(pixels)) {
      const made = fromSource(this, pixels, Number(format), Number(type), "texImage3D");
      if (!made) return;
      withTightUnpack(this, () => this._op("texImage3D", Number(target), Number(level), Number(internalformat), Number(width), Number(height), Number(depth), Number(border), Number(format), Number(type), made.data));
      return;
    }
    if (typeof pixels === "number") { this._op("texImage3D", Number(target), Number(level), Number(internalformat), Number(width), Number(height), Number(depth), Number(border), Number(format), Number(type), pixels); return; }
    this._op("texImage3D", Number(target), Number(level), Number(internalformat), Number(width), Number(height), Number(depth), Number(border), Number(format), Number(type), pixels === null || pixels === undefined ? null : sliceView(pixels, srcOffset));
  });
  method2("texSubImage3D", function texSubImage3D(target, level, x, y, z, width, height, depth, format, type, pixels, srcOffset) {
    if (this._lost) return;
    if (isSource(pixels)) {
      const made = fromSource(this, pixels, Number(format), Number(type), "texSubImage3D");
      if (!made) return;
      withTightUnpack(this, () => this._op("texSubImage3D", Number(target), Number(level), Number(x), Number(y), Number(z), Number(width), Number(height), Number(depth), Number(format), Number(type), made.data));
      return;
    }
    if (typeof pixels === "number") { this._op("texSubImage3D", Number(target), Number(level), Number(x), Number(y), Number(z), Number(width), Number(height), Number(depth), Number(format), Number(type), pixels); return; }
    this._op("texSubImage3D", Number(target), Number(level), Number(x), Number(y), Number(z), Number(width), Number(height), Number(depth), Number(format), Number(type), sliceView(pixels, srcOffset));
  });
  method2("copyTexSubImage3D", function (t, l, x, y, z, a, b, c, d) { if (!this._lost) this._op("copyTexSubImage3D", ...[t, l, x, y, z, a, b, c, d].map(Number)); });
  method2("compressedTexImage3D", function (t, l, f, w, h, d, b, data, srcOffset) { if (!this._lost) this._op("compressedTexImage3D", Number(t), Number(l), Number(f), Number(w), Number(h), Number(d), Number(b), typeof data === "number" ? data : sliceView(data, srcOffset)); });
  method2("compressedTexSubImage3D", function (t, l, x, y, z, w, h, d, f, data, srcOffset) { if (!this._lost) this._op("compressedTexSubImage3D", Number(t), Number(l), Number(x), Number(y), Number(z), Number(w), Number(h), Number(d), Number(f), typeof data === "number" ? data : sliceView(data, srcOffset)); });
  method2("drawBuffers", function drawBuffers(buffers) { if (!this._lost) this._op("drawBuffers", Uint32Array.from(buffers)); });
  const clearBuffer = (suffix, Type) => method2("clearBuffer" + suffix, function (buffer, drawbuffer, values, srcOffset) {
    if (this._lost) return;
    let a = values instanceof Type ? values : Type.from(values);
    if (srcOffset !== undefined) a = a.subarray(Number(srcOffset));
    this._op("clearBuffer" + suffix, Number(buffer), Number(drawbuffer), a);
  });
  clearBuffer("fv", Float32Array); clearBuffer("iv", Int32Array); clearBuffer("uiv", Uint32Array);
  method2("clearBufferfi", function (buffer, drawbuffer, depth, stencil) { if (!this._lost) this._op("clearBufferfi", Number(buffer), Number(drawbuffer), Number(depth), Number(stencil)); });
  method2("vertexAttribIPointer", function (index, size, type, stride, offset) { if (!this._lost) this._op("vertexAttribIPointer", Number(index), Number(size), Number(type), Number(stride), Number(offset)); });
  method2("vertexAttribI4i", function (i, x, y, z, w) { if (!this._lost) this._op("vertexAttribI4i", ...[i, x, y, z, w].map(Number)); });
  method2("vertexAttribI4ui", function (i, x, y, z, w) { if (!this._lost) this._op("vertexAttribI4ui", ...[i, x, y, z, w].map(Number)); });
  method2("vertexAttribI4iv", function (i, v) { if (!this._lost) this._op("vertexAttribI4iv", Number(i), Int32Array.from(v)); });
  method2("vertexAttribI4uiv", function (i, v) { if (!this._lost) this._op("vertexAttribI4uiv", Number(i), Uint32Array.from(v)); });
  method2("vertexAttribDivisor", function (i, d) { if (!this._lost) this._op("vertexAttribDivisor", Number(i), Number(d)); });
  method2("drawArraysInstanced", function (m, f, c, n) { if (!this._lost) this._op("drawArraysInstanced", ...[m, f, c, n].map(Number)); });
  method2("drawElementsInstanced", function (m, c, t, o, n) { if (!this._lost) this._op("drawElementsInstanced", ...[m, c, t, o, n].map(Number)); });
  method2("drawRangeElements", function (m, s, e, c, t, o) { if (!this._lost) this._op("drawRangeElements", ...[m, s, e, c, t, o].map(Number)); });
  for (const n of [1, 2, 3, 4]) {
    method2("uniform" + n + "ui", function (location, ...v) { if (this._lost) return; const l = locOf(this, location); if (l !== null) this._op("uniform" + n + "ui", l, ...v.slice(0, n).map(Number)); });
    method2("uniform" + n + "uiv", function (location, v, srcOffset, srcLength) {
      if (this._lost) return;
      const l = locOf(this, location); if (l === null) return;
      let a = v instanceof Uint32Array ? v : Uint32Array.from(v);
      if (srcOffset !== undefined) a = a.subarray(Number(srcOffset), srcLength ? Number(srcOffset) + Number(srcLength) : undefined);
      if (a.length % n || a.length === 0) { this._setError(C.INVALID_VALUE); return; }
      this._op("uniform" + n + "uiv", l, a);
    });
  }
  for (const [name, size] of [["2x3", 6], ["3x2", 6], ["2x4", 8], ["4x2", 8], ["3x4", 12], ["4x3", 12]]) {
    method2("uniformMatrix" + name + "fv", function (location, transpose, v, srcOffset, srcLength) {
      if (this._lost) return;
      const l = locOf(this, location); if (l === null) return;
      let a = v instanceof Float32Array ? v : Float32Array.from(v);
      if (srcOffset !== undefined) a = a.subarray(Number(srcOffset), srcLength ? Number(srcOffset) + Number(srcLength) : undefined);
      if (a.length % size || a.length === 0) { this._setError(C.INVALID_VALUE); return; }
      this._op("uniformMatrix" + name + "fv", l, transpose ? 1 : 0, a);
    });
  }
  method2("getFragDataLocation", function (program, name) { const p = this._use(program, Program); return p > 0 ? this._op("getFragDataLocation", p, String(name)) : -1; });
  method2("getUniformBlockIndex", function (program, name) { const p = this._use(program, Program); return p > 0 ? this._op("getUniformBlockIndex", p, String(name)) : -1; });
  method2("uniformBlockBinding", function (program, index, binding) { const p = this._use(program, Program); if (p > 0) this._op("uniformBlockBinding", p, Number(index), Number(binding)); });
  method2("getActiveUniformBlockName", function (program, index) { const p = this._use(program, Program); return p > 0 ? this._op("getActiveUniformBlockName", p, Number(index)) : null; });
  method2("getActiveUniformBlockParameter", function (program, index, pname) {
    const p = this._use(program, Program); if (p <= 0) return null;
    pname = Number(pname);
    const v = this._op("getActiveUniformBlockParameter", p, Number(index), pname);
    if (pname === C.UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES) return new Uint32Array(v);
    if (pname === C.UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER || pname === C.UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER) return !!v;
    return v;
  });
  method2("getUniformIndices", function (program, names) { const p = this._use(program, Program); return p > 0 ? Array.from(names, String).length ? this._op("getUniformIndices", p, Array.from(names, String).join(",")) : [] : null; });
  method2("getActiveUniforms", function (program, indices, pname) {
    const p = this._use(program, Program); if (p <= 0) return null;
    pname = Number(pname);
    const v = this._op("getActiveUniforms", p, Uint32Array.from(indices), pname);
    return pname === C.UNIFORM_IS_ROW_MAJOR ? v.map(Boolean) : v;
  });
  method2("transformFeedbackVaryings", function (program, varyings, mode) { const p = this._use(program, Program); if (p > 0) this._op("transformFeedbackVaryings", p, Array.from(varyings, String).join(","), Number(mode)); });
  method2("getTransformFeedbackVarying", function (program, index) {
    const p = this._use(program, Program); if (p <= 0) return null;
    const info = JSON.parse(this._op("getTransformFeedbackVarying", p, Number(index)));
    return new WebGLActiveInfo(info.name, info.size, info.type);
  });
  method2("beginTransformFeedback", function (mode) { if (!this._lost) this._op("beginTransformFeedback", Number(mode)); });
  method2("endTransformFeedback", function () { if (!this._lost) this._op("endTransformFeedback"); });
  method2("pauseTransformFeedback", function () { if (!this._lost) this._op("pauseTransformFeedback"); });
  method2("resumeTransformFeedback", function () { if (!this._lost) this._op("resumeTransformFeedback"); });
  const objectMethods2 = [
    ["createQuery", Query, "createQuery", "deleteQuery", "isQuery"], ["createSampler", Sampler, "createSampler", "deleteSampler", "isSampler"],
    ["createTransformFeedback", TransformFeedback, "createTransformFeedback", "deleteTransformFeedback", "isTransformFeedback"],
    ["createVertexArray", VertexArray, "createVertexArray", "deleteVertexArray", "isVertexArray"],
  ];
  for (const [create, cls, createOp, deleteOp, isOp] of objectMethods2) {
    method2(create, function () { return this._lost ? null : new cls(token, this, this._op(createOp)); });
    method2("delete" + cls.name.replace("WebGL", "").replace("Object", ""), function (obj) {
      if (this._lost) return;
      const n = this._use(obj, cls, true); if (n <= 0 || obj._deleted) return;
      obj._deleted = true; this._op(deleteOp, n);
      if (obj === this._vao) this._vao = null;
    });
    method2("is" + cls.name.replace("WebGL", "").replace("Object", ""), function (obj) { return !this._lost && obj instanceof cls && obj._ctx === this && !obj._deleted && !!this._op(isOp, obj._name); });
  }
  method2("bindVertexArray", function (vao) {
    if (this._lost) return;
    const n = this._use(vao, VertexArray); if (n < 0) return;
    this._op("bindVertexArray", n);
    this._vao = vao;
  });
  method2("bindSampler", function (unit, sampler) { const n = this._use(sampler, Sampler); if (n >= 0 && !this._lost) this._op("bindSampler", Number(unit), n); });
  method2("samplerParameteri", function (s, p, v) { const n = this._use(s, Sampler); if (n > 0) this._op("samplerParameteri", n, Number(p), Number(v)); });
  method2("samplerParameterf", function (s, p, v) { const n = this._use(s, Sampler); if (n > 0) this._op("samplerParameterf", n, Number(p), Number(v)); });
  method2("getSamplerParameter", function (s, p) { const n = this._use(s, Sampler); return n > 0 ? this._op("getSamplerParameter", n, Number(p)) : null; });
  method2("bindTransformFeedback", function (target, tf) { const n = this._use(tf, TransformFeedback); if (n >= 0 && !this._lost) this._op("bindTransformFeedback", Number(target), n); });
  method2("beginQuery", function (target, q) { const n = this._use(q, Query); if (n > 0) { this._op("beginQuery", Number(target), n); q._target = Number(target); (this._activeQueries || (this._activeQueries = new Map())).set(Number(target), q); } });
  method2("endQuery", function (target) { if (!this._lost) { this._op("endQuery", Number(target)); if (this._activeQueries) this._activeQueries.delete(Number(target)); } });
  method2("getQuery", function (target, pname) { if (this._lost) return null; if (Number(pname) === C.CURRENT_QUERY) return (this._activeQueries && this._activeQueries.get(Number(target))) || null; return this._op("getQuery", Number(target), Number(pname)); });
  method2("getQueryParameter", function (q, pname) {
    const n = this._use(q, Query); if (n <= 0) return null;
    pname = Number(pname);
    const v = this._op("getQueryParameter", n, pname);
    return pname === C.QUERY_RESULT_AVAILABLE ? !!v : v;
  });
  method2("fenceSync", function (condition, flags) {
    if (this._lost) return null;
    const id = this._op("fenceSync", Number(condition), Number(flags));
    return id ? new Sync(token, this, id) : null;
  });
  method2("isSync", function (s) { return !this._lost && s instanceof Sync && s._ctx === this && !s._deleted && !!this._op("isSync", s._name); });
  method2("deleteSync", function (s) { const n = this._use(s, Sync, true); if (n > 0 && !s._deleted) { s._deleted = true; this._op("deleteSync", n); } });
  method2("clientWaitSync", function (s, flags, timeout) { const n = this._use(s, Sync); return n > 0 ? this._op("clientWaitSync", n, Number(flags), Number(timeout)) : C.WAIT_FAILED; });
  method2("waitSync", function (s, flags, timeout) { const n = this._use(s, Sync); if (n > 0) this._op("waitSync", n); });
  method2("getSyncParameter", function (s, pname) {
    const n = this._use(s, Sync); if (n <= 0) return null;
    const v = this._op("getSyncParameter", n, Number(pname));
    return v;
  });
  method2("getIndexedParameter", function (target, index) {
    if (this._lost) return null;
    target = Number(target);
    if (target === C.UNIFORM_BUFFER_BINDING || target === C.TRANSFORM_FEEDBACK_BUFFER_BINDING) return this._bound.get(target === C.UNIFORM_BUFFER_BINDING ? C.UNIFORM_BUFFER : C.TRANSFORM_FEEDBACK_BUFFER) || null;
    return this._op("getIntegeri_v", target, Number(index), 1)[0];
  });
  method2("getFragDataLocation", proto2.getFragDataLocation);

  Object.defineProperty(WebGLRenderingContext.prototype, Symbol.toStringTag, { value: "WebGLRenderingContext", configurable: true });
  Object.defineProperty(WebGL2RenderingContext.prototype, Symbol.toStringTag, { value: "WebGL2RenderingContext", configurable: true });
  // The constants are on the classes and on the instances' prototypes.
  for (const [name, value] of Object.entries(C)) {
    for (const cls of [WebGLRenderingContext, WebGL2RenderingContext]) Object.defineProperty(cls, name, { value, enumerable: true });
    Object.defineProperty(WebGLRenderingContext.prototype, name, { value, enumerable: true });
  }
  define(globalThis, "WebGLRenderingContext", WebGLRenderingContext);
  define(globalThis, "WebGL2RenderingContext", WebGL2RenderingContext);

  // ---- Extensions ----
  // name -> {has(context), make(context)}. Most are wrappers of what ES 3 already has.
  const EXTENSIONS = {};
  const extension = (name, has, make) => { EXTENSIONS[name] = { has, make }; };
  const nativeHas = (...names) => (gl) => names.some((n) => gl._native.has(n));
  const always = () => true;
  const makeExt = (name, constants, methods) => {
    const cls = { [name]: class {} }[name];
    Object.defineProperty(cls.prototype, Symbol.toStringTag, { value: name, configurable: true });
    const object = new cls();
    for (const [k, v] of Object.entries(constants || {})) Object.defineProperty(cls.prototype, k, { value: v, enumerable: true });
    for (const [k, v] of Object.entries(methods || {})) Object.defineProperty(cls.prototype, k, { value: v, enumerable: false, writable: true, configurable: true });
    return object;
  };
  extension("ANGLE_instanced_arrays", (gl) => gl._kind === 1, (gl) => makeExt("ANGLE_instanced_arrays", { VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE: 0x88FE }, {
    drawArraysInstancedANGLE(m, f, c, n) { gl._op("drawArraysInstanced", Number(m), Number(f), Number(c), Number(n)); },
    drawElementsInstancedANGLE(m, c, t, o, n) { gl._op("drawElementsInstanced", Number(m), Number(c), Number(t), Number(o), Number(n)); },
    vertexAttribDivisorANGLE(i, d) { gl._op("vertexAttribDivisor", Number(i), Number(d)); },
  }));
  extension("OES_vertex_array_object", (gl) => gl._kind === 1, (gl) => makeExt("OES_vertex_array_object", { VERTEX_ARRAY_BINDING_OES: 0x85B5 }, {
    createVertexArrayOES() { return gl._lost ? null : new VertexArray(token, gl, gl._op("createVertexArray")); },
    deleteVertexArrayOES(v) { const n = gl._use(v, VertexArray, true); if (n > 0 && !v._deleted) { v._deleted = true; gl._op("deleteVertexArray", n); if (gl._vao === v) gl._vao = null; } },
    isVertexArrayOES(v) { return v instanceof VertexArray && v._ctx === gl && !v._deleted && !!gl._op("isVertexArray", v._name); },
    bindVertexArrayOES(v) { const n = gl._use(v, VertexArray); if (n >= 0) { gl._op("bindVertexArray", n); gl._vao = v; } },
  }));
  extension("OES_element_index_uint", (gl) => gl._kind === 1, () => makeExt("OES_element_index_uint"));
  extension("OES_standard_derivatives", (gl) => gl._kind === 1, () => makeExt("OES_standard_derivatives", { FRAGMENT_SHADER_DERIVATIVE_HINT_OES: 0x8B8B }));
  extension("EXT_frag_depth", (gl) => gl._kind === 1, () => makeExt("EXT_frag_depth"));
  extension("EXT_shader_texture_lod", (gl) => gl._kind === 1, () => makeExt("EXT_shader_texture_lod"));
  extension("EXT_blend_minmax", (gl) => gl._kind === 1, () => makeExt("EXT_blend_minmax", { MIN_EXT: C.MIN, MAX_EXT: C.MAX }));
  extension("WEBGL_depth_texture", (gl) => gl._kind === 1, () => makeExt("WEBGL_depth_texture", { UNSIGNED_INT_24_8_WEBGL: C.UNSIGNED_INT_24_8 }));
  extension("OES_texture_float", (gl) => gl._kind === 1, () => makeExt("OES_texture_float"));
  extension("OES_texture_half_float", (gl) => gl._kind === 1, () => makeExt("OES_texture_half_float", { HALF_FLOAT_OES: 0x8D61 }));
  extension("OES_texture_float_linear", nativeHas("GL_OES_texture_float_linear"), () => makeExt("OES_texture_float_linear"));
  extension("OES_texture_half_float_linear", (gl) => gl._kind === 1, () => makeExt("OES_texture_half_float_linear"));
  extension("WEBGL_draw_buffers", (gl) => gl._kind === 1, (gl) => {
    const constants = { MAX_COLOR_ATTACHMENTS_WEBGL: C.MAX_COLOR_ATTACHMENTS, MAX_DRAW_BUFFERS_WEBGL: C.MAX_DRAW_BUFFERS };
    for (let i = 0; i < 16; i++) { constants["COLOR_ATTACHMENT" + i + "_WEBGL"] = C.COLOR_ATTACHMENT0 + i; constants["DRAW_BUFFER" + i + "_WEBGL"] = C.DRAW_BUFFER0 + i; }
    return makeExt("WEBGL_draw_buffers", constants, { drawBuffersWEBGL(buffers) { gl._op("drawBuffers", Uint32Array.from(buffers)); } });
  });
  extension("EXT_color_buffer_float", (gl) => gl._kind === 2 && nativeHas("GL_EXT_color_buffer_float")(gl), () => makeExt("EXT_color_buffer_float"));
  extension("EXT_color_buffer_half_float", nativeHas("GL_EXT_color_buffer_half_float", "GL_EXT_color_buffer_float"), () => makeExt("EXT_color_buffer_half_float", { RGBA16F_EXT: C.RGBA16F, RGB16F_EXT: C.RGB16F, FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT: 0x8211, UNSIGNED_NORMALIZED_EXT: 0x8C17 }));
  extension("EXT_float_blend", nativeHas("GL_EXT_float_blend"), () => makeExt("EXT_float_blend"));
  extension("EXT_texture_filter_anisotropic", nativeHas("GL_EXT_texture_filter_anisotropic"), () => makeExt("EXT_texture_filter_anisotropic", { TEXTURE_MAX_ANISOTROPY_EXT: 0x84FE, MAX_TEXTURE_MAX_ANISOTROPY_EXT: 0x84FF }));
  extension("WEBGL_debug_renderer_info", always, () => makeExt("WEBGL_debug_renderer_info", { UNMASKED_VENDOR_WEBGL: 0x9245, UNMASKED_RENDERER_WEBGL: 0x9246 }));
  extension("WEBGL_compressed_texture_s3tc", nativeHas("GL_EXT_texture_compression_s3tc", "GL_ANGLE_texture_compression_dxt5"), () => makeExt("WEBGL_compressed_texture_s3tc", { COMPRESSED_RGB_S3TC_DXT1_EXT: 0x83F0, COMPRESSED_RGBA_S3TC_DXT1_EXT: 0x83F1, COMPRESSED_RGBA_S3TC_DXT3_EXT: 0x83F2, COMPRESSED_RGBA_S3TC_DXT5_EXT: 0x83F3 }));
  extension("KHR_parallel_shader_compile", always, () => makeExt("KHR_parallel_shader_compile", { COMPLETION_STATUS_KHR: 0x91B1 }));
  extension("OES_fbo_render_mipmap", (gl) => gl._kind === 1, () => makeExt("OES_fbo_render_mipmap"));
  extension("WEBGL_lose_context", always, (gl) => makeExt("WEBGL_lose_context", {}, {
    loseContext() { gl._loseContext(); },
    restoreContext() { gl._restoreContext(); },
  }));

  // ---- Losing the context ----
  method("_loseContext", function () {
    if (this._lost) return;
    this._lost = true; this._restorable = true;
    N.GlDestroy(this._id);
    setTimeout(() => {
      const event = new WebGLContextEvent("webglcontextlost", { cancelable: true, bubbles: false });
      this._canvas.dispatchEvent(event);
      if (!event.defaultPrevented) this._restorable = false;
    }, 0);
  });
  method("_restoreContext", function () {
    if (!this._lost || !this._restorable) return;
    setTimeout(() => {
      if (!this._lost || !this._restorable) return;
      this._lost = false; this._error = 0; this._extensions.clear(); this._bound.clear(); this._vao = null; this._program = null;
      this._create();
      this._canvas.dispatchEvent(new WebGLContextEvent("webglcontextrestored"));
    }, 0);
  });

  // ---- The canvas gets its contexts ----
  internals.setWebGl((canvas, kind, attributes, width, height) => {
    const preferred = attributes && attributes.failIfMajorPerformanceCaveat;
    let context;
    try { context = new (kind === 2 ? WebGL2RenderingContext : WebGLRenderingContext)(token, canvas, kind, attributes, width, height); } catch (e) { context = null; }
    if (!context) {
      try { if (typeof canvas.dispatchEvent === "function") canvas.dispatchEvent(new WebGLContextEvent("webglcontextcreationerror", { statusMessage: "WebGL is not available." })); } catch (e) { /* the event is a courtesy */ }
      return null;
    }
    return context;
  });
})();
