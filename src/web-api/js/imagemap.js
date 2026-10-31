// Image maps (https://html.spec.whatwg.org/multipage/image-maps.html): the map's areas. (A point on an image with a usemap is on the area of its map that holds it: that is found where the
// elements at a point are (src/layout/Layout.cpp), for elementFromPoint and for the pointer of the window.)
(() => {
  "use strict";
  if (typeof HTMLAreaElement !== "function" || typeof document === "undefined") return;
  if (typeof HTMLMapElement === "function") {
    Object.defineProperty(HTMLMapElement.prototype, "areas", { get() { return this.getElementsByTagName("area"); }, enumerable: true, configurable: true });
  }
})();
