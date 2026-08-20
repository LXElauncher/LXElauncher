(function (global) {
  'use strict';

  var nextId = 1;
  var pending = new Map();
  var listeners = new Map();

  function call(method, params) {
    return new Promise(function (resolve, reject) {
      var id = nextId++;
      pending.set(id, { resolve: resolve, reject: reject });
      if (global.chrome && global.chrome.webview) {
        global.chrome.webview.postMessage(
          JSON.stringify({ id: id, method: method, params: params || {} })
        );
      } else {
        reject(new Error('不在 WebView2 环境里运行，无法调用后端接口'));
      }
    });
  }

  function on(event, handler) {
    var list = listeners.get(event);
    if (!list) {
      list = [];
      listeners.set(event, list);
    }
    list.push(handler);
    return function unsubscribe() {
      off(event, handler);
    };
  }

  function off(event, handler) {
    var list = listeners.get(event);
    if (!list || !list.length) return;
    if (!handler) {
      listeners.delete(event);
      return;
    }
    var i = list.indexOf(handler);
    if (i >= 0) list.splice(i, 1);
    if (!list.length) listeners.delete(event);
  }

  function emit(event, data) {
    var list = (listeners.get(event) || []).slice();
    list.forEach(function (h) { h(data); });
  }

  if (global.chrome && global.chrome.webview) {
    global.chrome.webview.addEventListener('message', function (e) {
      var msg = e.data;
      if (!msg) return;
      if (typeof msg.id !== 'undefined') {
        var p = pending.get(msg.id);
        if (!p) return;
        pending.delete(msg.id);
        if (msg.error) {
          var err = new Error(msg.error.message || ('后端返回错误 ' + msg.error.code));
          err.code = msg.error.code;
          p.reject(err);
        } else {
          p.resolve(msg.result);
        }
      } else if (msg.event) {
        var list = listeners.get(msg.event) || [];
        list.forEach(function (h) { h(msg.data); });
      }
    });
  }

  global.LX = {
    call: call,
    on: on,
    off: off,
    emit: emit,
    invoke: call
  };
})(window);