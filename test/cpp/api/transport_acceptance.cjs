const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const net = require('node:net');
const http = require('node:http');
const https = require('node:https');
const http2 = require('node:http2');
const tls = require('node:tls');
const { spawn, spawnSync } = require('node:child_process');

const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const observations = [];

function run(command, args) {
  const result = spawnSync(command, args, { encoding: 'utf8' });
  assert.equal(result.status, 0, `${command}: ${result.error || result.stderr || result.stdout}`);
  return result;
}

async function free_port() {
  const server = net.createServer();
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
  const port = server.address().port;
  await new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
  return port;
}

async function start_server(binary, mode, certificates, directory, extra = [], ready = 'READY\n') {
  const port = await free_port();
  const deployment = await fs.mkdtemp(path.join(directory, `${mode}-empty-deployment-`));
  const args = ['--transport', mode, '--port', String(port), '--cert', certificates.cert, '--key', certificates.key, ...extra];
  const process_ = spawn(binary, args, { cwd: deployment, stdio: ['ignore', 'pipe', 'pipe'] });
  let log = '';
  process_.stdout.on('data', bytes => { log += bytes; });
  process_.stderr.on('data', bytes => { log += bytes; });
  const exited = new Promise((resolve, reject) => {
    process_.once('error', reject);
    process_.once('exit', (code, signal) => resolve({ code, signal }));
  });
  for (let attempt = 0; !log.includes(ready); ++attempt) {
    if (process_.exitCode !== null || attempt >= 1000) {
      if (process_.exitCode === null) assert(process_.kill('SIGKILL'), 'stop unready fixture');
      await exited;
      assert.fail(`startup failed: ${log}`);
    }
    await delay(10);
  }
  let stopped = false;
  return {
    port, deployment,
    async stop() {
      if (stopped) return log;
      stopped = true;
      assert(process_.kill('SIGTERM'), 'request fixture shutdown');
      const timer = setTimeout(() => { assert(process_.kill('SIGKILL'), 'stop timed out fixture'); }, 10000);
      const result = await exited;
      clearTimeout(timer);
      assert.deepEqual(result, { code: 0, signal: null }, `shutdown failed: ${log}`);
      assert(!/ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:/.test(log), log);
      assert.deepEqual(await fs.readdir(deployment), [], 'no UI extraction or runtime deployment files');
      return log;
    }
  };
}

async function make_client(mode, port, ca, settings = {}) {
  const secure = mode === 'https' || mode === 'h2';
  const origin = `${secure ? 'https' : 'http'}://127.0.0.1:${port}`;
  if (mode === 'http' || mode === 'https') {
    return {
      origin, protocol: secure ? 'http/1.1' : 'HTTP/1.1',
      request(method, target, body = '', content_type = 'application/json') {
        return new Promise((resolve, reject) => {
          const request = (secure ? https : http).request(`${origin}${target}`, {
            method, agent: false, ca, servername: 'localhost', ALPNProtocols: ['http/1.1'],
            headers: body ? { 'content-type': content_type, 'content-length': Buffer.byteLength(body) } : {}
          }, response => {
            const chunks = [];
            response.on('data', chunk => chunks.push(chunk));
            response.once('error', reject);
            response.once('end', () => {
              assert.equal(response.httpVersion, '1.1');
              if (secure) assert.equal(response.socket.alpnProtocol, 'http/1.1');
              resolve({ status: response.statusCode, headers: response.headers, bytes: Buffer.concat(chunks) });
            });
          });
          request.setTimeout(10000, () => request.destroy(new Error('HTTP request timeout')));
          request.once('error', reject);
          request.end(body);
        });
      },
      async close() {}
    };
  }
  const session = http2.connect(origin, { ca, servername: 'localhost', ALPNProtocols: ['h2'], settings });
  await new Promise((resolve, reject) => { session.once('connect', resolve); session.once('error', reject); });
  session.on('error', () => {}); // Each in-flight stream also reports session failure.
  if (secure) assert.equal(session.socket.alpnProtocol, 'h2', 'real TLS ALPN h2, not HTTP/1 fallback');
  return {
    origin, session, protocol: secure ? session.socket.alpnProtocol : 'h2c-prior-knowledge',
    request(method, target, body = '', content_type = 'application/json') {
      return new Promise((resolve, reject) => {
        const headers = { ':method': method, ':path': target };
        if (body) Object.assign(headers, { 'content-type': content_type, 'content-length': String(Buffer.byteLength(body)) });
        const stream = session.request(headers, { endStream: !body });
        let response_headers;
        const chunks = [];
        stream.setTimeout(10000, () => stream.destroy(new Error('HTTP/2 stream timeout')));
        stream.once('response', headers_ => { response_headers = headers_; });
        stream.on('data', chunk => chunks.push(chunk));
        stream.once('error', error => reject(new Error(
          `${mode} ${method} ${target} body=${JSON.stringify(body)} headers=${JSON.stringify(response_headers)} received=${Buffer.concat(chunks).length}: ${error.message}`,
          { cause: error })));
        stream.once('end', () => {
          assert(response_headers, 'HTTP/2 HEADERS response');
          assert(!('connection' in response_headers) && !('transfer-encoding' in response_headers), 'no HTTP/1 connection headers on H2');
          resolve({ status: response_headers[':status'], headers: response_headers, bytes: Buffer.concat(chunks) });
        });
        if (body) { stream.write(body.slice(0, 3)); stream.end(body.slice(3)); }
      });
    },
    async close() {
      if (session.destroyed) return;
      const closed = new Promise(resolve => session.once('close', resolve));
      session.close();
      await closed;
    }
  };
}

async function expect_json(client, operation_id, method, target, body, status, expected, content_type = 'application/json') {
  const response = await client.request(method, target, body, content_type);
  assert.equal(response.status, status, `${client.protocol} ${method} ${target}`);
  assert.match(response.headers['content-type'], /^application\/json/);
  const decoded = JSON.parse(response.bytes);
  if (typeof expected === 'string') assert.equal(decoded.code, expected);
  else assert.deepEqual(decoded, expected);
  observations.push({ transport: client.protocol, operationId: operation_id, status, response: decoded,
    request: { method, target, body, contentType: content_type } });
  return decoded;
}

async function test_requests(client, document, assets, output, mode) {
  await expect_json(client, 'getValue', 'GET', '/values/7?verbose=false', '', 200, { id: 7, title: 'Ada', verbose: false });
  await expect_json(client, 'getValue', 'GET', '/values/%37?verbose=%74rue', '', 200, { id: 7, title: 'Ada', verbose: true });
  await expect_json(client, 'getValue', 'GET', '/values/7', '', 200, { id: 7, title: 'Ada', verbose: true });
  const valid_body = '{"display-title":"Ada"}';
  await expect_json(client, 'postValue', 'POST', '/values/8?verbose=false', valid_body, 201, { id: 8, title: 'Ada', verbose: false });
  await expect_json(client, 'postValue', 'POST', '/values/8', valid_body, 201, { id: 8, title: 'Ada', verbose: true });
  const unicode_title = '\u{1f600}'.repeat(40);
  await expect_json(client, 'postValue', 'POST', '/values/8', JSON.stringify({ 'display-title': unicode_title }),
    201, { id: 8, title: unicode_title, verbose: true });
  for (const target of ['/values/0', '/values/7x', '/values/7?verbose=1']) {
    await expect_json(client, 'getValue', 'GET', target, '', 400, 'bad_request');
  }
  for (const body of ['{}', '{', '{"display-title":""}', '{"display-title":"Ada","id":8}', '{"display-title":null}',
    JSON.stringify({ 'display-title': `${unicode_title}\u{1f600}` })]) {
    await expect_json(client, 'postValue', 'POST', '/values/8', body, 400, 'bad_request');
  }
  await expect_json(client, 'postValue', 'POST', '/values/8', valid_body, 415, 'unsupported_media_type', 'text/plain');
  await expect_json(client, 'getValue', 'GET', '/values/7', '{}', 400, 'bad_request');
  await expect_json(client, 'businessError', 'GET', '/business', '', 404, 'business_error');
  await expect_json(client, 'undeclaredError', 'GET', '/undeclared', '', 500, 'business_error');
  await expect_json(client, 'encodingError', 'GET', '/encoding-error', '', 500, 'encoding_error');
  const borrowed = await client.request('POST', '/borrowed/8', valid_body);
  assert.equal(borrowed.status, 200);
  assert.equal(JSON.parse(borrowed.bytes), valid_body, 'borrowed context survives suspension and response serialization');
  for (const [method, target, status] of [['GET', '/no-content', 204], ['POST', '/reset-content', 205],
    ['HEAD', '/head/7', 200], ['HEAD', '/head/7?verbose=1', 400]]) {
    const response = await client.request(method, target);
    assert.equal(response.status, status);
    assert.equal(response.bytes.length, 0);
    assert(!response.headers['content-type'], 'empty response does not advertise JSON');
    if (status === 204) assert(!response.headers['content-length'], '204 forbids Content-Length');
    const operation_id = status === 204 ? 'noContent' : status === 205 ? 'resetContent' : 'headValue';
    observations.push({ transport: client.protocol, operationId: operation_id, status, response: null, bytes: 0,
      request: { method, target, body: '', contentType: '' } });
  }
  assert.equal((await client.request('GET', '/missing')).status, 404);
  const served = await client.request('GET', '/openapi.json');
  assert.equal(served.status, 200);
  assert.equal(served.bytes.toString(), document, 'same immutable OpenAPI on every transport');
  await fs.writeFile(path.join(output, `${mode}-openapi.json`), served.bytes);
  for (const target of ['/docs', '/docs/']) {
    const response = await client.request('GET', target);
    assert.equal(response.status, 200);
    assert.match(response.bytes.toString(), /swagger-ui-bundle.js/);
    assert(!/https?:\/\//.test(response.bytes.toString()), 'no CDN');
  }
  const initializer = await client.request('GET', '/docs/swagger-initializer.js');
  assert.equal(initializer.status, 200);
  assert.match(initializer.bytes.toString(), /"validatorUrl":null/);
  for (const [name, expected] of assets) {
    const resource = await client.request('GET', `/docs/${name}`);
    assert.equal(resource.status, 200, name);
    assert.equal(resource.headers['x-content-type-options'], 'nosniff');
    assert.deepEqual(resource.bytes, expected, `${mode}: complete offline ${name}`);
  }
}

async function stats(client) { return JSON.parse((await client.request('GET', '/stats')).bytes); }

async function wait_for(client, predicate) {
  for (let attempt = 0; attempt < 500; ++attempt) {
    const current = await stats(client);
    if (predicate(current)) return current;
    await delay(5);
  }
  assert.fail('HTTP/2 handlers did not reach the expected lifecycle state');
}

async function test_streams(client, mode, port, ca, assets) {
  const browser_window = 6291456;
  const large_window_client = await make_client(mode, port, ca, { initialWindowSize: browser_window });
  try {
    await Promise.all([...assets].map(async ([name, expected]) => {
      const response = await large_window_client.request('GET', `/docs/${name}`);
      assert.equal(response.status, 200);
      assert.deepEqual(response.bytes, expected, 'concurrent assets must honor negotiated windows on new streams');
    }));
  } finally { await large_window_client.close(); }
  const before = await stats(client);
  const concurrent = await Promise.all(Array.from({ length: 24 }, (_, index) =>
    client.request('GET', `/slow/${index + 1}?verbose=false`)));
  concurrent.forEach((response, index) => {
    assert.equal(response.status, 200);
    assert.deepEqual(JSON.parse(response.bytes), { id: index + 1, title: 'suspended', verbose: false });
  });
  const after = await stats(client);
  assert.equal(after.completed - before.completed, 24);
  assert(after.peak >= 4 && after.active === 0, 'concurrent real stream handlers, not serialized calls');

  const posts_before = after.posts;
  const with_trailers = await new Promise((resolve, reject) => {
    const body = '{"display-title":"trailers"}';
    const stream = client.session.request({ ':method': 'POST', ':path': '/values/8',
      'content-type': 'application/json', 'content-length': String(Buffer.byteLength(body)) }, { waitForTrailers: true });
    stream.setTimeout(10000, () => stream.destroy(new Error('trailing HEADERS timeout')));
    stream.once('wantTrailers', () => stream.sendTrailers({ 'x-trailer': 'complete' }));
    let headers;
    const chunks = [];
    stream.once('response', value => { headers = value; });
    stream.on('data', chunk => chunks.push(chunk));
    stream.once('error', reject);
    stream.once('end', () => resolve({ status: headers[':status'], body: JSON.parse(Buffer.concat(chunks)) }));
    stream.end(body);
  });
  assert.equal(with_trailers.status, 201);
  assert.deepEqual(with_trailers.body, { id: 8, title: 'trailers', verbose: true });
  assert.equal((await stats(client)).posts, posts_before + 1, 'trailing HEADERS complete one request, never spawn a second handler');

  const mismatch = client.session.request({ ':method': 'POST', ':path': '/values/8',
    'content-type': 'application/json', 'content-length': '100' });
  const mismatch_closed = new Promise(resolve => mismatch.once('close', resolve));
  mismatch.on('error', error => assert.equal(error.code, 'ERR_HTTP2_STREAM_ERROR'));
  mismatch.end('{}');
  await mismatch_closed;
  assert.equal(mismatch.rstCode, http2.constants.NGHTTP2_PROTOCOL_ERROR, 'content-length mismatch resets only the malformed stream');
  assert.equal((await client.request('GET', '/values/7')).status, 200);

  const incomplete = client.session.request({ ':method': 'POST', ':path': '/values/8', 'content-type': 'application/json' });
  incomplete.on('error', () => {});
  incomplete.write('{"display-title":');
  const incomplete_closed = new Promise(resolve => incomplete.once('close', resolve));
  incomplete.close(http2.constants.NGHTTP2_CANCEL);
  await incomplete_closed;
  assert.equal((await client.request('GET', '/values/7')).status, 200, 'reset does not kill sibling streams');

  const active_before = await stats(client);
  const cancelled = client.session.request({ ':method': 'GET', ':path': '/slow/77' });
  cancelled.on('error', () => {});
  let responded = false;
  cancelled.on('response', () => { responded = true; });
  await wait_for(client, value => value.started > active_before.started && value.active > 0);
  const cancelled_closed = new Promise(resolve => cancelled.once('close', resolve));
  cancelled.close(http2.constants.NGHTTP2_CANCEL);
  await cancelled_closed;
  await wait_for(client, value => value.active === 0 && value.completed > active_before.completed);
  assert.equal(responded, false, 'do not emit a response after RST_STREAM');

  const blocked_session = http2.connect(client.origin, { ca, servername: 'localhost', settings: { initialWindowSize: 1024 } });
  blocked_session.on('error', () => {});
  await new Promise((resolve, reject) => { blocked_session.once('connect', resolve); blocked_session.once('error', reject); });
  const blocked = blocked_session.request({ ':path': '/docs/swagger-ui-bundle.js' });
  blocked.on('error', () => {});
  blocked.pause();
  await new Promise((resolve, reject) => { blocked.once('response', resolve); blocked.once('error', reject); });
  blocked.close(http2.constants.NGHTTP2_CANCEL);
  const blocked_closed = new Promise(resolve => blocked_session.once('close', resolve));
  blocked_session.close();
  await blocked_closed;
  assert.equal((await client.request('GET', '/values/7')).status, 200, 'flow-control blocked asset reset releases its waiter');

  for (let iteration = 0; iteration < 3; ++iteration) {
    const other = await make_client(mode, port, ca);
    const checkpoint = await stats(client);
    const stream = other.session.request({ ':path': '/slow/99' });
    stream.on('error', () => {});
    stream.resume();
    await wait_for(client, value => value.started > checkpoint.started);
    const closed = new Promise(resolve => other.session.once('close', resolve));
    other.session.destroy();
    await closed;
    await wait_for(client, value => value.active === 0 && value.completed > checkpoint.completed);
    assert.equal((await client.request('GET', '/values/7')).status, 200, 'abrupt peer close cleans up only that connection');
  }
  return { concurrentStreams: concurrent.length, peak: after.peak, initialWindow: browser_window, concurrentAssets: assets.size,
    trailingHeaders: true, contentLengthReset: true,
    incompleteReset: true, handlerReset: true, blockedAssetReset: true, abruptConnections: 3 };
}

async function verify_alpn(mode, port, ca) {
  const socket = tls.connect({ host: '127.0.0.1', port, servername: 'localhost', ca,
    ALPNProtocols: mode === 'h2' ? ['http/1.1', 'h2'] : ['h2', 'http/1.1'] });
  await new Promise((resolve, reject) => { socket.once('secureConnect', resolve); socket.once('error', reject); });
  assert.equal(socket.alpnProtocol, mode === 'h2' ? 'h2' : 'http/1.1', 'server preference, not client ordering');
  assert(socket.authorized, 'test CA and hostname are verified');
  const closed = new Promise(resolve => socket.once('close', resolve));
  socket.destroy();
  await closed;

  for (const [options, expected] of [[{ servername: 'not-localhost.test', ca }, 'ERR_TLS_CERT_ALTNAME_INVALID'],
    [{ servername: 'localhost' }, 'DEPTH_ZERO_SELF_SIGNED_CERT']]) {
    const rejected = tls.connect({ host: '127.0.0.1', port, ...options, ALPNProtocols: ['h2', 'http/1.1'] });
    const rejected_closed = new Promise(resolve => rejected.once('close', resolve));
    const error = await new Promise((resolve, reject) => {
      rejected.once('secureConnect', () => reject(new Error('untrusted certificate or hostname was accepted')));
      rejected.once('error', resolve);
    });
    rejected.destroy();
    await rejected_closed;
    assert.equal(error.code, expected);
  }
  if (mode === 'h2') {
    const fallback = await make_client('https', port, ca);
    assert.equal((await fallback.request('GET', '/values/7')).status, 404, 'native HTTP/1 fallback is not typed H2 support');
    assert.equal((await fallback.request('GET', '/docs')).status, 404, 'HTTP/1 fallback cannot be counted as H2 docs');
    await fallback.close();
  } else {
    const no_overlap = tls.connect({ host: '127.0.0.1', port, servername: 'localhost', ca, ALPNProtocols: ['h2'] });
    await new Promise((resolve, reject) => { no_overlap.once('secureConnect', resolve); no_overlap.once('error', reject); });
    assert.equal(no_overlap.alpnProtocol, false, 'HTTPS never negotiates unsupported h2');
    const closed_ = new Promise(resolve => no_overlap.once('close', resolve));
    no_overlap.destroy();
    await closed_;
  }
  return { preferred: mode === 'h2' ? 'h2' : 'http/1.1', verifiedTrust: true, wrongHostnameRejected: true,
    untrustedCertificateRejected: true, nonH2Fallback: mode === 'h2' ? 404 : false };
}

async function test_live_stop(server, client, mode, ca) {
  const sockets = [];
  const unfinished_handshake = mode === 'https' || mode === 'h2';
  const unfinished_preface = mode === 'h2c' || mode === 'h2';
  const idle = net.connect({ host: '127.0.0.1', port: server.port });
  idle.on('error', () => {});
  idle.on('data', () => {});
  await new Promise((resolve, reject) => { idle.once('connect', resolve); idle.once('error', reject); });
  sockets.push(idle);
  if (mode === 'h2') {
    const preface = tls.connect({ host: '127.0.0.1', port: server.port, ca, servername: 'localhost', ALPNProtocols: ['h2'] });
    preface.on('error', () => {});
    preface.on('data', () => {});
    await new Promise((resolve, reject) => { preface.once('secureConnect', resolve); preface.once('error', reject); });
    assert.equal(preface.alpnProtocol, 'h2');
    sockets.push(preface);
  }
  if (client.session) {
    const fallback = mode === 'h2'
      ? tls.connect({ host: '127.0.0.1', port: server.port, ca, servername: 'localhost', ALPNProtocols: ['http/1.1'] })
      : net.connect({ host: '127.0.0.1', port: server.port });
    fallback.on('error', () => {});
    await new Promise((resolve, reject) => { fallback.once(mode === 'h2' ? 'secureConnect' : 'connect', resolve); fallback.once('error', reject); });
    const response = new Promise((resolve, reject) => { fallback.once('data', resolve); fallback.once('error', reject); });
    fallback.write('GET /openapi.json HTTP/1.1\r\nHost: localhost\r\n\r\n');
    assert.match((await response).toString(), /^HTTP\/1\.1 404 /, 'native HTTP/1 fallback is not the typed HTTP/2 API');
    fallback.on('data', () => {});
    sockets.push(fallback);
  }
  let blocked_session;
  if (client.session) {
    const incomplete = client.session.request({ ':method': 'POST', ':path': '/values/8', 'content-type': 'application/json' });
    incomplete.on('error', () => {});
    incomplete.write('{"');
    const slow = client.session.request({ ':path': '/slow/99' });
    slow.on('error', () => {});
    slow.resume();
    await wait_for(client, value => value.active > 0);
    blocked_session = http2.connect(client.origin, { ca, servername: 'localhost', settings: { initialWindowSize: 0 } });
    blocked_session.on('error', () => {});
    await new Promise((resolve, reject) => { blocked_session.once('connect', resolve); blocked_session.once('error', reject); });
    const blocked = blocked_session.request({ ':path': '/docs/swagger-ui-bundle.js' });
    blocked.on('error', () => {});
    await new Promise((resolve, reject) => { blocked.once('response', resolve); blocked.once('error', reject); });
  } else {
    for (const target of ['POST /values/8 HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{',
      'GET /slow/99 HTTP/1.1\r\nHost: localhost\r\n\r\n']) {
      const socket = mode === 'https'
        ? tls.connect({ host: '127.0.0.1', port: server.port, ca, servername: 'localhost', ALPNProtocols: ['http/1.1'] })
        : net.connect({ host: '127.0.0.1', port: server.port });
      socket.on('error', () => {});
      socket.on('data', () => {});
      await new Promise((resolve, reject) => { socket.once(mode === 'https' ? 'secureConnect' : 'connect', resolve); socket.once('error', reject); });
      socket.write(target);
      sockets.push(socket);
    }
    await wait_for(client, value => value.active > 0);
  }
  // Process exit and peer socket close events can arrive in different event-loop turns.
  const connections_closed = Promise.all(sockets.map(socket => socket.destroyed
    ? Promise.resolve() : new Promise(resolve => socket.once('close', resolve))));
  const log = await server.stop();
  let close_timer;
  try {
    await Promise.race([connections_closed, new Promise((_, reject) => {
      close_timer = setTimeout(() => reject(new Error(`stop left live ${mode} connections open`)), 5000);
    })]);
  } finally {
    clearTimeout(close_timer);
  }
  for (const socket of sockets) {
    assert(socket.destroyed, 'stop closes live HTTP connections as well as listeners');
  }
  if (blocked_session && !blocked_session.destroyed) {
    const closed = new Promise(resolve => blocked_session.once('close', resolve));
    blocked_session.destroy();
    await closed;
  }
  await client.close();
  return { activeHandler: true, incompleteRequest: true, blockedAsset: Boolean(client.session),
    unfinishedHandshake: unfinished_handshake, unfinishedPreface: unfinished_preface,
    idleFallback: Boolean(client.session), log };
}

async function main() {
  const [binary_, asset_directory, output_, modes_ = 'http,https,h2c,h2'] = process.argv.slice(2);
  const binary = path.resolve(binary_);
  const output = path.resolve(output_);
  await fs.mkdir(output, { recursive: true });
  const certificates = { cert: path.join(output, 'localhost.crt'), key: path.join(output, 'localhost.key') };
  run('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2', '-subj', '/CN=localhost',
    '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1', '-out', certificates.cert, '-keyout', certificates.key]);
  const ca = await fs.readFile(certificates.cert);
  const document_path = path.join(output, 'openapi.json');
  run(binary, ['--export', document_path]);
  const document = await fs.readFile(document_path, 'utf8');
  const assets = new Map();
  for (const name of ['swagger-ui.css', 'swagger-ui-bundle.js', 'swagger-ui-standalone-preset.js',
    'favicon-16x16.png', 'favicon-32x32.png', 'LICENSE', 'NOTICE', 'README.md', 'SHA256SUMS']) {
    assets.set(name, await fs.readFile(path.join(asset_directory, name)));
  }
  const evidence = { node: process.version, transports: [] };
  for (const mode of modes_.split(',')) {
    const server = await start_server(binary, mode, certificates, output);
    const client = await make_client(mode, server.port, ca);
    let streams, tls_checks, stopping;
    try {
      await test_requests(client, document, assets, output, mode);
      if (client.session) streams = await test_streams(client, mode, server.port, ca, assets);
      if (mode === 'https' || mode === 'h2') tls_checks = await verify_alpn(mode, server.port, ca);
      stopping = await test_live_stop(server, client, mode, ca);
    } finally {
      await client.close();
      await fs.writeFile(path.join(output, `${mode}-server.log`), await server.stop());
    }
    const hidden_server = await start_server(binary, mode, certificates, output, ['--no-swagger']);
    const hidden_client = await make_client(mode, hidden_server.port, ca);
    try {
      await expect_json(hidden_client, 'getValue', 'GET', '/values/7', '', 200,
        { id: 7, title: 'Ada', verbose: true });
      await expect_json(hidden_client, 'getValue', 'GET', '/values/7?verbose=1', '', 400, 'bad_request');
      await expect_json(hidden_client, 'postValue', 'POST', '/values/8', '{"display-title":"hidden"}', 201,
        { id: 8, title: 'hidden', verbose: true });
      await expect_json(hidden_client, 'postValue', 'POST', '/values/8', '{}', 400, 'bad_request');
      await expect_json(hidden_client, 'postValue', 'POST', '/values/8', '{}', 415,
        'unsupported_media_type', 'text/plain');
      await expect_json(hidden_client, 'businessError', 'GET', '/business', '', 404, 'business_error');
      await expect_json(hidden_client, 'undeclaredError', 'GET', '/undeclared', '', 500, 'business_error');
      await expect_json(hidden_client, 'encodingError', 'GET', '/encoding-error', '', 500, 'encoding_error');
      for (const target of ['/openapi.json', '/docs', '/docs/swagger-ui-bundle.js', '/docs/favicon-32x32.png']) {
        assert.equal((await hidden_client.request('GET', target)).status, 404, 'Swagger-disabled builder exposes no documentation');
      }
    } finally { await hidden_client.close(); await hidden_server.stop(); }
    const missing_port = await free_port();
    const missing = spawnSync(binary, ['--transport', mode, '--port', String(missing_port), '--cert', certificates.cert,
      '--key', certificates.key, '--assets', path.join(output, 'missing-assets')], { encoding: 'utf8' });
    assert.equal(missing.status, 1, 'explicit directory with missing resources must fail');
    assert.match(missing.stderr, /resource_error.*swagger-ui.css/);
    assert(!/ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:/.test(missing.stdout + missing.stderr),
      `missing-directory cleanup sanitizer failure: ${missing.stdout}${missing.stderr}`);
    evidence.transports.push({ mode, port: server.port, protocol: client.protocol, embeddedAssets: assets.size,
      docs: true, noSwagger: true, missingDirectoryFailure: true, streams, tls: tls_checks,
      liveStop: { activeHandler: stopping.activeHandler, incompleteRequest: stopping.incompleteRequest, blockedAsset: stopping.blockedAsset,
        unfinishedHandshake: stopping.unfinishedHandshake, unfinishedPreface: stopping.unfinishedPreface, idleFallback: stopping.idleFallback } });
  }
  await fs.writeFile(path.join(output, 'observations.json'), JSON.stringify(observations, null, 2));
  await fs.writeFile(path.join(output, 'transports.json'), JSON.stringify(evidence, null, 2));
  console.log(JSON.stringify(evidence, null, 2));
}

module.exports = { delay, free_port, start_server, make_client, run };
if (require.main === module) main().catch(error => { console.error(error); process.exitCode = 1; });
