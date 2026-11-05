// Build first with make -C htsim. No third-party dependencies.
import assert from 'node:assert/strict';
import {mkdtempSync, readFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';

const htsim = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const artifacts = mkdtempSync(join(tmpdir(), 'htsim-output-'));
const columns = ['mode', 'workload', 'load', 'seed', 'routing_seed', 'flow_id',
    'source', 'destination', 'size_bytes', 'start_us', 'finish_us', 'fct_us'];

function run(name, options = {}) {
    const prefix = join(artifacts, name);
    const flags = {expt: 2, small: 1, duration: 1, utilization: 0.0001, flowsize: 10000,
        lb: 'conga', endhost: 'dctcp', flowdist: 'uniform', rngseed: 12314, routingseed: 42,
        ...options, logfile: prefix};
    const result = spawnSync(join(htsim, 'htsim'),
        Object.entries(flags).map(([key, value]) => `--${key}=${value}`),
        {encoding: 'utf8', timeout: 60000, maxBuffer: 16 * 1024 * 1024});
    assert.equal(result.status, 0, result.stderr || String(result.error));
    const metadata = JSON.parse(readFileSync(prefix + '.run.json', 'utf8'));
    const [header, ...lines] = readFileSync(prefix + '.flows.csv', 'utf8').trim().split('\n');
    assert.deepEqual(header.split(','), columns);
    const flows = lines.map(line => {
        const values = line.split(',');
        assert.equal(values.length, columns.length);
        return Object.fromEntries(columns.map((key, i) => [key, values[i]]));
    });
    const {config, counts} = metadata;
    assert.equal(metadata.schema_version, 1);
    for (const [key, value] of Object.entries({experiment: flags.expt, mode: flags.lb,
        workload: flags.flowdist, load: flags.utilization, seed: flags.rngseed,
        routing_seed: flags.routingseed, duration_seconds: flags.duration, flowsize_bytes: flags.flowsize,
        endhost: flags.endhost, queue: 'droptail', small: flags.small === 1, verbose: flags.verbose === 1,
        cores: flags.small ? 2 : 12, leaves: flags.small ? 2 : 24,
        servers_per_leaf: flags.small ? 2 : 32, server_link_bps: 10000000000,
        fabric_link_bps: flags.expt === 4 ? 20000000000 : 40000000000,
        dre_interval_us: 20, dre_alpha: 0.125, dre_levels: 8})) {
        assert.equal(config[key], value, `Incorrect effective configuration: ${key}`);
    }
    assert.equal(counts.completed, flows.length);
    assert.equal(counts.generated, counts.completed + counts.incomplete);
    assert.equal(counts.route_violations, 0);
    const routeLines = readFileSync(prefix + '.routes.csv', 'utf8').trim().split('\n').slice(1);
    const routes = new Map(routeLines.map(line => {
        const [id, source, destination] = line.split(',');
        return [id, {source, destination}];
    }));
    assert.equal(routes.size, counts.generated);
    const seen = new Set();
    for (const flow of flows) {
        assert.ok(!seen.has(flow.flow_id), 'Duplicate completion');
        seen.add(flow.flow_id);
        const route = routes.get(flow.flow_id);
        assert.ok(route, 'Completion ID is not a generator routing ID');
        assert.equal(flow.source, route.source);
        assert.equal(flow.destination, route.destination);
        assert.equal(flow.mode, flags.lb);
        assert.equal(flow.workload, flags.flowdist);
        assert.equal(+flow.load, flags.utilization);
        assert.equal(+flow.seed, flags.rngseed);
        assert.equal(+flow.routing_seed, flags.routingseed);
        assert.equal(+flow.size_bytes, flags.flowsize);
        assert.ok(+flow.start_us >= 1);
        assert.ok(+flow.finish_us > +flow.start_us);
        assert.ok(+flow.finish_us <= metadata.end_time_us + 1e-6);
        assert.ok(Math.abs(+flow.finish_us - +flow.start_us - +flow.fct_us) < 1e-6);
    }
    if (!flags.verbose) {
        assert.equal(result.stdout, `generated=${counts.generated} completed=${counts.completed} incomplete=${counts.incomplete}\n`);
        assert.equal(result.stderr, '', 'Default mode streamed diagnostics');
    }
    return {prefix, result, metadata, flows};
}

for (const endhost of ['tcp', 'dctcp', 'timely', 'pp']) {
    const quiet = run(endhost, {endhost});
    const verbose = run(endhost + '-verbose', {endhost, verbose: 1});
    assert.ok(quiet.flows.length > 0, `${endhost} did not complete any flows`);
    for (const suffix of ['flows', 'routes', 'summary', 'links', 'decisions', 'selections']) {
        assert.equal(readFileSync(quiet.prefix + `.${suffix}.csv`, 'utf8'),
            readFileSync(verbose.prefix + `.${suffix}.csv`, 'utf8'),
            `Verbose mode changed ${endhost} ${suffix}`);
    }
    assert.equal((verbose.result.stdout.match(/^Flow src/gm) || []).length, quiet.flows.length);
    assert.match(verbose.result.stdout, /^src0 /m);
    assert.match(verbose.result.stdout, /Live Flows:/);
    console.log(`${endhost}: complete records, generator identity, timing, and verbosity isolation passed`);
}
const truncated = run('truncated', {linkdelay: 1000000});
assert.ok(truncated.metadata.counts.generated > 0);
assert.equal(truncated.flows.length, 0, 'Unfinished flows were emitted as completions');
assert.equal(truncated.metadata.counts.incomplete, truncated.metadata.counts.generated);
for (const lb of ['ecmp', 'conga']) {
    const fractional = run('fractional-' + lb, {lb, duration: 0.01});
    assert.ok(fractional.metadata.counts.generated > 0);
    assert.ok(fractional.flows.length > 0);
    assert.ok(Math.abs(fractional.metadata.end_time_us - 10000) < 1e-5,
        'Fractional duration was truncated or converted incorrectly');
}
for (const duration of ['0', '-1', 'nan', 'inf', '0.1junk', '0.000001', '1e20']) {
    const invalidDuration = spawnSync(join(htsim, 'htsim'), ['--expt=2', '--small=1',
        `--logfile=${join(artifacts, 'invalid-duration')}`, `--duration=${duration}`], {encoding: 'utf8', timeout: 5000});
    assert.equal(invalidDuration.status, 1, `Invalid duration accepted: ${duration}`);
    assert.match(invalidDuration.stderr, /duration/);
}
const invalid = spawnSync(join(htsim, 'htsim'), ['--expt=2', '--verbose=banana'], {encoding: 'utf8'});
assert.equal(invalid.status, 1);
assert.match(invalid.stderr, /verbose must be 0 or 1/);
const usage = spawnSync(join(htsim, 'htsim'), [], {encoding: 'utf8'});
assert.match(usage.stderr, /4\s+conga_testbed_oversubscription/);
for (const lb of ['ecmp', 'conga']) {
    const baseline = run('baseline-full-' + lb, {small: 0, duration: 0.002, lb});
    const oversubscribed = run('oversubscribed-full-' + lb, {expt: 4, small: 0, duration: 0.002, lb});
    const demands = prefix => readFileSync(prefix + '.routes.csv', 'utf8').trim()
        .split('\n').slice(1).map(line => line.split(',').slice(0, 3));
    assert.deepEqual(demands(oversubscribed.prefix), demands(baseline.prefix),
        'Changing fabric capacity changed the generated endpoints');
    const config = oversubscribed.metadata.config;
    assert.equal(config.servers_per_leaf * config.server_link_bps /
        (config.cores * config.fabric_link_bps), 4 / 3);
    for (const [simulation, fabricRate] of [[baseline, 40000000000], [oversubscribed, 20000000000]]) {
        const [header, ...rows] = readFileSync(simulation.prefix + '.links.csv', 'utf8').trim().split('\n');
        const fields = header.split(',');
        let fabricLinks = 0;
        for (const line of rows) {
            const values = line.split(',');
            const name = values[fields.indexOf('link')];
            const fabric = name.startsWith('q-leaf-core-') || name.startsWith('q-core-leaf-');
            assert.equal(+values[fields.indexOf('bitrate_bps')], fabric ? fabricRate : 10000000000);
            if (fabric) ++fabricLinks;
        }
        assert.equal(fabricLinks, 576);
    }
    console.log(`${lb}: experiment 4, full-topology 4:3 capacity, output flushing, and shared demands passed`);
}
console.log(`Cutoff accounting and invalid verbosity passed. Artifacts: ${artifacts}`);
