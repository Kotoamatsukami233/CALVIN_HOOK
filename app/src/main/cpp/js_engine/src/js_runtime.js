'use strict';

// Bootstrap: wraps _hook namespace into scripting API

const _np = _hook.NativePointer;
const _mem = _hook.Memory;
const _mod = _hook.Module;
const _int = _hook.Interceptor;
const _nf = _hook.NativeFunction;
const _nc = _hook.NativeCallback;
const _java = _hook.Java;

// ptr(v) — shorthand for new NativePointer(v)
function ptr(v) {
    return new _np(v);
}

// Memory API
const Memory = {
    readU8:            (a) => _mem.readU8(a),
    readU16:           (a) => _mem.readU16(a),
    readU32:           (a) => _mem.readU32(a),
    readU64:           (a) => _mem.readU64(a),
    readS8:            (a) => _mem.readS8(a),
    readS16:           (a) => _mem.readS16(a),
    readS32:           (a) => _mem.readS32(a),
    readS64:           (a) => _mem.readS64(a),
    readFloat:         (a) => _mem.readFloat(a),
    readDouble:        (a) => _mem.readDouble(a),
    readPointer:       (a) => _mem.readPointer(a),
    readUtf8String:    (a, s) => _mem.readUtf8String(a, s),
    readByteArray:     (a, s) => _mem.readByteArray(a, s),
    writeU8:           (a, v) => _mem.writeU8(a, v),
    writeU16:          (a, v) => _mem.writeU16(a, v),
    writeU32:          (a, v) => _mem.writeU32(a, v),
    writeU64:          (a, v) => _mem.writeU64(a, v),
    writeS8:           (a, v) => _mem.writeS8(a, v),
    writeS16:          (a, v) => _mem.writeS16(a, v),
    writeS32:          (a, v) => _mem.writeS32(a, v),
    writeS64:          (a, v) => _mem.writeS64(a, v),
    writeFloat:        (a, v) => _mem.writeFloat(a, v),
    writeDouble:       (a, v) => _mem.writeDouble(a, v),
    writePointer:      (a, v) => _mem.writePointer(a, v),
    writeUtf8String:   (a, v) => _mem.writeUtf8String(a, v),
    writeByteArray:    (a, v) => _mem.writeByteArray(a, v),
    alloc:             (s) => _mem.alloc(s),
};

// Module API
const Module = {
    findExportByName:  (m, s) => _mod.findExportByName(m, s),
    getBaseAddress:    (m) => _mod.getBaseAddress(m),
};

// Interceptor API
const Interceptor = {
    attach:            (t, cb) => _int.attach(t, cb),
    replace:           (t, r) => _int.replace(t, r),
    detachAll:         () => _int.detachAll(),
};

// Java API
const Java = {
    perform:           (fn) => _java.perform(fn),
    use:               (cls) => _java.use(cls),
};

// Expose to global scope
globalThis.ptr = ptr;
globalThis.NativePointer = _np;
globalThis.NativeFunction = _nf;
globalThis.NativeCallback = _nc;
globalThis.Memory = Memory;
globalThis.Module = Module;
globalThis.Interceptor = Interceptor;
globalThis.Java = Java;
