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
    const records = Object.fromEntries(
        ['links', 'routes', 'summary', 'decisions', 'selections', 'flows']
            .map(k => [k, csv(`${prefix}.${k}.csv`)]));
    assert.ok(records.decisions.every(decision => decision.mode === flags.lb),
        'Decision diagnostics recorded the wrong load-balancer mode');
    return {output: result.stdout, records};
}

function check({records, output}, cores, leaves, servers) {
    const summary = records.summary[0];
    assert.equal(+summary.generated, records.routes.length);
    assert.equal(+summary.completed, records.flows.length);
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
            assert.equal(+link.dre_sent_bytes, +link.departed_bytes, link.link);
        } else {
            assert.equal(link.dre_metric, '', link.link);
            assert.equal(link.dre_sent_bytes, '', link.link);
        }
    }
    assert.equal(records.decisions.length, records.routes.length);
    const expectedCounts = new Map();
    let lastTime = -1;
    for (let i = 0; i < records.decisions.length; ++i) {
        const decision = records.decisions[i];
        const route = records.routes[i];
        assert.equal(+decision.flow_id, +route.flow_id);
        assert.equal(decision.source, route.source);
        assert.equal(decision.destination, route.destination);
        assert.ok(['ecmp', 'conga'].includes(decision.mode));
        assert.ok(+decision.time_us >= lastTime);
        lastTime = +decision.time_us;
        assert.equal(+decision.source_leaf, Math.floor(+route.source / (servers / leaves)));
        assert.equal(+decision.destination_leaf, Math.floor(+route.destination / (servers / leaves)));
        assert.equal(+decision.uses_fabric === 1, +route.core >= 0);
        if (+decision.uses_fabric) {
            // Only this testbed maps candidate IDs and local uplinks onto core numbers.
            assert.equal(+decision.path_id, +route.core);
            assert.equal(+decision.source_uplink, +route.core);
            assert.ok(+decision.local_metric >= 0 && +decision.local_metric < 8);
            assert.ok(+decision.remote_metric >= 0 && +decision.remote_metric < 8);
            assert.ok(['0', '1'].includes(decision.remote_valid));
            if (decision.remote_valid === '0') assert.equal(+decision.remote_metric, 0);
            assert.equal(+decision.score, Math.max(+decision.local_metric, +decision.remote_metric));
        } else {
            for (const key of ['source_uplink', 'local_metric', 'remote_metric', 'remote_valid', 'score']) {
                assert.equal(decision[key], '', `Direct route unexpectedly recorded ${key}`);
            }
        }
        const key = [decision.mode, decision.source_leaf, decision.destination_leaf,
            decision.path_id, decision.source_uplink].join(',');
        const counts = expectedCounts.get(key) || {selected: 0, unknown: 0};
        ++counts.selected;
        if (decision.uses_fabric === '1' && decision.remote_valid === '0') ++counts.unknown;
        expectedCounts.set(key, counts);
    }
    assert.equal(records.selections.length, expectedCounts.size);
    for (const count of records.selections) {
        const key = [count.mode, count.source_leaf, count.destination_leaf,
            count.path_id, count.source_uplink].join(',');
        const expected = expectedCounts.get(key);
        assert.ok(expected, 'Unexpected aggregate selection key: ' + key);
        assert.equal(+count.selected_flows, expected.selected);
        assert.equal(+count.unknown_remote_flows, expected.unknown);
        expectedCounts.delete(key);
    }
    assert.equal(expectedCounts.size, 0);
}

const first = run('full-first');
const repeat = run('full-repeat');
check(first, 12, 24, 768);
assert.deepEqual(first, repeat, 'Identical seeds must reproduce routes, counters, and FCT output');
const conga = run('full-conga', {lb: 'conga'});
const congaRepeat = run('full-conga-repeat', {lb: 'conga'});
check(conga, 12, 24, 768);
assert.deepEqual(conga, congaRepeat, 'Identical seeds must reproduce CONGA routing and results');
assert.equal(conga.records.summary[0].generated, first.records.summary[0].generated,
    'Policy selection must not change generated traffic counts');
assert.notDeepEqual(conga.records.routes, first.records.routes,
    'CONGA selection must no longer be the ECMP hash placeholder');
console.log('CONGA: full-topology reachability, fixed routes, and repeatability passed');
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
    check(run('small-conga-' + queue, {small: 1, queue, lb: 'conga'}), 2, 2, 4);
    console.log(`${queue}: ECMP/CONGA counters and packet route stability passed`);
}

check(run('periodic-options', {small: 1, 'dre-interval': 10, 'dre-alpha': 0.25}), 2, 2, 4);
for (const option of ['--dre-alpha=0', '--dre-alpha=1', '--dre-alpha=nan',
    '--dre-interval=0', '--dre-levels=16', '--dre-tau=160']) {
    const result = spawnSync(join(htsim, 'htsim'), ['--expt=2', '--small=1',
        `--logfile=${join(artifacts, 'invalid-dre')}`, option], {encoding: 'utf8'});
    assert.equal(result.status, 1, `${option} must be rejected: ${result.stderr}`);
}
console.log('Periodic DRE options and invalid-configuration checks passed');
