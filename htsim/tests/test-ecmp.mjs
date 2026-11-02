// Integration checks for the final topology; no third-party dependencies.
import assert from 'node:assert/strict';
import {mkdtempSync, readFileSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';

const htsim = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const artifacts = mkdtempSync(join(tmpdir(), 'htsim-ecmp-'));
console.log(`Test artifacts: ${artifacts}`);

function csv(path) {
    const [header, ...rows] = readFileSync(path, 'utf8').trim().split('\n');
    const keys = header.split(',');
    return rows.map(row => Object.fromEntries(row.split(',').map((v, i) => [keys[i], v])));
}

function run(name, options = {}) {
    const prefix = join(artifacts, name);
    const flags = {expt: 2, lb: 'ecmp', small: 0, queue: 'droptail', duration: 1,
        utilization: options.small ? 0.001 : 0.00002, flowsize: 10000,
        rngseed: 12314, routingseed: 42, ...options, logfile: prefix};
    const result = spawnSync(join(htsim, 'htsim'),
        Object.entries(flags).map(([k, v]) => `--${k}=${v}`),
        {encoding: 'utf8', timeout: 60000, maxBuffer: 16 * 1024 * 1024});
    assert.equal(result.status, 0, result.stderr || String(result.error));
    writeFileSync(`${prefix}.stdout`, result.stdout);
    return {output: result.stdout, records: Object.fromEntries(
        ['links', 'routes', 'summary'].map(k => [k, csv(`${prefix}.${k}.csv`)]))};
}

function check({records, output}, cores, leaves, servers) {
    const summary = records.summary[0];
    assert.equal(+summary.generated, records.routes.length);
    assert.equal(+summary.completed, (output.match(/\bFlow src/g) || []).length);
    assert.ok(+summary.completed > 0);
    assert.equal(+summary.completed + +summary.incomplete, +summary.generated);
    assert.equal(+summary.route_violations, 0);
    assert.equal(records.links.length, 2 * leaves * cores + 2 * servers);
    for (const route of records.routes) {
        const {source: src, destination: dst, core} = route;
        assert.notEqual(+src, +dst);
        assert.ok(+src >= 0 && +src < servers && +dst >= 0 && +dst < servers);
        const sameLeaf = Math.floor(src / (servers / leaves)) === Math.floor(dst / (servers / leaves));
        assert.equal(+core === -1, sameLeaf);
        if (!sameLeaf) assert.ok(+core >= 0 && +core < cores);
    }
    for (const link of records.links) {
        assert.ok(+link.departed_bytes <= +link.enqueued_bytes, link.link);
        assert.ok(+link.utilization >= 0 && +link.utilization <= 1.001, link.link);
        const fabric = link.link.startsWith('q-leaf-core-') || link.link.startsWith('q-core-leaf-');
        if (fabric) {
            assert.notEqual(link.dre_metric, '', link.link);
            assert.ok(+link.dre_metric >= 0 && +link.dre_metric < 8, link.link);
            assert.ok(+link.dre_normalized_load >= 0, link.link);
            assert.ok(+link.dre_offered_bytes >= +link.enqueued_bytes, link.link);
            if (+link.dropped_bytes === 0) {
                assert.equal(+link.dre_offered_bytes, +link.enqueued_bytes, link.link);
            }
        } else {
            assert.equal(link.dre_metric, '', link.link);
            assert.equal(link.dre_offered_bytes, '', link.link);
        }
    }
}

const first = run('full-first');
const repeat = run('full-repeat');
check(first, 12, 24, 768);
assert.deepEqual(first, repeat, 'Identical seeds must reproduce routes, counters, and FCT output');
const selected = new Set(first.records.routes.map(r => +r.core));
for (let core = 0; core < 12; core++) {
    assert.ok(selected.has(core), `Core ${core} was never selected`);
    for (const direction of ['q-leaf-core', 'q-core-leaf']) {
        const carried = first.records.links.filter(link =>
            link.link.startsWith(direction + '-') &&
            +link.link.split('-')[direction === 'q-leaf-core' ? 4 : 3] === core)
            .reduce((sum, link) => sum + +link.departed_bytes, 0);
        assert.ok(carried > 0, `Core ${core} carried no traffic in ${direction}`);
    }
}
console.log(`Full topology: ${first.records.routes.length} flows, all 12 cores used; repeatability passed`);

const cutoff = run('cutoff', {small: 1, linkdelay: 1000});
check(cutoff, 2, 2, 4);
assert.ok(+cutoff.records.summary[0].incomplete > 0, 'Cutoff test must retain unfinished flows');
console.log('Cutoff: incomplete flows are counted separately from completed flows');

for (const queue of ['droptail', 'fq', 'afq', 'pq', 'sfq']) {
    check(run('small-' + queue, {small: 1, queue}), 2, 2, 4);
    console.log(`${queue}: link/completion counters and packet route stability passed`);
}
