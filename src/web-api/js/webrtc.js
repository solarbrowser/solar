// WebRTC (https://www.w3.org/TR/webrtc/), without a network: a connection can be made, offered and described, and gathers no candidates (the one with no candidate that
// says the gathering is done is all it ever gives), and a data channel never opens. That is what a page sees where no network interface is allowed to WebRTC, which
// pages are written for (a script that asks for the addresses of the machine gets none), and it keeps the ones that need only the interfaces from stopping at a missing
// RTCPeerConnection. What is left for the real thing: ICE, DTLS, SRTP, SCTP, and the media of tracks.
(() => {
  "use strict";
  if (typeof globalThis.RTCPeerConnection === "function") return;

  const handler = (proto, names) => {
    for (const name of names) {
      const slot = Symbol("on" + name);
      Object.defineProperty(proto, "on" + name, {
        get() { return this[slot] === undefined ? null : this[slot]; },
        set(f) {
          if (this[slot] === undefined) this.addEventListener(name, (e) => { const h = this[slot]; if (typeof h === "function") h.call(this, e); });
          this[slot] = typeof f === "function" || (f !== null && typeof f === "object") ? f : null;
        },
        enumerable: true, configurable: true,
      });
    }
  };
  const tag = (cls, name) => Object.defineProperty(cls.prototype, Symbol.toStringTag, { value: name, configurable: true });
  const illegal = () => { throw new TypeError("Illegal constructor"); };

  const SDP = (type) => "v=0\r\no=- " + Math.floor(Math.random() * 1e18) + " 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\na=extmap-allow-mixed\r\na=msid-semantic: WMS\r\n" +
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\na=ice-ufrag:" + Math.random().toString(36).slice(2, 6) + "\r\na=ice-pwd:" + Math.random().toString(36).slice(2, 26) +
    "\r\na=ice-options:trickle\r\na=fingerprint:sha-256 " + Array.from({ length: 32 }, () => (Math.random() * 256 | 0).toString(16).padStart(2, "0").toUpperCase()).join(":") +
    "\r\na=setup:" + (type === "answer" ? "active" : "actpass") + "\r\na=mid:0\r\na=sctp-port:5000\r\na=max-message-size:262144\r\n";

  class RTCSessionDescription {
    #type; #sdp;
    constructor(init) {
      if (init === undefined || init === null) throw new TypeError("Failed to construct 'RTCSessionDescription': 1 argument required, but only 0 present.");
      this.#type = init.type === undefined ? "" : String(init.type);
      this.#sdp = init.sdp === undefined ? "" : String(init.sdp);
    }
    get type() { return this.#type; }
    get sdp() { return this.#sdp; }
    toJSON() { return { type: this.#type, sdp: this.#sdp }; }
  }
  tag(RTCSessionDescription, "RTCSessionDescription");

  class RTCIceCandidate {
    #init;
    constructor(init = {}) {
      if (init.sdpMid == null && init.sdpMLineIndex == null) throw new TypeError("Failed to construct 'RTCIceCandidate': sdpMid and sdpMLineIndex are both null.");
      this.#init = { candidate: init.candidate === undefined ? "" : String(init.candidate), sdpMid: init.sdpMid ?? null, sdpMLineIndex: init.sdpMLineIndex ?? null, usernameFragment: init.usernameFragment ?? null };
    }
    get candidate() { return this.#init.candidate; }
    get sdpMid() { return this.#init.sdpMid; }
    get sdpMLineIndex() { return this.#init.sdpMLineIndex; }
    get usernameFragment() { return this.#init.usernameFragment; }
    toJSON() { return { ...this.#init }; }
  }
  tag(RTCIceCandidate, "RTCIceCandidate");

  class RTCPeerConnectionIceEvent extends Event {
    #candidate;
    constructor(type, init = {}) { super(type, init); this.#candidate = init.candidate ?? null; }
    get candidate() { return this.#candidate; }
  }
  tag(RTCPeerConnectionIceEvent, "RTCPeerConnectionIceEvent");

  class RTCDataChannel extends EventTarget {
    #label; #init; #state = "connecting";
    constructor(label, init, token) {
      if (token !== illegal) illegal();
      super();
      this.#label = label;
      this.#init = init || {};
    }
    get label() { return this.#label; }
    get readyState() { return this.#state; }
    get ordered() { return this.#init.ordered !== false; }
    get maxPacketLifeTime() { return this.#init.maxPacketLifeTime ?? null; }
    get maxRetransmits() { return this.#init.maxRetransmits ?? null; }
    get protocol() { return this.#init.protocol === undefined ? "" : String(this.#init.protocol); }
    get negotiated() { return !!this.#init.negotiated; }
    get id() { return this.#init.id ?? null; }
    get bufferedAmount() { return 0; }
    bufferedAmountLowThreshold = 0;
    binaryType = "blob";
    send() { if (this.#state !== "open") throw new DOMException("Failed to execute 'send' on 'RTCDataChannel': RTCDataChannel.readyState is not 'open'", "InvalidStateError"); }
    close() { if (this.#state === "closed") return; this.#state = "closed"; }
  }
  handler(RTCDataChannel.prototype, ["open", "message", "bufferedamountlow", "error", "closing", "close"]);
  tag(RTCDataChannel, "RTCDataChannel");

  const configError = (what) => new TypeError("Failed to construct 'RTCPeerConnection': " + what);
  class RTCPeerConnection extends EventTarget {
    #signaling = "stable"; #gathering = "new"; #ice = "new"; #connection = "new";
    #local = null; #remote = null; #config; #closed = false;
    constructor(configuration = {}) {
      super();
      if (configuration === null || (typeof configuration !== "object" && typeof configuration !== "undefined")) throw configError("The provided value is not of type 'RTCConfiguration'.");
      for (const server of configuration.iceServers || []) {
        const urls = server && server.urls;
        for (const url of Array.isArray(urls) ? urls : [urls]) {
          if (typeof url !== "string" || !/^(stun|stuns|turn|turns):/i.test(url)) throw new DOMException("Failed to construct 'RTCPeerConnection': '" + url + "' is not a valid URL.", "SyntaxError");
        }
      }
      this.#config = { iceServers: [], iceTransportPolicy: "all", bundlePolicy: "balanced", rtcpMuxPolicy: "require", iceCandidatePoolSize: 0, ...configuration };
    }
    get signalingState() { return this.#closed ? "closed" : this.#signaling; }
    get iceGatheringState() { return this.#gathering; }
    get iceConnectionState() { return this.#closed ? "closed" : this.#ice; }
    get connectionState() { return this.#closed ? "closed" : this.#connection; }
    get localDescription() { return this.#local; }
    get currentLocalDescription() { return this.#signaling === "stable" ? this.#local : null; }
    get pendingLocalDescription() { return this.#signaling === "have-local-offer" ? this.#local : null; }
    get remoteDescription() { return this.#remote; }
    get currentRemoteDescription() { return this.#signaling === "stable" ? this.#remote : null; }
    get pendingRemoteDescription() { return this.#signaling === "have-remote-offer" ? this.#remote : null; }
    get canTrickleIceCandidates() { return this.#remote ? true : null; }
    get sctp() { return null; }
    #check(name) { if (this.#closed) throw new DOMException("Failed to execute '" + name + "' on 'RTCPeerConnection': The RTCPeerConnection's signalingState is 'closed'.", "InvalidStateError"); }
    getConfiguration() { return { ...this.#config }; }
    setConfiguration(configuration = {}) { this.#check("setConfiguration"); this.#config = { ...this.#config, ...configuration }; }
    async createOffer() { this.#check("createOffer"); return new RTCSessionDescription({ type: "offer", sdp: SDP("offer") }); }
    async createAnswer() {
      this.#check("createAnswer");
      if (this.#signaling !== "have-remote-offer" && this.#signaling !== "have-local-pranswer") throw new DOMException("Failed to execute 'createAnswer' on 'RTCPeerConnection': PeerConnection cannot create an answer in a state other than have-remote-offer or have-local-pranswer.", "InvalidStateError");
      return new RTCSessionDescription({ type: "answer", sdp: SDP("answer") });
    }
    async setLocalDescription(description) {
      this.#check("setLocalDescription");
      const type = description && description.type ? description.type : (this.#signaling === "have-remote-offer" ? "answer" : "offer");
      const sdp = description && description.sdp ? description.sdp : SDP(type);
      this.#local = new RTCSessionDescription({ type, sdp });
      this.#signaling = type === "offer" ? "have-local-offer" : type === "rollback" ? "stable" : "stable";
      this.dispatchEvent(new Event("signalingstatechange"));
      if (this.#gathering === "new") {
        // Nothing to gather: it starts, and it is done, with the one candidate that is none.
        setTimeout(() => {
          if (this.#closed) return;
          this.#gathering = "gathering";
          this.dispatchEvent(new Event("icegatheringstatechange"));
          setTimeout(() => {
            if (this.#closed) return;
            this.dispatchEvent(new RTCPeerConnectionIceEvent("icecandidate", { candidate: null }));
            this.#gathering = "complete";
            this.dispatchEvent(new Event("icegatheringstatechange"));
          }, 0);
        }, 0);
      }
    }
    async setRemoteDescription(description) {
      this.#check("setRemoteDescription");
      if (!description || !description.type) throw new TypeError("Failed to execute 'setRemoteDescription' on 'RTCPeerConnection': Failed to read the 'type' property from 'RTCSessionDescriptionInit': Required member is undefined.");
      this.#remote = new RTCSessionDescription(description);
      this.#signaling = description.type === "offer" ? "have-remote-offer" : "stable";
      this.dispatchEvent(new Event("signalingstatechange"));
    }
    async addIceCandidate() { this.#check("addIceCandidate"); }
    createDataChannel(label, init) {
      this.#check("createDataChannel");
      if (arguments.length === 0) throw new TypeError("Failed to execute 'createDataChannel' on 'RTCPeerConnection': 1 argument required, but only 0 present.");
      return new RTCDataChannel(String(label), init, illegal);
    }
    addTrack() { throw new DOMException("Failed to execute 'addTrack' on 'RTCPeerConnection': tracks are not carried here.", "NotSupportedError"); }
    addTransceiver() { throw new DOMException("Failed to execute 'addTransceiver' on 'RTCPeerConnection': tracks are not carried here.", "NotSupportedError"); }
    removeTrack() {}
    getSenders() { return []; }
    getReceivers() { return []; }
    getTransceivers() { return []; }
    async getStats() { return new Map(); }
    restartIce() {}
    close() {
      if (this.#closed) return;
      this.#closed = true;
      this.#signaling = "closed";
    }
    static async generateCertificate() { return { expires: Date.now() + 2592000000, getFingerprints() { return []; } }; }
  }
  handler(RTCPeerConnection.prototype, ["negotiationneeded", "icecandidate", "icecandidateerror", "signalingstatechange", "iceconnectionstatechange", "icegatheringstatechange", "connectionstatechange", "track", "datachannel"]);
  tag(RTCPeerConnection, "RTCPeerConnection");

  for (const [name, value] of Object.entries({ RTCPeerConnection, RTCSessionDescription, RTCIceCandidate, RTCPeerConnectionIceEvent, RTCDataChannel })) {
    Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
  }
  // The names this was known by.
  Object.defineProperty(globalThis, "webkitRTCPeerConnection", { value: RTCPeerConnection, writable: true, configurable: true, enumerable: false });
})();
