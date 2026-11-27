import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const base = dirname(fileURLToPath(import.meta.url));
const server = spawn(process.execPath, [join(base, 'server.mjs'), ...process.argv.slice(2)], {
  stdio: ['pipe', 'pipe', 'inherit'],
  windowsHide: true,
});
const lines = createInterface({ input: server.stdout });
const pending = new Map();
let nextId = 0;

lines.on('line', line => {
  const message = JSON.parse(line);
  const request = pending.get(message.id);
  if (!request) return;
  pending.delete(message.id);
  clearTimeout(request.timer);
  if (message.error) request.reject(new Error(JSON.stringify(message.error)));
  else request.resolve(message.result);
});

server.on('exit', (code, signal) => {
  for (const request of pending.values()) {
    clearTimeout(request.timer);
    request.reject(new Error(`MCP exited before replying (${code ?? signal})`));
  }
  pending.clear();
});

function rpc(method, params = {}) {
  const id = ++nextId;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`MCP timeout in ${method}`));
    }, 70000);
    pending.set(id, { resolve, reject, timer });
    server.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', id, method, params })}\n`);
  });
}

async function call(name, args = {}) {
  const result = await rpc('tools/call', { name, arguments: args });
  assert.equal(Boolean(result.isError), false, result.content[0].text);
  return JSON.parse(result.content[0].text);
}

function near(actual, expected) {
  assert.ok(Math.abs(actual - expected) < Math.max(1e-6, Math.abs(expected) * 0.0005), `${actual} vs ${expected}`);
}

try {
  const setup = await rpc('initialize', {
    protocolVersion: '2024-11-05', capabilities: {},
    clientInfo: { name: 'creo2-smoke', version: '1.0.0' },
  });
  assert.equal(setup.protocolVersion, '2024-11-05');
  server.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n');
  const available = await rpc('tools/list');
  for (const name of ['creo_status', 'creo_new_part', 'creo_rectangle_sketch', 'creo_extrude',
    'creo_through_hole', 'creo_inspect', 'creo_set_dimension', 'creo_save_part', 'creo_open_part']) {
    assert.ok(available.tools.some(tool => tool.name === name), name);
  }
  const invalid = await rpc('tools/call', { name: 'creo_new_part', arguments: { name: '../escape' } });
  assert.equal(invalid.isError, true);
  assert.match(invalid.content[0].text, /name/i);
  const reserved = await rpc('tools/call', { name: 'creo_new_part', arguments: { name: 'mmns_part_solid' } });
  assert.equal(reserved.isError, true);
  assert.match(reserved.content[0].text, /non-reserved/i);
  const zeroDimension = await rpc('tools/call', {
    name: 'creo_set_dimension', arguments: { model: 'plate', feature_id: 1, dimension_id: 0, value_mm: 0 },
  });
  assert.equal(zeroDimension.isError, true);
  assert.match(zeroDimension.content[0].text, /value_mm/i);

  const status = await call('creo_status');
  if (!status.connected && process.env.CREO2_LIVE_TEST !== '1') {
    console.log('MCP handshake, tool list and validation passed. Live modeling skipped.');
  } else {
    assert.equal(status.connected, true, JSON.stringify(status));
    assert.equal(status.protocol, 2);
    const model = process.env.CREO2_SMOKE_MODEL || `agent_plate_${Date.now().toString(36)}`;
    await call('creo_new_part', { name: model });
    const sketch = await call('creo_rectangle_sketch', { model, width_mm: 80, height_mm: 50 });
    const extrude = await call('creo_extrude', { model, sketch_id: sketch.feature_id, depth_mm: 12 });
    const hole = await call('creo_through_hole', {
      model, extrude_id: extrude.feature_id, x_mm: 11, y_mm: -6, diameter_mm: 8,
    });
    const initial = await call('creo_inspect', { model });
    const sketchInfo = initial.features.find(feature => feature.id === sketch.feature_id);
    const extrudeInfo = initial.features.find(feature => feature.id === extrude.feature_id);
    const holeInfo = initial.features.find(feature => feature.id === hole.feature_id);
    assert.equal(sketchInfo?.type, 'sketch');
    assert.equal(extrudeInfo?.type, 'extrusion');
    assert.equal(holeInfo?.type, 'hole');
    const width = sketchInfo.dimensions.find(dimension => dimension.symbol.startsWith('RECT_WIDTH_'));
    const height = sketchInfo.dimensions.find(dimension => dimension.symbol.startsWith('RECT_HEIGHT_'));
    const depth = extrudeInfo.dimensions.find(dimension => dimension.symbol.startsWith('EXTRUDE_DEPTH_'));
    const diameter = holeInfo.dimensions.find(dimension => dimension.length_mm && Math.abs(dimension.value - 8) < 1e-6);
    const locationX = holeInfo.dimensions.find(dimension => dimension.length_mm && Math.abs(dimension.value - 51) < 1e-6);
    assert.ok(width, JSON.stringify(sketchInfo));
    assert.ok(height, JSON.stringify(sketchInfo));
    assert.ok(depth, JSON.stringify(extrudeInfo));
    assert.ok(diameter && locationX, JSON.stringify(holeInfo));
    const baseline = { width: 80, depth: 12, diameter: 8, leftEdgeX: -40, holeOffsetX: 51 };
    const enlarged = { width: 90, depth: 14, diameter: 10, leftEdgeX: -50, holeOffsetX: 56 };
    function verifyPlate(inspection, expected) {
      for (const [originalFeature, originalDimension, value] of [
        [sketchInfo, width, expected.width],
        [sketchInfo, height, 50],
        [extrudeInfo, depth, expected.depth],
        [holeInfo, diameter, expected.diameter],
        [holeInfo, locationX, expected.holeOffsetX],
      ]) {
        const feature = inspection.features.find(item => item.id === originalFeature.id);
        assert.equal(feature?.type, originalFeature.type, `Feature ${originalFeature.id}`);
        const dimension = feature.dimensions.find(item => item.id === originalDimension.id);
        assert.ok(dimension, `Feature ${feature.id}, dimension ${originalDimension.id}`);
        assert.equal(dimension.driving, true, JSON.stringify(dimension));
        assert.equal(dimension.length_mm, true, JSON.stringify(dimension));
        near(dimension.value, value);
      }
      const plateArea = expected.width * 50;
      const holeArea = Math.PI * (expected.diameter / 2) ** 2;
      const solidArea = plateArea - holeArea;
      const plateCenterX = expected.leftEdgeX + expected.width / 2;
      const holeCenterX = expected.leftEdgeX + expected.holeOffsetX;
      near(inspection.volume_mm3, solidArea * expected.depth);
      near(inspection.centroid_mm[0], (plateArea * plateCenterX - holeArea * holeCenterX) / solidArea);
      near(inspection.centroid_mm[1], holeArea * 6 / solidArea);
      near(inspection.centroid_mm[2], expected.depth / 2);
    }
    async function saveAndVerifyReload(expected) {
      const saved = await call('creo_save_part', { model });
      assert.match(saved.path, /\.prt(?:\.\d+)?$/i);
      await call('creo_open_part', { name: model, reload: true });
      const inspection = await call('creo_inspect', { model });
      verifyPlate(inspection, expected);
      return { saved, inspection };
    }
    const edit = (feature, dimension, value_mm) => call('creo_set_dimension', {
      model, feature_id: feature.id, dimension_id: dimension.id, value_mm,
    });
    verifyPlate(initial, baseline);
    await edit(sketchInfo, width, enlarged.width);
    await edit(extrudeInfo, depth, enlarged.depth);
    await edit(holeInfo, diameter, enlarged.diameter);
    await edit(holeInfo, locationX, enlarged.holeOffsetX);
    verifyPlate(await call('creo_inspect', { model }), enlarged);
    await saveAndVerifyReload(enlarged);

    await edit(sketchInfo, width, baseline.width);
    const unsavedReload = await rpc('tools/call', { name: 'creo_open_part', arguments: { name: model, reload: true } });
    assert.equal(unsavedReload.isError, true);
    assert.equal(JSON.parse(unsavedReload.content[0].text).code, -10004);
    await edit(extrudeInfo, depth, baseline.depth);
    await edit(holeInfo, diameter, baseline.diameter);
    await edit(holeInfo, locationX, baseline.holeOffsetX);
    const { saved, inspection } = await saveAndVerifyReload(baseline);
    console.log(JSON.stringify({ model, saved: saved.path, features: [sketch, extrude, hole],
      volume: inspection.volume_mm3, centroid: inspection.centroid_mm }));
  }
} finally {
  lines.close();
  server.stdin.end();
}
