const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const { free_port, make_client, run, delay } = require('./transport_acceptance.cjs');

async function verify_browser(binary, assetDirectory, outputDirectory, mode, certificates) {
  await fs.mkdir(outputDirectory, { recursive: true });
  const deployment = await fs.mkdtemp(path.join(path.resolve(outputDirectory), 'empty-deployment-'));
  const expected_assets = new Map();
  for (const name of ['swagger-ui.css', 'swagger-ui-bundle.js', 'swagger-ui-standalone-preset.js',
    'favicon-16x16.png', 'favicon-32x32.png', 'LICENSE', 'NOTICE', 'README.md', 'SHA256SUMS']) {
    expected_assets.set(name, await fs.readFile(path.join(assetDirectory, name)));
  }
  const port = await free_port();
  const secure = mode === 'https' || mode === 'h2';
  const origin = `${secure ? 'https' : 'http'}://127.0.0.1:${port}`;
  const trace_file = process.env.STRACE_OUTPUT && `${path.resolve(process.env.STRACE_OUTPUT)}.${mode}`;
  if (trace_file) await fs.rm(trace_file, { force: true });
  const command = trace_file ? 'strace' : path.resolve(binary);
  const serve_arguments = ['--port', String(port), '--transport', mode, '--cert', certificates.cert, '--key', certificates.key];
  const arguments_ = trace_file ? ['-f', '-s', '4096', '-o', trace_file, '-e', 'trace=%file',
    path.resolve(binary), ...serve_arguments] : serve_arguments;
  const server = spawn(command, arguments_,
    { cwd: deployment, stdio: ['ignore', 'pipe', 'pipe'] });
  let application_pid = server.pid;
  let serverLog = '';
  server.stdout.on('data', bytes => { serverLog += bytes; });
  server.stderr.on('data', bytes => { serverLog += bytes; });
  const exited = new Promise((resolve, reject) => {
    server.once('error', reject);
    server.once('exit', (code, signal) => resolve({ code, signal }));
  });
  let browser;
  let client;
  try {
    if (trace_file) {
      for (let attempt = 0; attempt < 100; ++attempt) {
        const trace = await fs.readFile(trace_file, 'utf8').catch(error => {
          if (error.code === 'ENOENT') return '';
          throw error;
        });
        const started = trace.match(/^(\d+)\s+execve\(/m);
        if (started) { application_pid = Number(started[1]); break; }
        assert(server.exitCode === null, `strace startup failed: ${serverLog}`);
        await new Promise(resolve => setTimeout(resolve, 20));
      }
      assert(application_pid !== server.pid, 'strace must report the traced application PID');
    }
    for (let attempt = 0; !serverLog.includes('Swagger UI: '); ++attempt) {
      assert(server.exitCode === null && attempt < 200, `server did not become ready: ${serverLog}`);
      await delay(50);
    }
    client = await make_client(mode, port, await fs.readFile(certificates.cert));
    const served = await client.request('GET', '/openapi.json');
    assert.equal(served.status, 200);
    const spec = served.bytes.toString();
    run(path.resolve(binary), ['--export', path.join(outputDirectory, 'export-openapi.json')]);
    assert.equal(await fs.readFile(path.join(outputDirectory, 'export-openapi.json'), 'utf8'), spec);
    await fs.writeFile(path.join(outputDirectory, 'served-openapi.json'), spec);
    for (const [name, expected] of expected_assets) {
      const response = await client.request('GET', `/docs/${name}`);
      assert.equal(response.status, 200, name);
      assert.equal(response.headers['x-content-type-options'], 'nosniff');
      assert.deepEqual(response.bytes, expected);
    }
    browser = await chromium.launch({ headless: true, args: process.env.CHROMIUM_NET_LOG
      ? [`--log-net-log=${path.resolve(process.env.CHROMIUM_NET_LOG)}.${mode}`, '--net-log-capture-mode=Everything'] : [] });
    const evidence = { browser: browser.version(), mode, origin, serverProtocol: client.protocol,
      selfSignedBrowserCertificateException: secure, embeddedResources: expected_assets.size,
      filesystemTrace: trace_file || null,
      workingDirectory: deployment, viewports: [] };
    for (const [name, viewport] of [['desktop', { width: 1440, height: 1000 }],
      ['mobile', { width: 390, height: 844 }]]) {
      const context = await browser.newContext({ viewport, serviceWorkers: 'block', ignoreHTTPSErrors: secure });
      const page = await context.newPage();
      const cdp = await context.newCDPSession(page);
      await cdp.send('Network.enable');
      const protocols = [];
      const network_events = [];
      for (const event of ['requestWillBeSent', 'dataReceived', 'loadingFinished', 'loadingFailed']) {
        cdp.on(`Network.${event}`, value => network_events.push({ event, ...value }));
      }
      cdp.on('Network.responseReceived', event => {
        network_events.push({ event: 'responseReceived', ...event });
        if (event.response.url.startsWith(origin)) protocols.push({ url: event.response.url, protocol: event.response.protocol });
      });
      const errors = [];
      const external = [];
      page.on('pageerror', error => errors.push(error.message));
      page.on('console', message => { if (message.type() === 'error') errors.push(message.text()); });
      await context.route('**/*', route => {
        const url = new URL(route.request().url());
        if (url.origin !== origin) { external.push(url.href); return route.abort(); }
        return route.continue();
      });
      page.on('response', response => { if (response.status() >= 400) errors.push(`${response.status()} ${response.url()}`); });
      let response;
      try {
        response = await page.goto(`${origin}/docs`, { waitUntil: 'networkidle' });
      } catch (error) {
        await fs.writeFile(path.join(outputDirectory, `${name}-failed.json`),
          JSON.stringify({ error: error.message, protocols, errors, external, network_events }, null, 2));
        throw error;
      }
      if (mode === 'h2c') {
        assert.equal(response.status(), 404, 'browser HTTP/1 fallback must not count as h2c UI support');
        assert(protocols.length > 0 && protocols.every(value => value.protocol === 'http/1.1'));
        assert.deepEqual(external, []);
        evidence.browserLimitation = 'Chromium uses HTTP/1.1 on cleartext origins, not h2c prior knowledge';
        evidence.viewports.push({ name, http1FallbackStatus: 404, protocols, tryItOut: 'not available over browser h2c' });
        await page.screenshot({ path: path.join(outputDirectory, `${name}-h2c-limitation.png`) });
        await context.close();
        continue;
      }
      assert.equal(response.status(), 200);
      await page.locator('.opblock-get').waitFor({ state: 'visible' });
      await page.locator('.opblock-post').waitFor({ state: 'visible' });
      assert.equal(await page.evaluate(() => window.ui.specSelectors.specJson().get('openapi')), '3.1.0');
      const layout = await page.evaluate(() => ({
        viewport: innerWidth,
        width: document.documentElement.scrollWidth,
        operations: [...document.querySelectorAll('.opblock-summary')].map(element => ({
          text: element.textContent.trim(), width: element.getBoundingClientRect().width,
          height: element.getBoundingClientRect().height,
        })),
      }));
      assert(layout.width <= layout.viewport + 1, `${name} has horizontal overflow: ${JSON.stringify(layout)}`);
      assert(layout.operations.every(operation => operation.width > 0 && operation.height >= 30), 'visible operation controls');
      await page.screenshot({ path: path.join(outputDirectory, `${name}.png`), fullPage: true });
      {
        const get = page.locator('.opblock-get');
        await get.locator('.opblock-summary').click();
        await get.getByRole('button', { name: /Try it out/i }).click();
        await get.locator('input[type="text"]').first().fill('1');
        const getResponse = page.waitForResponse(result => result.request().method() === 'GET' &&
          new URL(result.url()).pathname === '/users/1');
        await get.getByRole('button', { name: 'Execute', exact: true }).click();
        const got = await getResponse;
        assert.equal(got.status(), 200);
        assert.equal((await got.json()).name, 'Ada');
        const post = page.locator('.opblock-post');
        await post.locator('.opblock-summary').click();
        await post.getByRole('button', { name: /Try it out/i }).click();
        await post.locator('textarea').first().fill('{"name":"Lin","email":null}');
        const postResponse = page.waitForResponse(result => result.request().method() === 'POST' &&
          new URL(result.url()).pathname === '/users');
        await post.getByRole('button', { name: 'Execute', exact: true }).click();
        const created = await postResponse;
        assert.equal(created.status(), 201);
        assert.equal((await created.json()).name, 'Lin');
        await page.screenshot({ path: path.join(outputDirectory, `${name}-executed.png`), fullPage: true });
        evidence.get = 200;
        evidence.post = 201;
      }
      assert.deepEqual(external, [], 'offline UI attempted an external request');
      assert.deepEqual(errors, [], 'UI resource/console errors');
      const expected_protocol = mode === 'h2' ? 'h2' : 'http/1.1';
      assert(protocols.length >= 7 && protocols.every(value => value.protocol === expected_protocol),
        `${mode}: browser docs, resources and Try it out must actually use ${expected_protocol}: ${JSON.stringify(protocols)}`);
      evidence.viewports.push({ name, ...layout, externalRequests: external.length, errors: errors.length,
        protocols, tryItOut: { get: 200, post: 201 } });
      await context.close();
    }
    await fs.writeFile(path.join(outputDirectory, 'browser.json'), JSON.stringify(evidence, null, 2));
    console.log(JSON.stringify(evidence, null, 2));
    return evidence;
  } finally {
    if (browser) await browser.close();
    if (client) await client.close();
    if (server.exitCode === null) {
      try { process.kill(application_pid, 'SIGTERM'); } catch (error) {
        if (error.code !== 'ESRCH') throw error;
      }
    }
    const timer = setTimeout(() => {
      if (server.exitCode === null) {
        try { process.kill(application_pid, 'SIGKILL'); } catch (error) {
          if (error.code !== 'ESRCH') throw error;
        }
        server.kill('SIGKILL');
      }
    }, 5000);
    const result = await exited;
    clearTimeout(timer);
    await fs.writeFile(path.join(outputDirectory, 'server.log'), serverLog);
    assert.deepEqual(await fs.readdir(deployment), [], 'server must not create or extract UI files');
    await fs.rmdir(deployment);
    assert.equal(result.code, 0, `server cleanup failed: ${JSON.stringify(result)} ${serverLog}`);
    if (trace_file) {
      const trace = await fs.readFile(trace_file, 'utf8');
      const paths = [...expected_assets.keys(), 'assets/swagger-ui', 'share/galay/swagger-ui'];
      assert(!paths.some(name => trace.includes(name)), 'startup or requests accessed UI resource files');
    }
  }
}

async function main() {
  const [binary, assets, output_, modes = 'http,https,h2,h2c'] = process.argv.slice(2);
  assert(binary && assets && output_, 'binary, assets and output directory are required');
  const output = path.resolve(output_);
  await fs.mkdir(output, { recursive: true });
  const certificates = { cert: path.join(output, 'localhost.crt'), key: path.join(output, 'localhost.key') };
  run('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2', '-subj', '/CN=localhost',
    '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1', '-out', certificates.cert, '-keyout', certificates.key]);
  const evidence = [];
  for (const mode of modes.split(',')) {
    evidence.push(await verify_browser(binary, assets, path.join(output, mode), mode, certificates));
  }
  await fs.writeFile(path.join(output, 'browsers.json'), JSON.stringify(evidence, null, 2));
}

main().catch(error => { console.error(error); process.exitCode = 1; });
