const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

// Execute the real fixture functions with synthetic display callbacks.
// This is not browser automation and does not read any open browser page.
const html = fs.readFileSync(path.join(__dirname, 'web-marker.html'), 'utf8');
const script = html.match(/<script>([\s\S]*)<\/script>/)[1]
    .split('\ncalibrate().then(')[0];

function fixture(motion = false) {
  let now = 0, clockOffset = 1000;
  const posts = [], transforms = [];
  const drawing = {fillRect() {}, fillText() {}, save() {}, restore() {},
    translate() {}, setTransform(...args) {transforms.push(args);}, createPattern() {return {};},
    createImageData(w,h) {return {data:new Uint8ClampedArray(w*h*4)};}, putImageData() {}};
  const canvas = {width: 1280, height: 720, getContext: () => drawing};
  const context = vm.createContext({
    crypto: require('node:crypto').webcrypto,
    document: {getElementById: () => canvas, createElement: () => ({getContext: () => drawing})},
    location: {pathname: '/test/marker', hash: motion ? '#motion' : ''},
    devicePixelRatio: 1.5,
    performance: {now: () => now},
    innerWidth: 1280, innerHeight: 720,
    screen: {width: 1280, height: 720},
    navigator: {userAgent: 'synthetic-unit-test'},
    requestAnimationFrame() {}, setTimeout() {}, console,
    fetch: async (url, options) => {
      if (url.endsWith('/clock')) {
        return {json: async () => ({receive_ms: now + clockOffset, send_ms: now + clockOffset})};
      }
      posts.push(JSON.parse(options.body));
      return {ok: true};
    },
  });
  vm.runInContext(script, context);
  return {
    run: source => vm.runInContext(source, context),
    draw(time) {now = time; vm.runInContext(`draw(${time})`, context);},
    setClockOffset(value) {clockOffset = value;},
    posts, transforms,
  };
}

for (const hz of [60, 120, 144, 165]) {
  test(`60 marker updates on a ${hz} Hz source display`, () => {
    const marker = fixture();
    marker.run('ready=true');
    for (let frame = 0; frame < hz; frame++) marker.draw(frame * 1000 / hz);
    assert.equal(marker.run('seq'), 60);
  });
}

test('a delayed callback skips expired slots without a catch-up burst', () => {
  const marker = fixture();
  marker.run('ready=true');
  marker.draw(0);
  marker.draw(1000);
  const sequence = marker.run('seq');
  marker.draw(1001);
  marker.draw(1002);
  assert.equal(marker.run('seq'), sequence);
});

test('recalibration records drift but never steps the rendered clock', async () => {
  const marker = fixture();
  await marker.run('calibrate()');
  assert.equal(marker.run('offset'), 1000);
  marker.setClockOffset(1004);
  await marker.run('calibrate()');
  assert.equal(marker.run('offset'), 1000);
  assert.equal(marker.posts[1].render_offset_ms, 1000);
  assert.equal(marker.posts[1].offset_low_ms, 1003);
  assert.equal(marker.posts[1].offset_high_ms, 1005);
});

test('motion stress keeps 60 Hz cadence and native backing pixels', async () => {
  const marker = fixture(true);
  marker.run('ready=true');
  for (let frame=0;frame<120;frame++) marker.draw(frame*1000/120);
  assert.equal(marker.run('seq'),60);
  assert.equal(marker.run('canvas.width'),1920);
  assert.equal(marker.run('canvas.height'),1080);
  await marker.run('calibrate()');
  assert.equal(marker.posts[0].motion_mode,true);
  assert.deepEqual(marker.posts[0].canvas_pixels,[1920,1080]);
});

test('each frame resets its transform after a same-pixel-count display change', () => {
  const marker = fixture(true);
  marker.run('ready=true');
  marker.draw(0);
  assert.deepEqual(marker.transforms.at(-1),[1.5,0,0,1.5,0,0]);
  marker.run('innerWidth=1920;innerHeight=1080;devicePixelRatio=1');
  marker.draw(20);
  assert.equal(marker.run('canvas.width'),1920);
  assert.equal(marker.run('canvas.height'),1080);
  assert.deepEqual(marker.transforms.at(-1),[1,0,0,1,0,0]);
  marker.draw(40);
  assert.equal(marker.transforms.length,3);
});
