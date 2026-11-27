import { randomBytes } from 'node:crypto';
import { readFile, readdir, rename, unlink, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { setTimeout as delay } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';

const launchedFrom = dirname(fileURLToPath(import.meta.url));
const options = process.argv.slice(2);
if (options.length && (options.length !== 2 || options[0] !== '--session' || !options[1])) {
  throw new Error('Usage: node server.mjs [--session <directory>]');
}
const session = resolve(options[1] || process.env.CREO2_BRIDGE_SESSION || join(launchedFrom, 'session'));
const queue = join(session, 'queue');
const models = join(session, 'models');
const mmLimit = 100_000;
const idLimit = 2_147_483_647;
const modelPattern = '^[A-Za-z][A-Za-z0-9_]{0,29}(?![\\s\\S])';
const reservedName = /^(con|prn|aux|nul|com[0-9]|lpt[0-9]|mmns_part_solid)$/i;
const nameField = { type: 'string', pattern: modelPattern, description: 'Creo model name without an extension.' };
const sizeField = { type: 'number', exclusiveMinimum: 0, maximum: mmLimit };
const offsetField = { type: 'number', minimum: -mmLimit, maximum: mmLimit };
const featureField = { type: 'integer', minimum: 1, maximum: idLimit };
const dimensionField = { type: 'integer', minimum: 0, maximum: idLimit };
const protocols = ['2025-11-25', '2025-06-18', '2025-03-26', '2024-11-05'];

function tool(name, command, description, properties = {}, required = Object.keys(properties)) {
  return { command, definition: { name, description, inputSchema: {
    type: 'object', properties, required, additionalProperties: false,
  } } };
}

const tools = [
  tool('creo_status', 'status', 'Check the connection to the local Creo 2.0 M250 session.'),
  tool('creo_new_part', 'new', 'Create a millimeter Creo part in the bridge session.', { name: nameField }),
  tool('creo_rectangle_sketch', 'sketch', 'Create an editable rectangular sketch centered at the origin in the XY plane.',
    { model: nameField, width_mm: sizeField, height_mm: sizeField }),
  tool('creo_extrude', 'extrude', 'Extrude an existing sketch in the positive Z direction as a linked native feature.',
    { model: nameField, sketch_id: featureField, depth_mm: sizeField }),
  tool('creo_through_hole', 'hole', 'Create an editable through-all hole on the extrusion top face at model XY coordinates.',
    { model: nameField, extrude_id: featureField, x_mm: offsetField, y_mm: offsetField, diameter_mm: sizeField }),
  tool('creo_inspect', 'inspect', 'Read the native part features, dimensions and mass properties.', { model: nameField }),
  tool('creo_set_dimension', 'dimension', 'Set a driving length or diameter in millimeters and regenerate the native part.',
    { model: nameField, feature_id: featureField, dimension_id: dimensionField, value_mm: sizeField }),
  tool('creo_save_part', 'save', 'Save the part in the bridge models directory and return the saved file path.', { model: nameField }),
  tool('creo_open_part', 'open', 'Open a saved bridge part. With reload enabled, reopen the saved version from disk.',
    { name: nameField, reload: { type: 'boolean', default: false } }, ['name']),
];

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function argumentsFor(definition, args) {
  if (!isObject(args)) throw new Error('Tool arguments must be an object.');
  const { properties, required } = definition.inputSchema;
  for (const key of Object.keys(args)) {
    if (!Object.hasOwn(properties, key)) throw new Error(`Unexpected argument: ${key}.`);
  }
  return Object.entries(properties).map(([key, schema]) => {
    if (!Object.hasOwn(args, key)) {
      if (required.includes(key)) throw new Error(`Missing argument: ${key}.`);
      return schema.default ? '1' : '0';
    }
    const value = args[key];
    if (schema.type === 'string') {
      if (typeof value !== 'string' || !new RegExp(schema.pattern).test(value) || reservedName.test(value)) {
        throw new Error(`${key} must be a non-reserved Creo name of 1 to 30 ASCII letters, digits or underscores, starting with a letter.`);
      }
      return value.toLowerCase();
    }
    if (schema.type === 'boolean') {
      if (typeof value !== 'boolean') throw new Error(`${key} must be a boolean.`);
      return value ? '1' : '0';
    }
    if (!Number.isFinite(value) || (schema.type === 'integer' && !Number.isInteger(value)) ||
        value < schema.minimum || value <= schema.exclusiveMinimum || value > schema.maximum) {
      const minimum = schema.minimum ?? `greater than ${schema.exclusiveMinimum}`;
      throw new Error(`${key} must be a ${schema.type} from ${minimum} to ${schema.maximum}.`);
    }
    return String(value);
  });
}

async function readyRuntime() {
  let ready;
  try {
    ready = JSON.parse(await readFile(join(session, 'ready.json'), 'utf8'));
  } catch (error) {
    if (error.code === 'ENOENT') throw new Error('The Creo bridge is not running in this session.');
    if (error instanceof SyntaxError) throw new Error('The Creo bridge ready file is not valid JSON.');
    throw error;
  }
  if (!isObject(ready) || ready.protocol !== 2 || !Number.isInteger(ready.pid) || ready.pid <= 0 || ready.pid > idLimit ||
      typeof ready.epoch !== 'string' || ready.epoch.length !== 32 || /[^a-f0-9]/.test(ready.epoch)) {
    throw new Error('Incompatible Creo bridge session. Start Creo using the current MCP launcher.');
  }
  try {
    process.kill(ready.pid, 0);
  } catch (error) {
    if (error.code !== 'EPERM') throw new Error(`The Creo bridge PID ${ready.pid} is unavailable.`);
  }
  return ready;
}

async function readResponse(filePath) {
  let responseText;
  try {
    responseText = await readFile(filePath, 'utf8');
  } catch (error) {
    if (error.code === 'ENOENT') return undefined;
    throw error;
  }
  await unlink(filePath).catch(() => {});
  const response = JSON.parse(responseText);
  if (!isObject(response) || typeof response.ok !== 'boolean') {
    throw new Error('The Creo bridge returned a malformed response.');
  }
  return response;
}

async function runNative(runtime, command, args, timeoutMs) {
  const requestId = randomBytes(16).toString('hex');
  const request = join(queue, `${requestId}.req`);
  const temporary = join(queue, `${requestId}.tmp`);
  const response = join(queue, `${requestId}.res`);
  const deadline = Date.now() + timeoutMs;
  await writeFile(temporary, `${[runtime.epoch, deadline, command, ...args].join('\t')}\n`, { encoding: 'ascii', flag: 'wx' });
  try {
    await rename(temporary, request);
  } catch (error) {
    await unlink(temporary).catch(() => {});
    throw error;
  }

  while (Date.now() < deadline) {
    const result = await readResponse(response);
    if (result) return result;
    await delay(75);
  }
  const removed = await unlink(request).then(() => true, () => false);
  const finalResult = await readResponse(response);
  if (finalResult) return finalResult;
  if (removed) throw new Error('The Creo bridge timed out before accepting the request.');
  throw new Error('The Creo bridge timed out after the request may have been accepted. Inspect the model before retrying.');
}

async function savedPartPath(model) {
  const pattern = new RegExp(`^${model}\\.prt(?:\\.(\\d+))?$`, 'i');
  let fileName;
  let newestVersion = -2n;
  for (const entry of await readdir(models, { withFileTypes: true })) {
    if (!entry.isFile()) continue;
    const match = pattern.exec(entry.name);
    if (!match) continue;
    const version = match[1] ? BigInt(match[1]) : -1n;
    if (version > newestVersion) {
      fileName = entry.name;
      newestVersion = version;
    }
  }
  if (!fileName) throw new Error(`Creo reported saving ${model}, but the saved part could not be found in the session models directory.`);
  return join(models, fileName);
}

function toolResult(data, isError = false) {
  return { content: [{ type: 'text', text: JSON.stringify(data) }], ...(isError ? { isError: true } : {}) };
}

async function callTool(params) {
  try {
    if (!isObject(params) || typeof params.name !== 'string') throw new Error('A tool name is required.');
    const entry = tools.find(candidate => candidate.definition.name === params.name);
    if (!entry) throw new Error(`Unknown tool: ${params.name}.`);
    const args = argumentsFor(entry.definition, Object.hasOwn(params, 'arguments') ? params.arguments : {});
    if (entry.command === 'status') {
      try {
        const ready = await readyRuntime();
        const status = await runNative(ready, 'status', [], 3_000);
        if (!status.ok) return toolResult({ ...status, connected: false });
        if (status.pid !== ready.pid || status.protocol !== ready.protocol) {
          throw new Error('The Creo bridge restarted while checking the connection.');
        }
        return toolResult({ ...status, connected: true });
      } catch (error) {
        return toolResult({ connected: false, message: error.message });
      }
    }
    const ready = await readyRuntime();
    const response = await runNative(ready, entry.command, args, 45_000);
    if (response.ok && entry.command === 'save') response.path = await savedPartPath(args[0]);
    return toolResult(response, !response.ok);
  } catch (error) {
    return toolResult({ ok: false, message: error.message }, true);
  }
}

function rpcError(id, code, message) {
  return { jsonrpc: '2.0', id, error: { code, message } };
}

async function respond(message) {
  if (!isObject(message) || message.jsonrpc !== '2.0' || typeof message.method !== 'string') {
    return rpcError(null, -32600, 'Invalid Request');
  }
  if (!Object.hasOwn(message, 'id')) return undefined;
  const { id, method, params } = message;
  if (typeof id !== 'string' && !Number.isInteger(id)) return rpcError(null, -32600, 'Invalid Request ID');
  let result;
  switch (method) {
    case 'initialize':
      if (!isObject(params) || typeof params.protocolVersion !== 'string') {
        return rpcError(id, -32602, 'A requested protocol version is required.');
      }
      result = {
        protocolVersion: protocols.includes(params.protocolVersion) ? params.protocolVersion : protocols[0],
        capabilities: { tools: {} },
        serverInfo: { name: 'creo2-m250', version: '1.0.0' },
      };
      break;
    case 'ping':
      result = {};
      break;
    case 'tools/list':
      result = { tools: tools.map(entry => entry.definition) };
      break;
    case 'tools/call':
      result = await callTool(params);
      break;
    default:
      return rpcError(id, -32601, 'Method not found');
  }
  return { jsonrpc: '2.0', id, result };
}

const input = createInterface({ input: process.stdin, crlfDelay: Infinity, terminal: false });
for await (const line of input) {
  if (!line.trim()) continue;
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    process.stdout.write(`${JSON.stringify(rpcError(null, -32700, 'Parse error'))}\n`);
    continue;
  }
  const response = await respond(message);
  if (response) process.stdout.write(`${JSON.stringify(response)}\n`);
}
