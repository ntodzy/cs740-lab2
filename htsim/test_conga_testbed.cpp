#include "eventlist.h"
#include "logfile.h"
#include "aprx-fairqueue.h"
#include "fairqueue.h"
#include "priorityqueue.h"
#include "stoc-fairqueue.h"
#include "flow-generator.h"
#include "pipe.h"
#include "test.h"
#include "load-balancers/ecmp.h"
#include "stats/experiment-stats.h"
#include "congestion/dre-queue-wrapper.h"

#include <memory>

namespace conga {

const uint32_t N_CORE = 12;
const uint32_t N_LEAF = 24;
const uint32_t N_SERVER = 32; // Per leaf.

// Arrays below retain full-topology capacity; only active ports are created.
struct TopologyConfig {
    uint32_t cores;
    uint32_t leaves;
    uint32_t serversPerLeaf;

    uint32_t nodes() const { return leaves * serversPerLeaf; }
};

const uint64_t LEAF_BUFFER = 512000;
const uint64_t CORE_BUFFER = 1024000;
const uint64_t ENDH_BUFFER = 8192000;

const uint64_t LEAF_SPEED = 10000000000ULL; // 10 Gbps.
const uint64_t CORE_SPEED = 40000000000ULL; // 40 Gbps.
const double LINK_DELAY = 0.1;               // Microseconds per direction.

// Every physical link is modeled in both directions. A Queue models the
// transmitting port and a Pipe models propagation delay.
Queue *qServerLeaf[N_LEAF][N_SERVER] = {};
Pipe  *pServerLeaf[N_LEAF][N_SERVER] = {};
Queue *qLeafServer[N_LEAF][N_SERVER] = {};
Pipe  *pLeafServer[N_LEAF][N_SERVER] = {};

Queue *qLeafCore[N_LEAF][N_CORE] = {};
Pipe  *pLeafCore[N_LEAF][N_CORE] = {};
Queue *qCoreLeaf[N_CORE][N_LEAF] = {};
Pipe  *pCoreLeaf[N_CORE][N_LEAF] = {};
DreQueueWrapper *wLeafCore[N_LEAF][N_CORE] = {};
DreQueueWrapper *wCoreLeaf[N_CORE][N_LEAF] = {};

std::shared_ptr<ExperimentStats> runStats;
FlowGenerator *runGenerator = NULL;

void createQueue(const std::string &queueType,
                 Queue *&queue,
                 linkspeed_bps speed,
                 mem_b buffer);
void nameObject(Logged &object, const std::string &name, Logfile &logfile,
                linkspeed_bps speed = LEAF_SPEED, int core = -1,
                DreQueueWrapper *dre = nullptr);
void generateRandomRoute(LoadBalancer &loadBalancer,
                         const TopologyConfig &topology,
                         ExperimentStats &stats,
                         route_t *&fwd,
                         route_t *&rev,
                         uint32_t &src,
                         uint32_t &dst);

} // namespace conga

using namespace std;
using namespace conga;

void
conga_testbed(const ArgList &args, Logfile &logfile)
{
    uint32_t duration = 5;
    uint32_t avgFlowSize = 100000;
    double utilization = 0.1;
    double linkDelay = LINK_DELAY;
    string queueType = "droptail";
    string endHost = "dctcp";
    string flowDist = "uniform";
    string lb = "ecmp";
    uint32_t small = 0;
    uint32_t trafficSeed = 1729;
    double dreTauUs = 160;
    uint32_t dreLevels = 8;

    parseInt(args, "duration", duration);
    parseInt(args, "flowsize", avgFlowSize);
    parseDouble(args, "utilization", utilization);
    parseDouble(args, "linkdelay", linkDelay);
    parseString(args, "queue", queueType);
    parseString(args, "endhost", endHost);
    parseString(args, "flowdist", flowDist);
    parseString(args, "lb", lb);
    parseInt(args, "small", small);
    parseInt(args, "rngseed", trafficSeed);
    // Independent engine even when the two seeds have the same numeric value.
    uint32_t routingSeed = trafficSeed;
    parseInt(args, "routingseed", routingSeed);
    parseDouble(args, "dre-tau", dreTauUs);
    parseInt(args, "dre-levels", dreLevels);

    string dreTauText = "160";
    parseString(args, "dre-tau", dreTauText);
    if (dreTauText.find_first_not_of("0123456789.eE+-") != string::npos ||
        dreTauUs <= 0 || dreTauUs > 1000000 ||
        timeFromUs(dreTauUs) == 0 || dreLevels < 2) {
        cerr << "dre-tau must be representable in (0, 1000000] us; dre-levels must be >= 2\n";
        exit(1);
    }

    if (small > 1) {
        cerr << "small must be 0 or 1\n";
        exit(1);
    }
    const TopologyConfig topology = small
        ? TopologyConfig{2, 2, 2}
        : TopologyConfig{N_CORE, N_LEAF, N_SERVER};

    if (duration == 0 || utilization <= 0 || linkDelay < 0) {
        cerr << "duration and utilization must be positive; linkdelay cannot be negative\n";
        exit(1);
    }

    shared_ptr<LoadBalancer> loadBalancer;
    if (lb == "ecmp") {
        loadBalancer = make_shared<EcmpLoadBalancer>(topology.cores, routingSeed);
    } else if (lb == "conga") {
        cerr << "Load balancer 'conga' is not implemented yet\n";
        exit(1);
    } else {
        cerr << "Unknown load balancer: " << lb << "; expected ecmp or conga\n";
        exit(1);
    }

    string statsPrefix = "data/htsim-log";
    parseString(args, "logfile", statsPrefix);
    runStats = make_shared<ExperimentStats>(statsPrefix);

    // Fully connect the active leaves and cores.
    for (uint32_t leaf = 0; leaf < topology.leaves; leaf++) {
        for (uint32_t core = 0; core < topology.cores; core++) {
            createQueue(queueType, qLeafCore[leaf][core], CORE_SPEED, LEAF_BUFFER);
            wLeafCore[leaf][core] = new DreQueueWrapper(*qLeafCore[leaf][core],
                CORE_SPEED, timeFromUs(dreTauUs), dreLevels, core);
            nameObject(*qLeafCore[leaf][core],
                       "q-leaf-core-" + to_string(leaf) + "-" + to_string(core),
                       logfile, CORE_SPEED, core, wLeafCore[leaf][core]);
            pLeafCore[leaf][core] = new Pipe(timeFromUs(linkDelay));
            nameObject(*pLeafCore[leaf][core],
                       "p-leaf-core-" + to_string(leaf) + "-" + to_string(core),
                       logfile);

            createQueue(queueType, qCoreLeaf[core][leaf], CORE_SPEED, CORE_BUFFER);
            wCoreLeaf[core][leaf] = new DreQueueWrapper(*qCoreLeaf[core][leaf],
                CORE_SPEED, timeFromUs(dreTauUs), dreLevels);
            nameObject(*qCoreLeaf[core][leaf],
                       "q-core-leaf-" + to_string(core) + "-" + to_string(leaf),
                       logfile, CORE_SPEED, core, wCoreLeaf[core][leaf]);
            pCoreLeaf[core][leaf] = new Pipe(timeFromUs(linkDelay));
            nameObject(*pCoreLeaf[core][leaf],
                       "p-core-leaf-" + to_string(core) + "-" + to_string(leaf),
                       logfile);
        }
    }

    // Attach each active server to exactly one leaf with a 10 Gbps link.
    for (uint32_t leaf = 0; leaf < topology.leaves; leaf++) {
        for (uint32_t server = 0; server < topology.serversPerLeaf; server++) {
            createQueue("fq", qServerLeaf[leaf][server], LEAF_SPEED, ENDH_BUFFER);
            nameObject(*qServerLeaf[leaf][server],
                       "q-server-leaf-" + to_string(leaf) + "-" + to_string(server),
                       logfile);
            pServerLeaf[leaf][server] = new Pipe(timeFromUs(linkDelay));
            nameObject(*pServerLeaf[leaf][server],
                       "p-server-leaf-" + to_string(leaf) + "-" + to_string(server),
                       logfile);

            createQueue(queueType, qLeafServer[leaf][server], LEAF_SPEED, LEAF_BUFFER);
            nameObject(*qLeafServer[leaf][server],
                       "q-leaf-server-" + to_string(leaf) + "-" + to_string(server),
                       logfile);
            pLeafServer[leaf][server] = new Pipe(timeFromUs(linkDelay));
            nameObject(*pLeafServer[leaf][server],
                       "p-leaf-server-" + to_string(leaf) + "-" + to_string(server),
                       logfile);
        }
    }

    DataSource::EndHost endpoint = DataSource::TCP;
    if (endHost == "dctcp") {
        endpoint = DataSource::DCTCP;
    } else if (endHost == "timely") {
        endpoint = DataSource::TIMELY;
    } else if (endHost == "pp") {
        endpoint = DataSource::PKTPAIR;
    } else if (endHost != "tcp") {
        cerr << "Unknown endhost type: " << endHost << endl;
        exit(1);
    }

    Workloads::FlowDist distribution = Workloads::UNIFORM;
    if (flowDist == "pareto") {
        distribution = Workloads::PARETO;
    } else if (flowDist == "enterprise") {
        distribution = Workloads::ENTERPRISE;
    } else if (flowDist == "datamining") {
        distribution = Workloads::DATAMINING;
    } else if (flowDist != "uniform") {
        cerr << "Unknown flow distribution: " << flowDist << endl;
        exit(1);
    }

    // Offered load is relative to the aggregate 10 Gbps server-facing
    // capacity. The shared server-to-leaf queues enforce each host's link rate.
    linkspeed_bps flowRate = llround(utilization * LEAF_SPEED * topology.nodes());
    // Capture ownership by value: the policy must survive this setup function.
    auto stats = runStats;
    route_gen_t fnGenerateRoute = [loadBalancer, topology, stats](route_t *&fwd, route_t *&rev,
                                               uint32_t &src, uint32_t &dst) {
        conga::generateRandomRoute(*loadBalancer, topology, *stats, fwd, rev, src, dst);
    };
    FlowGenerator *flowGenerator = new FlowGenerator(endpoint,
                                                      fnGenerateRoute,
                                                      flowRate,
                                                      avgFlowSize,
                                                      distribution);
    runGenerator = flowGenerator;
    flowGenerator->setTimeLimits(timeFromUs(1), timeFromSec(duration) - 1);
    EventList::Get().setEndtime(timeFromSec(duration));

    cerr << "CONGA topology: " << topology.cores << " cores, " << topology.leaves
         << " leaves, " << topology.nodes() << " servers" << endl;
    cerr << "Traffic seed: " << trafficSeed << ", routing seed: " << routingSeed << endl;
}

void
conga::generateRandomRoute(LoadBalancer &loadBalancer,
                           const TopologyConfig &topology,
                           ExperimentStats &stats,
                           route_t *&fwd,
                           route_t *&rev,
                           uint32_t &src,
                           uint32_t &dst)
{
    src = rand() % topology.nodes();
    do {
        dst = rand() % topology.nodes();
    } while (dst == src);

    uint32_t srcLeaf = src / topology.serversPerLeaf;
    uint32_t dstLeaf = dst / topology.serversPerLeaf;
    uint32_t srcServer = src % topology.serversPerLeaf;
    uint32_t dstServer = dst % topology.serversPerLeaf;

    fwd = new route_t();
    rev = new route_t();

    fwd->push_back(qServerLeaf[srcLeaf][srcServer]);
    fwd->push_back(pServerLeaf[srcLeaf][srcServer]);
    rev->push_back(qServerLeaf[dstLeaf][dstServer]);
    rev->push_back(pServerLeaf[dstLeaf][dstServer]);

    // LOAD BALANCER: choose the source leaf's uplink (identified by core).
    // Delegate policy to the selected implementation; routes stay shared.
    // Same-leaf traffic needs no fabric uplink selection.
    int selectedCore = -1; // Same-leaf traffic has no fabric hop.
    if (srcLeaf != dstLeaf) {
        uint32_t core = loadBalancer.chooseUplink(src, dst, stats.nextFlowId());
        assert(core < topology.cores);
        selectedCore = core;

        // Install the selected path for all data packets and reverse ACKs
        // in this flow (the lab treats a whole flow as one flowlet).
        fwd->push_back(wLeafCore[srcLeaf][core]);
        fwd->push_back(pLeafCore[srcLeaf][core]);
        fwd->push_back(wCoreLeaf[core][dstLeaf]);
        fwd->push_back(pCoreLeaf[core][dstLeaf]);

        rev->push_back(wLeafCore[dstLeaf][core]);
        rev->push_back(pLeafCore[dstLeaf][core]);
        rev->push_back(wCoreLeaf[core][srcLeaf]);
        rev->push_back(pCoreLeaf[core][srcLeaf]);
    }

    fwd->push_back(qLeafServer[dstLeaf][dstServer]);
    fwd->push_back(pLeafServer[dstLeaf][dstServer]);
    rev->push_back(qLeafServer[srcLeaf][srcServer]);
    rev->push_back(pLeafServer[srcLeaf][srcServer]);
    stats.recordRoute(src, dst, selectedCore);
}

void
conga::createQueue(const string &queueType,
                   Queue *&queue,
                   linkspeed_bps speed,
                   mem_b buffer)
{
    // Aggregate counters only; avoid periodic sampling of 2,112 queues.
    if (queueType == "fq") {
        queue = new FairQueue(speed, buffer, runStats.get());
    } else if (queueType == "afq") {
        queue = new AprxFairQueue(speed, buffer, runStats.get());
    } else if (queueType == "pq") {
        queue = new PriorityQueue(speed, buffer, runStats.get());
    } else if (queueType == "sfq") {
        queue = new StocFairQueue(speed, buffer, runStats.get());
    } else if (queueType == "droptail") {
        queue = new Queue(speed, buffer, runStats.get());
    } else {
        cerr << "Unknown queue type: " << queueType << endl;
        exit(1);
    }
}

void
conga::nameObject(Logged &object, const string &name, Logfile &logfile,
                   linkspeed_bps speed, int core, DreQueueWrapper *dre)
{
    object.setName(name);
    logfile.writeName(object);
    Queue *queue = dynamic_cast<Queue*>(&object);
    if (queue != NULL) {
        runStats->registerLink(*queue, speed, core, dre);
    }
}

// Flush after the event loop, including all events at the simulation cutoff.
void finish_conga_testbed()
{
    conga::runStats->write(*conga::runGenerator, EventList::Get().now());
}
