// Browser host for the Dalvik WASM VM: APK (zip) access, GLES 1.1 on WebGL, audio, input, saves.
'use strict';

/* ================= ZIP ================= */
async function inflateRaw(bytes) {
  const ds = new DecompressionStream('deflate-raw');
  const stream = new Blob([bytes]).stream().pipeThrough(ds);
  return new Uint8Array(await new Response(stream).arrayBuffer());
}
async function inflateZlib(bytes) {
  const ds = new DecompressionStream('deflate');
  const stream = new Blob([bytes]).stream().pipeThrough(ds);
  return new Uint8Array(await new Response(stream).arrayBuffer());
}
function parseZip(buf) {
  const u8 = new Uint8Array(buf), dv = new DataView(buf);
  let eocd = -1;
  for (let i = u8.length - 22; i >= Math.max(0, u8.length - 65557); i--) {
    if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error('Not a valid APK/zip file');
  const n = dv.getUint16(eocd + 10, true);
  let p = dv.getUint32(eocd + 16, true);
  const entries = new Map();
  const dec = new TextDecoder();
  for (let i = 0; i < n; i++) {
    if (dv.getUint32(p, true) !== 0x02014b50) throw new Error('Corrupt zip directory');
    const method = dv.getUint16(p + 10, true);
    const csize = dv.getUint32(p + 20, true), usize = dv.getUint32(p + 24, true);
    const nl = dv.getUint16(p + 28, true), xl = dv.getUint16(p + 30, true), cl = dv.getUint16(p + 32, true);
    const off = dv.getUint32(p + 42, true);
    const name = dec.decode(u8.subarray(p + 46, p + 46 + nl));
    entries.set(name, { name, method, csize, usize, off });
    p += 46 + nl + xl + cl;
  }
  return {
    entries,
    async read(name) {
      const e = entries.get(name);
      if (!e) return null;
      const lnl = dv.getUint16(e.off + 26, true), lxl = dv.getUint16(e.off + 28, true);
      const start = e.off + 30 + lnl + lxl;
      const raw = u8.subarray(start, start + e.csize);
      if (e.method === 0) return raw;
      if (e.method === 8) return inflateRaw(raw);
      throw new Error('Unsupported zip compression in ' + name);
    }
  };
}

/* ================= PNG (exact pixels, no premultiply) ================= */
async function decodePNG(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let p = 8, w = 0, h = 0, depth = 0, ctype = 0, interlace = 0, palette = null, trns = null;
  const idat = [];
  while (p < bytes.length) {
    const len = dv.getUint32(p); const type = String.fromCharCode(...bytes.subarray(p + 4, p + 8));
    const data = bytes.subarray(p + 8, p + 8 + len);
    if (type === 'IHDR') { w = dv.getUint32(p + 8); h = dv.getUint32(p + 12); depth = data[8]; ctype = data[9]; interlace = data[12]; }
    else if (type === 'PLTE') palette = data;
    else if (type === 'tRNS') trns = data;
    else if (type === 'IDAT') idat.push(data);
    else if (type === 'IEND') break;
    p += 12 + len;
  }
  if (depth !== 8 || interlace) return decodeViaCanvas(bytes);
  const comp = new Uint8Array(idat.reduce((a, b) => a + b.length, 0));
  let o = 0; for (const d of idat) { comp.set(d, o); o += d.length; }
  const raw = await inflateZlib(comp);
  const ch = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[ctype];
  const bpp = ch, stride = w * ch;
  const out = new Uint8Array(w * h * 4);
  let prev = new Uint8Array(stride), cur = new Uint8Array(stride);
  let rp = 0;
  for (let y = 0; y < h; y++) {
    const f = raw[rp++];
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? cur[x - bpp] : 0, b = prev[x], c = x >= bpp ? prev[x - bpp] : 0;
      let v = raw[rp++];
      switch (f) {
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) >> 1; break;
        case 4: { const pp = a + b - c, pa = Math.abs(pp - a), pb = Math.abs(pp - b), pc = Math.abs(pp - c); v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); break; }
      }
      cur[x] = v & 255;
    }
    for (let x = 0; x < w; x++) {
      const d = (y * w + x) * 4;
      if (ctype === 6) { out[d] = cur[4 * x]; out[d + 1] = cur[4 * x + 1]; out[d + 2] = cur[4 * x + 2]; out[d + 3] = cur[4 * x + 3]; }
      else if (ctype === 2) { out[d] = cur[3 * x]; out[d + 1] = cur[3 * x + 1]; out[d + 2] = cur[3 * x + 2]; out[d + 3] = 255; }
      else if (ctype === 3) { const i = cur[x]; out[d] = palette[3 * i]; out[d + 1] = palette[3 * i + 1]; out[d + 2] = palette[3 * i + 2]; out[d + 3] = trns && i < trns.length ? trns[i] : 255; }
      else if (ctype === 0) { out[d] = out[d + 1] = out[d + 2] = cur[x]; out[d + 3] = 255; }
      else if (ctype === 4) { out[d] = out[d + 1] = out[d + 2] = cur[2 * x]; out[d + 3] = cur[2 * x + 1]; }
    }
    const t = prev; prev = cur; cur = t;
  }
  return { w, h, rgba: out };
}
async function decodeViaCanvas(bytes) {
  const bmp = await createImageBitmap(new Blob([bytes], { type: 'image/png' }), { premultiplyAlpha: 'none', colorSpaceConversion: 'none' });
  const c = document.createElement('canvas'); c.width = bmp.width; c.height = bmp.height;
  const g = c.getContext('2d'); g.drawImage(bmp, 0, 0);
  return { w: bmp.width, h: bmp.height, rgba: new Uint8Array(g.getImageData(0, 0, bmp.width, bmp.height).data.buffer) };
}

/* ================= GLES 1.1 on WebGL ================= */
class GLES1 {
  constructor(canvas, mem) {
    const opts = { alpha: false, antialias: false, depth: true, stencil: false, premultipliedAlpha: false, preserveDrawingBuffer: false, powerPreference: 'high-performance' };
    let gl = canvas.getContext('webgl2', opts);
    this.webgl2 = !!gl;
    if (!gl) gl = canvas.getContext('webgl', opts) || canvas.getContext('experimental-webgl', opts);
    if (!gl) throw new Error('WebGL is not available in this browser');
    this.gl = gl; this.mem = mem;
    const vs = `attribute vec4 aPos; attribute vec4 aColor; attribute vec2 aTex;
      uniform mat4 uMVP; varying vec4 vColor; varying vec2 vTex;
      void main(){ gl_Position = uMVP * aPos; vColor = aColor; vTex = aTex; }`;
    const fs = `precision mediump float; uniform sampler2D uTex; varying vec4 vColor; varying vec2 vTex;
      void main(){ gl_FragColor = vColor * texture2D(uTex, vTex); }`;
    const sh = (t, s) => { const o = gl.createShader(t); gl.shaderSource(o, s); gl.compileShader(o); if (!gl.getShaderParameter(o, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(o)); return o; };
    const prog = gl.createProgram();
    gl.attachShader(prog, sh(gl.VERTEX_SHADER, vs)); gl.attachShader(prog, sh(gl.FRAGMENT_SHADER, fs));
    gl.bindAttribLocation(prog, 0, 'aPos'); gl.bindAttribLocation(prog, 1, 'aColor'); gl.bindAttribLocation(prog, 2, 'aTex');
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(prog));
    gl.useProgram(prog);
    this.uMVP = gl.getUniformLocation(prog, 'uMVP');
    gl.uniform1i(gl.getUniformLocation(prog, 'uTex'), 0);
    this.white = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.white);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([255, 255, 255, 255]));
    this.textures = new Map(); this.nextTex = 1; this.bound = 0;
    this.texInfo = new Map();
    this.vbo = gl.createBuffer(); this.ibo = gl.createBuffer();
    this.vboSize = 1 << 22; this.vboOff = 0;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo); gl.bufferData(gl.ARRAY_BUFFER, this.vboSize, gl.STREAM_DRAW);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.ibo);
    this.mv = [this.ident()]; this.pj = [this.ident()]; this.tx = [this.ident()]; this.mode = 0x1700;
    this.color = [1, 1, 1, 1];
    this.texEnabled = false; this.blend = false;
    this.arrays = { 0x8074: null, 0x8076: null, 0x8078: null };
    this.enabled = { 0x8074: false, 0x8076: false, 0x8078: false };
    this.err = 0; this.draws = 0;
    this.mvp = new Float32Array(16);
    this.scratchF = new Float32Array(1 << 16);
  }
  ident() { const m = new Float32Array(16); m[0] = m[5] = m[10] = m[15] = 1; return m; }
  stack() { return this.mode === 0x1701 ? this.pj : this.mode === 0x1702 ? this.tx : this.mv; }
  top() { const s = this.stack(); return s[s.length - 1]; }
  mul(a, b) { // a = a*b (column-major)
    const r = new Float32Array(16);
    for (let c = 0; c < 4; c++) for (let rr = 0; rr < 4; rr++) {
      let s = 0; for (let k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
      r[c * 4 + rr] = s;
    }
    a.set(r);
  }
  u8() { return new Uint8Array(this.mem.buffer); }
  f32(p, n) { return new Float32Array(this.mem.buffer, p, n); }
  i32(p, n) { return new Int32Array(this.mem.buffer, p, n); }

  // --- state
  BindTexture(t, id) { this.bound = id; this.gl.bindTexture(this.gl.TEXTURE_2D, this.tex(id)); }
  tex(id) {
    if (!id) return null;
    let t = this.textures.get(id);
    if (!t) { t = this.gl.createTexture(); this.textures.set(id, t); }
    return t;
  }
  BlendFunc(s, d) { this.gl.blendFunc(s, d); }
  Clear(m) { this.gl.clear(m & (0x4000 | 0x100)); }
  ClearColor(r, g, b, a) { this.gl.clearColor(r, g, b, a); }
  Color4f(r, g, b, a) { this.color = [r, g, b, a]; }
  DeleteTextures(n, p) { const ids = this.i32(p, n); for (let i = 0; i < n; i++) { const t = this.textures.get(ids[i]); if (t) { this.gl.deleteTexture(t); this.textures.delete(ids[i]); } } }
  GenTextures(n, p) { const ids = this.i32(p, n); for (let i = 0; i < n; i++) ids[i] = this.nextTex++; }
  Enable(c) { this.cap(c, true); }
  Disable(c) { this.cap(c, false); }
  cap(c, on) {
    const gl = this.gl;
    if (c === 0x0DE1) this.texEnabled = on;
    else if (c === 0x0BE2) { on ? gl.enable(gl.BLEND) : gl.disable(gl.BLEND); }
    else if (c === 0x0B44) { on ? gl.enable(gl.CULL_FACE) : gl.disable(gl.CULL_FACE); }
    else if (c === 0x0B71) { on ? gl.enable(gl.DEPTH_TEST) : gl.disable(gl.DEPTH_TEST); }
    else if (c === 0x0C11) { on ? gl.enable(gl.SCISSOR_TEST) : gl.disable(gl.SCISSOR_TEST); }
  }
  EnableClientState(a) { this.enabled[a] = true; }
  DisableClientState(a) { this.enabled[a] = false; }
  GetIntegerv(pn, p) {
    const o = this.i32(p, 4);
    if (pn === 0x8069) o[0] = this.bound;
    else if (pn === 0x0D33) o[0] = 2048;
    else if (pn === 0x0BA2) { const v = this.gl.getParameter(this.gl.VIEWPORT); o.set(v); }
    else o[0] = 0;
  }
  GetError() { const e = this.err; this.err = 0; return e; }
  LoadIdentity() { this.top().set(this.ident()); }
  LoadMatrixf(p) { this.top().set(this.f32(p, 16)); }
  MultMatrixf(p) { this.mul(this.top(), this.f32(p, 16).slice()); }
  MatrixMode(m) { this.mode = m; }
  Orthof(l, r, b, t, n, f) {
    const m = new Float32Array(16);
    m[0] = 2 / (r - l); m[5] = 2 / (t - b); m[10] = -2 / (f - n);
    m[12] = -(r + l) / (r - l); m[13] = -(t + b) / (t - b); m[14] = -(f + n) / (f - n); m[15] = 1;
    this.mul(this.top(), m);
  }
  PushMatrix() { const s = this.stack(); s.push(s[s.length - 1].slice()); }
  PopMatrix() { const s = this.stack(); if (s.length > 1) s.pop(); }
  Rotatef(a, x, y, z) {
    const l = Math.hypot(x, y, z); if (!l) return; x /= l; y /= l; z /= l;
    const r = a * Math.PI / 180, c = Math.cos(r), s = Math.sin(r), C = 1 - c;
    const m = new Float32Array([x * x * C + c, y * x * C + z * s, x * z * C - y * s, 0, x * y * C - z * s, y * y * C + c, y * z * C + x * s, 0, x * z * C + y * s, y * z * C - x * s, z * z * C + c, 0, 0, 0, 0, 1]);
    this.mul(this.top(), m);
  }
  Scalef(x, y, z) { const m = this.ident(); m[0] = x; m[5] = y; m[10] = z; this.mul(this.top(), m); }
  Translatef(x, y, z) { const m = this.ident(); m[12] = x; m[13] = y; m[14] = z; this.mul(this.top(), m); }
  ShadeModel() { }
  TexParameteri(t, pn, v) {
    const gl = this.gl;
    if (pn === 0x2801) { // MIN_FILTER: no mipmaps unless uploaded
      const info = this.texInfo.get(this.bound);
      if (v >= 0x2700 && v <= 0x2703 && !(info && info.mips)) v = (v === 0x2700 || v === 0x2702) ? gl.NEAREST : gl.LINEAR;
    }
    if (pn === 0x8191) return; // GENERATE_MIPMAP
    if ((pn === 0x2802 || pn === 0x2803) && !this.webgl2) {
      const info = this.texInfo.get(this.bound);
      if (info && !info.pot) v = gl.CLAMP_TO_EDGE;
    }
    gl.texParameteri(gl.TEXTURE_2D, pn, v);
  }
  texBytes(w, h, fmt, type) {
    let bpp;
    if (type === 0x1401) bpp = { 0x1906: 1, 0x1909: 1, 0x190A: 2, 0x1907: 3, 0x1908: 4 }[fmt] || 4;
    else bpp = 2;
    const row = w * bpp, aligned = (row + 3) & ~3;
    return { size: aligned * (h - 1) + row, bpp };
  }
  texData(p, w, h, fmt, type) {
    if (!p) return null;
    const { size } = this.texBytes(w, h, fmt, type);
    if (type === 0x1401) return new Uint8Array(this.mem.buffer, p, size);
    return new Uint16Array(this.mem.buffer, p, size >> 1);
  }
  TexImage2D(t, level, ifmt, w, h, border, fmt, type, p) {
    const gl = this.gl;
    const pot = (w & (w - 1)) === 0 && (h & (h - 1)) === 0;
    const info = this.texInfo.get(this.bound) || {};
    info.pot = pot; if (level > 0) info.mips = true;
    this.texInfo.set(this.bound, info);
    gl.texImage2D(gl.TEXTURE_2D, level, fmt, w, h, 0, fmt, type, this.texData(p, w, h, fmt, type));
    if (!this.webgl2 && !pot) { gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE); }
  }
  TexSubImage2D(t, level, x, y, w, h, fmt, type, p) {
    if (!p) return;
    this.gl.texSubImage2D(this.gl.TEXTURE_2D, level, x, y, w, h, fmt, type, this.texData(p, w, h, fmt, type));
  }
  texImageRGBA(level, bmp) {
    const gl = this.gl;
    // Android uploads premultiplied bitmap data
    const src = bmp.rgba, pm = new Uint8Array(src.length);
    for (let i = 0; i < src.length; i += 4) {
      const a = src[i + 3];
      pm[i] = (src[i] * a + 127) / 255; pm[i + 1] = (src[i + 1] * a + 127) / 255; pm[i + 2] = (src[i + 2] * a + 127) / 255; pm[i + 3] = a;
    }
    const pot = (bmp.w & (bmp.w - 1)) === 0 && (bmp.h & (bmp.h - 1)) === 0;
    this.texInfo.set(this.bound, { pot });
    gl.texImage2D(gl.TEXTURE_2D, level, gl.RGBA, bmp.w, bmp.h, 0, gl.RGBA, gl.UNSIGNED_BYTE, pm);
  }
  VertexPointer(s, t, st, p) { this.arrays[0x8074] = { s, t, st, p }; }
  ColorPointer(s, t, st, p) { this.arrays[0x8076] = { s, t, st, p }; }
  TexCoordPointer(s, t, st, p) { this.arrays[0x8078] = { s, t, st, p }; }
  Viewport(x, y, w, h) { this.gl.viewport(x, y, w, h); }

  // --- drawing
  typeSize(t) { return t === 0x1400 || t === 0x1401 ? 1 : t === 0x1402 || t === 0x1403 ? 2 : 4; }
  frameStart() {
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, this.vboSize, gl.STREAM_DRAW);
    this.vboOff = 0; this.draws = 0;
  }
  upload(arr, first, count, loc, norm) {
    const gl = this.gl;
    const ts = this.typeSize(arr.t);
    const stride = arr.st || arr.s * ts;
    const start = arr.p + first * stride;
    const bytes = (count - 1) * stride + arr.s * ts;
    let type = arr.t, data = new Uint8Array(this.mem.buffer, start, bytes), outStride = stride;
    if (type === 0x140C) { // GL_FIXED -> float
      const f = new Float32Array(count * arr.s);
      const dv = new DataView(this.mem.buffer);
      for (let i = 0; i < count; i++) for (let k = 0; k < arr.s; k++) f[i * arr.s + k] = dv.getInt32(start + i * stride + 4 * k, true) / 65536;
      data = new Uint8Array(f.buffer); type = 0x1406; outStride = arr.s * 4;
    }
    const len = data.length;
    if (this.vboOff + len + 16 > this.vboSize) {
      if (len + 16 > this.vboSize) { this.vboSize = Math.max(this.vboSize * 2, len * 2); }
      gl.bufferData(gl.ARRAY_BUFFER, this.vboSize, gl.STREAM_DRAW); this.vboOff = 0;
    }
    gl.bufferSubData(gl.ARRAY_BUFFER, this.vboOff, data);
    gl.enableVertexAttribArray(loc);
    // offset is relative to vertex `first`; shift so index 0 maps to vertex 0 of this window
    gl.vertexAttribPointer(loc, arr.s, type, norm, outStride, this.vboOff);
    this.vboOff = (this.vboOff + len + 15) & ~15;
  }
  prepare(first, count) {
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    const v = this.arrays[0x8074];
    if (!this.enabled[0x8074] || !v) return false;
    this.upload(v, first, count, 0, false);
    const c = this.arrays[0x8076];
    if (this.enabled[0x8076] && c) this.upload(c, first, count, 1, c.t === 0x1401);
    else { gl.disableVertexAttribArray(1); gl.vertexAttrib4f(1, this.color[0], this.color[1], this.color[2], this.color[3]); }
    const t = this.arrays[0x8078];
    const useTex = this.texEnabled && this.bound && this.textures.has(this.bound);
    if (useTex && this.enabled[0x8078] && t) this.upload(t, first, count, 2, false);
    else { gl.disableVertexAttribArray(2); gl.vertexAttrib2f(2, 0, 0); }
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, useTex ? this.textures.get(this.bound) : this.white);
    // MVP = P * MV
    const P = this.pj[this.pj.length - 1], M = this.mv[this.mv.length - 1], r = this.mvp;
    for (let col = 0; col < 4; col++) for (let row = 0; row < 4; row++) {
      let s = 0; for (let k = 0; k < 4; k++) s += P[k * 4 + row] * M[col * 4 + k];
      r[col * 4 + row] = s;
    }
    gl.uniformMatrix4fv(this.uMVP, false, r);
    return true;
  }
  DrawArrays(mode, first, count) {
    if (count <= 0 || !this.prepare(first, count)) return;
    this.gl.drawArrays(mode, 0, count);
    this.gl.bindTexture(this.gl.TEXTURE_2D, this.tex(this.bound));
    this.draws++;
  }
  DrawElements(mode, count, type, p) {
    if (count <= 0 || !p) return;
    const idx = type === 0x1401 ? new Uint8Array(this.mem.buffer, p, count) : new Uint16Array(this.mem.buffer, p, count);
    let max = 0, min = 65535;
    for (let i = 0; i < count; i++) { const v = idx[i]; if (v > max) max = v; if (v < min) min = v; }
    if (!this.prepare(0, max + 1)) return;
    const gl = this.gl;
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, idx, gl.STREAM_DRAW);
    gl.drawElements(mode, count, type === 0x1401 ? gl.UNSIGNED_BYTE : gl.UNSIGNED_SHORT, 0);
    this.gl.bindTexture(this.gl.TEXTURE_2D, this.tex(this.bound));
    this.draws++;
  }
}

/* ================= Audio ================= */
class Audio {
  constructor() {
    this.ctx = null; this.sounds = new Map(); this.streams = new Map(); this.nextSound = 1; this.nextStream = 1;
    this.music = new Map(); this.nextMusic = 1; this.muted = false; this.master = 1;
    let can = '';
    try { can = new window.Audio().canPlayType('audio/ogg; codecs="vorbis"'); } catch (e) { }
    this.canOgg = !!can;
    this.decoder = null; // set by host: bytes -> AudioBuffer (wasm stb_vorbis)
    this.decodeQueue = [];
  }
  softDecode(bytes) {
    const ctx = this.ensure();
    if (!ctx || !this.decoder) return null;
    try { return this.decoder(ctx, bytes); } catch (e) { return null; }
  }
  pumpQueue() {
    if (this.pumping) return;
    this.pumping = true;
    const step = () => {
      const job = this.decodeQueue.shift();
      if (!job) { this.pumping = false; return; }
      job();
      setTimeout(step, 0);
    };
    setTimeout(step, 0);
  }
  ensure() {
    if (!this.ctx) {
      const AC = window.AudioContext || window.webkitAudioContext;
      if (!AC) return null;
      this.ctx = new AC();
      this.gain = this.ctx.createGain(); this.gain.connect(this.ctx.destination);
    }
    if (this.ctx.state === 'suspended') this.ctx.resume().catch(() => {});
    return this.ctx;
  }
  setMuted(m) {
    this.muted = m;
    if (this.gain) this.gain.gain.value = m ? 0 : 1;
    for (const a of this.music.values()) if (a.el) a.el.muted = m;
  }
  load(bytes) {
    const id = this.nextSound++;
    const rec = { buf: null, pending: [] };
    this.sounds.set(id, rec);
    const ctx = this.ensure();
    const done = (b) => { if (!b) return; rec.buf = b; for (const f of rec.pending) f(); rec.pending = []; };
    const soft = () => { this.decodeQueue.push(() => done(this.softDecode(bytes))); this.pumpQueue(); };
    if (ctx && bytes) {
      if (this.canOgg) ctx.decodeAudioData(bytes.slice().buffer).then(done).catch(soft);
      else soft();
    }
    return id;
  }
  play(id, lv, rv, loop, rate) {
    const rec = this.sounds.get(id);
    if (!rec) return 0;
    const sid = this.nextStream++;
    const go = () => {
      const ctx = this.ctx; if (!ctx || !rec.buf) return;
      const src = ctx.createBufferSource(); src.buffer = rec.buf;
      src.loop = loop !== 0; src.playbackRate.value = rate || 1;
      const g = ctx.createGain(); g.gain.value = Math.max(lv, rv);
      src.connect(g); g.connect(this.gain);
      src.start();
      src.onended = () => this.streams.delete(sid);
      this.streams.set(sid, src);
    };
    if (rec.buf) go();
    else { const t0 = performance.now(); rec.pending.push(() => { if (performance.now() - t0 < 400) go(); }); }
    return sid;
  }
  stop(sid) { const s = this.streams.get(sid); if (s) { try { s.stop(); } catch (e) { } this.streams.delete(sid); } }
  musCreate(bytes) {
    const id = this.nextMusic++;
    if (this.canOgg) {
      const url = URL.createObjectURL(new Blob([bytes], { type: 'audio/ogg' }));
      const el = new window.Audio(); el.src = url; el.preload = 'auto'; el.muted = this.muted;
      this.music.set(id, { el, url, vol: 1 });
    } else {
      this.music.set(id, { web: true, bytes, buf: null, loop: false, vol: 1, src: null, gain: null, offset: 0, startedAt: 0, playing: false });
    }
    return id;
  }
  webMusicStart(m) {
    const ctx = this.ensure(); if (!ctx) return;
    if (!m.buf) { m.buf = this.softDecode(m.bytes); if (!m.buf) return; }
    if (m.src) { try { m.src.stop(); } catch (e) { } }
    const src = ctx.createBufferSource(); src.buffer = m.buf; src.loop = m.loop;
    const g = ctx.createGain(); g.gain.value = m.vol;
    src.connect(g); g.connect(this.gain);
    const off = m.offset % m.buf.duration;
    src.start(0, off);
    m.src = src; m.gain = g; m.startedAt = ctx.currentTime - off; m.playing = true;
    src.onended = () => { if (m.src === src) { m.playing = false; m.src = null; m.offset = 0; } };
  }
  webMusicStop(m, keepPos) {
    if (m.src) { const s = m.src; m.src = null; try { s.stop(); } catch (e) { } }
    m.offset = keepPos && this.ctx ? this.ctx.currentTime - m.startedAt : 0;
    m.playing = false;
  }
  musCmd(id, cmd, a, b) {
    const m = this.music.get(id); if (!m) return;
    if (m.web) {
      switch (cmd) {
        case 1: this.webMusicStart(m); break;
        case 2: this.webMusicStop(m, false); break;
        case 3: this.webMusicStop(m, true); break;
        case 4: m.loop = a !== 0; if (m.src) m.src.loop = m.loop; break;
        case 5: m.vol = Math.max(0, Math.min(1, Math.max(a, b))); if (m.gain) m.gain.gain.value = m.vol; break;
        case 9: this.webMusicStop(m, false); this.music.delete(id); break;
      }
      return;
    }
    const el = m.el;
    switch (cmd) {
      case 1: el.play().catch(() => { this.blocked = m; }); break;
      case 2: el.pause(); el.currentTime = 0; break;
      case 3: el.pause(); break;
      case 4: el.loop = a !== 0; break;
      case 5: el.volume = Math.max(0, Math.min(1, Math.max(a, b))); break;
      case 9: el.pause(); URL.revokeObjectURL(m.url); this.music.delete(id); break;
    }
  }
  pauseAll() { if (this.ctx) this.ctx.suspend().catch(() => {}); for (const m of this.music.values()) { if (m.el && !m.el.paused) { m.wasPlaying = true; m.el.pause(); } } }
  resumeAll() { if (this.ctx) this.ctx.resume().catch(() => {}); for (const m of this.music.values()) { if (m.el && m.wasPlaying) { m.wasPlaying = false; m.el.play().catch(() => {}); } } if (this.blocked) { this.blocked.el.play().catch(() => {}); this.blocked = null; } }
}

/* ================= Host ================= */
class PGHost {
  constructor(canvas, ui) {
    this.canvas = canvas; this.ui = ui || {};
    this.audio = new Audio();
    this.files = new Map();
    this.bitmaps = new Map(); this.nextBmp = 1;
    this.assets = new Map();
    this.logLines = [];
    this.running = false; this.exited = false;
    this.savePrefix = 'pgweb:file:';
  }
  log(s) {
    this.logLines.push(s); if (this.logLines.length > 400) this.logLines.shift();
    if (this.ui.onLog) this.ui.onLog(s);
  }
  cstr(p, len) {
    const u8 = new Uint8Array(this.mem.buffer);
    if (len === undefined) { len = 0; while (u8[p + len]) len++; }
    return new TextDecoder().decode(u8.slice(p, p + len));
  }
  loadSaves() {
    try {
      for (let i = 0; i < localStorage.length; i++) {
        const k = localStorage.key(i);
        if (k && k.startsWith(this.savePrefix)) {
          const bin = atob(localStorage.getItem(k));
          const u = new Uint8Array(bin.length); for (let j = 0; j < bin.length; j++) u[j] = bin.charCodeAt(j);
          this.files.set(k.slice(this.savePrefix.length), u);
        }
      }
    } catch (e) { }
  }
  persist(name, data) {
    this.files.set(name, data);
    try {
      let s = ''; for (let i = 0; i < data.length; i += 8192) s += String.fromCharCode.apply(null, data.subarray(i, i + 8192));
      localStorage.setItem(this.savePrefix + name, btoa(s));
    } catch (e) { }
  }

  async loadApk(buf, progress) {
    const zip = parseZip(buf);
    this.zip = zip;
    const names = [...zip.entries.keys()];
    const wanted = names.filter(n => n === 'classes.dex' || n.startsWith('assets/') || (n.startsWith('res/') && !n.endsWith('.xml')));
    let done = 0, total = wanted.reduce((a, n) => a + zip.entries.get(n).usize, 0) || 1, bytes = 0;
    const out = this.assets;
    const queue = wanted.slice();
    const worker = async () => {
      while (queue.length) {
        const n = queue.shift();
        const d = await zip.read(n);
        out.set(n, d);
        bytes += zip.entries.get(n).usize; done++;
        if (progress) progress(bytes / total, n);
      }
    };
    await Promise.all([worker(), worker(), worker(), worker()]);
    if (!out.get('classes.dex')) throw new Error('This APK has no classes.dex');
    // pre-decode drawables (bitmaps must be synchronous inside the VM)
    this.drawables = new Map();
    for (const n of wanted) {
      if (n.startsWith('res/drawable') && n.endsWith('.png')) {
        try { this.drawables.set(n, await decodePNG(out.get(n))); } catch (e) { this.log('png decode failed ' + n); }
      }
    }
    // lowercase index for case-insensitive lookups (Android asset names are case sensitive, keep exact first)
    this.lower = new Map();
    for (const n of out.keys()) this.lower.set(n.toLowerCase(), n);
  }

  async start(wasmBytes) {
    this.loadSaves();
    const self = this;
    const imports = {
      wasi_snapshot_preview1: {
        fd_write: (fd, iovs, n, nw) => {
          const dv = new DataView(self.mem.buffer); let tot = 0, s = '';
          for (let i = 0; i < n; i++) { const p = dv.getUint32(iovs + 8 * i, true), l = dv.getUint32(iovs + 8 * i + 4, true); s += self.cstr(p, l); tot += l; }
          dv.setUint32(nw, tot, true); if (s.trim()) self.log(s.trimEnd()); return 0;
        },
        fd_close: () => 0, fd_seek: () => 0,
      },
      host: this.hostImports(),
    };
    const { instance } = await WebAssembly.instantiate(wasmBytes, imports);
    this.inst = instance; this.ex = instance.exports; this.mem = this.ex.memory;
    this.gles = new GLES1(this.canvas, this.mem);
    if (this.audio.gain) this.audio.gain.gain.value = this.audio.muted ? 0 : 1;
    this.audio.decoder = (ctx, bytes) => {
      const ex = this.ex;
      const p = ex.vm_alloc(bytes.length);
      new Uint8Array(this.mem.buffer).set(bytes, p);
      const n = ex.vorbis_decode(p, bytes.length);
      ex.vm_free(p);
      if (n <= 0) { ex.vorbis_free(); return null; }
      const ch = ex.vorbis_channels(), rate = ex.vorbis_rate();
      const pcm = new Int16Array(this.mem.buffer, ex.vorbis_samples(), n * ch);
      const buf = ctx.createBuffer(ch, n, rate);
      for (let c = 0; c < ch; c++) {
        const d = buf.getChannelData(c);
        for (let i = 0; i < n; i++) d[i] = pcm[i * ch + c] / 32768;
      }
      ex.vorbis_free();
      return buf;
    };
    if (this.ex._initialize) this.ex._initialize();
    const dex = this.assets.get('classes.dex');
    const p = this.ex.vm_alloc(dex.length);
    new Uint8Array(this.mem.buffer).set(dex, p);
    this.gles.frameStart();
    if (!this.ex.vm_start(p, dex.length)) throw new Error('The VM could not start this app');
    this.resize(true);
    this.installInput();
    this.running = true;
    this.lastFrame = 0;
    const loop = (ts) => {
      if (!this.running) return;
      this.raf = requestAnimationFrame(loop);
      if (this.paused) return;
      this.resize(false);
      this.gles.frameStart();
      const t0 = performance.now();
      try { this.ex.vm_frame(); }
      catch (e) {
        this.running = false;
        if (String(e).indexOf('__vm_exit__') >= 0) return;
        this.log('VM crashed: ' + e); if (this.ui.onCrash) this.ui.onCrash(e);
        return;
      }
      this.frameMs = performance.now() - t0;
      this.frames = (this.frames || 0) + 1;
      if (this.ui.onFrame) this.ui.onFrame(this);
    };
    this.raf = requestAnimationFrame(loop);
  }

  resize(force) {
    const c = this.canvas;
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const r = c.getBoundingClientRect();
    let w = Math.round(r.width * dpr), h = Math.round(r.height * dpr);
    // cap backing size (game is designed for ~800x480)
    const cap = 1600;
    if (w > cap) { h = Math.round(h * cap / w); w = cap; }
    if (w < 2 || h < 2) return;
    if (force || w !== c.width || h !== c.height) {
      c.width = w; c.height = h;
      this.ex.vm_surface(w, h);
    }
  }

  installInput() {
    const c = this.canvas;
    const active = []; // pointerId per slot
    const pos = new Map();
    const buf = this.ex.vm_alloc(64);
    const send = (action, slot) => {
      const ids = [];
      for (let i = 0; i < active.length; i++) if (active[i] !== undefined) ids.push(i);
      const f = new Float32Array(this.mem.buffer, buf, 16);
      let k = 0, idx = 0;
      for (const s of ids) { const p = pos.get(active[s]); f[k++] = s; f[k++] = p.x; f[k++] = p.y; if (s === slot) idx = ids.indexOf(s); }
      let a = action;
      if (action === 5 || action === 6) a = action | (idx << 8);
      this.ex.vm_touch(a, ids.length, buf);
    };
    const toLocal = (e) => {
      const r = c.getBoundingClientRect();
      return { x: (e.clientX - r.left) * c.width / r.width, y: (e.clientY - r.top) * c.height / r.height };
    };
    c.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      this.audio.ensure(); this.audio.resumeAll();
      if (this.ui.onFirstTouch) { const f = this.ui.onFirstTouch; this.ui.onFirstTouch = null; f(); }
      try { c.setPointerCapture(e.pointerId); } catch (_) { }
      let slot = active.indexOf(undefined); if (slot < 0) slot = active.length;
      if (slot > 1) return;
      active[slot] = e.pointerId; pos.set(e.pointerId, toLocal(e));
      const count = active.filter(x => x !== undefined).length;
      send(count === 1 ? 0 : 5, slot);
    });
    c.addEventListener('pointermove', (e) => {
      if (!pos.has(e.pointerId)) return;
      e.preventDefault();
      pos.set(e.pointerId, toLocal(e));
      send(2, -1);
    });
    const up = (e) => {
      const slot = active.indexOf(e.pointerId);
      if (slot < 0) return;
      pos.set(e.pointerId, toLocal(e));
      const count = active.filter(x => x !== undefined).length;
      send(count === 1 ? (e.type === 'pointercancel' ? 3 : 1) : 6, slot);
      active[slot] = undefined; pos.delete(e.pointerId);
      while (active.length && active[active.length - 1] === undefined) active.pop();
    };
    c.addEventListener('pointerup', up);
    c.addEventListener('pointercancel', up);
    c.addEventListener('contextmenu', (e) => e.preventDefault());
    // accelerometer
    const abuf = () => { };
    this.sendAccel = (x, y, z) => { if (this.ex) this.ex.vm_accel(x, y, z); };
    // Motion sensor -> the game's legacy SensorListener convention: gravity direction in *screen*
    // coordinates, so a phone held sideways and upright reads (0, -9.8, 0).
    const ios = /iPad|iPhone|iPod/.test(navigator.userAgent) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
    let sign = ios ? 1 : -1, calib = 0, calibSum = 0;
    const screenAngle = () => {
      let a = screen.orientation && typeof screen.orientation.angle === 'number' ? screen.orientation.angle : (window.orientation || 0);
      return ((a % 360) + 360) % 360;
    };
    window.addEventListener('devicemotion', (e) => {
      const g = e.accelerationIncludingGravity; if (!g || g.x === null || g.x === undefined) return;
      // device axes (x right, y toward the top of the phone) -> screen axes. The screen angle is how far
      // the device is turned counter-clockwise: at 90 deg the phone's top points left, so
      // screen-right = -device.y and screen-up = +device.x.
      const t = screenAngle() * Math.PI / 180, c = Math.cos(t), s = Math.sin(t);
      const sx = g.x * c - g.y * s;
      const sy = g.x * s + g.y * c;
      // Browsers disagree on the sign (reaction force vs gravity). Calibrate once from how the phone
      // is actually held: whichever sign makes "screen up" point away from the ground.
      if (calib < 20 && Math.abs(sy) > 6 && Math.abs(sy) > Math.abs(sx)) {
        calibSum += sy; calib++;
        if (calib === 20) sign = calibSum > 0 ? -1 : 1;
      }
      this.motion = { x: sign * sx, y: sign * sy, z: sign * g.z, t: performance.now() };
    });
    void abuf;
  }

  hostImports() {
    const self = this;
    const G = () => self.gles;
    const str = (p, l) => self.cstr(p, l);
    const findAsset = (path) => {
      let d = self.assets.get(path);
      if (!d) { const k = self.lower.get(path.toLowerCase()); if (k) d = self.assets.get(k); }
      return d;
    };
    return {
      log: (p, l) => self.log(str(p, l)),
      now: () => Date.now(),
      exit: (code) => { self.exited = true; self.running = false; if (self.ui.onExit) self.ui.onExit(code); throw new Error('__vm_exit__'); },
      asset_size: (p, l) => { const d = self.assets.get(str(p, l)); return d ? d.length : -1; },
      asset_read: (p, l, dst) => { const d = self.assets.get(str(p, l)); if (d) new Uint8Array(self.mem.buffer).set(d, dst); },
      res_path: (tp, np, out, cap) => {
        const type = str(tp), name = str(np);
        for (const k of self.assets.keys()) {
          if (!k.startsWith('res/' + type)) continue;
          const base = k.slice(k.lastIndexOf('/') + 1);
          const dot = base.lastIndexOf('.');
          if ((dot >= 0 ? base.slice(0, dot) : base) === name) {
            const enc = new TextEncoder().encode(k);
            new Uint8Array(self.mem.buffer).set(enc.subarray(0, cap), out);
            return Math.min(enc.length, cap);
          }
        }
        return -1;
      },
      file_size: (p, l) => { const d = self.files.get(str(p, l)); return d ? d.length : -1; },
      file_read: (p, l, dst) => { const d = self.files.get(str(p, l)); if (d) new Uint8Array(self.mem.buffer).set(d, dst); },
      file_write: (p, l, data, dl) => { self.persist(str(p, l), new Uint8Array(self.mem.buffer).slice(data, data + dl)); },
      bitmap_load: (p, l) => {
        const b = self.drawables.get(str(p, l));
        if (!b) return 0;
        const id = self.nextBmp++; self.bitmaps.set(id, b); return id;
      },
      bitmap_width: (h) => self.bitmaps.get(h).w,
      bitmap_height: (h) => self.bitmaps.get(h).h,
      bitmap_pixels: (h, dst, off, stride, x, y, w, hh) => {
        const b = self.bitmaps.get(h); const out = new Int32Array(self.mem.buffer, dst);
        const s = b.rgba;
        for (let j = 0; j < hh; j++) for (let i = 0; i < w; i++) {
          const q = ((y + j) * b.w + x + i) * 4;
          out[off + j * stride + i] = (s[q + 3] << 24) | (s[q] << 16) | (s[q + 1] << 8) | s[q + 2];
        }
      },
      bitmap_free: (h) => { self.bitmaps.delete(h); },
      bitmap_teximage: (t, level, h) => { const b = self.bitmaps.get(h); if (b) G().texImageRGBA(level, b); },
      snd_load: (p, l) => self.audio.load(findAsset(str(p, l))),
      snd_play: (id, lv, rv, loop, rate) => self.audio.play(id, lv, rv, loop, rate),
      snd_stop: (sid) => self.audio.stop(sid),
      mus_create: (p, l) => { const d = findAsset(str(p, l)); return d ? self.audio.musCreate(d) : 0; },
      mus_cmd: (h, c, a, b) => self.audio.musCmd(h, c, a, b),
      vibrate: (ms) => { try { if (navigator.vibrate) navigator.vibrate(ms); } catch (e) { } },
      dialog: (id, tp, mp, pp, np, xp, input) => {
        const d = { id, title: str(tp), msg: str(mp), pos: str(pp), neg: str(np), text: str(xp), input: !!input };
        if (self.ui.onDialog) self.ui.onDialog(d, (which, text) => self.dialogResult(id, which, text));
        else self.dialogResult(id, -2, null);
      },
      glBindTexture: (a, b) => G().BindTexture(a, b),
      glBlendFunc: (a, b) => G().BlendFunc(a, b),
      glClear: (m) => G().Clear(m),
      glClearColor: (r, g, b, a) => G().ClearColor(r, g, b, a),
      glColor4f: (r, g, b, a) => G().Color4f(r, g, b, a),
      glDeleteTextures: (n, p) => G().DeleteTextures(n, p),
      glGenTextures: (n, p) => G().GenTextures(n, p),
      glEnable: (c) => G().Enable(c),
      glDisable: (c) => G().Disable(c),
      glEnableClientState: (a) => G().EnableClientState(a),
      glDisableClientState: (a) => G().DisableClientState(a),
      glDrawArrays: (m, f, c) => G().DrawArrays(m, f, c),
      glDrawElements: (m, c, t, p) => G().DrawElements(m, c, t, p),
      glGetIntegerv: (pn, p) => G().GetIntegerv(pn, p),
      glGetError: () => G().GetError(),
      glLoadIdentity: () => G().LoadIdentity(),
      glLoadMatrixf: (p) => G().LoadMatrixf(p),
      glMultMatrixf: (p) => G().MultMatrixf(p),
      glMatrixMode: (m) => G().MatrixMode(m),
      glOrthof: (l, r, b, t, n, f) => G().Orthof(l, r, b, t, n, f),
      glPushMatrix: () => G().PushMatrix(),
      glPopMatrix: () => G().PopMatrix(),
      glRotatef: (a, x, y, z) => G().Rotatef(a, x, y, z),
      glScalef: (x, y, z) => G().Scalef(x, y, z),
      glTranslatef: (x, y, z) => G().Translatef(x, y, z),
      glShadeModel: (m) => G().ShadeModel(m),
      glTexParameteri: (t, p, v) => G().TexParameteri(t, p, v),
      glTexImage2D: (t, l, i, w, h, b, f, ty, p) => G().TexImage2D(t, l, i, w, h, b, f, ty, p),
      glTexSubImage2D: (t, l, x, y, w, h, f, ty, p) => G().TexSubImage2D(t, l, x, y, w, h, f, ty, p),
      glVertexPointer: (s, t, st, p) => G().VertexPointer(s, t, st, p),
      glTexCoordPointer: (s, t, st, p) => G().TexCoordPointer(s, t, st, p),
      glColorPointer: (s, t, st, p) => G().ColorPointer(s, t, st, p),
      glViewport: (x, y, w, h) => G().Viewport(x, y, w, h),
    };
  }

  dialogResult(id, which, text) {
    let p = 0;
    if (text !== null && text !== undefined) {
      const enc = new TextEncoder().encode(text + '\0');
      p = this.ex.vm_alloc(enc.length); new Uint8Array(this.mem.buffer).set(enc, p);
    }
    try { this.ex.vm_dialog_result(id, which, p); } catch (e) { if (String(e).indexOf('__vm_exit__') < 0) throw e; }
    if (p) this.ex.vm_free(p);
  }
  back() { if (this.ex && this.running) { try { this.ex.vm_back(); } catch (e) { if (String(e).indexOf('__vm_exit__') < 0) throw e; } } }
  pause() { this.paused = true; this.audio.pauseAll(); }
  resume() { this.paused = false; this.audio.resumeAll(); }
}

window.PGHost = PGHost;
window.parseZip = parseZip;
