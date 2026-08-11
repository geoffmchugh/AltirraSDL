#!/usr/bin/env node

// Regression checks for the WASM deployment gate. These tests operate on the
// generated index.html so they cover CMake substitution and the exact script
// that is shipped, rather than a duplicate implementation.

'use strict';

const fs = require('fs');

const path = process.argv[2];
if (!path) {
  console.error('usage: deployment_policy_test.js <generated-index.html>');
  process.exit(2);
}

const html = fs.readFileSync(path, 'utf8');

function between(start, end) {
  const startIndex = html.indexOf(start);
  const endIndex = html.indexOf(end, startIndex);
  if (startIndex < 0 || endIndex < 0)
    throw new Error(`generated script section not found: ${start}`);
  return html.slice(startIndex, endIndex);
}

// Syntax-check the actual inline application script. An explanatory HTML
// comment also contains the literal word "<script>", so select by the unique
// application marker rather than assuming the first regex match is code.
const inlineScripts = [...html.matchAll(
  /<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/gi)]
  .map((match) => match[1])
  .filter((script) => script.includes('Persistence root'));
if (inlineScripts.length !== 1)
  throw new Error(`expected one application script, found ${inlineScripts.length}`);
new Function(inlineScripts[0]);

const policySource = between(
  '    function applyNetplayDeploymentPolicy()',
  '    function decideFirstRunAction()');

function runPolicy(config) {
  const lib = {
    brokerMode: {},
    autoHost: true,
    paths: ['game'],
    firmwarePaths: ['firmware'],
    vfsPaths: ['vfs'],
  };
  const window = {
    location: {search: '?broker=1&host=1&lib=game'},
    __altirraLib: lib,
  };
  const execute = Function('window', 'HOST_CONFIG', 'jlog',
    `${policySource}; applyNetplayDeploymentPolicy();`);
  execute(window, config, () => {});
  return {lib, rejected: !!window.__altirraBrokerRejected};
}

const standalone = runPolicy({
  deploymentMode: 'standalone',
  enableLobbyBroker: false,
});
if (!standalone.rejected
    || standalone.lib.autoHost
    || standalone.lib.paths.length
    || standalone.lib.firmwarePaths.length
    || standalone.lib.vfsPaths.length) {
  throw new Error('standalone deployment did not reject the broker payload');
}

const lobby = runPolicy({
  deploymentMode: 'lobby',
  enableLobbyBroker: true,
});
if (lobby.rejected || !lobby.lib.autoHost || lobby.lib.paths.length !== 1)
  throw new Error('lobby deployment rejected an enabled broker payload');

const preRunSource = between(
  '      preRun: [function () {',
  '      onRuntimeInitialized: () => {');
if (preRunSource.indexOf('applyNetplayDeploymentPolicy();')
    > preRunSource.indexOf('preRunFetch(Module')) {
  throw new Error('broker policy runs after the deep-link payload fetch');
}

const loadSource = between(
  '    function loadHostConfig()',
  '    let HOST_RUNTIME_CONFIG_APPLIED');
let fetchCount = 0;
const loadTwice = Function(
  'fetch', 'AbortController', 'setTimeout', 'clearTimeout', 'cacheBust',
  'jlog',
  `let GAME_PACKS = [];
   let HOST_CONFIG = null;
   let HOST_CONFIG_PROMISE = null;
   ${loadSource}
   return Promise.all([loadHostConfig(), loadHostConfig()]);`);

const fetchMock = async () => {
  ++fetchCount;
  return {
    ok: true,
    json: async () => ({deploymentMode: 'standalone', gamePacks: []}),
  };
};

(async () => {
  await loadTwice(fetchMock, AbortController, setTimeout, clearTimeout,
    'test', () => {});
  if (fetchCount !== 1)
    throw new Error(`host config fetched ${fetchCount} times`);
  console.log('WASM deployment policy tests passed');
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
