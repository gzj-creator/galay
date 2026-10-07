const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const net = require('node:net');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');

async function free_port() {
  const probe = net.createServer();
  await new Promise((resolve, reject) => {
    probe.once('error', reject);
    probe.listen(0, '127.0.0.1', resolve);
  });
  const port = probe.address().port;
  await new Promise((resolve, reject) => probe.close(error => error ? reject(error) : resolve()));
  return port;
}

async function main() {
  const [binary, assetDirectory, outputDirectory] = process.argv.slice(2);
  assert(binary && assetDirectory && outputDirectory, 'binary, assets and output directory are required');
  await fs.mkdir(outputDirectory, { recursive: true });
  const deployment = await fs.mkdtemp(path.join(path.resolve(outputDirectory), 'empty-deployment-'));
  const expected_assets = new Map();
  for (const name of ['swagger-ui.css', 'swagger-ui-bundle.js', 'swagger-ui-standalone-preset.js',
    'favicon-16x16.png', 'favicon-32x32.png', 'LICENSE', 'NOTICE', 'README.md', 'SHA256SUMS']) {
    expected_assets.set(name, await fs.readFile(path.join(assetDirectory, name)));
  }
  const port = await free_port();
  const origin = `http://127.0.0.1:${port}`;
  const trace_file = process.env.STRACE_OUTPUT && path.resolve(process.env.STRACE_OUTPUT);
  const command = trace_file ? 'strace' : path.resolve(binary);
  const arguments_ = trace_file ? ['-f', '-s', '4096', '-o', trace_file, '-e', 'trace=%file',
    path.resolve(binary), '--port', String(port)] : ['--port', String(port)];
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
    let spec;
    for (let attempt = 0; attempt < 200; ++attempt) {
      if (server.exitCode !== null) throw new Error(`server exited: ${serverLog}`);
      try {
        const response = await fetch(`${origin}/openapi.json`);
        if (response.ok) { spec = await response.text(); break; }
      } catch (error) {
        if (attempt === 199) throw error;
      }
      await new Promise(resolve => setTimeout(resolve, 50));
    }
    assert(spec, `server did not become ready: ${serverLog}`);
    await fs.writeFile(path.join(outputDirectory, 'served-openapi.json'), spec);
    for (const [name, expected] of expected_assets) {
      const response = await fetch(`${origin}/docs/${name}`);
      assert.equal(response.status, 200, name);
      assert.equal(response.headers.get('x-content-type-options'), 'nosniff');
      assert.deepEqual(Buffer.from(await response.arrayBuffer()), expected);
    }
    browser = await chromium.launch({ headless: true });
    const evidence = { browser: browser.version(), origin, embeddedResources: expected_assets.size,
      filesystemTrace: trace_file || null,
      workingDirectory: deployment, viewports: [] };
    for (const [name, viewport] of [['desktop', { width: 1440, height: 1000 }],
      ['mobile', { width: 390, height: 844 }]]) {
      const context = await browser.newContext({ viewport, serviceWorkers: 'block' });
      const page = await context.newPage();
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
      const response = await page.goto(`${origin}/docs`, { waitUntil: 'networkidle' });
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
      if (name === 'desktop') {
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
        await page.screenshot({ path: path.join(outputDirectory, 'desktop-executed.png'), fullPage: true });
        evidence.get = 200;
        evidence.post = 201;
      }
      assert.deepEqual(external, [], 'offline UI attempted an external request');
      assert.deepEqual(errors, [], 'UI resource/console errors');
      evidence.viewports.push({ name, ...layout, externalRequests: external.length, errors: errors.length });
      await context.close();
    }
    await fs.writeFile(path.join(outputDirectory, 'browser.json'), JSON.stringify(evidence, null, 2));
    console.log(JSON.stringify(evidence, null, 2));
  } finally {
    if (browser) await browser.close();
    if (server.exitCode === null) process.kill(application_pid, 'SIGTERM');
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

main().catch(error => { console.error(error); process.exitCode = 1; });
